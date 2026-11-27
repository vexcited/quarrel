#include "images.h"

#include <stdcountof.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord/emoji.h"
#include "http.h"

constexpr size_t IMAGES_MAX_DOWNLOAD = 32u << 20;

// the one cache, for the answers of http that only carry a load.
static civue_images *images;

static void on_fetched(void *ctx, const HttpResponse *res) {
  uint64_t load = (uintptr_t)ctx;

  if (res->status == 200 && res->body_len != 0) {
    civue_images_fetched(images, load, res->body, res->body_len,
                         CIVUE_FETCH_OK);
    return;
  }

  // no answer or a server in trouble may work later, a 404 never will.
  bool later = res->status == 0 || res->status >= 500 || res->status == 429;

  civue_images_fetched(images, load, nullptr, 0,
                       later ? CIVUE_FETCH_LATER : CIVUE_FETCH_NEVER);
}

// an image picked on the disk, shown before it is uploaded. answered at
// once, which civue allows.
static uint64_t read_file(uint64_t load, const char *path) {
  FILE *file = fopen(path, "rb");
  long size = 0;
  void *bytes = nullptr;

  if (file != nullptr && fseek(file, 0, SEEK_END) == 0) {
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
  }

  if (size > 0 && (size_t)size <= IMAGES_MAX_DOWNLOAD) {
    bytes = malloc((size_t)size);
  }

  if (bytes != nullptr && fread(bytes, 1, (size_t)size, file) == (size_t)size) {
    civue_images_fetched(images, load, bytes, (size_t)size, CIVUE_FETCH_OK);
  }
  else {
    civue_images_fetched(images, load, nullptr, 0, CIVUE_FETCH_NEVER);
  }

  free(bytes);

  if (file != nullptr) {
    fclose(file);
  }

  return 0;
}

static uint64_t fetch(void *, uint64_t load, const char *url) {
  if (strncmp(url, "file://", 7) == 0) {
    return read_file(load, url + 7);
  }

  HttpHeader headers[] = {
    {"Accept", "image/webp,image/png,image/jpeg,image/gif,*/*;q=0.8"},
  };

  HttpRequest req = {
    .url = url,
    .headers = headers,
    .nheaders = countof(headers),
    .max_response_len = IMAGES_MAX_DOWNLOAD,
  };

  return http_send(&req, on_fetched, (void *)(uintptr_t)load);
}

static const void *held(void *, const char *url, size_t *len) {
  return emoji_svg(url, len);
}

static void cancel(void *, uint64_t op) {
  http_cancel(op);
}

civue_images *images_new(void) {
  images = civue_images_new(&(civue_images_options){
    .fetch = fetch,
    .cancel = cancel,
    .bytes_of = held,
    .disk_name = "quarrel",
    // a screen of attachments is a few tens of megabytes. what scrolled
    // away comes back from the disk cache.
    .memory_bytes = 64u << 20,
  });
  return images;
}
