#ifndef QUARREL_DISCORD_GIFS_H
#define QUARREL_DISCORD_GIFS_H

#include "discord/api.h"

/* ========================================================================== */
/* the gifs of the picker, from one provider at a time: the categories of    */
/* its front page, then what a search or the trending tile finds. answers    */
/* run on the main thread, an older one dropped once a newer one was asked.  */
/* ========================================================================== */

typedef enum : uint8_t {
  // klipy's, through discord's own routes as the client asks them.
  GIF_PROVIDER_DISCORD,
  GIF_PROVIDER_TENOR,
  GIF_PROVIDER_GIPHY,
  GIF_PROVIDERS,
} GifProvider;

typedef struct {
  // the provider's, for what it counts of the gifs sent.
  char *id;
  // what the message sends: a page discord embeds as a gif.
  char *url;
  // a picture that does not move, an mp4 that does, and the gif or the webp
  // the web client shows of a favorite. null when the provider gave none.
  char *still, *video, *image;
  uint32_t width, height;
} Gif;

typedef struct {
  // what picking it searches for.
  char *name;
  char *still;
} GifCategory;

typedef struct {
  GifProvider provider;

  // the front page. `trending_still` shows on the tile of the trending
  // gifs, null until known.
  GifCategory *categories;
  size_t ncategories;
  char *trending_still;
  // loaded once answered, though it failed: the front page has its tiles.
  bool categories_loading, categories_loaded;

  // what `query` found, the trending gifs for an empty one. `error` says
  // why nothing came, null when it did.
  char *query;
  Gif *results;
  size_t nresults;
  bool loading;
  char *error;

  // bumped with every change of the fields above.
  uint64_t version;

  // its own.
  Api *api;
  void (*on_change)(void);
  struct GifTicket *tickets;
  uint64_t categories_seq, results_seq;
} Gifs;

// asks discord through `api`, which must outlive it.
Gifs *gifs_new(Api *api, void (*on_change)(void));
void gifs_free(Gifs *g);

const char *gif_provider_name(GifProvider provider);
// forgets what the last provider found.
void gifs_set_provider(Gifs *g, GifProvider provider);

// asks once for the front page.
void gifs_fetch_categories(Gifs *g);
// asks for what `query` finds, the trending gifs for an empty one. the same
// query again asks nothing unless it failed.
void gifs_search(Gifs *g, const char *query);
// tells the provider a gif found by `query` was sent, as the client does.
void gifs_picked(Gifs *g, const Gif *gif, const char *query);

#endif
