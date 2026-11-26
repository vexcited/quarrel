#include "components.h"

#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/expressions.h"
#include "discord/fakestickers.h"
#include "discord/markdown.h"
#include "media.h"
#include "widgets.h"

constexpr float GUTTER = MESSAGE_GUTTER;
constexpr int64_t GROUP_GAP_MS = 7 * 60 * 1000;

// what the keys of messages hang from, so that one shown twice, as in the
// inbox, keeps two of each.
static civue_id key_scope;

static civue_id message_key(const Message *m) {
  return id_at(key_scope ? key_scope : id("message"),
               m->id ? m->id : m->nonce);
}

static bool starts_with(const char *s, const char *prefix) {
  return s && !strncmp(s, prefix, strlen(prefix));
}

// one picture of an embed, `size` points, opened in the viewer when pressed.
static void embed_picture(App *app, civue_id id, Snowflake message_id,
                          const EmbedMedia *m, civue_vec2 size) {
  MediaItem item = {
    .kind = MEDIA_IMAGE,
    .url = m->url,
    .proxy_url = m->proxy_url,
    .width = m->width,
    .height = m->height,
    .animated = m->animated,
  };

  media_tile(app, id, message_id, &item, 1, 0, size, 4);
}

static void format_bytes(uint64_t size, char *out, size_t cap) {
  if (size < 1024) {
    snprintf(out, cap, "%llu bytes", (unsigned long long)size);
  }
  else if (size < 1024 * 1024) {
    snprintf(out, cap, "%.2f KB", (double)size / 1024);
  }
  else {
    snprintf(out, cap, "%.2f MB", (double)size / (1024 * 1024));
  }
}

void file_card(App *app, civue_id id, const Attachment *a) {
  const Look *k = &app->look;
  char size[32];

  format_bytes(a->size, size, sizeof size);

  div(flex, items_center, gap(2), w(full), max_w(px(432)), p(4), rounded(lg),
      bg(k->surface), border(1), border_color(k->border)) {
    icon(lucide, file, size(8), flex_shrink_0, text_color(k->subtle));

    div(flex, flex_col, flex_1, min_w(0)) {
      div(key(id), text_color(k->link), cursor_pointer) {
        if (button().activated && a->url) {
          civue_open_url(a->url);
        }

        text(a->filename ? a->filename : "file", truncate);
      }

      text(size, text_size(px(12)), line_height(px(16)), text_color(k->muted));
    }
  }
}

// what of an attachment shows as a picture or plays as a video. discord
// tells them apart by the name's extension, its content type only when it
// has none it knows.
static bool attachment_media(const Attachment *a, MediaItem *out) {
  static const char *pictures[] = {"jpg",  "jpeg", "png", "webp",
                                   "gif",  "avif", "heic", "heif"};
  static const char *videos[] = {"mp4", "webm", "mov", "m4v"};
  const char *dot = a->filename ? strrchr(a->filename, '.') : nullptr;
  bool image = false, video = false;

  for (size_t i = 0; dot && i < countof(pictures); i++) {
    image |= !strcasecmp(dot + 1, pictures[i]);
  }

  for (size_t i = 0; dot && i < countof(videos); i++) {
    video |= !strcasecmp(dot + 1, videos[i]);
  }

  if (!image && !video) {
    image = starts_with(a->content_type, "image/");
    video = starts_with(a->content_type, "video/");
  }

  // a video plays from the file, its picture from the media proxy.
  if (!a->width || !a->height || !(image || (video && a->proxy_url))) {
    return false;
  }

  bool gif = (dot && !strcasecmp(dot + 1, "gif")) ||
             starts_with(a->content_type, "image/gif");

  *out = (MediaItem){
    .kind = video ? MEDIA_VIDEO : MEDIA_IMAGE,
    .url = a->url,
    .proxy_url = a->proxy_url,
    .poster = a->proxy_url,
    .width = a->width,
    .height = a->height,
    .filename = a->filename,
    .animated = image && (gif || a->animated),
    .spoiler = a->spoiler,
    .duration = a->duration,
  };

  return true;
}

// FakeNitro's filterAttachments: the gif of a sticker is shown as the
// sticker.
static bool sticker_file(App *app, const Attachment *a) {
  FakeSticker shown;

  return fake_sticker_attachment(app->session->store, a, &shown);
}

static void attachments(App *app, const Message *m) {
  MediaItem items[10];
  civue_id ids[10];
  size_t n = 0;

  for (size_t i = 0; i < m->nattachments && n < countof(items); i++) {
    if (!m->attachments[i].thumbnail &&
        !sticker_file(app, &m->attachments[i]) &&
        attachment_media(&m->attachments[i], &items[n])) {
      ids[n++] = id_at(message_key(m), 0x100 + i);
    }
  }

  if (n) {
    media_mosaic(app, id_at(message_key(m), 0x80), m->id ? m->id : m->nonce,
                 items, ids, n);
  }

  // past the tenth, media shows as files.
  for (size_t i = 0, shown = 0; i < m->nattachments; i++) {
    const Attachment *a = &m->attachments[i];
    MediaItem item;

    if (a->thumbnail || sticker_file(app, a)) {
      continue;
    }

    if (attachment_media(a, &item) && shown < countof(items)) {
      shown++;
      continue;
    }

    div(mt(1)) {
      file_card(app, id_at(message_key(m), 0x100 + i), a);
    }
  }
}

/* ========================================================================== */
/* embeds, as discord's client lays them out. what follows comes from its     */
/* embed component and the stylesheet next to it.                             */
/* ========================================================================== */

constexpr float EMBED_MAX_WIDTH = 516;
constexpr float EMBED_THUMBNAIL = 80;
// the padding left and right of the content, and the borders around it.
constexpr float EMBED_CHROME = 12 + 16 + 4 + 1;

// an embed as the client reads it: discord's fields are not shown as they
// come.
typedef struct {
  const Embed *e;
  Snowflake message_id;
  const EmbedMedia *image, *thumbnail, *video;
  // more than one when embeds with the same url merged into a gallery.
  const EmbedMedia *images[4];
  size_t nimages;
  bool gifv, rich, fields, describes;
} EmbedView;

static bool type_is(const Embed *e, const char *type) {
  return e->type && !strcmp(e->type, type);
}

static bool media_sized(const EmbedMedia *m) {
  return m->url && m->width > 0 && m->height > 0;
}

