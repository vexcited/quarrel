#include "alerts.h"

#include <inttypes.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/window.h>

#include "components.h"
#include "discord/cdn.h"
#include "discord/markdown.h"
#include "http.h"
#include "sounds.h"

// learned from the web client's NotificationStore: a message's sound plays
// at four tenths of the volume, a channel keeps its 20 latest notifications,
// and they go away once the channel is read or looked at.
constexpr float MESSAGE_VOLUME = .4f;
constexpr size_t CHANNEL_MOST = 20;
constexpr size_t SHOWN_MOST = 128;
// the avatars kept as files for the system to read, the ones used longest
// ago written over.
constexpr size_t AVATARS = 64;
constexpr size_t AVATAR_BYTES_MOST = 1u << 20;
constexpr uint32_t AVATAR_PIXELS = 128;
// a notification waits this long for its avatar, then goes without.
constexpr double AVATAR_SECONDS = 5;
// learned from the same client: a call rings at the whole volume, for the
// one rung and for the one who waits for an answer.
constexpr float RING_VOLUME = 1;
constexpr size_t RINGS_MOST = 16;

#define ENABLED_KEY "notify_desktop"

typedef struct {
  // the platform's, 0 for none.
  uint64_t id;
  Snowflake account_id, channel_id, message_id;
  // a call the account is rung for, which has no message.
  bool call;
} Shown;

typedef struct {
  Snowflake account_id, channel_id;
} Ring;

// a notification on its way: its avatar is being fetched, or one that came
// before it still waits for its own.
typedef struct Waiting Waiting;

struct Waiting {
  Waiting *next;
  // 0 once answered or when nothing was asked.
  HttpOp op;
  Shown shown;
  size_t slot;
  char url[CDN_URL_MAX];
  char title[256], body[512];
};

typedef struct {
  char url[CDN_URL_MAX];
  // when it was last shown, by `clock`, and whether its file is written.
  uint64_t used;
  bool ready;
} Avatar;

static struct {
  App *app;
  bool focused;
  bool loaded, enabled;
  // oldest first.
  Shown shown[SHOWN_MOST];
  size_t nshown;
  uint64_t serial, clock;
  // oldest first.
  Waiting *waiting;
  Avatar avatars[AVATARS];
  // the channel of the notification pressed, opened by the next frame,
  // and whether its call is answered too.
  Snowflake open_account, open_channel;
  bool open_answers;
  // the calls the accounts were rung for when last looked at.
  Ring rings[RINGS_MOST];
  size_t nrings;
} A;

static const Sound FETCHED_FIRST[] = {
  SOUND_MESSAGE1,
  SOUND_CALL_RINGING,
  SOUND_CALL_CALLING,
};

bool alerts_enabled(void) {
  if (!A.loaded) {
    char *saved = platform_pref_get(ENABLED_KEY);

    A.loaded = true;
    A.enabled = saved == nullptr || saved[0] != '0';
    free(saved);
  }

  return A.enabled;
}

void alerts_set_enabled(bool on) {
  A.loaded = true;
  A.enabled = on;
  platform_pref_set(ENABLED_KEY, on ? "1" : "0");
}

/* ========================================================================== */
/* what a notification says. learned from the same client: the title is who   */
/* wrote and where, the body what they wrote as it reads without markup.      */
/* ========================================================================== */

typedef struct {
  char *text;
  size_t len, cap;
} Line;

// adds what fits, never half a character of utf-8.
static void put(Line *l, const char *s) {
  size_t n = s != nullptr ? strlen(s) : 0;
  size_t room = l->cap - 1 - l->len;

  if (n > room) {
    n = room;

    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
      n--;
    }
  }

  memcpy(l->text + l->len, s, n);
  l->len += n;
  l->text[l->len] = '\0';
}

