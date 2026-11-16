#include "components.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

constexpr float SWITCHER_WIDTH = 300;
constexpr float ROW_HEIGHT = 48;
constexpr float ADD_HEIGHT = 40;

static const char *account_name(const SessionAccount *a) {
  return a->global_name ? a->global_name
         : a->username  ? a->username
                        : "Account";
}

static bool account_row(App *app, const SessionAccount *a, bool current) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];
  bool pressed = false;

  cdn_avatar(url, a->id, a->avatar, a->discriminator, 64);

  div(key(id_at(id("account"), a->id)), flex, items_center, gap(3),
      flex_shrink_0, h(px(ROW_HEIGHT)), px(2), rounded(lg), hover(bg(k->hover)),
      cursor_pointer) {
    pressed = button().activated;

    div(size(8), flex_shrink_0, rounded(full), bg(k->surface_high)) {
      image_fill(app, url, 16);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      text(account_name(a), truncate, text_size(px(14)), line_height(px(18)),
           font_weight(semibold), text_color(k->strong));

      if (a->username) {
        text(a->username, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }
    }

    if (current) {
      icon(lucide, check, size(4), flex_shrink_0, text_color(k->positive));
    }
  }

  return pressed;
}

void account_switcher(App *app, civue_rect anchor, civue_rect toggle) {
  if (!app->accounts_open) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  size_t count = session->naccounts;
  float height = 16 + (float)count * ROW_HEIGHT + 9 + ADD_HEIGHT;
  civue_rect card =
    xywh(anchor.x0, anchor.y0 - 8 - height, SWITCHER_WIDTH, height);
  Snowflake chosen = 0;
  bool add = false;

  overlay() {
    div(key(id("accounts")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(SWITCHER_WIDTH)), h(px(height)), p(2),
        rounded(lg), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg)) {
      for (size_t i = 0; i < count; i++) {
        const SessionAccount *a = &session->accounts[i];

        if (account_row(app, a, a->id == session->account_id)) {
          chosen = a->id;
        }
      }

      div(flex_shrink_0, h(px(1)), my(1), bg(k->border)) {}

      div(key(id("account-add")), flex, items_center, gap(3), flex_shrink_0,
          h(px(ADD_HEIGHT)), px(2), rounded(lg), text_color(k->text),
          hover(bg(k->hover), text_color(k->strong)), text_size(px(14)),
          line_height(px(18)), font_weight(medium), cursor_pointer) {
        add = button().activated;

        div(flex, items_center, justify_center, size(8), flex_shrink_0) {
          icon(lucide, plus, size(5));
        }

        text("Add an account");
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(toggle, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->accounts_open = false;
  }

  // the session changes last: it empties the list the rows were read from.
  if (chosen) {
    app->accounts_open = false;
    session_switch_account(session, chosen);
  }
  else if (add) {
    app->accounts_open = false;
    session_add_account(session);
  }
}

void account_choices(App *app) {
  const Look *k = &app->look;
  Session *session = app->session;
  Snowflake chosen = 0;

  if (session->naccounts == 0) {
    return;
  }

  div(flex, flex_col, mt(5), pt(4), border_t(1), border_color(k->border)) {
    text("Or continue as", mb(2), text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));

    for (size_t i = 0; i < session->naccounts; i++) {
      if (account_row(app, &session->accounts[i], false)) {
        chosen = session->accounts[i].id;
      }
    }
  }

  if (chosen) {
    session_switch_account(session, chosen);
  }
}
