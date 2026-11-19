#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Network/Network.h>

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

static bool online = true;
static void (*network_changed)(void *ctx, bool online, bool returned);
static void *network_ctx;

static void watch_network(void) {
  static bool watching;

  if (watching) {
    return;
  }

  watching = true;

  nw_path_monitor_t monitor = nw_path_monitor_create();
  // the first update only says how things are.
  __block bool first = true;

  nw_path_monitor_set_queue(monitor, dispatch_get_main_queue());
  nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
    bool was = online;

    online = nw_path_get_status(path) == nw_path_status_satisfied;

    if (!first && network_changed != nullptr) {
      network_changed(network_ctx, online, online && !was);
    }

    first = false;
  });
  nw_path_monitor_start(monitor);

  [NSWorkspace.sharedWorkspace.notificationCenter
    addObserverForName:NSWorkspaceDidWakeNotification
                object:nil
                 queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *) {
              if (online && network_changed != nullptr) {
                network_changed(network_ctx, true, false);
              }
            }];
}

bool platform_online(void) {
  watch_network();
  return online;
}

void platform_network_watch(void (*changed)(void *ctx, bool online,
                                            bool returned),
                            void *ctx) {
  network_changed = changed;
  network_ctx = ctx;
  watch_network();
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
