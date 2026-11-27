#include "sounds.h"

#include <pthread.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ciren/ciren.h>

#include "platform/platform.h"
#include "sound_files.h"
#include "voice/devices.h"

#define PACK_KEY "sound_pack"

// the client plays the sounds of messages once a second at most, whatever
// comes meanwhile.
constexpr double MESSAGE_APART = 1;

typedef struct {
  const char *name;
  // seconds left between two plays, 0 for none.
  double apart;
} Entry;

static const Entry ENTRIES[SOUNDS] = {
  [SOUND_MESSAGE1] = {"message1", MESSAGE_APART},
  [SOUND_MESSAGE2] = {"message2", MESSAGE_APART},
  [SOUND_MESSAGE3] = {"message3", MESSAGE_APART},
  [SOUND_MENTION1] = {"mention1", 0},
  [SOUND_MENTION2] = {"mention2", 0},
  [SOUND_MENTION3] = {"mention3", 0},
  [SOUND_MUTE] = {"mute", 0},
  [SOUND_UNMUTE] = {"unmute", 0},
  [SOUND_DEAFEN] = {"deafen", 0},
  [SOUND_UNDEAFEN] = {"undeafen", 0},
  [SOUND_PTT_START] = {"ptt_start", 0},
  [SOUND_PTT_STOP] = {"ptt_stop", 0},
  [SOUND_USER_JOIN] = {"user_join", 0},
  [SOUND_USER_LEAVE] = {"user_leave", 0},
  [SOUND_USER_MOVED] = {"user_moved", 0},
  [SOUND_DISCONNECT] = {"disconnect", 0},
  [SOUND_RECONNECT] = {"reconnect", 0},
  [SOUND_CALL_CALLING] = {"call_calling", 0},
  [SOUND_CALL_RINGING] = {"call_ringing", 0},
  [SOUND_CAMERA_ON] = {"camera_on", 0},
  [SOUND_CAMERA_OFF] = {"camera_off", 0},
  [SOUND_STREAM_STARTED] = {"stream_started", 0},
  [SOUND_STREAM_ENDED] = {"stream_ended", 0},
  [SOUND_STREAM_USER_JOINED] = {"stream_user_joined", 0},
  [SOUND_STREAM_USER_LEFT] = {"stream_user_left", 0},
  [SOUND_STAGE_WAITING] = {"stage_waiting", 0},
  [SOUND_ACTIVITY_LAUNCH] = {"activity_launch", 0},
  [SOUND_ACTIVITY_END] = {"activity_end", 0},
  [SOUND_ACTIVITY_USER_JOIN] = {"activity_user_join", 0},
  [SOUND_ACTIVITY_USER_LEFT] = {"activity_user_left", 0},
  [SOUND_CLIP_SAVE] = {"clip_save", 0},
  [SOUND_CLIP_ERROR] = {"clip_error", 0},
  [SOUND_VOICE_MESSAGE_RECORDING_STOPPED] = {"voice_message_recording_stopped",
                                             0},
  [SOUND_HANG_STATUS_SELECT] = {"hang_status_select", 0},
  [SOUND_SUCCESS] = {"success", 0},
  [SOUND_OVERLAYUNLOCK] = {"overlayunlock", 0},
  [SOUND_DISCODO] = {"discodo", 0},
};

const char *const SOUND_PACK_NAMES[PACKS] = {
  [PACK_DISCORD] = "Discord",
  [PACK_FLUXER] = "Fluxer",
};

static const char *const PACK_IDS[PACKS] = {
  [PACK_DISCORD] = "discord",
  [PACK_FLUXER] = "fluxer",
};

static struct {
  // the user turned it off, once `asked` their preferences.
  bool off, asked;
  // where it plays from, once `placed` from their preferences.
  bool placed;
  SoundFrom from;
  char *file;
  // the file would not decode: the pack's plays until another is set.
  bool failed;
  // the volume it loops at, 0 for not at all, and it last played at.
  float looping, played;
  double played_at;
} held[SOUNDS];

static SoundPack pack;
static bool pack_known;

/* ========================================================================== */
/* where a sound plays from.                                                  */
/* ========================================================================== */

// the bytes of a pack, or the path of the user's file.
typedef struct {
  const unsigned char *bytes;
  size_t len;
  char path[1024];
} Source;

static bool same_source(const Source *a, const Source *b) {
  return a->bytes == b->bytes && strcmp(a->path, b->path) == 0;
}

