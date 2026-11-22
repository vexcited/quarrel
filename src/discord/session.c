#include "discord/session.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "discord/api.h"
#include "discord/gateway.h"
#include "discord/remote_auth.h"
#include "discord/settings_proto.h"
#include "http.h"
#include "platform/platform.h"
#include "voice/voice.h"

// the keychain holds one token per account, under "token:" and its id. the
// bare key is where the only token lived before accounts could be switched.
#define LEGACY_TOKEN_KEY "token"
#define ACCOUNTS_KEY "accounts"
#define LAST_ACCOUNT_KEY "last_account"

constexpr uint32_t HISTORY_PAGE = 50;
constexpr size_t KEEP_UNSELECTED = 100;
constexpr double TYPING_INTERVAL = 8;
constexpr uint32_t LIST_BLOCK = 100;

// a request whose answer needs to know what it was about. each lives in the
// session's list until it is answered, so none is lost when the session ends
// first.
typedef struct Call Call;

struct Call {
  Call *next;
  Session *s;
  Snowflake channel_id, id;
  HistoryKind kind;
  uint64_t epoch;
};

static Call *call_new(Session *s, Snowflake channel_id, Snowflake id) {
  Call *call = calloc(1, sizeof *call);

  if (call == nullptr) {
    cilo_oom(sizeof *call);
  }

  call->s = s;
  call->channel_id = channel_id;
  call->id = id;
  call->epoch = s->epoch;
  call->next = s->calls;
  s->calls = call;
  return call;
}

static void call_end(Call *call) {
  for (Call **at = (Call **)&call->s->calls; *at != nullptr;
       at = &(*at)->next) {
    if (*at == call) {
      *at = call->next;
      break;
    }
  }

  free(call);
}

// the answer is for an account the session left.
static bool call_stale(Call *call) {
  if (call->epoch == call->s->epoch) {
    return false;
  }

  call_end(call);
  return true;
}

static void changed(Session *s) {
  s->version++;

  if (s->options.on_change != nullptr) {
    s->options.on_change();
  }
}

static void set_state(Session *s, SessionState state) {
  s->state = state;
  changed(s);
}

static void set_error(Session *s, const char *error) {
  str_set(&s->error, error);
}

static void wipe(char **secret) {
  if (*secret == nullptr) {
    return;
  }

  // through a volatile pointer, so the compiler keeps the writes.
  volatile char *at = *secret;

  while (*at != '\0') {
    *at++ = '\0';
  }

  free(*secret);
  *secret = nullptr;
}

static void clear_captcha(Session *s) {
  free(s->captcha.service);
  free(s->captcha.sitekey);
  free(s->captcha.rqdata);
  free(s->captcha.session_id);
  free(s->captcha.rqtoken);
  s->captcha = (SessionCaptcha){};
}

static void clear_login(Session *s) {
  wipe(&s->login);
  wipe(&s->password);
  wipe(&s->mfa_ticket);
  free(s->mfa_instance_id);
  s->mfa_instance_id = nullptr;
  s->mfa = (SessionMfa){};
  clear_captcha(s);
}

static bool online(const Session *s) {
  return !s->offline && s->api != nullptr && s->token != nullptr;
}

static bool gateway_ready(const Session *s) {
  return s->gateway != nullptr && gateway_status(s->gateway) == GATEWAY_READY;
}

/* ========================================================================== */
/* history and the member list.                                               */
/* ========================================================================== */

// members are not sent with fetched messages, so the ones needed to show
// nicknames and role colors are asked from the gateway.
static void request_unknown_members(Session *s, const Channel *c) {
  const Guild *g = store_guild(s->store, c->guild_id);

  if (g == nullptr || !gateway_ready(s)) {
    return;
  }

  Snowflake ids[100];
  size_t count = 0;

  for (size_t i = 0; i < c->nmessages && count < 100; i++) {
    const Message *m = c->messages[i];
    bool known = m->webhook_id != 0 || m->author_id == 0 ||
                 store_member(g, m->author_id) != nullptr;

    for (size_t j = 0; j < count && !known; j++) {
      known = ids[j] == m->author_id;
    }

    if (!known) {
      ids[count++] = m->author_id;
    }
  }

  gateway_request_members(s->gateway, g->id, ids, count);
}

static void on_history(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Channel *c = store_channel(s->store, call->channel_id);

  if (c != nullptr) {
    if (res->ok && (json_type(res->json) == CIMA_JSON_ARRAY)) {
      store_put_history(s->store, c, res->json, call->kind, HISTORY_PAGE);
      request_unknown_members(s, c);
    }
    else {
      c->loading = false;
      store_touch_channel(s->store, c);
    }
  }

  call_end(call);
  changed(s);
}

static void fetch_history(Session *s, Channel *c, HistoryKind kind) {
  if (!online(s) || c->loading) {
    return;
  }

  Snowflake before = 0;

  if (kind == HISTORY_BEFORE) {
    if (c->nmessages == 0) {
      return;
    }

    before = c->messages[0]->id;
  }

  Call *call = call_new(s, c->id, 0);
  call->kind = kind;

  c->loading = true;
  store_touch_channel(s->store, c);
  api_get_messages(s->api, c->id, HISTORY_PAGE, before, 0, 0, on_history, call);
}

static void subscribe(Session *s) {
  if (!gateway_ready(s) || s->list_guild_id == 0) {
    return;
  }

  // the first rows are always kept in sync, then the blocks of a hundred
  // around what is shown.
  GatewayRange ranges[3] = {{0, LIST_BLOCK - 1}};
  size_t count = 1;
  uint32_t first = s->list_first / LIST_BLOCK;
  uint32_t last = s->list_last / LIST_BLOCK;

  for (uint32_t block = first; block <= last && count < 3; block++) {
    if (block != 0) {
      ranges[count++] = (GatewayRange){
        block * LIST_BLOCK,
        block * LIST_BLOCK + LIST_BLOCK - 1,
      };
    }
  }

  gateway_subscribe(s->gateway, s->list_guild_id, s->list_channel_id, ranges,
                    count);
}

// what showing the selected channel takes: its history once, and its
// guild's member list.
static void activate(Session *s) {
  Channel *c = store_channel(s->store, s->selected_channel_id);

  if (c == nullptr) {
    return;
  }

  if (!c->history_loaded && channel_has_text(c)) {
    fetch_history(s, c, HISTORY_LATEST);
  }

  if (c->guild_id != 0) {
    bool same = s->list_guild_id == c->guild_id && s->list_channel_id == c->id;

    if (!same) {
      s->list_guild_id = c->guild_id;
      s->list_channel_id = c->id;
      s->list_first = 0;
      s->list_last = 0;
    }

    subscribe(s);
  }
}

void session_select_channel(Session *s, Snowflake channel_id) {
  if (s->selected_channel_id == channel_id) {
    return;
  }

  Channel *previous = store_channel(s->store, s->selected_channel_id);

  if (previous != nullptr && !s->offline) {
    store_trim_messages(s->store, previous, KEEP_UNSELECTED);
  }

  s->selected_channel_id = channel_id;
  activate(s);
  changed(s);
}

void session_load_older(Session *s, Snowflake channel_id) {
  Channel *c = store_channel(s->store, channel_id);

  if (c != nullptr && c->has_older && c->history_loaded) {
    fetch_history(s, c, HISTORY_BEFORE);
    changed(s);
  }
}

void session_member_list_range(Session *s, uint32_t first_row,
                               uint32_t last_row) {
  if (last_row < first_row) {
    last_row = first_row;
  }

  bool same = s->list_first / LIST_BLOCK == first_row / LIST_BLOCK &&
              s->list_last / LIST_BLOCK == last_row / LIST_BLOCK;

  s->list_first = first_row;
  s->list_last = last_row;

  if (!same) {
    subscribe(s);
  }
}

/* ========================================================================== */
/* the saved accounts.                                                        */
/* ========================================================================== */

static void end_session(Session *s, const char *error);
static void snapshot_forget(Snowflake id);

static bool keeps(const Roster *r) {
  return !r->options.no_keychain && getenv("QUARREL_TOKEN") == nullptr;
}

static bool persists(const Session *s) {
  return keeps(s->roster) && !s->offline;
}

static char *token_key(Snowflake id, char out[static 32]) {
  char digits[21];

  snprintf(out, 32, "token:%s", snowflake_format(id, digits));
  return out;
}

// a token starts with its user's id in base64, up to the first dot.
static Snowflake token_account(const char *token) {
  size_t len = strcspn(token, ".");
  char *head = str_dup_n(token, len);
  size_t ndigits = 0;
  uint8_t *digits = base64_decode(head, &ndigits);
  Snowflake id =
    digits != nullptr ? snowflake_parse_n((const char *)digits, ndigits) : 0;

  free(digits);
  free(head);
  return id;
}

static void account_clear(SessionAccount *a) {
  free(a->username);
  free(a->global_name);
  free(a->discriminator);
  free(a->avatar);
}

static SessionAccount *account_find(Roster *r, Snowflake id) {
  for (size_t i = 0; id != 0 && i < r->naccounts; i++) {
    if (r->accounts[i].id == id) {
      return &r->accounts[i];
    }
  }

  return nullptr;
}

