#include "discord/gifs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "platform/platform.h"

#define TENOR_KEY "3Z0688EVWYKH"
#define GIPHY_KEY "Gc7131jiJuvI7IdN0HZ1D7nh0ow5BU6g"
#define TENOR_API "https://api.tenor.com/v1"
#define GIPHY_API "https://api.giphy.com/v1/gifs"

// a page of gifs, discord's own when it is asked for none.
constexpr int PAGE = 50;

typedef enum : uint8_t {
  ASK_CATEGORIES,
  ASK_RESULTS,
} Ask;

// a request in flight. those through the api are only answered while it
// lives, so they are freed with the gifs, never after.
typedef struct GifTicket {
  struct GifTicket *next;
  Gifs *g;
  Ask ask;
  GifProvider provider;
  uint64_t seq;
  HttpOp op;
} GifTicket;

static void changed(Gifs *g) {
  g->version++;

  if (g->on_change != nullptr) {
    g->on_change();
  }
}

static GifTicket *ticket_new(Gifs *g, Ask ask) {
  GifTicket *t = calloc(1, sizeof *t);

  if (t == nullptr) {
    cilo_oom(sizeof *t);
  }

  *t = (GifTicket){
    .next = g->tickets,
    .g = g,
    .ask = ask,
    .provider = g->provider,
    .seq = ask == ASK_CATEGORIES ? g->categories_seq : g->results_seq,
  };
  g->tickets = t;
  return t;
}

// takes the ticket off its list and frees it. true when its answer is still
// wanted.
static bool ticket_end(GifTicket *t) {
  Gifs *g = t->g;
  bool wanted = t->provider == g->provider &&
                t->seq == (t->ask == ASK_CATEGORIES ? g->categories_seq
                                                    : g->results_seq);

  for (GifTicket **at = &g->tickets; *at != nullptr; at = &(*at)->next) {
    if (*at == t) {
      *at = t->next;
      break;
    }
  }

  free(t);
  return wanted;
}

static void free_results(Gifs *g) {
  for (size_t i = 0; i < g->nresults; i++) {
    free(g->results[i].id);
    free(g->results[i].url);
    free(g->results[i].still);
    free(g->results[i].video);
    free(g->results[i].image);
  }

  free(g->results);
  g->results = nullptr;
  g->nresults = 0;
}

static void free_categories(Gifs *g) {
  for (size_t i = 0; i < g->ncategories; i++) {
    free(g->categories[i].name);
    free(g->categories[i].still);
  }

  free(g->categories);
  g->categories = nullptr;
  g->ncategories = 0;
  str_set(&g->trending_still, nullptr);
}

/* ========================================================================== */
/* what each provider answers.                                                */
/* ========================================================================== */

static const char *string_at(const cima_json *v, const char *key) {
  return json_get_or(v, key, (const char *)nullptr);
}

static uint32_t number_at(const cima_json *v, const char *key) {
  return (uint32_t)json_get_coerce(v, key, (uint64_t)0);
}

// { id, title, url, src, gif_src, width, height, preview }, `src` being of
// the media_format asked.
static Gif discord_gif(const cima_json *item) {
  return (Gif){
    .id = str_dup(string_at(item, "id")),
    .url = str_dup(string_at(item, "url")),
    .still = str_dup(string_at(item, "gif_src")),
    .video = str_dup(string_at(item, "src")),
    .image = str_dup(string_at(item, "gif_src")),
    .width = number_at(item, "width"),
    .height = number_at(item, "height"),
  };
}

// { id, itemurl, media: [{ tinygif, gif, mp4: { url, preview, dims } }] }
static Gif tenor_gif(const cima_json *item) {
  const cima_json *media = json_index(json_get(item, "media"), 0);
  const cima_json *tiny = json_get(media, "tinygif");
  const cima_json *mp4 = json_get(media, "mp4");
  const cima_json *dims = json_get(tiny, "dims");

  return (Gif){
    .id = str_dup(string_at(item, "id")),
    .url = str_dup(string_at(item, "itemurl")),
    .still = str_dup(string_at(tiny, "preview")),
    .video = str_dup(string_at(mp4, "url")),
    .image = str_dup(string_at(tiny, "url")),
    .width = (uint32_t)json_coerce(json_index(dims, 0), (uint64_t)0),
    .height = (uint32_t)json_coerce(json_index(dims, 1), (uint64_t)0),
  };
}

