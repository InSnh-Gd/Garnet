// Cached Vulkan-to-Metal limits; pure C so the allocation policy is host-testable.
#ifndef RDNA4_DEVCAPS_H
#define RDNA4_DEVCAPS_H
#include <stddef.h>
#include <stdint.h>

#define RDNA4DC_HEAP_PAGE UINT64_C(16384)
#define RDNA4DC_MEMORY_CAPS_VERSION UINT64_C(1)
#define RDNA4DC_MEMORY_CAPS_PROPERTY "Garnet,MemoryCaps"
#define RDNA4DC_MESH_MAX_PAYLOAD_BYTES UINT32_C(16384)
#define RDNA4DC_MESH_MIN_GRID UINT32_C(1024)
#define RDNA4DC_MESH_MAX_INVOCATIONS UINT32_C(1024)
#define RDNA4DC_MESH_MAX_VERTICES UINT32_C(256)
#define RDNA4DC_MESH_MAX_PRIMITIVES UINT32_C(512)
#define RDNA4DC_IO_MAX_COMMANDS UINT32_C(8192)

typedef struct {
    uint64_t size;
    int device_local;
} rdna4dc_heap;

typedef struct {
    int api_1_2;
    int shader_int64;
    int buffer_device_address;
    int sample_rate_shading;
    int ext_barycentric, feat_barycentric;
    int ext_atomic_float, feat_atomic_float_buffer, feat_atomic_float_buffer_add;
    int feat_atomic_float_shared, feat_atomic_float_shared_add;
    int feat_atomic_float_image, feat_atomic_float_image_add;
    int ext_atomic_float2, feat_atomic_float_buffer_minmax, feat_atomic_float_shared_minmax;
    int ext_atomic_int64, feat_atomic_int64_buffer, feat_atomic_int64_shared;
    int ext_mesh_shader, feat_task_shader, feat_mesh_shader;
    uint32_t mesh_max_payload_bytes;
    uint32_t object_max_workgroup_count[3], object_max_workgroup_total_count, object_max_workgroup_invocations;
    uint32_t mesh_max_workgroup_count[3], mesh_max_workgroup_total_count, mesh_max_workgroup_invocations;
    uint32_t mesh_max_output_vertices, mesh_max_output_primitives;
    int ext_deferred_host_ops, ext_accel_struct, feat_accel_struct;
    int feat_accel_indirect_build, feat_accel_uab;
    uint32_t rt_min_scratch_alignment;
    uint64_t rt_max_geometries, rt_max_instances, rt_max_primitives;
    int ext_ray_query, feat_ray_query;
    int timestamp_query_funcs, timestamp_compute_and_graphics;
    float timestamp_period_ns;
    uint32_t timestamp_valid_bits;
    int sparse_queue, sparse_bind_func;
    int sparse_binding, sparse_residency_buffer;
    int sparse_residency_image_2d, sparse_residency_image_3d;
    int sparse_residency_2_samples, sparse_residency_4_samples;
    int sparse_residency_8_samples, sparse_residency_16_samples;
    int sparse_residency_aliased;
} rdna4dc_metal3_probe;

typedef struct {
    int barycentric;
    int pull_model_interpolation;
    int atomic_float_buffer, atomic_float_shared, atomic_float_image, atomic_float_minmax;
    int atomic_int64_buffer, atomic_int64_shared;
    int object_shader, mesh_shader;
    uint32_t mesh_max_payload_bytes;
    uint32_t object_max_workgroup_count[3], object_max_workgroup_total_count, object_max_workgroup_invocations;
    uint32_t mesh_max_workgroup_count[3], mesh_max_workgroup_total_count, mesh_max_workgroup_invocations;
    uint32_t mesh_max_output_vertices, mesh_max_output_primitives;
    int deferred_host_ops, acceleration_structure, accel_indirect_build, accel_uab, ray_query;
    uint32_t rt_min_scratch_alignment;
    uint64_t rt_max_geometries, rt_max_instances, rt_max_primitives;
    int timestamp_supported, timestamp_compute_and_graphics;
    float timestamp_period_ns;
    uint32_t timestamp_valid_bits;
    int sparse_binding, sparse_residency_buffer;
    int sparse_residency_image_2d, sparse_residency_image_3d;
    int sparse_residency_2_samples, sparse_residency_4_samples;
    int sparse_residency_8_samples, sparse_residency_16_samples;
    int sparse_residency_aliased;
} rdna4dc_metal3_caps;

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
    const rdna4dc_metal3_probe *metal3;
} rdna4dc_properties;