static SessionAccount *account_add(Roster *r, Snowflake id) {
  SessionAccount *found = account_find(r, id);

  if (found != nullptr) {
    return found;
  }

  size_t size = (r->naccounts + 1) * sizeof *r->accounts;
  SessionAccount *grown = realloc(r->accounts, size);

  if (grown == nullptr) {
    cilo_oom(size);
  }

  r->accounts = grown;
  r->accounts[r->naccounts] = (SessionAccount){.id = id};
  return &r->accounts[r->naccounts++];
}

static SessionAccount *account_of(Roster *r, const Session *s) {
  for (size_t i = 0; i < r->naccounts; i++) {
    if (r->accounts[i].session == s) {
      return &r->accounts[i];
    }
  }

  return nullptr;
}

// a session nothing shows anymore is freed later, once what called for it
// is done with it and its last requests left.
static void bury(Roster *r, Session *s) {
  size_t size = (r->ndead + 1) * sizeof *r->dead;
  Session **grown = realloc(r->dead, size);

  if (grown == nullptr) {
    cilo_oom(size);
  }

  r->dead = grown;
  r->dead[r->ndead++] = s;
}

static void save_last(Snowflake id) {
  char digits[21];

  platform_pref_set(LAST_ACCOUNT_KEY,
                    id != 0 ? snowflake_format(id, digits) : nullptr);
}

static void save_accounts(const Roster *r) {
  cima_json_writer w = {};

  json_begin_array(&w);

  for (size_t i = 0; i < r->naccounts; i++) {
    const SessionAccount *a = &r->accounts[i];

    json_begin_object(&w);
    json_key(&w, "id"), snowflake_write(&w, a->id);
    json_field(&w, "username", a->username);
    json_field(&w, "global_name", a->global_name);
    json_field(&w, "discriminator", a->discriminator);
    json_field(&w, "avatar", a->avatar);
    json_end_object(&w);
  }

  json_end_array(&w);
  platform_pref_set(ACCOUNTS_KEY, cima_json_writer_text(&w));
  cima_json_writer_dispose(&w);
}

static void load_accounts(Roster *r) {
  char *text = platform_pref_get(ACCOUNTS_KEY);

  if (text == nullptr) {
    return;
  }

  cilo_arena arena = {};
  const cima_json *saved = json_parse(&arena, text, strlen(text), nullptr);

  CIMA_EACH(item, json_items(saved)) {
    Snowflake id = snowflake_of(json_get(item, "id"));

    if (id == 0) {
      continue;
    }

    SessionAccount *a = account_add(r, id);

    str_set(&a->username, json_get_or(item, "username", nullptr));
    str_set(&a->global_name, json_get_or(item, "global_name", nullptr));
    str_set(&a->discriminator, json_get_or(item, "discriminator", nullptr));
    str_set(&a->avatar, json_get_or(item, "avatar", nullptr));
  }

  cilo_arena_drop(&arena);
  free(text);
}

// keeps the account for the next launch: its token, what the switcher shows
// of it, and that it was the last one used when it is the one shown.
static void remember(Session *s) {
  Roster *r = s->roster;

  if (!persists(s) || s->token == nullptr || s->account_id == 0) {
    return;
  }

  char key[32];

  if (!s->token_saved) {
    s->token_saved =
      platform_secret_set(token_key(s->account_id, key), s->token);
    platform_secret_delete(LEGACY_TOKEN_KEY);
  }

  SessionAccount *a = account_add(r, s->account_id);
  const User *self = s->store->self;

  // a new login takes over from the session the account had.
  if (a->session != s) {
    if (a->session != nullptr) {
      end_session(a->session, nullptr);
      bury(r, a->session);
    }

    a->session = s;
  }

  if (self != nullptr && self->id == a->id) {
    str_set(&a->username, self->username);
    str_set(&a->global_name, self->global_name);
    str_set(&a->discriminator, self->discriminator);
    str_set(&a->avatar, self->avatar);
  }

  save_accounts(r);

  if (s == r->current) {
    save_last(s->account_id);
  }
}

// the session the account had is left to who asked.
static void forget(Roster *r, Snowflake id) {
  SessionAccount *a = account_find(r, id);

  if (!keeps(r) || id == 0) {
    return;
  }

  char key[32];

  platform_secret_delete(token_key(id, key));
  snapshot_forget(id);

  if (a != nullptr) {
    size_t after = r->naccounts - (size_t)(a - r->accounts) - 1;

    account_clear(a);
    memmove(a, a + 1, after * sizeof *a);
    r->naccounts--;
    save_accounts(r);
  }

  char *last = platform_pref_get(LAST_ACCOUNT_KEY);

  if (snowflake_parse(last) == id) {
    save_last(r->naccounts > 0 ? r->accounts[0].id : 0);
  }

  free(last);
}

/* ========================================================================== */
/* the gateway.                                                               */
/* ========================================================================== */

static void on_typing_timer(void *ctx);
static void watch_status(Session *s);

static void schedule_typing(Session *s) {
  if (s->typing_timer != nullptr) {
    return;
  }

  double next = store_typing_expire(s->store);

  if (next > 0) {
    double delay = next - store_now(s->store) + 0.05;
    s->typing_timer = platform_timer(delay, on_typing_timer, s);
  }
}

static void on_typing_timer(void *ctx) {
  Session *s = ctx;

  s->typing_timer = nullptr;
  schedule_typing(s);
  changed(s);
}

static void on_affinities(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  if (res->ok) {
    store_put_affinities(s->store, json_get(res->json, "user_affinities"));
  }

  call_end(call);
  changed(s);
}

/* ========================================================================== */
/* voice: the gateway is told which channel, and answers with the session's   */
/* own voice state, then with the server to connect to.                       */
/* ========================================================================== */

constexpr double VOICE_JOIN_TIMEOUT = 15;
constexpr double VOICE_SETTLE = 3;

static void voice_tell(Session *s, Snowflake channel_id) {
  if (gateway_ready(s)) {
    gateway_voice_state(s->gateway, s->voice_guild_id, channel_id,
                        s->voice_mute || s->voice_deaf, s->voice_deaf);
  }
}

// lets go of the call, the gateway not being told.
static void voice_reset(Session *s) {
  platform_timer_cancel(s->voice_timer);
  s->voice_timer = nullptr;

  if (s->voice != nullptr) {
    voice_disconnect(s->voice);
  }

  wipe(&s->voice_token);
  str_set(&s->voice_endpoint, nullptr);
  str_set(&s->voice_error, nullptr);
  s->voice_guild_id = s->voice_channel_id = 0;
  s->voice_stated = s->voice_live = s->voice_rejoin = false;
}

static void on_voice_changed(void *ctx) {
  changed(ctx);
}

// the gateway may still count the account in the channel of a call that
// failed, unless the account was only moved to another, which shows a
// moment later.
static void on_voice_settled(void *ctx) {
  Session *s = ctx;

  s->voice_timer = nullptr;

  if (s->voice != nullptr && voice_status(s->voice) == VOICE_FAILED) {
    voice_tell(s, 0);
  }
}

static void on_voice_ended(void *ctx) {
  Session *s = ctx;

  platform_timer_cancel(s->voice_timer);
  s->voice_timer = platform_timer(VOICE_SETTLE, on_voice_settled, s);
}

static void on_voice_timeout(void *ctx) {
  Session *s = ctx;

  s->voice_timer = nullptr;
  voice_tell(s, 0);
  str_set(&s->voice_error, "Discord did not open the call.");
  changed(s);
}

static void voice_try(Session *s) {
  const char *session_id =
    s->gateway != nullptr ? gateway_session_id(s->gateway) : nullptr;

  if (s->voice_live || !s->voice_stated || s->voice_channel_id == 0 ||
      s->voice_token == nullptr || s->voice_endpoint == nullptr ||
      session_id == nullptr) {
    return;
  }

  if (s->voice == nullptr) {
    VoiceEvents events = {s, on_voice_changed, on_voice_ended};

    s->voice = voice_new(s->store->self_id, &events);

    if (s->voice == nullptr) {
      return;
    }
  }

  platform_timer_cancel(s->voice_timer);
  s->voice_timer = nullptr;
  s->voice_live = true;
  voice_set_mute(s->voice, s->voice_mute || s->voice_deaf);
  voice_set_deaf(s->voice, s->voice_deaf);
  // a private channel's call is identified with the channel's id.
  voice_connect(s->voice,
                s->voice_guild_id != 0 ? s->voice_guild_id
                                       : s->voice_channel_id,
                s->voice_channel_id, session_id, s->voice_token,
                s->voice_endpoint);
}

