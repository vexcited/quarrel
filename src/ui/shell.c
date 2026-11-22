#include "components.h"

#include <math.h>
#include <stdio.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float SWITCH_ICON = 20;

// mentions waiting anywhere but in what the sidebar shows.
static uint32_t mentions_elsewhere(App *app) {
  Store *store = app->session->store;
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);
  uint32_t total = app->guild_id ? store_private_unread(store).mentions : 0;

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      Guild *guild = folders[i].guilds[j];

      if (guild->id != app->guild_id) {
        total += store_guild_unread(store, guild).mentions;
      }
    }
  }

  return total;
}

// a red dot at the corner of the open node.
static void mention_dot(const Look *k) {
  civue_rect r = here();

  fill(xywh(r.x1 - 5, r.y0 - 3, 10, 10), k->frame, 5);
  fill(xywh(r.x1 - 3, r.y0 - 1, 6, 6), k->danger, 3);
}

static void account_switch(App *app) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const char *name = store_user_name(store->self);
  civue_id id = id("account-switch");
  civue_rect anchor = {};

  div(key(id), flex, items_center, gap(2), min_w(0), h(7), pl(1), pr(1p5),
      rounded(lg),
      style(.bg = app->popover == POPOVER_ACCOUNTS ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover)), cursor_pointer) {
    civue_button_state state = button();

    anchor = here();

    if (state.activated) {
      app_toggle(app, POPOVER_ACCOUNTS);
    }

    div(relative, flex_shrink_0) {
      avatar(app, store->self, 0, SWITCH_ICON, false, k->frame);

      civue_rect r = here();
      status_dot(store->self_status, xywh(r.x1 - 7, r.y1 - 7, 9, 9), k,
                 k->frame);

      // another account has something waiting.
      if (accounts_waiting(app)) {
        mention_dot(k);
      }
    }

    text(name, min_w(0), truncate, text_color(k->strong));
    icon(lucide, chevron_down, size(px(14)), flex_shrink_0,
         text_color(k->faint));

    if (app->popover != POPOVER_ACCOUNTS) {
      hint(app, id, &state.response, name, CIVUE_TOOLTIP_BELOW);
    }
  }

  account_switcher(app, anchor);
}

// discord's badge of a student hub, a cap on a grey disc.
static void hub_badge(App *app) {
  const Look *k = &app->look;
  civue_id id = id("hub-badge");

  div(key(id), flex, items_center, justify_center, size(4), flex_shrink_0,
      rounded(full), bg(k->border_strong)) {
    civue_response hovered = civue_interact(civue_ui_current(), id, here(),
                                            CIVUE_SENSE_HOVER);

    glyph(CIVUE_ICON(lucide, graduation_cap), 11, CIVUE_WHITE);
    hint(app, id, &hovered, "Student Hub", CIVUE_TOOLTIP_BELOW);
  }
}

static void guild_switch(App *app) {
  const Look *k = &app->look;
  const Guild *guild = app_guild(app);
  const char *name = guild ? guild->name ? guild->name : "" : "Direct Messages";
  bool mentioned = mentions_elsewhere(app) > 0;
  civue_rect anchor = {};

  div(key(id("guild-switch")), flex, items_center, gap(2), flex_1, min_w(0),
      h(9), pl(2), pr(2), rounded(lg), bg(k->selected), hover(bg(k->hover)),
      cursor_pointer) {
    anchor = here();

    if (button().activated) {
      app_toggle(app, POPOVER_GUILDS);
      app->guilds_fresh = GUILDS_SETTLE;
    }

    div(relative, flex, items_center, justify_center, size(px(SWITCH_ICON)),
        flex_shrink_0) {
      if (guild) {
        div(flex, items_center, justify_center, size(full), rounded(md),
            overflow_hidden) {
          guild_icon(app, guild, SWITCH_ICON, false);
        }
      }
      else {
        icon(lucide, message_circle, size(px(18)), text_color(k->subtle));
      }

      if (mentioned) {
        mention_dot(k);
      }
    }

    text(name, flex_1, min_w(0), truncate, font_weight(semibold),
         text_color(k->strong));

    if (guild && guild->hub) {
      hub_badge(app);
    }

    icon(lucide, chevrons_up_down, size(px(14)), flex_shrink_0,
         text_color(k->faint));
  }

  guild_switcher(app, anchor);
  new_guild(app, anchor);
}

