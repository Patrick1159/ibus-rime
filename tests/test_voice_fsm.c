/* Unit tests for the dictation state machine (voice_fsm.c). */
#include <glib.h>
#include <ibus.h>
#include <string.h>
#include "../voice_fsm.h"

#define HOTKEY_CODE 100
#define KEYCODE_A 38
#define KEYCODE_BS 22
#define KEYCODE_TAB 23
#define KEYCODE_ESC 9

static VoiceAction press(VoiceFsm *f, guint keyval, guint keycode, guint mods) {
  return voice_fsm_key(f, keyval, keycode, mods, FALSE);
}

static VoiceAction release(VoiceFsm *f, guint keyval, guint keycode, guint mods) {
  return voice_fsm_key(f, keyval, keycode, mods | IBUS_RELEASE_MASK, FALSE);
}

/* Bare press + release of the hotkey; returns the release action. */
static VoiceAction tap(VoiceFsm *f, gboolean composing) {
  voice_fsm_key(f, IBUS_KEY_Alt_R, HOTKEY_CODE, 0, composing);
  return voice_fsm_key(f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_RELEASE_MASK | IBUS_MOD1_MASK,
                       composing);
}

static void to_recording(VoiceFsm *f) {
  VoiceAction a = tap(f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  g_assert_cmpint(f->state, ==, VOICE_RECORDING);
}

static void to_finalizing(VoiceFsm *f, const char *partial) {
  to_recording(f);
  if (partial)
    voice_fsm_event(f, VOICE_EV_PARTIAL, partial);
  VoiceAction a = tap(f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  g_assert_cmpint(f->state, ==, VOICE_FINALIZING);
}

static void queue_three_keys(VoiceFsm *f) {
  VoiceAction a;
  a = press(f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_true(a.consume);
  a = release(f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_true(a.consume);
  a = press(f, IBUS_KEY_BackSpace, KEYCODE_BS, 0);
  g_assert_true(a.consume);
}

static void test_tap_starts_recording(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  VoiceAction a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, 0, FALSE);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_RELEASE_MASK | IBUS_MOD1_MASK, FALSE);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  g_assert_cmpstr(a.aux, ==, VOICE_AUX_RECORDING);
  voice_fsm_clear(&f);
}

static void test_partial_updates_aux(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_PARTIAL, "你好");
  g_assert_cmpstr(a.aux, ==, VOICE_AUX_PARTIAL_RECORDING "你好");
  g_assert_cmpstr(a.aux, ==, "🎤 你好");
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  voice_fsm_clear(&f);
}

static void test_second_tap_stops(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  voice_fsm_event(&f, VOICE_EV_PARTIAL, "你好");
  VoiceAction a = tap(&f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  g_assert_cmpint(f.state, ==, VOICE_FINALIZING);
  g_assert_cmpstr(a.aux, ==, VOICE_AUX_PARTIAL_FINALIZING "你好");
  g_assert_true(g_str_has_prefix(a.aux, "⏳"));
  voice_fsm_clear(&f);

  /* No partial yet: plain finalizing text. */
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  a = tap(&f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  g_assert_cmpstr(a.aux, ==, VOICE_AUX_FINALIZING);
  voice_fsm_clear(&f);
}

static void test_final_commits_and_idles(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, "你好");
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "你好 world");
  g_assert_cmpstr(a.commit, ==, "你好 world");
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_false(a.replay);
  voice_fsm_clear(&f);
}

static void test_empty_final_commits_nothing(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "");
  g_assert_null(a.commit);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_escape_cancels(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  VoiceAction a = press(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_true(a.consume);
  g_assert_false(a.replay);
  guint n = 99;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 0);
  voice_fsm_clear(&f);
}

static void test_other_keys_swallowed_while_recording(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  VoiceAction a = press(&f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  a = release(&f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  guint n = 99;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 0);
  voice_fsm_clear(&f);
}

static void test_keys_queued_while_finalizing(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, "x");
  queue_three_keys(&f);
  guint n = 0;
  const VoiceKey *q = voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpuint(q[0].keyval, ==, IBUS_KEY_a);
  g_assert_cmpuint(q[0].keycode, ==, KEYCODE_A);
  g_assert_cmpuint(q[0].modifiers, ==, 0);
  g_assert_cmpuint(q[1].keyval, ==, IBUS_KEY_a);
  g_assert_cmpuint(q[1].keycode, ==, KEYCODE_A);
  g_assert_cmpuint(q[1].modifiers, ==, IBUS_RELEASE_MASK);
  g_assert_cmpuint(q[2].keyval, ==, IBUS_KEY_BackSpace);
  g_assert_cmpuint(q[2].keycode, ==, KEYCODE_BS);
  g_assert_cmpuint(q[2].modifiers, ==, 0);
  g_assert_cmpint(f.state, ==, VOICE_FINALIZING);
  voice_fsm_clear(&f);
}

static void test_final_replays_queue(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, NULL);
  queue_three_keys(&f);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "x");
  g_assert_cmpstr(a.commit, ==, "x");
  g_assert_true(a.replay);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  guint n = 0;
  const VoiceKey *q = voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpuint(q[0].keyval, ==, IBUS_KEY_a);
  g_assert_cmpuint(q[2].keyval, ==, IBUS_KEY_BackSpace);
  voice_fsm_drop_queue(&f);
  n = 99;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 0);
  voice_fsm_clear(&f);
}

