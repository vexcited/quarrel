#include "discord/expressions.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cilo/cilo.h>

#include "discord/emoji.h"

static void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count != 0 ? count : 1, size);

  if (p == nullptr) {
    cilo_oom(count * size);
  }

  return p;
}

/* ========================================================================== */
/* who may use which.                                                         */
/* ========================================================================== */

// every nitro there is lets emojis go anywhere and move.
static bool premium(const Store *s) {
  const User *self = store_user(s, s->self_id);

  return self != nullptr && self->premium_type != 0;
}

// an emoji kept for some roles is no one else's, as the client's isUsable.
static bool role_allows(const Store *s, const Guild *owner, const Emoji *e) {
  if (e->nroles == 0) {
    return true;
  }

  const Member *me = store_member(owner, s->self_id);

  for (size_t i = 0; me != nullptr && i < me->nroles; i++) {
    for (size_t j = 0; j < e->nroles; j++) {
      if (me->roles[i] == e->roles[j]) {
        return true;
      }
    }
  }

  return false;
}

// the client's getEmojiUnavailableReason, in its order.
EmojiAccess guild_emoji_access(Store *s, const Guild *owner, const Emoji *e,
                               const Channel *c) {
  if (!role_allows(s, owner, e)) {
    return EMOJI_HIDDEN;
  }

  // in a dm no guild is the channel's, so every emoji comes from elsewhere.
  bool own = c != nullptr && c->guild_id != 0 && c->guild_id == owner->id;

  if (!own && c != nullptr && c->guild_id != 0 &&
      !store_can(s, c, PERM_USE_EXTERNAL_EMOJIS)) {
    return EMOJI_HIDDEN;
  }

  if (!e->available) {
    return EMOJI_NEEDS_BOOSTS;
  }

  bool paid = premium(s);

  if (!own && !paid && !e->managed) {
    return EMOJI_NEEDS_PREMIUM;
  }

  return e->animated && !paid ? EMOJI_NEEDS_PREMIUM : EMOJI_USABLE;
}

bool guild_emoji_native(Store *s, const Guild *owner, const Emoji *e,
                        const Channel *c) {
  if (!e->available) {
    return false;
  }

  bool own = c != nullptr && c->guild_id != 0 && c->guild_id == owner->id;
  // one a twitch or youtube subscription gave goes anywhere.
  bool given = e->managed && e->nroles != 0 && role_allows(s, owner, e);

  if (!premium(s) && !given) {
    return own && !e->animated;
  }

  return own || c == nullptr || c->guild_id == 0 ||
         store_can(s, c, PERM_USE_EXTERNAL_EMOJIS);
}

const Guild *guild_emoji_owner(Store *s, Snowflake emoji_id) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(s, &count);

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      if (store_emoji(folders[i].guilds[j], emoji_id)) {
        return folders[i].guilds[j];
      }
    }
  }

  return nullptr;
}

/* ========================================================================== */
/* names. the client gives the bare name to the first that has it: an emoji   */
/* of unicode, then the current guild's, then the other guilds' in the order  */
/* of the guild bar. the ones after get ~1, ~2 and so on.                     */
/* ========================================================================== */

typedef struct {
  const Store *store;
  Snowflake guild_id;
  uint64_t emojis_version, guilds_version;
  bool made;
  GuildEmoji *all;
  size_t count;
  // `all` in the order of the names and of the ids.
  uint32_t *by_name, *by_id;
  // the names that got a suffix.
  char *pool;
} Names;

// one is enough: only the account shown asks.
static Names names;

static int by_own_name(const void *a, const void *b) {
  const GuildEmoji *x = a, *y = b;
  int order = strcmp(x->emoji->name, y->emoji->name);

  return order != 0
           ? order
           : (x->emoji->id > y->emoji->id) - (x->emoji->id < y->emoji->id);
}

static int by_name_then_place(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  int order = strcmp(names.all[x].emoji->name, names.all[y].emoji->name);

  return order != 0 ? order : (x > y) - (x < y);
}

static int by_final_name(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

  return strcmp(names.all[x].name, names.all[y].name);
}

static int by_id(const void *a, const void *b) {
  Snowflake x = names.all[*(const uint32_t *)a].emoji->id;
  Snowflake y = names.all[*(const uint32_t *)b].emoji->id;

  return (x > y) - (x < y);
}

static size_t add_guild(Store *s, const Guild *g, size_t at) {
  size_t from = at;

  for (size_t i = 0; i < g->nemojis; i++) {
    const Emoji *e = &g->emojis[i];

    if (e->name != nullptr && *e->name != '\0' && role_allows(s, g, e)) {
      names.all[at++] = (GuildEmoji){.guild = g, .emoji = e, .name = e->name};
    }
  }

  qsort(names.all + from, at - from, sizeof *names.all, by_own_name);
  return at;
}

