#include "complete.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "discord/gateway.h"
#include "widgets.h"

// discord's client lists ten rows, users first, then the roles there is
// room left for.
constexpr size_t ROWS_MAX = 10;
// the names that only hold the query in its order fill the list, out of
// this many.
constexpr size_t FUZZY_MAX = 50;
// the members discord is asked for while a name is typed, once the typing
// rested this long, and once a minute for the same name.
constexpr uint32_t MEMBERS_ASKED = 10;
constexpr double MEMBERS_REST = 0.2;
constexpr double MEMBERS_KEEP = 60;

/* ========================================================================== */
/* names as discord's client compares them: in lower case, their accents     */
/* left out.                                                                  */
/* ========================================================================== */

// the letters of latin-1 from U+00C0 and of latin extended-a from U+0100
// without what they wear, a dot for the ones that are their own.
static const char LATIN_1[] =
  "aaaaaa.ceeeeiiii.nooooo.ouuuuy..aaaaaa.ceeeeiiii.nooooo.ouuuuy.y";
static const char LATIN_A[] =
  "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii..jjkkkllllllllll"
  "nnnnnnnnnoooooo..rrrrrrsssssssstttttt"
  "uuuuuuuuuuuuwwyyyzzzzzzs";

static_assert(sizeof LATIN_1 == 0x40 + 1 && sizeof LATIN_A == 0x80 + 1);

void mention_fold(const char *in, char *out, size_t cap) {
  size_t n = 0;

  for (const unsigned char *c = (const unsigned char *)(in ? in : "");
       *c && n + 4 < cap;) {
    if (*c < 0x80) {
      out[n++] = (char)tolower(*c++);
      continue;
    }

    // two bytes hold what U+0080 to U+07FF take.
    if ((*c & 0xe0) == 0xc0 && (c[1] & 0xc0) == 0x80) {
      uint32_t point = (uint32_t)(*c & 0x1f) << 6 | (c[1] & 0x3f);
      char base = point >= 0xc0 && point < 0x100    ? LATIN_1[point - 0xc0]
                  : point >= 0x100 && point < 0x180 ? LATIN_A[point - 0x100]
                                                    : '.';

      // a mark put over the letter before it.
      if (point >= 0x300 && point < 0x370) {
        c += 2;
        continue;
      }

      if (base != '.') {
        out[n++] = base;
        c += 2;
        continue;
      }
    }

    out[n++] = (char)*c++;

    while ((*c & 0xc0) == 0x80 && n + 1 < cap) {
      out[n++] = (char)*c++;
    }
  }

  out[n] = 0;
}

bool mention_fuzzy(const char *query, const char *text) {
  for (; *query; query++, text++) {
    while (*text && *text != *query) {
      text++;
    }

    if (!*text) {
      return false;
    }
  }

  return true;
}

// every word of `query`, cut at blanks and commas, is in `name`.
static bool holds_words(const char *name, const char *query) {
  char word[128];

  for (const char *at = query; *at;) {
    size_t len = strcspn(at, ", ");

    if (len && len < sizeof word) {
      memcpy(word, at, len);
      word[len] = 0;

      if (!strstr(name, word)) {
        return false;
      }
    }

    at += len + (at[len] != 0);
  }

  return true;
}

int mention_rank(const char *name, const char *query) {
  char folded[256];

  if (!*query) {
    return 7;
  }

  mention_fold(name, folded, sizeof folded);

  if (strncmp(folded, query, strlen(query)) == 0) {
    return folded[strlen(query)] ? 7 : 10;
  }

  return strstr(folded, query)          ? 5
         : holds_words(folded, query)   ? 3
         : mention_fuzzy(query, folded) ? 1
                                        : 0;
}

/* ========================================================================== */
/* the mentions the composer holds, shown as one chip each.                   */
/* ========================================================================== */

typedef enum : uint8_t {
  MENTION_NONE,
  MENTION_USER,
  MENTION_ROLE,
  MENTION_CHANNEL,
} MentionKind;

// the mention `text` starts with as discord writes them, <@id>, <@!id>,
// <@&id> and <#id>, and how many bytes it takes. MENTION_NONE for none.
static MentionKind mention_at(const char *text, uint32_t len, Snowflake *id,
                              uint32_t *taken) {
  MentionKind kind = MENTION_NONE;
  uint32_t i = 2;

  if (len < 4 || text[0] != '<') {
    return MENTION_NONE;
  }

  if (text[1] == '#') {
    kind = MENTION_CHANNEL;
  }
  else if (text[1] == '@') {
    kind = MENTION_USER;

    if (text[2] == '&') {
      kind = MENTION_ROLE;
      i = 3;
    }
    else if (text[2] == '!') {
      i = 3;
    }
  }
  else {
    return MENTION_NONE;
  }

  uint32_t digits = i;

  while (i < len && text[i] >= '0' && text[i] <= '9') {
    i++;
  }

  if (i == digits || i - digits > 20 || i >= len || text[i] != '>') {
    return MENTION_NONE;
  }

  *id = snowflake_parse_n(text + digits, i - digits);
  *taken = i + 1;
  return *id ? kind : MENTION_NONE;
}

