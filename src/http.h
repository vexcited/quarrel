#ifndef QUARREL_HTTP_H
#define QUARREL_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* ========================================================================== */
/* http, for the api and for images. requests go through ciphon clients,      */
/* curl ones, on the blocking pool of a cinc runtime that has a thread of     */
/* its own. http_send is called from the main thread and its callback runs    */
/* on the main thread.                                                        */
/* ========================================================================== */

// a request still running. 0 is never one.
typedef uint64_t HttpOp;

typedef struct {
  const char *name, *value;
} HttpHeader;

typedef struct {
  // "GET" when null.
  const char *method;
  const char *url;
  const HttpHeader *headers;
  size_t nheaders;
  // sent as it is, the content type being one of `headers`.
  const void *body;
  size_t body_len;
  // a file streamed as the body instead of `body`, never held in memory.
  // http_sent tells how much of it went.
  const char *body_file;
  // sends the cookies earlier answers set and keeps the ones this one sets,
  // like a browser's session. for the api, not for images.
  bool cookies;
  // seconds before giving up, 30 when 0.
  double timeout;
  // the largest body accepted in bytes, 64 MiB when 0.
  size_t max_response_len;
} HttpRequest;

typedef struct {
  // 0 when no answer came, `error` then says why.
  int status;
  const char *error;
  // only valid during the callback. nul terminated past `body_len`.
  const char *body;
  size_t body_len;
  // every header in the order received.
  const HttpHeader *headers;
  size_t nheaders;
} HttpResponse;

typedef void (*HttpDone)(void *ctx, const HttpResponse *res);

// starts a request. `done` runs once unless the request is cancelled first.
// everything in `req` is copied.
HttpOp http_send(const HttpRequest *req, HttpDone done, void *ctx);
// the bytes of a request's `body_file` sent so far, 0 for an unknown one.
uint64_t http_sent(HttpOp op);
// forgets a request: its callback never runs and its transfer stops at the
// next bytes received. finished or unknown ones are fine.
void http_cancel(HttpOp op);

// the value of the first header called `name`, compared without case, or
// null.
const char *http_header(const HttpResponse *res, const char *name);

// stops the runtime once the requests in flight are done and frees what http
// holds, for a clean exit. nothing may be sent after.
void http_shutdown(void);

#endif
