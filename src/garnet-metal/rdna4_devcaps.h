// Cached Vulkan-to-Metal limits; pure C so the allocation policy is host-testable.
#ifndef RDNA4_DEVCAPS_H
#define RDNA4_DEVCAPS_H
#include <stddef.h>
#include <stdint.h>

#define RDNA4DC_HEAP_PAGE UINT64_C(16384)
#define RDNA4DC_MEMORY_CAPS_VERSION UINT64_C(1)
#define RDNA4DC_MEMORY_CAPS_PROPERTY "Garnet,MemoryCaps"

typedef struct {
    uint64_t size;
    int device_local;
} rdna4dc_heap;

typedef struct {
    const char *name;
    uint64_t storage_buffer_range, allocation_size;
    const rdna4dc_heap *heaps;
    size_t heap_count;
    uint32_t workgroup_size[3], invocations, shared_memory, subgroup_size;
    uint64_t allocation_budget;
    int budget_valid;
    uint64_t logical_buffer_size;
    int addressing64;
} rdna4dc_properties;

typedef struct {
    char name[256];
    uint64_t buffer_length, working_set, allocation_budget;
    uint32_t workgroup_size[3], invocations, shared_memory, subgroup_size;
} rdna4dc_caps;

typedef struct {
    uint64_t version, physical, pool, visible, application_budget, max_buffer;
    uint64_t composite_max_buffer;
} rdna4dc_native_memory;

// Queries must not open RADV. Until it opens, use Vulkan minimum compute/storage
// limits and a small provisional heap budget, without naming an unobserved GPU.
static inline rdna4dc_caps rdna4dc_fallback(void) {
    return (rdna4dc_caps){ .name = "AMD Radeon (native Vulkan)",
        .buffer_length = UINT64_C(128) << 20, .working_set = UINT64_C(256) << 20,
        .allocation_budget = UINT64_C(256) << 20,
        .workgroup_size = {128, 128, 64}, .invocations = 128,
        .shared_memory = 16384, .subgroup_size = 32 };
}

static inline uint64_t rdna4dc_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

// The nub publishes this immutable snapshot before registering the accelerator.
// It describes implemented limits, not current free bytes or a second heap.
static inline int rdna4dc_native_memory_valid(const rdna4dc_native_memory *m) {
    return m && m->version == RDNA4DC_MEMORY_CAPS_VERSION && m->pool &&
        m->physical >= m->pool && m->visible <= m->pool &&
        m->application_budget <= m->pool && m->max_buffer && m->max_buffer <= m->pool;
}

static inline rdna4dc_caps rdna4dc_from_native_memory(const rdna4dc_native_memory *m, int is_ws) {
    rdna4dc_caps c = rdna4dc_fallback();
    if (!rdna4dc_native_memory_valid(m)) return c;
    c.allocation_budget = is_ws ? m->pool : m->application_budget;
    c.working_set = rdna4dc_min(c.allocation_budget, UINT64_C(12) << 30);
    c.buffer_length = m->max_buffer;
    // Additive schema-v1 field: old bundles keep their descriptor-sized limit.
    if (m->composite_max_buffer >= m->max_buffer &&
        m->composite_max_buffer <= m->pool &&
        m->composite_max_buffer <= (UINT64_C(8) << 30))
        c.buffer_length = m->composite_max_buffer;
    return c;
}

static inline rdna4dc_caps rdna4dc_from_properties(const rdna4dc_properties *p) {
    rdna4dc_caps c = rdna4dc_fallback();
    if (p->name && p->name[0]) {
        size_t i = 0;
        for (; i < sizeof c.name - 1 && p->name[i]; i++) c.name[i] = p->name[i];
        c.name[i] = '\0';
    }
    // Vulkan heaps are disjoint. RADV splits visible VRAM from high VRAM;
    // sum them once for the pool, but one allocation still belongs to one heap.
    uint64_t local = 0, pool = 0;
    for (size_t i = 0; i < p->heap_count; i++) {
        if (!p->heaps[i].device_local) continue;
        uint64_t size = p->heaps[i].size;
        if (size > local) local = size;
        pool = size > UINT64_MAX - pool ? UINT64_MAX : pool + size;
    }
    c.allocation_budget = p->budget_valid ? rdna4dc_min(pool, p->allocation_budget) : pool;
    c.working_set = rdna4dc_min(c.allocation_budget, UINT64_C(12) << 30);
    c.buffer_length = p->addressing64 && p->logical_buffer_size
        ? rdna4dc_min(p->logical_buffer_size, local)
        : rdna4dc_min(rdna4dc_min(p->storage_buffer_range, local), UINT32_MAX);
    // maintenance3 is optional before Vulkan 1.1; zero means not queried.
    if (p->allocation_size) c.buffer_length = rdna4dc_min(c.buffer_length, p->allocation_size);
    c.invocations = (uint32_t)rdna4dc_min(p->invocations, 1024);
    for (size_t i = 0; i < 3; i++)
        c.workgroup_size[i] = (uint32_t)rdna4dc_min(p->workgroup_size[i], c.invocations);
    c.shared_memory = (uint32_t)rdna4dc_min(p->shared_memory, 32768);
    if (p->subgroup_size) c.subgroup_size = p->subgroup_size;
    return c;
}

static inline int rdna4dc_buffer_ok(const rdna4dc_caps *c, uint64_t length) {
    return length && length <= c->buffer_length;
}

static inline uint32_t rdna4dc_thread_execution_width(const rdna4dc_caps *c) {
    return (c && c->subgroup_size && c->subgroup_size < 32u) ? c->subgroup_size : 32u;
}

static inline uint64_t rdna4dc_heap_size(uint64_t requested, uint64_t limit) {
    if (!requested || requested > UINT64_MAX - (RDNA4DC_HEAP_PAGE - 1)) return 0;
    uint64_t rounded = (requested + RDNA4DC_HEAP_PAGE - 1) & ~(RDNA4DC_HEAP_PAGE - 1);
    return rounded <= limit ? rounded : 0;
}

static inline uint64_t rdna4dc_heap_available(const rdna4dc_caps *c, uint64_t used) {
    return used < c->allocation_budget ? c->allocation_budget - used : 0;
}

static inline int rdna4dc_workgroup_ok(const rdna4dc_caps *c, uint64_t x, uint64_t y, uint64_t z) {
    const uint64_t dims[3] = {x, y, z};
    uint64_t total = 1;
    for (size_t i = 0; i < 3; i++) {
        if (!dims[i] || dims[i] > c->workgroup_size[i] || dims[i] > c->invocations / total) return 0;
        total *= dims[i];
    }
    return 1;
}
#endif
