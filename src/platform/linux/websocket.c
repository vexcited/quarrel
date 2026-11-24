#include <curl/curl.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// ready is several megabytes.
constexpr size_t MESSAGE_MOST = 256 << 20;
// what a read asks for at once, and what the buffer of a message shrinks
// back to once a large one was handed over.
constexpr size_t READ_CHUNK = 64 << 10;
constexpr size_t KEPT_MOST = 1 << 20;
// a close that was asked for waits this long for the peer's.
constexpr guint CLOSE_WAIT_MS = 5000;
constexpr long TIMEOUT_SECONDS = 30;

typedef struct {
  uint8_t *data;
  size_t len, sent;
  unsigned flags;
} Out;

struct PlatformWebSocket {
  CURL *easy;
  struct curl_slist *headers;
  PlatformWebSocketEvents events;
  char error[CURL_ERROR_SIZE];

  int fd;
  guint watch, close_timer, later;
  bool writing;

  // what waits to be sent, from `head` on.
  Out *out;
  size_t nout, head, out_cap;

  // the message coming in.
  uint8_t *message;
  size_t len, cap;
  unsigned kind;
  uint8_t close_payload[125];
  size_t close_len;

  int close_code;
  char close_reason[124];

  bool opened, finished, dropped, closing;
  // how deep in its own callbacks the socket is, freed once out of them.
  int busy;
};

// one multi handle opens every socket, as glib's loop says theirs are ready.
static CURLM *multi;
static guint multi_timer;

static void destroy(PlatformWebSocket *ws);

static void enter(PlatformWebSocket *ws) {
  ws->busy++;
}

// false when the socket was dropped meanwhile, freed then.
static bool leave(PlatformWebSocket *ws) {
  ws->busy--;

  if (ws->dropped && ws->busy == 0) {
    destroy(ws);
    return false;
  }

  return !ws->dropped;
}

static void unwatch(PlatformWebSocket *ws) {
  if (ws->watch != 0) {
    g_source_remove(ws->watch);
    ws->watch = 0;
  }
}

static void finish(PlatformWebSocket *ws, int code, const char *reason) {
  if (ws->finished || ws->dropped) {
    return;
  }

  ws->finished = true;
  ws->opened = false;
  unwatch(ws);

  if (ws->events.closed != nullptr) {
    ws->events.closed(ws->events.ctx, code, reason);
  }
}

static void finish_error(PlatformWebSocket *ws, CURLcode rc) {
  finish(ws, 1006, ws->error[0] ? ws->error : curl_easy_strerror(rc));
}

static gboolean on_socket_ready(int fd, GIOCondition cond, void *data);

static void watch(PlatformWebSocket *ws, bool writing) {
  if (ws->watch != 0 && ws->writing == writing) {
    return;
  }

  unwatch(ws);
  ws->writing = writing;
  ws->watch = g_unix_fd_add(ws->fd, G_IO_IN | (writing ? G_IO_OUT : 0),
                            on_socket_ready, ws);
}

static void flush(PlatformWebSocket *ws) {
  while (ws->head < ws->nout) {
    Out *o = &ws->out[ws->head];
    size_t n = 0;
    CURLcode rc = curl_ws_send(ws->easy, o->data + o->sent, o->len - o->sent,
                               &n, 0, o->flags);

    o->sent += n;

    if (rc == CURLE_AGAIN || (rc == CURLE_OK && o->sent < o->len)) {
      watch(ws, true);
      return;
    }

    // a broken connection is found by the read that follows.
    if (rc != CURLE_OK) {
      break;
    }

    free(o->data);
    ws->head++;
  }

  for (size_t i = ws->head; i < ws->nout; i++) {
    free(ws->out[i].data);
  }

  ws->nout = ws->head = 0;
  watch(ws, false);
}

static bool queue(PlatformWebSocket *ws, const void *data, size_t len,
                  unsigned flags) {
  if (ws->nout == ws->out_cap) {
    size_t cap = ws->out_cap ? ws->out_cap * 2 : 4;
    Out *grown = realloc(ws->out, cap * sizeof *grown);

    if (grown == nullptr) {
      return false;
    }

    ws->out = grown;
    ws->out_cap = cap;
  }

  Out o = {malloc(len ? len : 1), len, 0, flags};

  if (o.data == nullptr) {
    return false;
  }

  memcpy(o.data, data, len);
  ws->out[ws->nout++] = o;
  watch(ws, true);
  return true;
}

static bool send_frame(PlatformWebSocket *ws, const void *data, size_t len,
                       unsigned flags) {
  if (ws->head < ws->nout) {
    return queue(ws, data, len, flags);
  }

  // the queue is empty most of the time, and the frame goes out at once.
  size_t n = 0;
  CURLcode rc = curl_ws_send(ws->easy, data, len, &n, 0, flags);

  if (rc == CURLE_OK && n == len) {
    return true;
  }

  // curl keeps what it took of the frame and wants the rest next time.
  return (rc == CURLE_OK || rc == CURLE_AGAIN) &&
         queue(ws, (const uint8_t *)data + n, len - n, flags);
}

