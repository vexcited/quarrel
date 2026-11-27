#include <initguid.h>
#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice/audio.h"
#include "win.h"

/* ========================================================================== */
/* sound through wasapi, each way on a thread of its own at the priority of   */
/* mmcss, shared with the other apps as mono floats at 48 kHz, which windows  */
/* converts from the device's own format.                                     */
/*                                                                            */
/* echo is taken out by the system as it is for every call app: the          */
/* communications category puts the device's voice processing in the path of  */
/* the microphone, echo cancellation among it, and windows 11 brings its own  */
/* where the device has none. there the canceller is also told which         */
/* speakers to take out, the ones the call plays to, not only the default     */
/* ones. the voice capture dsp would cancel on any windows, but at 16 kHz and */
/* through capture of its own, the devices picked by index.                   */
/* ========================================================================== */

// the most a callback is handed at a time.
constexpr UINT32 FRAMES_MOST = 4096;
// what the streams ask the engine for, in hundreds of nanoseconds: 20 ms,
// which the engine serves every 10.
constexpr REFERENCE_TIME BUFFER = 200'000;
// a stream that loses its device opens again on the system's own, this
// many times.
constexpr int REOPENS_MOST = 8;

// windows 11's, which mingw's headers have not.
typedef struct EchoControl EchoControl;

typedef struct {
  HRESULT(STDMETHODCALLTYPE *QueryInterface)(EchoControl *, REFIID, void **);
  ULONG(STDMETHODCALLTYPE *AddRef)(EchoControl *);
  ULONG(STDMETHODCALLTYPE *Release)(EchoControl *);
  HRESULT(STDMETHODCALLTYPE *SetEchoCancellationRenderEndpoint)(EchoControl *,
                                                                LPCWSTR id);
} EchoControlVtbl;

struct EchoControl {
  const EchoControlVtbl *lpVtbl;
};

DEFINE_GUID(IID_EchoControl, 0xf4ae25b5, 0xaaa3, 0x437d, 0xb6, 0xb3, 0xdb, 0xbe,
            0x2d, 0x0e, 0x95, 0x49);

struct Audio {
  AudioEvents e;
  // audio_stop was called: the callbacks touch nothing of their owner's
  // anymore, and `busy` counts the ones that still do.
  atomic_bool stopped;
  atomic_int busy;
  atomic_bool live, failed;
  // the owner's, each stream's and a failure on its way.
  atomic_int refs;
  wchar_t *input, *output;
  bool raw;
  HANDLE stop;
};

typedef struct {
  Audio *a;
  bool capturing;
} Stream;

static bool enter(Audio *a) {
  atomic_fetch_add(&a->busy, 1);

  if (atomic_load(&a->stopped)) {
    atomic_fetch_sub(&a->busy, 1);
    return false;
  }

  return true;
}

static void leave(Audio *a) {
  atomic_fetch_sub(&a->busy, 1);
}

static void release(Audio *a) {
  if (atomic_fetch_sub(&a->refs, 1) == 1) {
    CloseHandle(a->stop);
    free(a->input);
    free(a->output);
    free(a);
  }
}

// a desktop app is asked nothing, the user allows them all or none in the
// settings: a start that is refused says so.
void audio_ask(void (*done)(void *ctx, bool allowed), void *ctx) {
  done(ctx, true);
}

// the system has no menu of its own for how the microphone is shaped.
void audio_show_modes(void) {
}

static void failed_on_main(void *ctx) {
  Audio *a = ctx;

  if (!atomic_load(&a->stopped) && a->e.failed != nullptr) {
    a->e.failed(a->e.ctx);
  }

  release(a);
}

static void fail(Audio *a, const char *what, HRESULT hr) {
  fprintf(stderr, "quarrel: %s failed: 0x%08lx\n", what, (unsigned long)hr);

  if (!atomic_exchange(&a->failed, true)) {
    atomic_fetch_add(&a->refs, 1);
    platform_post(failed_on_main, a);
  }
}

static IMMDeviceEnumerator *enumerator(void) {
  IMMDeviceEnumerator *e = nullptr;

  if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                              &IID_IMMDeviceEnumerator, (void **)&e))) {
    return nullptr;
  }

  return e;
}

