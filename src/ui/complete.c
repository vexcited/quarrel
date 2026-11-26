#include "complete.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "components.h"
#include "discord/cdn.h"
#include "discord/commands.h"
#include "discord/expressions.h"
#include "widgets.h"

// the list's metrics: a title, a row, a command's row with its description
// under its name, the bar of the command being typed, and how tall the card
// gets before it scrolls.
constexpr float TITLE = 28;
constexpr float ROW = 36;
constexpr float COMMAND_ROW = 48;
constexpr float BAR = 40;
constexpr float CARD_MAX = 400;
constexpr float PAD = 8;

// discord's client lists twenty commands for what was typed.
constexpr size_t COMMANDS_MAX = 20;
// every command of the channel shows after a slash alone, up to this many.
constexpr size_t BROWSE_MAX = 200;
// a command takes this many options at most.
constexpr size_t OPTIONS_MAX = 25;
constexpr size_t ROWS_MAX = 10;

// what picking a row does.
typedef enum : uint8_t {
  // puts its text in place of the token.
  PICK_INSERT,
  // starts the command `index` of the channel's.
  PICK_SLASH,
  // names the option `index` of the command.
  PICK_OPTION,
  // gives its text to the option being typed.
  PICK_VALUE,
} Pick;

typedef struct {
  // a line over the rows after it, or one that says how the list is when
  // `info`: neither is picked.
  bool title, info;
  CompleteIcon icon;
  Snowflake id;
  char *name;
  civue_icon glyph;
  civue_color color;
  char *label, *detail, *insert;
  Pick pick;
  uint32_t index;
  // what a value picked sends when it is not what it shows.
  char *value;
} Row;

struct CompleteList {
  Row *v;
  size_t len, cap;
  // what the rows added next do when picked.
  Pick pick;
  uint32_t index;
  const char *value;
};

// a value an application completed an option with: what the composer shows
// for it and what is sent.
typedef struct {
  char *option, *shown, *value;
} Picked;

struct Complete {
  CompleteList list;
  // the bytes of the composer a pick replaces.
  uint32_t from, to;
  uint32_t selected;
  // enter picks the selected row, where it otherwise sends.
  bool takes_enter;
  // what the list was made from, and the text alone.
  uint64_t stamp, text_stamp;
  // the text escape closed the list at.
  uint64_t dismissed;
  // the selection moved by the keys and is to be scrolled to.
  bool reveal;

  // the command being typed: its id and its words, null for none.
  Snowflake command_id;
  char *slash;
  // what the bar over the list says of it, and what holds it back.
  char *hint, *error;
  uint64_t error_stamp;
  Picked *picked;
  size_t npicked;

  // the composer the list was last drawn over.
  civue_rect box;
};

static civue_id composer_id(void) {
  return id("composer");
}

static struct Complete *state(App *app) {
  if (!app->complete) {
    app->complete = calloc(1, sizeof *app->complete);
  }

  return app->complete;
}

static uint64_t mix(uint64_t h, const void *bytes, size_t len) {
  const uint8_t *b = bytes;

  for (size_t i = 0; i < len; i++) {
    h = (h ^ b[i]) * 0x100000001b3ull;
  }

  return h;
}

static uint64_t mix_u64(uint64_t h, uint64_t v) {
  return mix(h, &v, sizeof v);
}

static bool blank_char(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

static char *copy(const char *s) {
  return s ? strdup(s) : nullptr;
}

static char *slice(const char *text, uint32_t from, uint32_t to) {
  return strndup(text + from, to - from);
}

/* ========================================================================== */
/* the list.                                                                  */
/* ========================================================================== */

static void free_row(Row *r) {
  free(r->name);
  free(r->label);
  free(r->detail);
  free(r->insert);
  free(r->value);
}

static void list_clear(CompleteList *list) {
  for (size_t i = 0; i < list->len; i++) {
    free_row(&list->v[i]);
  }

  list->len = 0;
  list->pick = PICK_INSERT;
  list->index = 0;
  list->value = nullptr;
}

static Row *list_push(CompleteList *list) {
  if (list->len == list->cap) {
    list->cap = list->cap ? list->cap * 2 : 32;
    list->v = realloc(list->v, list->cap * sizeof *list->v);
  }

  Row *r = &list->v[list->len++];

  *r = (Row){};
  return r;
}

CompleteList *complete_list_new(void) {
  return calloc(1, sizeof(CompleteList));
}

void complete_list_take(CompleteList *list, CompleteList *from) {
  for (size_t i = 0; i < from->len; i++) {
    *list_push(list) = from->v[i];
  }

  free(from->v);
  free(from);
}

void complete_title(CompleteList *list, const char *title) {
  Row *r = list_push(list);

  r->title = true;
  r->label = copy(title ? title : "");
}

// a line that tells how the list is, like one that loads.
static void list_info(CompleteList *list, const char *says) {
  complete_title(list, says);
  list->v[list->len - 1].info = true;
}

void complete_add(CompleteList *list, const CompleteRow *row) {
  Row *r = list_push(list);

  *r = (Row){
    .icon = row->icon,
    .id = row->id,
    .name = copy(row->name),
    .glyph = row->glyph,
    .color = row->color,
    .label = copy(row->label ? row->label : ""),
    .detail = copy(row->detail),
    .insert = copy(row->insert ? row->insert : ""),
    .pick = list->pick,
    .index = list->index,
    .value = copy(list->value),
  };
}

// a title no row followed says nothing.
static void list_trim(CompleteList *list) {
  while (list->len && list->v[list->len - 1].title &&
         !list->v[list->len - 1].info) {
    free_row(&list->v[--list->len]);
  }
}

static size_t list_rows(const CompleteList *list) {
  size_t rows = 0;

  for (size_t i = 0; i < list->len; i++) {
    rows += !list->v[i].title;
  }

  return rows;
}

static float row_height(const Row *r) {
  return r->title ? TITLE : r->pick == PICK_SLASH ? COMMAND_ROW : ROW;
}

// the next row to select from `at`, going `step`, titles passed over and
// the ends joined.
static uint32_t step_from(const CompleteList *list, uint32_t at, int step) {
  for (size_t tries = 0; tries < list->len; tries++) {
    at = (uint32_t)((at + list->len + step) % list->len);

    if (!list->v[at].title) {
      return at;
    }
  }

  return at;
}

static uint32_t first_row(const CompleteList *list) {
  for (uint32_t i = 0; i < list->len; i++) {
    if (!list->v[i].title) {
      return i;
    }
  }

  return 0;
}

// each kind of completion, the first whose token the caret is in listing,
// in the order of discord's client.
static const CompleteSource SOURCES[] = {
  {emoji_complete_find, emoji_complete_fill},
  {channel_find, channel_offer},
  {mention_find, mention_offer},
};

// lists what the sources complete at the caret. true when one did.
static bool offer_sources(App *app, Channel *c, const char *text,
                          uint32_t caret) {
  struct Complete *k = state(app);
  CompleteList *list = &k->list;

  for (size_t i = 0; i < countof(SOURCES); i++) {
    int32_t start = SOURCES[i].find(app, c, text, caret);

    if (start < 0 || (uint32_t)start >= caret) {
      continue;
    }

    char *token = slice(text, (uint32_t)start, caret);

    list_clear(list);
    SOURCES[i].offer(app, c, token, list);
    free(token);
    list_trim(list);

    if (list_rows(list)) {
      k->from = (uint32_t)start;
      k->to = caret;
      k->takes_enter = true;
      return true;
    }
  }

  list_clear(list);
  return false;
}

/* ========================================================================== */
/* the command being typed.                                                   */
/* ========================================================================== */

// an option as it was typed: `name:` then its value, or the value alone
// for the text typed before any name.
typedef struct {
  const CommandOption *option;
  // where its name starts, and the bytes of its value without the blanks
  // around it. `end` is where the next option starts.
  uint32_t key, from, to, end;
  bool loose;
} Given;

typedef struct {
  const Slash *slash;
  Given given[OPTIONS_MAX + 1];
  size_t ngiven;
  // the one the caret is in, -1 for none.
  int active;
  // text typed that no option takes.
  bool stray;
} Typed;

static const Slash *slash_find(const Slash *slashes, size_t count,
                               Snowflake command_id, const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (slashes[i].command->id == command_id &&
        strcmp(slashes[i].name, name) == 0) {
      return &slashes[i];
    }
  }

  return nullptr;
}

