#include "components.h"

#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

// discord's inbox is 560 wide, and its bookmarks are whole messages.
constexpr float INBOX_WIDTH = 560;
constexpr float INBOX_HEIGHT = 560;
constexpr float ROW_HEIGHT = 36;
constexpr size_t UNREADS_MAX = 200;
// the list of bookmarks asks for its next page this close to its end, as
// discord's client does.
constexpr float BOOKMARKS_AHEAD = 250;

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

static void empty_with(App *app, civue_icon glyph, const char *title,
                       const char *detail) {
  const Look *k = &app->look;

  div(flex, flex_col, items_center, justify_center, gap(1), flex_1, px(8)) {
    div(flex, items_center, justify_center, size(px(56)), mb(3), rounded(full),
        bg(k->hover), text_color(k->subtle)) {
      civue_node_icon(civue_ui_current(), glyph,
                      &CIVUE_NODE_STYLE(.w = civue_px(24), .h = civue_px(24)));
    }

    text(title, text_size(px(16)), line_height(px(22)), font_weight(semibold),
         text_color(k->strong));
    text(detail, text_center, text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));
  }
}

static void empty(App *app, const char *title, const char *detail) {
  empty_with(app, CIVUE_ICON(lucide, inbox), title, detail);
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
      avatar(app, author, guild_id, 32, false, k->popover);

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

/* ========================================================================== */
/* bookmarks, as discord's inbox lists them: a card for each, its channel    */
/* over the message, the latest saved first.                                  */
/* ========================================================================== */

// the guild and the channel a thread belongs to, "Guild › parent".
static void place_of(App *app, const Channel *c, char *out, size_t cap) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  const Channel *parent =
    channel_is_thread(c) ? store_channel(store, c->parent_id) : nullptr;
  char name[128] = "";

  if (parent) {
    store_channel_name(store, parent, name, sizeof name);
  }

  if (c->type == CHANNEL_GROUP_DM) {
    snprintf(out, cap, "%zu Members", c->nrecipients + 1);
  }
  else if (guild && *name) {
    snprintf(out, cap, "%s › %s", guild->name ? guild->name : "", name);
  }
  else {
    snprintf(out, cap, "%s", guild && guild->name ? guild->name : "");
  }
}

static void place_icon(App *app, const Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);

  if (guild) {
    div(flex, items_center, justify_center, size(8), flex_shrink_0,
        rounded(lg), overflow_hidden) {
      guild_icon(app, guild, 32, false);
    }
  }
  else if (c->type == CHANNEL_GROUP_DM) {
    char url[CDN_URL_MAX];

    cdn_channel_icon(url, c->id, c->icon, 64);

    div(flex, items_center, justify_center, size(8), flex_shrink_0,
        rounded(full), bg(k->primary), text_color(k->on_primary)) {
      if (!*url || !image_fill(app, url, 16)) {
        icon(lucide, users, size(4));
      }
    }
  }
  else {
    const User *user =
      c->nrecipients ? store_user(store, c->recipients[0]) : nullptr;

    avatar(app, user, 0, 32, false, k->popover);
  }
}

// a bookmark whose message or channel is gone, or that the user may no
// longer see.
static void lost_bookmark(App *app, const Bookmark *b) {
  const Look *k = &app->look;
  civue_id base = id_at(id("inbox-lost"), b->message_id);

  div(key(base), flex, items_center, justify_between, gap(2), flex_shrink_0,
      mb(4), p(3), rounded(lg), bg(k->surface), border(1),
      border_color(k->border)) {
    div(flex, items_center, gap(px(6)), min_w(0)) {
      div(flex, items_center, justify_center, size(6), flex_shrink_0,
          rounded(md), bg(k->frame), text_color(k->strong)) {
        icon(lucide, circle_alert, size(px(14)));
      }

      text("A message you bookmarked could not be found.", min_w(0),
           text_size(px(14)), line_height(px(18)), font_weight(semibold),
           text_color(k->text));
    }

    if (icon_button(app, id_at(base, 1), CIVUE_ICON(lucide, x), 28, 16,
                    "Remove Bookmark")) {
      session_bookmark(app->session, b->channel_id, b->message_id, false);
    }
  }
}

