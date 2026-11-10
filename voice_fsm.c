#include <ibus.h>
#include "voice_fsm.h"

#define IGNORED_MODS (IBUS_LOCK_MASK | IBUS_MOD2_MASK)
#define COMMAND_MODS (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_MOD4_MASK)

static VoiceAction pass(gboolean consume) {
  VoiceAction a = {consume, VOICE_CMD_NONE, NULL, NULL, NULL, FALSE, FALSE};
  return a;
}

void voice_fsm_init(VoiceFsm *f, guint hotkey_keyval, gboolean review_enabled) {
  f->state = VOICE_IDLE;
  f->hotkey = hotkey_keyval;
  f->review_enabled = review_enabled;
  f->armed = FALSE;
  f->from_review = FALSE;
  f->aux = g_string_new("");
  f->partial = g_string_new("");
  f->review = g_string_new("");
  f->cursor = 0;
  f->commit_buf = g_string_new("");
  f->notify_buf = g_string_new("");
  f->queued = g_array_new(FALSE, FALSE, sizeof(VoiceKey));
}

void voice_fsm_clear(VoiceFsm *f) {
  g_string_free(f->aux, TRUE);
  g_string_free(f->partial, TRUE);
  g_string_free(f->review, TRUE);
  g_string_free(f->commit_buf, TRUE);
  g_string_free(f->notify_buf, TRUE);
  g_array_free(f->queued, TRUE);
}

static const char *set_aux(VoiceFsm *f, const char *a, const char *b) {
  g_string_assign(f->aux, a);
  if (b) g_string_append(f->aux, b);
  return f->aux->str;
}

/* ---- review buffer ---- */

static gsize byte_at(VoiceFsm *f, glong chars) {
  return g_utf8_offset_to_pointer(f->review->str, chars) - f->review->str;
}

static guint review_len(VoiceFsm *f) {
  return g_utf8_strlen(f->review->str, -1);
}

static const char *render_review(VoiceFsm *f) {
  guint len = review_len(f);
  guint start = 0, end = len;
  if (len > VOICE_REVIEW_WINDOW) {
    gint s = (gint)f->cursor - VOICE_REVIEW_WINDOW / 2;
    start = (guint)CLAMP(s, 0, (gint)(len - VOICE_REVIEW_WINDOW));
    end = start + VOICE_REVIEW_WINDOW;
  }
  const char *text = f->review->str;
  g_string_assign(f->aux, VOICE_REVIEW_PREFIX);
  if (start > 0) g_string_append(f->aux, "…");
  g_string_append_len(f->aux, text + byte_at(f, start), byte_at(f, f->cursor) - byte_at(f, start));
  g_string_append(f->aux, VOICE_REVIEW_CURSOR);
  g_string_append_len(f->aux, text + byte_at(f, f->cursor), byte_at(f, end) - byte_at(f, f->cursor));
  if (end < len) g_string_append(f->aux, "…");
  g_string_append(f->aux, VOICE_REVIEW_HINT);
  return f->aux->str;
}

static void review_insert(VoiceFsm *f, const char *text) {
  g_string_insert(f->review, byte_at(f, f->cursor), text);
  f->cursor += g_utf8_strlen(text, -1);
}

static void review_erase(VoiceFsm *f, guint from) { /* one character at `from` */
  gsize a = byte_at(f, from);
  g_string_erase(f->review, a, byte_at(f, from + 1) - a);
}

static void review_reset(VoiceFsm *f) {
  g_string_truncate(f->review, 0);
  f->cursor = 0;
}

/* ---- sessions ---- */

static gboolean bare(guint modifiers) {
  return (modifiers & ~IGNORED_MODS) == 0;
}

static VoiceAction start_recording(VoiceFsm *f, gboolean from_review) {
  VoiceAction a = pass(TRUE);
  f->armed = FALSE;
  f->from_review = from_review;
  f->state = VOICE_RECORDING;
  g_string_truncate(f->partial, 0);
  a.send = VOICE_CMD_START;
  a.aux = set_aux(f, VOICE_AUX_RECORDING, NULL);
  return a;
}

