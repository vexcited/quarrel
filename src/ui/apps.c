#include "explore.h"

#include <inttypes.h>
#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "authorize.h"
#include "components.h"
#include "discord/cdn.h"
#include "forms.h"
#include "widgets.h"

// discord's sizes for its applications.
constexpr float CARD_LEAST = 254;
constexpr float CARD_ICON = 48;
// what a card holds under its picture: the name, a line about it and three
// more.
constexpr float CARD_TEXT = 132;
constexpr float SIMILAR_LEAST = 230;
constexpr float PAGE_ICON = 96;
constexpr float SIDE_WIDTH = 240;
// an application's page keeps its side column from this width on.
constexpr float SIDE_FROM = 820;
// the overview shows this much before it is asked for whole, 12 lines.
constexpr float OVERVIEW_FOLDED = 240;
constexpr uint32_t CAROUSEL_MOST = 12;
constexpr uint32_t SIMILAR_SHOWN = 3;
// the mark that a link was copied stays this long.
constexpr double COPIED_FOR = 2;

// what each category says of itself, as the client words it.
static const struct {
  int32_t id;
  const char *about;
} ABOUT[] = {
  {4, "Watch videos, listen to music, and share moments together."},
  {6, "Jump into solo puzzles, party games, competitive showdowns, and more."},
  {8, "Build, manage, and grow a thriving community."},
  {9, "Gather, spark conversations, and build connections."},
  {10, "Add convenience, organization, and productivity to your hangouts."},
};

// the languages discord's client speaks, the ones an application's page
// lists.
static const struct {
  const char *code, *name;
} LOCALES[] = {
  {"id", "Indonesian"},
  {"da", "Danish"},
  {"de", "German"},
  {"en-GB", "English, UK"},
  {"en-US", "English, US"},
  {"es-ES", "Spanish"},
  {"es-419", "Spanish, LATAM"},
  {"fr", "French"},
  {"hr", "Croatian"},
  {"it", "Italian"},
  {"lt", "Lithuanian"},
  {"hu", "Hungarian"},
  {"nl", "Dutch"},
  {"no", "Norwegian"},
  {"pl", "Polish"},
  {"pt-BR", "Portuguese, Brazilian"},
  {"ro", "Romanian, Romania"},
  {"fi", "Finnish"},
  {"sv-SE", "Swedish"},
  {"vi", "Vietnamese"},
  {"tr", "Turkish"},
  {"cs", "Czech"},
  {"el", "Greek"},
  {"bg", "Bulgarian"},
  {"ru", "Russian"},
  {"uk", "Ukrainian"},
  {"hi", "Hindi"},
  {"th", "Thai"},
  {"zh-CN", "Chinese, China"},
  {"ja", "Japanese"},
  {"zh-TW", "Chinese, Taiwan"},
  {"ko", "Korean"},
};

// what the data an application's flags let it read is called.
static const struct {
  uint64_t flags;
  const char *title, *about;
} INTENTS[] = {
  {APP_GATEWAY_MESSAGE_CONTENT | APP_GATEWAY_MESSAGE_CONTENT_LIMITED,
   "Read Messages",
   "Can access messages in the server and read their contents."},
  {APP_GATEWAY_PRESENCE | APP_GATEWAY_PRESENCE_LIMITED, "Presence",
   "Can see online status of server members and what they are playing, "
   "watching, or listening to."},
  {APP_GATEWAY_GUILD_MEMBERS | APP_GATEWAY_GUILD_MEMBERS_LIMITED, "Members",
   "Can see who's in the server and their roles, nicknames, and permission "
   "changes."},
};

typedef struct ExploreApps {
  // the tab under the banner, APPS_ALL for what discord features, and the
  // page of its list.
  int32_t tab;
  uint32_t page;
  // what the search field holds, the words searched, null for none, the
  // category the search keeps to and the page of what it found.
  char *field, *query;
  int32_t category;
  uint32_t found_page;
  // the applications whose pages opened one from another, the last one
  // showing.
  Snowflake trail[16];
  size_t ntrail;
  // of the page showing: its overview shows whole, the picture its carousel
  // is on, and when its link was copied.
  bool whole;
  uint32_t slide;
  double copied_at;
} ExploreApps;

static ExploreApps *state_of(App *app) {
  if (!app->explore_apps) {
    app->explore_apps = calloc(1, sizeof *app->explore_apps);

    if (!app->explore_apps) {
      cilo_oom(sizeof *app->explore_apps);
    }

    app->explore_apps->page = 1;
    app->explore_apps->found_page = 1;
  }

  return app->explore_apps;
}

void apps_ui_free(App *app) {
  if (app->explore_apps) {
    free(app->explore_apps->field);
    free(app->explore_apps->query);
    free(app->explore_apps);
    app->explore_apps = nullptr;
  }
}

static void open_app(ExploreApps *st, Snowflake id) {
  if (st->ntrail && st->trail[st->ntrail - 1] == id) {
    return;
  }

  if (st->ntrail == countof(st->trail)) {
    memmove(st->trail, st->trail + 1, (st->ntrail - 1) * sizeof *st->trail);
    st->ntrail--;
  }

  st->trail[st->ntrail++] = id;
  st->whole = false;
  st->slide = 0;
  st->copied_at = 0;
  civue_set_scroll_y(civue_ui_current(), id("apps-app-scroll"), 0);
}

static void close_app(ExploreApps *st) {
  if (st->ntrail) {
    st->ntrail--;
  }

  st->whole = false;
  st->slide = 0;
  st->copied_at = 0;
  civue_set_scroll_y(civue_ui_current(), id("apps-app-scroll"), 0);
}

static void end_search(ExploreApps *st) {
  free(st->field);
  free(st->query);
  st->field = nullptr;
  st->query = nullptr;
  st->category = APPS_ALL;
  st->found_page = 1;
}

/* ========================================================================== */
/* pieces.                                                                    */
/* ========================================================================== */

