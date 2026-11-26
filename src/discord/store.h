#ifndef QUARREL_DISCORD_STORE_H
#define QUARREL_DISCORD_STORE_H

#include "discord/permissions.h"
#include "discord/util.h"
#include <cima/json_dsl.h>

/* ========================================================================== */
/* the model the ui reads. everything lives on the main thread, without       */
/* locks, and only changes while an event is applied or a session call runs,  */
/* never during a frame.                                                      */
/*                                                                            */
/* pointers. a User, Guild, Channel, Role, Member or Message stays where it   */
/* is until it is removed, so a pointer is good for the frame that looked it  */
/* up. keep ids across frames, not pointers, and look them up again: a        */
/* lookup is one probe of a map. arrays the store answers with, like          */
/* store_guild_channels, are caches good until the next change.               */
/*                                                                            */
/* strings are utf-8 from malloc, owned by the store, and null when discord   */
/* sent none, a user without an avatar for one. the store_*_name helpers      */
/* never answer null.                                                         */
/*                                                                            */
/* versions. every change bumps the counters of what it touched, so the ui    */
/* memoizes on them:                                                          */
/*   store->version           anything at all                                 */
/*   store->users_version     a user's names, avatar or presence              */
/*   store->guilds_version    the guilds there are, their order and folders   */
/*   store->private_version   the dms there are and their order               */
/*   guild->version           anything in the guild, its channels included    */
/*   guild->members_version   its members and roles, what names and colors    */
/*                            of authors are made of                          */
/*   channel->version         the channel, its messages, read state, typing   */
/*   list->version            a member list                                   */
/* ========================================================================== */

typedef struct Store Store;
typedef struct Guild Guild;
typedef struct Channel Channel;
typedef struct Message Message;

/* ========================================================================== */
/* users and presences.                                                       */
/* ========================================================================== */

typedef enum : uint8_t {
  STATUS_OFFLINE,
  STATUS_ONLINE,
  STATUS_IDLE,
  STATUS_DND,
} UserStatus;

typedef enum : uint8_t {
  ACTIVITY_PLAYING = 0,
  ACTIVITY_STREAMING = 1,
  ACTIVITY_LISTENING = 2,
  ACTIVITY_WATCHING = 3,
  ACTIVITY_CUSTOM = 4,
  ACTIVITY_COMPETING = 5,
} ActivityType;

// something a user is doing, as their presence tells it.
typedef struct {
  ActivityType type;
  char *name, *details, *state;
  Snowflake application_id;
  // `assets.large_image` as discord sends it, see cdn_activity_image.
  char *large_image;
  // which field stands for the activity in a status line: the name for
  // 0, the state for 1, the details for 2.
  uint8_t display;
  // a song played through spotify, its state holding the artists.
  bool spotify;
  // unix milliseconds it started at, 0 when the activity does not say,
  // and when the presence first carried it.
  int64_t started_ms, created_ms;
} Activity;

// what stands for the activity in one line, like under a friend's name:
// a field of the activity, or `out` when the text had to be made. null
// when the activity has nothing to show.
const char *activity_status_text(const Activity *a, char *out, size_t cap);

// where a user is connected to voice.
typedef struct {
  Snowflake user_id;
  // 0 for a call in a private channel.
  Snowflake guild_id, channel_id;
  // sharing their screen.
  bool streaming;
  // by their own doing or by the guild's.
  bool muted, deafened;
} VoiceState;

// a call in a private channel, there from its first member joining until
// its last one left.
typedef struct {
  Snowflake channel_id;
  // who is being rung to join it, in no order.
  Snowflake *ringing;
  size_t nringing;
  // who rings the account, 0 when nobody does.
  Snowflake ringer_id;
} PrivateCall;

typedef struct {
  Snowflake id;
  char *username;
  // the display name the user chose, null when none.
  char *global_name;
  // null or "0" for a unique username, four digits for bots and old accounts.
  char *discriminator;
  char *avatar, *banner;
  // the asset of what frames the avatar, null for nothing.
  char *decoration;
  // what the profile says. discord sends these for the current user only.
  char *bio, *pronouns;
  // 0xrrggbb when `has_accent`, the banner's color when there is no image.
  uint32_t accent_color;
  bool has_accent;
  uint64_t public_flags;
  bool bot, system;
  // the nitro the user pays for, 0 for none. discord sends it for the
  // current user only.
  uint8_t premium_type;

  // offline until a presence says otherwise.
  UserStatus status;
  bool on_desktop, on_mobile, on_web;
  // what is shown under the name: the custom status, or "Playing a game".
  char *activity;
  // whether `activity` is the status the user wrote themselves.
  bool custom_status;
  // the status the user wrote, in its parts: the words, and the emoji
  // before them by its name, with its id when it is a guild's. null and 0
  // for what is missing.
  char *status_text, *status_emoji;
  Snowflake status_emoji_id;
  // what the user is doing, their custom status left out.
  Activity *activities;
  size_t nactivities;
  // the tag of the guild the user wears, null when none, with the guild
  // and the hash of its badge.
  char *tag, *tag_badge;
  Snowflake tag_guild_id;
} User;

typedef enum : uint8_t {
  RELATIONSHIP_NONE = 0,
  RELATIONSHIP_FRIEND = 1,
  RELATIONSHIP_BLOCKED = 2,
  RELATIONSHIP_INCOMING = 3,
  RELATIONSHIP_OUTGOING = 4,
  RELATIONSHIP_IMPLICIT = 5,
} RelationshipType;

typedef struct {
  Snowflake user_id;
  RelationshipType type;
  // the name the current user gave this friend, null when none.
  char *nickname;
  // the current user ignores them, which is not a type of its own.
  bool ignored;
} Relationship;

/* ========================================================================== */
/* guilds.                                                                    */
/* ========================================================================== */

