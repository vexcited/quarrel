#include <initguid.h>
#include <windows.h>

#include <cfgmgr32.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice/camera.h"
#include "win.h"

/* ========================================================================== */
/* the camera through media foundation, read on a thread of its own by a      */
/* source reader that turns what the camera sends into rgb32.                 */
/* ========================================================================== */

// what a call films at, as on macOS.
constexpr uint32_t WIDE = 1280, TALL = 720;

// the cameras, as windows 10 files them.
DEFINE_GUID(CAMERAS, 0xe5323777, 0xf976, 0x4f5b, 0x9b, 0x55, 0xb9, 0x46, 0x99,
            0xc4, 0x6e, 0x44);

typedef struct {
  // the main thread's, the thread's and each post on its way.
  atomic_int refs;
  atomic_bool stop, denied;
  wchar_t *link;

  // a picture waits for the main thread, the ones after it are dropped: the
  // thread writes it while `waiting` is false, the main thread reads it
  // while it is true.
  atomic_bool waiting;
  uint8_t *pixels;
  size_t cap;
  uint32_t width, height;
} Film;

static struct {
  civue_gfx *gfx;
  civue_texture texture, dropped;
  uint32_t width, height;
  char *filming;
  bool wanted, denied, fresh, pending;
  void (*on_changed)(void);
  Film *film;
} C;

static void release(Film *f) {
  if (atomic_fetch_sub(&f->refs, 1) == 1) {
    free(f->link);
    free(f->pixels);
    free(f);
  }
}

static bool media(void) {
  static int state;

  if (state == 0) {
    state = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)) ? 1 : -1;
  }

  return state > 0;
}

