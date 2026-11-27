#include "components.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "explore.h"
#include "widgets.h"

// discord's sizes for its discovery.
constexpr float CARD_HEIGHT = 320;
constexpr float CARD_MIN = 280;
constexpr float SPLASH_HEIGHT = 146;
constexpr float ICON_SIZE = 48;
constexpr float SEARCH_WIDTH = 240;
// the next results are asked for this close to the end of the list.
constexpr float MORE_AT = 240;
// the tab after discord's lists: the hubs the user is in.
constexpr uint8_t EXPLORE_HUBS = DISCOVERY_TABS;

// what each tab says, as the client words it. its banners are in capitals.
static const struct {
  const char *label, *title, *about, *heading;
} TABS[] = {
  [DISCOVERY_FEATURED] = {"Home", "FIND YOUR COMMUNITY ON DISCORD",
                          "From gaming, to music, to learning, there's a "
                          "place for you.",
                          "Featured Servers"},
  [DISCOVERY_GAMING] = {"Gaming", "GAMING",
                        "Get updates, connect with developers, and chat with "
                        "other players.",
                        "Popular Gaming Servers"},
  [DISCOVERY_MUSIC] = {"Music", "MUSIC",
                       "Get updates, connect with artists, and chat with "
                       "other music fans.",
                       "Popular Music Servers"},
  [DISCOVERY_ENTERTAINMENT] = {"Entertainment", "ENTERTAINMENT",
                               "Connect with your favorite creators and their "
                               "fans.",
                               "Popular Entertainment Servers"},
  [DISCOVERY_TECH] = {"Science & Tech", "SCIENCE & TECH",
                      "Connect with developers, aficionados, and technology "
                      "brands.",
                      "Popular Science & Tech Servers"},
  [DISCOVERY_EDUCATION] = {"Education", "EDUCATION",
                           "Connect with students, teachers, and learning "
                           "centers.",
                           "Popular Education Servers"},
  [EXPLORE_HUBS] = {"Student Hubs", "JOIN THE STUDENT HUB FOR YOUR SCHOOL",
                    "Meet classmates from your school, discover communities, "
                    "and share your servers.",
                    "Your Hubs"},
};

// the languages the client offers a search, by the code discord takes.
static const struct {
  const char *code, *name;
} LANGUAGES[] = {
  {"", "All Languages"},
  {"en-US", "English"},
  {"bg", "Bulgarian"},
  {"zh-CN", "Chinese, Simplified"},
  {"zh-TW", "Chinese, Traditional"},
  {"hr", "Croatian"},
  {"cs", "Czech"},
  {"da", "Danish"},
  {"nl", "Dutch"},
  {"fi", "Finnish"},
  {"fr", "French"},
  {"de", "German"},
  {"el", "Greek"},
  {"hi", "Hindi"},
  {"hu", "Hungarian"},
  {"it", "Italian"},
  {"ja", "Japanese"},
  {"ko", "Korean"},
  {"lt", "Lithuanian"},
  {"no", "Norwegian"},
  {"pl", "Polish"},
  {"pt-BR", "Portuguese, Brazilian"},
  {"ro", "Romanian"},
  {"ru", "Russian"},
  {"es-ES", "Spanish"},
  {"es-419", "Spanish, LATAM"},
  {"sv-SE", "Swedish"},
  {"th", "Thai"},
  {"tr", "Turkish"},
  {"uk", "Ukrainian"},
  {"vi", "Vietnamese"},
};

// what a card shows of a server, discoverable or one of the store's.
typedef struct {
  Snowflake id;
  const char *name, *description, *icon, *splash;
  uint32_t member_count, presence_count;
  bool counted, verified, partnered;
} Card;

/* ========================================================================== */
/* pieces.                                                                    */
/* ========================================================================== */

void explore_count(uint32_t n, char *out, size_t cap) {
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

void explore_spinner(civue_color color, float size) {
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

      fill(xywh(x - 2.5f, y - 2.5f, 5, 5), civue_alpha(color, 1 - .85f * fade),
           2.5f);
    }
  }

  civue_request_repaint_after(ui, 1.0 / 30);
}

static void count_dot(const Look *k, civue_color color, const char *label) {
  div(flex, items_center, min_w(0), mr(4)) {
    div(size(2), mr(1), flex_shrink_0, rounded(full), bg(color)) {}
    text(label, min_w(0), truncate, text_size(px(12)), line_height(px(16)),
         text_color(k->text));
  }
}

