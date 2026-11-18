#include "components.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "voice/devices.h"
#include "voice/voice.h"
#include "widgets.h"

constexpr float TILE_GAP = 8;
constexpr size_t TILES_MAX = 25;
constexpr float MENU_WIDTH = 288;
constexpr float SIDE_WIDTH = 280;
constexpr float SIDE_ROW = 40;
constexpr int METER_BARS = 28;

/* ========================================================================== */
/* who is in the call.                                                        */
/* ========================================================================== */

static void tile(App *app, const Channel *c, const VoiceState *v,
                 civue_rect at) {
  const Look *k = &app->look;
  const Session *s = app->session;
  Store *store = s->store;
  float height = (float)(at.y1 - at.y0);
  float face = fminf(fmaxf(height * 0.3f, 32), 96);
  bool speaking = s->voice && s->voice_channel_id == c->id &&
                  voice_speaking(s->voice, v->user_id);

  div(key(id_at(id("tile"), v->user_id)), absolute, left(px(at.x0)),
      top(px(at.y0)), w(px(at.x1 - at.x0)), h(px(height)), flex, items_center,
      justify_center, rounded(xl), bg(k->surface), overflow_hidden) {
    civue_rect r = here();

    avatar(app, store_user(store, v->user_id), c->guild_id, face, false,
           k->surface);

    div(absolute, left(2), bottom(2), flex, items_center, gap(1p5), h(7), px(2),
        rounded(lg), bg(rgba(0x000000b3)), max_w(2_3)) {
      if (v->deafened) {
        glyph(CIVUE_ICON(lucide, headphone_off), 14, k->danger);
      }
      else if (v->muted) {
        glyph(CIVUE_ICON(lucide, mic_off), 14, k->danger);
      }

      text(store_display_name(store, c->guild_id, v->user_id), min_w(0),
           truncate, text_size(px(14)), line_height(px(18)),
           font_weight(medium), text_color(CIVUE_WHITE));
    }

    if (speaking) {
      stroke(r, k->positive, 2, 12);
    }
  }
}

// the tiles as a grid of 16 by 9 ones, as large as the room allows.
static void tiles(App *app, const Channel *c, civue_rect room) {
  const Store *store = app->session->store;
  const VoiceState *shown[TILES_MAX];
  size_t count = 0;

  for (size_t i = 0; i < store->nvoice_states && count < TILES_MAX; i++) {
    if (store->voice_states[i].channel_id == c->id) {
      shown[count++] = &store->voice_states[i];
    }
  }

  float width = (float)(room.x1 - room.x0), height = (float)(room.y1 - room.y0);

  if (!count || width <= 0 || height <= 0) {
    return;
  }

  // the number of columns that leaves the tiles the largest.
  size_t columns = 1, rows = count;
  float tile_width = 0;

  for (size_t n = 1; n <= count; n++) {
    size_t lines = (count + n - 1) / n;
    float fits =
      fminf((width - TILE_GAP * (float)(n - 1)) / (float)n,
            (height - TILE_GAP * (float)(lines - 1)) / (float)lines * 16 / 9);

    if (fits > tile_width) {
      tile_width = fits;
      columns = n;
      rows = lines;
    }
  }

  tile_width = floorf(tile_width);

  float tile_height = floorf(tile_width * 9 / 16);
  float top =
    (height - (float)rows * tile_height - TILE_GAP * (float)(rows - 1)) / 2;

  for (size_t i = 0; i < count; i++) {
    size_t row = i / columns;
    // the last row is centered when it is not full.
    size_t held = row == rows - 1 ? count - row * columns : columns;
    float left =
      (width - (float)held * tile_width - TILE_GAP * (float)(held - 1)) / 2;
    float x = left + (float)(i % columns) * (tile_width + TILE_GAP);
    float y = top + (float)row * (tile_height + TILE_GAP);

    tile(app, c, shown[i], xywh(x, y, tile_width, tile_height));
  }
}

/* ========================================================================== */
/* the menus: which microphone, which speakers, which camera.                 */
/* ========================================================================== */

void devices_list(App *app) {
  for (int kind = 0; kind < 3; kind++) {
    audio_devices_free(app->devices[kind], app->ndevices[kind]);
    app->ndevices[kind] =
      audio_devices((AudioKind)kind, &app->devices[kind]);
    audio_default_name((AudioKind)kind, app->default_names[kind],
                       sizeof app->default_names[kind]);
  }

  app->voice_side = -1;
}

