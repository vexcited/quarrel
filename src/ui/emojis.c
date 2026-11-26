#include "components.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "complete.h"
#include "discord/cdn.h"
#include "discord/emoji.h"
#include "discord/expressions.h"
#include "discord/markdown.h"
#include "widgets.h"

// the client's metrics: its picker without the tabs, the guilds on its side,
// the search over them, what is pointed at under them, and its cells.
constexpr float RAIL = 48;
constexpr float SEARCH = 40;
constexpr float INSPECTOR = 48;
constexpr float CELL = 48;
constexpr float IMAGE = 40;
constexpr float TITLE = 32;
constexpr float SECTION_GAP = 12;
constexpr float LIST_PAD = 8;
// what is written in the composer is this wide, 1.375em.
constexpr float INLINE = 22;
// the client lists this many of the emojis used the most.
constexpr size_t FREQUENT_MOST = 42;
// the rows left of a guild whose emojis all need nitro.
constexpr uint32_t LOCKED_ROWS = 3;
// built this far out of view, so what scrolls in is ready.
constexpr float AHEAD = 96;
// the client completes with this many, and its rail goes straight to the
// emojis of unicode past this many guilds.
constexpr size_t COMPLETE_MOST = 10;
constexpr size_t SHORTCUT_FROM = 7;

/* ========================================================================== */
/* cells: an emoji as the picker and the searches see one.                    */
/* ========================================================================== */

typedef struct {
  // one of a guild, or one of unicode when null.
  const GuildEmoji *custom;
  const UnicodeEmoji *unicode;
  EmojiAccess access;
  // how well it answers a search.
  float score;
} Cell;

static const char *cell_name(const Cell *cell) {
  return cell->custom ? cell->custom->name : cell->unicode->name;
}

static ReactionPick pick_of(const Cell *cell) {
  if (cell->custom) {
    return (ReactionPick){
      .id = cell->custom->emoji->id,
      .name = cell->custom->emoji->name,
      .animated = cell->custom->emoji->animated,
    };
  }

  return (ReactionPick){.name = cell->unicode->utf8};
}

static Snowflake guild_of(const Channel *c) {
  return c ? c->guild_id : 0;
}

// in a message every emoji goes: the ones discord would refuse are sent as
// a link to their picture, as Vencord's FakeNitro does. a reaction has no
// such way, so discord's own rules hold there.
static EmojiAccess access_for(Store *store, const GuildEmoji *e,
                              const Channel *c, EmojiUse use) {
  return use == EMOJI_FOR_CHAT
           ? EMOJI_USABLE
           : guild_emoji_access(store, e->guild, e->emoji, c);
}

// what a key of the user's settings is where `c` is: the first name of an
// emoji of unicode, or the id of a guild's. a few of unicode are named by
// digits alone, so the names come first.
static bool cell_of_key(App *app, const Channel *c, EmojiUse use,
                        const char *key, Cell *out) {
  Store *store = app->session->store;
  const UnicodeEmoji *unicode = emoji_by_name(key);

  if (unicode) {
    *out = (Cell){.unicode = unicode};
    return true;
  }

  const GuildEmoji *custom =
    guild_emoji_by_id(store, guild_of(c), snowflake_parse(key));

  if (!custom) {
    return false;
  }

  *out = (Cell){
    .custom = custom,
    .access = access_for(store, custom, c, use),
  };

  // the client leaves these out of what is starred and used the most.
  return out->access != EMOJI_HIDDEN && out->access != EMOJI_NEEDS_BOOSTS;
}

bool emoji_pick_by_key(App *app, const Channel *c, const char *key,
                       ReactionPick *out) {
  Cell cell;

  if (!cell_of_key(app, c, EMOJI_FOR_REACTION, key, &cell) ||
      cell.access != EMOJI_USABLE) {
    return false;
  }

  *out = pick_of(&cell);
  return true;
}

// what a pick is written as in a message.
static void pick_text(char *out, size_t cap, ReactionPick pick) {
  if (pick.id) {
    snprintf(out, cap, "<%s:%s:%" PRIu64 ">", pick.animated ? "a" : "",
             pick.name, pick.id);
  }
  else {
    snprintf(out, cap, "%s", pick.name);
  }
}

/* ========================================================================== */
/* searching, as the client's queryEmojiResults.                              */
/* ========================================================================== */

typedef CIVUE_VEC(Cell) Cells;

static void push_cell(Cells *cells, Cell cell) {
  if (cells->len == cells->cap) {
    cells->cap = cells->cap ? cells->cap * 2 : 256;
    cells->v = realloc(cells->v, cells->cap * sizeof *cells->v);
  }

  cells->v[cells->len++] = cell;
}

// in lower case without spaces and underscores, which is what the client
// compares.
static void squeeze(char *out, size_t cap, const char *s) {
  size_t n = 0;

  for (; *s && n + 1 < cap; s++) {
    if (*s != ' ' && *s != '_') {
      out[n++] = (char)tolower((unsigned char)*s);
    }
  }

  out[n] = '\0';
}

static bool holds(const char *name, const char *needle) {
  char squeezed[96];

  squeeze(squeezed, sizeof squeezed, name);
  return strstr(squeezed, needle) != nullptr;
}

// `query` is a word of the name: at its start or after an underscore, up to
// its end or an underscore, a plural s allowed.
static bool whole_word(const char *name, const char *query, size_t len) {
  for (const char *at = strcasestr(name, query); at;
       at = strcasestr(at + 1, query)) {
    const char *end = at + len;

    end += *end == 's';

    if ((at == name || at[-1] == '_') && (*end == '\0' || *end == '_')) {
      return true;
    }
  }

  return false;
}

// the client's: one, four more for the name itself, two for a word of it,
// one for its start, all of it times what the user makes of the emoji.
static float score_of(const Session *s, const char *name, const char *key,
                      const char *query) {
  size_t len = strlen(query);
  float score = 1;

  score += strcasecmp(name, query) == 0 ? 4 : 0;
  score += whole_word(name, query, len) ? 2 : 0;
  score += strncasecmp(name, query, len) == 0 ? 1 : 0;

  return score * fmaxf(1, (float)session_emoji_score(s, key) / 100);
}

