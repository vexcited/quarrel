#include "components.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

constexpr float ROW_HEIGHT = 62;
constexpr float NOW_WIDTH = 360;
constexpr size_t GROUPS_MAX = 24;
constexpr size_t CARDS_MAX = 40;
constexpr size_t FACES_MAX = 3;

typedef struct {
  const Relationship *relation;
  const User *user;
  const char *name;
} Friend;

// the friends sharing one voice channel.
typedef struct {
  Snowflake guild_id, channel_id;
  const User *first;
  char title[160];
  size_t count;
} VoiceGroup;

/* ========================================================================== */
/* the list.                                                                  */
/* ========================================================================== */

static const char *friend_name(const Relationship *r, const User *u) {
  return r && r->nickname && *r->nickname ? r->nickname : store_user_name(u);
}

static bool contains(const char *text, const char *needle) {
  if (!text) {
    return false;
  }

  size_t len = strlen(needle);

  for (; *text; text++) {
    if (strncasecmp(text, needle, len) == 0) {
      return true;
    }
  }

  return false;
}

static int by_name(const void *a, const void *b) {
  const Friend *x = a, *y = b;
  int order = strcasecmp(x->name, y->name);

  if (order) {
    return order;
  }

  return (x->user->id > y->user->id) - (x->user->id < y->user->id);
}

static bool in_tab(FriendsTab tab, const Relationship *r, const User *u) {
  if (tab == FRIENDS_ONLINE) {
    return r->type == RELATIONSHIP_FRIEND && u->status != STATUS_OFFLINE;
  }

  return r->type == RELATIONSHIP_FRIEND;
}

static bool searched(const App *app, const char *name, const User *u) {
  const char *search = app->friends_search;

  return !search || !*search || contains(name, search) ||
         contains(u->username, search);
}

// who the tab and the search keep, from malloc. the implicit tab lists the
// affinities discord computed, friends or not, closest first. only the ones
// the store has a name for can be shown.
static Friend *collect(App *app, size_t *count) {
  Store *store = app->session->store;
  bool implicit = app->friends_tab == FRIENDS_IMPLICIT;
  size_t total = implicit ? store->naffinities : store->nrelationships;
  Friend *friends = calloc(total + 1, sizeof *friends);
  size_t n = 0;

  *count = 0;

  if (!friends) {
    return nullptr;
  }

  for (size_t i = 0; i < total; i++) {
    Snowflake id =
      implicit ? store->affinities[i] : store->relationships[i].user_id;
    const Relationship *r =
      implicit ? store_relationship(store, id) : &store->relationships[i];
    const User *u = store_user(store, id);

    if (!u || !u->username || (!implicit && !in_tab(app->friends_tab, r, u))) {
      continue;
    }

    const char *name = friend_name(r, u);

    if (searched(app, name, u)) {
      friends[n++] = (Friend){r, u, name};
    }
  }

  if (!implicit) {
    qsort(friends, n, sizeof *friends, by_name);
  }

  *count = n;
  return friends;
}

static const char *status_label(UserStatus status) {
  switch (status) {
    case STATUS_ONLINE:
      return "Online";
    case STATUS_IDLE:
      return "Idle";
    case STATUS_DND:
      return "Do Not Disturb";
    default:
      return "Offline";
  }
}

static void tag_chip(App *app, const User *u) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_tag_badge(url, u->tag_guild_id, u->tag_badge, 32);

  div(flex, items_center, gap(1), flex_shrink_0, h(px(18)), px(1), rounded(sm),
      bg(k->hover)) {
    if (*url) {
      div(size(px(12)), flex_shrink_0) {
        image_fill(app, url, 0);
      }
    }

    text(u->tag, text_size(px(12)), line_height(px(16)), font_weight(semibold),
         text_color(k->text), whitespace_nowrap);
  }
}

