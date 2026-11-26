#include "discord/settings_proto.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "discord/util.h"
#include <cima/json_dsl.h>

// the field numbers, from the protobuf-ts classes of the client's bundle:
//   PreloadedUserSettings  text_and_images 6, status 11, guild_folders 14
//   TextAndImagesSettings  animate_stickers 15 a UInt32Value
//   StatusSettings         status 1 a StringValue, custom_status 2,
//                          status_expires_at_ms 4 fixed64,
//                          status_created_at_ms 5 a UInt64Value
//   CustomStatus           text 1, emoji_id 2 fixed64, emoji_name 3,
//                          expires_at_ms 4 fixed64
//   GuildFolders           folders 1 repeated, guild_positions 2 fixed64
//   GuildFolder            guild_ids 1 fixed64, id 2 an Int64Value,
//                          name 3 a StringValue, color 4 a UInt64Value
// the wrapper messages hold their value in field 1.
enum {
  WIRE_VARINT = 0,
  WIRE_FIXED64 = 1,
  WIRE_BYTES = 2,
  WIRE_FIXED32 = 5,
};

typedef struct {
  const uint8_t *at, *end;
  bool bad;
} Reader;

typedef struct {
  uint32_t number, wire;
  uint64_t value;
  // what a length delimited field holds.
  Reader bytes;
} Field;

static uint64_t read_varint(Reader *r) {
  uint64_t value = 0;

  for (int shift = 0; shift < 64; shift += 7) {
    if (r->at >= r->end) {
      r->bad = true;
      return 0;
    }

    uint8_t byte = *r->at++;
    value |= (uint64_t)(byte & 0x7f) << shift;

    if ((byte & 0x80) == 0) {
      return value;
    }
  }

  r->bad = true;
  return 0;
}

static uint64_t read_fixed(Reader *r, size_t size) {
  if ((size_t)(r->end - r->at) < size) {
    r->bad = true;
    return 0;
  }

  uint64_t value = 0;

  for (size_t i = 0; i < size; i++) {
    value |= (uint64_t)r->at[i] << (8 * i);
  }

  r->at += size;
  return value;
}

static bool next_field(Reader *r, Field *f) {
  if (r->bad || r->at >= r->end) {
    return false;
  }

  uint64_t tag = read_varint(r);

  *f = (Field){.number = (uint32_t)(tag >> 3), .wire = (uint32_t)(tag & 7)};

  switch (f->wire) {
    case WIRE_VARINT:
      f->value = read_varint(r);
      break;

    case WIRE_FIXED64:
      f->value = read_fixed(r, 8);
      break;

    case WIRE_FIXED32:
      f->value = read_fixed(r, 4);
      break;

    case WIRE_BYTES: {
      uint64_t len = read_varint(r);

      if (r->bad || len > (uint64_t)(r->end - r->at)) {
        r->bad = true;
        return false;
      }

      f->bytes = (Reader){.at = r->at, .end = r->at + len};
      r->at += len;
      break;
    }

    default:
      r->bad = true;
      break;
  }

  return !r->bad;
}

// the value of a wrapper message, 0 when it is not set.
static uint64_t wrapped_number(Reader r) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_VARINT) {
      return f.value;
    }
  }

  return 0;
}

static void write_wrapped_string(cima_json_writer *w, Reader r) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      cima_json_write_string_n(w, (const char *)f.bytes.at,
                               (size_t)(f.bytes.end - f.bytes.at));
      return;
    }
  }

  json_put(w, "");
}

// a repeated fixed64 comes packed, or one value a field from old writers.
static void write_ids(cima_json_writer *w, Reader r, uint32_t number) {
  Field f;

  json_begin_array(w);

  while (next_field(&r, &f)) {
    if (f.number != number) {
      continue;
    }

    if (f.wire == WIRE_FIXED64) {
      snowflake_write(w, f.value);
    }
    else if (f.wire == WIRE_BYTES) {
      while (!f.bytes.bad && f.bytes.end - f.bytes.at >= 8) {
        snowflake_write(w, read_fixed(&f.bytes, 8));
      }
    }
  }

  json_end_array(w);
}