// the account's own voice state, as this session of the gateway has it:
// the account may be in a call from another device too.
static void on_voice_state(Session *s, const cima_json *d) {
  const char *session_id =
    s->gateway != nullptr ? gateway_session_id(s->gateway) : nullptr;
  const char *theirs = json_get_or(d, "session_id", nullptr);
  Snowflake channel_id = snowflake_of(json_get(d, "channel_id"));

  if (s->voice_channel_id == 0 || s->voice_error != nullptr ||
      snowflake_of(json_get(d, "user_id")) != s->store->self_id ||
      session_id == nullptr || theirs == nullptr ||
      strcmp(session_id, theirs) != 0) {
    return;
  }

  if (channel_id == 0) {
    // someone took the account out of the call. one that failed stays
    // shown with its reason.
    if (s->voice == nullptr || voice_status(s->voice) != VOICE_FAILED) {
      voice_reset(s);
    }

    return;
  }

  // someone moved the account to another channel.
  if (channel_id != s->voice_channel_id) {
    s->voice_channel_id = channel_id;
    s->voice_live = false;
  }

  s->voice_stated = true;
  voice_try(s);
}

static void on_voice_server(Session *s, const cima_json *d) {
  const char *token = json_get_or(d, "token", nullptr);
  const char *endpoint = json_get_or(d, "endpoint", nullptr);
  Snowflake channel_id = snowflake_of(json_get(d, "channel_id"));

  if (s->voice_channel_id == 0 ||
      snowflake_of(json_get(d, "guild_id")) != s->voice_guild_id ||
      (s->voice_guild_id == 0 && channel_id != s->voice_channel_id)) {
    return;
  }

  // no endpoint: the server went away and discord is finding another,
  // which a next update names.
  if (token == nullptr || endpoint == nullptr) {
    str_set(&s->voice_endpoint, nullptr);
    s->voice_live = false;
    return;
  }

  bool same = s->voice_token != nullptr && s->voice_endpoint != nullptr &&
              strcmp(s->voice_token, token) == 0 &&
              strcmp(s->voice_endpoint, endpoint) == 0;

  if (same && s->voice_live && s->voice != nullptr &&
      voice_status(s->voice) != VOICE_FAILED) {
    return;
  }

  wipe(&s->voice_token);
  str_set(&s->voice_token, token);
  str_set(&s->voice_endpoint, endpoint);
  s->voice_live = false;
  voice_try(s);
}

void session_voice_join(Session *s, Snowflake channel_id) {
  const Channel *c = store_channel(s->store, channel_id);
  bool failed = s->voice_error != nullptr ||
                (s->voice != nullptr && voice_status(s->voice) == VOICE_FAILED);

  if (c == nullptr || s->offline || !gateway_ready(s) ||
      (s->voice_channel_id == channel_id && !failed)) {
    return;
  }

  voice_reset(s);
  s->voice_guild_id = c->guild_id;
  s->voice_channel_id = channel_id;
  voice_tell(s, channel_id);
  s->voice_timer = platform_timer(VOICE_JOIN_TIMEOUT, on_voice_timeout, s);
  changed(s);
}

void session_voice_leave(Session *s) {
  if (s->voice_channel_id == 0) {
    return;
  }

  voice_tell(s, 0);
  voice_reset(s);
  changed(s);
}

static void voice_apply(Session *s) {
  if (s->voice != nullptr) {
    voice_set_mute(s->voice, s->voice_mute || s->voice_deaf);
    voice_set_deaf(s->voice, s->voice_deaf);
  }

  if (s->voice_channel_id != 0 && s->voice_error == nullptr) {
    voice_tell(s, s->voice_channel_id);
  }

  changed(s);
}

void session_voice_mute(Session *s, bool mute) {
  s->voice_mute = mute;

  // speaking again means hearing again.
  if (!mute) {
    s->voice_deaf = false;
  }

  voice_apply(s);
}

void session_voice_deafen(Session *s, bool deaf) {
  s->voice_deaf = deaf;
  voice_apply(s);
}

/* ========================================================================== */
/* the last READY of an account, kept in the user's caches as discord wrote   */
/* it, and played back into the store at the next launch so that the ui has   */
/* something to show while the gateway connects.                              */
/* ========================================================================== */

static bool snapshot_path(Snowflake id, char out[static 1024]) {
  char name[48];

  snprintf(name, sizeof name, "ready-%llu.json", (unsigned long long)id);
  return id != 0 && platform_cache_path(name, out, 1024);
}

static void snapshot_forget(Snowflake id) {
  char path[1024];

  if (snapshot_path(id, path)) {
    remove(path);
  }
}

// writes the READY held, with what came with it when given, as the array
// events_feed takes. the file is whole or not there: it is renamed into
// place.
static void snapshot_save(Session *s, const char *with, size_t with_len) {
  char path[1024], writing[1040];

  if (s->ready_text == nullptr || !persists(s) ||
      !snapshot_path(s->account_id, path)) {
    return;
  }

  snprintf(writing, sizeof writing, "%s.new", path);

  FILE *file = fopen(writing, "wb");

  if (file == nullptr) {
    return;
  }

  // it holds what the account can see: for its owner alone.
  chmod(writing, 0600);

  bool ok = fputc('[', file) != EOF && fputs(s->ready_text, file) != EOF;

  if (with != nullptr) {
    ok = ok && fputc(',', file) != EOF &&
         fwrite(with, 1, with_len, file) == with_len;
  }

  ok = ok && fputc(']', file) != EOF;
  ok = fclose(file) == 0 && ok;

  if (!ok || rename(writing, path) != 0) {
    remove(writing);
  }
}

// whether the store now holds what the account last had.
static bool snapshot_load(Session *s) {
  char path[1024];

  if (!persists(s) || !snapshot_path(s->account_id, path) ||
      !events_feed_file(s->store, path)) {
    return false;
  }

  // a token discord handed out then is not one to take up now.
  str_set(&s->store->refreshed_token, nullptr);

  if (s->store->self_id != s->account_id) {
    store_reset(s->store);
    remove(path);
    return false;
  }

  return true;
}

static void on_snapshot(void *ctx, const char *type, const char *text,
                        size_t len) {
  Session *s = ctx;

  if (strcmp(type, "READY") == 0) {
    free(s->ready_text);
    s->ready_text = strndup(text, len);
    return;
  }

  snapshot_save(s, text, len);
  free(s->ready_text);
  s->ready_text = nullptr;
}

static void on_dispatch(void *ctx, const char *type, const cima_json *data) {
  Session *s = ctx;

  events_dispatch(s->store, type, data);
  watch_status(s);

  if (strcmp(type, "VOICE_STATE_UPDATE") == 0) {
    on_voice_state(s, data);
  }
  else if (strcmp(type, "VOICE_SERVER_UPDATE") == 0) {
    on_voice_server(s, data);
  }

  if (strcmp(type, "READY") == 0) {
    // the client asks who the user is close to once it is ready. a new
    // session emptied the store, so every READY asks again.
    ApiRequest req = {.method = "GET", .path = "/users/@me/affinities/users"};

    api_request(s->api, &req, on_affinities, call_new(s, 0, 0));

    s->was_ready = true;

    // discord names the installation when the identify carried none.
    api_set_installation_id(
      json_get_or(json_get(data, "apex_experiments"), "installation", nullptr));

    // discord may replace the token, the old one stops working later.
    if (s->store->refreshed_token != nullptr) {
      str_set(&s->token, s->store->refreshed_token);
      api_set_token(s->api, s->token);
      gateway_set_token(s->gateway, s->token);
      s->token_saved = false;
    }

    s->account_id = s->store->self_id;
    s->cached = false;
    remember(s);

    // a READY that carries a token stays off the disk.
    if (s->store->refreshed_token != nullptr) {
      free(s->ready_text);
      s->ready_text = nullptr;
    }

    // what comes with it may not come: the READY alone is kept meanwhile.
    snapshot_save(s, nullptr, 0);

    // a new session of the gateway is in no call: it joins again.
    s->voice_rejoin = s->voice_channel_id != 0 && s->voice_error == nullptr;
  }
  else if (strcmp(type, "TYPING_START") == 0) {
    schedule_typing(s);
  }

  changed(s);
}

static void on_status(void *ctx, GatewayStatus status) {
  Session *s = ctx;

  switch (status) {
    case GATEWAY_READY:
      set_state(s, SESSION_READY);

      // a new session knows nothing of what was shown, a resumed one does.
      activate(s);

      if (s->voice_rejoin) {
        s->voice_rejoin = s->voice_stated = s->voice_live = false;
        wipe(&s->voice_token);
        str_set(&s->voice_endpoint, nullptr);
        voice_tell(s, s->voice_channel_id);
      }

      break;
    case GATEWAY_CONNECTING:
    case GATEWAY_HANDSHAKING:
    case GATEWAY_WAITING:
      set_state(s, s->was_ready || s->cached ? SESSION_RECONNECTING
                                             : SESSION_CONNECTING);
      break;
    case GATEWAY_STOPPED:
      break;
  }
}

static void clear_mentions(Session *s) {
  for (size_t i = 0; i < s->nmentions; i++) {
    free(s->mentions[i].content);
  }

  free(s->mentions);
  s->mentions = nullptr;
  s->nmentions = 0;
  s->mentions_loading = false;
  s->mentions_loaded = false;
}

static void clear_frecency(Session *s) {
  for (size_t i = 0; i < s->nfrecent_reactions; i++) {
    free(s->frecent_reactions[i]);
  }

  free(s->frecent_reactions);
  s->frecent_reactions = nullptr;
  s->nfrecent_reactions = 0;
  s->frecency_loading = false;
  s->frecency_loaded = false;
}