static void flatten(Line *l, Store *store, Snowflake guild_id,
                    const MdNode *n) {
  char name[128];

  switch (n->kind) {
    case MD_TEXT:
    case MD_CODE:
    case MD_CODE_BLOCK:
    case MD_EMOJI:
    case MD_TIMESTAMP:
      put(l, n->text);
      break;
    case MD_LINE_BREAK:
      put(l, " ");
      break;
    // what was hidden in the channel stays hidden on the screen.
    case MD_SPOILER:
      put(l, "(spoiler)");
      return;
    case MD_MENTION_USER:
      put(l, "@");
      put(l, store_display_name(store, guild_id, n->id));
      break;
    case MD_MENTION_ROLE: {
      const Role *role = store_role(store_guild(store, guild_id), n->id);

      put(l, "@");
      put(l, role != nullptr && role->name != nullptr ? role->name
                                                      : "unknown-role");
      break;
    }
    case MD_MENTION_CHANNEL: {
      const Channel *c = store_channel(store, n->id);

      put(l, "#");
      put(l, c != nullptr ? store_channel_name(store, c, name, sizeof name)
                          : "unknown");
      break;
    }
    case MD_MENTION_EVERYONE:
      put(l, "@everyone");
      break;
    case MD_MENTION_HERE:
      put(l, "@here");
      break;
    case MD_CUSTOM_EMOJI:
      put(l, ":");
      put(l, n->text);
      put(l, ":");
      break;
    default:
      break;
  }

  for (const MdNode *child = n->first; child != nullptr; child = child->next) {
    flatten(l, store, guild_id, child);

    bool block =
      n->kind == MD_ROOT || n->kind == MD_QUOTE || n->kind == MD_LIST;

    if (block && child->next != nullptr) {
      put(l, " ");
    }
  }
}

// what stands for a message without words, as the client picks it: its
// first embed, a voice message, the name of its first file.
static void wordless(Line *l, const Message *m) {
  if (m->nembeds != 0) {
    const Embed *e = &m->embeds[0];

    if (e->description != nullptr || e->title != nullptr) {
      put(l, e->title);
      put(l, e->title != nullptr && e->description != nullptr ? " " : "");
      put(l, e->description);
      return;
    }

    if (e->nfields != 0) {
      put(l, e->fields[0].name);
      put(l, " ");
      put(l, e->fields[0].value);
      return;
    }
  }

  if ((m->flags & MESSAGE_FLAG_VOICE_MESSAGE) != 0) {
    put(l, "Sent a voice message");
  }
  else if (m->nattachments != 0) {
    put(l, m->attachments[0].filename != nullptr ? m->attachments[0].filename
                                                 : "Sent a file");
  }
}

static void describe(Store *store, const Channel *c, const Message *m,
                     char *title, size_t title_cap, char *body,
                     size_t body_cap) {
  Author author = store_author(store, m);
  Line head = {title, 0, title_cap}, text = {body, 0, body_cap};
  const Channel *parent = store_channel(store, c->parent_id);
  char name[128];

  title[0] = body[0] = '\0';
  put(&head, author.name);

  // a thread's first message is told as its channel's.
  if (m->type == MESSAGE_THREAD_STARTER_MESSAGE && parent != nullptr) {
    c = parent;
    parent = store_channel(store, c->parent_id);
  }

  if (c->type == CHANNEL_GROUP_DM || c->guild_id != 0) {
    put(&head, " (");
    put(&head, c->guild_id != 0 && !channel_is_voice(c) ? "#" : "");
    put(&head, store_channel_name(store, c, name, sizeof name));

    if (parent != nullptr) {
      put(&head, ", ");
      put(&head, store_channel_name(store, parent, name, sizeof name));
    }

    put(&head, ")");
  }

  if (message_is_system(m)) {
    char line[384];
    const char *other =
      m->nmentions != 0 ? store_display_name(store, c->guild_id, m->mentions[0])
                        : "someone";

    put(&text, author.name);
    put(&text, " ");
    put(&text, message_system_text(m, line, sizeof line, other));
    return;
  }

  // an application may write with its components alone.
  const char *words =
    m->content[0] != '\0' ? m->content : message_component_text(m);

  if (m->forwarded) {
    put(&text, "Forwarded a message");
  }
  else if (m->nstickers != 0) {
    put(&text, "Sticker: ");
    put(&text, m->stickers[0].name != nullptr ? m->stickers[0].name : "");
  }
  else if (words != nullptr) {
    MdDoc *doc = md_parse(words, strlen(words), nullptr);

    flatten(&text, store, c->guild_id, doc->root);
    md_free(doc);
  }

  if (text.len == 0) {
    wordless(&text, m);
  }

  if (text.len == 0) {
    put(&text, "Sent a message");
  }
}