static bool has_text_in(const char *s, const char *word) {
  return s && strcasestr(s, word);
}

static EmbedView embed_view(const Embed *e) {
  EmbedView v = {
    .e = e,
    .gifv = type_is(e, "gifv"),
    .rich = type_is(e, "rich"),
  };

  // a thumbnail is the picture itself for an article and an image.
  if (media_sized(&e->thumbnail)) {
    if (type_is(e, "article") || type_is(e, "image")) {
      v.image = &e->thumbnail;
    }
    else {
      v.thumbnail = &e->thumbnail;
    }
  }

  if (media_sized(&e->image)) {
    v.image = &e->image;
  }

  if (e->video.url) {
    // a video without a thumbnail shows its own first frame.
    if (!v.thumbnail && e->video.proxy_url && media_sized(&e->video)) {
      v.thumbnail = &e->video;
    }

    bool sketchfab = has_text_in(e->provider_name, "sketchfab") ||
                     starts_with(e->video.url, "https://sketchfab.com");
    bool reachable = e->video.proxy_url || starts_with(e->video.url, "https:");

    if (v.thumbnail && media_sized(&e->video) && !sketchfab && reachable) {
      v.video = &e->video;
    }
  }

  if (v.image) {
    v.images[v.nimages++] = v.image;
  }

  // only these types keep their fields, and these drop their description.
  v.fields =
    e->nfields && (v.rich || type_is(e, "auto_moderation_message") ||
                   type_is(e, "auto_moderation_notification") ||
                   type_is(e, "safety_policy_notice") ||
                   type_is(e, "safety_system_notification") ||
                   type_is(e, "voice_channel") || type_is(e, "poll_result"));
  v.describes =
    e->description && !type_is(e, "image") && !type_is(e, "video") && !v.gifv;

  return v;
}

// the picture alone, without the card around it.
static bool embed_inline(const EmbedView *v) {
  return (v->image || v->video) &&
         (v->gifv || (!v->rich && !v->e->author_name && !v->e->title));
}

// tiktok and upright youtube videos get more height.
static civue_vec2 embed_media_max(const EmbedView *v) {
  const char *provider = v->e->provider_name;
  bool tiktok = provider && !strcmp(provider, "TikTok");
  bool youtube =
    (provider && !strcmp(provider, "YouTube")) ||
    (v->video && starts_with(v->video->url, "https://www.youtube.com"));
  bool upright = v->video && v->video->height > v->video->width;

  return point(400, tiktok || (upright && youtube) ? 450 : 300);
}

static civue_vec2 embed_video_size(const EmbedView *v) {
  civue_vec2 max = embed_media_max(v);
  const EmbedMedia *of = v->video ? v->video : v->thumbnail;
  civue_vec2 size = media_fit(of->width, of->height, max.x, max.y);

  return point(fmax(size.x, 150), fmax(size.y, 144));
}

// a video the system cannot play, a youtube one: its first frame, opened in
// the browser when pressed.
static void embed_link_video(App *app, civue_id id, const EmbedView *v) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_vec2 size = embed_video_size(v);
  const EmbedMedia *poster = v->thumbnail;
  const char *target = v->e->url ? v->e->url : v->video->url;
  char sized[CDN_URL_MAX];

  cdn_media(sized, poster->proxy_url ? poster->proxy_url : poster->url,
            (uint32_t)(size.x * ui->scale), (uint32_t)(size.y * ui->scale));

  div(key(id), relative, flex, items_center, justify_center, flex_shrink_0,
      w(px(size.x)), max_w(full), h(px(size.y)), rounded(px(4)), bg(k->surface),
      overflow_hidden, cursor_pointer) {
    if (button().activated && target) {
      civue_open_url(target);
    }

    image_cover(app, sized, 4);

    div(flex, items_center, justify_center, size(12), rounded(full),
        bg(rgba(0x00000099)), text_color(CIVUE_WHITE)) {
      icon(lucide, play, size(6));
    }
  }
}

// a video the media proxy serves, or a gif made a video, played in place.
static void embed_video(App *app, civue_id id, const EmbedView *v) {
  const EmbedMedia *video = v->video, *poster = v->thumbnail;

  if (!v->gifv && !video->proxy_url) {
    embed_link_video(app, id, v);
    return;
  }

  MediaItem item = {
    .kind = v->gifv ? MEDIA_GIFV : MEDIA_VIDEO,
    .url = v->gifv || !video->proxy_url ? video->url : video->proxy_url,
    .proxy_url = video->proxy_url,
    .poster = poster->proxy_url ? poster->proxy_url : poster->url,
    .width = video->width ? video->width : poster->width,
    .height = video->height ? video->height : poster->height,
  };

  media_tile(app, id, v->message_id, &item, 1, 0, embed_video_size(v), 4);
}

// two to four pictures in two columns, the third under the first when there
// are four and under the second otherwise.
static void embed_gallery(App *app, civue_id id, const EmbedView *v) {
  size_t n = v->nimages;
  civue_vec2 max = embed_media_max(v);
  float column = (max.x - 4) / 2;
  float tall = max.y, half = (max.y - 4) / 2;
  MediaItem items[4];

  for (size_t i = 0; i < n; i++) {
    const EmbedMedia *m = v->images[i];

    items[i] = (MediaItem){
      .kind = MEDIA_IMAGE,
      .url = m->url,
      .proxy_url = m->proxy_url,
      .width = m->width,
      .height = m->height,
      .animated = m->animated,
    };
  }

  div(flex, gap(1), w(px(max.x)), max_w(full)) {
    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      media_tile(app, id_at(id, 0), v->message_id, items, n, 0,
                 point(column, n == 4 ? half : tall), 4);

      if (n == 4) {
        media_tile(app, id_at(id, 2), v->message_id, items, n, 2,
                   point(column, half), 4);
      }
    }

    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      media_tile(app, id_at(id, 1), v->message_id, items, n, 1,
                 point(column, n == 2 ? tall : half), 4);

      if (n >= 3) {
        media_tile(app, id_at(id, n == 3 ? 2 : 3), v->message_id, items, n,
                   n == 3 ? 2 : 3, point(column, half), 4);
      }
    }
  }
}

