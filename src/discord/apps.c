#include "discord/apps.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// a list, the categories and the collections are asked for again once they
// are this old, in seconds, as the client does.
constexpr double FRESH_FOR = 10 * 60;

// discord's kinds of what a collection or a search holds.
constexpr int64_t ITEM_APPLICATION = 1;
// a collection drawn as a gallery, which the client leaves out.
constexpr int64_t COLLECTION_GALLERY = 3;
// its kinds of what a page's carousel holds.
constexpr int64_t MEDIA_IMAGE = 1;
constexpr int64_t MEDIA_YOUTUBE = 2;
// the surface of the client that makes an application an activity.
constexpr int64_t SURFACE_MAIN = 0;

typedef enum : uint8_t {
  ASK_CATEGORIES,
  ASK_COLLECTIONS,
  ASK_LIST,
  ASK_COUNTS,
  ASK_APP,
  ASK_SIMILAR,
  ASK_COVER,
} Ask;

// a request in flight. they are only answered while the api lives, so they
// are freed with the directory, never after.
typedef struct AppsTicket {
  struct AppsTicket *next;
  Apps *a;
  Ask ask;
  uint64_t seq;
  Snowflake id;
} AppsTicket;

static void changed(Apps *a) {
  a->version++;

  if (a->on_change != nullptr) {
    a->on_change();
  }
}

static AppsTicket *ticket_new(Apps *a, Ask ask, Snowflake id) {
  AppsTicket *t = calloc(1, sizeof *t);

  if (t == nullptr) {
    cilo_oom(sizeof *t);
  }

  *t = (AppsTicket){
    .next = a->tickets,
    .a = a,
    .ask = ask,
    .seq = a->list_seq,
    .id = id,
  };
  a->tickets = t;
  return t;
}

static void ticket_end(AppsTicket *t) {
  for (AppsTicket **at = &t->a->tickets; *at != nullptr; at = &(*at)->next) {
    if (*at == t) {
      *at = t->next;
      break;
    }
  }

  free(t);
}

static void *grown(void *items, size_t count, size_t size) {
  void *more = realloc(items, count * size);

  if (more == nullptr) {
    cilo_oom(count * size);
  }

  return more;
}

/* ========================================================================== */
/* applications.                                                              */
/* ========================================================================== */

static void free_app(DirectoryApp *app) {
  for (size_t i = 0; i < app->ncategories; i++) {
    free(app->categories[i].name);
  }

  for (size_t i = 0; i < app->nlocales; i++) {
    free(app->locales[i]);
  }

  for (size_t i = 0; i < app->nlinks; i++) {
    free(app->links[i].name);
    free(app->links[i].url);
  }

  for (size_t i = 0; i < app->ncommands; i++) {
    free(app->commands[i].name);
    free(app->commands[i].description);
  }

  for (size_t i = 0; i < app->nmedia; i++) {
    free(app->media[i].source);
  }

  for (size_t i = 0; i < APP_INSTALLS; i++) {
    free(app->installs[i].scopes);
  }

  free(app->name);
  free(app->icon);
  free(app->summary);
  free(app->detail);
  free(app->bot_banner);
  free(app->categories);
  free(app->locales);
  free(app->links);
  free(app->commands);
  free(app->media);
  free(app->terms_url);
  free(app->privacy_url);
  free(app->install_url);
  free(app->install.scopes);
  free(app->guild.name);
  free(app->guild.icon);
  free(app->guild.splash);
  free(app->guild.description);
  *app = (DirectoryApp){};
}

static void free_apps(DirectoryApp *apps, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free_app(&apps[i]);
  }

  free(apps);
}

static void free_list(AppList *list) {
  free_apps(list->apps, list->napps);
  free(list->error);
  *list = (AppList){};
}

static const char *string_at(const cima_json *v, const char *key) {
  return json_get_or(v, key, (const char *)nullptr);
}

// an empty string says nothing either.
static char *text_at(const cima_json *v, const char *key) {
  const char *s = string_at(v, key);

  return s != nullptr && *s != '\0' ? str_dup(s) : nullptr;
}

static size_t count_of(const cima_json *array) {
  return json_type(array) == CIMA_JSON_ARRAY ? json_len(array) : 0;
}

