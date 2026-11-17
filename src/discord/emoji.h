#ifndef QUARREL_DISCORD_EMOJI_H
#define QUARREL_DISCORD_EMOJI_H

#include <stddef.h>
#include <stdint.h>

// an emoji of unicode as discord's client knows it.
typedef struct {
  const char *utf8;
  // the first of its names, which is how the user's settings know it.
  const char *name;
  // the twemoji svg discord draws it with is /assets/<asset in hex>.svg.
  uint64_t asset;
  // where it comes in discord's picker.
  uint16_t order;
  // a skin tone of another one.
  bool toned;
} UnicodeEmoji;

typedef struct {
  const char *name;
  uint16_t index;
} UnicodeEmojiName;

// sorted by their utf-8, and every name any of them has, sorted too.
extern const UnicodeEmoji UNICODE_EMOJIS[];
extern const size_t UNICODE_EMOJIS_COUNT;
extern const UnicodeEmojiName UNICODE_EMOJI_NAMES[];
extern const size_t UNICODE_EMOJI_NAMES_COUNT;

constexpr size_t EMOJI_URL_MAX = 64;

// null when discord has no such emoji.
const UnicodeEmoji *emoji_by_name(const char *name);
// `utf8` with or without the selector that asks for the colored glyph.
const UnicodeEmoji *emoji_by_utf8(const char *utf8);
void emoji_url(char out[static EMOJI_URL_MAX], const UnicodeEmoji *emoji);

#endif
