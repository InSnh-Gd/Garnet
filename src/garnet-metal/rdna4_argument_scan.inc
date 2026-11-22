/*
 * Reflection-to-plan adapter for rdna4_argument_scan.h.
 *
 * This file is intentionally pure C so the mapping can be host-tested. The
 * Device.m bridge only has to flatten reflective binding/field rows into
 * rdna4_argument_scan_reflection_field values.
 *
 * A row with count_known=false means the translator no longer carries the
 * array extent. Its root is scanned conservatively from byte zero. That is a
 * root-local fallback, not a scan of every declared or live resource.
 *
 * Prefer building rows before rdna4_gpu_argument_spv() replaces its metadata
 * with the lowered sidecar, because lowering changes embedded descriptor
 * counts to zero. The lowered form remains safe through the fallback above.
 */

#ifndef RDNA4_ARGUMENT_SCAN_INC
#define RDNA4_ARGUMENT_SCAN_INC

#include "rdna4_argument_scan.h"

typedef struct {
    uint32_t root;
    uint32_t kind;
    uint32_t count_known;
    uint32_t writable;
    uint64_t field_offset;
    uint64_t count;
    uint64_t stride;
} rdna4_argument_scan_reflection_field;

typedef struct {
    rdna4_as_span *storage;
    size_t storage_count;
    rdna4_as_plan plan;
} rdna4_argument_scan_reflection_plan;

static inline void rdna4_argument_scan_reflection_plan_destroy(
    rdna4_argument_scan_reflection_plan *plan) {
    if (!plan) return;
    free(plan->storage);
    memset(plan, 0, sizeof(*plan));
}

static inline rdna4_as_status rdna4_argument_scan_reflection_plan_build(
    rdna4_argument_scan_reflection_plan *out,
    const rdna4_argument_scan_reflection_field *fields, size_t field_count,
    const uint32_t *roots, size_t root_count) {
    if (!out || (!fields && field_count) || (!roots && root_count))
        return RDNA4_AS_INVALID;
    memset(out, 0, sizeof(*out));
    if (field_count > SIZE_MAX - root_count)
        return RDNA4_AS_OVERFLOW;
    size_t capacity = field_count + root_count;
    rdna4_as_span *storage = NULL;
    if (capacity) {
        storage = calloc(capacity, sizeof(*storage));
        if (!storage) return RDNA4_AS_CAPACITY;
    }
    out->storage = storage;
    out->storage_count = capacity;
    if (rdna4_as_plan_init(&out->plan, storage, capacity) != RDNA4_AS_OK) {
        rdna4_argument_scan_reflection_plan_destroy(out);
        return RDNA4_AS_INVALID;
    }

    bool fallback[RDNA4_AS_ROOTS] = {false};
    bool fallback_writable[RDNA4_AS_ROOTS] = {false};
    for (size_t i = 0; i < field_count; ++i) {
        const rdna4_argument_scan_reflection_field *field = &fields[i];
        if (field->root >= RDNA4_AS_ROOTS ||
            !rdna4_as_kind_valid(field->kind)) {
            rdna4_argument_scan_reflection_plan_destroy(out);
            return RDNA4_AS_INVALID;
        }
        if (!field->count_known || !field->count) {
            fallback[field->root] = true;
            fallback_writable[field->root] |=
                field->writable != 0;
        }
    }
    for (size_t i = 0; i < field_count; ++i) {
        const rdna4_argument_scan_reflection_field *field = &fields[i];
        if (fallback[field->root]) continue;
        uint64_t stride = field->stride ?
            field->stride : RDNA4_AS_TOKEN_BYTES;
        rdna4_as_status status = rdna4_as_plan_add_field_array(
            &out->plan, field->root, field->field_offset, field->count,
            stride, field->kind, field->writable != 0);
        if (status != RDNA4_AS_OK) {
            rdna4_argument_scan_reflection_plan_destroy(out);
            return status;
        }
    }

    /*
     * The lowered metadata drops descriptor array counts. Preserve every
     * possible token in that argument root instead of guessing an array end
     * from an adjacent scalar field.
     */
    for (uint32_t root = 0; root < RDNA4_AS_ROOTS; ++root) {
        if (!fallback[root]) continue;
        rdna4_as_status status = rdna4_as_plan_add_fallback_root(
            &out->plan, root, fallback_writable[root]);
        if (status != RDNA4_AS_OK) {
            rdna4_argument_scan_reflection_plan_destroy(out);
            return status;
        }
    }

    /*
     * argument_resource_tables.roots covers texture/sampler roots only. A
     * root with no typed row still has to be scanned conservatively because a
     * dynamic/nested graph can hide every coordinate from the CPU.
     */
    for (size_t i = 0; i < root_count; ++i) {
        uint32_t root = roots[i];
        if (root >= RDNA4_AS_ROOTS) {
            rdna4_argument_scan_reflection_plan_destroy(out);
            return RDNA4_AS_INVALID;
        }
        if (fallback[root]) continue;
        bool has_typed_span = false;
        for (size_t span = 0; span < out->plan.count; ++span)
            if (out->plan.spans[span].root == root) {
                has_typed_span = true;
                break;
            }
        if (has_typed_span) continue;
        rdna4_as_status status = rdna4_as_plan_add_fallback_root(
            &out->plan, root, true);
        if (status != RDNA4_AS_OK) {
            rdna4_argument_scan_reflection_plan_destroy(out);
            return status;
        }
        fallback[root] = true;
    }

    rdna4_as_status status = rdna4_as_plan_finish(&out->plan);
    if (status != RDNA4_AS_OK) {
        rdna4_argument_scan_reflection_plan_destroy(out);
        return status;
    }
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_argument_scan_reflection_plan_materialize(
    const rdna4_argument_scan_reflection_plan *reflected,
    const rdna4_as_bound roots[RDNA4_AS_ROOTS],
    const rdna4_as_transport *transports, size_t transport_count,
    rdna4_as_work *works, size_t capacity, size_t *work_count,
    rdna4_as_work_stats *stats) {
    if (!reflected) return RDNA4_AS_INVALID;
    return rdna4_as_materialize(&reflected->plan, roots, transports,
                                transport_count, works, capacity, work_count,
                                stats);
}

/*
 * Proposed Device.m bridge:
 *
 *   static rdna4_as_bound rdna4_argument_scan_bound(
 *       RDNA4Buffer *buffer, NSUInteger offset, BOOL writable);
 *   static BOOL rdna4_argument_scan_bounds(
 *       rdna4_as_bound out[RDNA4_AS_ROOTS],
 *       const RDNA4StageState *state, NSSet *writable);
 *
 * The first reads [buffer length], [buffer contents], and [buffer
 * storageMode]; the second copies state->buf[root].b/.off into that helper for
 * roots 0..30. The resulting bounds and this plan are passed directly to
 * rdna4_as_materialize.
 */

#endif