static civue_color mix(civue_color a, civue_color b, float t) {
  return (civue_color){
    a.r + (b.r - a.r) * t,
    a.g + (b.g - a.g) * t,
    a.b + (b.b - a.b) * t,
    a.a + (b.a - a.a) * t,
  };
}

// "15M", "1.5K" or "950", as the client shortens counts.
static void format_short(uint32_t n, char *out, size_t cap) {
  static const struct {
    double unit;
    const char *mark;
  } units[] = {{1e9, "B"}, {1e6, "M"}, {1e3, "K"}};

  for (size_t i = 0; i < countof(units); i++) {
    double of = n / units[i].unit;

    if (of >= 1) {
      // a digit after the dot under ten, none from there on.
      if (of < 9.95) {
        snprintf(out, cap, "%.1f%s", of, units[i].mark);

        char *zero = strstr(out, ".0");

        if (zero) {
          memmove(zero, zero + 2, strlen(zero + 2) + 1);
        }
      }
      else {
        snprintf(out, cap, "%.0f%s", of, units[i].mark);
      }

      return;
    }
  }

  snprintf(out, cap, "%u", n);
}

static void app_icon(App *app, const DirectoryApp *d, float size,
                     float radius) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_application_icon(url, d->id, d->icon, (uint32_t)(size * 2));

  if (*url && image_fill(app, url, radius)) {
    return;
  }

  char acronym[32];

  guild_acronym(*d->name ? d->name : "?", acronym, sizeof acronym);
  acronym[2] = 0;
  fill(here(), k->surface_high, radius);
  text(acronym, text_size(px(size >= 64 ? 28 : 16)), font_weight(medium),
       text_color(k->text), whitespace_nowrap);
}

// the color of its icon fading from a corner, where the client paints a
// gradient of the icon's colors.
static void icon_fade(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  char url[CDN_URL_MAX];
  civue_color from = k->surface_high;

  cdn_application_icon(url, d->id, d->icon, (uint32_t)(CARD_ICON * 2));

  if (*url) {
    civue_image_info info =
      civue_images_get(app->images, ui, url, (uint32_t)(CARD_ICON * ui->scale));

    if (info.color) {
      from = rgba(info.color);
    }
  }

  constexpr int STRIPS = 48;
  civue_color to = mix(from, k->chat, .65f);
  float w = (float)civue_rect_w(r);

  for (int i = 0; i < STRIPS; i++) {
    fill(xywh((float)r.x0 + w * (float)i / STRIPS, (float)r.y0, w / STRIPS + 1,
              (float)civue_rect_h(r)),
         mix(from, to, (float)i / (STRIPS - 1)), 0);
  }
}

// the picture over the open node: the cover of an activity on its card,
// else the banner of its bot, else its icon's color.
static void picture(App *app, Apps *a, const DirectoryApp *d, bool card) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  char url[CDN_URL_MAX] = "";

  // nothing is asked for a picture out of view.
  if (r.y1 < 0 || r.y0 > ui->size.y) {
    return;
  }

  if (card && d->embedded) {
    Snowflake asset = apps_cover(a, d->id);

    if (asset) {
      char digits[21];

      snprintf(digits, sizeof digits, "%" PRIu64, asset);
      cdn_activity_image(url, d->id, digits, 600);
    }
  }

  if (!*url && d->bot_banner) {
    cdn_banner(url, d->bot_id, d->bot_banner, card ? 600 : 1024);
  }

  if (*url && image_cover(app, url, 0)) {
    return;
  }

  icon_fade(app, d);
}

// the icon half over the picture above it, in a frame of the page's color.
static void raised_icon(App *app, const DirectoryApp *d, float size, float left,
                        float under, civue_color frame) {
  div(absolute, left(px(left)), top(px(under - size / 2 - 4)),
      size(px(size + 8)), p(1), rounded(px(size >= 64 ? 20 : 12)), bg(frame)) {
    div(flex, items_center, justify_center, size(full),
        rounded(px(size >= 64 ? 16 : 8)), overflow_hidden) {
      app_icon(app, d, size, size >= 64 ? 16 : 8);
    }
  }
}

static void app_card(App *app, Apps *a, const DirectoryApp *d, civue_id key,
                     float width, bool with_category) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  const char *category =
    with_category && d->ncategories ? d->categories[0].name : nullptr;
  bool promoted = d->flags & APP_PROMOTED;
  float tall = floorf(width * 9 / 16);

  div(key(key), relative, flex, flex_col, min_w(0), h(px(tall + CARD_TEXT)),
      rounded(lg), border(1), border_color(k->border), bg(k->surface),
      hover(border_color(k->border_strong)), overflow_hidden, cursor_pointer) {
    if (button().activated) {
      open_app(st, d->id);
    }

    div(flex_shrink_0, h(px(tall)), bg(k->hover)) {
      picture(app, a, d, true);
    }

    raised_icon(app, d, CARD_ICON, 12, tall, k->surface);

    div(flex, flex_col, flex_1, min_h(0), pt(8), px(4), pb(4)) {
      text(d->name, flex_shrink_0, truncate, text_size(px(14)),
           line_height(px(18)), font_weight(semibold), text_color(k->strong));

      if (category || promoted) {
        div(flex, items_center, gap(1), flex_shrink_0, mt(1), min_w(0),
            text_size(px(12)), line_height(px(16)), font_weight(medium)) {
          if (category) {
            text(category, min_w(0), truncate, text_color(k->text));
          }

          if (category && promoted) {
            text("•", flex_shrink_0, text_color(k->subtle));
          }

          if (promoted) {
            text("Promoted", flex_shrink_0, text_color(k->subtle));
          }
        }
      }

      // three lines of it, two under a line about it.
      div(flex_shrink_0, mt(2), max_h(px(category || promoted ? 36 : 54)),
          overflow_hidden) {
        text(d->summary ? d->summary : "", text_size(px(14)),
             line_height(px(18)), font_weight(medium), text_color(k->text));
      }
    }
  }
}

