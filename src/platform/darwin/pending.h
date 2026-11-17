#ifndef QUARREL_DARWIN_PENDING_H
#define QUARREL_DARWIN_PENDING_H

#import <Foundation/Foundation.h>

#include "platform/platform.h"

// runs `block` now on the main thread, or soon from any other.
void quarrel_on_main(dispatch_block_t block);

#endif
