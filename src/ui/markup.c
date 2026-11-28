#include "components.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/emoji.h"
#include "discord/expressions.h"
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
  Snowflake channel_id, user_id;
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
  // what it reads as: the bytes walked so far and the last of them, and the
  // ones lit in `lit`.
  uint32_t at;
  char last;
  uint32_t lit_from, lit_to;
  civue_color lit;
  // the line the last ground was put on, and where it ended.
  float lit_line;
  double lit_end;
  // the byte nearest to `pointer` so far, looked for while `seek`.
  bool seek;
  civue_vec2 pointer;
  uint32_t found;
  // it is the end of the last piece of the line at `found_y`.
  bool tail;
  float found_y;
  // a link, a mention or an emoji of the words took a press.
  bool pressed;
  // the bytes from `from` to `to` copied there when it is set.
  char **out;
  size_t *len;
  uint32_t from, to;
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

static void emit(Flow *f, const char *s, size_t n) {
  if (!n) {
    return;
  }

  uint32_t lo = f->at > f->from ? f->at : f->from;
  uint32_t end = f->at + (uint32_t)n;
  uint32_t hi = end < f->to ? end : f->to;

  if (f->out && lo < hi) {
    *f->out = realloc(*f->out, *f->len + (hi - lo) + 1);
    memcpy(*f->out + *f->len, s + (lo - f->at), hi - lo);
    *f->len += hi - lo;
    (*f->out)[*f->len] = 0;
  }

  f->last = s[n - 1];
  f->at = end;
}

// whether the bytes `from` to `from + n` are lit, and which of them.
static bool lit_part(const Flow *f, uint32_t from, size_t n, uint32_t *lo,
                     uint32_t *hi) {
  uint32_t end = from + (uint32_t)n;

  *lo = from > f->lit_from ? from : f->lit_from;
  *hi = end < f->lit_to ? end : f->lit_to;
  return *lo < *hi;
}

// a ground of what is lit, on whole pixels: two that touch leave no seam.
static void lit_fill(Flow *f, double x0, double y0, double x1, double y1) {
  double s = f->ui->scale;
  civue_rect at = {round(x0 * s) / s, round(y0 * s) / s, round(x1 * s) / s,
                   round(y1 * s) / s};

  if (at.x1 > at.x0) {
    fill(at, f->lit, 0);
  }
}

// the break that ends a line shows lit as a space past it, as chromium's
// does.
static void light_break(Flow *f) {
  uint32_t lo, hi;

  if (!f->paint || !lit_part(f, f->at, 1, &lo, &hi)) {
    return;
  }

  double from = f->origin.x + f->x, top = f->origin.y + f->y;
  double to = from + space_of(f, style_of(f, &(Run){}));

  lit_fill(f, f->lit_line == f->y ? f->lit_end : from, top, to,
           top + f->line);
}

static void end_block(Flow *f) {
  bool breaks = f->at && f->last != '\n';

  if (breaks && f->dirty) {
    light_break(f);
  }

  if (f->dirty) {
    new_line(f);
  }

  if (breaks) {
    emit(f, "\n", 1);
  }

  f->x = f->indent;
}

// the ground of what is lit of a piece, from where the one before it on the
// line ended, pieces being a hair apart and what is lit having no hole. a
// piece that reads as other than what it draws is lit whole.
static void light(Flow *f, const char *s, size_t n, civue_text_style st,
                  float w, size_t reads) {
  uint32_t lo, hi, line;

  if (!lit_part(f, f->at, reads, &lo, &hi)) {
    return;
  }

  float x0 = 0, x1 = w;

  if (s && n && n == reads) {
    const civue_text_block *b = civue_layout_n(f->ui, s, n, st, 0);

    x0 = civue_text_caret_x(b, lo - f->at, &line);
    x1 = hi - f->at < n ? civue_text_caret_x(b, hi - f->at, &line) : w;
  }

  double from = f->origin.x + f->x + x0, to = f->origin.x + f->x + x1;
  double top = f->origin.y + f->y;

  if (f->lit_line == f->y) {
    from = f->lit_end;
  }

  lit_fill(f, from, top, to, top + f->line);
  f->lit_line = f->y;
  f->lit_end = to;
}

