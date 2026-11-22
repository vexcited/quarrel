#include "components.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/emoji.h"
#include "widgets.h"

constexpr float MENU_WIDTH = 248;
constexpr float PICKER_WIDTH = 332;
constexpr float PICKER_HEIGHT = 340;
constexpr float CELL = 38;
constexpr size_t PICKER_MAX = 64;
constexpr size_t FORWARD_MAX = 40;

static bool all_digits(const char *s) {
  for (const char *c = s; *c; c++) {
    if (!isdigit((unsigned char)*c)) {
      return false;
    }
  }

  return *s != '\0';
}

// what a key of the user's settings is here: an emoji of unicode by its
// first name, or one of the channel's guild by its id. those of another
// guild need nitro, which Quarrel does not know of.
static bool pick_of(App *app, const Channel *c, const char *key,
                    ReactionPick *out) {
  if (!all_digits(key)) {
    const UnicodeEmoji *emoji = emoji_by_name(key);

    if (!emoji) {
      return false;
    }

    *out = (ReactionPick){.name = emoji->utf8};
    return true;
  }

  const Guild *guild = store_guild(app->session->store, c->guild_id);
  const Emoji *emoji =
    guild ? store_emoji(guild, strtoull(key, nullptr, 10)) : nullptr;

  if (!emoji || !emoji->available || emoji->animated || !emoji->name) {
    return false;
  }

  *out = (ReactionPick){.id = emoji->id, .name = emoji->name};
  return true;
}

size_t quick_reactions(App *app, const Channel *c, ReactionPick *out,
                       size_t max) {
  Session *session = app->session;
  size_t n = 0;

  session_fetch_frecency(session);

  for (size_t i = 0; i < session->nfrecent_reactions && n < max; i++) {
    n += pick_of(app, c, session->frecent_reactions[i], &out[n]);
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
  ReactionPick picks[4];
  size_t npicks = reacts ? quick_reactions(app, c, picks, 4) : 0;

  size_t rows = reacts + (own && plain) + (writes && plain) + plain + has_text +
                pins + bookmarks + 3 + removable;
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
        w(px(MENU_WIDTH)), p(1), rounded(xl), bg(k->surface_high), border(1),
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
        ROW(REACT, CIVUE_ICON(lucide, smile_plus), "Add Reaction", k->text);
        separator(k);
      }

      if (own && plain) {
        ROW(EDIT, CIVUE_ICON(lucide, pencil), "Edit Message", k->text);
      }

      if (writes && plain) {
        ROW(REPLY, CIVUE_ICON(lucide, reply), "Reply", k->text);
      }

      if (plain) {
        ROW(FORWARD, CIVUE_ICON(lucide, forward), "Forward", k->text);
      }

      separator(k);

      if (has_text) {
        ROW(COPY_TEXT, CIVUE_ICON(lucide, copy), "Copy Text", k->text);
      }

      if (pins) {
        if (m->pinned) {
          ROW(PIN, CIVUE_ICON(lucide, pin_off), "Unpin Message", k->text);
        }
        else {
          ROW(PIN, CIVUE_ICON(lucide, pin), "Pin Message", k->text);
        }
      }

      if (bookmarks && bookmarked) {
        ROW(BOOKMARK, CIVUE_ICON(lucide, bookmark_minus), "Remove Bookmark",
            k->text);
      }
      else if (bookmarks) {
        ROW(BOOKMARK, CIVUE_ICON(lucide, bookmark), "Bookmark Message",
            k->text);
      }

      ROW(UNREAD, CIVUE_ICON(lucide, message_square_dot), "Mark Unread",
          k->text);
      ROW(COPY_LINK, CIVUE_ICON(lucide, link), "Copy Message Link", k->text);
      separator(k);
      ROW(COPY_ID, CIVUE_ICON(lucide, fingerprint), "Copy Message ID", k->text);

      if (removable) {
        ROW(DELETE, CIVUE_ICON(lucide, trash_2),
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

// the emojis a search finds: the guild's first, then those whose name starts
// with it, then those that hold it.
static size_t search_emojis(App *app, const Channel *c, const char *query,
                            ReactionPick *out, size_t max) {
  const Guild *guild = store_guild(app->session->store, c->guild_id);
  size_t n = 0, len = strlen(query);

  for (size_t i = 0; guild && i < guild->nemojis && n < max; i++) {
    const Emoji *emoji = &guild->emojis[i];

    if (emoji->available && !emoji->animated && contains(emoji->name, query)) {
      out[n++] = (ReactionPick){.id = emoji->id, .name = emoji->name};
    }
  }

  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < UNICODE_EMOJI_NAMES_COUNT && n < max; i++) {
      const UnicodeEmojiName *name = &UNICODE_EMOJI_NAMES[i];
      const UnicodeEmoji *emoji = &UNICODE_EMOJIS[name->index];
      bool starts = !strncasecmp(name->name, query, len);
      bool seen = false;

      if (emoji->toned || starts != (pass == 0) ||
          (!starts && !contains(name->name, query))) {
        continue;
      }

      // an emoji found by two of its names shows once.
      for (size_t j = 0; j < n && !seen; j++) {
        seen = !out[j].id && out[j].name == emoji->utf8;
      }

      if (!seen) {
        out[n++] = (ReactionPick){.name = emoji->utf8};
      }
    }
  }

  return n;
}

static void react_picker(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  civue_rect card = card_at(app->menu_anchor, PICKER_WIDTH, PICKER_HEIGHT);
  ReactionPick picks[PICKER_MAX];
  const char *query = app->menu_search ? app->menu_search : "";
  size_t npicks = *query ? search_emojis(app, c, query, picks, PICKER_MAX)
                         : quick_reactions(app, c, picks, PICKER_MAX);
  int picked = -1;
  bool submitted = false;

  overlay() {
    div(key(id("react-picker")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(PICKER_WIDTH)), h(px(PICKER_HEIGHT)),
        rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), overflow_hidden) {
      card = here();
      submitted = search_field(app, id("react-search"), "Find an emoji");

      text(*query ? "Results" : "Frequently Used", flex_shrink_0, px(4), pb(1),
           text_size(px(12)), line_height(px(16)), font_weight(semibold),
           text_color(k->subtle));

      div(key(id("react-grid")), flex, flex_wrap, content_start, flex_1,
          min_h(0), px(3), pb(3), overflow_y_scroll,
          scrollbars(&app->thin_bars)) {
        for (size_t i = 0; i < npicks; i++) {
          div(key(id_at(id("react-cell"), i)), flex, items_center,
              justify_center, size(px(CELL)), flex_shrink_0, rounded(lg),
              hover(bg(k->hover)), cursor_pointer) {
            if (button().activated) {
              picked = (int)i;
            }

            emoji_node(app, picks[i].id, picks[i].name, 26);
          }
        }

        if (!npicks) {
          text(*query ? "No emoji matches." : "Type to find an emoji.", px(1),
               py(2), text_size(px(14)), line_height(px(18)),
               text_color(k->faint));
        }
      }
    }
  }

  dismiss(app, card);

  // enter takes the first one found.
  if (submitted && npicks) {
    picked = 0;
  }

  if (picked >= 0) {
    react_with(app, m, picks[picked]);
    app->popover = POPOVER_NONE;
  }
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
      icon(lucide, hash, size(4), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(lucide, at_sign, size(4), flex_shrink_0, text_color(k->faint));
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
        rounded(xl), bg(k->surface_high), border(1),
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
