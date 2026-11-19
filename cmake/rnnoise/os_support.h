#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H

// rnnoise 0.2 took its neon code from opus without this header of opus,
// which it includes. this is all that code uses of it.

#include <string.h>

#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n) * sizeof(*(dst))))

#endif