static void test_err_and_disconnect_replay(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);

  to_finalizing(&f, NULL);
  queue_three_keys(&f);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_ERR, "busy");
  g_assert_true(a.replay);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_nonnull(a.notify);
  g_assert_true(g_str_has_prefix(a.notify, VOICE_NOTIFY_ERROR_PREFIX));
  g_assert_nonnull(strstr(a.notify, "busy"));
  guint n = 0;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 3);
  voice_fsm_drop_queue(&f);

  to_finalizing(&f, NULL);
  queue_three_keys(&f);
  a = voice_fsm_disconnected(&f);
  g_assert_true(a.replay);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_DISCONNECTED);
  n = 0;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 3);
  voice_fsm_drop_queue(&f);
  voice_fsm_clear(&f);
}

static void test_escape_while_finalizing_drops_queue(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, "x");
  queue_three_keys(&f);
  VoiceAction a = press(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_true(a.consume);
  g_assert_false(a.replay);
  guint n = 99;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 0);
  voice_fsm_clear(&f);
}

static void test_idle_after_final_passes_keys(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, NULL);
  voice_fsm_event(&f, VOICE_EV_FINAL, "abc");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  VoiceAction a = press(&f, IBUS_KEY_BackSpace, KEYCODE_BS, 0);
  g_assert_false(a.consume);
  voice_fsm_clear(&f);
}

static void test_focus_out_cancels(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);

  to_recording(&f);
  VoiceAction a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_FOCUS_LOST);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);

  to_finalizing(&f, "x");
  queue_three_keys(&f);
  a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_FOCUS_LOST);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_false(a.replay);
  guint n = 99;
  voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 0);

  /* In IDLE focus-out is a no-op. */
  a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_null(a.notify);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_disconnect_returns_idle(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_recording(&f);
  VoiceAction a = voice_fsm_disconnected(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_DISCONNECTED);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_err_event_notifies(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_ERR, "busy");
  g_assert_nonnull(a.notify);
  g_assert_true(g_str_has_prefix(a.notify, VOICE_NOTIFY_ERROR_PREFIX));
  g_assert_nonnull(strstr(a.notify, "busy"));
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_cmpstr(a.aux, ==, "");
  voice_fsm_clear(&f);
}

static void test_chord_does_not_trigger(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  VoiceAction a = press(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, 0);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  a = press(&f, IBUS_KEY_Tab, KEYCODE_TAB, IBUS_MOD1_MASK);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  a = release(&f, IBUS_KEY_Tab, KEYCODE_TAB, IBUS_MOD1_MASK);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  a = release(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_MOD1_MASK);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_composing_ignores_hotkey(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  VoiceAction a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, 0, TRUE);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_RELEASE_MASK | IBUS_MOD1_MASK, TRUE);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  /* Same via the helper. */
  a = tap(&f, TRUE);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_idle_other_keys_pass(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  VoiceAction a = press(&f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  voice_fsm_clear(&f);
}

static void test_shifted_hotkey_does_not_arm(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, FALSE);
  voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_SHIFT_MASK, FALSE);
  VoiceAction a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE,
                                IBUS_RELEASE_MASK | IBUS_SHIFT_MASK | IBUS_MOD1_MASK, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

/* ---- review mode ---- */

#define KEYCODE_RETURN 28
#define KEYCODE_KPENTER 96
#define KEYCODE_LEFT 105
#define KEYCODE_RIGHT 106
#define KEYCODE_HOME 102
#define KEYCODE_END 107
#define KEYCODE_DELETE 111
#define KEYCODE_SHIFT 50
#define KEYCODE_C 54
#define KEYCODE_UP 103
#define KEYCODE_DOWN 108

