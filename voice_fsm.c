#include <string.h>
#include <ibus.h>
#include "voice_fsm.h"

#define IGNORED_MODS (IBUS_LOCK_MASK | IBUS_MOD2_MASK)
#define COMMAND_MODS (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_MOD4_MASK)

static VoiceAction pass(gboolean consume) {
  VoiceAction a = {.consume = consume, .send = VOICE_CMD_NONE};
  return a;
}

void voice_fsm_init(VoiceFsm *f, guint hotkey_keyval, gboolean review_enabled) {
  f->state = VOICE_IDLE;
  f->hotkey = hotkey_keyval;
  f->review_enabled = review_enabled;
  f->armed = FALSE;
  f->from_review = FALSE;
  f->then = VOICE_THEN_REVIEW;
  f->drop_release = 0;
  f->aux = g_string_new("");
  f->partial = g_string_new("");
  f->review = g_string_new("");
  f->cursor = 0;
  f->cursor_up = FALSE;
  f->hotkey_down = FALSE;
  f->shift_r_down = FALSE;
  f->return_pending = FALSE;
  f->swallow_shift = FALSE;
  f->chord_dropped = FALSE;
  f->settings_row = 0;
  f->settings_back = VOICE_IDLE;
  f->note = g_ptr_array_new_with_free_func(g_free);
  f->note_labels = g_ptr_array_new();  /* borrowed static strings */
  f->enter_sends = FALSE;
  f->cursor_mark = VOICE_REVIEW_CURSOR;
  f->blink_off = FALSE;
  f->note_cursor = -1;
  f->compose_hint = g_string_new("");
  f->commit_buf = g_string_new("");
  f->notify_buf = g_string_new("");
  f->queued = g_array_new(FALSE, FALSE, sizeof(VoiceKey));
}

void voice_fsm_clear(VoiceFsm *f) {
  g_string_free(f->aux, TRUE);
  g_string_free(f->partial, TRUE);
  g_string_free(f->review, TRUE);
  g_ptr_array_unref(f->note);
  g_ptr_array_unref(f->note_labels);
  g_string_free(f->compose_hint, TRUE);
  g_string_free(f->commit_buf, TRUE);
  g_string_free(f->notify_buf, TRUE);
  g_array_free(f->queued, TRUE);
}

static const char *set_aux(VoiceFsm *f, const char *a, const char *b) {
  g_string_assign(f->aux, a);
  if (b) g_string_append(f->aux, b);
  return f->aux->str;
}

static gsize byte_at(VoiceFsm *f, glong chars) {
  return g_utf8_offset_to_pointer(f->review->str, chars) - f->review->str;
}

static guint review_len(VoiceFsm *f) {
  return g_utf8_strlen(f->review->str, -1);
}

static gboolean is_closing_punct(gunichar c) {
  return g_utf8_strchr("，。、；：？！）》」』”’…,.;:?!)]}%", -1, c) != NULL;
}

static gboolean is_ascii_word(gunichar c) {
  return c < 128 && g_ascii_isalnum((gchar)c);
}

