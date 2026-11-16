#ifndef QUARREL_DISCORD_PERMISSIONS_H
#define QUARREL_DISCORD_PERMISSIONS_H

#include "discord/util.h"

typedef uint64_t Permissions;

constexpr Permissions PERM_CREATE_INSTANT_INVITE = 1ull << 0;
constexpr Permissions PERM_KICK_MEMBERS = 1ull << 1;
constexpr Permissions PERM_BAN_MEMBERS = 1ull << 2;
constexpr Permissions PERM_ADMINISTRATOR = 1ull << 3;
constexpr Permissions PERM_MANAGE_CHANNELS = 1ull << 4;
constexpr Permissions PERM_MANAGE_GUILD = 1ull << 5;
constexpr Permissions PERM_ADD_REACTIONS = 1ull << 6;
constexpr Permissions PERM_VIEW_AUDIT_LOG = 1ull << 7;
constexpr Permissions PERM_PRIORITY_SPEAKER = 1ull << 8;
constexpr Permissions PERM_STREAM = 1ull << 9;
constexpr Permissions PERM_VIEW_CHANNEL = 1ull << 10;
constexpr Permissions PERM_SEND_MESSAGES = 1ull << 11;
constexpr Permissions PERM_SEND_TTS_MESSAGES = 1ull << 12;
constexpr Permissions PERM_MANAGE_MESSAGES = 1ull << 13;
constexpr Permissions PERM_EMBED_LINKS = 1ull << 14;
constexpr Permissions PERM_ATTACH_FILES = 1ull << 15;
constexpr Permissions PERM_READ_MESSAGE_HISTORY = 1ull << 16;
constexpr Permissions PERM_MENTION_EVERYONE = 1ull << 17;
constexpr Permissions PERM_USE_EXTERNAL_EMOJIS = 1ull << 18;
constexpr Permissions PERM_VIEW_GUILD_INSIGHTS = 1ull << 19;
constexpr Permissions PERM_CONNECT = 1ull << 20;
constexpr Permissions PERM_SPEAK = 1ull << 21;
constexpr Permissions PERM_MUTE_MEMBERS = 1ull << 22;
constexpr Permissions PERM_DEAFEN_MEMBERS = 1ull << 23;
constexpr Permissions PERM_MOVE_MEMBERS = 1ull << 24;
constexpr Permissions PERM_USE_VAD = 1ull << 25;
constexpr Permissions PERM_CHANGE_NICKNAME = 1ull << 26;
constexpr Permissions PERM_MANAGE_NICKNAMES = 1ull << 27;
constexpr Permissions PERM_MANAGE_ROLES = 1ull << 28;
constexpr Permissions PERM_MANAGE_WEBHOOKS = 1ull << 29;
constexpr Permissions PERM_MANAGE_EXPRESSIONS = 1ull << 30;
constexpr Permissions PERM_USE_APPLICATION_COMMANDS = 1ull << 31;
constexpr Permissions PERM_REQUEST_TO_SPEAK = 1ull << 32;
constexpr Permissions PERM_MANAGE_EVENTS = 1ull << 33;
constexpr Permissions PERM_MANAGE_THREADS = 1ull << 34;
constexpr Permissions PERM_CREATE_PUBLIC_THREADS = 1ull << 35;
constexpr Permissions PERM_CREATE_PRIVATE_THREADS = 1ull << 36;
constexpr Permissions PERM_USE_EXTERNAL_STICKERS = 1ull << 37;
constexpr Permissions PERM_SEND_MESSAGES_IN_THREADS = 1ull << 38;
constexpr Permissions PERM_USE_EMBEDDED_ACTIVITIES = 1ull << 39;
constexpr Permissions PERM_MODERATE_MEMBERS = 1ull << 40;
constexpr Permissions PERM_USE_SOUNDBOARD = 1ull << 42;
constexpr Permissions PERM_CREATE_EXPRESSIONS = 1ull << 43;
constexpr Permissions PERM_CREATE_EVENTS = 1ull << 44;
constexpr Permissions PERM_USE_EXTERNAL_SOUNDS = 1ull << 45;
constexpr Permissions PERM_SEND_VOICE_MESSAGES = 1ull << 46;
constexpr Permissions PERM_SET_VOICE_CHANNEL_STATUS = 1ull << 48;
constexpr Permissions PERM_SEND_POLLS = 1ull << 49;
constexpr Permissions PERM_USE_EXTERNAL_APPS = 1ull << 50;
constexpr Permissions PERM_PIN_MESSAGES = 1ull << 51;
constexpr Permissions PERM_BYPASS_SLOWMODE = 1ull << 52;

constexpr Permissions PERM_ALL = (1ull << 54) - 1;

// what everyone may do in a dm or a group dm, where no role exists.
constexpr Permissions PERM_DM =
  PERM_VIEW_CHANNEL | PERM_SEND_MESSAGES | PERM_SEND_TTS_MESSAGES |
  PERM_EMBED_LINKS | PERM_ATTACH_FILES | PERM_READ_MESSAGE_HISTORY |
  PERM_MENTION_EVERYONE | PERM_USE_EXTERNAL_EMOJIS | PERM_ADD_REACTIONS |
  PERM_CONNECT | PERM_SPEAK | PERM_STREAM | PERM_USE_VAD |
  PERM_USE_EXTERNAL_STICKERS | PERM_SEND_VOICE_MESSAGES | PERM_SEND_POLLS |
  PERM_USE_APPLICATION_COMMANDS | PERM_USE_EXTERNAL_APPS | PERM_PIN_MESSAGES;

typedef enum : uint8_t {
  OVERWRITE_ROLE = 0,
  OVERWRITE_MEMBER = 1,
} OverwriteType;

typedef struct {
  // a role or a user. the role whose id is the guild's is @everyone.
  Snowflake id;
  OverwriteType type;
  Permissions allow, deny;
} Overwrite;

// a member as permissions see one.
typedef struct {
  Snowflake guild_id, owner_id, user_id;
  // what @everyone is granted in the guild.
  Permissions everyone;
  // the member's roles, @everyone left out, and what each grants.
  const Snowflake *role_ids;
  const Permissions *role_permissions;
  size_t nroles;
  // communication disabled until some time still ahead.
  bool timed_out;
} PermissionSubject;

typedef enum : uint8_t {
  PERM_CHANNEL_TEXT,
  PERM_CHANNEL_VOICE,
  // its overwrites are its parent's.
  PERM_CHANNEL_THREAD,
} PermissionChannelKind;

// what the member may do in the guild, before any channel. PERM_ALL for the
// owner and for administrators.
Permissions permissions_base(const PermissionSubject *who);

// what the member may do in a channel with these overwrites, the implicit
// rules applied: nothing without VIEW_CHANNEL, no mentions or files without
// SEND_MESSAGES, nothing of voice without CONNECT, only reading while timed
// out, and SEND_MESSAGES in a thread being SEND_MESSAGES_IN_THREADS.
Permissions permissions_in_channel(const PermissionSubject *who,
                                   const Overwrite *overwrites,
                                   size_t noverwrites,
                                   PermissionChannelKind kind);

#endif
