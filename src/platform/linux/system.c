#include <gio/gio.h>
#include <stddefer.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "platform/platform.h"

void platform_os_version(char *out, size_t cap) {
  struct utsname sys = {};

  uname(&sys);
  snprintf(out, cap, "%s", sys.release);
}

// the first language the user asked for, "fr_FR.UTF-8@euro" made "fr-FR".
void platform_locale(char *out, size_t cap) {
  const char *const *names = g_get_language_names();
  const char *name = names[0];

  if (name == nullptr || strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0) {
    name = "en-US";
  }

  size_t n = strcspn(name, ".@");

  snprintf(out, cap, "%.*s", (int)n, name);

  for (char *c = out; *c; c++) {
    *c = *c == '_' ? '-' : *c;
  }
}

void platform_timezone(char *out, size_t cap) {
  GTimeZone *zone = g_time_zone_new_local();

  defer g_time_zone_unref(zone);

  const char *name = g_time_zone_get_identifier(zone);

  // glib names a zone it read from /etc/localtime by the file's path.
  const char *found = strstr(name, "zoneinfo/");
  name = found ? found + strlen("zoneinfo/") : name;
  name = name[0] == ':' ? name + 1 : name;
  snprintf(out, cap, "%s",
           name[0] && strcmp(name, "localtime") != 0 ? name : "UTC");
}

void platform_random(void *out, size_t len) {
  for (size_t done = 0; done < len;) {
    ssize_t n = getrandom((char *)out + done, len - done, 0);

    done += n > 0 ? (size_t)n : 0;
  }
}

void platform_uuid(char out[static 37]) {
  uint8_t b[16];

  platform_random(b, sizeof b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(out, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
}

double platform_now(void) {
  struct timespec t;

  clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// nothing on linux slows down the timers or the sockets of an app that is
// hidden: a compositor only stops asking it for frames.
void platform_stay_responsive(void) {
}

static GNetworkMonitor *monitor;
static bool online = true;
static void (*network_changed)(void *ctx, bool online, bool returned);
static void *network_ctx;

static void on_network(GNetworkMonitor *, gboolean available, void *) {
  bool was = online;

  online = available;

  if (network_changed != nullptr) {
    network_changed(network_ctx, online, online && !was);
  }
}

// logind says the machine woke with PrepareForSleep false.
static void on_sleep(GDBusConnection *, const char *, const char *,
                     const char *, const char *, GVariant *args, void *) {
  gboolean sleeping = true;

  g_variant_get(args, "(b)", &sleeping);

  if (!sleeping && online && network_changed != nullptr) {
    network_changed(network_ctx, true, false);
  }
}

static void on_system_bus(GObject *, GAsyncResult *result, void *) {
  GDBusConnection *bus = g_bus_get_finish(result, nullptr);

  if (bus != nullptr) {
    g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
      "PrepareForSleep", "/org/freedesktop/login1", nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, on_sleep, nullptr, nullptr);
  }
}

static void watch_network(void) {
  if (monitor != nullptr) {
    return;
  }

  monitor = g_network_monitor_get_default();
  online = g_network_monitor_get_network_available(monitor);
  g_signal_connect(monitor, "network-changed", G_CALLBACK(on_network), nullptr);
  g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, on_system_bus, nullptr);
}

bool platform_online(void) {
  watch_network();
  return online;
}

void platform_network_watch(void (*changed)(void *ctx, bool online,
                                            bool returned),
                            void *ctx) {
  network_changed = changed;
  network_ctx = ctx;
  watch_network();
}

bool platform_cache_path(const char *name, char *out, size_t cap) {
  char *ours = g_build_filename(g_get_user_cache_dir(), "quarrel", nullptr);

  defer g_free(ours);

  return g_mkdir_with_parents(ours, 0o700) == 0 &&
         (size_t)snprintf(out, cap, "%s/%s", ours, name) < cap;
}