// what the embed shows as its media, nothing when it has none.
static bool embed_media(App *app, civue_id id, const EmbedView *v) {
  civue_vec2 max = embed_media_max(v);

  if (v->gifv) {
    if (!v->video || !v->thumbnail) {
      return false;
    }

    embed_video(app, id, v);
  }
  else if (v->video) {
    embed_video(app, id, v);
  }
  else if (v->nimages > 1) {
    embed_gallery(app, id, v);
  }
  else if (v->image) {
    embed_picture(app, id, v->message_id, v->image,
                  media_fit(v->image->width, v->image->height, max.x, max.y));
  }
  else {
    return false;
  }

  return true;
}

static bool embed_has_media(const EmbedView *v) {
  return v->gifv ? v->video && v->thumbnail : v->video || v->image;
}

// how wide one line of text is, `cap` once it reaches it.
static float line_width(const char *s, size_t n, float size, float weight,
                        float cap) {
  // a line this long is wider than any embed.
  if (n > 160) {
    return cap;
  }

  civue_text_style style = civue_style(size, weight, size * 1.3f, 0);
  float width = civue_layout_n(civue_ui_current(), s, n, style, 0)->width;

  return width < cap ? width : cap;
}

// the widest line of a text that may hold several.
static float text_width(const char *s, float size, float weight, float cap) {
  float widest = 0;

  while (s && *s && widest < cap) {
    size_t n = strcspn(s, "\n");

    widest = fmaxf(widest, line_width(s, n, size, weight, cap));
    s += n + (s[n] == '\n');
  }

  return widest;
}

// the fields in rows: the inline ones side by side, three at most.
static size_t field_row(const Embed *e, size_t first) {
  size_t n = 1;

  if (!e->fields[first].is_inline) {
    return 1;
  }

  while (n < 3 && first + n < e->nfields && e->fields[first + n].is_inline) {
    n++;
  }

  return n;
}

static void embed_footer_text(const Embed *e, char *out, size_t cap) {
  char stamp[48] = "";

  if (e->timestamp) {
    format_stamp(e->timestamp, stamp, sizeof stamp);
  }

  snprintf(out, cap, "%s%s%s", e->footer_text ? e->footer_text : "",
           e->footer_text && e->timestamp ? " • " : "", stamp);
}

// the width the card takes: what its widest line and its media ask for, up
// to the most discord lets it.
static float embed_width(const EmbedView *v, bool thumbnail) {
  const Embed *e = v->e;
  const EmbedMedia *sized = v->image ? v->image : v->video;
  float cap = EMBED_MAX_WIDTH;
  float media_width = 0;

  if (v->video) {
    media_width = embed_video_size(v).x;
  }
  else if (v->image) {
    civue_vec2 max = embed_media_max(v);

    media_width = media_fit(v->image->width, v->image->height, max.x, max.y).x;
  }

  // a card around media stays close to it.
  if (sized && (type_is(e, "video") || media_width >= 300)) {
    cap = media_width;
  }
  else if (v->rich && v->nimages > 1) {
    cap = 520 - EMBED_CHROME;
  }

  float beside = thumbnail ? EMBED_THUMBNAIL + 16 : 0;
  float room = cap - beside;
  float widest = v->nimages > 1 ? cap : media_width;
  char footer[512];

  widest = fmaxf(widest, text_width(e->provider_name, 12, 400, room) + beside);
  widest = fmaxf(widest, text_width(e->author_name, 14, 600, room) +
                           (e->author_icon ? 32 : 0) + beside);
  widest = fmaxf(widest, text_width(e->title, 16, 600, room) + beside);

  if (v->describes) {
    widest = fmaxf(widest, text_width(e->description, 14, 400, room) + beside);
  }

  for (size_t i = 0; v->fields && i < e->nfields;) {
    size_t n = field_row(e, i);
    float row = (float)(n - 1) * 8;

    for (size_t j = i; j < i + n; j++) {
      row += fmaxf(text_width(e->fields[j].name, 14, 600, room),
                   text_width(e->fields[j].value, 14, 400, room));
    }

    widest = fmaxf(widest, row + beside);
    i += n;
  }

  embed_footer_text(e, footer, sizeof footer);
  widest =
    fmaxf(widest, text_width(footer, 12, 500, cap) + (e->footer_icon ? 28 : 0));

  // a hair more than measured, so no line wraps for a fraction of a point.
  return fminf(widest + 1, cap) + EMBED_CHROME;
}

