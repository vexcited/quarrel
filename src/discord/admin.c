#include "discord/admin.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "discord/api.h"
#include "discord/gateway.h"

typedef enum : uint8_t {
  REQ_EDIT_GUILD,
  REQ_LEAVE,
  REQ_CREATE_ROLE,
  REQ_EDIT_ROLE,
  REQ_DELETE_ROLE,
  REQ_ORDER_ROLES,
  REQ_COUNTS,
  REQ_MEMBERS,
  REQ_ADD_MEMBERS,
  REQ_REMOVE_MEMBER,
  REQ_CREATE_CHANNEL,
  REQ_DELETE_CHANNEL,
  REQ_RENAME_CHANNEL,
  REQ_MOVE_CHANNELS,
  REQ_BANS,
  REQ_UNBAN,
  REQ_INVITES,
  REQ_REVOKE,
  REQ_EMOJI,
} RequestKind;

// a request on its way, in the admin's list until it is answered or the
// admin is freed.
typedef struct Request Request;

struct Request {
  Request *next;
  Session *s;
  RequestKind kind;
  uint64_t epoch, ticket;
  Snowflake guild_id, id, user_id;
  // the events that undo what was applied before discord answered, for a
  // move it refuses.
  char *undo;
  char *code;
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

/* ========================================================================== */
/* the admin and its ops.                                                     */
/* ========================================================================== */

static void clear_lists(Admin *a) {
  for (size_t i = 0; i < a->nbans; i++) {
    free(a->bans[i].reason);
  }

  for (size_t i = 0; i < a->ninvites; i++) {
    free(a->invites[i].code);
  }

  free(a->bans);
  free(a->invites);
  free(a->counts);
  free(a->members);
  a->bans = nullptr;
  a->invites = nullptr;
  a->counts = nullptr;
  a->members = nullptr;
  a->nbans = a->ninvites = a->ncounts = a->nmembers = 0;
  a->bans_loading = a->bans_loaded = false;
  a->invites_loading = a->invites_loaded = false;
  a->counts_loading = a->members_loading = false;
  a->members_role = 0;
}

static void clear_ops(Admin *a) {
  AdminOp *ops[] = {&a->save,   &a->role,   &a->channel,
                    &a->move,   &a->action, &a->leave};

  for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
    admin_clear(a, ops[i]);
  }
}

static void free_request(Request *r) {
  free(r->undo);
  free(r->code);
  free(r);
}

static Admin *admin_get(Session *s) {
  if (s->admin == nullptr) {
    s->admin = xcalloc(1, sizeof(Admin));
    s->admin->epoch = s->epoch;
  }

  Admin *a = s->admin;

  // another account: nothing known still holds.
  if (a->epoch != s->epoch) {
    clear_lists(a);
    clear_ops(a);
    a->guild_id = 0;
    a->epoch = s->epoch;
  }

  return a;
}

Admin *admin_of(Session *s, Snowflake guild_id) {
  Admin *a = admin_get(s);

  if (a->guild_id != guild_id) {
    clear_lists(a);
    clear_ops(a);
    a->guild_id = guild_id;
    a->version++;
  }

  return a;
}

void admin_free(Admin *a) {
  if (a == nullptr) {
    return;
  }

  for (Request *r = a->requests; r != nullptr;) {
    Request *next = r->next;
    free_request(r);
    r = next;
  }

  clear_lists(a);
  clear_ops(a);
  free(a);
}

void admin_clear(Admin *a, AdminOp *op) {
  free(op->error);
  op->error = nullptr;
  op->status = ADMIN_IDLE;
  op->made = 0;
  // an answer still on its way is not this op's anymore.
  op->ticket = ++a->tickets;
  a->version++;
}

static void changed(Session *s) {
  s->admin->version++;
  s->version++;

  if (s->options.on_change != nullptr) {
    s->options.on_change();
  }
}

static void op_start(Admin *a, AdminOp *op) {
  free(op->error);
  op->error = nullptr;
  op->status = ADMIN_BUSY;
  op->made = 0;
  op->ticket = ++a->tickets;
}

static void op_fail(Session *s, AdminOp *op, const char *error) {
  op->status = ADMIN_FAILED;
  str_set(&op->error, error);
  changed(s);
}

static void op_done(Session *s, AdminOp *op, Snowflake made) {
  op->status = ADMIN_DONE;
  op->made = made;
  changed(s);
}

// the op of a request kind, which its answer ends.
static AdminOp *op_of(Admin *a, RequestKind kind) {
  switch (kind) {
    case REQ_EDIT_GUILD:
    case REQ_EDIT_ROLE:
      return &a->save;
    case REQ_LEAVE:
      return &a->leave;
    case REQ_CREATE_ROLE:
    case REQ_DELETE_ROLE:
      return &a->role;
    case REQ_ORDER_ROLES:
    case REQ_MOVE_CHANNELS:
      return &a->move;
    case REQ_CREATE_CHANNEL:
    case REQ_DELETE_CHANNEL:
    case REQ_RENAME_CHANNEL:
      return &a->channel;
    case REQ_ADD_MEMBERS:
    case REQ_REMOVE_MEMBER:
    case REQ_UNBAN:
    case REQ_REVOKE:
    case REQ_EMOJI:
      return &a->action;
    default:
      return nullptr;
  }
}

/* ========================================================================== */
/* requests.                                                                  */
/* ========================================================================== */

static void on_answer(void *ctx, const ApiResponse *res);

static Request *request_new(Session *s, RequestKind kind, Snowflake guild_id) {
  Admin *a = admin_get(s);
  Request *r = xcalloc(1, sizeof *r);
  AdminOp *op = op_of(a, kind);

  r->s = s;
  r->kind = kind;
  r->epoch = s->epoch;
  r->guild_id = guild_id;

  if (op != nullptr) {
    op_start(a, op);
    r->ticket = op->ticket;
  }

  r->next = a->requests;
  a->requests = r;
  return r;
}

static void request_unlink(Admin *a, Request *r) {
  for (Request **at = (Request **)&a->requests; *at != nullptr;
       at = &(*at)->next) {
    if (*at == r) {
      *at = r->next;
      return;
    }
  }
}

static void send(Session *s, Request *r, const char *method, const char *path,
                 const char *route, const char *body) {
  ApiRequest req = {
    .method = method, .path = path, .route = route, .body = body};

  api_request(s->api, &req, on_answer, r);
}

// discord's events, `events` a json array of {t, d}, applied to the store.
static void apply_events(Session *s, const char *events) {
  if (events != nullptr) {
    (void)events_feed(s->store, events, strlen(events));
  }
}

static void apply_event(Session *s, const char *type, const char *data) {
  size_t len = strlen(type) + strlen(data) + 32;
  char *text = xcalloc(1, len);

  snprintf(text, len, "{\"t\":\"%s\",\"d\":%s}", type, data);
  apply_events(s, text);
  free(text);
}

