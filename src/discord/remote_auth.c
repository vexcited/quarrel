#include "discord/remote_auth.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "discord/api.h"
#include "platform/platform.h"

#define REMOTE_AUTH_URL "wss://remote-auth-gateway.discord.gg/?v=2"

// a key made off the main thread, which takes a moment. `ra` is only
// touched on the main thread and is null once the RemoteAuth is gone.
typedef struct {
  RemoteAuth *ra;
  EVP_PKEY *key;
} Keygen;

struct RemoteAuth {
  RemoteAuthEvents events;
  PlatformWebSocket *ws;
  PlatformTimer *heartbeat;
  Keygen *keygen;
  EVP_PKEY *key;
  double interval;
  bool open, acked;
};

static void drop_socket(RemoteAuth *ra) {
  platform_timer_cancel(ra->heartbeat);
  ra->heartbeat = nullptr;

  if (ra->ws != nullptr) {
    platform_ws_close(ra->ws, 1000, "");
    platform_ws_free(ra->ws);
    ra->ws = nullptr;
  }

  ra->open = false;
}

// the callback may free `ra`, nothing touches it after.
static void fail(RemoteAuth *ra) {
  drop_socket(ra);

  if (ra->events.failed != nullptr) {
    ra->events.failed(ra->events.ctx);
  }
}

static void send_json(RemoteAuth *ra, cima_json_writer *w) {
  if (ra->ws != nullptr) {
    platform_ws_send_text(ra->ws, cima_json_writer_text(w));
  }

  cima_json_writer_dispose(w);
}

// base64 without padding, `-` and `_` for `+` and `/`.
static char *base64_url(const void *bytes, size_t len) {
  char *text = base64_encode(bytes, len);

  if (text == nullptr) {
    return nullptr;
  }

  char *at = text;

  for (; *at != '\0' && *at != '='; at++) {
    *at = *at == '+' ? '-' : *at == '/' ? '_' : *at;
  }

  *at = '\0';
  return text;
}

// the public key as the SubjectPublicKeyInfo der the web client exports.
static uint8_t *public_key(EVP_PKEY *key, size_t *len) {
  uint8_t *der = nullptr;
  int n = i2d_PUBKEY(key, &der);

  if (n <= 0) {
    return nullptr;
  }

  *len = (size_t)n;
  return der;
}

// the sha-256 of the public key, which discord names the session after.
static char *fingerprint_of(EVP_PKEY *key) {
  size_t len = 0;
  uint8_t *der = public_key(key, &len);
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  char *text = nullptr;

  if (der != nullptr &&
      EVP_Digest(der, len, digest, &size, EVP_sha256(), nullptr) == 1) {
    text = base64_url(digest, size);
  }

  OPENSSL_free(der);
  return text;
}

// rsa-oaep with sha-256, nul terminated for the text it may hold.
static uint8_t *decrypt(EVP_PKEY *key, const char *encoded, size_t *len) {
  size_t in_len = 0;
  uint8_t *in = base64_decode(encoded, &in_len);
  EVP_PKEY_CTX *ctx =
    key != nullptr && in != nullptr ? EVP_PKEY_CTX_new(key, nullptr) : nullptr;
  uint8_t *out = nullptr;
  size_t out_len = 0;

  bool ready = ctx != nullptr && EVP_PKEY_decrypt_init(ctx) == 1 &&
               EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) == 1 &&
               EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) == 1 &&
               EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) == 1 &&
               EVP_PKEY_decrypt(ctx, nullptr, &out_len, in, in_len) == 1;

  if (ready) {
    out = malloc(out_len + 1);

    if (out != nullptr &&
        EVP_PKEY_decrypt(ctx, out, &out_len, in, in_len) == 1) {
      out[out_len] = '\0';
      *len = out_len;
    }
    else {
      free(out);
      out = nullptr;
    }
  }

  EVP_PKEY_CTX_free(ctx);
  free(in);
  return out;
}

static void send_init(RemoteAuth *ra) {
  size_t len = 0;
  uint8_t *der = public_key(ra->key, &len);
  char *encoded = der != nullptr ? base64_encode(der, len) : nullptr;
  cima_json_writer w = {};

  OPENSSL_free(der);

  if (encoded == nullptr) {
    fail(ra);
    return;
  }

  json_begin_object(&w);
  json_field(&w, "op", "init");
  json_field(&w, "encoded_public_key", encoded);
  json_end_object(&w);
  send_json(ra, &w);
  free(encoded);
}

