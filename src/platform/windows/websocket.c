#include <curl/curl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

/* ========================================================================== */
/* websockets through curl, each on a thread of its own that blocks in       */
/* curl_multi_poll on the socket until it can be read or what the main thread */
/* asked wakes it. what comes is posted to the main thread, which is the only */
/* one that calls back.                                                       */
/* ========================================================================== */

// ready is several megabytes.
constexpr size_t MESSAGE_MOST = 256 << 20;
// what a read asks for at once, and the most of a message the buffer keeps
// for the next one: a larger one is handed over as it is.
constexpr size_t READ_CHUNK = 64 << 10;
constexpr size_t KEPT_MOST = 1 << 20;
// a close that was asked for waits this long for the peer's.
constexpr ULONGLONG CLOSE_WAIT_MS = 5000;
constexpr long TIMEOUT_SECONDS = 30;

typedef struct {
  uint8_t *data;
  size_t len, sent;
  unsigned flags;
} Out;

typedef struct {
  Out *v;
  size_t len, head, cap;
} Outs;

struct PlatformWebSocket {
  // the main thread's and the socket's own, each holding one, and every
  // event on its way.
  atomic_int refs;
  CURLM *multi;

  // the main thread's.
  PlatformWebSocketEvents events;
  bool opened, closing, finished, dropped;

  // what the main thread hands over, under the lock.
  SRWLOCK lock;
  Outs asked;
  bool close_asked;
  int close_code;
  char close_reason[124];
  atomic_bool stop;

  // the socket's thread's.
  CURL *easy;
  struct curl_slist *headers;
  char error[CURL_ERROR_SIZE];
  curl_socket_t fd;
  Outs out;
  uint8_t *message;
  size_t len, cap;
  unsigned kind;
  uint8_t close_payload[125];
  size_t close_len;
  ULONGLONG close_by;
};

typedef enum : uint8_t {
  OPENED,
  TEXT,
  BINARY,
  CLOSED,
} Kind;

typedef struct {
  PlatformWebSocket *ws;
  Kind kind;
  uint8_t *data;
  size_t len;
  int code;
  char reason[124];
} Event;

static void drop_outs(Outs *o) {
  for (size_t i = o->head; i < o->len; i++) {
    free(o->v[i].data);
  }

  free(o->v);
  *o = (Outs){};
}

static void release(PlatformWebSocket *ws) {
  if (atomic_fetch_sub(&ws->refs, 1) != 1) {
    return;
  }

  curl_multi_cleanup(ws->multi);
  drop_outs(&ws->asked);
  free(ws);
}

static void deliver(void *ctx) {
  Event *e = ctx;
  PlatformWebSocket *ws = e->ws;
  const PlatformWebSocketEvents *on = &ws->events;

  // a callback may free the socket, which the event keeps until it is done.
  if (!ws->dropped && !ws->finished) {
    switch (e->kind) {
      case OPENED:
        ws->opened = true;

        if (on->open != nullptr) {
          on->open(on->ctx);
        }

        break;
      case TEXT:
        if (on->text != nullptr) {
          on->text(on->ctx, (const char *)e->data, e->len);
        }

        break;
      case BINARY:
        if (on->binary != nullptr) {
          on->binary(on->ctx, e->data, e->len);
        }

        break;
      case CLOSED:
        ws->finished = true;
        ws->opened = false;

        if (on->closed != nullptr) {
          on->closed(on->ctx, e->code, e->reason);
        }

        break;
    }
  }

  free(e->data);
  free(e);
  release(ws);
}

static void post(PlatformWebSocket *ws, Kind kind, uint8_t *data, size_t len,
                 int code, const char *reason) {
  Event *e = calloc(1, sizeof *e);

  if (e == nullptr) {
    abort();
  }

  *e = (Event){.ws = ws, .kind = kind, .data = data, .len = len, .code = code};
  snprintf(e->reason, sizeof e->reason, "%s", reason ? reason : "");
  atomic_fetch_add(&ws->refs, 1);
  platform_post(deliver, e);
}

/* ========================================================================== */
/* the socket's thread.                                                       */
/* ========================================================================== */

typedef struct {
  int code;
  char reason[124];
} End;

static bool push(Outs *o, Out out) {
  if (o->len == o->cap) {
    size_t cap = o->cap ? o->cap * 2 : 8;
    Out *grown = realloc(o->v, cap * sizeof *grown);

    if (grown == nullptr) {
      return false;
    }

    o->v = grown;
    o->cap = cap;
  }

  o->v[o->len++] = out;
  return true;
}