static guint cols_of(gunichar c) {
  return g_unichar_iswide_cjk(c) ? 2 : 1;
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

const char *voice_cursor_mark(const char *style) {
  if (style && !strcmp(style, "block")) return "▮";
  if (style && !strcmp(style, "caret")) return "˰";
  if (style && !strcmp(style, "bar")) return "|";
  if (style && !strcmp(style, "triangle")) return "▲";
  return VOICE_REVIEW_CURSOR;
}

const char *voice_cursor_blank(const char *mark) {
  if (!strcmp(mark, "▮")) return "\xE2\x80\x84 ";                /* three-per-em + space */
  if (!strcmp(mark, "˰")) return "\xE2\x80\x86\xE2\x80\x85";  /* six-per-em + four-per-em */
  if (!strcmp(mark, "|")) return "\xE2\x80\x88";                 /* punctuation space */
  if (!strcmp(mark, "▲")) return "\xE3\x80\x80";                 /* ideographic space */
  return "\xE2\x80\x88\xE2\x80\x87";                          /* ❚: punctuation + figure */
}

static const char *hotkey_label(guint hotkey) {
  switch (hotkey) {
  case IBUS_KEY_Alt_R: return "右Alt";
  case IBUS_KEY_Control_R: return "右Ctrl";
  case IBUS_KEY_Shift_R: return "右Shift";
  default: {
    const char *name = ibus_keyval_name(hotkey);
    return name ? name : "热键";
  }
  }
}

static const char *enter_hint(gboolean enter_sends) {
  return enter_sends ? "⏎ 发送 · 右⇧⏎ 上屏" : "⏎ 上屏 · 右⇧⏎ 发送";
}

void voice_review_header(guint hotkey, gboolean enter_sends, GString *out) {
  g_string_append_printf(out, VOICE_REVIEW_PREFIX "语音草稿 · %s · Esc 放弃 · %s 续说\n"
                         VOICE_REVIEW_RULE, enter_hint(enter_sends), hotkey_label(hotkey));
}

/* Rows of the plain review text and the character offset each row starts at. The cursor
 * marker takes no wrap width, so rows stay put while the cursor moves. */
static GPtrArray *review_rows(VoiceFsm *f, GArray *starts) {
  GPtrArray *rows = voice_wrap(f->review->str, VOICE_REVIEW_COLS);
  guint at = 0;
  for (guint i = 0; i < rows->len; ++i) {
    g_array_append_val(starts, at);
    at += g_utf8_strlen(rows->pdata[i], -1);
  }
  return rows;
}

static guint row_of(GArray *starts, guint cursor) {
  guint r = 0;
  while (r + 1 < starts->len && g_array_index(starts, guint, r + 1) <= cursor) ++r;
  return r;
}

/* Row the cursor is drawn in: at a wrap point, the upper row when cursor_up is set. */
static guint cursor_row(VoiceFsm *f, GArray *starts) {
  guint r = row_of(starts, f->cursor);
  if (f->cursor_up && r > 0 && g_array_index(starts, guint, r) == f->cursor) --r;
  return r;
}

static void clear_note(VoiceFsm *f) {
  g_ptr_array_set_size(f->note, 0);
  g_ptr_array_set_size(f->note_labels, 0);
  f->note_cursor = -1;
  g_string_truncate(f->compose_hint, 0);
}

/* Lays out the note rows and returns the header for the aux text. */
static const char *layout_review(VoiceFsm *f) {
  GArray *starts = g_array_new(FALSE, FALSE, sizeof(guint));
  GPtrArray *rows = review_rows(f, starts);
  guint cur = cursor_row(f, starts);
  gsize at = g_utf8_offset_to_pointer(rows->pdata[cur], f->cursor - g_array_index(starts, guint, cur)) -
             (const char *)rows->pdata[cur];
  /* the compose hint always shows the mark; the note row follows the blink phase */
  GString *hint = g_string_new(rows->pdata[cur]);
  g_string_insert(hint, at, f->cursor_mark);
  GString *row = g_string_new(rows->pdata[cur]);
  g_string_insert(row, at, f->blink_off ? voice_cursor_blank(f->cursor_mark) : f->cursor_mark);
  g_free(rows->pdata[cur]);
  rows->pdata[cur] = g_string_free(row, FALSE);

  guint first = 0, last = rows->len;
  if (rows->len > VOICE_REVIEW_MAX_LINES) {
    gint s = (gint)cur - VOICE_REVIEW_MAX_LINES / 2;
    first = (guint)CLAMP(s, 0, (gint)(rows->len - VOICE_REVIEW_MAX_LINES));
    last = first + VOICE_REVIEW_MAX_LINES;
  }
  clear_note(f);
  g_string_append(f->compose_hint, VOICE_COMPOSE_PREFIX);
  g_string_append(f->compose_hint, hint->str);
  g_string_free(hint, TRUE);
  guint n = rows->len, v = last - first;
  guint thumb_len = 0, thumb_start = 0;
  if (n > v) {
    thumb_len = MAX(1, (guint)(((gdouble)v * v / n) + 0.5));
    thumb_start = (guint)((gdouble)first * (v - thumb_len) / (n - v) + 0.5);
  }
  for (guint i = first; i < last; ++i) {
    guint k = i - first;
    if (i == cur) f->note_cursor = f->note->len;
    g_ptr_array_add(f->note, g_strdup(rows->pdata[i]));
    g_ptr_array_add(f->note_labels,
                    (gpointer)(n <= v ? VOICE_SCROLL_NONE
                               : (k >= thumb_start && k < thumb_start + thumb_len)
                                   ? VOICE_SCROLL_THUMB : VOICE_SCROLL_TRACK));
  }

  g_ptr_array_unref(rows);
  g_array_free(starts, TRUE);
  g_string_truncate(f->aux, 0);
  voice_review_header(f->hotkey, f->enter_sends, f->aux);
  return f->aux->str;
}

/* Redraw after a change: the mark shows again whatever the blink phase. */
static const char *render_review(VoiceFsm *f) {
  f->blink_off = FALSE;
  return layout_review(f);
}

VoiceAction voice_fsm_blink(VoiceFsm *f) {
  VoiceAction a = pass(FALSE);
  if (f->state != VOICE_REVIEW) return a;
  f->blink_off = !f->blink_off;
  layout_review(f);
  a.note = TRUE;
  return a;
}

/* Up/Down: same character offset in the neighbouring row; at or past its end, its end. */
static void move_row(VoiceFsm *f, gint delta) {
  GArray *starts = g_array_new(FALSE, FALSE, sizeof(guint));
  GPtrArray *rows = review_rows(f, starts);
  gint cur = cursor_row(f, starts), to = cur + delta;
  if (to >= 0 && to < (gint)rows->len) {
    guint offset = f->cursor - g_array_index(starts, guint, cur);
    guint len = g_utf8_strlen(rows->pdata[to], -1);
    f->cursor = g_array_index(starts, guint, to) + MIN(offset, len);
    f->cursor_up = offset >= len;
  }
  g_ptr_array_unref(rows);
  g_array_free(starts, TRUE);
}

/* Ctrl+Left/Right: chunks are separated by punctuation and spaces, which suits both Chinese
 * (no spaces, clauses between punctuation) and English words. */
static gboolean is_chunk_sep(VoiceFsm *f, guint i) {
  gunichar c = g_utf8_get_char(f->review->str + byte_at(f, i));
  return g_unichar_isspace(c) || g_unichar_ispunct(c);
}

static void jump_chunk(VoiceFsm *f, gboolean forward) {
  guint len = review_len(f), p = f->cursor;
  if (forward) {
    while (p < len && !is_chunk_sep(f, p)) ++p;
    while (p < len && is_chunk_sep(f, p)) ++p;
  } else {
    while (p > 0 && is_chunk_sep(f, p - 1)) --p;
    while (p > 0 && !is_chunk_sep(f, p - 1)) --p;
  }
  f->cursor = p;
}

static void review_insert(VoiceFsm *f, const char *text) {
  g_string_insert(f->review, byte_at(f, f->cursor), text);
  f->cursor += g_utf8_strlen(text, -1);
  f->cursor_up = TRUE;
}

static void review_erase(VoiceFsm *f, guint from) { /* one character at `from` */
  gsize a = byte_at(f, from);
  g_string_erase(f->review, a, byte_at(f, from + 1) - a);
}

static void review_reset(VoiceFsm *f) {
  g_string_truncate(f->review, 0);
  f->cursor = 0;
}

/* Keys the compositor has already applied by the time we see them: replaying one later through
 * forward_key_event (a virtual keyboard) would apply it a second time, e.g. toggle Caps Lock
 * back against its LED or leave a Shift held. Queued keys carry the modifier mask anyway. */
static gboolean modifier_or_lock(guint keyval) {
  switch (keyval) {
  case IBUS_KEY_Shift_L: case IBUS_KEY_Shift_R: case IBUS_KEY_Shift_Lock:
  case IBUS_KEY_Control_L: case IBUS_KEY_Control_R:
  case IBUS_KEY_Alt_L: case IBUS_KEY_Alt_R: case IBUS_KEY_Meta_L: case IBUS_KEY_Meta_R:
  case IBUS_KEY_Super_L: case IBUS_KEY_Super_R: case IBUS_KEY_Hyper_L: case IBUS_KEY_Hyper_R:
  case IBUS_KEY_Caps_Lock: case IBUS_KEY_Num_Lock: case IBUS_KEY_Scroll_Lock:
  case IBUS_KEY_ISO_Level3_Shift: case IBUS_KEY_ISO_Level5_Shift: case IBUS_KEY_Mode_switch:
  case IBUS_KEY_ISO_Next_Group: case IBUS_KEY_ISO_Prev_Group:
    return TRUE;
  default:
    return FALSE;
  }
}

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
  f->then = VOICE_THEN_REVIEW;
  g_string_printf(f->aux, VOICE_AUX_RECORDING_FMT, hotkey_label(f->hotkey), enter_hint(f->enter_sends),
                  hotkey_label(f->hotkey));
  a.aux = f->aux->str;
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
    a.note = TRUE;
  } else {
    f->state = VOICE_IDLE;
    clear_note(f);
    a.aux = set_aux(f, "", NULL);
  }
  f->from_review = FALSE;
  f->then = VOICE_THEN_REVIEW;
  a.send = send;
  a.notify = notify;
  a.replay = f->queued->len > 0;
  return a;
}