typedef struct SessionApplication {
  Snowflake id;
  char *icon;
} SessionApplication;

static void clear_applications(Session *s) {
  for (size_t i = 0; i < s->napplications; i++) {
    free(s->applications[i].icon);
  }

  free(s->applications);
  s->applications = nullptr;
  s->napplications = 0;
}

static void clear_profile_badges(Session *s) {
  for (size_t i = 0; i < s->nbadges; i++) {
    free(s->badges[i].icon);
    free(s->badges[i].description);
  }

  free(s->badges);
  s->badges = nullptr;
  s->nbadges = 0;
}

static void clear_profile(Session *s) {
  clear_profile_badges(s);
  s->profile_loading = false;
  s->profile_loaded = false;

  platform_timer_cancel(s->status_timer);
  s->status_timer = nullptr;
  s->status_timed = 0;
}

static void clear_invite(Session *s) {
  free(s->invite_code);
  free(s->invite_error);
  s->invite_code = nullptr;
  s->invite_error = nullptr;
  s->invite_guild_id = 0;
  s->invite_loading = false;

  free(s->guild_error);
  s->guild_error = nullptr;
  s->guild_creating = false;
  s->guild_created = 0;
}

static void qr_stop(Session *s);

static void end_session(Session *s, const char *error) {
  qr_stop(s);
  clear_mentions(s);
  clear_frecency(s);
  clear_applications(s);
  clear_profile(s);
  clear_invite(s);
  gateway_free(s->gateway);
  s->gateway = nullptr;

  platform_timer_cancel(s->typing_timer);
  s->typing_timer = nullptr;
  voice_reset(s);

  if (s->api != nullptr) {
    api_set_token(s->api, nullptr);
  }

  wipe(&s->token);
  clear_login(s);
  store_reset(s->store);

  s->epoch++;
  s->account_id = 0;
  s->token_saved = false;
  s->was_ready = false;
  s->cached = false;
  free(s->ready_text);
  s->ready_text = nullptr;
  s->offline = false;
  s->selected_channel_id = 0;
  s->list_guild_id = 0;
  s->list_channel_id = 0;

  set_error(s, error);
  set_state(s, SESSION_LOGGED_OUT);
}

static void on_fatal(void *ctx, GatewayFatal why, int code,
                     const char *reason) {
  Session *s = ctx;
  Roster *r = s->roster;
  SessionAccount *a = account_find(r, s->account_id);
  bool shown = s == r->current;
  char text[160];

  // the session is no longer the account's: a switch to it starts another.
  if (why == GATEWAY_FATAL_AUTH) {
    forget(r, s->account_id);
    snprintf(text, sizeof text,
             "Discord no longer accepts this login. Log in again.");
  }
  else {
    if (a != nullptr) {
      a->session = nullptr;
    }

    snprintf(text, sizeof text, "Discord refused the connection (%d%s%s).",
             code, reason[0] != '\0' ? " " : "", reason);
  }

  end_session(s, shown ? text : nullptr);

  // an account in the background leaves quietly.
  if (!shown) {
    bury(r, s);
    changed(r->current);
  }
}

static void connect(Session *s) {
  GatewayEvents events = {
    .ctx = s,
    .dispatch = on_dispatch,
    .snapshot = on_snapshot,
    .status = on_status,
    .fatal = on_fatal,
  };

  if (s->api == nullptr) {
    s->api = api_new();
  }

  api_set_token(s->api, s->token);

  gateway_free(s->gateway);
  s->gateway = gateway_new(s->token, &events);
  s->was_ready = false;
  s->cached = snapshot_load(s);

  set_state(s, s->cached ? SESSION_RECONNECTING : SESSION_CONNECTING);
  gateway_start(s->gateway);
}

SessionLink session_link(const Session *s) {
  if (s->offline || s->gateway == nullptr) {
    return SESSION_LINK_UP;
  }

  switch (gateway_status(s->gateway)) {
    case GATEWAY_READY:
      return SESSION_LINK_UP;
    case GATEWAY_CONNECTING:
    case GATEWAY_HANDSHAKING:
      return SESSION_LINK_CONNECTING;
    default:
      return SESSION_LINK_DOWN;
  }
}

/* ========================================================================== */
/* logging in and out.                                                        */
/* ========================================================================== */

// connects with a token the keychain may not hold yet.
static void enter(Session *s, const char *token, bool saved) {
  str_set(&s->token, token);
  s->account_id = token_account(token);
  s->token_saved = saved;

  qr_stop(s);
  clear_login(s);
  set_error(s, nullptr);
  remember(s);
  connect(s);
}

static void finish_login(Session *s, const char *token) {
  enter(s, token, false);
}

static void on_login(void *ctx, const ApiLoginResult *result) {
  Session *s = ctx;
  bool second_factor = s->mfa_ticket != nullptr;

  // the login was left while its answer was on its way.
  if (s->login == nullptr && !second_factor) {
    return;
  }

  switch (result->kind) {
    case API_LOGIN_TOKEN:
      finish_login(s, result->token);
      return;
    case API_LOGIN_MFA:
      wipe(&s->password);
      str_set(&s->mfa_ticket, result->mfa.ticket);
      str_set(&s->mfa_instance_id, result->mfa.login_instance_id);
      s->mfa = (SessionMfa){
        .totp = result->mfa.totp,
        .sms = result->mfa.sms,
        .backup = result->mfa.backup,
      };
      set_error(s, nullptr);
      set_state(s, SESSION_MFA);
      return;
    case API_LOGIN_CAPTCHA:
      clear_captcha(s);
      s->captcha = (SessionCaptcha){
        .service = str_dup(result->captcha.service),
        .sitekey = str_dup(result->captcha.sitekey),
        .rqdata = str_dup(result->captcha.rqdata),
        .session_id = str_dup(result->captcha.session_id),
        .rqtoken = str_dup(result->captcha.rqtoken),
        .serve_invisible = result->captcha.serve_invisible,
      };
      set_error(s, nullptr);
      set_state(s, SESSION_CAPTCHA);
      return;
    case API_LOGIN_ERROR:
      set_error(s, result->error.message);

      // a wrong code is typed again, anything else starts over.
      if (second_factor) {
        set_state(s, SESSION_MFA);
        return;
      }

      clear_login(s);
      set_state(s, SESSION_LOGGED_OUT);
      return;
  }
}

static void on_fingerprint(void *ctx, const ApiResponse *) {
  Session *s = ctx;

  // a login without a fingerprint still works, so a failure here is not one.
  if (s->state == SESSION_LOGGING_IN && s->login != nullptr) {
    api_login(s->api, s->login, s->password, nullptr, on_login, s);
  }
}

void session_login(Session *s, const char *login, const char *password) {
  bool busy = s->state != SESSION_LOGGED_OUT;

  if (busy || login == nullptr || password == nullptr) {
    return;
  }

  if (s->api == nullptr) {
    s->api = api_new();
  }

  clear_login(s);
  s->login = str_dup(login);
  s->password = str_dup(password);

  set_error(s, nullptr);
  set_state(s, SESSION_LOGGING_IN);
  api_fetch_fingerprint(s->api, on_fingerprint, s);
}

void session_login_token(Session *s, const char *token) {
  if (s->state != SESSION_LOGGED_OUT || token == nullptr || token[0] == '\0') {
    return;
  }

  finish_login(s, token);
}

void session_submit_mfa(Session *s, SessionMfaMethod method, const char *code) {
  if (s->state != SESSION_MFA || s->mfa_ticket == nullptr) {
    return;
  }

  ApiMfa mfa = {
    .ticket = s->mfa_ticket,
    .login_instance_id = s->mfa_instance_id,
  };
  ApiMfaKind kind = method == SESSION_MFA_SMS      ? API_MFA_SMS
                    : method == SESSION_MFA_BACKUP ? API_MFA_BACKUP
                                                   : API_MFA_TOTP;

  set_error(s, nullptr);
  set_state(s, SESSION_LOGGING_IN);
  api_mfa(s->api, kind, &mfa, code, on_login, s);
}

static void on_sms_sent(void *ctx, const ApiResponse *res) {
  Session *s = ctx;

  if (s->state != SESSION_MFA) {
    return;
  }

  if (res->ok) {
    s->mfa.sms_sent = true;
  }
  else {
    set_error(s, res->error.message);
  }

  changed(s);
}

void session_request_sms(Session *s) {
  if (s->state == SESSION_MFA && s->mfa_ticket != nullptr) {
    api_mfa_send_sms(s->api, s->mfa_ticket, on_sms_sent, s);
  }
}

void session_submit_captcha(Session *s, const char *solution) {
  if (s->state != SESSION_CAPTCHA || s->login == nullptr ||
      solution == nullptr) {
    return;
  }

  // the request copies what it needs before the challenge is forgotten.
  ApiCaptchaSolution solved = {
    .key = solution,
    .session_id = s->captcha.session_id,
    .rqtoken = s->captcha.rqtoken,
  };

  api_login(s->api, s->login, s->password, &solved, on_login, s);
  clear_captcha(s);
  set_error(s, nullptr);
  set_state(s, SESSION_LOGGING_IN);
}

