#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "server.h"
#include "widgets.h"

civue_icon channel_icon(const Channel *c) {
  switch (c->type) {
    case CHANNEL_GUILD_VOICE:
      return CIVUE_ICON(mingcute, volume_fill);
    case CHANNEL_GUILD_STAGE:
      return CIVUE_ICON(mingcute, base_station_fill);
    case CHANNEL_GUILD_ANNOUNCEMENT:
      return CIVUE_ICON(mingcute, announcement_fill);
    case CHANNEL_GUILD_FORUM:
      return CIVUE_ICON(mingcute, messages_square_fill);
    case CHANNEL_GUILD_MEDIA:
      return CIVUE_ICON(mingcute, pic_fill);
    case CHANNEL_GUILD_DIRECTORY:
      return CIVUE_ICON(mingcute, compass_fill);
    case CHANNEL_ANNOUNCEMENT_THREAD:
    case CHANNEL_PUBLIC_THREAD:
    case CHANNEL_PRIVATE_THREAD:
      return CIVUE_ICON(mingcute, message_2_fill);
    default:
      return CIVUE_ICON(mingcute, hashtag_fill);
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
      icon(mingcute, right_fill, size(3), flex_shrink_0);
    }
    else {
      icon(mingcute, down_fill, size(3), flex_shrink_0);
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
        icon(mingcute, add_fill, size(4));
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
                      : unread.muted  ? k->dim
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
          rounded(full), bg(k->primary), text_color(k->on_primary)) {
        if (!*url || !image_fill(app, url, 16)) {
          icon(mingcute, group_fill, size(4));
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
  size_t count = 0;
  Channel *const *channels =
    store_private_channels(app->session->store, &count);

  div(key(id("privates")), flex, flex_col, gap(px(2)), flex_1, min_h(0),
      overflow_y_scroll, scrollbars(&app->thin_bars), pb(3)) {
    // a row is built for the conversations in view alone, however many
    // there are. each holds the gap to the next.
    rows((uint32_t)count, PRIVATE_ROW + 2, i) {
      private_row(app, channels[i]);
    }

    if (!count) {
      text("No conversations yet.", px(4), py(2), text_size(px(14)),
           line_height(px(18)), text_color(k->faint));
    }
  }
}

static size_t friends_online(const Store *store) {
  size_t online = 0;

  for (size_t i = 0; i < store->nrelationships; i++) {
    const Relationship *r = &store->relationships[i];
    const User *user = store_user(store, r->user_id);

    online +=
      r->type == RELATIONSHIP_FRIEND && user && user->status != STATUS_OFFLINE;
  }

  return online;
}

// over the conversations, where discord has it too.
static void friends_row(App *app) {
  const Look *k = &app->look;
  bool selected = !app->session->selected_channel_id && !app->exploring;
  char online[32];

  snprintf(online, sizeof online, "%zu online",
           friends_online(app->session->store));

  div(key(id("friends")), flex, items_center, gap(3), flex_shrink_0, h(10),
      mx(2), px(2), rounded(lg), text_size(px(14)), line_height(px(18)),
      text_color(selected ? k->strong : k->subtle),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    if (button().activated) {
      app_select_channel(app, 0);
    }

    div(flex, items_center, justify_center, size(8), flex_shrink_0) {
      icon(mingcute, group_fill, size(5));
    }

    text("Friends", flex_1, min_w(0), truncate, font_weight(semibold));
    text(online, flex_shrink_0, text_size(px(12)), line_height(px(16)),
         text_color(k->faint));
  }
}

// the direct messages' name, how many there are, and a new conversation.
static void messages_head(App *app) {
  const Look *k = &app->look;
  civue_id id = id("new-dm");
  bool open = app->popover == POPOVER_NEW_DM;
  size_t count = 0;
  civue_rect anchor = {};
  char places[24];

  store_private_channels(app->session->store, &count);
  snprintf(places, sizeof places, "%zu", count);

  div(flex, items_center, gap(2), flex_shrink_0, h(12), pl(3), pr(2)) {
    window_handle(id("messages-head"));

    div(flex, items_center, justify_center, size(6), flex_shrink_0,
        rounded(md), bg(civue_alpha(k->primary, .2f))) {
      glyph(CIVUE_ICON(ph, chat_circle_fill), 14, k->primary);
    }

    text("Messages", min_w(0), truncate, text_size(px(15)),
         line_height(px(20)), font_weight(semibold), text_color(k->strong));
    text(places, flex_shrink_0, text_size(px(12)), line_height(px(16)),
         font_weight(medium), text_color(k->faint));
    div(flex_1) {}

    div(key(id), flex, items_center, justify_center, size(7), flex_shrink_0,
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

      icon(mingcute, add_fill, size(px(18)));

      if (!open) {
        hint(app, id, &state.response, "New Message", CIVUE_TOOLTIP_BELOW);
      }
    }
  }

  new_dm(app, anchor);
}

// discord's badge of a student hub, a cap on a grey disc.
static void hub_badge(App *app) {
  const Look *k = &app->look;
  civue_id id = id("hub-badge");

  div(key(id), flex, items_center, justify_center, size(4), flex_shrink_0,
      rounded(full), bg(k->border_strong)) {
    civue_response hovered = civue_interact(civue_ui_current(), id, here(),
                                            CIVUE_SENSE_HOVER);

    glyph(CIVUE_ICON(mingcute, mortarboard_fill), 11, k->strong);
    hint(app, id, &hovered, "Student Hub", CIVUE_TOOLTIP_BELOW);
  }
}

// the open guild's name, pressed for what can be done with it as discord's.
static void guild_head(App *app, Guild *guild) {
  const Look *k = &app->look;
  bool open =
    app->popover == POPOVER_GUILD_MENU || app->popover == POPOVER_INVITE;
  civue_rect anchor = {};

  div(flex, items_center, flex_shrink_0, h(12), px(1)) {
    window_handle(id("guild-head-bar"));

    div(key(id("guild-head")), flex, items_center, gap(2), flex_1, min_w(0),
        h(10), pl(2), pr(2), rounded(lg),
        style(.bg = open ? k->hover : CIVUE_CLEAR), hover(bg(k->hover)),
        cursor_pointer) {
      anchor = here();

      if (button().activated) {
        app->popover = open ? POPOVER_NONE : POPOVER_GUILD_MENU;
      }

      div(flex, items_center, justify_center, size(6), flex_shrink_0,
          rounded(md), overflow_hidden) {
        guild_icon(app, guild, 24, false);
      }

      text(guild->name ? guild->name : "", min_w(0), truncate,
           text_size(px(15)), line_height(px(20)), font_weight(semibold),
           text_color(k->strong));

      if (guild->hub) {
        hub_badge(app);
      }

      div(flex_1) {}

      if (open) {
        icon(mingcute, close_fill, size(px(14)), flex_shrink_0,
             text_color(k->subtle));
      }
      else {
        icon(mingcute, down_fill, size(px(14)), flex_shrink_0,
             text_color(k->faint));
      }
    }
  }

  guild_menu(app, anchor);
  invite_card(app, anchor);
}

// what looks like a search opens the launcher, kept to the guild open.
static void jump_button(App *app, Snowflake guild_id) {
  const Look *k = &app->look;

  div(flex_shrink_0, px(2), pb(2)) {
    div(key(id("jump")), flex, items_center, gap(2), h(8), pl(2), pr(1),
        rounded(lg), bg(k->field), border(1), border_color(k->border),
        text_size(px(14)), line_height(px(18)), text_color(k->faint),
        hover(bg(k->hover), text_color(k->text)), cursor_pointer) {
      if (button().activated) {
        launcher_show(app, guild_id);
      }

      icon(mingcute, search_2_fill, size(px(14)), flex_shrink_0);
      text(guild_id ? "Jump to..." : "Find a conversation", flex_1, min_w(0),
           truncate);
      keycap(k, true, "K");
    }
  }
}

void sidebar_card(App *app) {
  const Look *k = &app->look;
  Guild *guild = app_guild(app);

  div(flex, flex_col, gap(2), flex_1, min_w(0), h(full), py(2), pr(2)) {
    div(key(id("sidebar-card")), relative, flex, flex_col, flex_1, min_h(0),
        rounded(px(PANEL_RADIUS)), bg(k->chat), border(1),
        border_color(k->frame_border), overflow_hidden) {
      if (guild) {
        guild_head(app, guild);
        jump_button(app, guild->id);
        channel_list(app, guild);
      }
      else if (app->exploring) {
        explore_nav(app);
      }
      else {
        messages_head(app);
        jump_button(app, 0);
        friends_row(app);
        text("Direct Messages", flex_shrink_0, mt(3), mb(1), px(4),
             text_size(px(12)), line_height(px(16)), font_weight(medium),
             text_color(k->faint));
        private_list(app);
      }

      link_toast(app);
    }

    voice_panel(app);
    user_panel(app);
  }
}
