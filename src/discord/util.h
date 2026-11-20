#ifndef QUARREL_DISCORD_UTIL_H
#define QUARREL_DISCORD_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include <cilo/map.h>
#include <cima/json.h>

/* ========================================================================== */
/* snowflakes. an id is 64 bits, its top 42 the milliseconds since the first  */
/* second of 2015, so ids sort by age.                                        */
/* ========================================================================== */

// read from a string of digits or a number, written as a string, through
// cima: Snowflake_list and Snowflake_maybe come along, and schemas can hold it.
extern const cima_codec snowflake_codec;
CIMA_CUSTOM(Snowflake, uint64_t, &snowflake_codec);

constexpr int64_t DISCORD_EPOCH_MS = 1420070400000;

// unix milliseconds of when `id` was made.
static inline int64_t snowflake_time_ms(Snowflake id) {
  return (int64_t)(id >> 22) + DISCORD_EPOCH_MS;
}

// the smallest id made at `unix_ms`, 0 before the epoch.
static inline Snowflake snowflake_from_time_ms(int64_t unix_ms) {
  if (unix_ms <= DISCORD_EPOCH_MS) {
    return 0;
  }

  return (Snowflake)(unix_ms - DISCORD_EPOCH_MS) << 22;
}

// 0 when `s` is null, empty or not a decimal number.
Snowflake snowflake_parse(const char *s);
// the same over the first `len` bytes of `s`.
Snowflake snowflake_parse_n(const char *s, size_t len);
// writes the decimal digits of `id` and returns `out`.
char *snowflake_format(Snowflake id, char out[static 21]);
// the id held by `v`, a string of digits as discord writes ids and
// permissions, or a number. 0 for anything else.
Snowflake snowflake_of(const cima_json *v);
// `id` as a string, the way discord reads them.
void snowflake_write(cima_json_writer *w, Snowflake id);

/* ========================================================================== */
/* time and text.                                                             */
/* ========================================================================== */

// unix milliseconds of an iso 8601 timestamp like discord writes them,
// "2024-05-01T12:30:00.123000+00:00". 0 when `s` is null or malformed.
int64_t iso8601_parse_ms(const char *s);

// frees `*dst` and points it at a copy of `src`, or at null for null.
void str_set(char **dst, const char *src);
// the same when `src` is not null, leaving `*dst` alone otherwise.
void str_update(char **dst, const char *src);
// a copy from malloc, null for null.
char *str_dup(const char *src);
char *str_dup_n(const char *src, size_t len);

// base64 with padding, from malloc.
char *base64_encode(const void *bytes, size_t len);
// the bytes of standard or url safe base64, padded or not, from malloc.
// null when `text` is null or holds anything else.
uint8_t *base64_decode(const char *text, size_t *len);
// percent encodes what an url's path or query cannot hold as it is.
char *url_encode(const char *s);

// libc's malloc as a cilo allocator, for maps that live as long as the store
// and that a leak checker should see.
extern cilo_allocator *const quarrel_libc;

#endif