// what a mention reads as, and the color of a role's. false for one of
// nothing the store knows.
static bool mention_label(App *app, MentionKind kind, Snowflake id, char *out,
                          size_t cap, uint32_t *color) {
  Store *store = app->session->store;
  const Channel *here = app_channel(app);
  Snowflake guild_id = here ? here->guild_id : 0;

  *color = 0;

  if (kind == MENTION_USER) {
    if (!store_user(store, id)) {
      return false;
    }

    snprintf(out, cap, "@%s", store_display_name(store, guild_id, id));
    return true;
  }

  if (kind == MENTION_ROLE) {
    const Guild *guild = store_guild(store, guild_id);
    const Role *role = guild ? store_role(guild, id) : nullptr;

    if (!role || !role->name) {
      return false;
    }

    *color = role->color;
    snprintf(out, cap, "@%s", role->name);
    return true;
  }

  const Channel *channel = store_channel(store, id);
  char name[128];

  if (!channel) {
    return false;
  }

  snprintf(out, cap, "#%s",
           store_channel_name(store, channel, name, sizeof name));
  return true;
}

static civue_text_style chip_style(void) {
  return civue_style(16, 500, 22, 0);
}

uint32_t mention_inlay_find(App *app, const char *text, uint32_t len,
                            uint32_t at, float *width) {
  Snowflake id = 0;
  uint32_t taken = 0, color;
  char label[160];

  if (at >= len || text[at] != '<') {
    return 0;
  }

  MentionKind kind = mention_at(text + at, len - at, &id, &taken);

  if (kind == MENTION_NONE ||
      !mention_label(app, kind, id, label, sizeof label, &color)) {
    return 0;
  }

  *width = civue_layout(civue_ui_current(), label, chip_style(), 0)->width + 4;
  return taken;
}

void mention_inlay_paint(App *app, civue_ui *ui, const char *text, uint32_t len,
                         civue_rect r) {
  const Look *k = &app->look;
  Snowflake id = 0;
  uint32_t taken = 0, color = 0;
  char label[160];
  MentionKind kind = mention_at(text, len, &id, &taken);

  if (kind == MENTION_NONE ||
      !mention_label(app, kind, id, label, sizeof label, &color)) {
    return;
  }

  civue_color ink = color ? rgb(color) : k->mention_text;
  civue_color ground = color ? civue_alpha(ink, .12f) : k->mention_bg;
  civue_rect chip = r;

  chip.y0 += 1;
  chip.y1 -= 1;
  civue_fill(ui, chip, ground, 3);
  civue_text_in(ui, label, chip_style(), r, ink, CIVUE_ALIGN_CENTER);
}

bool mention_whole(const char *text) {
  Snowflake id = 0;
  uint32_t taken = 0, len = (uint32_t)strlen(text);

  return mention_at(text, len, &id, &taken) != MENTION_NONE && taken == len;
}

/* ========================================================================== */
/* the members discord is asked for.                                          */
/* ========================================================================== */

typedef struct {
  Snowflake guild_id;
  uint64_t name;
  double at;
} Asked;

// the names asked lately, the oldest replaced, and the one waiting for the
// typing to rest.
static Asked asked[32];
static size_t asked_next;

static struct {
  Snowflake guild_id;
  char query[96];
  double at;
  bool waits;
} wanted;

static uint64_t name_hash(const char *name) {
  uint64_t h = 0xcbf29ce484222325ull;

  for (; *name; name++) {
    h = (h ^ (uint8_t)*name) * 0x100000001b3ull;
  }

  return h;
}

// has discord asked for the members a name starts, as the client does for
// every name typed in a guild: once a minute for each, and only the last
// of the ones typed in a row.
static void want_members(const Channel *c, const char *query) {
  double now = civue_ui_current()->now;
  uint64_t name = name_hash(query);

  if (!c->guild_id) {
    return;
  }

  for (size_t i = 0; i < countof(asked); i++) {
    if (asked[i].guild_id == c->guild_id && asked[i].name == name &&
        now - asked[i].at < MEMBERS_KEEP) {
      return;
    }
  }

  asked[asked_next++ % countof(asked)] = (Asked){c->guild_id, name, now};
  wanted.guild_id = c->guild_id;
  wanted.at = now;
  wanted.waits = true;
  snprintf(wanted.query, sizeof wanted.query, "%s", query);
}