void session_cancel_login(Session *s) {
  if (s->state != SESSION_MFA && s->state != SESSION_CAPTCHA) {
    return;
  }

  clear_login(s);
  set_error(s, nullptr);
  set_state(s, SESSION_LOGGED_OUT);
}

/* ========================================================================== */
/* logging in by qr code, see remote_auth.h.                                  */
/* ========================================================================== */

// the web client's Backoff(1500, 30000).
constexpr double QR_BACKOFF_FIRST = 1.5;
constexpr double QR_BACKOFF_MAX = 30;

static void qr_connect(Session *s);

static void qr_set(Session *s, SessionQrStep step) {
  s->qr.step = step;
  changed(s);
}

// forgets the attempt, and the answers still on their way for it.
static void qr_drop(Session *s) {
  remote_auth_free(s->remote_auth);
  s->remote_auth = nullptr;
  platform_timer_cancel(s->qr_timer);
  s->qr_timer = nullptr;
  s->qr_attempt++;

  free(s->qr.url);
  free(s->qr.username);
  free(s->qr.discriminator);
  free(s->qr.avatar);
  s->qr = (SessionQr){.step = s->qr.step};
}

static void qr_stop(Session *s) {
  if (s->qr.step == SESSION_QR_OFF) {
    return;
  }

  qr_drop(s);
  qr_set(s, SESSION_QR_OFF);
}

static void qr_retry(void *ctx) {
  Session *s = ctx;

  s->qr_timer = nullptr;
  qr_connect(s);
}

// each failure waits longer, by up to twice the last wait.
static void qr_failed(Session *s) {
  uint32_t random;

  qr_drop(s);
  platform_random(&random, sizeof random);

  double grown =
    s->qr_backoff + s->qr_backoff * 2 * ((double)random / 4294967296.0);

  s->qr_backoff = grown < QR_BACKOFF_MAX ? grown : QR_BACKOFF_MAX;
  s->qr_timer = platform_timer(s->qr_backoff, qr_retry, s);
  qr_set(s, SESSION_QR_LOADING);
}

static void qr_on_ready(void *ctx, const char *fingerprint) {
  Session *s = ctx;
  char url[128];

  snprintf(url, sizeof url, "https://discord.com/ra/%s", fingerprint);
  str_set(&s->qr.url, url);
  s->qr_backoff = QR_BACKOFF_FIRST;
  qr_set(s, SESSION_QR_READY);
}

static void qr_on_scanned(void *ctx, const RemoteAuthUser *user) {
  Session *s = ctx;

  s->qr.user_id = user->id;
  str_set(&s->qr.username, user->username);
  str_set(&s->qr.discriminator, user->discriminator);
  str_set(&s->qr.avatar, user->avatar);
  qr_set(s, SESSION_QR_SCANNED);
}

static void qr_on_cancelled(void *ctx) {
  qr_connect(ctx);
}

static void qr_on_failed(void *ctx) {
  qr_failed(ctx);
}

static void qr_on_token(void *ctx, const ApiResponse *res) {
  Call *call = ctx;
  Session *s = call->s;
  bool current = call->id == s->qr_attempt;

  if (call_stale(call)) {
    return;
  }

  call_end(call);

  if (!current) {
    return;
  }

  const char *encrypted =
    res->ok ? json_get_or(res->json, "encrypted_token", nullptr) : nullptr;
  char *token = remote_auth_decrypt(s->remote_auth, encrypted);

  if (token == nullptr || token[0] == '\0') {
    wipe(&token);
    qr_failed(s);
    return;
  }

  enter(s, token, false);
  wipe(&token);
}

static void qr_on_ticket(void *ctx, const char *ticket) {
  Session *s = ctx;

  if (s->api == nullptr) {
    s->api = api_new();
  }

  qr_set(s, SESSION_QR_LOGGING_IN);
  api_remote_auth_login(s->api, ticket, qr_on_token,
                        call_new(s, 0, s->qr_attempt));
}

static void qr_connect(Session *s) {
  RemoteAuthEvents events = {
    .ctx = s,
    .ready = qr_on_ready,
    .scanned = qr_on_scanned,
    .ticket = qr_on_ticket,
    .cancelled = qr_on_cancelled,
    .failed = qr_on_failed,
  };

  qr_drop(s);
  s->remote_auth = remote_auth_new(&events);
  qr_set(s, SESSION_QR_LOADING);
}

void session_qr(Session *s, bool shown) {
  if (!shown || s->offline) {
    qr_stop(s);
  }
  else if (s->qr.step == SESSION_QR_OFF) {
    s->qr_backoff = QR_BACKOFF_FIRST;
    qr_connect(s);
  }
}

void session_qr_restart(Session *s) {
  if (s->qr.step != SESSION_QR_OFF) {
    qr_connect(s);
  }
}

static Session *session_new(Roster *r) {
  Session *s = calloc(1, sizeof *s);

  if (s == nullptr) {
    cilo_oom(sizeof *s);
  }

  s->roster = r;
  s->options = r->options;
  s->store = store_new();
  return s;
}

static void session_free(Session *s) {
  if (s == nullptr) {
    return;
  }

  s->options.on_change = nullptr;

  qr_stop(s);
  voice_reset(s);
  voice_free(s->voice);
  gateway_free(s->gateway);
  api_free(s->api);
  platform_timer_cancel(s->typing_timer);

  for (Call *call = s->calls; call != nullptr;) {
    Call *next = call->next;
    free(call);
    call = next;
  }

  clear_login(s);
  clear_mentions(s);
  clear_frecency(s);
  clear_applications(s);
  clear_profile(s);
  clear_invite(s);
  wipe(&s->token);
  store_free(s->store);
  free(s->error);
  free(s);
}

/* ========================================================================== */
/* the roster.                                                                */
/* ========================================================================== */

static void reap(Roster *r) {
  for (size_t i = 0; i < r->ndead; i++) {
    session_free(r->dead[i]);
  }

  r->ndead = 0;
}

// connects the account with the token the keychain holds for it. false when
// it holds none, the account is then forgotten, or would not give it: the
// account is kept for a later try then, its token and snapshot with it.
static bool open_account(Roster *r, Snowflake id) {
  char key[32];
  bool missing = false;
  char *token = platform_secret_get(token_key(id, key), &missing);

  if (token == nullptr || token[0] == '\0') {
    if (missing || token != nullptr) {
      forget(r, id);
    }

    wipe(&token);
    return false;
  }

  Session *s = session_new(r);

  account_add(r, id)->session = s;
  enter(s, token, true);
  wipe(&token);
  return true;
}

// shows `s`. the session left stays connected when it is an account's.
static void show(Roster *r, Session *s) {
  Session *left = r->current;

  if (left == s) {
    return;
  }

  r->current = s;

  if (left != nullptr && account_of(r, left) == nullptr) {
    end_session(left, nullptr);
    bury(r, left);
  }

  if (keeps(r) && s->account_id != 0) {
    save_last(s->account_id);
  }

  changed(s);
}

void roster_switch(Roster *r, Snowflake account_id) {
  SessionAccount *a = account_find(r, account_id);

  reap(r);

  if (a == nullptr || !keeps(r)) {
    return;
  }

  // discord refused its connection earlier, so it is tried again.
  if (a->session == nullptr && !open_account(r, account_id)) {
    changed(r->current);
    return;
  }

  show(r, account_find(r, account_id)->session);
}

void roster_add(Roster *r) {
  reap(r);

  if (account_of(r, r->current) != nullptr) {
    show(r, session_new(r));
  }
}

void roster_logout(Roster *r) {
  Session *s = r->current;

  reap(r);

  if (s->state == SESSION_LOGGED_OUT) {
    return;
  }

  // the request takes the token as it leaves, before it is forgotten.
  if (online(s)) {
    api_logout(s->api, nullptr, nullptr);
  }

  forget(r, s->account_id);
  end_session(s, nullptr);

  for (size_t i = 0; i < r->naccounts; i++) {
    if (r->accounts[i].session != nullptr) {
      show(r, r->accounts[i].session);
      return;
    }
  }
}

bool session_load_fixture(Session *s, const char *path) {
  gateway_free(s->gateway);
  s->gateway = nullptr;

  if (!events_feed_file(s->store, path)) {
    set_error(s, "The fixture could not be read.");
    set_state(s, SESSION_LOGGED_OUT);
    return false;
  }

  // there is no history to fetch: what the fixture holds is all there is.
  CILO_MAP_EACH(entry, &s->store->channels) {
    entry->val->history_loaded = true;
    entry->val->has_older = false;
  }

  s->offline = true;
  set_error(s, nullptr);
  set_state(s, SESSION_READY);
  return true;
}

