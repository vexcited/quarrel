#include <stdio.h>
#include <stdlib.h>

#include <civue/dsl.h>

#include "http.h"
#include "ui/alerts.h"
#include "ui/authorize.h"
#include "ui/complete.h"
#include "ui/components.h"
#include "ui/cues.h"
#include "ui/explore.h"
#include "ui/media.h"
#include "ui/server.h"
#include "ui/widgets.h"

#define SIDEBAR_KEY "sidebar_width"

// the message under the pointer changes once a frame, and stays while the
// pointer is over its toolbar, which reaches over the message before it.
static void settle_hover(App *app, civue_ui *ui) {
  if (ui->rerun) {
    return;
  }

  bool held = app->hover_message && ui->input.has_mouse &&
              civue_rect_contains(app->toolbar_rect, ui->input.mouse);

  if (!held) {
    app->hover_message = app->hover_next;
  }

  app->toolbar_rect = (civue_rect){};
}

static void sidebar(App *app) {
  Guild *guild = app_guild(app);

  div(flex, flex_col, flex_shrink_0, w(px(app->sidebar_width)), h(full)) {
    top_bar(app);
    place_bar(app);

    if (guild) {
      channel_list(app, guild);
    }
    else {
      private_list(app);
    }

    voice_panel(app);
  }
}

// the sidebar's edge is dragged to size it, and pressed twice to bring its
// width back. the width is kept once the edge is let go.
static void sidebar_handle(App *app) {
  civue_ui *ui = civue_ui_current();
  float edge = app->sidebar_width;
  civue_response grip = civue_interact(
    ui, id("sidebar-handle"), xywh(edge - 3, TOP_BAR_HEIGHT, 6, ui->size.y),
    CIVUE_SENSE_DRAG);
  float width = edge;

  if (grip.hovered || grip.dragging) {
    civue_set_cursor(ui, CIVUE_CURSOR_RESIZE_H);
  }

  if (grip.double_clicked) {
    width = SIDEBAR_WIDTH;
  }
  else if (grip.dragging && ui->input.has_mouse) {
    width = ui->input.mouse.x;
  }

  width = width < SIDEBAR_MIN ? SIDEBAR_MIN : width;
  width = width > SIDEBAR_MAX ? SIDEBAR_MAX : width;

  if (width != app->sidebar_width) {
    app->sidebar_width = width;
    civue_window_wake();
  }

  if (grip.released || grip.double_clicked) {
    char digits[16];

    snprintf(digits, sizeof digits, "%d", (int)width);
    platform_pref_set(SIDEBAR_KEY, digits);
  }
}

// windows 11 rounds its windows by 8 points, and the panel's corners follow
// them there.
#ifdef _WIN32
constexpr float PANEL_RADIUS = 8;
#else
constexpr float PANEL_RADIUS = 12;
#endif

// where the window's buttons, at the end of the title bar as on windows, cut
// into the panel: as far from them as the panel is from the window's edge.
static civue_rect notch_of(civue_rect panel) {
  civue_ui *ui = civue_ui_current();
  double gap = ui->size.x - panel.x1;

  if (ui->titlebar_end_inset <= 0 || civue_rect_w(panel) <= 0 ||
      panel.y0 >= ui->titlebar_height) {
    return (civue_rect){};
  }

  return civue_rect_from(ui->size.x - ui->titlebar_end_inset - gap, panel.y0,
                         panel.x1, ui->titlebar_height);
}

static civue_color over(civue_color top, civue_color under) {
  float a = top.a;

  return (civue_color){top.r * a + under.r * (1 - a),
                       top.g * a + under.g * (1 - a),
                       top.b * a + under.b * (1 - a), 1};
}

// rounds the panel's top right corner at `at`: the frame outside its round,
// as the inside of a border on the top and the right of a box around it, and
// the panel's border along it.
static void round_corner(civue_ui *ui, civue_vec2 at, float r,
                         civue_color frame, civue_color line) {
  civue_stroke_sides(
    ui, civue_rect_from(at.x - 3 * r, at.y - r, at.x + r, at.y + 3 * r), frame,
    (float[4]){r, r, 0, 0}, (float[4]){0, 2 * r, 0, 0});
  civue_stroke_sides(ui,
                     civue_rect_from(at.x - 2 * r, at.y, at.x, at.y + 2 * r),
                     line, (float[4]){1, 1, 0, 0}, (float[4]){0, r, 0, 0});
}

// the panel going around the window's buttons: the frame over its corner,
// round where the panel turns, with the panel's border along it.
static void paint_notch(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect n = app->notch;
  float r = PANEL_RADIUS;
  // a border is whole pixels.
  float bw = fmaxf(roundf(ui->scale), 1) / ui->scale;
  // the border as it shows over the panel. its pieces meet and cross here,
  // and an opaque one stays the same where they do.
  civue_color line = over(k->frame_border, k->chat);

  if (civue_rect_w(n) <= 0) {
    return;
  }

  civue_fill_corners(ui, n, k->frame, (float[4]){0, 0, 0, r});
  civue_stroke_sides(ui, civue_rect_from(n.x0 - bw, n.y0, n.x1, n.y1 + bw),
                     line, (float[4]){0, 0, 1, 1}, (float[4]){0, 0, 0, r + bw});
  round_corner(ui, civue_v2(n.x0, n.y0), r, k->frame, line);
  round_corner(ui, civue_v2(n.x1, n.y1), r, k->frame, line);
}

