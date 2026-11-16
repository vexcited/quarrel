#ifndef QUARREL_DISCORD_API_H
#define QUARREL_DISCORD_API_H

#include "discord/util.h"
#include "json.h"

/* ========================================================================== */
/* discord's rest api as a user client speaks it. every call answers through  */
/* its callback on the main thread, once, unless the Api is freed first.      */
/* requests of the same route wait for each other, and one answered 429 is    */
/* sent again after the time discord asks for.                                */
/* ========================================================================== */

constexpr uint32_t DISCORD_API_VERSION = 9;
// the build of the web client whose behavior this follows.
constexpr uint32_t DISCORD_BUILD_NUMBER = 673869;

typedef struct Api Api;

/* ========================================================================== */
/* client properties, the one place that says what client this is. the        */
/* gateway identifies with the same ones.                                     */
/* ========================================================================== */

// the User-Agent of every request, also sent as browser_user_agent.
const char *api_user_agent(void);
// writes the client properties object. `gateway` adds the fields only sent
// when identifying.
void api_write_client_properties(JsonWriter *w, bool gateway);

// the id discord gave this installation, null before it gave one. it is
// kept across launches, sent with every request and when identifying.
const char *api_installation_id(void);
void api_set_installation_id(const char *id);

/* ========================================================================== */
/* answers.                                                                   */
/* ========================================================================== */

typedef struct {
  // where in the request the error is, like "login" or "embeds.0.title".
  const char *path;
  // discord's name for it, like "INVALID_LOGIN".
  const char *code;
  const char *message;
} ApiFieldError;

typedef struct {
  // the http status, 0 when no answer came.
  int status;
  // discord's json error code, 0 when it sent none.
  uint32_t code;
  // never null, what to show: the first field error when there is one, else
  // discord's message, else what went wrong with the connection.
  const char *message;
  const ApiFieldError *fields;
  size_t nfields;
} ApiError;

// what discord asks to be solved before it takes the request.
typedef struct {
  // "hcaptcha", "recaptcha" or "recaptcha_enterprise".
  const char *service;
  // null for a service whose key is fixed.
  const char *sitekey;
  const char *session_id;
  // must be passed to the challenge when not null.
  const char *rqdata;
  const char *rqtoken;
  bool serve_invisible;
  // the errors of the last attempt, or a message for clients without
  // captchas.
  const char *const *keys;
  size_t nkeys;
} ApiCaptcha;

// a solved challenge, sent with the request it was asked for.
typedef struct {
  const char *key;
  // from the challenge, null when it had none.
  const char *session_id, *rqtoken;
} ApiCaptchaSolution;

typedef struct {
  // a 2xx status.
  bool ok;
  int status;
  // the body when it is json, null otherwise. only valid during the callback.
  const Json *json;
  // when not ok.
  ApiError error;
  // not null when the request was challenged.
  const ApiCaptcha *captcha;
} ApiResponse;

typedef void (*ApiDone)(void *ctx, const ApiResponse *res);

typedef enum : uint8_t {
  // `token` is good to use.
  API_LOGIN_TOKEN,
  // a second factor is asked, see `mfa`.
  API_LOGIN_MFA,
  // a captcha must be solved and the call made again with its solution.
  API_LOGIN_CAPTCHA,
  API_LOGIN_ERROR,
} ApiLoginKind;

typedef struct {
  const char *ticket;
  // null when discord sent none.
  const char *login_instance_id;
  bool totp, sms, backup, webauthn;
} ApiMfa;

// everything here is only valid during the callback.
typedef struct {
  ApiLoginKind kind;
  const char *token;
  Snowflake user_id;
  ApiMfa mfa;
  ApiCaptcha captcha;
  ApiError error;
} ApiLoginResult;

typedef void (*ApiLoginDone)(void *ctx, const ApiLoginResult *result);

