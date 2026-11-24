#include <pipewire/impl-module.h>
#include <sched.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pipe.h"
#include "platform/platform.h"

// the most a callback is handed at a time.
constexpr uint32_t FRAMES_MOST = 4096;
// what a stream asks the graph for, 10 ms as the echo canceller works in.
#define LATENCY "480/48000"
// the nodes of the echo canceller are waited for this long.
constexpr double ECHO_WAIT_SECONDS = 3;

struct Audio {
  AudioEvents e;
  // audio_stop was called: the callbacks touch nothing of their owner's
  // anymore, and `busy` counts the ones that still do.
  atomic_bool stopped;
  atomic_int busy;
  atomic_bool live;
  char *input, *output;
  bool raw;

  // on the loop's thread.
  struct pw_impl_module *echo;
  char echo_source[96], echo_sink[96];
  struct pw_stream *capture, *playback;
  struct spa_hook capture_listener, playback_listener;
  struct spa_source *echo_timer;
  Audio *next_waiting;
  bool failed;
};

// the ones that wait for their echo canceller's nodes.
static Audio *waiting_echo;

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

// linux asks no app for the microphone, a sandbox's portal aside.
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
}

// on the loop's thread. it comes before what audio_stop posts to free it.
static void fail(Audio *a) {
  if (!a->failed) {
    a->failed = true;
    platform_post(failed_on_main, a);
  }
}

static void on_capture(void *data) {
  Audio *a = data;
  struct pw_buffer *b = pw_stream_dequeue_buffer(a->capture);

  if (b == nullptr) {
    return;
  }

  struct spa_data *d = &b->buffer->datas[0];

  if (d->data != nullptr && enter(a)) {
    uint32_t offset = d->chunk->offset % d->maxsize;
    uint32_t size = d->chunk->size < d->maxsize - offset ? d->chunk->size
                                                         : d->maxsize - offset;
    const float *heard = SPA_PTROFF(d->data, offset, const float);
    size_t count = size / sizeof(float);

    if (count > 0) {
      atomic_store_explicit(&a->live, true, memory_order_relaxed);
    }

    for (size_t at = 0; at < count; at += FRAMES_MOST) {
      size_t n = count - at < FRAMES_MOST ? count - at : FRAMES_MOST;

      a->e.captured(a->e.ctx, heard + at, n);
    }

    leave(a);
  }

  pw_stream_queue_buffer(a->capture, b);
}

static void on_playback(void *data) {
  Audio *a = data;
  struct pw_buffer *b = pw_stream_dequeue_buffer(a->playback);

  if (b == nullptr) {
    return;
  }

  struct spa_data *d = &b->buffer->datas[0];
  float *out = d->data;
  uint32_t most = d->maxsize / sizeof(float);
  // a graph that says nothing gets 10 ms.
  uint32_t count = b->requested > 0 ? (uint32_t)b->requested : 480;

  count = count < most ? count : most;

  if (out != nullptr && enter(a)) {
    for (uint32_t at = 0; at < count; at += FRAMES_MOST) {
      uint32_t n = count - at < FRAMES_MOST ? count - at : FRAMES_MOST;

      a->e.render(a->e.ctx, out + at, n);
    }

    leave(a);
  }
  else if (out != nullptr) {
    memset(out, 0, count * sizeof(float));
  }

  d->chunk->offset = 0;
  d->chunk->stride = sizeof(float);
  d->chunk->size = count * sizeof(float);
  pw_stream_queue_buffer(a->playback, b);
}

static void on_state(void *data, enum pw_stream_state,
                     enum pw_stream_state state, const char *error) {
  Audio *a = data;

  if (state == PW_STREAM_STATE_ERROR) {
    fprintf(stderr, "quarrel: sound failed: %s\n", error ? error : "");
    fail(a);
  }
}

static const struct pw_stream_events capture_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state,
  .process = on_capture,
};

static const struct pw_stream_events playback_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state,
  .process = on_playback,
};

