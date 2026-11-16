#include "discord/permissions.h"

constexpr Permissions PERM_VOICE_ONLY =
  PERM_SPEAK | PERM_STREAM | PERM_MUTE_MEMBERS | PERM_DEAFEN_MEMBERS |
  PERM_MOVE_MEMBERS | PERM_USE_VAD | PERM_PRIORITY_SPEAKER |
  PERM_REQUEST_TO_SPEAK | PERM_USE_EMBEDDED_ACTIVITIES | PERM_USE_SOUNDBOARD |
  PERM_USE_EXTERNAL_SOUNDS | PERM_SET_VOICE_CHANNEL_STATUS |
  PERM_MANAGE_CHANNELS;

constexpr Permissions PERM_NEEDS_SEND =
  PERM_MENTION_EVERYONE | PERM_SEND_TTS_MESSAGES | PERM_ATTACH_FILES |
  PERM_EMBED_LINKS | PERM_SEND_VOICE_MESSAGES | PERM_SEND_POLLS;

constexpr Permissions PERM_TIMED_OUT =
  PERM_VIEW_CHANNEL | PERM_READ_MESSAGE_HISTORY;

Permissions permissions_base(const PermissionSubject *who) {
  if (who->user_id != 0 && who->user_id == who->owner_id) {
    return PERM_ALL;
  }

  Permissions permissions = who->everyone;

  for (size_t i = 0; i < who->nroles; i++) {
    permissions |= who->role_permissions[i];
  }

  if (permissions & PERM_ADMINISTRATOR) {
    return PERM_ALL;
  }

  return permissions;
}

static bool has_role(const PermissionSubject *who, Snowflake id) {
  for (size_t i = 0; i < who->nroles; i++) {
    if (who->role_ids[i] == id) {
      return true;
    }
  }

  return false;
}

Permissions permissions_in_channel(const PermissionSubject *who,
                                   const Overwrite *overwrites,
                                   size_t noverwrites,
                                   PermissionChannelKind kind) {
  Permissions permissions = permissions_base(who);

  // the owner and administrators bypass overwrites and time outs alike.
  if (permissions == PERM_ALL) {
    return PERM_ALL;
  }

  Permissions role_allow = 0, role_deny = 0;
  Permissions member_allow = 0, member_deny = 0;

  for (size_t i = 0; i < noverwrites; i++) {
    const Overwrite *o = &overwrites[i];

    if (o->type == OVERWRITE_MEMBER) {
      if (o->id == who->user_id) {
        member_allow |= o->allow;
        member_deny |= o->deny;
      }
    }
    else if (o->id == who->guild_id) {
      permissions &= ~o->deny;
      permissions |= o->allow;
    }
  }

  // @everyone is applied before the other roles, wherever it sits in the
  // list, so the roles are gathered in a second pass.
  for (size_t i = 0; i < noverwrites; i++) {
    const Overwrite *o = &overwrites[i];
    bool role = o->type == OVERWRITE_ROLE && o->id != who->guild_id;

    if (role && has_role(who, o->id)) {
      role_allow |= o->allow;
      role_deny |= o->deny;
    }
  }

  permissions &= ~role_deny;
  permissions |= role_allow;
  permissions &= ~member_deny;
  permissions |= member_allow;

  if (who->timed_out) {
    permissions &= PERM_TIMED_OUT;
  }

  if (!(permissions & PERM_VIEW_CHANNEL)) {
    return 0;
  }

  if (kind == PERM_CHANNEL_THREAD) {
    if (permissions & PERM_SEND_MESSAGES_IN_THREADS) {
      permissions |= PERM_SEND_MESSAGES;
    }
    else {
      permissions &= ~PERM_SEND_MESSAGES;
    }
  }

  if (!(permissions & PERM_SEND_MESSAGES)) {
    permissions &= ~PERM_NEEDS_SEND;
  }

  if (kind == PERM_CHANNEL_VOICE && !(permissions & PERM_CONNECT)) {
    permissions &= ~PERM_VOICE_ONLY;
  }

  return permissions;
}
