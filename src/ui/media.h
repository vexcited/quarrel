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

// a value that changes while a message has a player alive, for its memo:
// a player not drawn for a while is let go.
uint64_t media_live(App *app, Snowflake message_id);

// whether the viewer is open, the keys going to it.
bool media_viewing(App *app);
// the media opened over the window, while one is. once a frame, last.
void media_viewer(App *app);
// lets go of the players no frame drew lately. once a frame.
void media_sweep(App *app);
// frees every player and what the viewer holds, as the app quits.
void media_free(App *app);

#endif
