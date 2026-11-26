#include "discord/commands.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "discord/api.h"
#include "discord/gateway.h"
#include "platform/platform.h"

// the errors discord answers a command the index no longer has right with.
#define BAD_VERSION "INTERACTION_APPLICATION_COMMAND_INVALID_VERSION"
#define BAD_COMMAND "INTERACTION_APPLICATION_COMMAND_INVALID"
constexpr uint32_t UNKNOWN_INTEGRATION = 10005;

// the contexts a command may be used in, as bits of their numbers.
constexpr uint8_t CONTEXT_GUILD = 1 << 0;
constexpr uint8_t CONTEXT_BOT_DM = 1 << 1;
constexpr uint8_t CONTEXT_PRIVATE_CHANNEL = 1 << 2;

// where an application was installed, the same way.
constexpr uint8_t INSTALL_GUILD = 1 << 0;
constexpr uint8_t INSTALL_USER = 1 << 1;

// what discord's client waits after an index that failed or is still
// being made, in seconds, and how many times it asks before giving up.
constexpr double INDEX_RETRY = 5;
constexpr uint8_t INDEX_TRIES = 3;
// the client asks an application to complete an option at once, then at
// most once in this long, the last text typed going last.
constexpr double SUGGEST_REST = 0.5;
// how long the application has to answer that.
constexpr double SUGGEST_PATIENCE = 3;
// discord tells of a command that failed, and of an application that took
// more than three seconds. past this, the gateway is taken to have missed
// it.
constexpr double INTERACTION_PATIENCE = 20;
// an application that answered has its message come within this.
constexpr double ANSWER_PATIENCE = 10;
// a failure this long after the command was run is the application's
// silence, whatever discord called it.
constexpr double SILENCE_AFTER = 3;

// who may run commands, as an index tells it for an application or for one
// command: the current user when it names them, some roles and some
// channels.
typedef struct {
  Snowflake id;
  bool allowed;
} Allowance;

typedef struct {
  bool has_user, user;
  Allowance *roles;
  size_t nroles;
  Allowance *channels;
  size_t nchannels;
} Gate;

typedef struct {
  // first, a Command is known to be in one.
  Command command;
  Gate gate;
  bool has_gate;
  // the permissions its application asks of who runs it. 0 asks for an
  // administrator.
  Permissions member_permissions;
  bool has_member_permissions;
  // CONTEXT_ and INSTALL_ bits, with whether discord sent any.
  uint8_t contexts, installs;
  bool has_contexts, has_installs;
  bool dm_permission, nsfw;
  // one of discord's own: where it is offered, everywhere when null.
  bool (*offered)(Session *s, const Channel *c);
} Entry;

typedef struct {
  CommandApplication application;
  Gate gate;
  bool has_gate;
} Listed;

// the commands of a guild, of the conversation with a bot, or of the
// user's account when both ids are 0.
typedef struct {
  Snowflake guild_id, channel_id;
  Listed *applications;
  size_t napplications;
  Entry *entries;
  size_t nentries;
  bool loading, loaded;
  // discord has a newer one than what is held.
  bool stale;
  // store_now before which it is not asked for again, and how many times
  // it was asked in a row without an answer to keep.
  double retry_at;
  uint8_t tries;
} Index;

typedef enum : uint8_t {
  ASK_INDEX,
  ASK_RUN,
  ASK_SUGGEST,
  ASK_THREAD,
  ASK_MODERATE,
  ASK_TTS,
} AskKind;

// a request on its way, in the commands' list until it is answered.
typedef struct Ask Ask;

struct Ask {
  Ask *next;
  Session *s;
  AskKind kind;
  uint64_t epoch;
  Snowflake guild_id, channel_id, nonce;
  Builtin builtin;
  // what a thread starts with, and what to tell once a member was dealt
  // with.
  char *text;
};

// an interaction discord was asked for, until the message that answers it
// or its failure comes.
typedef struct Waiting Waiting;

struct Waiting {
  Waiting *next;
  Session *s;
  Snowflake nonce, channel_id, guild_id, application_id;
  PlatformTimer *timer;
  // discord said the application answered.
  bool answered;
  // a component pressed rather than a command run: its message and what
  // its application knows it by.
  Snowflake message_id;
  char *custom_id;
};

struct Commands {
  Session *session;
  Index **indexes;
  size_t nindexes;
  Ask *asks;
  Waiting *waiting;
  // wakes the ui once an index may be asked for again.
  PlatformTimer *retry;

  // the commands of `slashes_channel` as they were last listed.
  Slash *slashes;
  size_t nslashes;
  Snowflake slashes_channel;
  uint64_t slashes_stamp;
  // bumped with every index that arrives, goes or turns stale.
  uint64_t version;

  // the last question asked of an application: what tells it from another,
  // the nonce its answer carries, what it answered and whether it did.
  uint64_t suggest_key;
  Snowflake suggest_nonce;
  CommandChoice *choices;
  size_t nchoices;
  bool suggest_loading, suggest_failed;
  // a question waits in `suggest_body` while `suggest_rest` runs, and
  // `suggest_patience` ends one the application left unanswered.
  PlatformTimer *suggest_rest, *suggest_patience;
  char *suggest_body;
};

static void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n, size);

  if (p == nullptr) {
    cilo_oom(n * size);
  }

  return p;
}

static void *xrealloc(void *p, size_t size) {
  void *next = realloc(p, size);

  if (next == nullptr) {
    cilo_oom(size);
  }

  return next;
}

static bool online(const Session *s) {
  return !s->offline && s->api != nullptr && s->token != nullptr;
}

static void changed(Session *s) {
  s->version++;

  if (s->options.on_change != nullptr) {
    s->options.on_change();
  }
}

static void notify(Session *s, const char *notice, bool failed) {
  str_set(&s->notice, notice);
  s->notice_failed = failed;
  s->notice_version++;
}

static struct Commands *commands_of(Session *s) {
  if (s->commands == nullptr) {
    s->commands = xcalloc(1, sizeof *s->commands);
    s->commands->session = s;
  }

  return s->commands;
}

static Snowflake new_nonce(Session *s) {
  int64_t now = (int64_t)(store_now(s->store) * 1000);

  return snowflake_from_time_ms(now) | (++s->nonce_counter & 0xfff);
}

static Ask *ask_new(Session *s, AskKind kind) {
  struct Commands *cs = commands_of(s);
  Ask *ask = xcalloc(1, sizeof *ask);

  ask->s = s;
  ask->kind = kind;
  ask->epoch = s->epoch;
  ask->next = cs->asks;
  cs->asks = ask;
  return ask;
}

// takes the ask off its list and frees it. true when its answer is for
// the account the session still has.
static bool ask_end(Ask *ask) {
  struct Commands *cs = ask->s->commands;
  bool wanted = ask->epoch == ask->s->epoch;

  for (Ask **at = &cs->asks; *at != nullptr; at = &(*at)->next) {
    if (*at == ask) {
      *at = ask->next;
      break;
    }
  }

  free(ask->text);
  free(ask);
  return wanted;
}

/* ========================================================================== */
/* what an index holds.                                                       */
/* ========================================================================== */

static void free_option(CommandOption *o) {
  for (size_t i = 0; i < o->nchoices; i++) {
    free(o->choices[i].name);
    free(o->choices[i].value);
  }

  for (size_t i = 0; i < o->noptions; i++) {
    free_option(&o->options[i]);
  }

  free(o->name);
  free(o->shown);
  free(o->description);
  free(o->choices);
  free(o->options);
  free(o->channel_types);
}

static void free_gate(Gate *g) {
  free(g->roles);
  free(g->channels);
}

static void free_index(Index *x) {
  for (size_t i = 0; i < x->napplications; i++) {
    free(x->applications[i].application.name);
    free(x->applications[i].application.description);
    free(x->applications[i].application.icon);
    free_gate(&x->applications[i].gate);
  }

  for (size_t i = 0; i < x->nentries; i++) {
    Command *c = &x->entries[i].command;

    for (size_t j = 0; j < c->noptions; j++) {
      free_option(&c->options[j]);
    }

    free(c->name);
    free(c->shown);
    free(c->description);
    free(c->options);
    free(c->json);
    free_gate(&x->entries[i].gate);
  }

  free(x->applications);
  free(x->entries);
  x->applications = nullptr;
  x->entries = nullptr;
  x->napplications = x->nentries = 0;
}

static const char *string_at(const cima_json *v, const char *key) {
  return json_get_or(v, key, (const char *)nullptr);
}

static bool bool_at(const cima_json *v, const char *key, bool fallback) {
  const cima_json *b = json_get(v, key);

  return json_type(b) == CIMA_JSON_BOOL ? b->b : fallback;
}

static bool is_number(const cima_json *v) {
  return json_type(v) >= CIMA_JSON_INT && json_type(v) <= CIMA_JSON_DOUBLE;
}

// an index writes a name in the user's language when the application has
// it so, and then the name discord knows it by beside it.
static const char *known_at(const cima_json *v, const char *key,
                            const char *key_default) {
  const char *known = string_at(v, key_default);

  return known != nullptr ? known : json_get_or(v, key, "");
}

static CommandChoice put_choice(const cima_json *json) {
  const cima_json *value = json_get(json, "value");
  const char *localized = string_at(json, "name_localized");
  CommandChoice choice = {
    .name = str_dup(localized != nullptr ? localized : string_at(json, "name")),
  };

  if (is_number(value)) {
    char digits[CIMA_JSON_NUMBER_MAX];
    double number = json_coerce(value, (double)0);

    // whole numbers keep their digits, discord's go up to 2^53.
    if (number == floor(number) && fabs(number) < 0x1p63) {
      snprintf(digits, sizeof digits, "%" PRId64, (int64_t)number);
    }
    else {
      snprintf(digits, sizeof digits, "%.17g", number);
    }

    choice.value = str_dup(digits);
    choice.number = true;
  }
  else {
    choice.value = str_dup(json_or(value, ""));
  }

  if (choice.name == nullptr) {
    choice.name = str_dup(choice.value);
  }

  return choice;
}