static void write_folder(cima_json_writer *w, Reader r) {
  Reader fields = r;
  Field f;

  json_begin_object(w);
  json_key(w, "guild_ids"), write_ids(w, r, 1);

  while (next_field(&fields, &f)) {
    if (f.wire != WIRE_BYTES) {
      continue;
    }

    switch (f.number) {
      case 2:
        json_key(w, "id"), snowflake_write(w, wrapped_number(f.bytes));
        break;

      case 3:
        json_key(w, "name"), write_wrapped_string(w, f.bytes);
        break;

      case 4:
        json_field(w, "color", wrapped_number(f.bytes));
        break;
    }
  }

  json_end_object(w);
}

static void write_folders(cima_json_writer *w, Reader r) {
  Reader fields = r;
  Field f;

  json_key(w, "guild_folders");
  json_begin_array(w);

  while (next_field(&fields, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      write_folder(w, f.bytes);
    }
  }

  json_end_array(w);
  json_key(w, "guild_positions"), write_ids(w, r, 2);
}

static void write_custom_status(cima_json_writer *w, Reader r) {
  Field f;

  json_begin_object(w);

  while (next_field(&r, &f)) {
    size_t len = (size_t)(f.bytes.end - f.bytes.at);

    if (f.number == 1 && f.wire == WIRE_BYTES) {
      json_key(w, "text"),
        cima_json_write_string_n(w, (const char *)f.bytes.at, len);
    }
    else if (f.number == 2 && f.wire == WIRE_FIXED64) {
      json_key(w, "emoji_id"), snowflake_write(w, f.value);
    }
    else if (f.number == 3 && f.wire == WIRE_BYTES) {
      json_key(w, "emoji_name"),
        cima_json_write_string_n(w, (const char *)f.bytes.at, len);
    }
    else if (f.number == 4 && f.wire == WIRE_FIXED64) {
      json_key(w, "expires_at_ms"), snowflake_write(w, f.value);
    }
  }

  json_end_object(w);
}

static void write_status(cima_json_writer *w, Reader r) {
  char *section = base64_encode(r.at, (size_t)(r.end - r.at));
  uint64_t expires = 0;
  bool custom = false;
  Field f;

  json_field(w, "status_proto", section);
  free(section);

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      json_key(w, "status"), write_wrapped_string(w, f.bytes);
    }
    else if (f.number == 2 && f.wire == WIRE_BYTES) {
      json_key(w, "custom_status"), write_custom_status(w, f.bytes);
      custom = true;
    }
    else if (f.number == 4 && f.wire == WIRE_FIXED64) {
      expires = f.value;
    }
  }

  // both are gone when the message leaves them out.
  json_key(w, "status_expires_at_ms"), snowflake_write(w, expires);

  if (!custom) {
    json_field(w, "custom_status", nullptr);
  }
}

typedef struct {
  uint8_t *data;
  size_t len, cap;
} Bytes;

static void put_bytes(Bytes *b, const void *data, size_t len) {
  if (b->len + len > b->cap) {
    b->cap = (b->len + len) * 2 + 16;
    b->data = realloc(b->data, b->cap);

    if (b->data == nullptr) {
      abort();
    }
  }

  memcpy(b->data + b->len, data, len);
  b->len += len;
}

static void put_varint(Bytes *b, uint64_t value) {
  do {
    uint8_t byte = (uint8_t)(value & 0x7f);

    value >>= 7;
    byte |= value != 0 ? 0x80 : 0;
    put_bytes(b, &byte, 1);
  } while (value != 0);
}

static void put_tag(Bytes *b, uint32_t number, uint32_t wire) {
  put_varint(b, (uint64_t)number << 3 | wire);
}

static void put_message(Bytes *b, uint32_t number, const Bytes *message) {
  put_tag(b, number, WIRE_BYTES);
  put_varint(b, message->len);
  put_bytes(b, message->data, message->len);
}

