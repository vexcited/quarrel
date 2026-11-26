#include "components.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "platform/platform.h"
#include "widgets.h"

// the client's metrics: its picker, the gutter of its grids, the width a
// column would like and the height of a category.
constexpr float PICKER_WIDTH = 498;
constexpr float PICKER_HEIGHT = 486;
constexpr float GUTTER = 12;
constexpr float PAD = 16;
constexpr float COLUMN_WIDTH = 200;
constexpr uint32_t RESULT_COLUMNS = 8;
constexpr uint32_t CATEGORY_COLUMNS = 6;
constexpr float CATEGORY_HEIGHT = 110;
constexpr float FOOTER = 40;
// the client waits this long after the last key before it searches.
constexpr double SEARCH_REST = 0.25;
// built this far out of view, so what scrolls in is ready.
constexpr float AHEAD = 240;
constexpr double PLAYER_KEEP = 1;
constexpr uint32_t PLAYERS_MAX = 24;
constexpr uint32_t STAR_COLOR = 0xf0b232;

#define PROVIDER_KEY "gif_provider"

typedef enum : uint8_t {
  PAGE_FRONT,
  PAGE_SEARCH,
  PAGE_TRENDING,
  PAGE_CATEGORY,
  PAGE_FAVORITES,
} Page;

/* ========================================================================== */
/* players, one per gif in view, muted and looping.                           */
/* ========================================================================== */

typedef struct {
  char *url;
  civue_video *video;
  double used_at;
} Player;

struct GifPlayers {
  CIVUE_VEC(Player) v;
};

static void player_free(Player *p) {
  civue_video_free(p->video);
  free(p->url);
}

// the player of `url`, null where the system plays nothing or too many play.
static civue_video *player_of(App *app, const char *url) {
  civue_ui *ui = civue_ui_current();

  if (app->gif_players == nullptr) {
    app->gif_players = calloc(1, sizeof *app->gif_players);
  }

  struct GifPlayers *ps = app->gif_players;

  for (uint32_t i = 0; i < ps->v.len; i++) {
    if (strcmp(ps->v.v[i].url, url) == 0) {
      ps->v.v[i].used_at = ui->now;
      return ps->v.v[i].video;
    }
  }

  if (ps->v.len >= PLAYERS_MAX) {
    return nullptr;
  }

  civue_video *video = civue_video_new(
    url, &(civue_video_options){.autoplay = true, .loop = true, .muted = true});

  if (video == nullptr) {
    return nullptr;
  }

  if (ps->v.len == ps->v.cap) {
    ps->v.cap = ps->v.cap ? ps->v.cap * 2 : 16;
    ps->v.v = realloc(ps->v.v, ps->v.cap * sizeof *ps->v.v);
  }

  ps->v.v[ps->v.len++] =
    (Player){.url = strdup(url), .video = video, .used_at = ui->now};
  return video;
}

void gif_players_sweep(App *app) {
  civue_ui *ui = civue_ui_current();
  struct GifPlayers *ps = app->gif_players;

  if (ps == nullptr || ui->rerun) {
    return;
  }

  for (uint32_t i = 0; i < ps->v.len;) {
    if (ui->now - ps->v.v[i].used_at > PLAYER_KEEP) {
      player_free(&ps->v.v[i]);
      ps->v.v[i] = ps->v.v[--ps->v.len];
    }
    else {
      i++;
    }
  }
}

void gif_players_free(App *app) {
  struct GifPlayers *ps = app->gif_players;

  if (ps == nullptr) {
    return;
  }

  for (uint32_t i = 0; i < ps->v.len; i++) {
    player_free(&ps->v.v[i]);
  }

  free(ps->v.v);
  free(ps);
  app->gif_players = nullptr;
}

static bool is_mp4(const char *url) {
  size_t len = url ? strcspn(url, "?#") : 0;

  return len >= 4 && strncmp(url + len - 4, ".mp4", 4) == 0;
}

// a gif over the open node: its video once it plays, its still until then.
static void gif_picture(App *app, const char *still, const char *video,
                        float radius) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  civue_video *v = video ? player_of(app, video) : nullptr;
  civue_video_info info = v ? civue_video_frame(ui, v) : (civue_video_info){};

  // gifs only move while the window has the user's attention.
  if (v && info.state == CIVUE_VIDEO_READY &&
      info.playing != ui->window_focused) {
    if (ui->window_focused) {
      civue_video_play(v);
    }
    else {
      civue_video_pause(v);
    }
  }

  if (info.texture != 0) {
    civue_image(ui, info.texture, r, (float[4]){0, 0, 1, 1}, 1, radius, false);
  }
  else if (still) {
    image_cover(app, still, radius);
  }
}