static void enter_review(VoiceFsm *f, const char *text) {
  VoiceAction a = tap(f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  a = tap(f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  voice_fsm_event(f, VOICE_EV_FINAL, text);
  g_assert_cmpint(f->state, ==, VOICE_REVIEW);
}

static VoiceAction rkey(VoiceFsm *f, guint keyval, guint keycode) {
  return press(f, keyval, keycode, 0);
}

static void left_n(VoiceFsm *f, guint n) {
  for (guint i = 0; i < n; i++) {
    VoiceAction a = rkey(f, IBUS_KEY_Left, KEYCODE_LEFT);
    g_assert_true(a.consume);
  }
}

/* In REVIEW the aux text is only the header line. */
static void assert_header(const char *aux) {
  GString *g = g_string_new("");
  voice_review_header(IBUS_KEY_Alt_R, g);
  g_assert_nonnull(aux);
  g_assert_cmpstr(aux, ==, g->str);
  g_assert_true(g_str_has_prefix(aux, VOICE_REVIEW_PREFIX));
  g_assert_null(strchr(aux, '\n'));
  g_string_free(g, TRUE);
}

/* Compare voice_fsm_note() with a NULL-terminated list of rows and the cursor row. */
static void assert_rows_(VoiceFsm *f, gint want_cursor, const char *const *want) {
  gint cr = -2;
  GPtrArray *rows = voice_fsm_note(f, &cr);
  guint n = 0;
  while (want[n]) n++;
  g_assert_nonnull(rows);
  g_assert_cmpuint(rows->len, ==, n);
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr(rows->pdata[i], ==, want[i]);
  g_assert_cmpint(cr, ==, want_cursor);
}
#define assert_rows(f, cursor_row, ...) \
  assert_rows_((f), (cursor_row), (const char *const[]){__VA_ARGS__, NULL})

static void assert_review(VoiceFsm *f, const char *text, guint cursor) {
  g_assert_cmpint(f->state, ==, VOICE_REVIEW);
  g_assert_cmpstr(f->review->str, ==, text);
  g_assert_cmpuint(f->cursor, ==, cursor);
}

static void test_final_enters_review(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "你好世界");
  g_assert_null(a.commit);
  assert_review(&f, "你好世界", 4);
  assert_header(a.aux);
  g_assert_true(a.note);
  assert_rows(&f, 0, "你好世界" VOICE_REVIEW_CURSOR);
  voice_fsm_clear(&f);
}

static void test_review_empty_final_goes_idle(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "");
  g_assert_null(a.commit);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_review_cursor_moves(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  VoiceAction a = rkey(&f, IBUS_KEY_Left, KEYCODE_LEFT);
  g_assert_true(a.consume);
  a = rkey(&f, IBUS_KEY_Left, KEYCODE_LEFT);
  g_assert_true(a.consume);
  assert_review(&f, "你好世界", 2);
  g_assert_true(a.note);
  if (a.aux) assert_header(a.aux);
  assert_rows(&f, 0, "你好" VOICE_REVIEW_CURSOR "世界");
  a = rkey(&f, IBUS_KEY_Home, KEYCODE_HOME);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, "你好世界", 0);
  assert_rows(&f, 0, VOICE_REVIEW_CURSOR "你好世界");
  a = rkey(&f, IBUS_KEY_Left, KEYCODE_LEFT);
  g_assert_true(a.consume);
  assert_review(&f, "你好世界", 0);
  a = rkey(&f, IBUS_KEY_End, KEYCODE_END);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, "你好世界", 4);
  assert_rows(&f, 0, "你好世界" VOICE_REVIEW_CURSOR);
  a = rkey(&f, IBUS_KEY_Right, KEYCODE_RIGHT);
  g_assert_true(a.consume);
  assert_review(&f, "你好世界", 4);
  voice_fsm_clear(&f);
}

static void test_review_backspace_delete(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 2);
  VoiceAction a = rkey(&f, IBUS_KEY_BackSpace, KEYCODE_BS);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, "你世界", 1);
  assert_rows(&f, 0, "你" VOICE_REVIEW_CURSOR "世界");
  a = rkey(&f, IBUS_KEY_Delete, KEYCODE_DELETE);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, "你界", 1);
  assert_rows(&f, 0, "你" VOICE_REVIEW_CURSOR "界");
  /* no-ops at the edges */
  rkey(&f, IBUS_KEY_Home, KEYCODE_HOME);
  a = rkey(&f, IBUS_KEY_BackSpace, KEYCODE_BS);
  g_assert_true(a.consume);
  assert_review(&f, "你界", 0);
  rkey(&f, IBUS_KEY_End, KEYCODE_END);
  a = rkey(&f, IBUS_KEY_Delete, KEYCODE_DELETE);
  g_assert_true(a.consume);
  assert_review(&f, "你界", 2);
  voice_fsm_clear(&f);
}

static void test_review_enter_commits(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  VoiceAction a = press(&f, IBUS_KEY_Return, KEYCODE_RETURN, 0);
  g_assert_true(a.consume);
  g_assert_cmpstr(a.commit, ==, "你好世界");
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_cmpuint(f.review->len, ==, 0);
  voice_fsm_clear(&f);

  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  a = press(&f, IBUS_KEY_KP_Enter, KEYCODE_KPENTER, 0);
  g_assert_true(a.consume);
  g_assert_cmpstr(a.commit, ==, "你好世界");
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_cmpuint(f.review->len, ==, 0);
  voice_fsm_clear(&f);
}