// an emoji of unicode goes by this name first.
static bool unicode_name(const char *name) {
  const UnicodeEmoji *emoji = emoji_by_name(name);

  return emoji != nullptr && strcmp(emoji->name, name) == 0;
}

static void make_names(Store *s, Snowflake guild_id) {
  size_t nfolders = 0, most = 0, bytes = 0;
  const GuildFolder *folders = store_guild_folders(s, &nfolders);
  const Guild *current = guild_id != 0 ? store_guild(s, guild_id) : nullptr;

  free(names.all);
  free(names.by_name);
  free(names.by_id);
  free(names.pool);

  for (size_t i = 0; i < nfolders; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      most += folders[i].guilds[j]->nemojis;
    }
  }

  most += current != nullptr ? current->nemojis : 0;

  names = (Names){
    .store = s,
    .guild_id = guild_id,
    .emojis_version = s->emojis_version,
    .guilds_version = s->guilds_version,
    .made = true,
    .all = xcalloc(most, sizeof *names.all),
    .by_name = xcalloc(most, sizeof *names.by_name),
    .by_id = xcalloc(most, sizeof *names.by_id),
  };

  size_t n = current != nullptr ? add_guild(s, current, 0) : 0;

  for (size_t i = 0; i < nfolders; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      if (folders[i].guilds[j] != current) {
        n = add_guild(s, folders[i].guilds[j], n);
      }
    }
  }

  names.count = n;

  for (size_t i = 0; i < n; i++) {
    names.by_name[i] = names.by_id[i] = (uint32_t)i;
    bytes += strlen(names.all[i].name) + 12;
  }

  // the holders of each name together, the first in line first.
  qsort(names.by_name, n, sizeof *names.by_name, by_name_then_place);
  names.pool = xcalloc(bytes, 1);

  char *pool = names.pool;

  for (size_t i = 0; i < n;) {
    const char *name = names.all[names.by_name[i]].emoji->name;
    uint32_t before = unicode_name(name);

    for (; i < n && !strcmp(names.all[names.by_name[i]].emoji->name, name);
         i++, before++) {
      if (before != 0) {
        names.all[names.by_name[i]].name = pool;
        pool += sprintf(pool, "%s~%" PRIu32, name, before) + 1;
      }
    }
  }

  qsort(names.by_name, n, sizeof *names.by_name, by_final_name);
  qsort(names.by_id, n, sizeof *names.by_id, by_id);
}

static void need_names(Store *s, Snowflake guild_id) {
  if (!names.made || names.store != s || names.guild_id != guild_id ||
      names.emojis_version != s->emojis_version ||
      names.guilds_version != s->guilds_version) {
    make_names(s, guild_id);
  }
}

const GuildEmoji *guild_emojis(Store *s, Snowflake guild_id, size_t *count) {
  need_names(s, guild_id);
  *count = names.count;
  return names.all;
}

const GuildEmoji *guild_emoji_by_name(Store *s, Snowflake guild_id,
                                      const char *name, size_t len) {
  need_names(s, guild_id);

  size_t lo = 0, hi = names.count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const GuildEmoji *at = &names.all[names.by_name[mid]];
    int order = strncmp(at->name, name, len);

    if (order == 0 && at->name[len] == '\0') {
      return at;
    }

    // the same start and more after it comes later.
    if (order < 0) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }

  return nullptr;
}

const GuildEmoji *guild_emoji_by_id(Store *s, Snowflake guild_id,
                                    Snowflake emoji_id) {
  need_names(s, guild_id);

  size_t lo = 0, hi = names.count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const GuildEmoji *at = &names.all[names.by_id[mid]];

    if (at->emoji->id == emoji_id) {
      return at;
    }

    if (at->emoji->id < emoji_id) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }

  return nullptr;
}

/* ========================================================================== */
/* what a message holds.                                                      */
/* ========================================================================== */

static bool word(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '_';
}

