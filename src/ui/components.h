#ifndef QUARREL_UI_COMPONENTS_H
#define QUARREL_UI_COMPONENTS_H

#include "app.h"

void login_screen(App *app);
void connecting_screen(App *app);

void top_bar(App *app);
// under it: the guilds' switcher, then the way to the friends or what the
// open guild offers.
void place_bar(App *app);
// the first letter of each word, what discord shows for a guild without an
// icon.
void guild_acronym(const char *name, char *out, size_t cap);
// the guild's icon or its acronym over the open node, `size` points square.
void guild_icon(App *app, const Guild *guild, float size, bool lit);
// the guilds under `anchor`, while the app shows POPOVER_GUILDS. a press on
// `anchor` is left to what opened it.
void guild_switcher(App *app, civue_rect anchor);
// the dialog creating or joining a server while the app shows
// POPOVER_NEW_GUILD.
void new_guild(App *app);
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
// joins a voice channel, after asking when that leaves a call and the
// user wants to be asked.
void voice_join(App *app, Snowflake channel_id);
// asks whether to leave the call for `app->voice_switch`, while it is set.
void voice_switch_dialog(App *app);
// the calls that ring any account, each with who calls and a button to
// decline and one to answer, over whatever the window shows.
void call_cards(App *app);
// the call of a voice channel in place of its chat: who is in it, and the
// microphone, the camera and the way out under them.
void voice_stage(App *app, Channel *c);
// takes the machine's devices into `app->devices`, as a menu of them opens.
void devices_list(App *app);
// takes them again once one came or went. for every frame that shows them.
void devices_follow(App *app);
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
// what the session last told of something the user did, at the foot of the
// window for a moment.
void notice_toast(App *app);
// the saved accounts under the login form, nothing when there is none.
void account_choices(App *app);

void chat(App *app, Channel *channel);
// the posts of a forum or a media channel, and the way to write one.
void forum_page(App *app, Channel *forum);
// the tags a post wears, as a row of small pills, `most` of them and a
// count of the others. nothing when it wears none.
void post_tags(App *app, const Channel *post, size_t most);
// a tag as a pill, small or not.
void tag_pill(App *app, const ForumTag *tag, bool small);
// what can be done with a post, under `anchor`, while the app shows
// POPOVER_POST_MENU.
void post_menu(App *app, Channel *post, civue_rect anchor);
void message_list(App *app, Channel *channel);
// a message of `c` alone, under its author and without its toolbar, in a
// node `width` points wide.
void message_preview(App *app, const Channel *c, const Message *m,
                     float width);
// what a message that is a line of its own says after its author's name,
// `other` being who it is about. `out` or words of its own.
const char *message_system_text(const Message *m, char *out, size_t cap,
                                const char *other);
void composer(App *app, Channel *channel);
// a button of the composer's row in a node known by `id`, lit while what
// it opens is `open`. `label` hints at it unless null, `at` takes its place
// unless null.
civue_button_state composer_button(App *app, civue_id id, civue_icon icon,
                                   bool open, const char *label,
                                   civue_rect *at);
// the gifs to send to the channel over the composer, while the app shows
// POPOVER_GIFS. lets go of their players once it does not.
void gif_picker(App *app, Channel *channel);
// opens the picker over `anchor`, the composer, or closes it.
void gif_picker_toggle(App *app, civue_rect anchor);
// opens it on what `query` finds, as the gif command does.
void gif_picker_search(App *app, civue_rect anchor, const char *query);
// lets go of the players of the gifs no frame drew lately. once a frame.
void gif_players_sweep(App *app);
void gif_players_free(App *app);
// the stickers of the user's guilds to send to the channel over the
// composer, while the app shows POPOVER_STICKERS.
void sticker_picker(App *app, Channel *channel);
// opens the picker over `anchor`, the composer, or closes it.
void sticker_picker_toggle(App *app, civue_rect anchor);
// the composer's button that opens it.
void sticker_button(App *app, civue_rect box);
// the sticker that goes with the channel's next message, 0 for none.
Snowflake sticker_pending(App *app, const Channel *c);
// `content` with a link to the sticker after it, from malloc, where discord
// would refuse the sticker itself: Vencord's FakeNitro. null for one that
// goes as it is.
char *sticker_linked(App *app, const Channel *c, Snowflake sticker_id,
                     const char *content);
// it over the composer's field, with the way to take it back. nothing
// without one.
void sticker_tray(App *app, const Channel *c);
// a sticker of a message in a node known by `id`, moving or still, which
// opens its card when pressed if it `opens` one.
void sticker_in_message(App *app, civue_id id, const Message *m, size_t index,
                        const Sticker *sticker, bool moving, bool opens);
// the card of the sticker pressed in a message of the channel, while the
// app shows POPOVER_STICKER_INFO.
void sticker_info(App *app, Channel *c);
// adds `text` at the end of what the composer holds, a space between.
void composer_append(App *app, const char *text);
// puts `text` where the composer's caret is, at its end when it has none.
void composer_insert(App *app, const char *text);

// an emoji picked: one of a guild by its id and name, or one of unicode,
// `name` its utf-8 and `id` 0.
typedef struct {
  Snowflake id;
  const char *name;
  bool animated;
} ReactionPick;

// what an emoji is picked for, which decides the ones used the most.
typedef enum : uint8_t {
  EMOJI_FOR_CHAT,
  EMOJI_FOR_REACTION,
} EmojiUse;

