#ifndef QUARREL_UI_WIDGETS_H
#define QUARREL_UI_WIDGETS_H

#include "app.h"

// an icon node `size` points square in `color`.
void glyph(civue_icon icon, float size, civue_color color);
// the image at `url` over the open node, nothing while it loads. false until
// it shows.
bool image_fill(App *app, const char *url, float radius);
// the same, the middle of the image cut to the node's shape.
bool image_cover(App *app, const char *url, float radius);
// a guild's sticker inside the open node, whole whatever its shape: every
// frame of one that moves when `animated`, its first one until they came
// and otherwise. false for a lottie one, which is no image, and until it
// shows.
bool sticker_fill(App *app, Snowflake sticker_id, uint8_t format, bool animated,
                  float opacity);
// an emoji as discord draws it, `size` points square: one of a guild when
// `emoji_id`, else the twemoji of `name`, its utf-8, or the system's glyph
// when discord has none.
void emoji_node(App *app, Snowflake emoji_id, const char *name, float size);
// a guild's emoji over `r`, every frame of one that moves while the window
// has the user's attention, as the client plays them. false until it shows.
bool emoji_image(App *app, Snowflake emoji_id, bool animated, civue_rect r);
// a round avatar node, with the user's status at its corner over `ring` when
// `status` is set.
void avatar(App *app, const User *user, Snowflake guild_id, float size,
            bool status, civue_color ring);
void status_dot(UserStatus status, civue_rect r, const Look *k,
                civue_color ring);
// a guild's tag with the image of its badge, none for null.
void guild_tag_pill(App *app, const char *tag, const char *badge);
// the tag of the guild the user wears, with its badge. nothing without one.
void tag_chip(App *app, const User *u);
// a red dot at the corner of the open node, cut out of `under`.
void mention_dot(const Look *k, civue_color under);
// the red count of mentions, nothing for 0.
void mention_badge(const Look *k, uint32_t count);
// every mention waiting in the store.
uint32_t mentions_of(Store *store);
// a triangle 10 wide and 5 deep from the edge of the tooltip `box` that
// faces `anchor`, as shadcn's tooltips point.
void tooltip_arrow(civue_rect box, civue_rect anchor, civue_tooltip_side side,
                   civue_color color);
// a label by what answered `resp` after a moment of hover.
void hint(App *app, civue_id id, const civue_response *resp, const char *label,
          civue_tooltip_side side);
// the open node moves the window when pressed where nothing of it answers,
// and zooms it on a double click.
void window_handle(civue_id id);
// room at the end of a page's header, the open node, for the window's buttons
// where they cut into it. its last child.
void header_end(App *app);
// a square button holding an icon, true when pressed.
bool icon_button(App *app, civue_id id, civue_icon icon, float size,
                 float icon_size, const char *label);
// a row of a menu, 40 points high, true when pressed.
bool menu_row(App *app, civue_id id, civue_icon icon, const char *label,
              civue_color color);
[[gnu::const]] civue_icon activity_icon(ActivityType type);
civue_field_look field_look(const Look *k);
civue_scrollbar_look scrollbars_of(App *app, float size);

// "14:32", "Today at 14:32" or "16/11/2026 14:32", and "16 November 2026".
void format_clock(int64_t ms, char *out, size_t cap);
void format_stamp(int64_t ms, char *out, size_t cap);
void format_day(int64_t ms, char *out, size_t cap);
// "Nov 20, 2026", as profiles write their dates.
void format_short_day(int64_t ms, char *out, size_t cap);
// the day `ms` falls on in the local time zone, to tell two apart.
int64_t day_of(int64_t ms);

typedef struct {
  // the guild names and role colors come from, 0 outside one.
  Snowflake guild_id;
  // the text's size, 16 when 0, and its color, the look's when clear.
  float size;
  civue_color color;
  // one paragraph of inline markup, for an embed's title.
  bool inline_only;
  // ends with the mark of an edited message.
  bool edited;
  // a message's own words: a link to an emoji's picture alone is shown as
  // the emoji, as FakeNitro does.
  bool fake_emojis;
  // the width laid out at until the node has one of its own.
  float hint;
} MarkupOptions;

// discord's markdown as a node as wide as what holds it, known by `key`.
void markup(App *app, civue_id key, const char *source, const MarkupOptions *o);

// where a card over the rest may go, 8 points in from the screen's edges
// or from the window's where it cannot leave it. lets this frame's overlays
// out of the window.
civue_rect overlay_room(void);

// what a file panel picked, absolute paths freed once `done` returns, none
// on cancel. `arg` is what the panel was opened with.
typedef void (*FilesPicked)(App *app, civue_strs files, uint64_t arg);
// a saved file's path for `done` to free, null on cancel.
typedef void (*FileSaved)(char *path, void *ctx);

// the system's panel picking files of `types`, any for none, opened once
// the frame is over: it draws the window while it waits, which a frame
// with nodes open cannot take. `types` must outlive the frame.
void pick_files_later(App *app, const char *const *types, uint32_t n,
                      bool multiple, FilesPicked done, uint64_t arg);
// the same with the panel saving a file named `name`.
void save_file_later(const char *name, FileSaved done, void *ctx);

// starts discord's challenge for `sitekey` unless `c` runs one already.
void captcha_start(Captcha *c, const char *sitekey, const char *rqdata);
void captcha_drop(Captcha *c);
// a spinner while the challenge loads, as the client's modal holds. an open
// challenge goes over the whole window, as hcaptcha's popup does on the web.
hcaptcha_state captcha_node(App *app, Captcha *c, civue_id key);
// the build has no web view to show a challenge in, which no retry mends.
[[gnu::pure]] bool captcha_unshowable(hcaptcha_state s);

#endif