void mention_ask_members(App *app) {
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  double left = wanted.at + MEMBERS_REST - ui->now;

  if (!wanted.waits) {
    return;
  }

  if (left > 0) {
    civue_request_repaint_after(ui, left);
    return;
  }

  wanted.waits = false;

  if (session->gateway && !session->offline) {
    gateway_search_members(session->gateway, wanted.guild_id, wanted.query,
                           MEMBERS_ASKED, true);
  }
}

/* ========================================================================== */
/* who to mention.                                                            */
/* ========================================================================== */

static bool blank_char(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

int32_t mention_token(const char *text, uint32_t caret, char opener) {
  uint32_t start = caret;

  // the word under the caret, which the opener starts, a parenthesis
  // before it left out.
  while (start > 0 && !blank_char(text[start - 1])) {
    start--;
  }

  for (uint32_t i = start; i < caret; i++) {
    if ((i == start || text[i - 1] == '(') && text[i] == opener) {
      return (int32_t)i;
    }
  }

  return -1;
}

int32_t mention_find(App *, const Channel *, const char *text, uint32_t caret) {
  return mention_token(text, caret, '@');
}

typedef struct {
  Snowflake id;
  int score;
  int32_t order;
  const char *name;
} Found;

static int by_score(const void *a, const void *b) {
  const Found *x = a, *y = b;

  if (x->score != y->score) {
    return y->score - x->score;
  }

  if (x->order != y->order) {
    return x->order < y->order ? -1 : 1;
  }

  return strcasecmp(x->name, y->name);
}

// keeps the `max` best of what was found.
static void found_add(Found *found, size_t *count, size_t max, Found one) {
  if (*count < max) {
    found[(*count)++] = one;
    return;
  }

  size_t worst = 0;

  for (size_t i = 1; i < max; i++) {
    if (by_score(&found[i], &found[worst]) > 0) {
      worst = i;
    }
  }

  if (by_score(&one, &found[worst]) < 0) {
    found[worst] = one;
  }
}

// 2 for a name the query starts, 1 for one that holds it in its order, 0
// for one that does not answer.
static int name_answers(const char *name, const char *query) {
  char folded[256];

  if (!name) {
    return 0;
  }

  mention_fold(name, folded, sizeof folded);

  return strncmp(folded, query, strlen(query)) == 0 ? 2
         : mention_fuzzy(query, folded)             ? 1
                                                    : 0;
}

static int best(int a, int b) {
  return a > b ? a : b;
}

// by their username, the name they chose, and the one they wear here or
// the user gave them.
static int user_answers(Store *store, const Member *member, const User *user,
                        const char *query) {
  const Relationship *known =
    member ? nullptr : store_relationship(store, user->id);
  int score = best(name_answers(user->username, query),
                   name_answers(user->global_name, query));

  if (member) {
    score = best(score, name_answers(member->nick, query));
  }

  if (known) {
    score = best(score, name_answers(known->nickname, query));
  }

  return score;
}

// whether a member may view the channel, a thread being its channel's.
static bool member_views(Store *store, const Guild *guild, const Channel *c,
                         const Member *member) {
  Snowflake ids[64];
  Permissions granted[64];
  size_t count = 0;
  const Role *everyone = store_role(guild, guild->id);
  const Channel *source = c;

  if (channel_is_thread(c)) {
    const Channel *parent = store_channel(store, c->parent_id);

    source = parent ? parent : c;
  }

  for (size_t i = 0; i < member->nroles && count < countof(ids); i++) {
    const Role *role = store_role(guild, member->roles[i]);

    if (role) {
      ids[count] = role->id;
      granted[count++] = role->permissions;
    }
  }

  PermissionSubject who = {
    .guild_id = guild->id,
    .owner_id = guild->owner_id,
    .user_id = member->user_id,
    .everyone = everyone ? everyone->permissions : 0,
    .role_ids = ids,
    .role_permissions = granted,
    .nroles = count,
  };
  Permissions perms = permissions_in_channel(
    &who, source->overwrites, source->noverwrites,
    channel_is_voice(source) ? PERM_CHANNEL_VOICE : PERM_CHANNEL_TEXT);

  return (perms & PERM_VIEW_CHANNEL) != 0;
}

static void user_row(App *app, const Channel *c, CompleteList *list,
                     Snowflake id) {
  Store *store = app->session->store;
  const User *user = store_user(store, id);
  uint32_t color = store_member_color(store, c->guild_id, id);
  char insert[40];

  snprintf(insert, sizeof insert, "<@%" PRIu64 "> ", id);
  complete_add(list, &(CompleteRow){
                       .icon = COMPLETE_ICON_USER,
                       .id = id,
                       .color = color ? rgb(color) : CIVUE_CLEAR,
                       .label = store_display_name(store, c->guild_id, id),
                       .detail = user->username,
                       .insert = insert,
                     });
}

// who wrote the messages loaded in the channel, the latest first, each
// once: what the client lists before anything is typed.
static size_t recent_authors(Store *store, const Guild *guild, const Channel *c,
                             Found *found, size_t max) {
  size_t count = 0;

  for (size_t i = c->nmessages; i-- > 0 && count < max;) {
    const Message *m = c->messages[i];
    const Member *member = store_member(guild, m->author_id);
    bool listed = false;

    for (size_t j = 0; j < count; j++) {
      listed |= found[j].id == m->author_id;
    }

    if (!listed && !m->webhook_id && member && !member->pending &&
        store_user(store, m->author_id)) {
      found[count] = (Found){.id = m->author_id, .order = (int32_t)count};
      count++;
    }
  }

  return count;
}

size_t mention_users(App *app, Channel *c, const char *typed,
                     CompleteList *list, size_t max) {
  Store *store = app->session->store;
  Guild *guild = store_guild(store, c->guild_id);
  Found found[ROWS_MAX], loose[FUZZY_MAX];
  size_t count = 0, nloose = 0;
  char query[128];

  mention_fold(typed, query, sizeof query);
  max = max < countof(found) ? max : countof(found);

  if (!max) {
    return 0;
  }

  if (guild && !*query) {
    count = recent_authors(store, guild, c, found, max);
  }

  if (guild && !count) {
    int32_t order = 0;

    CILO_MAP_EACH(entry, &guild->members) {
      const Member *member = entry->val;
      const User *user = store_user(store, member->user_id);
      int score =
        user && !member->pending ? user_answers(store, member, user, query) : 0;

      if (!score || !member_views(store, guild, c, member)) {
        continue;
      }

      Found one = {user->id, score, order++, store_user_name(user)};

      if (score == 2) {
        found_add(found, &count, max, one);
      }
      else if (nloose < countof(loose)) {
        loose[nloose++] = one;
      }
    }
  }
  else if (!guild) {
    for (size_t i = 0; i <= c->nrecipients && count < max; i++) {
      // the people of the conversation, then the user.
      Snowflake id = i < c->nrecipients ? c->recipients[i] : store->self_id;
      const User *user = store_user(store, id);
      int score = user ? user_answers(store, nullptr, user, query) : 0;

      if (score == 2) {
        found[count++] = (Found){id, 2, (int32_t)i, store_user_name(user)};
      }
      else if (score && nloose < countof(loose)) {
        loose[nloose++] = (Found){id, 1, (int32_t)i, store_user_name(user)};
      }
    }
  }

  // discord is asked in lower case, the accents as they were typed.
  if (guild && (*query || !count)) {
    char lower[128];
    size_t n = 0;

    for (; typed[n] && n + 1 < sizeof lower; n++) {
      lower[n] = (char)tolower((unsigned char)typed[n]);
    }

    lower[n] = 0;
    want_members(c, lower);
  }

  qsort(found, count, sizeof *found, by_score);
  qsort(loose, nloose, sizeof *loose, by_score);

  for (size_t i = 0; i < nloose && count < max; i++) {
    found[count++] = loose[i];
  }

  if (count) {
    complete_title(list, "MEMBERS");
  }

  for (size_t i = 0; i < count; i++) {
    user_row(app, c, list, found[i].id);
  }

  return count;
}

// how the client's sorter ranks a name against what was typed: the same, 7
// to the letter and 6 but for its case, 5 for one it starts, 4 for one a
// word of which it starts, 3 for one that holds it, 1 for its letters in
// their order.
static int sorter_rank(const char *name, const char *typed) {
  char folded[256], query[128];

  mention_fold(name, folded, sizeof folded);
  mention_fold(typed, query, sizeof query);

  if (strcmp(name, typed) == 0) {
    return 7;
  }

  if (strcmp(folded, query) == 0) {
    return 6;
  }

  if (strncmp(folded, query, strlen(query)) == 0) {
    return 5;
  }

  const char *at = strstr(folded, query);

  for (const char *hit = at; hit; hit = strstr(hit + 1, query)) {
    if (hit > folded && hit[-1] == ' ') {
      return 4;
    }
  }

  return at ? 3 : mention_fuzzy(query, folded) ? 1 : 0;
}

size_t mention_roles(App *app, Channel *c, const char *typed,
                     CompleteList *list, size_t max, bool any) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  bool all = any || store_can(store, c, PERM_MENTION_EVERYONE);
  Found found[ROWS_MAX];
  size_t count = 0;

  max = max < countof(found) ? max : countof(found);

  for (size_t i = 0; guild && max && i < guild->nroles; i++) {
    const Role *role = guild->roles[i];
    int score = role->name ? sorter_rank(role->name, typed) : 0;

    // @everyone is the role whose id is the guild's.
    if (score && role->id != guild->id && (all || role->mentionable)) {
      found_add(found, &count, max, (Found){role->id, score, 0, role->name});
    }
  }

  qsort(found, count, sizeof *found, by_score);

  if (count) {
    complete_title(list, "ROLES");
  }

  for (size_t i = 0; i < count; i++) {
    const Role *role = store_role(guild, found[i].id);
    char insert[40];

    snprintf(insert, sizeof insert, "<@&%" PRIu64 "> ", role->id);
    complete_add(
      list,
      &(CompleteRow){
        .icon = COMPLETE_ICON_GLYPH,
        .glyph = CIVUE_ICON(mingcute, at_fill),
        .color = role->color ? rgb(role->color) : CIVUE_CLEAR,
        .label = role->name,
        .detail = any ? nullptr
                      : "Notify users with this role who have permission to "
                        "view this channel.",
        .insert = insert,
      });
  }

  return count;
}

