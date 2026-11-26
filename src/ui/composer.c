#include "components.h"

#include <math.h>
#include <stdcountof.h>
#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/expressions.h"
#include "discord/uploads.h"
#include "forms.h"
#include "media.h"
#include "server.h"
#include "widgets.h"

constexpr float LINE = 22;
constexpr float MAX_LINES = 12;
constexpr float ROW = 56;
constexpr double TYPING_BEAT = 1.2;

static civue_id composer_id() {
  return id("composer");
}

// replaces what the composer holds, through the field while it edits: it
// keeps a text of its own then.
static void set_draft(App *app, const char *value) {
  civue_ui *ui = civue_ui_current();
  uint32_t length = app->draft ? (uint32_t)strlen(app->draft) : 0;
  char *held = civue_field_replace(ui, composer_id(), 0, length, value);

  free(app->draft);
  app->draft = held ? held : strdup(value);
}

void composer_append(App *app, const char *text) {
  const char *draft = app->draft ? app->draft : "";
  size_t had = strlen(draft);
  bool spaced = had == 0 || draft[had - 1] == ' ' || draft[had - 1] == '\n';
  char *joined = malloc(had + strlen(text) + 3);

  sprintf(joined, "%s%s%s ", draft, spaced ? "" : " ", text);
  set_draft(app, joined);
  free(joined);
  app->focus_composer = true;
}

void composer_insert(App *app, const char *text) {
  civue_ui *ui = civue_ui_current();
  char *held = civue_field_insert(ui, composer_id(), text);

  // the field keeps no caret once it lost the focus: the end then.
  if (!held) {
    const char *draft = app->draft ? app->draft : "";

    held = malloc(strlen(draft) + strlen(text) + 1);
    sprintf(held, "%s%s", draft, text);
  }

  free(app->draft);
  app->draft = held;
}

void upload_clear_draft(App *app) {
  set_draft(app, "");
}

static void stop_editing(App *app) {
  char *kept = app->kept_draft;

  app->editing = 0;
  app->kept_draft = nullptr;
  set_draft(app, kept ? kept : "");
  free(kept);
}

static bool blank(const char *s) {
  for (; s && *s; s++) {
    if (*s != ' ' && *s != '\n' && *s != '\t') {
      return false;
    }
  }

  return true;
}

// `sure` once FakeNitro's notice was answered.
static void submit(App *app, Channel *c, bool sure) {
  Session *session = app->session;
  bool faked = false;

  // the emojis named between colons as a message holds them. the ones
  // discord would refuse go as links to their pictures, as Vencord's
  // FakeNitro sends them.
  char *written =
    expressions_send(session->store, c, app->draft ? app->draft : "", &faked);

  defer free(written);

  // a sticker discord would refuse goes as a link too.
  Snowflake sticker = app->editing ? 0 : sticker_pending(app, c);
  char *linked =
    sticker_linked(app, c, sticker, blank(written) ? "" : written);

  defer free(linked);
  faked = faked || linked;

  // a link shows no picture where the user may not embed any.
  if (faked && !sure && c->guild_id &&
      !store_can(session->store, c, PERM_EMBED_LINKS)) {
    app->fake_ask = true;
    return;
  }

  if (app->editing) {
    if (!blank(written)) {
      session_edit(session, c->id, app->editing, written);
    }

    stop_editing(app);
    return;
  }

  Upload *files[1];
  bool attached = uploads_of(session, c->id, 0, files, 1) != 0;
  const char *said = linked ? linked : blank(written) ? "" : written;

  if (!*said && !attached && !sticker) {
    return;
  }

  if (upload_message_too_long(app, said)) {
    app->long_message = true;
    return;
  }

  session_send_stickers(session, c->id, said, &sticker,
                        sticker && !linked ? 1 : 0, app->reply_to,
                        app->reply_ping);

  if (linked) {
    session_use_sticker(session, sticker);
  }

  app->sticker_pending = 0;
  app->reply_to = 0;
  app->stick = true;
  set_draft(app, "");
}

static void edit_last(App *app, Channel *c) {
  Snowflake self = app->session->store->self_id;

  for (size_t i = c->nmessages; i-- > 0;) {
    const Message *m = c->messages[i];

    if (m->author_id == self && m->state == MESSAGE_SENT &&
        !message_is_system(m)) {
      app->kept_draft = app->draft;
      app->draft = nullptr;
      app->editing = m->id;
      app->reply_to = 0;
      set_draft(app, m->content);
      app->focus_composer = true;
      return;
    }
  }
}

