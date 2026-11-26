#include "components.h"

#include <math.h>
#include <stdio.h>

#include <civue/dsl.h>

#include "alerts.h"
#include "cues.h"
#include "forms.h"
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
constexpr float SIDEBAR = 256;
constexpr float PAGE_MAX = 696;
// a select and a button, as shadcn's: 36 tall.
constexpr float ROW = 36;
// a row of the list a select opens.
constexpr float OPTION = 32;
// what the camera shows before it films.
constexpr float PREVIEW = 220;

typedef enum : uint8_t {
  PART_VOICE,
  PART_CAMERA,
  PART_ADVANCED,
  PARTS,
} Part;

static const char *const PART_NAMES[PARTS] = {"Voice", "Camera", "Advanced"};
static const char *const PAGE_NAMES[] = {
  [SETTINGS_APPEARANCE] = "Appearance",
  [SETTINGS_VOICE] = "Voice & Video",
  [SETTINGS_NOTIFICATIONS] = "Notifications",
};

/* ========================================================================== */
/* the sidebar.                                                               */
/* ========================================================================== */

// a row of the sidebar as shadcn's settings draw theirs: a ghost button 36
// tall, 12 to the sides, a 16 point icon 8 before 14 point text. the
// destructive one is red and reddens under the pointer.
static bool nav_item(App *app, civue_id id, civue_icon icon, const char *label,
                     bool active, bool destructive) {
  const Look *k = &app->look;
  civue_color ink = destructive ? k->critical_text : k->text;
  civue_color lit = destructive ? civue_alpha(k->danger, .2f) : k->hover;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(9), px(3),
      rounded(lg), text_color(ink), text_size(px(14)), line_height(px(20)),
      font_weight(medium), style(.bg = active ? k->selected : CIVUE_CLEAR),
      hover(bg(active ? k->selected : lit)), cursor_pointer) {
    pressed = button().activated;

    civue_node_icon(civue_ui_current(), icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(16), .h = civue_px(16),
                                      .shrink = 0, .has_shrink = true));
    text(label, flex_1, min_w(0), truncate);
  }

  return pressed;
}

// the parts of the page under its item, as shadcn's sub menus: 14 in, a
// line at their left and rows 28 tall. the part pressed, -1 for none.
static int subnav(App *app) {
  const Look *k = &app->look;
  int pressed = -1;

  div(flex, flex_col, gap(1), flex_shrink_0, mx(px(14)), px(px(10)),
      py(px(2)), border_l(1), border_color(k->border)) {
    for (int part = 0; part < PARTS; part++) {
      bool active = app->settings_part == part;

      div(key(id_at(id("settings-part"), part)), flex, items_center, h(7),
          px(2), rounded(lg), text_size(px(14)), line_height(px(20)),
          text_color(k->text), style(.bg = active ? k->selected : CIVUE_CLEAR),
          hover(bg(active ? k->selected : k->hover)), cursor_pointer) {
        if (button().activated) {
          pressed = part;
        }

        text(PART_NAMES[part], truncate);
      }
    }
  }

  return pressed;
}

// opens `page`. what the voice page was testing stops once it is left.
static void open_page(App *app, SettingsPage page) {
  if (app->settings_page == SETTINGS_VOICE && page != SETTINGS_VOICE) {
    app->settings_select = -1;
    mictest_stop();
    camera_stop();
  }

  app->settings_page = page;
}

// the label of a group of rows, shadcn's: 32 tall, 12 point, the text's
// color at 70 percent.
static void section_label(const Look *k, const char *label) {
  div(flex, items_center, flex_shrink_0, h(8), px(2)) {
    text(label, truncate, text_size(px(12)), line_height(px(16)),
         font_weight(medium), text_color(civue_alpha(k->text, .7f)));
  }
}

