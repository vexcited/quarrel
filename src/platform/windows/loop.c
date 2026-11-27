#include <stdlib.h>
#include <winsock2.h>

#include "win.h"

/* ========================================================================== */
/* the main thread's loop is civue's, which dispatches every message of the   */
/* thread: a window of Quarrel's own that only gets messages has what other   */
/* threads post and the timers that are due come to it. platform_run spins    */
/* the same messages without a window.                                        */
/* ========================================================================== */

enum : UINT {
  POSTED = WM_APP + 1,
  TIMERS,
};

static HWND window;

typedef struct {
  void (*fn)(void *ctx);
  void *ctx;
} Post;

typedef struct {
  Post *v;
  size_t len, cap;
} Posts;

// what other threads posted, run in the order it came by one message: the
// list being run and the one filling swap, so that posting allocates nothing
// once both grew.
static SRWLOCK posts_lock = SRWLOCK_INIT;
static Posts pending, running;
static bool scheduled;

static void run_posts(void) {
  AcquireSRWLockExclusive(&posts_lock);
  Posts batch = pending;
  pending = running;
  pending.len = 0;
  scheduled = false;
  ReleaseSRWLockExclusive(&posts_lock);

  for (size_t i = 0; i < batch.len; i++) {
    batch.v[i].fn(batch.v[i].ctx);
  }

  AcquireSRWLockExclusive(&posts_lock);

  // a batch posted meanwhile went to the other list, this one is spare.
  running = batch;
  running.len = 0;
  ReleaseSRWLockExclusive(&posts_lock);
}

void platform_post(void (*fn)(void *ctx), void *ctx) {
  AcquireSRWLockExclusive(&posts_lock);

  if (pending.len == pending.cap) {
    size_t cap = pending.cap ? pending.cap * 2 : 64;
    Post *grown = realloc(pending.v, cap * sizeof *grown);

    if (grown == nullptr) {
      ReleaseSRWLockExclusive(&posts_lock);
      abort();
    }

    pending = (Posts){grown, pending.len, cap};
  }

  pending.v[pending.len++] = (Post){fn, ctx};

  bool wake = !scheduled;

  scheduled = true;
  ReleaseSRWLockExclusive(&posts_lock);

  if (wake) {
    PostMessageW(window, POSTED, 0, 0);
  }
}

/* ========================================================================== */
/* timers, on the main thread in the order they are due. the thread of a     */
/* high resolution waitable timer posts when the first is due, which wakes   */
/* the loop within a millisecond where SetTimer would round to the tick of   */
/* the system, 15.6 ms.                                                       */
/* ========================================================================== */

typedef struct Timer Timer;

struct Timer {
  Timer *next;
  uint64_t id;
  // in the ticks of the performance counter.
  int64_t due;
  void (*fn)(void *ctx);
  void *ctx;
};

static Timer *timers;
static uint64_t last_id;
static HANDLE waitable;
static int64_t ticks_per_second;

static int64_t ticks(void) {
  LARGE_INTEGER now;

  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

static DWORD WINAPI wait_timers(void *) {
  while (WaitForSingleObject(waitable, INFINITE) == WAIT_OBJECT_0) {
    PostMessageW(window, TIMERS, 0, 0);
  }

  return 0;
}

// the waitable timer goes off when the first is due.
static void arm(void) {
  if (timers == nullptr) {
    CancelWaitableTimer(waitable);
    return;
  }

  int64_t left = timers->due - ticks();
  // a negative time is relative, in hundreds of nanoseconds.
  LARGE_INTEGER after = {
    .QuadPart = left > 0 ? -(left * 10'000'000 / ticks_per_second) - 1 : -1,
  };

  SetWaitableTimer(waitable, &after, 0, nullptr, nullptr, false);
}

static void run_timers(void) {
  int64_t now = ticks();
  // a timer made meanwhile waits for the next round, one of no time too.
  uint64_t made = last_id;

  while (timers != nullptr && timers->due <= now && timers->id <= made) {
    Timer *t = timers;

    timers = t->next;
    t->fn(t->ctx);
    free(t);
  }

  arm();
}

PlatformTimer *platform_timer(double seconds, void (*fn)(void *ctx),
                              void *ctx) {
  Timer *t = malloc(sizeof *t);

  if (t == nullptr) {
    abort();
  }

  double after = seconds > 0 ? seconds * (double)ticks_per_second : 0;

  *t = (Timer){
    .id = ++last_id,
    .due = ticks() + (after < 4e18 ? (int64_t)after : (int64_t)4e18),
    .fn = fn,
    .ctx = ctx,
  };

  Timer **at = &timers;

  while (*at != nullptr && (*at)->due <= t->due) {
    at = &(*at)->next;
  }

  t->next = *at;
  *at = t;

  if (timers == t) {
    arm();
  }

  return (PlatformTimer *)(uintptr_t)t->id;
}

void platform_timer_cancel(PlatformTimer *timer) {
  uint64_t id = (uint64_t)(uintptr_t)timer;

  for (Timer **at = &timers; id != 0 && *at != nullptr; at = &(*at)->next) {
    if ((*at)->id == id) {
      Timer *t = *at;
      bool first = at == &timers;

      *at = t->next;
      free(t);

      if (first) {
        arm();
      }

      return;
    }
  }
}

/* ========================================================================== */
/* the window, made on the main thread before main.                           */
/* ========================================================================== */

static LRESULT CALLBACK on_message(HWND hwnd, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  switch (message) {
    case POSTED:
      run_posts();
      return 0;
    case TIMERS:
      run_timers();
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

[[gnu::constructor]] static void start(void) {
  LARGE_INTEGER frequency;

  QueryPerformanceFrequency(&frequency);
  ticks_per_second = frequency.QuadPart;

  // the sockets of a call are winsock's, which wants this once.
  WSADATA wsa;

  WSAStartup(MAKEWORD(2, 2), &wsa);

  WNDCLASSEXW kind = {
    .cbSize = sizeof kind,
    .lpfnWndProc = on_message,
    .hInstance = GetModuleHandleW(nullptr),
    .lpszClassName = L"QuarrelLoop",
  };

  RegisterClassExW(&kind);
  window = CreateWindowExW(0, kind.lpszClassName, L"Quarrel", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, kind.hInstance, nullptr);

  // windows before 10's 1803 has no high resolution, the tick then.
  waitable = CreateWaitableTimerExW(
    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  if (waitable == nullptr) {
    waitable = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }

  if (window == nullptr || waitable == nullptr) {
    abort();
  }

  HANDLE thread = CreateThread(nullptr, 0, wait_timers, nullptr, 0, nullptr);

  if (thread == nullptr) {
    abort();
  }

  CloseHandle(thread);
}

static bool spinning;

void platform_run(void) {
  MSG msg;
  bool was = spinning;

  spinning = true;

  while (spinning && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  spinning = was;
}

void platform_stop(void) {
  spinning = false;
  PostMessageW(window, WM_NULL, 0, 0);
}
