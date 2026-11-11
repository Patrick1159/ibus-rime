/* Dictation session state machine. Pure: no IO, no IBus calls; the engine glue
 * applies the returned VoiceAction. */
#ifndef _IBUS_RIME_VOICE_FSM_H_
#define _IBUS_RIME_VOICE_FSM_H_

#include <glib.h>
#include "voice_proto.h"

typedef enum { VOICE_IDLE, VOICE_RECORDING, VOICE_FINALIZING, VOICE_REVIEW, VOICE_SETTINGS } VoiceState;
typedef enum { VOICE_CMD_NONE, VOICE_CMD_START, VOICE_CMD_STOP, VOICE_CMD_CANCEL } VoiceCmd;

typedef struct {
  gboolean consume;     /* TRUE: voice handled the key. FALSE: hand it to Rime; while in REVIEW
                           the glue then calls voice_fsm_unhandled() if Rime declines it */
  VoiceCmd send;        /* command to send to the daemon */
  const char *aux;      /* NULL: leave unchanged; "": hide aux and the note; else show as IBus
                           auxiliary text (status line, or the review header) */
  gboolean note;        /* review note rows changed: redraw them from voice_fsm_note() */
  const char *commit;   /* NULL: nothing to commit (owned by the fsm) */
  const char *notify;   /* NULL: no desktop notification */
  gboolean replay;      /* after commit, replay voice_fsm_queued() keys in order */
  gboolean send_return; /* forward a Return press+release, after the commit if there is one */
  const char *setting_key;   /* settings panel changed voice/<key> to setting_value (static */
  const char *setting_value; /* strings): the glue applies it and saves it */
} VoiceAction;

typedef struct { guint keyval, keycode, modifiers; } VoiceKey;

typedef struct {
  VoiceState state;
  guint hotkey;
  gboolean review_enabled; /* FINAL text goes to REVIEW instead of being committed */
  const char *cursor_mark;  /* review cursor glyph, see voice_cursor_mark(); set by the glue */
  gboolean blink_off;      /* blink phase: draw voice_cursor_blank() instead of the mark */
  gboolean enter_sends;    /* in REVIEW: FALSE (default): Enter commits, right Shift+Enter commits
                              + sends; TRUE: swapped. Left Shift+Enter acts like Enter (left Shift
                              stays Rime's 中/英 key). A send with a Shift held waits for the Shift
                              release, so the application sees a plain Return. Set by the glue. */
  gboolean armed;          /* bare hotkey press seen, waiting for its release */
  gboolean hotkey_down;    /* hotkey held: hotkey + I opens the settings panel */
  gboolean shift_r_down;   /* right Shift held: Enter means "the other one" (send by default) */
  gboolean return_pending; /* Shift+Enter committed; send Return when Shift is released */
  gboolean chord_dropped;  /* hotkey + I seen while FINALIZING: drop it, release included */
  gboolean swallow_shift;  /* Shift+Enter ended a review: keep its Shift release from Rime,
                              which never saw the Enter and would take a lone Shift tap */
  gboolean from_review;    /* current recording was started from REVIEW */
  GString *aux;            /* current auxiliary text ("" when nothing to show) */
  GString *partial;        /* latest PARTIAL payload */
  GString *review;         /* review buffer */
  guint cursor;            /* review cursor, in characters */
  gboolean cursor_up;      /* at a wrap point, draw the cursor at the end of the upper row (after
                              Right, End, typing, Up/Down onto a row end) instead of the start of
                              the lower one (after Left, Home) */
  guint settings_row;      /* selected row of the settings panel */
  VoiceState settings_back; /* IDLE or REVIEW: where closing the settings panel returns */
  GPtrArray *note;         /* visible note rows (wrapped review text, cursor marked) */
  GPtrArray *note_labels;  /* per visible row: scrollbar glyph (or " " when all rows fit) */
  gint note_cursor;        /* index in note of the row holding the cursor, -1 if none */
  GString *compose_hint;   /* aux text while Rime composes inside the review */
  GString *commit_buf;
  GString *notify_buf;
  GArray *queued;          /* VoiceKey typed while FINALIZING */
} VoiceFsm;

#define VOICE_AUX_RECORDING_FMT "🎤 录音中 · %s 结束 · Esc 取消 · %s+I 设置" /* %s: hotkey label */
#define VOICE_AUX_FINALIZING "⏳ 识别中…"
#define VOICE_AUX_PARTIAL_RECORDING "🎤 "
#define VOICE_AUX_PARTIAL_FINALIZING "⏳ "
/* Review note: the header block (key hints, then a rule) goes to the auxiliary text; the
 * review text, wrapped to VOICE_REVIEW_COLS display columns (wide/ambiguous CJK = 2), is shown
 * as rows of the IBus lookup table so the cursor row gets the theme's highlight. At most
 * VOICE_REVIEW_MAX_LINES rows around the cursor are shown. When rows are hidden, each row's
 * label (the only aligned column the panel draws) is a scrollbar segment: VOICE_SCROLL_THUMB
 * for the visible part's share of the whole text, VOICE_SCROLL_TRACK elsewhere. */
