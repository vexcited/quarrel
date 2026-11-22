#include "discord/store.h"

#include <stdlib.h>
#include <string.h>

#define MAP_PUT(map, key, ...) \
  CILO_MAP_PUT(quarrel_libc, map, key __VA_OPT__(, ) __VA_ARGS__)
#define MAP_DROP(map) CILO_MAP_DROP(quarrel_libc, map)

static void put_text(char **dst, const cima_json *obj, const char *key) {
  const cima_json *v = json_get(obj, key);

  if (v != nullptr) {
    str_set(dst, json_or(v, nullptr));
  }
}

static void put_count(uint32_t *dst, const cima_json *obj, const char *key) {
  const cima_json *v = json_get(obj, key);

  if (v != nullptr) {
    *dst = (uint32_t)json_coerce(v, (int64_t)0);
  }
}

static void free_entry(DirectoryEntry *e) {
  free(e->description);
  free(e->name);
  free(e->icon);
  free(e->splash);
  free(e->banner);
  free(e->event_name);
  free(e->event_description);
  free(e->event_image);
  free(e);
}

static void touch(Store *s, Directory *d) {
  d->version++;
  s->version++;
}

Directory *store_directory(const Store *s, Snowflake channel_id) {
  Directory **found = CILO_MAP_GET(&s->directories, channel_id);
  return found != nullptr ? *found : nullptr;
}

DirectoryEntry *store_directory_entry(const Directory *d, Snowflake entity_id) {
  for (size_t i = 0; d != nullptr && i < d->nentries; i++) {
    if (d->entries[i]->entity_id == entity_id) {
      return d->entries[i];
    }
  }

  return nullptr;
}

Channel *store_guild_directory(Store *s, const Guild *g) {
  Channel *first = nullptr;

  for (size_t i = 0; g != nullptr && i < g->nchannels; i++) {
    Channel *c = g->channels[i];

    if (c->type != CHANNEL_GUILD_DIRECTORY ||
        !store_can(s, c, PERM_VIEW_CHANNEL)) {
      continue;
    }

    if (first == nullptr || c->position < first->position ||
        (c->position == first->position && c->id < first->id)) {
      first = c;
    }
  }

  return first;
}

Directory *store_open_directory(Store *s, Snowflake channel_id) {
  Directory *d = store_directory(s, channel_id);

  if (d != nullptr || channel_id == 0) {
    return d;
  }

  d = calloc(1, sizeof *d);

  if (d == nullptr) {
    cilo_oom(sizeof *d);
  }

  d->channel_id = channel_id;
  *MAP_PUT(&s->directories, channel_id) = d;
  return d;
}

static void count(Directory *d, DirectoryCategory category, int by) {
  if (!d->counted || category >= DIRECTORY_CATEGORIES) {
    return;
  }

  if (by < 0 && d->counts[category] == 0) {
    return;
  }

  d->counts[category] += (uint32_t)by;
}

static void put_guild(DirectoryEntry *e, const cima_json *guild) {
  if (json_type(guild) != CIMA_JSON_OBJECT) {
    return;
  }

  Snowflake id = snowflake_of(json_get(guild, "id"));

  if (id != 0) {
    e->guild_id = id;
  }

  put_text(&e->name, guild, "name");
  put_text(&e->icon, guild, "icon");
  put_text(&e->splash, guild, "splash");
  put_text(&e->banner, guild, "banner");
  put_count(&e->member_count, guild, "approximate_member_count");
  put_count(&e->presence_count, guild, "approximate_presence_count");

  const cima_json *v = json_get(guild, "featurable_in_directory");

  if (v != nullptr) {
    e->featurable = json_coerce(v, false);
  }
}

