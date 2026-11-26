#include "components.h"

#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/commands.h"
#include "media.h"
#include "widgets.h"

/* ========================================================================== */
/* what an application builds a message with, as discord's client lays it     */
/* out. the sizes come from its stylesheets.                                  */
/* ========================================================================== */

constexpr float CONTAINER_WIDTH = 600;
constexpr float CONTAINER_PAD = 16;
constexpr float THUMBNAIL = 85;
constexpr float SELECT_WIDTH = 400;
constexpr float SELECT_ROW = 44;
constexpr float SELECT_MOST = 320;

typedef struct {
  App *app;
  const Message *m;
  Snowflake guild_id;
  // what the keys hang from, and how many were handed out.
  civue_id key;
  uint32_t keys;
} Walk;

static civue_id next_key(Walk *w) {
  return id_at(w->key, ++w->keys);
}

static Snowflake message_id(const Message *m) {
  return m->id ? m->id : m->nonce;
}

static bool is_select(const Component *c) {
  return c->type == COMPONENT_STRING_SELECT ||
         (c->type >= COMPONENT_USER_SELECT &&
          c->type <= COMPONENT_CHANNEL_SELECT);
}

static MediaItem media_item(const ComponentMedia *cm) {
  const EmbedMedia *media = &cm->media;
  const char *type = cm->content_type ? cm->content_type : "";
  bool video = !strncmp(type, "video/", 6) && media->proxy_url;

  return (MediaItem){
    .kind = video ? MEDIA_VIDEO : MEDIA_IMAGE,
    .url = media->url,
    .proxy_url = media->proxy_url,
    .poster = media->proxy_url,
    .width = media->width,
    .height = media->height,
    .animated = !video && (!strcmp(type, "image/gif") || media->animated),
    .spoiler = cm->spoiler,
  };
}

static void emoji_of(App *app, Snowflake emoji_id, const char *name,
                     bool animated, float size) {
  if (emoji_id && animated) {
    div(size(px(size)), flex_shrink_0) {
      emoji_image(app, emoji_id, true, here());
    }
  }
  else {
    emoji_node(app, emoji_id, name, size);
  }
}

static void text_display(Walk *w, const Component *c, float room) {
  if (!c->text || !*c->text) {
    return;
  }

  markup(w->app, next_key(w), c->text,
         &(MarkupOptions){.guild_id = w->guild_id, .hint = room});
}

static void thumbnail(Walk *w, const Component *c) {
  civue_id id = next_key(w);

  if (!c->nmedia || !c->media[0].media.url) {
    return;
  }

  MediaItem item = media_item(&c->media[0]);

  media_tile(w->app, id, message_id(w->m), &item, 1, 0,
             point(THUMBNAIL, THUMBNAIL), 8);
}

static void gallery(Walk *w, const Component *c) {
  MediaItem items[10];
  civue_id ids[10];
  civue_id box = next_key(w);
  size_t n = 0;

  for (size_t i = 0; i < c->nmedia && n < countof(items); i++) {
    if (c->media[i].media.url) {
      items[n] = media_item(&c->media[i]);
      ids[n++] = id_at(box, i + 1);
    }
  }

  if (n) {
    media_mosaic(w->app, box, message_id(w->m), items, ids, n);
  }
}

static void file(Walk *w, const Component *c) {
  civue_id id = next_key(w);

  if (!c->nmedia || !c->media[0].media.url) {
    return;
  }

  // the name of an attachment is the end of its address.
  const char *url = c->media[0].media.url;
  const char *slash = strrchr(url, '/');
  char name[256];

  snprintf(name, sizeof name, "%s",
           c->text ? c->text
           : slash ? slash + 1
                   : url);
  name[strcspn(name, "?")] = 0;

  file_card(
    w->app, id,
    &(Attachment){.filename = name, .url = (char *)url, .size = c->size});
}

static void separator(Walk *w, const Component *c) {
  const Look *k = &w->app->look;
  float space = c->large ? 8 : 0;

  div(flex, items_center, flex_shrink_0, w(full), h(px(1 + 2 * space))) {
    if (c->divider) {
      div(flex_1, h(px(1)), bg(k->border_strong)) {}
    }
  }
}