static bool blank(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

// the client's /^<a?:(\w+):(\d+)>/.
size_t emoji_token(const char *text, size_t len, Snowflake *id, bool *animated,
                   const char **name, size_t *name_len) {
  size_t i = 1;

  if (len < 6 || text[0] != '<') {
    return 0;
  }

  bool moves = text[i] == 'a';

  i += moves;

  if (text[i++] != ':') {
    return 0;
  }

  size_t from = i;

  while (i < len && word(text[i])) {
    i++;
  }

  size_t named = i - from;

  if (named == 0 || i >= len || text[i++] != ':') {
    return 0;
  }

  size_t digits = i;

  while (i < len && text[i] >= '0' && text[i] <= '9') {
    i++;
  }

  if (i == digits || i >= len || text[i] != '>') {
    return 0;
  }

  Snowflake found = snowflake_parse_n(text + digits, i - digits);

  if (found == 0) {
    return 0;
  }

  if (id != nullptr) {
    *id = found;
  }

  if (animated != nullptr) {
    *animated = moves;
  }

  if (name != nullptr) {
    *name = text + from;
    *name_len = named;
  }

  return i + 1;
}

// the client's /^:([^\s:]+?(?:::skin-tone-\d)?):/.
size_t emoji_colons(const char *text, size_t len, const char **name,
                    size_t *name_len, uint8_t *tone) {
  static const char TONE[] = "::skin-tone-";
  constexpr size_t TONE_LEN = sizeof TONE - 1;
  size_t i = 1;

  if (len < 3 || text[0] != ':') {
    return 0;
  }

  while (i < len && !blank(text[i]) && text[i] != ':') {
    i++;
  }

  if (i == 1 || i >= len || text[i] != ':') {
    return 0;
  }

  *name = text + 1;
  *name_len = i - 1;
  *tone = 0;

  if (len - i >= TONE_LEN + 2 && memcmp(text + i, TONE, TONE_LEN) == 0 &&
      text[i + TONE_LEN] >= '1' && text[i + TONE_LEN] <= '5' &&
      text[i + TONE_LEN + 1] == ':') {
    *tone = (uint8_t)(text[i + TONE_LEN] - '0');
    return i + TONE_LEN + 2;
  }

  return i + 1;
}

typedef struct {
  char *v;
  size_t len, cap;
} Out;

static void put(Out *out, const char *bytes, size_t n) {
  if (out->len + n + 1 > out->cap) {
    out->cap = (out->len + n + 1) * 2;
    out->v = realloc(out->v, out->cap);

    if (out->v == nullptr) {
      cilo_oom(out->cap);
    }
  }

  memcpy(out->v + out->len, bytes, n);
  out->len += n;
  out->v[out->len] = '\0';
}

// how many bytes of code start `text`: a run of backticks, what it holds
// and the same run after. 0 when it is never closed.
static size_t code_span(const char *text, size_t len) {
  size_t ticks = 0;

  while (ticks < len && text[ticks] == '`') {
    ticks++;
  }

  for (size_t i = ticks; i + ticks <= len; i++) {
    size_t run = 0;

    while (i + run < len && text[i + run] == '`') {
      run++;
    }

    if (run == ticks) {
      return i + run;
    }

    i += run;
  }

  return 0;
}

static size_t link(const char *text, size_t len) {
  size_t i = 0;

  if ((len < 8 || memcmp(text, "https://", 8) != 0) &&
      (len < 7 || memcmp(text, "http://", 7) != 0)) {
    return 0;
  }

  while (i < len && !blank(text[i])) {
    i++;
  }

  return i;
}

size_t expressions_literal(const char *text, size_t len) {
  if (len == 0) {
    return 0;
  }

  switch (text[0]) {
    case '\\':
      return len > 1 ? 2 : 0;
    case '`':
      return code_span(text, len);
    case '<':
      return emoji_token(text, len, nullptr, nullptr, nullptr, nullptr);
    case 'h':
      return link(text, len);
    default:
      return 0;
  }
}

// FakeNitro's link: the url the client gives the emoji at 48 pixels, with
// its name and lossless after it, under the emoji's name.
static void put_fake(Out *out, const Emoji *e, char after) {
  char link[256];
  // kept apart from the words around it, or it would be no link.
  bool before = out->len != 0 && !blank(out->v[out->len - 1]);
  int n = snprintf(link, sizeof link,
                   "%s[%s](https://cdn.discordapp.com/emojis/%" PRIu64
                   ".webp?size=48%s&name=%s&lossless=true)%s",
                   before ? " " : "", e->name, e->id,
                   e->animated ? "&animated=true" : "", e->name,
                   after != '\0' && !blank(after) ? " " : "");

  put(out, link, n < (int)sizeof link ? (size_t)n : sizeof link - 1);
}

static void put_token(Out *out, const Emoji *e) {
  char token[96];
  int n = snprintf(token, sizeof token, "<%s:%s:%" PRIu64 ">",
                   e->animated ? "a" : "", e->name, e->id);

  put(out, token, (size_t)n);
}

// `faked` is null where discord's own rules hold.
static char *encode(Store *s, const Channel *c, const char *text, bool *faked) {
  size_t len = strlen(text);
  Snowflake guild_id = c != nullptr ? c->guild_id : 0;
  Out out = {};

  put(&out, "", 0);

  for (size_t i = 0; i < len;) {
    const char *at = text + i, *name = nullptr;
    size_t rest = len - i, kept = expressions_literal(at, rest), name_len = 0;
    uint8_t tone = 0;
    Snowflake id = 0;

    if (faked != nullptr && *at == '<' && kept != 0 &&
        emoji_token(at, rest, &id, nullptr, nullptr, nullptr) != 0) {
      const GuildEmoji *held = guild_emoji_by_id(s, guild_id, id);

      if (held != nullptr &&
          !guild_emoji_native(s, held->guild, held->emoji, c)) {
        put_fake(&out, held->emoji, at[kept]);
        *faked = true;
        i += kept;
        continue;
      }
    }

    if (kept == 0 && *at == ':') {
      size_t taken = emoji_colons(at, rest, &name, &name_len, &tone);
      const GuildEmoji *named =
        taken ? guild_emoji_by_name(s, guild_id, name, name_len) : nullptr;
      const UnicodeEmoji *unicode =
        taken && !named ? emoji_by_colons(name, name_len, tone) : nullptr;

      if (named != nullptr && faked != nullptr &&
          !guild_emoji_native(s, named->guild, named->emoji, c)) {
        put_fake(&out, named->emoji, at[taken]);
        *faked = true;
        i += taken;
        continue;
      }

      if (named != nullptr &&
          (faked != nullptr || guild_emoji_access(s, named->guild, named->emoji,
                                                  c) == EMOJI_USABLE)) {
        put_token(&out, named->emoji);
        i += taken;
        continue;
      }

      if (unicode != nullptr) {
        put(&out, unicode->utf8, strlen(unicode->utf8));
        i += taken;
        continue;
      }
    }

    kept = kept != 0 ? kept : 1;
    put(&out, at, kept);
    i += kept;
  }

  return out.v;
}

char *expressions_encode(Store *s, const Channel *c, const char *text) {
  return encode(s, c, text, nullptr);
}

char *expressions_send(Store *s, const Channel *c, const char *text,
                       bool *faked) {
  *faked = false;
  return encode(s, c, text, faked);
}

/* ========================================================================== */
/* FakeNitro's links read back.                                               */
/* ========================================================================== */

static bool starts(const char *text, const char *with) {
  return strncmp(text, with, strlen(with)) == 0;
}

// its /\/emojis\/(\d+?)\.(png|webp|gif)/, then the name and animated the
// url carries.
bool fake_emoji_url(const char *url, Snowflake *id, bool *animated,
                    const char **name, size_t *name_len) {
  static const char PATH[] = "/emojis/";

  for (const char *at = url; (at = strstr(at, PATH)) != nullptr; at++) {
    const char *digits = at + sizeof PATH - 1, *end = digits;

    while (*end >= '0' && *end <= '9') {
      end++;
    }

    bool gif = starts(end, ".gif");

    if (end == digits ||
        !(gif || starts(end, ".png") || starts(end, ".webp")) ||
        (*id = snowflake_parse_n(digits, (size_t)(end - digits))) == 0) {
      continue;
    }

    *animated = gif;
    *name = nullptr;
    *name_len = 0;

    const char *query = strchr(end, '?');

    for (const char *q = query ? query + 1 : nullptr; q && *q && *q != '#';) {
      size_t n = strcspn(q, "&#");

      if (starts(q, "name=")) {
        *name = q + 5;
        *name_len = n - 5;
      }
      else if (n == 13 && starts(q, "animated=true")) {
        *animated = true;
      }

      q += n + (q[n] == '&');
    }

    return true;
  }

  return false;
}

// its hyperLinkOnlyRegex, /^\[.+?\]\((https?:\/\/.+?)\)$/, or one word.
bool fake_emoji_message(const char *content) {
  size_t len = strlen(content);
  const char *url = content;
  const char *name = nullptr;
  size_t name_len = 0;
  Snowflake id = 0;
  bool animated = false;

  if (content[0] == '[' && len != 0 && content[len - 1] == ')') {
    const char *open = strstr(content + 2, "](http");

    url = open != nullptr ? open + 2 : content;
  }
  else if (strpbrk(content, " \n\t\r") != nullptr) {
    return false;
  }

  return (starts(url, "https://") || starts(url, "http://")) &&
         fake_emoji_url(url, &id, &animated, &name, &name_len);
}
