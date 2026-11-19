#include "discord/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord/settings_proto.h"

// a versioned field is an object holding its entries, a plain one the
// entries themselves.
static const Json *entries_of(const Json *field) {
  if (json_is(field, JSON_OBJECT)) {
    return json_get(field, "entries");
  }

  return field;
}

static void put_presences(Store *s, const Json *presences) {
  json_each(presence, presences) {
    store_put_presence(s, presence);
  }
}

// the settings as the USER_SETTINGS_PROTO capability sends them.
static void put_settings_proto(Store *s, const char *base64) {
  char *text = settings_proto_to_json(base64);

  if (text == nullptr) {
    return;
  }

  cilo_arena arena = {};
  const Json *settings = json_parse(&arena, text, strlen(text), nullptr);

  if (settings != nullptr) {
    store_put_user_settings(s, settings);
  }

  cilo_arena_drop(&arena);
  free(text);
}

// merged members and presences line up with the guilds of the same payload,
// one array per guild.
static void put_merged(Store *s, const Json *guilds, const Json *members,
                       const Json *presences) {
  const Json *guild_members = json_first(members);
  const Json *guild_presences = json_first(presences);

  json_each(guild, guilds) {
    Guild *g = store_guild(s, json_get_u64(guild, "id"));

    if (g != nullptr) {
      json_each(member, guild_members) {
        store_put_member(s, g, member, 0);
      }
    }

    put_presences(s, guild_presences);

    guild_members = guild_members != nullptr ? guild_members->next : nullptr;
    guild_presences =
      guild_presences != nullptr ? guild_presences->next : nullptr;
  }
}

static void on_ready(Store *s, const Json *d) {
  // a new session starts from nothing: what was missed is not replayed.
  store_reset(s);

  // with deduplicated users every object below only names its users.
  json_each(user, json_get(d, "users")) {
    store_put_user(s, user);
  }

  User *self = store_put_user(s, json_get(d, "user"));

  if (self != nullptr) {
    s->self = self;
    s->self_id = self->id;
    s->self_status = STATUS_ONLINE;
  }

  if (json_is(json_get(d, "user_settings"), JSON_OBJECT)) {
    store_put_user_settings(s, json_get(d, "user_settings"));
  }

  put_settings_proto(s, json_get_str(d, "user_settings_proto"));
  s->notify_flags =
    (uint32_t)json_get_u64(json_get(d, "notification_settings"), "flags");

  const Json *guilds = json_get(d, "guilds");
  const Json *merged = json_get(d, "merged_presences");

  json_each(guild, guilds) {
    store_put_guild(s, guild);
  }

  put_merged(s, guilds, json_get(d, "merged_members"),
             json_get(merged, "guilds"));
  put_presences(s, json_get(merged, "friends"));
  put_presences(s, json_get(d, "presences"));

  json_each(channel, json_get(d, "private_channels")) {
    store_put_channel(s, channel, 0);
  }

  json_each(relationship, json_get(d, "relationships")) {
    store_put_relationship(s, relationship);
  }

  json_each(entry, entries_of(json_get(d, "read_state"))) {
    store_put_read_state(s, entry);
  }

  json_each(entry, entries_of(json_get(d, "user_guild_settings"))) {
    store_put_guild_settings(s, entry);
  }

  str_set(&s->session_id, json_get_str(d, "session_id"));
  str_set(&s->resume_gateway_url, json_get_str(d, "resume_gateway_url"));
  str_set(&s->refreshed_token, json_get_str(d, "auth_token"));

  s->ready = true;
  s->guilds_version++;
  s->private_version++;
  s->version++;
}

static void on_ready_supplemental(Store *s, const Json *d) {
  const Json *merged = json_get(d, "merged_presences");

  put_merged(s, json_get(d, "guilds"), json_get(d, "merged_members"),
             json_get(merged, "guilds"));
  put_presences(s, json_get(merged, "friends"));

  json_each(guild, json_get(d, "guilds")) {
    Snowflake id = json_get_u64(guild, "id");

    json_each(state, json_get(guild, "voice_states")) {
      store_put_voice_state(s, state, id);
    }
  }

  json_each(channel, json_get(d, "lazy_private_channels")) {
    store_put_channel(s, channel, 0);
  }

  s->version++;
}

static void on_guild_delete(Store *s, const Json *d) {
  Snowflake id = json_get_u64(d, "id");
  Guild *g = store_guild(s, id);

  if (g == nullptr) {
    return;
  }

  // an outage keeps the guild, leaving or being removed drops it.
  if (json_get_bool(d, "unavailable", false)) {
    g->unavailable = true;
    s->guilds_version++;
    store_touch_guild(s, g);
    return;
  }

  store_remove_guild(s, id);
}

