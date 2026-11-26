#include "components.h"

#include <math.h>
#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <civue/dsl.h>

#include "discord/uploads.h"
#include "widgets.h"

// the web client's tray: cards of 200 points, padded by 8, 24 apart.
constexpr float CARD = 200;
constexpr float CARD_PAD = 8;
constexpr float CARD_GAP = 24;
constexpr float CARD_NAME = 26;
// what pasted text may weigh before it becomes a file.
constexpr size_t PASTE_AS_FILE = 4000;
constexpr size_t MESSAGE_MAX = 2000;
constexpr size_t MESSAGE_MAX_NITRO = 4000;
constexpr size_t PROGRESS_WIDTH = 432;

static void format_size(uint64_t bytes, char *out, size_t cap) {
  if (bytes < 1024) {
    snprintf(out, cap, "%llu bytes", (unsigned long long)bytes);
  }
  else if (bytes < 1024 * 1024) {
    snprintf(out, cap, "%.2f KB", (double)bytes / 1024);
  }
  else {
    snprintf(out, cap, "%.2f MB", (double)bytes / (1024 * 1024));
  }
}

static void notice(App *app, const char *title, const char *body) {
  snprintf(app->upload_title, sizeof app->upload_title, "%s", title);
  snprintf(app->upload_body, sizeof app->upload_body, "%s", body);
}

static bool is_image(const Upload *u) {
  return u->content_type && !strncmp(u->content_type, "image/", 6);
}

static bool is_video(const Upload *u) {
  return u->content_type && !strncmp(u->content_type, "video/", 6);
}

// whether `adding` more files of `bytes` together fit the message, as the
// web client checks them before they reach the tray. says why when not.
static bool admit(App *app, Channel *c, size_t adding, uint64_t largest,
                  uint64_t bytes) {
  Session *s = app->session;
  Upload *held[UPLOADS_MAX];
  size_t n = uploads_of(s, c->id, 0, held, UPLOADS_MAX);
  uint64_t total = bytes, max = uploads_max_size(s, c);
  char line[160], size[32];

  for (size_t i = 0; i < n && i < UPLOADS_MAX; i++) {
    total += held[i]->size;
  }

  if (n + adding > UPLOADS_MAX) {
    notice(app, "Too many uploads!", "You can only upload 10 files at a time!");
    return false;
  }

  if (largest > max) {
    format_size(max, size, sizeof size);
    snprintf(line, sizeof line, "Max file size is %s please.", size);
    notice(app, "Your files are too powerful", line);
    return false;
  }

  if (total > UPLOADS_MAX_TOTAL) {
    notice(app, "Your files are too powerful",
           "Total message size must be less than 500 MB");
    return false;
  }

  return true;
}

// adds files picked, dropped or pasted, all of them or none.
static bool add_paths(App *app, Channel *c, const char *const *paths,
                      size_t count) {
  uint64_t largest = 0, bytes = 0;

  for (size_t i = 0; i < count; i++) {
    struct stat st;

    if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode)) {
      notice(app, "Invalid file type", "That file type is not supported.");
      return false;
    }

    largest = (uint64_t)st.st_size > largest ? (uint64_t)st.st_size : largest;
    bytes += (uint64_t)st.st_size;
  }

  if (!count || !admit(app, c, count, largest, bytes)) {
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    uploads_add(app->session, c->id, paths[i], nullptr, false);
  }

  app->focus_composer = true;
  return true;
}

static void free_strs(civue_strs *strs) {
  for (uint32_t i = 0; i < strs->len; i++) {
    free(strs->v[i]);
  }

  free(strs->v);
  *strs = (civue_strs){};
}

static void pick_files(App *app, Channel *c) {
  // a channel in slow mode takes one file at a time.
  civue_strs picked =
    civue_file_panel_open(nullptr, 0, c->rate_limit_per_user == 0);

  add_paths(app, c, (const char *const *)picked.v, picked.len);
  free_strs(&picked);
}

/* ========================================================================== */
/* text sent as a file: what is too long for a message, or pasted too long.   */
/* ========================================================================== */

