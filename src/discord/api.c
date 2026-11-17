#include "discord/api.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include "http.h"
#include "platform/platform.h"

#define API_BASE "https://discord.com/api/v9"
#define INSTALLATION_KEY "installation_id"

// Quarrel says it is the desktop client of the system it runs on. these are
// read from Discord.app 0.0.422 for macOS: its Info.plist, the Electron
// framework it ships and the metadata_version of its installer.db, which
// DiscordNative.app.getBuildNumber answers with.
#define CLIENT_VERSION "0.0.422"
#define ELECTRON_VERSION "44.10.2"
#define CHROME_VERSION "152.0.7977.169"
#define CHROME_MAJOR "152"
constexpr uint64_t NATIVE_BUILD_NUMBER = 98625;

// chromium freezes the system's version at 10.15.7 whatever it really is.
#define USER_AGENT                                                 \
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "               \
  "AppleWebKit/537.36 (KHTML, like Gecko) discord/" CLIENT_VERSION \
  " Chrome/" CHROME_VERSION " Electron/" ELECTRON_VERSION " Safari/537.36"

// chromium's greased brand list, seeded by its major version: the filler
// characters are picked at major % 11 and major + 1 % 11 out of
// " (:-./);=?_", the version at major % 3 out of 8, 99, 24, and the order
// at major % 2 since electron lists no third brand.
#define SEC_CH_UA \
  "\"Not?A_Brand\";v=\"24\", \"Chromium\";v=\"" CHROME_MAJOR "\""

// the analytics heartbeat session lasts this long, then gets a new id.
constexpr double HEARTBEAT_SESSION_SECONDS = 30 * 60;

// how often a request answered 429 is sent again before giving up.
constexpr int API_MAX_ATTEMPTS = 4;
constexpr size_t API_MAX_FIELD_ERRORS = 16;
constexpr size_t API_PATH_MAX = 96;

typedef struct Request Request;
typedef struct Bucket Bucket;

struct Request {
  Request *next;
  Bucket *bucket;
  char *method, *url, *body;
  char *captcha_key, *captcha_session, *captcha_rqtoken;
  ApiDone done;
  void *ctx;
  // freed with the request: what a typed callback needs to answer.
  void *owned;
  int attempts;
};

// the requests of one route, sent one at a time in the order they came.
struct Bucket {
  Bucket *next;
  Api *api;
  char *route;
  Request *head, *tail;
  Request *active;
  HttpOp op;
  PlatformTimer *timer;
  // unix seconds the route may be used again at.
  double blocked_until;
};

struct Api {
  char *token, *fingerprint;
  char *super_properties;
  // the heartbeat session the properties above were written with.
  double properties_stamp;
  char locale[32], timezone[64], languages[80];
  Bucket *buckets;
  // unix seconds a global rate limit ends at.
  double global_until;
};

/* ========================================================================== */
/* client properties.                                                         */
/* ========================================================================== */

static struct {
  bool made;
  char launch_id[37], heartbeat_session_id[37], launch_signature[37];
  double heartbeat_since;
  char *installation_id;
} identity;

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }

  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
}

// a launch signature is a random uuid whose bits naming client mods are
// cleared, since there are none here. the bits are the ones the docs list.
static void make_launch_signature(char out[static 37]) {
  static const uint8_t mod_bits[] = {
    119, 108, 100, 91, 84, 75, 61, 55, 48, 38, 24, 11,
  };
  static const char hex[] = "0123456789abcdef";

  char uuid[37];
  uint8_t bytes[16];
  size_t n = 0;

  platform_uuid(uuid);

  for (size_t i = 0; uuid[i] != '\0' && uuid[i + 1] != '\0' && n < 16;) {
    if (uuid[i] == '-') {
      i++;
      continue;
    }

    bytes[n++] = (uint8_t)(hex_value(uuid[i]) << 4 | hex_value(uuid[i + 1]));
    i += 2;
  }

  // bit 0 is the last bit of the last byte.
  for (size_t i = 0; i < sizeof mod_bits; i++) {
    bytes[15 - mod_bits[i] / 8] &= (uint8_t)~(1u << (mod_bits[i] % 8));
  }

  size_t at = 0;

  for (size_t i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }

    out[at++] = hex[bytes[i] >> 4];
    out[at++] = hex[bytes[i] & 15];
  }

  out[at] = '\0';
}

static void make_identity(void) {
  double now = platform_now();

  if (identity.made) {
    if (now - identity.heartbeat_since >= HEARTBEAT_SESSION_SECONDS) {
      platform_uuid(identity.heartbeat_session_id);
      identity.heartbeat_since = now;
    }

    return;
  }

  identity.made = true;
  identity.heartbeat_since = now;
  identity.installation_id = platform_pref_get(INSTALLATION_KEY);
  platform_uuid(identity.launch_id);
  platform_uuid(identity.heartbeat_session_id);
  make_launch_signature(identity.launch_signature);
}

