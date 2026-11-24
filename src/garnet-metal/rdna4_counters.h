// Pure-C GPU timestamp counter accounting and vertex amplification validation
// (host-testable without Metal/Vulkan headers; see test-counters-amp.c).
#ifndef RDNA4_COUNTERS_H
#define RDNA4_COUNTERS_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rdna4_devcaps.h"

#define RDNA4CNT_ERROR_VALUE UINT64_MAX
#define RDNA4CNT_MAX_VALID_NS (UINT64_MAX - UINT64_C(1))
#define RDNA4CNT_DONT_SAMPLE UINT64_MAX
#define RDNA4CNT_MAX_SAMPLES UINT32_C(65536)
#define RDNA4CNT_MAX_PASS_ATTACHMENTS UINT32_C(4)

enum {
    RDNA4CNT_SP_STAGE = 0,
    RDNA4CNT_SP_DRAW = 1,
    RDNA4CNT_SP_DISPATCH = 2,
    RDNA4CNT_SP_TILE_DISPATCH = 3,
    RDNA4CNT_SP_BLIT = 4,
};

enum {
    RDNA4CNT_STORAGE_SHARED = 0,
    RDNA4CNT_STORAGE_MANAGED = 1,
    RDNA4CNT_STORAGE_PRIVATE = 2,
    RDNA4CNT_STORAGE_MEMORYLESS = 3,
};

enum {
    RDNA4CNT_DESC_OK = 0,
    RDNA4CNT_DESC_INVALID = 1,
    RDNA4CNT_DESC_OOM = 2,
};

// shortcut: max supported vertex amplification count is 1 until garnetxlate lowers
// [[amplification_id]]/[[amplification_count]] for count > 1 and Vulkan multiview is
// enabled in rdna4_backend_impl.inc; raise once translator + multiview are verified.
#define RDNA4AMP_MAX_SUPPORTED_COUNT UINT32_C(1)
#define RDNA4AMP_MAX_VIEWPORTS UINT32_C(16)

typedef struct {
    uint32_t sample_count;
    uint32_t storage_mode;
    uint32_t valid_bits;
    double period_ns;
    uint8_t *sampled_bits;
    size_t sampled_bytes;
} rdna4cnt_pool_state;

typedef struct {
    uint32_t viewport_offset;
    uint32_t layer_offset;
} rdna4amp_mapping;

// Gate MTLCounterSamplingPoint on probed Vulkan timestamp caps; tile dispatch stays off (EX04).
static inline int rdna4cnt_supports_sampling_point(const rdna4dc_metal3_caps *caps,
                                                   uint64_t point) {
    if (!rdna4dc_metal3_timestamps_supported(caps)) return 0;
    switch (point) {
    case RDNA4CNT_SP_STAGE:
    case RDNA4CNT_SP_DRAW:
    case RDNA4CNT_SP_DISPATCH:
    case RDNA4CNT_SP_BLIT:
        return 1;
    case RDNA4CNT_SP_TILE_DISPATCH:
    default:
        return 0;
    }
}

static inline int rdna4cnt_validate_desc(uint64_t sample_count, uint64_t storage_mode,
                                         const char **why) {
    if (why) *why = NULL;
    if (storage_mode != RDNA4CNT_STORAGE_SHARED && storage_mode != RDNA4CNT_STORAGE_PRIVATE) {
        if (why) *why = "counter sample buffer storageMode must be Shared or Private";
        return RDNA4CNT_DESC_INVALID;
    }
    if (sample_count == 0u) {
        if (why) *why = "counter sample buffer sampleCount must be > 0";
        return RDNA4CNT_DESC_INVALID;
    }
    if (sample_count > RDNA4CNT_MAX_SAMPLES) {
        if (why) *why = "counter sample buffer sampleCount exceeds device limit";
        return RDNA4CNT_DESC_OOM;
    }
    return RDNA4CNT_DESC_OK;
}

static inline int rdna4cnt_index_ok(uint64_t sample_count, uint64_t index) {
    return sample_count > 0u && sample_count <= RDNA4CNT_MAX_SAMPLES && index < sample_count;
}

static inline int rdna4cnt_pass_index_ok(uint64_t sample_count, uint64_t index) {
    return index == RDNA4CNT_DONT_SAMPLE || rdna4cnt_index_ok(sample_count, index);
}

static inline int rdna4cnt_range_ok(uint64_t sample_count, uint64_t location, uint64_t length) {
    return sample_count > 0u && sample_count <= RDNA4CNT_MAX_SAMPLES &&
        length > 0u && location < sample_count && length <= sample_count - location;
}

static inline int rdna4cnt_resolve_cpu_allowed(uint32_t storage_mode) {
    return storage_mode == RDNA4CNT_STORAGE_SHARED;
}

static inline int rdna4cnt_period_valid(uint32_t valid_bits, double period_ns) {
    return valid_bits > 0u && valid_bits <= 64u && period_ns > 0.0 && period_ns < 1e9;
}

