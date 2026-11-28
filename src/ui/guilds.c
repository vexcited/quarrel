#include "components.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/templates.h"
#include "server.h"
#include "widgets.h"

// guilds smaller than this show no acronym.
constexpr float ACRONYM_SIZE = 20;

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

  // asked at its size whatever it is drawn at, a tile held down shrinking
  // it.
  if (*url && image_fill_at(app, url, size * .3f, size)) {
    return;
  }

  char acronym[32];
  guild_acronym(guild->name ? guild->name : "?", acronym, sizeof acronym);

  fill(here(), lit ? k->primary : k->hover, size * .3f);

  if (size >= ACRONYM_SIZE) {
    text(acronym, text_size(px(strlen(acronym) > 2 ? 9 : 12)),
         font_weight(medium), text_color(lit ? k->on_primary : k->text),
         whitespace_nowrap);
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
  Store *store = app->session->store;
  bool hides = ids_has(&app->hide_muted, guild->id);
  bool owner = guild->owner_id == store->self_id;
  bool manages = store_guild_permissions(store, guild) & PERM_MANAGE_CHANNELS;
  bool invite = false, hide = false, copy = false, settings = false,
       channel = false, category = false, profile = false, leave = false,
       leave_hub = false;
  civue_rect card = {};

  overlay() {
    div(key(id("guild-menu-card")), flex, flex_col, w(px(240)),
        left(px(card_left(anchor, 240))), top(px(anchor.y1 + 6)), p(2),
        rounded(xl), bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg)) {
      card = here();

      invite = menu_row(app, id("menu-invite"), CIVUE_ICON(mingcute, user_add_fill),
                        guild->hub ? "Invite People" : "Invite to Server",
                        k->link);

      if (guild->hub) {
        profile =
          menu_row(app, id("menu-hub-profile"), CIVUE_ICON(mingcute, idcard_fill),
                   "Edit Hub Profile", k->text);
      }

      if (server_can_manage(app, guild)) {
        settings = menu_row(app, id("menu-settings"),
                            CIVUE_ICON(mingcute, settings_3_fill), "Server Settings",
                            k->text);
      }

      if (manages) {
        channel = menu_row(app, id("menu-create-channel"),
                           CIVUE_ICON(mingcute, add_circle_fill), "Create Channel",
                           k->text);
        category = menu_row(app, id("menu-create-category"),
                            CIVUE_ICON(mingcute, new_folder_fill), "Create Category",
                            k->text);
      }

      div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}

      hide = menu_row(
        app, id("menu-hide-muted"),
        hides ? CIVUE_ICON(mingcute, eye_2_fill) : CIVUE_ICON(mingcute, eye_2_off_fill),
        hides ? "Show Muted Channels" : "Hide Muted Channels", k->text);

      if (!owner && !guild->hub) {
        div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}

        leave = menu_row(app, id("menu-leave"), CIVUE_ICON(mingcute, exit_door_fill),
                         "Leave Server", k->critical_text);
      }

      div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}

      copy = menu_row(app, id("menu-copy-id"), CIVUE_ICON(mingcute, copy_2_fill),
                      "Copy Server ID", k->text);

      if (guild->hub && !owner) {
        leave_hub = menu_row(app, id("menu-leave-hub"),
                             CIVUE_ICON(mingcute, exit_fill), "Leave Hub",
                             k->critical_text);
      }
    }
  }

  dismiss(app, card, anchor);

  if (settings) {
    server_settings_open(app, guild->id, SERVER_PROFILE);
  }
  else if (channel || category) {
    channel_create_open(app, guild->id, 0, category);
  }
  else if (profile) {
    hub_nick_open(app, guild->id);
  }
  else if (leave_hub) {
    hub_leave_open(app, guild->id);
  }
  else if (leave) {
    server_leave_ask(app, guild->id);
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
        rounded(xl), bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg), text_size(px(14)), line_height(px(18))) {
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
              flex_shrink_0, rounded(md), bg(k->primary),
              hover(bg(k->primary_hover)), text_color(k->on_primary),
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

/* ========================================================================== */
/* the dialog adding a server, laid out as discord's.                         */
/* ========================================================================== */

// a dialog in the middle of the window over a dark backdrop that closes it
// when pressed.
#define new_guild_modal(closed)                                              \
  overlay()                                                                  \
  div(key(id("new-guild-backdrop")), left(0), top(0), w(px(ui->size.x)),     \
      h(px(ui->size.y)), flex, items_center, justify_center, bg(k->scrim))   \
  for (bool once_ = ((closed) |= button().activated, true); once_;           \
       once_ = false)                                                        \
  div(key(id("new-guild-dialog")), flex, flex_col, gap(5),                   \
      w(px(dialog_width())), max_h(px(ui->size.y - 48)), p(6),              \
      rounded(px(DIALOG_RADIUS)), bg(k->dialog), border(1),                  \
      border_color(k->border), shadow(lg), text_color(k->text),              \
      text_size(px(14)), line_height(px(20)))                                \
  for (bool inner_ = (button(), true); inner_; inner_ = false)

// the title and words centered, the close button in the corner.
static void header(App *app, const char *title, const char *about,
                   bool *close) {
  const Look *k = &app->look;

  div(flex, flex_col, items_center, gap(2), flex_shrink_0, px(6)) {
    text(title, text_center, text_size(px(24)), line_height(px(30)),
         font_weight(bold), text_color(k->strong));
    text(about, text_center, text_size(px(16)), line_height(px(20)),
         text_color(k->text));

    div(absolute, top(px(-8)), right(px(-8))) {
      *close |= icon_button(app, id("new-guild-close"),
                            CIVUE_ICON(mingcute, close_line), 28, 20,
                            nullptr);
    }
  }
}

// the round mark of a choice, in the primary color.
static void mark(const Look *k, civue_icon icon, float side) {
  div(flex, items_center, justify_center, size(px(side)), flex_shrink_0,
      rounded(full), bg(k->primary)) {
    glyph(icon, side / 2, k->on_primary);
  }
}

// a template's drawing, whole in a square of `side`.
static void template_mark(App *app, const char *url, float side) {
  div(size(px(side)), flex_shrink_0) {
    civue_ui *ui = civue_ui_current();
    civue_rect r = here();
    civue_image_info image =
      civue_images_get(app->images, ui, url, (uint32_t)(side * ui->scale));

    if (image.state == CIVUE_IMAGE_READY && image.width && image.height) {
      double fit = fmin(civue_rect_w(r) / image.width,
                        civue_rect_h(r) / image.height);
      double w = image.width * fit, h = image.height * fit;
      double x = r.x0 + (civue_rect_w(r) - w) / 2;
      double y = r.y0 + (civue_rect_h(r) - h) / 2;

      civue_image(ui, image.texture, civue_rect_from(x, y, x + w, y + h),
                  (float[4]){0, 0, 1, 1}, 1, 0, false);
    }
  }
}

static bool template_row(App *app, size_t index) {
  const Look *k = &app->look;
  const GuildTemplate *chosen = &guild_templates[index];
  bool pressed = false;

  div(key(id_at(id("new-guild-template"), index)), flex, items_center,
      gap(4), flex_shrink_0, h(px(54)), px(4), rounded(lg), border(1),
      border_color(k->border), bg(k->message_hover), hover(bg(k->hover)),
      active(bg(k->pressed)), cursor_pointer) {
    pressed = button().activated;
    template_mark(app, chosen->icon, 20);
    text(chosen->label, flex_1, min_w(0), truncate, text_size(px(16)),
         line_height(px(20)), font_weight(medium), text_color(k->strong));
    icon(mingcute, right_line, size(5), flex_shrink_0, text_color(k->faint));
  }

  return pressed;
}

static bool row(App *app, civue_id key_id, civue_icon icon, const char *label,
                const char *about) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(key_id), flex, items_center, gap(4), flex_shrink_0, p(4),
      rounded(xl), border(1), border_color(k->border_strong),
      hover(bg(k->hover)), active(bg(k->pressed)), cursor_pointer) {
    pressed = button().activated;
    mark(k, icon, 40);

    div(flex, flex_col, flex_1, min_w(0)) {
      text(label, font_weight(bold), text_color(k->strong), truncate);

      if (about) {
        text(about, text_color(k->subtle));
      }
    }

    icon(mingcute, right_fill, size(5), flex_shrink_0, text_color(k->faint));
  }

  return pressed;
}

