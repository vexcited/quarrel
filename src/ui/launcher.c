#include "components.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "media.h"
#include "server.h"
#include "widgets.h"

typedef enum : uint8_t {
  KIND_ALL,
  KIND_UNREAD,
  KIND_MESSAGES,
  KIND_SERVERS,
  KIND_CHANNELS,
  KINDS,
} Kind;

static const char *const KIND_NAMES[KINDS] = {
  "All", "Unread", "Messages", "Servers", "Channels",
};

typedef enum : uint8_t {
  PLACE_PRIVATE,
  PLACE_GUILD,
  PLACE_CHANNEL,
} PlaceKind;

typedef struct {
  PlaceKind kind;
  // how well its name matches, 4 for the whole of it down to 1 for a piece
  // of it.
  uint8_t score;
  // where it is among the visits, the latest highest, -1 for none.
  int32_t visit;
  // its last message, for the unread ones.
  Snowflake recent;
  uint32_t order;
  // the title of its part of the list, none for a ranked one.
  const char *section;
  Guild *guild;
  Channel *channel;
} Place;

typedef struct {
  Place *v;
  size_t len, cap;
} Places;

// what one frame looks for.
typedef struct {
  App *app;
  Store *store;
  // what was typed, folded as names are.
  char query[256];
  bool typed;
  Places places;
  // the guild of each visit, 0 for a direct message.
  Snowflake visit_guilds[64];
  uint32_t visits;
} Search;

constexpr size_t PLACES = 80;
constexpr size_t RECENT = 6;
constexpr size_t MENTIONS = 8;
constexpr float WIDTH = 640;
constexpr float ROW = 44;
constexpr float SECTION = 28;
constexpr float SEARCH_ROW = 56;

// the keys pressed this frame that the launcher answers once it knows its
// rows.
static bool entered, follow;

static void show(App *app, Kind kind, Snowflake guild_id) {
  app->launcher_open = true;
  app->launcher_kind = kind;
  app->launcher_scoped = guild_id;
  app->launcher_guild = guild_id;
  app->launcher_pick = 0;
  app->popover = POPOVER_NONE;
  free(app->launcher_query);
  app->launcher_query = nullptr;
  civue_set_scroll_y(civue_ui_current(), id("launcher-list"), 0);
}

void launcher_show(App *app, Snowflake guild_id) {
  show(app, guild_id ? KIND_ALL : KIND_MESSAGES, guild_id);
}

static void hide(App *app) {
  app->launcher_open = false;
  free(app->launcher_query);
  app->launcher_query = nullptr;
  civue_ui_current()->focus = 0;
}

// the kinds a guild holds none of leave the guild kept to.
static void pick_kind(App *app, int kind) {
  app->launcher_kind = (uint8_t)kind;
  app->launcher_pick = 0;
  app->launcher_scoped &= kind != KIND_MESSAGES && kind != KIND_SERVERS;
  civue_set_scroll_y(civue_ui_current(), id("launcher-list"), 0);
}

static void drop_key(civue_ui *ui, uint32_t at) {
  for (uint32_t i = at + 1; i < ui->input.keys.len; i++) {
    ui->input.keys.v[i - 1] = ui->input.keys.v[i];
  }

  ui->input.keys.len--;
}

void launcher_keys(App *app) {
  civue_ui *ui = civue_ui_current();

  for (uint32_t i = 0; i < ui->input.keys.len; i++) {
    civue_key_press press = ui->input.keys.v[i];
    bool command = press.mods & CIVUE_MOD_CMD;
    bool taken = true;

    // a dialog over the window keeps the keys.
    if (press.key == 'k' && command && !app->settings_open &&
        !server_covers(app) && !media_viewing(app)) {
      if (app->launcher_open) {
        hide(app);
      }
      else {
        show(app, KIND_ALL, 0);
      }
    }
    else if (!app->launcher_open) {
      taken = false;
    }
    else if (press.key == CIVUE_KEY_ESCAPE) {
      hide(app);
    }
    else if (press.key == CIVUE_KEY_UP) {
      app->launcher_pick--;
      follow = true;
    }
    else if (press.key == CIVUE_KEY_DOWN) {
      app->launcher_pick++;
      follow = true;
    }
    else if (press.key == CIVUE_KEY_ENTER) {
      entered = true;
    }
    else if (press.key == CIVUE_KEY_TAB) {
      int step = press.mods & CIVUE_MOD_SHIFT ? KINDS - 1 : 1;

      pick_kind(app, (app->launcher_kind + step) % KINDS);
    }
    // a backspace in an empty search leaves the guild it kept to.
    else if (press.key == CIVUE_KEY_BACKSPACE && app->launcher_scoped &&
             !(app->launcher_query && *app->launcher_query)) {
      app->launcher_scoped = false;
    }
    else {
      taken = false;
    }

    if (taken) {
      drop_key(ui, i--);
    }
  }
}

