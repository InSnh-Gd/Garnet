#ifndef RDNA4_ARGUMENT_SCAN_H
#define RDNA4_ARGUMENT_SCAN_H

/*
 * Bounded CPU scan planning for Metal argument buffers.
 *
 * A plan is built once from reflection. At draw/dispatch time it is resolved
 * against the currently bound roots. A missing, malformed, or out-of-range
 * typed field expands to that root only; it never expands to every live
 * resource on the device.
 *
 * All arithmetic is 64-bit and checked. There is no legacy 64 MiB scan limit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4_AS_ROOTS 31u
#define RDNA4_AS_TOKEN_BYTES 8u

typedef enum {
    RDNA4_AS_OK = 0,
    RDNA4_AS_INVALID,
    RDNA4_AS_CAPACITY,
    RDNA4_AS_OVERFLOW,
    RDNA4_AS_NOT_FINALIZED,
} rdna4_as_status;

typedef enum {
    RDNA4_AS_KIND_UNKNOWN = 0,
    RDNA4_AS_KIND_BUFFER,
    RDNA4_AS_KIND_TEXTURE,
    RDNA4_AS_KIND_SAMPLER,
} rdna4_as_kind;

enum {
    RDNA4_AS_WRITABLE = 1u << 0,
    RDNA4_AS_DYNAMIC = 1u << 1,
    RDNA4_AS_TO_END = 1u << 2,
    RDNA4_AS_TRANSPORT = 1u << 3,
    RDNA4_AS_FALLBACK = 1u << 4,
};

typedef enum {
    RDNA4_AS_STORAGE_SHARED = 0,
    RDNA4_AS_STORAGE_MANAGED = 1,
    RDNA4_AS_STORAGE_PRIVATE = 2,
    RDNA4_AS_STORAGE_MEMORYLESS = 3,
    RDNA4_AS_STORAGE_UNKNOWN = UINT8_MAX,
} rdna4_as_storage;

typedef enum {
    RDNA4_AS_SCAN_NONE = 0,
    RDNA4_AS_SCAN_CPU_MAPPED,
    RDNA4_AS_SCAN_GPU_SUMMARY,
    RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE,
} rdna4_as_scan_mode;

typedef struct {
    uint32_t root;
    uint32_t kind;
    uint32_t flags;
    uint64_t offset;
    uint64_t stride;
    uint64_t count;
    uint64_t width;
} rdna4_as_span;

typedef struct {
    uint64_t fields_seen;
    uint64_t elements_seen;
    uint64_t spans_added;
    uint64_t spans_coalesced;
    uint64_t overflow_rejections;
    uint64_t capacity_rejections;
    bool saturated;
} rdna4_as_build_stats;

typedef struct {
    rdna4_as_span *spans;
    size_t capacity;
    size_t count;
    bool finalized;
    rdna4_as_build_stats stats;
} rdna4_as_plan;

typedef struct {
    bool present;
    bool mapped;
    bool writable;
    uint8_t storage;
    uint64_t length;
    uint64_t binding_offset;
    /*
     * Visibility epochs are bookkeeping only. Shared, Managed, and unknown
     * coherent mappings are rechecked on every new work item. Equal epochs do
     * not permit skipping a direct CPU write made through -contents.
     */
    uint64_t dirty_epoch;
    uint64_t scanned_epoch;
} rdna4_as_bound;

typedef struct {
    uint32_t root;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
} rdna4_as_transport;

typedef struct {
    /*
     * Absolute byte offsets in state->buf[root].b. Visit count words at
     * offset + i * stride, each width bytes. CPU_MAPPED is the only mode an
     * event/completion path may read; Private stays for the GPU summary and
     * opaque storage remains conservatively owned.
     */
    uint32_t root;
    uint32_t kind;
    uint32_t flags;
    uint8_t mode;
    uint64_t offset;
    uint64_t stride;
    uint64_t count;
    uint64_t width;
} rdna4_as_work;

typedef struct {
    uint64_t works;
    uint64_t cpu_works;
    uint64_t gpu_works;
    uint64_t opaque_works;
    uint64_t elements;
    uint64_t touched_bytes;
    uint64_t extent_bytes;
    uint64_t cpu_elements;
    uint64_t cpu_touched_bytes;
    uint64_t cpu_extent_bytes;
    uint64_t gpu_elements;
    uint64_t gpu_extent_bytes;
    uint64_t opaque_elements;
    uint64_t opaque_extent_bytes;
    uint64_t fallback_works;
    uint64_t transport_works;
    uint64_t unknown_works;
    uint64_t missing_roots;
    uint64_t invalid_spans;
    uint64_t capacity_rejections;
    bool saturated;
} rdna4_as_work_stats;

typedef bool (*rdna4_as_token_visitor)(void *context,
                                       const rdna4_as_work *work,
                                       uint64_t offset, uint64_t width);

typedef bool (*rdna4_as_byte_range_visitor)(void *context,
                                            const rdna4_as_work *work,
                                            uint64_t offset, uint64_t width);

static inline uint64_t rdna4_as_sat_add(uint64_t a, uint64_t b,
                                         bool *saturated) {
    if (b > UINT64_MAX - a) {
        if (saturated) *saturated = true;
        return UINT64_MAX;
    }
    return a + b;
}

static inline uint64_t rdna4_as_sat_mul(uint64_t a, uint64_t b,
                                         bool *saturated) {
    if (a && b > UINT64_MAX / a) {
        if (saturated) *saturated = true;
        return UINT64_MAX;
    }
    return a * b;
}

static inline bool rdna4_as_add_overflows(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a;
}

static inline bool rdna4_as_mul_overflows(uint64_t a, uint64_t b) {
    return a && b > UINT64_MAX / a;
}