bool explore_trouble(App *app, civue_id id, const char *why, bool retry) {
  const Look *k = &app->look;
  bool pressed = false;

  div(flex, flex_col, items_center, gap(3), my(6), py(px(72)), px(4),
      rounded(lg), border(1), border_color(k->border)) {
    text("Something went wrong", text_size(px(20)), line_height(px(24)),
         font_weight(semibold), text_color(k->strong));
    text(why, text_center, text_color(k->critical_text));

    if (retry) {
      div(key(id), flex, items_center, justify_center, h(8), px(4), mt(1),
          rounded(lg), bg(k->surface_high), hover(bg(k->hover)),
          text_size(px(14)), font_weight(medium), text_color(k->strong),
          cursor_pointer) {
        pressed = button().activated;
        text("Try Again");
      }
    }
  }

  return pressed;
}

/* ========================================================================== */
/* opening the page and the servers it lists.                                 */
/* ========================================================================== */

void explore_open(App *app) {
  app_select_channel(app, 0);
  app->exploring = true;
}

void explore_frame(App *app) {
  Session *s = app->session;
  Snowflake opening = s->preview_opening;

  // the guild a card asked for opens once the gateway brought it, unless
  // the user went elsewhere since.
  if (opening && app->exploring && store_guild(s->store, opening)) {
    app_select_guild(app, opening);
  }

  if (s->npreviews) {
    session_end_previews(s, app->guild_id);
  }
}

// the client's card: a guild the account is in opens, any other is looked
// at first.
void explore_open_guild(App *app, Snowflake id, PreviewFrom from) {
  Session *s = app->session;

  if (store_guild(s->store, id)) {
    app_select_guild(app, id);
  }
  else {
    session_preview_guild(s, id, from);
  }
}

// a button over the primary color, drawn in what is written on it.
static bool outlined(const Look *k, civue_id id, const char *label,
                     civue_icon *glyph_icon, bool enabled) {
  bool pressed = false;

  div(key(id), flex, items_center, gap(1), flex_shrink_0, h(6), px(2),
      rounded(md), border(1), border_color(k->on_primary),
      hover(bg(civue_alpha(k->on_primary, .15f))),
      active(bg(civue_alpha(k->on_primary, .25f))),
      font_weight(medium),
      style(.cursor = enabled ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    pressed = button().activated && enabled;

    if (glyph_icon) {
      glyph(*glyph_icon, 14, k->on_primary);
    }

    text(label, whitespace_nowrap, max_w(px(260)), truncate);
  }

  return pressed;
}

void preview_bar(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  const Guild *g = app_guild(app);

  if (!g || !session_previewing(s, g->id)) {
    return;
  }

  bool joining = s->preview_joining == g->id;
  civue_icon back = CIVUE_ICON(mingcute, arrow_left_fill);
  char label[160];

  if (joining) {
    snprintf(label, sizeof label, "Joining...");
  }
  else {
    snprintf(label, sizeof label, "Join %s", g->name ? g->name : "Server");
  }

  // it ends before the window's buttons where they are at the end of the
  // title bar, as on windows.
  float end = civue_ui_current()->titlebar_end_inset;

  div(key(id("preview-bar")), flex, items_center, gap(3), flex_shrink_0,
      min_h(9), mb(2), px(2), rounded(lg), bg(k->primary), text_size(px(14)),
      line_height(px(18)), text_color(k->on_primary),
      style(.margin[1] = civue_px(end))) {
    window_handle(id("preview-bar-handle"));

    if (outlined(k, id("preview-back"), "Back", &back, true)) {
      explore_open(app);
    }

    text("You are currently in preview mode. Join this server to start "
         "chatting!",
         flex_1, min_w(0), truncate, text_center);

    if (outlined(k, id("preview-join"), label, nullptr, !joining)) {
      session_join_preview(s, g->id);
    }
  }
}

/* ========================================================================== */
/* cards.                                                                     */
/* ========================================================================== */

static void card_icon(App *app, const Card *c) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_guild_icon(url, c->id, c->icon, (uint32_t)(ICON_SIZE * 2));

  if (*url && image_fill(app, url, ICON_SIZE * .3f)) {
    return;
  }

  char acronym[32];

  guild_acronym(c->name && *c->name ? c->name : "?", acronym, sizeof acronym);

  // a long name's letters are made small enough to stay in.
  size_t letters = strlen(acronym);
  float size = letters > 4 ? 9 : letters > 2 ? 12 : 16;

  fill(here(), k->surface_high, ICON_SIZE * .3f);
  text(acronym, text_size(px(size)), font_weight(medium), text_color(k->text),
       whitespace_nowrap);
}

