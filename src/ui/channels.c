#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float DRAG_SLOP = 4;
constexpr size_t MOST_ROWS = 1024;

/* ========================================================================== */
/* who may do what.                                                           */
/* ========================================================================== */

static bool manages_channels(App *app, const Guild *g) {
  return (store_guild_permissions(app->session->store, g) &
          PERM_MANAGE_CHANNELS) != 0;
}

// MANAGE_CHANNELS where a channel sits: in its category, or in the guild.
static bool manages_in(App *app, const Guild *g, Snowflake parent_id) {
  Store *store = app->session->store;
  const Channel *parent = store_channel(store, parent_id);

  if (parent == nullptr) {
    return manages_channels(app, g);
  }

  return store_can(store, parent, PERM_MANAGE_CHANNELS);
}

typedef enum : uint8_t {
  BUCKET_TEXT,
  BUCKET_VOICE,
  BUCKET_CATEGORY,
} Bucket;

// the three lists discord numbers its channels in.
static Bucket bucket_of(const Channel *c) {
  if (c->type == CHANNEL_GUILD_CATEGORY) {
    return BUCKET_CATEGORY;
  }

  return channel_is_voice(c) ? BUCKET_VOICE : BUCKET_TEXT;
}

/* ========================================================================== */
/* making a channel.                                                          */
/* ========================================================================== */

typedef struct {
  ChannelType type;
  const char *name, *about;
} ChannelKind;

static const ChannelKind KINDS[] = {
  {CHANNEL_GUILD_TEXT, "Text",
   "Send messages, images, GIFs, emoji, opinions, and puns"},
  {CHANNEL_GUILD_VOICE, "Voice",
   "Hang out together with voice, video, and screen share"},
  {CHANNEL_GUILD_FORUM, "Forum", "Create a space for organized discussions"},
  {CHANNEL_GUILD_ANNOUNCEMENT, "Announcement",
   "Important updates for people in and out of the server"},
  {CHANNEL_GUILD_STAGE, "Stage",
   "Host events, panels, and Q&As for an audience"},
};

static civue_icon kind_icon(ChannelType type, bool private) {
  switch (type) {
    case CHANNEL_GUILD_VOICE:
      return private ? CIVUE_ICON(mingcute, volume_off_fill)
                     : CIVUE_ICON(mingcute, volume_fill);
    case CHANNEL_GUILD_FORUM:
      return CIVUE_ICON(mingcute, messages_square_fill);
    case CHANNEL_GUILD_ANNOUNCEMENT:
      return CIVUE_ICON(mingcute, announcement_fill);
    case CHANNEL_GUILD_STAGE:
      return CIVUE_ICON(mingcute, base_station_fill);
    default:
      return private ? CIVUE_ICON(mingcute, lock_fill) : CIVUE_ICON(mingcute, hashtag_fill);
  }
}

// the kinds the guild offers, as discord's dialog lists them.
static bool kind_offered(App *app, const Guild *g, ChannelType type) {
  Permissions p = store_guild_permissions(app->session->store, g);

  switch (type) {
    case CHANNEL_GUILD_ANNOUNCEMENT:
      return store_guild_has(g, "NEWS");
    case CHANNEL_GUILD_STAGE:
      return g->community && (p & PERM_MANAGE_ROLES) &&
             (p & PERM_MANAGE_CHANNELS);
    default:
      return true;
  }
}

static bool limited_name(ChannelType type) {
  return type == CHANNEL_GUILD_TEXT || type == CHANNEL_GUILD_ANNOUNCEMENT ||
         type == CHANNEL_GUILD_FORUM || type == CHANNEL_GUILD_MEDIA;
}

void channel_create_open(App *app, Snowflake guild_id, Snowflake parent_id,
                         bool category) {
  ServerUi *ui = server_ui(app);
  CreateChannel *c = &ui->create;
  Admin *a = admin_of(app->session, guild_id);

  free(c->name);
  free(c->search);
  c->name = c->search = nullptr;
  c->open = true;
  c->guild_id = guild_id;
  c->parent_id = category ? 0 : parent_id;
  c->category = category;
  c->type = category ? CHANNEL_GUILD_CATEGORY : CHANNEL_GUILD_TEXT;
  c->private = false;
  c->picking = false;
  c->roles.len = c->users.len = 0;
  admin_clear(a, &a->channel);
  app->popover = POPOVER_NONE;

  // discord's dialog types into its name at once.
  if (civue_ui_current() != nullptr) {
    civue_ui_current()->focus = id("create-name");
  }
}