// a single block of code goes as a file named after its language.
static bool text_as_file(App *app, Channel *c, const char *text) {
  size_t len = strlen(text);
  const char *body = text, *nl = strchr(text, '\n');
  char name[48] = "message.txt";
  size_t body_len = len;

  if (len > 6 && !strncmp(text, "```", 3) && !strcmp(text + len - 3, "```") &&
      nl && nl < text + len - 3) {
    size_t lang = (size_t)(nl - text - 3);
    bool word = lang > 0 && lang < 24;

    for (size_t i = 0; word && i < lang; i++) {
      char ch = text[3 + i];
      word = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '+' || ch == '-';
    }

    if (word) {
      snprintf(name, sizeof name, "message.%.*s", (int)lang, text + 3);
    }

    body = nl + 1;
    body_len = (size_t)(text + len - 3 - body);
  }

  if (!admit(app, c, 1, body_len, body_len)) {
    return false;
  }

  return uploads_add_bytes(app->session, c->id, body, body_len, name) != 0;
}

bool upload_message_too_long(App *app, const char *draft) {
  const User *self = app->session->store->self;
  size_t max = self && self->premium_type ? MESSAGE_MAX_NITRO : MESSAGE_MAX;
  size_t points = 0;

  // discord counts characters, not bytes.
  for (const unsigned char *p = (const unsigned char *)draft; p && *p; p++) {
    points += (*p & 0xc0) != 0x80;
  }

  return points > max;
}

/* ========================================================================== */
/* the clipboard.                                                             */
/* ========================================================================== */

static const char *extension_of(const char *mime) {
  if (mime && !strcmp(mime, "image/jpeg")) {
    return "jpg";
  }

  if (mime && !strcmp(mime, "image/gif")) {
    return "gif";
  }

  if (mime && !strcmp(mime, "image/webp")) {
    return "webp";
  }

  return "png";
}

bool upload_paste(App *app, Channel *c) {
  Session *s = app->session;
  civue_strs files = civue_pasteboard_files();
  defer free_strs(&files);

  if (files.len) {
    add_paths(app, c, (const char *const *)files.v, files.len);
    return true;
  }

  civue_bytes image = {};
  char *mime = nullptr;
  defer {
    free(image.v);
    free(mime);
  }

  if (civue_pasteboard_image(&image, &mime, nullptr) && image.len) {
    char name[32];

    snprintf(name, sizeof name, "image.%s", extension_of(mime));

    if (admit(app, c, 1, image.len, image.len)) {
      uploads_add_bytes(s, c->id, image.v, image.len, name);
    }

    return true;
  }

  // a long text pasted goes as a file, as the web client offers.
  char *text = civue_clipboard_text();
  bool long_text = text && strlen(text) > PASTE_AS_FILE;

  if (long_text) {
    text_as_file(app, c, text);
  }

  free(text);
  return long_text;
}

/* ========================================================================== */
/* the plus of the composer and its menu.                                     */
/* ========================================================================== */

void upload_button(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_rect anchor = {}, card = {};
  bool pick = false, as_file = false;
  bool has_text = app->draft && *app->draft && !app->editing;

  civue_button_state b =
    composer_button(app, id("upload-plus"), CIVUE_ICON(lucide, plus),
                    app->upload_menu, nullptr, &anchor);

  // a double click goes straight to the files.
  if (b.response.double_clicked) {
    app->upload_menu = false;
    pick = true;
  }
  else if (b.activated) {
    app->upload_menu = !app->upload_menu;
  }

  if (app->upload_menu) {
    overlay() {
      div(key(id("upload-menu")), flex, flex_col, left(px(anchor.x0 - 4)),
          bottom(px(ui->size.y - anchor.y0 + 20)), w(px(264)), p(2),
          rounded(lg), bg(k->surface_high), border(1),
          border_color(k->border_strong), shadow(lg)) {
        card = here();

        if (menu_row(app, id("upload-file"), CIVUE_ICON(lucide, upload),
                     "Upload a File", k->text)) {
          pick = true;
        }

        if (has_text &&
            menu_row(app, id("upload-message"), CIVUE_ICON(lucide, file_text),
                     "Upload your message as a file", k->text)) {
          as_file = true;
        }
      }
    }

    bool outside = ui->input.pressed[0] &&
                   !civue_rect_contains(card, ui->input.mouse) &&
                   !civue_rect_contains(anchor, ui->input.mouse);

    if (outside || pick || as_file || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
      app->upload_menu = false;
    }
  }

  if (as_file && text_as_file(app, c, app->draft)) {
    upload_clear_draft(app);
  }

  if (pick) {
    pick_files(app, c);
  }
}

