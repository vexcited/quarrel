#include "media.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

/* ========================================================================== */
/* what follows comes from discord's client: its media mosaic, its lazy       */
/* image, its video player and its media viewer, with their stylesheets.      */
/* ========================================================================== */

// a player not drawn for this long is let go.
constexpr double PLAYER_KEEP = 1.5;
// the controls hide once the pointer stopped this long over a playing video,
// in the chat and in the viewer.
constexpr double CONTROLS_LINGER = 3;
constexpr double CONTROLS_LINGER_VIEWER = 1;
constexpr float CONTROLS_HEIGHT = 32;
// narrower than this, a video's controls leave its time out.
constexpr float CONTROLS_TIME_MIN = 250;
// how much a click zooms an image in the viewer, and how far a press may
// move and still be a click that zooms back out.
constexpr float ZOOM = 2.5f;
constexpr float ZOOM_CLICK = 20;

typedef struct {
  civue_id id;
  Snowflake message_id;
  civue_video *video;
  double used_at;
} Player;

// what the viewer holds of a media, its strings its own.
typedef struct {
  MediaItem item;
  char *strings[4];
} Held;

struct Media {
  CIVUE_VEC(Player) players;

  // the viewer: what it goes through, the one shown, and who sent them.
  Held *held;
  size_t count, index;
  Snowflake message_id, author_id, guild_id;
  char author[128];
  int64_t sent_at;
  // where a video the chat played goes on from, once, -1 for its start.
  double resume_at;
  // an image zoomed in, and where its top left is.
  bool zoomed;
  civue_vec2 pan;
  // how many times it opened, to come in each time.
  uint32_t opens;
};

static struct Media *media_of(App *app) {
  if (app->media == nullptr) {
    app->media = calloc(1, sizeof *app->media);
  }

  return app->media;
}

/* ========================================================================== */
/* players.                                                                   */
/* ========================================================================== */

static Player *player_find(App *app, civue_id id) {
  struct Media *m = media_of(app);

  for (uint32_t i = 0; i < m->players.len; i++) {
    if (m->players.v[i].id == id) {
      return &m->players.v[i];
    }
  }

  return nullptr;
}

// the player of `id`, made for `url` when there is none. null where the
// system plays nothing. a pointer good until the next player is made.
static Player *player_of(App *app, civue_id id, Snowflake message_id,
                         const char *url, const civue_video_options *o) {
  struct Media *m = media_of(app);
  Player *p = player_find(app, id);

  if (p == nullptr) {
    civue_video *video = civue_video_new(url, o);

    if (video == nullptr) {
      return nullptr;
    }

    if (m->players.len == m->players.cap) {
      m->players.cap = m->players.cap ? m->players.cap * 2 : 8;
      m->players.v =
        realloc(m->players.v, m->players.cap * sizeof *m->players.v);
    }

    p = &m->players.v[m->players.len++];
    *p = (Player){.id = id, .message_id = message_id, .video = video};
  }

  p->used_at = civue_ui_current()->now;
  return p;
}

static void player_drop(App *app, civue_id id) {
  struct Media *m = media_of(app);

  for (uint32_t i = 0; i < m->players.len; i++) {
    if (m->players.v[i].id == id) {
      civue_video_free(m->players.v[i].video);
      m->players.v[i] = m->players.v[--m->players.len];
      return;
    }
  }
}

uint64_t media_live(App *app, Snowflake message_id) {
  struct Media *m = media_of(app);

  for (uint32_t i = 0; i < m->players.len; i++) {
    if (m->players.v[i].message_id == message_id) {
      return civue_ui_current()->frames;
    }
  }

  return 0;
}

void media_sweep(App *app) {
  struct Media *m = media_of(app);
  civue_ui *ui = civue_ui_current();

  if (ui->rerun) {
    return;
  }

  for (uint32_t i = 0; i < m->players.len;) {
    if (ui->now - m->players.v[i].used_at > PLAYER_KEEP) {
      civue_video_free(m->players.v[i].video);
      m->players.v[i] = m->players.v[--m->players.len];
    }
    else {
      i++;
    }
  }
}

/* ========================================================================== */
/* pictures.                                                                  */
/* ========================================================================== */

// gifs move while the window is focused, as with discord's default of
// playing them on its own.
static bool moving(void) {
  return civue_ui_current()->window_focused;
}

// the picture of `item` for `w` by `h` points: the media proxy's still of a
// video, every frame of an animated image that plays.
static void picture_url(char out[static CDN_URL_MAX], const MediaItem *item,
                        float w, float h, bool playing) {
  civue_ui *ui = civue_ui_current();
  const char *from = item->kind == MEDIA_IMAGE ? item->proxy_url : item->poster;
  uint32_t pw = (uint32_t)fminf(ceilf(w * ui->scale), 4096);
  uint32_t ph = (uint32_t)fminf(ceilf(h * ui->scale), 4096);

  from = from ? from : item->kind == MEDIA_IMAGE ? item->url : nullptr;

  if (item->kind == MEDIA_IMAGE && item->animated && playing) {
    cdn_media_animated(out, from, pw, ph);
  }
  else {
    cdn_media(out, from, pw, ph);
  }
}