// the roles and the members that may see a private channel.
static void access_step(App *app, CreateChannel *c, Guild *g) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const char *q = c->search;
  bool members_only = q && q[0] == '@';
  const char *needle = members_only ? q + 1 : q;
  size_t shown = 0;

  form_input(app, id("create-search"), &c->search,
             "e.g. Moderators, @wumpus", nullptr, 0);
  text("Add individual members by starting with @, or type a role name",
       flex_shrink_0, my(1), text_size(px(12)), line_height(px(16)),
       text_color(k->subtle));

  div(key(id("create-access")), flex, flex_col, h(px(280)), mt(2),
      overflow_y_scroll, scrollbars(&app->thin_bars)) {
    if (!members_only) {
      text("Roles", flex_shrink_0, h(8), pt(2), text_size(px(14)),
           font_weight(semibold), text_color(k->text));

      for (size_t i = 0; i < g->nroles; i++) {
        const Role *r = g->roles[i];
        bool on = ids_has(&c->roles, r->id);

        if (r->id == g->id ||
            (needle && *needle && !(r->name && strcasestr(r->name, needle)))) {
          continue;
        }

        shown++;

        div(key(id_at(id("create-role"), r->id)), flex, items_center, gap(2),
            flex_shrink_0, h(px(40)), px(px(6)), mr(1), rounded(md),
            style(.bg = on ? k->selected : CIVUE_CLEAR),
            hover(bg(k->selected)), cursor_pointer) {
          bool flip = button().activated;

          div(size(3), rounded(full), flex_shrink_0, mx(1),
              bg(rgb(r->color ? r->color : 0x99aab5))) {}
          text(r->name ? r->name : "", flex_1, min_w(0), truncate,
               text_size(px(14)), text_color(k->strong));
          text("Role", text_size(px(12)), text_color(k->muted));
          flip |= form_checkbox(app, id_at(id("create-role-box"), r->id), on);

          if (flip) {
            ids_toggle(&c->roles, r->id);
          }
        }
      }
    }

    text("Members", flex_shrink_0, h(8), pt(2), text_size(px(14)),
         font_weight(semibold), text_color(k->text));

    CILO_MAP_EACH(entry, &g->members) {
      const Member *m = entry->val;
      const User *u = store_user(store, m->user_id);
      const char *name = store_display_name(store, g->id, m->user_id);
      bool on = ids_has(&c->users, m->user_id);

      if (needle && *needle && !strcasestr(name, needle) &&
          !(u && u->username && strcasestr(u->username, needle))) {
        continue;
      }

      if (++shown > 150) {
        break;
      }

      div(key(id_at(id("create-user"), m->user_id)), flex, items_center,
          gap(2), flex_shrink_0, h(px(40)), px(px(6)), mr(1), rounded(md),
          style(.bg = on ? k->selected : CIVUE_CLEAR), hover(bg(k->selected)),
          cursor_pointer) {
        bool flip = button().activated;

        avatar(app, u, g->id, 24, false, k->dialog);
        text(name, min_w(0), truncate, text_size(px(14)),
             text_color(k->strong));

        if (u != nullptr && u->username != nullptr) {
          text(u->username, flex_1, min_w(0), truncate, text_size(px(14)),
               text_color(k->muted));
        }
        else {
          div(flex_1) {}
        }

        text("Member", text_size(px(12)), text_color(k->muted));
        flip |=
          form_checkbox(app, id_at(id("create-user-box"), m->user_id), on);

        if (flip) {
          ids_toggle(&c->users, m->user_id);
        }
      }
    }
  }
}

