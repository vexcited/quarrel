#include "voice/voice.h"

#ifdef _WIN32
#include "platform/windows/sockets.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <math.h>
#include <opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord/api.h"
#include <cima/json_dsl.h>
#include "platform/platform.h"
#include "voice/audio.h"
#include "voice/dave.h"
#include "voice/denoise.h"
#include "voice/devices.h"
#include "voice/rtp.h"

// the version of the voice socket the desktop client speaks.
constexpr char VOICE_QUERY[] = "/?v=9";
constexpr char MODE[] = "aead_aes256_gcm_rtpsize";

enum {
  OP_IDENTIFY = 0,
  OP_SELECT_PROTOCOL = 1,
  OP_READY = 2,
  OP_HEARTBEAT = 3,
  OP_SESSION_DESCRIPTION = 4,
  OP_SPEAKING = 5,
  OP_HEARTBEAT_ACK = 6,
  OP_RESUME = 7,
  OP_HELLO = 8,
  OP_RESUMED = 9,
  OP_CLIENTS_CONNECT = 11,
  OP_VIDEO = 12,
  OP_CLIENT_DISCONNECT = 13,
  OP_DAVE_PREPARE_TRANSITION = 21,
  OP_DAVE_EXECUTE_TRANSITION = 22,
  OP_DAVE_TRANSITION_READY = 23,
  OP_DAVE_PREPARE_EPOCH = 24,
  OP_MLS_EXTERNAL_SENDER = 25,
  OP_MLS_KEY_PACKAGE = 26,
  OP_MLS_PROPOSALS = 27,
  OP_MLS_COMMIT_WELCOME = 28,
  OP_MLS_ANNOUNCE_COMMIT = 29,
  OP_MLS_WELCOME = 30,
  OP_MLS_INVALID_COMMIT_WELCOME = 31,
};

// 20 ms of sound, what a packet carries.
constexpr int FRAME = AUDIO_RATE / 50;
// the longest frame opus may hold, 120 ms.
constexpr int FRAME_MAX = AUDIO_RATE * 120 / 1000;
constexpr size_t OPUS_MAX = 1275;
constexpr size_t PACKET_MAX = 2048;
constexpr int BITRATE = 64000;

constexpr size_t STREAMS_MAX = 64;
// a second of sound, a power of two.
constexpr uint32_t RING = 65536;
constexpr uint32_t CAPTURE_RING = 16384;
// what a stream holds before it is played, and what it is cut back to when
// it holds too much: sound that waits is sound heard late.
constexpr uint32_t PREROLL = 3 * FRAME;
constexpr uint32_t LATE = AUDIO_RATE / 4;

// how loud the microphone has to be for it to be someone speaking, and for
// how many frames it then stays open.
constexpr float GATE = 0.006f;
constexpr int HANGOVER = 15;
// opus frames of silence sent when the speaking stops, so that the others'
// decoders do not fill the gap with a guess.
constexpr int SILENCES = 5;
static const uint8_t SILENCE[] = {0xf8, 0xff, 0xfe};

constexpr double SPOKE_FOR = 0.25;
// how often who speaks is looked at, for `changed`.
constexpr double PULSE_EVERY = 0.1;
constexpr double DISCOVERY_EVERY = 0.5;
constexpr int DISCOVERY_TRIES = 10;
constexpr double KEEPALIVE_EVERY = 5;
constexpr int RESUMES_MAX = 5;
constexpr double HANDSHAKE_TIMEOUT = 20;

typedef struct {
  // 0 for a free one. the main thread gives them, the others read.
  atomic_uint ssrc;
  atomic_uint_fast64_t user_id;
  // when its sound last came, milliseconds.
  atomic_uint_fast64_t heard_at;

  // the media thread's.
  OpusDecoder *decoder;
  uint32_t decoding;
  uint16_t next;
  bool started;

  // written by the media thread, read by the audio one.
  float ring[RING];
  atomic_uint head, tail;
  // the audio thread's.
  bool buffering;
} Stream;

// what a callback posted to the main thread holds on to: the call may be
// gone when it runs.
typedef struct {
  atomic_int refs;
  Voice *voice;
} Link;

struct Voice {
  VoiceEvents e;
  Snowflake self_id, guild_id, channel_id;
  char *session_id, *token, *endpoint;
  VoiceStatus status;
  char error[160];
  Link *link;
  // one more for every connection, what was posted for an older is dropped.
  uint32_t generation;

  PlatformWebSocket *ws;
  PlatformTimer *heartbeat, *watchdog, *retry, *pulse;
  // who spoke at the last pulse, mixed into one number.
  uint64_t spoke;
  double heartbeat_every, beat_at, latency;
  // the last sequence the server numbered a message with, -1 before any.
  int64_t seq;
  bool described;
  int resumes;

  Dave *dave;
  uint32_t ssrc;
  struct sockaddr_in server;
  int udp;
  int wake[2];
  pthread_t thread;
  bool threaded;
  atomic_bool stop, keyed, mute, deaf, speaking;
  RtpCipher *cipher;
  OpusEncoder *encoder;

  Audio *audio;
  bool no_microphone;
  // the devices the sound runs through, as devices_version had them.
  uint32_t devices;
  // the packets carry the mark of VOICE_QOS.
  bool marked;
  // audio_live as the last pulse found it.
  bool heard;
  // how loud the microphone was at its last frame, 0 to 1.
  _Atomic float level;
  float capture[CAPTURE_RING];
  atomic_uint capture_head, capture_tail;
  Stream streams[STREAMS_MAX];
};

static uint64_t now_ms(void) {
  return (uint64_t)(platform_now() * 1000);
}

static void changed(Voice *v) {
  if (v->e.changed != nullptr) {
    v->e.changed(v->e.ctx);
  }
}

/* ========================================================================== */
/* posting to the main thread.                                                */
/* ========================================================================== */

