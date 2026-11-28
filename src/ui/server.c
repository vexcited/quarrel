#include "server.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

/* ========================================================================== */
/* the layer, as discord's StandardSidebarView lays it out: a sidebar region  */
/* growing from the left whose 264 points of items sit at its right, then the */
/* page, a column of 740 at most, and the close button beside it.             */
/* ========================================================================== */

constexpr float SIDEBAR = 264;
constexpr float COLUMN = 740;
constexpr float TOOLS = 60;
// what discord takes for an emoji.
constexpr size_t EMOJI_MOST = 256u << 10;

static const char *const IMAGE_TYPES[] = {"png", "jpg", "jpeg", "gif", "webp"};

// the fields each page saves, sent whether they changed or not as discord's
// client does, images only once changed.
constexpr uint32_t IMAGE_FIELDS =
  GUILD_EDIT_ICON | GUILD_EDIT_BANNER | GUILD_EDIT_SPLASH;
constexpr uint32_t PROFILE_FIELDS =
  GUILD_EDIT_NAME | GUILD_EDIT_DESCRIPTION | GUILD_EDIT_ICON;
constexpr uint32_t ENGAGEMENT_FIELDS =
  GUILD_EDIT_SYSTEM_CHANNEL | GUILD_EDIT_SYSTEM_FLAGS | GUILD_EDIT_AFK_CHANNEL |
  GUILD_EDIT_AFK_TIMEOUT | GUILD_EDIT_NOTIFICATIONS;
constexpr uint32_t BOOST_FIELDS =
  GUILD_EDIT_PROGRESS_BAR | GUILD_EDIT_BANNER | GUILD_EDIT_SPLASH;
constexpr uint32_t COMMUNITY_FIELDS =
  GUILD_EDIT_RULES_CHANNEL | GUILD_EDIT_UPDATES_CHANNEL |
  GUILD_EDIT_SAFETY_CHANNEL | GUILD_EDIT_LOCALE | GUILD_EDIT_DESCRIPTION |
  GUILD_EDIT_FEATURES;

ServerUi *server_ui(App *app) {
  if (app->server == nullptr) {
    app->server = calloc(1, sizeof(ServerUi));
    form_nag_reset(&app->server->nag);
  }

  return app->server;
}

/* ========================================================================== */
/* the draft of the guild.                                                    */
/* ========================================================================== */

static void draft_clear(GuildDraft *d) {
  free(d->name);
  free(d->description);
  free(d->locale);
  free(d->icon_path);
  free(d->banner_path);
  free(d->splash_path);
  *d = (GuildDraft){};
}

static void draft_take(GuildDraft *d, const Guild *g) {
  draft_clear(d);
  d->taken = true;
  d->stamp = g->version;
  d->name = str_dup(g->name);
  d->description = str_dup(g->description);
  d->locale = str_dup(g->preferred_locale ? g->preferred_locale : "en-US");
  d->afk_channel_id = g->afk_channel_id;
  d->afk_timeout = g->afk_timeout ? g->afk_timeout : 300;
  d->system_channel_id = g->system_channel_id;
  d->system_flags = g->system_channel_flags;
  d->notifications = g->default_notifications;
  d->progress_bar = g->premium_progress_bar;
  d->rules_channel_id = g->rules_channel_id;
  d->updates_channel_id = g->public_updates_channel_id;
  d->safety_channel_id = g->safety_alerts_channel_id;
}

static bool same_text(const char *a, const char *b) {
  return strcmp(a ? a : "", b ? b : "") == 0;
}

// what the draft changes of the guild, as the fields of an edit.
static uint32_t draft_changes(const GuildDraft *d, const Guild *g) {
  uint32_t f = 0;

  if (!d->taken) {
    return 0;
  }

  f |= !same_text(d->name, g->name) ? GUILD_EDIT_NAME : 0;
  f |= !same_text(d->description, g->description) ? GUILD_EDIT_DESCRIPTION
                                                  : 0;
  f |= !same_text(d->locale,
                  g->preferred_locale ? g->preferred_locale : "en-US")
         ? GUILD_EDIT_LOCALE
         : 0;
  f |= d->icon_path || d->icon_removed ? GUILD_EDIT_ICON : 0;
  f |= d->banner_path || d->banner_removed ? GUILD_EDIT_BANNER : 0;
  f |= d->splash_path || d->splash_removed ? GUILD_EDIT_SPLASH : 0;
  f |= d->afk_channel_id != g->afk_channel_id ? GUILD_EDIT_AFK_CHANNEL : 0;
  f |= d->afk_timeout != (g->afk_timeout ? g->afk_timeout : 300)
         ? GUILD_EDIT_AFK_TIMEOUT
         : 0;
  f |= d->system_channel_id != g->system_channel_id ? GUILD_EDIT_SYSTEM_CHANNEL
                                                    : 0;
  f |= d->system_flags != g->system_channel_flags ? GUILD_EDIT_SYSTEM_FLAGS : 0;
  f |= d->notifications != g->default_notifications ? GUILD_EDIT_NOTIFICATIONS
                                                    : 0;
  f |= d->progress_bar != g->premium_progress_bar ? GUILD_EDIT_PROGRESS_BAR : 0;
  f |= d->rules_channel_id != g->rules_channel_id ? GUILD_EDIT_RULES_CHANNEL
                                                  : 0;
  f |= d->updates_channel_id != g->public_updates_channel_id
         ? GUILD_EDIT_UPDATES_CHANNEL
         : 0;
  f |= d->safety_channel_id != g->safety_alerts_channel_id
         ? GUILD_EDIT_SAFETY_CHANNEL
         : 0;
  f |= d->community_off ? GUILD_EDIT_FEATURES : 0;

  return f;
}

static uint32_t page_fields(ServerPage page) {
  switch (page) {
    case SERVER_PROFILE:
      return PROFILE_FIELDS;
    case SERVER_ENGAGEMENT:
      return ENGAGEMENT_FIELDS;
    case SERVER_BOOST:
      return BOOST_FIELDS;
    case SERVER_COMMUNITY:
      return COMMUNITY_FIELDS;
    default:
      return 0;
  }
}

static bool page_dirty(const ServerUi *ui, const Guild *g) {
  if (ui->page == SERVER_ROLES) {
    return roles_dirty(ui, g);
  }

  return (draft_changes(&ui->draft, g) & page_fields(ui->page)) != 0;
}

// the features of the guild with `feature` added, or without the ones
// community brings when `community` is false.
static size_t features_for(const Guild *g, bool community, const char **out,
                           size_t cap) {
  size_t n = 0;

  for (size_t i = 0; i < g->nfeatures && n < cap; i++) {
    const char *f = g->features[i];
    bool brought = strcmp(f, "COMMUNITY") == 0 ||
                   strcmp(f, "DISCOVERABLE") == 0 ||
                   strcmp(f, "PREVIEW_ENABLED") == 0;

    if (!brought || (community && strcmp(f, "COMMUNITY") == 0)) {
      out[n++] = f;
    }
  }

  if (community && !store_guild_has(g, "COMMUNITY") && n < cap) {
    out[n++] = "COMMUNITY";
  }

  return n;
}

static void draft_save(App *app, ServerUi *ui, const Guild *g) {
  GuildDraft *d = &ui->draft;
  uint32_t changes = draft_changes(d, g);
  uint32_t mask = page_fields(ui->page);
  uint32_t fields = (mask & ~IMAGE_FIELDS & ~GUILD_EDIT_FEATURES) |
                    (changes & mask & (IMAGE_FIELDS | GUILD_EDIT_FEATURES));
  char *icon = nullptr, *banner = nullptr, *splash = nullptr;
  const char *features[64];

  free(ui->error);
  ui->error = nullptr;

  if ((fields & GUILD_EDIT_ICON) && d->icon_path != nullptr) {
    icon = admin_image_uri(d->icon_path, IMAGE_MOST, &ui->error);
  }

  if ((fields & GUILD_EDIT_BANNER) && d->banner_path != nullptr) {
    banner = admin_image_uri(d->banner_path, IMAGE_MOST, &ui->error);
  }

  if ((fields & GUILD_EDIT_SPLASH) && d->splash_path != nullptr) {
    splash = admin_image_uri(d->splash_path, IMAGE_MOST, &ui->error);
  }

  if (ui->error == nullptr) {
    GuildEdit edit = {
      .fields = fields,
      .name = d->name,
      .description = d->description && *d->description ? d->description
                                                       : nullptr,
      .locale = d->locale,
      .icon = icon,
      .banner = banner,
      .splash = splash,
      .afk_channel_id = d->afk_channel_id,
      .afk_timeout = d->afk_timeout,
      .system_channel_id = d->system_channel_id,
      .system_flags = d->system_flags,
      .notifications = d->notifications,
      .progress_bar = d->progress_bar,
      .rules_channel_id = d->community_off ? 0 : d->rules_channel_id,
      .updates_channel_id = d->community_off ? 0 : d->updates_channel_id,
      .safety_channel_id = d->safety_channel_id,
      .features = features,
      .nfeatures = features_for(g, !d->community_off, features, 64),
    };

    admin_edit_guild(app->session, g->id, &edit);
  }

  free(icon);
  free(banner);
  free(splash);
}

/* ========================================================================== */
/* opening and closing.                                                       */
/* ========================================================================== */

static void forget_page(ServerUi *ui) {
  form_dropdown_close(&ui->dropdown);
  draft_clear(&ui->draft);
  roles_forget(ui);
  ui->community_open = false;
  ui->confirm = CONFIRM_NONE;
  ui->emoji_id = 0;
  free(ui->error);
  free(ui->confirm_text);
  free(ui->emoji_name);
  free(ui->member_search);
  ui->error = ui->confirm_text = ui->emoji_name = ui->member_search = nullptr;
}

void server_settings_open(App *app, Snowflake guild_id, ServerPage page) {
  ServerUi *ui = server_ui(app);
  Admin *a = admin_of(app->session, guild_id);

  forget_page(ui);
  form_nag_reset(&ui->nag);
  admin_clear(a, &a->save);
  admin_clear(a, &a->leave);
  ui->open = true;
  ui->guild_id = guild_id;
  ui->page = page;
  app->popover = POPOVER_NONE;

  // the composer under the layer keeps no keys, escape among them.
  if (civue_ui_current() != nullptr) {
    ui->opened_at = civue_ui_current()->now;
    civue_ui_current()->focus = 0;
  }
}