// what was typed after the slash as the client reads a command's name in
// it, in lower case: the options typed left out, three words at most.
// `ended` when a blank or something more followed the name.
static void typed_name(const char *text, char *out, size_t cap, bool *ended) {
  const char *typed = text + 1;
  const char *colon = strchr(typed, ':');
  size_t len = strlen(typed), words = 0;

  *ended = false;

  // an option's name and what follows it are no part of the command's.
  if (colon) {
    const char *space = colon;

    while (space > typed && *space != ' ') {
      space--;
    }

    *ended = *space == ' ';
    len = *ended ? (size_t)(space - typed) : (size_t)(colon - typed);
  }

  for (size_t i = 0; i < len; i++) {
    if (typed[i] == ' ' && ++words == 3) {
      *ended = true;
      len = i;
    }
  }

  while (len && typed[len - 1] == ' ') {
    *ended = true;
    len--;
  }

  snprintf(out, cap, "%.*s", (int)len, typed);

  for (char *c = out; *c; c++) {
    *c = (char)tolower((unsigned char)*c);
  }
}

// whether `text` still starts with the words of the command picked.
static bool starts_with_slash(const char *text, const char *name) {
  size_t len = strlen(name);

  return text[0] == '/' && strncmp(text + 1, name, len) == 0 &&
         (text[1 + len] == ' ' || text[1 + len] == '\0');
}

static void unpin(struct Complete *k) {
  free(k->slash);
  k->slash = nullptr;
  k->command_id = 0;

  for (size_t i = 0; i < k->npicked; i++) {
    free(k->picked[i].option);
    free(k->picked[i].shown);
    free(k->picked[i].value);
  }

  free(k->picked);
  k->picked = nullptr;
  k->npicked = 0;
}

static void pin(struct Complete *k, const Slash *slash) {
  unpin(k);
  k->command_id = slash->command->id;
  k->slash = strdup(slash->name);
}

// the command the composer holds: the one picked while the text still
// starts with it, else the one whose words were typed whole with a blank
// after them, when no other command is called so or starts so. `ended`
// takes the words alone too, for a text about to be sent.
static const Slash *pinned(App *app, Channel *c, const char *text, bool ended) {
  struct Complete *k = state(app);
  size_t count = 0, same = 0, longer = 0;
  const Slash *typed = nullptr;
  char name[160];
  bool terminated = false;

  if (text[0] != '/' || app->editing) {
    unpin(k);
    return nullptr;
  }

  // the commands are only asked of discord once a slash was typed.
  const Slash *slashes = commands_in(app->session, c, &count);

  if (k->slash) {
    const Slash *kept = slash_find(slashes, count, k->command_id, k->slash);

    if (kept && starts_with_slash(text, kept->name)) {
      return kept;
    }

    unpin(k);
  }

  typed_name(text, name, sizeof name, &terminated);

  // the name typed whole, then with fewer of its words: the keys of a
  // frame may bring more than the blank that ends it.
  for (char *cut = name; cut && *name; terminated = true) {
    size_t len = strlen(name);

    same = longer = 0;

    for (size_t i = 0; i < count && (terminated || ended); i++) {
      if (strcasecmp(slashes[i].name, name) == 0) {
        typed = &slashes[i];
        same++;
      }
      else if (strncasecmp(slashes[i].name, name, len) == 0 &&
               slashes[i].name[len] == ' ') {
        longer++;
      }
    }

    if (same == 1 && !longer) {
      pin(k, typed);
      return typed;
    }

    if ((cut = strrchr(name, ' ')) != nullptr) {
      *cut = 0;
    }
  }

  return nullptr;
}