/* ========================================================================== */
/* the notifications shown.                                                   */
/* ========================================================================== */

static void forget(size_t at) {
  memmove(&A.shown[at], &A.shown[at + 1],
          (A.nshown - at - 1) * sizeof *A.shown);
  A.nshown--;
}

static void take_back(size_t at) {
  platform_notify_close(A.shown[at].id);
  forget(at);
}

static Session *session_of(Snowflake account_id) {
  const Roster *roster = A.app->roster;

  if (roster->current->account_id == account_id) {
    return roster->current;
  }

  for (size_t i = 0; i < roster->naccounts; i++) {
    if (roster->accounts[i].id == account_id) {
      return roster->accounts[i].session;
    }
  }

  return nullptr;
}

// whether what a notification tells of was read, or is gone.
static bool stale(const Shown *shown) {
  const Session *s = session_of(shown->account_id);

  if (shown->call && s != nullptr) {
    const PrivateCall *call = store_call(s->store, shown->channel_id);

    return call == nullptr || !call_rings(s->store, call);
  }

  return s == nullptr ||
         store_channel(s->store, shown->channel_id) == nullptr ||
         store_last_acked(s->store, shown->channel_id) >= shown->message_id;
}

static void show(const Shown *shown, const char *title, const char *body,
                 const char *image) {
  size_t same = 0;

  // read on another device while its avatar was fetched.
  if (stale(shown)) {
    return;
  }

  for (size_t i = A.nshown; i > 0; i--) {
    const Shown *other = &A.shown[i - 1];

    if (other->account_id == shown->account_id &&
        other->channel_id == shown->channel_id && ++same >= CHANNEL_MOST) {
      take_back(i - 1);
    }
  }

  if (A.nshown == SHOWN_MOST) {
    take_back(0);
  }

  A.shown[A.nshown++] = *shown;
  platform_notify(&(PlatformNotification){
    .id = shown->id,
    .title = title,
    .body = body,
    .image = image,
    .call = shown->call,
  });
}

static void follow_calls(void);

void alerts_sweep(void) {
  if (A.app == nullptr) {
    return;
  }

  follow_calls();

  for (size_t i = A.nshown; i > 0; i--) {
    if (stale(&A.shown[i - 1])) {
      take_back(i - 1);
    }
  }
}

/* ========================================================================== */
/* avatars. the system reads the picture of a notification from a file, a     */
/* png for every server to know it.                                           */
/* ========================================================================== */

// named after its url: a server keeps what it read from a path, and would
// show a slot's former avatar if the name stayed.
static bool avatar_path(size_t slot, char out[static 1024]) {
  uint64_t hash = 0xcbf29ce484222325;
  char name[40];

  for (const char *at = A.avatars[slot].url; *at != '\0'; at++) {
    hash = (hash ^ (uint8_t)*at) * 0x100000001b3;
  }

  snprintf(name, sizeof name, "notify-%016" PRIx64 ".png", hash);
  return platform_cache_path(name, out, 1024);
}

// the cdn serves the same picture under any of its formats.
static void as_png(char url[static CDN_URL_MAX]) {
  char *webp = strstr(url, ".webp");

  if (webp != nullptr) {
    memmove(webp + 4, webp + 5, strlen(webp + 5) + 1);
    memcpy(webp, ".png", 4);
  }
}

