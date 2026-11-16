#include "http.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ciphon/curl.h>
#include <cinc/cinc.h>

#include "platform/platform.h"

// transfers running at once, each on a curl handle of its own that keeps its
// connection alive.
constexpr size_t HTTP_CLIENTS = 6;
constexpr size_t HTTP_QUEUE = 1024;
constexpr size_t HTTP_DEFAULT_MAX = 64u << 20;

typedef struct {
  char *name, *value;
} Cookie;

typedef struct Job {
  HttpOp op;
  HttpDone done;
  void *ctx;
  bool cookies;
  // set on the main thread, read by the transfer.
  atomic_bool cancelled;

  ciphon_method method;
  char *url;
  ciphon_header *headers;
  size_t nheaders;
  uint8_t *body;
  size_t body_len;
  uint32_t timeout_ms;
  uint64_t max_len;

  // the answer, filled on the pool thread.
  ciphon_response response;
  ciphon_error error;
  char *message;
  char *received;
  size_t received_len, received_cap;
} Job;

typedef CINC_CHAN(Job *) Jobs;

static struct {
  bool started, stopped;
  pthread_t thread;
  Jobs jobs;
  cinc_group tasks;
  cinc_sem free_clients;

  pthread_mutex_t lock;
  ciphon_client clients[HTTP_CLIENTS];
  bool busy[HTTP_CLIENTS];

  // the requests whose callbacks may still run, main thread only.
  Job **live;
  size_t nlive, live_cap;
  HttpOp last_op;

  Cookie *cookies;
  size_t ncookies;
} http = {.lock = PTHREAD_MUTEX_INITIALIZER};

static char *copy(const char *s) {
  return s != nullptr ? strdup(s) : nullptr;
}

static void job_free(Job *job) {
  for (size_t i = 0; i < job->nheaders; i++) {
    free((char *)job->headers[i].name);
    free((char *)job->headers[i].value);
  }

  ciphon_response_dispose(&job->response);
  free(job->headers);
  free(job->url);
  free(job->body);
  free(job->message);
  free(job->received);
  free(job);
}

/* ========================================================================== */
/* cookies. curl handles are reset for each request and there are several,    */
/* so the session's cookies are kept here.                                    */
/* ========================================================================== */

static void cookie_store(const char *header) {
  const char *equals = strchr(header, '=');
  const char *end = strchr(header, ';');

  if (end == nullptr) {
    end = header + strlen(header);
  }

  if (equals == nullptr || equals > end || equals == header) {
    return;
  }

  size_t name_len = (size_t)(equals - header);
  size_t value_len = (size_t)(end - equals - 1);
  bool expired = strcasestr(end, "max-age=0") != nullptr || value_len == 0;

  for (size_t i = 0; i < http.ncookies; i++) {
    Cookie *c = &http.cookies[i];

    if (strlen(c->name) != name_len ||
        strncmp(c->name, header, name_len) != 0) {
      continue;
    }

    free(c->value);

    if (expired) {
      free(c->name);
      *c = http.cookies[--http.ncookies];
    }
    else {
      c->value = strndup(equals + 1, value_len);
    }

    return;
  }

  if (expired) {
    return;
  }

  Cookie *grown = realloc(http.cookies, (http.ncookies + 1) * sizeof *grown);

  if (grown == nullptr) {
    return;
  }

  http.cookies = grown;
  http.cookies[http.ncookies++] = (Cookie){
    .name = strndup(header, name_len),
    .value = strndup(equals + 1, value_len),
  };
}

// "a=1; b=2" from malloc, null without cookies.
static char *cookie_header(void) {
  size_t len = 0;

  for (size_t i = 0; i < http.ncookies; i++) {
    len += strlen(http.cookies[i].name) + strlen(http.cookies[i].value) + 3;
  }

  if (len == 0) {
    return nullptr;
  }

  char *out = malloc(len + 1);

  if (out == nullptr) {
    return nullptr;
  }

  size_t at = 0;

  for (size_t i = 0; i < http.ncookies; i++) {
    at +=
      (size_t)snprintf(out + at, len + 1 - at, "%s%s=%s", i != 0 ? "; " : "",
                       http.cookies[i].name, http.cookies[i].value);
  }

  return out;
}

/* ========================================================================== */
/* the transfer, on a thread of the blocking pool.                            */
/* ========================================================================== */