typedef struct {
  Snowflake id;
  char *name;
  // 0xrrggbb, 0 for no color.
  uint32_t color;
  // the other ends of a gradient, 0 for a plain color. a holographic role
  // has a tertiary color.
  uint32_t secondary_color, tertiary_color;
  int32_t position;
  Permissions permissions;
  // shown apart in the member list.
  bool hoist, mentionable, managed;
  char *icon, *unicode_emoji;
  // the role a bot, a boost or a subscription comes with, which nobody
  // hands out. 0 for none.
  Snowflake bot_id, integration_id;
  bool premium_subscriber;
} Role;

typedef struct {
  Snowflake user_id;
  char *nick;
  // the avatar set for this guild, null for the user's own.
  char *avatar;
  // the same for what frames it.
  char *decoration;
  Snowflake *roles;
  size_t nroles;
  // unix milliseconds, 0 when unknown.
  int64_t joined_at;
  // unix milliseconds the member is timed out until, 0 when not.
  int64_t timeout_until;
  bool pending;

  // the store's own.
  uint32_t color;
  uint64_t color_stamp;
} Member;

typedef struct {
  Snowflake id;
  char *name;
  bool animated, available;
} Emoji;

typedef enum : uint8_t {
  NOTIFY_ALL_MESSAGES = 0,
  NOTIFY_ONLY_MENTIONS = 1,
  NOTIFY_NOTHING = 2,
  // a channel following its guild.
  NOTIFY_INHERIT = 3,
} NotifyLevel;

// the account's notification_settings flags.
constexpr uint32_t NOTIFY_FLAG_NEW_NOTIFICATIONS = 1u << 4;
constexpr uint32_t NOTIFY_FLAG_MENTION_ON_ALL_MESSAGES = 1u << 5;

// the flags of a guild's settings.
constexpr uint32_t GUILD_SETTINGS_UNREADS_ALL_MESSAGES = 1u << 11;
constexpr uint32_t GUILD_SETTINGS_UNREADS_ONLY_MENTIONS = 1u << 12;
constexpr uint32_t GUILD_SETTINGS_OPT_IN_CHANNELS_ON = 1u << 14;

// the flags of a channel's override in them.
constexpr uint32_t OVERRIDE_UNREADS_ONLY_MENTIONS = 1u << 9;
constexpr uint32_t OVERRIDE_UNREADS_ALL_MESSAGES = 1u << 10;
constexpr uint32_t OVERRIDE_OPT_IN_ENABLED = 1u << 12;

// the flags of the current user's membership of a thread, which say what
// of it they are told.
constexpr uint32_t THREAD_NOTIFY_ALL_MESSAGES = 1u << 1;
constexpr uint32_t THREAD_NOTIFY_ONLY_MENTIONS = 1u << 2;
constexpr uint32_t THREAD_NOTIFY_NOTHING = 1u << 3;

// a channel listed in the guild's resources rather than with the others.
constexpr uint32_t CHANNEL_FLAG_RESOURCE = 1u << 7;

typedef struct {
  // messages wait and the user's settings want them shown.
  bool unread;
  // messages newer than what was acknowledged, whatever the settings say.
  bool pending;
  // the channel, its category or its guild is muted: `unread` should not be
  // shown, `mentions` still are.
  bool muted;
  uint32_t mentions;
} Unread;

typedef enum : uint8_t {
  MEMBER_ROW_UNLOADED,
  MEMBER_ROW_GROUP,
  MEMBER_ROW_MEMBER,
} MemberRowKind;

typedef enum : uint8_t {
  MEMBER_GROUP_ROLE,
  MEMBER_GROUP_ONLINE,
  MEMBER_GROUP_OFFLINE,
} MemberGroupKind;

typedef struct {
  MemberRowKind kind;
  // for a group row.
  MemberGroupKind group;
  // the user of a member row, the role of a group row of MEMBER_GROUP_ROLE.
  Snowflake id;
  // the members of a group row.
  uint32_t count;
} MemberRow;

// the member list of the channels sharing the same viewers, as the gateway
// keeps it in sync for the ranges the session subscribed to.
typedef struct {
  // "everyone" or a hash of the overwrites.
  char *id;
  Snowflake guild_id;
  uint32_t member_count, online_count;
  // every row of the list, the ones outside the subscribed ranges being
  // MEMBER_ROW_UNLOADED.
  MemberRow *rows;
  size_t nrows;
  uint64_t version;

  // the store's own.
  size_t rows_cap;
  uint64_t touched;
} MemberList;

typedef enum : uint8_t {
  CHANNEL_ROW_CATEGORY,
  CHANNEL_ROW_CHANNEL,
  CHANNEL_ROW_THREAD,
} ChannelRowKind;

typedef struct {
  ChannelRowKind kind;
  Channel *channel;
} ChannelRow;

typedef enum : uint8_t {
  VERIFICATION_NONE = 0,
  VERIFICATION_LOW = 1,
  VERIFICATION_MEDIUM = 2,
  VERIFICATION_HIGH = 3,
  VERIFICATION_VERY_HIGH = 4,
} VerificationLevel;

typedef enum : uint8_t {
  CONTENT_FILTER_DISABLED = 0,
  CONTENT_FILTER_MEMBERS_WITHOUT_ROLES = 1,
  CONTENT_FILTER_ALL_MEMBERS = 2,
} ContentFilter;

// what the system channel does not say, the flags being the ones that
// silence it.
constexpr uint32_t SYSTEM_SUPPRESS_JOIN = 1u << 0;
constexpr uint32_t SYSTEM_SUPPRESS_BOOSTS = 1u << 1;
constexpr uint32_t SYSTEM_SUPPRESS_SETUP_TIPS = 1u << 2;
constexpr uint32_t SYSTEM_SUPPRESS_JOIN_REPLIES = 1u << 3;
constexpr uint32_t SYSTEM_SUPPRESS_ROLE_SUBSCRIPTION = 1u << 4;
constexpr uint32_t SYSTEM_SUPPRESS_ROLE_SUBSCRIPTION_REPLIES = 1u << 5;