typedef struct {
  Link *link;
  uint32_t generation;
  void (*fn)(Voice *v, uint64_t a, uint64_t b);
  uint64_t a, b;
} Posted;

static void link_drop(Link *link) {
  if (atomic_fetch_sub(&link->refs, 1) == 1) {
    free(link);
  }
}

static void run_posted(void *ctx) {
  Posted *p = ctx;
  Voice *v = p->link->voice;

  if (v != nullptr && v->generation == p->generation) {
    p->fn(v, p->a, p->b);
  }

  link_drop(p->link);
  free(p);
}

static void post(Voice *v, uint32_t generation,
                 void (*fn)(Voice *v, uint64_t a, uint64_t b), uint64_t a,
                 uint64_t b) {
  Posted *p = malloc(sizeof *p);

  if (p == nullptr) {
    return;
  }

  atomic_fetch_add(&v->link->refs, 1);
  *p = (Posted){v->link, generation, fn, a, b};
  platform_post(run_posted, p);
}

/* ========================================================================== */
/* the audio thread: the microphone into a ring, the streams out of theirs.   */
/* ========================================================================== */

static void on_captured(void *ctx, const float *samples, size_t count) {
  Voice *v = ctx;
  uint32_t head = atomic_load_explicit(&v->capture_head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&v->capture_tail, memory_order_acquire);

  // sound nobody took is dropped, the newest first.
  if (count > CAPTURE_RING - (head - tail)) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    v->capture[(head + i) % CAPTURE_RING] = samples[i];
  }

  atomic_store_explicit(&v->capture_head, head + (uint32_t)count,
                        memory_order_release);

  if (head + count - tail >= (uint32_t)FRAME) {
    (void)!write(v->wake[1], "", 1);
  }
}

static void on_render(void *ctx, float *samples, size_t count)
  [[clang::nonblocking]] {
  Voice *v = ctx;

  memset(samples, 0, count * sizeof *samples);

  if (atomic_load_explicit(&v->deaf, memory_order_relaxed)) {
    return;
  }

  for (size_t i = 0; i < STREAMS_MAX; i++) {
    Stream *s = &v->streams[i];

    if (atomic_load_explicit(&s->ssrc, memory_order_relaxed) == 0) {
      continue;
    }

    uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint32_t held = head - tail;

    if (held > LATE) {
      tail = head - PREROLL;
      held = PREROLL;
    }

    if (s->buffering && held < PREROLL) {
      atomic_store_explicit(&s->tail, tail, memory_order_release);
      continue;
    }

    s->buffering = false;

    uint32_t take = held < count ? held : (uint32_t)count;

    for (uint32_t j = 0; j < take; j++) {
      samples[j] += s->ring[(tail + j) % RING];
    }

    // it ran dry: the next sound waits for some more before it plays.
    if (take < count) {
      s->buffering = true;
    }

    atomic_store_explicit(&s->tail, tail + take, memory_order_release);
  }

  float volume = devices_output_volume();

  for (size_t i = 0; i < count; i++) {
    samples[i] = fminf(fmaxf(samples[i] * volume, -1), 1);
  }
}

/* ========================================================================== */
/* the media thread: what the socket receives, and what the microphone heard. */
/* ========================================================================== */

typedef struct {
  Voice *v;
  uint32_t generation;
  uint16_t sequence;
  uint32_t timestamp, nonce;
  bool discovered;
  int tries;
  double discovery_at, keepalive_at;
  uint32_t keepalives;
  int open, silences;
  bool speaking;
  float pcm[FRAME_MAX];
  // null when it could not be made: the noise then stays.
  Denoise *denoise;
} Media;

static void on_discovered(Voice *v, uint64_t address, uint64_t port);
static void on_discovery_failed(Voice *v, uint64_t, uint64_t);
static void on_speaking(Voice *v, uint64_t speaking, uint64_t);

// asks the server what address and port it sees the socket's packets come
// from, which it then sends the others' sound to.
static void send_discovery(Media *m) {
  Voice *v = m->v;
  uint8_t packet[74] = {0, 1, 0, 70};

  packet[4] = (uint8_t)(v->ssrc >> 24), packet[5] = (uint8_t)(v->ssrc >> 16);
  packet[6] = (uint8_t)(v->ssrc >> 8), packet[7] = (uint8_t)v->ssrc;
  send(v->udp, packet, sizeof packet, 0);
  m->discovery_at = platform_now();
  m->tries++;
}

static bool read_discovery(Media *m, const uint8_t *packet, size_t len) {
  if (len != 74 || packet[0] != 0 || packet[1] != 2) {
    return false;
  }

  char address[65] = {};
  struct in_addr parsed;

  memcpy(address, packet + 8, 64);

  if (inet_pton(AF_INET, address, &parsed) != 1) {
    return false;
  }

  m->discovered = true;
  post(m->v, m->generation, on_discovered, parsed.s_addr,
       (uint64_t)packet[72] << 8 | packet[73]);
  return true;
}

static Stream *stream_of(Voice *v, uint32_t ssrc) {
  for (size_t i = 0; i < STREAMS_MAX; i++) {
    if (atomic_load_explicit(&v->streams[i].ssrc, memory_order_acquire) ==
        ssrc) {
      return &v->streams[i];
    }
  }

  return nullptr;
}

static void play(Stream *s, const float *pcm, int count) {
  uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);

  if ((uint32_t)count > RING - (head - tail)) {
    return;
  }

  for (int i = 0; i < count; i++) {
    s->ring[(head + (uint32_t)i) % RING] = pcm[i];
  }

  atomic_store_explicit(&s->head, head + (uint32_t)count,
                        memory_order_release);
}

