#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include "voice_client.h"

struct _VoiceClient {
  gint ref;
  gboolean dead;  /* voice_client_free() called: no more callbacks */
  gboolean done;  /* FINAL/ERR seen or closure reported */
  GCancellable *cancel;
  GSocketConnection *conn;
  GDataInputStream *in;
  GString *pending; /* commands issued before the connection was up */
  VoiceClientEventFn on_event;
  VoiceClientClosedFn on_closed;
  gpointer user_data;
};

static VoiceClient *ref(VoiceClient *c) {
  c->ref++;
  return c;
}

static void unref(VoiceClient *c) {
  if (--c->ref) return;
  g_clear_object(&c->in);
  g_clear_object(&c->conn);
  g_object_unref(c->cancel);
  g_string_free(c->pending, TRUE);
  g_free(c);
}

static void report_closed(VoiceClient *c) {
  if (c->dead || c->done) return;
  c->done = TRUE;
  c->on_closed(c->user_data);
}

static void write_line(VoiceClient *c, const char *line) {
  if (!c->conn) {
    g_string_append(c->pending, line);
    return;
  }
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c->conn));
  g_output_stream_write_all(out, line, strlen(line), NULL, c->cancel, NULL);
}

static void read_next(VoiceClient *c);

static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
  VoiceClient *c = data;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
  if (!c->dead && !line) {
    report_closed(c);
  } else if (!c->dead) {
    if (g_utf8_validate(line, -1, NULL)) { /* drop malformed lines, keep reading */
      g_strchomp(line);
      const char *payload;
      VoiceEvType type = voice_proto_parse(line, &payload);
      if (type == VOICE_EV_FINAL || type == VOICE_EV_ERR) c->done = TRUE;
      if (type != VOICE_EV_INVALID) c->on_event(type, payload, c->user_data);
    }
    if (!c->dead && !c->done) read_next(c);
  }
  g_free(line);
  unref(c);
}

static void read_next(VoiceClient *c) {
  g_data_input_stream_read_line_async(c->in, G_PRIORITY_DEFAULT, c->cancel, on_line, ref(c));
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer data) {
  VoiceClient *c = data;
  GSocketConnection *conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, NULL);
  if (c->dead) {
    g_clear_object(&conn);
  } else if (!conn) {
    report_closed(c);
  } else {
    c->conn = conn;
    c->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    g_data_input_stream_set_newline_type(c->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    GString *queued = c->pending;
    c->pending = g_string_new("");
    write_line(c, "START\n");
    write_line(c, queued->str);
    g_string_free(queued, TRUE);
    read_next(c);
  }
  g_object_unref(src);
  unref(c);
}

char *voice_client_default_path(void) {
  return g_build_filename(g_get_user_runtime_dir(), "toolbox-voiced.sock", NULL);
}

VoiceClient *voice_client_start(const char *socket_path, VoiceClientEventFn on_event,
                                VoiceClientClosedFn on_closed, gpointer user_data) {
  VoiceClient *c = g_new0(VoiceClient, 1);
  c->ref = 1;
  c->cancel = g_cancellable_new();
  c->pending = g_string_new("");
  c->on_event = on_event;
  c->on_closed = on_closed;
  c->user_data = user_data;
  GSocketClient *sc = g_socket_client_new();
  GSocketAddress *addr = g_unix_socket_address_new(socket_path);
  g_socket_client_connect_async(sc, G_SOCKET_CONNECTABLE(addr), c->cancel, on_connected, ref(c));
  g_object_unref(addr);
  return c;
}

void voice_client_send(VoiceClient *c, VoiceCmd cmd) {
  if (cmd == VOICE_CMD_STOP) write_line(c, "STOP\n");
  else if (cmd == VOICE_CMD_CANCEL) write_line(c, "CANCEL\n");
}

void voice_client_free(VoiceClient *c) {
  if (!c) return;
  c->dead = TRUE;
  g_cancellable_cancel(c->cancel);
  if (c->conn) g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
  unref(c);
}

/* ---- one-shot settings save ---- */

typedef struct {
  char *line;
  VoiceSaveDoneFn done;
  gpointer user_data;
  GSocketConnection *conn;
  GDataInputStream *in;
  GCancellable *cancel;
  guint timeout_id;
  gboolean timed_out;
} SaveReq;

static void save_finish(SaveReq *r, const char *error) {
  if (r->timeout_id) g_source_remove(r->timeout_id);
  r->done(r->timed_out ? "no answer from the voice daemon in time" : error, r->user_data);
  g_object_unref(r->cancel);
  g_clear_object(&r->in);
  if (r->conn) g_io_stream_close(G_IO_STREAM(r->conn), NULL, NULL);
  g_clear_object(&r->conn);
  g_free(r->line);
  g_free(r);
}

static void on_save_reply(GObject *src, GAsyncResult *res, gpointer data) {
  SaveReq *r = data;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
  if (line) g_strchomp(line);
  if (line && !strcmp(line, "OK")) save_finish(r, NULL);
  else if (line && g_str_has_prefix(line, "ERR ")) save_finish(r, line + 4);
  else save_finish(r, "no answer from the voice daemon");
  g_free(line);
}

static void on_save_connected(GObject *src, GAsyncResult *res, gpointer data) {
  SaveReq *r = data;
  GError *err = NULL;
  r->conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &err);
  g_object_unref(src);
  if (!r->conn) {
    char *msg = g_strdup_printf("voice daemon not reachable: %s", err->message);
    g_error_free(err);
    save_finish(r, msg);
    g_free(msg);
    return;
  }
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(r->conn));
  if (!g_output_stream_write_all(out, r->line, strlen(r->line), NULL, r->cancel, NULL)) {
    save_finish(r, "could not write to the voice daemon");
    return;
  }
  r->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(r->conn)));
  g_data_input_stream_set_newline_type(r->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
  g_data_input_stream_read_line_async(r->in, G_PRIORITY_DEFAULT, r->cancel, on_save_reply, r);
}

static gboolean on_save_timeout(gpointer data) {
  SaveReq *r = data;
  r->timeout_id = 0;
  r->timed_out = TRUE;
  g_cancellable_cancel(r->cancel); /* the pending connect or read finishes with an error */
  return G_SOURCE_REMOVE;
}

void voice_client_save_setting(const char *socket_path, const char *key, const char *value,
                               VoiceSaveDoneFn done, gpointer user_data) {
  SaveReq *r = g_new0(SaveReq, 1);
  r->line = g_strdup_printf("SET %s %s\n", key, value);
  r->done = done;
  r->user_data = user_data;
  r->cancel = g_cancellable_new();
  r->timeout_id = g_timeout_add(VOICE_SAVE_TIMEOUT_MS, on_save_timeout, r);
  GSocketClient *sc = g_socket_client_new();
  GSocketAddress *addr = g_unix_socket_address_new(socket_path);
  g_socket_client_connect_async(sc, G_SOCKET_CONNECTABLE(addr), r->cancel, on_save_connected, r);
  g_object_unref(addr);
}
