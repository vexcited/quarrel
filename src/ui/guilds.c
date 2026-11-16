#include "components.h"

#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

constexpr float ICON_SIZE = 40;

// the white bar at the edge of the bar: short for unread, taller under the
// pointer, the icon's height for the guild shown.
static void indicator(const Look *k, civue_id id, bool unread, bool hovered,
                      bool selected) {
  float target = selected ? 40 : hovered ? 20 : unread ? 8 : 0;
  float height =
    civue_animate(civue_ui_current(), id_in(id, "pill"), target, .15f);

  if (height < 1) {
    return;
  }

  div(absolute, left(0), top(px((ICON_SIZE - height) / 2)), w(1), h(px(height)),
      rounded_r(px(4)), bg(k->strong)) {}
}

static void corner_badge(const Look *k, uint32_t mentions) {
  if (!mentions) {
    return;
  }

  div(absolute, right(px(10)), bottom(px(-4)), p(px(3)), rounded(full),
      bg(k->frame)) {
    mention_badge(k, mentions);
  }
}

// the first letter of each word, what discord shows for a guild without an
// icon.
static void acronym_of(const char *name, char *out, size_t cap) {
  size_t n = 0;
  bool start = true;

  for (size_t i = 0; name[i] && n + 5 < cap; i++) {
    if (name[i] == ' ') {
      start = true;
      continue;
    }

    if (start) {
      size_t next = i;

      civue_utf8_next(name, strlen(name), &next);
      memcpy(out + n, name + i, next - i);
      n += next - i;
      start = false;
    }
  }

  out[n] = 0;
}

static void guild_icon(App *app, const Guild *guild, float size, bool lit) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_guild_icon(url, guild->id, guild->icon, (uint32_t)(size * 2));

  if (*url && image_fill(app, url, size * .3f)) {
    return;
  }

  char acronym[32];
  acronym_of(guild->name ? guild->name : "?", acronym, sizeof acronym);

  fill(here(), lit ? k->brand : k->hover, size * .3f);

  if (size >= ICON_SIZE) {
    text(acronym, text_size(px(strlen(acronym) > 3 ? 12 : 16)),
         font_weight(medium), text_color(k->text), whitespace_nowrap);
  }
}

static void guild_item(App *app, Guild *guild) {
  const Look *k = &app->look;
  civue_id id = id_at(id("guild"), guild->id);
  bool selected = app->guild_id == guild->id;
  Unread unread = store_guild_unread(app->session->store, guild);

  div(relative, flex, justify_center, flex_shrink_0, w(full),
      h(px(ICON_SIZE))) {
    bool hovered = false;

    div(key(id), flex, items_center, justify_center, size(px(ICON_SIZE)),
        rounded(xl), overflow_hidden, cursor_pointer) {
      civue_button_state state = button();
      hovered = state.response.hovered;

      if (state.activated) {
        app_select_guild(app, guild->id);
      }

      guild_icon(app, guild, ICON_SIZE, selected || hovered);
      hint(app, id, &state.response, guild->name ? guild->name : "",
           CIVUE_TOOLTIP_RIGHT);
    }

    indicator(k, id, unread.unread && !unread.muted, hovered, selected);
    corner_badge(k, unread.mentions);
  }
}

static bool folder_holds(const GuildFolder *folder, Snowflake guild_id) {
  for (size_t i = 0; i < folder->nguilds; i++) {
    if (folder->guilds[i]->id == guild_id) {
      return true;
    }
  }

  return false;
}

static void folder_preview(App *app, const GuildFolder *folder) {
  div(grid, grid_cols(2), grid_rows(2), gap(px(2)), size(full), p(px(4))) {
    for (size_t i = 0; i < folder->nguilds && i < 4; i++) {
      div(flex, items_center, justify_center, rounded(sm), overflow_hidden) {
        guild_icon(app, folder->guilds[i], 15, false);
      }
    }
  }
}

static void folder_item(App *app, const GuildFolder *folder) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  civue_id id = id_at(id("folder"), folder->id);
  bool *open = civue_memory(ui, id, sizeof *open, nullptr);
  civue_color tint = folder->has_color ? rgb(folder->color) : k->brand;
  bool holds = folder_holds(folder, app->guild_id);
  Unread all = {};

  for (size_t i = 0; i < folder->nguilds; i++) {
    Unread one = store_guild_unread(store, folder->guilds[i]);

    all.unread |= one.unread && !one.muted;
    all.mentions += one.mentions;
  }

  div(relative, flex, flex_col, items_center, gap(2), flex_shrink_0, w(full)) {
    bool hovered = false;

    // an open folder sits on a ground of its own with its guilds.
    if (*open) {
      div(absolute, left(px(12)), right(px(12)), top(0), bottom(0),
          rounded(px(16)), bg(k->hover)) {}
    }

    div(key(id), flex, items_center, justify_center, size(px(ICON_SIZE)),
        rounded(xl), cursor_pointer,
        style(.bg = *open ? CIVUE_CLEAR : civue_alpha(tint, .4f))) {
      civue_button_state state = button();
      hovered = state.response.hovered;

      if (state.activated) {
        *open = !*open;
      }

      if (*open) {
        icon(lucide, folder, size(5), text_color(tint));
      }
      else {
        folder_preview(app, folder);
      }

      hint(app, id, &state.response,
           folder->name && *folder->name ? folder->name : "Folder",
           CIVUE_TOOLTIP_RIGHT);
    }

    if (!*open) {
      indicator(k, id, all.unread, hovered, holds);
      corner_badge(k, all.mentions);
    }
    else {
      for (size_t i = 0; i < folder->nguilds; i++) {
        guild_item(app, folder->guilds[i]);
      }

      div(h(px(0))) {}
    }
  }
}

static void home_button(App *app) {
  const Look *k = &app->look;
  civue_id id = id("home");
  bool selected = app->guild_id == 0;
  Unread unread = store_private_unread(app->session->store);

  div(relative, flex, justify_center, flex_shrink_0, w(full),
      h(px(ICON_SIZE))) {
    bool hovered = false;

    div(key(id), flex, items_center, justify_center, size(px(ICON_SIZE)),
        rounded(xl), bg(selected ? k->brand : k->hover),
        text_color(selected ? CIVUE_WHITE : k->text),
        hover(bg(k->brand), text_color(CIVUE_WHITE)), cursor_pointer) {
      civue_button_state state = button();
      hovered = state.response.hovered;

      if (state.activated) {
        app_select_guild(app, 0);
      }

      icon(ic, baseline_discord, size(6));
      hint(app, id, &state.response, "Direct Messages", CIVUE_TOOLTIP_RIGHT);
    }

    indicator(k, id, unread.unread && !unread.muted, hovered, selected);
    corner_badge(k, unread.mentions);
  }
}

void guild_bar(App *app) {
  const Look *k = &app->look;
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(app->session->store, &count);

  div(key(id("guilds")), flex, flex_col, items_center, gap(2), flex_shrink_0,
      w(px(GUILD_BAR_WIDTH)), h(full), overflow_y_scroll, pb(20)) {
    home_button(app);

    div(w(8), h(px), flex_shrink_0, bg(k->frame_border)) {}

    for (size_t i = 0; i < count; i++) {
      const GuildFolder *folder = &folders[i];

      if (folder->id == 0 && folder->nguilds == 1) {
        guild_item(app, folder->guilds[0]);
      }
      else if (folder->nguilds) {
        folder_item(app, folder);
      }
    }
  }
}
