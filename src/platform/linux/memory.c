#include <malloc.h>

// glibc's heap as a client that rests most of the time wants it, set before
// anything is asked of it.
[[gnu::constructor]] static void lean(void) {
#ifdef __GLIBC__
  // a block this large is the system's own, and goes back to it the moment
  // it is freed: an image's bytes, a payload's text. left alone glibc
  // raises this to the largest block freed, 32 MiB at most, and the
  // megabytes a ready takes would then come from the heap, which keeps
  // them between what was allocated meanwhile.
  mallopt(M_MMAP_THRESHOLD, 64 << 10);
  // what is free at the top of the heap past this goes back too.
  mallopt(M_TRIM_THRESHOLD, 256 << 10);
  // a dozen threads, asleep most of the time, share one heap past the main
  // thread's, where each would grow its own and keep what it freed there.
  mallopt(M_ARENA_MAX, 2);
#endif
}
