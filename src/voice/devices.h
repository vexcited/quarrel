#ifndef QUARREL_VOICE_DEVICES_H
#define QUARREL_VOICE_DEVICES_H

#include <stdint.h>

#include "voice/audio.h"

/* ========================================================================== */
/* what the user picked to be heard and to hear with, kept from a launch to   */
/* the next. the same for every account. main thread, but for the volumes,    */
/* which any thread may read.                                                 */
/* ========================================================================== */

// the id of the device picked, null for the system's own.
const char *devices_get(AudioKind kind);
void devices_set(AudioKind kind, const char *id);
// bumped when the microphone or the speakers picked change, or whether
// the echo is taken out.
uint32_t devices_version(void);

// what the microphone's sound is multiplied by, 0 to 1.
float devices_input_volume(void) [[clang::nonblocking]];
// the same for what plays, 0 to 2.
float devices_output_volume(void) [[clang::nonblocking]];
void devices_set_input_volume(float volume);
void devices_set_output_volume(float volume);
// writes the volumes down, once a slider is let go.
void devices_save_volumes(void);

typedef enum : unsigned char {
  // the packets of a call say they are urgent to the routers on their way.
  VOICE_QOS,
  // asks before leaving a call for another.
  VOICE_SWITCH_WARNING,
  VOICE_ALWAYS_PREVIEW,
  // the two of the custom profile that are on or off, see VoiceInput.
  VOICE_ECHO,
  VOICE_AUTO_SENSITIVITY,
  VOICE_OPTIONS,
} VoiceOption;

// any thread may read one.
bool devices_option(VoiceOption option);
void devices_set_option(VoiceOption option, bool on);


// what is done to the microphone before anyone hears it.
typedef enum : unsigned char {
  // the noise taken out by Quarrel's own network, and nothing sent but a
  // voice.
  PROFILE_ISOLATION,
  // the microphone as the system hands it, shaped by its own Mic Mode.
  PROFILE_STUDIO,
  // what the user set, one thing at a time.
  PROFILE_CUSTOM,
  PROFILES,
} VoiceProfile;

// any thread may read it.
VoiceProfile devices_profile(void);
void devices_set_profile(VoiceProfile profile);

typedef enum : unsigned char {
  NOISE_OFF,
  // the background taken out, by rnnoise.
  NOISE_STANDARD,
  // and everything muted while nobody speaks.
  NOISE_VOICE,
  NOISES,
} VoiceNoise;

// what the profile comes down to.
typedef struct {
  VoiceNoise noise;
  // the system takes what the speakers play out of the microphone. without
  // it the microphone is left alone, and the system's Mic Mode with it.
  bool echo;
  // the microphone is sent when Quarrel finds it loud enough, or else when
  // it is louder than `sensitivity`, in decibels under the loudest, -100
  // to 0.
  bool auto_sensitivity;
  float sensitivity;
} VoiceInput;

// any thread may read it.
VoiceInput devices_input(void);
// of the custom profile.
VoiceNoise devices_noise(void);
void devices_set_noise(VoiceNoise noise);
float devices_sensitivity(void);
void devices_set_sensitivity(float decibels);
// writes it down, once the slider is let go.
void devices_save_sensitivity(void);

#endif