static void close_settings(ServerUi *ui) {
  forget_page(ui);
  ui->open = false;
}

void server_ui_free(App *app) {
  ServerUi *ui = app->server;

  if (ui == nullptr) {
    return;
  }

  forget_page(ui);
  free(ui->roles);
  free(ui->order);
  free(ui->create.name);
  free(ui->create.search);
  free(ui->create.roles.v);
  free(ui->create.users.v);
  free(ui->picks.v);
  free(ui->rename);
  free(ui->pending);
  free(ui);
  app->server = nullptr;
}

bool server_can_manage(App *app, const Guild *g) {
  Permissions p = store_guild_permissions(app->session->store, g);

  return (p & (PERM_MANAGE_GUILD | PERM_MANAGE_ROLES | PERM_ADMINISTRATOR |
               PERM_BAN_MEMBERS | PERM_MANAGE_NICKNAMES |
               PERM_CREATE_EXPRESSIONS | PERM_MANAGE_EXPRESSIONS |
               PERM_MANAGE_WEBHOOKS | PERM_VIEW_AUDIT_LOG |
               PERM_VIEW_GUILD_INSIGHTS)) != 0;
}

bool server_covers(App *app) {
  const ServerUi *ui = app->server;

  return ui != nullptr &&
         (ui->open || ui->create.open || ui->renaming != 0 || ui->menu_open ||
          ui->confirm != CONFIRM_NONE);
}

void server_leave_ask(App *app, Snowflake guild_id) {
  ServerUi *ui = server_ui(app);
  Admin *a = admin_of(app->session, guild_id);

  admin_clear(a, &a->leave);
  ui->confirm = CONFIRM_LEAVE_GUILD;
  ui->confirm_id = guild_id;
  app->popover = POPOVER_NONE;
}

// whether the user may see a page, as discord's sidebar decides.
static bool page_allowed(App *app, const Guild *g, ServerPage page) {
  Permissions p = store_guild_permissions(app->session->store, g);

  switch (page) {
    case SERVER_PROFILE:
    case SERVER_ENGAGEMENT:
    case SERVER_BOOST:
      return true;
    case SERVER_ROLES:
      return p & PERM_MANAGE_ROLES;
    case SERVER_EMOJI:
      return p & (PERM_MANAGE_EXPRESSIONS | PERM_CREATE_EXPRESSIONS);
    case SERVER_BANS:
      return p & (PERM_BAN_MEMBERS | PERM_VIEW_AUDIT_LOG);
    default:
      return p & PERM_MANAGE_GUILD;
  }
}

static bool manages_guild(App *app, const Guild *g) {
  return store_guild_permissions(app->session->store, g) & PERM_MANAGE_GUILD;
}

/* ========================================================================== */
/* the sidebar.                                                               */
/* ========================================================================== */

static bool nav_item(App *app, civue_id id, const char *label, bool active,
                     bool destructive) {
  const Look *k = &app->look;
  bool pressed = false;
  civue_color color = destructive ? k->critical_text : k->subtle;

  div(key(id), flex, items_center, flex_shrink_0, h(8), px(px(10)), mb(px(2)),
      rounded(md), text_color(active ? k->strong : color),
      style(.bg = active ? k->selected : CIVUE_CLEAR),
      hover(bg(destructive ? civue_alpha(k->danger, .08f)
               : active    ? k->selected
                           : k->hover),
            text_color(destructive ? k->critical_text : k->strong)),
      active(bg(destructive ? civue_alpha(k->danger, .08f) : k->selected)),
      style(.cursor = active ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    pressed = button().activated;
    text(label, flex_1, min_w(0), truncate, font_weight(medium));
  }

  return pressed;
}

static void nav_header(const Look *k, const char *label, bool first) {
  text(label, flex_shrink_0, truncate, px(px(10)), pb(px(6)),
       text_size(px(14)), line_height(px(18)), font_weight(medium),
       text_color(k->faint), style(.padding[0] = civue_px(first ? 0 : 6)));
}

static void nav_separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), mx(px(10)), my(2), bg(k->border)) {}
}

typedef struct {
  int page;
  bool delete_guild;
} NavChoice;

static void nav_page(App *app, ServerUi *ui, const Guild *g, ServerPage page,
                     const char *label, NavChoice *choice) {
  if (!page_allowed(app, g, page)) {
    return;
  }

  if (nav_item(app, id_at(id("server-nav"), page), label, ui->page == page,
               false)) {
    choice->page = page;
  }
}

static NavChoice sidebar(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  Permissions p = store_guild_permissions(app->session->store, g);
  NavChoice choice = {.page = -1};
  bool owner = g->owner_id == app->session->store->self_id;

  div(flex, justify_end, flex_shrink_0, h(full), bg(k->frame),
      style(.grow = 1, .basis = civue_px(SIDEBAR))) {
    div(key(id("server-nav")), flex, flex_col, w(px(SIDEBAR)), h(full),
        overflow_y_scroll, scrollbars(&app->thin_bars), pt(px(60)),
        pb(px(60)), pl(px(20)), pr(px(6))) {
      nav_header(k, g->name && *g->name ? g->name : "Server Settings", true);
      nav_page(app, ui, g, SERVER_PROFILE, "Server Profile", &choice);
      nav_page(app, ui, g, SERVER_ENGAGEMENT, "Engagement", &choice);
      nav_page(app, ui, g, SERVER_BOOST, "Boost Perks", &choice);

      if (p & (PERM_MANAGE_EXPRESSIONS | PERM_CREATE_EXPRESSIONS)) {
        nav_separator(k);
        nav_header(k, "Expression", false);
        nav_page(app, ui, g, SERVER_EMOJI, "Emoji", &choice);
      }

      if (p & (PERM_MANAGE_GUILD | PERM_MANAGE_ROLES | PERM_ADMINISTRATOR)) {
        nav_separator(k);
        nav_header(k, "People", false);
        nav_page(app, ui, g, SERVER_ROLES, "Roles", &choice);
        nav_page(app, ui, g, SERVER_INVITES, "Invites", &choice);
      }

      if (p & (PERM_MANAGE_GUILD | PERM_VIEW_AUDIT_LOG | PERM_BAN_MEMBERS)) {
        nav_separator(k);
        nav_header(k, "Moderation", false);
        nav_page(app, ui, g, SERVER_SAFETY, "Safety Setup", &choice);
        nav_page(app, ui, g, SERVER_BANS, "Bans", &choice);
      }

      if (p & PERM_MANAGE_GUILD) {
        nav_separator(k);
        nav_page(app, ui, g, SERVER_COMMUNITY,
                 g->community ? "Community Overview" : "Enable Community",
                 &choice);
      }

      if (owner) {
        nav_separator(k);
        choice.delete_guild = nav_item(app, id("server-nav-delete"),
                                       "Delete Server", false, true);
      }
    }
  }

  return choice;
}

/* ========================================================================== */
/* pieces of pages.                                                           */
/* ========================================================================== */

// the text channels, or the voice ones, as the choices of a dropdown after
// `extra` choices of their own. `ids` and `names` hold `cap`. how many.
static size_t channel_choices(App *app, const Guild *g, bool voice,
                              const char *const *extra,
                              const Snowflake *extra_ids, size_t nextra,
                              Snowflake *ids, const char **names, size_t cap) {
  Store *store = app->session->store;
  size_t count = 0, n = 0;
  const ChannelRow *rows = store_guild_channels(store, (Guild *)g, &count);

  for (size_t i = 0; i < nextra && n < cap; i++) {
    ids[n] = extra_ids[i];
    names[n++] = extra[i];
  }

  for (size_t i = 0; i < count && n < cap; i++) {
    const Channel *c = rows[i].channel;
    bool fits = voice ? c->type == CHANNEL_GUILD_VOICE
                      : c->type == CHANNEL_GUILD_TEXT;

    if (rows[i].kind == CHANNEL_ROW_CHANNEL && fits) {
      ids[n] = c->id;
      names[n++] = c->name ? c->name : "";
    }
  }

  return n;
}

// a dropdown of channels, the one picked written to `*picked`.
static void channel_select(App *app, ServerUi *ui, civue_id id, const Guild *g,
                           bool voice, const char *none, Snowflake *picked,
                           bool disabled) {
  Snowflake ids[256];
  const char *names[256];
  const char *extra[] = {none};
  Snowflake extra_ids[] = {0};
  size_t n = channel_choices(app, g, voice, extra, extra_ids,
                             none != nullptr ? 1 : 0, ids, names, 256);
  int current = -1;

  for (size_t i = 0; i < n; i++) {
    current = ids[i] == *picked ? (int)i : current;
  }

  int chosen = form_select(app, &ui->dropdown, id, names, n, current,
                           "Select a channel", disabled);

  if (chosen >= 0) {
    *picked = ids[chosen];
  }
}

typedef struct {
  char **path;
  bool *removed;
} ImagePick;

static void image_picked(App *, civue_strs files, uint64_t arg) {
  ImagePick *pick = (ImagePick *)(uintptr_t)arg;

  if (files.len) {
    free(*pick->path);
    *pick->path = strdup(files.v[0]);

    if (pick->removed) {
      *pick->removed = false;
    }
  }

  free(pick);
}

void pick_image(App *app, char **path, bool *removed) {
  ImagePick *pick = malloc(sizeof *pick);

  *pick = (ImagePick){path, removed};
  pick_files_later(app, IMAGE_TYPES, countof(IMAGE_TYPES), false,
                   image_picked, (uintptr_t)pick);
}

// what shows for an image: the file picked, nothing once taken off, or the
// guild's own.
static const char *shown_image(const char *url, const char *path, bool removed,
                               char out[static CDN_URL_MAX]) {
  if (path != nullptr) {
    snprintf(out, CDN_URL_MAX, "file://%s", path);
    return out;
  }

  return removed || url == nullptr || *url == 0 ? nullptr : url;
}