// { id, url, images: { fixed_width: { url, mp4, webp, width, height },
// fixed_width_still: { url } } }, the sizes as strings.
static Gif giphy_gif(const cima_json *item) {
  const cima_json *images = json_get(item, "images");
  const cima_json *moving = json_get(images, "fixed_width");

  return (Gif){
    .id = str_dup(string_at(item, "id")),
    .url = str_dup(string_at(item, "url")),
    .still = str_dup(string_at(json_get(images, "fixed_width_still"), "url")),
    .video = str_dup(string_at(moving, "mp4")),
    .image = str_dup(string_at(moving, "webp")),
    .width = number_at(moving, "width"),
    .height = number_at(moving, "height"),
  };
}

static void take_results(Gifs *g, GifProvider provider, const cima_json *json) {
  const cima_json *items = provider == GIF_PROVIDER_TENOR ? json_get(json, "results")
                           : provider == GIF_PROVIDER_GIPHY ? json_get(json, "data")
                                                            : json;
  size_t count = json_type(items) == CIMA_JSON_ARRAY ? json_len(items) : 0;

  free_results(g);
  g->results = count != 0 ? calloc(count, sizeof *g->results) : nullptr;

  if (count != 0 && g->results == nullptr) {
    cilo_oom(count * sizeof *g->results);
  }

  CIMA_EACH(item, json_items(items)) {
    Gif gif = provider == GIF_PROVIDER_TENOR   ? tenor_gif(item)
              : provider == GIF_PROVIDER_GIPHY ? giphy_gif(item)
                                               : discord_gif(item);

    // nothing to send or nothing to show.
    if (gif.url == nullptr || (gif.still == nullptr && gif.video == nullptr) ||
        gif.width == 0 || gif.height == 0) {
      free(gif.id);
      free(gif.url);
      free(gif.still);
      free(gif.video);
      free(gif.image);
      continue;
    }

    g->results[g->nresults++] = gif;
  }
}

static void add_category(Gifs *g, size_t cap, const char *name,
                         const char *still) {
  if (name == nullptr || *name == '\0' || g->ncategories == cap) {
    return;
  }

  g->categories[g->ncategories++] = (GifCategory){
    .name = str_dup(name),
    .still = str_dup(still),
  };
}

// discord: { categories: [{ name, src }], gifs }, tenor: { tags: [{
// searchterm, image }] }, giphy: { data: [{ name, gif: { images } }] }.
static void take_categories(Gifs *g, GifProvider provider,
                            const cima_json *json) {
  const char *key = provider == GIF_PROVIDER_TENOR   ? "tags"
                    : provider == GIF_PROVIDER_GIPHY ? "data"
                                                     : "categories";
  const cima_json *items = json_get(json, key);
  size_t cap = json_type(items) == CIMA_JSON_ARRAY ? json_len(items) : 0;

  free_categories(g);
  g->categories = cap != 0 ? calloc(cap, sizeof *g->categories) : nullptr;

  if (cap != 0 && g->categories == nullptr) {
    cilo_oom(cap * sizeof *g->categories);
  }

  CIMA_EACH(item, json_items(items)) {
    switch (provider) {
      case GIF_PROVIDER_TENOR:
        add_category(g, cap, string_at(item, "searchterm"),
                     string_at(item, "image"));
        break;

      case GIF_PROVIDER_GIPHY: {
        const cima_json *images = json_get(json_get(item, "gif"), "images");

        add_category(
          g, cap, string_at(item, "name"),
          string_at(json_get(images, "fixed_width_still"), "url"));
        break;
      }

      default:
        add_category(g, cap, string_at(item, "name"), string_at(item, "src"));
        break;
    }
  }

  if (provider == GIF_PROVIDER_DISCORD) {
    const cima_json *first = json_index(json_get(json, "gifs"), 0);

    str_set(&g->trending_still, string_at(first, "gif_src"));
  }
}