const char *api_installation_id(void) {
  make_identity();
  return identity.installation_id;
}

void api_set_installation_id(const char *id) {
  make_identity();

  if (id == nullptr || id[0] == '\0' ||
      (identity.installation_id != nullptr &&
       strcmp(identity.installation_id, id) == 0)) {
    return;
  }

  str_set(&identity.installation_id, id);
  platform_pref_set(INSTALLATION_KEY, id);
}

const char *api_user_agent(void) {
  return USER_AGENT;
}

// the fields the desktop client sends, in the order its bundle builds them:
// the native ones first, the build numbers next, then what is added once
// the app runs. the docs show os_version as the kernel's on macOS, the
// client now sends what process.getSystemVersion says with
// os_version_is_release, and keeps the kernel's major in os_sdk_version.
void api_write_client_properties(JsonWriter *w, bool gateway) {
  char locale[32], os_version[32], sdk[16];
  struct utsname sys = {};

  make_identity();
  platform_locale(locale, sizeof locale);
  platform_os_version(os_version, sizeof os_version);
  uname(&sys);
  snprintf(sdk, sizeof sdk, "%.*s", (int)strcspn(sys.release, "."),
           sys.release);

  // node names the architectures its own way.
  const char *arch = strcmp(sys.machine, "x86_64") == 0 ? "x64" : sys.machine;

  jw_object(w);
  jw_key(w, "os"), jw_str(w, "Mac OS X");
  jw_key(w, "browser"), jw_str(w, "Discord Client");
  jw_key(w, "release_channel"), jw_str(w, "stable");
  jw_key(w, "client_version"), jw_str(w, CLIENT_VERSION);
  jw_key(w, "os_version"), jw_str(w, os_version);
  jw_key(w, "os_version_is_release"), jw_bool(w, true);
  jw_key(w, "os_sdk_version"), jw_str(w, sdk);
  jw_key(w, "os_arch"), jw_str(w, arch);
  jw_key(w, "app_arch"), jw_str(w, arch);
  jw_key(w, "system_locale"), jw_str(w, locale);
  jw_key(w, "has_client_mods"), jw_bool(w, false);
  jw_key(w, "client_launch_id"), jw_str(w, identity.launch_id);
  jw_key(w, "browser_user_agent"), jw_str(w, USER_AGENT);
  jw_key(w, "browser_version"), jw_str(w, ELECTRON_VERSION);
  jw_key(w, "client_build_number"), jw_u64(w, DISCORD_BUILD_NUMBER);
  jw_key(w, "native_build_number"), jw_u64(w, NATIVE_BUILD_NUMBER);
  jw_key(w, "client_event_source"), jw_null(w);
  jw_key(w, "launch_signature"), jw_str(w, identity.launch_signature);
  jw_key(w, "client_heartbeat_session_id");
  jw_str(w, identity.heartbeat_session_id);
  jw_key(w, "client_app_state"), jw_str(w, "focused");

  if (gateway) {
    jw_key(w, "is_fast_connect"), jw_bool(w, false);
    jw_key(w, "gateway_connect_reasons"), jw_str(w, "AppSkeleton");

    if (identity.installation_id != nullptr) {
      jw_key(w, "installation_id"), jw_str(w, identity.installation_id);
    }
  }

  jw_end(w);
}

static void refresh_properties(Api *api) {
  make_identity();

  if (api->super_properties != nullptr &&
      api->properties_stamp == identity.heartbeat_since) {
    return;
  }

  JsonWriter w = {};
  api_write_client_properties(&w, false);
  free(api->super_properties);
  api->super_properties = base64_encode(w.data, w.len);
  api->properties_stamp = identity.heartbeat_since;
  jw_free(&w);
}

/* ========================================================================== */
/* requests and their buckets.                                                */
/* ========================================================================== */

static void pump(Bucket *b);

static void request_free(Request *r) {
  free(r->method);
  free(r->url);
  free(r->body);
  free(r->captcha_key);
  free(r->captcha_session);
  free(r->captcha_rqtoken);
  free(r->owned);
  free(r);
}

static Bucket *bucket_for(Api *api, const char *route) {
  for (Bucket *b = api->buckets; b != nullptr; b = b->next) {
    if (strcmp(b->route, route) == 0) {
      return b;
    }
  }

  Bucket *b = calloc(1, sizeof *b);

  if (b == nullptr) {
    cilo_oom(sizeof *b);
  }

  b->api = api;
  b->route = str_dup(route);
  b->next = api->buckets;
  api->buckets = b;
  return b;
}

