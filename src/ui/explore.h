#ifndef QUARREL_UI_EXPLORE_H
#define QUARREL_UI_EXPLORE_H

#include "app.h"

/* ========================================================================== */
/* what the explore page shares between its servers, in explore.c, and its   */
/* applications, in apps.c.                                                   */
/* ========================================================================== */

// what the page lists.
typedef enum : uint8_t {
  EXPLORE_SERVERS,
  EXPLORE_APPS,
  EXPLORE_HUBS,
} ExploreMode;

// what the header's search field asked for.
typedef enum : uint8_t {
  EXPLORE_SEARCH_NONE,
  EXPLORE_SEARCH_SUBMIT,
  EXPLORE_SEARCH_CLEAR,
} ExploreSearch;

// discord's sizes for its discovery.
constexpr float EXPLORE_GUTTER = 16;
constexpr float EXPLORE_PAGE_MAX = 1300;

// "1,234", as discord writes counts.
void explore_count(uint32_t n, char *out, size_t cap);
// dots turning in a ring, while something loads.
void explore_spinner(civue_color color, float size);
// something went wrong in place of a list, with the way to ask again when
// `retry` is set. true when it was pressed.
bool explore_trouble(App *app, civue_id id, const char *why, bool retry);

// how wide the grid `list` was last frame, kept by explore_keep_width from
// inside its node, and how many cards at least `least` wide fit a row of
// it.
float explore_width(civue_id list);
uint16_t explore_columns(civue_id list, float least);
void explore_keep_width(civue_id list);

// the header's search field over `*field`, with the way to clear it while
// it holds words or `active` is set. enter over words submits them, enter
// over nothing clears.
ExploreSearch explore_search(App *app, civue_id id, char **field, bool active);
// a choice of the header's row, with how many it holds when `count` is
// not negative. true when pressed while off.
bool explore_pill(App *app, civue_id id, const char *label, int64_t count,
                  bool on);
// the row the pills go in, filling the header's room and scrolled sideways
// when they need more.
#define explore_pills(app, row)                                        \
  div(key(row), flex, items_center, gap(1), flex_1, min_w(0), h(full), \
      overflow_x_scroll, overflow_y_hidden, scrollbars(&(app)->thin_bars))
void explore_heading(const Look *k, const char *label);
// a search found nothing, in `category` when not null. true when the way
// back to every category was pressed.
bool explore_nothing(App *app, civue_id id, const char *category);
// opens a server: one the account is in shows, any other is previewed, as
// from the page `from` names.
void explore_open_guild(App *app, Snowflake id, PreviewFrom from);

// the applications, header and all, and what they keep.
void apps_page(App *app);
void apps_ui_free(App *app);

#endif
