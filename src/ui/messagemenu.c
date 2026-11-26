#include "components.h"

#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/emoji.h"
#include "discord/fakestickers.h"
#include "widgets.h"

constexpr float MENU_WIDTH = 248;
constexpr float PICKER_WIDTH = 332;
constexpr float PICKER_HEIGHT = 340;
constexpr size_t FORWARD_MAX = 40;

size_t quick_reactions(App *app, const Channel *c, ReactionPick *out,
                       size_t max) {
  Session *session = app->session;
  size_t n = 0;

  session_fetch_frecency(session);

  for (size_t i = 0; i < session->nfrecent_reactions && n < max; i++) {
    n += emoji_pick_by_key(app, c, session->frecent_reactions[i], &out[n]);
  }

  return n;
}

static bool same_pick(const Reaction *r, ReactionPick pick) {
  if (pick.id || r->emoji_id) {
    return pick.id == r->emoji_id;
  }

  return r->name && emoji_by_utf8(r->name) == emoji_by_utf8(pick.name) &&
         emoji_by_utf8(pick.name);
}

void react_with(App *app, const Message *m, ReactionPick pick) {
  bool mine = false;

  for (size_t i = 0; i < m->nreactions; i++) {
    mine |= m->reactions[i].me && same_pick(&m->reactions[i], pick);
  }

  session_react(app->session, m->channel_id, m->id, pick.id, pick.name, !mine);
}

void message_popover(App *app, Popover popover, const Message *m,
                     civue_rect anchor) {
  if (app->popover == popover && app->menu_message == m->id) {
    app->popover = POPOVER_NONE;
    return;
  }

  app->popover = popover;
  app->menu_message = m->id;
  app->menu_anchor = anchor;
  app->menu_armed = false;
  app->menu_focus = true;
  free(app->menu_search);
  app->menu_search = nullptr;
}

static void separator(const Look *k) {
  div(flex_shrink_0, h(px(1)), my(1), mx(2), bg(k->border)) {}
}

// where a card goes by its anchor: its right side on the anchor's, under it
// or over it when there is no room.
static civue_rect card_at(civue_rect anchor, float width, float height) {
  civue_rect room = overlay_room();
  float left = fmaxf((float)anchor.x1 - width, (float)room.x0);
  float top = (float)anchor.y1 + 4;

  if (top + height > room.y1) {
    top = (float)anchor.y0 - 4 - height;
  }

  top = fmaxf(fminf(top, (float)room.y1 - height), (float)room.y0);
  return xywh(left, top, width, height);
}

static void dismiss(App *app, civue_rect card) {
  civue_ui *ui = civue_ui_current();
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(app->menu_anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}

static bool search_field(App *app, civue_id id, const char *placeholder) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool submitted = false;

  div(key(id), flex_shrink_0, h(9), mx(3), mt(3), mb(2), text_size(px(14)),
      line_height(px(18))) {
    civue_field_look look = field_look(k);

    look.pad = 10;

    if (app->menu_focus && !ui->rerun) {
      ui->focus = id;
      app->menu_focus = false;
    }

    submitted = text_field(&app->menu_search, placeholder, look).submitted;
  }

  return submitted;
}

