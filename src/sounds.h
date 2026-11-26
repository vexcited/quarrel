#ifndef QUARREL_SOUNDS_H
#define QUARREL_SOUNDS_H

#include <stdint.h>

/* ========================================================================== */
/* the sounds of discord's client, which its site serves as mp3 files. one    */
/* is fetched the first time it is wanted and kept in the user's caches, and  */
/* they play through ciren, on the system's own output as the client's do.    */
/* main thread.                                                               */
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

// plays a sound, `volume` from 0 to 1 of what the speakers are set to. one
// that was never fetched plays once it came, and one that would not come is
// asked for again a minute later. nothing for one the user turned off.
void sounds_play(Sound sound, float volume);
// plays a sound over and over until sounds_stop, as a ringtone goes. one
// that already loops goes on as it is.
void sounds_loop(Sound sound, float volume);
// ends the loop of a sound. fine when it has none.
void sounds_stop(Sound sound);
// fetches a sound ahead of its first use.
void sounds_fetch(Sound sound);

// whether the user wants a sound played, kept from a launch to the next
// and the same for every account, as discord keeps it on the machine.
bool sounds_enabled(Sound sound);
void sounds_set_enabled(Sound sound, bool on);

#endif