static int by_score(const void *a, const void *b) {
  const Cell *x = a, *y = b;
  bool xu = x->access == EMOJI_USABLE, yu = y->access == EMOJI_USABLE;

  if (xu != yu) {
    return xu ? -1 : 1;
  }

  if (x->score != y->score) {
    return x->score < y->score ? 1 : -1;
  }

  return strcmp(cell_name(x), cell_name(y));
}

// the emojis `query` finds where `c` is, the best first, those that need
// nitro after the others when `locked` asks for them.
static void search(App *app, const Channel *c, EmojiUse use, const char *query,
                   bool locked, Cells *out) {
  Session *session = app->session;
  Store *store = session->store;
  char needle[96], key[24];
  size_t count = 0;
  const GuildEmoji *all = guild_emojis(store, guild_of(c), &count);

  squeeze(needle, sizeof needle, query);

  if (!*needle) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    if (!holds(all[i].name, needle)) {
      continue;
    }

    EmojiAccess access = access_for(store, &all[i], c, use);

    if (access == EMOJI_USABLE || (locked && access == EMOJI_NEEDS_PREMIUM)) {
      push_cell(out, (Cell){
                       .custom = &all[i],
                       .access = access,
                       .score = score_of(
                         session, all[i].name,
                         snowflake_format(all[i].emoji->id, key), query),
                     });
    }
  }

  // an emoji found by two of its names shows once.
  bool *found = calloc(UNICODE_EMOJIS_COUNT, sizeof *found);

  for (size_t i = 0; i < UNICODE_EMOJI_NAMES_COUNT; i++) {
    const UnicodeEmojiName *name = &UNICODE_EMOJI_NAMES[i];
    const UnicodeEmoji *emoji = &UNICODE_EMOJIS[name->index];

    if (emoji->toned || found[name->index] || !holds(name->name, needle)) {
      continue;
    }

    found[name->index] = true;
    push_cell(out,
              (Cell){
                .unicode = emoji,
                .score = score_of(session, emoji->name, emoji->name, query),
              });
  }

  free(found);

  if (out->len) {
    qsort(out->v, out->len, sizeof *out->v, by_score);
  }
}

/* ========================================================================== */
/* completing a name after a colon in the composer.                           */
/* ========================================================================== */

