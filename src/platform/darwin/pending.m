#import "pending.h"

static NSMutableDictionary<NSNumber *, id> *pending(void) {
  static NSMutableDictionary *ops;

  if (ops == nil) {
    ops = [NSMutableDictionary dictionary];
  }

  return ops;
}

PlatformOp quarrel_op_begin(NSURLSessionTask *task) {
  static PlatformOp last;

  PlatformOp op = ++last;
  pending()[@(op)] = task != nil ? task : (id)NSNull.null;
  return op;
}

bool quarrel_op_end(PlatformOp op) {
  NSNumber *key = @(op);

  if (pending()[key] == nil) {
    return false;
  }

  [pending() removeObjectForKey:key];
  return true;
}

void platform_cancel(PlatformOp op) {
  NSNumber *key = @(op);
  id task = pending()[key];

  if (task == nil) {
    return;
  }

  [pending() removeObjectForKey:key];

  if ([task isKindOfClass:NSURLSessionTask.class]) {
    [(NSURLSessionTask *)task cancel];
  }
}

void quarrel_on_main(dispatch_block_t block) {
  if (NSThread.isMainThread) {
    block();
  }
  else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}
