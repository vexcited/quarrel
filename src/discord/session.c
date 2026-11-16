#include "discord/session.h"

#include <stdio.h>
#include <time.h>

#include "discord/api.h"
#include "discord/gateway.h"
#include "http.h"
#include "platform/platform.h"

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
    if (res->ok && json_is(res->json, JSON_ARRAY)) {
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

static bool persists(const Session *s) {
  return !s->options.no_keychain && !s->offline &&
         getenv("QUARREL_TOKEN") == nullptr;
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

static SessionAccount *account_find(Session *s, Snowflake id) {
  for (size_t i = 0; i < s->naccounts; i++) {
    if (s->accounts[i].id == id) {
      return &s->accounts[i];
    }
  }

  return nullptr;
}

static SessionAccount *account_add(Session *s, Snowflake id) {
  SessionAccount *found = account_find(s, id);

  if (found != nullptr) {
    return found;
  }

  size_t size = (s->naccounts + 1) * sizeof *s->accounts;
  SessionAccount *grown = realloc(s->accounts, size);

  if (grown == nullptr) {
    cilo_oom(size);
  }

  s->accounts = grown;
  s->accounts[s->naccounts] = (SessionAccount){.id = id};
  return &s->accounts[s->naccounts++];
}

static void save_last(Snowflake id) {
  char digits[21];

  platform_pref_set(LAST_ACCOUNT_KEY,
                    id != 0 ? snowflake_format(id, digits) : nullptr);
}

static void save_accounts(const Session *s) {
  JsonWriter w = {};

  jw_array(&w);

  for (size_t i = 0; i < s->naccounts; i++) {
    const SessionAccount *a = &s->accounts[i];

    jw_object(&w);
    jw_key(&w, "id"), jw_id(&w, a->id);
    jw_key(&w, "username"), jw_str(&w, a->username);
    jw_key(&w, "global_name"), jw_str(&w, a->global_name);
    jw_key(&w, "discriminator"), jw_str(&w, a->discriminator);
    jw_key(&w, "avatar"), jw_str(&w, a->avatar);
    jw_end(&w);
  }

  jw_end(&w);
  platform_pref_set(ACCOUNTS_KEY, w.data);
  jw_free(&w);
}

static void load_accounts(Session *s) {
  char *text = platform_pref_get(ACCOUNTS_KEY);

  if (text == nullptr) {
    return;
  }

  cilo_arena arena = {};
  const Json *saved = json_parse(&arena, text, strlen(text), nullptr);

  json_each(item, saved) {
    Snowflake id = json_get_u64(item, "id");

    if (id == 0) {
      continue;
    }

    SessionAccount *a = account_add(s, id);

    str_set(&a->username, json_get_str(item, "username"));
    str_set(&a->global_name, json_get_str(item, "global_name"));
    str_set(&a->discriminator, json_get_str(item, "discriminator"));
    str_set(&a->avatar, json_get_str(item, "avatar"));
  }

  cilo_arena_drop(&arena);
  free(text);
}

// keeps the account for the next launch: its token, what the switcher shows
// of it, and that it was the last one used.
static void remember(Session *s) {
  if (!persists(s) || s->token == nullptr || s->account_id == 0) {
    return;
  }

  char key[32];

  if (!s->token_saved) {
    s->token_saved =
      platform_secret_set(token_key(s->account_id, key), s->token);
    platform_secret_delete(LEGACY_TOKEN_KEY);
  }

  SessionAccount *a = account_add(s, s->account_id);
  const User *self = s->store->self;

  if (self != nullptr && self->id == a->id) {
    str_set(&a->username, self->username);
    str_set(&a->global_name, self->global_name);
    str_set(&a->discriminator, self->discriminator);
    str_set(&a->avatar, self->avatar);
  }

  save_accounts(s);
  save_last(s->account_id);
}

static void forget(Session *s, Snowflake id) {
  SessionAccount *a = account_find(s, id);

  if (!persists(s) || id == 0) {
    return;
  }

  char key[32];

  platform_secret_delete(token_key(id, key));

  if (a != nullptr) {
    size_t after = s->naccounts - (size_t)(a - s->accounts) - 1;

    account_clear(a);
    memmove(a, a + 1, after * sizeof *a);
    s->naccounts--;
    save_accounts(s);
  }

  char *last = platform_pref_get(LAST_ACCOUNT_KEY);

  if (snowflake_parse(last) == id) {
    save_last(s->naccounts > 0 ? s->accounts[0].id : 0);
  }

  free(last);
}

/* ========================================================================== */
/* the gateway.                                                               */
/* ========================================================================== */

static void on_typing_timer(void *ctx);

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

static void on_dispatch(void *ctx, const char *type, const Json *data) {
  Session *s = ctx;

  events_dispatch(s->store, type, data);

  if (strcmp(type, "READY") == 0) {
    // the client asks who the user is close to once it is ready. a new
    // session emptied the store, so every READY asks again.
    ApiRequest req = {.method = "GET", .path = "/users/@me/affinities/users"};

    api_request(s->api, &req, on_affinities, call_new(s, 0, 0));

    s->was_ready = true;

    // discord names the installation when the identify carried none.
    api_set_installation_id(
      json_get_str(json_get(data, "apex_experiments"), "installation"));

    // discord may replace the token, the old one stops working later.
    if (s->store->refreshed_token != nullptr) {
      str_set(&s->token, s->store->refreshed_token);
      api_set_token(s->api, s->token);
      gateway_set_token(s->gateway, s->token);
      s->token_saved = false;
    }

    s->account_id = s->store->self_id;
    remember(s);
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
      break;
    case GATEWAY_CONNECTING:
    case GATEWAY_HANDSHAKING:
    case GATEWAY_WAITING:
      set_state(s, s->was_ready ? SESSION_RECONNECTING : SESSION_CONNECTING);
      break;
    case GATEWAY_STOPPED:
      break;
  }
}

static void end_session(Session *s, const char *error) {
  gateway_free(s->gateway);
  s->gateway = nullptr;

  platform_timer_cancel(s->typing_timer);
  s->typing_timer = nullptr;

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
  char text[160];

  if (why == GATEWAY_FATAL_AUTH) {
    forget(s, s->account_id);
    end_session(s, "Discord no longer accepts this login. Log in again.");
    return;
  }

  snprintf(text, sizeof text, "Discord refused the connection (%d%s%s).", code,
           reason[0] != '\0' ? " " : "", reason);
  end_session(s, text);
}

static void connect(Session *s) {
  GatewayEvents events = {
    .ctx = s,
    .dispatch = on_dispatch,
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

  set_state(s, SESSION_CONNECTING);
  gateway_start(s->gateway);
}

/* ========================================================================== */
/* logging in and out.                                                        */
/* ========================================================================== */

// connects with a token the keychain may not hold yet.
static void enter(Session *s, const char *token, bool saved) {
  str_set(&s->token, token);
  s->account_id = token_account(token);
  s->token_saved = saved;

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

void session_logout(Session *s) {
  if (s->state == SESSION_LOGGED_OUT) {
    return;
  }

  // the request takes the token as it leaves, before it is forgotten.
  if (online(s)) {
    api_logout(s->api, nullptr, nullptr);
  }

  forget(s, s->account_id);
  end_session(s, nullptr);

  if (s->naccounts > 0) {
    session_switch_account(s, s->accounts[0].id);
  }
}

void session_switch_account(Session *s, Snowflake account_id) {
  if (account_id == 0 || account_id == s->account_id || !persists(s)) {
    return;
  }

  char key[32];
  char *token = platform_secret_get(token_key(account_id, key));

  // the keychain lost it, so the account is no longer logged in.
  if (token == nullptr || token[0] == '\0') {
    wipe(&token);
    forget(s, account_id);
    changed(s);
    return;
  }

  end_session(s, nullptr);
  enter(s, token, true);
  wipe(&token);
}

void session_add_account(Session *s) {
  if (s->state != SESSION_LOGGED_OUT) {
    end_session(s, nullptr);
  }
}

/* ========================================================================== */
/* starting.                                                                  */
/* ========================================================================== */

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

void session_start(Session *s) {
  const char *fixture = getenv("QUARREL_FIXTURE");
  const char *token = getenv("QUARREL_TOKEN");

  if (fixture != nullptr && fixture[0] != '\0') {
    session_load_fixture(s, fixture);
    return;
  }

  if (token != nullptr && token[0] != '\0') {
    str_set(&s->token, token);
    connect(s);
    return;
  }

  if (!persists(s)) {
    return;
  }

  load_accounts(s);

  char *last = platform_pref_get(LAST_ACCOUNT_KEY);
  Snowflake id = snowflake_parse(last);

  free(last);

  if (id == 0 && s->naccounts > 0) {
    id = s->accounts[0].id;
  }

  // an account whose token is gone is forgotten, the next one is tried.
  while (id != 0 && s->token == nullptr) {
    session_switch_account(s, id);
    id = s->naccounts > 0 ? s->accounts[0].id : 0;
  }

  if (s->token != nullptr) {
    return;
  }

  char *legacy = platform_secret_get(LEGACY_TOKEN_KEY);

  if (legacy != nullptr && legacy[0] != '\0') {
    enter(s, legacy, false);
  }

  wipe(&legacy);
}

Session *session_new(const SessionOptions *options) {
  Session *s = calloc(1, sizeof *s);

  if (s == nullptr) {
    cilo_oom(sizeof *s);
  }

  if (options != nullptr) {
    s->options = *options;
  }

  s->store = store_new();
  return s;
}

void session_free(Session *s) {
  if (s == nullptr) {
    return;
  }

  s->options.on_change = nullptr;

  gateway_free(s->gateway);
  api_free(s->api);
  platform_timer_cancel(s->typing_timer);

  for (Call *call = s->calls; call != nullptr;) {
    Call *next = call->next;
    free(call);
    call = next;
  }

  for (size_t i = 0; i < s->naccounts; i++) {
    account_clear(&s->accounts[i]);
  }

  free(s->accounts);
  clear_login(s);
  wipe(&s->token);
  store_free(s->store);
  free(s->error);
  free(s);
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
  JsonWriter w = {};
  char timestamp[40];

  iso_now(s->store, timestamp);

  jw_object(&w);
  jw_key(&w, "id"), jw_id(&w, pending->nonce);
  jw_key(&w, "nonce"), jw_id(&w, pending->nonce);
  jw_key(&w, "channel_id"), jw_id(&w, c->id);
  jw_key(&w, "type"), jw_u64(&w, pending->type);
  jw_key(&w, "content"), jw_str(&w, pending->content);
  jw_key(&w, "timestamp"), jw_str(&w, timestamp);
  jw_key(&w, "author");
  jw_object(&w);
  jw_key(&w, "id"), jw_id(&w, s->store->self_id);
  jw_end(&w);

  if (pending->reference_id != 0) {
    jw_key(&w, "message_reference");
    jw_object(&w);
    jw_key(&w, "message_id"), jw_id(&w, pending->reference_id);
    jw_key(&w, "channel_id"), jw_id(&w, c->id);
    jw_end(&w);

    const Message *target = store_message(c, pending->reference_id);

    if (target != nullptr) {
      jw_key(&w, "referenced_message");
      jw_object(&w);
      jw_key(&w, "id"), jw_id(&w, target->id);
      jw_key(&w, "channel_id"), jw_id(&w, c->id);
      jw_key(&w, "content"), jw_str(&w, target->content);
      jw_key(&w, "author");
      jw_object(&w);
      jw_key(&w, "id"), jw_id(&w, target->author_id);
      jw_end(&w);
      jw_end(&w);
    }
  }

  jw_end(&w);

  cilo_arena arena = {};
  store_create_message(s->store, json_parse(&arena, w.data, w.len, nullptr));
  cilo_arena_drop(&arena);
  jw_free(&w);
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
static void apply_local(Session *s, const char *type, JsonWriter *w) {
  cilo_arena arena = {};

  events_dispatch(s->store, type, json_parse(&arena, w->data, w->len, nullptr));
  cilo_arena_drop(&arena);
  jw_free(w);
}

void session_edit(Session *s, Snowflake channel_id, Snowflake message_id,
                  const char *content) {
  Channel *c = store_channel(s->store, channel_id);

  if (store_message(c, message_id) == nullptr || content == nullptr) {
    return;
  }

  if (s->offline) {
    JsonWriter w = {};
    char timestamp[40];

    iso_now(s->store, timestamp);

    jw_object(&w);
    jw_key(&w, "id"), jw_id(&w, message_id);
    jw_key(&w, "channel_id"), jw_id(&w, channel_id);
    jw_key(&w, "content"), jw_str(&w, content);
    jw_key(&w, "edited_timestamp"), jw_str(&w, timestamp);
    jw_end(&w);

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
    JsonWriter w = {};
    cilo_arena arena = {};

    jw_object(&w);
    jw_key(&w, "id");

    if (emoji_id != 0) {
      jw_id(&w, emoji_id);
    }
    else {
      jw_null(&w);
    }

    jw_key(&w, "name"), jw_str(&w, name);
    jw_end(&w);

    const Json *emoji = json_parse(&arena, w.data, w.len, nullptr);

    if (add) {
      store_reaction_add(s->store, channel_id, message_id, s->store->self_id,
                         emoji);
    }
    else {
      store_reaction_remove(s->store, channel_id, message_id, s->store->self_id,
                            emoji);
    }

    cilo_arena_drop(&arena);
    jw_free(&w);
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

void session_set_status(Session *s, UserStatus status, bool invisible) {
  const char *name = invisible               ? "invisible"
                     : status == STATUS_IDLE ? "idle"
                     : status == STATUS_DND  ? "dnd"
                                             : "online";

  s->store->self_status = invisible ? STATUS_OFFLINE : status;
  s->store->version++;

  if (gateway_ready(s)) {
    gateway_update_presence(s->gateway, name, false);
  }

  changed(s);
}