static void menu(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  bool own = m->author_id == store->self_id;
  bool plain = !message_is_system(m);
  bool reacts = store_can(store, c, PERM_ADD_REACTIONS);
  bool writes = store_can(store, c, PERM_SEND_MESSAGES);
  bool removable =
    own || (c->guild_id && store_can(store, c, PERM_MANAGE_MESSAGES));
  bool pins = !c->guild_id || store_can(store, c, PERM_PIN_MESSAGES) ||
              store_can(store, c, PERM_MANAGE_MESSAGES);
  bool has_text = *m->content != '\0';
  bool bookmarked = store_bookmarked(store, m->id);
  // discord's client offers no bookmark of what the user cannot read again.
  bool bookmarks = bookmarked || channel_is_private(c) ||
                   store_can(store, c, PERM_READ_MESSAGE_HISTORY);
  // a sticker of one of the user's guilds may be starred from its message.
  // one read from a link or a gif counts, as Vencord's FakeNitro shows it.
  FakeSticker linked;
  Snowflake first = m->nstickers                          ? m->stickers[0].id
                    : fake_stickers(store, m, &linked, 1) ? linked.id
                                                          : 0;
  Snowflake sticker = store_sticker(store, first, nullptr) ? first : 0;
  bool starred = session_favorite_sticker(app->session, sticker);
  ReactionPick picks[4];
  size_t npicks = reacts ? quick_reactions(app, c, picks, countof(picks)) : 0;

  size_t rows = reacts + (own && plain) + (writes && plain) + plain + has_text +
                (sticker != 0) + pins + bookmarks + 3 + removable;
  float height = 2 + 8 + (float)rows * 40 + 3 * 9 + (npicks ? 52 : 0);
  civue_rect card = card_at(app->menu_anchor, MENU_WIDTH, height);
  civue_id base = id("message-menu");
  int picked = -1;

  enum {
    NOTHING,
    REACT,
    EDIT,
    REPLY,
    FORWARD,
    COPY_TEXT,
    STICKER,
    PIN,
    BOOKMARK,
    UNREAD,
    COPY_LINK,
    COPY_ID,
    DELETE,
  } chosen = NOTHING;

#define ROW(what, glyph, label, color)                         \
  if (menu_row(app, id_at(base, what), glyph, label, color)) { \
    chosen = what;                                             \
  }

  overlay() {
    div(key(base), flex, flex_col, left(px(card.x0)), top(px(card.y0)),
        w(px(MENU_WIDTH)), p(1), rounded(xl), bg(k->popover), border(1),
        border_color(k->border_strong), shadow(lg)) {
      card = here();

      if (npicks) {
        div(flex, gap(1), flex_shrink_0, p(1), pb(2)) {
          for (size_t i = 0; i < npicks; i++) {
            div(key(id_at(base, 0x100 + i)), flex, items_center, justify_center,
                flex_1, h(10), rounded(lg), bg(k->hover),
                hover(bg(k->selected)), cursor_pointer) {
              if (button().activated) {
                picked = (int)i;
              }

              emoji_node(app, picks[i].id, picks[i].name, 22);
            }
          }
        }
      }

      if (reacts) {
        ROW(REACT, CIVUE_ICON(mingcute, emoji_2_fill), "Add Reaction", k->text);
        separator(k);
      }

      if (own && plain) {
        ROW(EDIT, CIVUE_ICON(mingcute, pencil_fill), "Edit Message", k->text);
      }

      if (writes && plain) {
        ROW(REPLY, CIVUE_ICON(mingcute, corner_up_left_fill), "Reply", k->text);
      }

      if (plain) {
        ROW(FORWARD, CIVUE_ICON(mingcute, corner_up_right_fill), "Forward", k->text);
      }

      separator(k);

      if (has_text) {
        ROW(COPY_TEXT, CIVUE_ICON(mingcute, copy_2_fill), "Copy Text", k->text);
      }

      if (sticker) {
        ROW(STICKER, CIVUE_ICON(mingcute, star_fill),
            starred ? "Unfavorite Sticker" : "Favorite Sticker", k->text);
      }

      if (pins) {
        if (m->pinned) {
          ROW(PIN, CIVUE_ICON(mingcute, pin_fill), "Unpin Message", k->text);
        }
        else {
          ROW(PIN, CIVUE_ICON(mingcute, pin_2_fill), "Pin Message", k->text);
        }
      }

      if (bookmarks && bookmarked) {
        ROW(BOOKMARK, CIVUE_ICON(mingcute, bookmark_remove_fill), "Remove Bookmark",
            k->text);
      }
      else if (bookmarks) {
        ROW(BOOKMARK, CIVUE_ICON(mingcute, bookmark_fill), "Bookmark Message",
            k->text);
      }

      ROW(UNREAD, CIVUE_ICON(mingcute, message_square_dot_fill), "Mark Unread",
          k->text);
      ROW(COPY_LINK, CIVUE_ICON(mingcute, link_fill), "Copy Message Link", k->text);
      separator(k);
      ROW(COPY_ID, CIVUE_ICON(mingcute, fingerprint_fill), "Copy Message ID", k->text);

      if (removable) {
        ROW(DELETE, CIVUE_ICON(mingcute, delete_2_fill),
            app->menu_armed ? "Press again to delete" : "Delete Message",
            k->critical_text);
      }
    }
  }

#undef ROW

  dismiss(app, card);

  if (picked >= 0) {
    react_with(app, m, picks[picked]);
    app->popover = POPOVER_NONE;
    return;
  }

  char scratch[128];
  civue_rect anchor = app->menu_anchor;

  switch (chosen) {
    case NOTHING:
      return;

    case REACT:
      message_popover(app, POPOVER_REACT, m, anchor);
      return;

    case FORWARD:
      message_popover(app, POPOVER_FORWARD, m, anchor);
      return;

    case EDIT:
      app_edit_message(app, m);
      break;

    case REPLY:
      app->reply_to = m->id;
      app->reply_ping = true;
      app->focus_composer = true;
      break;

    case COPY_TEXT:
      civue_clipboard_set_text(m->content, strlen(m->content));
      break;

    case STICKER:
      session_set_favorite_sticker(app->session, sticker, !starred);
      break;

    case PIN:
      session_pin(app->session, c->id, m->id, !m->pinned);
      break;

    case BOOKMARK:
      session_bookmark(app->session, c->id, m->id, !bookmarked);
      break;

    case UNREAD:
      session_mark_unread(app->session, c->id, m->id);
      break;

    case COPY_LINK:
      if (c->guild_id) {
        snprintf(scratch, sizeof scratch,
                 "https://discord.com/channels/%llu/%llu/%llu",
                 (unsigned long long)c->guild_id, (unsigned long long)c->id,
                 (unsigned long long)m->id);
      }
      else {
        snprintf(scratch, sizeof scratch,
                 "https://discord.com/channels/@me/%llu/%llu",
                 (unsigned long long)c->id, (unsigned long long)m->id);
      }

      civue_clipboard_set_text(scratch, strlen(scratch));
      break;

    case COPY_ID:
      snprintf(scratch, sizeof scratch, "%llu", (unsigned long long)m->id);
      civue_clipboard_set_text(scratch, strlen(scratch));
      break;

    case DELETE:
      // a first press arms the row, a second one deletes.
      if (!app->menu_armed) {
        app->menu_armed = true;
        return;
      }

      session_delete(app->session, c->id, m->id);
      break;
  }

  app->popover = POPOVER_NONE;
}

