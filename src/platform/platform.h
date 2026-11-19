#ifndef QUARREL_PLATFORM_H
#define QUARREL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* ========================================================================== */
/* what Quarrel asks of the system. everything here is called from the main   */
/* thread and every callback runs on the main thread, from the run loop       */
/* civue_window_run spins. src/platform/darwin implements it. http is not     */
/* here: it goes through ciphon and cinc, see http.h.                         */
/* ========================================================================== */

/* ========================================================================== */
/* websockets. messages may be many megabytes.                                */
/* ========================================================================== */

typedef struct {
  const char *name, *value;
} PlatformHeader;

typedef struct PlatformWebSocket PlatformWebSocket;

typedef struct {
  void *ctx;
  void (*open)(void *ctx);
  // a text or a binary message, only valid during the call.
  void (*text)(void *ctx, const char *data, size_t len);
  void (*binary)(void *ctx, const uint8_t *data, size_t len);
  // the end of the connection, once and last: the peer's close code and
  // reason, or 1006 and what went wrong when it dropped.
  void (*closed)(void *ctx, int code, const char *reason);
} PlatformWebSocketEvents;

PlatformWebSocket *platform_ws_connect(const char *url,
                                       const PlatformHeader *headers,
                                       size_t nheaders,
                                       const PlatformWebSocketEvents *events);
// false when the socket is not open.
bool platform_ws_send_binary(PlatformWebSocket *ws, const uint8_t *data,
                             size_t len);
bool platform_ws_send_text(PlatformWebSocket *ws, const char *text);
// asks for a close, `closed` still runs when it is done.
void platform_ws_close(PlatformWebSocket *ws, int code, const char *reason);
// drops the socket at once. no callback runs after this.
void platform_ws_free(PlatformWebSocket *ws);

/* ========================================================================== */
/* timers.                                                                    */
/* ========================================================================== */

typedef struct PlatformTimer PlatformTimer;

// calls `fn` once after `seconds`. the timer is gone once `fn` was called or
// once it was cancelled, whichever comes first.
PlatformTimer *platform_timer(double seconds, void (*fn)(void *ctx), void *ctx);
// null is fine.
void platform_timer_cancel(PlatformTimer *timer);

/* ========================================================================== */
/* storage. secrets go to the keychain, preferences to the user's defaults.   */
/* ========================================================================== */

bool platform_secret_set(const char *key, const char *value);
// from malloc, null when there is none.
char *platform_secret_get(const char *key);
bool platform_secret_delete(const char *key);

// a null value removes the key.
void platform_pref_set(const char *key, const char *value);
// from malloc, null when there is none.
char *platform_pref_get(const char *key);

/* ========================================================================== */
/* the system.                                                                */
/* ========================================================================== */

// the system's version like "27.2.0".
void platform_os_version(char *out, size_t cap);
// the user's locale as a bcp 47 tag like "en-US".
void platform_locale(char *out, size_t cap);
// the iana name of the time zone like "Europe/Paris".
void platform_timezone(char *out, size_t cap);
// a random version 4 uuid, lowercase.
void platform_uuid(char out[static 37]);
void platform_random(void *out, size_t len);
// seconds since 1970.
double platform_now(void);

// tells the system the app has work to do while it is not in front, so
// that its timers and its sockets are not slowed down. lasts until exit.
void platform_stay_responsive(void);

// whether the machine has a network to reach the internet through.
bool platform_online(void);
// `changed` runs on the main thread when the network the machine goes
// through changes, with whether it has one now and whether it had none
// before, and when the machine wakes from sleep with one. one watcher, the
// last given.
void platform_network_watch(void (*changed)(void *ctx, bool online,
                                            bool returned),
                            void *ctx);

// calls `fn` soon on the main thread, from any thread.
void platform_post(void (*fn)(void *ctx), void *ctx);

// runs the main thread's loop until platform_stop, for tools and tests that
// have no window. an app's loop is civue_window_run.
void platform_run(void);
void platform_stop(void);

#endif