// an id for what is made offline, after every id there is.
static Snowflake made_id(Session *s) {
  static uint64_t counter;
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  int64_t ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

  return snowflake_from_time_ms(ms) + (++counter & 0x3fffff) +
         ((uint64_t)(s->epoch & 0xff) << 12);
}

static void write_permissions(cima_json_writer *w, const char *key,
                              Permissions p) {
  char digits[24];

  snprintf(digits, sizeof digits, "%" PRIu64, p);
  json_field(w, key, digits);
}

static void write_id_or_null(cima_json_writer *w, const char *key,
                             Snowflake id) {
  json_key(w, key);

  if (id != 0) {
    snowflake_write(w, id);
  }
  else {
    cima_json_write_null(w);
  }
}

/* ========================================================================== */
/* answers.                                                                   */
/* ========================================================================== */

static void put_counts(Admin *a, const cima_json *json) {
  free(a->counts);
  a->counts = nullptr;
  a->ncounts = 0;

  if (json_type(json) != CIMA_JSON_OBJECT) {
    return;
  }

  a->counts = xcalloc(json_len(json) + 1, sizeof *a->counts);

  CIMA_EACH(field, json_members(json)) {
    a->counts[a->ncounts++] = (AdminRoleCount){
      .role_id = snowflake_parse(field->key),
      .count = (uint32_t)snowflake_of(&field->value),
    };
  }
}

static void put_members(Session *s, Admin *a, const cima_json *json) {
  free(a->members);
  a->members = nullptr;
  a->nmembers = 0;

  if (json_type(json) != CIMA_JSON_ARRAY) {
    return;
  }

  a->members = xcalloc(json_len(json) + 1, sizeof *a->members);

  CIMA_EACH(item, json_items(json)) {
    Snowflake id = snowflake_of(item);

    if (id != 0) {
      a->members[a->nmembers++] = id;
    }
  }

  // their names and avatars, for the ones the store does not know.
  Guild *g = store_guild(s->store, a->guild_id);
  Snowflake unknown[100];
  size_t nunknown = 0;

  for (size_t i = 0; g != nullptr && i < a->nmembers && nunknown < 100; i++) {
    if (store_member(g, a->members[i]) == nullptr) {
      unknown[nunknown++] = a->members[i];
    }
  }

  if (nunknown != 0 && s->gateway != nullptr) {
    gateway_request_members(s->gateway, a->guild_id, unknown, nunknown);
  }
}

static void put_bans(Session *s, Admin *a, const cima_json *json) {
  for (size_t i = 0; i < a->nbans; i++) {
    free(a->bans[i].reason);
  }

  free(a->bans);
  a->bans = nullptr;
  a->nbans = 0;

  if (json_type(json) != CIMA_JSON_ARRAY) {
    return;
  }

  a->bans = xcalloc(json_len(json) + 1, sizeof *a->bans);

  CIMA_EACH(item, json_items(json)) {
    User *u = store_put_user(s->store, json_get(item, "user"));

    if (u != nullptr) {
      a->bans[a->nbans++] = (AdminBan){
        .user_id = u->id,
        .reason = str_dup(json_get_or(item, "reason", nullptr)),
      };
    }
  }
}

static void put_invites(Session *s, Admin *a, const cima_json *json) {
  for (size_t i = 0; i < a->ninvites; i++) {
    free(a->invites[i].code);
  }

  free(a->invites);
  a->invites = nullptr;
  a->ninvites = 0;

  if (json_type(json) != CIMA_JSON_ARRAY) {
    return;
  }

  a->invites = xcalloc(json_len(json) + 1, sizeof *a->invites);

  CIMA_EACH(item, json_items(json)) {
    const char *code = json_get_or(item, "code", nullptr);
    User *inviter = store_put_user(s->store, json_get(item, "inviter"));
    int64_t created =
      iso8601_parse_ms(json_get_or(item, "created_at", nullptr));
    int64_t max_age = json_get_coerce(item, "max_age", (int64_t)0);
    int64_t expires =
      iso8601_parse_ms(json_get_or(item, "expires_at", nullptr));

    if (code == nullptr) {
      continue;
    }

    if (expires == 0 && max_age > 0 && created != 0) {
      expires = created + max_age * 1000;
    }

    a->invites[a->ninvites++] = (AdminInvite){
      .code = str_dup(code),
      .channel_id = snowflake_of(json_get(json_get(item, "channel"), "id")),
      .inviter_id = inviter != nullptr ? inviter->id : 0,
      .uses = (uint32_t)json_get_coerce(item, "uses", (int64_t)0),
      .max_uses = (uint32_t)json_get_coerce(item, "max_uses", (int64_t)0),
      .expires_at = expires,
      .temporary = json_get_coerce(item, "temporary", false),
    };
  }
}

static void drop_ban(Admin *a, Snowflake user_id) {
  for (size_t i = 0; i < a->nbans; i++) {
    if (a->bans[i].user_id == user_id) {
      free(a->bans[i].reason);
      a->bans[i] = a->bans[--a->nbans];
      return;
    }
  }
}

static void drop_invite(Admin *a, const char *code) {
  for (size_t i = 0; code != nullptr && i < a->ninvites; i++) {
    if (strcmp(a->invites[i].code, code) == 0) {
      free(a->invites[i].code);
      memmove(&a->invites[i], &a->invites[i + 1],
              (a->ninvites - i - 1) * sizeof *a->invites);
      a->ninvites--;
      return;
    }
  }
}

static void drop_member(Admin *a, Snowflake user_id) {
  for (size_t i = 0; i < a->nmembers; i++) {
    if (a->members[i] == user_id) {
      memmove(&a->members[i], &a->members[i + 1],
              (a->nmembers - i - 1) * sizeof *a->members);
      a->nmembers--;
      return;
    }
  }
}

