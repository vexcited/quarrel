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
// an emoji as discord draws it, `size` points square: one of a guild when
// `emoji_id`, else the twemoji of `name`, its utf-8, or the system's glyph
// when discord has none.
void emoji_node(App *app, Snowflake emoji_id, const char *name, float size);
// a round avatar node, with the user's status at its corner over `ring` when
// `status` is set.
void avatar(App *app, const User *user, Snowflake guild_id, float size,
            bool status, civue_color ring);
void status_dot(UserStatus status, civue_rect r, const Look *k,
                civue_color ring);
// the tag of the guild the user wears, with its badge. nothing without one.
void tag_chip(App *app, const User *u);
// the red count of mentions, nothing for 0.
void mention_badge(const Look *k, uint32_t count);
// every mention waiting in the store.
uint32_t mentions_of(Store *store);
// a label by what answered `resp` after a moment of hover.
void hint(App *app, civue_id id, const civue_response *resp, const char *label,
          civue_tooltip_side side);
// the open node moves the window when pressed where nothing of it answers,
// and zooms it on a double click.
void window_handle(civue_id id);
// a square button holding an icon, true when pressed.
bool icon_button(App *app, civue_id id, civue_icon icon, float size,
                 float icon_size, const char *label);
// a row of a menu, 40 points high, true when pressed.
bool menu_row(App *app, civue_id id, civue_icon icon, const char *label,
              civue_color color);
// a button of a dialog's foot, as wide as it may be, true when pressed. the
// primary one is the brand's.
bool dialog_button(const Look *k, civue_id id, const char *label,
                   bool primary);
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
  // the width laid out at until the node has one of its own.
  float hint;
} MarkupOptions;

// discord's markdown as a node as wide as what holds it, known by `key`.
void markup(App *app, civue_id key, const char *source, const MarkupOptions *o);

// where a card over the rest may go, 8 points in from the screen's edges
// or from the window's where it cannot leave it. lets this frame's overlays
// out of the window.
civue_rect overlay_room(void);

#endif
