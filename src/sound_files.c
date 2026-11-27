#include "sound_files.h"

// only the sounds the app plays, the others of assets/ stay out of the
// binary.

static const unsigned char DISCORD_MESSAGE1[] = {
#embed "../assets/discord/sounds/message1.mp3"
};
static const unsigned char DISCORD_MUTE[] = {
#embed "../assets/discord/sounds/mute.mp3"
};
static const unsigned char DISCORD_UNMUTE[] = {
#embed "../assets/discord/sounds/unmute.mp3"
};
static const unsigned char DISCORD_DEAFEN[] = {
#embed "../assets/discord/sounds/deafen.mp3"
};
static const unsigned char DISCORD_UNDEAFEN[] = {
#embed "../assets/discord/sounds/undeafen.mp3"
};
static const unsigned char DISCORD_USER_JOIN[] = {
#embed "../assets/discord/sounds/user_join.mp3"
};
static const unsigned char DISCORD_USER_LEAVE[] = {
#embed "../assets/discord/sounds/user_leave.mp3"
};
static const unsigned char DISCORD_USER_MOVED[] = {
#embed "../assets/discord/sounds/user_moved.mp3"
};
static const unsigned char DISCORD_DISCONNECT[] = {
#embed "../assets/discord/sounds/disconnect.mp3"
};
static const unsigned char DISCORD_CALL_CALLING[] = {
#embed "../assets/discord/sounds/call_calling.mp3"
};
static const unsigned char DISCORD_CALL_RINGING[] = {
#embed "../assets/discord/sounds/call_ringing.mp3"
};
static const unsigned char DISCORD_STREAM_STARTED[] = {
#embed "../assets/discord/sounds/stream_started.mp3"
};

static const unsigned char FLUXER_MESSAGE1[] = {
#embed "../assets/fluxer/sounds/message.mp3"
};
static const unsigned char FLUXER_MUTE[] = {
#embed "../assets/fluxer/sounds/mute.mp3"
};
static const unsigned char FLUXER_UNMUTE[] = {
#embed "../assets/fluxer/sounds/unmute.mp3"
};
static const unsigned char FLUXER_DEAFEN[] = {
#embed "../assets/fluxer/sounds/deaf.mp3"
};
static const unsigned char FLUXER_UNDEAFEN[] = {
#embed "../assets/fluxer/sounds/undeaf.mp3"
};
static const unsigned char FLUXER_USER_JOIN[] = {
#embed "../assets/fluxer/sounds/user-join.mp3"
};
static const unsigned char FLUXER_USER_LEAVE[] = {
#embed "../assets/fluxer/sounds/user-leave.mp3"
};
static const unsigned char FLUXER_USER_MOVED[] = {
#embed "../assets/fluxer/sounds/user-move.mp3"
};
static const unsigned char FLUXER_DISCONNECT[] = {
#embed "../assets/fluxer/sounds/voice-disconnect.mp3"
};
static const unsigned char FLUXER_CALL_RINGING[] = {
#embed "../assets/fluxer/sounds/incoming-ring.mp3"
};
static const unsigned char FLUXER_STREAM_STARTED[] = {
#embed "../assets/fluxer/sounds/stream-start.mp3"
};

#define BYTES(array) {array, sizeof array}

const SoundFile SOUND_FILES[PACKS][SOUNDS] = {
  [PACK_DISCORD] =
    {
      [SOUND_MESSAGE1] = BYTES(DISCORD_MESSAGE1),
      [SOUND_MUTE] = BYTES(DISCORD_MUTE),
      [SOUND_UNMUTE] = BYTES(DISCORD_UNMUTE),
      [SOUND_DEAFEN] = BYTES(DISCORD_DEAFEN),
      [SOUND_UNDEAFEN] = BYTES(DISCORD_UNDEAFEN),
      [SOUND_USER_JOIN] = BYTES(DISCORD_USER_JOIN),
      [SOUND_USER_LEAVE] = BYTES(DISCORD_USER_LEAVE),
      [SOUND_USER_MOVED] = BYTES(DISCORD_USER_MOVED),
      [SOUND_DISCONNECT] = BYTES(DISCORD_DISCONNECT),
      [SOUND_CALL_CALLING] = BYTES(DISCORD_CALL_CALLING),
      [SOUND_CALL_RINGING] = BYTES(DISCORD_CALL_RINGING),
      [SOUND_STREAM_STARTED] = BYTES(DISCORD_STREAM_STARTED),
    },
  // fluxer's lacks the others, discord's play in their place.
  [PACK_FLUXER] =
    {
      [SOUND_MESSAGE1] = BYTES(FLUXER_MESSAGE1),
      [SOUND_MUTE] = BYTES(FLUXER_MUTE),
      [SOUND_UNMUTE] = BYTES(FLUXER_UNMUTE),
      [SOUND_DEAFEN] = BYTES(FLUXER_DEAFEN),
      [SOUND_UNDEAFEN] = BYTES(FLUXER_UNDEAFEN),
      [SOUND_USER_JOIN] = BYTES(FLUXER_USER_JOIN),
      [SOUND_USER_LEAVE] = BYTES(FLUXER_USER_LEAVE),
      [SOUND_USER_MOVED] = BYTES(FLUXER_USER_MOVED),
      [SOUND_DISCONNECT] = BYTES(FLUXER_DISCONNECT),
      [SOUND_CALL_RINGING] = BYTES(FLUXER_CALL_RINGING),
      [SOUND_STREAM_STARTED] = BYTES(FLUXER_STREAM_STARTED),
    },
};