void mention_offer(App *app, Channel *c, const char *token,
                   CompleteList *list) {
  Store *store = app->session->store;
  const char *typed = token + 1;
  char query[128];
  CompleteList *roles = complete_list_new();
  size_t count = mention_users(app, c, typed, list, ROWS_MAX);

  mention_fold(typed, query, sizeof query);

  // the roles take the room the users left, then everyone and here what
  // is left after them, which the client still lists before the roles.
  if (c->guild_id) {
    count += mention_roles(app, c, typed, roles, ROWS_MAX - count, false);
  }

  if (c->guild_id && store_can(store, c, PERM_MENTION_EVERYONE)) {
    bool everyone = count < ROWS_MAX && mention_fuzzy(query, "everyone");
    bool here = count + everyone < ROWS_MAX && mention_fuzzy(query, "here");

    if (everyone || here) {
      complete_title(list, "EVERYONE");
    }

    if (everyone) {
      complete_add(list,
                   &(CompleteRow){
                     .icon = COMPLETE_ICON_GLYPH,
                     .glyph = CIVUE_ICON(mingcute, at_fill),
                     .label = "@everyone",
                     .detail = "Notify everyone who has permission to view "
                               "this channel.",
                     .insert = "@everyone ",
                   });
    }

    if (here) {
      complete_add(list,
                   &(CompleteRow){
                     .icon = COMPLETE_ICON_GLYPH,
                     .glyph = CIVUE_ICON(mingcute, at_fill),
                     .label = "@here",
                     .detail = "Notify everyone online who has permission to "
                               "view this channel.",
                     .insert = "@here ",
                   });
    }
  }

  complete_list_take(list, roles);
}