static inline bool rdna4_as_range_valid(uint64_t offset, uint64_t length,
                                        uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

static inline bool rdna4_as_span_extent(const rdna4_as_span *span,
                                        uint64_t *extent) {
    if (!span || !span->width || !span->count || span->flags & RDNA4_AS_TO_END)
        return false;
    if (span->count > 1 &&
        (span->stride < span->width ||
         rdna4_as_mul_overflows(span->count - 1, span->stride)))
        return false;
    uint64_t tail = span->count > 1 ? (span->count - 1) * span->stride : 0;
    if (rdna4_as_add_overflows(tail, span->width)) return false;
    *extent = tail + span->width;
    return true;
}

static inline bool rdna4_as_kind_valid(uint32_t kind) {
    return kind <= RDNA4_AS_KIND_SAMPLER;
}

static inline bool rdna4_as_flags_valid(uint32_t flags) {
    const uint32_t all = RDNA4_AS_WRITABLE | RDNA4_AS_DYNAMIC |
        RDNA4_AS_TO_END | RDNA4_AS_TRANSPORT | RDNA4_AS_FALLBACK;
    return !(flags & ~all);
}

static inline rdna4_as_status rdna4_as_plan_init(rdna4_as_plan *plan,
                                                 rdna4_as_span *spans,
                                                 size_t capacity) {
    if (!plan || (!spans && capacity)) return RDNA4_AS_INVALID;
    memset(plan, 0, sizeof(*plan));
    plan->spans = spans;
    plan->capacity = capacity;
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_as_plan_add(rdna4_as_plan *plan,
                                                rdna4_as_span span) {
    if (!plan || plan->finalized || span.root >= RDNA4_AS_ROOTS ||
        !rdna4_as_kind_valid(span.kind) || !rdna4_as_flags_valid(span.flags) ||
        !span.width)
        return RDNA4_AS_INVALID;
    if (span.flags & RDNA4_AS_TO_END) {
        if (span.count) return RDNA4_AS_INVALID;
    } else {
        uint64_t extent = 0;
        if (!rdna4_as_span_extent(&span, &extent) ||
            rdna4_as_add_overflows(span.offset, extent)) {
            plan->stats.overflow_rejections =
                rdna4_as_sat_add(plan->stats.overflow_rejections, 1,
                                 &plan->stats.saturated);
            return RDNA4_AS_OVERFLOW;
        }
    }
    if (plan->count == plan->capacity) {
        plan->stats.capacity_rejections =
            rdna4_as_sat_add(plan->stats.capacity_rejections, 1,
                             &plan->stats.saturated);
        return RDNA4_AS_CAPACITY;
    }
    plan->spans[plan->count++] = span;
    plan->stats.fields_seen =
        rdna4_as_sat_add(plan->stats.fields_seen, 1,
                         &plan->stats.saturated);
    plan->stats.elements_seen =
        rdna4_as_sat_add(plan->stats.elements_seen, span.count,
                         &plan->stats.saturated);
    plan->stats.spans_added =
        rdna4_as_sat_add(plan->stats.spans_added, 1,
                         &plan->stats.saturated);
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_as_plan_add_field_array(
    rdna4_as_plan *plan, uint32_t root, uint64_t offset, uint64_t count,
    uint64_t stride, uint32_t kind, bool writable) {
    if (!count) return RDNA4_AS_INVALID;
    if (count == 1) stride = RDNA4_AS_TOKEN_BYTES;
    return rdna4_as_plan_add(plan, (rdna4_as_span){
        .root = root,
        .kind = kind,
        .flags = writable ? RDNA4_AS_WRITABLE : 0,
        .offset = offset,
        .stride = stride,
        .count = count,
        .width = RDNA4_AS_TOKEN_BYTES,
    });
}

static inline rdna4_as_status rdna4_as_plan_add_unknown_to_end(
    rdna4_as_plan *plan, uint32_t root, uint64_t offset, bool writable) {
    return rdna4_as_plan_add(plan, (rdna4_as_span){
        .root = root,
        .kind = RDNA4_AS_KIND_UNKNOWN,
        .flags = RDNA4_AS_DYNAMIC | RDNA4_AS_TO_END |
            (writable ? RDNA4_AS_WRITABLE : 0),
        .offset = offset,
        .stride = RDNA4_AS_TOKEN_BYTES,
        .width = RDNA4_AS_TOKEN_BYTES,
    });
}

static inline rdna4_as_status rdna4_as_plan_add_fallback_root(
    rdna4_as_plan *plan, uint32_t root, bool writable) {
    return rdna4_as_plan_add(plan, (rdna4_as_span){
        .root = root,
        .kind = RDNA4_AS_KIND_UNKNOWN,
        .flags = RDNA4_AS_DYNAMIC | RDNA4_AS_TO_END |
            RDNA4_AS_FALLBACK | (writable ? RDNA4_AS_WRITABLE : 0),
        .offset = 0,
        .stride = RDNA4_AS_TOKEN_BYTES,
        .width = RDNA4_AS_TOKEN_BYTES,
    });
}

static inline int rdna4_as_cmp_u64(uint64_t a, uint64_t b) {
    return a < b ? -1 : a > b ? 1 : 0;
}

static inline int rdna4_as_span_compare(const void *left, const void *right) {
    const rdna4_as_span *a = left;
    const rdna4_as_span *b = right;
    int result = (int)a->root - (int)b->root;
    if (result) return result;
    result = (int)a->flags - (int)b->flags;
    if (result) return result;
    result = (int)a->kind - (int)b->kind;
    if (result) return result;
    result = rdna4_as_cmp_u64(a->offset, b->offset);
    if (result) return result;
    result = rdna4_as_cmp_u64(a->stride, b->stride);
    if (result) return result;
    result = rdna4_as_cmp_u64(a->width, b->width);
    if (result) return result;
    return rdna4_as_cmp_u64(a->count, b->count);
}

static inline rdna4_as_status rdna4_as_plan_finish(rdna4_as_plan *plan) {
    if (!plan) return RDNA4_AS_INVALID;
    if (plan->finalized) return RDNA4_AS_OK;
    if (!plan->spans) {
        if (plan->count) return RDNA4_AS_INVALID;
        plan->finalized = true;
        return RDNA4_AS_OK;
    }
    qsort(plan->spans, plan->count, sizeof(*plan->spans),
          rdna4_as_span_compare);
    size_t out = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        rdna4_as_span next = plan->spans[i];
        if (out) {
            rdna4_as_span *prev = &plan->spans[out - 1];
            bool same = prev->root == next.root && prev->kind == next.kind &&
                prev->flags == next.flags && prev->stride == next.stride &&
                prev->width == next.width &&
                !(prev->flags & RDNA4_AS_TO_END) &&
                !(next.flags & RDNA4_AS_TO_END);
            if (same && prev->offset == next.offset) {
                if (next.count > prev->count) prev->count = next.count;
                plan->stats.spans_coalesced =
                    rdna4_as_sat_add(plan->stats.spans_coalesced, 1,
                                     &plan->stats.saturated);
                continue;
            }
            if (same && !rdna4_as_mul_overflows(prev->count, prev->stride) &&
                !rdna4_as_add_overflows(prev->offset,
                                        prev->count * prev->stride) &&
                prev->offset + prev->count * prev->stride == next.offset &&
                !rdna4_as_add_overflows(prev->count, next.count)) {
                prev->count += next.count;
                plan->stats.spans_coalesced =
                    rdna4_as_sat_add(plan->stats.spans_coalesced, 1,
                                     &plan->stats.saturated);
                continue;
            }
        }
        plan->spans[out++] = next;
    }
    plan->count = out;
    plan->finalized = true;
    return RDNA4_AS_OK;
}

static inline rdna4_as_scan_mode rdna4_as_scan_mode_for(
    const rdna4_as_bound *bound) {
    if (!bound || !bound->present) return RDNA4_AS_SCAN_NONE;
    if (bound->mapped &&
        (bound->storage == RDNA4_AS_STORAGE_SHARED ||
         bound->storage == RDNA4_AS_STORAGE_MANAGED ||
         bound->storage == RDNA4_AS_STORAGE_UNKNOWN))
        return RDNA4_AS_SCAN_CPU_MAPPED;
    if (bound->storage == RDNA4_AS_STORAGE_PRIVATE)
        return RDNA4_AS_SCAN_GPU_SUMMARY;
    return RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE;
}

static inline bool rdna4_as_cpu_scan_eligible(uint8_t storage, bool mapped,
                                               uint64_t dirty_epoch,
                                               uint64_t scanned_epoch) {
    (void)dirty_epoch;
    (void)scanned_epoch;
    if (!mapped) return false;
    /*
     * The native backend's Shared and Managed memory is host coherent. A
     * direct -contents store need not call a setter, so neither an unchanged
     * dirty epoch nor a prior scan may suppress the check for new work.
     */
    return storage == RDNA4_AS_STORAGE_SHARED ||
        storage == RDNA4_AS_STORAGE_MANAGED ||
        storage == RDNA4_AS_STORAGE_UNKNOWN;
}

static inline rdna4_as_status rdna4_as_work_extent(const rdna4_as_work *work,
                                                    uint64_t *extent) {
    if (!work || !work->width || !work->count) return RDNA4_AS_INVALID;
    if (work->count > 1 &&
        (work->stride < work->width ||
         rdna4_as_mul_overflows(work->count - 1, work->stride)))
        return RDNA4_AS_OVERFLOW;
    uint64_t tail = work->count > 1 ? (work->count - 1) * work->stride : 0;
    if (rdna4_as_add_overflows(tail, work->width)) return RDNA4_AS_OVERFLOW;
    *extent = tail + work->width;
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_as_visit_tokens(
    const rdna4_as_work *work, rdna4_as_token_visitor visitor, void *context) {
    if (!work || !visitor || !work->count || !work->width)
        return RDNA4_AS_INVALID;
    uint64_t offset = work->offset;
    for (uint64_t i = 0; i < work->count; ++i) {
        if (!visitor(context, work, offset, work->width)) break;
        if (i + 1 < work->count) {
            if (rdna4_as_add_overflows(offset, work->stride))
                return RDNA4_AS_OVERFLOW;
            offset += work->stride;
        }
    }
    return RDNA4_AS_OK;
}

/*
 * Visit the exact byte ranges covered by a work item. A dense work item is one
 * range; a strided work item is one range per element, so padding bytes between
 * typed array elements are never reported as accessed or written.
 */
static inline rdna4_as_status rdna4_as_visit_byte_ranges(
    const rdna4_as_work *work, rdna4_as_byte_range_visitor visitor,
    void *context) {
    if (!work || !visitor || !work->count || !work->width)
        return RDNA4_AS_INVALID;
    uint64_t extent = 0;
    rdna4_as_status status = rdna4_as_work_extent(work, &extent);
    if (status != RDNA4_AS_OK) return status;
    if (work->stride == work->width) {
        (void)visitor(context, work, work->offset, extent);
        return RDNA4_AS_OK;
    }
    uint64_t offset = work->offset;
    for (uint64_t i = 0; i < work->count; ++i) {
        if (!visitor(context, work, offset, work->width)) break;
        if (i + 1 < work->count) {
            if (rdna4_as_add_overflows(offset, work->stride))
                return RDNA4_AS_OVERFLOW;
            offset += work->stride;
        }
    }
    return RDNA4_AS_OK;
}

static inline void rdna4_as_work_stats_add(rdna4_as_work_stats *stats,
                                           const rdna4_as_work *work) {
    if (!stats || !work) return;
    uint64_t extent = 0;
    if (rdna4_as_work_extent(work, &extent) != RDNA4_AS_OK) {
        stats->saturated = true;
        return;
    }
    uint64_t touched = rdna4_as_sat_mul(work->count, work->width,
                                        &stats->saturated);
    stats->works = rdna4_as_sat_add(stats->works, 1, &stats->saturated);
    stats->elements = rdna4_as_sat_add(stats->elements, work->count,
                                       &stats->saturated);
    stats->touched_bytes = rdna4_as_sat_add(stats->touched_bytes, touched,
                                            &stats->saturated);
    stats->extent_bytes = rdna4_as_sat_add(stats->extent_bytes, extent,
                                           &stats->saturated);
    if (work->mode == RDNA4_AS_SCAN_CPU_MAPPED) {
        stats->cpu_works = rdna4_as_sat_add(stats->cpu_works, 1,
                                            &stats->saturated);
        stats->cpu_elements = rdna4_as_sat_add(stats->cpu_elements,
                                               work->count,
                                               &stats->saturated);
        stats->cpu_touched_bytes = rdna4_as_sat_add(
            stats->cpu_touched_bytes, touched, &stats->saturated);
        stats->cpu_extent_bytes = rdna4_as_sat_add(
            stats->cpu_extent_bytes, extent, &stats->saturated);
    } else if (work->mode == RDNA4_AS_SCAN_GPU_SUMMARY) {
        stats->gpu_works = rdna4_as_sat_add(stats->gpu_works, 1,
                                            &stats->saturated);
        stats->gpu_elements = rdna4_as_sat_add(stats->gpu_elements,
                                               work->count,
                                               &stats->saturated);
        stats->gpu_extent_bytes = rdna4_as_sat_add(
            stats->gpu_extent_bytes, extent, &stats->saturated);
    } else if (work->mode == RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE) {
        stats->opaque_works = rdna4_as_sat_add(stats->opaque_works, 1,
                                               &stats->saturated);
        stats->opaque_elements = rdna4_as_sat_add(
            stats->opaque_elements, work->count, &stats->saturated);
        stats->opaque_extent_bytes = rdna4_as_sat_add(
            stats->opaque_extent_bytes, extent, &stats->saturated);
    }
    if (work->flags & RDNA4_AS_FALLBACK)
        stats->fallback_works = rdna4_as_sat_add(
            stats->fallback_works, 1, &stats->saturated);
    if (work->flags & RDNA4_AS_TRANSPORT)
        stats->transport_works = rdna4_as_sat_add(
            stats->transport_works, 1, &stats->saturated);
    if (work->kind == RDNA4_AS_KIND_UNKNOWN)
        stats->unknown_works = rdna4_as_sat_add(
            stats->unknown_works, 1, &stats->saturated);
}

static inline bool rdna4_as_work_same(const rdna4_as_work *a,
                                      const rdna4_as_work *b) {
    return a->root == b->root && a->kind == b->kind &&
        a->flags == b->flags && a->mode == b->mode &&
        a->stride == b->stride && a->width == b->width &&
        !rdna4_as_mul_overflows(a->count, a->stride) &&
        !rdna4_as_add_overflows(a->offset, a->count * a->stride) &&
        a->offset + a->count * a->stride == b->offset;
}

static inline rdna4_as_status rdna4_as_append_work(
    rdna4_as_work *works, size_t capacity, size_t *count,
    rdna4_as_work_stats *stats, const rdna4_as_work *work) {
    if (!works || !count || !stats || !work || *count >= capacity)
        return RDNA4_AS_CAPACITY;
    if (*count && rdna4_as_work_same(&works[*count - 1], work) &&
        !rdna4_as_add_overflows(works[*count - 1].count, work->count)) {
        works[*count - 1].count += work->count;
        rdna4_as_work_stats_add(stats, work);
        return RDNA4_AS_OK;
    }
    works[(*count)++] = *work;
    rdna4_as_work_stats_add(stats, work);
    return RDNA4_AS_OK;
}

static inline bool rdna4_as_make_absolute(const rdna4_as_bound *bound,
                                          uint64_t relative,
                                          uint64_t *absolute) {
    if (!bound || !bound->present || !absolute ||
        rdna4_as_add_overflows(bound->binding_offset, relative))
        return false;
    *absolute = bound->binding_offset + relative;
    return true;
}

static inline rdna4_as_status rdna4_as_append_fallback(
    const rdna4_as_bound *bound, uint32_t root, uint32_t kind, uint32_t flags,
    rdna4_as_work *works, size_t capacity, size_t *count,
    rdna4_as_work_stats *stats) {
    if (!bound || !bound->present) return RDNA4_AS_OK;
    /*
     * A valid bound starts at its buffer-relative binding offset. If that
     * base is itself out of range, the only conservative resource-local range
     * still available is the whole buffer.
     */
    uint64_t offset =
        bound->binding_offset <= bound->length ? bound->binding_offset : 0;
    uint64_t remainder = offset % RDNA4_AS_TOKEN_BYTES;
    if (remainder) {
        uint64_t align = RDNA4_AS_TOKEN_BYTES - remainder;
        if (align > bound->length - offset) return RDNA4_AS_OK;
        offset += align;
    }
    uint64_t words =
        (bound->length - offset) / RDNA4_AS_TOKEN_BYTES;
    if (!words) return RDNA4_AS_OK;
    rdna4_as_scan_mode mode = rdna4_as_scan_mode_for(bound);
    if (mode == RDNA4_AS_SCAN_NONE) return RDNA4_AS_OK;
    rdna4_as_work fallback = {
        .root = root,
        .kind = kind,
        .flags = (flags & ~(uint32_t)RDNA4_AS_TO_END) |
            RDNA4_AS_FALLBACK,
        .mode = (uint8_t)mode,
        .offset = offset,
        .stride = RDNA4_AS_TOKEN_BYTES,
        .count = words,
        .width = RDNA4_AS_TOKEN_BYTES,
    };
    return rdna4_as_append_work(works, capacity, count, stats, &fallback);
}

static inline rdna4_as_status rdna4_as_work_capacity(
    const rdna4_as_plan *plan, size_t transport_count, size_t *capacity) {
    if (!plan || !capacity) return RDNA4_AS_INVALID;
    if (plan->count > SIZE_MAX - transport_count) return RDNA4_AS_OVERFLOW;
    *capacity = plan->count + transport_count;
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_as_materialize(
    const rdna4_as_plan *plan, const rdna4_as_bound roots[RDNA4_AS_ROOTS],
    const rdna4_as_transport *transports, size_t transport_count,
    rdna4_as_work *works, size_t capacity, size_t *out_count,
    rdna4_as_work_stats *stats) {
    if (!plan || !roots || (!transports && transport_count) ||
        (!works && capacity) || !out_count || !stats)
        return RDNA4_AS_INVALID;
    *out_count = 0;
    memset(stats, 0, sizeof(*stats));
    if (!plan->finalized) return RDNA4_AS_NOT_FINALIZED;
    if (!plan->count && !transport_count) return RDNA4_AS_OK;
    size_t required = 0;
    if (rdna4_as_work_capacity(plan, transport_count, &required) !=
            RDNA4_AS_OK ||
        required > capacity) {
        stats->capacity_rejections = 1;
        stats->saturated = true;
        return RDNA4_AS_CAPACITY;
    }

    size_t count = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        const rdna4_as_span *span = &plan->spans[i];
        const rdna4_as_bound *bound = &roots[span->root];
        if (!bound->present) {
            stats->missing_roots = rdna4_as_sat_add(
                stats->missing_roots, 1, &stats->saturated);
            continue;
        }
        uint64_t absolute = 0;
        if (!rdna4_as_make_absolute(bound, span->offset, &absolute) ||
            absolute > bound->length) {
            stats->invalid_spans = rdna4_as_sat_add(
                stats->invalid_spans, 1, &stats->saturated);
            rdna4_as_status fallback = rdna4_as_append_fallback(
                bound, span->root, span->kind, span->flags, works, capacity,
                &count, stats);
            if (fallback != RDNA4_AS_OK) return fallback;
            continue;
        }

        uint64_t stride = span->stride;
        uint64_t width = span->width;
        uint64_t elements = span->count;
        uint32_t flags = span->flags;
        if (flags & RDNA4_AS_TO_END) {
            uint64_t remainder = absolute % width;
            uint64_t align = remainder ? width - remainder : 0;
            if (align > bound->length - absolute) continue;
            absolute += align;
            uint64_t remaining = bound->length - absolute;
            if (remaining < width) continue;
            stride = width;
            elements = remaining / width;
            flags &= ~(uint32_t)RDNA4_AS_TO_END;
        } else {
            uint64_t extent = 0;
            if (!rdna4_as_span_extent(span, &extent) ||
                !rdna4_as_range_valid(absolute, extent, bound->length)) {
                stats->invalid_spans = rdna4_as_sat_add(
                    stats->invalid_spans, 1, &stats->saturated);
                rdna4_as_status fallback = rdna4_as_append_fallback(
                    bound, span->root, span->kind, span->flags, works, capacity,
                    &count, stats);
                if (fallback != RDNA4_AS_OK) return fallback;
                continue;
            }
        }
        rdna4_as_scan_mode mode = rdna4_as_scan_mode_for(bound);
        if (mode == RDNA4_AS_SCAN_NONE || !elements) continue;
        if (!bound->writable) flags &= ~(uint32_t)RDNA4_AS_WRITABLE;
        rdna4_as_work work = {
            .root = span->root,
            .kind = span->kind,
            .flags = flags,
            .mode = (uint8_t)mode,
            .offset = absolute,
            .stride = stride,
            .count = elements,
            .width = width,
        };
        rdna4_as_status status = rdna4_as_append_work(
            works, capacity, &count, stats, &work);
        if (status != RDNA4_AS_OK) return status;
    }

    for (size_t i = 0; i < transport_count; ++i) {
        const rdna4_as_transport *transport = &transports[i];
        if (transport->root >= RDNA4_AS_ROOTS) return RDNA4_AS_INVALID;
        const rdna4_as_bound *bound = &roots[transport->root];
        if (!bound->present) {
            stats->missing_roots = rdna4_as_sat_add(
                stats->missing_roots, 1, &stats->saturated);
            continue;
        }
        uint64_t absolute = 0;
        if (!rdna4_as_make_absolute(bound, transport->offset, &absolute) ||
            !rdna4_as_range_valid(absolute, transport->length, bound->length) ||
            (absolute & (RDNA4_AS_TOKEN_BYTES - 1)) ||
            (transport->length & (RDNA4_AS_TOKEN_BYTES - 1))) {
            stats->invalid_spans = rdna4_as_sat_add(
                stats->invalid_spans, 1, &stats->saturated);
            rdna4_as_status fallback = rdna4_as_append_fallback(
                bound, transport->root, RDNA4_AS_KIND_UNKNOWN,
                transport->flags | RDNA4_AS_TRANSPORT, works, capacity, &count,
                stats);
            if (fallback != RDNA4_AS_OK) return fallback;
            continue;
        }
        if (!transport->length) continue;
        rdna4_as_scan_mode mode = rdna4_as_scan_mode_for(bound);
        if (mode == RDNA4_AS_SCAN_NONE) continue;
        uint32_t flags = transport->flags | RDNA4_AS_TRANSPORT;
        if (!bound->writable) flags &= ~(uint32_t)RDNA4_AS_WRITABLE;
        rdna4_as_work work = {
            .root = transport->root,
            .kind = RDNA4_AS_KIND_UNKNOWN,
            .flags = flags,
            .mode = (uint8_t)mode,
            .offset = absolute,
            .stride = RDNA4_AS_TOKEN_BYTES,
            .count = transport->length / RDNA4_AS_TOKEN_BYTES,
            .width = RDNA4_AS_TOKEN_BYTES,
        };
        rdna4_as_status status = rdna4_as_append_work(
            works, capacity, &count, stats, &work);
        if (status != RDNA4_AS_OK) return status;
    }
    *out_count = count;
    return RDNA4_AS_OK;
}


/*
 * Reflection-to-plan adapter for garnet_argument_scan.h.
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

/*
 * Incremental dirty-range and fast pure-C token scanning for mapped argument
 * transport buffers.
 *
 * Correctness contract:
 *   - Coherent storage (MTLStorageModeShared and Garnet's coherent
 *     MTLStorageModeManaged) may be mutated directly via -contents without
 *     calling -didModifyRange: or MTLArgumentEncoder. Passing
 *     coherent_direct_writes=true ALWAYS reads all covered token words through
 *     the pure-C 4x-unrolled range-filtered scanner (zero ObjC overhead, zero
 *     hash collisions, 100% exact).
 *   - Immutable / read-only or explicitly dirty-tracked buffers
 *     (coherent_direct_writes=false) maintain a 4 KiB chunk dirty bitset and
 *     cached 0x5244-prefixed token list per chunk, rescanning only dirty chunks
 *     and skipping clean empty chunks 64 at a time.
 */
#ifndef RDNA4_ARGUMENT_SCAN_INCREMENTAL_INC
#define RDNA4_ARGUMENT_SCAN_INCREMENTAL_INC

#define RDNA4_AS_TOKEN_PREFIX_SHIFT 48u
#define RDNA4_AS_TOKEN_PREFIX UINT64_C(0x5244)
#define RDNA4_AS_INC_CHUNK_SHIFT 12u
#define RDNA4_AS_INC_CHUNK_BYTES (UINT64_C(1) << RDNA4_AS_INC_CHUNK_SHIFT)
#define RDNA4_AS_LIVE_BITSET_SPAN 4096u

typedef struct {
    uint64_t token;
    uint32_t value_index;
} rdna4_as_live_entry;

typedef struct {
    rdna4_as_live_entry *entries;
    size_t count;
    uint64_t min_token;
    uint64_t max_token;
    uint64_t span;
    bool use_bitset;
    uint64_t bitset[RDNA4_AS_LIVE_BITSET_SPAN / 64u];
} rdna4_as_live_table;

typedef struct {
    uint64_t *bits;
    size_t bit_words;
    size_t value_count;
    uint64_t inline_bits[8];
} rdna4_as_match_set;

typedef struct {
    uint64_t *tokens;
    uint32_t count;
    uint32_t cap;
} rdna4_as_inc_chunk;

typedef struct {
    uint64_t buffer_length;
    size_t chunk_count;
    size_t bit_words;
    uint64_t *dirty_bits;
    uint64_t *nonempty_bits;
    rdna4_as_inc_chunk *chunks;
    uint64_t generation;
    uint64_t dirty_chunks_scanned;
    uint64_t clean_chunks_skipped;
    uint64_t coherent_words_scanned;
} rdna4_as_inc_cache;

static inline int rdna4_as_live_entry_compare(const void *left,
                                              const void *right) {
    const rdna4_as_live_entry *a = (const rdna4_as_live_entry *)left;
    const rdna4_as_live_entry *b = (const rdna4_as_live_entry *)right;
    if (a->token < b->token) return -1;
    if (a->token > b->token) return 1;
    return a->value_index < b->value_index ? -1 :
        a->value_index > b->value_index ? 1 : 0;
}

static inline rdna4_as_status rdna4_as_live_table_build(
    rdna4_as_live_table *table, rdna4_as_live_entry *entries, size_t count) {
    if (!table || (!entries && count)) return RDNA4_AS_INVALID;
    memset(table, 0, sizeof(*table));
    if (!count) return RDNA4_AS_OK;
    if (count > 1u)
        qsort(entries, count, sizeof(*entries), rdna4_as_live_entry_compare);
    table->entries = entries;
    table->count = count;
    table->min_token = entries[0].token;
    table->max_token = entries[count - 1u].token;
    table->span = table->max_token - table->min_token;
    if (table->span < RDNA4_AS_LIVE_BITSET_SPAN) {
        table->use_bitset = true;
        for (size_t i = 0; i < count; ++i) {
            uint64_t delta = entries[i].token - table->min_token;
            table->bitset[delta >> 6] |= UINT64_C(1) << (delta & 63u);
        }
    }
    return RDNA4_AS_OK;
}

static inline bool rdna4_as_live_table_find(const rdna4_as_live_table *table,
                                            uint64_t token,
                                            uint32_t *out_index) {
    if (!table || !table->count) return false;
    uint64_t delta = token - table->min_token;
    if (delta > table->span) return false;
    if (table->use_bitset &&
        !((table->bitset[delta >> 6] >> (delta & 63u)) & UINT64_C(1)))
        return false;
    size_t lo = 0, hi = table->count;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        uint64_t cur = table->entries[mid].token;
        if (cur == token) {
            if (out_index) *out_index = table->entries[mid].value_index;
            return true;
        }
        if (cur < token) lo = mid + 1u;
        else hi = mid;
    }
    return false;
}

static inline bool rdna4_as_match_set_init(rdna4_as_match_set *set,
                                           size_t value_count) {
    if (!set) return false;
    memset(set, 0, sizeof(*set));
    set->value_count = value_count;
    if (!value_count) return true;
    size_t words = (value_count + 63u) >> 6;
    set->bit_words = words;
    if (words <= (sizeof(set->inline_bits) / sizeof(set->inline_bits[0]))) {
        set->bits = set->inline_bits;
        return true;
    }
    set->bits = (uint64_t *)calloc(words, sizeof(*set->bits));
    if (!set->bits) {
        set->bit_words = 0;
        set->value_count = 0;
        return false;
    }
    return true;
}

static inline void rdna4_as_match_set_destroy(rdna4_as_match_set *set) {
    if (!set) return;
    if (set->bits && set->bits != set->inline_bits) free(set->bits);
    memset(set, 0, sizeof(*set));
}

static inline void rdna4_as_match_set_add(rdna4_as_match_set *set,
                                          uint32_t index) {
    if (!set || !set->bits || (size_t)index >= set->value_count) return;
    set->bits[index >> 6] |= UINT64_C(1) << (index & 63u);
}

static inline bool rdna4_as_match_set_has(const rdna4_as_match_set *set,
                                          uint32_t index) {
    if (!set || !set->bits || (size_t)index >= set->value_count) return false;
    return ((set->bits[index >> 6] >> (index & 63u)) & UINT64_C(1)) != 0;
}

static inline rdna4_as_status rdna4_as_scan_dense_words(
    const uint8_t *bytes, uint64_t byte_len, uint64_t offset,
    uint64_t word_count, const rdna4_as_live_table *live,
    rdna4_as_match_set *out) {
    if (!bytes || !live || !out) return RDNA4_AS_INVALID;
    if (offset > byte_len ||
        word_count > (byte_len - offset) / RDNA4_AS_TOKEN_BYTES)
        return RDNA4_AS_OVERFLOW;
    if (!word_count || !live->count) return RDNA4_AS_OK;
    const uint8_t *p = bytes + offset;
    const uint64_t min_tok = live->min_token;
    const uint64_t span = live->span;
    uint64_t rem = word_count;
    while (rem >= 4u) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p, sizeof(w0));
        memcpy(&w1, p + 8u, sizeof(w1));
        memcpy(&w2, p + 16u, sizeof(w2));
        memcpy(&w3, p + 24u, sizeof(w3));
        if (__builtin_expect(((w0 - min_tok) <= span) |
                             ((w1 - min_tok) <= span) |
                             ((w2 - min_tok) <= span) |
                             ((w3 - min_tok) <= span), 0)) {
            uint32_t idx = 0;
            if (rdna4_as_live_table_find(live, w0, &idx))
                rdna4_as_match_set_add(out, idx);
            if (rdna4_as_live_table_find(live, w1, &idx))
                rdna4_as_match_set_add(out, idx);
            if (rdna4_as_live_table_find(live, w2, &idx))
                rdna4_as_match_set_add(out, idx);
            if (rdna4_as_live_table_find(live, w3, &idx))
                rdna4_as_match_set_add(out, idx);
        }
        p += 32u;
        rem -= 4u;
    }
    while (rem--) {
        uint64_t w;
        memcpy(&w, p, sizeof(w));
        if (__builtin_expect((w - min_tok) <= span, 0)) {
            uint32_t idx = 0;
            if (rdna4_as_live_table_find(live, w, &idx))
                rdna4_as_match_set_add(out, idx);
        }
        p += 8u;
    }
    return RDNA4_AS_OK;
}

