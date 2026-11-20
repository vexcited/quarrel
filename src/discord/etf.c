#include "discord/etf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <cima/json_dsl.h>

enum {
  ETF_VERSION = 131,
  ETF_NEW_FLOAT = 70,
  ETF_COMPRESSED = 80,
  ETF_SMALL_INTEGER = 97,
  ETF_INTEGER = 98,
  ETF_FLOAT = 99,
  ETF_ATOM = 100,
  ETF_SMALL_TUPLE = 104,
  ETF_LARGE_TUPLE = 105,
  ETF_NIL = 106,
  ETF_STRING = 107,
  ETF_LIST = 108,
  ETF_BINARY = 109,
  ETF_SMALL_BIG = 110,
  ETF_LARGE_BIG = 111,
  ETF_SMALL_ATOM = 115,
  ETF_MAP = 116,
  ETF_ATOM_UTF8 = 118,
  ETF_SMALL_ATOM_UTF8 = 119,
};

constexpr uint32_t ETF_MAX_DEPTH = 128;
// what a compressed term may inflate to.
constexpr uint32_t ETF_MAX_INFLATED = 256u << 20;

typedef struct {
  char *data;
  size_t len, cap;
} Buffer;

static void buffer_put(Buffer *b, const void *bytes, size_t len) {
  if (b->cap - b->len < len + 1) {
    size_t cap = b->cap != 0 ? b->cap : 256;

    while (cap - b->len < len + 1) {
      cap *= 2;
    }

    char *grown = realloc(b->data, cap);

    if (grown == nullptr) {
      cilo_oom(cap);
    }

    b->data = grown;
    b->cap = cap;
  }

  memcpy(b->data + b->len, bytes, len);
  b->len += len;
  b->data[b->len] = '\0';
}

static void buffer_byte(Buffer *b, uint8_t byte) {
  buffer_put(b, &byte, 1);
}

static void buffer_text(Buffer *b, const char *text) {
  buffer_put(b, text, strlen(text));
}

static void buffer_u32(Buffer *b, uint32_t n) {
  uint8_t bytes[] = {
    (uint8_t)(n >> 24),
    (uint8_t)(n >> 16),
    (uint8_t)(n >> 8),
    (uint8_t)n,
  };

  buffer_put(b, bytes, sizeof bytes);
}

/* ========================================================================== */
/* decoding.                                                                  */
/* ========================================================================== */

typedef struct {
  const uint8_t *data;
  size_t len, at;
  uint32_t depth;
  Buffer out;
} Reader;

static bool read_bytes(Reader *r, size_t n, const uint8_t **bytes) {
  if (r->len - r->at < n) {
    return false;
  }

  *bytes = r->data + r->at;
  r->at += n;
  return true;
}

static bool read_u8(Reader *r, uint32_t *n) {
  const uint8_t *b;

  if (!read_bytes(r, 1, &b)) {
    return false;
  }

  *n = b[0];
  return true;
}

static bool read_u16(Reader *r, uint32_t *n) {
  const uint8_t *b;

  if (!read_bytes(r, 2, &b)) {
    return false;
  }

  *n = (uint32_t)b[0] << 8 | b[1];
  return true;
}

static bool read_u32(Reader *r, uint32_t *n) {
  const uint8_t *b;

  if (!read_bytes(r, 4, &b)) {
    return false;
  }

  *n = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
  return true;
}

// how many bytes the utf-8 sequence at `s` takes, 0 when it is not one.
static size_t utf8_sequence(const uint8_t *s, size_t left) {
  uint8_t c = s[0];
  size_t n = c < 0x80 ? 1 : c >> 5 == 0x06 ? 2 : c >> 4 == 0x0e ? 3 : 4;

  if ((c >= 0x80 && c < 0xc2) || c > 0xf4 || left < n) {
    return 0;
  }

  for (size_t i = 1; i < n; i++) {
    if (s[i] >> 6 != 0x02) {
      return 0;
    }
  }

  // overlong forms, surrogates and what lies past the last code point.
  bool bad = (c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] > 0x9f) ||
             (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] > 0x8f);

  return bad ? 0 : n;
}