static bool blank_byte(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

// the client's: the word the caret ends starts with a colon, or with one
// after a parenthesis, and two characters at least follow it.
int32_t emoji_complete_find(App *, const Channel *, const char *text,
                            uint32_t caret) {
  uint32_t start = caret;

  while (start > 0 && !blank_byte(text[start - 1])) {
    start--;
  }

  start += start < caret && text[start] == '(';

  if (start >= caret || text[start] != ':' || caret - start < 3) {
    return -1;
  }

  // a second colon closes the name: nothing is left to complete.
  for (uint32_t i = start + 1; i < caret; i++) {
    if (text[i] == ':') {
      return -1;
    }
  }

  return (int32_t)start;
}

void emoji_complete_fill(App *app, Channel *c, const char *token,
                         CompleteList *list) {
  Cells cells = {};
  char title[128], label[80], insert[112];
  const char *query = token + 1;

  // what the user writes the most comes first.
  session_fetch_frecency(app->session);
  search(app, c, EMOJI_FOR_CHAT, query, false, &cells);

  // a query that starts with a dash only finds the names that are it, so
  // that :-1: is not lost among the others.
  if (*query == '-') {
    uint32_t kept = 0;

    for (uint32_t i = 0; i < cells.len; i++) {
      if (strcasecmp(cell_name(&cells.v[i]), query) == 0) {
        cells.v[kept++] = cells.v[i];
      }
    }

    cells.len = kept;
  }

  if (cells.len) {
    snprintf(title, sizeof title, "Emoji matching %s", token);
    complete_title(list, title);
  }

  for (uint32_t i = 0; i < cells.len && i < COMPLETE_MOST; i++) {
    const Cell *cell = &cells.v[i];
    ReactionPick pick = pick_of(cell);
    size_t len = 0;

    pick_text(insert, sizeof insert - 1, pick);
    len = strlen(insert);
    insert[len] = ' ';
    insert[len + 1] = '\0';
    snprintf(label, sizeof label, ":%s:", cell_name(cell));

    complete_add(list,
                 &(CompleteRow){
                   .icon = COMPLETE_ICON_EMOJI,
                   .id = pick.id,
                   .name = pick.name,
                   .label = label,
                   .detail = cell->custom ? cell->custom->guild->name : nullptr,
                   .insert = insert,
                 });
  }

  free(cells.v);
}

/* ========================================================================== */
/* what the composer shows for the emojis its text names.                     */
/* ========================================================================== */

static struct {
  App *app;
  Channel *channel;
  // where the field last asked, and where what a message keeps as it is,
  // code or a link, ends: the field asks from the start of its text on.
  uint32_t asked, literal_end;
} inlaid;

// the emoji a name between colons would be sent as from the channel.
static bool colons_cell(App *app, const Channel *c, const char *name,
                        size_t len, uint8_t tone, Cell *out) {
  Store *store = app->session->store;
  const GuildEmoji *custom = guild_emoji_by_name(store, guild_of(c), name, len);

  if (custom) {
    *out = (Cell){.custom = custom};
    return true;
  }

  *out = (Cell){.unicode = emoji_by_colons(name, len, tone)};
  return out->unicode != nullptr;
}

static uint32_t inlay_find(void *, const char *text, uint32_t len, uint32_t at,
                           float *width) {
  uint32_t mention = mention_inlay_find(inlaid.app, text, len, at, width);
  const char *name = nullptr;
  size_t taken = 0, name_len = 0;
  uint8_t tone = 0;
  Cell cell;

  if (mention) {
    return mention;
  }

  if (at == 0 || at < inlaid.asked) {
    inlaid.literal_end = 0;
  }

  inlaid.asked = at;

  if (at < inlaid.literal_end) {
    return 0;
  }

  if (text[at] == '<') {
    taken =
      emoji_token(text + at, len - at, nullptr, nullptr, nullptr, nullptr);
  }
  else if ((taken = expressions_literal(text + at, len - at)) != 0) {
    inlaid.literal_end = at + (uint32_t)taken;
    return 0;
  }
  else if (text[at] == ':') {
    taken = emoji_colons(text + at, len - at, &name, &name_len, &tone);

    if (taken &&
        !colons_cell(inlaid.app, inlaid.channel, name, name_len, tone, &cell)) {
      taken = 0;
    }
  }
  else if ((unsigned char)text[at] >= 0x80 || text[at] == '#' ||
           text[at] == '*' || (text[at] >= '0' && text[at] <= '9')) {
    taken = md_emoji_length(text + at, len - at);
  }

  *width = INLINE;
  return (uint32_t)taken;
}

// an emoji of unicode as messages draw them, in the system's own glyphs.
static void unicode_glyph(civue_ui *ui, const char *utf8, size_t len,
                          civue_rect r) {
  civue_text_style style = civue_style(20, 400, INLINE, 0);

  style.emoji = true;

  const civue_text_block *block = civue_layout_n(ui, utf8, len, style, 0);
  civue_vec2 middle = civue_rect_center(r);

  civue_draw_text(ui, block,
                  point(middle.x - block->width / 2, middle.y - INLINE / 2),
                  CIVUE_WHITE);
}

static void inlay_paint(civue_ui *ui, void *, const char *text, uint32_t len,
                        civue_rect r) {
  App *app = inlaid.app;
  civue_vec2 middle = civue_rect_center(r);
  civue_rect box =
    xywh(middle.x - INLINE / 2, middle.y - INLINE / 2, INLINE, INLINE);
  Snowflake emoji_id = 0;
  bool animated = false;
  const char *name = nullptr;
  size_t name_len = 0;
  uint8_t tone = 0;
  Cell cell = {};

  if (text[0] == '<' && text[1] != ':' && text[1] != 'a') {
    mention_inlay_paint(app, ui, text, len, r);
    return;
  }

  if (emoji_token(text, len, &emoji_id, &animated, nullptr, nullptr)) {
    if (!emoji_image(app, emoji_id, animated, box)) {
      fill(civue_rect_inflate(box, -2, -2), app->look.hover, 4);
    }

    return;
  }

  if (text[0] != ':') {
    unicode_glyph(ui, text, len, r);
    return;
  }

  if (!emoji_colons(text, len, &name, &name_len, &tone) ||
      !colons_cell(app, inlaid.channel, name, name_len, tone, &cell)) {
    return;
  }

  if (cell.custom) {
    emoji_image(app, cell.custom->emoji->id, cell.custom->emoji->animated, box);
  }
  else {
    unicode_glyph(ui, cell.unicode->utf8, strlen(cell.unicode->utf8), r);
  }
}

const civue_field_inlays *composer_inlays(App *app, Channel *c) {
  static const civue_field_inlays inlays = {
    .find = inlay_find,
    .paint = inlay_paint,
  };

  inlaid.app = app;
  inlaid.channel = c;
  return &inlays;
}

/* ========================================================================== */
/* the picker: what it shows, made again when what it is made of changes.     */
/* ========================================================================== */

typedef enum : uint8_t {
  SECTION_RESULTS,
  SECTION_FAVORITES,
  SECTION_FREQUENT,
  SECTION_GUILD,
  SECTION_UNICODE,
} SectionKind;

typedef struct {
  SectionKind kind;
  const Guild *guild;
  uint8_t category;
  // what tells it from the others across frames.
  uint64_t key;
  const char *title;
  // its cells among the picker's.
  uint32_t first, count;
  // every emoji of it needs nitro: it is cut short until asked for whole.
  bool locked, cut, folded;
  // the cells it shows, the one that asks for the others included.
  uint32_t shown;
  float y, height;
} Section;

typedef CIVUE_VEC(Section) Sections;

struct EmojiPicker {
  char *search;
  bool focus;
  // the sections folded, and the ones cut short shown whole.
  Ids folded, unfolded;
  uint32_t folds;
  // the emoji last pointed at.
  Snowflake inspected;
  const UnicodeEmoji *inspected_unicode;

  uint64_t stamp;
  Cells cells;
  Sections sections;
  // the sections the rail leads to, which a search does not take away.
  Sections places;
  uint32_t columns;
  float height;
  // the section at the top of the list last frame.
  uint64_t active;
  // the section the list is to start at, 0 for none.
  uint64_t jump;
};

static const struct {
  const char *title;
  civue_icon glyph;
} CATEGORIES[EMOJI_CATEGORIES] = {
  {"People", CIVUE_ICON(lucide, smile)},
  {"Nature", CIVUE_ICON(lucide, leaf)},
  {"Food", CIVUE_ICON(lucide, utensils)},
  {"Activities", CIVUE_ICON(lucide, gamepad_2)},
  {"Travel", CIVUE_ICON(lucide, bike)},
  {"Objects", CIVUE_ICON(lucide, lightbulb)},
  {"Symbols", CIVUE_ICON(lucide, heart)},
  {"Flags", CIVUE_ICON(lucide, flag)},
};

constexpr uint64_t KEY_RESULTS = 1;
constexpr uint64_t KEY_FAVORITES = 2;
constexpr uint64_t KEY_FREQUENT = 3;
constexpr uint64_t KEY_UNICODE = 16;

static struct EmojiPicker *picker_of(App *app) {
  if (!app->emojis) {
    app->emojis = calloc(1, sizeof *app->emojis);
  }

  return app->emojis;
}

void emoji_picker_free(App *app) {
  struct EmojiPicker *p = app->emojis;

  if (!p) {
    return;
  }

  free(p->search);
  free(p->folded.v);
  free(p->unfolded.v);
  free(p->cells.v);
  free(p->sections.v);
  free(p->places.v);
  free(p);
  app->emojis = nullptr;
}

static Section *add_section(struct EmojiPicker *p, Section section) {
  if (p->sections.len == p->sections.cap) {
    p->sections.cap = p->sections.cap ? p->sections.cap * 2 : 32;
    p->sections.v =
      realloc(p->sections.v, p->sections.cap * sizeof *p->sections.v);
  }

  section.first = p->cells.len;
  p->sections.v[p->sections.len] = section;
  return &p->sections.v[p->sections.len++];
}

// takes the section back when it got no cell.
static void end_section(struct EmojiPicker *p, Section *section) {
  section->count = p->cells.len - section->first;

  if (!section->count) {
    p->sections.len--;
  }
}

static void add_keys(App *app, struct EmojiPicker *p, const Channel *c,
                     EmojiUse use, Section section, char *const *keys,
                     size_t count) {
  Section *s = add_section(p, section);

  for (size_t i = 0; i < count; i++) {
    Cell cell;

    if (cell_of_key(app, c, use, keys[i], &cell)) {
      push_cell(&p->cells, cell);
    }
  }

  end_section(p, s);
}

// the emojis the user gives or writes the most, 42 as the client keeps.
static void add_frequent(App *app, struct EmojiPicker *p, const Channel *c,
                         EmojiUse use) {
  Session *session = app->session;
  char *keys[FREQUENT_MOST];
  size_t count = 0;

  if (use == EMOJI_FOR_REACTION) {
    for (; count < session->nfrecent_reactions && count < FREQUENT_MOST;
         count++) {
      keys[count] = session->frecent_reactions[count];
    }
  }
  else {
    for (; count < session->nfrecent_emojis && count < FREQUENT_MOST; count++) {
      keys[count] = session->frecent_emojis[count].key;
    }
  }

  add_keys(app, p, c, use,
           (Section){.kind = SECTION_FREQUENT,
                     .key = KEY_FREQUENT,
                     .title = "Frequently Used"},
           keys, count);
}

static void add_guilds(App *app, struct EmojiPicker *p, const Channel *c,
                       EmojiUse use) {
  Store *store = app->session->store;
  size_t count = 0;
  const GuildEmoji *all = guild_emojis(store, guild_of(c), &count);

  for (size_t i = 0; i < count;) {
    const Guild *guild = all[i].guild;
    Section *s = add_section(p, (Section){
                                  .kind = SECTION_GUILD,
                                  .guild = guild,
                                  .key = guild->id,
                                  .title = guild->name ? guild->name : "",
                                  .locked = true,
                                });
    size_t from = i;

    // the ones the user may use, then the others.
    for (int pass = 0; pass < 2; pass++) {
      for (i = from; i < count && all[i].guild == guild; i++) {
        EmojiAccess access = access_for(store, &all[i], c, use);

        if (access != EMOJI_HIDDEN && (access == EMOJI_USABLE) == (pass == 0)) {
          s->locked &= access == EMOJI_NEEDS_PREMIUM;
          push_cell(&p->cells, (Cell){.custom = &all[i], .access = access});
        }
      }
    }

    end_section(p, s);
  }
}

static void add_unicode(struct EmojiPicker *p) {
  size_t count = 0;
  const UnicodeEmoji *const *order = emoji_picker_order(&count);

  for (size_t category = 0; category < EMOJI_CATEGORIES; category++) {
    Section *s = add_section(p, (Section){
                                  .kind = SECTION_UNICODE,
                                  .category = (uint8_t)category,
                                  .key = KEY_UNICODE + category,
                                  .title = CATEGORIES[category].title,
                                });

    for (size_t i = EMOJI_CATEGORY_STARTS[category];
         i < EMOJI_CATEGORY_STARTS[category + 1] && i < count; i++) {
      if (order[i]) {
        push_cell(&p->cells, (Cell){.unicode = order[i]});
      }
    }

    end_section(p, s);
  }
}

static void lay(struct EmojiPicker *p) {
  uint32_t columns = p->columns;
  float y = 0;

  for (uint32_t i = 0; i < p->sections.len; i++) {
    Section *s = &p->sections.v[i];
    bool titled = s->kind != SECTION_RESULTS;

    s->folded = titled && ids_has(&p->folded, s->key);
    s->cut = s->locked && s->count > LOCKED_ROWS * columns &&
             !ids_has(&p->unfolded, s->key);
    s->shown = s->folded ? 0 : s->cut ? LOCKED_ROWS * columns : s->count;
    s->y = y;
    s->height = (titled ? TITLE + SECTION_GAP : LIST_PAD) +
                (float)((s->shown + columns - 1) / columns) * CELL;
    y += s->height;
  }

  p->height = y + LIST_PAD;
}

static void build(App *app, struct EmojiPicker *p, const Channel *c,
                  EmojiUse use) {
  Session *session = app->session;
  const char *query = p->search ? p->search : "";

  p->cells.len = 0;
  p->sections.len = 0;

  add_keys(app, p, c, use,
           (Section){.kind = SECTION_FAVORITES,
                     .key = KEY_FAVORITES,
                     .title = "Favorites"},
           session->favorite_emojis, session->nfavorite_emojis);
  add_frequent(app, p, c, use);
  add_guilds(app, p, c, use);
  add_unicode(p);
  lay(p);

  if (p->places.cap < p->sections.len) {
    p->places.cap = p->sections.cap;
    p->places.v = realloc(p->places.v, p->places.cap * sizeof *p->places.v);
  }

  p->places.len = p->sections.len;
  memcpy(p->places.v, p->sections.v, p->sections.len * sizeof *p->places.v);

  // what a search finds takes the place of it all.
  if (*query) {
    p->cells.len = 0;
    p->sections.len = 0;

    Section *s =
      add_section(p, (Section){.kind = SECTION_RESULTS, .key = KEY_RESULTS});

    search(app, c, use, query, true, &p->cells);
    end_section(p, s);
    lay(p);
  }
}

static const Cell *inspected_cell(const struct EmojiPicker *p) {
  for (uint32_t i = 0; i < p->cells.len; i++) {
    const Cell *cell = &p->cells.v[i];

    if (cell->custom ? cell->custom->emoji->id == p->inspected
                     : !p->inspected && cell->unicode == p->inspected_unicode) {
      return cell;
    }
  }

  return p->cells.len ? &p->cells.v[0] : nullptr;
}

/* ========================================================================== */
/* drawing it.                                                                */
/* ========================================================================== */

typedef struct {
  bool picked;
  ReactionPick pick;
} Press;

static void section_glyph(App *app, const Section *s, float size) {
  const Look *k = &app->look;

  switch (s->kind) {
    case SECTION_GUILD:
      div(size(px(size)), flex, items_center, justify_center, flex_shrink_0) {
        guild_icon(app, s->guild, size, false);
      }

      break;
    case SECTION_FAVORITES:
      glyph(CIVUE_ICON(lucide, star), size, k->subtle);
      break;
    case SECTION_FREQUENT:
      glyph(CIVUE_ICON(lucide, clock), size, k->subtle);
      break;
    case SECTION_UNICODE:
      glyph(CATEGORIES[s->category].glyph, size, k->subtle);
      break;
    default:
      break;
  }
}

static void emoji_cell(App *app, struct EmojiPicker *p, civue_id id,
                       const Cell *cell, float x, float y, bool veiled,
                       Press *press) {
  const Look *k = &app->look;

  div(key(id), absolute, left(px(x)), top(px(y)), size(px(CELL)), flex,
      items_center, justify_center, rounded(px(4)), hover(bg(k->selected)),
      cursor_pointer) {
    civue_button_state state = button();
    civue_rect r =
      civue_rect_inflate(here(), -(CELL - IMAGE) / 2, -(CELL - IMAGE) / 2);
    bool over = state.response.hovered;

    if (over) {
      p->inspected = cell->custom ? cell->custom->emoji->id : 0;
      p->inspected_unicode = cell->unicode;
    }

    // the one pointed at moves, a grid of them all would not be read.
    if (cell->custom) {
      emoji_image(app, cell->custom->emoji->id,
                  cell->custom->emoji->animated && over, r);
    }
    else {
      emoji_node(app, 0, cell->unicode->utf8, IMAGE);
    }

    // the client's lock over what the user may not use. a guild of nothing
    // else is only dimmed, its title saying so once.
    if (cell->access != EMOJI_USABLE) {
      div(absolute, left(px((CELL - IMAGE) / 2)), top(px((CELL - IMAGE) / 2)),
          size(px(IMAGE)), flex, items_center, justify_center, rounded(px(2)),
          bg(veiled ? civue_alpha(k->surface_high, .5f) : rgba(0x00000099))) {
        if (!veiled) {
          icon(lucide, lock, size(4), text_color(CIVUE_WHITE));
        }
      }
    }

    if (state.activated && cell->access == EMOJI_USABLE) {
      press->picked = true;
      press->pick = pick_of(cell);
    }
  }
}

// the cell that ends a section cut short: how many more it holds.
static bool more_cell(App *app, civue_id id, uint32_t more, float x, float y) {
  const Look *k = &app->look;
  char count[16];
  bool pressed = false;

  snprintf(count, sizeof count, "+%u", more);

  div(key(id), absolute, left(px(x + 4)), top(px(y + 4)), size(px(IMAGE)), flex,
      items_center, justify_center, rounded(px(4)), bg(k->hover),
      hover(bg(k->selected)), cursor_pointer) {
    pressed = button().activated;
    text(count, text_size(px(12)), line_height(px(16)), font_weight(semibold),
         text_color(k->subtle));
  }

  return pressed;
}

static void section_title(App *app, struct EmojiPicker *p, const Section *s,
                          float y) {
  const Look *k = &app->look;

  div(key(id_at(id("emoji-title"), s->key)), absolute, left(0), top(px(y)),
      w(full), h(px(TITLE)), flex, items_center, gap(2), pl(2), pr(1),
      bg(k->surface_high), text_color(k->subtle), hover(text_color(k->strong)),
      cursor_pointer) {
    if (button().activated) {
      ids_toggle(&p->folded, s->key);
      p->folds++;
    }

    section_glyph(app, s, 16);
    text(s->title, min_w(0), truncate, text_size(px(14)), line_height(px(18)),
         font_weight(semibold));

    if (s->locked) {
      icon(lucide, lock, size(px(12)), flex_shrink_0);
    }

    if (s->folded) {
      icon(lucide, chevron_right, size(4), flex_shrink_0);
    }
    else {
      icon(lucide, chevron_down, size(4), flex_shrink_0);
    }
  }
}

static void emoji_list(App *app, struct EmojiPicker *p, civue_id list,
                       Press *press) {
  civue_ui *ui = civue_ui_current();
  float top = civue_scroll_y(ui, list);
  float view = (float)last_size(list).y;
  float from = top - AHEAD, to = top + (view > 0 ? view : 400) + AHEAD;
  uint32_t columns = p->columns;
  civue_id base = id("emoji-cell");

  if (!p->cells.len) {
    text(p->search && *p->search ? "No emoji matches your search."
                                 : "No emoji to show.",
         flex_shrink_0, px(4), py(6), text_center, text_size(px(14)),
         line_height(px(18)), text_color(app->look.faint));
    return;
  }

  div(relative, flex_shrink_0, w(full), h(px(p->height))) {
    for (uint32_t i = 0; i < p->sections.len; i++) {
      const Section *s = &p->sections.v[i];
      bool titled = s->kind != SECTION_RESULTS;
      float cells_y = s->y + (titled ? TITLE : LIST_PAD);

      if (s->y + s->height < from || s->y > to) {
        continue;
      }

      for (uint32_t n = 0; n < s->shown; n++) {
        float x = LIST_PAD + (float)(n % columns) * CELL;
        float y = cells_y + (float)(n / columns) * CELL;
        civue_id id = id_at(id_at(base, s->key), n);

        if (y + CELL < from || y > to) {
          continue;
        }

        if (s->cut && n + 1 == s->shown) {
          if (more_cell(app, id, s->count - n, x, y)) {
            ids_toggle(&p->unfolded, s->key);
            p->folds++;
          }
        }
        else {
          emoji_cell(app, p, id, &p->cells.v[s->first + n], x, y, s->locked,
                     press);
        }
      }
    }

    // the titles after the cells, over which the top one stays as they
    // scroll under it.
    for (uint32_t i = 0; i < p->sections.len; i++) {
      const Section *s = &p->sections.v[i];

      if (s->kind == SECTION_RESULTS || s->y + s->height < from || s->y > to) {
        continue;
      }

      float last = s->y + s->height - SECTION_GAP - TITLE;

      section_title(app, p, s, fmaxf(s->y, fminf(top, last)));
    }
  }
}

// the section the list starts in.
static const Section *active_section(const struct EmojiPicker *p, float top) {
  const Section *active = nullptr;

  for (uint32_t i = 0; i < p->sections.len; i++) {
    if (p->sections.v[i].y <= top + 1) {
      active = &p->sections.v[i];
    }
  }

  return active;
}

// what the rail names once the pointer rests on it. the label is drawn
// once the picker is closed: a node opened over an open overlay is laid out
// inside it, where the rail would cut it.
typedef struct {
  civue_id id;
  civue_response response;
  const char *title;
} RailTip;

static bool rail_entry(App *app, const Section *s, bool active, RailTip *tip) {
  const Look *k = &app->look;
  civue_id id = id_at(id("emoji-rail"), s->key);
  bool pressed = false;

  if (s->kind == SECTION_GUILD) {
    char url[CDN_URL_MAX];

    cdn_guild_icon(url, s->guild->id, s->guild->icon, 64);

    div(key(id), flex, items_center, justify_center, size(8), my(1),
        flex_shrink_0, cursor_pointer) {
      civue_button_state state = button();
      // round until pointed at or shown, as the client's.
      float radius = active || state.response.hovered ? 10 : 16;

      pressed = state.activated;

      if (!*url || !image_fill(app, url, radius)) {
        char acronym[32];

        guild_acronym(s->guild->name ? s->guild->name : "?", acronym,
                      sizeof acronym);
        fill(here(), active ? k->brand : k->hover, radius);
        text(acronym, text_size(px(strlen(acronym) > 2 ? 9 : 12)),
             font_weight(medium), text_color(k->text), whitespace_nowrap);
      }

      if (state.response.hovered) {
        *tip = (RailTip){id, state.response, s->title};
      }
    }

    return pressed;
  }

  // the emojis of unicode stand closer together than the guilds.
  float margin = s->kind == SECTION_UNICODE ? 1 : 4;

  div(key(id), flex, items_center, justify_center, size(8), my(px(margin)),
      flex_shrink_0, rounded(px(4)), bg(active ? k->selected : CIVUE_CLEAR),
      hover(bg(k->hover)), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    section_glyph(app, s, 24);

    if (state.response.hovered) {
      *tip = (RailTip){id, state.response, s->title};
    }
  }

  return pressed;
}

// the sections down the side of the picker. the one pressed, null for none.
static const Section *rail(App *app, struct EmojiPicker *p,
                           const Section *active, RailTip *tip) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = id("emoji-rail-list");
  const Section *pressed = nullptr, *people = nullptr;
  uint32_t before = 0;
  float y = 8, active_y = -1, active_h = 0;

  for (uint32_t i = 0; i < p->places.len; i++) {
    before += p->places.v[i].kind != SECTION_UNICODE;
  }

  div(flex, flex_col, flex_shrink_0, w(px(RAIL)), h(full)) {
    div(key(id), flex, flex_col, items_center, flex_1, min_h(0), py(2),
        overflow_y_scroll) {
      for (uint32_t i = 0; i < p->places.len; i++) {
        const Section *s = &p->places.v[i];
        float height = s->kind == SECTION_UNICODE ? 34 : 40;

        // a line between the guilds and the emojis of unicode.
        if (s->kind == SECTION_UNICODE && !people) {
          people = s;

          if (before) {
            div(w(8), h(px(1)), my(3), flex_shrink_0, bg(k->border)) {}
            y += 25;
          }
        }

        bool shown = active && s->key == active->key;

        if (shown) {
          active_y = y;
          active_h = height;
        }

        if (rail_entry(app, s, shown, tip)) {
          pressed = s;
        }

        y += height;
      }
    }

    // with many guilds, a way straight past them.
    if (people && before >= SHORTCUT_FROM) {
      civue_id shortcut = id("emoji-rail-unicode");

      div(key(shortcut), flex, items_center, justify_center, flex_shrink_0,
          h(px(INSPECTOR)), border_t(1), border_color(k->border),
          text_color(k->subtle), hover(text_color(k->strong)), cursor_pointer) {
        civue_button_state state = button();

        if (state.activated) {
          pressed = people;
        }

        icon(lucide, smile, size(6));

        if (state.response.hovered) {
          *tip = (RailTip){shortcut, state.response, "Scroll to Unicode emoji"};
        }
      }
    }
  }

  // the rail follows the list.
  uint64_t key = active ? active->key : 0;

  float view = (float)last_size(id).y;

  if (key != p->active && active_y >= 0 && view > 0) {
    civue_scroll_to_center(ui, id, active_y - 8, active_y + active_h + 8, view,
                           false);
  }

  p->active = key;
  return pressed;
}

