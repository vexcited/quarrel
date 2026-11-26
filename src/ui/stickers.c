#include "components.h"

#include <math.h>
#include <stdcountof.h>
#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/emoji.h"
#include "discord/fakestickers.h"
#include "discord/uploads.h"
#include "http.h"
#include "widgets.h"

// the client's metrics, from the STICKERS_CONSTANTS of its bundle and the
// stylesheets of its expression picker.
constexpr float PICKER_WIDTH = 498;
constexpr float PICKER_HEIGHT = 486;
constexpr float RAIL = 48;
constexpr float RAIL_ICON = 32;
constexpr float RAIL_GAP = 8;
constexpr float LIST_PAD = 8;
constexpr float LIST_BAR = 8;
constexpr float HEADING = 32;
constexpr float INSPECTOR = 48;
// a list no wider than this has its stickers smaller and closer.
constexpr float SMALL_UNDER = 360;
// what a row adds under its stickers.
constexpr float ROW_MARGIN = 12;
// the scroll its rail follows is this far into a section.
constexpr float ACTIVE_OFFSET = 20;
// built this far out of view, so what scrolls in is ready.
constexpr float AHEAD = 220;
constexpr float TRAY_STICKER = 48;
constexpr float MESSAGE_STICKER = 160;
constexpr float INFO_WIDTH = 288;

// the sections that are no guild's, by what stands for their id.
constexpr Snowflake KEY_FAVORITES = 1;
constexpr Snowflake KEY_FREQUENT = 2;
constexpr Snowflake KEY_RESULTS = 3;

/* ========================================================================== */
/* what the picker lists.                                                     */
/* ========================================================================== */

typedef struct {
  const GuildSticker *sticker;
  const Guild *guild;
} Item;

typedef struct {
  Snowflake key;
  const Guild *guild;
  const char *name;
  size_t first, count;
  bool headed, folded;
  // where it starts in the list.
  float top;
} Section;

typedef struct {
  Item *items;
  size_t nitems, items_cap;
  Section *sections;
  size_t nsections, sections_cap;
  uint32_t columns;
  float cell, gap, row, height;
} Listing;

static void listing_free(Listing *l) {
  free(l->items);
  free(l->sections);
  *l = (Listing){};
}

static void push_item(Listing *l, const Guild *guild,
                      const GuildSticker *sticker) {
  if (l->nitems == l->items_cap) {
    l->items_cap = l->items_cap ? l->items_cap * 2 : 64;
    l->items = realloc(l->items, l->items_cap * sizeof *l->items);
  }

  l->items[l->nitems++] = (Item){sticker, guild};
}

static void open_section(App *app, Listing *l, Snowflake key,
                         const Guild *guild, const char *name, bool headed) {
  if (l->nsections == l->sections_cap) {
    l->sections_cap = l->sections_cap ? l->sections_cap * 2 : 16;
    l->sections = realloc(l->sections, l->sections_cap * sizeof *l->sections);
  }

  l->sections[l->nsections++] = (Section){
    .key = key,
    .guild = guild,
    .name = name,
    .first = l->nitems,
    .headed = headed,
    .folded = headed && ids_has(&app->sticker_folded, key),
  };
}

// ends the section opened last, which is dropped when nothing went in it.
static void close_section(Listing *l) {
  Section *s = &l->sections[l->nsections - 1];

  s->count = l->nitems - s->first;

  if (s->count == 0) {
    l->nsections--;
  }
}

static void list_guild(App *app, Listing *l, const Guild *guild) {
  open_section(app, l, guild->id, guild, guild->name ? guild->name : "", true);

  for (size_t i = 0; i < guild->nstickers; i++) {
    push_item(l, guild, &guild->stickers[i]);
  }

  close_section(l);
}

// the favorites, the ones used the most, then every guild's in the order
// of the guilds, the channel's own first. each of them can be sent: the
// ones discord would refuse go as Vencord's FakeNitro sends them.
static void browse(App *app, Listing *l, const Channel *c) {
  Session *s = app->session;
  Store *store = s->store;
  const Guild *here = store_guild(store, c->guild_id);
  size_t nfolders = 0;
  const GuildFolder *folders = store_guild_folders(store, &nfolders);

  open_section(app, l, KEY_FAVORITES, nullptr, "Favorites", true);

  for (size_t i = 0; i < s->stickers.nfavorites; i++) {
    const Guild *guild = nullptr;
    const GuildSticker *sticker =
      store_sticker(store, s->stickers.favorites[i], &guild);

    if (sticker) {
      push_item(l, guild, sticker);
    }
  }

  close_section(l);
  open_section(app, l, KEY_FREQUENT, nullptr, "Frequently Used", true);

  for (size_t i = 0; i < s->nfrecent_stickers; i++) {
    const Guild *guild = nullptr;
    const GuildSticker *sticker =
      store_sticker(store, s->frecent_stickers[i], &guild);

    if (sticker) {
      push_item(l, guild, sticker);
    }
  }

  close_section(l);

  if (here) {
    list_guild(app, l, here);
  }

  for (size_t i = 0; i < nfolders; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      if (folders[i].guilds[j] != here) {
        list_guild(app, l, folders[i].guilds[j]);
      }
    }
  }
}

/* ========================================================================== */
/* the search, scored as the client's queryStickers.                          */
/* ========================================================================== */

// what `value` is worth for the query: all of it, its start, or for a name
// any part of it.
static double field_score(const char *value, const char *query, double weight,
                          bool named) {
  if (!value) {
    return 0;
  }

  while (*value == ' ' || *value == '\t' || *value == '\n') {
    value++;
  }

  size_t len = strlen(value), asked = strlen(query);

  while (len && (value[len - 1] == ' ' || value[len - 1] == '\n')) {
    len--;
  }

  if (len < asked) {
    return 0;
  }

  if (!strncasecmp(value, query, asked)) {
    return (len == asked ? 10 : 7) * weight;
  }

  return named && strcasestr(value, query) ? 5 * weight : 0;
}