// a bucket with nothing left to do or to wait for is forgotten, so visiting
// many channels does not pile them up.
static void bucket_release(Bucket *b) {
  bool idle = b->head == nullptr && b->active == nullptr &&
              b->timer == nullptr && b->blocked_until <= platform_now();

  if (!idle) {
    return;
  }

  for (Bucket **at = &b->api->buckets; *at != nullptr; at = &(*at)->next) {
    if (*at == b) {
      *at = b->next;
      break;
    }
  }

  free(b->route);
  free(b);
}

static void on_timer(void *ctx) {
  Bucket *b = ctx;
  b->timer = nullptr;
  pump(b);
  bucket_release(b);
}

// the field errors of a form, each with the path of keys leading to it.
static void collect_errors(const Json *node, char *path, size_t path_len,
                           size_t cap, ApiFieldError *fields,
                           char (*paths)[API_PATH_MAX], size_t *count) {
  if (!json_is(node, JSON_OBJECT)) {
    return;
  }

  json_each(error, json_get(node, "_errors")) {
    if (*count >= API_MAX_FIELD_ERRORS) {
      return;
    }

    snprintf(paths[*count], API_PATH_MAX, "%.*s", (int)path_len, path);

    fields[*count] = (ApiFieldError){
      .path = paths[*count],
      .code = json_get_str(error, "code"),
      .message = json_get_str(error, "message"),
    };

    (*count)++;
  }

  json_each(child, node) {
    if (strcmp(child->key, "_errors") == 0) {
      continue;
    }

    int n = snprintf(path + path_len, cap - path_len, "%s%s",
                     path_len != 0 ? "." : "", child->key);

    if (n < 0 || (size_t)n >= cap - path_len) {
      continue;
    }

    collect_errors(child, path, path_len + (size_t)n, cap, fields, paths,
                   count);
  }
}

typedef struct {
  ApiCaptcha captcha;
  const char *keys[8];
} CaptchaView;

static bool read_captcha(const Json *json, int status, CaptchaView *view) {
  const Json *keys = json_get(json, "captcha_key");

  if (status != 400 || keys == nullptr) {
    return false;
  }

  *view = (CaptchaView){};

  json_each(key, keys) {
    if (view->captcha.nkeys < 8 && json_is(key, JSON_STRING)) {
      view->keys[view->captcha.nkeys++] = key->string;
    }
  }

  view->captcha.keys = view->keys;
  view->captcha.service =
    json_str(json_get(json, "captcha_service"), "hcaptcha");
  view->captcha.sitekey = json_get_str(json, "captcha_sitekey");
  view->captcha.session_id = json_get_str(json, "captcha_session_id");
  view->captcha.rqdata = json_get_str(json, "captcha_rqdata");
  view->captcha.rqtoken = json_get_str(json, "captcha_rqtoken");
  view->captcha.serve_invisible =
    json_get_bool(json, "should_serve_invisible", false);
  return true;
}

static void on_response(void *ctx, const HttpResponse *http) {
  Request *r = ctx;
  Bucket *b = r->bucket;
  Api *api = b->api;
  double now = platform_now();

  cilo_arena arena = {};
  const Json *json = nullptr;

  if (http->body_len > 0) {
    json = json_parse(&arena, http->body, http->body_len, nullptr);
  }

  b->active = nullptr;
  b->op = 0;

  const char *remaining = http_header(http, "x-ratelimit-remaining");
  const char *reset_after = http_header(http, "x-ratelimit-reset-after");
  const char *retry_header = http_header(http, "retry-after");
  const char *scope = http_header(http, "x-ratelimit-scope");

  if (http->status == 429) {
    double retry = json_get_f64(json, "retry_after", -1);

    if (retry < 0 && retry_header != nullptr) {
      retry = strtod(retry_header, nullptr);
    }

    if (retry < 0) {
      retry = 1;
    }

    bool global = json_get_bool(json, "global", false) ||
                  http_header(http, "x-ratelimit-global") != nullptr ||
                  (scope != nullptr && strcmp(scope, "global") == 0);

    if (global) {
      api->global_until = now + retry;
    }

    b->blocked_until = now + retry;

    // a short wait is taken here, a long one is the caller's to show.
    if (++r->attempts < API_MAX_ATTEMPTS && retry <= 30) {
      r->next = b->head;
      b->head = r;

      if (b->tail == nullptr) {
        b->tail = r;
      }

      cilo_arena_drop(&arena);
      pump(b);
      return;
    }
  }
  else if (remaining != nullptr && reset_after != nullptr &&
           atoi(remaining) == 0) {
    // the route is spent: the next request waits rather than earning a 429.
    b->blocked_until = now + strtod(reset_after, nullptr);
  }

  ApiFieldError fields[API_MAX_FIELD_ERRORS];
  char paths[API_MAX_FIELD_ERRORS][API_PATH_MAX];
  size_t nfields = 0;
  CaptchaView captcha;
  char path[API_PATH_MAX] = "";
  char message[256];

  ApiResponse res = {
    .ok = http->status >= 200 && http->status < 300,
    .status = http->status,
    .json = json,
  };

  if (!res.ok) {
    collect_errors(json_get(json, "errors"), path, 0, sizeof path, fields,
                   paths, &nfields);

    res.error.status = http->status;
    res.error.code = (uint32_t)json_get_u64(json, "code");
    res.error.fields = fields;
    res.error.nfields = nfields;

    const char *text = nfields > 0 ? fields[0].message : nullptr;

    if (text == nullptr) {
      text = json_get_str(json, "message");
    }

    if (text == nullptr && http->status == 0) {
      text = http->error;
    }

    if (text == nullptr) {
      snprintf(message, sizeof message, "Discord answered %d", http->status);
      text = message;
    }

    res.error.message = text;

    if (read_captcha(json, http->status, &captcha)) {
      res.captcha = &captcha.captcha;
    }
  }

  // the bucket moves on first: the callback may free the api.
  ApiDone done = r->done;
  void *done_ctx = r->ctx;

  pump(b);
  bucket_release(b);

  if (done != nullptr) {
    done(done_ctx, &res);
  }

  request_free(r);
  cilo_arena_drop(&arena);
}

