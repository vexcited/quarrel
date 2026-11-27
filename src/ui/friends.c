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

        tag_chip(app, u);
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

    icon(mingcute, chat_3_fill, size(5), flex_shrink_0, mr(2),
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

// the client's tab in the primary color, light while it is open.
static void add_friend_tab(App *app) {
  const Look *k = &app->look;
  bool on = app->friends_tab == FRIENDS_ADD;
  civue_color ground = on ? civue_alpha(k->primary, .16f) : k->primary;

  div(key(id_at(id("friends-tab"), FRIENDS_ADD)), flex, items_center,
      flex_shrink_0, h(8), px(3), rounded(lg), font_weight(medium),
      text_color(on ? k->accent_text : k->on_primary), style(.bg = ground),
      hover(bg(on ? ground : k->primary_hover)),
      active(bg(on ? ground : k->primary_active)), cursor_pointer) {
    if (button().activated) {
      app->friends_tab = FRIENDS_ADD;
    }

    text("Add Friend");
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
/* add friend.                                                                */
/* ========================================================================== */

constexpr size_t NOTE_MAX = 120;

// the length the client counts, in utf-16 units.
static size_t note_length(const char *text) {
  size_t n = 0;

  for (const unsigned char *c = (const unsigned char *)text; c && *c; c++) {
    if ((*c & 0xc0) != 0x80) {
      n += *c >= 0xf0 ? 2 : 1;
    }
  }

  return n;
}

// cuts the note where the client's field stops taking characters.
static void clamp_note(char *text) {
  size_t n = 0;

  for (unsigned char *c = (unsigned char *)text; c && *c; c++) {
    if ((*c & 0xc0) == 0x80) {
      continue;
    }

    n += *c >= 0xf0 ? 2 : 1;

    if (n > NOTE_MAX) {
      *c = 0;
      return;
    }
  }
}

static civue_field_look bare_field(const Look *k) {
  civue_field_box box = {.text = k->text};

  return (civue_field_look){
    .editing = box,
    .idle = box,
    .hovered = box,
    .placeholder = k->muted,
    .selection = k->selection,
  };
}

static bool send_button(const Look *k, bool enabled) {
  bool pressed = false;
  civue_color ground = enabled ? k->primary : civue_alpha(k->primary, .5f);

  div(key(id("add-friend-send")), flex, items_center, justify_center,
      flex_shrink_0, h(8), px(3), rounded(lg), bg(ground),
      text_color(enabled ? k->on_primary
                         : civue_alpha(k->on_primary, .5f)),
      text_size(px(14)), line_height(px(16)), font_weight(medium),
      style(.hover = {.bg = k->primary_hover, .has_bg = enabled},
            .active = {.bg = k->primary_active, .has_bg = enabled},
            .cursor = enabled ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    pressed = button().activated && enabled;
    text("Send Friend Request");
  }

  return pressed;
}

// the challenge discord holds the request back for, where the client opens
// its captcha modal.
static void friend_challenge(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  const SessionCaptcha *asked = &session->friend_captcha;

  if (!asked->service) {
    captcha_drop(&app->friend_captcha);
    return;
  }

  if (strcmp(asked->service, "hcaptcha") || !asked->sitekey) {
    text("Discord asks for a challenge Quarrel cannot show.", mt(2),
         text_size(px(14)), line_height(px(18)),
         text_color(k->critical_text));
    return;
  }

  captcha_start(&app->friend_captcha, asked->sitekey, asked->rqdata);

  hcaptcha_state state =
    captcha_node(app, &app->friend_captcha, id("add-friend-captcha"));

  if (state.status == HCAPTCHA_SOLVED && state.token) {
    session_add_friend_captcha(session, state.token);
    captcha_drop(&app->friend_captcha);
  }
  else if (state.status == HCAPTCHA_CANCELLED ||
           state.status == HCAPTCHA_EXPIRED ||
           state.status == HCAPTCHA_FAILED) {
    captcha_drop(&app->friend_captcha);
    session_add_friend_clear(session);
  }
}

static void add_friend(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  civue_ui *ui = civue_ui_current();
  civue_id tag_id = id("add-friend-tag");
  const char *told = session->friend_message;
  bool sent = told && session->friend_sent;
  bool failed = told && !session->friend_sent;
  bool submit = false;

  // a request that went through empties the form for the next one.
  if (sent && (app->friend_tag || app->friend_note)) {
    free(app->friend_tag);
    free(app->friend_note);
    app->friend_tag = nullptr;
    app->friend_note = nullptr;
    app->friend_ready = false;
    ui->focus = tag_id;
  }

  civue_color edge = sent                  ? k->online
                     : failed              ? k->critical_text
                     : app->friend_focused ? k->ring
                                           : k->border_strong;
  size_t length = note_length(app->friend_note);
  char left[8];

  snprintf(left, sizeof left, "%zu", NOTE_MAX - length);

  div(key(id("add-friend")), flex, flex_col, flex_1, min_w(0), h(full),
      overflow_y_scroll, scrollbars(&app->bars)) {
    div(flex, flex_col, flex_shrink_0, px(px(30)), py(px(20)), border_b(1),
        border_color(k->border)) {
      text("Add Friend", mb(2), text_size(px(20)), line_height(px(24)),
           font_weight(semibold), text_color(k->strong));
      text("You can add friends with their Discord username.",
           text_size(px(14)), line_height(px(18)), font_weight(medium),
           text_color(k->text));

      div(flex, flex_col, mt(4), px(3), rounded(px(16)),
          bg(failed ? civue_alpha(k->danger, .04f) : k->field),
          border(1), border_color(edge)) {
        div(flex, items_center, gap(4), py(px(2))) {
          div(key(tag_id), flex_1, min_w(0), h(px(40)), text_size(px(16)),
              line_height(px(20))) {
            civue_field_response field =
              text_field(&app->friend_tag, "Enter a username", bare_field(k));

            app->friend_focused = field.focused;

            // typing starts over: what was told goes, the button wakes up.
            if (field.changed) {
              app->friend_ready = app->friend_tag && *app->friend_tag;
              session_add_friend_clear(session);
            }

            submit |= field.submitted && app->friend_ready;
          }

          submit |= send_button(k, app->friend_ready &&
                                     !session->friend_adding);
        }

        div(flex, flex_col, py(3), border_t(1), border_color(k->border)) {
          div(key(id("add-friend-note")), h(px(18)), text_size(px(14)),
              line_height(px(18))) {
            civue_field_response field =
              text_field(&app->friend_note,
                         "Personalize your request (Optional)", bare_field(k));

            if (field.changed) {
              clamp_note(app->friend_note);
            }
          }

          text(left, self_end, pt(1), text_size(px(14)), line_height(px(18)),
               text_color(length >= NOTE_MAX ? k->critical_text : k->subtle));
        }
      }

      if (told) {
        text(told, mt(2), text_size(px(14)), line_height(px(18)),
             text_color(sent ? k->positive_text : k->critical_text));
      }
      else {
        text("What you write here will also show up in your DM if you "
             "become friends.",
             mt(2), text_size(px(14)), line_height(px(18)),
             text_color(k->subtle));
      }

      friend_challenge(app);
    }
  }

  if (submit && !session->friend_adding) {
    session_add_friend(session, app->friend_tag, app->friend_note);
  }
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

// `badge` is the url of a small picture at the end, empty for none.
static void card_header(App *app, const User *u, const char *title,
                        const char *subtitle, const char *badge) {
  const Look *k = &app->look;

  div(flex, items_center, gap(3), min_w(0)) {
    avatar(app, u, 0, 32, true, k->surface);

    div(flex, flex_col, flex_1, min_w(0)) {
      text(title, truncate, font_weight(semibold), line_height(px(20)),
           text_color(k->strong));
      text(subtitle, truncate, text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));
    }

    if (*badge) {
      div(size(px(24)), flex_shrink_0, rounded(md), overflow_hidden) {
        image_fill(app, badge, 6);
      }
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
         font_weight(bold), text_color(k->on_danger));
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
    card_header(app, g->first, g->title, "In a Voice Channel", "");

    div(flex, items_center, gap(3), min_w(0)) {
      section_icon(app, url, CIVUE_ICON(mingcute, volume_fill));

      div(flex, flex_col, flex_1, min_w(0)) {
        text(guild && guild->name ? guild->name : "Voice", truncate,
             text_size(px(14)), line_height(px(18)), font_weight(semibold),
             text_color(k->strong));

        div(flex, items_center, gap(1), min_w(0)) {
          icon(mingcute, volume_fill, size(3), flex_shrink_0, text_color(k->subtle));
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
  char subtitle[200], span[16], elapsed[32];
  char url[CDN_URL_MAX], badge[CDN_URL_MAX];
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

  cdn_application_icon(
    badge, a->application_id,
    session_application_icon(app->session, a->application_id), 64);
  cdn_activity_image(url, a->application_id, a->large_image, 64);

  // discord shows the application's own icon when the activity has no
  // picture.
  if (!*url) {
    snprintf(url, sizeof url, "%s", badge);
  }

  div(key(id_at(id("now-activity"), u->id)), flex, flex_col, gap(4), p(4),
      mb(4), rounded(xl), bg(k->surface), border(1), border_color(k->border)) {
    card_header(app, u, friend_name(r, u), subtitle, badge);

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

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
        border_b(1), border_color(k->border)) {
      window_handle(id("friends-header"));

      icon(mingcute, group_fill, size(5), text_color(k->faint));
      text("Friends", mr(2), font_weight(semibold), text_color(k->strong));

      div(size(1), mr(2), rounded(full), bg(k->border_strong)) {}

      tab(app, FRIENDS_ONLINE, "Online");
      tab(app, FRIENDS_ALL, "All");
      tab(app, FRIENDS_IMPLICIT, "Implicit");
      add_friend_tab(app);

      header_end(app);
    }

    div(flex, flex_1, min_h(0)) {
      if (app->friends_tab == FRIENDS_ADD) {
        add_friend(app);
      }
      else {
        friends_list(app);
      }

      active_now(app);
    }
  }
}
