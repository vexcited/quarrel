#ifndef QUARREL_DISCORD_OAUTH_H
#define QUARREL_DISCORD_OAUTH_H

#include "discord/api.h"
#include "discord/permissions.h"

/* ========================================================================== */
/* letting an application in, as discord's authorize page does: what discord */
/* says of the asking, then the user's answer. one asking at a time, and its */
/* answers run on the main thread.                                            */
/* ========================================================================== */

// where the application goes, discord's integration types.
typedef enum : uint8_t {
  OAUTH_GUILD,
  OAUTH_USER,
} OauthInstall;

// a server of the user's, as the asking lists them.
typedef struct {
  Snowflake id;
  char *name, *icon;
  // what the user may do there.
  Permissions permissions;
} OauthGuild;

// a challenge discord holds the answer back for. `service` is null for
// none, the other strings when discord sent none.
typedef struct {
  char *service, *sitekey, *rqdata;
  char *session_id, *rqtoken;
} OauthCaptcha;

typedef struct {
  // what is asked: by which application, for which scopes, separated by
  // spaces, where it goes and what its bot may do there. 0 while nothing
  // is asked.
  Snowflake client_id;
  char *scopes;
  OauthInstall install;
  Permissions permissions;

  // discord's word on it, there once `ready`. `error` says why it cannot
  // be asked or why the answer was refused, null otherwise.
  bool loading, ready;
  char *error;
  // never null once ready.
  char *name;
  char *icon, *terms_url, *privacy_url;
  // its bot, 0 without one, and how many servers it is in.
  Snowflake bot_id;
  bool bot_verified;
  uint32_t guild_count;
  // the user's servers, for an application that goes in one.
  OauthGuild *guilds;
  size_t nguilds;
  // what its makers may do, as discord discloses it.
  bool sees_ip, shows_ads;

  // the answer: on its way, held back by a challenge, or taken. once
  // `done`, `guild_id` is the server it went in, 0 for none, and `redirect`
  // where its makers want the user next, null for nowhere.
  bool sending, done;
  OauthCaptcha captcha;
  Snowflake guild_id;
  char *redirect;

  // bumped with every change of the fields above.
  uint64_t version;

  // its own.
  Api *api;
  void (*on_change)(void);
  struct OauthTicket *tickets;
  uint64_t seq;
  Permissions granted;
  // discord's ids of what it discloses, sent back once authorized.
  int32_t disclosures[8];
  size_t ndisclosures;
  bool disclosed, previewed;
} Oauth;

// asks discord through `api`, which must outlive it.
Oauth *oauth_new(Api *api, void (*on_change)(void));
void oauth_free(Oauth *o);

// starts asking, in place of what was asked before.
void oauth_ask(Oauth *o, Snowflake client_id, const char *scopes,
               OauthInstall install, Permissions permissions);
// the user's answer: yes in `guild_id`, 0 outside one, letting the bot do
// `permissions`, or no.
void oauth_answer(Oauth *o, bool authorize, Snowflake guild_id,
                  Permissions permissions);
// the same answer again with the challenge solved.
void oauth_answer_captcha(Oauth *o, const char *solution);
// gives the challenge up, back to before the answer.
void oauth_drop_captcha(Oauth *o);
// forgets what was asked.
void oauth_end(Oauth *o);

// `scopes` holds `scope`.
[[gnu::pure]] bool oauth_has_scope(const char *scopes, const char *scope);

#endif