static void send_now(Bucket *b, Request *r) {
  Api *api = b->api;
  HttpHeader headers[28];
  size_t n = 0;

  refresh_properties(api);

  // what electron's fetch sends for a same origin request, then what the
  // client's own request hook adds, in its order.
  headers[n++] = (HttpHeader){"User-Agent", USER_AGENT};
  headers[n++] = (HttpHeader){"Accept", "*/*"};
  headers[n++] = (HttpHeader){"Accept-Language", api->languages};
  headers[n++] = (HttpHeader){"Origin", "https://discord.com"};
  headers[n++] = (HttpHeader){"Referer", "https://discord.com/channels/@me"};
  headers[n++] = (HttpHeader){"Sec-CH-UA", SEC_CH_UA};
  headers[n++] = (HttpHeader){"Sec-CH-UA-Mobile", "?0"};
  headers[n++] = (HttpHeader){"Sec-CH-UA-Platform", "\"macOS\""};
  headers[n++] = (HttpHeader){"Sec-Fetch-Dest", "empty"};
  headers[n++] = (HttpHeader){"Sec-Fetch-Mode", "cors"};
  headers[n++] = (HttpHeader){"Sec-Fetch-Site", "same-origin"};
  headers[n++] = (HttpHeader){"Priority", "u=1, i"};

  if (r->body != nullptr) {
    headers[n++] = (HttpHeader){"Content-Type", "application/json"};
  }

  if (api->token != nullptr) {
    headers[n++] = (HttpHeader){"Authorization", api->token};
  }

  headers[n++] = (HttpHeader){"X-Super-Properties", api->super_properties};

  // the fingerprint outlives the login, the client sends it while it has it.
  if (api->fingerprint != nullptr) {
    headers[n++] = (HttpHeader){"X-Fingerprint", api->fingerprint};
  }

  if (identity.installation_id != nullptr) {
    headers[n++] = (HttpHeader){"X-Installation-ID", identity.installation_id};
  }

  headers[n++] = (HttpHeader){"X-Discord-Locale", api->locale};
  headers[n++] = (HttpHeader){"X-Discord-Timezone", api->timezone};
  headers[n++] = (HttpHeader){"X-Debug-Options", "bugReporterEnabled"};

  if (r->captcha_key != nullptr) {
    headers[n++] = (HttpHeader){"X-Captcha-Key", r->captcha_key};
  }

  if (r->captcha_session != nullptr) {
    headers[n++] = (HttpHeader){"X-Captcha-Session-Id", r->captcha_session};
  }

  if (r->captcha_rqtoken != nullptr) {
    headers[n++] = (HttpHeader){"X-Captcha-Rqtoken", r->captcha_rqtoken};
  }

  HttpRequest req = {
    .method = r->method,
    .url = r->url,
    .headers = headers,
    .nheaders = n,
    .body = r->body,
    .body_len = r->body != nullptr ? strlen(r->body) : 0,
    .cookies = true,
  };

  b->active = r;
  b->op = http_send(&req, on_response, r);
}

static void pump(Bucket *b) {
  if (b->active != nullptr || b->timer != nullptr || b->head == nullptr) {
    return;
  }

  double now = platform_now();
  double until = b->blocked_until > b->api->global_until ? b->blocked_until
                                                         : b->api->global_until;

  if (until > now) {
    b->timer = platform_timer(until - now + 0.05, on_timer, b);
    return;
  }

  Request *r = b->head;
  b->head = r->next;
  r->next = nullptr;

  if (b->head == nullptr) {
    b->tail = nullptr;
  }

  send_now(b, r);
}