static inline rdna4_as_status rdna4_as_scan_strided_words(
    const uint8_t *bytes, uint64_t byte_len, const rdna4_as_work *work,
    const rdna4_as_live_table *live, rdna4_as_match_set *out) {
    if (!bytes || !work || !live || !out ||
        work->width != RDNA4_AS_TOKEN_BYTES)
        return RDNA4_AS_INVALID;
    uint64_t extent = 0;
    rdna4_as_status st = rdna4_as_work_extent(work, &extent);
    if (st != RDNA4_AS_OK) return st;
    if (work->offset > byte_len || extent > byte_len - work->offset)
        return RDNA4_AS_OVERFLOW;
    if (!live->count) return RDNA4_AS_OK;
    if (work->stride == RDNA4_AS_TOKEN_BYTES)
        return rdna4_as_scan_dense_words(bytes, byte_len, work->offset,
                                         work->count, live, out);
    const uint64_t min_tok = live->min_token;
    const uint64_t span = live->span;
    uint64_t off = work->offset;
    for (uint64_t i = 0; i < work->count; ++i) {
        uint64_t w;
        memcpy(&w, bytes + off, sizeof(w));
        if (__builtin_expect((w - min_tok) <= span, 0)) {
            uint32_t idx = 0;
            if (rdna4_as_live_table_find(live, w, &idx))
                rdna4_as_match_set_add(out, idx);
        }
        if (i + 1u < work->count) off += work->stride;
    }
    return RDNA4_AS_OK;
}