// back at the left, on at the right, as wide as each other.
static void foot(App *app, const char *next, bool ready, bool *back,
                 bool *go) {
  div(flex, gap(3), flex_shrink_0, mt(1)) {
    div(flex, flex_col, items_stretch, flex_1) {
      *back = form_button(app, id("new-guild-back"), "Back", BUTTON_OUTLINE,
                          false);
    }

    if (next) {
      div(flex, flex_col, items_stretch, flex_1) {
        *go = form_button(app, id("new-guild-next"), next, BUTTON_PRIMARY,
                          !ready);
      }
    }
  }
}

// the name discord fills in for a new server.
static void name_guild(App *app) {
  if (app->guild_name) {
    return;
  }

  char name[96];

  snprintf(name, sizeof name, "%s's server",
           store_user_name(app->session->store->self));
  app->guild_name = strdup(name);
}

static void start(App *app, bool *close) {
  const Look *k = &app->look;
  size_t chosen = guild_template_count;

  header(app, "Create Your Server",
         "Your server is where you and your friends hang out. Make yours and "
         "start talking.",
         close);

  div(key(id("new-guild-templates")), flex, flex_col, gap(2), h(px(330)),
      min_h(0), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    if (template_row(app, 0)) {
      chosen = 0;
    }

    text("Start from a template", mt(2), text_size(px(12)),
         line_height(px(16)), font_weight(semibold), text_color(k->text));

    for (size_t i = 1; i < guild_template_count; i++) {
      if (template_row(app, i)) {
        chosen = i;
      }
    }
  }

  div(flex, flex_col, items_stretch, gap(2), flex_shrink_0) {
    text("Have an invite already?", text_center, text_size(px(20)),
         line_height(px(24)), font_weight(semibold), text_color(k->strong));

    if (form_button(app, id("new-guild-join"), "Join a Server",
                    BUTTON_SECONDARY, false)) {
      app->new_guild_step = NEW_GUILD_JOIN;
    }
  }

  if (chosen < guild_template_count) {
    app->new_guild_template = (uint8_t)chosen;
    app->new_guild_step = NEW_GUILD_INTENT;
  }
}