static void server_card(App *app, const Card *c) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool opening = s->preview_opening == c->id;
  char url[CDN_URL_MAX], online[48], members[48], count[24];

  div(key(id_at(id("explore-card"), c->id)), relative, flex, flex_col,
      h(px(CARD_HEIGHT)), min_w(0), rounded(lg), border(1),
      border_color(k->border), bg(k->surface),
      hover(border_color(k->border_strong)), overflow_hidden,
      style(.cursor = opening ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (button().activated && !opening) {
      explore_open_guild(app, c->id, PREVIEW_FROM_DISCOVERY);
    }

    div(flex_shrink_0, h(px(SPLASH_HEIGHT)), bg(k->hover)) {
      cdn_discovery_splash(url, c->id, c->splash, 600);

      if (*url) {
        image_cover(app, url, 0);
      }
    }

    // the icon sits 26 points under the picture, in a mask of the card's
    // color.
    div(absolute, left(px(12)), top(px(SPLASH_HEIGHT + 26 - ICON_SIZE - 8)),
        size(px(ICON_SIZE + 8)), p(1), rounded(px(18)), bg(k->surface)) {
      div(flex, items_center, justify_center, size(full), rounded(px(14)),
          overflow_hidden) {
        card_icon(app, c);
      }
    }

    div(flex, flex_col, flex_1, min_h(0), pt(px(28)), px(4), pb(4)) {
      div(flex, items_center, gap(1), flex_shrink_0, min_w(0)) {
        if (c->verified) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 16, k->positive);
        }
        else if (c->partnered) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 16, k->accent_text);
        }

        text(c->name ? c->name : "", min_w(0), truncate, text_size(px(16)),
             line_height(px(20)), font_weight(semibold), text_color(k->strong));
      }

      // three lines of it at most.
      div(flex_shrink_0, max_h(px(54)), mt(1), overflow_hidden) {
        text(c->description ? c->description : "", text_size(px(14)),
             line_height(px(18)), text_color(k->text));
      }

      div(flex_1, min_h(4)) {}

      if (c->counted) {
        explore_count(c->presence_count, count, sizeof count);
        snprintf(online, sizeof online, "%s Online", count);
        explore_count(c->member_count, count, sizeof count);
        snprintf(members, sizeof members, "%s Member%s", count,
                 c->member_count == 1 ? "" : "s");

        div(flex, items_center, flex_shrink_0, min_w(0)) {
          count_dot(k, k->online, online);
          count_dot(k, k->offline, members);
        }
      }
    }

    if (opening) {
      div(absolute, left(0), top(0), size(full), flex, items_center,
          justify_center, bg(rgba(0x00000099))) {
        explore_spinner(CIVUE_WHITE, 32);
      }
    }
  }
}

static Card card_of(const DiscoverableGuild *g) {
  return (Card){
    .id = g->id,
    .name = g->name,
    .description = g->description,
    .icon = g->icon,
    .splash = g->splash,
    .member_count = g->member_count,
    .presence_count = g->presence_count,
    .counted = true,
    .verified = g->verified,
    .partnered = g->partnered,
  };
}

float explore_width(civue_id list) {
  float *width = civue_memory(civue_ui_current(), list, sizeof *width, nullptr);

  return *width;
}

uint16_t explore_columns(civue_id list, float least) {
  float width = explore_width(list);

  return (uint16_t)fmaxf(
    1, floorf((width + EXPLORE_GUTTER) / (least + EXPLORE_GUTTER)));
}

void explore_keep_width(civue_id list) {
  civue_ui *ui = civue_ui_current();
  float *width = civue_memory(ui, list, sizeof *width, nullptr);
  float now = (float)civue_rect_w(here());

  // what was laid out by the old width is laid out again.
  if (*width != now) {
    *width = now;
    civue_request_repaint(ui);
  }
}

// cards as many to a row as fit, discord's grid.
static void card_grid(App *app, civue_id list, const DiscoveryList *guilds) {
  div(key(list), grid, grid_cols(explore_columns(list, CARD_MIN)), gap(4),
      py(4), w(full)) {
    explore_keep_width(list);

    for (size_t i = 0; i < guilds->nguilds; i++) {
      Card c = card_of(&guilds->guilds[i]);

      server_card(app, &c);
    }
  }
}

