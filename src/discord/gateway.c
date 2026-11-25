#include "discord/gateway.h"

#include <stdcountof.h>
#include <stdio.h>

#ifdef QUARREL_ZSTD
#include <zstd.h>
#else
#include <zlib.h>
#endif

#include "discord/api.h"
#include "discord/etf.h"
#include "platform/platform.h"

#define GATEWAY_URL "wss://gateway.discord.gg"

// the desktop client's choices: etf since it ships erlpack, and zstd since
// it ships discord_zstd. zlib is what is left when zstd is not around.
#ifdef QUARREL_ZSTD
#define GATEWAY_QUERY "/?encoding=etf&v=9&compress=zstd-stream"
#else
#define GATEWAY_QUERY "/?encoding=etf&v=9&compress=zlib-stream"
#endif

// the capabilities the desktop client identifies with, 1734653 in its
// bundle, so that the session looks like one of its own. what the store has
// to take for it:
//   USER_SETTINGS_PROTO      1 << 9   the settings come as a protobuf, see
//                                     settings_proto.c.
//   NON_CHANNEL_READ_STATES  1 << 7   read states of other kinds come too,
//                                     the store skips them.
//   CLIENT_STATE_V2          1 << 10  guilds would come as partials if
//                                     guild_versions named any. none are.
// bits 17, 19 and 20 are sent by the client and not documented.
const uint32_t GATEWAY_CAPABILITIES = 1734653;

constexpr double BACKOFF_FIRST = 1;
constexpr double BACKOFF_MAX = 60;
// how long discord gets to say hello or to answer an identify.
constexpr double HANDSHAKE_TIMEOUT = 30;
// learned from the client's GatewaySocket. when the network changes it
// beats at once and gives discord this long to answer before it takes the
// connection for dead: 5 s once a network is back or the machine woke up,
// 15 s when the network is said to be gone, which may be wrong.
constexpr double PROBE_ONLINE = 5;
constexpr double PROBE_OFFLINE = 15;
// a session is resumed when its last heartbeat was answered within 3
// minutes, and identified anew past that or past 4 failed attempts.
constexpr double RESUME_WITHIN = 180;
constexpr int FAILS_MAX = 4;

enum {
  OP_DISPATCH = 0,
  OP_HEARTBEAT = 1,
  OP_IDENTIFY = 2,
  OP_PRESENCE_UPDATE = 3,
  OP_VOICE_STATE_UPDATE = 4,
  OP_RESUME = 6,
  OP_RECONNECT = 7,
  OP_REQUEST_GUILD_MEMBERS = 8,
  OP_INVALID_SESSION = 9,
  OP_HELLO = 10,
  OP_HEARTBEAT_ACK = 11,
  OP_REQUEST_FORUM_UNREADS = 28,
  OP_GUILD_SUBSCRIPTIONS_BULK = 37,
};

struct Gateway {
  GatewayEvents events;
  char *token;
  char *url_override;
  GatewayStatus status;

  PlatformWebSocket *ws;
  PlatformTimer *heartbeat, *retry, *watchdog;
  // runs out when a heartbeat sent for a change of network stays unanswered.
  PlatformTimer *probe;

  // the session to resume, null when the next connection identifies.
  char *session_id, *resume_url;
  // the last sequence number received, negative before the first.
  int64_t seq;

  double heartbeat_interval;
  double heartbeat_sent_at, latency;
  // the last heartbeat was answered.
  bool acked;
  // when one last was, or the connection last tried to resume, 0 for never.
  double acked_at;
  double backoff;
  // attempts that had to wait since the last session, and whether this one
  // got its hello.
  int fails;
  bool greeted;

  // the decompression stream of the connection and what it has produced of
  // the message being received.
#ifdef QUARREL_ZSTD
  ZSTD_DStream *zstd;
#else
  z_stream inflate;
  bool inflating;
#endif
  char *message;
  size_t message_len, message_cap;
};

static void connect_now(Gateway *g);
static void reconnect(Gateway *g, bool resume, double delay);

static void set_status(Gateway *g, GatewayStatus status) {
  if (g->status == status) {
    return;
  }

  g->status = status;

  if (g->events.status != nullptr) {
    g->events.status(g->events.ctx, status);
  }
}