static void put_event(DirectoryEntry *e, const cima_json *event) {
  if (json_type(event) != CIMA_JSON_OBJECT) {
    return;
  }

  put_text(&e->event_name, event, "name");
  put_text(&e->event_description, event, "description");
  put_text(&e->event_image, event, "image");
  put_count(&e->user_count, event, "user_count");

  const cima_json *v = json_get(event, "scheduled_start_time");

  if (v != nullptr) {
    e->starts_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  if ((v = json_get(event, "scheduled_end_time")) != nullptr) {
    e->ends_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  if ((v = json_get(event, "user_rsvp")) != nullptr) {
    e->rsvp = json_coerce(v, false);
  }

  if ((v = json_get(event, "guild_id")) != nullptr && snowflake_of(v) != 0) {
    e->guild_id = snowflake_of(v);
  }

  put_guild(e, json_get(event, "guild"));
}

// an entry made new or found again, at the head of its directory.
static DirectoryEntry *entry_in(Directory *d, Snowflake entity_id, bool *made) {
  DirectoryEntry *e = store_directory_entry(d, entity_id);

  *made = e == nullptr;

  if (e != nullptr) {
    return e;
  }

  e = calloc(1, sizeof *e);

  if (e == nullptr) {
    cilo_oom(sizeof *e);
  }

  e->entity_id = entity_id;

  if (d->nentries == d->entries_cap) {
    size_t cap = d->entries_cap ? d->entries_cap * 2 : 16;
    DirectoryEntry **moved = realloc(d->entries, cap * sizeof *moved);

    if (moved == nullptr) {
      cilo_oom(cap * sizeof *moved);
    }

    d->entries = moved;
    d->entries_cap = cap;
  }

  memmove(d->entries + 1, d->entries, d->nentries * sizeof *d->entries);
  d->entries[0] = e;
  d->nentries++;
  return e;
}

static DirectoryEntry *put_entry(Store *s, Directory *d,
                                 const cima_json *entry) {
  Snowflake entity_id = snowflake_of(json_get(entry, "entity_id"));

  if (entity_id == 0) {
    return nullptr;
  }

  bool made = false;
  DirectoryEntry *e = entry_in(d, entity_id, &made);
  DirectoryCategory was = e->category;
  const cima_json *v;

  if ((v = json_get(entry, "type")) != nullptr) {
    e->type = (DirectoryEntryType)json_coerce(v, (int64_t)0);
  }

  if ((v = json_get(entry, "primary_category_id")) != nullptr) {
    int64_t category = json_coerce(v, (int64_t)0);

    e->category = category >= 0 && category < (int64_t)DIRECTORY_CATEGORIES
                    ? (DirectoryCategory)category
                    : DIRECTORY_UNCATEGORIZED;
  }

  if ((v = json_get(entry, "author_id")) != nullptr) {
    e->author_id = snowflake_of(v);
  }

  if ((v = json_get(entry, "created_at")) != nullptr) {
    e->created_at = iso8601_parse_ms(json_or(v, nullptr));
  }

  put_text(&e->description, entry, "description");

  if (e->type == DIRECTORY_ENTRY_GUILD) {
    e->guild_id = entity_id;
    put_guild(e, json_get(entry, "guild"));
  }
  else {
    put_event(e, json_get(entry, "guild_scheduled_event"));
  }

  if (made) {
    count(d, e->category, 1);
  }
  else if (was != e->category) {
    count(d, was, -1);
    count(d, e->category, 1);
  }

  touch(s, d);
  return e;
}

DirectoryEntry *store_put_directory_entry(Store *s, const cima_json *entry) {
  Snowflake channel_id = snowflake_of(json_get(entry, "directory_channel_id"));

  if (channel_id == 0) {
    return nullptr;
  }

  return put_entry(s, store_open_directory(s, channel_id), entry);
}

static int by_newest(const void *a, const void *b) {
  const DirectoryEntry *x = *(DirectoryEntry *const *)a;
  const DirectoryEntry *y = *(DirectoryEntry *const *)b;

  if (x->created_at != y->created_at) {
    return x->created_at > y->created_at ? -1 : 1;
  }

  return x->entity_id > y->entity_id ? -1 : x->entity_id < y->entity_id;
}

void store_put_directory(Store *s, Snowflake channel_id,
                         const cima_json *entries) {
  Directory *d = store_open_directory(s, channel_id);

  if (d == nullptr || json_type(entries) != CIMA_JSON_ARRAY) {
    return;
  }

  for (size_t i = 0; i < d->nentries; i++) {
    free_entry(d->entries[i]);
  }

  d->nentries = 0;

  // the counts route says how many there are, a whole listing as much.
  bool counted = d->counted;

  d->counted = false;

  CIMA_EACH(entry, json_items(entries)) {
    put_entry(s, d, entry);
  }

  d->counted = counted;
  qsort(d->entries, d->nentries, sizeof *d->entries, by_newest);
  d->loaded = true;
  touch(s, d);
}

void store_put_directory_counts(Store *s, Snowflake channel_id,
                                const cima_json *counts) {
  Directory *d = store_open_directory(s, channel_id);

  if (d == nullptr || json_type(counts) != CIMA_JSON_OBJECT) {
    return;
  }

  memset(d->counts, 0, sizeof d->counts);

  CIMA_EACH(member, json_members(counts)) {
    Snowflake category =
      snowflake_parse_n(member->key, cima_json_key_len(member));

    if (category < DIRECTORY_CATEGORIES) {
      d->counts[category] = (uint32_t)json_coerce(&member->value, (int64_t)0);
    }
  }

  d->counted = true;
  touch(s, d);
}

void store_remove_directory_entry(Store *s, Snowflake channel_id,
                                  Snowflake entity_id) {
  Directory *d = store_directory(s, channel_id);

  for (size_t i = 0; d != nullptr && i < d->nentries; i++) {
    DirectoryEntry *e = d->entries[i];

    if (e->entity_id != entity_id) {
      continue;
    }

    count(d, e->category, -1);
    free_entry(e);
    memmove(d->entries + i, d->entries + i + 1,
            (d->nentries - i - 1) * sizeof *d->entries);
    d->nentries--;
    touch(s, d);
    return;
  }
}

void store_drop_directories(Store *s) {
  CILO_MAP_EACH(slot, &s->directories) {
    Directory *d = slot->val;

    for (size_t i = 0; i < d->nentries; i++) {
      free_entry(d->entries[i]);
    }

    free(d->entries);
    free(d);
  }

  MAP_DROP(&s->directories);
}
