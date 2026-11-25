#include "components.h"

#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "server.h"
#include "widgets.h"

static civue_icon channel_icon(const Channel *c) {
  switch (c->type) {
    case CHANNEL_GUILD_VOICE:
      return CIVUE_ICON(lucide, volume_2);
    case CHANNEL_GUILD_STAGE:
      return CIVUE_ICON(lucide, radio);
    case CHANNEL_GUILD_ANNOUNCEMENT:
      return CIVUE_ICON(lucide, megaphone);
    case CHANNEL_GUILD_FORUM:
      return CIVUE_ICON(lucide, messages_square);
    case CHANNEL_GUILD_MEDIA:
      return CIVUE_ICON(lucide, image);
    case CHANNEL_GUILD_DIRECTORY:
      return CIVUE_ICON(lucide, compass);
    case CHANNEL_ANNOUNCEMENT_THREAD:
    case CHANNEL_PUBLIC_THREAD:
    case CHANNEL_PRIVATE_THREAD:
      return CIVUE_ICON(lucide, message_square);
    default:
      return CIVUE_ICON(lucide, hash);
  }
}

static void unread_mark(const Look *k) {
  div(absolute, left(px(-8)), top(px(12)), w(1), h(2), rounded_r(px(4)),
      bg(k->strong)) {}
}

static void category_row(App *app, Guild *guild, Channel *category) {
  const Look *k = &app->look;
  bool manages = store_can(app->session->store, category,
                           PERM_MANAGE_CHANNELS);
  bool create = false;

  div(key(id_at(id("category"), category->id)), flex, items_center, gap(1),
      h(8), mt(3), px(2), mx(2), text_color(k->faint),
      hover(text_color(k->text)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), cursor_pointer) {
    civue_button_state state = button();

    channel_drag_row(app, guild, category, here(), &state.response);

    if (state.activated && !channel_dragging(app)) {
      category->collapsed = !category->collapsed;
    }

    text(category->name ? category->name : "", truncate, min_w(0));

    if (category->collapsed) {
      icon(lucide, chevron_right, size(3), flex_shrink_0);
    }
    else {
      icon(lucide, chevron_down, size(3), flex_shrink_0);
    }

    div(flex_1) {}

    // discord's "+" of a category, while the pointer is over it.
    if (manages && state.response.hovered && !channel_dragging(app)) {
      civue_id plus = id_at(id("category-create"), category->id);

      div(key(plus), flex, items_center, justify_center, size(5),
          flex_shrink_0, text_color(k->subtle), hover(text_color(k->strong))) {
        civue_button_state add = button();

        create = add.activated;
        hint(app, plus, &add.response, "Create Channel", CIVUE_TOOLTIP_ABOVE);
        icon(lucide, plus, size(4));
      }
    }
  }

  if (create) {
    channel_create_open(app, guild->id, category->id, false);
  }
}

static void channel_row(App *app, Guild *guild, Channel *c, bool thread) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool selected = app->session->selected_channel_id == c->id;
  Unread unread = store_channel_unread(store, c);
  bool lit = unread.unread && !unread.muted;
  civue_color color = selected || lit ? k->strong
                      : unread.muted  ? rgb(0x4f505a)
                                      : k->faint;

  color = civue_alpha(color, color.a * channel_drag_alpha(app, c));

  div(key(id_at(id("channel"), c->id)), relative, flex, items_center, gap(2),
      h(8), my(px(1)), mx(2), px(2), rounded(lg), text_color(color),
      style(.bg = selected ? k->selected : CIVUE_CLEAR,
            .padding[3] = civue_px(thread ? 28 : 8)),
      hover(bg(selected ? k->selected : k->hover),
            text_color(selected || lit ? k->strong : k->text)),
      cursor_pointer) {
    civue_button_state state = button();

    // threads stay where their channel is.
    if (!thread) {
      channel_drag_row(app, guild, c, here(), &state.response);
    }
    else if (state.response.right_clicked) {
      channel_menu_open(app, c->id, civue_ui_current()->input.mouse);
    }

    if (state.activated && !channel_dragging(app)) {
      // a voice channel is joined and shows its call.
      if (channel_is_voice(c)) {
        if (store_can(store, c, PERM_CONNECT)) {
          voice_join(app, c->id);
        }

        app->voice_chat = false;
        app_select_channel(app, c->id);
      }
      else if (channel_has_text(c) || channel_is_forum(c) ||
               c->type == CHANNEL_GUILD_DIRECTORY) {
        app_select_channel(app, c->id);
      }
    }

    if (lit && !selected) {
      unread_mark(k);
    }

    civue_node_icon(civue_ui_current(), channel_icon(c),
                    &CIVUE_NODE_STYLE(.w = civue_px(thread ? 16 : 20),
                                      .h = civue_px(thread ? 16 : 20),
                                      .shrink = 0, .has_shrink = true));
    text(c->name ? c->name : "", flex_1, min_w(0), truncate,
         font_weight(medium));
    mention_badge(k, unread.mentions);
  }
}