static inline void rdna4_as_inc_cache_destroy(rdna4_as_inc_cache *cache) {
    if (!cache) return;
    if (cache->chunks) {
        for (size_t i = 0; i < cache->chunk_count; ++i)
            free(cache->chunks[i].tokens);
        free(cache->chunks);
    }
    free(cache->dirty_bits);
    free(cache->nonempty_bits);
    memset(cache, 0, sizeof(*cache));
}

static inline rdna4_as_status rdna4_as_inc_cache_init(
    rdna4_as_inc_cache *cache, uint64_t buffer_length) {
    if (!cache) return RDNA4_AS_INVALID;
    memset(cache, 0, sizeof(*cache));
    cache->buffer_length = buffer_length;
    if (!buffer_length) return RDNA4_AS_OK;
    if (buffer_length > UINT64_MAX - (RDNA4_AS_INC_CHUNK_BYTES - 1u))
        return RDNA4_AS_OVERFLOW;
    uint64_t chunks64 =
        (buffer_length + RDNA4_AS_INC_CHUNK_BYTES - 1u) >>
        RDNA4_AS_INC_CHUNK_SHIFT;
    if (chunks64 > (uint64_t)SIZE_MAX) return RDNA4_AS_OVERFLOW;
    size_t chunk_count = (size_t)chunks64;
    if (chunk_count > SIZE_MAX - 63u) return RDNA4_AS_OVERFLOW;
    size_t bit_words = (chunk_count + 63u) >> 6;
    cache->dirty_bits =
        (uint64_t *)calloc(bit_words, sizeof(*cache->dirty_bits));
    cache->nonempty_bits =
        (uint64_t *)calloc(bit_words, sizeof(*cache->nonempty_bits));
    cache->chunks = (rdna4_as_inc_chunk *)calloc(
        chunk_count, sizeof(*cache->chunks));
    if (!cache->dirty_bits || !cache->nonempty_bits || !cache->chunks) {
        rdna4_as_inc_cache_destroy(cache);
        return RDNA4_AS_CAPACITY;
    }
    cache->chunk_count = chunk_count;
    cache->bit_words = bit_words;
    for (size_t i = 0; i < bit_words; ++i)
        cache->dirty_bits[i] = UINT64_MAX;
    unsigned rem_bits = (unsigned)(chunk_count & 63u);
    if (rem_bits)
        cache->dirty_bits[bit_words - 1u] = (UINT64_C(1) << rem_bits) - 1u;
    return RDNA4_AS_OK;
}

