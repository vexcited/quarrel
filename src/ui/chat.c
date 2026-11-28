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
      icon(mingcute, pic_fill, size(5), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(mingcute, messages_square_fill, size(5), flex_shrink_0,
           text_color(k->faint));
    }

    text(forum->name ? forum->name : "", min_w(0), truncate,
         font_weight(semibold));
  }

  icon(mingcute, right_fill, size(4), flex_shrink_0, text_color(k->faint));
}

static void follow_button(App *app, const Channel *c) {
  const Look *k = &app->look;
  bool joined = c->thread_joined;

  div(key(id("follow-post")), flex, items_center, gap(1), flex_shrink_0, h(8),
      px(3), rounded(lg), border(1),
      border_color(joined ? k->border_strong : CIVUE_CLEAR),
      style(.bg = joined ? CIVUE_CLEAR : k->primary),
      hover(bg(joined ? k->hover : k->primary_hover)), text_size(px(14)),
      line_height(px(18)), font_weight(semibold),
      text_color(joined ? k->text : k->on_primary), cursor_pointer) {
    if (button().activated) {
      session_follow(app->session, c->id, !joined);
    }

    if (joined) {
      icon(mingcute, notification_fill, size(4));
    }
    else {
      icon(mingcute, bell_plus_fill, size(4));
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
      icon(mingcute, group_fill, size(5), flex_shrink_0, text_color(k->faint));
    }
    else if (channel_is_voice(c)) {
      icon(mingcute, volume_fill, size(5), flex_shrink_0, text_color(k->faint));
    }
    else if (channel_is_thread(c)) {
      icon(mingcute, message_2_fill, size(5), flex_shrink_0,
           text_color(k->faint));
    }
    else {
      icon(mingcute, hashtag_fill, size(5), flex_shrink_0, text_color(k->faint));
    }

    text(name, flex_shrink_0, max_w(1_2), truncate, font_weight(semibold),
         text_color(k->strong));

    if (forum) {
      civue_rect anchor = {};

      div(flex_1) {}
      follow_button(app, c);

      div(flex_shrink_0) {
        anchor = here();

        if (icon_button(app, id("post-more"), CIVUE_ICON(mingcute, more_1_fill), 32,
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

    // a private channel is called from here, the call then shows over its
    // messages with the rest of its buttons.
    if (channel_is_private(c) && app->session->voice_channel_id != c->id) {
      bool going = store_call(store, c->id) != nullptr;

      if (icon_button(app, id("call-start"),
                      going ? CIVUE_ICON(mingcute, phone_incoming_fill)
                            : CIVUE_ICON(mingcute, phone_fill),
                      32, 20, going ? "Join Call" : "Start Voice Call")) {
        voice_join(app, c->id);
      }
    }

    if (channel_is_voice(c) &&
        icon_button(app, id("call-toggle"),
                    app->voice_chat ? CIVUE_ICON(mingcute, volume_fill)
                                    : CIVUE_ICON(ph, chat_circle_fill),
                    32, 20, app->voice_chat ? "Show Call" : "Show Chat")) {
      app->voice_chat = !app->voice_chat;
    }

    if (has_member_list(c) && (!channel_is_voice(c) || app->voice_chat) &&
        icon_button(
          app, id("members-toggle"), CIVUE_ICON(mingcute, group_fill), 32, 20,
          app->members_hidden ? "Show Member List" : "Hide Member List")) {
      app->members_hidden = !app->members_hidden;
    }

    header_end(app);
  }
}

void chat(App *app, Channel *c) {
  const Session *s = app->session;
  bool call = channel_is_voice(c) && !app->voice_chat;
  bool members = has_member_list(c) && !app->members_hidden;
  // the call of a private channel shows over its messages, as the client
  // has it.
  bool calling =
    channel_is_private(c) &&
    (s->voice_channel_id == c->id || store_call(s->store, c->id) != nullptr);
  float window = civue_ui_current()->size.y;
  float stage = window * 0.36f < 220 ? 220 : window * 0.36f;

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    chat_header(app, c);

    if (call) {
      voice_stage(app, c);
    }
    else {
      div(flex, flex_1, min_h(0)) {
        div(flex, flex_col, flex_1, min_w(0), h(full)) {
          if (calling) {
            div(key(id("private-call")), flex, flex_col, flex_shrink_0,
                h(px(stage))) {
              voice_stage(app, c);
            }
          }

          message_list(app, c);
          composer(app, c);
          message_popovers(app, c);
          component_select(app, c);
          gif_picker(app, c);
          sticker_picker(app, c);
          sticker_info(app, c);
          emoji_picker(app, c);
          emoji_info_card(app, c);
        }

        if (members) {
          member_list(app, c);
        }
      }
    }
  }
}
