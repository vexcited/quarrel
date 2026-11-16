#ifndef QUARREL_DISCORD_GATEWAY_H
#define QUARREL_DISCORD_GATEWAY_H

#include "discord/util.h"
#include "json.h"

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
  void (*dispatch)(void *ctx, const char *type, const Json *data);
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

typedef struct {
  uint32_t first, last;
} GatewayRange;

// asks for the member list of a channel, its rows in `ranges`, and for the
// typing, thread and activity events of its guild. discord answers with
// GUILD_MEMBER_LIST_UPDATE and keeps the ranges in sync from then on. the
// subscription replaces the guild's previous one.
bool gateway_subscribe(Gateway *g, Snowflake guild_id, Snowflake channel_id,
                       const GatewayRange *ranges, size_t nranges);

// asks for the members of up to 100 users, answered by GUILD_MEMBERS_CHUNK.
bool gateway_request_members(Gateway *g, Snowflake guild_id,
                             const Snowflake *user_ids, size_t count);

/* ========================================================================== */
/* for tests.                                                                 */
/* ========================================================================== */

// connects there instead of discord's gateway.
void gateway_set_url(Gateway *g, const char *url);

#endif