// the cameras, each to release and free with CoTaskMemFree.
static UINT32 cameras(IMFActivate ***out) {
  IMFAttributes *attrs = nullptr;
  UINT32 n = 0;

  *out = nullptr;

  if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
      SUCCEEDED(attrs->lpVtbl->SetGUID(
        attrs, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
      FAILED(MFEnumDeviceSources(attrs, out, &n))) {
    *out = nullptr;
    n = 0;
  }

  if (attrs != nullptr) {
    attrs->lpVtbl->Release(attrs);
  }

  return n;
}

static void cameras_free(IMFActivate **all, UINT32 n) {
  for (UINT32 i = 0; i < n; i++) {
    all[i]->lpVtbl->Release(all[i]);
  }

  CoTaskMemFree(all);
}

static char *string_of(IMFActivate *a, const GUID *key) {
  wchar_t *value = nullptr;
  UINT32 len = 0;
  char *s = nullptr;

  if (SUCCEEDED(a->lpVtbl->GetAllocatedString(a, key, &value, &len))) {
    s = win_utf8(value);
  }

  CoTaskMemFree(value);
  return s;
}

size_t win_cameras(AudioDevice **out) {
  IMFActivate **all = nullptr;
  UINT32 n = win_com() && media() ? cameras(&all) : 0;
  AudioDevice *list = n > 0 ? calloc(n, sizeof *list) : nullptr;
  size_t kept = 0;

  for (UINT32 i = 0; list != nullptr && i < n; i++) {
    char *id = string_of(
      all[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    char *name = string_of(all[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);

    if (id != nullptr) {
      list[kept++] = (AudioDevice){id, name != nullptr ? name : strdup("")};
    }
    else {
      free(name);
    }
  }

  cameras_free(all, n);
  *out = list;
  return kept;
}

static DWORD CALLBACK on_camera(HCMNOTIFICATION, void *, CM_NOTIFY_ACTION,
                                PCM_NOTIFY_EVENT_DATA, DWORD) {
  win_devices_moved();
  return ERROR_SUCCESS;
}

void win_cameras_watch(void) {
  static HCMNOTIFICATION watching;
  CM_NOTIFY_FILTER filter = {
    .cbSize = sizeof filter,
    .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE,
    .u.DeviceInterface.ClassGuid = CAMERAS,
  };

  if (watching == nullptr) {
    CM_Register_Notification(&filter, nullptr, on_camera, &watching);
  }
}

/* ========================================================================== */
/* the thread that films.                                                     */
/* ========================================================================== */

// how far a way of filming is from what a call wants, the lowest first:
// 720p at a smooth rate, else smaller at a fair one, else up to 1080p.
static uint64_t distance(IMFMediaType *type) {
  UINT64 size = 0, rate = 0;

  if (FAILED(type->lpVtbl->GetUINT64(type, &MF_MT_FRAME_SIZE, &size))) {
    return UINT64_MAX;
  }

  type->lpVtbl->GetUINT64(type, &MF_MT_FRAME_RATE, &rate);

  uint32_t w = (uint32_t)(size >> 32), h = (uint32_t)size;
  uint32_t num = (uint32_t)(rate >> 32), den = (uint32_t)rate;
  double fps = den > 0 ? (double)num / den : 0;
  uint64_t area = (uint64_t)w * h, wanted = (uint64_t)WIDE * TALL;
  uint64_t tier = w == WIDE && h == TALL && fps >= 24 ? 0
                  : area <= wanted && fps >= 15       ? 1
                  : w <= 1920 && h <= 1080            ? 2
                                                      : 3;

  return tier << 48 | (area > wanted ? area - wanted : wanted - area);
}

static HRESULT choose_format(IMFSourceReader *r) {
  constexpr DWORD VIDEO = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
  IMFMediaType *best = nullptr, *type = nullptr;
  uint64_t closest = UINT64_MAX;

  for (DWORD i = 0;
       SUCCEEDED(r->lpVtbl->GetNativeMediaType(r, VIDEO, i, &type)); i++) {
    uint64_t d = distance(type);

    if (d < closest) {
      if (best != nullptr) {
        best->lpVtbl->Release(best);
      }

      best = type;
      closest = d;
    }
    else {
      type->lpVtbl->Release(type);
    }
  }

  if (best != nullptr) {
    r->lpVtbl->SetCurrentMediaType(r, VIDEO, nullptr, best);
    best->lpVtbl->Release(best);
  }

  // the reader turns any of them into rgb32.
  HRESULT hr = MFCreateMediaType(&type);

  if (SUCCEEDED(hr)) {
    type->lpVtbl->SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    type->lpVtbl->SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = r->lpVtbl->SetCurrentMediaType(r, VIDEO, nullptr, type);
    type->lpVtbl->Release(type);
  }

  return hr;
}

typedef struct {
  uint32_t width, height;
  // bytes from a row to the next, below zero when the rows go up.
  int32_t stride;
} Shape;

static HRESULT shape_of(IMFSourceReader *r, Shape *s) {
  IMFMediaType *type = nullptr;
  HRESULT hr = r->lpVtbl->GetCurrentMediaType(
    r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
  UINT64 size = 0;
  UINT32 stride = 0;

  if (FAILED(hr)) {
    return hr;
  }

  hr = type->lpVtbl->GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
  s->width = (uint32_t)(size >> 32);
  s->height = (uint32_t)size;
  s->stride =
    SUCCEEDED(type->lpVtbl->GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride))
      ? (int32_t)stride
      : (int32_t)(s->width * 4);
  type->lpVtbl->Release(type);
  return hr;
}

// bgrx rows into rgba ones, mirrored.
static void convert(Film *f, const uint8_t *top, LONG pitch, uint32_t w,
                    uint32_t h) {
  for (uint32_t row = 0; row < h; row++) {
    const uint8_t *s = top + (ptrdiff_t)row * pitch;
    uint8_t *out = f->pixels + (size_t)row * w * 4 + (size_t)(w - 1) * 4;

    for (uint32_t x = 0; x < w; x++, s += 4, out -= 4) {
      out[0] = s[2];
      out[1] = s[1];
      out[2] = s[0];
      out[3] = 255;
    }
  }
}

static bool room_for(Film *f, uint32_t w, uint32_t h) {
  size_t need = (size_t)w * h * 4;

  if (need > f->cap) {
    uint8_t *grown = realloc(f->pixels, need);

    if (grown == nullptr) {
      return false;
    }

    f->pixels = grown;
    f->cap = need;
  }

  f->width = w;
  f->height = h;
  return true;
}

static bool take(Film *f, IMFSample *sample, const Shape *shape) {
  IMFMediaBuffer *buffer = nullptr;
  IMF2DBuffer *flat = nullptr;
  BYTE *data = nullptr;
  LONG pitch = 0;
  DWORD len = 0;
  bool taken = false;
  uint32_t w = shape->width, h = shape->height;

  if (w == 0 || h == 0 || !room_for(f, w, h) ||
      FAILED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer))) {
    return false;
  }

  // a 2d buffer knows where its rows start, a plain one is as the format
  // says.
  if (SUCCEEDED(buffer->lpVtbl->QueryInterface(buffer, &IID_IMF2DBuffer,
                                               (void **)&flat)) &&
      SUCCEEDED(flat->lpVtbl->Lock2D(flat, &data, &pitch))) {
    convert(f, data, pitch, w, h);
    flat->lpVtbl->Unlock2D(flat);
    taken = true;
  }
  else if (SUCCEEDED(buffer->lpVtbl->Lock(buffer, &data, nullptr, &len))) {
    size_t rows = (size_t)(shape->stride < 0 ? -shape->stride : shape->stride);

    if (rows >= (size_t)w * 4 && len >= rows * h) {
      convert(f, shape->stride < 0 ? data + rows * (h - 1) : data,
              shape->stride, w, h);
      taken = true;
    }

    buffer->lpVtbl->Unlock(buffer);
  }

  if (flat != nullptr) {
    flat->lpVtbl->Release(flat);
  }

  buffer->lpVtbl->Release(buffer);
  return taken;
}

static void shown(void *ctx) {
  Film *f = ctx;

  if (f == C.film && C.wanted) {
    C.pending = true;

    if (C.on_changed != nullptr) {
      C.on_changed();
    }
  }

  release(f);
}

static void shut(void);

static void failed_on_main(void *ctx) {
  Film *f = ctx;

  if (f == C.film && C.wanted) {
    C.denied = atomic_load(&f->denied);
    shut();
    C.wanted = false;

    if (C.on_changed != nullptr) {
      C.on_changed();
    }
  }

  release(f);
}

static void post(Film *f, void (*fn)(void *)) {
  atomic_fetch_add(&f->refs, 1);
  platform_post(fn, f);
}

static IMFSourceReader *open_reader(Film *f, HRESULT *hr) {
  IMFActivate **all = nullptr;
  UINT32 n = cameras(&all), at = 0;
  IMFMediaSource *source = nullptr;
  IMFAttributes *attrs = nullptr;
  IMFSourceReader *r = nullptr;

  *hr = n > 0 ? S_OK : MF_E_NOT_FOUND;

  // the one of this id, the first when it is gone.
  for (UINT32 i = 0; f->link != nullptr && i < n; i++) {
    wchar_t *link = nullptr;
    UINT32 len = 0;

    if (SUCCEEDED(all[i]->lpVtbl->GetAllocatedString(
          all[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
          &link, &len)) &&
        _wcsicmp(link, f->link) == 0) {
      at = i;
    }

    CoTaskMemFree(link);
  }

  if (n > 0) {
    *hr = all[at]->lpVtbl->ActivateObject(all[at], &IID_IMFMediaSource,
                                          (void **)&source);
  }

  cameras_free(all, n);

  if (SUCCEEDED(*hr)) {
    *hr = MFCreateAttributes(&attrs, 2);
  }

  if (SUCCEEDED(*hr)) {
    attrs->lpVtbl->SetUINT32(attrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                             true);
    attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_DISABLE_CONVERTERS, false);
    *hr = MFCreateSourceReaderFromMediaSource(source, attrs, &r);
  }

  if (SUCCEEDED(*hr)) {
    *hr = choose_format(r);
  }

  if (attrs != nullptr) {
    attrs->lpVtbl->Release(attrs);
  }

  // the reader holds the source, which is shut down with it.
  if (FAILED(*hr) && source != nullptr) {
    source->lpVtbl->Shutdown(source);
  }

  if (source != nullptr) {
    source->lpVtbl->Release(source);
  }

  if (FAILED(*hr) && r != nullptr) {
    r->lpVtbl->Release(r);
    r = nullptr;
  }

  return r;
}

static DWORD WINAPI run(void *arg) {
  Film *f = arg;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMFSourceReader *r = SUCCEEDED(hr) ? open_reader(f, &hr) : nullptr;
  Shape shape = {};

  if (r != nullptr) {
    hr = shape_of(r, &shape);
  }

  while (SUCCEEDED(hr) && !atomic_load(&f->stop)) {
    DWORD stream = 0, flags = 0;
    LONGLONG at = 0;
    IMFSample *sample = nullptr;

    hr = r->lpVtbl->ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               &stream, &flags, &at, &sample);

    if (SUCCEEDED(hr) &&
        flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
      hr = MF_E_END_OF_STREAM;
    }

    if (SUCCEEDED(hr) && flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      hr = shape_of(r, &shape);
    }

    if (SUCCEEDED(hr) && sample != nullptr && !atomic_load(&f->stop) &&
        !atomic_load(&f->waiting) && take(f, sample, &shape)) {
      atomic_store(&f->waiting, true);
      post(f, shown);
    }

    if (sample != nullptr) {
      sample->lpVtbl->Release(sample);
    }
  }

  if (FAILED(hr) && !atomic_load(&f->stop)) {
    fprintf(stderr, "quarrel: the camera failed: 0x%08lx\n", (unsigned long)hr);
    atomic_store(&f->denied, hr == E_ACCESSDENIED);
    post(f, failed_on_main);
  }

  if (r != nullptr) {
    r->lpVtbl->Release(r);
  }

  CoUninitialize();
  release(f);
  return 0;
}

/* ========================================================================== */
/* the main thread's side.                                                    */
/* ========================================================================== */

static void film(void) {
  Film *f = calloc(1, sizeof *f);
  HANDLE thread = nullptr;

  if (f == nullptr || !media()) {
    free(f);
    C.wanted = false;
    return;
  }

  f->link = win_wide(C.filming);
  atomic_init(&f->refs, 2);
  thread = CreateThread(nullptr, 0, run, f, 0, nullptr);

  if (thread == nullptr) {
    free(f->link);
    free(f);
    C.wanted = false;
    return;
  }

  CloseHandle(thread);
  C.film = f;
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

// the thread ends at the next picture, on its own.
static void shut(void) {
  C.fresh = false;
  C.pending = false;

  if (C.film != nullptr) {
    atomic_store(&C.film->stop, true);
    release(C.film);
    C.film = nullptr;
  }

  retire(C.texture);
  C.texture = 0;
}

void camera_start(civue_gfx *g, const char *id, void (*changed)(void)) {
  if (C.wanted) {
    return;
  }

  C.wanted = true;
  C.gfx = g;
  C.on_changed = changed;
  free(C.filming);
  C.filming = id != nullptr ? strdup(id) : nullptr;
  // the camera then answers E_ACCESSDENIED, after a moment.
  C.denied = win_denied(L"webcam");

  if (!C.denied && win_com()) {
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

  if (C.film == nullptr || strcmp(now, next) == 0) {
    return;
  }

  shut();
  free(C.filming);
  C.filming = id != nullptr ? strdup(id) : nullptr;
  film();
}

civue_texture camera_picture(uint32_t *width, uint32_t *height) {
  Film *f = C.film;

  if (C.pending && f != nullptr) {
    civue_texture made =
      civue_gfx_upload_image(C.gfx, f->width, f->height, f->pixels);

    if (made != 0) {
      retire(C.texture);
      C.texture = made;
      C.width = f->width;
      C.height = f->height;
      C.fresh = true;
    }

    C.pending = false;
    atomic_store(&f->waiting, false);
  }

  if (!C.fresh || C.texture == 0) {
    return 0;
  }

  *width = C.width;
  *height = C.height;
  return C.texture;
}