static void put_option(CommandOption *o, const cima_json *json) {
  const cima_json *choices = json_get(json, "choices");
  const cima_json *options = json_get(json, "options");
  const cima_json *types = json_get(json, "channel_types");
  const cima_json *v;

  *o = (CommandOption){
    .type = (uint8_t)json_get_coerce(json, "type", (int64_t)0),
    .name = str_dup(known_at(json, "name", "name_default")),
    .shown = str_dup(json_get_or(json, "name", "")),
    .description = str_dup(json_get_or(json, "description", "")),
    .required = bool_at(json, "required", false),
    .autocomplete = bool_at(json, "autocomplete", false),
  };

  if (json_len(choices) != 0) {
    o->choices = xcalloc(json_len(choices), sizeof *o->choices);

    CIMA_EACH(choice, json_items(choices)) {
      o->choices[o->nchoices++] = put_choice(choice);
    }
  }

  if (json_len(options) != 0) {
    o->options = xcalloc(json_len(options), sizeof *o->options);

    CIMA_EACH(option, json_items(options)) {
      put_option(&o->options[o->noptions++], option);
    }
  }

  if (json_len(types) != 0) {
    o->channel_types = xcalloc(json_len(types), sizeof *o->channel_types);

    CIMA_EACH(type, json_items(types)) {
      o->channel_types[o->nchannel_types++] =
        (uint8_t)json_coerce(type, (int64_t)0);
    }
  }

  if (is_number(v = json_get(json, "min_value"))) {
    o->has_min = true;
    o->min_value = json_coerce(v, (double)0);
  }

  if (is_number(v = json_get(json, "max_value"))) {
    o->has_max = true;
    o->max_value = json_coerce(v, (double)0);
  }

  if (is_number(v = json_get(json, "min_length"))) {
    o->has_min_length = true;
    o->min_length = (uint32_t)json_coerce(v, (int64_t)0);
  }

  if (is_number(v = json_get(json, "max_length"))) {
    o->has_max_length = true;
    o->max_length = (uint32_t)json_coerce(v, (int64_t)0);
  }
}

static size_t put_allowances(Allowance **out, const cima_json *map) {
  size_t count = 0;

  if (json_len(map) == 0) {
    return 0;
  }

  *out = xcalloc(json_len(map), sizeof **out);

  CIMA_EACH(member, json_members(map)) {
    if (json_type(&member->value) == CIMA_JSON_BOOL) {
      (*out)[count++] = (Allowance){
        snowflake_parse_n(member->key, cima_json_key_len(member)),
        member->value.b,
      };
    }
  }

  return count;
}

static bool put_gate(Gate *g, const cima_json *json) {
  const cima_json *user = json_get(json, "user");

  if (json_type(json) != CIMA_JSON_OBJECT) {
    return false;
  }

  if (json_type(user) == CIMA_JSON_BOOL) {
    g->has_user = true;
    g->user = user->b;
  }

  g->nroles = put_allowances(&g->roles, json_get(json, "roles"));
  g->nchannels = put_allowances(&g->channels, json_get(json, "channels"));
  return true;
}

static uint8_t bits_of(const cima_json *list) {
  uint8_t bits = 0;

  CIMA_EACH(item, json_items(list)) {
    int64_t n = json_coerce(item, (int64_t)-1);

    if (n >= 0 && n < 8) {
      bits |= (uint8_t)(1 << n);
    }
  }

  return bits;
}

typedef enum : uint8_t {
  KEPT_COMMAND,
  KEPT_OPTION,
  KEPT_CHOICE,
} Kept;

static void write_kept(cima_json_writer *w, const cima_json *json, Kept kind,
                       Snowflake self_id);

static void write_kept_list(cima_json_writer *w, const cima_json *list,
                            Kept kind, Snowflake self_id) {
  json_begin_array(w);

  CIMA_EACH(item, json_items(list)) {
    write_kept(w, item, kind, self_id);
  }

  json_end_array(w);
}

// the permissions of an index as the client lists them: the user's entry
// under their id, then the channels, then the roles.
static void write_kept_gate(cima_json_writer *w, const cima_json *gate,
                            Snowflake self_id) {
  static const char *const maps[] = {"channels", "roles"};
  static const int64_t types[] = {3, 1};
  const cima_json *user = json_get(gate, "user");

  json_begin_array(w);

  if (json_type(user) == CIMA_JSON_BOOL) {
    json_begin_object(w);
    json_field(w, "type", (int64_t)2);
    json_key(w, "id"), snowflake_write(w, self_id);
    json_field(w, "permission", user->b);
    json_end_object(w);
  }

  for (size_t i = 0; i < countof(maps); i++) {
    CIMA_EACH(member, json_members(json_get(gate, maps[i]))) {
      json_begin_object(w);
      json_field(w, "type", types[i]);
      json_key(w, "id");
      cima_json_write_string_n(w, member->key, cima_json_key_len(member));
      json_key(w, "permission");
      cima_json_write(w, &member->value);
      json_end_object(w);
    }
  }

  json_end_array(w);
}

// a command of an index as discord's client keeps it, which is what it
// sends back when the command is run: `name` and `description` are the
// ones discord knows it by, the user's language going to `name_localized`
// and `description_localized`, in its options and its choices too, and its
// permissions are a list. the fields keep the place they came at, the ones
// the client adds going last.
static void write_kept(cima_json_writer *w, const cima_json *json, Kept kind,
                       Snowflake self_id) {
  const cima_json *name = json_get(json, "name");
  const cima_json *name_default = json_get(json, "name_default");
  const cima_json *says = json_get(json, "description");
  const cima_json *says_default = json_get(json, "description_default");
  const char *known = json_type(name_default) == CIMA_JSON_STRING
                        ? name_default->str
                        : json_or(name, (const char *)nullptr);
  const char *said = json_type(says_default) == CIMA_JSON_STRING
                       ? says_default->str
                       : json_or(says, (const char *)nullptr);
  bool had_says = false, had_options = false;

  if (json_type(json) != CIMA_JSON_OBJECT) {
    cima_json_write(w, json);
    return;
  }

  json_begin_object(w);

  CIMA_EACH(member, json_members(json)) {
    const char *key = member->key;

    if (strcmp(key, "name") == 0) {
      if (known != nullptr) {
        json_field(w, "name", known);
      }
    }
    else if (strcmp(key, "description") == 0 && kind != KEPT_CHOICE) {
      had_says = true;

      if (said != nullptr || kind == KEPT_COMMAND) {
        json_field(w, "description", said != nullptr ? said : "");
      }
    }
    else if (strcmp(key, "options") == 0 && kind != KEPT_CHOICE) {
      had_options = true;
      json_key(w, "options");
      write_kept_list(w, &member->value, KEPT_OPTION, self_id);
    }
    else if (strcmp(key, "choices") == 0 && kind == KEPT_OPTION &&
             json_type(&member->value) == CIMA_JSON_ARRAY) {
      json_key(w, "choices");
      write_kept_list(w, &member->value, KEPT_CHOICE, self_id);
    }
    else if (strcmp(key, "permissions") == 0 && kind == KEPT_COMMAND) {
      if (json_type(&member->value) == CIMA_JSON_OBJECT) {
        json_key(w, "permissions");
        write_kept_gate(w, &member->value, self_id);
      }
    }
    else {
      cima_json_write_key_n(w, key, cima_json_key_len(member));
      cima_json_write(w, &member->value);
    }
  }

  if (kind == KEPT_COMMAND) {
    if (!had_says) {
      json_field(w, "description", said != nullptr ? said : "");
    }

    if (!had_options) {
      json_key(w, "options");
      json_begin_array(w);
      json_end_array(w);
    }
  }
  else if (kind == KEPT_OPTION && !had_says && said != nullptr) {
    json_field(w, "description", said);
  }

  // the client compares what it read with the default, which is not there
  // for a name nobody translated: the localized one is then the name.
  bool says_differ = json_type(says) == CIMA_JSON_STRING &&
                     (json_type(says_default) != CIMA_JSON_STRING ||
                      strcmp(says->str, says_default->str) != 0);
  bool names_differ = json_type(name) == CIMA_JSON_STRING &&
                      (json_type(name_default) != CIMA_JSON_STRING ||
                       strcmp(name->str, name_default->str) != 0);

  if (says_differ && kind != KEPT_CHOICE) {
    json_field(w, "description_localized", says->str);
  }

  if (names_differ) {
    json_field(w, "name_localized", name->str);
  }

  json_end_object(w);
}

// false for what is no slash command.
static bool put_entry(Session *s, Entry *e, const cima_json *json) {
  const cima_json *options = json_get(json, "options");
  const cima_json *asked = json_get(json, "default_member_permissions");
  const cima_json *contexts = json_get(json, "contexts");
  const cima_json *installs = json_get(json, "integration_types");
  Command *c = &e->command;

  if (json_get_coerce(json, "type", (int64_t)1) != 1) {
    return false;
  }

  *e = (Entry){
    .command =
      {
        .id = snowflake_of(json_get(json, "id")),
        .application_id = snowflake_of(json_get(json, "application_id")),
        .guild_id = snowflake_of(json_get(json, "guild_id")),
        .version = snowflake_of(json_get(json, "version")),
        .name = str_dup(known_at(json, "name", "name_default")),
        .shown = str_dup(json_get_or(json, "name", "")),
        .description = str_dup(json_get_or(json, "description", "")),
      },
    .dm_permission = bool_at(json, "dm_permission", true),
    .nsfw = bool_at(json, "nsfw", false),
  };

  if (c->id == 0 || c->application_id == 0 || *c->name == '\0') {
    free(c->name);
    free(c->shown);
    free(c->description);
    return false;
  }

  cima_json_writer w = {};

  write_kept(&w, json, KEPT_COMMAND, s->store->self_id);
  c->json = str_dup(cima_json_writer_text(&w));
  cima_json_writer_dispose(&w);

  if (json_len(options) != 0) {
    c->options = xcalloc(json_len(options), sizeof *c->options);

    CIMA_EACH(option, json_items(options)) {
      put_option(&c->options[c->noptions++], option);
    }
  }

  if (json_type(asked) == CIMA_JSON_STRING || is_number(asked)) {
    e->has_member_permissions = true;
    e->member_permissions = snowflake_of(asked);
  }

  if (json_type(contexts) == CIMA_JSON_ARRAY) {
    e->has_contexts = true;
    e->contexts = bits_of(contexts);
  }

  if (json_type(installs) == CIMA_JSON_ARRAY) {
    e->has_installs = true;
    e->installs = bits_of(installs);
  }

  e->has_gate = put_gate(&e->gate, json_get(json, "permissions"));
  return true;
}

