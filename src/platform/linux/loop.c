#include <glib.h>
#include <pthread.h>
#include <stdlib.h>

#include <civue/window.h>

#include "platform/platform.h"

// glib's default loop is the main thread's: civue's window spins it, and
// platform_run does without a window.
[[gnu::constructor]] static void drive(void) {
  civue_glib_drive();
}

typedef struct {
  void (*fn)(void *ctx);
  void *ctx;
} Post;

typedef struct {
  Post *v;
  size_t len, cap;
} Posts;

// what other threads posted, run in the order it came by one source: the
// list being run and the one filling swap, so that posting allocates nothing
// once both grew.
static pthread_mutex_t posts_lock = PTHREAD_MUTEX_INITIALIZER;
static Posts pending, running;
static bool scheduled;

static void run_posts(void *) {
  pthread_mutex_lock(&posts_lock);
  Posts batch = pending;
  pending = running;
  pending.len = 0;
  scheduled = false;
  pthread_mutex_unlock(&posts_lock);

  for (size_t i = 0; i < batch.len; i++) {
    batch.v[i].fn(batch.v[i].ctx);
  }

  pthread_mutex_lock(&posts_lock);

  // a batch posted meanwhile went to the other list, this one is spare.
  running = batch;
  running.len = 0;
  pthread_mutex_unlock(&posts_lock);
}

void platform_post(void (*fn)(void *ctx), void *ctx) {
  pthread_mutex_lock(&posts_lock);

  if (pending.len == pending.cap) {
    size_t cap = pending.cap ? pending.cap * 2 : 64;
    Post *grown = realloc(pending.v, cap * sizeof *grown);

    if (grown == nullptr) {
      pthread_mutex_unlock(&posts_lock);
      abort();
    }

    pending = (Posts){grown, pending.len, cap};
  }

  pending.v[pending.len++] = (Post){fn, ctx};

  if (!scheduled) {
    scheduled = true;
    g_timeout_add_once(0, run_posts, nullptr);
  }

  pthread_mutex_unlock(&posts_lock);
}

PlatformTimer *platform_timer(double seconds, void (*fn)(void *ctx),
                              void *ctx) {
  double ms = seconds > 0 ? seconds * 1000 : 0;
  guint id =
    g_timeout_add_once(ms < G_MAXUINT ? (guint)ms : G_MAXUINT, fn, ctx);

  return (PlatformTimer *)(uintptr_t)id;
}

void platform_timer_cancel(PlatformTimer *timer) {
  GSource *source =
    timer != nullptr
      ? g_main_context_find_source_by_id(nullptr, (guint)(uintptr_t)timer)
      : nullptr;

  if (source != nullptr) {
    g_source_destroy(source);
  }
}

static GMainLoop *loop;

void platform_run(void) {
  GMainLoop *mine = g_main_loop_new(nullptr, false);

  loop = mine;
  g_main_loop_run(mine);
  loop = nullptr;
  g_main_loop_unref(mine);
}

void platform_stop(void) {
  if (loop != nullptr) {
    g_main_loop_quit(loop);
  }
}