static void avatar_url(Store *store, const Channel *c, const Message *m,
                       char out[static CDN_URL_MAX]) {
  Author author = store_author(store, m);

  out[0] = '\0';

  if (author.user == nullptr) {
    return;
  }

  if (author.member != nullptr && author.member->avatar != nullptr) {
    cdn_member_avatar(out, c->guild_id, author.user->id, author.member->avatar,
                      AVATAR_PIXELS);
  }
  else {
    cdn_avatar(out, author.user->id, author.user->avatar,
               author.user->discriminator, AVATAR_PIXELS);
  }

  as_png(out);
}

// the slot of an url, its file written or not, taking the one used longest
// ago for an url that has none.
static size_t avatar_slot(const char *url) {
  size_t oldest = 0;

  for (size_t i = 0; i < AVATARS; i++) {
    if (strcmp(A.avatars[i].url, url) == 0) {
      A.avatars[i].used = ++A.clock;
      return i;
    }

    if (A.avatars[i].used < A.avatars[oldest].used) {
      oldest = i;
    }
  }

  char path[1024];

  if (A.avatars[oldest].ready && avatar_path(oldest, path)) {
    remove(path);
  }

  snprintf(A.avatars[oldest].url, CDN_URL_MAX, "%s", url);
  A.avatars[oldest].used = ++A.clock;
  A.avatars[oldest].ready = false;
  return oldest;
}

static bool is_png(const HttpResponse *res) {
  return res->status == 200 && res->body_len > 8 &&
         memcmp(res->body, "\x89PNG\r\n\x1a\n", 8) == 0;
}

// the file is whole or not there: it is renamed into place.
static bool avatar_write(size_t slot, const HttpResponse *res) {
  char path[1024], writing[1040];

  if (!avatar_path(slot, path)) {
    return false;
  }

  snprintf(writing, sizeof writing, "%s.new", path);

  FILE *file = fopen(writing, "wb");

  if (file == nullptr) {
    return false;
  }

  bool ok = fwrite(res->body, 1, res->body_len, file) == res->body_len;

  ok = fclose(file) == 0 && ok;

  if (!ok || rename(writing, path) != 0) {
    remove(writing);
    return false;
  }

  return true;
}

// shows what waits at the front and is done waiting, so that notifications
// come in the order of their messages.
static void flush(void) {
  while (A.waiting != nullptr && A.waiting->op == 0) {
    Waiting *w = A.waiting;
    const Avatar *avatar = &A.avatars[w->slot];
    char path[1024];
    // the slot may be another url's by now.
    bool pictured = w->url[0] != '\0' && avatar->ready &&
                    strcmp(avatar->url, w->url) == 0 &&
                    avatar_path(w->slot, path);

    A.waiting = w->next;
    show(&w->shown, w->title, w->body, pictured ? path : nullptr);
    free(w);
  }
}

static void on_avatar(void *ctx, const HttpResponse *res) {
  Waiting *w = ctx;
  Avatar *avatar = &A.avatars[w->slot];

  w->op = 0;

  if (strcmp(avatar->url, w->url) == 0 && !avatar->ready && is_png(res)) {
    avatar->ready = avatar_write(w->slot, res);
  }

  flush();
}

// shows a notification once its avatar came, after the ones before it.
static void queue(Waiting *w) {
  Waiting **last = &A.waiting;

  while (*last != nullptr) {
    last = &(*last)->next;
  }

  *last = w;

  if (w->url[0] != '\0') {
    w->slot = avatar_slot(w->url);

    if (!A.avatars[w->slot].ready) {
      HttpRequest req = {
        .url = w->url,
        .timeout = AVATAR_SECONDS,
        .max_response_len = AVATAR_BYTES_MOST,
      };

      w->op = http_send(&req, on_avatar, w);
    }
  }

  flush();
}

