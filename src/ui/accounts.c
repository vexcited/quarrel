#include "components.h"

#include <stdcountof.h>
#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

constexpr float SWITCHER_WIDTH = 320;
constexpr float BANNER_HEIGHT = 84;
constexpr float PROFILE_AVATAR = 64;
constexpr float ROW_HEIGHT = 48;
constexpr float STATUS_HEIGHT = 32;

static const char *account_name(const SessionAccount *a) {
  return a->global_name ? a->global_name
         : a->username  ? a->username
                        : "Account";
}

static uint32_t account_mentions(const SessionAccount *a) {
  return a->session && a->session->state >= SESSION_READY
           ? mentions_of(a->session->store)
           : 0;
}

uint32_t accounts_waiting(App *app) {
  const Roster *roster = app->roster;
  uint32_t total = 0;

  for (size_t i = 0; i < roster->naccounts; i++) {
    if (roster->accounts[i].session != app->session) {
      total += account_mentions(&roster->accounts[i]);
    }
  }

  return total;
}

static bool account_row(App *app, const SessionAccount *a, bool current) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];
  bool pressed = false;

  cdn_avatar(url, a->id, a->avatar, a->discriminator, 64);

  div(key(id_at(id("account"), a->id)), flex, items_center, gap(3),
      flex_shrink_0, h(px(ROW_HEIGHT)), px(2), rounded(lg), hover(bg(k->hover)),
      cursor_pointer) {
    pressed = button().activated;

    div(size(8), flex_shrink_0, rounded(full), bg(k->surface_high)) {
      image_fill(app, url, 16);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      text(account_name(a), truncate, text_size(px(14)), line_height(px(18)),
           font_weight(semibold), text_color(k->strong));

      if (a->username) {
        text(a->username, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }
    }

    if (current) {
      icon(mingcute, check_fill, size(4), flex_shrink_0, text_color(k->positive));
    }
    else {
      mention_badge(k, account_mentions(a));
    }
  }

  return pressed;
}

// `lasts` for a status picked for a while: its row opens the durations.
static bool status_row(App *app, UserStatus status, const char *label,
                       bool lasts) {
  const Look *k = &app->look;
  bool current = app->session->store->self_status == status;
  bool listed = app->status_menu_open && app->status_menu == status;
  bool pressed = false;

  div(key(id_at(id("status"), status)), flex, items_center, gap(3),
      flex_shrink_0, h(px(STATUS_HEIGHT)), px(2), rounded(lg),
      text_color(k->text), style(.bg = listed ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), cursor_pointer) {
    pressed = button().activated;

    if (listed) {
      app->status_menu_row = here();
    }

    div(flex, items_center, justify_center, size(8), flex_shrink_0) {
      div(size(3)) {
        status_dot(status, here(), k, k->popover);
      }
    }

    text(label, flex_1);

    if (current) {
      icon(mingcute, check_fill, size(4), flex_shrink_0, text_color(k->positive));
    }

    if (lasts) {
      icon(mingcute, right_fill, size(4), flex_shrink_0,
           text_color(k->subtle));
    }
  }

  return pressed;
}

// how long the status of the open row is kept, beside the card. answers
// the seconds picked with 0 for good, negative while nothing is. `box` is
// where the list was drawn.
static int64_t status_durations(App *app, civue_rect card, civue_rect *box) {
  static const struct {
    const char *label;
    int64_t seconds;
  } spans[] = {
    {"For 15 Minutes", 900},  {"For 1 Hour", 3600},  {"For 8 Hours", 28800},
    {"For 24 Hours", 86400},  {"For 3 Days", 259200}, {"Forever", 0},
  };

  const Look *k = &app->look;
  int64_t picked = -1;

  overlay() {
    div(key(id("status-durations")), flex, flex_col, left(px(card.x1 + 4)),
        top(px(app->status_menu_row.y0 - 8)), w(px(184)), p(2), rounded(xl),
        bg(k->popover), border(1), border_color(k->border_strong), shadow(lg)) {
      *box = here();

      for (size_t i = 0; i < countof(spans); i++) {
        div(key(id_at(id("status-span"), i)), flex, items_center,
            flex_shrink_0, h(px(STATUS_HEIGHT)), px(3), rounded(lg),
            text_color(k->text), hover(bg(k->hover)), text_size(px(14)),
            line_height(px(18)), font_weight(medium), cursor_pointer) {
          if (button().activated) {
            picked = spans[i].seconds;
          }

          text(spans[i].label);
        }
      }
    }
  }

  return picked;
}