static void inspector(App *app, struct EmojiPicker *p) {
  const Look *k = &app->look;
  const Cell *cell = inspected_cell(p);
  char name[96], key[24];

  div(flex, items_center, gap(2), flex_shrink_0, h(px(INSPECTOR)), px(4),
      bg(k->panel), border_t(1), border_color(k->border_strong)) {
    if (!cell) {
      continue;
    }

    snprintf(name, sizeof name, ":%s:", cell_name(cell));

    div(size(px(28)), flex, items_center, justify_center, flex_shrink_0) {
      if (cell->custom) {
        emoji_image(app, cell->custom->emoji->id, cell->custom->emoji->animated,
                    here());
      }
      else {
        emoji_node(app, 0, cell->unicode->utf8, 28);
      }
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      div(flex, items_center, gap(1), min_w(0)) {
        // the client's star before the ones the user starred.
        if (session_emoji_starred(
              app->session, cell->custom
                              ? snowflake_format(cell->custom->emoji->id, key)
                              : cell->unicode->name)) {
          icon(lucide, star, size(px(15)), flex_shrink_0,
               text_color(rgb(0xf0b232)));
        }

        text(name, min_w(0), truncate, text_size(px(16)), line_height(px(20)),
             font_weight(semibold), text_color(k->strong));
      }

      if (cell->custom) {
        const char *why = cell->access == EMOJI_NEEDS_PREMIUM ? "Requires Nitro"
                          : cell->access == EMOJI_NEEDS_BOOSTS
                            ? "Requires more Server Boosts"
                            : nullptr;

        div(flex, items_center, gap(1), min_w(0), text_size(px(12)),
            line_height(px(16)), text_color(k->subtle)) {
          text("from", flex_shrink_0);
          text(cell->custom->guild->name ? cell->custom->guild->name : "",
               min_w(0), truncate, font_weight(semibold));

          if (why) {
            icon(lucide, lock, size(px(11)), flex_shrink_0, ml(1));
            text(why, flex_shrink_0, whitespace_nowrap);
          }
        }
      }
    }

    if (cell->custom) {
      div(size(8), flex, items_center, justify_center, flex_shrink_0) {
        guild_icon(app, cell->custom->guild, 32, false);
      }
    }
  }
}