/* ========================================================================== */
/* server profile.                                                            */
/* ========================================================================== */

static void profile_preview(App *app, const Guild *g, const GuildDraft *d) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char url[CDN_URL_MAX] = "", local[CDN_URL_MAX];
  char members[64];
  uint32_t online = 0;

  cdn_guild_icon(url, g->id, g->icon, 256);

  const char *icon = shown_image(url, d->icon_path, d->icon_removed, local);

  CILO_MAP_EACH(entry, &((Guild *)g)->members) {
    const User *u = store_user(store, entry->val->user_id);
    online += u != nullptr && u->status != STATUS_OFFLINE;
  }

  snprintf(members, sizeof members, "%u Members", g->member_count);

  div(flex, flex_col, w(px(300)), flex_shrink_0, rounded(xl), bg(k->frame),
      border(1), border_color(k->border), overflow_hidden) {
    div(flex_shrink_0, h(px(90)), bg(k->banner)) {}

    div(flex, flex_col, gap(2), px(4), pb(4), mt(px(-36))) {
      div(flex, items_center, justify_center, size(px(72)), flex_shrink_0,
          rounded(px(20)), bg(k->frame), p(1)) {
        div(flex, items_center, justify_center, size(full), rounded(px(16)),
            bg(k->surface_high), overflow_hidden, text_color(k->strong)) {
          if (icon == nullptr || !image_cover(app, icon, 16)) {
            char initial[2] = {d->name && *d->name ? d->name[0] : '?', 0};

            text(initial, text_size(px(24)), font_weight(semibold));
          }
        }
      }

      text(d->name && *d->name ? d->name : "Server", text_size(px(20)),
           line_height(px(24)), font_weight(semibold), text_color(k->strong));

      div(flex, items_center, gap(3), text_size(px(12)), line_height(px(16)),
          text_color(k->subtle)) {
        div(flex, items_center, gap(1)) {
          div(size(2), rounded(full), bg(k->online)) {}
          char line[48];
          snprintf(line, sizeof line, "%u Online", online);
          text(line);
        }

        div(flex, items_center, gap(1)) {
          div(size(2), rounded(full), bg(k->offline)) {}
          text(members);
        }
      }

      if (d->description && *d->description) {
        text(d->description, text_size(px(14)), line_height(px(18)),
             text_color(k->text));
      }
    }
  }
}

static void profile_page(App *app, ServerUi *ui, Guild *g, float wide) {
  const Look *k = &app->look;
  GuildDraft *d = &ui->draft;
  bool editable = manages_guild(app, g);
  bool has_icon = (g->icon != nullptr && !d->icon_removed) ||
                  d->icon_path != nullptr;

  div(flex, items_start, gap(10), flex_shrink_0) {
    div(flex, flex_col, flex_1, min_w(0), max_w(px(560))) {
      form_heading(k, "Server Profile",
                   "Customize how your server appears in invite links and, "
                   "if enabled, in Server Discovery and Announcement Channel "
                   "messages");

      div(flex_shrink_0, h(10)) {}

      form_label(k, "Name");

      if (editable) {
        form_input(app, id("profile-name"), &d->name, "", nullptr, 100);
      }
      else {
        text(d->name ? d->name : "", text_color(k->text));
      }

      form_divider(k);

      form_label(k, "Icon");
      text("We recommend an image of at least 512x512.", flex_shrink_0, mb(4),
           text_size(px(14)), line_height(px(18)), text_color(k->subtle));

      div(flex, gap(2), flex_shrink_0) {
        if (form_button_small(app, id("profile-icon"), "Change Server Icon",
                              BUTTON_PRIMARY, !editable)) {
          pick_image(app, &d->icon_path, &d->icon_removed);
        }

        if (has_icon && form_button_small(app, id("profile-icon-remove"),
                                          "Remove Icon",
                                          BUTTON_DANGER_SECONDARY,
                                          !editable)) {
          free(d->icon_path);
          d->icon_path = nullptr;
          d->icon_removed = g->icon != nullptr;
        }
      }

      form_divider(k);

      form_label(k, "Description");
      text("How did your server get started? Why should people join?",
           flex_shrink_0, mb(4), text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));

      if (editable) {
        form_area(app, id("profile-description"), &d->description,
                  "Tell the world a bit about this server.", 4, 300);
      }
    }

    // the preview is hidden in a narrow window, as discord's.
    if (wide >= 1024) {
      profile_preview(app, g, d);
    }
  }
}

/* ========================================================================== */
/* engagement.                                                                */
/* ========================================================================== */

static const uint32_t AFK_TIMEOUTS[] = {60, 300, 900, 1800, 3600};
static const char *const AFK_NAMES[] = {"1 minute", "5 minutes", "15 minutes",
                                        "30 minutes", "1 hour"};

static void flag_switch(App *app, GuildDraft *d, civue_id id, uint32_t flag,
                        const char *label, bool disabled) {
  bool on = !(d->system_flags & flag);

  div(flex_shrink_0, mb(4)) {
    if (form_switch_row(app, id, label, nullptr, on, disabled)) {
      d->system_flags ^= flag;
    }
  }
}

static void engagement_page(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  GuildDraft *d = &ui->draft;
  bool editable = manages_guild(app, g);
  bool silent = d->system_channel_id == 0 || !editable;

  form_heading(k, "Engagement",
               "Manage settings that help keep your server active.");
  form_divider(k);

  form_legend(k, "System Messages",
              "Configure system event messages sent to your server.");
  div(flex_shrink_0, h(4)) {}

  flag_switch(app, d, id("engagement-join"), SYSTEM_SUPPRESS_JOIN,
              "Send a random welcome message when someone joins this server.",
              silent);
  flag_switch(app, d, id("engagement-replies"), SYSTEM_SUPPRESS_JOIN_REPLIES,
              "Prompt members to reply to welcome messages with a sticker.",
              silent);
  flag_switch(app, d, id("engagement-boosts"), SYSTEM_SUPPRESS_BOOSTS,
              "Send a message when someone boosts this server.", silent);
  flag_switch(app, d, id("engagement-tips"), SYSTEM_SUPPRESS_SETUP_TIPS,
              "Send helpful tips for server setup.", silent);

  div(flex, items_center, gap(4), flex_shrink_0, mt(2)) {
    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      text("System Messages Channel", font_weight(medium),
           text_color(k->strong));
      text("This is the channel we send system event messages to.",
           text_size(px(14)), line_height(px(18)), text_color(k->subtle));
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      channel_select(app, ui, id("engagement-system"), g, false,
                     "No System Messages", &d->system_channel_id, !editable);
    }
  }

  form_divider(k);

  form_legend(k, "Default Notification Settings",
              "This will determine whether members who have not explicitly "
              "set their notification settings receive a notification for "
              "every message sent in this server or not.");

  if (g->community) {
    form_note(k, "We highly recommend setting this to only @mentions for a "
                 "Community Server.");
  }

  div(flex, flex_col, gap(4), flex_shrink_0, mt(4)) {
    if (form_radio(app, id("engagement-all"), nullptr, "All Messages",
                   g->member_count >= 2500
                     ? "Due to this server's size, members will not get "
                       "mobile push notifications for non-@mention "
                       "messages."
                     : nullptr,
                   d->notifications == NOTIFY_ALL_MESSAGES, !editable)) {
      d->notifications = NOTIFY_ALL_MESSAGES;
    }

    if (form_radio(app, id("engagement-mentions"), nullptr, "Only @mentions",
                   nullptr, d->notifications == NOTIFY_ONLY_MENTIONS,
                   !editable)) {
      d->notifications = NOTIFY_ONLY_MENTIONS;
    }
  }

  form_divider(k);

  div(flex, gap(4), flex_shrink_0, mb(2)) {
    div(flex, flex_col, flex_1, min_w(0)) {
      form_label(k, "Inactive Channel");
      channel_select(app, ui, id("engagement-afk"), g, true,
                     "No Inactive Channel", &d->afk_channel_id, !editable);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      int current = -1;

      for (size_t i = 0; i < 5; i++) {
        current = AFK_TIMEOUTS[i] == d->afk_timeout ? (int)i : current;
      }

      form_label(k, "Inactive Timeout");

      int chosen = form_select(app, &ui->dropdown, id("engagement-timeout"),
                               AFK_NAMES, 5, current, "5 minutes",
                               !editable || d->afk_channel_id == 0);

      if (chosen >= 0) {
        d->afk_timeout = AFK_TIMEOUTS[chosen];
      }
    }
  }

  text("Automatically move members to this channel and mute them when they "
       "have been idle for longer than the inactive timeout. This does not "
       "affect browsers.",
       flex_shrink_0, text_size(px(14)), line_height(px(18)),
       font_weight(medium), text_color(k->subtle));
}

/* ========================================================================== */
/* boost perks.                                                               */
/* ========================================================================== */