/* ========================================================================== */
/* channels.                                                                  */
/* ========================================================================== */

int32_t channel_find(App *, const Channel *c, const char *text,
                     uint32_t caret) {
  return c->guild_id ? mention_token(text, caret, '#') : -1;
}

static civue_icon channel_glyph(const Channel *c) {
  switch (c->type) {
    case CHANNEL_GUILD_VOICE:
      return CIVUE_ICON(mingcute, volume_fill);
    case CHANNEL_GUILD_STAGE:
      return CIVUE_ICON(mingcute, base_station_fill);
    case CHANNEL_GUILD_ANNOUNCEMENT:
      return CIVUE_ICON(mingcute, announcement_fill);
    case CHANNEL_GUILD_FORUM:
      return CIVUE_ICON(mingcute, messages_square_fill);
    case CHANNEL_GUILD_MEDIA:
      return CIVUE_ICON(mingcute, pic_fill);
    case CHANNEL_GUILD_CATEGORY:
      return CIVUE_ICON(mingcute, folder_fill);
    case CHANNEL_ANNOUNCEMENT_THREAD:
    case CHANNEL_PUBLIC_THREAD:
    case CHANNEL_PRIVATE_THREAD:
      return CIVUE_ICON(mingcute, message_2_fill);
    default:
      return CIVUE_ICON(mingcute, hashtag_fill);
  }
}

// the channels a # lists: the ones of text and of posts, and the threads
// the user is in that are still open.
static bool selectable(const Channel *c) {
  switch (c->type) {
    case CHANNEL_GUILD_TEXT:
    case CHANNEL_GUILD_ANNOUNCEMENT:
    case CHANNEL_GUILD_DIRECTORY:
    case CHANNEL_GUILD_FORUM:
    case CHANNEL_GUILD_MEDIA:
      return true;
    default:
      return channel_is_thread(c) && c->thread_joined && !c->thread_archived;
  }
}

static bool type_among(const Channel *c, const uint8_t *types, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (types[i] == c->type) {
      return true;
    }
  }

  return count == 0;
}

typedef enum : uint8_t {
  // what a # lists.
  CHANNELS_SELECTABLE,
  // the voice channels the user may join, after a #!.
  CHANNELS_VOICE,
  // what a channel option takes: any kind, of `types` when it names some.
  CHANNELS_OPTION,
} ChannelsAsked;

