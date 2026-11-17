#include "widgets.h"

#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/markdown.h"

// how the text of a run is drawn.
typedef struct {
  bool bold, italic, underline, strike, code;
  // a rounded ground under it: mentions, inline code and timestamps.
  bool pill;
  civue_color ground;
  civue_color color;
  // a spoiler not clicked yet, its text hidden.
  bool hidden;
  civue_id spoiler;
  const char *url;
  Snowflake channel_id;
} Run;

// pieces flow from left to right in lines of one height, wrapped at the
// width. the same walk measures and paints.
typedef struct {
  App *app;
  civue_ui *ui;
  const MarkupOptions *o;
  civue_id key;
  bool paint;
  civue_vec2 origin;
  float width, indent;
  float x, y;
  float size, line, weight;
  // something sits on the current line.
  bool dirty;
  bool jumbo;
  uint32_t pieces, spoilers;
} Flow;

typedef struct {
  float w, h;
  uint64_t hash;
} Kept;

static civue_text_style style_of(const Flow *f, const Run *r) {
  civue_text_style s =
    civue_style(f->size, r->bold ? 700 : f->weight, f->line, 0);

  s.italic = r->italic;

  if (r->code) {
    s.mono = true;
    s.size = f->size * .85f;
  }

  if (r->pill && !r->code) {
    s.weight = fmaxf(s.weight, 500);
  }

  return s;
}

static float measure(Flow *f, const char *s, size_t n, civue_text_style st) {
  return n ? civue_layout_n(f->ui, s, n, st, 0)->width : 0;
}

// a space is measured between two letters, a text's own trailing ones not
// being counted by every shaper.
static float space_of(Flow *f, civue_text_style st) {
  return measure(f, "n n", 3, st) - measure(f, "nn", 2, st);
}

static void new_line(Flow *f) {
  f->x = f->indent;
  f->y += f->line;
  f->dirty = false;
}

static void end_block(Flow *f) {
  if (f->dirty) {
    new_line(f);
  }

  f->x = f->indent;
}

static bool revealed(const App *app, civue_id spoiler) {
  for (uint32_t i = 0; i < app->revealed.len; i++) {
    if (app->revealed.v[i] == spoiler) {
      return true;
    }
  }

  return false;
}

static void reveal(App *app, civue_id spoiler) {
  if (app->revealed.len == app->revealed.cap) {
    app->revealed.cap = app->revealed.cap ? app->revealed.cap * 2 : 16;
    app->revealed.v =
      realloc(app->revealed.v, app->revealed.cap * sizeof *app->revealed.v);
  }

  app->revealed.v[app->revealed.len++] = spoiler;
}

// what a piece does under the pointer: a spoiler shows, a link opens, a
// channel mention goes to its channel. true while a link is hovered.
static bool interact_piece(Flow *f, const Run *r, civue_rect at) {
  if (!r->hidden && !r->url && !r->channel_id) {
    return false;
  }

  civue_id id = id_at(f->key, 0x1000 + f->pieces);
  civue_response resp = civue_interact(f->ui, id, at, CIVUE_SENSE_CLICK);

  if (resp.hovered) {
    civue_set_cursor(f->ui, CIVUE_CURSOR_POINTER);
  }

  if (r->hidden) {
    if (resp.clicked) {
      reveal(f->app, r->spoiler);
      civue_request_repaint(f->ui);
    }

    return false;
  }

  if (resp.clicked && r->url) {
    civue_open_url(r->url);
  }
  else if (resp.clicked) {
    app_select_channel(f->app, r->channel_id);
  }

  return resp.hovered && r->url;
}

static void paint_piece(Flow *f, const Run *r, const char *s, size_t n,
                        civue_text_style st, float w) {
  const Look *k = &f->app->look;
  double x = f->origin.x + f->x;
  double y = f->origin.y + f->y;
  civue_rect at = xywh(x, y, w, f->line);
  bool hovered = interact_piece(f, r, at);

  if (r->hidden) {
    fill(civue_rect_inflate(at, 0, -1), k->spoiler, 3);
    return;
  }

  if (r->pill) {
    fill(civue_rect_inflate(at, 2, -1), r->ground, 4);
  }

  if (n) {
    const civue_text_block *b = civue_layout_n(f->ui, s, n, st, 0);

    civue_draw_text(f->ui, b, point(x, y), r->color);
  }

  double middle = y + f->line / 2;

  if (r->underline || hovered) {
    civue_hline(f->ui, x, x + w, middle + st.size * .5, r->color);
  }

  if (r->strike) {
    civue_hline(f->ui, x, x + w, middle + 1, r->color);
  }
}