static void cancel_timer(PlatformTimer **timer) {
  platform_timer_cancel(*timer);
  *timer = nullptr;
}

static void clear_session(Gateway *g) {
  free(g->session_id);
  free(g->resume_url);
  g->session_id = nullptr;
  g->resume_url = nullptr;
  g->seq = -1;
}

// drops the socket without waiting for it. `code` not 0 sends a close frame
// first: 1000 ends the session on discord's side, anything else keeps it.
static void drop_socket(Gateway *g, int code) {
  cancel_timer(&g->heartbeat);
  cancel_timer(&g->watchdog);
  cancel_timer(&g->probe);

  if (g->ws != nullptr) {
    if (code != 0) {
      platform_ws_close(g->ws, code, "");
    }

    platform_ws_free(g->ws);
    g->ws = nullptr;
  }

#ifdef QUARREL_ZSTD
  ZSTD_freeDStream(g->zstd);
  g->zstd = nullptr;
#else
  if (g->inflating) {
    inflateEnd(&g->inflate);
    g->inflating = false;
  }
#endif

  g->message_len = 0;
}

static bool send_payload(Gateway *g, cima_json_writer *w) {
  uint8_t *etf = nullptr;
  size_t len = 0;
  bool sent = g->ws != nullptr &&
              etf_from_json(cima_json_writer_text(w), w->len, &etf, &len) &&
              platform_ws_send_binary(g->ws, etf, len);

  free(etf);
  cima_json_writer_dispose(w);
  return sent;
}

static void begin_payload(cima_json_writer *w, int op) {
  json_begin_object(w);
  json_field(w, "op", op);
  json_key(w, "d");
}

static void send_heartbeat(Gateway *g) {
  cima_json_writer w = {};

  begin_payload(&w, OP_HEARTBEAT);

  if (g->seq >= 0) {
    json_put(&w, g->seq);
  }
  else {
    cima_json_write_null(&w);
  }

  json_end_object(&w);

  g->acked = false;
  g->heartbeat_sent_at = platform_now();
  send_payload(g, &w);
}

static void on_heartbeat(void *ctx) {
  Gateway *g = ctx;
  g->heartbeat = nullptr;

  // a heartbeat left unanswered means the connection is dead though the
  // socket has not noticed: it is dropped and the session resumed. one
  // sent for a change of network has its own, shorter, wait.
  if (!g->acked && g->probe == nullptr) {
    reconnect(g, true, 0);
    return;
  }

  if (g->acked) {
    send_heartbeat(g);
  }

  g->heartbeat = platform_timer(g->heartbeat_interval, on_heartbeat, g);
}

static void on_watchdog(void *ctx) {
  Gateway *g = ctx;
  g->watchdog = nullptr;

  // no hello in time: the same as a socket that failed, see on_closed.
  if (!g->greeted) {
    free(g->resume_url);
    g->resume_url = nullptr;
  }

  reconnect(g, g->session_id != nullptr, g->backoff);
}

static void on_probe(void *ctx) {
  Gateway *g = ctx;
  g->probe = nullptr;

  if (!g->acked) {
    reconnect(g, true, 0);
  }
}

// the client's expeditedHeartbeat.
static void probe(Gateway *g, double patience) {
  send_heartbeat(g);
  cancel_timer(&g->probe);
  g->probe = platform_timer(patience, on_probe, g);
}

// the client's networkStateChange, which it calls with 15 s when the
// browser says offline and 5 s when it says online, and its power monitor's
// resume, which beats with 5 s. connected is from the identify on. without
// a connection the client only acts once online: it forgets its backoff
// and connects at once, dropping an attempt under way.
void gateway_network_changed(Gateway *g, bool online, bool returned) {
  bool connected =
    g->status == GATEWAY_HANDSHAKING || g->status == GATEWAY_READY;

  if (g->status == GATEWAY_STOPPED) {
    return;
  }

  if (connected) {
    probe(g, online ? PROBE_ONLINE : PROBE_OFFLINE);
    return;
  }

  // an attempt made while the network only changed is left to end.
  if (!online || (g->status == GATEWAY_CONNECTING && !returned)) {
    return;
  }

  g->backoff = BACKOFF_FIRST;
  g->fails = 0;
  reconnect(g, g->session_id != nullptr, 0);
}