// a letter with an accent as the one without, and every letter small, so
// that "general" finds "Général".
static void fold(const char *s, char *out, size_t cap) {
  static const char LATIN[] = "aaaaaaaceeeeiiiidnooooo/ouuuuypsaaaaaaaceeeeiiii"
                              "dnooooo/ouuuuypy";
  size_t n = 0;

  for (const unsigned char *at = (const unsigned char *)s; *at && n + 1 < cap;
       at++) {
    if (at[0] == 0xc3 && at[1] >= 0x80 && at[1] <= 0xbf) {
      out[n++] = LATIN[at[1] - 0x80];
      at++;
    }
    else {
      out[n++] = (char)tolower(*at);
    }
  }

  out[n] = 0;
}

// how well `name` holds what was typed: 4 for all of it, 3 for its start,
// 2 for the start of one of its words, 1 anywhere.
static uint8_t match(const char *name, const char *query) {
  char folded[256];

  if (!name) {
    return 0;
  }

  fold(name, folded, sizeof folded);

  const char *at = strstr(folded, query);

  if (!at) {
    return 0;
  }

  if (!strcmp(folded, query)) {
    return 4;
  }

  if (at == folded) {
    return 3;
  }

  for (; at; at = strstr(at + 1, query)) {
    if (!isalnum((unsigned char)at[-1])) {
      return 2;
    }
  }

  return 1;
}

// as `match`, and a search of words may find some in where the place is:
// "mines gen" finds a "général" of "CRTL+ALT+MINES".
static uint8_t match_in(const char *name, const char *context,
                        const char *query) {
  uint8_t score = match(name, query);

  if (score || !context || !strchr(query, ' ')) {
    return score;
  }

  char words[256], both[512];

  snprintf(both, sizeof both, "%s %s", name ? name : "", context);
  fold(both, words, sizeof words);
  snprintf(both, sizeof both, "%s", query);

  for (char *save = nullptr, *word = strtok_r(both, " ", &save); word;
       word = strtok_r(nullptr, " ", &save)) {
    if (!strstr(words, word)) {
      return 0;
    }
  }

  return 1;
}

static void push(Search *s, Place one) {
  Places *p = &s->places;

  // a channel shows once, in the first section that lists it.
  for (size_t i = 0; one.section && one.channel && i < p->len; i++) {
    if (p->v[i].channel == one.channel) {
      return;
    }
  }

  if (p->len == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 64;
    p->v = realloc(p->v, p->cap * sizeof *p->v);
  }

  one.order = (uint32_t)p->len;
  p->v[p->len++] = one;
}

static int32_t visit_of(const Search *s, Snowflake channel_id) {
  const Ids *v = &s->app->visits;

  for (uint32_t i = v->len; i-- > 0;) {
    if (v->v[i] == channel_id) {
      return (int32_t)i;
    }
  }

  return -1;
}

static int32_t guild_visit(const Search *s, Snowflake guild_id) {
  for (uint32_t i = s->visits; i-- > 0;) {
    if (s->visit_guilds[i] == guild_id) {
      return (int32_t)i;
    }
  }

  return -1;
}

// whether the place may be listed: the guild the launcher keeps to.
static bool kept(const Search *s, Snowflake guild_id) {
  const App *app = s->app;

  return !app->launcher_scoped || guild_id == app->launcher_guild;
}