// one piece that is never cut, moved to the next line when it does not fit.
static void piece(Flow *f, const Run *r, const char *s, size_t n,
                  civue_text_style st, float w) {
  if (f->x > f->indent && f->x + w > f->width + .5f) {
    new_line(f);
  }

  if (f->paint) {
    paint_piece(f, r, s, n, st, w);
  }

  f->pieces++;
  f->x += w;
  f->dirty = true;
}

// how many bytes of `s`, ending where a word does, fit in `room`. 0 when not
// even its first word does.
static size_t words_fitting(Flow *f, const char *s, size_t n,
                            civue_text_style st, float room, float whole,
                            float *width) {
  size_t guess = whole > 0 ? (size_t)((float)n * room / whole) : n;
  size_t end = guess < n ? guess : n;

  while (end > 0 && end < n && s[end] != ' ') {
    end--;
  }

  while (end > 0 && s[end - 1] == ' ') {
    end--;
  }

  float w = measure(f, s, end, st);

  while (end > 0 && w > room) {
    while (end > 0 && s[end - 1] != ' ') {
      end--;
    }

    while (end > 0 && s[end - 1] == ' ') {
      end--;
    }

    w = measure(f, s, end, st);
  }

  // the guess may have fallen short of what fits.
  for (;;) {
    size_t next = end;

    while (next < n && s[next] == ' ') {
      next++;
    }

    while (next < n && s[next] != ' ') {
      next++;
    }

    if (next == end) {
      break;
    }

    float wider = measure(f, s, next, st);

    if (wider > room) {
      break;
    }

    end = next;
    w = wider;
  }

  *width = w;
  return end;
}

// the bytes of a word too long for a line that fit in `room`, whole
// characters, one at least.
static size_t letters_fitting(Flow *f, const char *s, size_t n,
                              civue_text_style st, float room, float *width) {
  size_t end = 0;
  float w = 0;

  while (end < n) {
    size_t next = end;

    civue_utf8_next(s, n, &next);

    float wider = measure(f, s, next, st);

    if (wider > room && end > 0) {
      break;
    }

    end = next;
    w = wider;
  }

  *width = w;
  return end;
}

static void words(Flow *f, const Run *r, const char *s, size_t n) {
  civue_text_style st = style_of(f, r);
  float space = space_of(f, st);
  size_t i = 0;

  while (i < n) {
    if (s[i] == ' ' || s[i] == '\n') {
      // a line never starts with the space it was cut at.
      if (f->dirty) {
        piece(f, r, nullptr, 0, st, space);
      }

      i++;
      continue;
    }

    size_t rest = n - i;

    for (size_t j = i; j < n; j++) {
      if (s[j] == '\n') {
        rest = j - i;
        break;
      }
    }

    size_t trimmed = rest;

    while (trimmed > 0 && s[i + trimmed - 1] == ' ') {
      trimmed--;
    }

    float whole = measure(f, s + i, trimmed, st);
    float room = f->width - f->x;

    if (whole <= room + .5f) {
      piece(f, r, s + i, trimmed, st, whole);
      i += trimmed;
      continue;
    }

    float w = 0;
    size_t fit = words_fitting(f, s + i, trimmed, st, room, whole, &w);

    if (fit) {
      piece(f, r, s + i, fit, st, w);
      i += fit;
      new_line(f);
      continue;
    }

    if (f->dirty) {
      new_line(f);
      continue;
    }

    // a word wider than a whole line, like an url, is cut where it ends.
    size_t word = 0;

    while (word < trimmed && s[i + word] != ' ') {
      word++;
    }

    fit = letters_fitting(f, s + i, word, st, f->width - f->x, &w);
    piece(f, r, s + i, fit, st, w);
    i += fit;

    if (fit < word) {
      new_line(f);
    }
  }
}