static void put_index(Session *s, Index *x, const cima_json *json) {
  const cima_json *applications = json_get(json, "applications");
  const cima_json *commands = json_get(json, "application_commands");
  Snowflake unknown[100];
  size_t nunknown = 0;

  free_index(x);

  if (json_len(applications) != 0) {
    x->applications = xcalloc(json_len(applications), sizeof *x->applications);
  }

  CIMA_EACH(app, json_items(applications)) {
    const cima_json *bot = json_get(app, "bot");
    Listed *listed = &x->applications[x->napplications];
    Snowflake id = snowflake_of(json_get(app, "id"));
    Snowflake bot_id = snowflake_of(json_get(app, "bot_id"));
    const User *user = nullptr;

    if (id == 0) {
      continue;
    }

    // the bot is who the application's answers are written by, and whose
    // name its commands are listed under.
    if (json_type(bot) == CIMA_JSON_OBJECT) {
      user = store_put_user(s->store, bot);
    }
    else if (bot_id != 0) {
      user = store_user(s->store, bot_id);

      if (user == nullptr && nunknown < countof(unknown)) {
        unknown[nunknown++] = bot_id;
      }
    }

    const char *name = user != nullptr && user->username != nullptr
                         ? user->username
                         : json_get_or(app, "name", "");

    *listed = (Listed){
      .application =
        {
          .id = id,
          .name = str_dup(name),
          .description = str_dup(string_at(app, "description")),
          .icon = str_dup(string_at(app, "icon")),
          .bot_id = user != nullptr ? user->id : bot_id,
        },
    };
    listed->has_gate = put_gate(&listed->gate, json_get(app, "permissions"));
    x->napplications++;
  }

  // as the client asks for the bots it does not know of a guild.
  if (x->guild_id != 0 && nunknown != 0 && s->gateway != nullptr) {
    gateway_request_members(s->gateway, x->guild_id, unknown, nunknown);
  }

  if (json_len(commands) != 0) {
    x->entries = xcalloc(json_len(commands), sizeof *x->entries);
  }

  CIMA_EACH(command, json_items(commands)) {
    if (put_entry(s, &x->entries[x->nentries], command)) {
      x->nentries++;
    }
  }
}

/* ========================================================================== */
/* asking for an index.                                                       */
/* ========================================================================== */

static Index *index_find(struct Commands *cs, Snowflake guild_id,
                         Snowflake channel_id) {
  for (size_t i = 0; cs != nullptr && i < cs->nindexes; i++) {
    Index *x = cs->indexes[i];

    if (x->guild_id == guild_id && x->channel_id == channel_id) {
      return x;
    }
  }

  return nullptr;
}

static Index *index_get(struct Commands *cs, Snowflake guild_id,
                        Snowflake channel_id) {
  Index *x = index_find(cs, guild_id, channel_id);

  if (x != nullptr) {
    return x;
  }

  x = xcalloc(1, sizeof *x);
  x->guild_id = guild_id;
  x->channel_id = channel_id;
  cs->indexes = xrealloc(cs->indexes, (cs->nindexes + 1) * sizeof *cs->indexes);
  cs->indexes[cs->nindexes++] = x;
  return x;
}

static void on_retry(void *ctx) {
  struct Commands *cs = ctx;

  // the ui lists the commands again and the index is asked for then.
  cs->retry = nullptr;
  cs->version++;
  changed(cs->session);
}

static void on_index(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  Snowflake guild_id = ask->guild_id, channel_id = ask->channel_id;

  if (!ask_end(ask)) {
    return;
  }

  struct Commands *cs = s->commands;
  Index *x = index_find(cs, guild_id, channel_id);

  if (x == nullptr) {
    return;
  }

  x->loading = false;

  // 202 for an index discord is still making.
  if (res->ok && res->status != 202 &&
      json_type(res->json) == CIMA_JSON_OBJECT) {
    put_index(s, x, res->json);
    x->loaded = true;
    x->stale = false;
    x->tries = 0;
    x->retry_at = 0;
  }
  else {
    bool making = res->status == 202 && ++x->tries < INDEX_TRIES;

    // one that failed keeps what it had and is asked for again in a while:
    // by itself when discord was still making it, else the next time its
    // commands are listed.
    x->retry_at = store_now(s->store) + INDEX_RETRY;
    x->stale = true;
    x->loaded = true;

    if (!making) {
      x->tries = 0;
    }
    else if (cs->retry == nullptr) {
      cs->retry = platform_timer(INDEX_RETRY + 0.1, on_retry, cs);
    }
  }

  cs->version++;
  changed(s);
}

// a fixture has no commands of its own: QUARREL_COMMANDS names an index as
// discord answers one, which every guild then has.
static Index *index_fixture(Session *s, Snowflake guild_id) {
  const char *path = getenv("QUARREL_COMMANDS");
  FILE *file = path != nullptr && guild_id != 0 ? fopen(path, "rb") : nullptr;

  if (file == nullptr) {
    return nullptr;
  }

  Index *x = index_get(commands_of(s), guild_id, 0);

  if (!x->loaded) {
    cilo_arena arena = {};
    char *text = nullptr;
    size_t len = 0, cap = 0, got = 0;

    do {
      cap = cap != 0 ? cap * 2 : 1 << 16;
      text = xrealloc(text, cap);
      got = fread(text + len, 1, cap - len, file);
      len += got;
    } while (got != 0);

    put_index(s, x, json_parse(&arena, text, len, nullptr));
    x->loaded = true;
    cilo_arena_drop(&arena);
    free(text);
  }

  fclose(file);
  return x;
}

// asks for the index when it was never asked for, or discord has a newer
// one. null offline.
static Index *index_want(Session *s, Snowflake guild_id, Snowflake channel_id) {
  if (!online(s)) {
    return s->offline ? index_fixture(s, guild_id) : nullptr;
  }

  struct Commands *cs = commands_of(s);
  Index *x = index_get(cs, guild_id, channel_id);

  if (x->loading || (x->loaded && !x->stale) ||
      store_now(s->store) < x->retry_at) {
    return x;
  }

  char path[96];
  const char *route = "/users/@me/application-command-index";

  if (guild_id != 0) {
    route = "/guilds/:id/application-command-index";
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/application-command-index",
             guild_id);
  }
  else if (channel_id != 0) {
    route = "/channels/:id/application-command-index";
    snprintf(path, sizeof path,
             "/channels/%" PRIu64 "/application-command-index", channel_id);
  }
  else {
    snprintf(path, sizeof path, "%s", route);
  }

  Ask *ask = ask_new(s, ASK_INDEX);

  ask->guild_id = guild_id;
  ask->channel_id = channel_id;
  x->loading = true;
  api_request(s->api,
              &(ApiRequest){.method = "GET", .path = path, .route = route},
              on_index, ask);
  return x;
}

static void index_stale(struct Commands *cs, Index *x) {
  if (x != nullptr) {
    x->stale = true;
    cs->version++;
  }
}

/* ========================================================================== */
/* discord's own commands.                                                    */
/* ========================================================================== */

static bool offered_tts(Session *s, const Channel *c) {
  return c->guild_id != 0 && store_can(s->store, c, PERM_SEND_TTS_MESSAGES);
}

static bool offered_nick(Session *s, const Channel *c) {
  Permissions perms = store_permissions(s->store, c);

  return c->guild_id != 0 &&
         (perms & (PERM_CHANGE_NICKNAME | PERM_MANAGE_NICKNAMES)) != 0;
}

// a channel the composer shows that holds threads.
static bool offered_thread(Session *s, const Channel *c) {
  bool holds =
    c->type == CHANNEL_GUILD_TEXT || c->type == CHANNEL_GUILD_ANNOUNCEMENT;

  return holds &&
         store_can(s->store, c,
                   PERM_CREATE_PUBLIC_THREADS | PERM_READ_MESSAGE_HISTORY);
}

static bool guild_can(Session *s, const Channel *c, Permissions p) {
  const Guild *g = store_guild(s->store, c->guild_id);

  return g != nullptr && (store_guild_permissions(s->store, g) & p) == p;
}

static bool offered_kick(Session *s, const Channel *c) {
  return guild_can(s, c, PERM_KICK_MEMBERS);
}

static bool offered_ban(Session *s, const Channel *c) {
  return guild_can(s, c, PERM_BAN_MEMBERS);
}

static bool offered_timeout(Session *s, const Channel *c) {
  return guild_can(s, c, PERM_MODERATE_MEMBERS);
}

#define OPTION(kind, called, says, ...) \
  {.type = kind,                        \
   .name = called,                      \
   .shown = called,                     \
   .description = says,                 \
   __VA_ARGS__}

