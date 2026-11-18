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
// bumped when the microphone or the speakers picked change.
uint32_t devices_version(void);

// what the microphone's sound is multiplied by, 0 to 1.
float devices_input_volume(void);
// the same for what plays, 0 to 2.
float devices_output_volume(void);
void devices_set_input_volume(float volume);
void devices_set_output_volume(float volume);
// writes the volumes down, once a slider is let go.
void devices_save_volumes(void);

#endif