static void test_review_shift_enter_commits_too(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  VoiceAction a = press(&f, IBUS_KEY_Return, KEYCODE_RETURN, IBUS_SHIFT_MASK);
  g_assert_true(a.consume);
  g_assert_cmpstr(a.commit, ==, "你好世界");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_review_escape_discards(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  VoiceAction a = rkey(&f, IBUS_KEY_Escape, KEYCODE_ESC);
  g_assert_true(a.consume);
  g_assert_null(a.commit);
  g_assert_cmpstr(a.aux, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_review_composing_passes_everything(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 1);
  VoiceAction a;
  a = voice_fsm_key(&f, IBUS_KEY_Return, KEYCODE_RETURN, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  a = voice_fsm_key(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  a = voice_fsm_key(&f, IBUS_KEY_Left, KEYCODE_LEFT, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  a = voice_fsm_key(&f, IBUS_KEY_BackSpace, KEYCODE_BS, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  a = voice_fsm_key(&f, IBUS_KEY_a, KEYCODE_A, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  a = tap(&f, TRUE);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  assert_review(&f, "你好世界", 3);
  voice_fsm_clear(&f);
}

static void test_review_insert_at_cursor(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 2);
  VoiceAction a = voice_fsm_insert(&f, "美丽的");
  assert_review(&f, "你好美丽的世界", 5);
  g_assert_null(a.commit);
  assert_header(a.aux);
  g_assert_true(a.note);
  assert_rows(&f, 0, "你好美丽的" VOICE_REVIEW_CURSOR "世界");
  voice_fsm_clear(&f);
}

static void test_review_other_keys_go_to_rime_then_unhandled(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 2);
  VoiceAction a = press(&f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 2);
  a = voice_fsm_unhandled(&f, IBUS_KEY_a, 0);
  g_assert_true(a.consume);
  assert_review(&f, "你好a世界", 3);
  g_assert_null(a.commit);
  g_assert_true(a.note);
  if (a.aux) assert_header(a.aux);
  assert_rows(&f, 0, "你好a" VOICE_REVIEW_CURSOR "世界");
  /* Control combos are swallowed, not inserted */
  a = voice_fsm_unhandled(&f, IBUS_KEY_c, IBUS_CONTROL_MASK);
  g_assert_true(a.consume);
  assert_review(&f, "你好a世界", 3);
  g_assert_null(a.commit);
  /* releases are swallowed, not inserted */
  a = voice_fsm_unhandled(&f, IBUS_KEY_a, IBUS_RELEASE_MASK);
  g_assert_true(a.consume);
  assert_review(&f, "你好a世界", 3);
  /* Shift must reach Rime for its 中/英 toggle */
  a = rkey(&f, IBUS_KEY_Shift_L, KEYCODE_SHIFT);
  g_assert_false(a.consume);
  assert_review(&f, "你好a世界", 3);
  voice_fsm_clear(&f);
}

static void test_unhandled_and_insert_outside_review_are_noops(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  VoiceAction a = voice_fsm_unhandled(&f, IBUS_KEY_a, 0);
  g_assert_false(a.consume);
  g_assert_null(a.aux);
  g_assert_null(a.commit);
  a = voice_fsm_insert(&f, "abc");
  g_assert_false(a.consume);
  g_assert_null(a.aux);
  g_assert_null(a.commit);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_review_hotkey_resumes_recording(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 2);
  VoiceAction a = tap(&f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  g_assert_cmpstr(a.aux, ==, VOICE_AUX_RECORDING);
  a = tap(&f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  g_assert_cmpint(f.state, ==, VOICE_FINALIZING);
  a = voice_fsm_event(&f, VOICE_EV_FINAL, "很大的");
  g_assert_null(a.commit);
  assert_review(&f, "你好很大的世界", 5);
  assert_header(a.aux);
  g_assert_true(a.note);
  assert_rows(&f, 0, "你好很大的" VOICE_REVIEW_CURSOR "世界");
  voice_fsm_clear(&f);
}

/* Fresh review "你好世界" with cursor 2, then a recording resumed from it. */
static void resume_from_review(VoiceFsm *f, gboolean finalizing) {
  voice_fsm_init(f, IBUS_KEY_Alt_R, TRUE);
  enter_review(f, "你好世界");
  left_n(f, 2);
  VoiceAction a = tap(f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  if (finalizing) {
    a = tap(f, FALSE);
    g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
    g_assert_cmpint(f->state, ==, VOICE_FINALIZING);
  }
}

static void assert_back_in_review(VoiceFsm *f, VoiceAction a) {
  assert_review(f, "你好世界", 2);
  assert_header(a.aux);
  g_assert_true(a.note);
  assert_rows(f, 0, "你好" VOICE_REVIEW_CURSOR "世界");
  g_assert_null(a.commit);
}

static void test_review_resume_cancel_returns_to_review(void) {
  VoiceFsm f;
  VoiceAction a;

  resume_from_review(&f, FALSE);
  a = rkey(&f, IBUS_KEY_Escape, KEYCODE_ESC);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  assert_back_in_review(&f, a);
  voice_fsm_clear(&f);

  resume_from_review(&f, FALSE);
  a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_FOCUS_LOST);
  assert_back_in_review(&f, a);
  voice_fsm_clear(&f);

  resume_from_review(&f, TRUE);
  a = voice_fsm_event(&f, VOICE_EV_ERR, "busy");
  g_assert_nonnull(a.notify);
  g_assert_true(g_str_has_prefix(a.notify, VOICE_NOTIFY_ERROR_PREFIX));
  assert_back_in_review(&f, a);
  voice_fsm_clear(&f);

  resume_from_review(&f, FALSE);
  a = voice_fsm_disconnected(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_DISCONNECTED);
  assert_back_in_review(&f, a);
  voice_fsm_clear(&f);

  resume_from_review(&f, TRUE);
  a = voice_fsm_event(&f, VOICE_EV_FINAL, "");
  g_assert_null(a.notify);
  assert_back_in_review(&f, a);
  voice_fsm_clear(&f);
}

static void test_review_survives_focus_change(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  VoiceAction a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_null(a.notify);
  assert_review(&f, "你好世界", 4);
  a = voice_fsm_focus_in(&f);
  assert_header(a.aux);
  g_assert_true(a.note);
  assert_rows(&f, 0, "你好世界" VOICE_REVIEW_CURSOR);
  assert_review(&f, "你好世界", 4);
  voice_fsm_clear(&f);
}

static void test_focus_in_idle_is_noop(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  VoiceAction a = voice_fsm_focus_in(&f);
  g_assert_null(a.aux);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static GPtrArray *wrap(const char *t, guint cols) { return voice_wrap(t, cols); }

static void test_wrap_breaks_by_columns(void) {
  GString *t = g_string_new("");
  for (int i = 0; i < 40; i++) g_string_append(t, "中");
  GPtrArray *l = wrap(t->str, 36);
  g_assert_cmpuint(l->len, ==, 3);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[0], -1), ==, 18);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[1], -1), ==, 18);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[2], -1), ==, 4);
  g_ptr_array_unref(l);
  g_string_free(t, TRUE);
}

static void test_wrap_short_is_one_line(void) {
  GPtrArray *l = wrap("你好 world", 36);
  g_assert_cmpuint(l->len, ==, 1);
  g_assert_cmpstr(l->pdata[0], ==, "你好 world");
  g_ptr_array_unref(l);
  l = wrap("", 36);
  g_assert_cmpuint(l->len, ==, 1);
  g_assert_cmpstr(l->pdata[0], ==, "");
  g_ptr_array_unref(l);
}

static void test_wrap_no_line_starts_with_punct(void) {
  GString *t = g_string_new("");
  for (int i = 0; i < 18; i++) g_string_append(t, "中");
  g_string_append(t, "，好");
  GPtrArray *l = wrap(t->str, 36);
  g_assert_cmpuint(l->len, ==, 2);
  g_assert_true(g_str_has_suffix(l->pdata[0], "中，"));
  g_assert_cmpstr(l->pdata[1], ==, "好");
  g_ptr_array_unref(l);
  g_string_free(t, TRUE);
}

static void test_wrap_keeps_short_ascii_word(void) {
  GString *t = g_string_new("");
  for (int i = 0; i < 16; i++) g_string_append(t, "中");
  g_string_append(t, "hello");
  GPtrArray *l = wrap(t->str, 36);
  g_assert_cmpuint(l->len, ==, 2);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[0], -1), ==, 16);
  g_assert_cmpstr(l->pdata[1], ==, "hello");
  g_ptr_array_unref(l);
  g_string_free(t, TRUE);
}