// what the account wrote as its status, in the bubble discord draws
// beside the avatar.
static void status_bubble(App *app, const User *self) {
  const Look *k = &app->look;

  if (!self->status_text && !self->status_emoji) {
    return;
  }

  div(absolute, left(px(PROFILE_AVATAR + 28)), top(px(-14)),
      max_w(px(SWITCHER_WIDTH - PROFILE_AVATAR - 80)), flex, items_start,
      gap(1p5), px(3), py(2), rounded(xl), bg(k->surface), border(1),
      border_color(k->border_strong)) {
    if (self->status_emoji) {
      emoji_node(app, self->status_emoji_id, self->status_emoji, 18);
    }

    if (self->status_text) {
      div(min_w(0), max_h(px(36)), overflow_hidden) {
        text(self->status_text, text_size(px(14)), line_height(px(18)),
             text_color(k->text));
      }
    }
  }
}

// what the profile wears, each with its name when pointed at.
static void badges(App *app) {
  const Look *k = &app->look;
  const Session *session = app->session;
  char url[CDN_URL_MAX];

  if (session->nbadges == 0) {
    return;
  }

  div(flex, items_center, flex_wrap, gap(1), self_start, mt(2), px(1p5),
      py(1), rounded(lg), border(1), border_color(k->border)) {
    for (size_t i = 0; i < session->nbadges; i++) {
      const SessionBadge *badge = &session->badges[i];
      civue_id id = id_at(id("profile-badge"), i);

      cdn_badge_icon(url, badge->icon);

      div(key(id), size(px(20)), flex_shrink_0) {
        civue_button_state state = button();

        image_fill(app, url, 0);

        if (badge->description) {
          hint(app, id, &state.response, badge->description,
               CIVUE_TOOLTIP_ABOVE);
        }
      }
    }
  }
}

static void separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}
}

static void profile_banner(App *app, const User *self) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX] = "";
  civue_color ground = self->has_accent ? rgb(self->accent_color) : k->banner;

  cdn_banner(url, self->id, self->banner, 640);

  div(flex_shrink_0, h(px(BANNER_HEIGHT)), bg(ground)) {
    image_cover(app, url, 0);
  }
}

// who the account is: its banner, its names and what it wrote of itself.
static void profile(App *app) {
  const Look *k = &app->look;
  const User *self = app->session->store->self;
  char digits[21];
  bool copy = false;

  if (!self) {
    return;
  }

  profile_banner(app, self);

  div(relative, flex, items_center, justify_end, flex_shrink_0, h(10), px(3)) {
    div(absolute, left(px(12)), top(px(-PROFILE_AVATAR / 2 - 4)), p(1),
        rounded(full), bg(k->popover)) {
      avatar(app, self, 0, PROFILE_AVATAR, true, k->popover);
    }

    status_bubble(app, self);

    copy = icon_button(app, id("copy-id"), CIVUE_ICON(mingcute, copy_2_fill), 28, 16,
                       "Copy User ID");
  }

  div(flex, flex_col, flex_shrink_0, px(4), pb(3)) {
    text(store_user_name(self), truncate, text_size(px(20)),
         line_height(px(24)), font_weight(bold), text_color(k->strong));

    div(flex, items_center, gap(1p5), min_w(0), text_size(px(14)),
        line_height(px(18)), text_color(k->subtle)) {
      text(self->username ? self->username : "", min_w(0), truncate);

      if (self->pronouns && *self->pronouns) {
        text("\u00b7", flex_shrink_0, text_color(k->faint));
        text(self->pronouns, flex_shrink_0);
      }

      tag_chip(app, self);
    }

    badges(app);

    if (self->bio && *self->bio) {
      div(mt(2), max_h(px(132)), overflow_hidden) {
        markup(app, id("profile-bio"), self->bio,
               &(MarkupOptions){.size = 14, .hint = SWITCHER_WIDTH - 32});
      }
    }
  }

  if (copy) {
    snowflake_format(self->id, digits);
    civue_clipboard_set_text(digits, strlen(digits));
  }
}

