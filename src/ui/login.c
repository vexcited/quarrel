#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "platform/platform.h"
#include "widgets.h"

constexpr float CARD_WIDTH = 480;
// discord's expanded auth box, the form beside the qr code.
constexpr float WIDE_CARD_WIDTH = 784;
constexpr float QR_PANEL_WIDTH = 240;
constexpr float QR_BOX = 176;
constexpr float QR_PADDING = 8;
constexpr float QR_LOGO = 50;
constexpr float SCANNED_AVATAR = 120;

static void heading(const Look *k, const char *title, const char *subtitle) {
  div(flex, flex_col, items_center, gap(2), mb(5)) {
    text(title, text_size(px(24)), line_height(px(30)), font_weight(semibold),
         text_color(k->strong), text_center);
    text(subtitle, text_color(k->subtle), text_center);
  }
}

static void field_label(const Look *k, const char *label) {
  div(flex, gap(1), mb(2)) {
    text(label, font_weight(medium));
    text("*", text_color(k->critical_text));
  }
}

static bool primary_button(const Look *k, civue_id id, const char *label,
                           bool enabled) {
  bool pressed = false;
  civue_color ground = enabled ? k->brand : civue_alpha(k->brand, .5f);

  div(key(id), flex, items_center, justify_center, w(full), h(10), rounded(lg),
      bg(ground), text_color(CIVUE_WHITE), font_weight(medium),
      style(.hover = {.bg = k->brand_hover, .has_bg = enabled},
            .active = {.bg = k->brand_active, .has_bg = enabled},
            .cursor = enabled ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    pressed = button().activated && enabled;
    text(label);
  }

  return pressed;
}

static bool link_button(const Look *k, civue_id id, const char *label) {
  bool pressed = false;

  div(key(id), self_start, text_color(k->link), text_size(px(14)),
      line_height(px(18)), cursor_pointer) {
    pressed = button().activated;
    text(label);
  }

  return pressed;
}

static void drop_last_character(char *s) {
  size_t n = strlen(s);

  while (n > 0 && ((unsigned char)s[n - 1] & 0xc0) == 0x80) {
    n--;
  }

  if (n > 0) {
    n--;
  }

  s[n] = 0;
}

static void append(char **value, const char *more, size_t n) {
  size_t had = *value ? strlen(*value) : 0;

  *value = realloc(*value, had + n + 1);
  memcpy(*value + had, more, n);
  (*value)[had + n] = 0;
}

// a field showing a dot for each character, typed at its end only: civue's
// fields have no way to hide what they hold.
static bool password_field(App *app, civue_id id, char **value) {
  civue_ui *ui = civue_ui_current();
  civue_field_look look = field_look(&app->look);
  bool submitted = false;

  div(key(id), w(full), h(10)) {
    civue_rect r = here();
    civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);

    if (resp.hovered) {
      civue_set_cursor(ui, CIVUE_CURSOR_TEXT);
    }

    if (resp.pressed) {
      ui->focus = id;
    }

    bool focused = ui->focus == id;
    civue_field_box box = focused ? look.editing : look.idle;

    if (focused) {
      civue_claim_focus(ui);
      civue_claim_secure_input(ui);

      for (uint32_t i = 0; i < ui->input.keys.len; i++) {
        civue_key_press press = ui->input.keys.v[i];
        bool command = press.mods & CIVUE_MOD_CMD;

        if (press.key == CIVUE_KEY_BACKSPACE && *value) {
          if (command) {
            **value = 0;
          }
          else {
            drop_last_character(*value);
          }
        }
        else if (press.key == CIVUE_KEY_ENTER) {
          submitted = true;
        }
        else if (press.key == 'v' && command) {
          char *pasted = civue_clipboard_text();

          if (pasted) {
            append(value, pasted, strcspn(pasted, "\r\n"));
            free(pasted);
          }
        }
      }

      if (ui->input.text.len) {
        append(value, (const char *)ui->input.text.v, ui->input.text.len);
      }
    }

    civue_fill_stroke(ui, r, box.bg, box.border, box.border_width, box.radius);

    size_t count = 0;

    for (const char *c = *value ? *value : ""; *c; c++) {
      count += ((unsigned char)*c & 0xc0) != 0x80;
    }

    double x = r.x0 + look.pad;
    double middle = (r.y0 + r.y1) / 2;

    for (size_t i = 0; i < count && x < r.x1 - look.pad; i++) {
      fill(xywh(x, middle - 3, 6, 6), box.text, 3);
      x += 10;
    }

    // the caret blinks every half second.
    if (focused) {
      if (fmod(ui->now, 1) < .5) {
        fill(xywh(x, middle - 10, 1.5, 20), box.text, 0);
      }

      civue_request_repaint_after(ui, .5 - fmod(ui->now, .5) + .01);
    }
  }

  return submitted;
}