// what the answer of a request changes in the store, before the gateway
// tells the same.
static void take_answer(Session *s, Admin *a, Request *r,
                        const ApiResponse *res) {
  Store *store = s->store;
  Guild *g = store_guild(store, r->guild_id);
  const cima_json *json = res->json;
  bool mine = r->guild_id == a->guild_id;

  switch (r->kind) {
    case REQ_EDIT_GUILD:
      if (json_type(json) == CIMA_JSON_OBJECT) {
        store_update_guild(store, json);
      }
      break;
    case REQ_CREATE_ROLE:
    case REQ_EDIT_ROLE:
      if (g != nullptr && json_type(json) == CIMA_JSON_OBJECT) {
        Role *role = store_put_role(store, g, json);
        r->id = role != nullptr ? role->id : 0;
      }
      break;
    case REQ_DELETE_ROLE:
      if (g != nullptr) {
        store_remove_role(store, g, r->id);
      }
      break;
    case REQ_ORDER_ROLES:
      CIMA_EACH(role, json_items(json)) {
        if (g != nullptr) {
          store_put_role(store, g, role);
        }
      }
      break;
    case REQ_COUNTS:
      if (mine) {
        put_counts(a, json);
        a->counts_loading = false;
      }
      break;
    case REQ_MEMBERS:
      if (mine && a->members_role == r->id) {
        put_members(s, a, json);
        a->members_loading = false;
      }
      break;
    case REQ_ADD_MEMBERS:
      CIMA_EACH(field, json_members(json)) {
        store_put_member(store, g, &field->value, 0);
      }

      if (mine && a->members_role != 0) {
        admin_fetch_members(s, r->guild_id, a->members_role);
        admin_fetch_counts(s, r->guild_id);
      }
      break;
    case REQ_REMOVE_MEMBER:
      if (json_type(json) == CIMA_JSON_OBJECT) {
        store_put_member(store, g, json, r->user_id);
      }

      if (mine && a->members_role == r->id) {
        drop_member(a, r->user_id);
      }
      break;
    case REQ_CREATE_CHANNEL:
      if (json_type(json) == CIMA_JSON_OBJECT) {
        Channel *c = store_put_channel(store, json, r->guild_id);
        r->id = c != nullptr ? c->id : 0;
      }
      break;
    case REQ_DELETE_CHANNEL:
      store_remove_channel(store, r->id);
      break;
    case REQ_RENAME_CHANNEL:
      if (json_type(json) == CIMA_JSON_OBJECT) {
        store_put_channel(store, json, r->guild_id);
      }
      break;
    case REQ_BANS:
      if (mine) {
        put_bans(s, a, json);
        a->bans_loading = false;
        a->bans_loaded = true;
      }
      break;
    case REQ_UNBAN:
      if (mine) {
        drop_ban(a, r->user_id);
      }
      break;
    case REQ_INVITES:
      if (mine) {
        put_invites(s, a, json);
        a->invites_loading = false;
        a->invites_loaded = true;
      }
      break;
    case REQ_REVOKE:
      if (mine) {
        drop_invite(a, r->code);
      }
      break;
    case REQ_LEAVE:
    case REQ_MOVE_CHANNELS:
    case REQ_EMOJI:
      break;
  }
}

static void on_answer(void *ctx, const ApiResponse *res) {
  Request *r = ctx;
  Session *s = r->s;
  Admin *a = s->admin;

  request_unlink(a, r);

  // the account it was made for is gone.
  if (r->epoch != s->epoch) {
    free_request(r);
    return;
  }

  AdminOp *op = op_of(a, r->kind);
  bool current = op != nullptr && op->ticket == r->ticket &&
                 r->guild_id == a->guild_id;

  if (res->ok) {
    take_answer(s, a, r, res);
  }
  else {
    // what was applied ahead goes back.
    apply_events(s, r->undo);

    if (r->kind == REQ_COUNTS) {
      a->counts_loading = false;
    }
    else if (r->kind == REQ_MEMBERS) {
      a->members_loading = false;
    }
    else if (r->kind == REQ_BANS) {
      a->bans_loading = false;
    }
    else if (r->kind == REQ_INVITES) {
      a->invites_loading = false;
    }
  }

  if (current && res->ok) {
    op_done(s, op, r->id);
  }
  else if (current) {
    op_fail(s, op, res->error.message);
  }
  else {
    changed(s);
  }

  free_request(r);
}

// a request that cannot go: offline it is applied to the store by the
// caller, online without a connection it fails at once.
static bool can_send(Session *s, AdminOp *op) {
  if (online(s)) {
    return true;
  }

  if (!s->offline && op != nullptr) {
    op->status = ADMIN_FAILED;
    str_set(&op->error, "This needs a connection.");
    changed(s);
  }

  return false;
}

static void offline_done(Session *s, AdminOp *op, Snowflake made) {
  if (op != nullptr) {
    op_start(s->admin, op);
    op_done(s, op, made);
  }
  else {
    changed(s);
  }
}

/* ========================================================================== */
/* the guild.                                                                 */
/* ========================================================================== */

static void write_guild_edit(cima_json_writer *w, const GuildEdit *e) {
  uint32_t f = e->fields;

  if (f & GUILD_EDIT_NAME) {
    json_field(w, "name", e->name);
  }

  if (f & GUILD_EDIT_ICON) {
    json_field(w, "icon", e->icon);
  }

  if (f & GUILD_EDIT_BANNER) {
    json_field(w, "banner", e->banner);
  }

  if (f & GUILD_EDIT_SPLASH) {
    json_field(w, "splash", e->splash);
  }

  if (f & GUILD_EDIT_DESCRIPTION) {
    json_field(w, "description", e->description);
  }

  if (f & GUILD_EDIT_AFK_CHANNEL) {
    write_id_or_null(w, "afk_channel_id", e->afk_channel_id);
  }

  if (f & GUILD_EDIT_AFK_TIMEOUT) {
    json_field(w, "afk_timeout", (int64_t)e->afk_timeout);
  }

  if (f & GUILD_EDIT_SYSTEM_CHANNEL) {
    write_id_or_null(w, "system_channel_id", e->system_channel_id);
  }

  if (f & GUILD_EDIT_SYSTEM_FLAGS) {
    json_field(w, "system_channel_flags", (int64_t)e->system_flags);
  }

  if (f & GUILD_EDIT_NOTIFICATIONS) {
    json_field(w, "default_message_notifications",
               (int64_t)e->notifications);
  }

  if (f & GUILD_EDIT_VERIFICATION) {
    json_field(w, "verification_level", (int64_t)e->verification);
  }

  if (f & GUILD_EDIT_CONTENT_FILTER) {
    json_field(w, "explicit_content_filter", (int64_t)e->content_filter);
  }

  if (f & GUILD_EDIT_RULES_CHANNEL) {
    write_id_or_null(w, "rules_channel_id", e->rules_channel_id);
  }

  if (f & GUILD_EDIT_UPDATES_CHANNEL) {
    write_id_or_null(w, "public_updates_channel_id", e->updates_channel_id);
  }

  if (f & GUILD_EDIT_SAFETY_CHANNEL) {
    write_id_or_null(w, "safety_alerts_channel_id", e->safety_channel_id);
  }

  if (f & GUILD_EDIT_LOCALE) {
    json_field(w, "preferred_locale", e->locale);
  }

  if (f & GUILD_EDIT_PROGRESS_BAR) {
    json_field(w, "premium_progress_bar_enabled", e->progress_bar);
  }

  if (f & GUILD_EDIT_FEATURES) {
    json_key(w, "features");
    json_begin_array(w);

    for (size_t i = 0; i < e->nfeatures; i++) {
      cima_json_write_string(w, e->features[i]);
    }

    json_end_array(w);
  }
}

