#include "components.h"

#include <ctype.h>
#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "explore.h"
#include "widgets.h"

// sizes after fluxer's cards: a splash, a round icon cut into it, the
// name, four lines of description and the counts under a line.
constexpr float CARD_HEIGHT = 336;
constexpr float CARD_MIN = 300;
constexpr float SPLASH_HEIGHT = 120;
constexpr float ICON_SIZE = 64;
constexpr float ICON_RING = 4;
constexpr float DESCRIPTION_LINE = 20;
constexpr float SEARCH_WIDTH = 220;
constexpr float LANGUAGES_WIDTH = 300;
// the next results are asked for this close to the end of the list.
constexpr float MORE_AT = 240;

// the client's tabs, as pills over the cards.
static const char *const TABS[DISCOVERY_TABS] = {
  [DISCOVERY_FEATURED] = "Featured",
  [DISCOVERY_GAMING] = "Gaming",
  [DISCOVERY_MUSIC] = "Music",
  [DISCOVERY_ENTERTAINMENT] = "Entertainment",
  [DISCOVERY_TECH] = "Science & Tech",
  [DISCOVERY_EDUCATION] = "Education",
};

// the languages the client offers a search, by the code discord takes,
// and as their speakers write them.
static const struct {
  const char *code, *name, *native;
} LANGUAGES[] = {
  {"", "All languages", nullptr},
  {"bg", "Bulgarian", "Български"},
  {"zh-CN", "Chinese, Simplified", "简体中文"},
  {"zh-TW", "Chinese, Traditional", "繁體中文"},
  {"hr", "Croatian", "Hrvatski"},
  {"cs", "Czech", "Čeština"},
  {"da", "Danish", "Dansk"},
  {"nl", "Dutch", "Nederlands"},
  {"en-US", "English", nullptr},
  {"fi", "Finnish", "Suomi"},
  {"fr", "French", "Français"},
  {"de", "German", "Deutsch"},
  {"el", "Greek", "Ελληνικά"},
  {"hi", "Hindi", "हिन्दी"},
  {"hu", "Hungarian", "Magyar"},
  {"it", "Italian", "Italiano"},
  {"ja", "Japanese", "日本語"},
  {"ko", "Korean", "한국어"},
  {"lt", "Lithuanian", "Lietuvių"},
  {"no", "Norwegian", "Norsk"},
  {"pl", "Polish", "Polski"},
  {"pt-BR", "Portuguese, Brazilian", "Português do Brasil"},
  {"ro", "Romanian", "Română"},
  {"ru", "Russian", "Русский"},
  {"es-ES", "Spanish", "Español"},
  {"es-419", "Spanish, LATAM", "Español, LATAM"},
  {"sv-SE", "Swedish", "Svenska"},
  {"th", "Thai", "ไทย"},
  {"tr", "Turkish", "Türkçe"},
  {"uk", "Ukrainian", "Українська"},
  {"vi", "Vietnamese", "Tiếng Việt"},
};

// what a card shows of a server, discoverable or one of the store's.
typedef struct {
  Snowflake id;
  const char *name, *description, *icon, *splash;
  uint32_t member_count, presence_count;
  bool counted, verified, partnered;
} Card;

static bool blank(const char *s) {
  while (s && *s == ' ') {
    s++;
  }

  return !s || !*s;
}

static void drop(char **text) {
  free(*text);
  *text = nullptr;
}

// whether `words` are in `text`, the case of ascii letters aside.
static bool has_words(const char *text, const char *words) {
  size_t len = strlen(words);

  for (; text && *text; text++) {
    size_t i = 0;

    while (i < len && tolower((unsigned char)text[i]) ==
                        tolower((unsigned char)words[i])) {
      i++;
    }

    if (i == len) {
      return true;
    }
  }

  return len == 0;
}

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
  div(flex, items_center, gap(px(6)), min_w(0)) {
    div(size(2), flex_shrink_0, rounded(full), bg(color)) {}
    text(label, min_w(0), truncate, text_size(px(13)), line_height(px(16)),
         font_weight(medium), text_color(k->subtle));
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
      active(bg(civue_alpha(k->on_primary, .25f))), font_weight(medium),
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

  if (*url && image_fill(app, url, ICON_SIZE / 2)) {
    return;
  }

  char acronym[32];

  guild_acronym(c->name && *c->name ? c->name : "?", acronym, sizeof acronym);

  // a long name's letters are made small enough to stay in.
  size_t letters = strlen(acronym);
  float size = letters > 4 ? 14 : letters > 2 ? 18 : 24;

  fill(here(), k->surface_high, ICON_SIZE / 2);
  text(acronym, text_size(px(size)), font_weight(semibold),
       text_color(k->strong), whitespace_nowrap);
}