static civue_icon activity_icon(ActivityType type) {
  switch (type) {
    case ACTIVITY_LISTENING:
      return CIVUE_ICON(lucide, music);
    case ACTIVITY_WATCHING:
      return CIVUE_ICON(lucide, tv);
    case ACTIVITY_STREAMING:
      return CIVUE_ICON(lucide, radio);
    case ACTIVITY_COMPETING:
      return CIVUE_ICON(lucide, trophy);
    default:
      return CIVUE_ICON(lucide, gamepad_2);
  }
}

static void friend_row(App *app, const Friend *f) {
  const Look *k = &app->look;
  const User *u = f->user;
  char made[256];
  const char *doing =
    u->nactivities && u->status != STATUS_OFFLINE
      ? activity_status_text(&u->activities[0], made, sizeof made)
      : nullptr;

  div(key(id_at(id("friend"), u->id)), flex, items_center, gap(3),
      h(px(ROW_HEIGHT)), px(2), rounded(lg), border_t(1),
      border_color(k->border), hover(bg(k->hover)), cursor_pointer) {
    if (button().activated) {
      session_open_dm(app->session, u->id);
    }

    avatar(app, u, 0, 32, true, k->chat);

    div(flex, flex_col, flex_1, min_w(0)) {
      div(flex, items_center, gap(2), min_w(0)) {
        text(f->name, min_w(0), truncate, font_weight(semibold),
             text_color(k->strong));

        if (u->tag && *u->tag) {
          tag_chip(app, u);
        }
      }

      // what the friend is doing comes before the status they wrote, which
      // follows it after a dot.
      if (doing) {
        div(flex, items_center, gap(1), min_w(0), text_size(px(14)),
            line_height(px(18)), text_color(k->subtle)) {
          glyph(activity_icon(u->activities[0].type), 16, k->online);
          text(doing, flex_shrink_0, max_w(2_3), truncate);

          if (u->activity && u->custom_status) {
            text("•", flex_shrink_0, text_color(k->faint));
            text(u->activity, min_w(0), truncate);
          }
        }
      }
      else if (u->activity && u->custom_status) {
        text(u->activity, truncate, text_size(px(14)), line_height(px(18)),
             text_color(k->subtle));
      }
      else {
        text(status_label(u->status), truncate, text_size(px(14)),
             line_height(px(18)), text_color(k->subtle));
      }
    }

    icon(lucide, message_circle, size(5), flex_shrink_0, mr(2),
         text_color(k->subtle));
  }
}

static void tab(App *app, FriendsTab which, const char *label) {
  const Look *k = &app->look;
  bool on = app->friends_tab == which;

  div(key(id_at(id("friends-tab"), which)), flex, items_center, flex_shrink_0,
      h(8), px(3), rounded(lg), font_weight(medium),
      text_color(on ? k->strong : k->subtle),
      style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(on ? k->selected : k->hover), text_color(k->text)),
      cursor_pointer) {
    if (button().activated) {
      app->friends_tab = which;
    }

    text(label);
  }
}

static void friends_list(App *app) {
  static const char *const titles[] = {"Online", "All friends", "Implicit"};

  const Look *k = &app->look;
  size_t count = 0;
  Friend *friends = collect(app, &count);
  char title[48];

  snprintf(title, sizeof title, "%s — %zu", titles[app->friends_tab], count);

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    div(key(id("friends-search")), flex_shrink_0, h(10), mx(5), mt(4)) {
      text_field(&app->friends_search, "Search", field_look(k));
    }

    text(title, flex_shrink_0, mx(5), mt(5), mb(3), text_size(px(14)),
         line_height(px(18)), font_weight(medium), text_color(k->text));

    // each tab scrolls on its own.
    div(key(id_at(id("friends-list"), app->friends_tab)), flex_1, min_h(0),
        overflow_y_scroll, scrollbars(&app->bars), px(3), pb(4)) {
      rows((uint32_t)count, ROW_HEIGHT, i) {
        friend_row(app, &friends[i]);
      }
    }
  }

  free(friends);
}

/* ========================================================================== */
/* active now.                                                                */
/* ========================================================================== */

static void on_tick(void *ctx) {
  App *app = ctx;

  app->friends_tick = nullptr;
  civue_window_wake();
}