/* Enter means "commit and send": right Shift+Enter by default, plain (or left Shift+) Enter
 * when enter_sends. */
static gboolean enter_sends(VoiceFsm *f, guint modifiers) {
  gboolean alt = (modifiers & IBUS_SHIFT_MASK) && f->shift_r_down;
  return f->enter_sends ? !alt : alt;
}

static VoiceAction stop_recording(VoiceFsm *f) {
  VoiceAction a = pass(TRUE);
  f->armed = FALSE;
  f->state = VOICE_FINALIZING;
  a.send = VOICE_CMD_STOP;
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
  clear_note(f);
  f->state = VOICE_IDLE;
  a.aux = set_aux(f, "", NULL);
  return a;
}

/* ---- settings panel ---- */

static const char *const CURSOR_STYLES[] = {"heavy", "block", "caret", "bar", "triangle"};
static const char *const CURSOR_NAMES[] = {"❚ 粗竖线", "▮ 方块", "˰ 下角标", "| 细竖线", "▲ 三角"};
static const char *const HOTKEY_NAMES[] = {"Alt_R", "Control_R", "Menu"};
static const guint HOTKEY_VALS[] = {IBUS_KEY_Alt_R, IBUS_KEY_Control_R, IBUS_KEY_Menu};

static guint cursor_style_index(VoiceFsm *f) {
  for (guint i = 0; i < G_N_ELEMENTS(CURSOR_STYLES); ++i)
    if (!strcmp(voice_cursor_mark(CURSOR_STYLES[i]), f->cursor_mark)) return i;
  return 0;
}

