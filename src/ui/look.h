#ifndef QUARREL_UI_LOOK_H
#define QUARREL_UI_LOOK_H

#include <civue/civue.h>

// discord's dark theme, the tokens of its stylesheets resolved.
typedef struct {
  civue_color frame;
  civue_color frame_border;
  civue_color chat;
  civue_color panel;
  civue_color surface;
  civue_color surface_high;
  civue_color input;
  civue_color text;
  civue_color strong;
  civue_color subtle;
  civue_color muted;
  civue_color faint;
  civue_color link;
  civue_color brand;
  civue_color brand_hover;
  civue_color brand_active;
  civue_color brand_text;
  civue_color danger;
  civue_color critical_text;
  civue_color positive;
  civue_color positive_text;
  civue_color online;
  civue_color idle;
  civue_color dnd;
  civue_color offline;
  civue_color hover;
  civue_color selected;
  civue_color pressed;
  civue_color border;
  civue_color border_strong;
  civue_color message_hover;
  civue_color mentioned;
  civue_color mentioned_bar;
  civue_color mention_bg;
  civue_color mention_text;
  civue_color code_bg;
  civue_color spoiler;
  civue_color thumb;
} Look;

Look look_dark();

#endif
