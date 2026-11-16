#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

constexpr uint32_t JSON_MAX_DEPTH = 256;

typedef struct {
  cilo_arena *arena;
  const char *at, *end;
  const char *error;
  uint32_t depth;
} Parser;

static Json *parse_value(Parser *p);

static void skip_space(Parser *p) {
  while (p->at < p->end) {
    char c = *p->at;

    if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
      break;
    }

    p->at++;
  }
}

static Json *fail(Parser *p, const char *why) {
  if (p->error == nullptr) {
    p->error = why;
  }

  return nullptr;
}

static int hex4(const char *s) {
  int value = 0;

  for (int i = 0; i < 4; i++) {
    char c = s[i];
    int digit;

    if (c >= '0' && c <= '9') {
      digit = c - '0';
    }
    else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    }
    else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    }
    else {
      return -1;
    }

    value = value * 16 + digit;
  }

  return value;
}

static size_t put_utf8(char *out, uint32_t cp) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }

  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }

  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }

  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

// the string starting after the quote at p->at. unescaping never grows it, so
// the copy is as long as what is left between the quotes.
static const char *parse_string(Parser *p, uint32_t *len) {
  const char *close = p->at;

  while (close < p->end && *close != '"') {
    if (*close == '\\') {
      close++;
    }

    close++;
  }

  if (close >= p->end) {
    fail(p, "unterminated string");
    return nullptr;
  }

  char *out = cilo_arena_alloc(p->arena, (size_t)(close - p->at) + 1, 1);
  size_t n = 0;
  const char *s = p->at;

  while (s < close) {
    char c = *s++;

    if (c != '\\') {
      out[n++] = c;
      continue;
    }

    char e = *s++;

    switch (e) {
      case 'n':
        out[n++] = '\n';
        break;
      case 't':
        out[n++] = '\t';
        break;
      case 'r':
        out[n++] = '\r';
        break;
      case 'b':
        out[n++] = '\b';
        break;
      case 'f':
        out[n++] = '\f';
        break;
      case 'u': {
        if (close - s < 4) {
          fail(p, "short unicode escape");
          return nullptr;
        }

        int hi = hex4(s);

        if (hi < 0) {
          fail(p, "bad unicode escape");
          return nullptr;
        }

        s += 4;
        uint32_t cp = (uint32_t)hi;

        // a surrogate pair is one code point. a lone half reads as u+fffd.
        if (cp >= 0xd800 && cp < 0xdc00) {
          int lo = -1;

          if (close - s >= 6 && s[0] == '\\' && s[1] == 'u') {
            lo = hex4(s + 2);
          }

          if (lo >= 0xdc00 && lo < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint32_t)lo - 0xdc00);
            s += 6;
          }
          else {
            cp = 0xfffd;
          }
        }
        else if (cp >= 0xdc00 && cp < 0xe000) {
          cp = 0xfffd;
        }

        n += put_utf8(out + n, cp);
        break;
      }
      default:
        out[n++] = e;
        break;
    }
  }

  out[n] = '\0';
  *len = (uint32_t)n;
  p->at = close + 1;
  return out;
}

// whether the bytes spell a json number: a sign, digits, a fraction and an
// exponent, the last three optional.
static bool number_ok(const char *s, size_t len) {
  size_t i = 0;

  if (i < len && s[i] == '-') {
    i++;
  }

  size_t digits = i;

  while (i < len && s[i] >= '0' && s[i] <= '9') {
    i++;
  }

  if (i == digits) {
    return false;
  }

  if (i < len && s[i] == '.') {
    size_t fraction = ++i;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
      i++;
    }

    if (i == fraction) {
      return false;
    }
  }

  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    i++;

    if (i < len && (s[i] == '+' || s[i] == '-')) {
      i++;
    }

    size_t exponent = i;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
      i++;
    }

    if (i == exponent) {
      return false;
    }
  }

  return i == len;
}

