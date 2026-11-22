#ifndef QUARREL_DISCORD_SETTINGS_PROTO_H
#define QUARREL_DISCORD_SETTINGS_PROTO_H

#include <stddef.h>
#include <stdint.h>

// turns a PreloadedUserSettings protobuf, in base64 as the gateway sends
// it, into the json `user_settings` used to be, with the fields Quarrel
// reads: `status`, `guild_folders` and `guild_positions`. a field the
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
