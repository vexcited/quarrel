#include "discord/session.h"

#include <inttypes.h>
#include <stdcountof.h>
#include <stddefer.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "discord/admin.h"
#include "discord/api.h"
#include "discord/emoji.h"
#include "discord/expressions.h"
#include "discord/gateway.h"
#include "discord/markdown.h"
#include "discord/remote_auth.h"
#include "discord/settings_proto.h"
#include "discord/uploads.h"
#include "discord/templates.h"
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
  // a bookmark is made rather than taken away.
  bool add;
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
  remote_auth_free(s->qr_held);
  s->qr_held = nullptr;
  wipe(&s->qr_ticket);
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

  for (size_t i = 0; i < c->nmessages && count < countof(ids); i++) {
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

  for (uint32_t block = first; block <= last && count < countof(ranges);
       block++) {
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
  s->voice_ring_id = 0;
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
    s->voice_moves++;
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

// learned from the web client: a call nobody is in yet is made by joining
// its private channel, and the others are rung once discord tells of the
// call, with null for everyone.
static void ring_try(Session *s) {
  char path[96];

  if (s->voice_ring_id == 0 || s->voice_ring_id != s->voice_channel_id ||
      store_call(s->store, s->voice_ring_id) == nullptr) {
    return;
  }

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/call/ring",
           s->voice_ring_id);
  s->voice_ring_id = 0;

  ApiRequest req = {
    .method = "POST",
    .path = path,
    .body = "{\"recipients\":null}",
  };

  api_request(s->api, &req, nullptr, nullptr);
}

static void voice_begin(Session *s) {
  voice_tell(s, s->voice_channel_id);
  s->voice_timer = platform_timer(VOICE_JOIN_TIMEOUT, on_voice_timeout, s);
  ring_try(s);
}

// the client asks whether the other of a dm may be called before it joins,
// and rings only when discord says they may be rung.
static void on_ringable(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake channel_id = call->channel_id;
  uint64_t asked = call->id;

  call_end(call);

  // left, or joined again, while it asked.
  if (s->voice_channel_id != channel_id || s->voice_asked != asked) {
    return;
  }

  if (!res->ok) {
    str_set(&s->voice_error, "Discord does not let you call them.");
    changed(s);
    return;
  }

  if (json_get_coerce(res->json, "ringable", false)) {
    s->voice_ring_id = channel_id;
  }

  voice_begin(s);
  changed(s);
}

void session_voice_join(Session *s, Snowflake channel_id) {
  const Channel *c = store_channel(s->store, channel_id);
  bool failed = s->voice_error != nullptr ||
                (s->voice != nullptr && voice_status(s->voice) == VOICE_FAILED);

  if (c == nullptr || s->offline || !gateway_ready(s) ||
      (s->voice_channel_id == channel_id && !failed)) {
    return;
  }

  // a private channel nobody is in a call of: this starts one.
  bool starts =
    channel_is_private(c) && store_voice_count(s->store, channel_id) == 0;

  voice_reset(s);
  s->voice_guild_id = c->guild_id;
  s->voice_channel_id = channel_id;
  s->voice_asked++;
  // joining answers the call.
  store_stop_ringing(s->store, channel_id);

  if (starts && c->type == CHANNEL_DM) {
    char path[96];

    snprintf(path, sizeof path, "/channels/%" PRIu64 "/call", channel_id);

    ApiRequest req = {.method = "GET", .path = path};

    api_request(s->api, &req, on_ringable,
                call_new(s, channel_id, s->voice_asked));
  }
  else {
    s->voice_ring_id = starts ? channel_id : 0;
    voice_begin(s);
  }

  changed(s);
}

void session_call_decline(Session *s, Snowflake channel_id) {
  const PrivateCall *call = store_call(s->store, channel_id);
  char path[96];

  if (call == nullptr || call->ringer_id == 0) {
    return;
  }

  store_stop_ringing(s->store, channel_id);
  snprintf(path, sizeof path, "/channels/%" PRIu64 "/call/stop-ringing",
           channel_id);

  // no recipients: the account alone, wherever it is logged in.
  ApiRequest req = {.method = "POST", .path = path, .body = "{}"};

  if (online(s)) {
    api_request(s->api, &req, nullptr, nullptr);
  }

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
  chmod(writing, 0o600);

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

static void take_stickers(Session *s, const char *proto);

// the favorites another client changed. the frecency settings are type 2,
// and an update only carries the fields that changed.
static void on_settings_update(Session *s, const cima_json *settings) {
  if (json_get_coerce(settings, "type", (int64_t)0) != 2 ||
      !s->favorite_gifs.present) {
    return;
  }

  const char *proto = json_get_or(settings, "proto", nullptr);
  FavoriteGifs favorites = settings_proto_favorite_gifs(proto);

  if (favorites.present) {
    settings_proto_free_favorites(&s->favorite_gifs);
    s->favorite_gifs = favorites;
  }

  take_stickers(s, proto);
  changed(s);
}

// a message the gateway just brought, told to the ui when the account's
// settings want it told.
static void on_message(Session *s, const cima_json *d) {
  const Channel *c =
    store_channel(s->store, snowflake_of(json_get(d, "channel_id")));
  const Message *m =
    c != nullptr ? store_message(c, snowflake_of(json_get(d, "id"))) : nullptr;

  if (s->options.on_message != nullptr && m != nullptr &&
      store_notifies(s->store, m)) {
    s->options.on_message(s, m);
  }
}

static void on_dispatch(void *ctx, const char *type, const cima_json *data) {
  Session *s = ctx;

  events_dispatch(s->store, type, data);
  watch_status(s);

  if (strcmp(type, "MESSAGE_CREATE") == 0) {
    on_message(s, data);
  }

  if (strcmp(type, "USER_SETTINGS_PROTO_UPDATE") == 0) {
    on_settings_update(s, json_get(data, "settings"));
  }

  if (strcmp(type, "VOICE_STATE_UPDATE") == 0) {
    on_voice_state(s, data);
  }
  else if (strcmp(type, "VOICE_SERVER_UPDATE") == 0) {
    on_voice_server(s, data);
  }
  else if (strcmp(type, "CALL_CREATE") == 0) {
    ring_try(s);
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
  else if (strcmp(type, "USER_NOTE_UPDATE") == 0) {
    session_put_note(s, snowflake_of(json_get(data, "id")),
                     json_get_or(data, "note", nullptr));
  }

  changed(s);
}

static void fetch_bookmark_ids(Session *s);

static void on_status(void *ctx, GatewayStatus status) {
  Session *s = ctx;

  switch (status) {
    case GATEWAY_READY:
      set_state(s, SESSION_READY);

      // a new session knows nothing of what was shown, a resumed one does.
      activate(s);
      // as discord's client does on every connection.
      fetch_bookmark_ids(s);

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

static void clear_emoji_sources(Session *s) {
  for (size_t i = 0; i < s->nemoji_sources; i++) {
    free(s->emoji_sources[i].guild_name);
    free(s->emoji_sources[i].guild_icon);
  }

  free(s->emoji_sources);
  s->emoji_sources = nullptr;
  s->nemoji_sources = 0;
}

static void clear_emoji_ranks(Session *s) {
  for (size_t i = 0; i < s->nfrecent_emojis; i++) {
    free(s->frecent_emojis[i].key);
  }

  free(s->frecent_emojis);
  s->frecent_emojis = nullptr;
  s->nfrecent_emojis = 0;
}

static void clear_emojis(Session *s) {
  clear_emoji_ranks(s);

  for (size_t i = 0; i < s->nemoji_uses; i++) {
    free(s->emoji_uses[i].key);
  }

  for (size_t i = 0; i < s->nfavorite_emojis; i++) {
    free(s->favorite_emojis[i]);
  }

  free(s->emoji_uses);
  free(s->favorite_emojis);
  s->emoji_uses = nullptr;
  s->favorite_emojis = nullptr;
  s->nemoji_uses = s->nfavorite_emojis = 0;
}

static void clear_frecency(Session *s) {
  for (size_t i = 0; i < s->nfrecent_reactions; i++) {
    free(s->frecent_reactions[i]);
  }

  clear_emojis(s);
  clear_emoji_sources(s);
  platform_timer_cancel(s->emoji_timer);
  s->emoji_timer = nullptr;
  s->emoji_uses_known = false;
  free(s->frecent_reactions);
  s->frecent_reactions = nullptr;
  s->nfrecent_reactions = 0;
  settings_proto_free_favorites(&s->favorite_gifs);
  settings_proto_free_stickers(&s->stickers);
  free(s->frecent_stickers);
  s->frecent_stickers = nullptr;
  s->nfrecent_stickers = 0;
  platform_timer_cancel(s->sticker_timer);
  s->sticker_timer = nullptr;
  s->frecency_loading = false;
  s->frecency_loaded = false;
}

typedef struct SessionApplication {
  Snowflake id;
  char *icon;
} SessionApplication;

static void clear_sticker_infos(Session *s) {
  for (size_t i = 0; i < s->nsticker_infos; i++) {
    free(s->sticker_infos[i].name);
    free(s->sticker_infos[i].description);
    free(s->sticker_infos[i].guild_name);
    free(s->sticker_infos[i].guild_icon);
  }

  free(s->sticker_infos);
  s->sticker_infos = nullptr;
  s->nsticker_infos = 0;
}

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

static void clear_profiles(Session *s);

static void clear_profile(Session *s) {
  clear_profile_badges(s);
  clear_profiles(s);
  free(s->dm_text);
  s->dm_text = nullptr;
  s->dm_user = 0;
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

static void clear_forum(Session *s);
static void clear_friend(Session *s);
static void clear_bookmarks(Session *s);
static void qr_stop(Session *s);
static void clear_directories(Session *s);

static void end_session(Session *s, const char *error) {
  uploads_clear(s);
  qr_stop(s);
  clear_forum(s);
  clear_directories(s);
  clear_mentions(s);
  clear_bookmarks(s);
  clear_frecency(s);
  clear_applications(s);
  clear_sticker_infos(s);
  clear_profile(s);
  clear_invite(s);
  clear_friend(s);
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

// the call of a qr code's ticket sent again with a solved challenge, which
// no attempt of the code's is.
constexpr Snowflake QR_HELD = UINT64_MAX;

static void qr_on_token(void *ctx, const ApiResponse *res);

void session_submit_captcha(Session *s, const char *solution) {
  bool qr = s->qr_held != nullptr && s->qr_ticket != nullptr;

  if (s->state != SESSION_CAPTCHA || (s->login == nullptr && !qr) ||
      solution == nullptr) {
    return;
  }

  // the request copies what it needs before the challenge is forgotten.
  ApiCaptchaSolution solved = {
    .key = solution,
    .session_id = s->captcha.session_id,
    .rqtoken = s->captcha.rqtoken,
  };

  if (qr) {
    api_remote_auth_login(s->api, s->qr_ticket, &solved, qr_on_token,
                          call_new(s, 0, QR_HELD));
  }
  else {
    api_login(s->api, s->login, s->password, &solved, on_login, s);
  }

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

// discord wants a challenge solved before it gives the token: the ticket
// and the attempt's key are kept through it, the code is hidden meanwhile.
static void qr_challenged(Session *s, const ApiCaptcha *asked, bool held) {
  // a challenge answered with another one keeps the key it holds.
  if (!held) {
    remote_auth_free(s->qr_held);
    s->qr_held = s->remote_auth;
    s->remote_auth = nullptr;
  }

  clear_captcha(s);
  s->captcha = (SessionCaptcha){
    .service = str_dup(asked->service),
    .sitekey = str_dup(asked->sitekey),
    .rqdata = str_dup(asked->rqdata),
    .session_id = str_dup(asked->session_id),
    .rqtoken = str_dup(asked->rqtoken),
    .serve_invisible = asked->serve_invisible,
  };
  set_error(s, nullptr);
  set_state(s, SESSION_CAPTCHA);
}

static void qr_on_token(void *ctx, const ApiResponse *res) {
  Call *call = ctx;
  Session *s = call->s;
  bool held = call->id == QR_HELD;
  bool current = held ? s->qr_held != nullptr && s->state == SESSION_LOGGING_IN
                      : call->id == s->qr_attempt;

  if (call_stale(call)) {
    return;
  }

  call_end(call);

  if (!current) {
    return;
  }

  if (res->captcha != nullptr) {
    qr_challenged(s, res->captcha, held);
    return;
  }

  const char *encrypted =
    res->ok ? json_get_or(res->json, "encrypted_token", nullptr) : nullptr;
  char *token =
    remote_auth_decrypt(held ? s->qr_held : s->remote_auth, encrypted);
  defer wipe(&token);

  if (token != nullptr && token[0] != '\0') {
    enter(s, token, false);
  }
  else if (held) {
    // the code starts over, with what went wrong under the form.
    set_error(s, res->ok ? "The QR code login failed, try again."
                         : res->error.message);
    clear_login(s);
    set_state(s, SESSION_LOGGED_OUT);
  }
  else {
    wipe(&s->qr_ticket);
    qr_failed(s);
  }
}

static void qr_on_ticket(void *ctx, const char *ticket) {
  Session *s = ctx;

  if (s->api == nullptr) {
    s->api = api_new();
  }

  str_set(&s->qr_ticket, ticket);
  qr_set(s, SESSION_QR_LOGGING_IN);
  api_remote_auth_login(s->api, ticket, nullptr, qr_on_token,
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
  s->options.on_message = nullptr;

  qr_stop(s);
  voice_reset(s);
  voice_free(s->voice);
  gateway_free(s->gateway);
  api_free(s->api);
  // after the api, whose answers it would wait for.
  gifs_free(s->gifs);
  admin_free(s->admin);
  platform_timer_cancel(s->typing_timer);

  for (Call *call = s->calls; call != nullptr;) {
    Call *next = call->next;
    free(call);
    call = next;
  }

  clear_login(s);
  uploads_clear(s);
  clear_forum(s);
  clear_directories(s);
  clear_mentions(s);
  clear_bookmarks(s);
  clear_frecency(s);
  clear_applications(s);
  clear_sticker_infos(s);
  clear_profile(s);
  clear_invite(s);
  clear_friend(s);
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
  defer wipe(&token);

  if (token == nullptr || token[0] == '\0') {
    if (missing || token != nullptr) {
      forget(r, id);
    }

    return false;
  }

  Session *s = session_new(r);

  account_add(r, id)->session = s;
  enter(s, token, true);
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
  // nothing was bookmarked that a fixture would know of.
  s->store->bookmarks_known = true;
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
  r->options.on_message = nullptr;
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

  message_write_stickers(&w, pending);
  json_end_object(&w);

  cilo_arena arena = {};
  const cima_json *echoed =
    json_parse(&arena, cima_json_writer_text(&w), w.len, nullptr);

  store_create_message(s->store, echoed);
  session_track_stickers(s, echoed);
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
    session_track_stickers(s, res->json);
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

  Snowflake stickers[MESSAGE_STICKERS_MAX];
  size_t nstickers = message_sticker_ids(pending, stickers);

  ApiSendMessage message = {
    .content = pending->content,
    .sticker_ids = stickers,
    .nsticker_ids = nstickers,
    .nonce = pending->nonce,
    .reply_to = pending->reference_id,
    .reply_ping = reply_ping,
  };

  api_send_message(s->api, c->id, &message, on_sent,
                   call_new(s, c->id, pending->nonce));
}

Snowflake session_send(Session *s, Snowflake channel_id, const char *content,
                       Snowflake reply_to, bool reply_ping) {
  return session_send_stickers(s, channel_id, content, nullptr, 0, reply_to,
                               reply_ping);
}

static void track_emojis(Session *s, const char *content);

Snowflake session_send_stickers(Session *s, Snowflake channel_id,
                                const char *content,
                                const Snowflake *sticker_ids, size_t nstickers,
                                Snowflake reply_to, bool reply_ping) {
  Channel *c = store_channel(s->store, channel_id);

  Upload *files[1];
  bool attached = uploads_of(s, channel_id, 0, files, countof(files)) != 0;

  nstickers =
    nstickers < MESSAGE_STICKERS_MAX ? nstickers : MESSAGE_STICKERS_MAX;

  // files and stickers may go without text.
  if (c == nullptr || content == nullptr ||
      (content[0] == '\0' && !attached && nstickers == 0) ||
      s->store->self_id == 0) {
    return 0;
  }

  // a snowflake of now, like the web client's nonces, with a counter in the
  // low bits so two messages of the same millisecond differ.
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  Snowflake nonce = snowflake_from_time_ms(now) | (++s->nonce_counter & 0xfff);

  Message *pending = store_add_pending(s->store, c, content, nonce, reply_to,
                                       sticker_ids, nstickers);

  if (uploads_attach(s, channel_id, nonce, reply_ping)) {
    uploads_send(s, channel_id, nonce);
  }
  else {
    transmit(s, c, pending, reply_ping);
  }

  track_emojis(s, content);
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

  if (uploads_owns(s, nonce)) {
    uploads_send(s, channel_id, nonce);
  }
  else {
    transmit(s, c, failed, true);
  }

  changed(s);
}

void session_discard(Session *s, Snowflake channel_id, Snowflake nonce) {
  if (uploads_owns(s, nonce)) {
    uploads_cancel(s, channel_id, nonce);
    return;
  }

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
  bool starter = c != nullptr && c->starter != nullptr &&
                 c->starter->id == message_id;

  if ((store_message(c, message_id) == nullptr && !starter) ||
      name == nullptr) {
    return;
  }

  // shown at once. the gateway telling of the user's own reaction after
  // changes nothing.
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

  if (online(s) && add) {
    api_add_reaction(s->api, channel_id, message_id, emoji_id, name, nullptr,
                     nullptr);
  }
  else if (online(s)) {
    api_remove_reaction(s->api, channel_id, message_id, emoji_id, name, nullptr,
                        nullptr);
  }

  changed(s);
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

/* ========================================================================== */
/* bookmarks. discord's client asks which messages are bookmarked on every    */
/* connection, then lists them by pages of 25 as the inbox scrolls.          */
/* ========================================================================== */

// how many bookmarks discord keeps for an account, as its client tells.
constexpr int BOOKMARKS_MAX = 500;
// discord's code for a full list of saved messages.
constexpr uint32_t TOO_MANY_SAVED_MESSAGES = 30074;

static void notify(Session *s, const char *notice, bool failed) {
  str_set(&s->notice, notice);
  s->notice_failed = failed;
  s->notice_version++;
}

// forgets the pages fetched, and drops the answers on their way.
static void restart_bookmarks(Session *s) {
  free(s->bookmarks_before);
  s->bookmarks_before = nullptr;
  s->bookmarks_generation++;
  s->bookmarks_loading = false;
  s->bookmarks_more = false;
  s->bookmarks_fetched = false;
  s->bookmarks_failed = false;
}

static void clear_bookmarks(Session *s) {
  restart_bookmarks(s);
  free(s->notice);
  s->notice = nullptr;
  s->notice_failed = false;
}

static void on_bookmark_ids(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  uint64_t generation = call->id;

  call_end(call);

  if (generation != s->bookmarks_generation) {
    return;
  }

  // the client takes a failure for no bookmarks at all.
  store_set_bookmark_ids(s->store,
                         res->ok ? json_get(res->json, "bookmark_ids")
                                 : nullptr);
  s->bookmarks_loading = false;
  s->bookmarks_more = s->store->nbookmark_ids > 0;
  changed(s);
}

// which messages are bookmarked, the list starting over.
static void fetch_bookmark_ids(Session *s) {
  if (!online(s)) {
    return;
  }

  ApiRequest req = {
    .method = "GET",
    .path = "/users/@me/saved-messages?bookmark_ids=true",
    .route = "/users/@me/saved-messages",
  };

  restart_bookmarks(s);
  s->bookmarks_loading = true;
  api_request(s->api, &req, on_bookmark_ids,
              call_new(s, 0, s->bookmarks_generation));
}

static void on_bookmarks(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  uint64_t generation = call->id;

  call_end(call);

  if (generation != s->bookmarks_generation) {
    return;
  }

  s->bookmarks_loading = false;

  if (!res->ok) {
    s->bookmarks_failed = true;
    changed(s);
    return;
  }

  const char *last = nullptr;

  CIMA_EACH(saved, json_items(json_get(res->json, "results"))) {
    store_put_saved_message(s->store, saved, true);
    last = json_get_or(json_get(saved, "save_data"), "saved_at", last);
  }

  if (last != nullptr) {
    str_set(&s->bookmarks_before, last);
  }

  s->bookmarks_fetched = true;
  s->bookmarks_failed = false;
  s->bookmarks_more =
    json_get_coerce(res->json, "has_more", false) && last != nullptr;
  changed(s);
}

void session_fetch_bookmarks(Session *s, bool more) {
  bool first = !s->bookmarks_fetched && !s->bookmarks_failed;

  if (!online(s) || s->bookmarks_loading || !s->store->bookmarks_known ||
      !s->bookmarks_more || (!more && !first)) {
    return;
  }

  char path[160] = "/users/@me/saved-messages/bookmarks?limit=25";

  if (s->bookmarks_before != nullptr) {
    char *before = url_encode(s->bookmarks_before);

    snprintf(path, sizeof path,
             "/users/@me/saved-messages/bookmarks?before=%s&limit=25", before);
    free(before);
  }

  ApiRequest req = {
    .method = "GET",
    .path = path,
    .route = "/users/@me/saved-messages/bookmarks",
  };

  s->bookmarks_loading = true;
  api_request(s->api, &req, on_bookmarks,
              call_new(s, 0, s->bookmarks_generation));
  changed(s);
}

static void on_bookmark_set(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake message_id = call->id;
  bool add = call->add;

  call_end(call);

  if (!res->ok && res->error.code == TOO_MANY_SAVED_MESSAGES) {
    char line[160];

    snprintf(line, sizeof line,
             "You've reached the limit for bookmarks (%d total). To add more, "
             "please delete a few messages.",
             BOOKMARKS_MAX);
    notify(s, line, true);
  }
  else if (!res->ok) {
    notify(s, res->error.message, true);
  }
  // the gateway tells too, the answer is only here first.
  else if (add) {
    store_put_saved_message(s->store, res->json, false);
    notify(s, "Bookmark added. For your eyes only.", false);
  }
  else {
    store_remove_saved_message(s->store, message_id);
    notify(s, "Bookmark removed.", false);
  }

  changed(s);
}

void session_bookmark(Session *s, Snowflake channel_id, Snowflake message_id,
                      bool add) {
  if (channel_id == 0 || message_id == 0) {
    return;
  }

  if (s->offline) {
    if (add) {
      store_add_bookmark(s->store, channel_id, message_id,
                         (int64_t)(store_now(s->store) * 1000));
    }
    else {
      store_remove_saved_message(s->store, message_id);
    }

    notify(s, add ? "Bookmark added. For your eyes only." : "Bookmark removed.",
           false);
    changed(s);
    return;
  }

  if (!online(s)) {
    return;
  }

  char path[96];

  snprintf(path, sizeof path, "/users/@me/saved-messages/%" PRIu64 "/%" PRIu64,
           channel_id, message_id);

  // no due date makes a bookmark rather than a reminder.
  ApiRequest req = {
    .method = add ? "PUT" : "DELETE",
    .path = path,
    .route = "/users/@me/saved-messages/:id/:id",
    .body = add ? "{\"source\":\"context_menu\"}" : nullptr,
  };
  Call *call = call_new(s, channel_id, message_id);

  call->add = add;
  api_request(s->api, &req, on_bookmark_set, call);
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
  constexpr size_t NSTARTERS = countof(STARTERS);

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
        score += frecency_weight((now - use->recent_uses[j]) / 86400000);
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

/* ========================================================================== */
/* stickers: the ones used the most and the ones starred.                     */
/* ========================================================================== */

// the client keeps this many of the most used, and this many starred.
constexpr size_t FRECENT_STICKERS = 20;
constexpr size_t FAVORITE_STICKERS_MAX = 250;
// the client saves what it counts by itself this long after.
constexpr double STICKER_SAVE_DELAY = 30;

typedef struct {
  Snowflake id;
  uint64_t frecency;
} RankedSticker;

static int by_sticker_frecency(const void *a, const void *b) {
  const RankedSticker *x = a, *y = b;

  if (x->frecency != y->frecency) {
    return x->frecency < y->frecency ? 1 : -1;
  }

  return x->id < y->id ? -1 : x->id > y->id;
}

// the stickers used the most among the ones of the user's guilds.
static void rank_stickers(Session *s) {
  size_t count = s->stickers.nuses, n = 0;
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  RankedSticker *ranked = calloc(count ? count : 1, sizeof *ranked);

  if (ranked == nullptr) {
    cilo_oom(count * sizeof *ranked);
  }

  for (size_t i = 0; i < count; i++) {
    const StickerUse *use = &s->stickers.uses[i];
    uint64_t frecency = settings_proto_sticker_frecency(use, now);

    if (frecency != 0 && store_sticker(s->store, use->id, nullptr) != nullptr) {
      ranked[n++] = (RankedSticker){.id = use->id, .frecency = frecency};
    }
  }

  qsort(ranked, n, sizeof *ranked, by_sticker_frecency);
  n = n < FRECENT_STICKERS ? n : FRECENT_STICKERS;

  free(s->frecent_stickers);
  s->frecent_stickers = calloc(n ? n : 1, sizeof *s->frecent_stickers);
  s->nfrecent_stickers = 0;

  if (s->frecent_stickers == nullptr) {
    cilo_oom(n * sizeof *s->frecent_stickers);
  }

  for (size_t i = 0; i < n; i++) {
    s->frecent_stickers[s->nfrecent_stickers++] = ranked[i].id;
  }

  free(ranked);
}

// the stickers of a FrecencyUserSettings, each kind replacing the one known
// when the message carries it.
static void take_stickers(Session *s, const char *proto) {
  StickerSettings got = settings_proto_stickers(proto);

  if (got.has_favorites) {
    free(s->stickers.favorites);
    s->stickers.favorites = got.favorites;
    s->stickers.nfavorites = got.nfavorites;
    s->stickers.has_favorites = true;
    got.favorites = nullptr;
  }

  // what was counted here and not saved yet would be lost to it.
  if (got.has_uses && s->sticker_timer == nullptr) {
    free(s->stickers.uses);
    s->stickers.uses = got.uses;
    s->stickers.nuses = got.nuses;
    s->stickers.has_uses = true;
    got.uses = nullptr;
  }

  settings_proto_free_stickers(&got);
  rank_stickers(s);
}

static void save_frecency(Session *s, char *settings, ApiDone done) {
  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "settings", settings);
  json_end_object(&w);

  ApiRequest req = {
    .method = "PATCH",
    .path = "/users/@me/settings-proto/2",
    .body = cima_json_writer_text(&w),
  };

  api_request(s->api, &req, done, done ? call_new(s, 0, 0) : nullptr);
  cima_json_writer_dispose(&w);
  free(settings);
}

static void on_sticker_timer(void *ctx) {
  Session *s = ctx;

  s->sticker_timer = nullptr;

  if (online(s) && s->stickers.has_uses) {
    int64_t now = (int64_t)(store_now(s->store) * 1000);

    save_frecency(s,
                  settings_proto_with_sticker_uses(s->stickers.uses,
                                                   s->stickers.nuses, now),
                  nullptr);
  }
}

static void track_sticker(Session *s, Snowflake id, int64_t now) {
  StickerSettings *k = &s->stickers;
  StickerUse *use = nullptr;

  for (size_t i = 0; i < k->nuses && use == nullptr; i++) {
    use = k->uses[i].id == id ? &k->uses[i] : nullptr;
  }

  if (use == nullptr) {
    k->uses = realloc(k->uses, (k->nuses + 1) * sizeof *k->uses);

    if (k->uses == nullptr) {
      cilo_oom((k->nuses + 1) * sizeof *k->uses);
    }

    use = &k->uses[k->nuses++];
    *use = (StickerUse){.id = id};
  }

  // the oldest goes once there are as many as the client keeps.
  if (use->nrecent_uses == FRECENCY_SAMPLES) {
    memmove(use->recent_uses, use->recent_uses + 1,
            (FRECENCY_SAMPLES - 1) * sizeof *use->recent_uses);
    use->nrecent_uses--;
  }

  use->recent_uses[use->nrecent_uses++] = now;
  use->total_uses++;
}

// the ones used the most are ranked again, and saved in a while.
static void stickers_used(Session *s) {
  rank_stickers(s);

  if (s->sticker_timer == nullptr && online(s)) {
    s->sticker_timer = platform_timer(STICKER_SAVE_DELAY, on_sticker_timer, s);
  }
}

void session_track_stickers(Session *s, const cima_json *message) {
  const cima_json *items = json_get(message, "sticker_items");
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  bool any = false;

  // counting before the settings came would save over what they hold.
  if (!s->stickers.has_uses && !s->offline) {
    return;
  }

  CIMA_EACH(item, json_items(items)) {
    Snowflake id = snowflake_of(json_get(item, "id"));

    if (id != 0) {
      track_sticker(s, id, now);
      any = true;
    }
  }

  if (any) {
    stickers_used(s);
  }
}

void session_use_sticker(Session *s, Snowflake id) {
  if (id == 0 || (!s->stickers.has_uses && !s->offline)) {
    return;
  }

  track_sticker(s, id, (int64_t)(store_now(s->store) * 1000));
  stickers_used(s);
}

static StickerInfo *sticker_info_of(Session *s, Snowflake id) {
  for (size_t i = 0; i < s->nsticker_infos; i++) {
    if (s->sticker_infos[i].id == id) {
      return &s->sticker_infos[i];
    }
  }

  return nullptr;
}

static void on_sticker_guild(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  StickerInfo *info = sticker_info_of(s, call->id);

  call_end(call);

  if (info == nullptr) {
    return;
  }

  info->loading = false;

  // a guild nobody may find answers 404.
  if (res->ok) {
    str_set(&info->guild_name, json_get_or(res->json, "name", nullptr));
    str_set(&info->guild_icon, json_get_or(res->json, "icon", nullptr));
    info->guild_online = (uint32_t)json_get_coerce(
      res->json, "approximate_presence_count", (uint64_t)0);
  }

  changed(s);
}

static void on_sticker_info(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake id = call->id;
  StickerInfo *info = sticker_info_of(s, id);

  call_end(call);

  if (info == nullptr) {
    return;
  }

  info->loading = false;

  if (res->ok) {
    str_set(&info->name, json_get_or(res->json, "name", nullptr));
    str_set(&info->description, json_get_or(res->json, "description", nullptr));
    info->guild_id = snowflake_of(json_get(res->json, "guild_id"));
  }

  // the guild of a sticker from elsewhere, when discord lists it.
  if (info->guild_id != 0 && store_guild(s->store, info->guild_id) == nullptr) {
    char path[96];

    snprintf(path, sizeof path, "/stickers/%" PRIu64 "/guild", id);
    info->loading = true;
    api_request(s->api,
                &(ApiRequest){.method = "GET",
                              .path = path,
                              .route = "/stickers/:id/guild"},
                on_sticker_guild, call_new(s, 0, id));
  }

  changed(s);
}

const StickerInfo *session_sticker_info(Session *s, Snowflake id) {
  StickerInfo *known = sticker_info_of(s, id);

  if (known != nullptr) {
    return known;
  }

  StickerInfo *grown = realloc(s->sticker_infos, (s->nsticker_infos + 1) *
                                                   sizeof *s->sticker_infos);

  if (grown == nullptr) {
    cilo_oom(sizeof *grown);
  }

  s->sticker_infos = grown;

  StickerInfo *info = &s->sticker_infos[s->nsticker_infos++];

  *info = (StickerInfo){.id = id, .loading = online(s)};

  if (info->loading) {
    char path[64];

    snprintf(path, sizeof path, "/stickers/%" PRIu64, id);
    api_request(
      s->api,
      &(ApiRequest){.method = "GET", .path = path, .route = "/stickers/:id"},
      on_sticker_info, call_new(s, 0, id));
  }

  return info;
}

bool session_favorite_sticker(const Session *s, Snowflake id) {
  for (size_t i = 0; i < s->stickers.nfavorites; i++) {
    if (s->stickers.favorites[i] == id) {
      return true;
    }
  }

  return false;
}

static void on_sticker_favorites_saved(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  // a failure fetches them again, so what shows is what discord keeps.
  if (!res->ok) {
    notify(s, "Your favorite stickers could not be saved.", true);
    clear_frecency(s);
    session_fetch_frecency(s);
  }

  changed(s);
}

void session_set_favorite_sticker(Session *s, Snowflake id, bool favorite) {
  StickerSettings *k = &s->stickers;
  bool known = s->offline || (online(s) && k->has_favorites);

  // saving replaces them all, never before they are known.
  if (!known || id == 0 || favorite == session_favorite_sticker(s, id)) {
    return;
  }

  if (favorite) {
    // the client's words for it.
    if (k->nfavorites >= FAVORITE_STICKERS_MAX) {
      notify(s, "You cannot have more than 250 favorites.", true);
      changed(s);
      return;
    }

    k->favorites =
      realloc(k->favorites, (k->nfavorites + 1) * sizeof *k->favorites);

    if (k->favorites == nullptr) {
      cilo_oom((k->nfavorites + 1) * sizeof *k->favorites);
    }

    k->favorites[k->nfavorites++] = id;
  }
  else {
    size_t kept = 0;

    for (size_t i = 0; i < k->nfavorites; i++) {
      if (k->favorites[i] != id) {
        k->favorites[kept++] = k->favorites[i];
      }
    }

    k->nfavorites = kept;
  }

  if (online(s)) {
    save_frecency(
      s, settings_proto_with_favorite_stickers(k->favorites, k->nfavorites),
      on_sticker_favorites_saved);
  }

  changed(s);
}

static int by_emoji_frecency(const void *a, const void *b) {
  const FrecentEmoji *x = a, *y = b;

  return (x->frecency < y->frecency) - (x->frecency > y->frecency);
}

// ranks the emojis written in messages again from their uses.
static void rank_emojis(Session *s) {
  int64_t now = (int64_t)(store_now(s->store) * 1000);

  clear_emoji_ranks(s);
  s->frecent_emojis = calloc(s->nemoji_uses + 1, sizeof *s->frecent_emojis);

  if (s->frecent_emojis == nullptr) {
    cilo_oom(s->nemoji_uses * sizeof *s->frecent_emojis);
  }

  for (size_t i = 0; i < s->nemoji_uses; i++) {
    const FrecencyUse *use = &s->emoji_uses[i];
    uint32_t score = 0;

    for (size_t j = 0; j < use->nrecent_uses; j++) {
      if (use->recent_uses[j] > 0) {
        score += frecency_weight((now - use->recent_uses[j]) / 86400000);
      }
    }

    // the client forgets what scores nothing.
    if (score == 0) {
      continue;
    }

    uint64_t total = (uint64_t)use->total_uses * score;

    s->frecent_emojis[s->nfrecent_emojis++] = (FrecentEmoji){
      .key = str_dup(use->key),
      .score = score,
      .frecency = (total + use->nrecent_uses - 1) / use->nrecent_uses,
    };
  }

  // stable, so the ones used as much stay in the order they came.
  for (size_t i = 1; i < s->nfrecent_emojis; i++) {
    FrecentEmoji moved = s->frecent_emojis[i];
    size_t at = i;

    for (; at > 0 && by_emoji_frecency(&s->frecent_emojis[at - 1], &moved) > 0;
         at--) {
      s->frecent_emojis[at] = s->frecent_emojis[at - 1];
    }

    s->frecent_emojis[at] = moved;
  }
}

// the client saves what it counts by itself this long after.
constexpr double EMOJI_SAVE_DELAY = 30;

static void track_emoji(Session *s, const char *key) {
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  FrecencyUse *use = nullptr;

  for (size_t i = 0; i < s->nemoji_uses && use == nullptr; i++) {
    use = strcmp(s->emoji_uses[i].key, key) == 0 ? &s->emoji_uses[i] : nullptr;
  }

  if (use == nullptr) {
    s->emoji_uses =
      realloc(s->emoji_uses, (s->nemoji_uses + 1) * sizeof *s->emoji_uses);

    if (s->emoji_uses == nullptr) {
      cilo_oom((s->nemoji_uses + 1) * sizeof *s->emoji_uses);
    }

    use = &s->emoji_uses[s->nemoji_uses++];
    *use = (FrecencyUse){.key = str_dup(key)};
  }

  // the client keeps the last ten.
  if (use->nrecent_uses == FRECENCY_SAMPLES) {
    memmove(use->recent_uses, use->recent_uses + 1,
            (FRECENCY_SAMPLES - 1) * sizeof *use->recent_uses);
    use->nrecent_uses--;
  }

  use->recent_uses[use->nrecent_uses++] = now;
  use->total_uses++;
}

// the emojis of the settings, null for settings discord did not give.
static void take_emojis(Session *s, const char *settings) {
  // what the client counts as used before the user wrote any.
  static const char *const STARTERS[] = {
    "thumbsup", "eyes", "laughing",   "watermelon", "fork_and_knife",
    "yum",      "weary", "tired_face", "poop",       "100",
  };

  if (settings != nullptr) {
    s->emoji_uses = settings_proto_emoji_uses(settings, &s->nemoji_uses);
    s->favorite_emojis =
      settings_proto_favorite_emojis(settings, &s->nfavorite_emojis);
    s->emoji_uses_known = true;
  }

  if (s->nemoji_uses == 0) {
    for (size_t i = 0; i < countof(STARTERS); i++) {
      track_emoji(s, STARTERS[i]);
    }
  }

  rank_emojis(s);
}

static void on_emoji_timer(void *ctx) {
  Session *s = ctx;

  s->emoji_timer = nullptr;

  if (online(s) && s->emoji_uses_known) {
    int64_t now = (int64_t)(store_now(s->store) * 1000);

    save_frecency(
      s, settings_proto_with_emoji_uses(s->emoji_uses, s->nemoji_uses, now),
      nullptr);
  }
}

static bool track_emojis_in(Session *s, const MdNode *first) {
  char key[24];
  bool any = false;

  for (const MdNode *n = first; n != nullptr; n = n->next) {
    const char *name = nullptr;
    size_t name_len = 0;
    Snowflake id = 0;
    bool animated = false;

    if (n->kind == MD_CUSTOM_EMOJI) {
      track_emoji(s, snowflake_format(n->id, key));
      any = true;
    }
    // one of the user's guilds sent as a link to its picture, which
    // FakeNitro leaves the client count as any other.
    else if (n->kind == MD_LINK && n->text != nullptr &&
             fake_emoji_url(n->text, &id, &animated, &name, &name_len) &&
             guild_emoji_owner(s->store, id) != nullptr) {
      track_emoji(s, snowflake_format(id, key));
      any = true;
      continue;
    }
    else if (n->kind == MD_EMOJI) {
      const UnicodeEmoji *emoji = emoji_by_utf8(n->text);

      if (emoji != nullptr) {
        track_emoji(s, emoji->name);
        any = true;
      }
    }

    any |= track_emojis_in(s, n->first);
  }

  return any;
}

// counts a use of each emoji a message the user sends holds, as the client
// does once it went.
static void track_emojis(Session *s, const char *content) {
  // nothing is counted before the uses discord holds came: they are asked
  // for, so the next message counts.
  if (!s->frecency_loaded || content == nullptr || *content == '\0') {
    session_fetch_frecency(s);
    return;
  }

  MdDoc *doc = md_parse(content, strlen(content), nullptr);

  if (track_emojis_in(s, doc->root)) {
    rank_emojis(s);

    // counted before discord told of the uses, they would be saved over
    // the ones it holds.
    if (s->emoji_uses_known && s->emoji_timer == nullptr && online(s)) {
      s->emoji_timer = platform_timer(EMOJI_SAVE_DELAY, on_emoji_timer, s);
    }
  }

  md_free(doc);
}

static EmojiSource *emoji_source(Session *s, Snowflake emoji_id) {
  for (size_t i = 0; i < s->nemoji_sources; i++) {
    if (s->emoji_sources[i].emoji_id == emoji_id) {
      return &s->emoji_sources[i];
    }
  }

  return nullptr;
}

static void on_emoji_source(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  EmojiSource *source = emoji_source(s, call->id);

  call_end(call);

  if (source == nullptr) {
    return;
  }

  source->loading = false;

  // a guild that keeps to itself is answered with an error, or without one.
  const cima_json *guild = res->ok ? json_get(res->json, "guild") : nullptr;

  if (json_type(guild) == CIMA_JSON_OBJECT) {
    source->guild_id = snowflake_of(json_get(guild, "id"));
    source->guild_name = str_dup(json_get_or(guild, "name", nullptr));
    source->guild_icon = str_dup(json_get_or(guild, "icon", nullptr));
    source->online = (uint32_t)json_get_coerce(
      guild, "approximate_presence_count", (uint64_t)0);
  }

  changed(s);
}

const EmojiSource *session_emoji_source(Session *s, Snowflake emoji_id) {
  EmojiSource *known = emoji_source(s, emoji_id);

  if (known != nullptr || emoji_id == 0 || !online(s)) {
    return known;
  }

  char path[64];

  snprintf(path, sizeof path, "/emojis/%" PRIu64 "/source", emoji_id);

  ApiRequest req = {.method = "GET",
                    .path = path,
                    .route = "/emojis/:id/source"};
  EmojiSource *grown = realloc(
    s->emoji_sources, (s->nemoji_sources + 1) * sizeof *s->emoji_sources);

  if (grown == nullptr) {
    cilo_oom(sizeof *grown);
  }

  s->emoji_sources = grown;
  s->emoji_sources[s->nemoji_sources] =
    (EmojiSource){.emoji_id = emoji_id, .loading = true};
  api_request(s->api, &req, on_emoji_source, call_new(s, 0, emoji_id));
  return &s->emoji_sources[s->nemoji_sources++];
}

uint32_t session_emoji_score(const Session *s, const char *key) {
  for (size_t i = 0; i < s->nfrecent_emojis; i++) {
    if (strcmp(s->frecent_emojis[i].key, key) == 0) {
      return s->frecent_emojis[i].score;
    }
  }

  return 0;
}

bool session_emoji_starred(const Session *s, const char *key) {
  for (size_t i = 0; i < s->nfavorite_emojis; i++) {
    if (strcmp(s->favorite_emojis[i], key) == 0) {
      return true;
    }
  }

  return false;
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
      s->favorite_gifs = settings_proto_favorite_gifs(settings);
      take_stickers(s, settings);
    }

    // no sticker starred or used is known too, and only then may either
    // be saved.
    s->stickers.has_favorites = true;
    s->stickers.has_uses = true;

    // none starred is known too, and only then may a star be saved.
    s->favorite_gifs.present = true;
  }

  rank_reactions(s, uses, count);
  take_emojis(
    s, res->ok ? json_get_or(res->json, "settings", nullptr) : nullptr);

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
  // a fixture has no settings: what the client starts from.
  if (s->offline && !s->frecency_loaded) {
    s->frecency_loaded = true;
    rank_reactions(s, nullptr, 0);
    take_emojis(s, nullptr);
    return;
  }

  if (s->frecency_loading || s->frecency_loaded || !online(s)) {
    return;
  }

  // 2 is the FrecencyUserSettings, which the gateway does not send.
  ApiRequest req = {.method = "GET", .path = "/users/@me/settings-proto/2"};

  s->frecency_loading = true;
  api_request(s->api, &req, on_frecency, call_new(s, 0, 0));
}

Gifs *session_gifs(Session *s) {
  if (s->api == nullptr) {
    s->api = api_new();
  }

  if (s->gifs == nullptr) {
    s->gifs = gifs_new(s->api, s->options.on_change);
  }

  return s->gifs;
}

const FavoriteGif *session_favorite_gif(const Session *s, const char *url) {
  for (size_t i = 0; url != nullptr && i < s->favorite_gifs.count; i++) {
    if (strcmp(s->favorite_gifs.gifs[i].url, url) == 0) {
      return &s->favorite_gifs.gifs[i];
    }
  }

  return nullptr;
}

static void on_favorites_saved(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  // discord answers with the settings as they are now. a failure fetches
  // them again, so what shows is what discord keeps.
  FavoriteGifs favorites = settings_proto_favorite_gifs(
    res->ok ? json_get_or(res->json, "settings", nullptr) : nullptr);

  if (favorites.present) {
    settings_proto_free_favorites(&s->favorite_gifs);
    s->favorite_gifs = favorites;
  }
  else if (!res->ok) {
    clear_frecency(s);
    session_fetch_frecency(s);
  }

  changed(s);
}

void session_set_favorite_gif(Session *s, const FavoriteGif *gif,
                              bool favorite) {
  FavoriteGifs *list = &s->favorite_gifs;
  const FavoriteGif *found = session_favorite_gif(s, gif->url);

  // saving replaces them all, never before they are known.
  if (!online(s) || !list->present || favorite == (found != nullptr)) {
    return;
  }

  if (favorite) {
    uint32_t order = 0;

    for (size_t i = 0; i < list->count; i++) {
      order = list->gifs[i].order > order ? list->gifs[i].order : order;
    }

    list->gifs = realloc(list->gifs, (list->count + 1) * sizeof *list->gifs);

    if (list->gifs == nullptr) {
      cilo_oom((list->count + 1) * sizeof *list->gifs);
    }

    list->gifs[list->count++] = (FavoriteGif){
      .url = str_dup(gif->url),
      .src = str_dup(gif->src),
      .format = gif->format,
      .width = gif->width,
      .height = gif->height,
      .order = order + 1,
    };

    // the client stops explaining the star past the second one.
    list->hide_tooltip |= list->count > 2;
  }
  else {
    size_t at = (size_t)(found - list->gifs);

    free(list->gifs[at].url);
    free(list->gifs[at].src);
    list->gifs[at] = list->gifs[--list->count];
  }

  char *settings = settings_proto_with_favorite_gifs(list);

  // too many to save, as the client says.
  if (settings == nullptr) {
    FavoriteGif added = list->gifs[--list->count];

    free(added.url);
    free(added.src);
    changed(s);
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "settings", settings);
  json_end_object(&w);

  ApiRequest req = {
    .method = "PATCH",
    .path = "/users/@me/settings-proto/2",
    .body = cima_json_writer_text(&w),
  };

  api_request(s->api, &req, on_favorites_saved, call_new(s, 0, 0));
  cima_json_writer_dispose(&w);
  free(settings);
  changed(s);
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

    // a message written to them before the conversation existed.
    if (c != nullptr && s->dm_text != nullptr && s->dm_user == call->id) {
      session_send(s, c->id, s->dm_text, 0, false);
    }
  }

  if (s->dm_user == call->id) {
    free(s->dm_text);
    s->dm_text = nullptr;
    s->dm_user = 0;
  }

  call_end(call);
  changed(s);
}

void session_message_user(Session *s, Snowflake user_id,
                          const char *content) {
  size_t count;
  Channel *const *privates = store_private_channels(s->store, &count);

  for (size_t i = 0; i < count; i++) {
    const Channel *c = privates[i];

    if (c->type == CHANNEL_DM && c->nrecipients == 1 &&
        c->recipients[0] == user_id) {
      session_select_channel(s, c->id);
      session_send(s, c->id, content, 0, false);
      return;
    }
  }

  free(s->dm_text);
  s->dm_text = str_dup(content);
  s->dm_user = user_id;
  session_open_dm(s, user_id);
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

/* ========================================================================== */
/* friend requests.                                                           */
/* ========================================================================== */

static void clear_friend_captcha(Session *s) {
  SessionCaptcha *c = &s->friend_captcha;

  free(c->service);
  free(c->sitekey);
  free(c->rqdata);
  free(c->session_id);
  free(c->rqtoken);
  *c = (SessionCaptcha){};
}

static void clear_friend(Session *s) {
  clear_friend_captcha(s);
  str_set(&s->friend_message, nullptr);
  str_set(&s->friend_username, nullptr);
  str_set(&s->friend_note, nullptr);
  s->friend_adding = false;
  s->friend_sent = false;
}

static bool username_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.';
}

// the client's own check, from its add friend form: a username, or with a
// '#' an email like text or a name and a four digit tag.
static bool tag_valid(const char *tag) {
  size_t len = strlen(tag);

  if (!len) {
    return false;
  }

  if (!strchr(tag, '#')) {
    for (const char *c = tag; *c; c++) {
      if (!username_char(*c)) {
        return false;
      }
    }

    return true;
  }

  bool tagged = len >= 6 && tag[len - 5] == '#';

  for (size_t i = len >= 4 ? len - 4 : 0; tagged && i < len; i++) {
    tagged = tag[i] >= '0' && tag[i] <= '9';
  }

  const char *at = strchr(tag + 1, '@');
  const char *dot = at && at[1] ? strchr(at + 2, '.') : nullptr;

  return tagged || (dot && dot[1]);
}

// the length javascript gives, in utf-16 units.
static size_t utf16_length(const char *text) {
  size_t n = 0;

  for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
    if ((*c & 0xc0) != 0x80) {
      n += *c >= 0xf0 ? 2 : 1;
    }
  }

  return n;
}

static char *trimmed(const char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
    text++;
  }

  size_t len = strlen(text);

  while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                 text[len - 1] == '\n' || text[len - 1] == '\r')) {
    len--;
  }

  return str_dup_n(text, len);
}

static void friend_tell(Session *s, const char *message, bool sent) {
  s->friend_adding = false;
  s->friend_sent = sent;
  str_set(&s->friend_message, message);
  changed(s);
}

// what the client's form says of a refused request.
static const char *friend_error(const ApiResponse *res, const char *tag,
                                char *out, size_t cap) {
  if (res->status == 0) {
    return res->error.message;
  }

  if (json_get(res->json, "note") != nullptr) {
    return "Notes can only contain plain text and must be under 120 "
           "characters.";
  }

  switch (res->error.code) {
    case 80000:
      snprintf(out, cap,
               "%s is not accepting friend requests. They'll have to add you "
               "to become friends.",
               tag);
      return out;
    case 30002:
      return "You've maxed out your friend list. Welcome to the elite 1,000 "
             "friends club!";
    case 80007:
      return "You're already friends with that user!";
    case 40068:
    case 340007:
      return "Your account has limited access";
    case 30059:
      return "You've maxed out your block list.";
    case 30078:
      return "You've maxed out your pending outgoing friend requests.";
    // the client says these in a dialog, over the generic text.
    case 40002:
      return "You're going to need to claim your account and verify your "
             "e-mail before you can send a friend request.";
    default:
      if (res->status == 429) {
        return "You're sending friend requests too quickly!";
      }

      return "Hm, didn't work. Double check that the username is correct.";
  }
}

static void on_friend_request(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  char tag[64], message[192];

  call_end(call);

  if (s->friend_username == nullptr) {
    return;
  }

  if (s->friend_discriminator >= 0) {
    snprintf(tag, sizeof tag, "%s#%04d", s->friend_username,
             (int)s->friend_discriminator);
  }
  else {
    snprintf(tag, sizeof tag, "%s", s->friend_username);
  }

  if (res->captcha != nullptr) {
    clear_friend_captcha(s);
    s->friend_captcha = (SessionCaptcha){
      .service = str_dup(res->captcha->service),
      .sitekey = str_dup(res->captcha->sitekey),
      .rqdata = str_dup(res->captcha->rqdata),
      .session_id = str_dup(res->captcha->session_id),
      .rqtoken = str_dup(res->captcha->rqtoken),
      .serve_invisible = res->captcha->serve_invisible,
    };
    s->friend_adding = false;
    changed(s);
    return;
  }

  if (res->ok) {
    snprintf(message, sizeof message,
             "Success! Your friend request to %s was sent.", tag);
    friend_tell(s, message, true);
  }
  else {
    friend_tell(s, friend_error(res, tag, message, sizeof message), false);
  }

  str_set(&s->friend_username, nullptr);
  str_set(&s->friend_note, nullptr);
}

static void send_friend_request(Session *s,
                                const ApiCaptchaSolution *captcha) {
  s->friend_adding = true;
  api_send_friend_request(s->api, s->friend_username, s->friend_discriminator,
                          s->friend_note, captcha, on_friend_request,
                          call_new(s, 0, 0));
}

void session_add_friend(Session *s, const char *tag, const char *note) {
  if (s->friend_adding || tag == nullptr) {
    return;
  }

  clear_friend(s);

  char *wanted = trimmed(tag);
  defer free(wanted);
  char *said = nullptr;
  defer free(said);
  const char *name = wanted;

  if (!strchr(name, '#') && name[0] == '@') {
    name++;
  }

  if (note != nullptr) {
    if (utf16_length(note) > 120) {
      friend_tell(s,
                  "Notes can only contain plain text and must be under 120 "
                  "characters.",
                  false);
      return;
    }

    char *flat = str_dup(note);

    for (char *c = flat; *c; c++) {
      if (*c == '\n') {
        *c = ' ';
      }
    }

    said = trimmed(flat);
    free(flat);
  }

  if (!tag_valid(name)) {
    friend_tell(
      s, "Hm, didn't work. Double check that the username is correct.", false);
  }
  else if (!online(s)) {
    friend_tell(s, "Sending a friend request needs a connection.", false);
  }
  else {
    const char *hash = strchr(name, '#');
    char *digits = nullptr;
    long parsed = hash ? strtol(hash + 1, &digits, 10) : -1;

    s->friend_username =
      hash ? str_dup_n(name, (size_t)(hash - name)) : str_dup(name);
    s->friend_discriminator =
      hash && digits != hash + 1 && parsed >= 0 && parsed <= INT32_MAX
        ? (int32_t)parsed
        : -1;
    s->friend_note = said != nullptr && said[0] ? str_dup(said) : nullptr;
    send_friend_request(s, nullptr);
    changed(s);
  }
}

void session_add_friend_captcha(Session *s, const char *solution) {
  if (s->friend_captcha.service == nullptr || s->friend_username == nullptr ||
      solution == nullptr || !online(s)) {
    return;
  }

  ApiCaptchaSolution solved = {
    .key = solution,
    .session_id = s->friend_captcha.session_id,
    .rqtoken = s->friend_captcha.rqtoken,
  };

  send_friend_request(s, &solved);
  clear_friend_captcha(s);
  changed(s);
}

void session_add_friend_clear(Session *s) {
  if (s->friend_adding) {
    return;
  }

  if (s->friend_message != nullptr || s->friend_captcha.service != nullptr) {
    clear_friend(s);
    changed(s);
  }
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
    // a hub's invites never expire, as the client makes them.
    api_create_invite(s->api, c->id, g->hub ? 0 : 604800, on_invite,
                      call_new(s, 0, guild_id));
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

/* ========================================================================== */
/* profiles of others.                                                        */
/* ========================================================================== */

constexpr size_t PROFILES_KEPT = 64;
// seconds a profile is good for before it is asked again as it shows.
constexpr double PROFILE_FRESH = 60;
constexpr double PROFILE_RETRY = 60;

static void free_badges(SessionBadge *badges, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(badges[i].icon);
    free(badges[i].description);
  }

  free(badges);
}

// what an answer fills, before it is filled again.
static void empty_profile(Profile *p) {
  free(p->bio);
  free(p->pronouns);
  free(p->banner);
  free(p->guild_bio);
  free(p->guild_pronouns);
  free(p->guild_banner);
  free(p->legacy_username);
  free_badges(p->badges, p->nbadges);

  for (size_t i = 0; i < p->nmutual_guilds; i++) {
    free(p->mutual_guilds[i].nick);
  }

  for (size_t i = 0; i < p->nconnections; i++) {
    free(p->connections[i].type);
    free(p->connections[i].name);
  }

  free(p->mutual_guilds);
  free(p->mutual_friends);
  free(p->connections);

  *p = (Profile){
    .user_id = p->user_id,
    .guild_id = p->guild_id,
    .loading = p->loading,
    .full = p->full,
    .friends_loading = p->friends_loading,
    .note = p->note,
    .note_loaded = p->note_loaded,
  };
}

static void free_profile(Profile *p) {
  empty_profile(p);
  free(p->note);
  free(p);
}

static void clear_profiles(Session *s) {
  for (size_t i = 0; i < s->nprofiles; i++) {
    free_profile(s->profiles[i]);
  }

  free(s->profiles);
  s->profiles = nullptr;
  s->nprofiles = 0;
}

static Profile *find_profile(Session *s, Snowflake user_id,
                             Snowflake guild_id) {
  for (size_t i = 0; i < s->nprofiles; i++) {
    Profile *p = s->profiles[i];

    if (p->user_id == user_id && p->guild_id == guild_id) {
      return p;
    }
  }

  return nullptr;
}

// a new profile, the one answered longest ago making room for it.
static Profile *add_profile(Session *s, Snowflake user_id, Snowflake guild_id) {
  if (s->nprofiles == PROFILES_KEPT) {
    size_t oldest = SIZE_MAX;

    for (size_t i = 0; i < s->nprofiles; i++) {
      const Profile *p = s->profiles[i];

      if (!p->loading &&
          (oldest == SIZE_MAX || p->at < s->profiles[oldest]->at)) {
        oldest = i;
      }
    }

    if (oldest == SIZE_MAX) {
      oldest = 0;
    }

    free_profile(s->profiles[oldest]);
    s->profiles[oldest] = s->profiles[--s->nprofiles];
  }

  if (s->profiles == nullptr) {
    s->profiles = calloc(PROFILES_KEPT, sizeof *s->profiles);

    if (s->profiles == nullptr) {
      cilo_oom(PROFILES_KEPT * sizeof *s->profiles);
    }
  }

  Profile *p = calloc(1, sizeof *p);

  if (p == nullptr) {
    cilo_oom(sizeof *p);
  }

  *p = (Profile){.user_id = user_id, .guild_id = guild_id};
  s->profiles[s->nprofiles++] = p;

  return p;
}

static void put_theme(const cima_json *colors, uint32_t theme[2], bool *has) {
  *has = json_type(colors) == CIMA_JSON_ARRAY && json_len(colors) >= 2;

  if (!*has) {
    return;
  }

  size_t i = 0;

  CIMA_EACH(color, json_items(colors)) {
    if (i < 2) {
      theme[i++] = (uint32_t)json_coerce(color, (uint64_t)0);
    }
  }
}

static void put_badges(Profile *p, const cima_json *badges) {
  size_t n = json_type(badges) == CIMA_JSON_ARRAY ? json_len(badges) : 0;

  if (n == 0) {
    return;
  }

  SessionBadge *grown =
    realloc(p->badges, (p->nbadges + n) * sizeof *p->badges);

  if (grown == nullptr) {
    cilo_oom((p->nbadges + n) * sizeof *p->badges);
  }

  p->badges = grown;

  CIMA_EACH(badge, json_items(badges)) {
    p->badges[p->nbadges++] = (SessionBadge){
      .icon = str_dup(json_get_or(badge, "icon", nullptr)),
      .description = str_dup(json_get_or(badge, "description", nullptr)),
    };
  }
}

static void put_mutuals(Session *s, Profile *p, const cima_json *d) {
  const cima_json *guilds = json_get(d, "mutual_guilds");
  const cima_json *friends = json_get(d, "mutual_friends");
  size_t n = json_type(guilds) == CIMA_JSON_ARRAY ? json_len(guilds) : 0;

  p->mutual_guilds = n ? calloc(n, sizeof *p->mutual_guilds) : nullptr;

  CIMA_EACH(guild, json_items(guilds)) {
    Snowflake id = snowflake_of(json_get(guild, "id"));

    if (id != 0) {
      p->mutual_guilds[p->nmutual_guilds++] = (ProfileGuild){
        .id = id,
        .nick = str_dup(json_get_or(guild, "nick", nullptr)),
      };
    }
  }

  n = json_type(friends) == CIMA_JSON_ARRAY ? json_len(friends) : 0;
  p->mutual_friends = n ? calloc(n, sizeof *p->mutual_friends) : nullptr;

  CIMA_EACH(user, json_items(friends)) {
    const User *u = store_put_user(s->store, user);

    if (u != nullptr) {
      p->mutual_friends[p->nmutual_friends++] = u->id;
    }
  }
}

static void put_connections(Profile *p, const cima_json *accounts) {
  size_t n = json_type(accounts) == CIMA_JSON_ARRAY ? json_len(accounts) : 0;

  p->connections = n ? calloc(n, sizeof *p->connections) : nullptr;

  CIMA_EACH(account, json_items(accounts)) {
    const char *type = json_get_or(account, "type", nullptr);
    const char *name = json_get_or(account, "name", nullptr);

    if (type != nullptr && name != nullptr) {
      p->connections[p->nconnections++] = (ProfileConnection){
        .type = str_dup(type),
        .name = str_dup(name),
        .verified = json_get_coerce(account, "verified", false),
      };
    }
  }
}

static void fill_profile(Session *s, Profile *p, const cima_json *d) {
  const cima_json *mine = json_get(d, "user_profile");
  const cima_json *theirs = json_get(d, "guild_member_profile");
  const cima_json *member = json_get(d, "guild_member");
  const cima_json *accent = json_get(mine, "accent_color");
  Guild *guild = store_guild(s->store, p->guild_id);

  empty_profile(p);
  store_put_user(s->store, json_get(d, "user"));

  if (guild != nullptr && json_type(member) == CIMA_JSON_OBJECT) {
    store_put_member(s->store, guild, member, p->user_id);
  }

  p->bio = str_dup(json_get_or(mine, "bio", nullptr));
  p->pronouns = str_dup(json_get_or(mine, "pronouns", nullptr));
  p->banner = str_dup(json_get_or(mine, "banner", nullptr));
  p->has_accent = cima_json_is_number(accent);
  p->accent_color = (uint32_t)json_coerce(accent, (uint64_t)0);
  put_theme(json_get(mine, "theme_colors"), p->theme, &p->has_theme);

  p->guild_bio = str_dup(json_get_or(theirs, "bio", nullptr));
  p->guild_pronouns = str_dup(json_get_or(theirs, "pronouns", nullptr));
  p->guild_banner = str_dup(json_get_or(theirs, "banner", nullptr));
  put_theme(json_get(theirs, "theme_colors"), p->guild_theme,
            &p->has_guild_theme);

  put_badges(p, json_get(d, "badges"));
  put_badges(p, json_get(d, "guild_badges"));
  p->legacy_username = str_dup(json_get_or(d, "legacy_username", nullptr));
  put_mutuals(s, p, d);
  p->mutual_friends_count =
    (uint32_t)json_get_coerce(d, "mutual_friends_count",
                              (uint64_t)p->nmutual_friends);
  p->premium_type = (uint8_t)json_get_coerce(d, "premium_type", (uint64_t)0);
  put_connections(p, json_get(d, "connected_accounts"));
  p->premium_since =
    iso8601_parse_ms(json_get_or(d, "premium_since", nullptr));
  p->premium_guild_since =
    iso8601_parse_ms(json_get_or(d, "premium_guild_since", nullptr));

  p->loaded = true;
}

static void on_user_profile(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Profile *p = find_profile(s, call->id, call->channel_id);

  call_end(call);

  if (p == nullptr) {
    return;
  }

  p->loading = false;
  p->at = store_now(s->store);

  if (res->ok && json_type(res->json) == CIMA_JSON_OBJECT) {
    fill_profile(s, p, res->json);
    p->failed = false;
  }
  else {
    p->failed = !p->loaded;
  }

  changed(s);
}

// a note is the same seen from any guild.
static void spread_note(Session *s, Snowflake user_id, const char *note) {
  for (size_t i = 0; i < s->nprofiles; i++) {
    Profile *p = s->profiles[i];

    if (p->user_id == user_id) {
      str_set(&p->note, note != nullptr && *note ? note : nullptr);
      p->note_loaded = true;
    }
  }
}

static void on_note(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake user_id = call->id;

  call_end(call);

  // no note is a 404.
  if (res->ok || res->status == 404) {
    spread_note(s, user_id, json_get_or(res->json, "note", nullptr));
    changed(s);
  }
}

// what the store knows, for a session that cannot ask.
static void profile_of_store(Session *s, Profile *p) {
  const User *u = store_user(s->store, p->user_id);

  empty_profile(p);
  p->loaded = true;
  p->note_loaded = true;
  p->at = store_now(s->store);

  if (u != nullptr) {
    p->bio = str_dup(u->bio);
    p->pronouns = str_dup(u->pronouns);
    p->banner = str_dup(u->banner);
    p->has_accent = u->has_accent;
    p->accent_color = u->accent_color;
  }
}

const Profile *session_profile(Session *s, Snowflake user_id,
                               Snowflake guild_id, bool full) {
  Profile *p = find_profile(s, user_id, guild_id);
  bool fresh = p == nullptr;

  if (p == nullptr) {
    p = add_profile(s, user_id, guild_id);
  }

  // the note of another guild's profile of them serves.
  if (fresh) {
    for (size_t i = 0; i < s->nprofiles; i++) {
      const Profile *other = s->profiles[i];

      if (other != p && other->user_id == user_id && other->note_loaded) {
        p->note = str_dup(other->note);
        p->note_loaded = true;
        break;
      }
    }
  }

  if (!online(s)) {
    if (!p->loaded) {
      profile_of_store(s, p);
    }

    return p;
  }

  double now = store_now(s->store);
  double age = now - p->at;
  bool stale = p->loaded ? age > PROFILE_FRESH || (full && !p->full)
                         : !p->failed || age > PROFILE_RETRY;

  if (p->loading || !stale) {
    return p;
  }

  char path[256];
  char guild[48] = "";
  const User *u = store_user(s->store, user_id);
  bool self = user_id == s->store->self_id;
  bool bot = u != nullptr && u->bot;

  if (guild_id != 0) {
    snprintf(guild, sizeof guild, "&guild_id=%llu",
             (unsigned long long)guild_id);
  }

  // as the client asks for its popout and its full profile: the popout
  // wants a few mutual friends, the full one their count.
  snprintf(path, sizeof path,
           "/users/%llu/profile?type=%s&with_mutual_guilds=%s"
           "&with_mutual_friends=%s&with_mutual_friends_count=%s%s",
           (unsigned long long)user_id, full ? "modal" : "popout",
           self && !full ? "false" : "true",
           !full && !bot && !self ? "true" : "false",
           full && !bot ? "true" : "false", guild);

  ApiRequest req = {.method = "GET",
                    .path = path,
                    .route = "/users/:id/profile"};

  p->loading = true;
  p->full = p->full || full;
  api_request(s->api, &req, on_user_profile, call_new(s, guild_id, user_id));

  if (!p->note_loaded && user_id != s->store->self_id) {
    snprintf(path, sizeof path, "/users/@me/notes/%llu",
             (unsigned long long)user_id);

    ApiRequest note = {.method = "GET",
                       .path = path,
                       .route = "/users/@me/notes/:id"};

    api_request(s->api, &note, on_note, call_new(s, 0, user_id));
  }

  return p;
}

static void on_mutual_friends(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Profile *p = find_profile(s, call->id, call->channel_id);

  call_end(call);

  if (p == nullptr) {
    return;
  }

  p->friends_loading = false;

  if (!res->ok) {
    return;
  }

  size_t n = json_type(res->json) == CIMA_JSON_ARRAY ? json_len(res->json) : 0;

  free(p->mutual_friends);
  p->mutual_friends = n ? calloc(n, sizeof *p->mutual_friends) : nullptr;
  p->nmutual_friends = 0;

  CIMA_EACH(user, json_items(res->json)) {
    const User *u = store_put_user(s->store, user);

    if (u != nullptr) {
      p->mutual_friends[p->nmutual_friends++] = u->id;
    }
  }

  p->mutual_friends_count = (uint32_t)p->nmutual_friends;
  p->friends_loaded = true;
  changed(s);
}

void session_mutual_friends(Session *s, Snowflake user_id,
                            Snowflake guild_id) {
  Profile *p = find_profile(s, user_id, guild_id);

  if (p == nullptr || p->friends_loading || p->friends_loaded || !online(s)) {
    return;
  }

  char path[64];

  snprintf(path, sizeof path, "/users/%llu/relationships",
           (unsigned long long)user_id);

  ApiRequest req = {.method = "GET",
                    .path = path,
                    .route = "/users/:id/relationships"};

  p->friends_loading = true;
  api_request(s->api, &req, on_mutual_friends, call_new(s, guild_id, user_id));
}

void session_put_note(Session *s, Snowflake user_id, const char *note) {
  spread_note(s, user_id, note);
  changed(s);
}

void session_set_note(Session *s, Snowflake user_id, const char *note) {
  session_put_note(s, user_id, note);

  if (!online(s)) {
    return;
  }

  char path[64];
  cima_json_writer w = {};

  snprintf(path, sizeof path, "/users/@me/notes/%llu",
           (unsigned long long)user_id);
  json_begin_object(&w);
  json_field(&w, "note", note != nullptr ? note : "");
  json_end_object(&w);

  ApiRequest req = {.method = "PUT",
                    .path = path,
                    .route = "/users/@me/notes/:id",
                    .body = cima_json_writer_text(&w)};

  api_request(s->api, &req, nullptr, nullptr);
  cima_json_writer_dispose(&w);
}

/* ========================================================================== */
/* forums.                                                                    */
/* ========================================================================== */

constexpr uint32_t FORUM_PAGE = 25;
constexpr size_t STARTERS_BATCH = 10;
constexpr size_t TITLE_MAX = 100;

static void clear_query(ForumQuery *q) {
  free(q->tags);
  free(q->query);
  *q = (ForumQuery){};
}

static void clear_forum(Session *s) {
  clear_query(&s->forum.query);
  free(s->forum.posts);
  free(s->forum.error);
  s->forum = (SessionForum){.generation = s->forum.generation + 1};

  free(s->starters_asked);
  s->starters_asked = nullptr;
  s->nstarters_asked = 0;
  s->starters_cap = 0;

  free(s->post_error);
  s->post_error = nullptr;
  s->post_creating = false;
  s->post_created = 0;
}

static bool same_query(const ForumQuery *a, const ForumQuery *b) {
  const char *qa = a->query != nullptr ? a->query : "";
  const char *qb = b->query != nullptr ? b->query : "";

  if (a->sort != b->sort || a->match_all != b->match_all ||
      a->ntags != b->ntags || strcmp(qa, qb) != 0) {
    return false;
  }

  for (size_t i = 0; i < a->ntags; i++) {
    if (a->tags[i] != b->tags[i]) {
      return false;
    }
  }

  return true;
}

static void copy_query(ForumQuery *to, const ForumQuery *from) {
  clear_query(to);
  to->sort = from->sort;
  to->match_all = from->match_all;
  to->query = from->query != nullptr && from->query[0] != '\0'
                ? str_dup(from->query)
                : nullptr;

  if (from->ntags != 0) {
    to->tags = malloc(from->ntags * sizeof *to->tags);

    if (to->tags == nullptr) {
      cilo_oom(from->ntags * sizeof *to->tags);
    }

    memcpy(to->tags, from->tags, from->ntags * sizeof *to->tags);
    to->ntags = from->ntags;
  }
}

static void add_post(SessionForum *f, Snowflake id) {
  for (size_t i = 0; i < f->nposts; i++) {
    if (f->posts[i] == id) {
      return;
    }
  }

  Snowflake *grown = realloc(f->posts, (f->nposts + 1) * sizeof *f->posts);

  if (grown == nullptr) {
    cilo_oom((f->nposts + 1) * sizeof *f->posts);
  }

  f->posts = grown;
  f->posts[f->nposts++] = id;
}

// the members of threads the answer carries are the current user's.
static void put_thread_members(Session *s, const cima_json *members) {
  CIMA_EACH(member, json_items(members)) {
    Snowflake id = snowflake_of(json_get(member, "id"));
    Channel *c = store_channel(s->store, id);

    if (c == nullptr || c->thread_joined) {
      continue;
    }

    store_set_thread_joined(s->store, id, true);

    int64_t joined =
      iso8601_parse_ms(json_get_or(member, "join_timestamp", nullptr));

    if (joined != 0) {
      c->thread_joined_at = joined;
    }
  }
}

static void put_first_messages(Session *s, const cima_json *messages) {
  CIMA_EACH(message, json_items(messages)) {
    Channel *post = store_channel(
      s->store, snowflake_of(json_get(message, "channel_id")));

    if (post != nullptr) {
      store_put_starter(s->store, post, message);
    }
  }
}

static void on_forum_page(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  SessionForum *f = &s->forum;
  bool current =
    call->id == f->generation && call->channel_id == f->channel_id;

  call_end(call);

  // the list was asked again since.
  if (!current) {
    return;
  }

  f->loading = false;

  if (!res->ok) {
    str_set(&f->error, res->error.message);
    f->has_more = false;
    changed(s);
    return;
  }

  const Channel *forum = store_channel(s->store, f->channel_id);
  Snowflake guild_id = forum != nullptr ? forum->guild_id : 0;

  CIMA_EACH(thread, json_items(json_get(res->json, "threads"))) {
    Channel *c = store_put_channel(s->store, thread, guild_id);

    if (c != nullptr) {
      add_post(f, c->id);
    }
  }

  put_thread_members(s, json_get(res->json, "members"));
  put_first_messages(s, json_get(res->json, "first_messages"));

  f->has_more = json_get_coerce(res->json, "has_more", false);
  f->total = (uint32_t)json_get_coerce(res->json, "total_results",
                                       (int64_t)f->nposts);
  str_set(&f->error, nullptr);
  changed(s);
}

// the posts discord finds for the query, from the `offset`th on.
static void fetch_forum_page(Session *s) {
  SessionForum *f = &s->forum;
  const ForumQuery *q = &f->query;
  char *path = nullptr;
  size_t size = 0;
  FILE *out = open_memstream(&path, &size);

  if (out == nullptr) {
    return;
  }

  // what the web client asks: the archived posts, the active ones coming
  // through the gateway, unless words are looked for in every post. both
  // are latest first whatever the order shown.
  fprintf(out, "/channels/%" PRIu64 "/threads/search?", f->channel_id);

  if (q->query != nullptr) {
    char *words = url_encode(q->query);

    fprintf(out, "name=%s&", words);
    free(words);
  }
  else {
    fprintf(out, "archived=true&");
  }

  fprintf(out,
          "sort_by=last_message_time&sort_order=desc&limit=%u"
          "&tag_setting=%s&offset=%zu",
          FORUM_PAGE, q->match_all ? "match_all" : "match_some", f->nposts);

  for (size_t i = 0; i < q->ntags; i++) {
    fprintf(out, i == 0 ? "&tag=%" PRIu64 : ",%" PRIu64, q->tags[i]);
  }

  fclose(out);

  ApiRequest req = {
    .method = "GET",
    .path = path,
    .route = "/channels/:id/threads/search",
  };

  f->loading = true;
  api_request(s->api, &req, on_forum_page,
              call_new(s, f->channel_id, f->generation));
  free(path);
}

// how many messages came in the posts opened before that have news, for
// the counts of the list.
static void request_forum_unreads(Session *s, Snowflake forum_id) {
  const Channel *forum = store_channel(s->store, forum_id);

  if (forum == nullptr || !gateway_ready(s)) {
    return;
  }

  size_t count = 0, asked = 0;
  Channel *const *posts = store_forum_posts(s->store, forum, &count);
  GatewayForumAck acks[180];

  for (size_t i = 0; i < count && asked < countof(acks); i++) {
    const Channel *post = posts[i];
    Snowflake acked = store_last_acked(s->store, post->id);

    if (!post->thread_archived && store_has_read_state(s->store, post->id) &&
        acked < post->last_message_id) {
      acks[asked++] = (GatewayForumAck){post->id, acked};
    }
  }

  gateway_request_forum_unreads(s->gateway, forum->guild_id, forum_id, acks,
                                asked);
}

void session_forum_query(Session *s, Snowflake forum_id,
                         const ForumQuery *query) {
  SessionForum *f = &s->forum;

  if (f->channel_id == forum_id && same_query(&f->query, query)) {
    return;
  }

  free(f->posts);
  f->posts = nullptr;
  f->nposts = 0;
  f->total = 0;
  f->generation++;
  f->loading = false;
  f->channel_id = forum_id;
  str_set(&f->error, nullptr);
  copy_query(&f->query, query);

  // offline, the posts the store holds are all there is.
  f->has_more = online(s) && forum_id != 0;

  if (f->has_more) {
    fetch_forum_page(s);
  }

  request_forum_unreads(s, forum_id);
  changed(s);
}

void session_forum_more(Session *s) {
  SessionForum *f = &s->forum;

  if (f->has_more && !f->loading && online(s)) {
    fetch_forum_page(s);
    changed(s);
  }
}

static bool starter_asked(Session *s, Snowflake id) {
  for (size_t i = 0; i < s->nstarters_asked; i++) {
    if (s->starters_asked[i] == id) {
      return true;
    }
  }

  return false;
}

static void on_starters(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (!res->ok) {
    return;
  }

  // keyed by the post's id, each with its first message and its owner.
  const cima_json *threads = json_get(res->json, "threads");

  CIMA_EACH(entry, json_members(threads)) {
    Channel *post = store_channel(s->store, snowflake_parse(entry->key));
    const cima_json *first = json_get(&entry->value, "first_message");

    if (post == nullptr) {
      continue;
    }

    const cima_json *owner = json_get(&entry->value, "owner");
    Guild *g = store_guild(s->store, post->guild_id);

    if (g != nullptr && json_type(owner) == CIMA_JSON_OBJECT) {
      store_put_member(s->store, g, owner, post->owner_id);
    }

    store_put_starter(s->store, post, first);
  }

  changed(s);
}

void session_fetch_starters(Session *s, Snowflake forum_id,
                            const Snowflake *post_ids, size_t count) {
  if (!online(s)) {
    return;
  }

  Snowflake batch[STARTERS_BATCH];
  size_t n = 0;

  for (size_t i = 0; i <= count; i++) {
    if (i < count) {
      const Channel *post = store_channel(s->store, post_ids[i]);

      if (post == nullptr || post->starter_known ||
          starter_asked(s, post->id)) {
        continue;
      }

      if (s->nstarters_asked == s->starters_cap) {
        s->starters_cap = s->starters_cap != 0 ? s->starters_cap * 2 : 64;
        s->starters_asked = realloc(
          s->starters_asked, s->starters_cap * sizeof *s->starters_asked);

        if (s->starters_asked == nullptr) {
          cilo_oom(s->starters_cap * sizeof *s->starters_asked);
        }
      }

      s->starters_asked[s->nstarters_asked++] = post->id;
      batch[n++] = post->id;
    }

    if (n == 0 || (n < STARTERS_BATCH && i < count)) {
      continue;
    }

    cima_json_writer w = {};
    char path[96];

    snprintf(path, sizeof path, "/channels/%" PRIu64 "/post-data", forum_id);
    json_begin_object(&w);
    json_key(&w, "thread_ids");
    json_begin_array(&w);

    for (size_t j = 0; j < n; j++) {
      snowflake_write(&w, batch[j]);
    }

    json_end_array(&w);
    json_end_object(&w);

    ApiRequest req = {
      .method = "POST",
      .path = path,
      .route = "/channels/:id/post-data",
      .body = cima_json_writer_text(&w),
    };

    api_request(s->api, &req, on_starters, call_new(s, forum_id, 0));
    cima_json_writer_dispose(&w);
    n = 0;
  }
}

// offline, a post is made up the way discord would tell of it.
static void echo_post(Session *s, const Channel *forum, const char *title,
                      const char *content, const Snowflake *tags,
                      size_t ntags) {
  int64_t now = (int64_t)(store_now(s->store) * 1000);
  Snowflake id = snowflake_from_time_ms(now) | (++s->nonce_counter & 0xfff);
  char timestamp[40];
  cima_json_writer w = {};

  iso_now(s->store, timestamp);

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, id);
  json_key(&w, "guild_id"), snowflake_write(&w, forum->guild_id);
  json_key(&w, "parent_id"), snowflake_write(&w, forum->id);
  json_key(&w, "owner_id"), snowflake_write(&w, s->store->self_id);
  json_field(&w, "type", (int64_t)CHANNEL_PUBLIC_THREAD);
  json_field(&w, "name", title);
  json_key(&w, "applied_tags");
  json_begin_array(&w);

  for (size_t i = 0; i < ntags; i++) {
    snowflake_write(&w, tags[i]);
  }

  json_end_array(&w);
  json_key(&w, "thread_metadata");
  json_begin_object(&w);
  json_field(&w, "archived", false);
  json_field(&w, "locked", false);
  json_field(&w, "create_timestamp", timestamp);
  json_end_object(&w);
  json_key(&w, "member");
  json_begin_object(&w);
  json_field(&w, "join_timestamp", timestamp);
  json_end_object(&w);
  json_end_object(&w);
  apply_local(s, "THREAD_CREATE", &w);

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, id);
  json_key(&w, "channel_id"), snowflake_write(&w, id);
  json_field(&w, "content", content);
  json_field(&w, "timestamp", timestamp);
  json_key(&w, "author");
  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, s->store->self_id);
  json_end_object(&w);
  json_end_object(&w);
  apply_local(s, "MESSAGE_CREATE", &w);

  s->post_created = id;
}

