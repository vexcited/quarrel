#include "forms.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "widgets.h"

/* ========================================================================== */
/* buttons.                                                                   */
/* ========================================================================== */

static bool button_of(App *app, civue_id id, const char *label,
                      ButtonKind kind, bool disabled, bool small) {
  const Look *k = &app->look;
  bool pressed = false;
  civue_color bg = k->primary, over = k->primary_hover;
  civue_color down = k->primary_active, color = k->on_primary;
  civue_color edge = CIVUE_CLEAR;

  switch (kind) {
    case BUTTON_DANGER:
      bg = k->danger;
      over = k->danger_hover;
      down = k->danger_active;
      color = k->on_danger;
      break;
    case BUTTON_SECONDARY:
    case BUTTON_DANGER_SECONDARY:
      bg = k->secondary;
      over = down = k->secondary_hover;
      color = kind == BUTTON_SECONDARY ? k->strong : k->critical_text;
      break;
    case BUTTON_OUTLINE:
      bg = k->field;
      over = down = k->hover;
      edge = k->border_strong;
      color = k->strong;
      break;
    case BUTTON_LINK:
      bg = over = down = CIVUE_CLEAR;
      color = k->text;
      break;
    case BUTTON_PRIMARY:
      break;
  }

  // shadcn's: half as opaque, the same under the pointer.
  if (disabled) {
    bg = civue_alpha(bg, .5f);
    over = down = bg;
    color = civue_alpha(color, .5f);
    edge = civue_alpha(edge, .5f);
  }

  // shadcn's default and small sizes: 36 and 32 tall, 16 and 12 to the
  // sides, 14 point text.
  div(key(id), flex, items_center, justify_center, flex_shrink_0, rounded(lg),
      border(1), border_color(edge), bg(bg), hover(bg(over)),
      active(bg(down)), text_color(color), font_weight(medium),
      whitespace_nowrap,
      style(.h = civue_px(small ? 32 : 36),
            .padding[1] = civue_px(kind == BUTTON_LINK ? 4 : small ? 12 : 16),
            .padding[3] = civue_px(kind == BUTTON_LINK ? 4 : small ? 12 : 16),
            .text_size = civue_px(14),
            .line_height = civue_px(20),
            .cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    pressed = state.activated && !disabled;
    text(label, whitespace_nowrap);

    // a link is underlined while the pointer is over it.
    if (kind == BUTTON_LINK && state.response.hovered && !disabled) {
      civue_rect r = content();
      double y = (r.y0 + r.y1) / 2 + 9;

      fill(civue_rect_from(r.x0, y, r.x1, y + 1), color, 0);
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
  // shadcn's: the track is a field's when off, the thumb the text's.
  civue_color track = mix(k->selected, k->primary, at);
  civue_color thumb = mix(k->strong, k->on_primary, at);
  float fade = disabled ? .5f : 1;

  // shadcn's: 32 by 18, a thumb of 16.
  div(key(id), relative, flex_shrink_0, w(px(32)), h(px(18)), rounded(full),
      bg(civue_alpha(track, fade)),
      style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_button_state state = button();

    flipped = state.activated && !disabled;

    div(absolute, top(px(1)), left(px(1 + 14 * at)), size(px(16)),
        rounded(full), bg(civue_alpha(thumb, fade))) {}
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

  div(key(id), flex, items_center, justify_center, size(4), flex_shrink_0,
      rounded(px(4)), border(1),
      border_color(on ? k->primary : k->border_strong),
      bg(on ? k->primary : k->field), text_color(k->on_primary),
      cursor_pointer) {
    pressed = button().activated;

    if (on) {
      icon(lucide, check, size(px(14)));
    }
  }

  return pressed;
}

float dialog_width(void) {
  return fminf(DIALOG_WIDTH, (float)civue_ui_current()->size.x - 32);
}

void dialog_header(const Look *k, const char *title, const char *about) {
  div(flex, flex_col, gap(2), flex_shrink_0, min_w(0)) {
    text(title, text_size(px(18)), line_height(px(22)), font_weight(semibold),
         text_color(k->strong));

    if (about != nullptr) {
      text(about, text_size(px(14)), line_height(px(20)),
           text_color(k->subtle));
    }
  }
}

// the round mark of a choice, 16 across as shadcn's.
static void radio_mark(const Look *k, bool on, bool hovered) {
  div(flex, items_center, justify_center, size(4), flex_shrink_0, mt(px(2)),
      rounded(full), border(1),
      border_color(on        ? k->primary
                   : hovered ? k->ring
                             : k->border_strong),
      bg(k->field)) {
    if (on) {
      div(size(2), rounded(full), bg(k->primary)) {}
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

        text(title, text_size(px(14)), line_height(px(20)),
             font_weight(medium), text_color(civue_alpha(k->strong, fade)));
      }

      if (about != nullptr) {
        text(about, text_size(px(14)), line_height(px(20)),
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
  text(title, flex_shrink_0, mb(2), text_size(px(14)), line_height(px(20)),
       font_weight(medium), text_color(k->strong));
}

void form_note(const Look *k, const char *line) {
  text(line, flex_shrink_0, mt(2), text_size(px(14)), line_height(px(20)),
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
  div(flex_shrink_0, h(px(1)), my(10), bg(k->border)) {}
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
    .bg = k->field,
    .border = k->border_strong,
    .border_width = 1,
    .radius = 8,
    .text = k->text,
  };
  civue_field_box editing = idle;

  editing.border = k->ring;
  editing.border_width = 2;

  return (civue_field_look){
    .pad = pad,
    .editing = editing,
    .idle = idle,
    .hovered = idle,
    .placeholder = k->muted,
    .selection = k->selection,
  };
}

civue_field_response form_input(App *app, civue_id id, char **value,
                                const char *placeholder, const char *prefix,
                                size_t most) {
  const Look *k = &app->look;
  civue_field_response r = {};
  civue_field_look look = input_look(k, prefix != nullptr ? 32 : 12);

  // shadcn's: 36 tall, 12 to the sides, 14 point text.
  div(relative, flex, items_center, flex_shrink_0, h(9)) {
    div(key(id), flex_1, min_w(0), h(full), text_size(px(14)),
        line_height(px(20))) {
      r = text_field(value, placeholder, look);
    }

    if (prefix != nullptr) {
      text(prefix, absolute, left(px(12)), top(px(8)), text_size(px(14)),
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

  div(key(id), flex_shrink_0, text_size(px(14)), line_height(px(20)),
      style(.h = civue_px(16 + 20 * (float)lines))) {
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

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(9), px(3),
      rounded(lg), text_size(px(14)), line_height(px(20)),
      bg(civue_alpha(k->field, fade)),
      border(open ? 2 : 1),
      border_color(open ? k->ring
                        : civue_alpha(k->border_strong, fade)),
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
         text_color(civue_alpha(picked ? k->text : k->muted, fade)));

    if (open) {
      icon(lucide, chevron_up, size(4), flex_shrink_0,
           text_color(civue_alpha(k->subtle, fade)));
    }
    else {
      icon(lucide, chevron_down, size(4), flex_shrink_0,
           text_color(civue_alpha(k->subtle, fade)));
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
  // shadcn's rows: 6 above and under a line of 20.
  float row = 32;
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
        p(1), rounded(lg), bg(k->popover), border(1),
        border_color(k->border), shadow(md), overflow_y_scroll,
        text_size(px(14)), line_height(px(20)),
        scrollbars(&app->thin_bars)) {
      list = here();
      button();

      for (size_t i = 0; i < d->count; i++) {
        bool on = (int)i == d->current;

        div(key(id_at(id_in(d->open, "option"), i)), flex, items_center,
            gap(2), flex_shrink_0, h(px(row)), px(2), rounded(md),
            hover(bg(k->hover)),
            cursor_pointer) {
          if (button().activated) {
            chosen = (int)i;
          }

          text(d->names[i], flex_1, min_w(0), truncate,
               text_color(on ? k->strong : k->text));

          if (on) {
            icon(lucide, check, size(4), flex_shrink_0, text_color(k->strong));
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
          bg(mix(k->popover, mix(k->popover, k->danger, .3f), flush)),
          border(1), border_color(mix(k->border_strong, k->critical_text, flush)),
          shadow(lg),
          style(.text_color = civue_alpha(mix(k->strong, k->critical_text,
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
                                BUTTON_PRIMARY, saving)) {
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
        bg(k->scrim)) {
      if (button().activated && !busy) {
        answer = -1;
      }

      div(key(id), flex, flex_col, gap(4), w(px(dialog_width())), p(6),
          rounded(px(DIALOG_RADIUS)), bg(k->dialog), border(1),
          border_color(k->border), shadow(lg), text_color(k->text)) {
        button();

        dialog_header(k, title, body);

        if (error != nullptr) {
          text(error, text_size(px(14)), line_height(px(20)),
               text_color(k->critical_text));
        }

        div(flex, justify_end, gap(2), flex_shrink_0) {
          if (form_button(app, id_in(id, "no"), "Cancel", BUTTON_OUTLINE,
                          busy)) {
            answer = -1;
          }

          if (form_button(app, id_in(id, "yes"), busy ? "Working..." : yes,
                          danger ? BUTTON_DANGER : BUTTON_PRIMARY, busy)) {
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
