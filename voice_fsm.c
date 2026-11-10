#include <string.h>
#include <ibus.h>
#include "voice_fsm.h"

#define IGNORED_MODS (IBUS_LOCK_MASK | IBUS_MOD2_MASK)
#define COMMAND_MODS (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_MOD4_MASK)

static VoiceAction pass(gboolean consume) {
  VoiceAction a = {consume, VOICE_CMD_NONE, NULL, NULL, NULL, FALSE};
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

/* ---- review note layout ---- */

static gboolean is_closing_punct(gunichar c) {
  return g_utf8_strchr("，。、；：？！）》」』”’…,.;:?!)]}%", -1, c) != NULL;
}

static gboolean is_ascii_word(gunichar c) {
  return c < 128 && g_ascii_isalnum((gchar)c);
}

static guint cols_of(gunichar c) {
  return g_unichar_iswide(c) ? 2 : 1;
}

GPtrArray *voice_wrap(const char *text, guint cols) {
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  GString *cur = g_string_new("");
  guint cur_w = 0;
  gsize run_start = 0; /* byte offset in cur of the trailing ASCII word */
  guint run_w = 0;
  for (const char *p = text; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    guint w = cols_of(c);
    if (cur->len && cur_w + w > cols && !is_closing_punct(c)) {
      if (is_ascii_word(c) && run_w && run_start > 0 && run_w < cols / 2) {
        /* move the unfinished ASCII word to the next line */
        g_ptr_array_add(lines, g_strndup(cur->str, run_start));
        g_string_erase(cur, 0, run_start);
        cur_w = run_w;
        run_start = 0;
      } else {
        g_ptr_array_add(lines, g_strdup(cur->str));
        g_string_truncate(cur, 0);
        cur_w = 0;
        run_w = 0;
        run_start = 0;
      }
    }
    if (is_ascii_word(c)) {
      if (!run_w) run_start = cur->len;
      run_w += w;
    } else {
      run_w = 0;
    }
    g_string_append_unichar(cur, c);
    cur_w += w;
  }
  g_ptr_array_add(lines, g_string_free(cur, FALSE));
  return lines;
}

void voice_review_header(guint hotkey, GString *out) {
  const char *label;
  switch (hotkey) {
  case IBUS_KEY_Alt_R: label = "右Alt"; break;
  case IBUS_KEY_Control_R: label = "右Ctrl"; break;
  case IBUS_KEY_Shift_R: label = "右Shift"; break;
  default: label = ibus_keyval_name(hotkey); break;
  }
  g_string_append_printf(out, VOICE_REVIEW_PREFIX "语音输入 · ⏎ 上屏 · Esc 放弃 · %s 续说",
                         label ? label : "热键");
}

static const char *render_review(VoiceFsm *f) {
  GString *body = g_string_new(f->review->str);
  g_string_insert(body, byte_at(f, f->cursor), VOICE_REVIEW_CURSOR);
  GPtrArray *lines = voice_wrap(body->str, VOICE_REVIEW_COLS);
  guint cursor_line = 0;
  for (guint i = 0; i < lines->len; ++i) {
    if (strstr(lines->pdata[i], VOICE_REVIEW_CURSOR)) cursor_line = i;
  }
  guint first = 0, last = lines->len;
  if (lines->len > VOICE_REVIEW_MAX_LINES) {
    gint s = (gint)cursor_line - VOICE_REVIEW_MAX_LINES / 2;
    first = (guint)CLAMP(s, 0, (gint)(lines->len - VOICE_REVIEW_MAX_LINES));
    last = first + VOICE_REVIEW_MAX_LINES;
  }
  g_string_truncate(f->aux, 0);
  voice_review_header(f->hotkey, f->aux);
  if (first > 0) g_string_append(f->aux, "\n" VOICE_REVIEW_MORE);
  for (guint i = first; i < last; ++i) {
    g_string_append_c(f->aux, '\n');
    g_string_append(f->aux, lines->pdata[i]);
  }
  if (last < lines->len) g_string_append(f->aux, "\n" VOICE_REVIEW_MORE);
  g_ptr_array_unref(lines);
  g_string_free(body, TRUE);
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

static VoiceAction finish_review(VoiceFsm *f, gboolean commit) {
  VoiceAction a = pass(TRUE);
  if (commit) {
    g_string_assign(f->commit_buf, f->review->str);
    a.commit = f->commit_buf->len ? f->commit_buf->str : NULL;
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
    return finish_review(f, TRUE);
  case IBUS_KEY_Escape:
    return finish_review(f, FALSE);
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
