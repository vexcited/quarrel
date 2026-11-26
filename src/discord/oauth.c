#include "discord/oauth.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the public flag of a bot discord verified.
constexpr uint64_t VERIFIED_BOT = 1ull << 16;
// discord's ids of what an application discloses.
constexpr int32_t DISCLOSES_IP = 1;
constexpr int32_t DISCLOSES_ADS = 2;

typedef enum : uint8_t {
  ASK_DISCLOSURES,
  ASK_PREVIEW,
  ASK_ANSWER,
  // answers nobody waits for: a refusal, and the disclosures read.
  ASK_QUIET,
} Ask;

// a request in flight. they are only answered while the api lives, so they
// are freed with the asking, never after.
typedef struct OauthTicket {
  struct OauthTicket *next;
  Oauth *o;
  Ask ask;
  uint64_t seq;
  bool authorize;
} OauthTicket;

static void changed(Oauth *o) {
  o->version++;

  if (o->on_change != nullptr) {
    o->on_change();
  }
}

static OauthTicket *ticket_new(Oauth *o, Ask ask) {
  OauthTicket *t = calloc(1, sizeof *t);

  if (t == nullptr) {
    cilo_oom(sizeof *t);
  }

  *t = (OauthTicket){.next = o->tickets, .o = o, .ask = ask, .seq = o->seq};
  o->tickets = t;
  return t;
}

static void ticket_end(OauthTicket *t) {
  for (OauthTicket **at = &t->o->tickets; *at != nullptr; at = &(*at)->next) {
    if (*at == t) {
      *at = t->next;
      break;
    }
  }

  free(t);
}

static void free_captcha(OauthCaptcha *c) {
  free(c->service);
  free(c->sitekey);
  free(c->rqdata);
  free(c->session_id);
  free(c->rqtoken);
  *c = (OauthCaptcha){};
}

// forgets what was asked and drops the answers on their way.
static void reset(Oauth *o) {
  for (size_t i = 0; i < o->nguilds; i++) {
    free(o->guilds[i].name);
    free(o->guilds[i].icon);
  }

  free(o->scopes);
  free(o->error);
  free(o->name);
  free(o->icon);
  free(o->terms_url);
  free(o->privacy_url);
  free(o->guilds);
  free(o->redirect);
  free_captcha(&o->captcha);

  o->seq++;
  o->client_id = 0;
  o->scopes = nullptr;
  o->install = OAUTH_GUILD;
  o->permissions = 0;
  o->loading = false;
  o->ready = false;
  o->error = nullptr;
  o->name = nullptr;
  o->icon = nullptr;
  o->terms_url = nullptr;
  o->privacy_url = nullptr;
  o->bot_id = 0;
  o->bot_verified = false;
  o->guild_count = 0;
  o->guilds = nullptr;
  o->nguilds = 0;
  o->sees_ip = false;
  o->shows_ads = false;
  o->sending = false;
  o->done = false;
  o->guild_id = 0;
  o->redirect = nullptr;
  o->granted = 0;
  o->ndisclosures = 0;
  o->disclosed = false;
  o->previewed = false;
}

bool oauth_has_scope(const char *scopes, const char *scope) {
  size_t len = strlen(scope);

  for (const char *at = scopes; at != nullptr && *at != '\0';) {
    size_t word = strcspn(at, " ");

    if (word == len && strncmp(at, scope, len) == 0) {
      return true;
    }

    at += word;
    at += *at == ' ';
  }

  return false;
}

static const char *string_at(const cima_json *v, const char *key) {
  return json_get_or(v, key, (const char *)nullptr);
}

// { application, user, authorized, bot?, guilds? }.
static void take_preview(Oauth *o, const cima_json *json) {
  const cima_json *app = json_get(json, "application");
  const cima_json *bot = json_get(json, "bot");
  const cima_json *guilds = json_get(json, "guilds");
  const cima_json *count = json_get(app, "approximate_guild_count");
  const char *name = string_at(app, "name");
  size_t nguilds = json_type(guilds) == CIMA_JSON_ARRAY ? json_len(guilds) : 0;

  str_set(&o->name, name != nullptr ? name : "");
  str_set(&o->icon, string_at(app, "icon"));
  str_set(&o->terms_url, string_at(app, "terms_of_service_url"));
  str_set(&o->privacy_url, string_at(app, "privacy_policy_url"));
  o->bot_id = snowflake_of(json_get(bot, "id"));
  o->bot_verified =
    (json_get_coerce(bot, "public_flags", (uint64_t)0) & VERIFIED_BOT) != 0;

  // the application's count, its bot's when it has none.
  if (json_type(count) == CIMA_JSON_NULL) {
    count = json_get(bot, "approximate_guild_count");
  }

  o->guild_count = (uint32_t)json_coerce(count, (uint64_t)0);

  if (nguilds == 0) {
    return;
  }

  o->guilds = calloc(nguilds, sizeof *o->guilds);

  if (o->guilds == nullptr) {
    cilo_oom(nguilds * sizeof *o->guilds);
  }

  CIMA_EACH(guild, json_items(guilds)) {
    const char *named = string_at(guild, "name");
    Snowflake id = snowflake_of(json_get(guild, "id"));

    if (id != 0) {
      o->guilds[o->nguilds++] = (OauthGuild){
        .id = id,
        .name = str_dup(named != nullptr ? named : ""),
        .icon = str_dup(string_at(guild, "icon")),
        .permissions = snowflake_of(json_get(guild, "permissions")),
      };
    }
  }
}

