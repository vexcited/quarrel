#include "images.h"

#include <civue/window.h>
#include <cilo/map.h>

#include "http.h"
#include "platform/platform.h"

constexpr size_t IMAGES_DEFAULT_BYTES = 256u << 20;
constexpr uint32_t IMAGES_MAX_PIXELS = 4096;
// an image asked for this recently may still be in a frame being shown.
constexpr double IMAGES_KEEP_SECONDS = 2;
constexpr double IMAGES_RETRY_SECONDS = 60;
constexpr size_t IMAGES_MAX_DOWNLOAD = 32u << 20;

typedef struct Entry Entry;

struct Entry {
  ImageCache *cache;
  char *url;
  uint32_t max_pixels;
  uint64_t key;
  ImageState state;
  civue_texture texture;
  uint32_t width, height;
  size_t bytes;
  HttpOp fetch;
  PlatformOp decode;
  // unix seconds of the last images_get, and of a failure worth retrying.
  double used_at, retry_at;
  // most recently asked first.
  Entry *newer, *older;
};

struct ImageCache {
  CILO_MAP(uint64_t, Entry *) entries;
  Entry *newest, *oldest;
  civue_gfx *gfx;
  size_t bytes, max_bytes;
  uint64_t version;
  void (*wake)(void);
};

static uint64_t key_of(const char *url, uint32_t max_pixels) {
  return cilo_hash_str(url) ^ cilo_hash_u64(max_pixels);
}

static void unlink_entry(ImageCache *c, Entry *e) {
  if (e->newer != nullptr) {
    e->newer->older = e->older;
  }
  else if (c->newest == e) {
    c->newest = e->older;
  }

  if (e->older != nullptr) {
    e->older->newer = e->newer;
  }
  else if (c->oldest == e) {
    c->oldest = e->newer;
  }

  e->newer = e->older = nullptr;
}

static void link_newest(ImageCache *c, Entry *e) {
  e->older = c->newest;

  if (c->newest != nullptr) {
    c->newest->newer = e;
  }

  c->newest = e;

  if (c->oldest == nullptr) {
    c->oldest = e;
  }
}

static void drop_entry(ImageCache *c, Entry *e) {
  http_cancel(e->fetch);
  platform_cancel(e->decode);

  if (e->texture != 0 && c->gfx != nullptr) {
    civue_gfx_free_texture(c->gfx, e->texture);
  }

  c->bytes -= e->bytes;
  unlink_entry(c, e);
  CILO_MAP_DEL(&c->entries, e->key);
  free(e->url);
  free(e);
}

// frees the images asked for longest ago until the budget holds.
static void evict(ImageCache *c) {
  double now = platform_now();
  Entry *e = c->oldest;

  while (e != nullptr && c->bytes > c->max_bytes) {
    Entry *next = e->newer;

    if (now - e->used_at < IMAGES_KEEP_SECONDS) {
      break;
    }

    if (e->state == IMAGE_READY) {
      drop_entry(c, e);
      c->version++;
    }

    e = next;
  }
}

static void fail(Entry *e, bool retry) {
  e->state = IMAGE_FAILED;
  e->retry_at = retry ? platform_now() + IMAGES_RETRY_SECONDS : 0;
  e->cache->version++;
  e->cache->wake();
}

static void on_decoded(void *ctx, const PlatformImage *image) {
  Entry *e = ctx;
  ImageCache *c = e->cache;

  e->decode = 0;

  if (image == nullptr || c->gfx == nullptr) {
    fail(e, false);
    return;
  }

  e->texture =
    civue_gfx_upload_image(c->gfx, image->width, image->height, image->rgba);

  if (e->texture == 0) {
    fail(e, false);
    return;
  }

  e->state = IMAGE_READY;
  e->width = image->width;
  e->height = image->height;
  e->bytes = (size_t)image->width * image->height * 4;
  c->bytes += e->bytes;
  c->version++;

  evict(c);
  c->wake();
}

static void on_fetched(void *ctx, const HttpResponse *res) {
  Entry *e = ctx;

  e->fetch = 0;

  if (res->status != 200 || res->body_len == 0) {
    // no answer or a server in trouble may work later, a 404 never will.
    fail(e, res->status == 0 || res->status >= 500 || res->status == 429);
    return;
  }

  e->decode = platform_image_decode(res->body, res->body_len, e->max_pixels,
                                    on_decoded, e);
}

static void start(Entry *e) {
  HttpHeader headers[] = {
    {"Accept", "image/webp,image/png,image/jpeg,image/gif,*/*;q=0.8"},
  };

  HttpRequest req = {
    .url = e->url,
    .headers = headers,
    .nheaders = 1,
    .max_response_len = IMAGES_MAX_DOWNLOAD,
  };

  e->state = IMAGE_LOADING;
  e->fetch = http_send(&req, on_fetched, e);

  if (e->fetch == 0) {
    fail(e, true);
  }
}

ImageCache *images_new(size_t max_bytes, void (*wake)(void)) {
  ImageCache *c = calloc(1, sizeof *c);

  if (c == nullptr) {
    cilo_oom(sizeof *c);
  }

  c->max_bytes = max_bytes != 0 ? max_bytes : IMAGES_DEFAULT_BYTES;
  c->wake = wake != nullptr ? wake : civue_window_wake;
  return c;
}

void images_free(ImageCache *c) {
  if (c == nullptr) {
    return;
  }

  while (c->newest != nullptr) {
    drop_entry(c, c->newest);
  }

  CILO_MAP_DROP(&cilo_heap, &c->entries);
  free(c);
}

Image images_get(ImageCache *c, civue_ui *ui, const char *url,
                 uint32_t max_pixels) {
  if (c == nullptr || url == nullptr || url[0] == '\0') {
    return (Image){.state = IMAGE_FAILED};
  }

  if (ui != nullptr && ui->gfx != nullptr) {
    c->gfx = ui->gfx;
  }

  if (max_pixels == 0 || max_pixels > IMAGES_MAX_PIXELS) {
    max_pixels = IMAGES_MAX_PIXELS;
  }

  uint64_t key = key_of(url, max_pixels);
  double now = platform_now();
  bool created = false;
  Entry **slot = CILO_MAP_PUT(&cilo_heap, &c->entries, key, &created);

  if (created) {
    Entry *e = calloc(1, sizeof *e);

    if (e == nullptr) {
      cilo_oom(sizeof *e);
    }

    e->cache = c;
    e->url = strdup(url);
    e->max_pixels = max_pixels;
    e->key = key;
    *slot = e;

    link_newest(c, e);
    start(e);
  }

  Entry *e = *slot;

  if (c->newest != e) {
    unlink_entry(c, e);
    link_newest(c, e);
  }

  e->used_at = now;

  if (e->state == IMAGE_FAILED && e->retry_at != 0 && now >= e->retry_at) {
    e->retry_at = 0;
    start(e);
  }

  return (Image){e->state, e->texture, e->width, e->height};
}

uint64_t images_version(const ImageCache *c) {
  return c != nullptr ? c->version : 0;
}
