#include "components.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

// a row of the switcher is the button that opens it: as high, its icon as
// large, so the open place lies over the button.
constexpr float ROW_HEIGHT = 36;
constexpr float ICON_SIZE = 20;
constexpr float CARD_PAD = 4;
constexpr float LIGHTS_BOTTOM = 28;
constexpr float ARROW_HEIGHT = 18;
// points a second.
constexpr float ARROW_SPEED = 420;
// the two buttons under the list: a line, then 32 points between two pads.
constexpr float FOOTER = 1 + 2 * CARD_PAD + 32;
// under it the card stops lying over its trigger, as base ui's select.
// direct messages over the list: a row between two pads, then a line.
constexpr float HEADER = 2 * CARD_PAD + ROW_HEIGHT + 1;
constexpr float HEIGHT_MIN = 100 + HEADER + FOOTER;

void guild_acronym(const char *name, char *out, size_t cap) {
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

void guild_icon(App *app, const Guild *guild, float size, bool lit) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_guild_icon(url, guild->id, guild->icon, (uint32_t)(size * 2));

  if (*url && image_fill(app, url, size * .3f)) {
    return;
  }

  char acronym[32];
  guild_acronym(guild->name ? guild->name : "?", acronym, sizeof acronym);

  fill(here(), lit ? k->brand : k->hover, size * .3f);

  if (size >= ICON_SIZE) {
    text(acronym, text_size(px(strlen(acronym) > 2 ? 9 : 12)),
         font_weight(medium), text_color(k->text), whitespace_nowrap);
  }
}

static void unread_dot(const Look *k, Unread unread) {
  if (unread.mentions) {
    mention_badge(k, unread.mentions);
  }
  else if (unread.unread && !unread.muted) {
    div(size(2), flex_shrink_0, rounded(full), bg(k->strong)) {}
  }
}

static void selected_mark(const Look *k, bool selected) {
  if (selected) {
    icon(lucide, check, size(px(14)), flex_shrink_0, text_color(k->subtle));
  }
}

static bool guild_row(App *app, Guild *guild, bool nested) {
  const Look *k = &app->look;
  bool selected = app->guild_id == guild->id;
  bool pressed = false;

  div(key(id_at(id("guild"), guild->id)), flex, items_center, gap(2),
      flex_shrink_0, h(px(ROW_HEIGHT)), px(2), rounded(lg),
      // the selected one lies over the button that opened the list, so it
      // is not pushed in like the others of its folder.
      style(.bg = selected ? k->selected : CIVUE_CLEAR,
            .padding[3] = civue_px(nested && !selected ? 20 : 8)),
      hover(bg(selected ? k->selected : k->hover)), cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(px(ICON_SIZE)), flex_shrink_0,
        rounded(md), overflow_hidden) {
      guild_icon(app, guild, ICON_SIZE, selected);
    }

    if (selected) {
      text(guild->name ? guild->name : "", flex_1, min_w(0), truncate,
           text_size(px(14)), line_height(px(18)), font_weight(semibold),
           text_color(k->strong));
    }
    else {
      text(guild->name ? guild->name : "", flex_1, min_w(0), truncate,
           text_size(px(14)), line_height(px(18)), font_weight(medium),
           text_color(k->text));
    }

    unread_dot(k, store_guild_unread(app->session->store, guild));
    selected_mark(k, selected);
  }

  return pressed;
}

static bool home_row(App *app) {
  const Look *k = &app->look;
  bool selected = app->guild_id == 0;
  bool pressed = false;

  div(key(id("home")), flex, items_center, gap(2), flex_shrink_0,
      h(px(ROW_HEIGHT)), px(2), rounded(lg),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover)), cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(px(ICON_SIZE)),
        flex_shrink_0) {
      icon(lucide, message_circle, size(px(18)), text_color(k->subtle));
    }

    if (selected) {
      text("Direct Messages", flex_1, min_w(0), truncate, text_size(px(14)),
           line_height(px(18)), font_weight(semibold), text_color(k->strong));
    }
    else {
      text("Direct Messages", flex_1, min_w(0), truncate, text_size(px(14)),
           line_height(px(18)), font_weight(medium), text_color(k->text));
    }

    unread_dot(k, store_private_unread(app->session->store));
    selected_mark(k, selected);
  }

  return pressed;
}