static gint hotkey_index(VoiceFsm *f) {
  for (guint i = 0; i < G_N_ELEMENTS(HOTKEY_VALS); ++i)
    if (HOTKEY_VALS[i] == f->hotkey) return i;
  return -1;
}

static const char *render_settings(VoiceFsm *f) {
  clear_note(f);
  for (guint i = 0; i < VOICE_SETTINGS_ROWS; ++i) {
    const char *name = "", *value = "";
    switch (i) {
    case 0: name = "发送方式"; value = enter_hint(f->enter_sends); break;
    case 1: name = "光标样式"; value = CURSOR_NAMES[cursor_style_index(f)]; break;
    case 2: name = "审阅便签"; value = f->review_enabled ? "开 · 识别结果先进便签" : "关 · 识别结果直接上屏"; break;
    case 3: name = "录音热键"; value = hotkey_label(f->hotkey); break;
    }
    g_ptr_array_add(f->note, i == f->settings_row ? g_strdup_printf("%s　◂ %s ▸", name, value)
                                                  : g_strdup_printf("%s　  %s", name, value));
    g_ptr_array_add(f->note_labels, (gpointer)VOICE_SCROLL_NONE);
  }
  g_ptr_array_add(f->note, g_strdup(VOICE_SETTINGS_FOOTER));
  g_ptr_array_add(f->note_labels, (gpointer)VOICE_SCROLL_NONE);
  f->note_cursor = f->settings_row;
  return set_aux(f, VOICE_SETTINGS_HEADER, NULL);
}

static VoiceAction open_settings(VoiceFsm *f, VoiceState back) {
  VoiceAction a = pass(TRUE);
  f->armed = FALSE;
  f->settings_back = back;
  f->settings_row = 0;
  f->state = VOICE_SETTINGS;
  a.aux = render_settings(f);
  a.note = TRUE;
  return a;
}

