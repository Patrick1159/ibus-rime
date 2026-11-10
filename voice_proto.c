#include <string.h>
#include "voice_proto.h"

static const struct { const char *kind; VoiceEvType type; } kinds[] = {
  {"REC", VOICE_EV_REC},
  {"PARTIAL", VOICE_EV_PARTIAL},
  {"FINAL", VOICE_EV_FINAL},
  {"ERR", VOICE_EV_ERR},
};

VoiceEvType voice_proto_parse(const char *line, const char **payload) {
  *payload = "";
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    size_t n = strlen(kinds[i].kind);
    if (strncmp(line, kinds[i].kind, n) != 0) continue;
    if (line[n] == '\0') return kinds[i].type;
    if (line[n] == ' ') {
      *payload = line + n + 1;
      return kinds[i].type;
    }
  }
  return VOICE_EV_INVALID;
}
