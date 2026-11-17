#include "components.h"

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
      icon(lucide, check, size(4), flex_shrink_0, text_color(k->positive));
    }
    else {
      mention_badge(k, account_mentions(a));
    }
  }

  return pressed;
}

static bool status_row(App *app, UserStatus status, const char *label) {
  const Look *k = &app->look;
  bool current = app->session->store->self_status == status;
  bool pressed = false;

  div(key(id_at(id("status"), status)), flex, items_center, gap(3),
      flex_shrink_0, h(px(STATUS_HEIGHT)), px(2), rounded(lg),
      text_color(k->text), hover(bg(k->hover)), text_size(px(14)),
      line_height(px(18)), font_weight(medium), cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(8), flex_shrink_0) {
      div(size(3)) {
        status_dot(status, here(), k, k->surface_high);
      }
    }

    text(label, flex_1);

    if (current) {
      icon(lucide, check, size(4), flex_shrink_0, text_color(k->positive));
    }
  }

  return pressed;
}

static void separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}
}

static void profile_banner(App *app, const User *self) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX] = "";
  civue_color ground = self->has_accent ? rgb(self->accent_color) : k->brand;

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
        rounded(full), bg(k->surface_high)) {
      avatar(app, self, 0, PROFILE_AVATAR, true, k->surface_high);
    }

    copy = icon_button(app, id("copy-id"), CIVUE_ICON(lucide, copy), 28, 16,
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
    }

    if (self->custom_status && self->activity) {
      text(self->activity, mt(2), text_size(px(14)), line_height(px(18)),
           text_color(k->text));
    }

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

void account_switcher(App *app, civue_rect anchor) {
  if (app->popover != POPOVER_ACCOUNTS) {
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
  civue_rect card = {};
  Snowflake chosen = 0;
  bool add = false, leave = false;

  overlay_room();

  overlay() {
    div(key(id("accounts")), flex, flex_col, left(px(anchor.x0)),
        top(px(anchor.y1 + 6)), w(px(SWITCHER_WIDTH)), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
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
          add = menu_row(app, id("account-add"), CIVUE_ICON(lucide, plus),
                         "Add an account", k->text);
        }

        separator(k);

        for (size_t i = 0; i < 4; i++) {
          UserStatus status = statuses[i].status;

          if (status_row(app, status, statuses[i].label)) {
            session_set_status(session, status, status == STATUS_OFFLINE);
          }
        }

        separator(k);

        leave = menu_row(app, id("logout"), CIVUE_ICON(lucide, log_out),
                         "Log Out", k->critical_text);
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
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