void admin_edit_guild(Session *s, Snowflake guild_id, const GuildEdit *edit) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[64];

  json_begin_object(&w);
  write_guild_edit(&w, edit);
  json_end_object(&w);

  if (can_send(s, &a->save)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64, guild_id);
    send(s, request_new(s, REQ_EDIT_GUILD, guild_id), "PATCH", path, nullptr,
         cima_json_writer_text(&w));
  }
  else if (s->offline) {
    cima_json_writer event = {};
    char digits[21];

    // offline, images are dropped: nothing serves them.
    GuildEdit kept = *edit;
    kept.fields &= ~(GUILD_EDIT_ICON | GUILD_EDIT_BANNER | GUILD_EDIT_SPLASH);

    json_begin_object(&event);
    json_field(&event, "id", snowflake_format(guild_id, digits));
    write_guild_edit(&event, &kept);
    json_end_object(&event);
    apply_event(s, "GUILD_UPDATE", cima_json_writer_text(&event));
    cima_json_writer_dispose(&event);
    offline_done(s, &a->save, 0);
  }

  cima_json_writer_dispose(&w);
}

void admin_delete_guild(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  char path[64];

  if (can_send(s, &a->leave)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/delete", guild_id);
    send(s, request_new(s, REQ_LEAVE, guild_id), "POST", path, nullptr,
         nullptr);
  }
  else if (s->offline) {
    char data[64];

    snprintf(data, sizeof data, "{\"id\":\"%" PRIu64 "\"}", guild_id);
    apply_event(s, "GUILD_DELETE", data);
    offline_done(s, &a->leave, 0);
  }
}

void admin_leave_guild(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  char path[64];

  if (can_send(s, &a->leave)) {
    snprintf(path, sizeof path, "/users/@me/guilds/%" PRIu64, guild_id);
    send(s, request_new(s, REQ_LEAVE, guild_id), "DELETE", path,
         "/users/@me/guilds/", "{\"lurking\":false}");
  }
  else if (s->offline) {
    char data[64];

    snprintf(data, sizeof data, "{\"id\":\"%" PRIu64 "\"}", guild_id);
    apply_event(s, "GUILD_DELETE", data);
    offline_done(s, &a->leave, 0);
  }
}

/* ========================================================================== */
/* roles.                                                                     */
/* ========================================================================== */

static void write_role_edit(cima_json_writer *w, const RoleEdit *e) {
  uint32_t f = e->fields;

  if (f & ROLE_EDIT_NAME) {
    json_field(w, "name", e->name);
  }

  if (f & ROLE_EDIT_COLOR) {
    json_field(w, "color", (int64_t)e->color);
    json_key(w, "colors");
    json_begin_object(w);
    json_field(w, "primary_color", (int64_t)e->color);

    if (e->secondary_color != 0) {
      json_field(w, "secondary_color", (int64_t)e->secondary_color);
    }
    else {
      json_key(w, "secondary_color");
      cima_json_write_null(w);
    }

    if (e->tertiary_color != 0) {
      json_field(w, "tertiary_color", (int64_t)e->tertiary_color);
    }
    else {
      json_key(w, "tertiary_color");
      cima_json_write_null(w);
    }

    json_end_object(w);
  }

  if (f & ROLE_EDIT_HOIST) {
    json_field(w, "hoist", e->hoist);
  }

  if (f & ROLE_EDIT_MENTIONABLE) {
    json_field(w, "mentionable", e->mentionable);
  }

  if (f & ROLE_EDIT_PERMISSIONS) {
    write_permissions(w, "permissions", e->permissions);
  }

  if (f & ROLE_EDIT_ICON) {
    json_field(w, "icon", e->icon);
    json_field(w, "unicode_emoji", e->unicode_emoji);
  }
}

static void role_event(Session *s, const char *type, Snowflake guild_id,
                       Snowflake role_id, const char *fields) {
  size_t len = strlen(fields) + 128;
  char *data = xcalloc(1, len);

  snprintf(data, len,
           "{\"guild_id\":\"%" PRIu64 "\",\"role\":{\"id\":\"%" PRIu64
           "\"%s%s}}",
           guild_id, role_id, *fields ? "," : "", fields);
  apply_event(s, type, data);
  free(data);
}

void admin_create_role(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  const Guild *g = store_guild(s->store, guild_id);
  cima_json_writer w = {};
  char path[64];

  // what discord's client posts, a role that grants nothing.
  json_begin_object(&w);
  json_field(&w, "name", "new role");
  json_field(&w, "color", 0);
  json_key(&w, "colors");
  json_begin_object(&w);
  json_field(&w, "primary_color", 0);
  json_key(&w, "secondary_color");
  cima_json_write_null(&w);
  json_key(&w, "tertiary_color");
  cima_json_write_null(&w);
  json_end_object(&w);
  write_permissions(&w, "permissions", 0);
  json_end_object(&w);

  if (can_send(s, &a->role)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/roles", guild_id);
    send(s, request_new(s, REQ_CREATE_ROLE, guild_id), "POST", path, nullptr,
         cima_json_writer_text(&w));
  }
  else if (s->offline) {
    Snowflake id = made_id(s);
    char fields[160];

    // a new role goes right over @everyone.
    snprintf(fields, sizeof fields,
             "\"name\":\"new role\",\"color\":0,\"position\":1,"
             "\"permissions\":\"0\"");

    for (size_t i = 0; g != nullptr && i < g->nroles; i++) {
      const Role *other = g->roles[i];
      char moved[48];

      if (other->id != guild_id) {
        snprintf(moved, sizeof moved, "\"position\":%d", other->position + 1);
        role_event(s, "GUILD_ROLE_UPDATE", guild_id, other->id, moved);
      }
    }

    role_event(s, "GUILD_ROLE_CREATE", guild_id, id, fields);
    offline_done(s, &a->role, id);
  }

  cima_json_writer_dispose(&w);
}

void admin_edit_role(Session *s, Snowflake guild_id, Snowflake role_id,
                     const RoleEdit *edit) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[96];

  json_begin_object(&w);
  write_role_edit(&w, edit);
  json_end_object(&w);

  if (can_send(s, &a->save)) {
    Request *r = request_new(s, REQ_EDIT_ROLE, guild_id);

    r->id = role_id;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/roles/%" PRIu64,
             guild_id, role_id);
    send(s, r, "PATCH", path, "/guilds/roles", cima_json_writer_text(&w));
  }
  else if (s->offline) {
    const char *text = cima_json_writer_text(&w);
    // the fields without their braces.
    size_t len = strlen(text);
    char *fields = str_dup_n(text + 1, len >= 2 ? len - 2 : 0);

    role_event(s, "GUILD_ROLE_UPDATE", guild_id, role_id, fields);
    free(fields);
    offline_done(s, &a->save, role_id);
  }

  cima_json_writer_dispose(&w);
}

void admin_delete_role(Session *s, Snowflake guild_id, Snowflake role_id) {
  Admin *a = admin_of(s, guild_id);
  char path[96];

  if (can_send(s, &a->role)) {
    Request *r = request_new(s, REQ_DELETE_ROLE, guild_id);

    r->id = role_id;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/roles/%" PRIu64,
             guild_id, role_id);
    send(s, r, "DELETE", path, "/guilds/roles", nullptr);
  }
  else if (s->offline) {
    char data[96];

    snprintf(data, sizeof data,
             "{\"guild_id\":\"%" PRIu64 "\",\"role_id\":\"%" PRIu64 "\"}",
             guild_id, role_id);
    apply_event(s, "GUILD_ROLE_DELETE", data);
    offline_done(s, &a->role, 0);
  }
}