SoundPack sounds_pack(void) {
  if (!pack_known) {
    char *saved = platform_pref_get(PACK_KEY);

    pack_known = true;
    pack = saved != nullptr && strcmp(saved, PACK_IDS[PACK_FLUXER]) == 0
             ? PACK_FLUXER
             : PACK_DISCORD;
    free(saved);
  }

  return pack;
}

void sounds_set_pack(SoundPack chosen) {
  if (chosen < PACKS) {
    pack_known = true;
    pack = chosen;
    platform_pref_set(PACK_KEY, PACK_IDS[chosen]);
  }
}

bool sounds_has(SoundPack in, Sound sound) {
  return in < PACKS && sound < SOUNDS && SOUND_FILES[in][sound].bytes;
}

static void from_key(Sound sound, char out[static 64]) {
  snprintf(out, 64, "sound_from_%s", ENTRIES[sound].name);
}

// the preference is a pack's name or the absolute path of a file.
static void place(Sound sound) {
  char key[64];

  if (held[sound].placed) {
    return;
  }

  from_key(sound, key);

  char *saved = platform_pref_get(key);

  held[sound].placed = true;
  held[sound].from = SOUND_FROM_PACK;

  if (saved != nullptr && saved[0] == '/') {
    held[sound].from = SOUND_FROM_FILE;
    held[sound].file = saved;
    return;
  }

  for (size_t i = 0; saved != nullptr && i < PACKS; i++) {
    if (strcmp(saved, PACK_IDS[i]) == 0) {
      held[sound].from = (SoundFrom)(SOUND_FROM_DISCORD + i);
    }
  }

  free(saved);
}

SoundFrom sounds_from(Sound sound) {
  if (sound >= SOUNDS) {
    return SOUND_FROM_PACK;
  }

  place(sound);
  return held[sound].from;
}

const char *sounds_file(Sound sound) {
  return sounds_from(sound) == SOUND_FROM_FILE ? held[sound].file : nullptr;
}

bool sounds_file_failed(Sound sound) {
  return sounds_from(sound) == SOUND_FROM_FILE && held[sound].failed;
}

void sounds_set_from(Sound sound, SoundFrom from, const char *path) {
  char key[64];

  if (sound >= SOUNDS || from > SOUND_FROM_FILE ||
      (from == SOUND_FROM_FILE && (path == nullptr || path[0] != '/'))) {
    return;
  }

  place(sound);
  free(held[sound].file);
  held[sound].file = from == SOUND_FROM_FILE ? strdup(path) : nullptr;
  held[sound].from = from;
  held[sound].failed = false;

  from_key(sound, key);
  platform_pref_set(key, from == SOUND_FROM_FILE ? path
                         : from == SOUND_FROM_PACK
                           ? nullptr
                           : PACK_IDS[from - SOUND_FROM_DISCORD]);
}

static Source source_of(Sound sound) {
  Source s = {};
  SoundFrom from = sounds_from(sound);

  if (from == SOUND_FROM_FILE && !held[sound].failed &&
      held[sound].file != nullptr) {
    snprintf(s.path, sizeof s.path, "%s", held[sound].file);
    return s;
  }

  SoundPack in = from == SOUND_FROM_DISCORD  ? PACK_DISCORD
                 : from == SOUND_FROM_FLUXER ? PACK_FLUXER
                                             : sounds_pack();
  SoundFile file = sounds_has(in, sound) ? SOUND_FILES[in][sound]
                                         : SOUND_FILES[PACK_DISCORD][sound];

  s.bytes = file.bytes;
  s.len = file.len;
  return s;
}

/* ========================================================================== */
/* the output. opening a device may take the system a moment, so a thread of  */
/* its own does it, and lets go of the device once nothing plays: an app      */
/* that tells of a message now and then keeps no device awake between.        */
/* ========================================================================== */

// what the device asks for at once: nothing here is timed to the ear.
constexpr uint32_t OUTPUT_FRAMES = 1024;
// the device is kept this long past the end of what it plays.
constexpr uint64_t LINGER_NANOS = 2'000'000'000;

typedef struct {
  Sound sound;
  float gain;
  Source from;
} Request;

// a sound to play over and over, while its gain is over 0.
typedef struct {
  float gain;
  Source from;
} Loop;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t asked = PTHREAD_COND_INITIALIZER;
static Request requests[16];
static size_t nrequests;
static Loop loops[SOUNDS];
// the loops wanted are not all the ones playing.
static bool loops_changed;
static bool running;

static void play(Sound sound, float volume);
static void loop(Sound sound);

// what failed, for the main thread.
constexpr uintptr_t FAILED_FILE = 1 << 8, FAILED_LOOP = 1 << 9;