static Json *parse_number(Parser *p) {
  const char *start = p->at;

  while (p->at < p->end) {
    char c = *p->at;
    bool part = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                c == 'e' || c == 'E';

    if (!part) {
      break;
    }

    p->at++;
  }

  size_t len = (size_t)(p->at - start);

  if (!number_ok(start, len)) {
    return fail(p, len == 0 ? "unexpected character" : "malformed number");
  }

  return CILO_NEW(p->arena, Json, .type = JSON_NUMBER, .len = (uint32_t)len,
                  .number =
                    cilo_strndup((cilo_allocator *)p->arena, start, len));
}

static bool take(Parser *p, const char *word) {
  size_t n = strlen(word);

  if ((size_t)(p->end - p->at) < n || memcmp(p->at, word, n) != 0) {
    return false;
  }

  p->at += n;
  return true;
}

static Json *parse_container(Parser *p, bool object) {
  if (p->depth >= JSON_MAX_DEPTH) {
    return fail(p, "nested too deep");
  }

  p->depth++;
  p->at++;

  Json *v = CILO_NEW(p->arena, Json, .type = object ? JSON_OBJECT : JSON_ARRAY);
  Json *last = nullptr;
  char close = object ? '}' : ']';

  skip_space(p);

  if (p->at < p->end && *p->at == close) {
    p->at++;
    p->depth--;
    return v;
  }

  for (;;) {
    const char *key = nullptr;

    skip_space(p);

    if (object) {
      if (p->at >= p->end || *p->at != '"') {
        return fail(p, "expected a key");
      }

      p->at++;
      uint32_t key_len;
      key = parse_string(p, &key_len);

      if (key == nullptr) {
        return nullptr;
      }

      skip_space(p);

      if (p->at >= p->end || *p->at != ':') {
        return fail(p, "expected a colon");
      }

      p->at++;
    }

    Json *item = parse_value(p);

    if (item == nullptr) {
      return nullptr;
    }

    item->key = key;

    if (last != nullptr) {
      last->next = item;
    }
    else {
      v->first = item;
    }

    last = item;
    v->len++;

    skip_space(p);

    if (p->at >= p->end) {
      return fail(p, "unterminated container");
    }

    char c = *p->at++;

    if (c == close) {
      break;
    }

    if (c != ',') {
      return fail(p, "expected a comma");
    }
  }

  p->depth--;
  return v;
}

static Json *parse_value(Parser *p) {
  skip_space(p);

  if (p->at >= p->end) {
    return fail(p, "unexpected end");
  }

  switch (*p->at) {
    case '{':
      return parse_container(p, true);
    case '[':
      return parse_container(p, false);
    case '"': {
      p->at++;
      uint32_t len;
      const char *s = parse_string(p, &len);

      if (s == nullptr) {
        return nullptr;
      }

      return CILO_NEW(p->arena, Json, .type = JSON_STRING, .len = len,
                      .string = s);
    }
    case 't':
      if (take(p, "true")) {
        return CILO_NEW(p->arena, Json, .type = JSON_BOOL, .boolean = true);
      }

      return fail(p, "unexpected character");
    case 'f':
      if (take(p, "false")) {
        return CILO_NEW(p->arena, Json, .type = JSON_BOOL);
      }

      return fail(p, "unexpected character");
    case 'n':
      if (take(p, "null")) {
        return CILO_NEW(p->arena, Json, .type = JSON_NULL);
      }

      return fail(p, "unexpected character");
    default:
      return parse_number(p);
  }
}

const Json *json_parse(cilo_arena *arena, const char *text, size_t len,
                       const char **error) {
  Parser p = {.arena = arena, .at = text, .end = text + len};
  Json *v = text != nullptr ? parse_value(&p) : nullptr;

  if (v != nullptr) {
    skip_space(&p);

    if (p.at != p.end) {
      v = fail(&p, "trailing characters");
    }
  }

  if (error != nullptr) {
    *error = v != nullptr ? nullptr : p.error != nullptr ? p.error : "no text";
  }

  return v;
}

