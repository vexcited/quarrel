#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/window.h>

#include "platform/platform.h"

/* ========================================================================== */
/* notifications through org.freedesktop.Notifications, the session bus's     */
/* service every desktop has a server for. the server numbers what it shows,  */
/* and tells by that number what was pressed or went away.                    */
/* ========================================================================== */

#define SERVICE "org.freedesktop.Notifications"
#define OBJECT "/org/freedesktop/Notifications"

typedef struct Shown Shown;

struct Shown {
  Shown *next;
  uint64_t id;
  // the server's number, 0 until it answered.
  uint32_t number;
  // taken back before the server answered, closed once it does.
  bool closed;
  // what waits for the server's capabilities, null once sent.
  char *title, *body, *image;
  bool call;
  // what the server hands before it says the notification was pressed,
  // for the compositor to let the window come forward. null for none.
  char *token;
};

static GDBusConnection *bus;
static Shown *shown;
// the server was asked what it can do, it answered, and it reads the body
// as markup.
static bool asked, known, markup;
static void (*activated)(void *ctx, uint64_t id, PlatformNotifyAction action);
static void *activated_ctx;

static Shown **find(uint64_t id) {
  for (Shown **at = &shown; *at != nullptr; at = &(*at)->next) {
    if ((*at)->id == id) {
      return at;
    }
  }

  return nullptr;
}

static Shown **find_number(uint32_t number) {
  for (Shown **at = &shown; *at != nullptr; at = &(*at)->next) {
    if ((*at)->number == number && number != 0) {
      return at;
    }
  }

  return nullptr;
}

static void drop(Shown **at) {
  Shown *n = *at;

  *at = n->next;
  free(n->title);
  free(n->body);
  free(n->image);
  free(n->token);
  free(n);
}

static void close_number(uint32_t number) {
  g_dbus_connection_call(bus, SERVICE, OBJECT, SERVICE, "CloseNotification",
                         g_variant_new("(u)", number), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static void on_signal(GDBusConnection *, const char *, const char *,
                      const char *, const char *signal, GVariant *args,
                      void *) {
  uint32_t number = 0;
  bool named = g_variant_is_of_type(args, G_VARIANT_TYPE("(us)"));

  // a server of the 1.2 specification says this right before ActionInvoked.
  if (strcmp(signal, "ActivationToken") == 0 && named) {
    const char *token = nullptr;

    g_variant_get(args, "(u&s)", &number, &token);

    Shown **at = find_number(number);

    if (at != nullptr) {
      free((*at)->token);
      (*at)->token = strdup(token);
    }
  }
  else if (strcmp(signal, "ActionInvoked") == 0 && named) {
    const char *name = nullptr;

    g_variant_get(args, "(u&s)", &number, &name);

    Shown **at = find_number(number);

    if (at == nullptr) {
      return;
    }

    uint64_t id = (*at)->id;
    PlatformNotifyAction action =
      strcmp(name, "answer") == 0    ? PLATFORM_NOTIFY_ANSWERED
      : strcmp(name, "decline") == 0 ? PLATFORM_NOTIFY_DECLINED
                                     : PLATFORM_NOTIFY_OPENED;

    // with the server's token the compositor knows the user asked. without
    // one the window asks by itself, and may only be flagged.
    if (action != PLATFORM_NOTIFY_DECLINED) {
      civue_window_activate((*at)->token);
    }

    // a server may leave what was pressed on the screen.
    close_number(number);
    drop(at);

    if (activated != nullptr) {
      activated(activated_ctx, id, action);
    }
  }
  else if (strcmp(signal, "NotificationClosed") == 0 &&
           g_variant_is_of_type(args, G_VARIANT_TYPE("(uu)"))) {
    g_variant_get(args, "(uu)", &number, nullptr);

    Shown **at = find_number(number);

    if (at != nullptr) {
      drop(at);
    }
  }
}

static bool connect(void) {
  if (bus != nullptr) {
    return true;
  }

  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);

  if (bus == nullptr) {
    return false;
  }

  g_dbus_connection_signal_subscribe(bus, SERVICE, SERVICE, nullptr, OBJECT,
                                     nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                     on_signal, nullptr, nullptr);
  return true;
}

static void on_notified(GObject *, GAsyncResult *result, void *ctx) {
  uint64_t id = *(uint64_t *)ctx;
  GVariant *answer = g_dbus_connection_call_finish(bus, result, nullptr);
  Shown **at = find(id);
  uint32_t number = 0;

  free(ctx);

  if (answer != nullptr) {
    g_variant_get(answer, "(u)", &number);
    g_variant_unref(answer);
  }

  if (at == nullptr) {
    return;
  }

  // no server, or the app took it back meanwhile.
  if (number == 0 || (*at)->closed) {
    if (number != 0) {
      close_number(number);
    }

    drop(at);
    return;
  }

  (*at)->number = number;
}

static void send(Shown *n) {
  GVariantBuilder actions, hints;
  char *body = markup ? g_markup_escape_text(n->body, -1) : g_strdup(n->body);
  uint64_t *id = malloc(sizeof *id);

  if (id == nullptr) {
    abort();
  }

  *id = n->id;

  // the action called default is the one a press on the notification runs.
  g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
  g_variant_builder_add(&actions, "s", "default");
  g_variant_builder_add(&actions, "s", "Open");

  if (n->call) {
    g_variant_builder_add(&actions, "s", "answer");
    g_variant_builder_add(&actions, "s", "Answer");
    g_variant_builder_add(&actions, "s", "decline");
    g_variant_builder_add(&actions, "s", "Decline");
  }

  // the app plays the sound itself.
  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "desktop-entry",
                        g_variant_new_string("quarrel"));
  g_variant_builder_add(
    &hints, "{sv}", "category",
    g_variant_new_string(n->call ? "call.incoming" : "im.received"));
  g_variant_builder_add(&hints, "{sv}", "suppress-sound",
                        g_variant_new_boolean(true));

  // a critical one is not taken away after a moment: a call stays for as
  // long as it rings.
  if (n->call) {
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(2));
  }

  if (n->image != nullptr) {
    g_variant_builder_add(&hints, "{sv}", "image-path",
                          g_variant_new_string(n->image));
  }

  g_dbus_connection_call(bus, SERVICE, OBJECT, SERVICE, "Notify",
                         g_variant_new("(susssasa{sv}i)", "Quarrel", (guint32)0,
                                       "", n->title, body, &actions, &hints,
                                       (gint32)(n->call ? 0 : -1)),
                         G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
                         nullptr, on_notified, id);

  g_free(body);
  free(n->title);
  free(n->body);
  free(n->image);
  n->title = n->body = n->image = nullptr;
}