// the search over the emojis. true when enter was pressed in it.
static bool search_bar(App *app, struct EmojiPicker *p, civue_id list) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_id id = id("emoji-search");
  bool submitted = false;

  div(flex, items_center, flex_shrink_0, px(4), pt(4), pb(3), border_b(1),
      border_color(k->border)) {
    div(key(id), flex_1, h(px(SEARCH)), text_size(px(16)),
        line_height(px(20))) {
      civue_field_look look = field_look(k);

      look.pad = 12;

      if (p->focus && !ui->rerun) {
        ui->focus = id;
        p->focus = false;
      }

      civue_field_response field =
        text_field(&p->search, "Find the perfect emoji", look);

      // what is found starts at its top.
      if (field.changed) {
        civue_set_scroll_y(ui, list, 0);
      }

      submitted = field.submitted;
    }
  }

  return submitted;
}

bool emoji_card(App *app, const Channel *c, EmojiUse use, bool fresh,
                civue_rect card, ReactionPick *out, bool *keep) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  struct EmojiPicker *p = picker_of(app);
  civue_id list = id("emoji-list");
  float width = (float)civue_rect_w(card);
  uint32_t columns =
    (uint32_t)fmaxf(floorf((width - 2 - RAIL - 2 * LIST_PAD) / CELL), 1);
  Press press = {};
  RailTip tip = {};
  bool submitted = false;

  session_fetch_frecency(session);

  if (fresh) {
    free(p->search);
    p->search = nullptr;
    p->focus = true;
    p->inspected = 0;
    p->inspected_unicode = nullptr;
    p->stamp = 0;
    civue_set_scroll_y(ui, list, 0);
  }

  // everything the cells are made of. they point into the store, so any
  // change of it makes them again.
  uint64_t inputs[] = {
    (uintptr_t)session->store,
    session->store->version,
    session->store->emojis_version,
    session->version,
    c ? c->id : 0,
    use,
    columns,
    p->folds,
    civue_hash(p->search ? p->search : "", p->search ? strlen(p->search) : 0),
  };
  uint64_t stamp = civue_hash(inputs, sizeof inputs) | 1;

  if (stamp != p->stamp) {
    p->columns = columns;
    build(app, p, c, use);
    p->stamp = stamp;
  }

  bool searching = p->search && *p->search;
  const Section *active =
    searching ? nullptr : active_section(p, civue_scroll_y(ui, list));
  const Section *jump = nullptr;
  bool inside = ui->input.pressed[0] && ui->input.has_mouse &&
                civue_rect_contains(card, ui->input.mouse);

  overlay() {
    div(key(id("emoji-picker")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(width)), h(px(civue_rect_h(card))), rounded(xl),
        bg(k->surface_high), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      // a press on the card is not one on what is behind it.
      button();

      submitted = search_bar(app, p, list);

      div(flex, flex_1, min_h(0)) {
        jump = rail(app, p, active, &tip);

        div(flex, flex_col, flex_1, min_w(0)) {
          div(key(list), flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
              scrollbars(&app->thin_bars)) {
            emoji_list(app, p, list, &press);
          }

          inspector(app, p);
        }
      }
    }
  }

  if (tip.title) {
    hint(app, tip.id, &tip.response, tip.title, CIVUE_TOOLTIP_RIGHT);
  }

  // the search keeps the keys whatever of the card is pressed.
  if (inside && ui->focus == id("emoji-search")) {
    civue_keep_focus(ui);
  }

  // a section of the rail: the search goes, and the list starts at it once
  // it is laid out again.
  if (jump) {
    uint64_t key = jump->key;

    free(p->search);
    p->search = nullptr;
    p->jump = key;

    if (ids_has(&p->folded, key)) {
      ids_toggle(&p->folded, key);
      p->folds++;
    }

    civue_request_repaint(ui);
  }
  else if (p->jump) {
    for (uint32_t i = 0; i < p->sections.len; i++) {
      if (p->sections.v[i].key == p->jump) {
        civue_set_scroll_y(ui, list, p->sections.v[i].y);
      }
    }

    p->jump = 0;
    civue_request_repaint(ui);
  }

  // enter takes the first one found.
  if (submitted && p->search && *p->search && p->cells.len &&
      p->cells.v[0].access == EMOJI_USABLE) {
    press.picked = true;
    press.pick = pick_of(&p->cells.v[0]);
  }

  if (!press.picked) {
    return false;
  }

  *out = press.pick;
  *keep = (ui->input.mods & CIVUE_MOD_SHIFT) != 0;
  return true;
}