/* ========================================================================== */
/* the grid: each item in the shortest column, as the client lays them.       */
/* ========================================================================== */

typedef struct {
  float x, y, w, h;
} Spot;

typedef struct {
  civue_id id;
  // the rows in view, past `AHEAD`.
  float top, bottom;
  float column;
} Grid;

// lays `count` items of `shapes`, height over width, or of `fixed` height
// each, and answers how tall they all are.
static float lay(const float *shapes, float fixed, size_t count, float width,
                 uint32_t most, Spot *out, float *column) {
  uint32_t columns = (uint32_t)fmaxf(floorf(width / COLUMN_WIDTH), 1);
  float heights[RESULT_COLUMNS] = {};
  float tallest = 0;

  columns = columns < most ? columns : most;
  *column = (width - (float)(columns - 1) * GUTTER) / (float)columns;

  for (size_t i = 0; i < count; i++) {
    uint32_t at = 0;

    for (uint32_t c = 1; c < columns; c++) {
      at = heights[c] < heights[at] ? c : at;
    }

    float h = fixed > 0 ? fixed : roundf(*column * shapes[i]);

    out[i] = (Spot){
      .x = (float)at * (*column + GUTTER),
      .y = heights[at],
      .w = *column,
      .h = h,
    };
    heights[at] += h + GUTTER;
    tallest = fmaxf(tallest, heights[at] - GUTTER);
  }

  return tallest;
}

static bool in_view(const Grid *g, Spot s) {
  return s.y + s.h >= g->top && s.y <= g->bottom;
}

// the scrolled list a grid goes in, whose width it takes.
static Grid grid_of(civue_id id) {
  civue_ui *ui = civue_ui_current();
  float top = civue_scroll_y(ui, id);
  float view = (float)last_size(id).y;

  return (Grid){
    .id = id,
    .top = top - AHEAD,
    .bottom = top + (view > 0 ? view : PICKER_HEIGHT) + AHEAD,
  };
}

static float grid_width(civue_id id) {
  float width = (float)last_size(id).x - 2 * PAD;

  return width > 0 ? width : PICKER_WIDTH - 2 * PAD;
}

/* ========================================================================== */
/* sending.                                                                   */
/* ========================================================================== */

// the client keeps urls without their scheme in places.
static void full_url(char *out, size_t cap, const char *url) {
  snprintf(out, cap, "%s%s", strncmp(url, "//", 2) == 0 ? "https:" : "", url);
}

static void close_picker(App *app) {
  app->popover = POPOVER_NONE;
  app->focus_composer = true;
}

// a press sends, one with shift puts the link in the composer instead.
static void choose(App *app, const Channel *c, const char *url) {
  civue_ui *ui = civue_ui_current();
  char link[1024];

  full_url(link, sizeof link, url);

  if (ui->input.mods & CIVUE_MOD_SHIFT) {
    composer_append(app, link);
  }
  else {
    session_send(app->session, c->id, link, app->reply_to, app->reply_ping);
    app->reply_to = 0;
    app->stick = true;
  }

  close_picker(app);
}

// the gif as a favorite, the way the client keeps it: an image it shows
// rather than a video, under the link sent.
static FavoriteGif favorite_of(const Gif *gif) {
  const char *src = gif->image ? gif->image : gif->video;

  return (FavoriteGif){
    .url = gif->url,
    .src = (char *)src,
    .format = gif->image ? GIF_FORMAT_IMAGE : GIF_FORMAT_VIDEO,
    .width = gif->width,
    .height = gif->height,
  };
}

/* ========================================================================== */
/* tiles.                                                                     */
/* ========================================================================== */