static void on_post_created(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  const Channel *forum = store_channel(s->store, call->channel_id);

  call_end(call);
  s->post_creating = false;

  // the answer is the thread, its first message under `message`.
  Channel *post =
    res->ok ? store_put_channel(s->store, res->json,
                                forum != nullptr ? forum->guild_id : 0)
            : nullptr;

  if (post != nullptr) {
    store_set_thread_joined(s->store, post->id, true);

    if (json_type(json_get(res->json, "message")) == CIMA_JSON_OBJECT) {
      store_create_message(s->store, json_get(res->json, "message"));
    }

    s->post_created = post->id;
  }
  else {
    str_set(&s->post_error,
            res->ok ? "Discord sent no post." : res->error.message);
  }

  changed(s);
}

void session_create_post(Session *s, Snowflake forum_id, const char *title,
                         const char *content, const Snowflake *tags,
                         size_t ntags) {
  const Channel *forum = store_channel(s->store, forum_id);

  if (forum == nullptr || s->post_creating || title == nullptr ||
      title[0] == '\0' || content == nullptr || content[0] == '\0') {
    return;
  }

  str_set(&s->post_error, nullptr);
  s->post_created = 0;

  if (s->offline) {
    echo_post(s, forum, title, content, tags, ntags);
    changed(s);
    return;
  }

  if (!online(s)) {
    str_set(&s->post_error, "Posting needs a connection.");
    changed(s);
    return;
  }

  char path[96];
  cima_json_writer w = {};

  char name[TITLE_MAX * 4 + 1];

  // a title is a hundred characters at most, cut where one begins.
  snprintf(name, sizeof name, "%s", title);

  for (size_t i = 0, chars = 0; name[i] != '\0'; i++) {
    if ((name[i] & 0xc0) != 0x80 && chars++ == TITLE_MAX) {
      name[i] = '\0';
      break;
    }
  }

  snprintf(path, sizeof path,
           "/channels/%" PRIu64 "/threads?use_nested_fields=true", forum_id);

  json_begin_object(&w);
  json_field(&w, "name", name);
  json_key(&w, "applied_tags");
  json_begin_array(&w);

  for (size_t i = 0; i < ntags; i++) {
    snowflake_write(&w, tags[i]);
  }

  json_end_array(&w);
  json_key(&w, "message");
  json_begin_object(&w);
  json_field(&w, "content", content);
  json_end_object(&w);
  json_end_object(&w);

  ApiRequest req = {
    .method = "POST",
    .path = path,
    .route = "/channels/:id/threads",
    .body = cima_json_writer_text(&w),
  };

  s->post_creating = true;
  api_request(s->api, &req, on_post_created, call_new(s, forum_id, 0));
  cima_json_writer_dispose(&w);
  changed(s);
}

