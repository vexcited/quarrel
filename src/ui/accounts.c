#include "components.h"

#include <math.h>
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
constexpr float PANEL_HEIGHT = 56;
constexpr float PANEL_AVATAR = 32;
// a button of the panel, and the arrow beside it that opens its menu.
constexpr float PANEL_BUTTON = 32;
constexpr float PANEL_ARROW = 16;
// how long the line under the name takes to roll to the username.
constexpr float ROLL_SECONDS = .22f;

static const struct {
  UserStatus status;
  const char *label;
} statuses[] = {
  {STATUS_ONLINE, "Online"},
  {STATUS_IDLE, "Idle"},
  {STATUS_DND, "Do Not Disturb"},
  {STATUS_OFFLINE, "Invisible"},
};

static const char *status_label(UserStatus status) {
  for (size_t i = 0; i < countof(statuses); i++) {
    if (statuses[i].status == status) {
      return statuses[i].label;
    }
  }

  return "Online";
}

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

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  Roster *roster = app->roster;
  size_t count = roster->naccounts;
  bool can_add = count > 0;
  civue_rect card = {}, spans = {};
  Snowflake chosen = 0;
  bool add = false, leave = false;
  civue_id at = id("accounts");
  float top = fmaxf((float)anchor.y0 - 8 - (float)last_size(at).y, 8);

  overlay_room();
  session_fetch_profile(session);

  overlay() {
    div(key(at), flex, flex_col, left(px(anchor.x0)), top(px(top)),
        w(px(SWITCHER_WIDTH)), rounded(xl),
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

// `top` over `under`, as one color.
static civue_color over(civue_color top, civue_color under) {
  float a = top.a;

  return (civue_color){top.r * a + under.r * (1 - a),
                       top.g * a + under.g * (1 - a),
                       top.b * a + under.b * (1 - a), under.a};
}

// the colors of nameplates as discord names them, the dark theme's.
static const struct {
  const char *name;
  uint32_t dark;
} PALETTES[] = {
  {"crimson", 0x900007}, {"berry", 0x893a99},  {"sky", 0x0080b7},
  {"teal", 0x086460},    {"forest", 0x2d5401}, {"bubble_gum", 0xdc3e97},
  {"violet", 0x730bc8},  {"cobalt", 0x0131c2}, {"clover", 0x047b20},
  {"lemon", 0xf6cd12},   {"white", 0xffffff},  {"black", 0x000000},
};

// the color of the nameplate the account wears, clear for none.
static civue_color plate_color(const User *self) {
  for (size_t i = 0; self && self->nameplate && self->nameplate_palette &&
                     i < countof(PALETTES);
       i++) {
    if (!strcmp(self->nameplate_palette, PALETTES[i].name)) {
      return rgb(PALETTES[i].dark);
    }
  }

  return CIVUE_CLEAR;
}

// how much of the nameplate's color is at `x`, 0 to 1 across the panel: a
// tenth at its left, two fifths at its right.
static float plate_alpha(civue_rect panel, double x) {
  float t = (float)((x - panel.x0) / fmax(civue_rect_w(panel), 1));

  return .1f + .3f * fminf(fmaxf(t, 0), 1);
}

// the nameplate behind the panel, as discord's: its color across it, and
// its picture at the right, faint, moving while the pointer is on the panel.
static void nameplate(App *app, const User *self, civue_color tint,
                      bool hovered) {
  civue_ui *ui = civue_ui_current();
  bool moves = hovered && ui->window_focused && !ui->reduce_motion;
  char url[CDN_URL_MAX];

  div(absolute, left(0), top(0), right(0), bottom(0)) {
    civue_rect r = here();
    uint32_t pixels = (uint32_t)(civue_rect_h(r) * 6 * ui->scale);
    civue_image_info image = {};

    // civue draws no gradient: thin columns of it.
    for (double x = r.x0; tint.a > 0 && x < r.x1; x += 2) {
      fill(civue_rect_from(x, r.y0, fmin(x + 2, r.x1), r.y1),
           civue_alpha(tint, plate_alpha(r, x)), 0);
    }

    if (moves) {
      cdn_nameplate(url, self->nameplate, true);
      image = civue_images_get_animated(app->images, ui, url, pixels);
    }

    if (image.state != CIVUE_IMAGE_READY) {
      cdn_nameplate(url, self->nameplate, false);
      image = civue_images_get(app->images, ui, url, pixels);
    }

    if (image.state == CIVUE_IMAGE_READY && image.width && image.height) {
      double h = civue_rect_h(r), w = h * image.width / image.height;

      civue_image(ui, image.texture, xywh(r.x1 - w, r.y0, w, h),
                  (float[4]){0, 0, 1, 1}, .4f, 0, false);
    }
  }
}

// what the account says under its name: the status it wrote, else what it
// does, else how it shows.
static void status_line(App *app, const User *self) {
  const Store *store = app->session->store;
  char doing[256];
  const char *words = status_label(store->self_status);

  if (self && self->status_text) {
    words = self->status_text;
  }
  else if (self && self->nactivities && store->self_status != STATUS_OFFLINE) {
    words = activity_status_text(&self->activities[0], doing, sizeof doing);
  }

  div(flex, items_center, gap(1), min_w(0), h(px(16))) {
    if (self && self->status_emoji) {
      emoji_node(app, self->status_emoji_id, self->status_emoji, 14);
    }

    text(words, min_w(0), truncate);
  }
}

// who is signed in, pressed for the accounts. the line under the name rolls
// to the username while the pointer is on the panel, as discord's.
static void account_chip(App *app, civue_color ground, bool rolled) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const User *self = app->session->store->self;
  civue_id id = id("account-chip");
  bool open = app->popover == POPOVER_ACCOUNTS;
  float roll = civue_animate(ui, civue_ids(id, "roll"), rolled ? 1 : 0,
                             ui->reduce_motion ? 0 : ROLL_SECONDS);
  civue_rect anchor = {};

  div(key(id), flex, items_center, gap(2), flex_1, min_w(0), h(px(42)), pl(1),
      pr(2), rounded(lg), style(.bg = open ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover)), cursor_pointer) {
    civue_button_state state = button();
    civue_color under =
      open || state.response.hovered ? over(k->hover, ground) : ground;

    anchor = here();

    if (state.activated) {
      app_toggle(app, POPOVER_ACCOUNTS);
    }

    div(relative, flex_shrink_0) {
      avatar(app, self, 0, PANEL_AVATAR, true, under);

      // another account has something waiting.
      if (accounts_waiting(app)) {
        mention_dot(k, under);
      }
    }

    div(flex, flex_col, gap(px(2)), flex_1, min_w(0)) {
      text(store_user_name(self), min_w(0), truncate, text_size(px(16)),
           line_height(px(18)), font_weight(medium), text_color(k->strong));

      // the status goes up and out as the username comes up from under it.
      div(relative, h(px(16)), min_w(0), overflow_hidden, text_size(px(12)),
          line_height(px(16)), font_weight(medium), text_color(k->subtle)) {
        div(absolute, left(0), right(0), top(px(-17 * roll))) {
          faded(1 - roll) {
            status_line(app, self);
          }
        }

        div(absolute, left(0), right(0), top(px(17 * (1 - roll)))) {
          faded(roll) {
            text(self ? self->username : "", min_w(0), truncate);
          }
        }
      }
    }
  }

  account_switcher(app, anchor);
}

