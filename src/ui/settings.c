#include "components.h"

#include <math.h>
#include <stdio.h>

#include <civue/dsl.h>

#include "voice/camera.h"
#include "voice/devices.h"
#include "voice/mictest.h"
#include "voice/voice.h"
#include "widgets.h"

/* ========================================================================== */
/* the settings, laid out as discord's are. learned from its stylesheets:     */
/* the dialog leaves 40 points around itself and grows to 1400 at most, and   */
/* takes the whole window once that is 1080 wide or less, under the title     */
/* bar. its sidebar is 240 wide whatever the window. the page is a column of  */
/* 696 at most, 64 from the top, 80 in a window of 1080 or less, and what it  */
/* sets side by side goes one under the other in a window of 800 or less.     */
/* ========================================================================== */

constexpr float GUTTER = 40;
constexpr float DIALOG_MAX = 1400;
constexpr float FULL_UNDER = 1080;
constexpr float WIDE_OVER = 1600;
constexpr float STACK_UNDER = 800;
constexpr float SIDEBAR = 240;
constexpr float PAGE_MAX = 696;
constexpr float ROW = 40;
// what the camera shows before it films.
constexpr float PREVIEW = 220;

typedef enum : uint8_t {
  PART_VOICE,
  PART_CAMERA,
  PART_ADVANCED,
  PARTS,
} Part;

static const char *const PART_NAMES[PARTS] = {"Voice", "Camera", "Advanced"};

/* ========================================================================== */
/* the sidebar.                                                               */
/* ========================================================================== */

static bool nav_item(App *app, civue_id id, civue_icon icon, const char *label,
                     bool active, civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, p(1), rounded(lg),
      text_color(active ? k->strong : color),
      style(.bg = active ? k->selected : CIVUE_CLEAR),
      hover(bg(active ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    pressed = button().activated;

    civue_node_icon(civue_ui_current(), icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20),
                                      .shrink = 0, .has_shrink = true));
    text(label, flex_1, min_w(0), truncate, font_weight(medium));
  }

  return pressed;
}

// the parts of the page under its item, by a line whose lit stretch says
// which one shows. the part pressed, -1 for none.
static int subnav(App *app) {
  const Look *k = &app->look;
  int pressed = -1;

  div(flex, gap(px(13)), flex_shrink_0, ml(px(13)), py(1)) {
    div(relative, w(px(2)), flex_shrink_0, rounded(full), bg(k->border)) {
      div(absolute, left(0), top(px((float)app->settings_part * 32)), w(px(2)),
          h(5), rounded(full), bg(k->strong)) {}
    }

    div(flex, flex_col, gap(3), flex_1, min_w(0)) {
      for (int part = 0; part < PARTS; part++) {
        bool active = app->settings_part == part;

        div(key(id_at(id("settings-part"), part)), flex, items_center, h(5),
            text_color(active ? k->strong : k->faint),
            hover(text_color(k->strong)), cursor_pointer) {
          if (button().activated) {
            pressed = part;
          }

          text(PART_NAMES[part], truncate);
        }
      }
    }
  }

  return pressed;
}

static void section_label(const Look *k, const char *label) {
  text(label, flex_shrink_0, mx(2), mt(2), mb(1), text_size(px(12)),
       line_height(px(16)), font_weight(medium), text_color(k->faint));
}

