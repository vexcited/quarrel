#include "discord/uploads.h"

#include <fcntl.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "discord/api.h"
#include "platform/platform.h"

// what the web client allows everyone, and what nitro raises it to.
constexpr uint64_t MAX_SIZE_FLOOR = 20ull << 20;
constexpr uint64_t MAX_SIZE_FREE = 10ull << 20;
constexpr uint64_t MAX_SIZE_BASIC = 50ull << 20;
constexpr uint64_t MAX_SIZE_NITRO = 500ull << 20;
// an upload url older than this is asked again before it is used.
constexpr double URL_LIFETIME = 12 * 3600;
constexpr uint8_t MAX_ATTEMPTS = 12;
// a file of 500 MiB on a slow line.
constexpr double PUT_TIMEOUT = 4 * 3600;

typedef struct {
  Session *s;
  uint32_t id;
} UploadCall;

typedef struct {
  Session *s;
  Snowflake channel_id, nonce;
} MessageCall;

static void changed(Session *s) {
  s->version++;

  if (s->options.on_change != nullptr) {
    s->options.on_change();
  }
}

static bool online(const Session *s) {
  return !s->offline && s->api != nullptr && s->token != nullptr;
}

static UploadCall *upload_call(Session *s, uint32_t id) {
  UploadCall *call = calloc(1, sizeof *call);

  if (call == nullptr) {
    cilo_oom(sizeof *call);
  }

  *call = (UploadCall){.s = s, .id = id};
  return call;
}

Upload *uploads_get(Session *s, uint32_t id) {
  for (size_t i = 0; id != 0 && i < s->nuploads; i++) {
    if (s->uploads[i]->id == id) {
      return s->uploads[i];
    }
  }

  return nullptr;
}

size_t uploads_of(Session *s, Snowflake channel_id, Snowflake nonce,
                  Upload **out, size_t cap) {
  size_t n = 0;

  for (size_t i = 0; i < s->nuploads; i++) {
    Upload *u = s->uploads[i];

    if (u->channel_id == channel_id && u->nonce == nonce) {
      if (n < cap) {
        out[n] = u;
      }

      n++;
    }
  }

  return n;
}

bool uploads_owns(Session *s, Snowflake nonce) {
  for (size_t i = 0; nonce != 0 && i < s->nuploads; i++) {
    if (s->uploads[i]->nonce == nonce) {
      return true;
    }
  }

  return false;
}

uint64_t uploads_sent(const Upload *u) {
  if (u->state == UPLOAD_DONE) {
    return u->size;
  }

  uint64_t sent = u->state == UPLOAD_SENDING ? http_sent(u->op) : 0;

  return sent < u->size ? sent : u->size;
}

uint64_t uploads_max_size(Session *s, const Channel *c) {
  const User *self = s->store->self;
  const Guild *g = c != nullptr ? store_guild(s->store, c->guild_id) : nullptr;
  uint64_t user = MAX_SIZE_FREE;

  if (self != nullptr && self->premium_type == 2) {
    user = MAX_SIZE_NITRO;
  }
  else if (self != nullptr &&
           (self->premium_type == 1 || self->premium_type == 3)) {
    user = MAX_SIZE_BASIC;
  }

  uint64_t max =
    g != nullptr && g->upload_feature > user ? g->upload_feature : user;

  return max > MAX_SIZE_FLOOR ? max : MAX_SIZE_FLOOR;
}

/* ========================================================================== */
/* the media type of a file, from its name. the web client sends the one its  */
/* browser found, which goes by the name too.                                 */
/* ========================================================================== */

