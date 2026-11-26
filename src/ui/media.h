#ifndef QUARREL_UI_MEDIA_H
#define QUARREL_UI_MEDIA_H

#include "app.h"

/* ========================================================================== */
/* the pictures, gifs and videos of messages, played where they are and      */
/* opened over the window. the players live while a frame draws them.         */
/* ========================================================================== */

typedef enum : uint8_t {
  MEDIA_IMAGE,
  // a video with its sound and its controls.
  MEDIA_VIDEO,
  // a video standing for a gif: muted, looping, without controls.
  MEDIA_GIFV,
} MediaKind;

typedef struct {
  MediaKind kind;
  // the file, what the media proxy serves of it, and the picture shown
  // before a video plays, its proxy's.
  const char *url, *proxy_url, *poster;
  uint32_t width, height;
  // the name of an attachment, null for an embed's media.
  const char *filename;
  // an image whose frames move: a gif, an animated webp or png.
  bool animated;
  bool spoiler;
  // seconds an attachment's video lasts, 0 when unknown.
  double duration;
} MediaItem;

// a picture, a gif or a video of a message, `size` points, inside the open
// node, `id` telling it from the others. `items` are every media of the
// message, the one drawn being `index`: they are what the viewer goes
// through once it is pressed.
void media_tile(App *app, civue_id id, Snowflake message_id,
                const MediaItem *items, size_t count, size_t index,
                civue_vec2 size, float radius);

// the size a media of `width` by `height` takes within the most it may.
civue_vec2 media_fit(uint32_t width, uint32_t height, float max_width,
                     float max_height);
// several media as discord's mosaic lays them out in a node known by `box`,
// at most 550 points wide: alone at its own shape, two side by side, three
// as one beside two, four in a square, and more under a row of one or two
// in rows of three squares. `ids` tell the tiles apart.
void media_mosaic(App *app, civue_id box, Snowflake message_id,
                  const MediaItem *items, const civue_id *ids, size_t n);

// a value that changes while a message has a player alive, for its memo:
// a player not drawn for a while is let go.
[[nodiscard]] uint64_t media_live(App *app, Snowflake message_id);

// whether the viewer is open, the keys going to it.
[[nodiscard]] bool media_viewing(App *app);
// the media opened over the window, while one is. once a frame, last.
void media_viewer(App *app);
// lets go of the players no frame drew lately. once a frame.
void media_sweep(App *app);
// frees every player and what the viewer holds, as the app quits.
void media_free(App *app);

#endif