static void receive(Media *m, const uint8_t *packet, size_t len) {
  Voice *v = m->v;
  uint8_t sealed[PACKET_MAX], frame[PACKET_MAX];
  size_t sealed_len = 0;
  RtpHeader h;

  if (!rtp_open(v->cipher, packet, len, &h, sealed, sizeof sealed,
                &sealed_len) ||
      h.type != RTP_OPUS) {
    return;
  }

  Stream *s = stream_of(v, h.ssrc);

  // nobody is known to send with this ssrc yet.
  if (s == nullptr) {
    return;
  }

  Snowflake user_id = atomic_load(&s->user_id);
  size_t n =
    dave_open(v->dave, user_id, sealed, sealed_len, frame, sizeof frame);

  if (n == 0) {
    return;
  }

  if (s->decoder == nullptr) {
    int error = 0;

    s->decoder = opus_decoder_create(AUDIO_RATE, 1, &error);

    if (s->decoder == nullptr) {
      return;
    }
  }

  // the slot was another's before.
  if (s->decoding != h.ssrc) {
    opus_decoder_ctl(s->decoder, OPUS_RESET_STATE);
    s->decoding = h.ssrc;
    s->started = false;
  }

  int16_t ahead = (int16_t)(h.sequence - s->next);

  // late or twice.
  if (s->started && ahead < 0) {
    return;
  }

  bool silent = n == sizeof SILENCE && memcmp(frame, SILENCE, n) == 0;

  if (!silent) {
    atomic_store(&s->heard_at, now_ms());
  }

  if (atomic_load_explicit(&v->deaf, memory_order_relaxed)) {
    s->next = (uint16_t)(h.sequence + 1);
    s->started = true;
    return;
  }

  // a few packets lost are guessed by the decoder, more are a pause.
  for (int lost = 0; s->started && lost < ahead && ahead <= 5; lost++) {
    int guessed = opus_decode_float(s->decoder, nullptr, 0, m->pcm, FRAME, 0);

    if (guessed > 0) {
      play(s, m->pcm, guessed);
    }
  }

  int count =
    opus_decode_float(s->decoder, frame, (opus_int32)n, m->pcm, FRAME_MAX, 0);

  if (count > 0) {
    play(s, m->pcm, count);
  }

  s->next = (uint16_t)(h.sequence + 1);
  s->started = true;
}

static void send_frame(Media *m, const uint8_t *opus, size_t len) {
  Voice *v = m->v;
  uint8_t sealed[PACKET_MAX], packet[PACKET_MAX];

  if (dave_sealed_size(v->dave, len) > sizeof sealed - RTP_OVERHEAD) {
    return;
  }

  // 0 while the call's keys are not there yet: nothing goes out unsealed.
  size_t n = dave_seal(v->dave, v->ssrc, opus, len, sealed, sizeof sealed);

  if (n == 0) {
    return;
  }

  RtpHeader h = {RTP_OPUS, m->sequence++, m->timestamp, v->ssrc};
  size_t size =
    rtp_seal(v->cipher, &h, m->nonce++, sealed, n, packet, sizeof packet);

  if (size != 0) {
    send(v->udp, packet, size, 0);
    m->keepalive_at = platform_now();
  }
}

static void set_speaking(Media *m, bool speaking) {
  if (m->speaking == speaking) {
    return;
  }

  m->speaking = speaking;
  atomic_store(&m->v->speaking, speaking);
  post(m->v, m->generation, on_speaking, speaking, 0);
}

// one frame off the microphone: sent when someone speaks into it.
static void capture(Media *m, float *pcm) {
  Voice *v = m->v;
  float energy = 0, volume = devices_input_volume();

  VoiceInput input = devices_input();

  if (m->denoise != nullptr && input.noise != NOISE_OFF) {
    denoise_run(m->denoise, pcm, FRAME, input.noise == NOISE_VOICE);
  }

  for (int i = 0; i < FRAME; i++) {
    pcm[i] *= volume;
    energy += pcm[i] * pcm[i];
  }

  float heard = sqrtf(energy / FRAME);
  bool loud = heard > (input.auto_sensitivity
                         ? GATE
                         : powf(10, input.sensitivity / 20));

  atomic_store_explicit(&v->level, heard, memory_order_relaxed);
  bool muted = atomic_load_explicit(&v->mute, memory_order_relaxed);

  m->open = muted ? 0 : loud ? HANGOVER : m->open > 0 ? m->open - 1 : 0;

  if (m->open > 0) {
    uint8_t opus[OPUS_MAX];
    opus_int32 n = opus_encode_float(v->encoder, pcm, FRAME, opus, sizeof opus);

    set_speaking(m, true);
    m->silences = SILENCES;

    if (n > 0) {
      send_frame(m, opus, (size_t)n);
    }
  }
  else if (m->silences > 0) {
    m->silences--;
    send_frame(m, SILENCE, sizeof SILENCE);

    if (m->silences == 0) {
      set_speaking(m, false);
    }
  }

  // time goes on while nothing is sent.
  m->timestamp += (uint32_t)FRAME;
}

static void drain_capture(Media *m) {
  Voice *v = m->v;
  uint32_t tail = atomic_load_explicit(&v->capture_tail, memory_order_relaxed);
  float pcm[FRAME];

  for (;;) {
    uint32_t head = atomic_load_explicit(&v->capture_head, memory_order_acquire);

    if (head - tail < (uint32_t)FRAME) {
      break;
    }

    for (int i = 0; i < FRAME; i++) {
      pcm[i] = v->capture[(tail + (uint32_t)i) % CAPTURE_RING];
    }

    tail += (uint32_t)FRAME;
    atomic_store_explicit(&v->capture_tail, tail, memory_order_release);

    if (atomic_load(&v->keyed)) {
      capture(m, pcm);
    }
  }
}

// a packet now and then keeps the way back open through a router while
// nobody speaks.
static void keep_alive(Media *m) {
  uint8_t packet[8] = {};
  uint32_t count = m->keepalives++;

  memcpy(packet, &count, sizeof count);
  send(m->v->udp, packet, sizeof packet, 0);
  m->keepalive_at = platform_now();
}

