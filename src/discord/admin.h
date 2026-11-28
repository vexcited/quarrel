#ifndef QUARREL_DISCORD_ADMIN_H
#define QUARREL_DISCORD_ADMIN_H

#include "discord/session.h"
#include "discord/tags.h"

/* ========================================================================== */
/* managing a guild: its settings, roles, channels, bans, invites and emojis, */
/* as the settings of discord's client do. each call answers through the      */
/* store, which the gateway updates as well, and through the AdminOp it names */
/* for what is on its way or why it failed. offline, a call is applied to the */
/* store as discord's events would apply it.                                  */
/* ========================================================================== */

typedef enum : uint8_t {
  ADMIN_IDLE,
  ADMIN_BUSY,
  ADMIN_DONE,
  ADMIN_FAILED,
} AdminStatus;

typedef struct {
  AdminStatus status;
  // why it failed, null otherwise.
  char *error;
  // the id of what it made, 0 until it did.
  Snowflake made;

  // the admin's own, so an older answer is not taken for the last one.
  uint64_t ticket;
} AdminOp;

typedef struct {
  Snowflake user_id;
  // null when none was given.
  char *reason;
} AdminBan;

typedef struct {
  char *code;
  Snowflake channel_id, inviter_id;
  uint32_t uses, max_uses;
  // unix milliseconds it ends at, 0 for never.
  int64_t expires_at;
  bool temporary;
} AdminInvite;

typedef struct {
  Snowflake role_id;
  uint32_t count;
} AdminRoleCount;

// a guild's tag as its profile holds it.
typedef struct {
  // null when the tag is off.
  char *tag;
  uint8_t badge;
  // 0xrrggbb, or TAG_TINT_NONE for the badge's own colors.
  int32_t primary, secondary;
  // the profile is seen from outside the guild, which the tag leads to.
  bool visible;
} AdminTag;

typedef struct Admin {
  // the guild what is below belongs to.
  Snowflake guild_id;

  // saving the guild or a role.
  AdminOp save;
  AdminOp role;
  AdminOp channel;
  // a move of channels or roles, undone when it fails.
  AdminOp move;
  // a ban lifted, an invite revoked, an emoji changed, a member added to a
  // role or taken out of one.
  AdminOp action;
  // the guild deleted or left.
  AdminOp leave;

  // how many members each role has, as discord counts them.
  AdminRoleCount *counts;
  size_t ncounts;
  bool counts_loading;

  // the members of one role, at most 100.
  Snowflake members_role;
  Snowflake *members;
  size_t nmembers;
  bool members_loading;

  AdminBan *bans;
  size_t nbans;
  bool bans_loading, bans_loaded;

  AdminInvite *invites;
  size_t ninvites;
  bool invites_loading, invites_loaded;

  AdminTag tag;
  bool tag_loading, tag_loaded;

  // bumped with every change of the fields above.
  uint64_t version;

  // the admin's own.
  uint64_t epoch, tickets;
  void *requests;
} Admin;

// the session's, made on first use. a guild other than the one it holds
// starts it over.
Admin *admin_of(Session *s, Snowflake guild_id);
void admin_free(Admin *a);
// forgets how an op ended, for a dialog that opens again.
void admin_clear(Admin *a, AdminOp *op);

/* ========================================================================== */
/* the guild.                                                                 */
/* ========================================================================== */

// what a guild edit changes, one flag per field sent.
enum : uint32_t {
  GUILD_EDIT_NAME = 1u << 0,
  GUILD_EDIT_ICON = 1u << 1,
  GUILD_EDIT_BANNER = 1u << 2,
  GUILD_EDIT_SPLASH = 1u << 3,
  GUILD_EDIT_DESCRIPTION = 1u << 4,
  GUILD_EDIT_AFK_CHANNEL = 1u << 5,
  GUILD_EDIT_AFK_TIMEOUT = 1u << 6,
  GUILD_EDIT_SYSTEM_CHANNEL = 1u << 7,
  GUILD_EDIT_SYSTEM_FLAGS = 1u << 8,
  GUILD_EDIT_NOTIFICATIONS = 1u << 9,
  GUILD_EDIT_VERIFICATION = 1u << 10,
  GUILD_EDIT_CONTENT_FILTER = 1u << 11,
  GUILD_EDIT_RULES_CHANNEL = 1u << 12,
  GUILD_EDIT_UPDATES_CHANNEL = 1u << 13,
  GUILD_EDIT_SAFETY_CHANNEL = 1u << 14,
  GUILD_EDIT_LOCALE = 1u << 15,
  GUILD_EDIT_FEATURES = 1u << 16,
  GUILD_EDIT_PROGRESS_BAR = 1u << 17,
};

typedef struct {
  uint32_t fields;
  const char *name, *description, *locale;
  // data uris of the new images, null to take them off.
  const char *icon, *banner, *splash;
  // 0 for none.
  Snowflake afk_channel_id, system_channel_id, rules_channel_id,
    updates_channel_id, safety_channel_id;
  uint32_t afk_timeout, system_flags;
  NotifyLevel notifications;
  VerificationLevel verification;
  ContentFilter content_filter;
  bool progress_bar;
  // the whole list, as discord wants it.
  const char *const *features;
  size_t nfeatures;
} GuildEdit;