static CommandOption gif_options[] = {
  OPTION(OPTION_STRING, "query", "Search for a GIF", .required = true),
};
static CommandOption optional_message[] = {
  OPTION(OPTION_STRING, "message", "Your message"),
};
static CommandOption required_message[] = {
  OPTION(OPTION_STRING, "message", "Your message", .required = true),
};
static CommandOption nick_options[] = {
  OPTION(OPTION_STRING, "new_nick", "New nickname", .has_max_length = true,
         .max_length = 32),
};
static CommandOption thread_options[] = {
  OPTION(OPTION_STRING, "name", "Type a name for your thread", .required = true,
         .has_max_length = true, .max_length = 100),
  OPTION(OPTION_STRING, "message", "Type the first message in your thread",
         .required = true),
};
static CommandOption kick_options[] = {
  OPTION(OPTION_USER, "user", "The user to kick", .required = true),
  OPTION(OPTION_STRING, "reason", "The reason for kicking, if any"),
};
// in seconds, what the ban has discord delete.
static CommandChoice ban_deletes[] = {
  {"Don't Delete Any", "0", true},      {"Previous Hour", "3600", true},
  {"Previous 6 Hours", "21600", true},  {"Previous 12 Hours", "43200", true},
  {"Previous 24 Hours", "86400", true}, {"Previous 3 Days", "259200", true},
  {"Previous 7 Days", "604800", true},
};
static CommandOption ban_options[] = {
  OPTION(OPTION_USER, "user", "The user to ban", .required = true),
  OPTION(OPTION_INTEGER, "delete_messages",
         "How much of their recent message history to delete", .required = true,
         .choices = ban_deletes, .nchoices = countof(ban_deletes)),
  OPTION(OPTION_STRING, "reason", "The reason for banning, if any"),
};
static CommandChoice timeout_durations[] = {
  {"60 secs", "60", true},  {"5 mins", "300", true},
  {"10 mins", "600", true}, {"1 hour", "3600", true},
  {"1 day", "86400", true}, {"1 week", "604800", true},
};
static CommandOption timeout_options[] = {
  OPTION(OPTION_USER, "user", "The user to timeout", .required = true),
  OPTION(OPTION_INTEGER, "duration", "How long they should be timed out for",
         .required = true, .choices = timeout_durations,
         .nchoices = countof(timeout_durations)),
  OPTION(OPTION_STRING, "reason", "The reason for timing them out, if any"),
};
static CommandOption msg_options[] = {
  OPTION(OPTION_USER, "user", "The user to message", .required = true),
  OPTION(OPTION_STRING, "message", "Message to send", .required = true),
};

#define BUILT_IN(which, called, says, takes, where) \
  {.command = {.id = COMMANDS_BUILT_IN - (which),   \
               .application_id = COMMANDS_BUILT_IN, \
               .name = called,                      \
               .shown = called,                     \
               .description = says,                 \
               .options = takes,                    \
               .noptions = countof(takes),          \
               .builtin = which},                   \
   .dm_permission = true,                           \
   .offered = where}

// in the order of discord's client, without the ones Quarrel has nothing
// for: leaving a group, scheduling a message, stickers and its dice.
static Entry built_ins[] = {
  BUILT_IN(BUILTIN_GIF, "gif", "Search Animated GIFs on the Web", gif_options,
           nullptr),
  BUILT_IN(BUILTIN_SHRUG, "shrug", "Appends ¯\\_(ツ)_/¯ to your message.",
           optional_message, nullptr),
  BUILT_IN(BUILTIN_TABLEFLIP, "tableflip",
           "Appends (╯°□°)╯︵ ┻━┻ to your message.", optional_message, nullptr),
  BUILT_IN(BUILTIN_UNFLIP, "unflip",
           "Appends ┬─┬ ノ( ゜-゜ノ) to your message.", optional_message,
           nullptr),
  BUILT_IN(BUILTIN_TTS, "tts",
           "Use text-to-speech to read the message to all members currently "
           "viewing the channel.",
           required_message, offered_tts),
  BUILT_IN(BUILTIN_ME, "me", "Displays text with emphasis.", required_message,
           nullptr),
  BUILT_IN(BUILTIN_SPOILER, "spoiler", "Marks your message as a spoiler.",
           required_message, nullptr),
  BUILT_IN(BUILTIN_NICK, "nick", "Change nickname on this server.",
           nick_options, offered_nick),
  BUILT_IN(BUILTIN_THREAD, "thread", "Start new thread", thread_options,
           offered_thread),
  BUILT_IN(BUILTIN_KICK, "kick", "Kick user", kick_options, offered_kick),
  BUILT_IN(BUILTIN_BAN, "ban", "Ban user", ban_options, offered_ban),
  BUILT_IN(BUILTIN_TIMEOUT, "timeout", "Timeout user", timeout_options,
           offered_timeout),
  BUILT_IN(BUILTIN_MSG, "msg", "Message user", msg_options, nullptr),
};

static const CommandApplication built_in_application = {
  .id = COMMANDS_BUILT_IN,
  .name = "Built-In",
};

/* ========================================================================== */
/* who may run what, after the one function of discord's client that says.    */
/* ========================================================================== */

static const Allowance *allowance(const Allowance *list, size_t count,
                                  Snowflake id) {
  for (size_t i = 0; i < count; i++) {
    if (list[i].id == id) {
      return &list[i];
    }
  }

  return nullptr;
}

typedef enum : uint8_t {
  GATE_SILENT,
  GATE_ALLOWS,
  GATE_DENIES,
} GateSays;

static GateSays said(const Allowance *a) {
  return a == nullptr ? GATE_SILENT : a->allowed ? GATE_ALLOWS : GATE_DENIES;
}

// what a gate says of the current user: their own entry first, then their
// roles, of which one that allows is enough and one that denies settles
// it, then @everyone, the role whose id is the guild's.
static GateSays gate_user(const Gate *g, const Guild *guild, const Member *me) {
  bool named = false;

  if (g == nullptr) {
    return GATE_SILENT;
  }

  if (g->has_user) {
    return g->user ? GATE_ALLOWS : GATE_DENIES;
  }

  for (size_t i = 0; me != nullptr && i < me->nroles; i++) {
    const Allowance *a = allowance(g->roles, g->nroles, me->roles[i]);

    if (a != nullptr && a->allowed) {
      return GATE_ALLOWS;
    }

    named |= a != nullptr;
  }

  return named ? GATE_DENIES : said(allowance(g->roles, g->nroles, guild->id));
}

// the channel's own entry, then the one of every channel, whose id is the
// guild's less one.
static GateSays gate_channel(const Gate *g, const Guild *guild,
                             Snowflake channel_id) {
  if (g == nullptr) {
    return GATE_SILENT;
  }

  const Allowance *a = allowance(g->channels, g->nchannels, channel_id);

  if (a == nullptr) {
    a = allowance(g->channels, g->nchannels, guild->id - 1);
  }

  return said(a);
}

static const Listed *listed_in(const Index *x, Snowflake application_id) {
  for (size_t i = 0; x != nullptr && i < x->napplications; i++) {
    if (x->applications[i].application.id == application_id) {
      return &x->applications[i];
    }
  }

  return nullptr;
}

// the context a channel is to the commands of an application: a guild, the
// conversation with its own bot, or any other private channel.
static uint8_t context_of(const Channel *c, const Listed *app) {
  if (c->guild_id != 0) {
    return CONTEXT_GUILD;
  }

  bool own = c->type == CHANNEL_DM && c->nrecipients == 1 && app != nullptr &&
             app->application.bot_id == c->recipients[0];

  return own ? CONTEXT_BOT_DM : CONTEXT_PRIVATE_CHANNEL;
}

// what viewing the channel and using commands in it takes, or being an
// administrator, without which a guild's index is not asked for.
static bool base_access(Session *s, const Channel *c) {
  Permissions perms = store_permissions(s->store, c);
  Permissions base = PERM_VIEW_CHANNEL | PERM_USE_APPLICATION_COMMANDS;

  return (perms & base) == base || (perms & PERM_ADMINISTRATOR) != 0;
}

// whether the current user may run a command of `x` in the channel. `mine`
// is the index of the user's account.
static bool usable(Session *s, const Channel *c, const Index *x,
                   const Index *mine, const Entry *e) {
  Store *store = s->store;
  const Listed *app = listed_in(x, e->command.application_id);

  // an age restricted command goes in an age restricted channel.
  if (e->nsfw && !(c->guild_id != 0 && c->nsfw)) {
    return false;
  }

  uint8_t context = context_of(c, app);

  if (e->has_contexts) {
    if ((e->contexts & context) == 0) {
      return false;
    }
  }
  else if ((!e->dm_permission && context == CONTEXT_BOT_DM) ||
           context == CONTEXT_PRIVATE_CHANNEL) {
    return false;
  }

  const Guild *guild = store_guild(store, c->guild_id);

  if (guild == nullptr) {
    return c->guild_id == 0;
  }

  Permissions perms = store_permissions(store, c);

  if ((perms & PERM_ADMINISTRATOR) != 0) {
    return true;
  }

  // one of an application on the user's account, installed for them.
  if (listed_in(mine, e->command.application_id) != nullptr &&
      (e->installs & INSTALL_USER) != 0) {
    return true;
  }

  // in a thread, sending is sending in threads, which the store folds.
  Permissions base = PERM_VIEW_CHANNEL | PERM_USE_APPLICATION_COMMANDS;
  bool reaches = (perms & base) == base && (perms & PERM_SEND_MESSAGES) != 0;

  if (!reaches && (!e->has_installs || (e->installs & INSTALL_GUILD) != 0)) {
    return false;
  }

  const Member *me = store_member(guild, store->self_id);
  const Gate *own = e->has_gate ? &e->gate : nullptr;
  const Gate *shared = app != nullptr && app->has_gate ? &app->gate : nullptr;
  // a thread is under its channel's entry.
  Snowflake channel_id = channel_is_thread(c) ? c->parent_id : c->id;
  GateSays where = gate_channel(own, guild, channel_id);

  if (where == GATE_DENIES ||
      (where == GATE_SILENT &&
       gate_channel(shared, guild, channel_id) == GATE_DENIES)) {
    return false;
  }

  GateSays who = gate_user(own, guild, me);

  if (who != GATE_SILENT) {
    return who == GATE_ALLOWS;
  }

  if (gate_user(shared, guild, me) == GATE_DENIES) {
    return false;
  }

  if (!e->has_member_permissions) {
    return true;
  }

  return e->member_permissions != 0 &&
         (perms & e->member_permissions) == e->member_permissions;
}

/* ========================================================================== */
/* the commands of a channel.                                                 */
/* ========================================================================== */

typedef struct {
  Slash *v;
  size_t len, cap;
} Slashes;

