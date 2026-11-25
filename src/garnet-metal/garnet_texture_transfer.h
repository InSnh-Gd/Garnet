#ifndef RDNA4_TEXTURE_TRANSFER_H
#define RDNA4_TEXTURE_TRANSFER_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t row_bytes, packed_bytes, span;
} rdna4_texture_transfer;

// span covers bytes actually touched, without requiring final-row padding.
static inline int rdna4_texture_transfer_layout(size_t width, size_t height,
    size_t depth, size_t bpp, size_t row_stride, size_t image_stride,
    rdna4_texture_transfer *out) {
    size_t row, packed, span, extra;
    if (!out || !width || !height || !depth || !bpp ||
        __builtin_mul_overflow(width, bpp, &row) || row_stride < row ||
        __builtin_mul_overflow(row, height, &packed) ||
        __builtin_mul_overflow(packed, depth, &packed) ||
        __builtin_mul_overflow(height - 1, row_stride, &span) ||
        __builtin_add_overflow(span, row, &span)) return 0;
    if (depth > 1) {
        size_t minimum_image;
        if (__builtin_mul_overflow(row_stride, height, &minimum_image) ||
            image_stride < minimum_image ||
            __builtin_mul_overflow(depth - 1, image_stride, &extra) ||
            __builtin_add_overflow(span, extra, &span)) return 0;
    }
    *out = (rdna4_texture_transfer){row, packed, span};
    return 1;
}

static inline int rdna4_texture_transfer_pointer(const void *bytes, size_t span) {
    return bytes && span && span <= UINTPTR_MAX - (uintptr_t)bytes;
}
#endif