static bool folder_holds(const GuildFolder *folder, Snowflake guild_id) {
  for (size_t i = 0; i < folder->nguilds; i++) {
    if (folder->guilds[i]->id == guild_id) {
      return true;
    }
  }

  return false;
}

static bool is_folder(const GuildFolder *folder) {
  return folder->id != 0 || folder->nguilds > 1;
}

// whether the folder shows its guilds. the one holding the open guild starts
// open.
static bool *folder_open(App *app, const GuildFolder *folder) {
  typedef struct {
    bool known, open;
  } Fold;

  civue_id id = id_at(id("folder-open"), folder->id);
  Fold *fold = civue_memory(civue_ui_current(), id, sizeof *fold, nullptr);

  if (!fold->known) {
    fold->known = true;
    fold->open = folder_holds(folder, app->guild_id);
  }

  return &fold->open;
}

// the folder's name, its guilds' when it has none, as discord shows it.
static void folder_name(const GuildFolder *folder, char *out, size_t cap) {
  size_t n = 0;

  out[0] = 0;

  if (folder->name && *folder->name) {
    snprintf(out, cap, "%s", folder->name);
    return;
  }

  for (size_t i = 0; i < folder->nguilds && n + 1 < cap; i++) {
    const char *name = folder->guilds[i]->name;

    n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "",
                          name ? name : "");
  }
}

static void folder_row(App *app, const GuildFolder *folder, bool *open) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  civue_color tint = folder->has_color ? rgb(folder->color) : k->brand;
  Unread all = {};
  char name[160];

  folder_name(folder, name, sizeof name);

  for (size_t i = 0; i < folder->nguilds; i++) {
    Unread one = store_guild_unread(store, folder->guilds[i]);

    all.unread |= one.unread && !one.muted;
    all.mentions += one.mentions;
  }

  div(key(id_at(id("folder"), folder->id)), flex, items_center, gap(2),
      flex_shrink_0, h(px(ROW_HEIGHT)), px(2), rounded(lg), hover(bg(k->hover)),
      cursor_pointer) {
    if (button().activated) {
      *open = !*open;
    }

    div(flex, items_center, justify_center, size(px(ICON_SIZE)), flex_shrink_0,
        rounded(md), bg(civue_alpha(tint, .25f))) {
      if (*open) {
        icon(lucide, folder_open, size(px(12)), text_color(tint));
      }
      else {
        icon(lucide, folder, size(px(12)), text_color(tint));
      }
    }

    text(name, flex_1, min_w(0), truncate, text_size(px(14)),
         line_height(px(18)), font_weight(medium), text_color(k->text));

    // an open folder lets its guilds say what waits.
    if (!*open) {
      unread_dot(k, all);
    }

    if (*open) {
      icon(lucide, chevron_down, size(px(14)), flex_shrink_0,
           text_color(k->faint));
    }
    else {
      icon(lucide, chevron_right, size(px(14)), flex_shrink_0,
           text_color(k->faint));
    }
  }
}

static bool footer_button(App *app, civue_id id, civue_icon glyph_icon,
                          const char *label) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, gap(1p5), flex_1, min_w(0),
      h(8), rounded(lg), text_color(k->subtle),
      hover(bg(k->hover), text_color(k->strong)), text_size(px(13)),
      line_height(px(16)), font_weight(medium), cursor_pointer) {
    pressed = button().activated;
    glyph(glyph_icon, 15, k->subtle);
    text(label, min_w(0), truncate);
  }

  return pressed;
}

