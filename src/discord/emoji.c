#include "discord/emoji.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void emoji_url(char out[static EMOJI_URL_MAX], const UnicodeEmoji *emoji) {
  snprintf(out, EMOJI_URL_MAX, "https://discord.com/assets/%016" PRIx64 ".svg",
           emoji->asset);
}