static void add_slash(Slashes *out, const Command *command,
                      const CommandOption *group, const CommandOption *sub) {
  char name[160];

  if (out->len == out->cap) {
    out->cap = out->cap != 0 ? out->cap * 2 : 64;
    out->v = xrealloc(out->v, out->cap * sizeof *out->v);
  }

  snprintf(name, sizeof name, "%s%s%s%s%s", command->shown,
           group != nullptr ? " " : "", group != nullptr ? group->shown : "",
           sub != nullptr ? " " : "", sub != nullptr ? sub->shown : "");

  const char *says = sub != nullptr ? sub->description : command->description;

  out->v[out->len++] = (Slash){
    .command = command,
    .group = group,
    .sub = sub,
    .name = str_dup(name),
    .description = says != nullptr ? says : "",
    .options = sub != nullptr ? sub->options : command->options,
    .noptions = sub != nullptr ? sub->noptions : command->noptions,
  };
}

// a command with subcommands is typed as each of them, the ones of its
// groups first.
static void add_command(Slashes *out, const Command *command) {
  bool nested = false;

  for (size_t i = 0; i < command->noptions; i++) {
    const CommandOption *o = &command->options[i];

    for (size_t j = 0; o->type == OPTION_SUB_COMMAND_GROUP && j < o->noptions;
         j++) {
      if (o->options[j].type == OPTION_SUB_COMMAND) {
        add_slash(out, command, o, &o->options[j]);
      }
    }

    nested |= o->type == OPTION_SUB_COMMAND_GROUP;
  }

  for (size_t i = 0; i < command->noptions; i++) {
    if (command->options[i].type == OPTION_SUB_COMMAND) {
      add_slash(out, command, nullptr, &command->options[i]);
      nested = true;
    }
  }

  if (!nested) {
    add_slash(out, command, nullptr, nullptr);
  }
}

static const char *application_name(Session *s, Snowflake id) {
  const CommandApplication *app = commands_application(s, id);

  return app != nullptr && app->name != nullptr ? app->name : "";
}

// each application's commands together and as they were listed, the
// applications by name, discord's own last.
static void sort_sections(Session *s, Slashes *out) {
  for (size_t i = 1; i < out->len; i++) {
    Slash moved = out->v[i];
    Snowflake app = moved.command->application_id;
    const char *name = application_name(s, app);
    size_t at = i;

    while (at > 0) {
      Snowflake before = out->v[at - 1].command->application_id;
      int order = 0;

      if (before != app) {
        order = before == COMMANDS_BUILT_IN ? 1
                : app == COMMANDS_BUILT_IN
                  ? -1
                  : strcasecmp(application_name(s, before), name);
        order = order != 0 ? order : before > app ? 1 : -1;
      }

      if (order <= 0) {
        break;
      }

      out->v[at] = out->v[at - 1];
      at--;
    }

    out->v[at] = moved;
  }
}

static bool listed_already(const Slashes *out, const Command *command) {
  for (size_t i = 0; i < out->len; i++) {
    if (out->v[i].command->id == command->id) {
      return true;
    }
  }

  return false;
}

static void free_slashes(struct Commands *cs) {
  for (size_t i = 0; i < cs->nslashes; i++) {
    free(cs->slashes[i].name);
  }

  free(cs->slashes);
  cs->slashes = nullptr;
  cs->nslashes = 0;
}

// the index a channel's commands come from beside the user's own: its
// guild's, or its own in the conversation with a bot. people talking have
// none.
static Index *context_index(Session *s, const Channel *c, bool ask) {
  struct Commands *cs = commands_of(s);

  if (c->guild_id != 0) {
    if (!base_access(s, c)) {
      return nullptr;
    }

    return ask ? index_want(s, c->guild_id, 0) : index_find(cs, c->guild_id, 0);
  }

  const User *other = c->type == CHANNEL_DM && c->nrecipients == 1
                        ? store_user(s->store, c->recipients[0])
                        : nullptr;

  if (other == nullptr || !other->bot) {
    return nullptr;
  }

  return ask ? index_want(s, 0, c->id) : index_find(cs, 0, c->id);
}

const Slash *commands_in(Session *s, const Channel *c, size_t *count) {
  *count = 0;

  if (c == nullptr) {
    return nullptr;
  }

  struct Commands *cs = commands_of(s);
  const Guild *guild = store_guild(s->store, c->guild_id);
  Index *here = context_index(s, c, true);
  Index *mine = index_want(s, 0, 0);
  uint64_t stamp = cs->version * 31 + (guild != nullptr ? guild->version : 0);

  if (cs->slashes != nullptr && cs->slashes_channel == c->id &&
      cs->slashes_stamp == stamp) {
    *count = cs->nslashes;
    return cs->slashes;
  }

  Slashes out = {};

  free_slashes(cs);

  // the ones of the user's account win over the same ones of the guild.
  for (size_t i = 0; mine != nullptr && i < mine->nentries; i++) {
    const Entry *e = &mine->entries[i];

    if (usable(s, c, mine, mine, e)) {
      add_command(&out, &e->command);
    }
  }

  for (size_t i = 0; here != nullptr && i < here->nentries; i++) {
    const Entry *e = &here->entries[i];

    if (!listed_already(&out, &e->command) && usable(s, c, here, mine, e)) {
      add_command(&out, &e->command);
    }
  }

  for (size_t i = 0; i < countof(built_ins); i++) {
    const Entry *e = &built_ins[i];

    if (e->offered == nullptr || e->offered(s, c)) {
      add_command(&out, &e->command);
    }
  }

  sort_sections(s, &out);

  cs->slashes = out.v;
  cs->nslashes = out.len;
  cs->slashes_channel = c->id;
  cs->slashes_stamp = stamp;
  *count = out.len;
  return out.v;
}

bool commands_loading(Session *s, const Channel *c) {
  struct Commands *cs = s->commands;

  if (cs == nullptr || c == nullptr) {
    return false;
  }

  const Index *here = context_index(s, c, false);
  const Index *mine = index_find(cs, 0, 0);

  // one discord is still making counts as on its way.
  return (here != nullptr && (here->loading || here->tries != 0)) ||
         (mine != nullptr && (mine->loading || mine->tries != 0));
}

const CommandApplication *commands_application(Session *s,
                                               Snowflake application_id) {
  struct Commands *cs = s->commands;

  if (application_id == COMMANDS_BUILT_IN) {
    return &built_in_application;
  }

  for (size_t i = 0; cs != nullptr && i < cs->nindexes; i++) {
    const Listed *listed = listed_in(cs->indexes[i], application_id);

    if (listed != nullptr) {
      return &listed->application;
    }
  }

  return nullptr;
}

/* ========================================================================== */
/* how well a command answers what was typed.                                 */
/* ========================================================================== */

static bool starts(const char *text, const char *with) {
  return text != nullptr && strncasecmp(text, with, strlen(with)) == 0;
}

static bool holds(const char *text, const char *what) {
  return text != nullptr && strcasestr(text, what) != nullptr;
}

// every character of `query` is in `text`, in its order.
static bool fuzzy(const char *query, const char *text) {
  if (text == nullptr) {
    return false;
  }

  for (; *query != '\0'; query++, text++) {
    while (*text != '\0' &&
           tolower((unsigned char)*text) != tolower((unsigned char)*query)) {
      text++;
    }

    if (*text == '\0') {
      return false;
    }
  }

  return true;
}

// the name after its first word starts with what follows the query's.
static bool starts_by_words(const char *name, const char *query) {
  const char *rest = strchr(query, ' ');
  const char *after = strchr(name, ' ');
  size_t first = rest != nullptr ? (size_t)(rest - query) : strlen(query);

  if (strncasecmp(name, query, first) != 0) {
    return false;
  }

  return starts(after != nullptr ? after + 1 : "",
                rest != nullptr ? rest + 1 : "");
}

int commands_score(Session *s, const Slash *slash, const char *query) {
  const Command *command = slash->command;
  const CommandApplication *app =
    commands_application(s, command->application_id);
  const char *says = slash->description;
  char known[160];
  bool option_holds = false, option_fuzzy = false;
  int score = -1;

  // the words discord knows it by, beside the ones the user reads.
  snprintf(known, sizeof known, "%s%s%s%s%s", command->name,
           slash->group != nullptr ? " " : "",
           slash->group != nullptr ? slash->group->name : "",
           slash->sub != nullptr ? " " : "",
           slash->sub != nullptr ? slash->sub->name : "");

  if (starts(slash->name, query) || starts(known, query)) {
    score = 0;
  }
  else if (starts_by_words(slash->name, query) ||
           starts_by_words(known, query)) {
    score = 1;
  }
  else if (holds(slash->name, query) || holds(known, query)) {
    score = 2;
  }

  for (size_t i = 0; score < 0 && i < slash->noptions; i++) {
    const CommandOption *o = &slash->options[i];
    char with[280];

    snprintf(with, sizeof with, "%s %s", slash->name, o->shown);

    if (starts(o->name, query) || starts(o->shown, query) ||
        starts(with, query)) {
      score = 3;
    }

    option_holds |= holds(o->name, query) || holds(o->shown, query);
    option_fuzzy |= fuzzy(query, o->name) || fuzzy(query, o->shown);
  }

  if (score < 0) {
    score = option_holds                                       ? 4
            : holds(says, query)                               ? 7
            : fuzzy(query, slash->name) || fuzzy(query, known) ? 9
            : option_fuzzy                                     ? 10
            : fuzzy(query, says)                               ? 12
                                                               : -1;
  }

  // its application answers for every command it has.
  int shared = app == nullptr                   ? -1
               : starts(app->name, query)       ? 5
               : holds(app->name, query)        ? 6
               : holds(app->description, query) ? 8
               : fuzzy(query, app->name)        ? 11
                                                : -1;

  return score < 0 || (shared >= 0 && shared < score) ? shared : score;
}

/* ========================================================================== */
/* running discord's own.                                                     */
/* ========================================================================== */

static const char *value_of(const CommandValue *values, size_t count,
                            const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(values[i].option->name, name) == 0) {
      return values[i].text;
    }
  }

  return nullptr;
}

// `text` without the blanks around it, from malloc.
static char *trimmed(const char *text) {
  size_t len = strlen(text);

  while (len != 0 && (text[len - 1] == ' ' || text[len - 1] == '\n')) {
    len--;
  }

  while (len != 0 && (*text == ' ' || *text == '\n')) {
    text++;
    len--;
  }

  return str_dup_n(text, len);
}

