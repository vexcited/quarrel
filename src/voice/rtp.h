#ifndef QUARREL_VOICE_RTP_H
#define QUARREL_VOICE_RTP_H

#include <stddef.h>
#include <stdint.h>

/* ========================================================================== */
/* the packets voice goes in: rtp, sealed with discord's                      */
/* aead_aes256_gcm_rtpsize. the header is in the clear and authenticated,     */
/* the payload is encrypted, then come the 16 bytes of the tag and the 4 of   */
/* a counter that is the start of the nonce. one cipher is used by one        */
/* thread.                                                                    */
/* ========================================================================== */

constexpr size_t RTP_HEADER = 12;
// what sealing adds to a payload: the header, the tag and the nonce.
constexpr size_t RTP_OVERHEAD = RTP_HEADER + 16 + 4;
constexpr uint8_t RTP_OPUS = 120;

typedef struct RtpCipher RtpCipher;

// null when the key cannot be used.
RtpCipher *rtp_cipher_new(const uint8_t key[static 32]);
void rtp_cipher_free(RtpCipher *c);

typedef struct {
  uint8_t type;
  uint16_t sequence;
  uint32_t timestamp, ssrc;
} RtpHeader;

// writes the packet of `payload` into `out`, which holds `len` and
// RTP_OVERHEAD more. `nonce` is one more for every packet. 0 when it fails.
size_t rtp_seal(RtpCipher *c, const RtpHeader *h, uint32_t nonce,
                const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

// reads a packet: its header into `h` and its payload, the header's
// extension skipped, into `out`. false for what is no rtp, rtcp for one, or
// does not authenticate.
bool rtp_open(RtpCipher *c, const uint8_t *packet, size_t len, RtpHeader *h,
              uint8_t *out, size_t cap, size_t *out_len);

// whether a packet is rtcp, which shares the socket.
[[gnu::pure]] bool rtp_is_rtcp(const uint8_t *packet, size_t len);

#endif