static void button_component(Walk *w, const Component *c) {
  App *app = w->app;
  const Look *k = &app->look;
  civue_id id = next_key(w);
  bool link = c->style == BUTTON_STYLE_LINK;
  bool busy =
    !link && commands_component_busy(app->session, w->m->id, c->custom_id);
  // one of a message not sent yet, or of a preview, answers nothing.
  bool dead = c->disabled || w->m->state != MESSAGE_SENT ||
              (link ? !c->url : !c->custom_id);
  // the colors of the buttons of the forms, see forms.c.
  civue_color ground = k->hover, lit = k->pressed, down = k->selected;
  civue_color ink = k->strong;

  // a premium button sells something Quarrel has no shop for.
  if (c->style == BUTTON_STYLE_PREMIUM) {
    return;
  }

  if (c->style == BUTTON_STYLE_PRIMARY) {
    ground = k->primary, lit = k->primary_hover, down = k->primary_active;
    ink = k->on_primary;
  }
  else if (c->style == BUTTON_STYLE_SUCCESS) {
    ground = k->positive, lit = k->positive_hover, down = k->positive_active;
    ink = k->on_positive;
  }
  else if (c->style == BUTTON_STYLE_DANGER) {
    ground = k->danger, lit = k->danger_hover, down = k->danger_active;
    ink = k->on_danger;
  }

  if (dead || busy) {
    ground = civue_alpha(ground, .5f);
    lit = down = ground;
    ink = civue_alpha(ink, .5f);
  }

  div(key(id), flex, items_center, justify_center, gap(1), flex_shrink_0, h(8),
      min_w(px(c->text ? 60 : 32)), max_w(full), px(px(11)), border(1),
      border_color(CIVUE_CLEAR), rounded(lg), bg(ground), hover(bg(lit)),
      active(bg(down)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), text_color(ink),
      style(.cursor = dead ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    if (button().activated && !dead && !busy) {
      if (link) {
        civue_open_url(c->url);
      }
      else {
        commands_component(app->session, w->m, c, nullptr, 0);
      }
    }

    if (c->emoji_id || c->emoji_name) {
      emoji_of(app, c->emoji_id, c->emoji_name, c->emoji_animated, 18);
    }

    if (c->text) {
      text(c->text, min_w(0), truncate);
    }

    if (link) {
      icon(mingcute, external_link_fill, size(4), flex_shrink_0);
    }
  }
}

// what a menu shows while closed: the options picked for it, else what the
// application wrote to stand there.
static const char *select_label(const Component *c, char *out, size_t cap,
                                const ComponentOption **only) {
  size_t at = 0, n = 0;

  out[0] = 0;
  *only = nullptr;

  for (size_t i = 0; i < c->noptions; i++) {
    const ComponentOption *o = &c->options[i];

    if (!o->chosen || !o->label) {
      continue;
    }

    int wrote = snprintf(out + at, cap - at, "%s%s", n ? ", " : "", o->label);

    at = wrote < 0 || at + (size_t)wrote >= cap ? cap - 1 : at + (size_t)wrote;
    *only = n++ ? nullptr : o;
  }

  return n ? out : c->text ? c->text : "Make a selection";
}

static void select_component(Walk *w, const Component *c) {
  App *app = w->app;
  const Look *k = &app->look;
  civue_id id = next_key(w);
  bool open = app->popover == POPOVER_COMPONENT_SELECT &&
              app->select_message == w->m->id && app->select_custom &&
              c->custom_id && !strcmp(app->select_custom, c->custom_id);
  bool busy = commands_component_busy(app->session, w->m->id, c->custom_id);
  // only a menu of strings lists what it offers itself.
  bool dead = c->disabled || w->m->state != MESSAGE_SENT || !c->custom_id ||
              c->type != COMPONENT_STRING_SELECT || !c->noptions;
  float fade = dead || busy ? .5f : 1;
  const ComponentOption *only;
  char label[256];
  const char *shown = select_label(c, label, sizeof label, &only);

  div(key(id), flex, items_center, gap(2), flex_shrink_0, w(full),
      max_w(px(SELECT_WIDTH)), h(11), px(3), rounded(lg), bg(k->input),
      border(1), border_color(open ? k->ring : k->border_strong),
      text_size(px(16)), line_height(px(20)),
      style(.cursor = dead ? CIVUE_CURSOR_DEFAULT : CIVUE_CURSOR_POINTER,
            .has_cursor = true)) {
    civue_rect at = here();

    if (button().activated && !dead && !busy) {
      if (open) {
        app->popover = POPOVER_NONE;
      }
      else {
        app->popover = POPOVER_COMPONENT_SELECT;
        app->select_message = w->m->id;
        app->select_anchor = at;
        app->select_picked = 0;
        app->select_changed = false;
        free(app->select_custom);
        app->select_custom = strdup(c->custom_id);

        for (size_t i = 0; i < c->noptions && i < 32; i++) {
          app->select_picked |= (uint32_t)c->options[i].chosen << i;
        }
      }
    }

    if (only && (only->emoji_id || only->emoji_name)) {
      emoji_of(app, only->emoji_id, only->emoji_name, only->emoji_animated, 20);
    }

    text(shown, flex_1, min_w(0), truncate,
         text_color(civue_alpha(shown == label ? k->text : k->muted, fade)));
    icon(mingcute, down_fill, size(5), flex_shrink_0,
         text_color(civue_alpha(k->subtle, fade)));
  }
}

static void action_row(Walk *w, const Component *c) {
  div(flex, flex_wrap, gap(2), w(full), min_w(0)) {
    for (size_t i = 0; i < c->nchildren; i++) {
      const Component *child = &c->children[i];

      if (child->type == COMPONENT_BUTTON) {
        button_component(w, child);
      }
      else if (is_select(child)) {
        select_component(w, child);
      }
    }
  }
}

static void section(Walk *w, const Component *c, float room) {
  const Component *side = c->accessory;
  bool pictured = side && side->type == COMPONENT_THUMBNAIL;
  float beside = !side ? 0 : pictured ? THUMBNAIL + 12 : 120;

  div(flex, items_start, gap(3), w(full), min_w(0)) {
    div(flex, flex_col, gap(2), flex_1, min_w(0)) {
      for (size_t i = 0; i < c->nchildren; i++) {
        if (c->children[i].type == COMPONENT_TEXT_DISPLAY) {
          text_display(w, &c->children[i], room - beside);
        }
      }
    }

    if (pictured) {
      thumbnail(w, side);
    }
    else if (side && side->type == COMPONENT_BUTTON) {
      button_component(w, side);
    }
  }
}

static void component(Walk *w, const Component *c, float room);

static void container(Walk *w, const Component *c, float room) {
  App *app = w->app;
  const Look *k = &app->look;
  civue_id id = next_key(w);
  float wide = fminf(room, CONTAINER_WIDTH);
  float inner = wide - 2 * CONTAINER_PAD - (c->has_color ? 4 : 0);
  bool hidden = c->spoiler && !app_revealed(app, id);

  // the colored edge is the card's own ground, showing left of its content.
  div(key(id), relative, flex, w(full), max_w(px(CONTAINER_WIDTH)),
      pl(px(c->has_color ? 4 : 0)), rounded(lg),
      style(.bg = c->has_color ? rgb(c->color) : CIVUE_CLEAR),
      overflow_hidden) {
    div(flex, flex_col, gap(2), flex_1, min_w(0), p(px(CONTAINER_PAD)),
        bg(k->surface_high), border(1), border_color(k->border),
        style(.radius = {civue_px(c->has_color ? 0 : 8), civue_px(8),
                         civue_px(8), civue_px(c->has_color ? 0 : 8)})) {
      for (size_t i = 0; i < c->nchildren; i++) {
        component(w, &c->children[i], inner);
      }
    }

    if (hidden) {
      div(absolute, left(0), top(0), w(full), h(full), flex, items_center,
          justify_center, bg(k->spoiler), cursor_pointer) {
        if (button().activated) {
          app_reveal(app, id);
        }

        div(px(3), py(px(6)), rounded(full), bg(k->popover)) {
          text("SPOILER", text_size(px(15)), line_height(px(18)),
               font_weight(semibold), text_color(k->strong));
        }
      }
    }
  }
}

static void component(Walk *w, const Component *c, float room) {
  switch (c->type) {
    case COMPONENT_ACTION_ROW:
      action_row(w, c);
      break;
    case COMPONENT_SECTION:
      section(w, c, room);
      break;
    case COMPONENT_TEXT_DISPLAY:
      text_display(w, c, room);
      break;
    case COMPONENT_MEDIA_GALLERY:
      gallery(w, c);
      break;
    case COMPONENT_FILE:
      file(w, c);
      break;
    case COMPONENT_SEPARATOR:
      separator(w, c);
      break;
    case COMPONENT_CONTAINER:
      container(w, c, room);
      break;
    default:
      break;
  }
}

void message_components(App *app, const Message *m, Snowflake guild_id,
                        civue_id id, float room) {
  Walk w = {.app = app, .m = m, .guild_id = guild_id, .key = id};

  if (!m->ncomponents) {
    return;
  }

  div(flex, flex_col, gap(2), mt(1), w(full), min_w(0)) {
    for (size_t i = 0; i < m->ncomponents; i++) {
      component(&w, &m->components[i], room);
    }
  }
}

/* ========================================================================== */
/* the options of a menu, under it.                                           */
/* ========================================================================== */

static const Component *find_select(const Component *list, size_t n,
                                    const char *custom_id) {
  for (size_t i = 0; i < n; i++) {
    const Component *c = &list[i];
    const Component *found = nullptr;

    if (is_select(c) && c->custom_id && !strcmp(c->custom_id, custom_id)) {
      return c;
    }

    if ((found = find_select(c->children, c->nchildren, custom_id))) {
      return found;
    }
  }

  return nullptr;
}

static void select_send(App *app, const Message *m, const Component *c) {
  const char *values[32];
  size_t n = 0;

  for (size_t i = 0; i < c->noptions && i < countof(values); i++) {
    if ((app->select_picked >> i & 1) && c->options[i].value) {
      values[n++] = c->options[i].value;
    }
  }

  commands_component(app->session, m, c, values, n);
}

static bool select_option(App *app, civue_id id, const ComponentOption *o,
                          bool on, bool several) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(2), flex_shrink_0, h(px(SELECT_ROW)),
      px(3), style(.bg = on ? k->selected : CIVUE_CLEAR), hover(bg(k->hover)),
      cursor_pointer) {
    pressed = button().activated;

    if (o->emoji_id || o->emoji_name) {
      emoji_of(app, o->emoji_id, o->emoji_name, o->emoji_animated, 20);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      text(o->label ? o->label : "", truncate, text_size(px(14)),
           line_height(px(18)), font_weight(medium), text_color(k->strong));

      if (o->description) {
        text(o->description, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->subtle));
      }
    }

    if (on || several) {
      div(flex, items_center, justify_center, size(5), flex_shrink_0,
          rounded(full), border(on ? 0 : 1), border_color(k->border_strong),
          style(.bg = on ? k->primary : CIVUE_CLEAR)) {
        if (on) {
          icon(mingcute, check_fill, size(px(14)), text_color(k->on_primary));
        }
      }
    }
  }

  return pressed;
}