/* ========================================================================== */
/* the picker over the composer.                                              */
/* ========================================================================== */

void emoji_picker_toggle(App *app, civue_rect anchor) {
  if (app->popover == POPOVER_EMOJIS) {
    app->popover = POPOVER_NONE;
    app->focus_composer = true;
    return;
  }

  app->popover = POPOVER_EMOJIS;
  app->emoji_anchor = anchor;
  app->emoji_fresh = true;
}

void emoji_picker(App *app, Channel *c) {
  civue_ui *ui = civue_ui_current();

  if (app->popover != POPOVER_EMOJIS) {
    return;
  }

  // over the composer, on its right.
  civue_rect room = overlay_room();
  civue_rect anchor = app->emoji_anchor;
  float width = fminf(EMOJI_CARD_WIDTH, (float)(room.x1 - room.x0));
  float height = fminf(EMOJI_CARD_HEIGHT, (float)(anchor.y0 - 8 - room.y0));
  float left = fmaxf((float)anchor.x1 - width, (float)room.x0);
  civue_rect card = xywh(left, (float)anchor.y0 - 8 - height, width, height);
  ReactionPick pick = {};
  bool keep = false;
  bool fresh = app->emoji_fresh;

  app->emoji_fresh = false;

  if (emoji_card(app, c, EMOJI_FOR_CHAT, fresh, card, &pick, &keep)) {
    char written[112];
    size_t len = 0;

    // a press writes the emoji and a space, one with shift only the emoji,
    // for another to follow.
    pick_text(written, sizeof written - 1, pick);
    len = strlen(written);

    if (!keep) {
      written[len] = ' ';
      written[len + 1] = '\0';
    }

    composer_insert(app, written);

    if (!keep) {
      app->popover = POPOVER_NONE;
      app->focus_composer = true;
    }

    return;
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(app->emoji_button, ui->input.mouse);

  if (outside) {
    app->popover = POPOVER_NONE;
  }
  else if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
    app->focus_composer = true;
  }
}