static void server_card(App *app, const Card *c) {
  const Look *k = &app->look;
  Session *s = app->session;
  bool opening = s->preview_opening == c->id;
  char url[CDN_URL_MAX], online[48], members[48], count[24];

  div(key(id_at(id("explore-card"), c->id)), relative, flex, flex_col,
      h(px(CARD_HEIGHT)), min_w(0), rounded(xl), border(1),
      border_color(k->border), bg(k->surface),
      hover(border_color(k->border_strong)), overflow_hidden,
      style(.cursor = opening ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (button().activated && !opening) {
      explore_open_guild(app, c->id, PREVIEW_FROM_DISCOVERY);
    }

    div(flex_shrink_0, h(px(SPLASH_HEIGHT)), bg(k->surface_high)) {
      cdn_discovery_splash(url, c->id, c->splash, 1024);

      if (*url) {
        image_cover(app, url, 0);
      }
    }

    // half of the icon hangs over the splash, in a ring of the card's color.
    div(absolute, left(px(16)),
        top(px(SPLASH_HEIGHT - ICON_SIZE / 2 - ICON_RING)),
        size(px(ICON_SIZE + ICON_RING * 2)), p(px(ICON_RING)), rounded(full),
        bg(k->surface)) {
      div(flex, items_center, justify_center, size(full), rounded(full),
          overflow_hidden) {
        card_icon(app, c);
      }
    }

    div(flex, flex_col, flex_1, min_h(0), pt(px(ICON_SIZE / 2 + 12)), px(4)) {
      div(flex, items_center, gap(1), flex_shrink_0, min_w(0)) {
        if (c->verified) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 18, k->positive);
        }
        else if (c->partnered) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 18, k->accent_text);
        }

        text(c->name ? c->name : "", min_w(0), truncate, text_size(px(17)),
             line_height(px(22)), font_weight(bold), text_color(k->strong));
      }

      // four lines of it at most.
      div(flex_shrink_0, max_h(px(DESCRIPTION_LINE * 4)), mt(2),
          overflow_hidden) {
        text(c->description ? c->description : "", text_size(px(14)),
             line_height(px(DESCRIPTION_LINE)), text_color(k->text));
      }
    }

    if (c->counted) {
      explore_count(c->presence_count, count, sizeof count);
      snprintf(online, sizeof online, "%s online", count);
      explore_count(c->member_count, count, sizeof count);
      snprintf(members, sizeof members, "%s member%s", count,
               c->member_count == 1 ? "" : "s");

      div(flex, items_center, gap(4), flex_shrink_0, h(11), px(4), border_t(1),
          border_color(k->border)) {
        count_dot(k, k->online, online);
        count_dot(k, k->offline, members);
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

// cards as many to a row as fit.
static void card_grid(App *app, civue_id list, const DiscoveryList *guilds) {
  div(key(list), grid, grid_cols(explore_columns(list, CARD_MIN)), gap(4),
      w(full)) {
    explore_keep_width(list);

    for (size_t i = 0; i < guilds->nguilds; i++) {
      Card c = card_of(&guilds->guilds[i]);

      server_card(app, &c);
    }
  }
}

/* ========================================================================== */
/* the sidebar's ways around the page.                                        */
/* ========================================================================== */

static void nav_row(App *app, civue_id id, civue_icon glyph_icon,
                    const char *label, ExploreMode mode) {
  const Look *k = &app->look;
  bool on = app->explore_mode == mode;

  div(key(id), flex, items_center, gap(3), flex_shrink_0, h(10), mx(2), px(2),
      rounded(lg), text_size(px(14)), line_height(px(18)),
      text_color(on ? k->strong : k->subtle),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    if (button().activated) {
      app->explore_mode = mode;
    }

    div(flex, items_center, justify_center, size(8), flex_shrink_0) {
      glyph(glyph_icon, 20, on ? k->strong : k->subtle);
    }

    text(label, flex_1, min_w(0), truncate, font_weight(semibold));
  }
}

void explore_nav(App *app) {
  const Look *k = &app->look;

  div(flex, items_center, gap(2), flex_shrink_0, h(12), pl(3), pr(2)) {
    window_handle(id("explore-nav-head"));

    div(flex, items_center, justify_center, size(6), flex_shrink_0, rounded(md),
        bg(civue_alpha(k->primary, .2f))) {
      glyph(CIVUE_ICON(mingcute, compass_fill), 14, k->primary);
    }

    text("Explore", min_w(0), truncate, text_size(px(15)), line_height(px(20)),
         font_weight(semibold), text_color(k->strong));
  }

  div(flex, flex_col, gap(px(2)), flex_shrink_0) {
    nav_row(app, id("explore-nav-servers"), CIVUE_ICON(mingcute, group_3_fill),
            "Servers", EXPLORE_SERVERS);
    nav_row(app, id("explore-nav-apps"), CIVUE_ICON(mingcute, app_window_fill),
            "Apps", EXPLORE_APPS);
    nav_row(app, id("explore-nav-hubs"), CIVUE_ICON(mingcute, mortarboard_fill),
            "Student Hubs", EXPLORE_HUBS);
  }
}

/* ========================================================================== */
/* the header: its pills, its search and the language of a search.            */
/* ========================================================================== */

static void end_search(App *app, Discovery *d) {
  drop(&app->explore_field);
  discovery_search(d, nullptr);

  if (app->popover == POPOVER_EXPLORE_LANGUAGE) {
    app->popover = POPOVER_NONE;
  }
}

ExploreSearch explore_search(App *app, civue_id id, char **field, bool active) {
  const Look *k = &app->look;
  civue_id input = id_in(id, "field");
  bool focused = civue_ui_current()->focus == input;
  ExploreSearch asked = EXPLORE_SEARCH_NONE;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, w(px(SEARCH_WIDTH)),
      h(9), pl(3), pr(1), rounded(lg), bg(k->field), border(1),
      border_color(focused ? k->ring : k->border_strong), text_size(px(14)),
      line_height(px(18))) {
    icon(mingcute, search_fill, size(4), flex_shrink_0, text_color(k->faint));

    div(key(input), flex_1, min_w(0), h(full)) {
      civue_field_look look = {
        .editing = {.text = k->strong},
        .idle = {.text = k->text},
        .hovered = {.text = k->text},
        .placeholder = k->faint,
        .selection = k->selection,
      };

      // discord searches on enter, never as the user types.
      if (text_field(field, "Search", look).submitted) {
        asked = !blank(*field) ? EXPLORE_SEARCH_SUBMIT
                : active       ? EXPLORE_SEARCH_CLEAR
                               : EXPLORE_SEARCH_NONE;
      }
    }

    if ((active || !blank(*field)) &&
        icon_button(app, id_in(id, "clear"), CIVUE_ICON(mingcute, close_fill),
                    26, 14, "Clear")) {
      drop(field);
      asked = EXPLORE_SEARCH_CLEAR;
    }
  }

  return asked;
}

bool explore_pill(App *app, civue_id id, const char *label, int64_t count,
                  bool on) {
  const Look *k = &app->look;
  bool pressed = false;
  char found[24];

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(9), px(3),
      rounded(lg), style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover), text_color(k->strong)),
      text_size(px(14)), line_height(px(18)), font_weight(semibold),
      text_color(on ? k->strong : k->subtle), cursor_pointer) {
    pressed = button().activated && !on;
    text(label, whitespace_nowrap);

    if (count >= 0) {
      explore_count((uint32_t)count, found, sizeof found);
      text(found, whitespace_nowrap, text_size(px(12)), line_height(px(16)),
           font_weight(medium), text_color(k->faint));
    }
  }

  return pressed;
}