// a stream of mono floats to or from the node of that name, the system's
// own when there is none.
static struct pw_stream *stream(Audio *a, bool capturing, const char *target,
                                bool own_target) {
  struct pw_properties *props = pw_properties_new(
    PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
    capturing ? "Capture" : "Playback", PW_KEY_MEDIA_ROLE, "Communication",
    PW_KEY_NODE_LATENCY, LATENCY, PW_KEY_NODE_NAME,
    capturing ? "quarrel-microphone" : "quarrel-speaker",
    PW_KEY_NODE_DESCRIPTION, capturing ? "Quarrel microphone" : "Quarrel",
    nullptr);

  if (target != nullptr) {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
  }

  // the echo canceller's nodes are the only ones it may go to.
  if (own_target) {
    pw_properties_set(props, "node.dont-fallback", "true");
    pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
  }

  struct pw_stream *s = pw_stream_new(
    pipe_core(), capturing ? "Quarrel microphone" : "Quarrel", props);

  if (s == nullptr) {
    return nullptr;
  }

  pw_stream_add_listener(
    s, capturing ? &a->capture_listener : &a->playback_listener,
    capturing ? &capture_events : &playback_events, a);

  uint8_t buffer[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  struct spa_audio_info_raw info = {
    .format = SPA_AUDIO_FORMAT_F32,
    .rate = AUDIO_RATE,
    .channels = 1,
    .position = {SPA_AUDIO_CHANNEL_MONO},
  };
  const struct spa_pod *params[] = {
    spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info),
  };

  if (pw_stream_connect(
        s, capturing ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
          PW_STREAM_FLAG_RT_PROCESS,
        params, 1) < 0) {
    pw_stream_destroy(s);
    return nullptr;
  }

  return s;
}

static void open_streams(Audio *a) {
  bool echo = a->echo != nullptr;
  const char *input = echo                                     ? a->echo_source
                      : pipe_is_device(AUDIO_INPUTS, a->input) ? a->input
                                                               : nullptr;
  const char *output = echo                                       ? a->echo_sink
                       : pipe_is_device(AUDIO_OUTPUTS, a->output) ? a->output
                                                                  : nullptr;

  a->capture = stream(a, true, input, echo);
  a->playback = stream(a, false, output, echo);

  if (a->capture == nullptr || a->playback == nullptr) {
    fail(a);
  }
}

static void stop_waiting(Audio *a) {
  if (a->echo_timer != nullptr) {
    pw_loop_destroy_source(pw_thread_loop_get_loop(pipe_loop()), a->echo_timer);
    a->echo_timer = nullptr;
  }

  for (Audio **at = &waiting_echo; *at != nullptr; at = &(*at)->next_waiting) {
    if (*at == a) {
      *at = a->next_waiting;
      break;
    }
  }
}

// the echo canceller's nodes came: the streams go to them.
static void on_nodes(void) {
  for (Audio *a = waiting_echo, *next; a != nullptr; a = next) {
    next = a->next_waiting;

    if (pipe_has_node(a->echo_source) && pipe_has_node(a->echo_sink)) {
      stop_waiting(a);
      open_streams(a);
    }
  }
}

static void echo_waited(void *data, uint64_t) {
  Audio *a = data;

  fprintf(stderr, "quarrel: the echo canceller did not start\n");
  stop_waiting(a);
  fail(a);
}

// copies `s` quoted for spa's json.
static void quoted(char *out, size_t cap, const char *s) {
  size_t n = 0;

  out[n++] = '"';

  for (; *s && n + 3 < cap; s++) {
    if (*s == '"' || *s == '\\') {
      out[n++] = '\\';
    }

    out[n++] = *s;
  }

  out[n++] = '"';
  out[n] = '\0';
}