// "2h", "26m", nothing under a minute.
static void format_span(int64_t since_ms, char *out, size_t cap) {
  int64_t minutes = ((int64_t)(platform_now() * 1000) - since_ms) / 60000;

  if (since_ms <= 0 || minutes < 1) {
    out[0] = 0;
  }
  else if (minutes < 60) {
    snprintf(out, cap, "%lldm", (long long)minutes);
  }
  else if (minutes < 60 * 24) {
    snprintf(out, cap, "%lldh", (long long)(minutes / 60));
  }
  else {
    snprintf(out, cap, "%lldd", (long long)(minutes / (60 * 24)));
  }
}

static void format_elapsed(int64_t since_ms, char *out, size_t cap) {
  int64_t seconds = ((int64_t)(platform_now() * 1000) - since_ms) / 1000;

  if (seconds < 0) {
    seconds = 0;
  }

  if (seconds >= 3600) {
    snprintf(out, cap, "%02lld:%02lld:%02lld elapsed",
             (long long)(seconds / 3600), (long long)(seconds / 60 % 60),
             (long long)(seconds % 60));
  }
  else {
    snprintf(out, cap, "%02lld:%02lld elapsed", (long long)(seconds / 60),
             (long long)(seconds % 60));
  }
}

static void card_header(App *app, const User *u, const char *title,
                        const char *subtitle) {
  const Look *k = &app->look;

  div(flex, items_center, gap(3), min_w(0)) {
    avatar(app, u, 0, 32, true, k->surface);

    div(flex, flex_col, flex_1, min_w(0)) {
      text(title, truncate, font_weight(semibold), line_height(px(20)),
           text_color(k->strong));
      text(subtitle, truncate, text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));
    }
  }
}

static void section_icon(App *app, const char *url, civue_icon fallback) {
  const Look *k = &app->look;

  div(flex, items_center, justify_center, size(px(32)), flex_shrink_0,
      rounded(lg), bg(k->hover)) {
    if (!*url || !image_fill(app, url, 8)) {
      glyph(fallback, 18, k->subtle);
    }
  }
}

static void live_badge(const Look *k) {
  div(flex, items_center, flex_shrink_0, h(4), px(1), rounded(full),
      bg(k->danger)) {
    text("LIVE", px(1), text_size(px(12)), line_height(px(16)),
         font_weight(bold), text_color(CIVUE_WHITE));
  }
}

// who is in the channel, friends or not: a few faces and how many more.
static void faces(App *app, const VoiceGroup *g) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  size_t shown = 0, total = 0;

  div(flex, items_center, gap(1), flex_shrink_0) {
    for (size_t i = 0; i < store->nvoice_states; i++) {
      const VoiceState *v = &store->voice_states[i];

      if (v->channel_id != g->channel_id) {
        continue;
      }

      total++;

      if (shown < FACES_MAX) {
        avatar(app, store_user(store, v->user_id), g->guild_id, 24, false,
               k->surface);
        shown++;
      }
    }

    if (total > shown) {
      char more[16];

      snprintf(more, sizeof more, "+%zu", total - shown);

      div(flex, items_center, justify_center, h(6), min_w(6), px(1),
          rounded(full), bg(k->hover)) {
        text(more, text_size(px(12)), line_height(px(16)),
             font_weight(semibold), text_color(k->text));
      }
    }
  }
}

