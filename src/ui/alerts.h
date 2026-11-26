#ifndef QUARREL_UI_ALERTS_H
#define QUARREL_UI_ALERTS_H

#include "app.h"

/* ========================================================================== */
/* what the user is told of while they look elsewhere: a notification of the  */
/* system for a message worth one or for a call, with discord's sounds.       */
/* every account logged in tells, shown or not. main thread.                  */
/* ========================================================================== */

// whether the system's notifications are shown, kept from a launch to the
// next and the same for every account, as discord keeps it on the machine.
// the sound is its own switch, see sounds_enabled.
bool alerts_enabled(void);
void alerts_set_enabled(bool on);

// once, before the roster starts.
void alerts_start(App *app);
// a SessionOptions' `on_message`.
void alerts_message(Session *s, const Message *m);
// takes back the notifications of what was read since, here or on another
// device, and rings for the calls of private channels: the ringtone while
// an account is rung, with a notification to answer or decline from when
// the window is not looked at, and the tone of one waiting while an
// account rings others. after any change of a session.
void alerts_sweep(void);
// shows a channel of an account at the next frame, switching to the
// account, and joins its call when `answer` is set.
void alerts_open(Snowflake account_id, Snowflake channel_id, bool answer);
// at the start of every frame: whether the window is looked at, and the
// channel of a notification that was pressed.
void alerts_frame(App *app);
// takes back every notification, as the app quits.
void alerts_free(void);

#endif