static const CommandOption *key_at(const Slash *slash, const Typed *typed,
                                   const char *text, uint32_t at,
                                   uint32_t *len) {
  for (size_t i = 0; i < slash->noptions; i++) {
    const CommandOption *o = &slash->options[i];
    size_t n = strlen(o->shown);
    bool given = false;

    if (strncmp(text + at, o->shown, n) != 0 || text[at + n] != ':') {
      continue;
    }

    for (size_t j = 0; j < typed->ngiven; j++) {
      given |= typed->given[j].option == o;
    }

    if (!given) {
      *len = (uint32_t)n + 1;
      return o;
    }
  }

  return nullptr;
}

static void trim(const char *text, uint32_t *from, uint32_t *to) {
  while (*from < *to && blank_char(text[*from])) {
    (*from)++;
  }

  while (*to > *from && blank_char(text[*to - 1])) {
    (*to)--;
  }
}

static bool is_given(const Typed *typed, const CommandOption *o) {
  for (size_t i = 0; i < typed->ngiven; i++) {
    if (typed->given[i].option == o) {
      return true;
    }
  }

  return false;
}

// the first option of the command not given yet, the ones it needs first,
// or only those when `needed`.
static const CommandOption *next_option(const Typed *typed, bool needed) {
  const Slash *slash = typed->slash;

  for (int pass = 0; pass < (needed ? 1 : 2); pass++) {
    for (size_t i = 0; i < slash->noptions; i++) {
      const CommandOption *o = &slash->options[i];

      if (!is_given(typed, o) && o->required == (pass == 0)) {
        return o;
      }
    }
  }

  return nullptr;
}

// reads the options typed after the words of `slash`: each `name:` at the
// start of a word opens one that runs to the next, and what comes before
// the first goes to the first option the command still takes.
static void read_typed(const Slash *slash, const char *text, uint32_t caret,
                       Typed *typed) {
  uint32_t len = (uint32_t)strlen(text);
  uint32_t start = (uint32_t)strlen(slash->name) + 1;
  uint32_t loose_end = len;
  bool word = true;
  Given *open = nullptr;

  *typed = (Typed){.slash = slash, .active = -1};

  if (start > len) {
    start = len;
  }

  for (uint32_t i = start; i < len;) {
    uint32_t taken = 0;
    const CommandOption *o =
      word ? key_at(slash, typed, text, i, &taken) : nullptr;

    if (o && typed->ngiven < OPTIONS_MAX) {
      if (open) {
        open->end = open->to = i;
      }
      else {
        loose_end = i;
      }

      open = &typed->given[typed->ngiven++];
      *open = (Given){.option = o, .key = i, .from = i + taken};
      i += taken;
      word = false;
      continue;
    }

    word = blank_char(text[i]);
    i++;
  }

  if (open) {
    open->end = open->to = len;
  }

  for (size_t i = 0; i < typed->ngiven; i++) {
    Given *g = &typed->given[i];

    if (caret >= g->from && caret <= g->end) {
      typed->active = (int)i;
    }

    trim(text, &g->from, &g->to);
  }

  uint32_t from = start, to = loose_end;
  bool inside = caret > start && caret <= loose_end;

  trim(text, &from, &to);

  if (from == to && (!inside || typed->ngiven)) {
    return;
  }

  const CommandOption *takes = next_option(typed, false);

  if (!takes) {
    typed->stray = from < to;
    return;
  }

  Given *g = &typed->given[typed->ngiven];

  *g = (Given){
    .option = takes,
    .key = from,
    .from = from,
    .to = to,
    .end = loose_end,
    .loose = true,
  };

  if (inside) {
    typed->active = (int)typed->ngiven;
  }

  typed->ngiven++;
}

static const Picked *picked_for(const struct Complete *k,
                                const CommandOption *o, const char *shown) {
  for (size_t i = 0; i < k->npicked; i++) {
    if (strcmp(k->picked[i].option, o->name) == 0 &&
        strcmp(k->picked[i].shown, shown) == 0) {
      return &k->picked[i];
    }
  }

  return nullptr;
}

static void remember_pick(struct Complete *k, const CommandOption *o,
                          const char *shown, const char *value) {
  for (size_t i = 0; i < k->npicked; i++) {
    if (strcmp(k->picked[i].option, o->name) == 0) {
      free(k->picked[i].shown);
      free(k->picked[i].value);
      k->picked[i].shown = strdup(shown);
      k->picked[i].value = strdup(value);
      return;
    }
  }

  k->picked = realloc(k->picked, (k->npicked + 1) * sizeof *k->picked);
  k->picked[k->npicked++] = (Picked){
    strdup(o->name),
    strdup(shown),
    strdup(value),
  };
}

// the choice whose name is what was typed, to the letter as the client
// compares them.
static const CommandChoice *choice_named(const CommandOption *o,
                                         const char *typed) {
  for (size_t i = 0; i < o->nchoices; i++) {
    if (strcmp(o->choices[i].name, typed) == 0) {
      return &o->choices[i];
    }
  }

  return nullptr;
}

static size_t utf16_length(const char *text) {
  size_t n = 0;

  for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
    if ((*c & 0xc0) != 0x80) {
      n += *c >= 0xf0 ? 2 : 1;
    }
  }

  return n;
}

// why a string of `units` is not of a length the option takes, in the
// client's words. false when it is.
static bool wrong_length(const CommandOption *o, size_t units, char *why,
                         size_t cap) {
  bool low = o->has_min_length && units < o->min_length;
  bool high = o->has_max_length && units > o->max_length;

  if (!low && !high) {
    return false;
  }

  if (o->has_min_length && o->has_max_length) {
    if (o->min_length == o->max_length) {
      snprintf(why, cap, "Enter %u characters.", o->min_length);
    }
    else {
      snprintf(why, cap, "Enter between %u and %u characters.", o->min_length,
               o->max_length);
    }
  }
  else if (o->has_min_length) {
    snprintf(why, cap, "Enter %u or more characters.", o->min_length);
  }
  else {
    snprintf(why, cap, "Enter %u or fewer characters.", o->max_length);
  }

  return true;
}