void session_follow(Session *s, Snowflake thread_id, bool follow) {
  Channel *c = store_channel(s->store, thread_id);

  if (c == nullptr || !channel_is_thread(c) || c->thread_joined == follow) {
    return;
  }

  // the gateway tells of it too, which changes nothing then.
  store_set_thread_joined(s->store, thread_id, follow);

  if (online(s)) {
    char path[96];

    snprintf(path, sizeof path,
             "/channels/%" PRIu64
             "/thread-members/@me?location=Forum%%20Toolbar",
             thread_id);

    ApiRequest req = {
      .method = follow ? "POST" : "DELETE",
      .path = path,
      .route = "/channels/:id/thread-members/@me",
    };

    api_request(s->api, &req, nullptr, nullptr);
  }

  changed(s);
}

void session_set_post_tags(Session *s, Snowflake post_id, const Snowflake *tags,
                           size_t ntags) {
  Channel *post = store_channel(s->store, post_id);

  if (post == nullptr || !channel_is_thread(post)) {
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, post_id);
  json_key(&w, "applied_tags");
  json_begin_array(&w);

  for (size_t i = 0; i < ntags; i++) {
    snowflake_write(&w, tags[i]);
  }

  json_end_array(&w);
  json_end_object(&w);

  // shown at once, discord's THREAD_UPDATE following.
  if (online(s)) {
    char path[96];

    snprintf(path, sizeof path, "/channels/%" PRIu64, post_id);

    ApiRequest req = {
      .method = "PATCH",
      .path = path,
      .route = "/channels/:id",
      .body = cima_json_writer_text(&w),
    };

    api_request(s->api, &req, nullptr, nullptr);
  }

  apply_local(s, "THREAD_UPDATE", &w);
  changed(s);
}