struct Guild {
  Snowflake id;
  char *name, *icon, *banner, *splash, *description;
  Snowflake owner_id, system_channel_id, rules_channel_id;
  // where members go once idle in voice for `afk_timeout` seconds, 0 for
  // nowhere.
  Snowflake afk_channel_id;
  uint32_t afk_timeout;
  // a community's: where discord posts what moderators should know, and
  // its safety alerts.
  Snowflake public_updates_channel_id, safety_alerts_channel_id;
  // SYSTEM_SUPPRESS_ flags.
  uint32_t system_channel_flags;
  // the level new members get unless they set their own, which `community`
  // wants at NOTIFY_ONLY_MENTIONS.
  NotifyLevel default_notifications;
  VerificationLevel verification_level;
  ContentFilter content_filter;
  // moderators need two factors to moderate.
  bool mfa_required;
  bool premium_progress_bar;
  // like "en-US".
  char *preferred_locale;
  // the guild's features as discord names them, like "COMMUNITY".
  char **features;
  size_t nfeatures;
  char *vanity_code;
  uint32_t member_count;
  uint8_t premium_tier;
  uint32_t premium_subscription_count;
  // the largest file its features allow uploading in bytes, 0 when none
  // raises the limit.
  uint64_t upload_feature;
  // an outage: nothing but the id is known.
  bool unavailable;
  bool large;
  // unix milliseconds the current user joined at.
  int64_t joined_at;

  // highest first.
  Role **roles;
  size_t nroles;
  Emoji *emojis;
  size_t nemojis;
  // every channel of the guild and the threads known in it, in no order. see
  // store_guild_channels for the list to show.
  Channel **channels;
  size_t nchannels;

  // the current user's notification settings for the guild.
  bool muted;
  // unix milliseconds the mute ends at, 0 for never.
  int64_t mute_until;
  NotifyLevel notifications;
  bool suppress_everyone, suppress_roles;
  // GUILD_SETTINGS_ flags.
  uint32_t settings_flags;
  // the user has settings of their own for the guild, `notifications` is
  // the guild's default until then.
  bool has_settings;
  // the guild has the COMMUNITY feature.
  bool community;
  // the guild is a student hub, with HUB, and lists servers in its
  // directory channels.
  bool hub;
  // a HubType, what kind of school the hub is for.
  uint8_t hub_type;

  uint64_t version, members_version;

  // the store's own.
  CILO_MAP(Snowflake, Member *) members;
  MemberList **lists;
  size_t nlists;
  ChannelRow *rows;
  size_t nrows;
  // bumped by what the channel list and permissions are made of.
  uint64_t rows_stamp, layout_version;
  Unread unread;
  uint64_t unread_stamp;
};

// a folder of the guild bar, or one guild on its own when `id` is 0 and it
// holds a single guild.
typedef struct {
  Snowflake id;
  char *name;
  // 0xrrggbb when `has_color`.
  uint32_t color;
  bool has_color;
  Guild **guilds;
  size_t nguilds;
} GuildFolder;

/* ========================================================================== */
/* channels.                                                                  */
/* ========================================================================== */

// unknown types are kept and shown as text channels when they can be.
typedef enum : uint8_t {
  CHANNEL_GUILD_TEXT = 0,
  CHANNEL_DM = 1,
  CHANNEL_GUILD_VOICE = 2,
  CHANNEL_GROUP_DM = 3,
  CHANNEL_GUILD_CATEGORY = 4,
  CHANNEL_GUILD_ANNOUNCEMENT = 5,
  CHANNEL_ANNOUNCEMENT_THREAD = 10,
  CHANNEL_PUBLIC_THREAD = 11,
  CHANNEL_PRIVATE_THREAD = 12,
  CHANNEL_GUILD_STAGE = 13,
  CHANNEL_GUILD_DIRECTORY = 14,
  CHANNEL_GUILD_FORUM = 15,
  CHANNEL_GUILD_MEDIA = 16,
} ChannelType;

// pinned to the top of its forum, for a post.
constexpr uint32_t CHANNEL_FLAG_PINNED = 1u << 1;
// a forum whose posts need a tag.
constexpr uint32_t CHANNEL_FLAG_REQUIRE_TAG = 1u << 4;

// a tag of a forum, that its posts wear.
typedef struct {
  Snowflake id;
  char *name;
  // the emoji before the name: a guild's by its id, else the utf-8 of
  // `emoji_name`. 0 and null for none.
  Snowflake emoji_id;
  char *emoji_name;
  // only who manages threads may put it on a post.
  bool moderated;
} ForumTag;

typedef enum : uint8_t {
  FORUM_SORT_ACTIVITY = 0,
  FORUM_SORT_CREATION = 1,
} ForumSort;

typedef enum : uint8_t {
  FORUM_LAYOUT_DEFAULT = 0,
  FORUM_LAYOUT_LIST = 1,
  FORUM_LAYOUT_GALLERY = 2,
} ForumLayout;

typedef struct {
  Snowflake user_id;
  // unix seconds the indicator goes away at.
  double until;
} Typing;

typedef struct {
  // false when discord never sent a read state for the channel.
  bool known;
  Snowflake last_acked_id;
  uint32_t mention_count;
  uint32_t flags;
} ReadState;

struct Channel {
  Snowflake id;
  // 0 for dms and group dms.
  Snowflake guild_id;
  // the category of a channel, the channel of a thread, 0 for none.
  Snowflake parent_id;
  // a ChannelType, or a number this build does not know.
  uint8_t type;
  char *name, *topic;
  int32_t position;
  uint32_t flags;
  bool nsfw;
  uint32_t rate_limit_per_user;
  Overwrite *overwrites;
  size_t noverwrites;
  Snowflake last_message_id;

  // dms and group dms: the other users, the current one left out.
  Snowflake *recipients;
  size_t nrecipients;
  // the owner of a group dm, the creator of a thread.
  Snowflake owner_id;
  // a group dm's icon.
  char *icon;

