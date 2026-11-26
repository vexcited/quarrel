#include "authorize.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "components.h"
#include "discord/cdn.h"
#include "forms.h"
#include "widgets.h"

constexpr float ICON = 80;

// the permissions the client's dialog knows, in its order.
static const AuthorizePermission PERMISSIONS[] = {
  {PERM_ADMINISTRATOR, "Administrator"},
  {PERM_MANAGE_GUILD, "Manage Server"},
  {PERM_MANAGE_ROLES, "Manage Roles"},
  {PERM_MANAGE_CHANNELS, "Manage Channels"},
  {PERM_KICK_MEMBERS, "Kick Members"},
  {PERM_BAN_MEMBERS, "Ban Members"},
  {PERM_CREATE_INSTANT_INVITE, "Create Invite"},
  {PERM_MANAGE_NICKNAMES, "Manage Nicknames"},
  {PERM_CHANGE_NICKNAME, "Change Nickname"},
  {PERM_MANAGE_EXPRESSIONS, "Manage Expressions"},
  {PERM_CREATE_EXPRESSIONS, "Create Expressions"},
  {PERM_MANAGE_WEBHOOKS, "Manage Webhooks"},
  {PERM_VIEW_AUDIT_LOG, "View Audit Log"},
  {PERM_VIEW_CHANNEL, "View Channels"},
  {PERM_MANAGE_EVENTS, "Manage Events"},
  {PERM_CREATE_EVENTS, "Create Events"},
  {PERM_MODERATE_MEMBERS, "Moderate Members"},
  {PERM_VIEW_GUILD_INSIGHTS, "View Server Insights"},
  // VIEW_CREATOR_MONETIZATION_ANALYTICS.
  {1ull << 41, "View Server Subscription Insights"},
  {PERM_USE_EXTERNAL_APPS, "Use External Apps"},
  {PERM_SEND_MESSAGES, "Send Messages"},
  {PERM_SEND_MESSAGES_IN_THREADS, "Send Messages in Threads"},
  {PERM_CREATE_PUBLIC_THREADS, "Create Public Threads"},
  {PERM_CREATE_PRIVATE_THREADS, "Create Private Threads"},
  {PERM_SEND_TTS_MESSAGES, "Send TTS Messages"},
  {PERM_MANAGE_MESSAGES, "Manage Messages"},
  {PERM_MANAGE_THREADS, "Manage Threads"},
  {PERM_EMBED_LINKS, "Embed Links"},
  {PERM_ATTACH_FILES, "Attach Files"},
  {PERM_READ_MESSAGE_HISTORY, "Read Message History"},
  {PERM_MENTION_EVERYONE, "Mention @everyone, @here, and All Roles"},
  {PERM_ADD_REACTIONS, "Add Reactions"},
  {PERM_USE_EXTERNAL_EMOJIS, "Use External Emoji"},
  {PERM_USE_EXTERNAL_STICKERS, "Use External Stickers"},
  {PERM_USE_APPLICATION_COMMANDS, "Use Application Commands"},
  {PERM_SEND_VOICE_MESSAGES, "Send Voice Message"},
  {PERM_SEND_POLLS, "Create Polls"},
  {PERM_PIN_MESSAGES, "Pin Messages"},
  {PERM_BYPASS_SLOWMODE, "Bypass Slowmode"},
  // MANAGE_OFFICIAL_MESSAGES.
  {1ull << 53, "Manage Official Messages"},
  {PERM_CONNECT, "Connect"},
  {PERM_SPEAK, "Speak"},
  {PERM_MUTE_MEMBERS, "Mute Members"},
  {PERM_DEAFEN_MEMBERS, "Deafen Members"},
  {PERM_MOVE_MEMBERS, "Move Members"},
  {PERM_USE_VAD, "Use Voice Activity"},
  {PERM_PRIORITY_SPEAKER, "Priority Speaker"},
  {PERM_REQUEST_TO_SPEAK, "Request to Speak"},
  {PERM_STREAM, "Video"},
  {PERM_USE_EMBEDDED_ACTIVITIES, "Use Activities"},
  {PERM_USE_SOUNDBOARD, "Use Soundboard"},
  {PERM_USE_EXTERNAL_SOUNDS, "Use External Sounds"},
  {PERM_SET_VOICE_CHANNEL_STATUS, "Set Voice Channel Status"},
};

