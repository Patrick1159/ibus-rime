#include <ibus.h>
#include "voice_fsm.h"

#define IGNORED_MODS (IBUS_LOCK_MASK | IBUS_MOD2_MASK)

static VoiceAction pass(gboolean consume) {
  VoiceAction a = {consume, VOICE_CMD_NONE, NULL, NULL, NULL, FALSE};
  return a;
}

void voice_fsm_init(VoiceFsm *f, guint hotkey_keyval) {
  f->state = VOICE_IDLE;
  f->hotkey = hotkey_keyval;
  f->armed = FALSE;
  f->buf = g_string_new("");
  f->partial = g_string_new("");
  f->commit_buf = g_string_new("");
  f->notify_buf = g_string_new("");
  f->queued = g_array_new(FALSE, FALSE, sizeof(VoiceKey));
}

void voice_fsm_clear(VoiceFsm *f) {
  g_string_free(f->buf, TRUE);
  g_string_free(f->partial, TRUE);
  g_string_free(f->commit_buf, TRUE);
  g_string_free(f->notify_buf, TRUE);
  g_array_free(f->queued, TRUE);
}

static const char *show(VoiceFsm *f, const char *prefix) {
  g_string_assign(f->buf, prefix);
  g_string_append(f->buf, f->partial->str);
  return f->buf->str;
}

/* Leave the session; keeps the key queue for replay unless drop is set. */
static VoiceAction finish(VoiceFsm *f, VoiceCmd send, const char *notify, gboolean drop) {
  VoiceAction a = pass(TRUE);
  f->state = VOICE_IDLE;
  f->armed = FALSE;
  g_string_truncate(f->partial, 0);
  g_string_truncate(f->buf, 0);
  if (drop) voice_fsm_drop_queue(f);
  a.send = send;
  a.preedit = f->buf->str;
  a.notify = notify;
  a.replay = f->queued->len > 0;
  return a;
}

static gboolean bare(guint modifiers) {
  return (modifiers & ~IGNORED_MODS) == 0;
}

VoiceAction voice_fsm_key(VoiceFsm *f, guint keyval, guint keycode, guint modifiers,
                          gboolean rime_composing) {
  gboolean release = (modifiers & IBUS_RELEASE_MASK) != 0;
  gboolean hot = keyval == f->hotkey;
  VoiceAction a;

  switch (f->state) {
  case VOICE_IDLE:
    if (rime_composing) {
      f->armed = FALSE;
      return pass(FALSE);
    }
    if (hot && !release) {
      f->armed = bare(modifiers);
      return pass(f->armed);
    }
    if (hot && release && f->armed) {
      f->armed = FALSE;
      f->state = VOICE_RECORDING;
      g_string_truncate(f->partial, 0);
      a = pass(TRUE);
      a.send = VOICE_CMD_START;
      a.preedit = show(f, VOICE_PREEDIT_RECORDING);
      return a;
    }
    if (!release) f->armed = FALSE;
    return pass(FALSE);

  case VOICE_RECORDING:
    if (keyval == IBUS_KEY_Escape && !release)
      return finish(f, VOICE_CMD_CANCEL, NULL, TRUE);
    if (hot && !release) {
      f->armed = bare(modifiers);
      return pass(TRUE);
    }
    if (hot && release && f->armed) {
      f->armed = FALSE;
      f->state = VOICE_FINALIZING;
      a = pass(TRUE);
      a.send = VOICE_CMD_STOP;
      a.preedit = show(f, VOICE_PREEDIT_FINALIZING);
      return a;
    }
    if (!release) f->armed = FALSE;
    return pass(TRUE);

  case VOICE_FINALIZING:
    if (keyval == IBUS_KEY_Escape && !release)
      return finish(f, VOICE_CMD_CANCEL, NULL, TRUE);
    if (!hot) {
      VoiceKey k = {keyval, keycode, modifiers};
      g_array_append_val(f->queued, k);
    }
    return pass(TRUE);
  }
  return pass(FALSE);
}

VoiceAction voice_fsm_event(VoiceFsm *f, VoiceEvType type, const char *payload) {
  VoiceAction a = pass(FALSE);
  if (f->state == VOICE_IDLE) return a;
  switch (type) {
  case VOICE_EV_PARTIAL:
    g_string_assign(f->partial, payload);
    a.preedit = show(f, f->state == VOICE_RECORDING ? VOICE_PREEDIT_RECORDING
                                                     : VOICE_PREEDIT_FINALIZING);
    return a;
  case VOICE_EV_FINAL:
    g_string_assign(f->commit_buf, payload);
    a = finish(f, VOICE_CMD_NONE, NULL, FALSE);
    a.commit = f->commit_buf->len ? f->commit_buf->str : NULL;
    return a;
  case VOICE_EV_ERR:
    g_string_assign(f->notify_buf, VOICE_NOTIFY_ERROR_PREFIX);
    g_string_append(f->notify_buf, payload);
    return finish(f, VOICE_CMD_NONE, f->notify_buf->str, FALSE);
  default:
    return a;
  }
}

VoiceAction voice_fsm_disconnected(VoiceFsm *f) {
  if (f->state == VOICE_IDLE) return pass(FALSE);
  return finish(f, VOICE_CMD_NONE, VOICE_NOTIFY_DISCONNECTED, FALSE);
}

VoiceAction voice_fsm_focus_out(VoiceFsm *f) {
  if (f->state == VOICE_IDLE) return pass(FALSE);
  return finish(f, VOICE_CMD_CANCEL, VOICE_NOTIFY_FOCUS_LOST, TRUE);
}

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out) {
  *n_out = f->queued->len;
  return (const VoiceKey *)f->queued->data;
}

void voice_fsm_drop_queue(VoiceFsm *f) {
  g_array_set_size(f->queued, 0);
}
