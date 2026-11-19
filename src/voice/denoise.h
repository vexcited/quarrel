#ifndef QUARREL_VOICE_DENOISE_H
#define QUARREL_VOICE_DENOISE_H

#include <stddef.h>

/* ========================================================================== */
/* the noise taken out of a microphone by rnnoise, a small network that       */
/* runs anywhere, so that isolating a voice asks nothing of the system. mono  */
/* floats at 48 kHz. one thread at a time for each.                           */
/* ========================================================================== */

typedef struct Denoise Denoise;

Denoise *denoise_new(void);
void denoise_free(Denoise *d);
// takes the noise out of the samples, in place, and with `gate` everything
// out while nobody speaks: the network says how likely each 10 ms are a
// voice, which a key or a click is not. what comes out is 20 ms late, one
// block to hear and one to let a voice start before it was known to be one.
// it never blocks.
void denoise_run(Denoise *d, float *samples, size_t count, bool gate);

#endif
