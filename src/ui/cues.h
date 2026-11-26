#ifndef QUARREL_UI_CUES_H
#define QUARREL_UI_CUES_H

#include "app.h"
#include "sounds.h"

/* ========================================================================== */
/* the sounds of a call: the microphone and the headphones going on and off,  */
/* the call starting and dropping, who comes and goes. every account logged   */
/* in is followed, shown or not. main thread.                                 */
/* ========================================================================== */

// the sounds played here, for the settings to list.
extern const Sound CUE_SOUNDS[];
extern const size_t CUE_SOUNDS_COUNT;

// once, before the roster starts.
void cues_start(App *app);
// plays what changed since it was last called. after any change of a
// session.
void cues_follow(void);

#endif
