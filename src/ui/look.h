#ifndef QUARREL_UI_LOOK_H
#define QUARREL_UI_LOOK_H

#include <civue/civue.h>

typedef enum : uint8_t {
  // shadcn's neutral dark theme.
  THEME_MONOCHROME,
  // discord's dark theme, the tokens of its stylesheets resolved.
  THEME_DISCORD,
  THEMES,
} Theme;

// the colors the whole app is drawn with. a theme changes these alone, the
// shapes stay.
typedef struct {
  // the window's ground and the sidebar on it, the edge of the page beside
  // it, and the page.
  civue_color frame;
  civue_color frame_border;
  civue_color chat;
  // what stands on the page, from the lowest to the highest.
  civue_color panel;
  civue_color surface;
  civue_color surface_high;
  // menus, pickers and cards over the rest.
  civue_color popover;
  // a dialog over the darkened window.
  civue_color dialog;
  // the composer's ground.
  civue_color input;
  // the ground of a field, a box or a switch that is off.
  civue_color field;
  // what darkens the window under a dialog.
  civue_color scrim;
  civue_color tooltip;
  civue_color on_tooltip;

  civue_color strong;
  civue_color text;
  civue_color subtle;
  civue_color muted;
  civue_color faint;
  // what is muted or turned off, still to be read.
  civue_color dim;
  civue_color link;

  // the main action, and what is written over it.
  civue_color primary;
  civue_color primary_hover;
  civue_color primary_active;
  civue_color on_primary;
  // a lesser action.
  civue_color secondary;
  civue_color secondary_hover;
  // text that stands out where the main action's ground would be too much.
  civue_color accent_text;

  civue_color danger;
  civue_color danger_hover;
  civue_color danger_active;
  civue_color on_danger;
  civue_color critical_text;
  civue_color positive;
  civue_color positive_hover;
  civue_color positive_active;
  civue_color on_positive;
  civue_color positive_text;
  civue_color warning;

  civue_color online;
  civue_color idle;
  civue_color dnd;
  civue_color offline;

  civue_color hover;
  civue_color selected;
  civue_color pressed;
  civue_color border;
  civue_color border_strong;
  // the edge of what has the focus, and selected text.
  civue_color ring;
  civue_color selection;
  // an opaque line: quotes, the spine of a reply, dividers.
  civue_color rule;
  // where what is dragged lands.
  civue_color drop;
  // a favorite.
  civue_color star;
  // the banner of a server or an account that has none.
  civue_color banner;

  civue_color message_hover;
  civue_color mentioned;
  civue_color mentioned_bar;
  civue_color mention_bg;
  civue_color mention_text;
  civue_color code_bg;
  civue_color spoiler;
  civue_color thumb;
} Look;

extern const char *const THEME_NAMES[THEMES];

Look look_of(Theme theme);
// the theme kept in the preferences, the monochrome one when none is.
Theme theme_saved(void);
void theme_save(Theme theme);

#endif