static void drop_key(civue_ui *ui, uint32_t at) {
  for (uint32_t i = at + 1; i < ui->input.keys.len; i++) {
    ui->input.keys.v[i - 1] = ui->input.keys.v[i];
  }

  ui->input.keys.len--;
}

static bool attachable(App *app, Channel *c) {
  return !c->guild_id || store_can(app->session->store, c, PERM_ATTACH_FILES);
}

// the keys the composer takes before its field sees them: enter sends where
// the field would start a line, which shift and enter still do.
static void take_keys(App *app, Channel *c) {
  civue_ui *ui = civue_ui_current();

  for (uint32_t i = 0; i < ui->input.keys.len; i++) {
    civue_key_press press = ui->input.keys.v[i];

    if (press.key == CIVUE_KEY_ENTER && !(press.mods & CIVUE_MOD_SHIFT)) {
      drop_key(ui, i--);
      submit(app, c, false);
    }
    else if (press.key == CIVUE_KEY_ESCAPE && app->editing) {
      drop_key(ui, i--);
      stop_editing(app);
    }
    else if (press.key == CIVUE_KEY_ESCAPE && app->reply_to) {
      drop_key(ui, i--);
      app->reply_to = 0;
    }
    else if (press.key == CIVUE_KEY_ESCAPE && sticker_pending(app, c)) {
      drop_key(ui, i--);
      app->sticker_pending = 0;
    }
    else if (press.key == CIVUE_KEY_UP && blank(app->draft) &&
             !app->editing && !uploads_of(app->session, c->id, 0, nullptr, 0)) {
      drop_key(ui, i--);
      edit_last(app, c);
    }
    // files, a picture or a long text go to the tray rather than the field.
    else if (press.key == 'v' && (press.mods & CIVUE_MOD_CMD) &&
             !app->editing && attachable(app, c) && upload_paste(app, c)) {
      drop_key(ui, i--);
    }
  }
}

// typing with nothing focused goes to the composer, as in discord.
static void catch_typing(App *app) {
  civue_ui *ui = civue_ui_current();
  civue_bytes *typed = &ui->input.text;

  if (ui->focus || !typed->len || (ui->input.mods & CIVUE_MOD_CMD) ||
      media_viewing(app) || server_covers(app)) {
    return;
  }

  size_t had = app->draft ? strlen(app->draft) : 0;

  app->draft = realloc(app->draft, had + typed->len + 1);
  memcpy(app->draft + had, typed->v, typed->len);
  app->draft[had + typed->len] = 0;
  typed->len = 0;
  app->focus_composer = true;
}

civue_button_state composer_button(App *app, civue_id id, civue_icon icon,
                                   bool open, const char *label,
                                   civue_rect *at) {
  const Look *k = &app->look;
  civue_button_state state = {};

  div(key(id), flex, items_center, justify_center, flex_shrink_0, size(8),
      rounded(lg), style(.bg = open ? k->selected : CIVUE_CLEAR),
      text_color(open ? k->strong : k->subtle),
      hover(bg(k->hover), text_color(k->strong)), active(bg(k->pressed)),
      cursor_pointer) {
    state = button();

    if (at) {
      *at = here();
    }

    icon_of(icon, size(5));

    if (label && !open) {
      hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
    }
  }

  return state;
}

// what the gutter of a row holds, under the avatars of the messages.
static void gutter_icon(civue_icon icon, civue_color color) {
  div(flex, items_center, justify_center, flex_shrink_0,
      w(px(MESSAGE_GUTTER))) {
    icon_of(icon, size(4), text_color(color));
  }
}