// one choice of the tag to wear: a guild's, or none for a null guild.
static bool tag_choice(App *app, const Guild *g, bool worn) {
  const Look *k = &app->look;
  bool pressed = false;
  char url[CDN_URL_MAX] = "";

  div(key(id_at(id("tag-choice"), g != nullptr ? g->id : 0)), flex,
      items_center, gap(2), flex_shrink_0, h(9), px(2), rounded(lg),
      hover(bg(k->hover)), active(bg(k->pressed)), cursor_pointer) {
    pressed = button().activated;

    if (g != nullptr) {
      cdn_tag_badge(url, g->id, g->tag_badge, 32);
      guild_tag_pill(app, g->tag, url);
      text(g->name ? g->name : "", flex_1, min_w(0), truncate,
           text_size(px(14)), line_height(px(18)), text_color(k->subtle));
    }
    else {
      text("No Server Tag", flex_1, min_w(0), truncate, text_size(px(14)),
           line_height(px(18)), text_color(k->text));
    }

    if (worn) {
      glyph(CIVUE_ICON(mingcute, check_fill), 16, k->primary);
    }
  }

  return pressed;
}

// the tags of the user's guilds, under the row that opens them. true once
// one was picked.
static bool tag_choices(App *app) {
  Session *session = app->session;
  const User *self = session->store->self;
  Snowflake worn = self != nullptr && self->tag ? self->tag_guild_id : 0;
  size_t count = 0, shown = 0;
  const GuildFolder *folders = store_guild_folders(session->store, &count);
  bool picked = false;

  div(key(id("tag-choices")), flex, flex_col, flex_shrink_0, pl(4),
      max_h(px(264)), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < count; i++) {
      for (size_t j = 0; j < folders[i].nguilds; j++) {
        const Guild *g = folders[i].guilds[j];

        if (g->tag == nullptr || !*g->tag) {
          continue;
        }

        shown++;

        if (tag_choice(app, g, worn == g->id) && worn != g->id) {
          session_wear_tag(session, g->id);
          picked = true;
        }
      }
    }

    if (shown == 0) {
      text("You don't have any Server Tags to choose from.", px(2), py(2),
           text_size(px(12)), line_height(px(16)),
           text_color(app->look.subtle));
    }
    else if (tag_choice(app, nullptr, worn == 0) && worn != 0) {
      session_wear_tag(session, 0);
      picked = true;
    }
  }

  return picked;
}