char *settings_proto_with_status(const char *section, const char *status,
                                 int64_t expires_ms, int64_t now_ms) {
  size_t len = 0;
  uint8_t *old = section != nullptr ? base64_decode(section, &len) : nullptr;
  Reader r = {.at = old, .end = old + len};
  Bytes out = {}, created = {}, name = {}, whole = {};
  bool same = false;
  Field f;

  // everything but the status and its times is kept as it was written.
  while (old != nullptr && r.at < r.end) {
    const uint8_t *from = r.at;

    if (!next_field(&r, &f)) {
      break;
    }

    if (f.number == 1 && f.wire == WIRE_BYTES) {
      Reader value = f.bytes;
      Field inner;

      while (next_field(&value, &inner)) {
        size_t size = (size_t)(inner.bytes.end - inner.bytes.at);

        same = inner.number == 1 && inner.wire == WIRE_BYTES &&
               size == strlen(status) &&
               memcmp(inner.bytes.at, status, size) == 0;
      }
    }
    else if (f.number == 5 && f.wire == WIRE_BYTES) {
      created.len = 0;
      put_bytes(&created, f.bytes.at, (size_t)(f.bytes.end - f.bytes.at));
    }
    else if (f.number != 4) {
      put_bytes(&out, from, (size_t)(r.at - from));
    }
  }

  put_tag(&name, 1, WIRE_BYTES);
  put_varint(&name, strlen(status));
  put_bytes(&name, status, strlen(status));
  put_message(&out, 1, &name);

  if (expires_ms > 0) {
    uint8_t fixed[8];

    for (int i = 0; i < 8; i++) {
      fixed[i] = (uint8_t)((uint64_t)expires_ms >> (8 * i));
    }

    put_tag(&out, 4, WIRE_FIXED64);
    put_bytes(&out, fixed, sizeof fixed);
  }

  // the time the status was picked only moves when the status does.
  if (!same || created.len == 0) {
    created.len = 0;
    put_tag(&created, 1, WIRE_VARINT);
    put_varint(&created, (uint64_t)now_ms);
  }

  put_message(&out, 5, &created);
  put_message(&whole, 11, &out);

  char *text = base64_encode(whole.data, whole.len);

  free(old);
  free(out.data);
  free(created.data);
  free(name.data);
  free(whole.data);
  return text;
}

// a TextAndImagesSettings left without the field has its default, 0.
static void write_text_and_images(cima_json_writer *w, Reader r) {
  uint64_t animate_stickers = 0;
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 15 && f.wire == WIRE_BYTES) {
      animate_stickers = wrapped_number(f.bytes);
    }
  }

  json_field(w, "animate_stickers", animate_stickers);
}

char *settings_proto_to_json(const char *base64) {
  size_t len = 0;
  uint8_t *bytes = base64_decode(base64, &len);

  if (bytes == nullptr) {
    return nullptr;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  cima_json_writer w = {};
  Field f;

  json_begin_object(&w);

  while (next_field(&r, &f)) {
    if (f.wire != WIRE_BYTES) {
      continue;
    }

    if (f.number == 6) {
      write_text_and_images(&w, f.bytes);
    }
    else if (f.number == 11) {
      write_status(&w, f.bytes);
    }
    else if (f.number == 14) {
      write_folders(&w, f.bytes);
    }
  }

  json_end_object(&w);
  free(bytes);

  if (r.bad) {
    cima_json_writer_dispose(&w);
    return nullptr;
  }

  return cima_json_writer_finish(&w, nullptr);
}

// from the same classes:
//   FrecencyUserSettings  favorite_emojis 5, a FavoriteEmojis,
//                         emoji_frecency 6 and emoji_reaction_frecency 13,
//                         each an EmojiFrecency
//   FavoriteEmojis        emojis 1 repeated string
//   EmojiFrecency         emojis 1, a map from a string to a FrecencyItem
//   FrecencyItem          total_uses 1, recent_uses 2 repeated uint64
static void read_use(Reader r, FrecencyUse *use) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_VARINT) {
      use->total_uses = (uint32_t)f.value;
    }
    else if (f.number == 2 && f.wire == WIRE_VARINT) {
      if (use->nrecent_uses < FRECENCY_SAMPLES) {
        use->recent_uses[use->nrecent_uses++] = (int64_t)f.value;
      }
    }
    else if (f.number == 2 && f.wire == WIRE_BYTES) {
      // packed, as the client writes them.
      Reader packed = f.bytes;

      while (packed.at < packed.end && !packed.bad) {
        uint64_t at = read_varint(&packed);

        if (!packed.bad && use->nrecent_uses < FRECENCY_SAMPLES) {
          use->recent_uses[use->nrecent_uses++] = (int64_t)at;
        }
      }
    }
  }
}

static bool read_entry(Reader r, FrecencyUse *use) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.wire != WIRE_BYTES) {
      continue;
    }

    if (f.number == 1) {
      size_t len = (size_t)(f.bytes.end - f.bytes.at);

      free(use->key);
      use->key = malloc(len + 1);

      if (use->key == nullptr) {
        return false;
      }

      memcpy(use->key, f.bytes.at, len);
      use->key[len] = '\0';
    }
    else if (f.number == 2) {
      read_use(f.bytes, use);
    }
  }

  return use->key != nullptr && *use->key != '\0';
}

