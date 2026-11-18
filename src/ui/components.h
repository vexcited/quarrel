#ifndef QUARREL_UI_COMPONENTS_H
#define QUARREL_UI_COMPONENTS_H

#include "app.h"

void login_screen(App *app);
void connecting_screen(App *app);

void top_bar(App *app);
// under it: the guilds' switcher, then the way to the friends or what the
// open guild offers.
void place_bar(App *app);
// the guild's icon or its acronym over the open node, `size` points square.
void guild_icon(App *app, const Guild *guild, float size, bool lit);
// the guilds under `anchor`, while the app shows POPOVER_GUILDS. a press on
// `anchor` is left to what opened it.
void guild_switcher(App *app, civue_rect anchor);
// a name for a new guild, under `anchor`, while the app shows
// POPOVER_NEW_GUILD.
void new_guild(App *app, civue_rect anchor);
// the guild's banner, which scrolls with its channels. nothing when it has
// none.
void guild_banner(App *app, const Guild *guild);
// what can be done with the open guild, under `anchor`, while the app shows
// POPOVER_GUILD_MENU.
void guild_menu(App *app, civue_rect anchor);
// a link that invites to the open guild, under `anchor`, while the app
// shows POPOVER_INVITE.
void invite_card(App *app, civue_rect anchor);
// the friends to start a conversation with, under `anchor`, while the app
// shows POPOVER_NEW_DM.
void new_dm(App *app, civue_rect anchor);
void channel_list(App *app, Guild *guild);
// who is in the voice channel, under its row. nothing for an empty one.
void voice_members(App *app, const Channel *c);
// the call the account is in, at the foot of the sidebar: how it goes and
// the microphone, the sound and the way out. nothing outside a call.
void voice_panel(App *app);
// the call of a voice channel in place of its chat: who is in it, and the
// microphone, the camera and the way out under them.
void voice_stage(App *app, Channel *c);
// takes the machine's devices into `app->devices`, as a menu of them opens.
void devices_list(App *app);
// the device in use of a kind in `app->devices`, -1 for the system's own.
int device_picked(const App *app, AudioKind kind);
// a slider from 0 to `most` over a node of its own, its new value when it
// moved. `let_go` once the drag ended.
float volume_slider(App *app, civue_id id, float value, float most,
                    bool *let_go);
// how loud `level` is, 0 to 1, as `bars` bars over the open node.
void level_bars(App *app, float level, int bars);
// the settings over the whole window, while `app->settings_open`.
void settings_dialog(App *app);
void private_list(App *app);
// the saved accounts, the status and the way out under `anchor`, while
// the app shows POPOVER_ACCOUNTS. a press on `anchor` is left to what opened
// it.
void account_switcher(App *app, civue_rect anchor);
// the mentions waiting in the accounts that are not shown.
uint32_t accounts_waiting(App *app);
// what waits to be read and what mentions the user, under `anchor`, while
// the app shows POPOVER_INBOX. a press on `anchor` is left to what opened it.
void inbox(App *app, civue_rect anchor);
// the saved accounts under the login form, nothing when there is none.
void account_choices(App *app);

void chat(App *app, Channel *channel);
void message_list(App *app, Channel *channel);
void composer(App *app, Channel *channel);

// an emoji to react with: one of a guild by its id and name, or one of
// unicode, `name` its utf-8 and `id` 0.
typedef struct {
  Snowflake id;
  const char *name;
} ReactionPick;

// the emojis the user reacts with the most that the channel can use, the
// most used first. none until discord said which.
size_t quick_reactions(App *app, const Channel *c, ReactionPick *out,
                       size_t max);
// adds the reaction, or takes it back when the user already gave it.
void react_with(App *app, const Message *m, ReactionPick pick);
// opens `popover` for the message by `anchor`, or closes it when it is the
// one open.
void message_popover(App *app, Popover popover, const Message *m,
                     civue_rect anchor);
bool message_popover_open(const App *app, const Message *m);
// what is open for a message of the channel: POPOVER_MESSAGE_MENU,
// POPOVER_REACT or POPOVER_FORWARD.
void message_popovers(App *app, Channel *c);
// puts the message's text in the composer to be edited.
void app_edit_message(App *app, const Message *m);
void member_list(App *app, Channel *channel);
void friends_page(App *app);

#endif