// what the main thread asked for, taken in the order it came.
static void take(PlatformWebSocket *ws, bool *close_asked, End *asked) {
  AcquireSRWLockExclusive(&ws->lock);

  for (size_t i = ws->asked.head; i < ws->asked.len; i++) {
    if (!push(&ws->out, ws->asked.v[i])) {
      free(ws->asked.v[i].data);
    }
  }

  ws->asked.len = ws->asked.head = 0;
  *close_asked = ws->close_asked;
  asked->code = ws->close_code;
  snprintf(asked->reason, sizeof asked->reason, "%s", ws->close_reason);
  ReleaseSRWLockExclusive(&ws->lock);
}

// false when the socket would take no more for now.
static bool flush(PlatformWebSocket *ws) {
  Outs *o = &ws->out;

  while (o->head < o->len) {
    Out *f = &o->v[o->head];
    size_t n = 0;
    CURLcode rc = curl_ws_send(ws->easy, f->data + f->sent, f->len - f->sent,
                               &n, 0, f->flags);

    f->sent += n;

    // curl keeps what it took of the frame and wants the rest next time.
    if (rc == CURLE_AGAIN || (rc == CURLE_OK && f->sent < f->len)) {
      return false;
    }

    // a broken connection is found by the read that follows.
    if (rc != CURLE_OK) {
      break;
    }

    free(f->data);
    o->head++;
  }

  for (size_t i = o->head; i < o->len; i++) {
    free(o->v[i].data);
  }

  o->len = o->head = 0;
  return true;
}

static void send_close(PlatformWebSocket *ws, int code, const char *reason) {
  uint8_t *payload = malloc(125);
  size_t len = code != 0 ? 2 + strlen(reason) : 0;

  if (payload == nullptr) {
    return;
  }

  payload[0] = (uint8_t)(code >> 8);
  payload[1] = (uint8_t)code;
  memcpy(payload + 2, reason, len > 2 ? len - 2 : 0);

  if (!push(&ws->out, (Out){payload, len, 0, CURLWS_CLOSE})) {
    free(payload);
  }
}

static bool grow_message(PlatformWebSocket *ws) {
  if (ws->cap - ws->len >= READ_CHUNK / 4) {
    return true;
  }

  size_t cap = ws->cap ? ws->cap * 2 : READ_CHUNK;
  uint8_t *grown = realloc(ws->message, cap);

  if (grown == nullptr) {
    return false;
  }

  ws->message = grown;
  ws->cap = cap;
  return true;
}

static void hand_over(PlatformWebSocket *ws) {
  size_t len = ws->len;
  uint8_t *data = nullptr;

  if (len > KEPT_MOST) {
    data = ws->message;
    ws->message = nullptr;
    ws->cap = 0;
  }
  else if ((data = malloc(len ? len : 1)) != nullptr) {
    memcpy(data, ws->message, len);
  }

  ws->len = 0;

  if (data != nullptr) {
    post(ws, ws->kind & CURLWS_TEXT ? TEXT : BINARY, data, len, 0, nullptr);
  }
}

// reads what came until the socket has nothing more, handing over every
// message once whole. false once the connection is over, `end` then says
// why.
static bool receive(PlatformWebSocket *ws, bool closing, End *end) {
  for (;;) {
    if (!grow_message(ws)) {
      *end = (End){1009, "out of memory"};
      return false;
    }

    const struct curl_ws_frame *meta = nullptr;
    size_t n = 0;
    CURLcode rc = curl_ws_recv(ws->easy, ws->message + ws->len,
                               ws->cap - ws->len, &n, &meta);

    if (rc == CURLE_AGAIN) {
      return true;
    }

    if (rc == CURLE_GOT_NOTHING) {
      *end = (End){1006, "connection closed"};
      return false;
    }

    if (rc != CURLE_OK) {
      end->code = 1006;
      snprintf(end->reason, sizeof end->reason, "%s",
               ws->error[0] ? ws->error : curl_easy_strerror(rc));
      return false;
    }

    if (meta->flags & CURLWS_CLOSE) {
      size_t room = sizeof ws->close_payload - ws->close_len;

      n = n < room ? n : room;
      memcpy(ws->close_payload + ws->close_len, ws->message + ws->len, n);
      ws->close_len += n;

      if (meta->bytesleft != 0) {
        continue;
      }

      *end = (End){1005, ""};

      if (ws->close_len >= 2) {
        end->code = ws->close_payload[0] << 8 | ws->close_payload[1];
        memcpy(end->reason, ws->close_payload + 2, ws->close_len - 2);
        end->reason[ws->close_len - 2] = '\0';
      }

      // the peer's close is answered with its own code.
      if (!closing) {
        send_close(ws, ws->close_len >= 2 ? end->code : 0, "");
        flush(ws);
      }

      return false;
    }

    // curl answers the pings itself.
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
      continue;
    }

    ws->kind = (unsigned)meta->flags;
    ws->len += n;

    if (ws->len > MESSAGE_MOST) {
      *end = (End){1009, "message too big"};
      return false;
    }

    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
      hand_over(ws);
    }
  }
}