// an arrow over an end of the switcher's list while it has more that way, as
// the menus of macos have: the list scrolls for as long as the pointer is
// over it.
static void scroll_arrow(App *app, civue_id list, float at, float most,
                         bool up) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id arrow = up ? id("guild-arrow-up") : id("guild-arrow-down");
  bool over = false;

  div(key(arrow), absolute, left(0), right(0), h(px(ARROW_HEIGHT)), flex,
      items_center, justify_center, bg(k->surface_high),
      style(.inset[0] = up ? civue_px(HEADER) : (civue_length){},
            .inset[2] = up ? (civue_length){} : civue_px(FOOTER))) {
    // an arrow that shows up under a pointer at rest scrolls nothing, as
    // in the menus of macos.
    over = civue_interact(ui, arrow, here(), CIVUE_SENSE_HOVER).hovered &&
           app->guilds_armed;
    glyph(up ? CIVUE_ICON(lucide, chevron_up)
             : CIVUE_ICON(lucide, chevron_down),
          14, over ? k->strong : k->faint);
  }

  if (!over) {
    app->guilds_arrow[up] = 0;
    return;
  }

  // a frame may have been long ago: the list does not jump for it.
  double since = app->guilds_arrow[up] ? ui->now - app->guilds_arrow[up] : 0;
  float by = ARROW_SPEED * (float)fmin(since, 0.05);
  float to = fminf(fmaxf(at + (up ? -by : by), 0), most);

  app->guilds_arrow[up] = ui->now;
  civue_set_scroll_y(ui, list, to);
  civue_request_repaint(ui);
}