static Request *enqueue(Api *api, const ApiRequest *req, ApiDone done,
                        void *ctx, void *owned) {
  Request *r = calloc(1, sizeof *r);

  if (r == nullptr) {
    cilo_oom(sizeof *r);
  }

  size_t url_len = strlen(API_BASE) + strlen(req->path) + 1;

  r->method = str_dup(req->method != nullptr ? req->method : "GET");
  r->url = malloc(url_len);
  r->body = str_dup(req->body);
  r->done = done;
  r->ctx = ctx;
  r->owned = owned;

  if (r->url == nullptr) {
    cilo_oom(url_len);
  }

  snprintf(r->url, url_len, "%s%s", API_BASE, req->path);

  if (req->captcha != nullptr) {
    r->captcha_key = str_dup(req->captcha->key);
    r->captcha_session = str_dup(req->captcha->session_id);
    r->captcha_rqtoken = str_dup(req->captcha->rqtoken);
  }

  // the method is part of what is limited.
  char route[256];
  snprintf(route, sizeof route, "%s %s", r->method,
           req->route != nullptr ? req->route : req->path);

  Bucket *b = bucket_for(api, route);
  r->bucket = b;

  if (b->tail != nullptr) {
    b->tail->next = r;
  }
  else {
    b->head = r;
  }

  b->tail = r;
  pump(b);
  return r;
}

void api_request(Api *api, const ApiRequest *req, ApiDone done, void *ctx) {
  enqueue(api, req, done, ctx, nullptr);
}

Api *api_new(void) {
  Api *api = calloc(1, sizeof *api);

  if (api == nullptr) {
    cilo_oom(sizeof *api);
  }

  platform_locale(api->locale, sizeof api->locale);
  platform_timezone(api->timezone, sizeof api->timezone);

  // chromium's accept-language: the locale, then its language alone.
  size_t lang = strcspn(api->locale, "-");

  if (api->locale[lang] == '-') {
    snprintf(api->languages, sizeof api->languages, "%s,%.*s;q=0.9",
             api->locale, (int)lang, api->locale);
  }
  else {
    snprintf(api->languages, sizeof api->languages, "%s", api->locale);
  }

  refresh_properties(api);

  return api;
}

void api_free(Api *api) {
  if (api == nullptr) {
    return;
  }

  Bucket *b = api->buckets;

  while (b != nullptr) {
    Bucket *next = b->next;

    http_cancel(b->op);
    platform_timer_cancel(b->timer);

    if (b->active != nullptr) {
      request_free(b->active);
    }

    for (Request *r = b->head; r != nullptr;) {
      Request *after = r->next;
      request_free(r);
      r = after;
    }

    free(b->route);
    free(b);
    b = next;
  }

  free(api->token);
  free(api->fingerprint);
  free(api->super_properties);
  free(api);
}

void api_set_token(Api *api, const char *token) {
  str_set(&api->token, token);
}

const char *api_token(const Api *api) {
  return api->token;
}

/* ========================================================================== */
/* authentication.                                                            */
/* ========================================================================== */

typedef struct {
  Api *api;
  ApiDone done;
  void *ctx;
} FingerprintCall;

static void on_fingerprint(void *ctx, const ApiResponse *res) {
  FingerprintCall *call = ctx;
  const char *fingerprint = json_get_str(res->json, "fingerprint");

  if (res->ok && fingerprint != nullptr) {
    str_set(&call->api->fingerprint, fingerprint);
  }

  if (call->done != nullptr) {
    call->done(call->ctx, res);
  }
}

void api_fetch_fingerprint(Api *api, ApiDone done, void *ctx) {
  FingerprintCall *call = calloc(1, sizeof *call);

  if (call == nullptr) {
    cilo_oom(sizeof *call);
  }

  *call = (FingerprintCall){api, done, ctx};

  ApiRequest req = {.method = "GET", .path = "/experiments"};
  enqueue(api, &req, on_fingerprint, call, call);
}

typedef struct {
  ApiLoginDone done;
  void *ctx;
} LoginCall;

