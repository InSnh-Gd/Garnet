// Host build:
//   cc -std=c11 -O2 -Wall -Wextra -Werror -I. test-argument-scan.c -o /tmp/test-argument-scan
//   /tmp/test-argument-scan

#include "garnet_argument_scan.h"
#include "garnet_argument_scan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ARGUMENT_SCAN_FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static rdna4_as_bound shared_root(uint64_t length, uint64_t offset) {
    return (rdna4_as_bound){
        .present = true,
        .mapped = true,
        .writable = true,
        .storage = RDNA4_AS_STORAGE_SHARED,
        .length = length,
        .binding_offset = offset,
        .dirty_epoch = 7,
        .scanned_epoch = 7,
    };
}

typedef struct {
    const uint8_t *bytes;
    uint64_t wanted;
    uint64_t found;
    uint64_t visits;
} token_scan;

typedef struct {
    uint64_t offset[8];
    uint64_t width[8];
    size_t count;
} range_scan;

static bool scan_token(void *context, const rdna4_as_work *work, uint64_t offset,
                       uint64_t width) {
    (void)work;
    token_scan *scan = context;
    CHECK(width == 8);
    uint64_t value = 0;
    memcpy(&value, scan->bytes + offset, sizeof(value));
    scan->visits++;
    if (value == scan->wanted) scan->found = value;
    return true;
}

static bool collect_range(void *context, const rdna4_as_work *work,
                          uint64_t offset, uint64_t width) {
    (void)work;
    range_scan *scan = context;
    CHECK(scan->count < sizeof(scan->offset) / sizeof(scan->offset[0]));
    scan->offset[scan->count] = offset;
    scan->width[scan->count] = width;
    scan->count++;
    return true;
}

static void test_typed_byte_ranges_are_exact(void) {
    rdna4_as_work work = {
        .root = 2,
        .kind = RDNA4_AS_KIND_SAMPLER,
        .flags = RDNA4_AS_WRITABLE,
        .mode = RDNA4_AS_SCAN_CPU_MAPPED,
        .offset = 24,
        .stride = 32,
        .count = 4,
        .width = 8,
    };
    range_scan scan = {0};
    CHECK(rdna4_as_visit_byte_ranges(&work, collect_range, &scan) ==
          RDNA4_AS_OK);
    CHECK(scan.count == 4);
    for (size_t i = 0; i < scan.count; ++i) {
        CHECK(scan.offset[i] == 24 + i * 32);
        CHECK(scan.width[i] == 8);
    }

    memset(&scan, 0, sizeof(scan));
    work.stride = work.width;
    CHECK(rdna4_as_visit_byte_ranges(&work, collect_range, &scan) ==
          RDNA4_AS_OK);
    CHECK(scan.count == 1);
    CHECK(scan.offset[0] == 24 && scan.width[0] == 32);

    memset(&scan, 0, sizeof(scan));
    work.stride = UINT64_MAX;
    work.count = 2;
    CHECK(rdna4_as_visit_byte_ranges(&work, collect_range, &scan) ==
          RDNA4_AS_OVERFLOW);
    CHECK(scan.count == 0);
}

static void test_typed_fields_merge_and_keep_large_offsets(void) {
    rdna4_as_span spans[16];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 16) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 3, 0, 2, RDNA4_AS_TOKEN_BYTES, RDNA4_AS_KIND_TEXTURE,
        true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 3, 16, 1, RDNA4_AS_TOKEN_BYTES, RDNA4_AS_KIND_TEXTURE,
        true) == RDNA4_AS_OK);
    const uint64_t large = (UINT64_C(96) << 20);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 3, large, 1, RDNA4_AS_TOKEN_BYTES, RDNA4_AS_KIND_SAMPLER,
        true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    CHECK(plan.count == 2);
    CHECK(plan.spans[0].kind == RDNA4_AS_KIND_TEXTURE);
    CHECK(plan.spans[0].offset == 0);
    CHECK(plan.spans[0].count == 3);
    CHECK(plan.spans[0].stride == 8);
    CHECK(plan.spans[1].offset == large);
    CHECK(plan.stats.spans_coalesced == 1);

    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[3] = shared_root((UINT64_C(128) << 20), 4096);
    roots[4] = shared_root(UINT64_C(1) << 40, 0);
    rdna4_as_work works[8];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 8, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 2);
    CHECK(works[0].offset == 4096 && works[0].count == 3);
    CHECK(works[1].offset == 4096 + large);
    for (size_t i = 0; i < count; ++i) CHECK(works[i].root == 3);
    CHECK(stats.cpu_works == 2);
    CHECK(stats.cpu_elements == 4);
    CHECK(stats.cpu_touched_bytes == 32);
}