static void *media_thread(void *arg) {
  Media *m = arg;
  Voice *v = m->v;
  uint8_t packet[PACKET_MAX];

  while (!atomic_load(&v->stop)) {
    struct pollfd fds[] = {{v->udp, POLLIN, 0}, {v->wake[0], POLLIN, 0}};
    double now = platform_now();

    if (!m->discovered && now - m->discovery_at >= DISCOVERY_EVERY) {
      if (m->tries >= DISCOVERY_TRIES) {
        post(v, m->generation, on_discovery_failed, 0, 0);
        break;
      }

      send_discovery(m);
    }

    if (m->discovered && now - m->keepalive_at >= KEEPALIVE_EVERY) {
      keep_alive(m);
    }

    poll(fds, countof(fds), 250);

    if (fds[1].revents & POLLIN) {
      uint8_t woken[64];

      while (read(v->wake[0], woken, sizeof woken) > 0) {
      }
    }

    for (;;) {
      ssize_t len = recv(v->udp, packet, sizeof packet, 0);

      if (len <= 0) {
        break;
      }

      if (!m->discovered) {
        read_discovery(m, packet, (size_t)len);
      }
      else if (atomic_load(&v->keyed)) {
        receive(m, packet, (size_t)len);
      }
    }

    drain_capture(m);
  }

  denoise_free(m->denoise);
  free(m);
  return nullptr;
}

/* ========================================================================== */
/* the socket: what is sent.                                                  */
/* ========================================================================== */

static void begin(cima_json_writer *w, int op) {
  json_begin_object(w);
  json_field(w, "op", op);
  json_key(w, "d");
}

static void send_json(Voice *v, cima_json_writer *w) {
  json_end_object(w);

  if (v->ws != nullptr) {
    platform_ws_send_text(v->ws, cima_json_writer_text(w));
  }

  cima_json_writer_dispose(w);
}

static void send_binary(Voice *v, uint8_t op, const uint8_t *bytes,
                        size_t len) {
  uint8_t *message = malloc(len + 1);

  if (message == nullptr || v->ws == nullptr) {
    free(message);
    return;
  }

  message[0] = op;
  memcpy(message + 1, bytes, len);
  platform_ws_send_binary(v->ws, message, len + 1);
  free(message);
}

static void send_identify(Voice *v) {
  cima_json_writer w = {};

  begin(&w, OP_IDENTIFY);
  json_begin_object(&w);
  json_key(&w, "server_id"), snowflake_write(&w, v->guild_id);
  json_key(&w, "channel_id"), snowflake_write(&w, v->channel_id);
  json_key(&w, "user_id"), snowflake_write(&w, v->self_id);
  json_field(&w, "session_id", v->session_id);
  json_field(&w, "token", v->token);
  json_field(&w, "max_dave_protocol_version", dave_max_version());
  json_field(&w, "video", false);
  json_end_object(&w);
  send_json(v, &w);
}

static void send_resume(Voice *v) {
  cima_json_writer w = {};

  begin(&w, OP_RESUME);
  json_begin_object(&w);
  json_key(&w, "server_id"), snowflake_write(&w, v->guild_id);
  json_field(&w, "session_id", v->session_id);
  json_field(&w, "token", v->token);
  json_field(&w, "seq_ack", v->seq);
  json_end_object(&w);
  send_json(v, &w);
}

static void send_select_protocol(Voice *v, const char *address,
                                 uint16_t port) {
  cima_json_writer w = {};
  char connection[37];

  platform_uuid(connection);
  begin(&w, OP_SELECT_PROTOCOL);
  json_begin_object(&w);
  json_field(&w, "protocol", "udp");
  json_key(&w, "data");
  json_begin_object(&w);
  json_field(&w, "address", address);
  json_field(&w, "port", port);
  json_field(&w, "mode", MODE);
  json_end_object(&w);
  // the desktop client repeats them beside `data`.
  json_field(&w, "address", address);
  json_field(&w, "port", port);
  json_field(&w, "mode", MODE);
  json_key(&w, "codecs");
  json_begin_array(&w);
  json_begin_object(&w);
  json_field(&w, "name", "opus");
  json_field(&w, "type", "audio");
  json_field(&w, "priority", 1000);
  json_field(&w, "payload_type", RTP_OPUS);
  json_end_object(&w);
  json_end_array(&w);
  json_field(&w, "rtc_connection_id", connection);
  json_end_object(&w);
  send_json(v, &w);
}

static void send_speaking(Voice *v, bool speaking) {
  cima_json_writer w = {};

  begin(&w, OP_SPEAKING);
  json_begin_object(&w);
  json_field(&w, "speaking", speaking ? 1 : 0);
  json_field(&w, "delay", 0);
  json_field(&w, "ssrc", v->ssrc);
  json_end_object(&w);
  send_json(v, &w);
}

// what the account sends with: sound only.
static void send_sources(Voice *v) {
  cima_json_writer w = {};

  begin(&w, OP_VIDEO);
  json_begin_object(&w);
  json_field(&w, "audio_ssrc", v->ssrc);
  json_field(&w, "video_ssrc", 0);
  json_field(&w, "rtx_ssrc", 0);
  json_key(&w, "streams");
  json_begin_array(&w);
  json_end_array(&w);
  json_end_object(&w);
  send_json(v, &w);
}

static void on_heartbeat(void *ctx) {
  Voice *v = ctx;
  cima_json_writer w = {};

  v->beat_at = platform_now();
  begin(&w, OP_HEARTBEAT);
  json_begin_object(&w);
  json_field(&w, "t", now_ms());
  json_field(&w, "seq_ack", v->seq);
  json_end_object(&w);
  send_json(v, &w);
  v->heartbeat = platform_timer(v->heartbeat_every, on_heartbeat, v);
}

/* ========================================================================== */
/* dave's messages.                                                           */
/* ========================================================================== */