// the part asked for and whether to log out.
static int sidebar(App *app, bool *leave) {
  const Look *k = &app->look;
  const Store *store = app->session->store;
  int part = -1;

  div(flex, flex_col, flex_shrink_0, w(px(SIDEBAR)), h(full), pt(4), pl(3),
      bg(k->frame), border_r(1), border_color(k->border)) {
    div(flex, items_center, gap(3), flex_shrink_0, pl(1), pr(4), pb(2)) {
      avatar(app, store->self, 0, 40, false, k->frame);
      text(store_user_name(store->self), flex_1, min_w(0), truncate,
           font_weight(semibold), text_color(k->strong));
    }

    div(key(id("settings-nav")), flex, flex_col, gap(4), flex_1, min_h(0),
        overflow_y_scroll, scrollbars(&app->thin_bars), pt(3), pb(4), pl(1),
        pr(3)) {
      div(flex, flex_col, gap(1), flex_shrink_0, pb(1), border_b(1),
          border_color(k->border)) {
        section_label(k, "Experience");
        nav_item(app, id("settings-voice"), CIVUE_ICON(lucide, mic),
                 "Voice & Video", true, k->subtle);
        part = subnav(app);
      }

      div(flex, flex_col, gap(1), flex_shrink_0) {
        *leave = nav_item(app, id("settings-logout"),
                          CIVUE_ICON(lucide, log_out), "Log Out", false,
                          k->critical_text);
      }
    }
  }

  return part;
}

/* ========================================================================== */
/* the page.                                                                  */
/* ========================================================================== */

static void heading(const Look *k, const char *label) {
  text(label, flex_shrink_0, mb(6), text_size(px(24)), line_height(px(30)),
       font_weight(semibold), text_color(k->strong));
}

static void field_label(const Look *k, const char *name) {
  text(name, flex_shrink_0, mb(2), font_weight(medium), text_color(k->strong));
}

// the name a box shows for what is picked of a kind.
static void picked_name(const App *app, AudioKind kind, char *out, size_t cap) {
  int current = device_picked(app, kind);

  if (current >= 0) {
    snprintf(out, cap, "%s", app->devices[kind][current].name);
  }
  else if (kind == AUDIO_CAMERAS) {
    snprintf(out, cap, "No camera");
  }
  else if (app->default_names[kind][0]) {
    snprintf(out, cap, "macOS Default (%s)", app->default_names[kind]);
  }
  else {
    snprintf(out, cap, "macOS Default");
  }
}

// a box that opens the list of the devices of a kind under itself.
static void select_box(App *app, AudioKind kind) {
  const Look *k = &app->look;
  bool open = app->settings_select == (int8_t)kind;
  char name[160];

  picked_name(app, kind, name, sizeof name);

  div(key(id_at(id("settings-select"), kind)), flex, items_center, gap(2),
      flex_shrink_0, h(px(ROW)), px(3), rounded(lg), bg(k->input), border(1),
      border_color(open ? k->brand : k->border_strong), cursor_pointer) {
    if (open) {
      app->settings_select_rect = here();
    }

    if (button().activated) {
      app->settings_select = open ? -1 : (int8_t)kind;
      app->settings_select_rect = here();
    }

    if (kind == AUDIO_CAMERAS) {
      icon(lucide, camera, size(px(18)), flex_shrink_0,
           text_color(k->subtle));
    }

    text(name, flex_1, min_w(0), truncate, font_weight(medium),
         text_color(k->strong));

    if (open) {
      icon(lucide, chevron_up, size(4), flex_shrink_0, text_color(k->subtle));
    }
    else {
      icon(lucide, chevron_down, size(4), flex_shrink_0,
           text_color(k->subtle));
    }
  }
}

static bool option(App *app, civue_id id, const char *name, bool camera,
                   bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(px(ROW)), px(2),
      rounded(lg), style(.bg = on ? k->selected : CIVUE_CLEAR),
      hover(bg(k->hover)), cursor_pointer) {
    pressed = button().activated;

    if (camera) {
      icon(lucide, camera, size(px(18)), flex_shrink_0,
           text_color(k->subtle));
    }

    text(name, flex_1, min_w(0), truncate, font_weight(medium),
         text_color(k->strong));

    if (on) {
      div(flex, items_center, justify_center, size(5), flex_shrink_0,
          rounded(full), bg(k->brand), text_color(CIVUE_WHITE)) {
        icon(lucide, check, size(px(14)));
      }
    }
  }

  return pressed;
}