static void send_identify(Gateway *g) {
  cima_json_writer w = {};

  begin_payload(&w, OP_IDENTIFY);
  json_begin_object(&w);
  json_field(&w, "token", g->token);
  json_field(&w, "capabilities", GATEWAY_CAPABILITIES);
  json_key(&w, "properties");
  api_write_client_properties(&w, true);

  // a new session says nothing of the user's status: discord applies the
  // one in their settings.
  json_key(&w, "presence");
  json_begin_object(&w);
  json_field(&w, "status", "unknown");
  json_field(&w, "since", 0);
  json_key(&w, "activities");
  json_begin_array(&w);
  json_end_array(&w);
  json_field(&w, "afk", false);
  json_end_object(&w);

  json_field(&w, "compress", false);

  json_key(&w, "client_state");
  json_begin_object(&w);
  json_key(&w, "guild_versions");
  json_begin_object(&w);
  json_end_object(&w);
  json_end_object(&w);

  // a QosToken protobuf in base64: client_provided, field 1, holding
  // is_active, field 1, set. nothing is derived on a first launch.
  json_field(&w, "qos_token", "CgIIAQ==");

  json_end_object(&w);
  json_end_object(&w);
  send_payload(g, &w);
}

static void send_resume(Gateway *g) {
  cima_json_writer w = {};

  begin_payload(&w, OP_RESUME);
  json_begin_object(&w);
  json_field(&w, "token", g->token);
  json_field(&w, "session_id", g->session_id);
  json_field(&w, "seq", g->seq >= 0 ? g->seq : 0);
  json_end_object(&w);
  json_end_object(&w);
  send_payload(g, &w);
}

static void on_hello(Gateway *g, const cima_json *d) {
  double interval =
    json_get_coerce(d, "heartbeat_interval", (double)41250) / 1000;

  if (interval < 1) {
    interval = 1;
  }

  g->heartbeat_interval = interval;
  g->acked = true;
  g->greeted = true;

  // the client's _doResumeOrIdentify: a session left alone for too long
  // is not resumed.
  double now = platform_now();

  if (g->session_id != nullptr && g->acked_at != 0 &&
      now - g->acked_at > RESUME_WITHIN) {
    clear_session(g);
  }

  g->acked_at = now;

  // the first heartbeat comes after a random part of the interval, so
  // clients reconnecting together do not beat together.
  uint32_t random;
  platform_random(&random, sizeof random);

  double jitter = (double)random / 4294967296.0;

  cancel_timer(&g->heartbeat);
  g->heartbeat = platform_timer(interval * jitter, on_heartbeat, g);

  if (g->session_id != nullptr) {
    send_resume(g);
  }
  else {
    send_identify(g);
  }

  set_status(g, GATEWAY_HANDSHAKING);

  cancel_timer(&g->watchdog);
  g->watchdog = platform_timer(HANDSHAKE_TIMEOUT, on_watchdog, g);
}

static void on_dispatch(Gateway *g, const char *type, const cima_json *d) {
  if (type == nullptr) {
    return;
  }

  bool ready = strcmp(type, "READY") == 0;

  if (ready) {
    str_set(&g->session_id, json_get_or(d, "session_id", nullptr));
    str_set(&g->resume_url, json_get_or(d, "resume_gateway_url", nullptr));

    // discord may hand out a new token, the old one stops working later.
    if (json_get_or(d, "auth_token", nullptr) != nullptr) {
      str_set(&g->token, json_get_or(d, "auth_token", nullptr));
    }
  }

  if (g->events.dispatch != nullptr) {
    g->events.dispatch(g->events.ctx, type, d);
  }

  if (ready || strcmp(type, "RESUMED") == 0) {
    cancel_timer(&g->watchdog);
    g->backoff = BACKOFF_FIRST;
    g->fails = 0;
    set_status(g, GATEWAY_READY);
  }
}