static void error_text(App *app) {
  if (app->session->error) {
    text(app->session->error, text_size(px(14)), line_height(px(18)),
         text_color(app->look.critical_text), mt(3));
  }
}

static void credentials(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  civue_id email = id("login-email"), password = id("login-password");
  bool busy = session->state == SESSION_LOGGING_IN;
  bool submit = false;

  heading(k, "Welcome back!", "We're so excited to see you again!");

  field_label(k, "Email or Phone Number");

  div(key(email), w(full), h(10), mb(5)) {
    if (text_field(&app->login, "", field_look(k)).submitted) {
      ui->focus = password;
    }
  }

  // tab goes from the email to the password.
  if (ui->focus == email && civue_key_pressed(ui, CIVUE_KEY_TAB)) {
    ui->focus = password;
  }

  field_label(k, "Password");
  submit = password_field(app, password, &app->password);

  div(mt(1), mb(5)) {
    if (link_button(k, id("login-forgot"), "Forgot your password?")) {
      civue_open_url("https://discord.com/login");
    }
  }

  submit |= primary_button(k, id("login-submit"),
                           busy ? "Logging in..." : "Log In", !busy);

  if (submit && !busy && app->login && *app->login && app->password &&
      *app->password) {
    session_login(session, app->login, app->password);
  }

  error_text(app);

  div(flex, items_center, gap(1), mt(3), text_size(px(14)),
      line_height(px(18))) {
    text("Need an account?", text_color(k->subtle));

    if (link_button(k, id("login-register"), "Register")) {
      civue_open_url("https://discord.com/register");
    }
  }

  account_choices(app);
}

// discord's wandering cubes: two squares chasing each other around a 32
// point box, shrinking to half on the sides, every 1.8 seconds.
static void wandering_cubes(const Look *k) {
  static const double xs[] = {0, 22, 22, 0, 0};
  static const double ys[] = {0, 0, 22, 22, 0};
  static const double scales[] = {1, .5, 1, .5, 1};
  civue_ui *ui = civue_ui_current();

  div(size(px(32)), flex_shrink_0) {
    civue_rect r = here();

    for (int i = 0; i < 2; i++) {
      double t = fmod(ui->now / 1.8 + i * .5, 1) * 4;
      int at = (int)t;
      double f = t - at;

      // ease-in-out between each two steps.
      f = f * f * (3 - 2 * f);

      double x = xs[at] + (xs[at + 1] - xs[at]) * f;
      double y = ys[at] + (ys[at + 1] - ys[at]) * f;
      double side = 10 * (scales[at] + (scales[at + 1] - scales[at]) * f);

      fill(xywh(r.x0 + x + 5 - side / 2, r.y0 + y + 5 - side / 2, side, side),
           k->brand, 0);
    }

    civue_request_repaint_after(ui, 1. / 60);
  }
}

// words that wrap between them, each line centered.
static void centered_words(const char *s, const civue_color *strong) {
  char word[64];

  for (const char *at = s + strspn(s, " "); *at;) {
    size_t n = strcspn(at, " ");

    if (n < sizeof word) {
      memcpy(word, at, n);
      word[n] = 0;

      if (strong) {
        text(word, font_weight(semibold), text_color(*strong));
      }
      else {
        text(word);
      }
    }

    at += n;
    at += strspn(at, " ");
  }
}

// the code's modules, edges on whole pixels so that it stays sharp.
static void qr_modules(const char *url, civue_rect box) {
  static char *drawn;
  static uint8_t *modules;
  static size_t side;
  civue_ui *ui = civue_ui_current();

  if (!drawn || strcmp(drawn, url)) {
    free(drawn);
    free(modules);
    drawn = strdup(url);
    modules = platform_qr(url, &side);
  }

  if (!modules) {
    return;
  }

  double scale = ui->scale > 0 ? ui->scale : 1;
  double cell = (civue_rect_w(box) - QR_PADDING * 2) / (double)side;
  double x0 = box.x0 + QR_PADDING, y0 = box.y0 + QR_PADDING;

  for (size_t row = 0; row < side; row++) {
    double top = round((y0 + row * cell) * scale) / scale;
    double bottom = round((y0 + (row + 1) * cell) * scale) / scale;

    for (size_t col = 0; col < side;) {
      if (!modules[row * side + col]) {
        col++;
        continue;
      }

      size_t end = col;

      while (end < side && modules[row * side + end]) {
        end++;
      }

      double left = round((x0 + col * cell) * scale) / scale;
      double right = round((x0 + end * cell) * scale) / scale;

      fill(xywh(left, top, right - left, bottom - top), CIVUE_BLACK, 0);
      col = end;
    }
  }
}