// the uses the EmojiFrecency in the field `number` holds.
static FrecencyUse *uses_of(const char *base64, uint32_t number,
                            size_t *count) {
  size_t len = 0, cap = 0;
  uint8_t *bytes = base64_decode(base64, &len);
  FrecencyUse *uses = nullptr;

  *count = 0;

  if (bytes == nullptr) {
    return nullptr;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  Field f;

  while (next_field(&r, &f)) {
    if (f.number != number || f.wire != WIRE_BYTES) {
      continue;
    }

    Reader map = f.bytes;
    Field entry;

    while (next_field(&map, &entry)) {
      if (entry.number != 1 || entry.wire != WIRE_BYTES) {
        continue;
      }

      FrecencyUse use = {};

      if (!read_entry(entry.bytes, &use)) {
        free(use.key);
        continue;
      }

      if (*count == cap) {
        cap = cap ? cap * 2 : 32;

        FrecencyUse *grown = realloc(uses, cap * sizeof *uses);

        if (grown == nullptr) {
          free(use.key);
          break;
        }

        uses = grown;
      }

      uses[(*count)++] = use;
    }
  }

  free(bytes);
  return uses;
}

FrecencyUse *settings_proto_reaction_uses(const char *base64, size_t *count) {
  return uses_of(base64, 13, count);
}

FrecencyUse *settings_proto_emoji_uses(const char *base64, size_t *count) {
  return uses_of(base64, 6, count);
}

char **settings_proto_favorite_emojis(const char *base64, size_t *count) {
  size_t len = 0, cap = 0;
  uint8_t *bytes = base64_decode(base64, &len);
  char **keys = nullptr;

  *count = 0;

  if (bytes == nullptr) {
    return nullptr;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  Field f;

  while (next_field(&r, &f)) {
    if (f.number != 5 || f.wire != WIRE_BYTES) {
      continue;
    }

    Reader list = f.bytes;
    Field item;

    while (next_field(&list, &item)) {
      if (item.number != 1 || item.wire != WIRE_BYTES ||
          item.bytes.at == item.bytes.end) {
        continue;
      }

      if (*count == cap) {
        cap = cap ? cap * 2 : 16;

        char **grown = realloc(keys, cap * sizeof *keys);

        if (grown == nullptr) {
          break;
        }

        keys = grown;
      }

      keys[(*count)++] = str_dup_n((const char *)item.bytes.at,
                                   (size_t)(item.bytes.end - item.bytes.at));
    }
  }

  free(bytes);
  return keys;
}

// from the same classes:
//   FrecencyUserSettings  favorite_gifs 2, a FavoriteGIFs
//   FavoriteGIFs          gifs 1, a map from the url to a FavoriteGIF,
//                         hide_tooltip 2
//   FavoriteGIF           format 1, src 2, width 3, height 4, order 5
// the client refuses to save favorites that take more than this.
constexpr size_t FAVORITES_MAX_BYTES = 762880;

static char *copy_bytes(Reader r) {
  return str_dup_n((const char *)r.at, (size_t)(r.end - r.at));
}

static void read_favorite(Reader r, FavoriteGif *gif) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.wire == WIRE_BYTES && f.number == 2) {
      free(gif->src);
      gif->src = copy_bytes(f.bytes);
    }
    else if (f.wire == WIRE_VARINT) {
      switch (f.number) {
        case 1:
          gif->format = f.value <= GIF_FORMAT_VIDEO ? (GifFormat)f.value
                                                    : GIF_FORMAT_NONE;
          break;

        case 3:
          gif->width = (uint32_t)f.value;
          break;

        case 4:
          gif->height = (uint32_t)f.value;
          break;

        case 5:
          gif->order = (uint32_t)f.value;
          break;
      }
    }
  }
}