// the switcher lies over the button that opens it, like a select: as wide,
// the open place's row right over the button. it is free to reach over the
// title bar and the window's buttons, which hide for it.
void guild_switcher(App *app, civue_rect anchor) {
  if (app->popover != POPOVER_GUILDS) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id list = id("guild-switcher");
  size_t count = 0, rows = 0, before = 0;
  const GuildFolder *folders = store_guild_folders(app->session->store, &count);
  bool found = app->guild_id == 0;

  // the rows shown, and how many come before the open guild's.
  for (size_t i = 0; i < count; i++) {
    const GuildFolder *folder = &folders[i];
    bool nested = is_folder(folder);
    bool open = !nested || *folder_open(app, folder);

    if (!folder->nguilds) {
      continue;
    }

    if (nested) {
      // a closed folder stands for the guild it holds.
      if (!found && !open && folder_holds(folder, app->guild_id)) {
        found = true;
        before = rows;
      }

      rows++;
    }

    for (size_t j = 0; open && j < folder->nguilds; j++) {
      if (!found && folder->guilds[j]->id == app->guild_id) {
        found = true;
        before = rows;
      }

      rows++;
    }
  }

  // what is above the open place's row in the card, its border counted.
  float above = app->guild_id ? CARD_PAD + (float)before * ROW_HEIGHT : 0;
  float lead = 1 + (app->guild_id ? HEADER + above : CARD_PAD);
  float wanted = 2 + HEADER + 2 * CARD_PAD + (float)rows * ROW_HEIGHT + FOOTER;
  // as far from the window's top and bottom as from its side.
  float left = (float)anchor.x0 - CARD_PAD - 1;
  float gap = fmaxf(left, 0);
  // it may go past the window's top and bottom, up to the screen's, where
  // the window lets its overlays out.
  bool out = ui->screen.y0 < 0 || ui->screen.y1 > ui->size.y;
  float edge = (float)ui->screen.y0 + gap;
  float floor = (float)ui->screen.y1 - gap;

  ui->overlay_outside = out;

  // where the list is scrolled to, which it only says itself a frame after
  // being told.
  float at = civue_scroll_y(ui, list);

  // placed as it opens only: folding a folder then leaves it where it is.
  // the anchor is not always laid out on the very first frame.
  if (app->guilds_fresh) {
    app->guilds_mouse = ui->input.mouse;
    app->guilds_armed = false;

    float top = (float)anchor.y0 - lead;

    // the rows that would go above the window are scrolled away instead,
    // and the card is that much shorter. the open guild's row stays under
    // direct messages and under the arrow that then lies over the top of
    // the list, lower than the button when it has to.
    float room = fmaxf(above - ARROW_HEIGHT, 0);

    app->guilds_hidden = fminf(fmaxf(edge - top, 0), room);
    app->guilds_top = fmaxf(top, edge);
    app->guilds_grows = true;

    float scroll = app->guilds_hidden;
    float height = fminf(wanted - scroll, floor - app->guilds_top);

    // base ui's select does the same: too short a card over its trigger is
    // a plain menu under it instead, the open row in the middle.
    if (height < fminf(wanted, HEIGHT_MIN)) {
      app->guilds_top = (float)anchor.y1 + 4;
      app->guilds_hidden = 0;
      app->guilds_grows = false;

      float fixed = 2 + HEADER + FOOTER;
      float view = fminf(wanted, floor - app->guilds_top) - fixed;
      float most = fmaxf(wanted - fixed - view, 0);

      scroll = above - (view - ROW_HEIGHT) / 2;
      scroll = fminf(fmaxf(scroll, 0), most);
    }

    civue_set_scroll_y(ui, list, scroll);
    at = scroll;

    if (!ui->rerun) {
      app->guilds_fresh--;
    }
  }
  else if (fabs(ui->input.mouse.x - app->guilds_mouse.x) > 2 ||
           fabs(ui->input.mouse.y - app->guilds_mouse.y) > 2) {
    app->guilds_armed = true;
  }

  if (!app->guilds_fresh && app->guilds_grows) {
    // as base ui's select, scrolling grows the card before it moves its
    // rows, until it is as tall as the window lets it be.
    float scroll = at;

    // towards the first rows, its end goes down.
    app->guilds_hidden = fminf(app->guilds_hidden, scroll);

    // towards the last ones, its top goes up.
    if (scroll > 0 && app->guilds_top > edge) {
      float taken = fminf(scroll, app->guilds_top - edge);

      app->guilds_top -= taken;
      app->guilds_hidden = fmaxf(app->guilds_hidden - taken, 0);
      civue_set_scroll_y(ui, list, scroll - taken);
      at = scroll - taken;
    }
  }

  float top = app->guilds_top;
  float height = fminf(wanted - app->guilds_hidden, floor - top);
  float width = (float)civue_rect_w(anchor) + 2 * CARD_PAD + 2;
  civue_rect card = xywh(left, top, width, height);
  Guild *chosen = nullptr;
  bool home = false, explore = false, create = false;

  // only when it reaches them: they end 28 points down.
  if (card.x0 < ui->titlebar_inset && card.y0 < LIGHTS_BOTTOM) {
    // drawn over the window, they need no hiding.
    app->covers_buttons = !out;
  }

  overlay() {
    div(key(id("guild-card")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(width)), h(px(height)), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      div(flex, flex_col, flex_shrink_0, p(px(CARD_PAD)), border_b(1),
          border_color(k->border)) {
        home = home_row(app);
      }

      // no bars, as a menu of macos: arrows say there is more.
      div(key(list), flex, flex_col, flex_1, min_h(0), p(px(CARD_PAD)),
          overflow_y_scroll) {
        for (size_t i = 0; i < count; i++) {
          const GuildFolder *folder = &folders[i];
          bool nested = is_folder(folder);

          if (nested && folder->nguilds) {
            bool *open = folder_open(app, folder);

            folder_row(app, folder, open);

            if (!*open) {
              continue;
            }
          }

          for (size_t j = 0; j < folder->nguilds; j++) {
            if (guild_row(app, folder->guilds[j], nested)) {
              chosen = folder->guilds[j];
            }
          }
        }
      }

      div(flex, gap(1), flex_shrink_0, p(px(CARD_PAD)), border_t(1),
          border_color(k->border)) {
        explore = footer_button(app, id("guild-explore"),
                                CIVUE_ICON(lucide, compass), "Explore");
        create = footer_button(app, id("guild-create"),
                               CIVUE_ICON(lucide, plus), "Create");
      }

      float most = fmaxf(wanted - height, 0);

      if (at > 0.5f) {
        scroll_arrow(app, list, at, most, true);
      }

      if (at < most - 0.5f) {
        scroll_arrow(app, list, at, most, false);
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }

  if (chosen || home) {
    app->popover = POPOVER_NONE;
    app_select_guild(app, chosen ? chosen->id : 0);
  }
  else if (explore) {
    app->popover = POPOVER_NONE;
    civue_open_url("https://discord.com/servers");
  }
  else if (create) {
    app->popover = POPOVER_NEW_GUILD;
  }
}

void guild_banner(App *app, const Guild *guild) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX] = "";

  cdn_banner(url, guild->id, guild->banner, 1024);

  if (!*url) {
    return;
  }

  div(flex_shrink_0, h(px(108)), mx(2), mb(2), rounded(lg), bg(k->hover),
      overflow_hidden) {
    image_cover(app, url, 8);
  }
}

