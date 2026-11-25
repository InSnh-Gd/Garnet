#ifndef RDNA4_PITCH_H
#define RDNA4_PITCH_H

#include <stddef.h>
#include <stdint.h>

enum {
    RDNA4P_EXPLICIT_TRY = 0,
    RDNA4P_EXPLICIT_MATCH = 1,
    RDNA4P_EXPLICIT_NO_EXTENSION = 2,
    RDNA4P_EXPLICIT_FLAGS = 3,
    RDNA4P_EXPLICIT_SHAPE = 4,
    RDNA4P_EXPLICIT_PITCH = 5,
    RDNA4P_EXPLICIT_UNSUPPORTED = 6,
    RDNA4P_EXPLICIT_CREATE_FAILED = 7,
    RDNA4P_EXPLICIT_LAYOUT = 8,
    RDNA4P_EXPLICIT_BIND = 9,
    RDNA4P_EXPLICIT_OFFSET = 10
};

static inline int rdna4p_explicit_pitch_reason(size_t bpr, size_t width, size_t bpp,
        size_t natural_row_pitch, uint32_t flags, uint32_t mip_levels,
        uint32_t array_layers, uint32_t samples, int has_modifier_extension) {
    if (natural_row_pitch == bpr) return RDNA4P_EXPLICIT_MATCH;
    if (!has_modifier_extension) return RDNA4P_EXPLICIT_NO_EXTENSION;
    if (flags != 0) return RDNA4P_EXPLICIT_FLAGS;
    if (mip_levels != 1 || array_layers != 1 || samples != 1 || !width || !bpp || width > SIZE_MAX / bpp)
        return RDNA4P_EXPLICIT_SHAPE;
    if (bpr < width * bpp || bpr % 128u != 0) return RDNA4P_EXPLICIT_PITCH;
    return RDNA4P_EXPLICIT_TRY;
}

static inline const char *rdna4p_explicit_pitch_reason_text(int reason) {
    switch (reason) {
    case RDNA4P_EXPLICIT_MATCH: return "natural pitch already matches";
    case RDNA4P_EXPLICIT_NO_EXTENSION: return "DRM format modifier extension unavailable";
    case RDNA4P_EXPLICIT_FLAGS: return "image flags are not supported by the explicit-pitch retry";
    case RDNA4P_EXPLICIT_SHAPE: return "requires one mip, layer and sample";
    case RDNA4P_EXPLICIT_PITCH: return "pitch is too small or not 128-byte aligned";
    case RDNA4P_EXPLICIT_UNSUPPORTED: return "format/usage does not support the linear modifier";
    case RDNA4P_EXPLICIT_CREATE_FAILED: return "explicit-modifier image creation failed";
    case RDNA4P_EXPLICIT_LAYOUT: return "explicit image layout does not match requested pitch";
    case RDNA4P_EXPLICIT_BIND: return "explicit image requirements do not fit or bind to imported memory";
    case RDNA4P_EXPLICIT_OFFSET: return "natural image subresource offset is nonzero";
    case RDNA4P_EXPLICIT_TRY: return "eligible";
    default: return "invalid explicit-pitch decision";
    }
}

#endif
