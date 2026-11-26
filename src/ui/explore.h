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
} ExploreMode;

// discord's sizes for its discovery.
constexpr float EXPLORE_GUTTER = 16;
constexpr float EXPLORE_PAGE_MAX = 1300;
constexpr float EXPLORE_FILTERS_WIDTH = 186;

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

// the header's choice between the servers and the applications.
void explore_modes(App *app);
// the header's search field over `*field`. true when enter was pressed
// over words.
bool explore_search(App *app, civue_id id, char **field);
// the banner of a tab, its title in capitals over one line or two.
void explore_banner(const Look *k, const char *title, const char *second,
                    const char *about);
// a tab under the banner, true when pressed. call inside explore_tabs.
bool explore_tab(App *app, civue_id id, const char *label, bool on);
void explore_heading(const Look *k, const char *label);
// a category a search may keep to and how many it found there. true when
// pressed.
bool explore_filter(App *app, civue_id id, const char *name, uint32_t count,
                    bool on);
// a search found nothing, in `category` when not null. true when the way
// back to every category was pressed.
bool explore_nothing(App *app, civue_id id, const char *category);
// opens a server: one the account is in shows, any other is previewed, as
// from the page `from` names.
void explore_open_guild(App *app, Snowflake id, PreviewFrom from);


#endif