// `w` by `h` scaled to fit in `max`, larger only when `grow`.
static civue_vec2 fit(uint32_t w, uint32_t h, civue_vec2 max, bool grow) {
  double fw = w ? w : max.x, fh = h ? h : max.y;
  double scale = fmin(max.x / fw, max.y / fh);

  scale = grow ? scale : fmin(scale, 1);
  return point(fmax(round(fw * scale), 1), fmax(round(fh * scale), 1));
}

// the uvs that cut the middle of a `w` by `h` picture to the shape of `r`.
static void cover(civue_rect r, uint32_t w, uint32_t h, float uv[4]) {
  float shape = (float)(civue_rect_h(r) / civue_rect_w(r));
  float kept = shape * (float)w / (float)h;
  float cut_y = kept < 1 ? (1 - kept) / 2 : 0;
  float cut_x = kept > 1 ? (1 - 1 / kept) / 2 : 0;

  uv[0] = cut_x;
  uv[1] = cut_y;
  uv[2] = 1 - cut_x;
  uv[3] = 1 - cut_y;
}

// the picture over `r`, cut to its shape. false while it loads.
static bool picture(App *app, const char *url, civue_rect r, float radius,
                    bool animated) {
  civue_ui *ui = civue_ui_current();
  double side = fmax(civue_rect_w(r), civue_rect_h(r));
  uint32_t pixels = (uint32_t)fmin(ceil(side * ui->scale), 4096);
  civue_image_info image =
    animated ? civue_images_get_animated(app->images, ui, url, pixels)
             : civue_images_get(app->images, ui, url, pixels);
  float uv[4];

  if (image.state != CIVUE_IMAGE_READY || image.height == 0) {
    return false;
  }

  cover(r, image.width, image.height, uv);
  civue_image(ui, image.texture, r, uv, 1, radius, false);
  return true;
}

// a video's picture over `r`, cut to its shape.
static void frame_of(const civue_video_info *v, civue_rect r, float radius) {
  float uv[4];

  if (v->texture == 0 || v->width == 0 || v->height == 0) {
    return;
  }

  cover(r, v->width, v->height, uv);
  civue_image(civue_ui_current(), v->texture, r, uv, 1, radius, false);
}

// the pointer is over `r`, where it is not clipped away.
static bool over(civue_rect r) {
  civue_ui *ui = civue_ui_current();

  return ui->input.has_mouse && civue_rect_contains(r, ui->input.mouse) &&
         civue_rect_contains(civue_clip_rect(ui), ui->input.mouse);
}

/* ========================================================================== */
/* a video's controls.                                                        */
/* ========================================================================== */

// "1:05", or "01:05" for a video of ten minutes or more, as long as `as`.
static void format_time(double seconds, double as, char *out, size_t cap) {
  long s = seconds > 0 && isfinite(seconds) ? (long)seconds : 0;
  long longest = as > 0 && isfinite(as) ? (long)as : s;

  if (longest >= 3600) {
    snprintf(out, cap, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
  }
  else if (longest >= 600) {
    snprintf(out, cap, "%02ld:%02ld", s / 60, s % 60);
  }
  else {
    snprintf(out, cap, "%ld:%02ld", s / 60, s % 60);
  }
}

static bool control_button(civue_id id, civue_icon glyph_icon) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, size(8),
      text_color(rgb(0xdbdbde)), hover(text_color(CIVUE_WHITE)),
      cursor_pointer) {
    pressed = button().activated;
    civue_node_icon(civue_ui_current(), glyph_icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(24), .h = civue_px(24)));
  }

  return pressed;
}

// the bar a video seeks with: what played, what loaded and what is left.
static void seek_bar(App *app, civue_id id, civue_video *video,
                     const civue_video_info *v) {
  const Look *k = &app->look;
  double length = v->duration > 0 ? v->duration : 1;

  div(key(id), flex_1, min_w(0), h(8), flex, items_center, px(1),
      cursor_pointer) {
    civue_slider_state state = slider(v->time, 0, length, 0, 0);
    double at = state.changed ? state.value : v->time;

    if (state.changed && v->duration > 0) {
      civue_video_seek(video, state.value);
    }

    civue_rect track = content();
    double mid = (track.y0 + track.y1) / 2;
    double span = track.x1 - track.x0;
    double played = track.x0 + fmin(at / length, 1) * span;
    double loaded = track.x0 + fmin(v->buffered / length, 1) * span;
    bool hot = state.response.hovered || state.response.dragging;
    float grab =
      civue_animate(civue_ui_current(), id_in(id, "grab"), hot ? 5 : 0, .25f);

    fill(civue_rect_from(track.x0, mid - 3, track.x1, mid + 3),
         rgba(0x9799a14d), 3);
    fill(civue_rect_from(track.x0, mid - 3, loaded, mid + 3), rgba(0xffffff4d),
         3);
    fill(civue_rect_from(track.x0, mid - 3, played, mid + 3), k->brand, 3);

    if (grab > .5f) {
      fill(
        civue_rect_from(played - grab, mid - grab, played + grab, mid + grab),
        CIVUE_WHITE, grab);
    }
  }
}