void explore_heading(const Look *k, const char *label) {
  text(label, flex_shrink_0, mb(4), text_size(px(18)), line_height(px(22)),
       font_weight(bold), text_color(k->strong));
}

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

// the client's tabs, or the categories a search found its servers in.
static void pills(App *app, Discovery *d) {
  civue_ui *ui = civue_ui_current();

  explore_pills(app, id("explore-pills")) {
    if (d->query && !d->ncounts) {
      explore_pill(app, id("explore-category-all"), "All", -1, true);
    }

    for (size_t i = 0; d->query && i < d->ncounts; i++) {
      const DiscoveryCount *c = &d->counts[i];

      if (explore_pill(
            app, id_at(id("explore-category"), (uint64_t)(c->id + 2)),
            category_name(d, c->id), c->count, d->category == c->id)) {
        discovery_search_category(d, c->id);
        civue_set_scroll_y(ui, id("explore-results"), 0);
      }
    }

    for (uint8_t i = 0; !d->query && i < DISCOVERY_TABS; i++) {
      if (explore_pill(app, id_at(id("explore-tab"), i), TABS[i], -1,
                       app->explore_tab == i)) {
        app->explore_tab = i;
      }
    }
  }
}

// what keeps a search to a language, as fluxer's.
static void language_button(App *app, Discovery *d) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = id("explore-language");
  bool open = app->popover == POPOVER_EXPLORE_LANGUAGE;
  bool kept = *d->language != 0;

  div(key(id), relative, flex, items_center, justify_center, size(9),
      flex_shrink_0, rounded(lg),
      text_color(open || kept ? k->strong : k->subtle),
      style(.bg = open ? k->hover : CIVUE_CLEAR),
      hover(bg(k->hover), text_color(k->strong)), cursor_pointer) {
    civue_button_state state = button();
    civue_rect r = here();

    app->explore_anchor = r;

    if (state.activated) {
      app_toggle(app, POPOVER_EXPLORE_LANGUAGE);
      drop(&app->explore_language_search);

      if (app->popover == POPOVER_EXPLORE_LANGUAGE) {
        ui->focus = id("explore-language-search");
      }
    }

    icon(mingcute, translate_2_fill, size(5));

    if (kept) {
      fill(xywh((float)r.x1 - 10, (float)r.y0 + 5, 6, 6), k->primary, 3);
    }

    if (!open) {
      hint(app, id, &state.response, language_name(d), CIVUE_TOOLTIP_BELOW);
    }
  }
}