// closes the popover on a press outside of `card` and `anchor`, or on escape.
static void dismiss(App *app, civue_rect card, civue_rect anchor) {
  civue_ui *ui = civue_ui_current();
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}

// where a card `width` wide goes under `anchor`, kept on the screen.
static float card_left(civue_rect anchor, float width) {
  civue_rect room = overlay_room();
  float left = fminf((float)anchor.x0, (float)room.x1 - width);

  return fmaxf(left, (float)room.x0);
}

void guild_menu(App *app, civue_rect anchor) {
  const Guild *guild = app_guild(app);

  if (app->popover != POPOVER_GUILD_MENU || !guild) {
    return;
  }

  const Look *k = &app->look;
  bool hides = ids_has(&app->hide_muted, guild->id);
  bool owner = guild->owner_id == app->session->store->self_id;
  bool invite = false, hide = false, copy = false, profile = false,
       leave = false;
  civue_rect card = {};

  overlay() {
    div(key(id("guild-menu-card")), flex, flex_col, w(px(240)),
        left(px(card_left(anchor, 240))), top(px(anchor.y1 + 6)), p(2),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg)) {
      card = here();

      invite = menu_row(app, id("menu-invite"), CIVUE_ICON(lucide, user_plus),
                        guild->hub ? "Invite People" : "Invite to Server",
                        k->text);

      if (guild->hub) {
        profile =
          menu_row(app, id("menu-hub-profile"), CIVUE_ICON(lucide, id_card),
                   "Edit Hub Profile", k->text);
      }

      hide = menu_row(
        app, id("menu-hide-muted"),
        hides ? CIVUE_ICON(lucide, eye) : CIVUE_ICON(lucide, eye_off),
        hides ? "Show Muted Channels" : "Hide Muted Channels", k->text);

      div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}

      copy = menu_row(app, id("menu-copy-id"), CIVUE_ICON(lucide, copy),
                      "Copy Server ID", k->text);

      if (guild->hub && !owner) {
        leave = menu_row(app, id("menu-leave-hub"),
                         CIVUE_ICON(lucide, log_out), "Leave Hub",
                         k->critical_text);
      }
    }
  }

  dismiss(app, card, anchor);

  if (profile) {
    hub_nick_open(app, guild->id);
  }
  else if (leave) {
    hub_leave_open(app, guild->id);
  }
  else if (invite) {
    app->popover = POPOVER_INVITE;
    app->invite_copied = false;
    session_create_invite(app->session, guild->id);
  }
  else if (hide) {
    ids_toggle(&app->hide_muted, guild->id);
    app->popover = POPOVER_NONE;
  }
  else if (copy) {
    char digits[21];

    snowflake_format(guild->id, digits);
    civue_clipboard_set_text(digits, strlen(digits));
    app->popover = POPOVER_NONE;
  }
}