// { scopes: [], permissions: "8" }, the scopes joined by spaces.
static void take_install(AppInstall *out, const cima_json *params) {
  if (json_type(params) != CIMA_JSON_OBJECT) {
    return;
  }

  size_t len = 0;

  CIMA_EACH(scope, json_items(json_get(params, "scopes"))) {
    const char *name = json_or(scope, (const char *)nullptr);

    len += name != nullptr ? strlen(name) + 1 : 0;
  }

  char *scopes = calloc(len + 1, 1);

  if (scopes == nullptr) {
    cilo_oom(len + 1);
  }

  CIMA_EACH(scope, json_items(json_get(params, "scopes"))) {
    const char *name = json_or(scope, (const char *)nullptr);

    if (name != nullptr) {
      if (*scopes != '\0') {
        strcat(scopes, " ");
      }

      strcat(scopes, name);
    }
  }

  out->scopes = scopes;
  out->permissions = snowflake_of(json_get(params, "permissions"));
}

// the id of the video a youtube link plays, null for none. from malloc.
static char *youtube_id(const char *url) {
  static const char *const marks[] = {"youtu.be/", "/embed/",
                                      "watch?v=", "/v/"};

  for (size_t i = 0; url != nullptr && i < sizeof marks / sizeof *marks; i++) {
    const char *at = strstr(url, marks[i]);

    if (at != nullptr) {
      at += strlen(marks[i]);

      size_t len = strcspn(at, "#&?");

      return len != 0 ? str_dup_n(at, len) : nullptr;
    }
  }

  return nullptr;
}

static void take_media(DirectoryApp *app, const cima_json *items) {
  size_t count = count_of(items);

  if (count == 0) {
    return;
  }

  app->media = calloc(count, sizeof *app->media);

  if (app->media == nullptr) {
    cilo_oom(count * sizeof *app->media);
  }

  CIMA_EACH(item, json_items(items)) {
    int64_t type = json_get_coerce(item, "type", (int64_t)0);
    const char *url = string_at(item, "url");
    const char *proxied = string_at(item, "proxy_url");
    AppMedia m = {};

    if (type == MEDIA_IMAGE) {
      m.source = str_dup(proxied != nullptr ? proxied : url);
    }
    else if (type == MEDIA_YOUTUBE) {
      m.youtube = true;
      m.source = youtube_id(url);
    }

    if (m.source != nullptr) {
      app->media[app->nmedia++] = m;
    }
  }
}