/* Back to IDLE or REVIEW; the returned action redraws what is there. */
static VoiceAction close_settings(VoiceFsm *f) {
  VoiceAction a = pass(TRUE);
  f->state = f->settings_back;
  if (f->state == VOICE_REVIEW) {
    a.aux = render_review(f);
    a.note = TRUE;
  } else {
    clear_note(f);
    a.aux = set_aux(f, "", NULL);
  }
  return a;
}

/* Step the selected option by dir (+1/-1); fills in what changed for the glue. */
static void change_setting(VoiceFsm *f, gint dir, VoiceAction *a) {
  switch (f->settings_row) {
  case 0:
    f->enter_sends = !f->enter_sends;
    a->setting_key = "enter_sends";
    a->setting_value = f->enter_sends ? "true" : "false";
    break;
  case 1: {
    guint n = G_N_ELEMENTS(CURSOR_STYLES), i = (cursor_style_index(f) + n + dir) % n;
    f->cursor_mark = voice_cursor_mark(CURSOR_STYLES[i]);
    a->setting_key = "cursor";
    a->setting_value = CURSOR_STYLES[i];
    break;
  }
  case 2:
    f->review_enabled = !f->review_enabled;
    a->setting_key = "review";
    a->setting_value = f->review_enabled ? "true" : "false";
    break;
  case 3: {
    gint n = G_N_ELEMENTS(HOTKEY_VALS), i = hotkey_index(f);
    i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + n + dir) % n;
    f->hotkey = HOTKEY_VALS[i];
    f->hotkey_down = FALSE; /* the old key's release will no longer match */
    f->armed = FALSE;
    a->setting_key = "hotkey";
    a->setting_value = HOTKEY_NAMES[i];
    break;
  }
  }
}

/* Modifier the hotkey itself sets while held; Shift hotkeys get no chord (Shift+I is a capital I). */
static gboolean hotkey_mask(guint hotkey, guint *mask) {
  switch (hotkey) {
  case IBUS_KEY_Shift_L: case IBUS_KEY_Shift_R: return FALSE;
  case IBUS_KEY_Alt_L: case IBUS_KEY_Alt_R: *mask = IBUS_MOD1_MASK; return TRUE;
  case IBUS_KEY_Control_L: case IBUS_KEY_Control_R: *mask = IBUS_CONTROL_MASK; return TRUE;
  case IBUS_KEY_Super_L: case IBUS_KEY_Super_R: *mask = IBUS_SUPER_MASK | IBUS_MOD4_MASK; return TRUE;
  default: *mask = 0; return TRUE;
  }
}

/* hotkey + I, and nothing else held: the hotkey's own modifier must be down (so a stale
 * hotkey_down cannot fire) and no other one (Shift/Ctrl+Alt+I belong to the application). */
static gboolean is_settings_chord(VoiceFsm *f, guint keyval, guint modifiers) {
  guint mask = 0;
  if ((modifiers & IBUS_RELEASE_MASK) || !f->hotkey_down || !hotkey_mask(f->hotkey, &mask)) return FALSE;
  if (keyval != IBUS_KEY_i && keyval != IBUS_KEY_I) return FALSE;
  return (modifiers & ~IGNORED_MODS) == mask;
}

static VoiceAction settings_key(VoiceFsm *f, guint keyval, guint modifiers) {
  gboolean release = (modifiers & IBUS_RELEASE_MASK) != 0;
  VoiceAction a = pass(TRUE);
  if (release) return a;
  if (is_settings_chord(f, keyval, modifiers)) return close_settings(f);
  switch (keyval) {
  case IBUS_KEY_Escape:
  case IBUS_KEY_Return:
  case IBUS_KEY_KP_Enter:
    return close_settings(f);
  case IBUS_KEY_Up:
    if (f->settings_row > 0) f->settings_row--;
    break;
  case IBUS_KEY_Down:
    if (f->settings_row + 1 < VOICE_SETTINGS_ROWS) f->settings_row++;
    break;
  case IBUS_KEY_Left:
    change_setting(f, -1, &a);
    break;
  case IBUS_KEY_Right:
  case IBUS_KEY_space:
    change_setting(f, 1, &a);
    break;
  default:
    return a;
  }
  a.aux = render_settings(f);
  a.note = TRUE;
  return a;
}