// the user's file would not decode: the pack's plays in its place and from
// now on, a ringtone going on with it. a loop that could not start
// otherwise is not one that plays.
static void failed_on_main(void *ctx) {
  uintptr_t what = (uintptr_t)ctx;
  Sound sound = (Sound)(what & 0xff);

  if (what & FAILED_FILE) {
    held[sound].failed = true;
  }

  if ((what & FAILED_LOOP) && (what & FAILED_FILE)) {
    loop(sound);
  }
  else if (what & FAILED_FILE) {
    play(sound, held[sound].played);
  }
  else if (what & FAILED_LOOP) {
    held[sound].looping = 0;
  }
}

// the sound at the device's rate, decoded again when it plays from
// elsewhere since.
static ciren_sound *decoded(ciren *out, ciren_sound **loaded, Source *had,
                            const Source *from) {
  if (*loaded != nullptr && same_source(had, from)) {
    return *loaded;
  }

  ciren_sound_free(out, *loaded);
  *had = *from;
  *loaded = from->bytes != nullptr
              ? ciren_load_bytes(out, from->bytes, from->len)
              : ciren_load(out, from->path);
  return *loaded;
}

static void report(Sound sound, const Source *from, uintptr_t loop) {
  fprintf(stderr, "quarrel: the sound %s could not be decoded%s%s\n",
          ENTRIES[sound].name, from->bytes != nullptr ? "" : " from ",
          from->path);
  platform_post(
    failed_on_main,
    (void *)(sound | loop | (from->bytes != nullptr ? 0 : FAILED_FILE)));
}