// an application as the directory answers one.
static void take_app(DirectoryApp *app, const cima_json *json) {
  const cima_json *entry = json_get(json, "directory_entry");
  const cima_json *bot = json_get(json, "bot");
  const cima_json *guild = json_get(json, "guild");
  const cima_json *types = json_get(json, "integration_types_config");
  const cima_json *config = json_get(json, "embedded_activity_config");
  const char *name = string_at(json, "name");
  size_t count;

  *app = (DirectoryApp){
    .id = snowflake_of(json_get(json, "id")),
    .name = str_dup(name != nullptr ? name : ""),
    .icon = str_dup(string_at(json, "icon")),
    .summary = text_at(entry, "short_description"),
    .detail = text_at(entry, "detailed_description"),
    .bot_id = snowflake_of(json_get(bot, "id")),
    .bot_banner = str_dup(string_at(bot, "banner")),
    .flags = snowflake_of(json_get(json, "flags_new")),
    .monetized = json_get_or(json, "is_monetized", false),
    .ads = json_get_or(config, "displays_advertisements", false),
    .guild_count = (uint32_t)json_get_coerce(entry, "guild_count", (uint64_t)0),
    .terms_url = text_at(json, "terms_of_service_url"),
    .privacy_url = text_at(json, "privacy_policy_url"),
    .install_url = text_at(json, "custom_install_url"),
  };

  if (app->flags == 0) {
    app->flags = json_get_coerce(json, "flags", (uint64_t)0);
  }

  if (app->summary == nullptr) {
    app->summary = text_at(json, "description");
  }

  CIMA_EACH(surface, json_items(json_get(json, "embedded_surfaces"))) {
    app->embedded |= json_coerce(surface, (int64_t)-1) == SURFACE_MAIN;
  }

  if ((count = count_of(json_get(json, "categories"))) != 0) {
    app->categories = calloc(count, sizeof *app->categories);

    if (app->categories == nullptr) {
      cilo_oom(count * sizeof *app->categories);
    }

    CIMA_EACH(category, json_items(json_get(json, "categories"))) {
      const char *named = string_at(category, "name");

      if (named != nullptr) {
        app->categories[app->ncategories++] = (AppCategory){
          .id = (int32_t)json_get_coerce(category, "id", (int64_t)0),
          .name = str_dup(named),
        };
      }
    }
  }

  if ((count = count_of(json_get(entry, "supported_locales"))) != 0) {
    app->locales = calloc(count, sizeof *app->locales);

    if (app->locales == nullptr) {
      cilo_oom(count * sizeof *app->locales);
    }

    CIMA_EACH(locale, json_items(json_get(entry, "supported_locales"))) {
      const char *code = json_or(locale, (const char *)nullptr);

      if (code != nullptr) {
        app->locales[app->nlocales++] = str_dup(code);
      }
    }
  }

  if ((count = count_of(json_get(entry, "external_urls"))) != 0) {
    app->links = calloc(count, sizeof *app->links);

    if (app->links == nullptr) {
      cilo_oom(count * sizeof *app->links);
    }

    CIMA_EACH(link, json_items(json_get(entry, "external_urls"))) {
      const char *url = string_at(link, "url");
      const char *named = string_at(link, "name");

      if (url != nullptr) {
        app->links[app->nlinks++] = (AppLink){
          .name = str_dup(named != nullptr ? named : url),
          .url = str_dup(url),
        };
      }
    }
  }

  const cima_json *commands = json_get(entry, "popular_application_commands");

  if ((count = count_of(commands)) != 0) {
    app->commands = calloc(count, sizeof *app->commands);

    if (app->commands == nullptr) {
      cilo_oom(count * sizeof *app->commands);
    }

    CIMA_EACH(command, json_items(commands)) {
      const char *named = string_at(command, "name");

      if (named != nullptr) {
        app->commands[app->ncommands++] = (AppCommand){
          .name = str_dup(named),
          .description = str_dup(string_at(command, "description")),
        };
      }
    }
  }

  take_media(app, json_get(entry, "carousel_items"));
  take_install(&app->install, json_get(json, "install_params"));
  take_install(&app->installs[APP_INSTALL_GUILD],
               json_get(json_get(types, "0"), "oauth2_install_params"));
  take_install(&app->installs[APP_INSTALL_USER],
               json_get(json_get(types, "1"), "oauth2_install_params"));

  if (json_type(guild) == CIMA_JSON_OBJECT) {
    app->guild = (AppGuild){
      .id = snowflake_of(json_get(guild, "id")),
      .name = str_dup(string_at(guild, "name")),
      .icon = str_dup(string_at(guild, "icon")),
      .splash = str_dup(string_at(guild, "discovery_splash")),
      .description = text_at(guild, "description"),
      .member_count = (uint32_t)json_get_coerce(
        guild, "approximate_member_count", (uint64_t)0),
    };

    CIMA_EACH(feature, json_items(json_get(guild, "features"))) {
      const char *named = json_or(feature, (const char *)nullptr);

      if (named != nullptr) {
        app->guild.discoverable |= strcmp(named, "DISCOVERABLE") == 0;
        app->guild.verified |= strcmp(named, "VERIFIED") == 0;
        app->guild.partnered |= strcmp(named, "PARTNERED") == 0;
      }
    }
  }
}

bool app_installs(const DirectoryApp *app) {
  return app->install_url != nullptr || app->install.scopes != nullptr ||
         app->installs[APP_INSTALL_GUILD].scopes != nullptr ||
         app->installs[APP_INSTALL_USER].scopes != nullptr;
}