  // threads.
  bool thread_archived, thread_locked;
  // the current user is a member of the thread.
  bool thread_joined;
  // THREAD_NOTIFY_ flags of that membership.
  uint32_t thread_flags;
  // unix milliseconds the current user joined the thread at and the thread
  // was archived at, 0 when unknown.
  int64_t thread_joined_at, thread_archived_at;
  uint32_t message_count, member_count;
  // unix milliseconds a thread was made at, 0 when discord did not say.
  int64_t thread_created_at;

  // forums and media channels: the tags their posts may wear, the emoji
  // their posts are reacted with from the list, 0 and null for none, and
  // how the user sees them until they choose.
  ForumTag *tags;
  size_t ntags;
  Snowflake default_reaction_id;
  char *default_reaction_name;
  ForumSort default_sort;
  ForumLayout default_layout;
  // a filter of several tags wants posts wearing all of them.
  bool tags_match_all;
  // what a new post's message starts with, null for nothing.
  char *template;
  // a post: the tags it wears, and its first message, whose id is the
  // post's. `starter_known` once discord said, `starter` staying null when
  // the message was deleted.
  Snowflake *applied_tags;
  size_t napplied_tags;
  Message *starter;
  bool starter_known;
  // the messages of a post the user opened before that came since, as
  // FORUM_UNREADS counts them. `new_known` once it said.
  uint32_t new_messages;
  bool new_known;

  // a dm waiting as a message request, or one discord holds for spam.
  bool request;

  // the current user's notification settings for the channel.
  bool muted;
  int64_t mute_until;
  NotifyLevel notifications;
  // OVERRIDE_ flags.
  uint32_t override_flags;
  // a category the user folded.
  bool collapsed;

  // oldest first. pending and failed messages sit at the end, where they
  // were typed.
  Message **messages;
  size_t nmessages;
  // the latest messages were fetched once, see session_select_channel.
  bool history_loaded;
  // messages older than the first one loaded exist.
  bool has_older;
  // a fetch of history is running.
  bool loading;

  Typing *typing;
  size_t ntyping;

  uint64_t version;

  // the store's own.
  size_t messages_cap;
  Permissions perms;
  uint64_t perms_stamp;
  Channel **posts;
  size_t nposts;
  uint64_t posts_stamp;
};

/* ========================================================================== */
/* directories: the channels of a hub that list servers and events, which     */
/* the store holds apart from the channel itself.                             */
/* ========================================================================== */

typedef enum : uint8_t {
  DIRECTORY_ENTRY_GUILD = 0,
  DIRECTORY_ENTRY_EVENT = 1,
} DirectoryEntryType;

// discord no longer offers DIRECTORY_SUBJECT_MAJOR.
typedef enum : uint8_t {
  DIRECTORY_UNCATEGORIZED = 0,
  DIRECTORY_SCHOOL_CLUB = 1,
  DIRECTORY_CLASS = 2,
  DIRECTORY_STUDY_SOCIAL = 3,
  DIRECTORY_SUBJECT_MAJOR = 4,
  DIRECTORY_MISC = 5,
} DirectoryCategory;

constexpr size_t DIRECTORY_CATEGORIES = 6;

typedef struct {
  // the guild or the event listed.
  Snowflake entity_id;
  DirectoryEntryType type;
  DirectoryCategory category;
  // what whoever listed it wrote, null for nothing.
  char *description;
  Snowflake author_id;
  // unix milliseconds.
  int64_t created_at;

  // the guild listed, or the one of the event.
  Snowflake guild_id;
  char *name, *icon, *splash, *banner;
  uint32_t member_count, presence_count;
  bool featurable;

  // an event: what it is called, its cover, when it starts and ends in
  // unix milliseconds, 0 for no end, who is interested and whether the
  // current user is.
  char *event_name, *event_description, *event_image;
  int64_t starts_at, ends_at;
  uint32_t user_count;
  bool rsvp;
} DirectoryEntry;

typedef struct {
  Snowflake channel_id;
  // newest first.
  DirectoryEntry **entries;
  size_t nentries;
  // the entries of each DirectoryCategory, once discord counted them.
  uint32_t counts[DIRECTORY_CATEGORIES];
  bool counted;
  // every entry was fetched once, and a fetch is running.
  bool loaded, loading;
  uint64_t version;

  // the store's own.
  size_t entries_cap;
} Directory;

/* ========================================================================== */
/* messages.                                                                  */
/* ========================================================================== */

// the types drawn as a line of their own rather than as what someone said.
// see message_is_system.
typedef enum : uint8_t {
  MESSAGE_DEFAULT = 0,
  MESSAGE_RECIPIENT_ADD = 1,
  MESSAGE_RECIPIENT_REMOVE = 2,
  MESSAGE_CALL = 3,
  MESSAGE_CHANNEL_NAME_CHANGE = 4,
  MESSAGE_CHANNEL_ICON_CHANGE = 5,
  MESSAGE_CHANNEL_PINNED_MESSAGE = 6,
  MESSAGE_USER_JOIN = 7,
  MESSAGE_GUILD_BOOST = 8,
  MESSAGE_GUILD_BOOST_TIER_1 = 9,
  MESSAGE_GUILD_BOOST_TIER_2 = 10,
  MESSAGE_GUILD_BOOST_TIER_3 = 11,
  MESSAGE_CHANNEL_FOLLOW_ADD = 12,
  MESSAGE_THREAD_CREATED = 18,
  MESSAGE_REPLY = 19,
  MESSAGE_CHAT_INPUT_COMMAND = 20,
  MESSAGE_THREAD_STARTER_MESSAGE = 21,
  MESSAGE_CONTEXT_MENU_COMMAND = 23,
  MESSAGE_AUTO_MODERATION_ACTION = 24,
  MESSAGE_POLL_RESULT = 46,
} MessageType;

typedef enum : uint8_t {
  MESSAGE_SENT,
  // typed here and on its way, matched to discord's by its nonce.
  MESSAGE_PENDING,
  // discord refused it or never answered. `error` may say why.
  MESSAGE_FAILED,
} MessageState;