// the volume: a press mutes, the slider beside it shows while hovered.
static void volume(civue_id id, civue_video *video, const civue_video_info *v) {
  civue_ui *ui = civue_ui_current();
  bool *open = civue_memory(ui, id_in(id, "open"), sizeof *open, nullptr);
  bool silent = v->muted || v->volume <= 0;

  div(key(id), flex, items_center, flex_shrink_0) {
    if (*open) {
      div(key(id_in(id, "slider")), w(px(64)), h(8), ml(1), flex, items_center,
          cursor_pointer) {
        float level = v->muted ? 0 : v->volume;
        civue_slider_state s = slider(level, 0, 1, 0, .01);

        if (s.changed) {
          level = (float)s.value;
          civue_video_set_volume(video, level);
          civue_video_set_muted(video, level <= 0);
        }

        civue_rect track = content();
        double mid = (track.y0 + track.y1) / 2;
        double x = track.x0 + level * (track.x1 - track.x0);

        fill(civue_rect_from(track.x0, mid - 3, track.x1, mid + 3),
             rgba(0x9799a14d), 3);
        fill(civue_rect_from(track.x0, mid - 3, x, mid + 3), CIVUE_WHITE, 3);
      }
    }

    civue_icon glyph_icon = silent            ? CIVUE_ICON(lucide, volume_x)
                            : v->volume < .5f ? CIVUE_ICON(lucide, volume_1)
                                              : CIVUE_ICON(lucide, volume_2);

    if (control_button(id_in(id, "button"), glyph_icon)) {
      civue_video_set_muted(video, !silent);

      if (silent && v->volume <= 0) {
        civue_video_set_volume(video, 1);
      }
    }

    *open = over(here());
  }
}

static void toggle_play(civue_video *video, const civue_video_info *v) {
  if (v->playing) {
    civue_video_pause(video);
  }
  else {
    civue_video_play(video);
  }
}

// play, the time, the bar, the volume and, when `expand`, the button that
// opens the viewer, along the bottom of the open node. true when that
// button was pressed.
static bool controls(App *app, civue_id id, civue_video *video,
                     const civue_video_info *v, bool expand, float radius) {
  bool expanded = false;
  char now[16], total[16], line[40];

  format_time(v->time, v->duration, now, sizeof now);

  if (v->duration > 0) {
    format_time(v->duration, v->duration, total, sizeof total);
  }
  else {
    snprintf(total, sizeof total, "-:--");
  }

  snprintf(line, sizeof line, "%s/%s", now, total);

  div(key(id), absolute, left(0), bottom(0), w(full), h(px(CONTROLS_HEIGHT)),
      flex, items_center, gap(1), px(1), bg(rgba(0x00000099)),
      style(.radius = {civue_px(0), civue_px(0), civue_px(radius),
                       civue_px(radius)})) {
    // a press on the bar is not one on the video under it.
    button();

    civue_icon state = v->ended     ? CIVUE_ICON(lucide, rotate_ccw)
                       : v->playing ? CIVUE_ICON(lucide, pause)
                                    : CIVUE_ICON(lucide, play);

    if (control_button(id_in(id, "play"), state)) {
      toggle_play(video, v);
    }

    if (civue_rect_w(here()) > CONTROLS_TIME_MIN) {
      text(line, flex_shrink_0, mx(1), font_mono, text_size(px(12)),
           line_height(px(16)), text_color(CIVUE_WHITE));
    }

    seek_bar(app, id_in(id, "seek"), video, v);
    volume(id_in(id, "volume"), video, v);

    if (expand &&
        control_button(id_in(id, "expand"), CIVUE_ICON(lucide, maximize))) {
      expanded = true;
    }
  }

  return expanded;
}

// whether the controls of a video over `r` show: while it is paused, and
// while the pointer moves over it.
static bool controls_shown(civue_id id, civue_rect r, const civue_video_info *v,
                           double linger) {
  civue_ui *ui = civue_ui_current();
  double *moved = civue_memory(ui, id_in(id, "moved"), sizeof *moved, nullptr);
  bool hovered = over(r);

  if (hovered && (civue_pointer_moved(ui) || *moved == 0)) {
    *moved = ui->now;
  }

  if (!v->playing || v->ended) {
    return true;
  }

  bool shown = hovered && ui->now - *moved < linger;

  if (shown) {
    civue_request_repaint_after(ui, linger - (ui->now - *moved) + .01);
  }

  return shown;
}

/* ========================================================================== */
/* the viewer's state.                                                        */
/* ========================================================================== */

static void viewer_close(App *app) {
  struct Media *m = media_of(app);

  for (size_t i = 0; i < m->count; i++) {
    player_drop(app, id_at(id("viewer-video"), i));

    for (size_t j = 0; j < countof(m->held[i].strings); j++) {
      free(m->held[i].strings[j]);
    }
  }

  free(m->held);
  m->held = nullptr;
  m->count = m->index = 0;
  m->zoomed = false;
}

