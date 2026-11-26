#include "components.h"

#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/markdown.h"
#include "widgets.h"

static bool has_member_list(const Channel *c) {
  return c->guild_id || c->type == CHANNEL_GROUP_DM;
}

// the way back to the forum of a post, before its title.
static void forum_crumb(App *app, const Channel *forum) {
  const Look *k = &app->look;

  div(key(id("forum-crumb")), flex, items_center, gap(2), flex_shrink_0,
      min_w(0), max_w(px(240)), text_color(k->subtle),
      hover(text_color(k->strong)), cursor_pointer) {
    if (button().activated) {
      app_select_channel(app, forum->id);
    }

    if (forum->type == CHANNEL_GUILD_MEDIA) {
      icon(lucide, image, size(5), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(lucide, messages_square, size(5), flex_shrink_0,
           text_color(k->faint));
    }

    text(forum->name ? forum->name : "", min_w(0), truncate,
         font_weight(semibold));
  }

  icon(lucide, chevron_right, size(4), flex_shrink_0, text_color(k->faint));
}

static void follow_button(App *app, const Channel *c) {
  const Look *k = &app->look;
  bool joined = c->thread_joined;

  div(key(id("follow-post")), flex, items_center, gap(1), flex_shrink_0, h(8),
      px(3), rounded(lg), border(1),
      border_color(joined ? k->border_strong : CIVUE_CLEAR),
      style(.bg = joined ? CIVUE_CLEAR : k->brand),
      hover(bg(joined ? k->hover : k->brand_hover)), text_size(px(14)),
      line_height(px(18)), font_weight(semibold),
      text_color(joined ? k->text : CIVUE_WHITE), cursor_pointer) {
    if (button().activated) {
      session_follow(app->session, c->id, !joined);
    }

    if (joined) {
      icon(lucide, bell, size(4));
    }
    else {
      icon(lucide, bell_plus, size(4));
    }

    text(joined ? "Following" : "Follow");
    hint(app, id("follow-post"), &(civue_response){.rect = here()},
         "Add this post to your channel list and receive notifications about "
         "it.",
         CIVUE_TOOLTIP_BELOW);
  }
}

static void chat_header(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Channel *forum =
    store_is_post(store, c) ? store_channel(store, c->parent_id) : nullptr;
  char name[128];

  store_channel_name(store, c, name, sizeof name);

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("chat-header"));

    if (forum) {
      forum_crumb(app, forum);
    }
    else if (c->type == CHANNEL_DM) {
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
    else if (channel_is_thread(c)) {
      icon(lucide, message_square, size(5), flex_shrink_0,
           text_color(k->faint));
    }
    else {
      icon(lucide, hash, size(5), flex_shrink_0, text_color(k->faint));
    }

    text(name, flex_shrink_0, max_w(1_2), truncate, font_weight(semibold),
         text_color(k->strong));

    if (forum) {
      civue_rect anchor = {};

      div(flex_1) {}
      follow_button(app, c);

      div(flex_shrink_0) {
        anchor = here();

        if (icon_button(app, id("post-more"), CIVUE_ICON(lucide, ellipsis), 32,
                        20, "More")) {
          app_toggle(app, POPOVER_POST_MENU);
        }
      }

      post_menu(app, c, anchor);
    }
    else if (c->topic && *c->topic) {
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

    if (channel_is_voice(c) &&
        icon_button(app, id("call-toggle"),
                    app->voice_chat ? CIVUE_ICON(lucide, volume_2)
                                    : CIVUE_ICON(lucide, message_circle),
                    32, 20, app->voice_chat ? "Show Call" : "Show Chat")) {
      app->voice_chat = !app->voice_chat;
    }

    if (has_member_list(c) && (!channel_is_voice(c) || app->voice_chat) &&
        icon_button(
          app, id("members-toggle"), CIVUE_ICON(lucide, users), 32, 20,
          app->members_hidden ? "Show Member List" : "Hide Member List")) {
      app->members_hidden = !app->members_hidden;
    }
  }
}

void chat(App *app, Channel *c) {
  bool call = channel_is_voice(c) && !app->voice_chat;
  bool members = has_member_list(c) && !app->members_hidden;

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    chat_header(app, c);

    if (call) {
      voice_stage(app, c);
    }
    else {
      div(flex, flex_1, min_h(0)) {
        div(flex, flex_col, flex_1, min_w(0), h(full)) {
          message_list(app, c);
          composer(app, c);
          message_popovers(app, c);
          gif_picker(app, c);
        }

        if (members) {
          member_list(app, c);
        }
      }
    }
  }
}