// the list of the box that is open, over everything else of the dialog.
static void select_list(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect box = app->settings_select_rect, list = {};

  if (app->settings_select < 0) {
    return;
  }

  AudioKind kind = (AudioKind)app->settings_select;
  bool standard = kind != AUDIO_CAMERAS;
  size_t count = app->ndevices[kind];
  int current = device_picked(app, kind);
  float height = 10 + ROW * (float)(count + (standard ? 1 : 0));
  float top = (float)box.y1 + 4;
  bool chosen = false;
  char name[160];

  // over the box when the window ends under it.
  if (top + height > ui->size.y - 8) {
    top = fmaxf((float)box.y0 - 4 - height, 8);
  }

  overlay() {
    div(key(id("settings-options")), flex, flex_col, left(px(box.x0)),
        top(px(top)), w(px(box.x1 - box.x0)), p(1), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg)) {
      list = here();

      if (standard) {
        snprintf(name, sizeof name, "macOS Default%s%s%s",
                 app->default_names[kind][0] ? " (" : "",
                 app->default_names[kind],
                 app->default_names[kind][0] ? ")" : "");

        if (option(app, id("settings-option-default"), name, false,
                   current < 0)) {
          devices_set(kind, nullptr);
          chosen = true;
        }
      }

      for (size_t i = 0; i < count; i++) {
        const AudioDevice *d = &app->devices[kind][i];

        if (option(app, id_at(id("settings-option"), i), d->name, !standard,
                   current == (int)i)) {
          devices_set(kind, d->id);
          chosen = true;
        }
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(list, ui->input.mouse) &&
                 !civue_rect_contains(box, ui->input.mouse);

  if (chosen || outside) {
    app->settings_select = -1;
  }
}

static void device_field(App *app, AudioKind kind, const char *name) {
  div(flex, flex_col, flex_1, min_w(0)) {
    field_label(&app->look, name);
    select_box(app, kind);
  }
}

static void volume_field(App *app, bool input, bool *let_go) {
  div(flex, flex_col, flex_1, min_w(0)) {
    field_label(&app->look,
                input ? "Microphone Volume" : "Speaker Volume");

    if (input) {
      devices_set_input_volume(volume_slider(
        app, id("settings-input-volume"), devices_input_volume(), 1, let_go));
    }
    else {
      devices_set_output_volume(volume_slider(app, id("settings-output-volume"),
                                              devices_output_volume(), 2,
                                              let_go));
    }
  }
}

// two fields side by side, or one under the other in a narrow window.
#define PAIR(stacked, first, second)                   \
  if (stacked) {                                       \
    div(flex, flex_col, gap(4), flex_shrink_0, mb(6)) { \
      first;                                           \
      second;                                          \
    }                                                  \
  }                                                    \
  else {                                               \
    div(flex, gap(4), flex_shrink_0, mb(6)) {          \
      first;                                           \
      second;                                          \
    }                                                  \
  }

// the microphone's loudness as thin bars, the lit ones going from orange
// to green along the row.
static void test_meter(App *app, float level) {
  const Look *k = &app->look;
  civue_rect r = here();
  int bars = (int)((r.x1 - r.x0 + 4) / 8);
  // on a scale of decibels: 60 of them.
  float decibels = level > 0 ? 20 * log10f(level) : -100;
  int lit = (int)((decibels + 60) / 60 * (float)bars);
  civue_color low = rgba(0xf2a640ff), high = rgba(0x86d17bff);

  for (int i = 0; i < bars; i++) {
    double x = r.x0 + i * 8;
    float along = bars > 1 ? (float)i / (float)(bars - 1) : 0;
    civue_color warm = {
      low.r + (high.r - low.r) * along,
      low.g + (high.g - low.g) * along,
      low.b + (high.b - low.b) * along,
      1,
    };

    fill(civue_rect_from(x, r.y0, x + 4, r.y1),
         i < lit ? warm : k->border_strong, 2);
  }
}

// how loud the microphone is, as a call or the test hears it.
static float mic_test(App *app) {
  const Look *k = &app->look;
  const Session *s = app->session;
  bool calling = s->voice && voice_status(s->voice) == VOICE_CONNECTED;
  bool running = mictest_running();
  bool starting = calling ? voice_microphone_starting(s->voice)
                          : mictest_starting();
  float level = calling ? voice_input_level(s->voice) : mictest_level();

  div(flex, items_start, gap(4), flex_shrink_0, mb(6)) {
    // a call has the microphone, and shows how loud it is by itself.
    if (!calling) {
      div(key(id("mic-test")), flex, items_center, justify_center, h(px(ROW)),
          px(4), flex_shrink_0, rounded(lg), bg(k->brand),
          hover(bg(k->brand_hover)), active(bg(k->brand_active)),
          text_color(CIVUE_WHITE), cursor_pointer) {
        if (button().activated) {
          if (running) {
            mictest_stop();
          }
          else {
            mictest_start(civue_window_wake);
          }
        }

        text(!running ? "Mic Test" : starting ? "Starting..." : "Stop Testing",
             font_weight(medium));
      }
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      div(flex_shrink_0, h(6), my(2)) {
        test_meter(app, level);
      }

      if (mictest_denied()) {
        text("Quarrel may not use the microphone, see the system's settings.",
             flex_shrink_0, text_size(px(14)), line_height(px(18)),
             text_color(k->critical_text));
      }
      else if (starting) {
        text("Starting the microphone...", flex_shrink_0, text_size(px(14)),
             line_height(px(18)), text_color(k->subtle));
      }
      else if (running) {
        text("Playing your voice back to you.", flex_shrink_0,
             text_size(px(14)), line_height(px(18)), text_color(k->subtle));
      }
    }
  }

  if (running || calling) {
    mictest_follow();
    // the bars move without anything else changing.
    civue_window_wake();
  }

  return level;
}

static bool profile(App *app, civue_id id, const char *name, const char *about,
                    bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, gap(3), flex_shrink_0, cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(6), flex_shrink_0,
        rounded(full), border(2), style(.bg = on ? k->brand : CIVUE_CLEAR),
        border_color(on ? k->brand : k->border_strong)) {
      if (on) {
        div(size(px(8)), rounded(full), bg(k->chat)) {}
      }
    }

    div(flex, flex_col, gap(1), flex_1, min_w(0), pt(px(2))) {
      text(name, font_weight(medium), text_color(k->strong));
      text(about, text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));
    }
  }

  return pressed;
}