void channel_list(App *app, Guild *guild) {
  Store *store = app->session->store;
  size_t count = 0;
  const ChannelRow *rows = store_guild_channels(store, guild, &count);
  bool hides = ids_has(&app->hide_muted, guild->id);
  bool folded = false;

  div(key(id("channels")), flex_1, min_h(0), overflow_y_scroll,
      scrollbars(&app->thin_bars), pb(4)) {
    // a right click where no row is offers to make channels.
    if (button().response.right_clicked) {
      channel_menu_open(app, 0, civue_ui_current()->input.mouse);
    }

    guild_banner(app, guild);

    if (guild->hub) {
      hub_rows(app, guild);
    }

    for (size_t i = 0; i < count; i++) {
      Channel *c = rows[i].channel;

      // a hub's directory is its "Join Servers" row.
      if (guild->hub && c->type == CHANNEL_GUILD_DIRECTORY) {
        continue;
      }

      if (rows[i].kind == CHANNEL_ROW_CATEGORY) {
        folded = c->collapsed;
        category_row(app, guild, c);
        continue;
      }

      // a folded category still shows the channel open and the unread ones.
      if (folded && app->session->selected_channel_id != c->id) {
        Unread unread = store_channel_unread(store, c);

        if (!unread.mentions && !(unread.unread && !unread.muted)) {
          continue;
        }
      }

      // a muted channel still shows while it is open or mentions the user.
      if (hides && app->session->selected_channel_id != c->id) {
        Unread unread = store_channel_unread(store, c);

        if (unread.muted && !unread.mentions) {
          continue;
        }
      }

      channel_row(app, guild, c, rows[i].kind == CHANNEL_ROW_THREAD);

      if (channel_is_voice(c)) {
        voice_members(app, c);
      }
    }

    channel_drag_end(app, guild);
  }
}

constexpr float PRIVATE_ROW = 44;

// how long ago the channel last spoke, as short as "now", "5m", "3h" or
// "2d". empty for a channel without a message.
static void format_ago(const Channel *c, char *out, size_t cap) {
  out[0] = 0;

  if (!c->last_message_id) {
    return;
  }

  int64_t seconds =
    (int64_t)time(nullptr) - snowflake_time_ms(c->last_message_id) / 1000;

  if (seconds < 60) {
    snprintf(out, cap, "now");
  }
  else if (seconds < 3600) {
    snprintf(out, cap, "%lldm", (long long)(seconds / 60));
  }
  else if (seconds < 86400) {
    snprintf(out, cap, "%lldh", (long long)(seconds / 3600));
  }
  else if (seconds < 86400 * 30) {
    snprintf(out, cap, "%lldd", (long long)(seconds / 86400));
  }
  else if (seconds < 86400 * 365) {
    snprintf(out, cap, "%lldmo", (long long)(seconds / (86400 * 30)));
  }
  else {
    snprintf(out, cap, "%lldy", (long long)(seconds / (86400 * 365)));
  }
}

static const User *dm_user(const Store *store, const Channel *c) {
  return c->type == CHANNEL_DM && c->nrecipients
           ? store_user(store, c->recipients[0])
           : nullptr;
}

static bool private_matches(App *app, const Channel *c) {
  const char *search = app->dm_search;

  if (!search || !*search) {
    return true;
  }

  char name[128];
  const User *user = dm_user(app->session->store, c);

  store_channel_name(app->session->store, c, name, sizeof name);

  return strcasestr(name, search) ||
         (user && user->username && strcasestr(user->username, search));
}

// under the name: what the user is doing, else the status they wrote. the
// members' count for a group.
static void private_subtitle(App *app, const Channel *c, const User *user) {
  const Look *k = &app->look;
  char made[256];

  if (c->type == CHANNEL_GROUP_DM) {
    snprintf(made, sizeof made, "%zu members", c->nrecipients + 1);
    text(made, truncate, text_size(px(12)), line_height(px(16)),
         text_color(k->faint));
    return;
  }

  if (!user || user->status == STATUS_OFFLINE) {
    return;
  }

  if (user->nactivities) {
    const Activity *doing = &user->activities[0];

    div(flex, items_center, gap(1), min_w(0), text_size(px(12)),
        line_height(px(16)), text_color(k->faint)) {
      glyph(activity_icon(doing->type), 12, k->online);
      text(activity_status_text(doing, made, sizeof made), min_w(0), truncate);
    }
  }
  else if (user->activity && user->custom_status) {
    text(user->activity, truncate, text_size(px(12)), line_height(px(16)),
         text_color(k->faint));
  }
}