static void test_wrap_long_ascii_run_hard_breaks(void) {
  char t[81];
  memset(t, 'a', 80);
  t[80] = 0;
  GPtrArray *l = wrap(t, 36);
  g_assert_cmpuint(l->len, ==, 3);
  g_assert_cmpuint(strlen(l->pdata[0]), ==, 36);
  g_assert_cmpuint(strlen(l->pdata[1]), ==, 36);
  g_assert_cmpuint(strlen(l->pdata[2]), ==, 8);
  g_ptr_array_unref(l);
}

static void test_wrap_counts_ambiguous_as_wide(void) {
  GString *t = g_string_new("");
  for (int i = 0; i < 20; i++) g_string_append(t, VOICE_REVIEW_CURSOR);
  GPtrArray *l = wrap(t->str, 36);
  g_assert_cmpuint(l->len, ==, 2);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[0], -1), ==, 18);
  g_assert_cmpuint(g_utf8_strlen(l->pdata[1], -1), ==, 2);
  g_ptr_array_unref(l);
  g_string_free(t, TRUE);
}

static void test_review_header_names_hotkey(void) {
  GString *g = g_string_new("");
  voice_review_header(IBUS_KEY_Alt_R, g);
  g_assert_true(g_str_has_prefix(g->str, VOICE_REVIEW_PREFIX));
  g_assert_nonnull(strstr(g->str, "右Alt"));
  g_assert_nonnull(strstr(g->str, "上屏"));
  g_assert_null(strstr(g->str, "发送"));
  g_assert_nonnull(strstr(g->str, "放弃"));
  g_assert_nonnull(strstr(g->str, "语音草稿"));
  g_assert_null(strchr(g->str, '\n'));
  g_string_truncate(g, 0);
  voice_review_header(IBUS_KEY_F9, g);
  g_assert_nonnull(strstr(g->str, "F9"));
  g_assert_nonnull(strstr(g->str, "语音草稿"));
  g_assert_nonnull(strstr(g->str, "上屏"));
  g_assert_null(strstr(g->str, "发送"));
  g_assert_null(strchr(g->str, '\n'));
  g_string_free(g, TRUE);
}

