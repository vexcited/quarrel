#ifndef QUARREL_VOICE_CAMERA_H
#define QUARREL_VOICE_CAMERA_H

#include <stdint.h>

#include <civue/gfx.h>

/* ========================================================================== */
/* what a camera sees, as a texture to draw, mirrored as its owner expects    */
/* to see themselves. main thread.                                            */
/* ========================================================================== */

// asks for the camera when it was never asked, then films with the one of
// this id, the system's own for a null one or one that is gone. `changed`
// runs on the main thread once it started or could not, and at every picture.
void camera_start(civue_gfx *gfx, const char *id, void (*changed)(void));
void camera_stop(void);
bool camera_running(void);
// the camera may not be used, as the last start found.
bool camera_denied(void);
// films with the camera of this id when it is not the one filming.
void camera_follow(const char *id);
// the last picture and its size in pixels, 0 before the first one.
civue_texture camera_picture(uint32_t *width, uint32_t *height);

#endif