static double sticker_score(App *app, const Guild *guild,
                            const GuildSticker *sticker, const char *query,
                            int64_t now) {
  const StickerSettings *known = &app->session->stickers;
  double best = field_score(sticker->name, query, 11, true);

  // the client only looks further at a sticker that has tags.
  if (sticker->tags) {
    const UnicodeEmoji *emoji = emoji_by_name(sticker->tags);

    best = fmax(best, field_score(sticker->tags, query, 1, false));
    best = fmax(best, field_score(guild->name, query, 8, true));
    best =
      fmax(best, field_score(emoji ? emoji->utf8 : nullptr, query, 6, false));
  }

  // one that was used counts for what its last uses weigh, which lowers
  // one used long ago under one never used.
  for (size_t i = 0; best > 0 && i < known->nuses; i++) {
    if (known->uses[i].id == sticker->id) {
      uint64_t used = settings_proto_sticker_score(&known->uses[i], now);

      best = used ? best * (double)used / 100 : best;
    }
  }

  return best;
}

typedef struct {
  Item item;
  double score;
  size_t at;
} Found;

static int by_score(const void *a, const void *b) {
  const Found *x = a, *y = b;

  if (x->score != y->score) {
    return x->score < y->score ? 1 : -1;
  }

  return x->at < y->at ? -1 : x->at > y->at;
}

// what the query finds in every guild.
static void search(App *app, Listing *l, const char *query) {
  Store *store = app->session->store;
  int64_t now = (int64_t)(store_now(store) * 1000);
  size_t nfolders = 0, n = 0, cap = 0;
  const GuildFolder *folders = store_guild_folders(store, &nfolders);
  Found *found = nullptr;

  for (size_t i = 0; i < nfolders; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      const Guild *guild = folders[i].guilds[j];

      for (size_t at = 0; at < guild->nstickers; at++) {
        const GuildSticker *sticker = &guild->stickers[at];
        double score = sticker_score(app, guild, sticker, query, now);

        if (score <= 0) {
          continue;
        }

        if (n == cap) {
          cap = cap ? cap * 2 : 64;
          found = realloc(found, cap * sizeof *found);
        }

        found[n] = (Found){{sticker, guild}, score, n};
        n++;
      }
    }
  }

  if (n) {
    qsort(found, n, sizeof *found, by_score);
  }

  open_section(app, l, KEY_RESULTS, nullptr, nullptr, false);

  for (size_t i = 0; i < n; i++) {
    push_item(l, found[i].item.guild, found[i].item.sticker);
  }

  close_section(l);
  free(found);
}

/* ========================================================================== */
/* the grid, laid as the client's.                                            */
/* ========================================================================== */

static void lay(Listing *l, float width, bool searching) {
  float list = width - LIST_PAD - LIST_BAR;
  bool large = list > SMALL_UNDER;
  float margin = large ? 12 : 4;
  float y = searching ? 8 : 0;

  // a sticker and a point around it.
  l->cell = (large ? 96 : 80) + 2;
  l->columns = (uint32_t)fmaxf(floorf((list + margin) / (l->cell + margin)), 1);
  l->gap = l->columns > 1
             ? floorf(fmaxf(margin, (list - l->cell * (float)l->columns) /
                                      (float)(l->columns - 1)))
             : margin;
  l->row = l->cell + ROW_MARGIN;

  for (size_t i = 0; i < l->nsections; i++) {
    Section *s = &l->sections[i];
    size_t rows = (s->count + l->columns - 1) / l->columns;

    s->top = y;
    y += s->headed ? HEADING : 0;
    y += s->folded ? 0 : (float)rows * l->row;
  }

  l->height = y;
}

static civue_vec2 spot_of(const Listing *l, const Section *s, size_t at) {
  return point(LIST_PAD + (float)(at % l->columns) * (l->cell + l->gap),
               s->top + (s->headed ? HEADING : 0) +
                 (float)(at / l->columns) * l->row);
}

// the sticker pointed at, the first one shown when none is.
static const Item *inspected_of(App *app, const Listing *l, size_t *section,
                                size_t *at) {
  const Section *first = nullptr;

  for (size_t i = 0; i < l->nsections; i++) {
    const Section *s = &l->sections[i];

    if (s->folded) {
      continue;
    }

    first = first ? first : s;

    for (size_t j = 0; s->key == app->sticker_section && j < s->count; j++) {
      if (l->items[s->first + j].sticker->id == app->sticker_inspected) {
        *section = i;
        *at = j;
        return &l->items[s->first + j];
      }
    }
  }

  if (!first) {
    return nullptr;
  }

  *section = (size_t)(first - l->sections);
  *at = 0;
  return &l->items[first->first];
}

static void inspect(App *app, const Section *s, const Item *item) {
  app->sticker_section = s->key;
  app->sticker_inspected = item->sticker->id;
}

/* ========================================================================== */
/* picking.                                                                   */
/* ========================================================================== */

static bool blank(const char *s) {
  for (; s && *s; s++) {
    if (*s != ' ' && *s != '\t' && *s != '\n') {
      return false;
    }
  }

  return true;
}

static void close_picker(App *app) {
  app->popover = POPOVER_NONE;
  app->sticker_menu = 0;
  app->focus_composer = true;
}

Snowflake sticker_pending(App *app, const Channel *c) {
  // it stays while its guild has it, where it was picked.
  if (app->sticker_channel != c->id ||
      !store_sticker(app->session->store, app->sticker_pending, nullptr)) {
    app->sticker_pending = 0;
  }

  return app->sticker_pending;
}

char *sticker_linked(App *app, const Channel *c, Snowflake sticker_id,
                     const char *content) {
  Store *store = app->session->store;
  const Guild *guild = nullptr;
  const GuildSticker *sticker =
    sticker_id ? store_sticker(store, sticker_id, &guild) : nullptr;

  if (!sticker || sticker_native(store, guild, sticker, c)) {
    return nullptr;
  }

  return fake_sticker_content(content, sticker);
}

static void notice(App *app, const char *title, const char *body) {
  snprintf(app->upload_title, sizeof app->upload_title, "%s", title);
  snprintf(app->upload_body, sizeof app->upload_body, "%s", body);
}