static void private_row(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool selected = app->session->selected_channel_id == c->id;
  Unread unread = store_channel_unread(store, c);
  bool lit = unread.unread && !unread.muted;
  const User *user = dm_user(store, c);
  char name[128], ago[16];

  store_channel_name(store, c, name, sizeof name);
  format_ago(c, ago, sizeof ago);

  div(key(id_at(id("private"), c->id)), flex, items_center, gap(3),
      flex_shrink_0, h(px(PRIVATE_ROW)), mx(2), px(2), rounded(lg),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover)), cursor_pointer) {
    if (button().activated) {
      app_select_channel(app, c->id);
    }

    if (c->type == CHANNEL_GROUP_DM) {
      char url[CDN_URL_MAX];
      cdn_channel_icon(url, c->id, c->icon, 64);

      div(flex, items_center, justify_center, size(8), flex_shrink_0,
          rounded(full), bg(k->brand), text_color(CIVUE_WHITE)) {
        if (!*url || !image_fill(app, url, 16)) {
          icon(lucide, users, size(4));
        }
      }
    }
    else {
      avatar(app, user, 0, 32, true, k->frame);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      if (lit) {
        text(name, truncate, text_size(px(14)), line_height(px(18)),
             font_weight(semibold), text_color(k->strong));
      }
      else {
        text(name, truncate, text_size(px(14)), line_height(px(18)),
             font_weight(medium), text_color(selected ? k->strong : k->subtle));
      }
      private_subtitle(app, c, user);
    }

    if (unread.mentions) {
      mention_badge(k, unread.mentions);
    }
    else if (lit) {
      div(size(2), flex_shrink_0, rounded(full), bg(k->strong)) {}
    }
    else if (*ago) {
      text(ago, flex_shrink_0, text_size(px(12)), line_height(px(16)),
           text_color(k->faint));
    }
  }
}

void private_list(App *app) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  size_t count = 0, shown = 0;
  Channel *const *channels = store_private_channels(store, &count);
  bool searching = app->dm_search && *app->dm_search;
  Channel *first = nullptr;
  bool submitted = false;
  // the conversations listed: all of them, or the ones a search finds.
  Channel *const *listed = channels;
  Channel **found = nullptr;

  defer free(found);

  if (searching) {
    found = count ? malloc(count * sizeof *found) : nullptr;

    for (size_t i = 0; found != nullptr && i < count; i++) {
      if (private_matches(app, channels[i])) {
        found[shown++] = channels[i];
      }
    }

    listed = found;
  }
  else {
    shown = count;
  }

  if (shown) {
    first = listed[0];
  }

  div(flex, items_center, gap(1), flex_shrink_0, mx(2), mb(2)) {
    civue_id id = id("new-dm");
    bool open = app->popover == POPOVER_NEW_DM;
    civue_rect anchor = {};

    div(key(id("dm-search")), flex_1, min_w(0), h(8), text_size(px(14)),
        line_height(px(18))) {
      civue_field_look look = field_look(k);

      look.pad = 10;
      submitted =
        text_field(&app->dm_search, "Search conversations", look).submitted;
    }

    div(key(id), flex, items_center, justify_center, size(8), flex_shrink_0,
        rounded(lg), text_color(open ? k->strong : k->subtle),
        style(.bg = open ? k->hover : CIVUE_CLEAR),
        hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
      civue_button_state state = button();

      anchor = here();

      if (state.activated) {
        app_toggle(app, POPOVER_NEW_DM);
        app->picks.len = 0;
        free(app->pick_search);
        app->pick_search = nullptr;
      }

      icon(lucide, plus, size(px(18)));

      if (!open) {
        hint(app, id, &state.response, "New Message", CIVUE_TOOLTIP_BELOW);
      }
    }

    new_dm(app, anchor);
  }

  div(key(id("privates")), flex, flex_col, gap(px(2)), flex_1, min_h(0),
      overflow_y_scroll, scrollbars(&app->thin_bars), pb(3)) {
    if (searching) {
      text("Results", flex_shrink_0, px(4), mb(1), text_size(px(12)),
           line_height(px(16)), font_weight(semibold), text_color(k->faint));
    }

    // a row is built for the conversations in view alone, however many
    // there are. each holds the gap to the next.
    rows((uint32_t)shown, PRIVATE_ROW + 2, i) {
      private_row(app, listed[i]);
    }

    if (!shown) {
      text(searching ? "No conversation matches." : "No conversations yet.",
           px(4), py(2), text_size(px(14)), line_height(px(18)),
           text_color(k->faint));
    }
  }

  // enter opens the first result.
  if (submitted && searching && first) {
    free(app->dm_search);
    app->dm_search = nullptr;
    app_select_channel(app, first->id);
  }
}