static void test_array_stride_is_not_collapsed(void) {
    rdna4_as_span spans[8];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 8) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 2, 24, 4, 32, RDNA4_AS_KIND_BUFFER, false) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[2] = shared_root(512, 0);
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].offset == 24);
    CHECK(works[0].stride == 32);
    CHECK(works[0].count == 4);
    CHECK(works[0].width == 8);
    CHECK(stats.extent_bytes == 104);
    CHECK(stats.touched_bytes == 32);
}

static void test_shared_and_managed_recheck_every_new_work(void) {
    rdna4_as_span spans[4];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 4) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 1, 8, 1, 8, RDNA4_AS_KIND_SAMPLER, true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);

    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[1] = shared_root(64, 0);
    roots[1].dirty_epoch = 9;
    roots[1].scanned_epoch = 9;
    CHECK(rdna4_as_cpu_scan_eligible(roots[1].storage, roots[1].mapped,
                                     roots[1].dirty_epoch,
                                     roots[1].scanned_epoch));
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    for (unsigned round = 0; round < 2; ++round) {
        size_t count = 0;
        CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                                   &stats) == RDNA4_AS_OK);
        CHECK(count == 1);
        CHECK(works[0].mode == RDNA4_AS_SCAN_CPU_MAPPED);
        CHECK(stats.cpu_works == 1);
    }

    roots[1].storage = RDNA4_AS_STORAGE_MANAGED;
    roots[1].dirty_epoch = 1;
    roots[1].scanned_epoch = 1;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1 && works[0].mode == RDNA4_AS_SCAN_CPU_MAPPED);
}

static void test_private_and_unknown_do_not_use_cpu(void) {
    rdna4_as_span spans[4];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 4) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 5, 0, 1, 8, RDNA4_AS_KIND_TEXTURE, false) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[5] = (rdna4_as_bound){
        .present = true,
        .mapped = false,
        .storage = RDNA4_AS_STORAGE_PRIVATE,
        .length = 4096,
    };
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1 && works[0].mode == RDNA4_AS_SCAN_GPU_SUMMARY);
    CHECK(stats.gpu_works == 1 && stats.cpu_works == 0);
    CHECK(!rdna4_as_cpu_scan_eligible(roots[5].storage, roots[5].mapped, 0, 0));

    roots[5].storage = RDNA4_AS_STORAGE_MEMORYLESS;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1 &&
          works[0].mode == RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE);
    CHECK(stats.opaque_works == 1);
}

static void test_dynamic_unknown_is_bounded_to_its_root(void) {
    rdna4_as_span spans[4];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 4) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_unknown_to_end(&plan, 6, 5, true) ==
          RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[6] = shared_root(101, 0);
    roots[7] = shared_root(UINT64_C(1) << 40, 0);
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].root == 6);
    CHECK(works[0].offset == 8);
    CHECK(works[0].count == 11);
    CHECK(works[0].flags & RDNA4_AS_DYNAMIC);
    CHECK(!(works[0].flags & RDNA4_AS_TO_END));
    CHECK(stats.unknown_works == 1);
}

static void test_invalid_field_falls_back_to_only_that_root(void) {
    rdna4_as_span spans[8];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 8) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 8, 4096, 2, 8, RDNA4_AS_KIND_SAMPLER,
        true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[8] = shared_root(64, 0);
    roots[9] = shared_root(UINT64_C(1) << 40, 0);
    rdna4_as_work works[4];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 4, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].root == 8);
    CHECK(works[0].flags & RDNA4_AS_FALLBACK);
    CHECK(works[0].offset == 0);
    CHECK(works[0].count == 8);
    CHECK(stats.invalid_spans == 1);
    CHECK(stats.fallback_works == 1);
    CHECK(stats.cpu_elements == 8);
}