typedef struct {
  App *app;
  Snowflake channel_id, sticker_id;
} GifAsked;

// FakeNitro's sendAnimatedSticker: the frames of the animated png as a gif
// named after the sticker, left with the files of the composer.
static void on_gif_source(void *ctx, const HttpResponse *res) {
  GifAsked *asked = ctx;
  App *app = asked->app;
  Session *s = app->session;
  civue_frames frames = {};
  uint8_t *gif = nullptr;
  size_t len = 0;
  char name[32];

  defer free(asked);
  fake_sticker_filename(name, sizeof name, asked->sticker_id);

  if (res->status == 200 && civue_frames_decode(res->body, res->body_len,
                                                FAKE_STICKER_SIZE, &frames)) {
    gif = fake_sticker_gif(frames.rgba, frames.delays, frames.width,
                           frames.height, frames.count, &len);
    civue_frames_free(&frames);
  }

  defer free(gif);

  // the channel may be another account's by now.
  if (!s || !store_channel(s->store, asked->channel_id)) {
    return;
  }

  if (!gif || !uploads_add_bytes(s, asked->channel_id, gif, len, name)) {
    notice(app, "Sticker Not Sent",
           "This animated sticker could not be turned into a GIF.");
  }

  civue_window_wake();
}

static void send_as_gif(App *app, const Channel *c, Snowflake sticker_id) {
  GifAsked *asked = malloc(sizeof *asked);
  char url[CDN_URL_MAX];

  if (!asked) {
    return;
  }

  *asked = (GifAsked){app, c->id, sticker_id};
  fake_sticker_source(url, sizeof url, sticker_id);
  http_send(&(HttpRequest){.url = url, .max_response_len = 16u << 20},
            on_gif_source, asked);
}

// sends the sticker alone, or keeps it for the message being written, as
// the client does once something is typed or attached. one discord would
// refuse goes the way Vencord's FakeNitro sends it.
static void pick(App *app, Channel *c, const Item *item) {
  Session *s = app->session;
  Store *store = s->store;
  Snowflake id = item->sticker->id;
  bool native = sticker_native(store, item->guild, item->sticker, c);
  Upload *files[1];

  close_picker(app);

  // an animated png is no picture a link would show moving.
  if (!native && item->sticker->format == STICKER_APNG) {
    if (c->guild_id && !store_can(store, c, PERM_ATTACH_FILES)) {
      // the plugin's own words.
      notice(app, "Hold on!",
             "You cannot send this message because it contains an animated "
             "FakeNitro sticker, and you do not have permissions to attach "
             "files in the current channel.");
    }
    else {
      send_as_gif(app, c, id);
    }

    return;
  }

  bool waits = !blank(app->draft) || uploads_of(s, c->id, 0, files, 1) != 0 ||
               sticker_pending(app, c);
  // a link shows no picture where the user may not embed any, which the
  // composer asks about before it sends.
  bool asks = !native && c->guild_id && !store_can(store, c, PERM_EMBED_LINKS);

  if (waits || asks) {
    app->sticker_pending = id;
    app->sticker_channel = c->id;
    app->fake_ask = asks && !waits;
    return;
  }

  char *linked = sticker_linked(app, c, id, "");

  session_send_stickers(s, c->id, linked ? linked : "", &id, linked ? 0 : 1,
                        app->reply_to, app->reply_ping);

  if (linked) {
    session_use_sticker(s, id);
  }

  free(linked);
  app->reply_to = 0;
  app->stick = true;
}

static void toggle_favorite(App *app, Snowflake id) {
  session_set_favorite_sticker(app->session, id,
                               !session_favorite_sticker(app->session, id));
}

/* ========================================================================== */
/* the picker's parts.                                                        */
/* ========================================================================== */

// the color under the rail and the inspector, the client's base lower.
static civue_color lower(const Look *k) {
  return k->panel;
}

static void section_icon(App *app, const Section *s, float size) {
  const Look *k = &app->look;

  if (s->guild) {
    div(flex, items_center, justify_center, flex_shrink_0, size(px(size)),
        overflow_hidden) {
      guild_icon(app, s->guild, size, false);
    }
  }
  else if (s->key == KEY_FAVORITES) {
    icon(mingcute, star_fill, size(px(size)), flex_shrink_0, text_color(k->subtle));
  }
  else if (s->key == KEY_FREQUENT) {
    icon(mingcute, time_fill, size(px(size)), flex_shrink_0, text_color(k->subtle));
  }
}

// a section's name over its stickers at `y`, which a press folds away. the
// one held at the top of the list while its stickers scroll is `pinned`.
static void heading(App *app, const Section *s, float y, bool pinned) {
  const Look *k = &app->look;

  div(key(id_at(id(pinned ? "sticker-pinned" : "sticker-heading"), s->key)),
      absolute, left(0), top(px(y)), right(px(pinned ? LIST_BAR : 0)),
      h(px(HEADING)), flex, items_center, gap(2), pl(px(LIST_PAD)), pr(2),
      style(.bg = pinned ? k->popover : CIVUE_CLEAR), text_color(k->text),
      hover(text_color(k->strong)), cursor_pointer) {
    if (button().activated) {
      ids_toggle(&app->sticker_folded, s->key);
    }

    section_icon(app, s, 16);
    text(s->name, min_w(0), truncate, text_size(px(14)), line_height(px(18)),
         font_weight(semibold));

    if (s->folded) {
      icon(mingcute, right_fill, size(4), flex_shrink_0);
    }
    else {
      icon(mingcute, down_fill, size(4), flex_shrink_0);
    }
  }
}

typedef struct {
  bool chosen, starred, menu;
} CellPress;

