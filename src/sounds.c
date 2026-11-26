#include "sounds.h"

#include <pthread.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <ciren/ciren.h>

#include "http.h"
#include "platform/platform.h"
#include "voice/devices.h"

// learned from the web client: its main bundle maps "./message1.mp3" to a
// module that exports the path of the file, named after a hash of what it
// holds, under the /assets of discord.com. the name is what the client's
// code plays, the hash what its build of the day serves it as.
#define SOUNDS_BASE "https://discord.com/assets/"

constexpr size_t SOUND_BYTES_MOST = 4u << 20;
constexpr double RETRY_SECONDS = 60;
// the client plays the sounds of messages once a second at most, whatever
// comes meanwhile.
constexpr double MESSAGE_APART = 1;

typedef struct {
  const char *name, *asset;
  // seconds left between two plays, 0 for none.
  double apart;
} Entry;

static const Entry ENTRIES[SOUNDS] = {
  [SOUND_MESSAGE1] = {"message1", "3ed22d14f3c30bc4", MESSAGE_APART},
  [SOUND_MESSAGE2] = {"message2", "91f9f6fa8cc9ea5c", MESSAGE_APART},
  [SOUND_MESSAGE3] = {"message3", "4f53c2f31ea0cdd8", MESSAGE_APART},
  [SOUND_MENTION1] = {"mention1", "3baf62fc7aebb0a3", 0},
  [SOUND_MENTION2] = {"mention2", "e0a0235d2fa32496", 0},
  [SOUND_MENTION3] = {"mention3", "260dfacf63b6cb13", 0},
  [SOUND_MUTE] = {"mute", "2d3b4ba32c34c862", 0},
  [SOUND_UNMUTE] = {"unmute", "e74c4a06134a20e4", 0},
  [SOUND_DEAFEN] = {"deafen", "529ff198eac567af", 0},
  [SOUND_UNDEAFEN] = {"undeafen", "b150f03c89944403", 0},
  [SOUND_PTT_START] = {"ptt_start", "f5c8ec31889c5317", 0},
  [SOUND_PTT_STOP] = {"ptt_stop", "34b5969be3a063ba", 0},
  [SOUND_USER_JOIN] = {"user_join", "b135ff6c8e091b43", 0},
  [SOUND_USER_LEAVE] = {"user_leave", "7b9a183742515fc2", 0},
  [SOUND_USER_MOVED] = {"user_moved", "4003230e4086b680", 0},
  [SOUND_DISCONNECT] = {"disconnect", "dd0911025fe00b2e", 0},
  [SOUND_RECONNECT] = {"reconnect", "7dfc49b36dea3a6c", 0},
  [SOUND_CALL_CALLING] = {"call_calling", "5146737af413d88e", 0},
  [SOUND_CALL_RINGING] = {"call_ringing", "c2a7111bb44b8da0", 0},
  [SOUND_CAMERA_ON] = {"camera_on", "855607d0932ea396", 0},
  [SOUND_CAMERA_OFF] = {"camera_off", "b1dd204729fe78b4", 0},
  [SOUND_STREAM_STARTED] = {"stream_started", "abe52a3c92953edb", 0},
  [SOUND_STREAM_ENDED] = {"stream_ended", "cbff07b5a83e0321", 0},
  [SOUND_STREAM_USER_JOINED] = {"stream_user_joined", "3d2af2764581bb2e", 0},
  [SOUND_STREAM_USER_LEFT] = {"stream_user_left", "24ae9dc0184bd570", 0},
  [SOUND_STAGE_WAITING] = {"stage_waiting", "7458c5c3bc415b01", 0},
  [SOUND_ACTIVITY_LAUNCH] = {"activity_launch", "a71f183dc4de32a3", 0},
  [SOUND_ACTIVITY_END] = {"activity_end", "878f7e7f2b3bf972", 0},
  [SOUND_ACTIVITY_USER_JOIN] = {"activity_user_join", "31d2551c00b7c91e", 0},
  [SOUND_ACTIVITY_USER_LEFT] = {"activity_user_left", "9f34f40112215f5e", 0},
  [SOUND_CLIP_SAVE] = {"clip_save", "fece0a7646748c01", 0},
  [SOUND_CLIP_ERROR] = {"clip_error", "a95d4944eee85a14", 0},
  [SOUND_VOICE_MESSAGE_RECORDING_STOPPED] = {"voice_message_recording_stopped",
                                             "17e98c85a3d66341", 0},
  [SOUND_HANG_STATUS_SELECT] = {"hang_status_select", "c27233e99b338a12", 0},
  [SOUND_SUCCESS] = {"success", "90241c95e51f9261", 0},
  [SOUND_OVERLAYUNLOCK] = {"overlayunlock", "1d05544763057d17", 0},
  [SOUND_DISCODO] = {"discodo", "cb85be48267eb1b4", 0},
};

static struct {
  // the user turned it off, once `asked` their preferences.
  bool off, asked;
  // its file is in the caches.
  bool cached;
  bool fetching;
  // the volume to play at once fetched, 0 for not at all.
  float waiting;
  // the volume it loops at, 0 for not at all.
  float looping;
  double failed_at, played_at;
} held[SOUNDS];

static bool cache_path(Sound sound, char out[static 1024]) {
  char name[64];

  snprintf(name, sizeof name, "sound-%s.mp3", ENTRIES[sound].asset);
  return platform_cache_path(name, out, 1024);
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
  char path[1024];
} Request;