// what waited for the server's answer, in the order it came: the list
// holds the latest first.
static void send_waiting(Shown *n) {
  if (n == nullptr) {
    return;
  }

  send_waiting(n->next);

  if (n->title != nullptr) {
    send(n);
  }
}

static void on_capabilities(GObject *, GAsyncResult *result, void *) {
  GVariant *answer = g_dbus_connection_call_finish(bus, result, nullptr);

  if (answer != nullptr) {
    const char **names = nullptr;

    g_variant_get(answer, "(^a&s)", &names);

    for (size_t i = 0; names != nullptr && names[i] != nullptr; i++) {
      markup = markup || strcmp(names[i], "body-markup") == 0;
    }

    g_free(names);
    g_variant_unref(answer);
  }

  known = true;
  send_waiting(shown);
}

// what no server would take as text.
static char *valid(const char *text) {
  return g_utf8_make_valid(text != nullptr ? text : "", -1);
}

void platform_notify(const PlatformNotification *n) {
  if (n == nullptr || n->id == 0 || !connect()) {
    return;
  }

  Shown *made = calloc(1, sizeof *made);

  if (made == nullptr) {
    abort();
  }

  made->id = n->id;
  made->title = valid(n->title);
  made->body = valid(n->body);
  made->image = n->image != nullptr ? g_strdup(n->image) : nullptr;
  made->call = n->call;
  made->next = shown;
  shown = made;

  if (known) {
    send(made);
    return;
  }

  if (!asked) {
    asked = true;
    g_dbus_connection_call(bus, SERVICE, OBJECT, SERVICE, "GetCapabilities",
                           nullptr, G_VARIANT_TYPE("(as)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, on_capabilities,
                           nullptr);
  }
}

void platform_notify_close(uint64_t id) {
  Shown **at = find(id);

  if (at == nullptr) {
    return;
  }

  // not sent yet: it never is.
  if ((*at)->title != nullptr) {
    drop(at);
  }
  else if ((*at)->number == 0) {
    (*at)->closed = true;
  }
  else {
    close_number((*at)->number);
    drop(at);
  }
}

void platform_notify_watch(void (*fn)(void *ctx, uint64_t id,
                                      PlatformNotifyAction action),
                           void *ctx) {
  activated = fn;
  activated_ctx = ctx;
}

void platform_notify_clear(void) {
  while (shown != nullptr) {
    if (shown->number != 0) {
      close_number(shown->number);
    }

    drop(&shown);
  }

  // the loop that would send them is over.
  if (bus != nullptr) {
    g_dbus_connection_flush_sync(bus, nullptr, nullptr);
  }
}