static CellPress cell(App *app, const Listing *l, const Section *s, size_t at,
                      bool inspected, bool moved) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const Store *store = app->session->store;
  const Item *item = &l->items[s->first + at];
  civue_vec2 spot = spot_of(l, s, at);
  // at this width the client moves every sticker the user's setting lets
  // move, and the one pointed at otherwise.
  bool moves =
    store->animate_stickers == STICKERS_ANIMATE_ALWAYS ||
    (inspected && store->animate_stickers == STICKERS_ANIMATE_ON_INTERACTION);
  CellPress press = {};

  div(key(id_at(id_at(id("sticker-cell"), s->key), item->sticker->id)),
      absolute, left(px(spot.x)), top(px(spot.y)), size(px(l->cell)), flex,
      items_center, justify_center, rounded(px(l->cell * .26f)),
      style(.bg = inspected ? k->selected : CIVUE_CLEAR), cursor_pointer) {
    civue_button_state state = button();
    bool alt = ui->input.mods & CIVUE_MOD_ALT;

    if (state.response.hovered && moved) {
      inspect(app, s, item);
    }

    press.starred = state.activated && alt;
    press.chosen = state.activated && !press.starred;
    press.menu = state.response.right_clicked;

    div(relative, size(px(l->cell - 2))) {
      sticker_fill(app, item->sticker->id, item->sticker->format,
                   moves && ui->window_focused, 1);
    }
  }

  return press;
}

typedef struct {
  const Item *chosen, *starred, *menu;
} ListPress;

// the sections of the listing, only what is in view built.
static ListPress grid(App *app, const Listing *l, civue_id list,
                      const Item *inspected, const Section *inspected_in,
                      bool moved) {
  civue_ui *ui = civue_ui_current();
  float top = civue_scroll_y(ui, list) - AHEAD;
  float view = (float)last_size(list).y;
  float bottom = top + AHEAD + (view > 0 ? view : PICKER_HEIGHT) + AHEAD;
  ListPress out = {};

  div(relative, flex_shrink_0, w(full), h(px(l->height))) {
    for (size_t i = 0; i < l->nsections; i++) {
      const Section *s = &l->sections[i];

      if (s->headed && s->top + HEADING >= top && s->top <= bottom) {
        heading(app, s, s->top, false);
      }

      for (size_t at = 0; !s->folded && at < s->count; at++) {
        const Item *item = &l->items[s->first + at];
        float y = (float)spot_of(l, s, at).y;

        if (y + l->row < top || y > bottom) {
          continue;
        }

        CellPress press =
          cell(app, l, s, at, s == inspected_in && item == inspected, moved);

        out.chosen = press.chosen ? item : out.chosen;
        out.starred = press.starred ? item : out.starred;
        out.menu = press.menu ? item : out.menu;
      }
    }
  }

  return out;
}

// the heading of the section the list is scrolled into stays at its top,
// as the client's sticky ones, until the next one pushes it out.
static void pinned_heading(App *app, const Listing *l, float scroll) {
  for (size_t i = 0; i < l->nsections; i++) {
    const Section *s = &l->sections[i];
    float end = i + 1 < l->nsections ? l->sections[i + 1].top : l->height;

    if (s->headed && scroll > s->top && scroll < end) {
      heading(app, s, fminf(0, end - HEADING - scroll), true);
    }
  }
}

// the section the scroll is in.
static Snowflake active_of(const Listing *l, float scroll) {
  Snowflake active = 0;

  for (size_t i = 0; i < l->nsections; i++) {
    const Section *s = &l->sections[i];

    if (s->headed && (active == 0 || s->top <= scroll + ACTIVE_OFFSET)) {
      active = s->key;
    }
  }

  return active;
}

// a section in the rail: its icon, and the client's pill by it, short when
// pointed at and tall for the one in view.
static bool rail_entry(App *app, const Section *s, bool active,
                       civue_response *resp) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = id_at(id("sticker-rail-entry"), s->key);
  bool pressed = false;

  div(key(id), relative, flex, items_center, justify_center, flex_shrink_0,
      size(px(RAIL_ICON)), mb(px(RAIL_GAP)), rounded(px(4)), cursor_pointer,
      style(.bg = !s->guild && active ? k->surface : CIVUE_CLEAR),
      hover(bg(s->guild ? CIVUE_CLEAR : k->surface))) {
    civue_button_state state = button();
    float pill = civue_animate(ui, id_in(id, "pill"),
                               active                   ? 24
                               : state.response.hovered ? 10
                                                        : 0,
                               .2f);

    pressed = state.activated;
    *resp = state.response;

    if (pill > .5f) {
      div(absolute, left(px(-RAIL_GAP)), top(px((RAIL_ICON - pill) / 2)),
          w(px(4)), h(px(pill)), rounded_r(px(4)), bg(k->strong)) {}
    }

    section_icon(app, s, s->guild ? RAIL_ICON : 24);
  }

  return pressed;
}

typedef struct {
  const Section *section;
  civue_response resp;
} RailPoint;

// the sections down the left of the picker, which jump to theirs. the
// guild pointed at is answered, for its name to show.
static RailPoint rail(App *app, const Listing *all, Snowflake active) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = id("sticker-rail");
  float y = RAIL_GAP;
  RailPoint pointed = {};

  div(key(id), flex, flex_col, items_center, flex_shrink_0, w(px(RAIL)),
      h(full), pt(px(RAIL_GAP)), bg(lower(k)), overflow_y_scroll) {
    for (size_t i = 0; i < all->nsections; i++) {
      const Section *s = &all->sections[i];
      bool on = s->key == active;
      civue_response resp = {};

      if (rail_entry(app, s, on, &resp)) {
        app->sticker_jump = s->key;
        free(app->sticker_search);
        app->sticker_search = nullptr;
        civue_request_repaint(ui);
      }

      // the one in view stays in view of the rail, once it is laid out.
      if (on && app->sticker_active != active && last_size(id).y > 0) {
        app->sticker_active = active;
        civue_scroll_to(ui, id, y - RAIL_GAP, y + RAIL_ICON + RAIL_GAP,
                        (float)last_size(id).y);
      }

      if (resp.hovered && s->guild) {
        pointed = (RailPoint){s, resp};
      }

      y += RAIL_ICON + RAIL_GAP;
    }
  }

  return pointed;
}