// the device of this id when it is there and on, the system's own else.
static IMMDevice *device_of(IMMDeviceEnumerator *e, EDataFlow flow,
                            const wchar_t *id) {
  IMMDevice *d = nullptr;
  DWORD state = 0;

  if (id != nullptr && SUCCEEDED(e->lpVtbl->GetDevice(e, id, &d)) &&
      SUCCEEDED(d->lpVtbl->GetState(d, &state)) &&
      state == DEVICE_STATE_ACTIVE) {
    return d;
  }

  if (d != nullptr) {
    d->lpVtbl->Release(d);
    d = nullptr;
  }

  if (FAILED(e->lpVtbl->GetDefaultAudioEndpoint(e, flow, eConsole, &d))) {
    return nullptr;
  }

  return d;
}

typedef struct {
  IAudioClient2 *client;
  IAudioCaptureClient *capture;
  IAudioRenderClient *render;
  HANDLE ready;
  UINT32 frames;
} Open;

static void close_stream(Open *o) {
  if (o->client != nullptr) {
    o->client->lpVtbl->Stop(o->client);
  }

  if (o->capture != nullptr) {
    o->capture->lpVtbl->Release(o->capture);
  }

  if (o->render != nullptr) {
    o->render->lpVtbl->Release(o->render);
  }

  if (o->client != nullptr) {
    o->client->lpVtbl->Release(o->client);
  }

  *o = (Open){.ready = o->ready};
}

// the canceller of the microphone takes out what goes to these speakers.
static void point_echo(Audio *a, IMMDeviceEnumerator *e, IAudioClient *c) {
  EchoControl *echo = nullptr;
  IMMDevice *speakers = device_of(e, eRender, a->output);
  wchar_t *id = nullptr;

  if (speakers != nullptr &&
      SUCCEEDED(speakers->lpVtbl->GetId(speakers, &id)) &&
      SUCCEEDED(c->lpVtbl->GetService(c, &IID_EchoControl, (void **)&echo))) {
    echo->lpVtbl->SetEchoCancellationRenderEndpoint(echo, id);
    echo->lpVtbl->Release(echo);
  }

  CoTaskMemFree(id);

  if (speakers != nullptr) {
    speakers->lpVtbl->Release(speakers);
  }
}

// windows lowers the other apps' sound during a call unasked, which Quarrel
// does on no other system.
static void keep_others(IAudioClient *c) {
  IAudioSessionControl *session = nullptr;
  IAudioSessionControl2 *own = nullptr;

  if (FAILED(c->lpVtbl->GetService(c, &IID_IAudioSessionControl,
                                   (void **)&session))) {
    return;
  }

  if (SUCCEEDED(session->lpVtbl->QueryInterface(
        session, &IID_IAudioSessionControl2, (void **)&own))) {
    own->lpVtbl->SetDuckingPreference(own, true);
    own->lpVtbl->Release(own);
  }

  session->lpVtbl->Release(session);
}

static HRESULT open_stream(Stream *s, IMMDeviceEnumerator *e, Open *o) {
  Audio *a = s->a;
  IMMDevice *d = device_of(e, s->capturing ? eCapture : eRender,
                           s->capturing ? a->input : a->output);

  if (d == nullptr) {
    return E_NOTFOUND;
  }

  HRESULT hr = d->lpVtbl->Activate(d, &IID_IAudioClient2, CLSCTX_ALL, nullptr,
                                   (void **)&o->client);

  d->lpVtbl->Release(d);

  if (FAILED(hr)) {
    o->client = nullptr;
    return hr;
  }

  IAudioClient *c = (IAudioClient *)o->client;
  AudioClientProperties props = {
    .cbSize = sizeof props,
    .eCategory = a->raw ? AudioCategory_Other : AudioCategory_Communications,
    .Options = a->raw && s->capturing ? AUDCLNT_STREAMOPTIONS_RAW
                                      : AUDCLNT_STREAMOPTIONS_NONE,
  };

  // a microphone without a raw mode processes as it does for any app.
  if (FAILED(o->client->lpVtbl->SetClientProperties(o->client, &props))) {
    props.Options = AUDCLNT_STREAMOPTIONS_NONE;
    o->client->lpVtbl->SetClientProperties(o->client, &props);
  }

  WAVEFORMATEX format = {
    .wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
    .nChannels = 1,
    .nSamplesPerSec = AUDIO_RATE,
    .nAvgBytesPerSec = AUDIO_RATE * sizeof(float),
    .nBlockAlign = sizeof(float),
    .wBitsPerSample = 32,
  };

  hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             BUFFER, 0, &format, nullptr);

  if (SUCCEEDED(hr)) {
    hr = c->lpVtbl->SetEventHandle(c, o->ready);
  }

  if (SUCCEEDED(hr)) {
    hr = c->lpVtbl->GetBufferSize(c, &o->frames);
  }

  if (SUCCEEDED(hr)) {
    hr = s->capturing ? c->lpVtbl->GetService(c, &IID_IAudioCaptureClient,
                                              (void **)&o->capture)
                      : c->lpVtbl->GetService(c, &IID_IAudioRenderClient,
                                              (void **)&o->render);
  }

  if (SUCCEEDED(hr) && s->capturing && !a->raw) {
    point_echo(a, e, c);
  }

  if (SUCCEEDED(hr)) {
    keep_others(c);
  }

  // the speakers start with silence, not with what the buffer held.
  BYTE *silence = nullptr;

  if (SUCCEEDED(hr) && !s->capturing &&
      SUCCEEDED(o->render->lpVtbl->GetBuffer(o->render, o->frames, &silence))) {
    o->render->lpVtbl->ReleaseBuffer(o->render, o->frames,
                                     AUDCLNT_BUFFERFLAGS_SILENT);
  }

  if (SUCCEEDED(hr)) {
    hr = c->lpVtbl->Start(c);
  }

  if (FAILED(hr)) {
    close_stream(o);
  }

  return hr;
}