static bool lit(Store *store, const Channel *c) {
  Unread unread = store_channel_unread(store, c);

  return unread.mentions || (unread.unread && !unread.muted);
}

static bool openable(const Channel *c) {
  return channel_has_text(c) || channel_is_forum(c) || channel_is_voice(c) ||
         c->type == CHANNEL_GUILD_DIRECTORY;
}

static const User *dm_user(const Store *store, const Channel *c) {
  return c->type == CHANNEL_DM && c->nrecipients
           ? store_user(store, c->recipients[0])
           : nullptr;
}

static Place place_of(Search *s, Channel *c) {
  Place p = {
    .kind = c->guild_id ? PLACE_CHANNEL : PLACE_PRIVATE,
    .visit = visit_of(s, c->id),
    .recent = c->last_message_id,
    .channel = c,
  };

  p.guild = c->guild_id ? store_guild(s->store, c->guild_id) : nullptr;
  return p;
}

// the places opened last, the latest first, the one open now left out.
static void recent(Search *s, bool privates, bool channels, size_t most,
                   const char *section) {
  const Ids *v = &s->app->visits;
  size_t n = 0;

  for (uint32_t i = v->len; i-- > 0 && n < most;) {
    Channel *c = store_channel(s->store, v->v[i]);

    if (!c || c->id == s->app->session->selected_channel_id ||
        !(c->guild_id ? channels : privates) || !kept(s, c->guild_id)) {
      continue;
    }

    Place p = place_of(s, c);

    p.section = section;
    push(s, p);
    n++;
  }
}

static void privates(Search *s, bool unread, const char *section) {
  size_t count = 0;
  Channel *const *channels = store_private_channels(s->store, &count);

  if (s->app->launcher_scoped) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    Channel *c = channels[i];
    const User *user = dm_user(s->store, c);
    char name[128];
    uint8_t score = 0;

    if (unread && !lit(s->store, c)) {
      continue;
    }

    if (s->typed) {
      store_channel_name(s->store, c, name, sizeof name);
      score = match(name, s->query);

      if (user) {
        uint8_t by = match(user->username, s->query);

        score = by > score ? by : score;
      }

      if (!score) {
        continue;
      }
    }

    Place p = place_of(s, c);

    p.score = score;
    p.section = section;
    push(s, p);
  }
}

static void guilds(Search *s, const char *section) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(s->store, &count);

  // kept to a guild, its channels are what is looked for.
  if (s->app->launcher_scoped) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      Guild *guild = folders[i].guilds[j];
      uint8_t score = s->typed ? match(guild->name, s->query) : 0;

      if ((s->typed && !score) || !kept(s, guild->id)) {
        continue;
      }

      push(s, (Place){
                .kind = PLACE_GUILD,
                .score = score,
                .visit = guild_visit(s, guild->id),
                .section = section,
                .guild = guild,
              });
    }
  }
}

// the channels of every guild kept to that match, or that wait to be read
// when `unread`, or that mention the user when `mentions`.
static void channels(Search *s, bool unread, bool mentions,
                     const char *section) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(s->store, &count);

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      Guild *guild = folders[i].guilds[j];
      size_t nrows = 0;
      const ChannelRow *rows = store_guild_channels(s->store, guild, &nrows);

      if (!kept(s, guild->id)) {
        continue;
      }

      for (size_t r = 0; r < nrows; r++) {
        Channel *c = rows[r].channel;
        uint8_t score = 0;

        // a hub's directory is the hub itself.
        if (rows[r].kind == CHANNEL_ROW_CATEGORY || !openable(c) ||
            (guild->hub && c->type == CHANNEL_GUILD_DIRECTORY)) {
          continue;
        }

        if (unread && !lit(s->store, c)) {
          continue;
        }

        if (mentions && !store_channel_unread(s->store, c).mentions) {
          continue;
        }

        if (s->typed) {
          score = match_in(c->name, guild->name, s->query);

          if (!score) {
            continue;
          }
        }

        Place p = place_of(s, c);

        p.score = score;
        p.section = section;
        push(s, p);
      }
    }
  }
}

