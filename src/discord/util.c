#include "discord/util.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

Snowflake snowflake_parse_n(const char *s, size_t len) {
  if (s == nullptr || len == 0 || len > 20) {
    return 0;
  }

  Snowflake id = 0;

  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return 0;
    }

    Snowflake digit = (Snowflake)(s[i] - '0');

    if (ckd_mul(&id, id, (Snowflake)10) || ckd_add(&id, id, digit)) {
      return 0;
    }
  }

  return id;
}

Snowflake snowflake_parse(const char *s) {
  return s != nullptr ? snowflake_parse_n(s, strlen(s)) : 0;
}

char *snowflake_format(Snowflake id, char out[static 21]) {
  snprintf(out, 21, "%" PRIu64, id);
  return out;
}

static bool digits(const char *s, int count, int *value) {
  int n = 0;

  for (int i = 0; i < count; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }

    n = n * 10 + (s[i] - '0');
  }

  *value = n;
  return true;
}

int64_t iso8601_parse_ms(const char *s) {
  if (s == nullptr || strlen(s) < 19) {
    return 0;
  }

  int year, month, day, hour, minute, second;
  bool ok = digits(s, 4, &year) && s[4] == '-' && digits(s + 5, 2, &month) &&
            s[7] == '-' && digits(s + 8, 2, &day) &&
            (s[10] == 'T' || s[10] == 't' || s[10] == ' ') &&
            digits(s + 11, 2, &hour) && s[13] == ':' &&
            digits(s + 14, 2, &minute) && s[16] == ':' &&
            digits(s + 17, 2, &second);

  if (!ok || month < 1 || month > 12 || day < 1 || day > 31) {
    return 0;
  }

  const char *at = s + 19;
  int64_t millis = 0;

  if (*at == '.') {
    at++;
    int64_t scale = 100;

    while (*at >= '0' && *at <= '9') {
      millis += (*at - '0') * scale;
      scale /= 10;
      at++;
    }
  }

  int64_t offset = 0;

  if (*at == '+' || *at == '-') {
    int oh, om = 0;

    if (strlen(at) < 3 || !digits(at + 1, 2, &oh)) {
      return 0;
    }

    const char *minutes = at[3] == ':' ? at + 4 : at + 3;

    if (strlen(minutes) >= 2 && !digits(minutes, 2, &om)) {
      return 0;
    }

    offset = (oh * 60 + om) * 60;

    if (*at == '-') {
      offset = -offset;
    }
  }

  struct tm tm = {
    .tm_year = year - 1900,
    .tm_mon = month - 1,
    .tm_mday = day,
    .tm_hour = hour,
    .tm_min = minute,
    .tm_sec = second,
  };

  return ((int64_t)timegm(&tm) - offset) * 1000 + millis;
}

char *str_dup_n(const char *src, size_t len) {
  if (src == nullptr) {
    return nullptr;
  }

  char *copy = malloc(len + 1);

  if (copy == nullptr) {
    cilo_oom(len + 1);
  }

  memcpy(copy, src, len);
  copy[len] = '\0';
  return copy;
}

char *str_dup(const char *src) {
  return src != nullptr ? str_dup_n(src, strlen(src)) : nullptr;
}

void str_set(char **dst, const char *src) {
  if (*dst == src) {
    return;
  }

  if (*dst != nullptr && src != nullptr && strcmp(*dst, src) == 0) {
    return;
  }

  char *copy = str_dup(src);
  free(*dst);
  *dst = copy;
}

void str_update(char **dst, const char *src) {
  if (src != nullptr) {
    str_set(dst, src);
  }
}

char *base64_encode(const void *bytes, size_t len) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  const unsigned char *in = bytes;
  char *out = malloc((len + 2) / 3 * 4 + 1);

  if (out == nullptr) {
    cilo_oom(len);
  }

  size_t n = 0;

  for (size_t i = 0; i < len; i += 3) {
    uint32_t chunk = (uint32_t)in[i] << 16;

    if (i + 1 < len) {
      chunk |= (uint32_t)in[i + 1] << 8;
    }

    if (i + 2 < len) {
      chunk |= in[i + 2];
    }

    out[n++] = table[chunk >> 18];
    out[n++] = table[(chunk >> 12) & 63];
    out[n++] = i + 1 < len ? table[(chunk >> 6) & 63] : '=';
    out[n++] = i + 2 < len ? table[chunk & 63] : '=';
  }

  out[n] = '\0';
  return out;
}

static int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }

  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }

  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }

  if (c == '+' || c == '-') {
    return 62;
  }

  return c == '/' || c == '_' ? 63 : -1;
}

uint8_t *base64_decode(const char *text, size_t *len) {
  *len = 0;

  if (text == nullptr) {
    return nullptr;
  }

  size_t chars = strlen(text);

  while (chars > 0 && text[chars - 1] == '=') {
    chars--;
  }

  uint8_t *out = malloc(chars / 4 * 3 + 3);

  if (out == nullptr) {
    cilo_oom(chars);
  }

  uint32_t chunk = 0;
  int bits = 0;
  size_t n = 0;

  for (size_t i = 0; i < chars; i++) {
    int value = base64_value(text[i]);

    if (value < 0) {
      free(out);
      return nullptr;
    }

    chunk = chunk << 6 | (uint32_t)value;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      out[n++] = (uint8_t)(chunk >> bits);
    }
  }

  *len = n;
  return out;
}

char *url_encode(const char *s) {
  static const char hex[] = "0123456789ABCDEF";

  size_t len = s != nullptr ? strlen(s) : 0;
  char *out = malloc(len * 3 + 1);

  if (out == nullptr) {
    cilo_oom(len * 3 + 1);
  }

  size_t n = 0;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                 c == '~';

    if (plain) {
      out[n++] = (char)c;
    }
    else {
      out[n++] = '%';
      out[n++] = hex[c >> 4];
      out[n++] = hex[c & 15];
    }
  }

  out[n] = '\0';
  return out;
}

static void *libc_resize(cilo_allocator *, void *ptr, size_t, size_t new_size,
                         size_t align) {
  if (new_size == 0) {
    free(ptr);
    return nullptr;
  }

  if (align > 16) {
    return nullptr;
  }

  return realloc(ptr, new_size);
}

static const cilo_vtable libc_vtable = {.resize = libc_resize};
static cilo_allocator libc_allocator = &libc_vtable;

cilo_allocator *const quarrel_libc = &libc_allocator;