static void closed_by_peer(PlatformWebSocket *ws) {
  int code = 1005;
  char reason[124] = "";

  if (ws->close_len >= 2) {
    code = ws->close_payload[0] << 8 | ws->close_payload[1];
    memcpy(reason, ws->close_payload + 2, ws->close_len - 2);
  }

  // the peer's close is answered with its own code.
  if (!ws->closing) {
    send_frame(ws, ws->close_payload, ws->close_len >= 2 ? 2 : 0, CURLWS_CLOSE);
  }

  finish(ws, code, reason);
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

  ws->len = 0;

  if (ws->kind & CURLWS_TEXT) {
    if (ws->events.text != nullptr) {
      ws->events.text(ws->events.ctx, (const char *)ws->message, len);
    }
  }
  else if (ws->events.binary != nullptr) {
    ws->events.binary(ws->events.ctx, ws->message, len);
  }

  if (!ws->dropped && ws->cap > KEPT_MOST) {
    free(ws->message);
    ws->message = nullptr;
    ws->cap = 0;
  }
}

// reads what came until the socket has nothing more, handing over every
// message once whole.
static void receive(PlatformWebSocket *ws) {
  while (!ws->finished && !ws->dropped) {
    if (!grow_message(ws)) {
      finish(ws, 1009, "out of memory");
      return;
    }

    const struct curl_ws_frame *meta = nullptr;
    size_t n = 0;
    CURLcode rc = curl_ws_recv(ws->easy, ws->message + ws->len,
                               ws->cap - ws->len, &n, &meta);

    if (rc == CURLE_AGAIN) {
      return;
    }

    if (rc == CURLE_GOT_NOTHING) {
      finish(ws, 1006, "connection closed");
      return;
    }

    if (rc != CURLE_OK) {
      finish_error(ws, rc);
      return;
    }

    if (meta->flags & CURLWS_CLOSE) {
      size_t room = sizeof ws->close_payload - ws->close_len;

      n = n < room ? n : room;
      memcpy(ws->close_payload + ws->close_len, ws->message + ws->len, n);
      ws->close_len += n;

      if (meta->bytesleft == 0) {
        closed_by_peer(ws);
      }

      continue;
    }

    // curl answers the pings itself.
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
      continue;
    }

    ws->kind = (unsigned)meta->flags;
    ws->len += n;

    if (ws->len > MESSAGE_MOST) {
      finish(ws, 1009, "message too big");
      return;
    }

    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
      hand_over(ws);
    }
  }
}

static gboolean on_socket_ready(int, GIOCondition cond, void *data) {
  PlatformWebSocket *ws = data;

  enter(ws);

  if (cond & G_IO_OUT) {
    flush(ws);
  }

  if (!ws->dropped && !ws->finished) {
    receive(ws);
  }

  // a watch made again or taken away meanwhile was destroyed already.
  leave(ws);
  return G_SOURCE_CONTINUE;
}

static void opened(PlatformWebSocket *ws, CURLcode rc) {
  long status = 0;
  curl_socket_t fd = CURL_SOCKET_BAD;

  // a close asked for while it opened ended it already.
  if (ws->finished) {
    return;
  }

  curl_easy_getinfo(ws->easy, CURLINFO_RESPONSE_CODE, &status);

  if (rc != CURLE_OK) {
    finish_error(ws, rc);
    return;
  }

  if (status != 101 ||
      curl_easy_getinfo(ws->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK ||
      fd == CURL_SOCKET_BAD) {
    char why[64];

    snprintf(why, sizeof why, "the server answered %ld", status);
    finish(ws, 1006, why);
    return;
  }

  ws->fd = fd;
  ws->opened = true;
  watch(ws, false);

  if (ws->events.open != nullptr) {
    ws->events.open(ws->events.ctx);
  }

  // what came with the upgrade waits in curl, not on the socket.
  if (!ws->dropped) {
    receive(ws);
  }
}

static void check_done(void) {
  CURLMsg *m;
  int left = 0;

  while ((m = curl_multi_info_read(multi, &left)) != nullptr) {
    if (m->msg != CURLMSG_DONE) {
      continue;
    }

    PlatformWebSocket *ws = nullptr;
    CURLcode rc = m->data.result;

    curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &ws);
    enter(ws);
    opened(ws, rc);
    leave(ws);
  }
}

typedef struct {
  curl_socket_t fd;
  guint source;
} Socket;

static gboolean on_multi_ready(int fd, GIOCondition cond, void *) {
  int mask = (cond & G_IO_IN ? CURL_CSELECT_IN : 0) |
             (cond & G_IO_OUT ? CURL_CSELECT_OUT : 0) |
             (cond & (G_IO_ERR | G_IO_HUP) ? CURL_CSELECT_ERR : 0);
  int running = 0;

  curl_multi_socket_action(multi, fd, mask, &running);
  check_done();
  return G_SOURCE_CONTINUE;
}