static void test_overflow_is_rejected_atomically(void) {
    rdna4_as_span spans[4];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 4) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, UINT64_MAX - 4, 2, 8, RDNA4_AS_KIND_TEXTURE,
        false) == RDNA4_AS_OVERFLOW);
    CHECK(plan.count == 0);
    CHECK(plan.stats.overflow_rejections == 1);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, 0, UINT64_MAX, UINT64_MAX, RDNA4_AS_KIND_TEXTURE,
        false) == RDNA4_AS_OVERFLOW);
    CHECK(plan.count == 0);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
}

static void test_capacity_failure_never_truncates(void) {
    rdna4_as_span spans[2];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 1) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, 0, 1, 8, RDNA4_AS_KIND_TEXTURE,
        true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, 8, 1, 8, RDNA4_AS_KIND_TEXTURE,
        true) == RDNA4_AS_CAPACITY);
    CHECK(plan.count == 1);
    CHECK(plan.stats.capacity_rejections == 1);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);

    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[0] = shared_root(64, 0);
    rdna4_as_work work[1];
    rdna4_as_work_stats stats;
    size_t count = 99;
    size_t needed = 0;
    CHECK(rdna4_as_work_capacity(&plan, 1, &needed) == RDNA4_AS_OK);
    CHECK(needed == 2);
    rdna4_as_transport extra = { .root = 0, .offset = 16, .length = 16 };
    CHECK(rdna4_as_materialize(&plan, roots, &extra, 1, work, 1, &count,
                               &stats) == RDNA4_AS_CAPACITY);
    CHECK(count == 0);
}

static void test_transport_is_explicit_not_whole_buffer(void) {
    rdna4_as_span spans[2];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 2) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 1, 0, 1, 8, RDNA4_AS_KIND_SAMPLER, false) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[1] = shared_root(UINT64_C(256) << 20, 32);
    roots[2] = shared_root(UINT64_C(256) << 20, 0);
    rdna4_as_transport transport = {
        .root = 1,
        .flags = RDNA4_AS_WRITABLE,
        .offset = (UINT64_C(128) << 20) + 24,
        .length = 64,
    };
    rdna4_as_work works[4];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, &transport, 1, works, 4, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 2);
    CHECK(works[0].root == 1 && works[0].kind == RDNA4_AS_KIND_SAMPLER);
    CHECK(works[1].root == 1 && works[1].flags & RDNA4_AS_TRANSPORT);
    CHECK(works[1].offset == 32 + (UINT64_C(128) << 20) + 24);
    CHECK(works[1].stride == 8);
    CHECK(works[1].count == 8);
    CHECK(works[1].width == 8);
    CHECK(stats.transport_works == 1);
    CHECK(stats.cpu_extent_bytes == 72);
    for (size_t i = 0; i < count; ++i) CHECK(works[i].root != 2);

    transport.offset++;
    CHECK(rdna4_as_materialize(&plan, roots, &transport, 1, works, 4, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 2);
    CHECK(works[1].root == 1);
    CHECK(works[1].flags & RDNA4_AS_FALLBACK);
    CHECK(stats.invalid_spans == 1);
}

static void test_missing_root_and_empty_plan(void) {
    rdna4_as_span spans[2];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 2) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 4, 0, 1, 8, RDNA4_AS_KIND_TEXTURE, true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[3] = shared_root(64, 0);
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 99;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 0);
    CHECK(stats.missing_roots == 1);

    rdna4_as_plan empty;
    CHECK(rdna4_as_plan_init(&empty, NULL, 0) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&empty) == RDNA4_AS_OK);
    CHECK(rdna4_as_materialize(&empty, roots, NULL, 0, NULL, 0, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 0);
}

