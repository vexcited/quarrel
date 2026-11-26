#include "discord/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord/settings_proto.h"

// a versioned field is an object holding its entries, a plain one the
// entries themselves.
static const cima_json *entries_of(const cima_json *field) {
  if (json_type(field) == CIMA_JSON_OBJECT) {
    return json_get(field, "entries");
  }

  return field;
}

static void put_presences(Store *s, const cima_json *presences) {
  CIMA_EACH(presence, json_items(presences)) {
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
  const cima_json *settings = json_parse(&arena, text, strlen(text), nullptr);

  if (settings != nullptr) {
    store_put_user_settings(s, settings);
  }

  cilo_arena_drop(&arena);
  cilo_mfree(text);
}

// merged members and presences line up with the guilds of the same payload,
// one array per guild.
static void put_merged(Store *s, const cima_json *guilds,
                       const cima_json *members, const cima_json *presences) {
  size_t i = 0;

  CIMA_EACH(guild, json_items(guilds)) {
    Guild *g = store_guild(s, snowflake_of(json_get(guild, "id")));

    if (g != nullptr) {
      CIMA_EACH(member, json_items(json_index(members, i))) {
        store_put_member(s, g, member, 0);
      }
    }

    put_presences(s, json_index(presences, i));
    i++;
  }
}

static void on_ready(Store *s, const cima_json *d) {
  // a new session starts from nothing: what was missed is not replayed.
  store_reset(s);

  // with deduplicated users every object below only names its users.
  CIMA_EACH(user, json_items(json_get(d, "users"))) {
    store_put_user(s, user);
  }

  User *self = store_put_user(s, json_get(d, "user"));

  if (self != nullptr) {
    s->self = self;
    s->self_id = self->id;
    s->self_status = STATUS_ONLINE;
  }

  if (json_type(json_get(d, "user_settings")) == CIMA_JSON_OBJECT) {
    store_put_user_settings(s, json_get(d, "user_settings"));
  }

  put_settings_proto(s, json_get_or(d, "user_settings_proto", nullptr));
  s->notify_flags = (uint32_t)snowflake_of(
    json_get(json_get(d, "notification_settings"), "flags"));

  const cima_json *guilds = json_get(d, "guilds");
  const cima_json *merged = json_get(d, "merged_presences");

  CIMA_EACH(guild, json_items(guilds)) {
    store_put_guild(s, guild);
  }

  put_merged(s, guilds, json_get(d, "merged_members"),
             json_get(merged, "guilds"));
  put_presences(s, json_get(merged, "friends"));
  put_presences(s, json_get(d, "presences"));

  CIMA_EACH(channel, json_items(json_get(d, "private_channels"))) {
    store_put_channel(s, channel, 0);
  }

  CIMA_EACH(relationship, json_items(json_get(d, "relationships"))) {
    store_put_relationship(s, relationship);
  }

  CIMA_EACH(entry, json_items(entries_of(json_get(d, "read_state")))) {
    store_put_read_state(s, entry);
  }

  CIMA_EACH(entry, json_items(entries_of(json_get(d, "user_guild_settings")))) {
    store_put_guild_settings(s, entry);
  }

  str_set(&s->session_id, json_get_or(d, "session_id", nullptr));
  str_set(&s->resume_gateway_url,
          json_get_or(d, "resume_gateway_url", nullptr));
  str_set(&s->refreshed_token, json_get_or(d, "auth_token", nullptr));

  s->ready = true;
  s->guilds_version++;
  s->private_version++;
  s->version++;
}

static void on_ready_supplemental(Store *s, const cima_json *d) {
  const cima_json *merged = json_get(d, "merged_presences");

  put_merged(s, json_get(d, "guilds"), json_get(d, "merged_members"),
             json_get(merged, "guilds"));
  put_presences(s, json_get(merged, "friends"));

  CIMA_EACH(guild, json_items(json_get(d, "guilds"))) {
    Snowflake id = snowflake_of(json_get(guild, "id"));

    CIMA_EACH(state, json_items(json_get(guild, "voice_states"))) {
      store_put_voice_state(s, state, id);
    }
  }

  CIMA_EACH(channel, json_items(json_get(d, "lazy_private_channels"))) {
    store_put_channel(s, channel, 0);
  }

  s->version++;
}

static void on_guild_delete(Store *s, const cima_json *d) {
  Snowflake id = snowflake_of(json_get(d, "id"));
  Guild *g = store_guild(s, id);

  if (g == nullptr) {
    return;
  }

  // an outage keeps the guild, leaving or being removed drops it.
  if (json_get_coerce(d, "unavailable", false)) {
    g->unavailable = true;
    s->guilds_version++;
    store_touch_guild(s, g);
    return;
  }

  store_remove_guild(s, id);
}

static void on_unreads(Store *s, const cima_json *channels) {
  CIMA_EACH(channel, json_items(channels)) {
    store_set_last_message(s, snowflake_of(json_get(channel, "id")),
                           snowflake_of(json_get(channel, "last_message_id")));
  }
}

static void on_reaction_add_many(Store *s, const cima_json *d) {
  Snowflake channel_id = snowflake_of(json_get(d, "channel_id"));
  Snowflake message_id = snowflake_of(json_get(d, "message_id"));

  CIMA_EACH(reaction, json_items(json_get(d, "reactions"))) {
    CIMA_EACH(user, json_items(json_get(reaction, "users"))) {
      store_reaction_add(s, channel_id, message_id, snowflake_of(user),
                         json_get(reaction, "emoji"));
    }
  }
}

static void on_thread_members(Store *s, const cima_json *d) {
  Snowflake id = snowflake_of(json_get(d, "id"));

  CIMA_EACH(member, json_items(json_get(d, "added_members"))) {
    if (snowflake_of(json_get(member, "user_id")) == s->self_id) {
      store_set_thread_joined(s, id, true);
    }
  }

  CIMA_EACH(removed, json_items(json_get(d, "removed_member_ids"))) {
    if (snowflake_of(removed) == s->self_id) {
      store_set_thread_joined(s, id, false);
    }
  }
}

void events_dispatch(Store *s, const char *type, const cima_json *d) {
  if (s == nullptr || type == nullptr) {
    return;
  }

#define IS(name) (strcmp(type, name) == 0)

  Snowflake guild_id = snowflake_of(json_get(d, "guild_id"));
  Snowflake channel_id = snowflake_of(json_get(d, "channel_id"));
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
    store_remove_message(s, channel_id, snowflake_of(json_get(d, "id")));
  }
  else if (IS("MESSAGE_DELETE_BULK")) {
    CIMA_EACH(id, json_items(json_get(d, "ids"))) {
      store_remove_message(s, channel_id, snowflake_of(id));
    }
  }
  else if (IS("MESSAGE_REACTION_ADD")) {
    if (g != nullptr &&
        (json_type(json_get(d, "member")) == CIMA_JSON_OBJECT)) {
      store_put_member(s, g, json_get(d, "member"),
                       snowflake_of(json_get(d, "user_id")));
    }

    store_reaction_add(s, channel_id, snowflake_of(json_get(d, "message_id")),
                       snowflake_of(json_get(d, "user_id")),
                       json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_REACTION_ADD_MANY")) {
    on_reaction_add_many(s, d);
  }
  else if (IS("MESSAGE_REACTION_REMOVE")) {
    store_reaction_remove(
      s, channel_id, snowflake_of(json_get(d, "message_id")),
      snowflake_of(json_get(d, "user_id")), json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_REACTION_REMOVE_ALL")) {
    store_reaction_clear(s, channel_id, snowflake_of(json_get(d, "message_id")),
                         nullptr);
  }
  else if (IS("MESSAGE_REACTION_REMOVE_EMOJI")) {
    store_reaction_clear(s, channel_id, snowflake_of(json_get(d, "message_id")),
                         json_get(d, "emoji"));
  }
  else if (IS("MESSAGE_ACK")) {
    store_put_ack(s, d);
  }
  else if (IS("SAVED_MESSAGE_CREATE")) {
    store_put_saved_message(s, d, false);
  }
  else if (IS("SAVED_MESSAGE_DELETE")) {
    store_remove_saved_message(s, snowflake_of(json_get(d, "message_id")));
  }
  else if (IS("CHANNEL_CREATE") || IS("CHANNEL_UPDATE") ||
           IS("THREAD_CREATE") || IS("THREAD_UPDATE")) {
    store_put_channel(s, d, 0);
  }
  else if (IS("CHANNEL_DELETE") || IS("THREAD_DELETE")) {
    store_remove_channel(s, snowflake_of(json_get(d, "id")));
  }
  else if (IS("FORUM_UNREADS")) {
    store_put_forum_unreads(s, d);
  }
  else if (IS("THREAD_LIST_SYNC")) {
    store_sync_threads(s, d);
  }
  else if (IS("THREAD_MEMBER_UPDATE")) {
    store_set_thread_joined(s, snowflake_of(json_get(d, "id")), true);
  }
  else if (IS("THREAD_MEMBERS_UPDATE")) {
    on_thread_members(s, d);
  }
  else if (IS("CHANNEL_RECIPIENT_ADD")) {
    store_recipient_add(s, channel_id, json_get(d, "user"));
  }
  else if (IS("CHANNEL_RECIPIENT_REMOVE")) {
    store_recipient_remove(s, channel_id,
                           snowflake_of(json_get(json_get(d, "user"), "id")));
  }
  else if (IS("CHANNEL_UNREAD_UPDATE")) {
    on_unreads(s, json_get(d, "channel_unread_updates"));
  }
  else if (IS("PASSIVE_UPDATE_V1")) {
    on_unreads(s, json_get(d, "channels"));
  }
  else if (IS("PASSIVE_UPDATE_V2")) {
    on_unreads(s, json_get(d, "updated_channels"));

    CIMA_EACH(state, json_items(json_get(d, "updated_voice_states"))) {
      store_put_voice_state(s, state, guild_id);
    }

    CIMA_EACH(user_id, json_items(json_get(d, "removed_voice_states"))) {
      store_remove_voice_state(s, snowflake_of(user_id), guild_id);
    }
  }
  else if (IS("VOICE_STATE_UPDATE")) {
    store_put_voice_state(s, d, guild_id);
  }
  else if (IS("CALL_CREATE") || IS("CALL_UPDATE")) {
    store_put_call(s, d);
  }
  else if (IS("CALL_DELETE")) {
    store_remove_call(s, channel_id);
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
      store_remove_role(s, g, snowflake_of(json_get(d, "role_id")));
    }
  }
  else if (IS("GUILD_MEMBER_ADD") || IS("GUILD_MEMBER_UPDATE")) {
    store_put_member(s, g, d, 0);
  }
  else if (IS("GUILD_MEMBER_REMOVE")) {
    if (g != nullptr) {
      store_remove_member(s, g,
                          snowflake_of(json_get(json_get(d, "user"), "id")));
    }
  }
  else if (IS("GUILD_MEMBERS_CHUNK")) {
    CIMA_EACH(member, json_items(json_get(d, "members"))) {
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
    if (g != nullptr &&
        (json_type(json_get(d, "member")) == CIMA_JSON_OBJECT)) {
      store_put_member(s, g, json_get(d, "member"),
                       snowflake_of(json_get(d, "user_id")));
    }

    store_typing_start(s, channel_id, snowflake_of(json_get(d, "user_id")));
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
    const cima_json *settings = json_get(d, "settings");

    // type 1 is the preloaded settings, the others hold nothing shown.
    if (json_get_coerce(settings, "type", (int64_t)0) == 1) {
      put_settings_proto(s, json_get_or(settings, "proto", nullptr));
    }
  }
  else if (IS("GUILD_DIRECTORY_ENTRY_CREATE") ||
           IS("GUILD_DIRECTORY_ENTRY_UPDATE")) {
    store_put_directory_entry(s, d);
  }
  else if (IS("GUILD_DIRECTORY_ENTRY_DELETE")) {
    store_remove_directory_entry(
      s, snowflake_of(json_get(d, "directory_channel_id")),
      snowflake_of(json_get(d, "entity_id")));
  }
  else if (IS("RELATIONSHIP_ADD") || IS("RELATIONSHIP_UPDATE")) {
    store_put_relationship(s, d);
  }
  else if (IS("RELATIONSHIP_REMOVE")) {
    store_remove_relationship(s, snowflake_of(json_get(d, "id")));
  }

#undef IS
}

static void feed_payload(Store *s, const cima_json *payload) {
  const char *type = json_get_or(payload, "t", nullptr);

  if (type != nullptr) {
    events_dispatch(s, type, json_get(payload, "d"));
  }
}

bool events_feed(Store *s, const char *text, size_t len) {
  cilo_arena arena = {};
  const cima_json *doc = json_parse(&arena, text, len, nullptr);
  bool ok =
    (json_type(doc) == CIMA_JSON_ARRAY) || (json_type(doc) == CIMA_JSON_OBJECT);

  if (json_type(doc) == CIMA_JSON_ARRAY) {
    CIMA_EACH(payload, json_items(doc)) {
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
