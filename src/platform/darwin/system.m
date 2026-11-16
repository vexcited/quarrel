#import <Foundation/Foundation.h>

#include "platform/platform.h"

static void copy_out(char *out, size_t cap, NSString *s) {
  if (cap == 0) {
    return;
  }

  snprintf(out, cap, "%s", s != nil ? s.UTF8String : "");
}

void platform_os_version(char *out, size_t cap) {
  NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
  snprintf(out, cap, "%ld.%ld.%ld", (long)v.majorVersion, (long)v.minorVersion,
           (long)v.patchVersion);
}

void platform_locale(char *out, size_t cap) {
  NSString *tag = NSLocale.preferredLanguages.firstObject;
  copy_out(out, cap, tag.length > 0 ? tag : @"en-US");
}

void platform_timezone(char *out, size_t cap) {
  copy_out(out, cap, NSTimeZone.localTimeZone.name);
}

void platform_uuid(char out[static 37]) {
  copy_out(out, 37, NSUUID.UUID.UUIDString.lowercaseString);
}

void platform_random(void *out, size_t len) {
  arc4random_buf(out, len);
}

double platform_now(void) {
  return CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970;
}

// app nap delays the timers and the network callbacks of an app that is
// hidden or behind others by many seconds, which makes gateway events and
// heartbeats late.
void platform_stay_responsive(void) {
  static id<NSObject> activity;

  if (activity == nil) {
    activity = [NSProcessInfo.processInfo
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                        reason:@"Keeping the gateway connection live."];
  }
}

void platform_post(void (*fn)(void *ctx), void *ctx) {
  dispatch_async_f(dispatch_get_main_queue(), ctx, fn);
}

void platform_run(void) {
  CFRunLoopRun();
}

void platform_stop(void) {
  CFRunLoopStop(CFRunLoopGetMain());
}