/* ========================================================================== */
/* the card of an emoji of a message: what it is and where it comes from.     */
/* ========================================================================== */

constexpr float INFO_WIDTH = 300;

void emoji_card_open(App *app, Snowflake id, const char *name, bool animated,
                     bool fake, civue_rect at) {
  bool same = app->popover == POPOVER_EMOJI_CARD && app->card_emoji == id &&
              app->card_emoji_at.x0 == at.x0 && app->card_emoji_at.y0 == at.y0;

  if (same) {
    app->popover = POPOVER_NONE;
    return;
  }

  char *kept = name ? strdup(name) : nullptr;

  free(app->card_emoji_name);
  app->card_emoji_name = kept;
  app->popover = POPOVER_EMOJI_CARD;
  app->card_emoji = id;
  app->card_emoji_animated = animated;
  app->card_emoji_fake = fake;
  app->card_emoji_at = at;
}

// the guild of the user's that has the emoji, null for none.
static void info_guild(App *app, Snowflake guild_id, const char *name,
                       const char *icon_hash, const char *note) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  cdn_guild_icon(url, guild_id, icon_hash, 64);

  div(flex, flex_col, gap(2), pt(3), border_t(1), border_color(k->border)) {
    text("This emoji is from", text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(k->subtle));

    div(flex, items_center, gap(2), min_w(0)) {
      div(size(8), flex, items_center, justify_center, flex_shrink_0) {
        if (!*url || !image_fill(app, url, 10)) {
          char acronym[32];

          guild_acronym(name ? name : "?", acronym, sizeof acronym);
          fill(here(), k->hover, 10);
          text(acronym, text_size(px(strlen(acronym) > 2 ? 9 : 12)),
               font_weight(medium), text_color(k->text), whitespace_nowrap);
        }
      }

      div(flex, flex_col, flex_1, min_w(0)) {
        text(name ? name : "", truncate, text_size(px(14)), line_height(px(18)),
             font_weight(semibold), text_color(k->strong));

        if (note) {
          text(note, truncate, text_size(px(12)), line_height(px(16)),
               text_color(k->subtle));
        }
      }
    }
  }
}