// the pages of a list: the one before, the ones around, the one after.
// the page asked for, 0 for none.
static uint32_t pager(App *app, civue_id id, uint32_t page, uint32_t pages) {
  const Look *k = &app->look;
  uint32_t asked = 0;
  uint32_t shown[8];
  size_t nshown = 0;

  if (pages > APPS_PAGES_MOST) {
    pages = APPS_PAGES_MOST;
  }

  if (pages < 2) {
    return 0;
  }

  // the first, then the ones around this one. 0 stands for what is skipped.
  uint32_t from = page > 2 ? page - 1 : 1;
  uint32_t to = page + 1 < pages ? page + 1 : pages;

  if (page <= 3) {
    from = 1;
    to = pages < 5 ? pages : 5;
  }
  else {
    shown[nshown++] = 1;
    shown[nshown++] = 0;
  }

  for (uint32_t i = from; i <= to && nshown < countof(shown); i++) {
    shown[nshown++] = i;
  }

  div(key(id), flex, items_center, justify_center, gap(1), flex_shrink_0, py(4),
      text_size(px(14)), line_height(px(18)), font_weight(medium)) {
    bool first = page <= 1, last = page >= pages;

    div(key(id_in(id, "back")), flex, items_center, gap(1), h(8), px(2),
        rounded(lg), text_color(first ? k->faint : k->text),
        hover(bg(first ? CIVUE_CLEAR : k->hover)),
        style(.cursor = first ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
              .has_cursor = true)) {
      if (button().activated && !first) {
        asked = page - 1;
      }

      icon(mingcute, left_fill, size(4), flex_shrink_0);
      text("Back");
    }

    for (size_t i = 0; i < nshown; i++) {
      char digits[12];
      bool on = shown[i] == page;

      if (shown[i] == 0) {
        text("...", px(1), text_color(k->muted));
        continue;
      }

      snprintf(digits, sizeof digits, "%u", shown[i]);

      div(key(id_at(id, shown[i])), flex, items_center, justify_center,
          min_w(8), h(8), px(1), rounded(full),
          style(.bg = on ? k->primary : CIVUE_CLEAR),
          hover(bg(on ? k->primary : k->hover)),
          text_color(on ? k->on_primary : k->text), cursor_pointer) {
        if (button().activated && !on) {
          asked = shown[i];
        }

        text(digits);
      }
    }

    div(key(id_in(id, "next")), flex, items_center, gap(1), h(8), px(2),
        rounded(lg), text_color(last ? k->faint : k->text),
        hover(bg(last ? CIVUE_CLEAR : k->hover)),
        style(.cursor = last ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
              .has_cursor = true)) {
      if (button().activated && !last) {
        asked = page + 1;
      }

      text("Next");
      icon(mingcute, right_fill, size(4), flex_shrink_0);
    }
  }

  return asked;
}

// the applications of a list, as many to a row as fit.
static void app_grid(App *app, Apps *a, civue_id list, const AppList *apps,
                     bool with_category) {
  uint16_t columns = explore_columns(list, CARD_LEAST);
  float width = (explore_width(list) - (float)(columns - 1) * EXPLORE_GUTTER) /
                (float)columns;

  div(key(list), grid, grid_cols(columns), gap(4), w(full)) {
    explore_keep_width(list);

    for (size_t i = 0; i < apps->napps; i++) {
      app_card(app, a, &apps->apps[i], id_at(list, apps->apps[i].id), width,
               with_category);
    }
  }
}

/* ========================================================================== */
/* the header and the tabs.                                                   */
/* ========================================================================== */

static void header(App *app, Apps *a) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  Snowflake open = st->ntrail ? st->trail[st->ntrail - 1] : 0;

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("apps-header"));

    if (open) {
      const AppPage *page = apps_open(a, open);

      if (icon_button(app, id("apps-back"), CIVUE_ICON(mingcute, arrow_left_fill), 32,
                      20, "Back")) {
        close_app(st);
      }

      text(page->loaded ? page->app.name : "", flex_1, min_w(0), truncate,
           font_weight(semibold), text_color(k->strong));
      return;
    }

    if (st->query) {
      char title[160];

      snprintf(title, sizeof title, "Search results for \"%s\"", st->query);

      if (icon_button(app, id("apps-back"), CIVUE_ICON(mingcute, arrow_left_fill), 32,
                      20, "Back")) {
        end_search(st);
      }

      text(title, flex_1, min_w(0), truncate, font_weight(semibold),
           text_color(k->strong));
    }
    else {
      explore_modes(app);
    }

    // discord searches every category from the first page on.
    if (explore_search(app, id("apps-search"), &st->field)) {
      str_set(&st->query, st->field);
      st->category = APPS_ALL;
      st->found_page = 1;
      civue_set_scroll_y(civue_ui_current(), id("apps-found"), 0);
    }
  }
}

static void tabs(App *app, const Apps *a) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);

  div(key(id("apps-tabs")), flex, flex_wrap, flex_shrink_0, mt(5), border_b(2),
      border_color(k->border)) {
    if (explore_tab(app, id("apps-tab-featured"), "Featured",
                    st->tab == APPS_ALL)) {
      st->tab = APPS_ALL;
    }

    for (size_t i = 0; i < a->ncategories; i++) {
      const AppCategory *c = &a->categories[i];

      if (explore_tab(app, id_at(id("apps-tab"), (uint64_t)c->id), c->name,
                      st->tab == c->id)) {
        st->tab = c->id;
        st->page = 1;
      }
    }
  }
}

/* ========================================================================== */
/* what discord features, and the applications of a category.                 */
/* ========================================================================== */

