#ifndef QUARREL_SOUNDS_H
#define QUARREL_SOUNDS_H

#include <stdint.h>

/* ========================================================================== */
/* the sounds of the app, in packs kept in the binary: discord's and          */
/* fluxer's. each plays from the pack the user chose for it or from a file of */
/* theirs, discord's until they choose. they play through ciren, on the       */
/* system's own output as discord's client does. main thread.                 */
/* ========================================================================== */
// by the names the client knows them by.
typedef enum : uint8_t {
  // a message worth a notification came.
  SOUND_MESSAGE1,
  // a friend started playing, and a message came in the channel looked at.
  SOUND_MESSAGE2,
  SOUND_MESSAGE3,
  SOUND_MENTION1,
  SOUND_MENTION2,
  SOUND_MENTION3,
  // the account's own microphone and headphones.
  SOUND_MUTE,
  SOUND_UNMUTE,
  SOUND_DEAFEN,
  SOUND_UNDEAFEN,
  SOUND_PTT_START,
  SOUND_PTT_STOP,
  // a call: someone came, also the account itself, someone left, the
  // account was moved, and its connection dropped or was let go.
  SOUND_USER_JOIN,
  SOUND_USER_LEAVE,
  SOUND_USER_MOVED,
  SOUND_DISCONNECT,
  // asked to speak on a stage.
  SOUND_RECONNECT,
  // a private call rings, here or at the other end. both loop.
  SOUND_CALL_CALLING,
  SOUND_CALL_RINGING,
  SOUND_CAMERA_ON,
  SOUND_CAMERA_OFF,
  SOUND_STREAM_STARTED,
  SOUND_STREAM_ENDED,
  SOUND_STREAM_USER_JOINED,
  SOUND_STREAM_USER_LEFT,
  SOUND_STAGE_WAITING,
  SOUND_ACTIVITY_LAUNCH,
  SOUND_ACTIVITY_END,
  SOUND_ACTIVITY_USER_JOIN,
  SOUND_ACTIVITY_USER_LEFT,
  SOUND_CLIP_SAVE,
  SOUND_CLIP_ERROR,
  SOUND_VOICE_MESSAGE_RECORDING_STOPPED,
  SOUND_HANG_STATUS_SELECT,
  SOUND_SUCCESS,
  SOUND_OVERLAYUNLOCK,
  SOUND_DISCODO,
  SOUNDS,
} Sound;

typedef enum : uint8_t {
  PACK_DISCORD,
  PACK_FLUXER,
  PACKS,
} SoundPack;

extern const char *const SOUND_PACK_NAMES[PACKS];

// where a sound plays from. a pack that lacks it plays discord's.
typedef enum : uint8_t {
  SOUND_FROM_DISCORD,
  SOUND_FROM_FLUXER,
  SOUND_FROM_FILE,
} SoundFrom;

// plays a sound, `volume` from 0 to 1 of what the speakers are set to.
// nothing for one the user turned off.
void sounds_play(Sound sound, float volume);
// plays a sound once at the speakers' volume, turned off or not.
void sounds_preview(Sound sound);
// plays a sound over and over until sounds_stop, as a ringtone goes. one
// that already loops goes on as it is.
void sounds_loop(Sound sound, float volume);
// ends the loop of a sound. fine when it has none.
void sounds_stop(Sound sound);

// whether the user wants a sound played, kept from a launch to the next
// and the same for every account, as discord keeps it on the machine.
bool sounds_enabled(Sound sound);
void sounds_set_enabled(Sound sound, bool on);

// whether a pack has a sound of its own.
bool sounds_has(SoundPack pack, Sound sound);

SoundFrom sounds_from(Sound sound);
// the user's file, null unless the sound plays from one.
const char *sounds_file(Sound sound);
// the user's file would not play, discord's did instead.
bool sounds_file_failed(Sound sound);
// `path` is the file of SOUND_FROM_FILE, ignored otherwise.
void sounds_set_from(Sound sound, SoundFrom from, const char *path);

#endif
