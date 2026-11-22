#include "discord/store.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "discord/cdn.h"

// how long a typing indicator lasts without news, in seconds.
constexpr double TYPING_SECONDS = 10;
// the messages kept in a channel nobody opened yet.
constexpr size_t UNOPENED_MESSAGES = 50;
constexpr size_t MAX_SUBJECT_ROLES = 256;

#define MAP_PUT(map, key, ...) \
  CILO_MAP_PUT(quarrel_libc, map, key __VA_OPT__(, ) __VA_ARGS__)
#define MAP_DROP(map) CILO_MAP_DROP(quarrel_libc, map)

static void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count != 0 ? count : 1, size);

  if (p == nullptr) {
    cilo_oom(count * size);
  }

  return p;
}

// room for `need` items of `size` bytes in the array at `*items`.
static void reserve(void *items, size_t *cap, size_t need, size_t size) {
  if (need <= *cap) {
    return;
  }

  size_t grown = *cap != 0 ? *cap * 2 : 8;

  while (grown < need) {
    grown *= 2;
  }

  void **slot = items;
  void *moved = realloc(*slot, grown * size);

  if (moved == nullptr) {
    cilo_oom(grown * size);
  }

  *slot = moved;
  *cap = grown;
}

// an array of exactly `count` items, for the ones that rarely change.
static void *resized(void *items, size_t count, size_t size) {
  if (count == 0) {
    free(items);
    return nullptr;
  }

  void *moved = realloc(items, count * size);

  if (moved == nullptr) {
    cilo_oom(count * size);
  }

  return moved;
}

// sets `*dst` from the member `key` when `obj` has it, a json null clearing
// it. true when it changed.
static bool put_str(char **dst, const cima_json *obj, const char *key) {
  const cima_json *v = json_get(obj, key);

  if (v == nullptr) {
    return false;
  }

  const char *next = json_or(v, nullptr);
  bool same = (*dst == nullptr && next == nullptr) ||
              (*dst != nullptr && next != nullptr && strcmp(*dst, next) == 0);

  if (same) {
    return false;
  }

  str_set(dst, next);
  return true;
}

// `avatar_decoration_data` of a user or a member, null when they wear
// none. true when it changed.
static bool put_decoration(char **dst, const cima_json *owner) {
  const cima_json *data = json_get(owner, "avatar_decoration_data");

  if (data == nullptr) {
    return false;
  }

  if (json_type(data) == CIMA_JSON_OBJECT) {
    return put_str(dst, data, "asset");
  }

  bool worn = *dst != nullptr;

  free(*dst);
  *dst = nullptr;
  return worn;
}

static bool put_bool(bool *dst, const cima_json *obj, const char *key) {
  const cima_json *v = json_get(obj, key);

  if (v == nullptr) {
    return false;
  }

  bool next = json_coerce(v, false);
  bool changed = *dst != next;
  *dst = next;
  return changed;
}

static bool put_id(Snowflake *dst, const cima_json *obj, const char *key) {
  const cima_json *v = json_get(obj, key);

  if (v == nullptr) {
    return false;
  }

  Snowflake next = snowflake_of(v);
  bool changed = *dst != next;
  *dst = next;
  return changed;
}

// the ids of an array of snowflakes, or of objects when `key` names their
// id. from malloc.
static Snowflake *id_array(const cima_json *array, const char *key,
                           size_t *count) {
  size_t n = (json_type(array) == CIMA_JSON_ARRAY) ? json_len(array) : 0;
  Snowflake *ids = n != 0 ? xcalloc(n, sizeof *ids) : nullptr;
  size_t kept = 0;

  CIMA_EACH(item, json_items(array)) {
    Snowflake id =
      key != nullptr ? snowflake_of(json_get(item, key)) : snowflake_of(item);

    if (id != 0 && kept < n) {
      ids[kept++] = id;
    }
  }

  *count = kept;
  return ids;
}

double store_now(const Store *s) {
  if (s->clock != 0) {
    return s->clock;
  }

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int64_t now_ms(const Store *s) {
  return (int64_t)(store_now(s) * 1000);
}

void store_touch_guild(Store *s, Guild *g) {
  s->version++;

  if (g != nullptr) {
    g->version++;
  }
}

void store_touch_channel(Store *s, Channel *c) {
  c->version++;

  if (c->guild_id != 0) {
    store_touch_guild(s, store_guild(s, c->guild_id));
  }
  else {
    s->version++;
  }
}

static void touch_layout(Store *s, Guild *g) {
  g->layout_version++;
  store_touch_guild(s, g);
}

/* ========================================================================== */
/* lookups.                                                                   */
/* ========================================================================== */

User *store_user(const Store *s, Snowflake id) {
  User **found = CILO_MAP_GET(&s->users, id);
  return found != nullptr ? *found : nullptr;
}

Guild *store_guild(const Store *s, Snowflake id) {
  Guild **found = CILO_MAP_GET(&s->guilds, id);
  return found != nullptr ? *found : nullptr;
}

Channel *store_channel(const Store *s, Snowflake id) {
  Channel **found = CILO_MAP_GET(&s->channels, id);
  return found != nullptr ? *found : nullptr;
}

Role *store_role(const Guild *g, Snowflake id) {
  if (g == nullptr) {
    return nullptr;
  }

  for (size_t i = 0; i < g->nroles; i++) {
    if (g->roles[i]->id == id) {
      return g->roles[i];
    }
  }

  return nullptr;
}

Member *store_member(const Guild *g, Snowflake user_id) {
  if (g == nullptr) {
    return nullptr;
  }

  Member **found = CILO_MAP_GET(&g->members, user_id);
  return found != nullptr ? *found : nullptr;
}

Emoji *store_emoji(const Guild *g, Snowflake id) {
  if (g == nullptr) {
    return nullptr;
  }

  for (size_t i = 0; i < g->nemojis; i++) {
    if (g->emojis[i].id == id) {
      return &g->emojis[i];
    }
  }

  return nullptr;
}

// where the message `id` is or would be inserted in the channel.
static size_t message_slot(const Channel *c, Snowflake id) {
  size_t low = 0, high = c->nmessages;

  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (c->messages[mid]->id < id) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }

  return low;
}

Message *store_message(const Channel *c, Snowflake id) {
  if (c == nullptr || id == 0) {
    return nullptr;
  }

  size_t slot = message_slot(c, id);

  if (slot < c->nmessages && c->messages[slot]->id == id) {
    return c->messages[slot];
  }

  return nullptr;
}

const Relationship *store_relationship(const Store *s, Snowflake user_id) {
  for (size_t i = 0; i < s->nrelationships; i++) {
    if (s->relationships[i].user_id == user_id) {
      return &s->relationships[i];
    }
  }

  return nullptr;
}

const char *store_user_name(const User *u) {
  if (u == nullptr) {
    return "Unknown User";
  }

  if (u->global_name != nullptr && u->global_name[0] != '\0') {
    return u->global_name;
  }

  if (u->username != nullptr && u->username[0] != '\0') {
    return u->username;
  }

  return "Unknown User";
}

const char *store_display_name(const Store *s, Snowflake guild_id,
                               Snowflake user_id) {
  if (guild_id != 0) {
    const Member *m = store_member(store_guild(s, guild_id), user_id);

    if (m != nullptr && m->nick != nullptr && m->nick[0] != '\0') {
      return m->nick;
    }
  }
  else {
    const Relationship *r = store_relationship(s, user_id);

    if (r != nullptr && r->nickname != nullptr && r->nickname[0] != '\0') {
      return r->nickname;
    }
  }

  return store_user_name(store_user(s, user_id));
}

static uint32_t member_color(const Guild *g, Member *m) {
  if (m->color_stamp == g->members_version + 1) {
    return m->color;
  }

  uint32_t color = 0;

  // roles are kept highest first, so the first colored one a member has wins.
  for (size_t i = 0; i < g->nroles && color == 0; i++) {
    const Role *role = g->roles[i];

    if (role->color == 0) {
      continue;
    }

    for (size_t j = 0; j < m->nroles; j++) {
      if (m->roles[j] == role->id) {
        color = role->color;
        break;
      }
    }
  }

  m->color = color;
  m->color_stamp = g->members_version + 1;
  return color;
}

uint32_t store_member_color(const Store *s, Snowflake guild_id,
                            Snowflake user_id) {
  const Guild *g = store_guild(s, guild_id);
  Member *m = store_member(g, user_id);
  return m != nullptr ? member_color(g, m) : 0;
}

Author store_author(const Store *s, const Message *m) {
  Author author = {.user = store_user(s, m->author_id)};
  const Channel *c = store_channel(s, m->channel_id);
  const Guild *g = c != nullptr ? store_guild(s, c->guild_id) : nullptr;

  // a webhook's name is its own, never a member's.
  if (g != nullptr && m->webhook_id == 0) {
    Member *member = store_member(g, m->author_id);

    if (member != nullptr) {
      author.member = member;
      author.color = member_color(g, member);
    }
  }

  Snowflake guild_id = g != nullptr && m->webhook_id == 0 ? g->id : 0;
  author.name = m->webhook_id != 0
                  ? store_user_name(author.user)
                  : store_display_name(s, guild_id, m->author_id);
  return author;
}

const char *store_channel_name(const Store *s, const Channel *c, char *out,
                               size_t cap) {
  if (cap == 0) {
    return "";
  }

  out[0] = '\0';

  if (c == nullptr) {
    snprintf(out, cap, "unknown");
    return out;
  }

  if (c->name != nullptr && c->name[0] != '\0') {
    snprintf(out, cap, "%s", c->name);
    return out;
  }

  size_t len = 0;

  for (size_t i = 0; i < c->nrecipients && len + 1 < cap; i++) {
    const char *name = store_display_name(s, 0, c->recipients[i]);
    int n = snprintf(out + len, cap - len, "%s%s", i != 0 ? ", " : "", name);

    if (n < 0) {
      break;
    }

    len += (size_t)n < cap - len ? (size_t)n : cap - len - 1;
  }

  if (out[0] == '\0') {
    snprintf(out, cap, "%s",
             c->type == CHANNEL_GROUP_DM ? "Unnamed" : "unknown");
  }

  return out;
}

/* ========================================================================== */
/* users, presences and relationships.                                        */
/* ========================================================================== */

static void free_activities(User *u) {
  for (size_t i = 0; i < u->nactivities; i++) {
    Activity *a = &u->activities[i];
    free(a->name);
    free(a->details);
    free(a->state);
    free(a->large_image);
  }

  free(u->activities);
  u->activities = nullptr;
  u->nactivities = 0;
}

static void free_user(User *u) {
  free_activities(u);
  free(u->tag);
  free(u->tag_badge);
  free(u->username);
  free(u->global_name);
  free(u->discriminator);
  free(u->avatar);
  free(u->banner);
  free(u->bio);
  free(u->pronouns);
  free(u->activity);
  free(u->decoration);
  free(u->status_text);
  free(u->status_emoji);
  free(u);
}

static User *ensure_user(Store *s, Snowflake id) {
  bool created = false;
  User **slot = MAP_PUT(&s->users, id, &created);

  if (created) {
    *slot = xcalloc(1, sizeof **slot);
    (*slot)->id = id;
    s->users_version++;
    s->version++;
  }

  return *slot;
}

User *store_put_user(Store *s, const cima_json *user) {
  Snowflake id = snowflake_of(json_get(user, "id"));

  if (id == 0) {
    return nullptr;
  }

  User *u = ensure_user(s, id);
  bool changed = false;

  changed |= put_str(&u->username, user, "username");
  changed |= put_str(&u->global_name, user, "global_name");
  changed |= put_str(&u->discriminator, user, "discriminator");
  changed |= put_str(&u->avatar, user, "avatar");
  changed |= put_str(&u->banner, user, "banner");

  changed |= put_decoration(&u->decoration, user);
  changed |= put_str(&u->bio, user, "bio");
  changed |= put_str(&u->pronouns, user, "pronouns");
  changed |= put_bool(&u->bot, user, "bot");

  const cima_json *accent = json_get(user, "accent_color");

  if (accent != nullptr) {
    u->has_accent = cima_json_is_number(accent);
    u->accent_color =
      (uint32_t)json_get_coerce(user, "accent_color", (uint64_t)0);
  }

  changed |= put_bool(&u->system, user, "system");

  if (json_get(user, "public_flags") != nullptr) {
    u->public_flags = json_get_coerce(user, "public_flags", (uint64_t)0);
  }

  const cima_json *tag = json_get(user, "primary_guild");

  if (tag != nullptr) {
    bool worn = json_get_coerce(tag, "identity_enabled", false);

    changed |= put_str(&u->tag, worn ? tag : nullptr, "tag");
    changed |= put_str(&u->tag_badge, worn ? tag : nullptr, "badge");
    u->tag_guild_id =
      worn ? snowflake_of(json_get(tag, "identity_guild_id")) : 0;
  }

  if (changed) {
    s->users_version++;
    s->version++;
  }

  return u;
}

static UserStatus parse_status(const char *status) {
  if (status == nullptr) {
    return STATUS_OFFLINE;
  }

  if (strcmp(status, "online") == 0) {
    return STATUS_ONLINE;
  }

  if (strcmp(status, "idle") == 0) {
    return STATUS_IDLE;
  }

  if (strcmp(status, "dnd") == 0) {
    return STATUS_DND;
  }

  return STATUS_OFFLINE;
}

// what an activity reads as under a name, from malloc or null.
static void set_custom_status(User *u, const char *words, const char *emoji,
                              Snowflake emoji_id) {
  bool none = (words == nullptr || *words == '\0') &&
              (emoji == nullptr || *emoji == '\0');

  str_set(&u->status_text, words != nullptr && *words ? words : nullptr);
  str_set(&u->status_emoji, emoji != nullptr && *emoji ? emoji : nullptr);
  u->status_emoji_id = none ? 0 : emoji_id;
}

static void put_custom_status(User *u, const cima_json *activities) {
  CIMA_EACH(activity, json_items(activities)) {
    if (json_get_coerce(activity, "type", (int64_t)-1) == ACTIVITY_CUSTOM) {
      const cima_json *emoji = json_get(activity, "emoji");

      set_custom_status(u, json_get_or(activity, "state", nullptr),
                        json_get_or(emoji, "name", nullptr),
                        snowflake_of(json_get(emoji, "id")));
      return;
    }
  }

  set_custom_status(u, nullptr, nullptr, 0);
}

static char *activity_text(const cima_json *activities) {
  static const char *const verbs[] = {
    "Playing", "Streaming", "Listening to", "Watching", nullptr, "Competing in",
  };

  const cima_json *shown = nullptr;

  // a custom status is shown before anything the user is doing.
  CIMA_EACH(activity, json_items(activities)) {
    int64_t type = json_get_coerce(activity, "type", (int64_t)-1);

    if (type == 4) {
      shown = activity;
      break;
    }

    if (shown == nullptr && type >= 0) {
      shown = activity;
    }
  }

  if (shown == nullptr) {
    return nullptr;
  }

  char text[256];
  int64_t type = json_get_coerce(shown, "type", (int64_t)0);

  if (type == 4) {
    const cima_json *emoji = json_get(shown, "emoji");
    const char *state = json_get_or(shown, "state", nullptr);
    const char *glyph = snowflake_of(json_get(emoji, "id")) == 0
                          ? json_get_or(emoji, "name", nullptr)
                          : nullptr;

    if (state == nullptr && glyph == nullptr) {
      return nullptr;
    }

    snprintf(text, sizeof text, "%s%s%s", glyph != nullptr ? glyph : "",
             glyph != nullptr && state != nullptr ? " " : "",
             state != nullptr ? state : "");
    return str_dup(text);
  }

  const char *name = json_get_or(shown, "name", nullptr);

  if (name == nullptr) {
    return nullptr;
  }

  const char *verb = type >= 0 && type <= 5 ? verbs[type] : nullptr;
  snprintf(text, sizeof text, "%s %s", verb != nullptr ? verb : "Playing",
           name);
  return str_dup(text);
}

