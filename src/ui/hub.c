#include "components.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/templates.h"
#include "widgets.h"

// discord's sizes for the directory.
constexpr float CARD_HEIGHT = 274;
constexpr float CARD_MIN = 248;
constexpr float GUTTER = 16;
constexpr float PAGE_MAX = 1608;
constexpr size_t NEWEST = 5;
constexpr size_t DESCRIPTION_MAX = 200;
constexpr size_t SERVER_NAME_MAX = 100;

// the categories a hub offers, in the order of its tabs.
static const DirectoryCategory CATEGORIES[] = {
  DIRECTORY_SCHOOL_CLUB,
  DIRECTORY_CLASS,
  DIRECTORY_STUDY_SOCIAL,
  DIRECTORY_MISC,
};

constexpr size_t NCATEGORIES = sizeof CATEGORIES / sizeof *CATEGORIES;

// one directory names its categories after a workplace's, as discord's
// client has it.
constexpr Snowflake WORK_DIRECTORY = 883060064561299456;

static const char *category_label(Snowflake channel_id,
                                  DirectoryCategory category) {
  bool work = channel_id == WORK_DIRECTORY;

  switch (category) {
    case DIRECTORY_SCHOOL_CLUB:
      return work ? "Work" : "Clubs";
    case DIRECTORY_CLASS:
      return work ? "Social" : "Classes & Subjects";
    case DIRECTORY_STUDY_SOCIAL:
      return work ? "ERGs" : "Social & Study";
    case DIRECTORY_MISC:
      return work ? "Archives" : "Miscellaneous";
    default:
      return "Uncategorized";
  }
}

static const char *guild_name_of(const Guild *g) {
  return g && g->name ? g->name : "this server";
}

static civue_icon template_icon(size_t index) {
  switch (index) {
    case 1:
      return CIVUE_ICON(lucide, book_open);
    case 2:
      return CIVUE_ICON(lucide, flag);
    case 3:
      return CIVUE_ICON(lucide, graduation_cap);
    case 4:
      return CIVUE_ICON(lucide, party_popper);
    case 5:
      return CIVUE_ICON(lucide, library);
    case 6:
      return CIVUE_ICON(lucide, house);
    default:
      return CIVUE_ICON(lucide, plus);
  }
}

// "1,234", as discord writes counts.
static void format_count(uint32_t n, char *out, size_t cap) {
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

static civue_color mix(civue_color a, civue_color b, float t) {
  return (civue_color){
    a.r + (b.r - a.r) * t,
    a.g + (b.g - a.g) * t,
    a.b + (b.b - a.b) * t,
    a.a + (b.a - a.a) * t,
  };
}

/* ========================================================================== */
/* what the user may do.                                                      */
/* ========================================================================== */

static bool can_add(App *app, const Channel *dir) {
  return store_can(app->session->store, dir, PERM_SEND_MESSAGES);
}

// discord's isEntryAdmin: an administrator of the guild listed.
static bool entry_admin(App *app, Snowflake guild_id) {
  Store *store = app->session->store;
  const Guild *g = store_guild(store, guild_id);

  return g && (store_guild_permissions(store, g) & PERM_ADMINISTRATOR);
}

static bool can_edit(App *app, const Channel *dir, Snowflake guild_id) {
  return entry_admin(app, guild_id) ||
         store_can(app->session->store, dir, PERM_MANAGE_MESSAGES);
}

/* ========================================================================== */
/* opening dialogs.                                                           */
/* ========================================================================== */

static void forget(char **text) {
  free(*text);
  *text = nullptr;
}

static void open_dialog(App *app, HubDialog dialog, Snowflake channel_id,
                        Snowflake entity_id) {
  Session *s = app->session;

  app->popover = POPOVER_NONE;
  app->hub_dialog = dialog;
  app->hub_channel = channel_id;
  app->hub_entity = entity_id;
  app->category_open = false;
  app->add_listed = false;
  app->add_new = false;
  app->add_template = -1;
  forget(&app->entry_description);
  forget(&app->new_server_name);
  app->entry_category = 0;
  str_set(&s->directory_error, nullptr);
  s->directory_saved = 0;
}

static void open_edit(App *app, Snowflake channel_id, Snowflake entity_id) {
  const DirectoryEntry *e = store_directory_entry(
    store_directory(app->session->store, channel_id), entity_id);

  open_dialog(app, HUB_DIALOG_EDIT, channel_id, entity_id);

  if (e) {
    str_set(&app->entry_description, e->description);
    app->entry_category = e->category;
  }
}

// the add dialog, the category of the tab open picked already.
static void open_add(App *app, Snowflake channel_id) {
  open_dialog(app, HUB_DIALOG_ADD, channel_id, 0);
  app->add_step = ADD_CHOOSE;

  if (app->directory_tab > 0) {
    app->entry_category = (uint8_t)app->directory_tab;
  }
}

void hub_join_open(App *app) {
  open_dialog(app, HUB_DIALOG_JOIN, 0, 0);
  forget(&app->hub_email);
  forget(&app->hub_code);
  forget(&app->hub_school);
  forget(&app->hub_search);
  session_hub_reset(app->session);
}

void hub_nick_open(App *app, Snowflake guild_id) {
  const Guild *g = store_guild(app->session->store, guild_id);
  const Member *me = store_member(g, app->session->store->self_id);

  open_dialog(app, HUB_DIALOG_NICK, 0, guild_id);
  str_set(&app->nick, me ? me->nick : nullptr);
}

void hub_leave_open(App *app, Snowflake guild_id) {
  open_dialog(app, HUB_DIALOG_LEAVE, 0, guild_id);
}

/* ========================================================================== */
/* pieces.                                                                    */
/* ========================================================================== */

// dots turning in a ring, while something loads.
static void spinner(const Look *k, float size) {
  civue_ui *ui = civue_ui_current();

  div(size(px(size)), flex_shrink_0) {
    civue_rect r = here();
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    float ring = size / 2 - 3;
    int lead = (int)(ui->now * 10) % 8;

    for (int i = 0; i < 8; i++) {
      float angle = (float)i * (float)M_PI / 4;
      float x = cx + ring * cosf(angle), y = cy + ring * sinf(angle);
      float fade = (float)((i - lead + 8) % 8) / 8;

      fill(xywh(x - 2.5f, y - 2.5f, 5, 5),
           civue_alpha(k->subtle, 1 - .85f * fade), 2.5f);
    }
  }

  civue_request_repaint_after(ui, 1.0 / 30);
}

// a dashed border inside `r`, `width` thick.
static void dashed(civue_rect r, civue_color color, float width) {
  constexpr float DASH = 6, SPACE = 4;
  float x0 = (float)r.x0, y0 = (float)r.y0, x1 = (float)r.x1,
        y1 = (float)r.y1;

  for (float x = x0 + 2; x < x1 - 2; x += DASH + SPACE) {
    float len = fminf(DASH, x1 - 2 - x);

    fill(xywh(x, y0, len, width), color, 0);
    fill(xywh(x, y1 - width, len, width), color, 0);
  }

  for (float y = y0 + 2; y < y1 - 2; y += DASH + SPACE) {
    float len = fminf(DASH, y1 - 2 - y);

    fill(xywh(x0, y, width, len), color, 0);
    fill(xywh(x1 - width, y, width, len), color, 0);
  }
}

// discord's banner: a radial gradient from blue at its left to clear,
// over black, the whole at .7 over the page. its ellipse is far taller
// than the banner, so it changes from left to right only.
void discovery_gradient(const Look *k, civue_rect r) {
  constexpr int STRIPS = 96;
  civue_color stops[3] = {
    rgb(0x2f72c1),
    {37 / 255.f, 34 / 255.f, 154 / 255.f, .8f},
    {25 / 255.f, 23 / 255.f, 92 / 255.f, 0},
  };
  float w = (float)civue_rect_w(r);

  for (int i = 0; i < STRIPS; i++) {
    float x = w * (float)i / STRIPS;
    float t = (x / w + .0395f) / 1.3066f;
    civue_color c = t < .3573f ? mix(stops[0], stops[1], t / .3573f)
                    : t < 1    ? mix(stops[1], stops[2],
                                     (t - .3573f) / (1 - .3573f))
                               : stops[2];
    // the color over black, then that at .7 over the page.
    civue_color over = {c.r * c.a, c.g * c.a, c.b * c.a, 1};

    fill(xywh((float)r.x0 + x, (float)r.y0, w / STRIPS + 1,
              (float)civue_rect_h(r)),
         mix(k->chat, over, .7f), 0);
  }
}

static void entry_icon(App *app, const DirectoryEntry *e, float size) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_guild_icon(url, e->guild_id, e->icon, (uint32_t)(size * 2));

  if (*url && image_fill(app, url, size * .3f)) {
    return;
  }

  char acronym[32];

  guild_acronym(e->name ? e->name : "?", acronym, sizeof acronym);
  fill(here(), k->surface_high, size * .3f);
  text(acronym, text_size(px(size >= 40 ? 16 : 12)), font_weight(medium),
       text_color(k->text), whitespace_nowrap);
}