static bool wrong_number(const CommandOption *o, double number, char *why,
                         size_t cap) {
  bool low = o->has_min && number < o->min_value;
  bool high = o->has_max && number > o->max_value;

  if (!low && !high) {
    return false;
  }

  if (o->has_min && o->has_max) {
    snprintf(why, cap, "Enter a number between %g and %g.", o->min_value,
             o->max_value);
  }
  else if (o->has_min) {
    snprintf(why, cap, "Enter a number greater than or equal to %g.",
             o->min_value);
  }
  else {
    snprintf(why, cap, "Enter a number less than or equal to %g.",
             o->max_value);
  }

  return true;
}

// what is sent for the option from what was typed, from malloc, or null
// with why in `why`, in the client's words.
static char *value_for(App *app, Channel *c, const CommandOption *o,
                       const char *typed, char *why, size_t cap) {
  struct Complete *k = state(app);
  const CommandChoice *choice = choice_named(o, typed);
  const Picked *picked = o->autocomplete ? picked_for(k, o, typed) : nullptr;
  const Guild *guild = store_guild(app->session->store, c->guild_id);
  char digits[24];
  char *end = nullptr;
  Snowflake id = 0;

  if (o->nchoices && !choice) {
    snprintf(why, cap, "Not a valid choice.");
    return nullptr;
  }

  if (choice) {
    return strdup(choice->value);
  }

  switch (o->type) {
    case OPTION_STRING:
      if (picked) {
        return strdup(picked->value);
      }

      if (wrong_length(o, utf16_length(typed), why, cap)) {
        return nullptr;
      }

      // as a message is written: its mentions and its emojis as discord
      // holds them.
      char *named = complete_encode(app, c, typed);
      char *written = expressions_encode(app->session->store, c, named);

      free(named);
      return written;
    case OPTION_INTEGER:
    case OPTION_NUMBER: {
      const char *text = picked ? picked->value : typed;
      double number = strtod(text, &end);
      bool whole = o->type == OPTION_INTEGER;

      // javascript's numbers hold whole ones up to 2^53.
      if (end == text || *end || !isfinite(number) ||
          fabs(number) > 0x1p53 - 1 || (whole && number != floor(number))) {
        snprintf(why, cap,
                 whole ? "Input a valid integer." : "Input a valid number.");
        return nullptr;
      }

      if (wrong_number(o, number, why, cap)) {
        return nullptr;
      }

      return strdup(text);
    }
    case OPTION_BOOLEAN:
      if (strcasecmp(typed, "true") == 0 || strcasecmp(typed, "false") == 0) {
        return strdup(strcasecmp(typed, "true") == 0 ? "true" : "false");
      }

      snprintf(why, cap, "Pick either true or false.");
      return nullptr;
    case OPTION_USER:
      if ((id = mention_user_named(app, c, typed)) == 0) {
        snprintf(why, cap, "Not a valid user.");
      }

      break;
    case OPTION_ROLE:
      if ((id = mention_role_named(app, c, typed)) == 0) {
        snprintf(why, cap, "Not a valid role.");
      }

      break;
    case OPTION_MENTIONABLE:
      // a role is looked for first, as the client does, and everyone is
      // one.
      if (guild && strcmp(typed, "@everyone") == 0) {
        id = guild->id;
      }
      else if ((id = mention_role_named(app, c, typed)) == 0 &&
               (id = mention_user_named(app, c, typed)) == 0) {
        snprintf(why, cap, "Not a valid user or role.");
      }

      break;
    case OPTION_CHANNEL:
      if ((id = mention_channel_named(app, c, typed)) == 0) {
        snprintf(why, cap, "Not a valid channel.");
      }

      break;
    case OPTION_ATTACHMENT:
      snprintf(why, cap, "Quarrel cannot send a file to a command yet.");
      return nullptr;
    default:
      return strdup(typed);
  }

  return id ? strdup(snowflake_format(id, digits)) : nullptr;
}

/* ========================================================================== */
/* what the list holds while a command is typed.                              */
/* ========================================================================== */

typedef struct {
  uint32_t index;
  int score;
} Scored;

static const Slash *sorted_slashes;

static int by_command_score(const void *a, const void *b) {
  const Scored *x = a, *y = b;

  if (x->score != y->score) {
    return x->score - y->score;
  }

  int named =
    strcasecmp(sorted_slashes[x->index].name, sorted_slashes[y->index].name);

  return named ? named : (x->index > y->index) - (x->index < y->index);
}

static void command_row(App *app, CompleteList *list, const Slash *slash,
                        uint32_t index, bool sectioned) {
  const Command *command = slash->command;
  const CommandApplication *of =
    commands_application(app->session, command->application_id);
  char label[200];

  // a command found by what was typed tells whose it is.
  if (sectioned || !of || !of->name) {
    snprintf(label, sizeof label, "/%s", slash->name);
  }
  else {
    snprintf(label, sizeof label, "/%s  ·  %s", slash->name, of->name);
  }

  list->pick = PICK_SLASH;
  list->index = index;
  complete_add(list, &(CompleteRow){
                       .icon = command->builtin ? COMPLETE_ICON_GLYPH
                                                : COMPLETE_ICON_APPLICATION,
                       .id = command->application_id,
                       .name = of ? of->icon : nullptr,
                       .glyph = CIVUE_ICON(mingcute, terminal_box_fill),
                       .label = label,
                       .detail = slash->description,
                     });
}