static const char *content_type_of(const char *filename) {
  static const struct {
    const char *ext, *type;
  } types[] = {
    {"png", "image/png"},         {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},       {"gif", "image/gif"},
    {"webp", "image/webp"},       {"avif", "image/avif"},
    {"heic", "image/heic"},       {"bmp", "image/bmp"},
    {"svg", "image/svg+xml"},     {"tif", "image/tiff"},
    {"tiff", "image/tiff"},       {"mp4", "video/mp4"},
    {"mov", "video/quicktime"},   {"webm", "video/webm"},
    {"mkv", "video/x-matroska"},  {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},         {"wav", "audio/wav"},
    {"flac", "audio/flac"},       {"m4a", "audio/mp4"},
    {"txt", "text/plain"},        {"md", "text/markdown"},
    {"json", "application/json"}, {"pdf", "application/pdf"},
    {"zip", "application/zip"},   {"html", "text/html"},
    {"css", "text/css"},          {"js", "text/javascript"},
    {"csv", "text/csv"},
  };

  const char *dot = strrchr(filename, '.');

  for (size_t i = 0; dot != nullptr && i < countof(types); i++) {
    if (strcasecmp(dot + 1, types[i].ext) == 0) {
      return types[i].type;
    }
  }

  return nullptr;
}

/* ========================================================================== */
/* putting a file in the bucket.                                              */
/* ========================================================================== */

static void fail_message(Session *s, const Upload *u) {
  if (u->nonce != 0) {
    store_fail_pending(s->store, u->channel_id, u->nonce, u->error);
  }
}

static void fail(Session *s, Upload *u, const char *error) {
  u->state = UPLOAD_FAILED;
  str_set(&u->error, error);
  fail_message(s, u);
  changed(s);
}

static void forget_bucket(Session *s, Upload *u) {
  if (u->uploaded_filename != nullptr && online(s)) {
    api_delete_attachment(s->api, u->uploaded_filename, nullptr, nullptr);
  }

  str_set(&u->upload_url, nullptr);
  str_set(&u->uploaded_filename, nullptr);
}

static void reserve(Session *s, Upload *u);
static void try_send(Session *s, Snowflake channel_id, Snowflake nonce);

static void on_put(void *ctx, const HttpResponse *res) {
  UploadCall *call = ctx;
  Session *s = call->s;
  Upload *u = uploads_get(s, call->id);

  free(call);

  if (u == nullptr) {
    return;
  }

  u->op = 0;
  u->op_ctx = nullptr;

  if (res->status == 200 || res->status == 201) {
    u->state = UPLOAD_DONE;
    changed(s);
    try_send(s, u->channel_id, u->nonce);
    return;
  }

  // the web client tries again what the network or the bucket failed, the
  // bucket being asked for a new place each time here.
  bool again = res->status == 0 || res->status == 429 || res->status >= 500;

  if (again && ++u->attempts < MAX_ATTEMPTS && online(s)) {
    str_set(&u->upload_url, nullptr);
    str_set(&u->uploaded_filename, nullptr);
    reserve(s, u);
    return;
  }

  char error[160];

  snprintf(error, sizeof error, "The upload of %s failed.", u->filename);
  fail(s, u, error);
}

static void put(Session *s, Upload *u) {
  HttpHeader headers[] = {
    {"Content-Type", "application/octet-stream"},
  };
  HttpRequest req = {
    .method = "PUT",
    .url = u->upload_url,
    .headers = headers,
    .nheaders = countof(headers),
    .body_file = u->path,
    .timeout = PUT_TIMEOUT,
  };

  u->state = UPLOAD_SENDING;
  u->op_ctx = upload_call(s, u->id);
  u->op = http_send(&req, on_put, u->op_ctx);

  if (u->op == 0) {
    fail(s, u, "The file could not be sent.");
  }
}