static bool primary_button(const Look *k, civue_id id, const char *label,
                           bool enabled, civue_color color) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, h(10), px(4),
      rounded(lg), font_weight(medium),
      style(.bg = enabled ? color : k->hover,
            .text_color = enabled ? CIVUE_WHITE : k->faint,
            .has_text_color = true,
            .cursor = enabled ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true),
      hover(style(.bg = enabled ? mix(color, CIVUE_BLACK, .15f) : k->hover))) {
    pressed = button().activated && enabled;
    text(label);
  }

  return pressed;
}

static bool quiet_button(const Look *k, civue_id id, const char *label) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, h(10), px(4),
      rounded(lg), font_weight(medium), text_color(k->text),
      hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
    pressed = button().activated;
    text(label);
  }

  return pressed;
}

static bool link_text(const Look *k, civue_id id, const char *label) {
  bool pressed = false;

  div(key(id), text_color(k->link), hover(text_color(k->brand_hover)),
      cursor_pointer) {
    pressed = button().activated;
    text(label, whitespace_nowrap);
  }

  return pressed;
}

static void field_label(const Look *k, const char *label) {
  text(label, mt(2), mb(1), text_size(px(12)), line_height(px(16)),
       font_weight(semibold), text_color(k->subtle));
}

static civue_field_response line_field(App *app, civue_id id, char **value,
                                       const char *placeholder) {
  civue_field_response r = {};

  div(key(id), flex_shrink_0, h(10), text_size(px(16)), line_height(px(20))) {
    r = text_field(value, placeholder, field_look(&app->look));
  }

  return r;
}

// keeps `*value` within `most` bytes, cut where a character starts.
static void cap_text(char **value, size_t most) {
  size_t len = *value ? strlen(*value) : 0;

  if (len <= most) {
    return;
  }

  while (most > 0 && ((unsigned char)(*value)[most] & 0xc0) == 0x80) {
    most--;
  }

  (*value)[most] = 0;
}

static void description_field(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  char counter[24];

  cap_text(&app->entry_description, DESCRIPTION_MAX);
  snprintf(counter, sizeof counter, "%zu",
           DESCRIPTION_MAX - (app->entry_description
                                ? strlen(app->entry_description)
                                : 0));

  div(flex, items_end, justify_between) {
    field_label(k, "Server Description");
    text(counter, mb(1), text_size(px(12)), line_height(px(16)),
         text_color(k->faint));
  }

  div(key(id("entry-description")), flex_shrink_0, h(px(96)),
      text_size(px(16)), line_height(px(20))) {
    civue_text_area(ui, id("entry-description-area"), here(),
                    &app->entry_description,
                    "What's your server about? What do people do there?",
                    civue_style(16, 400, 20, 0), field_look(k));
  }
}

// discord's select of the categories, its options under it once open.
static void category_select(App *app, Snowflake channel_id) {
  const Look *k = &app->look;
  const char *picked = app->entry_category
                         ? category_label(channel_id, app->entry_category)
                         : "Select";

  field_label(k, "Category");

  div(key(id("category-select")), flex, items_center, gap(2), flex_shrink_0,
      h(10), px(3), rounded(lg), bg(rgba(0x0000001f)), border(1),
      border_color(app->category_open ? k->brand : k->border_strong),
      cursor_pointer) {
    if (button().activated) {
      app->category_open = !app->category_open;
    }

    text(picked, flex_1, min_w(0), truncate,
         text_color(app->entry_category ? k->text : rgb(0x8f9097)));

    if (app->category_open) {
      icon(lucide, chevron_up, size(4), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(lucide, chevron_down, size(4), flex_shrink_0, text_color(k->faint));
    }
  }

  if (!app->category_open) {
    return;
  }

  div(flex, flex_col, flex_shrink_0, mt(1), p(1), rounded(lg),
      bg(k->surface_high), border(1), border_color(k->border_strong)) {
    for (size_t i = 0; i < NCATEGORIES; i++) {
      DirectoryCategory category = CATEGORIES[i];
      bool on = app->entry_category == category;

      div(key(id_at(id("category-option"), category)), flex, items_center,
          h(9), px(2), rounded(md), style(.bg = on ? k->selected : CIVUE_CLEAR),
          hover(bg(on ? k->selected : k->hover)), cursor_pointer) {
        if (button().activated) {
          app->entry_category = category;
          app->category_open = false;
        }

        text(category_label(channel_id, category), flex_1,
             text_color(on ? k->strong : k->text));

        if (on) {
          icon(lucide, check, size(4), text_color(k->strong));
        }
      }
    }
  }
}

/* ========================================================================== */
/* the hub's rows over its channels.                                          */
/* ========================================================================== */

// the entries made since the user last looked, at most 5 as discord shows.
static uint32_t new_entries(App *app, const Channel *dir) {
  Store *store = app->session->store;
  const Directory *d = store_directory(store, dir->id);
  Snowflake acked = store_last_acked(store, dir->id);
  uint32_t count = 0;

  if (d == nullptr || acked == 0) {
    return 0;
  }

  int64_t since = snowflake_time_ms(acked);

  for (size_t i = 0; i < d->nentries && count < NEWEST; i++) {
    count += d->entries[i]->created_at > since;
  }

  return count;
}

static bool hub_row(App *app, civue_id id, civue_icon glyph_icon,
                    const char *label, bool selected, uint32_t badge) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), h(8), my(px(1)), mx(2), px(2),
      rounded(lg), text_color(selected ? k->strong : k->faint),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover),
            text_color(selected ? k->strong : k->text)),
      cursor_pointer) {
    pressed = button().activated;
    civue_node_icon(civue_ui_current(), glyph_icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20),
                                      .shrink = 0, .has_shrink = true));
    text(label, flex_1, min_w(0), truncate, font_weight(medium));
    mention_badge(k, badge);
  }

  return pressed;
}

void hub_rows(App *app, Guild *guild) {
  Session *s = app->session;
  Channel *dir = store_guild_directory(s->store, guild);

  div(flex, flex_col, flex_shrink_0, mt(px(14)), mb(2)) {
    if (dir) {
      bool open = s->selected_channel_id == dir->id;

      // the directory is fetched as the hub shows, for its count of new
      // servers.
      session_directory_open(s, dir->id);

      if (hub_row(app, id("hub-join"), CIVUE_ICON(lucide, compass),
                  "Join Servers", open, open ? 0 : new_entries(app, dir))) {
        app_select_channel(app, dir->id);
      }

      if (can_add(app, dir) &&
          hub_row(app, id("hub-add"), CIVUE_ICON(lucide, plus), "Add Servers",
                  false, 0)) {
        open_add(app, dir->id);
      }
    }

    if (hub_row(app, id("hub-invite"), CIVUE_ICON(lucide, user_plus),
                "Invite People", false, 0)) {
      app->popover = POPOVER_INVITE;
      app->invite_copied = false;
      session_create_invite(s, guild->id);
    }
  }
}