static void featured(App *app, Apps *a) {
  const Look *k = &app->look;

  apps_fetch_collections(a, false);

  explore_banner(k, "FIND THINGS TO", "DO TOGETHER",
                 "Bring the fun to your hangouts with apps for games, music, "
                 "creative tools, and more.");
  tabs(app, a);

  if (a->ncollections) {
    for (size_t i = 0; i < a->ncollections; i++) {
      const AppCollection *c = &a->collections[i];
      civue_id list = id_at(id("apps-collection"), i);
      // the client's rows hold four cards from a wide window on, two from
      // a narrow one, else one.
      float room = explore_width(list);
      uint16_t columns = room >= 820 ? 4 : room >= 420 ? 2 : 1;
      float width =
        (room - (float)(columns - 1) * EXPLORE_GUTTER) / (float)columns;

      if (!c->napps) {
        continue;
      }

      explore_heading(k, c->title);

      div(key(list), grid, grid_cols(columns), gap(4), py(4), w(full)) {
        explore_keep_width(list);

        for (size_t j = 0; j < c->napps; j++) {
          app_card(app, a, &c->apps[j], id_at(list, c->apps[j].id), width,
                   true);
        }
      }
    }
  }
  else if (a->collections_error) {
    if (explore_trouble(app, id("apps-retry"), a->collections_error,
                        !a->offline)) {
      apps_fetch_collections(a, true);
    }
  }
  else {
    div(flex, justify_center, mt(px(56))) {
      explore_spinner(k->subtle, 32);
    }
  }
}

static void category(App *app, Apps *a) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  const AppList *list = &a->list;
  const char *name = apps_category_name(a, st->tab);
  const char *about = "";

  for (size_t i = 0; i < countof(ABOUT); i++) {
    if (ABOUT[i].id == st->tab) {
      about = ABOUT[i].about;
    }
  }

  apps_show(a, nullptr, st->tab, st->page);

  char title[96];
  size_t len = 0;

  // the banner's titles are in capitals.
  for (const char *c = name ? name : ""; *c && len + 1 < sizeof title; c++) {
    title[len++] = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
  }

  title[len] = 0;
  explore_banner(k, title, nullptr, about);
  tabs(app, a);

  div(flex, flex_col, pt(4), w(full)) {
    if (list->napps) {
      app_grid(app, a, id("apps-list"), list, false);
    }

    if (list->loading) {
      div(flex, items_center, justify_center, flex_shrink_0, h(px(120))) {
        explore_spinner(k->subtle, 32);
      }
    }
    else if (list->error) {
      if (explore_trouble(app, id("apps-list-retry"), list->error,
                          !a->offline)) {
        apps_reload(a);
      }
    }
    else if (!list->napps) {
      text("Discord has no app to show here for now.", text_color(k->subtle));
    }

    uint32_t asked = pager(app, id("apps-pager"), st->page, list->pages);

    if (asked) {
      st->page = asked;
      civue_set_scroll_y(civue_ui_current(),
                         id_at(id("apps-scroll"), (uint64_t)st->tab), 0);
    }
  }
}

/* ========================================================================== */
/* a search: what it found, and the categories it may keep to.                */
/* ========================================================================== */

static uint32_t counted(const Apps *a, int32_t id) {
  for (size_t i = 0; i < a->ncounts; i++) {
    if (a->counts[i].id == id) {
      return a->counts[i].count;
    }
  }

  return 0;
}

static void keep_to(ExploreApps *st, int32_t id) {
  st->category = id;
  st->found_page = 1;
  civue_set_scroll_y(civue_ui_current(), id("apps-found"), 0);
}

static void found(App *app, Apps *a) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  const AppList *list = &a->list;

  apps_show(a, st->query, st->category, st->found_page);

  div(flex, flex_1, min_h(0)) {
    div(key(id("apps-found")), flex_1, min_w(0), h(full), overflow_y_scroll,
        scrollbars(&app->bars), px(6), py(6)) {
      div(flex, flex_col, w(full), max_w(px(EXPLORE_PAGE_MAX)), mx(auto)) {
        if (list->napps) {
          app_grid(app, a, id("apps-results"), list, true);
        }

        if (list->loading) {
          div(flex, items_center, justify_center, flex_shrink_0, h(px(120))) {
            explore_spinner(k->subtle, 32);
          }
        }
        else if (list->error) {
          if (explore_trouble(app, id("apps-found-retry"), list->error,
                              !a->offline)) {
            apps_reload(a);
          }
        }
        else if (!list->napps) {
          bool all = st->category == APPS_ALL;
          const char *name = apps_category_name(a, st->category);

          if (explore_nothing(app, id("apps-all"),
                              all    ? nullptr
                              : name ? name
                                     : "")) {
            keep_to(st, APPS_ALL);
          }
        }

        uint32_t asked =
          pager(app, id("apps-found-pager"), st->found_page, list->pages);

        if (asked) {
          st->found_page = asked;
          civue_set_scroll_y(civue_ui_current(), id("apps-found"), 0);
        }
      }
    }

    // every category at once, then the ones the words were found in.
    div(flex, flex_col, gap(px(2)), flex_shrink_0, w(px(EXPLORE_FILTERS_WIDTH)),
        h(full), py(6), pr(4), text_size(px(14)), line_height(px(18)),
        overflow_y_scroll, scrollbars(&app->thin_bars)) {
      if (a->ncounts) {
        if (explore_filter(app, id("apps-count-all"), "All",
                           counted(a, APPS_ALL), st->category == APPS_ALL)) {
          keep_to(st, APPS_ALL);
        }
      }

      for (size_t i = 0; a->ncounts && i < a->ncategories; i++) {
        const AppCategory *c = &a->categories[i];
        uint32_t count = counted(a, c->id);

        if (count &&
            explore_filter(app, id_at(id("apps-count"), (uint64_t)c->id),
                           c->name, count, st->category == c->id)) {
          keep_to(st, c->id);
        }
      }
    }
  }
}

/* ========================================================================== */
/* the page of an application.                                                */
/* ========================================================================== */

static void section(const Look *k, const char *title) {
  text(title, text_size(px(20)), line_height(px(24)), font_weight(semibold),
       text_color(k->strong));
}

static void side_title(const Look *k, const char *title) {
  text(title, text_size(px(12)), line_height(px(16)), font_weight(semibold),
       text_color(k->strong));
}