static void read_favorites(Reader r, FavoriteGifs *out) {
  size_t cap = 0;
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 2 && f.wire == WIRE_VARINT) {
      out->hide_tooltip = f.value != 0;
      continue;
    }

    if (f.number != 1 || f.wire != WIRE_BYTES) {
      continue;
    }

    FavoriteGif gif = {};
    Reader entry = f.bytes;
    Field e;

    while (next_field(&entry, &e)) {
      if (e.wire != WIRE_BYTES) {
        continue;
      }

      if (e.number == 1) {
        free(gif.url);
        gif.url = copy_bytes(e.bytes);
      }
      else if (e.number == 2) {
        read_favorite(e.bytes, &gif);
      }
    }

    if (gif.url == nullptr || *gif.url == '\0' || gif.src == nullptr) {
      free(gif.url);
      free(gif.src);
      continue;
    }

    if (out->count == cap) {
      cap = cap ? cap * 2 : 32;
      out->gifs = realloc(out->gifs, cap * sizeof *out->gifs);

      if (out->gifs == nullptr) {
        cilo_oom(cap * sizeof *out->gifs);
      }
    }

    out->gifs[out->count++] = gif;
  }
}

FavoriteGifs settings_proto_favorite_gifs(const char *base64) {
  FavoriteGifs out = {};
  size_t len = 0;
  uint8_t *bytes = base64_decode(base64, &len);

  if (bytes == nullptr) {
    return out;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 2 && f.wire == WIRE_BYTES) {
      settings_proto_free_favorites(&out);
      read_favorites(f.bytes, &out);
      out.present = true;
    }
  }

  free(bytes);
  return out;
}

void settings_proto_free_favorites(FavoriteGifs *favorites) {
  for (size_t i = 0; i < favorites->count; i++) {
    free(favorites->gifs[i].url);
    free(favorites->gifs[i].src);
  }

  free(favorites->gifs);
  *favorites = (FavoriteGifs){};
}

static void put_string(Bytes *b, uint32_t number, const char *s) {
  put_tag(b, number, WIRE_BYTES);
  put_varint(b, strlen(s));
  put_bytes(b, s, strlen(s));
}

static void put_number(Bytes *b, uint32_t number, uint64_t value) {
  // proto3 leaves out what is 0.
  if (value != 0) {
    put_tag(b, number, WIRE_VARINT);
    put_varint(b, value);
  }
}

// from the same classes:
//   FrecencyUserSettings  favorite_stickers 3, a FavoriteStickers,
//                         sticker_frecency 4, a StickerFrecency
//   FavoriteStickers      sticker_ids 1 repeated fixed64, packed
//   StickerFrecency       stickers 1, a map from a fixed64 to a FrecencyItem
//   FrecencyItem          frecency 3 and score 4, both int32
static void read_favorite_stickers(Reader r, StickerSettings *out) {
  size_t cap = 0;
  Field f;

  while (next_field(&r, &f)) {
    if (f.number != 1 || (f.wire != WIRE_BYTES && f.wire != WIRE_FIXED64)) {
      continue;
    }

    // one id when not packed.
    uint8_t lone[8];
    Reader ids = f.bytes;

    if (f.wire == WIRE_FIXED64) {
      for (int i = 0; i < 8; i++) {
        lone[i] = (uint8_t)(f.value >> (8 * i));
      }

      ids = (Reader){.at = lone, .end = lone + sizeof lone};
    }

    while (ids.at < ids.end && !ids.bad) {
      Snowflake id = read_fixed(&ids, 8);

      if (ids.bad || id == 0) {
        continue;
      }

      if (out->nfavorites == cap) {
        cap = cap ? cap * 2 : 16;
        out->favorites = realloc(out->favorites, cap * sizeof *out->favorites);

        if (out->favorites == nullptr) {
          cilo_oom(cap * sizeof *out->favorites);
        }
      }

      out->favorites[out->nfavorites++] = id;
    }
  }
}

static void read_sticker_uses(Reader r, StickerSettings *out) {
  size_t cap = 0;
  Field f;

  while (next_field(&r, &f)) {
    if (f.number != 1 || f.wire != WIRE_BYTES) {
      continue;
    }

    FrecencyUse item = {};
    Snowflake id = 0;
    Reader entry = f.bytes;
    Field e;

    while (next_field(&entry, &e)) {
      if (e.number == 1 && e.wire == WIRE_FIXED64) {
        id = e.value;
      }
      else if (e.number == 2 && e.wire == WIRE_BYTES) {
        read_use(e.bytes, &item);
      }
    }

    if (id == 0 || item.nrecent_uses == 0) {
      continue;
    }

    if (out->nuses == cap) {
      cap = cap ? cap * 2 : 32;
      out->uses = realloc(out->uses, cap * sizeof *out->uses);

      if (out->uses == nullptr) {
        cilo_oom(cap * sizeof *out->uses);
      }
    }

    StickerUse *use = &out->uses[out->nuses++];

    *use = (StickerUse){
      .id = id,
      .total_uses = item.total_uses,
      .nrecent_uses = item.nrecent_uses,
    };
    memcpy(use->recent_uses, item.recent_uses, sizeof use->recent_uses);
  }
}

