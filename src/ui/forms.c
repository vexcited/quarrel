#include "forms.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "widgets.h"

// discord's tokens these controls are drawn with, the dark theme's.
constexpr uint32_t INPUT_BG = 0x0000001f;
constexpr uint32_t BUTTON_EDGE = 0xffffff14;
constexpr uint32_t NOTICE_CRITICAL = 0x4f000f;
constexpr uint32_t NOTICE_CRITICAL_TEXT = 0xffc0bd;

/* ========================================================================== */
/* buttons.                                                                   */
/* ========================================================================== */

static bool button_of(App *app, civue_id id, const char *label,
                      ButtonKind kind, bool disabled, bool small) {
  const Look *k = &app->look;
  bool pressed = false;
  civue_color bg = k->brand, over = k->brand_hover, down = k->brand_active;
  civue_color color = CIVUE_WHITE, edge = rgba(BUTTON_EDGE);

  switch (kind) {
    case BUTTON_DANGER:
      bg = k->danger;
      over = rgb(0xa9232e);
      down = rgb(0x971d28);
      break;
    case BUTTON_GREEN:
      bg = k->positive;
      over = rgb(0x006c37);
      down = rgb(0x005f30);
      break;
    case BUTTON_SECONDARY:
    case BUTTON_DANGER_SECONDARY:
      bg = rgba(0x97979f1f);
      over = rgba(0x97979f29);
      down = rgba(0x97979f33);
      edge = rgba(0x97979f0a);
      color = kind == BUTTON_SECONDARY ? k->strong : k->critical_text;
      break;
    case BUTTON_LINK:
      bg = over = down = edge = CIVUE_CLEAR;
      color = k->text;
      break;
    case BUTTON_BRAND:
      break;
  }

  if (disabled) {
    bg = civue_alpha(bg, bg.a * .5f);
    over = down = bg;
    edge = civue_alpha(edge, edge.a * .5f);
    color = civue_alpha(color, .5f);
  }

  div(key(id), flex, items_center, justify_center, flex_shrink_0, border(1),
      border_color(edge), rounded(lg), bg(bg), hover(bg(over)),
      active(bg(down)), text_color(color), font_weight(medium),
      style(.h = civue_px(small ? 32 : 40),
            .min_w = civue_px(kind == BUTTON_LINK ? 0 : small ? 60 : 100),
            .padding[1] = civue_px(kind == BUTTON_LINK ? 4 : small ? 11 : 15),
            .padding[3] = civue_px(kind == BUTTON_LINK ? 4 : small ? 11 : 15),
            .text_size = civue_px(small ? 14 : 16),
            .line_height = civue_px(18),
            .cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    pressed = state.activated && !disabled;
    text(label, whitespace_nowrap);

    // a link is underlined while the pointer is over it.
    if (kind == BUTTON_LINK && state.response.hovered && !disabled) {
      civue_rect r = content();
      double y = (r.y0 + r.y1) / 2 + 9;

      fill(civue_rect_from(r.x0, y, r.x1, y + 1), k->strong, 0);
    }
  }

  return pressed;
}

bool form_button(App *app, civue_id id, const char *label, ButtonKind kind,
                 bool disabled) {
  return button_of(app, id, label, kind, disabled, false);
}

bool form_button_small(App *app, civue_id id, const char *label,
                       ButtonKind kind, bool disabled) {
  return button_of(app, id, label, kind, disabled, true);
}

/* ========================================================================== */
/* choices.                                                                   */
/* ========================================================================== */

static civue_color mix(civue_color a, civue_color b, float t) {
  return (civue_color){
    a.r + (b.r - a.r) * t,
    a.g + (b.g - a.g) * t,
    a.b + (b.b - a.b) * t,
    a.a + (b.a - a.a) * t,
  };
}

bool form_switch(App *app, civue_id id, bool on, bool disabled) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool flipped = false;
  // the thumb slides over 300 milliseconds, as discord's does.
  float at = civue_animate(ui, id_in(id, "thumb"), on ? 1 : 0, .3f);
  civue_color track = mix(rgba(0x0000001f), k->brand, at);
  civue_color edge = mix(rgba(0x97979f33), rgba(0x97979f1f), at);
  float fade = disabled ? .5f : 1;

  div(key(id), relative, flex_shrink_0, w(px(48)), h(px(24)), rounded(full),
      border(1), border_color(civue_alpha(edge, edge.a * fade)),
      bg(civue_alpha(track, track.a * fade)),
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    flipped = state.activated && !disabled;

    if (state.response.hovered && !disabled) {
      civue_rect r = here();

      civue_stroke(ui, r, at > .5f ? rgba(0x97979f1f) : rgba(0x97979f66), 1,
                   12);
    }

    div(absolute, top(px(3)), left(px(3 + 24 * at)), size(px(16)),
        rounded(full), bg(civue_alpha(CIVUE_WHITE, fade))) {}
  }

  return flipped;
}