static void on_login(void *ctx, const ApiResponse *res) {
  LoginCall *call = ctx;
  const Json *json = res->json;
  ApiLoginResult result = {.kind = API_LOGIN_ERROR, .error = res->error};

  if (res->captcha != nullptr) {
    result.kind = API_LOGIN_CAPTCHA;
    result.captcha = *res->captcha;
  }
  else if (res->ok && json_get_str(json, "token") != nullptr) {
    result.kind = API_LOGIN_TOKEN;
    result.token = json_get_str(json, "token");
    result.user_id = json_get_u64(json, "user_id");
  }
  else if (res->ok && json_get_str(json, "ticket") != nullptr) {
    result.kind = API_LOGIN_MFA;
    result.user_id = json_get_u64(json, "user_id");
    result.mfa = (ApiMfa){
      .ticket = json_get_str(json, "ticket"),
      .login_instance_id = json_get_str(json, "login_instance_id"),
      .totp = json_get_bool(json, "totp", false),
      .sms = json_get_bool(json, "sms", false),
      .backup = json_get_bool(json, "backup", false),
      .webauthn = json_get_str(json, "webauthn") != nullptr,
    };
  }
  else if (res->ok) {
    // a success that carries neither a token nor a ticket.
    result.error = (ApiError){
      .status = res->status,
      .message = "Discord answered the login with nothing to go on",
    };
  }
  else if (json_get_str(json, "suspended_user_token") != nullptr) {
    result.error.message = "This account is suspended";
  }

  call->done(call->ctx, &result);
}

static void login_request(Api *api, const char *path, char *body,
                          const ApiCaptchaSolution *captcha, ApiLoginDone done,
                          void *ctx) {
  LoginCall *call = calloc(1, sizeof *call);

  if (call == nullptr) {
    cilo_oom(sizeof *call);
  }

  *call = (LoginCall){done, ctx};

  ApiRequest req = {
    .method = "POST",
    .path = path,
    .body = body,
    .captcha = captcha,
  };

  enqueue(api, &req, on_login, call, call);
  free(body);
}

void api_login(Api *api, const char *login, const char *password,
               const ApiCaptchaSolution *captcha, ApiLoginDone done,
               void *ctx) {
  JsonWriter w = {};

  jw_object(&w);
  jw_key(&w, "login"), jw_str(&w, login != nullptr ? login : "");
  jw_key(&w, "password"), jw_str(&w, password != nullptr ? password : "");
  jw_key(&w, "undelete"), jw_bool(&w, false);
  jw_key(&w, "login_source"), jw_null(&w);
  jw_key(&w, "gift_code_sku_id"), jw_null(&w);
  jw_end(&w);

  login_request(api, "/auth/login", jw_take(&w), captcha, done, ctx);
}

void api_mfa(Api *api, ApiMfaKind kind, const ApiMfa *mfa, const char *code,
             ApiLoginDone done, void *ctx) {
  static const char *const paths[] = {
    [API_MFA_TOTP] = "/auth/mfa/totp",
    [API_MFA_SMS] = "/auth/mfa/sms",
    [API_MFA_BACKUP] = "/auth/mfa/backup",
  };

  JsonWriter w = {};

  jw_object(&w);
  jw_key(&w, "ticket"), jw_str(&w, mfa->ticket != nullptr ? mfa->ticket : "");

  if (mfa->login_instance_id != nullptr) {
    jw_key(&w, "login_instance_id"), jw_str(&w, mfa->login_instance_id);
  }

  jw_key(&w, "code"), jw_str(&w, code != nullptr ? code : "");
  jw_key(&w, "login_source"), jw_null(&w);
  jw_key(&w, "gift_code_sku_id"), jw_null(&w);
  jw_end(&w);

  const char *path = kind <= API_MFA_BACKUP ? paths[kind] : paths[0];
  login_request(api, path, jw_take(&w), nullptr, done, ctx);
}

void api_mfa_send_sms(Api *api, const char *ticket, ApiDone done, void *ctx) {
  JsonWriter w = {};

  jw_object(&w);
  jw_key(&w, "ticket"), jw_str(&w, ticket != nullptr ? ticket : "");
  jw_end(&w);

  ApiRequest req = {
    .method = "POST",
    .path = "/auth/mfa/sms/send",
    .body = w.data,
  };

  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_logout(Api *api, ApiDone done, void *ctx) {
  ApiRequest req = {
    .method = "POST",
    .path = "/auth/logout",
    .body = "{\"provider\":null,\"voip_provider\":null}",
  };

  api_request(api, &req, done, ctx);
}

/* ========================================================================== */
/* messages.                                                                  */
/* ========================================================================== */

void api_get_messages(Api *api, Snowflake channel_id, uint32_t limit,
                      Snowflake before, Snowflake after, Snowflake around,
                      ApiDone done, void *ctx) {
  char path[192], route[96];

  if (limit == 0 || limit > 100) {
    limit = 50;
  }

  int n = snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages?limit=%u",
                   channel_id, limit);
  size_t len = (size_t)n;

  if (before != 0) {
    snprintf(path + len, sizeof path - len, "&before=%" PRIu64, before);
  }
  else if (after != 0) {
    snprintf(path + len, sizeof path - len, "&after=%" PRIu64, after);
  }
  else if (around != 0) {
    snprintf(path + len, sizeof path - len, "&around=%" PRIu64, around);
  }

  snprintf(route, sizeof route, "/channels/%" PRIu64 "/messages", channel_id);

  ApiRequest req = {.method = "GET", .path = path, .route = route};
  api_request(api, &req, done, ctx);
}

