#ifndef QUARREL_DISCORD_CDN_H
#define QUARREL_DISCORD_CDN_H

#include "discord/util.h"

/* ========================================================================== */
/* urls of discord's cdn. each builder writes into `out`, nul terminated, and */
/* returns `out`, or "" when there is nothing to show: a null hash, a null    */
/* url. `size` is the side in pixels the image will be drawn at, rounded up   */
/* to a size the cdn serves, 0 for the cdn's default.                         */
/*                                                                            */
/*   char url[CDN_URL_MAX];                                                   */
/*   cdn_avatar(url, user->id, user->avatar, user->discriminator, 80);        */
/*   civue_image_info a = civue_images_get(im, ui, url, 80);                  */
/* ========================================================================== */

constexpr size_t CDN_URL_MAX = 1024;

// a size the cdn serves that is at least `pixels`, between 16 and 4096.
[[gnu::const]] uint32_t cdn_size(uint32_t pixels);

// the index of the avatar drawn for a user without one. users with a unique
// username have a discriminator of "0", or none.
[[gnu::pure]] uint32_t cdn_default_avatar_index(Snowflake user_id,
                                                const char *discriminator);

// the user's avatar, or the default one when `hash` is null. always an url.
char *cdn_avatar(char out[static CDN_URL_MAX], Snowflake user_id,
                 const char *hash, const char *discriminator, uint32_t size);
char *cdn_default_avatar(char out[static CDN_URL_MAX], Snowflake user_id,
                         const char *discriminator);
// the avatar a member set for one guild.
char *cdn_member_avatar(char out[static CDN_URL_MAX], Snowflake guild_id,
                        Snowflake user_id, const char *hash, uint32_t size);
// the banner a member set for a guild.
char *cdn_member_banner(char out[static CDN_URL_MAX], Snowflake guild_id,
                        Snowflake user_id, const char *hash, uint32_t size);
char *cdn_guild_icon(char out[static CDN_URL_MAX], Snowflake guild_id,
                     const char *hash, uint32_t size);
// a group dm's icon.
char *cdn_channel_icon(char out[static CDN_URL_MAX], Snowflake channel_id,
                       const char *hash, uint32_t size);
// a user's or a guild's banner, both under the same path.
char *cdn_banner(char out[static CDN_URL_MAX], Snowflake owner_id,
                 const char *hash, uint32_t size);
char *cdn_guild_splash(char out[static CDN_URL_MAX], Snowflake guild_id,
                       const char *hash, uint32_t size);
char *cdn_role_icon(char out[static CDN_URL_MAX], Snowflake role_id,
                    const char *hash, uint32_t size);
// a custom emoji as webp, its first frame when animated.
char *cdn_emoji(char out[static CDN_URL_MAX], Snowflake emoji_id,
                uint32_t size);

// the badge of a guild's tag.
char *cdn_tag_badge(char out[static CDN_URL_MAX], Snowflake guild_id,
                    const char *hash, uint32_t size);
// what frames an avatar, still: `asset` is the user's `decoration`.
char *cdn_avatar_decoration(char out[static CDN_URL_MAX], const char *asset,
                            uint32_t size);
// a badge of a profile.
char *cdn_badge_icon(char out[static CDN_URL_MAX], const char *hash);
char *cdn_application_icon(char out[static CDN_URL_MAX],
                           Snowflake application_id, const char *hash,
                           uint32_t size);
// an activity's `assets.large_image`: an asset of the application, or a
// "mp:" path the media proxy serves.
char *cdn_activity_image(char out[static CDN_URL_MAX], Snowflake application_id,
                         const char *image, uint32_t size);

typedef enum : uint8_t {
  STICKER_PNG = 1,
  STICKER_APNG = 2,
  STICKER_LOTTIE = 3,
  STICKER_GIF = 4,
} StickerFormat;

// a sticker as an image, every frame of one that moves when `animated`,
// else its first. "" for lottie ones, which are no image.
char *cdn_sticker(char out[static CDN_URL_MAX], Snowflake sticker_id,
                  StickerFormat format, uint32_t size, bool animated);

// an attachment's or an embed's `proxy_url` resized by the media proxy to fit
// `width` by `height` pixels, as webp. 0 for both keeps its size.
char *cdn_media(char out[static CDN_URL_MAX], const char *proxy_url,
                uint32_t width, uint32_t height);
// the same keeping every frame of an animated gif, webp or png.
char *cdn_media_animated(char out[static CDN_URL_MAX], const char *proxy_url,
                         uint32_t width, uint32_t height);

#endif