bool form_switch_row(App *app, civue_id id, const char *title,
                     const char *about, bool on, bool disabled) {
  const Look *k = &app->look;
  bool flipped = false;

  div(key(id_in(id, "row")), flex, items_start, gap(6), flex_shrink_0,
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    // the label flips it as well.
    flipped = button().activated && !disabled;

    div(flex, flex_col, gap(1), flex_1, min_w(0),
        style(.text_color = civue_alpha(k->strong, disabled ? .5f : 1),
              .has_text_color = true)) {
      text(title, font_weight(medium));

      if (about != nullptr) {
        text(about, text_size(px(14)), line_height(px(18)),
             text_color(civue_alpha(k->subtle, disabled ? .5f : 1)));
      }
    }

    flipped |= form_switch(app, id, on, disabled);
  }

  return flipped;
}

bool form_checkbox(App *app, civue_id id, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, size(5), flex_shrink_0,
      rounded(md), border(on ? 0 : 2), border_color(rgba(0x97979fa3)),
      style(.bg = on ? k->brand : CIVUE_CLEAR), text_color(CIVUE_WHITE),
      cursor_pointer) {
    pressed = button().activated;

    if (on) {
      icon(lucide, check, size(px(14)));
    }
  }

  return pressed;
}

// the round mark of a choice, 20 across.
static void radio_mark(const Look *k, bool on, bool hovered) {
  div(flex, items_center, justify_center, size(5), flex_shrink_0,
      rounded(full), border(2),
      border_color(on        ? k->brand
                   : hovered ? rgba(0x97979fcc)
                             : rgba(0x97979fa3)),
      style(.bg = on ? k->brand : CIVUE_CLEAR)) {
    if (on) {
      div(size(px(8)), rounded(full), bg(CIVUE_WHITE)) {}
    }
  }
}

bool form_radio(App *app, civue_id id, const civue_icon *glyph_icon,
                const char *title, const char *about, bool on,
                bool disabled) {
  const Look *k = &app->look;
  bool pressed = false;
  float fade = disabled ? .5f : 1;

  div(key(id), flex, items_start, gap(3), flex_shrink_0, rounded(lg),
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    pressed = state.activated && !disabled && !on;
    radio_mark(k, on, state.response.hovered && !disabled);

    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      div(flex, items_center, gap(1)) {
        if (glyph_icon != nullptr) {
          glyph(*glyph_icon, 20, civue_alpha(k->strong, fade));
        }

        text(title, text_color(civue_alpha(k->strong, fade)));
      }

      if (about != nullptr) {
        text(about, text_size(px(14)), line_height(px(18)),
             text_color(civue_alpha(k->subtle, fade)));
      }
    }
  }

  return pressed;
}

/* ========================================================================== */
/* text.                                                                      */
/* ========================================================================== */

void form_legend(const Look *k, const char *title, const char *about) {
  text(title, flex_shrink_0, text_size(px(20)), line_height(px(24)),
       font_weight(medium), text_color(k->strong));

  if (about != nullptr) {
    text(about, flex_shrink_0, mt(1), text_size(px(14)), line_height(px(18)),
         text_color(k->text));
  }
}

void form_label(const Look *k, const char *title) {
  text(title, flex_shrink_0, mb(2), font_weight(medium), text_color(k->strong));
}