static inline void rdna4_as_inc_cache_mark_dirty(rdna4_as_inc_cache *cache,
                                                 uint64_t offset,
                                                 uint64_t length) {
    if (!cache || !cache->chunk_count || !length ||
        offset >= cache->buffer_length)
        return;
    uint64_t avail = cache->buffer_length - offset;
    uint64_t clamped = length > avail ? avail : length;
    size_t first = (size_t)(offset >> RDNA4_AS_INC_CHUNK_SHIFT);
    size_t last =
        (size_t)((offset + clamped - 1u) >> RDNA4_AS_INC_CHUNK_SHIFT);
    if (last >= cache->chunk_count) last = cache->chunk_count - 1u;
    size_t w_first = first >> 6, w_last = last >> 6;
    if (w_first == w_last) {
        unsigned lo = (unsigned)(first & 63u);
        unsigned hi = (unsigned)(last & 63u);
        uint64_t mask = hi == 63u ? (~UINT64_C(0) << lo)
                                  : (((UINT64_C(1) << (hi - lo + 1u)) - 1u) << lo);
        cache->dirty_bits[w_first] |= mask;
    } else {
        cache->dirty_bits[w_first] |= (~UINT64_C(0) << (first & 63u));
        for (size_t wi = w_first + 1u; wi < w_last; ++wi)
            cache->dirty_bits[wi] = UINT64_MAX;
        unsigned hi = (unsigned)(last & 63u);
        uint64_t tail = hi == 63u ? UINT64_MAX : ((UINT64_C(1) << (hi + 1u)) - 1u);
        cache->dirty_bits[w_last] |= tail;
    }
    cache->generation++;
}