static void dave_key_package(void *ctx, const uint8_t *bytes, size_t len) {
  send_binary(ctx, OP_MLS_KEY_PACKAGE, bytes, len);
}

static void dave_commit_welcome(void *ctx, const uint8_t *bytes, size_t len) {
  send_binary(ctx, OP_MLS_COMMIT_WELCOME, bytes, len);
}

static void send_transition(Voice *v, int op, uint16_t transition_id) {
  cima_json_writer w = {};

  begin(&w, op);
  json_begin_object(&w);
  json_field(&w, "transition_id", transition_id);
  json_end_object(&w);
  send_json(v, &w);
}

static void dave_ready(void *ctx, uint16_t transition_id) {
  send_transition(ctx, OP_DAVE_TRANSITION_READY, transition_id);
}

static void dave_invalid(void *ctx, uint16_t transition_id) {
  send_transition(ctx, OP_MLS_INVALID_COMMIT_WELCOME, transition_id);
}

/* ========================================================================== */
/* connecting and letting go.                                                 */
/* ========================================================================== */

static void close_socket(Voice *v, int code) {
  platform_timer_cancel(v->heartbeat);
  platform_timer_cancel(v->watchdog);
  v->heartbeat = v->watchdog = nullptr;

  if (v->ws != nullptr) {
    platform_ws_close(v->ws, code, "");
    platform_ws_free(v->ws);
    v->ws = nullptr;
  }
}

// stops the sound and the threads, the socket staying as it is.
static void stop_media(Voice *v) {
  audio_stop(v->audio);
  v->audio = nullptr;

  if (v->threaded) {
    atomic_store(&v->stop, true);
    (void)!write(v->wake[1], "", 1);
    pthread_join(v->thread, nullptr);
    v->threaded = false;
  }

  if (v->udp >= 0) {
    close(v->udp);
    v->udp = -1;
  }

  for (size_t i = 0; i < countof(v->wake); i++) {
    if (v->wake[i] >= 0) {
      close(v->wake[i]);
      v->wake[i] = -1;
    }
  }

  atomic_store(&v->keyed, false);
  atomic_store(&v->speaking, false);
  rtp_cipher_free(v->cipher);
  v->cipher = nullptr;

  if (v->encoder != nullptr) {
    opus_encoder_destroy(v->encoder);
    v->encoder = nullptr;
  }

  for (size_t i = 0; i < STREAMS_MAX; i++) {
    Stream *s = &v->streams[i];

    if (s->decoder != nullptr) {
      opus_decoder_destroy(s->decoder);
    }

    atomic_store(&s->ssrc, 0);
    atomic_store(&s->user_id, 0);
    atomic_store(&s->heard_at, 0);
    atomic_store(&s->head, 0);
    atomic_store(&s->tail, 0);
    s->decoder = nullptr;
    s->decoding = 0;
    s->started = false;
    s->buffering = true;
  }

  atomic_store(&v->capture_head, 0);
  atomic_store(&v->capture_tail, 0);
}

static void teardown(Voice *v) {
  v->generation++;
  platform_timer_cancel(v->retry);
  platform_timer_cancel(v->pulse);
  v->retry = v->pulse = nullptr;
  close_socket(v, 1000);
  stop_media(v);
  dave_free(v->dave);
  v->dave = nullptr;
  v->described = false;
  v->seq = -1;
  v->latency = -1;
  v->resumes = 0;
}

[[gnu::cold]] static void fail(Voice *v, const char *why) {
  teardown(v);
  v->status = VOICE_FAILED;
  snprintf(v->error, sizeof v->error, "%s", why);
  changed(v);
}

// the call is over for the account, not by its own doing.
[[gnu::cold]] static void end(Voice *v, const char *why) {
  fail(v, why);

  if (v->e.ended != nullptr) {
    v->e.ended(v->e.ctx);
  }
}

static void open_socket(Voice *v);

static void on_retry(void *ctx) {
  Voice *v = ctx;

  v->retry = nullptr;
  open_socket(v);
}

static void on_watchdog(void *ctx) {
  Voice *v = ctx;

  v->watchdog = nullptr;
  fail(v, "The voice server did not answer.");
}

/* ========================================================================== */
/* the socket: what is received.                                              */
/* ========================================================================== */

static Stream *assign(Voice *v, Snowflake user_id, uint32_t ssrc) {
  Stream *free_one = nullptr;

  if (ssrc == 0 || user_id == 0 || user_id == v->self_id) {
    return nullptr;
  }

  for (size_t i = 0; i < STREAMS_MAX; i++) {
    Stream *s = &v->streams[i];
    uint32_t held = atomic_load(&s->ssrc);

    if (held != 0 && atomic_load(&s->user_id) == user_id) {
      // a user sends with one ssrc for sound.
      if (held != ssrc) {
        atomic_store(&s->ssrc, ssrc);
      }

      return s;
    }

    if (held == 0 && free_one == nullptr) {
      free_one = s;
    }
  }

  if (free_one != nullptr) {
    atomic_store(&free_one->user_id, user_id);
    atomic_store(&free_one->heard_at, 0);
    atomic_store(&free_one->ssrc, ssrc);
  }

  return free_one;
}

static void unassign(Voice *v, Snowflake user_id) {
  for (size_t i = 0; i < STREAMS_MAX; i++) {
    Stream *s = &v->streams[i];

    if (atomic_load(&s->ssrc) != 0 && atomic_load(&s->user_id) == user_id) {
      atomic_store(&s->ssrc, 0);
    }
  }
}

// learned from webrtc, which discord's calls are built on: sound is marked
// for expedited forwarding, 46, in the six high bits of the byte.
static void mark_packets(Voice *v) {
  int service = devices_option(VOICE_QOS) ? 46 << 2 : 0;

  v->marked = service != 0;

  if (v->udp >= 0) {
    setsockopt(v->udp, IPPROTO_IP, IP_TOS, &service, sizeof service);
  }
}

