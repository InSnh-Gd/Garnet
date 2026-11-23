// cc -std=c11 -O2 -Wall -Wextra -Werror test-devcaps.c -o /tmp/test-devcaps && /tmp/test-devcaps
#include <stdio.h>
#include <string.h>
#include "rdna4_devcaps.h"

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
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
