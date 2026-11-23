#ifndef QUARREL_DISCORD_SESSION_H
#define QUARREL_DISCORD_SESSION_H

#include "discord/gifs.h"
#include "discord/settings_proto.h"
#include "discord/store.h"

/* ========================================================================== */
/* a session is one account: the api, the gateway and the store, tied         */
/* together for the ui. the roster holds one for every account logged in on   */
/* this machine, all connected at once, and the one the ui shows.             */
/* everything is called from the main thread, and nothing here blocks: calls  */
/* return at once and the store or the session's state change later, after    */
/* which `on_change` is called so the ui can ask for a frame.                 */
/*                                                                            */
/*   Roster *roster = roster_new(&(SessionOptions){.on_change = wake});       */
/*   roster_start(roster);                                                    */
/*   ... each frame: read roster->current, its state and its store ...        */
/*   roster_free(roster);                                                     */
/*                                                                            */
/* environment:                                                               */
/*   QUARREL_FIXTURE=path   roster_start plays the recorded gateway events    */
/*                          at `path` instead of connecting, so the ui runs   */
/*                          without an account. what is sent, edited, read or */
/*                          reacted to is then applied to the store only.     */
/*   QUARREL_TOKEN=token    roster_start uses this token and leaves the       */
/*                          keychain and the saved accounts alone.            */
/* ========================================================================== */

typedef enum : uint8_t {
  // show the login form, with `error` when it is not null.
  SESSION_LOGGED_OUT,
  // a login request is on its way.
  SESSION_LOGGING_IN,
  // discord asks for a second factor, see `mfa`.
  SESSION_MFA,
  // discord asks for a captcha, see `captcha`.
  SESSION_CAPTCHA,
  // logged in, the gateway is connecting for the first time and nothing
  // was kept of the account to show meanwhile.
  SESSION_CONNECTING,
  // the store is filled and live.
  SESSION_READY,
  // the connection dropped, or was not made yet since the launch. the store
  // holds what it had, from the disk in the second case, and catches up
  // once the gateway is there.
  SESSION_RECONNECTING,
} SessionState;

typedef enum : uint8_t {
  SESSION_MFA_TOTP,
  SESSION_MFA_SMS,
  SESSION_MFA_BACKUP,
} SessionMfaMethod;

typedef struct {
  // the ways discord accepts for this account.
  bool totp, sms, backup;
  // a code was texted and can be submitted with SESSION_MFA_SMS.
  bool sms_sent;
} SessionMfa;

// what a captcha widget needs. strings are null when discord sent none.
typedef struct {
  // "hcaptcha", "recaptcha" or "recaptcha_enterprise".
  char *service;
  char *sitekey;
  // must be given to the challenge when not null.
  char *rqdata;
  bool serve_invisible;

  // the session's own, sent back with the solution.
  char *session_id, *rqtoken;
} SessionCaptcha;

typedef enum : uint8_t {
  // nothing asks for the qr code.
  SESSION_QR_OFF,
  // connecting to the remote auth gateway, or waiting to try again.
  SESSION_QR_LOADING,
  // `url` is the code to show.
  SESSION_QR_READY,
  // a phone scanned it, its user is asked to confirm.
  SESSION_QR_SCANNED,
  // the phone confirmed, the token is on its way.
  SESSION_QR_LOGGING_IN,
} SessionQrStep;

// the login by qr code that the login form shows beside it.
typedef struct {
  SessionQrStep step;
  // "https://discord.com/ra/" and the fingerprint, when READY.
  char *url;
  // who scanned it, when SCANNED.
  Snowflake user_id;
  char *username, *discriminator, *avatar;
} SessionQr;

typedef struct Session Session;
typedef struct Roster Roster;

// an account logged in on this machine. the names are null until its first
// READY.
typedef struct {
  Snowflake id;
  char *username, *global_name, *discriminator, *avatar;
  // its own, connected since the launch. null once discord refused its
  // connection, until a switch to the account tries again.
  Session *session;
} SessionAccount;

