#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#import "pending.h"

/* ========================================================================== */
/* notifications through the user notification center, which asks the user   */
/* once whether the app may. a notification is known by the app's own id,     */
/* written in digits.                                                         */
/* ========================================================================== */

#define CALL_CATEGORY @"call"
#define ANSWER_ACTION @"answer"
#define DECLINE_ACTION @"decline"

static void (*activated)(void *ctx, uint64_t id, PlatformNotifyAction action);
static void *activated_ctx;

@interface QuarrelNotifications : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation QuarrelNotifications

// the center shows nothing of an app in front unless told to. what is worth
// telling was decided before it was asked.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:
           (void (^)(UNNotificationPresentationOptions))done {
  done(UNNotificationPresentationOptionBanner |
       UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
  didReceiveNotificationResponse:(UNNotificationResponse *)response
           withCompletionHandler:(void (^)(void))done {
  NSString *name = response.notification.request.identifier;
  uint64_t id = strtoull(name.UTF8String, nullptr, 10);

  NSString *pressed = response.actionIdentifier;
  bool known = true;
  PlatformNotifyAction action = PLATFORM_NOTIFY_OPENED;

  if ([pressed isEqualToString:ANSWER_ACTION]) {
    action = PLATFORM_NOTIFY_ANSWERED;
  }
  else if ([pressed isEqualToString:DECLINE_ACTION]) {
    action = PLATFORM_NOTIFY_DECLINED;
  }
  else {
    // one swiped away says so too, which is nothing pressed.
    known = [pressed isEqualToString:UNNotificationDefaultActionIdentifier];
  }

  // the system brings the app forward by itself, but for declining.
  if (known) {
    quarrel_on_main(^{
      if (activated != nullptr && id != 0) {
        activated(activated_ctx, id, action);
      }
    });
  }

  done();
}

@end

// the center once a notification was shown, nil before.
static UNUserNotificationCenter *shared;

// nil for a binary run out of its bundle, which the center refuses.
static UNUserNotificationCenter *center(void) {
  static QuarrelNotifications *delegate;

  if (shared == nil && NSBundle.mainBundle.bundleIdentifier != nil) {
    shared = UNUserNotificationCenter.currentNotificationCenter;
    delegate = [QuarrelNotifications new];
    shared.delegate = delegate;

    // the buttons of a call. answering needs the app in front, declining
    // is done behind.
    UNNotificationAction *answer = [UNNotificationAction
      actionWithIdentifier:ANSWER_ACTION
                     title:@"Answer"
                   options:UNNotificationActionOptionForeground];
    UNNotificationAction *decline = [UNNotificationAction
      actionWithIdentifier:DECLINE_ACTION
                     title:@"Decline"
                   options:UNNotificationActionOptionDestructive];
    UNNotificationCategory *call = [UNNotificationCategory
      categoryWithIdentifier:CALL_CATEGORY
                     actions:@[ answer, decline ]
           intentIdentifiers:@[]
                     options:UNNotificationCategoryOptionNone];

    [shared setNotificationCategories:[NSSet setWithObject:call]];
    [shared requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL, NSError *){}];
  }

  return shared;
}

static NSString *text_of(const char *s) {
  NSString *text = s != nullptr ? [NSString stringWithUTF8String:s] : nil;
  return text != nil ? text : @"";
}

static NSString *name_of(uint64_t id) {
  return [NSString stringWithFormat:@"%llu", (unsigned long long)id];
}

void platform_notify(const PlatformNotification *n) {
  UNUserNotificationCenter *c = center();

  if (n == nullptr || n->id == 0 || c == nil) {
    return;
  }

  UNMutableNotificationContent *content = [UNMutableNotificationContent new];

  content.title = text_of(n->title);
  content.body = text_of(n->body);

  if (n->call) {
    content.categoryIdentifier = CALL_CATEGORY;
  }

  // the center moves an attachment into its own store: it is given a copy.
  if (n->image != nullptr) {
    NSURL *from = [NSURL fileURLWithPath:text_of(n->image)];
    NSString *name =
      [NSString stringWithFormat:@"quarrel-%@.png", name_of(n->id)];
    NSURL *copy = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
      URLByAppendingPathComponent:name];

    [NSFileManager.defaultManager removeItemAtURL:copy error:nil];

    if ([NSFileManager.defaultManager copyItemAtURL:from
                                              toURL:copy
                                              error:nil]) {
      UNNotificationAttachment *image =
        [UNNotificationAttachment attachmentWithIdentifier:@"image"
                                                       URL:copy
                                                   options:nil
                                                     error:nil];

      if (image != nil) {
        content.attachments = @[ image ];
      }
    }
  }

  // no sound of the system's: the app plays its own.
  [c addNotificationRequest:[UNNotificationRequest
                              requestWithIdentifier:name_of(n->id)
                                            content:content
                                            trigger:nil]
      withCompletionHandler:nil];
}

void platform_notify_close(uint64_t id) {
  if (shared == nil || id == 0) {
    return;
  }

  [shared removePendingNotificationRequestsWithIdentifiers:@[ name_of(id) ]];
  [shared removeDeliveredNotificationsWithIdentifiers:@[ name_of(id) ]];
}

void platform_notify_watch(void (*fn)(void *ctx, uint64_t id,
                                      PlatformNotifyAction action),
                           void *ctx) {
  activated = fn;
  activated_ctx = ctx;
}

// nothing is asked of a center the app never used, which would have the
// system ask the user as the app quits.
void platform_notify_clear(void) {
  [shared removeAllPendingNotificationRequests];
  [shared removeAllDeliveredNotifications];
}