void form_note(const Look *k, const char *line) {
  text(line, flex_shrink_0, mt(2), text_size(px(14)), line_height(px(18)),
       text_color(k->subtle));
}

void form_heading(const Look *k, const char *title, const char *about) {
  text(title, flex_shrink_0, text_size(px(20)), line_height(px(24)),
       font_weight(semibold), text_color(k->strong));

  if (about != nullptr) {
    text(about, flex_shrink_0, mt(2), text_size(px(14)), line_height(px(18)),
         font_weight(medium), text_color(k->text));
  }
}

void form_divider(const Look *k) {
  (void)k;
  div(flex_shrink_0, h(px(1)), my(10), bg(rgba(0x97979f70))) {}
}

// cuts `*value` to `most` bytes, never in the middle of a character.
static void clamp(char **value, size_t most) {
  if (most == 0 || *value == nullptr || strlen(*value) <= most) {
    return;
  }

  size_t n = most;

  while (n > 0 && ((unsigned char)(*value)[n] & 0xc0) == 0x80) {
    n--;
  }

  (*value)[n] = 0;
}

static civue_field_look input_look(const Look *k, float pad) {
  civue_field_box idle = {
    .bg = rgba(INPUT_BG),
    .border = k->border_strong,
    .border_width = 1,
    .radius = 8,
    .text = k->text,
  };
  civue_field_box editing = idle;

  editing.border = k->brand;
  editing.border_width = 2;

  return (civue_field_look){
    .pad = pad,
    .editing = editing,
    .idle = idle,
    .hovered = idle,
    .placeholder = rgb(0x8f9097),
    .selection = civue_alpha(k->brand, .45f),
  };
}

civue_field_response form_input(App *app, civue_id id, char **value,
                                const char *placeholder, const char *prefix,
                                size_t most) {
  const Look *k = &app->look;
  civue_field_response r = {};
  civue_field_look look = input_look(k, prefix != nullptr ? 32 : 12);

  div(relative, flex, items_center, flex_shrink_0, h(10)) {
    div(key(id), flex_1, min_w(0), h(full), text_size(px(16)),
        line_height(px(20))) {
      r = text_field(value, placeholder, look);
    }

    if (prefix != nullptr) {
      text(prefix, absolute, left(px(12)), top(px(10)), text_size(px(16)),
           line_height(px(20)), text_color(k->subtle));
    }
  }

  clamp(value, most);
  return r;
}

void form_input_set(civue_id id, char **value, const char *text) {
  civue_ui *ui = civue_ui_current();
  size_t len = *value != nullptr ? strlen(*value) : 0;
  char *edited = civue_field_replace(ui, id, 0, (uint32_t)len, text);

  free(*value);
  *value = edited != nullptr ? edited : strdup(text);
}

civue_field_response form_area(App *app, civue_id id, char **value,
                               const char *placeholder, int lines,
                               size_t most) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_field_response r = {};

  div(key(id), flex_shrink_0, text_size(px(16)), line_height(px(20)),
      style(.h = civue_px(24 + 20 * (float)lines))) {
    r = civue_text_area(ui, id, here(), value, placeholder,
                        civue_node_text_style(ui), input_look(k, 12));
  }

  clamp(value, most);
  return r;
}

/* ========================================================================== */
/* dropdowns.                                                                 */
/* ========================================================================== */

void form_dropdown_close(Dropdown *d) {
  for (size_t i = 0; i < d->count; i++) {
    free(d->names[i]);
  }

  free(d->names);
  d->names = nullptr;
  d->count = 0;
  d->open = 0;
}