// what a scope lets an application do, as the client words it, in the
// order it lists them.
static const struct {
  const char *scope, *allows;
} SCOPES[] = {
  {"bot", "Add a bot to a server"},
  {"identify", "Access your username, avatar, and banner"},
  {"email", "Access your email address"},
  {"connections", "Access your third-party connections"},
  {"guilds", "Know what servers you're in"},
  {"guilds.join", "Join servers for you"},
  {"guilds.members.read",
   "Read your member info (nickname, avatar, roles, etc...) for servers you "
   "belong to"},
  {"applications.commands", "Create commands"},
  {"applications.commands.update", "Update commands"},
  {"applications.commands.permissions.update",
   "Manage access to its commands in a server"},
};

// what the client says an application will not do, one picked each time.
static const char *const JOKES[] = {
  "Bake a cake",
  "Buy you a nice seafood dinner",
  "Have an existential crisis",
  "Microbrew some local kombucha",
  "Solve a mystery with Scooby and the gang",
  "Record a new mixtape",
  "Paint a happy little tree",
  "Read you a bedtime story",
};

typedef enum : uint8_t {
  STEP_INSTALL,
  STEP_SCOPES,
  STEP_PERMISSIONS,
} Step;

typedef struct AuthorizeUi {
  bool open;
  Snowflake client_id;
  char *name;
  // what each way of installing asks for, the scopes null for a way not
  // offered.
  char *scopes[APP_INSTALLS];
  Permissions permissions[APP_INSTALLS];
  Step step;
  // the server picked, 0 for none yet, and the permissions the user took
  // back from the bot.
  Snowflake guild_id;
  Permissions denied;
  Dropdown servers;
  const char *joke;
  hcaptcha *captcha;
} AuthorizeUi;

const AuthorizePermission *authorize_permissions(size_t *count) {
  *count = countof(PERMISSIONS);
  return PERMISSIONS;
}

static void drop_captcha(AuthorizeUi *au) {
  if (au->captcha) {
    hcaptcha_free(au->captcha);
    au->captcha = nullptr;
  }
}

// forgets what the dialog held. what was asked of discord is the
// session's to forget.
static void forget(AuthorizeUi *au) {
  for (size_t i = 0; i < APP_INSTALLS; i++) {
    free(au->scopes[i]);
    au->scopes[i] = nullptr;
  }

  free(au->name);
  au->name = nullptr;
  au->open = false;
  form_dropdown_close(&au->servers);
  drop_captcha(au);
}

static void close_dialog(App *app) {
  if (app->authorize && app->authorize->open) {
    forget(app->authorize);
    oauth_end(session_oauth(app->session));
  }
}

void authorize_free(App *app) {
  if (app->authorize) {
    forget(app->authorize);
    free(app->authorize);
    app->authorize = nullptr;
  }
}

// asks discord about one way of installing.
static void choose(App *app, OauthInstall install) {
  AuthorizeUi *au = app->authorize;

  au->step = STEP_SCOPES;
  au->guild_id = 0;
  au->denied = 0;
  oauth_ask(session_oauth(app->session), au->client_id, au->scopes[install],
            install, au->permissions[install]);
}

void authorize_open(App *app, const AuthorizeRequest *request) {
  if (!app->authorize) {
    app->authorize = calloc(1, sizeof *app->authorize);

    if (!app->authorize) {
      cilo_oom(sizeof *app->authorize);
    }
  }

  AuthorizeUi *au = app->authorize;
  size_t ways = 0;

  close_dialog(app);

  for (size_t i = 0; i < APP_INSTALLS; i++) {
    au->scopes[i] = str_dup(request->installs[i].scopes);
    au->permissions[i] = request->installs[i].permissions;
    ways += au->scopes[i] != nullptr;
  }

  if (!ways || !request->client_id) {
    return;
  }

  au->open = true;
  au->client_id = request->client_id;
  au->name = str_dup(request->name ? request->name : "");
  au->joke = JOKES[(size_t)(civue_ui_current()->now * 1000) % countof(JOKES)];
  app->popover = POPOVER_NONE;

  // an application that goes both ways asks where first.
  if (ways > 1) {
    au->step = STEP_INSTALL;
  }
  else {
    choose(app, au->scopes[OAUTH_USER] ? OAUTH_USER : OAUTH_GUILD);
  }
}