static void voice_card(App *app, const VoiceGroup *g) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, g->guild_id);
  const Channel *channel = store_channel(store, g->channel_id);
  char url[CDN_URL_MAX] = "";

  if (guild) {
    cdn_guild_icon(url, guild->id, guild->icon, 64);
  }

  div(key(id_at(id("now-voice"), g->channel_id)), flex, flex_col, gap(4), p(4),
      mb(4), rounded(xl), bg(k->surface), border(1), border_color(k->border)) {
    card_header(app, g->first, g->title, "In a Voice Channel");

    div(flex, items_center, gap(3), min_w(0)) {
      section_icon(app, url, CIVUE_ICON(lucide, volume_2));

      div(flex, flex_col, flex_1, min_w(0)) {
        text(guild && guild->name ? guild->name : "Voice", truncate,
             text_size(px(14)), line_height(px(18)), font_weight(semibold),
             text_color(k->strong));

        div(flex, items_center, gap(1), min_w(0)) {
          icon(lucide, volume_2, size(3), flex_shrink_0, text_color(k->subtle));
          text(channel && channel->name ? channel->name : "Voice Channel",
               min_w(0), truncate, text_size(px(12)), line_height(px(16)),
               text_color(k->subtle));
        }
      }

      faces(app, g);
    }

    for (size_t i = 0; i < store->nvoice_states; i++) {
      const VoiceState *v = &store->voice_states[i];
      const Relationship *r = store_relationship(store, v->user_id);
      const User *u = store_user(store, v->user_id);

      if (v->channel_id != g->channel_id || !v->streaming || !u || !r ||
          r->type != RELATIONSHIP_FRIEND) {
        continue;
      }

      div(flex, items_center, gap(3), min_w(0)) {
        avatar(app, u, 0, 32, false, k->surface);

        div(flex, flex_col, flex_1, min_w(0)) {
          text(friend_name(r, u), truncate, text_size(px(14)),
               line_height(px(18)), font_weight(semibold),
               text_color(k->strong));
          text("Sharing their screen", truncate, text_size(px(12)),
               line_height(px(16)), text_color(k->subtle));
        }

        live_badge(k);
      }
    }
  }
}

// true when the card counts seconds, so that the page keeps redrawing.
static bool activity_card(App *app, const Relationship *r, const User *u) {
  static const char *const verbs[] = {
    nullptr, "Streaming", "Listening to", "Watching", nullptr, "Competing in",
  };

  const Look *k = &app->look;
  const Activity *a = &u->activities[0];
  char subtitle[200], span[16], elapsed[32], url[CDN_URL_MAX];
  bool rich = a->details || a->state || a->large_image || a->started_ms;

  format_span(a->started_ms ? a->started_ms : a->created_ms, span, sizeof span);

  if (verbs[a->type]) {
    snprintf(subtitle, sizeof subtitle, "%s %s", verbs[a->type], a->name);
  }
  else if (*span) {
    snprintf(subtitle, sizeof subtitle, "%s – %s", a->name, span);
  }
  else {
    snprintf(subtitle, sizeof subtitle, "%s", a->name);
  }

  cdn_activity_image(url, a->application_id, a->large_image, 64);

  div(key(id_at(id("now-activity"), u->id)), flex, flex_col, gap(4), p(4),
      mb(4), rounded(xl), bg(k->surface), border(1), border_color(k->border)) {
    card_header(app, u, friend_name(r, u), subtitle);

    if (rich) {
      div(flex, items_center, gap(3), min_w(0)) {
        section_icon(app, url, activity_icon(a->type));

        div(flex, flex_col, flex_1, min_w(0)) {
          text(a->details ? a->details : a->name, truncate, text_size(px(14)),
               line_height(px(18)), font_weight(semibold),
               text_color(k->strong));

          if (a->state) {
            text(a->state, truncate, text_size(px(12)), line_height(px(16)),
                 text_color(k->subtle));
          }

          if (a->started_ms) {
            format_elapsed(a->started_ms, elapsed, sizeof elapsed);
            text(elapsed, truncate, text_size(px(12)), line_height(px(16)),
                 text_color(k->subtle));
          }
        }
      }
    }
  }

  return a->started_ms != 0;
}