static void on_heartbeat(void *ctx) {
  RemoteAuth *ra = ctx;
  cima_json_writer w = {};

  ra->heartbeat = nullptr;

  // the last one was never answered, the connection is dead.
  if (!ra->acked) {
    fail(ra);
    return;
  }

  ra->acked = false;
  json_begin_object(&w);
  json_field(&w, "op", "heartbeat");
  json_end_object(&w);
  send_json(ra, &w);

  ra->heartbeat = platform_timer(ra->interval, on_heartbeat, ra);
}

static void on_hello(RemoteAuth *ra, const cima_json *payload) {
  double interval =
    json_get_coerce(payload, "heartbeat_interval", (double)41250) / 1000;
  uint32_t random;

  ra->interval = interval < 1 ? 1 : interval;
  ra->acked = true;

  // the first one comes after a random part of the interval.
  platform_random(&random, sizeof random);
  platform_timer_cancel(ra->heartbeat);
  ra->heartbeat = platform_timer(
    ra->interval * ((double)random / 4294967296.0), on_heartbeat, ra);
}

static void on_nonce(RemoteAuth *ra, const cima_json *payload) {
  size_t len = 0;
  uint8_t *nonce =
    decrypt(ra->key, json_get_or(payload, "encrypted_nonce", nullptr), &len);
  char *proof = nonce != nullptr ? base64_url(nonce, len) : nullptr;
  cima_json_writer w = {};

  free(nonce);

  if (proof == nullptr) {
    fail(ra);
    return;
  }

  json_begin_object(&w);
  json_field(&w, "op", "nonce_proof");
  json_field(&w, "nonce", proof);
  json_end_object(&w);
  send_json(ra, &w);
  free(proof);
}

static void on_remote_init(RemoteAuth *ra, const cima_json *payload) {
  const char *sent = json_get_or(payload, "fingerprint", nullptr);
  char *ours = fingerprint_of(ra->key);

  // the session discord made is not for this key.
  if (ours == nullptr || sent == nullptr || strcmp(ours, sent) != 0) {
    free(ours);
    fail(ra);
    return;
  }

  if (ra->events.ready != nullptr) {
    ra->events.ready(ra->events.ctx, ours);
  }

  free(ours);
}

static bool all_of(const char *s, size_t n, int (*accepts)(int)) {
  for (size_t i = 0; i < n; i++) {
    if (!accepts((unsigned char)s[i])) {
      return false;
    }
  }

  return n > 0;
}

static int is_avatar(int c) {
  return isalnum(c) || c == '_';
}

// "id:discriminator:avatar:username", the avatar "0" for none. the
// username comes last since it may hold colons.
static bool parse_user(char *text, RemoteAuthUser *user) {
  char *id = text;
  char *discriminator = strchr(id, ':');
  char *avatar = discriminator ? strchr(discriminator + 1, ':') : nullptr;
  char *username = avatar ? strchr(avatar + 1, ':') : nullptr;

  if (username == nullptr) {
    return false;
  }

  *discriminator++ = *avatar++ = *username++ = '\0';

  size_t digits = strlen(discriminator);

  if (!all_of(id, strlen(id), isdigit) || digits > 4 ||
      !all_of(discriminator, digits, isdigit) ||
      !all_of(avatar, strlen(avatar), is_avatar)) {
    return false;
  }

  *user = (RemoteAuthUser){
    .id = snowflake_parse(id),
    .discriminator = discriminator,
    .username = username,
    .avatar = strcmp(avatar, "0") != 0 ? avatar : nullptr,
  };
  return true;
}

static void on_pending_ticket(RemoteAuth *ra, const cima_json *payload) {
  size_t len = 0;
  char *text = (char *)decrypt(
    ra->key, json_get_or(payload, "encrypted_user_payload", nullptr), &len);
  RemoteAuthUser user;

  if (text == nullptr || strlen(text) != len || !parse_user(text, &user)) {
    free(text);
    fail(ra);
    return;
  }

  if (ra->events.scanned != nullptr) {
    ra->events.scanned(ra->events.ctx, &user);
  }

  free(text);
}

