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

static const char *const OPTION_KEYS[VOICE_OPTIONS] = {
  [VOICE_QOS] = "voice_qos",
  [VOICE_SWITCH_WARNING] = "voice_switch_warning",
  [VOICE_ALWAYS_PREVIEW] = "voice_always_preview",
  [VOICE_ECHO] = "voice_echo",
  [VOICE_AUTO_SENSITIVITY] = "voice_auto_sensitivity",
};

static char *picked[3];
// as discord has them before anyone touches them.
static atomic_uchar profile, noise = NOISE_VOICE;
static _Atomic float sensitivity = -50;
static atomic_bool options[VOICE_OPTIONS] = {
  [VOICE_QOS] = true,
  [VOICE_SWITCH_WARNING] = true,
  [VOICE_ECHO] = true,
  [VOICE_AUTO_SENSITIVITY] = true,
};
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

  for (int option = 0; option < VOICE_OPTIONS; option++) {
    char *saved = platform_pref_get(OPTION_KEYS[option]);

    if (saved != nullptr && saved[0] != '\0') {
      atomic_store(&options[option], saved[0] == '1');
    }

    free(saved);
  }

  char *saved = platform_pref_get("voice_profile");

  if (saved != nullptr && saved[0] >= '0' && saved[0] < '0' + PROFILES) {
    atomic_store(&profile, (unsigned char)(saved[0] - '0'));
  }

  free(saved);
  saved = platform_pref_get("voice_noise");

  if (saved != nullptr && saved[0] >= '0' && saved[0] < '0' + NOISES) {
    atomic_store(&noise, (unsigned char)(saved[0] - '0'));
  }

  free(saved);
  saved = platform_pref_get("voice_sensitivity");

  if (saved != nullptr && saved[0] != '\0') {
    atomic_store(&sensitivity, -clamp(-strtof(saved, nullptr), 100));
  }

  free(saved);
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

bool devices_option(VoiceOption option) {
  load();
  return atomic_load_explicit(&options[option], memory_order_relaxed);
}

void devices_set_option(VoiceOption option, bool on) {
  load();
  atomic_store(&options[option], on);
  platform_pref_set(OPTION_KEYS[option], on ? "1" : "0");

  if (option == VOICE_ECHO) {
    version++;
  }
}

VoiceProfile devices_profile(void) {
  load();
  return (VoiceProfile)atomic_load_explicit(&profile, memory_order_relaxed);
}

void devices_set_profile(VoiceProfile to) {
  char digit[2] = {(char)('0' + to), '\0'};

  load();
  bool echo = devices_input().echo;

  atomic_store(&profile, to);
  platform_pref_set("voice_profile", digit);

  if (echo != devices_input().echo) {
    version++;
  }
}

VoiceInput devices_input(void) {
  switch (devices_profile()) {
    case PROFILE_ISOLATION:
      return (VoiceInput){NOISE_VOICE, true, true, 0};
    case PROFILE_STUDIO:
      return (VoiceInput){NOISE_OFF, true, true, 0};
    default:
      return (VoiceInput){
        devices_noise(),
        devices_option(VOICE_ECHO),
        devices_option(VOICE_AUTO_SENSITIVITY),
        devices_sensitivity(),
      };
  }
}

VoiceNoise devices_noise(void) {
  load();
  return (VoiceNoise)atomic_load_explicit(&noise, memory_order_relaxed);
}

void devices_set_noise(VoiceNoise to) {
  char digit[2] = {(char)('0' + to), '\0'};

  load();
  atomic_store(&noise, to);
  platform_pref_set("voice_noise", digit);
}

float devices_sensitivity(void) {
  load();
  return atomic_load_explicit(&sensitivity, memory_order_relaxed);
}

void devices_set_sensitivity(float decibels) {
  load();
  atomic_store(&sensitivity, -clamp(-decibels, 100));
}

void devices_save_sensitivity(void) {
  char digits[16];

  snprintf(digits, sizeof digits, "%.1f", (double)devices_sensitivity());
  platform_pref_set("voice_sensitivity", digits);
}