// connects and upgrades, false when it could not or was told to stop.
static bool open_socket(PlatformWebSocket *ws, End *end) {
  CURLMsg *m = nullptr;
  int left = 0, running = 1;

  if (curl_multi_add_handle(ws->multi, ws->easy) != CURLM_OK) {
    *end = (End){1006, "could not connect"};
    return false;
  }

  while (m == nullptr) {
    bool close_asked = false;
    End asked;

    take(ws, &close_asked, &asked);

    // a close asked for while it opened ends it with what was asked.
    if (atomic_load(&ws->stop) || close_asked) {
      *end = asked;
      return false;
    }

    curl_multi_perform(ws->multi, &running);

    while ((m = curl_multi_info_read(ws->multi, &left)) != nullptr &&
           m->msg != CURLMSG_DONE) {}

    if (m == nullptr) {
      curl_multi_poll(ws->multi, nullptr, 0, 1000, nullptr);
    }
  }

  long status = 0;
  curl_socket_t fd = CURL_SOCKET_BAD;

  curl_easy_getinfo(ws->easy, CURLINFO_RESPONSE_CODE, &status);

  if (m->data.result != CURLE_OK) {
    end->code = 1006;
    snprintf(end->reason, sizeof end->reason, "%s",
             ws->error[0] ? ws->error : curl_easy_strerror(m->data.result));
    return false;
  }

  if (status != 101 ||
      curl_easy_getinfo(ws->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK ||
      fd == CURL_SOCKET_BAD) {
    end->code = 1006;
    snprintf(end->reason, sizeof end->reason, "the server answered %ld",
             status);
    return false;
  }

  ws->fd = fd;
  return true;
}

static void talk(PlatformWebSocket *ws, End *end) {
  bool closing = false;
  End asked = {};

  while (!atomic_load(&ws->stop)) {
    bool close_asked = false;

    take(ws, &close_asked, &asked);

    if (close_asked && !closing) {
      closing = true;
      send_close(ws, asked.code, asked.reason);
      ws->close_by = GetTickCount64() + CLOSE_WAIT_MS;
    }

    bool drained = flush(ws);

    // what came with the upgrade waits in curl, not on the socket.
    if (!receive(ws, closing, end)) {
      return;
    }

    ULONGLONG now = GetTickCount64();

    if (closing && now >= ws->close_by) {
      *end = asked;
      return;
    }

    struct curl_waitfd wait = {
      .fd = ws->fd,
      .events = CURL_WAIT_POLLIN | (drained ? 0 : CURL_WAIT_POLLOUT),
    };
    int ms = closing ? (int)(ws->close_by - now) : 60'000;

    curl_multi_poll(ws->multi, &wait, 1, ms, nullptr);
  }
}

static DWORD WINAPI run(void *arg) {
  PlatformWebSocket *ws = arg;
  End end = {1006, ""};

  if (open_socket(ws, &end)) {
    post(ws, OPENED, nullptr, 0, 0, nullptr);
    talk(ws, &end);
  }

  if (!atomic_load(&ws->stop)) {
    post(ws, CLOSED, nullptr, 0, end.code, end.reason);
  }

  curl_multi_remove_handle(ws->multi, ws->easy);
  curl_easy_cleanup(ws->easy);
  curl_slist_free_all(ws->headers);
  drop_outs(&ws->out);
  free(ws->message);
  release(ws);
  return 0;
}

/* ========================================================================== */
/* the main thread's.                                                         */
/* ========================================================================== */

// a refused upgrade has a body, which goes nowhere.
static size_t drop_body(char *, size_t size, size_t n, void *) {
  return size * n;
}

static bool setup(PlatformWebSocket *ws, const char *url,
                  const PlatformHeader *headers, size_t nheaders) {
  static bool started;

  if (!started) {
    started = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  }

  ws->multi = curl_multi_init();
  ws->easy = curl_easy_init();

  if (ws->multi == nullptr || ws->easy == nullptr) {
    return false;
  }

  for (size_t i = 0; i < nheaders; i++) {
    if (headers[i].name == nullptr || headers[i].value == nullptr) {
      continue;
    }

    size_t len = strlen(headers[i].name) + strlen(headers[i].value) + 3;
    char *line = malloc(len);

    if (line == nullptr) {
      return false;
    }

    // curl sends a header without a value when it ends in a semicolon.
    snprintf(line, len, headers[i].value[0] ? "%s: %s" : "%s;", headers[i].name,
             headers[i].value);
    ws->headers = curl_slist_append(ws->headers, line);
    free(line);
  }

  curl_easy_setopt(ws->easy, CURLOPT_URL, url ? url : "");
  curl_easy_setopt(ws->easy, CURLOPT_HTTPHEADER, ws->headers);
  curl_easy_setopt(ws->easy, CURLOPT_PROTOCOLS_STR, "ws,wss");
  curl_easy_setopt(ws->easy, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(ws->easy, CURLOPT_WRITEFUNCTION, drop_body);
  curl_easy_setopt(ws->easy, CURLOPT_CONNECTTIMEOUT, TIMEOUT_SECONDS);
  curl_easy_setopt(ws->easy, CURLOPT_TIMEOUT, TIMEOUT_SECONDS);
  curl_easy_setopt(ws->easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(ws->easy, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(ws->easy, CURLOPT_ERRORBUFFER, ws->error);
  // the certificates windows trusts, which an installed Quarrel has no
  // bundle of msys2's for.
  curl_easy_setopt(ws->easy, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
  return true;
}

PlatformWebSocket *platform_ws_connect(const char *url,
                                       const PlatformHeader *headers,
                                       size_t nheaders,
                                       const PlatformWebSocketEvents *events) {
  PlatformWebSocket *ws = calloc(1, sizeof *ws);

  if (ws == nullptr) {
    return nullptr;
  }

  ws->events = *events;
  ws->fd = CURL_SOCKET_BAD;
  atomic_init(&ws->refs, 2);

  HANDLE thread = setup(ws, url, headers, nheaders)
                    ? CreateThread(nullptr, 0, run, ws, 0, nullptr)
                    : nullptr;

  if (thread == nullptr) {
    // the socket's thread never ran: its share is the event's.
    curl_easy_cleanup(ws->easy);
    curl_slist_free_all(ws->headers);
    ws->easy = nullptr;
    ws->headers = nullptr;
    post(ws, CLOSED, nullptr, 0, 1006, "no curl");
    release(ws);
    return ws;
  }

  CloseHandle(thread);
  return ws;
}

static bool hand(PlatformWebSocket *ws, const void *data, size_t len,
                 unsigned flags) {
  Out o = {malloc(len ? len : 1), len, 0, flags};

  if (o.data == nullptr) {
    return false;
  }

  memcpy(o.data, data, len);
  AcquireSRWLockExclusive(&ws->lock);

  bool queued = push(&ws->asked, o);

  ReleaseSRWLockExclusive(&ws->lock);

  if (!queued) {
    free(o.data);
    return false;
  }

  curl_multi_wakeup(ws->multi);
  return true;
}

static bool is_open(const PlatformWebSocket *ws) {
  return ws != nullptr && ws->opened && !ws->closing && !ws->finished &&
         !ws->dropped;
}

bool platform_ws_send_binary(PlatformWebSocket *ws, const uint8_t *data,
                             size_t len) {
  return is_open(ws) && hand(ws, data, len, CURLWS_BINARY);
}

bool platform_ws_send_text(PlatformWebSocket *ws, const char *text) {
  return is_open(ws) && text != nullptr &&
         hand(ws, text, strlen(text), CURLWS_TEXT);
}

void platform_ws_close(PlatformWebSocket *ws, int code, const char *reason) {
  if (ws == nullptr || ws->finished || ws->closing || ws->dropped) {
    return;
  }

  ws->closing = true;
  AcquireSRWLockExclusive(&ws->lock);
  ws->close_asked = true;
  ws->close_code = code;
  snprintf(ws->close_reason, sizeof ws->close_reason, "%.*s", 123,
           reason ? reason : "");
  ReleaseSRWLockExclusive(&ws->lock);
  curl_multi_wakeup(ws->multi);
}

void platform_ws_free(PlatformWebSocket *ws) {
  if (ws == nullptr || ws->dropped) {
    return;
  }

  ws->dropped = true;
  ws->opened = false;
  atomic_store(&ws->stop, true);

  if (ws->multi != nullptr) {
    curl_multi_wakeup(ws->multi);
  }

  release(ws);
}