static void test_binding_offset_overflow_falls_back(void) {
    rdna4_as_span spans[2];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 2) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, 8, 1, 8, RDNA4_AS_KIND_TEXTURE, true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[0] = shared_root(64, UINT64_MAX - 4);
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].flags & RDNA4_AS_FALLBACK);
    CHECK(works[0].offset == 0);
    CHECK(stats.invalid_spans == 1);
}

static void test_fallback_uses_binding_absolute_range(void) {
    rdna4_as_span spans[1];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 1) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, 96, 1, 8, RDNA4_AS_KIND_TEXTURE, true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);

    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[0] = shared_root(104, 28);
    rdna4_as_work works[1];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 1, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].flags & RDNA4_AS_FALLBACK);
    CHECK(works[0].offset == 32);
    CHECK(works[0].count == 9);
    CHECK(stats.cpu_extent_bytes == 72);

    roots[0].binding_offset = 120;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 1, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].flags & RDNA4_AS_FALLBACK);
    CHECK(works[0].offset == 0);
    CHECK(works[0].count == 13);
}

static void test_reference_tables_without_64m_limit(void) {
    rdna4_as_span spans[4];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 4) == RDNA4_AS_OK);
    const uint64_t offset = (UINT64_C(2) << 30) + 24;
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 3, offset, 2, 8, RDNA4_AS_KIND_SAMPLER,
        true) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[3] = shared_root((UINT64_C(4) << 30), 64);
    rdna4_as_work works[2];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 2, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].offset == offset + 64);
    CHECK(works[0].count == 2);
    CHECK(!(works[0].flags & RDNA4_AS_FALLBACK));
}

static void test_over_64m_mapped_reference_is_found(void) {
    const uint64_t old_limit = UINT64_C(64) << 20;
    const uint64_t length = old_limit + (UINT64_C(1) << 20);
    uint8_t *bytes = calloc(1, (size_t)length);
    CHECK(bytes != NULL);
    const uint64_t offset = old_limit + 4096;
    const uint64_t token = UINT64_C(0x5244000000001234);
    memcpy(bytes + offset, &token, sizeof(token));

    rdna4_as_span spans[2];
    rdna4_as_plan plan;
    CHECK(rdna4_as_plan_init(&plan, spans, 2) == RDNA4_AS_OK);
    CHECK(rdna4_as_plan_add_field_array(
        &plan, 0, offset, 1, 8, RDNA4_AS_KIND_SAMPLER, false) ==
        RDNA4_AS_OK);
    CHECK(rdna4_as_plan_finish(&plan) == RDNA4_AS_OK);
    rdna4_as_bound roots[RDNA4_AS_ROOTS] = {0};
    roots[0] = shared_root(length, 0);
    rdna4_as_work works[1];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_as_materialize(&plan, roots, NULL, 0, works, 1, &count,
                               &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    token_scan scan = {
        .bytes = bytes,
        .wanted = token,
    };
    CHECK(rdna4_as_visit_tokens(&works[0], scan_token, &scan) == RDNA4_AS_OK);
    CHECK(scan.found == token);
    CHECK(scan.visits == 1);
    CHECK(stats.cpu_touched_bytes == 8);
    free(bytes);
}

static void test_reflection_adapter_and_root_local_fallback(void) {
    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 3,
            .kind = RDNA4_AS_KIND_TEXTURE,
            .count_known = true,
            .writable = true,
            .field_offset = 0,
            .count = 2,
            .stride = 8,
        },
        {
            .root = 3,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = true,
            .writable = true,
            .field_offset = 16,
            .count = 2,
            .stride = 8,
        },
        {
            .root = 4,
            .kind = RDNA4_AS_KIND_UNKNOWN,
            .count_known = false,
            .writable = true,
            .field_offset = 64,
        },
    };
    const uint32_t roots[] = {3, 4, 5};
    rdna4_argument_scan_reflection_plan reflected;
    CHECK(rdna4_argument_scan_reflection_plan_build(
        &reflected, fields, sizeof(fields) / sizeof(fields[0]),
        roots, sizeof(roots) / sizeof(roots[0])) == RDNA4_AS_OK);
    CHECK(reflected.plan.count == 4);

    rdna4_as_bound bounds[RDNA4_AS_ROOTS] = {0};
    bounds[3] = shared_root(128, 0);
    bounds[4] = shared_root(80, 0);
    bounds[5] = shared_root(48, 0);
    bounds[6] = shared_root(UINT64_C(1) << 40, 0);
    rdna4_as_work works[8];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_argument_scan_reflection_plan_materialize(
        &reflected, bounds, NULL, 0, works, 8, &count,
        &stats) == RDNA4_AS_OK);
    CHECK(count == 4);
    CHECK(works[0].root == 3 && works[0].kind == RDNA4_AS_KIND_TEXTURE);
    CHECK(works[1].root == 3 && works[1].kind == RDNA4_AS_KIND_SAMPLER);
    CHECK(works[2].root == 4 && works[2].flags & RDNA4_AS_FALLBACK);
    CHECK(works[2].count == 10);
    CHECK(works[3].root == 5 && works[3].flags & RDNA4_AS_FALLBACK);
    CHECK(works[3].count == 6);
    for (size_t i = 0; i < count; ++i) CHECK(works[i].root != 6);
    rdna4_argument_scan_reflection_plan_destroy(&reflected);
    CHECK(reflected.storage == NULL && reflected.plan.spans == NULL);

    rdna4_argument_scan_reflection_plan invalid;
    const rdna4_argument_scan_reflection_field bad = {
        .root = RDNA4_AS_ROOTS,
        .kind = RDNA4_AS_KIND_TEXTURE,
        .count_known = true,
        .count = 1,
    };
    CHECK(rdna4_argument_scan_reflection_plan_build(
        &invalid, &bad, 1, NULL, 0) == RDNA4_AS_INVALID);
}