// the commands of the channel: all of them under their applications after
// a slash alone, else the twenty that answer best what was typed, as
// discord's client scores them.
static void fill_commands(App *app, Channel *c, const char *text,
                          CompleteList *list) {
  Session *session = app->session;
  size_t count = 0, found = 0;
  const Slash *slashes = commands_in(session, c, &count);
  char query[160];
  bool ended = false;

  typed_name(text, query, sizeof query, &ended);

  if (!*query && !ended) {
    Snowflake section = 0;

    for (size_t i = 0; i < count && i < BROWSE_MAX; i++) {
      Snowflake of = slashes[i].command->application_id;
      const CommandApplication *listed = commands_application(session, of);

      if (of != section || i == 0) {
        section = of;
        complete_title(list, listed && listed->name ? listed->name : "");
      }

      command_row(app, list, &slashes[i], (uint32_t)i, true);
    }
  }
  else if (count) {
    Scored *scored = malloc(count * sizeof *scored);
    size_t len = strlen(query);

    for (size_t i = 0; i < count; i++) {
      const char *name = slashes[i].name;
      int score = commands_score(session, &slashes[i], query);
      // once a blank ended the name, only the commands called so are left,
      // and the ones with more words after it.
      bool fits = !ended || (strncasecmp(name, query, len) == 0 &&
                             (name[len] == '\0' || name[len] == ' '));

      if (score >= 0 && fits) {
        scored[found++] = (Scored){(uint32_t)i, score};
      }
    }

    sorted_slashes = slashes;
    qsort(scored, found, sizeof *scored, by_command_score);

    for (size_t i = 0; i < found && i < COMMANDS_MAX; i++) {
      command_row(app, list, &slashes[scored[i].index], scored[i].index, false);
    }

    free(scored);
  }

  if (commands_loading(session, c)) {
    list_info(list, "Loading commands...");
  }
}

static int by_rank(const void *a, const void *b) {
  const Scored *x = a, *y = b;

  return x->score != y->score ? y->score - x->score
                              : (x->index > y->index) - (x->index < y->index);
}

// the choices that answer what was typed, the best first.
static void fill_choices(const CommandOption *o, const char *typed,
                         CompleteList *list) {
  Scored scored[OPTIONS_MAX];
  size_t found = 0;
  char query[160];

  mention_fold(typed, query, sizeof query);

  for (size_t i = 0; i < o->nchoices && found < countof(scored); i++) {
    int score = mention_rank(o->choices[i].name, query);

    if (score) {
      scored[found++] = (Scored){(uint32_t)i, score};
    }
  }

  qsort(scored, found, sizeof *scored, by_rank);

  for (size_t i = 0; i < found; i++) {
    const char *name = o->choices[scored[i].index].name;

    complete_add(list, &(CompleteRow){.label = name, .insert = name});
  }
}

// what its application completes the option with, asked as it is typed.
static void fill_suggestions(App *app, Channel *c, const Typed *typed,
                             const char *text, CompleteList *list) {
  const Given *g = &typed->given[typed->active];
  CommandValue values[OPTIONS_MAX + 1];
  char *texts[OPTIONS_MAX + 1];
  size_t count = 0, nchoices = 0;
  bool loading = false, failed = false;

  // the others as they are so far, the ones that would not be sent left
  // out.
  for (size_t i = 0; i < typed->ngiven; i++) {
    const Given *other = &typed->given[i];
    char *raw = slice(text, other->from, other->to);
    char why[8];

    if (other == g) {
      texts[count] = raw;
      values[count] = (CommandValue){g->option, texts[count], true};
      count++;
      continue;
    }

    texts[count] =
      *raw ? value_for(app, c, other->option, raw, why, sizeof why) : nullptr;
    free(raw);

    if (texts[count]) {
      values[count] = (CommandValue){other->option, texts[count], false};
      count++;
    }
  }

  commands_suggest(app->session, c->id, typed->slash, values, count);

  const CommandChoice *choices =
    commands_suggestions(app->session, &nchoices, &loading, &failed);

  for (size_t i = 0; i < nchoices && !loading; i++) {
    list->value = choices[i].value;
    complete_add(list, &(CompleteRow){
                         .label = choices[i].name,
                         .insert = choices[i].name,
                       });
  }

  list->value = nullptr;

  if (loading) {
    list_info(list, "Loading options...");
  }
  else if (failed) {
    list_info(list, "Loading options failed");
  }
  else if (!nchoices) {
    list_info(list, "No options match your search");
  }

  for (size_t i = 0; i < count; i++) {
    free(texts[i]);
  }
}

// what the option being typed takes: its choices, true or false, who or
// what it names, or what its application completes it with.
static void fill_values(App *app, Channel *c, const Typed *typed,
                        const char *text, CompleteList *list) {
  struct Complete *k = state(app);
  const Given *g = &typed->given[typed->active];
  const CommandOption *o = g->option;
  char *value = slice(text, g->from, g->to);
  const char *named = *value == '@' || *value == '#' ? value + 1 : value;

  list->pick = PICK_VALUE;

  if (o->nchoices) {
    if (!choice_named(o, value)) {
      fill_choices(o, value, list);
    }
  }
  else if (o->type == OPTION_BOOLEAN) {
    static CommandChoice words[] = {{.name = "True"}, {.name = "False"}};
    CommandOption both = {.choices = words, .nchoices = countof(words)};

    if (strcasecmp(value, "true") != 0 && strcasecmp(value, "false") != 0) {
      fill_choices(&both, value, list);
    }
  }
  else if (mention_whole(value)) {
    // named already.
  }
  else if (o->type == OPTION_USER) {
    mention_users(app, c, named, list, ROWS_MAX);
  }
  else if (o->type == OPTION_ROLE || o->type == OPTION_MENTIONABLE) {
    size_t users = o->type == OPTION_MENTIONABLE
                     ? mention_users(app, c, named, list, ROWS_MAX)
                     : 0;
    char query[128];

    mention_roles(app, c, named, list, ROWS_MAX - users, true);
    mention_fold(named, query, sizeof query);

    // everyone is a role to an option, the guild's own.
    if (c->guild_id && mention_fuzzy(query, "everyone") &&
        strcmp(value, "@everyone") != 0) {
      complete_add(list, &(CompleteRow){
                           .icon = COMPLETE_ICON_GLYPH,
                           .glyph = CIVUE_ICON(mingcute, at_fill),
                           .label = "@everyone",
                           .insert = "@everyone",
                         });
    }
  }
  else if (o->type == OPTION_CHANNEL) {
    mention_channels(app, c, named, list, o->channel_types, o->nchannel_types);
  }
  else if (o->autocomplete && !picked_for(k, o, value)) {
    fill_suggestions(app, c, typed, text, list);
  }

  free(value);
}

