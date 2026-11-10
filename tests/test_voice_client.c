/* Unit tests for the voiced socket client (voice_client.c), against an
 * in-process fake daemon. */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../voice_client.h"

typedef struct {
  VoiceEvType type;
  char *payload;
} Ev;

/* What the fake daemon does after it has read START. */
typedef struct {
  const char *lines[4];   /* written in order, NULL-terminated */
  guint delay_before_last_ms; /* sleep before writing the last line */
  gboolean read_second;   /* read one more line and record it */
} Script;

typedef struct {
  /* fake daemon */
  GThreadedSocketService *service;
  char *dir;
  char *path;
  const Script *script;
  GMutex lock;
  char *first_line;       /* guarded by lock */
  char *second_line;      /* guarded by lock */
  /* client side (main thread only) */
  GMainLoop *loop;
  GArray *events;         /* of Ev */
  guint closed;
  guint timeout_id;
  gboolean timed_out;
  gboolean quit_armed;
  VoiceClient *client;
  void (*on_event_hook)(gpointer ctx, VoiceEvType type);
} Ctx;

static gboolean quit_cb(gpointer data) {
  Ctx *c = data;
  g_main_loop_quit(c->loop);
  return G_SOURCE_REMOVE;
}

static gboolean timeout_cb(gpointer data) {
  Ctx *c = data;
  c->timed_out = TRUE;
  c->timeout_id = 0;
  g_main_loop_quit(c->loop);
  return G_SOURCE_REMOVE;
}

static void arm_quit(Ctx *c, guint ms) {
  if (c->quit_armed)
    return;
  c->quit_armed = TRUE;
  g_timeout_add(ms, quit_cb, c);
}

static gboolean quit_from_thread(gpointer data) {
  Ctx *c = data;
  g_main_loop_quit(c->loop);
  return G_SOURCE_REMOVE;
}

/* Runs on a worker thread (blocking IO would stall the main loop that the
 * client under test lives on). */
static gboolean on_run(GThreadedSocketService *svc, GSocketConnection *conn,
                       GObject *source, gpointer data) {
  Ctx *c = data;
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  char *line = g_data_input_stream_read_line(in, NULL, NULL, NULL);
  g_mutex_lock(&c->lock);
  c->first_line = line;
  g_mutex_unlock(&c->lock);
  if (line && strcmp(line, "START") == 0) {
    for (int i = 0; c->script->lines[i]; i++) {
      gboolean last = c->script->lines[i + 1] == NULL;
      if (last && c->script->delay_before_last_ms)
        g_usleep(c->script->delay_before_last_ms * 1000);
      const char *l = c->script->lines[i];
      g_output_stream_write_all(out, l, strlen(l), NULL, NULL, NULL);
    }
    if (c->script->read_second) {
      char *second = g_data_input_stream_read_line(in, NULL, NULL, NULL);
      g_mutex_lock(&c->lock);
      c->second_line = second;
      g_mutex_unlock(&c->lock);
      g_main_context_invoke(NULL, quit_from_thread, c);
    }
  }
  g_object_unref(in);
  return TRUE; /* connection is closed when the handler returns */
}

static void ctx_setup(Ctx *c, const Script *script) {
  memset(c, 0, sizeof *c);
  g_mutex_init(&c->lock);
  c->script = script;
  c->events = g_array_new(FALSE, TRUE, sizeof(Ev));
  c->loop = g_main_loop_new(NULL, FALSE);
  GError *err = NULL;
  c->dir = g_dir_make_tmp("voice-client-test-XXXXXX", &err);
  g_assert_no_error(err);
  c->path = g_build_filename(c->dir, "voiced.sock", NULL);
}

static void ctx_start_daemon(Ctx *c) {
  GError *err = NULL;
  c->service = G_THREADED_SOCKET_SERVICE(g_threaded_socket_service_new(4));
  GSocketAddress *addr = g_unix_socket_address_new(c->path);
  g_socket_listener_add_address(G_SOCKET_LISTENER(c->service), addr, G_SOCKET_TYPE_STREAM,
                                G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err);
  g_assert_no_error(err);
  g_object_unref(addr);
  g_signal_connect(c->service, "run", G_CALLBACK(on_run), c);
  g_socket_service_start(G_SOCKET_SERVICE(c->service));
}

static void ctx_teardown(Ctx *c) {
  if (c->timeout_id)
    g_source_remove(c->timeout_id);
  if (c->service) {
    g_socket_service_stop(G_SOCKET_SERVICE(c->service));
    g_socket_listener_close(G_SOCKET_LISTENER(c->service));
    g_object_unref(c->service);
  }
  g_unlink(c->path);
  g_rmdir(c->dir);
  for (guint i = 0; i < c->events->len; i++)
    g_free(g_array_index(c->events, Ev, i).payload);
  g_array_free(c->events, TRUE);
  g_main_loop_unref(c->loop);
  g_free(c->first_line);
  g_free(c->second_line);
  g_free(c->dir);
  g_free(c->path);
  g_mutex_clear(&c->lock);
}

static void on_event(VoiceEvType type, const char *payload, gpointer data) {
  Ctx *c = data;
  Ev e = {type, g_strdup(payload ? payload : "")};
  g_array_append_val(c->events, e);
  if (c->on_event_hook)
    c->on_event_hook(c, type);
}

static void on_closed(gpointer data) {
  Ctx *c = data;
  c->closed++;
  /* Linger a bit to catch duplicate callbacks. */
  arm_quit(c, 150);
}

