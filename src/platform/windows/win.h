#ifndef QUARREL_WINDOWS_WIN_H
#define QUARREL_WINDOWS_WIN_H

#include <windows.h>

#include "platform/platform.h"
#include "voice/audio.h"

/* ========================================================================== */
/* what the files of src/platform/windows share.                             */
/* ========================================================================== */

// windows' utf-16 of utf-8 and back, from malloc. null for null, and for
// what is not text.
[[gnu::malloc]] wchar_t *win_wide(const char *s);
[[gnu::malloc]] char *win_utf8(const wchar_t *s);

// com on the main thread, in its single threaded apartment, the one
// civue's window takes too. false when it would not.
bool win_com(void);

// the folder of this known id, %APPDATA% or %LOCALAPPDATA%, with Quarrel's
// own inside made when it was not. false when there is none.
bool win_folder(const GUID *known, char *out, size_t cap);

// the privacy settings refuse this capability, "microphone" or "webcam", to
// the desktop apps: the device then answers E_ACCESSDENIED once opened.
bool win_denied(const wchar_t *capability);

// the cameras, as audio_devices lists them.
size_t win_cameras(AudioDevice **out);
// cameras that come and go bump the devices' version too.
void win_cameras_watch(void);
// a device came, went or changed, from any thread.
void win_devices_moved(void);

#endif