static bool header(App *app, const Item *inspected) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id search = id("sticker-search");
  bool submitted = false;

  div(flex, items_center, flex_shrink_0, h(px(30 + 12)), px(4), pb(3),
      border_b(1), border_color(k->border)) {
    div(key(search), flex_1, h(px(30)), text_size(px(14)),
        line_height(px(18))) {
      civue_field_look look = field_look(k);

      look.pad = 8;

      // the keys stay the search's whatever was pressed in the picker, as
      // in the client.
      if ((app->sticker_focus || ui->focus != search) && !ui->rerun) {
        ui->focus = search;
        app->sticker_focus = false;
      }

      // the client names the sticker pointed at in its empty search.
      civue_field_response field = text_field(
        &app->sticker_search,
        inspected && inspected->sticker->name ? inspected->sticker->name
                                              : "Find the perfect sticker",
        look);

      if (field.changed) {
        app->sticker_inspected = 0;
        civue_set_scroll_y(ui, id("sticker-list"), 0);
      }

      submitted = field.submitted;
    }
  }

  return submitted;
}

// what is pointed at, at the foot of the list.
static void inspector(App *app, const Item *item) {
  const Look *k = &app->look;

  div(flex, items_center, flex_shrink_0, h(px(INSPECTOR)), px(4), bg(lower(k)),
      border_t(1), border_color(k->border)) {
    if (!item) {
      continue;
    }

    div(size(px(28)), flex_shrink_0, mr(2)) {
      sticker_fill(app, item->sticker->id, item->sticker->format, false, 1);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      text(item->sticker->name ? item->sticker->name : "", truncate,
           text_size(px(16)), line_height(px(20)), font_weight(semibold),
           text_color(k->strong));

      div(flex, gap(1), min_w(0), text_size(px(12)), line_height(px(16)),
          text_color(k->subtle)) {
        text("from", flex_shrink_0);
        text(item->guild->name ? item->guild->name : "", min_w(0), truncate,
             font_weight(semibold));
      }
    }

    div(flex, items_center, justify_center, flex_shrink_0, size(px(32)), ml(2),
        overflow_hidden) {
      guild_icon(app, item->guild, 32, false);
    }
  }
}

// the client's words to try when a search finds nothing.
static void nothing_found(App *app) {
  static const char *const WORDS[] = {
    "laugh", "wave", "yes", "dance", "sad", "no", "hi", "bye", "cry", "ok",
  };
  const Look *k = &app->look;

  div(flex, flex_col, items_center, gap(4), px(4), py(8)) {
    text("Try searching for another term or select one below", text_center,
         text_size(px(14)), line_height(px(18)), font_weight(medium),
         text_color(k->subtle));

    div(flex, flex_wrap, justify_center, gap(2)) {
      for (size_t i = 0; i < countof(WORDS); i++) {
        div(key(id_at(id("sticker-word"), i)), px(3), py(1), rounded(lg),
            bg(k->surface), hover(bg(k->hover)), text_size(px(14)),
            line_height(px(18)), font_weight(medium), text_color(k->text),
            cursor_pointer) {
          if (button().activated) {
            free(app->sticker_search);
            app->sticker_search = strdup(WORDS[i]);
            app->sticker_inspected = 0;
          }

          text(WORDS[i]);
        }
      }
    }
  }
}

// the menu a right click on a sticker opens, the client's one row of it.
static void sticker_menu(App *app, civue_rect *card) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect room = overlay_room();
  bool starred = session_favorite_sticker(app->session, app->sticker_menu);
  float width = 200, height = 40 + 16;
  float left = fminf((float)app->sticker_menu_at.x, (float)room.x1 - width);
  float top = fminf((float)app->sticker_menu_at.y, (float)room.y1 - height);
  bool pressed = false;

  overlay() {
    // a press outside its card closes it, and is no press on a sticker.
    div(key(id("sticker-menu-veil")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y))) {
      civue_button_state veil = button();

      if (veil.response.pressed || veil.response.right_clicked) {
        app->sticker_menu = 0;
      }
    }

    div(key(id("sticker-menu")), flex, flex_col, left(px(left)), top(px(top)),
        w(px(width)), p(2), rounded(lg), bg(k->popover), border(1),
        border_color(k->border_strong), shadow(lg)) {
      *card = here();
      button();
      pressed = menu_row(app, id("sticker-menu-star"), CIVUE_ICON(mingcute, star_fill),
                         starred ? "Unfavorite Sticker" : "Favorite Sticker",
                         starred ? k->star : k->text);
    }
  }

  if (pressed) {
    toggle_favorite(app, app->sticker_menu);
    app->sticker_menu = 0;
  }
  else if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->sticker_menu = 0;
  }
}

/* ========================================================================== */
/* the keys: the arrows move what is pointed at, the search keeps the rest.   */
/* ========================================================================== */

static const Section *step_section(const Listing *l, size_t from, int by) {
  for (size_t i = from + (size_t)by; i < l->nsections; i += (size_t)by) {
    if (!l->sections[i].folded) {
      return &l->sections[i];
    }
  }

  return nullptr;
}

static void move(App *app, const Listing *l, civue_id list, size_t section,
                 size_t at, uint32_t key) {
  civue_ui *ui = civue_ui_current();
  const Section *s = &l->sections[section], *to = s;
  size_t columns = l->columns, column = at % columns;

  switch (key) {
    case CIVUE_KEY_RIGHT:
      if (at + 1 < s->count) {
        at++;
      }
      else if ((to = step_section(l, section, 1))) {
        at = 0;
      }

      break;

    case CIVUE_KEY_LEFT:
      if (at > 0) {
        at--;
      }
      else if ((to = step_section(l, section, -1))) {
        at = to->count - 1;
      }

      break;

    case CIVUE_KEY_DOWN:
      if (at + columns < s->count) {
        at += columns;
      }
      else if (at / columns < (s->count - 1) / columns) {
        at = s->count - 1;
      }
      else if ((to = step_section(l, section, 1))) {
        at = column < to->count ? column : to->count - 1;
      }

      break;

    default:
      if (at >= columns) {
        at -= columns;
      }
      else if ((to = step_section(l, section, -1))) {
        size_t last = (to->count - 1) / columns * columns + column;

        at = last < to->count ? last : to->count - 1;
      }

      break;
  }

  if (!to) {
    return;
  }

  civue_vec2 spot = spot_of(l, to, at);
  float head = at < columns && to->headed ? HEADING : 0;

  inspect(app, to, &l->items[to->first + at]);
  civue_scroll_to(ui, list, (float)spot.y - head, (float)spot.y + l->row,
                  (float)last_size(list).y);
}

