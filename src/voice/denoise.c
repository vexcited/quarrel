#include "voice/denoise.h"

#include <stdlib.h>

#include <rnnoise.h>

// what rnnoise takes at a time, 10 ms.
constexpr int BLOCK = 480;
// it was trained on 16 bit samples held in floats.
constexpr float SCALE = 32768;

// learned from werman's noise suppression plugin, which gates rnnoise the
// same way: a block opens the gate when it is this likely a voice, a
// threshold it advises against keys, and the gate stays open this many
// blocks after the last one, the ends of words being quiet.
constexpr float VOICE = 0.85f;
constexpr int HOLD = 25;
constexpr int WARM = 10;

struct Denoise {
  DenoiseState *state;
  // what is being heard, what came out of the network before it, held back
  // a block, and what plays.
  float heard[BLOCK], clean[BLOCK], out[BLOCK];
  int filled;
  // blocks the gate stays open for, and how open it was at the end of the
  // last block, 0 to 1.
  int open;
  float gain;
  int blocks;
};

Denoise *denoise_new(void) {
  Denoise *d = calloc(1, sizeof *d);

  if (d == nullptr) {
    return nullptr;
  }

  d->state = rnnoise_create(nullptr);

  if (d->state == nullptr) {
    free(d);
    return nullptr;
  }

  return d;
}

void denoise_free(Denoise *d) {
  if (d == nullptr) {
    return;
  }

  rnnoise_destroy(d->state);
  free(d);
}

static void block(Denoise *d, bool gate) {
  float next[BLOCK];
  float voice = rnnoise_process_frame(d->state, next, d->heard);

  // the network takes anything for a voice in its first blocks.
  if (d->blocks < WARM) {
    d->blocks++;
  }
  else if (voice >= VOICE) {
    d->open = HOLD;
  }
  else if (d->open > 0) {
    d->open--;
  }

  // the block before this one plays, open already when this one is a voice.
  float to = !gate || d->open > 0 ? 1 : 0;

  for (int i = 0; i < BLOCK; i++) {
    float gain = d->gain + (to - d->gain) * (float)(i + 1) / BLOCK;

    d->out[i] = d->clean[i] * gain;
    d->clean[i] = next[i];
  }

  d->gain = to;
}

void denoise_run(Denoise *d, float *samples, size_t count, bool gate) {
  for (size_t i = 0; i < count; i++) {
    d->heard[d->filled] = samples[i] * SCALE;
    samples[i] = d->out[d->filled] / SCALE;

    if (++d->filled == BLOCK) {
      block(d, gate);
      d->filled = 0;
    }
  }
}