// the image whole inside the open node, whatever its shape. false until
// it shows.
static bool image_whole(App *app, const char *url) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  float w = (float)civue_rect_w(r), h = (float)civue_rect_h(r);

  if (w <= 0 || h <= 0) {
    return false;
  }

  civue_image_info image = civue_images_get(
    app->images, ui, url, (uint32_t)(fmaxf(w, h) * (float)ui->scale));

  if (image.state != CIVUE_IMAGE_READY || !image.width || !image.height) {
    return false;
  }

  float fit = fminf(w / (float)image.width, h / (float)image.height);
  float iw = (float)image.width * fit, ih = (float)image.height * fit;

  civue_image(
    ui, image.texture,
    xywh((float)r.x0 + (w - iw) / 2, (float)r.y0 + (h - ih) / 2, iw, ih),
    (float[4]){0, 0, 1, 1}, 1, 0, false);
  return true;
}

// the picture its carousel is on, with the ways to the others.
static void carousel(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  size_t count = d->nmedia < CAROUSEL_MOST ? d->nmedia : CAROUSEL_MOST;
  civue_id list = id("apps-carousel");

  if (!count) {
    return;
  }

  if (st->slide >= count) {
    st->slide = 0;
  }

  const AppMedia *m = &d->media[st->slide];
  float tall = floorf(explore_width(list) * 9 / 16);
  char url[CDN_URL_MAX];

  if (m->youtube) {
    snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/hqdefault.jpg",
             m->source);
  }
  else {
    cdn_media(url, m->source, 1280, 720);
  }

  div(key(list), flex, flex_col, gap(2), w(full)) {
    explore_keep_width(list);

    div(
      key(id_in(list, "picture")), relative, flex, items_center, justify_center,
      h(px(tall)), rounded(lg), bg(CIVUE_BLACK), overflow_hidden,
      style(.cursor = m->youtube ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
      // a video plays on youtube's own page.
      if (button().activated && m->youtube) {
        char page[160];

        snprintf(page, sizeof page, "https://www.youtube.com/watch?v=%s",
                 m->source);
        civue_open_url(page);
      }

      if (!image_whole(app, url)) {
        explore_spinner(k->subtle, 32);
      }
      else if (m->youtube) {
        div(flex, items_center, justify_center, size(px(56)), rounded(full),
            bg(rgba(0x000000b3))) {
          glyph(CIVUE_ICON(mingcute, play_fill), 28, CIVUE_WHITE);
        }
      }
    }

    if (count > 1) {
      div(flex, items_center, justify_center, gap(2)) {
        if (icon_button(app, id_in(list, "before"),
                        CIVUE_ICON(mingcute, left_fill), 28, 18, "Previous")) {
          st->slide = (uint32_t)((st->slide + count - 1) % count);
        }

        for (size_t i = 0; i < count; i++) {
          bool on = i == st->slide;

          div(key(id_at(list, i)), flex, items_center, justify_center, size(4),
              cursor_pointer) {
            if (button().activated) {
              st->slide = (uint32_t)i;
            }

            div(size(2), rounded(full), bg(on ? k->strong : k->faint)) {}
          }
        }

        if (icon_button(app, id_in(list, "after"),
                        CIVUE_ICON(mingcute, right_fill), 28, 18, "Next")) {
          st->slide = (uint32_t)((st->slide + 1) % count);
        }
      }
    }
  }
}

static void overview(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);
  civue_id body = id_at(id("apps-overview"), d->id);
  bool long_one = (float)last_size(body).y > OVERVIEW_FOLDED + 1;

  if (!d->detail && !d->summary) {
    return;
  }

  div(flex, flex_col, gap(2)) {
    section(k, "Overview");

    div(overflow_hidden, max_h(px(st->whole ? 1e6f : OVERVIEW_FOLDED))) {
      if (d->detail) {
        markup(app, body, d->detail,
               &(MarkupOptions){.size = 16, .color = k->text, .hint = 600});
      }
      else {
        text(d->summary, key(body), text_color(k->text));
      }
    }

    if (long_one) {
      div(key(id("apps-overview-more")), flex, items_center, gap(4), my(2),
          cursor_pointer) {
        if (button().activated) {
          st->whole = !st->whole;
        }

        div(flex_1, h(px(1)), bg(k->border_strong)) {}

        div(flex, items_center, gap(2), font_weight(semibold),
            text_color(k->subtle)) {
          text(st->whole ? "Show less" : "Show more");

          if (st->whole) {
            icon(mingcute, up_fill, size(px(18)), flex_shrink_0);
          }
          else {
            icon(mingcute, down_fill, size(px(18)), flex_shrink_0);
          }
        }

        div(flex_1, h(px(1)), bg(k->border_strong)) {}
      }
    }
  }
}

static void commands(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;

  if (!d->ncommands) {
    return;
  }

  div(flex, flex_col, gap(3)) {
    section(k, "Popular Slash Commands");

    div(flex, flex_col, gap(4), p(4), rounded(lg), border(1),
        border_color(k->border), bg(k->surface)) {
      for (size_t i = 0; i < d->ncommands; i++) {
        char name[80];

        snprintf(name, sizeof name, "/%s", d->commands[i].name);

        div(flex, items_start, gap(4)) {
          text(name, flex_shrink_0, px(1), py(px(2)), rounded(sm), border(1),
               border_color(k->border), bg(k->code_bg), text_size(px(14)),
               line_height(px(18)), font_weight(semibold),
               text_color(k->strong), whitespace_nowrap);

          if (d->commands[i].description) {
            text(d->commands[i].description, flex_1, min_w(0), pt(px(3)),
                 text_color(k->text));
          }
        }
      }
    }
  }
}