static void on_unreads(Store *s, const Json *channels) {
  json_each(channel, channels) {
    store_set_last_message(s, json_get_u64(channel, "id"),
                           json_get_u64(channel, "last_message_id"));
  }
}

static void on_reaction_add_many(Store *s, const Json *d) {
  Snowflake channel_id = json_get_u64(d, "channel_id");
  Snowflake message_id = json_get_u64(d, "message_id");

  json_each(reaction, json_get(d, "reactions")) {
    json_each(user, json_get(reaction, "users")) {
      store_reaction_add(s, channel_id, message_id, json_u64(user),
                         json_get(reaction, "emoji"));
    }
  }
}

static void on_thread_members(Store *s, const Json *d) {
  Snowflake id = json_get_u64(d, "id");

  json_each(member, json_get(d, "added_members")) {
    if (json_get_u64(member, "user_id") == s->self_id) {
      store_set_thread_joined(s, id, true);
    }
  }

  json_each(removed, json_get(d, "removed_member_ids")) {
    if (json_u64(removed) == s->self_id) {
      store_set_thread_joined(s, id, false);
    }
  }
}

void events_dispatch(Store *s, const char *type, const Json *d) {
  if (s == nullptr || type == nullptr) {
    return;
  }

#define IS(name) (strcmp(type, name) == 0)

  Snowflake guild_id = json_get_u64(d, "guild_id");
  Snowflake channel_id = json_get_u64(d, "channel_id");
  Guild *g = store_guild(s, guild_id);

  if (IS("READY")) {
    on_ready(s, d);
  }
  else if (IS("READY_SUPPLEMENTAL")) {
    on_ready_supplemental(s, d);
  }
  else if (IS("RESUMED")) {
    s->version++;
  }
  else if (IS("MESSAGE_CREATE")) {
    store_create_message(s, d);
  }
  else if (IS("MESSAGE_UPDATE")) {
    store_update_message(s, d);
  }
  else if (IS("MESSAGE_DELETE")) {
    store_remove_message(s, channel_id, json_get_u64(d, "id"));
  }
  else if (IS("MESSAGE_DELETE_BULK")) {
    json_each(id, json_get(d, "ids")) {
      store_remove_message(s, channel_id, json_u64(id));
    }
  }
  else if (IS("MESSAGE_REACTION_ADD")) {
    if (g != nullptr && json_is(json_get(d, "member"), JSON_OBJECT)) {
      store_put_member(s, g, json_get(d, "member"), json_get_u64(d, "user_id"));
    }

    store_reaction_add(s, channel_id, json_get_u64(d, "message_id"),
                       json_get_u64(d, "user_id"), json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_REACTION_ADD_MANY")) {
    on_reaction_add_many(s, d);
  }
  else if (IS("MESSAGE_REACTION_REMOVE")) {
    store_reaction_remove(s, channel_id, json_get_u64(d, "message_id"),
                          json_get_u64(d, "user_id"), json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_REACTION_REMOVE_ALL")) {
    store_reaction_clear(s, channel_id, json_get_u64(d, "message_id"), nullptr);
  }
  else if (IS("MESSAGE_REACTION_REMOVE_EMOJI")) {
    store_reaction_clear(s, channel_id, json_get_u64(d, "message_id"),
                         json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_ACK")) {
    store_put_ack(s, d);
  }
  else if (IS("CHANNEL_CREATE") || IS("CHANNEL_UPDATE") ||
           IS("THREAD_CREATE") || IS("THREAD_UPDATE")) {
    store_put_channel(s, d, 0);
  }
  else if (IS("CHANNEL_DELETE") || IS("THREAD_DELETE")) {
    store_remove_channel(s, json_get_u64(d, "id"));
  }
  else if (IS("THREAD_LIST_SYNC")) {
    store_sync_threads(s, d);
  }
  else if (IS("THREAD_MEMBER_UPDATE")) {
    store_set_thread_joined(s, json_get_u64(d, "id"), true);
  }
  else if (IS("THREAD_MEMBERS_UPDATE")) {
    on_thread_members(s, d);
  }
  else if (IS("CHANNEL_RECIPIENT_ADD")) {
    store_recipient_add(s, channel_id, json_get(d, "user"));
  }
  else if (IS("CHANNEL_RECIPIENT_REMOVE")) {
    store_recipient_remove(s, channel_id,
                           json_get_u64(json_get(d, "user"), "id"));
  }
  else if (IS("CHANNEL_UNREAD_UPDATE")) {
    on_unreads(s, json_get(d, "channel_unread_updates"));
  }
  else if (IS("PASSIVE_UPDATE_V1")) {
    on_unreads(s, json_get(d, "channels"));
  }
  else if (IS("PASSIVE_UPDATE_V2")) {
    on_unreads(s, json_get(d, "updated_channels"));

    json_each(state, json_get(d, "updated_voice_states")) {
      store_put_voice_state(s, state, guild_id);
    }

    json_each(user_id, json_get(d, "removed_voice_states")) {
      store_remove_voice_state(s, json_u64(user_id), guild_id);
    }
  }
  else if (IS("VOICE_STATE_UPDATE")) {
    store_put_voice_state(s, d, guild_id);
  }
  else if (IS("GUILD_CREATE")) {
    store_put_guild(s, d);
  }
  else if (IS("GUILD_UPDATE")) {
    store_update_guild(s, d);
  }
  else if (IS("GUILD_DELETE")) {
    on_guild_delete(s, d);
  }
  else if (IS("GUILD_ROLE_CREATE") || IS("GUILD_ROLE_UPDATE")) {
    if (g != nullptr) {
      store_put_role(s, g, json_get(d, "role"));
    }
  }
  else if (IS("GUILD_ROLE_DELETE")) {
    if (g != nullptr) {
      store_remove_role(s, g, json_get_u64(d, "role_id"));
    }
  }
  else if (IS("GUILD_MEMBER_ADD") || IS("GUILD_MEMBER_UPDATE")) {
    store_put_member(s, g, d, 0);
  }
  else if (IS("GUILD_MEMBER_REMOVE")) {
    if (g != nullptr) {
      store_remove_member(s, g, json_get_u64(json_get(d, "user"), "id"));
    }
  }
  else if (IS("GUILD_MEMBERS_CHUNK")) {
    json_each(member, json_get(d, "members")) {
      store_put_member(s, g, member, 0);
    }

    put_presences(s, json_get(d, "presences"));
  }
  else if (IS("GUILD_MEMBER_LIST_UPDATE")) {
    store_member_list_update(s, d);
  }
  else if (IS("GUILD_EMOJIS_UPDATE")) {
    if (g != nullptr) {
      store_put_emojis(s, g, json_get(d, "emojis"));
    }
  }
  else if (IS("PRESENCE_UPDATE")) {
    store_put_presence(s, d);
  }
  else if (IS("TYPING_START")) {
    if (g != nullptr && json_is(json_get(d, "member"), JSON_OBJECT)) {
      store_put_member(s, g, json_get(d, "member"), json_get_u64(d, "user_id"));
    }

    store_typing_start(s, channel_id, json_get_u64(d, "user_id"));
  }
  else if (IS("USER_UPDATE")) {
    store_put_user(s, d);
  }
  else if (IS("USER_SETTINGS_UPDATE")) {
    store_put_user_settings(s, d);
  }
  else if (IS("USER_GUILD_SETTINGS_UPDATE")) {
    store_put_guild_settings(s, d);
  }
  else if (IS("USER_SETTINGS_PROTO_UPDATE")) {
    const Json *settings = json_get(d, "settings");

    // type 1 is the preloaded settings, the others hold nothing shown.
    if (json_get_i64(settings, "type", 0) == 1) {
      put_settings_proto(s, json_get_str(settings, "proto"));
    }
  }
  else if (IS("RELATIONSHIP_ADD") || IS("RELATIONSHIP_UPDATE")) {
    store_put_relationship(s, d);
  }
  else if (IS("RELATIONSHIP_REMOVE")) {
    store_remove_relationship(s, json_get_u64(d, "id"));
  }

#undef IS
}

static void feed_payload(Store *s, const Json *payload) {
  const char *type = json_get_str(payload, "t");

  if (type != nullptr) {
    events_dispatch(s, type, json_get(payload, "d"));
  }
}

bool events_feed(Store *s, const char *text, size_t len) {
  cilo_arena arena = {};
  const Json *doc = json_parse(&arena, text, len, nullptr);
  bool ok = json_is(doc, JSON_ARRAY) || json_is(doc, JSON_OBJECT);

  if (json_is(doc, JSON_ARRAY)) {
    json_each(payload, doc) {
      feed_payload(s, payload);
    }
  }
  else if (ok) {
    feed_payload(s, doc);
  }

  cilo_arena_drop(&arena);
  return ok;
}

bool events_feed_file(Store *s, const char *path) {
  FILE *file = path != nullptr ? fopen(path, "rb") : nullptr;

  if (file == nullptr) {
    return false;
  }

  char *text = nullptr;
  size_t len = 0, cap = 0;

  for (;;) {
    if (len + 65536 > cap) {
      cap = cap != 0 ? cap * 2 : 1 << 18;

      char *grown = realloc(text, cap);

      if (grown == nullptr) {
        cilo_oom(cap);
      }

      text = grown;
    }

    size_t got = fread(text + len, 1, cap - len, file);

    if (got == 0) {
      break;
    }

    len += got;
  }

  fclose(file);

  bool ok = events_feed(s, text, len);
  free(text);
  return ok;
}