static inline int rdna4cnt_pool_init(rdna4cnt_pool_state *st, uint32_t sample_count,
                                     uint32_t storage_mode, uint32_t valid_bits,
                                     double period_ns) {
    if (!st) return 0;
    memset(st, 0, sizeof *st);
    if (rdna4cnt_validate_desc(sample_count, storage_mode, NULL) != RDNA4CNT_DESC_OK ||
        !rdna4cnt_period_valid(valid_bits, period_ns))
        return 0;
    size_t nbytes = ((size_t)sample_count + 7u) / 8u;
    uint8_t *bits = (uint8_t *)calloc(nbytes, 1u);
    if (!bits) return 0;
    st->sample_count = sample_count;
    st->storage_mode = storage_mode;
    st->valid_bits = valid_bits;
    st->period_ns = period_ns;
    st->sampled_bits = bits;
    st->sampled_bytes = nbytes;
    return 1;
}

static inline void rdna4cnt_pool_destroy(rdna4cnt_pool_state *st) {
    if (!st) return;
    free(st->sampled_bits);
    memset(st, 0, sizeof *st);
}

static inline int rdna4cnt_slot_mark_sampled(rdna4cnt_pool_state *st, uint32_t index) {
    if (!st || !st->sampled_bits || index >= st->sample_count) return 0;
    st->sampled_bits[index >> 3] |= (uint8_t)(1u << (index & 7u));
    return 1;
}

static inline int rdna4cnt_slot_clear(rdna4cnt_pool_state *st, uint32_t index) {
    if (!st || !st->sampled_bits || index >= st->sample_count) return 0;
    st->sampled_bits[index >> 3] &= (uint8_t)~(1u << (index & 7u));
    return 1;
}

static inline int rdna4cnt_slot_is_sampled(const rdna4cnt_pool_state *st, uint32_t index) {
    if (!st || !st->sampled_bits || index >= st->sample_count) return 0;
    return (st->sampled_bits[index >> 3] >> (index & 7u)) & 1u;
}

static inline uint32_t rdna4cnt_slot_sampled_count(const rdna4cnt_pool_state *st) {
    if (!st || !st->sampled_bits) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < st->sample_count; i++)
        n += (uint32_t)rdna4cnt_slot_is_sampled(st, i);
    return n;
}

// Exact 64x64 -> 128-bit unsigned multiply in ISO C11 (no float-to-int UB or 53-bit loss).
static inline void rdna4cnt_mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t a0 = a & UINT32_MAX, a1 = a >> 32;
    uint64_t b0 = b & UINT32_MAX, b1 = b >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & UINT32_MAX) + (p10 & UINT32_MAX);
    *lo = (p00 & UINT32_MAX) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static inline uint64_t rdna4cnt_mask_ticks(uint64_t raw_ticks, uint32_t valid_bits) {
    if (valid_bits == 0u || valid_bits > 64u) return 0u;
    if (valid_bits == 64u) return raw_ticks;
    return raw_ticks & ((UINT64_C(1) << valid_bits) - UINT64_C(1));
}

// Mask raw hardware counter ticks to valid_bits and scale by period_ns into monotonic
// nanoseconds without overflow. Valid results saturate at UINT64_MAX - 1 so they never
// collide with MTLCounterErrorValue (~0ULL).
static inline uint64_t rdna4cnt_ticks_to_ns(uint64_t raw_ticks, uint32_t valid_bits,
                                            double period_ns) {
    if (!rdna4cnt_period_valid(valid_bits, period_ns)) return RDNA4CNT_ERROR_VALUE;
    uint64_t ticks = rdna4cnt_mask_ticks(raw_ticks, valid_bits);
    uint64_t p_int = (uint64_t)period_ns;
    double p_frac = period_ns - (double)p_int;
    if (p_frac < 0.0) p_frac = 0.0;
    const uint64_t q_scale = UINT64_C(1) << 48;
    uint64_t p_frac_q = (uint64_t)(p_frac * (double)q_scale + 0.5);
    if (p_frac_q >= q_scale) {
        p_int++;
        p_frac_q = 0u;
    }
    uint64_t hi1 = 0u, lo1 = 0u, hi2 = 0u, lo2 = 0u;
    rdna4cnt_mul128(ticks, p_int, &hi1, &lo1);
    rdna4cnt_mul128(ticks, p_frac_q, &hi2, &lo2);
    uint64_t lo2_r = lo2 + (UINT64_C(1) << 47);
    uint64_t hi2_r = hi2 + (lo2_r < lo2 ? UINT64_C(1) : UINT64_C(0));
    if (hi1 != 0u || hi2_r >= q_scale) return RDNA4CNT_MAX_VALID_NS;
    uint64_t frac_ns = (hi2_r << 16) | (lo2_r >> 48);
    if (lo1 > RDNA4CNT_MAX_VALID_NS - frac_ns) return RDNA4CNT_MAX_VALID_NS;
    return lo1 + frac_ns;
}