static void input_profile(App *app) {
  const Look *k = &app->look;
  static const char *const NAMES[PROFILES] = {
    [PROFILE_ISOLATION] = "Voice Isolation",
    [PROFILE_STUDIO] = "Studio",
    [PROFILE_CUSTOM] = "Custom",
  };
  static const char *const ABOUT[PROFILES] = {
    [PROFILE_ISOLATION] = "Just your voice: Quarrel cuts through the noise",
    [PROFILE_STUDIO] = "Open mic: Quarrel leaves it to the Mic Mode of macOS",
    [PROFILE_CUSTOM] = "Advanced mode: set each part yourself",
  };
  VoiceProfile now = devices_profile();

  field_label(k, "Input Profile");

  div(flex, flex_col, gap(3), flex_shrink_0, mt(1)) {
    for (int i = 0; i < PROFILES; i++) {
      if (profile(app, id_at(id("profile"), i), NAMES[i], ABOUT[i],
                  now == i) &&
          now != i) {
        devices_set_profile((VoiceProfile)i);
      }
    }
  }

  if (now == PROFILE_STUDIO) {
    div(key(id("mic-modes")), flex, items_center, justify_center, self_start,
        h(8), px(3), ml(9), mt(3), flex_shrink_0, rounded(lg),
        bg(k->surface_high), hover(bg(k->hover)), text_color(k->strong),
        cursor_pointer) {
      if (button().activated) {
        audio_show_modes();
      }

      text("Open Mic Mode", text_size(px(14)), font_weight(medium));
    }
  }
}