static void intent(App *app, bool *close) {
  const Look *k = &app->look;
  bool chosen = false, back = false;

  header(app, "Tell Us More About Your Server",
         "In order to help you with your setup, is your new server for just "
         "a few friends or a larger community?",
         close);

  div(flex, flex_col, gap(3), flex_shrink_0) {
    chosen |= row(app, id("new-guild-friends"),
                  CIVUE_ICON(mingcute, sofa_fill), "For me and my friends",
                  nullptr);
    chosen |= row(app, id("new-guild-community"),
                  CIVUE_ICON(mingcute, earth_2_fill), "For a club or community",
                  nullptr);
  }

  div(flex, items_center, justify_center, gap(px(4)), flex_shrink_0) {
    text("Not sure? You can", text_color(k->subtle));

    div(key(id("new-guild-skip")), flex, cursor_pointer) {
      civue_button_state state = button();

      chosen |= state.activated;
      text("skip this question", text_color(k->link));

      if (state.response.hovered) {
        civue_rect r = content();

        fill(civue_rect_from(r.x0, r.y1 - 1, r.x1, r.y1), k->link, 0);
      }
    }

    text("for now.", text_color(k->subtle));
  }

  foot(app, nullptr, false, &back, &chosen);

  // discord asks only to help its setup, the server is made the same.
  if (chosen) {
    name_guild(app);
    app->new_guild_step = NEW_GUILD_CUSTOMIZE;
  }
  else if (back) {
    app->new_guild_step = NEW_GUILD_START;
  }
}