// the star over a gif under the pointer, true when pressed.
static bool star(App *app, civue_id id, bool starred, bool shown) {
  const Look *k = &app->look;
  bool pressed = false;

  if (!shown && !starred) {
    return false;
  }

  div(key(id), absolute, right(px(4)), top(px(4)), flex, items_center,
      justify_center, p(px(6)), rounded(px(5)), bg(k->surface_high),
      cursor_pointer) {
    civue_button_state state = button();
    civue_color color = starred || state.response.hovered ? rgb(STAR_COLOR)
                                                           : k->text;

    pressed = state.activated;

    icon(lucide, star, size(5), text_color(color));

    hint(app, id, &state.response,
         starred ? "Remove from Favorites" : "Add to Favorites",
         CIVUE_TOOLTIP_ABOVE);
  }

  return pressed;
}

typedef struct {
  bool chosen, starred;
} TilePress;

// a gif of a grid, its star while the pointer is over it.
static TilePress gif_tile(App *app, civue_id id, Spot s, const char *still,
                          const char *video, bool starred) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  TilePress press = {};

  div(key(id), absolute, left(px(s.x + PAD)), top(px(s.y)), w(px(s.w)),
      h(px(s.h)), rounded(px(5)), bg(k->surface), overflow_hidden,
      cursor_pointer) {
    civue_button_state state = button();
    bool over = ui->input.has_mouse && civue_rect_contains(here(), ui->input.mouse);

    gif_picture(app, still, video, 5);

    if (over) {
      div(absolute, left(0), top(0), w(full), h(full), rounded(px(5)),
          border(1), border_color(k->border_strong),
          bg(rgba(0xffffff0f))) {}
    }

    press.starred = star(app, id_in(id, "star"), starred, over);
    press.chosen = state.activated && !press.starred;
  }

  return press;
}

typedef enum : uint8_t {
  TILE_PLAIN,
  TILE_FAVORITES,
  TILE_TRENDING,
} TileKind;

// a tile of the front page: its picture darkened, its name over it.
static bool front_tile(App *app, civue_id id, Spot s, const char *still,
                       const char *name, TileKind kind) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), absolute, left(px(s.x + PAD)), top(px(s.y)), w(px(s.w)),
      h(px(s.h)), rounded(px(5)), bg(k->surface), overflow_hidden,
      cursor_pointer) {
    civue_button_state state = button();
    bool over = state.response.hovered;
    civue_color veil = kind == TILE_FAVORITES ? civue_alpha(k->brand, .8f)
                       : over                 ? rgba(0x000000a8)
                                              : rgba(0x00000080);

    pressed = state.activated;

    if (still) {
      image_cover(app, still, 5);
    }

    div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
        justify_center, gap(1), px(2), rounded(px(5)), bg(veil),
        text_color(CIVUE_WHITE),
        border(over ? 1 : 0), border_color(k->border_strong)) {
      if (kind == TILE_FAVORITES) {
        icon(lucide, star, size(5), flex_shrink_0);
      }
      else if (kind == TILE_TRENDING) {
        icon(lucide, trending_up, size(5), flex_shrink_0);
      }

      text(name, min_w(0), truncate, text_size(px(16)), line_height(px(20)),
           font_weight(semibold));
    }
  }

  return pressed;
}

/* ========================================================================== */
/* pages.                                                                     */
/* ========================================================================== */

static void open_page(App *app, Page page, const char *title) {
  app->gif_page = page;
  free(app->gif_title);
  app->gif_title = title ? strdup(title) : nullptr;
}

static void note(App *app, const char *line) {
  text(line, flex_shrink_0, px(4), py(6), text_center, text_size(px(14)),
       line_height(px(18)), text_color(app->look.faint));
}

static int by_order(const void *a, const void *b) {
  const FavoriteGif *x = *(const FavoriteGif *const *)a;
  const FavoriteGif *y = *(const FavoriteGif *const *)b;

  return x->order < y->order ? 1 : x->order > y->order ? -1 : 0;
}

// the newest favorite, null for none.
static const FavoriteGif *newest_favorite(const Session *s) {
  const FavoriteGif *newest = nullptr;

  for (size_t i = 0; i < s->favorite_gifs.count; i++) {
    const FavoriteGif *f = &s->favorite_gifs.gifs[i];

    newest = newest == nullptr || f->order > newest->order ? f : newest;
  }

  return newest;
}