/* ========================================================================== */
/* the header, its tabs and its search.                                       */
/* ========================================================================== */

static void end_search(App *app, Discovery *d) {
  free(app->explore_field);
  app->explore_field = nullptr;
  discovery_search(d, nullptr);

  if (app->popover == POPOVER_EXPLORE_LANGUAGE) {
    app->popover = POPOVER_NONE;
  }
}

bool explore_search(App *app, civue_id id, char **field) {
  const Look *k = &app->look;
  civue_field_look look = field_look(k);
  bool submitted = false;

  look.pad = 10;

  div(key(id), relative, flex_shrink_0, w(px(SEARCH_WIDTH)), h(8),
      text_size(px(14)), line_height(px(18))) {
    submitted = text_field(field, "Search", look).submitted;

    civue_rect r = here();
    civue_icon_centered(
      civue_ui_current(), CIVUE_ICON(mingcute, search_fill),
      xywh((float)r.x1 - 30, (float)r.y0, 24, (float)civue_rect_h(r)), 16,
      k->faint);
  }

  // discord searches on enter, never as the user types, and an empty field
  // searches nothing.
  const char *words = *field;

  while (words && *words == ' ') {
    words++;
  }

  return submitted && words && *words;
}

static bool mode_tab(App *app, civue_id id, const char *label, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, flex_shrink_0, h(8), px(3), rounded(lg),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover)), font_weight(semibold),
      text_color(on ? k->strong : k->subtle), cursor_pointer) {
    pressed = button().activated && !on;
    text(label, whitespace_nowrap);
  }

  return pressed;
}

void explore_modes(App *app) {
  const Look *k = &app->look;

  icon(mingcute, compass_fill, size(5), flex_shrink_0, mr(1), text_color(k->faint));

  if (mode_tab(app, id("explore-mode-servers"), "Servers",
               app->explore_mode == EXPLORE_SERVERS)) {
    app->explore_mode = EXPLORE_SERVERS;
  }

  if (mode_tab(app, id("explore-mode-apps"), "Apps",
               app->explore_mode == EXPLORE_APPS)) {
    app->explore_mode = EXPLORE_APPS;
  }

  div(flex_1, min_w(0)) {}
}

static void header(App *app, Discovery *d) {
  const Look *k = &app->look;

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("explore-header"));

    if (d->query) {
      char title[160];

      snprintf(title, sizeof title, "Search results for \"%s\"", d->query);

      if (icon_button(app, id("explore-back"), CIVUE_ICON(mingcute, arrow_left_fill),
                      32, 20, "Back")) {
        end_search(app, d);
      }

      text(title, flex_1, min_w(0), truncate, font_weight(semibold),
           text_color(k->strong));
    }
    else {
      explore_modes(app);
    }

    // the hubs are no part of what discord searches.
    if ((d->query || app->explore_tab != EXPLORE_HUBS) &&
        explore_search(app, id("explore-search"), &app->explore_field)) {
      discovery_search(d, app->explore_field);
      civue_set_scroll_y(civue_ui_current(), id("explore-results"), 0);
    }

    header_end(app);
  }
}

void explore_banner(const Look *k, const char *title, const char *second,
                    const char *about) {
  div(key(id("explore-banner")), relative, flex, flex_col, justify_center,
      flex_shrink_0, min_h(px(200)), px(8), py(8), rounded(lg),
      overflow_hidden) {
    discovery_gradient(k, here());

    text(title, max_w(px(740)), text_size(px(32)), line_height(px(38)),
         font_weight(extrabold), text_color(k->strong));

    if (second) {
      text(second, max_w(px(740)), text_size(px(32)), line_height(px(38)),
           font_weight(extrabold), text_color(k->strong));
    }

    text(about, max_w(px(600)), mt(3), text_size(px(16)), line_height(px(20)),
         font_weight(medium), text_color(k->strong));
  }
}

static void banner(const Look *k, uint8_t tab) {
  explore_banner(k, TABS[tab].title, nullptr, TABS[tab].about);
}