static void embed_link(const Look *k, civue_id id, const char *label,
                       const char *url, civue_color plain) {
  bool pressed = false;

  div(key(id), min_w(0), text_color(url ? k->link : plain),
      style(.cursor = url ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    pressed = button().activated && url;
    text(label);
  }

  if (pressed) {
    civue_open_url(url);
  }
}

static void embed_author(App *app, civue_id id, const Embed *e) {
  const Look *k = &app->look;

  div(flex, items_center, mt(2), min_w(0), text_size(px(14)),
      line_height(px(18)), font_weight(semibold)) {
    if (e->author_icon) {
      div(size(6), mr(2), flex_shrink_0, rounded(full), overflow_hidden) {
        image_fill(app, e->author_icon, 12);
      }
    }

    // discord keeps a linked author in the text's color.
    div(key(id), flex_1, min_w(0), text_color(k->strong),
        style(.cursor =
                e->author_url ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
              .has_cursor = true)) {
      if (button().activated && e->author_url) {
        civue_open_url(e->author_url);
      }

      text(e->author_name);
    }
  }
}

static void embed_fields(App *app, civue_id id, const Embed *e,
                         Snowflake guild_id, float width) {
  const Look *k = &app->look;

  div(flex, flex_col, gap(2), mt(2), min_w(0)) {
    for (size_t i = 0; i < e->nfields;) {
      size_t n = field_row(e, i);
      float each = (width - (float)(n - 1) * 8) / (float)n;

      div(flex, gap(2), min_w(0)) {
        for (size_t j = i; j < i + n; j++) {
          const EmbedField *field = &e->fields[j];

          div(flex, flex_col, flex_1, flex_basis(0), min_w(0)) {
            text(field->name ? field->name : "", mb(px(2)), text_size(px(14)),
                 line_height(px(18)), font_weight(semibold),
                 text_color(k->strong));
            markup(app, id_at(id, 0x40 + j), field->value ? field->value : "",
                   &(MarkupOptions){
                     .guild_id = guild_id,
                     .size = 14,
                     .hint = each,
                   });
          }
        }
      }

      i += n;
    }
  }
}

static void embed_footer(App *app, const Embed *e) {
  const Look *k = &app->look;
  char label[512];

  embed_footer_text(e, label, sizeof label);

  div(flex, items_center, mt(2), min_w(0)) {
    if (e->footer_icon && *e->footer_icon && e->footer_text) {
      div(size(5), mr(2), flex_shrink_0, rounded(full), overflow_hidden) {
        image_fill(app, e->footer_icon, 10);
      }
    }

    text(label, flex_1, min_w(0), text_size(px(12)), line_height(px(16)),
         font_weight(medium), text_color(k->text));
  }
}

static void embed(App *app, civue_id id, const EmbedView *v,
                  Snowflake guild_id) {
  const Look *k = &app->look;
  const Embed *e = v->e;

  if (embed_inline(v)) {
    embed_media(app, id_at(id, 1), v);
    return;
  }

  bool has_media = embed_has_media(v);
  // rich embeds keep their thumbnail next to an image, not next to a video.
  bool thumbnail =
    v->thumbnail && (v->rich || type_is(e, "audio") ? !v->video : !has_media);
  bool plain = !e->has_color || e->color == 0xffffff;
  float width = embed_width(v, thumbnail);
  float inner = width - EMBED_CHROME;
  float beside = thumbnail ? EMBED_THUMBNAIL + 16 : 0;

  // the colored edge is the card's own ground, showing left of its content.
  div(flex, w(px(width)), max_w(full), pl(1), rounded(px(4)),
      bg(plain ? rgb(0x4e4f58) : rgb(e->color)), overflow_hidden) {
    div(flex, flex_col, flex_1, min_w(0), pt(px(2)), pr(4), pb(4), pl(3),
        bg(k->surface_high), border_y(1), border_r(1),
        border_color(k->border)) {
      div(flex, min_w(0)) {
        div(flex, flex_col, flex_1, min_w(0)) {
          if (e->provider_name) {
            div(mt(2), min_w(0), text_size(px(12)), line_height(px(16))) {
              embed_link(k, id_at(id, 7), e->provider_name, e->provider_url,
                         k->subtle);
            }
          }

          if (e->author_name) {
            embed_author(app, id_at(id, 2), e);
          }

          if (e->title) {
            div(mt(2), min_w(0), text_size(px(16)), line_height(px(22)),
                font_weight(semibold)) {
              embed_link(k, id_at(id, 3), e->title, e->url, k->strong);
            }
          }

          if (v->describes) {
            div(mt(2), min_w(0)) {
              markup(app, id_at(id, 4), e->description,
                     &(MarkupOptions){
                       .guild_id = guild_id,
                       .size = 14,
                       .hint = inner - beside,
                     });
            }
          }

          if (v->fields) {
            embed_fields(app, id, e, guild_id, inner - beside);
          }
        }

        if (thumbnail) {
          div(ml(4), mt(2), flex_shrink_0) {
            embed_picture(app, id_at(id, 6), v->message_id, v->thumbnail,
                          media_fit(v->thumbnail->width, v->thumbnail->height,
                                    EMBED_THUMBNAIL, EMBED_THUMBNAIL));
          }
        }
      }

      if (has_media) {
        div(flex, mt(4), min_w(0)) {
          embed_media(app, id_at(id, 5), v);
        }
      }

      if (e->footer_text || e->timestamp) {
        embed_footer(app, e);
      }
    }
  }
}

static void embeds(App *app, const Message *m, Snowflake guild_id) {
  if (m->flags & MESSAGE_FLAG_SUPPRESS_EMBEDS) {
    return;
  }

  // FakeNitro's shouldIgnoreEmbed: the emoji is shown in the words already.
  bool fake = fake_emoji_message(m->content);

embeds:
  for (size_t i = 0; i < m->nembeds; i++) {
    const Embed *e = &m->embeds[i];
    EmbedView v = embed_view(e);
    const char *pictured = e->url             ? e->url
                           : e->image.url     ? e->image.url
                                              : e->thumbnail.url;
    const char *name = nullptr;
    size_t name_len = 0;
    Snowflake emoji_id = 0;
    bool animated = false;

    if (fake && pictured && type_is(e, "image") &&
        fake_emoji_url(pictured, &emoji_id, &animated, &name, &name_len)) {
      continue;
    }

    // the same for the sticker a message links to.
    if (fake_sticker_embed(app->session->store, m, e)) {
      continue;
    }

    v.message_id = m->id ? m->id : m->nonce;

    // embeds sharing a url are one embed: the first, with every picture.
    for (size_t j = 0; e->url && j < i; j++) {
      if (m->embeds[j].url && !strcmp(m->embeds[j].url, e->url)) {
        continue embeds;
      }
    }

    for (size_t j = i + 1;
         e->url && j < m->nembeds && v.nimages < countof(v.images); j++) {
      const Embed *next = &m->embeds[j];

      if (next->url && !strcmp(next->url, e->url)) {
        EmbedView more = embed_view(next);

        if (more.image) {
          v.images[v.nimages++] = more.image;
        }
      }
    }

    div(flex, flex_col, items_start, mt(1), min_w(0)) {
      embed(app, id_at(message_key(m), 0x1000 + i * 0x100), &v, guild_id);
    }
  }
}

// whether the stickers of a message move, from what the user set in
// discord, and only while the window has their attention, as the client's.
static bool stickers_move(App *app, const Message *m) {
  const Store *store = app->session->store;
  bool hot = app->hover_message == (m->id ? m->id : m->nonce);

  if (!civue_ui_current()->window_focused) {
    return false;
  }

  switch (store->animate_stickers) {
    case STICKERS_ANIMATE_NEVER:
      return false;
    case STICKERS_ANIMATE_ON_INTERACTION:
      return hot;
    default:
      return true;
  }
}

static void stickers(App *app, const Message *m) {
  // FakeNitro's patchFakeNitroStickers: the ones a message links to or
  // carries as a gif come after its own.
  FakeSticker fakes[FAKE_STICKERS_MAX];
  size_t nfakes =
    fake_stickers(app->session->store, m, fakes, countof(fakes));
  bool moving = (m->nstickers || nfakes) && stickers_move(app, m);

  // one in a preview, which is no message of the open channel, opens no
  // card.
  for (size_t i = 0; i < m->nstickers; i++) {
    sticker_in_message(app, id_at(message_key(m), 0x600 + i), m, i,
                       &m->stickers[i], moving, key_scope == 0);
  }

  for (size_t i = 0; i < nfakes; i++) {
    size_t at = m->nstickers + i;

    sticker_in_message(
      app, id_at(message_key(m), 0x600 + at), m, at,
      &(Sticker){fakes[i].id, fakes[i].name, fakes[i].format}, moving,
      key_scope == 0);
  }
}

static void reactions(App *app, const Message *m) {
  const Look *k = &app->look;

  if (!m->nreactions) {
    return;
  }

  div(flex, flex_wrap, gap(1), mt(1)) {
    for (size_t i = 0; i < m->nreactions; i++) {
      const Reaction *r = &m->reactions[i];
      char count[16];

      snprintf(count, sizeof count, "%u", r->count);

      div(key(id_at(message_key(m), 0x200 + i)), flex, items_center, gap(px(6)),
          h(7), px(px(7)), rounded(lg),
          bg(r->me ? civue_alpha(k->brand, .24f) : k->hover), border(1),
          border_color(r->me ? k->brand : CIVUE_CLEAR),
          hover(border_color(r->me ? k->brand : k->border_strong)),
          cursor_pointer) {
        if (button().activated) {
          session_react(app->session, m->channel_id, m->id, r->emoji_id,
                        r->name, !r->me);
        }

        if (r->emoji_id && r->animated) {
          div(size(4), flex_shrink_0) {
            emoji_image(app, r->emoji_id, true, here());
          }
        }
        else {
          emoji_node(app, r->emoji_id, r->name, 16);
        }

        text(count, text_size(px(14)), line_height(px(18)),
             font_weight(semibold),
             text_color(r->me ? k->mention_text : k->subtle));
      }
    }
  }
}

// the line over a reply: who and what it answers, tied to the avatar by a
// spine.
static void reply_line(App *app, const Message *m, Snowflake guild_id) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Message *to = m->referenced;

  div(relative, flex, items_center, gap(1), h(px(18)), mb(1), text_size(px(14)),
      line_height(px(18)), text_color(k->subtle)) {
    div(absolute, left(px(-37)), top(px(8)), w(px(33)), h(px(14)), border_l(2),
        border_t(2), rounded_tl(px(6)), border_color(rgb(0x4f505a))) {}

    if (!to) {
      text("Original message was deleted", italic, text_color(k->muted));
      continue;
    }

    Author author = store_author(store, to);
    char *plain = nullptr;

    if (*to->content) {
      MdDoc *doc = md_parse(to->content, strlen(to->content),
                            &(MdOptions){.inline_only = true});
      plain = md_plain_text(doc);
      md_free(doc);
    }

    avatar(app, author.user, guild_id, 16, false, k->chat);

    div(key(id_at(message_key(m), 0x502)), flex_shrink_0, cursor_pointer) {
      profile_name_trigger(app, to->author_id, guild_id);
      text(author.name, font_weight(medium), whitespace_nowrap,
           text_color(author.color ? rgb(author.color) : k->text));
    }

    if (plain && *plain) {
      text(plain, flex_1, min_w(0), truncate);
    }
    else {
      text("Click to see attachment", italic, flex_1, min_w(0), truncate);
    }

    free(plain);
  }
}

