#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "platform/platform.h"

static NSString *const service = @"com.vexcited.quarrel";

static NSMutableDictionary *secret_query(const char *key) {
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : service,
    (__bridge id)kSecAttrAccount : @(key),
  } mutableCopy];
}

bool platform_secret_set(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) {
    return false;
  }

  NSData *data = [NSData dataWithBytes:value length:strlen(value)];
  NSMutableDictionary *query = secret_query(key);
  NSDictionary *update = @{(__bridge id)kSecValueData : data};

  OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                  (__bridge CFDictionaryRef)update);

  if (status == errSecItemNotFound) {
    query[(__bridge id)kSecValueData] = data;
    status = SecItemAdd((__bridge CFDictionaryRef)query, nullptr);
  }

  return status == errSecSuccess;
}

char *platform_secret_get(const char *key, bool *missing) {
  if (missing != nullptr) {
    *missing = false;
  }

  if (key == nullptr) {
    return nullptr;
  }

  NSMutableDictionary *query = secret_query(key);
  query[(__bridge id)kSecReturnData] = @YES;
  query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

  CFTypeRef found = nullptr;
  OSStatus status =
    SecItemCopyMatching((__bridge CFDictionaryRef)query, &found);

  if (status != errSecSuccess || found == nullptr) {
    if (missing != nullptr) {
      *missing = status == errSecItemNotFound;
    }

    return nullptr;
  }

  NSData *data = (__bridge_transfer NSData *)found;
  char *value = malloc(data.length + 1);

  if (value != nullptr) {
    memcpy(value, data.bytes, data.length);
    value[data.length] = '\0';
  }

  return value;
}

bool platform_secret_delete(const char *key) {
  if (key == nullptr) {
    return false;
  }

  OSStatus status = SecItemDelete((__bridge CFDictionaryRef)secret_query(key));
  return status == errSecSuccess || status == errSecItemNotFound;
}

static NSUserDefaults *prefs(void) {
  static NSUserDefaults *defaults;

  // a suite named after the app's own bundle is refused, and the standard
  // defaults read the same file then.
  if (defaults == nil && [NSBundle.mainBundle.bundleIdentifier
                           isEqualToString:service]) {
    defaults = NSUserDefaults.standardUserDefaults;
  }
  else if (defaults == nil) {
    defaults = [[NSUserDefaults alloc] initWithSuiteName:service];
  }

  return defaults;
}

void platform_pref_set(const char *key, const char *value) {
  if (key == nullptr) {
    return;
  }

  if (value == nullptr) {
    [prefs() removeObjectForKey:@(key)];
  }
  else {
    [prefs() setObject:@(value) forKey:@(key)];
  }
}

char *platform_pref_get(const char *key) {
  if (key == nullptr) {
    return nullptr;
  }

  NSString *value = [prefs() stringForKey:@(key)];
  return value != nil ? strdup(value.UTF8String) : nullptr;
}