static void customize(App *app, bool *close) {
  const Look *k = &app->look;
  Session *session = app->session;
  bool named = false, create = false, back = false, upload = false;

  for (const char *c = app->guild_name; c && *c; c++) {
    named = named || *c != ' ';
  }

  bool ready = named && !session->guild_creating;

  header(app, "Customize Your Server",
         "Give your new server a personality with a name and an icon. You "
         "can always change it later.",
         close);

  div(flex, flex_col, flex_shrink_0) {
    form_label(k, "Server Icon");

    div(flex, items_center, gap(5)) {
      div(key(id("new-guild-icon")), flex, items_center, justify_center,
          size(px(96)), flex_shrink_0, rounded(full), border(1),
          border_color(k->border_strong), bg(k->field), overflow_hidden,
          text_color(k->faint), hover(text_color(k->strong)),
          cursor_pointer) {
        char url[CDN_URL_MAX] = "";

        upload |= button().activated;

        if (app->guild_icon) {
          snprintf(url, sizeof url, "file://%s", app->guild_icon);
        }

        if (!*url || !image_cover(app, url, 48)) {
          div(flex, flex_col, items_center, gap(1)) {
            icon(mingcute, camera_fill, size(6));
            text("Upload", text_size(px(12)), line_height(px(16)),
                 font_weight(bold));
          }
        }
      }

      div(flex, flex_col, items_start, gap(3), flex_1, min_w(0)) {
        div(flex, gap(2)) {
          upload |= form_button(app, id("new-guild-upload"), "Upload",
                                BUTTON_OUTLINE, false);

          if (app->guild_icon &&
              form_button(app, id("new-guild-remove"), "Remove",
                          BUTTON_DANGER_SECONDARY, false)) {
            free(app->guild_icon);
            app->guild_icon = nullptr;
          }
        }

        text("We recommend an image of at least 512x512.",
             text_color(k->subtle));
      }
    }
  }

  div(flex, flex_col, flex_shrink_0) {
    form_label(k, "Server Name");
    create = form_input(app, id("new-guild-name"), &app->guild_name, "",
                        nullptr, 100)
               .submitted;
  }

  markup(app, id("new-guild-terms"),
         "By creating a server, you agree to Discord's [Community "
         "Guidelines](https://discord.com/guidelines).",
         &(MarkupOptions){.size = 14, .color = k->subtle, .hint = 460});

  if (app->guild_icon_error || session->guild_error) {
    text(app->guild_icon_error ? app->guild_icon_error : session->guild_error,
         text_color(k->critical_text));
  }

  foot(app, session->guild_creating ? "Creating..." : "Create", ready, &back,
       &create);

  if (upload) {
    free(app->guild_icon_error);
    app->guild_icon_error = nullptr;
    pick_image(app, &app->guild_icon, nullptr);
  }

  if (back && !session->guild_creating) {
    app->new_guild_step = NEW_GUILD_INTENT;
  }
  else if (create && ready) {
    char *uri = nullptr;

    free(app->guild_icon_error);
    app->guild_icon_error = nullptr;

    if (app->guild_icon) {
      uri = admin_image_uri(app->guild_icon, IMAGE_MOST,
                            &app->guild_icon_error);
    }

    if (!app->guild_icon || uri) {
      session_create_guild(session, app->guild_name, uri,
                           app->new_guild_template);
    }

    free(uri);
  }
}

// the challenge discord holds the join back for.
static void challenge(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  const SessionCaptcha *asked = &session->join_captcha;

  if (!asked->service) {
    captcha_drop(&app->join_captcha);
    return;
  }

  if (strcmp(asked->service, "hcaptcha") || !asked->sitekey) {
    text("Discord asks for a challenge Quarrel cannot show.",
         text_color(k->critical_text));
    return;
  }

  captcha_start(&app->join_captcha, asked->sitekey, asked->rqdata);

  hcaptcha_state state =
    captcha_node(app, &app->join_captcha, id("new-guild-captcha"));

  if (state.status == HCAPTCHA_SOLVED && state.token) {
    session_join_invite_captcha(session, state.token);
    captcha_drop(&app->join_captcha);
  }
  else if (state.status == HCAPTCHA_CANCELLED ||
           state.status == HCAPTCHA_EXPIRED ||
           state.status == HCAPTCHA_FAILED) {
    captcha_drop(&app->join_captcha);
    session_new_guild_clear(session);
  }
}