static void front(App *app, civue_id list) {
  Session *s = app->session;
  Gifs *g = session_gifs(s);
  size_t count = 2 + g->ncategories;
  Spot *spots = malloc(count * sizeof *spots);
  float column = 0;
  float height = lay(nullptr, CATEGORY_HEIGHT, count, grid_width(list),
                     CATEGORY_COLUMNS, spots, &column);
  Grid grid = grid_of(list);
  const FavoriteGif *newest = newest_favorite(s);
  civue_id base = id("gif-front");
  char cover[1024] = "";

  if (newest && newest->format == GIF_FORMAT_IMAGE) {
    full_url(cover, sizeof cover, newest->src);
  }

  gifs_fetch_categories(g);

  div(relative, flex_shrink_0, w(full), h(px(height + PAD))) {
    if (front_tile(app, id_in(base, "favorites"), spots[0],
                   *cover ? cover : nullptr,
                   "Favorites", TILE_FAVORITES)) {
      open_page(app, PAGE_FAVORITES, "Favorites");
    }

    if (front_tile(app, id_in(base, "trending"), spots[1], g->trending_still,
                   "Trending GIFs", TILE_TRENDING)) {
      open_page(app, PAGE_TRENDING, "Trending GIFs");
    }

    for (size_t i = 0; i < g->ncategories; i++) {
      const GifCategory *category = &g->categories[i];

      if (in_view(&grid, spots[2 + i]) &&
          front_tile(app, id_at(base, i), spots[2 + i], category->still,
                     category->name, TILE_PLAIN)) {
        open_page(app, PAGE_CATEGORY, category->name);
      }
    }
  }

  if (g->categories_loading) {
    note(app, "Loading categories...");
  }

  free(spots);
}

static void results(App *app, const Channel *c, civue_id list,
                    const char *query) {
  const Look *k = &app->look;
  Session *s = app->session;
  Gifs *g = session_gifs(s);
  bool answered = g->query != nullptr && strcmp(g->query, query) == 0;

  if (!answered) {
    gifs_search(g, query);
  }

  if (g->loading) {
    note(app, "Looking for GIFs...");
    return;
  }

  if (g->error != nullptr) {
    note(app, g->error);

    div(key(id("gif-retry")), flex_shrink_0, self_center, px(4), py(2),
        rounded(lg), bg(k->brand), hover(bg(k->brand_hover)),
        text_size(px(14)), font_weight(medium), text_color(CIVUE_WHITE),
        cursor_pointer) {
      if (button().activated) {
        gifs_search(g, query);
      }

      text("Try Again");
    }

    return;
  }

  if (g->nresults == 0) {
    note(app, "No GIFs found.");
    return;
  }

  float *shapes = malloc(g->nresults * sizeof *shapes);
  Spot *spots = malloc(g->nresults * sizeof *spots);
  float column = 0;

  for (size_t i = 0; i < g->nresults; i++) {
    shapes[i] = (float)g->results[i].height / (float)g->results[i].width;
  }

  float height = lay(shapes, 0, g->nresults, grid_width(list), RESULT_COLUMNS,
                     spots, &column);
  Grid grid = grid_of(list);
  civue_id base = id("gif-result");
  const Gif *chosen = nullptr, *starred = nullptr;

  div(relative, flex_shrink_0, w(full), h(px(height + PAD))) {
    for (size_t i = 0; i < g->nresults; i++) {
      const Gif *gif = &g->results[i];

      if (!in_view(&grid, spots[i])) {
        continue;
      }

      TilePress press =
        gif_tile(app, id_at(base, i), spots[i], gif->still, gif->video,
                 session_favorite_gif(s, gif->url) != nullptr);

      chosen = press.chosen ? gif : chosen;
      starred = press.starred ? gif : starred;
    }
  }

  if (starred) {
    FavoriteGif favorite = favorite_of(starred);

    session_set_favorite_gif(
      s, &favorite, session_favorite_gif(s, starred->url) == nullptr);
  }

  if (chosen) {
    gifs_picked(g, chosen, query);
    choose(app, c, chosen->url);
  }

  free(shapes);
  free(spots);
}

