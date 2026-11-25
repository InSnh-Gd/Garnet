// garnet_vkpool.h — Pure-C bounded Vulkan handle reuse stacks (VkFence, VkCommandBuffer).
// Host-tested under ASan+UBSan by src/garnet-metal/test-vkpool.c.
// Caller holds the pool's mutex around every operation.
#ifndef GARNET_VKPOOL_H
#define GARNET_VKPOOL_H

#include <stdint.h>
#include <stddef.h>

#define GARNET_VKPOOL_FENCE_CAP 64u
#define GARNET_VKPOOL_CMD_CAP   32u
#define GARNET_VKPOOL_ONESHOT_CAP 16u

typedef struct {
    uint64_t handles[GARNET_VKPOOL_FENCE_CAP];
    uint32_t count;
    uint32_t cap;
    uint64_t hits;
    uint64_t misses;
    uint64_t returns;
    uint64_t drops;
} garnet_vkpool_t;

static inline void garnet_vkpool_init(garnet_vkpool_t *p, uint32_t cap) {
    if (!p) return;
    if (cap > GARNET_VKPOOL_FENCE_CAP) cap = GARNET_VKPOOL_FENCE_CAP;
    p->count = 0u;
    p->cap = cap;
    p->hits = 0u;
    p->misses = 0u;
    p->returns = 0u;
    p->drops = 0u;
}

// Pop one cached handle (LIFO for cache warmth), or 0 if empty.
static inline uint64_t garnet_vkpool_pop(garnet_vkpool_t *p) {
    if (!p || p->count == 0u) {
        if (p) p->misses++;
        return 0u;
    }
    uint64_t h = p->handles[--p->count];
    p->handles[p->count] = 0u;
    p->hits++;
    return h;
}

// Push one clean, resettable handle back into the pool.
// Returns 1 if stored (caller must NOT destroy/free it), 0 if rejected/full (caller must destroy/free it).
static inline int garnet_vkpool_push(garnet_vkpool_t *p, uint64_t handle) {
    if (!p || !handle) return 0;
    if (p->count >= p->cap) {
        p->drops++;
        return 0;
    }
    p->handles[p->count++] = handle;
    p->returns++;
    return 1;
}

// Drain up to maxOut handles into out[] (used during teardown or flush).
static inline uint32_t garnet_vkpool_drain(garnet_vkpool_t *p, uint64_t *out, uint32_t maxOut) {
    if (!p) return 0u;
    uint32_t n = 0u;
    while (p->count > 0u && n < maxOut) {
        uint64_t h = p->handles[--p->count];
        p->handles[p->count] = 0u;
        if (out) out[n] = h;
        n++;
    }
    return n;
}

// Dynamic render-pass state dirty bits for RDNA4RenderEncoder (when drawopt is ON).
enum {
    GARNET_DYN_VIEWPORT  = 1u << 0,
    GARNET_DYN_SCISSOR   = 1u << 1,
    GARNET_DYN_BLEND     = 1u << 2,
    GARNET_DYN_STENCIL_M = 1u << 3,   // compare/write masks (from depth-stencil state)
    GARNET_DYN_STENCIL_R = 1u << 4,   // front/back reference values
    GARNET_DYN_DEPTHBIAS = 1u << 5,
    GARNET_DYN_ALL       = 0x3Fu,
};

#endif