typedef enum : uint8_t {
  API_MFA_TOTP,
  API_MFA_SMS,
  API_MFA_BACKUP,
} ApiMfaKind;

/* ========================================================================== */
/* the client.                                                                */
/* ========================================================================== */

Api *api_new(void);
// no callback runs after this.
void api_free(Api *api);
// null for none. requests made before login carry the fingerprint instead.
void api_set_token(Api *api, const char *token);
const char *api_token(const Api *api);

// a request to any route. `path` starts with a slash, after /api/v9. `route`
// names what is rate limited together, the path with its minor ids left out,
// or null for `path` itself. `body` is json or null.
typedef struct {
  const char *method;
  const char *path;
  const char *route;
  const char *body;
  const ApiCaptchaSolution *captcha;
} ApiRequest;

void api_request(Api *api, const ApiRequest *req, ApiDone done, void *ctx);

/* ========================================================================== */
/* authentication.                                                            */
/* ========================================================================== */

// asks for the fingerprint discord follows a client by until it is logged
// in, and keeps it for the requests that follow. `done` may be null.
void api_fetch_fingerprint(Api *api, ApiDone done, void *ctx);

// `captcha` is null on the first attempt.
void api_login(Api *api, const char *login, const char *password,
               const ApiCaptchaSolution *captcha, ApiLoginDone done, void *ctx);
// completes a login with a code from an authenticator app, a text message
// or a backup code.
void api_mfa(Api *api, ApiMfaKind kind, const ApiMfa *mfa, const char *code,
             ApiLoginDone done, void *ctx);
// has discord text a code to the user's phone, for API_MFA_SMS.
void api_mfa_send_sms(Api *api, const char *ticket, ApiDone done, void *ctx);
// invalidates the token.
void api_logout(Api *api, ApiDone done, void *ctx);

/* ========================================================================== */
/* messages.                                                                  */
/* ========================================================================== */

// up to `limit` messages, at most 100. one of `before`, `after` and `around`
// may be given, 0 for none: the latest messages without any.
void api_get_messages(Api *api, Snowflake channel_id, uint32_t limit,
                      Snowflake before, Snowflake after, Snowflake around,
                      ApiDone done, void *ctx);

typedef struct {
  const char *content;
  // echoed in MESSAGE_CREATE so the pending message is found again.
  Snowflake nonce;
  // 0 for a message that answers none.
  Snowflake reply_to;
  // whether the author of the message answered is pinged.
  bool reply_ping;
} ApiSendMessage;

void api_send_message(Api *api, Snowflake channel_id,
                      const ApiSendMessage *message, ApiDone done, void *ctx);
void api_edit_message(Api *api, Snowflake channel_id, Snowflake message_id,
                      const char *content, ApiDone done, void *ctx);
void api_delete_message(Api *api, Snowflake channel_id, Snowflake message_id,
                        ApiDone done, void *ctx);
// marks the channel read up to `message_id`. `flags` are the read state's
// and `last_viewed` the days since discord's epoch, as the docs ask.
void api_ack_message(Api *api, Snowflake channel_id, Snowflake message_id,
                     uint32_t flags, uint32_t last_viewed, ApiDone done,
                     void *ctx);
void api_trigger_typing(Api *api, Snowflake channel_id, ApiDone done,
                        void *ctx);

// the current user's own reaction. `emoji_id` is 0 for a unicode emoji,
// `name` then being its utf-8.
void api_add_reaction(Api *api, Snowflake channel_id, Snowflake message_id,
                      Snowflake emoji_id, const char *name, ApiDone done,
                      void *ctx);
void api_remove_reaction(Api *api, Snowflake channel_id, Snowflake message_id,
                         Snowflake emoji_id, const char *name, ApiDone done,
                         void *ctx);

// opens the dm with a user, or finds it again. the answer is the channel.
void api_open_dm(Api *api, Snowflake user_id, ApiDone done, void *ctx);

#endif
