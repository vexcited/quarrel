#ifndef QUARREL_DISCORD_EMOJI_H
#define QUARREL_DISCORD_EMOJI_H

#include <stddef.h>
#include <stdint.h>

// an emoji of unicode as discord's client knows it.
typedef struct {
  const char *utf8;
  // the first of its names, which is how the user's settings know it.
  const char *name;
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

// twemoji's svgs, sorted by their names: the codepoints in hex between
// dashes, and where each is in all of them one after the other, which the
// binary holds as one zstd frame. made by the build.
typedef struct {
  const char *name;
  uint32_t at, len;
} Twemoji;

extern const Twemoji TWEMOJIS[];
extern const size_t TWEMOJIS_COUNT;
extern const unsigned char *const TWEMOJIS_PACKED;
extern const size_t TWEMOJIS_PACKED_LEN, TWEMOJIS_LEN;

constexpr size_t EMOJI_URL_MAX = 64;

// the picker's categories, and where each starts in its order: what the
// client's emojisByCategory holds.
constexpr size_t EMOJI_CATEGORIES = 8;
constexpr uint16_t EMOJI_CATEGORY_STARTS[EMOJI_CATEGORIES + 1] = {
  0, 511, 725, 855, 977, 1108, 1334, 1662, 1932,
};

// null when discord has no such emoji.
[[gnu::pure]] const UnicodeEmoji *emoji_by_name(const char *name);
// the one the `len` bytes of a name between colons are, in a skin tone from
// 1 to 5 when `tone` and it has them. null when discord has no such emoji.
[[gnu::pure]] const UnicodeEmoji *emoji_by_colons(const char *name, size_t len,
                                                  uint8_t tone);
// those of the picker in its order, the skin tones of another left out.
const UnicodeEmoji *const *emoji_picker_order(size_t *count);
// `utf8` with or without the selector that asks for the colored glyph.
[[gnu::pure]] const UnicodeEmoji *emoji_by_utf8(const char *utf8);
// an url civue's images read with emoji_svg, never fetched.
void emoji_url(char out[static EMOJI_URL_MAX], const UnicodeEmoji *emoji);
// the svg of an url of emoji_url, null for another url. the first unpacks
// them all, held until exit.
const char *emoji_svg(const char *url, size_t *len);

#endif