// the applications of an array, each under `key` of its item when not null,
// items of another kind left out.
static DirectoryApp *take_apps(const cima_json *items, const char *key,
                               size_t *taken) {
  size_t count = count_of(items);
  DirectoryApp *apps = nullptr;

  *taken = 0;

  if (count == 0) {
    return nullptr;
  }

  apps = calloc(count, sizeof *apps);

  if (apps == nullptr) {
    cilo_oom(count * sizeof *apps);
  }

  CIMA_EACH(item, json_items(items)) {
    const cima_json *json = key != nullptr ? json_get(item, key) : item;
    bool application =
      key == nullptr ||
      json_get_coerce(item, "type", (int64_t)0) == ITEM_APPLICATION;

    if (application && snowflake_of(json_get(json, "id")) != 0) {
      take_app(&apps[(*taken)++], json);
    }
  }

  return apps;
}

/* ========================================================================== */
/* categories and collections.                                                */
/* ========================================================================== */

static void free_categories(Apps *a) {
  for (size_t i = 0; i < a->ncategories; i++) {
    free(a->categories[i].name);
  }

  free(a->categories);
  a->categories = nullptr;
  a->ncategories = 0;
}

// [{ id, name }].
static void take_categories(Apps *a, const cima_json *json) {
  size_t count = count_of(json);

  free_categories(a);

  if (count == 0) {
    return;
  }

  a->categories = calloc(count, sizeof *a->categories);

  if (a->categories == nullptr) {
    cilo_oom(count * sizeof *a->categories);
  }

  CIMA_EACH(category, json_items(json)) {
    const char *name = string_at(category, "name");

    if (name != nullptr) {
      a->categories[a->ncategories++] = (AppCategory){
        .id = (int32_t)json_get_coerce(category, "id", (int64_t)0),
        .name = str_dup(name),
      };
    }
  }
}

static void free_collections(Apps *a) {
  for (size_t i = 0; i < a->ncollections; i++) {
    free(a->collections[i].title);
    free_apps(a->collections[i].apps, a->collections[i].napps);
  }

  free(a->collections);
  a->collections = nullptr;
  a->ncollections = 0;
}

typedef struct {
  const cima_json *json;
  int64_t position;
  Snowflake id;
} Placed;

static int by_place(const void *a, const void *b) {
  const Placed *x = a, *y = b;

  if (x->position != y->position) {
    return x->position < y->position ? -1 : 1;
  }

  return x->id < y->id ? -1 : x->id > y->id;
}

// the items of an array in the order the client draws them: by position,
// then by id. from malloc.
static Placed *placed(const cima_json *items, size_t *count) {
  Placed *out = nullptr;

  *count = count_of(items);

  if (*count == 0) {
    return nullptr;
  }

  out = calloc(*count, sizeof *out);

  if (out == nullptr) {
    cilo_oom(*count * sizeof *out);
  }

  size_t at = 0;

  CIMA_EACH(item, json_items(items)) {
    out[at++] = (Placed){
      .json = item,
      .position = json_get_coerce(item, "position", (int64_t)0),
      .id = snowflake_of(json_get(item, "id")),
    };
  }

  qsort(out, *count, sizeof *out, by_place);
  return out;
}

// [{ id, type, position, title, application_directory_collection_items:
// [{ id, type, position, application }] }].
static void take_collections(Apps *a, const cima_json *json) {
  size_t count = 0;
  Placed *order = placed(json, &count);

  free_collections(a);

  if (count == 0) {
    return;
  }

  a->collections = calloc(count, sizeof *a->collections);

  if (a->collections == nullptr) {
    cilo_oom(count * sizeof *a->collections);
  }

  for (size_t i = 0; i < count; i++) {
    const cima_json *collection = order[i].json;
    const char *title = string_at(collection, "title");

    if (json_get_coerce(collection, "type", (int64_t)0) == COLLECTION_GALLERY) {
      continue;
    }

    size_t nitems = 0;
    Placed *items = placed(
      json_get(collection, "application_directory_collection_items"), &nitems);
    AppCollection c = {.title = str_dup(title != nullptr ? title : "")};

    if (nitems != 0) {
      c.apps = calloc(nitems, sizeof *c.apps);

      if (c.apps == nullptr) {
        cilo_oom(nitems * sizeof *c.apps);
      }
    }

    for (size_t j = 0; j < nitems; j++) {
      const cima_json *app = json_get(items[j].json, "application");
      bool application =
        json_get_coerce(items[j].json, "type", (int64_t)0) == ITEM_APPLICATION;

      if (application && snowflake_of(json_get(app, "id")) != 0) {
        take_app(&c.apps[c.napps++], app);
      }
    }

    free(items);
    a->collections[a->ncollections++] = c;
  }

  free(order);
}

