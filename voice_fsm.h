/* Dictation session state machine. Pure: no IO, no IBus calls; the engine glue
 * applies the returned VoiceAction. */
#ifndef _IBUS_RIME_VOICE_FSM_H_
#define _IBUS_RIME_VOICE_FSM_H_

#include <glib.h>
#include "voice_proto.h"

typedef enum { VOICE_IDLE, VOICE_RECORDING, VOICE_FINALIZING, VOICE_REVIEW } VoiceState;
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
} VoiceAction;

typedef struct { guint keyval, keycode, modifiers; } VoiceKey;

typedef struct {
  VoiceState state;
  guint hotkey;
  gboolean review_enabled; /* FINAL text goes to REVIEW instead of being committed */
  gboolean armed;          /* bare hotkey press seen, waiting for its release */
  gboolean from_review;    /* current recording was started from REVIEW */
  GString *aux;            /* current auxiliary text ("" when nothing to show) */
  GString *partial;        /* latest PARTIAL payload */
  GString *review;         /* review buffer */
  guint cursor;            /* review cursor, in characters */
  GPtrArray *note;         /* visible note rows (wrapped review text, cursor marked) */
  gint note_cursor;        /* index in note of the row holding the cursor, -1 if none */
  GString *compose_hint;   /* aux text while Rime composes inside the review */
  GString *commit_buf;
  GString *notify_buf;
  GArray *queued;          /* VoiceKey typed while FINALIZING */
} VoiceFsm;

#define VOICE_AUX_RECORDING_FMT "🎤 录音中 · %s 结束 · Esc 取消" /* %s: hotkey label */
#define VOICE_AUX_FINALIZING "⏳ 识别中…"
#define VOICE_AUX_PARTIAL_RECORDING "🎤 "
#define VOICE_AUX_PARTIAL_FINALIZING "⏳ "
/* Review note: the header goes to the auxiliary text; the review text, wrapped to
 * VOICE_REVIEW_COLS display columns (wide/ambiguous CJK = 2), is shown as rows of the IBus
 * lookup table so the cursor row gets the theme's highlight. At most VOICE_REVIEW_MAX_LINES
 * rows around the cursor are shown; a VOICE_REVIEW_MORE row marks hidden rows above/below. */
#define VOICE_REVIEW_PREFIX "🎙 "
#define VOICE_REVIEW_CURSOR "˰" /* narrow, sits low: marks the gap without taking a cell */
#define VOICE_REVIEW_MORE "⋯"
#define VOICE_COMPOSE_PREFIX "✎ "
#define VOICE_REVIEW_COLS 36
#define VOICE_REVIEW_MAX_LINES 6
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

/* Header of the review note, e.g. "🎙 语音草稿 · ⏎ 上屏 · Esc 放弃 · 右Alt 续说". */
void voice_review_header(guint hotkey, GString *out);
/* Wrap text to cols display columns: a line never starts with closing punctuation (it may
 * overflow by one character instead), and an ASCII word shorter than cols/2 is moved whole to
 * the next line. Returns newly allocated strings (free with g_ptr_array_unref). */
GPtrArray *voice_wrap(const char *text, guint cols);

/* Rows of the review note (borrowed, valid until the next fsm call) and the cursor row. */
GPtrArray *voice_fsm_note(VoiceFsm *f, gint *cursor_row);
/* Aux text while Rime composes inside the review: VOICE_COMPOSE_PREFIX + the cursor row. */
const char *voice_fsm_compose_hint(VoiceFsm *f);

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out);
void voice_fsm_drop_queue(VoiceFsm *f);

#endif