static inline void rdna4_as_inc_cache_mark_all_dirty(
    rdna4_as_inc_cache *cache) {
    if (!cache || !cache->chunk_count) return;
    rdna4_as_inc_cache_mark_dirty(cache, 0, cache->buffer_length);
}

static inline bool rdna4_as_inc_chunk_push_unique(rdna4_as_inc_chunk *chunk,
                                                  uint64_t token) {
    for (uint32_t i = 0; i < chunk->count; ++i)
        if (chunk->tokens[i] == token) return true;
    if (chunk->count == chunk->cap) {
        uint32_t next_cap = chunk->cap ? chunk->cap * 2u : 4u;
        if (next_cap < chunk->cap) return false;
        uint64_t *grown = (uint64_t *)realloc(
            chunk->tokens, (size_t)next_cap * sizeof(*grown));
        if (!grown) return false;
        chunk->tokens = grown;
        chunk->cap = next_cap;
    }
    chunk->tokens[chunk->count++] = token;
    return true;
}

static inline bool rdna4_as_inc_cache_refresh_chunk(
    rdna4_as_inc_cache *cache, size_t chunk_idx, const uint8_t *bytes,
    uint64_t byte_len) {
    uint64_t chunk_off = (uint64_t)chunk_idx << RDNA4_AS_INC_CHUNK_SHIFT;
    if (chunk_off >= byte_len) return true;
    uint64_t avail = byte_len - chunk_off;
    uint64_t chunk_bytes =
        avail < RDNA4_AS_INC_CHUNK_BYTES ? avail : RDNA4_AS_INC_CHUNK_BYTES;
    uint64_t words = chunk_bytes / RDNA4_AS_TOKEN_BYTES;
    rdna4_as_inc_chunk *chunk = &cache->chunks[chunk_idx];
    chunk->count = 0;
    const uint8_t *p = bytes + chunk_off;
    uint64_t rem = words;
    while (rem >= 4u) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p, sizeof(w0));
        memcpy(&w1, p + 8u, sizeof(w1));
        memcpy(&w2, p + 16u, sizeof(w2));
        memcpy(&w3, p + 24u, sizeof(w3));
        if (__builtin_expect(((w0 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX) |
                             ((w1 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX) |
                             ((w2 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX) |
                             ((w3 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX), 0)) {
            if ((w0 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX &&
                !rdna4_as_inc_chunk_push_unique(chunk, w0)) return false;
            if ((w1 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX &&
                !rdna4_as_inc_chunk_push_unique(chunk, w1)) return false;
            if ((w2 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX &&
                !rdna4_as_inc_chunk_push_unique(chunk, w2)) return false;
            if ((w3 >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX &&
                !rdna4_as_inc_chunk_push_unique(chunk, w3)) return false;
        }
        p += 32u;
        rem -= 4u;
    }
    while (rem--) {
        uint64_t w;
        memcpy(&w, p, sizeof(w));
        if (__builtin_expect((w >> RDNA4_AS_TOKEN_PREFIX_SHIFT) == RDNA4_AS_TOKEN_PREFIX, 0)) {
            if (!rdna4_as_inc_chunk_push_unique(chunk, w)) return false;
        }
        p += 8u;
    }
    size_t wi = chunk_idx >> 6;
    uint64_t bit = UINT64_C(1) << (chunk_idx & 63u);
    if (chunk->count) cache->nonempty_bits[wi] |= bit;
    else cache->nonempty_bits[wi] &= ~bit;
    cache->dirty_bits[wi] &= ~bit;
    cache->dirty_chunks_scanned++;
    return true;
}

static inline rdna4_as_status rdna4_as_inc_cache_scan_work(
    rdna4_as_inc_cache *cache, const uint8_t *bytes, uint64_t byte_len,
    const rdna4_as_work *work, const rdna4_as_live_table *live,
    bool coherent_direct_writes, rdna4_as_match_set *out) {
    if (!bytes || !work || !live || !out ||
        work->width != RDNA4_AS_TOKEN_BYTES)
        return RDNA4_AS_INVALID;
    uint64_t extent = 0;
    rdna4_as_status st = rdna4_as_work_extent(work, &extent);
    if (st != RDNA4_AS_OK) return st;
    if (work->offset > byte_len || extent > byte_len - work->offset)
        return RDNA4_AS_OVERFLOW;
    if (!live->count) return RDNA4_AS_OK;

    if (coherent_direct_writes || !cache || !cache->chunk_count ||
        byte_len != cache->buffer_length ||
        work->stride != RDNA4_AS_TOKEN_BYTES ||
        (work->offset & (RDNA4_AS_TOKEN_BYTES - 1u)) != 0) {
        st = rdna4_as_scan_strided_words(bytes, byte_len, work, live, out);
        if (st == RDNA4_AS_OK && cache && coherent_direct_writes)
            cache->coherent_words_scanned += work->count;
        return st;
    }

    const uint64_t work_begin = work->offset;
    const uint64_t work_end = work->offset + extent;
    size_t first_chunk = (size_t)(work_begin >> RDNA4_AS_INC_CHUNK_SHIFT);
    size_t last_chunk = (size_t)((work_end - 1u) >> RDNA4_AS_INC_CHUNK_SHIFT);

    /* Partial leading chunk (or single-chunk subrange). */
    uint64_t first_start = (uint64_t)first_chunk << RDNA4_AS_INC_CHUNK_SHIFT;
    uint64_t first_limit = byte_len - first_start < RDNA4_AS_INC_CHUNK_BYTES
        ? byte_len : first_start + RDNA4_AS_INC_CHUNK_BYTES;
    uint64_t first_aligned_end = first_start +
        ((first_limit - first_start) & ~(uint64_t)(RDNA4_AS_TOKEN_BYTES - 1u));
    if (work_begin > first_start || work_end < first_aligned_end) {
        uint64_t seg_end = work_end < first_limit ? work_end : first_limit;
        uint64_t seg_words = (seg_end - work_begin) / RDNA4_AS_TOKEN_BYTES;
        st = rdna4_as_scan_dense_words(bytes, byte_len, work_begin, seg_words,
                                       live, out);
        if (st != RDNA4_AS_OK) return st;
        if (first_chunk == last_chunk) return RDNA4_AS_OK;
        first_chunk++;
    }

    /* Partial trailing chunk. */
    uint64_t last_start = (uint64_t)last_chunk << RDNA4_AS_INC_CHUNK_SHIFT;
    uint64_t last_limit = byte_len - last_start < RDNA4_AS_INC_CHUNK_BYTES
        ? byte_len : last_start + RDNA4_AS_INC_CHUNK_BYTES;
    uint64_t last_aligned_end = last_start +
        ((last_limit - last_start) & ~(uint64_t)(RDNA4_AS_TOKEN_BYTES - 1u));
    if (work_end < last_aligned_end) {
        uint64_t seg_words = (work_end - last_start) / RDNA4_AS_TOKEN_BYTES;
        st = rdna4_as_scan_dense_words(bytes, byte_len, last_start, seg_words,
                                       live, out);
        if (st != RDNA4_AS_OK) return st;
        if (first_chunk == last_chunk) return RDNA4_AS_OK;
        last_chunk--;
    }

    /* Full chunks [first_chunk, last_chunk]: skip clean empty chunks 64 at a time. */
    size_t w_first = first_chunk >> 6;
    size_t w_last = last_chunk >> 6;
    for (size_t wi = w_first; wi <= w_last; ++wi) {
        unsigned lo = wi == w_first ? (unsigned)(first_chunk & 63u) : 0u;
        unsigned hi = wi == w_last ? (unsigned)(last_chunk & 63u) : 63u;
        uint64_t mask = (lo == 0u && hi == 63u) ? UINT64_MAX
            : hi == 63u ? (~UINT64_C(0) << lo)
            : (((UINT64_C(1) << (hi - lo + 1u)) - 1u) << lo);
        uint64_t dirty = cache->dirty_bits[wi] & mask;
        uint64_t clean = mask & ~dirty;
        if (clean)
            cache->clean_chunks_skipped += (uint64_t)__builtin_popcountll(clean);
        uint64_t active = (dirty | (cache->nonempty_bits[wi] & mask));
        while (active) {
            unsigned bit = (unsigned)__builtin_ctzll(active);
            size_t c = (wi << 6) + bit;
            uint64_t bit_mask = UINT64_C(1) << bit;
            if (dirty & bit_mask) {
                if (!rdna4_as_inc_cache_refresh_chunk(cache, c, bytes, byte_len)) {
                    uint64_t c_off = (uint64_t)c << RDNA4_AS_INC_CHUNK_SHIFT;
                    uint64_t c_avail = byte_len - c_off;
                    uint64_t c_bytes = c_avail < RDNA4_AS_INC_CHUNK_BYTES
                        ? c_avail : RDNA4_AS_INC_CHUNK_BYTES;
                    st = rdna4_as_scan_dense_words(
                        bytes, byte_len, c_off, c_bytes / RDNA4_AS_TOKEN_BYTES,
                        live, out);
                    if (st != RDNA4_AS_OK) return st;
                    active &= active - 1u;
                    continue;
                }
            }
            if (cache->nonempty_bits[wi] & bit_mask) {
                const rdna4_as_inc_chunk *chunk = &cache->chunks[c];
                for (uint32_t ti = 0; ti < chunk->count; ++ti) {
                    uint32_t idx = 0;
                    if (rdna4_as_live_table_find(live, chunk->tokens[ti], &idx))
                        rdna4_as_match_set_add(out, idx);
                }
            }
            active &= active - 1u;
        }
    }
    return RDNA4_AS_OK;
}

#endif

#endif