static void put_activities(User *u, const cima_json *activities) {
  free_activities(u);

  CIMA_EACH(activity, json_items(activities)) {
    int64_t type = json_get_coerce(activity, "type", (int64_t)-1);

    if (type < 0 || type > ACTIVITY_COMPETING || type == ACTIVITY_CUSTOM ||
        json_get_or(activity, "name", nullptr) == nullptr) {
      continue;
    }

    u->activities =
      resized(u->activities, u->nactivities + 1, sizeof *u->activities);

    Activity *a = &u->activities[u->nactivities++];

    *a = (Activity){
      .type = (ActivityType)type,
      .name = str_dup(json_get_or(activity, "name", nullptr)),
      .details = str_dup(json_get_or(activity, "details", nullptr)),
      .state = str_dup(json_get_or(activity, "state", nullptr)),
      .application_id = snowflake_of(json_get(activity, "application_id")),
      .large_image = str_dup(
        json_get_or(json_get(activity, "assets"), "large_image", nullptr)),
      .started_ms =
        json_get_coerce(json_get(activity, "timestamps"), "start", (int64_t)0),
      .created_ms = json_get_coerce(activity, "created_at", (int64_t)0),
      .display =
        (uint8_t)json_get_coerce(activity, "status_display_type", (int64_t)0),
    };

    const char *party = json_get_or(json_get(activity, "party"), "id", nullptr);

    a->spotify = a->type == ACTIVITY_LISTENING && party != nullptr &&
                 strcmp(a->name, "Spotify") == 0 &&
                 strncmp(party, "spotify:", 8) == 0;
  }
}

static const char *filled(const char *text) {
  return text != nullptr && text[0] != '\0' ? text : nullptr;
}

// the client's own rule, from the function of its bundle that the friends
// list and the member list call:
//   a stream stands by its details, anything else by its name, unless
//   status_display_type picks the state or the details and they are set.
//   a game shows that. a song on spotify shows its artists, the state with
//   "; " turned into ", ". crunchyroll shows its details.
const char *activity_status_text(const Activity *a, char *out, size_t cap) {
  static constexpr Snowflake CRUNCHYROLL = 981509069309354054;

  const char *name = filled(a->name);
  const char *details = filled(a->details);
  const char *state = filled(a->state);
  const char *shown = name;

  if (a->type == ACTIVITY_STREAMING && details != nullptr) {
    shown = details;
  }

  if (a->display == 1 && state != nullptr) {
    shown = state;
  }
  else if (a->display == 2 && details != nullptr) {
    shown = details;
  }

  if (a->type == ACTIVITY_PLAYING) {
    return shown;
  }

  if (a->spotify && state != nullptr) {
    size_t n = 0;

    for (const char *c = state; *c != '\0' && n + 2 < cap; c++) {
      if (c[0] == ';' && c[1] == ' ') {
        out[n++] = ',';
        continue;
      }

      out[n++] = *c;
    }

    if (cap > 0) {
      out[n] = '\0';
    }

    return out;
  }

  if (a->application_id == CRUNCHYROLL && details != nullptr) {
    return details;
  }

  return shown;
}

static void put_presence(Store *s, const cima_json *presence,
                         Snowflake user_id) {
  const cima_json *partial = json_get(presence, "user");
  Snowflake id = snowflake_of(json_get(partial, "id"));

  if (id == 0) {
    id = snowflake_of(json_get(presence, "user_id"));
  }

  if (id == 0) {
    id = user_id;
  }

  if (id == 0) {
    return;
  }

  // a presence may be the first thing heard of a user.
  User *u = json_get(partial, "username") != nullptr
              ? store_put_user(s, partial)
              : ensure_user(s, id);

  const cima_json *clients = json_get(presence, "client_status");

  if (json_get(presence, "status") != nullptr) {
    u->status = parse_status(json_get_or(presence, "status", nullptr));
  }

  if (json_type(clients) == CIMA_JSON_OBJECT) {
    u->on_desktop = json_get_or(clients, "desktop", nullptr) != nullptr;
    u->on_mobile = json_get_or(clients, "mobile", nullptr) != nullptr;
    u->on_web = json_get_or(clients, "web", nullptr) != nullptr;
  }

  if (json_get(presence, "activities") != nullptr) {
    free(u->activity);
    u->activity = activity_text(json_get(presence, "activities"));
    u->custom_status = false;

    CIMA_EACH(activity, json_items(json_get(presence, "activities"))) {
      u->custom_status |=
        u->activity != nullptr &&
        json_get_coerce(activity, "type", (int64_t)-1) == ACTIVITY_CUSTOM;
    }

    put_activities(u, json_get(presence, "activities"));

    // the account's own comes with its settings once those are known.
    if (u->id != s->self_id || s->status_proto == nullptr) {
      put_custom_status(u, json_get(presence, "activities"));
    }
  }

  s->users_version++;
  s->version++;
}

void store_put_presence(Store *s, const cima_json *presence) {
  put_presence(s, presence, 0);
}

const VoiceState *store_voice_state(const Store *s, Snowflake user_id) {
  for (size_t i = 0; i < s->nvoice_states; i++) {
    if (s->voice_states[i].user_id == user_id) {
      return &s->voice_states[i];
    }
  }

  return nullptr;
}

void store_remove_voice_state(Store *s, Snowflake user_id, Snowflake guild_id) {
  for (size_t i = 0; i < s->nvoice_states; i++) {
    VoiceState *v = &s->voice_states[i];

    // 0 for the user drops every state of the guild.
    if ((user_id != 0 && v->user_id != user_id) || v->guild_id != guild_id) {
      continue;
    }

    *v = s->voice_states[--s->nvoice_states];
    i--;
    s->users_version++;
    s->version++;
  }
}

void store_put_voice_state(Store *s, const cima_json *state,
                           Snowflake guild_id) {
  Snowflake user_id = snowflake_of(json_get(state, "user_id"));
  Snowflake channel_id = snowflake_of(json_get(state, "channel_id"));

  if (snowflake_of(json_get(state, "guild_id")) != 0) {
    guild_id = snowflake_of(json_get(state, "guild_id"));
  }

  if (user_id == 0) {
    return;
  }

  if (channel_id == 0) {
    store_remove_voice_state(s, user_id, guild_id);
    return;
  }

  VoiceState *v = nullptr;

  // a user is in one voice channel at a time, the last one heard of wins.
  for (size_t i = 0; i < s->nvoice_states; i++) {
    if (s->voice_states[i].user_id == user_id) {
      v = &s->voice_states[i];
    }
  }

  if (v == nullptr) {
    s->voice_states =
      resized(s->voice_states, s->nvoice_states + 1, sizeof *s->voice_states);
    v = &s->voice_states[s->nvoice_states++];
  }

  *v = (VoiceState){
    .user_id = user_id,
    .guild_id = guild_id,
    .channel_id = channel_id,
    .streaming = json_get_coerce(state, "self_stream", false),
    .muted = json_get_coerce(state, "self_mute", false) ||
             json_get_coerce(state, "mute", false),
    .deafened = json_get_coerce(state, "self_deaf", false) ||
                json_get_coerce(state, "deaf", false),
  };

  s->users_version++;
  s->version++;
}

void store_put_affinities(Store *s, const cima_json *affinities) {
  free(s->affinities);
  s->affinities = nullptr;
  s->naffinities = 0;

  if (json_len(affinities) > 0) {
    s->affinities = xcalloc(json_len(affinities), sizeof *s->affinities);
  }

  CIMA_EACH(affinity, json_items(affinities)) {
    Snowflake id = snowflake_of(json_get(affinity, "user_id"));

    if (id != 0) {
      s->affinities[s->naffinities++] = id;
    }
  }

  s->users_version++;
  s->version++;
}

void store_put_relationship(Store *s, const cima_json *relationship) {
  const cima_json *user = json_get(relationship, "user");
  Snowflake id = snowflake_of(json_get(relationship, "id"));

  if (json_type(user) == CIMA_JSON_OBJECT) {
    store_put_user(s, user);

    if (id == 0) {
      id = snowflake_of(json_get(user, "id"));
    }
  }

  if (id == 0) {
    id = snowflake_of(json_get(relationship, "user_id"));
  }

  if (id == 0) {
    return;
  }

  Relationship *r = nullptr;

  for (size_t i = 0; i < s->nrelationships; i++) {
    if (s->relationships[i].user_id == id) {
      r = &s->relationships[i];
    }
  }

  if (r == nullptr) {
    s->relationships = resized(s->relationships, s->nrelationships + 1,
                               sizeof *s->relationships);
    r = &s->relationships[s->nrelationships++];
    *r = (Relationship){.user_id = id};
  }

  if (json_get(relationship, "type") != nullptr) {
    r->type =
      (RelationshipType)json_get_coerce(relationship, "type", (int64_t)0);
  }

  put_str(&r->nickname, relationship, "nickname");
  put_bool(&r->ignored, relationship, "user_ignored");

  s->users_version++;
  s->version++;
}

void store_remove_relationship(Store *s, Snowflake user_id) {
  for (size_t i = 0; i < s->nrelationships; i++) {
    if (s->relationships[i].user_id != user_id) {
      continue;
    }

    free(s->relationships[i].nickname);
    s->relationships[i] = s->relationships[--s->nrelationships];
    s->users_version++;
    s->version++;
    return;
  }
}

/* ========================================================================== */
/* messages.                                                                  */
/* ========================================================================== */

static void free_attachments(Message *m) {
  for (size_t i = 0; i < m->nattachments; i++) {
    Attachment *a = &m->attachments[i];
    free(a->filename);
    free(a->description);
    free(a->content_type);
    free(a->url);
    free(a->proxy_url);
  }

  free(m->attachments);
  m->attachments = nullptr;
  m->nattachments = 0;
}

static void free_media(EmbedMedia *media) {
  free(media->url);
  free(media->proxy_url);
}

static void free_embeds(Message *m) {
  for (size_t i = 0; i < m->nembeds; i++) {
    Embed *e = &m->embeds[i];
    free(e->type);
    free(e->title);
    free(e->description);
    free(e->url);
    free(e->author_name);
    free(e->author_url);
    free(e->author_icon);
    free(e->footer_text);
    free(e->footer_icon);
    free(e->provider_name);
    free(e->provider_url);
    free_media(&e->image);
    free_media(&e->thumbnail);
    free_media(&e->video);

    for (size_t j = 0; j < e->nfields; j++) {
      free(e->fields[j].name);
      free(e->fields[j].value);
    }

    free(e->fields);
  }

  free(m->embeds);
  m->embeds = nullptr;
  m->nembeds = 0;
}

static void free_reactions(Message *m) {
  for (size_t i = 0; i < m->nreactions; i++) {
    free(m->reactions[i].name);
  }

  free(m->reactions);
  m->reactions = nullptr;
  m->nreactions = 0;
}

static void free_stickers(Message *m) {
  for (size_t i = 0; i < m->nstickers; i++) {
    free(m->stickers[i].name);
  }

  free(m->stickers);
  m->stickers = nullptr;
  m->nstickers = 0;
}

static void free_message(Message *m) {
  if (m == nullptr) {
    return;
  }

  free_attachments(m);
  free_embeds(m);
  free_reactions(m);
  free_stickers(m);
  free_message(m->referenced);
  free(m->content);
  free(m->mentions);
  free(m->mention_roles);
  free(m->interaction_name);
  free(m->error);
  free(m);
}

static bool has_prefix(const char *s, const char *prefix) {
  return s != nullptr && strncmp(s, prefix, strlen(prefix)) == 0;
}

// the flags of an attachment, and of the media of an embed.
constexpr uint64_t ATTACHMENT_FLAG_THUMBNAIL = 1u << 1;
constexpr uint64_t ATTACHMENT_FLAG_SPOILER = 1u << 3;
constexpr uint64_t ATTACHMENT_FLAG_ANIMATED = 1u << 5;
constexpr uint64_t EMBED_MEDIA_FLAG_ANIMATED = 1u << 5;

static void fill_attachments(Message *m, const cima_json *attachments) {
  free_attachments(m);

  size_t n = json_len(attachments);

  if (json_type(attachments) != CIMA_JSON_ARRAY || n == 0) {
    return;
  }

  m->attachments = xcalloc(n, sizeof *m->attachments);

  CIMA_EACH(item, json_items(attachments)) {
    Attachment *a = &m->attachments[m->nattachments++];

    a->id = snowflake_of(json_get(item, "id"));
    a->filename = str_dup(json_get_or(item, "filename", nullptr));
    a->description = str_dup(json_get_or(item, "description", nullptr));
    a->content_type = str_dup(json_get_or(item, "content_type", nullptr));
    a->url = str_dup(json_get_or(item, "url", nullptr));
    a->proxy_url = str_dup(json_get_or(item, "proxy_url", nullptr));
    a->size = json_get_coerce(item, "size", (uint64_t)0);
    a->width = (uint32_t)json_get_coerce(item, "width", (uint64_t)0);
    a->height = (uint32_t)json_get_coerce(item, "height", (uint64_t)0);
    uint64_t flags = json_get_coerce(item, "flags", (uint64_t)0);

    a->spoiler = has_prefix(a->filename, "SPOILER_") ||
                 (flags & ATTACHMENT_FLAG_SPOILER) != 0;
    a->animated = (flags & ATTACHMENT_FLAG_ANIMATED) != 0;
    a->thumbnail = (flags & ATTACHMENT_FLAG_THUMBNAIL) != 0;
    a->duration = json_get_coerce(item, "duration_secs", 0.0);
  }
}

static void fill_media(EmbedMedia *media, const cima_json *v) {
  media->url = str_dup(json_get_or(v, "url", nullptr));
  media->proxy_url = str_dup(json_get_or(v, "proxy_url", nullptr));
  media->width = (uint32_t)json_get_coerce(v, "width", (uint64_t)0);
  media->height = (uint32_t)json_get_coerce(v, "height", (uint64_t)0);
  media->animated = (json_get_coerce(v, "flags", (uint64_t)0) &
                     EMBED_MEDIA_FLAG_ANIMATED) != 0;
}

