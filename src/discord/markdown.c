#include "discord/markdown.h"

#include <inttypes.h>
#include <stdcountof.h>
#include <stdio.h>
#include <time.h>

constexpr int MD_MAX_DEPTH = 24;
constexpr uint32_t MD_JUMBO_MAX = 30;

typedef struct {
  MdDoc *doc;
  cilo_arena *arena;
  int64_t now;
  int depth;
  // text waiting to become a node, shared by every level of the recursion:
  // each level only touches what it pushed.
  char *pending;
  size_t pending_len, pending_cap;
} Md;

static void parse_inline(Md *md, MdNode *parent, const char *s, size_t len);
static void parse_blocks(Md *md, MdNode *parent, const char *s, size_t len,
                         bool in_quote);

static MdNode *node_new(Md *md, MdKind kind) {
  return CILO_NEW(md->arena, MdNode, .kind = kind);
}

static void append(MdNode *parent, MdNode *child) {
  if (parent->last != nullptr) {
    parent->last->next = child;
  }
  else {
    parent->first = child;
  }

  parent->last = child;
}

static MdNode *add(Md *md, MdNode *parent, MdKind kind) {
  MdNode *n = node_new(md, kind);
  append(parent, n);
  return n;
}

static void set_text(Md *md, MdNode *n, const char *s, size_t len) {
  n->text = cilo_strndup((cilo_allocator *)md->arena, s, len);
  n->len = (uint32_t)len;
}

static void push(Md *md, const char *s, size_t len) {
  if (md->pending_len + len > md->pending_cap) {
    size_t cap = md->pending_cap != 0 ? md->pending_cap * 2 : 256;

    while (cap < md->pending_len + len) {
      cap *= 2;
    }

    char *grown = realloc(md->pending, cap);

    if (grown == nullptr) {
      cilo_oom(cap);
    }

    md->pending = grown;
    md->pending_cap = cap;
  }

  memcpy(md->pending + md->pending_len, s, len);
  md->pending_len += len;
}

// turns what was pushed since `base` into a text node.
static void flush(Md *md, MdNode *parent, size_t base) {
  if (md->pending_len > base) {
    MdNode *n = add(md, parent, MD_TEXT);
    set_text(md, n, md->pending + base, md->pending_len - base);
    md->pending_len = base;
  }
}

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