static void say(Session *s, Snowflake channel_id, const char *format,
                const char *message) {
  size_t cap = strlen(format) + strlen(message) + 1;
  char *joined = xcalloc(1, cap);

  snprintf(joined, cap, format, message);

  char *text = trimmed(joined);

  session_send(s, channel_id, text, 0, false);
  free(text);
  free(joined);
}

static void on_tts(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;

  if (ask_end(ask) && !res->ok) {
    notify(s, res->error.message, true);
    changed(s);
  }
}

// a message read aloud, which session_send has no way to ask for. it shows
// once discord tells of it.
static void say_aloud(Session *s, Snowflake channel_id, const char *message) {
  if (!online(s)) {
    session_send(s, channel_id, message, 0, false);
    return;
  }

  cima_json_writer w = {};
  char path[64];

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/messages", channel_id);
  json_begin_object(&w);
  json_field(&w, "content", message);
  json_key(&w, "nonce"), snowflake_write(&w, new_nonce(s));
  json_field(&w, "tts", true);
  json_field(&w, "flags", (int64_t)0);
  json_end_object(&w);
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = path,
                            .route = "/channels/:id/messages",
                            .body = cima_json_writer_text(&w)},
              on_tts, ask_new(s, ASK_TTS));
  cima_json_writer_dispose(&w);
}

static void on_thread(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  char *text = ask->text;

  ask->text = nullptr;

  if (!ask_end(ask)) {
    free(text);
    return;
  }

  if (res->ok) {
    // the gateway tells of the thread too, and telling twice changes
    // nothing.
    const Channel *thread = store_put_channel(s->store, res->json, 0);

    if (thread != nullptr) {
      store_set_thread_joined(s->store, thread->id, true);
      session_send(s, thread->id, text, 0, false);
    }
  }
  else {
    notify(s, res->error.message, true);
  }

  free(text);
  changed(s);
}

static void start_thread(Session *s, const Channel *c, const char *name,
                         const char *message) {
  if (!online(s)) {
    notify(s, "Starting a thread needs a connection.", true);
    return;
  }

  cima_json_writer w = {};
  char path[64];
  Ask *ask = ask_new(s, ASK_THREAD);

  ask->text = str_dup(message);
  snprintf(path, sizeof path, "/channels/%" PRIu64 "/threads", c->id);
  json_begin_object(&w);
  json_field(&w, "name", name);
  json_field(&w, "type",
             (int64_t)(c->type == CHANNEL_GUILD_ANNOUNCEMENT
                         ? CHANNEL_ANNOUNCEMENT_THREAD
                         : CHANNEL_PUBLIC_THREAD));
  // three days, what the client asks for a channel that says nothing.
  json_field(&w, "auto_archive_duration", (int64_t)4320);
  json_field(&w, "location", "Slash Command");
  json_end_object(&w);
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = path,
                            .route = "/channels/:id/threads",
                            .body = cima_json_writer_text(&w)},
              on_thread, ask);
  cima_json_writer_dispose(&w);
}

static void on_moderated(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  Builtin builtin = ask->builtin;
  char *done = ask->text;
  const char *deed = builtin == BUILTIN_KICK  ? "kick"
                     : builtin == BUILTIN_BAN ? "ban"
                                              : "timeout";
  char line[160];

  ask->text = nullptr;

  if (!ask_end(ask)) {
    free(done);
    return;
  }

  if (res->ok) {
    notify(s, done, false);
  }
  else if (res->status == 403) {
    snprintf(line, sizeof line,
             "You do not have the appropriate permissions to %s that user.",
             deed);
    notify(s, line, true);
  }
  else {
    snprintf(line, sizeof line,
             "Oops! We encountered an error when trying to %s that user.",
             deed);
    notify(s, line, true);
  }

  free(done);
  changed(s);
}

// kicks, bans or times out a member, as the commands of discord's client
// do, and tells how it went.
static void moderate(Session *s, const Channel *c, Builtin builtin,
                     const CommandValue *values, size_t count) {
  Snowflake user_id = snowflake_parse(value_of(values, count, "user"));
  const char *reason = value_of(values, count, "reason");
  const User *user = store_user(s->store, user_id);
  const char *name =
    user != nullptr && user->username != nullptr ? user->username : "The user";
  cima_json_writer w = {};
  char path[96], done[160];
  const char *method = "DELETE", *route = "/guilds/:id/members/:id";
  bool body = false;

  if (user_id == 0 || c->guild_id == 0) {
    return;
  }

  if (!online(s)) {
    notify(s, "Moderating needs a connection.", true);
    return;
  }

  snprintf(path, sizeof path, "/guilds/%" PRIu64 "/members/%" PRIu64,
           c->guild_id, user_id);
  json_begin_object(&w);

  if (builtin == BUILTIN_KICK) {
    snprintf(done, sizeof done, "%s was kicked from the server.", name);
  }
  else if (builtin == BUILTIN_BAN) {
    const char *seconds = value_of(values, count, "delete_messages");

    method = "PUT";
    route = "/guilds/:id/bans/:id";
    body = true;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/bans/%" PRIu64,
             c->guild_id, user_id);
    json_field(
      &w, "delete_message_seconds",
      (int64_t)strtoll(seconds != nullptr ? seconds : "0", nullptr, 10));
    snprintf(done, sizeof done, "%s was banned from the server.", name);
  }
  else {
    const char *typed = value_of(values, count, "duration");
    int64_t seconds = strtoll(typed != nullptr ? typed : "60", nullptr, 10);
    time_t until = (time_t)store_now(s->store) + (time_t)seconds;
    struct tm tm;
    char stamp[40];

    gmtime_r(&until, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    method = "PATCH";
    body = true;
    json_field(&w, "communication_disabled_until", stamp);
    snprintf(done, sizeof done, "%s timed out for %" PRId64 " seconds.", name,
             seconds);
  }

  json_end_object(&w);

  Ask *ask = ask_new(s, ASK_MODERATE);

  ask->builtin = builtin;
  ask->text = str_dup(done);
  api_request(
    s->api,
    &(ApiRequest){
      .method = method,
      .path = path,
      .route = route,
      .body = body ? cima_json_writer_text(&w) : nullptr,
      .reason = reason != nullptr && *reason != '\0' ? reason : nullptr,
    },
    on_moderated, ask);
  cima_json_writer_dispose(&w);
}

static void run_built_in(Session *s, const Channel *c, const Slash *slash,
                         const CommandValue *values, size_t count) {
  const char *typed = value_of(values, count, "message");
  const char *message = typed != nullptr ? typed : "";

  switch (slash->command->builtin) {
    case BUILTIN_SHRUG:
      say(s, c->id, "%s ¯\\_(ツ)_/¯", message);
      break;
    case BUILTIN_TABLEFLIP:
      say(s, c->id, "%s (╯°□°)╯︵ ┻━┻", message);
      break;
    case BUILTIN_UNFLIP:
      say(s, c->id, "%s ┬─┬ノ( º _ ºノ)", message);
      break;
    case BUILTIN_ME:
      say(s, c->id, "_%s_", message);
      break;
    case BUILTIN_SPOILER:
      say(s, c->id, "||%s||", message);
      break;
    case BUILTIN_TTS:
      say_aloud(s, c->id, message);
      break;
    case BUILTIN_NICK: {
      const char *nick = value_of(values, count, "new_nick");
      char line[160];

      session_set_nick(s, c->guild_id, nick);

      if (nick != nullptr && *nick != '\0') {
        snprintf(line, sizeof line,
                 "Your nickname on this server has been changed to %s.", nick);
        notify(s, line, false);
      }
      else {
        notify(s, "Your nickname on this server has been reset.", false);
      }

      break;
    }
    case BUILTIN_THREAD:
      start_thread(s, c, value_of(values, count, "name"), message);
      break;
    case BUILTIN_KICK:
    case BUILTIN_BAN:
    case BUILTIN_TIMEOUT:
      moderate(s, c, slash->command->builtin, values, count);
      break;
    case BUILTIN_MSG:
      session_message_user(s, snowflake_parse(value_of(values, count, "user")),
                           message);
      break;
    default:
      break;
  }
}

/* ========================================================================== */
/* interactions.                                                              */
/* ========================================================================== */

static Waiting *waiting_find(struct Commands *cs, Snowflake nonce) {
  for (Waiting *w = cs != nullptr ? cs->waiting : nullptr; w != nullptr;
       w = w->next) {
    if (w->nonce == nonce && nonce != 0) {
      return w;
    }
  }

  return nullptr;
}

static void waiting_end(Waiting *w) {
  struct Commands *cs = w->s->commands;

  for (Waiting **at = &cs->waiting; *at != nullptr; at = &(*at)->next) {
    if (*at == w) {
      *at = w->next;
      break;
    }
  }

  // the component is no longer shown as busy.
  if (w->message_id != 0) {
    Channel *c = store_channel(w->s->store, w->channel_id);

    if (c != nullptr) {
      store_touch_channel(w->s->store, c);
    }
  }

  platform_timer_cancel(w->timer);
  free(w->custom_id);
  free(w);
}

// the pending message turns into a failure the user dismisses. null for
// what discord's client says of one it knows nothing about.
static void fail(Session *s, Snowflake nonce, const char *why) {
  Waiting *w = waiting_find(s->commands, nonce);

  if (w == nullptr) {
    return;
  }

  // the client tells a late failure as the application's silence.
  int64_t age =
    (int64_t)(store_now(s->store) * 1000) - snowflake_time_ms(nonce);

  if (why == nullptr) {
    why = "This interaction failed";
  }

  if (age > (int64_t)(SILENCE_AFTER * 1000)) {
    why = "The application did not respond";
  }

  // a component has no pending message to tell it on.
  if (w->message_id != 0) {
    notify(s, why, true);
  }
  else {
    store_fail_pending(s->store, w->channel_id, nonce, why);
  }

  waiting_end(w);
  changed(s);
}

static void on_patience(void *ctx) {
  Waiting *w = ctx;
  Session *s = w->s;

  w->timer = nullptr;

  // an application that answered and whose message never came leaves
  // nothing to show.
  if (w->answered) {
    store_remove_pending(s->store, w->channel_id, w->nonce);
    waiting_end(w);
    changed(s);
    return;
  }

  fail(s, w->nonce, "The application did not respond");
}