static void create_dialog(App *app, ServerUi *ui) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  CreateChannel *c = &ui->create;
  Guild *g = store_guild(app->session->store, c->guild_id);
  bool close = false, back = false, create = false, next = false;
  char subtitle[160], slug[128];

  if (!c->open) {
    return;
  }

  if (g == nullptr) {
    c->open = false;
    return;
  }

  Admin *a = admin_of(app->session, g->id);
  const Channel *parent = store_channel(app->session->store, c->parent_id);
  bool busy = a->channel.status == ADMIN_BUSY;

  // discord turns the private toggle off where it is not offered.
  if (c->type == CHANNEL_GUILD_STAGE) {
    c->private = false;
  }

  // a text channel's name is written as discord keeps it, on every key.
  if (limited_name(c->type) && c->name != nullptr) {
    admin_channel_slug(c->name, slug, sizeof slug);

    if (strcmp(slug, c->name) != 0) {
      form_input_set(id("create-name"), &c->name, slug);
    }
  }

  bool named = false;

  for (const char *p = c->name; p != nullptr && *p; p++) {
    named = named || *p != ' ';
  }

  bool private_offered = c->type != CHANNEL_GUILD_ANNOUNCEMENT &&
                         c->type != CHANNEL_GUILD_STAGE;
  const char *create_label = c->category ? "Create Category" : "Create Channel";
  civue_icon name_icon = kind_icon(c->type, c->private);

  snprintf(subtitle, sizeof subtitle, "in %s",
           parent && parent->name ? parent->name : "");

  overlay() {
    div(key(id("create-shade")), left(0), top(0), w(px(cui->size.x)),
        h(px(cui->size.y)), flex, items_center, justify_center, bg(k->scrim)) {
      close = button().activated && !busy;

      div(key(id("create-dialog")), flex, flex_col, gap(4),
          w(px(dialog_width())), p(6), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_color(k->text), text_size(px(14)), line_height(px(20))) {
        button();

        div(flex, items_start, gap(2), flex_shrink_0) {
          div(flex, flex_col, flex_1, min_w(0)) {
            if (c->picking) {
              dialog_header(k, "Add members or roles", nullptr);

              div(flex, items_center, gap(1), mt(2), text_color(k->subtle)) {
                glyph(name_icon, 16, k->subtle);
                text(c->name ? c->name : "", truncate);
              }
            }
            else {
              dialog_header(k, create_label,
                            parent != nullptr && !c->category ? subtitle
                                                              : nullptr);
            }
          }

          if (icon_button(app, id("create-close"), CIVUE_ICON(mingcute, close_fill), 24,
                          16, nullptr) &&
              !busy) {
            close = true;
          }
        }

        div(flex, flex_col, gap(4)) {
          if (c->picking) {
            access_step(app, c, g);
          }
          else {
            if (!c->category) {
              div(flex, flex_col) {
                form_label(k, "Channel Type");

                div(flex, flex_col, gap(3)) {
                  for (size_t i = 0; i < sizeof KINDS / sizeof *KINDS; i++) {
                    civue_icon glyph_icon = kind_icon(KINDS[i].type, false);

                    if (!kind_offered(app, g, KINDS[i].type)) {
                      continue;
                    }

                    if (form_radio(app, id_at(id("create-kind"), i),
                                   &glyph_icon, KINDS[i].name, KINDS[i].about,
                                   c->type == KINDS[i].type, false)) {
                      c->type = KINDS[i].type;
                    }
                  }
                }
              }
            }

            div(flex, flex_col) {
              form_label(k, c->category ? "Category Name" : "Channel Name");

              div(relative, flex, flex_col) {
                civue_field_response field = form_input(
                  app, id("create-name"), &c->name,
                  c->category                        ? "New Category"
                  : c->type == CHANNEL_GUILD_FORUM   ? "forum-discussion"
                                                     : "new-channel",
                  c->category ? nullptr : " ", 100);

                if (!c->category) {
                  div(absolute, left(px(12)), top(px(10))) {
                    glyph(name_icon, 16, k->subtle);
                  }
                }

                if (field.submitted && named) {
                  next = c->private;
                  create = !c->private;
                }
              }
            }

            if (private_offered) {
              div(flex, items_start, gap(2)) {
                icon(mingcute, lock_fill, size(4), flex_shrink_0, mt(px(2)),
                     text_color(k->strong));

                div(flex, flex_col, flex_1, min_w(0)) {
                  if (form_switch_row(
                        app, id("create-private"),
                        c->category ? "Private Category" : "Private Channel",
                        c->category
                          ? "By making a category private, only select "
                            "members and roles will be able to view this "
                            "category. Linked channels in this category will "
                            "automatically match to this setting."
                          : "Only selected members and roles will be able to "
                            "view this channel.",
                        c->private, false)) {
                    c->private = !c->private;
                  }
                }
              }
            }
          }

          if (a->channel.status == ADMIN_FAILED) {
            div(flex, items_center, gap(2), p(3), rounded(lg),
                bg(civue_alpha(k->danger, .16f)),
                text_color(k->critical_text)) {
              icon(mingcute, warning_fill, size(4), flex_shrink_0);
              text(a->channel.error ? a->channel.error
                                    : "An error occurred, please try again.");
            }
          }
        }

        div(flex, flex_wrap, items_center, justify_end, gap(2),
            flex_shrink_0) {
          if (c->picking) {
            back = form_button(app, id("create-back"), "Back", BUTTON_OUTLINE,
                               busy);
            div(flex_1) {}
            create = form_button(app, id("create-go"),
                                 busy                           ? "Creating..."
                                 : c->roles.len || c->users.len ? create_label
                                                                : "Skip",
                                 BUTTON_PRIMARY, busy);
          }
          else {
            close |= form_button(app, id("create-cancel"), "Cancel",
                                 BUTTON_OUTLINE, busy);

            if (c->private) {
              next |= form_button(app, id("create-next"), "Next",
                                  BUTTON_PRIMARY, !named || busy);
            }
            else {
              create |= form_button(app, id("create-go"),
                                    busy ? "Creating..." : create_label,
                                    BUTTON_PRIMARY, !named || busy);
            }
          }
        }
      }
    }
  }

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE) && !busy) {
    close = true;
  }

  if (back) {
    c->picking = false;
  }
  else if (next && named) {
    c->picking = true;
  }
  else if (create && named && !busy) {
    ChannelCreate made = {
      .name = c->name,
      .type = c->type,
      .parent_id = c->category ? 0 : c->parent_id,
      .private = c->private,
      .roles = c->roles.v,
      .nroles = c->roles.len,
      .users = c->users.v,
      .nusers = c->users.len,
    };

    admin_create_channel(app->session, g->id, &made);
  }

  // the channel exists: the dialog closes on it.
  if (a->channel.status == ADMIN_DONE && a->channel.made != 0) {
    Snowflake made = a->channel.made;
    Channel *channel = store_channel(app->session->store, made);

    admin_clear(a, &a->channel);
    close = true;

    if (channel != nullptr && channel_has_text(channel) &&
        !channel_is_voice(channel)) {
      app_select_channel(app, made);
    }
  }

  if (close) {
    c->open = false;
    c->picking = false;
  }
}

/* ========================================================================== */
/* the menus of channels, editing and deleting them.                          */
/* ========================================================================== */

void channel_menu_open(App *app, Snowflake channel_id, civue_vec2 at) {
  ServerUi *ui = server_ui(app);

  ui->menu_open = true;
  ui->menu_channel = channel_id;
  ui->menu_at = at;
  app->popover = POPOVER_NONE;
}

// a row of a context menu, as discord draws them: 32 tall, no icon.
static bool menu_item(App *app, civue_id id, const char *label, bool danger) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, flex_shrink_0, h(8), px(2), rounded(md),
      text_size(px(14)), line_height(px(18)), font_weight(medium),
      text_color(danger ? k->critical_text : k->subtle),
      hover(bg(danger ? civue_alpha(k->danger, .08f) : k->hover),
            text_color(danger ? k->critical_text : k->strong)),
      cursor_pointer) {
    pressed = button().activated;
    text(label, flex_1, min_w(0), truncate);
  }

  return pressed;
}

static void menu_separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), mx(px(4)), my(px(4)), bg(k->border)) {}
}