const Json *json_get(const Json *v, const char *key) {
  if (v == nullptr || v->type != JSON_OBJECT) {
    return nullptr;
  }

  for (const Json *m = v->first; m != nullptr; m = m->next) {
    if (strcmp(m->key, key) == 0) {
      return m;
    }
  }

  return nullptr;
}

const Json *json_at(const Json *v, size_t index) {
  const Json *item = json_first(v);

  while (item != nullptr && index > 0) {
    item = item->next;
    index--;
  }

  return item;
}

size_t json_len(const Json *v) {
  if (v == nullptr || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) {
    return 0;
  }

  return v->len;
}

const char *json_str(const Json *v, const char *fallback) {
  return json_is(v, JSON_STRING) ? v->string : fallback;
}

static const char *digits_of(const Json *v) {
  if (v == nullptr) {
    return nullptr;
  }

  if (v->type == JSON_NUMBER) {
    return v->number;
  }

  if (v->type == JSON_STRING && v->len > 0) {
    return v->string;
  }

  return nullptr;
}

uint64_t json_u64(const Json *v) {
  const char *s = digits_of(v);

  if (s == nullptr || *s == '-') {
    return 0;
  }

  char *end;
  errno = 0;
  uint64_t n = strtoull(s, &end, 10);

  if (errno != 0 || end == s) {
    return 0;
  }

  // a number with a fraction or an exponent goes through a double.
  if (*end == '.' || *end == 'e' || *end == 'E') {
    double d = strtod(s, nullptr);
    return d > 0 && d < 18446744073709551616.0 ? (uint64_t)d : 0;
  }

  return *end == '\0' ? n : 0;
}

int64_t json_i64(const Json *v, int64_t fallback) {
  const char *s = digits_of(v);

  if (s == nullptr) {
    return fallback;
  }

  char *end;
  errno = 0;
  int64_t n = strtoll(s, &end, 10);

  if (errno != 0 || end == s) {
    return fallback;
  }

  if (*end == '.' || *end == 'e' || *end == 'E') {
    double d = strtod(s, nullptr);
    bool fits = d > -9223372036854775808.0 && d < 9223372036854775808.0;
    return fits ? (int64_t)d : fallback;
  }

  return *end == '\0' ? n : fallback;
}

double json_f64(const Json *v, double fallback) {
  const char *s = digits_of(v);

  if (s == nullptr) {
    return fallback;
  }

  char *end;
  double d = strtod(s, &end);
  return end != s && *end == '\0' && isfinite(d) ? d : fallback;
}

bool json_bool(const Json *v, bool fallback) {
  if (v == nullptr) {
    return fallback;
  }

  if (v->type == JSON_BOOL) {
    return v->boolean;
  }

  if (v->type == JSON_NUMBER) {
    return json_f64(v, 0) != 0;
  }

  return fallback;
}

const char *json_get_str(const Json *v, const char *key) {
  return json_str(json_get(v, key), nullptr);
}

uint64_t json_get_u64(const Json *v, const char *key) {
  return json_u64(json_get(v, key));
}

int64_t json_get_i64(const Json *v, const char *key, int64_t fallback) {
  return json_i64(json_get(v, key), fallback);
}

double json_get_f64(const Json *v, const char *key, double fallback) {
  return json_f64(json_get(v, key), fallback);
}

bool json_get_bool(const Json *v, const char *key, bool fallback) {
  return json_bool(json_get(v, key), fallback);
}

static void reserve(JsonWriter *w, size_t more) {
  if (w->len + more + 1 <= w->cap) {
    return;
  }

  size_t cap = w->cap != 0 ? w->cap * 2 : 256;

  while (cap < w->len + more + 1) {
    cap *= 2;
  }

  char *data = realloc(w->data, cap);

  if (data == nullptr) {
    cilo_oom(cap);
  }

  w->data = data;
  w->cap = cap;
}