bool explore_tab(App *app, civue_id id, const char *label, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), relative, flex_shrink_0, mr(8), pb(4), text_size(px(16)),
      line_height(px(20)), font_weight(medium),
      text_color(on ? k->strong : k->subtle),
      hover(text_color(on ? k->strong : k->text)), cursor_pointer) {
    pressed = button().activated;
    text(label, whitespace_nowrap);

    if (on) {
      civue_rect r = here();
      fill(xywh((float)r.x0, (float)r.y1, (float)civue_rect_w(r), 2), k->primary,
           1);
    }
  }

  return pressed;
}

static void tabs(App *app) {
  const Look *k = &app->look;

  div(key(id("explore-tabs")), flex, flex_wrap, flex_shrink_0, mt(5),
      border_b(2), border_color(k->border)) {
    for (uint8_t i = 0; i <= EXPLORE_HUBS; i++) {
      if (explore_tab(app, id_at(id("explore-tab"), i), TABS[i].label,
                      app->explore_tab == i)) {
        app->explore_tab = i;
      }
    }
  }
}

void explore_heading(const Look *k, const char *label) {
  text(label, mt(5), text_size(px(20)), line_height(px(24)),
       font_weight(semibold), text_color(k->strong));
}

/* ========================================================================== */
/* the tabs.                                                                  */
/* ========================================================================== */

static void listing(App *app, Discovery *d, DiscoveryTab which) {
  const Look *k = &app->look;
  const DiscoveryList *list = &d->tabs[which];

  discovery_fetch(d, which);

  banner(k, which);
  tabs(app);
  explore_heading(k, TABS[which].heading);

  if (list->nguilds) {
    card_grid(app, id("explore-grid"), list);
  }
  else if (list->error) {
    if (explore_trouble(app, id("explore-retry"), list->error, !d->offline)) {
      discovery_refresh(d, which);
    }
  }
  else if (list->loading || !list->fetched_at) {
    div(flex, justify_center, mt(px(56))) {
      explore_spinner(k->subtle, 32);
    }
  }
  else {
    text("Discord has no server to show here for now.", mt(4),
         text_color(k->subtle));
  }
}

// the card that offers a student hub, as discord's.
static void hub_card(App *app, bool member) {
  const Look *k = &app->look;

  div(key(id("explore-hub-join")), flex, flex_col, h(px(CARD_HEIGHT)), min_w(0),
      p(4), rounded(lg), border(1), border_color(k->border), bg(k->surface)) {
    div(flex, items_center, justify_center, size(12), flex_shrink_0,
        rounded(full), bg(k->primary)) {
      glyph(CIVUE_ICON(mingcute, mortarboard_fill), 24, k->on_primary);
    }

    text(member ? "Attend multiple schools?"
                : "Find study groups, clubs, and friends in the Student Hub "
                  "for your school!",
         mt(4), text_size(px(18)), line_height(px(22)), font_weight(semibold),
         text_color(k->strong));
    text(member ? "Join the Student Hub for any school you're attending."
                : "Meet classmates from your school, discover communities, "
                  "and share your servers, all in one place.",
         mt(2), text_size(px(14)), line_height(px(18)), text_color(k->text));

    div(flex_1, min_h(4)) {}

    div(key(id("explore-hub-button")), flex, items_center, justify_center,
        flex_shrink_0, h(10), rounded(lg), font_weight(medium),
        text_color(member ? k->strong : k->on_primary),
        bg(member ? k->surface_high : k->primary),
        hover(bg(member ? k->hover : k->primary_hover)), cursor_pointer) {
      if (button().activated) {
        hub_join_open(app);
      }

      text(member ? "Join Another Hub" : "Join Hub");
    }
  }
}

static void hubs(App *app) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  size_t count = 0, member = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);
  civue_id list = id("explore-hubs");

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      member += folders[i].guilds[j]->hub;
    }
  }

  banner(k, EXPLORE_HUBS);
  tabs(app);

  if (member) {
    explore_heading(k, TABS[EXPLORE_HUBS].heading);
  }

  div(key(list), grid, grid_cols(explore_columns(list, CARD_MIN)), gap(4),
      py(4), w(full)) {
    explore_keep_width(list);

    for (size_t i = 0; i < count; i++) {
      for (size_t j = 0; j < folders[i].nguilds; j++) {
        const Guild *g = folders[i].guilds[j];

        if (g->hub) {
          Card c = {
            .id = g->id,
            .name = g->name,
            .description = g->description,
            .icon = g->icon,
          };

          server_card(app, &c);
        }
      }
    }

    hub_card(app, member != 0);
  }

  div(flex, flex_col, gap(1), mt(4), pt(4), border_t(1),
      border_color(k->border), text_size(px(14)), line_height(px(18)),
      font_weight(medium), text_color(k->subtle)) {
    text("Hubs are not affiliated with or managed by schools.");
    text("Servers listed in the school's Student Hub are run by students. "
         "Users do not need to be a member of a Student Hub to be invited to "
         "join a server listed there.");
  }
}