static void list_channels(App *app, Channel *c, const char *typed,
                          CompleteList *list, ChannelsAsked asked,
                          const uint8_t *types, size_t ntypes) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  Found found[ROWS_MAX];
  size_t count = 0;
  char query[128];

  mention_fold(typed, query, sizeof query);

  for (size_t i = 0; guild && i < guild->nchannels; i++) {
    const Channel *channel = guild->channels[i];
    Permissions needs = PERM_VIEW_CHANNEL;
    bool wanted = false;

    switch (asked) {
      case CHANNELS_SELECTABLE:
        wanted = selectable(channel);
        break;
      case CHANNELS_VOICE:
        wanted = channel_is_voice(channel);
        needs |= PERM_CONNECT;
        break;
      case CHANNELS_OPTION:
        wanted = (selectable(channel) || channel_is_voice(channel) ||
                  channel->type == CHANNEL_GUILD_CATEGORY) &&
                 type_among(channel, types, ntypes);
        break;
    }

    int score =
      wanted && channel->name ? mention_rank(channel->name, query) : 0;

    if (score && store_can(store, channel, needs)) {
      // as the guild lists them, the threads after the channels.
      int32_t order = channel->position + (channel_is_thread(channel) << 20);

      found_add(found, &count, countof(found),
                (Found){channel->id, score, order, channel->name});
    }
  }

  // outside a guild an option only takes the channel it is typed in.
  if (!guild && asked == CHANNELS_OPTION && type_among(c, types, ntypes)) {
    found[count++] = (Found){.id = c->id};
  }

  qsort(found, count, sizeof *found, by_score);

  if (count) {
    complete_title(list,
                   asked == CHANNELS_VOICE ? "VOICE CHANNELS" : "CHANNELS");
  }

  for (size_t i = 0; i < count; i++) {
    const Channel *channel = store_channel(store, found[i].id);
    const Channel *parent = store_channel(store, channel->parent_id);
    char insert[40], name[128];

    snprintf(insert, sizeof insert, "<#%" PRIu64 "> ", channel->id);
    complete_add(
      list, &(CompleteRow){
              .icon = COMPLETE_ICON_GLYPH,
              .glyph = channel_glyph(channel),
              .label = store_channel_name(store, channel, name, sizeof name),
              .detail = parent ? parent->name : nullptr,
              .insert = insert,
            });
  }
}

void mention_channels(App *app, Channel *c, const char *typed,
                      CompleteList *list, const uint8_t *types, size_t ntypes) {
  list_channels(app, c, typed, list, CHANNELS_OPTION, types, ntypes);
}

void channel_offer(App *app, Channel *c, const char *token,
                   CompleteList *list) {
  bool voice = token[1] == '!';

  list_channels(app, c, token + 1 + voice, list,
                voice ? CHANNELS_VOICE : CHANNELS_SELECTABLE, nullptr, 0);
}

/* ========================================================================== */
/* what a name typed by hand names.                                           */
/* ========================================================================== */

// an id written alone, as discord's client takes one.
static Snowflake bare_id(const char *typed) {
  size_t len = strlen(typed);

  if (len < 17 || len > 20 || strspn(typed, "0123456789") != len) {
    return 0;
  }

  return snowflake_parse(typed);
}

// calls `each` with every user a mention may name in the channel: who is in
// the conversation, or the members the store knows of the guild.
typedef void (*EachUser)(void *ctx, const User *user);

static void each_user(Store *store, const Channel *c, EachUser each,
                      void *ctx) {
  Guild *guild = store_guild(store, c->guild_id);

  if (guild) {
    CILO_MAP_EACH(entry, &guild->members) {
      const User *user = store_user(store, entry->val->user_id);

      if (user && user->username) {
        each(ctx, user);
      }
    }

    return;
  }

  for (size_t i = 0; i <= c->nrecipients; i++) {
    const User *user =
      store_user(store, i < c->nrecipients ? c->recipients[i] : store->self_id);

    if (user && user->username) {
      each(ctx, user);
    }
  }
}

typedef struct {
  const char *name;
  Snowflake id;
  size_t starts;
} Exact;

static void exact_user(void *ctx, const User *user) {
  Exact *e = ctx;

  if (strncmp(user->username, e->name, strlen(e->name)) == 0) {
    e->starts++;

    if (strcmp(user->username, e->name) == 0) {
      e->id = user->id;
    }
  }
}

Snowflake mention_user_named(App *app, const Channel *c, const char *typed) {
  Snowflake id = 0;
  uint32_t taken = 0, len = (uint32_t)strlen(typed);

  if (mention_at(typed, len, &id, &taken) == MENTION_USER && taken == len) {
    return id;
  }

  if ((id = bare_id(typed)) != 0) {
    return id;
  }

  // a username, the only one that starts so.
  Exact exact = {.name = *typed == '@' ? typed + 1 : typed};

  if (!*exact.name) {
    return 0;
  }

  each_user(app->session->store, c, exact_user, &exact);
  return exact.starts == 1 ? exact.id : 0;
}