int form_select(App *app, Dropdown *d, civue_id id, const char *const *names,
                size_t count, int current, const char *placeholder,
                bool disabled) {
  const Look *k = &app->look;
  bool open = d->open == id;
  int chosen = -1;
  bool picked = current >= 0 && (size_t)current < count;
  const char *shown = picked ? names[current] : placeholder;
  float fade = disabled ? .5f : 1;

  if (d->chosen_for == id) {
    chosen = d->chosen;
    d->chosen_for = 0;
  }

  div(key(id), flex, items_center, gap(1), flex_shrink_0, h(10), pl(3),
      pr(2), rounded(lg), bg(civue_alpha(rgba(INPUT_BG), fade * .12f)),
      border(open ? 2 : 1),
      border_color(open ? k->brand
                        : civue_alpha(k->border_strong, .2f * fade)),
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (open) {
      d->box = here();
    }

    if (button().activated && !disabled) {
      form_dropdown_close(d);

      if (!open) {
        d->open = id;
        d->box = here();
        d->current = current;
        d->names = calloc(count ? count : 1, sizeof *d->names);
        d->count = count;

        for (size_t i = 0; i < count; i++) {
          d->names[i] = strdup(names[i]);
        }
      }
    }

    text(shown != nullptr ? shown : "", flex_1, min_w(0), truncate,
         text_color(civue_alpha(picked ? k->text : rgb(0x8f9097), fade)));

    if (open) {
      icon(lucide, chevron_up, size(5), flex_shrink_0,
           text_color(civue_alpha(k->strong, fade)));
    }
    else {
      icon(lucide, chevron_down, size(5), flex_shrink_0,
           text_color(civue_alpha(k->strong, fade)));
    }
  }

  return chosen;
}

