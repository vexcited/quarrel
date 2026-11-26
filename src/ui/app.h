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
// where what a message says starts, its avatar before it.
constexpr float MESSAGE_GUTTER = 72;
// the room a list of messages leaves under its last one, where the composer
// says who types.
constexpr float MESSAGE_FOOT = 24;
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
  FRIENDS_ADD,
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
  POPOVER_GIFS,
  POPOVER_STICKERS,
  POPOVER_STICKER_INFO,
  POPOVER_EMOJIS,
  POPOVER_EMOJI_CARD,
  POPOVER_FORUM_VIEW,
  POPOVER_FORUM_TAGS,
  POPOVER_POST_TAGS,
  POPOVER_POST_MENU,
  POPOVER_PROFILE,
  POPOVER_DIRECTORY_MENU,
  POPOVER_EXPLORE_LANGUAGE,
  POPOVER_COMPONENT_SELECT,
} Popover;

// the side of what was clicked a profile card opens on.
typedef enum : uint8_t {
  PROFILE_RIGHT,
  PROFILE_LEFT,
} ProfileSide;

// the tabs of the full profile.
typedef enum : uint8_t {
  PROFILE_TAB_ABOUT,
  PROFILE_TAB_ACTIVITY,
  PROFILE_TAB_GUILDS,
  PROFILE_TAB_FRIENDS,
} ProfileTab;

// what a student hub has open over the window, one at a time.
typedef enum : uint8_t {
  HUB_DIALOG_NONE,
  // a server to list in the directory.
  HUB_DIALOG_ADD,
  // the category and description of an entry.
  HUB_DIALOG_EDIT,
  // asks before taking an entry out.
  HUB_DIALOG_REMOVE,
  // asks before leaving the hub.
  HUB_DIALOG_LEAVE,
  // the email that joins a school's hub.
  HUB_DIALOG_JOIN,
  // the name a new member goes by in the hub.
  HUB_DIALOG_NICK,
} HubDialog;

// the steps of the dialog that adds a server, as discord's.
typedef enum : uint8_t {
  ADD_CHOOSE,
  ADD_TEMPLATES,
  ADD_CUSTOMIZE,
  ADD_DETAILS,
  ADD_DONE,
} AddStep;

// the pages of the settings.
typedef enum : uint8_t {
  SETTINGS_VOICE,
  SETTINGS_NOTIFICATIONS,
} SettingsPage;