void roster_start(Roster *r) {
  const char *fixture = getenv("QUARREL_FIXTURE");
  const char *token = getenv("QUARREL_TOKEN");

  if (fixture != nullptr && fixture[0] != '\0') {
    session_load_fixture(r->current, fixture);
    return;
  }

  if (token != nullptr && token[0] != '\0') {
    str_set(&r->current->token, token);
    connect(r->current);
    return;
  }

  if (!keeps(r)) {
    return;
  }

  load_accounts(r);

  // an account whose token is gone is forgotten, and takes its place away.
  // one the keychain would not answer for stays, without a session.
  for (size_t i = 0; i < r->naccounts;) {
    Snowflake id = r->accounts[i].id;

    open_account(r, id);

    if (account_find(r, id) != nullptr) {
      i++;
    }
  }

  char *last = platform_pref_get(LAST_ACCOUNT_KEY);
  SessionAccount *a = account_find(r, snowflake_parse(last));

  free(last);

  for (size_t i = 0; (a == nullptr || a->session == nullptr) &&
                     i < r->naccounts;
       i++) {
    a = &r->accounts[i];
  }

  if (a != nullptr && a->session != nullptr) {
    show(r, a->session);
    return;
  }

  if (r->naccounts > 0) {
    set_error(r->current, "The keychain did not give Quarrel your saved "
                          "logins. Allow it, then pick an account again.");
  }

  char *legacy = platform_secret_get(LEGACY_TOKEN_KEY, nullptr);

  if (legacy != nullptr && legacy[0] != '\0') {
    enter(r->current, legacy, false);
  }

  wipe(&legacy);
}

// every account's connection hears of the network, and the ui of whether
// there is one.
static void on_network(void *ctx, bool online, bool returned) {
  Roster *r = ctx;
  Session *shown = r->current;

  if (shown->gateway != nullptr && account_of(r, shown) == nullptr) {
    gateway_network_changed(shown->gateway, online, returned);
  }

  for (size_t i = 0; i < r->naccounts; i++) {
    Session *s = r->accounts[i].session;

    if (s != nullptr && s->gateway != nullptr) {
      gateway_network_changed(s->gateway, online, returned);
    }
  }

  changed(r->current);
}

Roster *roster_new(const SessionOptions *options) {
  Roster *r = calloc(1, sizeof *r);

  if (r == nullptr) {
    cilo_oom(sizeof *r);
  }

  if (options != nullptr) {
    r->options = *options;
  }

  r->current = session_new(r);
  platform_network_watch(on_network, r);
  return r;
}

void roster_free(Roster *r) {
  if (r == nullptr) {
    return;
  }

  r->options.on_change = nullptr;
  platform_network_watch(nullptr, nullptr);

  if (account_of(r, r->current) == nullptr) {
    session_free(r->current);
  }

  for (size_t i = 0; i < r->naccounts; i++) {
    session_free(r->accounts[i].session);
    account_clear(&r->accounts[i]);
  }

  reap(r);
  free(r->dead);
  free(r->accounts);
  free(r);
}

/* ========================================================================== */
/* messages.                                                                  */
/* ========================================================================== */

static void iso_now(const Store *store, char out[static 40]) {
  double now = store_now(store);
  time_t seconds = (time_t)now;
  struct tm tm;

  gmtime_r(&seconds, &tm);
  strftime(out, 40, "%Y-%m-%dT%H:%M:%S.000000+00:00", &tm);
}

// offline, what discord would have answered is made up here so the ui has
// something to show.
static void echo_message(Session *s, const Channel *c, const Message *pending) {
  cima_json_writer w = {};
  char timestamp[40];

  iso_now(s->store, timestamp);

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, pending->nonce);
  json_key(&w, "nonce"), snowflake_write(&w, pending->nonce);
  json_key(&w, "channel_id"), snowflake_write(&w, c->id);
  json_field(&w, "type", pending->type);
  json_field(&w, "content", pending->content);
  json_field(&w, "timestamp", timestamp);
  json_key(&w, "author");
  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, s->store->self_id);
  json_end_object(&w);

  if (pending->reference_id != 0) {
    json_key(&w, "message_reference");
    json_begin_object(&w);
    json_key(&w, "message_id"), snowflake_write(&w, pending->reference_id);
    json_key(&w, "channel_id"), snowflake_write(&w, c->id);
    json_end_object(&w);

    const Message *target = store_message(c, pending->reference_id);

    if (target != nullptr) {
      json_key(&w, "referenced_message");
      json_begin_object(&w);
      json_key(&w, "id"), snowflake_write(&w, target->id);
      json_key(&w, "channel_id"), snowflake_write(&w, c->id);
      json_field(&w, "content", target->content);
      json_key(&w, "author");
      json_begin_object(&w);
      json_key(&w, "id"), snowflake_write(&w, target->author_id);
      json_end_object(&w);
      json_end_object(&w);
    }
  }

  json_end_object(&w);

  cilo_arena arena = {};
  store_create_message(
    s->store, json_parse(&arena, cima_json_writer_text(&w), w.len, nullptr));
  cilo_arena_drop(&arena);
  cima_json_writer_dispose(&w);
}

static void on_sent(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  if (res->ok) {
    // the gateway usually tells first, and telling twice changes nothing.
    store_create_message(s->store, res->json);
    store_remove_pending(s->store, call->channel_id, call->id);
  }
  else {
    store_fail_pending(s->store, call->channel_id, call->id,
                       res->error.message);
  }

  call_end(call);
  changed(s);
}

static void transmit(Session *s, Channel *c, const Message *pending,
                     bool reply_ping) {
  if (s->offline) {
    echo_message(s, c, pending);
    return;
  }

  if (!online(s)) {
    store_fail_pending(s->store, c->id, pending->nonce, "Not connected.");
    return;
  }

  ApiSendMessage message = {
    .content = pending->content,
    .nonce = pending->nonce,
    .reply_to = pending->reference_id,
    .reply_ping = reply_ping,
  };

  api_send_message(s->api, c->id, &message, on_sent,
                   call_new(s, c->id, pending->nonce));
}

Snowflake session_send(Session *s, Snowflake channel_id, const char *content,
                       Snowflake reply_to, bool reply_ping) {
  Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || content == nullptr || content[0] == '\0' ||
      s->store->self_id == 0) {
    return 0;
  }

  // a snowflake of now, like the web client's nonces, with a counter in the
  // low bits so two messages of the same millisecond differ.
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  Snowflake nonce = snowflake_from_time_ms(now) | (++s->nonce_counter & 0xfff);

  Message *pending = store_add_pending(s->store, c, content, nonce, reply_to);

  transmit(s, c, pending, reply_ping);
  changed(s);
  return nonce;
}

static Message *find_failed(Channel *c, Snowflake nonce) {
  for (size_t i = 0; c != nullptr && i < c->nmessages; i++) {
    Message *m = c->messages[i];

    if (m->state == MESSAGE_FAILED && m->nonce == nonce) {
      return m;
    }
  }

  return nullptr;
}

void session_retry(Session *s, Snowflake channel_id, Snowflake nonce) {
  Channel *c = store_channel(s->store, channel_id);
  Message *failed = find_failed(c, nonce);

  if (failed == nullptr) {
    return;
  }

  failed->state = MESSAGE_PENDING;
  str_set(&failed->error, nullptr);
  store_touch_channel(s->store, c);

  transmit(s, c, failed, true);
  changed(s);
}

void session_discard(Session *s, Snowflake channel_id, Snowflake nonce) {
  store_remove_pending(s->store, channel_id, nonce);
  changed(s);
}

// applies an event made up here, for what is done offline.
static void apply_local(Session *s, const char *type, cima_json_writer *w) {
  cilo_arena arena = {};

  events_dispatch(
    s->store, type,
    json_parse(&arena, cima_json_writer_text(w), w->len, nullptr));
  cilo_arena_drop(&arena);
  cima_json_writer_dispose(w);
}

void session_edit(Session *s, Snowflake channel_id, Snowflake message_id,
                  const char *content) {
  Channel *c = store_channel(s->store, channel_id);

  if (store_message(c, message_id) == nullptr || content == nullptr) {
    return;
  }

  if (s->offline) {
    cima_json_writer w = {};
    char timestamp[40];

    iso_now(s->store, timestamp);

    json_begin_object(&w);
    json_key(&w, "id"), snowflake_write(&w, message_id);
    json_key(&w, "channel_id"), snowflake_write(&w, channel_id);
    json_field(&w, "content", content);
    json_field(&w, "edited_timestamp", timestamp);
    json_end_object(&w);

    apply_local(s, "MESSAGE_UPDATE", &w);
    changed(s);
    return;
  }

  // the gateway tells of the edit once discord took it.
  if (online(s)) {
    api_edit_message(s->api, channel_id, message_id, content, nullptr, nullptr);
  }
}

void session_delete(Session *s, Snowflake channel_id, Snowflake message_id) {
  Channel *c = store_channel(s->store, channel_id);

  if (store_message(c, message_id) == nullptr) {
    return;
  }

  if (s->offline) {
    store_remove_message(s->store, channel_id, message_id);
    changed(s);
    return;
  }

  if (online(s)) {
    api_delete_message(s->api, channel_id, message_id, nullptr, nullptr);
  }
}