static void inbox_button(App *app) {
  const Look *k = &app->look;
  civue_id id = id("inbox-button");
  civue_rect anchor = {};

  div(key(id), relative, flex, items_center, justify_center, size(7),
      flex_shrink_0, rounded(lg),
      text_color(app->popover == POPOVER_INBOX ? k->strong : k->subtle),
      style(.bg = app->popover == POPOVER_INBOX ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
    civue_button_state state = button();

    anchor = here();

    if (state.activated) {
      app_toggle(app, POPOVER_INBOX);
      app->inbox_asked = false;
    }

    div(relative, flex, items_center, justify_center, size(px(18))) {
      icon(lucide, inbox, size(px(18)));

      if (mentions_of(app->session->store)) {
        mention_dot(k);
      }
    }

    if (app->popover != POPOVER_INBOX) {
      hint(app, id, &state.response, "Inbox", CIVUE_TOOLTIP_BELOW);
    }
  }

  inbox(app, anchor);
}

// shown while what the window holds may be late: the connection to discord
// is being made, dropped, or there is no network at all.
// seconds for one pulse of the link icon.
constexpr double PULSE = 1.6;

static void link_state(App *app) {
  const Look *k = &app->look;
  civue_id id = id("link-state");
  SessionLink link = session_link(app->session);
  bool online = platform_online();

  if (link == SESSION_LINK_UP && online) {
    return;
  }

  bool down = !online || link == SESSION_LINK_DOWN;
  const char *why = !online ? "No internet connection"
                    : down  ? "Disconnected, trying again soon"
                            : "Connecting to Discord...";

  civue_color color = down ? k->critical_text : k->faint;

  // an attempt under way breathes, a slow fade out and back in.
  if (!down) {
    civue_ui *ui = civue_ui_current();

    color.a *= .65f + .35f * (float)cos(ui->now * 2 * M_PI / PULSE);
    civue_request_repaint_after(ui, 1.0 / 30);
  }

  div(key(id), flex, items_center, justify_center, size(7), flex_shrink_0,
      rounded(lg), text_color(color)) {
    civue_button_state state = button();

    if (down) {
      icon(lucide, unlink, size(px(18)));
    }
    else {
      icon(lucide, link, size(px(18)));
    }

    hint(app, id, &state.response, why, CIVUE_TOOLTIP_BELOW);
  }
}

static void settings_button(App *app) {
  const Look *k = &app->look;
  civue_id id = id("settings-button");

  div(key(id), flex, items_center, justify_center, size(7), flex_shrink_0,
      rounded(lg), text_color(k->subtle),
      hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
    civue_button_state state = button();

    if (state.activated) {
      app->popover = POPOVER_NONE;
      app->settings_open = true;
      app->settings_part = 0;
      app->settings_select = -1;
      devices_list(app);
    }

    icon(lucide, settings, size(px(18)));
    hint(app, id, &state.response, "Settings", CIVUE_TOOLTIP_BELOW);
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

static void friends_button(App *app) {
  const Look *k = &app->look;
  bool selected = !app->session->selected_channel_id;
  civue_id id = id("friends");
  char label[40];

  snprintf(label, sizeof label, "Friends, %zu online",
           friends_online(app->session->store));

  div(key(id), flex, items_center, justify_center, w(8), h(9), flex_shrink_0,
      rounded(lg), text_color(selected ? k->strong : k->subtle),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    civue_button_state state = button();

    if (state.activated) {
      app_select_channel(app, 0);
    }

    icon(lucide, users, size(px(18)));
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_BELOW);
  }
}

// a button of the place bar that opens `popover` under itself. true when
// pressed, its rect in `anchor`.
static bool bar_button(App *app, civue_id id, civue_icon glyph_icon,
                       const char *label, Popover popover, civue_rect *anchor) {
  const Look *k = &app->look;
  bool open = app->popover == popover;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, w(8), h(9), flex_shrink_0,
      rounded(lg), text_color(open ? k->strong : k->subtle),
      style(.bg = open ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
    civue_button_state state = button();

    *anchor = here();
    pressed = state.activated;

    if (pressed) {
      app_toggle(app, popover);
    }

    glyph(glyph_icon, 18, open ? k->strong : k->subtle);

    if (!open) {
      hint(app, id, &state.response, label, CIVUE_TOOLTIP_BELOW);
    }
  }

  return pressed;
}

// the invite's link opens under the menu that asked for it.
static void guild_buttons(App *app) {
  civue_rect anchor = {};

  bar_button(app, id("guild-menu"), CIVUE_ICON(lucide, ellipsis), "Server Menu",
             POPOVER_GUILD_MENU, &anchor);
  guild_menu(app, anchor);
  invite_card(app, anchor);
}

void place_bar(App *app) {
  const Look *k = &app->look;
  const Guild *guild = app_guild(app);

  div(flex, items_center, gap(1), flex_shrink_0, mx(2), mt(1), mb(2),
      text_size(px(14)), line_height(px(18)), text_color(k->text)) {
    guild_switch(app);

    if (guild) {
      guild_buttons(app);
    }
    else {
      friends_button(app);
    }
  }
}

void top_bar(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();

  div(flex, items_center, flex_shrink_0, w(full), h(px(TOP_BAR_HEIGHT)), pr(2),
      text_size(px(14)), line_height(px(18)), font_weight(medium),
      text_color(k->text),
      style(.padding[3] = civue_px(ui->titlebar_inset + 4))) {
    window_handle(id("title-bar"));

    account_switch(app);
    div(flex_1) {}
    link_state(app);
    inbox_button(app);
    settings_button(app);
  }
}

// how long a notice stays, in seconds. it sits 16 points over the foot of
// the window, as discord's toasts do.
constexpr double NOTICE_SECONDS = 3;

static void on_notice_end(void *ctx) {
  App *app = ctx;

  app->notice_timer = nullptr;
  civue_window_wake();
}

void notice_toast(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  double now = platform_now();

  // a notice of another account, or older than the one shown, is not news.
  if (app->notice_session != session) {
    app->notice_session = session;
    app->notice_seen = session->notice_version;
    app->notice_until = 0;
  }
  else if (app->notice_seen != session->notice_version) {
    app->notice_seen = session->notice_version;
    app->notice_until = now + NOTICE_SECONDS;
    platform_timer_cancel(app->notice_timer);
    app->notice_timer = platform_timer(NOTICE_SECONDS, on_notice_end, app);
  }

  if (!session->notice || now >= app->notice_until) {
    return;
  }

  civue_vec2 window = civue_ui_current()->size;
  float width = 440;

  overlay() {
    div(key(id("notice")), flex, justify_center,
        left(px(window.x / 2 - width / 2)), top(px(window.y - 16 - 36)),
        w(px(width))) {
      div(flex, items_center, gap(2), max_w(full), px(3), py(2), rounded(lg),
          bg(k->surface_high), border(1), border_color(k->border_strong),
          shadow(lg), text_size(px(14)), line_height(px(18)),
          font_weight(medium), text_color(k->strong)) {
        if (session->notice_failed) {
          icon(lucide, circle_alert, size(4), flex_shrink_0,
               text_color(k->critical_text));
        }
        else {
          icon(lucide, bookmark, size(4), flex_shrink_0,
               text_color(k->subtle));
        }

        text(session->notice, min_w(0));
      }
    }
  }
}