static void channel_menu(App *app, ServerUi *ui) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Store *store = app->session->store;
  Channel *c = store_channel(store, ui->menu_channel);
  Guild *g = c != nullptr ? store_guild(store, c->guild_id)
                          : store_guild(store, app->guild_id);
  bool edit = false, remove = false, copy = false, text_channel = false,
       voice_channel = false, category = false;
  civue_rect card = {};

  if (!ui->menu_open) {
    return;
  }

  if (g == nullptr) {
    ui->menu_open = false;
    return;
  }

  bool is_category = c != nullptr && c->type == CHANNEL_GUILD_CATEGORY;
  bool manages = c != nullptr ? manages_in(app, g, is_category ? 0 : c->parent_id) ||
                                  store_can(store, c, PERM_MANAGE_CHANNELS)
                              : manages_channels(app, g);
  civue_rect room = overlay_room();
  float left = fminf(ui->menu_at.x, (float)room.x1 - 220);
  float top = fminf(ui->menu_at.y, (float)room.y1 - 260);

  overlay() {
    div(key(id("channel-menu")), flex, flex_col, w(px(220)), left(px(left)),
        top(px(top)), px(2), py(2), rounded(lg), bg(k->popover), border(1),
        border_color(k->border), shadow(lg)) {
      card = here();

      if (c == nullptr) {
        // the list's own room: what makes channels.
        if (manages) {
          text_channel = menu_item(app, id("menu-create-channel"),
                                   "Create Channel", false);
          category = menu_item(app, id("menu-create-category"),
                               "Create Category", false);
        }
      }
      else {
        if (manages) {
          edit = menu_item(app, id("menu-edit"),
                           is_category ? "Edit Category" : "Edit Channel",
                           false);

          if (is_category || !channel_is_voice(c)) {
            text_channel = menu_item(app, id("menu-text"),
                                     "Create Text Channel", false);
          }

          if (is_category || channel_is_voice(c)) {
            voice_channel = menu_item(app, id("menu-voice"),
                                      "Create Voice Channel", false);
          }

          remove = menu_item(app, id("menu-delete"),
                             is_category ? "Delete Category"
                                         : "Delete Channel",
                             true);
          menu_separator(k);
        }

        copy = menu_item(app, id("menu-copy"),
                         is_category ? "Copy Category ID" : "Copy Channel ID",
                         false);
      }
    }
  }

  // the press that opened it is not one outside of it, its card not laid
  // out yet.
  bool outside = (cui->input.pressed[0] || cui->input.pressed[1]) &&
                 cui->input.has_mouse && card.x1 > card.x0 &&
                 !civue_rect_contains(card, cui->input.mouse);
  bool chosen = edit || remove || copy || text_channel || voice_channel ||
                category;

  if (chosen || outside || civue_key_pressed(cui, CIVUE_KEY_ESCAPE)) {
    ui->menu_open = false;
  }

  Snowflake parent = c == nullptr ? 0 : is_category ? c->id : c->parent_id;

  if (text_channel || voice_channel) {
    channel_create_open(app, g->id, parent, false);
    ui->create.type = voice_channel ? CHANNEL_GUILD_VOICE : CHANNEL_GUILD_TEXT;
  }
  else if (category) {
    channel_create_open(app, g->id, 0, true);
  }
  else if (edit) {
    Admin *a = admin_of(app->session, g->id);

    ui->renaming = c->id;
    str_set(&ui->rename, c->name);
    admin_clear(a, &a->channel);
    cui->focus = id("edit-name");
  }
  else if (remove) {
    Admin *a = admin_of(app->session, g->id);

    ui->confirm = CONFIRM_DELETE_CHANNEL;
    ui->confirm_id = c->id;
    admin_clear(a, &a->channel);
  }
  else if (copy) {
    char digits[21];

    snowflake_format(c->id, digits);
    civue_clipboard_set_text(digits, strlen(digits));
  }
}

// a channel's name and nothing else, what Quarrel edits of a channel.
static void edit_dialog(App *app, ServerUi *ui) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Channel *c = store_channel(app->session->store, ui->renaming);
  bool close = false, save = false;
  char slug[128];

  if (ui->renaming == 0) {
    return;
  }

  if (c == nullptr) {
    ui->renaming = 0;
    return;
  }

  Admin *a = admin_of(app->session, c->guild_id);
  bool busy = a->channel.status == ADMIN_BUSY;
  bool category = c->type == CHANNEL_GUILD_CATEGORY;

  if (limited_name((ChannelType)c->type) && ui->rename != nullptr) {
    admin_channel_slug(ui->rename, slug, sizeof slug);

    if (strcmp(slug, ui->rename) != 0) {
      form_input_set(id("edit-name"), &ui->rename, slug);
    }
  }

  bool named = ui->rename != nullptr && *ui->rename &&
               strcmp(ui->rename, c->name ? c->name : "") != 0;

  overlay() {
    div(key(id("edit-shade")), left(0), top(0), w(px(cui->size.x)),
        h(px(cui->size.y)), flex, items_center, justify_center, bg(k->scrim)) {
      close = button().activated && !busy;

      div(key(id("edit-dialog")), flex, flex_col, gap(4),
          w(px(dialog_width())), p(6), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_color(k->text), text_size(px(14)), line_height(px(20))) {
        button();

        dialog_header(k, category ? "Edit Category" : "Edit Channel", nullptr);

        div(flex, flex_col) {
          form_label(k, category ? "Category Name" : "Channel Name");
          save = form_input(app, id("edit-name"), &ui->rename, "", nullptr, 100)
                   .submitted;
        }

        if (a->channel.status == ADMIN_FAILED && a->channel.error) {
          text(a->channel.error, text_color(k->critical_text));
        }

        div(flex, justify_end, gap(2), flex_shrink_0) {
          close |= form_button(app, id("edit-cancel"), "Cancel",
                               BUTTON_OUTLINE, busy);
          save |= form_button(app, id("edit-save"),
                              busy ? "Saving..." : "Save Changes",
                              BUTTON_PRIMARY, !named || busy);
        }
      }
    }
  }

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE) && !busy) {
    close = true;
  }

  if (save && named && !busy) {
    admin_rename_channel(app->session, c->guild_id, c->id, ui->rename);
  }

  if (a->channel.status == ADMIN_DONE) {
    admin_clear(a, &a->channel);
    close = true;
  }

  if (close) {
    ui->renaming = 0;
  }
}