// discord no longer knows a command as the indexes have it: the ones it may
// have come from are asked for again the next time they are listed.
static void indexes_stale(Session *s, Snowflake guild_id,
                          Snowflake channel_id) {
  struct Commands *cs = s->commands;

  index_stale(cs, guild_id != 0 ? index_find(cs, guild_id, 0)
                                : index_find(cs, 0, channel_id));
  index_stale(cs, index_find(cs, 0, 0));
}

static void on_run(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  Snowflake nonce = ask->nonce;
  Snowflake guild_id = ask->guild_id, channel_id = ask->channel_id;

  if (!ask_end(ask) || res->ok) {
    return;
  }

  bool outdated = res->error.code == UNKNOWN_INTEGRATION;

  for (size_t i = 0; i < res->error.nfields; i++) {
    const char *code = res->error.fields[i].code;

    outdated |= code != nullptr && (strcmp(code, BAD_VERSION) == 0 ||
                                    strcmp(code, BAD_COMMAND) == 0);
  }

  if (outdated) {
    indexes_stale(s, guild_id, channel_id);
  }

  fail(s, nonce, res->status != 0 ? res->error.message : nullptr);
}

// a value as the option's type has it written. the one being typed is a
// string whatever its type.
static void write_value(cima_json_writer *w, const CommandValue *v) {
  const char *text = v->text != nullptr ? v->text : "";

  json_key(w, "value");

  if (v->focused) {
    cima_json_write_string(w, text);
    return;
  }

  switch (v->option->type) {
    case OPTION_INTEGER: {
      double number = strtod(text, nullptr);

      if (fabs(number) < 0x1p63) {
        cima_json_write_int(w, (int64_t)number);
      }
      else {
        cima_json_write_double(w, number);
      }

      break;
    }
    case OPTION_NUMBER:
      cima_json_write_double(w, strtod(text, nullptr));
      break;
    case OPTION_BOOLEAN:
      cima_json_write_bool(w, strcasecmp(text, "true") == 0);
      break;
    default:
      cima_json_write_string(w, text);
      break;
  }
}

// the options as discord nests them: under the subcommand, itself under its
// group.
static void write_options(cima_json_writer *w, const Slash *slash,
                          const CommandValue *values, size_t count) {
  const CommandOption *nest[] = {slash->group, slash->sub};
  int depth = 0;

  json_key(w, "options");
  json_begin_array(w);

  for (size_t i = 0; i < countof(nest); i++) {
    if (nest[i] != nullptr) {
      json_begin_object(w);
      json_field(w, "type", (int64_t)nest[i]->type);
      json_field(w, "name", nest[i]->name);
      json_key(w, "options");
      json_begin_array(w);
      depth++;
    }
  }

  for (size_t i = 0; i < count; i++) {
    json_begin_object(w);
    json_field(w, "type", (int64_t)values[i].option->type);
    json_field(w, "name", values[i].option->name);
    write_value(w, &values[i]);

    if (values[i].focused) {
      json_field(w, "focused", true);
    }

    json_end_object(w);
  }

  while (depth-- > 0) {
    json_end_array(w);
    json_end_object(w);
  }

  json_end_array(w);
}

// the body of an interaction about a command, 2 to run it and 4 to have
// its application complete the option being typed, in the order discord's
// client writes it.
static void write_interaction(Session *s, cima_json_writer *w, int type,
                              const Channel *c, const Slash *slash,
                              const CommandValue *values, size_t count,
                              Snowflake nonce) {
  const Command *command = slash->command;
  const char *session_id =
    s->gateway != nullptr ? gateway_session_id(s->gateway) : nullptr;

  json_begin_object(w);
  json_field(w, "type", (int64_t)type);
  json_key(w, "application_id"), snowflake_write(w, command->application_id);

  if (c->guild_id != 0) {
    json_key(w, "guild_id"), snowflake_write(w, c->guild_id);
  }

  json_key(w, "channel_id"), snowflake_write(w, c->id);
  json_field(w, "session_id", session_id != nullptr ? session_id : "");
  json_key(w, "data");
  json_begin_object(w);
  json_key(w, "version"), snowflake_write(w, command->version);
  json_key(w, "id"), snowflake_write(w, command->id);

  if (command->guild_id != 0) {
    json_key(w, "guild_id"), snowflake_write(w, command->guild_id);
  }

  json_field(w, "name", command->name);
  json_field(w, "type", (int64_t)1);
  write_options(w, slash, values, count);

  if (command->json != nullptr) {
    json_key(w, "application_command");
    cima_json_write_raw(w, command->json, strlen(command->json));
  }

  // a command is run with the files of its options, of which Quarrel
  // sends none yet.
  if (type == 2) {
    json_key(w, "attachments");
    json_begin_array(w);
    json_end_array(w);
  }

  json_end_object(w);
  json_key(w, "nonce"), snowflake_write(w, nonce);

  if (type == 2) {
    json_field(w, "analytics_location", "slash_ui");
  }

  json_end_object(w);
}

void commands_run(Session *s, Snowflake channel_id, const Slash *slash,
                  const CommandValue *values, size_t count) {
  Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || slash == nullptr) {
    return;
  }

  if (slash->command->builtin != BUILTIN_NONE) {
    run_built_in(s, c, slash, values, count);
    changed(s);
    return;
  }

  struct Commands *cs = commands_of(s);
  const CommandApplication *app =
    commands_application(s, slash->command->application_id);
  Snowflake nonce = new_nonce(s);
  Snowflake author = app != nullptr && app->bot_id != 0
                       ? app->bot_id
                       : slash->command->application_id;

  store_add_pending_command(s->store, c, nonce, author, slash->name);

  if (!online(s) || s->gateway == nullptr) {
    store_fail_pending(s->store, channel_id, nonce, "Not connected.");
    changed(s);
    return;
  }

  Waiting *waiting = xcalloc(1, sizeof *waiting);

  *waiting = (Waiting){
    .next = cs->waiting,
    .s = s,
    .nonce = nonce,
    .channel_id = channel_id,
    .guild_id = c->guild_id,
    .application_id = slash->command->application_id,
  };
  waiting->timer = platform_timer(INTERACTION_PATIENCE, on_patience, waiting);
  cs->waiting = waiting;

  cima_json_writer w = {};
  Ask *ask = ask_new(s, ASK_RUN);

  ask->nonce = nonce;
  ask->guild_id = c->guild_id;
  ask->channel_id = channel_id;
  write_interaction(s, &w, 2, c, slash, values, count, nonce);
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = "/interactions",
                            .body = cima_json_writer_text(&w)},
              on_run, ask);
  cima_json_writer_dispose(&w);
  changed(s);
}

/* ========================================================================== */
/* components.                                                                */
/* ========================================================================== */

bool commands_component_busy(const Session *s, Snowflake message_id,
                             const char *custom_id) {
  const struct Commands *cs = s->commands;

  for (const Waiting *w = cs != nullptr ? cs->waiting : nullptr; w != nullptr;
       w = w->next) {
    if (w->message_id == message_id && w->custom_id != nullptr &&
        custom_id != nullptr && strcmp(w->custom_id, custom_id) == 0) {
      return true;
    }
  }

  return false;
}

static void on_component(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  Snowflake nonce = ask->nonce;

  // the answer comes through the gateway: only a refusal ends it here.
  if (!ask_end(ask) || res->ok) {
    return;
  }

  fail(s, nonce, res->status != 0 ? res->error.message : nullptr);
}

void commands_component(Session *s, const Message *m, const Component *c,
                        const char *const *values, size_t count) {
  Channel *channel = store_channel(s->store, m->channel_id);
  const char *session_id =
    s->gateway != nullptr ? gateway_session_id(s->gateway) : nullptr;

  if (channel == nullptr || c->custom_id == nullptr || m->id == 0 ||
      commands_component_busy(s, m->id, c->custom_id)) {
    return;
  }

  if (!online(s) || session_id == nullptr) {
    notify(s, "Not connected.", true);
    changed(s);
    return;
  }

  struct Commands *cs = commands_of(s);
  Snowflake nonce = new_nonce(s);
  // a webhook's message names its application, a bot is its own.
  Snowflake application_id =
    m->application_id != 0 ? m->application_id : m->author_id;
  Waiting *waiting = xcalloc(1, sizeof *waiting);

  *waiting = (Waiting){
    .next = cs->waiting,
    .s = s,
    .nonce = nonce,
    .channel_id = channel->id,
    .guild_id = channel->guild_id,
    .application_id = application_id,
    .message_id = m->id,
    .custom_id = str_dup(c->custom_id),
  };
  waiting->timer = platform_timer(INTERACTION_PATIENCE, on_patience, waiting);
  cs->waiting = waiting;

  // in the order discord's client writes it.
  cima_json_writer w = {};
  Ask *ask = ask_new(s, ASK_RUN);

  ask->nonce = nonce;
  ask->guild_id = channel->guild_id;
  ask->channel_id = channel->id;

  json_begin_object(&w);
  json_field(&w, "type", (int64_t)3);
  json_key(&w, "nonce"), snowflake_write(&w, nonce);

  if (channel->guild_id != 0) {
    json_key(&w, "guild_id"), snowflake_write(&w, channel->guild_id);
  }

  json_key(&w, "channel_id"), snowflake_write(&w, channel->id);
  json_field(&w, "message_flags", (int64_t)m->flags);
  json_key(&w, "message_id"), snowflake_write(&w, m->id);
  json_key(&w, "application_id"), snowflake_write(&w, application_id);
  json_field(&w, "session_id", session_id);
  json_key(&w, "data");
  json_begin_object(&w);
  json_field(&w, "component_type", (int64_t)c->type);
  json_field(&w, "custom_id", c->custom_id);

  if (c->type != COMPONENT_BUTTON) {
    json_field(&w, "type", (int64_t)c->type);
    json_key(&w, "values");
    json_begin_array(&w);

    for (size_t i = 0; i < count; i++) {
      cima_json_write_string(&w, values[i]);
    }

    json_end_array(&w);
  }

  json_end_object(&w);
  json_end_object(&w);

  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = "/interactions",
                            .body = cima_json_writer_text(&w)},
              on_component, ask);
  cima_json_writer_dispose(&w);
  store_touch_channel(s->store, channel);
  changed(s);
}

