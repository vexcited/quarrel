#include "components.h"

#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
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

static void category_row(App *app, Channel *category) {
  const Look *k = &app->look;

  div(key(id_at(id("category"), category->id)), flex, items_center, gap(1),
      h(8), mt(3), px(2), mx(2), text_color(k->faint),
      hover(text_color(k->text)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), cursor_pointer) {
    if (button().activated) {
      category->collapsed = !category->collapsed;
    }

    text(category->name ? category->name : "", truncate, min_w(0));

    if (category->collapsed) {
      icon(lucide, chevron_right, size(3), flex_shrink_0);
    }
    else {
      icon(lucide, chevron_down, size(3), flex_shrink_0);
    }
  }
}

static void channel_row(App *app, Channel *c, bool thread) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool selected = app->session->selected_channel_id == c->id;
  Unread unread = store_channel_unread(store, c);
  bool lit = unread.unread && !unread.muted;
  civue_color color = selected || lit ? k->strong
                      : unread.muted  ? rgb(0x4f505a)
                                      : k->faint;

  div(key(id_at(id("channel"), c->id)), relative, flex, items_center, gap(2),
      h(8), my(px(1)), mx(2), px(2), rounded(lg), text_color(color),
      style(.bg = selected ? k->selected : CIVUE_CLEAR,
            .padding[3] = civue_px(thread ? 28 : 8)),
      hover(bg(selected ? k->selected : k->hover),
            text_color(selected || lit ? k->strong : k->text)),
      cursor_pointer) {
    if (button().activated && channel_has_text(c)) {
      app_select_channel(app, c->id);
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

static void guild_header(App *app, const Guild *guild) {
  const Look *k = &app->look;

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->frame_border)) {
    text(guild->name ? guild->name : "", flex_1, min_w(0), truncate,
         font_weight(semibold), text_color(k->strong));
    icon(lucide, chevron_down, size(4), text_color(k->subtle));
  }
}

void channel_list(App *app, Guild *guild) {
  Store *store = app->session->store;
  size_t count = 0;
  const ChannelRow *rows = store_guild_channels(store, guild, &count);
  bool folded = false;

  guild_header(app, guild);

  div(key(id("channels")), flex_1, min_h(0), overflow_y_scroll,
      scrollbars(&app->thin_bars), pt(1), pb(20)) {
    for (size_t i = 0; i < count; i++) {
      Channel *c = rows[i].channel;

      if (rows[i].kind == CHANNEL_ROW_CATEGORY) {
        folded = c->collapsed;
        category_row(app, c);
        continue;
      }

      // a folded category still shows the channel open and the unread ones.
      if (folded && app->session->selected_channel_id != c->id) {
        Unread unread = store_channel_unread(store, c);

        if (!unread.mentions && !(unread.unread && !unread.muted)) {
          continue;
        }
      }

      channel_row(app, c, rows[i].kind == CHANNEL_ROW_THREAD);
    }
  }
}

static void private_row(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool selected = app->session->selected_channel_id == c->id;
  Unread unread = store_channel_unread(store, c);
  bool lit = unread.unread && !unread.muted;
  char name[128], members[32];
  const User *user = c->type == CHANNEL_DM && c->nrecipients
                       ? store_user(store, c->recipients[0])
                       : nullptr;
  const char *subtitle = user ? user->activity : nullptr;

  store_channel_name(store, c, name, sizeof name);

  if (c->type == CHANNEL_GROUP_DM) {
    snprintf(members, sizeof members, "%zu Members", c->nrecipients + 1);
    subtitle = members;
  }

  div(key(id_at(id("private"), c->id)), relative, flex, items_center, gap(3),
      h(px(42)), my(px(1)), mx(2), px(2), rounded(lg),
      text_color(selected || lit ? k->strong : k->faint),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover), text_color(k->text)),
      cursor_pointer) {
    if (button().activated) {
      app_select_channel(app, c->id);
    }

    if (lit && !selected) {
      div(absolute, left(px(-8)), top(px(17)), w(1), h(2), rounded_r(px(4)),
          bg(k->strong)) {}
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
      text(name, truncate, font_weight(medium), line_height(px(20)));

      if (subtitle && *subtitle) {
        text(subtitle, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->faint));
      }
    }

    mention_badge(k, unread.mentions);
  }
}

