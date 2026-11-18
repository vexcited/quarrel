#ifndef QUARREL_VOICE_AUDIO_H
#define QUARREL_VOICE_AUDIO_H

#include <stddef.h>

/* ========================================================================== */
/* the microphone and the speakers of a call, through the system's voice      */
/* processing: echo cancellation, noise suppression and gain control. sound   */
/* is mono floats at 48 kHz both ways. the callbacks run on the system's      */
/* audio thread, where nothing may block.                                     */
/* ========================================================================== */

constexpr int AUDIO_RATE = 48000;

typedef struct Audio Audio;

typedef struct {
  void *ctx;
  // what the microphone heard since the last call.
  void (*captured)(void *ctx, const float *samples, size_t count);
  // asks for the next `count` samples to play.
  void (*render)(void *ctx, float *samples, size_t count);
} AudioEvents;

typedef enum : unsigned char {
  AUDIO_OK,
  // the user refused the microphone, or has not answered yet.
  AUDIO_DENIED,
  AUDIO_FAILED,
} AudioResult;

// asks for the microphone when it was never asked, `done` then runs on the
// main thread, with whether it may be used.
void audio_ask(void (*done)(void *ctx, bool allowed), void *ctx);

// starts capturing and playing, through the devices of these ids or the
// system's own for a null one or one that is gone. null when it cannot,
// `why` then says so.
Audio *audio_start(const AudioEvents *e, const char *input, const char *output,
                   AudioResult *why);
void audio_stop(Audio *a);

/* ========================================================================== */
/* what the machine has to hear, play and film with. main thread.             */
/* ========================================================================== */

typedef struct {
  // what the system knows it by, the same from a launch to the next.
  char *id;
  char *name;
} AudioDevice;

typedef enum : unsigned char {
  AUDIO_INPUTS,
  AUDIO_OUTPUTS,
  AUDIO_CAMERAS,
} AudioKind;

// the devices of a kind, to free with audio_devices_free. listing them
// asks the user for nothing.
size_t audio_devices(AudioKind kind, AudioDevice **out);
void audio_devices_free(AudioDevice *devices, size_t count);
// the name of the device the system uses when none is picked, empty when
// it has none.
void audio_default_name(AudioKind kind, char *out, size_t cap);

#endif
