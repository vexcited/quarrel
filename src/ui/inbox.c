#include "components.h"

#include <stdio.h>
#include <stdlib.h>

#include <civue/dsl.h>

#include "widgets.h"

constexpr float INBOX_WIDTH = 440;
constexpr float INBOX_HEIGHT = 560;
constexpr float ROW_HEIGHT = 36;
constexpr size_t UNREADS_MAX = 200;

static bool tab_button(App *app, InboxTab tab, const char *label) {
  const Look *k = &app->look;
  bool selected = app->inbox_tab == tab;
  bool pressed = false;

  div(key(id_at(id("inbox-tab"), tab)), flex, items_center, h(8), px(3),
      rounded(lg), text_size(px(14)), line_height(px(18)), font_weight(medium),
      text_color(selected ? k->strong : k->subtle),
      style(.bg = selected ? k->selected : CIVUE_CLEAR),
      hover(bg(selected ? k->selected : k->hover), text_color(k->strong)),
      cursor_pointer) {
    pressed = button().activated;
    text(label);
  }

  return pressed;
}

static void empty(App *app, const char *title, const char *detail) {
  const Look *k = &app->look;

  div(flex, flex_col, items_center, justify_center, gap(1), flex_1, px(8)) {
    div(flex, items_center, justify_center, size(px(56)), mb(3), rounded(full),
        bg(k->hover), text_color(k->subtle)) {
      icon(lucide, inbox, size(6));
    }

    text(title, text_size(px(16)), line_height(px(22)), font_weight(semibold),
         text_color(k->strong));
    text(detail, text_center, text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));
  }
}

// whether the channel is one the user is told about, as the guild's own
// count has it.
static bool watched(Store *store, const Channel *c) {
  if (!channel_has_text(c) || channel_is_voice(c)) {
    return false;
  }

  if (channel_is_thread(c) && (!c->thread_joined || c->thread_archived)) {
    return false;
  }

  return store_can(store, c, PERM_VIEW_CHANNEL);
}

static bool waits(Store *store, const Channel *c) {
  Unread unread = store_channel_unread(store, c);

  return unread.mentions || (unread.unread && !unread.muted);
}

static void place_label(App *app, const Guild *guild) {
  const Look *k = &app->look;

  div(flex, items_center, gap(2), flex_shrink_0, h(8), px(2), mt(2)) {
    if (guild) {
      div(flex, items_center, justify_center, size(5), flex_shrink_0,
          rounded(md), overflow_hidden) {
        guild_icon(app, guild, 20, false);
      }
    }
    else {
      icon(lucide, message_circle, size(4), flex_shrink_0,
           text_color(k->subtle));
    }

    text(guild ? guild->name ? guild->name : "" : "Direct Messages", min_w(0),
         truncate, text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(k->subtle));
  }
}

// one channel with something to read. `*chosen` is set when it is opened.
static void unread_row(App *app, Channel *c, Snowflake *chosen) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Unread unread = store_channel_unread(store, c);
  civue_id read_id = id_at(id("inbox-read"), c->id);
  char name[128];

  store_channel_name(store, c, name, sizeof name);

  div(key(id_at(id("inbox-unread"), c->id)), flex, items_center, gap(2),
      flex_shrink_0, h(px(ROW_HEIGHT)), pl(2), pr(1), rounded(lg),
      hover(bg(k->hover)), cursor_pointer) {
    if (button().activated) {
      *chosen = c->id;
    }

    if (c->guild_id) {
      icon(lucide, hash, size(4), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(lucide, at_sign, size(4), flex_shrink_0, text_color(k->faint));
    }

    text(name, flex_1, min_w(0), truncate, text_size(px(14)),
         line_height(px(18)), font_weight(medium), text_color(k->strong));
    mention_badge(k, unread.mentions);

    if (icon_button(app, read_id, CIVUE_ICON(lucide, check), 28, 16,
                    "Mark as read")) {
      session_ack(app->session, c->id);
    }
  }
}

static void unreads(App *app, Snowflake *chosen) {
  Store *store = app->session->store;
  size_t count = 0, nprivates = 0, shown = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);
  Channel *const *privates = store_private_channels(store, &nprivates);
  bool labelled = false;

  div(key(id("inbox-unreads")), flex, flex_col, flex_1, min_h(0), px(2), pb(2),
      overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < nprivates && shown < UNREADS_MAX; i++) {
      if (!waits(store, privates[i])) {
        continue;
      }

      if (!labelled) {
        place_label(app, nullptr);
        labelled = true;
      }

      unread_row(app, privates[i], chosen);
      shown++;
    }

    for (size_t i = 0; i < count; i++) {
      for (size_t j = 0; j < folders[i].nguilds; j++) {
        Guild *guild = folders[i].guilds[j];
        Unread all = store_guild_unread(store, guild);

        if (!all.unread && !all.mentions) {
          continue;
        }

        labelled = false;

        for (size_t n = 0; n < guild->nchannels && shown < UNREADS_MAX; n++) {
          Channel *c = guild->channels[n];

          if (!watched(store, c) || !waits(store, c)) {
            continue;
          }

          if (!labelled) {
            place_label(app, guild);
            labelled = true;
          }

          unread_row(app, c, chosen);
          shown++;
        }
      }
    }

    if (!shown) {
      empty(app, "You're all caught up!", "Nothing is waiting to be read.");
    }
  }
}