static void run_loop(Ctx *c) {
  c->timeout_id = g_timeout_add(2000, timeout_cb, c);
  g_main_loop_run(c->loop);
  g_assert_false(c->timed_out);
}

static Ev *ev_at(Ctx *c, guint i) {
  g_assert_cmpuint(c->events->len, >, i);
  return &g_array_index(c->events, Ev, i);
}

static void final_hook(gpointer ctx, VoiceEvType type) {
  if (type == VOICE_EV_FINAL)
    arm_quit(ctx, 150);
}

static void test_receives_events_in_order(void) {
  static const Script s = {{"REC\n", "PARTIAL a\n", "FINAL ab\n", NULL}, 0, FALSE};
  Ctx c;
  ctx_setup(&c, &s);
  ctx_start_daemon(&c);
  c.on_event_hook = final_hook;
  c.client = voice_client_start(c.path, on_event, on_closed, &c);
  g_assert_nonnull(c.client);
  run_loop(&c);
  g_assert_cmpuint(c.events->len, ==, 3);
  g_assert_cmpint(ev_at(&c, 0)->type, ==, VOICE_EV_REC);
  g_assert_cmpint(ev_at(&c, 1)->type, ==, VOICE_EV_PARTIAL);
  g_assert_cmpstr(ev_at(&c, 1)->payload, ==, "a");
  g_assert_cmpint(ev_at(&c, 2)->type, ==, VOICE_EV_FINAL);
  g_assert_cmpstr(ev_at(&c, 2)->payload, ==, "ab");
  g_assert_cmpuint(c.closed, ==, 0);
  g_mutex_lock(&c.lock);
  g_assert_cmpstr(c.first_line, ==, "START");
  g_mutex_unlock(&c.lock);
  voice_client_free(c.client);
  ctx_teardown(&c);
}

static void test_connect_failure_calls_closed(void) {
  Ctx c;
  ctx_setup(&c, NULL);
  /* No daemon: the socket path does not exist. */
  c.client = voice_client_start(c.path, on_event, on_closed, &c);
  g_assert_nonnull(c.client);
  run_loop(&c);
  g_assert_cmpuint(c.closed, ==, 1);
  g_assert_cmpuint(c.events->len, ==, 0);
  voice_client_free(c.client);
  ctx_teardown(&c);
}

static void test_drop_before_final_calls_closed(void) {
  static const Script s = {{"REC\n", NULL}, 0, FALSE};
  Ctx c;
  ctx_setup(&c, &s);
  ctx_start_daemon(&c);
  c.client = voice_client_start(c.path, on_event, on_closed, &c);
  run_loop(&c);
  g_assert_cmpuint(c.events->len, ==, 1);
  g_assert_cmpint(ev_at(&c, 0)->type, ==, VOICE_EV_REC);
  g_assert_cmpuint(c.closed, ==, 1);
  voice_client_free(c.client);
  ctx_teardown(&c);
}

static void free_in_rec_hook(gpointer ctx, VoiceEvType type) {
  Ctx *c = ctx;
  if (type == VOICE_EV_REC) {
    voice_client_free(c->client);
    c->client = NULL;
    arm_quit(c, 400);
  }
}

static void test_free_suppresses_callbacks(void) {
  static const Script s = {{"REC\n", "FINAL x\n", NULL}, 200, FALSE};
  Ctx c;
  ctx_setup(&c, &s);
  ctx_start_daemon(&c);
  c.on_event_hook = free_in_rec_hook;
  c.client = voice_client_start(c.path, on_event, on_closed, &c);
  run_loop(&c);
  g_assert_null(c.client);
  g_assert_cmpuint(c.events->len, ==, 1);
  g_assert_cmpint(ev_at(&c, 0)->type, ==, VOICE_EV_REC);
  g_assert_cmpuint(c.closed, ==, 0);
  ctx_teardown(&c);
}

static void send_stop_hook(gpointer ctx, VoiceEvType type) {
  Ctx *c = ctx;
  if (type == VOICE_EV_REC)
    voice_client_send(c->client, VOICE_CMD_STOP);
}

static void test_send_stop_reaches_daemon(void) {
  static const Script s = {{"REC\n", NULL}, 0, TRUE};
  Ctx c;
  ctx_setup(&c, &s);
  ctx_start_daemon(&c);
  c.on_event_hook = send_stop_hook;
  c.client = voice_client_start(c.path, on_event, on_closed, &c);
  run_loop(&c); /* the fake quits the loop once it has read the second line */
  g_mutex_lock(&c.lock);
  g_assert_cmpstr(c.second_line, ==, "STOP");
  g_mutex_unlock(&c.lock);
  voice_client_free(c.client);
  ctx_teardown(&c);
}

static void test_default_path(void) {
  char *p = voice_client_default_path();
  char *want = g_build_filename(g_get_user_runtime_dir(), "toolbox-voiced.sock", NULL);
  g_assert_cmpstr(p, ==, want);
  g_free(p);
  g_free(want);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/voice_client/receives_events_in_order", test_receives_events_in_order);
  g_test_add_func("/voice_client/connect_failure_calls_closed", test_connect_failure_calls_closed);
  g_test_add_func("/voice_client/drop_before_final_calls_closed",
                  test_drop_before_final_calls_closed);
  g_test_add_func("/voice_client/free_suppresses_callbacks", test_free_suppresses_callbacks);
  g_test_add_func("/voice_client/send_stop_reaches_daemon", test_send_stop_reaches_daemon);
  g_test_add_func("/voice_client/default_path", test_default_path);
  return g_test_run();
}