// how the buttons of the panel look over what is under them.
typedef struct {
  // over a nameplate, of its color.
  civue_color plate;
  bool on_plate;
} Ground;

// a button of the panel. `corners` rounds its left, right or both sides,
// `lit` tints it as the other half of its pair is pointed at, `off` makes it
// red. true when pressed, `right` when pressed with the other button.
static bool panel_button(App *app, civue_id id, civue_icon glyph_icon,
                         float width, float glyph_size, const Ground *g,
                         int corners, bool lit, bool off, bool open,
                         const char *label, bool *right) {
  const Look *k = &app->look;
  float radius = 8;
  civue_color rest = open ? k->hover : lit ? civue_alpha(k->hover, .5f)
                                           : CIVUE_CLEAR;
  civue_color hovered = k->hover;
  civue_color color = off ? k->danger : k->subtle;
  bool pressed = false;

  if (g->on_plate) {
    rest = open ? civue_alpha(g->plate, .2f) : CIVUE_CLEAR;
    hovered = civue_alpha(g->plate, .2f);
    color = off ? k->danger : civue_alpha(rgb(0xffffff), .9f);
  }

  // discord's red glow on what is turned off.
  if (off) {
    rest = g->on_plate ? rgba(0x00000038) : civue_alpha(k->danger, .12f);
    hovered = g->on_plate ? rgba(0x00000054) : civue_alpha(k->danger, .24f);
  }

  div(key(id), flex, items_center, justify_center, flex_shrink_0, w(px(width)),
      h(px(PANEL_BUTTON)),
      style(.radius = {corners & 1 ? civue_px(radius) : civue_px(0),
                       corners & 2 ? civue_px(radius) : civue_px(0),
                       corners & 2 ? civue_px(radius) : civue_px(0),
                       corners & 1 ? civue_px(radius) : civue_px(0)},
            .bg = rest),
      hover(bg(hovered)), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    *right |= state.response.right_clicked;
    glyph(glyph_icon, glyph_size, color);

    if (label) {
      hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
    }
  }

  return pressed;
}