static void mention_row(App *app, const SessionMention *m, Snowflake *chosen) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Channel *c = store_channel(store, m->channel_id);
  const Guild *guild =
    c && c->guild_id ? store_guild(store, c->guild_id) : nullptr;
  const User *author = store_user(store, m->author_id);
  Snowflake guild_id = guild ? guild->id : 0;
  char name[128] = "", place[200], stamp[48];

  if (c) {
    store_channel_name(store, c, name, sizeof name);
  }

  if (guild) {
    snprintf(place, sizeof place, "#%s · %s", name,
             guild->name ? guild->name : "");
  }
  else {
    snprintf(place, sizeof place, "%s", *name ? name : "Unknown channel");
  }

  format_stamp(snowflake_time_ms(m->id), stamp, sizeof stamp);

  div(key(id_at(id("inbox-mention"), m->id)), flex, flex_col, gap(1),
      flex_shrink_0, p(2), rounded(lg), hover(bg(k->hover)),
      style(.cursor = c ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    if (button().activated && c) {
      *chosen = c->id;
    }

    text(place, truncate, text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(k->subtle));

    div(flex, gap(3), min_w(0)) {
      avatar(app, author, guild_id, 32, false, k->surface_high);

      div(flex, flex_col, flex_1, min_w(0)) {
        div(flex, items_baseline, gap(2), min_w(0)) {
          text(store_display_name(store, guild_id, m->author_id), min_w(0),
               truncate, text_size(px(14)), line_height(px(18)),
               font_weight(semibold), text_color(k->strong));
          text(stamp, flex_shrink_0, text_size(px(12)), line_height(px(16)),
               text_color(k->faint));
        }

        if (*m->content) {
          markup(app, id_at(id("inbox-text"), m->id), m->content,
                 &(MarkupOptions){
                   .guild_id = guild_id,
                   .size = 14,
                   .hint = INBOX_WIDTH - 80,
                 });
        }
        else {
          text("Sent an attachment.", text_size(px(14)), line_height(px(18)),
               text_color(k->subtle));
        }
      }
    }
  }
}

static void mentions(App *app, Snowflake *chosen) {
  Session *session = app->session;

  if (!app->inbox_asked) {
    app->inbox_asked = true;
    session_fetch_mentions(session);
  }

  div(key(id("inbox-mentions")), flex, flex_col, gap(1), flex_1, min_h(0),
      px(2), py(2), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    for (size_t i = 0; i < session->nmentions; i++) {
      mention_row(app, &session->mentions[i], chosen);
    }

    if (session->nmentions) {
      continue;
    }

    if (session->mentions_loading || !session->mentions_loaded) {
      empty(app, session->offline ? "Nothing here." : "Loading...",
            session->offline ? "Mentions need a connection."
                             : "Fetching your recent mentions.");
    }
    else {
      empty(app, "No mentions.", "Messages that mention you show up here.");
    }
  }
}

void inbox(App *app, civue_rect anchor) {
  if (app->popover != POPOVER_INBOX) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  float top = (float)anchor.y1 + 6;
  float room = (float)overlay_room().y1 - top;
  float height = INBOX_HEIGHT < room ? INBOX_HEIGHT : room;
  civue_rect card = xywh(anchor.x0, top, INBOX_WIDTH, height);
  Snowflake chosen = 0;

  overlay() {
    div(key(id("inbox")), flex, flex_col, left(px(card.x0)), top(px(card.y0)),
        w(px(INBOX_WIDTH)), h(px(height)), rounded(xl), bg(k->surface_high),
        border(1), border_color(k->border_strong), shadow(lg),
        overflow_hidden) {
      div(flex, items_center, gap(1), flex_shrink_0, h(12), pl(4), pr(2),
          border_b(1), border_color(k->border)) {
        text("Inbox", flex_1, text_size(px(16)), line_height(px(22)),
             font_weight(semibold), text_color(k->strong));

        if (tab_button(app, INBOX_UNREADS, "Unreads")) {
          app->inbox_tab = INBOX_UNREADS;
        }

        if (tab_button(app, INBOX_MENTIONS, "Mentions")) {
          app->inbox_tab = INBOX_MENTIONS;
        }
      }

      if (app->inbox_tab == INBOX_MENTIONS) {
        mentions(app, &chosen);
      }
      else {
        unreads(app, &chosen);
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }

  if (chosen) {
    app->popover = POPOVER_NONE;
    app_select_channel(app, chosen);
  }
}