// a perk's image beside what it is: the box shows it, a gradient while
// there is none.
static void perk_section(App *app, civue_id id, const char *title,
                         const char *line1, const char *line2, bool unlocked,
                         uint8_t tier, const char *url, char **path,
                         bool *removed, bool editable) {
  const Look *k = &app->look;
  char local[CDN_URL_MAX], locked[64];
  const char *shown = shown_image(url, *path, *removed, local);
  bool pick = false, take_off = false;

  snprintf(locked, sizeof locked, "Unlocks at Boost Level %u.", tier);

  div(flex, gap(8), flex_shrink_0) {
    div(flex, flex_col, flex_1, min_w(0)) {
      text(title, mb(2), font_weight(medium), text_color(k->strong));
      text(line1, mb(2), text_size(px(14)), line_height(px(18)),
           font_weight(medium), text_color(k->subtle));
      text(line2, text_size(px(14)), line_height(px(18)), font_weight(medium),
           text_color(k->subtle));

      div(flex, mt(4)) {
        pick = form_button_small(app, id_in(id, "upload"), "Upload Background",
                                 BUTTON_PRIMARY, !unlocked || !editable);
      }

      if (!unlocked) {
        form_note(k, locked);
      }
    }

    div(flex, flex_col, items_center, flex_1, min_w(0)) {
      div(key(id), relative, flex, items_center, justify_center, w(full),
          h(px(180)), rounded(lg), overflow_hidden,
          bg(unlocked ? k->primary : k->rule),
          style(.cursor = unlocked && editable ? CIVUE_CURSOR_POINTER
                                               : CIVUE_CURSOR_DEFAULT,
                .has_cursor = true)) {
        civue_button_state state = button();

        pick |= state.activated && unlocked && editable;

        if (shown != nullptr) {
          image_cover(app, shown, 8);
        }

        if (state.response.hovered && unlocked && editable) {
          div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
              justify_center, bg(rgba(0x000000c2)), text_color(CIVUE_WHITE)) {
            text("Change Background", text_size(px(12)), font_weight(semibold));
          }
        }
      }

      if (shown != nullptr && editable) {
        div(flex, mt(px(10))) {
          take_off = form_button_small(app, id_in(id, "remove"), "Remove",
                                       BUTTON_LINK, false);
        }
      }
    }
  }

  if (pick) {
    pick_image(app, path, removed);
  }

  if (take_off) {
    free(*path);
    *path = nullptr;
    *removed = url != nullptr && *url;
  }
}

static void boost_page(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  GuildDraft *d = &ui->draft;
  bool editable = manages_guild(app, g);
  char banner[CDN_URL_MAX] = "", splash[CDN_URL_MAX] = "";

  cdn_banner(banner, g->id, g->banner, 1024);
  cdn_guild_splash(splash, g->id, g->splash, 1024);

  form_heading(k, "Boost Perks", nullptr);
  form_divider(k);

  if (form_switch_row(app, id("boost-progress"), "Show Boost progress bar",
                      "The Boost progress bar shows members how many Boosts "
                      "your server needs to unlock new perks.",
                      d->progress_bar, !editable)) {
    d->progress_bar = !d->progress_bar;
  }

  form_divider(k);

  perk_section(app, id("boost-banner"), "Server Banner Background",
               "This image will display at the top of your channels list.",
               "The recommended minimum size is 960x540 and recommended "
               "aspect ratio is 16:9.",
               store_guild_has(g, "BANNER"), 2, banner, &d->banner_path,
               &d->banner_removed, editable);

  form_divider(k);

  perk_section(app, id("boost-splash"), "Server Invite Background",
               "This image will display when your server invite is viewed in "
               "a browser, as well as in invite confirmation screens and "
               "Server Onboarding.",
               "The recommended minimum size is 1920x1080 and recommended "
               "aspect ratio is 16:9.",
               store_guild_has(g, "INVITE_SPLASH"), 1, splash, &d->splash_path,
               &d->splash_removed, editable);
}

/* ========================================================================== */
/* safety setup, saved as soon as it is picked.                               */
/* ========================================================================== */

static const char *const VERIFICATION_NAMES[] = {
  "None", "Low", "Medium", "High", "Highest"};
static const char *const VERIFICATION_ABOUT[] = {
  "Unrestricted",
  "Must have a verified email on their Discord account.",
  "Must also be registered on Discord for longer than 5 minutes.",
  "Must also be a member of this server for longer than 10 minutes.",
  "Must have a verified phone on their Discord account.",
};
static const char *const FILTER_NAMES[] = {
  "Do not filter",
  "Filter messages from server members without roles",
  "Filter messages from all members",
};
static const char *const FILTER_ABOUT[] = {
  "Media from anyone is left as it is.",
  "Media sent by members without a role is scanned.",
  "Recommended. Media sent by every member is scanned.",
};

static void safety_page(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  Admin *a = admin_of(app->session, g->id);
  bool busy = a->save.status == ADMIN_BUSY;
  GuildEdit edit = {};

  form_heading(k, "Safety Setup",
               "Keep your server safe from spam and unwanted content.");
  form_divider(k);

  form_legend(k, "Verification Level",
              "Members of the server must meet the following criteria before "
              "they can send messages in text channels or initiate a direct "
              "message conversation. If a member has an assigned role this "
              "does not apply.");

  div(flex, flex_col, gap(4), flex_shrink_0, mt(4)) {
    for (int i = 0; i <= VERIFICATION_VERY_HIGH; i++) {
      // a community needs at least an email.
      bool locked = busy || (g->community && i == VERIFICATION_NONE);

      if (form_radio(app, id_at(id("safety-verify"), i), nullptr,
                     VERIFICATION_NAMES[i], VERIFICATION_ABOUT[i],
                     g->verification_level == i, locked)) {
        edit.fields = GUILD_EDIT_VERIFICATION;
        edit.verification = (VerificationLevel)i;
      }
    }
  }

  form_divider(k);

  form_legend(k, "Explicit Media Content Filter",
              "Automatically detect and block media that contain explicit "
              "content.");

  div(flex, flex_col, gap(4), flex_shrink_0, mt(4)) {
    for (int i = 0; i <= CONTENT_FILTER_ALL_MEMBERS; i++) {
      // a community scans everyone's.
      bool locked = busy || (g->community && i != CONTENT_FILTER_ALL_MEMBERS);

      if (form_radio(app, id_at(id("safety-filter"), i), nullptr,
                     FILTER_NAMES[i], FILTER_ABOUT[i],
                     g->content_filter == i, locked)) {
        edit.fields = GUILD_EDIT_CONTENT_FILTER;
        edit.content_filter = (ContentFilter)i;
      }
    }
  }

  form_divider(k);

  form_legend(k, "2FA Requirement for Moderation",
              g->mfa_required
                ? "Moderators need two-factor authentication to take "
                  "moderation actions."
                : "Moderators may take moderation actions without "
                  "two-factor authentication.");

  if (a->save.status == ADMIN_FAILED && a->save.error != nullptr) {
    form_note(k, a->save.error);
  }

  if (edit.fields != 0) {
    admin_edit_guild(app->session, g->id, &edit);
  }

  (void)ui;
}

/* ========================================================================== */
/* community.                                                                 */
/* ========================================================================== */

static const char *const LOCALES[] = {
  "en-US", "zh-CN", "zh-TW", "cs", "da",    "nl",     "fr",    "de",
  "el",    "hu",    "it",    "ja", "ko",    "no",     "hr",    "pl",
  "pt-BR", "ru",    "es-ES", "es-419", "sv-SE", "tr", "bg",    "uk",
  "fi",    "ro",    "lt",    "th", "vi",    "hi",
};
static const char *const LOCALE_NAMES[] = {
  "English", "\xe4\xb8\xad\xe6\x96\x87", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad"
  "\xe6\x96\x87",
  "\xc4\x8c" "e\xc5\xa1tina", "Dansk", "Nederlands", "Fran\xc3\xa7" "ais",
  "Deutsch", "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac",
  "Magyar", "Italiano", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
  "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", "Norsk", "Hrvatski", "Polski",
  "Portugu\xc3\xaas do Brasil",
  "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
  "Espa\xc3\xb1ol", "Espa\xc3\xb1ol, LATAM", "Svenska", "T\xc3\xbcrk\xc3\xa7" "e",
  "\xd0\xb1\xd1\x8a\xd0\xbb\xd0\xb3\xd0\xb0\xd1\x80\xd1\x81\xd0\xba\xd0\xb8",
  "\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd1\x81\xd1\x8c\xd0\xba\xd0"
  "\xb0",
  "Suomi", "Rom\xc3\xa2n\xc4\x83", "Lietuvi\xc5\xa1kai",
  "\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2", "Ti\xe1\xba\xbfng Vi\xe1\xbb\x87t",
  "\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\x82\xe0\xa4\xa6\xe0\xa5\x80",
};
constexpr size_t NLOCALES = sizeof LOCALES / sizeof *LOCALES;

static_assert(sizeof LOCALE_NAMES / sizeof *LOCALE_NAMES == NLOCALES);

// what discord takes from @everyone as a guild becomes a community.
constexpr Permissions RISKY =
  PERM_ADMINISTRATOR | PERM_KICK_MEMBERS | PERM_BAN_MEMBERS |
  PERM_MANAGE_GUILD | PERM_MANAGE_CHANNELS | PERM_MANAGE_ROLES |
  PERM_MANAGE_MESSAGES | PERM_MANAGE_EXPRESSIONS | PERM_CREATE_EXPRESSIONS |
  PERM_MANAGE_EVENTS | PERM_CREATE_EVENTS | PERM_MODERATE_MEMBERS |
  PERM_MENTION_EVERYONE | (1ull << 53);

static Permissions everyone_permissions(const Guild *g) {
  const Role *everyone = store_role(g, g->id);

  return everyone != nullptr ? everyone->permissions : 0;
}

static bool needs_safety(const Guild *g) {
  return g->verification_level == VERIFICATION_NONE ||
         g->content_filter != CONTENT_FILTER_ALL_MEMBERS;
}

static bool needs_finish(const Guild *g) {
  return g->default_notifications != NOTIFY_ONLY_MENTIONS ||
         (everyone_permissions(g) & RISKY) != 0;
}

static bool step_shown(const Guild *g, CommunityStep step) {
  switch (step) {
    case COMMUNITY_SAFETY:
      return needs_safety(g);
    case COMMUNITY_FINISH:
      return needs_finish(g);
    default:
      return step < COMMUNITY_STEPS;
  }
}

static CommunityStep step_after(const Guild *g, CommunityStep step, int way) {
  int at = (int)step + way;

  while (at > COMMUNITY_INTRO && at < COMMUNITY_STEPS &&
         !step_shown(g, (CommunityStep)at)) {
    at += way;
  }

  return at < 0 ? COMMUNITY_INTRO : (CommunityStep)at;
}

