#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <stdatomic.h>
#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef QUARREL_TURBOJPEG
#include <turbojpeg.h>
#endif

#include "pipe.h"
#include "platform/platform.h"
#include "voice/camera.h"

// what a call films at, as on macOS.
constexpr uint32_t WIDE = 1280, TALL = 720;

static struct {
  // on the main thread.
  civue_gfx *gfx;
  civue_texture texture, dropped;
  uint32_t width, height;
  char *filming;
  bool wanted, denied, fresh, pending;
  // counts the starts and the stops, to tell what comes from an older one.
  uint32_t run;
  void (*on_changed)(void);

  // on the loop's thread, the lock held.
  struct pw_stream *stream;
  struct spa_hook listener;
  struct spa_video_info format;
#ifdef QUARREL_TURBOJPEG
  tjhandle jpeg;
#endif

  // a picture waits for the main thread, the ones after it are dropped: the
  // loop's thread writes it while `waiting` is false, the main thread reads
  // it while it is true.
  atomic_bool waiting;
  uint8_t *pixels;
  size_t cap;
  uint32_t picture_width, picture_height;
} C;

static uint8_t clamp(int v) {
  return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

// bt.601 as webcams send it, the studio range.
static void yuv(uint8_t *out, int y, int u, int v) {
  int c = 298 * (y - 16) + 128;
  int d = u - 128, e = v - 128;

  out[0] = clamp((c + 409 * e) >> 8);
  out[1] = clamp((c - 100 * d - 208 * e) >> 8);
  out[2] = clamp((c + 516 * d) >> 8);
  out[3] = 255;
}

// the pixel at `x` of a mirrored row.
static uint8_t *mirrored(uint8_t *row, uint32_t width, uint32_t x) {
  return row + (size_t)(width - 1 - x) * 4;
}

static void from_packed_yuv(const uint8_t *src, uint32_t stride, uint32_t w,
                            uint32_t h, int y0, int u, int y1, int v) {
  for (uint32_t row = 0; row < h; row++) {
    const uint8_t *s = src + (size_t)row * stride;
    uint8_t *out = C.pixels + (size_t)row * w * 4;

    for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
      yuv(mirrored(out, w, x), s[y0], s[u], s[v]);
      yuv(mirrored(out, w, x + 1), s[y1], s[u], s[v]);
    }
  }
}

static void from_planar_yuv(const uint8_t *ys, uint32_t ystride,
                            const uint8_t *us, const uint8_t *vs,
                            uint32_t cstride, uint32_t step, uint32_t w,
                            uint32_t h) {
  for (uint32_t row = 0; row < h; row++) {
    const uint8_t *y = ys + (size_t)row * ystride;
    const uint8_t *u = us + (size_t)(row / 2) * cstride;
    const uint8_t *v = vs + (size_t)(row / 2) * cstride;
    uint8_t *out = C.pixels + (size_t)row * w * 4;

    for (uint32_t x = 0; x < w; x++) {
      yuv(mirrored(out, w, x), y[x], u[x / 2 * step], v[x / 2 * step]);
    }
  }
}

static void from_rgb(const uint8_t *src, uint32_t stride, uint32_t w,
                     uint32_t h, uint32_t size, int r, int g, int b) {
  for (uint32_t row = 0; row < h; row++) {
    const uint8_t *s = src + (size_t)row * stride;
    uint8_t *out = C.pixels + (size_t)row * w * 4;

    for (uint32_t x = 0; x < w; x++, s += size) {
      uint8_t *p = mirrored(out, w, x);

      p[0] = s[r];
      p[1] = s[g];
      p[2] = s[b];
      p[3] = 255;
    }
  }
}

static void mirror_rows(uint32_t w, uint32_t h) {
  for (uint32_t row = 0; row < h; row++) {
    uint32_t *p = (uint32_t *)(C.pixels + (size_t)row * w * 4);

    for (uint32_t a = 0, b = w - 1; a < b; a++, b--) {
      uint32_t swap = p[a];

      p[a] = p[b];
      p[b] = swap;
    }
  }
}