/* ========================================================================== */
/* pieces.                                                                    */
/* ========================================================================== */

static void spinner(civue_color color, float size) {
  civue_ui *ui = civue_ui_current();

  div(size(px(size)), flex_shrink_0) {
    civue_rect r = here();
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    float ring = size / 2 - 3;
    int lead = (int)(ui->now * 10) % 8;

    for (int i = 0; i < 8; i++) {
      float angle = (float)i * 3.14159265f / 4;
      float x = cx + ring * cosf(angle), y = cy + ring * sinf(angle);
      float fade = (float)((i - lead + 8) % 8) / 8;

      fill(xywh(x - 2.5f, y - 2.5f, 5, 5), civue_alpha(color, 1 - .85f * fade),
           2.5f);
    }
  }

  civue_request_repaint_after(ui, 1.0 / 30);
}

static void app_icon(App *app, Snowflake id, const char *hash, const char *name,
                     float size) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  div(flex, items_center, justify_center, size(px(size)), flex_shrink_0,
      rounded(full), overflow_hidden) {
    cdn_application_icon(url, id, hash, (uint32_t)(size * 2));

    if (!*url || !image_fill(app, url, size / 2)) {
      char acronym[32];

      guild_acronym(name && *name ? name : "?", acronym, sizeof acronym);
      acronym[2] = 0;
      fill(here(), k->surface_high, size / 2);
      text(acronym, text_size(px(size >= 64 ? 28 : 16)), font_weight(medium),
           text_color(k->text), whitespace_nowrap);
    }
  }
}

// something the application will be allowed, or will not.
static void allowed(const Look *k, const char *what, bool really) {
  div(flex, items_start, gap(3)) {
    if (really) {
      glyph(CIVUE_ICON(lucide, circle_check), 20, k->positive);
    }
    else {
      glyph(CIVUE_ICON(lucide, circle_x), 20, k->muted);
    }

    text(what, flex_1, min_w(0), pt(px(1)),
         text_color(really ? k->text : k->muted));
  }
}

// a line of what is known about the application.
static void detail(const Look *k, civue_icon glyph_icon, const char *line) {
  div(flex, items_start, gap(2)) {
    glyph(glyph_icon, 16, k->subtle);
    text(line, flex_1, min_w(0), text_size(px(12)), line_height(px(16)),
         text_color(k->subtle));
  }
}

static void count_with_commas(uint32_t n, char *out, size_t cap) {
  char digits[16];
  int len = snprintf(digits, sizeof digits, "%u", n);
  size_t at = 0;

  for (int i = 0; i < len && at + 2 < cap; i++) {
    if (i > 0 && (len - i) % 3 == 0) {
      out[at++] = ',';
    }

    out[at++] = digits[i];
  }

  out[at] = 0;
}

// what the user may grant in a server: an administrator may all of it.
static Permissions grantable(const OauthGuild *g) {
  return g->permissions & PERM_ADMINISTRATOR ? PERM_ALL : g->permissions;
}

static const OauthGuild *guild_of(const Oauth *o, Snowflake id) {
  for (size_t i = 0; i < o->nguilds; i++) {
    if (o->guilds[i].id == id) {
      return &o->guilds[i];
    }
  }

  return nullptr;
}

static int by_name(const void *a, const void *b) {
  const OauthGuild *const *x = a, *const *y = b;

  return strcasecmp((*x)->name, (*y)->name);
}

/* ========================================================================== */
/* the steps.                                                                 */
/* ========================================================================== */

static bool way(App *app, civue_id id, civue_icon glyph_icon, const char *title,
                const char *about) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(3), p(4), rounded(lg), border(1),
      border_color(k->border), bg(k->surface_high), hover(bg(k->hover)),
      cursor_pointer) {
    pressed = button().activated;
    glyph(glyph_icon, 24, k->text);

    div(flex, flex_col, gap(px(2)), flex_1, min_w(0)) {
      text(title, font_weight(medium), text_color(k->strong));
      text(about, text_size(px(12)), line_height(px(16)), font_weight(medium),
           text_color(k->muted));
    }

    glyph(CIVUE_ICON(lucide, chevron_right), 24, k->subtle);
  }

  return pressed;
}