static bool contains(const char *text, const char *query) {
  return text && strcasestr(text, query) != nullptr;
}

static void react_picker(App *app, const Channel *c, const Message *m) {
  civue_rect room = overlay_room();
  civue_rect card =
    card_at(app->menu_anchor,
            fminf(EMOJI_CARD_WIDTH, (float)(room.x1 - room.x0)),
            fminf(EMOJI_CARD_HEIGHT, (float)(room.y1 - room.y0)));
  ReactionPick pick = {};
  bool keep = false;
  // set as the picker opens, and taken by the first of its frames.
  bool fresh = app->menu_focus;

  app->menu_focus = false;

  if (emoji_card(app, c, EMOJI_FOR_REACTION, fresh, card, &pick, &keep)) {
    react_with(app, m, pick);

    // a press with shift leaves the picker for another reaction.
    if (!keep) {
      app->popover = POPOVER_NONE;
    }

    return;
  }

  dismiss(app, card);
}

static bool forward_row(App *app, const Channel *to, const char *query,
                        size_t *shown) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char name[128];
  const Guild *guild = store_guild(store, to->guild_id);
  bool pressed = false;

  store_channel_name(store, to, name, sizeof name);

  if (*shown >= FORWARD_MAX || (*query && !contains(name, query) &&
                                !(guild && contains(guild->name, query)))) {
    return false;
  }

  (*shown)++;

  div(key(id_at(id("forward-row"), to->id)), flex, items_center, gap(2),
      flex_shrink_0, h(9), px(2), rounded(lg), hover(bg(k->hover)),
      text_size(px(14)), line_height(px(18)), cursor_pointer) {
    pressed = button().activated;

    if (guild) {
      icon(mingcute, hashtag_fill, size(4), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(mingcute, at_fill, size(4), flex_shrink_0, text_color(k->faint));
    }

    text(name, min_w(0), truncate, font_weight(medium), text_color(k->text));

    if (guild && guild->name) {
      text(guild->name, flex_1, min_w(0), truncate, text_right,
           text_size(px(12)), text_color(k->faint));
    }
  }

  return pressed;
}