// a name on a ground, a mention or inline code, kept in one piece.
static void pill(Flow *f, Run r, const char *s, civue_color ground) {
  r.pill = true;
  r.ground = ground;

  civue_text_style st = style_of(f, &r);
  size_t n = strlen(s);
  float w = measure(f, s, n, st);

  if (f->x > f->indent && f->x + w + 4 > f->width) {
    new_line(f);
  }

  f->x += 2;
  piece(f, &r, s, n, st, w);
  f->x += 2;
}

static void emoji(Flow *f, const Run *r, const char *s, size_t n) {
  Run plain = *r;
  civue_text_style st = style_of(f, &plain);

  st.emoji = true;
  st.size = f->jumbo ? 44 : f->size * 1.25f;

  piece(f, &plain, s, n, st, measure(f, s, n, st));
}

static void custom_emoji(Flow *f, const Run *r, const MdNode *node) {
  float side = f->jumbo ? 48 : f->size * 1.375f;

  if (f->x > f->indent && f->x + side > f->width) {
    new_line(f);
  }

  if (f->paint && !r->hidden) {
    char url[CDN_URL_MAX];
    uint32_t pixels = (uint32_t)(side * f->ui->scale);

    cdn_emoji(url, node->id, pixels);

    civue_image_info image =
      civue_images_get(f->app->images, f->ui, url, pixels);
    civue_rect at = xywh(f->origin.x + f->x,
                         f->origin.y + f->y + (f->line - side) / 2, side, side);

    if (image.state == CIVUE_IMAGE_READY) {
      civue_image(f->ui, image.texture, at, (float[4]){0, 0, 1, 1}, 1, 0,
                  false);
    }
    else {
      fill(civue_rect_inflate(at, -2, -2), f->app->look.hover, 4);
    }
  }

  Run blank = *r;

  blank.pill = false;
  piece(f, &blank, nullptr, 0, style_of(f, r), side);
}

static void code_block(Flow *f, const MdNode *node) {
  const Look *k = &f->app->look;
  civue_text_style st = civue_style(14, 400, 18, 0);
  float pad = 8;
  float room = f->width - f->indent - pad * 2;

  st.mono = true;
  end_block(f);

  const civue_text_block *b = civue_layout_n(
    f->ui, node->text ? node->text : "", node->len, st, fmaxf(room, 40));
  float h = fmaxf(b->height, st.line_height) + pad * 2;

  if (f->y > 0) {
    f->y += 4;
  }

  if (f->paint) {
    civue_rect at = xywh(f->origin.x + f->indent, f->origin.y + f->y,
                         f->width - f->indent, h);

    civue_fill_stroke(f->ui, at, k->frame, k->border, 1, 8);
    civue_draw_text(f->ui, b, point(at.x0 + pad, at.y0 + pad), rgb(0xe4e4e6));
  }

  f->y += h + 4;
}

static void inlines(Flow *f, const MdNode *first, Run r);
static void blocks(Flow *f, const MdNode *first, const Run *r);

static void mention(Flow *f, const Run *r, const MdNode *node) {
  const Look *k = &f->app->look;
  Store *store = f->app->session->store;
  char name[160];
  Run run = *r;

  run.color = k->mention_text;

  switch (node->kind) {
    case MD_MENTION_USER:
      snprintf(name, sizeof name, "@%s",
               store_display_name(store, f->o->guild_id, node->id));
      break;
    case MD_MENTION_ROLE: {
      const Guild *guild = store_guild(store, f->o->guild_id);
      const Role *role = guild ? store_role(guild, node->id) : nullptr;

      snprintf(name, sizeof name, "@%s",
               role && role->name ? role->name : "deleted-role");

      if (role && role->color) {
        run.color = rgb(role->color);
        pill(f, run, name, civue_alpha(run.color, .12f));
        return;
      }

      break;
    }
    case MD_MENTION_CHANNEL: {
      const Channel *channel = store_channel(store, node->id);
      char label[128];

      if (channel) {
        snprintf(name, sizeof name, "#%s",
                 store_channel_name(store, channel, label, sizeof label));
        run.channel_id = channel->id;
      }
      else {
        snprintf(name, sizeof name, "#unknown");
      }

      break;
    }
    case MD_MENTION_EVERYONE:
      snprintf(name, sizeof name, "@everyone");
      break;
    default:
      snprintf(name, sizeof name, "@here");
      break;
  }

  pill(f, run, name, k->mention_bg);
}

