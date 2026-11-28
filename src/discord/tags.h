#ifndef QUARREL_DISCORD_TAGS_H
#define QUARREL_DISCORD_TAGS_H

#include <stddef.h>
#include <stdint.h>

// the badges a guild's tag may wear, by the number discord gives them.
constexpr size_t TAG_BADGES = 41;
// what tag_badge_url writes at most.
constexpr size_t TAG_BADGE_URL_MAX = 40;
// a tint left out, drawn in the badge's own colors.
constexpr int32_t TAG_TINT_NONE = -1;

// a part of a badge drawn in a tint: its luminance is `x0` plus `w` times the
// tint's, as discord's client recolors it.
typedef struct {
  bool secondary;
  double x0, w;
} TagSlot;

typedef struct {
  const char *name, *label;
  // the svg, each tinted color marked with the index of its slot.
  const unsigned char *svg;
  size_t len;
  // the tints it takes, one or two.
  uint8_t colors;
  uint8_t nslots;
  TagSlot slots[8];
} TagBadge;

extern const TagBadge TAG_BADGE_ART[TAG_BADGES];

// the colors discord offers, a primary and a secondary each.
typedef struct {
  uint32_t primary, secondary;
} TagPalette;

constexpr size_t TAG_PALETTES = 13;
extern const TagPalette TAG_PALETTE[TAG_PALETTES];

// the badges every guild may pick, in discord's order. the first is the one
// a new tag wears.
constexpr size_t TAG_FREE_BADGES = 10;
extern const uint8_t TAG_FREE_BADGE[TAG_FREE_BADGES];

// the packs boosts unlock, five badges from `first` each, and the guild
// feature a pack unlocked shows.
typedef struct {
  const char *feature;
  uint8_t first;
} TagPack;

constexpr size_t TAG_PACKS = 4;
extern const TagPack TAG_PACK[TAG_PACKS];

// `tint` recolored as discord's client recolors a slot.
uint32_t tag_tint(const TagSlot *slot, uint32_t tint);

// an url civue's images read with tag_badge_svg, never fetched. a tint is a
// 0xrrggbb color or TAG_TINT_NONE.
void tag_badge_url(char out[static TAG_BADGE_URL_MAX], uint8_t badge,
                   int32_t primary, int32_t secondary);
// the svg of an url of tag_badge_url, null for another url. it lasts until
// the next call.
const char *tag_badge_svg(const char *url, size_t *len);

#endif