StickerSettings settings_proto_stickers(const char *base64) {
  StickerSettings out = {};
  size_t len = 0;
  uint8_t *bytes = base64_decode(base64, &len);

  if (bytes == nullptr) {
    return out;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 3 && f.wire == WIRE_BYTES) {
      free(out.favorites);
      out.favorites = nullptr;
      out.nfavorites = 0;
      read_favorite_stickers(f.bytes, &out);
      out.has_favorites = true;
    }
    else if (f.number == 4 && f.wire == WIRE_BYTES) {
      free(out.uses);
      out.uses = nullptr;
      out.nuses = 0;
      read_sticker_uses(f.bytes, &out);
      out.has_uses = true;
    }
  }

  free(bytes);
  return out;
}

void settings_proto_free_stickers(StickerSettings *stickers) {
  free(stickers->uses);
  free(stickers->favorites);
  *stickers = (StickerSettings){};
}

uint32_t frecency_weight(int64_t days) {
  return days <= 3    ? 100
         : days <= 15 ? 70
         : days <= 30 ? 50
         : days <= 45 ? 30
         : days <= 80 ? 10
                      : 1;
}

uint64_t settings_proto_sticker_score(const StickerUse *use, int64_t now_ms) {
  uint64_t score = 0;

  for (size_t i = 0; i < use->nrecent_uses; i++) {
    if (use->recent_uses[i] > 0) {
      score += frecency_weight((now_ms - use->recent_uses[i]) / 86400000);
    }
  }

  return score;
}

uint64_t settings_proto_sticker_frecency(const StickerUse *use,
                                         int64_t now_ms) {
  uint64_t score = settings_proto_sticker_score(use, now_ms);

  if (score == 0 || use->nrecent_uses == 0) {
    return 0;
  }

  uint64_t total = (uint64_t)use->total_uses * score;

  return (total + use->nrecent_uses - 1) / use->nrecent_uses;
}

static void put_fixed64(Bytes *b, uint64_t value) {
  uint8_t fixed[8];

  for (int i = 0; i < 8; i++) {
    fixed[i] = (uint8_t)(value >> (8 * i));
  }

  put_bytes(b, fixed, sizeof fixed);
}

char *settings_proto_with_favorite_stickers(const Snowflake *ids,
                                            size_t count) {
  Bytes packed = {}, list = {}, whole = {};

  for (size_t i = 0; i < count; i++) {
    put_fixed64(&packed, ids[i]);
  }

  if (count != 0) {
    put_message(&list, 1, &packed);
  }

  put_message(&whole, 3, &list);

  char *text = base64_encode(whole.data, whole.len);

  free(packed.data);
  free(list.data);
  free(whole.data);
  return text;
}

static int64_t last_use(const StickerUse *use) {
  return use->nrecent_uses ? use->recent_uses[use->nrecent_uses - 1] : 0;
}

static int by_last_use(const void *a, const void *b) {
  int64_t x = last_use(*(const StickerUse *const *)a);
  int64_t y = last_use(*(const StickerUse *const *)b);

  return x < y ? 1 : x > y ? -1 : 0;
}

// the client saves this many, the ones used last.
constexpr size_t STICKER_USES_MAX = 100;