// a sound to play over and over, while its gain is over 0.
typedef struct {
  float gain;
  char path[1024];
} Loop;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t asked = PTHREAD_COND_INITIALIZER;
static Request requests[16];
static size_t nrequests;
static Loop loops[SOUNDS];
// the loops wanted are not all the ones playing.
static bool loops_changed;
static bool running;

// the file would not decode: it is fetched again, later.
static void unreadable_on_main(void *ctx) {
  Sound sound = (Sound)(uintptr_t)ctx;
  char path[1024];

  if (cache_path(sound, path)) {
    remove(path);
  }

  held[sound].cached = false;
  held[sound].failed_at = platform_now();
}

// a loop that could not start is not one that plays.
static void silent_on_main(void *ctx) {
  held[(Sound)(uintptr_t)ctx].looping = 0;
}

static void *output(void *) {
  // the loops wanted as this thread last read them.
  static Loop wanted[SOUNDS];
  ciren *out = nullptr;
  ciren_sound *loaded[SOUNDS] = {};
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

      if (out != nullptr && loaded[i] == nullptr) {
        loaded[i] = ciren_load(out, wanted[i].path);
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
      platform_post(silent_on_main, (void *)(uintptr_t)i);

      if (out != nullptr && loaded[i] == nullptr) {
        fprintf(stderr, "quarrel: the sound %s could not be decoded\n",
                ENTRIES[i].name);
        platform_post(unreadable_on_main, (void *)(uintptr_t)i);
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
    if (loaded[r.sound] == nullptr) {
      loaded[r.sound] = ciren_load(out, r.path);

      if (loaded[r.sound] == nullptr) {
        fprintf(stderr, "quarrel: the sound %s could not be decoded\n",
                ENTRIES[r.sound].name);
        platform_post(unreadable_on_main, (void *)(uintptr_t)r.sound);
        continue;
      }
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
  Request r = {.sound = sound, .gain = level < 1 ? level : 1};

  held[sound].played_at = platform_now();

  if (r.gain <= 0 || !cache_path(sound, r.path)) {
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
  char path[1024] = "";

  if (level > 0 && !cache_path(sound, path)) {
    level = 0;
  }

  pthread_mutex_lock(&lock);

  if (level > 0 ? output_running() : running) {
    loops[sound].gain = level < 1 ? level : 1;
    memcpy(loops[sound].path, path, sizeof path);
    loops_changed = true;
    pthread_cond_signal(&asked);
  }

  pthread_mutex_unlock(&lock);
}

/* ========================================================================== */
/* fetching.                                                                  */
/* ========================================================================== */

// the file is whole or not there: it is renamed into place.
static bool write_cached(Sound sound, const void *bytes, size_t len) {
  char path[1024], writing[1040];

  if (!cache_path(sound, path)) {
    return false;
  }

  snprintf(writing, sizeof writing, "%s.new", path);

  FILE *file = fopen(writing, "wb");

  if (file == nullptr) {
    return false;
  }

  bool ok = fwrite(bytes, 1, len, file) == len;

  ok = fclose(file) == 0 && ok;

  if (!ok || rename(writing, path) != 0) {
    remove(writing);
    return false;
  }

  return true;
}

static void on_fetched(void *ctx, const HttpResponse *res) {
  Sound sound = (Sound)(uintptr_t)ctx;
  const char *type = http_header(res, "content-type");
  float waiting = held[sound].waiting;

  held[sound].fetching = false;
  held[sound].waiting = 0;

  // an error page is no sound to keep.
  if (res->status != 200 || res->body_len == 0 || type == nullptr ||
      strncasecmp(type, "audio/", 6) != 0 ||
      !write_cached(sound, res->body, res->body_len)) {
    fprintf(stderr, "quarrel: the sound %s did not come: %d %s\n",
            ENTRIES[sound].name, res->status, res->error ? res->error : "");
    held[sound].failed_at = platform_now();
    return;
  }

  held[sound].cached = true;

  if (waiting > 0) {
    play(sound, waiting);
  }

  if (held[sound].looping > 0) {
    loop(sound);
  }
}

// whether the sound is there to play now.
static bool fetch(Sound sound) {
  char path[1024];

  if (held[sound].cached) {
    return true;
  }

  if (held[sound].fetching ||
      platform_now() - held[sound].failed_at < RETRY_SECONDS) {
    return false;
  }

  if (cache_path(sound, path) && access(path, R_OK) == 0) {
    held[sound].cached = true;
    return true;
  }

  char url[160];

  snprintf(url, sizeof url, SOUNDS_BASE "%s.mp3", ENTRIES[sound].asset);

  HttpRequest req = {.url = url, .max_response_len = SOUND_BYTES_MOST};

  held[sound].fetching =
    http_send(&req, on_fetched, (void *)(uintptr_t)sound) != 0;
  return false;
}

void sounds_fetch(Sound sound) {
  if (sound < SOUNDS) {
    fetch(sound);
  }
}

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

  if (on) {
    fetch(sound);
  }
  else {
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

  if (fetch(sound)) {
    play(sound, volume);
  }
  else if (held[sound].fetching) {
    held[sound].waiting = volume;
    // what comes while it is fetched is one sound, not one each.
    held[sound].played_at = platform_now();
  }
}

void sounds_loop(Sound sound, float volume) {
  if (sound >= SOUNDS || volume <= 0 || !sounds_enabled(sound) ||
      held[sound].looping > 0) {
    return;
  }

  held[sound].looping = volume;

  // one still being fetched loops once it came.
  if (fetch(sound)) {
    loop(sound);
  }
}

void sounds_stop(Sound sound) {
  if (sound >= SOUNDS || held[sound].looping <= 0) {
    return;
  }

  held[sound].looping = 0;
  loop(sound);
}