static bool on_body(void *userdata, const ciphon_response *,
                    const uint8_t *data, size_t len) {
  Job *job = userdata;

  if (atomic_load(&job->cancelled)) {
    return false;
  }

  if (job->received_len + len + 1 > job->received_cap) {
    size_t cap = job->received_cap != 0 ? job->received_cap * 2 : 16384;

    while (cap < job->received_len + len + 1) {
      cap *= 2;
    }

    char *grown = realloc(job->received, cap);

    if (grown == nullptr) {
      return false;
    }

    job->received = grown;
    job->received_cap = cap;
  }

  memcpy(job->received + job->received_len, data, len);
  job->received_len += len;
  return true;
}

static void perform(void *arg) {
  Job *job = arg;
  size_t slot = 0;

  // the semaphore let this transfer in, so one client is free.
  pthread_mutex_lock(&http.lock);

  while (slot + 1 < HTTP_CLIENTS && http.busy[slot]) {
    slot++;
  }

  http.busy[slot] = true;

  if (http.clients[slot].send == nullptr) {
    http.clients[slot] = ciphon_curl();
  }

  ciphon_client *client = &http.clients[slot];
  pthread_mutex_unlock(&http.lock);

  ciphon_request request = {
    .method = job->method,
    .url = job->url,
    .headers = job->headers,
    .headers_count = job->nheaders,
    .body = job->body,
    .body_len = job->body_len,
    .follow_redirects = true,
    .timeout_ms = job->timeout_ms,
    .connect_timeout_ms = 15000,
    .max_response_len = job->max_len,
    .protocols = CIPHON_PROTOCOL_HTTPS | CIPHON_PROTOCOL_HTTP,
  };

  if (atomic_load(&job->cancelled)) {
    job->error = CIPHON_ERROR_CANCELLED;
  }
  else {
    job->error = ciphon_stream(client, &request,
                               (ciphon_sink){.userdata = job, .write = on_body},
                               &job->response, &job->message);
  }

  pthread_mutex_lock(&http.lock);
  http.busy[slot] = false;
  pthread_mutex_unlock(&http.lock);
}

/* ========================================================================== */
/* the main thread's side.                                                    */
/* ========================================================================== */

static bool live_remove(Job *job) {
  for (size_t i = 0; i < http.nlive; i++) {
    if (http.live[i] == job) {
      http.live[i] = http.live[--http.nlive];
      return true;
    }
  }

  return false;
}

static void deliver(void *arg) {
  Job *job = arg;
  bool wanted = live_remove(job) && !atomic_load(&job->cancelled);

  if (!wanted) {
    job_free(job);
    return;
  }

  HttpResponse res = {};

  if (job->error != CIPHON_OK) {
    res.error = job->message != nullptr ? job->message : "the request failed";
  }
  else {
    // ciphon's headers and ours are the same pair of strings.
    static_assert(sizeof(HttpHeader) == sizeof(ciphon_header));

    res.status = job->response.status;
    res.headers = (const HttpHeader *)job->response.headers;
    res.nheaders = job->response.headers_count;
    res.body = job->received != nullptr ? job->received : "";
    res.body_len = job->received_len;

    if (job->received != nullptr) {
      job->received[job->received_len] = '\0';
    }

    for (size_t i = 0; job->cookies && i < res.nheaders; i++) {
      if (strcasecmp(res.headers[i].name, "set-cookie") == 0) {
        cookie_store(res.headers[i].value);
      }
    }
  }

  job->done(job->ctx, &res);
  job_free(job);
}

static void run_job(Job *job) {
  if (cinc_sem_acquire(&http.free_clients) == CINC_OK) {
    if (cinc_blocking(perform, job) != CINC_OK && job->error == CIPHON_OK) {
      job->error = CIPHON_ERROR_CANCELLED;
    }

    cinc_sem_release(&http.free_clients);
  }
  else {
    job->error = CIPHON_ERROR_CANCELLED;
  }

  platform_post(deliver, job);
}

CINC_SPAWNABLE(run_job, Job *);

static int serve(void *) {
  CINC_RANGE(job, http.jobs) {
    CINC_SPAWN_IN(&http.tasks, run_job(job));
  }

  cinc_group_wait(&http.tasks);
  return 0;
}

static void *runtime_thread(void *) {
  cinc_run(
    &(const cinc_options){.workers = 1, .blocking_threads = HTTP_CLIENTS},
    serve, nullptr);
  return nullptr;
}

static bool start(void) {
  if (http.started) {
    return !http.stopped;
  }

  http.started = true;
  http.jobs = CINC_MAKE_CHAN(Jobs, HTTP_QUEUE);
  http.free_clients = (cinc_sem){.permits = HTTP_CLIENTS};

  if (pthread_create(&http.thread, nullptr, runtime_thread, nullptr) != 0) {
    http.stopped = true;
  }

  return !http.stopped;
}

