#import "pending.h"

// ready is several megabytes and the default limit is one.
static const NSInteger max_message_bytes = 256 << 20;

@interface QuarrelSocket : NSObject <NSURLSessionWebSocketDelegate>
@property(nonatomic) NSURLSession *session;
@property(nonatomic) NSURLSessionWebSocketTask *task;
@property(nonatomic) PlatformWebSocketEvents events;
@property(nonatomic) BOOL opened, finished, dropped, closing;
@end

@implementation QuarrelSocket

- (void)finishWithCode:(NSInteger)code reason:(NSString *)reason {
  if (self.finished || self.dropped) {
    return;
  }

  self.finished = YES;
  self.opened = NO;
  [self.session invalidateAndCancel];

  if (self.events.closed != nullptr) {
    self.events.closed(self.events.ctx, (int)code, reason.UTF8String ?: "");
  }
}

- (void)finishFromTask:(NSString *)fallback {
  NSURLSessionWebSocketCloseCode code = self.task.closeCode;

  if (code == NSURLSessionWebSocketCloseCodeInvalid) {
    [self finishWithCode:1006 reason:fallback];
    return;
  }

  NSData *data = self.task.closeReason;
  NSString *reason = data != nil
                       ? [[NSString alloc] initWithData:data
                                               encoding:NSUTF8StringEncoding]
                       : nil;
  [self finishWithCode:code reason:reason ?: @""];
}

- (void)receive {
  __weak QuarrelSocket *weak = self;

  [self.task receiveMessageWithCompletionHandler:^(
               NSURLSessionWebSocketMessage *message, NSError *error) {
    quarrel_on_main(^{
      QuarrelSocket *me = weak;

      if (me == nil || me.finished || me.dropped) {
        return;
      }

      if (error != nil) {
        // the read fails before the close frame is reported when the peer
        // closes, so the close code gets a moment to arrive.
        NSString *why = error.localizedDescription;

        if (me.task.closeCode != NSURLSessionWebSocketCloseCodeInvalid) {
          [me finishFromTask:why];
          return;
        }

        dispatch_after(
          dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
          dispatch_get_main_queue(), ^{ [weak finishFromTask:why]; });
        return;
      }

      if (message.type == NSURLSessionWebSocketMessageTypeString) {
        NSData *text = [message.string dataUsingEncoding:NSUTF8StringEncoding];

        if (me.events.text != nullptr && text != nil) {
          me.events.text(me.events.ctx, text.bytes, text.length);
        }
      }
      else if (me.events.binary != nullptr) {
        me.events.binary(me.events.ctx, message.data.bytes,
                         message.data.length);
      }

      // the callback may have dropped the socket.
      if (!me.finished && !me.dropped) {
        [me receive];
      }
    });
  }];
}

- (void)URLSession:(NSURLSession *)session
        webSocketTask:(NSURLSessionWebSocketTask *)task
  didOpenWithProtocol:(NSString *)protocol {
  quarrel_on_main(^{
    if (self.finished || self.dropped) {
      return;
    }

    self.opened = YES;

    if (self.events.open != nullptr) {
      self.events.open(self.events.ctx);
    }
  });
}

- (void)URLSession:(NSURLSession *)session
     webSocketTask:(NSURLSessionWebSocketTask *)task
  didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
            reason:(NSData *)data {
  quarrel_on_main(^{
    NSString *reason = data != nil
                         ? [[NSString alloc] initWithData:data
                                                 encoding:NSUTF8StringEncoding]
                         : nil;
    [self finishWithCode:code reason:reason ?: @""];
  });
}

- (void)URLSession:(NSURLSession *)session
                  task:(NSURLSessionTask *)task
  didCompleteWithError:(NSError *)error {
  quarrel_on_main(^{
    [self finishFromTask:error != nil ? error.localizedDescription
                                      : @"connection closed"];
  });
}

@end

PlatformWebSocket *platform_ws_connect(const char *url,
                                       const PlatformHeader *headers,
                                       size_t nheaders,
                                       const PlatformWebSocketEvents *events) {
  QuarrelSocket *socket = [QuarrelSocket new];
  socket.events = *events;

  NSURL *target = [NSURL URLWithString:@(url ?: "")];
  NSMutableURLRequest *request = [NSMutableURLRequest
    requestWithURL:target ?: [NSURL URLWithString:@"wss://invalid.invalid"]];
  request.timeoutInterval = 30;

  for (size_t i = 0; i < nheaders; i++) {
    if (headers[i].name != nullptr && headers[i].value != nullptr) {
      [request setValue:@(headers[i].value)
        forHTTPHeaderField:@(headers[i].name)];
    }
  }

  NSURLSessionConfiguration *config =
    NSURLSessionConfiguration.ephemeralSessionConfiguration;
  socket.session =
    [NSURLSession sessionWithConfiguration:config
                                  delegate:socket
                             delegateQueue:NSOperationQueue.mainQueue];
  socket.task = [socket.session webSocketTaskWithRequest:request];
  socket.task.maximumMessageSize = max_message_bytes;

  [socket.task resume];
  [socket receive];
  return (__bridge_retained void *)socket;
}

bool platform_ws_send_binary(PlatformWebSocket *ws, const uint8_t *data,
                             size_t len) {
  QuarrelSocket *socket = (__bridge QuarrelSocket *)(void *)ws;

  if (socket == nil || !socket.opened) {
    return false;
  }

  NSURLSessionWebSocketMessage *message = [[NSURLSessionWebSocketMessage alloc]
    initWithData:[NSData dataWithBytes:data length:len]];
  [socket.task sendMessage:message
         completionHandler:^(NSError *error) { (void)error; }];
  return true;
}

void platform_ws_close(PlatformWebSocket *ws, int code, const char *reason) {
  QuarrelSocket *socket = (__bridge QuarrelSocket *)(void *)ws;

  if (socket == nil || socket.finished) {
    return;
  }

  NSData *data = reason != nullptr
                   ? [NSData dataWithBytes:reason length:strlen(reason)]
                   : nil;
  socket.closing = YES;
  [socket.task cancelWithCloseCode:code reason:data];
}

void platform_ws_free(PlatformWebSocket *ws) {
  if (ws == nullptr) {
    return;
  }

  QuarrelSocket *socket = (__bridge_transfer QuarrelSocket *)(void *)ws;
  socket.dropped = YES;
  socket.opened = NO;

  // a close that was asked for still gets its frame out.
  if (socket.closing) {
    [socket.session finishTasksAndInvalidate];
    return;
  }

  [socket.task cancel];
  [socket.session invalidateAndCancel];
}