// the value of `name` in the query of `url`, its escapes undone. null when
// it has none. from malloc.
static char *query_value(const char *url, const char *name) {
  const char *at = strchr(url, '?');
  size_t len = strlen(name);

  while (at != nullptr) {
    at++;

    if (strncmp(at, name, len) == 0 && at[len] == '=') {
      at += len + 1;

      size_t end = strcspn(at, "&#");
      char *out = calloc(end + 1, 1);
      size_t n = 0;

      if (out == nullptr) {
        cilo_oom(end + 1);
      }

      for (size_t i = 0; i < end; i++) {
        unsigned hex = 0;

        if (at[i] == '%' && i + 2 < end + 1 &&
            sscanf(at + i + 1, "%2x", &hex) == 1) {
          out[n++] = (char)hex;
          i += 2;
        }
        else {
          out[n++] = at[i] == '+' ? ' ' : at[i];
        }
      }

      return out;
    }

    at = strchr(at, '&');
  }

  return nullptr;
}

// the path of an url of discord's, null for one of another site.
static const char *discord_path(const char *url) {
  static const char *const hosts[] = {
    "https://discord.com",
    "https://canary.discord.com",
    "https://ptb.discord.com",
  };

  for (size_t i = 0; i < sizeof hosts / sizeof *hosts; i++) {
    size_t len = strlen(hosts[i]);

    if (strncmp(url, hosts[i], len) == 0 &&
        (url[len] == '/' || url[len] == '\0')) {
      return url + len;
    }
  }

  return nullptr;
}

static void ack_disclosures(Oauth *o);

static void on_answer(void *ctx, const ApiResponse *res) {
  OauthTicket *t = ctx;
  OauthTicket asked = *t;
  Oauth *o = t->o;

  ticket_end(t);

  // something else was asked since.
  if (asked.ask == ASK_QUIET || asked.seq != o->seq) {
    return;
  }

  if (asked.ask == ASK_DISCLOSURES) {
    // { disclosures: [1], acked_disclosures, all_acked }.
    if (res->ok) {
      CIMA_EACH(disclosure, json_items(json_get(res->json, "disclosures"))) {
        int32_t id = (int32_t)json_coerce(disclosure, (int64_t)0);

        if (o->ndisclosures < sizeof o->disclosures / sizeof *o->disclosures) {
          o->disclosures[o->ndisclosures++] = id;
        }

        o->sees_ip |= id == DISCLOSES_IP;
        o->shows_ads |= id == DISCLOSES_ADS;
      }

      o->disclosed = true;
    }
    else if (o->error == nullptr) {
      str_set(&o->error, res->error.message);
    }
  }
  else if (asked.ask == ASK_PREVIEW) {
    if (res->ok) {
      take_preview(o, res->json);
      o->previewed = true;
    }
    else if (o->error == nullptr) {
      str_set(&o->error, res->error.message);
    }
  }

  if (asked.ask != ASK_ANSWER) {
    o->ready = o->disclosed && o->previewed;
    o->loading = !o->ready && o->error == nullptr;
    changed(o);
    return;
  }

  o->sending = false;

  if (res->captcha != nullptr) {
    free_captcha(&o->captcha);
    o->captcha = (OauthCaptcha){
      .service = str_dup(res->captcha->service),
      .sitekey = str_dup(res->captcha->sitekey),
      .rqdata = str_dup(res->captcha->rqdata),
      .session_id = str_dup(res->captcha->session_id),
      .rqtoken = str_dup(res->captcha->rqtoken),
    };
    changed(o);
    return;
  }

  const char *location = res->ok ? string_at(res->json, "location") : nullptr;
  const char *path = location != nullptr ? discord_path(location) : nullptr;

  if (!res->ok) {
    str_set(&o->error, res->error.message);
  }
  else if (path != nullptr && strncmp(path, "/oauth2/error", 13) == 0) {
    // discord answers its refusals as a page of its own.
    char *why = query_value(location, "error_description");

    if (why == nullptr) {
      why = query_value(location, "error");
    }

    str_set(&o->error, why != nullptr ? why : "Oops, something went wrong...");
    free(why);
  }
  else {
    // its page that says so, or the one the application's makers set.
    bool own = path != nullptr && strncmp(path, "/oauth2/authorized", 18) == 0;

    o->done = true;
    str_set(&o->redirect, own ? nullptr : location);
    ack_disclosures(o);
  }

  changed(o);
}

static void get(Oauth *o, const char *path, const char *route, Ask ask) {
  api_request(o->api,
              &(ApiRequest){.method = "GET", .path = path, .route = route},
              on_answer, ticket_new(o, ask));
}

