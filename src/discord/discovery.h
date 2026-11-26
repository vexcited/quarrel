#ifndef QUARREL_DISCORD_DISCOVERY_H
#define QUARREL_DISCORD_DISCOVERY_H

#include "discord/api.h"

/* ========================================================================== */
/* the servers discord lets anyone find: the ones it features, by category,  */
/* and what a search finds among all of them. answers run on the main        */
/* thread, an older search dropped once a newer one was asked.               */
/* ========================================================================== */

// the lists of the client's tabs, each a category of discord's but the
// first.
typedef enum : uint8_t {
  DISCOVERY_FEATURED,
  DISCOVERY_GAMING,
  DISCOVERY_MUSIC,
  DISCOVERY_ENTERTAINMENT,
  DISCOVERY_TECH,
  DISCOVERY_EDUCATION,
  DISCOVERY_TABS,
} DiscoveryTab;

// every category at once, where a category's id goes.
constexpr int32_t DISCOVERY_ALL = -1;

typedef struct {
  Snowflake id;
  // never null.
  char *name;
  // null when the guild has none.
  char *description, *icon, *splash;
  uint32_t member_count, presence_count;
  bool verified, partnered;
} DiscoverableGuild;

typedef struct {
  DiscoverableGuild *guilds;
  size_t nguilds;
  // how many discord has, of which `guilds` holds the first.
  uint32_t total;
  bool loading;
  // why nothing came, null when it did.
  char *error;
  // seconds since 1970 it was answered at, 0 before it ever was.
  double fetched_at;
} DiscoveryList;

// how many servers a search found in a category.
typedef struct {
  int32_t id;
  uint32_t count;
} DiscoveryCount;

typedef struct {
  int32_t id;
  char *name;
} DiscoveryCategory;

typedef struct {
  DiscoveryList tabs[DISCOVERY_TABS];

  // the search: its words, null for none, the category it keeps to, the
  // language of the servers wanted like "fr", empty for any, and what it
  // found so far.
  char *query;
  int32_t category;
  char language[12];
  DiscoveryList results;
  // where the words were found, DISCOVERY_ALL first. empty until the first
  // answer.
  DiscoveryCount counts[9];
  size_t ncounts;

  // discord's names for its categories, once asked.
  DiscoveryCategory *categories;
  size_t ncategories;
  bool categories_loading, categories_loaded;

  // what the client names a visit of its page by, sent as a server of it
  // opens.
  char load_id[33];

  // bumped with every change of the fields above.
  uint64_t version;

  // its own.
  Api *api;
  void (*on_change)(void);
  // nothing is asked, as when running from a fixture.
  bool offline;
  struct DiscoveryTicket *tickets;
  uint64_t results_seq;
} Discovery;

// asks discord through `api`, which must outlive it.
Discovery *discovery_new(Api *api, void (*on_change)(void), bool offline);
void discovery_free(Discovery *d);

// discord's id of a tab's category, DISCOVERY_ALL for the featured servers.
[[gnu::const]] int32_t discovery_tab_category(DiscoveryTab tab);
// asks for a tab's servers unless they are known and fresh.
void discovery_fetch(Discovery *d, DiscoveryTab tab);
// asks for them again whatever is known, as after a failure.
void discovery_refresh(Discovery *d, DiscoveryTab tab);

// searches every discoverable server for `query`, in all categories. an
// empty or null one ends the search.
void discovery_search(Discovery *d, const char *query);
// keeps the search to a category, or to all with DISCOVERY_ALL.
void discovery_search_category(Discovery *d, int32_t category);
// keeps the search to servers of a language, null or empty for any.
void discovery_search_language(Discovery *d, const char *language);
// asks for the results after the ones known, when there are more.
void discovery_search_more(Discovery *d);

// asks once for the names of the categories.
void discovery_fetch_categories(Discovery *d);
// null until known.
[[gnu::pure]] const char *discovery_category_name(const Discovery *d,
                                                  int32_t id);

#endif