/* End a recording without new text: back to REVIEW if it started there, else IDLE.
 * Queued keys are kept for replay unless drop is set. */
static VoiceAction leave_session(VoiceFsm *f, VoiceCmd send, const char *notify, gboolean drop) {
  VoiceAction a = pass(TRUE);
  f->armed = FALSE;
  g_string_truncate(f->partial, 0);
  if (drop) voice_fsm_drop_queue(f);
  if (f->from_review) {
    f->state = VOICE_REVIEW;
    a.aux = render_review(f);
  } else {
    f->state = VOICE_IDLE;
    a.aux = set_aux(f, "", NULL);
  }
  f->from_review = FALSE;
  a.send = send;
  a.notify = notify;
  a.replay = f->queued->len > 0;
  return a;
}

static VoiceAction finish_review(VoiceFsm *f, gboolean commit, gboolean send_return) {
  VoiceAction a = pass(TRUE);
  if (commit) {
    g_string_assign(f->commit_buf, f->review->str);
    a.commit = f->commit_buf->len ? f->commit_buf->str : NULL;
    a.send_return = send_return && a.commit;
  }
  review_reset(f);
  f->state = VOICE_IDLE;
  a.aux = set_aux(f, "", NULL);
  return a;
}

static VoiceAction review_key(VoiceFsm *f, guint keyval, guint modifiers, gboolean composing) {
  gboolean release = (modifiers & IBUS_RELEASE_MASK) != 0;
  if (composing) {
    f->armed = FALSE;
    return pass(FALSE);
  }
  if (keyval == f->hotkey) {
    if (!release) {
      f->armed = bare(modifiers);
      return pass(TRUE);
    }
    if (f->armed) return start_recording(f, TRUE);
    return pass(TRUE);
  }
  if (release) return pass(FALSE); /* Rime needs releases, e.g. Shift taps for 中/英 */
  f->armed = FALSE;

  guint len = review_len(f);
  VoiceAction a = pass(TRUE);
  switch (keyval) {
  case IBUS_KEY_Return:
  case IBUS_KEY_KP_Enter:
    return finish_review(f, TRUE, !(modifiers & IBUS_SHIFT_MASK));
  case IBUS_KEY_Escape:
    return finish_review(f, FALSE, FALSE);
  case IBUS_KEY_Left:
    if (f->cursor > 0) f->cursor--;
    break;
  case IBUS_KEY_Right:
    if (f->cursor < len) f->cursor++;
    break;
  case IBUS_KEY_Home:
    f->cursor = 0;
    break;
  case IBUS_KEY_End:
    f->cursor = len;
    break;
  case IBUS_KEY_BackSpace:
    if (f->cursor > 0) review_erase(f, --f->cursor);
    break;
  case IBUS_KEY_Delete:
    if (f->cursor < len) review_erase(f, f->cursor);
    break;
  default:
    return pass(FALSE); /* let Rime have it; the glue calls voice_fsm_unhandled if it declines */
  }
  a.aux = render_review(f);
  return a;
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
    if (hot && release && f->armed) return start_recording(f, FALSE);
    if (!release) f->armed = FALSE;
    return pass(FALSE);

  case VOICE_RECORDING:
    if (keyval == IBUS_KEY_Escape && !release)
      return leave_session(f, VOICE_CMD_CANCEL, NULL, TRUE);
    if (hot && !release) {
      f->armed = bare(modifiers);
      return pass(TRUE);
    }
    if (hot && release && f->armed) {
      f->armed = FALSE;
      f->state = VOICE_FINALIZING;
      a = pass(TRUE);
      a.send = VOICE_CMD_STOP;
      a.aux = f->partial->len ? set_aux(f, VOICE_AUX_PARTIAL_FINALIZING, f->partial->str)
                              : set_aux(f, VOICE_AUX_FINALIZING, NULL);
      return a;
    }
    if (!release) f->armed = FALSE;
    return pass(TRUE);

  case VOICE_FINALIZING:
    if (keyval == IBUS_KEY_Escape && !release)
      return leave_session(f, VOICE_CMD_CANCEL, NULL, TRUE);
    if (!hot) {
      VoiceKey k = {keyval, keycode, modifiers};
      g_array_append_val(f->queued, k);
    }
    return pass(TRUE);

  case VOICE_REVIEW:
    return review_key(f, keyval, modifiers, rime_composing);
  }
  return pass(FALSE);
}