static void test_review_note_wraps_and_windows(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  GString *t = g_string_new("");
  for (int i = 0; i < 200; i++) g_string_append(t, "中");   /* 11 rows of 18 + 2, marker in the 12th */
  enter_review(&f, t->str);
  VoiceAction a = voice_fsm_focus_in(&f);                     /* cursor at the end */
  assert_header(a.aux);
  g_assert_true(a.note);
  gint cr = -2;
  GPtrArray *rows = voice_fsm_note(&f, &cr);
  /* "⋯", then the last 6 rows; the cursor row is the last one */
  g_assert_cmpuint(rows->len, ==, 1 + VOICE_REVIEW_MAX_LINES);
  g_assert_cmpstr(rows->pdata[0], ==, VOICE_REVIEW_MORE);
  g_assert_cmpint(cr, ==, VOICE_REVIEW_MAX_LINES);
  g_assert_true(g_str_has_suffix(rows->pdata[cr], VOICE_REVIEW_CURSOR));

  a = rkey(&f, IBUS_KEY_Home, KEYCODE_HOME);
  g_assert_true(a.note);
  rows = voice_fsm_note(&f, &cr);
  g_assert_cmpuint(rows->len, ==, VOICE_REVIEW_MAX_LINES + 1);
  g_assert_true(g_str_has_prefix(rows->pdata[0], VOICE_REVIEW_CURSOR "中"));
  g_assert_cmpstr(rows->pdata[VOICE_REVIEW_MAX_LINES], ==, VOICE_REVIEW_MORE);
  g_assert_cmpint(cr, ==, 0);
  g_string_free(t, TRUE);
  voice_fsm_clear(&f);
}

/* Cursor in the middle: rows hidden both above and below. */
static void test_review_note_window_middle(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  GString *t = g_string_new("");
  for (int i = 0; i < 200; i++) g_string_append(t, "中");
  enter_review(&f, t->str);
  rkey(&f, IBUS_KEY_Home, KEYCODE_HOME);
  for (int i = 0; i < 5; i++) {
    VoiceAction a = rkey(&f, IBUS_KEY_Down, KEYCODE_DOWN);
    g_assert_true(a.consume);
    g_assert_true(a.note);
  }
  /* cursor is on full row 5, so rows 2..7 are shown: first = clamp(5 - 3, 0, n - 6) */
  gint cr = -2;
  GPtrArray *rows = voice_fsm_note(&f, &cr);
  g_assert_cmpuint(rows->len, ==, 1 + VOICE_REVIEW_MAX_LINES + 1);
  g_assert_cmpstr(rows->pdata[0], ==, VOICE_REVIEW_MORE);
  g_assert_cmpstr(rows->pdata[VOICE_REVIEW_MAX_LINES + 1], ==, VOICE_REVIEW_MORE);
  g_assert_cmpint(cr, ==, 4);
  g_assert_nonnull(strstr(rows->pdata[cr], VOICE_REVIEW_CURSOR));
  g_string_free(t, TRUE);
  voice_fsm_clear(&f);
}

/* 40 distinct characters; at cols 36 the rows of the plain text are
 * [0..17] [18..35] [36..39]; the marker takes no wrap width, so rows do not shift as the
 * cursor moves. */
