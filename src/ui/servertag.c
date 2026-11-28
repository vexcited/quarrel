#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

// what discord takes for a tag.
constexpr size_t TAG_LETTERS = 4;

static void take(ServerUi *ui, const Admin *a) {
  free(ui->tag_name);
  ui->tag_on = a->tag.tag != nullptr;
  ui->tag_name = strdup(a->tag.tag != nullptr ? a->tag.tag : "");
  ui->tag_badge = a->tag.badge;
  ui->tag_primary = a->tag.primary;
  ui->tag_secondary = a->tag.secondary;
  ui->tag_taken = true;
}

void tag_forget(ServerUi *ui) {
  free(ui->tag_name);
  ui->tag_name = nullptr;
  ui->tag_taken = false;
  ui->tag_picking = 0;
}

bool tag_dirty(App *app, const ServerUi *ui) {
  const Admin *a = app->session->admin;

  if (!ui->tag_taken || a == nullptr || a->guild_id != ui->guild_id) {
    return false;
  }

  const AdminTag *t = &a->tag;

  if (ui->tag_on != (t->tag != nullptr)) {
    return true;
  }

  return ui->tag_on &&
         (strcmp(ui->tag_name ? ui->tag_name : "", t->tag) != 0 ||
          ui->tag_badge != t->badge || ui->tag_primary != t->primary ||
          ui->tag_secondary != t->secondary);
}

void tag_save(App *app, ServerUi *ui) {
  AdminTag tag = {
    .tag = ui->tag_on ? ui->tag_name : nullptr,
    .badge = ui->tag_badge,
    .primary = ui->tag_primary,
    .secondary = ui->tag_secondary,
  };

  admin_save_tag(app->session, ui->guild_id, &tag);
}

// the first `TAG_LETTERS` characters of what was typed.
static void cap_tag(char *tag) {
  size_t letters = 0;

  for (size_t i = 0; tag[i]; i++) {
    if (((unsigned char)tag[i] & 0xc0) != 0x80 && letters++ == TAG_LETTERS) {
      tag[i] = '\0';
      return;
    }
  }
}

// a badge over the open node, whole and dimmed by `opacity`.
static void badge_art(App *app, uint8_t badge, int32_t primary,
                      int32_t secondary, float opacity) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  char url[TAG_BADGE_URL_MAX];

  tag_badge_url(url, badge, primary, secondary);

  civue_image_info image = civue_images_get(
    app->images, ui, url, (uint32_t)ceil(civue_rect_w(r) * ui->scale));

  if (image.state == CIVUE_IMAGE_READY) {
    civue_image(ui, image.texture, r, (float[4]){0, 0, 1, 1}, opacity, 0,
                false);
  }
}

// a square of the pickers, framed when it is the one picked. true when
// pressed.
static bool cell(App *app, civue_id key_id, const char *label, bool on,
                 bool disabled, civue_rect *at) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(key_id), relative, flex, items_center, justify_center, size(px(48)),
      flex_shrink_0, rounded(lg), bg(k->field), border(2),
      border_color(on ? k->primary : k->border),
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    pressed = state.activated && !disabled;

    if (state.response.hovered && !disabled && !on) {
      civue_stroke(civue_ui_current(), here(), k->border_strong, 2, 8);
    }

    if (label != nullptr) {
      hint(app, key_id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
    }

    if (at != nullptr) {
      *at = here();
    }
  }

  return pressed;
}

static bool badge_cell(App *app, ServerUi *ui, uint8_t badge, bool locked,
                       bool disabled) {
  const Look *k = &app->look;
  bool pressed = false;
  char label[48];

  snprintf(label, sizeof label, "%s badge%s", TAG_BADGE_ART[badge].label,
           locked ? ", unlocked with Boosts" : "");

  div(relative, flex_shrink_0) {
    pressed = cell(app, id_at(id("tag-badge"), badge), label,
                   ui->tag_badge == badge && !locked, disabled || locked,
                   nullptr);

    div(absolute, left(px(8)), top(px(8)), size(px(32))) {
      badge_art(app, badge, TAG_TINT_NONE, TAG_TINT_NONE,
                disabled || locked ? .35f : 1);
    }

    if (locked) {
      div(absolute, left(0), top(0), size(px(48)), flex, items_center,
          justify_center) {
        glyph(CIVUE_ICON(mingcute, lock_fill), 20, k->strong);
      }
    }
  }

  return pressed;
}