static void context_bar(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Message *to = app->reply_to ? store_message(c, app->reply_to) : nullptr;

  if (app->reply_to && !to) {
    app->reply_to = 0;
  }

  if (!app->editing && !to) {
    return;
  }

  div(flex, items_center, flex_shrink_0, h(9), pr(5), border_b(1),
      border_color(k->border), text_size(px(14)), line_height(px(18)),
      text_color(k->subtle)) {
    if (app->editing) {
      gutter_icon(CIVUE_ICON(lucide, pencil), k->faint);
      text("Editing message", flex_1, min_w(0), truncate, font_weight(medium));
      text("escape to cancel, enter to save", flex_shrink_0, mx(2),
           text_color(k->faint));
    }
    else {
      Author author = store_author(store, to);

      gutter_icon(CIVUE_ICON(lucide, reply), k->faint);
      text("Replying to", flex_shrink_0, mr(1));
      text(author.name, flex_1, min_w(0), truncate, font_weight(semibold),
           text_color(author.color ? rgb(author.color) : k->strong));

      div(key(id("reply-ping")), flex, items_center, gap(1), flex_shrink_0,
          h(6), mx(1), px(2), rounded(md), font_weight(bold),
          hover(bg(k->hover)), cursor_pointer,
          text_color(app->reply_ping ? k->link : k->faint)) {
        if (button().activated) {
          app->reply_ping = !app->reply_ping;
        }

        icon(lucide, at_sign, size(4));
        text(app->reply_ping ? "ON" : "OFF");
      }
    }

    if (icon_button(app, id("context-close"), CIVUE_ICON(lucide, x), 24, 16,
                    nullptr)) {
      if (app->editing) {
        stop_editing(app);
      }
      else {
        app->reply_to = 0;
      }
    }
  }
}

// who typed last in the channel, kept to show them out once they stopped.
typedef struct {
  Snowflake channel;
  Snowflake users[3];
  size_t count;
} Typers;

static void typer(const Look *k, const Store *store, const Channel *c,
                  Snowflake user, bool comma) {
  char name[160];

  snprintf(name, sizeof name, "%s%s",
           store_display_name(store, c->guild_id, user), comma ? "," : "");
  text(name, min_w(0), max_w(px(200)), truncate, font_weight(semibold),
       text_color(k->text));
}

// who types, on a tab over the composer's edge. it takes the room the list
// leaves under its last message, so nothing moves when it comes and goes.
static void typing_tab(App *app, const Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const Store *store = app->session->store;
  civue_id id = id("typing-tab");
  Typers *last = civue_memory(ui, id, sizeof *last, nullptr);
  Snowflake users[countof(last->users)] = {};
  size_t count = store_typing(store, c, users, countof(users));

  if (last->channel != c->id) {
    *last = (Typers){.channel = c->id};
  }

  if (count) {
    memcpy(last->users, users, sizeof users);
    last->count = count;
  }

  float shown =
    civue_animate_from(ui, id_in(id, "shown"), 0, count ? 1 : 0, .16f);

  if (shown <= 0 || !last->count) {
    return;
  }

  // one point lower than the edge, to hide the border it sits on.
  div(absolute, left(4), right(4), top(px(-MESSAGE_FOOT)),
      h(px(MESSAGE_FOOT + 1)), flex, items_start, overflow_hidden) {
    div(flex, items_center, gap(1), flex_shrink_0, max_w(full),
        h(px(MESSAGE_FOOT + 1)), mt(px((1 - shown) * (MESSAGE_FOOT + 1))),
        pl(px(10)), pr(3), rounded_t(lg), bg(k->input), border_t(1),
        border_x(1), border_color(k->border), text_size(px(13)),
        line_height(px(16)), text_color(k->subtle)) {
      div(flex, items_center, gap(px(3)), flex_shrink_0, mr(1)) {
        for (int i = 0; i < 3; i++) {
          double wave = sin(ui->now * 2 * M_PI / TYPING_BEAT - i * .9);

          div(size(px(5)), rounded(full),
              bg(civue_alpha(k->text, .3f + .35f * (float)(1 + wave)))) {}
        }
      }

      if (last->count > countof(last->users)) {
        text("Several people are typing", truncate);
      }
      else {
        for (size_t i = 0; i < last->count; i++) {
          if (i > 0 && i + 1 == last->count) {
            text("and", flex_shrink_0);
          }

          typer(k, store, c, last->users[i], i + 2 < last->count);
        }

        text(last->count > 1 ? "are typing" : "is typing", flex_shrink_0);
      }
    }
  }

  // the dots only breathe in a window that is looked at.
  if (count && ui->window_focused) {
    civue_request_repaint_after(ui, 1.0 / 30);
  }
}

// a channel that takes no message says why where it would be written.
static void closed_row(const Look *k, civue_icon icon, const char *why) {
  div(flex, items_center, flex_shrink_0, min_h(px(ROW)), pr(4),
      text_color(k->faint)) {
    gutter_icon(icon, k->faint);
    text(why, flex_1, min_w(0));
  }
}

static void gif_button(App *app, civue_rect box) {
  if (composer_button(app, id("composer-gif"), CIVUE_ICON(lucide, image_play),
                      app->popover == POPOVER_GIFS, "Send a GIF",
                      &app->gif_button)
        .activated) {
    gif_picker_toggle(app, box);
  }
}