void account_switcher(App *app, civue_rect anchor) {
  if (app->popover != POPOVER_ACCOUNTS) {
    app->status_menu_open = false;
    app->status_menu_row = (civue_rect){};
    return;
  }

  static const struct {
    UserStatus status;
    const char *label;
  } statuses[] = {
    {STATUS_ONLINE, "Online"},
    {STATUS_IDLE, "Idle"},
    {STATUS_DND, "Do Not Disturb"},
    {STATUS_OFFLINE, "Invisible"},
  };

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  Roster *roster = app->roster;
  size_t count = roster->naccounts;
  bool can_add = count > 0;
  civue_rect card = {}, spans = {};
  Snowflake chosen = 0;
  bool add = false, leave = false;

  overlay_room();
  session_fetch_profile(session);

  overlay() {
    div(key(id("accounts")), flex, flex_col, left(px(anchor.x0)),
        top(px(anchor.y1 + 6)), w(px(SWITCHER_WIDTH)), rounded(xl),
        bg(k->popover), border(1), border_color(k->border_strong), shadow(lg),
        overflow_hidden) {
      card = here();
      profile(app);

      div(flex, flex_col, flex_shrink_0, p(2), border_t(1),
          border_color(k->border)) {
        for (size_t i = 0; i < count; i++) {
          const SessionAccount *a = &roster->accounts[i];

          if (account_row(app, a, a->session == session)) {
            chosen = a->id;
          }
        }

        if (can_add) {
          add = menu_row(app, id("account-add"), CIVUE_ICON(mingcute, add_fill),
                         "Add an account", k->text);
        }

        separator(k);

        for (size_t i = 0; i < countof(statuses); i++) {
          UserStatus status = statuses[i].status;

          bool lasts = status != STATUS_ONLINE;

          if (!status_row(app, status, statuses[i].label, lasts)) {
            continue;
          }

          if (lasts) {
            bool same = app->status_menu_open && app->status_menu == status;

            app->status_menu = status;
            app->status_menu_open = !same;
            civue_request_repaint(ui);
          }
          else {
            app->status_menu_open = false;
            session_set_status(session, status, false, 0);
          }
        }

        separator(k);

        if (menu_row(app, id("server-tag"), CIVUE_ICON(mingcute, tag_fill),
                     session->tag_wearing ? "Changing Server Tag..."
                                          : "Change Server Tag",
                     k->text)) {
          app->tag_menu_open = !app->tag_menu_open;
          civue_request_repaint(ui);
        }

        if (app->tag_menu_open && tag_choices(app)) {
          app->tag_menu_open = false;
        }

        if (session->tag_error != nullptr) {
          text(session->tag_error, px(2), text_size(px(12)),
               line_height(px(16)), text_color(k->critical_text));
        }

        separator(k);

        leave = menu_row(app, id("logout"), CIVUE_ICON(mingcute, exit_fill),
                         "Log Out", k->critical_text);
      }
    }
  }

  // the row is known a frame after it opens its list.
  if (app->status_menu_open && civue_rect_h(app->status_menu_row) > 0) {
    int64_t seconds = status_durations(app, card, &spans);

    if (seconds >= 0) {
      session_set_status(session, app->status_menu,
                         app->status_menu == STATUS_OFFLINE, seconds);
      app->status_menu_open = false;
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(spans, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }

  if (app->popover != POPOVER_ACCOUNTS) {
    app->status_menu_open = false;
    app->status_menu_row = (civue_rect){};
    app->tag_menu_open = false;
  }

  // the roster changes last: the rows were read from it. the frame ends with
  // the session it started with.
  if (chosen) {
    app->popover = POPOVER_NONE;
    roster_switch(roster, chosen);
  }
  else if (add) {
    app->popover = POPOVER_NONE;
    roster_add(roster);
  }
  else if (leave) {
    app->popover = POPOVER_NONE;
    roster_logout(roster);
  }
}

void account_choices(App *app) {
  const Look *k = &app->look;
  Roster *roster = app->roster;
  Snowflake chosen = 0;

  if (roster->naccounts == 0) {
    return;
  }

  div(flex, flex_col, mt(5), pt(4), border_t(1), border_color(k->border)) {
    text("Or continue as", mb(2), text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));

    for (size_t i = 0; i < roster->naccounts; i++) {
      if (account_row(app, &roster->accounts[i], false)) {
        chosen = roster->accounts[i].id;
      }
    }
  }

  if (chosen) {
    roster_switch(roster, chosen);
  }
}