static void join(App *app, bool *close) {
  const Look *k = &app->look;
  Session *session = app->session;
  bool typed = app->guild_invite && *app->guild_invite;
  bool ready = typed && !session->joining && !session->join_captcha.service;
  bool go = false, back = false, discover = false;

  header(app, "Join a Server",
         "Enter an invite below to join an existing server", close);

  div(flex, flex_col, flex_shrink_0) {
    form_label(k, "Invite Link");
    go = form_input(app, id("new-guild-invite"), &app->guild_invite,
                    "https://discord.gg/hTKzmak", nullptr, 0)
           .submitted;
  }

  div(flex, flex_col, gap(1), flex_shrink_0) {
    text("Invites should look like", text_size(px(12)), line_height(px(16)),
         font_weight(bold), text_color(k->subtle));
    text("hTKzmak", text_color(k->text));
    text("https://discord.gg/hTKzmak", text_color(k->text));
    text("https://discord.gg/cool-people", text_color(k->text));
  }

  challenge(app);

  if (session->join_error) {
    text(session->join_error, text_color(k->critical_text));
  }

  discover = row(app, id("new-guild-discover"),
                 CIVUE_ICON(mingcute, compass_fill), "Don't have an invite?",
                 "Check out Discoverable communities in Server Discovery.");

  foot(app, session->joining ? "Joining..." : "Join Server", ready, &back,
       &go);

  if (discover) {
    *close = true;
    explore_open(app);
  }
  else if (back && !session->joining) {
    session_new_guild_clear(session);
    app->new_guild_step = NEW_GUILD_START;
  }
  else if (go && ready) {
    session_join_invite(session, app->guild_invite);
  }
}

static void new_guild_reset(App *app) {
  free(app->guild_name);
  free(app->guild_icon);
  free(app->guild_icon_error);
  free(app->guild_invite);
  app->guild_name = nullptr;
  app->guild_icon = nullptr;
  app->guild_icon_error = nullptr;
  app->guild_invite = nullptr;
  app->new_guild_step = NEW_GUILD_START;
  app->new_guild_template = 0;
  captcha_drop(&app->join_captcha);
  session_new_guild_clear(app->session);
}

void new_guild(App *app) {
  Session *session = app->session;

  // the guild made or joined exists: it opens.
  if (session->guild_created &&
      store_guild(session->store, session->guild_created)) {
    Snowflake id = session->guild_created;

    session->guild_created = 0;
    app_select_guild(app, id);

    if (app->popover == POPOVER_NEW_GUILD) {
      app->popover = POPOVER_NONE;
    }

    new_guild_reset(app);
  }

  // joined a group rather than a server, which the direct messages list.
  if (session->joined && !session->guild_created) {
    if (app->popover == POPOVER_NEW_GUILD) {
      app->popover = POPOVER_NONE;
    }

    new_guild_reset(app);
  }

  if (app->popover != POPOVER_NEW_GUILD) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool close = false;

  new_guild_modal(close) {
    switch (app->new_guild_step) {
      case NEW_GUILD_START:
        start(app, &close);
        break;
      case NEW_GUILD_INTENT:
        intent(app, &close);
        break;
      case NEW_GUILD_CUSTOMIZE:
        customize(app, &close);
        break;
      case NEW_GUILD_JOIN:
        join(app, &close);
        break;
    }
  }

  // a challenge over the window takes the escape key itself.
  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) &&
      !session->join_captcha.service) {
    close = true;
  }

  if (close) {
    app->popover = POPOVER_NONE;
    new_guild_reset(app);
  }

}