// where it goes, for an application that may go both ways.
static void step_install(App *app) {
  const Look *k = &app->look;
  AuthorizeUi *au = app->authorize;

  div(flex, flex_col, items_center, gap(2), px(6), pt(6), pb(4)) {
    text(au->name, text_center, text_size(px(18)), line_height(px(22)),
         font_weight(semibold), text_color(k->strong));
  }

  div(flex, flex_col, gap(2), px(6), pb(4)) {
    if (way(app, id("authorize-user"), CIVUE_ICON(lucide, user_plus),
            "Add to My Apps", "Use this app everywhere!")) {
      choose(app, OAUTH_USER);
    }

    if (way(app, id("authorize-guild"), CIVUE_ICON(lucide, server),
            "Add to Server", "Customize your server by adding this app")) {
      choose(app, OAUTH_GUILD);
    }
  }
}

// the application and the account it asks to reach.
static void faces(App *app, const Oauth *o) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const User *self = store_user(store, store->self_id);
  bool reaches = false;

  for (size_t i = 0; i < countof(SCOPES); i++) {
    reaches |= oauth_has_scope(o->scopes, SCOPES[i].scope);
  }

  div(flex, flex_col, items_center, gap(1), px(6), pt(6), pb(4)) {
    div(flex, items_center, gap(4), mb(3)) {
      app_icon(app, o->client_id, o->icon, o->name, ICON);

      div(flex, items_center, gap(1)) {
        for (int i = 0; i < 3; i++) {
          div(size(px(6)), rounded(full), bg(k->faint)) {}
        }
      }

      avatar(app, self, 0, ICON, false, k->dialog);
    }

    div(flex, items_center, justify_center, gap(1), max_w(full)) {
      text(o->name, min_w(0), truncate, text_size(px(18)), line_height(px(22)),
           font_weight(semibold), text_color(k->strong));

      if (o->bot_id && o->bot_verified) {
        glyph(CIVUE_ICON(lucide, badge_check), 16, k->accent_text);
      }
    }

    text(reaches ? "wants to access your Discord account"
                 : "Connect to Discord",
         text_center, text_color(k->text));
  }
}

static void scopes(App *app, const Oauth *o) {
  const Look *k = &app->look;
  AuthorizeUi *au = app->authorize;
  char title[200];

  snprintf(title, sizeof title,
           "This will allow the developer of %s to:", o->name);

  div(flex, flex_col, gap(3)) {
    text(title, text_size(px(12)), line_height(px(16)), font_weight(medium),
         text_color(k->subtle));

    for (size_t i = 0; i < countof(SCOPES); i++) {
      if (oauth_has_scope(o->scopes, SCOPES[i].scope)) {
        allowed(k, SCOPES[i].allows, true);
      }
    }

    // a scope the table does not word is named as discord names it.
    for (const char *at = o->scopes; *at;) {
      size_t len = strcspn(at, " ");
      char scope[80];
      bool worded = false;

      snprintf(scope, sizeof scope, "%.*s", (int)len, at);

      for (size_t i = 0; i < countof(SCOPES); i++) {
        worded |= strcmp(scope, SCOPES[i].scope) == 0;
      }

      if (!worded && *scope) {
        allowed(k, scope, true);
      }

      at += len;
      at += *at == ' ';
    }

    if (o->install == OAUTH_USER &&
        oauth_has_scope(o->scopes, "applications.commands")) {
      allowed(k, "Send you direct messages", true);
    }

    allowed(k, au->joke, false);
  }
}

