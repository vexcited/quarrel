#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float CARD_WIDTH = 480;

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
static bool secret_field(App *app, civue_id id, char **value) {
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
  submit = secret_field(app, password, &app->password);

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

  if (session->state != SESSION_CAPTCHA) {
    drop_captcha(app);
  }

  div(size(full), flex, items_center, justify_center, bg(k->frame),
      text_color(k->text), text_size(px(16)), line_height(px(20))) {
    div(flex, flex_col, w(px(CARD_WIDTH)), p(8), rounded(lg), bg(k->surface),
        border(1), border_color(k->border), shadow(xl)) {
      if (session->state == SESSION_MFA) {
        second_factor(app);
      }
      else if (session->state == SESSION_CAPTCHA) {
        challenge(app);
      }
      else {
        credentials(app);
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