static void community_landing(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  bool admin = store_guild_permissions(app->session->store, g) &
               PERM_ADMINISTRATOR;
  static const char *const CARDS[][2] = {
    {"Grow your community",
     "Apply to be in Server Discovery so more people can find your server "
     "directly on Discord."},
    {"Keep members engaged",
     "Access tools like Server Insights that can better help you moderate and "
     "keep your server engaged."},
    {"Stay informed",
     "Get direct updates about new features built for communities from "
     "Discord."},
  };
  civue_color tints[] = {k->online, k->primary, k->dnd};

  div(flex, flex_col, items_center, flex_shrink_0, text_center) {
    div(flex, items_center, justify_center, w(full), h(px(204)), mb(5)) {
      div(flex, items_center, justify_center, size(px(120)), rounded(full),
          bg(k->chat), text_color(k->primary)) {
        icon(mingcute, home_4_fill, size(px(64)));
      }
    }

    text("Are you building a Community?", text_size(px(24)),
         line_height(px(32)), font_weight(semibold), text_color(k->strong),
         text_center);
    text("Convert to a Community Server to access additional administrative "
         "tools that help you moderate and grow your server.",
         mt(2), text_center, text_color(k->text));

    div(key(id("community-start-wrap")), flex, mt(4)) {
      civue_button_state state = {};

      if (form_button(app, id("community-start"), "Enable Community",
                      BUTTON_PRIMARY, !admin)) {
        ui->community_open = true;
        ui->community_step = COMMUNITY_INTRO;
        ui->community_rules = 1;
        ui->community_updates = g->public_updates_channel_id;
        ui->community_agreed = false;
      }

      (void)state;
    }

    if (!admin) {
      form_note(k, "Only server administrators are allowed to do this");
    }

    div(w(full), h(px(1)), my(8), bg(civue_alpha(k->strong, .06f))) {}

    text("Community Servers are larger spaces where people with shared "
         "interests can come together.",
         text_size(px(14)), line_height(px(18)), text_color(k->text),
         text_center);
    text("Enabling Community does not make your server visible on Server "
         "Discovery.",
         mt(2), text_size(px(14)), line_height(px(18)), text_color(k->text),
         text_center);

    div(grid, grid_cols(3), gap(4), w(full), mt(8)) {
      for (size_t i = 0; i < 3; i++) {
        div(flex, flex_col, p(4), rounded(lg), bg(k->chat), text_left) {
          div(flex, items_center, justify_center, size(10), rounded(full),
              bg(k->frame), text_color(tints[i])) {
            if (i == 2) {
              icon(mingcute, information_fill, size(6));
            }
            else {
              icon(mingcute, chart_bar_fill, size(6));
            }
          }

          text(CARDS[i][0], mt(4), mb(2), font_weight(semibold),
               text_color(k->strong));
          text(CARDS[i][1], text_size(px(14)), line_height(px(18)),
               text_color(k->text));
        }
      }
    }
  }
}

static void community_overview(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  GuildDraft *d = &ui->draft;
  Permissions p = store_guild_permissions(app->session->store, g);
  bool admin = p & PERM_ADMINISTRATOR;
  int locale = -1;

  form_heading(k, "Community Overview",
               "Community Servers are larger spaces where people with shared "
               "interests can come together.");
  form_divider(k);

  if (d->community_off) {
    form_legend(k, "Community will be disabled",
                "Save your changes to turn this server back into a regular "
                "one, or reset them to keep it a Community.");
    return;
  }

  form_legend(k, "Rules or guidelines channel",
              "Members of your server and users in Server Discovery will see "
              "this channel, which moderators may also point members to.");
  div(flex_shrink_0, mt(4)) {
    channel_select(app, ui, id("community-rules"), g, false, "None",
                   &d->rules_channel_id, false);
  }

  form_divider(k);

  form_legend(k, "Community Updates Channel",
              "This is the channel where Discord will send relevant updates "
              "for Community admins and moderators. As some info might be "
              "sensitive, we recommend selecting a role-restricted channel.");
  div(flex_shrink_0, mt(4)) {
    channel_select(app, ui, id("community-updates"), g, false, "None",
                   &d->updates_channel_id, !admin);
  }

  form_divider(k);

  form_legend(k, "Safety Notifications Channel",
              "This is the channel where Discord will send safety "
              "notifications, like raid alerts.");
  div(flex_shrink_0, mt(4)) {
    channel_select(app, ui, id("community-safety"), g, false, nullptr,
                   &d->safety_channel_id, false);
  }

  form_divider(k);

  for (size_t i = 0; i < NLOCALES; i++) {
    locale = same_text(d->locale, LOCALES[i]) ? (int)i : locale;
  }

  form_legend(k, "Server Primary Language",
              "Server Discovery and Discord's moderation rely on it.");
  div(flex_shrink_0, mt(4)) {
    int chosen = form_select(app, &ui->dropdown, id("community-locale"),
                             LOCALE_NAMES, NLOCALES, locale, "English", false);

    if (chosen >= 0) {
      str_set(&d->locale, LOCALES[chosen]);
    }
  }

  form_divider(k);

  form_legend(k, "Server Description",
              "How did your server get started? Why should people join?");
  div(flex_shrink_0, mt(4)) {
    form_area(app, id("community-description"), &d->description, "", 4, 300);
  }

  form_divider(k);

  div(flex, items_center, gap(4), flex_shrink_0) {
    div(flex, flex_col, flex_1, min_w(0)) {
      form_legend(k, "Disable Community",
                  "This will remove Server Discovery, Server Insights and "
                  "the other features of a Community Server.");
    }

    if (form_button_small(app, id("community-disable"), "Disable",
                          BUTTON_DANGER, !admin)) {
      ui->confirm = CONFIRM_DISABLE_COMMUNITY;
    }
  }

  if (!admin) {
    form_note(k, "Only server administrators are allowed to do this");
  }
}

static void community_page(App *app, ServerUi *ui, Guild *g) {
  if (g->community) {
    community_overview(app, ui, g);
  }
  else {
    community_landing(app, ui, g);
  }
}

// discord's client: @everyone loses what is risky first, then the guild
// becomes a community, meeting what it does not meet already.
static void community_enable(App *app, ServerUi *ui, Guild *g) {
  const char *features[64];
  Permissions everyone = everyone_permissions(g);
  GuildEdit edit = {
    .fields = GUILD_EDIT_FEATURES | GUILD_EDIT_RULES_CHANNEL |
              GUILD_EDIT_UPDATES_CHANNEL,
    .features = features,
    .nfeatures = features_for(g, true, features, 64),
    .rules_channel_id = ui->community_rules,
    .updates_channel_id = ui->community_updates,
    .verification = VERIFICATION_LOW,
    .content_filter = CONTENT_FILTER_ALL_MEMBERS,
    .notifications = NOTIFY_ONLY_MENTIONS,
  };

  if (g->verification_level == VERIFICATION_NONE) {
    edit.fields |= GUILD_EDIT_VERIFICATION;
  }

  if (g->content_filter != CONTENT_FILTER_ALL_MEMBERS) {
    edit.fields |= GUILD_EDIT_CONTENT_FILTER;
  }

  if (g->default_notifications != NOTIFY_ONLY_MENTIONS) {
    edit.fields |= GUILD_EDIT_NOTIFICATIONS;
  }

  if (everyone & RISKY) {
    RoleEdit role = {
      .fields = ROLE_EDIT_PERMISSIONS,
      .permissions = everyone & ~RISKY,
    };

    admin_edit_role(app->session, g->id, g->id, &role);
  }

  admin_edit_guild(app->session, g->id, &edit);
}

// a line of a checklist, with discord's green check before it.
static void check_item(const Look *k, const char *title, const char *about) {
  div(flex, items_start, gap(2), flex_shrink_0) {
    icon(mingcute, check_circle_fill, size(4), flex_shrink_0, mt(px(2)),
         text_color(k->online));

    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      text(title, font_weight(medium), text_color(k->strong));
      text(about, text_color(k->subtle));
    }
  }
}

static void risky_table(const Look *k, Permissions everyone) {
  static const struct {
    Permissions bit;
    const char *name;
  } NAMES[] = {
    {PERM_ADMINISTRATOR, "Administrator"},
    {PERM_MANAGE_GUILD, "Manage Server"},
    {PERM_MANAGE_CHANNELS, "Manage Channels"},
    {PERM_MANAGE_ROLES, "Manage Roles"},
    {PERM_KICK_MEMBERS, "Kick Members"},
    {PERM_BAN_MEMBERS, "Ban Members"},
    {PERM_MANAGE_MESSAGES, "Manage Messages"},
    {PERM_MENTION_EVERYONE, "Mention @everyone"},
    {PERM_CREATE_EXPRESSIONS, "Create Emoji"},
    {PERM_MANAGE_EXPRESSIONS, "Manage Emoji"},
    {PERM_CREATE_EVENTS, "Create Events"},
    {PERM_MANAGE_EVENTS, "Manage Events"},
    {PERM_MODERATE_MEMBERS, "Timeout Members"},
  };
  constexpr size_t COUNT = sizeof NAMES / sizeof *NAMES;

  for (int pass = 0; pass < 2; pass++) {
    bool enabled = pass == 0;
    size_t shown = 0;

    for (size_t i = 0; i < COUNT; i++) {
      shown += ((everyone & NAMES[i].bit) != 0) == enabled;
    }

    if (shown == 0) {
      continue;
    }

    div(flex, flex_col, flex_shrink_0, mt(3), px(5), py(4), rounded(md),
        bg(k->chat)) {
      text(enabled ? "Disabling risky permissions..." : "Currently Disabled",
           text_size(px(12)), line_height(px(16)), font_weight(semibold),
           text_color(k->muted));

      div(grid, grid_cols(2), gap_x(8), gap_y(2), mt(2)) {
        for (size_t i = 0; i < COUNT; i++) {
          if (((everyone & NAMES[i].bit) != 0) != enabled) {
            continue;
          }

          div(flex, items_center, gap(2)) {
            div(size(px(6)), rounded(full), flex_shrink_0,
                bg(enabled ? k->idle : k->hover)) {}
            text(NAMES[i].name, text_size(px(14)), line_height(px(18)),
                 text_color(k->text));
          }
        }
      }
    }
  }
}

