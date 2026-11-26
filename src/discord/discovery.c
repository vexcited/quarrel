#include "discord/discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// what the client asks for: FEATURED_DEFAULT_LIMIT of the featured servers,
// and a search 12 at a time.
constexpr uint32_t FEATURED_LIMIT = 30;
constexpr uint32_t SEARCH_PAGE = 12;
// a list is asked for again once it is this old, in seconds.
constexpr double FRESH_FOR = 10 * 60;
// the client shows the first 8 categories a search counted.
constexpr size_t COUNTED = 8;

typedef enum : uint8_t {
  ASK_TAB,
  ASK_SEARCH,
  ASK_CATEGORIES,
} Ask;

// a request in flight. they are only answered while the api lives, so they
// are freed with the discovery, never after.
typedef struct DiscoveryTicket {
  struct DiscoveryTicket *next;
  Discovery *d;
  Ask ask;
  DiscoveryTab tab;
  uint64_t seq;
  // the answer goes after the results known, and brings the counts.
  bool more, counts;
} DiscoveryTicket;

static void changed(Discovery *d) {
  d->version++;

  if (d->on_change != nullptr) {
    d->on_change();
  }
}

static DiscoveryTicket *ticket_new(Discovery *d, Ask ask) {
  DiscoveryTicket *t = calloc(1, sizeof *t);

  if (t == nullptr) {
    cilo_oom(sizeof *t);
  }

  *t = (DiscoveryTicket){
    .next = d->tickets,
    .d = d,
    .ask = ask,
    .seq = d->results_seq,
  };
  d->tickets = t;
  return t;
}

static void ticket_end(DiscoveryTicket *t) {
  for (DiscoveryTicket **at = &t->d->tickets; *at != nullptr;
       at = &(*at)->next) {
    if (*at == t) {
      *at = t->next;
      break;
    }
  }

  free(t);
}

static void free_guild(DiscoverableGuild *g) {
  free(g->name);
  free(g->description);
  free(g->icon);
  free(g->splash);
}

static void free_guilds(DiscoveryList *list) {
  for (size_t i = 0; i < list->nguilds; i++) {
    free_guild(&list->guilds[i]);
  }

  free(list->guilds);
  list->guilds = nullptr;
  list->nguilds = 0;
}

static void free_list(DiscoveryList *list) {
  free_guilds(list);
  free(list->error);
  *list = (DiscoveryList){};
}

static bool listed(const DiscoveryList *list, Snowflake id) {
  for (size_t i = 0; i < list->nguilds; i++) {
    if (list->guilds[i].id == id) {
      return true;
    }
  }

  return false;
}

static const char *string_at(const cima_json *v, const char *key) {
  return json_get_or(v, key, (const char *)nullptr);
}

// the discoverable guilds of an answer after the ones of `list`. a search
// may answer a guild twice across its pages.
static void take_guilds(DiscoveryList *list, const cima_json *guilds) {
  size_t more = json_type(guilds) == CIMA_JSON_ARRAY ? json_len(guilds) : 0;

  if (more == 0) {
    return;
  }

  list->guilds =
    realloc(list->guilds, (list->nguilds + more) * sizeof *list->guilds);

  if (list->guilds == nullptr) {
    cilo_oom((list->nguilds + more) * sizeof *list->guilds);
  }

  CIMA_EACH(guild, json_items(guilds)) {
    Snowflake id = snowflake_of(json_get(guild, "id"));
    const char *name = string_at(guild, "name");

    if (id == 0 || listed(list, id)) {
      continue;
    }

    DiscoverableGuild g = {
      .id = id,
      .name = str_dup(name != nullptr ? name : ""),
      .description = str_dup(string_at(guild, "description")),
      .icon = str_dup(string_at(guild, "icon")),
      .splash = str_dup(string_at(guild, "discovery_splash")),
      .member_count = (uint32_t)json_get_coerce(
        guild, "approximate_member_count", (uint64_t)0),
      .presence_count = (uint32_t)json_get_coerce(
        guild, "approximate_presence_count", (uint64_t)0),
    };

    CIMA_EACH(feature, json_items(json_get(guild, "features"))) {
      const char *named = json_or(feature, (const char *)nullptr);

      if (named != nullptr) {
        g.verified |= strcmp(named, "VERIFIED") == 0;
        g.partnered |= strcmp(named, "PARTNERED") == 0;
      }
    }

    list->guilds[list->nguilds++] = g;
  }
}