static void switch_row(App *app, VoiceOption option, const char *name,
                       const char *about) {
  const Look *k = &app->look;
  bool on = devices_option(option);

  div(flex, items_center, gap(4), flex_shrink_0) {
    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      text(name, font_weight(medium), text_color(k->strong));
      text(about, text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));
    }

    div(key(id_at(id("settings-switch"), option)), relative, w(10), h(6),
        flex_shrink_0, rounded(full),
        style(.bg = on ? k->brand : k->border_strong), cursor_pointer) {
      if (button().activated) {
        devices_set_option(option, !on);
      }

      div(absolute, top(px(3)), left(px(on ? 19 : 3)), size(px(18)),
          rounded(full), bg(CIVUE_WHITE)) {}
    }
  }
}

// where the microphone starts to be sent, over how loud it is now: what is
// under the thumb stays here, what is over it goes.
static void sensitivity_slider(App *app, float level) {
  const Look *k = &app->look;
  float value = devices_sensitivity();
  float decibels = level > 0 ? fmaxf(20 * log10f(level), -100) : -100;
  civue_color quiet = rgba(0xf2a640ff), sent = k->positive;
  char line[64];

  div(key(id("settings-sensitivity")), flex_shrink_0, h(6), mx(2), px(2)) {
    civue_slider_state state = slider(value, -100, 0, -100, 1);

    if (state.changed) {
      value = (float)state.value;
      devices_set_sensitivity(value);
    }

    if (state.ended) {
      devices_save_sensitivity();
    }

    civue_rect track = content();
    double mid = (track.y0 + track.y1) / 2, wide = track.x1 - track.x0;
    double x = track.x0 + (value + 100) / 100 * wide;
    double heard = track.x0 + (decibels + 100) / 100 * wide;

    quiet.a = sent.a = 0.3f;
    fill(civue_rect_from(track.x0, mid - 3, x, mid + 3), quiet, 3);
    fill(civue_rect_from(x, mid - 3, track.x1, mid + 3), sent, 3);
    quiet.a = sent.a = 1;
    fill(civue_rect_from(track.x0, mid - 3, fmin(heard, x), mid + 3), quiet, 3);

    if (heard > x) {
      fill(civue_rect_from(x, mid - 3, heard, mid + 3), sent, 3);
    }

    fill(civue_rect_from(x - 8, mid - 8, x + 8, mid + 8), CIVUE_WHITE, 8);
  }

  snprintf(line, sizeof line, "Sends what is louder than %.0f dB.",
           (double)value);
  text(line, flex_shrink_0, mt(1), text_size(px(14)), line_height(px(18)),
       text_color(k->subtle));
}

static void noise_row(App *app) {
  const Look *k = &app->look;
  static const char *const NAMES[NOISES] = {
    [NOISE_OFF] = "Off",
    [NOISE_STANDARD] = "Standard",
    [NOISE_VOICE] = "Voice only",
  };
  static const char *const ABOUT[NOISES] = {
    [NOISE_OFF] = "Nothing is taken out of your microphone.",
    [NOISE_STANDARD] = "Takes the background noise out of your microphone.",
    [NOISE_VOICE] = ("Takes the noise out, and mutes everything while you "
                     "do not speak, like keys and clicks."),
  };
  VoiceNoise now = devices_noise();

  div(flex, items_center, gap(4), flex_shrink_0) {
    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      text("Noise Suppression", font_weight(medium), text_color(k->strong));
      text(ABOUT[now], text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));
    }

    div(flex, gap(1), flex_shrink_0, p(1), rounded(lg), bg(k->input),
        border(1), border_color(k->border_strong)) {
      for (int i = 0; i < NOISES; i++) {
        bool on = now == i;

        div(key(id_at(id("settings-noise"), i)), flex, items_center,
            justify_center, h(8), px(3), rounded(md),
            style(.bg = on ? k->brand : CIVUE_CLEAR),
            text_color(on ? CIVUE_WHITE : k->subtle),
            hover(text_color(on ? CIVUE_WHITE : k->strong)), cursor_pointer) {
          if (button().activated && !on) {
            devices_set_noise((VoiceNoise)i);
          }

          text(NAMES[i], text_size(px(14)), font_weight(medium));
        }
      }
    }
  }
}