void session_react(Session *s, Snowflake channel_id, Snowflake message_id,
                   Snowflake emoji_id, const char *name, bool add) {
  Channel *c = store_channel(s->store, channel_id);

  if (store_message(c, message_id) == nullptr || name == nullptr) {
    return;
  }

  if (s->offline) {
    cima_json_writer w = {};
    cilo_arena arena = {};

    json_begin_object(&w);
    json_key(&w, "id");

    if (emoji_id != 0) {
      snowflake_write(&w, emoji_id);
    }
    else {
      cima_json_write_null(&w);
    }

    json_field(&w, "name", name);
    json_end_object(&w);

    const cima_json *emoji =
      json_parse(&arena, cima_json_writer_text(&w), w.len, nullptr);

    if (add) {
      store_reaction_add(s->store, channel_id, message_id, s->store->self_id,
                         emoji);
    }
    else {
      store_reaction_remove(s->store, channel_id, message_id, s->store->self_id,
                            emoji);
    }

    cilo_arena_drop(&arena);
    cima_json_writer_dispose(&w);
    changed(s);
    return;
  }

  if (!online(s)) {
    return;
  }

  if (add) {
    api_add_reaction(s->api, channel_id, message_id, emoji_id, name, nullptr,
                     nullptr);
  }
  else {
    api_remove_reaction(s->api, channel_id, message_id, emoji_id, name, nullptr,
                        nullptr);
  }
}

void session_typing(Session *s, Snowflake channel_id) {
  double now = platform_now();
  bool recent = s->typing_channel_id == channel_id &&
                now - s->typing_sent_at < TYPING_INTERVAL;

  if (recent || !online(s) || store_channel(s->store, channel_id) == nullptr) {
    return;
  }

  s->typing_channel_id = channel_id;
  s->typing_sent_at = now;
  api_trigger_typing(s->api, channel_id, nullptr, nullptr);
}

void session_ack(Session *s, Snowflake channel_id) {
  Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || c->last_message_id == 0) {
    return;
  }

  Unread unread = store_channel_unread(s->store, c);
  bool acked = store_last_acked(s->store, c->id) >= c->last_message_id;

  if (acked && unread.mentions == 0) {
    return;
  }

  store_ack(s->store, c->id, c->last_message_id, 0);

  if (online(s)) {
    // the flags and the day last viewed are the client's to compute.
    uint32_t flags =
      (c->guild_id != 0 ? 1u : 0u) | (channel_is_thread(c) ? 2u : 0u);
    int64_t now = (int64_t)(store_now(s->store) * 1000);
    uint32_t days = (uint32_t)((now - DISCORD_EPOCH_MS) / 86400000);

    api_ack_message(s->api, c->id, c->last_message_id, flags, days, nullptr,
                    nullptr);
  }

  changed(s);
}

static void on_mentions(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);
  clear_mentions(s);
  s->mentions_loaded = true;

  if (res->ok && (json_type(res->json) == CIMA_JSON_ARRAY)) {
    s->mentions = calloc(json_len(res->json) + 1, sizeof *s->mentions);

    if (s->mentions == nullptr) {
      cilo_oom(json_len(res->json) * sizeof *s->mentions);
    }

    CIMA_EACH(item, json_items(res->json)) {
      const cima_json *author = json_get(item, "author");
      const char *content = json_get_or(item, "content", nullptr);

      // the names the message shows and the ones its text mentions.
      store_put_user(s->store, author);

      CIMA_EACH(user, json_items(json_get(item, "mentions"))) {
        store_put_user(s->store, user);
      }

      s->mentions[s->nmentions++] = (SessionMention){
        .id = snowflake_of(json_get(item, "id")),
        .channel_id = snowflake_of(json_get(item, "channel_id")),
        .author_id = snowflake_of(json_get(author, "id")),
        .content = str_dup(content != nullptr ? content : ""),
      };
    }
  }

  changed(s);
}

void session_fetch_mentions(Session *s) {
  if (s->mentions_loading || !online(s)) {
    return;
  }

  ApiRequest req = {
    .method = "GET",
    .path = "/users/@me/mentions?limit=25&roles=true&everyone=true",
  };

  s->mentions_loading = true;
  api_request(s->api, &req, on_mentions, call_new(s, 0, 0));
  changed(s);
}

// how much a use counts for, from how many days ago it was. the client's.
static uint32_t use_weight(int64_t days) {
  return days <= 3    ? 100
         : days <= 15 ? 70
         : days <= 30 ? 50
         : days <= 45 ? 30
         : days <= 80 ? 10
                      : 1;
}

typedef struct {
  char *key;
  uint64_t frecency;
  size_t at;
} Ranked;

static int by_frecency(const void *a, const void *b) {
  const Ranked *x = a, *y = b;

  if (x->frecency != y->frecency) {
    return x->frecency < y->frecency ? 1 : -1;
  }

  return x->at < y->at ? -1 : x->at > y->at;
}

// the client keeps this many.
constexpr size_t FRECENT_MAX = 42;

static void rank_reactions(Session *s, FrecencyUse *uses, size_t count) {
  // what the client reacts with before the user used any.
  static const char *const STARTERS[] = {
    "100",   "thumbsup", "thumbsdown",       "heart", "point_up", "eyes",
    "weary", "laughing", "white_check_mark", "x",
  };
  constexpr size_t NSTARTERS = sizeof STARTERS / sizeof *STARTERS;

  Ranked *ranked = calloc(count + NSTARTERS, sizeof *ranked);
  size_t n = 0;
  int64_t now = (int64_t)(store_now(s->store) * 1000);

  if (ranked == nullptr) {
    cilo_oom((count + NSTARTERS) * sizeof *ranked);
  }

  for (size_t i = 0; i < count; i++) {
    FrecencyUse *use = &uses[i];
    uint64_t score = 0;

    for (size_t j = 0; j < use->nrecent_uses; j++) {
      if (use->recent_uses[j] > 0) {
        score += use_weight((now - use->recent_uses[j]) / 86400000);
      }
    }

    if (score == 0 || use->nrecent_uses == 0) {
      continue;
    }

    // the uses times the mean weight of the last ones, rounded up.
    uint64_t total = (uint64_t)use->total_uses * score;
    uint64_t frecency = (total + use->nrecent_uses - 1) / use->nrecent_uses;

    ranked[n++] = (Ranked){.key = use->key, .frecency = frecency, .at = i};
    use->key = nullptr;
  }

  if (n == 0) {
    for (size_t i = 0; i < NSTARTERS; i++) {
      ranked[n++] = (Ranked){.key = strdup(STARTERS[i]), .at = i};
    }
  }

  qsort(ranked, n, sizeof *ranked, by_frecency);

  size_t kept = n < FRECENT_MAX ? n : FRECENT_MAX;

  s->frecent_reactions = calloc(kept + 1, sizeof *s->frecent_reactions);

  if (s->frecent_reactions == nullptr) {
    cilo_oom(kept * sizeof *s->frecent_reactions);
  }

  for (size_t i = 0; i < n; i++) {
    if (i < kept) {
      s->frecent_reactions[s->nfrecent_reactions++] = ranked[i].key;
    }
    else {
      free(ranked[i].key);
    }
  }

  free(ranked);
}

static void on_frecency(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  size_t count = 0;
  FrecencyUse *uses = nullptr;

  call_end(call);
  clear_frecency(s);
  s->frecency_loaded = true;

  if (res->ok) {
    const char *settings = json_get_or(res->json, "settings", nullptr);

    if (settings != nullptr) {
      uses = settings_proto_reaction_uses(settings, &count);
    }
  }

  rank_reactions(s, uses, count);

  for (size_t i = 0; i < count; i++) {
    free(uses[i].key);
  }

  free(uses);
  changed(s);
}

static void on_application(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake id = call->id;

  call_end(call);

  if (!res->ok) {
    return;
  }

  CIMA_EACH(application, json_items(res->json)) {
    if (snowflake_of(json_get(application, "id")) != id) {
      continue;
    }

    for (size_t i = 0; i < s->napplications; i++) {
      if (s->applications[i].id == id && s->applications[i].icon == nullptr) {
        s->applications[i].icon =
          str_dup(json_get_or(application, "icon", nullptr));
      }
    }
  }

  changed(s);
}

const char *session_application_icon(Session *s, Snowflake application_id) {
  if (application_id == 0) {
    return nullptr;
  }

  for (size_t i = 0; i < s->napplications; i++) {
    if (s->applications[i].id == application_id) {
      return s->applications[i].icon;
    }
  }

  if (!online(s)) {
    return nullptr;
  }

  char path[96];

  // the route discord's client reads the applications of activities from.
  snprintf(path, sizeof path, "/applications/public?application_ids=%llu",
           (unsigned long long)application_id);

  ApiRequest req = {.method = "GET",
                    .path = path,
                    .route = "/applications/public"};

  SessionApplication *grown = realloc(
    s->applications, (s->napplications + 1) * sizeof *s->applications);

  if (grown == nullptr) {
    cilo_oom(sizeof *grown);
  }

  s->applications = grown;
  s->applications[s->napplications++] =
    (SessionApplication){.id = application_id};
  api_request(s->api, &req, on_application, call_new(s, 0, application_id));
  return nullptr;
}

void session_fetch_frecency(Session *s) {
  if (s->frecency_loading || s->frecency_loaded || !online(s)) {
    return;
  }

  // 2 is the FrecencyUserSettings, which the gateway does not send.
  ApiRequest req = {.method = "GET", .path = "/users/@me/settings-proto/2"};

  s->frecency_loading = true;
  api_request(s->api, &req, on_frecency, call_new(s, 0, 0));
}