// the part asked for and whether to log out.
static int sidebar(App *app, bool *leave) {
  const Look *k = &app->look;
  const Store *store = app->session->store;
  int part = -1;

  div(flex, flex_col, flex_shrink_0, w(px(SIDEBAR)), h(full), bg(k->frame),
      border_r(1), border_color(k->border)) {
    // shadcn's header: a large row of 48, the avatar 32 beside two lines.
    div(flex, items_center, gap(2), flex_shrink_0, h(12), m(2), px(2)) {
      avatar(app, store->self, 0, 32, false, k->frame);

      div(flex, flex_col, flex_1, min_w(0)) {
        text(store_user_name(store->self), truncate, text_size(px(14)),
             line_height(px(20)), font_weight(semibold),
             text_color(k->strong));
        text("User Settings", truncate, text_size(px(12)),
             line_height(px(16)), text_color(k->subtle));
      }
    }

    div(flex_shrink_0, h(px(1)), mx(2), bg(k->border)) {}

    div(key(id("settings-nav")), flex, flex_col, flex_1, min_h(0),
        overflow_y_scroll, scrollbars(&app->thin_bars), p(2)) {
      div(flex, flex_col, gap(1), flex_shrink_0) {
        SettingsPage now = app->settings_page;
        bool voice = now == SETTINGS_VOICE;

        section_label(k, "Experience");

        if (nav_item(app, id("settings-appearance"),
                     CIVUE_ICON(mingcute, palette_fill),
                     PAGE_NAMES[SETTINGS_APPEARANCE],
                     now == SETTINGS_APPEARANCE, false)) {
          open_page(app, SETTINGS_APPEARANCE);
        }

        if (nav_item(app, id("settings-voice"), CIVUE_ICON(mingcute, mic_fill),
                     PAGE_NAMES[SETTINGS_VOICE], voice, false) &&
            !voice) {
          open_page(app, SETTINGS_VOICE);
          part = PART_VOICE;
        }

        if (voice) {
          int pressed = subnav(app);

          part = pressed >= 0 ? pressed : part;
        }

        if (nav_item(app, id("settings-notifications"),
                     CIVUE_ICON(mingcute, notification_fill),
                     PAGE_NAMES[SETTINGS_NOTIFICATIONS],
                     now == SETTINGS_NOTIFICATIONS, false)) {
          open_page(app, SETTINGS_NOTIFICATIONS);
        }
      }

      div(flex_shrink_0, h(px(1)), my(2), mx(2), bg(k->border)) {}

      div(flex, flex_col, gap(1), flex_shrink_0) {
        *leave = nav_item(app, id("settings-logout"),
                          CIVUE_ICON(mingcute, exit_fill), "Log Out", false, true);
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
  form_label(k, name);
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
    snprintf(out, cap, AUDIO_SYSTEM_DEFAULT " (%s)", app->default_names[kind]);
  }
  else {
    snprintf(out, cap, AUDIO_SYSTEM_DEFAULT);
  }
}

// a box that opens the list of the devices of a kind under itself.
static void select_box(App *app, AudioKind kind) {
  const Look *k = &app->look;
  bool open = app->settings_select == (int8_t)kind;
  char name[160];

  picked_name(app, kind, name, sizeof name);

  div(key(id_at(id("settings-select"), kind)), flex, items_center, gap(2),
      flex_shrink_0, h(px(ROW)), px(3), rounded(lg), bg(k->field), border(1),
      border_color(open ? k->ring : k->border_strong), text_size(px(14)),
      line_height(px(20)), cursor_pointer) {
    if (open) {
      app->settings_select_rect = here();
    }

    if (button().activated) {
      app->settings_select = open ? -1 : (int8_t)kind;
      app->settings_select_rect = here();
    }

    if (kind == AUDIO_CAMERAS) {
      icon(mingcute, computer_camera_fill, size(4), flex_shrink_0, text_color(k->subtle));
    }

    text(name, flex_1, min_w(0), truncate, text_color(k->strong));

    if (open) {
      icon(mingcute, up_fill, size(4), flex_shrink_0, text_color(k->subtle));
    }
    else {
      icon(mingcute, down_fill, size(4), flex_shrink_0,
           text_color(k->subtle));
    }
  }
}

static bool option(App *app, civue_id id, const char *name, bool camera,
                   bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(px(OPTION)),
      px(2), rounded(md), text_size(px(14)), line_height(px(20)),
      hover(bg(k->hover)), cursor_pointer) {
    pressed = button().activated;

    if (camera) {
      icon(mingcute, computer_camera_fill, size(4), flex_shrink_0, text_color(k->subtle));
    }

    text(name, flex_1, min_w(0), truncate, text_color(k->strong));

    if (on) {
      icon(mingcute, check_fill, size(4), flex_shrink_0, text_color(k->strong));
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
  float height = 10 + OPTION * (float)(count + (standard ? 1 : 0));
  float top = (float)box.y1 + 4;
  bool chosen = false;
  char name[160];

  // over the box when the window ends under it.
  if (top + height > ui->size.y - 8) {
    top = fmaxf((float)box.y0 - 4 - height, 8);
  }

  overlay() {
    div(key(id("settings-options")), flex, flex_col, left(px(box.x0)),
        top(px(top)), w(px(box.x1 - box.x0)), p(1), rounded(lg),
        bg(k->popover), border(1), border_color(k->border), shadow(md)) {
      list = here();

      if (standard) {
        snprintf(name, sizeof name, AUDIO_SYSTEM_DEFAULT "%s%s%s",
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

// the microphone's loudness as thin bars, the lit ones going from the
// theme's warning to its positive along the row.
static void test_meter(App *app, float level) {
  const Look *k = &app->look;
  civue_rect r = here();
  int bars = (int)((r.x1 - r.x0 + 4) / 8);
  // on a scale of decibels: 60 of them.
  float decibels = level > 0 ? 20 * log10f(level) : -100;
  int lit = (int)((decibels + 60) / 60 * (float)bars);
  civue_color low = k->warning, high = k->positive_text;

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
      if (form_button(app, id("mic-test"),
                      !running   ? "Mic Test"
                      : starting ? "Starting..."
                                 : "Stop Testing",
                      BUTTON_PRIMARY, false)) {
        if (running) {
          mictest_stop();
        }
        else {
          mictest_start(civue_window_wake);
        }
      }
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      div(flex_shrink_0, h(6), my(px(6))) {
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

  // shadcn's radio: 16 across with a dot of 8, 12 before its label.
  div(key(id), flex, items_start, gap(3), flex_shrink_0, text_size(px(14)),
      line_height(px(20)), cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(4), flex_shrink_0, mt(px(2)),
        rounded(full), border(1), bg(k->field),
        border_color(on ? k->primary : k->border_strong)) {
      if (on) {
        div(size(2), rounded(full), bg(k->primary)) {}
      }
    }

    div(flex, flex_col, gap(px(2)), flex_1, min_w(0)) {
      text(name, font_weight(medium), text_color(k->strong));
      text(about, text_color(k->subtle));
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
    [PROFILE_STUDIO] =
      AUDIO_HAS_MODES ? "Open mic: Quarrel leaves it to the Mic Mode of macOS"
                      : "Open mic: Quarrel only takes out the echo",
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

  if (now == PROFILE_STUDIO && AUDIO_HAS_MODES) {
    div(flex, flex_shrink_0, ml(7), mt(3)) {
      if (form_button_small(app, id("mic-modes"), "Open Mic Mode",
                            BUTTON_OUTLINE, false)) {
        audio_show_modes();
      }
    }
  }
}

// a switch beside what it turns on and off, said in a line of its own
// when `about` is given. true when it was pressed.
static bool switch_box(App *app, civue_id id, const char *name,
                       const char *about, bool on) {
  const Look *k = &app->look;
  bool pressed = false;

  div(flex, items_center, gap(4), flex_shrink_0) {
    div(flex, flex_col, gap(1), flex_1, min_w(0)) {
      text(name, font_weight(medium), text_color(k->strong));

      if (about != nullptr) {
        text(about, text_size(px(14)), line_height(px(18)),
             text_color(k->subtle));
      }
    }

    pressed = form_switch(app, id, on, false);
  }

  return pressed;
}

static void switch_row(App *app, VoiceOption option, const char *name,
                       const char *about) {
  bool on = devices_option(option);

  if (switch_box(app, id_at(id("settings-switch"), option), name, about, on)) {
    devices_set_option(option, !on);
  }
}

// what the settings call a sound.
static const char *sound_label(Sound sound) {
  switch (sound) {
    case SOUND_MESSAGE1:
      return "Message";
    case SOUND_DEAFEN:
      return "Deafen";
    case SOUND_UNDEAFEN:
      return "Undeafen";
    case SOUND_MUTE:
      return "Mute";
    case SOUND_UNMUTE:
      return "Unmute";
    case SOUND_DISCONNECT:
      return "Voice Disconnected";
    case SOUND_USER_JOIN:
      return "User Join";
    case SOUND_USER_LEAVE:
      return "User Leave";
    case SOUND_USER_MOVED:
      return "User Moved";
    case SOUND_STREAM_STARTED:
      return "Stream Started";
    case SOUND_CALL_RINGING:
      return "Incoming Ring";
    case SOUND_CALL_CALLING:
      return "Outgoing Ring";
    default:
      return "Sound";
  }
}

static void sound_row(App *app, Sound sound) {
  bool on = sounds_enabled(sound);

  if (switch_box(app, id_at(id("settings-sound"), sound), sound_label(sound),
                 nullptr, on)) {
    sounds_set_enabled(sound, !on);
  }
}

// where the microphone starts to be sent, over how loud it is now: what is
// under the thumb stays here, what is over it goes.
static void sensitivity_slider(App *app, float level) {
  const Look *k = &app->look;
  float value = devices_sensitivity();
  float decibels = level > 0 ? fmaxf(20 * log10f(level), -100) : -100;
  civue_color quiet = k->warning, sent = k->positive;
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

    fill(civue_rect_from(x - 8, mid - 8, x + 8, mid + 8), k->strong, 8);
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

    // shadcn's tabs: a muted list 36 tall, 3 of padding, the active one on
    // a field's ground and edge.
    div(flex, items_center, flex_shrink_0, h(9), p(px(3)), rounded(lg),
        bg(k->surface_high)) {
      for (int i = 0; i < NOISES; i++) {
        bool on = now == i;

        div(key(id_at(id("settings-noise"), i)), flex, items_center,
            justify_center, h(full), px(2), rounded(lg), border(1),
            border_color(on ? k->border_strong : CIVUE_CLEAR),
            style(.bg = on ? k->field : CIVUE_CLEAR),
            text_color(on ? k->strong : k->subtle), hover(text_color(k->strong)),
            text_size(px(14)), line_height(px(20)), font_weight(medium),
            cursor_pointer) {
          if (button().activated && !on) {
            devices_set_noise((VoiceNoise)i);
          }

          text(NAMES[i], whitespace_nowrap);
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
               AUDIO_HAS_MODES
                 ? "macOS takes what your speakers play out of your "
                   "microphone. Its Mic Mode only applies with this on."
                 : "Quarrel takes what your speakers play out of your "
                   "microphone.");
  }
}

static bool video_button(App *app, civue_id id, const char *label) {
  return form_button(app, id, label, BUTTON_PRIMARY, false);
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

// a small window drawn in the colors of `theme`, its name under it. true
// when pressed.
static bool theme_tile(App *app, Theme theme) {
  const Look *k = &app->look;
  Look t = look_of(theme);
  bool on = app->theme == theme, pressed = false;

  div(key(id_at(id("settings-theme"), theme)), flex, flex_col, gap(3), flex_1,
      min_w(0), p(2), rounded(xl), border(2),
      border_color(on ? k->primary : k->border),
      hover(border_color(on ? k->primary : k->border_strong)),
      cursor_pointer) {
    pressed = button().activated;

    div(flex, gap(2), h(px(120)), p(2), rounded(lg), bg(t.frame),
        overflow_hidden) {
      div(flex, flex_col, gap(2), w(px(56)), flex_shrink_0, pt(1)) {
        for (int i = 0; i < 4; i++) {
          div(h(px(8)), rounded(sm), bg(i == 1 ? t.selected : t.hover)) {}
        }
      }

      div(flex, flex_col, justify_end, gap(2), flex_1, min_w(0), p(2),
          rounded(md), bg(t.chat), border(1), border_color(t.frame_border)) {
        for (int i = 0; i < 2; i++) {
          div(flex, items_center, gap(2), flex_shrink_0) {
            div(size(px(14)), flex_shrink_0, rounded(full),
                bg(t.surface_high)) {}

            div(flex, flex_col, gap(1), flex_1, min_w(0)) {
              div(h(px(4)), w(px(36)), rounded(full), bg(t.strong)) {}
              div(h(px(4)), rounded(full), bg(t.subtle),
                  style(.w = civue_relative(i ? .55f : .8f))) {}
            }
          }
        }

        div(flex, items_center, justify_end, h(px(20)), px(1), flex_shrink_0,
            rounded(sm), bg(t.input), border(1), border_color(t.border)) {
          div(w(px(20)), h(px(12)), rounded(px(3)), bg(t.primary)) {}
        }
      }
    }

    div(flex, items_center, gap(2), px(1), flex_shrink_0) {
      div(flex, items_center, justify_center, size(4), flex_shrink_0,
          rounded(full), border(1), bg(k->field),
          border_color(on ? k->primary : k->border_strong)) {
        if (on) {
          div(size(2), rounded(full), bg(k->primary)) {}
        }
      }

      text(THEME_NAMES[theme], flex_1, min_w(0), truncate, text_size(px(14)),
           line_height(px(20)), font_weight(medium), text_color(k->strong));
    }
  }

  return pressed;
}

// how the app looks. the themes change its colors and nothing else.
static void appearance_page(App *app, bool narrow, bool stacked) {
  const Look *k = &app->look;
  Theme picked = app->theme;

  div(flex, justify_center, flex_shrink_0, px(4),
      style(.padding[0] = civue_px(narrow ? 80 : 64),
            .padding[2] = civue_px(narrow ? 80 : 64))) {
    div(flex, flex_col, flex_1, min_w(0), max_w(px(PAGE_MAX))) {
      heading(k, "Appearance");
      field_label(k, "Theme");
      text("Only the colors change, everything stays where it is.",
           flex_shrink_0, mb(4), text_size(px(14)), line_height(px(18)),
           text_color(k->subtle));

      div(flex, gap(4), flex_shrink_0,
          style(.direction = stacked ? CIVUE_COL : CIVUE_ROW)) {
        for (int i = 0; i < THEMES; i++) {
          if (theme_tile(app, (Theme)i)) {
            picked = (Theme)i;
          }
        }
      }
    }
  }

  if (picked != app->theme) {
    app_set_theme(app, picked);
  }
}

// what the user is told of while they look elsewhere. which messages are
// worth it stays with their notification settings on discord.
static void notifications_page(App *app, bool narrow) {
  const Look *k = &app->look;

  div(flex, justify_center, flex_shrink_0, px(4),
      style(.padding[0] = civue_px(narrow ? 80 : 64),
            .padding[2] = civue_px(narrow ? 80 : 64))) {
    div(flex, flex_col, gap(6), flex_1, min_w(0), max_w(px(PAGE_MAX))) {
      text("Notifications", flex_shrink_0, text_size(px(24)),
           line_height(px(30)), font_weight(semibold), text_color(k->strong));
      if (switch_box(app, id("settings-notify"),
                     "Enable Desktop Notifications",
                     "Shows a notification of your system for the messages "
                     "your notification settings want you told of, from "
                     "every account logged in.",
                     alerts_enabled())) {
        alerts_set_enabled(!alerts_enabled());
      }

      div(flex_shrink_0, h(px(1)), my(3), bg(k->border)) {}

      div(flex, flex_col, gap(1), flex_shrink_0) {
        text("Sounds", font_weight(semibold), text_size(px(20)),
             line_height(px(24)), text_color(k->strong));
        text("Discord's own, at the volume of your speaker and through "
             "the output of your system.",
             text_size(px(14)), line_height(px(18)), text_color(k->subtle));
      }

      sound_row(app, SOUND_MESSAGE1);

      for (size_t i = 0; i < CUE_SOUNDS_COUNT; i++) {
        sound_row(app, CUE_SOUNDS[i]);
      }

      sound_row(app, SOUND_CALL_RINGING);
      sound_row(app, SOUND_CALL_CALLING);
    }
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
  uint8_t shown = app->settings_page;

  overlay() {
    div(key(id("settings-backdrop")), left(0), top(0), w(px(width)),
        h(px(height)), flex, items_center, justify_center,
        bg(k->scrim)) {
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
            text(PAGE_NAMES[app->settings_page], flex_1, min_w(0), truncate,
                 font_weight(medium), text_color(k->strong));

            if (icon_button(app, id("settings-close"), CIVUE_ICON(mingcute, close_fill),
                            32, 20, nullptr)) {
              close = true;
            }
          }

          div(key(body), flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
              scrollbars(&app->bars)) {
            if (app->settings_page == SETTINGS_APPEARANCE) {
              appearance_page(app, full, width <= STACK_UNDER);
            }
            else if (app->settings_page == SETTINGS_NOTIFICATIONS) {
              notifications_page(app, full);
            }
            else {
              page(app, full, width <= STACK_UNDER, tops);
            }
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
  else if (app->settings_page != shown) {
    civue_set_scroll_y(ui, body, 0);
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