static void test_review_up_down_moves_by_row(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  GString *t = g_string_new("");
  for (int i = 0; i < 4; i++) g_string_append(t, "一二三四五六七八九十");
  enter_review(&f, t->str);
  assert_review(&f, t->str, 40);
  gint cr = -2;
  GPtrArray *rows = voice_fsm_note(&f, &cr);
  g_assert_cmpuint(rows->len, ==, 3);
  g_assert_cmpint(cr, ==, 2);
  g_assert_cmpstr(rows->pdata[2], ==, "七八九十" VOICE_REVIEW_CURSOR);

  /* offset 4 in row 2 -> row 1 starts at char 18 -> 22 */
  VoiceAction a = rkey(&f, IBUS_KEY_Up, KEYCODE_UP);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, t->str, 22);
  voice_fsm_note(&f, &cr);
  g_assert_cmpint(cr, ==, 1);
  /* offset 4 in row 1 -> row 0 -> 4 */
  a = rkey(&f, IBUS_KEY_Up, KEYCODE_UP);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, t->str, 4);
  voice_fsm_note(&f, &cr);
  g_assert_cmpint(cr, ==, 0);
  /* first row: no-op */
  a = rkey(&f, IBUS_KEY_Up, KEYCODE_UP);
  g_assert_true(a.consume);
  assert_review(&f, t->str, 4);
  /* Rows are measured on the plain text (the marker takes no wrap width), so they stay
   * [0..17] [18..35] [36..39]: Down from 4 -> 22, Down -> 40 (clamped to row 2's length). */
  a = rkey(&f, IBUS_KEY_Down, KEYCODE_DOWN);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, t->str, 22);
  a = rkey(&f, IBUS_KEY_Down, KEYCODE_DOWN);
  g_assert_true(a.consume);
  g_assert_true(a.note);
  assert_review(&f, t->str, 40);
  /* last row: no-op */
  a = rkey(&f, IBUS_KEY_Down, KEYCODE_DOWN);
  g_assert_true(a.consume);
  assert_review(&f, t->str, 40);
  voice_fsm_note(&f, &cr);
  g_assert_cmpint(cr, ==, 2);
  g_string_free(t, TRUE);
  voice_fsm_clear(&f);
}

/* Up/Down on a one-row review change nothing and are swallowed. */
static void test_review_up_down_single_row_noop(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 1);
  VoiceAction a = rkey(&f, IBUS_KEY_Up, KEYCODE_UP);
  g_assert_true(a.consume);
  assert_review(&f, "你好世界", 3);
  a = rkey(&f, IBUS_KEY_Down, KEYCODE_DOWN);
  g_assert_true(a.consume);
  assert_review(&f, "你好世界", 3);
  assert_rows(&f, 0, "你好世" VOICE_REVIEW_CURSOR "界");
  /* composing: Up/Down belong to Rime (candidate selection) */
  a = voice_fsm_key(&f, IBUS_KEY_Up, KEYCODE_UP, 0, TRUE);
  g_assert_false(a.consume);
  a = voice_fsm_key(&f, IBUS_KEY_Down, KEYCODE_DOWN, 0, TRUE);
  g_assert_false(a.consume);
  assert_review(&f, "你好世界", 3);
  voice_fsm_clear(&f);
}

static void test_compose_hint_shows_cursor_row(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好世界");
  left_n(&f, 2);
  g_assert_cmpstr(voice_fsm_compose_hint(&f), ==, VOICE_COMPOSE_PREFIX "你好" VOICE_REVIEW_CURSOR "世界");
  g_assert_cmpstr(voice_fsm_compose_hint(&f), ==, "✎ 你好▲世界");
  voice_fsm_clear(&f);
}

static void test_note_empty_outside_review(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  gint cr = 99;
  GPtrArray *rows = voice_fsm_note(&f, &cr);
  g_assert_nonnull(rows);
  g_assert_cmpuint(rows->len, ==, 0);
  g_assert_cmpint(cr, ==, -1);
  to_recording(&f);
  cr = 99;
  rows = voice_fsm_note(&f, &cr);
  g_assert_cmpuint(rows->len, ==, 0);
  g_assert_cmpint(cr, ==, -1);
  /* leaving REVIEW: aux hidden, rows gone */
  voice_fsm_clear(&f);
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  enter_review(&f, "你好");
  VoiceAction a = press(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0);
  g_assert_cmpstr(a.aux, ==, "");
  cr = 99;
  rows = voice_fsm_note(&f, &cr);
  g_assert_cmpuint(rows->len, ==, 0);
  g_assert_cmpint(cr, ==, -1);
  voice_fsm_clear(&f);
}