// a message of the inbox that mentions the current user.
typedef struct {
  Snowflake id, channel_id, author_id;
  // never null.
  char *content;
} SessionMention;

// what a search of a directory found.
typedef struct {
  Snowflake channel_id;
  // the words searched, and the DirectoryCategory it kept to, -1 for all.
  char *query;
  int8_t category;
  // the entities found, in discord's order, each an entry of the store's
  // directory.
  Snowflake *results;
  size_t nresults;
  bool loading, done;
  // why it failed, null when it did not.
  char *error;

  // the session's own, counting searches so a late answer is dropped.
  uint64_t asked;
} SessionDirectorySearch;

// a school whose hub the email may join.
typedef struct {
  Snowflake id;
  char *name, *icon;
} SessionHubSchool;

typedef enum : uint8_t {
  // asks for the email.
  HUB_JOIN_EMAIL,
  // the email fits several schools, one is to be picked.
  HUB_JOIN_SCHOOL,
  // a code was emailed for `guild_id`.
  HUB_JOIN_CODE,
  // no school has a hub for the email yet.
  HUB_JOIN_WAITLIST,
  // signed up to the waitlist.
  HUB_JOIN_WAITING,
  // joined `guild_id`, which comes through the gateway.
  HUB_JOIN_JOINED,
} SessionHubStep;

typedef struct {
  SessionHubStep step;
  char *email;
  Snowflake guild_id;
  SessionHubSchool *schools;
  size_t nschools;
  bool loading;
  // why the last step failed, null when it did not.
  char *error;
} SessionHubJoin;

typedef struct {
  // called on the main thread after the state or the store changed.
  // civue_window_wake fits as it is.
  void (*on_change)(void);
  // never reads or writes the token in the keychain.
  bool no_keychain;
} SessionOptions;

typedef struct Api Api;
typedef struct Gateway Gateway;
typedef struct Voice Voice;

// a badge of a profile: `icon` is a hash for cdn_badge_icon.
typedef struct {
  char *icon, *description;
} SessionBadge;

// a guild the current user shares with someone, with the name they wear
// there, null for none.
typedef struct {
  Snowflake id;
  char *nick;
} ProfileGuild;

// an account elsewhere a user shows on their profile, like "github".
typedef struct {
  char *type, *name;
  bool verified;
} ProfileConnection;

// what discord says of a user beyond the user object, seen from a guild or
// from none. see session_profile.
typedef struct {
  Snowflake user_id, guild_id;
  // asked and not answered yet, answered, or answered with an error.
  bool loading, loaded, failed;
  // the mutuals were asked for, as the full profile does.
  bool full;
  // store_now when it was answered, to ask again after a while.
  double at;
  char *bio, *pronouns, *banner;
  // 0xrrggbb when `has_accent`, the banner's color without an image.
  uint32_t accent_color;
  bool has_accent;
  // the two colors of a themed profile, top then bottom, when `has_theme`.
  uint32_t theme[2];
  bool has_theme;
  // what the member set in `guild_id`, null or false where they did not.
  char *guild_bio, *guild_pronouns, *guild_banner;
  uint32_t guild_theme[2];
  bool has_guild_theme;
  // the user's badges, then the ones of the guild.
  SessionBadge *badges;
  size_t nbadges;
  // "name#1234" from before unique usernames, null when hidden or none.
  char *legacy_username;
  ProfileGuild *mutual_guilds;
  size_t nmutual_guilds;
  // users the store holds: the few discord tells with the profile, then
  // every one once the full profile listed them.
  Snowflake *mutual_friends;
  size_t nmutual_friends;
  uint32_t mutual_friends_count;
  bool friends_loading, friends_loaded;
  // the nitro they pay for, which their theme needs.
  uint8_t premium_type;
  ProfileConnection *connections;
  size_t nconnections;
  // unix milliseconds, 0 for never.
  int64_t premium_since, premium_guild_since;
  // what the current user wrote about them, null for nothing.
  char *note;
  bool note_loaded;
} Profile;

// how the posts of a forum are looked for.
typedef struct {
  ForumSort sort;
  // posts wearing these tags, all of them when `match_all`, else any.
  Snowflake *tags;
  size_t ntags;
  bool match_all;
  // words of their titles, null for none.
  char *query;
} ForumQuery;