/* ========================================================================== */
/* the directory page.                                                        */
/* ========================================================================== */

static int by_members(const void *a, const void *b) {
  const DirectoryEntry *x = *(DirectoryEntry *const *)a;
  const DirectoryEntry *y = *(DirectoryEntry *const *)b;

  if (x->member_count != y->member_count) {
    return x->member_count > y->member_count ? -1 : 1;
  }

  return x->entity_id < y->entity_id ? -1 : x->entity_id > y->entity_id;
}

typedef CIVUE_VEC(const DirectoryEntry *) Entries;

static void push(Entries *list, const DirectoryEntry *e) {
  if (list->len == list->cap) {
    list->cap = list->cap ? list->cap * 2 : 32;
    list->v = realloc(list->v, list->cap * sizeof *list->v);

    if (list->v == nullptr) {
      cilo_oom(list->cap * sizeof *list->v);
    }
  }

  list->v[list->len++] = e;
}

static void sort_members(Entries *list) {
  if (list->len > 1) {
    qsort(list->v, list->len, sizeof *list->v, by_members);
  }
}

static void count_dot(const Look *k, civue_color color, const char *label) {
  div(flex, items_center, min_w(0), mr(4)) {
    div(size(2), mr(1), flex_shrink_0, rounded(full), bg(color)) {}
    text(label, min_w(0), truncate, text_size(px(12)), line_height(px(16)),
         text_color(k->subtle));
  }
}

// the card's menu, by the button that opened it or where it was right
// clicked.
static void card_menu(App *app, const Channel *dir) {
  if (app->popover != POPOVER_DIRECTORY_MENU) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Snowflake entity = app->directory_entity;
  bool editable = can_edit(app, dir, entity);
  bool edit = false, remove = false, copy = false;
  civue_rect anchor = app->directory_anchor, room = overlay_room(), card = {};
  // under the button its right edge lines up with, after the pointer
  // when right clicked.
  bool cursor = civue_rect_w(anchor) <= 1;
  float left = fminf(cursor ? (float)anchor.x0 : (float)anchor.x1 - 200,
                     (float)room.x1 - 200);

  overlay() {
    div(key(id("directory-menu")), flex, flex_col, w(px(200)),
        left(px(fmaxf(left, (float)room.x0))), top(px(anchor.y1 + 4)), p(2),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg)) {
      card = here();

      if (editable) {
        edit = menu_row(app, id("directory-edit"), CIVUE_ICON(lucide, pencil),
                        "Edit Settings", k->text);
        remove =
          menu_row(app, id("directory-remove"), CIVUE_ICON(lucide, trash_2),
                   "Remove from Hub", k->critical_text);
        div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}
      }

      copy = menu_row(app, id("directory-copy"), CIVUE_ICON(lucide, copy),
                      "Copy Server ID", k->text);
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }

  if (edit) {
    open_edit(app, dir->id, entity);
  }
  else if (remove) {
    open_dialog(app, HUB_DIALOG_REMOVE, dir->id, entity);
  }
  else if (copy) {
    char digits[21];

    snowflake_format(entity, digits);
    civue_clipboard_set_text(digits, strlen(digits));
    app->popover = POPOVER_NONE;
  }
}

static void open_menu(App *app, Snowflake entity, civue_rect anchor) {
  bool same =
    app->popover == POPOVER_DIRECTORY_MENU && app->directory_entity == entity;

  app->popover = same ? POPOVER_NONE : POPOVER_DIRECTORY_MENU;
  app->directory_entity = entity;
  app->directory_anchor = anchor;
}

static void card_join(App *app, const Channel *dir, const DirectoryEntry *e) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool member = store_guild(s->store, e->guild_id) != nullptr;
  bool joining = s->directory_joining == e->guild_id;
  civue_id id = id_at(id("directory-join"), e->entity_id);

  if (member) {
    div(key(id), flex, items_center, justify_center, flex_shrink_0, h(8),
        mt(4), rounded(lg), bg(k->surface_high), hover(bg(k->hover)),
        text_size(px(14)), font_weight(medium), text_color(k->strong),
        cursor_pointer) {
      if (button().activated) {
        app_select_guild(app, e->guild_id);
      }

      text("Go to Server");
    }

    return;
  }

  div(key(id), flex, items_center, justify_center, flex_shrink_0, h(8), mt(4),
      rounded(lg), bg(k->brand), hover(bg(k->brand_hover)),
      active(bg(k->brand_active)), text_size(px(14)), font_weight(medium),
      text_color(CIVUE_WHITE),
      style(.cursor = joining ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (button().activated && !joining) {
      app->directory_join = e->guild_id;
      session_directory_join(s, dir->id, e->guild_id);
    }

    text(joining ? "Joining..." : "Join");
  }
}

static void entry_card(App *app, const Channel *dir, const DirectoryEntry *e) {
  const Look *k = &app->look;
  Session *s = app->session;
  civue_ui *ui = civue_ui_current();
  civue_id card_id = id_at(id("directory-card"), e->entity_id);
  bool menu_open = app->popover == POPOVER_DIRECTORY_MENU &&
                   app->directory_entity == e->entity_id;
  bool failed = app->directory_join == e->guild_id &&
                s->directory_joining == 0 && s->directory_join_error;
  char url[CDN_URL_MAX], online[48], members[48], count[24];

  format_count(e->presence_count, count, sizeof count);
  snprintf(online, sizeof online, "%s Online", count);
  format_count(e->member_count, count, sizeof count);
  snprintf(members, sizeof members, "%s Member%s", count,
           e->member_count == 1 ? "" : "s");

  div(key(card_id), relative, flex, flex_col, h(px(CARD_HEIGHT)), min_w(0),
      rounded(lg), border(1), border_color(k->border), bg(k->surface),
      overflow_hidden) {
    civue_rect r = here();
    civue_response hit = civue_interact(ui, id_at(card_id, 1), r,
                                        CIVUE_SENSE_CLICK);
    // a dialog or a popover over the page holds the pointer.
    bool covered = app->hub_dialog != HUB_DIALOG_NONE || app->settings_open ||
                   app->voice_switch ||
                   (app->popover != POPOVER_NONE &&
                    app->popover != POPOVER_DIRECTORY_MENU);
    bool over = !covered && ui->input.has_mouse &&
                civue_rect_contains(r, ui->input.mouse);

    if (hit.right_clicked) {
      open_menu(app, e->entity_id,
                xywh(ui->input.mouse.x, ui->input.mouse.y, 1, 1));
    }

    div(flex_shrink_0, h(px(40)), mb(px(36)), bg(k->hover)) {
      cdn_guild_splash(url, e->guild_id, e->splash, 600);

      if (*url) {
        image_cover(app, url, 0);
      }
    }

    div(absolute, left(px(12)), top(px(20)), size(px(48)), p(1),
        rounded(px(16)), bg(k->chat)) {
      div(flex, items_center, justify_center, size(full), rounded(px(12)),
          overflow_hidden) {
        entry_icon(app, e, 40);
      }
    }

    div(flex, flex_col, flex_1, min_h(0), px(4), pb(4)) {
      text(e->name ? e->name : "", flex_shrink_0, truncate, text_size(px(16)),
           line_height(px(20)), font_weight(semibold), text_color(k->strong));

      div(flex_shrink_0, max_h(px(72)), mt(1), overflow_hidden) {
        text(e->description ? e->description : "", text_size(px(14)),
             line_height(px(18)), text_color(k->subtle));
      }

      div(flex_1, min_h(4)) {}

      if (failed) {
        text(s->directory_join_error, flex_shrink_0, truncate,
             text_size(px(12)), line_height(px(16)),
             text_color(k->critical_text));
      }
      else {
        div(flex, items_center, flex_shrink_0, min_w(0)) {
          count_dot(k, k->online, online);
          count_dot(k, k->offline, members);
        }
      }

      card_join(app, dir, e);
    }

    if (over || menu_open) {
      div(absolute, right(px(16)), top(px(24)), flex, gap(1), p(px(2)),
          rounded(lg), bg(k->surface_high), border(1),
          border_color(k->border_strong)) {
        if (can_edit(app, dir, e->entity_id) &&
            icon_button(app, id_at(id("directory-pencil"), e->entity_id),
                        CIVUE_ICON(lucide, pencil), 28, 18, "Edit Settings")) {
          open_edit(app, dir->id, e->entity_id);
        }

        civue_rect anchor = {};

        div(flex) {
          anchor = here();

          if (icon_button(app, id_at(id("directory-more"), e->entity_id),
                          CIVUE_ICON(lucide, ellipsis), 28, 18, "More")) {
            open_menu(app, e->entity_id, anchor);
          }
        }
      }
    }
  }
}