static void *output(void *) {
  // the loops wanted as this thread last read them.
  static Loop wanted[SOUNDS];
  ciren *out = nullptr;
  ciren_sound *loaded[SOUNDS] = {};
  static Source loaded_from[SOUNDS];
  ciren_voice looping[SOUNDS] = {};
  size_t nlooping = 0;
  // by ciren's clock, when the last voice is over.
  uint64_t quiet_at = 0;

  for (;;) {
    pthread_mutex_lock(&lock);

    while (nrequests == 0 && !loops_changed) {
      // a loop keeps the device until it is stopped.
      if (out == nullptr || nlooping != 0) {
        pthread_cond_wait(&asked, &lock);
        continue;
      }

      uint64_t now = ciren_now();
      uint64_t left =
        quiet_at + LINGER_NANOS > now ? quiet_at + LINGER_NANOS - now : 0;
      struct timespec until;

      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += (time_t)(left / 1'000'000'000);
      until.tv_nsec += (long)(left % 1'000'000'000);

      if (until.tv_nsec >= 1'000'000'000) {
        until.tv_sec++;
        until.tv_nsec -= 1'000'000'000;
      }

      if (left != 0 && pthread_cond_timedwait(&asked, &lock, &until) == 0) {
        continue;
      }

      if (nrequests != 0 || loops_changed ||
          ciren_now() < quiet_at + LINGER_NANOS) {
        continue;
      }

      // nothing plays anymore.
      pthread_mutex_unlock(&lock);

      for (size_t i = 0; i < SOUNDS; i++) {
        ciren_sound_free(out, loaded[i]);
        loaded[i] = nullptr;
      }

      ciren_close(out);
      out = nullptr;
      pthread_mutex_lock(&lock);
    }

    bool reconcile = loops_changed;
    bool asked_one = nrequests != 0;
    Request r = {};

    if (reconcile) {
      memcpy(wanted, loops, sizeof wanted);
      loops_changed = false;
    }

    if (asked_one) {
      r = requests[0];
      memmove(&requests[0], &requests[1], --nrequests * sizeof *requests);
    }

    pthread_mutex_unlock(&lock);

    // the loops first: one that stops is not heard a moment more.
    for (size_t i = 0; reconcile && i < SOUNDS; i++) {
      bool on = wanted[i].gain > 0;

      if (!on && looping[i] != 0) {
        ciren_stop(out, looping[i]);
        looping[i] = 0;
        nlooping--;
        quiet_at = ciren_now() > quiet_at ? ciren_now() : quiet_at;
      }

      if (!on || looping[i] != 0) {
        continue;
      }

      if (out == nullptr) {
        out = CIREN_OPEN(.frames = OUTPUT_FRAMES, .voices = 16);
      }

      if (out != nullptr) {
        decoded(out, &loaded[i], &loaded_from[i], &wanted[i].from);
      }

      if (out != nullptr && loaded[i] != nullptr) {
        looping[i] =
          CIREN_PLAY(out, loaded[i], .gain = wanted[i].gain, .loop = true);
      }

      if (looping[i] != 0) {
        nlooping++;
        continue;
      }

      // no device or no sound: the loop is not tried again by itself.
      pthread_mutex_lock(&lock);
      loops[i].gain = 0;
      pthread_mutex_unlock(&lock);

      if (out != nullptr) {
        report((Sound)i, &wanted[i].from, FAILED_LOOP);
      }
      else {
        platform_post(failed_on_main, (void *)(i | FAILED_LOOP));
      }
    }

    if (!asked_one) {
      continue;
    }

    if (out == nullptr) {
      out = CIREN_OPEN(.frames = OUTPUT_FRAMES, .voices = 16);
    }

    if (out == nullptr) {
      continue;
    }

    // decoded to the rate of the device, which may be another next time.
    if (decoded(out, &loaded[r.sound], &loaded_from[r.sound], &r.from) ==
        nullptr) {
      report(r.sound, &r.from, 0);
      continue;
    }

    uint64_t over =
      ciren_now() + (uint64_t)(ciren_sound_seconds(loaded[r.sound]) * 1e9);

    CIREN_PLAY(out, loaded[r.sound], .gain = r.gain);
    quiet_at = over > quiet_at ? over : quiet_at;
  }

  return nullptr;
}

// starts the thread of the output, with the lock held.
static bool output_running(void) {
  if (!running) {
    pthread_t thread;

    running = pthread_create(&thread, nullptr, output, nullptr) == 0;

    if (running) {
      pthread_detach(thread);
    }
  }

  return running;
}

static void play(Sound sound, float volume) {
  float level = devices_output_volume() * volume;
  Request r = {
    .sound = sound,
    .gain = level < 1 ? level : 1,
    .from = source_of(sound),
  };

  held[sound].played_at = platform_now();
  held[sound].played = volume;

  // one the binary leaves out is silent.
  if (r.gain <= 0 || (r.from.bytes == nullptr && r.from.path[0] == '\0')) {
    return;
  }

  pthread_mutex_lock(&lock);

  if (output_running() && nrequests < countof(requests)) {
    requests[nrequests++] = r;
    pthread_cond_signal(&asked);
  }

  pthread_mutex_unlock(&lock);
}

// tells the output of the loop `held` wants for a sound, or of none.
static void loop(Sound sound) {
  float level = devices_output_volume() * held[sound].looping;
  Source from = source_of(sound);

  if (from.bytes == nullptr && from.path[0] == '\0') {
    level = 0;
  }

  pthread_mutex_lock(&lock);

  if (level > 0 ? output_running() : running) {
    loops[sound].gain = level < 1 ? level : 1;
    loops[sound].from = from;
    loops_changed = true;
    pthread_cond_signal(&asked);
  }

  pthread_mutex_unlock(&lock);
}

/* ========================================================================== */
/* what the app asks.                                                         */
/* ========================================================================== */

static void pref_key(Sound sound, char out[static 64]) {
  snprintf(out, 64, "sound_%s", ENTRIES[sound].name);
}

bool sounds_enabled(Sound sound) {
  if (sound >= SOUNDS) {
    return false;
  }

  if (!held[sound].asked) {
    char key[64];

    pref_key(sound, key);

    char *saved = platform_pref_get(key);

    held[sound].asked = true;
    held[sound].off = saved != nullptr && saved[0] == '0';
    free(saved);
  }

  return !held[sound].off;
}

void sounds_set_enabled(Sound sound, bool on) {
  char key[64];

  if (sound >= SOUNDS) {
    return;
  }

  pref_key(sound, key);
  held[sound].asked = true;
  held[sound].off = !on;
  platform_pref_set(key, on ? "1" : "0");

  if (!on) {
    sounds_stop(sound);
  }
}

void sounds_play(Sound sound, float volume) {
  if (sound >= SOUNDS || volume <= 0 || !sounds_enabled(sound)) {
    return;
  }

  double apart = ENTRIES[sound].apart;

  if (apart > 0 && platform_now() - held[sound].played_at < apart) {
    return;
  }

  play(sound, volume);
}

void sounds_preview(Sound sound) {
  if (sound < SOUNDS) {
    play(sound, 1);
  }
}

void sounds_loop(Sound sound, float volume) {
  if (sound >= SOUNDS || volume <= 0 || !sounds_enabled(sound) ||
      held[sound].looping > 0) {
    return;
  }

  held[sound].looping = volume;
  loop(sound);
}

void sounds_stop(Sound sound) {
  if (sound >= SOUNDS || held[sound].looping <= 0) {
    return;
  }

  held[sound].looping = 0;
  loop(sound);
}