static void delete_dialog(App *app, ServerUi *ui) {
  Channel *c = store_channel(app->session->store, ui->confirm_id);
  char body[256];

  if (c == nullptr) {
    ui->confirm = CONFIRM_NONE;
    return;
  }

  Admin *a = admin_of(app->session, c->guild_id);
  bool category = c->type == CHANNEL_GUILD_CATEGORY;

  snprintf(body, sizeof body,
           "Are you sure you want to delete %s%s? This cannot be undone.",
           category || channel_is_voice(c) ? "" : "#",
           c->name ? c->name : "this channel");

  int answer = form_confirm(app, id("confirm-delete-channel"),
                            category ? "Delete Category" : "Delete Channel",
                            body,
                            category ? "Delete Category" : "Delete Channel",
                            true, a->channel.status == ADMIN_BUSY,
                            a->channel.error);

  if (answer > 0) {
    admin_delete_channel(app->session, c->guild_id, c->id);
  }
  else if (answer < 0) {
    ui->confirm = CONFIRM_NONE;
  }

  if (a->channel.status == ADMIN_DONE) {
    admin_clear(a, &a->channel);
    ui->confirm = CONFIRM_NONE;

    if (app->session->selected_channel_id == ui->confirm_id) {
      app_select_channel(app, 0);
    }
  }
}

static void leave_dialog(App *app, ServerUi *ui) {
  Guild *g = store_guild(app->session->store, ui->confirm_id);
  char title[160], body[320];

  if (g == nullptr) {
    ui->confirm = CONFIRM_NONE;
    return;
  }

  Admin *a = admin_of(app->session, g->id);

  snprintf(title, sizeof title, "Leave '%s'", g->name ? g->name : "");
  snprintf(body, sizeof body,
           "Are you sure you want to leave %s? You won't be able to rejoin "
           "this server unless you are re-invited.",
           g->name ? g->name : "this server");

  int answer = form_confirm(app, id("confirm-leave"), title, body,
                            "Leave Server", true,
                            a->leave.status == ADMIN_BUSY, a->leave.error);

  if (answer > 0) {
    admin_leave_guild(app->session, g->id);
  }
  else if (answer < 0) {
    ui->confirm = CONFIRM_NONE;
  }

  if (a->leave.status == ADMIN_DONE) {
    admin_clear(a, &a->leave);
    ui->confirm = CONFIRM_NONE;

    if (app->guild_id == g->id) {
      app_select_guild(app, 0);
    }
  }
}

// the moved channel's permissions may become its new category's.
static void sync_dialog(App *app, ServerUi *ui) {
  Store *store = app->session->store;
  char body[320];

  if (ui->npending == 0) {
    ui->confirm = CONFIRM_NONE;
    return;
  }

  ChannelPlace *moved = &ui->pending[ui->pending_moved];
  const Channel *c = store_channel(store, moved->id);
  const Channel *category = store_channel(store, moved->parent_id);

  snprintf(body, sizeof body,
           "Would you like to sync #%s permissions with %s?",
           c && c->name ? c->name : "channel",
           category && category->name ? category->name : "the category");

  int answer = form_confirm(app, id("confirm-sync"), "Sync permissions?", body,
                            "Sync Permissions", true, false, nullptr);

  if (answer == 0) {
    return;
  }

  Snowflake guild_id = c != nullptr ? c->guild_id : app->guild_id;

  // keeping them still moves the channel.
  moved->lock_permissions = answer > 0;
  admin_move_channels(app->session, guild_id, ui->pending, ui->npending);
  free(ui->pending);
  ui->pending = nullptr;
  ui->npending = 0;
  ui->confirm = CONFIRM_NONE;
}

void channel_dialogs(App *app) {
  ServerUi *ui = app->server;

  if (ui == nullptr) {
    return;
  }

  channel_menu(app, ui);
  create_dialog(app, ui);
  edit_dialog(app, ui);

  // the settings show their own confirmations over themselves.
  if (ui->open) {
    return;
  }

  switch (ui->confirm) {
    case CONFIRM_DELETE_CHANNEL:
      delete_dialog(app, ui);
      break;
    case CONFIRM_LEAVE_GUILD:
      leave_dialog(app, ui);
      break;
    case CONFIRM_SYNC_PERMISSIONS:
      sync_dialog(app, ui);
      break;
    default:
      break;
  }
}

/* ========================================================================== */
/* dragging channels and categories, after the rules of discord's client.     */
/* ========================================================================== */

typedef struct {
  Channel *c;
  civue_rect r;
} DragRow;

static DragRow drag_rows[MOST_ROWS];
static size_t ndrag_rows;
static uint64_t drag_frame;

bool channel_dragging(App *app) {
  return app->server != nullptr && app->server->channel_drag.id != 0;
}

float channel_drag_alpha(App *app, const Channel *c) {
  if (!channel_dragging(app)) {
    return 1;
  }

  const Channel *d = store_channel(app->session->store,
                                   app->server->channel_drag.id);

  if (d == nullptr || c->type == CHANNEL_GUILD_CATEGORY) {
    return 1;
  }

  // rows that cannot take what is dragged fade, as discord's.
  bool fits = d->type != CHANNEL_GUILD_CATEGORY && bucket_of(c) == bucket_of(d);

  return fits ? 1 : .3f;
}