// the message the media come from: one of the channel shown, or one a
// message of it forwards.
static const Message *message_of(App *app, Snowflake id) {
  Channel *c = app_channel(app);
  const Message *found = c ? store_message(c, id) : nullptr;

  for (size_t i = 0; c && !found && i < c->nmessages; i++) {
    const Message *ref = c->messages[i]->referenced;

    found = ref && ref->id == id ? ref : nullptr;
  }

  return found;
}

static void viewer_open(App *app, Snowflake message_id, const MediaItem *items,
                        size_t count, size_t index, double resume_at) {
  struct Media *m = media_of(app);
  const Message *msg = message_of(app, message_id);
  Channel *c = app_channel(app);

  viewer_close(app);
  m->held = calloc(count, sizeof *m->held);

  for (size_t i = 0; i < count; i++) {
    Held *h = &m->held[i];

    h->item = items[i];
    h->strings[0] = str_dup(items[i].url);
    h->strings[1] = str_dup(items[i].proxy_url);
    h->strings[2] = str_dup(items[i].poster);
    h->strings[3] = str_dup(items[i].filename);
    h->item.url = h->strings[0];
    h->item.proxy_url = h->strings[1];
    h->item.poster = h->strings[2];
    h->item.filename = h->strings[3];
  }

  m->count = count;
  m->index = index;
  m->message_id = message_id;
  m->author_id = msg ? msg->author_id : 0;
  m->guild_id = c ? c->guild_id : 0;
  m->sent_at = msg ? msg->timestamp : 0;
  m->author[0] = 0;

  if (msg) {
    snprintf(m->author, sizeof m->author, "%s",
             store_author(app->session->store, msg).name);
  }

  m->resume_at = resume_at;
  m->zoomed = false;
  m->opens++;
  app->popover = POPOVER_NONE;
  // the arrows and space go to the viewer, not to the composer.
  civue_ui_current()->focus = 0;
}

bool media_viewing(App *app) {
  return app->media != nullptr && app->media->count != 0;
}

/* ========================================================================== */
/* a media of a message.                                                      */
/* ========================================================================== */

static void spoiler_cover(App *app, const MediaItem *item, civue_rect r,
                          float radius) {
  char url[CDN_URL_MAX];

  // the picture under it at its smallest, too blurred to make out.
  picture_url(url, item, 8, 8, false);
  picture(app, url, r, radius, false);

  div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
      justify_center, bg(rgba(0x1e1f22cc))) {
    div(px(3), py(px(6)), rounded(full), bg(rgba(0x00000099)),
        hover(bg(rgba(0x000000e0)))) {
      text("SPOILER", text_size(px(15)), line_height(px(18)),
           font_weight(semibold), text_color(CIVUE_WHITE));
    }
  }
}

// the mark of a gif that does not move, gone under the pointer.
static void gif_badge(civue_rect r) {
  if (over(r)) {
    return;
  }

  div(absolute, left(px(6)), top(px(6)), w(px(29)), h(px(22)), flex,
      items_center, justify_center, rounded(px(3)), bg(rgba(0x202225cc))) {
    text("GIF", text_size(px(11)), line_height(px(14)), font_weight(bold),
         text_color(CIVUE_WHITE));
  }
}

static void play_badge(void) {
  div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
      justify_center) {
    div(flex, items_center, justify_center, size(12), rounded(full),
        bg(rgba(0x00000099)), hover(bg(rgba(0x000000cc))),
        text_color(CIVUE_WHITE)) {
      icon(lucide, play, size(6));
    }
  }
}

static void video_tile(App *app, civue_id id, Snowflake message_id,
                       const MediaItem *items, size_t count, size_t index,
                       civue_rect r, float radius) {
  civue_ui *ui = civue_ui_current();
  const MediaItem *item = &items[index];
  Player *p = player_find(app, id);
  char poster[CDN_URL_MAX];

  picture_url(poster, item, civue_rect_w(r), civue_rect_h(r), false);

  if (p == nullptr) {
    picture(app, poster, r, radius, false);
    play_badge();

    // among other media, a video opens the viewer as they do.
    if (button().activated) {
      if (count > 1) {
        viewer_open(app, message_id, items, count, index, -1);
      }
      else {
        player_of(app, id, message_id, item->url,
                  &(civue_video_options){.autoplay = true});
      }
    }

    return;
  }

  p = player_of(app, id, message_id, item->url, nullptr);
  civue_video *video = p->video;
  civue_video_info v = civue_video_frame(ui, video);

  if (v.texture == 0) {
    picture(app, poster, r, radius, false);
  }

  frame_of(&v, r, radius);

  if (v.state == CIVUE_VIDEO_FAILED) {
    div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
        justify_center, px(4), bg(rgba(0x000000b3))) {
      text("This video cannot be played here.", text_center, text_size(px(14)),
           text_color(CIVUE_WHITE));
    }

    if (button().activated && item->url) {
      civue_open_url(item->url);
    }

    return;
  }

  if (button().activated) {
    toggle_play(video, &v);
  }

  if (controls_shown(id, r, &v, CONTROLS_LINGER) &&
      controls(app, id_in(id, "controls"), video, &v, true, radius)) {
    civue_video_pause(video);
    viewer_open(app, message_id, items, count, index, v.time);
  }
}