/* ========================================================================== */
/* pages and covers.                                                          */
/* ========================================================================== */

static AppPage *page_of(const Apps *a, Snowflake id) {
  for (size_t i = 0; i < a->npages; i++) {
    if (a->pages[i]->id == id) {
      return a->pages[i];
    }
  }

  return nullptr;
}

static AppCover *cover_of(const Apps *a, Snowflake id) {
  for (size_t i = 0; i < a->ncovers; i++) {
    if (a->covers[i].id == id) {
      return &a->covers[i];
    }
  }

  return nullptr;
}

/* ========================================================================== */
/* answers.                                                                   */
/* ========================================================================== */

// { counts_by_category: { "6": 44 }, result_count }. the client lists every
// category at once first, by the count of the whole search.
static void take_counts(Apps *a, const cima_json *json) {
  a->ncounts = 0;
  a->counts[a->ncounts++] = (AppCount){
    .id = APPS_ALL,
    .count = (uint32_t)json_get_coerce(json, "result_count", (uint64_t)0),
  };

  CIMA_EACH(member, json_members(json_get(json, "counts_by_category"))) {
    if (a->ncounts == sizeof a->counts / sizeof *a->counts) {
      break;
    }

    char digits[16];
    size_t len = cima_json_key_len(member);

    if (len == 0 || len >= sizeof digits) {
      continue;
    }

    memcpy(digits, member->key, len);
    digits[len] = '\0';
    a->counts[a->ncounts++] = (AppCount){
      .id = (int32_t)strtol(digits, nullptr, 10),
      .count = (uint32_t)json_coerce(&member->value, (uint64_t)0),
    };
  }
}

static void on_answer(void *ctx, const ApiResponse *res) {
  AppsTicket *t = ctx;
  AppsTicket asked = *t;
  Apps *a = t->a;
  const char *error = res->ok ? nullptr : res->error.message;

  ticket_end(t);

  switch (asked.ask) {
    case ASK_CATEGORIES:
      a->categories_loading = false;
      a->categories_at = platform_now();

      if (error == nullptr) {
        take_categories(a, res->json);
      }

      break;

    case ASK_COLLECTIONS:
      a->collections_loading = false;
      a->collections_at = platform_now();
      str_set(&a->collections_error, error);

      // what was featured stays when asking again failed.
      if (error == nullptr) {
        take_collections(a, res->json);
      }

      break;

    case ASK_LIST: {
      // another list was asked for since.
      if (asked.seq != a->list_seq) {
        return;
      }

      AppList *list = &a->list;

      list->loading = false;
      list->fetched_at = platform_now();
      str_set(&list->error, error);

      if (error == nullptr) {
        free_apps(list->apps, list->napps);
        list->apps =
          take_apps(json_get(res->json, "results"), "data", &list->napps);
        list->pages =
          (uint32_t)json_get_coerce(res->json, "num_pages", (uint64_t)0);
      }

      break;
    }

    case ASK_COUNTS:
      if (asked.seq != a->counts_seq || error != nullptr) {
        return;
      }

      take_counts(a, res->json);
      break;

    case ASK_APP: {
      AppPage *page = page_of(a, asked.id);

      if (page == nullptr) {
        return;
      }

      page->loading = false;
      page->fetched_at = platform_now();
      page->failed = error != nullptr;

      if (error == nullptr) {
        free_app(&page->app);
        take_app(&page->app, res->json);
        page->loaded = true;
      }

      break;
    }

    case ASK_SIMILAR: {
      AppPage *page = page_of(a, asked.id);

      if (page == nullptr) {
        return;
      }

      AppList *list = &page->similar;

      list->loading = false;
      list->fetched_at = platform_now();

      if (error == nullptr) {
        free_apps(list->apps, list->napps);
        list->apps =
          take_apps(json_get(res->json, "applications"), nullptr, &list->napps);
      }

      break;
    }

    case ASK_COVER: {
      AppCover *cover = cover_of(a, asked.id);

      if (cover == nullptr || error != nullptr) {
        return;
      }

      // [{ id, type, name }], the card's the one named so.
      CIMA_EACH(asset, json_items(res->json)) {
        const char *name = string_at(asset, "name");

        if (name != nullptr && strcmp(name, "embedded_cover") == 0) {
          cover->asset = snowflake_of(json_get(asset, "id"));
          break;
        }
      }

      if (cover->asset == 0) {
        return;
      }

      break;
    }
  }

  changed(a);
}