static void fill_embeds(Message *m, const cima_json *embeds) {
  free_embeds(m);

  size_t n = json_len(embeds);

  if (json_type(embeds) != CIMA_JSON_ARRAY || n == 0) {
    return;
  }

  m->embeds = xcalloc(n, sizeof *m->embeds);

  CIMA_EACH(item, json_items(embeds)) {
    Embed *e = &m->embeds[m->nembeds++];
    const cima_json *author = json_get(item, "author");
    const cima_json *footer = json_get(item, "footer");
    const cima_json *color = json_get(item, "color");
    const cima_json *fields = json_get(item, "fields");

    e->type = str_dup(json_get_or(item, "type", nullptr));
    e->title = str_dup(json_get_or(item, "title", nullptr));
    e->description = str_dup(json_get_or(item, "description", nullptr));
    e->url = str_dup(json_get_or(item, "url", nullptr));
    e->has_color = cima_json_is_number(color);
    e->color = (uint32_t)snowflake_of(color) & 0xffffff;
    e->timestamp = iso8601_parse_ms(json_get_or(item, "timestamp", nullptr));
    e->author_name = str_dup(json_get_or(author, "name", nullptr));
    e->author_url = str_dup(json_get_or(author, "url", nullptr));
    e->author_icon = str_dup(json_get_or(author, "proxy_icon_url", nullptr));
    e->footer_text = str_dup(json_get_or(footer, "text", nullptr));
    e->footer_icon = str_dup(json_get_or(footer, "proxy_icon_url", nullptr));
    e->provider_name =
      str_dup(json_get_or(json_get(item, "provider"), "name", nullptr));
    e->provider_url =
      str_dup(json_get_or(json_get(item, "provider"), "url", nullptr));

    fill_media(&e->image, json_get(item, "image"));
    fill_media(&e->thumbnail, json_get(item, "thumbnail"));
    fill_media(&e->video, json_get(item, "video"));

    size_t nfields =
      (json_type(fields) == CIMA_JSON_ARRAY) ? json_len(fields) : 0;

    if (nfields == 0) {
      continue;
    }

    e->fields = xcalloc(nfields, sizeof *e->fields);

    CIMA_EACH(field, json_items(fields)) {
      EmbedField *f = &e->fields[e->nfields++];
      f->name = str_dup(json_get_or(field, "name", nullptr));
      f->value = str_dup(json_get_or(field, "value", nullptr));
      f->is_inline = json_get_coerce(field, "inline", false);
    }
  }
}

static void fill_reactions(Message *m, const cima_json *reactions) {
  free_reactions(m);

  size_t n = json_len(reactions);

  if (json_type(reactions) != CIMA_JSON_ARRAY || n == 0) {
    return;
  }

  m->reactions = xcalloc(n, sizeof *m->reactions);

  CIMA_EACH(item, json_items(reactions)) {
    const cima_json *emoji = json_get(item, "emoji");
    Reaction *r = &m->reactions[m->nreactions++];

    r->emoji_id = snowflake_of(json_get(emoji, "id"));
    r->name = str_dup(json_get_or(emoji, "name", nullptr));
    r->animated = json_get_coerce(emoji, "animated", false);
    r->count = (uint32_t)json_get_coerce(item, "count", (uint64_t)0);
    r->me = json_get_coerce(item, "me", false);
  }
}

static void fill_stickers(Message *m, const cima_json *stickers) {
  free_stickers(m);

  size_t n = json_len(stickers);

  if (json_type(stickers) != CIMA_JSON_ARRAY || n == 0) {
    return;
  }

  m->stickers = xcalloc(n, sizeof *m->stickers);

  CIMA_EACH(item, json_items(stickers)) {
    Sticker *sticker = &m->stickers[m->nstickers++];

    sticker->id = snowflake_of(json_get(item, "id"));
    sticker->name = str_dup(json_get_or(item, "name", nullptr));
    sticker->format =
      (uint8_t)json_get_coerce(item, "format_type", (uint64_t)0);
  }
}

static bool channel_muted(const Store *s, const Channel *c);
static int64_t mute_end(const cima_json *settings);

// whether the message names the current user. `settings` leaves out what
// the user's settings for the guild silence, as discord does when it counts
// a ping.
static bool mentions_self(const Store *s, const Message *m, const Guild *g,
                          bool settings) {
  bool quiet = settings && g != nullptr;

  if (m->mention_everyone && !(quiet && g->suppress_everyone)) {
    return true;
  }

  for (size_t i = 0; i < m->nmentions; i++) {
    if (m->mentions[i] == s->self_id) {
      return true;
    }
  }

  const Member *me = store_member(g, s->self_id);

  if (me == nullptr || (quiet && g->suppress_roles)) {
    return false;
  }

  for (size_t i = 0; i < m->nmention_roles; i++) {
    for (size_t j = 0; j < me->nroles; j++) {
      if (m->mention_roles[i] == me->roles[j]) {
        return true;
      }
    }
  }

  return false;
}

// copies what `json` carries into `m`, leaving the rest as it was, since an
// update may hold nothing but what changed.
static void fill_message(Store *s, Message *m, const cima_json *json, Guild *g,
                         int depth) {
  const cima_json *author = json_get(json, "author");
  const cima_json *v;

  if (json_type(author) == CIMA_JSON_OBJECT) {
    store_put_user(s, author);
    m->author_id = snowflake_of(json_get(author, "id"));
  }

  if (g != nullptr &&
      (json_type(json_get(json, "member")) == CIMA_JSON_OBJECT) &&
      m->author_id != 0) {
    store_put_member(s, g, json_get(json, "member"), m->author_id);
  }

  put_id(&m->webhook_id, json, "webhook_id");

  if ((v = json_get(json, "type")) != nullptr) {
    m->type = (uint8_t)snowflake_of(v);
  }

  if ((v = json_get(json, "flags")) != nullptr) {
    m->flags = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(json, "content")) != nullptr) {
    str_set(&m->content, json_or(v, ""));
  }

  if ((v = json_get(json, "timestamp")) != nullptr) {
    m->timestamp = iso8601_parse_ms(json_or(v, nullptr));
  }

  if ((v = json_get(json, "edited_timestamp")) != nullptr) {
    m->edited_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  put_bool(&m->pinned, json, "pinned");
  put_bool(&m->tts, json, "tts");
  put_bool(&m->mention_everyone, json, "mention_everyone");

  if ((v = json_get(json, "mentions")) != nullptr) {
    CIMA_EACH(user, json_items(v)) {
      store_put_user(s, user);

      if (g != nullptr &&
          (json_type(json_get(user, "member")) == CIMA_JSON_OBJECT)) {
        store_put_member(s, g, json_get(user, "member"),
                         snowflake_of(json_get(user, "id")));
      }
    }

    free(m->mentions);
    m->mentions = id_array(v, "id", &m->nmentions);
  }

  if ((v = json_get(json, "mention_roles")) != nullptr) {
    free(m->mention_roles);
    m->mention_roles = id_array(v, nullptr, &m->nmention_roles);
  }

  if ((v = json_get(json, "attachments")) != nullptr) {
    fill_attachments(m, v);
  }

  if ((v = json_get(json, "embeds")) != nullptr) {
    fill_embeds(m, v);
  }

  if ((v = json_get(json, "reactions")) != nullptr) {
    fill_reactions(m, v);
  }

  if ((v = json_get(json, "sticker_items")) != nullptr) {
    fill_stickers(m, v);
  }

  if ((v = json_get(json, "message_reference")) != nullptr) {
    m->reference_id = snowflake_of(json_get(v, "message_id"));
    m->reference_channel_id = snowflake_of(json_get(v, "channel_id"));
    m->forwarded = json_get_coerce(v, "type", (int64_t)0) == 1;
  }

  // a reply carries the message it answers, a forward a snapshot of what it
  // forwards. both are kept the same way, one level deep.
  const cima_json *referenced = json_get(json, "referenced_message");
  const cima_json *snapshot =
    json_get(json_index(json_get(json, "message_snapshots"), 0), "message");

  if (json_type(snapshot) == CIMA_JSON_OBJECT) {
    referenced = snapshot;
    m->forwarded = true;
  }

  if (referenced != nullptr && depth == 0) {
    free_message(m->referenced);
    m->referenced = nullptr;

    if (json_type(referenced) == CIMA_JSON_OBJECT) {
      Message *copy = xcalloc(1, sizeof *copy);
      copy->id = snowflake_of(json_get(referenced, "id"));
      copy->channel_id = snowflake_of(json_get(referenced, "channel_id"));
      copy->content = str_dup("");

      if (copy->id == 0) {
        copy->id = m->reference_id;
      }

      if (copy->channel_id == 0) {
        copy->channel_id = m->reference_channel_id != 0
                             ? m->reference_channel_id
                             : m->channel_id;
      }

      fill_message(s, copy, referenced, g, depth + 1);
      m->referenced = copy;
    }
  }

  const cima_json *interaction = json_get(json, "interaction");

  if (json_type(interaction) != CIMA_JSON_OBJECT) {
    interaction = json_get(json, "interaction_metadata");
  }

  if (json_type(interaction) == CIMA_JSON_OBJECT) {
    const cima_json *user = json_get(interaction, "user");

    put_str(&m->interaction_name, interaction, "name");

    if (json_type(user) == CIMA_JSON_OBJECT) {
      store_put_user(s, user);
      m->interaction_user_id = snowflake_of(json_get(user, "id"));
    }
  }

  m->mentions_me = mentions_self(s, m, g, false);
}

static void insert_at(Channel *c, size_t slot, Message *m) {
  reserve(&c->messages, &c->messages_cap, c->nmessages + 1,
          sizeof *c->messages);
  memmove(&c->messages[slot + 1], &c->messages[slot],
          (c->nmessages - slot) * sizeof *c->messages);
  c->messages[slot] = m;
  c->nmessages++;
}

static void remove_at(Channel *c, size_t slot) {
  free_message(c->messages[slot]);
  memmove(&c->messages[slot], &c->messages[slot + 1],
          (c->nmessages - slot - 1) * sizeof *c->messages);
  c->nmessages--;
}

// the message `json` describes in the channel, made when it is new.
static Message *upsert_message(Store *s, Channel *c, const cima_json *json) {
  Snowflake id = snowflake_of(json_get(json, "id"));

  if (id == 0) {
    return nullptr;
  }

  size_t slot = message_slot(c, id);
  Message *m;

  if (slot < c->nmessages && c->messages[slot]->id == id) {
    m = c->messages[slot];
  }
  else {
    m = xcalloc(1, sizeof *m);
    m->id = id;
    m->channel_id = c->id;
    m->content = str_dup("");
    insert_at(c, slot, m);
  }

  fill_message(s, m, json, store_guild(s, c->guild_id), 0);

  // a pending message that got its answer is a sent one.
  m->state = MESSAGE_SENT;
  return m;
}

static ReadState *read_state(Store *s, Snowflake channel_id) {
  return MAP_PUT(&s->read_states, channel_id);
}

static size_t find_pending(const Channel *c, Snowflake nonce) {
  for (size_t i = c->nmessages; i > 0; i--) {
    const Message *m = c->messages[i - 1];

    if (m->state != MESSAGE_SENT && m->nonce == nonce) {
      return i - 1;
    }
  }

  return SIZE_MAX;
}

static bool mute_active(bool muted, int64_t until, int64_t now);
static NotifyLevel notify_level(const Store *s, const Channel *c);

// whether a message of someone else counts as a ping, after the mention
// test of discord's read state store.
static bool pings(const Store *s, const Channel *c, const Message *m) {
  const Relationship *r = store_relationship(s, m->author_id);

  if (r != nullptr && (r->type == RELATIONSHIP_BLOCKED || r->ignored)) {
    return false;
  }

  if (m->type == MESSAGE_RECIPIENT_REMOVE && c->type == CHANNEL_GROUP_DM) {
    return false;
  }

  if (mentions_self(s, m, store_guild(s, c->guild_id), true)) {
    return true;
  }

  // every message of a dm counts until it is muted.
  if (channel_is_private(c)) {
    return !channel_muted(s, c);
  }

  // the account may ask for a count wherever every message notifies. a
  // thread follows its own setting, which is not kept, so threads are out.
  return (s->notify_flags & NOTIFY_FLAG_MENTION_ON_ALL_MESSAGES) != 0 &&
         !channel_is_thread(c) && !channel_is_voice(c) &&
         !mute_active(c->muted, c->mute_until, now_ms(s)) &&
         notify_level(s, c) == NOTIFY_ALL_MESSAGES;
}

/* ========================================================================== */
/* bookmarks.                                                                 */
/* ========================================================================== */

// where `id` is or goes in the sorted `bookmark_ids`.
static size_t bookmark_id_slot(const Store *s, Snowflake id) {
  size_t low = 0, high = s->nbookmark_ids;

  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (s->bookmark_ids[mid] < id) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }

  return low;
}

bool store_bookmarked(const Store *s, Snowflake message_id) {
  size_t slot = bookmark_id_slot(s, message_id);

  return slot < s->nbookmark_ids && s->bookmark_ids[slot] == message_id;
}

static void add_bookmark_id(Store *s, Snowflake id) {
  size_t slot = bookmark_id_slot(s, id);

  if (id == 0 || (slot < s->nbookmark_ids && s->bookmark_ids[slot] == id)) {
    return;
  }

  s->bookmark_ids = resized(s->bookmark_ids, s->nbookmark_ids + 1,
                            sizeof *s->bookmark_ids);
  memmove(&s->bookmark_ids[slot + 1], &s->bookmark_ids[slot],
          (s->nbookmark_ids - slot) * sizeof *s->bookmark_ids);
  s->bookmark_ids[slot] = id;
  s->nbookmark_ids++;
}

static void drop_bookmark_id(Store *s, Snowflake id) {
  size_t slot = bookmark_id_slot(s, id);

  if (slot >= s->nbookmark_ids || s->bookmark_ids[slot] != id) {
    return;
  }

  memmove(&s->bookmark_ids[slot], &s->bookmark_ids[slot + 1],
          (s->nbookmark_ids - slot - 1) * sizeof *s->bookmark_ids);
  s->bookmark_ids =
    resized(s->bookmark_ids, s->nbookmark_ids - 1, sizeof *s->bookmark_ids);
  s->nbookmark_ids--;
}

static Bookmark *find_bookmark(Store *s, Snowflake message_id) {
  for (size_t i = 0; i < s->nbookmarks; i++) {
    if (s->bookmarks[i].message_id == message_id) {
      return &s->bookmarks[i];
    }
  }

  return nullptr;
}

static void drop_bookmark(Store *s, Snowflake message_id) {
  Bookmark *b = find_bookmark(s, message_id);

  if (b == nullptr) {
    return;
  }

  size_t at = (size_t)(b - s->bookmarks);

  free_message(b->message);
  memmove(b, b + 1, (s->nbookmarks - at - 1) * sizeof *b);
  s->bookmarks =
    resized(s->bookmarks, s->nbookmarks - 1, sizeof *s->bookmarks);
  s->nbookmarks--;
}

static void free_bookmarks(Store *s) {
  for (size_t i = 0; i < s->nbookmarks; i++) {
    free_message(s->bookmarks[i].message);
  }

  free(s->bookmarks);
  s->bookmarks = nullptr;
  s->nbookmarks = 0;
}

// the listed bookmark of a message, made in its place by when it was saved
// when it is new. one saved again moves to its new place.
static Bookmark *put_bookmark(Store *s, Snowflake channel_id,
                              Snowflake message_id, int64_t saved_at) {
  Bookmark kept = {};
  Bookmark *old = find_bookmark(s, message_id);

  if (old != nullptr) {
    kept = *old;
    size_t at = (size_t)(old - s->bookmarks);

    memmove(old, old + 1, (s->nbookmarks - at - 1) * sizeof *old);
    s->nbookmarks--;
  }

  size_t at = 0;

  while (at < s->nbookmarks && s->bookmarks[at].saved_at >= saved_at) {
    at++;
  }

  s->bookmarks =
    resized(s->bookmarks, s->nbookmarks + 1, sizeof *s->bookmarks);
  memmove(&s->bookmarks[at + 1], &s->bookmarks[at],
          (s->nbookmarks - at) * sizeof *s->bookmarks);
  s->nbookmarks++;

  Bookmark *b = &s->bookmarks[at];

  *b = kept;
  b->channel_id = channel_id;
  b->message_id = message_id;
  b->saved_at = saved_at;
  return b;
}