static void header(App *app, Discovery *d) {
  const Look *k = &app->look;

  div(flex, items_center, gap(3), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("explore-header"));

    if (app->explore_mode == EXPLORE_HUBS) {
      icon(mingcute, mortarboard_fill, size(5), flex_shrink_0,
           text_color(k->faint));
      text("Student Hubs", flex_1, min_w(0), truncate, font_weight(semibold),
           text_color(k->strong));
    }
    else {
      pills(app, d);

      switch (explore_search(app, id("explore-search"), &app->explore_field,
                             d->query != nullptr)) {
        case EXPLORE_SEARCH_SUBMIT:
          discovery_search(d, app->explore_field);
          civue_set_scroll_y(civue_ui_current(), id("explore-results"), 0);
          break;
        case EXPLORE_SEARCH_CLEAR:
          end_search(app, d);
          break;
        case EXPLORE_SEARCH_NONE:
          break;
      }

      // discord keeps only a search to a language.
      if (d->query) {
        language_button(app, d);
      }
    }

    header_end(app);
  }
}

// the languages under their button, found by either of their names.
static void language_menu(App *app, Discovery *d) {
  if (app->popover != POPOVER_EXPLORE_LANGUAGE) {
    drop(&app->explore_language_search);
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id field = id("explore-language-search");
  civue_rect anchor = app->explore_anchor, room = overlay_room(), card = {};
  float left = fmaxf((float)room.x0 + 8, (float)anchor.x1 - LANGUAGES_WIDTH);
  float top = (float)anchor.y1 + 6;
  float list = fminf(380, (float)room.y1 - top - 72);
  const char *words = app->explore_language_search;
  size_t shown = 0;

  while (words && *words == ' ') {
    words++;
  }

  overlay() {
    div(key(id("explore-languages")), flex, flex_col, left(px(left)),
        top(px(top)), w(px(LANGUAGES_WIDTH)), p(2), rounded(xl), bg(k->popover),
        border(1), border_color(k->border_strong), shadow(lg),
        text_size(px(14)), line_height(px(18)), text_color(k->text)) {
      card = here();

      div(flex, items_center, gap(2), flex_shrink_0, h(9), pl(3), pr(2), mb(2),
          rounded(lg), bg(k->field), border(1),
          border_color(ui->focus == field ? k->ring : k->border_strong)) {
        icon(mingcute, search_fill, size(4), flex_shrink_0,
             text_color(k->faint));

        div(key(field), flex_1, min_w(0), h(full)) {
          civue_field_look look = {
            .editing = {.text = k->strong},
            .idle = {.text = k->text},
            .hovered = {.text = k->text},
            .placeholder = k->faint,
            .selection = k->selection,
          };

          text_field(&app->explore_language_search, "Search", look);
        }
      }

      div(flex, flex_col, gap(px(2)), max_h(px(list)), overflow_y_scroll,
          scrollbars(&app->thin_bars)) {
        for (size_t i = 0; i < countof(LANGUAGES); i++) {
          bool on = strcmp(LANGUAGES[i].code, d->language) == 0;
          char label[96];

          if (words && !has_words(LANGUAGES[i].name, words) &&
              !has_words(LANGUAGES[i].native, words)) {
            continue;
          }

          if (LANGUAGES[i].native) {
            snprintf(label, sizeof label, "%s (%s)", LANGUAGES[i].name,
                     LANGUAGES[i].native);
          }
          else {
            snprintf(label, sizeof label, "%s", LANGUAGES[i].name);
          }

          shown++;

          div(key(id_at(id("explore-language-row"), i)), flex, items_center,
              gap(2), flex_shrink_0, h(9), px(2), rounded(md),
              hover(bg(k->hover)), cursor_pointer) {
            if (button().activated) {
              discovery_search_language(d, LANGUAGES[i].code);
              civue_set_scroll_y(ui, id("explore-results"), 0);
              app->popover = POPOVER_NONE;
            }

            text(label, flex_1, min_w(0), truncate, font_weight(medium),
                 text_color(k->strong));

            if (on) {
              div(flex, items_center, justify_center, size(5), flex_shrink_0,
                  rounded(full), bg(k->primary)) {
                glyph(CIVUE_ICON(mingcute, check_fill), 12, k->on_primary);
              }
            }
          }
        }

        if (!shown) {
          text("No language goes by that name.", px(2), py(2),
               text_color(k->subtle));
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

/* ========================================================================== */
/* the pages.                                                                 */
/* ========================================================================== */

static void listing(App *app, Discovery *d, DiscoveryTab which) {
  const Look *k = &app->look;
  const DiscoveryList *list = &d->tabs[which];

  discovery_fetch(d, which);

  if (list->nguilds) {
    card_grid(app, id_at(id("explore-grid"), which), list);
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
    text("Discord has no server to show here for now.", text_color(k->subtle));
  }
}

// the card that offers a student hub, as discord's.
static void hub_card(App *app, bool member) {
  const Look *k = &app->look;

  div(key(id("explore-hub-join")), flex, flex_col, h(px(CARD_HEIGHT)), min_w(0),
      p(4), rounded(xl), border(1), border_color(k->border), bg(k->surface)) {
    div(flex, items_center, justify_center, size(12), flex_shrink_0,
        rounded(full), bg(k->primary)) {
      glyph(CIVUE_ICON(mingcute, mortarboard_fill), 24, k->on_primary);
    }

    text(member ? "Attend multiple schools?"
                : "Find study groups, clubs, and friends in the Student Hub "
                  "for your school!",
         mt(4), text_size(px(17)), line_height(px(22)), font_weight(bold),
         text_color(k->strong));
    text(member ? "Join the Student Hub for any school you're attending."
                : "Meet classmates from your school, discover communities, "
                  "and share your servers, all in one place.",
         mt(2), text_size(px(14)), line_height(px(DESCRIPTION_LINE)),
         text_color(k->text));

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

  if (member) {
    explore_heading(k, "Your Hubs");
  }

  div(key(list), grid, grid_cols(explore_columns(list, CARD_MIN)), gap(4),
      w(full)) {
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

  div(flex, flex_col, gap(1), mt(6), pt(4), border_t(1),
      border_color(k->border), text_size(px(14)), line_height(px(18)),
      font_weight(medium), text_color(k->subtle)) {
    text("Hubs are not affiliated with or managed by schools.");
    text("Servers listed in the school's Student Hub are run by students. "
         "Users do not need to be a member of a Student Hub to be invited to "
         "join a server listed there.");
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

  div(flex, flex_col, items_center, justify_center, gap(2), min_h(px(400)),
      p(4), rounded(xl), border(1), border_color(k->border)) {
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

  div(key(scroll), flex_1, min_h(0), overflow_y_scroll, scrollbars(&app->bars),
      p(6)) {
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
}

void explore_page(App *app) {
  if (app->explore_mode == EXPLORE_APPS) {
    apps_page(app);
    return;
  }

  Discovery *d = session_discovery(app->session);
  bool hub = app->explore_mode == EXPLORE_HUBS;
  uint8_t which = app->explore_tab < DISCOVERY_TABS ? app->explore_tab : 0;

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    header(app, d);

    if (d->query && !hub) {
      results(app, d);
    }
    else {
      div(key(id_at(id("explore-scroll"), hub ? DISCOVERY_TABS : which)),
          flex_1, min_h(0), overflow_y_scroll, scrollbars(&app->bars), p(6)) {
        div(flex, flex_col, w(full), max_w(px(EXPLORE_PAGE_MAX)), mx(auto),
            min_w(px(320))) {
          if (hub) {
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
