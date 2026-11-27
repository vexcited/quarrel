#include <windows.h>

#include <bcrypt.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

void platform_os_version(char *out, size_t cap) {
  // GetVersionEx says what the manifest claims, ntdll what runs.
  RTL_OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
  LONG(WINAPI * get)(RTL_OSVERSIONINFOW *) =
    (LONG(WINAPI *)(RTL_OSVERSIONINFOW *))(void *)GetProcAddress(
      GetModuleHandleW(L"ntdll"), "RtlGetVersion");

  if (get == nullptr || get(&v) != 0) {
    v = (RTL_OSVERSIONINFOW){.dwMajorVersion = 10};
  }

  snprintf(out, cap, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
           v.dwBuildNumber);
}

// a sort of its own follows the name, "de-DE_phoneb".
void platform_locale(char *out, size_t cap) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH];
  char *tag = GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0
                ? win_utf8(name)
                : nullptr;

  snprintf(out, cap, "%.*s", tag != nullptr ? (int)strcspn(tag, "_") : 5,
           tag != nullptr && tag[0] ? tag : "en-US");
  free(tag);
}

// the C of icu, which windows ships since 10's 1903. its names are
// unversioned there and its strings utf-16.
typedef int32_t(WINAPI *DefaultZone)(wchar_t *out, int32_t cap, int *error);
typedef int32_t(WINAPI *ZoneOfWindows)(const wchar_t *id, int32_t len,
                                       const char *region, wchar_t *out,
                                       int32_t cap, int *error);

void platform_timezone(char *out, size_t cap) {
  HMODULE icu =
    LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  DefaultZone default_zone =
    icu ? (DefaultZone)(void *)GetProcAddress(icu, "ucal_getDefaultTimeZone")
        : nullptr;
  ZoneOfWindows of_windows = icu ? (ZoneOfWindows)(void *)GetProcAddress(
                                     icu, "ucal_getTimeZoneIDForWindowsID")
                                 : nullptr;
  wchar_t name[128] = L"";
  int error = 0;
  int32_t len = 0;

  if (default_zone != nullptr) {
    len = default_zone(name, 127, &error);
  }

  // what windows calls the zone, its region's iana name.
  if ((error > 0 || len <= 0) && of_windows != nullptr) {
    DYNAMIC_TIME_ZONE_INFORMATION zone;

    error = 0;

    if (GetDynamicTimeZoneInformation(&zone) != TIME_ZONE_ID_INVALID) {
      len = of_windows(zone.TimeZoneKeyName, -1, nullptr, name, 127, &error);
    }
  }

  name[error > 0 || len <= 0 ? 0 : len < 127 ? len : 127] = L'\0';

  char *iana = name[0] ? win_utf8(name) : nullptr;

  snprintf(out, cap, "%s", iana != nullptr ? iana : "UTC");
  free(iana);

  if (icu != nullptr) {
    FreeLibrary(icu);
  }
}

void platform_random(void *out, size_t len) {
  for (size_t done = 0; done < len;) {
    ULONG n = len - done < 1u << 30 ? (ULONG)(len - done) : 1u << 30;

    if (BCryptGenRandom(nullptr, (uint8_t *)out + done, n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
      done += n;
    }
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
  FILETIME t;

  GetSystemTimePreciseAsFileTime(&t);

  // hundreds of nanoseconds since 1601.
  uint64_t since = (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;

  return (double)(since - 116'444'736'000'000'000) / 1e7;
}

// windows 11 runs an app in the background as it saves power: its threads
// on the slow cores at a low clock, its timers late as the system's tick
// rounds them. Quarrel asks for neither, which keeps the gateway's events
// and heartbeats on time.
void platform_stay_responsive(void) {
  PROCESS_POWER_THROTTLING_STATE state = {
    .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
    .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                   PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
    .StateMask = 0,
  };

  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                        sizeof state);
}

/* ========================================================================== */
/* the network, as the hints of iphlpapi tell it since windows 10's 2004, and */
/* the machine waking, which mingw's headers have neither of.                */
/* ========================================================================== */

typedef enum {
  HINT_UNKNOWN,
  HINT_NONE,
  HINT_LOCAL,
  HINT_INTERNET,
  HINT_CONSTRAINED,
  HINT_HIDDEN,
} ConnectivityLevel;

typedef struct {
  ConnectivityLevel level;
  int cost;
  BOOLEAN approaching_limit, over_limit, roaming;
} ConnectivityHint;

typedef void(WINAPI *HintChanged)(void *ctx, ConnectivityHint hint);
typedef DWORD(WINAPI *NotifyHints)(HintChanged fn, void *ctx, BOOLEAN initial,
                                   HANDLE *handle);
typedef DWORD(WINAPI *GetHint)(ConnectivityHint *hint);

typedef struct {
  ULONG(CALLBACK *fn)(void *ctx, ULONG type, void *setting);
  void *ctx;
} PowerCallback;

constexpr DWORD DEVICE_NOTIFY_CALLBACK = 2;

static ConnectivityLevel level = HINT_UNKNOWN;
static void (*network_changed)(void *ctx, bool online, bool returned);
static void *network_ctx;

// a network without the internet is one still, as on the other systems:
// the probe that tells them apart is often kept out.
static bool online_at(ConnectivityLevel at) {
  return at != HINT_NONE;
}

static void on_network(void *ctx) {
  ConnectivityLevel now = (ConnectivityLevel)(uintptr_t)ctx;
  bool was = online_at(level);

  if (now == level) {
    return;
  }

  level = now;

  if (network_changed != nullptr) {
    network_changed(network_ctx, online_at(now), online_at(now) && !was);
  }
}

static void WINAPI on_hint(void *, ConnectivityHint hint) {
  platform_post(on_network, (void *)(uintptr_t)hint.level);
}

static void on_wake(void *) {
  if (online_at(level) && network_changed != nullptr) {
    network_changed(network_ctx, true, false);
  }
}

static ULONG CALLBACK on_power(void *, ULONG type, void *) {
  if (type == PBT_APMRESUMEAUTOMATIC) {
    platform_post(on_wake, nullptr);
  }

  return 0;
}

static void watch_network(void) {
  static bool watching;

  if (watching) {
    return;
  }

  watching = true;

  HMODULE ip =
    LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  GetHint get =
    ip != nullptr
      ? (GetHint)(void *)GetProcAddress(ip, "GetNetworkConnectivityHint")
      : nullptr;
  NotifyHints notify = ip != nullptr
                         ? (NotifyHints)(void *)GetProcAddress(
                             ip, "NotifyNetworkConnectivityHintChange")
                         : nullptr;
  ConnectivityHint hint = {};
  HANDLE hints = nullptr;

  if (get != nullptr && get(&hint) == NO_ERROR) {
    level = hint.level;
  }

  if (notify != nullptr) {
    notify(on_hint, nullptr, false, &hints);
  }

  static PowerCallback power = {on_power, nullptr};

  RegisterSuspendResumeNotification(&power, DEVICE_NOTIFY_CALLBACK);
}

bool platform_online(void) {
  watch_network();
  return online_at(level);
}

void platform_network_watch(void (*changed)(void *ctx, bool online,
                                            bool returned),
                            void *ctx) {
  network_changed = changed;
  network_ctx = ctx;
  watch_network();
}

bool platform_cache_path(const char *name, char *out, size_t cap) {
  char ours[1024];

  return win_folder(&FOLDERID_LocalAppData, ours, sizeof ours) &&
         (size_t)snprintf(out, cap, "%s\\%s", ours, name) < cap;
}
