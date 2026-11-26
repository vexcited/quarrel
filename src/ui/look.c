#include "look.h"

#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "platform/platform.h"

#define THEME_KEY "theme"

const char *const THEME_NAMES[THEMES] = {
  [THEME_MONOCHROME] = "Monochrome",
  [THEME_DISCORD] = "Discord",
};

// what the preferences keep of each.
static const char *const THEME_KEYS[THEMES] = {
  [THEME_MONOCHROME] = "monochrome",
  [THEME_DISCORD] = "discord",
};

// shadcn's neutral dark theme, its oklch tokens in srgb: the sidebar,
// background, card, popover and accent, foreground and muted-foreground for
// text, input and border as white at 15 and 10 percent. its destructive is
// #ff6467, drawn at 60 percent under white text. tailwind's green and amber
// stand for what shadcn has no token of.
static Look monochrome() {
  return (Look){
    .frame = rgb(0x171717),
    .frame_border = rgba(0xffffff1a),
    .chat = rgb(0x0a0a0a),
    .panel = rgb(0x171717),
    .surface = rgb(0x171717),
    .surface_high = rgb(0x262626),
    .popover = rgb(0x171717),
    .dialog = rgb(0x0a0a0a),
    .input = rgb(0x151515),
    .field = rgba(0xffffff0b),
    .scrim = rgba(0x00000080),
    .tooltip = rgb(0xfafafa),
    .on_tooltip = rgb(0x0a0a0a),
    .strong = rgb(0xfafafa),
    .text = rgb(0xfafafa),
    .subtle = rgb(0xa1a1a1),
    .muted = rgb(0xa1a1a1),
    .faint = rgb(0xa1a1a1),
    .dim = rgb(0x808080),
    .link = rgb(0xfafafa),
    .primary = rgb(0xe5e5e5),
    .primary_hover = rgb(0xcfcfcf),
    .primary_active = rgb(0xb9b9b9),
    .on_primary = rgb(0x171717),
    .secondary = rgb(0x262626),
    .secondary_hover = rgba(0x262626cc),
    .accent_text = rgb(0xfafafa),
    .danger = rgb(0x9d4042),
    .danger_hover = rgb(0x843738),
    .danger_active = rgb(0x6c2e2f),
    .on_danger = CIVUE_WHITE,
    .critical_text = rgb(0xff6467),
    .positive = rgb(0x15803d),
    .positive_hover = rgb(0x166534),
    .positive_active = rgb(0x14532d),
    .on_positive = CIVUE_WHITE,
    .positive_text = rgb(0x4ade80),
    .warning = rgb(0xf59e0b),
    .online = rgb(0x22c55e),
    .idle = rgb(0xf59e0b),
    .dnd = rgb(0xef4444),
    .offline = rgb(0x737373),
    .hover = rgba(0xffffff12),
    .selected = rgba(0xffffff1c),
    .pressed = rgba(0xffffff1a),
    .border = rgba(0xffffff1a),
    .border_strong = rgba(0xffffff26),
    .ring = rgb(0x737373),
    .selection = rgba(0xffffff40),
    .rule = rgb(0x404040),
    .drop = rgb(0xfafafa),
    .star = rgb(0xfafafa),
    .banner = rgb(0x262626),
    .message_hover = rgba(0xffffff0b),
    .mentioned = rgba(0xffffff0f),
    .mentioned_bar = rgb(0xe5e5e5),
    .mention_bg = rgba(0xffffff1f),
    .mention_text = rgb(0xfafafa),
    .code_bg = rgba(0xffffff1f),
    .spoiler = rgb(0x333333),
    .thumb = rgb(0x525252),
  };
}

static Look discord() {
  return (Look){
    .frame = rgb(0x121214),
    .frame_border = rgba(0x97979f1f),
    .chat = rgb(0x1a1a1e),
    .panel = rgb(0x202024),
    .surface = rgb(0x242429),
    .surface_high = rgb(0x2c2d32),
    .popover = rgb(0x28282d),
    .dialog = rgb(0x242429),
    .input = rgb(0x222327),
    .field = rgba(0x0000001f),
    .scrim = rgba(0x000000b3),
    .tooltip = rgb(0x2c2d32),
    .on_tooltip = rgb(0xefeff1),
    .strong = rgb(0xfbfbfb),
    .text = rgb(0xefeff1),
    .subtle = rgb(0xabacb2),
    .muted = rgb(0x96979e),
    .faint = rgb(0x81828a),
    .dim = rgb(0x4f505a),
    .link = rgb(0x4d96ee),
    .primary = rgb(0x5865f2),
    .primary_hover = rgb(0x4452bb),
    .primary_active = rgb(0x3a48a3),
    .on_primary = CIVUE_WHITE,
    .secondary = rgba(0x97979f1f),
    .secondary_hover = rgba(0x97979f29),
    .accent_text = rgb(0x94a8ff),
    .danger = rgb(0xd22d39),
    .danger_hover = rgb(0xa9232e),
    .danger_active = rgb(0x971d28),
    .on_danger = CIVUE_WHITE,
    .critical_text = rgb(0xf87e7a),
    .positive = rgb(0x008545),
    .positive_hover = rgb(0x006c37),
    .positive_active = rgb(0x005f30),
    .on_positive = CIVUE_WHITE,
    .positive_text = rgb(0x7ecb94),
    .warning = rgb(0xf2a640),
    .online = rgb(0x3d9e60),
    .idle = rgb(0xffcb6e),
    .dnd = rgb(0xdc4247),
    .offline = rgb(0x9d9ea5),
    .hover = rgba(0x97979f1f),
    .selected = rgba(0x97979f33),
    .pressed = rgba(0x97979f29),
    .border = rgba(0x97979f1f),
    .border_strong = rgba(0x97979f33),
    .ring = rgb(0x5865f2),
    .selection = rgba(0x5865f273),
    .rule = rgb(0x4f505a),
    .drop = rgb(0x3d9e60),
    .star = rgb(0xf0b232),
    .banner = rgb(0x5865f2),
    .message_hover = rgba(0x97979f0f),
    .mentioned = rgba(0xf8a30014),
    .mentioned_bar = rgb(0xf8a300),
    .mention_bg = rgba(0x5865f23d),
    .mention_text = rgb(0xa9bbff),
    .code_bg = rgba(0x97979f1f),
    .spoiler = rgb(0x41424a),
    .thumb = rgb(0x5f606a),
  };
}

Look look_of(Theme theme) {
  return theme == THEME_DISCORD ? discord() : monochrome();
}

Theme theme_saved(void) {
  char *kept = platform_pref_get(THEME_KEY);
  Theme theme = THEME_MONOCHROME;

  for (int i = 0; kept && i < THEMES; i++) {
    if (!strcmp(kept, THEME_KEYS[i])) {
      theme = (Theme)i;
    }
  }

  free(kept);
  return theme;
}

void theme_save(Theme theme) {
  platform_pref_set(THEME_KEY, THEME_KEYS[theme]);
}
