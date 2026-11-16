#ifndef QUARREL_DISCORD_SETTINGS_PROTO_H
#define QUARREL_DISCORD_SETTINGS_PROTO_H

#include <stddef.h>

// turns a PreloadedUserSettings protobuf, in base64 as the gateway sends
// it, into the json `user_settings` used to be, with the fields Quarrel
// reads: `status`, `guild_folders` and `guild_positions`. a field the
// message does not carry is left out, as in a partial update. from malloc,
// null when `base64` is no such message.
char *settings_proto_to_json(const char *base64);

#endif