static int on_multi_socket(CURL *, curl_socket_t fd, int what, void *,
                           void *assigned) {
  Socket *s = assigned;

  if (s != nullptr) {
    g_source_remove(s->source);
  }

  if (what == CURL_POLL_REMOVE) {
    free(s);
    curl_multi_assign(multi, fd, nullptr);
    return 0;
  }

  if (s == nullptr) {
    s = calloc(1, sizeof *s);
    s->fd = fd;
    curl_multi_assign(multi, fd, s);
  }

  GIOCondition cond =
    (what & CURL_POLL_IN ? G_IO_IN : 0) | (what & CURL_POLL_OUT ? G_IO_OUT : 0);

  s->source = g_unix_fd_add(fd, cond, on_multi_ready, nullptr);
  return 0;
}

static void on_multi_timeout(void *) {
  int running = 0;

  multi_timer = 0;
  curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_done();
}

static int on_multi_timer(CURLM *, long ms, void *) {
  if (multi_timer != 0) {
    g_source_remove(multi_timer);
    multi_timer = 0;
  }

  if (ms >= 0) {
    multi_timer = g_timeout_add_once((guint)ms, on_multi_timeout, nullptr);
  }

  return 0;
}

static bool start_multi(void) {
  if (multi != nullptr) {
    return true;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  multi = curl_multi_init();

  if (multi == nullptr) {
    return false;
  }

  curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, on_multi_socket);
  curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, on_multi_timer);
  return true;
}

// a refused upgrade has a body, which goes nowhere.
static size_t drop_body(char *, size_t size, size_t n, void *) {
  return size * n;
}

static void fail_later(void *data) {
  PlatformWebSocket *ws = data;

  ws->later = 0;
  enter(ws);
  finish(ws, ws->close_code, ws->close_reason);
  leave(ws);
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
  ws->fd = -1;
  ws->easy = start_multi() ? curl_easy_init() : nullptr;

  if (ws->easy == nullptr) {
    ws->close_code = 1006;
    snprintf(ws->close_reason, sizeof ws->close_reason, "no curl");
    ws->later = g_timeout_add_once(0, fail_later, ws);
    return ws;
  }

  for (size_t i = 0; i < nheaders; i++) {
    if (headers[i].name == nullptr || headers[i].value == nullptr) {
      continue;
    }

    // curl sends a header without a value when it ends in a semicolon.
    char *line = g_strdup_printf(headers[i].value[0] ? "%s: %s" : "%s;",
                                 headers[i].name, headers[i].value);

    ws->headers = curl_slist_append(ws->headers, line);
    g_free(line);
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
  curl_easy_setopt(ws->easy, CURLOPT_PRIVATE, ws);

  if (curl_multi_add_handle(multi, ws->easy) != CURLM_OK) {
    ws->close_code = 1006;
    snprintf(ws->close_reason, sizeof ws->close_reason, "could not connect");
    ws->later = g_timeout_add_once(0, fail_later, ws);
  }

  return ws;
}

bool platform_ws_send_binary(PlatformWebSocket *ws, const uint8_t *data,
                             size_t len) {
  return ws != nullptr && ws->opened && !ws->closing &&
         send_frame(ws, data, len, CURLWS_BINARY);
}

bool platform_ws_send_text(PlatformWebSocket *ws, const char *text) {
  return ws != nullptr && text != nullptr && ws->opened && !ws->closing &&
         send_frame(ws, text, strlen(text), CURLWS_TEXT);
}

static void close_waited(void *data) {
  PlatformWebSocket *ws = data;

  ws->close_timer = 0;
  enter(ws);
  finish(ws, ws->close_code, ws->close_reason);
  leave(ws);
}

void platform_ws_close(PlatformWebSocket *ws, int code, const char *reason) {
  if (ws == nullptr || ws->finished || ws->closing) {
    return;
  }

  ws->closing = true;
  ws->close_code = code;
  snprintf(ws->close_reason, sizeof ws->close_reason, "%s",
           reason ? reason : "");

  if (!ws->opened) {
    if (ws->later == 0) {
      ws->later = g_timeout_add_once(0, fail_later, ws);
    }

    return;
  }

  uint8_t payload[125] = {(uint8_t)(code >> 8), (uint8_t)code};
  size_t len = strlen(ws->close_reason);

  memcpy(payload + 2, ws->close_reason, len);
  send_frame(ws, payload, 2 + len, CURLWS_CLOSE);
  ws->close_timer = g_timeout_add_once(CLOSE_WAIT_MS, close_waited, ws);
}

static void destroy(PlatformWebSocket *ws) {
  unwatch(ws);

  if (ws->close_timer != 0) {
    g_source_remove(ws->close_timer);
  }

  if (ws->later != 0) {
    g_source_remove(ws->later);
  }

  if (ws->easy != nullptr) {
    curl_multi_remove_handle(multi, ws->easy);
    curl_easy_cleanup(ws->easy);
  }

  for (size_t i = ws->head; i < ws->nout; i++) {
    free(ws->out[i].data);
  }

  curl_slist_free_all(ws->headers);
  free(ws->out);
  free(ws->message);
  free(ws);
}

void platform_ws_free(PlatformWebSocket *ws) {
  if (ws == nullptr || ws->dropped) {
    return;
  }

  ws->dropped = true;
  ws->opened = false;

  if (ws->busy == 0) {
    destroy(ws);
  }
}