static int by_rank(const void *a, const void *b) {
  const Place *x = a, *y = b;

  if (x->score != y->score) {
    return y->score - x->score;
  }

  if (x->visit != y->visit) {
    return y->visit > x->visit ? 1 : -1;
  }

  if (x->kind != y->kind) {
    return x->kind - y->kind;
  }

  if (x->recent != y->recent) {
    return y->recent > x->recent ? 1 : -1;
  }

  return x->order < y->order ? -1 : x->order > y->order;
}

static void rank(Search *s, size_t from) {
  Places *p = &s->places;

  if (p->len > from) {
    qsort(p->v + from, p->len - from, sizeof *p->v, by_rank);
  }
}

static void gather(Search *s) {
  Kind kind = s->app->launcher_kind;

  if (s->typed) {
    if (kind == KIND_ALL || kind == KIND_MESSAGES || kind == KIND_UNREAD) {
      privates(s, kind == KIND_UNREAD, nullptr);
    }

    if (kind == KIND_ALL || kind == KIND_SERVERS) {
      guilds(s, nullptr);
    }

    if (kind == KIND_ALL || kind == KIND_CHANNELS || kind == KIND_UNREAD) {
      channels(s, kind == KIND_UNREAD, false, nullptr);
    }

    rank(s, 0);
    return;
  }

  switch (kind) {
    case KIND_ALL: {
      recent(s, true, true, RECENT, "Recent");

      size_t from = s->places.len;

      privates(s, true, "Mentions");

      for (size_t i = s->places.len; i-- > from;) {
        if (!store_channel_unread(s->store, s->places.v[i].channel).mentions) {
          s->places.v[i] = s->places.v[--s->places.len];
        }
      }

      channels(s, false, true, "Mentions");
      rank(s, from);

      if (s->places.len > from + MENTIONS) {
        s->places.len = from + MENTIONS;
      }

      // the guild kept to lists its channels instead.
      if (s->app->launcher_scoped) {
        channels(s, false, false, "Channels");
      }
      else {
        guilds(s, "Servers");
      }
      break;
    }
    case KIND_UNREAD:
      privates(s, true, nullptr);
      channels(s, true, false, nullptr);
      rank(s, 0);
      break;
    case KIND_MESSAGES:
      privates(s, false, nullptr);
      break;
    case KIND_SERVERS:
      guilds(s, nullptr);
      break;
    case KIND_CHANNELS:
      recent(s, false, true, PLACES, "Recent");
      break;
    default:
      break;
  }
}

static void search_of(App *app, Search *s) {
  const Ids *v = &app->visits;
  const char *typed = app->launcher_query ? app->launcher_query : "";

  *s = (Search){.app = app, .store = app->session->store};

  while (*typed == ' ') {
    typed++;
  }

  fold(typed, s->query, sizeof s->query);

  for (size_t n = strlen(s->query); n && s->query[n - 1] == ' '; n--) {
    s->query[n - 1] = 0;
  }

  s->typed = *s->query != 0;

  for (uint32_t i = 0; i < v->len && s->visits < 64; i++) {
    const Channel *c = store_channel(s->store, v->v[i]);

    s->visit_guilds[s->visits++] = c ? c->guild_id : 0;
  }

  gather(s);

  if (s->places.len > PLACES) {
    s->places.len = PLACES;
  }
}

void keycap(const Look *k, bool command, const char *label) {
  div(flex, items_center, justify_center, gap(px(2)), flex_shrink_0, h(5),
      min_w(5), px(px(5)), rounded(md), border(1),
      border_color(k->border_strong), text_size(px(11)), line_height(px(14)),
      font_weight(medium), text_color(k->faint)) {
    if (command) {
#ifdef __APPLE__
      icon(mingcute, command_fill, size(px(11)));
#else
      text("Ctrl");
#endif
    }

    if (label) {
      text(label);
    }
  }
}

