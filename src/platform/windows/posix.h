#ifndef QUARREL_WINDOWS_POSIX_H
#define QUARREL_WINDOWS_POSIX_H

// the crt's localtime_r and gmtime_r.
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* the little of posix the shared files use that windows has under other     */
/* names, or not at all. force included before anything else, so the headers  */
/* that declare these are read before the names are taken. it includes none   */
/* that declares open, send or connect, which files name functions after.     */
/* ========================================================================== */

// the crt opens files as text unless told, and a descriptor is inherited.
#define O_CLOEXEC (O_BINARY | O_NOINHERIT)

// windows renames over a file only when asked.
int quarrel_rename(const char *from, const char *to);
#define rename quarrel_rename

#define timegm _mkgmtime

char *quarrel_strcasestr(const char *hay, const char *needle);
#define strcasestr quarrel_strcasestr

#endif