static VoiceAction review_key(VoiceFsm *f, guint keyval, guint modifiers, gboolean composing) {
  gboolean release = (modifiers & IBUS_RELEASE_MASK) != 0;
  if (composing) {
    f->armed = FALSE;
    return pass(FALSE);
  }
  if (is_settings_chord(f, keyval, modifiers)) return open_settings(f, VOICE_REVIEW);
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
  case IBUS_KEY_KP_Enter: {
    gboolean shift = (modifiers & IBUS_SHIFT_MASK) != 0;
    gboolean send = enter_sends(f, modifiers);
    a = finish_review(f, TRUE, send && !shift);
    f->return_pending = send && shift && a.commit; /* sent on the Shift release */
    f->swallow_shift = shift;
    return a;
  }
  case IBUS_KEY_Escape:
    return finish_review(f, FALSE, FALSE);
  case IBUS_KEY_Left:
    if (modifiers & IBUS_CONTROL_MASK) jump_chunk(f, FALSE);
    else if (f->cursor > 0) f->cursor--;
    f->cursor_up = FALSE;
    break;
  case IBUS_KEY_Right:
    if (modifiers & IBUS_CONTROL_MASK) {
      jump_chunk(f, TRUE);
      f->cursor_up = FALSE; /* a chunk start belongs before the word on the lower row */
    } else {
      if (f->cursor < len) f->cursor++;
      f->cursor_up = TRUE;
    }
    break;
  case IBUS_KEY_Up:
    move_row(f, -1);
    break;
  case IBUS_KEY_Down:
    move_row(f, 1);
    break;
  case IBUS_KEY_Home:
    f->cursor = 0;
    f->cursor_up = FALSE;
    break;
  case IBUS_KEY_End:
    f->cursor = len;
    f->cursor_up = TRUE;
    break;
  case IBUS_KEY_BackSpace:
    if (f->cursor > 0) review_erase(f, --f->cursor);
    f->cursor_up = TRUE;
    break;
  case IBUS_KEY_Delete:
    if (f->cursor < len) review_erase(f, f->cursor);
    break;
  default:
    return pass(FALSE); /* let Rime have it; the glue calls voice_fsm_unhandled if it declines */
  }
  a.aux = render_review(f);
  a.note = TRUE;
  return a;
}

VoiceAction voice_fsm_key(VoiceFsm *f, guint keyval, guint keycode, guint modifiers,
                          gboolean rime_composing) {
  gboolean release = (modifiers & IBUS_RELEASE_MASK) != 0;
  gboolean hot = keyval == f->hotkey;
  VoiceAction a;

  if (hot) f->hotkey_down = !release;
  if (keyval == IBUS_KEY_Shift_R) f->shift_r_down = !release;
  if (release && f->drop_release && keyval == f->drop_release) {
    f->drop_release = 0;
    return pass(TRUE);
  }
  if (f->swallow_shift) {
    gboolean shift = keyval == IBUS_KEY_Shift_L || keyval == IBUS_KEY_Shift_R;
    if (shift && release) {
      a = pass(TRUE); /* Rime saw the Shift press but not the Enter: it would take a Shift tap */
      a.send_return = f->return_pending;
      f->swallow_shift = f->return_pending = FALSE;
      return a;
    }
    if (!release && !shift) f->swallow_shift = f->return_pending = FALSE;
  }

  switch (f->state) {
  case VOICE_IDLE:
    if (rime_composing) {
      f->armed = FALSE;
      return pass(FALSE);
    }
    if (is_settings_chord(f, keyval, modifiers)) return open_settings(f, VOICE_IDLE);
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
    if (is_settings_chord(f, keyval, modifiers)) {
      leave_session(f, VOICE_CMD_NONE, NULL, TRUE);
      a = open_settings(f, f->state);
      a.send = VOICE_CMD_CANCEL;
      return a;
    }
    if (hot && !release) {
      f->armed = bare(modifiers);
      return pass(TRUE);
    }
    if (hot && release && f->armed) {
      a = stop_recording(f);
      a.aux = f->partial->len ? set_aux(f, VOICE_AUX_PARTIAL_FINALIZING, f->partial->str)
                              : set_aux(f, VOICE_AUX_FINALIZING, NULL);
      return a;
    }
    if ((keyval == IBUS_KEY_Return || keyval == IBUS_KEY_KP_Enter) && !release) {
      gboolean send = enter_sends(f, modifiers);
      a = stop_recording(f);
      f->then = send ? VOICE_THEN_SEND : VOICE_THEN_COMMIT;
      f->drop_release = keyval;
      f->swallow_shift = (modifiers & IBUS_SHIFT_MASK) != 0; /* see the Shift release above */
      f->return_pending = FALSE;
      a.aux = set_aux(f, send ? VOICE_AUX_FINALIZING_SEND : VOICE_AUX_FINALIZING_COMMIT, NULL);
      return a;
    }
    if (!release) f->armed = FALSE;
    return pass(TRUE);

  case VOICE_FINALIZING:
    if (keyval == IBUS_KEY_Escape && !release)
      return leave_session(f, VOICE_CMD_CANCEL, NULL, TRUE);
    if (keyval == IBUS_KEY_i || keyval == IBUS_KEY_I) {
      if (is_settings_chord(f, keyval, modifiers)) f->chord_dropped = TRUE;
      if (f->chord_dropped) { /* not replayed: neither the panel nor Alt+I later */
        if (release) f->chord_dropped = FALSE;
        return pass(TRUE);
      }
    }
    if (!hot && !modifier_or_lock(keyval)) {
      VoiceKey k = {keyval, keycode, modifiers};
      g_array_append_val(f->queued, k);
    }
    return pass(TRUE);

  case VOICE_REVIEW:
    return review_key(f, keyval, modifiers, rime_composing);

  case VOICE_SETTINGS:
    return settings_key(f, keyval, modifiers);
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
    a.note = TRUE;
  }
  return a;
}