static void on_reserved(void *ctx, const ApiResponse *res) {
  UploadCall *call = ctx;
  Session *s = call->s;
  Upload *u = uploads_get(s, call->id);
  const cima_json *slot = json_index(json_get(res->json, "attachments"), 0);
  const char *url = json_get_or(slot, "upload_url", (const char *)nullptr);
  const char *name =
    json_get_or(slot, "upload_filename", (const char *)nullptr);

  free(call);

  // taken out of the composer while discord made room for it.
  if (u == nullptr) {
    if (res->ok && name != nullptr && online(s)) {
      api_delete_attachment(s->api, name, nullptr, nullptr);
    }

    return;
  }

  if (!res->ok || url == nullptr || name == nullptr) {
    fail(s, u,
         res->ok ? "Discord gave nowhere to upload to." : res->error.message);
    return;
  }

  str_set(&u->upload_url, url);
  str_set(&u->uploaded_filename, name);
  u->reserved_at = platform_now();
  put(s, u);
  changed(s);
}

static void reserve(Session *s, Upload *u) {
  // offline, the bucket is made up and takes everything at once.
  if (s->offline) {
    u->state = UPLOAD_DONE;
    u->reserved_at = platform_now();
    str_set(&u->uploaded_filename, u->filename);
    return;
  }

  if (!online(s)) {
    u->state = UPLOAD_WAITING;
    return;
  }

  ApiUploadFile file = {
    .filename = u->filename,
    .size = u->size,
    .content_type = u->content_type,
  };

  u->state = UPLOAD_RESERVING;
  str_set(&u->error, nullptr);
  api_create_attachments(s->api, u->channel_id, &file, 1, on_reserved,
                         upload_call(s, u->id));
}

/* ========================================================================== */
/* the composer's files.                                                      */
/* ========================================================================== */

static void upload_free(Upload *u) {
  // a cancelled request never answers, its context is freed here.
  if (u->op != 0) {
    http_cancel(u->op);
    free(u->op_ctx);
  }

  if (u->temporary) {
    unlink(u->path);
  }

  free(u->path);
  free(u->filename);
  free(u->description);
  free(u->content_type);
  free(u->error);
  free(u->upload_url);
  free(u->uploaded_filename);
  free(u);
}

static void take_out(Session *s, Upload *u) {
  for (size_t i = 0; i < s->nuploads; i++) {
    if (s->uploads[i] == u) {
      memmove(s->uploads + i, s->uploads + i + 1,
              (s->nuploads - i - 1) * sizeof *s->uploads);
      s->nuploads--;
      break;
    }
  }

  upload_free(u);
}

uint32_t uploads_add(Session *s, Snowflake channel_id, const char *path,
                     const char *filename, bool temporary) {
  struct stat st;

  if (path == nullptr || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (temporary && path != nullptr) {
      unlink(path);
    }

    return 0;
  }

  if (filename == nullptr) {
    const char *slash = strrchr(path, '/');
    filename = slash != nullptr ? slash + 1 : path;
  }

  const char *type = content_type_of(filename);
  Upload *u = calloc(1, sizeof *u);
  Upload **grown = realloc(s->uploads, (s->nuploads + 1) * sizeof *s->uploads);

  if (u == nullptr || grown == nullptr) {
    cilo_oom(sizeof *u);
  }

  *u = (Upload){
    .id = ++s->upload_serial,
    .channel_id = channel_id,
    .path = str_dup(path),
    .temporary = temporary,
    .filename = str_dup(filename),
    .content_type = type != nullptr ? str_dup(type) : nullptr,
    .size = (uint64_t)st.st_size,
  };

  s->uploads = grown;
  s->uploads[s->nuploads++] = u;

  // files without anything in them are refused by discord.
  if (u->size == 0) {
    u->state = UPLOAD_FAILED;
    str_set(&u->error, "The file is empty.");
  }
  else {
    reserve(s, u);
  }

  changed(s);
  return u->id;
}

uint32_t uploads_add_bytes(Session *s, Snowflake channel_id, const void *bytes,
                           size_t len, const char *filename) {
  char uuid[37], name[64], path[1024];

  platform_uuid(uuid);
  snprintf(name, sizeof name, "upload-%s", uuid);

  if (!platform_cache_path(name, path, sizeof path)) {
    return 0;
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o600);

  if (fd < 0) {
    return 0;
  }

  size_t at = 0;

  while (at < len) {
    ssize_t n = write(fd, (const char *)bytes + at, len - at);

    if (n <= 0) {
      break;
    }

    at += (size_t)n;
  }

  close(fd);

  if (at != len) {
    unlink(path);
    return 0;
  }

  return uploads_add(s, channel_id, path, filename, true);
}