// a piece's part in finding the byte under the pointer: the lines above it
// give their ends, the one it is on the byte nearest.
static void seek(Flow *f, const char *s, size_t n, civue_text_style st,
                 float w, size_t reads) {
  float x = (float)(f->pointer.x - f->origin.x);
  float y = (float)(f->pointer.y - f->origin.y);

  if (y < f->y) {
    return;
  }

  if (y >= f->y + f->line || x >= f->x + w) {
    f->found = f->at + (uint32_t)reads;
  }
  else if (x < f->x) {
    // left of a line, its start. a piece right of the pointer has no say.
    if (f->dirty) {
      return;
    }

    f->found = f->at;
  }
  else if (s && n && n == reads) {
    const civue_text_block *b = civue_layout_n(f->ui, s, n, st, 0);

    f->found = f->at + civue_text_hit(b, point(x - f->x, 0));
  }
  else {
    f->found = f->at + (x < f->x + w / 2 ? 0 : (uint32_t)reads);
  }

  f->tail = f->found == f->at + reads;
  f->found_y = f->y;
}

// a press on what answers one inside the words starts a selection as one on
// the words does. true when it was a click, with nothing selected between
// its ends.
static bool piece_clicked(Flow *f, const civue_response *resp) {
  f->pressed = f->pressed || resp->pressed;
  return resp->clicked &&
         (resp->pressed || !f->o->select || !selection_on(f->app));
}

// what a piece does under the pointer: a spoiler shows, a link opens, a
// channel mention goes to its channel and a user's opens their profile.
// true while it is hovered.
static bool interact_piece(Flow *f, const Run *r, civue_rect at) {
  if (!r->hidden && !r->url && !r->channel_id && !r->user_id) {
    return false;
  }

  civue_id id = id_at(f->key, 0x1000 + f->pieces);
  civue_response resp = civue_interact(f->ui, id, at, CIVUE_SENSE_CLICK);
  bool clicked = piece_clicked(f, &resp);

  if (resp.hovered) {
    civue_set_cursor(f->ui, CIVUE_CURSOR_POINTER);
  }

  if (resp.right_clicked && r->url && !r->hidden) {
    str_set(&f->app->context_link, r->url);
  }

  if (r->hidden) {
    if (clicked) {
      app_reveal(f->app, r->spoiler);
      civue_request_repaint(f->ui);
    }

    return false;
  }

  if (clicked && r->url) {
    civue_open_url(r->url);
  }
  else if (clicked && r->user_id) {
    profile_open(f->app, r->user_id, f->o->guild_id, at, PROFILE_RIGHT);
  }
  else if (clicked) {
    app_select_channel(f->app, r->channel_id);
  }

  return resp.hovered;
}

static void paint_piece(Flow *f, const Run *r, const char *s, size_t n,
                        civue_text_style st, float w, size_t reads) {
  const Look *k = &f->app->look;
  double x = f->origin.x + f->x;
  double y = f->origin.y + f->y;
  civue_rect at = xywh(x, y, w, f->line);
  bool hovered = interact_piece(f, r, at);

  if (r->hidden) {
    fill(civue_rect_inflate(at, 0, -1), k->spoiler, 3);
    light(f, nullptr, 0, st, w, reads);
    return;
  }

  // a mention lights up as discord's do.
  bool lit = hovered && r->pill && (r->user_id || r->channel_id);
  civue_color ink = lit ? k->on_primary : r->color;

  if (r->pill) {
    fill(civue_rect_inflate(at, 2, -1), lit ? k->primary : r->ground, 4);
  }

  // on the ground of a mention under the pointer, what is selected shows in
  // the ink of its name.
  civue_color selected = f->lit;

  if (lit) {
    f->lit = civue_alpha(ink, .3f);
  }

  light(f, s, n, st, w, reads);
  f->lit = selected;

  if (n) {
    const civue_text_block *b = civue_layout_n(f->ui, s, n, st, 0);

    civue_draw_text(f->ui, b, point(x, y), ink);
  }

  double middle = y + f->line / 2;

  // a link is underlined as shadcn's are, faintly until the pointer is over
  // it.
  if (r->underline || r->url) {
    float seen = r->underline || hovered ? 1 : .4f;

    civue_hline(f->ui, x, x + w, middle + st.size * .5,
                civue_alpha(r->color, seen));
  }

  if (r->strike) {
    civue_hline(f->ui, x, x + w, middle + 1, r->color);
  }
}

