#include "discord/cdn.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define CDN_BASE "https://cdn.discordapp.com"
#define MEDIA_BASE "https://media.discordapp.net"

uint32_t cdn_size(uint32_t pixels) {
  uint32_t size = 16;

  while (size < pixels && size < 4096) {
    size *= 2;
  }

  return size;
}

uint32_t cdn_default_avatar_index(Snowflake user_id,
                                  const char *discriminator) {
  Snowflake legacy = snowflake_parse(discriminator);

  if (legacy != 0) {
    return (uint32_t)(legacy % 5);
  }

  return (uint32_t)((user_id >> 22) % 6);
}

static char *empty(char out[static CDN_URL_MAX]) {
  out[0] = '\0';
  return out;
}

// a hash goes into a path as it is, so anything but what a hash is made of
// is refused.
static bool hash_ok(const char *hash) {
  if (hash == nullptr || hash[0] == '\0' || strlen(hash) > 128) {
    return false;
  }

  for (const char *c = hash; *c != '\0'; c++) {
    bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';

    if (!ok) {
      return false;
    }
  }

  return true;
}

static char *sized(char out[static CDN_URL_MAX], size_t len, uint32_t size) {
  if (size != 0 && len < CDN_URL_MAX) {
    snprintf(out + len, CDN_URL_MAX - len, "?size=%u", cdn_size(size));
  }

  return out;
}

static char *hashed(char out[static CDN_URL_MAX], const char *folder,
                    Snowflake id, const char *hash, uint32_t size) {
  if (!hash_ok(hash)) {
    return empty(out);
  }

  int len = snprintf(out, CDN_URL_MAX, CDN_BASE "/%s/%" PRIu64 "/%s.webp",
                     folder, id, hash);
  return sized(out, (size_t)len, size);
}

char *cdn_default_avatar(char out[static CDN_URL_MAX], Snowflake user_id,
                         const char *discriminator) {
  snprintf(out, CDN_URL_MAX, CDN_BASE "/embed/avatars/%u.png",
           cdn_default_avatar_index(user_id, discriminator));
  return out;
}

char *cdn_avatar(char out[static CDN_URL_MAX], Snowflake user_id,
                 const char *hash, const char *discriminator, uint32_t size) {
  if (!hash_ok(hash)) {
    return cdn_default_avatar(out, user_id, discriminator);
  }

  return hashed(out, "avatars", user_id, hash, size);
}

char *cdn_member_avatar(char out[static CDN_URL_MAX], Snowflake guild_id,
                        Snowflake user_id, const char *hash, uint32_t size) {
  if (!hash_ok(hash)) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX,
             CDN_BASE "/guilds/%" PRIu64 "/users/%" PRIu64 "/avatars/%s.webp",
             guild_id, user_id, hash);
  return sized(out, (size_t)len, size);
}

char *cdn_member_banner(char out[static CDN_URL_MAX], Snowflake guild_id,
                        Snowflake user_id, const char *hash, uint32_t size) {
  if (!hash_ok(hash)) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX,
             CDN_BASE "/guilds/%" PRIu64 "/users/%" PRIu64 "/banners/%s.webp",
             guild_id, user_id, hash);
  return sized(out, (size_t)len, size);
}

char *cdn_guild_icon(char out[static CDN_URL_MAX], Snowflake guild_id,
                     const char *hash, uint32_t size) {
  return hashed(out, "icons", guild_id, hash, size);
}

char *cdn_channel_icon(char out[static CDN_URL_MAX], Snowflake channel_id,
                       const char *hash, uint32_t size) {
  return hashed(out, "channel-icons", channel_id, hash, size);
}

char *cdn_banner(char out[static CDN_URL_MAX], Snowflake owner_id,
                 const char *hash, uint32_t size) {
  return hashed(out, "banners", owner_id, hash, size);
}

char *cdn_guild_splash(char out[static CDN_URL_MAX], Snowflake guild_id,
                       const char *hash, uint32_t size) {
  return hashed(out, "splashes", guild_id, hash, size);
}