static void gifv_tile(App *app, civue_id id, Snowflake message_id,
                      const MediaItem *item, civue_rect r, float radius) {
  civue_ui *ui = civue_ui_current();
  char poster[CDN_URL_MAX];
  bool play = moving();
  Player *p = player_of(
    app, id, message_id, item->proxy_url ? item->proxy_url : item->url,
    &(civue_video_options){.autoplay = play, .loop = true, .muted = true});
  civue_video_info v = p ? civue_video_frame(ui, p->video)
                         : (civue_video_info){.state = CIVUE_VIDEO_FAILED};

  if (p && v.playing != play) {
    if (play) {
      civue_video_play(p->video);
    }
    else {
      civue_video_pause(p->video);
    }
  }

  if (v.texture == 0) {
    picture_url(poster, item, civue_rect_w(r), civue_rect_h(r), false);
    picture(app, poster, r, radius, false);
  }

  frame_of(&v, r, radius);

  if (!play) {
    gif_badge(r);
  }
}

void media_tile(App *app, civue_id id, Snowflake message_id,
                const MediaItem *items, size_t count, size_t index,
                civue_vec2 size, float radius) {
  const Look *k = &app->look;
  const MediaItem *item = &items[index];
  div(key(id), relative, flex_shrink_0, w(px(size.x)), max_w(full),
      h(px(size.y)), rounded(px(radius)), bg(k->surface), overflow_hidden,
      cursor_pointer) {
    civue_rect r = here();

    if (item->spoiler && !app_revealed(app, id)) {
      spoiler_cover(app, item, r, radius);

      if (button().activated) {
        app_reveal(app, id);
      }

      continue;
    }

    switch (item->kind) {
      case MEDIA_VIDEO:
        video_tile(app, id, message_id, items, count, index, r, radius);
        break;
      case MEDIA_GIFV:
        gifv_tile(app, id, message_id, item, r, radius);

        if (button().activated) {
          viewer_open(app, message_id, items, count, index, -1);
        }

        break;
      case MEDIA_IMAGE: {
        bool play = item->animated && moving();
        char url[CDN_URL_MAX];

        picture_url(url, item, civue_rect_w(r), civue_rect_h(r), play);
        picture(app, url, r, radius, play);

        if (item->animated && !play) {
          gif_badge(r);
        }

        if (button().activated) {
          viewer_open(app, message_id, items, count, index, -1);
        }

        break;
      }
    }
  }
}

civue_vec2 media_fit(uint32_t width, uint32_t height, float max_width,
                     float max_height) {
  double w = width ? width : max_width;
  double h = height ? height : max_height;
  double scale = fmin(1, fmin(max_width / w, max_height / h));

  return point(fmax(round(w * scale), 16), fmax(round(h * scale), 16));
}

constexpr float MOSAIC_WIDTH = 550;
constexpr float MOSAIC_MAX_HEIGHT = 350;
constexpr float MOSAIC_RADIUS = 8;

// a row of a mosaic: tiles of `size` side by side.
static void mosaic_row(App *app, Snowflake message_id, const MediaItem *items,
                       const civue_id *ids, size_t n, size_t from,
                       size_t across, civue_vec2 size) {
  div(flex, gap(1), flex_shrink_0) {
    for (size_t i = from; i < from + across && i < n; i++) {
      media_tile(app, ids[i], message_id, items, n, i, size, 2);
    }
  }
}

void media_mosaic(App *app, civue_id box, Snowflake message_id,
                  const MediaItem *items, const civue_id *ids, size_t n) {
  float room = (float)last_size(box).x;
  float wide = room > 0 ? fminf(room, MOSAIC_WIDTH) : MOSAIC_WIDTH;
  float half = roundf((wide - 4) / 2), third = roundf((wide - 8) / 3);

  div(key(box), flex, flex_col, gap(1), mt(1), w(full), max_w(px(wide)),
      rounded(px(MOSAIC_RADIUS)), overflow_hidden) {
    if (n == 1) {
      civue_vec2 size =
        media_fit(items[0].width, items[0].height, wide, MOSAIC_MAX_HEIGHT);

      media_tile(app, ids[0], message_id, items, 1, 0, size, MOSAIC_RADIUS);
    }
    else if (n == 2) {
      mosaic_row(app, message_id, items, ids, n, 0, 2,
                 point(half, fminf(half, 280)));
    }
    else if (n == 3) {
      float left = roundf(2 * (wide - 4) / 3);

      div(flex, gap(1)) {
        media_tile(app, ids[0], message_id, items, n, 0, point(left, 350), 2);

        div(flex, flex_col, gap(1)) {
          for (size_t i = 1; i < 3; i++) {
            media_tile(app, ids[i], message_id, items, n, i,
                       point(wide - 4 - left, 173), 2);
          }
        }
      }
    }
    else if (n == 4) {
      mosaic_row(app, message_id, items, ids, n, 0, 2, point(half, 173));
      mosaic_row(app, message_id, items, ids, n, 2, 2, point(half, 173));
    }
    else {
      size_t lead = n % 3;

      if (lead == 1) {
        mosaic_row(app, message_id, items, ids, n, 0, 1, point(wide, 280));
      }
      else if (lead == 2) {
        mosaic_row(app, message_id, items, ids, n, 0, 2,
                   point(half, fminf(half, 280)));
      }

      for (size_t i = lead; i < n; i += 3) {
        mosaic_row(app, message_id, items, ids, n, i, 3, point(third, third));
      }
    }
  }
}