static void free_categories(Discovery *d) {
  for (size_t i = 0; i < d->ncategories; i++) {
    free(d->categories[i].name);
  }

  free(d->categories);
  d->categories = nullptr;
  d->ncategories = 0;
}

// [{ id, name, is_primary }].
static void take_categories(Discovery *d, const cima_json *json) {
  size_t count = json_type(json) == CIMA_JSON_ARRAY ? json_len(json) : 0;

  free_categories(d);

  if (count == 0) {
    return;
  }

  d->categories = calloc(count, sizeof *d->categories);

  if (d->categories == nullptr) {
    cilo_oom(count * sizeof *d->categories);
  }

  CIMA_EACH(category, json_items(json)) {
    const char *name = string_at(category, "name");

    if (name == nullptr) {
      continue;
    }

    d->categories[d->ncategories++] = (DiscoveryCategory){
      .id = (int32_t)json_get_coerce(category, "id", (int64_t)0),
      .name = str_dup(name),
    };
  }
}

// { guilds, total_count, categories: [{ id, count }] }. the client lists
// the first categories counted, after all of them at once: discord counts
// that one itself, as -1, when it counts anything.
static void take_counts(Discovery *d, const cima_json *json) {
  d->ncounts = 0;
  d->counts[d->ncounts++] = (DiscoveryCount){
    .id = DISCOVERY_ALL,
    .count = d->results.total,
  };

  size_t taken = 0;

  CIMA_EACH(category, json_items(json_get(json, "categories"))) {
    if (taken++ == COUNTED) {
      break;
    }

    int32_t id = (int32_t)json_get_coerce(category, "id", (int64_t)0);
    uint32_t count = (uint32_t)json_get_coerce(category, "count", (uint64_t)0);

    if (id == DISCOVERY_ALL) {
      d->counts[0].count = count;
      continue;
    }

    d->counts[d->ncounts++] = (DiscoveryCount){.id = id, .count = count};
  }
}

static void on_answer(void *ctx, const ApiResponse *res) {
  DiscoveryTicket *t = ctx;
  DiscoveryTicket asked = *t;
  Discovery *d = t->d;
  const char *error = res->ok ? nullptr : res->error.message;

  ticket_end(t);

  if (asked.ask == ASK_CATEGORIES) {
    d->categories_loading = false;
    d->categories_loaded = true;

    if (error == nullptr) {
      take_categories(d, res->json);
    }

    changed(d);
    return;
  }

  if (asked.ask == ASK_TAB) {
    DiscoveryList *list = &d->tabs[asked.tab];

    list->loading = false;
    list->fetched_at = platform_now();
    str_set(&list->error, error);

    // what was listed stays when asking again failed.
    if (error == nullptr) {
      free_guilds(list);
      take_guilds(list, json_get(res->json, "guilds"));
      list->total = (uint32_t)json_get_coerce(res->json, "total", (uint64_t)0);
    }

    changed(d);
    return;
  }

  // another search was made since.
  if (asked.seq != d->results_seq) {
    return;
  }

  DiscoveryList *list = &d->results;

  list->loading = false;
  list->fetched_at = platform_now();
  str_set(&list->error, error);

  if (error == nullptr) {
    if (!asked.more) {
      free_guilds(list);
    }

    size_t before = list->nguilds;

    take_guilds(list, json_get(res->json, "guilds"));
    list->total =
      (uint32_t)json_get_coerce(res->json, "total_count", (uint64_t)0);

    // a page that brought nothing new is the last one.
    if (list->nguilds == before) {
      list->total = (uint32_t)list->nguilds;
    }

    if (asked.counts) {
      take_counts(d, res->json);
    }
  }
  else if (asked.counts) {
    d->ncounts = 0;
  }

  changed(d);
}