static void bookmark_card(App *app, const Bookmark *b, const Channel *c,
                          const Message *m, Snowflake *chosen) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  civue_id base = id_at(id("inbox-bookmark"), b->message_id);
  char name[128], place[256];

  store_channel_name(store, c, name, sizeof name);
  place_of(app, c, place, sizeof place);

  div(key(base), flex, flex_col, flex_shrink_0, mb(4), rounded(lg),
      bg(k->surface), border(1), border_color(k->border), overflow_hidden) {
    div(flex, items_center, gap(3), pl(3), pr(2), pt(3)) {
      div(key(id_at(base, 1)), flex, items_center, gap(3), flex_1, min_w(0),
          cursor_pointer) {
        if (button().activated) {
          *chosen = c->id;
        }

        place_icon(app, c);

        div(flex, flex_col, flex_1, min_w(0)) {
          div(flex, items_center, gap(1), min_w(0)) {
            if (!channel_is_private(c)) {
              icon(lucide, hash, size(px(14)), flex_shrink_0,
                   text_color(k->faint));
            }

            text(name, min_w(0), truncate, text_size(px(14)),
                 line_height(px(18)), font_weight(semibold),
                 text_color(k->strong));
          }

          if (*place) {
            text(place, min_w(0), truncate, text_size(px(12)),
                 line_height(px(16)), text_color(k->subtle));
          }
        }
      }

      if (icon_button(app, id_at(base, 2),
                      CIVUE_ICON(lucide, message_square_share), 28, 16,
                      "Jump to Message")) {
        *chosen = c->id;
      }

      if (icon_button(app, id_at(base, 3), CIVUE_ICON(lucide, x), 28, 16,
                      "Remove Bookmark")) {
        session_bookmark(app->session, b->channel_id, b->message_id, false);
      }
    }

    div(my(3), overflow_hidden) {
      const Guild *guild = store_guild(store, c->guild_id);
      uint64_t reads[] = {
        store->bookmarks_version,
        c->version,
        guild ? guild->members_version : 0,
        store->users_version,
        civue_images_version(app->images),
        app->revealed.len,
        (uint64_t)day_of((int64_t)store_now(store) * 1000),
      };

      memo(id_at(base, 4), civue_hash(reads, sizeof reads)) {
        message_preview(app, c, m, INBOX_WIDTH - 34);
      }
    }
  }
}

// the empty list: what bookmarks are for, over a conversation where one is
// being made, after discord's.
static void bookmarks_empty(App *app) {
  const Look *k = &app->look;
  static const char *const LINES[] = {
    "Concert tickets go on sale Friday at 10am!",
    "wait, did you see this",
    "the setlist for tonight looks unreal",
  };
  static const char *const HINT[] = {
    "Right-click", "any", "message", "and", "pick",   "Bookmark", "Message.",
    "Find",        "it",  "here,",   "for", "your",   "eyes",     "only.",
  };
  User wumpus = {.id = 313338, .username = "Wumpus", .discriminator = "0000"};

  div(flex, flex_col, items_center, flex_1, justify_center, px(4), py(4)) {
    div(flex, items_center, justify_center, size(px(56)), mb(4), rounded(full),
        bg(k->hover), text_color(k->subtle)) {
      icon(lucide, bookmark, size(6));
    }

    text("Collect your favorite memories", text_center, mb(2),
         text_size(px(20)), line_height(px(24)), font_weight(semibold),
         text_color(k->strong));

    // word by word, so that the lines wrap around the middle.
    div(flex, flex_wrap, justify_center, gap_x(px(4)), w(full), mb(4),
        text_size(px(14)), line_height(px(18)), text_color(k->subtle)) {
      for (size_t i = 0; i < countof(HINT); i++) {
        if (!strcmp(HINT[i], "Bookmark")) {
          text(HINT[i], font_weight(semibold), text_color(k->strong));
        }
        // the name of the row ends in bold, its period does not.
        else if (!strcmp(HINT[i], "Message.")) {
          div(flex) {
            text("Message", font_weight(semibold), text_color(k->strong));
            text(".");
          }
        }
        else {
          text(HINT[i]);
        }
      }
    }

    div(relative, self_stretch, flex, flex_col, py(2), rounded(lg), bg(k->frame),
        border(1), border_color(k->border_strong)) {
      for (size_t i = 0; i < countof(LINES); i++) {
        div(relative, flex, gap(4), px(4), py(px(2))) {
          if (i == 0) {
            avatar(app, &wumpus, 0, 40, false, k->frame);
          }
          else {
            div(w(px(40)), flex_shrink_0) {}
          }

          div(flex, flex_col, min_w(0)) {
            if (i == 0) {
              text("Wumpus", text_size(px(16)), line_height(px(22)),
                   font_weight(medium), text_color(k->strong));
            }

            text(LINES[i], text_size(px(16)), line_height(px(22)),
                 text_color(k->text));
          }

          // the menu the first message has open.
          if (i == 0) {
            div(absolute, right(4), top(px(16)), flex, flex_col, p(1),
                rounded(md), bg(k->popover), border(1), border_color(k->border),
                shadow(lg)) {
              div(flex, items_center, gap(2), px(2), py(px(6)), rounded(sm),
                  bg(k->hover), text_color(k->strong)) {
                icon(lucide, bookmark, size(4));
                text("Bookmark Message", text_size(px(14)),
                     line_height(px(18)), font_weight(medium));
              }
            }
          }
        }
      }
    }
  }
}