// the events setting each role of `ids` at `positions`.
static void role_positions(cima_json_writer *w, Snowflake guild_id,
                           const Snowflake *ids, const int32_t *positions,
                           size_t count) {
  json_begin_array(w);

  for (size_t i = 0; i < count; i++) {
    json_begin_object(w);
    json_field(w, "t", "GUILD_ROLE_UPDATE");
    json_key(w, "d");
    json_begin_object(w);
    json_key(w, "guild_id");
    snowflake_write(w, guild_id);
    json_key(w, "role");
    json_begin_object(w);
    json_key(w, "id");
    snowflake_write(w, ids[i]);
    json_field(w, "position", (int64_t)positions[i]);
    json_end_object(w);
    json_end_object(w);
    json_end_object(w);
  }

  json_end_array(w);
}

void admin_order_roles(Session *s, Snowflake guild_id, const Snowflake *order,
                       size_t count) {
  Admin *a = admin_of(s, guild_id);
  Guild *g = store_guild(s->store, guild_id);

  if (g == nullptr || count == 0) {
    return;
  }

  // as discord's client does: the whole list numbered from the top down to
  // @everyone at 0, only the roles whose position changes sent.
  Snowflake *ids = xcalloc(count, sizeof *ids);
  int32_t *to = xcalloc(count, sizeof *to);
  int32_t *from = xcalloc(count, sizeof *from);
  size_t n = 0;

  for (size_t i = count; i > 0; i--) {
    const Role *role = store_role(g, order[i - 1]);
    int32_t position = (int32_t)(count - i);

    if (role != nullptr && role->position != position) {
      ids[n] = role->id;
      to[n] = position;
      from[n] = role->position;
      n++;
    }
  }

  if (n != 0) {
    cima_json_writer apply = {}, undo = {}, body = {};

    role_positions(&apply, guild_id, ids, to, n);
    role_positions(&undo, guild_id, ids, from, n);
    apply_events(s, cima_json_writer_text(&apply));

    json_begin_array(&body);

    for (size_t i = 0; i < n; i++) {
      json_begin_object(&body);
      json_key(&body, "id");
      snowflake_write(&body, ids[i]);
      json_field(&body, "position", (int64_t)to[i]);
      json_end_object(&body);
    }

    json_end_array(&body);

    if (can_send(s, &a->move)) {
      Request *r = request_new(s, REQ_ORDER_ROLES, guild_id);
      char path[64];

      r->undo = str_dup(cima_json_writer_text(&undo));
      snprintf(path, sizeof path, "/guilds/%" PRIu64 "/roles", guild_id);
      send(s, r, "PATCH", path, nullptr, cima_json_writer_text(&body));
    }
    else if (s->offline) {
      offline_done(s, &a->move, 0);
    }
    else {
      apply_events(s, cima_json_writer_text(&undo));
    }

    cima_json_writer_dispose(&apply);
    cima_json_writer_dispose(&undo);
    cima_json_writer_dispose(&body);
  }

  free(ids);
  free(to);
  free(from);
}

void admin_fetch_counts(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  char path[80];

  if (a->counts_loading) {
    return;
  }

  if (can_send(s, nullptr)) {
    a->counts_loading = true;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/roles/member-counts",
             guild_id);
    send(s, request_new(s, REQ_COUNTS, guild_id), "GET", path, nullptr,
         nullptr);
    return;
  }

  // offline, the members the store knows.
  Guild *g = store_guild(s->store, guild_id);

  free(a->counts);
  a->counts = nullptr;
  a->ncounts = 0;

  if (g == nullptr || g->nroles == 0) {
    return;
  }

  a->counts = xcalloc(g->nroles, sizeof *a->counts);

  for (size_t i = 0; i < g->nroles; i++) {
    uint32_t count = 0;

    CILO_MAP_EACH(entry, &g->members) {
      const Member *m = entry->val;

      for (size_t j = 0; j < m->nroles; j++) {
        count += m->roles[j] == g->roles[i]->id;
      }
    }

    a->counts[a->ncounts++] = (AdminRoleCount){g->roles[i]->id, count};
  }

  changed(s);
}

uint32_t admin_count(const Admin *a, Snowflake role_id) {
  for (size_t i = 0; i < a->ncounts; i++) {
    if (a->counts[i].role_id == role_id) {
      return a->counts[i].count;
    }
  }

  return 0;
}

void admin_fetch_members(Session *s, Snowflake guild_id, Snowflake role_id) {
  Admin *a = admin_of(s, guild_id);
  char path[96];

  if (a->members_role != role_id) {
    free(a->members);
    a->members = nullptr;
    a->nmembers = 0;
  }

  a->members_role = role_id;

  if (can_send(s, nullptr)) {
    Request *r = request_new(s, REQ_MEMBERS, guild_id);

    r->id = role_id;
    a->members_loading = true;
    snprintf(path, sizeof path,
             "/guilds/%" PRIu64 "/roles/%" PRIu64 "/member-ids", guild_id,
             role_id);
    send(s, r, "GET", path, "/guilds/roles/member-ids", nullptr);
    return;
  }

  Guild *g = store_guild(s->store, guild_id);
  size_t cap = 0;

  if (g == nullptr) {
    return;
  }

  free(a->members);
  a->members = nullptr;
  a->nmembers = 0;

  CILO_MAP_EACH(entry, &g->members) {
    const Member *m = entry->val;

    for (size_t j = 0; j < m->nroles; j++) {
      if (m->roles[j] != role_id) {
        continue;
      }

      if (a->nmembers == cap) {
        cap = cap ? cap * 2 : 16;
        a->members = xrealloc(a->members, cap * sizeof *a->members);
      }

      a->members[a->nmembers++] = m->user_id;
    }
  }

  changed(s);
}

// a member's roles with `role_id` added or taken out, as the event that
// says so.
static void member_roles_event(Session *s, Snowflake guild_id,
                               Snowflake user_id, Snowflake role_id,
                               bool add) {
  const Guild *g = store_guild(s->store, guild_id);
  const Member *m = g != nullptr ? store_member(g, user_id) : nullptr;
  cima_json_writer w = {};

  if (m == nullptr) {
    return;
  }

  json_begin_object(&w);
  json_key(&w, "guild_id");
  snowflake_write(&w, guild_id);
  json_key(&w, "user_id");
  snowflake_write(&w, user_id);
  json_key(&w, "roles");
  json_begin_array(&w);

  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i] != role_id) {
      snowflake_write(&w, m->roles[i]);
    }
  }

  if (add) {
    snowflake_write(&w, role_id);
  }

  json_end_array(&w);
  json_end_object(&w);
  apply_event(s, "GUILD_MEMBER_UPDATE", cima_json_writer_text(&w));
  cima_json_writer_dispose(&w);
}