static void add_card(App *app, const Channel *dir) {
  const Look *k = &app->look;

  div(key(id("directory-add")), flex, flex_col, items_center, justify_center,
      h(px(CARD_HEIGHT)), rounded(sm), text_color(k->subtle),
      hover(bg(k->hover), text_color(k->strong)), active(bg(k->pressed)),
      cursor_pointer) {
    if (button().activated) {
      open_add(app, dir->id);
    }

    dashed(here(), k->muted, 2);
    icon(lucide, circle_plus, size(10));
    text("Add Server", mt(2), font_weight(semibold));
  }
}

// cards as many to a row as fit, discord's grid. `add` puts the add card
// last.
static void card_grid(App *app, const Channel *dir, civue_id list,
                      const DirectoryEntry **v, size_t n, bool add) {
  float *width = civue_memory(civue_ui_current(), list, sizeof *width, nullptr);
  uint16_t cols = (uint16_t)fmaxf(1, floorf((*width + GUTTER) /
                                            (CARD_MIN + GUTTER)));

  if (n == 0 && !add) {
    return;
  }

  div(key(list), grid, grid_cols(cols), gap(4), py(6), w(full)) {
    *width = (float)civue_rect_w(here());

    for (size_t i = 0; i < n; i++) {
      entry_card(app, dir, v[i]);
    }

    if (add) {
      add_card(app, dir);
    }
  }
}

static void section_header(const Look *k, const char *label) {
  text(label, mt(5), text_size(px(16)), line_height(px(20)),
       font_weight(semibold), text_color(k->strong));
}

static void search_bar(App *app, const Channel *dir) {
  const Look *k = &app->look;
  civue_field_look look = field_look(k);
  bool submitted = false;

  look.idle.bg = look.hovered.bg = look.editing.bg = k->surface;
  look.idle.radius = look.hovered.radius = look.editing.radius = 4;
  look.idle.border = look.hovered.border = k->border;

  div(key(id("directory-search")), relative, flex_shrink_0, w(full), h(11),
      text_size(px(16)), line_height(px(20))) {
    submitted = text_field(&app->directory_field, "Explore servers", look)
                  .submitted;

    civue_rect r = here();
    civue_icon_centered(civue_ui_current(), CIVUE_ICON(lucide, search),
                        xywh((float)r.x1 - 36, (float)r.y0, 28,
                             (float)civue_rect_h(r)),
                        18, k->faint);
  }

  // discord searches on enter, never as the user types.
  const char *words = app->directory_field;

  while (words && *words == ' ') {
    words++;
  }

  if (submitted && words && *words) {
    str_set(&app->directory_query, words);
    session_directory_search(app->session, dir->id, words, -1);
  }
}

static void banner(App *app, const Channel *dir) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool narrow = civue_rect_w(civue_node_rect(ui)) < 800;

  div(key(id("directory-banner")), relative, flex, justify_center,
      flex_shrink_0, min_h(px(200)), rounded(lg), overflow_hidden) {
    discovery_gradient(k, here());

    div(flex, flex_col, items_center, flex_1, max_w(px(700)), py(8),
        mx(px(narrow ? 36 : 60))) {
      text("Find your people", text_center, text_size(px(24)),
           line_height(px(32)), font_weight(semibold), text_color(k->strong));
      text("From clubs, to study groups, to game nights, there's a place "
           "for you.",
           text_center, my(2), text_size(px(16)), line_height(px(20)),
           text_color(k->text));
      search_bar(app, dir);
    }
  }
}

static void tab(App *app, int8_t which, const char *label) {
  const Look *k = &app->look;
  bool on = app->directory_tab == which;

  div(key(id_at(id("directory-tab"), (uint64_t)(which + 1))), relative,
      flex_shrink_0, mr(8), pb(4), text_size(px(16)), line_height(px(20)),
      font_weight(medium), text_color(on ? k->strong : k->subtle),
      hover(text_color(on ? k->strong : k->text)), cursor_pointer) {
    if (button().activated) {
      app->directory_tab = which;
    }

    text(label, whitespace_nowrap);

    if (on) {
      civue_rect r = here();
      fill(xywh((float)r.x0, (float)r.y1, (float)civue_rect_w(r), 2), k->brand,
           1);
    }
  }
}

static void tabs(App *app, const Channel *dir, const Directory *d,
                 size_t shown) {
  const Look *k = &app->look;
  char label[64];

  div(key(id("directory-tabs")), flex, flex_wrap, flex_shrink_0, mt(5),
      border_b(2), border_color(k->border)) {
    snprintf(label, sizeof label, "Home (%zu)", shown);
    tab(app, -1, label);

    for (size_t i = 0; i < NCATEGORIES; i++) {
      DirectoryCategory category = CATEGORIES[i];
      const char *name = category_label(dir->id, category);

      if (d->counted) {
        snprintf(label, sizeof label, "%s (%u)", name, d->counts[category]);
      }
      else {
        snprintf(label, sizeof label, "%s", name);
      }

      tab(app, (int8_t)category, label);
    }
  }
}

static int by_newest(const void *a, const void *b) {
  const DirectoryEntry *x = *(DirectoryEntry *const *)a;
  const DirectoryEntry *y = *(DirectoryEntry *const *)b;

  if (x->created_at != y->created_at) {
    return x->created_at > y->created_at ? -1 : 1;
  }

  return x->entity_id > y->entity_id ? -1 : x->entity_id < y->entity_id;
}

// discord's "Newest": the five latest of the entries it may feature.
static void home(App *app, const Channel *dir, const Entries *all) {
  Entries newest = {}, rest = {};

  for (size_t i = 0; i < all->len; i++) {
    if (all->v[i]->featurable) {
      push(&newest, all->v[i]);
    }
  }

  if (newest.len > 1) {
    qsort(newest.v, newest.len, sizeof *newest.v, by_newest);
  }

  newest.len = newest.len < NEWEST ? newest.len : NEWEST;

  for (size_t i = 0; i < all->len; i++) {
    bool shown = false;

    for (size_t j = 0; j < newest.len && !shown; j++) {
      shown = newest.v[j] == all->v[i];
    }

    if (!shown) {
      push(&rest, all->v[i]);
    }
  }

  sort_members(&rest);

  if (newest.len) {
    section_header(&app->look, "Newest");
    card_grid(app, dir, id("directory-newest"), newest.v, newest.len, false);
  }

  if (newest.len || rest.len) {
    section_header(&app->look, "All Servers");
    card_grid(app, dir, id("directory-all"), rest.v, rest.len,
              can_add(app, dir));
  }

  free(newest.v);
  free(rest.v);
}

static void back_from_search(App *app) {
  forget(&app->directory_query);
  forget(&app->directory_field);
  session_directory_search(app->session, 0, nullptr, -1);
}

