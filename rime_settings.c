#include "rime_config.h"
#include "rime_settings.h"
#include <rime_api.h>
#include <string.h>

extern RimeApi *rime_api;

static struct ColorSchemeDefinition preset_color_schemes[] = {
  { "aqua", 0xffffff, 0x0a3dfa },
  { "azure", 0xffffff, 0x0a3dea },
  { "ink", 0xffffff, 0x000000 },
  { "luna", 0x000000, 0xffff7f },
  { NULL, 0, 0 }
};

static struct IBusRimeSettings ibus_rime_settings_default = {
  .embed_preedit_text = TRUE,
  .preedit_style = PREEDIT_STYLE_COMPOSITION,
  .cursor_type = CURSOR_TYPE_INSERT,
  .lookup_table_orientation = IBUS_ORIENTATION_SYSTEM,
  .color_scheme = NULL,
  .voice_hotkey = IBUS_KEY_Alt_R,
  .voice_review = TRUE,
  .voice_enter_sends = FALSE,
};

struct IBusRimeSettings g_ibus_rime_settings;

static void
select_color_scheme(struct IBusRimeSettings* settings,
		    const char* color_scheme_id)
{
  struct ColorSchemeDefinition* c;
  for (c = preset_color_schemes; c->color_scheme_id; ++c) {
    if (!strcmp(c->color_scheme_id, color_scheme_id)) {
      settings->color_scheme = c;
      g_debug("selected color scheme: %s", color_scheme_id);
      return;
    }
  }
  // fallback to default
  settings->color_scheme = NULL;
}

void
ibus_rime_load_settings()
{
  g_ibus_rime_settings = ibus_rime_settings_default;

  RimeConfig config = {0};
  if (!rime_api->config_open("ibus_rime", &config)) {
    g_error("error loading settings for ibus_rime");
    return;
  }

  Bool inline_preedit = False;
  if (rime_api->config_get_bool(
          &config, "style/inline_preedit", &inline_preedit)) {
    g_ibus_rime_settings.embed_preedit_text = !!inline_preedit;
  }

  const char* preedit_style_str =
      rime_api->config_get_cstring(&config, "style/preedit_style");
  if(preedit_style_str) {
    if(!strcmp(preedit_style_str, "composition")) {
      g_ibus_rime_settings.preedit_style = PREEDIT_STYLE_COMPOSITION;
    } else if(!strcmp(preedit_style_str, "preview")) {
      g_ibus_rime_settings.preedit_style = PREEDIT_STYLE_PREVIEW;
    }
  }

  const char* cursor_type_str =
      rime_api->config_get_cstring(&config, "style/cursor_type");
  if (cursor_type_str) {
    if (!strcmp(cursor_type_str, "insert")) {
      g_ibus_rime_settings.cursor_type = CURSOR_TYPE_INSERT;
    } else if (!strcmp(cursor_type_str, "select")) {
      g_ibus_rime_settings.cursor_type = CURSOR_TYPE_SELECT;
    }
  }

  Bool horizontal = False;
  if (rime_api->config_get_bool(&config, "style/horizontal", &horizontal)) {
    g_ibus_rime_settings.lookup_table_orientation =
      horizontal ? IBUS_ORIENTATION_HORIZONTAL : IBUS_ORIENTATION_VERTICAL;
  }

  const char* color_scheme =
    rime_api->config_get_cstring(&config, "style/color_scheme");
  if (color_scheme) {
    select_color_scheme(&g_ibus_rime_settings, color_scheme);
  }

  const char* voice_hotkey =
    rime_api->config_get_cstring(&config, "voice/hotkey");
  if (voice_hotkey) {
    guint keyval = ibus_keyval_from_name(voice_hotkey);
    if (keyval != IBUS_KEY_VoidSymbol) {
      g_ibus_rime_settings.voice_hotkey = keyval;
    }
  }

  Bool voice_review = True;
  if (rime_api->config_get_bool(&config, "voice/review", &voice_review)) {
    g_ibus_rime_settings.voice_review = !!voice_review;
  }

  Bool voice_enter_sends = False;
  if (rime_api->config_get_bool(&config, "voice/enter_sends", &voice_enter_sends)) {
    g_ibus_rime_settings.voice_enter_sends = !!voice_enter_sends;
  }

  rime_api->config_close(&config);
}