void admin_add_members(Session *s, Snowflake guild_id, Snowflake role_id,
                       const Snowflake *user_ids, size_t count) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[96];

  if (count == 0) {
    return;
  }

  json_begin_object(&w);
  json_key(&w, "member_ids");
  json_begin_array(&w);

  for (size_t i = 0; i < count; i++) {
    snowflake_write(&w, user_ids[i]);
  }

  json_end_array(&w);
  json_end_object(&w);

  if (can_send(s, &a->action)) {
    Request *r = request_new(s, REQ_ADD_MEMBERS, guild_id);

    r->id = role_id;
    snprintf(path, sizeof path,
             "/guilds/%" PRIu64 "/roles/%" PRIu64 "/members", guild_id,
             role_id);
    send(s, r, "PATCH", path, "/guilds/roles/members",
         cima_json_writer_text(&w));
  }
  else if (s->offline) {
    for (size_t i = 0; i < count; i++) {
      member_roles_event(s, guild_id, user_ids[i], role_id, true);
    }

    admin_fetch_members(s, guild_id, role_id);
    admin_fetch_counts(s, guild_id);
    offline_done(s, &a->action, 0);
  }

  cima_json_writer_dispose(&w);
}

void admin_remove_member(Session *s, Snowflake guild_id, Snowflake role_id,
                         Snowflake user_id) {
  Admin *a = admin_of(s, guild_id);
  const Guild *g = store_guild(s->store, guild_id);
  const Member *m = g != nullptr ? store_member(g, user_id) : nullptr;
  char path[96];

  if (can_send(s, &a->action)) {
    Request *r = request_new(s, REQ_REMOVE_MEMBER, guild_id);
    cima_json_writer w = {};

    // discord's client sends the roles the member keeps.
    json_begin_object(&w);
    json_key(&w, "roles");
    json_begin_array(&w);

    for (size_t i = 0; m != nullptr && i < m->nroles; i++) {
      if (m->roles[i] != role_id) {
        snowflake_write(&w, m->roles[i]);
      }
    }

    json_end_array(&w);
    json_end_object(&w);

    r->id = role_id;
    r->user_id = user_id;

    // without the member, the one route that needs nothing else.
    if (m == nullptr) {
      snprintf(path, sizeof path,
               "/guilds/%" PRIu64 "/members/%" PRIu64 "/roles/%" PRIu64,
               guild_id, user_id, role_id);
      send(s, r, "DELETE", path, "/guilds/members/roles", nullptr);
    }
    else {
      snprintf(path, sizeof path, "/guilds/%" PRIu64 "/members/%" PRIu64,
               guild_id, user_id);
      send(s, r, "PATCH", path, "/guilds/members", cima_json_writer_text(&w));
    }

    cima_json_writer_dispose(&w);
  }
  else if (s->offline) {
    member_roles_event(s, guild_id, user_id, role_id, false);

    if (a->members_role == role_id) {
      drop_member(a, user_id);
    }

    admin_fetch_counts(s, guild_id);
    offline_done(s, &a->action, 0);
  }
}

void admin_search_members(Session *s, Snowflake guild_id, const char *query) {
  if (online(s) && s->gateway != nullptr) {
    gateway_search_members(s->gateway, guild_id, query ? query : "", 10,
                           false);
  }
}

/* ========================================================================== */
/* channels.                                                                  */
/* ========================================================================== */

void admin_channel_slug(const char *name, char *out, size_t cap) {
  // discord's client: runs of spaces, dashes and tildes become one dash,
  // leading dashes go, so does punctuation, and the rest is lowered.
  static const char DROPPED[] = "\\'!\"#$%&()*+,./:;<=>?@[]^`{|}~";
  size_t n = 0;
  bool dash = false;

  if (cap == 0) {
    return;
  }

  for (const unsigned char *p = (const unsigned char *)name;
       p != nullptr && *p && n + 1 < cap; p++) {
    unsigned char c = *p;

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-' ||
        c == '~') {
      dash = true;
      continue;
    }

    if (dash && n != 0 && n + 2 < cap) {
      out[n++] = '-';
    }

    dash = false;

    if (c < 0x80 && strchr(DROPPED, c) != nullptr) {
      continue;
    }

    out[n++] = c < 0x80 ? (char)tolower(c) : (char)c;
  }

  // a dash typed last stays while the name is being typed.
  if (dash && n != 0 && n + 1 < cap) {
    out[n++] = '-';
  }

  out[n] = 0;
}

static void write_overwrite(cima_json_writer *w, Snowflake id,
                            OverwriteType type, Permissions allow,
                            Permissions deny) {
  json_begin_object(w);
  json_key(w, "id");
  snowflake_write(w, id);
  json_field(w, "type", (int64_t)type);
  write_permissions(w, "allow", allow);
  write_permissions(w, "deny", deny);
  json_end_object(w);
}

// what discord's client sends from its dialog: a private voice channel or
// category hides from @everyone and keeps it from connecting, an
// announcement channel only lets it read, and whoever makes a private
// channel keeps seeing it.
static void write_channel_create(cima_json_writer *w, Snowflake guild_id,
                                 const ChannelCreate *c, Snowflake self) {
  bool closed = c->type == CHANNEL_GUILD_VOICE ||
                c->type == CHANNEL_GUILD_CATEGORY;
  Permissions seen = PERM_VIEW_CHANNEL | (closed ? PERM_CONNECT : 0);
  bool private = c->private && c->type != CHANNEL_GUILD_ANNOUNCEMENT &&
                 c->type != CHANNEL_GUILD_STAGE;

  json_field(w, "type", (int64_t)c->type);
  json_field(w, "name", c->name);
  json_key(w, "permission_overwrites");
  json_begin_array(w);

  if (c->type == CHANNEL_GUILD_ANNOUNCEMENT) {
    write_overwrite(w, guild_id, OVERWRITE_ROLE, 0, PERM_SEND_MESSAGES);
  }

  if (private) {
    write_overwrite(w, guild_id, OVERWRITE_ROLE, 0, seen);

    for (size_t i = 0; i < c->nroles; i++) {
      write_overwrite(w, c->roles[i], OVERWRITE_ROLE, seen, 0);
    }

    for (size_t i = 0; i < c->nusers; i++) {
      write_overwrite(w, c->users[i], OVERWRITE_MEMBER, seen, 0);
    }

    if (self != 0) {
      write_overwrite(w, self, OVERWRITE_MEMBER, seen, 0);
    }
  }

  json_end_array(w);

  if (c->parent_id != 0 && c->type != CHANNEL_GUILD_CATEGORY) {
    json_key(w, "parent_id");
    snowflake_write(w, c->parent_id);
  }
}