static void icon_cap(const Look *k, civue_icon glyph_icon) {
  div(flex, items_center, justify_center, flex_shrink_0, size(5), rounded(md),
      border(1), border_color(k->border_strong), text_color(k->faint)) {
    icon_of(glyph_icon, size(px(11)));
  }
}

static void place_art(App *app, const Place *p) {
  const Look *k = &app->look;

  if (p->kind == PLACE_GUILD) {
    div(flex, items_center, justify_center, size(6), flex_shrink_0,
        rounded(md), overflow_hidden) {
      guild_icon(app, p->guild, 24, false);
    }
    return;
  }

  if (p->kind == PLACE_CHANNEL) {
    div(flex, items_center, justify_center, size(6), flex_shrink_0,
        rounded(md), bg(k->hover)) {
      glyph(channel_icon(p->channel), 14, k->subtle);
    }
    return;
  }

  if (p->channel->type != CHANNEL_GROUP_DM) {
    avatar(app, dm_user(app->session->store, p->channel), 0, 24, false,
           k->popover);
    return;
  }

  char url[CDN_URL_MAX];
  cdn_channel_icon(url, p->channel->id, p->channel->icon, 48);

  div(flex, items_center, justify_center, size(6), flex_shrink_0,
      rounded(full), bg(k->primary), text_color(k->on_primary)) {
    if (!*url || !image_fill(app, url, 12)) {
      icon(mingcute, group_fill, size(px(14)));
    }
  }
}

// where the place is: a channel's guild, a guild's space, a conversation's
// username.
static void place_context(App *app, const Place *p, char *out, size_t cap) {
  const User *user = nullptr;

  out[0] = 0;

  switch (p->kind) {
    case PLACE_CHANNEL:
      // kept to the guild, where the channel is in it.
      if (app->launcher_scoped) {
        const Channel *parent =
          p->channel->parent_id
            ? store_channel(app->session->store, p->channel->parent_id)
            : nullptr;

        snprintf(out, cap, "%s", parent && parent->name ? parent->name : "");
      }
      else {
        snprintf(out, cap, "%s",
                 p->guild && p->guild->name ? p->guild->name : "");
      }
      break;
    case PLACE_GUILD:
      guild_folder_name(app, p->guild->id, out, cap);
      break;
    case PLACE_PRIVATE:
      user = dm_user(app->session->store, p->channel);

      if (user && user->username) {
        snprintf(out, cap, "@%s", user->username);
      }
      else if (p->channel->type == CHANNEL_GROUP_DM) {
        snprintf(out, cap, "%zu members", p->channel->nrecipients + 1);
      }
      break;
  }
}

static bool place_row(App *app, const Place *p, int32_t index, bool moved) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool picked = app->launcher_pick == index;
  Unread unread = p->kind == PLACE_GUILD ? store_guild_unread(store, p->guild)
                                         : store_channel_unread(store,
                                                                p->channel);
  bool bright = unread.mentions || (unread.unread && !unread.muted);
  bool pressed = false;
  char name[128], context[160];

  if (p->kind == PLACE_GUILD) {
    snprintf(name, sizeof name, "%s", p->guild->name ? p->guild->name : "");
  }
  else if (p->kind == PLACE_CHANNEL) {
    snprintf(name, sizeof name, "%s",
             p->channel->name ? p->channel->name : "");
  }
  else {
    store_channel_name(store, p->channel, name, sizeof name);
  }

  place_context(app, p, context, sizeof context);

  div(key(id_at(id("launcher-place"), (uint64_t)index)), flex, items_center,
      gap(3), flex_shrink_0, h(px(ROW)), px(3), rounded(lg),
      style(.bg = picked ? k->selected : CIVUE_CLEAR), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;

    if (state.response.hovered && moved) {
      app->launcher_pick = index;
    }

    place_art(app, p);

    div(flex, items_baseline, gap(2), flex_1, min_w(0)) {
      if (bright || picked) {
        text(name, min_w(0), truncate, font_weight(semibold),
             text_color(k->strong));
      }
      else {
        text(name, min_w(0), truncate, font_weight(medium),
             text_color(k->text));
      }

      text(context, flex_1, min_w(0), truncate, text_size(px(12)),
           line_height(px(16)), text_color(k->faint));
    }

    if (unread.mentions) {
      mention_badge(k, unread.mentions);
    }
    else if (bright) {
      div(size(2), flex_shrink_0, rounded(full), bg(k->strong)) {}
    }

    if (picked) {
      icon(mingcute, corner_down_left_fill, size(4), flex_shrink_0,
           text_color(k->faint));
    }
  }

  return pressed;
}

