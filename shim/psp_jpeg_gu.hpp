#pragma once

#include <stddef.h>

// Full-resolution libjpeg RGB decoding followed by the existing PSP surface
// cache's integer bilinear filter and little-endian GU RGB565 packing.
// These helpers do not own, publish, flush or free the caller's GE storage.
extern "C" int th06_psp_img_jpeg_dimensions(const unsigned char *data, size_t size,
                                            unsigned int *width, unsigned int *height);

// storage dimensions are powers of two, at most 512. target dimensions are
// the content footprint inside that storage (e.g. 480x272 or 480x442), chosen
// by the existing GE layout. Padding is zeroed; failed output is unpublished
// and must be discarded by the caller. Returns 1 on success, 0 on failure.
// At most two full-width RGB scanlines are owned in addition to libjpeg's
// internal state; no full RGB surface or lower-resolution JPEG decode exists.
extern "C" int th06_psp_img_load_jpeg_gu565(const unsigned char *data, size_t size,
                                           void *destination, size_t destinationBytes,
                                           unsigned int storageWidth, unsigned int storageHeight,
                                           unsigned int targetWidth, unsigned int targetHeight);