static HRESULT capture(Audio *a, Open *o) {
  static const float quiet[FRAMES_MOST];
  UINT32 next = 0;
  HRESULT hr;

  while (
    SUCCEEDED(hr = o->capture->lpVtbl->GetNextPacketSize(o->capture, &next)) &&
    next > 0) {
    BYTE *data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;

    hr = o->capture->lpVtbl->GetBuffer(o->capture, &data, &frames, &flags,
                                       nullptr, nullptr);

    if (FAILED(hr)) {
      return hr;
    }

    if (frames > 0 && enter(a)) {
      const float *heard =
        flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : (const float *)data;

      atomic_store_explicit(&a->live, true, memory_order_relaxed);

      for (UINT32 at = 0; at < frames; at += FRAMES_MOST) {
        UINT32 n = frames - at < FRAMES_MOST ? frames - at : FRAMES_MOST;

        a->e.captured(a->e.ctx, heard != nullptr ? heard + at : quiet, n);
      }

      leave(a);
    }

    o->capture->lpVtbl->ReleaseBuffer(o->capture, frames);
  }

  return hr;
}

static HRESULT render(Audio *a, Open *o) {
  IAudioClient *c = (IAudioClient *)o->client;
  UINT32 padding = 0;
  BYTE *data = nullptr;
  HRESULT hr = c->lpVtbl->GetCurrentPadding(c, &padding);
  UINT32 frames = o->frames - padding;

  if (FAILED(hr) || frames == 0) {
    return hr;
  }

  hr = o->render->lpVtbl->GetBuffer(o->render, frames, &data);

  if (FAILED(hr)) {
    return hr;
  }

  float *out = (float *)data;

  if (enter(a)) {
    for (UINT32 at = 0; at < frames; at += FRAMES_MOST) {
      UINT32 n = frames - at < FRAMES_MOST ? frames - at : FRAMES_MOST;

      a->e.render(a->e.ctx, out + at, n);
    }

    leave(a);
  }
  else {
    memset(out, 0, frames * sizeof *out);
  }

  return o->render->lpVtbl->ReleaseBuffer(o->render, frames, 0);
}

static DWORD WINAPI run(void *arg) {
  Stream s = *(Stream *)arg;
  Audio *a = s.a;
  DWORD task_index = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  IMMDeviceEnumerator *e = nullptr;
  Open o = {.ready = CreateEventW(nullptr, false, false, nullptr)};
  const char *what = s.capturing ? "the microphone" : "the speakers";

  free(arg);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  e = enumerator();

  HRESULT hr =
    e != nullptr && o.ready != nullptr ? open_stream(&s, e, &o) : E_FAIL;

  // a device that went, or changed its format, is opened again.
  for (int reopens = 0; SUCCEEDED(hr) && !atomic_load(&a->stopped);) {
    HANDLE waits[] = {a->stop, o.ready};
    DWORD woke = WaitForMultipleObjects(2, waits, false, 2000);

    if (woke == WAIT_OBJECT_0) {
      break;
    }

    if (woke == WAIT_TIMEOUT) {
      continue;
    }

    hr = s.capturing ? capture(a, &o) : render(a, &o);

    if (hr == AUDCLNT_E_DEVICE_INVALIDATED && reopens++ < REOPENS_MOST) {
      close_stream(&o);
      hr = open_stream(&s, e, &o);
    }
  }

  if (FAILED(hr) && !atomic_load(&a->stopped)) {
    fail(a, what, hr);
  }

  close_stream(&o);

  if (e != nullptr) {
    e->lpVtbl->Release(e);
  }

  if (o.ready != nullptr) {
    CloseHandle(o.ready);
  }

  if (task != nullptr) {
    AvRevertMmThreadCharacteristics(task);
  }

  CoUninitialize();
  release(a);
  return 0;
}

