#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>

#include "platform/platform.h"

// a finder pattern, the square in three corners of every code, at `row`
// and `col`: dark ring, light ring, dark center.
static bool finder_at(const uint8_t *m, size_t side, size_t row, size_t col) {
  for (size_t y = 0; y < 7; y++) {
    for (size_t x = 0; x < 7; x++) {
      size_t ring = MIN(MIN(x, y), MIN(6 - x, 6 - y));

      if (m[(row + y) * side + col + x] != (ring != 1)) {
        return false;
      }
    }
  }

  return true;
}

uint8_t *platform_qr(const char *text, size_t *side) {
  static CIContext *context;

  @autoreleasepool {
    CIFilter *filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    NSData *data = [NSData dataWithBytes:text length:strlen(text)];

    [filter setValue:data forKey:@"inputMessage"];
    [filter setValue:@"M" forKey:@"inputCorrectionLevel"];

    CIImage *image = filter.outputImage;

    if (image == nil) {
      return nullptr;
    }

    if (context == nil) {
      context = [CIContext
        contextWithOptions:@{kCIContextUseSoftwareRenderer : @YES}];
    }

    CGRect extent = image.extent;
    size_t width = (size_t)extent.size.width;
    size_t height = (size_t)extent.size.height;
    uint8_t *pixels = malloc(width * height);

    if (pixels == nullptr || width == 0 || width != height) {
      free(pixels);
      return nullptr;
    }

    [context render:image
           toBitmap:pixels
           rowBytes:(ptrdiff_t)width
             bounds:extent
             format:kCIFormatR8
         colorSpace:nil];

    // the image has a quiet zone of its own, the dark modules say where the
    // code is since a finder pattern sits in three of its corners.
    size_t first = width, last = 0;

    for (size_t y = 0; y < height; y++) {
      for (size_t x = 0; x < width; x++) {
        if (pixels[y * width + x] < 128) {
          first = MIN(first, MIN(x, y));
          last = MAX(last, MAX(x, y));
        }
      }
    }

    size_t n = last >= first ? last - first + 1 : 0;
    uint8_t *modules = n >= 21 ? malloc(n * n) : nullptr;

    if (modules == nullptr) {
      free(pixels);
      return nullptr;
    }

    for (size_t y = 0; y < n; y++) {
      for (size_t x = 0; x < n; x++) {
        modules[y * n + x] = pixels[(first + y) * width + first + x] < 128;
      }
    }

    free(pixels);

    // core image's rows may come from the bottom: then the corner without
    // a finder is the top right one rather than the bottom right one.
    if (!finder_at(modules, n, 0, n - 7) &&
        finder_at(modules, n, n - 7, n - 7)) {
      for (size_t y = 0; y < n / 2; y++) {
        for (size_t x = 0; x < n; x++) {
          uint8_t swap = modules[y * n + x];
          modules[y * n + x] = modules[(n - 1 - y) * n + x];
          modules[(n - 1 - y) * n + x] = swap;
        }
      }
    }

    *side = n;
    return modules;
  }
}