// what the custom profile lets its user set.
static void custom_input(App *app, float level) {
  if (devices_profile() != PROFILE_CUSTOM) {
    return;
  }

  div(flex, flex_col, gap(6), flex_shrink_0, mt(6)) {
    div(flex, flex_col, flex_shrink_0) {
      switch_row(app, VOICE_AUTO_SENSITIVITY, "Automatic Input Sensitivity",
                 "Quarrel decides when your microphone is loud enough to "
                 "send.");

      if (!devices_option(VOICE_AUTO_SENSITIVITY)) {
        div(flex, flex_col, flex_shrink_0, mt(3)) {
          sensitivity_slider(app, level);
        }
      }
    }

    noise_row(app);
    switch_row(app, VOICE_ECHO, "Echo Cancellation",
               "macOS takes what your speakers play out of your microphone. "
               "Its Mic Mode only applies with this on.");
  }
}

static bool video_button(App *app, civue_id id, const char *label) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, h(px(ROW)), px(4),
      flex_shrink_0, rounded(lg), bg(k->brand), hover(bg(k->brand_hover)),
      active(bg(k->brand_active)), text_color(CIVUE_WHITE), cursor_pointer) {
    pressed = button().activated;
    text(label, font_weight(medium));
  }

  return pressed;
}

// what the camera picked sees, in a box as wide as the page, `wide`.
static void camera_preview(App *app, float wide) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool running = camera_running();
  uint32_t width = 0, height = 0;
  civue_texture picture = running ? camera_picture(&width, &height) : 0;
  // the shape of the picture once there is one.
  float tall = picture != 0 && width != 0
                 ? wide * (float)height / (float)width
                 : PREVIEW;

  div(relative, flex, items_center, justify_center, flex_shrink_0, mb(6),
      rounded(lg), bg(k->frame), border(1), border_color(k->border),
      style(.h = civue_px(tall))) {
    if (picture != 0) {
      civue_image(ui, picture, here(), (float[4]){0, 0, 1, 1}, 1, 8, false);

      div(absolute, left(0), top(px(tall - ROW - 12)), w(full), flex,
          justify_center) {
        if (video_button(app, id("video-stop"), "Stop Video")) {
          camera_stop();
        }
      }
    }
    else if (running) {
      text("Starting the camera...", text_color(k->subtle));
    }
    else if (video_button(app, id("video-test"), "Test Video")) {
      camera_start(ui->gfx, devices_get(AUDIO_CAMERAS), civue_window_wake);
    }
  }

  if (camera_denied()) {
    text("Quarrel may not use the camera, see the system's settings.",
         flex_shrink_0, mb(6), text_size(px(14)), line_height(px(18)),
         text_color(k->critical_text));
  }

  if (running) {
    camera_follow(devices_get(AUDIO_CAMERAS));
  }
}

// the page, and where each of its parts starts down it.
static void page(App *app, bool narrow, bool stacked, float tops[PARTS]) {
  const Look *k = &app->look;
  bool let_go = false;

  div(flex, justify_center, flex_shrink_0, px(4),
      style(.padding[0] = civue_px(narrow ? 80 : 64),
            .padding[2] = civue_px(narrow ? 80 : 64))) {
    div(flex, flex_col, flex_1, min_w(0), max_w(px(PAGE_MAX))) {
      civue_rect column = here();
      float start = (float)column.y0;

      div(flex, flex_col, flex_shrink_0) {
        tops[PART_VOICE] = (float)here().y0 - start;
        heading(k, "Voice");
        PAIR(stacked, device_field(app, AUDIO_INPUTS, "Microphone"),
             device_field(app, AUDIO_OUTPUTS, "Speaker"));
        PAIR(stacked, volume_field(app, true, &let_go),
             volume_field(app, false, &let_go));
        float level = mic_test(app);

        input_profile(app);
        custom_input(app, level);
      }

      div(flex_shrink_0, h(px(1)), my(9), bg(k->border)) {}

      div(flex, flex_col, flex_shrink_0) {
        tops[PART_CAMERA] = (float)here().y0 - start;
        heading(k, "Camera");
        camera_preview(app, (float)(column.x1 - column.x0));
        switch_row(app, VOICE_ALWAYS_PREVIEW, "Always preview video",
                   "Shows your picture before it goes to a call");
        div(flex, flex_col, flex_shrink_0, mt(6)) {
          field_label(k, "Camera");
          select_box(app, AUDIO_CAMERAS);
        }
      }

      div(flex_shrink_0, h(px(1)), my(9), bg(k->border)) {}

      div(flex, flex_col, gap(6), flex_shrink_0) {
        tops[PART_ADVANCED] = (float)here().y0 - start;
        text("Advanced", flex_shrink_0, text_size(px(24)), line_height(px(30)),
             font_weight(semibold), text_color(k->strong));
        switch_row(app, VOICE_SWITCH_WARNING, "Switch Voice Channel Warning",
                   "Asks before a call is left for another voice channel.");
        switch_row(app, VOICE_QOS, "Quality Of Service",
                   "Tells your router that the packets of a call are urgent. "
                   "Some routers and internet providers misbehave with it.");
      }
    }
  }

  if (let_go) {
    devices_save_volumes();
  }
}