static void answered(GifTicket *t, const cima_json *json, const char *error) {
  Gifs *g = t->g;
  Ask ask = t->ask;
  GifProvider provider = t->provider;

  if (!ticket_end(t)) {
    return;
  }

  if (ask == ASK_CATEGORIES) {
    g->categories_loading = false;
    g->categories_loaded = true;

    if (error == nullptr) {
      take_categories(g, provider, json);
    }
  }
  else {
    g->loading = false;
    str_set(&g->error, error);

    if (error == nullptr) {
      take_results(g, provider, json);
    }
    else {
      free_results(g);
    }
  }

  changed(g);
}

static void on_api(void *ctx, const ApiResponse *res) {
  answered(ctx, res->json, res->ok ? nullptr : res->error.message);
}

static void on_http(void *ctx, const HttpResponse *res) {
  GifTicket *t = ctx;
  cilo_arena arena = {};
  const cima_json *json = nullptr;
  char error[96] = "";

  if (res->status == 0) {
    snprintf(error, sizeof error, "%s",
             res->error != nullptr ? res->error : "No answer came.");
  }
  else if (res->status < 200 || res->status >= 300) {
    snprintf(error, sizeof error, "%s answered %d.",
             gif_provider_name(t->provider), res->status);
  }
  else {
    json = json_parse(&arena, res->body, res->body_len, nullptr);
  }

  answered(t, json, *error != '\0' ? error : nullptr);
  cilo_arena_drop(&arena);
}

/* ========================================================================== */
/* asking.                                                                    */
/* ========================================================================== */

// the user's locale, "en-US" or "en_US" as tenor writes it.
static void locale(char *out, size_t cap, char separator) {
  platform_locale(out, cap);

  for (char *at = out; *at; at++) {
    if (*at == '-' || *at == '_') {
      *at = separator;
    }
  }
}

// `path` through discord's api, else the full url of another provider.
static void ask(Gifs *g, Ask what, const char *path, const char *route) {
  GifTicket *t = ticket_new(g, what);

  if (g->provider == GIF_PROVIDER_DISCORD) {
    ApiRequest req = {.method = "GET", .path = path, .route = route};

    api_request(g->api, &req, on_api, t);
    return;
  }

  HttpRequest req = {.url = path};

  t->op = http_send(&req, on_http, t);
}

const char *gif_provider_name(GifProvider provider) {
  switch (provider) {
    case GIF_PROVIDER_TENOR:
      return "Tenor";
    case GIF_PROVIDER_GIPHY:
      return "GIPHY";
    default:
      return "KLIPY";
  }
}

Gifs *gifs_new(Api *api, void (*on_change)(void)) {
  Gifs *g = calloc(1, sizeof *g);

  if (g == nullptr) {
    cilo_oom(sizeof *g);
  }

  g->api = api;
  g->on_change = on_change;
  return g;
}

void gifs_free(Gifs *g) {
  if (g == nullptr) {
    return;
  }

  // the api is freed first, nothing of it answers anymore.
  for (GifTicket *t = g->tickets; t != nullptr;) {
    GifTicket *next = t->next;

    http_cancel(t->op);
    free(t);
    t = next;
  }

  free_results(g);
  free_categories(g);
  free(g->query);
  free(g->error);
  free(g);
}

void gifs_set_provider(Gifs *g, GifProvider provider) {
  if (provider >= GIF_PROVIDERS || provider == g->provider) {
    return;
  }

  g->provider = provider;
  g->categories_seq++;
  g->results_seq++;
  free_results(g);
  free_categories(g);
  str_set(&g->query, nullptr);
  str_set(&g->error, nullptr);
  g->loading = false;
  g->categories_loading = false;
  g->categories_loaded = false;
  changed(g);
}