static void search_results(App *app, const Channel *dir, const Directory *d) {
  const Look *k = &app->look;
  const SessionDirectorySearch *search = &app->session->directory_search;
  Entries found = {};
  char count[24], quoted[160];

  for (size_t i = 0; search->channel_id == dir->id && i < search->nresults;
       i++) {
    const DirectoryEntry *e = store_directory_entry(d, search->results[i]);

    if (e && e->type == DIRECTORY_ENTRY_GUILD) {
      push(&found, e);
    }
  }

  sort_members(&found);
  format_count((uint32_t)found.len, count, sizeof count);
  snprintf(quoted, sizeof quoted, "\"%s\"", app->directory_query);

  div(flex, flex_col, gap(2), flex_shrink_0) {
    div(flex, items_center, min_w(0)) {
      if (icon_button(app, id("directory-back"), CIVUE_ICON(lucide, arrow_left),
                      32, 20, "Back")) {
        back_from_search(app);
      }

      div(flex, items_baseline, gap(1), min_w(0), ml(3), text_size(px(16)),
          line_height(px(20))) {
        if (!search->loading) {
          text(count, flex_shrink_0, font_weight(bold), text_color(k->strong));
          text(found.len == 1 ? "community for" : "communities for",
               flex_shrink_0, text_color(k->muted));
        }
        else {
          text("Searching for", flex_shrink_0, text_color(k->muted));
        }

        text(quoted, min_w(0), truncate, font_weight(bold),
             text_color(k->strong));
      }
    }

    search_bar(app, dir);
  }

  if (!app->directory_query) {
    free(found.v);
    return;
  }

  if (search->loading) {
    div(flex, justify_center, mt(px(56))) {
      spinner(k, 32);
    }
  }
  else if (found.len) {
    card_grid(app, dir, id("directory-found"), found.v, found.len, false);
  }
  else {
    div(flex, flex_col, items_center, gap(2), my(6), py(px(92)), px(4),
        rounded(lg), bg(k->chat), border(1), border_color(k->border)) {
      text("No matches found", text_size(px(20)), line_height(px(24)),
           font_weight(semibold), text_color(k->strong));

      if (search->error) {
        text(search->error, text_center, text_color(k->critical_text));
      }
      else if (can_add(app, dir)) {
        div(flex, items_center, gap(1), text_color(k->subtle)) {
          text("Try another search or");

          if (link_text(k, id("directory-empty-add"), "add a server")) {
            open_add(app, dir->id);
          }

          text(".", ml(px(-4)));
        }
      }
      else {
        text("Try another search", text_color(k->subtle));
      }
    }
  }

  free(found.v);
}

static void directory_header(App *app, Channel *c) {
  const Look *k = &app->look;
  const Guild *g = store_guild(app->session->store, c->guild_id);
  char title[160];

  if (g && g->hub) {
    snprintf(title, sizeof title, "Student Hub for %s", guild_name_of(g));
  }
  else {
    snprintf(title, sizeof title, "%s",
             g && g->name ? g->name : c->name ? c->name : "");
  }

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("directory-header"));
    icon(lucide, compass, size(5), flex_shrink_0, text_color(k->faint));
    text(title, flex_1, min_w(0), truncate, font_weight(semibold),
         text_color(k->strong));

    if (icon_button(
          app, id("directory-members"), CIVUE_ICON(lucide, users), 32, 20,
          app->members_hidden ? "Show Member List" : "Hide Member List")) {
      app->members_hidden = !app->members_hidden;
    }
  }
}

void directory_page(App *app, Channel *c) {
  const Look *k = &app->look;
  Session *s = app->session;
  Directory *d = store_open_directory(s->store, c->id);
  Entries all = {};
  size_t shown = 0;

  session_directory_open(s, c->id);
  session_ack(s, c->id);

  for (size_t i = 0; i < d->nentries; i++) {
    const DirectoryEntry *e = d->entries[i];

    // discord's client shows no entry without a guild.
    if (e->type != DIRECTORY_ENTRY_GUILD) {
      continue;
    }

    shown++;

    if (app->directory_tab < 0 || e->category == app->directory_tab) {
      push(&all, e);
    }
  }

  // a guild joined from here opens once the gateway brought it.
  if (app->directory_join && store_guild(s->store, app->directory_join) &&
      s->directory_joining == app->directory_join) {
    Snowflake id = app->directory_join;

    app->directory_join = 0;
    s->directory_joining = 0;
    app_select_guild(app, id);
    free(all.v);
    return;
  }

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    directory_header(app, c);

    div(flex, flex_1, min_h(0)) {
      div(key(id_at(id("directory-scroll"), c->id)), flex_1, min_w(0),
          h(full), overflow_y_scroll, scrollbars(&app->bars), p(8)) {
        div(flex, flex_col, w(full), max_w(px(PAGE_MAX)), mx(auto),
            min_w(px(320))) {
          if (app->directory_query) {
            search_results(app, c, d);
          }
          else {
            banner(app, c);
            tabs(app, c, d, shown);

            if (!d->loaded) {
              div(flex, justify_center, mt(px(56))) {
                spinner(k, 32);
              }
            }
            else if (app->directory_tab < 0) {
              home(app, c, &all);
            }
            else {
              sort_members(&all);
              card_grid(app, c, id("directory-category"), all.v, all.len,
                        can_add(app, c));
            }
          }
        }
      }

      if (!app->members_hidden) {
        member_list(app, c);
      }
    }
  }

  card_menu(app, c);
  free(all.v);
}

/* ========================================================================== */
/* dialogs.                                                                   */
/* ========================================================================== */

// a dialog in the middle of the window over a dark backdrop that closes it
// when pressed.
#define hub_modal(name, width, closed)                                       \
  overlay()                                                                  \
  div(key(id(name "-backdrop")), left(0), top(0), w(px(ui->size.x)),         \
      h(px(ui->size.y)), flex, items_center, justify_center,                 \
      bg(rgba(0x000000b3)))                                                  \
  for (bool hub_once_ = ((closed) |= button().activated, true); hub_once_;   \
       hub_once_ = false)                                                    \
  div(key(id(name)), flex, flex_col, w(px(width)),                           \
      max_h(px(ui->size.y - 48)), p(6), rounded(xl), bg(k->surface),         \
      border(1), border_color(k->border), shadow(lg), text_color(k->text),   \
      text_size(px(14)), line_height(px(18)))                                \
  for (bool hub_inner_ = (button(), true); hub_inner_; hub_inner_ = false)

static void dialog_title(const Look *k, const char *title,
                         const char *subtitle) {
  text(title, text_center, text_size(px(20)), line_height(px(24)),
       font_weight(semibold), text_color(k->strong));

  if (subtitle) {
    text(subtitle, text_center, mt(2), text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));
  }
}

static void error_line(const Look *k, const char *error) {
  if (error) {
    text(error, mt(2), text_color(k->critical_text));
  }
}

static int by_name(const void *a, const void *b) {
  const Guild *x = *(Guild *const *)a, *y = *(Guild *const *)b;

  return strcasecmp(x->name ? x->name : "", y->name ? y->name : "");
}

// the guilds the user could list: theirs to administer, the hub left out.
static size_t admin_guilds(App *app, Snowflake hub_id, Guild **out,
                           size_t cap) {
  Store *store = app->session->store;
  size_t count = 0, n = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds && n < cap; j++) {
      Guild *g = folders[i].guilds[j];

      if (g->id != hub_id && !g->unavailable &&
          (store_guild_permissions(store, g) & PERM_ADMINISTRATOR)) {
        out[n++] = g;
      }
    }
  }

  qsort(out, n, sizeof *out, by_name);
  return n;
}

static void choose_tab(App *app, bool listed, const char *label) {
  const Look *k = &app->look;
  bool on = app->add_listed == listed;

  div(key(id_at(id("add-tab"), listed)), flex, items_center, justify_center,
      flex_1, h(8), rounded(md), font_weight(medium),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      text_color(on ? k->strong : k->subtle), hover(text_color(k->strong)),
      cursor_pointer) {
    if (button().activated) {
      app->add_listed = listed;
    }

    text(label, truncate);
  }
}