void session_update_post(Session *s, Snowflake post_id, bool pinned,
                         bool archived, bool locked) {
  Channel *post = store_channel(s->store, post_id);

  if (post == nullptr || !channel_is_thread(post)) {
    return;
  }

  // archiving takes the pin away, and a pinned post is never archived.
  uint32_t flags = pinned && !archived ? post->flags | CHANNEL_FLAG_PINNED
                                       : post->flags & ~CHANNEL_FLAG_PINNED;
  cima_json_writer w = {};

  json_begin_object(&w);
  json_key(&w, "id"), snowflake_write(&w, post_id);
  json_field(&w, "flags", (int64_t)flags);
  json_key(&w, "thread_metadata");
  json_begin_object(&w);
  json_field(&w, "archived", archived);
  json_field(&w, "locked", locked);
  json_end_object(&w);
  json_end_object(&w);

  if (online(s)) {
    char path[96];
    cima_json_writer body = {};

    snprintf(path, sizeof path, "/channels/%" PRIu64, post_id);
    json_begin_object(&body);

    if (flags != post->flags) {
      json_field(&body, "flags", (int64_t)flags);
    }

    json_field(&body, "archived", archived);
    json_field(&body, "locked", locked);
    json_end_object(&body);

    ApiRequest req = {
      .method = "PATCH",
      .path = path,
      .route = "/channels/:id",
      .body = cima_json_writer_text(&body),
    };

    api_request(s->api, &req, nullptr, nullptr);
    cima_json_writer_dispose(&body);
  }

  apply_local(s, "THREAD_UPDATE", &w);
  changed(s);
}