// the server it goes in, among the ones the user manages.
static void servers(App *app, const Oauth *o) {
  const Look *k = &app->look;
  AuthorizeUi *au = app->authorize;
  const OauthGuild **offered = calloc(o->nguilds + 1, sizeof *offered);
  const char **names = calloc(o->nguilds + 1, sizeof *names);
  size_t count = 0;
  int current = -1;

  if (!offered || !names) {
    cilo_oom((o->nguilds + 1) * sizeof *offered);
  }

  for (size_t i = 0; i < o->nguilds; i++) {
    if (grantable(&o->guilds[i]) & PERM_MANAGE_GUILD) {
      offered[count++] = &o->guilds[i];
    }
  }

  qsort(offered, count, sizeof *offered, by_name);

  for (size_t i = 0; i < count; i++) {
    names[i] = offered[i]->name;

    if (offered[i]->id == au->guild_id) {
      current = (int)i;
    }
  }

  div(flex, flex_col, gap(2)) {
    text("Add to server:", font_weight(medium), text_color(k->strong));

    int chosen = form_select(app, &au->servers, id("authorize-server"), names,
                             count, current, "Select a server", o->sending);

    if (chosen >= 0 && (size_t)chosen < count) {
      au->guild_id = offered[chosen]->id;
      au->denied = 0;
    }

    text("This requires you to have Manage Server permission in the server.",
         text_size(px(12)), line_height(px(16)), text_color(k->subtle));
  }

  free(offered);
  free(names);
}

static void details(App *app, const Oauth *o) {
  const Look *k = &app->look;
  char line[320], day[32], count[24];
  bool writes = oauth_has_scope(o->scopes, "dm_channels.messages.write") ||
                oauth_has_scope(o->scopes, "sdk.social_layer");
  bool reads = strstr(o->scopes, "messages.read") != nullptr;

  div(flex, flex_col, gap(2), pt(4), border_t(1), border_color(k->border)) {
    snprintf(line, sizeof line,
             "The developer of %s's privacy policy and terms of service apply "
             "to this application. Discord's Privacy Policy applies to data "
             "it receives.",
             o->name);
    detail(k, CIVUE_ICON(lucide, lock), line);

    format_short_day(snowflake_time_ms(o->client_id), day, sizeof day);
    snprintf(line, sizeof line, "Active since %s", day);
    detail(k, CIVUE_ICON(lucide, clock), line);

    if (oauth_has_scope(o->scopes, "bot") && o->guild_count) {
      count_with_commas(o->guild_count, count, sizeof count);
      snprintf(line, sizeof line, "Used in %s servers", count);
      detail(k, CIVUE_ICON(lucide, bot), line);
    }

    detail(k, CIVUE_ICON(lucide, shield),
           writes  ? "This application can read and send messages as you."
           : reads ? "This application can read your messages but can not "
                     "send messages as you."
                   : "This application cannot read your messages or send "
                     "messages as you.");

    if (o->sees_ip) {
      detail(k, CIVUE_ICON(lucide, globe),
             "Developer may see your IP address while you use this "
             "application.");
    }

    if (o->shows_ads) {
      detail(k, CIVUE_ICON(lucide, megaphone),
             "Developer may show ads while you use this application.");
    }

    if (o->terms_url || o->privacy_url) {
      div(flex, items_center, gap(3), pl(6), text_size(px(12)),
          line_height(px(16)), text_color(k->link)) {
        if (o->privacy_url) {
          div(key(id("authorize-privacy")), hover(text_color(k->primary_hover)),
              cursor_pointer) {
            if (button().activated) {
              civue_open_url(o->privacy_url);
            }

            text("Privacy policy", whitespace_nowrap);
          }
        }

        if (o->terms_url) {
          div(key(id("authorize-terms")), hover(text_color(k->primary_hover)),
              cursor_pointer) {
            if (button().activated) {
              civue_open_url(o->terms_url);
            }

            text("Terms of service", whitespace_nowrap);
          }
        }
      }
    }
  }
}

