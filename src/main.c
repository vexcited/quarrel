#include <stdlib.h>

#include <civue/dsl.h>

#include "http.h"
#include "ui/components.h"
#include "ui/widgets.h"

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
  const Look *k = &app->look;
  Guild *guild = app_guild(app);

  div(relative, flex, flex_shrink_0, h(full)) {
    guild_bar(app);

    div(flex, flex_col, flex_shrink_0, w(px(CHANNEL_LIST_WIDTH)), h(full),
        border_l(1), border_t(1), border_color(k->frame_border), rounded_tl(xl),
        overflow_hidden) {
      if (guild) {
        channel_list(app, guild);
      }
      else {
        private_list(app);
      }
    }

    user_panel(app);
  }
}

static void shell(App *app) {
  const Look *k = &app->look;
  Channel *channel = app_channel(app);

  div(size(full), flex, flex_col, bg(k->frame), text_color(k->text),
      text_size(px(16)), line_height(px(22))) {
    top_bar(app);

    div(flex, flex_1, min_h(0)) {
      sidebar(app);

      div(flex, flex_1, min_w(0), h(full), border_t(1),
          border_color(k->frame_border)) {
        if (channel) {
          chat(app, channel);
        }
        else {
          friends_page(app);
        }
      }
    }
  }
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
  Session *session = app->session;

  ui->appearance = CIVUE_APPEARANCE_DARK;
  ui->scrollbars = nullptr;
  settle_hover(app, ui);

  switch (session->state) {
    case SESSION_READY:
    case SESSION_RECONNECTING:
      open_asked(app);
      shell(app);
      break;
    case SESSION_CONNECTING:
      connecting_screen(app);
      break;
    default:
      login_screen(app);
      break;
  }
}

static civue_color background(void *ctx) {
  App *app = ctx;

  return app->look.frame;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  App *app = arg;

  platform_stay_responsive();
  app->images = images_new(0, nullptr);
  app->session = session_new(&(SessionOptions){.on_change = civue_window_wake});
  session_start(app->session);

  return (civue_app){app, frame, background};
}

int main() {
  App app = {.look = look_dark(), .stick = true};

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

  if (app.captcha) {
    hcaptcha_free(app.captcha);
  }

  session_free(app.session);
  images_free(app.images);
  http_shutdown();

  free(app.login);
  free(app.password);
  free(app.code);
  free(app.draft);
  free(app.kept_draft);
  free(app.friends_search);
  platform_timer_cancel(app.friends_tick);
  free(app.last_channels.v);
  free(app.revealed.v);

  return 0;
}