/* ========================================================================== */
/* directories.                                                               */
/* ========================================================================== */

static void clear_directory_search(Session *s) {
  SessionDirectorySearch *search = &s->directory_search;

  free(search->query);
  free(search->results);
  free(search->error);
  *search = (SessionDirectorySearch){.asked = search->asked + 1,
                                     .category = -1};
}

static void clear_schools(SessionHubJoin *hub);

static void clear_directories(Session *s) {
  clear_directory_search(s);
  clear_schools(&s->hub);
  free(s->hub.email);
  free(s->hub.error);
  s->hub = (SessionHubJoin){};
  s->leaving_guild = 0;
  s->directory_checking = false;
  str_set(&s->directory_join_error, nullptr);
  str_set(&s->directory_error, nullptr);
  s->directory_joining = 0;
  s->directory_saving = false;
  s->directory_saved = 0;
  s->directory_pending_channel = 0;
  str_set(&s->directory_pending_description, nullptr);
}

static void on_directory_entries(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Directory *d = store_open_directory(s->store, call->channel_id);

  call_end(call);
  d->loading = false;

  // a listing that failed shows empty until the next connection asks again,
  // rather than being asked for on every frame.
  if (res->ok) {
    store_put_directory(s->store, d->channel_id, res->json);
  }
  else {
    d->loaded = true;
  }

  d->version++;
  changed(s);
}