void channel_drag_row(App *app, Guild *g, Channel *c, civue_rect r,
                      const civue_response *press) {
  civue_ui *cui = civue_ui_current();
  ServerUi *ui = server_ui(app);
  Drag *d = &ui->channel_drag;

  if (drag_frame != cui->frames) {
    drag_frame = cui->frames;
    ndrag_rows = 0;
  }

  if (ndrag_rows < MOST_ROWS) {
    drag_rows[ndrag_rows++] = (DragRow){c, r};
  }

  if (press->right_clicked) {
    channel_menu_open(app, c->id, cui->input.mouse);
  }

  bool movable = c->type == CHANNEL_GUILD_CATEGORY
                   ? manages_channels(app, g)
                   : manages_in(app, g, c->parent_id);

  if (press->pressed && movable) {
    d->pressed = c->id;
    d->from = cui->input.mouse;
  }

  if (d->pressed == c->id && d->id == 0 && cui->input.down[0]) {
    float dx = cui->input.mouse.x - d->from.x;
    float dy = cui->input.mouse.y - d->from.y;

    if (dx * dx + dy * dy > DRAG_SLOP * DRAG_SLOP) {
      d->id = c->id;
    }
  }
}

// the categories in the order they show, and the channels of a bucket in
// the order they show across every category, uncategorized ones first.
typedef struct {
  Channel *items[MOST_ROWS];
  size_t count;
} List;

static int compare_place(const void *a, const void *b) {
  const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;

  if (x->position != y->position) {
    return x->position < y->position ? -1 : 1;
  }

  return x->id < y->id ? -1 : x->id > y->id;
}

static void categories_of(Store *store, Guild *g, List *out) {
  size_t count = 0;
  const ChannelRow *rows = store_guild_channels(store, g, &count);

  out->count = 0;

  for (size_t i = 0; i < count && out->count < MOST_ROWS; i++) {
    if (rows[i].kind == CHANNEL_ROW_CATEGORY) {
      out->items[out->count++] = rows[i].channel;
    }
  }
}

// the parent a channel sits under in the list: its category, or none for
// one whose category is gone.
static Snowflake shown_parent(Store *store, const Channel *c) {
  const Channel *parent = store_channel(store, c->parent_id);

  return parent != nullptr && parent->type == CHANNEL_GUILD_CATEGORY
           ? parent->id
           : 0;
}

static void bucket_list(Store *store, Guild *g, const List *categories,
                        Bucket bucket, List *out) {
  size_t count = 0;
  const ChannelRow *rows = store_guild_channels(store, g, &count);
  Channel *loose[MOST_ROWS];
  size_t nloose = 0;

  out->count = 0;

  // what has no category comes first.
  for (size_t i = 0; i < count && nloose < MOST_ROWS; i++) {
    Channel *c = rows[i].channel;

    if (rows[i].kind == CHANNEL_ROW_CHANNEL && bucket_of(c) == bucket &&
        shown_parent(store, c) == 0) {
      loose[nloose++] = c;
    }
  }

  qsort(loose, nloose, sizeof *loose, compare_place);

  for (size_t i = 0; i < nloose; i++) {
    out->items[out->count++] = loose[i];
  }

  for (size_t j = 0; j < categories->count; j++) {
    Channel *kids[MOST_ROWS];
    size_t nkids = 0;

    for (size_t i = 0; i < count && nkids < MOST_ROWS; i++) {
      Channel *c = rows[i].channel;

      if (rows[i].kind == CHANNEL_ROW_CHANNEL && bucket_of(c) == bucket &&
          shown_parent(store, c) == categories->items[j]->id) {
        kids[nkids++] = c;
      }
    }

    qsort(kids, nkids, sizeof *kids, compare_place);

    for (size_t i = 0; i < nkids && out->count < MOST_ROWS; i++) {
      out->items[out->count++] = kids[i];
    }
  }
}

static size_t index_in(const List *l, const Channel *c) {
  for (size_t i = 0; i < l->count; i++) {
    if (l->items[i] == c) {
      return i;
    }
  }

  return l->count;
}

static void remove_at(List *l, size_t at) {
  memmove(&l->items[at], &l->items[at + 1],
          (l->count - at - 1) * sizeof *l->items);
  l->count--;
}

static void insert_at(List *l, size_t at, Channel *c) {
  if (l->count >= MOST_ROWS) {
    return;
  }

  memmove(&l->items[at + 1], &l->items[at],
          (l->count - at) * sizeof *l->items);
  l->items[at] = c;
  l->count++;
}

// where the channels of `parent` begin in a bucket's list, or end.
static size_t slot_of(Store *store, const List *l, const List *categories,
                      Snowflake parent, bool end) {
  size_t rank = 0;

  for (size_t j = 0; j < categories->count; j++) {
    rank = categories->items[j]->id == parent ? j + 1 : rank;
  }

  size_t at = 0;

  for (size_t i = 0; i < l->count; i++) {
    Snowflake p = shown_parent(store, l->items[i]);
    size_t r = 0;

    for (size_t j = 0; j < categories->count && p != 0; j++) {
      r = categories->items[j]->id == p ? j + 1 : r;
    }

    if (r < rank || (end && r == rank)) {
      at = i + 1;
    }
  }

  return at;
}

// a move's entry for every channel of `l` whose place it changes.
static void numbered(const List *l, Channel *moved, Snowflake parent,
                     ChannelPlace *places, size_t *n, size_t cap) {
  for (size_t i = 0; i < l->count && *n < cap; i++) {
    Channel *c = l->items[i];
    bool mine = c == moved;
    bool reparent = mine && c->parent_id != parent;

    if (c->position == (int32_t)i && !reparent) {
      continue;
    }

    places[(*n)++] = (ChannelPlace){
      .id = c->id,
      .position = (int32_t)i,
      .parent_id = mine ? parent : c->parent_id,
      .moved = reparent,
    };
  }
}