// the line over what an application answered a command with: who ran it
// and what it is called, tied to the avatar like the line of a reply.
static void command_line(App *app, const Message *m, Snowflake guild_id) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const User *user = store_user(store, m->interaction_user_id);
  uint32_t color = store_member_color(store, guild_id, m->interaction_user_id);
  char name[160];

  snprintf(name, sizeof name, "/%s", m->interaction_name);

  div(relative, flex, items_center, gap(1), h(px(18)), mb(1), text_size(px(14)),
      line_height(px(18)), text_color(k->subtle)) {
    div(absolute, left(px(-37)), top(px(8)), w(px(33)), h(px(14)), border_l(2),
        border_t(2), rounded_tl(px(6)), border_color(rgb(0x4f505a))) {}

    avatar(app, user, guild_id, 16, false, k->chat);

    div(key(id_at(message_key(m), 0x503)), flex_shrink_0, cursor_pointer) {
      profile_name_trigger(app, m->interaction_user_id, guild_id);
      text(store_display_name(store, guild_id, m->interaction_user_id),
           font_weight(medium), whitespace_nowrap,
           text_color(color ? rgb(color) : k->text));
    }

    text("used", flex_shrink_0);
    text(name, min_w(0), truncate, font_weight(medium), text_color(k->link));
  }
}

// what stands for the answer of a command that has none yet, in the words
// of discord's client. null once it has one.
static const char *command_wait(App *app, const Message *m, char *out,
                                size_t cap) {
  if (*m->content || m->nembeds || m->nattachments || m->ncomponents ||
      m->state == MESSAGE_FAILED) {
    return nullptr;
  }

  if (m->flags & MESSAGE_FLAG_LOADING) {
    snprintf(out, cap, "%s is thinking...",
             store_author(app->session->store, m).name);
    return out;
  }

  return m->state == MESSAGE_PENDING && m->interaction_name
           ? "Sending command..."
           : nullptr;
}

const char *message_system_text(const Message *m, char *out, size_t cap,
                                const char *other) {
  switch (m->type) {
    case MESSAGE_RECIPIENT_ADD:
      snprintf(out, cap, "added %s to the group.", other);
      break;
    case MESSAGE_RECIPIENT_REMOVE:
      snprintf(out, cap, "removed %s from the group.", other);
      break;
    case MESSAGE_CALL:
      return "started a call.";
    case MESSAGE_CHANNEL_NAME_CHANGE:
      snprintf(out, cap, "changed the channel name: %s", m->content);
      break;
    case MESSAGE_CHANNEL_ICON_CHANGE:
      return "changed the channel icon.";
    case MESSAGE_CHANNEL_PINNED_MESSAGE:
      return "pinned a message to this channel.";
    case MESSAGE_USER_JOIN:
      return "just showed up!";
    case MESSAGE_GUILD_BOOST:
    case MESSAGE_GUILD_BOOST_TIER_1:
    case MESSAGE_GUILD_BOOST_TIER_2:
    case MESSAGE_GUILD_BOOST_TIER_3:
      return "just boosted the server!";
    case MESSAGE_CHANNEL_FOLLOW_ADD:
      snprintf(out, cap, "has added %s to this channel.", m->content);
      break;
    case MESSAGE_THREAD_CREATED:
      snprintf(out, cap, "started a thread: %s", m->content);
      break;
    default:
      return *m->content ? m->content : "sent a message Quarrel cannot show.";
  }

  return out;
}