static void on_directory_counts(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (res->ok) {
    store_put_directory_counts(s->store, call->channel_id, res->json);
  }

  changed(s);
}

void session_directory_open(Session *s, Snowflake channel_id) {
  const Channel *c = store_channel(s->store, channel_id);

  if (c == nullptr || c->type != CHANNEL_GUILD_DIRECTORY) {
    return;
  }

  Directory *d = store_open_directory(s->store, channel_id);

  if (d->loaded || d->loading) {
    return;
  }

  // without a connection, what the fixture announced is all there is.
  if (!online(s)) {
    memset(d->counts, 0, sizeof d->counts);

    for (size_t i = 0; i < d->nentries; i++) {
      d->counts[d->entries[i]->category]++;
    }

    d->counted = true;
    d->loaded = true;
    d->version++;
    changed(s);
    return;
  }

  char path[96], counts[112];

  snprintf(path, sizeof path, "/channels/%" PRIu64 "/directory-entries",
           channel_id);
  snprintf(counts, sizeof counts, "%s/counts", path);

  d->loading = true;
  api_request(s->api,
              &(ApiRequest){.method = "GET",
                            .path = path,
                            .route = "/channels/:id/directory-entries"},
              on_directory_entries, call_new(s, channel_id, 0));
  api_request(s->api,
              &(ApiRequest){.method = "GET",
                            .path = counts,
                            .route = "/channels/:id/directory-entries/counts"},
              on_directory_counts, call_new(s, channel_id, 0));
  changed(s);
}