const Message *store_bookmark_message(const Store *s, const Bookmark *b) {
  if (b->message != nullptr) {
    return b->message;
  }

  return store_message(store_channel(s, b->channel_id), b->message_id);
}

void store_set_bookmark_ids(Store *s, const cima_json *ids) {
  free_bookmarks(s);
  free(s->bookmark_ids);
  s->bookmark_ids = nullptr;
  s->nbookmark_ids = 0;

  CIMA_EACH(id, json_items(ids)) {
    add_bookmark_id(s, snowflake_of(id));
  }

  s->bookmarks_known = true;
  s->bookmarks_version++;
  s->version++;
}

void store_put_saved_message(Store *s, const cima_json *saved, bool page) {
  const cima_json *data = json_get(saved, "save_data");
  Snowflake channel_id = snowflake_of(json_get(data, "channel_id"));
  Snowflake message_id = snowflake_of(json_get(data, "message_id"));

  if (channel_id == 0 || message_id == 0) {
    return;
  }

  s->bookmarks_version++;
  s->version++;

  // a reminder is no bookmark, even of a message that was one.
  if (json_type(json_get(data, "due_at")) == CIMA_JSON_STRING) {
    drop_bookmark_id(s, message_id);
    drop_bookmark(s, message_id);
    return;
  }

  if (page && !store_bookmarked(s, message_id)) {
    return;
  }

  int64_t saved_at =
    iso8601_parse_ms(json_get_or(data, "saved_at", nullptr));
  Bookmark *b = put_bookmark(s, channel_id, message_id, saved_at);
  const cima_json *message = json_get(saved, "message");

  add_bookmark_id(s, message_id);
  // discord writes 0 for no guild.
  b->guild_id = snowflake_of(json_get(data, "guild_id"));

  if (json_type(message) != CIMA_JSON_OBJECT) {
    free_message(b->message);
    b->message = nullptr;
    return;
  }

  if (b->message == nullptr) {
    b->message = xcalloc(1, sizeof *b->message);
    b->message->id = message_id;
    b->message->channel_id = channel_id;
    b->message->content = str_dup("");
  }

  fill_message(s, b->message, message, store_guild(s, b->guild_id), 0);
}

void store_add_bookmark(Store *s, Snowflake channel_id, Snowflake message_id,
                        int64_t saved_at) {
  Channel *c = store_channel(s, channel_id);
  Bookmark *b = put_bookmark(s, channel_id, message_id, saved_at);

  add_bookmark_id(s, message_id);
  b->guild_id = c != nullptr ? c->guild_id : 0;
  s->bookmarks_version++;
  s->version++;
}

void store_remove_saved_message(Store *s, Snowflake message_id) {
  drop_bookmark_id(s, message_id);
  drop_bookmark(s, message_id);
  s->bookmarks_version++;
  s->version++;
}

// the copy a bookmark keeps of a message, null for none.
static Message *bookmark_copy(Store *s, Snowflake message_id) {
  Bookmark *b = find_bookmark(s, message_id);

  return b != nullptr ? b->message : nullptr;
}

Message *store_create_message(Store *s, const cima_json *message) {
  Channel *c = store_channel(s, snowflake_of(json_get(message, "channel_id")));

  if (c == nullptr) {
    return nullptr;
  }

  // the nonce discord echoes tells which pending message this one is.
  Snowflake nonce = snowflake_of(json_get(message, "nonce"));
  Snowflake author = snowflake_of(json_get(json_get(message, "author"), "id"));

  if (nonce != 0 && author == s->self_id) {
    size_t pending = find_pending(c, nonce);

    if (pending != SIZE_MAX) {
      remove_at(c, pending);
    }
  }

  Message *m = upsert_message(s, c, message);

  if (m == nullptr) {
    return nullptr;
  }

  if (m->id > c->last_message_id) {
    c->last_message_id = m->id;
  }

  for (size_t i = 0; i < c->ntyping; i++) {
    if (c->typing[i].user_id == m->author_id) {
      c->typing[i] = c->typing[--c->ntyping];
      break;
    }
  }

  ReadState *rs = read_state(s, c->id);

  // writing in a channel reads it, the system line of a poll's end aside.
  if (m->author_id == s->self_id && m->type != MESSAGE_POLL_RESULT) {
    rs->known = true;
    rs->last_acked_id = m->id;
    rs->mention_count = 0;
  }
  // a message older than what was acknowledged was read already.
  else if (m->id > rs->last_acked_id && pings(s, c, m)) {
    rs->known = true;
    rs->mention_count++;
  }

  if (!c->history_loaded && c->nmessages > UNOPENED_MESSAGES) {
    store_trim_messages(s, c, UNOPENED_MESSAGES);
  }

  if (channel_is_private(c)) {
    s->private_version++;
  }

  store_touch_channel(s, c);
  return m;
}

void store_update_message(Store *s, const cima_json *message) {
  Channel *c = store_channel(s, snowflake_of(json_get(message, "channel_id")));
  Snowflake id = snowflake_of(json_get(message, "id"));
  Message *found[] = {store_message(c, id), starter_of(c, id)};
  Bookmark *b = find_bookmark(s, id);

  for (size_t i = 0; i < 2; i++) {
    if (found[i] != nullptr) {
      fill_message(s, found[i], message, store_guild(s, c->guild_id), 0);
      store_touch_channel(s, c);
    }
  }

  if (b != nullptr && b->message != nullptr) {
    fill_message(s, b->message, message, store_guild(s, b->guild_id), 0);
    s->bookmarks_version++;
    s->version++;
  }
}

void store_remove_message(Store *s, Snowflake channel_id, Snowflake id) {
  Channel *c = store_channel(s, channel_id);
  Bookmark *b = find_bookmark(s, id);

  // the bookmark stays, and tells that its message is gone.
  if (b != nullptr && b->message != nullptr) {
    free_message(b->message);
    b->message = nullptr;
    s->bookmarks_version++;
    s->version++;
  }

  if (c == nullptr || id == 0) {
    return;
  }

  size_t slot = message_slot(c, id);
  bool changed = false;

  if (slot < c->nmessages && c->messages[slot]->id == id) {
    remove_at(c, slot);
    changed = true;
  }

  // replies to it show that what they answer is gone.
  for (size_t i = 0; i < c->nmessages; i++) {
    Message *m = c->messages[i];

    if (m->reference_id == id && !m->forwarded && m->referenced != nullptr) {
      free_message(m->referenced);
      m->referenced = nullptr;
      changed = true;
    }
  }

  if (changed) {
    store_touch_channel(s, c);
  }
}

void store_put_history(Store *s, Channel *c, const cima_json *messages,
                       HistoryKind kind, uint32_t limit) {
  size_t count = 0;

  CIMA_EACH(message, json_items(messages)) {
    if (upsert_message(s, c, message) != nullptr) {
      count++;
    }
  }

  // a short page means there is nothing further that way.
  if (kind == HISTORY_LATEST) {
    c->history_loaded = true;
    c->has_older = count >= limit;
  }
  else if (kind == HISTORY_BEFORE) {
    c->has_older = count >= limit;
  }

  if (c->nmessages > 0) {
    Snowflake last = c->messages[c->nmessages - 1]->id;
    bool sent = c->messages[c->nmessages - 1]->state == MESSAGE_SENT;

    if (sent && last > c->last_message_id) {
      c->last_message_id = last;
    }
  }

  c->loading = false;
  store_touch_channel(s, c);
}

void store_trim_messages(Store *s, Channel *c, size_t keep) {
  if (c->nmessages <= keep) {
    return;
  }

  size_t drop = c->nmessages - keep;

  for (size_t i = 0; i < drop; i++) {
    free_message(c->messages[i]);
  }

  memmove(c->messages, c->messages + drop, keep * sizeof *c->messages);
  c->nmessages = keep;
  c->has_older = true;
  store_touch_channel(s, c);
}

Message *store_add_pending(Store *s, Channel *c, const char *content,
                           Snowflake nonce, Snowflake reply_to) {
  Message *m = xcalloc(1, sizeof *m);

  // the nonce is a snowflake of now, so the message sorts last like the one
  // discord will answer with.
  m->id = nonce;
  m->nonce = nonce;
  m->channel_id = c->id;
  m->author_id = s->self_id;
  m->state = MESSAGE_PENDING;
  m->content = str_dup(content != nullptr ? content : "");
  m->timestamp = now_ms(s);

  const Message *target = store_message(c, reply_to);

  if (reply_to != 0) {
    m->type = MESSAGE_REPLY;
    m->reference_id = reply_to;
    m->reference_channel_id = c->id;
  }

  if (target != nullptr) {
    Message *copy = xcalloc(1, sizeof *copy);
    copy->id = target->id;
    copy->channel_id = target->channel_id;
    copy->author_id = target->author_id;
    copy->webhook_id = target->webhook_id;
    copy->type = target->type;
    copy->timestamp = target->timestamp;
    copy->content = str_dup(target->content);
    m->referenced = copy;
  }

  insert_at(c, message_slot(c, m->id), m);
  store_touch_channel(s, c);
  return m;
}

void store_fail_pending(Store *s, Snowflake channel_id, Snowflake nonce,
                        const char *error) {
  Channel *c = store_channel(s, channel_id);
  size_t slot = c != nullptr ? find_pending(c, nonce) : SIZE_MAX;

  if (slot == SIZE_MAX) {
    return;
  }

  c->messages[slot]->state = MESSAGE_FAILED;
  str_set(&c->messages[slot]->error, error);
  store_touch_channel(s, c);
}

void store_remove_pending(Store *s, Snowflake channel_id, Snowflake nonce) {
  Channel *c = store_channel(s, channel_id);
  size_t slot = c != nullptr ? find_pending(c, nonce) : SIZE_MAX;

  if (slot == SIZE_MAX) {
    return;
  }

  remove_at(c, slot);
  store_touch_channel(s, c);
}

static bool same_emoji(const Reaction *r, Snowflake id, const char *name) {
  if (id != 0 || r->emoji_id != 0) {
    return r->emoji_id == id;
  }

  return r->name != nullptr && name != nullptr && strcmp(r->name, name) == 0;
}

static Message *reaction_target(Store *s, Snowflake channel_id,
                                Snowflake message_id, Channel **channel) {
  *channel = store_channel(s, channel_id);
  return store_message(*channel, message_id);
}

void store_reaction_add(Store *s, Snowflake channel_id, Snowflake message_id,
                        Snowflake user_id, const cima_json *emoji) {
  Channel *c;
  Message *m = reaction_target(s, channel_id, message_id, &c);

  if (m == nullptr) {
    return;
  }

  Snowflake id = snowflake_of(json_get(emoji, "id"));
  const char *name = json_get_or(emoji, "name", nullptr);
  Reaction *r = nullptr;

  for (size_t i = 0; i < m->nreactions; i++) {
    if (same_emoji(&m->reactions[i], id, name)) {
      r = &m->reactions[i];
    }
  }

  if (r == nullptr) {
    m->reactions =
      resized(m->reactions, m->nreactions + 1, sizeof *m->reactions);
    r = &m->reactions[m->nreactions++];
    *r = (Reaction){
      .emoji_id = id,
      .name = str_dup(name),
      .animated = json_get_coerce(emoji, "animated", false),
    };
  }

  // the current user's own reaction is counted once, whether the answer to
  // the request or the gateway tells of it first.
  if (user_id == s->self_id) {
    if (!r->me) {
      r->me = true;
      r->count++;
    }
  }
  else {
    r->count++;
  }

  store_touch_channel(s, c);
}

void store_reaction_remove(Store *s, Snowflake channel_id, Snowflake message_id,
                           Snowflake user_id, const cima_json *emoji) {
  Channel *c;
  Message *m = reaction_target(s, channel_id, message_id, &c);

  if (m == nullptr) {
    return;
  }

  Snowflake id = snowflake_of(json_get(emoji, "id"));
  const char *name = json_get_or(emoji, "name", nullptr);

  for (size_t i = 0; i < m->nreactions; i++) {
    Reaction *r = &m->reactions[i];

    if (!same_emoji(r, id, name)) {
      continue;
    }

    if (user_id == s->self_id) {
      if (!r->me) {
        return;
      }

      r->me = false;
    }

    if (r->count > 0) {
      r->count--;
    }

    if (r->count == 0) {
      free(r->name);
      memmove(r, r + 1, (m->nreactions - i - 1) * sizeof *r);
      m->nreactions--;
    }

    store_touch_channel(s, c);
    return;
  }
}

void store_reaction_clear(Store *s, Snowflake channel_id, Snowflake message_id,
                          const cima_json *emoji) {
  Channel *c;
  Message *m = reaction_target(s, channel_id, message_id, &c);

  if (m == nullptr) {
    return;
  }

  if (json_type(emoji) != CIMA_JSON_OBJECT) {
    free_reactions(m);
    store_touch_channel(s, c);
    return;
  }

  Snowflake id = snowflake_of(json_get(emoji, "id"));
  const char *name = json_get_or(emoji, "name", nullptr);

  for (size_t i = 0; i < m->nreactions; i++) {
    Reaction *r = &m->reactions[i];

    if (same_emoji(r, id, name)) {
      free(r->name);
      memmove(r, r + 1, (m->nreactions - i - 1) * sizeof *r);
      m->nreactions--;
      break;
    }
  }

  store_touch_channel(s, c);
}

/* ========================================================================== */
/* guilds, roles and members.                                                 */
/* ========================================================================== */

static void free_role(Role *r) {
  free(r->name);
  free(r->icon);
  free(r->unicode_emoji);
  free(r);
}

static void free_member(Member *m) {
  free(m->nick);
  free(m->avatar);
  free(m->decoration);
  free(m->roles);
  free(m);
}

static void free_emojis(Guild *g) {
  for (size_t i = 0; i < g->nemojis; i++) {
    free(g->emojis[i].name);
  }

  free(g->emojis);
  g->emojis = nullptr;
  g->nemojis = 0;
}

static void free_list(MemberList *list) {
  free(list->id);
  free(list->rows);
  free(list);
}

static int compare_roles(const void *a, const void *b) {
  const Role *x = *(Role *const *)a, *y = *(Role *const *)b;

  if (x->position != y->position) {
    return x->position > y->position ? -1 : 1;
  }

  // of two roles at the same position the older one is the higher.
  return x->id < y->id ? -1 : x->id > y->id;
}