// the free badges, then the packs the guild unlocked, then the locked ones.
static void badges(App *app, ServerUi *ui, const Guild *g, bool disabled) {
  const Look *k = &app->look;
  bool unlocked[TAG_PACKS];

  form_label(k, "Choose Badge");
  text("Changing just the badge won't remove the Server Tag from members.",
       flex_shrink_0, mb(4), text_size(px(14)), line_height(px(18)),
       text_color(k->subtle));

  for (size_t i = 0; i < TAG_PACKS; i++) {
    unlocked[i] = store_guild_has(g, TAG_PACK[i].feature);
  }

  div(flex, flex_wrap, gap(2), flex_shrink_0) {
    for (size_t i = 0; i < TAG_FREE_BADGES; i++) {
      if (badge_cell(app, ui, TAG_FREE_BADGE[i], false, disabled)) {
        ui->tag_badge = TAG_FREE_BADGE[i];
      }
    }

    for (int locked = 0; locked < 2; locked++) {
      for (size_t i = 0; i < TAG_PACKS; i++) {
        if (unlocked[i] == (locked != 0)) {
          continue;
        }

        for (uint8_t b = TAG_PACK[i].first; b < TAG_PACK[i].first + 5; b++) {
          if (badge_cell(app, ui, b, locked, disabled)) {
            ui->tag_badge = b;
          }
        }
      }
    }
  }
}

static void colors(App *app, ServerUi *ui, bool disabled) {
  const Look *k = &app->look;
  uint8_t badge = ui->tag_badge;
  bool none = ui->tag_primary == TAG_TINT_NONE &&
              ui->tag_secondary == TAG_TINT_NONE;
  bool preset = false;
  civue_rect custom[2] = {};

  form_label(k, "Choose Color");

  div(flex, flex_wrap, gap(2), flex_shrink_0) {
    for (size_t i = 0; i < TAG_PALETTES; i++) {
      const TagPalette *p = &TAG_PALETTE[i];
      bool on = ui->tag_primary == (int32_t)p->primary &&
                ui->tag_secondary == (int32_t)p->secondary;

      preset = preset || on;

      div(relative, flex_shrink_0) {
        if (cell(app, id_at(id("tag-palette"), i), nullptr, on, disabled,
                 nullptr)) {
          ui->tag_primary = (int32_t)p->primary;
          ui->tag_secondary = (int32_t)p->secondary;
        }

        div(absolute, left(px(8)), top(px(8)), size(px(32))) {
          badge_art(app, badge, (int32_t)p->primary, (int32_t)p->secondary,
                    disabled ? .35f : 1);
        }
      }
    }

    div(relative, flex_shrink_0) {
      if (cell(app, id("tag-palette-reset"), "Reset to default colors", none,
               disabled, nullptr)) {
        ui->tag_primary = ui->tag_secondary = TAG_TINT_NONE;
      }

      div(absolute, left(px(8)), top(px(8)), size(px(32))) {
        badge_art(app, badge, TAG_TINT_NONE, TAG_TINT_NONE,
                  disabled ? .35f : 1);
      }

      div(absolute, right(px(-4)), bottom(px(-4)), flex, items_center,
          justify_center, size(px(18)), rounded(full), bg(k->surface_high)) {
        glyph(CIVUE_ICON(mingcute, refresh_2_line), 12, k->strong);
      }
    }

    // discord picks both colors in one popout, which here is one each.
    for (uint8_t which = 1; which <= TAG_BADGE_ART[badge].colors; which++) {
      int32_t tint = which == 1 ? ui->tag_primary : ui->tag_secondary;

      div(relative, flex_shrink_0) {
        if (cell(app, id_at(id("tag-custom"), which),
                 which == 1 ? "Custom primary color"
                            : "Custom secondary color",
                 !none && !preset, disabled, &custom[which - 1])) {
          ui->tag_picking = ui->tag_picking == which ? 0 : which;
        }

        div(absolute, left(0), top(0), size(px(48)), flex, items_center,
            justify_center) {
          if (tint != TAG_TINT_NONE && !preset) {
            div(size(px(20)), rounded(full), bg(rgb((uint32_t)tint))) {}
          }
          else {
            glyph(CIVUE_ICON(mingcute, color_picker_line), 20,
                  disabled ? k->faint : k->strong);
          }
        }
      }
    }
  }

  text("These preset color pairs set your badge's primary and secondary "
       "colors.",
       flex_shrink_0, mt(2), text_size(px(12)), line_height(px(16)),
       text_color(k->subtle));

  if (ui->tag_picking != 0 && !disabled) {
    uint8_t which = ui->tag_picking;
    int32_t *tint = which == 1 ? &ui->tag_primary : &ui->tag_secondary;
    // a custom color starts from the first pair, as discord's.
    uint32_t color = *tint != TAG_TINT_NONE
                       ? (uint32_t)*tint
                       : (which == 1 ? TAG_PALETTE[0].primary
                                     : TAG_PALETTE[0].secondary);
    uint32_t was = color;

    if (color_picker(app, custom[which - 1], &color)) {
      ui->tag_picking = 0;
    }

    if (color != was || *tint == TAG_TINT_NONE) {
      *tint = (int32_t)color;

      // a pair needs both colors.
      if (ui->tag_primary == TAG_TINT_NONE) {
        ui->tag_primary = (int32_t)TAG_PALETTE[0].primary;
      }

      if (ui->tag_secondary == TAG_TINT_NONE) {
        ui->tag_secondary = (int32_t)TAG_PALETTE[0].secondary;
      }
    }
  }
}