static void qr_code(const Look *k, const SessionQr *qr) {
  bool ready = qr->step == SESSION_QR_READY && qr->url;

  div(relative, flex, items_center, justify_center, size(px(QR_BOX)),
      flex_shrink_0, mb(8), rounded(sm),
      style(.bg = ready ? CIVUE_WHITE : CIVUE_CLEAR)) {
    if (!ready) {
      wandering_cubes(k);
    }
    else {
      qr_modules(qr->url, here());

      div(flex, items_center, justify_center, size(px(QR_LOGO)),
          rounded(full), bg(CIVUE_BLACK)) {
        icon(ic, baseline_discord, size(px(28)), text_color(CIVUE_WHITE));
      }
    }
  }
}

static void qr_waiting(const Look *k, const SessionQr *qr) {
  qr_code(k, qr);

  text("Log in with QR Code", text_size(px(24)), line_height(px(30)),
       font_weight(semibold), text_color(k->strong), text_center, mb(2));

  div(flex, flex_wrap, justify_center, gap_x(px(4)), text_color(k->text)) {
    centered_words("Scan this with the", nullptr);
    centered_words("Discord mobile app", &k->strong);
    centered_words("to log in instantly.", nullptr);
  }
}

static void qr_scanned(App *app, const SessionQr *qr) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX], tag[64], line[96];
  bool discriminated = qr->discriminator && strcmp(qr->discriminator, "0");

  cdn_avatar(url, qr->user_id, qr->avatar, qr->discriminator,
             (uint32_t)SCANNED_AVATAR * 2);

  div(size(px(SCANNED_AVATAR)), flex_shrink_0, mb(8), rounded(full),
      bg(k->surface_high)) {
    civue_rect r = here();
    double dot = 24;

    image_fill(app, url, SCANNED_AVATAR / 2);
    status_dot(STATUS_ONLINE, xywh(r.x1 - dot - 4, r.y1 - dot - 4, dot, dot),
               k, k->surface);
  }

  snprintf(tag, sizeof tag, "%s%s%s", qr->username ? qr->username : "",
           discriminated ? "#" : "", discriminated ? qr->discriminator : "");
  snprintf(line, sizeof line, "Logging in as %s", tag);

  text("Check your phone!", text_size(px(24)), line_height(px(30)),
       font_weight(semibold), text_color(k->strong), text_center, mb(2));
  text(line, text_color(k->text), text_center);

  div(mt(4)) {
    div(key(id("qr-restart")), text_color(k->brand), text_size(px(14)),
        line_height(px(18)), cursor_pointer) {
      if (button().activated) {
        session_qr_restart(app->session);
      }

      text("Not me, start over");
    }
  }
}

static void qr_panel(App *app) {
  const SessionQr *qr = &app->session->qr;

  div(flex, flex_col, items_center, justify_center, w(px(QR_PANEL_WIDTH)),
      flex_shrink_0, overflow_hidden) {
    switch (qr->step) {
      case SESSION_QR_SCANNED:
        qr_scanned(app, qr);
        break;
      case SESSION_QR_LOGGING_IN:
        wandering_cubes(&app->look);
        break;
      default:
        qr_waiting(&app->look, qr);
        break;
    }
  }
}

static void method_choice(App *app, SessionMfaMethod method, const char *label,
                          bool offered) {
  const Look *k = &app->look;
  bool chosen = app->mfa_method == method;

  if (!offered) {
    return;
  }

  div(key(id_at(id("mfa-method"), method)), px(3), py(1p5), rounded(lg),
      border(1), border_color(chosen ? k->brand : k->border_strong),
      style(.bg = chosen ? civue_alpha(k->brand, .2f) : CIVUE_CLEAR),
      hover(bg(k->hover)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), cursor_pointer) {
    if (button().activated) {
      app->mfa_method = method;
    }

    text(label);
  }
}

static void second_factor(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  SessionMfa mfa = session->mfa;
  bool submit = false;

  // the first way discord offers, when the chosen one is not.
  bool offered[] = {mfa.totp, mfa.sms, mfa.backup};

  if (!offered[app->mfa_method]) {
    app->mfa_method = mfa.totp  ? SESSION_MFA_TOTP
                      : mfa.sms ? SESSION_MFA_SMS
                                : SESSION_MFA_BACKUP;
  }

  const char *subtitle = app->mfa_method == SESSION_MFA_TOTP
                           ? "Enter the code from your authenticator app."
                         : app->mfa_method == SESSION_MFA_SMS
                           ? "Enter the code we text to your phone."
                           : "Enter one of your 8 character backup codes.";

  heading(k, "Multi-Factor Authentication", subtitle);

  div(flex, flex_wrap, gap(2), mb(4)) {
    method_choice(app, SESSION_MFA_TOTP, "Authenticator app", mfa.totp);
    method_choice(app, SESSION_MFA_SMS, "Text message", mfa.sms);
    method_choice(app, SESSION_MFA_BACKUP, "Backup code", mfa.backup);
  }

  if (app->mfa_method == SESSION_MFA_SMS && !mfa.sms_sent) {
    if (primary_button(k, id("mfa-sms"), "Send me a code", true)) {
      session_request_sms(session);
    }
  }
  else {
    field_label(k, "Code");

    div(key(id("mfa-code")), w(full), h(10), mb(5)) {
      submit = text_field(&app->code, "", field_look(k)).submitted;
    }

    submit |= primary_button(k, id("mfa-submit"), "Confirm", true);
  }

  if (submit && app->code && *app->code) {
    session_submit_mfa(session, app->mfa_method, app->code);
    free(app->code);
    app->code = nullptr;
  }

  error_text(app);

  div(mt(3)) {
    if (link_button(k, id("mfa-back"), "Go back to login")) {
      session_cancel_login(session);
    }
  }
}