char *settings_proto_with_sticker_uses(const StickerUse *uses, size_t count,
                                       int64_t now_ms) {
  const StickerUse **sorted = calloc(count ? count : 1, sizeof *sorted);
  Bytes map = {}, entry = {}, item = {}, recent = {}, whole = {};

  if (sorted == nullptr) {
    cilo_oom(count * sizeof *sorted);
  }

  for (size_t i = 0; i < count; i++) {
    sorted[i] = &uses[i];
  }

  qsort(sorted, count, sizeof *sorted, by_last_use);

  for (size_t i = 0; i < count && i < STICKER_USES_MAX; i++) {
    const StickerUse *use = sorted[i];
    uint64_t frecency = settings_proto_sticker_frecency(use, now_ms);

    // what the client forgets as it computes.
    if (frecency == 0) {
      continue;
    }

    entry.len = item.len = recent.len = 0;

    for (size_t j = 0; j < use->nrecent_uses; j++) {
      if (use->recent_uses[j] > 0) {
        put_varint(&recent, (uint64_t)use->recent_uses[j]);
      }
    }

    put_number(&item, 1, use->total_uses);
    put_message(&item, 2, &recent);
    put_number(&item, 3, frecency > INT32_MAX ? INT32_MAX : frecency);
    put_number(&item, 4, settings_proto_sticker_score(use, now_ms));

    put_tag(&entry, 1, WIRE_FIXED64);
    put_fixed64(&entry, use->id);
    put_message(&entry, 2, &item);
    put_message(&map, 1, &entry);
  }

  put_message(&whole, 4, &map);

  char *text = base64_encode(whole.data, whole.len);

  free(sorted);
  free(map.data);
  free(entry.data);
  free(item.data);
  free(recent.data);
  free(whole.data);
  return text;
}

char *settings_proto_with_favorite_gifs(const FavoriteGifs *favorites) {
  Bytes list = {}, entry = {}, value = {}, whole = {};

  for (size_t i = 0; i < favorites->count; i++) {
    const FavoriteGif *gif = &favorites->gifs[i];

    entry.len = value.len = 0;
    put_number(&value, 1, gif->format);
    put_string(&value, 2, gif->src);
    put_number(&value, 3, gif->width);
    put_number(&value, 4, gif->height);
    put_number(&value, 5, gif->order);

    put_string(&entry, 1, gif->url);
    put_message(&entry, 2, &value);
    put_message(&list, 1, &entry);
  }

  put_number(&list, 2, favorites->hide_tooltip);

  char *text = nullptr;

  if (list.len <= FAVORITES_MAX_BYTES) {
    put_message(&whole, 2, &list);
    text = base64_encode(whole.data, whole.len);
  }

  free(list.data);
  free(entry.data);
  free(value.data);
  free(whole.data);
  return text;
}

static int by_last_emoji_use(const void *a, const void *b) {
  const FrecencyUse *x = *(const FrecencyUse *const *)a;
  const FrecencyUse *y = *(const FrecencyUse *const *)b;
  int64_t xl = x->nrecent_uses ? x->recent_uses[x->nrecent_uses - 1] : 0;
  int64_t yl = y->nrecent_uses ? y->recent_uses[y->nrecent_uses - 1] : 0;

  return xl < yl ? 1 : xl > yl ? -1 : 0;
}

// the client saves this many, the ones used last.
constexpr size_t EMOJI_USES_MAX = 100;

char *settings_proto_with_emoji_uses(const FrecencyUse *uses, size_t count,
                                     int64_t now_ms) {
  const FrecencyUse **sorted = calloc(count ? count : 1, sizeof *sorted);
  Bytes map = {}, entry = {}, item = {}, recent = {}, whole = {};

  if (sorted == nullptr) {
    cilo_oom(count * sizeof *sorted);
  }

  for (size_t i = 0; i < count; i++) {
    sorted[i] = &uses[i];
  }

  qsort(sorted, count, sizeof *sorted, by_last_emoji_use);

  for (size_t i = 0; i < count && i < EMOJI_USES_MAX; i++) {
    const FrecencyUse *use = sorted[i];
    uint64_t score = 0, kept = 0;

    entry.len = item.len = recent.len = 0;

    for (size_t j = 0; j < use->nrecent_uses; j++) {
      if (use->recent_uses[j] > 0) {
        score += frecency_weight((now_ms - use->recent_uses[j]) / 86400000);
        put_varint(&recent, (uint64_t)use->recent_uses[j]);
        kept++;
      }
    }

    // what the client forgets as it computes.
    if (score == 0) {
      continue;
    }

    uint64_t frecency = (use->total_uses * score + kept - 1) / kept;

    put_number(&item, 1, use->total_uses);
    put_message(&item, 2, &recent);
    put_number(&item, 3, frecency > INT32_MAX ? INT32_MAX : frecency);
    put_number(&item, 4, score);

    put_string(&entry, 1, use->key);
    put_message(&entry, 2, &item);
    put_message(&map, 1, &entry);
  }

  put_message(&whole, 6, &map);

  char *text = base64_encode(whole.data, whole.len);

  free(sorted);
  free(map.data);
  free(entry.data);
  free(item.data);
  free(recent.data);
  free(whole.data);
  return text;
}