/* ========================================================================== */
/* the viewer over the window: the media as large as it fits, who sent it     */
/* and what can be done with it above, the way to the other media of the      */
/* message on its sides and under it.                                         */
/* ========================================================================== */

// the margins of its grid, and the height of its rows of buttons.
constexpr float VIEWER_MARGIN_Y = 36;
constexpr float VIEWER_MARGIN_X = 24;
constexpr float VIEWER_BAR = 40;
constexpr float THUMB = 40;

static bool viewer_action(App *app, civue_id id, civue_icon glyph_icon,
                          const char *label) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, size(8),
      rounded(md), text_color(rgb(0xdbdbde)),
      hover(bg(rgba(0xffffff14)), text_color(CIVUE_WHITE)), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    civue_node_icon(civue_ui_current(), glyph_icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20)));
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_BELOW);
  }

  return pressed;
}

// a round button 40 points across.
static bool viewer_button(App *app, civue_id id, civue_icon glyph_icon,
                          const char *label) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, size(10),
      rounded(full), bg(rgb(0x17181c)), border(1),
      border_color(rgba(0x97979f29)), text_color(rgb(0xdbdbde)),
      hover(bg(rgb(0x242529)), text_color(CIVUE_WHITE)), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    civue_node_icon(civue_ui_current(), glyph_icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20)));
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_BELOW);
  }

  return pressed;
}

typedef struct {
  bool close, zoom;
} BarResult;

// who sent it on the left, what can be done with it and the way out on the
// right.
static BarResult viewer_bar(App *app, const MediaItem *item) {
  civue_ui *ui = civue_ui_current();
  struct Media *m = media_of(app);
  const User *author = store_user(app->session->store, m->author_id);
  BarResult result = {};
  bool open = false, copy = false;
  char when[64] = "";

  if (m->sent_at) {
    format_stamp(m->sent_at, when, sizeof when);
  }

  div(absolute, left(px(VIEWER_MARGIN_X)), top(px(VIEWER_MARGIN_Y)),
      w(px(ui->size.x - 2 * VIEWER_MARGIN_X)), h(px(VIEWER_BAR)), flex,
      items_center, gap(3)) {
    div(flex, items_center, gap(3), flex_1, min_w(0)) {
      if (author) {
        avatar(app, author, m->guild_id, 40, false, CIVUE_BLACK);
      }

      div(flex, flex_col, min_w(0)) {
        text(*m->author       ? m->author
             : item->filename ? item->filename
                              : "",
             truncate, text_size(px(16)), line_height(px(20)),
             font_weight(semibold), text_color(CIVUE_WHITE));

        if (*when) {
          text(when, truncate, text_size(px(12)), line_height(px(16)),
               text_color(rgb(0xb5b6bc)));
        }
      }
    }

    div(flex, items_center, gap(1), flex_shrink_0, p(px(3)), rounded(lg),
        bg(rgb(0x17181c)), border(1), border_color(rgba(0x97979f29))) {
      if (item->kind == MEDIA_IMAGE) {
        result.zoom = viewer_action(app, id("viewer-zoom"),
                                    m->zoomed ? CIVUE_ICON(lucide, zoom_out)
                                              : CIVUE_ICON(lucide, zoom_in),
                                    m->zoomed ? "Zoom Out" : "Zoom In");
      }

      copy = viewer_action(app, id("viewer-copy"), CIVUE_ICON(lucide, link),
                           "Copy Link");
      open =
        viewer_action(app, id("viewer-open"), CIVUE_ICON(lucide, external_link),
                      "Open in Browser");
    }

    result.close =
      viewer_button(app, id("viewer-close"), CIVUE_ICON(lucide, x), "Close");
  }

  if (open && item->url) {
    civue_open_url(item->url);
  }

  if (copy && item->url) {
    civue_clipboard_set_text(item->url, strlen(item->url));
  }

  return result;
}

// where discord puts the media: the larger of what fits under the bars and
// what fits between the arrows, never larger than it is.
static civue_rect viewer_room(const MediaItem *item, bool several) {
  civue_ui *ui = civue_ui_current();
  float width = ui->size.x, height = ui->size.y;
  civue_vec2 a =
    fit(item->width, item->height,
        point(width - 2 * (several ? 76 : 24), height - 176), false);
  civue_vec2 b =
    fit(item->width, item->height,
        point(width - 544, height - 2 * (several ? 88 : 36)), false);
  civue_vec2 size = a.x >= b.x ? a : b;

  return xywh((width - size.x) / 2, (height - size.y) / 2, size.x, size.y);
}

