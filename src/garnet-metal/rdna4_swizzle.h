#ifndef RDNA4_SWIZZLE_H
#define RDNA4_SWIZZLE_H

// Pure-C texture swizzle helpers (shared by GarnetDevice.m and host C unit tests).
#ifndef RDNA4_SWIZZLE_PURE_H
#define RDNA4_SWIZZLE_PURE_H
#include <stddef.h>
#include <stdint.h>

enum {
    RDNA4SWZ_ZERO  = 0,
    RDNA4SWZ_ONE   = 1,
    RDNA4SWZ_RED   = 2,
    RDNA4SWZ_GREEN = 3,
    RDNA4SWZ_BLUE  = 4,
    RDNA4SWZ_ALPHA = 5,
};
enum {
    RDNA4VKSWZ_IDENTITY = 0,
    RDNA4VKSWZ_ZERO     = 1,
    RDNA4VKSWZ_ONE      = 2,
    RDNA4VKSWZ_R        = 3,
    RDNA4VKSWZ_G        = 4,
    RDNA4VKSWZ_B        = 5,
    RDNA4VKSWZ_A        = 6,
};
enum {
    RDNA4SWZ_USE_READ  = 1u,
    RDNA4SWZ_USE_WRITE = 2u,
    RDNA4SWZ_USE_RT    = 4u,
    RDNA4SWZ_USE_PFV   = 16u,
};
#define RDNA4SWZ_FMT_A8    1u
#define RDNA4SWZ_FMT_TEXEL 4u

typedef struct { uint8_t r, g, b, a; } rdna4swz_t;
typedef struct { uint32_t r, g, b, a; } rdna4swz_vk_t;

static inline rdna4swz_t rdna4swz_identity(void) {
    return (rdna4swz_t){ RDNA4SWZ_RED, RDNA4SWZ_GREEN, RDNA4SWZ_BLUE, RDNA4SWZ_ALPHA };
}
static inline int rdna4swz_valid(rdna4swz_t s) {
    return s.r <= RDNA4SWZ_ALPHA && s.g <= RDNA4SWZ_ALPHA &&
           s.b <= RDNA4SWZ_ALPHA && s.a <= RDNA4SWZ_ALPHA;
}
static inline int rdna4swz_is_identity(rdna4swz_t s) {
    return s.r == RDNA4SWZ_RED && s.g == RDNA4SWZ_GREEN &&
           s.b == RDNA4SWZ_BLUE && s.a == RDNA4SWZ_ALPHA;
}
static inline int rdna4swz_compose(rdna4swz_t parent, rdna4swz_t view, rdna4swz_t *out) {
    if (!out || !rdna4swz_valid(parent) || !rdna4swz_valid(view)) return 0;
    const uint8_t p[4] = { parent.r, parent.g, parent.b, parent.a };
    const uint8_t v[4] = { view.r, view.g, view.b, view.a };
    uint8_t r[4];
    for (int i = 0; i < 4; i++)
        r[i] = v[i] >= RDNA4SWZ_RED ? p[v[i] - RDNA4SWZ_RED] : v[i];
    *out = (rdna4swz_t){ r[0], r[1], r[2], r[3] };
    return 1;
}
static inline int rdna4swz_to_vk(uint32_t fmt_flags, rdna4swz_t swz, rdna4swz_vk_t *out) {
    static const uint32_t base[6] = {
        RDNA4VKSWZ_ZERO, RDNA4VKSWZ_ONE,
        RDNA4VKSWZ_R, RDNA4VKSWZ_G, RDNA4VKSWZ_B, RDNA4VKSWZ_A
    };
    static const uint32_t a8[6] = {
        RDNA4VKSWZ_ZERO, RDNA4VKSWZ_ONE,
        RDNA4VKSWZ_ZERO, RDNA4VKSWZ_ZERO, RDNA4VKSWZ_ZERO, RDNA4VKSWZ_R
    };
    if (!out || !rdna4swz_valid(swz)) return 0;
    const uint32_t *m = (fmt_flags & RDNA4SWZ_FMT_A8) ? a8 : base;
    *out = (rdna4swz_vk_t){ m[swz.r], m[swz.g], m[swz.b], m[swz.a] };
    return 1;
}
static inline int rdna4swz_vk_is_identity(rdna4swz_vk_t m) {
    return (m.r == RDNA4VKSWZ_R || m.r == RDNA4VKSWZ_IDENTITY) &&
           (m.g == RDNA4VKSWZ_G || m.g == RDNA4VKSWZ_IDENTITY) &&
           (m.b == RDNA4VKSWZ_B || m.b == RDNA4VKSWZ_IDENTITY) &&
           (m.a == RDNA4VKSWZ_A || m.a == RDNA4VKSWZ_IDENTITY);
}
static inline uint32_t rdna4swz_key(rdna4swz_t s) {
    return (uint32_t)s.r | ((uint32_t)s.g << 8) | ((uint32_t)s.b << 16) | ((uint32_t)s.a << 24);
}
static inline int rdna4swz_check(rdna4swz_t swz, unsigned long usage, uint32_t fmt_flags,
                                 int is_view, const char **why) {
    if (why) *why = NULL;
    if (!rdna4swz_valid(swz)) {
        if (why) *why = "invalid MTLTextureSwizzle channel enum";
        return 0;
    }
    if (rdna4swz_is_identity(swz)) return 1;
    if (fmt_flags & RDNA4SWZ_FMT_TEXEL) {
        if (why) *why = "non-identity swizzle is not supported on texture buffers";
        return 0;
    }
    if (usage & RDNA4SWZ_USE_WRITE) {
        if (why) *why = "non-identity swizzle is incompatible with MTLTextureUsageShaderWrite";
        return 0;
    }
    if (!is_view && (usage & RDNA4SWZ_USE_RT)) {
        if (why) *why = "non-identity descriptor swizzle is incompatible with MTLTextureUsageRenderTarget";
        return 0;
    }
    if (!(usage & RDNA4SWZ_USE_READ)) {
        if (why) *why = "non-identity swizzle requires MTLTextureUsageShaderRead";
        return 0;
    }
    return 1;
}
#endif


#endif // RDNA4_SWIZZLE_H
