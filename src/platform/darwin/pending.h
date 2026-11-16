#ifndef QUARREL_DARWIN_PENDING_H
#define QUARREL_DARWIN_PENDING_H

#import <Foundation/Foundation.h>

#include "platform/platform.h"

// the operations whose callbacks may still run, main thread only. `task` is
// cancelled with the operation when given.
PlatformOp quarrel_op_begin(NSURLSessionTask *task);
// true when `op` was not cancelled. it is forgotten either way.
bool quarrel_op_end(PlatformOp op);

// runs `block` now on the main thread, or soon from any other.
void quarrel_on_main(dispatch_block_t block);

#endif