/* ========================================================================== */
/* a search: what it found, and the categories and the language it may keep  */
/* to.                                                                        */
/* ========================================================================== */

static const char *category_name(const Discovery *d, int32_t id) {
  const char *name =
    id == DISCOVERY_ALL ? "All" : discovery_category_name(d, id);

  return name ? name : "";
}

static const char *language_name(const Discovery *d) {
  for (size_t i = 0; i < countof(LANGUAGES); i++) {
    if (strcmp(LANGUAGES[i].code, d->language) == 0) {
      return LANGUAGES[i].name;
    }
  }

  return LANGUAGES[0].name;
}

// the languages, under the button that opened them.
static void language_menu(App *app, Discovery *d) {
  if (app->popover != POPOVER_EXPLORE_LANGUAGE) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect anchor = app->explore_anchor, room = overlay_room(), card = {};
  float width = (float)civue_rect_w(anchor);
  float top = (float)anchor.y1 + 4;
  float height = fminf(360, (float)room.y1 - top);

  overlay() {
    div(key(id("explore-languages")), flex, flex_col, left(px(anchor.x0)),
        top(px(top)), w(px(width)), max_h(px(height)), p(1), rounded(lg),
        bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg), overflow_y_scroll, scrollbars(&app->thin_bars),
        text_size(px(14)), line_height(px(18)), text_color(k->text)) {
      card = here();

      for (size_t i = 0; i < countof(LANGUAGES); i++) {
        bool on = strcmp(LANGUAGES[i].code, d->language) == 0;

        div(key(id_at(id("explore-language"), i)), flex, items_center,
            flex_shrink_0, h(8), px(2), rounded(md),
            style(.bg = on ? k->selected : CIVUE_CLEAR),
            hover(bg(on ? k->selected : k->hover)), cursor_pointer) {
          if (button().activated) {
            discovery_search_language(d, LANGUAGES[i].code);
            civue_set_scroll_y(ui, id("explore-results"), 0);
            app->popover = POPOVER_NONE;
          }

          text(LANGUAGES[i].name, flex_1, min_w(0), truncate,
               text_color(on ? k->strong : k->text));

          if (on) {
            icon(mingcute, check_fill, size(4), flex_shrink_0, text_color(k->strong));
          }
        }
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}

bool explore_filter(App *app, civue_id id, const char *name, uint32_t count,
                    bool on) {
  const Look *k = &app->look;
  bool pressed = false;
  char found[24];

  explore_count(count, found, sizeof found);

  div(key(id), flex, items_center, flex_shrink_0, p(2), rounded(md),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover)), cursor_pointer) {
    pressed = button().activated;
    text(name, flex_1, min_w(0), truncate, font_weight(medium),
         text_color(k->strong));
    text(found, flex_shrink_0, ml(2), text_color(k->muted));
  }

  return pressed;
}

static void filters(App *app, Discovery *d) {
  const Look *k = &app->look;
  bool open = app->popover == POPOVER_EXPLORE_LANGUAGE;

  div(flex, flex_col, flex_shrink_0, w(px(EXPLORE_FILTERS_WIDTH)), h(full),
      py(6), pr(4), text_size(px(14)), line_height(px(18))) {
    div(key(id("explore-language-select")), flex, items_center, gap(2),
        flex_shrink_0, h(9), px(3), mb(4), rounded(lg), bg(k->input), border(1),
        border_color(open ? k->ring : k->border_strong), cursor_pointer) {
      if (button().activated) {
        app_toggle(app, POPOVER_EXPLORE_LANGUAGE);
      }

      app->explore_anchor = here();

      text(language_name(d), flex_1, min_w(0), truncate, text_color(k->text));

      if (open) {
        icon(mingcute, up_fill, size(4), flex_shrink_0, text_color(k->faint));
      }
      else {
        icon(mingcute, down_fill, size(4), flex_shrink_0,
             text_color(k->faint));
      }
    }

    div(flex, flex_col, gap(px(2)), flex_1, min_h(0), overflow_y_scroll,
        scrollbars(&app->thin_bars)) {
      for (size_t i = 0; i < d->ncounts; i++) {
        const DiscoveryCount *c = &d->counts[i];
        bool on = d->category == c->id;

        if (explore_filter(app,
                           id_at(id("explore-category"), (uint64_t)(c->id + 2)),
                           category_name(d, c->id), c->count, on)) {
          discovery_search_category(d, c->id);
          civue_set_scroll_y(civue_ui_current(), id("explore-results"), 0);
        }
      }
    }
  }
}