Snowflake mention_role_named(App *app, const Channel *c, const char *typed) {
  const Guild *guild = store_guild(app->session->store, c->guild_id);
  Snowflake id = 0;
  uint32_t taken = 0, len = (uint32_t)strlen(typed);

  if (!guild) {
    return 0;
  }

  if (mention_at(typed, len, &id, &taken) == MENTION_ROLE && taken == len) {
    return id;
  }

  if ((id = bare_id(typed)) != 0) {
    return id;
  }

  const char *name = *typed == '@' ? typed + 1 : typed;

  if (strcmp(name, "everyone") == 0) {
    return guild->id;
  }

  for (size_t i = 0; i < guild->nroles; i++) {
    const Role *role = guild->roles[i];

    if (role->name && strcmp(role->name, name) == 0) {
      return role->id;
    }
  }

  return 0;
}

/* ========================================================================== */
/* the text sent: what discord's client turns into mentions as it parses     */
/* what was typed.                                                            */
/* ========================================================================== */

typedef struct {
  char *v;
  size_t len, cap;
} Out;

static void out_put(Out *out, const char *bytes, size_t len) {
  if (out->len + len + 1 > out->cap) {
    out->cap = (out->len + len + 1) * 2;
    out->v = realloc(out->v, out->cap);
  }

  memcpy(out->v + out->len, bytes, len);
  out->len += len;
  out->v[out->len] = 0;
}

// the best name found at a sigil so far: the longest one the text starts
// with, whatever its case, unless one is all the text left to the letter.
typedef struct {
  const char *rest;
  Snowflake id;
  size_t len;
  bool whole;
} Match;

static void match_try(Match *m, const char *name, size_t len, Snowflake id) {
  bool whole = strlen(m->rest) == len && strncmp(m->rest, name, len) == 0;

  if (!len || strncasecmp(m->rest, name, len) != 0 || m->whole ||
      (!whole && len <= m->len)) {
    return;
  }

  m->id = id;
  m->len = len;
  m->whole = whole;
}

typedef struct {
  Match match;
  // the name with its four digits, or the username alone.
  bool tagged;
} UserMatch;

static void match_user(void *ctx, const User *user) {
  UserMatch *m = ctx;
  char tag[160];
  const char *digits = user->discriminator ? user->discriminator : "0";

  if (!m->tagged) {
    match_try(&m->match, user->username, strlen(user->username), user->id);
    return;
  }

  snprintf(tag, sizeof tag, "%s#%04d", user->username, atoi(digits));
  match_try(&m->match, tag, strlen(tag), user->id);
}

// whether the text holds one of the words the client keeps from being
// someone's name, at least as long as what was found.
static bool shadowed(const char *text, size_t found) {
  static const char *const words[] = {"@everyone", "@here", "@Clyde"};

  for (size_t i = 0; i < countof(words); i++) {
    size_t len = strlen(words[i]);

    for (const char *at = strstr(text, words[i]); at;
         at = strstr(at + 1, words[i])) {
      bool ends = !isalnum((unsigned char)at[len]) && at[len] != '_';

      if (ends && found <= len) {
        return true;
      }
    }
  }

  return false;
}

// an address is nobody's mention: something before the @ and a dot after
// it, without a blank.
static bool is_address(const char *text, size_t at) {
  const char *rest = text + at + 1;
  size_t host = strcspn(rest, " \n\t");
  const char *dot = memchr(rest, '.', host);

  if (at == 0 || blank_char(text[at - 1]) || text[at - 1] == '(') {
    return false;
  }

  return dot && dot > rest && dot + 1 < rest + host && dot[1] != '.';
}

// the mention the text names at an @: a user by their name and four
// digits, then a role, then a user by their username.
static size_t user_at(App *app, const Channel *c, const char *text, size_t at,
                      char *mention, size_t cap) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  const char *rest = text + at + 1;
  UserMatch tagged = {.match = {.rest = rest}, .tagged = true};
  UserMatch named = {.match = {.rest = rest}};
  Match role = {.rest = rest};
  bool all = guild && store_can(store, c, PERM_MENTION_EVERYONE);

  if (is_address(text, at)) {
    return 0;
  }

  each_user(store, c, match_user, &tagged);

  if (tagged.match.len) {
    snprintf(mention, cap, "<@%" PRIu64 ">", tagged.match.id);
    return tagged.match.len + 1;
  }

  for (size_t i = 0; guild && i < guild->nroles; i++) {
    const Role *r = guild->roles[i];

    if (r->name && r->id != guild->id && (all || r->mentionable)) {
      match_try(&role, r->name, strlen(r->name), r->id);
    }
  }

  if (role.len) {
    snprintf(mention, cap, "<@&%" PRIu64 ">", role.id);
    return role.len + 1;
  }

  each_user(store, c, match_user, &named);

  // the message that starts with @silent is sent without a sound.
  bool silent = at == 0 && strncmp(text, "@silent", 7) == 0 &&
                (!text[7] || blank_char(text[7])) && named.match.len <= 6;

  if (!named.match.len || silent || shadowed(text + at, named.match.len + 1)) {
    return 0;
  }

  snprintf(mention, cap, "<@%" PRIu64 ">", named.match.id);
  return named.match.len + 1;
}