// the permissions it asks a server for when it sets no way of installing
// apart.
static void permissions(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  Permissions asked = d->install.scopes ? d->install.permissions : 0;
  size_t count = 0;
  const AuthorizePermission *all = authorize_permissions(&count);

  if (!asked) {
    return;
  }

  div(flex, flex_col, gap(3)) {
    section(k, "Permissions");

    div(flex, flex_col, gap(2), p(4), rounded(lg), border(1),
        border_color(k->border), bg(k->surface)) {
      text("Required Permissions", mb(1), text_size(px(14)),
           line_height(px(18)), font_weight(semibold), text_color(k->text));

      for (size_t i = 0; i < count; i++) {
        if (asked & all[i].bit) {
          div(flex, items_center, gap(2)) {
            glyph(CIVUE_ICON(mingcute, check_fill), 16, k->positive);
            text(all[i].name, flex_1, min_w(0), truncate, text_color(k->text));
          }
        }
      }
    }
  }
}

static void data_access(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  const civue_icon icons[] = {
    CIVUE_ICON(mingcute, message_2_fill),
    CIVUE_ICON(mingcute, circle_user_fill),
    CIVUE_ICON(mingcute, group_fill),
  };
  bool any = false;

  for (size_t i = 0; i < countof(INTENTS); i++) {
    any |= (d->flags & INTENTS[i].flags) != 0;
  }

  div(flex, flex_col, gap(3)) {
    section(k, "Data Access");

    div(flex, flex_col, p(4), rounded(lg), border(1), border_color(k->border),
        bg(k->surface)) {
      if (any) {
        div(flex, flex_col, gap(4), mb(4), pb(4), border_b(1),
            border_color(k->border)) {
          text("This app can access the following data in the server:",
               text_size(px(14)), line_height(px(18)), font_weight(semibold),
               text_color(k->text));

          for (size_t i = 0; i < countof(INTENTS); i++) {
            if (!(d->flags & INTENTS[i].flags)) {
              continue;
            }

            div(flex, gap(4)) {
              glyph(icons[i], 24, k->text);

              div(flex, flex_col, gap(1), flex_1, min_w(0)) {
                text(INTENTS[i].title, text_size(px(14)), line_height(px(18)),
                     font_weight(semibold), text_color(k->text));
                text(INTENTS[i].about, text_color(k->text));
              }
            }
          }
        }
      }

      div(flex, flex_wrap, items_center, gap(1), text_color(k->text)) {
        text("All apps can access a set of baseline data. Please visit the");

        div(key(id("apps-help")), text_color(k->link),
            hover(text_color(k->primary_hover)), cursor_pointer) {
          // the article the client's link opens.
          if (button().activated) {
            civue_open_url("https://support.discord.com/hc/en-us/articles/"
                           "7933951485975");
          }

          text("Help Center", whitespace_nowrap);
        }

        text("to learn more.");
      }

      if (d->privacy_url) {
        div(flex, flex_wrap, items_center, gap(1), mt(2), text_color(k->text)) {
          text("For more information about this app's privacy practices, see "
               "the developer's");

          div(key(id("apps-privacy")), text_color(k->link),
              hover(text_color(k->primary_hover)), cursor_pointer) {
            if (button().activated) {
              civue_open_url(d->privacy_url);
            }

            text("privacy policy", whitespace_nowrap);
          }

          text(".", ml(px(-4)));
        }
      }
    }
  }
}

static void link_row(App *app, civue_id id, civue_icon glyph_icon,
                     const char *label, const char *url) {
  const Look *k = &app->look;

  div(key(id), flex, items_center, gap(2), text_color(k->subtle),
      hover(text_color(k->strong)), cursor_pointer) {
    if (button().activated) {
      civue_open_url(url);
    }

    civue_node_icon(civue_ui_current(), glyph_icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20)));
    text(label, flex_1, min_w(0), truncate, text_size(px(14)),
         line_height(px(18)), font_weight(medium));
  }
}

// the server where its makers answer, as a card that opens it.
static void support_server(App *app, const AppGuild *g) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX], count[24], members[48];

  explore_count(g->member_count, count, sizeof count);
  snprintf(members, sizeof members, "%s Member%s", count,
           g->member_count == 1 ? "" : "s");

  div(key(id("apps-support")), relative, flex, flex_col, min_h(px(248)),
      rounded(lg), bg(k->surface_high), overflow_hidden,
      style(.cursor = app->session->preview_opening == g->id
                        ? CIVUE_CURSOR_DEFAULT
                        : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (button().activated && app->session->preview_opening != g->id) {
      explore_open_guild(app, g->id, PREVIEW_FROM_APPS);
    }

    div(flex_shrink_0, h(px(108)), bg(k->hover)) {
      cdn_discovery_splash(url, g->id, g->splash, 480);

      if (*url) {
        image_cover(app, url, 0);
      }
    }

    div(absolute, left(px(12)), top(px(108 + 32 - 58)), size(px(58)), p(1),
        rounded(full), bg(k->surface_high)) {
      div(flex, items_center, justify_center, size(full), rounded(full),
          overflow_hidden) {
        cdn_guild_icon(url, g->id, g->icon, 100);

        if (!*url || !image_fill(app, url, 25)) {
          char acronym[32];

          guild_acronym(g->name, acronym, sizeof acronym);
          acronym[3] = 0;
          fill(here(), k->hover, 25);
          text(acronym, text_size(px(14)), font_weight(medium),
               text_color(k->text), whitespace_nowrap);
        }
      }
    }

    div(flex, flex_col, flex_1, min_h(0), pt(px(33)), px(4), pb(4)) {
      div(flex, items_center, gap(1), flex_shrink_0, mb(1), min_w(0)) {
        if (g->verified) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 16, k->positive);
        }
        else if (g->partnered) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 16, k->accent_text);
        }

        text(g->name, min_w(0), truncate, text_size(px(14)),
             line_height(px(18)), font_weight(semibold), text_color(k->strong));
      }

      div(flex_shrink_0, max_h(px(36)), overflow_hidden) {
        text(g->description ? g->description : "", text_size(px(14)),
             line_height(px(18)), text_color(k->text));
      }

      div(flex_1, min_h(4)) {}

      div(flex, items_center, gap(1), flex_shrink_0) {
        glyph(CIVUE_ICON(mingcute, user_2_fill), 14, k->text);
        text(members, text_size(px(12)), line_height(px(16)),
             text_color(k->text));
      }
    }

    if (app->session->preview_opening == g->id) {
      div(absolute, left(0), top(0), size(full), flex, items_center,
          justify_center, bg(rgba(0x00000099))) {
        explore_spinner(CIVUE_WHITE, 32);
      }
    }
  }
}