// tells discord what was disclosed was read, as the client does once the
// application is let in.
static void ack_disclosures(Oauth *o) {
  char path[96], body[160];
  int n = snprintf(body, sizeof body, "{\"disclosures\":[");

  for (size_t i = 0; i < o->ndisclosures; i++) {
    n += snprintf(body + n, sizeof body - (size_t)n, "%s%d", i ? "," : "",
                  o->disclosures[i]);
  }

  snprintf(body + n, sizeof body - (size_t)n, "]}");
  snprintf(path, sizeof path, "/applications/%" PRIu64 "/disclosures",
           o->client_id);
  api_request(o->api,
              &(ApiRequest){.method = "POST",
                            .path = path,
                            .route = "/applications/:id/disclosures",
                            .body = body},
              on_answer, ticket_new(o, ASK_QUIET));
}

Oauth *oauth_new(Api *api, void (*on_change)(void)) {
  Oauth *o = calloc(1, sizeof *o);

  if (o == nullptr) {
    cilo_oom(sizeof *o);
  }

  o->api = api;
  o->on_change = on_change;
  return o;
}

void oauth_free(Oauth *o) {
  if (o == nullptr) {
    return;
  }

  // the api is freed first, nothing of it answers anymore.
  for (OauthTicket *t = o->tickets; t != nullptr;) {
    OauthTicket *next = t->next;

    free(t);
    t = next;
  }

  reset(o);
  free(o);
}

void oauth_ask(Oauth *o, Snowflake client_id, const char *scopes,
               OauthInstall install, Permissions permissions) {
  reset(o);

  if (client_id == 0 || scopes == nullptr || *scopes == '\0') {
    return;
  }

  o->client_id = client_id;
  o->scopes = str_dup(scopes);
  o->install = install;
  o->permissions = permissions;
  o->loading = true;

  char path[640];
  char *encoded = url_encode(scopes);

  snprintf(path, sizeof path, "/applications/%" PRIu64 "/disclosures",
           client_id);
  get(o, path, "/applications/:id/disclosures", ASK_DISCLOSURES);

  // the client's query, in its order.
  snprintf(path, sizeof path,
           "/oauth2/authorize?client_id=%" PRIu64 "&scope=%s"
           "&integration_type=%d",
           client_id, encoded, (int)install);
  get(o, path, "/oauth2/authorize", ASK_PREVIEW);
  free(encoded);
  changed(o);
}

static void send(Oauth *o, bool authorize, const char *solution) {
  char path[640], body[320], guild[64] = "";
  char *encoded = url_encode(o->scopes);
  ApiCaptchaSolution solved = {
    .key = solution,
    .session_id = o->captcha.session_id,
    .rqtoken = o->captcha.rqtoken,
  };

  if (o->guild_id != 0) {
    snprintf(guild, sizeof guild, "\"guild_id\":\"%" PRIu64 "\",", o->guild_id);
  }

  snprintf(path, sizeof path,
           "/oauth2/authorize?client_id=%" PRIu64 "&scope=%s", o->client_id,
           encoded);
  free(encoded);
  // the client's body. it names the channel the user is in, which is none
  // on the pages this is asked from: 10000 stands for unknown.
  snprintf(body, sizeof body,
           "{%s\"permissions\":\"%" PRIu64 "\",\"authorize\":%s,"
           "\"integration_type\":%d,\"location_context\":{\"guild_id\":"
           "\"10000\",\"channel_id\":\"10000\",\"channel_type\":10000}}",
           guild, o->granted, authorize ? "true" : "false", (int)o->install);

  OauthTicket *t = ticket_new(o, authorize ? ASK_ANSWER : ASK_QUIET);

  t->authorize = authorize;
  api_request(o->api,
              &(ApiRequest){.method = "POST",
                            .path = path,
                            .route = "/oauth2/authorize",
                            .body = body,
                            .captcha = solution != nullptr ? &solved : nullptr},
              on_answer, t);
}

void oauth_answer(Oauth *o, bool authorize, Snowflake guild_id,
                  Permissions permissions) {
  if (!o->ready || o->sending || o->done) {
    return;
  }

  o->guild_id = o->install == OAUTH_GUILD ? guild_id : 0;
  o->granted = permissions;

  // a refusal is told to discord too, and waits for nothing.
  if (!authorize) {
    send(o, false, nullptr);
    reset(o);
    changed(o);
    return;
  }

  o->sending = true;
  str_set(&o->error, nullptr);
  free_captcha(&o->captcha);
  send(o, true, nullptr);
  changed(o);
}

void oauth_answer_captcha(Oauth *o, const char *solution) {
  if (o->captcha.service == nullptr || o->sending || solution == nullptr) {
    return;
  }

  o->sending = true;
  send(o, true, solution);
  free_captcha(&o->captcha);
  changed(o);
}

void oauth_drop_captcha(Oauth *o) {
  if (o->captcha.service != nullptr) {
    free_captcha(&o->captcha);
    changed(o);
  }
}

void oauth_end(Oauth *o) {
  if (o->client_id != 0) {
    reset(o);
    changed(o);
  }
}
