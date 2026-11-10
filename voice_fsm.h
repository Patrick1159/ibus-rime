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
  const char *aux;      /* NULL: leave unchanged; "": hide; else show as IBus auxiliary text */
  const char *commit;   /* NULL: nothing to commit (owned by the fsm) */
  const char *notify;   /* NULL: no desktop notification */
  gboolean replay;      /* after commit, replay voice_fsm_queued() keys in order */
  gboolean send_return; /* after commit, forward a Return press+release to the application */
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
  GString *commit_buf;
  GString *notify_buf;
  GArray *queued;          /* VoiceKey typed while FINALIZING */
} VoiceFsm;

#define VOICE_AUX_RECORDING "🎤 录音中 · 热键结束 · Esc 取消"
#define VOICE_AUX_FINALIZING "⏳ 识别中…"
#define VOICE_AUX_PARTIAL_RECORDING "🎤 "
#define VOICE_AUX_PARTIAL_FINALIZING "⏳ "
#define VOICE_REVIEW_PREFIX "📝 "
#define VOICE_REVIEW_CURSOR "‸"
#define VOICE_REVIEW_HINT "  ⏎发送 ⇧⏎上屏 Esc放弃"
#define VOICE_REVIEW_WINDOW 40 /* characters of review text shown around the cursor */
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

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out);
void voice_fsm_drop_queue(VoiceFsm *f);

#endif