static void on_payload(Gateway *g, const char *text, size_t len) {
  cilo_arena arena = {};
  const cima_json *payload = json_parse(&arena, text, len, nullptr);
  const cima_json *d = json_get(payload, "d");
  const cima_json *s = json_get(payload, "s");

  if (cima_json_is_number(s)) {
    g->seq = json_coerce(s, (int64_t)(g->seq));
  }

  switch (json_get_coerce(payload, "op", (int64_t)-1)) {
    case OP_DISPATCH: {
      const char *type = json_get_or(payload, "t", nullptr);

      if (type != nullptr && g->events.snapshot != nullptr &&
          (strcmp(type, "READY") == 0 ||
           strcmp(type, "READY_SUPPLEMENTAL") == 0)) {
        g->events.snapshot(g->events.ctx, type, text, len);
      }

      on_dispatch(g, type, d);
      break;
    }
    case OP_HEARTBEAT:
      send_heartbeat(g);
      break;
    case OP_HEARTBEAT_ACK:
      g->acked = true;
      g->acked_at = platform_now();
      g->latency = g->acked_at - g->heartbeat_sent_at;
      cancel_timer(&g->probe);
      break;
    case OP_HELLO:
      on_hello(g, d);
      break;
    case OP_RECONNECT:
      reconnect(g, true, 0);
      break;
    case OP_INVALID_SESSION: {
      // `d` tells whether the session may still be resumed. a new one is
      // asked for after a few seconds, as discord wants.
      bool resumable = json_coerce(d, false);
      uint32_t random;
      platform_random(&random, sizeof random);

      reconnect(g, resumable, 1 + (double)(random % 4000) / 1000);
      break;
    }
    default:
      break;
  }

  cilo_arena_drop(&arena);
}

// a message may not grow past this, whatever the stream says.
constexpr size_t MESSAGE_MAX = 256u << 20;
// the room kept from a message to the next. a ready takes megabytes, once
// a connection, and what follows it a few kilobytes.
constexpr size_t MESSAGE_KEPT = 256u << 10;

// makes room for more of the message, false once it is too large.
static bool message_room(Gateway *g) {
  if (g->message_cap - g->message_len >= 32768) {
    return true;
  }

  size_t cap = g->message_cap != 0 ? g->message_cap * 2 : 65536;

  if (cap > MESSAGE_MAX) {
    return false;
  }

  char *grown = realloc(g->message, cap);

  if (grown == nullptr) {
    cilo_oom(cap);
  }

  g->message = grown;
  g->message_cap = cap;
  return true;
}

#ifdef QUARREL_ZSTD

// zstd-stream: one stream lasts as long as the connection, and every
// websocket message holds one whole payload without ending a frame.
static bool decompress(Gateway *g, const uint8_t *data, size_t len,
                       bool *whole) {
  if (g->zstd == nullptr) {
    g->zstd = ZSTD_createDStream();

    if (g->zstd == nullptr) {
      return false;
    }
  }

  ZSTD_inBuffer in = {.src = data, .size = len};

  for (;;) {
    if (!message_room(g)) {
      return false;
    }

    ZSTD_outBuffer out = {
      .dst = g->message,
      .size = g->message_cap,
      .pos = g->message_len,
    };

    size_t result = ZSTD_decompressStream(g->zstd, &out, &in);

    g->message_len = out.pos;

    if (ZSTD_isError(result)) {
      return false;
    }

    // room left over means everything received came out.
    if (in.pos == in.size && out.pos < out.size) {
      break;
    }
  }

  *whole = true;
  return true;
}

#else