static bool start_stream(Audio *a, bool capturing) {
  Stream *s = malloc(sizeof *s);
  HANDLE thread = nullptr;

  if (s == nullptr) {
    return false;
  }

  *s = (Stream){a, capturing};
  atomic_fetch_add(&a->refs, 1);
  thread = CreateThread(nullptr, 0, run, s, 0, nullptr);

  if (thread == nullptr) {
    free(s);
    atomic_fetch_sub(&a->refs, 1);
    return false;
  }

  CloseHandle(thread);
  return true;
}

Audio *audio_start(const AudioEvents *e, const char *input, const char *output,
                   bool raw, AudioResult *why) {
  *why = AUDIO_FAILED;

  // wasapi says so once the stream is asked for, which comes after.
  if (win_denied(L"microphone")) {
    *why = AUDIO_DENIED;
    return nullptr;
  }

  Audio *a = calloc(1, sizeof *a);

  if (a == nullptr) {
    return nullptr;
  }

  a->e = *e;
  a->input = win_wide(input);
  a->output = win_wide(output);
  a->raw = raw;
  a->stop = CreateEventW(nullptr, true, false, nullptr);
  atomic_init(&a->refs, 1);

  if (a->stop == nullptr) {
    release(a);
    return nullptr;
  }

  bool started = start_stream(a, true) && start_stream(a, false);

  if (!started) {
    audio_stop(a);
    return nullptr;
  }

  *why = AUDIO_OK;
  return a;
}

bool audio_live(const Audio *a) {
  return atomic_load_explicit(&a->live, memory_order_relaxed);
}

void audio_stop(Audio *a) {
  if (a == nullptr) {
    return;
  }

  atomic_store(&a->stopped, true);
  SetEvent(a->stop);

  // a callback that already started ends within a few milliseconds.
  while (atomic_load(&a->busy) != 0) {
    SwitchToThread();
  }

  release(a);
}

/* ========================================================================== */
/* the devices, by the ids of their endpoints, which stay from a launch to    */
/* the next.                                                                  */
/* ========================================================================== */

static char *name_of(IMMDevice *d) {
  IPropertyStore *props = nullptr;
  PROPVARIANT name;
  char *named = nullptr;

  PropVariantInit(&name);

  if (SUCCEEDED(d->lpVtbl->OpenPropertyStore(d, STGM_READ, &props)) &&
      SUCCEEDED(
        props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &name)) &&
      name.vt == VT_LPWSTR) {
    named = win_utf8(name.pwszVal);
  }

  PropVariantClear(&name);

  if (props != nullptr) {
    props->lpVtbl->Release(props);
  }

  return named;
}

size_t audio_devices(AudioKind kind, AudioDevice **out) {
  *out = nullptr;

  if (kind == AUDIO_CAMERAS) {
    return win_cameras(out);
  }

  IMMDeviceEnumerator *e = win_com() ? enumerator() : nullptr;
  IMMDeviceCollection *all = nullptr;
  UINT count = 0;
  size_t n = 0;

  if (e == nullptr) {
    return 0;
  }

  if (SUCCEEDED(e->lpVtbl->EnumAudioEndpoints(
        e, kind == AUDIO_INPUTS ? eCapture : eRender, DEVICE_STATE_ACTIVE,
        &all)) &&
      SUCCEEDED(all->lpVtbl->GetCount(all, &count)) && count > 0) {
    AudioDevice *list = calloc(count, sizeof *list);

    for (UINT i = 0; list != nullptr && i < count; i++) {
      IMMDevice *d = nullptr;
      wchar_t *id = nullptr;

      if (FAILED(all->lpVtbl->Item(all, i, &d))) {
        continue;
      }

      if (SUCCEEDED(d->lpVtbl->GetId(d, &id))) {
        char *name = name_of(d);

        list[n++] = (AudioDevice){win_utf8(id), name ? name : strdup("")};
      }

      CoTaskMemFree(id);
      d->lpVtbl->Release(d);
    }

    *out = list;
  }

  if (all != nullptr) {
    all->lpVtbl->Release(all);
  }

  e->lpVtbl->Release(e);
  return n;
}