// what the side of its page tells: how many servers it is in, its
// categories, its languages, its links and its server.
static void information(App *app, const DirectoryApp *d) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);

  if (d->guild_count) {
    char count[24], line[64];

    format_short(d->guild_count, count, sizeof count);
    snprintf(line, sizeof line, "in %s servers", count);

    div(flex, flex_col, gap(3)) {
      side_title(k, "Server Count");

      div(flex, items_center, gap(1)) {
        glyph(CIVUE_ICON(mingcute, server_fill), 16, k->subtle);
        text(line, text_size(px(14)), line_height(px(18)), text_color(k->text));
      }
    }
  }

  if (d->ncategories) {
    div(flex, flex_col, gap(3)) {
      side_title(k, "Categories");

      div(flex, flex_wrap, gap(2)) {
        for (size_t i = 0; i < d->ncategories; i++) {
          const AppCategory *c = &d->categories[i];

          div(key(id_at(id("apps-chip"), (uint64_t)c->id)), flex, items_center,
              justify_center, px(3), py(px(6)), rounded(sm), border(1),
              border_color(k->border_strong), hover(bg(k->hover)),
              cursor_pointer) {
            // the client lists the category from here.
            if (button().activated) {
              end_search(st);
              st->ntrail = 0;
              st->tab = c->id;
              st->page = 1;
            }

            text(c->name, text_size(px(14)), line_height(px(18)),
                 font_weight(semibold), text_color(k->text), whitespace_nowrap);
          }
        }
      }
    }
  }

  if (d->nlocales) {
    char spoken[768] = "";
    size_t len = 0;

    // in the order of the client's own list.
    for (size_t i = 0; i < countof(LOCALES); i++) {
      for (size_t j = 0; j < d->nlocales; j++) {
        if (strcmp(d->locales[j], LOCALES[i].code) == 0 &&
            len + strlen(LOCALES[i].name) + 6 < sizeof spoken) {
          len += (size_t)snprintf(spoken + len, sizeof spoken - len, "%s%s",
                                  len ? " • " : "", LOCALES[i].name);
          break;
        }
      }
    }

    if (len) {
      div(flex, flex_col, gap(3)) {
        side_title(k, "Supported Languages");
        text(spoken, text_size(px(14)), line_height(px(18)),
             text_color(k->text));
      }
    }
  }

  if (d->nlinks || d->terms_url || d->privacy_url) {
    div(flex, flex_col, gap(3)) {
      side_title(k, "Links");

      for (size_t i = 0; i < d->nlinks; i++) {
        link_row(app, id_at(id("apps-link"), i), CIVUE_ICON(mingcute, link_fill),
                 d->links[i].name, d->links[i].url);
      }

      if (d->terms_url) {
        link_row(app, id("apps-terms"), CIVUE_ICON(mingcute, document_2_fill),
                 "Terms of Service", d->terms_url);
      }

      if (d->privacy_url) {
        link_row(app, id("apps-policy"), CIVUE_ICON(mingcute, lock_fill),
                 "Privacy Policy", d->privacy_url);
      }
    }
  }

  if (d->guild.id && d->guild.name && d->guild.discoverable) {
    div(flex, flex_col, gap(3)) {
      side_title(k, "Support Server");
      support_server(app, &d->guild);
    }
  }
}

static void similar_card(App *app, const DirectoryApp *d, civue_id key) {
  const Look *k = &app->look;
  ExploreApps *st = state_of(app);

  div(key(key), flex, flex_col, gap(4), min_w(0), p(4), rounded(lg), border(1),
      border_color(k->border), bg(k->surface),
      hover(border_color(k->border_strong)), cursor_pointer) {
    if (button().activated) {
      open_app(st, d->id);
    }

    div(flex, items_center, gap(4), min_w(0)) {
      div(flex, items_center, justify_center, size(px(CARD_ICON)),
          flex_shrink_0, rounded(lg), overflow_hidden) {
        app_icon(app, d, CARD_ICON, 8);
      }

      div(flex, flex_col, gap(1), flex_1, min_w(0)) {
        text(d->name, truncate, text_size(px(14)), line_height(px(18)),
             font_weight(semibold), text_color(k->strong));

        if (d->ncategories) {
          text(d->categories[0].name, truncate, text_size(px(12)),
               line_height(px(16)), font_weight(medium), text_color(k->muted));
        }
      }
    }

    div(h(px(36)), overflow_hidden) {
      text(d->summary ? d->summary : "", text_size(px(14)), line_height(px(18)),
           font_weight(medium), text_color(k->text));
    }

    if (d->guild_count) {
      char count[24], line[64];

      format_short(d->guild_count, count, sizeof count);
      snprintf(line, sizeof line, "in %s servers", count);

      div(flex, items_center, gap(1)) {
        glyph(CIVUE_ICON(mingcute, server_fill), 16, k->subtle);
        text(line, text_size(px(12)), line_height(px(16)), font_weight(medium),
             text_color(k->text));
      }
    }
  }
}

static void similar(App *app, const AppPage *page) {
  const Look *k = &app->look;
  const AppList *list = &page->similar;
  civue_id grid_id = id("apps-similar");
  size_t count = list->napps < SIMILAR_SHOWN ? list->napps : SIMILAR_SHOWN;

  if (!count && !list->loading) {
    return;
  }

  div(flex, flex_col, gap(4)) {
    div(h(px(1)), my(4), bg(k->border_strong)) {}
    side_title(k, "You Might Also Like");

    if (!count) {
      div(flex, justify_center, py(4)) {
        explore_spinner(k->subtle, 32);
      }
    }
    else {
      uint16_t columns = explore_columns(grid_id, SIMILAR_LEAST);

      if (columns > count) {
        columns = (uint16_t)count;
      }

      div(key(grid_id), grid, grid_cols(columns), gap(4), w(full)) {
        explore_keep_width(grid_id);

        for (size_t i = 0; i < count; i++) {
          similar_card(app, &list->apps[i], id_at(grid_id, list->apps[i].id));
        }
      }
    }
  }
}