void form_dropdowns(App *app, Dropdown *d) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect list = {};
  int chosen = -1;

  if (d->open == 0) {
    return;
  }

  civue_rect box = d->box;
  float row = 40;
  float most = fminf(ui->size.y * .5f, 10 + row * 8);
  float height = fminf(10 + row * (float)d->count, most);
  float top = (float)box.y1 + 4;

  // over the box when the window ends under it.
  if (top + height > ui->size.y - 8) {
    top = fmaxf((float)box.y0 - 4 - height, 8);
  }

  overlay() {
    div(key(id_in(d->open, "list")), flex, flex_col, left(px(box.x0)),
        top(px(top)), w(px(box.x1 - box.x0)), style(.h = civue_px(height)),
        p(1), rounded(lg), bg(rgb(0x28282d)), border(1),
        border_color(k->border), shadow(lg), overflow_y_scroll,
        scrollbars(&app->thin_bars)) {
      list = here();
      button();

      for (size_t i = 0; i < d->count; i++) {
        bool on = (int)i == d->current;

        div(key(id_at(id_in(d->open, "option"), i)), flex, items_center,
            gap(2), flex_shrink_0, h(px(row - 2)), px(3), rounded(md),
            style(.bg = on ? k->hover : CIVUE_CLEAR), hover(bg(k->hover)),
            cursor_pointer) {
          if (button().activated) {
            chosen = (int)i;
          }

          text(d->names[i], flex_1, min_w(0), truncate,
               text_color(on ? k->strong : k->text));

          if (on) {
            icon(lucide, check, size(5), flex_shrink_0, text_color(k->brand));
          }
        }
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(list, ui->input.mouse) &&
                 !civue_rect_contains(box, ui->input.mouse);

  if (chosen >= 0) {
    d->chosen_for = d->open;
    d->chosen = chosen;
    civue_window_wake();
  }

  if (chosen >= 0 || outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    form_dropdown_close(d);
  }
}

/* ========================================================================== */
/* the notice of unsaved changes.                                             */
/* ========================================================================== */

void form_nag(Nag *n) {
  civue_ui *ui = civue_ui_current();

  if (n->intensity < 1.4f) {
    n->intensity = 1.4f;
  }

  n->nagged_at = ui->now;
  // discord's shakes 2 pixels harder each time, 15 at most.
  n->intensity = fminf(n->intensity + 2, 15);
  civue_request_repaint(ui);
}

void form_nag_reset(Nag *n) {
  n->nagged_at = 0;
  n->intensity = 1.4f;
}

civue_vec2 form_shake(const Nag *n) {
  civue_ui *ui = civue_ui_current();
  double since = ui->now - n->nagged_at;

  if (n->nagged_at == 0 || since >= .3) {
    return (civue_vec2){};
  }

  // every other frame it swaps sides, and jumps up or down at random.
  float strength = n->intensity - 2;
  int beat = (int)(since * 30);
  float x = (beat % 2 ? 1 : -1) * strength;
  float y = (float)((beat * 7919 % 200) - 100) / 100 * strength;

  civue_request_repaint(ui);
  return civue_v2(x, y);
}

NoticeAction form_notice(App *app, civue_rect region, bool shown, bool saving,
                         const char *error, const Nag *n) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  NoticeAction action = NOTICE_NONE;
  // it springs up from under the page, a little past its place.
  float rise = civue_animate(ui, id("notice-rise"), shown ? 1 : 0, .35f);
  bool red = (n->nagged_at > 0 && ui->now - n->nagged_at < 1) ||
             (error != nullptr && !saving);
  float flush = civue_animate(ui, id("notice-red"), red ? 1 : 0, .2f);

  if (rise <= .01f) {
    return NOTICE_NONE;
  }

  if (red && n->nagged_at > 0 && ui->now - n->nagged_at < 1) {
    civue_request_repaint_after(ui, 1 - (ui->now - n->nagged_at));
  }

  float width = fminf((float)(region.x1 - region.x0) - 40, 700);
  float left = (float)region.x0 + 20;
  float bottom = (float)region.y1 - 20;
  float top = bottom - 54 + (1 - rise) * 54 * 1.5f;

  overlay() {
    clipped(region) {
      div(key(id("notice")), flex, items_center, gap(3), left(px(left)),
          top(px(top)), w(px(width)), pl(4), pr(px(10)), py(px(10)),
          rounded(lg),
          bg(mix(rgb(0x2c2d32), rgb(NOTICE_CRITICAL), flush)), border(1),
          border_color(mix(k->border, k->critical_text, flush)), shadow(lg),
          style(.text_color = civue_alpha(mix(CIVUE_WHITE,
                                              rgb(NOTICE_CRITICAL_TEXT),
                                              flush),
                                          rise),
                .has_text_color = true)) {
        button();

        text(error != nullptr ? error
             : saving         ? "Saving your changes..."
                              : "Careful, you have unsaved changes!",
             flex_1, min_w(0), truncate, font_weight(medium));

        div(flex, items_center, gap(px(10)), flex_shrink_0) {
          if (form_button_small(app, id("notice-reset"), "Reset", BUTTON_LINK,
                                saving)) {
            action = NOTICE_RESET;
          }

          if (form_button_small(app, id("notice-save"),
                                saving ? "Saving..." : "Save Changes",
                                BUTTON_GREEN, saving)) {
            action = NOTICE_SAVE;
          }
        }
      }
    }
  }

  return action;
}

/* ========================================================================== */
/* dialogs.                                                                   */
/* ========================================================================== */

int form_confirm(App *app, civue_id id, const char *title, const char *body,
                 const char *yes, bool danger, bool busy, const char *error) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  int answer = 0;

  overlay() {
    div(key(id_in(id, "shade")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(rgba(0x000000b3))) {
      if (button().activated && !busy) {
        answer = -1;
      }

      div(key(id), flex, flex_col, w(px(440)), rounded(xl), bg(k->surface),
          border(1), border_color(k->border), shadow(lg), text_color(k->text),
          overflow_hidden) {
        button();

        div(flex, flex_col, gap(3), px(6), pt(6), pb(4)) {
          text(title, text_size(px(20)), line_height(px(24)),
               font_weight(semibold), text_color(k->strong));
          text(body, text_color(k->subtle));

          if (error != nullptr) {
            text(error, text_size(px(14)), line_height(px(18)),
                 text_color(k->critical_text));
          }
        }

        div(flex, justify_end, gap(2), flex_shrink_0, px(6), pt(2), pb(4)) {
          if (form_button(app, id_in(id, "no"), "Cancel", BUTTON_SECONDARY,
                          busy)) {
            answer = -1;
          }

          if (form_button(app, id_in(id, "yes"), busy ? "Working..." : yes,
                          danger ? BUTTON_DANGER : BUTTON_BRAND, busy)) {
            answer = 1;
          }
        }
      }
    }
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) && !busy) {
    answer = -1;
  }

  return answer;
}