static void choose_guild(App *app, const Channel *dir, const Guild *hub) {
  const Look *k = &app->look;
  Session *s = app->session;
  const Directory *d = store_directory(s->store, dir->id);
  Guild *guilds[256];
  size_t n = admin_guilds(app, hub ? hub->id : 0, guilds, 256);
  char title[160];
  size_t shown = 0;

  // discord asks once which of them the directory lists already.
  civue_id asked = id_at(id("add-asked"), dir->id);
  bool *done = civue_memory(civue_ui_current(), asked, sizeof *done, nullptr);

  if (!*done && n) {
    Snowflake ids[256];

    for (size_t i = 0; i < n; i++) {
      ids[i] = guilds[i]->id;
    }

    session_directory_check(s, dir->id, ids, n);
    *done = true;
  }

  snprintf(title, sizeof title, "Add a server to %s", guild_name_of(hub));
  dialog_title(k, title,
               "Your server will be discoverable by people in this Hub. You "
               "can add a server if you have Admin permissions for that "
               "server.");

  div(flex, gap(1), flex_shrink_0, mt(4), p(1), rounded(lg), bg(k->chat)) {
    choose_tab(app, false, "Servers You Can Add");
    choose_tab(app, true, "Servers You've Added");
  }

  div(key(id("add-guilds")), flex, flex_col, gap(1), flex_shrink_0,
      h(px(240)), mt(3), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < n; i++) {
      Guild *g = guilds[i];
      bool listed = store_directory_entry(d, g->id) != nullptr;

      if (listed != app->add_listed) {
        continue;
      }

      shown++;

      div(key(id_at(id("add-guild"), g->id)), flex, items_center, gap(3),
          flex_shrink_0, h(12), px(3), rounded(lg), bg(k->chat),
          hover(bg(k->hover)), cursor_pointer) {
        bool pressed = button().activated;

        div(flex, items_center, justify_center, size(8), flex_shrink_0,
            rounded(lg), overflow_hidden) {
          guild_icon(app, g, 32, false);
        }

        text(g->name ? g->name : "", flex_1, min_w(0), truncate,
             font_weight(semibold), text_color(k->strong));

        if (listed) {
          if (icon_button(app, id_at(id("add-edit"), g->id),
                          CIVUE_ICON(lucide, pencil), 28, 16,
                          "Edit Settings")) {
            open_edit(app, dir->id, g->id);
          }

          if (icon_button(app, id_at(id("add-remove"), g->id),
                          CIVUE_ICON(lucide, trash_2), 28, 16,
                          "Remove from Hub")) {
            open_dialog(app, HUB_DIALOG_REMOVE, dir->id, g->id);
          }
        }
        else {
          icon(lucide, chevron_right, size(4), flex_shrink_0,
               text_color(k->faint));

          if (pressed) {
            app->hub_entity = g->id;
            app->add_new = false;
            app->add_step = ADD_DETAILS;
          }
        }
      }
    }

    if (!shown) {
      text(app->add_listed ? "You haven't added any servers yet!"
                           : "You are not an admin of any unlisted servers.",
           text_center, mt(12), text_color(k->faint));
    }
  }

  div(flex, items_center, gap(3), flex_shrink_0, mt(4), pt(4), border_t(1),
      border_color(k->border)) {
    text("Want to start a new group?", flex_1, min_w(0),
         font_weight(semibold), text_color(k->strong));

    if (primary_button(k, id("add-create"), "Create New Server", true,
                       k->brand)) {
      app->add_step = ADD_TEMPLATES;
    }
  }
}

static void template_row(App *app, size_t index, const char *label) {
  const Look *k = &app->look;

  div(key(id_at(id("add-template"), index)), flex, items_center, flex_shrink_0,
      w(full), mb(2), rounded(lg), bg(k->message_hover), border(1),
      border_color(k->border), hover(bg(k->hover)), active(bg(k->pressed)),
      cursor_pointer) {
    if (button().activated) {
      app->add_template = (int8_t)index;
      app->add_new = true;
      app->add_step = ADD_CUSTOMIZE;

      if (!app->new_server_name) {
        char name[96];

        snprintf(name, sizeof name, "%s's server",
                 store_user_name(app->session->store->self));
        str_set(&app->new_server_name, name);
      }
    }

    div(flex, items_center, justify_center, size(12), m(4), flex_shrink_0,
        rounded(full), bg(k->brand), text_color(CIVUE_WHITE)) {
      glyph(template_icon(index), 24, CIVUE_WHITE);
    }

    text(label, flex_1, min_w(0), truncate, text_size(px(16)),
         line_height(px(20)), font_weight(semibold), text_color(k->strong));
    icon(lucide, chevron_right, size(5), mr(4), flex_shrink_0,
         text_color(k->faint));
  }
}

static void choose_template(App *app, const Guild *hub) {
  const Look *k = &app->look;
  char title[160];

  snprintf(title, sizeof title, "Help grow the Student Hub for %s",
           guild_name_of(hub));
  dialog_title(k, title,
               "You will still be able to invite people from outside the Hub "
               "to your server.");

  div(key(id("add-templates")), flex, flex_col, flex_shrink_0, h(px(330)),
      mt(4), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    template_row(app, 0, hub_templates[0].label);
    text("Start from a template", mt(4), mb(2), text_size(px(14)),
         font_weight(medium), text_color(k->subtle));

    for (size_t i = 1; i < hub_template_count; i++) {
      template_row(app, i, hub_templates[i].label);
    }
  }

  div(flex, flex_shrink_0, mt(4)) {
    if (quiet_button(k, id("add-templates-back"), "Back")) {
      app->add_step = ADD_CHOOSE;
    }
  }
}

static void customize(App *app) {
  const Look *k = &app->look;
  bool named = app->new_server_name && *app->new_server_name;
  bool next = false;

  cap_text(&app->new_server_name, SERVER_NAME_MAX);
  dialog_title(k, "Customize Your Server",
               "Give your new server a personality with a name. You can "
               "always change it later.");

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    field_label(k, "Server Name");
    next = line_field(app, id("add-server-name"), &app->new_server_name, "")
             .submitted;
  }

  div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
    if (quiet_button(k, id("add-customize-back"), "Back")) {
      app->add_step = ADD_TEMPLATES;
    }

    div(flex_1) {}
    next |= primary_button(k, id("add-next"), "Next", named, k->brand);
  }

  if (next && named) {
    app->add_step = ADD_DETAILS;
  }
}

static void details(App *app, const Channel *dir) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool ready = app->entry_description && *app->entry_description &&
               app->entry_category != 0 && !s->directory_saving;

  dialog_title(k, "One last step!",
               "Give your server a description and category to help people "
               "find it.");

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    description_field(app);
    category_select(app, dir->id);
  }

  error_line(k, s->directory_error);

  div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
    if (quiet_button(k, id("add-details-back"), "Back")) {
      app->add_step = app->add_new ? ADD_CUSTOMIZE : ADD_CHOOSE;
    }

    div(flex_1) {}

    if (primary_button(k, id("add-server"),
                       s->directory_saving ? "Adding..." : "Add Server", ready,
                       k->brand)) {
      if (app->add_new) {
        session_directory_create(s, dir->id, (size_t)app->add_template,
                                 app->new_server_name, app->entry_category,
                                 app->entry_description);
      }
      else {
        session_directory_add(s, dir->id, app->hub_entity, app->entry_category,
                              app->entry_description);
      }
    }
  }

  // listed: the guild made or picked is the one discord answered for.
  if (s->directory_saved && (app->add_new || s->directory_saved ==
                                               app->hub_entity)) {
    app->hub_entity = s->directory_saved;
    s->directory_saved = 0;
    app->add_step = ADD_DONE;
  }
}