static void system_message(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Author author = store_author(store, m);
  char line[512], stamp[48];
  const char *other = m->nmentions
                        ? store_display_name(store, c->guild_id, m->mentions[0])
                        : "someone";
  bool boost =
    m->type >= MESSAGE_GUILD_BOOST && m->type <= MESSAGE_GUILD_BOOST_TIER_3;

  format_stamp(m->timestamp, stamp, sizeof stamp);

  div(absolute, left(0), top(px(4)), w(px(GUTTER)), flex, justify_center) {
    if (m->type == MESSAGE_USER_JOIN || m->type == MESSAGE_RECIPIENT_ADD) {
      icon(lucide, arrow_right, size(px(18)), text_color(k->online));
    }
    else if (m->type == MESSAGE_CHANNEL_PINNED_MESSAGE) {
      icon(lucide, pin, size(px(18)), text_color(k->subtle));
    }
    else if (boost) {
      icon(lucide, gem, size(px(18)), text_color(rgb(0xff73fa)));
    }
    else {
      icon(lucide, info, size(px(18)), text_color(k->subtle));
    }
  }

  div(flex, flex_wrap, items_baseline, gap_x(1), text_color(k->subtle)) {
    div(key(id_at(message_key(m), 0x500)), cursor_pointer) {
      profile_name_trigger(app, m->author_id, c->guild_id);
      text(author.name, font_weight(medium),
           text_color(author.color ? rgb(author.color) : k->strong));
    }

    text(message_system_text(m, line, sizeof line, other), min_w(0));
    text(stamp, text_size(px(12)), text_color(k->faint), ml(1),
         whitespace_nowrap);
  }
}

static void header(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Author author = store_author(app->session->store, m);
  char stamp[48];
  bool tagged = (author.user && author.user->bot) || m->webhook_id;
  bool profiled = author.user && !m->webhook_id;

  format_stamp(m->timestamp, stamp, sizeof stamp);

  div(flex, items_baseline, gap(2), min_w(0)) {
    div(key(id_at(message_key(m), 0x500)), min_w(0),
        style(.has_cursor = profiled, .cursor = CIVUE_CURSOR_POINTER)) {
      if (profiled) {
        profile_name_trigger(app, m->author_id, c->guild_id);
      }

      text(author.name, font_weight(medium), truncate,
           text_color(author.color ? rgb(author.color) : k->strong));
    }

    if (tagged) {
      div(self_center, flex_shrink_0, px(px(5)), rounded(sm), bg(k->brand)) {
        text("APP", text_size(px(12)), line_height(px(16)),
             font_weight(semibold), text_color(CIVUE_WHITE));
      }
    }

    text(stamp, flex_shrink_0, text_size(px(12)), text_color(k->faint),
         whitespace_nowrap);
  }
}

static bool tool(App *app, civue_id id, civue_icon icon, const char *label,
                 civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, size(8), rounded(md),
      text_color(color), hover(bg(k->hover), text_color(k->strong)),
      cursor_pointer) {
    civue_button_state state = button();
    pressed = state.activated;

    civue_node_icon(civue_ui_current(), icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20)));
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
  }

  return pressed;
}

void app_edit_message(App *app, const Message *m) {
  if (!app->editing) {
    app->kept_draft = app->draft;
    app->draft = nullptr;
  }

  free(app->draft);
  app->draft = strdup(m->content);
  app->editing = m->id;
  app->reply_to = 0;
  app->focus_composer = true;
}

// discord's: the reactions the user gives the most, then a way to any
// other, to answer or edit, to forward, and to the rest.
static void toolbar(App *app, Channel *c, const Message *m) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  civue_id id = id_at(message_key(m), 0x300);
  bool own = m->author_id == store->self_id;
  bool plain = !message_is_system(m);
  bool reacts = store_can(store, c, PERM_ADD_REACTIONS);
  ReactionPick picks[3];
  size_t npicks = reacts ? quick_reactions(app, c, picks, countof(picks)) : 0;
  civue_rect bar = {};
  Popover opens = POPOVER_NONE;

  div(absolute, right(4), top(px(-16)), flex, items_center, p(px(2)),
      rounded(lg), bg(k->surface), border(1), border_color(k->border),
      shadow(sm)) {
    bar = here();
    app->toolbar_rect = civue_rect_inflate(bar, 4, 4);

    for (size_t i = 0; i < npicks; i++) {
      div(key(id_at(id, 0x10 + i)), flex, items_center, justify_center, size(8),
          rounded(md), hover(bg(k->hover)), cursor_pointer) {
        if (button().activated) {
          react_with(app, m, picks[i]);
        }

        emoji_node(app, picks[i].id, picks[i].name, 20);
      }
    }

    if (npicks) {
      div(flex_shrink_0, w(px(1)), h(5), mx(1), bg(k->border)) {}
    }

    if (reacts && tool(app, id_at(id, 1), CIVUE_ICON(lucide, smile_plus),
                       "Add Reaction", k->subtle)) {
      opens = POPOVER_REACT;
    }

    if (own && plain) {
      if (tool(app, id_at(id, 2), CIVUE_ICON(lucide, pencil), "Edit",
               k->subtle)) {
        app_edit_message(app, m);
      }
    }
    else if (plain && store_can(store, c, PERM_SEND_MESSAGES) &&
             tool(app, id_at(id, 3), CIVUE_ICON(lucide, reply), "Reply",
                  k->subtle)) {
      app->reply_to = m->id;
      app->reply_ping = true;
      app->focus_composer = true;
    }

    if (plain && tool(app, id_at(id, 4), CIVUE_ICON(lucide, forward), "Forward",
                      k->subtle)) {
      opens = POPOVER_FORWARD;
    }

    if (tool(app, id_at(id, 5), CIVUE_ICON(lucide, ellipsis), "More",
             k->subtle)) {
      opens = POPOVER_MESSAGE_MENU;
    }
  }

  if (opens != POPOVER_NONE) {
    message_popover(app, opens, m, bar);
  }
}

