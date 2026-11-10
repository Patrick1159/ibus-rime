/* Asynchronous client for the voiced daemon socket. All callbacks run on the
 * GLib main loop thread. */
#ifndef _IBUS_RIME_VOICE_CLIENT_H_
#define _IBUS_RIME_VOICE_CLIENT_H_

#include <glib.h>
#include "voice_proto.h"
#include "voice_fsm.h"

typedef struct _VoiceClient VoiceClient;
typedef void (*VoiceClientEventFn)(VoiceEvType type, const char *payload, gpointer user_data);
/* Connection failed, or closed by the daemon before FINAL/ERR was received. */
typedef void (*VoiceClientClosedFn)(gpointer user_data);

char *voice_client_default_path(void); /* g_free() the result */
/* Connects asynchronously and sends START. Never returns NULL. */
VoiceClient *voice_client_start(const char *socket_path, VoiceClientEventFn on_event,
                                VoiceClientClosedFn on_closed, gpointer user_data);
void voice_client_send(VoiceClient *c, VoiceCmd cmd); /* VOICE_CMD_STOP or VOICE_CMD_CANCEL */
/* Closes the connection; no callback fires after this returns. */
void voice_client_free(VoiceClient *c);

#endif