// the current user, when a private channel would hide from them: neither
// owner nor administrator, without any role picked, and not picked
// themselves.
static Snowflake creator_kept(Session *s, Snowflake guild_id,
                              const ChannelCreate *c) {
  Store *store = s->store;
  const Guild *g = store_guild(store, guild_id);
  const Member *me = g != nullptr ? store_member(g, store->self_id) : nullptr;

  if (!c->private || g == nullptr || g->owner_id == store->self_id ||
      (store_guild_permissions(store, g) & PERM_ADMINISTRATOR)) {
    return 0;
  }

  for (size_t i = 0; i < c->nusers; i++) {
    if (c->users[i] == store->self_id) {
      return 0;
    }
  }

  for (size_t i = 0; me != nullptr && i < me->nroles; i++) {
    for (size_t j = 0; j < c->nroles; j++) {
      if (me->roles[i] == c->roles[j]) {
        return 0;
      }
    }
  }

  return store->self_id;
}

void admin_create_channel(Session *s, Snowflake guild_id,
                          const ChannelCreate *create) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[64];

  Snowflake self = creator_kept(s, guild_id, create);

  json_begin_object(&w);
  write_channel_create(&w, guild_id, create, self);
  json_end_object(&w);

  if (can_send(s, &a->channel)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/channels", guild_id);
    send(s, request_new(s, REQ_CREATE_CHANNEL, guild_id), "POST", path,
         nullptr, cima_json_writer_text(&w));
  }
  else if (s->offline) {
    Guild *g = store_guild(s->store, guild_id);
    cima_json_writer event = {};
    Snowflake id = made_id(s);
    int32_t last = 0;
    bool category = create->type == CHANNEL_GUILD_CATEGORY;

    // it goes last among the channels of its kind.
    for (size_t i = 0; g != nullptr && i < g->nchannels; i++) {
      const Channel *c = g->channels[i];

      if ((c->type == CHANNEL_GUILD_CATEGORY) == category &&
          c->position > last) {
        last = c->position;
      }
    }

    json_begin_object(&event);
    json_key(&event, "id");
    snowflake_write(&event, id);
    json_key(&event, "guild_id");
    snowflake_write(&event, guild_id);
    json_field(&event, "position", (int64_t)last + 1);
    write_channel_create(&event, guild_id, create, self);
    json_end_object(&event);
    apply_event(s, "CHANNEL_CREATE", cima_json_writer_text(&event));
    cima_json_writer_dispose(&event);
    offline_done(s, &a->channel, id);
  }

  cima_json_writer_dispose(&w);
}

void admin_delete_channel(Session *s, Snowflake guild_id,
                          Snowflake channel_id) {
  Admin *a = admin_of(s, guild_id);
  char path[64];

  if (can_send(s, &a->channel)) {
    Request *r = request_new(s, REQ_DELETE_CHANNEL, guild_id);

    r->id = channel_id;
    snprintf(path, sizeof path, "/channels/%" PRIu64, channel_id);
    send(s, r, "DELETE", path, nullptr, nullptr);
  }
  else if (s->offline) {
    char data[64];

    snprintf(data, sizeof data, "{\"id\":\"%" PRIu64 "\"}", channel_id);
    apply_event(s, "CHANNEL_DELETE", data);
    offline_done(s, &a->channel, 0);
  }
}

void admin_rename_channel(Session *s, Snowflake guild_id, Snowflake channel_id,
                          const char *name) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[64];

  json_begin_object(&w);
  json_field(&w, "name", name);
  json_end_object(&w);

  if (can_send(s, &a->channel)) {
    Request *r = request_new(s, REQ_RENAME_CHANNEL, guild_id);

    r->id = channel_id;
    snprintf(path, sizeof path, "/channels/%" PRIu64, channel_id);
    send(s, r, "PATCH", path, nullptr, cima_json_writer_text(&w));
  }
  else if (s->offline) {
    cima_json_writer event = {};

    json_begin_object(&event);
    json_key(&event, "id");
    snowflake_write(&event, channel_id);
    json_field(&event, "name", name);
    json_end_object(&event);
    apply_event(s, "CHANNEL_UPDATE", cima_json_writer_text(&event));
    cima_json_writer_dispose(&event);
    offline_done(s, &a->channel, channel_id);
  }

  cima_json_writer_dispose(&w);
}

// a CHANNEL_UPDATE of a channel's place.
static void place_event(cima_json_writer *w, Snowflake id, int32_t position,
                        Snowflake parent_id) {
  json_begin_object(w);
  json_field(w, "t", "CHANNEL_UPDATE");
  json_key(w, "d");
  json_begin_object(w);
  json_key(w, "id");
  snowflake_write(w, id);
  json_field(w, "position", (int64_t)position);
  write_id_or_null(w, "parent_id", parent_id);
  json_end_object(w);
  json_end_object(w);
}

void admin_move_channels(Session *s, Snowflake guild_id,
                         const ChannelPlace *places, size_t count) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer apply = {}, undo = {}, body = {};

  if (count == 0) {
    return;
  }

  json_begin_array(&apply);
  json_begin_array(&undo);
  json_begin_array(&body);

  for (size_t i = 0; i < count; i++) {
    const ChannelPlace *p = &places[i];
    const Channel *c = store_channel(s->store, p->id);

    if (c == nullptr) {
      continue;
    }

    place_event(&apply, p->id, p->position, p->parent_id);
    place_event(&undo, p->id, c->position, c->parent_id);

    json_begin_object(&body);
    json_key(&body, "id");
    snowflake_write(&body, p->id);
    json_field(&body, "position", (int64_t)p->position);

    if (p->moved) {
      write_id_or_null(&body, "parent_id", p->parent_id);
      json_field(&body, "lock_permissions", p->lock_permissions);
    }

    json_end_object(&body);
  }

  json_end_array(&apply);
  json_end_array(&undo);
  json_end_array(&body);

  // the list shows the move at once, as discord's does.
  apply_events(s, cima_json_writer_text(&apply));

  if (can_send(s, &a->move)) {
    Request *r = request_new(s, REQ_MOVE_CHANNELS, guild_id);
    char path[64];

    r->undo = str_dup(cima_json_writer_text(&undo));
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/channels", guild_id);
    send(s, r, "PATCH", path, nullptr, cima_json_writer_text(&body));
  }
  else if (s->offline) {
    offline_done(s, &a->move, 0);
  }
  else {
    apply_events(s, cima_json_writer_text(&undo));
  }

  cima_json_writer_dispose(&apply);
  cima_json_writer_dispose(&undo);
  cima_json_writer_dispose(&body);
}

/* ========================================================================== */
/* moderation.                                                                */
/* ========================================================================== */

void admin_fetch_bans(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  char path[80];

  if (a->bans_loading) {
    return;
  }

  if (can_send(s, nullptr)) {
    a->bans_loading = true;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/bans?limit=1000",
             guild_id);
    send(s, request_new(s, REQ_BANS, guild_id), "GET", path, "/guilds/bans",
         nullptr);
  }
  else {
    a->bans_loaded = true;
    changed(s);
  }
}