char *cdn_discovery_splash(char out[static CDN_URL_MAX], Snowflake guild_id,
                           const char *hash, uint32_t size) {
  return hashed(out, "discovery-splashes", guild_id, hash, size);
}

char *cdn_role_icon(char out[static CDN_URL_MAX], Snowflake role_id,
                    const char *hash, uint32_t size) {
  if (!hash_ok(hash)) {
    return empty(out);
  }

  int len = snprintf(out, CDN_URL_MAX,
                     CDN_BASE "/role-icons/%" PRIu64 "/%s.webp", role_id, hash);
  return sized(out, (size_t)len, size);
}

char *cdn_emoji(char out[static CDN_URL_MAX], Snowflake emoji_id,
                uint32_t size) {
  if (emoji_id == 0) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX, CDN_BASE "/emojis/%" PRIu64 ".webp", emoji_id);
  return sized(out, (size_t)len, size);
}

char *cdn_emoji_animated(char out[static CDN_URL_MAX], Snowflake emoji_id,
                         uint32_t size) {
  if (emoji_id == 0) {
    return empty(out);
  }

  // the client asks the webp for its frames rather than for a gif.
  snprintf(out, CDN_URL_MAX,
           CDN_BASE "/emojis/%" PRIu64 ".webp?size=%u&animated=true", emoji_id,
           cdn_size(size ? size : 48));
  return out;
}

char *cdn_nameplate(char out[static CDN_URL_MAX], Snowflake sku_id,
                    bool animated) {
  if (sku_id == 0) {
    return empty(out);
  }

  snprintf(out, CDN_URL_MAX,
           CDN_BASE "/media/v1/collectibles-shop/%" PRIu64 "/%s", sku_id,
           animated ? "animated" : "static");
  return out;
}

char *cdn_tag_badge(char out[static CDN_URL_MAX], Snowflake guild_id,
                    const char *hash, uint32_t size) {
  if (guild_id == 0 || !hash_ok(hash)) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX, CDN_BASE "/guild-tag-badges/%" PRIu64 "/%s.png",
             guild_id, hash);
  return sized(out, (size_t)len, size);
}

// what an url's path may hold, so that a presence cannot smuggle anything
// else into one.
static bool path_ok(const char *path) {
  if (path[0] == '\0' || strlen(path) > 512) {
    return false;
  }

  for (const char *c = path; *c != '\0'; c++) {
    bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || strchr("/._-~%=", *c) != nullptr;

    if (!ok) {
      return false;
    }
  }

  return strstr(path, "..") == nullptr;
}

char *cdn_avatar_decoration(char out[static CDN_URL_MAX], const char *asset,
                            uint32_t size) {
  if (!hash_ok(asset)) {
    return empty(out);
  }

  // an animated one is an apng, which the proxy stills when asked.
  snprintf(out, CDN_URL_MAX,
           CDN_BASE "/avatar-decoration-presets/%s.png?size=%u"
                    "&passthrough=false",
           asset, cdn_size(size));
  return out;
}

char *cdn_badge_icon(char out[static CDN_URL_MAX], const char *hash) {
  if (!hash_ok(hash)) {
    return empty(out);
  }

  snprintf(out, CDN_URL_MAX, CDN_BASE "/badge-icons/%s.png", hash);
  return out;
}

char *cdn_application_icon(char out[static CDN_URL_MAX],
                           Snowflake application_id, const char *hash,
                           uint32_t size) {
  if (application_id == 0 || !hash_ok(hash)) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX, CDN_BASE "/app-icons/%" PRIu64 "/%s.png",
             application_id, hash);
  return sized(out, (size_t)len, size);
}

