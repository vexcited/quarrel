#include "images.h"

#include <stdint.h>

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

static uint64_t fetch(void *, uint64_t load, const char *url) {
  HttpHeader headers[] = {
    {"Accept", "image/webp,image/png,image/jpeg,image/gif,*/*;q=0.8"},
  };

  HttpRequest req = {
    .url = url,
    .headers = headers,
    .nheaders = 1,
    .max_response_len = IMAGES_MAX_DOWNLOAD,
  };

  return http_send(&req, on_fetched, (void *)(uintptr_t)load);
}

static void cancel(void *, uint64_t op) {
  http_cancel(op);
}

civue_images *images_new(void) {
  images = civue_images_new(&(civue_images_options){
    .fetch = fetch,
    .cancel = cancel,
    .disk_name = "quarrel",
  });
  return images;
}