void uploads_remove(Session *s, uint32_t id) {
  Upload *u = uploads_get(s, id);

  if (u == nullptr) {
    return;
  }

  forget_bucket(s, u);
  take_out(s, u);
  changed(s);
}

/* ========================================================================== */
/* the message that names them.                                               */
/* ========================================================================== */

static Message *pending_of(Session *s, Snowflake channel_id, Snowflake nonce) {
  Channel *c = store_channel(s->store, channel_id);

  for (size_t i = c != nullptr ? c->nmessages : 0; i > 0; i--) {
    Message *m = c->messages[i - 1];

    if (m->state != MESSAGE_SENT && m->nonce == nonce) {
      return m;
    }
  }

  return nullptr;
}

// drops the files of a message once discord took it, or once it is gone.
static void forget_files(Session *s, Snowflake nonce, bool bucket) {
  for (size_t i = s->nuploads; i > 0; i--) {
    Upload *u = s->uploads[i - 1];

    if (u->nonce == nonce) {
      if (bucket) {
        forget_bucket(s, u);
      }

      take_out(s, u);
    }
  }
}

static void on_sent(void *ctx, const ApiResponse *res) {
  MessageCall *call = ctx;
  Session *s = call->s;
  Snowflake channel_id = call->channel_id, nonce = call->nonce;

  free(call);

  if (!uploads_owns(s, nonce)) {
    return;
  }

  if (res->ok) {
    store_create_message(s->store, res->json);
    store_remove_pending(s->store, channel_id, nonce);
    forget_files(s, nonce, false);
  }
  else {
    // discord lost the files, they go up again with the next try.
    if (res->error.code == 50146) {
      for (size_t i = 0; i < s->nuploads; i++) {
        Upload *u = s->uploads[i];

        if (u->nonce == nonce) {
          str_set(&u->upload_url, nullptr);
          str_set(&u->uploaded_filename, nullptr);
          u->state = UPLOAD_WAITING;
        }
      }
    }

    store_fail_pending(s->store, channel_id, nonce, res->error.message);
  }

  changed(s);
}

// offline, what discord would have answered is made up, the files pointing
// at themselves on disk.
static void echo(Session *s, const Message *pending, Upload **files, size_t n) {
  cima_json_writer w = {};
  char url[1100], stamp[40];
  time_t now = (time_t)store_now(s->store);
  struct tm tm;

  gmtime_r(&now, &tm);
  strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S.000000+00:00", &tm);

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, pending->nonce);
  json_key(&w, "nonce"), snowflake_write(&w, pending->nonce);
  json_key(&w, "channel_id"), snowflake_write(&w, pending->channel_id);
  json_field(&w, "content", pending->content);
  json_field(&w, "timestamp", stamp);
  json_key(&w, "author");
  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, s->store->self_id);
  json_end_object(&w);
  json_key(&w, "attachments");
  json_begin_array(&w);

  for (size_t i = 0; i < n; i++) {
    snprintf(url, sizeof url, "file://%s", files[i]->path);
    json_begin_object(&w);
    json_key(&w, "id"), snowflake_write(&w, pending->nonce + i + 1);
    json_field(&w, "filename", files[i]->filename);
    json_field(&w, "size", files[i]->size);
    json_field(&w, "url", url);
    json_field(&w, "flags", files[i]->spoiler ? 8 : 0);

    if (files[i]->content_type != nullptr) {
      json_field(&w, "content_type", files[i]->content_type);
    }

    json_end_object(&w);
  }

  json_end_array(&w);
  json_end_object(&w);

  cilo_arena arena = {};
  store_create_message(
    s->store, json_parse(&arena, cima_json_writer_text(&w), w.len, nullptr));
  cilo_arena_drop(&arena);
  cima_json_writer_dispose(&w);
}