/* ========================================================================== */
/* a message came.                                                            */
/* ========================================================================== */

void alerts_message(Session *s, const Message *m) {
  const Channel *c = store_channel(s->store, m->channel_id);
  if (A.app == nullptr || c == nullptr) {
    return;
  }

  // nothing is told of the channel being looked at.
  if (A.focused && s == A.app->roster->current &&
      s->selected_channel_id == m->channel_id) {
    return;
  }

  // the sound comes whether the notification shows or not, as the
  // client's does.
  sounds_play(SOUND_MESSAGE1, MESSAGE_VOLUME);

  if (!alerts_enabled()) {
    return;
  }

  Waiting *w = calloc(1, sizeof *w);

  if (w == nullptr) {
    return;
  }

  w->shown = (Shown){
    .id = ++A.serial,
    .account_id = s->account_id,
    .channel_id = m->channel_id,
    .message_id = m->id,
  };
  describe(s->store, c, m, w->title, sizeof w->title, w->body, sizeof w->body);
  avatar_url(s->store, c, m, w->url);
  queue(w);
}

/* ========================================================================== */
/* calls. learned from the web client: while a call rings the account its     */
/* ringtone loops, and while the account is alone in a call that rings        */
/* someone it hears the tone of one waiting for an answer.                    */
/* ========================================================================== */

static bool rang(Snowflake account_id, Snowflake channel_id) {
  for (size_t i = 0; i < A.nrings; i++) {
    if (A.rings[i].account_id == account_id &&
        A.rings[i].channel_id == channel_id) {
      return true;
    }
  }

  return false;
}

// a call started to ring: the system tells of it while the window, which
// shows it with its buttons, is not looked at.
static void announce(Session *s, Snowflake channel_id) {
  Store *store = s->store;
  const Channel *c = store_channel(store, channel_id);

  if (A.focused || !alerts_enabled() || c == nullptr) {
    return;
  }

  Waiting *w = calloc(1, sizeof *w);

  if (w == nullptr) {
    return;
  }

  const User *user = c->type == CHANNEL_DM && c->nrecipients != 0
                       ? store_user(store, c->recipients[0])
                       : nullptr;

  w->shown = (Shown){
    .id = ++A.serial,
    .account_id = s->account_id,
    .channel_id = channel_id,
    .call = true,
  };
  store_channel_name(store, c, w->title, sizeof w->title);
  snprintf(w->body, sizeof w->body, "Incoming call");

  if (user != nullptr) {
    cdn_avatar(w->url, user->id, user->avatar, user->discriminator,
               AVATAR_PIXELS);
    as_png(w->url);
  }

  queue(w);
}

static void follow_calls_of(Session *s, Ring *now, size_t *nnow, bool *incoming,
                            bool *outgoing) {
  const Store *store = s->store;

  if (s->state != SESSION_READY) {
    return;
  }

  for (size_t i = 0; i < store->ncalls; i++) {
    const PrivateCall *call = &store->calls[i];

    if (call_rings(store, call)) {
      *incoming = true;

      if (*nnow < RINGS_MOST) {
        now[(*nnow)++] = (Ring){s->account_id, call->channel_id};
      }

      if (!rang(s->account_id, call->channel_id)) {
        announce(s, call->channel_id);
      }
    }

    *outgoing =
      *outgoing || (call->nringing != 0 && s->voice_error == nullptr &&
                    s->voice_channel_id == call->channel_id &&
                    store_voice_count(store, call->channel_id) < 2);
  }
}

