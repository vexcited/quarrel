#ifndef QUARREL_SOUND_FILES_H
#define QUARREL_SOUND_FILES_H

#include <stddef.h>

#include "sounds.h"

// a sound's mp3 in the binary, null bytes for one its pack does not have
// or the app never plays.
typedef struct {
  const unsigned char *bytes;
  size_t len;
} SoundFile;

extern const SoundFile SOUND_FILES[PACKS][SOUNDS];

#endif