constexpr uint32_t MESSAGE_FLAG_SUPPRESS_EMBEDS = 1u << 2;
constexpr uint32_t MESSAGE_FLAG_EPHEMERAL = 1u << 6;
constexpr uint32_t MESSAGE_FLAG_LOADING = 1u << 7;
constexpr uint32_t MESSAGE_FLAG_SUPPRESS_NOTIFICATIONS = 1u << 12;
constexpr uint32_t MESSAGE_FLAG_VOICE_MESSAGE = 1u << 13;

typedef struct {
  Snowflake id;
  char *filename, *description, *content_type;
  // `url` is the file, `proxy_url` what cdn_media resizes.
  char *url, *proxy_url;
  uint64_t size;
  // 0 for what is no image or video.
  uint32_t width, height;
  // the file is named SPOILER_ or flagged as one.
  bool spoiler;
  // an image whose frames move, a gif or an animated webp or png.
  bool animated;
  // the picture of another attachment, which discord's client never shows.
  bool thumbnail;
  // how long a video or a voice message lasts in seconds, 0 when unknown.
  double duration;
} Attachment;

typedef struct {
  char *url, *proxy_url;
  uint32_t width, height;
  // an image whose frames move.
  bool animated;
} EmbedMedia;

typedef struct {
  char *name, *value;
  // drawn next to its neighbours rather than under them.
  bool is_inline;
} EmbedField;

typedef struct {
  // "rich", "image", "video", "gifv", "article", "link" and so on.
  char *type;
  char *title, *description, *url;
  // 0xrrggbb when `has_color`.
  uint32_t color;
  bool has_color;
  // unix milliseconds, 0 for none.
  int64_t timestamp;
  char *author_name, *author_url, *author_icon;
  char *footer_text, *footer_icon;
  char *provider_name, *provider_url;
  EmbedMedia image, thumbnail, video;
  EmbedField *fields;
  size_t nfields;
} Embed;

typedef struct {
  // 0 for a unicode emoji, whose utf-8 is `name`.
  Snowflake emoji_id;
  char *name;
  bool animated;
  uint32_t count;
  // the current user reacted with it.
  bool me;
} Reaction;

typedef struct {
  Snowflake id;
  char *name;
  // a StickerFormat of cdn.h.
  uint8_t format;
} Sticker;

struct Message {
  Snowflake id, channel_id;
  // look the author up with store_user, or use store_author.
  Snowflake author_id;
  // not 0 for a webhook's message, whose author is no real user.
  Snowflake webhook_id;
  // a MessageType, or a number this build does not know.
  uint8_t type;
  uint32_t flags;
  MessageState state;
  // never null, "" when empty.
  char *content;
  // unix milliseconds. `edited_at` is 0 when never edited.
  int64_t timestamp, edited_at;
  bool pinned, tts, mention_everyone;
  // the current user is mentioned, by name, by role or by @everyone.
  bool mentions_me;

  Snowflake *mentions;
  size_t nmentions;
  Snowflake *mention_roles;
  size_t nmention_roles;

  Attachment *attachments;
  size_t nattachments;
  Embed *embeds;
  size_t nembeds;
  Reaction *reactions;
  size_t nreactions;
  Sticker *stickers;
  size_t nstickers;

  // what the message answers or forwards, 0 for none.
  Snowflake reference_id, reference_channel_id;
  // the reference is a forward, `referenced` holding what was forwarded.
  bool forwarded;
  // a copy of the message answered or forwarded as it was when this one was
  // sent, null when discord sent none or when it was deleted.
  Message *referenced;

  // the command this message answers, null for none, and who ran it.
  char *interaction_name;
  Snowflake interaction_user_id;

  // pending and failed messages: the nonce they were sent with, and why
  // sending failed.
  Snowflake nonce;
  char *error;
};

static inline bool message_is_system(const Message *m) {
  return m->type != MESSAGE_DEFAULT && m->type != MESSAGE_REPLY &&
         m->type != MESSAGE_CHAT_INPUT_COMMAND &&
         m->type != MESSAGE_CONTEXT_MENU_COMMAND;
}

/* ========================================================================== */
/* bookmarks, the saved messages without a reminder.                          */
/* ========================================================================== */

// a bookmark the inbox lists.
typedef struct {
  Snowflake channel_id, message_id;
  // 0 outside a guild.
  Snowflake guild_id;
  // unix milliseconds.
  int64_t saved_at;
  // the message as discord sent it with the bookmark, kept in sync since.
  // null once it was deleted, when discord sent none, or for a bookmark
  // made offline. see store_bookmark_message.
  Message *message;
} Bookmark;

/* ========================================================================== */
/* the store.                                                                 */
/* ========================================================================== */

typedef struct {
  Snowflake id;
  char *name;
  uint32_t color;
  bool has_color;
  Snowflake *guild_ids;
  size_t nguilds;
} StoredFolder;

struct Store {
  // the current user, 0 and null before READY.
  Snowflake self_id;
  User *self;
  // the status the user picked in their settings: what to show for them and
  // what to tell the gateway.
  UserStatus self_status;
  // unix milliseconds `self_status` lasts until, 0 for no end.
  int64_t self_status_until;
  // the StatusSettings of the account in base64 as discord last sent it,
  // null before. see settings_proto_with_status.
  char *status_proto;
  // NOTIFY_FLAG_, the account's notification_settings flags.
  uint32_t notify_flags;
  // true once READY was applied.
  bool ready;

  uint64_t version, users_version, guilds_version, private_version;

  // friends, blocked users and requests, in no order.
  Relationship *relationships;
  size_t nrelationships;

  // the users discord thinks the current one is close to, closest first.
  Snowflake *affinities;
  size_t naffinities;

  // everyone known to be in a voice channel, in no order.
  VoiceState *voice_states;
  size_t nvoice_states;
  // the calls of the private channels, in no order.
  PrivateCall *calls;
  size_t ncalls;