/* ========================================================================== */
/* asking.                                                                    */
/* ========================================================================== */

Apps *apps_new(Api *api, void (*on_change)(void), bool offline) {
  Apps *a = calloc(1, sizeof *a);

  if (a == nullptr) {
    cilo_oom(sizeof *a);
  }

  a->api = api;
  a->on_change = on_change;
  a->offline = offline;
  return a;
}

void apps_free(Apps *a) {
  if (a == nullptr) {
    return;
  }

  // the api is freed first, nothing of it answers anymore.
  for (AppsTicket *t = a->tickets; t != nullptr;) {
    AppsTicket *next = t->next;

    free(t);
    t = next;
  }

  for (size_t i = 0; i < a->npages; i++) {
    free_app(&a->pages[i]->app);
    free_list(&a->pages[i]->similar);
    free(a->pages[i]);
  }

  free_categories(a);
  free_collections(a);
  free_list(&a->list);
  free(a->collections_error);
  free(a->query);
  free(a->counted);
  free(a->pages);
  free(a->covers);
  free(a);
}

static bool fresh(double at) {
  return at != 0 && platform_now() - at < FRESH_FOR;
}

static void get(Apps *a, const char *path, const char *route, Ask ask,
                Snowflake id) {
  AppsTicket *t = ticket_new(a, ask, id);

  // the counts outlive the pages asked after them.
  if (ask == ASK_COUNTS) {
    t->seq = ++a->counts_seq;
  }

  api_request(a->api,
              &(ApiRequest){.method = "GET", .path = path, .route = route},
              on_answer, t);
}

void apps_fetch_categories(Apps *a) {
  if (a->categories_loading || fresh(a->categories_at) || a->offline) {
    return;
  }

  a->categories_loading = true;
  get(a, "/application-directory-static/categories?locale=en-US",
      "/application-directory-static/categories", ASK_CATEGORIES, 0);
}

void apps_fetch_collections(Apps *a, bool again) {
  // one that failed counts as fresh too, rather than being asked for on
  // every frame.
  if (a->collections_loading || (fresh(a->collections_at) && !again)) {
    return;
  }

  if (a->offline) {
    a->collections_at = platform_now();
    str_set(&a->collections_error, "Exploring apps needs a connection.");
    changed(a);
    return;
  }

  a->collections_loading = true;
  // the client's surface is the directory's, its platform the web's, and it
  // asks for the collections in use.
  get(a,
      "/application-directory-static/collections"
      "?surface=1&active_state=1&platform=4&locale=en-US&cache=true",
      "/application-directory-static/collections", ASK_COLLECTIONS, 0);
  changed(a);
}

const char *apps_category_name(const Apps *a, int32_t id) {
  for (size_t i = 0; i < a->ncategories; i++) {
    if (a->categories[i].id == id) {
      return a->categories[i].name;
    }
  }

  return nullptr;
}

// asks for the list as it is set, and for the counts of its words when
// they are not the ones counted.
static void ask_list(Apps *a) {
  AppList *list = &a->list;

  a->list_seq++;

  if (a->offline) {
    list->loading = false;
    list->fetched_at = platform_now();
    str_set(&list->error, "Exploring apps needs a connection.");
    return;
  }

  char *words = url_encode(a->query != nullptr ? a->query : "");
  char path[640];

  // the client's query, in its order.
  snprintf(path, sizeof path,
           "/application-directory-static/search?query=%s&page=%u"
           "&page_size=%u&category_id=%d&locale=en-US&source=0",
           words, a->page, APPS_PAGE, a->category);
  list->loading = true;
  str_set(&list->error, nullptr);
  get(a, path, "/application-directory-static/search", ASK_LIST, 0);

  bool counted = a->counted != nullptr && a->query != nullptr &&
                 strcmp(a->counted, a->query) == 0;

  if (a->query != nullptr && !counted) {
    // the same search without a page, which the client reads the counts
    // of every category from.
    snprintf(path, sizeof path,
             "/application-directory-static/search?query=%s&locale=en-US"
             "&source=0",
             words);
    str_set(&a->counted, a->query);
    a->ncounts = 0;
    get(a, path, "/application-directory-static/search", ASK_COUNTS, 0);
  }

  free(words);
}