static void shell(App *app) {
  const Look *k = &app->look;
  Channel *channel = app_channel(app);

  div(size(full), flex, bg(k->frame), text_color(k->text), text_size(px(16)),
      line_height(px(22))) {
    sidebar(app);

    div(flex, flex_col, flex_1, min_w(0), h(full), py(2), pr(2)) {
      preview_bar(app);

      div(flex, flex_1, min_w(0), min_h(0), rounded(px(PANEL_RADIUS)),
          bg(k->chat), border(1), border_color(k->frame_border),
          overflow_hidden) {
        app->notch = notch_of(here());

        if (channel && channel_is_forum(channel)) {
          forum_page(app, channel);
        }
        else if (channel && channel->type == CHANNEL_GUILD_DIRECTORY) {
          directory_page(app, channel);
        }
        else if (channel) {
          chat(app, channel);
        }
        else if (app->exploring) {
          explore_page(app);
        }
        else {
          friends_page(app);
        }
      }
    }
  }

  paint_notch(app);
  sidebar_handle(app);
}

// QUARREL_CHANNEL opens a channel by its id once the store is ready, to land
// where a fixture is worth looking at.
static void open_asked(App *app) {
  static bool done;
  const char *asked = getenv("QUARREL_CHANNEL");

  if (done) {
    return;
  }

  done = true;

  if (asked) {
    app_select_channel(app, snowflake_parse(asked));
  }
}

static void frame(void *ctx, civue_ui *ui) {
  App *app = ctx;
  Session *session = app->roster->current;

  app->session = session;
  ui->appearance = CIVUE_APPEARANCE_DARK;
  ui->scrollbars = nullptr;
  settle_hover(app, ui);

  if (app->account_id != session->account_id) {
    app_reset(app);
  }

  alerts_frame(app);

  app->covers_buttons = false;

  switch (session->state) {
    case SESSION_READY:
    case SESSION_RECONNECTING:
      open_asked(app);
      explore_frame(app);
      shell(app);
      profile_popout(app);
      profile_modal(app);
      server_settings(app);
      channel_dialogs(app);
      settings_dialog(app);
      voice_switch_dialog(app);
      call_cards(app);
      hub_dialogs(app);
      authorize_dialog(app);
      media_viewer(app);
      notice_toast(app);
      break;
    case SESSION_CONNECTING:
      connecting_screen(app);
      break;
    default:
      login_screen(app);
      break;
  }

  media_sweep(app);
  gif_players_sweep(app);

  if (app->covers_buttons != app->buttons_hidden) {
    app->buttons_hidden = app->covers_buttons;
    civue_window_buttons_hidden(app->buttons_hidden);
  }
}

static civue_color background(void *ctx) {
  App *app = ctx;

  return app->look.frame;
}

// a session changed: the window draws it, what was read since needs no
// notification anymore, and what changed of a call has its sound.
static void on_change(void) {
  civue_window_wake();
  alerts_sweep();
  cues_follow();
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  App *app = arg;

  platform_stay_responsive();
  app->images = images_new();
  app->roster = roster_new(&(SessionOptions){
    .on_change = on_change,
    .on_message = alerts_message,
  });
  alerts_start(app);
  cues_start(app);
  app->session = app->roster->current;
  roster_start(app->roster);
  app->session = app->roster->current;

  return (civue_app){app, frame, background};
}

int main() {
  App app = {.theme = theme_saved(), .stick = true, .directory_tab = -1};
  char *width = platform_pref_get(SIDEBAR_KEY);

  app.look = look_of(app.theme);

  app.sidebar_width = width ? (float)atof(width) : SIDEBAR_WIDTH;
  free(width);

  if (app.sidebar_width < SIDEBAR_MIN || app.sidebar_width > SIDEBAR_MAX) {
    app.sidebar_width = SIDEBAR_WIDTH;
  }

  app.bars = scrollbars_of(&app, 12);
  app.thin_bars = scrollbars_of(&app, 8);

  civue_window_options options = {
    .title = "Quarrel",
    .width = 1280,
    .height = 800,
    .min_width = 940,
    .min_height = 500,
    .titlebar = TOP_BAR_HEIGHT,
    .app_id = "quarrel",
  };

  civue_window_run(&options, make, &app);

  captcha_drop(&app.captcha);
  captcha_drop(&app.friend_captcha);

  media_free(&app);
  gif_players_free(&app);
  complete_free(&app);
  emoji_picker_free(&app);
  free(app.card_emoji_name);
  server_ui_free(&app);
  alerts_free();
  roster_free(app.roster);
  civue_images_free(app.images);
  http_shutdown();

  free(app.login);
  free(app.password);
  free(app.code);
  free(app.draft);
  free(app.gif_search);
  free(app.gif_title);
  free(app.sticker_search);
  free(app.sticker_folded.v);
  free(app.kept_draft);
  free(app.profile_message);
  free(app.profile_note);
  free(app.friends_search);
  free(app.friend_tag);
  free(app.friend_note);
  free(app.dm_search);
  free(app.pick_search);
  free(app.guild_name);
  free(app.menu_search);
  free(app.forum_search);
  free(app.post_title);
  free(app.post_body);
  free(app.forum_tags.v);
  free(app.post_tags.v);
  free(app.directory_field);
  free(app.directory_query);
  free(app.explore_field);
  apps_ui_free(&app);
  authorize_free(&app);
  free(app.new_server_name);
  free(app.entry_description);
  free(app.hub_email);
  free(app.hub_code);
  free(app.hub_school);
  free(app.hub_search);
  free(app.nick);
  free(app.hide_muted.v);
  free(app.picks.v);
  platform_timer_cancel(app.friends_tick);
  free(app.last_channels.v);
  free(app.revealed.v);

  return 0;
}