int device_picked(const App *app, AudioKind kind) {
  const char *id = devices_get(kind);

  for (size_t i = 0; i < app->ndevices[kind]; i++) {
    const AudioDevice *d = &app->devices[kind][i];

    // a camera is always one of the list: the system's when none is picked.
    if (id ? strcmp(d->id, id) == 0
           : kind == AUDIO_CAMERAS &&
               strcmp(d->name, app->default_names[kind]) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static void separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}
}

// a row that opens the list of a kind beside the menu.
static void device_row(App *app, AudioKind kind, const char *label) {
  const Look *k = &app->look;
  int current = device_picked(app, kind);
  bool open = app->voice_side == (int8_t)kind;
  const char *name =
    current >= 0 ? app->devices[kind][current].name : "macOS Default";

  div(key(id_at(id("device-row"), kind)), flex, items_center, gap(2),
      flex_shrink_0, h(px(52)), px(2), rounded(lg),
      style(.bg = open ? k->hover : CIVUE_CLEAR), hover(bg(k->hover)),
      cursor_pointer) {
    civue_button_state state = button();

    if (state.activated || state.response.hovered) {
      app->voice_side = (int8_t)kind;
    }

    if (open) {
      app->voice_side_row = here();
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      text(label, truncate, font_weight(medium), text_color(k->strong));
      text(name, truncate, text_size(px(12)), line_height(px(16)),
           text_color(k->subtle));
    }

    icon(lucide, chevron_right, size(4), flex_shrink_0, text_color(k->subtle));
  }
}

static bool radio_row(App *app, civue_id id, const char *name,
                      const char *under, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(px(SIDE_ROW)),
      px(2), rounded(lg), hover(bg(k->hover)), cursor_pointer) {
    pressed = button().activated;

    div(flex, flex_col, flex_1, min_w(0)) {
      text(name, truncate, font_weight(medium), text_color(k->strong));

      if (under && *under) {
        text(under, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }
    }

    div(flex, items_center, justify_center, size(5), flex_shrink_0,
        rounded(full), border(2),
        border_color(on ? k->brand : k->border_strong)) {
      if (on) {
        div(size(px(10)), rounded(full), bg(k->brand)) {}
      }
    }
  }

  return pressed;
}

// the devices of the kind open, beside `card`. its rect, empty when none
// is open.
static civue_rect side_list(App *app, civue_rect card) {
  const Look *k = &app->look;
  civue_rect room = overlay_room();
  civue_rect list = {};

  if (app->voice_side < 0) {
    return list;
  }

  AudioKind kind = (AudioKind)app->voice_side;
  bool standard = kind != AUDIO_CAMERAS;
  size_t count = app->ndevices[kind];
  int current = device_picked(app, kind);
  float height = 18 + SIDE_ROW * (float)(count + (standard ? 1 : 0));
  float left = (float)card.x1 + 4;
  float top = fminf((float)app->voice_side_row.y0 - 8,
                    (float)room.y1 - height);

  // on the menu's other side when the screen ends there.
  if (left + SIDE_WIDTH > room.x1) {
    left = (float)card.x0 - 4 - SIDE_WIDTH;
  }

  overlay() {
    div(key(id("device-list")), flex, flex_col, left(px(left)), top(px(top)),
        w(px(SIDE_WIDTH)), p(2), rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), text_size(px(14)),
        line_height(px(18))) {
      list = here();

      if (standard &&
          radio_row(app, id("device-default"), "macOS Default",
                    app->default_names[kind], current < 0)) {
        devices_set(kind, nullptr);
      }

      for (size_t i = 0; i < count; i++) {
        const AudioDevice *d = &app->devices[kind][i];

        if (radio_row(app, id_at(id("device"), i), d->name, nullptr,
                      current == (int)i)) {
          devices_set(kind, d->id);
        }
      }
    }
  }

  return list;
}

float volume_slider(App *app, civue_id id, float value, float most,
                           bool *let_go) {
  const Look *k = &app->look;

  div(key(id), flex_shrink_0, h(6), mx(2), px(2)) {
    civue_slider_state state = slider(value, 0, most, 0, most / 100);

    if (state.changed) {
      value = (float)state.value;
    }

    *let_go |= state.ended;

    civue_rect track = content();
    double mid = (track.y0 + track.y1) / 2;
    double x = track.x0 + value / most * (track.x1 - track.x0);

    fill(civue_rect_from(track.x0, mid - 2, track.x1, mid + 2),
         k->border_strong, 2);
    fill(civue_rect_from(track.x0, mid - 2, x, mid + 2), k->brand, 2);
    fill(civue_rect_from(x - 8, mid - 8, x + 8, mid + 8), CIVUE_WHITE, 8);
  }

  return value;
}

