#ifndef QUARREL_DISCORD_SETTINGS_PROTO_H
#define QUARREL_DISCORD_SETTINGS_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "discord/util.h"

// turns a PreloadedUserSettings protobuf, in base64 as the gateway sends
// it, into the json `user_settings` used to be, with the fields Quarrel
// reads: `status`, `guild_folders`, `guild_positions` and
// `animate_stickers`, a StickerAnimation. a field the
// message does not carry is left out, as in a partial update. freed with
// cilo_mfree, null when `base64` is no such message.
// the status comes with `status_expires_at_ms`, `custom_status` as an
// object of `text`, `emoji_id`, `emoji_name` and `expires_at_ms` or null,
// and `status_proto`, the StatusSettings itself in base64.
char *settings_proto_to_json(const char *base64);

// the PreloadedUserSettings to save for a new status, in base64 from
// malloc. `section` is the `status_proto` last read or null: discord
// replaces the whole StatusSettings, so everything else it holds is
// written back. `expires_ms` is unix milliseconds, 0 for never.
char *settings_proto_with_status(const char *section, const char *status,
                                 int64_t expires_ms, int64_t now_ms);

constexpr size_t FRECENCY_SAMPLES = 10;

// how much something was used, as the client tracks it.
typedef struct {
  // an emoji's first name, or its id for one of a guild.
  char *key;
  uint32_t total_uses;
  // unix milliseconds of the last uses.
  int64_t recent_uses[FRECENCY_SAMPLES];
  size_t nrecent_uses;
} FrecencyUse;

// the emojis reacted with of a FrecencyUserSettings protobuf in base64.
// from malloc with its keys, null when it has none.
FrecencyUse *settings_proto_reaction_uses(const char *base64, size_t *count);
// how much a sticker was used.
typedef struct {
  Snowflake id;
  uint32_t total_uses;
  // unix milliseconds of the last uses, the latest last.
  int64_t recent_uses[FRECENCY_SAMPLES];
  size_t nrecent_uses;
} StickerUse;

// the stickers of a FrecencyUserSettings: how much each was used, in no
// order, and the ones starred, in the order they were.
typedef struct {
  StickerUse *uses;
  size_t nuses;
  Snowflake *favorites;
  size_t nfavorites;
  // the message carries each, the only time they replace the ones known.
  bool has_uses, has_favorites;
} StickerSettings;

// from malloc, freed with settings_proto_free_stickers.
StickerSettings settings_proto_stickers(const char *base64);
void settings_proto_free_stickers(StickerSettings *stickers);

// how much a use counts for, from how many days ago it was. the client's.
[[gnu::const]] uint32_t frecency_weight(int64_t days);
// the weights of a sticker's last uses together, which the client's search
// favors it by. 0 for one it would forget.
[[gnu::pure]] uint64_t settings_proto_sticker_score(const StickerUse *use,
                                                    int64_t now_ms);
// what the client ranks a sticker by at `now_ms`: its uses times the mean
// weight of the last ones, rounded up. 0 for one it would forget.
[[gnu::pure]] uint64_t settings_proto_sticker_frecency(const StickerUse *use,
                                                       int64_t now_ms);

// a FrecencyUserSettings holding only the starred stickers, or only how
// much each was used, the hundred used last as the client keeps, in base64
// from malloc. each replaces all of its kind once saved.
char *settings_proto_with_favorite_stickers(const Snowflake *ids, size_t count);
char *settings_proto_with_sticker_uses(const StickerUse *uses, size_t count,
                                       int64_t now_ms);

typedef enum : uint8_t {
  GIF_FORMAT_NONE = 0,
  // an image the system shows itself, a gif or a webp.
  GIF_FORMAT_IMAGE = 1,
  // an mp4 or a webm.
  GIF_FORMAT_VIDEO = 2,
} GifFormat;

// a gif the user starred, as the client keeps it.
typedef struct {
  // what the message sends, the key of the favorite.
  char *url;
  // what the picker shows.
  char *src;
  GifFormat format;
  uint32_t width, height;
  // the newest has the highest.
  uint32_t order;
} FavoriteGif;

typedef struct {
  FavoriteGif *gifs;
  size_t count;
  // the client stops explaining the star once three were starred.
  bool hide_tooltip;
  // the message carries favorites, the only time they replace the ones known.
  bool present;
} FavoriteGifs;

// the favorite gifs of a FrecencyUserSettings protobuf in base64, its
// strings from malloc. freed with settings_proto_free_favorites.
FavoriteGifs settings_proto_favorite_gifs(const char *base64);
void settings_proto_free_favorites(FavoriteGifs *favorites);

// a FrecencyUserSettings holding only these favorites in base64 from malloc,
// which replace all of the user's once saved. null when they outgrow what
// the client allows.
char *settings_proto_with_favorite_gifs(const FavoriteGifs *favorites);

#endif
