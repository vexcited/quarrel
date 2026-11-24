#ifndef QUARREL_LINUX_PIPE_H
#define QUARREL_LINUX_PIPE_H

#include <pipewire/pipewire.h>

#include "voice/audio.h"

/* ========================================================================== */
/* the one connection to pipewire that sound, the camera and the device list  */
/* share, made at their first use. its loop runs on a thread of its own:      */
/* pipe_lock around what other threads do with it.                            */
/* ========================================================================== */

// connects when it is not, false when pipewire is not there. main thread.
bool pipe_start(void);
struct pw_thread_loop *pipe_loop(void);
struct pw_context *pipe_context(void);
struct pw_core *pipe_core(void);
void pipe_lock(void);
void pipe_unlock(void);
// runs `fn` on the loop's thread, soon, its lock held.
void pipe_run(void (*fn)(void *ctx), void *ctx);

// what the nodes this process makes for itself are named after, kept out of
// the devices.
#define PIPE_OWN "quarrel."

// whether a node of this name is there. the lock held.
bool pipe_has_node(const char *name);
// whether a node of this name is a device of that kind. the lock held.
bool pipe_is_device(AudioKind kind, const char *name);
// runs on the loop's thread when a node comes or goes, one listener.
void pipe_on_nodes(void (*fn)(void));

// a stream or the camera keeps the connection, which is made again when
// pipewire went away and comes back while nothing uses it.
void pipe_hold(void);
void pipe_release(void);

#endif
