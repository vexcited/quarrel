#ifndef QUARREL_DISCORD_SESSION_H
#define QUARREL_DISCORD_SESSION_H

#include "discord/store.h"

/* ========================================================================== */
/* one account: the api, the gateway and the store, tied together for the     */
/* ui. everything is called from the main thread, and nothing here blocks:    */
/* calls return at once and the store or the session's state change later,    */
/* after which `on_change` is called so the ui can ask for a frame.           */
/*                                                                            */
/*   Session *session = session_new(&(SessionOptions){.on_change = wake});    */
/*   session_start(session);                                                  */
/*   ... each frame: read session->state and session->store ...               */
/*   session_free(session);                                                   */
/*                                                                            */
/* environment:                                                               */
/*   QUARREL_FIXTURE=path   session_start plays the recorded gateway events   */
/*                          at `path` instead of connecting, so the ui runs   */
/*                          without an account. what is sent, edited, read or */
/*                          reacted to is then applied to the store only.     */
/*   QUARREL_TOKEN=token    session_start uses this token and leaves the      */
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
  // logged in, the gateway is connecting for the first time.
  SESSION_CONNECTING,
  // the store is filled and live.
  SESSION_READY,
  // the connection dropped. the store still holds what it had and catches
  // up once the gateway is back.
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

// an account logged in on this machine. the names are null until its first
// READY.
typedef struct {
  Snowflake id;
  char *username, *global_name, *discriminator, *avatar;
} SessionAccount;

typedef struct {
  // called on the main thread after the state or the store changed.
  // civue_window_wake fits as it is.
  void (*on_change)(void);
  // never reads or writes the token in the keychain.
  bool no_keychain;
} SessionOptions;

typedef struct Api Api;
typedef struct Gateway Gateway;

typedef struct Session {
  SessionState state;
  // never null, and the same store for the session's whole life.
  Store *store;
  // why the last login failed or why the session ended, null when there is
  // nothing to say. cleared by the next attempt.
  char *error;
  SessionMfa mfa;
  SessionCaptcha captcha;
  // the channel the ui shows, 0 for none.
  Snowflake selected_channel_id;
  // the store came from a fixture and nothing touches the network.
  bool offline;
  // the accounts that stay logged in, in the order they were added.
  SessionAccount *accounts;
  size_t naccounts;
  // the account connected or connecting, 0 on the login form.
  Snowflake account_id;
  // bumped with every change of the fields above.
  uint64_t version;

  // the session's own.
  SessionOptions options;
  Api *api;
  Gateway *gateway;
  char *token;
  // the keychain holds `token` under the account's key.
  bool token_saved;
  // bumped when the session ends, so answers meant for it are dropped.
  uint64_t epoch;
  char *login, *password;
  char *mfa_ticket, *mfa_instance_id;
  bool was_ready;
  void *calls;
  void *typing_timer;
  Snowflake typing_channel_id;
  double typing_sent_at;
  Snowflake list_guild_id, list_channel_id;
  uint32_t list_first, list_last;
  uint64_t nonce_counter;
} Session;

Session *session_new(const SessionOptions *options);
// closes the connection and frees the store. no callback runs after this.
void session_free(Session *s);

// what to do at launch: plays QUARREL_FIXTURE when set, else connects with
// QUARREL_TOKEN or as the account used last, else stays logged out.
void session_start(Session *s);
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
// ends the session on discord's side too and forgets the account, then
// connects as another saved one when there is one.
void session_logout(Session *s);
// connects as a saved account. the one left stays logged in.
void session_switch_account(Session *s, Snowflake account_id);
// leaves the account for the login form without logging it out.
void session_add_account(Session *s);

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
// opens the dm with a user and selects it once it exists.
void session_open_dm(Session *s, Snowflake user_id);
// the status others see.
void session_set_status(Session *s, UserStatus status, bool invisible);

#endif
