#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/markdown.h"
#include "widgets.h"

static bool has_member_list(const Channel *c) {
  return c->guild_id || c->type == CHANNEL_GROUP_DM;
}

static void chat_header(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char name[128];

  store_channel_name(store, c, name, sizeof name);

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    if (c->type == CHANNEL_DM) {
      const User *user =
        c->nrecipients ? store_user(store, c->recipients[0]) : nullptr;

      avatar(app, user, 0, 24, true, k->chat);
    }
    else if (c->type == CHANNEL_GROUP_DM) {
      icon(lucide, users, size(5), flex_shrink_0, text_color(k->faint));
    }
    else if (channel_is_voice(c)) {
      icon(lucide, volume_2, size(5), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(lucide, hash, size(5), flex_shrink_0, text_color(k->faint));
    }

    text(name, flex_shrink_0, max_w(1_2), truncate, font_weight(semibold),
         text_color(k->strong));

    if (c->topic && *c->topic) {
      MdDoc *doc =
        md_parse(c->topic, strlen(c->topic), &(MdOptions){.inline_only = true});
      char *plain = md_plain_text(doc);

      div(w(px), h(6), mx(2), flex_shrink_0, bg(k->border_strong)) {}
      text(plain ? plain : "", flex_1, min_w(0), truncate, text_size(px(14)),
           text_color(k->subtle));

      free(plain);
      md_free(doc);
    }
    else {
      div(flex_1) {}
    }

    if (has_member_list(c) &&
        icon_button(
          app, id("members-toggle"), CIVUE_ICON(lucide, users), 32, 20,
          app->members_hidden ? "Show Member List" : "Hide Member List")) {
      app->members_hidden = !app->members_hidden;
    }
  }
}

static void typing_line(App *app, const Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Snowflake users[3];
  size_t count = store_typing(store, c, users, 3);
  char line[320] = "";

  if (count == 1) {
    snprintf(line, sizeof line, "%s is typing...",
             store_display_name(store, c->guild_id, users[0]));
  }
  else if (count == 2) {
    snprintf(line, sizeof line, "%s and %s are typing...",
             store_display_name(store, c->guild_id, users[0]),
             store_display_name(store, c->guild_id, users[1]));
  }
  else if (count == 3) {
    snprintf(line, sizeof line, "%s, %s and %s are typing...",
             store_display_name(store, c->guild_id, users[0]),
             store_display_name(store, c->guild_id, users[1]),
             store_display_name(store, c->guild_id, users[2]));
  }
  else if (count > 3) {
    snprintf(line, sizeof line, "Several people are typing...");
  }

  div(flex, items_center, flex_shrink_0, h(6), px(4)) {
    text(line, min_w(0), truncate, text_size(px(14)), line_height(px(18)),
         font_weight(medium), text_color(k->text));
  }
}

void chat(App *app, Channel *c) {
  const Look *k = &app->look;
  bool members = has_member_list(c) && !app->members_hidden;

  div(flex, flex_col, flex_1, min_w(0), h(full), bg(k->chat)) {
    chat_header(app, c);

    div(flex, flex_1, min_h(0)) {
      div(flex, flex_col, flex_1, min_w(0), h(full)) {
        message_list(app, c);
        composer(app, c);
        typing_line(app, c);
      }

      if (members) {
        member_list(app, c);
      }
    }
  }
}
