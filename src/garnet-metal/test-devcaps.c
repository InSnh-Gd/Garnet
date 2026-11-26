// cc -std=c11 -O2 -Wall -Wextra -Werror test-devcaps.c -o /tmp/test-devcaps && /tmp/test-devcaps
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "garnet_devcaps.h"

#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
static unsigned checks, failures;
#define CHECK(message, condition) do { \
    checks++; \
    if (condition) printf("PASS: %s\n", message); \
    else { printf("FAIL: %s (line %d)\n", message, __LINE__); failures++; } \
} while (0)

int main(void) {
    rdna4dc_caps fallback = rdna4dc_fallback();
    CHECK("pre-open identity does not invent a 9070 or 9060", !strstr(fallback.name, "9070") && !strstr(fallback.name, "9060"));
    CHECK("pre-open buffer limit is Vulkan's 128 MiB storage minimum", fallback.buffer_length == 128 * MIB);
    CHECK("pre-open budget is provisional, not a 12 GiB VRAM claim", fallback.working_set == 256 * MIB);
    CHECK("pre-open compute limits use minimum axes/invocations/shared memory", fallback.workgroup_size[0] == 128 && fallback.workgroup_size[1] == 128 && fallback.workgroup_size[2] == 64 && fallback.invocations == 128 && fallback.shared_memory == 16384);
    CHECK("pre-open workgroups enforce the product, not only each axis", rdna4dc_workgroup_ok(&fallback, 128, 1, 1) && !rdna4dc_workgroup_ok(&fallback, 128, 2, 1));

    rdna4dc_native_memory native = { .version = RDNA4DC_MEMORY_CAPS_VERSION,
        .physical = 16 * GIB, .pool = 15 * GIB, .visible = 217 * MIB,
        .application_budget = 14 * GIB - 64 * MIB, .max_buffer = UINT64_C(0xfffffffc) };
    rdna4dc_caps cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("a known native device exposes implemented memory limits before opening RADV",
        cold.buffer_length == UINT64_C(0xfffffffc) && cold.allocation_budget == 14 * GIB - 64 * MIB);
    CHECK("the recommendation is independent of the allocation budget",
        cold.working_set == 12 * GIB && rdna4dc_heap_available(&cold, 12 * GIB) == 2 * GIB - 64 * MIB);
    CHECK("WindowServer retains its unbudgeted pool", rdna4dc_from_native_memory(&native, 1).allocation_budget == 15 * GIB);
    CHECK("a memory-only snapshot does not invent higher compute limits", cold.invocations == fallback.invocations && cold.shared_memory == fallback.shared_memory);
    const rdna4dc_native_memory malformed[] = {
        {0}, {2, 16 * GIB, 15 * GIB, 217 * MIB, 14 * GIB, GIB, 0},
        {1, GIB, 2 * GIB, MIB, GIB, GIB, 0}, {1, GIB, GIB, 2 * GIB, GIB, GIB, 0},
        {1, GIB, GIB, MIB, 2 * GIB, GIB, 0}, {1, GIB, GIB, MIB, GIB, 0, 0},
        {1, GIB, GIB, MIB, GIB, 2 * GIB, 0}, {1, UINT64_MAX, 0, 0, 0, GIB, 0}
    };
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        CHECK("unknown or malformed native snapshot keeps the conservative fallback",
            !rdna4dc_native_memory_valid(&malformed[i]) && rdna4dc_from_native_memory(&malformed[i], 0).buffer_length == fallback.buffer_length);
    }
    native.application_budget = 0;
    cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("a valid exhausted native budget preserves the implemented resource limit",
        cold.buffer_length == native.max_buffer && !cold.allocation_budget && !cold.working_set);
    native.max_buffer = 8 * GIB;
    native.application_budget = 2 * GIB;
    cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("pre-open logical length is independent of the current allocation allowance",
        cold.buffer_length == 8 * GIB && cold.allocation_budget == 2 * GIB);
    native.max_buffer = UINT64_C(0xfffffffc);
    native.composite_max_buffer = 8 * GIB;
    cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("new clients discover composite capacity without raising the legacy field",
        cold.buffer_length == 8 * GIB && native.max_buffer == UINT64_C(0xfffffffc));
    native.application_budget = 0;
    CHECK("composite discovery does not confuse current budget with logical length",
        rdna4dc_from_native_memory(&native, 0).buffer_length == 8 * GIB &&
        !rdna4dc_from_native_memory(&native, 0).allocation_budget);
    const uint64_t invalid_composite[] = {UINT64_MAX, 16 * GIB, 9 * GIB, GIB};
    for (size_t i = 0; i < sizeof invalid_composite / sizeof invalid_composite[0]; i++) {
        native.composite_max_buffer = invalid_composite[i];
        CHECK("invalid optional composite limits preserve the valid legacy capacity",
            rdna4dc_from_native_memory(&native, 0).buffer_length == native.max_buffer);
    }
    native.composite_max_buffer = 0;
    native.max_buffer = 8 * GIB;
    native.application_budget = 2 * GIB;
    cold = rdna4dc_from_native_memory(&native, 0);

    rdna4dc_heap heaps[] = {{32 * GIB, 0}, {8 * GIB, 1}, {256 * MIB, 1}};
    char name[] = "AMD Radeon RX 9060 XT (RADV GFX1200)";
    rdna4dc_properties p = { .name = name, .storage_buffer_range = UINT32_MAX,
        .allocation_size = 8 * GIB, .heaps = heaps, .heap_count = 3,
        .workgroup_size = {1024, 1024, 1024}, .invocations = 1024,
        .shared_memory = 65536, .subgroup_size = 64 };
    rdna4dc_caps c = rdna4dc_from_properties(&p);
    CHECK("gfx1200 identity comes from the queried name", !strcmp(c.name, name));
    name[0] = 'X';
    CHECK("the published name owns its bytes", c.name[0] == 'A');
    CHECK("disjoint high and visible heaps are counted once without a second reduction", c.working_set == 8 * GIB + 256 * MIB && c.allocation_budget == 8 * GIB + 256 * MIB);
    CHECK("host RAM is excluded from the VRAM budget", c.allocation_budget != 32 * GIB + 8 * GIB + 256 * MIB);
    CHECK("UINT32_MAX buffer range does not admit the old 4 GiB off-by-one", c.buffer_length == UINT32_MAX && rdna4dc_buffer_ok(&c, UINT32_MAX) && !rdna4dc_buffer_ok(&c, UINT64_C(1) << 32));
    CHECK("zero length is rejected through the common buffer constructor", !rdna4dc_buffer_ok(&c, 0));
    CHECK("LDS keeps the existing 32 KiB Metal ceiling", c.shared_memory == 32768);
    CHECK("compute pipeline execution width follows Vulkan subgroup properties", c.subgroup_size == 64);
    CHECK("Metal compute threadExecutionWidth tracks Metal 32-lane simdgroup execution",
        rdna4dc_thread_execution_width(&fallback) == 32 &&
        rdna4dc_thread_execution_width(&c) == 32);
    p.subgroup_size = 32;
    CHECK("subgroupSize 32 reports threadExecutionWidth 32", rdna4dc_thread_execution_width(&(rdna4dc_caps){ .subgroup_size = 32 }) == 32);
    CHECK("narrow hardware subgroups (<32) preserve smaller width", rdna4dc_thread_execution_width(&(rdna4dc_caps){ .subgroup_size = 16 }) == 16);
    p.subgroup_size = 64;
    CHECK("a valid 1024-thread group is accepted", rdna4dc_workgroup_ok(&c, 32, 32, 1));
    CHECK("zero and out-of-axis local sizes are refused", !rdna4dc_workgroup_ok(&c, 1, 0, 1) && !rdna4dc_workgroup_ok(&c, 1025, 1, 1));
    CHECK("a per-axis-valid group can still exceed the invocation limit", !rdna4dc_workgroup_ok(&c, 32, 32, 2));

    p.allocation_size = 64 * MIB;
    c = rdna4dc_from_properties(&p);
    CHECK("maintenance3 limits single buffers independently of the heap budget", c.buffer_length == 64 * MIB && c.working_set == 8 * GIB + 256 * MIB);
    CHECK("the exact allocation boundary is accepted, the next byte is refused", rdna4dc_buffer_ok(&c, 64 * MIB) && !rdna4dc_buffer_ok(&c, 64 * MIB + 1));
    p.allocation_size = 0;
    p.storage_buffer_range = 16 * MIB;
    c = rdna4dc_from_properties(&p);
    CHECK("storage descriptor range also constrains small devices", c.buffer_length == 16 * MIB);
    p.storage_buffer_range = UINT64_MAX;
    heaps[1].size = 16 * GIB;
    c = rdna4dc_from_properties(&p);
    CHECK("a 12 GiB recommendation does not cap the disjoint allocation budget", c.working_set == 12 * GIB && c.allocation_budget == 16 * GIB + 256 * MIB && c.buffer_length == UINT32_MAX);
    p.budget_valid = 1; p.allocation_budget = 9 * GIB;
    c = rdna4dc_from_properties(&p);
    CHECK("the backend budget bounds the pool without shrinking physical heaps", c.allocation_budget == 9 * GIB && c.working_set == 9 * GIB && heaps[1].size == 16 * GIB);
    p.allocation_budget = UINT64_MAX;
    c = rdna4dc_from_properties(&p);
    CHECK("a malformed oversized budget is bounded by the physical pool", c.allocation_budget == 16 * GIB + 256 * MIB);
    p.allocation_budget = 0;
    c = rdna4dc_from_properties(&p);
    CHECK("a measured zero budget stays exhausted", c.allocation_budget == 0 && c.working_set == 0);
    p.budget_valid = 0;

    p.logical_buffer_size = 8 * GIB; p.allocation_size = 8 * GIB;
    p.storage_buffer_range = 128 * MIB;
    c = rdna4dc_from_properties(&p);
    CHECK("large Vulkan buffers require the implemented 64-bit addressing path", c.buffer_length == 128 * MIB);
    p.addressing64 = 1;
    c = rdna4dc_from_properties(&p);
    CHECK("logical buffer length is independent of a bounded descriptor view", c.buffer_length == 8 * GIB && rdna4dc_buffer_ok(&c, 8 * GIB));
    p.budget_valid = 1; p.allocation_budget = 2 * GIB;
    c = rdna4dc_from_properties(&p);
    CHECK("opened logical length uses the same budget-independent contract as native discovery",
        c.buffer_length == cold.buffer_length && c.allocation_budget == cold.allocation_budget);
    p.budget_valid = 0;
    p.logical_buffer_size = 6 * GIB;
    CHECK("maintenance4 independently bounds buffer creation", rdna4dc_from_properties(&p).buffer_length == 6 * GIB);
    p.allocation_size = 5 * GIB;
    CHECK("maintenance3 still bounds large backing allocations", rdna4dc_from_properties(&p).buffer_length == 5 * GIB);
    p.logical_buffer_size = 0;
    CHECK("missing logical size stays conservative despite BDA", rdna4dc_from_properties(&p).buffer_length == 128 * MIB);
    p.addressing64 = 0; p.logical_buffer_size = 0; p.allocation_size = 0; p.storage_buffer_range = UINT64_MAX;

    heaps[1].size = 128 * MIB;
    heaps[2].size = 0;
    p.workgroup_size[0] = 512; p.workgroup_size[1] = 128; p.workgroup_size[2] = 64;
    p.invocations = 256; p.shared_memory = 8192; p.subgroup_size = 32;
    c = rdna4dc_from_properties(&p);
    CHECK("small local heaps constrain both buffer length and working set", c.buffer_length == 128 * MIB && c.working_set == 128 * MIB);
    CHECK("workgroup axes are bounded by total invocations", c.workgroup_size[0] == 256 && c.workgroup_size[1] == 128 && c.workgroup_size[2] == 64);
    CHECK("lower shared memory and subgroup limits are preserved", c.shared_memory == 8192 && c.subgroup_size == 32);
    CHECK("a heap made before open cannot keep allocating above a newly smaller budget", rdna4dc_heap_available(&c, 127 * MIB) == MIB && rdna4dc_heap_available(&c, 128 * MIB) == 0 && rdna4dc_heap_available(&c, fallback.working_set) == 0);
    CHECK("heap usage underflow cannot manufacture capacity", rdna4dc_heap_available(&c, UINT64_MAX) == 0);
    CHECK("heap sizes are page-rounded before the budget comparison", rdna4dc_heap_size(128 * MIB, c.allocation_budget) == 128 * MIB && rdna4dc_heap_size(128 * MIB - 1, c.allocation_budget) == 128 * MIB && rdna4dc_heap_size(128 * MIB + 1, c.allocation_budget) == 0);
    CHECK("heap rounding does not wrap huge requests into a tiny accepted heap", !rdna4dc_heap_size(UINT64_MAX, UINT64_MAX) && !rdna4dc_heap_size(UINT64_MAX - RDNA4DC_HEAP_PAGE + 2, UINT64_MAX));
    CHECK("zero heap sizes are refused; a one-byte request charges one page", !rdna4dc_heap_size(0, UINT64_MAX) && rdna4dc_heap_size(1, RDNA4DC_HEAP_PAGE) == RDNA4DC_HEAP_PAGE);

    p.workgroup_size[0] = p.workgroup_size[1] = p.workgroup_size[2] = UINT32_MAX;
    p.invocations = UINT32_MAX;
    c = rdna4dc_from_properties(&p);
    CHECK("larger backend compute limits do not expand the Metal ceiling", c.invocations == 1024 && c.workgroup_size[0] == 1024);
    CHECK("huge local-size products are rejected without multiplication overflow", !rdna4dc_workgroup_ok(&c, UINT64_MAX, UINT64_MAX, UINT64_MAX) && !rdna4dc_workgroup_ok(&c, 1024, 1024, 1024));

    heaps[0].device_local = heaps[1].device_local = heaps[2].device_local = 0;
    c = rdna4dc_from_properties(&p);
    CHECK("missing local memory never falls back to the 9070 XT VRAM budget", c.buffer_length == 0 && c.working_set == 0 && !rdna4dc_buffer_ok(&c, 1));
    p.heaps = NULL; p.heap_count = 0;
    p.invocations = 0; p.shared_memory = 0;
    c = rdna4dc_from_properties(&p);
    CHECK("incomplete measured properties fail closed for compute and allocation", !rdna4dc_workgroup_ok(&c, 1, 1, 1) && !rdna4dc_heap_size(1, c.working_set) && c.shared_memory == 0);
    char long_name[300]; memset(long_name, 'Q', sizeof long_name); long_name[299] = 0;
    p.name = long_name;
    c = rdna4dc_from_properties(&p);
    CHECK("long physical device names are bounded and terminated", strlen(c.name) == 255 && c.name[255] == 0);
    p.name = "";
    c = rdna4dc_from_properties(&p);
    CHECK("empty physical names use a generic identity", !strcmp(c.name, fallback.name));

    unsigned violations = 0;
    for (uint32_t inv = 1; inv <= 1024; inv *= 2) {
        p.invocations = inv; p.workgroup_size[0] = 127; p.workgroup_size[1] = 63; p.workgroup_size[2] = 31;
        c = rdna4dc_from_properties(&p);
        for (uint64_t x = 1; x <= 128; x++)
            for (uint64_t y = 1; y <= 64; y++)
                for (uint64_t z = 1; z <= 32; z++) {
                    int expected = x <= 127 && y <= 63 && z <= 31 && x * y * z <= inv;
                    if (rdna4dc_workgroup_ok(&c, x, y, z) != expected) violations++;
                }
    }
    CHECK("2,883,584 asymmetric workgroup shapes match independent axis/product bounds", violations == 0);

    // ---- Metal 3 capability defaults, gating, and limit clamping ----
    {
        rdna4dc_metal3_caps zero = rdna4dc_metal3_fallback();
        CHECK("pre-open fallback keeps all Metal 3 capabilities conservatively off",
            memcmp(&fallback.metal3, &zero, sizeof zero) == 0 &&
            memcmp(&cold.metal3, &zero, sizeof zero) == 0 &&
            !rdna4dc_metal3_barycentric_ok(&fallback.metal3) &&
            !rdna4dc_metal3_pull_model_ok(&fallback.metal3) &&
            !rdna4dc_metal3_atomic_float_ok(&fallback.metal3) &&
            !rdna4dc_metal3_atomic_int64_ok(&fallback.metal3) &&
            !rdna4dc_metal3_mesh_supported(&fallback.metal3) &&
            !rdna4dc_metal3_mesh_payload_ok(&fallback.metal3, 0) &&
            !rdna4dc_metal3_mesh_grid_ok(&fallback.metal3, 1, 1, 1) &&
            !rdna4dc_metal3_object_grid_ok(&fallback.metal3, 1, 1, 1) &&
            !rdna4dc_metal3_raytracing_supported(&fallback.metal3) &&
            !rdna4dc_metal3_timestamps_supported(&fallback.metal3) &&
            !rdna4dc_metal3_sparse_buffer_ok(&fallback.metal3) &&
            !rdna4dc_metal3_sparse_texture_ok(&fallback.metal3));
        CHECK("NULL probe and unprobed properties stay conservatively off",
            memcmp(&(rdna4dc_metal3_caps){0}, &(rdna4dc_caps){0}.metal3, sizeof zero) == 0 &&
            !rdna4dc_metal3_mesh_supported(NULL) && !rdna4dc_metal3_raytracing_supported(NULL) &&
            !rdna4dc_metal3_timestamps_supported(NULL) && !rdna4dc_metal3_sparse_texture_ok(NULL) &&
            memcmp(&c.metal3, &zero, sizeof zero) == 0);

        rdna4dc_metal3_probe mp = {
            .api_1_2 = 1, .shader_int64 = 1, .buffer_device_address = 1, .sample_rate_shading = 1,
            .ext_barycentric = 1, .feat_barycentric = 1,
            .ext_atomic_float = 1, .feat_atomic_float_buffer = 1, .feat_atomic_float_buffer_add = 1,
            .feat_atomic_float_shared = 1, .feat_atomic_float_shared_add = 1,
            .feat_atomic_float_image = 1, .feat_atomic_float_image_add = 1,
            .ext_atomic_float2 = 1, .feat_atomic_float_buffer_minmax = 1, .feat_atomic_float_shared_minmax = 1,
            .ext_atomic_int64 = 1, .feat_atomic_int64_buffer = 1, .feat_atomic_int64_shared = 1,
            .ext_mesh_shader = 1, .feat_task_shader = 1, .feat_mesh_shader = 1,
            .mesh_max_payload_bytes = 65536u,
            .object_max_workgroup_count = { 65535u, 65535u, 65535u },
            .object_max_workgroup_total_count = 4194304u, .object_max_workgroup_invocations = 2048u,
            .mesh_max_workgroup_count = { 65535u, 65535u, 65535u },
            .mesh_max_workgroup_total_count = 4194304u, .mesh_max_workgroup_invocations = 2048u,
            .mesh_max_output_vertices = 512u, .mesh_max_output_primitives = 1024u,
            .ext_deferred_host_ops = 1, .ext_accel_struct = 1, .feat_accel_struct = 1,
            .feat_accel_indirect_build = 1, .feat_accel_uab = 1,
            .rt_min_scratch_alignment = 128u, .rt_max_geometries = 1u << 24,
            .rt_max_instances = 1u << 24, .rt_max_primitives = 1u << 29,
            .ext_ray_query = 1, .feat_ray_query = 1,
            .ext_ray_tracing_position_fetch = 1, .feat_ray_tracing_position_fetch = 1,
            .timestamp_query_funcs = 1, .timestamp_compute_and_graphics = 1,
            .timestamp_period_ns = 10.0f, .timestamp_valid_bits = 64u,
            .sparse_queue = 1, .sparse_bind_func = 1,
            .sparse_binding = 1, .sparse_residency_buffer = 1,
            .sparse_residency_image_2d = 1, .sparse_residency_image_3d = 1,
            .sparse_residency_2_samples = 1, .sparse_residency_4_samples = 1,
            .sparse_residency_8_samples = 1, .sparse_residency_aliased = 1
        };
        p.metal3 = &mp;
        rdna4dc_caps m3c = rdna4dc_from_properties(&p);
        const rdna4dc_metal3_caps *m = &m3c.metal3;
        CHECK("probed Metal 3 features gate on cleanly",
            rdna4dc_metal3_barycentric_ok(m) && rdna4dc_metal3_pull_model_ok(m) &&
            rdna4dc_metal3_atomic_float_ok(m) && m->atomic_float_image && m->atomic_float_minmax &&
            rdna4dc_metal3_atomic_int64_ok(m) && rdna4dc_metal3_mesh_supported(m) &&
            rdna4dc_metal3_raytracing_supported(m) && m->accel_indirect_build && m->accel_uab &&
            m->rt_min_scratch_alignment == 128u && rdna4dc_metal3_timestamps_supported(m) &&
            m->timestamp_compute_and_graphics && m->timestamp_valid_bits == 64u &&
            m->timestamp_period_ns == 10.0f && rdna4dc_metal3_sparse_buffer_ok(m) &&
            rdna4dc_metal3_sparse_texture_ok(m) && m->sparse_residency_image_3d && m->sparse_residency_aliased);
        CHECK("oversized mesh payload, invocations, vertices, and primitives clamp to Metal 3 ceilings",
            m->mesh_max_payload_bytes == RDNA4DC_MESH_MAX_PAYLOAD_BYTES &&
            m->object_max_workgroup_invocations == RDNA4DC_MESH_MAX_INVOCATIONS &&
            m->mesh_max_workgroup_invocations == RDNA4DC_MESH_MAX_INVOCATIONS &&
            m->mesh_max_output_vertices == RDNA4DC_MESH_MAX_VERTICES &&
            m->mesh_max_output_primitives == RDNA4DC_MESH_MAX_PRIMITIVES);
        CHECK("mesh payload and grid bounds check exact edges without overflow",
            rdna4dc_metal3_mesh_payload_ok(m, 0) &&
            rdna4dc_metal3_mesh_payload_ok(m, RDNA4DC_MESH_MAX_PAYLOAD_BYTES) &&
            !rdna4dc_metal3_mesh_payload_ok(m, RDNA4DC_MESH_MAX_PAYLOAD_BYTES + 1u) &&
            rdna4dc_metal3_mesh_grid_ok(m, 1024, 1, 1) &&
            rdna4dc_metal3_object_grid_ok(m, 64, 64, 1024) &&
            !rdna4dc_metal3_mesh_grid_ok(m, 0, 1, 1) &&
            !rdna4dc_metal3_mesh_grid_ok(m, 65536, 1, 1) &&
            !rdna4dc_metal3_mesh_grid_ok(m, UINT64_MAX, UINT64_MAX, UINT64_MAX));

        rdna4dc_metal3_probe sub = mp;
        sub.mesh_max_payload_bytes = 8192u;
        rdna4dc_metal3_caps mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("undersized mesh payload is preserved honestly and fails Metal 3 mesh gate",
            mc.mesh_max_payload_bytes == 8192u && !rdna4dc_metal3_mesh_supported(&mc) &&
            rdna4dc_metal3_mesh_payload_ok(&mc, 8192u) && !rdna4dc_metal3_mesh_payload_ok(&mc, 8193u));

        sub = mp; sub.mesh_max_workgroup_total_count = 512u;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("mesh axes clamp to total count and grids < 1024 fail Metal 3 mesh gate",
            mc.mesh_max_workgroup_count[0] == 512u && !rdna4dc_metal3_mesh_supported(&mc) &&
            rdna4dc_metal3_mesh_grid_ok(&mc, 512, 1, 1) && !rdna4dc_metal3_mesh_grid_ok(&mc, 513, 1, 1));

        sub = mp; sub.feat_task_shader = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("missing taskShader zeroes object/payload limits while keeping mesh-only grid",
            mc.mesh_shader && !mc.object_shader && !mc.mesh_max_payload_bytes &&
            !mc.object_max_workgroup_total_count && !rdna4dc_metal3_mesh_supported(&mc) &&
            !rdna4dc_metal3_mesh_payload_ok(&mc, 0) && rdna4dc_metal3_mesh_grid_ok(&mc, 1024, 1, 1));

        sub = mp; sub.ext_mesh_shader = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("missing VK_EXT_mesh_shader zeroes every mesh and object limit",
            !mc.mesh_shader && !mc.object_shader && !mc.mesh_max_payload_bytes &&
            !mc.mesh_max_workgroup_total_count && !mc.mesh_max_output_vertices);

        sub = mp; sub.ext_deferred_host_ops = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("ray tracing fails closed without VK_KHR_deferred_host_operations",
            !mc.deferred_host_ops && !mc.acceleration_structure && !mc.ray_query &&
            !mc.rt_min_scratch_alignment && !rdna4dc_metal3_raytracing_supported(&mc));

        sub = mp; sub.buffer_device_address = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("ray tracing fails closed without bufferDeviceAddress",
            !mc.acceleration_structure && !mc.ray_query && !rdna4dc_metal3_raytracing_supported(&mc));

        sub = mp; sub.feat_accel_struct = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("ray_query fails closed without accelerationStructure",
            !mc.acceleration_structure && !mc.ray_query && !mc.ray_tracing_position_fetch &&
            !rdna4dc_metal3_raytracing_supported(&mc) && !rdna4dc_metal3_rt_position_fetch_supported(&mc));

        CHECK("position_fetch gates on cleanly when RT + ext + feat present",
            m->ray_tracing_position_fetch && rdna4dc_metal3_rt_position_fetch_supported(m));
        sub = mp; sub.feat_ray_query = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("position_fetch fails closed without ray_query",
            !mc.ray_tracing_position_fetch && !rdna4dc_metal3_rt_position_fetch_supported(&mc));
        sub = mp; sub.ext_ray_tracing_position_fetch = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("position_fetch fails closed without extension",
            !mc.ray_tracing_position_fetch && !rdna4dc_metal3_rt_position_fetch_supported(&mc));
        sub = mp; sub.feat_ray_tracing_position_fetch = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("position_fetch fails closed without feature bit",
            !mc.ray_tracing_position_fetch && !rdna4dc_metal3_rt_position_fetch_supported(&mc));

        sub = mp; sub.ext_barycentric = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("barycentric requires both extension and feature", !rdna4dc_metal3_barycentric_ok(&mc));
        sub = mp; sub.feat_atomic_float_buffer_add = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("atomic_float_buffer requires both atomics and atomicAdd", !mc.atomic_float_buffer && !rdna4dc_metal3_atomic_float_ok(&mc));
        sub = mp; sub.ext_atomic_float = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("atomic_float2 minmax fails closed without VK_EXT_shader_atomic_float", !mc.atomic_float_minmax && !rdna4dc_metal3_atomic_float_ok(&mc));
        sub = mp; sub.shader_int64 = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("atomic_int64 fails closed without shaderInt64", !rdna4dc_metal3_atomic_int64_ok(&mc));

        const float bad_periods[] = { 0.0f, -1.0f, 1e9f, (float)NAN, (float)INFINITY };
        int bad_ts_ok = 1;
        for (size_t i = 0; i < sizeof bad_periods / sizeof bad_periods[0]; i++) {
            sub = mp; sub.timestamp_period_ns = bad_periods[i];
            mc = rdna4dc_metal3_from_probe(&sub);
            if (rdna4dc_metal3_timestamps_supported(&mc) || mc.timestamp_valid_bits) bad_ts_ok = 0;
        }
        sub = mp; sub.timestamp_valid_bits = 0u; mc = rdna4dc_metal3_from_probe(&sub);
        bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
        sub = mp; sub.timestamp_valid_bits = 65u; mc = rdna4dc_metal3_from_probe(&sub);
        bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
        sub = mp; sub.timestamp_query_funcs = 0; mc = rdna4dc_metal3_from_probe(&sub);
        bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
        CHECK("invalid timestamp period, validBits, or missing query funcs fail closed", bad_ts_ok);

        sub = mp; sub.sparse_binding = 0;
        mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("sparse residency flags fail closed when sparseBinding is false",
            !mc.sparse_binding && !mc.sparse_residency_buffer && !mc.sparse_residency_image_2d &&
            !mc.sparse_residency_image_3d && !mc.sparse_residency_aliased);
        sub = mp; sub.sparse_queue = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("sparse fails closed without a sparse-capable queue", !mc.sparse_binding && !rdna4dc_metal3_sparse_texture_ok(&mc));
        sub = mp; sub.sparse_bind_func = 0; mc = rdna4dc_metal3_from_probe(&sub);
        CHECK("sparse fails closed without vkQueueBindSparse", !mc.sparse_binding && !rdna4dc_metal3_sparse_buffer_ok(&mc));
        p.metal3 = NULL;
    }

    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