  // the messages the user bookmarked, by id, as discord last listed them.
  // `bookmarks_known` once it did since READY.
  Snowflake *bookmark_ids;
  size_t nbookmark_ids;
  bool bookmarks_known;
  // the bookmarks fetched for the inbox, the latest saved first. some of
  // `bookmark_ids` are not here until their page was fetched.
  Bookmark *bookmarks;
  size_t nbookmarks;
  // bumped with every change of the bookmarks or of their messages.
  uint64_t bookmarks_version;

  // the session id and the url to resume at, as READY gave them.
  char *session_id, *resume_gateway_url;
  // the token discord wants used from now on, when READY carried one.
  char *refreshed_token;

  // the store's own.
  CILO_MAP(Snowflake, User *) users;
  CILO_MAP(Snowflake, Guild *) guilds;
  CILO_MAP(Snowflake, Channel *) channels;
  CILO_MAP(Snowflake, ReadState) read_states;
  CILO_MAP(Snowflake, Directory *) directories;
  // the guilds in the order READY gave them, new ones first.
  Snowflake *guild_order;
  size_t nguild_order;
  StoredFolder *stored_folders;
  size_t nstored_folders;
  bool has_stored_folders;
  GuildFolder *folders;
  size_t nfolders;
  uint64_t folders_stamp;
  Channel **privates;
  size_t nprivates;
  uint64_t privates_stamp;
  Unread private_unread;
  uint64_t private_unread_stamp;
  uint64_t list_clock;
  // unix seconds standing for now when not 0, for tests.
  double clock;
};

[[nodiscard]] Store *store_new(void);
void store_free(Store *s);
// forgets everything, as after a logout. versions keep counting.
void store_reset(Store *s);
// unix seconds, the system's clock unless `clock` is set.
double store_now(const Store *s);

/* ========================================================================== */
/* reading.                                                                   */
/* ========================================================================== */

// null when unknown.
[[gnu::pure]] User *store_user(const Store *s, Snowflake id);
[[gnu::pure]] Guild *store_guild(const Store *s, Snowflake id);
[[gnu::pure]] Channel *store_channel(const Store *s, Snowflake id);
[[gnu::pure]] Role *store_role(const Guild *g, Snowflake id);
[[gnu::pure]] Member *store_member(const Guild *g, Snowflake user_id);
[[gnu::pure]] Emoji *store_emoji(const Guild *g, Snowflake id);
[[gnu::pure]] Message *store_message(const Channel *c, Snowflake id);
[[gnu::pure]] const Relationship *store_relationship(const Store *s,
                                                     Snowflake user_id);
// null when the user is in no voice channel the store heard of.
[[gnu::pure]] const VoiceState *store_voice_state(const Store *s,
                                                  Snowflake user_id);
// null when the private channel has no call the store heard of.
[[gnu::pure]] const PrivateCall *store_call(const Store *s,
                                            Snowflake channel_id);
// how many are in the voice channel or in the private channel's call.
[[gnu::pure]] size_t store_voice_count(const Store *s, Snowflake channel_id);

// whether the call rings for the account to answer. learned from the web
// client: one who is not to be disturbed is rung in silence, with nothing
// shown.
static inline bool call_rings(const Store *s, const PrivateCall *call) {
  return call->ringer_id != 0 && s->self_status != STATUS_DND;
}

// the name to show for a user: their display name, else their username.
// "Unknown User" for null.
[[gnu::pure]] const char *store_user_name(const User *u);
// the same in a guild, where a nickname comes first. `guild_id` may be 0.
[[gnu::pure]] const char *store_display_name(const Store *s, Snowflake guild_id,
                                             Snowflake user_id);
// 0xrrggbb of the member's highest role that has a color, 0 for none.
uint32_t store_member_color(const Store *s, Snowflake guild_id,
                            Snowflake user_id);

// who wrote a message as it should be shown.
typedef struct {
  // null for an author the store never saw.
  const User *user;
  // null outside a guild or for a member not loaded yet.
  const Member *member;
  // never null.
  const char *name;
  // 0 for none.
  uint32_t color;
} Author;

Author store_author(const Store *s, const Message *m);

// a channel's name as shown: its own, the other user's for a dm, the
// recipients' for a group dm without one. written to `out`, never empty.
const char *store_channel_name(const Store *s, const Channel *c, char *out,
                               size_t cap);

static inline bool channel_is_thread(const Channel *c) {
  return c->type == CHANNEL_ANNOUNCEMENT_THREAD ||
         c->type == CHANNEL_PUBLIC_THREAD || c->type == CHANNEL_PRIVATE_THREAD;
}

static inline bool channel_is_voice(const Channel *c) {
  return c->type == CHANNEL_GUILD_VOICE || c->type == CHANNEL_GUILD_STAGE;
}

static inline bool channel_is_private(const Channel *c) {
  return c->type == CHANNEL_DM || c->type == CHANNEL_GROUP_DM;
}

// a channel that holds messages to read.
static inline bool channel_has_text(const Channel *c) {
  return c->type != CHANNEL_GUILD_CATEGORY &&
         c->type != CHANNEL_GUILD_DIRECTORY && c->type != CHANNEL_GUILD_FORUM &&
         c->type != CHANNEL_GUILD_MEDIA;
}

// a channel of posts rather than messages.
static inline bool channel_is_forum(const Channel *c) {
  return c->type == CHANNEL_GUILD_FORUM || c->type == CHANNEL_GUILD_MEDIA;
}

// a tag of the forum by its id, null when it has none such.
[[gnu::pure]] const ForumTag *store_forum_tag(const Channel *forum,
                                              Snowflake id);
// a thread of a forum, a post.
[[gnu::pure]] bool store_is_post(const Store *s, const Channel *c);

// the posts of a forum the store knows of, in no order. a cache good until
// the next change.
Channel *const *store_forum_posts(Store *s, const Channel *forum,
                                  size_t *count);

// the guild bar: folders and lone guilds in the user's order, guilds they
// never placed first. the order READY gave when discord sent no folders.
//
//   size_t n;
//   const GuildFolder *folders = store_guild_folders(store, &n);
//   for (size_t i = 0; i < n; i++)
//     for (size_t j = 0; j < folders[i].nguilds; j++) ...
const GuildFolder *store_guild_folders(Store *s, size_t *count);