// what the bot may do in the server picked, each of it the user's to take
// back, then what the user cannot grant there.
static void step_permissions(App *app, const Oauth *o) {
  const Look *k = &app->look;
  AuthorizeUi *au = app->authorize;
  const OauthGuild *g = guild_of(o, au->guild_id);
  Permissions may = g ? grantable(g) : 0;
  char line[320];

  div(flex, flex_col, gap(4), px(6), pt(6), pb(4), max_h(px(460)),
      overflow_y_scroll, scrollbars(&app->thin_bars)) {
    if (o->permissions & may) {
      snprintf(line, sizeof line,
               "Confirm that you want to grant %s the following permissions "
               "on %s:",
               o->name, g ? g->name : "");
      text(line, font_weight(medium), text_color(k->text));

      for (size_t i = 0; i < countof(PERMISSIONS); i++) {
        Permissions bit = PERMISSIONS[i].bit;

        if (!(o->permissions & may & bit)) {
          continue;
        }

        div(key(id_at(id("authorize-permission"), i)), flex, items_center,
            gap(3), cursor_pointer) {
          bool row = button().activated;

          if (form_checkbox(app, id_at(id("authorize-check"), i),
                            !(au->denied & bit)) ||
              row) {
            au->denied ^= bit;
          }

          text(PERMISSIONS[i].name, flex_1, min_w(0), text_color(k->text));
        }
      }
    }

    if (o->permissions & ~may) {
      snprintf(line, sizeof line,
               "%s has also asked for the following permissions, but you "
               "can't grant them:",
               o->name);
      text(line, font_weight(medium), text_color(k->text));

      for (size_t i = 0; i < countof(PERMISSIONS); i++) {
        if (o->permissions & ~may & PERMISSIONS[i].bit) {
          div(flex, items_center, gap(3)) {
            glyph(CIVUE_ICON(lucide, x), 16, k->muted);
            text(PERMISSIONS[i].name, flex_1, min_w(0), text_color(k->muted));
          }
        }
      }
    }
  }
}

// the challenge discord holds the answer back for.
static void challenge(App *app, Oauth *o) {
  const Look *k = &app->look;
  AuthorizeUi *au = app->authorize;

  if (strcmp(o->captcha.service, "hcaptcha") || !o->captcha.sitekey) {
    text("Discord asks for a challenge Quarrel cannot show.", p(6),
         text_color(k->critical_text));
    return;
  }

  if (!au->captcha) {
    au->captcha = hcaptcha_new(&(hcaptcha_options){
      .sitekey = o->captcha.sitekey,
      .origin = "https://discord.com",
      .rqdata = o->captcha.rqdata,
      .theme = "dark",
    });
  }

  hcaptcha_state state = {.status = HCAPTCHA_FAILED,
                          .error = "web views are missing here"};

  div(key(id("authorize-captcha")), w(px(400)), h(px(460)), mx(auto), my(4)) {
    if (au->captcha) {
      state = hcaptcha_node(civue_ui_current(), au->captcha);
    }
  }

  if (state.status == HCAPTCHA_SOLVED && state.token) {
    oauth_answer_captcha(o, state.token);
    drop_captcha(au);
  }
  else if (state.status == HCAPTCHA_CANCELLED ||
           state.status == HCAPTCHA_EXPIRED ||
           state.status == HCAPTCHA_FAILED) {
    drop_captcha(au);
    oauth_drop_captcha(o);
  }
}

/* ========================================================================== */
/* the dialog.                                                                */
/* ========================================================================== */