void admin_unban(Session *s, Snowflake guild_id, Snowflake user_id) {
  Admin *a = admin_of(s, guild_id);
  char path[96];

  if (can_send(s, &a->action)) {
    Request *r = request_new(s, REQ_UNBAN, guild_id);

    r->user_id = user_id;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/bans/%" PRIu64, guild_id,
             user_id);
    send(s, r, "DELETE", path, "/guilds/bans", nullptr);
  }
  else if (s->offline) {
    drop_ban(a, user_id);
    offline_done(s, &a->action, 0);
  }
}

void admin_fetch_invites(Session *s, Snowflake guild_id) {
  Admin *a = admin_of(s, guild_id);
  char path[64];

  if (a->invites_loading) {
    return;
  }

  if (can_send(s, nullptr)) {
    a->invites_loading = true;
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/invites", guild_id);
    send(s, request_new(s, REQ_INVITES, guild_id), "GET", path, nullptr,
         nullptr);
  }
  else {
    a->invites_loaded = true;
    changed(s);
  }
}

void admin_revoke_invite(Session *s, Snowflake guild_id, const char *code) {
  Admin *a = admin_of(s, guild_id);
  char path[160];

  if (code == nullptr) {
    return;
  }

  if (can_send(s, &a->action)) {
    Request *r = request_new(s, REQ_REVOKE, guild_id);
    char *encoded = url_encode(code);

    r->code = str_dup(code);
    snprintf(path, sizeof path, "/invites/%s", encoded);
    free(encoded);
    send(s, r, "DELETE", path, "/invites/", nullptr);
  }
  else if (s->offline) {
    drop_invite(a, code);
    offline_done(s, &a->action, 0);
  }
}

/* ========================================================================== */
/* emojis.                                                                    */
/* ========================================================================== */

// GUILD_EMOJIS_UPDATE with the guild's emojis, `skip` left out, renamed or
// added.
static void emojis_event(Session *s, const Guild *g, Snowflake skip,
                         Snowflake renamed, const char *name,
                         Snowflake added) {
  cima_json_writer w = {};

  json_begin_object(&w);
  json_key(&w, "guild_id");
  snowflake_write(&w, g->id);
  json_key(&w, "emojis");
  json_begin_array(&w);

  for (size_t i = 0; i < g->nemojis; i++) {
    const Emoji *e = &g->emojis[i];

    if (e->id == skip) {
      continue;
    }

    json_begin_object(&w);
    json_key(&w, "id");
    snowflake_write(&w, e->id);
    json_field(&w, "name", e->id == renamed ? name : e->name);
    json_field(&w, "animated", e->animated);
    json_field(&w, "available", e->available);
    json_end_object(&w);
  }

  if (added != 0) {
    json_begin_object(&w);
    json_key(&w, "id");
    snowflake_write(&w, added);
    json_field(&w, "name", name);
    json_end_object(&w);
  }

  json_end_array(&w);
  json_end_object(&w);
  apply_event(s, "GUILD_EMOJIS_UPDATE", cima_json_writer_text(&w));
  cima_json_writer_dispose(&w);
}

void admin_create_emoji(Session *s, Snowflake guild_id, const char *name,
                        const char *image) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[64];

  json_begin_object(&w);
  json_field(&w, "name", name);
  json_field(&w, "image", image);
  json_end_object(&w);

  if (can_send(s, &a->action)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/emojis", guild_id);
    send(s, request_new(s, REQ_EMOJI, guild_id), "POST", path, nullptr,
         cima_json_writer_text(&w));
  }
  else if (s->offline) {
    const Guild *g = store_guild(s->store, guild_id);

    if (g != nullptr) {
      emojis_event(s, g, 0, 0, name, made_id(s));
    }

    offline_done(s, &a->action, 0);
  }

  cima_json_writer_dispose(&w);
}

void admin_rename_emoji(Session *s, Snowflake guild_id, Snowflake emoji_id,
                        const char *name) {
  Admin *a = admin_of(s, guild_id);
  cima_json_writer w = {};
  char path[96];

  json_begin_object(&w);
  json_field(&w, "name", name);
  json_end_object(&w);

  if (can_send(s, &a->action)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/emojis/%" PRIu64,
             guild_id, emoji_id);
    send(s, request_new(s, REQ_EMOJI, guild_id), "PATCH", path,
         "/guilds/emojis", cima_json_writer_text(&w));
  }
  else if (s->offline) {
    const Guild *g = store_guild(s->store, guild_id);

    if (g != nullptr) {
      emojis_event(s, g, 0, emoji_id, name, 0);
    }

    offline_done(s, &a->action, 0);
  }

  cima_json_writer_dispose(&w);
}

void admin_delete_emoji(Session *s, Snowflake guild_id, Snowflake emoji_id) {
  Admin *a = admin_of(s, guild_id);
  char path[96];

  if (can_send(s, &a->action)) {
    snprintf(path, sizeof path, "/guilds/%" PRIu64 "/emojis/%" PRIu64,
             guild_id, emoji_id);
    send(s, request_new(s, REQ_EMOJI, guild_id), "DELETE", path,
         "/guilds/emojis", nullptr);
  }
  else if (s->offline) {
    const Guild *g = store_guild(s->store, guild_id);

    if (g != nullptr) {
      emojis_event(s, g, emoji_id, 0, nullptr, 0);
    }

    offline_done(s, &a->action, 0);
  }
}

/* ========================================================================== */
/* images.                                                                    */
/* ========================================================================== */

// the mime type of an image by its first bytes, null for none discord
// takes.
static const char *image_mime(const uint8_t *b, size_t n) {
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return "image/png";
  }

  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
    return "image/jpeg";
  }

  if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) {
    return "image/gif";
  }

  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) {
    return "image/webp";
  }

  return nullptr;
}

char *admin_image_uri(const char *path, size_t most, char **error) {
  FILE *file = path != nullptr ? fopen(path, "rb") : nullptr;

  if (file == nullptr) {
    str_set(error, "The file could not be read.");
    return nullptr;
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size <= 0 || (size_t)size > most) {
    char line[96];

    fclose(file);
    snprintf(line, sizeof line, "The file is too large, the most is %zu KB.",
             most / 1024);
    str_set(error, size <= 0 ? "The file is empty." : line);
    return nullptr;
  }

  uint8_t *bytes = xcalloc(1, (size_t)size);
  size_t read = fread(bytes, 1, (size_t)size, file);

  fclose(file);

  const char *mime = image_mime(bytes, read);

  if (mime == nullptr) {
    free(bytes);
    str_set(error, "Only PNG, JPEG, GIF and WebP images are accepted.");
    return nullptr;
  }

  char *encoded = base64_encode(bytes, read);
  size_t len = strlen(mime) + strlen(encoded) + 16;
  char *uri = xcalloc(1, len);

  snprintf(uri, len, "data:%s;base64,%s", mime, encoded);
  free(encoded);
  free(bytes);
  return uri;
}