static Role *put_role(Guild *g, const cima_json *role) {
  Snowflake id = snowflake_of(json_get(role, "id"));

  if (id == 0) {
    return nullptr;
  }

  Role *r = store_role(g, id);

  if (r == nullptr) {
    r = xcalloc(1, sizeof *r);
    r->id = id;
    g->roles = resized(g->roles, g->nroles + 1, sizeof *g->roles);
    g->roles[g->nroles++] = r;
  }

  const cima_json *v;

  put_str(&r->name, role, "name");
  put_str(&r->icon, role, "icon");
  put_str(&r->unicode_emoji, role, "unicode_emoji");
  put_bool(&r->hoist, role, "hoist");
  put_bool(&r->mentionable, role, "mentionable");
  put_bool(&r->managed, role, "managed");

  if ((v = json_get(role, "color")) != nullptr) {
    r->color = (uint32_t)snowflake_of(v) & 0xffffff;
  }

  if ((v = json_get(role, "position")) != nullptr) {
    r->position = (int32_t)json_coerce(v, (int64_t)0);
  }

  if ((v = json_get(role, "permissions")) != nullptr) {
    r->permissions = snowflake_of(v);
  }

  return r;
}

static void roles_changed(Store *s, Guild *g) {
  qsort(g->roles, g->nroles, sizeof *g->roles, compare_roles);
  g->members_version++;
  touch_layout(s, g);
}

Role *store_put_role(Store *s, Guild *g, const cima_json *role) {
  Role *r = put_role(g, role);

  if (r != nullptr) {
    roles_changed(s, g);
  }

  return r;
}

void store_remove_role(Store *s, Guild *g, Snowflake id) {
  for (size_t i = 0; i < g->nroles; i++) {
    if (g->roles[i]->id != id) {
      continue;
    }

    free_role(g->roles[i]);
    memmove(&g->roles[i], &g->roles[i + 1],
            (g->nroles - i - 1) * sizeof *g->roles);
    g->nroles--;
    roles_changed(s, g);
    return;
  }
}

void store_put_emojis(Store *s, Guild *g, const cima_json *emojis) {
  free_emojis(g);

  size_t n = (json_type(emojis) == CIMA_JSON_ARRAY) ? json_len(emojis) : 0;

  if (n != 0) {
    g->emojis = xcalloc(n, sizeof *g->emojis);
  }

  CIMA_EACH(item, json_items(emojis)) {
    Snowflake id = snowflake_of(json_get(item, "id"));

    if (id == 0) {
      continue;
    }

    g->emojis[g->nemojis++] = (Emoji){
      .id = id,
      .name = str_dup(json_get_or(item, "name", nullptr)),
      .animated = json_get_coerce(item, "animated", false),
      .available = json_get_coerce(item, "available", true),
    };
  }

  store_touch_guild(s, g);
}

Member *store_put_member(Store *s, Guild *g, const cima_json *member,
                         Snowflake user_id) {
  const cima_json *user = json_get(member, "user");

  if (json_type(user) == CIMA_JSON_OBJECT) {
    store_put_user(s, user);
    user_id = snowflake_of(json_get(user, "id"));
  }
  else if (snowflake_of(json_get(member, "user_id")) != 0) {
    user_id = snowflake_of(json_get(member, "user_id"));
  }

  if (g == nullptr || user_id == 0 || json_type(member) != CIMA_JSON_OBJECT) {
    return nullptr;
  }

  bool created = false;
  Member **slot = MAP_PUT(&g->members, user_id, &created);

  if (created) {
    *slot = xcalloc(1, sizeof **slot);
    (*slot)->user_id = user_id;
  }

  Member *m = *slot;
  const cima_json *v;

  put_str(&m->nick, member, "nick");
  put_str(&m->avatar, member, "avatar");
  put_decoration(&m->decoration, member);
  put_bool(&m->pending, member, "pending");

  if ((v = json_get(member, "roles")) != nullptr) {
    free(m->roles);
    m->roles = id_array(v, nullptr, &m->nroles);
  }

  if ((v = json_get(member, "joined_at")) != nullptr) {
    m->joined_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  if ((v = json_get(member, "communication_disabled_until")) != nullptr) {
    m->timeout_until = iso8601_parse_ms(json_or(v, nullptr));
  }

  g->members_version++;

  // the current user's roles decide what they see.
  if (user_id == s->self_id) {
    touch_layout(s, g);
  }
  else {
    store_touch_guild(s, g);
  }

  return m;
}

void store_remove_member(Store *s, Guild *g, Snowflake user_id) {
  Member *m = store_member(g, user_id);

  if (m == nullptr) {
    return;
  }

  CILO_MAP_DEL(&g->members, user_id);
  free_member(m);
  g->members_version++;
  store_touch_guild(s, g);
}

static void free_channel(Channel *c);

static void guild_add_channel(Guild *g, Channel *c) {
  for (size_t i = 0; i < g->nchannels; i++) {
    if (g->channels[i] == c) {
      return;
    }
  }

  g->channels = resized(g->channels, g->nchannels + 1, sizeof *g->channels);
  g->channels[g->nchannels++] = c;
}

static void guild_drop_channel(Guild *g, const Channel *c) {
  for (size_t i = 0; i < g->nchannels; i++) {
    if (g->channels[i] == c) {
      g->channels[i] = g->channels[--g->nchannels];
      return;
    }
  }
}

static void free_folders(Store *s) {
  for (size_t i = 0; i < s->nfolders; i++) {
    free(s->folders[i].guilds);
  }

  free(s->folders);
  s->folders = nullptr;
  s->nfolders = 0;
}

static void free_guild(Store *s, Guild *g) {
  for (size_t i = 0; i < g->nchannels; i++) {
    CILO_MAP_DEL(&s->channels, g->channels[i]->id);
    free_channel(g->channels[i]);
  }

  for (size_t i = 0; i < g->nroles; i++) {
    free_role(g->roles[i]);
  }

  CILO_MAP_EACH(entry, &g->members) {
    free_member(entry->val);
  }

  for (size_t i = 0; i < g->nlists; i++) {
    free_list(g->lists[i]);
  }

  MAP_DROP(&g->members);
  free_emojis(g);
  free(g->channels);
  free(g->roles);
  free(g->lists);
  free(g->rows);
  free(g->name);
  free(g->icon);
  free(g->banner);
  free(g->splash);
  free(g->description);
  free(g);
}

// the fields a guild object carries about the guild itself. with client
// state v2 they sit in `properties`, without it next to everything else.
static void fill_guild(Guild *g, const cima_json *guild) {
  const cima_json *props = json_get(guild, "properties");
  const cima_json *v;

  if (json_type(props) != CIMA_JSON_OBJECT) {
    props = guild;
  }

  put_str(&g->name, props, "name");
  put_str(&g->icon, props, "icon");
  put_str(&g->banner, props, "banner");
  put_str(&g->splash, props, "splash");
  put_str(&g->description, props, "description");
  put_id(&g->owner_id, props, "owner_id");
  put_id(&g->system_channel_id, props, "system_channel_id");
  put_id(&g->rules_channel_id, props, "rules_channel_id");

  if ((v = json_get(props, "premium_tier")) != nullptr) {
    g->premium_tier = (uint8_t)snowflake_of(v);
  }

  v = json_get(guild, "premium_subscription_count");

  if (v == nullptr) {
    v = json_get(props, "premium_subscription_count");
  }

  if (v != nullptr) {
    g->premium_subscription_count = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(guild, "member_count")) != nullptr) {
    g->member_count = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(guild, "joined_at")) != nullptr) {
    g->joined_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  put_bool(&g->large, guild, "large");

  if ((v = json_get(props, "features")) != nullptr) {
    g->community = false;

    CIMA_EACH(feature, json_items(v)) {
      const char *name = json_or(feature, nullptr);
      g->community = g->community || (name != nullptr &&
                                      strcmp(name, "COMMUNITY") == 0);
    }
  }

  // the guild's default stands until the user picks a level of their own.
  v = json_get(props, "default_message_notifications");

  if (v != nullptr && !g->has_settings) {
    g->notifications = (NotifyLevel)snowflake_of(v);
  }
}

static bool array_has_id(const cima_json *array, Snowflake id) {
  CIMA_EACH(item, json_items(array)) {
    if (snowflake_of(json_get(item, "id")) == id) {
      return true;
    }
  }

  return false;
}

Guild *store_put_guild(Store *s, const cima_json *guild) {
  Snowflake id = snowflake_of(json_get(guild, "id"));

  if (id == 0) {
    return nullptr;
  }

  bool created = false;
  Guild **slot = MAP_PUT(&s->guilds, id, &created);

  if (created) {
    *slot = xcalloc(1, sizeof **slot);
    (*slot)->id = id;
    (*slot)->notifications = NOTIFY_ALL_MESSAGES;

    // a guild joined while running goes first, like discord places it.
    s->guild_order =
      resized(s->guild_order, s->nguild_order + 1, sizeof *s->guild_order);
    size_t at = s->ready ? 0 : s->nguild_order;
    memmove(&s->guild_order[at + 1], &s->guild_order[at],
            (s->nguild_order - at) * sizeof *s->guild_order);
    s->guild_order[at] = id;
    s->nguild_order++;
  }

  Guild *g = *slot;
  s->guilds_version++;

  if (json_get_coerce(guild, "unavailable", false)) {
    g->unavailable = true;
    touch_layout(s, g);
    return g;
  }

  g->unavailable = false;
  fill_guild(g, guild);

  const cima_json *roles = json_get(guild, "roles");
  const cima_json *channels = json_get(guild, "channels");

  if (json_type(roles) == CIMA_JSON_ARRAY) {
    for (size_t i = g->nroles; i > 0; i--) {
      if (!array_has_id(roles, g->roles[i - 1]->id)) {
        free_role(g->roles[i - 1]);
        g->roles[i - 1] = g->roles[--g->nroles];
      }
    }

    CIMA_EACH(role, json_items(roles)) {
      put_role(g, role);
    }

    qsort(g->roles, g->nroles, sizeof *g->roles, compare_roles);
  }

  if (json_get(guild, "emojis") != nullptr) {
    store_put_emojis(s, g, json_get(guild, "emojis"));
  }

  if (json_type(channels) == CIMA_JSON_ARRAY) {
    // channels deleted while the guild was away are not in the new list.
    for (size_t i = g->nchannels; i > 0; i--) {
      Channel *c = g->channels[i - 1];

      if (!channel_is_thread(c) && !array_has_id(channels, c->id)) {
        store_remove_channel(s, c->id);
        // removing swaps channels around, so the scan starts over.
        i = g->nchannels + 1;
      }
    }

    CIMA_EACH(channel, json_items(channels)) {
      store_put_channel(s, channel, id);
    }
  }

  CIMA_EACH(thread, json_items(json_get(guild, "threads"))) {
    store_put_channel(s, thread, id);
  }

  CIMA_EACH(member, json_items(json_get(guild, "members"))) {
    store_put_member(s, g, member, 0);
  }

  CIMA_EACH(presence, json_items(json_get(guild, "presences"))) {
    store_put_presence(s, presence);
  }

  if (json_get(guild, "voice_states") != nullptr) {
    store_remove_voice_state(s, 0, id);

    CIMA_EACH(state, json_items(json_get(guild, "voice_states"))) {
      store_put_voice_state(s, state, id);
    }
  }

  g->members_version++;
  touch_layout(s, g);
  return g;
}

void store_update_guild(Store *s, const cima_json *guild) {
  Guild *g = store_guild(s, snowflake_of(json_get(guild, "id")));

  if (g == nullptr) {
    return;
  }

  fill_guild(g, guild);

  const cima_json *roles = json_get(guild, "roles");

  if (json_type(roles) == CIMA_JSON_ARRAY) {
    CIMA_EACH(role, json_items(roles)) {
      put_role(g, role);
    }

    qsort(g->roles, g->nroles, sizeof *g->roles, compare_roles);
    g->members_version++;
  }

  if (json_type(json_get(guild, "emojis")) == CIMA_JSON_ARRAY) {
    store_put_emojis(s, g, json_get(guild, "emojis"));
  }

  s->guilds_version++;
  touch_layout(s, g);
}

void store_remove_guild(Store *s, Snowflake id) {
  Guild *g = store_guild(s, id);

  if (g == nullptr) {
    return;
  }

  for (size_t i = 0; i < s->nguild_order; i++) {
    if (s->guild_order[i] == id) {
      memmove(&s->guild_order[i], &s->guild_order[i + 1],
              (s->nguild_order - i - 1) * sizeof *s->guild_order);
      s->nguild_order--;
      break;
    }
  }

  // the folders hold pointers to the guild.
  free_folders(s);
  s->folders_stamp = 0;

  store_remove_voice_state(s, 0, id);

  CILO_MAP_DEL(&s->guilds, id);
  free_guild(s, g);
  s->guilds_version++;
  s->version++;
}

/* ========================================================================== */
/* channels.                                                                  */
/* ========================================================================== */

static void free_channel(Channel *c) {
  for (size_t i = 0; i < c->nmessages; i++) {
    free_message(c->messages[i]);
  }

  free(c->messages);
  free(c->name);
  free(c->topic);
  free(c->icon);
  free(c->overwrites);
  free(c->recipients);
  free(c->typing);
  free(c);
}

static void fill_overwrites(Channel *c, const cima_json *overwrites) {
  size_t n =
    (json_type(overwrites) == CIMA_JSON_ARRAY) ? json_len(overwrites) : 0;

  free(c->overwrites);
  c->overwrites = n != 0 ? xcalloc(n, sizeof *c->overwrites) : nullptr;
  c->noverwrites = 0;

  CIMA_EACH(item, json_items(overwrites)) {
    const cima_json *type = json_get(item, "type");
    Snowflake id = snowflake_of(json_get(item, "id"));

    if (id == 0) {
      continue;
    }

    // old payloads name the type, new ones number it.
    bool member = (json_type(type) == CIMA_JSON_STRING)
                    ? strcmp(json_or(type, ""), "member") == 0
                    : snowflake_of(type) == 1;

    c->overwrites[c->noverwrites++] = (Overwrite){
      .id = id,
      .type = member ? OVERWRITE_MEMBER : OVERWRITE_ROLE,
      .allow = snowflake_of(json_get(item, "allow")),
      .deny = snowflake_of(json_get(item, "deny")),
    };
  }
}

