#import <Foundation/Foundation.h>

#include "platform/platform.h"

@interface QuarrelTimer : NSObject
@property(nonatomic) dispatch_source_t source;
@end

@implementation QuarrelTimer
@end

PlatformTimer *platform_timer(double seconds, void (*fn)(void *ctx),
                              void *ctx) {
  QuarrelTimer *timer = [QuarrelTimer new];
  dispatch_source_t source = dispatch_source_create(
    DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

  if (seconds < 0) {
    seconds = 0;
  }

  int64_t delay = (int64_t)(seconds * (double)NSEC_PER_SEC);
  uint64_t leeway = (uint64_t)(delay / 20);
  dispatch_source_set_timer(source, dispatch_time(DISPATCH_TIME_NOW, delay),
                            DISPATCH_TIME_FOREVER, leeway);

  // the handle holds one reference, given back when the timer fires or is
  // cancelled.
  void *handle = (__bridge_retained void *)timer;

  dispatch_source_set_event_handler(source, ^{
    QuarrelTimer *fired = (__bridge_transfer QuarrelTimer *)handle;
    dispatch_source_cancel(fired.source);
    fired.source = nil;
    fn(ctx);
  });

  timer.source = source;
  dispatch_resume(source);
  return handle;
}

void platform_timer_cancel(PlatformTimer *handle) {
  if (handle == nullptr) {
    return;
  }

  QuarrelTimer *timer = (__bridge_transfer QuarrelTimer *)(void *)handle;
  dispatch_source_cancel(timer.source);
  timer.source = nil;
}