void gifs_fetch_categories(Gifs *g) {
  if (g->categories_loading || g->categories_loaded) {
    return;
  }

  char url[256], lang[32];

  switch (g->provider) {
    case GIF_PROVIDER_TENOR:
      locale(lang, sizeof lang, '_');
      snprintf(url, sizeof url,
               TENOR_API "/categories?key=" TENOR_KEY "&locale=%s"
                         "&type=featured",
               lang);
      break;

    case GIF_PROVIDER_GIPHY:
      snprintf(url, sizeof url, GIPHY_API "/categories?api_key=" GIPHY_KEY);
      break;

    default:
      // mp4 rather than the client's webm, which the system cannot play.
      locale(lang, sizeof lang, '-');
      snprintf(url, sizeof url, "/gifs/trending?locale=%s&media_format=mp4",
               lang);
      break;
  }

  g->categories_seq++;
  g->categories_loading = true;
  ask(g, ASK_CATEGORIES, url, "/gifs/trending");
  changed(g);
}

void gifs_search(Gifs *g, const char *query) {
  query = query != nullptr ? query : "";

  if (g->query != nullptr && strcmp(g->query, query) == 0 &&
      (g->loading || g->error == nullptr)) {
    return;
  }

  char url[768], lang[32];
  // it may be `g->query` itself, which is replaced.
  char *wanted = str_dup(query);
  char *q = url_encode(wanted);
  bool trending = *wanted == '\0';

  switch (g->provider) {
    case GIF_PROVIDER_TENOR:
      locale(lang, sizeof lang, '_');
      snprintf(url, sizeof url,
               TENOR_API "/%s?key=" TENOR_KEY "&locale=%s&limit=%d"
                         "&media_filter=minimal%s%s",
               trending ? "trending" : "search", lang, PAGE,
               trending ? "" : "&q=", q);
      break;

    case GIF_PROVIDER_GIPHY:
      locale(lang, sizeof lang, '-');
      lang[strcspn(lang, "-")] = '\0';
      snprintf(url, sizeof url,
               GIPHY_API "/%s?api_key=" GIPHY_KEY "&lang=%s&limit=%d%s%s",
               trending ? "trending" : "search", lang, PAGE,
               trending ? "" : "&q=", q);
      break;

    default:
      locale(lang, sizeof lang, '-');

      if (trending) {
        snprintf(url, sizeof url,
                 "/gifs/trending-gifs?media_format=mp4&locale=%s", lang);
      }
      else {
        snprintf(url, sizeof url,
                 "/gifs/search?q=%s&media_format=mp4&locale=%s", q, lang);
      }

      break;
  }

  free(q);
  free(g->query);
  g->query = wanted;
  str_set(&g->error, nullptr);
  free_results(g);
  g->results_seq++;
  g->loading = true;
  ask(g, ASK_RESULTS, url, trending ? "/gifs/trending-gifs" : "/gifs/search");
  changed(g);
}

static void ignore(void *, const HttpResponse *) {}

void gifs_picked(Gifs *g, const Gif *gif, const char *query) {
  if (gif->id == nullptr) {
    return;
  }

  query = query != nullptr ? query : "";

  if (g->provider == GIF_PROVIDER_DISCORD) {
    cima_json_writer w = {};

    json_begin_object(&w);
    json_field(&w, "id", gif->id);
    json_field(&w, "q", query);
    json_end_object(&w);

    ApiRequest req = {
      .method = "POST",
      .path = "/gifs/select",
      .body = cima_json_writer_text(&w),
    };

    api_request(g->api, &req, nullptr, nullptr);
    cima_json_writer_dispose(&w);
  }
  else if (g->provider == GIF_PROVIDER_TENOR) {
    char url[512], lang[32];
    char *id = url_encode(gif->id), *q = url_encode(query);

    locale(lang, sizeof lang, '_');
    snprintf(url, sizeof url,
             TENOR_API "/registershare?key=" TENOR_KEY "&locale=%s&id=%s&q=%s",
             lang, id, q);
    http_send(&(HttpRequest){.url = url}, ignore, nullptr);
    free(id);
    free(q);
  }
}
