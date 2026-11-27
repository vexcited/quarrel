#include "discord/emoji.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

static int by_name(const void *key, const void *item) {
  return strcmp(key, ((const UnicodeEmojiName *)item)->name);
}

static int by_utf8(const void *key, const void *item) {
  return strcmp(key, ((const UnicodeEmoji *)item)->utf8);
}

const UnicodeEmoji *emoji_by_name(const char *name) {
  if (name == nullptr) {
    return nullptr;
  }

  const UnicodeEmojiName *found =
    bsearch(name, UNICODE_EMOJI_NAMES, UNICODE_EMOJI_NAMES_COUNT, sizeof *found,
            by_name);

  return found ? &UNICODE_EMOJIS[found->index] : nullptr;
}

const UnicodeEmoji *emoji_by_colons(const char *name, size_t len,
                                    uint8_t tone) {
  char key[80];

  if (len == 0 || len + 8 > sizeof key) {
    return nullptr;
  }

  snprintf(key, sizeof key, "%.*s", (int)len, name);

  const UnicodeEmoji *plain = emoji_by_name(key);

  if (plain == nullptr || tone == 0) {
    return plain;
  }

  snprintf(key, sizeof key, "%s_tone%u", plain->name, tone);

  const UnicodeEmoji *toned = emoji_by_name(key);

  return toned != nullptr ? toned : plain;
}

const UnicodeEmoji *const *emoji_picker_order(size_t *count) {
  constexpr size_t MOST = EMOJI_CATEGORY_STARTS[EMOJI_CATEGORIES];
  static const UnicodeEmoji *order[MOST];
  static bool made;

  // the table has one for every place under the last category's end.
  for (size_t i = 0; !made && i < UNICODE_EMOJIS_COUNT; i++) {
    const UnicodeEmoji *emoji = &UNICODE_EMOJIS[i];

    if (emoji->order < MOST && !emoji->toned) {
      order[emoji->order] = emoji;
    }
  }

  made = true;
  *count = MOST;
  return order;
}

static const UnicodeEmoji *find(const char *utf8) {
  return bsearch(utf8, UNICODE_EMOJIS, UNICODE_EMOJIS_COUNT,
                 sizeof *UNICODE_EMOJIS, by_utf8);
}

const UnicodeEmoji *emoji_by_utf8(const char *utf8) {
  static const char SELECTOR[] = "\xef\xb8\x8f";
  char other[64];
  size_t len = utf8 ? strlen(utf8) : 0;

  if (len == 0 || len + sizeof SELECTOR > sizeof other) {
    return nullptr;
  }

  const UnicodeEmoji *emoji = find(utf8);

  if (emoji != nullptr) {
    return emoji;
  }

  // discord's table and what a message holds do not always agree on it.
  if (len > 3 && !strcmp(utf8 + len - 3, SELECTOR)) {
    memcpy(other, utf8, len - 3);
    other[len - 3] = '\0';
  }
  else {
    snprintf(other, sizeof other, "%s%s", utf8, SELECTOR);
  }

  return find(other);
}

static int by_twemoji(const void *key, const void *item) {
  return strcmp(key, ((const Twemoji *)item)->name);
}

// twemoji's name for it: its codepoints, the selector that asks for the
// colored glyph left out unless a joiner is in it.
void emoji_url(char out[static EMOJI_URL_MAX], const UnicodeEmoji *emoji) {
  const unsigned char *at = (const unsigned char *)emoji->utf8;
  bool joined = strstr(emoji->utf8, "\xe2\x80\x8d") != nullptr;
  size_t len = (size_t)snprintf(out, EMOJI_URL_MAX, "twemoji:");

  while (*at != '\0') {
    uint32_t cp = *at;
    size_t more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;

    cp &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
    at++;

    for (; more > 0 && (*at & 0xc0) == 0x80; more--, at++) {
      cp = cp << 6 | (*at & 0x3f);
    }

    if ((cp == 0xfe0f && !joined) || len >= EMOJI_URL_MAX) {
      continue;
    }

    len += (size_t)snprintf(out + len, EMOJI_URL_MAX - len, "%s%" PRIx32,
                            len > 8 ? "-" : "", cp);
  }
}

// every svg, unpacked once. null when they would not unpack.
static const char *unpacked(void) {
  static char *all;
  static bool tried;

  if (!tried) {
    tried = true;
    all = malloc(TWEMOJIS_LEN);

    size_t got = all != nullptr
                   ? ZSTD_decompress(all, TWEMOJIS_LEN, TWEMOJIS_PACKED,
                                     TWEMOJIS_PACKED_LEN)
                   : 0;

    if (all != nullptr && (ZSTD_isError(got) || got != TWEMOJIS_LEN)) {
      free(all);
      all = nullptr;
    }
  }

  return all;
}

const char *emoji_svg(const char *url, size_t *len) {
  if (strncmp(url, "twemoji:", 8) != 0) {
    return nullptr;
  }

  const Twemoji *found =
    bsearch(url + 8, TWEMOJIS, TWEMOJIS_COUNT, sizeof *TWEMOJIS, by_twemoji);
  const char *all = found != nullptr ? unpacked() : nullptr;

  if (all == nullptr) {
    return nullptr;
  }

  *len = found->len;
  return all + found->at;
}
