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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  VoiceAction a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, 0, FALSE);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_RELEASE_MASK | IBUS_MOD1_MASK, FALSE);
  g_assert_true(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_START);
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  g_assert_cmpstr(a.preedit, ==, VOICE_PREEDIT_RECORDING);
  voice_fsm_clear(&f);
}

static void test_partial_updates_preedit(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_recording(&f);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_PARTIAL, "你好");
  g_assert_cmpstr(a.preedit, ==, VOICE_PREEDIT_RECORDING "你好");
  g_assert_cmpstr(a.preedit, ==, "🎤 你好");
  g_assert_cmpint(f.state, ==, VOICE_RECORDING);
  voice_fsm_clear(&f);
}

static void test_second_tap_stops(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_recording(&f);
  voice_fsm_event(&f, VOICE_EV_PARTIAL, "你好");
  VoiceAction a = tap(&f, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_STOP);
  g_assert_cmpint(f.state, ==, VOICE_FINALIZING);
  g_assert_nonnull(a.preedit);
  g_assert_true(g_str_has_prefix(a.preedit, VOICE_PREEDIT_FINALIZING));
  g_assert_nonnull(strstr(a.preedit, "你好"));
  voice_fsm_clear(&f);
}

static void test_final_commits_and_idles(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_finalizing(&f, "你好");
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "你好 world");
  g_assert_cmpstr(a.commit, ==, "你好 world");
  g_assert_cmpstr(a.preedit, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_false(a.replay);
  voice_fsm_clear(&f);
}

static void test_empty_final_commits_nothing(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_FINAL, "");
  g_assert_null(a.commit);
  g_assert_cmpstr(a.preedit, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_escape_cancels(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_recording(&f);
  VoiceAction a = press(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.preedit, ==, "");
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);

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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_finalizing(&f, "x");
  queue_three_keys(&f);
  VoiceAction a = press(&f, IBUS_KEY_Escape, KEYCODE_ESC, 0);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.preedit, ==, "");
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_finalizing(&f, NULL);
  voice_fsm_event(&f, VOICE_EV_FINAL, "abc");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  VoiceAction a = press(&f, IBUS_KEY_BackSpace, KEYCODE_BS, 0);
  g_assert_false(a.consume);
  voice_fsm_clear(&f);
}

static void test_focus_out_cancels(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);

  to_recording(&f);
  VoiceAction a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_FOCUS_LOST);
  g_assert_cmpstr(a.preedit, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);

  to_finalizing(&f, "x");
  queue_three_keys(&f);
  a = voice_fsm_focus_out(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_CANCEL);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_FOCUS_LOST);
  g_assert_cmpstr(a.preedit, ==, "");
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_recording(&f);
  VoiceAction a = voice_fsm_disconnected(&f);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpstr(a.notify, ==, VOICE_NOTIFY_DISCONNECTED);
  g_assert_cmpstr(a.preedit, ==, "");
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

static void test_err_event_notifies(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  to_finalizing(&f, NULL);
  VoiceAction a = voice_fsm_event(&f, VOICE_EV_ERR, "busy");
  g_assert_nonnull(a.notify);
  g_assert_true(g_str_has_prefix(a.notify, VOICE_NOTIFY_ERROR_PREFIX));
  g_assert_nonnull(strstr(a.notify, "busy"));
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  g_assert_cmpstr(a.preedit, ==, "");
  voice_fsm_clear(&f);
}

static void test_chord_does_not_trigger(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
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
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  VoiceAction a = press(&f, IBUS_KEY_a, KEYCODE_A, 0);
  g_assert_false(a.consume);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  voice_fsm_clear(&f);
}

static void test_shifted_hotkey_does_not_arm(void) {
  VoiceFsm f;
  voice_fsm_init(&f, IBUS_KEY_Alt_R);
  voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE, IBUS_SHIFT_MASK, FALSE);
  VoiceAction a = voice_fsm_key(&f, IBUS_KEY_Alt_R, HOTKEY_CODE,
                                IBUS_RELEASE_MASK | IBUS_SHIFT_MASK | IBUS_MOD1_MASK, FALSE);
  g_assert_cmpint(a.send, ==, VOICE_CMD_NONE);
  g_assert_cmpint(f.state, ==, VOICE_IDLE);
  voice_fsm_clear(&f);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/voice_fsm/tap_starts_recording", test_tap_starts_recording);
  g_test_add_func("/voice_fsm/partial_updates_preedit", test_partial_updates_preedit);
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
  return g_test_run();
}
