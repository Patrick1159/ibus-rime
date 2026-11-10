#include <glib.h>
#include "../voice_proto.h"

static void test_parse(void) {
  const char *p;
  g_assert_cmpint(voice_proto_parse("REC", &p), ==, VOICE_EV_REC);
  g_assert_cmpstr(p, ==, "");
  g_assert_cmpint(voice_proto_parse("PARTIAL 你好 ok", &p), ==, VOICE_EV_PARTIAL);
  g_assert_cmpstr(p, ==, "你好 ok");
  g_assert_cmpint(voice_proto_parse("FINAL", &p), ==, VOICE_EV_FINAL);
  g_assert_cmpstr(p, ==, "");
  g_assert_cmpint(voice_proto_parse("ERR busy", &p), ==, VOICE_EV_ERR);
  g_assert_cmpstr(p, ==, "busy");
  g_assert_cmpint(voice_proto_parse("FINALX a", &p), ==, VOICE_EV_INVALID);
  g_assert_cmpint(voice_proto_parse("", &p), ==, VOICE_EV_INVALID);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/voice_proto/parse", test_parse);
  return g_test_run();
}