// zlib-stream: every message is a piece of one deflate stream that lasts as
// long as the connection, and a payload is whole once a piece ends with the
// sync flush marker 00 00 ff ff.
static bool decompress(Gateway *g, const uint8_t *data, size_t len,
                       bool *whole) {
  if (!g->inflating) {
    g->inflate = (z_stream){};

    if (inflateInit(&g->inflate) != Z_OK) {
      return false;
    }

    g->inflating = true;
  }

  g->inflate.next_in = (Bytef *)data;
  g->inflate.avail_in = (uInt)len;

  for (;;) {
    if (!message_room(g)) {
      return false;
    }

    size_t room = g->message_cap - g->message_len;

    g->inflate.next_out = (Bytef *)g->message + g->message_len;
    g->inflate.avail_out = (uInt)room;

    int result = inflate(&g->inflate, Z_SYNC_FLUSH);

    g->message_len += room - g->inflate.avail_out;

    if (result != Z_OK && result != Z_BUF_ERROR) {
      return false;
    }

    // room left over means everything received so far came out.
    if (g->inflate.avail_out != 0) {
      break;
    }
  }

  *whole = len >= 4 && data[len - 4] == 0x00 && data[len - 3] == 0x00 &&
           data[len - 2] == 0xff && data[len - 1] == 0xff;
  return true;
}

#endif

static void on_binary(void *ctx, const uint8_t *data, size_t len) {
  Gateway *g = ctx;
  bool whole = false;

  // a broken stream cannot be picked up again.
  if (!decompress(g, data, len, &whole)) {
    reconnect(g, g->session_id != nullptr, g->backoff);
    return;
  }

  if (!whole) {
    return;
  }

  char *json = nullptr;
  size_t json_len = 0;
  bool decoded =
    etf_to_json((const uint8_t *)g->message, g->message_len, &json, &json_len);

  g->message_len = 0;

  if (g->message_cap > MESSAGE_KEPT) {
    free(g->message);
    g->message = nullptr;
    g->message_cap = 0;
  }

  if (!decoded) {
    reconnect(g, g->session_id != nullptr, g->backoff);
    return;
  }

  // the payload is handled from a buffer of its own: handling it may drop
  // the connection and this stream with it.
  on_payload(g, json, json_len);
  free(json);
}

static void on_open(void *ctx) {
  Gateway *g = ctx;

  // hello is next, and the watchdog covers its absence.
  cancel_timer(&g->watchdog);
  g->watchdog = platform_timer(HANDSHAKE_TIMEOUT, on_watchdog, g);
}

static void on_closed(void *ctx, int code, const char *reason) {
  Gateway *g = ctx;

  // the socket is done, what is left is to forget it.
  drop_socket(g, 0);

  // the client's socket error: the next attempt goes back to the gateway
  // everyone starts from, the one the session named being perhaps what
  // cannot be reached.
  if (!g->greeted) {
    free(g->resume_url);
    g->resume_url = nullptr;
  }

  switch (code) {
    case 4004:
      clear_session(g);
      set_status(g, GATEWAY_STOPPED);

      if (g->events.fatal != nullptr) {
        g->events.fatal(g->events.ctx, GATEWAY_FATAL_AUTH, code, reason);
      }

      return;
    case 4010:
    case 4011:
    case 4012:
    case 4013:
    case 4014:
      clear_session(g);
      set_status(g, GATEWAY_STOPPED);

      if (g->events.fatal != nullptr) {
        g->events.fatal(g->events.ctx, GATEWAY_FATAL_REFUSED, code, reason);
      }

      return;
    case 1000:
    case 4007:
    case 4009:
      // the session is over: a normal close, a bad sequence or a time out.
      reconnect(g, false, g->backoff);
      return;
    default:
      reconnect(g, g->session_id != nullptr, g->backoff);
      return;
  }
}

static void on_retry(void *ctx) {
  Gateway *g = ctx;
  g->retry = nullptr;
  connect_now(g);
}

// drops the connection and opens another after `delay` seconds. without
// `resume` the session is forgotten and the next one identifies.
static void reconnect(Gateway *g, bool resume, double delay) {
  // a close code other than 1000 keeps the session alive for the resume.
  drop_socket(g, resume ? 4000 : 1000);
  cancel_timer(&g->retry);

  if (!resume) {
    clear_session(g);
  }

  if (delay <= 0) {
    connect_now(g);
    return;
  }

  // the client's _handleClose: past a few attempts the session is given up.
  if (++g->fails > FAILS_MAX) {
    clear_session(g);
  }

  // each failed attempt doubles the wait, up to a minute.
  g->backoff = g->backoff * 2 < BACKOFF_MAX ? g->backoff * 2 : BACKOFF_MAX;

  uint32_t random;
  platform_random(&random, sizeof random);

  double jitter = 0.75 + (double)(random % 1000) / 2000;

  set_status(g, GATEWAY_WAITING);
  g->retry = platform_timer(delay * jitter, on_retry, g);
}