static bool room_for(uint32_t w, uint32_t h) {
  size_t need = (size_t)w * h * 4;

  if (need > C.cap) {
    uint8_t *grown = realloc(C.pixels, need);

    if (grown == nullptr) {
      return false;
    }

    C.pixels = grown;
    C.cap = need;
  }

  C.picture_width = w;
  C.picture_height = h;
  return true;
}

static const uint8_t *plane(const struct spa_buffer *b, uint32_t i,
                            uint32_t *stride) {
  const struct spa_data *d = &b->datas[i];

  if (d->data == nullptr || d->chunk->offset >= d->maxsize) {
    return nullptr;
  }

  *stride = d->chunk->stride > 0 ? (uint32_t)d->chunk->stride : 0;
  return SPA_PTROFF(d->data, d->chunk->offset, const uint8_t);
}

#ifdef QUARREL_TURBOJPEG
static bool from_jpeg(const struct spa_buffer *b) {
  uint32_t ignored;
  const uint8_t *jpeg = plane(b, 0, &ignored);
  size_t size = b->datas[0].chunk->size;

  if (jpeg == nullptr || size == 0) {
    return false;
  }

  if (C.jpeg == nullptr) {
    C.jpeg = tj3Init(TJINIT_DECOMPRESS);
  }

  if (C.jpeg == nullptr || tj3DecompressHeader(C.jpeg, jpeg, size) != 0) {
    return false;
  }

  int w = tj3Get(C.jpeg, TJPARAM_JPEGWIDTH);
  int h = tj3Get(C.jpeg, TJPARAM_JPEGHEIGHT);

  if (w <= 0 || h <= 0 || !room_for((uint32_t)w, (uint32_t)h) ||
      tj3Decompress8(C.jpeg, jpeg, size, C.pixels, w * 4, TJPF_RGBA) != 0) {
    return false;
  }

  mirror_rows((uint32_t)w, (uint32_t)h);
  return true;
}
#endif

// the picture as premultiplied rgba, mirrored, into the pixels.
static bool convert(const struct spa_buffer *b) {
#ifdef QUARREL_TURBOJPEG
  if (C.format.media_subtype == SPA_MEDIA_SUBTYPE_mjpg) {
    return from_jpeg(b);
  }
#endif

  if (C.format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
    return false;
  }

  uint32_t w = C.format.info.raw.size.width & ~1u;
  uint32_t h = C.format.info.raw.size.height & ~1u;
  uint32_t stride = 0;
  const uint8_t *src = plane(b, 0, &stride);

  if (src == nullptr || w == 0 || h == 0 || !room_for(w, h)) {
    return false;
  }

  // planes that come in one buffer follow each other.
  uint32_t cstride = 0;
  const uint8_t *second = nullptr, *third = nullptr;
  enum spa_video_format f = C.format.info.raw.format;
  bool planar = f == SPA_VIDEO_FORMAT_NV12 || f == SPA_VIDEO_FORMAT_I420 ||
                f == SPA_VIDEO_FORMAT_YV12;

  stride = stride ? stride : planar ? w : 0;

  if (planar && b->n_datas >= 2) {
    second = plane(b, 1, &cstride);
    third = b->n_datas >= 3 ? plane(b, 2, &cstride) : nullptr;
  }
  else if (planar) {
    second = src + (size_t)stride * h;
    cstride = f == SPA_VIDEO_FORMAT_NV12 ? stride : stride / 2;
    third = second + (size_t)cstride * (h / 2);
  }

  cstride = cstride ? cstride : f == SPA_VIDEO_FORMAT_NV12 ? w : w / 2;

  switch (f) {
    case SPA_VIDEO_FORMAT_YUY2:
      from_packed_yuv(src, stride ? stride : w * 2, w, h, 0, 1, 2, 3);
      return true;
    case SPA_VIDEO_FORMAT_UYVY:
      from_packed_yuv(src, stride ? stride : w * 2, w, h, 1, 0, 3, 2);
      return true;
    case SPA_VIDEO_FORMAT_NV12:
      if (second == nullptr) {
        return false;
      }

      from_planar_yuv(src, stride, second, second + 1, cstride, 2, w, h);
      return true;
    case SPA_VIDEO_FORMAT_I420:
    case SPA_VIDEO_FORMAT_YV12:
      if (second == nullptr || third == nullptr) {
        return false;
      }

      from_planar_yuv(src, stride, f == SPA_VIDEO_FORMAT_I420 ? second : third,
                      f == SPA_VIDEO_FORMAT_I420 ? third : second, cstride, 1,
                      w, h);
      return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
      from_rgb(src, stride ? stride : w * 4, w, h, 4, 0, 1, 2);
      return true;
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
      from_rgb(src, stride ? stride : w * 4, w, h, 4, 2, 1, 0);
      return true;
    case SPA_VIDEO_FORMAT_RGB:
      from_rgb(src, stride ? stride : w * 3, w, h, 3, 0, 1, 2);
      return true;
    case SPA_VIDEO_FORMAT_BGR:
      from_rgb(src, stride ? stride : w * 3, w, h, 3, 2, 1, 0);
      return true;
    default:
      return false;
  }
}