bool explore_nothing(App *app, civue_id id, const char *category) {
  const Look *k = &app->look;
  bool all = category == nullptr, pressed = false;
  char title[160];

  if (all) {
    snprintf(title, sizeof title, "No results found");
  }
  else {
    snprintf(title, sizeof title, "No results found in %s", category);
  }

  div(flex, flex_col, items_center, justify_center, gap(2), my(4),
      min_h(px(400)), p(4), rounded(lg), bg(k->surface)) {
    text(title, text_center, text_size(px(24)), line_height(px(30)),
         font_weight(semibold), text_color(k->strong));

    if (all) {
      text("Try searching for something else.", text_center,
           text_color(k->text));
    }
    else {
      div(flex, flex_wrap, items_center, justify_center, gap(1),
          text_color(k->text)) {
        text("Try searching for something else or");

        div(key(id), text_color(k->link), hover(text_color(k->primary_hover)),
            cursor_pointer) {
          pressed = button().activated;
          text("see results from all categories", whitespace_nowrap);
        }

        text(".", ml(px(-4)));
      }
    }
  }

  return pressed;
}

static void results(App *app, Discovery *d) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const DiscoveryList *list = &d->results;
  civue_id scroll = id("explore-results"), inner = id("explore-results-inner");

  discovery_fetch_categories(d);

  // the next results once the end of the list comes near, which it is at
  // once when what was found does not fill the page. sizes are the last
  // frame's, so they only count once it drew what is there now.
  size_t *drawn =
    civue_memory(ui, id_in(scroll, "drawn"), sizeof *drawn, nullptr);
  float left = (float)last_size(inner).y - civue_scroll_y(ui, scroll) -
               (float)last_size(scroll).y;

  if (*drawn == list->nguilds && list->nguilds && left < MORE_AT) {
    discovery_search_more(d);
  }

  *drawn = list->nguilds;

  div(flex, flex_1, min_h(0)) {
    div(key(scroll), flex_1, min_w(0), h(full), overflow_y_scroll,
        scrollbars(&app->bars), px(6), py(2)) {
      div(key(inner), flex, flex_col, w(full), max_w(px(EXPLORE_PAGE_MAX)),
          mx(auto)) {
        if (list->nguilds) {
          card_grid(app, id("explore-found"), list);
        }

        if (list->loading) {
          div(flex, items_center, justify_center, flex_shrink_0, h(px(120))) {
            explore_spinner(k->subtle, 32);
          }
        }
        else if (list->error) {
          if (explore_trouble(app, id("explore-search-retry"), list->error,
                              !d->offline && !list->nguilds)) {
            // the same words again, which a failure lets through.
            char *words = str_dup(d->query);

            discovery_search(d, words);
            free(words);
          }
        }
        else if (!list->nguilds) {
          bool all = d->category == DISCOVERY_ALL;

          if (explore_nothing(app, id("explore-all"),
                              all ? nullptr : category_name(d, d->category))) {
            discovery_search_category(d, DISCOVERY_ALL);
          }
        }
      }
    }

    filters(app, d);
  }
}

void explore_page(App *app) {
  if (app->explore_mode == EXPLORE_APPS) {
    apps_page(app);
    return;
  }

  Discovery *d = session_discovery(app->session);
  uint8_t which = app->explore_tab <= EXPLORE_HUBS ? app->explore_tab : 0;

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    header(app, d);

    if (d->query) {
      results(app, d);
    }
    else {
      div(key(id_at(id("explore-scroll"), which)), flex_1, min_h(0),
          overflow_y_scroll, scrollbars(&app->bars), p(6)) {
        div(flex, flex_col, w(full), max_w(px(EXPLORE_PAGE_MAX)), mx(auto),
            min_w(px(320))) {
          if (which == EXPLORE_HUBS) {
            hubs(app);
          }
          else {
            listing(app, d, which);
          }
        }
      }
    }
  }

  language_menu(app, d);
}