// a select of "None", "Create one for me" and the text channels.
static void community_channel(App *app, ServerUi *ui, civue_id id,
                              const Guild *g, Snowflake *picked) {
  Snowflake ids[256];
  const char *names[256];
  const char *extra[] = {"None", "Create one for me"};
  Snowflake extra_ids[] = {0, 1};
  size_t n = channel_choices(app, g, false, extra, extra_ids, 2, ids, names,
                             256);
  int current = -1;

  for (size_t i = 0; i < n; i++) {
    current = ids[i] == *picked ? (int)i : current;
  }

  int chosen =
    form_select(app, &ui->dropdown, id, names, n, current, "None", false);

  if (chosen >= 0) {
    *picked = ids[chosen];
  }
}

static void community_dialog(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Admin *a = admin_of(app->session, g->id);
  CommunityStep step = ui->community_step;
  bool busy = a->save.status == ADMIN_BUSY;
  bool close = false, back = false, next = false;
  int total = 0, number = 0;
  char progress[32];

  if (!ui->community_open) {
    return;
  }

  for (int s = COMMUNITY_SAFETY; s < COMMUNITY_STEPS; s++) {
    if (step_shown(g, (CommunityStep)s)) {
      total++;
      number += s <= (int)step;
    }
  }

  snprintf(progress, sizeof progress, "Step %d of %d", number, total);

  const char *primary = step == COMMUNITY_INTRO   ? "Get Started"
                        : step == COMMUNITY_RULES ? "Finish Setup"
                        : step == COMMUNITY_BASICS ? "Next"
                                                   : "Enable";

  overlay() {
    div(key(id("community-shade")), left(0), top(0), w(px(cui->size.x)),
        h(px(cui->size.y)), flex, items_center, justify_center, bg(k->scrim)) {
      close = button().activated && !busy;

      // a banner or the progress at its top, then what the step asks and
      // its foot, 24 from the edges and 16 apart.
      div(key(id("community-dialog")), flex, flex_col, gap(4),
          w(px(dialog_width())), pb(6), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_color(k->text), text_size(px(14)), line_height(px(20)),
          overflow_hidden) {
        button();

        if (step == COMMUNITY_INTRO) {
          div(flex, items_center, justify_center, h(px(140)), mb(2),
              rounded_t(px(DIALOG_RADIUS)), bg(k->positive),
              text_color(k->on_positive)) {
            icon(mingcute, home_4_fill, size(px(64)));
          }
        }
        else {
          div(flex, flex_col, gap(2), px(6), pt(6)) {
            text(progress, text_size(px(12)), line_height(px(16)),
                 font_weight(semibold), text_color(k->subtle));

            div(h(px(4)), rounded(full), bg(k->hover)) {
              civue_rect bar = here();
              float done = total ? (float)number / (float)total : 0;

              fill(civue_rect_from(bar.x0, bar.y0,
                                   bar.x0 + (bar.x1 - bar.x0) * done, bar.y1),
                   k->primary, 2);
            }
          }
        }

        div(flex, flex_col, gap(4), px(6)) {
          switch (step) {
            case COMMUNITY_INTRO:
              dialog_header(k, "Enable Community",
                            "Community Servers are larger spaces built around "
                            "shared interests. Enabling Community requires "
                            "setting up safety settings, an Updates channel, "
                            "and moderation settings.");
              break;
            case COMMUNITY_SAFETY:
              dialog_header(k, "Keep your community safe",
                            "To ensure user safety, Community Servers must "
                            "have the following moderation settings enabled.");

              div(flex, flex_col, gap(3)) {
                if (g->verification_level == VERIFICATION_NONE) {
                  check_item(k, "Verified email required",
                             "To minimize spam, members of the server must "
                             "have a verified email before they can send "
                             "messages. This does not apply to members with "
                             "roles.");
                }

                if (g->content_filter != CONTENT_FILTER_ALL_MEMBERS) {
                  check_item(k, "Explicit Media Content Filter",
                             "Discord will automatically scan and delete "
                             "media sent in this server that contain "
                             "explicit content (excludes age-restricted "
                             "channels).");
                }
              }
              break;
            case COMMUNITY_BASICS:
              dialog_header(k, "Set up the basics",
                            "Tell us which channel has your server rules and "
                            "which channel we should send announcements to!");

              div(flex, flex_col, gap(4)) {
                div(flex, flex_col) {
                  form_label(k, "Rules or Guidelines Channel");
                  text("Optionally select a channel for your server rules or "
                       "guidelines, or choose None.",
                       mb(2), text_color(k->subtle));
                  community_channel(app, ui, id("community-dialog-rules"), g,
                                    &ui->community_rules);
                }

                div(flex, flex_col) {
                  form_label(k, "Community Updates Channel");
                  text("This is the channel where Discord will send relevant "
                       "updates for Community admins and moderators. As some "
                       "info might be sensitive, we recommend selecting a "
                       "role-restricted channel.",
                       mb(2), text_color(k->subtle));
                  community_channel(app, ui, id("community-dialog-updates"), g,
                                    &ui->community_updates);

                  if (ui->community_updates == 0) {
                    form_note(k, "Optional. Without a moderator channel, raid "
                                 "alerts and lockdown notices won't be "
                                 "delivered.");
                  }
                }
              }
              break;
            case COMMUNITY_FINISH:
              dialog_header(k, "Update default settings", nullptr);

              div(flex, flex_col, gap(3)) {
                if (g->default_notifications != NOTIFY_ONLY_MENTIONS) {
                  check_item(k, "Set default settings to Mentions Only",
                             "Because Community Servers tend to have a lot of "
                             "people talking at once, defaulting Server "
                             "Notifications for members to Mentions Only can "
                             "help reduce members leaving. Members can "
                             "always override this option.");
                }

                check_item(k, "Disable risky permissions for @everyone",
                           "This will remove the following permissions from "
                           "the @everyone role: Administrator, Manage "
                           "Server, Manage Channels, Manage Roles, Kick "
                           "Members, Ban Members, Manage Messages, and "
                           "Mention @everyone, @here, and All Roles.");
              }

              risky_table(k, everyone_permissions(g));
              break;
            case COMMUNITY_RULES:
            case COMMUNITY_STEPS:
              dialog_header(k, "One last step", nullptr);

              div(flex, flex_col) {
                form_label(k, "Follow the rules");
                text("By setting your server as a Community Server, you agree "
                     "that your server complies with our Community Server "
                     "Guidelines.",
                     mb(4), text_color(k->subtle));

                div(key(id("community-agree-row")), flex, items_center, gap(2),
                    cursor_pointer) {
                  bool flip = button().activated;

                  flip |= form_checkbox(app, id("community-agree"),
                                        ui->community_agreed);

                  if (flip) {
                    ui->community_agreed = !ui->community_agreed;
                  }

                  text("I agree and understand", text_color(k->strong));
                }
              }
              break;
          }

          if (a->save.status == ADMIN_FAILED && a->save.error != nullptr) {
            text(a->save.error, text_color(k->critical_text));
          }
        }

        div(flex, items_center, justify_end, gap(2), flex_shrink_0, px(6)) {
          if (step == COMMUNITY_INTRO) {
            close |= form_button(app, id("community-cancel"), "Cancel",
                                 BUTTON_OUTLINE, false);
            next = form_button(app, id("community-next"), primary,
                               BUTTON_PRIMARY, false);
          }
          else {
            back = form_button(app, id("community-back"), "Back",
                               BUTTON_OUTLINE, busy);
            div(flex_1) {}
            next = form_button(
              app, id("community-next"), busy ? "Enabling..." : primary,
              BUTTON_PRIMARY,
              busy || (step == COMMUNITY_RULES && !ui->community_agreed));
          }
        }
      }
    }
  }

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE) && !busy &&
      ui->dropdown.open == 0) {
    close = true;
  }

  if (back) {
    ui->community_step = step_after(g, step, -1);
  }
  else if (next && step != COMMUNITY_RULES) {
    ui->community_step = step_after(g, step, 1);
    admin_clear(a, &a->save);
  }
  else if (next && ui->community_agreed && !busy) {
    community_enable(app, ui, g);
  }

  // discord made it a community: the dialog has done its job.
  if (g->community && a->save.status == ADMIN_DONE) {
    close = true;
    admin_clear(a, &a->save);
    draft_clear(&ui->draft);
  }

  if (close) {
    ui->community_open = false;
    form_dropdown_close(&ui->dropdown);
  }
}

/* ========================================================================== */
/* emoji.                                                                     */
/* ========================================================================== */

// how many emojis of each kind a boost level allows.
static size_t emoji_slots(const Guild *g) {
  static const size_t SLOTS[] = {50, 100, 150, 250};

  return SLOTS[g->premium_tier < 4 ? g->premium_tier : 3];
}

static void emojis_picked(App *app, civue_strs files, uint64_t guild_id) {
  ServerUi *ui = server_ui(app);

  if (!files.len) {
    return;
  }

  free(ui->error);
  ui->error = nullptr;

  for (uint32_t i = 0; i < files.len; i++) {
    char *uri = admin_image_uri(files.v[i], EMOJI_MOST, &ui->error);
    // the name is the file's, without its extension, as discord's does.
    const char *base = strrchr(files.v[i], '/');
    char name[33];
    size_t n = 0;

    base = base != nullptr ? base + 1 : files.v[i];

    for (const char *c = base; *c && *c != '.' && n < 32; c++) {
      bool word = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_';
      name[n++] = word ? *c : '_';
    }

    while (n < 2) {
      name[n++] = '_';
    }

    name[n] = 0;

    if (uri != nullptr) {
      admin_create_emoji(app->session, guild_id, name, uri);
    }

    free(uri);
  }
}