static void shown(void *ctx) {
  uint32_t run = (uint32_t)(uintptr_t)ctx;

  if (run != C.run || !C.wanted) {
    atomic_store(&C.waiting, false);
    return;
  }

  C.pending = true;

  if (C.on_changed != nullptr) {
    C.on_changed();
  }
}

static void on_process(void *ctx) {
  struct pw_buffer *b, *last = nullptr;

  // the newest picture is the one shown, the older ones go back at once.
  while ((b = pw_stream_dequeue_buffer(C.stream)) != nullptr) {
    if (last != nullptr) {
      pw_stream_queue_buffer(C.stream, last);
    }

    last = b;
  }

  if (last == nullptr) {
    return;
  }

  if (!atomic_load(&C.waiting) && convert(last->buffer)) {
    atomic_store(&C.waiting, true);
    platform_post(shown, ctx);
  }

  pw_stream_queue_buffer(C.stream, last);
}

static void on_param(void *, uint32_t id, const struct spa_pod *param) {
  if (param == nullptr || id != SPA_PARAM_Format) {
    return;
  }

  C.format = (struct spa_video_info){};
  spa_format_video_parse(param, &C.format);

  // buffers the cpu can read, which a jpeg stream does not get unasked.
  uint8_t buffer[256];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[] = {
    spa_pod_builder_add_object(
      &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_dataType,
      SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))),
  };

  pw_stream_update_params(C.stream, params, 1);
}

static void failed_on_main(void *ctx);

static void on_state(void *ctx, enum pw_stream_state,
                     enum pw_stream_state state, const char *error) {
  if (state == PW_STREAM_STATE_ERROR) {
    fprintf(stderr, "quarrel: the camera failed: %s\n", error ? error : "");
    platform_post(failed_on_main, ctx);
  }
}

static const struct pw_stream_events events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state,
  .param_changed = on_param,
  .process = on_process,
};

static const struct spa_pod *format(struct spa_pod_builder *b, bool jpeg,
                                    uint32_t most_wide, uint32_t most_tall,
                                    bool exact, uint32_t least_fps) {
  struct spa_pod_frame f;

  spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
    b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype,
    SPA_POD_Id(jpeg ? SPA_MEDIA_SUBTYPE_mjpg : SPA_MEDIA_SUBTYPE_raw), 0);

  if (!jpeg) {
    spa_pod_builder_add(
      b, SPA_FORMAT_VIDEO_format,
      SPA_POD_CHOICE_ENUM_Id(
        12, SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_UYVY,
        SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_I420, SPA_VIDEO_FORMAT_YV12,
        SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR),
      0);
  }

  if (exact) {
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_Rectangle(&SPA_RECTANGLE(WIDE, TALL)), 0);
  }
  else {
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(
                          &SPA_RECTANGLE(WIDE, TALL), &SPA_RECTANGLE(1, 1),
                          &SPA_RECTANGLE(most_wide, most_tall)),
                        0);
  }

  spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
                      SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(30, 1),
                                                    &SPA_FRACTION(least_fps, 1),
                                                    &SPA_FRACTION(120, 1)),
                      0);
  return spa_pod_builder_pop(b, &f);
}