void private_list(App *app) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  size_t count = 0;
  Channel *const *channels = store_private_channels(store, &count);
  bool friends = app->session->selected_channel_id == 0;

  div(flex, items_center, flex_shrink_0, h(px(HEADER_HEIGHT)), px(2),
      border_b(1), border_color(k->frame_border)) {
    div(flex, items_center, justify_center, flex_1, h(8), rounded(lg),
        bg(k->hover), text_size(px(14)), line_height(px(18)),
        font_weight(medium), text_color(k->subtle)) {
      text("Find or start a conversation", truncate);
    }
  }

  div(key(id("privates")), flex_1, min_h(0), overflow_y_scroll,
      scrollbars(&app->thin_bars), pt(2), pb(20)) {
    div(key(id("friends")), flex, items_center, gap(3), h(10), mx(2), px(3),
        rounded(lg), text_color(friends ? k->strong : k->faint),
        style(.bg = friends ? k->selected : CIVUE_CLEAR),
        hover(bg(friends ? k->selected : k->hover), text_color(k->text)),
        font_weight(medium), cursor_pointer) {
      if (button().activated) {
        app_select_channel(app, 0);
      }

      icon(lucide, users, size(5));
      text("Friends");
    }

    text("Direct Messages", px(4), mt(4), mb(1), text_size(px(14)),
         line_height(px(18)), font_weight(medium), text_color(k->faint));

    for (size_t i = 0; i < count; i++) {
      private_row(app, channels[i]);
    }
  }
}

static const char *status_name(const Store *store) {
  switch (store->self_status) {
    case STATUS_ONLINE:
      return "Online";
    case STATUS_IDLE:
      return "Idle";
    case STATUS_DND:
      return "Do Not Disturb";
    default:
      return "Invisible";
  }
}

void user_panel(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  Store *store = session->store;
  const User *self = store->self;
  civue_id id = id("status");
  civue_rect panel = {}, toggle = {};

  div(absolute, left(2), right(2), bottom(2), flex, items_center, gap(2),
      h(px(56)), px(2), rounded(lg), bg(k->panel), border(1),
      border_color(k->border)) {
    panel = here();

    div(key(id), flex, items_center, gap(2), flex_1, min_w(0), p(1),
        rounded(lg), hover(bg(k->hover)), cursor_pointer) {
      civue_button_state state = button();

      // each press goes to the next status.
      if (state.activated) {
        UserStatus next = store->self_status == STATUS_ONLINE ? STATUS_IDLE
                          : store->self_status == STATUS_IDLE ? STATUS_DND
                          : store->self_status == STATUS_DND  ? STATUS_OFFLINE
                                                              : STATUS_ONLINE;

        session_set_status(session, next, next == STATUS_OFFLINE);
      }

      div(relative, flex_shrink_0) {
        avatar(app, self, 0, 40, false, k->panel);

        civue_rect r = here();
        status_dot(store->self_status, xywh(r.x1 - 12, r.y1 - 12, 12, 12), k,
                   k->panel);
      }

      div(flex, flex_col, flex_1, min_w(0)) {
        text(store_user_name(self), truncate, text_size(px(14)),
             line_height(px(18)), font_weight(semibold), text_color(k->strong));
        text(status_name(store), truncate, text_size(px(12)),
             line_height(px(16)), text_color(k->subtle));
      }

      hint(app, id, &state.response, "Change status", CIVUE_TOOLTIP_ABOVE);
    }

    if (!session->offline) {
      div(flex_shrink_0) {
        toggle = here();

        if (icon_button(app, id("switch"), CIVUE_ICON(lucide, arrow_left_right),
                        32, 20,
                        app->accounts_open ? nullptr : "Switch Accounts")) {
          app->accounts_open = !app->accounts_open;
        }
      }
    }

    if (icon_button(app, id("logout"), CIVUE_ICON(lucide, log_out), 32, 20,
                    "Log Out")) {
      session_logout(session);
    }
  }

  account_switcher(app, panel, toggle);
}