static void on_text(void *ctx, const char *text, size_t len) {
  RemoteAuth *ra = ctx;
  cilo_arena arena = {};
  const cima_json *payload = json_parse(&arena, text, len, nullptr);
  const char *op = json_get_or(payload, "op", "");

  if (strcmp(op, "hello") == 0) {
    on_hello(ra, payload);
  }
  else if (strcmp(op, "heartbeat_ack") == 0) {
    ra->acked = true;
  }
  else if (strcmp(op, "nonce_proof") == 0) {
    on_nonce(ra, payload);
  }
  else if (strcmp(op, "pending_remote_init") == 0) {
    on_remote_init(ra, payload);
  }
  else if (strcmp(op, "pending_ticket") == 0) {
    on_pending_ticket(ra, payload);
  }
  else if (strcmp(op, "pending_login") == 0) {
    const char *ticket = json_get_or(payload, "ticket", nullptr);

    if (ticket == nullptr) {
      fail(ra);
    }
    else {
      drop_socket(ra);

      if (ra->events.ticket != nullptr) {
        ra->events.ticket(ra->events.ctx, ticket);
      }
    }
  }
  else if (strcmp(op, "cancel") == 0) {
    drop_socket(ra);

    if (ra->events.cancelled != nullptr) {
      ra->events.cancelled(ra->events.ctx);
    }
  }

  cilo_arena_drop(&arena);
}

static void on_open(void *ctx) {
  RemoteAuth *ra = ctx;

  ra->open = true;

  if (ra->key != nullptr) {
    send_init(ra);
  }
}

static void on_closed(void *ctx, int, const char *) {
  RemoteAuth *ra = ctx;

  platform_ws_free(ra->ws);
  ra->ws = nullptr;
  fail(ra);
}

static void generated(void *ctx) {
  Keygen *job = ctx;
  RemoteAuth *ra = job->ra;
  EVP_PKEY *key = job->key;

  free(job);

  if (ra == nullptr) {
    EVP_PKEY_free(key);
    return;
  }

  ra->keygen = nullptr;
  ra->key = key;

  if (key == nullptr) {
    fail(ra);
  }
  else if (ra->open) {
    send_init(ra);
  }
}

static void *generate(void *arg) {
  Keygen *job = arg;

  job->key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", (size_t)2048);
  platform_post(generated, job);
  return nullptr;
}

RemoteAuth *remote_auth_new(const RemoteAuthEvents *events) {
  RemoteAuth *ra = calloc(1, sizeof *ra);
  Keygen *job = calloc(1, sizeof *job);
  pthread_t thread;

  if (ra == nullptr || job == nullptr) {
    cilo_oom(sizeof *ra + sizeof *job);
  }

  ra->events = *events;
  job->ra = ra;
  ra->keygen = job;

  if (pthread_create(&thread, nullptr, generate, job) != 0) {
    generate(job);
  }
  else {
    pthread_detach(thread);
  }

  // what the web client sends, discord refuses other origins.
  PlatformHeader headers[] = {
    {"Origin", "https://discord.com"},
    {"User-Agent", api_user_agent()},
  };

  PlatformWebSocketEvents socket = {
    .ctx = ra,
    .open = on_open,
    .text = on_text,
    .closed = on_closed,
  };

  ra->ws = platform_ws_connect(REMOTE_AUTH_URL, headers, 2, &socket);
  return ra;
}

void remote_auth_free(RemoteAuth *ra) {
  if (ra == nullptr) {
    return;
  }

  ra->events = (RemoteAuthEvents){};

  if (ra->keygen != nullptr) {
    ra->keygen->ra = nullptr;
  }

  drop_socket(ra);
  EVP_PKEY_free(ra->key);
  free(ra);
}

char *remote_auth_decrypt(RemoteAuth *ra, const char *encrypted) {
  size_t len = 0;
  char *token = ra != nullptr ? (char *)decrypt(ra->key, encrypted, &len)
                              : nullptr;

  if (token != nullptr && strlen(token) != len) {
    free(token);
    return nullptr;
  }

  return token;
}