// the posts of the forum the ui shows that discord was asked for, see
// session_forum_query. the active ones come with the forum, through the
// gateway, and are not listed here unless the query has words.
typedef struct {
  Snowflake channel_id;
  ForumQuery query;
  // what discord answered, in its order, without duplicates.
  Snowflake *posts;
  size_t nposts;
  // all the posts matching, as discord counts them.
  uint32_t total;
  bool loading, has_more;
  // why the last page did not come.
  char *error;

  // the session's own, bumped with every new query.
  uint64_t generation;
} SessionForum;

struct Session {
  SessionState state;
  // never null, and the same store for the session's whole life.
  Store *store;
  // why the last login failed or why the session ended, null when there is
  // nothing to say. cleared by the next attempt.
  char *error;
  SessionMfa mfa;
  SessionCaptcha captcha;
  SessionQr qr;
  // the channel the ui shows, 0 for none.
  Snowflake selected_channel_id;
  // the store came from a fixture and nothing touches the network.
  bool offline;
  // the recent messages that mention the user, newest first, as the last
  // session_fetch_mentions found them.
  SessionMention *mentions;
  size_t nmentions;
  bool mentions_loading, mentions_loaded;
  // the pages of bookmarks the inbox lists, which the store holds: one on
  // its way, more to fetch, the first came, the last failed.
  bool bookmarks_loading, bookmarks_more, bookmarks_fetched, bookmarks_failed;
  // what the last change of a bookmark came to, for the ui to tell for a
  // moment, `notice_failed` when it went wrong. null for nothing, and
  // `notice_version` bumped with every new one.
  char *notice;
  bool notice_failed;
  uint64_t notice_version;
  // the invite session_create_invite asked for: the guild it is to, its
  // code once discord answered, or why there is none.
  Snowflake invite_guild_id;
  char *invite_code, *invite_error;
  bool invite_loading;
  // the guild session_create_guild asked for: on its way, why it failed, or
  // its id once discord made it. the ui takes the id and zeroes it.
  bool guild_creating;
  char *guild_error;
  Snowflake guild_created;
  // the friend request session_add_friend sent: on its way, then what to
  // tell of it, `friend_sent` saying whether it went through. a challenge
  // discord asks for waits in `friend_captcha`, service null for none.
  bool friend_adding, friend_sent;
  char *friend_message;
  SessionCaptcha friend_captcha;
  // the emojis the user reacts with the most, the most used first: the
  // first name of an emoji of unicode or the id of a guild's. see
  // session_fetch_frecency.
  char **frecent_reactions;
  size_t nfrecent_reactions;
  // the gifs the user starred, in no order, which come with the same
  // settings and follow the other clients' changes. `present` once known.
  FavoriteGifs favorite_gifs;
  // the picker's gifs, see session_gifs.
  Gifs *gifs;
  bool frecency_loading, frecency_loaded;
  // the applications asked about, see session_application_icon.
  struct SessionApplication *applications;
  size_t napplications;
  // the voice channel the account joined or is joining from here, 0 for
  // none, and its guild. see session_voice_join.
  Snowflake voice_guild_id, voice_channel_id;
  // the call itself, see voice/voice.h for its status and who speaks. null
  // until discord said where to connect.
  Voice *voice;
  bool voice_mute, voice_deaf;
  // why discord did not open the call, null when it did or may still.
  char *voice_error;
  // the forum the ui shows, see session_forum_query.
  SessionForum forum;
  // the post session_create_post made: on its way, why it failed, or its
  // id once discord made it. the ui takes the id and zeroes it.
  bool post_creating;
  char *post_error;
  Snowflake post_created;
  // the last search of a directory, see session_directory_search.
  SessionDirectorySearch directory_search;
  // the guild of a directory being joined, and why the last join failed.
  Snowflake directory_joining;
  char *directory_join_error;
  // an entry being listed, changed or taken out of a directory, and why the
  // last such change failed. `directory_saved` is the entity of the last
  // one that went through, which the ui takes and zeroes.
  bool directory_saving;
  char *directory_error;
  Snowflake directory_saved;
  // what session_directory_create lists once its server exists.
  Snowflake directory_pending_channel;
  DirectoryCategory directory_pending_category;
  char *directory_pending_description;
  // the guilds the add-server dialog asked about that the directory
  // already lists, see session_directory_check.
  Snowflake directory_checked_channel;
  bool directory_checking;
  // the student hub being joined by email, see session_hub_lookup.
  SessionHubJoin hub;
  // the guild session_leave_guild is leaving.
  Snowflake leaving_guild;
  // what managing a guild needs, see discord/admin.h. null until used.
  struct Admin *admin;
  // the account connected or connecting, 0 on the login form.
  Snowflake account_id;
  // bumped with every change of the fields above.
  uint64_t version;