// takes the arrows before the search sees them. left and right stay its own
// once it holds something.
static void take_keys(App *app, const Listing *l, civue_id list,
                      const Item *inspected, size_t section, size_t at) {
  civue_ui *ui = civue_ui_current();
  bool typed = app->sticker_search && *app->sticker_search;

  for (uint32_t i = 0; inspected && i < ui->input.keys.len; i++) {
    uint32_t key = ui->input.keys.v[i].key;
    bool sideways = key == CIVUE_KEY_LEFT || key == CIVUE_KEY_RIGHT;

    if ((key != CIVUE_KEY_UP && key != CIVUE_KEY_DOWN && !sideways) ||
        (sideways && typed)) {
      continue;
    }

    for (uint32_t j = i + 1; j < ui->input.keys.len; j++) {
      ui->input.keys.v[j - 1] = ui->input.keys.v[j];
    }

    ui->input.keys.len--;
    i--;
    move(app, l, list, section, at, key);
    inspected = inspected_of(app, l, &section, &at);
  }
}

/* ========================================================================== */
/* the picker.                                                                */
/* ========================================================================== */

void sticker_picker_toggle(App *app, civue_rect anchor) {
  if (app->popover == POPOVER_STICKERS) {
    close_picker(app);
    return;
  }

  app->popover = POPOVER_STICKERS;
  app->sticker_anchor = anchor;
  app->sticker_focus = true;
  app->sticker_inspected = 0;
  app->sticker_menu = 0;
  app->sticker_jump = 0;
  app->sticker_active = 0;
  free(app->sticker_search);
  app->sticker_search = nullptr;
  // it opens at its top, whatever it was left on.
  civue_set_scroll_y(civue_ui_current(), id("sticker-list"), 0);
  civue_set_scroll_y(civue_ui_current(), id("sticker-rail"), 0);
  // the favorites and the ones used the most come with these.
  session_fetch_frecency(app->session);
}

void sticker_picker(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();

  if (app->popover != POPOVER_STICKERS) {
    return;
  }

  // a message being edited takes no sticker.
  if (app->editing) {
    app->popover = POPOVER_NONE;
    return;
  }

  // over the composer, on its right.
  civue_rect room = overlay_room();
  civue_rect anchor = app->sticker_anchor;
  float width = fminf(PICKER_WIDTH, (float)(room.x1 - room.x0));
  float height = fminf(PICKER_HEIGHT, (float)(anchor.y0 - 8 - room.y0));
  float left = fmaxf((float)anchor.x1 - width, (float)room.x0);
  civue_rect card = xywh(left, (float)anchor.y0 - 8 - height, width, height);
  civue_rect menu_card = {};
  civue_id list = id("sticker-list");
  const char *query = app->sticker_search;
  bool searching = !blank(query);
  bool menu_was_open = app->sticker_menu != 0;
  bool moved =
    ui->input.has_mouse && (ui->input.mouse.x != app->sticker_mouse.x ||
                            ui->input.mouse.y != app->sticker_mouse.y);
  Listing all = {}, found = {};

  while (searching && (*query == ' ' || *query == '\t')) {
    query++;
  }

  browse(app, &all, c);

  if (searching) {
    search(app, &found, query);
  }

  Listing *shown = searching ? &found : &all;
  float list_width = (float)last_size(list).x;

  lay(shown, list_width > 0 ? list_width : width - RAIL - 2, searching);

  // the section the rail was pressed for, now that it is laid out.
  for (size_t i = 0; app->sticker_jump && !searching && i < all.nsections;
       i++) {
    if (all.sections[i].key == app->sticker_jump) {
      civue_set_scroll_y(ui, list, all.sections[i].top);
      app->sticker_jump = 0;
    }
  }

  size_t section = 0, at = 0;
  const Item *inspected = inspected_of(app, shown, &section, &at);

  take_keys(app, shown, list, inspected, section, at);
  inspected = inspected_of(app, shown, &section, &at);

  const Section *inspected_in = inspected ? &shown->sections[section] : nullptr;
  Snowflake active = searching ? 0 : active_of(&all, civue_scroll_y(ui, list));
  ListPress press = {};
  RailPoint pointed = {};
  bool submitted = false;

  overlay() {
    div(key(id("sticker-picker")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(width)), h(px(height)), pt(4), rounded(xl),
        bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      card = here();

      // a press on the card is not one on what is behind it.
      button();

      submitted = header(app, inspected);

      div(flex, flex_1, min_h(0)) {
        pointed = rail(app, &all, active);

        div(flex, flex_col, flex_1, min_w(0)) {
          div(relative, flex, flex_col, flex_1, min_h(0), overflow_hidden) {
            div(key(list), flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
                scrollbars(&app->thin_bars), text_color(k->text)) {
              if (shown->nitems) {
                press = grid(app, shown, list, inspected, inspected_in, moved);
              }
              else if (searching) {
                nothing_found(app);
              }
              else {
                text("Your servers have no stickers.", px(4), py(8),
                     text_center, text_size(px(14)), line_height(px(18)),
                     text_color(k->faint));
              }
            }

            pinned_heading(app, shown, civue_scroll_y(ui, list));
          }

          inspector(app, inspected);
        }
      }
    }
  }

  // a guild's name by its icon in the rail. a node opened over the picker
  // would be one of its own, so it waits for the picker to be closed.
  if (pointed.section) {
    hint(app, id_at(id("sticker-rail-entry"), pointed.section->key),
         &pointed.resp, pointed.section->name, CIVUE_TOOLTIP_RIGHT);
  }

  if (app->sticker_menu) {
    sticker_menu(app, &menu_card);
  }

  app->sticker_mouse = ui->input.has_mouse ? ui->input.mouse : point(-1, -1);

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !menu_was_open &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(app->sticker_button, ui->input.mouse);

  // enter takes the one pointed at.
  if (submitted && inspected && !press.chosen) {
    press.chosen = inspected;
  }

  if (press.menu) {
    app->sticker_menu = press.menu->sticker->id;
    app->sticker_menu_at = ui->input.mouse;
  }
  else if (press.starred) {
    toggle_favorite(app, press.starred->sticker->id);
  }
  else if (press.chosen) {
    pick(app, c, press.chosen);
  }
  else if (outside) {
    app->popover = POPOVER_NONE;
    app->sticker_menu = 0;
  }
  else if (!menu_was_open && civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    close_picker(app);
  }

  listing_free(&all);
  listing_free(&found);
}