// into `save`.
void admin_edit_guild(Session *s, Snowflake guild_id, const GuildEdit *edit);
// deletes the guild, which only its owner may. into `leave`.
void admin_delete_guild(Session *s, Snowflake guild_id);
void admin_leave_guild(Session *s, Snowflake guild_id);

/* ========================================================================== */
/* the tag.                                                                   */
/* ========================================================================== */

// the guild's profile, see `tag`.
void admin_fetch_tag(Session *s, Snowflake guild_id);
// sends the whole tag, a null `tag->tag` taking it off. into `save`.
void admin_save_tag(Session *s, Snowflake guild_id, const AdminTag *tag);

/* ========================================================================== */
/* roles.                                                                     */
/* ========================================================================== */

// a role as discord makes it from the settings, "new role" in no color with
// what @everyone may do. into `role`, its id in `role.made`.
void admin_create_role(Session *s, Snowflake guild_id);

enum : uint32_t {
  ROLE_EDIT_NAME = 1u << 0,
  ROLE_EDIT_COLOR = 1u << 1,
  ROLE_EDIT_HOIST = 1u << 2,
  ROLE_EDIT_MENTIONABLE = 1u << 3,
  ROLE_EDIT_PERMISSIONS = 1u << 4,
  ROLE_EDIT_ICON = 1u << 5,
};

typedef struct {
  uint32_t fields;
  const char *name;
  uint32_t color, secondary_color, tertiary_color;
  bool hoist, mentionable;
  Permissions permissions;
  // a data uri of the icon, or the utf-8 of an emoji, null for neither.
  const char *icon, *unicode_emoji;
} RoleEdit;

// into `save`.
void admin_edit_role(Session *s, Snowflake guild_id, Snowflake role_id,
                     const RoleEdit *edit);
// into `role`.
void admin_delete_role(Session *s, Snowflake guild_id, Snowflake role_id);

// moves a role between others, highest first in `order`, which holds every
// role the user may move. the store moves at once and back when discord
// refuses. into `move`.
void admin_order_roles(Session *s, Snowflake guild_id, const Snowflake *order,
                       size_t count);

// how many members each role has, see `counts`.
void admin_fetch_counts(Session *s, Snowflake guild_id);
uint32_t admin_count(const Admin *a, Snowflake role_id);

// the members of a role, see `members`. their names come through the
// gateway.
void admin_fetch_members(Session *s, Snowflake guild_id, Snowflake role_id);
// gives the role to these members. into `action`.
void admin_add_members(Session *s, Snowflake guild_id, Snowflake role_id,
                       const Snowflake *user_ids, size_t count);
// takes it from one. into `action`.
void admin_remove_member(Session *s, Snowflake guild_id, Snowflake role_id,
                         Snowflake user_id);
// asks for the members whose names start with `query`, which come in the
// store. what a picker of members shows while the user types.
void admin_search_members(Session *s, Snowflake guild_id, const char *query);

/* ========================================================================== */
/* channels.                                                                  */
/* ========================================================================== */

typedef struct {
  const char *name;
  ChannelType type;
  // 0 for none.
  Snowflake parent_id;
  // seen by none but the owner, administrators and what is in `roles` and
  // `users`.
  bool private;
  const Snowflake *roles, *users;
  size_t nroles, nusers;
} ChannelCreate;

// into `channel`, its id in `channel.made`.
void admin_create_channel(Session *s, Snowflake guild_id,
                          const ChannelCreate *create);
// into `channel`.
void admin_delete_channel(Session *s, Snowflake guild_id, Snowflake channel_id);
void admin_rename_channel(Session *s, Snowflake guild_id, Snowflake channel_id,
                          const char *name);

// a channel's place after a move.
typedef struct {
  Snowflake id;
  int32_t position;
  Snowflake parent_id;
  // its permissions become its new category's.
  bool lock_permissions;
  // whether to send `parent_id` and `lock_permissions`, for a channel that
  // changed category.
  bool moved;
} ChannelPlace;

// where channels go, the ones whose place did not change left out. the
// store moves them at once and back when discord refuses. into `move`.
void admin_move_channels(Session *s, Snowflake guild_id,
                         const ChannelPlace *places, size_t count);

// what a channel name becomes for discord: lowercase, spaces as dashes,
// at most 100 bytes. for text channels, the others keep theirs.
void admin_channel_slug(const char *name, char *out, size_t cap);

/* ========================================================================== */
/* moderation.                                                                */
/* ========================================================================== */

void admin_fetch_bans(Session *s, Snowflake guild_id);
// into `action`.
void admin_unban(Session *s, Snowflake guild_id, Snowflake user_id);
void admin_fetch_invites(Session *s, Snowflake guild_id);
// into `action`.
void admin_revoke_invite(Session *s, Snowflake guild_id, const char *code);

/* ========================================================================== */
/* emojis.                                                                    */
/* ========================================================================== */

// `image` is a data uri. into `action`.
void admin_create_emoji(Session *s, Snowflake guild_id, const char *name,
                        const char *image);
void admin_rename_emoji(Session *s, Snowflake guild_id, Snowflake emoji_id,
                        const char *name);
void admin_delete_emoji(Session *s, Snowflake guild_id, Snowflake emoji_id);

// a data uri of the image file at `path`, null when it is no png, jpeg,
// gif or webp, or larger than `most` bytes. from malloc.
char *admin_image_uri(const char *path, size_t most, char **error);

#endif