VoiceAction voice_fsm_insert(VoiceFsm *f, const char *text) {
  if (f->state != VOICE_REVIEW) return pass(FALSE);
  VoiceAction a = pass(TRUE);
  review_insert(f, text);
  a.aux = render_review(f);
  a.note = TRUE;
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
    if (f->then != VOICE_THEN_REVIEW) { /* stopped by Enter: commit the would-be note */
      gboolean send = f->then == VOICE_THEN_SEND;
      gboolean held = f->swallow_shift; /* that Enter's Shift is still down */
      if (!f->from_review) review_reset(f);
      review_insert(f, payload);
      leave_session(f, VOICE_CMD_NONE, NULL, FALSE);
      a = finish_review(f, TRUE, send && !held);
      f->return_pending = send && held && a.commit;
      a.replay = f->queued->len > 0;
      return a;
    }
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
  f->armed = FALSE;
  f->hotkey_down = FALSE;
  f->shift_r_down = FALSE;
  f->return_pending = FALSE;
  f->swallow_shift = FALSE;
  f->drop_release = 0;
  if (f->state == VOICE_SETTINGS) { /* close it; a review behind it stays */
    VoiceAction a = close_settings(f);
    a.consume = FALSE;
    a.note = FALSE;
    a.aux = f->state == VOICE_IDLE ? a.aux : NULL;
    return a;
  }
  if (f->state != VOICE_RECORDING && f->state != VOICE_FINALIZING) return pass(FALSE);
  return leave_session(f, VOICE_CMD_CANCEL, VOICE_NOTIFY_FOCUS_LOST, TRUE);
}

VoiceAction voice_fsm_focus_in(VoiceFsm *f) {
  f->armed = FALSE;
  if (f->state == VOICE_REVIEW && f->blink_off) render_review(f); /* come back with the mark shown */
  f->hotkey_down = FALSE;
  f->shift_r_down = FALSE;
  VoiceAction a = pass(FALSE);
  if (f->state != VOICE_IDLE) a.aux = f->aux->str;
  a.note = f->state == VOICE_REVIEW;
  return a;
}

GPtrArray *voice_fsm_note(VoiceFsm *f, gint *cursor_row) {
  *cursor_row = f->note_cursor;
  return f->note;
}

GPtrArray *voice_fsm_note_labels(VoiceFsm *f) {
  return f->note_labels;
}

const char *voice_fsm_compose_hint(VoiceFsm *f) {
  return f->compose_hint->str;
}

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out) {
  *n_out = f->queued->len;
  return (const VoiceKey *)f->queued->data;
}

void voice_fsm_drop_queue(VoiceFsm *f) {
  g_array_set_size(f->queued, 0);
}