  // the session's own.
  Roster *roster;
  SessionOptions options;
  Api *api;
  Gateway *gateway;
  char *token;
  // the keychain holds `token` under the account's key.
  bool token_saved;
  // bumped when the session ends, so answers meant for it are dropped.
  uint64_t epoch;
  char *login, *password;
  // the friend request a challenge holds back, sent again once solved.
  char *friend_username, *friend_note;
  int32_t friend_discriminator;
  char *mfa_ticket, *mfa_instance_id;
  // the qr code's attempt, its retry after a failure and how long that
  // waits, and a count bumped with every attempt so late answers are
  // dropped.
  void *remote_auth, *qr_timer;
  double qr_backoff;
  uint64_t qr_attempt;
  bool was_ready;
  // the store came from the account's last READY, kept on disk, and waits
  // for the gateway to say how things are now.
  bool cached;
  // the READY being received, held until what comes with it did.
  char *ready_text;
  void *calls;
  void *typing_timer;
  // ends a status picked for a while, see session_set_status.
  void *status_timer;
  int64_t status_timed;
  // what the account's profile wears, see session_fetch_profile.
  SessionBadge *badges;
  size_t nbadges;
  bool profile_loading, profile_loaded;
  // the profiles of others asked for, see session_profile.
  Profile **profiles;
  size_t nprofiles;
  // what session_message_user sends once the conversation is opened.
  Snowflake dm_user;
  char *dm_text;
  Snowflake typing_channel_id;
  double typing_sent_at;
  Snowflake list_guild_id, list_channel_id;
  uint32_t list_first, list_last;
  uint64_t nonce_counter;
  // the files put in the composer or sent with a pending message, see
  // discord/uploads.h.
  struct Upload **uploads;
  size_t nuploads;
  uint32_t upload_serial;
  char *voice_token, *voice_endpoint;
  // when the last bookmark listed was saved, as discord wrote it, which the
  // next page comes before. bumped with every new list of them.
  char *bookmarks_before;
  uint64_t bookmarks_generation;
  // the posts whose first message was asked for.
  Snowflake *starters_asked;
  size_t nstarters_asked, starters_cap;
  void *voice_timer;
  // discord confirmed the channel for this gateway session, the call was
  // started with what is known, and a new gateway session has to join again.
  bool voice_stated, voice_live, voice_rejoin;
};

struct Roster {
  // the accounts that stay logged in, in the order they were added.
  SessionAccount *accounts;
  size_t naccounts;
  // the session the ui shows, never null. it is no account's on the login
  // form. it changes with roster_switch, roster_add and roster_logout only,
  // and the one it was stays valid until the next of these calls.
  Session *current;

  // the roster's own.
  SessionOptions options;
  Session **dead;
  size_t ndead;
};

[[nodiscard]] Roster *roster_new(const SessionOptions *options);
// closes every connection and frees every store. no callback runs after
// this.
void roster_free(Roster *r);

