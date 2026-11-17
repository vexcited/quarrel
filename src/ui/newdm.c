#include "components.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float CARD_WIDTH = 340;
constexpr float CARD_HEIGHT = 460;
// a group dm holds ten people, the user among them.
constexpr uint32_t PICKS_MAX = 9;

static bool matches(const char *search, const User *user,
                    const Relationship *r) {
  if (!search || !*search) {
    return true;
  }

  return strcasestr(store_user_name(user), search) ||
         (user->username && strcasestr(user->username, search)) ||
         (r->nickname && strcasestr(r->nickname, search));
}

static bool friend_row(App *app, const User *user, bool picked, bool full) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id_at(id("pick"), user->id)), flex, items_center, gap(3),
      flex_shrink_0, h(11), px(2), rounded(lg), hover(bg(k->hover)),
      cursor_pointer) {
    pressed = button().activated;

    avatar(app, user, 0, 32, true, k->surface_high);

    div(flex, flex_col, flex_1, min_w(0)) {
      text(store_user_name(user), truncate, text_size(px(14)),
           line_height(px(18)), font_weight(medium), text_color(k->strong));
      text(user->username ? user->username : "", truncate, text_size(px(12)),
           line_height(px(16)), text_color(k->subtle));
    }

    if (picked) {
      div(flex, items_center, justify_center, size(5), flex_shrink_0,
          rounded(md), bg(k->brand), text_color(CIVUE_WHITE)) {
        icon(lucide, check, size(px(14)));
      }
    }
    else {
      div(size(5), flex_shrink_0, rounded(md), border(1),
          border_color(k->border_strong)) {}
    }
  }

  return pressed && (picked || !full);
}

void new_dm(App *app, civue_rect anchor) {
  if (app->popover != POPOVER_NEW_DM) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  float top = (float)anchor.y1 + 6;
  float room = (float)overlay_room().y1 - top;
  float height = CARD_HEIGHT < room ? CARD_HEIGHT : room;
  civue_rect card = xywh(anchor.x0, top, CARD_WIDTH, height);
  bool full = app->picks.len >= PICKS_MAX;
  Snowflake toggled = 0;
  bool create = false;
  size_t shown = 0;
  char count[48];

  snprintf(count, sizeof count, "You can add %u more friends.",
           PICKS_MAX - app->picks.len);

  overlay() {
    div(key(id("new-dm-card")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(CARD_WIDTH)), h(px(height)), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      div(flex, flex_col, gap(1), flex_shrink_0, px(4), pt(4)) {
        text("New Message", text_size(px(16)), line_height(px(22)),
             font_weight(semibold), text_color(k->strong));
        text(count, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }

      div(key(id("pick-search")), flex_shrink_0, h(9), mx(4), my(3),
          text_size(px(14)), line_height(px(18))) {
        civue_field_look look = field_look(k);

        look.pad = 10;
        text_field(&app->pick_search, "Search friends", look);
      }

      div(key(id("pick-list")), flex, flex_col, gap(px(2)), flex_1, min_h(0),
          px(2), overflow_y_scroll, scrollbars(&app->thin_bars)) {
        for (size_t i = 0; i < store->nrelationships; i++) {
          const Relationship *r = &store->relationships[i];
          const User *user = store_user(store, r->user_id);

          if (r->type != RELATIONSHIP_FRIEND || !user ||
              !matches(app->pick_search, user, r)) {
            continue;
          }

          if (friend_row(app, user, ids_has(&app->picks, user->id), full)) {
            toggled = user->id;
          }

          shown++;
        }

        if (!shown) {
          text("No friend matches.", px(2), py(2), text_size(px(14)),
               line_height(px(18)), text_color(k->faint));
        }
      }

      div(flex_shrink_0, p(3), border_t(1), border_color(k->border)) {
        bool ready = app->picks.len > 0;

        div(key(id("pick-create")), flex, items_center, justify_center, h(9),
            rounded(lg), text_size(px(14)), line_height(px(18)),
            font_weight(medium),
            style(.bg = ready ? k->brand : k->hover,
                  .text_color = ready ? CIVUE_WHITE : k->faint,
                  .has_text_color = true,
                  .cursor = ready ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
                  .has_cursor = true)) {
          create = button().activated && ready;
          text(app->picks.len > 1 ? "Create Group DM" : "Create DM");
        }
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }

  if (toggled) {
    ids_toggle(&app->picks, toggled);
  }

  if (create) {
    app->popover = POPOVER_NONE;
    app->guild_id = 0;
    session_open_group(app->session, app->picks.v, app->picks.len);
    app->picks.len = 0;
  }
}