static void on_directory_search(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  SessionDirectorySearch *search = &s->directory_search;

  call_end(call);

  // another search was made since.
  if (call->id != search->asked) {
    return;
  }

  search->loading = false;
  search->done = true;

  if (!res->ok || json_type(res->json) != CIMA_JSON_ARRAY) {
    str_set(&search->error, res->ok ? "Discord sent no results."
                                    : res->error.message);
    changed(s);
    return;
  }

  search->results = calloc(json_len(res->json) + 1, sizeof *search->results);

  if (search->results == nullptr) {
    cilo_oom(json_len(res->json) * sizeof *search->results);
  }

  CIMA_EACH(entry, json_items(res->json)) {
    const DirectoryEntry *e = store_put_directory_entry(s->store, entry);

    if (e != nullptr) {
      search->results[search->nresults++] = e->entity_id;
    }
  }

  changed(s);
}

void session_directory_search(Session *s, Snowflake channel_id,
                              const char *query, int category) {
  SessionDirectorySearch *search = &s->directory_search;
  size_t len = query != nullptr ? strlen(query) : 0;

  clear_directory_search(s);

  if (len == 0) {
    changed(s);
    return;
  }

  search->channel_id = channel_id;
  search->query = str_dup_n(query, len > 100 ? 100 : len);
  search->category =
    category >= 0 && category < (int)DIRECTORY_CATEGORIES ? (int8_t)category
                                                          : -1;

  // without a connection, the entries known stand for discord's search.
  if (!online(s)) {
    const Directory *d = store_directory(s->store, channel_id);

    search->done = true;
    search->results = calloc(d ? d->nentries + 1 : 1, sizeof *search->results);

    if (search->results == nullptr) {
      cilo_oom(sizeof *search->results);
    }

    for (size_t i = 0; d != nullptr && i < d->nentries; i++) {
      const DirectoryEntry *e = d->entries[i];
      bool named = e->name && strcasestr(e->name, search->query);
      bool told = e->description && strcasestr(e->description, search->query);

      if (named || told) {
        search->results[search->nresults++] = e->entity_id;
      }
    }

    changed(s);
    return;
  }

  char *words = url_encode(search->query);
  char path[512];
  int n = snprintf(path, sizeof path,
                   "/channels/%" PRIu64 "/directory-entries/search?query=%s",
                   channel_id, words);

  free(words);

  if (search->category >= 0 && n > 0 && (size_t)n < sizeof path) {
    snprintf(path + n, sizeof path - (size_t)n, "&category_id=%d",
             search->category);
  }

  search->loading = true;
  api_request(s->api,
              &(ApiRequest){.method = "GET",
                            .path = path,
                            .route = "/channels/:id/directory-entries/search"},
              on_directory_search, call_new(s, channel_id, search->asked));
  changed(s);
}

static void on_directory_join(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (s->directory_joining != call->id) {
    return;
  }

  // the guild itself comes through the gateway's GUILD_CREATE.
  if (!res->ok) {
    s->directory_joining = 0;
    str_set(&s->directory_join_error, res->error.message);
  }

  changed(s);
}

void session_directory_join(Session *s, Snowflake channel_id,
                            Snowflake guild_id) {
  if (guild_id == 0 || store_guild(s->store, guild_id) != nullptr ||
      s->directory_joining == guild_id) {
    return;
  }

  str_set(&s->directory_join_error, nullptr);

  if (!online(s)) {
    str_set(&s->directory_join_error, "Joining needs a connection.");
    changed(s);
    return;
  }

  char path[160];

  snprintf(path, sizeof path,
           "/guilds/%" PRIu64 "/members/@me?lurker=false&from_directory=true",
           guild_id);

  s->directory_joining = guild_id;
  api_request(s->api,
              &(ApiRequest){.method = "PUT",
                            .path = path,
                            .route = "/guilds/:id/members/@me",
                            .body = "{}",
                            .context =
                              "{\"source\":\"Directory Channel Entry\"}"},
              on_directory_join, call_new(s, channel_id, guild_id));
  changed(s);
}

static void on_directory_saved(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);
  s->directory_saving = false;

  if (!res->ok) {
    str_set(&s->directory_error, res->error.message);
    changed(s);
    return;
  }

  // the gateway tells the same, which may come first or not at all.
  if (json_type(res->json) == CIMA_JSON_OBJECT) {
    store_put_directory_entry(s->store, res->json);
  }
  else {
    store_remove_directory_entry(s->store, call->channel_id, call->id);
  }

  s->directory_saved = call->id;
  changed(s);
}

// an entry as discord would answer it, applied to the store of a session
// without a connection.
static void put_local_entry(Session *s, Snowflake channel_id,
                            Snowflake entity_id, const char *body) {
  cilo_arena arena = {};
  const cima_json *fields = json_parse(&arena, body, strlen(body), nullptr);
  const Guild *g = store_guild(s->store, entity_id);
  cima_json_writer w = {};

  json_begin_object(&w);
  json_key(&w, "directory_channel_id"), snowflake_write(&w, channel_id);
  json_key(&w, "entity_id"), snowflake_write(&w, entity_id);
  json_key(&w, "author_id"), snowflake_write(&w, s->store->self_id);
  json_field(&w, "type", (int)DIRECTORY_ENTRY_GUILD);
  json_field(&w, "description",
             json_get_or(fields, "description", (const char *)nullptr));
  json_field(&w, "primary_category_id",
             (int)json_get_coerce(fields, "primary_category_id", (int64_t)0));

  if (g != nullptr) {
    json_key(&w, "guild");
    json_begin_object(&w);
    json_field(&w, "name", g->name);
    json_field(&w, "icon", g->icon);
    json_field(&w, "splash", g->splash);
    json_field(&w, "approximate_member_count", (int64_t)g->member_count);
    json_end_object(&w);
  }

  json_end_object(&w);

  const char *text = cima_json_writer_text(&w);
  const cima_json *entry = json_parse(&arena, text, strlen(text), nullptr);

  store_put_directory_entry(s->store, entry);
  cima_json_writer_dispose(&w);
  cilo_arena_drop(&arena);
}

