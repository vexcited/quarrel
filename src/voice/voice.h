#ifndef QUARREL_VOICE_VOICE_H
#define QUARREL_VOICE_VOICE_H

#include "discord/util.h"

/* ========================================================================== */
/* one call: the voice socket that sets it up, the udp socket its sound goes  */
/* through, and the microphone and speakers. the gateway says where to        */
/* connect, see session_voice_join. everything is called from the main        */
/* thread, `changed` runs on it. sound is encoded, encrypted and mixed on     */
/* threads of the call's own.                                                 */
/* ========================================================================== */

typedef enum : uint8_t {
  VOICE_IDLE,
  // the socket is opening or the handshake is under way.
  VOICE_CONNECTING,
  VOICE_CONNECTED,
  // the connection dropped and is being taken up again.
  VOICE_RESUMING,
  // it ended for a reason voice_error tells.
  VOICE_FAILED,
} VoiceStatus;

typedef struct Voice Voice;

typedef struct {
  void *ctx;
  // the status, who speaks or the latency changed.
  void (*changed)(void *ctx);
  // discord ended the call, the account was moved out or disconnected: the
  // gateway's voice state is to be let go of.
  void (*ended)(void *ctx);
} VoiceEvents;

Voice *voice_new(Snowflake self_id, const VoiceEvents *e);
void voice_free(Voice *v);

// connects with what the gateway's VOICE_STATE_UPDATE and
// VOICE_SERVER_UPDATE gave. `guild_id` is the channel's id for a call in a
// private channel. a call under way is dropped first.
void voice_connect(Voice *v, Snowflake guild_id, Snowflake channel_id,
                   const char *session_id, const char *token,
                   const char *endpoint);
void voice_disconnect(Voice *v);

VoiceStatus voice_status(const Voice *v);
// why it failed, never null.
const char *voice_error(const Voice *v);
Snowflake voice_channel(const Voice *v);
// the round trip of the socket's last heartbeat in seconds, negative before
// the first.
double voice_latency(const Voice *v);
// whether sound is encrypted end to end.
bool voice_encrypted(const Voice *v);

// a muted microphone sends nothing, a deafened call plays nothing.
void voice_set_mute(Voice *v, bool mute);
void voice_set_deaf(Voice *v, bool deaf);
// how loud the microphone is, 0 to 1, 0 while muted.
float voice_input_level(const Voice *v);
// the microphone could not be used, the call only listens.
bool voice_no_microphone(const Voice *v);
// the microphone was started and is not heard yet.
bool voice_microphone_starting(const Voice *v);

// whether sound of `user_id` came in the last moments, the account's own
// microphone for its own id.
bool voice_speaking(const Voice *v, Snowflake user_id);

#endif