static void done(App *app, const Guild *hub, bool *close) {
  const Look *k = &app->look;
  char line[200];

  if (app->add_new) {
    snprintf(line, sizeof line,
             "Your server has been created and added to the %s Student Hub",
             guild_name_of(hub));
  }
  else {
    snprintf(line, sizeof line,
             "Your server has been added to the %s Student Hub.",
             guild_name_of(hub));
  }

  div(flex, justify_center, flex_shrink_0, mb(4)) {
    div(flex, items_center, justify_center, size(16), rounded(full),
        bg(k->positive)) {
      glyph(CIVUE_ICON(lucide, check), 36, CIVUE_WHITE);
    }
  }

  dialog_title(k, "Success!", line);

  div(flex, gap(2), flex_shrink_0, mt(6)) {
    if (!app->add_new) {
      *close |= dialog_button(k, id("add-got-it"), "Got it!", true);
    }
    else {
      *close |= dialog_button(k, id("add-back-hub"), "Back to Hub", false);

      if (dialog_button(k, id("add-go"), "Go to Server", true) &&
          store_guild(app->session->store, app->hub_entity)) {
        *close = true;
        app_select_guild(app, app->hub_entity);
      }
    }
  }
}

static void add_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  const Channel *dir = store_channel(store, app->hub_channel);
  const Guild *hub = dir ? store_guild(store, dir->guild_id) : nullptr;
  bool close = false;

  if (!dir) {
    app->hub_dialog = HUB_DIALOG_NONE;
    return;
  }

  hub_modal("add-dialog", 460, close) {
    switch (app->add_step) {
      case ADD_CHOOSE:
        choose_guild(app, dir, hub);
        break;
      case ADD_TEMPLATES:
        choose_template(app, hub);
        break;
      case ADD_CUSTOMIZE:
        customize(app);
        break;
      case ADD_DETAILS:
        details(app, dir);
        break;
      case ADD_DONE:
        done(app, hub, &close);
        break;
    }
  }

  if (close || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->hub_dialog = HUB_DIALOG_NONE;
    civue_forget(ui, id_at(id("add-asked"), dir->id));
  }
}

static void edit_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const Directory *d = store_directory(s->store, app->hub_channel);
  const DirectoryEntry *e = store_directory_entry(d, app->hub_entity);
  bool close = false;
  char title[160];

  if (!e) {
    app->hub_dialog = HUB_DIALOG_NONE;
    return;
  }

  const char *was = e->description ? e->description : "";
  const char *now = app->entry_description ? app->entry_description : "";
  bool changed = strcmp(was, now) != 0 || e->category != app->entry_category;
  bool ready = changed && *now && app->entry_category && !s->directory_saving;

  snprintf(title, sizeof title, "Edit Settings for %s",
           e->name ? e->name : "this server");

  hub_modal("edit-dialog", 460, close) {
    dialog_title(k, title,
                 "This server description will appear in the Student Hub.");

    div(flex, flex_col, flex_shrink_0, mt(4)) {
      description_field(app);
      category_select(app, app->hub_channel);
    }

    error_line(k, s->directory_error);

    div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
      div(flex_1) {}
      close |= quiet_button(k, id("edit-cancel"), "Cancel");

      if (primary_button(k, id("edit-save"),
                         s->directory_saving ? "Saving..." : "Save", ready,
                         k->brand)) {
        session_directory_edit(s, app->hub_channel, e->entity_id,
                               app->entry_category, app->entry_description);
      }
    }
  }

  if (s->directory_saved == app->hub_entity) {
    s->directory_saved = 0;
    close = true;
  }

  if (close || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->hub_dialog = HUB_DIALOG_NONE;
  }
}

// asks before something that cannot be undone, true once confirmed.
static bool confirm_dialog(App *app, const char *title, const char *line,
                           const char *confirm) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool close = false, yes = false;

  hub_modal("hub-confirm", 420, close) {
    text(title, text_size(px(20)), line_height(px(24)), font_weight(semibold),
         text_color(k->strong));
    text(line, mt(2), text_size(px(16)), line_height(px(20)),
         text_color(k->subtle));

    div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
      div(flex_1) {}
      close |= quiet_button(k, id("hub-confirm-cancel"), "Cancel");
      yes = primary_button(k, id("hub-confirm-yes"), confirm, true, k->danger);
    }
  }

  if (yes || close || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->hub_dialog = HUB_DIALOG_NONE;
  }

  return yes;
}

static void remove_dialog(App *app) {
  const DirectoryEntry *e = store_directory_entry(
    store_directory(app->session->store, app->hub_channel), app->hub_entity);
  char line[200];

  snprintf(line, sizeof line,
           "Are you sure you want to remove %s from this Student Hub?",
           e && e->name ? e->name : "this server");

  if (confirm_dialog(app, "Remove from Hub", line, "Remove")) {
    session_directory_remove(app->session, app->hub_channel, app->hub_entity);
  }
}

static void leave_dialog(App *app) {
  const Guild *g = store_guild(app->session->store, app->hub_entity);
  char title[160], line[200];

  if (!g) {
    app->hub_dialog = HUB_DIALOG_NONE;
    return;
  }

  snprintf(title, sizeof title, "Leave '%s'", guild_name_of(g));
  snprintf(line, sizeof line, "Are you sure you want to leave %s?",
           guild_name_of(g));

  if (confirm_dialog(app, title, line, "Leave Hub")) {
    Snowflake id = g->id;

    if (app->guild_id == id) {
      app_select_guild(app, 0);
    }

    session_leave_guild(app->session, id);
  }
}

static void nick_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const Guild *g = store_guild(app->session->store, app->hub_entity);
  bool close = false, save = false;
  char title[160];

  if (!g) {
    app->hub_dialog = HUB_DIALOG_NONE;
    return;
  }

  cap_text(&app->nick, 32);
  snprintf(title, sizeof title, "Welcome to the Student Hub for %s!",
           guild_name_of(g));

  hub_modal("nick-dialog", 440, close) {
    dialog_title(k, title, nullptr);

    div(flex, flex_col, flex_shrink_0, mt(4)) {
      field_label(k, "Your Name");
      save = line_field(app, id("nick-field"), &app->nick,
                        "Student Hub Nickname")
               .submitted;
      text("This will help your classmates recognize you in the Student Hub. "
           "You can change this any time via the Hub Settings.",
           mt(2), text_size(px(12)), line_height(px(16)),
           text_color(k->muted));
    }

    div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
      div(flex_1) {}
      close |= quiet_button(k, id("nick-cancel"), "Cancel");
      save |= primary_button(k, id("nick-save"), "Let's go!", true, k->brand);
    }
  }

  if (save) {
    session_set_nick(app->session, g->id, app->nick);
  }

  if (save || close || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->hub_dialog = HUB_DIALOG_NONE;
  }
}

static void join_email(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool ready = app->hub_email && strchr(app->hub_email, '@') && !s->hub.loading;
  bool submit = false;

  dialog_title(k,
               "Find study groups, clubs, and friends in the Student Hub for "
               "your school!",
               nullptr);

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    field_label(k, "Your Student Email");
    submit = line_field(app, id("hub-email"), &app->hub_email,
                        "name@school.edu")
               .submitted;

    div(mt(2)) {
      markup(app, id("hub-terms"),
             "Review Discord's [Terms of Service](https://discord.com/terms) "
             "and [Privacy Policy](https://discord.com/privacy) to learn more "
             "about how we use your data. Hubs are not affiliated with or "
             "managed by schools. Servers listed in the school's Student Hub "
             "are run by students. Users do not need to be a member of a "
             "Student Hub to be invited to join a server listed there.",
             &(MarkupOptions){.size = 12, .color = k->muted, .hint = 380});
    }
  }

  error_line(k, s->hub.error);

  div(flex, flex_col, flex_shrink_0, mt(6)) {
    submit |= primary_button(k, id("hub-join-submit"),
                             s->hub.loading ? "Sending..." : "Join Hub", ready,
                             k->brand);
  }

  if (submit && ready) {
    session_hub_lookup(s, app->hub_email, 0);
  }
}

