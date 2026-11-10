/* Dictation session state machine. Pure: no IO, no IBus calls; the engine glue
 * applies the returned VoiceAction. */
#ifndef _IBUS_RIME_VOICE_FSM_H_
#define _IBUS_RIME_VOICE_FSM_H_

#include <glib.h>
#include "voice_proto.h"

typedef enum { VOICE_IDLE, VOICE_RECORDING, VOICE_FINALIZING } VoiceState;
typedef enum { VOICE_CMD_NONE, VOICE_CMD_START, VOICE_CMD_STOP, VOICE_CMD_CANCEL } VoiceCmd;

typedef struct {
  gboolean consume;     /* return value for process_key_event */
  VoiceCmd send;        /* command to send to the daemon */
  const char *preedit;  /* NULL: leave unchanged; "": hide; else show (owned by the fsm) */
  const char *commit;   /* NULL: nothing to commit (owned by the fsm) */
  const char *notify;   /* NULL: no desktop notification */
  gboolean replay;      /* after commit, replay voice_fsm_queued() keys in order */
} VoiceAction;

typedef struct { guint keyval, keycode, modifiers; } VoiceKey;

typedef struct {
  VoiceState state;
  guint hotkey;
  gboolean armed;       /* bare hotkey press seen, waiting for its release */
  GString *buf;         /* preedit text */
  GString *partial;     /* latest PARTIAL payload */
  GString *commit_buf;  /* final text */
  GString *notify_buf;
  GArray *queued;       /* VoiceKey typed while FINALIZING */
} VoiceFsm;

#define VOICE_PREEDIT_RECORDING "🎤 "
#define VOICE_PREEDIT_FINALIZING "⏳ "
#define VOICE_NOTIFY_FOCUS_LOST "语音输入已取消（焦点切走）"
#define VOICE_NOTIFY_DISCONNECTED "语音服务未运行或已断开"
#define VOICE_NOTIFY_ERROR_PREFIX "语音识别出错："

void voice_fsm_init(VoiceFsm *f, guint hotkey_keyval);
void voice_fsm_clear(VoiceFsm *f);

/* keyval/keycode/modifiers exactly as IBus delivers them (IBUS_RELEASE_MASK on release).
 * rime_composing: Rime currently has an uncommitted composition. */
VoiceAction voice_fsm_key(VoiceFsm *f, guint keyval, guint keycode, guint modifiers,
                          gboolean rime_composing);
VoiceAction voice_fsm_event(VoiceFsm *f, VoiceEvType type, const char *payload);
VoiceAction voice_fsm_disconnected(VoiceFsm *f);
VoiceAction voice_fsm_focus_out(VoiceFsm *f);

const VoiceKey *voice_fsm_queued(VoiceFsm *f, guint *n_out);
void voice_fsm_drop_queue(VoiceFsm *f);

#endif
