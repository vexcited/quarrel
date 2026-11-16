#ifndef QUARREL_JSON_H
#define QUARREL_JSON_H

#include <stddef.h>
#include <stdint.h>

#include <cilo/cilo.h>

/* ========================================================================== */
/* reading. a document is parsed into an arena and freed with it. every       */
/* getter takes a null value, a missing key or a value of another type and    */
/* answers its fallback, so chains never need a check.                        */
/*                                                                            */
/*   cilo_arena arena = {};                                                   */
/*   const Json *doc = json_parse(&arena, text, len, nullptr);                */
/*   uint64_t id = json_get_u64(json_get(doc, "author"), "id");               */
/*   json_each(item, json_get(doc, "mentions")) { ... }                       */
/*   cilo_arena_drop(&arena);                                                 */
/* ========================================================================== */

typedef enum : uint8_t {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
} JsonType;

typedef struct Json Json;

struct Json {
  JsonType type;
  bool boolean;
  // the bytes of a string, or the items of an array or an object.
  uint32_t len;
  // the name of a member of an object, null elsewhere.
  const char *key;
  // the next item of the array or the object holding this value.
  const Json *next;

  union {
    // a string, unescaped and nul terminated. it may hold nul bytes itself.
    const char *string;
    // a number as it was written, nul terminated.
    const char *number;
    const Json *first;
  };
};

// null when `text` is not one json value. `error` then says why when given.
const Json *json_parse(cilo_arena *arena, const char *text, size_t len,
                       const char **error);

// the member `key` of an object, null when missing or when `v` is no object.
const Json *json_get(const Json *v, const char *key);
// the item `index` of an array, null past its end.
const Json *json_at(const Json *v, size_t index);
// the items of an array or the members of an object, 0 for anything else.
size_t json_len(const Json *v);

#define json_each(item, v) \
  for (const Json *item = json_first(v); item != nullptr; item = item->next)

static inline const Json *json_first(const Json *v) {
  if (v == nullptr || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) {
    return nullptr;
  }

  return v->first;
}

static inline bool json_is(const Json *v, JsonType type) {
  return v != nullptr && v->type == type;
}

// true for a missing value too.
static inline bool json_is_null(const Json *v) {
  return v == nullptr || v->type == JSON_NULL;
}

// a string's bytes, or `fallback` for anything else.
const char *json_str(const Json *v, const char *fallback);
// a number, or a string holding one as discord writes snowflakes and
// permissions. 0 for anything else.
uint64_t json_u64(const Json *v);
int64_t json_i64(const Json *v, int64_t fallback);
double json_f64(const Json *v, double fallback);
// true and false, and numbers as c reads them.
bool json_bool(const Json *v, bool fallback);

// the same over a member of `v`. json_get_str answers null when the member
// is missing, null or no string.
const char *json_get_str(const Json *v, const char *key);
uint64_t json_get_u64(const Json *v, const char *key);
int64_t json_get_i64(const Json *v, const char *key, int64_t fallback);
double json_get_f64(const Json *v, const char *key, double fallback);
bool json_get_bool(const Json *v, const char *key, bool fallback);

/* ========================================================================== */
/* writing. commas and quotes are the writer's business.                      */
/*                                                                            */
/*   JsonWriter w = {};                                                       */
/*   jw_object(&w);                                                           */
/*   jw_key(&w, "content"), jw_str(&w, text);                                 */
/*   jw_end(&w);                                                              */
/*   send(w.data, w.len);                                                     */
/*   jw_free(&w);                                                             */
/* ========================================================================== */

typedef struct {
  // nul terminated once anything was written.
  char *data;
  size_t len, cap;
  // one bit per open array or object, set once it holds a value.
  uint64_t filled;
  // one bit per open array or object, set for an object.
  uint64_t objects;
  uint32_t depth;
  bool after_key;
} JsonWriter;

void jw_object(JsonWriter *w);
void jw_array(JsonWriter *w);
// closes the innermost array or object.
void jw_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
// null writes a json null.
void jw_str(JsonWriter *w, const char *s);
void jw_strn(JsonWriter *w, const char *s, size_t len);
void jw_i64(JsonWriter *w, int64_t n);
void jw_u64(JsonWriter *w, uint64_t n);
// a snowflake, written as a string like discord does.
void jw_id(JsonWriter *w, uint64_t id);
void jw_f64(JsonWriter *w, double n);
void jw_bool(JsonWriter *w, bool b);
void jw_null(JsonWriter *w);
// json written elsewhere, as it is.
void jw_raw(JsonWriter *w, const char *json);
// gives the text to the caller, who frees it. the writer is empty again.
char *jw_take(JsonWriter *w);
void jw_free(JsonWriter *w);

#endif