// what to do at launch: plays QUARREL_FIXTURE when set, else connects with
// QUARREL_TOKEN, else connects every saved account and shows the one used
// last, else stays logged out.
void roster_start(Roster *r);
// shows a saved account. the one left stays connected.
void roster_switch(Roster *r, Snowflake account_id);
// shows the login form for one more account. the one left stays connected.
void roster_add(Roster *r);
// ends the shown session on discord's side too and forgets its account,
// then shows another saved one when there is one.
void roster_logout(Roster *r);
// plays recorded gateway events as if they came from the gateway and goes
// to SESSION_READY, offline. false when the file cannot be read as json.
bool session_load_fixture(Session *s, const char *path);

/* ========================================================================== */
/* logging in and out.                                                        */
/* ========================================================================== */

// email or phone number, and password. ends in SESSION_CONNECTING,
// SESSION_MFA, SESSION_CAPTCHA, or SESSION_LOGGED_OUT with `error`.
void session_login(Session *s, const char *login, const char *password);
// connects with a token the user has, saving it like one from a login.
void session_login_token(Session *s, const char *token);
// in SESSION_MFA: a code from the method picked. a wrong code comes back to
// SESSION_MFA with `error` set.
void session_submit_mfa(Session *s, SessionMfaMethod method, const char *code);
// in SESSION_MFA: has discord text a code, `mfa.sms_sent` turns true.
void session_request_sms(Session *s);
// in SESSION_CAPTCHA: the token the solved challenge gave.
void session_submit_captcha(Session *s, const char *solution);
// leaves SESSION_MFA or SESSION_CAPTCHA for SESSION_LOGGED_OUT.
void session_cancel_login(Session *s);
// called each frame with whether the qr code is on screen: it is kept alive
// while it is, see `qr`, and dropped once it is not. a scan that is
// confirmed logs in like session_login_token.
void session_qr(Session *s, bool shown);
// leaves the scan of the wrong account for a new code.
void session_qr_restart(Session *s);

/* ========================================================================== */
/* channels and messages. each call is a no op when its channel or message    */
/* is unknown.                                                                */
/* ========================================================================== */

// the channel the ui shows, 0 for none. fetches its latest messages the
// first time, see channel->history_loaded and channel->loading, and
// subscribes to the member list of a guild channel, see store_member_list.
// the channel shown before keeps its 100 latest messages.
void session_select_channel(Session *s, Snowflake channel_id);
// fetches the 50 messages before the oldest one loaded, when
// channel->has_older and nothing is loading.
void session_load_older(Session *s, Snowflake channel_id);
// the rows of the selected channel's member list the ui shows, so the
// gateway keeps them in sync. the first hundred always are.
void session_member_list_range(Session *s, uint32_t first_row,
                               uint32_t last_row);

// sends a message, shown at once as MESSAGE_PENDING. `reply_to` is 0 for a
// message that answers none. returns the nonce of the pending message, 0
// when nothing was sent.
Snowflake session_send(Session *s, Snowflake channel_id, const char *content,
                       Snowflake reply_to, bool reply_ping);
// sends a MESSAGE_FAILED message again, or drops it.
void session_retry(Session *s, Snowflake channel_id, Snowflake nonce);
void session_discard(Session *s, Snowflake channel_id, Snowflake nonce);

void session_edit(Session *s, Snowflake channel_id, Snowflake message_id,
                  const char *content);
void session_delete(Session *s, Snowflake channel_id, Snowflake message_id);
// adds or removes the current user's reaction. `emoji_id` is 0 for a
// unicode emoji, whose utf-8 is `name`.
void session_react(Session *s, Snowflake channel_id, Snowflake message_id,
                   Snowflake emoji_id, const char *name, bool add);
// tells the channel the user is typing. call it on every keystroke, it
// sends at most once every 8 seconds.
void session_typing(Session *s, Snowflake channel_id);
// marks the channel read up to its last message. call it while the user
// looks at the bottom of the channel, it only sends when there is news.
void session_ack(Session *s, Snowflake channel_id);
// fetches the 25 latest messages that mention the user, see `mentions`.
void session_fetch_mentions(Session *s);
// fetches the first page of the bookmarks, the latest saved first, once
// discord said which messages are. with `more`, the next page, or the one
// that failed again. see store->bookmarks and `bookmarks_more`.
void session_fetch_bookmarks(Session *s, bool more);
// bookmarks a message or takes its bookmark away, see `notice`.
void session_bookmark(Session *s, Snowflake channel_id, Snowflake message_id,
                      bool add);
