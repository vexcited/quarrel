#ifndef QUARREL_DISCORD_SETTINGS_PROTO_H
#define QUARREL_DISCORD_SETTINGS_PROTO_H

#include <stddef.h>
#include <stdint.h>

// turns a PreloadedUserSettings protobuf, in base64 as the gateway sends
// it, into the json `user_settings` used to be, with the fields Quarrel
// reads: `status`, `guild_folders` and `guild_positions`. a field the
// message does not carry is left out, as in a partial update. from malloc,
// null when `base64` is no such message.
char *settings_proto_to_json(const char *base64);

constexpr size_t FRECENCY_SAMPLES = 10;

// how much something was used, as the client tracks it.
typedef struct {
  // an emoji's first name, or its id for one of a guild.
  char *key;
  uint32_t total_uses;
  // unix milliseconds of the last uses.
  int64_t recent_uses[FRECENCY_SAMPLES];
  size_t nrecent_uses;
} FrecencyUse;

// the emojis reacted with of a FrecencyUserSettings protobuf in base64.
// from malloc with its keys, null when it has none.
FrecencyUse *settings_proto_reaction_uses(const char *base64, size_t *count);

#endif
