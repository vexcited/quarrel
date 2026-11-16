#ifndef QUARREL_UI_APP_H
#define QUARREL_UI_APP_H

#include <civue/civue.h>
#include <hcaptcha/hcaptcha.h>

#include "discord/session.h"
#include "images.h"
#include "look.h"
#include "platform/platform.h"

constexpr float GUILD_BAR_WIDTH = 72;
constexpr float CHANNEL_LIST_WIDTH = 302;
constexpr float MEMBER_LIST_WIDTH = 264;
constexpr float TOP_BAR_HEIGHT = 32;
constexpr float HEADER_HEIGHT = 48;

// the channel a guild was last left on.
typedef struct {
  Snowflake guild_id, channel_id;
} LastChannel;

typedef enum : uint8_t {
  FRIENDS_ONLINE,
  FRIENDS_ALL,
  FRIENDS_IMPLICIT,
} FriendsTab;

typedef struct {
  Session *session;
  ImageCache *images;
  Look look;
  civue_scrollbar_look bars, thin_bars;

  // the account what is below belongs to, and whether its switcher shows.
  Snowflake account_id;
  bool accounts_open;

  // the guild whose channels show, 0 for the direct messages.
  Snowflake guild_id;
  CIVUE_VEC(LastChannel) last_channels;
  bool members_hidden;

  // the login form.
  char *login, *password, *code;
  SessionMfaMethod mfa_method;
  hcaptcha *captcha;
  uint64_t captcha_version;

  // the composer.
  char *draft;
  Snowflake reply_to;
  bool reply_ping;
  // the message being edited, its text in `draft` and what was typed before
  // in `kept_draft`.
  Snowflake editing;
  char *kept_draft;
  bool focus_composer;

  // the message list: whether it follows its end, and what it held last
  // frame, to stay put when history arrives over it.
  Snowflake list_channel;
  bool stick;
  double unstuck_at;
  float list_height, from_bottom;
  Snowflake list_last;

  // the message under the pointer, and its toolbar's rect, which holds the
  // hover while the pointer is over it.
  Snowflake hover_message, hover_next;
  civue_rect toolbar_rect;

  // the friends page: its tab, what its search holds, and the timer that
  // redraws it while a counter of elapsed time shows.
  FriendsTab friends_tab;
  char *friends_search;
  PlatformTimer *friends_tick;

  // the spoilers the user clicked.
  CIVUE_VEC(civue_id) revealed;
} App;

// forgets what was shown and typed, for another account.
void app_reset(App *app);
void app_select_guild(App *app, Snowflake guild_id);
void app_select_channel(App *app, Snowflake channel_id);
Channel *app_channel(App *app);
Guild *app_guild(App *app);

#endif