static void heading(const Look *k, const char *label) {
  text(label, flex_shrink_0, px(2), pt(2), pb(1), font_weight(medium),
       text_color(k->strong));
}

void level_bars(App *app, float level, int bars) {
  const Look *k = &app->look;
  civue_rect r = here();
  // on a scale of decibels: 60 of them.
  float decibels = level > 0 ? 20 * log10f(level) : -100;
  int lit = (int)((decibels + 60) / 60 * (float)bars);
  double step = (r.x1 - r.x0) / bars;

  for (int i = 0; i < bars; i++) {
    fill(civue_rect_from(r.x0 + i * step, r.y0, r.x0 + (i + 1) * step - 2,
                         r.y1),
         i < lit ? k->positive : k->border_strong, 2);
  }
}

static void level_meter(App *app) {
  const Session *s = app->session;

  div(flex_shrink_0, h(4), my(1), mx(2)) {
    level_bars(app, s->voice ? voice_input_level(s->voice) : 0, METER_BARS);
  }

  // it moves without anything else changing.
  civue_window_wake();
}

static void dismiss(App *app, civue_rect card, civue_rect list,
                    civue_rect anchor) {
  civue_ui *ui = civue_ui_current();
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(list, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}

// the microphone's menu over `anchor`: the devices, how loud, and deafen.
static void sound_menu(App *app, civue_rect anchor) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  civue_rect card = {};
  bool let_go = false, deafen = false;

  if (app->popover != POPOVER_VOICE_SOUND) {
    return;
  }

  overlay_room();

  overlay() {
    div(key(id("sound-menu")), flex, flex_col, left(px(anchor.x0)),
        bottom(px(ui->size.y - anchor.y0 + 8)), w(px(MENU_WIDTH)), p(2),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), text_size(px(14)),
        line_height(px(18))) {
      card = here();

      device_row(app, AUDIO_INPUTS, "Input Device");
      device_row(app, AUDIO_OUTPUTS, "Output Device");
      separator(k);

      heading(k, "Input Volume");
      devices_set_input_volume(volume_slider(
        app, id("input-volume"), devices_input_volume(), 1, &let_go));
      heading(k, "Input Level");
      level_meter(app);
      heading(k, "Output Volume");
      devices_set_output_volume(volume_slider(
        app, id("output-volume"), devices_output_volume(), 2, &let_go));
      separator(k);

      div(key(id("deafen-row")), flex, items_center, gap(2), flex_shrink_0,
          h(10), px(2), rounded(lg), hover(bg(k->hover)), cursor_pointer) {
        deafen = button().activated;

        text("Deafen", flex_1, min_w(0), font_weight(medium),
             text_color(k->strong));

        div(flex, items_center, justify_center, size(5), flex_shrink_0,
            rounded(md), border(2),
            border_color(s->voice_deaf ? k->brand : k->border_strong),
            style(.bg = s->voice_deaf ? k->brand : CIVUE_CLEAR),
            text_color(CIVUE_WHITE)) {
          if (s->voice_deaf) {
            icon(lucide, check, size(px(14)));
          }
        }
      }
    }
  }

  if (let_go) {
    devices_save_volumes();
  }

  if (deafen) {
    session_voice_deafen(s, !s->voice_deaf);
  }

  dismiss(app, card, side_list(app, card), anchor);
}

static void camera_menu(App *app, civue_rect anchor) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect card = {};

  if (app->popover != POPOVER_VOICE_CAMERA) {
    return;
  }

  overlay_room();

  overlay() {
    div(key(id("camera-menu")), flex, flex_col, left(px(anchor.x0)),
        bottom(px(ui->size.y - anchor.y0 + 8)), w(px(MENU_WIDTH)), p(2),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), text_size(px(14)),
        line_height(px(18))) {
      card = here();

      device_row(app, AUDIO_CAMERAS, "Camera");
    }
  }

  dismiss(app, card, side_list(app, card), anchor);
}

/* ========================================================================== */
/* the bar under the tiles.                                                   */
/* ========================================================================== */