static void open_place(App *app, const Place *p) {
  hide(app);

  if (p->kind == PLACE_GUILD) {
    app_select_guild(app, p->guild->id);
    return;
  }

  if (channel_is_voice(p->channel)) {
    app->voice_chat = false;
  }

  app_select_channel(app, p->channel->id);
}

static bool chip(App *app, civue_id id, const char *label, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, flex_shrink_0, h(7), px(3),
      rounded(full), border(1),
      border_color(on ? k->border_strong : k->border),
      text_size(px(13)), line_height(px(16)), font_weight(medium),
      text_color(on ? k->strong : k->subtle),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    pressed = button().activated;
    text(label);
  }

  return pressed;
}

static void search_row(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id field = id("launcher-search");
  bool unscope = false;

  div(flex, items_center, gap(3), flex_shrink_0, h(px(SEARCH_ROW)), px(4),
      border_b(1), border_color(k->border)) {
    icon(mingcute, search_2_fill, size(5), flex_shrink_0,
         text_color(k->faint));

    Guild *scope = app->launcher_scoped
                     ? store_guild(app->session->store, app->launcher_guild)
                     : nullptr;

    if (scope) {
      const char *name = scope->name ? scope->name : "";

      div(key(id("launcher-scope")), flex, items_center, gap(1),
          flex_shrink_0, max_w(px(180)), h(7), pl(2), pr(1), rounded(md),
          bg(k->selected), text_size(px(13)), line_height(px(16)),
          font_weight(medium), text_color(k->strong), cursor_pointer) {
        civue_button_state state = button();

        unscope = state.activated;
        text(name, min_w(0), truncate);
        icon(mingcute, close_fill, size(px(12)), flex_shrink_0,
             text_color(k->faint));
        hint(app, id("launcher-scope"), &state.response,
             "Search everywhere", CIVUE_TOOLTIP_BELOW);
      }
    }

    div(key(field), flex_1, min_w(0), h(px(28)), text_size(px(17)),
        line_height(px(24))) {
      civue_field_look look = field_look(k);

      look.pad = 0;
      look.idle.bg = look.hovered.bg = look.editing.bg = CIVUE_CLEAR;
      look.idle.border = look.hovered.border = look.editing.border =
        CIVUE_CLEAR;

      if (ui->focus != field && !ui->rerun) {
        ui->focus = field;
      }

      const char *hint_text = app->launcher_kind == KIND_MESSAGES
                                ? "Find a conversation"
                              : scope ? "Find a channel"
                                      : "Where to?";

      if (text_field(&app->launcher_query, hint_text, look).changed) {
        app->launcher_pick = 0;
        civue_set_scroll_y(ui, id("launcher-list"), 0);
      }
    }

    keycap(k, false, "esc");
  }

  if (unscope) {
    app->launcher_scoped = false;
  }
}

static void chips(App *app) {
  const Look *k = &app->look;
  int picked = -1;

  div(flex, items_center, gap(2), flex_shrink_0, px(4), py(3), border_b(1),
      border_color(k->border)) {
    for (int i = 0; i < KINDS; i++) {
      if (chip(app, id_at(id("launcher-kind"), (uint64_t)i), KIND_NAMES[i],
               app->launcher_kind == i)) {
        picked = i;
      }
    }
  }

  if (picked >= 0) {
    pick_kind(app, picked);
  }
}