// whether a channel may be named after a #: what a # lists, the voice
// channels, and the categories in view.
static bool nameable(Store *store, const Channel *c) {
  return c->name &&
         (selectable(c) || channel_is_voice(c) ||
          c->type == CHANNEL_GUILD_CATEGORY) &&
         store_can(store, c, PERM_VIEW_CHANNEL);
}

// the channel the text names at a #: by a name between quotes, to the
// letter, or by the longest name it starts with.
static size_t channel_at(App *app, const Channel *c, const char *text,
                         size_t at, char *mention, size_t cap) {
  Store *store = app->session->store;
  const Guild *guild = store_guild(store, c->guild_id);
  const char *rest = text + at + 1;
  Match match = {.rest = rest};
  char quoted[160];
  size_t taken = 0;

  if (!guild) {
    return 0;
  }

  if (*rest == '"') {
    size_t n = 0, i = 1;

    for (; rest[i] && rest[i] != '"' && n + 1 < sizeof quoted; i++) {
      // a backslash keeps the quote or the backslash after it.
      if (rest[i] == '\\' && (rest[i + 1] == '"' || rest[i + 1] == '\\')) {
        i++;
      }

      quoted[n++] = rest[i];
    }

    if (rest[i] != '"') {
      return 0;
    }

    quoted[n] = 0;
    taken = i + 2;
  }

  for (size_t i = 0; i < guild->nchannels; i++) {
    const Channel *channel = guild->channels[i];

    if (!nameable(store, channel)) {
      continue;
    }

    if (taken) {
      if (strcmp(channel->name, quoted) == 0 && !match.id) {
        match.id = channel->id;
      }
    }
    else {
      match_try(&match, channel->name, strlen(channel->name), channel->id);
    }
  }

  if (!match.id) {
    return 0;
  }

  snprintf(mention, cap, "<#%" PRIu64 ">", match.id);
  return taken ? taken : match.len + 1;
}

Snowflake mention_channel_named(App *app, const Channel *c, const char *typed) {
  Snowflake id = 0;
  uint32_t taken = 0, len = (uint32_t)strlen(typed);
  char mention[40];

  if (mention_at(typed, len, &id, &taken) == MENTION_CHANNEL && taken == len) {
    return id;
  }

  if ((id = bare_id(typed)) != 0) {
    return id;
  }

  if (*typed != '#' ||
      channel_at(app, c, typed, 0, mention, sizeof mention) != len) {
    return 0;
  }

  return snowflake_parse_n(mention + 2, strlen(mention) - 3);
}

// how many bytes from `at` are kept as they are: code, a link, or what is
// already written as discord sends it.
static size_t kept_at(const char *text, size_t at) {
  const char *s = text + at;
  const char *end = nullptr;

  if (strncmp(s, "```", 3) == 0) {
    end = strstr(s + 3, "```");
    return end ? (size_t)(end - s) + 3 : strlen(s);
  }

  if (*s == '`') {
    end = strchr(s + 1, '`');
    return end ? (size_t)(end - s) + 1 : 0;
  }

  if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) {
    return strcspn(s, " \n\t");
  }

  // a mention, an emoji or a link between angle brackets.
  if (*s == '<' && (s[1] == '@' || s[1] == '#' || s[1] == ':' || s[1] == 'a' ||
                    s[1] == 'h')) {
    size_t len = strcspn(s, "> \n\t");

    return s[len] == '>' ? len + 1 : 0;
  }

  return 0;
}

char *complete_encode(App *app, const Channel *c, const char *text) {
  Out out = {};
  size_t len = strlen(text);

  out_put(&out, "", 0);

  for (size_t i = 0; i < len;) {
    size_t kept = kept_at(text, i);
    char mention[40];
    size_t taken = 0;

    if (kept) {
      out_put(&out, text + i, kept);
      i += kept;
      continue;
    }

    if (text[i] == '@') {
      taken = user_at(app, c, text, i, mention, sizeof mention);
    }
    else if (text[i] == '#') {
      taken = channel_at(app, c, text, i, mention, sizeof mention);
    }

    if (taken) {
      out_put(&out, mention, strlen(mention));
      i += taken;
    }
    else {
      out_put(&out, text + i, 1);
      i++;
    }
  }

  return out.v;
}