VoiceAction voice_fsm_unhandled(VoiceFsm *f, guint keyval, guint modifiers) {
  if (f->state != VOICE_REVIEW) return pass(FALSE);
  VoiceAction a = pass(TRUE);
  if (modifiers & (IBUS_RELEASE_MASK | COMMAND_MODS)) return a;
  gunichar ch = ibus_keyval_to_unicode(keyval);
  if (ch && g_unichar_isprint(ch)) {
    char buf[7] = {0};
    g_unichar_to_utf8(ch, buf);
    review_insert(f, buf);
    a.aux = render_review(f);
  }
  return a;
}

VoiceAction voice_fsm_insert(VoiceFsm *f, const char *text) {
  if (f->state != VOICE_REVIEW) return pass(FALSE);
  VoiceAction a = pass(TRUE);
  review_insert(f, text);
  a.aux = render_review(f);
  return a;
}

VoiceAction voice_fsm_event(VoiceFsm *f, VoiceEvType type, const char *payload) {
  if (f->state != VOICE_RECORDING && f->state != VOICE_FINALIZING) return pass(FALSE);
  VoiceAction a = pass(FALSE);
  switch (type) {
  case VOICE_EV_PARTIAL:
    g_string_assign(f->partial, payload);
    a.aux = set_aux(f, f->state == VOICE_RECORDING ? VOICE_AUX_PARTIAL_RECORDING
                                                   : VOICE_AUX_PARTIAL_FINALIZING,
                    payload);
    return a;
  case VOICE_EV_FINAL:
    if (!*payload) return leave_session(f, VOICE_CMD_NONE, NULL, FALSE);
    if (f->review_enabled || f->from_review) {
      if (!f->from_review) review_reset(f);
      review_insert(f, payload);
      f->from_review = TRUE; /* leave_session lands in REVIEW */
      return leave_session(f, VOICE_CMD_NONE, NULL, FALSE);
    }
    g_string_assign(f->commit_buf, payload);
    a = leave_session(f, VOICE_CMD_NONE, NULL, FALSE);
    a.commit = f->commit_buf->str;
    return a;
  case VOICE_EV_ERR:
    g_string_assign(f->notify_buf, VOICE_NOTIFY_ERROR_PREFIX);
    g_string_append(f->notify_buf, payload);
    return leave_session(f, VOICE_CMD_NONE, f->notify_buf->str, FALSE);
  default:
    return a;
  }
}

VoiceAction voice_fsm_disconnected(VoiceFsm *f) {
  if (f->state != VOICE_RECORDING && f->state != VOICE_FINALIZING) return pass(FALSE);
  return leave_session(f, VOICE_CMD_NONE, VOICE_NOTIFY_DISCONNECTED, FALSE);
}

VoiceAction voice_fsm_focus_out(VoiceFsm *f) {
  if (f->state != VOICE_RECORDING && f->state != VOICE_FINALIZING) return pass(FALSE);
  return leave_session(f, VOICE_CMD_CANCEL, VOICE_NOTIFY_FOCUS_LOST, TRUE);
}

VoiceAction voice_fsm_focus_in(VoiceFsm *f) {
  VoiceAction a = pass(FALSE);
  if (f->state != VOICE_IDLE) a.aux = f->aux->str;
  return a;
}

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out) {
  *n_out = f->queued->len;
  return (const VoiceKey *)f->queued->data;
}

void voice_fsm_drop_queue(VoiceFsm *f) {
  g_array_set_size(f->queued, 0);
}