// keeps a zoomed image over the window: centered when it fits, never
// leaving an edge empty when it does not.
static civue_vec2 clamp_pan(civue_vec2 at, civue_vec2 size) {
  civue_ui *ui = civue_ui_current();

  return point(size.x <= ui->size.x ? (ui->size.x - size.x) / 2
                                    : fmin(fmax(at.x, ui->size.x - size.x), 0),
               size.y <= ui->size.y ? (ui->size.y - size.y) / 2
                                    : fmin(fmax(at.y, ui->size.y - size.y), 0));
}

// zooms in on `at`, the point under `focus` staying under it.
static void zoom_in(struct Media *m, civue_rect at, civue_vec2 focus) {
  civue_vec2 size = point(civue_rect_w(at) * ZOOM, civue_rect_h(at) * ZOOM);

  m->zoomed = true;
  m->pan = clamp_pan(point(focus.x - (focus.x - at.x0) * ZOOM,
                           focus.y - (focus.y - at.y0) * ZOOM),
                     size);
}

// an image zoomed in, dragged or scrolled around, and zoomed out by a press
// that barely moved. one not zoomed in zooms in where it is pressed.
static void viewer_image(App *app, const MediaItem *item, civue_id id,
                         civue_rect r, const char *url, bool play) {
  civue_ui *ui = civue_ui_current();
  struct Media *m = media_of(app);
  civue_response press = civue_interact(ui, id, r, CIVUE_SENSE_DRAG);
  civue_vec2 size = point(civue_rect_w(r), civue_rect_h(r));
  // zoomed in, it is the proxy's file as it was sent.
  const char *full = item->proxy_url ? item->proxy_url : item->url;
  bool shown = m->zoomed && full && picture(app, full, r, 0, play);

  // what the chat showed stands in while the larger one loads.
  if (!shown && !picture(app, url, r, 0, play)) {
    char small[CDN_URL_MAX];

    picture_url(small, item, fminf(size.x, 550), fminf(size.y, 350), false);
    picture(app, small, r, 0, false);
  }

  if (!m->zoomed) {
    if (press.clicked && ui->input.has_mouse) {
      zoom_in(m, r, ui->input.mouse);
    }

    return;
  }

  if (press.dragging && ui->input.has_mouse) {
    m->pan = clamp_pan(point(m->pan.x + ui->input.mouse.x - ui->last_mouse.x,
                             m->pan.y + ui->input.mouse.y - ui->last_mouse.y),
                       size);
  }

  if (press.hovered && (ui->input.scroll.x != 0 || ui->input.scroll.y != 0)) {
    m->pan = clamp_pan(
      point(m->pan.x + ui->input.scroll.x, m->pan.y + ui->input.scroll.y),
      size);
  }

  if (press.released && ui->input.has_mouse &&
      hypot(ui->input.mouse.x - ui->press_origin.x,
            ui->input.mouse.y - ui->press_origin.y) < ZOOM_CLICK) {
    m->zoomed = false;
  }
}

// a video or a gif, playing at once.
static void viewer_video(App *app, const MediaItem *item, civue_id id,
                         civue_rect r, const char *poster, bool *close) {
  civue_ui *ui = civue_ui_current();
  struct Media *m = media_of(app);
  bool gifv = item->kind == MEDIA_GIFV;
  const char *source = gifv && item->proxy_url ? item->proxy_url : item->url;
  Player *p = player_of(app, id, m->message_id, source,
                        &(civue_video_options){
                          .autoplay = true,
                          .loop = gifv,
                          .muted = gifv,
                        });

  if (p == nullptr) {
    picture(app, poster, r, 0, false);
    return;
  }

  civue_video *video = p->video;

  if (m->resume_at > 0) {
    civue_video_seek(video, m->resume_at);
  }

  m->resume_at = -1;

  civue_video_info v = civue_video_frame(ui, video);

  if (v.texture == 0) {
    picture(app, poster, r, 0, false);
  }

  frame_of(&v, r, 0);

  // a gif is no more than a picture: a press on it is one outside.
  if (gifv) {
    *close |= button().activated;
    return;
  }

  if (button().activated || civue_key_pressed(ui, CIVUE_KEY_SPACE)) {
    toggle_play(video, &v);
  }

  if (controls_shown(id, r, &v, CONTROLS_LINGER_VIEWER)) {
    controls(app, id_in(id, "controls"), video, &v, false, 0);
  }
}

