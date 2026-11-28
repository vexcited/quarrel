#include "components.h"

#include <math.h>

#include <civue/dsl.h>

#include "widgets.h"

// shown while what the window holds may be late: the connection to discord
// is being made, dropped, or there is no network at all. a link back within
// LINK_GRACE seconds was a blink, not worth a word.
constexpr double LINK_GRACE = 1;
constexpr float LINK_IN_SECONDS = .2f;
constexpr float LINK_OUT_SECONDS = .15f;
// seconds for one pulse of the link icon.
constexpr double PULSE = 1.6;

typedef enum {
  LINK_CONNECTING,
  LINK_DOWN,
  LINK_OFFLINE,
} LinkTrouble;

typedef struct {
  bool troubled;
  double since;
  LinkTrouble trouble;
} LinkSeen;

void link_toast(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id at = id("link-toast");
  LinkSeen *seen =
    civue_memory(ui, civue_ids(at, "seen"), sizeof *seen, nullptr);
  SessionLink link = session_link(app->session);
  bool online = platform_online();
  bool up = link == SESSION_LINK_UP && online;

  if (up) {
    seen->troubled = false;
  }
  else {
    if (!seen->troubled) {
      seen->troubled = true;
      seen->since = ui->now;
    }

    seen->trouble = !online                    ? LINK_OFFLINE
                    : link == SESSION_LINK_DOWN ? LINK_DOWN
                                                : LINK_CONNECTING;
  }

  double waited = up ? 0 : ui->now - seen->since;
  bool on = !up && waited >= LINK_GRACE;

  if (!up && !on) {
    civue_request_repaint_after(ui, LINK_GRACE - waited);
  }

  float shown = civue_animate(ui, civue_ids(at, "shown"), on ? 1 : 0,
                              on ? LINK_IN_SECONDS : LINK_OUT_SECONDS);

  if (shown < .02f) {
    return;
  }

  bool down = seen->trouble != LINK_CONNECTING;
  const char *why = seen->trouble == LINK_OFFLINE ? "No internet connection"
                    : down ? "Disconnected, trying again soon"
                           : "Connecting to Discord...";
  civue_color color = down ? k->critical_text : k->subtle;
  float rise = ui->reduce_motion ? 0 : (1 - shown) * 8;

  // an attempt under way breathes, a slow fade out and back in.
  if (!down) {
    color.a *= .65f + .35f * (float)cos(ui->now * 2 * M_PI / PULSE);
    civue_request_repaint_after(ui, 1.0 / 30);
  }

  div(key(at), absolute, left(0), right(0), bottom(px(8 - rise)), px(2)) {
    faded(shown) {
      div(flex, items_center, gap(2), px(3), py(2), rounded(lg),
          bg(k->popover), border(1), border_color(k->border_strong),
          shadow(md), text_size(px(14)), line_height(px(18)),
          font_weight(medium), text_color(k->strong)) {
        if (down) {
          icon(mingcute, unlink_fill, size(4), flex_shrink_0,
               text_color(color));
        }
        else {
          icon(mingcute, link_fill, size(4), flex_shrink_0, text_color(color));
        }

        text(why, min_w(0), truncate);
      }
    }
  }
}

// how long a notice stays, in seconds. it sits 16 points over the foot of
// the window, as discord's toasts do.
constexpr double NOTICE_SECONDS = 3;

static void on_notice_end(void *ctx) {
  App *app = ctx;

  app->notice_timer = nullptr;
  civue_window_wake();
}

void notice_toast(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  double now = platform_now();

  // a notice of another account, or older than the one shown, is not news.
  if (app->notice_session != session) {
    app->notice_session = session;
    app->notice_seen = session->notice_version;
    app->notice_until = 0;
  }
  else if (app->notice_seen != session->notice_version) {
    app->notice_seen = session->notice_version;
    app->notice_until = now + NOTICE_SECONDS;
    platform_timer_cancel(app->notice_timer);
    app->notice_timer = platform_timer(NOTICE_SECONDS, on_notice_end, app);
  }

  if (!session->notice || now >= app->notice_until) {
    return;
  }

  civue_vec2 window = civue_ui_current()->size;
  float width = 440;

  overlay() {
    div(key(id("notice")), flex, justify_center,
        left(px(window.x / 2 - width / 2)), top(px(window.y - 16 - 36)),
        w(px(width))) {
      div(flex, items_center, gap(2), max_w(full), px(3), py(2), rounded(lg),
          bg(k->popover), border(1), border_color(k->border_strong), shadow(lg),
          text_size(px(14)), line_height(px(18)), font_weight(medium),
          text_color(k->strong)) {
        if (session->notice_failed) {
          icon(mingcute, warning_fill, size(4), flex_shrink_0,
               text_color(k->critical_text));
        }
        else {
          icon(mingcute, bookmark_fill, size(4), flex_shrink_0,
               text_color(k->subtle));
        }

        text(session->notice, min_w(0));
      }
    }
  }
}