void api_send_message(Api *api, Snowflake channel_id,
                      const ApiSendMessage *message, ApiDone done, void *ctx) {
  char path[96];
  JsonWriter w = {};

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages", channel_id);

  jw_object(&w);
  jw_key(&w, "mobile_network_type"), jw_str(&w, "unknown");
  jw_key(&w, "content");
  jw_str(&w, message->content != nullptr ? message->content : "");

  if (message->nonce != 0) {
    jw_key(&w, "nonce"), jw_id(&w, message->nonce);
  }

  jw_key(&w, "tts"), jw_bool(&w, false);
  jw_key(&w, "flags"), jw_u64(&w, 0);

  if (message->forward_id != 0) {
    // type 1 is a forward, which carries a snapshot and no text of its own.
    jw_key(&w, "message_reference");
    jw_object(&w);
    jw_key(&w, "type"), jw_u64(&w, 1);
    jw_key(&w, "channel_id"), jw_id(&w, message->forward_channel_id);
    jw_key(&w, "message_id"), jw_id(&w, message->forward_id);

    if (message->forward_guild_id != 0) {
      jw_key(&w, "guild_id"), jw_id(&w, message->forward_guild_id);
    }

    jw_end(&w);
  }
  else if (message->reply_to != 0) {
    jw_key(&w, "message_reference");
    jw_object(&w);
    jw_key(&w, "channel_id"), jw_id(&w, channel_id);
    jw_key(&w, "message_id"), jw_id(&w, message->reply_to);
    jw_end(&w);

    // the web client lists what may ping and says whether the answered
    // author is one of them.
    jw_key(&w, "allowed_mentions");
    jw_object(&w);
    jw_key(&w, "parse");
    jw_array(&w);
    jw_str(&w, "users");
    jw_str(&w, "roles");
    jw_str(&w, "everyone");
    jw_end(&w);
    jw_key(&w, "replied_user"), jw_bool(&w, message->reply_ping);
    jw_end(&w);
  }

  jw_end(&w);

  ApiRequest req = {.method = "POST", .path = path, .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

// messages of a channel are limited together whichever one is touched.
static void message_route(char *route, size_t cap, Snowflake channel_id,
                          const char *what) {
  snprintf(route, cap, "/channels/%" PRIu64 "/messages/:id%s", channel_id,
           what);
}

void api_edit_message(Api *api, Snowflake channel_id, Snowflake message_id,
                      const char *content, ApiDone done, void *ctx) {
  char path[128], route[128];
  JsonWriter w = {};

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages/%" PRIu64,
           channel_id, message_id);
  message_route(route, sizeof route, channel_id, "");

  jw_object(&w);
  jw_key(&w, "content"), jw_str(&w, content != nullptr ? content : "");
  jw_end(&w);

  ApiRequest req = {
    .method = "PATCH", .path = path, .route = route, .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_delete_message(Api *api, Snowflake channel_id, Snowflake message_id,
                        ApiDone done, void *ctx) {
  char path[128], route[128];

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages/%" PRIu64,
           channel_id, message_id);
  message_route(route, sizeof route, channel_id, "");

  ApiRequest req = {.method = "DELETE", .path = path, .route = route};
  api_request(api, &req, done, ctx);
}

void api_ack_message(Api *api, Snowflake channel_id, Snowflake message_id,
                     uint32_t flags, uint32_t last_viewed, ApiDone done,
                     void *ctx) {
  char path[128], route[128];
  JsonWriter w = {};

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages/%" PRIu64 "/ack",
           channel_id, message_id);
  message_route(route, sizeof route, channel_id, "/ack");

  // ack tokens are gone from discord's side, null is what is expected.
  jw_object(&w);
  jw_key(&w, "token"), jw_null(&w);
  jw_key(&w, "flags"), jw_u64(&w, flags);
  jw_key(&w, "last_viewed"), jw_u64(&w, last_viewed);
  jw_end(&w);

  ApiRequest req = {
    .method = "POST", .path = path, .route = route, .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_trigger_typing(Api *api, Snowflake channel_id, ApiDone done,
                        void *ctx) {
  char path[96];
  snprintf(path, sizeof path, "/channels/%" PRIu64 "/typing", channel_id);

  ApiRequest req = {.method = "POST", .path = path};
  api_request(api, &req, done, ctx);
}

void api_ack_manual(Api *api, Snowflake channel_id, Snowflake message_id,
                    uint32_t mentions, ApiDone done, void *ctx) {
  char path[128], route[128];
  JsonWriter w = {};

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages/%" PRIu64 "/ack",
           channel_id, message_id);
  message_route(route, sizeof route, channel_id, "/ack");

  jw_object(&w);
  jw_key(&w, "manual"), jw_bool(&w, true);
  jw_key(&w, "mention_count"), jw_u64(&w, mentions);
  jw_end(&w);

  ApiRequest req = {
    .method = "POST", .path = path, .route = route, .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_pin_message(Api *api, Snowflake channel_id, Snowflake message_id,
                     bool pin, ApiDone done, void *ctx) {
  char path[128], route[128];

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages/pins/%" PRIu64,
           channel_id, message_id);
  message_route(route, sizeof route, channel_id, "/pins");

  ApiRequest req = {
    .method = pin ? "PUT" : "DELETE", .path = path, .route = route};
  api_request(api, &req, done, ctx);
}

static void reaction_request(Api *api, const char *method, Snowflake channel_id,
                             Snowflake message_id, Snowflake emoji_id,
                             const char *name, const char *query, ApiDone done,
                             void *ctx) {
  char emoji[160], path[384], route[128];
  char *encoded = url_encode(name != nullptr ? name : "");

  // a custom emoji goes by name:id, a unicode one by itself.
  if (emoji_id != 0) {
    snprintf(emoji, sizeof emoji, "%s%%3A%" PRIu64, encoded, emoji_id);
  }
  else {
    snprintf(emoji, sizeof emoji, "%s", encoded);
  }

  free(encoded);

  snprintf(path, sizeof path,
           "/channels/%" PRIu64 "/messages/%" PRIu64 "/reactions/%s/%s",
           channel_id, message_id, emoji, query);
  message_route(route, sizeof route, channel_id, "/reactions");

  ApiRequest req = {.method = method, .path = path, .route = route};
  api_request(api, &req, done, ctx);
}

void api_add_reaction(Api *api, Snowflake channel_id, Snowflake message_id,
                      Snowflake emoji_id, const char *name, ApiDone done,
                      void *ctx) {
  reaction_request(api, "PUT", channel_id, message_id, emoji_id, name,
                   "@me?location=Message%20Reaction%20Picker&type=0", done,
                   ctx);
}

void api_remove_reaction(Api *api, Snowflake channel_id, Snowflake message_id,
                         Snowflake emoji_id, const char *name, ApiDone done,
                         void *ctx) {
  reaction_request(api, "DELETE", channel_id, message_id, emoji_id, name,
                   "0/@me?location=Message%20Inline%20Button&burst=false", done,
                   ctx);
}

void api_open_dm(Api *api, Snowflake user_id, ApiDone done, void *ctx) {
  JsonWriter w = {};

  jw_object(&w);
  jw_key(&w, "recipients");
  jw_array(&w);
  jw_id(&w, user_id);
  jw_end(&w);
  jw_end(&w);

  ApiRequest req = {
    .method = "POST", .path = "/users/@me/channels", .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_open_group(Api *api, const Snowflake *user_ids, size_t count,
                    ApiDone done, void *ctx) {
  JsonWriter w = {};

  jw_object(&w);
  jw_key(&w, "recipients");
  jw_array(&w);

  for (size_t i = 0; i < count; i++) {
    jw_id(&w, user_ids[i]);
  }

  jw_end(&w);
  jw_end(&w);

  ApiRequest req = {
    .method = "POST", .path = "/users/@me/channels", .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_create_invite(Api *api, Snowflake channel_id, ApiDone done,
                       void *ctx) {
  char path[96];
  JsonWriter w = {};

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/invites", channel_id);

  // what the client sends for the link of its invite dialog.
  jw_object(&w);
  jw_key(&w, "max_age"), jw_u64(&w, 604800);
  jw_key(&w, "max_uses"), jw_u64(&w, 0);
  jw_key(&w, "target_type"), jw_null(&w);
  jw_key(&w, "temporary"), jw_bool(&w, false);
  jw_key(&w, "flags"), jw_u64(&w, 0);
  jw_end(&w);

  ApiRequest req = {.method = "POST", .path = path, .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}

void api_create_guild(Api *api, const char *name, ApiDone done, void *ctx) {
  JsonWriter w = {};

  // what the client sends for "Create My Own", the code being the template
  // its new servers start from.
  jw_object(&w);
  jw_key(&w, "name"), jw_str(&w, name);
  jw_key(&w, "icon"), jw_null(&w);
  jw_key(&w, "channels"), jw_array(&w), jw_end(&w);
  jw_key(&w, "system_channel_id"), jw_null(&w);
  jw_key(&w, "guild_template_code"), jw_str(&w, "2TffvPucqHkN");
  jw_end(&w);

  ApiRequest req = {.method = "POST", .path = "/guilds", .body = w.data};
  api_request(api, &req, done, ctx);
  jw_free(&w);
}
