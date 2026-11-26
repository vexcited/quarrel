#ifndef QUARREL_DISCORD_APPS_H
#define QUARREL_DISCORD_APPS_H

#include "discord/api.h"
#include "discord/permissions.h"

/* ========================================================================== */
/* discord's directory of applications: the collections it features, the     */
/* applications of a category or that a search finds, a page at a time, and  */
/* what the page of one says. answers run on the main thread, an older list  */
/* dropped once another was asked.                                            */
/* ========================================================================== */

// every category at once, where a category's id goes.
constexpr int32_t APPS_ALL = 0;
// how many applications the client asks a page of.
constexpr uint32_t APPS_PAGE = 48;
// the client never shows more pages than this.
constexpr uint32_t APPS_PAGES_MOST = 100;

// the application flags its pages read.
constexpr uint64_t APP_GATEWAY_PRESENCE = 1ull << 12;
constexpr uint64_t APP_GATEWAY_PRESENCE_LIMITED = 1ull << 13;
constexpr uint64_t APP_GATEWAY_GUILD_MEMBERS = 1ull << 14;
constexpr uint64_t APP_GATEWAY_GUILD_MEMBERS_LIMITED = 1ull << 15;
constexpr uint64_t APP_GATEWAY_MESSAGE_CONTENT = 1ull << 18;
constexpr uint64_t APP_GATEWAY_MESSAGE_CONTENT_LIMITED = 1ull << 19;
constexpr uint64_t APP_PROMOTED = 1ull << 29;
constexpr uint64_t APP_PARTNER = 1ull << 30;

// where an application installs, discord's integration types.
typedef enum : uint8_t {
  APP_INSTALL_GUILD,
  APP_INSTALL_USER,
  APP_INSTALLS,
} AppInstallType;

typedef struct {
  int32_t id;
  char *name;
} AppCategory;

typedef struct {
  char *name, *url;
} AppLink;

typedef struct {
  char *name, *description;
} AppCommand;

// a picture of its page: an image the media proxy serves, or the id of a
// youtube video.
typedef struct {
  bool youtube;
  char *source;
} AppMedia;

// what installing asks for, `scopes` separated by spaces, null when the
// application does not install that way.
typedef struct {
  char *scopes;
  Permissions permissions;
} AppInstall;

// the server where its makers answer.
typedef struct {
  Snowflake id;
  // null when the application names none.
  char *name;
  char *icon, *splash, *description;
  uint32_t member_count;
  bool discoverable, verified, partnered;
} AppGuild;

typedef struct {
  Snowflake id;
  // never null.
  char *name;
  // null when it has none: its icon's hash, the line of its card, and the
  // markdown of its page.
  char *icon, *summary, *detail;
  // its bot and the banner of it, 0 and null without.
  Snowflake bot_id;
  char *bot_banner;
  uint64_t flags;
  // an activity, which runs inside the client.
  bool embedded;
  bool monetized, ads;
  AppCategory *categories;
  size_t ncategories;
  uint32_t guild_count;
  char **locales;
  size_t nlocales;
  AppLink *links;
  size_t nlinks;
  AppCommand *commands;
  size_t ncommands;
  AppMedia *media;
  size_t nmedia;
  char *terms_url, *privacy_url;
  // the page of its makers that installs it, null when discord does.
  char *install_url;
  // what each way of installing asks for, and what the application asks
  // for when it sets no way apart.
  AppInstall installs[APP_INSTALLS];
  AppInstall install;
  AppGuild guild;
} DirectoryApp;

typedef struct {
  DirectoryApp *apps;
  size_t napps;
  // how many pages discord has of it.
  uint32_t pages;
  bool loading;
  // why nothing came, null when it did.
  char *error;
  // seconds since 1970 it was answered at, 0 before it ever was.
  double fetched_at;
} AppList;

typedef struct {
  char *title;
  DirectoryApp *apps;
  size_t napps;
} AppCollection;

// how many applications a search found in a category.
typedef struct {
  int32_t id;
  uint32_t count;
} AppCount;

// the page of one application.
typedef struct {
  Snowflake id;
  // empty until `loaded`.
  DirectoryApp app;
  bool loading, loaded, failed;
  double fetched_at;
  // the ones discord finds alike.
  AppList similar;
} AppPage;

// the picture of an activity's card, an asset of its application.
typedef struct {
  Snowflake id;
  // 0 while unknown and for none.
  Snowflake asset;
} AppCover;

typedef struct {
  // discord's categories, in its order.
  AppCategory *categories;
  size_t ncategories;
  bool categories_loading;
  double categories_at;

  // what its home features.
  AppCollection *collections;
  size_t ncollections;
  bool collections_loading;
  char *collections_error;
  double collections_at;

  // the list shown: the applications `query` finds, every one when null,
  // in `category`, and which page of them.
  char *query;
  int32_t category;
  uint32_t page;
  AppList list;
  // where `counted` was found, APPS_ALL first. empty until answered.
  char *counted;
  AppCount counts[16];
  size_t ncounts;

  AppPage **pages;
  size_t npages;
  AppCover *covers;
  size_t ncovers;

  // bumped with every change of the fields above.
  uint64_t version;

  // its own.
  Api *api;
  void (*on_change)(void);
  // nothing is asked, as when running from a fixture.
  bool offline;
  struct AppsTicket *tickets;
  uint64_t list_seq, counts_seq;
} Apps;

// asks discord through `api`, which must outlive it.
Apps *apps_new(Api *api, void (*on_change)(void), bool offline);
void apps_free(Apps *a);

// asks for the categories and for the home's collections unless they are
// known and fresh. `again` asks whatever is known, as after a failure.
void apps_fetch_categories(Apps *a);
void apps_fetch_collections(Apps *a, bool again);
// null until known.
[[gnu::pure]] const char *apps_category_name(const Apps *a, int32_t id);

// makes the list the applications `query` finds in `category`, page `page`
// from 1. a null or empty query lists the whole category. nothing is asked
// when it is that list already and it is fresh.
void apps_show(Apps *a, const char *query, int32_t category, uint32_t page);
// asks for the list again, as after a failure.
void apps_reload(Apps *a);

// the page of an application, asked for when unknown or old. never null.
AppPage *apps_open(Apps *a, Snowflake id);
// asks for it again after a failure.
void apps_reopen(Apps *a, Snowflake id);

// the asset an activity's card shows, 0 while unknown and for none. asked
// for the first time it is.
Snowflake apps_cover(Apps *a, Snowflake id);

// the application has discord or its makers install it.
[[gnu::pure]] bool app_installs(const DirectoryApp *app);

#endif