static void test_keys_queued_while_finalizing_replay_into_review(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R, TRUE);
  to_finalizing(&f, NULL);
  queue_three_keys(&f);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "x");
  g_assert_null(a.commit);
  g_assert_true(a.replay);
  assert_review(&f, "x", 1);
  guint n = 0;
  const VoiceKey *q = voice_fsm_queued(&f, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpuint(q[0].keyval, ==, IBUS_KEY_a);
  g_assert_cmpuint(q[2].keyval, ==, IBUS_KEY_BackSpace);
  voice_fsm_drop_queue(&f);
  voice_fsm_clear(&f);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/voice_fsm/tap_starts_recording", test_tap_starts_recording);
  g_test_add_func("/voice_fsm/partial_updates_aux", test_partial_updates_aux);
  g_test_add_func("/voice_fsm/second_tap_stops", test_second_tap_stops);
  g_test_add_func("/voice_fsm/final_commits_and_idles", test_final_commits_and_idles);
  g_test_add_func("/voice_fsm/empty_final_commits_nothing", test_empty_final_commits_nothing);
  g_test_add_func("/voice_fsm/escape_cancels", test_escape_cancels);
  g_test_add_func("/voice_fsm/other_keys_swallowed_while_recording",
                  test_other_keys_swallowed_while_recording);
  g_test_add_func("/voice_fsm/keys_queued_while_finalizing", test_keys_queued_while_finalizing);
  g_test_add_func("/voice_fsm/final_replays_queue", test_final_replays_queue);
  g_test_add_func("/voice_fsm/err_and_disconnect_replay", test_err_and_disconnect_replay);
  g_test_add_func("/voice_fsm/escape_while_finalizing_drops_queue",
                  test_escape_while_finalizing_drops_queue);
  g_test_add_func("/voice_fsm/idle_after_final_passes_keys", test_idle_after_final_passes_keys);
  g_test_add_func("/voice_fsm/focus_out_cancels", test_focus_out_cancels);
  g_test_add_func("/voice_fsm/disconnect_returns_idle", test_disconnect_returns_idle);
  g_test_add_func("/voice_fsm/err_event_notifies", test_err_event_notifies);
  g_test_add_func("/voice_fsm/chord_does_not_trigger", test_chord_does_not_trigger);
  g_test_add_func("/voice_fsm/composing_ignores_hotkey", test_composing_ignores_hotkey);
  g_test_add_func("/voice_fsm/idle_other_keys_pass", test_idle_other_keys_pass);
  g_test_add_func("/voice_fsm/shifted_hotkey_does_not_arm", test_shifted_hotkey_does_not_arm);
  g_test_add_func("/voice_fsm/final_enters_review", test_final_enters_review);
  g_test_add_func("/voice_fsm/review_empty_final_goes_idle", test_review_empty_final_goes_idle);
  g_test_add_func("/voice_fsm/review_cursor_moves", test_review_cursor_moves);
  g_test_add_func("/voice_fsm/review_backspace_delete", test_review_backspace_delete);
  g_test_add_func("/voice_fsm/review_enter_commits", test_review_enter_commits);
  g_test_add_func("/voice_fsm/review_shift_enter_commits_too", test_review_shift_enter_commits_too);
  g_test_add_func("/voice_fsm/review_escape_discards", test_review_escape_discards);
  g_test_add_func("/voice_fsm/review_composing_passes_everything", test_review_composing_passes_everything);
  g_test_add_func("/voice_fsm/review_insert_at_cursor", test_review_insert_at_cursor);
  g_test_add_func("/voice_fsm/review_other_keys_go_to_rime_then_unhandled", test_review_other_keys_go_to_rime_then_unhandled);
  g_test_add_func("/voice_fsm/unhandled_and_insert_outside_review_are_noops", test_unhandled_and_insert_outside_review_are_noops);
  g_test_add_func("/voice_fsm/review_hotkey_resumes_recording", test_review_hotkey_resumes_recording);
  g_test_add_func("/voice_fsm/review_resume_cancel_returns_to_review", test_review_resume_cancel_returns_to_review);
  g_test_add_func("/voice_fsm/review_survives_focus_change", test_review_survives_focus_change);
  g_test_add_func("/voice_fsm/focus_in_idle_is_noop", test_focus_in_idle_is_noop);
  g_test_add_func("/voice_fsm/wrap_breaks_by_columns", test_wrap_breaks_by_columns);
  g_test_add_func("/voice_fsm/wrap_short_is_one_line", test_wrap_short_is_one_line);
  g_test_add_func("/voice_fsm/wrap_no_line_starts_with_punct", test_wrap_no_line_starts_with_punct);
  g_test_add_func("/voice_fsm/wrap_keeps_short_ascii_word", test_wrap_keeps_short_ascii_word);
  g_test_add_func("/voice_fsm/wrap_long_ascii_run_hard_breaks", test_wrap_long_ascii_run_hard_breaks);
  g_test_add_func("/voice_fsm/wrap_counts_ambiguous_as_wide", test_wrap_counts_ambiguous_as_wide);
  g_test_add_func("/voice_fsm/review_header_names_hotkey", test_review_header_names_hotkey);
  g_test_add_func("/voice_fsm/review_note_wraps_and_windows", test_review_note_wraps_and_windows);
  g_test_add_func("/voice_fsm/review_note_window_middle", test_review_note_window_middle);
  g_test_add_func("/voice_fsm/review_up_down_moves_by_row", test_review_up_down_moves_by_row);
  g_test_add_func("/voice_fsm/review_up_down_single_row_noop", test_review_up_down_single_row_noop);
  g_test_add_func("/voice_fsm/compose_hint_shows_cursor_row", test_compose_hint_shows_cursor_row);
  g_test_add_func("/voice_fsm/note_empty_outside_review", test_note_empty_outside_review);
  g_test_add_func("/voice_fsm/keys_queued_while_finalizing_replay_into_review", test_keys_queued_while_finalizing_replay_into_review);
  return g_test_run();
}
