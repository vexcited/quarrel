#ifndef QUARREL_DISCORD_GATEWAY_H
#define QUARREL_DISCORD_GATEWAY_H

#include "discord/util.h"
#include <cima/json_dsl.h>

/* ========================================================================== */
/* the gateway connection: identifying, heartbeats, resuming after a drop     */
/* and backing off between attempts. everything runs on the main thread.      */
/* ========================================================================== */

typedef enum : uint8_t {
  // not connected and not trying to.
  GATEWAY_STOPPED,
  // opening the socket or waiting for discord's hello.
  GATEWAY_CONNECTING,
  // identified or resuming, waiting for READY or RESUMED.
  GATEWAY_HANDSHAKING,
  GATEWAY_READY,
  // the connection dropped and the next attempt waits for its turn.
  GATEWAY_WAITING,
} GatewayStatus;

typedef enum : uint8_t {
  // the token is no good: discord closed with 4004.
  GATEWAY_FATAL_AUTH,
  // discord refused the connection for good, a bad version for one.
  GATEWAY_FATAL_REFUSED,
} GatewayFatal;

typedef struct {
  void *ctx;
  // a dispatch, `data` only valid during the call. READY and RESUMED come
  // through here like any other, after the gateway took what it needs.
  void (*dispatch)(void *ctx, const char *type, const cima_json *data);
  // the whole payload of a READY or a READY_SUPPLEMENTAL as discord wrote
  // it, before its dispatch, for who keeps it. may be null.
  void (*snapshot)(void *ctx, const char *type, const char *text, size_t len);
  void (*status)(void *ctx, GatewayStatus status);
  // the gateway gave up and stopped. `reason` is discord's, may be empty.
  void (*fatal)(void *ctx, GatewayFatal why, int code, const char *reason);
} GatewayEvents;

typedef struct Gateway Gateway;

// the capabilities Quarrel identifies with, see gateway.c for why these.
extern const uint32_t GATEWAY_CAPABILITIES;

Gateway *gateway_new(const char *token, const GatewayEvents *events);
// no callback runs after this.
void gateway_free(Gateway *g);

// starts connecting, and keeps reconnecting until gateway_stop or a fatal
// close.
void gateway_start(Gateway *g);
// closes the connection and ends the session.
void gateway_stop(Gateway *g);
GatewayStatus gateway_status(const Gateway *g);
// the network under the connection changed, or the machine woke up:
// `online` for whether there is one, `returned` when there was none before.
// a dead connection is found within seconds rather than at the next
// heartbeat, and an attempt that waits for its turn is made at once.
void gateway_network_changed(Gateway *g, bool online, bool returned);
// the round trip of the last heartbeat in seconds, negative before the
// first.
double gateway_latency(const Gateway *g);

// the token to resume and identify with from now on.
void gateway_set_token(Gateway *g, const char *token);

/* ========================================================================== */
/* commands. each is dropped when the gateway is not ready, and answers       */
/* whether it was sent.                                                       */
/* ========================================================================== */

// `status` is "online", "idle", "dnd" or "invisible".
bool gateway_update_presence(Gateway *g, const char *status, bool afk);

// joins the voice channel, moves to it, or leaves voice with a
// `channel_id` of 0. `guild_id` is 0 for a call in a private channel.
// discord answers a join with VOICE_STATE_UPDATE, which names the session,
// and VOICE_SERVER_UPDATE, which tells where to connect.
bool gateway_voice_state(Gateway *g, Snowflake guild_id, Snowflake channel_id,
                         bool mute, bool deaf);
// the session's id, null before the first READY.
const char *gateway_session_id(const Gateway *g);

typedef struct {
  uint32_t first, last;
} GatewayRange;

// asks for the member list of a channel, its rows in `ranges`, and for the
// typing, thread and activity events of its guild. discord answers with
// GUILD_MEMBER_LIST_UPDATE and keeps the ranges in sync from then on. the
// subscription replaces the guild's previous one.
bool gateway_subscribe(Gateway *g, Snowflake guild_id, Snowflake channel_id,
                       const GatewayRange *ranges, size_t nranges);

typedef struct {
  Snowflake thread_id, ack_id;
} GatewayForumAck;

// asks how many messages came in up to 180 posts of a forum since they were
// read up to `ack_id`, answered by FORUM_UNREADS.
bool gateway_request_forum_unreads(Gateway *g, Snowflake guild_id,
                                   Snowflake forum_id,
                                   const GatewayForumAck *acks, size_t count);

// asks for the members of up to 100 users, answered by GUILD_MEMBERS_CHUNK.
bool gateway_request_members(Gateway *g, Snowflake guild_id,
                             const Snowflake *user_ids, size_t count);
// asks for up to `limit` members whose names start with `query`, answered
// by GUILD_MEMBERS_CHUNK. what pickers of members ask while the user types.
bool gateway_search_members(Gateway *g, Snowflake guild_id, const char *query,
                            uint32_t limit);

/* ========================================================================== */
/* for tests.                                                                 */
/* ========================================================================== */

// connects there instead of discord's gateway.
void gateway_set_url(Gateway *g, const char *url);

#endif