Channel *store_put_channel(Store *s, const cima_json *channel,
                           Snowflake guild_id) {
  Snowflake id = snowflake_of(json_get(channel, "id"));

  if (id == 0) {
    return nullptr;
  }

  if (snowflake_of(json_get(channel, "guild_id")) != 0) {
    guild_id = snowflake_of(json_get(channel, "guild_id"));
  }

  bool created = false;
  Channel **slot = MAP_PUT(&s->channels, id, &created);

  if (created) {
    *slot = xcalloc(1, sizeof **slot);
    (*slot)->id = id;
    (*slot)->guild_id = guild_id;
    (*slot)->notifications = NOTIFY_INHERIT;
  }

  Channel *c = *slot;
  Guild *g = store_guild(s, c->guild_id);
  const cima_json *v;

  if (created && g != nullptr) {
    guild_add_channel(g, c);
  }

  if ((v = json_get(channel, "type")) != nullptr) {
    c->type = (uint8_t)snowflake_of(v);
  }

  put_str(&c->name, channel, "name");
  put_str(&c->topic, channel, "topic");
  put_str(&c->icon, channel, "icon");
  put_id(&c->parent_id, channel, "parent_id");
  put_id(&c->owner_id, channel, "owner_id");
  put_bool(&c->nsfw, channel, "nsfw");

  if ((v = json_get(channel, "position")) != nullptr) {
    c->position = (int32_t)json_coerce(v, (int64_t)0);
  }

  if ((v = json_get(channel, "flags")) != nullptr) {
    c->flags = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(channel, "rate_limit_per_user")) != nullptr) {
    c->rate_limit_per_user = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(channel, "permission_overwrites")) != nullptr) {
    fill_overwrites(c, v);
  }

  // an update never moves the last message back.
  Snowflake last = snowflake_of(json_get(channel, "last_message_id"));

  if (last > c->last_message_id) {
    c->last_message_id = last;
  }

  // recipients come whole, or as ids when ready deduplicates users.
  const cima_json *recipients = json_get(channel, "recipients");
  const cima_json *recipient_ids = json_get(channel, "recipient_ids");

  if (json_type(recipients) == CIMA_JSON_ARRAY) {
    CIMA_EACH(user, json_items(recipients)) {
      store_put_user(s, user);
    }

    free(c->recipients);
    c->recipients = id_array(recipients, "id", &c->nrecipients);
  }
  else if (json_type(recipient_ids) == CIMA_JSON_ARRAY) {
    free(c->recipients);
    c->recipients = id_array(recipient_ids, nullptr, &c->nrecipients);
  }

  const cima_json *thread = json_get(channel, "thread_metadata");

  if (json_type(thread) == CIMA_JSON_OBJECT) {
    put_bool(&c->thread_archived, thread, "archived");
    put_bool(&c->thread_locked, thread, "locked");

    if ((v = json_get(thread, "archive_timestamp")) != nullptr) {
      c->thread_archived_at = iso8601_parse_ms(json_or(v, nullptr));
    }
  }

  // a thread comes with the current user's membership when they joined it.
  const cima_json *membership = json_get(channel, "member");

  if (json_type(membership) == CIMA_JSON_OBJECT) {
    c->thread_joined = true;
    c->thread_joined_at =
      iso8601_parse_ms(json_get_or(membership, "join_timestamp", nullptr));
    put_bool(&c->muted, membership, "muted");

    if (json_get(membership, "mute_config") != nullptr) {
      c->mute_until = mute_end(membership);
    }
  }

  if (json_get(channel, "is_message_request") != nullptr ||
      json_get(channel, "is_spam") != nullptr) {
    c->request = json_coerce(json_get(channel, "is_message_request"), false) ||
                 json_coerce(json_get(channel, "is_spam"), false);
  }

  if ((v = json_get(channel, "message_count")) != nullptr) {
    c->message_count = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(channel, "member_count")) != nullptr) {
    c->member_count = (uint32_t)snowflake_of(v);
  }

  if (g != nullptr) {
    g->layout_version++;
  }
  else {
    s->private_version++;
  }

  store_touch_channel(s, c);
  return c;
}

void store_remove_channel(Store *s, Snowflake id) {
  Channel *c = store_channel(s, id);

  if (c == nullptr) {
    return;
  }

  Guild *g = store_guild(s, c->guild_id);

  if (g != nullptr) {
    // its threads go with it.
    for (size_t i = g->nchannels; i > 0; i--) {
      Channel *thread = g->channels[i - 1];

      if (thread->parent_id == id && channel_is_thread(thread)) {
        guild_drop_channel(g, thread);
        CILO_MAP_DEL(&s->channels, thread->id);
        free_channel(thread);
        // removing swaps channels around, so the scan starts over.
        i = g->nchannels + 1;
      }
    }

    guild_drop_channel(g, c);
    touch_layout(s, g);
  }
  else {
    s->private_version++;
    s->version++;
  }

  CILO_MAP_DEL(&s->channels, id);
  free_channel(c);
}

void store_recipient_add(Store *s, Snowflake channel_id,
                         const cima_json *user) {
  Channel *c = store_channel(s, channel_id);
  User *u = store_put_user(s, user);

  if (c == nullptr || u == nullptr) {
    return;
  }

  for (size_t i = 0; i < c->nrecipients; i++) {
    if (c->recipients[i] == u->id) {
      return;
    }
  }

  c->recipients =
    resized(c->recipients, c->nrecipients + 1, sizeof *c->recipients);
  c->recipients[c->nrecipients++] = u->id;
  s->private_version++;
  store_touch_channel(s, c);
}

void store_recipient_remove(Store *s, Snowflake channel_id, Snowflake user_id) {
  Channel *c = store_channel(s, channel_id);

  if (c == nullptr) {
    return;
  }

  for (size_t i = 0; i < c->nrecipients; i++) {
    if (c->recipients[i] != user_id) {
      continue;
    }

    memmove(&c->recipients[i], &c->recipients[i + 1],
            (c->nrecipients - i - 1) * sizeof *c->recipients);
    c->nrecipients--;
    s->private_version++;
    store_touch_channel(s, c);
    return;
  }
}

void store_sync_threads(Store *s, const cima_json *sync) {
  Snowflake guild_id = snowflake_of(json_get(sync, "guild_id"));
  Guild *g = store_guild(s, guild_id);

  if (g == nullptr) {
    return;
  }

  const cima_json *parents = json_get(sync, "channel_ids");
  const cima_json *threads = json_get(sync, "threads");

  // the threads of the synced channels that are not listed anymore are no
  // longer active.
  for (size_t i = g->nchannels; i > 0; i--) {
    Channel *c = g->channels[i - 1];

    if (!channel_is_thread(c) || array_has_id(threads, c->id)) {
      continue;
    }

    bool synced = json_type(parents) != CIMA_JSON_ARRAY;

    CIMA_EACH(parent, json_items(parents)) {
      synced = synced || snowflake_of(parent) == c->parent_id;
    }

    if (synced) {
      store_remove_channel(s, c->id);
      // removing swaps channels around, so the scan starts over.
      i = g->nchannels + 1;
    }
  }

  CIMA_EACH(thread, json_items(threads)) {
    store_put_channel(s, thread, guild_id);
  }

  CIMA_EACH(member, json_items(json_get(sync, "members"))) {
    Channel *c = store_channel(s, snowflake_of(json_get(member, "id")));

    if (c != nullptr) {
      c->thread_joined = true;
      c->thread_joined_at =
        iso8601_parse_ms(json_get_or(member, "join_timestamp", nullptr));
    }
  }

  touch_layout(s, g);
}

void store_set_thread_joined(Store *s, Snowflake channel_id, bool joined) {
  Channel *c = store_channel(s, channel_id);
  Guild *g = c != nullptr ? store_guild(s, c->guild_id) : nullptr;

  if (c == nullptr || c->thread_joined == joined) {
    return;
  }

  c->thread_joined = joined;
  // the event's own time is seconds away at most.
  c->thread_joined_at = joined ? now_ms(s) : 0;

  if (g != nullptr) {
    g->layout_version++;
  }

  store_touch_channel(s, c);
}

void store_set_last_message(Store *s, Snowflake channel_id, Snowflake id) {
  Channel *c = store_channel(s, channel_id);

  if (c == nullptr || id <= c->last_message_id) {
    return;
  }

  c->last_message_id = id;

  if (channel_is_private(c)) {
    s->private_version++;
  }

  store_touch_channel(s, c);
}

/* ========================================================================== */
/* read states and settings.                                                  */
/* ========================================================================== */

void store_put_read_state(Store *s, const cima_json *entry) {
  Snowflake id = snowflake_of(json_get(entry, "id"));

  // only channels have messages to read, the other types badge features.
  if (id == 0 || json_get_coerce(entry, "read_state_type", (int64_t)0) != 0) {
    return;
  }

  ReadState *rs = read_state(s, id);
  const cima_json *v;

  rs->known = true;

  if ((v = json_get(entry, "last_message_id")) != nullptr) {
    rs->last_acked_id = snowflake_of(v);
  }

  if ((v = json_get(entry, "mention_count")) != nullptr) {
    rs->mention_count = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(entry, "flags")) != nullptr) {
    rs->flags = (uint32_t)snowflake_of(v);
  }

  Channel *c = store_channel(s, id);

  if (c != nullptr) {
    store_touch_channel(s, c);
  }
}

void store_ack(Store *s, Snowflake channel_id, Snowflake message_id,
               uint32_t mention_count) {
  if (channel_id == 0) {
    return;
  }

  ReadState *rs = read_state(s, channel_id);

  rs->known = true;
  rs->last_acked_id = message_id;
  rs->mention_count = mention_count;

  Channel *c = store_channel(s, channel_id);

  if (c != nullptr) {
    store_touch_channel(s, c);
  }
  else {
    s->version++;
  }
}

void store_put_ack(Store *s, const cima_json *ack) {
  // only channels are tracked, the other read state types badge features.
  if (json_get_coerce(ack, "ack_type", (int64_t)0) != 0) {
    return;
  }

  Snowflake channel_id = snowflake_of(json_get(ack, "channel_id"));
  Snowflake message_id = snowflake_of(json_get(ack, "message_id"));

  // marking as unread somewhere tells how many pings come after the
  // message. reading does not: discord's client drops every ping then, and
  // leaves an ack of what is acknowledged already alone.
  if (json_coerce(json_get(ack, "manual"), false)) {
    store_ack(s, channel_id, message_id,
              (uint32_t)json_get_coerce(ack, "mention_count", (uint64_t)0));
  }
  else if (message_id != store_last_acked(s, channel_id)) {
    store_ack(s, channel_id, message_id, 0);
  }
}

Snowflake store_last_acked(const Store *s, Snowflake channel_id) {
  const ReadState *rs = CILO_MAP_GET(&s->read_states, channel_id);
  return rs != nullptr ? rs->last_acked_id : 0;
}

static int64_t mute_end(const cima_json *settings) {
  const cima_json *config = json_get(settings, "mute_config");
  return iso8601_parse_ms(json_get_or(config, "end_time", nullptr));
}

static void apply_override(Store *s, const cima_json *override, Snowflake id) {
  if (snowflake_of(json_get(override, "channel_id")) != 0) {
    id = snowflake_of(json_get(override, "channel_id"));
  }

  Channel *c = store_channel(s, id);

  if (c == nullptr || json_type(override) != CIMA_JSON_OBJECT) {
    return;
  }

  const cima_json *v;

  put_bool(&c->muted, override, "muted");
  put_bool(&c->collapsed, override, "collapsed");

  if (json_get(override, "mute_config") != nullptr) {
    c->mute_until = mute_end(override);
  }

  if ((v = json_get(override, "message_notifications")) != nullptr) {
    c->notifications = (NotifyLevel)snowflake_of(v);
  }

  if ((v = json_get(override, "flags")) != nullptr) {
    c->override_flags = (uint32_t)snowflake_of(v);
  }

  c->version++;
}

static void clear_override(Channel *c) {
  c->muted = false;
  c->collapsed = false;
  c->mute_until = 0;
  c->notifications = NOTIFY_INHERIT;
  c->override_flags = 0;
  c->version++;
}

void store_put_guild_settings(Store *s, const cima_json *settings) {
  Snowflake guild_id = snowflake_of(json_get(settings, "guild_id"));
  Guild *g = store_guild(s, guild_id);
  const cima_json *overrides = json_get(settings, "channel_overrides");
  const cima_json *v;

  // the entry without a guild holds the overrides of dms.
  if (g != nullptr) {
    put_bool(&g->muted, settings, "muted");
    put_bool(&g->suppress_everyone, settings, "suppress_everyone");
    put_bool(&g->suppress_roles, settings, "suppress_roles");

    if (json_get(settings, "mute_config") != nullptr) {
      g->mute_until = mute_end(settings);
    }

    if ((v = json_get(settings, "message_notifications")) != nullptr) {
      g->notifications = (NotifyLevel)snowflake_of(v);
      g->has_settings = true;
    }

    if ((v = json_get(settings, "flags")) != nullptr) {
      g->settings_flags = (uint32_t)snowflake_of(v);
    }
  }

  // the whole list replaces the overrides: one that is gone was removed.
  if (json_type(overrides) == CIMA_JSON_ARRAY) {
    CILO_MAP_EACH(entry, &s->channels) {
      Channel *c = entry->val;

      if (c->guild_id == guild_id && !channel_is_thread(c)) {
        clear_override(c);
      }
    }
  }

  // an array when read, a map by channel id when it is an update's.
  CIMA_EACH(override, json_items(overrides)) {
    apply_override(s, override, 0);
  }

  CIMA_EACH(override, json_members(overrides)) {
    apply_override(s, &override->value, snowflake_parse(override->key));
  }

  if (g != nullptr) {
    store_touch_guild(s, g);
  }
  else {
    s->private_version++;
    s->version++;
  }
}

static void free_stored_folders(Store *s) {
  for (size_t i = 0; i < s->nstored_folders; i++) {
    free(s->stored_folders[i].name);
    free(s->stored_folders[i].guild_ids);
  }

  free(s->stored_folders);
  s->stored_folders = nullptr;
  s->nstored_folders = 0;

  // the folders shown borrow their names from these.
  free_folders(s);
  s->folders_stamp = 0;
}

void store_put_user_settings(Store *s, const cima_json *settings) {
  const cima_json *folders = json_get(settings, "guild_folders");
  const cima_json *positions = json_get(settings, "guild_positions");

  if (json_get(settings, "status") != nullptr) {
    const char *status = json_get_or(settings, "status", nullptr);
    bool hidden = status != nullptr && strcmp(status, "invisible") == 0;
    s->self_status = hidden ? STATUS_OFFLINE : parse_status(status);
  }

  if (json_get(settings, "status_expires_at_ms") != nullptr) {
    s->self_status_until =
      (int64_t)snowflake_of(json_get(settings, "status_expires_at_ms"));
  }

  put_str(&s->status_proto, settings, "status_proto");

  // the status the account wrote, which its presence leaves out while it
  // is invisible.
  const cima_json *custom = json_get(settings, "custom_status");

  if (custom != nullptr && s->self != nullptr) {
    User *self = s->self;
    int64_t until = (int64_t)snowflake_of(json_get(custom, "expires_at_ms"));
    bool gone = json_type(custom) != CIMA_JSON_OBJECT ||
                (until != 0 && until <= now_ms(s));
    const char *words = gone ? nullptr : json_get_or(custom, "text", nullptr);
    const char *emoji =
      gone ? nullptr : json_get_or(custom, "emoji_name", nullptr);

    set_custom_status(self, words, emoji,
                      gone ? 0 : snowflake_of(json_get(custom, "emoji_id")));
    s->users_version++;
    s->version++;
  }

  if ((json_type(folders) == CIMA_JSON_ARRAY) && json_len(folders) > 0) {
    free_stored_folders(s);
    s->stored_folders = xcalloc(json_len(folders), sizeof *s->stored_folders);

    CIMA_EACH(folder, json_items(folders)) {
      const cima_json *color = json_get(folder, "color");
      StoredFolder *f = &s->stored_folders[s->nstored_folders++];

      f->id = snowflake_of(json_get(folder, "id"));
      f->name = str_dup(json_get_or(folder, "name", nullptr));
      f->has_color = cima_json_is_number(color);
      f->color = (uint32_t)snowflake_of(color) & 0xffffff;
      f->guild_ids =
        id_array(json_get(folder, "guild_ids"), nullptr, &f->nguilds);
    }

    s->has_stored_folders = true;
  }
  else if ((json_type(positions) == CIMA_JSON_ARRAY) &&
           json_len(positions) > 0 && !s->has_stored_folders) {
    // the order from before folders existed, each guild on its own.
    free_stored_folders(s);
    s->stored_folders = xcalloc(json_len(positions), sizeof *s->stored_folders);

    CIMA_EACH(position, json_items(positions)) {
      StoredFolder *f = &s->stored_folders[s->nstored_folders++];
      f->guild_ids = xcalloc(1, sizeof *f->guild_ids);
      f->guild_ids[0] = snowflake_of(position);
      f->nguilds = 1;
    }

    s->has_stored_folders = true;
  }

  s->guilds_version++;
  s->version++;
}

