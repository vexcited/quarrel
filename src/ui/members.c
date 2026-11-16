#include "components.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float ROW_HEIGHT = 44;

static void group_row(App *app, const Guild *guild, const MemberRow *row) {
  const Look *k = &app->look;
  const Role *role = guild && row->group == MEMBER_GROUP_ROLE
                       ? store_role(guild, row->id)
                       : nullptr;
  const char *name = row->group == MEMBER_GROUP_ONLINE    ? "Online"
                     : row->group == MEMBER_GROUP_OFFLINE ? "Offline"
                     : role && role->name                 ? role->name
                                                          : "Members";
  char label[160];

  snprintf(label, sizeof label, "%s — %u", name, row->count);

  div(flex, items_end, size(full), px(4), pb(1)) {
    text(label, min_w(0), truncate, text_size(px(14)), line_height(px(18)),
         font_weight(medium), text_color(k->faint));
  }
}

static void person(App *app, Snowflake guild_id, const User *user,
                   bool dimmed) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  uint32_t color = store_member_color(store, guild_id, user->id);
  civue_color ink = color ? rgb(color) : k->subtle;
  float fade = dimmed ? .35f : 1;

  div(flex, items_center, gap(3), h(px(42)), mx(2), px(2), rounded(lg),
      hover(bg(k->hover))) {
    avatar(app, user, guild_id, 32, !dimmed, k->chat);

    div(flex, flex_col, flex_1, min_w(0)) {
      text(store_display_name(store, guild_id, user->id), truncate,
           font_weight(medium), line_height(px(20)),
           text_color(civue_alpha(ink, fade)));

      if (user->activity && *user->activity && !dimmed) {
        text(user->activity, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->faint));
      }
    }
  }
}

static void placeholder(const Look *k) {
  div(flex, items_center, gap(3), h(px(42)), mx(2), px(2)) {
    div(size(8), rounded(full), bg(k->hover)) {}
    div(w(24), h(3), rounded(full), bg(k->hover)) {}
  }
}

static void guild_members(App *app, Channel *c) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  const MemberList *list = store_member_list(store, c);
  uint32_t first = UINT32_MAX, last = 0;

  if (!list) {
    return;
  }

  rows((uint32_t)list->nrows, ROW_HEIGHT, i) {
    const MemberRow *row = &list->rows[i];

    first = i < first ? i : first;
    last = i > last ? i : last;

    if (row->kind == MEMBER_ROW_GROUP) {
      group_row(app, guild, row);
    }
    else if (row->kind == MEMBER_ROW_MEMBER && store_user(store, row->id)) {
      const User *user = store_user(store, row->id);

      person(app, c->guild_id, user, user->status == STATUS_OFFLINE);
    }
    else {
      placeholder(&app->look);
    }
  }

  // the rows in view are the ones the gateway keeps in sync.
  if (first <= last) {
    session_member_list_range(app->session, first, last);
  }
}

static void group_members(App *app, const Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char label[48];

  snprintf(label, sizeof label, "Members — %zu", c->nrecipients + 1);
  text(label, px(4), pt(4), pb(1), text_size(px(14)), line_height(px(18)),
       font_weight(medium), text_color(k->faint));

  if (store->self) {
    person(app, 0, store->self, false);
  }

  for (size_t i = 0; i < c->nrecipients; i++) {
    const User *user = store_user(store, c->recipients[i]);

    if (user) {
      person(app, 0, user, user->status == STATUS_OFFLINE);
    }
  }
}

void member_list(App *app, Channel *c) {
  const Look *k = &app->look;

  div(key(id("members")), flex_shrink_0, w(px(MEMBER_LIST_WIDTH)), h(full),
      border_l(1), border_color(k->border), overflow_y_scroll,
      scrollbars(&app->thin_bars), pb(4)) {
    if (c->guild_id) {
      guild_members(app, c);
    }
    else {
      group_members(app, c);
    }
  }
}