void invite_card(App *app, civue_rect anchor) {
  const Guild *guild = app_guild(app);

  if (app->popover != POPOVER_INVITE || !guild) {
    return;
  }

  const Look *k = &app->look;
  const Session *session = app->session;
  bool mine = session->invite_guild_id == guild->id;
  const char *code = mine ? session->invite_code : nullptr;
  char link[96] = "", title[160];
  civue_rect card = {};
  bool copy = false;

  if (code) {
    snprintf(link, sizeof link, "https://discord.gg/%s", code);
  }

  // a hub is joined with a student email, its invites never expire.
  if (guild->hub) {
    snprintf(title, sizeof title,
             "Invite friends to the Student Hub for your school");
  }
  else {
    snprintf(title, sizeof title, "Invite people to %s",
             guild->name ? guild->name : "this server");
  }

  overlay() {
    div(key(id("invite-card")), flex, flex_col, gap(2), w(px(320)),
        left(px(card_left(anchor, 320))), top(px(anchor.y1 + 6)), p(4),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), text_size(px(14)),
        line_height(px(18))) {
      card = here();

      text(title, text_size(px(16)), line_height(px(22)),
           font_weight(semibold), text_color(k->strong));

      if (guild->hub) {
        text("A student email address is required to join.",
             text_color(k->subtle));
      }

      if (code) {
        div(flex, items_center, gap(2), h(10), pl(3), pr(1), rounded(lg),
            bg(k->input), border(1), border_color(k->border)) {
          text(link, flex_1, min_w(0), truncate, text_color(k->text));

          div(key(id("invite-copy")), flex, items_center, h(8), px(3),
              flex_shrink_0, rounded(md), bg(k->brand),
              hover(bg(k->brand_hover)), text_color(CIVUE_WHITE),
              font_weight(medium), cursor_pointer) {
            copy = button().activated;
            text(app->invite_copied ? "Copied" : "Copy");
          }
        }

        text(guild->hub ? "This link never expires."
                        : "This link expires in 7 days.",
             text_size(px(12)),
             line_height(px(16)), text_color(k->faint));
      }
      else if (mine && session->invite_error) {
        text(session->invite_error, text_color(k->critical_text));
      }
      else {
        text("Creating a link...", text_color(k->subtle));
      }
    }
  }

  dismiss(app, card, anchor);

  if (copy) {
    civue_clipboard_set_text(link, strlen(link));
    app->invite_copied = true;
  }
}

// the user is in a student hub already.
static bool in_hub(Store *store) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      if (folders[i].guilds[j]->hub) {
        return true;
      }
    }
  }

  return false;
}

void new_guild(App *app, civue_rect anchor) {
  Session *session = app->session;

  // the guild asked for exists: it opens.
  if (session->guild_created &&
      store_guild(session->store, session->guild_created)) {
    Snowflake id = session->guild_created;

    session->guild_created = 0;
    free(app->guild_name);
    app->guild_name = nullptr;

    if (app->popover == POPOVER_NEW_GUILD) {
      app->popover = POPOVER_NONE;
    }

    app_select_guild(app, id);
  }

  if (app->popover != POPOVER_NEW_GUILD) {
    return;
  }

  const Look *k = &app->look;
  bool named = app->guild_name && *app->guild_name;
  bool ready = named && !session->guild_creating;
  bool create = false, join_hub = false;
  civue_rect card = {};

  overlay() {
    div(key(id("new-guild-card")), flex, flex_col, gap(3), w(px(300)),
        left(px(card_left(anchor, 300))), top(px(anchor.y0)), p(4), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), text_size(px(14)), line_height(px(18))) {
      card = here();

      text("Create a server", text_size(px(16)), line_height(px(22)),
           font_weight(semibold), text_color(k->strong));

      div(key(id("new-guild-name")), h(9)) {
        civue_field_look look = field_look(k);

        look.pad = 10;
        create = text_field(&app->guild_name, "Server name", look).submitted;
      }

      if (session->guild_error) {
        text(session->guild_error, text_color(k->critical_text));
      }

      div(key(id("new-guild-create")), flex, items_center, justify_center, h(9),
          rounded(lg), font_weight(medium),
          style(.bg = ready ? k->brand : k->hover,
                .text_color = ready ? CIVUE_WHITE : k->faint,
                .has_text_color = true,
                .cursor = ready ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
                .has_cursor = true)) {
        create |= button().activated;
        text(session->guild_creating ? "Creating..." : "Create");
      }

      div(flex_shrink_0, h(px(1)), bg(k->border)) {}

      // discord offers this in its discovery's student hubs.
      join_hub = menu_row(app, id("new-guild-hub"),
                          CIVUE_ICON(lucide, graduation_cap),
                          in_hub(session->store)
                            ? "Join Another Hub"
                            : "Join the Student Hub for your school",
                          k->text);
    }
  }

  dismiss(app, card, anchor);

  if (join_hub) {
    app->popover = POPOVER_NONE;
    hub_join_open(app);
  }
  else if (create && ready) {
    session_create_guild(session, app->guild_name);
  }
}
