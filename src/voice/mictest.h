#ifndef QUARREL_VOICE_MICTEST_H
#define QUARREL_VOICE_MICTEST_H

/* ========================================================================== */
/* the microphone played back to its owner, to hear how it sounds with the    */
/* devices and the volumes picked. main thread.                               */
/* ========================================================================== */

// asks for the microphone when it was never asked, then plays it back.
// `changed` runs on the main thread once it started or could not.
void mictest_start(void (*changed)(void));
void mictest_stop(void);
bool mictest_running(void);
// running, and the microphone is not heard yet.
bool mictest_starting(void);
// the microphone may not be used, as the last start found.
bool mictest_denied(void);
// how loud the microphone is, 0 to 1, 0 when not running.
float mictest_level(void);
// takes up the devices picked since it started. to call every frame it
// shows in.
void mictest_follow(void);

#endif