static bool takes_forwards(Store *store, const Channel *c) {
  return (c->type == CHANNEL_GUILD_TEXT ||
          c->type == CHANNEL_GUILD_ANNOUNCEMENT) &&
         store_can(store, c, PERM_VIEW_CHANNEL) &&
         store_can(store, c, PERM_SEND_MESSAGES);
}

static void forward_picker(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  civue_rect card = card_at(app->menu_anchor, PICKER_WIDTH, PICKER_HEIGHT);
  const char *query = app->menu_search ? app->menu_search : "";
  size_t count = 0, nfolders = 0, shown = 0;
  Channel *const *privates = store_private_channels(store, &count);
  const GuildFolder *folders = store_guild_folders(store, &nfolders);
  const Channel *to = nullptr;

  overlay() {
    div(key(id("forward-picker")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(PICKER_WIDTH)), h(px(PICKER_HEIGHT)),
        rounded(xl), bg(k->popover), border(1),
        border_color(k->border_strong), shadow(lg), overflow_hidden) {
      card = here();

      text("Forward To", flex_shrink_0, px(4), pt(3), text_size(px(16)),
           line_height(px(22)), font_weight(semibold), text_color(k->strong));
      search_field(app, id("forward-search"), "Search channels and people");

      div(key(id("forward-list")), flex, flex_col, gap(px(2)), flex_1, min_h(0),
          px(2), pb(2), overflow_y_scroll, scrollbars(&app->thin_bars)) {
        for (size_t i = 0; i < count; i++) {
          if (forward_row(app, privates[i], query, &shown)) {
            to = privates[i];
          }
        }

        // without a search only the open guild's channels follow.
        for (size_t i = 0; i < nfolders; i++) {
          for (size_t j = 0; j < folders[i].nguilds; j++) {
            const Guild *guild = folders[i].guilds[j];

            if (!*query && guild->id != c->guild_id) {
              continue;
            }

            for (size_t n = 0; n < guild->nchannels; n++) {
              const Channel *channel = guild->channels[n];

              if (takes_forwards(store, channel) &&
                  forward_row(app, channel, query, &shown)) {
                to = channel;
              }
            }
          }
        }

        if (!shown) {
          text("Nothing matches.", px(2), py(2), text_size(px(14)),
               line_height(px(18)), text_color(k->faint));
        }
      }
    }
  }

  dismiss(app, card);

  if (to) {
    session_forward(app->session, to->id, c->id, m->id);
    app->popover = POPOVER_NONE;
  }
}

bool message_popover_open(const App *app, const Message *m) {
  return m->id && app->menu_message == m->id &&
         (app->popover == POPOVER_MESSAGE_MENU ||
          app->popover == POPOVER_REACT || app->popover == POPOVER_FORWARD);
}

void message_popovers(App *app, Channel *c) {
  if (app->popover != POPOVER_MESSAGE_MENU && app->popover != POPOVER_REACT &&
      app->popover != POPOVER_FORWARD) {
    return;
  }

  const Message *m = store_message(c, app->menu_message);

  // the message went away under its menu.
  if (!m) {
    app->popover = POPOVER_NONE;
    return;
  }

  switch (app->popover) {
    case POPOVER_MESSAGE_MENU:
      menu(app, c, m);
      break;
    case POPOVER_REACT:
      react_picker(app, c, m);
      break;
    default:
      forward_picker(app, c, m);
      break;
  }
}
