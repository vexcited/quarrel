#ifndef QUARREL_VOICE_AUDIO_H
#define QUARREL_VOICE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

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
  // the sound could not start after all. main thread, may be null.
  void (*failed)(void *ctx);
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
// system's own for a null one or one that is gone. `raw` leaves the
// microphone alone: no echo taken out, nothing of the system's voice
// processing. null when it may not, `why` then says so. neither this nor
// audio_stop waits for the system: the sound starts a moment later, and
// `failed` says when it does not.
Audio *audio_start(const AudioEvents *e, const char *input, const char *output,
                   bool raw, AudioResult *why);
// the microphone was heard: the system takes a moment, seconds with some
// devices, between the start and the first sound. any thread.
bool audio_live(const Audio *a);
// no callback runs once it returned.
void audio_stop(Audio *a);

// how the system shapes the microphone is its user's choice alone, made in
// the system's own menu, which no app may make for them. opens that menu.
void audio_show_modes(void);

// the system has that menu, and says which device is its own by its name.
#ifdef __APPLE__
constexpr bool AUDIO_HAS_MODES = true;
#define AUDIO_SYSTEM_DEFAULT "macOS Default"
#else
constexpr bool AUDIO_HAS_MODES = false;
#define AUDIO_SYSTEM_DEFAULT "System Default"
#endif

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
// bumped when a device comes or goes, or the system picks another as its
// own. the first call starts watching, `changed` then runs on the main
// thread at each bump.
uint32_t audio_devices_version(void (*changed)(void));
// the name of the device the system uses when none is picked, empty when
// it has none.
void audio_default_name(AudioKind kind, char *out, size_t cap);

#endif