static ciphon_method method_of(const char *name) {
  static const struct {
    const char *name;
    ciphon_method method;
  } methods[] = {
    {"GET", CIPHON_METHOD_GET},     {"HEAD", CIPHON_METHOD_HEAD},
    {"POST", CIPHON_METHOD_POST},   {"PUT", CIPHON_METHOD_PUT},
    {"PATCH", CIPHON_METHOD_PATCH}, {"DELETE", CIPHON_METHOD_DELETE},
  };

  for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
    if (name != nullptr && strcasecmp(name, methods[i].name) == 0) {
      return methods[i].method;
    }
  }

  return CIPHON_METHOD_GET;
}

static void fail_later(void *arg) {
  Job *job = arg;

  if (live_remove(job) && !atomic_load(&job->cancelled)) {
    HttpResponse res = {.error = "http is not running"};
    job->done(job->ctx, &res);
  }

  job_free(job);
}

HttpOp http_send(const HttpRequest *req, HttpDone done, void *ctx) {
  Job *job = calloc(1, sizeof *job);

  if (job == nullptr) {
    return 0;
  }

  double timeout = req->timeout > 0 ? req->timeout : 30;

  job->op = ++http.last_op;
  job->done = done;
  job->ctx = ctx;
  job->cookies = req->cookies;
  job->method = method_of(req->method);
  job->url = copy(req->url != nullptr ? req->url : "");
  job->timeout_ms = (uint32_t)(timeout * 1000);
  job->max_len =
    req->max_response_len != 0 ? req->max_response_len : HTTP_DEFAULT_MAX;
  job->headers = calloc(req->nheaders + 1, sizeof *job->headers);

  for (size_t i = 0; job->headers != nullptr && i < req->nheaders; i++) {
    if (req->headers[i].name == nullptr || req->headers[i].value == nullptr) {
      continue;
    }

    job->headers[job->nheaders++] = (ciphon_header){
      .name = copy(req->headers[i].name),
      .value = copy(req->headers[i].value),
    };
  }

  char *cookies = req->cookies ? cookie_header() : nullptr;

  if (cookies != nullptr && job->headers != nullptr) {
    job->headers[job->nheaders++] =
      (ciphon_header){.name = copy("Cookie"), .value = cookies};
  }
  else {
    free(cookies);
  }

  if (req->body != nullptr && req->body_len > 0) {
    job->body = malloc(req->body_len);

    if (job->body != nullptr) {
      memcpy(job->body, req->body, req->body_len);
      job->body_len = req->body_len;
    }
  }

  if (http.nlive == http.live_cap) {
    size_t cap = http.live_cap != 0 ? http.live_cap * 2 : 32;
    Job **grown = realloc(http.live, cap * sizeof *grown);

    if (grown == nullptr) {
      job_free(job);
      return 0;
    }

    http.live = grown;
    http.live_cap = cap;
  }

  http.live[http.nlive++] = job;

  // the callback never runs inside this call, even when nothing can be sent.
  if (!start() || CINC_CHAN_SEND(http.jobs, job) != CINC_OK) {
    platform_post(fail_later, job);
  }

  return job->op;
}

void http_cancel(HttpOp op) {
  for (size_t i = 0; i < http.nlive; i++) {
    if (http.live[i]->op == op) {
      // the job frees itself once its transfer noticed.
      atomic_store(&http.live[i]->cancelled, true);
      http.live[i] = http.live[--http.nlive];
      return;
    }
  }
}

const char *http_header(const HttpResponse *res, const char *name) {
  for (size_t i = 0; res != nullptr && i < res->nheaders; i++) {
    if (strcasecmp(res->headers[i].name, name) == 0) {
      return res->headers[i].value;
    }
  }

  return nullptr;
}

void http_shutdown(void) {
  if (http.started && !http.stopped) {
    CINC_CHAN_CLOSE(http.jobs);
    pthread_join(http.thread, nullptr);
    CINC_CHAN_FREE(http.jobs);
  }

  http.stopped = true;

  for (size_t i = 0; i < HTTP_CLIENTS; i++) {
    if (http.clients[i].send != nullptr) {
      ciphon_client_release(&http.clients[i]);
      http.clients[i] = (ciphon_client){};
    }
  }

  for (size_t i = 0; i < http.ncookies; i++) {
    free(http.cookies[i].name);
    free(http.cookies[i].value);
  }

  free(http.cookies);
  free(http.live);
  http.cookies = nullptr;
  http.live = nullptr;
  http.ncookies = http.nlive = http.live_cap = 0;
}