static void failure(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  civue_id id = id_at(message_key(m), 0x400);

  div(flex, items_center, gap(2), text_size(px(14)), line_height(px(18)),
      text_color(k->critical_text)) {
    text(m->error ? m->error : "Your message could not be delivered.");

    // a command is typed again rather than sent again.
    if (!m->interaction_name) {
      div(key(id_at(id, 1)), text_color(k->link), cursor_pointer) {
        if (button().activated) {
          session_retry(app->session, c->id, m->nonce);
        }

        text("Retry");
      }
    }

    div(key(id_at(id, 2)), text_color(k->link), cursor_pointer) {
      if (button().activated) {
        session_discard(app->session, c->id, m->nonce);
      }

      text("Dismiss");
    }
  }
}

// what a message says, in a node GUTTER points in on the left: the line of
// what it answers, its author when `head`, then its text in `ink` and what
// it carries. the avatar is drawn over `ring`.
static void message_body(App *app, const Channel *c, const Message *m,
                         bool head, float width, civue_color ink,
                         civue_color ring) {
  Store *store = app->session->store;
  civue_id id = message_key(m);
  bool reply = m->reference_id && !m->forwarded && !message_is_system(m);
  bool command = !reply && m->interaction_name && m->interaction_user_id;
  char waits[160];

  if (message_is_system(m)) {
    system_message(app, c, m);
    return;
  }

  if (reply) {
    reply_line(app, m, c->guild_id);
  }
  else if (command) {
    command_line(app, m, c->guild_id);
  }

  if (head) {
    Author author = store_author(store, m);

    div(absolute, left(4), top(px(reply || command ? 26 : 2))) {
      // a webhook has no profile to show.
      if (author.user && !m->webhook_id) {
        div(key(id_at(id, 0x501)), cursor_pointer) {
          profile_trigger(app, m->author_id, c->guild_id, PROFILE_RIGHT);
          avatar(app, author.user, c->guild_id, 40, false, ring);
        }
      }
      else {
        avatar(app, author.user, c->guild_id, 40, false, ring);
      }
    }

    header(app, c, m);
  }

  if (command_wait(app, m, waits, sizeof waits)) {
    text(waits, text_color(app->look.faint));
  }

  FakeSticker linked;

  // FakeNitro's transformAst: a link to a sticker is shown as the sticker.
  if (*m->content &&
      !fake_sticker_message(app->session->store, m->content, &linked)) {
    markup(app, id_at(id, 1), m->content,
           &(MarkupOptions){
             .guild_id = c->guild_id,
             .color = ink,
             .edited = m->edited_at != 0,
             .fake_emojis = true,
             .hint = width - GUTTER - 48,
           });
  }

  attachments(app, m);
  embeds(app, m, c->guild_id);
  stickers(app, m);
  message_components(app, m, c->guild_id, id_at(id, 0x7000),
                     width - GUTTER - 48);

  // words carry the mark of an edit themselves.
  if (m->ncomponents && !*m->content && m->edited_at) {
    text("(edited)", text_size(px(10)), line_height(px(16)),
         text_color(app->look.muted));
  }

  reactions(app, m);
}

void message_preview(App *app, const Channel *c, const Message *m,
                     float width) {
  const Look *k = &app->look;
  civue_id kept = key_scope;

  key_scope = id("message-preview");

  div(key(message_key(m)), relative, pl(px(GUTTER)), pr(12), py(px(2))) {
    message_body(app, c, m, true, width, k->text, k->surface_high);
  }

  key_scope = kept;
}

static void message(App *app, Channel *c, Message *m, bool head, float width) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = message_key(m);
  bool hot = app->hover_message == (m->id ? m->id : m->nonce) ||
             message_popover_open(app, m);
  bool editing = app->editing && app->editing == m->id;
  civue_color ground = m->mentions_me ? k->mentioned
                       : editing      ? civue_alpha(k->brand, .12f)
                                      : k->message_hover;
  civue_color ink = m->state == MESSAGE_PENDING  ? k->faint
                    : m->state == MESSAGE_FAILED ? k->critical_text
                                                 : k->text;

  div(key(id), relative, pl(px(GUTTER)), pr(12), py(px(2)),
      mt(px(head ? 17 : 0)),
      style(.bg = hot || m->mentions_me || editing ? ground : CIVUE_CLEAR)) {
    civue_rect r = here();
    civue_vec2 mouse = ui->input.mouse;

    if (ui->input.has_mouse && civue_rect_contains(r, mouse) &&
        civue_rect_contains(civue_clip_rect(ui), mouse) &&
        !civue_under_overlay(ui, mouse)) {
      app->hover_next = m->id ? m->id : m->nonce;
    }

    if (m->mentions_me) {
      div(absolute, left(0), top(0), bottom(0), w(px(2)),
          bg(k->mentioned_bar)) {}
    }

    if (!head && hot && !message_is_system(m)) {
      char clock[16];
      format_clock(m->timestamp, clock, sizeof clock);

      div(absolute, left(0), top(px(2)), w(px(GUTTER - 16))) {
        text(clock, text_right, text_size(px(11)), line_height(px(22)),
             text_color(k->faint));
      }
    }

    message_body(app, c, m, head, width, ink, k->chat);
    upload_progress(app, c, m);

    if (m->state == MESSAGE_FAILED && !message_is_system(m)) {
      failure(app, c, m);
    }

    if (hot && m->state == MESSAGE_SENT) {
      toolbar(app, c, m);
    }
  }
}

static void divider(const Look *k, const char *label, bool fresh) {
  civue_color line = fresh ? k->danger : k->border;

  div(flex, items_center, gap(1), mt(6), mb(2), ml(4), mr(px(fresh ? 0 : 16)),
      h(px(0))) {
    div(flex_1, h(px), bg(line)) {}

    if (label) {
      text(label, px(1), text_size(px(12)), line_height(px(13)),
           font_weight(semibold), text_color(k->muted));
    }

    div(flex_1, h(px), bg(line)) {}

    if (fresh) {
      div(px(1), rounded_l(sm), bg(k->danger)) {
        text("NEW", text_size(px(10)), line_height(px(13)), font_weight(bold),
             text_color(CIVUE_WHITE));
      }
    }
  }
}

// a post begins with its title and the tags it wears, its first message
// under them.
static void post_beginning(App *app, const Channel *c) {
  const Look *k = &app->look;

  div(flex, flex_col, gap(2), px(4), pt(4), mb(2)) {
    text(c->name ? c->name : "", text_size(px(24)), line_height(px(30)),
         font_weight(bold), text_color(k->strong));
    post_tags(app, c, SIZE_MAX);
  }
}