/* ========================================================================== */
/* typing.                                                                    */
/* ========================================================================== */

void store_typing_start(Store *s, Snowflake channel_id, Snowflake user_id) {
  Channel *c = store_channel(s, channel_id);

  if (c == nullptr || user_id == 0 || user_id == s->self_id) {
    return;
  }

  double until = store_now(s) + TYPING_SECONDS;

  for (size_t i = 0; i < c->ntyping; i++) {
    if (c->typing[i].user_id == user_id) {
      c->typing[i].until = until;
      return;
    }
  }

  c->typing = resized(c->typing, c->ntyping + 1, sizeof *c->typing);
  c->typing[c->ntyping++] = (Typing){user_id, until};
  store_touch_channel(s, c);
}

double store_typing_expire(Store *s) {
  double now = store_now(s);
  double next = 0;

  CILO_MAP_EACH(entry, &s->channels) {
    Channel *c = entry->val;
    bool changed = false;

    for (size_t i = c->ntyping; i > 0; i--) {
      if (c->typing[i - 1].until <= now) {
        c->typing[i - 1] = c->typing[--c->ntyping];
        changed = true;
      }
    }

    for (size_t i = 0; i < c->ntyping; i++) {
      if (next == 0 || c->typing[i].until < next) {
        next = c->typing[i].until;
      }
    }

    if (changed) {
      store_touch_channel(s, c);
    }
  }

  return next;
}

size_t store_typing(const Store *s, const Channel *c, Snowflake *out,
                    size_t cap) {
  double now = store_now(s);
  size_t count = 0;

  for (size_t i = 0; c != nullptr && i < c->ntyping; i++) {
    if (c->typing[i].until <= now) {
      continue;
    }

    if (count < cap) {
      out[count] = c->typing[i].user_id;
    }

    count++;
  }

  return count;
}

/* ========================================================================== */
/* permissions and unread states.                                             */
/* ========================================================================== */

Permissions store_permissions(Store *s, const Channel *c) {
  if (c == nullptr) {
    return 0;
  }

  if (c->guild_id == 0) {
    return PERM_DM;
  }

  const Guild *g = store_guild(s, c->guild_id);

  if (g == nullptr) {
    return 0;
  }

  if (c->perms_stamp == g->layout_version + 1) {
    return c->perms;
  }

  const Member *me = store_member(g, s->self_id);
  const Role *everyone = store_role(g, g->id);
  Snowflake ids[MAX_SUBJECT_ROLES];
  Permissions granted[MAX_SUBJECT_ROLES];
  size_t nroles = 0;

  for (size_t i = 0; me != nullptr && i < me->nroles; i++) {
    const Role *role = store_role(g, me->roles[i]);

    if (role != nullptr && nroles < MAX_SUBJECT_ROLES) {
      ids[nroles] = role->id;
      granted[nroles] = role->permissions;
      nroles++;
    }
  }

  PermissionSubject who = {
    .guild_id = g->id,
    .owner_id = g->owner_id,
    .user_id = s->self_id,
    .everyone = everyone != nullptr ? everyone->permissions : 0,
    .role_ids = ids,
    .role_permissions = granted,
    .nroles = nroles,
    .timed_out = me != nullptr && me->timeout_until > now_ms(s),
  };

  // a thread has no overwrites of its own, its channel's apply.
  const Channel *source = c;
  PermissionChannelKind kind = PERM_CHANNEL_TEXT;

  if (channel_is_thread(c)) {
    const Channel *parent = store_channel(s, c->parent_id);
    source = parent != nullptr ? parent : c;
    kind = PERM_CHANNEL_THREAD;
  }
  else if (channel_is_voice(c)) {
    kind = PERM_CHANNEL_VOICE;
  }

  Permissions perms =
    permissions_in_channel(&who, source->overwrites, source->noverwrites, kind);

  // the cache is the store's own, behind a pointer the reader may not write
  // through.
  Channel *cached = store_channel(s, c->id);

  if (cached != nullptr) {
    cached->perms = perms;
    cached->perms_stamp = g->layout_version + 1;
  }

  return perms;
}

static bool mute_active(bool muted, int64_t until, int64_t now) {
  return muted && (until == 0 || until > now);
}

static bool channel_muted(const Store *s, const Channel *c) {
  int64_t now = now_ms(s);

  if (mute_active(c->muted, c->mute_until, now)) {
    return true;
  }

  // a muted category mutes what it holds, a muted channel its threads.
  const Channel *parent = store_channel(s, c->parent_id);

  if (parent != nullptr) {
    if (mute_active(parent->muted, parent->mute_until, now)) {
      return true;
    }

    const Channel *category = store_channel(s, parent->parent_id);

    if (category != nullptr &&
        mute_active(category->muted, category->mute_until, now)) {
      return true;
    }
  }

  const Guild *g = store_guild(s, c->guild_id);
  return g != nullptr && mute_active(g->muted, g->mute_until, now);
}

static NotifyLevel notify_level(const Store *s, const Channel *c) {
  if (c->notifications != NOTIFY_INHERIT) {
    return c->notifications;
  }

  const Channel *parent = store_channel(s, c->parent_id);

  if (parent != nullptr && parent->notifications != NOTIFY_INHERIT) {
    return parent->notifications;
  }

  const Guild *g = store_guild(s, c->guild_id);
  return g != nullptr ? g->notifications : NOTIFY_ALL_MESSAGES;
}

// whether unread messages show, or only pings do: the channel's setting,
// its parent's, the guild's, then what the notifications ask for.
static bool shows_unreads(const Store *s, const Channel *c) {
  bool modern = (s->notify_flags & NOTIFY_FLAG_NEW_NOTIFICATIONS) != 0;

  if (!modern || channel_is_thread(c) || channel_is_private(c)) {
    return true;
  }

  const Channel *parent = store_channel(s, c->parent_id);
  const Guild *g = store_guild(s, c->guild_id);
  uint32_t flags[] = {c->override_flags,
                      parent != nullptr ? parent->override_flags : 0};

  for (size_t i = 0; i < 2; i++) {
    if ((flags[i] & OVERRIDE_UNREADS_ALL_MESSAGES) != 0) {
      return true;
    }

    if ((flags[i] & OVERRIDE_UNREADS_ONLY_MENTIONS) != 0) {
      return false;
    }
  }

  if (g != nullptr) {
    if ((g->settings_flags & GUILD_SETTINGS_UNREADS_ALL_MESSAGES) != 0) {
      return true;
    }

    if ((g->settings_flags & GUILD_SETTINGS_UNREADS_ONLY_MENTIONS) != 0) {
      return false;
    }
  }

  return notify_level(s, c) == NOTIFY_ALL_MESSAGES;
}

// when the channel was read up to, in unix milliseconds. one never
// acknowledged counts as read up to when the user came to it.
static int64_t ack_time(const Store *s, const Channel *c, const ReadState *rs) {
  if (rs != nullptr && rs->last_acked_id != 0) {
    return snowflake_time_ms(rs->last_acked_id);
  }

  const Guild *g = store_guild(s, c->guild_id);
  int64_t joined = g != nullptr ? g->joined_at : 0;

  if (channel_is_thread(c)) {
    const Channel *parent = store_channel(s, c->parent_id);
    int64_t entered = c->thread_joined_at - 5000;
    int64_t archived =
      c->thread_archived_at != 0 ? c->thread_archived_at - 1 : 0;
    int64_t since = entered > archived ? entered : archived;

    if (since <= 0) {
      since = snowflake_time_ms(c->id) - 1;
    }

    // a post of a forum is new to who joined the guild after it.
    if (parent != nullptr && (parent->type == CHANNEL_GUILD_FORUM ||
                              parent->type == CHANNEL_GUILD_MEDIA)) {
      joined = 0;
    }

    return joined > since ? joined : since;
  }

  return joined != 0 ? joined : snowflake_time_ms(c->id);
}

constexpr int64_t DAY_MS = 86400000;

Unread store_channel_unread(Store *s, const Channel *c) {
  Unread unread = {};

  if (c == nullptr || !channel_has_text(c)) {
    return unread;
  }

  unread.muted = channel_muted(s, c);

  bool thread = channel_is_thread(c);

  // an archived thread, a resource channel and a channel out of view track
  // nothing. neither does a thread before it is joined, nor a dm waiting
  // as a request.
  if ((thread && (c->thread_archived || !c->thread_joined)) || c->request ||
      (c->flags & CHANNEL_FLAG_RESOURCE) != 0 ||
      (c->guild_id != 0 && !store_can(s, c, PERM_VIEW_CHANNEL))) {
    return unread;
  }

  const ReadState *rs = CILO_MAP_GET(&s->read_states, c->id);
  const Guild *g = store_guild(s, c->guild_id);
  int64_t now = now_ms(s);
  int64_t last =
    c->last_message_id != 0 ? snowflake_time_ms(c->last_message_id) : 0;
  int64_t acked = ack_time(s, c, rs);
  uint32_t mentions = rs != nullptr ? rs->mention_count : 0;

  // a community where channels are picked one by one: nothing older than
  // a week shows, and a channel left out only does for a ping or when it
  // was read in the last three days.
  bool picked = g != nullptr && g->community &&
                (g->settings_flags & GUILD_SETTINGS_OPT_IN_CHANNELS_ON) != 0;
  bool stale = picked && last < now - 7 * DAY_MS;

  unread.mentions = stale ? 0 : mentions;
  unread.pending = acked < last;
  unread.unread = unread.pending && shows_unreads(s, c);

  if (picked && !thread) {
    const Channel *parent = store_channel(s, c->parent_id);
    bool kept = (c->override_flags & OVERRIDE_OPT_IN_ENABLED) != 0 ||
                (parent != nullptr &&
                 (parent->override_flags & OVERRIDE_OPT_IN_ENABLED) != 0);
    bool visited = last > 0 && rs != nullptr && rs->last_acked_id != 0 &&
                   acked > now - 3 * DAY_MS;

    if (stale || (!kept && !visited && mentions == 0)) {
      unread.unread = false;
    }
  }

  return unread;
}

Unread store_guild_unread(Store *s, Guild *g) {
  if (g == nullptr) {
    return (Unread){};
  }

  if (g->unread_stamp == g->version + 1) {
    return g->unread;
  }

  int64_t now = now_ms(s);
  Unread total = {.muted = mute_active(g->muted, g->mute_until, now)};

  for (size_t i = 0; i < g->nchannels; i++) {
    const Channel *c = g->channels[i];
    Unread unread = store_channel_unread(s, c);
    // a thread only answers for its own mute, not for its channel's.
    bool muted = channel_is_thread(c)
                   ? mute_active(c->muted, c->mute_until, now)
                   : unread.muted;

    total.mentions += unread.mentions;

    // the chat of a voice channel only tells of its pings.
    if (!channel_is_voice(c)) {
      total.unread = total.unread || (unread.unread && !muted);
    }
  }

  if (total.muted) {
    total.unread = false;
  }

  g->unread = total;
  g->unread_stamp = g->version + 1;
  return total;
}

Unread store_private_unread(Store *s) {
  if (s->private_unread_stamp == s->version + 1) {
    return s->private_unread;
  }

  Unread total = {};

  CILO_MAP_EACH(entry, &s->channels) {
    const Channel *c = entry->val;

    if (!channel_is_private(c)) {
      continue;
    }

    Unread unread = store_channel_unread(s, c);

    total.mentions += unread.mentions;
    total.unread = total.unread || (unread.unread && !unread.muted);
  }

  s->private_unread = total;
  s->private_unread_stamp = s->version + 1;
  return total;
}

/* ========================================================================== */
/* the lists the ui shows.                                                    */
/* ========================================================================== */

static bool folder_holds(const Store *s, Snowflake guild_id) {
  for (size_t i = 0; i < s->nstored_folders; i++) {
    const StoredFolder *f = &s->stored_folders[i];

    for (size_t j = 0; j < f->nguilds; j++) {
      if (f->guild_ids[j] == guild_id) {
        return true;
      }
    }
  }

  return false;
}

static GuildFolder *add_folder(Store *s, size_t *cap, size_t nguilds) {
  reserve(&s->folders, cap, s->nfolders + 1, sizeof *s->folders);

  GuildFolder *f = &s->folders[s->nfolders++];
  *f = (GuildFolder){.guilds = xcalloc(nguilds, sizeof *f->guilds)};
  return f;
}

const GuildFolder *store_guild_folders(Store *s, size_t *count) {
  if (s->folders_stamp == s->guilds_version + 1) {
    *count = s->nfolders;
    return s->folders;
  }

  free_folders(s);

  size_t cap = 0;

  // guilds the user never placed go first, where discord puts new ones.
  for (size_t i = 0; i < s->nguild_order; i++) {
    Guild *g = store_guild(s, s->guild_order[i]);

    if (g == nullptr || folder_holds(s, g->id)) {
      continue;
    }

    GuildFolder *f = add_folder(s, &cap, 1);
    f->guilds[f->nguilds++] = g;
  }

  for (size_t i = 0; i < s->nstored_folders; i++) {
    const StoredFolder *stored = &s->stored_folders[i];
    GuildFolder *f = add_folder(s, &cap, stored->nguilds);

    for (size_t j = 0; j < stored->nguilds; j++) {
      Guild *g = store_guild(s, stored->guild_ids[j]);

      if (g != nullptr) {
        f->guilds[f->nguilds++] = g;
      }
    }

    // a folder whose guilds were all left is not shown.
    if (f->nguilds == 0) {
      free(f->guilds);
      s->nfolders--;
      continue;
    }

    f->id = stored->id;
    f->name = stored->name;
    f->color = stored->color;
    f->has_color = stored->has_color;
  }

  s->folders_stamp = s->guilds_version + 1;
  *count = s->nfolders;
  return s->folders;
}

static int compare_position(const void *a, const void *b) {
  const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;
  bool x_voice = channel_is_voice(x), y_voice = channel_is_voice(y);

  if (x_voice != y_voice) {
    return x_voice ? 1 : -1;
  }

  if (x->position != y->position) {
    return x->position < y->position ? -1 : 1;
  }

  return x->id < y->id ? -1 : x->id > y->id;
}

// the most recently active thread first.
static int compare_recent(const void *a, const void *b) {
  const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;
  Snowflake x_last = x->last_message_id > x->id ? x->last_message_id : x->id;
  Snowflake y_last = y->last_message_id > y->id ? y->last_message_id : y->id;

  return x_last > y_last ? -1 : x_last < y_last;
}

static void add_row(Guild *g, size_t *cap, ChannelRowKind kind, Channel *c) {
  reserve(&g->rows, cap, g->nrows + 1, sizeof *g->rows);
  g->rows[g->nrows++] = (ChannelRow){kind, c};
}

static void add_channel_rows(Guild *g, size_t *cap, Channel *c,
                             Channel **threads, size_t nthreads) {
  add_row(g, cap, CHANNEL_ROW_CHANNEL, c);

  for (size_t i = 0; i < nthreads; i++) {
    if (threads[i]->parent_id == c->id) {
      add_row(g, cap, CHANNEL_ROW_THREAD, threads[i]);
    }
  }
}