// a guild's channels as discord lists them and as the current user may view
// them: the channels without a category, then each category followed by its
// channels, text before voice, by position. the threads the user joined
// follow their channel. categories left empty are dropped unless the user
// manages channels.
//
//   size_t n;
//   const ChannelRow *rows = store_guild_channels(store, guild, &n);
const ChannelRow *store_guild_channels(Store *s, Guild *g, size_t *count);

// whether the guild has a feature, like "COMMUNITY" or "BANNER".
bool store_guild_has(const Guild *g, const char *feature);
// the position of the current user's highest role, which only roles under
// it may be managed from. INT32_MAX for the owner.
int32_t store_highest_position(Store *s, const Guild *g);

// dms and group dms, the latest message first.
Channel *const *store_private_channels(Store *s, size_t *count);

// what the current user may do in a channel. PERM_DM for dms.
Permissions store_permissions(Store *s, const Channel *c);

static inline bool store_can(Store *s, const Channel *c, Permissions p) {
  return (store_permissions(s, c) & p) == p;
}

// read states, after the rules of discord's client: a channel is unread
// when its last message is newer than the last one acknowledged, and the
// user's settings for it want unread messages shown.
Unread store_channel_unread(Store *s, const Channel *c);
// every channel of the guild the user may view, muted ones left out of
// `unread` but not of `mentions`.
Unread store_guild_unread(Store *s, Guild *g);
// every dm and group dm.
Unread store_private_unread(Store *s);
// whether a message that just came is worth telling the user of, after the
// rules of discord's client: who wrote it, the user's status, and what
// their settings ask of its channel, its category, its guild or its thread.
// whether its channel is the one looked at is the ui's to weigh.
bool store_notifies(Store *s, const Message *m);
// the last message acknowledged in the channel, 0 when unknown. the unread
// divider goes after it.
[[gnu::pure]] Snowflake store_last_acked(const Store *s, Snowflake channel_id);

// whether the user bookmarked the message.
[[gnu::pure]] bool store_bookmarked(const Store *s, Snowflake message_id);
// the message a bookmark is for: its own copy, else the one loaded in its
// channel. null when neither is there, as for a deleted message.
[[gnu::pure]] const Message *store_bookmark_message(const Store *s,
                                                    const Bookmark *b);

// the users typing in a channel but the current one, into `out`. returns how
// many there are, which may be more than `cap`.
size_t store_typing(const Store *s, const Channel *c, Snowflake *out,
                    size_t cap);

// the member list of a channel, null until the gateway synced it. see
// session_select_channel and session_member_list_range.
//
//   const MemberList *list = store_member_list(store, channel);
//   for (size_t i = 0; list && i < list->nrows; i++) {
//     const MemberRow *row = &list->rows[i];
//     if (row->kind == MEMBER_ROW_GROUP) ...   a role, online or offline
//     if (row->kind == MEMBER_ROW_MEMBER) ...  store_user(store, row->id)
//   }
const MemberList *store_member_list(Store *s, const Channel *c);
// the id discord gives the member list of a channel.
void store_member_list_id(const Store *s, const Channel *c, char *out,
                          size_t cap);

// what the current user may do in the guild, before any channel. PERM_ALL
// for its owner and its administrators.
Permissions store_guild_permissions(Store *s, const Guild *g);

// the listing of a directory channel, null until something of it was
// fetched or announced.
Directory *store_directory(const Store *s, Snowflake channel_id);
// an entry of it by the guild or the event it lists, null when unknown.
DirectoryEntry *store_directory_entry(const Directory *d, Snowflake entity_id);
// the first directory channel of a guild the user may view, null for none.
Channel *store_guild_directory(Store *s, const Guild *g);

/* ========================================================================== */
/* writing. for events.c and session.c, the ui never calls these.             */
/* ========================================================================== */

// each takes an object as discord sends it, whole or partial, and survives
// anything missing or mistyped in it. they return null when the object has no
// usable id.
User *store_put_user(Store *s, const cima_json *user);
void store_put_presence(Store *s, const cima_json *presence);
// a guild as READY and GUILD_CREATE send it, its channels, threads, roles,
// emojis and members included.
Guild *store_put_guild(Store *s, const cima_json *guild);
// GUILD_UPDATE, which carries no channels.
void store_update_guild(Store *s, const cima_json *guild);
void store_remove_guild(Store *s, Snowflake id);
Role *store_put_role(Store *s, Guild *g, const cima_json *role);
void store_remove_role(Store *s, Guild *g, Snowflake id);
void store_put_emojis(Store *s, Guild *g, const cima_json *emojis);
// `user_id` is used when the member carries neither a user nor a user_id.
Member *store_put_member(Store *s, Guild *g, const cima_json *member,
                         Snowflake user_id);
void store_remove_member(Store *s, Guild *g, Snowflake user_id);
// `guild_id` is used when the channel does not say.
Channel *store_put_channel(Store *s, const cima_json *channel,
                           Snowflake guild_id);
void store_remove_channel(Store *s, Snowflake id);
void store_recipient_add(Store *s, Snowflake channel_id, const cima_json *user);
void store_recipient_remove(Store *s, Snowflake channel_id, Snowflake user_id);
// THREAD_LIST_SYNC.
void store_sync_threads(Store *s, const cima_json *sync);
// whether the current user is a member of a thread.
void store_set_thread_joined(Store *s, Snowflake channel_id, bool joined);
// the current user's membership of a thread as THREAD_MEMBER_UPDATE and the
// lists of threads send it: its flags and its mute.
void store_put_thread_member(Store *s, Snowflake channel_id,
                             const cima_json *member);