// a json string out of any bytes: what is not utf-8 becomes U+FFFD.
static void write_string(Buffer *out, const uint8_t *s, size_t len) {
  static const char hex[] = "0123456789abcdef";
  size_t from = 0;

  buffer_byte(out, '"');

  for (size_t i = 0; i < len;) {
    uint8_t c = s[i];
    size_t n = utf8_sequence(s + i, len - i);

    if (n != 0 && c >= 0x20 && c != '"' && c != '\\') {
      i += n;
      continue;
    }

    buffer_put(out, s + from, i - from);

    if (n == 0) {
      buffer_text(out, "\xef\xbf\xbd");
    }
    else if (c == '"' || c == '\\') {
      buffer_put(out, (char[]){'\\', (char)c}, 2);
    }
    else {
      buffer_put(out, (char[]){'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]},
                 6);
    }

    i += n != 0 ? n : 1;
    from = i;
  }

  buffer_put(out, s + from, len - from);
  buffer_byte(out, '"');
}

static void write_i64(Buffer *out, int64_t n) {
  char text[24];

  snprintf(text, sizeof text, "%lld", (long long)n);
  buffer_text(out, text);
}

static void write_f64(Buffer *out, double n) {
  char text[32];

  if (!isfinite(n)) {
    buffer_text(out, "null");
    return;
  }

  snprintf(text, sizeof text, "%.17g", n);
  buffer_text(out, text);
}

static bool decode_term(Reader *r, bool key);

static bool decode_atom(Reader *r, uint32_t len, bool key) {
  const uint8_t *name;

  if (!read_bytes(r, len, &name)) {
    return false;
  }

  if (!key) {
    if ((len == 3 && memcmp(name, "nil", 3) == 0) ||
        (len == 4 && memcmp(name, "null", 4) == 0)) {
      buffer_text(&r->out, "null");
      return true;
    }

    if (len == 4 && memcmp(name, "true", 4) == 0) {
      buffer_text(&r->out, "true");
      return true;
    }

    if (len == 5 && memcmp(name, "false", 5) == 0) {
      buffer_text(&r->out, "false");
      return true;
    }
  }

  write_string(&r->out, name, len);
  return true;
}

static bool decode_float(Reader *r) {
  const uint8_t *digits;
  char text[32] = {};
  char *end;

  if (!read_bytes(r, 31, &digits)) {
    return false;
  }

  memcpy(text, digits, 31);

  double n = strtod(text, &end);

  if (end == text) {
    return false;
  }

  write_f64(&r->out, n);
  return true;
}

static bool decode_new_float(Reader *r) {
  const uint8_t *b;
  uint64_t bits = 0;
  double n;

  if (!read_bytes(r, 8, &b)) {
    return false;
  }

  for (size_t i = 0; i < 8; i++) {
    bits = bits << 8 | b[i];
  }

  memcpy(&n, &bits, sizeof n);
  write_f64(&r->out, n);
  return true;
}

// erlpack's rule: up to 4 bytes is a number, up to 8 a string of digits,
// which is how every snowflake arrives.
static bool decode_big(Reader *r, uint32_t digits, bool key) {
  const uint8_t *b;
  uint32_t sign;
  uint64_t value = 0;
  char text[24];

  if (!read_u8(r, &sign) || digits > 8 || !read_bytes(r, digits, &b)) {
    return false;
  }

  for (uint32_t i = 0; i < digits; i++) {
    value |= (uint64_t)b[i] << (8 * i);
  }

  snprintf(text, sizeof text, "%s%llu", sign != 0 && value != 0 ? "-" : "",
           (unsigned long long)value);

  if (digits <= 4 && !key) {
    buffer_text(&r->out, text);
  }
  else {
    write_string(&r->out, (const uint8_t *)text, strlen(text));
  }

  return true;
}

static bool decode_items(Reader *r, uint32_t count) {
  // every item takes a byte at least.
  if (count > r->len - r->at) {
    return false;
  }

  buffer_byte(&r->out, '[');

  for (uint32_t i = 0; i < count; i++) {
    if (i != 0) {
      buffer_byte(&r->out, ',');
    }

    if (!decode_term(r, false)) {
      return false;
    }
  }

  buffer_byte(&r->out, ']');
  return true;
}

static bool decode_list(Reader *r) {
  uint32_t count, tail;

  // erlpack refuses improper lists too.
  return read_u32(r, &count) && decode_items(r, count) && read_u8(r, &tail) &&
         tail == ETF_NIL;
}

static bool decode_string(Reader *r) {
  const uint8_t *b;
  uint32_t len;

  if (!read_u16(r, &len) || !read_bytes(r, len, &b)) {
    return false;
  }

  buffer_byte(&r->out, '[');

  for (uint32_t i = 0; i < len; i++) {
    if (i != 0) {
      buffer_byte(&r->out, ',');
    }

    write_i64(&r->out, b[i]);
  }

  buffer_byte(&r->out, ']');
  return true;
}

static bool decode_map(Reader *r) {
  uint32_t count;

  if (!read_u32(r, &count) || count > (r->len - r->at) / 2) {
    return false;
  }

  buffer_byte(&r->out, '{');

  for (uint32_t i = 0; i < count; i++) {
    if (i != 0) {
      buffer_byte(&r->out, ',');
    }

    if (!decode_term(r, true)) {
      return false;
    }

    buffer_byte(&r->out, ':');

    if (!decode_term(r, false)) {
      return false;
    }
  }

  buffer_byte(&r->out, '}');
  return true;
}

static bool decode_compressed(Reader *r) {
  uint32_t size;

  if (!read_u32(r, &size) || size > ETF_MAX_INFLATED) {
    return false;
  }

  uint8_t *inflated = malloc(size != 0 ? size : 1);

  if (inflated == nullptr) {
    cilo_oom(size);
  }

  uLongf got = size;
  uLong left = (uLong)(r->len - r->at);
  int result = uncompress2(inflated, &got, r->data + r->at, &left);
  bool ok = result == Z_OK && got == size;

  if (ok) {
    Reader inner = {
      .data = inflated,
      .len = size,
      .depth = r->depth,
      .out = r->out,
    };

    ok = decode_term(&inner, false) && inner.at == inner.len;
    r->out = inner.out;
    r->at += left;
  }

  free(inflated);
  return ok;
}

// `key` is set for the key of a map, which json wants as a string.
static bool decode_term(Reader *r, bool key) {
  uint32_t tag, n;
  const uint8_t *b;
  char text[16];

  if (r->depth >= ETF_MAX_DEPTH || !read_u8(r, &tag)) {
    return false;
  }

  bool ok = false;

  r->depth++;

  switch (tag) {
    case ETF_SMALL_INTEGER:
    case ETF_INTEGER:
      if (tag == ETF_SMALL_INTEGER ? !read_u8(r, &n) : !read_u32(r, &n)) {
        break;
      }

      snprintf(text, sizeof text, "%lld",
               tag == ETF_INTEGER ? (long long)(int32_t)n : (long long)n);

      if (key) {
        write_string(&r->out, (const uint8_t *)text, strlen(text));
      }
      else {
        buffer_text(&r->out, text);
      }

      ok = true;
      break;
    case ETF_SMALL_BIG:
      ok = read_u8(r, &n) && decode_big(r, n, key);
      break;
    case ETF_LARGE_BIG:
      ok = read_u32(r, &n) && decode_big(r, n, key);
      break;
    case ETF_ATOM:
    case ETF_ATOM_UTF8:
      ok = read_u16(r, &n) && decode_atom(r, n, key);
      break;
    case ETF_SMALL_ATOM:
    case ETF_SMALL_ATOM_UTF8:
      ok = read_u8(r, &n) && decode_atom(r, n, key);
      break;
    case ETF_BINARY:
      ok = read_u32(r, &n) && read_bytes(r, n, &b);

      if (ok) {
        write_string(&r->out, b, n);
      }

      break;
    default:
      break;
  }

  // everything below cannot name a member of an object.
  if (!ok && !key) {
    switch (tag) {
      case ETF_FLOAT:
        ok = decode_float(r);
        break;
      case ETF_NEW_FLOAT:
        ok = decode_new_float(r);
        break;
      case ETF_SMALL_TUPLE:
        ok = read_u8(r, &n) && decode_items(r, n);
        break;
      case ETF_LARGE_TUPLE:
        ok = read_u32(r, &n) && decode_items(r, n);
        break;
      case ETF_NIL:
        buffer_text(&r->out, "[]");
        ok = true;
        break;
      case ETF_STRING:
        ok = decode_string(r);
        break;
      case ETF_LIST:
        ok = decode_list(r);
        break;
      case ETF_MAP:
        ok = decode_map(r);
        break;
      case ETF_COMPRESSED:
        ok = decode_compressed(r);
        break;
      default:
        break;
    }
  }

  r->depth--;
  return ok;
}

bool etf_to_json(const uint8_t *etf, size_t len, char **json,
                 size_t *json_len) {
  Reader r = {.data = etf, .len = len, .at = 1};

  if (len < 2 || etf[0] != ETF_VERSION || !decode_term(&r, false) ||
      r.at != r.len) {
    free(r.out.data);
    return false;
  }

  *json = r.out.data;

  if (json_len != nullptr) {
    *json_len = r.out.len;
  }

  return true;
}

/* ========================================================================== */
/* encoding, with the choices of erlpack's packer.                            */
/* ========================================================================== */

static void encode_binary(Buffer *out, const char *s, size_t len) {
  buffer_byte(out, ETF_BINARY);
  buffer_u32(out, (uint32_t)len);
  buffer_put(out, s, len);
}

static void encode_atom(Buffer *out, const char *name) {
  buffer_byte(out, ETF_SMALL_ATOM);
  buffer_byte(out, (uint8_t)strlen(name));
  buffer_text(out, name);
}

static void encode_new_float(Buffer *out, double n) {
  uint64_t bits;

  memcpy(&bits, &n, sizeof bits);
  buffer_byte(out, ETF_NEW_FLOAT);
  buffer_u32(out, (uint32_t)(bits >> 32));
  buffer_u32(out, (uint32_t)bits);
}

// 0 to 255 is a small integer, the rest of int32 an integer, the rest of
// uint32 a big of 4 bytes, and anything else a float, as javascript numbers
// are packed.
static void encode_number(Buffer *out, const cima_json *v) {
  bool whole = v->type != CIMA_JSON_DOUBLE;
  double n = v->type == CIMA_JSON_INT    ? (double)v->i
             : v->type == CIMA_JSON_UINT ? (double)v->u
                                         : v->d;

  if (!whole || n < -2147483648.0 || n > 4294967295.0) {
    encode_new_float(out, n);
    return;
  }

  int64_t i = (int64_t)n;

  if (i >= 0 && i <= 255) {
    buffer_byte(out, ETF_SMALL_INTEGER);
    buffer_byte(out, (uint8_t)i);
  }
  else if (i <= 2147483647) {
    buffer_byte(out, ETF_INTEGER);
    buffer_u32(out, (uint32_t)(int32_t)i);
  }
  else {
    buffer_put(out, (uint8_t[]){ETF_SMALL_BIG, 4, 0}, 3);
    buffer_put(out, (uint8_t[]){(uint8_t)i, (uint8_t)(i >> 8)}, 2);
    buffer_put(out, (uint8_t[]){(uint8_t)(i >> 16), (uint8_t)(i >> 24)}, 2);
  }
}

static void encode_value(Buffer *out, const cima_json *v) {
  switch (v->type) {
    case CIMA_JSON_BOOL:
      encode_atom(out, v->b ? "true" : "false");
      break;
    case CIMA_JSON_INT:
    case CIMA_JSON_UINT:
    case CIMA_JSON_DOUBLE:
      encode_number(out, v);
      break;
    case CIMA_JSON_STRING:
      encode_binary(out, v->str, v->len);
      break;
    case CIMA_JSON_ARRAY:
      if (v->len == 0) {
        buffer_byte(out, ETF_NIL);
        break;
      }

      buffer_byte(out, ETF_LIST);
      buffer_u32(out, v->len);

      CIMA_EACH(item, json_items(v)) {
        encode_value(out, item);
      }

      buffer_byte(out, ETF_NIL);
      break;
    case CIMA_JSON_OBJECT:
      buffer_byte(out, ETF_MAP);
      buffer_u32(out, v->len);

      CIMA_EACH(member, json_members(v)) {
        encode_binary(out, member->key, cima_json_key_len(member));
        encode_value(out, &member->value);
      }

      break;
    default:
      encode_atom(out, "nil");
      break;
  }
}

bool etf_from_json(const char *json, size_t len, uint8_t **etf,
                   size_t *etf_len) {
  cilo_arena arena = {};
  const cima_json *value = json_parse(&arena, json, len, nullptr);

  if (value == nullptr) {
    cilo_arena_drop(&arena);
    return false;
  }

  Buffer out = {};

  buffer_byte(&out, ETF_VERSION);
  encode_value(&out, value);
  cilo_arena_drop(&arena);

  *etf = (uint8_t *)out.data;
  *etf_len = out.len;
  return true;
}