// the microphone or the sound and the arrow of its menu, as one: turned off
// in red. true when the button was pressed, the pair's rect in `anchor`.
static bool audio_pair(App *app, AudioKind kind, bool off, const Ground *g,
                       civue_rect *anchor) {
  civue_ui *ui = civue_ui_current();
  bool input = kind == AUDIO_INPUTS;
  Popover popover = input ? POPOVER_AUDIO_INPUT : POPOVER_AUDIO_OUTPUT;
  civue_id id = id_at(id("panel-audio"), kind);
  bool open = app->popover == popover;
  bool pressed = false, toggle = false, right = false;
  civue_icon glyph_icon =
    input ? off ? CIVUE_ICON(mingcute, mic_off_fill)
                : CIVUE_ICON(mingcute, mic_fill)
    : off ? CIVUE_ICON(mingcute, headphone_off_fill)
          : CIVUE_ICON(mingcute, headphone_fill);
  const char *label = input ? off ? "Unmute" : "Mute"
                      : off ? "Undeafen"
                            : "Deafen";
  civue_icon arrow = open ? CIVUE_ICON(mingcute, up_fill)
                          : CIVUE_ICON(mingcute, down_fill);

  div(flex, items_center, gap(px(1)), flex_shrink_0) {
    *anchor = here();

    // the other half is tinted while one is pointed at.
    bool lit = ui->input.has_mouse &&
               civue_rect_contains(*anchor, ui->input.mouse) &&
               !civue_under_overlay(ui, ui->input.mouse);

    pressed = panel_button(app, id_at(id, 1), glyph_icon, PANEL_BUTTON, 20, g,
                           1, lit, off, false, open ? nullptr : label, &right);
    toggle = panel_button(app, id_at(id, 2), arrow, PANEL_ARROW, 16, g, 2,
                          lit, off, open, nullptr, &right);
  }

  // the other button on the pair opens its menu too.
  if (toggle || right) {
    app_toggle(app, popover);

    if (app->popover == popover) {
      devices_list(app);
    }
  }

  return pressed;
}

void user_panel(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const User *self = s->store->self;
  bool muted = s->voice_mute || s->voice_deaf;
  civue_color tint = plate_color(self);
  Ground g = {.plate = tint, .on_plate = tint.a > 0};
  civue_rect input = {}, output = {};
  bool cog = false, right = false;

  div(key(id("user-panel")), relative, flex, items_center, gap(2),
      flex_shrink_0, h(px(PANEL_HEIGHT)), px(2), rounded(px(PANEL_RADIUS)),
      bg(k->chat), border(1), border_color(k->frame_border), overflow_hidden) {
    civue_rect r = here();
    bool hovered = ui->input.has_mouse &&
                   civue_rect_contains(r, ui->input.mouse) &&
                   !civue_under_overlay(ui, ui->input.mouse);
    // what is under the avatar, its status cut out of it.
    civue_color ground =
      g.on_plate
        ? over(civue_alpha(tint, plate_alpha(r, r.x0 + 28)), k->chat)
        : k->chat;

    if (g.on_plate) {
      nameplate(app, self, tint, hovered);
    }

    account_chip(app, ground, hovered);

    div(flex, items_center, gap(2), flex_shrink_0) {
      if (audio_pair(app, AUDIO_INPUTS, muted, &g, &input)) {
        session_voice_mute(s, !muted);
      }

      if (audio_pair(app, AUDIO_OUTPUTS, s->voice_deaf, &g, &output)) {
        session_voice_deafen(s, !s->voice_deaf);
      }

      cog = panel_button(app, id("panel-settings"),
                         CIVUE_ICON(mingcute, settings_3_fill), PANEL_BUTTON,
                         20, &g, 3, false, false, app->settings_open,
                         "User Settings", &right);
    }
  }

  audio_menu(app, input, AUDIO_INPUTS);
  audio_menu(app, output, AUDIO_OUTPUTS);

  // in a call, its settings first.
  if (cog) {
    settings_show(app, s->voice_channel_id ? SETTINGS_VOICE
                                           : SETTINGS_APPEARANCE);
  }
}