static bool bar_button(App *app, civue_id id, civue_icon icon, float width,
                       float icon_size, civue_color color, const char *label,
                       bool quiet) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, w(px(width)),
      h(10), rounded(lg), hover(bg(k->hover)), active(bg(k->pressed)),
      cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    glyph(icon, icon_size, color);

    if (label && !quiet) {
      hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
    }
  }

  return pressed;
}

// a button and the arrow that opens its menu, as one. true when the
// button was pressed, the pair's rect in `anchor`.
static bool bar_pair(App *app, civue_id id, civue_icon icon, civue_color color,
                     const char *label, Popover popover, civue_rect *anchor) {
  const Look *k = &app->look;
  bool open = app->popover == popover;
  bool pressed = false;

  div(key(id), flex, items_center, flex_shrink_0, p(1), rounded(xl),
      bg(k->surface), border(1), border_color(k->border)) {
    *anchor = here();
    pressed = bar_button(app, id_at(id, 1), icon, 40, 20, color, label, open);

    civue_icon arrow = open ? CIVUE_ICON(lucide, chevron_up)
                            : CIVUE_ICON(lucide, chevron_down);

    if (bar_button(app, id_at(id, 2), arrow, 24, 16, k->subtle, nullptr,
                   true)) {
      app_toggle(app, popover);

      if (app->popover == popover) {
        devices_list(app);
      }
    }
  }

  return pressed;
}

static void controls(App *app, const Channel *c) {
  const Look *k = &app->look;
  Session *s = app->session;
  Store *store = s->store;
  civue_rect sound = {}, camera = {};
  bool here_now = s->voice_channel_id == c->id;

  div(flex, items_center, justify_center, gap(2), flex_shrink_0, pb(4),
      text_size(px(14)), line_height(px(18))) {
    if (!here_now) {
      if (store_can(store, c, PERM_CONNECT)) {
        div(key(id("stage-join")), flex, items_center, gap(2), h(10), px(4),
            rounded(xl), bg(k->brand), hover(bg(k->brand_hover)),
            active(bg(k->brand_active)), text_color(CIVUE_WHITE),
            cursor_pointer) {
          if (button().activated) {
            session_voice_join(s, c->id);
          }

          icon(lucide, phone, size(px(18)));
          text("Join Voice", font_weight(semibold));
        }
      }
      else {
        text("You may not join this channel.", text_color(k->faint));
      }
    }
    else {
      bool muted = s->voice_mute || s->voice_deaf;
      civue_icon microphone =
        muted ? CIVUE_ICON(lucide, mic_off) : CIVUE_ICON(lucide, mic);

      if (bar_pair(app, id("stage-sound"), microphone,
                   muted ? k->danger : k->strong, muted ? "Unmute" : "Mute",
                   POPOVER_VOICE_SOUND, &sound)) {
        session_voice_mute(s, !muted);
      }

      // the camera is picked here, its picture is not sent yet.
      bar_pair(app, id("stage-camera"), CIVUE_ICON(lucide, video_off),
               k->faint, "Video is not sent yet", POPOVER_VOICE_CAMERA,
               &camera);

      div(key(id("stage-leave")), flex, items_center, justify_center, w(16),
          h(12), flex_shrink_0, rounded(xl), bg(k->danger),
          hover(bg(k->dnd)), cursor_pointer) {
        civue_button_state state = button();

        if (state.activated) {
          session_voice_leave(s);
        }

        glyph(CIVUE_ICON(lucide, phone_off), 20, CIVUE_WHITE);
        hint(app, id("stage-leave"), &state.response, "Disconnect",
             CIVUE_TOOLTIP_ABOVE);
      }
    }
  }

  if (here_now) {
    sound_menu(app, sound);
    camera_menu(app, camera);
  }
}

void voice_stage(App *app, Channel *c) {
  const Look *k = &app->look;
  const Store *store = app->session->store;
  bool anyone = false;

  for (size_t i = 0; i < store->nvoice_states && !anyone; i++) {
    anyone = store->voice_states[i].channel_id == c->id;
  }

  div(key(id("stage")), flex, flex_col, flex_1, min_h(0), bg(CIVUE_BLACK)) {
    div(key(id("stage-tiles")), relative, flex, items_center, justify_center,
        flex_1, min_h(0), m(4)) {
      civue_rect room = here();

      if (anyone) {
        tiles(app, c,
              xywh(0, 0, room.x1 - room.x0, room.y1 - room.y0));
      }
      else {
        text("No one is here yet.", text_color(k->faint));
      }
    }

    controls(app, c);
  }
}