// the options the command still takes, the ones it needs first.
static void fill_options(const Typed *typed, CompleteList *list) {
  const Slash *slash = typed->slash;
  bool titled = false;

  list->pick = PICK_OPTION;

  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < slash->noptions; i++) {
      const CommandOption *o = &slash->options[i];

      if (is_given(typed, o) || o->required != (pass == 0)) {
        continue;
      }

      if (!titled) {
        complete_title(list, "OPTIONS");
        titled = true;
      }

      list->index = (uint32_t)i;
      complete_add(list, &(CompleteRow){
                           .label = o->shown,
                           .detail = *o->description ? o->description : nullptr,
                         });
    }
  }
}

static void set_hint(struct Complete *k, const char *hint) {
  free(k->hint);
  k->hint = copy(hint);
}

// the list of a composer that holds a command: what the option under the
// caret takes, else what an @ or a # in it names, else the options left.
static void fill_command(App *app, Channel *c, const Slash *slash,
                         const char *text, uint32_t caret) {
  struct Complete *k = state(app);
  CompleteList *list = &k->list;
  Typed typed;
  char hint[320];
  bool free_text = true;

  read_typed(slash, text, caret, &typed);
  snprintf(hint, sizeof hint, "%s", slash->description);

  if (typed.active >= 0) {
    const Given *g = &typed.given[typed.active];

    fill_values(app, c, &typed, text, list);
    list_trim(list);
    snprintf(hint, sizeof hint, "%s: %s", g->option->shown,
             g->option->description);
    k->from = g->loose ? g->key : g->from;
    k->to = g->to;
    k->takes_enter = list_rows(list) != 0;
    free_text = g->option->type == OPTION_STRING && !g->option->nchoices;
  }

  set_hint(k, hint);

  if (list->len) {
    return;
  }

  if (free_text && offer_sources(app, c, text, caret)) {
    return;
  }

  fill_options(&typed, list);
  k->from = k->to = (uint32_t)strlen(text);
  k->takes_enter = false;
}

/* ========================================================================== */
/* making the list and picking from it.                                       */
/* ========================================================================== */

static void set_error(struct Complete *k, const char *error) {
  free(k->error);
  k->error = copy(error);
  k->error_stamp = k->text_stamp;
}

static void rebuild(App *app, Channel *c, const char *text, uint32_t caret) {
  struct Complete *k = state(app);
  CompleteList *list = &k->list;
  const Slash *slash = pinned(app, c, text, false);

  list_clear(list);
  set_hint(k, nullptr);
  k->takes_enter = true;
  k->from = k->to = caret;

  if (slash) {
    fill_command(app, c, slash, text, caret);
    list_trim(list);
    return;
  }

  if (offer_sources(app, c, text, caret)) {
    return;
  }

  // a command is named from the start of the text, on its first line.
  if (text[0] == '/' && !app->editing && !strchr(text, '\n')) {
    fill_commands(app, c, text, list);
    list_trim(list);
    k->from = 0;
    k->to = (uint32_t)strlen(text);
  }
}

// puts `with` in place of the bytes `from` to `to` of the composer, the
// caret after it.
static void splice(App *app, uint32_t from, uint32_t to, const char *with) {
  civue_ui *ui = civue_ui_current();
  char *held = civue_field_replace(ui, composer_id(), from, to, with);

  if (held) {
    free(app->draft);
    app->draft = held;
  }

  civue_request_repaint(ui);
}

static void pick(App *app, Channel *c, const Row *row) {
  struct Complete *k = state(app);
  const char *text = app->draft ? app->draft : "";
  uint32_t len = (uint32_t)strlen(text);
  char put[512];

  if (k->from > k->to || k->to > len) {
    return;
  }

  if (row->pick == PICK_INSERT) {
    splice(app, k->from, k->to, row->insert);
    return;
  }

  size_t count = 0;
  const Slash *slashes = commands_in(app->session, c, &count);

  if (row->pick == PICK_SLASH) {
    if (row->index >= count) {
      return;
    }

    const Slash *slash = &slashes[row->index];
    Typed none = {.slash = slash};
    const CommandOption *first = next_option(&none, true);

    // the command, then the name of the first option it needs.
    snprintf(put, sizeof put, "/%s %s%s", slash->name,
             first ? first->shown : "", first ? ":" : "");
    pin(k, slash);
    splice(app, 0, len, put);
    return;
  }

  const Slash *slash = pinned(app, c, text, true);

  if (!slash) {
    return;
  }

  if (row->pick == PICK_OPTION) {
    if (row->index >= slash->noptions) {
      return;
    }

    bool spaced = len == 0 || blank_char(text[len - 1]);

    snprintf(put, sizeof put, "%s%s:", spaced ? "" : " ",
             slash->options[row->index].shown);
    splice(app, len, len, put);
    return;
  }

  Typed typed;

  read_typed(slash, text, k->to, &typed);

  if (typed.active < 0) {
    return;
  }

  const Given *g = &typed.given[typed.active];
  const CommandOption *next = next_option(&typed, true);
  char *insert = strdup(row->insert);
  size_t end = strlen(insert);

  // a mention comes with a blank of its own.
  while (end && insert[end - 1] == ' ') {
    insert[--end] = 0;
  }

  if (row->value) {
    remember_pick(k, g->option, insert, row->value);
  }

  // the value under its option's name, then the next one the command
  // needs.
  snprintf(put, sizeof put, "%s%s%s %s%s", g->loose ? g->option->shown : "",
           g->loose ? ":" : "", insert, next ? next->shown : "",
           next ? ":" : "");
  free(insert);
  splice(app, k->from, g->to > k->from ? g->to : k->from, put);
}

