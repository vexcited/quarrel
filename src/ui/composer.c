#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/uploads.h"
#include "media.h"
#include "widgets.h"

constexpr float LINE = 22;
constexpr float MAX_LINES = 12;

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

static void submit(App *app, Channel *c) {
  Session *session = app->session;

  if (app->editing) {
    if (!blank(app->draft)) {
      session_edit(session, c->id, app->editing, app->draft);
    }

    stop_editing(app);
    return;
  }

  Upload *files[1];
  bool attached = uploads_of(session, c->id, 0, files, 1) != 0;

  if (blank(app->draft) && !attached) {
    return;
  }

  if (upload_message_too_long(app, app->draft)) {
    app->long_message = true;
    return;
  }

  session_send(session, c->id, blank(app->draft) ? "" : app->draft,
               app->reply_to, app->reply_ping);
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
      submit(app, c);
    }
    else if (press.key == CIVUE_KEY_ESCAPE && app->editing) {
      drop_key(ui, i--);
      stop_editing(app);
    }
    else if (press.key == CIVUE_KEY_ESCAPE && app->reply_to) {
      drop_key(ui, i--);
      app->reply_to = 0;
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
      media_viewing(app)) {
    return;
  }

  size_t had = app->draft ? strlen(app->draft) : 0;

  app->draft = realloc(app->draft, had + typed->len + 1);
  memcpy(app->draft + had, typed->v, typed->len);
  app->draft[had + typed->len] = 0;
  typed->len = 0;
  app->focus_composer = true;
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

  div(flex, items_center, gap(2), h(9), px(4), rounded_t(lg), bg(k->panel),
      text_size(px(14)), line_height(px(18)), text_color(k->subtle)) {
    if (app->editing) {
      text("Editing message", flex_1, font_weight(medium));
      text("escape to cancel, enter to save", text_color(k->faint));
    }
    else {
      Author author = store_author(store, to);

      text("Replying to");
      text(author.name, flex_1, min_w(0), truncate, font_weight(semibold),
           text_color(author.color ? rgb(author.color) : k->strong));

      div(key(id("reply-ping")), flex, items_center, gap(1), px(2),
          font_weight(bold), cursor_pointer,
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

void composer(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  civue_id id = composer_id();
  civue_text_style style = civue_style(16, 400, LINE, 0);
  char name[128], placeholder[160];
  bool allowed = store_can(store, c, PERM_SEND_MESSAGES) || !c->guild_id;
  bool barred = app->editing || app->reply_to;

  store_channel_name(store, c, name, sizeof name);
  snprintf(placeholder, sizeof placeholder,
           channel_is_private(c) ? "Message %s%s" : "Message #%s%s",
           c->type == CHANNEL_DM ? "@" : "", name);

  if (!allowed) {
    div(flex, items_center, flex_shrink_0, min_h(px(56)), mx(2), px(4),
        rounded(lg), bg(k->input), border(1), border_color(k->border)) {
      text("You do not have permission to send messages in this channel.",
           text_color(k->faint));
    }

    return;
  }

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
  float lines = LINE;

  if (width > 0 && *draft) {
    lines = civue_layout(ui, draft, style, width)->height;

    if (draft[strlen(draft) - 1] == '\n') {
      lines += LINE;
    }
  }

  lines = fminf(fmaxf(lines, LINE), LINE * MAX_LINES);

  div(flex, flex_col, flex_shrink_0, mx(2)) {
    context_bar(app, c);

    div(flex, flex_col, bg(k->input), border(1), border_color(k->border),
        style(.radius = {civue_px(barred ? 0 : 8), civue_px(barred ? 0 : 8),
                         civue_px(8), civue_px(8)})) {
      civue_rect box = here();

      if (!app->editing) {
        upload_tray(app, c);
      }

      div(flex, items_center, gap(3), min_h(px(54)), px(4), py(3)) {
        if (attachable(app, c) && !app->editing) {
          upload_button(app, c);
        }
        else {
          icon(lucide, circle_plus, size(5), flex_shrink_0, self_start, mt(1),
               text_color(k->faint));
        }

        div(key(id), flex_1, min_w(0), h(px(lines + 8))) {
          civue_field_look look = {
            .editing = {.text = k->text},
            .idle = {.text = k->text},
            .hovered = {.text = k->text},
            .placeholder = k->faint,
            .selection = civue_alpha(k->brand, .45f),
          };
          civue_field_response field = civue_text_area(
            ui, id, here(), &app->draft, placeholder, style, look);

          if (field.changed && !blank(app->draft) && !app->editing) {
            session_typing(app->session, c->id);
          }
        }

        if (!app->editing) {
          gif_button(app, box);
        }
      }
    }
  }

  if (attachable(app, c) && !app->editing) {
    upload_drop_zone(app, c);
  }

  upload_dialogs(app, c);

  // a field just focused selects its text, the caret goes after it instead.
  if (app->focus_composer && civue_field_caret_to_end(ui, id)) {
    app->focus_composer = false;
  }
}