Discovery *discovery_new(Api *api, void (*on_change)(void), bool offline) {
  Discovery *d = calloc(1, sizeof *d);
  char uuid[37];

  if (d == nullptr) {
    cilo_oom(sizeof *d);
  }

  d->api = api;
  d->on_change = on_change;
  d->offline = offline;
  d->category = DISCOVERY_ALL;

  // the client's load id is a uuid without its dashes.
  platform_uuid(uuid);

  for (size_t i = 0, at = 0; uuid[i] != '\0' && at < sizeof d->load_id - 1;
       i++) {
    if (uuid[i] != '-') {
      d->load_id[at++] = uuid[i];
    }
  }

  return d;
}

void discovery_free(Discovery *d) {
  if (d == nullptr) {
    return;
  }

  // the api is freed first, nothing of it answers anymore.
  for (DiscoveryTicket *t = d->tickets; t != nullptr;) {
    DiscoveryTicket *next = t->next;

    free(t);
    t = next;
  }

  for (size_t i = 0; i < DISCOVERY_TABS; i++) {
    free_list(&d->tabs[i]);
  }

  free_list(&d->results);
  free_categories(d);
  free(d->query);
  free(d);
}

int32_t discovery_tab_category(DiscoveryTab tab) {
  switch (tab) {
    case DISCOVERY_GAMING:
      return 1;
    case DISCOVERY_MUSIC:
      return 2;
    case DISCOVERY_ENTERTAINMENT:
      return 3;
    case DISCOVERY_TECH:
      return 5;
    case DISCOVERY_EDUCATION:
      return 6;
    default:
      return DISCOVERY_ALL;
  }
}

static bool refuse(Discovery *d, DiscoveryList *list) {
  if (!d->offline) {
    return false;
  }

  list->loading = false;
  list->fetched_at = platform_now();
  str_set(&list->error, "Exploring servers needs a connection.");
  changed(d);
  return true;
}

void discovery_refresh(Discovery *d, DiscoveryTab tab) {
  if (tab < DISCOVERY_TABS) {
    d->tabs[tab].fetched_at = 0;
    discovery_fetch(d, tab);
  }
}

void discovery_fetch(Discovery *d, DiscoveryTab tab) {
  if (tab >= DISCOVERY_TABS) {
    return;
  }

  DiscoveryList *list = &d->tabs[tab];
  // one that failed counts as fresh too, rather than being asked for on
  // every frame.
  bool fresh =
    list->fetched_at != 0 && platform_now() - list->fetched_at < FRESH_FOR;

  if (list->loading || fresh || refuse(d, list)) {
    return;
  }

  char path[96];

  if (tab == DISCOVERY_FEATURED) {
    snprintf(path, sizeof path, "/discoverable-guilds?offset=0&limit=%u",
             FEATURED_LIMIT);
  }
  else {
    snprintf(path, sizeof path, "/discoverable-guilds?categories=%d",
             discovery_tab_category(tab));
  }

  DiscoveryTicket *t = ticket_new(d, ASK_TAB);

  t->tab = tab;
  list->loading = true;
  api_request(d->api,
              &(ApiRequest){
                .method = "GET", .path = path, .route = "/discoverable-guilds"},
              on_answer, t);
  changed(d);
}