void authorize_dialog(App *app) {
  AuthorizeUi *au = app->authorize;

  if (!au || !au->open) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Oauth *o = session_oauth(app->session);
  bool asking = au->step != STEP_INSTALL;

  // another account took the window since it opened.
  if (asking && o->client_id != au->client_id) {
    forget(au);
    return;
  }

  bool in_guild = asking && o->install == OAUTH_GUILD &&
                  (oauth_has_scope(o->scopes, "bot") ||
                   oauth_has_scope(o->scopes, "applications.commands"));
  const OauthGuild *g = asking ? guild_of(o, au->guild_id) : nullptr;
  bool close = false, back = false, yes = false, onward = false, go = false;
  // the cancel button tells discord no, as the client's does. closing the
  // dialog another way tells it nothing.
  bool refuse = false;

  // its makers' page, when they set one, opens once discord let it in.
  if (asking && o->done && o->redirect) {
    civue_open_url(o->redirect);
    close_dialog(app);
    return;
  }

  overlay() {
    div(key(id("authorize-shade")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(k->scrim)) {
      if (button().activated && !(asking && o->sending)) {
        close = true;
      }

      // each step's parts lay their own paddings: 24 from the edges and
      // 16 apart, as a shadcn dialog's.
      div(key(id("authorize")), flex, flex_col, w(px(dialog_width())),
          max_h(px(ui->size.y - 48)), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_size(px(14)), line_height(px(20)), text_color(k->text),
          overflow_hidden) {
        button();

        if (!asking) {
          step_install(app);

          div(flex, justify_end, flex_shrink_0, px(6), pb(6)) {
            close |= form_button(app, id("authorize-cancel"), "Cancel",
                                 BUTTON_OUTLINE, false);
          }
        }
        else if (o->done) {
          char line[320];

          if (g) {
            snprintf(line, sizeof line,
                     "%s has been authorized and added to %s.", o->name,
                     g->name);
          }
          else {
            snprintf(line, sizeof line,
                     "%s has been authorized and added to your apps!", o->name);
          }

          div(flex, flex_col, items_center, gap(2), px(6), pt(6), pb(4)) {
            glyph(CIVUE_ICON(lucide, circle_check), 48, k->positive);
            text("Success!", mt(2), text_size(px(18)), line_height(px(22)),
                 font_weight(semibold), text_color(k->strong));
            text(line, text_center, text_color(k->subtle));
          }

          div(flex, justify_end, gap(2), flex_shrink_0, px(6), pb(6)) {
            close |= form_button(app, id("authorize-close"), "Close",
                                 BUTTON_OUTLINE, false);

            if (g && store_guild(app->session->store, g->id)) {
              go = form_button(app, id("authorize-go"), "Go to Server",
                               BUTTON_PRIMARY, false);
            }
          }
        }
        else if (o->error) {
          div(flex, flex_col, px(6), pt(6), pb(4)) {
            dialog_header(k, "Error", o->error);
          }

          div(flex, justify_end, flex_shrink_0, px(6), pb(6)) {
            close |= form_button(app, id("authorize-close"), "Close",
                                 BUTTON_OUTLINE, false);
          }
        }
        else if (!o->ready) {
          div(flex, items_center, justify_center, h(px(240))) {
            spinner(k->subtle, 32);
          }
        }
        else if (o->captcha.service) {
          challenge(app, o);
        }
        else {
          bool bot = oauth_has_scope(o->scopes, "bot");
          bool missing = in_guild && !g;
          // the bot's permissions are their own step.
          bool more = au->step == STEP_SCOPES && bot && o->permissions && g;

          if (au->step == STEP_PERMISSIONS) {
            step_permissions(app, o);
          }
          else {
            faces(app, o);

            // as tall as what it holds, up to what the window leaves it.
            div(flex, flex_col, gap(4), flex_shrink_0, px(6), pb(4),
                max_h(px(fmaxf(160, (float)ui->size.y - 340))),
                overflow_y_scroll, scrollbars(&app->thin_bars)) {
              scopes(app, o);

              if (in_guild) {
                servers(app, o);
              }

              details(app, o);
            }
          }

          div(flex, justify_between, gap(2), flex_shrink_0, px(6), pt(4), pb(6),
              border_t(1), border_color(k->border)) {
            if (au->step == STEP_PERMISSIONS) {
              back = form_button(app, id("authorize-back"), "Back",
                                 BUTTON_OUTLINE, o->sending);
            }
            else {
              refuse = form_button(app, id("authorize-cancel"), "Cancel",
                                   BUTTON_OUTLINE, o->sending);
            }

            if (more) {
              onward = form_button(app, id("authorize-next"), "Continue",
                                   BUTTON_PRIMARY, false);
            }
            else {
              yes = form_button(app, id("authorize-yes"),
                                o->sending ? "Authorizing..."
                                : missing  ? "You missed some fields"
                                           : "Authorize",
                                BUTTON_PRIMARY, o->sending || missing);
            }
          }
        }
      }
    }
  }

  // escape closes the list of servers before it closes the dialog.
  bool listing = au->servers.open != 0;

  form_dropdowns(app, &au->servers);

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) && !(asking && o->sending) &&
      !listing) {
    close = true;
  }

  if (back) {
    au->step = STEP_SCOPES;
  }
  else if (onward) {
    au->step = STEP_PERMISSIONS;
    // what the user cannot grant is not asked for.
    au->denied = o->permissions & ~grantable(g);
  }
  else if (yes) {
    Permissions may = g ? grantable(g) : PERM_ALL;

    oauth_answer(o, true, in_guild ? au->guild_id : 0,
                 o->permissions & may & ~au->denied);
  }
  else if (go) {
    Snowflake guild_id = g->id;

    close_dialog(app);
    app_select_guild(app, guild_id);
  }
  else if (refuse) {
    oauth_answer(o, false, 0, 0);
    close_dialog(app);
  }
  else if (close) {
    close_dialog(app);
  }
}