static void school_row(App *app, const SessionHubSchool *school) {
  const Look *k = &app->look;
  Session *s = app->session;
  char url[CDN_URL_MAX];

  div(key(id_at(id("hub-school"), school->id)), flex, items_center,
      flex_shrink_0, mb(2), py(3), pl(3), pr(4), rounded(lg), bg(k->hover),
      hover(bg(k->selected)), cursor_pointer) {
    if (button().activated && !s->hub.loading) {
      session_hub_lookup(s, s->hub.email, school->id);
    }

    div(flex, items_center, justify_center, size(10), flex_shrink_0,
        rounded(full), bg(k->surface), overflow_hidden) {
      cdn_guild_icon(url, school->id, school->icon, 80);

      if (!*url || !image_fill(app, url, 20)) {
        char acronym[32];

        guild_acronym(school->name, acronym, sizeof acronym);
        text(acronym, text_size(px(14)), font_weight(medium),
             text_color(k->text), whitespace_nowrap);
      }
    }

    text(school->name, flex_1, min_w(0), ml(4), truncate, text_size(px(16)),
         line_height(px(20)), font_weight(semibold), text_color(k->strong));
    icon(lucide, chevron_right, size(5), flex_shrink_0, text_color(k->faint));
  }
}

static void join_school(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  size_t shown = 0;

  dialog_title(k, "Select your school", nullptr);

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    line_field(app, id("hub-school-search"), &app->hub_search, "Search");
  }

  div(key(id("hub-schools")), flex, flex_col, flex_shrink_0, max_h(px(400)),
      mt(3), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < s->hub.nschools; i++) {
      const SessionHubSchool *school = &s->hub.schools[i];

      if (app->hub_search && *app->hub_search &&
          !strcasestr(school->name, app->hub_search)) {
        continue;
      }

      shown++;
      school_row(app, school);
    }

    if (!shown) {
      text("No matches found", text_center, my(4), font_weight(semibold),
           text_color(k->strong));
    }
  }

  error_line(k, s->hub.error);

  div(flex, items_center, justify_center, gap(1), flex_shrink_0, mt(4),
      text_color(k->subtle)) {
    text("Can't find your school?");

    if (link_text(k, id("hub-to-waitlist"), "Join the waitlist.")) {
      s->hub.step = HUB_JOIN_WAITLIST;
    }
  }
}

static void join_code(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool ready = app->hub_code && *app->hub_code && !s->hub.loading;
  bool submit = false;
  char line[400];

  snprintf(line, sizeof line, "We sent a code to %s.",
           s->hub.email ? s->hub.email : "your email");
  dialog_title(k, "Verify your email to join", line);

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    field_label(k, "Enter Discord verification code");
    submit = line_field(app, id("hub-code"), &app->hub_code, "").submitted;
  }

  error_line(k, s->hub.error);

  div(flex, items_center, gap(2), flex_shrink_0, mt(6)) {
    if (link_text(k, id("hub-resend"), "Resend code") && !s->hub.loading) {
      session_hub_lookup(s, s->hub.email, s->hub.guild_id);
    }

    div(flex_1) {}
    submit |= primary_button(k, id("hub-code-submit"),
                             s->hub.loading ? "Verifying..." : "Submit", ready,
                             k->brand);
  }

  if (submit && ready) {
    session_hub_verify(s, app->hub_code);
  }
}

static void join_waitlist(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool ready = app->hub_school && strlen(app->hub_school) >= 3 &&
               !s->hub.loading;
  bool submit = false;

  cap_text(&app->hub_school, 20);
  dialog_title(k, "Join the waitlist",
               "The Hub for your school isn't live yet. Enter your school's "
               "name and we'll let you know when it is!");

  div(flex, flex_col, flex_shrink_0, mt(4)) {
    field_label(k, "School Name");
    submit = line_field(app, id("hub-school-name"), &app->hub_school,
                        "Full school name")
               .submitted;
  }

  error_line(k, s->hub.error);

  div(flex, flex_col, flex_shrink_0, mt(6)) {
    submit |= primary_button(k, id("hub-waitlist-submit"), "Join Waitlist",
                             ready, k->brand);
  }

  if (submit && ready) {
    session_hub_waitlist(s, app->hub_school);
  }
}

static void join_waiting(App *app) {
  const Look *k = &app->look;

  div(flex, justify_center, flex_shrink_0, mb(4)) {
    div(flex, items_center, justify_center, size(16), rounded(full),
        bg(k->positive)) {
      glyph(CIVUE_ICON(lucide, check), 36, CIVUE_WHITE);
    }
  }

  dialog_title(k, "Thanks! You're on the waitlist.", nullptr);

  div(flex, flex_col, flex_shrink_0, mt(6)) {
    if (primary_button(k, id("hub-other-email"), "Try A Different Email", true,
                       k->brand)) {
      forget(&app->hub_school);
      session_hub_reset(app->session);
    }
  }
}

static void join_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  bool close = false;

  // joined: the hub opens once the gateway brought it, and asks for a name.
  if (s->hub.step == HUB_JOIN_JOINED) {
    Snowflake id = s->hub.guild_id;
    const Guild *g = store_guild(s->store, id);

    if (g) {
      const Member *me = store_member(g, s->store->self_id);

      session_hub_reset(s);
      app->hub_dialog = HUB_DIALOG_NONE;
      app_select_guild(app, id);

      if (!me || !me->nick) {
        hub_nick_open(app, id);
      }
    }

    return;
  }

  hub_modal("join-dialog", 460, close) {
    div(flex, justify_center, flex_shrink_0, mb(4)) {
      div(flex, items_center, justify_center, size(16), rounded(full),
          bg(k->brand)) {
        glyph(CIVUE_ICON(lucide, graduation_cap), 36, CIVUE_WHITE);
      }
    }

    switch (s->hub.step) {
      case HUB_JOIN_EMAIL:
        join_email(app);
        break;
      case HUB_JOIN_SCHOOL:
        join_school(app);
        break;
      case HUB_JOIN_CODE:
        join_code(app);
        break;
      case HUB_JOIN_WAITLIST:
        join_waitlist(app);
        break;
      case HUB_JOIN_WAITING:
      case HUB_JOIN_JOINED:
        join_waiting(app);
        break;
    }

    // back to the step before, as the dialog keeps them.
    if (s->hub.step == HUB_JOIN_SCHOOL || s->hub.step == HUB_JOIN_CODE ||
        s->hub.step == HUB_JOIN_WAITLIST) {
      div(flex, flex_shrink_0, mt(2)) {
        if (quiet_button(k, id("hub-join-back"), "Back") && !s->hub.loading) {
          bool several = s->hub.nschools > 1 &&
                         s->hub.step != HUB_JOIN_SCHOOL;

          s->hub.step = several ? HUB_JOIN_SCHOOL : HUB_JOIN_EMAIL;
          str_set(&s->hub.error, nullptr);
        }
      }
    }
  }

  if (close || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->hub_dialog = HUB_DIALOG_NONE;
  }
}

void hub_dialogs(App *app) {
  switch (app->hub_dialog) {
    case HUB_DIALOG_NONE:
      break;
    case HUB_DIALOG_ADD:
      add_dialog(app);
      break;
    case HUB_DIALOG_EDIT:
      edit_dialog(app);
      break;
    case HUB_DIALOG_REMOVE:
      remove_dialog(app);
      break;
    case HUB_DIALOG_LEAVE:
      leave_dialog(app);
      break;
    case HUB_DIALOG_JOIN:
      join_dialog(app);
      break;
    case HUB_DIALOG_NICK:
      nick_dialog(app);
      break;
  }

}