static void name(App *app, ServerUi *ui, const Admin *a, bool disabled) {
  const Look *k = &app->look;

  form_label(k, "Choose Name");

  div(flex, items_center, gap(3), flex_shrink_0) {
    div(flex, items_center, justify_center, size(px(40)), flex_shrink_0) {
      div(size(px(32))) {
        badge_art(app, ui->tag_badge, ui->tag_primary, ui->tag_secondary,
                  disabled ? .35f : 1);
      }
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      if (disabled) {
        div(flex, items_center, h(px(40)), px(3), rounded(md), bg(k->field),
            border(1), border_color(k->border)) {
          text(ui->tag_name && *ui->tag_name ? ui->tag_name : "WUMP",
               text_color(k->faint));
        }
      }
      else {
        form_input(app, id("tag-name"), &ui->tag_name, "WUMP", nullptr, 32);

        if (ui->tag_name != nullptr) {
          size_t len = strlen(ui->tag_name);

          cap_tag(ui->tag_name);

          if (strlen(ui->tag_name) != len) {
            char *capped = strdup(ui->tag_name);

            form_input_set(id("tag-name"), &ui->tag_name, capped);
            free(capped);
          }
        }
      }
    }
  }

  text("You may use max 4 characters, alphabet and numbers.", flex_shrink_0,
       mt(2), text_size(px(12)), line_height(px(16)), text_color(k->subtle));

  // a new name takes the tag off everyone wearing it.
  if (a->tag.tag != nullptr && ui->tag_on && ui->tag_name &&
      strcmp(ui->tag_name, a->tag.tag) != 0) {
    div(flex_shrink_0, mt(3), p(3), rounded(md),
        bg(civue_alpha(k->warning, .12f)), border(1),
        border_color(civue_alpha(k->warning, .5f))) {
      text("Updating your Server Tag will require all of your members to "
           "manually reapply the tag to their profile. We do this to "
           "prevent abuse.",
           text_size(px(12)), line_height(px(16)), text_color(k->text));
    }
  }
}

// a message as the tag would show next to the user's name.
static void sample(App *app, const User *self, const Guild *g,
                   const char *tag, const char *badge) {
  const Look *k = &app->look;

  div(flex, items_start, gap(3), flex_shrink_0) {
    avatar(app, self, g->id, 40, false, k->frame);

    div(flex, flex_col, flex_1, min_w(0)) {
      div(flex, items_center, gap(1)) {
        text(store_user_name(self), truncate, font_weight(semibold),
             text_color(k->strong));

        if (tag != nullptr && *tag) {
          guild_tag_pill(app, tag, badge);
        }
      }

      text("wow this looks so cool", text_color(k->text));
    }
  }
}