static void footer(App *app, size_t count) {
  const Look *k = &app->look;
  char found[32];

  snprintf(found, sizeof found, "%zu %s", count,
           count == 1 ? "place" : "places");

  div(flex, items_center, gap(4), flex_shrink_0, h(10), px(4), border_t(1),
      border_color(k->border), text_size(px(12)), line_height(px(16)),
      text_color(k->faint)) {
    div(flex, items_center, gap(1)) {
      icon_cap(k, CIVUE_ICON(mingcute, arrow_up_fill));
      icon_cap(k, CIVUE_ICON(mingcute, arrow_down_fill));
      text("Move");
    }

    div(flex, items_center, gap(1)) {
      icon_cap(k, CIVUE_ICON(mingcute, corner_down_left_fill));
      text("Open");
    }

    div(flex, items_center, gap(1)) {
      keycap(k, false, "tab");
      text("Filter");
    }

    div(flex_1) {}
    text(found);
  }
}

// the rows and the titles over their parts, the picked one kept in view.
static const Place *list(App *app, const Places *places, float view) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id list_id = id("launcher-list");
  // the row under a pointer at rest is not picked as the list scrolls.
  bool moved = civue_pointer_moved(ui);
  const char *section = nullptr;
  const Place *pressed = nullptr;
  float y = 0, top = 0;

  for (size_t i = 0; i < places->len; i++) {
    if (places->v[i].section && places->v[i].section != section) {
      y += SECTION;
    }

    section = places->v[i].section;

    if ((int32_t)i == app->launcher_pick) {
      top = y;
    }

    y += ROW;
  }

  if (follow) {
    // the title over the first row comes along.
    civue_scroll_to(ui, list_id, top <= SECTION ? 0 : top, top + ROW + 16,
                    view);
    follow = false;
  }

  section = nullptr;

  div(key(list_id), flex, flex_col, flex_1, min_h(0), p(2), text_size(px(14)),
      line_height(px(18)), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < places->len; i++) {
      const Place *p = &places->v[i];

      if (p->section && p->section != section) {
        text(p->section, flex_shrink_0, h(px(SECTION)), px(3), pt(3),
             text_size(px(12)), line_height(px(16)), font_weight(medium),
             text_color(k->faint));
      }

      section = p->section;

      if (place_row(app, p, (int32_t)i, moved)) {
        pressed = p;
      }
    }

    if (!places->len) {
      div(flex, flex_col, items_center, justify_center, gap(2), flex_1,
          text_color(k->faint)) {
        icon(mingcute, search_2_fill, size(8));
        text(app->launcher_query && *app->launcher_query
               ? "Nothing goes by that name."
               : "Nothing here yet.",
             text_size(px(14)), line_height(px(18)));
      }
    }
  }

  return pressed;
}

void launcher(App *app) {
  if (!app->launcher_open) {
    entered = follow = false;
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  float width = fminf(WIDTH, ui->size.x - 48);
  float top = fmaxf(48, ui->size.y * .14f);
  float height = fminf(520, ui->size.y - top - 32);
  // what the rows have of the card, its other parts and its border off.
  float view = height - SEARCH_ROW - 44 - 40 - 2 - 16;
  Search s = {};
  const Place *pressed = nullptr;
  bool close = false;

  search_of(app, &s);

  int32_t count = (int32_t)s.places.len;

  // the arrows go around.
  if (count) {
    app->launcher_pick = (app->launcher_pick % count + count) % count;
  }

  app->covers_buttons = true;

  overlay() {
    div(key(id("launcher-backdrop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), bg(k->scrim)) {
      close = button().activated;

      div(key(id("launcher")), absolute, flex, flex_col,
          left(px((ui->size.x - width) / 2)), top(px(top)), w(px(width)),
          h(px(height)), rounded(xl), bg(k->popover), border(1),
          border_color(k->border_strong), shadow(lg), overflow_hidden,
          text_color(k->text)) {
        // a press on the card is not one on what is behind it.
        button();

        search_row(app);
        chips(app);
        pressed = list(app, &s.places, view);
        footer(app, s.places.len);
      }
    }
  }

  if (entered && count) {
    pressed = &s.places.v[app->launcher_pick];
  }

  entered = false;

  if (pressed) {
    open_place(app, pressed);
  }
  else if (close) {
    hide(app);
  }

  free(s.places.v);
}
