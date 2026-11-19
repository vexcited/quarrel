#ifndef QUARREL_UI_APP_H
#define QUARREL_UI_APP_H

#include <civue/civue.h>
#include <hcaptcha/hcaptcha.h>

#include "discord/session.h"
#include "images.h"
#include "look.h"
#include "platform/platform.h"
#include "voice/audio.h"

constexpr float SIDEBAR_WIDTH = 300;
constexpr float SIDEBAR_MIN = 240;
constexpr float SIDEBAR_MAX = 440;
constexpr float MEMBER_LIST_WIDTH = 264;
constexpr float TOP_BAR_HEIGHT = 38;
constexpr float HEADER_HEIGHT = 48;
// a list takes its scroll once it was laid out, a frame after it opened.
constexpr uint8_t GUILDS_SETTLE = 3;

// the channel a guild was last left on.
typedef struct {
  Snowflake guild_id, channel_id;
} LastChannel;

typedef enum : uint8_t {
  FRIENDS_ONLINE,
  FRIENDS_ALL,
  FRIENDS_IMPLICIT,
} FriendsTab;

typedef CIVUE_VEC(Snowflake) Ids;

// what opens under a button, one at a time.
typedef enum : uint8_t {
  POPOVER_NONE,
  POPOVER_ACCOUNTS,
  POPOVER_GUILDS,
  POPOVER_INBOX,
  POPOVER_GUILD_MENU,
  POPOVER_INVITE,
  POPOVER_NEW_DM,
  POPOVER_NEW_GUILD,
  POPOVER_MESSAGE_MENU,
  POPOVER_REACT,
  POPOVER_FORWARD,
  POPOVER_VOICE_SOUND,
  POPOVER_VOICE_CAMERA,
} Popover;

typedef enum : uint8_t {
  INBOX_UNREADS,
  INBOX_MENTIONS,
} InboxTab;

typedef struct {
  Roster *roster;
  // the roster's current one, taken at the start of each frame.
  Session *session;
  civue_images *images;
  Look look;
  civue_scrollbar_look bars, thin_bars;

  // the account what is below belongs to, and what is open over the window.
  Snowflake account_id;
  Popover popover;
  // the status whose durations are listed beside the accounts' card, with
  // the row they hang from. `status_menu_open` is false for none.
  UserStatus status_menu;
  bool status_menu_open;
  civue_rect status_menu_row;
  // the guilds' switcher: the frames left to settle it since it opened.
  uint8_t guilds_fresh;
  // where the switcher's card starts, and how much of its list the card
  // is too short to show.
  float guilds_top, guilds_hidden;
  // scrolling still makes the card taller.
  bool guilds_grows;
  // when the pointer was last over its arrows, down then up, 0 when not.
  double guilds_arrow[2];
  // where the pointer was as it opened. its arrows wait for it to move.
  civue_vec2 guilds_mouse;
  bool guilds_armed;
  // something is drawn over the window's buttons, and they are hidden for it.
  bool covers_buttons, buttons_hidden;
  // the inbox's tab, and whether its mentions were asked for since it
  // opened.
  bool inbox_asked;
  InboxTab inbox_tab;
  // how wide the user made the sidebar.
  float sidebar_width;

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
  // the message a menu, an emoji picker or a forward is open for, the
  // toolbar it opened from and what its search holds.
  Snowflake menu_message;
  civue_rect menu_anchor;
  char *menu_search;
  // its row that deletes was pressed once, and its search is to be focused.
  bool menu_armed, menu_focus;

  // the friends page: its tab, what its search holds, and the timer that
  // redraws it while a counter of elapsed time shows.
  FriendsTab friends_tab;
  char *friends_search;
  // what the search over the direct messages holds.
  char *dm_search;
  PlatformTimer *friends_tick;

  // the guilds whose muted channels are hidden.
  Ids hide_muted;
  // the new conversation: what its search holds and the friends picked.
  char *pick_search;
  Ids picks;
  // the name typed for a new guild.
  char *guild_name;
  // the invite's link was copied since it opened.
  bool invite_copied;

  // the open voice channel shows its chat rather than its call.
  bool voice_chat;
  // what a menu of the call lists, by AudioKind, as the machine had them
  // when it opened, with the name of the system's own of each.
  AudioDevice *devices[3];
  size_t ndevices[3];
  char default_names[3][96];
  // audio_devices_version as they were taken.
  uint32_t devices_seen;
  // the list open beside the menu, an AudioKind, -1 for none, and the row
  // it opened from.
  int8_t voice_side;
  civue_rect voice_side_row;

  // the settings: the part of their page the sidebar points at, the kind
  // whose list of devices is open under its box, -1 for none, and that box.
  bool settings_open;
  uint8_t settings_part;
  int8_t settings_select;
  civue_rect settings_select_rect;

  // the spoilers the user clicked.
  CIVUE_VEC(civue_id) revealed;
} App;

// forgets what was shown and typed, for another account.
void app_reset(App *app);
void app_select_guild(App *app, Snowflake guild_id);
void app_select_channel(App *app, Snowflake channel_id);
Channel *app_channel(App *app);
Guild *app_guild(App *app);
bool ids_has(const Ids *ids, Snowflake id);
// adds `id`, or removes it when it is there.
void ids_toggle(Ids *ids, Snowflake id);

static inline void app_toggle(App *app, Popover popover) {
  app->popover = app->popover == popover ? POPOVER_NONE : popover;
}

#endif
