#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>

#include <stdatomic.h>

#include "voice/camera.h"

@interface QuarrelCameraSink
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) uint32_t run;
@end

static AVCaptureSession *session;
static QuarrelCameraSink *sink;
static dispatch_queue_t queue;
static civue_gfx *gfx;
// one texture for every run: what was drawn this frame may still name it.
static id<MTLTexture> picture;
static civue_texture texture;
static char *filming;
static bool wanted, denied, fresh;
// counts the starts and the stops, to tell what comes from an older one.
static uint32_t run;
static void (*on_changed)(void);
// a picture waits for the main thread, the ones after it are dropped.
static atomic_bool waiting;

static void show(CVPixelBufferRef pixels) {
  size_t width = CVPixelBufferGetWidth(pixels);
  size_t height = CVPixelBufferGetHeight(pixels);

  if (picture == nil || picture.width != width || picture.height != height) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];

    d.usage = MTLTextureUsageShaderRead;
    picture = [(__bridge id<MTLDevice>)civue_gfx_device(gfx)
      newTextureWithDescriptor:d];

    if (picture == nil) {
      return;
    }

    if (texture == 0) {
      texture = civue_gfx_register_texture(gfx, (__bridge void *)picture);
    }
    else {
      civue_gfx_update_texture(gfx, texture, (__bridge void *)picture);
    }
  }

  CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  [picture replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:CVPixelBufferGetBaseAddress(pixels)
             bytesPerRow:CVPixelBufferGetBytesPerRow(pixels)];
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  fresh = true;
}

@implementation QuarrelCameraSink

- (void)captureOutput:(AVCaptureOutput *)output
  didOutputSampleBuffer:(CMSampleBufferRef)sample
         fromConnection:(AVCaptureConnection *)connection {
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
  uint32_t mine = self.run;

  if (pixels == nullptr || atomic_exchange(&waiting, true)) {
    return;
  }

  CVPixelBufferRetain(pixels);
  dispatch_async(dispatch_get_main_queue(), ^{
    atomic_store(&waiting, false);

    if (mine == run && wanted) {
      show(pixels);

      if (on_changed != nullptr) {
        on_changed();
      }
    }

    CVPixelBufferRelease(pixels);
  });
}

@end

static AVCaptureDevice *camera_of(const char *id) {
  AVCaptureDevice *camera = nil;

  if (id != nullptr && id[0] != '\0') {
    camera = [AVCaptureDevice deviceWithUniqueID:@(id)];
  }

  return camera ?: [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
}

static void film(void) {
  AVCaptureDevice *camera = camera_of(filming);
  AVCaptureDeviceInput *input =
    camera != nil ? [AVCaptureDeviceInput deviceInputWithDevice:camera
                                                          error:nil]
                  : nil;
  AVCaptureVideoDataOutput *output = [AVCaptureVideoDataOutput new];
  AVCaptureSession *made = [AVCaptureSession new];

  if (input == nil || ![made canAddInput:input] ||
      ![made canAddOutput:output]) {
    wanted = false;
    return;
  }

  if (queue == nil) {
    queue = dispatch_queue_create("quarrel.camera", DISPATCH_QUEUE_SERIAL);
  }

  sink = [QuarrelCameraSink new];
  sink.run = run;
  output.videoSettings =
    @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
  output.alwaysDiscardsLateVideoFrames = YES;
  [output setSampleBufferDelegate:sink queue:queue];

  [made beginConfiguration];

  if ([made canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
    made.sessionPreset = AVCaptureSessionPreset1280x720;
  }

  [made addInput:input];
  [made addOutput:output];

  AVCaptureConnection *connection =
    [output connectionWithMediaType:AVMediaTypeVideo];

  if (connection.supportsVideoMirroring) {
    connection.automaticallyAdjustsVideoMirroring = NO;
    connection.videoMirrored = YES;
  }

  [made commitConfiguration];
  session = made;

  // starting waits for the camera, not on the main thread.
  dispatch_async(queue, ^{
    [made startRunning];
  });
}

static void shut(void) {
  AVCaptureSession *made = session;

  run++;
  session = nil;
  sink = nil;
  fresh = false;

  if (made != nil) {
    dispatch_async(queue, ^{
      [made stopRunning];
    });
  }
}

void camera_start(civue_gfx *g, const char *id, void (*changed)(void)) {
  if (wanted) {
    return;
  }

  wanted = true;
  denied = false;
  gfx = g;
  on_changed = changed;
  free(filming);
  filming = id != nullptr ? strdup(id) : nullptr;

  uint32_t mine = ++run;
  AVAuthorizationStatus status =
    [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
  void (^answered)(BOOL) = ^(BOOL granted) {
    if (!wanted || mine != run) {
      return;
    }

    if (granted) {
      film();
    }
    else {
      wanted = false;
      denied = true;
    }

    if (on_changed != nullptr) {
      on_changed();
    }
  };

  if (status != AVAuthorizationStatusNotDetermined) {
    answered(status == AVAuthorizationStatusAuthorized);
    return;
  }

  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
                             dispatch_async(dispatch_get_main_queue(), ^{
                               answered(granted);
                             });
                           }];
}

void camera_stop(void) {
  wanted = false;
  shut();
}

bool camera_running(void) {
  return wanted;
}

bool camera_denied(void) {
  return denied;
}

void camera_follow(const char *id) {
  const char *now = filming != nullptr ? filming : "";
  const char *next = id != nullptr ? id : "";

  if (session == nil || strcmp(now, next) == 0) {
    return;
  }

  shut();
  free(filming);
  filming = id != nullptr ? strdup(id) : nullptr;
  film();
}

civue_texture camera_picture(uint32_t *width, uint32_t *height) {
  if (!fresh || picture == nil) {
    return 0;
  }

  *width = (uint32_t)picture.width;
  *height = (uint32_t)picture.height;
  return texture;
}