void emoji_info_card(App *app, Channel *c) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;

  if (app->popover != POPOVER_EMOJI_CARD) {
    return;
  }

  Snowflake emoji_id = app->card_emoji;
  const Guild *owner = guild_emoji_owner(store, emoji_id);
  const EmojiSource *source =
    owner ? nullptr : session_emoji_source(app->session, emoji_id);
  const Emoji *emoji = owner ? store_emoji(owner, emoji_id) : nullptr;
  const char *about = nullptr;
  char name[80], online[48] = "", told[320];

  // the name its guild gives it now, when the message's is an older one.
  snprintf(name, sizeof name, ":%s:",
           emoji && emoji->name   ? emoji->name
           : app->card_emoji_name ? app->card_emoji_name
                                  : "");

  if (owner && owner->id == c->guild_id) {
    about = "This emoji is from this server.";
  }
  else if (owner) {
    about = "This emoji is from one of your servers. Type its name in the "
            "chat bar to use it.";
  }
  else if (source && source->loading) {
    about = "Looking for where this emoji is from...";
  }
  else if (source && source->guild_id) {
    about = "This emoji is from a server you are not in.";
  }
  else {
    about = "This emoji is from a server that is either invite-only or "
            "unavailable.";
  }

  // FakeNitro's own words under its emojis.
  if (app->card_emoji_fake) {
    snprintf(told, sizeof told,
             "%s This is a FakeNitro emoji and renders like a real emoji only "
             "for you. Appears as a link to non-plugin users.",
             about);
    about = told;
  }

  if (source && source->online) {
    snprintf(online, sizeof online, "%u Online", source->online);
  }

  // on the right of the emoji, on its left where there is no room.
  civue_rect room = overlay_room();
  civue_rect at = app->card_emoji_at;
  civue_id id = id("emoji-info");
  float height = (float)last_size(id).y;
  float left = (float)at.x1 + 8;

  if (left + INFO_WIDTH > room.x1) {
    left = fmaxf((float)at.x0 - 8 - INFO_WIDTH, (float)room.x0);
  }

  float top =
    fmaxf(fminf((float)at.y0 - 16, (float)room.y1 - height), (float)room.y0);
  civue_rect card = xywh(left, top, INFO_WIDTH, height);

  overlay() {
    div(key(id), flex, flex_col, gap(3), left(px(left)), top(px(top)),
        w(px(INFO_WIDTH)), p(4), rounded(xl), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg)) {
      card = here();
      button();

      div(flex, items_start, gap(3)) {
        div(size(12), flex_shrink_0) {
          if (!emoji_image(app, emoji_id, app->card_emoji_animated, here())) {
            fill(civue_rect_inflate(here(), -4, -4), k->hover, 6);
          }
        }

        div(flex, flex_col, gap(1), flex_1, min_w(0)) {
          text(name, truncate, text_size(px(16)), line_height(px(20)),
               font_weight(semibold), text_color(k->strong));
          text(about, text_size(px(14)), line_height(px(18)),
               text_color(k->text));
        }
      }

      if (owner) {
        info_guild(app, owner->id, owner->name, owner->icon, nullptr);
      }
      else if (source && source->guild_id) {
        info_guild(app, source->guild_id, source->guild_name,
                   source->guild_icon, *online ? online : nullptr);
      }
    }
  }

  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(at, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}