static void save_entry(Session *s, const char *method, Snowflake channel_id,
                       Snowflake entity_id, const char *body) {
  if (s->directory_saving || entity_id == 0) {
    return;
  }

  str_set(&s->directory_error, nullptr);
  s->directory_saved = 0;

  // the client takes an entry out before discord answers.
  if (body == nullptr) {
    store_remove_directory_entry(s->store, channel_id, entity_id);
  }

  if (!online(s)) {
    if (body != nullptr) {
      put_local_entry(s, channel_id, entity_id, body);
    }

    s->directory_saved = entity_id;
    changed(s);
    return;
  }

  char path[128];

  snprintf(path, sizeof path,
           "/channels/%" PRIu64 "/directory-entry/%" PRIu64, channel_id,
           entity_id);

  s->directory_saving = true;
  api_request(s->api,
              &(ApiRequest){.method = method,
                            .path = path,
                            .route = "/channels/:id/directory-entry",
                            .body = body},
              on_directory_saved, call_new(s, channel_id, entity_id));
  changed(s);
}

static char *entry_body(DirectoryCategory category, const char *description) {
  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "description", description != nullptr ? description : "");
  json_field(&w, "primary_category_id", (int)category);
  json_end_object(&w);

  char *body = str_dup(cima_json_writer_text(&w));

  cima_json_writer_dispose(&w);
  return body;
}

void session_directory_add(Session *s, Snowflake channel_id,
                           Snowflake guild_id, DirectoryCategory category,
                           const char *description) {
  char *body = entry_body(category, description);

  save_entry(s, "POST", channel_id, guild_id, body);
  free(body);
}

void session_directory_edit(Session *s, Snowflake channel_id,
                            Snowflake entity_id, DirectoryCategory category,
                            const char *description) {
  char *body = entry_body(category, description);

  save_entry(s, "PATCH", channel_id, entity_id, body);
  free(body);
}

void session_directory_remove(Session *s, Snowflake channel_id,
                              Snowflake entity_id) {
  save_entry(s, "DELETE", channel_id, entity_id, nullptr);
}

static void on_directory_check(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);
  s->directory_checking = false;

  if (res->ok) {
    CIMA_EACH(entry, json_items(res->json)) {
      store_put_directory_entry(s->store, entry);
    }
  }

  changed(s);
}

void session_directory_check(Session *s, Snowflake channel_id,
                             const Snowflake *guild_ids, size_t count) {
  if (count == 0 || !online(s) || s->directory_checking) {
    return;
  }

  // up to a hundred ids of twenty digits, each behind its key.
  constexpr size_t MOST = 100;
  size_t cap = 96 + (count < MOST ? count : MOST) * 34;
  char *path = malloc(cap);

  if (path == nullptr) {
    cilo_oom(cap);
  }

  size_t n = (size_t)snprintf(
    path, cap, "/channels/%" PRIu64 "/directory-entries/list", channel_id);

  for (size_t i = 0; i < count && i < MOST; i++) {
    n += (size_t)snprintf(path + n, cap - n, "%sentity_ids=%" PRIu64,
                          i ? "&" : "?", guild_ids[i]);
  }

  s->directory_checking = true;
  s->directory_checked_channel = channel_id;
  api_request(s->api,
              &(ApiRequest){.method = "GET",
                            .path = path,
                            .route = "/channels/:id/directory-entries/list"},
              on_directory_check, call_new(s, channel_id, 0));
  free(path);
  changed(s);
}

/* ========================================================================== */
/* student hubs.                                                              */
/* ========================================================================== */

static void clear_schools(SessionHubJoin *hub) {
  for (size_t i = 0; i < hub->nschools; i++) {
    free(hub->schools[i].name);
    free(hub->schools[i].icon);
  }

  free(hub->schools);
  hub->schools = nullptr;
  hub->nschools = 0;
}

void session_hub_reset(Session *s) {
  SessionHubJoin *hub = &s->hub;

  clear_schools(hub);
  free(hub->email);
  free(hub->error);
  *hub = (SessionHubJoin){};
  changed(s);
}

// a hub's step failed: `error` says why, the step stays.
static bool hub_failed(Session *s, const ApiResponse *res) {
  s->hub.loading = false;

  if (res->ok) {
    return false;
  }

  str_set(&s->hub.error, res->error.message);
  changed(s);
  return true;
}

static void on_hub_lookup(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  SessionHubJoin *hub = &s->hub;

  call_end(call);

  if (hub_failed(s, res)) {
    return;
  }

  const cima_json *schools = json_get(res->json, "guilds_info");

  if (json_get_coerce(res->json, "has_matching_guild", false) &&
      call->id != 0) {
    hub->guild_id = call->id;
    hub->step = HUB_JOIN_CODE;
    changed(s);
    return;
  }

  clear_schools(hub);
  hub->schools = calloc(json_len(schools) + 1, sizeof *hub->schools);

  if (hub->schools == nullptr) {
    cilo_oom(json_len(schools) * sizeof *hub->schools);
  }

  CIMA_EACH(school, json_items(schools)) {
    Snowflake id = snowflake_of(json_get(school, "id"));

    // a guild discord's client takes for no hub at all.
    if (id == 0 || id == 884924873015689226) {
      continue;
    }

    hub->schools[hub->nschools++] = (SessionHubSchool){
      .id = id,
      .name = str_dup(json_get_or(school, "name", "")),
      .icon = str_dup(json_get_or(school, "icon", (const char *)nullptr)),
    };
  }

  if (hub->nschools == 0) {
    hub->step = HUB_JOIN_WAITLIST;
    changed(s);
  }
  else if (hub->nschools == 1) {
    // the only school: its code is asked for at once.
    session_hub_lookup(s, hub->email, hub->schools[0].id);
  }
  else {
    hub->step = HUB_JOIN_SCHOOL;
    changed(s);
  }
}

void session_hub_lookup(Session *s, const char *email, Snowflake guild_id) {
  SessionHubJoin *hub = &s->hub;

  if (hub->loading || email == nullptr || *email == '\0') {
    return;
  }

  if (hub->email != email) {
    str_set(&hub->email, email);
  }

  str_set(&hub->error, nullptr);

  if (!online(s)) {
    str_set(&hub->error, "Joining a hub needs a connection.");
    changed(s);
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "email", hub->email);

  if (guild_id != 0) {
    json_key(&w, "guild_id"), snowflake_write(&w, guild_id);
  }
  else {
    json_field(&w, "guild_id", nullptr);
  }

  json_field(&w, "allow_multiple_guilds", true);
  json_field(&w, "use_verification_code", true);
  json_end_object(&w);

  hub->loading = true;
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = "/guilds/automations/email-domain-lookup",
                            .body = cima_json_writer_text(&w)},
              on_hub_lookup, call_new(s, 0, guild_id));
  cima_json_writer_dispose(&w);
  changed(s);
}

static void on_hub_verify(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (hub_failed(s, res)) {
    return;
  }

  Snowflake id = snowflake_of(json_get(json_get(res->json, "guild"), "id"));

  s->hub.guild_id = id != 0 ? id : s->hub.guild_id;
  s->hub.step = HUB_JOIN_JOINED;
  changed(s);
}

void session_hub_verify(Session *s, const char *code) {
  SessionHubJoin *hub = &s->hub;

  if (hub->loading || hub->step != HUB_JOIN_CODE || code == nullptr ||
      *code == '\0') {
    return;
  }

  str_set(&hub->error, nullptr);

  if (!online(s)) {
    str_set(&hub->error, "Joining a hub needs a connection.");
    changed(s);
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "code", code);
  json_key(&w, "guild_id"), snowflake_write(&w, hub->guild_id);
  json_field(&w, "email", hub->email);
  json_end_object(&w);

  hub->loading = true;
  api_request(
    s->api,
    &(ApiRequest){.method = "POST",
                  .path = "/guilds/automations/email-domain-lookup/verify-code",
                  .body = cima_json_writer_text(&w)},
    on_hub_verify, call_new(s, 0, 0));
  cima_json_writer_dispose(&w);
  changed(s);
}

static void on_hub_waitlist(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (!hub_failed(s, res)) {
    s->hub.step = HUB_JOIN_WAITING;
    changed(s);
  }
}

void session_hub_waitlist(Session *s, const char *school) {
  SessionHubJoin *hub = &s->hub;

  if (hub->loading || hub->email == nullptr || school == nullptr ||
      *school == '\0') {
    return;
  }

  str_set(&hub->error, nullptr);

  if (!online(s)) {
    str_set(&hub->error, "Joining the waitlist needs a connection.");
    changed(s);
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "email", hub->email);
  json_field(&w, "school", school);
  json_end_object(&w);

  hub->loading = true;
  api_request(s->api,
              &(ApiRequest){.method = "POST",
                            .path = "/hub-waitlist/signup",
                            .body = cima_json_writer_text(&w)},
              on_hub_waitlist, call_new(s, 0, 0));
  cima_json_writer_dispose(&w);
  changed(s);
}

/* ========================================================================== */
/* membership.                                                                */
/* ========================================================================== */

static void on_left(void *ctx, [[maybe_unused]] const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;

  call_end(call);

  if (s->leaving_guild != call->id) {
    return;
  }

  // the guild goes with the gateway's GUILD_DELETE, or stays when discord
  // refused.
  s->leaving_guild = 0;
  changed(s);
}

void session_leave_guild(Session *s, Snowflake guild_id) {
  const Guild *g = store_guild(s->store, guild_id);

  if (g == nullptr || g->owner_id == s->store->self_id ||
      s->leaving_guild != 0) {
    return;
  }

  if (!online(s)) {
    store_remove_guild(s->store, guild_id);
    changed(s);
    return;
  }

  char path[64];

  snprintf(path, sizeof path, "/users/@me/guilds/%" PRIu64, guild_id);

  s->leaving_guild = guild_id;
  api_request(s->api,
              &(ApiRequest){.method = "DELETE",
                            .path = path,
                            .route = "/users/@me/guilds/:id",
                            .body = "{\"lurking\":false}"},
              on_left, call_new(s, 0, guild_id));
  changed(s);
}

void session_set_nick(Session *s, Snowflake guild_id, const char *nick) {
  Guild *g = store_guild(s->store, guild_id);

  if (g == nullptr) {
    return;
  }

  bool none = nick == nullptr || *nick == '\0';
  Member *me = store_member(g, s->store->self_id);

  // shown at once, the gateway's GUILD_MEMBER_UPDATE confirms it.
  if (me != nullptr) {
    str_set(&me->nick, none ? nullptr : nick);
    g->members_version++;
    store_touch_guild(s->store, g);
  }

  if (online(s)) {
    cima_json_writer w = {};
    char path[64];

    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/members/@me", guild_id);
    json_begin_object(&w);

    if (none) {
      json_field(&w, "nick", nullptr);
    }
    else {
      json_field(&w, "nick", nick);
    }

    json_end_object(&w);
    api_request(s->api,
                &(ApiRequest){.method = "PATCH",
                              .path = path,
                              .route = "/guilds/:id/members/@me",
                              .body = cima_json_writer_text(&w)},
                nullptr, nullptr);
    cima_json_writer_dispose(&w);
  }

  changed(s);
}

static void on_directory_created(void *ctx, const ApiResponse *res) {
  Call *call = ctx;

  if (call_stale(call)) {
    return;
  }

  Session *s = call->s;
  Snowflake id = snowflake_of(json_get(res->json, "id"));

  call_end(call);
  s->directory_saving = false;

  if (!res->ok || id == 0) {
    str_set(&s->directory_error,
            res->ok ? "Discord sent no server." : res->error.message);
    changed(s);
    return;
  }

  // the server exists, it is listed next. the guild itself comes through
  // the gateway's GUILD_CREATE.
  char *description = s->directory_pending_description;

  s->directory_pending_description = nullptr;
  session_directory_add(s, s->directory_pending_channel, id,
                        s->directory_pending_category, description);
  free(description);
}

void session_directory_create(Session *s, Snowflake channel_id,
                              size_t template_index, const char *name,
                              DirectoryCategory category,
                              const char *description) {
  if (s->directory_saving || template_index >= hub_template_count ||
      name == nullptr || *name == '\0') {
    return;
  }

  str_set(&s->directory_error, nullptr);
  s->directory_saved = 0;

  if (!online(s)) {
    str_set(&s->directory_error, "Creating a server needs a connection.");
    changed(s);
    return;
  }

  cima_json_writer w = {};

  json_begin_object(&w);
  json_field(&w, "name", name);
  json_end_object(&w);

  // the name, then the template's own fields.
  const char *named = cima_json_writer_text(&w);
  const char *rest = hub_templates[template_index].body + 1;
  size_t len = strlen(named);
  char *body = malloc(len + strlen(rest) + 1);

  if (body == nullptr) {
    cilo_oom(len + strlen(rest) + 1);
  }

  memcpy(body, named, len - 1);
  body[len - 1] = ',';
  strcpy(body + len, rest);
  cima_json_writer_dispose(&w);

  s->directory_pending_channel = channel_id;
  s->directory_pending_category = category;
  str_set(&s->directory_pending_description, description);
  s->directory_saving = true;
  api_request(s->api,
              &(ApiRequest){.method = "POST", .path = "/guilds", .body = body},
              on_directory_created, call_new(s, channel_id, 0));
  free(body);
  changed(s);
}