static void try_send(Session *s, Snowflake channel_id, Snowflake nonce) {
  Upload *files[UPLOADS_MAX];
  size_t n = uploads_of(s, channel_id, nonce, files, UPLOADS_MAX);

  if (nonce == 0 || n == 0) {
    return;
  }

  for (size_t i = 0; i < n && i < UPLOADS_MAX; i++) {
    if (files[i]->state != UPLOAD_DONE) {
      return;
    }
  }

  Message *pending = pending_of(s, channel_id, nonce);

  if (pending == nullptr) {
    forget_files(s, nonce, true);
    return;
  }

  n = n < UPLOADS_MAX ? n : UPLOADS_MAX;

  if (s->offline) {
    echo(s, pending, files, n);
    store_remove_pending(s->store, channel_id, nonce);
    forget_files(s, nonce, false);
    changed(s);
    return;
  }

  if (!online(s)) {
    store_fail_pending(s->store, channel_id, nonce, "Not connected.");
    return;
  }

  ApiAttachment attachments[UPLOADS_MAX];

  for (size_t i = 0; i < n; i++) {
    attachments[i] = (ApiAttachment){
      .filename = files[i]->filename,
      .uploaded_filename = files[i]->uploaded_filename,
      .description = files[i]->description,
      .content_type = files[i]->content_type,
      .spoiler = files[i]->spoiler,
    };
  }

  ApiSendMessage message = {
    .content = pending->content,
    .attachments = attachments,
    .nattachments = n,
    .nonce = nonce,
    .reply_to = pending->reference_id,
    .reply_ping = files[0]->reply_ping,
  };
  MessageCall *call = calloc(1, sizeof *call);

  if (call == nullptr) {
    cilo_oom(sizeof *call);
  }

  *call = (MessageCall){.s = s, .channel_id = channel_id, .nonce = nonce};
  api_send_message(s->api, channel_id, &message, on_sent, call);
}

bool uploads_attach(Session *s, Snowflake channel_id, Snowflake nonce,
                    bool reply_ping) {
  bool any = false;

  for (size_t i = 0; i < s->nuploads; i++) {
    Upload *u = s->uploads[i];

    if (u->channel_id == channel_id && u->nonce == 0) {
      u->nonce = nonce;
      u->reply_ping = reply_ping;
      any = true;
    }
  }

  return any;
}

void uploads_send(Session *s, Snowflake channel_id, Snowflake nonce) {
  double now = platform_now();

  for (size_t i = 0; i < s->nuploads; i++) {
    Upload *u = s->uploads[i];

    if (u->channel_id != channel_id || u->nonce != nonce) {
      continue;
    }

    bool stale = u->state == UPLOAD_DONE && now - u->reserved_at > URL_LIFETIME;

    if (u->state == UPLOAD_FAILED || u->state == UPLOAD_WAITING || stale) {
      // an empty file never goes.
      if (u->size == 0) {
        fail_message(s, u);
        return;
      }

      str_set(&u->upload_url, nullptr);
      str_set(&u->uploaded_filename, nullptr);
      u->attempts = 0;
      reserve(s, u);

      if (u->state == UPLOAD_WAITING) {
        store_fail_pending(s->store, channel_id, nonce, "Not connected.");
        return;
      }
    }
  }

  try_send(s, channel_id, nonce);
}

void uploads_cancel(Session *s, Snowflake channel_id, Snowflake nonce) {
  forget_files(s, nonce, true);
  store_remove_pending(s->store, channel_id, nonce);
  changed(s);
}

void uploads_clear(Session *s) {
  for (size_t i = 0; i < s->nuploads; i++) {
    upload_free(s->uploads[i]);
  }

  free(s->uploads);
  s->uploads = nullptr;
  s->nuploads = 0;
}