static void emoji_row(App *app, ServerUi *ui, Guild *g, const Emoji *e) {
  const Look *k = &app->look;
  bool editing = ui->emoji_id == e->id;
  bool remove = false, rename = false;

  div(key(id_at(id("emoji-row"), e->id)), flex, items_center, gap(4),
      flex_shrink_0, h(px(56)), px(2), border_b(1), border_color(k->border),
      hover(bg(k->message_hover))) {
    div(flex, items_center, justify_center, w(px(48)), flex_shrink_0) {
      emoji_node(app, e->id, e->name, 32);
    }

    if (editing) {
      div(flex_1, min_w(0)) {
        if (form_input(app, id("emoji-name"), &ui->emoji_name, "", ":", 32)
              .submitted) {
          rename = true;
        }
      }
    }
    else {
      div(key(id_at(id("emoji-label"), e->id)), flex, items_center, flex_1,
          min_w(0), h(full), cursor_text) {
        if (button().activated) {
          ui->emoji_id = e->id;
          str_set(&ui->emoji_name, e->name);
        }

        text(e->name ? e->name : "", truncate, text_color(k->strong));
      }
    }

    if (icon_button(app, id_at(id("emoji-delete"), e->id),
                    CIVUE_ICON(mingcute, close_fill), 32, 18, "Delete")) {
      remove = true;
    }
  }

  if (rename && ui->emoji_name != nullptr && strlen(ui->emoji_name) >= 2) {
    admin_rename_emoji(app->session, g->id, e->id, ui->emoji_name);
    ui->emoji_id = 0;
  }

  if (remove) {
    ui->confirm = CONFIRM_DELETE_EMOJI;
    ui->confirm_id = e->id;
  }
}

static void emoji_page(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  Admin *a = admin_of(app->session, g->id);
  size_t slots = emoji_slots(g);
  size_t animated = 0;
  char line[96];
  const char *error = ui->error ? ui->error
                      : a->action.status == ADMIN_FAILED ? a->action.error
                                                         : nullptr;

  for (size_t i = 0; i < g->nemojis; i++) {
    animated += g->emojis[i].animated;
  }

  form_heading(k, "Emoji",
               "Add up to 50 custom emoji that anyone can use in this server. "
               "Animated GIF emoji may be used by members with Discord Nitro. "
               "Emoji names must be at least 2 characters long and can only "
               "contain alphanumeric characters and underscores. Emoji must "
               "be under 256 KB in size.");

  div(flex, items_center, gap(4), flex_shrink_0, mt(6), mb(8)) {
    if (form_button_small(app, id("emoji-upload"), "Upload Emoji",
                          BUTTON_PRIMARY, a->action.status == ADMIN_BUSY)) {
      pick_files_later(app, IMAGE_TYPES, countof(IMAGE_TYPES), true,
                       emojis_picked, g->id);
    }

    if (error != nullptr) {
      text(error, flex_1, min_w(0), text_size(px(14)), line_height(px(18)),
           text_color(k->critical_text));
    }
  }

  for (int pass = 0; pass < 2; pass++) {
    bool want = pass == 1;
    size_t used = want ? animated : g->nemojis - animated;

    snprintf(line, sizeof line, "%s \xc2\xb7 %zu slots available",
             want ? "Animated Emoji" : "Emoji",
             slots > used ? slots - used : 0);
    text(line, flex_shrink_0, mb(3), font_weight(semibold),
         text_color(k->strong));

    div(flex, items_center, gap(4), flex_shrink_0, h(8), px(2), border_b(1),
        border_color(k->border), text_size(px(14)), line_height(px(18)),
        font_weight(semibold), text_color(k->text)) {
      text("Image", w(px(48)), flex_shrink_0, text_center);
      text("Name", flex_1);
    }

    div(flex, flex_col, flex_shrink_0, mb(10)) {
      for (size_t i = 0; i < g->nemojis; i++) {
        if (g->emojis[i].animated == want) {
          emoji_row(app, ui, g, &g->emojis[i]);
        }
      }

      if (used == 0) {
        text("No emoji yet.", my(4), text_size(px(14)), text_color(k->faint));
      }
    }
  }
}

/* ========================================================================== */
/* bans and invites.                                                          */
/* ========================================================================== */

static void bans_page(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Admin *a = admin_of(app->session, g->id);
  bool may = store_guild_permissions(store, g) & PERM_BAN_MEMBERS;
  size_t shown = 0;

  if (!a->bans_loaded && !a->bans_loading) {
    admin_fetch_bans(app->session, g->id);
  }

  form_heading(k, "Bans",
               "Bans by default are by account and IP. A user can circumvent "
               "an IP ban by using a proxy.");

  div(flex_shrink_0, mt(6), mb(4)) {
    form_input(app, id("bans-search"), &ui->member_search, "Search Bans",
               nullptr, 0);
  }

  if (a->bans_loading) {
    text("Loading bans...", text_color(k->subtle));
    return;
  }

  for (size_t i = 0; i < a->nbans; i++) {
    const AdminBan *ban = &a->bans[i];
    const User *u = store_user(store, ban->user_id);
    const char *name = store_user_name(u);
    bool lift = false;

    if (ui->member_search && *ui->member_search &&
        !strcasestr(name, ui->member_search) &&
        !(u && u->username && strcasestr(u->username, ui->member_search))) {
      continue;
    }

    shown++;

    div(key(id_at(id("ban-row"), ban->user_id)), flex, items_center, gap(3),
        flex_shrink_0, h(px(56)), px(2), rounded(md), hover(bg(k->hover))) {
      avatar(app, u, 0, 32, false, k->panel);

      div(flex, flex_col, flex_1, min_w(0)) {
        text(name, truncate, font_weight(medium), text_color(k->strong));
        text(ban->reason ? ban->reason : "No reason given.", truncate,
             text_size(px(14)), line_height(px(18)), text_color(k->subtle));
      }

      if (may) {
        lift = form_button_small(app, id_at(id("ban-revoke"), ban->user_id),
                                 "Revoke Ban", BUTTON_DANGER,
                                 a->action.status == ADMIN_BUSY);
      }
    }

    if (lift) {
      admin_unban(app->session, g->id, ban->user_id);
      break;
    }
  }

  if (shown == 0) {
    text(a->nbans ? "No bans match." : "No bans. Nice.", mt(6), text_center,
         text_color(k->faint));
  }
}

static void format_expiry(int64_t at, char *out, size_t cap) {
  if (at == 0) {
    snprintf(out, cap, "Never");
    return;
  }

  int64_t left = at / 1000 - (int64_t)time(nullptr);

  if (left <= 0) {
    snprintf(out, cap, "Expired");
  }
  else if (left < 3600) {
    snprintf(out, cap, "%lldm", (long long)(left / 60));
  }
  else if (left < 86400) {
    snprintf(out, cap, "%lldh %lldm", (long long)(left / 3600),
             (long long)(left % 3600 / 60));
  }
  else {
    snprintf(out, cap, "%lldd %lldh", (long long)(left / 86400),
             (long long)(left % 86400 / 3600));
  }
}

static void invites_page(App *app, Guild *g) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Admin *a = admin_of(app->session, g->id);

  if (!a->invites_loaded && !a->invites_loading) {
    admin_fetch_invites(app->session, g->id);
  }

  form_heading(k, "Invites",
               "Here's a list of all active invite links. You can revoke any "
               "one.");

  div(flex, items_center, gap(4), flex_shrink_0, h(8), px(2), mt(6),
      border_b(1), border_color(k->border), text_size(px(14)),
      line_height(px(18)), font_weight(semibold), text_color(k->text)) {
    text("Inviter", flex_1);
    text("Invite Code", w(px(120)), flex_shrink_0);
    text("Uses", w(px(56)), flex_shrink_0);
    text("Expires", w(px(80)), flex_shrink_0);
    div(w(px(32)), flex_shrink_0) {}
  }

  if (a->invites_loading) {
    text("Loading invites...", mt(4), text_color(k->subtle));
    return;
  }

  for (size_t i = 0; i < a->ninvites; i++) {
    const AdminInvite *inv = &a->invites[i];
    const User *u = store_user(store, inv->inviter_id);
    const Channel *c = store_channel(store, inv->channel_id);
    char uses[24], expires[32], where[128];
    bool revoke = false;

    snprintf(uses, sizeof uses, "%u", inv->uses);
    snprintf(where, sizeof where, "#%s", c && c->name ? c->name : "unknown");
    format_expiry(inv->expires_at, expires, sizeof expires);

    div(key(id_at(id("invite-row"), i)), flex, items_center, gap(4),
        flex_shrink_0, h(px(56)), px(2), border_b(1), border_color(k->border),
        hover(bg(k->message_hover)), text_size(px(14)), line_height(px(18))) {
      div(flex, items_center, gap(3), flex_1, min_w(0)) {
        avatar(app, u, 0, 32, false, k->panel);

        div(flex, flex_col, flex_1, min_w(0)) {
          text(store_user_name(u), truncate, font_weight(medium),
               text_color(k->strong));
          text(where, truncate, text_size(px(12)), line_height(px(16)),
               text_color(k->subtle));
        }
      }

      text(inv->code, w(px(120)), flex_shrink_0, truncate, font_mono,
           text_color(k->text));
      text(uses, w(px(56)), flex_shrink_0, text_color(k->text));
      text(expires, w(px(80)), flex_shrink_0, tabular_nums,
           text_color(k->text));
      revoke = icon_button(app, id_at(id("invite-revoke"), i),
                           CIVUE_ICON(mingcute, close_fill), 32, 18, "Revoke invite");
    }

    if (revoke) {
      admin_revoke_invite(app->session, g->id, inv->code);
      break;
    }
  }

  if (a->ninvites == 0) {
    text("No invites yet.", mt(6), text_center, text_color(k->faint));
  }
}

/* ========================================================================== */
/* deleting the guild.                                                        */
/* ========================================================================== */