static bool start_media(Voice *v, const char *ip, uint16_t port) {
  v->server = (struct sockaddr_in){
    .sin_family = AF_INET,
    .sin_port = htons(port),
  };

  if (inet_pton(AF_INET, ip, &v->server.sin_addr) != 1) {
    return false;
  }

  v->udp = socket(AF_INET, SOCK_DGRAM, 0);

  if (v->udp < 0 || pipe(v->wake) != 0) {
    return false;
  }

  fcntl(v->udp, F_SETFL, fcntl(v->udp, F_GETFL) | O_NONBLOCK);
  mark_packets(v);
  fcntl(v->wake[0], F_SETFL, fcntl(v->wake[0], F_GETFL) | O_NONBLOCK);
  fcntl(v->wake[1], F_SETFL, fcntl(v->wake[1], F_GETFL) | O_NONBLOCK);

  if (connect(v->udp, (struct sockaddr *)&v->server, sizeof v->server) != 0) {
    return false;
  }

  Media *m = calloc(1, sizeof *m);

  if (m == nullptr) {
    return false;
  }

  m->v = v;
  m->generation = v->generation;
  m->denoise = denoise_new();
  platform_random(&m->sequence, sizeof m->sequence);
  platform_random(&m->timestamp, sizeof m->timestamp);
  atomic_store(&v->stop, false);

  if (pthread_create(&v->thread, nullptr, media_thread, m) != 0) {
    denoise_free(m->denoise);
    free(m);
    return false;
  }

  v->threaded = true;
  return true;
}

static void on_ready(Voice *v, const cima_json *d) {
  bool supported = false;

  CIMA_EACH(mode, json_items(json_get(d, "modes"))) {
    supported |= strcmp(json_or(mode, ""), MODE) == 0;
  }

  if (!supported) {
    fail(v, "The voice server has no encryption Quarrel knows.");
    return;
  }

  v->ssrc = (uint32_t)json_get_coerce(d, "ssrc", (uint64_t)0);

  if (!start_media(v,
                   json_get_or(d, "ip", nullptr) != nullptr
                     ? json_get_or(d, "ip", nullptr)
                     : "",
                   (uint16_t)json_get_coerce(d, "port", (uint64_t)0))) {
    fail(v, "The voice server could not be reached.");
  }
}

static void on_discovered(Voice *v, uint64_t address, uint64_t port) {
  struct in_addr in = {.s_addr = (in_addr_t)address};
  char text[INET_ADDRSTRLEN] = "";

  inet_ntop(AF_INET, &in, text, sizeof text);
  send_select_protocol(v, text, (uint16_t)port);
}

static void on_discovery_failed(Voice *v, uint64_t, uint64_t) {
  fail(v, "The voice server did not answer over UDP.");
}

static void on_speaking(Voice *v, uint64_t speaking, uint64_t) {
  send_speaking(v, speaking != 0);
  changed(v);
}

static void restart_audio(Voice *v);

// the others speak and stop without the server saying so: it shows in what
// they send.
static void on_pulse(void *ctx) {
  Voice *v = ctx;
  uint64_t now = now_ms(), spoke = 0;

  for (size_t i = 0; i < STREAMS_MAX; i++) {
    const Stream *s = &v->streams[i];

    if (atomic_load(&s->ssrc) != 0 &&
        now - atomic_load(&s->heard_at) < (uint64_t)(SPOKE_FOR * 1000)) {
      spoke = spoke * 31 + atomic_load(&s->user_id);
    }
  }

  v->pulse = platform_timer(PULSE_EVERY, on_pulse, v);

  // another microphone or other speakers were picked.
  if (v->audio != nullptr && v->devices != devices_version()) {
    restart_audio(v);
  }

  if (v->marked != devices_option(VOICE_QOS)) {
    mark_packets(v);
  }

  bool heard = v->audio != nullptr && audio_live(v->audio);

  if (heard != v->heard) {
    v->heard = heard;
    changed(v);
  }

  if (spoke != v->spoke) {
    v->spoke = spoke;
    changed(v);
  }
}

static void on_audio_failed(void *ctx) {
  Voice *v = ctx;

  audio_stop(v->audio);
  v->audio = nullptr;
  v->no_microphone = true;
  changed(v);
}

static void start_audio(Voice *v) {
  AudioEvents events = {v, on_captured, on_render, on_audio_failed};
  AudioResult why = AUDIO_OK;

  if (v->audio != nullptr || v->status != VOICE_CONNECTED) {
    return;
  }

  v->devices = devices_version();
  v->audio = audio_start(&events, devices_get(AUDIO_INPUTS),
                         devices_get(AUDIO_OUTPUTS), !devices_input().echo,
                         &why);
  v->no_microphone = v->audio == nullptr;
  v->heard = false;
  changed(v);
}

static void restart_audio(Voice *v) {
  audio_stop(v->audio);
  v->audio = nullptr;
  atomic_store(&v->capture_head, 0);
  atomic_store(&v->capture_tail, 0);
  start_audio(v);
}

static void on_microphone(Voice *v, uint64_t, uint64_t) {
  start_audio(v);
}

static void on_asked(void *ctx, bool) {
  Posted *p = ctx;

  // through the same door as what the threads post, the call may be gone.
  run_posted(p);
}

static void ask_microphone(Voice *v) {
  Posted *p = malloc(sizeof *p);

  if (p == nullptr) {
    return;
  }

  atomic_fetch_add(&v->link->refs, 1);
  *p = (Posted){v->link, v->generation, on_microphone, 0, 0};
  audio_ask(on_asked, p);
}

