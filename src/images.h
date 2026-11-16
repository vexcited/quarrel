#ifndef QUARREL_IMAGES_H
#define QUARREL_IMAGES_H

#include <civue/ui.h>

/* ========================================================================== */
/* images from urls, for avatars, icons, emoji and attachments. main thread   */
/* only.                                                                      */
/*                                                                            */
/*   Image avatar = images_get(cache, ui, url, 80);                           */
/*   if (avatar.texture != 0) civue_image(ui, avatar.texture, rect, ...);     */
/*                                                                            */
/* the first call for an url starts fetching it and answers a texture of 0.   */
/* the bytes are decoded off the main thread, uploaded on it, and the window  */
/* is woken, so the next frame gets the texture. ask again every frame: an    */
/* image nobody asked for in a while may be evicted to stay under the budget, */
/* and comes back the same way. an image never changes once loaded.           */
/* ========================================================================== */

typedef enum : uint8_t {
  IMAGE_LOADING,
  IMAGE_READY,
  // the url gave no image. it is not asked again, or only after a minute
  // when the network was what failed.
  IMAGE_FAILED,
} ImageState;

typedef struct {
  ImageState state;
  // 0 unless ready.
  civue_texture texture;
  // pixels of the texture, which may be smaller than the image at the url.
  uint32_t width, height;
} Image;

typedef struct ImageCache ImageCache;

// `max_bytes` of pixels are kept at most, 256 MiB when 0. `wake` is called
// when an image is ready to be drawn, civue_window_wake when null.
ImageCache *images_new(size_t max_bytes, void (*wake)(void));
// frees every texture. pending fetches are dropped.
void images_free(ImageCache *cache);

// the image at `url`, decoded so that its longest side is at most
// `max_pixels`, 0 for 4096. the same url at another size is another image. a
// null or empty url is IMAGE_FAILED.
Image images_get(ImageCache *cache, civue_ui *ui, const char *url,
                 uint32_t max_pixels);

// bumped whenever an image finishes loading, fails or is evicted. a memo
// that draws images has it among its inputs, since the texture it drew may
// be gone.
uint64_t images_version(const ImageCache *cache);

#endif
