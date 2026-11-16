#include "discord/settings_proto.h"

#include <stdint.h>
#include <stdlib.h>

#include "discord/util.h"
#include "json.h"

// the field numbers, from the protobuf-ts classes of the client's bundle:
//   PreloadedUserSettings  status 11, guild_folders 14
//   StatusSettings         status 1, a StringValue
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

static void write_wrapped_string(JsonWriter *w, Reader r) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      jw_strn(w, (const char *)f.bytes.at, (size_t)(f.bytes.end - f.bytes.at));
      return;
    }
  }

  jw_str(w, "");
}

// a repeated fixed64 comes packed, or one value a field from old writers.
static void write_ids(JsonWriter *w, Reader r, uint32_t number) {
  Field f;

  jw_array(w);

  while (next_field(&r, &f)) {
    if (f.number != number) {
      continue;
    }

    if (f.wire == WIRE_FIXED64) {
      jw_id(w, f.value);
    }
    else if (f.wire == WIRE_BYTES) {
      while (!f.bytes.bad && f.bytes.end - f.bytes.at >= 8) {
        jw_id(w, read_fixed(&f.bytes, 8));
      }
    }
  }

  jw_end(w);
}

static void write_folder(JsonWriter *w, Reader r) {
  Reader fields = r;
  Field f;

  jw_object(w);
  jw_key(w, "guild_ids"), write_ids(w, r, 1);

  while (next_field(&fields, &f)) {
    if (f.wire != WIRE_BYTES) {
      continue;
    }

    switch (f.number) {
      case 2:
        jw_key(w, "id"), jw_id(w, wrapped_number(f.bytes));
        break;

      case 3:
        jw_key(w, "name"), write_wrapped_string(w, f.bytes);
        break;

      case 4:
        jw_key(w, "color"), jw_u64(w, wrapped_number(f.bytes));
        break;
    }
  }

  jw_end(w);
}

static void write_folders(JsonWriter *w, Reader r) {
  Reader fields = r;
  Field f;

  jw_key(w, "guild_folders");
  jw_array(w);

  while (next_field(&fields, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      write_folder(w, f.bytes);
    }
  }

  jw_end(w);
  jw_key(w, "guild_positions"), write_ids(w, r, 2);
}

static void write_status(JsonWriter *w, Reader r) {
  Field f;

  while (next_field(&r, &f)) {
    if (f.number == 1 && f.wire == WIRE_BYTES) {
      jw_key(w, "status"), write_wrapped_string(w, f.bytes);
    }
  }
}

char *settings_proto_to_json(const char *base64) {
  size_t len = 0;
  uint8_t *bytes = base64_decode(base64, &len);

  if (bytes == nullptr) {
    return nullptr;
  }

  Reader r = {.at = bytes, .end = bytes + len};
  JsonWriter w = {};
  Field f;

  jw_object(&w);

  while (next_field(&r, &f)) {
    if (f.wire != WIRE_BYTES) {
      continue;
    }

    if (f.number == 11) {
      write_status(&w, f.bytes);
    }
    else if (f.number == 14) {
      write_folders(&w, f.bytes);
    }
  }

  jw_end(&w);
  free(bytes);

  if (r.bad) {
    jw_free(&w);
    return nullptr;
  }

  return jw_take(&w);
}