static void favorites(App *app, const Channel *c, civue_id list) {
  Session *s = app->session;
  size_t count = s->favorite_gifs.count;

  session_fetch_frecency(s);

  if (!s->favorite_gifs.present) {
    note(app, "Loading your favorites...");
    return;
  }

  if (count == 0) {
    note(app, "Star a GIF to keep it here.");
    return;
  }

  const FavoriteGif **sorted = malloc(count * sizeof *sorted);
  float *shapes = malloc(count * sizeof *shapes);
  Spot *spots = malloc(count * sizeof *spots);
  float column = 0;

  for (size_t i = 0; i < count; i++) {
    sorted[i] = &s->favorite_gifs.gifs[i];
  }

  qsort(sorted, count, sizeof *sorted, by_order);

  for (size_t i = 0; i < count; i++) {
    const FavoriteGif *f = sorted[i];

    shapes[i] = f->width && f->height ? (float)f->height / (float)f->width : 1;
  }

  float height = lay(shapes, 0, count, grid_width(list), RESULT_COLUMNS, spots,
                     &column);
  Grid grid = grid_of(list);
  civue_id base = id("gif-favorite");
  const FavoriteGif *chosen = nullptr, *unstarred = nullptr;

  div(relative, flex_shrink_0, w(full), h(px(height + PAD))) {
    for (size_t i = 0; i < count; i++) {
      const FavoriteGif *f = sorted[i];
      char src[1024];

      if (!in_view(&grid, spots[i])) {
        continue;
      }

      full_url(src, sizeof src, f->src);

      bool video = f->format == GIF_FORMAT_VIDEO || is_mp4(src);
      TilePress press =
        gif_tile(app, id_in(base, f->url), spots[i], video ? nullptr : src,
                 video ? src : nullptr, true);

      chosen = press.chosen ? f : chosen;
      unstarred = press.starred ? f : unstarred;
    }
  }

  // copied first: the favorite goes away as its star does.
  char url[1024] = "";

  if (chosen) {
    snprintf(url, sizeof url, "%s", chosen->url);
  }

  if (unstarred) {
    FavoriteGif gone = *unstarred;

    session_set_favorite_gif(s, &gone, false);
  }

  if (*url) {
    choose(app, c, url);
  }

  free(sorted);
  free(shapes);
  free(spots);
}

/* ========================================================================== */
/* the picker.                                                                */
/* ========================================================================== */

static GifProvider saved_provider(void) {
  char *name = platform_pref_get(PROVIDER_KEY);
  GifProvider provider = GIF_PROVIDER_DISCORD;

  for (int i = 0; name && i < GIF_PROVIDERS; i++) {
    if (strcmp(name, gif_provider_name((GifProvider)i)) == 0) {
      provider = (GifProvider)i;
    }
  }

  free(name);
  return provider;
}

void gif_picker_toggle(App *app, civue_rect anchor) {
  if (app->popover == POPOVER_GIFS) {
    close_picker(app);
    return;
  }

  app->popover = POPOVER_GIFS;
  app->gif_anchor = anchor;
  app->gif_focus = true;
  open_page(app, PAGE_FRONT, nullptr);
  free(app->gif_search);
  app->gif_search = nullptr;
  gifs_set_provider(session_gifs(app->session), saved_provider());
}

static bool blank(const char *s) {
  for (; s && *s; s++) {
    if (*s != ' ' && *s != '\t' && *s != '\n') {
      return false;
    }
  }

  return true;
}

void gif_picker_search(App *app, civue_rect anchor, const char *query) {
  if (app->popover != POPOVER_GIFS) {
    gif_picker_toggle(app, anchor);
  }

  free(app->gif_search);
  app->gif_search = strdup(query);
  app->gif_typed_at = 0;
  open_page(app, blank(query) ? PAGE_FRONT : PAGE_SEARCH, nullptr);
}

// the search, or the way back and the title of a page of results.
static void header(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Gifs *g = session_gifs(app->session);

  div(flex, items_center, gap(2), flex_shrink_0, h(px(30 + 12)), px(4),
      pb(3), border_b(1), border_color(k->border)) {
    if (app->gif_page != PAGE_FRONT && app->gif_page != PAGE_SEARCH) {
      if (icon_button(app, id("gif-back"), CIVUE_ICON(lucide, arrow_left), 28,
                      20, nullptr)) {
        open_page(app, PAGE_FRONT, nullptr);
      }

      text(app->gif_title ? app->gif_title : "", flex_1, min_w(0), truncate,
           text_size(px(16)), line_height(px(20)), font_weight(semibold),
           text_color(k->strong));
      return;
    }

    civue_id search = id("gif-search");
    char placeholder[64];

    snprintf(placeholder, sizeof placeholder, "Search %s",
             gif_provider_name(g->provider));

    div(key(search), flex_1, h(px(30)), text_size(px(14)),
        line_height(px(18))) {
      civue_field_look look = field_look(k);

      look.pad = 8;

      if (app->gif_focus && !ui->rerun) {
        ui->focus = search;
        app->gif_focus = false;
      }

      civue_field_response field =
        text_field(&app->gif_search, placeholder, look);

      if (field.changed) {
        app->gif_typed_at = ui->now;
        app->gif_page = blank(app->gif_search) ? PAGE_FRONT : PAGE_SEARCH;
      }

      // enter searches without waiting.
      if (field.submitted) {
        app->gif_typed_at = 0;
      }
    }
  }
}