bool complete_key(App *app, Channel *c, civue_key_press press) {
  struct Complete *k = app->complete;
  uint32_t mods = press.mods & (CIVUE_MOD_CMD | CIVUE_MOD_CTRL | CIVUE_MOD_ALT);

  if (!k || !list_rows(&k->list) || k->dismissed == k->text_stamp || mods) {
    return false;
  }

  CompleteList *list = &k->list;

  if (k->selected >= list->len || list->v[k->selected].title) {
    k->selected = first_row(list);
  }

  switch (press.key) {
    case CIVUE_KEY_UP:
    case CIVUE_KEY_DOWN:
      k->selected =
        step_from(list, k->selected, press.key == CIVUE_KEY_UP ? -1 : 1);
      k->reveal = true;
      return true;
    case CIVUE_KEY_ESCAPE:
      k->dismissed = k->text_stamp;
      return true;
    case CIVUE_KEY_ENTER:
      if (!k->takes_enter || (press.mods & CIVUE_MOD_SHIFT)) {
        return false;
      }

      [[fallthrough]];
    case CIVUE_KEY_TAB:
      pick(app, c, &list->v[k->selected]);
      return true;
    default:
      return false;
  }
}

/* ========================================================================== */
/* drawing.                                                                   */
/* ========================================================================== */

static void row_icon(App *app, const Row *r) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Channel *here = app_channel(app);
  civue_color color = r->color.a > 0 ? r->color : k->subtle;
  float side = r->pick == PICK_SLASH ? 32 : 24;

  switch (r->icon) {
    case COMPLETE_ICON_USER:
      avatar(app, store_user(store, r->id), here ? here->guild_id : 0, 24,
             false, k->popover);
      break;
    case COMPLETE_ICON_EMOJI:
      div(flex, items_center, justify_center, size(6), flex_shrink_0) {
        emoji_node(app, r->id, r->name, 20);
      }

      break;
    case COMPLETE_ICON_APPLICATION: {
      char url[CDN_URL_MAX] = "";

      if (r->name) {
        cdn_application_icon(url, r->id, r->name, 64);
      }

      div(flex, items_center, justify_center, size(8), flex_shrink_0,
          rounded(full), bg(k->surface), overflow_hidden) {
        if (!*url || !image_cover(app, url, 16)) {
          glyph(CIVUE_ICON(mingcute, robot_fill), 18, k->subtle);
        }
      }

      break;
    }
    case COMPLETE_ICON_GLYPH:
      div(flex, items_center, justify_center, flex_shrink_0,
          style(.w = civue_px(side), .h = civue_px(side))) {
        glyph(r->glyph, 20, color);
      }

      break;
    default:
      break;
  }
}

// one row, true when it was pressed.
static bool draw_row(App *app, uint32_t index, const Row *r, bool selected) {
  const Look *k = &app->look;
  struct Complete *s = state(app);
  civue_ui *ui = civue_ui_current();
  bool pressed = false;

  if (r->title) {
    div(flex, items_center, flex_shrink_0, h(px(TITLE)), px(2)) {
      if (r->info) {
        text(r->label, truncate, text_size(px(14)), line_height(px(18)),
             text_color(k->subtle));
      }
      else {
        text(r->label, truncate, text_size(px(12)), line_height(px(16)),
             font_weight(semibold), text_color(k->subtle));
      }
    }

    return false;
  }

  div(key(id_at(id("complete-row"), index)), flex, items_center, gap(2),
      flex_shrink_0, h(px(row_height(r))), px(2), rounded(md), cursor_pointer,
      style(.bg = selected ? k->hover : CIVUE_CLEAR)) {
    civue_button_state state = button();

    pressed = state.activated;

    // the pointer selects what it moves over, the keys moving it after.
    if (state.response.hovered && civue_pointer_moved(ui) && !selected) {
      s->selected = index;
      civue_request_repaint(ui);
    }

    row_icon(app, r);

    if (r->pick == PICK_SLASH) {
      div(flex, flex_col, flex_1, min_w(0)) {
        text(r->label, truncate, text_size(px(15)), line_height(px(20)),
             font_weight(medium), text_color(k->strong));
        text(r->detail ? r->detail : "", truncate, text_size(px(12)),
             line_height(px(16)), text_color(k->subtle));
      }

      continue;
    }

    text(r->label, flex_shrink_0, max_w(1_2), truncate, text_size(px(15)),
         line_height(px(20)), font_weight(medium),
         text_color(r->color.a > 0 ? r->color : k->strong));

    if (r->detail) {
      text(r->detail, flex_1, min_w(0), truncate, text_right, text_size(px(13)),
           line_height(px(18)), text_color(k->subtle));
    }
  }

  return pressed;
}

// the bar of the command being typed: its words, then what the option
// under the caret is for, or what holds the command back.
static void bar(App *app) {
  const Look *k = &app->look;
  struct Complete *s = state(app);
  bool wrong = s->error && s->error_stamp == s->text_stamp;
  char name[160];

  snprintf(name, sizeof name, "/%s", s->slash ? s->slash : "");

  div(flex, items_center, gap(2), flex_shrink_0, h(px(BAR)), px(4),
      bg(k->surface), border_b(1), border_color(k->border)) {
    text(name, flex_shrink_0, max_w(1_2), truncate, text_size(px(14)),
         line_height(px(18)), font_weight(semibold), text_color(k->strong));
    text(wrong     ? s->error
         : s->hint ? s->hint
                   : "",
         flex_1, min_w(0), truncate, text_size(px(14)), line_height(px(18)),
         text_color(wrong ? k->critical_text : k->subtle));
  }
}