static void on_described(Voice *v, const cima_json *d) {
  const cima_json *key = json_get(d, "secret_key");
  uint8_t secret[32] = {};
  size_t n = 0;

  if (strcmp(json_get_or(d, "mode", nullptr) != nullptr
               ? json_get_or(d, "mode", nullptr)
               : "",
             MODE) != 0 ||
      json_len(key) != sizeof secret) {
    fail(v, "The voice server chose an encryption Quarrel does not know.");
    return;
  }

  CIMA_EACH(byte, json_items(key)) {
    secret[n++] = (uint8_t)snowflake_of(byte);
  }

  int error = 0;

  // a resumed call keeps its keys.
  if (v->cipher == nullptr) {
    v->cipher = rtp_cipher_new(secret);
    v->encoder =
      opus_encoder_create(AUDIO_RATE, 1, OPUS_APPLICATION_VOIP, &error);
  }

  if (v->cipher == nullptr || v->encoder == nullptr) {
    fail(v, "The call's sound could not be set up.");
    return;
  }

  opus_encoder_ctl(v->encoder, OPUS_SET_BITRATE(BITRATE));
  opus_encoder_ctl(v->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(v->encoder, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(v->encoder, OPUS_SET_PACKET_LOSS_PERC(10));

  dave_set_ssrc(v->dave, v->ssrc);
  dave_selected(v->dave, (uint16_t)json_get_coerce(d, "dave_protocol_version",
                                                   (uint64_t)0));
  atomic_store(&v->keyed, true);

  platform_timer_cancel(v->watchdog);
  v->watchdog = nullptr;
  v->described = true;
  v->resumes = 0;
  v->status = VOICE_CONNECTED;
  send_speaking(v, false);
  send_sources(v);
  platform_timer_cancel(v->pulse);
  v->pulse = platform_timer(PULSE_EVERY, on_pulse, v);
  changed(v);
  ask_microphone(v);
}

static void on_text(void *ctx, const char *data, size_t len) {
  Voice *v = ctx;
  cilo_arena arena = {};
  const cima_json *message = json_parse(&arena, data, len, nullptr);
  const cima_json *d = json_get(message, "d");
  int op = (int)json_get_coerce(message, "op", (int64_t)-1);

  if (json_get(message, "seq") != nullptr) {
    v->seq = json_get_coerce(message, "seq", (int64_t)(v->seq));
  }

  switch (op) {
    case OP_HELLO:
      v->heartbeat_every =
        json_get_coerce(d, "heartbeat_interval", (double)13750) / 1000;
      platform_timer_cancel(v->heartbeat);
      v->heartbeat = platform_timer(v->heartbeat_every, on_heartbeat, v);

      if (v->described) {
        send_resume(v);
      }
      else {
        send_identify(v);
      }

      break;
    case OP_READY:
      on_ready(v, d);
      break;
    case OP_SESSION_DESCRIPTION:
      on_described(v, d);
      break;
    case OP_RESUMED:
      platform_timer_cancel(v->watchdog);
      v->watchdog = nullptr;
      v->resumes = 0;
      v->status = VOICE_CONNECTED;
      changed(v);
      break;
    case OP_HEARTBEAT_ACK:
      v->latency = platform_now() - v->beat_at;
      changed(v);
      break;
    case OP_SPEAKING:
      assign(v, snowflake_of(json_get(d, "user_id")),
             (uint32_t)json_get_coerce(d, "ssrc", (uint64_t)0));
      dave_add_user(v->dave, snowflake_of(json_get(d, "user_id")));
      break;
    case OP_VIDEO:
      assign(v, snowflake_of(json_get(d, "user_id")),
             (uint32_t)json_get_coerce(d, "audio_ssrc", (uint64_t)0));
      dave_add_user(v->dave, snowflake_of(json_get(d, "user_id")));
      break;
    case OP_CLIENTS_CONNECT:
      CIMA_EACH(user_id, json_items(json_get(d, "user_ids"))) {
        if (v->dave != nullptr) {
          dave_add_user(v->dave, snowflake_of(user_id));
        }
      }

      break;
    case OP_CLIENT_DISCONNECT:
      unassign(v, snowflake_of(json_get(d, "user_id")));

      if (v->dave != nullptr) {
        dave_remove_user(v->dave, snowflake_of(json_get(d, "user_id")));
      }

      changed(v);
      break;
    case OP_DAVE_PREPARE_TRANSITION:
      dave_prepare_transition(
        v->dave, (uint16_t)snowflake_of(json_get(d, "transition_id")),
        (uint16_t)json_get_coerce(d, "protocol_version", (uint64_t)0));
      break;
    case OP_DAVE_EXECUTE_TRANSITION:
      dave_execute_transition(
        v->dave, (uint16_t)snowflake_of(json_get(d, "transition_id")));
      changed(v);
      break;
    case OP_DAVE_PREPARE_EPOCH:
      dave_prepare_epoch(
        v->dave, json_get_coerce(d, "epoch", (uint64_t)0),
        (uint16_t)json_get_coerce(d, "protocol_version", (uint64_t)0));
      break;
    default:
      break;
  }

  cilo_arena_drop(&arena);
}

// a binary message is the server's sequence, an opcode, then mls bytes.
static void on_binary(void *ctx, const uint8_t *data, size_t len) {
  Voice *v = ctx;

  if (len < 3 || v->dave == nullptr) {
    return;
  }

  const uint8_t *body = data + 3;
  size_t size = len - 3;

  v->seq = data[0] << 8 | data[1];

  switch (data[2]) {
    case OP_MLS_EXTERNAL_SENDER:
      dave_external_sender(v->dave, body, size);
      break;
    case OP_MLS_PROPOSALS:
      dave_proposals(v->dave, body, size);
      break;
    case OP_MLS_ANNOUNCE_COMMIT:
      if (size >= 2) {
        dave_commit(v->dave, (uint16_t)(body[0] << 8 | body[1]), body + 2,
                    size - 2);
      }

      break;
    case OP_MLS_WELCOME:
      if (size >= 2) {
        dave_welcome(v->dave, (uint16_t)(body[0] << 8 | body[1]), body + 2,
                     size - 2);
      }

      break;
    default:
      break;
  }

  changed(v);
}

static void on_closed(void *ctx, int code, const char *reason) {
  Voice *v = ctx;

  platform_ws_free(v->ws);
  v->ws = nullptr;
  platform_timer_cancel(v->heartbeat);
  v->heartbeat = nullptr;

  switch (code) {
    case 4014:
      end(v, "You were disconnected from the call.");
      return;
    case 4022:
      end(v, "The call ended.");
      return;
    case 4004:
    case 4006:
    case 4009:
    case 4011:
      end(v, "The voice session is no longer valid.");
      return;
    case 4016:
    case 4017:
      end(v, "The call needs an encryption Quarrel does not have.");
      return;
    case 4021:
      end(v, "Discord is rate limiting voice connections.");
      return;
    default:
      break;
  }

  // anything else is the connection or the server: the call is taken up
  // again where it was, a few times.
  if (v->described && v->resumes < RESUMES_MAX) {
    v->resumes++;
    v->status = VOICE_RESUMING;
    platform_timer_cancel(v->retry);
    v->retry = platform_timer(1, on_retry, v);
    changed(v);
    return;
  }

  char why[160];

  snprintf(why, sizeof why, "The voice connection closed. %s",
           reason != nullptr ? reason : "");
  fail(v, why);
}

static void open_socket(Voice *v) {
  char url[320];
  bool bare = strstr(v->endpoint, "://") == nullptr;

  snprintf(url, sizeof url, "%s%s%s", bare ? "wss://" : "", v->endpoint,
           VOICE_QUERY);

  PlatformHeader headers[] = {
    {"Origin", "https://discord.com"},
    {"User-Agent", api_user_agent()},
  };
  PlatformWebSocketEvents events = {
    .ctx = v,
    .text = on_text,
    .binary = on_binary,
    .closed = on_closed,
  };

  v->ws = platform_ws_connect(url, headers, countof(headers), &events);
  platform_timer_cancel(v->watchdog);
  v->watchdog = platform_timer(HANDSHAKE_TIMEOUT, on_watchdog, v);
}

/* ========================================================================== */
/* the public side.                                                           */
/* ========================================================================== */

Voice *voice_new(Snowflake self_id, const VoiceEvents *e) {
  Voice *v = calloc(1, sizeof *v);
  Link *link = calloc(1, sizeof *link);

  if (v == nullptr || link == nullptr) {
    free(v);
    free(link);
    return nullptr;
  }

  atomic_init(&link->refs, 1);
  link->voice = v;
  v->link = link;
  v->e = *e;
  v->self_id = self_id;
  v->udp = v->wake[0] = v->wake[1] = -1;
  v->seq = -1;
  v->latency = -1;
  return v;
}

static void forget(Voice *v) {
  free(v->session_id);
  free(v->token);
  free(v->endpoint);
  v->session_id = v->token = v->endpoint = nullptr;
}

void voice_free(Voice *v) {
  if (v == nullptr) {
    return;
  }

  teardown(v);
  forget(v);
  v->link->voice = nullptr;
  link_drop(v->link);
  free(v);
}

void voice_connect(Voice *v, Snowflake guild_id, Snowflake channel_id,
                   const char *session_id, const char *token,
                   const char *endpoint) {
  DaveEvents events = {
    .ctx = v,
    .key_package = dave_key_package,
    .commit_welcome = dave_commit_welcome,
    .ready_for_transition = dave_ready,
    .invalid_commit_welcome = dave_invalid,
  };

  teardown(v);
  forget(v);
  v->guild_id = guild_id;
  v->channel_id = channel_id;
  v->session_id = strdup(session_id);
  v->token = strdup(token);
  v->endpoint = strdup(endpoint);
  v->dave = dave_new(v->self_id, channel_id, &events);
  v->error[0] = '\0';
  v->no_microphone = false;

  if (v->dave == nullptr) {
    fail(v, "The call's encryption could not be set up.");
    return;
  }

  v->status = VOICE_CONNECTING;
  open_socket(v);
  changed(v);
}

void voice_disconnect(Voice *v) {
  teardown(v);
  forget(v);
  v->channel_id = v->guild_id = 0;
  v->status = VOICE_IDLE;
  v->error[0] = '\0';
  changed(v);
}

VoiceStatus voice_status(const Voice *v) {
  return v->status;
}

const char *voice_error(const Voice *v) {
  return v->error;
}

Snowflake voice_channel(const Voice *v) {
  return v->channel_id;
}

double voice_latency(const Voice *v) {
  return v->latency;
}

bool voice_encrypted(const Voice *v) {
  return v->dave != nullptr && dave_active(v->dave);
}

void voice_set_mute(Voice *v, bool mute) {
  atomic_store(&v->mute, mute);
}

void voice_set_deaf(Voice *v, bool deaf) {
  atomic_store(&v->deaf, deaf);
}

float voice_input_level(const Voice *v) {
  return v->audio != nullptr && !atomic_load(&v->mute)
           ? atomic_load_explicit(&v->level, memory_order_relaxed)
           : 0;
}

bool voice_microphone_starting(const Voice *v) {
  return v->audio != nullptr && !v->heard;
}

bool voice_no_microphone(const Voice *v) {
  return v->no_microphone;
}

bool voice_speaking(const Voice *v, Snowflake user_id) {
  if (v->status != VOICE_CONNECTED) {
    return false;
  }

  if (user_id == v->self_id) {
    return atomic_load(&v->speaking);
  }

  uint64_t now = now_ms();

  for (size_t i = 0; i < STREAMS_MAX; i++) {
    const Stream *s = &v->streams[i];

    if (atomic_load(&s->ssrc) != 0 && atomic_load(&s->user_id) == user_id) {
      return now - atomic_load(&s->heard_at) < (uint64_t)(SPOKE_FOR * 1000);
    }
  }

  return false;
}