static void emoji_button(App *app, civue_rect box) {
  if (composer_button(app, id("composer-emoji"), CIVUE_ICON(lucide, smile),
                      app->popover == POPOVER_EMOJIS, "Select emoji",
                      &app->emoji_button)
        .activated) {
    emoji_picker_toggle(app, box);
  }
}

static void writing(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = composer_id();
  civue_text_style style = civue_style(16, 400, LINE, 0);
  char name[128], placeholder[160];

  store_channel_name(app->session->store, c, name, sizeof name);
  snprintf(placeholder, sizeof placeholder,
           channel_is_private(c) ? "Message %s%s" : "Message #%s%s",
           c->type == CHANNEL_DM ? "@" : "", name);

  catch_typing(app);

  if (app->focus_composer) {
    ui->focus = id;
  }

  if (ui->focus == id) {
    take_keys(app, c);
  }

  // as tall as its lines, up to a dozen of them.
  float width = (float)last_size(id).x;
  const char *draft = app->draft ? app->draft : "";
  const civue_field_inlays *inlays = composer_inlays(app, c);
  float lines = LINE;

  if (width > 0 && *draft) {
    lines = civue_text_area_height(ui, draft, style, width, inlays);
  }

  lines = fminf(fmaxf(lines, LINE), LINE * MAX_LINES);

  // what opens over the composer keeps clear of the edges of the chat.
  civue_rect box = here();

  box.x0 += 8;
  box.x1 -= 8;

  context_bar(app, c);

  if (!app->editing) {
    sticker_tray(app, c);
    upload_tray(app, c);
  }

  // the plus under the avatars of the messages, the words under theirs.
  div(flex, items_center, min_h(px(ROW)), pr(4), py(3)) {
    div(flex, items_center, justify_center, flex_shrink_0, self_start,
        w(px(MESSAGE_GUTTER)), h(8)) {
      // an edit takes no file.
      if (!app->editing && attachable(app, c)) {
        upload_button(app, c);
      }
      else if (!app->editing) {
        icon(lucide, plus, size(5), text_color(k->faint));
      }
    }

    div(key(id), flex_1, min_w(0), h(px(lines + 8))) {
      civue_field_look look = {
        .editing = {.text = k->text},
        .idle = {.text = k->text},
        .hovered = {.text = k->text},
        .placeholder = k->faint,
        .selection = civue_alpha(k->brand, .45f),
      };
      civue_field_response field = civue_text_area_inlaid(
        ui, id, here(), &app->draft, placeholder, style, look, inlays);

      if (field.changed && !blank(app->draft) && !app->editing) {
        session_typing(app->session, c->id);
      }
    }

    div(flex, items_center, gap(1), flex_shrink_0, self_start, ml(2)) {
      if (!app->editing) {
        gif_button(app, box);
        sticker_button(app, box);
      }

      emoji_button(app, box);
    }
  }
}

void composer(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool allowed =
    store_can(app->session->store, c, PERM_SEND_MESSAGES) || !c->guild_id;

  // the foot of the chat, as flush with it as its header.
  div(relative, flex, flex_col, flex_shrink_0, bg(k->input), border_t(1),
      border_color(k->border)) {
    typing_tab(app, c);

    if (!allowed) {
      closed_row(
        k, CIVUE_ICON(lucide, lock),
        "You do not have permission to send messages in this channel.");
    }
    else {
      writing(app, c);
    }
  }

  if (!allowed) {
    return;
  }

  if (attachable(app, c) && !app->editing) {
    upload_drop_zone(app, c);
  }

  upload_dialogs(app, c);

  if (app->fake_ask) {
    // FakeNitro's own words.
    int answer = form_confirm(
      app, id("fake-nitro"), "Hold on!",
      "You are trying to send/edit a message that contains a FakeNitro emoji "
      "or sticker, however you do not have permissions to embed links in the "
      "current channel. Are you sure you want to send this message? Your "
      "FakeNitro items will appear as a link only.",
      "Send Anyway", false, false, nullptr);

    app->fake_ask = answer == 0;

    if (answer == 1) {
      submit(app, c, true);
    }
  }

  // a field just focused selects its text, the caret goes after it instead.
  if (app->focus_composer && civue_field_caret_to_end(ui, composer_id())) {
    app->focus_composer = false;
  }
}