static void one_inline(Flow *f, const MdNode *node, Run r) {
  const Look *k = &f->app->look;

  switch (node->kind) {
    case MD_TEXT:
      words(f, &r, node->text ? node->text : "", node->len);
      break;
    case MD_LINE_BREAK:
      new_line(f);
      break;
    case MD_BOLD:
      r.bold = true;
      inlines(f, node->first, r);
      break;
    case MD_ITALIC:
      r.italic = true;
      inlines(f, node->first, r);
      break;
    case MD_UNDERLINE:
      r.underline = true;
      inlines(f, node->first, r);
      break;
    case MD_STRIKETHROUGH:
      r.strike = true;
      inlines(f, node->first, r);
      break;
    case MD_SPOILER:
      r.spoiler = id_at(f->key, 0x800000 + f->spoilers++);

      if (revealed(f->app, r.spoiler)) {
        r.pill = true;
        r.ground = k->pressed;
      }
      else {
        r.hidden = true;
      }

      inlines(f, node->first, r);
      break;
    case MD_CODE:
      r.code = true;
      pill(f, r, node->text ? node->text : "", k->code_bg);
      break;
    case MD_LINK:
      r.url = node->text;
      r.color = k->link;

      if (node->first) {
        inlines(f, node->first, r);
      }
      else {
        words(f, &r, node->text ? node->text : "", node->len);
      }

      break;
    case MD_MENTION_USER:
    case MD_MENTION_ROLE:
    case MD_MENTION_CHANNEL:
    case MD_MENTION_EVERYONE:
    case MD_MENTION_HERE:
      mention(f, &r, node);
      break;
    case MD_EMOJI:
      emoji(f, &r, node->text ? node->text : "", node->len);
      break;
    case MD_CUSTOM_EMOJI:
      custom_emoji(f, &r, node);
      break;
    case MD_TIMESTAMP:
      pill(f, r, node->text ? node->text : "", k->pressed);
      break;
    case MD_CODE_BLOCK:
      code_block(f, node);
      break;
    default:
      // a block where inline nodes are expected is still laid out.
      end_block(f);
      blocks(f, node, &r);
      break;
  }
}

static void inlines(Flow *f, const MdNode *first, Run r) {
  for (const MdNode *node = first; node; node = node->next) {
    one_inline(f, node, r);
  }
}

static void heading(Flow *f, const MdNode *node, const Run *r) {
  static const float sizes[] = {24, 24, 20, 16};
  float size = f->size, line = f->line, weight = f->weight;

  if (f->y > 0) {
    f->y += 8;
  }

  f->size = sizes[node->level < 4 ? node->level : 3];
  f->line = roundf(f->size * 1.375f);
  f->weight = 700;

  inlines(f, node->first, *r);
  end_block(f);

  f->y += 4;
  f->size = size, f->line = line, f->weight = weight;
}

static void subtext(Flow *f, const MdNode *node, const Run *r) {
  float size = f->size, line = f->line;
  Run run = *r;

  run.color = f->app->look.muted;
  f->size = 13;
  f->line = 18;

  inlines(f, node->first, run);
  end_block(f);

  f->size = size, f->line = line;
}

static void quote(Flow *f, const MdNode *node, const Run *r) {
  float top = f->y, indent = f->indent;

  f->indent += 16;
  f->x = f->indent;
  blocks(f, node->first, r);
  f->indent = indent;
  f->x = indent;

  if (f->paint) {
    fill(xywh(f->origin.x + indent, f->origin.y + top, 4, f->y - top),
         rgb(0x4f505a), 2);
  }
}

static void list(Flow *f, const MdNode *node, const Run *r) {
  float indent = f->indent;
  float gutter = node->ordered ? 28 : 22;
  uint32_t number = node->start;

  for (const MdNode *item = node->first; item; item = item->next) {
    char marker[16];

    if (node->ordered) {
      snprintf(marker, sizeof marker, "%u.", number++);
    }
    else {
      snprintf(marker, sizeof marker, "•");
    }

    civue_text_style st = style_of(f, r);
    float w = measure(f, marker, strlen(marker), st);

    f->x = indent + fmaxf(gutter - w - 8, 0);
    piece(f, r, marker, strlen(marker), st, w);
    f->indent = indent + gutter;
    f->x = f->indent;

    for (const MdNode *child = item->first; child; child = child->next) {
      if (child->kind == MD_LIST) {
        end_block(f);
        list(f, child, r);
      }
      else {
        one_inline(f, child, *r);
      }
    }

    end_block(f);
    f->indent = indent;
    f->x = indent;
  }
}