// whether two channels' overwrites are the same, a missing @everyone one
// counting as empty.
static bool same_overwrites(const Channel *a, const Channel *b,
                            Snowflake guild_id) {
  for (int pass = 0; pass < 2; pass++) {
    const Channel *x = pass ? b : a, *y = pass ? a : b;

    for (size_t i = 0; i < x->noverwrites; i++) {
      const Overwrite *o = &x->overwrites[i];
      bool found = false;

      for (size_t j = 0; j < y->noverwrites; j++) {
        const Overwrite *p = &y->overwrites[j];

        if (p->id == o->id && p->type == o->type) {
          found = p->allow == o->allow && p->deny == o->deny;
          goto next;
        }
      }

      found = o->id == guild_id && o->allow == 0 && o->deny == 0;

    next:
      if (!found) {
        return false;
      }
    }
  }

  return true;
}

// sends a move, or asks first whether to sync the permissions of the
// channel moved into another category.
static void send_move(App *app, Guild *g, Channel *dragged, Snowflake parent,
                      ChannelPlace *places, size_t n) {
  Store *store = app->session->store;
  ServerUi *ui = server_ui(app);
  size_t kept = 0, moved = n;

  // discord leaves out what the user may not move.
  for (size_t i = 0; i < n; i++) {
    const Channel *c = store_channel(store, places[i].id);
    bool category = c != nullptr && c->type == CHANNEL_GUILD_CATEGORY;

    if (c != nullptr &&
        (category ? manages_channels(app, g)
                  : manages_in(app, g, c->parent_id))) {
      moved = places[i].id == dragged->id ? kept : moved;
      places[kept++] = places[i];
    }
  }

  if (kept == 0) {
    return;
  }

  const Channel *category = store_channel(store, parent);
  const Channel *old = store_channel(store, dragged->parent_id);
  bool reparent = moved < kept && places[moved].moved && category != nullptr;
  bool may_sync = reparent && store_can(store, dragged, PERM_MANAGE_ROLES) &&
                  store_can(store, category, PERM_MANAGE_ROLES);
  bool was_synced = old == nullptr || same_overwrites(dragged, old, g->id);

  if (may_sync && was_synced &&
      !same_overwrites(dragged, category, g->id)) {
    free(ui->pending);
    ui->pending = calloc(kept, sizeof *ui->pending);
    memcpy(ui->pending, places, kept * sizeof *places);
    ui->npending = kept;
    ui->pending_moved = moved;
    ui->confirm = CONFIRM_SYNC_PERMISSIONS;
    return;
  }

  admin_move_channels(app->session, g->id, places, kept);
}

// a channel taking the place of `reference` in its bucket, under `parent`,
// or at `slot` of the bucket when `reference` is null.
static void move_channel(App *app, Guild *g, Channel *dragged,
                         Channel *reference, Snowflake parent, size_t slot) {
  Store *store = app->session->store;
  List categories, list;
  ChannelPlace places[MOST_ROWS];
  size_t n = 0;

  categories_of(store, g, &categories);
  bucket_list(store, g, &categories, bucket_of(dragged), &list);

  size_t from = index_in(&list, dragged);

  if (from == list.count) {
    return;
  }

  if (reference != nullptr) {
    // discord's moveItemFromTo: out of its place, into the reference's.
    size_t to = index_in(&list, reference);

    remove_at(&list, from);
    insert_at(&list, to, dragged);
  }
  else {
    remove_at(&list, from);
    insert_at(&list, slot, dragged);
  }

  numbered(&list, dragged, parent, places, &n, MOST_ROWS);
  send_move(app, g, dragged, parent, places, n);
}

// a category taking the place of `reference`: the categories numbered
// again, then the channels, which follow their categories.
static void move_category(App *app, Guild *g, Channel *dragged,
                          Channel *reference) {
  Store *store = app->session->store;
  List categories, text, voice;
  ChannelPlace places[MOST_ROWS];
  size_t n = 0;

  categories_of(store, g, &categories);

  size_t from = index_in(&categories, dragged);
  size_t to = index_in(&categories, reference);

  if (from == categories.count || to == categories.count) {
    return;
  }

  remove_at(&categories, from);
  insert_at(&categories, to, dragged);
  numbered(&categories, nullptr, 0, places, &n, MOST_ROWS);

  bucket_list(store, g, &categories, BUCKET_TEXT, &text);
  bucket_list(store, g, &categories, BUCKET_VOICE, &voice);
  numbered(&text, nullptr, 0, places, &n, MOST_ROWS);
  numbered(&voice, nullptr, 0, places, &n, MOST_ROWS);

  // categories have no parent to send.
  for (size_t i = 0; i < n; i++) {
    const Channel *c = store_channel(store, places[i].id);

    places[i].parent_id = c != nullptr ? c->parent_id : 0;
  }

  send_move(app, g, dragged, 0, places, n);
}

typedef struct {
  bool valid;
  // the line under the row rather than over it.
  bool below;
  // what the drop does.
  Channel *reference;
  Snowflake parent;
  // when there is no reference: the parent's slot in the bucket, at its
  // start or at its end.
  bool at_end;
} Landing;

// the row after `at` in the list shown, null at its end.
static const DragRow *row_after(size_t at) {
  return at + 1 < ndrag_rows ? &drag_rows[at + 1] : nullptr;
}

static const DragRow *row_before(size_t at) {
  return at > 0 ? &drag_rows[at - 1] : nullptr;
}