static void close_settings(App *app) {
  app->settings_open = false;
  app->settings_select = -1;
  mictest_stop();
  camera_stop();
}

void settings_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();

  if (!app->settings_open) {
    return;
  }

  devices_follow(app);

  float width = ui->size.x, height = ui->size.y;
  bool full = width <= FULL_UNDER;
  float gutter = GUTTER + (width > WIDE_OVER ? TOP_BAR_HEIGHT : 0);
  float wide = full ? width : fminf(width - 2 * gutter, DIALOG_MAX);
  float tall = full ? height : height - 2 * gutter;
  // the dialog has a border and round corners unless it is the window.
  float edge = full ? 0 : 1;
  civue_id body = id("settings-body");
  // a press that closes a list of devices does nothing else.
  bool listing = app->settings_select >= 0;
  bool close = false, leave = false;
  float tops[PARTS] = {};
  int part = -1;

  overlay() {
    div(key(id("settings-backdrop")), left(0), top(0), w(px(width)),
        h(px(height)), flex, items_center, justify_center,
        bg(rgba(0x000000b3))) {
      close = button().activated && !listing;

      div(key(id("settings")), flex, w(px(wide)), h(px(tall)), bg(k->frame),
          overflow_hidden, text_size(px(16)), line_height(px(20)),
          text_color(k->text),
          border_color(k->border),
          style(.radius = {civue_px(edge * 12), civue_px(edge * 12),
                           civue_px(edge * 12), civue_px(edge * 12)},
                .border_w = {edge, edge, edge, edge},
                .padding[0] = civue_px(full ? TOP_BAR_HEIGHT : 0))) {
        // a press on the dialog is not one on what is behind it.
        button();

        part = sidebar(app, &leave);

        div(flex, flex_col, flex_1, min_w(0), h(full), bg(k->chat)) {
          div(flex, items_center, flex_shrink_0, h(12), pl(4), pr(2),
              border_b(1), border_color(k->border)) {
            text("Voice & Video", flex_1, min_w(0), truncate,
                 font_weight(medium), text_color(k->strong));

            if (icon_button(app, id("settings-close"), CIVUE_ICON(lucide, x),
                            32, 20, nullptr)) {
              close = true;
            }
          }

          div(key(body), flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
              scrollbars(&app->bars)) {
            page(app, full, width <= STACK_UNDER, tops);
          }
        }
      }
    }
  }

  select_list(app);

  if (part >= 0) {
    app->settings_part = (uint8_t)part;
    civue_set_scroll_y(ui, body, tops[part]);
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    if (listing) {
      app->settings_select = -1;
    }
    else {
      close = true;
    }
  }

  if (close || leave) {
    close_settings(app);
  }

  // the roster changes last: the frame ends with the session it started
  // with.
  if (leave) {
    roster_logout(app->roster);
  }
}