// opens the dm with a user and selects it once it exists.
void session_open_dm(Session *s, Snowflake user_id);
// the same with several users: a new group dm, selected once it exists. one
// user alone opens their dm.
void session_open_group(Session *s, const Snowflake *user_ids, size_t count);
// creates a guild named `name`, see `guild_created`.
void session_create_guild(Session *s, const char *name);
// a friend request to a username, or a legacy "name#0000" tag, with a note
// of at most 120 characters, null or empty for none. see `friend_adding`.
void session_add_friend(Session *s, const char *tag, const char *note);
// the token of the challenge the friend request is held back by.
void session_add_friend_captcha(Session *s, const char *solution);
// forgets what was told of the last friend request, and any challenge.
void session_add_friend_clear(Session *s);
// the hash of an application's icon, for cdn_application_icon. null when it
// has none or while discord is asked, which happens once.
const char *session_application_icon(Session *s, Snowflake application_id);
// asks once for what the user uses the most, see `frecent_reactions`.
void session_fetch_frecency(Session *s);
// the gifs of the picker, made as they are first asked for.
Gifs *session_gifs(Session *s);
// the favorite starred under `url`, null when it is not one.
[[gnu::pure]] const FavoriteGif *session_favorite_gif(const Session *s,
                                                      const char *url);
// stars a gif, newest first, or takes its star away. `gif` is copied.
void session_set_favorite_gif(Session *s, const FavoriteGif *gif,
                              bool favorite);
// pins the message or takes its pin away.
void session_pin(Session *s, Snowflake channel_id, Snowflake message_id,
                 bool pin);
// the channel is unread again from `message_id` on.
void session_mark_unread(Session *s, Snowflake channel_id,
                         Snowflake message_id);
// sends the message of `from_channel_id` to `channel_id` as a forward.
void session_forward(Session *s, Snowflake channel_id,
                     Snowflake from_channel_id, Snowflake message_id);
// asks for an invite link to the guild that lasts a week, or for ever in a
// hub, see `invite_code`.
// the same guild is not asked twice while its code is known.
void session_create_invite(Session *s, Snowflake guild_id);
// the status others see, kept for `seconds` before it goes back to online,
// or for good with 0. it is saved in the account's settings, so that every
// client of the account follows.
void session_set_status(Session *s, UserStatus status, bool invisible,
                        int64_t seconds);
// asks once for the account's own profile, see `badges`.
void session_fetch_profile(Session *s);
// the profile of a user as seen from a guild, 0 for none, never null. it is
// asked for when unknown or old, as the full profile asks it when `full`,
// and with the note on them. offline, it holds what the store knows.
const Profile *session_profile(Session *s, Snowflake user_id,
                               Snowflake guild_id, bool full);
// every friend the current user shares with someone, into the profile's
// `mutual_friends`, once.
void session_mutual_friends(Session *s, Snowflake user_id, Snowflake guild_id);
// what the current user writes about someone, empty to take it away.
void session_set_note(Session *s, Snowflake user_id, const char *note);
// a note changed on another client.
void session_put_note(Session *s, Snowflake user_id, const char *note);
// sends a direct message to a user and shows it, opening the conversation
// first when there is none yet.
void session_message_user(Session *s, Snowflake user_id, const char *content);

/* ========================================================================== */
/* forums.                                                                    */
/* ========================================================================== */

// the posts of a forum the ui lists, see `forum`. a query like the one
// asked last does nothing, another starts the list over.
void session_forum_query(Session *s, Snowflake forum_id,
                         const ForumQuery *query);
// the next page of the list, when it has more and nothing is loading.
void session_forum_more(Session *s);
// asks for the first message of these posts of the forum, the ones known or
// asked for already left out. see channel->starter.
void session_fetch_starters(Session *s, Snowflake forum_id,
                            const Snowflake *post_ids, size_t count);
