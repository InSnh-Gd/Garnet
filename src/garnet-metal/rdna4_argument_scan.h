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

#endif