void audio_devices_free(AudioDevice *devices, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(devices[i].id);
    free(devices[i].name);
  }

  free(devices);
}

void audio_default_name(AudioKind kind, char *out, size_t cap) {
  out[0] = '\0';

  // a camera is seldom made the default, the first one is then.
  if (kind == AUDIO_CAMERAS) {
    AudioDevice *cameras = nullptr;
    size_t n = win_cameras(&cameras);

    if (n > 0) {
      snprintf(out, cap, "%s", cameras[0].name);
    }

    audio_devices_free(cameras, n);
    return;
  }

  IMMDeviceEnumerator *e = win_com() ? enumerator() : nullptr;
  IMMDevice *d = nullptr;

  if (e != nullptr &&
      SUCCEEDED(e->lpVtbl->GetDefaultAudioEndpoint(
        e, kind == AUDIO_INPUTS ? eCapture : eRender, eConsole, &d))) {
    char *name = name_of(d);

    snprintf(out, cap, "%s", name ? name : "");
    free(name);
    d->lpVtbl->Release(d);
  }

  if (e != nullptr) {
    e->lpVtbl->Release(e);
  }
}

/* ========================================================================== */
/* what comes and goes, told on threads of the system's and once to the main  */
/* thread however many changes came before it looked.                         */
/* ========================================================================== */

static uint32_t devices_version;
static void (*devices_changed)(void);
static atomic_bool moved_posted;

static void moved_on_main(void *) {
  atomic_store(&moved_posted, false);
  devices_version++;

  if (devices_changed != nullptr) {
    devices_changed();
  }
}

void win_devices_moved(void) {
  if (!atomic_exchange(&moved_posted, true)) {
    platform_post(moved_on_main, nullptr);
  }
}

static HRESULT STDMETHODCALLTYPE watcher_query(IMMNotificationClient *self,
                                               REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_IMMNotificationClient)) {
    *out = self;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

// the one watcher lasts as long as the process.
static ULONG STDMETHODCALLTYPE watcher_retain(IMMNotificationClient *) {
  return 1;
}

static HRESULT STDMETHODCALLTYPE on_state(IMMNotificationClient *, LPCWSTR,
                                          DWORD) {
  win_devices_moved();
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE on_device(IMMNotificationClient *, LPCWSTR) {
  win_devices_moved();
  return S_OK;
}

// the system's own changes for each of its roles, the console's is the one
// Quarrel follows.
static HRESULT STDMETHODCALLTYPE on_default(IMMNotificationClient *, EDataFlow,
                                            ERole role, LPCWSTR) {
  if (role == eConsole) {
    win_devices_moved();
  }

  return S_OK;
}

// a device renamed, what else of its properties changes is not shown.
static HRESULT STDMETHODCALLTYPE on_property(IMMNotificationClient *, LPCWSTR,
                                             const PROPERTYKEY key) {
  if (key.pid == PKEY_Device_FriendlyName.pid &&
      IsEqualGUID(&key.fmtid, &PKEY_Device_FriendlyName.fmtid)) {
    win_devices_moved();
  }

  return S_OK;
}

static const IMMNotificationClientVtbl WATCHER = {
  watcher_query, watcher_retain, watcher_retain, on_state,
  on_device,     on_device,      on_default,     on_property,
};

uint32_t audio_devices_version(void (*changed)(void)) {
  static IMMNotificationClient watcher = {
    (IMMNotificationClientVtbl *)&WATCHER};
  static IMMDeviceEnumerator *watching;

  if (devices_changed == nullptr) {
    devices_changed = changed;
  }

  if (watching == nullptr && win_com() &&
      (watching = enumerator()) != nullptr) {
    watching->lpVtbl->RegisterEndpointNotificationCallback(watching, &watcher);
    win_cameras_watch();
  }

  return devices_version;
}
