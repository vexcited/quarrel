#include "voice/rtp.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

constexpr size_t TAG = 16;
constexpr size_t NONCE = 4;

struct RtpCipher {
  EVP_CIPHER_CTX *seal, *open;
};

RtpCipher *rtp_cipher_new(const uint8_t key[static 32]) {
  RtpCipher *c = calloc(1, sizeof *c);

  if (c == nullptr) {
    return nullptr;
  }

  c->seal = EVP_CIPHER_CTX_new();
  c->open = EVP_CIPHER_CTX_new();

  // the key is set once, each packet only gives its nonce after.
  bool ok =
    c->seal != nullptr && c->open != nullptr &&
    EVP_EncryptInit_ex(c->seal, EVP_aes_256_gcm(), nullptr, key, nullptr) ==
      1 &&
    EVP_DecryptInit_ex(c->open, EVP_aes_256_gcm(), nullptr, key, nullptr) == 1;

  if (!ok) {
    rtp_cipher_free(c);
    return nullptr;
  }

  return c;
}

void rtp_cipher_free(RtpCipher *c) {
  if (c == nullptr) {
    return;
  }

  EVP_CIPHER_CTX_free(c->seal);
  EVP_CIPHER_CTX_free(c->open);
  free(c);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}

// the counter, then zeros.
static void nonce_of(const uint8_t counter[NONCE], uint8_t iv[12]) {
  memset(iv, 0, 12);
  memcpy(iv, counter, NONCE);
}

size_t rtp_seal(RtpCipher *c, const RtpHeader *h, uint32_t nonce,
                const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
  if (cap < len + RTP_OVERHEAD) {
    return 0;
  }

  uint8_t iv[12];
  uint8_t *body = out + RTP_HEADER;
  uint8_t *tail = body + len;
  int n = 0, m = 0;

  out[0] = 0x80;
  out[1] = h->type;
  put16(out + 2, h->sequence);
  put32(out + 4, h->timestamp);
  put32(out + 8, h->ssrc);
  put32(tail + TAG, nonce);
  nonce_of(tail + TAG, iv);

  bool ok =
    EVP_EncryptInit_ex(c->seal, nullptr, nullptr, nullptr, iv) == 1 &&
    EVP_EncryptUpdate(c->seal, nullptr, &n, out, (int)RTP_HEADER) == 1 &&
    EVP_EncryptUpdate(c->seal, body, &n, payload, (int)len) == 1 &&
    EVP_EncryptFinal_ex(c->seal, body + n, &m) == 1 &&
    EVP_CIPHER_CTX_ctrl(c->seal, EVP_CTRL_GCM_GET_TAG, (int)TAG, tail) == 1;

  return ok ? len + RTP_OVERHEAD : 0;
}

bool rtp_is_rtcp(const uint8_t *packet, size_t len) {
  return len >= 2 && packet[1] >= 192 && packet[1] <= 223;
}

bool rtp_open(RtpCipher *c, const uint8_t *packet, size_t len, RtpHeader *h,
              uint8_t *out, size_t cap, size_t *out_len) {
  if (len < RTP_OVERHEAD || (packet[0] >> 6) != 2 || rtp_is_rtcp(packet, len)) {
    return false;
  }

  bool padded = packet[0] & 0x20, extended = packet[0] & 0x10;
  size_t clear = RTP_HEADER + 4 * (size_t)(packet[0] & 0x0f);
  size_t extension = 0;

  // of an extension, the four bytes that say its kind and length are in the
  // clear with the header, and what it holds is encrypted with the payload.
  if (extended) {
    if (len < clear + 4 + TAG + NONCE) {
      return false;
    }

    extension = 4 * ((size_t)packet[clear + 2] << 8 | packet[clear + 3]);
    clear += 4;
  }

  if (len < clear + TAG + NONCE) {
    return false;
  }

  size_t sealed = len - clear - TAG - NONCE;
  const uint8_t *tag = packet + clear + sealed;
  uint8_t iv[12];
  int n = 0, m = 0;

  if (sealed > cap || sealed < extension) {
    return false;
  }

  nonce_of(tag + TAG, iv);

  bool ok =
    EVP_DecryptInit_ex(c->open, nullptr, nullptr, nullptr, iv) == 1 &&
    EVP_DecryptUpdate(c->open, nullptr, &n, packet, (int)clear) == 1 &&
    EVP_DecryptUpdate(c->open, out, &n, packet + clear, (int)sealed) == 1 &&
    EVP_CIPHER_CTX_ctrl(c->open, EVP_CTRL_GCM_SET_TAG, (int)TAG,
                        (void *)tag) == 1 &&
    EVP_DecryptFinal_ex(c->open, out + n, &m) == 1;

  if (!ok) {
    return false;
  }

  size_t payload = sealed - extension;

  if (padded && payload > 0) {
    size_t pad = out[sealed - 1];

    if (pad > payload) {
      return false;
    }

    payload -= pad;
  }

  memmove(out, out + extension, payload);
  *out_len = payload;
  h->type = packet[1] & 0x7f;
  h->sequence = (uint16_t)(packet[2] << 8 | packet[3]);
  h->timestamp = get32(packet + 4);
  h->ssrc = get32(packet + 8);
  return true;
}