void complete_popup(App *app, Channel *c, civue_rect box) {
  const Look *k = &app->look;
  struct Complete *s = state(app);
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  const char *text = app->draft ? app->draft : "";
  uint32_t len = (uint32_t)strlen(text);
  uint32_t caret = 0;

  mention_ask_members(app);

  if (!civue_field_caret(ui, composer_id(), &caret, nullptr) ||
      app->popover != POPOVER_NONE) {
    list_clear(&s->list);
    s->stamp = 0;
    return;
  }

  if (caret > len) {
    caret = len;
  }

  s->box = box;

  uint64_t text_stamp = mix_u64(mix(0xcbf29ce484222325ull, text, len), caret);
  uint64_t stamp = mix_u64(text_stamp, c->id);

  stamp = mix_u64(stamp, session->version);
  stamp = mix_u64(stamp, session->store->version);
  stamp = mix_u64(stamp, app->editing);

  if (stamp != s->stamp) {
    bool moved = text_stamp != s->text_stamp;

    s->stamp = stamp;
    s->text_stamp = text_stamp;
    rebuild(app, c, text, caret);

    if (moved || s->selected >= s->list.len ||
        (s->list.len && s->list.v[s->selected].title)) {
      s->selected = first_row(&s->list);
      s->reveal = true;
    }
  }

  CompleteList *list = &s->list;
  bool wrong = s->error && s->error_stamp == s->text_stamp;
  bool barred = s->slash && text[0] == '/' && (s->hint || wrong);

  if ((!list->len && !barred) || s->dismissed == s->text_stamp) {
    return;
  }

  // over the composer and as wide, as tall as its rows up to where it
  // scrolls.
  civue_rect room = overlay_room();
  float rows = 0, top_of = 0, bottom_of = 0;

  for (uint32_t i = 0; i < list->len; i++) {
    if (i == s->selected) {
      top_of = rows;
      bottom_of = rows + row_height(&list->v[i]);
    }

    rows += row_height(&list->v[i]);
  }

  float chrome = (barred ? BAR : 0) + (list->len ? PAD * 2 : 0) + 2;
  float most = fminf(CARD_MAX, (float)(box.y0 - 8 - room.y0));
  float height = fminf(rows + chrome, most);
  float view = height - chrome;
  float width = (float)(box.x1 - box.x0);
  civue_id list_id = id("complete-list");
  civue_rect card = xywh(box.x0, (float)box.y0 - 8 - height, width, height);
  int32_t pressed = -1;

  if (width <= 0 || height <= 0) {
    return;
  }

  if (s->reveal && list->len) {
    // the title over the first row comes along.
    civue_scroll_to(ui, list_id, top_of <= TITLE ? 0 : top_of,
                    bottom_of + PAD * 2, view + PAD * 2);
    s->reveal = false;
  }

  overlay() {
    div(key(id("complete")), flex, flex_col, left(px(card.x0)),
        top(px(card.y0)), w(px(width)), h(px(height)), rounded(lg),
        bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg), overflow_hidden) {
      // a press on the card is not one on what is behind it.
      button();

      if (barred) {
        bar(app);
      }

      if (list->len) {
        div(key(list_id), flex, flex_col, flex_1, min_h(0), p(px(PAD)),
            overflow_y_scroll, scrollbars(&app->thin_bars)) {
          for (uint32_t i = 0; i < list->len; i++) {
            if (draw_row(app, i, &list->v[i], i == s->selected)) {
              pressed = (int32_t)i;
            }
          }
        }
      }
    }
  }

  // the composer keeps the keyboard while its list is pressed.
  if (ui->input.has_mouse && civue_rect_contains(card, ui->input.mouse) &&
      (ui->input.pressed[0] || ui->input.down[0] || ui->input.released[0])) {
    civue_keep_focus(ui);
  }

  if (pressed >= 0 && (uint32_t)pressed < list->len) {
    pick(app, c, &list->v[pressed]);
  }
}

/* ========================================================================== */
/* sending.                                                                   */
/* ========================================================================== */

bool complete_submit(App *app, Channel *c) {
  struct Complete *k = state(app);
  const char *text = app->draft ? app->draft : "";
  const Slash *slash = pinned(app, c, text, true);
  Typed typed;
  CommandValue values[OPTIONS_MAX + 1];
  char *texts[OPTIONS_MAX + 1];
  size_t count = 0;
  char why[200] = "", wrong[240] = "";

  if (!slash) {
    return false;
  }

  read_typed(slash, text, (uint32_t)strlen(text), &typed);

  if (typed.stray) {
    snprintf(wrong, sizeof wrong, "/%s takes nothing more.", slash->name);
  }

  for (size_t i = 0; i < typed.ngiven && !*wrong; i++) {
    const Given *g = &typed.given[i];
    char *raw = slice(text, g->from, g->to);

    // an option named and left empty is one not given.
    if (*raw) {
      texts[count] = value_for(app, c, g->option, raw, why, sizeof why);

      if (texts[count]) {
        values[count] = (CommandValue){g->option, texts[count], false};
        count++;
      }
      else {
        snprintf(wrong, sizeof wrong, "%s: %s", g->option->shown, why);
      }
    }

    free(raw);
  }

  for (size_t i = 0; i < slash->noptions && !*wrong; i++) {
    const CommandOption *o = &slash->options[i];
    bool given = false;

    for (size_t j = 0; j < count; j++) {
      given |= values[j].option == o;
    }

    if (o->required && !given) {
      snprintf(wrong, sizeof wrong,
               "%s: This option is required. Specify a value.", o->shown);
    }
  }

  if (*wrong) {
    set_error(k, wrong);
  }
  else if (slash->command->builtin == BUILTIN_GIF) {
    char *query = strdup(count ? texts[0] : "");

    upload_clear_draft(app);
    unpin(k);
    gif_picker_search(app, k->box, query);
    free(query);
  }
  else {
    commands_run(app->session, c->id, slash, values, count);
    app->stick = true;
    upload_clear_draft(app);
    unpin(k);
  }

  for (size_t i = 0; i < count; i++) {
    free(texts[i]);
  }

  civue_request_repaint(civue_ui_current());
  return true;
}

void complete_free(App *app) {
  struct Complete *k = app->complete;

  if (!k) {
    return;
  }

  list_clear(&k->list);
  unpin(k);
  free(k->list.v);
  free(k->hint);
  free(k->error);
  free(k);
  app->complete = nullptr;
}