// the media at `at`, coming in from 90% of its size as `grow` goes to 1.
static void viewer_media(App *app, const MediaItem *item, civue_rect at,
                         float grow, bool *close) {
  struct Media *m = media_of(app);
  civue_id id = id_at(id("viewer-video"), m->index);
  bool image = item->kind == MEDIA_IMAGE;
  bool play = item->animated && moving();
  char url[CDN_URL_MAX];

  if (m->zoomed && image) {
    at = xywh(m->pan.x, m->pan.y, civue_rect_w(at) * ZOOM,
              civue_rect_h(at) * ZOOM);
  }
  else {
    float scale = .9f + .1f * grow;
    double cx = (at.x0 + at.x1) / 2, cy = (at.y0 + at.y1) / 2;
    double hw = civue_rect_w(at) * scale / 2, hh = civue_rect_h(at) * scale / 2;

    at = civue_rect_from(cx - hw, cy - hh, cx + hw, cy + hh);
  }

  picture_url(url, item, civue_rect_w(at), civue_rect_h(at), play);

  div(key(id_in(id, "media")), absolute, left(px(at.x0)), top(px(at.y0)),
      w(px(civue_rect_w(at))), h(px(civue_rect_h(at))),
      style(.cursor = !image      ? CIVUE_CURSOR_DEFAULT
                      : m->zoomed ? CIVUE_CURSOR_GRABBING
                                  : CIVUE_CURSOR_ZOOM_IN,
            .has_cursor = true)) {
    if (image) {
      viewer_image(app, item, id_in(id, "press"), here(), url, play);
    }
    else {
      viewer_video(app, item, id, here(), url, close);
    }
  }
}

static void viewer_thumbs(App *app, size_t *to) {
  civue_ui *ui = civue_ui_current();
  struct Media *m = media_of(app);
  float wide = (float)m->count * (THUMB + 2) - 2;

  div(absolute, left(px((ui->size.x - wide) / 2)),
      top(px(ui->size.y - VIEWER_MARGIN_Y - THUMB)), flex, gap(px(2)),
      rounded(lg), overflow_hidden) {
    for (size_t i = 0; i < m->count; i++) {
      const MediaItem *item = &m->held[i].item;
      char url[CDN_URL_MAX];

      div(key(id_at(id("viewer-thumb"), i)), relative, size(px(THUMB)),
          flex_shrink_0, bg(rgb(0x17181c)), cursor_pointer) {
        if (button().activated) {
          *to = i;
        }

        picture_url(url, item, THUMB, THUMB, false);
        picture(app, url, here(), 0, false);

        if (i != m->index) {
          fill(here(), rgba(0x00000080), 0);
        }
      }
    }
  }
}

void media_viewer(App *app) {
  civue_ui *ui = civue_ui_current();
  struct Media *m = media_of(app);

  if (m->count == 0) {
    return;
  }

  const MediaItem *item = &m->held[m->index].item;
  civue_id opening = id_at(id("viewer-opening"), m->opens);
  float shade = civue_animate_from(ui, id_in(opening, "shade"), 0, 1, .2f);
  float grow =
    civue_ease(civue_animate_from(ui, id_in(opening, "grow"), 0, 1, .2f));
  bool several = m->count > 1;
  civue_rect at = viewer_room(item, several);
  bool close = false, back = false, next = false, zoom = false;
  size_t to = m->index;

  app->covers_buttons = true;

  overlay() {
    div(key(id("viewer-backdrop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), bg(civue_alpha(rgba(0x000000eb), shade))) {
      close = button().activated;

      viewer_media(app, item, at, grow, &close);

      // what is around the media gives it the room while it is zoomed in.
      if (!m->zoomed) {
        BarResult bar = viewer_bar(app, item);

        close |= bar.close;
        zoom = bar.zoom;

        if (several) {
          float mid = ui->size.y / 2 - VIEWER_BAR / 2;

          div(absolute, left(px(VIEWER_MARGIN_X)), top(px(mid))) {
            back = viewer_button(app, id("viewer-back"),
                                 CIVUE_ICON(lucide, chevron_left), "Previous");
          }

          div(absolute, left(px(ui->size.x - VIEWER_MARGIN_X - VIEWER_BAR)),
              top(px(mid))) {
            next = viewer_button(app, id("viewer-next"),
                                 CIVUE_ICON(lucide, chevron_right), "Next");
          }

          viewer_thumbs(app, &to);
        }
      }
    }
  }

  close |= civue_key_pressed(ui, CIVUE_KEY_ESCAPE);

  // space plays a video, and goes on through the rest.
  if (several) {
    back |= civue_key_pressed(ui, CIVUE_KEY_LEFT) ||
            civue_key_pressed(ui, CIVUE_KEY_UP);
    next |=
      civue_key_pressed(ui, CIVUE_KEY_RIGHT) ||
      civue_key_pressed(ui, CIVUE_KEY_DOWN) ||
      (item->kind != MEDIA_VIDEO && civue_key_pressed(ui, CIVUE_KEY_SPACE));
  }

  if (zoom && m->zoomed) {
    m->zoomed = false;
  }
  else if (zoom) {
    zoom_in(m, at, point((at.x0 + at.x1) / 2, (at.y0 + at.y1) / 2));
  }

  if (back || next || to != m->index) {
    player_drop(app, id_at(id("viewer-video"), m->index));
    m->index = to != m->index ? to
               : back         ? (m->index + m->count - 1) % m->count
                              : (m->index + 1) % m->count;
    m->zoomed = false;
    m->resume_at = -1;
  }

  if (close) {
    viewer_close(app);
  }
}

void media_free(App *app) {
  if (app->media == nullptr) {
    return;
  }

  viewer_close(app);

  for (uint32_t i = 0; i < app->media->players.len; i++) {
    civue_video_free(app->media->players.v[i].video);
  }

  free(app->media->players.v);
  free(app->media);
  app->media = nullptr;
}