void component_select(App *app, Channel *channel) {
  if (app->popover != POPOVER_COMPONENT_SELECT) {
    return;
  }

  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  const Message *m = store_message(channel, app->select_message);
  const Component *c =
    m && app->select_custom
      ? find_select(m->components, m->ncomponents, app->select_custom)
      : nullptr;

  // the menu went away under its options.
  if (!c || !c->noptions) {
    app->popover = POPOVER_NONE;
    return;
  }

  civue_rect anchor = app->select_anchor;
  civue_rect room = overlay_room();
  bool several = c->max_values > 1;
  uint32_t before = app->select_picked;
  float width = (float)civue_rect_w(anchor);
  float height = fminf((float)c->noptions * SELECT_ROW, SELECT_MOST) + 2;
  float top = (float)anchor.y1 + 4;
  civue_rect card = {};
  bool done = false;

  if (top + height > room.y1) {
    top = fmaxf((float)anchor.y0 - 4 - height, (float)room.y0);
  }

  overlay() {
    div(key(id("component-select")), flex, flex_col, w(px(width)),
        max_h(px(height)), left(px(anchor.x0)), top(px(top)), rounded(lg),
        bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg), overflow_y_scroll, scrollbars(&app->bars)) {
      card = here();

      for (size_t i = 0; i < c->noptions && i < 32; i++) {
        bool on = app->select_picked >> i & 1;

        if (!select_option(app, id_at(id("component-option"), i),
                           &c->options[i], on, several)) {
          continue;
        }

        if (!several) {
          app->select_picked = 1u << i;
          done = true;
        }
        else if (on || (uint32_t)__builtin_popcount(app->select_picked) <
                         c->max_values) {
          app->select_picked ^= 1u << i;
        }
      }
    }
  }

  // one that takes several is sent as it closes.
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);
  bool closed = outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE);
  uint32_t picked = (uint32_t)__builtin_popcount(app->select_picked);

  app->select_changed |= app->select_picked != before;

  if (several && closed && app->select_changed && picked >= c->min_values) {
    done = true;
  }

  if (done) {
    select_send(app, m, c);
  }

  if (done || closed) {
    app->popover = POPOVER_NONE;
    app->select_changed = false;
  }
}