static void connect_now(Gateway *g) {
  char url[512];
  const char *base = g->url_override != nullptr ? g->url_override
                     : g->resume_url != nullptr ? g->resume_url
                                                : GATEWAY_URL;

  snprintf(url, sizeof url, "%s%s", base, GATEWAY_QUERY);

  // a browser says where the page opening the socket came from.
  PlatformHeader headers[] = {
    {"Origin", "https://discord.com"},
    {"User-Agent", api_user_agent()},
  };

  PlatformWebSocketEvents events = {
    .ctx = g,
    .open = on_open,
    .binary = on_binary,
    .closed = on_closed,
  };

  set_status(g, GATEWAY_CONNECTING);

  g->greeted = false;
  g->ws = platform_ws_connect(url, headers, countof(headers), &events);
  g->watchdog = platform_timer(HANDSHAKE_TIMEOUT, on_watchdog, g);
}

Gateway *gateway_new(const char *token, const GatewayEvents *events) {
  Gateway *g = calloc(1, sizeof *g);

  if (g == nullptr) {
    cilo_oom(sizeof *g);
  }

  g->events = *events;
  g->token = str_dup(token != nullptr ? token : "");
  g->seq = -1;
  g->latency = -1;
  g->backoff = BACKOFF_FIRST;
  return g;
}

void gateway_free(Gateway *g) {
  if (g == nullptr) {
    return;
  }

  g->events = (GatewayEvents){};
  gateway_stop(g);
  free(g->token);
  free(g->url_override);
  free(g->message);
  free(g);
}

void gateway_start(Gateway *g) {
  if (g->status != GATEWAY_STOPPED) {
    return;
  }

  g->backoff = BACKOFF_FIRST;
  connect_now(g);
}

void gateway_stop(Gateway *g) {
  cancel_timer(&g->retry);
  drop_socket(g, 1000);
  clear_session(g);
  set_status(g, GATEWAY_STOPPED);
}

GatewayStatus gateway_status(const Gateway *g) {
  return g->status;
}

double gateway_latency(const Gateway *g) {
  return g->latency;
}

void gateway_set_token(Gateway *g, const char *token) {
  str_set(&g->token, token != nullptr ? token : "");
}

void gateway_set_url(Gateway *g, const char *url) {
  str_set(&g->url_override, url);
}

bool gateway_update_presence(Gateway *g, const char *status, bool afk) {
  if (g->status != GATEWAY_READY) {
    return false;
  }

  cima_json_writer w = {};
  bool idle = status != nullptr && strcmp(status, "idle") == 0;

  begin_payload(&w, OP_PRESENCE_UPDATE);
  json_begin_object(&w);
  json_field(&w, "status", status != nullptr ? status : "online");
  json_key(&w, "since");
  json_put(&w, idle ? (int64_t)(platform_now() * 1000) : 0);
  json_key(&w, "activities");
  json_begin_array(&w);
  json_end_array(&w);
  json_key(&w, "afk"), cima_json_write_bool(&w, afk);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}

bool gateway_voice_state(Gateway *g, Snowflake guild_id, Snowflake channel_id,
                         bool mute, bool deaf) {
  if (g->status != GATEWAY_READY) {
    return false;
  }

  cima_json_writer w = {};

  begin_payload(&w, OP_VOICE_STATE_UPDATE);
  json_begin_object(&w);
  json_key(&w, "guild_id");

  if (guild_id != 0) {
    snowflake_write(&w, guild_id);
  }
  else {
    cima_json_write_null(&w);
  }

  json_key(&w, "channel_id");

  if (channel_id != 0) {
    snowflake_write(&w, channel_id);
  }
  else {
    cima_json_write_null(&w);
  }

  json_key(&w, "self_mute"), cima_json_write_bool(&w, mute);
  json_key(&w, "self_deaf"), cima_json_write_bool(&w, deaf);
  json_field(&w, "self_video", false);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}