static void edited_mark(Flow *f) {
  Run run = {.color = f->app->look.muted};
  civue_text_style st = civue_style(10, 400, f->line, 0);
  const char *mark = "(edited)";

  if (f->dirty) {
    f->x += 4;
  }

  piece(f, &run, mark, strlen(mark), st, measure(f, mark, strlen(mark), st));
}

static void blocks(Flow *f, const MdNode *first, const Run *r) {
  for (const MdNode *node = first; node; node = node->next) {
    bool last = !node->next && f->o->edited && f->indent == 0;

    switch (node->kind) {
      case MD_HEADING:
        heading(f, node, r);
        break;
      case MD_SUBTEXT:
        subtext(f, node, r);
        break;
      case MD_QUOTE:
        quote(f, node, r);
        break;
      case MD_LIST:
        list(f, node, r);
        break;
      case MD_CODE_BLOCK:
        code_block(f, node);
        break;
      case MD_PARAGRAPH:
        inlines(f, node->first, *r);

        if (last) {
          edited_mark(f);
          last = false;
        }

        end_block(f);
        break;
      default:
        one_inline(f, node, *r);
        end_block(f);
        break;
    }

    if (last) {
      edited_mark(f);
      end_block(f);
    }
  }
}

static float flow(App *app, civue_id key, const MdDoc *doc,
                  const MarkupOptions *o, float width, bool paint,
                  civue_vec2 origin) {
  float size = o->size > 0 ? o->size : 16;
  Flow f = {
    .app = app,
    .ui = civue_ui_current(),
    .o = o,
    .key = key,
    .paint = paint,
    .origin = origin,
    .width = width,
    .size = size,
    .line = roundf(size * 1.375f),
    .weight = 400,
    .jumbo = doc->jumbo && !o->inline_only,
  };
  Run run = {.color = o->color.a > 0 ? o->color : app->look.text};

  if (f.jumbo) {
    f.line = 56;
  }

  blocks(&f, doc->root ? doc->root->first : nullptr, &run);
  end_block(&f);

  return fmaxf(f.y, f.line);
}

void markup(App *app, civue_id key, const char *source,
            const MarkupOptions *o) {
  civue_ui *ui = civue_ui_current();
  const Store *store = app->session->store;
  const Guild *guild = o->guild_id ? store_guild(store, o->guild_id) : nullptr;
  size_t len = strlen(source);
  Kept *kept = civue_memory(ui, id_in(key, "markup"), sizeof *kept, nullptr);
  float width = (float)last_size(key).x;

  if (width <= 0) {
    width = o->hint > 0 ? o->hint : 400;
  }

  // names and colors of mentions change what is drawn too.
  uint64_t inputs[] = {
    civue_hash(source, len),
    store->users_version,
    guild ? guild->members_version : 0,
    app->revealed.len,
    (uint64_t)(o->size * 16) | (uint64_t)o->edited << 32 |
      (uint64_t)o->inline_only << 33,
    (uint64_t)(ui->scale * 64),
  };
  uint64_t hash = civue_hash(inputs, sizeof inputs) | 1;
  MdOptions options = {.inline_only = o->inline_only};
  MdDoc *doc = nullptr;

  if (kept->w != width || kept->hash != hash) {
    doc = md_parse(source, len, &options);
    kept->h = flow(app, key, doc, o, width, false, point(0, 0));
    kept->w = width;
    kept->hash = hash;
  }

  div(key(key), w(full), h(px(kept->h)), flex_shrink_0) {
    civue_rect at = here();
    civue_rect seen = civue_rect_intersect(at, civue_clip_rect(ui));

    // what the clip hides is not walked again.
    if (!civue_rect_empty(at) && !civue_rect_empty(seen)) {
      if (!doc) {
        doc = md_parse(source, len, &options);
      }

      flow(app, key, doc, o, width, true, point(at.x0, at.y0));
    }
  }

  if (doc) {
    md_free(doc);
  }
}