static void drop_captcha(App *app) {
  if (app->captcha) {
    hcaptcha_free(app->captcha);
    app->captcha = nullptr;
  }
}

static void challenge(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  const SessionCaptcha *asked = &session->captcha;
  bool known =
    asked->service && !strcmp(asked->service, "hcaptcha") && asked->sitekey;

  heading(k, "Wait! Are you human?",
          "Please confirm you're not a robot to continue.");

  if (!known) {
    text("Discord asks for a challenge Quarrel cannot show.",
         text_color(k->critical_text), text_center);
  }
  else {
    // each challenge discord sends is a new one.
    if (app->captcha && app->captcha_version != session->version) {
      drop_captcha(app);
    }

    if (!app->captcha) {
      app->captcha = hcaptcha_new(&(hcaptcha_options){
        .sitekey = asked->sitekey,
        .origin = "https://discord.com",
        .rqdata = asked->rqdata,
        .theme = "dark",
      });
      app->captcha_version = session->version;
    }

    hcaptcha_state state = {.status = HCAPTCHA_FAILED,
                            .error = "web views are missing here"};

    div(w(full), h(px(460))) {
      if (app->captcha) {
        state = hcaptcha_node(civue_ui_current(), app->captcha);
      }

      if (state.status == HCAPTCHA_LOADING) {
        text("Loading the challenge...", text_color(k->muted), text_center,
             mt(8));
      }
    }

    if (state.status == HCAPTCHA_SOLVED && state.token) {
      session_submit_captcha(session, state.token);
      drop_captcha(app);
      return;
    }

    if (state.status == HCAPTCHA_CANCELLED ||
        state.status == HCAPTCHA_EXPIRED || state.status == HCAPTCHA_FAILED) {
      const char *why = state.status == HCAPTCHA_FAILED && state.error
                          ? state.error
                          : "The challenge was closed before it was solved.";

      text(why, text_size(px(14)), line_height(px(18)),
           text_color(k->critical_text), mb(3));

      if (app->captcha &&
          primary_button(k, id("captcha-retry"), "Try again", true)) {
        hcaptcha_reset(app->captcha);
      }
    }
  }

  div(mt(3)) {
    if (link_button(k, id("captcha-back"), "Go back to login")) {
      drop_captcha(app);
      session_cancel_login(session);
    }
  }
}

void login_screen(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  bool form = session->state != SESSION_MFA &&
              session->state != SESSION_CAPTCHA;

  if (session->state != SESSION_CAPTCHA) {
    drop_captcha(app);
  }

  // a second factor being checked shows the form, not a new code.
  session_qr(session, form && session->mfa_ticket == nullptr);

  div(size(full), flex, items_center, justify_center, bg(k->frame),
      text_color(k->text), text_size(px(16)), line_height(px(20))) {
    div(flex, flex_col, w(px(form ? WIDE_CARD_WIDTH : CARD_WIDTH)), p(8),
        rounded(lg), bg(k->surface), border(1), border_color(k->border),
        shadow(xl)) {
      if (session->state == SESSION_MFA) {
        second_factor(app);
      }
      else if (session->state == SESSION_CAPTCHA) {
        challenge(app);
      }
      else {
        div(flex, items_center, gap(16)) {
          div(flex, flex_col, flex_1, min_w(0)) {
            credentials(app);
          }

          qr_panel(app);
        }
      }
    }
  }
}

void connecting_screen(App *app) {
  const Look *k = &app->look;

  div(size(full), flex, flex_col, items_center, justify_center, gap(4),
      bg(k->frame), text_color(k->text), text_size(px(16)),
      line_height(px(20))) {
    icon(ic, baseline_discord, size(16), text_color(k->brand));
    text("Connecting...", font_weight(semibold), text_color(k->strong));

    if (app->session->error) {
      text(app->session->error, text_color(k->critical_text));
    }
  }
}