/* ========================================================================== */
/* the tray over the field.                                                   */
/* ========================================================================== */

// the picture fitted in the open node, kept whole.
static void image_contain(App *app, const char *url) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  float w = civue_rect_w(r), h = civue_rect_h(r);

  if (w <= 0 || h <= 0) {
    return;
  }

  civue_image_info image =
    civue_images_get(app->images, ui, url, (uint32_t)(fmaxf(w, h) * ui->scale));

  if (image.state != CIVUE_IMAGE_READY || !image.width || !image.height) {
    return;
  }

  float scale = fminf(w / image.width, h / image.height);
  float iw = image.width * scale, ih = image.height * scale;
  civue_rect fit =
    civue_rect_xywh(r.x0 + (w - iw) / 2, r.y0 + (h - ih) / 2, iw, ih);

  civue_image(ui, image.texture, fit, (float[4]){0, 0, 1, 1}, 1, 8, false);
}

static bool bar_button(App *app, civue_id id, civue_icon glyph_icon,
                       const char *label, civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, h(6), min_w(px(24)), p(1),
      rounded(sm), text_color(color), hover(bg(k->hover)), cursor_pointer) {
    civue_button_state b = button();

    pressed = b.activated;
    hint(app, id, &b.response, label, CIVUE_TOOLTIP_ABOVE);
    glyph(glyph_icon, 18, color);
  }

  return pressed;
}

static void tag(const char *label) {
  div(px(px(3)), py(px(1)), rounded(px(3)), bg(rgb(0xf3f3f4)),
      text_color(rgb(0x000000)), text_size(px(10)), line_height(px(12)),
      font_weight(semibold)) {
    text(label);
  }
}

static void open_editor(App *app, const Upload *u) {
  app->upload_editing = u->id;
  free(app->upload_name);
  free(app->upload_alt);
  app->upload_name = strdup(u->filename);
  app->upload_alt = u->description ? strdup(u->description) : nullptr;
  app->upload_spoiler = u->spoiler;
}

static void card(App *app, Upload *u) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id key_id = id_at(id("upload-card"), u->id);
  char url[1100];
  bool remove = false;

  snprintf(url, sizeof url, "file://%s", u->path);

  div(key(key_id), relative, flex, flex_col, flex_shrink_0,
      w(px(CARD + CARD_PAD * 2 + 2)), h(px(CARD + CARD_PAD * 2 + 2)),
      p(px(CARD_PAD)), rounded(lg), bg(k->surface_high), border(1),
      border_color(k->border), shadow(sm)) {
    civue_rect r = here();
    bool hovered = ui->input.has_mouse &&
                   civue_rect_contains(r, ui->input.mouse) &&
                   !civue_under_overlay(ui, ui->input.mouse);

    div(relative, flex, items_center, justify_center, flex_1, min_h(0),
        rounded(lg), overflow_hidden) {
      if (is_image(u)) {
        image_contain(app, url);

        // the web client blurs it, it is hidden here.
        if (u->spoiler) {
          div(absolute, left(0), top(0), w(full), h(full), rounded(lg),
              bg(k->spoiler)) {}
        }
      }
      else {
        glyph(is_video(u) ? CIVUE_ICON(lucide, file_video)
                          : CIVUE_ICON(lucide, file),
              96, k->subtle);
      }

      if (u->description || u->spoiler) {
        div(absolute, left(px(4)), bottom(px(4)), flex, gap(1)) {
          if (u->description) {
            tag("ALT");
          }

          if (u->spoiler) {
            tag("SPOILER");
          }
        }
      }
    }

    text(u->filename, flex_shrink_0, mt(2), h(px(CARD_NAME - 8)), truncate,
         text_size(px(14)), line_height(px(18)), text_color(k->text));

    if (u->state == UPLOAD_FAILED && u->error) {
      div(absolute, left(px(CARD_PAD)), top(px(CARD_PAD)), px(1), rounded(sm),
          bg(k->danger), text_color(CIVUE_WHITE), text_size(px(10)),
          line_height(px(14)), font_weight(semibold)) {
        text("FAILED");
      }
    }

    if (hovered) {
      div(absolute, top(px(-8)), right(px(-8)), flex, items_center, h(8), px(1),
          rounded(md), bg(k->surface_high), border(1),
          border_color(k->border_strong), shadow(sm)) {
        if (bar_button(app, id_at(key_id, 1),
                       u->spoiler ? CIVUE_ICON(lucide, eye_off)
                                  : CIVUE_ICON(lucide, eye),
                       "Spoiler Attachment", k->subtle)) {
          u->spoiler = !u->spoiler;
        }

        if (bar_button(app, id_at(key_id, 2), CIVUE_ICON(lucide, pencil),
                       "Modify Attachment", k->subtle)) {
          open_editor(app, u);
        }

        remove = bar_button(app, id_at(key_id, 3), CIVUE_ICON(lucide, trash_2),
                            "Remove Attachment", rgb(0xff9d9a));
      }
    }
  }

  if (remove) {
    uploads_remove(app->session, u->id);
  }
}