// adds the application: its makers' page when they have one, as the client
// opens it, else discord's own asking.
static void install(App *app, const DirectoryApp *d) {
  if (d->install_url) {
    civue_open_url(d->install_url);
    return;
  }

  AuthorizeRequest request = {.client_id = d->id, .name = d->name};
  bool apart = false;

  for (size_t i = 0; i < APP_INSTALLS; i++) {
    request.installs[i] = d->installs[i];
    apart |= d->installs[i].scopes != nullptr;
  }

  // an application that sets no way apart installs in a server.
  if (!apart) {
    request.installs[APP_INSTALL_GUILD] = d->install;
  }

  authorize_open(app, &request);
}

static void app_page(App *app, Apps *a, Snowflake id) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  ExploreApps *st = state_of(app);
  const AppPage *page = apps_open(a, id);
  const DirectoryApp *d = &page->app;
  civue_id scroll = id("apps-app-scroll"), inner = id("apps-app-inner");

  if (!page->loaded) {
    div(flex, flex_col, flex_1, min_h(0), items_center, justify_center, p(6)) {
      if (page->failed) {
        if (explore_trouble(app, id("apps-app-retry"), "Try again later",
                            !a->offline)) {
          apps_reopen(a, id);
        }
      }
      else {
        explore_spinner(k->subtle, 32);
      }
    }

    return;
  }

  float room = (float)last_size(inner).x;
  bool side = room >= SIDE_FROM;
  // the client's banner is 325 wide for 72 tall.
  float tall = floorf(room * 72 / 325);
  bool copied = st->copied_at && ui->now - st->copied_at < COPIED_FOR;

  div(key(scroll), flex_1, min_h(0), overflow_y_scroll,
      scrollbars(&app->bars)) {
    div(key(inner), relative, flex, flex_col, w(full),
        max_w(px(EXPLORE_PAGE_MAX)), mx(auto), min_w(px(320)), pb(8)) {
      div(flex_shrink_0, h(px(tall)), bg(k->hover)) {
        picture(app, a, d, false);
      }

      raised_icon(app, d, PAGE_ICON, 28, tall, k->chat);

      div(flex, items_start, justify_between, gap(6), mt(px(52)), px(8), pt(5),
          pb(4)) {
        div(flex, flex_col, gap(1), flex_1, min_w(0)) {
          text(d->name, truncate, text_size(px(24)), line_height(px(30)),
               font_weight(semibold), text_color(k->strong));

          div(flex, items_center, gap(2), text_size(px(12)),
              line_height(px(16)), text_color(k->muted)) {
            bool first = true;
            const char *marks[] = {
              d->flags & APP_PARTNER ? "Partner" : nullptr,
              d->monetized ? "In-App Purchases" : nullptr,
              d->ads ? "Ad Supported" : nullptr,
            };

            for (size_t i = 0; i < countof(marks); i++) {
              if (!marks[i]) {
                continue;
              }

              if (!first) {
                text("•");
              }

              text(marks[i], whitespace_nowrap);
              first = false;
            }
          }
        }

        div(flex, items_center, gap(2), flex_shrink_0) {
          if (form_button(app, id("apps-add"), "Add App", BUTTON_PRIMARY,
                          !app_installs(d))) {
            install(app, d);
          }

          if (icon_button(app, id("apps-copy"),
                          copied ? CIVUE_ICON(mingcute, check_fill)
                                 : CIVUE_ICON(mingcute, link_fill),
                          40, 20, copied ? "Link Copied!" : "Copy Link")) {
            char url[96];

            // the page of the application in the client.
            int len = snprintf(
              url, sizeof url,
              "https://discord.com/discovery/applications/%" PRIu64, d->id);

            civue_clipboard_set_text(url, (size_t)len);
            st->copied_at = ui->now;
            civue_request_repaint_after(ui, COPIED_FOR);
          }
        }
      }

      div(flex, items_start, gap(12), px(8)) {
        div(flex, flex_col, gap(8), flex_1, min_w(0)) {
          carousel(app, d);
          overview(app, d);
          commands(app, d);
          permissions(app, d);
          data_access(app, d);

          // a narrow page tells below what a wide one tells aside.
          if (!side) {
            div(flex, flex_col, gap(6), pt(6), border_t(1),
                border_color(k->border_strong)) {
              section(k, "Information");
              information(app, d);
            }
          }

          similar(app, page);
        }

        if (side) {
          div(flex, flex_col, gap(8), flex_shrink_0, w(px(SIDE_WIDTH))) {
            information(app, d);
          }
        }
      }
    }
  }
}

/* ========================================================================== */
/* the page.                                                                  */
/* ========================================================================== */

void apps_page(App *app) {
  Apps *a = session_apps(app->session);
  ExploreApps *st = state_of(app);
  Snowflake open = st->ntrail ? st->trail[st->ntrail - 1] : 0;

  apps_fetch_categories(a);

  // a tab discord took away shows what it features.
  if (st->tab != APPS_ALL && a->ncategories &&
      !apps_category_name(a, st->tab)) {
    st->tab = APPS_ALL;
  }

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    header(app, a);

    if (open) {
      app_page(app, a, open);
    }
    else if (st->query) {
      found(app, a);
    }
    else {
      div(key(id_at(id("apps-scroll"), (uint64_t)st->tab)), flex_1, min_h(0),
          overflow_y_scroll, scrollbars(&app->bars), p(6)) {
        div(flex, flex_col, w(full), max_w(px(EXPLORE_PAGE_MAX)), mx(auto),
            min_w(px(320))) {
          if (st->tab == APPS_ALL) {
            featured(app, a);
          }
          else {
            category(app, a);
          }
        }
      }
    }
  }
}