// discord's confirmation: the name typed, unless the guild asks moderators
// for two factors.
static void delete_dialog(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Admin *a = admin_of(app->session, g->id);
  bool busy = a->leave.status == ADMIN_BUSY;
  bool typed = g->mfa_required ||
               same_text(ui->confirm_text, g->name ? g->name : "");
  bool close = false, yes = false;
  char title[160], body[256];

  snprintf(title, sizeof title, "Delete '%s'", g->name ? g->name : "");
  snprintf(body, sizeof body,
           "Are you sure you want to delete %s? This action cannot be "
           "undone.",
           g->name ? g->name : "this server");

  overlay() {
    div(key(id("delete-guild-shade")), left(0), top(0), w(px(cui->size.x)),
        h(px(cui->size.y)), flex, items_center, justify_center, bg(k->scrim)) {
      close = button().activated && !busy;

      div(key(id("delete-guild")), flex, flex_col, gap(4),
          w(px(dialog_width())), p(6), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_color(k->text), text_size(px(14)), line_height(px(20))) {
        button();

        dialog_header(k, title, nullptr);

        div(flex, flex_col, p(3), rounded(lg),
            bg(civue_alpha(k->danger, .16f))) {
          text(body, text_color(k->strong));
        }

        if (!g->mfa_required) {
          div(flex, flex_col) {
            form_label(k, "Enter server name");
            form_input(app, id("delete-guild-name"), &ui->confirm_text, "",
                       nullptr, 100);
          }
        }

        if (a->leave.error != nullptr) {
          text(a->leave.error, text_color(k->critical_text));
        }

        div(flex, justify_end, gap(2), flex_shrink_0) {
          close |= form_button(app, id("delete-guild-no"), "Cancel",
                               BUTTON_OUTLINE, busy);
          yes = form_button(app, id("delete-guild-yes"),
                            busy ? "Deleting..." : "Delete Server",
                            BUTTON_DANGER, busy || !typed);
        }
      }
    }
  }

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE) && !busy) {
    close = true;
  }

  if (yes && typed) {
    admin_delete_guild(app->session, g->id);
  }

  if (close) {
    ui->confirm = CONFIRM_NONE;
    free(ui->confirm_text);
    ui->confirm_text = nullptr;
    admin_clear(a, &a->leave);
  }
}

static void confirmations(App *app, ServerUi *ui, Guild *g) {
  Session *s = app->session;
  int answer = 0;
  char body[512];

  switch (ui->confirm) {
    case CONFIRM_DELETE_GUILD:
      delete_dialog(app, ui, g);
      return;
    case CONFIRM_DISABLE_COMMUNITY:
      answer = form_confirm(
        app, id("confirm-community"), "Disable Community",
        "Your server will lose Server Discovery, Server Insights, "
        "Announcement channels and the other features of a Community. Save "
        "your changes to confirm.",
        "Disable Community", true, false, nullptr);

      if (answer > 0) {
        ui->draft.community_off = true;
      }

      break;
    case CONFIRM_DELETE_EMOJI: {
      const Emoji *e = store_emoji(g, ui->confirm_id);

      snprintf(body, sizeof body, "Are you sure you want to delete :%s:?",
               e && e->name ? e->name : "emoji");
      answer = form_confirm(app, id("confirm-emoji"), "Delete Emoji", body,
                            "Delete", true, false, nullptr);

      if (answer > 0) {
        admin_delete_emoji(s, g->id, ui->confirm_id);
      }

      break;
    }
    default:
      return;
  }

  if (answer != 0) {
    ui->confirm = CONFIRM_NONE;
  }
}

/* ========================================================================== */
/* the layer.                                                                 */
/* ========================================================================== */

static bool page_has_draft(ServerPage page) {
  return page_fields(page) != 0;
}

static void page(App *app, ServerUi *ui, Guild *g, float wide) {
  switch (ui->page) {
    case SERVER_PROFILE:
      profile_page(app, ui, g, wide);
      break;
    case SERVER_ENGAGEMENT:
      engagement_page(app, ui, g);
      break;
    case SERVER_BOOST:
      boost_page(app, ui, g);
      break;
    case SERVER_EMOJI:
      emoji_page(app, ui, g);
      break;
    case SERVER_ROLES:
      roles_page(app, ui, g);
      break;
    case SERVER_INVITES:
      invites_page(app, g);
      break;
    case SERVER_SAFETY:
      safety_page(app, ui, g);
      break;
    case SERVER_BANS:
      bans_page(app, ui, g);
      break;
    case SERVER_COMMUNITY:
      community_page(app, ui, g);
      break;
    case SERVER_PAGES:
      break;
  }
}

// the round close button with "ESC" under it.
static bool close_button(App *app) {
  const Look *k = &app->look;
  bool pressed = false;

  div(flex, flex_col, items_center, gap(px(6))) {
    div(key(id("server-close")), flex, items_center, justify_center,
        size(px(36)), flex_shrink_0, rounded(full), border(2),
        border_color(k->subtle), text_color(k->subtle),
        hover(bg(k->hover), text_color(k->strong),
              border_color(k->strong)),
        active(bg(k->pressed)), cursor_pointer) {
      pressed = button().activated;
      icon(mingcute, close_fill, size(5));
    }

    text("ESC", text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(k->subtle));
  }

  return pressed;
}

void server_settings(App *app) {
  ServerUi *ui = app->server;
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();

  if (ui == nullptr || !ui->open) {
    return;
  }

  Session *s = app->session;
  Guild *g = store_guild(s->store, ui->guild_id);
  Admin *a = admin_of(s, ui->guild_id);

  // the guild went, was deleted or left.
  if (g == nullptr || a->leave.status == ADMIN_DONE) {
    if (app->guild_id == ui->guild_id) {
      app_select_guild(app, 0);
    }

    admin_clear(a, &a->leave);
    close_settings(ui);
    return;
  }

  if (!page_allowed(app, g, ui->page)) {
    ui->page = SERVER_PROFILE;
  }

  // the draft follows the guild until the user changes something.
  if (page_has_draft(ui->page) && !page_dirty(ui, g) &&
      a->save.status != ADMIN_BUSY && !ui->community_open &&
      (!ui->draft.taken || ui->draft.stamp != g->version)) {
    draft_take(&ui->draft, g);
  }

  if (a->save.status == ADMIN_DONE && !ui->community_open) {
    admin_clear(a, &a->save);
    form_nag_reset(&ui->nag);

    if (page_has_draft(ui->page)) {
      draft_take(&ui->draft, g);
    }
    else if (ui->page == SERVER_ROLES) {
      roles_forget(ui);
    }
  }

  float width = cui->size.x, height = cui->size.y;
  bool dirty = page_dirty(ui, g);
  bool busy = a->save.status == ADMIN_BUSY;
  bool modal = ui->community_open || ui->picker_open ||
               ui->confirm != CONFIRM_NONE || ui->dropdown.open != 0 ||
               ui->role_menu != 0;
  bool close = false;
  NavChoice nav = {.page = -1};
  civue_rect region = {};
  civue_vec2 shake = form_shake(&ui->nag);

  app->covers_buttons = true;

  overlay() {
    div(key(id("server-settings")), flex, left(px(shake.x)), top(px(shake.y)),
        w(px(width)), h(px(height)), bg(k->panel), text_size(px(16)),
        line_height(px(20)), text_color(k->text)) {
      button();

      nav = sidebar(app, ui, g);

      div(relative, flex, items_start, h(full), bg(k->panel),
          style(.grow = 1, .shrink = 1, .has_shrink = true,
                .basis = civue_px(800))) {
        region = here();

        div(key(id_at(id("server-body"), ui->page)), flex, flex_1, min_w(0),
            h(full), overflow_y_scroll, overflow_x_hidden,
            scrollbars(&app->bars)) {
          div(flex, items_start, flex_shrink_0, w(full)) {
            div(flex, flex_col, min_w(0), max_w(px(COLUMN)), pt(px(60)),
                px(10), pb(px(80)), style(.grow = 1, .shrink = 1,
                                          .has_shrink = true,
                                          .min_w = civue_px(460))) {
              page(app, ui, g, width);
            }

            // the close button stays put while the page scrolls, here
            // only its room.
            div(flex_shrink_0, w(px(TOOLS)), mr(px(21))) {}
          }
        }

        div(absolute, top(px(60)),
            style(.inset[1] = civue_px(fmaxf(
                    (float)(region.x1 - region.x0) - COLUMN - TOOLS - 21,
                    0) + 21))) {
          close = close_button(app);
        }
      }
    }
  }

  // the window's own strip, which the settings cover, still moves it.
  overlay() {
    div(left(0), top(0), w(px(width)), h(px(TOP_BAR_HEIGHT))) {
      window_handle(id("server-title-bar"));
    }
  }

  civue_rect notice_room = civue_rect_from(
    region.x0, region.y0,
    fminf((float)region.x0 + COLUMN, (float)region.x1 - 71), region.y1);
  const char *error = a->save.status == ADMIN_FAILED ? a->save.error
                      : ui->page != SERVER_EMOJI      ? ui->error
                                                      : nullptr;
  NoticeAction bar = form_notice(
    app, notice_room,
    dirty || (a->save.status == ADMIN_FAILED && ui->page != SERVER_SAFETY &&
              !ui->community_open),
    busy, error, &ui->nag);

  if (bar == NOTICE_RESET) {
    admin_clear(a, &a->save);
    free(ui->error);
    ui->error = nullptr;
    form_nag_reset(&ui->nag);

    if (ui->page == SERVER_ROLES) {
      roles_forget(ui);
    }
    else {
      draft_take(&ui->draft, g);
    }
  }
  else if (bar == NOTICE_SAVE && dirty) {
    if (ui->page == SERVER_ROLES) {
      roles_save(app, ui, g);
    }
    else {
      draft_save(app, ui, g);
    }
  }

  if (ui->page == SERVER_ROLES) {
    roles_dialogs(app, ui, g);
  }

  community_dialog(app, ui, g);
  confirmations(app, ui, g);
  form_dropdowns(app, &ui->dropdown);

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE) && !modal) {
    close = true;
  }

  bool leaving = close || (nav.page >= 0 && nav.page != ui->page) ||
                 nav.delete_guild;

  // unsaved changes hold the page: the app shakes instead.
  if (leaving && dirty) {
    form_nag(&ui->nag);
    return;
  }

  if (close) {
    close_settings(ui);
  }
  else if (nav.page >= 0 && nav.page != ui->page) {
    ServerPage to = (ServerPage)nav.page;

    forget_page(ui);
    form_nag_reset(&ui->nag);
    admin_clear(a, &a->save);
    ui->page = to;
  }
  else if (nav.delete_guild) {
    admin_clear(a, &a->leave);
    ui->confirm = CONFIRM_DELETE_GUILD;
    free(ui->confirm_text);
    ui->confirm_text = nullptr;
  }
}