static bool is_alnum(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_punct(char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
         (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static bool starts(const char *s, size_t len, size_t i, const char *word) {
  size_t n = strlen(word);
  return len - i >= n && memcmp(s + i, word, n) == 0;
}

static uint32_t utf8_decode(const char *s, size_t len, size_t *size) {
  unsigned char c = (unsigned char)s[0];
  uint32_t cp;
  size_t n;

  if (c < 0x80) {
    cp = c;
    n = 1;
  }
  else if ((c & 0xe0) == 0xc0) {
    cp = c & 0x1f;
    n = 2;
  }
  else if ((c & 0xf0) == 0xe0) {
    cp = c & 0x0f;
    n = 3;
  }
  else if ((c & 0xf8) == 0xf0) {
    cp = c & 0x07;
    n = 4;
  }
  else {
    *size = 1;
    return 0xfffd;
  }

  if (n > len) {
    *size = 1;
    return 0xfffd;
  }

  for (size_t i = 1; i < n; i++) {
    unsigned char next = (unsigned char)s[i];

    if ((next & 0xc0) != 0x80) {
      *size = 1;
      return 0xfffd;
    }

    cp = (cp << 6) | (next & 0x3f);
  }

  *size = n;
  return cp;
}

/* ========================================================================== */
/* unicode emoji.                                                             */
/* ========================================================================== */

typedef struct {
  uint32_t first, last;
} Range;

// drawn as emoji on their own.
static const Range emoji_ranges[] = {
  {0x231a, 0x231b},   {0x23e9, 0x23ec},   {0x23f0, 0x23f0},
  {0x23f3, 0x23f3},   {0x25fd, 0x25fe},   {0x2614, 0x2615},
  {0x2648, 0x2653},   {0x263a, 0x263a},   {0x267f, 0x267f},
  {0x2693, 0x2693},   {0x26a1, 0x26a1},   {0x26aa, 0x26ab},
  {0x26bd, 0x26be},   {0x26c4, 0x26c5},   {0x26ce, 0x26ce},
  {0x26d4, 0x26d4},   {0x26ea, 0x26ea},   {0x26f2, 0x26f3},
  {0x26f5, 0x26f5},   {0x26fa, 0x26fa},   {0x26fd, 0x26fd},
  {0x2705, 0x2705},   {0x270a, 0x270b},   {0x2728, 0x2728},
  {0x274c, 0x274c},   {0x274e, 0x274e},   {0x2753, 0x2755},
  {0x2757, 0x2757},   {0x2764, 0x2764},   {0x2795, 0x2797},
  {0x27b0, 0x27b0},   {0x27bf, 0x27bf},   {0x2b1b, 0x2b1c},
  {0x2b50, 0x2b50},   {0x2b55, 0x2b55},   {0x1f004, 0x1f004},
  {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a},
  {0x1f1e6, 0x1f1ff}, {0x1f201, 0x1f202}, {0x1f21a, 0x1f21a},
  {0x1f22f, 0x1f22f}, {0x1f232, 0x1f23a}, {0x1f250, 0x1f251},
  {0x1f300, 0x1f64f}, {0x1f680, 0x1f6ff}, {0x1f7e0, 0x1f7eb},
  {0x1f7f0, 0x1f7f0}, {0x1f900, 0x1f9ff}, {0x1fa70, 0x1faff},
};

// drawn as text unless the emoji variation selector follows.
static const Range text_ranges[] = {
  {0xa9, 0xa9},       {0xae, 0xae},       {0x203c, 0x203c},
  {0x2049, 0x2049},   {0x2122, 0x2122},   {0x2139, 0x2139},
  {0x2194, 0x21aa},   {0x2300, 0x23ff},   {0x24c2, 0x24c2},
  {0x25aa, 0x25fe},   {0x2600, 0x27bf},   {0x2934, 0x2935},
  {0x2b05, 0x2b55},   {0x3030, 0x3030},   {0x303d, 0x303d},
  {0x3297, 0x3297},   {0x3299, 0x3299},   {0x1f170, 0x1f171},
  {0x1f17e, 0x1f17f}, {0x1f202, 0x1f202}, {0x1f237, 0x1f237},
};

static bool in_ranges(const Range *ranges, size_t count, uint32_t cp) {
  for (size_t i = 0; i < count; i++) {
    if (cp >= ranges[i].first && cp <= ranges[i].last) {
      return true;
    }
  }

  return false;
}

static uint32_t peek(const char *s, size_t len, size_t at, size_t *size) {
  if (at >= len) {
    *size = 0;
    return 0;
  }

  return utf8_decode(s + at, len - at, size);
}

// the bytes of the emoji starting at `s`, 0 when there is none.
static size_t emoji_length(const char *s, size_t len) {
  size_t size, at = 0;
  uint32_t cp = peek(s, len, 0, &size);

  // a keycap: a digit, # or *, then the selector and the enclosing key.
  if (cp < 0x80) {
    bool key = is_digit((char)cp) || cp == '#' || cp == '*';

    if (!key) {
      return 0;
    }

    size_t n;
    at = 1;

    if (peek(s, len, at, &n) == 0xfe0f) {
      at += n;
    }

    return peek(s, len, at, &n) == 0x20e3 ? at + n : 0;
  }

  for (;;) {
    bool emoji = in_ranges(emoji_ranges, countof(emoji_ranges), cp);
    bool text = in_ranges(text_ranges, countof(text_ranges), cp);

    if (!emoji && !text) {
      return 0;
    }

    at += size;

    size_t n;
    uint32_t next = peek(s, len, at, &n);

    if (next == 0xfe0f) {
      at += n;
      emoji = true;
      next = peek(s, len, at, &n);
    }

    if (!emoji) {
      return 0;
    }

    // two regional indicators are one flag.
    if (cp >= 0x1f1e6 && cp <= 0x1f1ff) {
      if (next >= 0x1f1e6 && next <= 0x1f1ff) {
        at += n;
      }

      return at;
    }

    if (next >= 0x1f3fb && next <= 0x1f3ff) {
      at += n;
      next = peek(s, len, at, &n);
    }

    // the tags spelling a flag's region.
    while (next >= 0xe0020 && next <= 0xe007f) {
      at += n;
      next = peek(s, len, at, &n);
    }

    if (next != 0x200d) {
      return at;
    }

    // a joiner glues the next emoji on, when one follows.
    size_t joined;
    uint32_t after = peek(s, len, at + n, &joined);
    bool more = in_ranges(emoji_ranges, countof(emoji_ranges), after) ||
                in_ranges(text_ranges, countof(text_ranges), after);

    if (!more) {
      return at;
    }

    at += n;
    cp = after;
    size = joined;
  }
}

/* ========================================================================== */
/* timestamps.                                                                */
/* ========================================================================== */

static void format_relative(char *out, size_t cap, int64_t time, int64_t now) {
  static const struct {
    const char *name;
    int64_t seconds;
  } units[] = {
    {"year", 31536000}, {"month", 2592000}, {"day", 86400},
    {"hour", 3600},     {"minute", 60},     {"second", 1},
  };

  int64_t diff = time - now;
  int64_t distance = diff < 0 ? -diff : diff;

  if (distance < 1) {
    snprintf(out, cap, "now");
    return;
  }

  for (size_t i = 0; i < countof(units); i++) {
    if (distance < units[i].seconds) {
      continue;
    }

    int64_t count = distance / units[i].seconds;
    const char *plural = count == 1 ? "" : "s";

    if (diff < 0) {
      snprintf(out, cap, "%" PRId64 " %s%s ago", count, units[i].name, plural);
    }
    else {
      snprintf(out, cap, "in %" PRId64 " %s%s", count, units[i].name, plural);
    }

    return;
  }
}

static void format_timestamp(char *out, size_t cap, int64_t time, char style,
                             int64_t now) {
  if (style == 'R') {
    format_relative(out, cap, time, now);
    return;
  }

  const char *format;

  switch (style) {
    case 't':
      format = "%H:%M";
      break;
    case 'T':
      format = "%H:%M:%S";
      break;
    case 'd':
      format = "%d/%m/%Y";
      break;
    case 'D':
      format = "%e %B %Y";
      break;
    case 'F':
      format = "%A, %e %B %Y at %H:%M";
      break;
    case 's':
      format = "%d/%m/%Y, %H:%M";
      break;
    case 'S':
      format = "%d/%m/%Y, %H:%M:%S";
      break;
    default:
      format = "%e %B %Y at %H:%M";
      break;
  }

  time_t seconds = (time_t)time;
  struct tm tm;

  if (localtime_r(&seconds, &tm) == nullptr ||
      strftime(out, cap, format, &tm) == 0) {
    snprintf(out, cap, "invalid date");
    return;
  }

  // %e pads the day with a space.
  if (out[0] == ' ') {
    memmove(out, out + 1, strlen(out));
  }

  char *padded = strstr(out, ",  ");

  if (padded != nullptr) {
    memmove(padded + 2, padded + 3, strlen(padded + 3) + 1);
  }
}

/* ========================================================================== */
/* inline nodes.                                                              */
/* ========================================================================== */

// where the delimiter `delim` closes after `from`, or len. a closing run
// longer than the delimiter closes at its end, so ***a*** nests properly.
static size_t find_close(const char *s, size_t len, size_t from,
                         const char *delim) {
  size_t n = strlen(delim);

  for (size_t i = from; i + n <= len; i++) {
    if (s[i] == '\\') {
      i++;
      continue;
    }

    if (memcmp(s + i, delim, n) != 0) {
      continue;
    }

    while (i + n < len && s[i + n] == delim[0]) {
      i++;
    }

    return i;
  }

  return len;
}

// the closing mark of *italic* or _italic_, or len. pairs of the mark belong
// to bold or underline and are stepped over.
static size_t find_single(const char *s, size_t len, size_t from, char mark) {
  for (size_t i = from; i < len; i++) {
    if (s[i] == '\\') {
      i++;
      continue;
    }

    if (s[i] != mark) {
      continue;
    }

    if (i + 1 < len && s[i + 1] == mark) {
      // a run of three closes on its last mark: the pair is nested bold.
      if (i + 2 < len && s[i + 2] == mark) {
        return i + 2;
      }

      size_t close = find_close(s, len, i + 2, mark == '*' ? "**" : "__");

      if (close >= len) {
        return len;
      }

      i = close + 1;
      continue;
    }

    if (i > from && !is_space(s[i - 1])) {
      if (mark == '_' && i + 1 < len && is_alnum(s[i + 1])) {
        continue;
      }

      return i;
    }
  }

  return len;
}

static bool wrap(Md *md, MdNode *parent, size_t base, MdKind kind,
                 const char *s, size_t len, size_t *i, const char *delim) {
  size_t n = strlen(delim);
  size_t open = *i + n;
  size_t close = find_close(s, len, open, delim);

  if (close >= len || close == open) {
    return false;
  }

  flush(md, parent, base);
  parse_inline(md, add(md, parent, kind), s + open, close - open);
  *i = close + n;
  return true;
}

static bool wrap_single(Md *md, MdNode *parent, size_t base, const char *s,
                        size_t len, size_t *i, char mark) {
  size_t open = *i + 1;

  if (open >= len || is_space(s[open])) {
    return false;
  }

  // snake_case is not italic: the mark has to open a word.
  if (mark == '_' && *i > 0 && is_alnum(s[*i - 1])) {
    return false;
  }

  size_t close = find_single(s, len, open, mark);

  if (close >= len || close == open) {
    return false;
  }

  flush(md, parent, base);
  parse_inline(md, add(md, parent, MD_ITALIC), s + open, close - open);
  *i = close + 1;
  return true;
}

static bool lang_char(char c) {
  return is_alnum(c) || c == '_' || c == '+' || c == '-' || c == '.' ||
         c == '#';
}

static bool code(Md *md, MdNode *parent, size_t base, const char *s, size_t len,
                 size_t *i) {
  size_t ticks = 0;

  while (*i + ticks < len && s[*i + ticks] == '`') {
    ticks++;
  }

  if (ticks >= 3) {
    size_t open = *i + 3;
    const char *close = nullptr;

    for (size_t j = open; j + 3 <= len; j++) {
      if (memcmp(s + j, "```", 3) == 0) {
        close = s + j;
        break;
      }
    }

    if (close == nullptr || close == s + open) {
      return false;
    }

    const char *body = s + open;
    size_t body_len = (size_t)(close - body);
    const char *newline = memchr(body, '\n', body_len);
    const char *lang = nullptr;
    size_t lang_len = 0;

    // a first line made of one word names the language.
    if (newline != nullptr && newline > body) {
      bool word = true;

      for (const char *c = body; c < newline; c++) {
        word = word && lang_char(*c);
      }

      if (word && (size_t)(newline - body) < body_len - 1) {
        lang = body;
        lang_len = (size_t)(newline - body);
      }
    }

    if (lang != nullptr) {
      body_len -= lang_len + 1;
      body = newline + 1;
    }
    else if (body_len > 0 && body[0] == '\n') {
      body++;
      body_len--;
    }

    while (body_len > 0 && body[body_len - 1] == '\n') {
      body_len--;
    }

    flush(md, parent, base);

    MdNode *n = add(md, parent, MD_CODE_BLOCK);
    set_text(md, n, body, body_len);

    if (lang != nullptr) {
      n->lang = cilo_strndup((cilo_allocator *)md->arena, lang, lang_len);
    }

    *i = (size_t)(close - s) + 3;
    return true;
  }

  size_t open = *i + ticks;
  size_t close = len;

  for (size_t j = open; j + ticks <= len; j++) {
    if (s[j] != '`') {
      continue;
    }

    size_t run = 0;

    while (j + run < len && s[j + run] == '`') {
      run++;
    }

    if (run == ticks) {
      close = j;
      break;
    }

    j += run - 1;
  }

  if (close >= len || close == open) {
    return false;
  }

  const char *body = s + open;
  size_t body_len = close - open;

  // `` code `` drops the spaces that let it hold a backtick.
  if (ticks == 2 && body_len > 2 && body[0] == ' ' &&
      body[body_len - 1] == ' ') {
    body++;
    body_len -= 2;
  }

  flush(md, parent, base);
  set_text(md, add(md, parent, MD_CODE), body, body_len);
  *i = close + ticks;
  return true;
}

static bool url_scheme(const char *s, size_t len, size_t i) {
  return starts(s, len, i, "https://") || starts(s, len, i, "http://");
}

// the end of the bare url starting at `i`.
static size_t url_end(const char *s, size_t len, size_t i) {
  size_t end = i;
  int parens = 0;

  while (end < len && !is_space(s[end]) && s[end] != '<' && s[end] != '>') {
    if (s[end] == '(') {
      parens++;
    }
    else if (s[end] == ')') {
      if (parens == 0) {
        break;
      }

      parens--;
    }

    end++;
  }

  // punctuation ending a sentence is not part of the address.
  while (end > i) {
    char c = s[end - 1];
    bool trailing = c == '.' || c == ',' || c == ':' || c == ';' || c == '"' ||
                    c == '\'' || c == '!' || c == '?' || c == ']' || c == '*' ||
                    c == '_' || c == '~' || c == '|';

    if (!trailing) {
      break;
    }

    end--;
  }

  return end;
}

static void add_link(Md *md, MdNode *parent, const char *url, size_t len) {
  MdNode *link = add(md, parent, MD_LINK);
  set_text(md, link, url, len);
  set_text(md, add(md, link, MD_TEXT), url, len);
}

static bool masked_link(Md *md, MdNode *parent, size_t base, const char *s,
                        size_t len, size_t *i) {
  size_t label = *i + 1;
  size_t close = label;
  int depth = 0;

  for (; close < len; close++) {
    if (s[close] == '\\') {
      close++;
    }
    else if (s[close] == '[') {
      depth++;
    }
    else if (s[close] == ']') {
      if (depth == 0) {
        break;
      }

      depth--;
    }
    else if (s[close] == '\n') {
      return false;
    }
  }

  if (close + 1 >= len || close == label || s[close + 1] != '(') {
    return false;
  }

  size_t url = close + 2;
  bool angled = url < len && s[url] == '<';

  if (angled) {
    url++;
  }

  if (!url_scheme(s, len, url)) {
    return false;
  }

  size_t end = url;
  int parens = 0;

  while (end < len && !is_space(s[end])) {
    if (s[end] == '(') {
      parens++;
    }
    else if (s[end] == ')') {
      if (parens == 0) {
        break;
      }

      parens--;
    }
    else if (angled && s[end] == '>') {
      break;
    }

    end++;
  }

  size_t after = end;

  if (angled) {
    if (after >= len || s[after] != '>') {
      return false;
    }

    after++;
  }

  // an optional title in quotes, dropped.
  if (after < len && s[after] == ' ') {
    size_t quote = after + 1;

    if (quote < len && s[quote] == '"') {
      const char *closing = memchr(s + quote + 1, '"', len - quote - 1);

      if (closing != nullptr) {
        after = (size_t)(closing - s) + 1;
      }
    }
  }

  if (after >= len || s[after] != ')' || end == url) {
    return false;
  }

  flush(md, parent, base);

  MdNode *link = add(md, parent, MD_LINK);
  set_text(md, link, s + url, end - url);
  parse_inline(md, link, s + label, close - label);

  *i = after + 1;
  return true;
}

// the id between `from` and the closing bracket, 0 when it is not one.
static Snowflake bracket_id(const char *s, size_t len, size_t from,
                            size_t *end) {
  size_t at = from;

  while (at < len && is_digit(s[at])) {
    at++;
  }

  if (at == from || at >= len || s[at] != '>') {
    return 0;
  }

  *end = at + 1;
  return snowflake_parse_n(s + from, at - from);
}

static bool angle(Md *md, MdNode *parent, size_t base, const char *s,
                  size_t len, size_t *i) {
  size_t at = *i + 1;
  size_t end = 0;

  if (at >= len) {
    return false;
  }

  if (s[at] == '@' || s[at] == '#') {
    MdKind kind = s[at] == '#' ? MD_MENTION_CHANNEL : MD_MENTION_USER;
    size_t from = at + 1;

    if (kind == MD_MENTION_USER && from < len) {
      if (s[from] == '&') {
        kind = MD_MENTION_ROLE;
        from++;
      }
      else if (s[from] == '!') {
        from++;
      }
    }

    Snowflake id = bracket_id(s, len, from, &end);

    if (id == 0) {
      return false;
    }

    flush(md, parent, base);
    add(md, parent, kind)->id = id;
    *i = end;
    return true;
  }

  if (s[at] == ':' || starts(s, len, at, "a:")) {
    bool animated = s[at] == 'a';
    size_t name = at + (animated ? 2 : 1);
    size_t colon = name;

    while (colon < len && (is_alnum(s[colon]) || s[colon] == '_')) {
      colon++;
    }

    if (colon == name || colon >= len || s[colon] != ':') {
      return false;
    }

    Snowflake id = bracket_id(s, len, colon + 1, &end);

    if (id == 0) {
      return false;
    }

    flush(md, parent, base);

    MdNode *n = add(md, parent, MD_CUSTOM_EMOJI);
    n->id = id;
    n->animated = animated;
    set_text(md, n, s + name, colon - name);
    md->doc->emoji_count++;

    *i = end;
    return true;
  }

  if (starts(s, len, at, "t:")) {
    size_t from = at + 2;
    size_t digits = from;

    if (digits < len && s[digits] == '-') {
      digits++;
    }

    size_t first = digits;

    while (digits < len && is_digit(s[digits])) {
      digits++;
    }

    if (digits == first || digits - from > 15 || digits >= len) {
      return false;
    }

    char style = 'f';
    size_t close = digits;

    if (s[close] == ':' && close + 2 < len &&
        strchr("tTdDfFsSR", s[close + 1]) != nullptr) {
      style = s[close + 1];
      close += 2;
    }

    if (close >= len || s[close] != '>') {
      return false;
    }

    char number[20] = {};
    memcpy(number, s + from, digits - from);

    int64_t time = strtoll(number, nullptr, 10);
    char text[96];
    format_timestamp(text, sizeof text, time, style, md->now);

    flush(md, parent, base);

    MdNode *n = add(md, parent, MD_TIMESTAMP);
    n->time = time;
    n->style = style;
    set_text(md, n, text, strlen(text));

    *i = close + 1;
    return true;
  }

  if (url_scheme(s, len, at)) {
    size_t close = at;

    while (close < len && !is_space(s[close]) && s[close] != '>') {
      close++;
    }

    if (close >= len || s[close] != '>') {
      return false;
    }

    flush(md, parent, base);
    add_link(md, parent, s + at, close - at);
    *i = close + 1;
    return true;
  }

  return false;
}

static void parse_inline(Md *md, MdNode *parent, const char *s, size_t len) {
  size_t base = md->pending_len;

  if (md->depth >= MD_MAX_DEPTH) {
    push(md, s, len);
    flush(md, parent, base);
    return;
  }

  md->depth++;

  size_t i = 0;

  while (i < len) {
    char c = s[i];

    if (c == '\\' && i + 1 < len && is_punct(s[i + 1])) {
      push(md, s + i + 1, 1);
      i += 2;
      continue;
    }

    if (c == '\n') {
      flush(md, parent, base);
      add(md, parent, MD_LINE_BREAK);
      i++;
      continue;
    }

    if (c == '\r') {
      i++;
      continue;
    }

    bool taken = false;

    switch (c) {
      case '`':
        taken = code(md, parent, base, s, len, &i);

        // backticks that open nothing are text, all of them at once.
        if (!taken) {
          while (i < len && s[i] == '`') {
            push(md, s + i, 1);
            i++;
          }

          continue;
        }

        break;
      case '*':
        if (starts(s, len, i, "**")) {
          taken = wrap(md, parent, base, MD_BOLD, s, len, &i, "**");
        }

        if (!taken) {
          taken = wrap_single(md, parent, base, s, len, &i, '*');
        }

        break;
      case '_':
        if (starts(s, len, i, "__")) {
          taken = wrap(md, parent, base, MD_UNDERLINE, s, len, &i, "__");
        }

        if (!taken) {
          taken = wrap_single(md, parent, base, s, len, &i, '_');
        }

        break;
      case '~':
        if (starts(s, len, i, "~~")) {
          taken = wrap(md, parent, base, MD_STRIKETHROUGH, s, len, &i, "~~");
        }

        break;
      case '|':
        if (starts(s, len, i, "||")) {
          taken = wrap(md, parent, base, MD_SPOILER, s, len, &i, "||");
        }

        break;
      case '[':
        taken = masked_link(md, parent, base, s, len, &i);
        break;
      case '<':
        taken = angle(md, parent, base, s, len, &i);
        break;
      case '@':
        if (starts(s, len, i, "@everyone")) {
          flush(md, parent, base);
          add(md, parent, MD_MENTION_EVERYONE);
          i += 9;
          taken = true;
        }
        else if (starts(s, len, i, "@here")) {
          flush(md, parent, base);
          add(md, parent, MD_MENTION_HERE);
          i += 5;
          taken = true;
        }

        break;
      case 'h':
        if (url_scheme(s, len, i) && (i == 0 || !is_alnum(s[i - 1]))) {
          size_t end = url_end(s, len, i);
          size_t host = i + (s[i + 4] == 's' ? 8 : 7);

          if (end > host) {
            flush(md, parent, base);
            add_link(md, parent, s + i, end - i);
            i = end;
            taken = true;
          }
        }

        break;
      default:
        break;
    }

    if (taken) {
      continue;
    }

    bool maybe_emoji =
      (unsigned char)c >= 0x80 || is_digit(c) || c == '#' || c == '*';

    if (maybe_emoji) {
      size_t n = emoji_length(s + i, len - i);

      if (n > 0) {
        flush(md, parent, base);
        set_text(md, add(md, parent, MD_EMOJI), s + i, n);
        md->doc->emoji_count++;
        i += n;
        continue;
      }
    }

    // a whole character at once, so a flush never splits one.
    size_t size = 1;

    if ((unsigned char)c >= 0x80) {
      utf8_decode(s + i, len - i, &size);
    }

    push(md, s + i, size);
    i += size;
  }

  flush(md, parent, base);
  md->depth--;
}

/* ========================================================================== */
/* blocks.                                                                    */
/* ========================================================================== */

typedef struct {
  const char *s;
  size_t len;
} Line;

typedef struct {
  bool ordered;
  size_t indent;
  // where the item's text starts in the line.
  size_t content;
  uint32_t number;
} ListMark;

static bool list_mark(Line line, ListMark *mark) {
  size_t i = 0;

  while (i < line.len && line.s[i] == ' ') {
    i++;
  }

  *mark = (ListMark){.indent = i};

  if (i >= line.len) {
    return false;
  }

  if (line.s[i] == '-' || line.s[i] == '*') {
    i++;
  }
  else if (is_digit(line.s[i])) {
    size_t first = i;

    while (i < line.len && is_digit(line.s[i]) && i - first < 9) {
      mark->number = mark->number * 10 + (uint32_t)(line.s[i] - '0');
      i++;
    }

    if (i >= line.len || line.s[i] != '.') {
      return false;
    }

    mark->ordered = true;
    i++;
  }
  else {
    return false;
  }

  if (i >= line.len || line.s[i] != ' ') {
    return false;
  }

  while (i < line.len && line.s[i] == ' ') {
    i++;
  }

  mark->content = i;
  return i < line.len;
}

// 0 when the line is no heading.
static uint8_t heading_level(Line line, size_t *content) {
  size_t i = 0;

  while (i < line.len && line.s[i] == '#') {
    i++;
  }

  if (i == 0 || i > 3 || i >= line.len || line.s[i] != ' ') {
    return 0;
  }

  size_t at = i;

  while (at < line.len && line.s[at] == ' ') {
    at++;
  }

  if (at >= line.len) {
    return 0;
  }

  *content = at;
  return (uint8_t)i;
}

static bool subtext(Line line, size_t *content) {
  if (line.len < 4 || memcmp(line.s, "-# ", 3) != 0) {
    return false;
  }

  size_t at = 3;

  while (at < line.len && line.s[at] == ' ') {
    at++;
  }

  *content = at;
  return at < line.len;
}

// the items of `lines` from `*at` on that are indented `indent` or deeper.
static void parse_list(Md *md, MdNode *parent, const Line *lines, size_t count,
                       size_t *at, size_t indent, int depth) {
  ListMark first;
  list_mark(lines[*at], &first);

  MdNode *list = add(md, parent, MD_LIST);
  list->ordered = first.ordered;
  list->start = first.ordered ? first.number : 0;

  MdNode *item = nullptr;

  while (*at < count) {
    ListMark mark;

    if (!list_mark(lines[*at], &mark) || mark.indent < indent) {
      break;
    }

    bool deeper = mark.indent >= indent + 2;

    if (deeper && item != nullptr && depth < 10) {
      parse_list(md, item, lines, count, at, mark.indent, depth + 1);
      continue;
    }

    Line line = lines[*at];
    item = add(md, list, MD_LIST_ITEM);
    parse_inline(md, item, line.s + mark.content, line.len - mark.content);
    (*at)++;
  }
}

// the inline nodes of a run of plain lines. code blocks found in it become
// blocks of `parent`, the text around them paragraphs.
static void parse_paragraph(Md *md, MdNode *parent, const char *s, size_t len) {
  if (s == nullptr) {
    return;
  }

  while (len > 0 && s[len - 1] == '\n') {
    len--;
  }

  while (len > 0 && s[0] == '\n') {
    s++;
    len--;
  }

  if (len == 0) {
    return;
  }

  MdNode holder = {};
  parse_inline(md, &holder, s, len);

  MdNode *paragraph = nullptr;
  MdNode *n = holder.first;

  while (n != nullptr) {
    MdNode *next = n->next;
    n->next = nullptr;

    if (n->kind == MD_CODE_BLOCK) {
      append(parent, n);
      paragraph = nullptr;

      // the line break that only ends the fence's line.
      if (next != nullptr && next->kind == MD_LINE_BREAK) {
        next = next->next;
      }
    }
    else {
      bool break_before_code = n->kind == MD_LINE_BREAK && next != nullptr &&
                               next->kind == MD_CODE_BLOCK;

      if (!break_before_code) {
        if (paragraph == nullptr) {
          paragraph = add(md, parent, MD_PARAGRAPH);
        }

        append(paragraph, n);
      }
    }

    n = next;
  }
}

static size_t count_fences(Line line) {
  size_t fences = 0;

  for (size_t i = 0; i + 3 <= line.len; i++) {
    if (memcmp(line.s + i, "```", 3) == 0) {
      fences++;
      i += 2;
    }
  }

  return fences;
}

static void parse_blocks(Md *md, MdNode *parent, const char *s, size_t len,
                         bool in_quote) {
  size_t count = 1;

  for (size_t i = 0; i < len; i++) {
    count += s[i] == '\n';
  }

  Line *lines = CILO_NEW_ARRAY(md->arena, Line, count);
  size_t n = 0, from = 0;

  for (size_t i = 0; i <= len; i++) {
    if (i == len || s[i] == '\n') {
      size_t end = i;

      if (end > from && s[end - 1] == '\r') {
        end--;
      }

      lines[n++] = (Line){s + from, end - from};
      from = i + 1;
    }
  }

  // the plain lines gathered so far, from their first byte to the last.
  const char *run = nullptr;
  size_t run_len = 0;
  bool fenced = false;
  size_t at = 0;

  while (at < n) {
    Line line = lines[at];
    size_t content;

    if (!fenced) {
      MdNode *block = nullptr;
      bool quote_all =
        !in_quote && line.len >= 4 && memcmp(line.s, ">>> ", 4) == 0;
      bool quote = !in_quote && line.len >= 2 && memcmp(line.s, "> ", 2) == 0;
      ListMark mark;
      uint8_t level;

      if (quote_all || quote) {
        parse_paragraph(md, parent, run, run_len);
        run = nullptr;

        block = add(md, parent, MD_QUOTE);

        if (quote_all) {
          const char *rest = line.s + 4;
          parse_blocks(md, block, rest, (size_t)(s + len - rest), true);
          return;
        }

        // the lines quoted in a row, their marks dropped.
        size_t start = md->pending_len;

        while (at < n && lines[at].len >= 2 &&
               memcmp(lines[at].s, "> ", 2) == 0) {
          push(md, lines[at].s + 2, lines[at].len - 2);
          push(md, "\n", 1);
          at++;
        }

        size_t quoted_len = md->pending_len - start - 1;
        char *quoted = cilo_strndup((cilo_allocator *)md->arena,
                                    md->pending + start, quoted_len);
        md->pending_len = start;

        parse_blocks(md, block, quoted, quoted_len, true);
        continue;
      }

      if ((level = heading_level(line, &content)) != 0) {
        block = node_new(md, MD_HEADING);
        block->level = level;
      }
      else if (subtext(line, &content)) {
        block = node_new(md, MD_SUBTEXT);
      }

      if (block != nullptr) {
        parse_paragraph(md, parent, run, run_len);
        run = nullptr;

        append(parent, block);
        parse_inline(md, block, line.s + content, line.len - content);
        at++;
        continue;
      }

      if (list_mark(line, &mark)) {
        parse_paragraph(md, parent, run, run_len);
        run = nullptr;

        parse_list(md, parent, lines, n, &at, mark.indent, 0);
        continue;
      }
    }

    if (count_fences(line) % 2 == 1) {
      fenced = !fenced;
    }

    if (run == nullptr) {
      run = line.s;
    }

    run_len = (size_t)(line.s + line.len - run);
    at++;
  }

  parse_paragraph(md, parent, run, run_len);
}

static bool only_emoji(const MdNode *paragraph) {
  for (const MdNode *n = paragraph->first; n != nullptr; n = n->next) {
    if (n->kind == MD_EMOJI || n->kind == MD_CUSTOM_EMOJI ||
        n->kind == MD_LINE_BREAK) {
      continue;
    }

    if (n->kind != MD_TEXT) {
      return false;
    }

    for (uint32_t i = 0; i < n->len; i++) {
      if (!is_space(n->text[i])) {
        return false;
      }
    }
  }

  return true;
}

MdDoc *md_parse(const char *text, size_t len, const MdOptions *options) {
  MdDoc *doc = calloc(1, sizeof *doc);

  if (doc == nullptr) {
    cilo_oom(sizeof *doc);
  }

  Md md = {
    .doc = doc,
    .arena = &doc->arena,
    .now = options != nullptr && options->now != 0 ? options->now
                                                   : (int64_t)time(nullptr),
  };

  doc->root = node_new(&md, MD_ROOT);

  if (text == nullptr) {
    len = 0;
  }

  while (len > 0 && is_space(text[len - 1])) {
    len--;
  }

  while (len > 0 && (text[0] == '\n' || text[0] == '\r')) {
    text++;
    len--;
  }

  if (len > 0) {
    if (options != nullptr && options->inline_only) {
      parse_inline(&md, add(&md, doc->root, MD_PARAGRAPH), text, len);
    }
    else {
      parse_blocks(&md, doc->root, text, len, false);
    }
  }

  const MdNode *first = doc->root->first;
  doc->jumbo = first != nullptr && first->next == nullptr &&
               first->kind == MD_PARAGRAPH && doc->emoji_count > 0 &&
               doc->emoji_count <= MD_JUMBO_MAX && only_emoji(first);

  free(md.pending);
  return doc;
}

void md_free(MdDoc *doc) {
  if (doc == nullptr) {
    return;
  }

  cilo_arena_drop(&doc->arena);
  free(doc);
}

typedef struct {
  char *data;
  size_t len, cap;
} Plain;

static void plain_put(Plain *p, const char *s, size_t len) {
  if (p->len + len + 1 > p->cap) {
    size_t cap = p->cap != 0 ? p->cap * 2 : 128;

    while (cap < p->len + len + 1) {
      cap *= 2;
    }

    char *grown = realloc(p->data, cap);

    if (grown == nullptr) {
      cilo_oom(cap);
    }

    p->data = grown;
    p->cap = cap;
  }

  memcpy(p->data + p->len, s, len);
  p->len += len;
  p->data[p->len] = '\0';
}

static void plain_node(Plain *p, const MdNode *n) {
  char text[40];

  switch (n->kind) {
    case MD_TEXT:
    case MD_CODE:
    case MD_CODE_BLOCK:
    case MD_EMOJI:
    case MD_TIMESTAMP:
      plain_put(p, n->text, n->len);
      break;
    case MD_LINE_BREAK:
      plain_put(p, " ", 1);
      break;
    case MD_MENTION_USER:
      snprintf(text, sizeof text, "<@%" PRIu64 ">", n->id);
      plain_put(p, text, strlen(text));
      break;
    case MD_MENTION_ROLE:
      snprintf(text, sizeof text, "<@&%" PRIu64 ">", n->id);
      plain_put(p, text, strlen(text));
      break;
    case MD_MENTION_CHANNEL:
      snprintf(text, sizeof text, "<#%" PRIu64 ">", n->id);
      plain_put(p, text, strlen(text));
      break;
    case MD_MENTION_EVERYONE:
      plain_put(p, "@everyone", 9);
      break;
    case MD_MENTION_HERE:
      plain_put(p, "@here", 5);
      break;
    case MD_CUSTOM_EMOJI:
      plain_put(p, ":", 1);
      plain_put(p, n->text, n->len);
      plain_put(p, ":", 1);
      break;
    default:
      break;
  }

  for (const MdNode *child = n->first; child != nullptr; child = child->next) {
    plain_node(p, child);

    // blocks read as one line, a space between them.
    bool block =
      n->kind == MD_ROOT || n->kind == MD_QUOTE || n->kind == MD_LIST;

    if (block && child->next != nullptr) {
      plain_put(p, " ", 1);
    }
  }
}

char *md_plain_text(const MdDoc *doc) {
  Plain p = {};
  plain_put(&p, "", 0);

  if (doc != nullptr && doc->root != nullptr) {
    plain_node(&p, doc->root);
  }

  return p.data;
}