typedef enum : uint8_t {
  INBOX_UNREADS,
  INBOX_MENTIONS,
  INBOX_BOOKMARKS,
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
  // the session notice shown last, see session->notice, the session it came
  // from, when it goes away and the timer that redraws then.
  uint64_t notice_seen;
  const Session *notice_session;
  double notice_until;
  PlatformTimer *notice_timer;
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
  // the menu of the composer's plus is open.
  bool upload_menu;
  // the file whose modal is open, 0 for none, and what its fields hold.
  uint32_t upload_editing;
  char *upload_name, *upload_alt;
  bool upload_spoiler;
  // why files were refused, shown over the window until dismissed. empty
  // for nothing.
  char upload_title[64], upload_body[224];
  // the message typed is longer than discord takes.
  bool long_message;
  // the message typed holds an emoji sent as a link where the user may not
  // embed links: FakeNitro's notice asks before it goes.
  bool fake_ask;
  // the gif picker: the composer it opens over and the button that opens
  // it, the page it shows and the title of a page of results, what its
  // search holds and since when, and the players of the gifs in view.
  civue_rect gif_anchor, gif_button;
  uint8_t gif_page;
  char *gif_title;
  char *gif_search;
  double gif_typed_at;
  bool gif_focus;
  struct GifPlayers *gif_players;
  // the sticker picker: the composer it opens over and the button that
  // opens it, what its search holds, the sticker pointed at and the section
  // it is in, where the pointer last was, and the sections folded away, a
  // guild's by its id.
  civue_rect sticker_anchor, sticker_button;
  char *sticker_search;
  bool sticker_focus;
  Snowflake sticker_inspected, sticker_section;
  civue_vec2 sticker_mouse;
  Ids sticker_folded;
  // the section its rail jumps to once laid out, 0 for none, and the one
  // its rail last showed in view.
  Snowflake sticker_jump, sticker_active;
  // the sticker whose menu a right click opened, 0 for none, and where.
  Snowflake sticker_menu;
  civue_vec2 sticker_menu_at;
  // the sticker that goes with what is typed, 0 for none, and the channel
  // it was picked in.
  Snowflake sticker_pending, sticker_channel;
  // the sticker of a message whose card is open while the app shows
  // POPOVER_STICKER_INFO: its message, which of its stickers, and what was
  // pressed.
  Snowflake sticker_message;
  uint32_t sticker_index;
  civue_rect sticker_info_anchor;
  // the emoji picker: the composer it opens over and the button that opens
  // it, and what it shows, see emojis.c.
  civue_rect emoji_anchor, emoji_button;
  bool emoji_fresh;
  struct EmojiPicker *emojis;
  // the menu of a message open while the app shows POPOVER_COMPONENT_SELECT:
  // its message, what its application knows it by, where it was drawn, the
  // options ticked, a bit each, and whether the user ticked any.
  Snowflake select_message;
  char *select_custom;
  civue_rect select_anchor;
  uint32_t select_picked;
  bool select_changed;
  // the emoji of a message whose card is open while the app shows
  // POPOVER_EMOJI_CARD, and where it was drawn.
  Snowflake card_emoji;
  char *card_emoji_name;
  bool card_emoji_animated, card_emoji_fake;
  civue_rect card_emoji_at;

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
  // the add friend form: the username and the note typed, whether it may be
  // sent, whether its field had the focus last frame, and the challenge
  // discord asked for.
  char *friend_tag, *friend_note;
  bool friend_ready, friend_focused;
  hcaptcha *friend_captcha;

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

  // the voice channel asked for while in a call in another, 0 for none.
  Snowflake voice_switch;

  // the settings: the part of their page the sidebar points at, the kind
  // whose list of devices is open under its box, -1 for none, and that box.
  bool settings_open;
  // the page shown, a SettingsPage.
  uint8_t settings_page;
  uint8_t settings_part;
  int8_t settings_select;
  civue_rect settings_select_rect;

  // the directory shown: its tab, a DirectoryCategory or -1 for its home,
  // what its search field holds, and the words searched, null for none.
  int8_t directory_tab;
  char *directory_field;
  char *directory_query;
  // the guild of the card whose menu is open, and its button.
  Snowflake directory_entity;
  civue_rect directory_anchor;
  // a guild joined from a directory, opened once it arrives.
  Snowflake directory_join;

  // the servers anyone may find show in place of the friends: the tab, a
  // DiscoveryTab or the hubs after them, what the search field holds, and
  // the button its languages open under.
  bool exploring;
  uint8_t explore_tab;
  char *explore_field;
  civue_rect explore_anchor;
  // what the page lists, an ExploreMode, and what its applications keep,
  // see apps.c.
  uint8_t explore_mode;
  struct ExploreApps *explore_apps;
  // the dialog that lets an application in, see authorize.h.
  struct AuthorizeUi *authorize;

  // the hub's dialog, the directory it is about, and the guild it adds,
  // edits or removes, or the hub it leaves.
  HubDialog hub_dialog;
  Snowflake hub_channel, hub_entity;
  AddStep add_step;
  // the add dialog shows the servers already added, and the template the
  // new server starts from, -1 for none.
  bool add_listed;
  int8_t add_template;
  // the server added is made by the dialog.
  bool add_new;
  char *new_server_name;
  // what the entry says and its DirectoryCategory, 0 before one is picked,
  // and whether the list of categories is open.
  char *entry_description;
  uint8_t entry_category;
  bool category_open;
  // the student hub join: the email, the code, the school for the
  // waitlist, and the search over the schools.
  char *hub_email, *hub_code, *hub_school, *hub_search;
  // the nickname asked for in a hub just joined.
  char *nick;
  Snowflake nick_guild;

  // the spoilers the user clicked.
  CIVUE_VEC(civue_id) revealed;

  // the profile card open while the app shows POPOVER_PROFILE: whose, seen
  // from which guild, 0 for none, and by what it opened.
  Snowflake profile_user, profile_guild;
  civue_rect profile_anchor;
  ProfileSide profile_side;
  // bumped as a card or the full profile opens, for their animations, and
  // whether the card's menu or all its roles show.
  uint32_t profile_opens;
  bool profile_menu, profile_roles;
  // the full profile over the window, 0 for none, and its tab.
  Snowflake modal_user, modal_guild;
  ProfileTab modal_tab;
  // what the card's message box holds, and the note being written, null
  // while it is not.
  char *profile_message, *profile_note;
  Snowflake note_user;
  // the videos playing and the media open over the window, see media.h.
  struct Media *media;

  // the forum shown: the one what follows is about, what its search holds
  // and since when, the tags it filters by, and the order and the layout
  // the user chose over the forum's, -1 for its own.
  Snowflake forum_id;
  char *forum_search;
  double forum_typed_at;
  Ids forum_tags;
  int8_t forum_sort, forum_layout;
  bool forum_match_all;
  // the post being written in it, and the tags it will wear.
  bool post_open, post_focus;
  // posting was tried, so what it lacks is said.
  bool post_tried;
  char *post_title, *post_body;
  Ids post_tags;

  // a guild's settings and its channels being made or moved, see server.h.
  struct ServerUi *server;
  // the list that completes what the composer holds, see complete.h.
  struct Complete *complete;
} App;

// forgets what was shown and typed, for another account.
void app_reset(App *app);
void app_select_guild(App *app, Snowflake guild_id);
void app_select_channel(App *app, Snowflake channel_id);
Channel *app_channel(App *app);
Guild *app_guild(App *app);
[[gnu::pure]] bool ids_has(const Ids *ids, Snowflake id);
// adds `id`, or removes it when it is there.
void ids_toggle(Ids *ids, Snowflake id);

// whether a spoiler was pressed, and presses one.
[[gnu::pure]] bool app_revealed(const App *app, civue_id spoiler);
void app_reveal(App *app, civue_id spoiler);

static inline void app_toggle(App *app, Popover popover) {
  app->popover = app->popover == popover ? POPOVER_NONE : popover;
}

#endif