#define VOICE_REVIEW_PREFIX "🎙 "
#define VOICE_REVIEW_CURSOR "❚" /* default: heavy bar (DejaVu Sans), not CJK-wide */
#define VOICE_REVIEW_RULE "──────────────────────────────" /* 30 × U+2500 */
#define VOICE_SCROLL_THUMB "┃"
#define VOICE_SCROLL_TRACK "┆"
#define VOICE_SCROLL_NONE " "
#define VOICE_COMPOSE_PREFIX "✎ "
#define VOICE_REVIEW_COLS 60
#define VOICE_REVIEW_MAX_LINES 6
/* Settings panel, opened with hotkey + I: header in the aux text, one row per option (the
 * selected one highlighted) plus a footer row pointing at the command line for the rest. */
#define VOICE_SETTINGS_PREFIX "⚙ "
#define VOICE_SETTINGS_HEADER VOICE_SETTINGS_PREFIX "语音设置 · ↑↓ 选择 · ←→ 修改 · Esc 关闭\n" VOICE_REVIEW_RULE
#define VOICE_SETTINGS_ROWS 4
#define VOICE_SETTINGS_FOOTER "改动立即生效并保存 · 其他：toolbox run voice-input config"
#define VOICE_NOTIFY_FOCUS_LOST "语音输入已取消（焦点切走）"
#define VOICE_NOTIFY_DISCONNECTED "语音服务未运行或已断开"
#define VOICE_NOTIFY_ERROR_PREFIX "语音识别出错："

void voice_fsm_init(VoiceFsm *f, guint hotkey_keyval, gboolean review_enabled);
void voice_fsm_clear(VoiceFsm *f);

/* keyval/keycode/modifiers exactly as IBus delivers them (IBUS_RELEASE_MASK on release).
 * rime_composing: Rime currently has an uncommitted composition. */
VoiceAction voice_fsm_key(VoiceFsm *f, guint keyval, guint keycode, guint modifiers,
                          gboolean rime_composing);
/* REVIEW only: Rime declined this key. Printable characters (no Control/Alt) are inserted at
 * the cursor; everything else is swallowed. Always consume == TRUE in REVIEW. */
VoiceAction voice_fsm_unhandled(VoiceFsm *f, guint keyval, guint modifiers);
/* REVIEW only: Rime committed text; insert it at the cursor instead of the application. */
VoiceAction voice_fsm_insert(VoiceFsm *f, const char *text);
VoiceAction voice_fsm_event(VoiceFsm *f, VoiceEvType type, const char *payload);
VoiceAction voice_fsm_disconnected(VoiceFsm *f);
VoiceAction voice_fsm_focus_out(VoiceFsm *f);
VoiceAction voice_fsm_focus_in(VoiceFsm *f); /* re-shows the current aux text */

/* Cursor glyph for voice/cursor: "heavy" ❚ (default), "block" ▮, "caret" ˰, "bar" |,
 * "triangle" ▲. */
const char *voice_cursor_mark(const char *style);
/* Invisible text as wide as the mark (blink "off" phase), so the row does not shift. Widths were
 * measured with Pango for Ubuntu Sans and its fallbacks (DejaVu Sans, Noto Sans CJK); they match to
 * 0.01 em at 11 pt and at 100 px. */
const char *voice_cursor_blank(const char *mark);
/* REVIEW only: flip the blink phase and redraw the note (a.note). Any other redraw (a key,
 * inserted text) shows the mark again, so the cursor is visible right after it moves. */
VoiceAction voice_fsm_blink(VoiceFsm *f);
/* Header block of the review note, e.g. "🎙 语音草稿 · ⏎ 上屏 · 右⇧⏎ 发送 · Esc 放弃 · 右Alt 续说\n"
 * VOICE_REVIEW_RULE (Enter and Shift+Enter swapped when enter_sends). */
void voice_review_header(guint hotkey, gboolean enter_sends, GString *out);
/* Wrap text to cols display columns: a line never starts with closing punctuation (it may
 * overflow by one character instead), and an ASCII word shorter than cols/2 is moved whole to
 * the next line. Returns newly allocated strings (free with g_ptr_array_unref). */
GPtrArray *voice_wrap(const char *text, guint cols);

/* Rows of the review note (borrowed, valid until the next fsm call) and the cursor row. */
GPtrArray *voice_fsm_note(VoiceFsm *f, gint *cursor_row);
/* Scrollbar labels, one per row of voice_fsm_note() (borrowed). Thumb: when n rows exist and
 * v are visible starting at row `first`, the thumb is max(1, round(v*v/n)) rows long and starts
 * at round(first * (v - len) / (n - v)). All VOICE_SCROLL_NONE when everything fits. */
GPtrArray *voice_fsm_note_labels(VoiceFsm *f);
/* Aux text while Rime composes inside the review: VOICE_COMPOSE_PREFIX + the cursor row. */
const char *voice_fsm_compose_hint(VoiceFsm *f);

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out);
void voice_fsm_drop_queue(VoiceFsm *f);

#endif