/* ========================================================================== */
/* the composer: the button, and the sticker that waits for the message.      */
/* ========================================================================== */

void sticker_button(App *app, civue_rect box) {
  if (composer_button(app, id("composer-sticker"), CIVUE_ICON(mingcute, sticker_fill),
                      app->popover == POPOVER_STICKERS, "Open Sticker Picker",
                      &app->sticker_button)
        .activated) {
    sticker_picker_toggle(app, box);
  }
}

void sticker_tray(App *app, const Channel *c) {
  const Look *k = &app->look;
  Snowflake id = sticker_pending(app, c);
  const GuildSticker *sticker =
    id ? store_sticker(app->session->store, id, nullptr) : nullptr;

  if (!sticker) {
    return;
  }

  div(key(id("sticker-tray")), flex, flex_shrink_0, py(px(10)), pl(4),
      border_b(1), border_color(k->border)) {
    div(relative, flex_shrink_0, size(px(TRAY_STICKER)), mr(1)) {
      sticker_fill(app, id, sticker->format, false, 1);

      div(key(id("sticker-tray-close")), absolute, right(px(-4)), top(px(-4)),
          flex, items_center, justify_center, size(4), rounded(full),
          bg(k->subtle), hover(bg(k->strong)), cursor_pointer) {
        civue_button_state state = button();

        if (state.activated) {
          app->sticker_pending = 0;
          app->focus_composer = true;
        }

        icon(mingcute, close_fill, size(3), text_color(k->input));
        hint(app, id("sticker-tray-close"), &state.response, "Remove Sticker",
             CIVUE_TOOLTIP_ABOVE);
      }
    }
  }
}

/* ========================================================================== */
/* a message's sticker and its card.                                          */
/* ========================================================================== */

static bool info_open(const App *app, const Message *m, size_t index) {
  return app->popover == POPOVER_STICKER_INFO && m->id != 0 &&
         app->sticker_message == m->id && app->sticker_index == index;
}

// the client's tooltip: a sticker's mark and its name.
static void name_tip(App *app, civue_id id, const civue_response *resp,
                     const char *name) {
  const Look *k = &app->look;

  if (!tooltip(id, resp)) {
    return;
  }

  civue_id tip = id_in(id, "tip");
  civue_tooltip_place at =
    tooltip_at(resp->rect, last_size(tip), CIVUE_TOOLTIP_ABOVE, 8);

  civue_rect box = at.rect;

  overlay() {
    div(key(tip), left(px(at.rect.x0)), top(px(at.rect.y0)), flex, items_center,
        gap(px(6)), px(3), py(px(6)), rounded(lg), bg(k->tooltip),
        whitespace_nowrap, text_size(px(12)), line_height(px(16)),
        text_color(k->on_tooltip)) {
      box = here();
      icon(mingcute, sticker_fill, size(px(14)), flex_shrink_0);
      text(name);
    }

    tooltip_arrow(box, resp->rect, at.side, k->tooltip);
  }
}

void sticker_in_message(App *app, civue_id id, const Message *m, size_t index,
                        const Sticker *sticker, bool moving, bool opens) {
  const Look *k = &app->look;
  const char *name =
    sticker->name && *sticker->name ? sticker->name : "Sticker";
  bool open = opens && info_open(app, m, index);

  div(key(id), relative, flex, items_center, justify_center, flex_shrink_0,
      self_start, size(px(MESSAGE_STICKER)), mt(1),
      style(.cursor = opens ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    civue_button_state state = button();

    if (state.activated && opens && m->id) {
      if (open) {
        app->popover = POPOVER_NONE;
      }
      else {
        app->popover = POPOVER_STICKER_INFO;
        app->sticker_message = m->id;
        app->sticker_index = (uint32_t)index;
        app->sticker_info_anchor = here();
      }
    }

    // a lottie sticker is no image: its name stands for it.
    if (sticker->format == STICKER_LOTTIE || sticker->format == 0) {
      div(flex, flex_col, items_center, justify_center, gap(2), size(full),
          px(2), rounded(lg), bg(k->surface), text_color(k->muted)) {
        icon(mingcute, sticker_fill, size(8));
        text(name, max_w(full), truncate, text_size(px(14)),
             line_height(px(18)));
      }
    }
    else {
      sticker_fill(app, sticker->id, sticker->format, moving,
                   m->state == MESSAGE_SENT ? 1 : .5f);
    }

    // a preview is over the window already, where a tip would be laid in
    // its node.
    if (!open && opens) {
      name_tip(app, id, &state.response, name);
    }
  }
}

static void count_with_commas(uint32_t n, char *out, size_t cap) {
  char digits[16];
  int len = snprintf(digits, sizeof digits, "%u", n);
  size_t at = 0;

  for (int i = 0; i < len && at + 2 < cap; i++) {
    if (i && (len - i) % 3 == 0) {
      out[at++] = ',';
    }

    out[at++] = digits[i];
  }

  out[at] = 0;
}

// where a sticker is from, under what its card says of it.
static bool guild_row(App *app, const char *title, Snowflake guild_id,
                      const Guild *guild, const char *name,
                      const char *icon_hash, const char *line) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];
  bool pressed = false;

  cdn_guild_icon(url, guild_id, icon_hash, 64);

  div(flex, flex_col, gap(2), p(4), bg(lower(k))) {
    text(title, text_size(px(14)), line_height(px(18)), text_color(k->subtle));

    div(key(id("sticker-info-guild")), flex, items_center, gap(px(10)),
        style(.cursor = guild ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
              .has_cursor = true)) {
      pressed = guild && button().activated;

      div(flex, items_center, justify_center, flex_shrink_0, size(8),
          overflow_hidden) {
        if (guild) {
          guild_icon(app, guild, 32, false);
        }
        else if (!*url || !image_fill(app, url, 32 * .3f)) {
          fill(here(), k->hover, 32 * .3f);
        }
      }

      div(flex, flex_col, flex_1, min_w(0)) {
        text(name, truncate, text_size(px(16)), line_height(px(20)),
             font_weight(semibold), text_color(k->strong));
        text(line, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }
    }
  }

  return pressed;
}

