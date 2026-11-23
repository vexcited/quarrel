#include "voice/mictest.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

#include "voice/audio.h"
#include "voice/denoise.h"
#include "voice/devices.h"

// a second of sound, a power of two.
constexpr uint32_t RING = 65536;
// what is held back before it plays, so that the speakers never run dry.
constexpr uint32_t DELAY = AUDIO_RATE / 10;

static Audio *audio;
static bool wanted, denied;
static uint32_t devices;
static void (*on_changed)(void);
// asked for the microphone, to tell an answer that comes after a stop.
static uint32_t asks;

static float ring[RING];
static atomic_uint head, tail;
static _Atomic float level;
static bool primed;
// made before the sound starts, freed after it stopped.
static Denoise *denoise;
// the most the system hands at a time, see audio.m.
constexpr size_t HEARD_MAX = 4096;

static void on_captured(void *, const float *heard, size_t count) {
  uint32_t at = atomic_load_explicit(&head, memory_order_relaxed);
  float volume = devices_input_volume(), energy = 0;
  float samples[HEARD_MAX];

  if (count > HEARD_MAX) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    samples[i] = heard[i];
  }

  VoiceNoise noise = devices_input().noise;

  if (denoise != nullptr && noise != NOISE_OFF) {
    denoise_run(denoise, samples, count, noise == NOISE_VOICE);
  }

  for (size_t i = 0; i < count; i++) {
    float sample = samples[i] * volume;

    ring[(at + i) % RING] = sample;
    energy += sample * sample;
  }

  atomic_store_explicit(&head, at + (uint32_t)count, memory_order_release);

  if (count != 0) {
    atomic_store_explicit(&level, sqrtf(energy / (float)count),
                          memory_order_relaxed);
  }
}

static void on_render(void *, float *samples, size_t count)
  [[clang::nonblocking]] {
  uint32_t end = atomic_load_explicit(&head, memory_order_acquire);
  uint32_t at = atomic_load_explicit(&tail, memory_order_relaxed);
  uint32_t held = end - at;
  float volume = devices_output_volume();

  // too far behind, or not yet far enough: back to the delay.
  if (held > 4 * DELAY) {
    at = end - DELAY;
    held = DELAY;
  }

  if (!primed && held < DELAY) {
    held = 0;
  }

  primed = held >= count;

  for (size_t i = 0; i < count; i++) {
    float sample = i < held ? ring[(at + i) % RING] * volume : 0;

    samples[i] = fminf(fmaxf(sample, -1), 1);
  }

  atomic_store_explicit(&tail, at + (held < count ? held : (uint32_t)count),
                        memory_order_release);
}

static void on_failed(void *) {
  mictest_stop();

  if (on_changed != nullptr) {
    on_changed();
  }
}

static void open(void) {
  AudioEvents events = {nullptr, on_captured, on_render, on_failed};
  AudioResult why = AUDIO_OK;

  atomic_store(&head, 0);
  atomic_store(&tail, 0);
  primed = false;
  devices = devices_version();
  denoise_free(denoise);
  denoise = denoise_new();
  audio = audio_start(&events, devices_get(AUDIO_INPUTS),
                      devices_get(AUDIO_OUTPUTS), !devices_input().echo,
                      &why);
  denied = audio == nullptr && why == AUDIO_DENIED;
  wanted = audio != nullptr;
}

static void on_asked(void *ctx, bool) {
  if (!wanted || (uint32_t)(uintptr_t)ctx != asks) {
    return;
  }

  open();

  if (on_changed != nullptr) {
    on_changed();
  }
}

void mictest_start(void (*changed)(void)) {
  if (wanted) {
    return;
  }

  wanted = true;
  denied = false;
  on_changed = changed;
  audio_ask(on_asked, (void *)(uintptr_t)++asks);
}

void mictest_stop(void) {
  wanted = false;
  audio_stop(audio);
  audio = nullptr;
  denoise_free(denoise);
  denoise = nullptr;
  atomic_store(&level, 0);
}

bool mictest_running(void) {
  return wanted;
}

bool mictest_starting(void) {
  return wanted && (audio == nullptr || !audio_live(audio));
}

bool mictest_denied(void) {
  return denied;
}

float mictest_level(void) {
  return audio != nullptr
           ? atomic_load_explicit(&level, memory_order_relaxed)
           : 0;
}

void mictest_follow(void) {
  if (audio != nullptr && devices != devices_version()) {
    audio_stop(audio);
    audio = nullptr;
    open();
  }
}