void apps_show(Apps *a, const char *query, int32_t category, uint32_t page) {
  while (query != nullptr && *query == ' ') {
    query++;
  }

  size_t len = query != nullptr ? strlen(query) : 0;

  while (len != 0 && query[len - 1] == ' ') {
    len--;
  }

  // discord takes 100 characters.
  char *wanted = len != 0 ? str_dup_n(query, len > 100 ? 100 : len) : nullptr;
  bool same_words = (wanted == nullptr) == (a->query == nullptr) &&
                    (wanted == nullptr || strcmp(wanted, a->query) == 0);
  bool same = same_words && a->category == category && a->page == page;

  if (page == 0 || (same && (a->list.loading || a->list.fetched_at != 0))) {
    // a list that failed stays until it is asked for again.
    if (same && !a->list.loading && a->list.error == nullptr &&
        !fresh(a->list.fetched_at)) {
      ask_list(a);
      changed(a);
    }

    free(wanted);
    return;
  }

  free_list(&a->list);
  free(a->query);
  a->query = wanted;
  a->category = category;
  a->page = page;

  if (wanted == nullptr) {
    str_set(&a->counted, nullptr);
    a->ncounts = 0;
  }

  ask_list(a);
  changed(a);
}

void apps_reload(Apps *a) {
  if (a->page == 0 || a->list.loading) {
    return;
  }

  ask_list(a);
  changed(a);
}

static void ask_page(Apps *a, AppPage *page) {
  char path[160];

  if (a->offline) {
    page->failed = true;
    page->fetched_at = platform_now();
    return;
  }

  page->loading = true;
  page->failed = false;
  snprintf(path, sizeof path,
           "/application-directory-static/applications/%" PRIu64
           "?locale=en-US",
           page->id);
  get(a, path, "/application-directory-static/applications/:id", ASK_APP,
      page->id);

  page->similar.loading = true;
  snprintf(path, sizeof path,
           "/application-directory-static/applications/%" PRIu64
           "/similar?locale=en-US",
           page->id);
  get(a, path, "/application-directory-static/applications/:id/similar",
      ASK_SIMILAR, page->id);
}

AppPage *apps_open(Apps *a, Snowflake id) {
  AppPage *page = page_of(a, id);

  if (page == nullptr) {
    page = calloc(1, sizeof *page);

    if (page == nullptr) {
      cilo_oom(sizeof *page);
    }

    page->id = id;
    a->pages = grown(a->pages, a->npages + 1, sizeof *a->pages);
    a->pages[a->npages++] = page;
  }

  if (!page->loading && !fresh(page->fetched_at)) {
    ask_page(a, page);
    changed(a);
  }

  return page;
}

void apps_reopen(Apps *a, Snowflake id) {
  AppPage *page = page_of(a, id);

  if (page != nullptr && !page->loading) {
    ask_page(a, page);
    changed(a);
  }
}

Snowflake apps_cover(Apps *a, Snowflake id) {
  AppCover *cover = cover_of(a, id);

  if (cover != nullptr) {
    return cover->asset;
  }

  a->covers = grown(a->covers, a->ncovers + 1, sizeof *a->covers);
  a->covers[a->ncovers++] = (AppCover){.id = id};

  if (!a->offline) {
    char path[96];

    // each application is a route of its own, so the cards of a page ask
    // at once as the client's do.
    snprintf(path, sizeof path, "/oauth2/applications/%" PRIu64 "/assets", id);
    get(a, path, nullptr, ASK_COVER, id);
  }

  return 0;
}