const char *gateway_session_id(const Gateway *g) {
  return g->session_id;
}

// learned from the web client: op 14 is deprecated, its replacement is op 37
// with the same per guild object under `subscriptions`, keyed by guild id.
// ranges are rows of the member list, sent in blocks of 100.
bool gateway_subscribe(Gateway *g, Snowflake guild_id, Snowflake channel_id,
                       const GatewayRange *ranges, size_t nranges) {
  if (g->status != GATEWAY_READY || guild_id == 0) {
    return false;
  }

  cima_json_writer w = {};
  char id[21];

  begin_payload(&w, OP_GUILD_SUBSCRIPTIONS_BULK);
  json_begin_object(&w);
  json_key(&w, "subscriptions");
  json_begin_object(&w);
  json_key(&w, snowflake_format(guild_id, id));
  json_begin_object(&w);
  json_field(&w, "typing", true);
  json_field(&w, "threads", true);
  json_field(&w, "activities", true);

  if (channel_id != 0 && nranges > 0) {
    json_key(&w, "channels");
    json_begin_object(&w);
    json_key(&w, snowflake_format(channel_id, id));
    json_begin_array(&w);

    for (size_t i = 0; i < nranges; i++) {
      json_begin_array(&w);
      json_put(&w, ranges[i].first);
      json_put(&w, ranges[i].last);
      json_end_array(&w);
    }

    json_end_array(&w);
    json_end_object(&w);
  }

  json_end_object(&w);
  json_end_object(&w);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}

// learned from the web client: at most 180 posts a request, answered by the
// FORUM_UNREADS dispatch.
bool gateway_request_forum_unreads(Gateway *g, Snowflake guild_id,
                                   Snowflake forum_id,
                                   const GatewayForumAck *acks, size_t count) {
  if (g->status != GATEWAY_READY || guild_id == 0 || count == 0) {
    return false;
  }

  cima_json_writer w = {};

  begin_payload(&w, OP_REQUEST_FORUM_UNREADS);
  json_begin_object(&w);
  json_key(&w, "guild_id"), snowflake_write(&w, guild_id);
  json_key(&w, "channel_id"), snowflake_write(&w, forum_id);
  json_key(&w, "threads");
  json_begin_array(&w);

  for (size_t i = 0; i < count && i < 180; i++) {
    json_begin_object(&w);
    json_key(&w, "thread_id"), snowflake_write(&w, acks[i].thread_id);
    json_key(&w, "ack_message_id"), snowflake_write(&w, acks[i].ack_id);
    json_end_object(&w);
  }

  json_end_array(&w);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}

bool gateway_request_members(Gateway *g, Snowflake guild_id,
                             const Snowflake *user_ids, size_t count) {
  if (g->status != GATEWAY_READY || guild_id == 0 || count == 0) {
    return false;
  }

  cima_json_writer w = {};

  begin_payload(&w, OP_REQUEST_GUILD_MEMBERS);
  json_begin_object(&w);
  json_key(&w, "guild_id");
  json_begin_array(&w);
  snowflake_write(&w, guild_id);
  json_end_array(&w);
  json_key(&w, "user_ids");
  json_begin_array(&w);

  for (size_t i = 0; i < count && i < 100; i++) {
    snowflake_write(&w, user_ids[i]);
  }

  json_end_array(&w);
  json_field(&w, "presences", true);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}

bool gateway_search_members(Gateway *g, Snowflake guild_id, const char *query,
                            uint32_t limit) {
  if (g->status != GATEWAY_READY || guild_id == 0 || query == nullptr) {
    return false;
  }

  cima_json_writer w = {};

  begin_payload(&w, OP_REQUEST_GUILD_MEMBERS);
  json_begin_object(&w);
  json_key(&w, "guild_id");
  json_begin_array(&w);
  snowflake_write(&w, guild_id);
  json_end_array(&w);
  json_field(&w, "query", query);
  json_field(&w, "limit", (int64_t)limit);
  json_field(&w, "presences", false);
  json_end_object(&w);
  json_end_object(&w);
  return send_payload(g, &w);
}