// what the camera is asked for, the first it can do: 720p at a smooth rate,
// raw as it costs nothing to read, else as jpeg, else smaller or slower.
static uint32_t formats(struct spa_pod_builder *b,
                        const struct spa_pod *params[6]) {
  uint32_t n = 0;
  bool jpeg = false;

#ifdef QUARREL_TURBOJPEG
  jpeg = true;
#endif

  params[n++] = format(b, false, WIDE, TALL, true, 24);

  if (jpeg) {
    params[n++] = format(b, true, WIDE, TALL, true, 24);
  }

  params[n++] = format(b, false, WIDE, TALL, false, 15);

  if (jpeg) {
    params[n++] = format(b, true, WIDE, TALL, false, 15);
  }

  params[n++] = format(b, false, 1920, 1080, false, 0);

  if (jpeg) {
    params[n++] = format(b, true, 1920, 1080, false, 0);
  }

  return n;
}

static void film(void) {
  pipe_lock();

  defer pipe_unlock();

  struct pw_properties *props = pw_properties_new(
    PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
    PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_NODE_NAME, "quarrel-camera", nullptr);

  if (pipe_is_device(AUDIO_CAMERAS, C.filming)) {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, C.filming);
  }

  C.stream = pw_stream_new(pipe_core(), "Quarrel camera", props);

  if (C.stream == nullptr) {
    C.wanted = false;
    return;
  }

  uint8_t buffer[2048];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[6];
  uint32_t n = formats(&b, params);
  void *run = (void *)(uintptr_t)C.run;

  pw_stream_add_listener(C.stream, &C.listener, &events, run);

  if (pw_stream_connect(C.stream, SPA_DIRECTION_INPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                        params, n) < 0) {
    pw_stream_destroy(C.stream);
    C.stream = nullptr;
    C.wanted = false;
    return;
  }

  pipe_hold();
}

static void drop_texture(void *) {
  if (C.dropped != 0) {
    civue_gfx_free_texture(C.gfx, C.dropped);
    C.dropped = 0;
  }
}

// the frame being made may draw the texture, freed once it is done.
static void retire(civue_texture texture) {
  if (texture != 0) {
    drop_texture(nullptr);
    C.dropped = texture;
    platform_post(drop_texture, nullptr);
  }
}

static void shut(void) {
  C.run++;
  C.fresh = false;
  C.pending = false;

  if (C.stream != nullptr) {
    pipe_lock();
    pw_stream_destroy(C.stream);
    C.stream = nullptr;
    pipe_release();
    pipe_unlock();
  }

  atomic_store(&C.waiting, false);

  retire(C.texture);
  C.texture = 0;
}

static void failed_on_main(void *ctx) {
  if ((uint32_t)(uintptr_t)ctx != C.run || !C.wanted) {
    return;
  }

  shut();
  C.wanted = false;

  if (C.on_changed != nullptr) {
    C.on_changed();
  }
}

void camera_start(civue_gfx *g, const char *id, void (*changed)(void)) {
  if (C.wanted) {
    return;
  }

  C.wanted = true;
  C.denied = false;
  C.gfx = g;
  C.on_changed = changed;
  free(C.filming);
  C.filming = id != nullptr ? strdup(id) : nullptr;
  C.run++;

  if (pipe_start()) {
    film();
  }
  else {
    C.wanted = false;
  }

  if (!C.wanted && C.on_changed != nullptr) {
    C.on_changed();
  }
}

void camera_stop(void) {
  C.wanted = false;
  shut();
}

bool camera_running(void) {
  return C.wanted;
}

bool camera_denied(void) {
  return C.denied;
}

void camera_follow(const char *id) {
  const char *now = C.filming != nullptr ? C.filming : "";
  const char *next = id != nullptr ? id : "";

  if (C.stream == nullptr || strcmp(now, next) == 0) {
    return;
  }

  shut();
  free(C.filming);
  C.filming = id != nullptr ? strdup(id) : nullptr;
  film();
}

civue_texture camera_picture(uint32_t *width, uint32_t *height) {
  if (C.pending) {
    civue_texture made = civue_gfx_upload_image(C.gfx, C.picture_width,
                                                C.picture_height, C.pixels);

    if (made != 0) {
      retire(C.texture);
      C.texture = made;
      C.width = C.picture_width;
      C.height = C.picture_height;
      C.fresh = true;
    }

    C.pending = false;
    atomic_store(&C.waiting, false);
  }

  if (!C.fresh || C.texture == 0) {
    return 0;
  }

  *width = C.width;
  *height = C.height;
  return C.texture;
}