// the first message of a post, null when it was deleted.
void store_put_starter(Store *s, Channel *post, const cima_json *message);
// FORUM_UNREADS, the new messages of posts of a forum.
void store_put_forum_unreads(Store *s, const cima_json *unreads);
// whether discord holds a read state of the channel.
[[gnu::pure]] bool store_has_read_state(const Store *s, Snowflake channel_id);
// a channel's last message as passive updates tell it.
void store_set_last_message(Store *s, Snowflake channel_id, Snowflake id);

// MESSAGE_CREATE: adds the message, replaces the pending one it answers,
// moves read states and mention counts.
Message *store_create_message(Store *s, const cima_json *message);
// MESSAGE_UPDATE, which may carry only what changed.
void store_update_message(Store *s, const cima_json *message);
void store_remove_message(Store *s, Snowflake channel_id, Snowflake id);

typedef enum : uint8_t {
  // the latest messages of the channel.
  HISTORY_LATEST,
  // older than the first one loaded.
  HISTORY_BEFORE,
  HISTORY_AFTER,
} HistoryKind;

// the answer of a fetch of `limit` messages, in any order.
void store_put_history(Store *s, Channel *c, const cima_json *messages,
                       HistoryKind kind, uint32_t limit);
// keeps the `keep` latest messages of a channel no longer shown.
void store_trim_messages(Store *s, Channel *c, size_t keep);

// a message of the current user shown before discord confirms it.
Message *store_add_pending(Store *s, Channel *c, const char *content,
                           Snowflake nonce, Snowflake reply_to);
void store_fail_pending(Store *s, Snowflake channel_id, Snowflake nonce,
                        const char *error);
void store_remove_pending(Store *s, Snowflake channel_id, Snowflake nonce);

void store_reaction_add(Store *s, Snowflake channel_id, Snowflake message_id,
                        Snowflake user_id, const cima_json *emoji);
void store_reaction_remove(Store *s, Snowflake channel_id, Snowflake message_id,
                           Snowflake user_id, const cima_json *emoji);
// every reaction of a message, or only the ones of `emoji` when given.
void store_reaction_clear(Store *s, Snowflake channel_id, Snowflake message_id,
                          const cima_json *emoji);

void store_put_read_state(Store *s, const cima_json *read_state);
// the session's own acks: read up to `message_id`, `mention_count` pings
// left after it.
void store_ack(Store *s, Snowflake channel_id, Snowflake message_id,
               uint32_t mention_count);
// MESSAGE_ACK, an ack made by any of the account's clients.
void store_put_ack(Store *s, const cima_json *ack);
// the `bookmark_ids` of the saved messages route, which replace the ones
// known and drop the bookmarks fetched.
void store_set_bookmark_ids(Store *s, const cima_json *ids);
// saved messages as SAVED_MESSAGE_CREATE, the saved messages routes and a
// page of bookmarks send them. one with a due date is a reminder, never a
// bookmark. `page` keeps out those no longer in `bookmark_ids`, as discord
// does with what a page brings.
void store_put_saved_message(Store *s, const cima_json *saved, bool page);
// a bookmark made without discord, as a fixture does.
void store_add_bookmark(Store *s, Snowflake channel_id, Snowflake message_id,
                        int64_t saved_at);
// SAVED_MESSAGE_DELETE.
void store_remove_saved_message(Store *s, Snowflake message_id);
// one entry of user_guild_settings.
void store_put_guild_settings(Store *s, const cima_json *settings);
// user_settings, whole or partial.
void store_put_user_settings(Store *s, const cima_json *settings);
// a voice state of `guild_id`, 0 when the state names its guild or has
// none. one without a channel means the user left.
void store_put_voice_state(Store *s, const cima_json *state,
                           Snowflake guild_id);
void store_remove_voice_state(Store *s, Snowflake user_id, Snowflake guild_id);
// CALL_CREATE and CALL_UPDATE.
void store_put_call(Store *s, const cima_json *call);
// the call ended, and whoever was in it is not anymore.
void store_remove_call(Store *s, Snowflake channel_id);
// the account is not rung in the channel anymore, before discord says so.
void store_stop_ringing(Store *s, Snowflake channel_id);
// the `user_affinities` of the affinities route, replacing the ones known.
void store_put_affinities(Store *s, const cima_json *affinities);
void store_put_relationship(Store *s, const cima_json *relationship);
void store_remove_relationship(Store *s, Snowflake user_id);

void store_typing_start(Store *s, Snowflake channel_id, Snowflake user_id);
// drops the indicators that ran out. returns the unix seconds the next one
// runs out at, 0 when nobody is typing anywhere.
double store_typing_expire(Store *s);

// GUILD_MEMBER_LIST_UPDATE.
void store_member_list_update(Store *s, const cima_json *update);

// the directory of a channel, made empty the first time it is asked for.
Directory *store_open_directory(Store *s, Snowflake channel_id);
// an entry as the directory routes and GUILD_DIRECTORY_ENTRY_CREATE and
// _UPDATE send it, counted in its category when it is new.
DirectoryEntry *store_put_directory_entry(Store *s, const cima_json *entry);
// a directory's whole listing, replacing the entries known.
void store_put_directory(Store *s, Snowflake channel_id,
                         const cima_json *entries);
// the counts route's answer, an object of categories to counts.
void store_put_directory_counts(Store *s, Snowflake channel_id,
                                const cima_json *counts);
void store_remove_directory_entry(Store *s, Snowflake channel_id,
                                  Snowflake entity_id);

// call after changing a channel or a guild by hand.
void store_touch_channel(Store *s, Channel *c);
void store_touch_guild(Store *s, Guild *g);

/* ========================================================================== */
/* events. the path every gateway dispatch takes into the store.              */
/* ========================================================================== */

// applies one dispatch, `type` being its name like "MESSAGE_CREATE" and
// `data` its payload. unknown ones are ignored.
void events_dispatch(Store *s, const char *type, const cima_json *data);

// applies recorded gateway payloads: one payload, or an array of them, each
// an object with "t" and "d". false when `text` is not that.
[[nodiscard]] bool events_feed(Store *s, const char *text, size_t len);
// the same from a file.
[[nodiscard]] bool events_feed_file(Store *s, const char *path);

#endif
