#ifndef QUARREL_VOICE_DAVE_H
#define QUARREL_VOICE_DAVE_H

#include <stddef.h>
#include <stdint.h>

#include "discord/util.h"

/* ========================================================================== */
/* discord's end to end encryption of calls, dave, over its libdave: the mls  */
/* group every member of a call is in, and the keys of each frame that come   */
/* out of it. the voice socket carries the group's messages, which the main   */
/* thread hands here. frames are encrypted and decrypted from the media       */
/* thread.                                                                    */
/* ========================================================================== */

typedef struct Dave Dave;

typedef struct {
  void *ctx;
  // what the group asks to be sent on the voice socket, from the main
  // thread: the binary ops 26 and 28, and the json ones 23 and 31.
  void (*key_package)(void *ctx, const uint8_t *bytes, size_t len);
  void (*commit_welcome)(void *ctx, const uint8_t *bytes, size_t len);
  void (*ready_for_transition)(void *ctx, uint16_t transition_id);
  void (*invalid_commit_welcome)(void *ctx, uint16_t transition_id);
} DaveEvents;

// the highest version of the protocol this build speaks.
uint16_t dave_max_version(void);

// `group_id` is the channel's id.
Dave *dave_new(Snowflake self_id, Snowflake group_id, const DaveEvents *e);
void dave_free(Dave *d);

// who else is in the call, from the socket's ops 11 and 13.
void dave_add_user(Dave *d, Snowflake user_id);
void dave_remove_user(Dave *d, Snowflake user_id);

// the ssrc the account's own audio is sent with.
void dave_set_ssrc(Dave *d, uint32_t ssrc);

// the version the socket's session description, op 4, chose. 0 is a call
// without dave.
void dave_selected(Dave *d, uint16_t version);
// ops 21, 22 and 24.
void dave_prepare_transition(Dave *d, uint16_t transition_id, uint16_t version);
void dave_execute_transition(Dave *d, uint16_t transition_id);
void dave_prepare_epoch(Dave *d, uint64_t epoch, uint16_t version);
// the binary ops 25, 27, 29 and 30, without their sequence and opcode.
void dave_external_sender(Dave *d, const uint8_t *bytes, size_t len);
void dave_proposals(Dave *d, const uint8_t *bytes, size_t len);
void dave_commit(Dave *d, uint16_t transition_id, const uint8_t *bytes,
                 size_t len);
void dave_welcome(Dave *d, uint16_t transition_id, const uint8_t *bytes,
                  size_t len);

// what an opus frame grows to at most.
size_t dave_sealed_size(Dave *d, size_t len);
// 0 when the frame cannot be sent yet, the group's keys not being there.
size_t dave_seal(Dave *d, uint32_t ssrc, const uint8_t *frame, size_t len,
                 uint8_t *out, size_t cap);
// 0 when the frame of `user_id` cannot be read.
size_t dave_open(Dave *d, Snowflake user_id, const uint8_t *frame, size_t len,
                 uint8_t *out, size_t cap);

// whether frames are encrypted end to end right now.
bool dave_active(Dave *d);

#endif