typedef struct {
    char name[256];
    uint64_t buffer_length, working_set, allocation_budget;
    uint32_t workgroup_size[3], invocations, shared_memory, subgroup_size;
    rdna4dc_metal3_caps metal3;
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

static inline rdna4dc_metal3_caps rdna4dc_metal3_fallback(void) {
    return (rdna4dc_metal3_caps){0};
}

// Normalize and gate probed Vulkan Metal 3 extensions/limits. Unprobed or
// missing-dependency features always stay 0/false (never fake support).
static inline rdna4dc_metal3_caps rdna4dc_metal3_from_probe(const rdna4dc_metal3_probe *p) {
    rdna4dc_metal3_caps m = rdna4dc_metal3_fallback();
    if (!p) return m;
    m.barycentric = (p->ext_barycentric && p->feat_barycentric) ? 1 : 0;
    m.pull_model_interpolation = p->sample_rate_shading ? 1 : 0;
    if (p->ext_atomic_float) {
        m.atomic_float_buffer = (p->feat_atomic_float_buffer && p->feat_atomic_float_buffer_add) ? 1 : 0;
        m.atomic_float_shared = (p->feat_atomic_float_shared && p->feat_atomic_float_shared_add) ? 1 : 0;
        m.atomic_float_image = (p->feat_atomic_float_image && p->feat_atomic_float_image_add) ? 1 : 0;
        if (p->ext_atomic_float2)
            m.atomic_float_minmax = (p->feat_atomic_float_buffer_minmax && p->feat_atomic_float_shared_minmax) ? 1 : 0;
    }
    if (p->shader_int64 && (p->api_1_2 || p->ext_atomic_int64)) {
        m.atomic_int64_buffer = p->feat_atomic_int64_buffer ? 1 : 0;
        m.atomic_int64_shared = p->feat_atomic_int64_shared ? 1 : 0;
    }
    if (p->api_1_2 && p->ext_mesh_shader && p->feat_mesh_shader) {
        m.mesh_shader = 1;
        m.mesh_max_workgroup_total_count = p->mesh_max_workgroup_total_count;
        for (size_t i = 0; i < 3; i++)
            m.mesh_max_workgroup_count[i] = (uint32_t)rdna4dc_min(p->mesh_max_workgroup_count[i], m.mesh_max_workgroup_total_count);
        m.mesh_max_workgroup_invocations = (uint32_t)rdna4dc_min(p->mesh_max_workgroup_invocations, RDNA4DC_MESH_MAX_INVOCATIONS);
        m.mesh_max_output_vertices = (uint32_t)rdna4dc_min(p->mesh_max_output_vertices, RDNA4DC_MESH_MAX_VERTICES);
        m.mesh_max_output_primitives = (uint32_t)rdna4dc_min(p->mesh_max_output_primitives, RDNA4DC_MESH_MAX_PRIMITIVES);
        if (p->feat_task_shader) {
            m.object_shader = 1;
            m.mesh_max_payload_bytes = (uint32_t)rdna4dc_min(p->mesh_max_payload_bytes, RDNA4DC_MESH_MAX_PAYLOAD_BYTES);
            m.object_max_workgroup_total_count = p->object_max_workgroup_total_count;
            for (size_t i = 0; i < 3; i++)
                m.object_max_workgroup_count[i] = (uint32_t)rdna4dc_min(p->object_max_workgroup_count[i], m.object_max_workgroup_total_count);
            m.object_max_workgroup_invocations = (uint32_t)rdna4dc_min(p->object_max_workgroup_invocations, RDNA4DC_MESH_MAX_INVOCATIONS);
        }
    }
    m.deferred_host_ops = p->ext_deferred_host_ops ? 1 : 0;
    if (p->api_1_2 && p->buffer_device_address && p->ext_deferred_host_ops && p->ext_accel_struct && p->feat_accel_struct) {
        m.acceleration_structure = 1;
        m.accel_indirect_build = p->feat_accel_indirect_build ? 1 : 0;
        m.accel_uab = p->feat_accel_uab ? 1 : 0;
        m.rt_min_scratch_alignment = p->rt_min_scratch_alignment;
        m.rt_max_geometries = p->rt_max_geometries;
        m.rt_max_instances = p->rt_max_instances;
        m.rt_max_primitives = p->rt_max_primitives;
        if (p->ext_ray_query && p->feat_ray_query) m.ray_query = 1;
    }
    if (p->timestamp_query_funcs && p->timestamp_valid_bits > 0u && p->timestamp_valid_bits <= 64u &&
        p->timestamp_period_ns > 0.0f && p->timestamp_period_ns < 1e9f) {
        m.timestamp_supported = 1;
        m.timestamp_compute_and_graphics = p->timestamp_compute_and_graphics ? 1 : 0;
        m.timestamp_period_ns = p->timestamp_period_ns;
        m.timestamp_valid_bits = p->timestamp_valid_bits;
    }
    if (p->sparse_binding && p->sparse_queue && p->sparse_bind_func) {
        m.sparse_binding = 1;
        m.sparse_residency_buffer = p->sparse_residency_buffer ? 1 : 0;
        m.sparse_residency_image_2d = p->sparse_residency_image_2d ? 1 : 0;
        m.sparse_residency_image_3d = p->sparse_residency_image_3d ? 1 : 0;
        m.sparse_residency_2_samples = p->sparse_residency_2_samples ? 1 : 0;
        m.sparse_residency_4_samples = p->sparse_residency_4_samples ? 1 : 0;
        m.sparse_residency_8_samples = p->sparse_residency_8_samples ? 1 : 0;
        m.sparse_residency_16_samples = p->sparse_residency_16_samples ? 1 : 0;
        m.sparse_residency_aliased = p->sparse_residency_aliased ? 1 : 0;
    }
    return m;
}

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
    c.metal3 = rdna4dc_metal3_from_probe(p->metal3);
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

static inline int rdna4dc_metal3_barycentric_ok(const rdna4dc_metal3_caps *c) {
    return c && c->barycentric;
}

static inline int rdna4dc_metal3_pull_model_ok(const rdna4dc_metal3_caps *c) {
    return c && c->pull_model_interpolation;
}

static inline int rdna4dc_metal3_atomic_float_ok(const rdna4dc_metal3_caps *c) {
    return c && c->atomic_float_buffer && c->atomic_float_shared;
}

static inline int rdna4dc_metal3_atomic_int64_ok(const rdna4dc_metal3_caps *c) {
    return c && c->atomic_int64_buffer && c->atomic_int64_shared;
}

static inline int rdna4dc_metal3_grid_ok(const uint32_t max_count[3], uint32_t max_total,
                                         uint64_t x, uint64_t y, uint64_t z) {
    if (!max_count || !max_total) return 0;
    const uint64_t dims[3] = {x, y, z};
    uint64_t total = 1;
    for (size_t i = 0; i < 3; i++) {
        if (!dims[i] || dims[i] > max_count[i] || dims[i] > max_total / total) return 0;
        total *= dims[i];
    }
    return 1;
}

static inline int rdna4dc_metal3_mesh_supported(const rdna4dc_metal3_caps *c) {
    return c && c->mesh_shader && c->object_shader &&
        c->mesh_max_payload_bytes >= RDNA4DC_MESH_MAX_PAYLOAD_BYTES &&
        c->mesh_max_workgroup_total_count >= RDNA4DC_MESH_MIN_GRID &&
        c->mesh_max_workgroup_count[0] >= RDNA4DC_MESH_MIN_GRID &&
        c->mesh_max_workgroup_count[1] > 0u && c->mesh_max_workgroup_count[2] > 0u &&
        c->object_max_workgroup_total_count > 0u &&
        c->object_max_workgroup_count[0] > 0u &&
        c->object_max_workgroup_count[1] > 0u && c->object_max_workgroup_count[2] > 0u &&
        c->mesh_max_workgroup_invocations > 0u && c->object_max_workgroup_invocations > 0u &&
        c->mesh_max_output_vertices > 0u && c->mesh_max_output_primitives > 0u;
}

static inline int rdna4dc_metal3_mesh_payload_ok(const rdna4dc_metal3_caps *c, uint64_t bytes) {
    return c && c->object_shader && bytes <= c->mesh_max_payload_bytes;
}

static inline int rdna4dc_metal3_mesh_grid_ok(const rdna4dc_metal3_caps *c, uint64_t x, uint64_t y, uint64_t z) {
    return c && c->mesh_shader &&
        rdna4dc_metal3_grid_ok(c->mesh_max_workgroup_count, c->mesh_max_workgroup_total_count, x, y, z);
}

static inline int rdna4dc_metal3_object_grid_ok(const rdna4dc_metal3_caps *c, uint64_t x, uint64_t y, uint64_t z) {
    return c && c->object_shader &&
        rdna4dc_metal3_grid_ok(c->object_max_workgroup_count, c->object_max_workgroup_total_count, x, y, z);
}

static inline int rdna4dc_metal3_raytracing_supported(const rdna4dc_metal3_caps *c) {
    return c && c->deferred_host_ops && c->acceleration_structure && c->ray_query;
}

static inline int rdna4dc_metal3_timestamps_supported(const rdna4dc_metal3_caps *c) {
    return c && c->timestamp_supported && c->timestamp_valid_bits > 0u &&
        c->timestamp_valid_bits <= 64u && c->timestamp_period_ns > 0.0f && c->timestamp_period_ns < 1e9f;
}

static inline int rdna4dc_metal3_sparse_buffer_ok(const rdna4dc_metal3_caps *c) {
    return c && c->sparse_binding && c->sparse_residency_buffer;
}

static inline int rdna4dc_metal3_sparse_texture_ok(const rdna4dc_metal3_caps *c) {
    return c && c->sparse_binding && c->sparse_residency_image_2d;
}
#endif