// how the tag shows, then the guild's own for the user to wear it.
static void preview(App *app, ServerUi *ui, const Guild *g) {
  const Look *k = &app->look;
  Session *s = app->session;
  const User *self = store_user(s->store, s->store->self_id);
  char badge[TAG_BADGE_URL_MAX];

  if (self == nullptr) {
    return;
  }

  tag_badge_url(badge, ui->tag_badge, ui->tag_primary, ui->tag_secondary);

  div(flex, flex_col, gap(4), w(px(300)), flex_shrink_0) {
    div(flex, flex_col, gap(3), p(4), rounded(xl), bg(k->frame), border(1),
        border_color(k->border)) {
      text("Preview", text_size(px(12)), line_height(px(16)),
           font_weight(semibold), text_color(k->subtle));
      sample(app, self, g,
             ui->tag_on && ui->tag_name && *ui->tag_name ? ui->tag_name
                                                         : "WUMP",
             badge);
    }

    if (g->tag != nullptr && ui->tag_on) {
      bool worn = self->tag_guild_id == g->id;
      char url[CDN_URL_MAX];

      cdn_tag_badge(url, g->id, g->tag_badge, 32);

      div(flex, flex_col, gap(3), p(4), rounded(xl), bg(k->frame), border(1),
          border_color(k->border)) {
        text(worn ? "You're currently using this Server Tag. This is how "
                    "people will see you."
                  : "You don't have this Server Tag on your own profile yet. "
                    "Would you like to adopt it?",
             text_size(px(12)), line_height(px(16)), text_color(k->subtle));
        sample(app, self, g, g->tag, url);

        div(flex, flex_col, items_stretch) {
          if (form_button(app, id("tag-adopt"),
                          s->tag_wearing ? "Adopting..." : "Adopt Tag",
                          BUTTON_PRIMARY, worn || s->tag_wearing)) {
            session_wear_tag(s, g->id);
          }
        }

        if (s->tag_error != nullptr) {
          text(s->tag_error, text_size(px(12)), line_height(px(16)),
               text_color(k->critical_text));
        }
      }
    }
  }
}

void tag_page(App *app, ServerUi *ui, Guild *g, float wide) {
  const Look *k = &app->look;
  Session *s = app->session;
  Admin *a = admin_of(s, g->id);
  bool unlocked = store_guild_has(g, "GUILD_TAGS");

  if (!a->tag_loaded) {
    admin_fetch_tag(s, g->id);
    text("Loading...", text_color(k->subtle));
    return;
  }

  if (!ui->tag_taken) {
    take(ui, a);
  }

  bool disabled = !unlocked || !ui->tag_on;

  div(flex, items_start, gap(10), flex_shrink_0) {
    div(flex, flex_col, flex_1, min_w(0), max_w(px(560))) {
      form_heading(k, "Server Tag",
                   "Create a tag that your server members can display next "
                   "to their name! Anyone outside your server can view your "
                   "Server Profile through the Server Tag, and if "
                   "applications are enabled, they can apply to join.");

      if (!a->tag.visible) {
        div(flex_shrink_0, mt(4), p(3), rounded(md),
            bg(civue_alpha(k->warning, .12f)), border(1),
            border_color(civue_alpha(k->warning, .5f))) {
          text("Your Server Profile is private. Users will not be able to "
               "see it from a Server Tag.",
               text_size(px(12)), line_height(px(16)), text_color(k->text));
        }
      }

      div(flex_shrink_0, h(6)) {}

      if (unlocked) {
        if (form_switch_row(app, id("tag-enable"), "Enable Server Tag",
                            nullptr, ui->tag_on, false)) {
          ui->tag_on = !ui->tag_on;
          ui->tag_picking = 0;
        }
      }
      else {
        div(flex, items_center, gap(3), flex_shrink_0, p(4), rounded(lg),
            bg(k->frame), border(1), border_color(k->border)) {
          glyph(CIVUE_ICON(mingcute, rocket_fill), 20, k->strong);
          text("Unlock a Server Tag using Boosts.", flex_1, min_w(0),
               text_color(k->text));

          if (form_button_small(app, id("tag-boost"), "Boost Perks",
                                BUTTON_PRIMARY, false)) {
            tag_forget(ui);
            ui->page = SERVER_BOOST;
          }
        }
      }

      form_divider(k);
      name(app, ui, a, disabled);
      form_divider(k);
      badges(app, ui, g, disabled);
      form_divider(k);
      colors(app, ui, disabled);
    }

    // the preview is hidden in a narrow window, as the profile's.
    if (wide >= 1024) {
      preview(app, ui, g);
    }
  }
}