const ChannelRow *store_guild_channels(Store *s, Guild *g, size_t *count) {
  if (g == nullptr) {
    *count = 0;
    return nullptr;
  }

  if (g->rows_stamp == g->layout_version + 1) {
    *count = g->nrows;
    return g->rows;
  }

  size_t n = g->nchannels;
  Channel **categories = xcalloc(n, sizeof *categories);
  Channel **channels = xcalloc(n, sizeof *channels);
  Channel **threads = xcalloc(n, sizeof *threads);
  size_t ncategories = 0, nchannels = 0, nthreads = 0;

  for (size_t i = 0; i < n; i++) {
    Channel *c = g->channels[i];

    if (c->type == CHANNEL_GUILD_CATEGORY) {
      categories[ncategories++] = c;
    }
    else if (channel_is_thread(c)) {
      if (c->thread_joined && !c->thread_archived &&
          store_can(s, c, PERM_VIEW_CHANNEL)) {
        threads[nthreads++] = c;
      }
    }
    else if (store_can(s, c, PERM_VIEW_CHANNEL)) {
      channels[nchannels++] = c;
    }
  }

  qsort(categories, ncategories, sizeof *categories, compare_position);
  qsort(channels, nchannels, sizeof *channels, compare_position);
  qsort(threads, nthreads, sizeof *threads, compare_recent);

  size_t cap = 0;
  free(g->rows);
  g->rows = nullptr;
  g->nrows = 0;

  // the channels outside any category come first.
  for (size_t i = 0; i < nchannels; i++) {
    const Channel *parent = store_channel(s, channels[i]->parent_id);
    bool in_category =
      parent != nullptr && parent->type == CHANNEL_GUILD_CATEGORY;

    if (!in_category) {
      add_channel_rows(g, &cap, channels[i], threads, nthreads);
    }
  }

  for (size_t i = 0; i < ncategories; i++) {
    Channel *category = categories[i];
    size_t before = g->nrows;

    add_row(g, &cap, CHANNEL_ROW_CATEGORY, category);

    for (size_t j = 0; j < nchannels; j++) {
      if (channels[j]->parent_id == category->id) {
        add_channel_rows(g, &cap, channels[j], threads, nthreads);
      }
    }

    bool empty = g->nrows == before + 1;

    if (empty && !store_can(s, category, PERM_MANAGE_CHANNELS)) {
      g->nrows = before;
    }
  }

  free(categories);
  free(channels);
  free(threads);

  g->rows_stamp = g->layout_version + 1;
  *count = g->nrows;
  return g->rows;
}

static int compare_private(const void *a, const void *b) {
  return compare_recent(a, b);
}

Channel *const *store_private_channels(Store *s, size_t *count) {
  if (s->privates_stamp == s->private_version + 1) {
    *count = s->nprivates;
    return s->privates;
  }

  size_t cap = 0;
  free(s->privates);
  s->privates = nullptr;
  s->nprivates = 0;

  CILO_MAP_EACH(entry, &s->channels) {
    if (channel_is_private(entry->val)) {
      reserve(&s->privates, &cap, s->nprivates + 1, sizeof *s->privates);
      s->privates[s->nprivates++] = entry->val;
    }
  }

  if (s->nprivates > 1) {
    qsort(s->privates, s->nprivates, sizeof *s->privates, compare_private);
  }

  s->privates_stamp = s->private_version + 1;
  *count = s->nprivates;
  return s->privates;
}

/* ========================================================================== */
/* member lists.                                                              */
/* ========================================================================== */

static uint32_t murmur3(const char *key, size_t len) {
  uint32_t h = 0;
  size_t blocks = len / 4;

  for (size_t i = 0; i < blocks; i++) {
    uint32_t k;
    memcpy(&k, key + i * 4, 4);

    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;

    h ^= k;
    h = (h << 13) | (h >> 19);
    h = h * 5 + 0xe6546b64;
  }

  const unsigned char *tail = (const unsigned char *)key + blocks * 4;
  uint32_t k = 0;

  switch (len & 3) {
    case 3:
      k ^= (uint32_t)tail[2] << 16;
      [[fallthrough]];
    case 2:
      k ^= (uint32_t)tail[1] << 8;
      [[fallthrough]];
    case 1:
      k ^= tail[0];
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      break;
    default:
      break;
  }

  h ^= (uint32_t)len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

static int compare_text(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// learned from the web client: channels everyone can view share the list
// "everyone", the others the murmur3 hash of their overwrites touching
// VIEW_CHANNEL, written allow:id or deny:id, sorted and joined by commas.
void store_member_list_id(const Store *s, const Channel *c, char *out,
                          size_t cap) {
  snprintf(out, cap, "everyone");

  if (c == nullptr || c->guild_id == 0) {
    return;
  }

  if (channel_is_thread(c)) {
    const Channel *parent = store_channel(s, c->parent_id);

    if (parent != nullptr) {
      c = parent;
    }
  }

  const Guild *g = store_guild(s, c->guild_id);
  const Role *everyone = store_role(g, c->guild_id);
  Permissions base = everyone != nullptr ? everyone->permissions : 0;

  for (size_t i = 0; i < c->noverwrites; i++) {
    if (c->overwrites[i].id == c->guild_id) {
      base &= ~c->overwrites[i].deny;
      base |= c->overwrites[i].allow;
    }
  }

  if ((base & PERM_VIEW_CHANNEL) || c->noverwrites == 0) {
    return;
  }

  char **parts = xcalloc(c->noverwrites, sizeof *parts);
  size_t nparts = 0, total = 0;

  for (size_t i = 0; i < c->noverwrites; i++) {
    const Overwrite *o = &c->overwrites[i];
    const char *word = o->allow & PERM_VIEW_CHANNEL  ? "allow"
                       : o->deny & PERM_VIEW_CHANNEL ? "deny"
                                                     : nullptr;

    if (word == nullptr) {
      continue;
    }

    char part[40];
    snprintf(part, sizeof part, "%s:%" PRIu64, word, o->id);
    parts[nparts++] = str_dup(part);
    total += strlen(part) + 1;
  }

  qsort(parts, nparts, sizeof *parts, compare_text);

  char *joined = xcalloc(total + 1, 1);

  for (size_t i = 0; i < nparts; i++) {
    if (i != 0) {
      strcat(joined, ",");
    }

    strcat(joined, parts[i]);
    free(parts[i]);
  }

  snprintf(out, cap, "%u", murmur3(joined, strlen(joined)));
  free(joined);
  free(parts);
}

static MemberList *find_list(const Guild *g, const char *id) {
  for (size_t i = 0; i < g->nlists; i++) {
    if (strcmp(g->lists[i]->id, id) == 0) {
      return g->lists[i];
    }
  }

  return nullptr;
}

const MemberList *store_member_list(Store *s, const Channel *c) {
  const Guild *g = c != nullptr ? store_guild(s, c->guild_id) : nullptr;

  if (g == nullptr || g->nlists == 0) {
    return nullptr;
  }

  char id[32];
  store_member_list_id(s, c, id, sizeof id);

  MemberList *list = find_list(g, id);

  if (list != nullptr) {
    return list;
  }

  // should the id be computed wrong, the list discord synced last for the
  // guild is the one the session asked for.
  list = g->lists[0];

  for (size_t i = 1; i < g->nlists; i++) {
    if (g->lists[i]->touched > list->touched) {
      list = g->lists[i];
    }
  }

  return list;
}

static void list_resize(MemberList *list, size_t count) {
  reserve(&list->rows, &list->rows_cap, count, sizeof *list->rows);

  for (size_t i = list->nrows; i < count; i++) {
    list->rows[i] = (MemberRow){};
  }

  list->nrows = count;
}

static uint32_t group_count(const cima_json *groups, const char *id) {
  CIMA_EACH(group, json_items(groups)) {
    const char *other = json_get_or(group, "id", nullptr);

    if (id != nullptr && other != nullptr && strcmp(id, other) == 0) {
      return (uint32_t)json_get_coerce(group, "count", (uint64_t)0);
    }
  }

  return 0;
}

static MemberRow parse_row(Store *s, Guild *g, const cima_json *item,
                           const cima_json *groups) {
  const cima_json *group = json_get(item, "group");
  const cima_json *member = json_get(item, "member");

  if (json_type(group) == CIMA_JSON_OBJECT) {
    const char *id = json_get_or(group, "id", nullptr);
    MemberRow row = {.kind = MEMBER_ROW_GROUP};

    if (id != nullptr && strcmp(id, "online") == 0) {
      row.group = MEMBER_GROUP_ONLINE;
    }
    else if (id != nullptr && strcmp(id, "offline") == 0) {
      row.group = MEMBER_GROUP_OFFLINE;
    }
    else {
      row.group = MEMBER_GROUP_ROLE;
      row.id = snowflake_parse(id);
    }

    row.count = json_get(group, "count") != nullptr
                  ? (uint32_t)json_get_coerce(group, "count", (uint64_t)0)
                  : group_count(groups, id);
    return row;
  }

  if (json_type(member) == CIMA_JSON_OBJECT) {
    Member *m = store_put_member(s, g, member, 0);

    if (m != nullptr) {
      const cima_json *presence = json_get(member, "presence");

      if (json_type(presence) == CIMA_JSON_OBJECT) {
        put_presence(s, presence, m->user_id);
      }

      return (MemberRow){.kind = MEMBER_ROW_MEMBER, .id = m->user_id};
    }
  }

  return (MemberRow){};
}

void store_member_list_update(Store *s, const cima_json *update) {
  Guild *g = store_guild(s, snowflake_of(json_get(update, "guild_id")));
  const char *id = json_get_or(update, "id", nullptr);

  if (g == nullptr || id == nullptr) {
    return;
  }

  MemberList *list = find_list(g, id);

  if (list == nullptr) {
    list = xcalloc(1, sizeof *list);
    list->id = str_dup(id);
    list->guild_id = g->id;
    g->lists = resized(g->lists, g->nlists + 1, sizeof *g->lists);
    g->lists[g->nlists++] = list;
  }

  const cima_json *groups = json_get(update, "groups");
  const cima_json *v;

  if ((v = json_get(update, "member_count")) != nullptr) {
    list->member_count = (uint32_t)snowflake_of(v);
  }

  if ((v = json_get(update, "online_count")) != nullptr) {
    list->online_count = (uint32_t)snowflake_of(v);
  }

  CIMA_EACH(op, json_items(json_get(update, "ops"))) {
    const char *name = json_get_or(op, "op", nullptr);
    const cima_json *range = json_get(op, "range");
    size_t first = (size_t)snowflake_of(json_index(range, 0));
    size_t last = (size_t)snowflake_of(json_index(range, 1));
    size_t index = (size_t)json_get_coerce(op, "index", (uint64_t)0);

    // a list is at most as long as a guild has members, anything past that
    // is not an index.
    constexpr size_t limit = 1u << 24;

    if (name == nullptr || first > limit || last > limit || index > limit) {
      continue;
    }

    if (strcmp(name, "SYNC") == 0) {
      const cima_json *items = json_get(op, "items");
      size_t at = first;

      if (list->nrows < first + json_len(items)) {
        list_resize(list, first + json_len(items));
      }

      CIMA_EACH(item, json_items(items)) {
        list->rows[at++] = parse_row(s, g, item, groups);
      }
    }
    else if (strcmp(name, "INVALIDATE") == 0) {
      for (size_t i = first; i <= last && i < list->nrows; i++) {
        list->rows[i] = (MemberRow){};
      }
    }
    else if (strcmp(name, "INSERT") == 0) {
      MemberRow row = parse_row(s, g, json_get(op, "item"), groups);

      if (index > list->nrows) {
        list_resize(list, index);
      }

      list_resize(list, list->nrows + 1);
      memmove(&list->rows[index + 1], &list->rows[index],
              (list->nrows - index - 1) * sizeof *list->rows);
      list->rows[index] = row;
    }
    else if (strcmp(name, "UPDATE") == 0) {
      MemberRow row = parse_row(s, g, json_get(op, "item"), groups);

      if (index >= list->nrows) {
        list_resize(list, index + 1);
      }

      list->rows[index] = row;
    }
    else if (strcmp(name, "DELETE") == 0 && index < list->nrows) {
      memmove(&list->rows[index], &list->rows[index + 1],
              (list->nrows - index - 1) * sizeof *list->rows);
      list->nrows--;
    }
  }

  // the groups tell how long the whole list is, and how many each one holds
  // now.
  if (json_type(groups) == CIMA_JSON_ARRAY) {
    size_t total = 0;

    CIMA_EACH(group, json_items(groups)) {
      uint64_t count = json_get_coerce(group, "count", (uint64_t)0);
      total += count != 0 ? (size_t)count + 1 : 0;
    }

    if (total > list->nrows && total < (1u << 24)) {
      list_resize(list, total);
    }

    while (list->nrows > total &&
           list->rows[list->nrows - 1].kind == MEMBER_ROW_UNLOADED) {
      list->nrows--;
    }

    for (size_t i = 0; i < list->nrows; i++) {
      MemberRow *row = &list->rows[i];

      if (row->kind != MEMBER_ROW_GROUP) {
        continue;
      }

      char role[21];
      const char *group_id = row->group == MEMBER_GROUP_ONLINE ? "online"
                             : row->group == MEMBER_GROUP_OFFLINE
                               ? "offline"
                               : snowflake_format(row->id, role);
      row->count = group_count(groups, group_id);
    }
  }

  list->version++;
  list->touched = ++s->list_clock;
  store_touch_guild(s, g);
}

/* ========================================================================== */
/* the store itself.                                                          */
/* ========================================================================== */

Store *store_new(void) {
  return xcalloc(1, sizeof(Store));
}

void store_reset(Store *s) {
  CILO_MAP_EACH(entry, &s->guilds) {
    free_guild(s, entry->val);
  }

  // what is left are the dms, the guilds having freed their own channels.
  CILO_MAP_EACH(entry, &s->channels) {
    free_channel(entry->val);
  }

  CILO_MAP_EACH(entry, &s->users) {
    free_user(entry->val);
  }

  for (size_t i = 0; i < s->nrelationships; i++) {
    free(s->relationships[i].nickname);
  }

  free_folders(s);
  free_stored_folders(s);
  free_bookmarks(s);

  MAP_DROP(&s->guilds);
  MAP_DROP(&s->channels);
  MAP_DROP(&s->users);
  MAP_DROP(&s->read_states);

  free(s->relationships);
  free(s->voice_states);
  free(s->affinities);
  free(s->guild_order);
  free(s->privates);
  free(s->session_id);
  free(s->resume_gateway_url);
  free(s->refreshed_token);
  free(s->status_proto);
  free(s->bookmark_ids);

  // the counters go on, so nothing memoized on them is mistaken for fresh.
  Store fresh = {
    .version = s->version + 1,
    .users_version = s->users_version + 1,
    .guilds_version = s->guilds_version + 1,
    .private_version = s->private_version + 1,
    .bookmarks_version = s->bookmarks_version + 1,
    .list_clock = s->list_clock,
    .clock = s->clock,
  };

  *s = fresh;
}

void store_free(Store *s) {
  if (s == nullptr) {
    return;
  }

  store_reset(s);
  free(s);
}