void session_pin(Session *s, Snowflake channel_id, Snowflake message_id,
                 bool pin) {
  if (online(s)) {
    // the gateway answers with the message, pinned or not.
    api_pin_message(s->api, channel_id, message_id, pin, nullptr, nullptr);
  }
}

void session_mark_unread(Session *s, Snowflake channel_id,
                         Snowflake message_id) {
  Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || message_id == 0) {
    return;
  }

  // read up to right before the message, which no message need be.
  Snowflake before = message_id - 1;

  store_ack(s->store, c->id, before, 0);

  if (online(s)) {
    api_ack_manual(s->api, c->id, before, 0, nullptr, nullptr);
  }

  changed(s);
}

void session_forward(Session *s, Snowflake channel_id,
                     Snowflake from_channel_id, Snowflake message_id) {
  const Channel *from = store_channel(s->store, from_channel_id);

  if (from == nullptr || !online(s)) {
    return;
  }

  int64_t now = (int64_t)(store_now(s->store) * 1000);
  ApiSendMessage message = {
    .content = "",
    .nonce = snowflake_from_time_ms(now) | (++s->nonce_counter & 0xfff),
    .forward_id = message_id,
    .forward_channel_id = from_channel_id,
    .forward_guild_id = from->guild_id,
  };

  // the forward shows once the gateway sends it.
  api_send_message(s->api, channel_id, &message, nullptr, nullptr);
}

static void on_dm_opened(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  if (res->ok) {
    Channel *c = store_put_channel(s->store, res->json, 0);

    if (c != nullptr) {
      session_select_channel(s, c->id);
    }
  }

  call_end(call);
  changed(s);
}

void session_open_dm(Session *s, Snowflake user_id) {
  size_t count;
  Channel *const *privates = store_private_channels(s->store, &count);

  for (size_t i = 0; i < count; i++) {
    const Channel *c = privates[i];

    if (c->type == CHANNEL_DM && c->nrecipients == 1 &&
        c->recipients[0] == user_id) {
      session_select_channel(s, c->id);
      return;
    }
  }

  if (online(s)) {
    api_open_dm(s->api, user_id, on_dm_opened, call_new(s, 0, user_id));
  }
}

void session_open_group(Session *s, const Snowflake *user_ids, size_t count) {
  if (count == 1) {
    session_open_dm(s, user_ids[0]);
  }
  else if (count > 1 && online(s)) {
    api_open_group(s->api, user_ids, count, on_dm_opened, call_new(s, 0, 0));
  }
}

static void on_guild_created(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);
  s->guild_creating = false;

  // the guild itself comes through the gateway's GUILD_CREATE.
  if (res->ok && snowflake_of(json_get(res->json, "id")) != 0) {
    s->guild_created = snowflake_of(json_get(res->json, "id"));
  }
  else {
    str_set(&s->guild_error,
            res->ok ? "Discord sent no server." : res->error.message);
  }

  changed(s);
}

void session_create_guild(Session *s, const char *name) {
  if (s->guild_creating || name == nullptr || name[0] == '\0') {
    return;
  }

  str_set(&s->guild_error, nullptr);
  s->guild_created = 0;

  if (!online(s)) {
    str_set(&s->guild_error, "Creating a server needs a connection.");
  }
  else {
    s->guild_creating = true;
    api_create_guild(s->api, name, on_guild_created, call_new(s, 0, 0));
  }

  changed(s);
}

static void on_invite(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  bool wanted = s->invite_loading && s->invite_guild_id == call->id;

  call_end(call);

  // another guild was asked for since.
  if (!wanted) {
    return;
  }

  const char *code = json_get_or(res->json, "code", nullptr);

  s->invite_loading = false;

  if (res->ok && code != nullptr) {
    str_set(&s->invite_code, code);
  }
  else {
    str_set(&s->invite_error,
            res->ok ? "Discord sent no invite." : res->error.message);
  }

  changed(s);
}

// the channel an invite lands in: the one shown when it is the guild's,
// else the first the user may invite to.
static const Channel *invite_channel(Session *s, const Guild *g) {
  const Channel *shown = store_channel(s->store, s->selected_channel_id);

  if (shown != nullptr && shown->guild_id == g->id &&
      !channel_is_thread(shown) &&
      store_can(s->store, shown, PERM_CREATE_INSTANT_INVITE)) {
    return shown;
  }

  for (size_t i = 0; i < g->nchannels; i++) {
    const Channel *c = g->channels[i];

    if (c->type != CHANNEL_GUILD_CATEGORY && !channel_is_thread(c) &&
        store_can(s->store, c,
                  PERM_VIEW_CHANNEL | PERM_CREATE_INSTANT_INVITE)) {
      return c;
    }
  }

  return nullptr;
}

void session_create_invite(Session *s, Snowflake guild_id) {
  const Guild *g = store_guild(s->store, guild_id);
  bool known = s->invite_guild_id == guild_id &&
               (s->invite_loading || s->invite_code != nullptr);

  if (g == nullptr || known) {
    return;
  }

  const Channel *c = invite_channel(s, g);

  clear_invite(s);
  s->invite_guild_id = guild_id;

  if (c == nullptr) {
    s->invite_error = str_dup("You cannot invite people to this server.");
  }
  else if (!online(s)) {
    s->invite_error = str_dup("Invites need a connection.");
  }
  else {
    s->invite_loading = true;
    api_create_invite(s->api, c->id, on_invite, call_new(s, 0, guild_id));
  }

  changed(s);
}

void session_set_status(Session *s, UserStatus status, bool invisible,
                        int64_t seconds) {
  const char *name = invisible               ? "invisible"
                     : status == STATUS_IDLE ? "idle"
                     : status == STATUS_DND  ? "dnd"
                                             : "online";
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  int64_t until = seconds > 0 ? now + seconds * 1000 : 0;

  s->store->self_status = invisible ? STATUS_OFFLINE : status;
  s->store->self_status_until = until;
  s->store->version++;

  if (gateway_ready(s)) {
    gateway_update_presence(s->gateway, name, false);
  }

  // the settings are where discord's clients read the status from: saved
  // there, the others of the account follow and it survives a restart.
  // without them known the save would wipe the rest of the status.
  if (online(s) && s->store->status_proto != nullptr) {
    char *settings =
      settings_proto_with_status(s->store->status_proto, name, until, now);
    cima_json_writer w = {};

    json_begin_object(&w);
    json_field(&w, "settings", settings);
    json_end_object(&w);

    ApiRequest req = {.method = "PATCH",
                      .path = "/users/@me/settings-proto/1",
                      .body = cima_json_writer_text(&w)};

    api_request(s->api, &req, nullptr, nullptr);
    cima_json_writer_dispose(&w);
    free(settings);
  }

  watch_status(s);
  changed(s);
}

static void on_status_timer(void *ctx) {
  Session *s = ctx;

  s->status_timer = nullptr;
  s->status_timed = 0;
  watch_status(s);
}

// a status picked for a while ends here, as discord's client ends it: by
// picking online again.
static void watch_status(Session *s) {
  int64_t until = s->store->self_status_until;
  int64_t now = (int64_t)(store_now(s->store) * 1000);

  if (until != 0 && until <= now && s->store->ready && gateway_ready(s)) {
    session_set_status(s, STATUS_ONLINE, false, 0);
    return;
  }

  if (until == s->status_timed) {
    return;
  }

  platform_timer_cancel(s->status_timer);
  s->status_timer = nullptr;
  s->status_timed = until;

  if (until != 0) {
    double delay = (double)(until - now) / 1000 + 0.5;

    s->status_timer =
      platform_timer(delay > 1 ? delay : 1, on_status_timer, s);
  }
}

static void on_profile(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  const cima_json *badges = json_get(res->json, "badges");

  call_end(call);
  s->profile_loading = false;

  if (!res->ok) {
    return;
  }

  clear_profile_badges(s);
  s->profile_loaded = true;
  s->badges = calloc(json_len(badges) + 1, sizeof *s->badges);

  if (s->badges == nullptr) {
    cilo_oom(sizeof *s->badges);
  }

  CIMA_EACH(badge, json_items(badges)) {
    s->badges[s->nbadges++] = (SessionBadge){
      .icon = str_dup(json_get_or(badge, "icon", nullptr)),
      .description = str_dup(json_get_or(badge, "description", nullptr)),
    };
  }

  changed(s);
}

void session_fetch_profile(Session *s) {
  if (s->profile_loading || s->profile_loaded || !online(s) ||
      s->store->self_id == 0) {
    return;
  }

  char path[160];

  // what the client asks its own profile with.
  snprintf(path, sizeof path,
           "/users/%llu/profile?type=popout&with_mutual_guilds=false"
           "&with_mutual_friends=false&with_mutual_friends_count=false",
           (unsigned long long)s->store->self_id);

  ApiRequest req = {.method = "GET",
                    .path = path,
                    .route = "/users/:id/profile"};

  s->profile_loading = true;
  api_request(s->api, &req, on_profile, call_new(s, 0, 0));
}