static void add_to_group(VoiceGroup *groups, size_t *ngroups,
                         const VoiceState *v, const Relationship *r,
                         const User *u) {
  VoiceGroup *g = nullptr;

  for (size_t i = 0; i < *ngroups; i++) {
    if (groups[i].channel_id == v->channel_id) {
      g = &groups[i];
    }
  }

  if (!g) {
    if (*ngroups == GROUPS_MAX) {
      return;
    }

    g = &groups[(*ngroups)++];
    *g = (VoiceGroup){
      .guild_id = v->guild_id,
      .channel_id = v->channel_id,
      .first = u,
    };
  }

  // "a", "a and b", then "a, b, and 3 others" once the count is known.
  if (g->count < 2) {
    size_t len = strlen(g->title);

    snprintf(g->title + len, sizeof g->title - len, "%s%s", len ? ", " : "",
             friend_name(r, u));
  }

  g->count++;
}

static void now_empty(const Look *k) {
  div(flex, flex_col, items_center, gap(1), px(4), py(6)) {
    text("It's quiet for now...", font_weight(semibold), text_color(k->strong));
    text("When a friend starts an activity, like playing a game or hanging "
         "out on voice, we'll show it here!",
         text_center, text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));
  }
}

static void active_now(App *app) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  VoiceGroup groups[GROUPS_MAX];
  size_t ngroups = 0, cards = 0;
  bool counting = false;

  for (size_t i = 0; i < store->nrelationships; i++) {
    const Relationship *r = &store->relationships[i];
    const User *u = store_user(store, r->user_id);
    const VoiceState *v = store_voice_state(store, r->user_id);

    if (r->type == RELATIONSHIP_FRIEND && u && v && v->guild_id) {
      add_to_group(groups, &ngroups, v, r, u);
    }
  }

  div(key(id("active-now")), flex_shrink_0, w(px(NOW_WIDTH)), h(full),
      border_l(1), border_color(k->border), overflow_y_scroll,
      scrollbars(&app->thin_bars), px(4), pb(4)) {
    text("Active Now", mt(4), mb(4), text_size(px(20)), line_height(px(24)),
         font_weight(bold), text_color(k->strong));

    for (size_t i = 0; i < ngroups; i++) {
      VoiceGroup *g = &groups[i];

      if (g->count > 2) {
        size_t len = strlen(g->title);

        snprintf(g->title + len, sizeof g->title - len, ", and %zu other%s",
                 g->count - 2, g->count == 3 ? "" : "s");
      }
      else if (g->count == 2) {
        char *comma = strstr(g->title, ", ");
        char rest[160];

        // two names read "a and b".
        if (comma) {
          snprintf(rest, sizeof rest, "%s", comma + 2);
          snprintf(comma, sizeof g->title - (size_t)(comma - g->title),
                   " and %s", rest);
        }
      }

      voice_card(app, g);
      cards++;
    }

    for (size_t i = 0; i < store->nrelationships && cards < CARDS_MAX; i++) {
      const Relationship *r = &store->relationships[i];
      const User *u = store_user(store, r->user_id);
      const VoiceState *v = store_voice_state(store, r->user_id);

      if (r->type != RELATIONSHIP_FRIEND || !u || !u->nactivities ||
          u->status == STATUS_OFFLINE || (v && v->guild_id)) {
        continue;
      }

      counting |= activity_card(app, r, u);
      cards++;
    }

    if (!cards) {
      now_empty(k);
    }
  }

  // a counter of seconds redraws every second, the spans of minutes and
  // hours every half minute.
  if (cards && !app->friends_tick) {
    app->friends_tick = platform_timer(counting ? 1 : 30, on_tick, app);
  }
}

void friends_page(App *app) {
  const Look *k = &app->look;

  div(flex, flex_col, flex_1, min_w(0), h(full), bg(k->chat)) {
    div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
        border_b(1), border_color(k->border)) {
      icon(lucide, users, size(5), text_color(k->faint));
      text("Friends", mr(2), font_weight(semibold), text_color(k->strong));

      div(size(1), mr(2), rounded(full), bg(k->border_strong)) {}

      tab(app, FRIENDS_ONLINE, "Online");
      tab(app, FRIENDS_ALL, "All");
      tab(app, FRIENDS_IMPLICIT, "Implicit");
    }

    div(flex, flex_1, min_h(0)) {
      friends_list(app);
      active_now(app);
    }
  }
}