static void beginning(App *app, const Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char name[128], title[192], lead[256];

  if (store_is_post(store, c)) {
    post_beginning(app, c);
    return;
  }

  store_channel_name(store, c, name, sizeof name);

  if (channel_is_private(c)) {
    snprintf(title, sizeof title, "%s", name);
    snprintf(lead, sizeof lead,
             c->type == CHANNEL_DM
               ? "This is the beginning of your direct message history with "
                 "%s."
               : "Welcome to the beginning of the %s group.",
             name);
  }
  else {
    snprintf(title, sizeof title, "Welcome to #%s!", name);
    snprintf(lead, sizeof lead, "This is the start of the #%s channel.", name);
  }

  div(flex, flex_col, gap(2), px(4), pt(4), mb(2)) {
    if (c->type == CHANNEL_DM && c->nrecipients) {
      avatar(app, store_user(store, c->recipients[0]), 0, 80, false, k->chat);
    }
    else {
      div(flex, items_center, justify_center, size(px(68)), rounded(full),
          bg(k->surface_high), text_color(k->strong)) {
        if (channel_is_private(c)) {
          icon(lucide, users, size(10));
        }
        else {
          icon(lucide, hash, size(10));
        }
      }
    }

    text(title, text_size(px(32)), line_height(px(40)), font_weight(bold),
         text_color(k->strong));
    text(lead, text_color(k->subtle));
  }
}

static bool starts_group(const Message *m, const Message *before) {
  if (!before || message_is_system(m) || message_is_system(before)) {
    return true;
  }

  return m->author_id != before->author_id ||
         m->webhook_id != before->webhook_id ||
         (m->reference_id && !m->forwarded) || m->interaction_name ||
         m->timestamp - before->timestamp > GROUP_GAP_MS ||
         day_of(m->timestamp) != day_of(before->timestamp);
}

// where the list is, before its nodes: it follows its end while it sits
// there, and stays put when history arrives over what is read.
static void place_list(App *app, const Channel *c, civue_id list,
                       civue_id inner) {
  civue_ui *ui = civue_ui_current();
  float view = (float)last_size(list).y;
  float height = (float)last_size(inner).y;
  float most = fmaxf(height - view, 0);
  Snowflake last = c->nmessages ? c->messages[c->nmessages - 1]->id : 0;
  if (app->list_channel != c->id) {
    app->list_channel = c->id;
    app->stick = true;
    app->list_height = 0;
    app->hover_message = 0;
  }

  if (!ui->rerun) {
    bool up = ui->input.scroll.y > 0 || ui->input.down[0];

    // a slow scroll leaves the end by less than a point a frame, so the
    // list only follows again once the scroll has rested there.
    if (up) {
      app->stick = false;
      app->unstuck_at = ui->now;
    }
    else if (!app->stick && civue_scroll_y(ui, list) >= most - 1 &&
             ui->now - app->unstuck_at > .3) {
      app->stick = true;
    }
  }

  if (app->stick) {
    civue_set_scroll_y(ui, list, most);
  }
  else if (app->list_height > 0 && height != app->list_height &&
           last == app->list_last) {
    civue_set_scroll_y(ui, list, most - app->from_bottom);
  }

  float y = civue_scroll_y(ui, list);

  app->from_bottom = fmaxf(most - y, 0);
  app->list_height = height;
  app->list_last = last;

  if (y < 400 && c->has_older && !c->loading && c->history_loaded) {
    session_load_older(app->session, c->id);
  }

  if (app->stick && ui->window_focused) {
    session_ack(app->session, c->id);
  }
}

void message_list(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Guild *guild = c->guild_id ? store_guild(store, c->guild_id) : nullptr;
  civue_ui *ui = civue_ui_current();
  civue_id list = id("messages"), inner = id("messages-inner");
  float width = (float)last_size(list).x;
  // the divider stays where it was when the channel was opened, the channel
  // itself being read at once.
  Snowflake *fresh =
    civue_memory(ui, id_in(list, "fresh"), 2 * sizeof *fresh, nullptr);

  if (fresh[0] != c->id) {
    fresh[0] = c->id;
    fresh[1] = store_last_acked(app->session->store, c->id);
  }

  place_list(app, c, list, inner);
  app->hover_next = 0;

  div(key(list), flex_1, min_h(0), overflow_y_scroll, scrollbars(&app->bars)) {
    div(key(inner), flex, flex_col, justify_end, min_h(full),
        pb(px(MESSAGE_FOOT))) {
      if (c->loading) {
        text("Loading messages...", px(4), py(3), text_color(k->muted));
      }
      else if (!c->has_older && c->history_loaded) {
        beginning(app, c);
      }

      const Message *before = nullptr;
      bool marked = false;

      for (size_t i = 0; i < c->nmessages; i++) {
        Message *m = c->messages[i];
        bool head = starts_group(m, before);
        bool unread = !marked && fresh[1] && m->id > fresh[1] &&
                      m->state == MESSAGE_SENT &&
                      m->author_id != app->session->store->self_id;

        if (!before || day_of(m->timestamp) != day_of(before->timestamp)) {
          char day[48];
          format_day(m->timestamp, day, sizeof day);
          divider(k, day, false);
        }

        if (unread) {
          marked = true;
          head = true;

          if (i > 0) {
            divider(k, nullptr, true);
          }
        }

        // a message is built again when what it reads changed. one drawing
        // an image is never kept, civue builds those every frame.
        Snowflake known = m->id ? m->id : m->nonce;
        uint64_t reads[] = {
          c->version,
          guild ? guild->members_version : 0,
          store->users_version,
          civue_images_version(app->images),
          app->revealed.len,
          media_live(app, known),
          app->hover_message == known,
          app->editing == known,
          app->popover == POPOVER_COMPONENT_SELECT &&
            app->select_message == known,
          head,
          (uint64_t)width,
          (uint64_t)day_of((int64_t)store_now(store) * 1000),
        };

        memo(id_at(id("message-memo"), known),
             civue_hash(reads, sizeof reads)) {
          message(app, c, m, head, width);
        }

        before = m;
      }
    }
  }

  if (app->hover_next != app->hover_message) {
    civue_request_repaint(ui);
  }
}