// discord's QO: where a drop of the row `s` on the row `t` lands.
static Landing landing_of(App *app, size_t s, size_t t) {
  Store *store = app->session->store;
  Channel *dragged = drag_rows[s].c, *target = drag_rows[t].c;
  Landing l = {.below = t > s};
  bool down = t > s;

  if (s == t) {
    return l;
  }

  if (dragged->type == CHANNEL_GUILD_CATEGORY) {
    if (!down) {
      // up: only onto a category, which it goes before.
      if (target->type == CHANNEL_GUILD_CATEGORY) {
        l.valid = true;
        l.reference = target;
      }

      return l;
    }

    // down: onto the last row of a category's section, after which a
    // category or the end comes.
    const DragRow *next = row_after(t);
    bool last = next == nullptr || next->c->type == CHANNEL_GUILD_CATEGORY;
    Channel *section = target->type == CHANNEL_GUILD_CATEGORY
                         ? target
                         : store_channel(store, shown_parent(store, target));

    if (last && section != nullptr && section != dragged) {
      l.valid = true;
      l.reference = section;
    }

    return l;
  }

  Bucket mine = bucket_of(dragged);

  if (target->type == CHANNEL_GUILD_CATEGORY) {
    if (down) {
      // into it, first of its channels of the dragged one's bucket.
      l.valid = true;
      l.parent = target->id;
      return l;
    }

    // above it: last of the category before, or of the channels without
    // one, an empty category taking it in.
    const DragRow *before = row_before(t);

    l.valid = true;
    l.at_end = true;
    l.parent = before == nullptr ? 0
               : before->c->type == CHANNEL_GUILD_CATEGORY
                 ? before->c->id
                 : shown_parent(store, before->c);
    return l;
  }

  Snowflake parent = shown_parent(store, target);

  if (bucket_of(target) == mine) {
    l.valid = true;
    l.reference = target;
    l.parent = parent;
    return l;
  }

  // across text and voice, only where the two meet in a parent.
  if (mine == BUCKET_VOICE) {
    const DragRow *next = row_after(t);
    bool boundary = next == nullptr ||
                    next->c->type == CHANNEL_GUILD_CATEGORY ||
                    bucket_of(next->c) == BUCKET_VOICE;

    if (boundary) {
      l.valid = true;
      l.below = true;
      l.parent = parent;
    }
  }
  else {
    const DragRow *before = row_before(t);
    bool boundary = before == nullptr ||
                    before->c->type == CHANNEL_GUILD_CATEGORY ||
                    bucket_of(before->c) == BUCKET_TEXT;

    if (boundary) {
      l.valid = true;
      l.below = false;
      l.at_end = true;
      l.parent = parent;
    }
  }

  return l;
}

void channel_drag_end(App *app, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Store *store = app->session->store;
  ServerUi *ui = app->server;

  if (ui == nullptr) {
    return;
  }

  Drag *d = &ui->channel_drag;

  if (!cui->input.down[0] && d->id == 0) {
    d->pressed = 0;
  }

  if (d->id == 0) {
    return;
  }

  size_t s = ndrag_rows, t = ndrag_rows;
  float y = cui->input.mouse.y;

  for (size_t i = 0; i < ndrag_rows; i++) {
    s = drag_rows[i].c->id == d->id ? i : s;

    if (y >= drag_rows[i].r.y0 && y < drag_rows[i].r.y1) {
      t = i;
    }
  }

  if (s == ndrag_rows) {
    *d = (Drag){};
    return;
  }

  Channel *dragged = drag_rows[s].c;
  Landing l = t < ndrag_rows ? landing_of(app, s, t) : (Landing){};

  // the destination needs the user to see it and manage its channels.
  if (l.valid && dragged->type != CHANNEL_GUILD_CATEGORY &&
      l.parent != shown_parent(store, dragged)) {
    const Channel *to = store_channel(store, l.parent);

    l.valid = to == nullptr ? manages_channels(app, g)
                            : store_can(store, to, PERM_VIEW_CHANNEL |
                                                     PERM_MANAGE_CHANNELS);
  }

  overlay() {
    civue_rect from = drag_rows[s].r;

    if (l.valid) {
      civue_rect r = drag_rows[t].r;
      float line = l.below ? (float)r.y1 : (float)r.y0;
      civue_rect bar = civue_rect_from(r.x0 + 8, line - 2, r.x1, line + 2);

      civue_shadow(cui, bar, 2, 3, civue_v2(0, 0), rgba(0x00000066));
      fill(bar, k->drop, 2);
    }

    // a ghost of the row under the pointer, as a browser drags one.
    civue_rect ghost =
      civue_rect_from(from.x0, y - (from.y1 - from.y0) / 2, from.x1,
                      y + (from.y1 - from.y0) / 2);
    char name[128];

    snprintf(name, sizeof name, "%s%s",
             bucket_of(dragged) == BUCKET_TEXT ? "# " : "",
             dragged->name ? dragged->name : "");
    fill(ghost, civue_alpha(k->surface_high, .82f), 8);
    civue_text_in(
      cui, name, civue_node_text_style(cui),
      civue_rect_from(ghost.x0 + 12, ghost.y0, ghost.x1 - 8, ghost.y1),
      civue_alpha(k->strong, .8f), CIVUE_ALIGN_START);
  }

  civue_set_cursor(cui, CIVUE_CURSOR_GRABBING);
  civue_request_repaint(cui);

  if (cui->input.down[0] && !cui->input.released[0]) {
    return;
  }

  *d = (Drag){};

  if (!l.valid) {
    return;
  }

  if (dragged->type == CHANNEL_GUILD_CATEGORY) {
    move_category(app, g, dragged, l.reference);
    return;
  }

  if (l.reference != nullptr) {
    move_channel(app, g, dragged, l.reference, l.parent, 0);
    return;
  }

  List categories, list;

  categories_of(store, g, &categories);
  bucket_list(store, g, &categories, bucket_of(dragged), &list);
  remove_at(&list, index_in(&list, dragged));

  move_channel(app, g, dragged, nullptr, l.parent,
               slot_of(store, &list, &categories, l.parent, l.at_end));
}
