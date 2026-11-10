/* Line protocol spoken with the voiced daemon (see toolbox voice-input spec). */
#ifndef _IBUS_RIME_VOICE_PROTO_H_
#define _IBUS_RIME_VOICE_PROTO_H_

typedef enum {
  VOICE_EV_REC,
  VOICE_EV_PARTIAL,
  VOICE_EV_FINAL,
  VOICE_EV_ERR,
  VOICE_EV_INVALID,
} VoiceEvType;

/* Parse one daemon line with its trailing "\r\n" already removed.
 * *payload points into line (the text after "<KIND> "), or to "" if none. */
VoiceEvType voice_proto_parse(const char *line, const char **payload);

#endif