char *cdn_activity_image(char out[static CDN_URL_MAX], Snowflake application_id,
                         const char *image, uint32_t size) {
  if (image == nullptr) {
    return empty(out);
  }

  if (strncmp(image, "mp:", 3) == 0) {
    if (!path_ok(image + 3)) {
      return empty(out);
    }

    char url[CDN_URL_MAX];

    snprintf(url, sizeof url, "https://media.discordapp.net/%s", image + 3);
    return cdn_media(out, url, size, size);
  }

  if (strncmp(image, "spotify:", 8) == 0) {
    if (!hash_ok(image + 8)) {
      return empty(out);
    }

    snprintf(out, CDN_URL_MAX, "https://i.scdn.co/image/%s", image + 8);
    return out;
  }

  // anything else is the id of an asset of the application.
  if (application_id == 0 || !hash_ok(image) || strchr(image, ':') != nullptr) {
    return empty(out);
  }

  int len =
    snprintf(out, CDN_URL_MAX, CDN_BASE "/app-assets/%" PRIu64 "/%s.png",
             application_id, image);
  return sized(out, (size_t)len, size);
}

char *cdn_sticker(char out[static CDN_URL_MAX], Snowflake sticker_id,
                  StickerFormat format, uint32_t size, bool animated) {
  // as the client asks the media proxy for each: a gif only moves as a gif
  // and is a png when still, and an animated png comes whole unless told
  // not to pass through.
  const char *extension = "webp", *more = "&quality=lossless";

  switch (format) {
    case STICKER_PNG:
      break;

    case STICKER_APNG:
      extension = "png";
      more = animated ? "" : "&passthrough=false";
      break;

    case STICKER_GIF:
      extension = animated ? "gif" : "png";
      more = "";
      break;

    default:
      return empty(out);
  }

  if (sticker_id == 0) {
    return empty(out);
  }

  snprintf(out, CDN_URL_MAX, MEDIA_BASE "/stickers/%" PRIu64 ".%s?size=%u%s",
           sticker_id, extension, cdn_size(size ? size : 160), more);
  return out;
}

char *cdn_media(char out[static CDN_URL_MAX], const char *proxy_url,
                uint32_t width, uint32_t height) {
  if (proxy_url == nullptr || proxy_url[0] == '\0') {
    return empty(out);
  }

  size_t len = strlen(proxy_url);

  if (len + 64 >= CDN_URL_MAX) {
    return empty(out);
  }

  memcpy(out, proxy_url, len + 1);

  // a signed url already ends its query with an ampersand.
  bool has_query = strchr(out, '?') != nullptr;
  char last = out[len - 1];
  const char *join = !has_query ? "?" : last == '&' || last == '?' ? "" : "&";

  if (width != 0 && height != 0) {
    snprintf(out + len, CDN_URL_MAX - len, "%sformat=webp&width=%u&height=%u",
             join, width, height);
  }
  else {
    snprintf(out + len, CDN_URL_MAX - len, "%sformat=webp", join);
  }

  return out;
}

// whether the path of `url`, its query aside, ends with `.ext`.
static bool path_ends(const char *url, const char *ext) {
  size_t path = strcspn(url, "?#"), n = strlen(ext);

  return path > n && url[path - n - 1] == '.' &&
         strncasecmp(url + path - n, ext, n) == 0;
}

// a playing gif comes as itself, an animated webp or avif as an animated
// webp: the proxy's frozen ones are webp stills.
char *cdn_media_animated(char out[static CDN_URL_MAX], const char *proxy_url,
                         uint32_t width, uint32_t height) {
  if (proxy_url == nullptr || proxy_url[0] == '\0') {
    return empty(out);
  }

  size_t len = strlen(proxy_url);

  if (len + 80 >= CDN_URL_MAX) {
    return empty(out);
  }

  memcpy(out, proxy_url, len + 1);

  bool has_query = strchr(out, '?') != nullptr;
  char last = out[len - 1];
  const char *join = !has_query ? "?" : last == '&' || last == '?' ? "" : "&";
  bool gif = path_ends(proxy_url, "gif");
  bool avif = path_ends(proxy_url, "avif");

  snprintf(out + len, CDN_URL_MAX - len, "%s%swidth=%u&height=%u%s", join,
           avif ? "format=webp&" : "", width, height,
           gif ? "" : "&animated=true");
  return out;
}