static void test_unknown_array_count_supersedes_narrow_rows(void) {
    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 2,
            .kind = RDNA4_AS_KIND_TEXTURE,
            .count_known = true,
            .writable = true,
            .field_offset = 0,
            .count = 1,
            .stride = 8,
        },
        {
            .root = 2,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = false,
            .writable = true,
            .field_offset = 64,
        },
    };
    const uint32_t root = 2;
    rdna4_argument_scan_reflection_plan reflected;
    CHECK(rdna4_argument_scan_reflection_plan_build(
        &reflected, fields, 2, &root, 1) == RDNA4_AS_OK);
    CHECK(reflected.plan.count == 1);
    CHECK(reflected.plan.spans[0].flags & RDNA4_AS_FALLBACK);
    rdna4_as_bound bounds[RDNA4_AS_ROOTS] = {0};
    bounds[2] = shared_root(80, 0);
    rdna4_as_work works[1];
    rdna4_as_work_stats stats;
    size_t count = 0;
    CHECK(rdna4_argument_scan_reflection_plan_materialize(
        &reflected, bounds, NULL, 0, works, 1, &count,
        &stats) == RDNA4_AS_OK);
    CHECK(count == 1);
    CHECK(works[0].flags & RDNA4_AS_FALLBACK);
    CHECK(works[0].count == 10);
    rdna4_argument_scan_reflection_plan_destroy(&reflected);
}

int main(void) {
    test_typed_byte_ranges_are_exact();
    test_typed_fields_merge_and_keep_large_offsets();
    test_array_stride_is_not_collapsed();
    test_shared_and_managed_recheck_every_new_work();
    test_private_and_unknown_do_not_use_cpu();
    test_dynamic_unknown_is_bounded_to_its_root();
    test_invalid_field_falls_back_to_only_that_root();
    test_overflow_is_rejected_atomically();
    test_capacity_failure_never_truncates();
    test_transport_is_explicit_not_whole_buffer();
    test_missing_root_and_empty_plan();
    test_binding_offset_overflow_falls_back();
    test_fallback_uses_binding_absolute_range();
    test_reference_tables_without_64m_limit();
    test_over_64m_mapped_reference_is_found();
    test_reflection_adapter_and_root_local_fallback();
    test_unknown_array_count_supersedes_narrow_rows();
    puts("ARGUMENT_SCAN_PASS");
    return 0;
}