// Convert vkGetQueryPoolResults (64-bit | WITH_AVAILABILITY, stride 16) pairs into ns.
static inline void rdna4cnt_resolve_pairs(const uint64_t *raw_pairs, uint32_t count,
                                          uint32_t valid_bits, double period_ns,
                                          uint64_t *out_ns) {
    if (!out_ns) return;
    for (uint32_t i = 0u; i < count; i++) {
        if (!raw_pairs || raw_pairs[2u * i + 1u] == 0u)
            out_ns[i] = RDNA4CNT_ERROR_VALUE;
        else
            out_ns[i] = rdna4cnt_ticks_to_ns(raw_pairs[2u * i], valid_bits, period_ns);
    }
}

// ---- Vertex Amplification validation ----

static inline int rdna4amp_device_supports(uint64_t count, uint32_t max_supported) {
    return max_supported >= 1u && count >= 1u && count <= (uint64_t)max_supported;
}

static inline int rdna4amp_pipeline_count_ok(uint64_t max_count, uint32_t device_max,
                                             int is_tessellation, const char **why) {
    if (why) *why = NULL;
    if (max_count == 0u) {
        if (why) *why = "maxVertexAmplificationCount must be >= 1";
        return 0;
    }
    if (is_tessellation && max_count > 1u) {
        if (why) *why = "vertex amplification > 1 in tessellation pipelines is not supported";
        return 0;
    }
    if (!rdna4amp_device_supports(max_count, device_max)) {
        if (why) *why = "maxVertexAmplificationCount exceeds device supportsVertexAmplificationCount";
        return 0;
    }
    return 1;
}

static inline int rdna4amp_validate_encoder_set(uint64_t count, const rdna4amp_mapping *mappings,
                                                uint32_t device_max, uint32_t max_viewports,
                                                uint32_t pass_layers, const char **why) {
    if (why) *why = NULL;
    if (!rdna4amp_device_supports(count, device_max)) {
        if (why) *why = "vertex amplification count exceeds device supportsVertexAmplificationCount";
        return 0;
    }
    uint32_t vp_lim = max_viewports ? max_viewports : 1u;
    if (vp_lim > RDNA4AMP_MAX_VIEWPORTS) vp_lim = RDNA4AMP_MAX_VIEWPORTS;
    uint32_t layer_lim = pass_layers ? pass_layers : 1u;
    if (mappings) {
        for (uint64_t i = 0u; i < count; i++) {
            if (mappings[i].viewport_offset >= vp_lim) {
                if (why) *why = "vertex amplification viewportArrayIndexOffset exceeds device viewport limit";
                return 0;
            }
            if (mappings[i].layer_offset >= layer_lim) {
                if (why) *why = "vertex amplification renderTargetArrayIndexOffset exceeds pass layer count";
                return 0;
            }
        }
    }
    return 1;
}

static inline int rdna4amp_draw_ok(uint32_t amp_count, uint32_t pipeline_max,
                                   const rdna4amp_mapping *mappings, uint32_t bound_viewports,
                                   uint32_t pass_layers, const char **why) {
    if (why) *why = NULL;
    if (amp_count == 0u || pipeline_max == 0u || amp_count > pipeline_max) {
        if (why) *why = "encoder vertex amplification count exceeds render pipeline maxVertexAmplificationCount";
        return 0;
    }
    uint32_t vp_lim = bound_viewports ? bound_viewports : 1u;
    uint32_t layer_lim = pass_layers ? pass_layers : 1u;
    if (mappings) {
        for (uint32_t i = 0u; i < amp_count; i++) {
            if (mappings[i].viewport_offset >= vp_lim) {
                if (why) *why = "vertex amplification viewportArrayIndexOffset exceeds bound viewport count";
                return 0;
            }
            if (mappings[i].layer_offset >= layer_lim) {
                if (why) *why = "vertex amplification renderTargetArrayIndexOffset exceeds pass layer count";
                return 0;
            }
        }
    }
    return 1;
}

static inline uint32_t rdna4amp_viewport_count(uint32_t bound_viewports, uint32_t bound_scissors,
                                               uint32_t viewport_offset, uint32_t max_viewports) {
    uint32_t lim = max_viewports ? max_viewports : 1u;
    if (lim > RDNA4AMP_MAX_VIEWPORTS) lim = RDNA4AMP_MAX_VIEWPORTS;
    uint32_t eff_vp = bound_viewports > viewport_offset ? (bound_viewports - viewport_offset) : 1u;
    uint32_t eff_sc = bound_scissors > viewport_offset ? (bound_scissors - viewport_offset) : 1u;
    uint32_t count = eff_vp > eff_sc ? eff_vp : eff_sc;
    if (count < 1u) count = 1u;
    if (count > lim) count = lim;
    return count;
}
#endif