void upload_tray(App *app, Channel *c) {
  const Look *k = &app->look;
  Upload *held[UPLOADS_MAX];
  size_t n = uploads_of(app->session, c->id, 0, held, UPLOADS_MAX);

  if (n == 0) {
    return;
  }

  n = n < UPLOADS_MAX ? n : UPLOADS_MAX;

  div(key(id("upload-tray")), flex, gap(px(CARD_GAP)), flex_shrink_0,
      overflow_x_scroll, overflow_y_hidden, scrollbars(&app->thin_bars),
      pt(px(20)), pb(px(10)), px(px(10)), border_b(1),
      border_color(k->border)) {
    for (size_t i = 0; i < n; i++) {
      card(app, held[i]);
    }

    div(w(px(6)), flex_shrink_0) {}
  }
}

/* ========================================================================== */
/* dropping files on the window.                                              */
/* ========================================================================== */

void upload_drop_zone(App *app, Channel *c) {
  civue_ui *ui = civue_ui_current();
  civue_input *in = &ui->input;
  bool insta = in->mods & CIVUE_MOD_SHIFT;
  char name[128], title[160];

  if (in->drops.len) {
    // holding shift sends them at once, with nothing typed.
    if (add_paths(app, c, (const char *const *)in->drops.v, in->drops.len) &&
        insta) {
      session_send(app->session, c->id, "", 0, false);
      app->stick = true;
    }

    free_strs(&in->drops);
    return;
  }

  if (!in->drag_over) {
    return;
  }

  store_channel_name(app->session->store, c, name, sizeof name);

  if (insta) {
    snprintf(title, sizeof title, "Insta Upload Mode!");
  }
  else if (channel_is_thread(c)) {
    snprintf(title, sizeof title, "Upload to thread");
  }
  else {
    snprintf(title, sizeof title,
             channel_is_private(c) ? "Upload to %s%s" : "Upload to #%s%s",
             c->type == CHANNEL_DM ? "@" : "", name);
  }

  overlay() {
    div(key(id("upload-drop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(rgba(0x000000cc))) {
      div(flex, w(px(310)), h(px(170)), p(2), rounded(px(10)),
          bg(app->look.brand)) {
        div(flex, flex_col, items_center, justify_center, flex_1, px(4),
            rounded(px(6)), border(2), border_color(rgba(0xffffff66)),
            text_color(CIVUE_WHITE)) {
          glyph(CIVUE_ICON(lucide, files), 48, CIVUE_WHITE);

          text(title, mt(3), max_w(full), truncate, text_size(px(22)),
               line_height(px(28)), font_weight(bold));

          if (!insta) {
            text("You can add comments before uploading.", mt(1),
                 text_size(px(12)), line_height(px(16)));
            text("Hold shift to upload directly.", text_size(px(12)),
                 line_height(px(16)));
          }
        }
      }
    }
  }
}

/* ========================================================================== */
/* a pending message's files, as they go up.                                  */
/* ========================================================================== */

void upload_progress(App *app, Channel *c, const Message *m) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  Upload *files[UPLOADS_MAX];
  size_t n = uploads_of(s, c->id, m->nonce, files, UPLOADS_MAX);
  uint64_t sent = 0, total = 0;
  char line[160], size[32];

  if (n == 0 || m->state != MESSAGE_PENDING) {
    return;
  }

  n = n < UPLOADS_MAX ? n : UPLOADS_MAX;

  for (size_t i = 0; i < n; i++) {
    sent += uploads_sent(files[i]);
    total += files[i]->size;

    if (files[i]->state == UPLOAD_SENDING) {
      civue_request_repaint_after(ui, 0.05);
    }
  }

  float done = total ? (float)((double)sent / (double)total) : 1;
  bool processing = sent >= total;

  format_size(total, size, sizeof size);

  if (n == 1) {
    snprintf(line, sizeof line, "%s", files[0]->filename);
  }
  else {
    snprintf(line, sizeof line, "Uploading %zu Files", n);
  }

  div(flex, items_center, mt(1), w(px(PROGRESS_WIDTH)), max_w(full), p(4),
      rounded(lg), bg(k->surface_high), border(1), border_color(k->border)) {
    div(mr(2), flex_shrink_0) {
      glyph(CIVUE_ICON(lucide, file), 36, k->subtle);
    }

    div(flex, flex_col, flex_1, min_w(0), gap(1)) {
      div(flex, items_center, gap(1), min_w(0)) {
        text(line, min_w(0), truncate, text_color(CIVUE_WHITE));

        if (!processing) {
          text(size, flex_shrink_0, ml(1), text_size(px(12)),
               text_color(k->muted));
        }
      }

      div(flex, items_center, h(4)) {
        if (processing) {
          text("Processing...", text_size(px(12)), line_height(px(16)),
               text_color(k->muted));
        }
        else {
          float shown =
            civue_animate(ui, id_at(id("upload-bar"), m->nonce), done, .2f);

          div(relative, flex_1, h(2), rounded(full),
              bg(civue_alpha(rgb(0x97979f), .2f)), overflow_hidden) {
            div(absolute, left(0), top(0), h(full), w(px(0)),
                style(.w = civue_relative(shown)), rounded(full),
                bg(k->brand)) {}
          }
        }
      }
    }

    if (!processing && icon_button(app, id_at(id("upload-cancel"), m->nonce),
                                   CIVUE_ICON(lucide, x), 24, 20, "Cancel")) {
      // the text comes back when nothing was typed since.
      if (!app->draft || !*app->draft) {
        free(app->draft);
        app->draft = strdup(m->content);
      }

      session_discard(s, c->id, m->nonce);
    }
  }
}

/* ========================================================================== */
/* the dialogs.                                                               */
/* ========================================================================== */

static void field_label(const Look *k, const char *label) {
  text(label, mt(2), text_size(px(14)), line_height(px(18)),
       font_weight(medium), text_color(k->strong));
}

// the files' modal: their name, their alt text and whether they are a
// spoiler.
static void editor(App *app, Upload *u) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool close = false, save = false;
  bool media = is_image(u) || is_video(u);

  overlay() {
    div(key(id("upload-editor-backdrop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(rgba(0x000000b3))) {
      close = button().activated;

      div(key(id("upload-editor")), flex, flex_col, gap(2), w(px(440)), p(6),
          rounded(xl), bg(k->surface), border(1), border_color(k->border),
          shadow(lg), text_color(k->text)) {
        button();

        text(u->filename, text_size(px(20)), line_height(px(24)), truncate,
             font_weight(semibold), text_color(k->strong));

        field_label(k, "Filename");

        div(key(id("upload-editor-name")), w(full), h(10)) {
          save = text_field(&app->upload_name, "", field_look(k)).submitted;
        }

        if (media) {
          field_label(k, "Description (Alt Text)");

          div(key(id("upload-editor-alt")), w(full), h(10)) {
            save |=
              text_field(&app->upload_alt, "Add a description", field_look(k))
                .submitted;
          }
        }

        bool forced =
          app->upload_name && !strncmp(app->upload_name, "SPOILER_", 8);
        bool on = app->upload_spoiler || forced;

        div(key(id("upload-editor-spoiler")), flex, items_center, gap(2), mt(2),
            cursor_pointer) {
          if (button().activated && !forced) {
            app->upload_spoiler = !app->upload_spoiler;
          }

          div(flex, items_center, justify_center, size(5), flex_shrink_0,
              rounded(md), border(2),
              border_color(on ? k->brand : k->border_strong),
              style(.bg = on ? k->brand : CIVUE_CLEAR),
              text_color(CIVUE_WHITE)) {
            if (on) {
              icon(lucide, check, size(px(14)));
            }
          }

          text("Mark as spoiler", text_color(k->strong));
        }

        div(flex, gap(2), flex_shrink_0, mt(4)) {
          close |=
            dialog_button(k, id("upload-editor-cancel"), "Cancel", false);
          save |= dialog_button(k, id("upload-editor-save"), "Save", true);
        }
      }
    }
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    close = true;
  }

  if (save && app->upload_name && *app->upload_name) {
    size_t alt = app->upload_alt ? strlen(app->upload_alt) : 0;

    free(u->filename);
    u->filename = strndup(app->upload_name, 1024);
    free(u->description);
    u->description = alt ? strndup(app->upload_alt, 1024) : nullptr;
    u->spoiler = app->upload_spoiler || !strncmp(u->filename, "SPOILER_", 8);
    close = true;
  }

  if (close) {
    app->upload_editing = 0;
  }
}

static void message_box(App *app, const char *title, const char *body,
                        const char *action, bool *acted, bool *closed) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();

  overlay() {
    div(key(id("upload-notice-backdrop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(rgba(0x000000b3))) {
      *closed = button().activated;

      div(key(id("upload-notice")), flex, flex_col, gap(2), w(px(400)), p(6),
          rounded(xl), bg(k->surface), border(1), border_color(k->border),
          shadow(lg), text_color(k->text)) {
        button();

        text(title, text_size(px(20)), line_height(px(24)),
             font_weight(semibold), text_color(k->strong));
        text(body, text_color(k->subtle));

        div(flex, gap(2), flex_shrink_0, mt(4)) {
          if (action) {
            *closed |=
              dialog_button(k, id("upload-notice-cancel"), "Cancel", false);
            *acted = dialog_button(k, id("upload-notice-act"), action, true);
          }
          else {
            *closed |= dialog_button(k, id("upload-notice-ok"), "Okay", true);
          }
        }
      }
    }
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) ||
      (!action && civue_key_pressed(ui, CIVUE_KEY_ENTER))) {
    *closed = true;
  }
}

void upload_dialogs(App *app, Channel *c) {
  Upload *u = uploads_get(app->session, app->upload_editing);
  bool acted = false, closed = false;

  if (app->upload_editing && (!u || u->nonce)) {
    app->upload_editing = 0;
  }
  else if (u) {
    editor(app, u);
    return;
  }

  if (app->long_message) {
    const User *self = app->session->store->self;
    bool nitro = self && self->premium_type;

    message_box(app, "Your message is too long...",
                nitro ? "Messages can be at most 4000 characters long."
                      : "Messages can be at most 2000 characters long. You "
                        "can send it as a file instead.",
                nitro ? nullptr : "Upload as File", &acted, &closed);

    if (acted && app->draft && text_as_file(app, c, app->draft)) {
      upload_clear_draft(app);
    }

    if (acted || closed) {
      app->long_message = false;
    }

    return;
  }

  if (*app->upload_title) {
    message_box(app, app->upload_title, app->upload_body, nullptr, &acted,
                &closed);

    if (closed) {
      app->upload_title[0] = 0;
    }
  }
}