/* ========================================================================== */
/* what an application completes an option with.                              */
/* ========================================================================== */

static void free_choices(struct Commands *cs) {
  for (size_t i = 0; i < cs->nchoices; i++) {
    free(cs->choices[i].name);
    free(cs->choices[i].value);
  }

  free(cs->choices);
  cs->choices = nullptr;
  cs->nchoices = 0;
}

static void suggest_failed(struct Commands *cs) {
  platform_timer_cancel(cs->suggest_patience);
  cs->suggest_patience = nullptr;
  cs->suggest_loading = false;
  cs->suggest_failed = true;
  changed(cs->session);
}

static void on_suggest_patience(void *ctx) {
  struct Commands *cs = ctx;

  cs->suggest_patience = nullptr;
  suggest_failed(cs);
}

static void on_suggest(void *ctx, const ApiResponse *res) {
  Ask *ask = ctx;
  Session *s = ask->s;
  Snowflake nonce = ask->nonce;

  if (!ask_end(ask) || res->ok) {
    return;
  }

  // the answer comes through the gateway: only a refusal ends it here.
  if (s->commands->suggest_nonce == nonce) {
    suggest_failed(s->commands);
  }
}

static void on_suggest_rest(void *ctx);

static void suggest_send(struct Commands *cs) {
  Session *s = cs->session;
  Ask *ask = ask_new(s, ASK_SUGGEST);

  ask->nonce = cs->suggest_nonce;
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = "/interactions",
                            .body = cs->suggest_body},
              on_suggest, ask);
  str_set(&cs->suggest_body, nullptr);

  platform_timer_cancel(cs->suggest_patience);
  cs->suggest_patience =
    platform_timer(SUGGEST_PATIENCE, on_suggest_patience, cs);
  cs->suggest_rest = platform_timer(SUGGEST_REST, on_suggest_rest, cs);
}

// the rest is over: what was typed during it is asked now.
static void on_suggest_rest(void *ctx) {
  struct Commands *cs = ctx;

  cs->suggest_rest = nullptr;

  if (cs->suggest_body != nullptr && online(cs->session)) {
    suggest_send(cs);
  }
}

static uint64_t mix(uint64_t h, const char *text) {
  for (const char *c = text != nullptr ? text : ""; *c != '\0'; c++) {
    h = (h ^ (uint8_t)*c) * 0x100000001b3ull;
  }

  return (h ^ 0xff) * 0x100000001b3ull;
}

void commands_suggest(Session *s, Snowflake channel_id, const Slash *slash,
                      const CommandValue *values, size_t count) {
  const Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || slash == nullptr ||
      slash->command->builtin != BUILTIN_NONE || !online(s) ||
      s->gateway == nullptr) {
    return;
  }

  struct Commands *cs = commands_of(s);
  uint64_t key =
    mix(0xcbf29ce484222325ull ^ channel_id ^ slash->command->id, slash->name);

  for (size_t i = 0; i < count; i++) {
    key = mix(key, values[i].option->name);
    key = mix(key, values[i].text);
    key ^= values[i].focused;
  }

  if (key == cs->suggest_key) {
    return;
  }

  cima_json_writer w = {};

  cs->suggest_key = key;
  cs->suggest_nonce = new_nonce(s);
  cs->suggest_loading = true;
  cs->suggest_failed = false;
  free_choices(cs);
  write_interaction(s, &w, 4, c, slash, values, count, cs->suggest_nonce);
  str_set(&cs->suggest_body, cima_json_writer_text(&w));
  cima_json_writer_dispose(&w);

  // asked at once, unless one was within the rest: then when it ends.
  if (cs->suggest_rest == nullptr) {
    suggest_send(cs);
  }
}

const CommandChoice *commands_suggestions(const Session *s, size_t *count,
                                          bool *loading, bool *failed) {
  const struct Commands *cs = s->commands;

  *count = cs != nullptr ? cs->nchoices : 0;
  *loading = cs != nullptr && cs->suggest_loading;
  *failed = cs != nullptr && cs->suggest_failed;
  return cs != nullptr ? cs->choices : nullptr;
}

/* ========================================================================== */
/* what the gateway tells.                                                    */
/* ========================================================================== */

// the application was told: its pending message says it thinks.
static void on_created(Session *s, Waiting *w) {
  Channel *c = store_channel(s->store, w->channel_id);
  Message *m = c != nullptr ? store_message(c, w->nonce) : nullptr;

  if (m != nullptr && m->state == MESSAGE_PENDING) {
    m->flags |= MESSAGE_FLAG_LOADING;
    store_touch_channel(s->store, c);
  }
}

static void on_failure(Session *s, const cima_json *data, Snowflake nonce) {
  struct Commands *cs = s->commands;
  Waiting *w = waiting_find(cs, nonce);
  char why[160];

  if (nonce != 0 && nonce == cs->suggest_nonce && cs->suggest_loading) {
    suggest_failed(cs);
  }

  if (w == nullptr) {
    return;
  }

  // 2 for an application that took too long.
  if (json_get_coerce(data, "reason_code", (int64_t)0) == 2) {
    const CommandApplication *app = commands_application(s, w->application_id);

    snprintf(why, sizeof why, "%s didn't respond in time",
             app != nullptr && app->name != nullptr ? app->name
                                                    : "The application");
    fail(s, nonce, why);
    return;
  }

  fail(s, nonce, nullptr);
}

void commands_dispatch(Session *s, const char *type, const cima_json *data) {
  struct Commands *cs = s->commands;

  if (cs == nullptr) {
    return;
  }

  Snowflake nonce = snowflake_of(json_get(data, "nonce"));
  Waiting *w = waiting_find(cs, nonce);

  if (strcmp(type, "MESSAGE_CREATE") == 0) {
    // the application's answer takes the place of the pending message.
    if (w != nullptr) {
      store_remove_pending(s->store, w->channel_id, nonce);
      waiting_end(w);
    }
  }
  else if (strcmp(type, "INTERACTION_CREATE") == 0) {
    if (w != nullptr) {
      on_created(s, w);
    }
  }
  else if (strcmp(type, "INTERACTION_SUCCESS") == 0) {
    // a component may be answered by nothing more than this.
    if (w != nullptr && w->message_id != 0) {
      waiting_end(w);
      changed(s);
    }
    // the message that answers is on its way, or already came.
    else if (w != nullptr) {
      w->answered = true;
      platform_timer_cancel(w->timer);
      w->timer = platform_timer(ANSWER_PATIENCE, on_patience, w);
    }
  }
  else if (strcmp(type, "INTERACTION_FAILURE") == 0) {
    on_failure(s, data, nonce);
  }
  else if (strcmp(type, "INTERACTION_MODAL_CREATE") == 0 ||
           strcmp(type, "INTERACTION_IFRAME_MODAL_CREATE") == 0) {
    if (w != nullptr) {
      notify(s,
             w->message_id != 0
               ? "This opens a form, which Quarrel cannot show yet."
               : "This command opens a form, which Quarrel cannot show yet.",
             true);
      store_remove_pending(s->store, w->channel_id, nonce);
      waiting_end(w);
    }
  }
  else if (strcmp(type, "APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE") == 0) {
    const cima_json *choices = json_get(data, "choices");

    if (nonce == 0 || nonce != cs->suggest_nonce) {
      return;
    }

    free_choices(cs);
    platform_timer_cancel(cs->suggest_patience);
    cs->suggest_patience = nullptr;
    cs->suggest_loading = false;
    cs->suggest_failed = false;

    if (json_len(choices) != 0) {
      cs->choices = xcalloc(json_len(choices), sizeof *cs->choices);

      CIMA_EACH(choice, json_items(choices)) {
        cs->choices[cs->nchoices++] = put_choice(choice);
      }
    }
  }
  else if (strcmp(type, "GUILD_APPLICATION_COMMAND_INDEX_UPDATE") == 0) {
    index_stale(cs,
                index_find(cs, snowflake_of(json_get(data, "guild_id")), 0));
  }
  else if (strcmp(type, "USER_APPLICATION_UPDATE") == 0 ||
           strcmp(type, "USER_APPLICATION_REMOVE") == 0) {
    index_stale(cs, index_find(cs, 0, 0));
  }
  else if (strcmp(type, "READY") == 0) {
    // a new connection may have missed what changed.
    for (size_t i = 0; i < cs->nindexes; i++) {
      cs->indexes[i]->stale = true;
    }

    cs->version++;
  }
}

/* ========================================================================== */
/* the end.                                                                   */
/* ========================================================================== */

static void empty(struct Commands *cs) {
  for (size_t i = 0; i < cs->nindexes; i++) {
    free_index(cs->indexes[i]);
    free(cs->indexes[i]);
  }

  while (cs->waiting != nullptr) {
    Waiting *next = cs->waiting->next;

    platform_timer_cancel(cs->waiting->timer);
    free(cs->waiting->custom_id);
    free(cs->waiting);
    cs->waiting = next;
  }

  free(cs->indexes);
  cs->indexes = nullptr;
  cs->nindexes = 0;
  free_slashes(cs);
  free_choices(cs);
  platform_timer_cancel(cs->retry);
  platform_timer_cancel(cs->suggest_rest);
  platform_timer_cancel(cs->suggest_patience);
  cs->retry = cs->suggest_rest = cs->suggest_patience = nullptr;
  str_set(&cs->suggest_body, nullptr);
  cs->suggest_key = 0;
  cs->suggest_nonce = 0;
  cs->suggest_loading = cs->suggest_failed = false;
  cs->version++;
}

void commands_clear(Session *s) {
  if (s->commands != nullptr) {
    empty(s->commands);
  }
}

void commands_free(struct Commands *cs) {
  if (cs == nullptr) {
    return;
  }

  empty(cs);

  // the api is gone, none of these is answered any more.
  while (cs->asks != nullptr) {
    Ask *next = cs->asks->next;

    free(cs->asks->text);
    free(cs->asks);
    cs->asks = next;
  }

  free(cs);
}
