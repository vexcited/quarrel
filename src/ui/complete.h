#ifndef QUARREL_UI_COMPLETE_H
#define QUARREL_UI_COMPLETE_H

#include "app.h"

/* ========================================================================== */
/* the list over the composer that completes what is being typed: who to     */
/* mention after an @, a channel after a #, and after a / at the start a     */
/* command, then its options and what they take. a kind of completion is a   */
/* source, which finds its token before the caret and lists what completes   */
/* it. the list takes the arrows, tab, enter and escape while it shows.      */
/* ========================================================================== */

// what a row shows before its label.
typedef enum : uint8_t {
  COMPLETE_ICON_NONE,
  // the avatar of the user `id`.
  COMPLETE_ICON_USER,
  // an emoji: a guild's by `id`, else the unicode one `name` is the utf-8
  // of.
  COMPLETE_ICON_EMOJI,
  // `glyph` in `color`.
  COMPLETE_ICON_GLYPH,
  // the icon of the application `id`, `name` its hash.
  COMPLETE_ICON_APPLICATION,
} CompleteIcon;

typedef struct {
  CompleteIcon icon;
  Snowflake id;
  const char *name;
  civue_icon glyph;
  // the label's and the glyph's color, the look's when clear.
  civue_color color;
  // `label` is never null. `detail` is dimmer, at the row's right, null for
  // none.
  const char *label, *detail;
  // what the token becomes when the row is picked.
  const char *insert;
} CompleteRow;

typedef struct CompleteList CompleteList;

// a title over the rows added after it, like "MEMBERS".
void complete_title(CompleteList *list, const char *title);
// every string is copied.
void complete_add(CompleteList *list, const CompleteRow *row);

typedef struct {
  // the byte its token starts at in `text` when the caret, at the byte
  // `caret`, is in one, -1 otherwise. the token runs from there to the caret.
  int32_t (*find)(App *app, const Channel *c, const char *text, uint32_t caret);
  // lists what completes `token`, the whole of it, its first character
  // included.
  void (*offer)(App *app, Channel *c, const char *token, CompleteList *list);
} CompleteSource;

// a key of the composer the list takes instead: the arrows, tab, enter and
// escape while it shows rows. true when it did.
bool complete_key(App *app, Channel *c, civue_key_press press);
// the list over `box`, the composer's, once its field ran.
void complete_popup(App *app, Channel *c, civue_rect box);
// runs what the composer holds when it is a command. true when it was one,
// run or held back by what one of its options lacks.
bool complete_submit(App *app, Channel *c);
// what the composer holds as discord takes it: the names typed after an @
// or a # that are someone's or a channel's turn into their mentions. from
// malloc.
[[gnu::malloc, gnu::returns_nonnull]] char *
complete_encode(App *app, const Channel *c, const char *text);
void complete_free(App *app);

/* ========================================================================== */
/* mentions.c: who and what an @ and a # name, for the list and the field.    */
/* ========================================================================== */

// a list of its own, and its rows moved to the end of another.
CompleteList *complete_list_new(void);
void complete_list_take(CompleteList *list, CompleteList *from);

// `in` in lower case without its accents, as names are compared.
void mention_fold(const char *in, char *out, size_t cap);
// every character of `query` is in `text`, in its order.
[[gnu::pure]] bool mention_fuzzy(const char *query, const char *text);
// how well `name` answers `query`, already folded, as the client scores
// channels and choices: 10 for the same, 7 for a name it starts, 5 for one
// that holds it, 3 for one that holds its words, 1 for its letters in their
// order, 0 for none.
int mention_rank(const char *name, const char *query);
// `text` is one mention and nothing else.
[[gnu::pure]] bool mention_whole(const char *text);
// where the word under the caret starts when `opener` starts it, a
// parenthesis before it aside. -1 otherwise.
[[gnu::pure]] int32_t mention_token(const char *text, uint32_t caret,
                                    char opener);

// the sources of the mentions and of the channels.
int32_t mention_find(App *app, const Channel *c, const char *text,
                     uint32_t caret);
void mention_offer(App *app, Channel *c, const char *token, CompleteList *list);
int32_t channel_find(App *app, const Channel *c, const char *text,
                     uint32_t caret);
void channel_offer(App *app, Channel *c, const char *token, CompleteList *list);
// the users of the channel, then its guild's roles, that `typed` names, at
// most `max` of each. every role when `any`, else the ones the user may
// mention. how many were listed.
size_t mention_users(App *app, Channel *c, const char *typed,
                     CompleteList *list, size_t max);
size_t mention_roles(App *app, Channel *c, const char *typed,
                     CompleteList *list, size_t max, bool any);
// the channels an option of a command takes, of `types` when it names some.
void mention_channels(App *app, Channel *c, const char *typed,
                      CompleteList *list, const uint8_t *types, size_t ntypes);
// who or what `typed` names for an option: a mention, an id, or a name
// after its @ or its #. 0 for nothing the store knows.
Snowflake mention_user_named(App *app, const Channel *c, const char *typed);
Snowflake mention_role_named(App *app, const Channel *c, const char *typed);
Snowflake mention_channel_named(App *app, const Channel *c, const char *typed);
// asks discord for the members of the name last typed once it rested. once
// a frame.
void mention_ask_members(App *app);

// the source of the emojis, in emojis.c.
int32_t emoji_complete_find(App *app, const Channel *c, const char *text,
                            uint32_t caret);
void emoji_complete_fill(App *app, Channel *c, const char *token,
                         CompleteList *list);

// the mentions the composer shows as one chip each, for its field's inlays:
// how many bytes of `text` from `at` are one, 0 for none, and how wide its
// chip is.
uint32_t mention_inlay_find(App *app, const char *text, uint32_t len,
                            uint32_t at, float *width);
// draws the chip of the mention these bytes are over `r`.
void mention_inlay_paint(App *app, civue_ui *ui, const char *text, uint32_t len,
                         civue_rect r);

#endif