// the client's picker without its tabs.
constexpr float EMOJI_CARD_WIDTH = 498;
constexpr float EMOJI_CARD_HEIGHT = 440;

// the emojis of the user's guilds and of unicode as a card over `card`,
// with a search, the guilds on its side and what is pointed at under them.
// true when one was picked, `keep` when the card is to stay for another.
// `fresh` on the first frame it is open for something, to start anew.
bool emoji_card(App *app, const Channel *c, EmojiUse use, bool fresh,
                civue_rect card, ReactionPick *out, bool *keep);
// the picker over the composer, while the app shows POPOVER_EMOJIS.
void emoji_picker(App *app, Channel *channel);
// opens it over `anchor`, the composer, or closes it.
void emoji_picker_toggle(App *app, civue_rect anchor);
void emoji_picker_free(App *app);
// what a key of the user's settings is where `c` is, when the user may use
// it there: the first name of an emoji of unicode or the id of a guild's.
bool emoji_pick_by_key(App *app, const Channel *c, const char *key,
                       ReactionPick *out);
// what the composer shows in place of the emojis its text names.
const civue_field_inlays *composer_inlays(App *app, Channel *c);
// an emoji of a message was pressed: its card opens by `at`. `fake` for
// one that is a link to its picture, as FakeNitro sends them.
void emoji_card_open(App *app, Snowflake id, const char *name, bool animated,
                     bool fake, civue_rect at);
// the card of the emoji of a message, while the app shows
// POPOVER_EMOJI_CARD.
void emoji_info_card(App *app, Channel *channel);

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
[[gnu::pure]] bool message_popover_open(const App *app, const Message *m);
// the menu of a right click at `at`, with what this frame put under it.
void message_menu_at(App *app, const Message *m, civue_vec2 at);
// puts the words selected in the channel's messages on the clipboard.
void selection_copy(App *app, const Channel *c);
// what is open for a message of the channel: POPOVER_MESSAGE_MENU,
// POPOVER_REACT or POPOVER_FORWARD.
void message_popovers(App *app, Channel *c);
// what an application built the message with, its cards, texts, pictures,
// buttons and menus, under the open node in `room` points. `id` is what
// their keys hang from. nothing for a message without any.
void message_components(App *app, const Message *m, Snowflake guild_id,
                        civue_id id, float room);
// the options of the menu pressed in a message of the channel, while the
// app shows POPOVER_COMPONENT_SELECT.
void component_select(App *app, Channel *channel);
// a file as a card: its name, which opens it, and its size.
void file_card(App *app, civue_id id, const Attachment *a);
// puts the message's text in the composer to be edited.
void app_edit_message(App *app, const Message *m);
void member_list(App *app, Channel *channel);

// opens the profile card of a user by `anchor`, seen from a guild, 0 for
// none, or closes it when it is the one open.
void profile_open(App *app, Snowflake user_id, Snowflake guild_id,
                  civue_rect anchor, ProfileSide side);
// a press on the open node opens the user's profile card by it. true when
// it did.
bool profile_trigger(App *app, Snowflake user_id, Snowflake guild_id,
                     ProfileSide side);
// the same for a name, underlined while pointed at, the card opening on its
// right.
bool profile_name_trigger(App *app, Snowflake user_id, Snowflake guild_id);
// the card of `app->profile_user` while the app shows POPOVER_PROFILE.
void profile_popout(App *app);
// the full profile of `app->modal_user` over the whole window.
void profile_modal(App *app);
// a hub's ways in over its channels: joining the servers it lists, adding
// one and inviting people.
void hub_rows(App *app, Guild *guild);
// the servers a directory channel lists, in place of a chat.
void directory_page(App *app, Channel *channel);
// the dialog a hub has open over the window, see `app->hub_dialog`.
void hub_dialogs(App *app);
// asks for the email that joins a school's hub.
void hub_join_open(App *app);
// the blue that fades from the left of discord's discovery banners, over
// `r`.
void discovery_gradient(const Look *k, civue_rect r);

// the servers anyone may find, in place of the friends while the app is
// `exploring`.
void explore_page(App *app);
// the ways around it, in place of the conversations in the sidebar.
void explore_nav(App *app);
// shows them.
void explore_open(App *app);
// opens the server asked for from the page once it came, and stops looking
// at the ones left. for every frame of a ready session.
void explore_frame(App *app);
// what says the open guild is only looked at, with the way to join it.
// nothing for a guild the user is in.
void preview_bar(App *app);
// asks for the name the user goes by in a hub.
void hub_nick_open(App *app, Snowflake guild_id);
// asks before leaving a hub.
void hub_leave_open(App *app, Snowflake guild_id);
void friends_page(App *app);

// the composer's files: the plus and its menu, the tray over the field, the
// zone files are dropped on, and the dialogs about them.
void upload_button(App *app, Channel *c);
void upload_tray(App *app, Channel *c);
void upload_drop_zone(App *app, Channel *c);
void upload_dialogs(App *app, Channel *c);
// takes files, a picture or a long text from the clipboard into the tray.
// false when it holds none of these, for the field to paste.
bool upload_paste(App *app, Channel *c);
// how a pending message's files go up, under it.
void upload_progress(App *app, Channel *c, const Message *m);
// longer than discord takes from the user.
bool upload_message_too_long(App *app, const char *draft);
void upload_clear_draft(App *app);

#endif