// asks for the results from `offset` on, the client's query in its order.
static void ask_results(Discovery *d, uint32_t offset, bool counts) {
  DiscoveryList *list = &d->results;

  if (refuse(d, list)) {
    return;
  }

  char *words = url_encode(d->query);
  char path[640];
  int n =
    snprintf(path, sizeof path, "/discoverable-guilds/search?query=%s", words);

  free(words);

  if (d->category != DISCOVERY_ALL && n > 0 && (size_t)n < sizeof path) {
    n += snprintf(path + n, sizeof path - (size_t)n, "&category_id=%d",
                  d->category);
  }

  if (n > 0 && (size_t)n < sizeof path) {
    n += snprintf(path + n, sizeof path - (size_t)n, "&offset=%u&limit=%u",
                  offset, SEARCH_PAGE);
  }

  if (*d->language && n > 0 && (size_t)n < sizeof path) {
    n += snprintf(path + n, sizeof path - (size_t)n, "&language_code=%s",
                  d->language);
  }

  if (n > 0 && (size_t)n < sizeof path) {
    snprintf(path + n, sizeof path - (size_t)n, "&with_counts=%s",
             counts ? "true" : "false");
  }

  DiscoveryTicket *t = ticket_new(d, ASK_SEARCH);

  t->more = offset != 0;
  t->counts = counts;
  list->loading = true;
  str_set(&list->error, nullptr);
  api_request(d->api,
              &(ApiRequest){.method = "GET",
                            .path = path,
                            .route = "/discoverable-guilds/search"},
              on_answer, t);
}

// forgets what was found, and drops the answers on their way.
static void restart(Discovery *d) {
  d->results_seq++;
  free_list(&d->results);
}

void discovery_search(Discovery *d, const char *query) {
  while (query != nullptr && *query == ' ') {
    query++;
  }

  size_t len = query != nullptr ? strlen(query) : 0;

  while (len != 0 && query[len - 1] == ' ') {
    len--;
  }

  if (len == 0) {
    if (d->query != nullptr) {
      restart(d);
      str_set(&d->query, nullptr);
      d->category = DISCOVERY_ALL;
      d->ncounts = 0;
      changed(d);
    }

    return;
  }

  // discord takes 100 characters.
  char *wanted = str_dup_n(query, len > 100 ? 100 : len);

  if (d->query != nullptr && strcmp(d->query, wanted) == 0 &&
      (d->results.loading || d->results.error == nullptr)) {
    free(wanted);
    return;
  }

  restart(d);
  free(d->query);
  d->query = wanted;
  d->category = DISCOVERY_ALL;
  d->ncounts = 0;
  ask_results(d, 0, true);
  changed(d);
}

void discovery_search_category(Discovery *d, int32_t category) {
  if (d->query == nullptr || d->category == category) {
    return;
  }

  restart(d);
  d->category = category;
  ask_results(d, 0, false);
  changed(d);
}

void discovery_search_language(Discovery *d, const char *language) {
  language = language != nullptr ? language : "";

  if (strcmp(d->language, language) == 0 ||
      strlen(language) >= sizeof d->language) {
    return;
  }

  snprintf(d->language, sizeof d->language, "%s", language);

  if (d->query != nullptr) {
    // the counts are of the language too.
    restart(d);
    d->category = DISCOVERY_ALL;
    d->ncounts = 0;
    ask_results(d, 0, true);
  }

  changed(d);
}

void discovery_search_more(Discovery *d) {
  const DiscoveryList *list = &d->results;

  if (d->query == nullptr || list->loading || list->error != nullptr ||
      list->fetched_at == 0 || list->nguilds >= list->total) {
    return;
  }

  ask_results(d, (uint32_t)list->nguilds, false);
  changed(d);
}

void discovery_fetch_categories(Discovery *d) {
  if (d->categories_loading || d->categories_loaded) {
    return;
  }

  if (d->offline) {
    d->categories_loaded = true;
    return;
  }

  d->categories_loading = true;
  api_request(d->api,
              &(ApiRequest){
                .method = "GET",
                .path = "/discovery/categories?locale=en-US&primary_only=false",
                .route = "/discovery/categories"},
              on_answer, ticket_new(d, ASK_CATEGORIES));
}

const char *discovery_category_name(const Discovery *d, int32_t id) {
  for (size_t i = 0; i < d->ncategories; i++) {
    if (d->categories[i].id == id) {
      return d->categories[i].name;
    }
  }

  return nullptr;
}
