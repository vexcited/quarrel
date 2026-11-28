#ifndef QUARREL_UI_SERVER_H
#define QUARREL_UI_SERVER_H

#include "discord/admin.h"
#include "forms.h"

/* ========================================================================== */
/* a guild's settings, and the making and moving of its channels, laid out    */
/* as discord's client has them.                                              */
/* ========================================================================== */

// what discord takes for a guild's images.
constexpr size_t IMAGE_MOST = 10u << 20;

// a file picked from the disk for an image, written to `*path` once the
// panel answers, `*removed` then false when not null.
void pick_image(App *app, char **path, bool *removed);

typedef enum : uint8_t {
  SERVER_PROFILE,
  SERVER_ENGAGEMENT,
  SERVER_BOOST,
  SERVER_EMOJI,
  SERVER_ROLES,
  SERVER_INVITES,
  SERVER_SAFETY,
  SERVER_BANS,
  SERVER_COMMUNITY,
  SERVER_PAGES,
} ServerPage;

typedef enum : uint8_t {
  ROLE_TAB_DISPLAY,
  ROLE_TAB_PERMISSIONS,
  ROLE_TAB_MEMBERS,
} RoleTab;

typedef enum : uint8_t {
  CONFIRM_NONE,
  CONFIRM_DELETE_ROLE,
  CONFIRM_REMOVE_MEMBER,
  CONFIRM_DELETE_CHANNEL,
  CONFIRM_DELETE_GUILD,
  CONFIRM_LEAVE_GUILD,
  CONFIRM_DISABLE_COMMUNITY,
  CONFIRM_DELETE_EMOJI,
  CONFIRM_SYNC_PERMISSIONS,
} Confirm;

// the steps of enabling community, the ones a guild meets already skipped.
typedef enum : uint8_t {
  COMMUNITY_INTRO,
  COMMUNITY_SAFETY,
  COMMUNITY_BASICS,
  COMMUNITY_FINISH,
  COMMUNITY_RULES,
  COMMUNITY_STEPS,
} CommunityStep;

// what the guild's own pages edit before it is saved.
typedef struct {
  // taken from the guild, which it is compared to, and the guild's version
  // it was taken at.
  bool taken;
  uint64_t stamp;
  char *name, *description, *locale;
  // a file picked to upload, null for none, and the image taken off.
  char *icon_path, *banner_path, *splash_path;
  bool icon_removed, banner_removed, splash_removed;
  Snowflake afk_channel_id, system_channel_id, rules_channel_id,
    updates_channel_id, safety_channel_id;
  uint32_t afk_timeout, system_flags;
  NotifyLevel notifications;
  bool progress_bar;
  // the features once community is disabled, which the save sends.
  bool community_off;
} GuildDraft;

// a role edited before it is saved.
typedef struct {
  Snowflake id;
  char *name;
  uint32_t color;
  bool hoist, mentionable;
  Permissions permissions;
} RoleDraft;

// a row of a list dragged to another place.
typedef struct {
  // what is dragged, 0 for nothing.
  Snowflake id;
  // the press it started from, which only becomes a drag once the pointer
  // went 4 points away.
  Snowflake pressed;
  civue_vec2 from;
} Drag;

typedef struct {
  bool open;
  Snowflake guild_id, parent_id;
  bool category;
  ChannelType type;
  char *name;
  bool private;
  // the second step of a private channel: who may see it.
  bool picking;
  Ids roles, users;
  char *search;
} CreateChannel;

typedef struct ServerUi {
  bool open;
  Snowflake guild_id;
  ServerPage page;
  Dropdown dropdown;
  Nag nag;
  GuildDraft draft;
  char *error;
  // the frames since the layer opened, for its spring.
  double opened_at;

  // the roles: the one edited, 0 for the list, and its tab.
  Snowflake role_id;
  RoleTab role_tab;
  // every role edited and the order the user dragged them to, highest
  // first, none until they did. both wait for the notice's save.
  RoleDraft *roles;
  size_t nroles;
  Snowflake *order;
  size_t norder;
  char *role_search, *perm_search, *member_search;
  Drag role_drag;
  // the role whose menu is open, and where.
  Snowflake role_menu;
  civue_rect role_menu_at;
  // the members picked to be given the role, and what their search holds.
  bool picker_open;
  Ids picks;
  char *picker_search, *picker_asked;

  // the dialog enabling community, the step it shows, and what it set.
  bool community_open;
  CommunityStep community_step;
  Snowflake community_rules, community_updates;
  bool community_agreed;

  Confirm confirm;
  Snowflake confirm_id;
  char *confirm_text;

  // the emoji being renamed, and its new name.
  Snowflake emoji_id;
  char *emoji_name;

  CreateChannel create;
  // the channel whose menu is open, where it opened, and the one edited.
  Snowflake menu_channel;
  civue_vec2 menu_at;
  bool menu_open;
  Snowflake renaming;
  char *rename;

  // a channel or a category dragged in the sidebar.
  Drag channel_drag;
  // a move that waits for the user to say whether to sync permissions,
  // and which of its places is the channel moved.
  ChannelPlace *pending;
  size_t npending, pending_moved;
} ServerUi;

// the app's, made on first use.
ServerUi *server_ui(App *app);
void server_ui_free(App *app);

// opens the settings of a guild at `page`.
void server_settings_open(App *app, Snowflake guild_id, ServerPage page);
// the settings over the whole window, while open.
void server_settings(App *app);
// something of these is open over the app, which typing must not reach.
bool server_covers(App *app);
// whether the user may open the guild's settings at all.
bool server_can_manage(App *app, const Guild *g);
// asks to leave the guild, from its menu.
void server_leave_ask(App *app, Snowflake guild_id);

// the page of roles, in roles.c.
void roles_page(App *app, ServerUi *ui, Guild *g);
// the dialogs of that page, drawn over the settings.
void roles_dialogs(App *app, ServerUi *ui, Guild *g);
void roles_forget(ServerUi *ui);
bool roles_dirty(const ServerUi *ui, const Guild *g);
void roles_save(App *app, ServerUi *ui, Guild *g);

// the dialogs making a channel or a category.
void channel_create_open(App *app, Snowflake guild_id, Snowflake parent_id,
                         bool category);
// every dialog and menu of channels.
void channel_dialogs(App *app);
// the menu of a channel or a category, opened by a right click on its row.
void channel_menu_open(App *app, Snowflake channel_id, civue_vec2 at);

// a row of the channel list as the sidebar lays it out, its rect for this
// frame. call for every row shown, in order, then channel_drag_end.
void channel_drag_row(App *app, Guild *g, Channel *c, civue_rect r,
                      const civue_response *press);
// drops what was dragged once the button is let go, and draws where it
// would go.
void channel_drag_end(App *app, Guild *g);
// a channel is being dragged: rows take no clicks meanwhile.
bool channel_dragging(App *app);
// how a row is drawn during a drag: 1, or dimmed for one the dragged
// channel cannot go by.
float channel_drag_alpha(App *app, const Channel *c);

#endif
