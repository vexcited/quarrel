#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#import "pending.h"

// an image no window could show whole, refused rather than decoded.
static const size_t max_decoded_pixels = 64u << 20;

static CGImageRef create_image(NSData *data, uint32_t max_pixels) {
  CGImageSourceRef source =
    CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);

  if (source == nullptr) {
    return nullptr;
  }

  CGImageRef image = nullptr;

  if (CGImageSourceGetCount(source) > 0) {
    if (max_pixels > 0) {
      NSDictionary *options = @{
        (__bridge id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (__bridge id)kCGImageSourceCreateThumbnailWithTransform : @YES,
        (__bridge id)kCGImageSourceThumbnailMaxPixelSize : @(max_pixels),
      };
      image = CGImageSourceCreateThumbnailAtIndex(
        source, 0, (__bridge CFDictionaryRef)options);
    }
    else {
      image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    }
  }

  CFRelease(source);
  return image;
}

static uint8_t *draw_rgba(CGImageRef image, uint32_t *width, uint32_t *height) {
  size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);

  if (w == 0 || h == 0 || w * h > max_decoded_pixels) {
    return nullptr;
  }

  uint8_t *rgba = calloc(w * h, 4);

  if (rgba == nullptr) {
    return nullptr;
  }

  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
    rgba, w, h, 8, w * 4, space,
    (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);

  if (ctx == nullptr) {
    free(rgba);
    return nullptr;
  }

  CGContextSetBlendMode(ctx, kCGBlendModeCopy);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);

  *width = (uint32_t)w;
  *height = (uint32_t)h;
  return rgba;
}

PlatformOp platform_image_decode(const void *bytes, size_t len,
                                 uint32_t max_pixels, PlatformImageDone done,
                                 void *ctx) {
  NSData *data = [NSData dataWithBytes:bytes length:len];
  PlatformOp op = quarrel_op_begin(nil);

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    uint32_t width = 0, height = 0;
    uint8_t *rgba = nullptr;
    CGImageRef image = create_image(data, max_pixels);

    if (image != nullptr) {
      rgba = draw_rgba(image, &width, &height);
      CGImageRelease(image);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      if (quarrel_op_end(op)) {
        PlatformImage decoded = {width, height, rgba};
        done(ctx, rgba != nullptr ? &decoded : nullptr);
      }

      free(rgba);
    });
  });

  return op;
}