static void put(JsonWriter *w, const char *s, size_t n) {
  reserve(w, n);
  memcpy(w->data + w->len, s, n);
  w->len += n;
  w->data[w->len] = '\0';
}

// what comes before any value: a comma unless it follows a key or opens its
// container.
static void before_value(JsonWriter *w) {
  if (w->after_key) {
    w->after_key = false;
    return;
  }

  if (w->depth == 0) {
    return;
  }

  uint64_t bit = 1ull << ((w->depth - 1) & 63);

  if (w->filled & bit) {
    put(w, ",", 1);
  }

  w->filled |= bit;
}

static void open(JsonWriter *w, bool object) {
  before_value(w);
  put(w, object ? "{" : "[", 1);
  w->depth++;

  uint64_t bit = 1ull << ((w->depth - 1) & 63);
  w->filled &= ~bit;
  w->objects = object ? w->objects | bit : w->objects & ~bit;
}

void jw_object(JsonWriter *w) {
  open(w, true);
}

void jw_array(JsonWriter *w) {
  open(w, false);
}

void jw_end(JsonWriter *w) {
  if (w->depth == 0) {
    return;
  }

  uint64_t bit = 1ull << ((w->depth - 1) & 63);
  put(w, w->objects & bit ? "}" : "]", 1);
  w->depth--;
  w->after_key = false;
}

void jw_strn(JsonWriter *w, const char *s, size_t len) {
  before_value(w);
  put(w, "\"", 1);

  size_t run = 0;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    const char *escape = nullptr;
    char unicode[8];

    switch (c) {
      case '"':
        escape = "\\\"";
        break;
      case '\\':
        escape = "\\\\";
        break;
      case '\n':
        escape = "\\n";
        break;
      case '\r':
        escape = "\\r";
        break;
      case '\t':
        escape = "\\t";
        break;
      default:
        if (c < 0x20) {
          snprintf(unicode, sizeof unicode, "\\u%04x", c);
          escape = unicode;
        }

        break;
    }

    if (escape == nullptr) {
      continue;
    }

    put(w, s + run, i - run);
    put(w, escape, strlen(escape));
    run = i + 1;
  }

  put(w, s + run, len - run);
  put(w, "\"", 1);
}

void jw_str(JsonWriter *w, const char *s) {
  if (s == nullptr) {
    jw_null(w);
    return;
  }

  jw_strn(w, s, strlen(s));
}

void jw_key(JsonWriter *w, const char *key) {
  jw_strn(w, key, strlen(key));
  put(w, ":", 1);
  w->after_key = true;
}

void jw_raw(JsonWriter *w, const char *json) {
  before_value(w);
  put(w, json, strlen(json));
}

void jw_i64(JsonWriter *w, int64_t n) {
  char text[32];
  snprintf(text, sizeof text, "%" PRId64, n);
  jw_raw(w, text);
}

void jw_u64(JsonWriter *w, uint64_t n) {
  char text[32];
  snprintf(text, sizeof text, "%" PRIu64, n);
  jw_raw(w, text);
}

void jw_id(JsonWriter *w, uint64_t id) {
  char text[32];
  snprintf(text, sizeof text, "\"%" PRIu64 "\"", id);
  jw_raw(w, text);
}

void jw_f64(JsonWriter *w, double n) {
  if (!isfinite(n)) {
    jw_null(w);
    return;
  }

  char text[40];
  snprintf(text, sizeof text, "%.17g", n);
  jw_raw(w, text);
}

void jw_bool(JsonWriter *w, bool b) {
  jw_raw(w, b ? "true" : "false");
}

void jw_null(JsonWriter *w) {
  jw_raw(w, "null");
}

char *jw_take(JsonWriter *w) {
  reserve(w, 0);
  w->data[w->len] = '\0';

  char *data = w->data;
  *w = (JsonWriter){};
  return data;
}

void jw_free(JsonWriter *w) {
  free(w->data);
  *w = (JsonWriter){};
}