// one piece that is never cut, moved to the next line when it does not fit,
// reading as `reads`.
static void span(Flow *f, const Run *r, const char *s, size_t n,
                 civue_text_style st, float w, const char *reads,
                 size_t nreads) {
  if (f->x > f->indent && f->x + w > f->width + .5f) {
    new_line(f);
  }

  if (f->seek) {
    // what was found on this line is not its end, this piece follows it.
    f->tail = f->tail && f->found_y != f->y;
    seek(f, s, n, st, w, nreads);
  }

  if (f->paint) {
    paint_piece(f, r, s, n, st, w, nreads);
  }

  emit(f, reads, nreads);
  f->pieces++;
  f->x += w;
  f->dirty = true;
}

static void piece(Flow *f, const Run *r, const char *s, size_t n,
                  civue_text_style st, float w) {
  span(f, r, s, n, st, w, s, n);
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
      // a line never starts with the space it was cut at, which still reads.
      if (f->dirty) {
        span(f, r, nullptr, 0, st, space, s + i, 1);
      }
      else {
        emit(f, s + i, 1);
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

// the twemoji of an emoji of unicode, false when it has none.
static bool twemoji(Flow *f, const Run *r, const char *s, size_t n) {
  const UnicodeEmoji *known = emoji_by_utf8(s);
  char url[EMOJI_URL_MAX];
  size_t len;

  if (known == nullptr) {
    return false;
  }

  emoji_url(url, known);

  if (emoji_svg(url, &len) == nullptr) {
    return false;
  }

  float side = f->jumbo ? 48 : f->size * 1.375f;

  if (f->x > f->indent && f->x + side > f->width) {
    new_line(f);
  }

  if (f->paint && !r->hidden) {
    civue_rect at = xywh(f->origin.x + f->x,
                         f->origin.y + f->y + (f->line - side) / 2, side, side);
    uint32_t pixels = (uint32_t)(side * f->ui->scale);
    civue_image_info image =
      civue_images_get(f->app->images, f->ui, url, pixels);

    if (image.state == CIVUE_IMAGE_READY) {
      civue_image(f->ui, image.texture, at, (float[4]){0, 0, 1, 1}, 1, 0,
                  false);
    }

    civue_id id = id_at(f->key, 0x400000 + f->pieces);
    civue_response resp = civue_interact(f->ui, id, at, CIVUE_SENSE_CLICK);
    char name[80];

    snprintf(name, sizeof name, ":%s:", known->name);

    if (resp.hovered) {
      civue_set_cursor(f->ui, CIVUE_CURSOR_POINTER);
    }

    if (f->app->popover != POPOVER_EMOJI_CARD) {
      hint(f->app, id, &resp, name, CIVUE_TOOLTIP_ABOVE);
    }

    if (piece_clicked(f, &resp)) {
      emoji_card_open(f->app, 0, known->utf8, false, false, at);
    }
  }

  Run blank = *r;

  blank.pill = false;
  span(f, &blank, nullptr, 0, style_of(f, r), side, s, n);
  return true;
}

static void emoji(Flow *f, const Run *r, const char *s, size_t n) {
  if (twemoji(f, r, s, n)) {
    return;
  }

  Run plain = *r;
  civue_text_style st = style_of(f, &plain);

  st.emoji = true;
  st.size = f->jumbo ? 44 : f->size * 1.25f;

  piece(f, &plain, s, n, st, measure(f, s, n, st));
}

static void custom_emoji(Flow *f, const Run *r, const MdNode *node) {
  float side = f->jumbo ? 48 : f->size * 1.375f;
  char name[80];

  snprintf(name, sizeof name, ":%s:", node->text ? node->text : "");

  if (f->x > f->indent && f->x + side > f->width) {
    new_line(f);
  }

  if (f->paint && !r->hidden) {
    civue_rect at = xywh(f->origin.x + f->x,
                         f->origin.y + f->y + (f->line - side) / 2, side, side);
    civue_id id = id_at(f->key, 0x400000 + f->pieces);
    civue_response resp = civue_interact(f->ui, id, at, CIVUE_SENSE_CLICK);

    if (!emoji_image(f->app, node->id, node->animated, at)) {
      fill(civue_rect_inflate(at, -2, -2), f->app->look.hover, 4);
    }

    if (resp.hovered) {
      civue_set_cursor(f->ui, CIVUE_CURSOR_POINTER);
    }

    // its name over it, and where it comes from once pressed.
    if (f->app->popover != POPOVER_EMOJI_CARD) {
      hint(f->app, id, &resp, name, CIVUE_TOOLTIP_ABOVE);
    }

    if (piece_clicked(f, &resp)) {
      emoji_card_open(f->app, node->id, node->text, node->animated, node->fake,
                      at);
    }
  }

  Run blank = *r;

  blank.pill = false;
  span(f, &blank, nullptr, 0, style_of(f, r), side, name, strlen(name));
}

// the grounds of what is lit of a block of lines at `origin`, its bytes
// starting at `from`.
static void light_lines(Flow *f, const civue_text_block *b, civue_vec2 origin,
                        uint32_t from, size_t n) {
  uint32_t lo, hi;

  if (!lit_part(f, from, n, &lo, &hi) || !b->nlines) {
    return;
  }

  float tall = b->height / (float)b->nlines;

  lo -= from, hi -= from;

  for (uint32_t l = 0; l < b->nlines; l++) {
    const civue_text_line *line = &b->lines[l];
    uint32_t s = lo > line->start ? lo : line->start;
    uint32_t t = hi < line->end ? hi : line->end;
    uint32_t sl, tl;

    if (s >= t) {
      continue;
    }

    float x0 = civue_text_caret_x(b, s, &sl);
    float x1 = civue_text_caret_x(b, t, &tl);

    x0 = sl == l ? x0 : 0;
    x1 = tl == l ? x1 : b->width;

    double top = origin.y + line->top;

    lit_fill(f, origin.x + x0, top, origin.x + x1, top + tall);
  }
}

static void code_block(Flow *f, const MdNode *node) {
  const Look *k = &f->app->look;
  civue_text_style st = civue_style(14, 400, 18, 0);
  float pad = 8;
  float room = f->width - f->indent - pad * 2;
  const char *text = node->text ? node->text : "";

  st.mono = true;
  end_block(f);

  const civue_text_block *b =
    civue_layout_n(f->ui, text, node->len, st, fmaxf(room, 40));
  float h = fmaxf(b->height, st.line_height) + pad * 2;

  if (f->y > 0) {
    f->y += 4;
  }

  civue_vec2 inside = point(f->origin.x + f->indent + pad,
                            f->origin.y + f->y + pad);

  if (f->seek && f->pointer.y >= f->origin.y + f->y) {
    f->found = f->pointer.y >= f->origin.y + f->y + h
                 ? f->at + node->len
                 : f->at + civue_text_hit(b, point(f->pointer.x - inside.x,
                                                   f->pointer.y - inside.y));
    f->tail = false;
  }

  if (f->paint) {
    civue_rect at = xywh(f->origin.x + f->indent, f->origin.y + f->y,
                         f->width - f->indent, h);

    civue_fill_stroke(f->ui, at, k->frame, k->border, 1, 8);
    light_lines(f, b, inside, f->at, node->len);
    civue_draw_text(f->ui, b, inside, k->text);
  }

  emit(f, text, node->len);
  emit(f, "\n", 1);
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
      run.user_id = node->id;
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
      light_break(f);
      emit(f, "\n", 1);
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

      if (app_revealed(f->app, r.spoiler)) {
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
  const Look *k = &f->app->look;
  float top = f->y, indent = f->indent;

  f->indent += 16;
  f->x = f->indent;
  blocks(f, node->first, r);
  f->indent = indent;
  f->x = indent;

  if (f->paint) {
    fill(xywh(f->origin.x + indent, f->origin.y + top, 4, f->y - top),
         k->rule, 2);
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
    emit(f, " ", 1);
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

  span(f, &run, mark, strlen(mark), st, measure(f, mark, strlen(mark), st),
       nullptr, 0);
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

static Flow flow_of(App *app, civue_id key, const MarkupOptions *o,
                    float width) {
  float size = o->size > 0 ? o->size : 16;

  return (Flow){
    .app = app,
    .ui = civue_ui_current(),
    .o = o,
    .key = key,
    .width = width,
    .size = size,
    .line = roundf(size * 1.375f),
    .weight = 400,
    .lit_line = -1,
  };
}

// lays out, paints, seeks or copies the doc as `f` asks, its height.
static float walk(Flow *f, const MdDoc *doc) {
  Run run = {.color = f->o->color.a > 0 ? f->o->color : f->app->look.text};

  f->jumbo = doc->jumbo && !f->o->inline_only;

  if (f->jumbo) {
    f->line = 56;
  }

  blocks(f, doc->root ? doc->root->first : nullptr, &run);
  end_block(f);

  return fmaxf(f->y, f->line);
}

// FakeNitro's transformAst: a message that is one link to the picture of an
// emoji shows the emoji, under the name its guild gives it when the user is
// in it, then the one the url carries.
static MdDoc *parse(App *app, const char *source, size_t len,
                    const MdOptions *options, const MarkupOptions *o) {
  MdDoc *doc = md_parse(source, len, options);
  MdNode *block = doc->root ? doc->root->first : nullptr;
  bool alone = block && !block->next && block->kind == MD_PARAGRAPH;
  MdNode *link = alone ? block->first : nullptr;
  const char *name = nullptr;
  size_t name_len = 0;
  Snowflake id = 0;
  bool animated = false;

  if (!o->fake_emojis || !link || link->next || link->kind != MD_LINK ||
      !link->text ||
      !fake_emoji_url(link->text, &id, &animated, &name, &name_len)) {
    return doc;
  }

  Store *store = app->session->store;
  const Guild *owner = guild_emoji_owner(store, id);
  const Emoji *known = owner ? store_emoji(owner, id) : nullptr;

  if (known && known->name) {
    name = known->name;
    name_len = strlen(name);
  }
  else if (!name || !name_len) {
    name = "FakeNitroEmoji";
    name_len = strlen(name);
  }

  link->kind = MD_CUSTOM_EMOJI;
  link->id = id;
  link->animated = animated;
  link->fake = true;
  link->first = link->last = nullptr;
  link->text = cilo_strndup((cilo_allocator *)&doc->arena, name, name_len);
  link->len = (uint32_t)name_len;
  return doc;
}

/* ========================================================================== */
/* the selection, from the place a press went down on to where the pointer   */
/* is, messages ordered by their ids.                                         */
/* ========================================================================== */

static bool before(TextPlace a, TextPlace b) {
  return a.message < b.message || (a.message == b.message && a.at < b.at);
}

static void ends(const TextSelection *s, TextPlace *first, TextPlace *last) {
  bool back = before(s->focus, s->anchor);

  *first = back ? s->focus : s->anchor;
  *last = back ? s->anchor : s->focus;
}

bool selection_on(const App *app) {
  const TextSelection *s = &app->selection;

  return s->channel_id && (s->anchor.message != s->focus.message ||
                           s->anchor.at != s->focus.at);
}

bool selection_covers(const App *app, Snowflake message) {
  TextPlace first, last;

  if (!message || !selection_on(app)) {
    return false;
  }

  ends(&app->selection, &first, &last);
  return message >= first.message && message <= last.message;
}

bool selection_range(const App *app, Snowflake message, uint32_t *from,
                     uint32_t *to) {
  TextPlace first, last;

  if (!selection_covers(app, message)) {
    return false;
  }

  ends(&app->selection, &first, &last);
  *from = message == first.message ? first.at : 0;
  *to = message == last.message ? last.at : UINT32_MAX;
  return *from < *to;
}

void selection_clear(App *app) {
  uint64_t version = app->selection.version;

  app->selection = (TextSelection){.version = version + 1};
}

// a press with shift held moves an end of the selection to `o`, by the
// unit it was made with: the nearer one on a mac, where a selection has no
// direction, and the focus elsewhere, as chromium does.
static void extend(TextSelection *s, const TextOffer *o) {
  bool back = before(o->at, s->anchor);
  TextPlace keep = s->anchor;

#ifdef __APPLE__
  TextPlace first, last;

  ends(s, &first, &last);

  bool nearer = first.message == last.message
                  ? o->at.at - first.at <= last.at - o->at.at
                  : o->at.message == first.message;

  back = before(o->at, first) || (!before(last, o->at) && nearer);
  keep = back ? last : first;
#endif

  s->anchor = keep;
  s->focus = (TextPlace){o->at.message, back ? o->from : o->to};
  s->base = keep;
  s->base_from = s->base_to = keep.at;
}

void selection_step(App *app, Snowflake channel, bool pressed) {
  civue_ui *ui = civue_ui_current();
  TextSelection *s = &app->selection;
  TextOffer o = s->offer;
  TextPlace anchor = s->anchor, focus = s->focus;

  s->offer = (TextOffer){};

  if (o.pressed && o.extends) {
    extend(s, &o);
    s->dragging = true;
  }
  else if (o.pressed) {
    s->channel_id = channel;
    s->unit = o.unit;
    s->base = o.at;
    s->base_from = o.from;
    s->base_to = o.to;
    s->anchor = (TextPlace){o.at.message, o.from};
    s->focus = (TextPlace){o.at.message, o.to};
    s->dragging = true;
  }
  else if (pressed && s->channel_id) {
    selection_clear(app);
  }
  else if (s->dragging && o.found) {
    // from the unit pressed to the one under the pointer, both whole.
    bool back = before(o.at, s->base);

    s->anchor =
      (TextPlace){s->base.message, back ? s->base_to : s->base_from};
    s->focus = (TextPlace){o.at.message, back ? o.from : o.to};
  }

  if (s->dragging && !ui->input.down[0]) {
    s->dragging = false;
  }

  // a press held on a link keeps its pointer until it selects.
  if (s->dragging && selection_on(app)) {
    civue_set_cursor(ui, CIVUE_CURSOR_TEXT);
  }

  if (anchor.message != s->anchor.message || anchor.at != s->anchor.at ||
      focus.message != s->focus.message || focus.at != s->focus.at) {
    s->version++;
    civue_request_repaint(ui);
  }
}

// what these words say of the pointer, for the list to take once every
// message said its own: the last ones that start above it have it, the first
// ones below it when none does. `text` is what they read as, there when the
// unit is more than a letter.
static void offer(App *app, Snowflake message, const Flow *f, bool above,
                  TextOffer said, const char *text, size_t len) {
  TextOffer *o = &app->selection.offer;

  if (o->pressed || (!said.pressed && !above && o->found)) {
    return;
  }

  uint32_t at = above ? f->found : 0;

  said.found = true;
  said.at = (TextPlace){message, at};
  said.from = said.to = at;

  if (text) {
    civue_text_unit_at(text, len, at, said.unit, above && f->tail,
                       &said.from, &said.to);
  }

  *o = said;
}

void markup_text(App *app, const char *source, const MarkupOptions *o,
                 uint32_t from, uint32_t to, char **out, size_t *len) {
  MdOptions options = {.inline_only = o->inline_only};
  MdDoc *doc = parse(app, source, strlen(source), &options, o);
  Flow f = flow_of(app, 0, o, o->hint > 0 ? o->hint : 400);

  f.out = out;
  f.len = len;
  f.from = from;
  f.to = to;
  walk(&f, doc);
  md_free(doc);
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
    Flow f = flow_of(app, key, o, width);

    doc = parse(app, source, len, &options, o);
    kept->h = walk(&f, doc);
    kept->w = width;
    kept->hash = hash;
  }

  div(key(key), w(full), h(px(kept->h)), flex_shrink_0) {
    civue_rect at = here();
    civue_rect seen = civue_rect_intersect(at, civue_clip_rect(ui));
    const TextSelection *sel = &app->selection;

    // what the clip hides is not walked again.
    if (civue_rect_empty(at) || civue_rect_empty(seen)) {
      continue;
    }

    // the words take the press under what they hold that answers it.
    civue_response press = {};

    if (o->select) {
      press =
        civue_interact(ui, id_in(key, "select"), at, CIVUE_SENSE_DRAG);

      if (press.hovered) {
        civue_set_cursor(ui, CIVUE_CURSOR_TEXT);
      }
    }

    if (!doc) {
      doc = parse(app, source, len, &options, o);
    }

    Flow f = flow_of(app, key, o, width);
    char *text = nullptr;
    size_t text_len = 0;
    bool above = ui->input.has_mouse && ui->input.mouse.y >= at.y0;
    // a press here may land on a link or a mention of the words, which is
    // known once they are walked.
    bool down = o->select && ui->input.has_mouse && ui->input.pressed[0] &&
                civue_rect_contains(seen, ui->input.mouse);

    f.paint = true;
    f.origin = point(at.x0, at.y0);
    f.seek = o->select && (down || sel->dragging);
    f.pointer = ui->input.mouse;

    if (selection_range(app, o->select, &f.lit_from, &f.lit_to)) {
      f.lit = app->look.selection;
    }

    // a press picks the unit, one with shift held keeps the selection's.
    TextOffer said = {.unit = sel->unit};
    bool extends = sel->channel_id && (ui->input.mods & CIVUE_MOD_SHIFT);

    if (down && !extends) {
      said.unit = civue_text_unit_of(ui->input.clicks);
    }

    if (f.seek && said.unit != CIVUE_UNIT_CHAR) {
      f.out = &text;
      f.len = &text_len;
      f.to = UINT32_MAX;
    }

    walk(&f, doc);
    said.pressed = press.pressed || f.pressed;
    said.extends = said.pressed && extends;

    if (f.seek && (said.pressed || sel->dragging)) {
      offer(app, o->select, &f, above, said, text, text_len);
    }

    free(text);
  }

  if (doc) {
    md_free(doc);
  }
}
