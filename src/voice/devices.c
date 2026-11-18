#include "voice/devices.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

static const char *const KEYS[] = {
  [AUDIO_INPUTS] = "voice_input",
  [AUDIO_OUTPUTS] = "voice_output",
  [AUDIO_CAMERAS] = "voice_camera",
};

static char *picked[3];
static bool loaded;
static uint32_t version;
static _Atomic float input_volume = 1, output_volume = 1;

static float clamp(float value, float most) {
  return value < 0 ? 0 : value > most ? most : value;
}

static void load_volume(const char *key, _Atomic float *volume, float most) {
  char *saved = platform_pref_get(key);

  if (saved != nullptr && saved[0] != '\0') {
    atomic_store(volume, clamp(strtof(saved, nullptr), most));
  }

  free(saved);
}

static void load(void) {
  if (loaded) {
    return;
  }

  loaded = true;

  for (int kind = 0; kind < 3; kind++) {
    picked[kind] = platform_pref_get(KEYS[kind]);

    if (picked[kind] != nullptr && picked[kind][0] == '\0') {
      free(picked[kind]);
      picked[kind] = nullptr;
    }
  }

  load_volume("voice_input_volume", &input_volume, 1);
  load_volume("voice_output_volume", &output_volume, 2);
}

const char *devices_get(AudioKind kind) {
  load();
  return picked[kind];
}

void devices_set(AudioKind kind, const char *id) {
  load();
  free(picked[kind]);
  picked[kind] = id != nullptr && id[0] != '\0' ? strdup(id) : nullptr;
  platform_pref_set(KEYS[kind], picked[kind] != nullptr ? picked[kind] : "");

  if (kind != AUDIO_CAMERAS) {
    version++;
  }
}

uint32_t devices_version(void) {
  return version;
}

float devices_input_volume(void) {
  return atomic_load_explicit(&input_volume, memory_order_relaxed);
}

float devices_output_volume(void) {
  return atomic_load_explicit(&output_volume, memory_order_relaxed);
}

void devices_set_input_volume(float volume) {
  load();
  atomic_store(&input_volume, clamp(volume, 1));
}

void devices_set_output_volume(float volume) {
  load();
  atomic_store(&output_volume, clamp(volume, 2));
}

void devices_save_volumes(void) {
  char digits[16];

  snprintf(digits, sizeof digits, "%.3f", (double)devices_input_volume());
  platform_pref_set("voice_input_volume", digits);
  snprintf(digits, sizeof digits, "%.3f", (double)devices_output_volume());
  platform_pref_set("voice_output_volume", digits);
}