// a new post titled `title` that begins with `content`, wearing `tags`. see
// `post_created`.
void session_create_post(Session *s, Snowflake forum_id, const char *title,
                         const char *content, const Snowflake *tags,
                         size_t ntags);
// follows a post, which then tells of its new messages, or stops.
void session_follow(Session *s, Snowflake thread_id, bool follow);
// the tags a post wears.
void session_set_post_tags(Session *s, Snowflake post_id, const Snowflake *tags,
                           size_t ntags);
// pins a post, which unpins the forum's other one, closes it or locks it.
void session_update_post(Session *s, Snowflake post_id, bool pinned,
                         bool archived, bool locked);

/* ========================================================================== */
/* directories, the channels of a hub that list servers.                     */
/* ========================================================================== */

// fetches a directory's entries and how many each category holds, once
// for each connection: the gateway keeps them in sync after. see
// store_directory.
void session_directory_open(Session *s, Snowflake channel_id);
// searches a directory for `query`, 1 to 100 bytes, in one DirectoryCategory
// or in all with -1. see `directory_search`. an empty query clears it.
void session_directory_search(Session *s, Snowflake channel_id,
                              const char *query, int category);
// joins the guild an entry of the directory lists, which then comes through
// the gateway. see `directory_joining`.
void session_directory_join(Session *s, Snowflake channel_id,
                            Snowflake guild_id);
// lists a guild the user manages in the directory, `description` up to 200
// bytes or null. see `directory_saving`.
void session_directory_add(Session *s, Snowflake channel_id,
                           Snowflake guild_id, DirectoryCategory category,
                           const char *description);
// makes a server from one of `hub_templates`, named `name`, then lists it
// in the directory like session_directory_add.
void session_directory_create(Session *s, Snowflake channel_id,
                              size_t template_index, const char *name,
                              DirectoryCategory category,
                              const char *description);
// changes the category and the description of an entry.
void session_directory_edit(Session *s, Snowflake channel_id,
                            Snowflake entity_id, DirectoryCategory category,
                            const char *description);
// takes an entry out of the directory.
void session_directory_remove(Session *s, Snowflake channel_id,
                              Snowflake entity_id);

// asks discord which of `count` guilds the directory lists, as the
// add-server dialog does when it opens. the ones listed come into the
// store's directory, without their guild.
void session_directory_check(Session *s, Snowflake channel_id,
                             const Snowflake *guild_ids, size_t count);

// the email of a student, to join their school's hub with: a code is
// emailed when one school fits, `hub.schools` lists them when several do,
// and the waitlist is offered when none does. `guild_id` is 0 at first,
// then the school picked.
void session_hub_lookup(Session *s, const char *email, Snowflake guild_id);
// the code emailed, which joins the hub.
void session_hub_verify(Session *s, const char *code);
// signs the email up to hear when its school gets a hub.
void session_hub_waitlist(Session *s, const char *school);
// back to asking for an email.
void session_hub_reset(Session *s);

// leaves a guild the user does not own. see `leaving_guild`.
void session_leave_guild(Session *s, Snowflake guild_id);
// the user's nickname in a guild, null or empty for none.
void session_set_nick(Session *s, Snowflake guild_id, const char *nick);

typedef enum : uint8_t {
  SESSION_LINK_UP,
  // connecting, or resuming after a drop.
  SESSION_LINK_CONNECTING,
  // dropped, and the next attempt waits for its turn.
  SESSION_LINK_DOWN,
} SessionLink;

// how the session's connection to discord is, to tell the user when what
// they see may be late.
SessionLink session_link(const Session *s);

/* ========================================================================== */
/* voice.                                                                     */
/* ========================================================================== */

// joins a voice channel, leaving the one the account was in. see
// `voice_channel_id`, `voice` and `voice_error` for how it goes.
void session_voice_join(Session *s, Snowflake channel_id);
void session_voice_leave(Session *s);
// a muted microphone sends nothing. deafened, the call plays nothing and
// sends nothing either, as discord has it.
void session_voice_mute(Session *s, bool mute);
void session_voice_deafen(Session *s, bool deaf);

#endif