void sticker_info(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  Store *store = session->store;

  if (app->popover != POPOVER_STICKER_INFO) {
    return;
  }

  const Message *m = store_message(c, app->sticker_message);
  FakeSticker fakes[FAKE_STICKERS_MAX];
  size_t nfakes = m ? fake_stickers(store, m, fakes, countof(fakes)) : 0;

  // the message went away under its card.
  if (!m || app->sticker_index >= m->nstickers + nfakes) {
    app->popover = POPOVER_NONE;
    return;
  }

  // one read from a link or a gif comes after the message's own.
  bool fake = app->sticker_index >= m->nstickers;
  const FakeSticker *read =
    fake ? &fakes[app->sticker_index - m->nstickers] : nullptr;
  Sticker read_as =
    fake ? (Sticker){read->id, (char *)read->name, read->format} : (Sticker){};
  const Sticker *item = fake ? &read_as : &m->stickers[app->sticker_index];
  const Guild *guild = nullptr;
  const GuildSticker *own = store_sticker(store, item->id, &guild);
  // discord is only asked about a sticker from elsewhere.
  const StickerInfo *info =
    own ? nullptr : session_sticker_info(session, item->id);
  const char *name = own && own->name     ? own->name
                     : info && info->name ? info->name
                     : item->name         ? item->name
                                          : "Sticker";
  const char *about = own    ? own->description
                      : info ? info->description
                             : nullptr;
  const char *says = nullptr;
  char line[64] = "Invite-only server", told[320];

  // the client's words for each case, for one who can use stickers
  // everywhere: FakeNitro tells the client so.
  if (own && guild->id == c->guild_id) {
    says = "This sticker is from this server. You can use it everywhere.";
  }
  else if (own) {
    says = "This sticker is from one of your servers. You can use it "
           "everywhere.";
  }
  else if (info->loading) {
    says = "Loading...";
  }
  else if (info->guild_name) {
    says = "Want to use this sticker everywhere? Join the server.";
  }
  else if (info->guild_id || !info->name) {
    says = "This sticker is from a server that is either invite-only or "
           "unavailable.";
  }

  // the plugin's own words after them.
  if (fake) {
    snprintf(told, sizeof told,
             "%s%sThis is a FakeNitro sticker and renders like a real sticker "
             "only for you. Appears as a link to non-plugin users.",
             says ? says : "", says ? " " : "");
    says = told;
  }

  if (guild && store_guild_has(guild, "DISCOVERABLE")) {
    snprintf(line, sizeof line, "Discoverable Server");
  }
  else if (info && info->guild_name) {
    char online[16];

    count_with_commas(info->guild_online, online, sizeof online);
    snprintf(line, sizeof line, "%s Online · Discoverable Server", online);
  }

  // on the sticker's right, on its left when there is no room.
  civue_id card_id = id("sticker-info");
  civue_rect room = overlay_room();
  civue_rect anchor = app->sticker_info_anchor;
  float height = (float)last_size(card_id).y;
  float left = (float)anchor.x1 + 8;
  float top = (float)(anchor.y0 + anchor.y1) / 2 - height / 2;
  civue_rect card = {};
  bool opened = false;

  if (left + INFO_WIDTH > room.x1) {
    left = fmaxf((float)anchor.x0 - 8 - INFO_WIDTH, (float)room.x0);
  }

  top = fmaxf(fminf(top, (float)room.y1 - height), (float)room.y0);

  overlay() {
    div(key(card_id), flex, flex_col, left(px(left)), top(px(top)),
        w(px(INFO_WIDTH)), rounded(lg), bg(k->popover), border(1),
        border_color(k->border_strong), shadow(lg), overflow_hidden) {
      card = here();
      button();

      div(flex, flex_col, gap(2), p(4)) {
        div(flex, items_center, gap(px(10))) {
          div(flex, items_center, justify_center, size(px(48)), flex_shrink_0) {
            // a lottie sticker is no image.
            if (!sticker_fill(app, item->id, item->format, ui->window_focused,
                              1) &&
                (item->format == STICKER_LOTTIE || item->format == 0)) {
              icon(mingcute, sticker_fill, size(8), text_color(k->muted));
            }
          }

          div(flex, flex_col, flex_1, min_w(0)) {
            text(name, truncate, text_size(px(16)), line_height(px(20)),
                 font_weight(semibold), text_color(k->strong));

            if (about && *about) {
              text(about, text_size(px(14)), line_height(px(18)),
                   text_color(k->text));
            }
          }
        }

        if (says) {
          text(says, text_size(px(14)), line_height(px(18)),
               text_color(k->subtle));
        }
      }

      if (guild) {
        opened = guild_row(app, "This sticker is from", guild->id, guild,
                           guild->name ? guild->name : "", guild->icon, line);
      }
      else if (info->guild_name) {
        guild_row(app, "Get this sticker from", info->guild_id, nullptr,
                  info->guild_name, info->guild_icon, line);
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (opened) {
    app->popover = POPOVER_NONE;
    app_select_guild(app, guild->id);
  }
  else if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}