static void bookmarks(App *app, Snowflake *chosen) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  Store *store = session->store;
  civue_id list = id("inbox-bookmarks"), inner = id("inbox-bookmarks-inner");
  bool waiting = !session->offline && !store->bookmarks_known;

  session_fetch_bookmarks(session, false);

  // the next page once the end of the list comes near, which it is at
  // once when what was fetched does not fill the card. sizes are the last
  // frame's, so they only count once it drew what is there now.
  size_t *drawn = civue_memory(ui, id_in(list, "drawn"), sizeof *drawn,
                               nullptr);
  float left = (float)last_size(inner).y - civue_scroll_y(ui, list) -
               (float)last_size(list).y;

  if (session->bookmarks_fetched && !session->bookmarks_failed &&
      *drawn == store->nbookmarks && left < BOOKMARKS_AHEAD) {
    session_fetch_bookmarks(session, true);
  }

  *drawn = store->nbookmarks;

  div(key(list), flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
      scrollbars(&app->thin_bars)) {
    div(key(inner), flex, flex_col, min_h(full), px(4), pt(4)) {
      for (size_t i = 0; i < store->nbookmarks; i++) {
        const Bookmark *b = &store->bookmarks[i];
        const Channel *c = store_channel(store, b->channel_id);
        const Message *m = store_bookmark_message(store, b);
        bool seen = c && (channel_is_private(c) ||
                          store_can(store, c, PERM_VIEW_CHANNEL));

        if (m && seen) {
          bookmark_card(app, b, c, m, chosen);
        }
        else {
          lost_bookmark(app, b);
        }
      }

      if (session->bookmarks_loading && store->nbookmarks) {
        text("Loading...", text_center, flex_shrink_0, mb(4),
             text_size(px(14)), line_height(px(18)), text_color(k->subtle));
      }
      else if (session->bookmarks_more && store->nbookmarks) {
        div(key(id("inbox-bookmarks-more")), flex, items_center,
            justify_center, flex_shrink_0, h(8), mb(4), rounded(lg),
            bg(k->hover), hover(bg(k->selected)), text_size(px(14)),
            line_height(px(18)), font_weight(medium), text_color(k->strong),
            cursor_pointer) {
          if (button().activated) {
            session_fetch_bookmarks(session, true);
          }

          text(session->bookmarks_failed ? "Try again" : "Load more messages");
        }
      }

      if (store->nbookmarks) {
        continue;
      }

      if (session->bookmarks_failed) {
        empty_with(app, CIVUE_ICON(lucide, bookmark),
                   "Bookmarks could not be loaded.",
                   "Something went wrong while asking for them.");

        div(key(id("inbox-bookmarks-retry")), self_center, flex, items_center,
            h(8), px(4), mb(8), rounded(lg), bg(k->primary),
            hover(bg(k->primary_hover)), text_size(px(14)), line_height(px(18)),
            font_weight(medium), text_color(k->on_primary), cursor_pointer) {
          if (button().activated) {
            session_fetch_bookmarks(session, true);
          }

          text("Try again");
        }
      }
      else if (waiting || session->bookmarks_loading ||
               session->bookmarks_more) {
        empty_with(app, CIVUE_ICON(lucide, bookmark), "Loading...",
                   "Fetching your bookmarks.");
      }
      else {
        bookmarks_empty(app);
      }
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
        w(px(INBOX_WIDTH)), h(px(height)), rounded(xl), bg(k->popover),
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

        if (tab_button(app, INBOX_BOOKMARKS, "Bookmarks")) {
          app->inbox_tab = INBOX_BOOKMARKS;
        }
      }

      if (app->inbox_tab == INBOX_MENTIONS) {
        mentions(app, &chosen);
      }
      else if (app->inbox_tab == INBOX_BOOKMARKS) {
        bookmarks(app, &chosen);
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