// learned from the module's source: it makes a capture stream from the
// microphone, a playback stream to the speakers, and between them a source
// and a sink of its own, which the app records from and plays to. webrtc
// takes out of the first what went through the second.
static bool load_echo(Audio *a) {
  static atomic_uint made;
  unsigned n = atomic_fetch_add(&made, 1);
  char input[300] = "", output[300] = "", args[2048];

  snprintf(a->echo_source, sizeof a->echo_source, PIPE_OWN "echo-source.%d.%u",
           getpid(), n);
  snprintf(a->echo_sink, sizeof a->echo_sink, PIPE_OWN "echo-sink.%d.%u",
           getpid(), n);

  if (pipe_is_device(AUDIO_INPUTS, a->input)) {
    char q[280];

    quoted(q, sizeof q, a->input);
    snprintf(input, sizeof input, "target.object = %s", q);
  }

  if (pipe_is_device(AUDIO_OUTPUTS, a->output)) {
    char q[280];

    quoted(q, sizeof q, a->output);
    snprintf(output, sizeof output, "target.object = %s", q);
  }

  snprintf(args, sizeof args,
           "{ library.name = \"aec/libspa-aec-webrtc\" node.latency = " LATENCY
           " audio.rate = 48000 audio.channels = 1 audio.position = [ MONO ]"
           " aec.args = { webrtc.gain_control = true"
           " webrtc.noise_suppression = false webrtc.high_pass_filter = true"
           " webrtc.extended_filter = true webrtc.delay_agnostic = true }"
           " capture.props = { node.name = " PIPE_OWN "echo-capture.%d.%u"
           " node.description = \"Quarrel echo canceller\""
           " media.role = Communication %s }"
           " source.props = { node.name = %s"
           " node.description = \"Quarrel echo canceller\" }"
           " sink.props = { node.name = %s"
           " node.description = \"Quarrel echo canceller\" }"
           " playback.props = { node.name = " PIPE_OWN "echo-playback.%d.%u"
           " node.description = \"Quarrel echo canceller\""
           " media.role = Communication %s } }",
           getpid(), n, input, a->echo_source, a->echo_sink, getpid(), n,
           output);

  a->echo = pw_context_load_module(
    pipe_context(), "libpipewire-module-echo-cancel", args, nullptr);
  return a->echo != nullptr;
}

static void open_on_loop(void *ctx) {
  Audio *a = ctx;

  if (atomic_load(&a->stopped)) {
    return;
  }

  if (pipe_core() == nullptr) {
    fail(a);
    return;
  }

  if (a->raw || !load_echo(a)) {
    if (!a->raw) {
      fprintf(stderr, "quarrel: no echo canceller, the sound goes raw\n");
    }

    open_streams(a);
    return;
  }

  struct pw_loop *loop = pw_thread_loop_get_loop(pipe_loop());
  struct timespec after = {(time_t)ECHO_WAIT_SECONDS, 0};

  a->next_waiting = waiting_echo;
  waiting_echo = a;
  a->echo_timer = pw_loop_add_timer(loop, echo_waited, a);
  pw_loop_update_timer(loop, a->echo_timer, &after, nullptr, false);
  on_nodes();
}

Audio *audio_start(const AudioEvents *e, const char *input, const char *output,
                   bool raw, AudioResult *why) {
  *why = AUDIO_FAILED;

  if (!pipe_start()) {
    return nullptr;
  }

  Audio *a = calloc(1, sizeof *a);

  if (a == nullptr) {
    return nullptr;
  }

  a->e = *e;
  a->input = input != nullptr ? strdup(input) : nullptr;
  a->output = output != nullptr ? strdup(output) : nullptr;
  a->raw = raw;

  pipe_lock();
  pipe_hold();
  pipe_on_nodes(on_nodes);
  pipe_unlock();
  pipe_run(open_on_loop, a);

  *why = AUDIO_OK;
  return a;
}

bool audio_live(const Audio *a) {
  return atomic_load_explicit(&a->live, memory_order_relaxed);
}

static void free_on_main(void *ctx) {
  Audio *a = ctx;

  free(a->input);
  free(a->output);
  free(a);
}

static void close_on_loop(void *ctx) {
  Audio *a = ctx;

  stop_waiting(a);

  if (a->capture != nullptr) {
    pw_stream_destroy(a->capture);
  }

  if (a->playback != nullptr) {
    pw_stream_destroy(a->playback);
  }

  if (a->echo != nullptr) {
    pw_impl_module_destroy(a->echo);
  }

  pipe_release();
  platform_post(free_on_main, a);
}

void audio_stop(Audio *a) {
  if (a == nullptr) {
    return;
  }

  atomic_store(&a->stopped, true);

  // a callback that already started ends within a few milliseconds.
  while (atomic_load(&a->busy) != 0) {
    sched_yield();
  }

  pipe_run(close_on_loop, a);
}