// who answers, the client's own or another, as gifProviderSwitcher does.
static void footer(App *app) {
  const Look *k = &app->look;
  Gifs *g = session_gifs(app->session);

  div(flex, items_center, gap(1), flex_shrink_0, h(px(FOOTER)), px(4),
      border_t(1), border_color(k->border), text_size(px(12)),
      line_height(px(16))) {
    text("Powered by", flex_1, text_color(k->faint));

    for (int i = 0; i < GIF_PROVIDERS; i++) {
      bool on = g->provider == (GifProvider)i;
      const char *name = gif_provider_name((GifProvider)i);

      div(key(id_at(id("gif-provider"), i)), px(2), py(1), rounded(md),
          bg(on ? k->selected : CIVUE_CLEAR), hover(bg(k->hover)),
          style(.font_weight = on ? 600.f : 500.f),
          text_color(on ? k->strong : k->subtle), cursor_pointer) {
        if (button().activated && !on) {
          gifs_set_provider(g, (GifProvider)i);
          platform_pref_set(PROVIDER_KEY, name);
        }

        text(name);
      }
    }
  }
}

// what the search holds once the user stopped typing, null until then.
static const char *rested_search(App *app) {
  civue_ui *ui = civue_ui_current();
  double rest = app->gif_typed_at + SEARCH_REST - ui->now;

  if (rest > 0) {
    civue_request_repaint_after(ui, rest);
    return nullptr;
  }

  return app->gif_search;
}

static void body(App *app, const Channel *c) {
  const Look *k = &app->look;
  Gifs *g = session_gifs(app->session);
  const char *query = nullptr;
  char key_name[160];

  switch (app->gif_page) {
    case PAGE_TRENDING:
      query = "";
      break;
    case PAGE_CATEGORY:
      query = app->gif_title ? app->gif_title : "";
      break;
    case PAGE_SEARCH:
      // what was found last stays while the user types.
      query = rested_search(app);
      query = query ? query : g->query;
      break;
    default:
      break;
  }

  // a list of its own for each page and provider, which starts at its top.
  snprintf(key_name, sizeof key_name, "%d:%d:%s", app->gif_page, g->provider,
           query ? query : "");

  civue_id list = id_in(id("gif-list"), key_name);

  div(key(list), flex, flex_col, flex_1, min_h(0), pt(4), overflow_y_scroll,
      scrollbars(&app->thin_bars), text_color(k->text)) {
    switch (app->gif_page) {
      case PAGE_FRONT:
        front(app, list);
        break;

      case PAGE_FAVORITES:
        favorites(app, c, list);
        break;

      default:
        if (query) {
          results(app, c, list, query);
        }
        else {
          note(app, "Looking for GIFs...");
        }

        break;
    }
  }
}

void gif_picker(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();

  if (app->popover != POPOVER_GIFS) {
    return;
  }

  // over the composer, on its right.
  civue_rect room = overlay_room();
  civue_rect anchor = app->gif_anchor;
  float width = fminf(PICKER_WIDTH, (float)(room.x1 - room.x0));
  float height = fminf(PICKER_HEIGHT, (float)(anchor.y0 - 8 - room.y0));
  float left = fmaxf((float)anchor.x1 - width, (float)room.x0);
  civue_rect card = xywh(left, (float)anchor.y0 - 8 - height, width, height);

  overlay() {
    div(key(id("gif-picker")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(width)), h(px(height)), pt(4), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      card = here();

      // a press on the card is not one on what is behind it.
      button();

      header(app);
      body(app, c);
      footer(app);
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(app->gif_button, ui->input.mouse);

  if (outside) {
    app->popover = POPOVER_NONE;
  }
  else if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    close_picker(app);
  }
}