static void follow_calls(void) {
  const Roster *roster = A.app->roster;
  Ring now[RINGS_MOST];
  size_t nnow = 0;
  bool incoming = false, outgoing = false, listed = false;

  for (size_t i = 0; i < roster->naccounts; i++) {
    Session *s = roster->accounts[i].session;

    if (s != nullptr) {
      listed = listed || s == roster->current;
      follow_calls_of(s, now, &nnow, &incoming, &outgoing);
    }
  }

  // a session played from a file is no account's.
  if (!listed && roster->current != nullptr) {
    follow_calls_of(roster->current, now, &nnow, &incoming, &outgoing);
  }

  memcpy(A.rings, now, nnow * sizeof *now);
  A.nrings = nnow;

  if (incoming) {
    sounds_loop(SOUND_CALL_RINGING, RING_VOLUME);
  }
  else {
    sounds_stop(SOUND_CALL_RINGING);
  }

  if (outgoing) {
    sounds_loop(SOUND_CALL_CALLING, RING_VOLUME);
  }
  else {
    sounds_stop(SOUND_CALL_CALLING);
  }
}

void alerts_open(Snowflake account_id, Snowflake channel_id, bool answer) {
  A.open_account = account_id;
  A.open_channel = channel_id;
  A.open_answers = answer;

  // the frame then opens the channel in the account's own session.
  if (A.app->roster->current->account_id != account_id) {
    roster_switch(A.app->roster, account_id);
  }

  civue_window_wake();
}

// the system's own: the window is already asked forward, but to decline.
static void on_activated(void *, uint64_t id, PlatformNotifyAction action) {
  for (size_t i = 0; i < A.nshown; i++) {
    if (A.shown[i].id != id) {
      continue;
    }

    Shown shown = A.shown[i];
    Session *s = session_of(shown.account_id);

    forget(i);

    if (shown.call && action == PLATFORM_NOTIFY_DECLINED) {
      if (s != nullptr) {
        session_call_decline(s, shown.channel_id);
      }
    }
    else {
      alerts_open(shown.account_id, shown.channel_id,
                  shown.call && action == PLATFORM_NOTIFY_ANSWERED);
    }

    break;
  }

  civue_window_wake();
}

// where the window closing ends the process, nothing comes back to main:
// what is still shown would stay with nothing behind it to press.
static void at_exit(void) {
  platform_notify_clear();
}

void alerts_start(App *app) {
  A.app = app;
  // a window just opened is the one looked at.
  A.focused = true;
  platform_notify_watch(on_activated, nullptr);
  atexit(at_exit);

  // a call does not wait for its ringtone to be fetched.
  for (size_t i = 0; i < countof(FETCHED_FIRST); i++) {
    if (sounds_enabled(FETCHED_FIRST[i])) {
      sounds_fetch(FETCHED_FIRST[i]);
    }
  }
}

void alerts_frame(App *app) {
  const Session *s = app->session;

  A.focused = civue_ui_current()->window_focused;

  if (A.open_channel != 0 &&
      (s->state == SESSION_READY || s->state == SESSION_RECONNECTING)) {
    if (s->account_id == A.open_account) {
      app->popover = POPOVER_NONE;
      app_select_channel(app, A.open_channel);

      if (A.open_answers) {
        voice_join(app, A.open_channel);
      }
    }

    A.open_account = A.open_channel = 0;
    A.open_answers = false;
  }

  // what is looked at needs no telling anymore, and the window shows the
  // calls that ring whatever it is on.
  for (size_t i = A.nshown; A.focused && i > 0; i--) {
    if (A.shown[i - 1].call ||
        (A.shown[i - 1].account_id == s->account_id &&
         A.shown[i - 1].channel_id == s->selected_channel_id)) {
      take_back(i - 1);
    }
  }
}

void alerts_free(void) {
  for (Waiting *w = A.waiting, *next; w != nullptr; w = next) {
    next = w->next;

    if (w->op != 0) {
      http_cancel(w->op);
    }

    free(w);
  }

  A.waiting = nullptr;
  A.nshown = 0;
  A.nrings = 0;
  A.app = nullptr;
  sounds_stop(SOUND_CALL_RINGING);
  sounds_stop(SOUND_CALL_CALLING);
  platform_notify_clear();
}
