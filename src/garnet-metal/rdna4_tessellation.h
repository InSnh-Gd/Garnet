#ifndef RDNA4_TESSELLATION_H
#define RDNA4_TESSELLATION_H

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rdna4_spvrefl.h"

#define RDNA4_TESS_MAX_VBUFS 31u
#define RDNA4_TESS_MAX_INPUTS 32u
#define RDNA4_TESS_TRI_FACTOR_BYTES 8u
#define RDNA4_TESS_QUAD_FACTOR_BYTES 12u

typedef enum {
    RDNA4_TESS_DOM_TRI = 0,
    RDNA4_TESS_DOM_QUAD = 1
} rdna4_tess_domain_t;

typedef enum {
    RDNA4_TESS_PART_POW2 = 0,
    RDNA4_TESS_PART_INTEGER = 1,
    RDNA4_TESS_PART_FRAC_ODD = 2,
    RDNA4_TESS_PART_FRAC_EVEN = 3
} rdna4_tess_part_t;

typedef enum {
    RDNA4_TESS_WIND_CW = 0,
    RDNA4_TESS_WIND_CCW = 1
} rdna4_tess_wind_t;

typedef enum {
    RDNA4_TESS_FSTEP_CONST = 0,
    RDNA4_TESS_FSTEP_PER_PATCH = 1,
    RDNA4_TESS_FSTEP_PER_INSTANCE = 2,
    RDNA4_TESS_FSTEP_PER_PATCH_AND_INSTANCE = 3
} rdna4_tess_fstep_t;

typedef enum {
    RDNA4_TESS_CPIDX_NONE = 0,
    RDNA4_TESS_CPIDX_U16 = 1,
    RDNA4_TESS_CPIDX_U32 = 2
} rdna4_tess_cpidx_t;

typedef enum {
    RDNA4_TESS_VSTEP_CONST = 0,
    RDNA4_TESS_VSTEP_PER_VERTEX = 1,
    RDNA4_TESS_VSTEP_PER_INSTANCE = 2,
    RDNA4_TESS_VSTEP_PER_PATCH = 3,
    RDNA4_TESS_VSTEP_PER_PATCH_CP = 4
} rdna4_tess_vstep_t;

typedef enum {
    RDNA4_TSCALAR_NONE = 0,
    RDNA4_TSCALAR_F32 = 1,
    RDNA4_TSCALAR_S32 = 2,
    RDNA4_TSCALAR_U32 = 3,
    RDNA4_TSCALAR_F16 = 4,
    RDNA4_TSCALAR_S16 = 5,
    RDNA4_TSCALAR_U16 = 6
} rdna4_tess_scalar_t;

typedef enum {
    RDNA4_TFMT_INVALID = 0,
    RDNA4_TFMT_U8x1, RDNA4_TFMT_U8x2, RDNA4_TFMT_U8x3, RDNA4_TFMT_U8x4,
    RDNA4_TFMT_S8x1, RDNA4_TFMT_S8x2, RDNA4_TFMT_S8x3, RDNA4_TFMT_S8x4,
    RDNA4_TFMT_U8x1_N, RDNA4_TFMT_U8x2_N, RDNA4_TFMT_U8x3_N, RDNA4_TFMT_U8x4_N,
    RDNA4_TFMT_S8x1_N, RDNA4_TFMT_S8x2_N, RDNA4_TFMT_S8x3_N, RDNA4_TFMT_S8x4_N,
    RDNA4_TFMT_BGRA8_N,
    RDNA4_TFMT_U16x1, RDNA4_TFMT_U16x2, RDNA4_TFMT_U16x3, RDNA4_TFMT_U16x4,
    RDNA4_TFMT_S16x1, RDNA4_TFMT_S16x2, RDNA4_TFMT_S16x3, RDNA4_TFMT_S16x4,
    RDNA4_TFMT_U16x1_N, RDNA4_TFMT_U16x2_N, RDNA4_TFMT_U16x3_N, RDNA4_TFMT_U16x4_N,
    RDNA4_TFMT_S16x1_N, RDNA4_TFMT_S16x2_N, RDNA4_TFMT_S16x3_N, RDNA4_TFMT_S16x4_N,
    RDNA4_TFMT_F16x1, RDNA4_TFMT_F16x2, RDNA4_TFMT_F16x3, RDNA4_TFMT_F16x4,
    RDNA4_TFMT_F32x1, RDNA4_TFMT_F32x2, RDNA4_TFMT_F32x3, RDNA4_TFMT_F32x4,
    RDNA4_TFMT_S32x1, RDNA4_TFMT_S32x2, RDNA4_TFMT_S32x3, RDNA4_TFMT_S32x4,
    RDNA4_TFMT_U32x1, RDNA4_TFMT_U32x2, RDNA4_TFMT_U32x3, RDNA4_TFMT_U32x4,
    RDNA4_TFMT_S1010102_N,
    RDNA4_TFMT_U1010102_N
} rdna4_tess_vfmt_t;

typedef struct {
    uint32_t location;
    uint8_t is_patch;
    uint8_t has_attr;
    uint8_t vfmt;
    uint8_t buf_idx;
    uint8_t step_func;
    uint8_t tes_scalar;
    uint8_t tes_comps;
    uint8_t _pad;
    uint32_t step_rate;
    uint32_t attr_offset;
    uint32_t buf_stride;
} rdna4_tess_input_t;

typedef struct {
    uint8_t domain;
    uint8_t partition;
    uint8_t winding;
    uint8_t factor_step;
    uint8_t cp_idx_type;
    uint8_t factor_scale_enabled;
    uint8_t has_patch_id;
    uint8_t has_instance_id;
    uint8_t instance_id_scalar;
    uint8_t instance_id_comps;
    uint16_t _pad0;
    float max_tess_factor;
    uint32_t control_point_count;
    uint32_t tess_set;
    uint32_t patch_id_loc;
    uint32_t instance_id_loc;
    uint32_t patch_id_var;
    uint32_t num_inputs;
    rdna4_tess_input_t inputs[RDNA4_TESS_MAX_INPUTS];
} rdna4_tess_cfg_t;

// 560-byte SSBO read by generated TCS (STD430 matches C99 layout byte-for-byte).
typedef struct {
    uint64_t factor_bda;
    uint64_t factor_size;
    uint64_t factor_instance_stride;
    uint64_t patch_idx_bda;
    uint64_t patch_idx_size;
    uint64_t cp_idx_bda;
    uint64_t cp_idx_size;
    float factor_scale;
    uint32_t num_patch_cp;
    uint64_t vb_bda[RDNA4_TESS_MAX_VBUFS];
    uint64_t vb_size[RDNA4_TESS_MAX_VBUFS];
} rdna4_tess_draw_params_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rdna4_tess_draw_params_t) == 560, "rdna4_tess_draw_params_t must be 560 bytes");
#endif

static inline rdna4_tess_vfmt_t rdna4_tess_vfmt_from_vk(uint32_t vk) {
    switch (vk) {
    case 13: return RDNA4_TFMT_U8x1;   case 20: return RDNA4_TFMT_U8x2;
    case 27: return RDNA4_TFMT_U8x3;   case 41: return RDNA4_TFMT_U8x4;
    case 14: return RDNA4_TFMT_S8x1;   case 21: return RDNA4_TFMT_S8x2;
    case 28: return RDNA4_TFMT_S8x3;   case 42: return RDNA4_TFMT_S8x4;
    case 9:  return RDNA4_TFMT_U8x1_N; case 16: return RDNA4_TFMT_U8x2_N;
    case 23: return RDNA4_TFMT_U8x3_N; case 37: return RDNA4_TFMT_U8x4_N;
    case 10: return RDNA4_TFMT_S8x1_N; case 17: return RDNA4_TFMT_S8x2_N;
    case 24: return RDNA4_TFMT_S8x3_N; case 38: return RDNA4_TFMT_S8x4_N;
    case 44: return RDNA4_TFMT_BGRA8_N;
    case 74: return RDNA4_TFMT_U16x1;  case 81: return RDNA4_TFMT_U16x2;
    case 88: return RDNA4_TFMT_U16x3;  case 95: return RDNA4_TFMT_U16x4;
    case 75: return RDNA4_TFMT_S16x1;  case 82: return RDNA4_TFMT_S16x2;
    case 89: return RDNA4_TFMT_S16x3;  case 96: return RDNA4_TFMT_S16x4;
    case 70: return RDNA4_TFMT_U16x1_N; case 77: return RDNA4_TFMT_U16x2_N;
    case 84: return RDNA4_TFMT_U16x3_N; case 91: return RDNA4_TFMT_U16x4_N;
    case 71: return RDNA4_TFMT_S16x1_N; case 78: return RDNA4_TFMT_S16x2_N;
    case 85: return RDNA4_TFMT_S16x3_N; case 92: return RDNA4_TFMT_S16x4_N;
    case 76: return RDNA4_TFMT_F16x1;  case 83: return RDNA4_TFMT_F16x2;
    case 90: return RDNA4_TFMT_F16x3;  case 97: return RDNA4_TFMT_F16x4;
    case 100: return RDNA4_TFMT_F32x1; case 103: return RDNA4_TFMT_F32x2;
    case 106: return RDNA4_TFMT_F32x3; case 109: return RDNA4_TFMT_F32x4;
    case 99: return RDNA4_TFMT_S32x1;  case 102: return RDNA4_TFMT_S32x2;
    case 105: return RDNA4_TFMT_S32x3; case 108: return RDNA4_TFMT_S32x4;
    case 98: return RDNA4_TFMT_U32x1;  case 101: return RDNA4_TFMT_U32x2;
    case 104: return RDNA4_TFMT_U32x3; case 107: return RDNA4_TFMT_U32x4;
    case 65: return RDNA4_TFMT_S1010102_N;
    case 64: return RDNA4_TFMT_U1010102_N;
    default: return RDNA4_TFMT_INVALID;
    }
}

enum {
    RDNA4_TFENC_U8 = 0, RDNA4_TFENC_S8, RDNA4_TFENC_U8N, RDNA4_TFENC_S8N, RDNA4_TFENC_BGRA8N,
    RDNA4_TFENC_U16, RDNA4_TFENC_S16, RDNA4_TFENC_U16N, RDNA4_TFENC_S16N, RDNA4_TFENC_F16,
    RDNA4_TFENC_F32, RDNA4_TFENC_S32, RDNA4_TFENC_U32, RDNA4_TFENC_S1010102N, RDNA4_TFENC_U1010102N
};

typedef struct { uint8_t enc, comps, bytes, raw_scalar; } rdna4_tfmt_info_t;

static inline int rdna4_tess_vfmt_info(uint8_t vfmt, rdna4_tfmt_info_t *out) {
    if (vfmt == RDNA4_TFMT_INVALID || vfmt > RDNA4_TFMT_U1010102_N) return 0;
    static const rdna4_tfmt_info_t tab[] = {
        { 0, 0, 0, 0 },
        { RDNA4_TFENC_U8,  1, 1,  RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U8,  2, 2,  RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_U8,  3, 3,  RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U8,  4, 4,  RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_S8,  1, 1,  RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S8,  2, 2,  RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_S8,  3, 3,  RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S8,  4, 4,  RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_U8N, 1, 1,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_U8N, 2, 2,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_U8N, 3, 3,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_U8N, 4, 4,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_S8N, 1, 1,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_S8N, 2, 2,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_S8N, 3, 3,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_S8N, 4, 4,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_BGRA8N, 4, 4, RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_U16, 1, 2,  RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U16, 2, 4,  RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_U16, 3, 6,  RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U16, 4, 8,  RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_S16, 1, 2,  RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S16, 2, 4,  RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_S16, 3, 6,  RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S16, 4, 8,  RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_U16N,1, 2,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_U16N,2, 4,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_U16N,3, 6,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_U16N,4, 8,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_S16N,1, 2,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_S16N,2, 4,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_S16N,3, 6,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_S16N,4, 8,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_F16, 1, 2,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_F16, 2, 4,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_F16, 3, 6,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_F16, 4, 8,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_F32, 1, 4,  RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_F32, 2, 8,  RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_F32, 3, 12, RDNA4_TSCALAR_F32 }, { RDNA4_TFENC_F32, 4, 16, RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_S32, 1, 4,  RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S32, 2, 8,  RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_S32, 3, 12, RDNA4_TSCALAR_S32 }, { RDNA4_TFENC_S32, 4, 16, RDNA4_TSCALAR_S32 },
        { RDNA4_TFENC_U32, 1, 4,  RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U32, 2, 8,  RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_U32, 3, 12, RDNA4_TSCALAR_U32 }, { RDNA4_TFENC_U32, 4, 16, RDNA4_TSCALAR_U32 },
        { RDNA4_TFENC_S1010102N, 4, 4, RDNA4_TSCALAR_F32 },
        { RDNA4_TFENC_U1010102N, 4, 4, RDNA4_TSCALAR_F32 }
    };
    *out = tab[vfmt];
    return 1;
}

static inline int rdna4_tess_scalar_class(uint8_t s) {
    if (s == RDNA4_TSCALAR_F32 || s == RDNA4_TSCALAR_F16) return 1; // float
    if (s == RDNA4_TSCALAR_S32 || s == RDNA4_TSCALAR_S16) return 2; // sint
    if (s == RDNA4_TSCALAR_U32 || s == RDNA4_TSCALAR_U16) return 3; // uint
    return 0;
}

static inline uint32_t rdna4_tess_factor_stride(const rdna4_tess_cfg_t *c) {
    return c->domain == RDNA4_TESS_DOM_QUAD ? RDNA4_TESS_QUAD_FACTOR_BYTES : RDNA4_TESS_TRI_FACTOR_BYTES;
}

static inline int rdna4_tess_validate_cfg(const rdna4_tess_cfg_t *c, uint32_t max_hw_factor,
                                          uint32_t max_hw_patch_size, char *why, size_t whyn) {
    if (!c) { if (why && whyn) snprintf(why, whyn, "null tessellation config"); return 0; }
    if (c->domain > RDNA4_TESS_DOM_QUAD) {
        if (why && whyn) snprintf(why, whyn, "unsupported tessellation domain %u", c->domain);
        return 0;
    }
    if (c->partition > RDNA4_TESS_PART_FRAC_EVEN) {
        if (why && whyn) snprintf(why, whyn, "unsupported tessellation partition mode %u", c->partition);
        return 0;
    }
    if (c->winding > RDNA4_TESS_WIND_CCW) {
        if (why && whyn) snprintf(why, whyn, "unsupported tessellation output winding %u", c->winding);
        return 0;
    }
    if (c->factor_step > RDNA4_TESS_FSTEP_PER_PATCH_AND_INSTANCE) {
        if (why && whyn) snprintf(why, whyn, "unsupported tessellation factor step function %u", c->factor_step);
        return 0;
    }
    if (c->cp_idx_type > RDNA4_TESS_CPIDX_U32) {
        if (why && whyn) snprintf(why, whyn, "unsupported tessellation control point index type %u", c->cp_idx_type);
        return 0;
    }
    if (!isfinite(c->max_tess_factor) || c->max_tess_factor < 1.0f || c->max_tess_factor > (float)max_hw_factor) {
        if (why && whyn) snprintf(why, whyn, "maxTessellationFactor %g out of range [1, %u]", (double)c->max_tess_factor, max_hw_factor);
        return 0;
    }
    if (c->control_point_count > max_hw_patch_size) {
        if (why && whyn) snprintf(why, whyn, "controlPointCount %u exceeds max %u", c->control_point_count, max_hw_patch_size);
        return 0;
    }
    if (c->num_inputs > RDNA4_TESS_MAX_INPUTS) {
        if (why && whyn) snprintf(why, whyn, "tessellation input count %u exceeds %u", c->num_inputs, RDNA4_TESS_MAX_INPUTS);
        return 0;
    }
    if (c->has_instance_id && (!c->instance_id_comps || c->instance_id_comps > 4 || !rdna4_tess_scalar_class(c->instance_id_scalar))) {
        if (why && whyn) snprintf(why, whyn, "invalid tessellation instance_id type");
        return 0;
    }
    if (c->has_patch_id && c->has_instance_id && c->patch_id_loc == c->instance_id_loc) {
        if (why && whyn) snprintf(why, whyn, "patch_id and instance_id collide at location %u", c->patch_id_loc);
        return 0;
    }
    for (uint32_t i = 0; i < c->num_inputs; i++) {
        const rdna4_tess_input_t *in = &c->inputs[i];
        if (!in->tes_comps || in->tes_comps > 4 || !rdna4_tess_scalar_class(in->tes_scalar)) {
            if (why && whyn) snprintf(why, whyn, "tessellation input %u has unsupported SPIR-V type", in->location);
            return 0;
        }
        if ((c->has_patch_id && in->location == c->patch_id_loc) ||
            (c->has_instance_id && in->location == c->instance_id_loc)) {
            if (why && whyn) snprintf(why, whyn, "tessellation input location %u collides with system input", in->location);
            return 0;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (c->inputs[j].location == in->location) {
                if (why && whyn) snprintf(why, whyn, "duplicate tessellation input location %u", in->location);
                return 0;
            }
        }
        if (!in->has_attr) continue;
        rdna4_tfmt_info_t fi;
        if (!rdna4_tess_vfmt_info(in->vfmt, &fi)) {
            if (why && whyn) snprintf(why, whyn, "tessellation input %u has invalid vertex format %u", in->location, in->vfmt);
            return 0;
        }
        if (in->buf_idx >= RDNA4_TESS_MAX_VBUFS) {
            if (why && whyn) snprintf(why, whyn, "tessellation input %u bufferIndex %u >= %u", in->location, in->buf_idx, RDNA4_TESS_MAX_VBUFS);
            return 0;
        }
        if (in->step_func != RDNA4_TESS_VSTEP_CONST && in->step_func != RDNA4_TESS_VSTEP_PER_PATCH &&
            in->step_func != RDNA4_TESS_VSTEP_PER_PATCH_CP) {
            if (why && whyn) snprintf(why, whyn, "vertex buffer %u stepFunction %u invalid for tessellation", in->buf_idx, in->step_func);
            return 0;
        }
        if (in->is_patch && in->step_func == RDNA4_TESS_VSTEP_PER_PATCH_CP) {
            if (why && whyn) snprintf(why, whyn, "per-patch attribute %u cannot use PerPatchControlPoint stepFunction", in->location);
            return 0;
        }
        if (!in->is_patch && !c->control_point_count) {
            if (why && whyn) snprintf(why, whyn, "control-point attribute %u requires controlPointCount > 0", in->location);
            return 0;
        }
        if (in->step_func != RDNA4_TESS_VSTEP_CONST && (!in->step_rate || in->buf_stride > 2048)) {
            if (why && whyn) snprintf(why, whyn, "vertex buffer %u stride %u / stepRate %u invalid", in->buf_idx, in->buf_stride, in->step_rate);
            return 0;
        }
        if (in->attr_offset > 2047) {
            if (why && whyn) snprintf(why, whyn, "vertex attribute %u offset %u > 2047", in->location, in->attr_offset);
            return 0;
        }
    }
    return 1;
}

static inline void rdna4_tess_fnv(uint64_t *h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ull; }
}

static inline uint64_t rdna4_tess_pipeline_cache_key(const rdna4_tess_cfg_t *c, const void *tes_spv, size_t tes_bytes) {
    uint64_t h = 14695981039346656037ull;
    if (tes_spv && tes_bytes) rdna4_tess_fnv(&h, tes_spv, tes_bytes);
    if (!c) return h;
    uint32_t max_f_bits = 0;
    memcpy(&max_f_bits, &c->max_tess_factor, sizeof max_f_bits);
    uint8_t scalar[10] = { c->domain, c->partition, c->winding, c->factor_step, c->cp_idx_type,
                         c->factor_scale_enabled, c->has_patch_id, c->has_instance_id,
                         c->instance_id_scalar, c->instance_id_comps };
    rdna4_tess_fnv(&h, scalar, sizeof scalar);
    uint32_t u[7] = { max_f_bits, c->control_point_count, c->tess_set, c->patch_id_loc,
                      c->instance_id_loc, c->patch_id_var, c->num_inputs };
    rdna4_tess_fnv(&h, u, sizeof u);
    for (uint32_t i = 0; i < c->num_inputs && i < RDNA4_TESS_MAX_INPUTS; i++) {
        const rdna4_tess_input_t *in = &c->inputs[i];
        uint32_t d[8] = { in->location, in->is_patch, in->has_attr, in->vfmt,
                          in->buf_idx, in->step_func, in->tes_scalar, in->tes_comps };
        uint32_t r[3] = { in->step_rate, in->attr_offset, in->buf_stride };
        rdna4_tess_fnv(&h, d, sizeof d);
        rdna4_tess_fnv(&h, r, sizeof r);
    }
    return h;
}

static inline int rdna4_tess_type_desc(const RDNA4sMod *m, uint32_t tid, uint8_t *scalar, uint8_t *comps) {
    if (!m || !tid || tid > m->bound) return 0;
    uint32_t c = 1, sid = tid;
    if (m->id[tid].op == 23) { // OpTypeVector
        c = m->id[tid].n;
        sid = m->id[tid].elem;
    }
    if (!c || c > 4 || !sid || sid > m->bound) return 0;
    const RDNA4sId *st = &m->id[sid];
    uint8_t s = RDNA4_TSCALAR_NONE;
    if (st->op == 22) { // OpTypeFloat
        if (st->width == 32) s = RDNA4_TSCALAR_F32;
        else if (st->width == 16) s = RDNA4_TSCALAR_F16;
    } else if (st->op == 21) { // OpTypeInt
        if (st->width == 32) s = st->signedness ? RDNA4_TSCALAR_S32 : RDNA4_TSCALAR_U32;
        else if (st->width == 16) s = st->signedness ? RDNA4_TSCALAR_S16 : RDNA4_TSCALAR_U16;
    }
    if (!s) return 0;
    *scalar = s;
    *comps = (uint8_t)c;
    return 1;
}

// Inspects parsed TES SPIR-V inputs (call while RDNA4sMod is still live).
// If has_instance_id != 0 and instance_id_loc matches an input variable, records its SPIR-V type and omits it from user inputs[].
// Assigns patch_id_loc if TES declares BuiltIn PrimitiveId (7).
static inline int rdna4_tess_inspect_tes(const RDNA4sMod *m, rdna4_tess_cfg_t *c, char *why, size_t whyn) {
    if (!m || !c || !m->has_ep || m->ep_model != 2) {
        if (why && whyn) snprintf(why, whyn, "TES SPIR-V is not TessellationEvaluation");
        return 0;
    }
    if (m->tess_domain == 22) c->domain = RDNA4_TESS_DOM_TRI;
    else if (m->tess_domain == 24) c->domain = RDNA4_TESS_DOM_QUAD;
    else {
        if (why && whyn) snprintf(why, whyn, "TES SPIR-V lacks Triangles/Quads execution mode (%u)", m->tess_domain);
        return 0;
    }
    c->has_patch_id = 0;
    c->patch_id_var = 0;
    c->num_inputs = 0;
    uint8_t inst_enabled = c->has_instance_id;
    uint32_t inst_loc = c->instance_id_loc;
    c->has_instance_id = 0;
    uint64_t used_mask = 0;
    if (inst_enabled && inst_loc < 64) used_mask |= 1ull << inst_loc;
    uint32_t max_loc = inst_enabled ? inst_loc : 0;
    for (uint32_t id = 1; id <= m->bound; id++) {
        const RDNA4sId *v = &m->id[id];
        if (v->op != 59 || v->sc != 1) continue; // OpVariable Input
        if (v->builtin == 7) {
            if (c->has_patch_id) {
                if (why && whyn) snprintf(why, whyn, "multiple PrimitiveId inputs in TES");
                return 0;
            }
            c->has_patch_id = 1;
            c->patch_id_var = id;
            continue;
        }
        if (v->builtin >= 0 || v->loc < 0) continue;
        uint32_t ptr_id = v->elem;
        if (!ptr_id || ptr_id > m->bound || m->id[ptr_id].op != 32) {
            if (why && whyn) snprintf(why, whyn, "invalid TES input pointer");
            return 0;
        }
        uint32_t pt = m->id[ptr_id].elem;
        uint32_t et = pt;
        if (!v->patch && pt <= m->bound && m->id[pt].op == 28) et = m->id[pt].elem; // OpTypeArray for control points
        uint8_t sc = 0, nc = 0;
        if (!rdna4_tess_type_desc(m, et, &sc, &nc)) {
            if (why && whyn) snprintf(why, whyn, "unsupported TES input type at location %d", v->loc);
            return 0;
        }
        uint32_t loc = (uint32_t)v->loc;
        if (loc < 64) used_mask |= 1ull << loc;
        if (loc >= max_loc) max_loc = loc + 1;
        if (inst_enabled && loc == inst_loc) {
            c->has_instance_id = 1;
            c->instance_id_scalar = sc;
            c->instance_id_comps = nc;
            continue;
        }
        if (c->num_inputs >= RDNA4_TESS_MAX_INPUTS) {
            if (why && whyn) snprintf(why, whyn, "too many TES inputs (> %u)", RDNA4_TESS_MAX_INPUTS);
            return 0;
        }
        rdna4_tess_input_t *in = &c->inputs[c->num_inputs++];
        memset(in, 0, sizeof *in);
        in->location = loc;
        in->is_patch = v->patch ? 1 : 0;
        in->tes_scalar = sc;
        in->tes_comps = nc;
    }
    if (c->has_patch_id) {
        uint32_t pl = 0;
        while (pl < 64 && (used_mask & (1ull << pl))) pl++;
        c->patch_id_loc = pl < 64 ? pl : max_loc;
    }
    return 1;
}

// ---- Growable SPIR-V word buffer ----
typedef struct { uint32_t *w; size_t len, cap; int oom; } rdna4_tspv_buf_t;

static inline void rdna4_tb_free(rdna4_tspv_buf_t *b) { free(b->w); b->w = NULL; b->len = b->cap = 0; }

static inline void rdna4_tb_push(rdna4_tspv_buf_t *b, uint32_t v) {
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        uint32_t *nw = (uint32_t *)realloc(b->w, nc * 4);
        if (!nw) { b->oom = 1; return; }
        b->w = nw; b->cap = nc;
    }
    b->w[b->len++] = v;
}

static inline void rdna4_tb_append(rdna4_tspv_buf_t *dst, const rdna4_tspv_buf_t *src) {
    if (dst->oom || src->oom) { dst->oom = 1; return; }
    for (size_t i = 0; i < src->len; i++) rdna4_tb_push(dst, src->w[i]);
}

static inline void rdna4_tb_op(rdna4_tspv_buf_t *b, uint16_t op, uint16_t wc) {
    rdna4_tb_push(b, ((uint32_t)wc << 16) | op);
}

static inline size_t rdna4_tb_str_words(const char *s) {
    return (strlen(s) + 4) / 4;
}

static inline void rdna4_tb_str(rdna4_tspv_buf_t *b, const char *s) {
    size_t n = strlen(s) + 1;
    size_t nw = (n + 3) / 4;
    for (size_t i = 0; i < nw; i++) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4 && (i * 4 + k) < n; k++)
            w |= (uint32_t)(uint8_t)s[i * 4 + k] << (k * 8);
        rdna4_tb_push(b, w);
    }
}

static inline void rdna4_tb_dec(rdna4_tspv_buf_t *b, uint32_t target, uint32_t dec, int has_arg, uint32_t arg) {
    rdna4_tb_op(b, 71, has_arg ? 4 : 3);
    rdna4_tb_push(b, target); rdna4_tb_push(b, dec);
    if (has_arg) rdna4_tb_push(b, arg);
}

static inline void rdna4_tb_mdec(rdna4_tspv_buf_t *b, uint32_t st, uint32_t mi, uint32_t dec, uint32_t arg) {
    rdna4_tb_op(b, 72, 5);
    rdna4_tb_push(b, st); rdna4_tb_push(b, mi); rdna4_tb_push(b, dec); rdna4_tb_push(b, arg);
}

// Rewrites TES spacing + winding execution modes and converts BuiltIn PrimitiveId -> Location + Patch.
static inline int rdna4_tess_patch_tes_spv(const uint32_t *src, size_t nwords, const rdna4_tess_cfg_t *c,
                                           uint32_t **out_words, size_t *out_nwords) {
    if (!src || nwords < 5 || !c || !out_words || !out_nwords || src[0] != 0x07230203u) return 0;
    uint32_t want_spacing = (c->partition == RDNA4_TESS_PART_FRAC_ODD) ? 3u
                          : (c->partition == RDNA4_TESS_PART_FRAC_EVEN) ? 2u : 1u;
    uint32_t want_winding = (c->winding == RDNA4_TESS_WIND_CW) ? 4u : 5u;
    uint32_t ep_id = 0;
    size_t exec_insert_pos = 5;
    int saw_spacing = 0, saw_winding = 0;
    for (size_t p = 5; p < nwords; ) {
        uint32_t op = src[p] & 0xffffu, wc = src[p] >> 16;
        if (!wc || p + wc > nwords) return 0;
        if (op == 15 && wc >= 4) {
            if (!ep_id) ep_id = src[p + 2];
            if (p + wc > exec_insert_pos) exec_insert_pos = p + wc;
        }
        if (op == 16 && wc >= 3) {
            if (src[p + 1] == ep_id) {
                uint32_t m = src[p + 2];
                if (m == 1 || m == 2 || m == 3) saw_spacing = 1;
                if (m == 4 || m == 5) saw_winding = 1;
            }
            exec_insert_pos = p + wc;
        }
        p += wc;
    }
    if (!ep_id) return 0;
    rdna4_tspv_buf_t out = {0};
    for (size_t i = 0; i < 5; i++) rdna4_tb_push(&out, src[i]);
    int inserted_modes = 0;
    for (size_t p = 5; p < nwords; ) {
        uint32_t op = src[p] & 0xffffu, wc = src[p] >> 16;
        if (op == 16 && wc >= 3 && src[p + 1] == ep_id) {
            uint32_t m = src[p + 2];
            rdna4_tb_push(&out, src[p]);
            rdna4_tb_push(&out, src[p + 1]);
            rdna4_tb_push(&out, (m == 1 || m == 2 || m == 3) ? want_spacing : (m == 4 || m == 5) ? want_winding : m);
            for (size_t k = 3; k < wc; k++) rdna4_tb_push(&out, src[p + k]);
        } else if (op == 71 && wc >= 4 && c->has_patch_id && src[p + 1] == c->patch_id_var && src[p + 2] == 11 && src[p + 3] == 7) {
            rdna4_tb_dec(&out, c->patch_id_var, 30, 1, c->patch_id_loc); // Location
            rdna4_tb_dec(&out, c->patch_id_var, 15, 0, 0);               // Patch
        } else {
            for (size_t k = 0; k < wc; k++) rdna4_tb_push(&out, src[p + k]);
        }
        p += wc;
        if (!inserted_modes && p == exec_insert_pos) {
            if (!saw_spacing) { rdna4_tb_op(&out, 16, 3); rdna4_tb_push(&out, ep_id); rdna4_tb_push(&out, want_spacing); }
            if (!saw_winding) { rdna4_tb_op(&out, 16, 3); rdna4_tb_push(&out, ep_id); rdna4_tb_push(&out, want_winding); }
            inserted_modes = 1;
        }
    }
    if (out.oom) { rdna4_tb_free(&out); return 0; }
    *out_words = out.w;
    *out_nwords = out.len;
    return 1;
}

// Emits pass-through Vertex SPIR-V forwarding VertexIndex -> loc 0, InstanceIndex -> loc 1.
static inline int rdna4_tess_emit_vs_spv(uint32_t **out_words, size_t *out_nwords) {
    if (!out_words || !out_nwords) return 0;
    enum { ID_FN_MAIN = 1, ID_T_VOID, ID_T_U32, ID_T_FN, ID_P_IN, ID_P_OUT,
           ID_V_VTX, ID_V_INST, ID_O_PATCH, ID_O_INST, ID_LBL, ID_R_VTX, ID_R_INST, ID_BOUND };
    rdna4_tspv_buf_t b = {0};
    rdna4_tb_push(&b, 0x07230203u); rdna4_tb_push(&b, 0x00010500u); rdna4_tb_push(&b, 0); rdna4_tb_push(&b, ID_BOUND); rdna4_tb_push(&b, 0);
    rdna4_tb_op(&b, 17, 2); rdna4_tb_push(&b, 1); // OpCapability Shader
    rdna4_tb_op(&b, 14, 3); rdna4_tb_push(&b, 0); rdna4_tb_push(&b, 1); // OpMemoryModel Logical GLSL450
    rdna4_tb_op(&b, 15, (uint16_t)(3 + rdna4_tb_str_words("main") + 4));
    rdna4_tb_push(&b, 0); rdna4_tb_push(&b, ID_FN_MAIN); rdna4_tb_str(&b, "main");
    rdna4_tb_push(&b, ID_V_VTX); rdna4_tb_push(&b, ID_V_INST); rdna4_tb_push(&b, ID_O_PATCH); rdna4_tb_push(&b, ID_O_INST);
    rdna4_tb_dec(&b, ID_V_VTX, 11, 1, 42);  // BuiltIn VertexIndex
    rdna4_tb_dec(&b, ID_V_INST, 11, 1, 43); // BuiltIn InstanceIndex
    rdna4_tb_dec(&b, ID_O_PATCH, 30, 1, 0); // Location 0
    rdna4_tb_dec(&b, ID_O_INST, 30, 1, 1);  // Location 1
    rdna4_tb_op(&b, 19, 2); rdna4_tb_push(&b, ID_T_VOID);
    rdna4_tb_op(&b, 21, 4); rdna4_tb_push(&b, ID_T_U32); rdna4_tb_push(&b, 32); rdna4_tb_push(&b, 0);
    rdna4_tb_op(&b, 33, 3); rdna4_tb_push(&b, ID_T_FN); rdna4_tb_push(&b, ID_T_VOID);
    rdna4_tb_op(&b, 32, 4); rdna4_tb_push(&b, ID_P_IN); rdna4_tb_push(&b, 1); rdna4_tb_push(&b, ID_T_U32);
    rdna4_tb_op(&b, 32, 4); rdna4_tb_push(&b, ID_P_OUT); rdna4_tb_push(&b, 3); rdna4_tb_push(&b, ID_T_U32);
    rdna4_tb_op(&b, 59, 4); rdna4_tb_push(&b, ID_P_IN); rdna4_tb_push(&b, ID_V_VTX); rdna4_tb_push(&b, 1);
    rdna4_tb_op(&b, 59, 4); rdna4_tb_push(&b, ID_P_IN); rdna4_tb_push(&b, ID_V_INST); rdna4_tb_push(&b, 1);
    rdna4_tb_op(&b, 59, 4); rdna4_tb_push(&b, ID_P_OUT); rdna4_tb_push(&b, ID_O_PATCH); rdna4_tb_push(&b, 3);
    rdna4_tb_op(&b, 59, 4); rdna4_tb_push(&b, ID_P_OUT); rdna4_tb_push(&b, ID_O_INST); rdna4_tb_push(&b, 3);
    rdna4_tb_op(&b, 54, 5); rdna4_tb_push(&b, ID_T_VOID); rdna4_tb_push(&b, ID_FN_MAIN); rdna4_tb_push(&b, 0); rdna4_tb_push(&b, ID_T_FN);
    rdna4_tb_op(&b, 248, 2); rdna4_tb_push(&b, ID_LBL);
    rdna4_tb_op(&b, 61, 4); rdna4_tb_push(&b, ID_T_U32); rdna4_tb_push(&b, ID_R_VTX); rdna4_tb_push(&b, ID_V_VTX);
    rdna4_tb_op(&b, 61, 4); rdna4_tb_push(&b, ID_T_U32); rdna4_tb_push(&b, ID_R_INST); rdna4_tb_push(&b, ID_V_INST);
    rdna4_tb_op(&b, 62, 3); rdna4_tb_push(&b, ID_O_PATCH); rdna4_tb_push(&b, ID_R_VTX);
    rdna4_tb_op(&b, 62, 3); rdna4_tb_push(&b, ID_O_INST); rdna4_tb_push(&b, ID_R_INST);
    rdna4_tb_op(&b, 253, 1);
    rdna4_tb_op(&b, 56, 1);
    if (b.oom) { rdna4_tb_free(&b); return 0; }
    *out_words = b.w; *out_nwords = b.len;
    return 1;
}

// ---- Tessellation Control Shader (TCS) Builder ----
typedef struct {
    rdna4_tspv_buf_t dec, types, vars, code;
    uint32_t next_id, ext_glsl;
    uint32_t t_void, t_bool, t_u32, t_i32, t_u64, t_f32, t_f16, t_u16, t_i16;
    uint32_t t_vec[7][5]; // [scalar][1..4]
    uint32_t c_u32_0, c_u32_1, c_u32_2, c_u32_3, c_u32_4, c_u32_8, c_u32_16, c_u32_24, c_u32_32;
    uint32_t c_u32_ff, c_u32_ffff, c_u32_ffffff, c_u32_all;
    uint32_t c_u64_0, c_u64_2, c_u64_4;
    uint32_t c_f32_0, c_f32_1, c_f32_neg1, c_f32_max;
    uint32_t c_true, c_false;
    uint32_t fn_load32;
    uint32_t iface[128]; uint32_t niface;
    int need_f16, need_i16;
} rdna4_tcs_ctx_t;

static inline uint32_t rdna4_tcs_id(rdna4_tcs_ctx_t *k) { return k->next_id++; }

static inline uint32_t rdna4_tcs_u32(rdna4_tcs_ctx_t *k, uint32_t v) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 43, 4);
    rdna4_tb_push(&k->types, k->t_u32); rdna4_tb_push(&k->types, id); rdna4_tb_push(&k->types, v);
    return id;
}

static inline uint32_t rdna4_tcs_u64(rdna4_tcs_ctx_t *k, uint64_t v) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 43, 5);
    rdna4_tb_push(&k->types, k->t_u64); rdna4_tb_push(&k->types, id);
    rdna4_tb_push(&k->types, (uint32_t)v); rdna4_tb_push(&k->types, (uint32_t)(v >> 32));
    return id;
}

static inline uint32_t rdna4_tcs_f32(rdna4_tcs_ctx_t *k, float f) {
    uint32_t u = 0; memcpy(&u, &f, 4);
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 43, 4);
    rdna4_tb_push(&k->types, k->t_f32); rdna4_tb_push(&k->types, id); rdna4_tb_push(&k->types, u);
    return id;
}

static inline uint32_t rdna4_tcs_scalar_type(rdna4_tcs_ctx_t *k, uint8_t s) {
    switch (s) {
    case RDNA4_TSCALAR_F32: return k->t_f32;
    case RDNA4_TSCALAR_S32: return k->t_i32;
    case RDNA4_TSCALAR_U32: return k->t_u32;
    case RDNA4_TSCALAR_F16:
        k->need_f16 = 1;
        if (!k->t_f16) { k->t_f16 = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 22, 3); rdna4_tb_push(&k->types, k->t_f16); rdna4_tb_push(&k->types, 16); }
        return k->t_f16;
    case RDNA4_TSCALAR_S16:
        k->need_i16 = 1;
        if (!k->t_i16) { k->t_i16 = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_i16); rdna4_tb_push(&k->types, 16); rdna4_tb_push(&k->types, 1); }
        return k->t_i16;
    case RDNA4_TSCALAR_U16:
        k->need_i16 = 1;
        if (!k->t_u16) { k->t_u16 = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_u16); rdna4_tb_push(&k->types, 16); rdna4_tb_push(&k->types, 0); }
        return k->t_u16;
    default: return k->t_f32;
    }
}

static inline uint32_t rdna4_tcs_val_type(rdna4_tcs_ctx_t *k, uint8_t s, uint8_t comps) {
    uint32_t st = rdna4_tcs_scalar_type(k, s);
    if (comps <= 1) return st;
    if (!k->t_vec[s][comps]) {
        uint32_t vt = rdna4_tcs_id(k);
        rdna4_tb_op(&k->types, 23, 4);
        rdna4_tb_push(&k->types, vt); rdna4_tb_push(&k->types, st); rdna4_tb_push(&k->types, comps);
        k->t_vec[s][comps] = vt;
    }
    return k->t_vec[s][comps];
}

static inline uint32_t rdna4_tcs_ptr(rdna4_tcs_ctx_t *k, uint32_t sc, uint32_t pt) {
    uint32_t p = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 32, 4);
    rdna4_tb_push(&k->types, p); rdna4_tb_push(&k->types, sc); rdna4_tb_push(&k->types, pt);
    return p;
}

static inline uint32_t rdna4_tcs_arr(rdna4_tcs_ctx_t *k, uint32_t elem_t, uint32_t len_c_id) {
    uint32_t a = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 28, 4);
    rdna4_tb_push(&k->types, a); rdna4_tb_push(&k->types, elem_t); rdna4_tb_push(&k->types, len_c_id);
    return a;
}

static inline uint32_t rdna4_tcs_var(rdna4_tcs_ctx_t *k, uint32_t ptr_t, uint32_t sc) {
    uint32_t v = rdna4_tcs_id(k);
    rdna4_tb_op(&k->vars, 59, 4);
    rdna4_tb_push(&k->vars, ptr_t); rdna4_tb_push(&k->vars, v); rdna4_tb_push(&k->vars, sc);
    if (k->niface < 128) k->iface[k->niface++] = v;
    return v;
}

static inline uint32_t rdna4_tcs_op2(rdna4_tcs_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a, uint32_t b) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, op, 5);
    rdna4_tb_push(&k->code, rt); rdna4_tb_push(&k->code, id); rdna4_tb_push(&k->code, a); rdna4_tb_push(&k->code, b);
    return id;
}

static inline uint32_t rdna4_tcs_op1(rdna4_tcs_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, op, 4);
    rdna4_tb_push(&k->code, rt); rdna4_tb_push(&k->code, id); rdna4_tb_push(&k->code, a);
    return id;
}

static inline uint32_t rdna4_tcs_sel(rdna4_tcs_ctx_t *k, uint32_t rt, uint32_t c, uint32_t tv, uint32_t fv) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 169, 6);
    rdna4_tb_push(&k->code, rt); rdna4_tb_push(&k->code, id);
    rdna4_tb_push(&k->code, c); rdna4_tb_push(&k->code, tv); rdna4_tb_push(&k->code, fv);
    return id;
}

static inline uint32_t rdna4_tcs_ext1(rdna4_tcs_ctx_t *k, uint32_t rt, uint32_t ext_op, uint32_t a) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 12, 6);
    rdna4_tb_push(&k->code, rt); rdna4_tb_push(&k->code, id);
    rdna4_tb_push(&k->code, k->ext_glsl); rdna4_tb_push(&k->code, ext_op); rdna4_tb_push(&k->code, a);
    return id;
}

static inline uint32_t rdna4_tcs_ext2(rdna4_tcs_ctx_t *k, uint32_t rt, uint32_t ext_op, uint32_t a, uint32_t b) {
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 12, 7);
    rdna4_tb_push(&k->code, rt); rdna4_tb_push(&k->code, id);
    rdna4_tb_push(&k->code, k->ext_glsl); rdna4_tb_push(&k->code, ext_op);
    rdna4_tb_push(&k->code, a); rdna4_tb_push(&k->code, b);
    return id;
}

static inline uint32_t rdna4_tcs_zero(rdna4_tcs_ctx_t *k, uint8_t s, uint8_t comps) {
    uint32_t s0 = (s == RDNA4_TSCALAR_F32) ? k->c_f32_0 :
                  (s == RDNA4_TSCALAR_U32) ? k->c_u32_0 : 0;
    if (!s0) {
        uint32_t st = rdna4_tcs_scalar_type(k, s);
        s0 = rdna4_tcs_id(k);
        rdna4_tb_op(&k->types, 43, 4);
        rdna4_tb_push(&k->types, st); rdna4_tb_push(&k->types, s0); rdna4_tb_push(&k->types, 0);
    }
    if (comps <= 1) return s0;
    uint32_t vt = rdna4_tcs_val_type(k, s, comps);
    uint32_t id = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 44, (uint16_t)(3 + comps)); // OpConstantComposite
    rdna4_tb_push(&k->types, vt); rdna4_tb_push(&k->types, id);
    for (uint8_t i = 0; i < comps; i++) rdna4_tb_push(&k->types, s0);
    return id;
}

static inline uint32_t rdna4_tcs_scalar_const_f(rdna4_tcs_ctx_t *k, float f, uint8_t s) {
    if (s == RDNA4_TSCALAR_F32) return rdna4_tcs_f32(k, f);
    if (s == RDNA4_TSCALAR_F16) return rdna4_tcs_op1(k, 115, rdna4_tcs_scalar_type(k, s), rdna4_tcs_f32(k, f));
    int iv = (int)f;
    uint32_t c32 = rdna4_tcs_u32(k, (uint32_t)iv);
    if (s == RDNA4_TSCALAR_U32) return c32;
    if (s == RDNA4_TSCALAR_S32) return rdna4_tcs_op1(k, 124, k->t_i32, c32);
    return rdna4_tcs_op1(k, 113, rdna4_tcs_scalar_type(k, s), c32);
}

static inline uint32_t rdna4_tcs_load_le32(rdna4_tcs_ctx_t *k, uint32_t base, uint32_t sz, uint32_t off, uint32_t len_c) {
    uint32_t res = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 57, 8);
    rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, res);
    rdna4_tb_push(&k->code, k->fn_load32);
    rdna4_tb_push(&k->code, base); rdna4_tb_push(&k->code, sz); rdna4_tb_push(&k->code, off); rdna4_tb_push(&k->code, len_c);
    return res;
}

// Emits bounds-checked aligned-4 PhysicalStorageBuffer reader: load_le32(u64 base, u64 size, u64 off, u32 len) -> u32.
static inline void rdna4_tcs_emit_load32_fn(rdna4_tcs_ctx_t *k) {
    uint32_t t_psb_u32 = rdna4_tcs_ptr(k, 5349, k->t_u32);
    uint32_t t_fn = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 33, 7);
    rdna4_tb_push(&k->types, t_fn); rdna4_tb_push(&k->types, k->t_u32);
    rdna4_tb_push(&k->types, k->t_u64); rdna4_tb_push(&k->types, k->t_u64);
    rdna4_tb_push(&k->types, k->t_u64); rdna4_tb_push(&k->types, k->t_u32);
    uint32_t c_u64_not3 = rdna4_tcs_u64(k, ~UINT64_C(3));
    uint32_t c_u64_3 = rdna4_tcs_u64(k, 3);

    k->fn_load32 = rdna4_tcs_id(k);
    uint32_t p_base = rdna4_tcs_id(k), p_size = rdna4_tcs_id(k), p_off = rdna4_tcs_id(k), p_len = rdna4_tcs_id(k);
    uint32_t b_entry = rdna4_tcs_id(k), b_ok = rdna4_tcs_id(k), b_w1 = rdna4_tcs_id(k), b_w1m = rdna4_tcs_id(k);
    uint32_t b_unalign = rdna4_tcs_id(k), b_sm = rdna4_tcs_id(k), b_ret = rdna4_tcs_id(k);

    rdna4_tb_op(&k->code, 54, 5);
    rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, k->fn_load32); rdna4_tb_push(&k->code, 0); rdna4_tb_push(&k->code, t_fn);
    rdna4_tb_op(&k->code, 55, 3); rdna4_tb_push(&k->code, k->t_u64); rdna4_tb_push(&k->code, p_base);
    rdna4_tb_op(&k->code, 55, 3); rdna4_tb_push(&k->code, k->t_u64); rdna4_tb_push(&k->code, p_size);
    rdna4_tb_op(&k->code, 55, 3); rdna4_tb_push(&k->code, k->t_u64); rdna4_tb_push(&k->code, p_off);
    rdna4_tb_op(&k->code, 55, 3); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, p_len);

    // Entry: inbounds = (base != 0) && (len >= 1) && (len <= 4) && (size >= u64(len)) && (off <= size - u64(len))
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_entry);
    uint32_t len64 = rdna4_tcs_op1(k, 113, k->t_u64, p_len);
    uint32_t ok_base = rdna4_tcs_op2(k, 171, k->t_bool, p_base, k->c_u64_0);
    uint32_t ok_l0 = rdna4_tcs_op2(k, 174, k->t_bool, p_len, k->c_u32_1);
    uint32_t ok_l4 = rdna4_tcs_op2(k, 178, k->t_bool, p_len, k->c_u32_4);
    uint32_t ok_sz = rdna4_tcs_op2(k, 174, k->t_bool, p_size, len64);
    uint32_t sz_rem = rdna4_tcs_op2(k, 130, k->t_u64, p_size, len64);
    uint32_t ok_off = rdna4_tcs_op2(k, 178, k->t_bool, p_off, sz_rem);
    uint32_t ok01 = rdna4_tcs_op2(k, 167, k->t_bool, ok_base, ok_l0);
    uint32_t ok23 = rdna4_tcs_op2(k, 167, k->t_bool, ok_l4, ok_sz);
    uint32_t ok_a = rdna4_tcs_op2(k, 167, k->t_bool, ok01, ok23);
    uint32_t inbounds = rdna4_tcs_op2(k, 167, k->t_bool, ok_a, ok_off);
    rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, b_ret); rdna4_tb_push(&k->code, 0);
    rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, inbounds); rdna4_tb_push(&k->code, b_ok); rdna4_tb_push(&k->code, b_ret);

    // b_ok: load aligned word 0; conditionally load aligned word 1 iff (rem + len) > 4
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_ok);
    uint32_t abs_addr = rdna4_tcs_op2(k, 128, k->t_u64, p_base, p_off);
    uint32_t rem64 = rdna4_tcs_op2(k, 199, k->t_u64, abs_addr, c_u64_3);
    uint32_t rem = rdna4_tcs_op1(k, 113, k->t_u32, rem64);
    uint32_t a0 = rdna4_tcs_op2(k, 199, k->t_u64, abs_addr, c_u64_not3);
    uint32_t ptr0 = rdna4_tcs_op1(k, 120, t_psb_u32, a0);
    uint32_t w0 = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 61, 6); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, w0);
    rdna4_tb_push(&k->code, ptr0); rdna4_tb_push(&k->code, 2); rdna4_tb_push(&k->code, 4); // Aligned 4
    uint32_t span = rdna4_tcs_op2(k, 128, k->t_u32, rem, p_len);
    uint32_t need_w1_gt4 = rdna4_tcs_op2(k, 176, k->t_bool, k->c_u32_4, span);
    rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, b_w1m); rdna4_tb_push(&k->code, 0);
    rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, need_w1_gt4); rdna4_tb_push(&k->code, b_w1); rdna4_tb_push(&k->code, b_w1m);

    // b_w1: load second word at a0 + 4
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_w1);
    uint32_t a1 = rdna4_tcs_op2(k, 128, k->t_u64, a0, k->c_u64_4);
    uint32_t ptr1 = rdna4_tcs_op1(k, 120, t_psb_u32, a1);
    uint32_t w1_loaded = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 61, 6); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, w1_loaded);
    rdna4_tb_push(&k->code, ptr1); rdna4_tb_push(&k->code, 2); rdna4_tb_push(&k->code, 4);
    rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_w1m);

    // b_w1m: merge w1, branch for rem == 0 vs rem in {1,2,3} (avoids shift-by-32 UB)
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_w1m);
    uint32_t w1 = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 245, 7); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, w1);
    rdna4_tb_push(&k->code, w1_loaded); rdna4_tb_push(&k->code, b_w1);
    rdna4_tb_push(&k->code, k->c_u32_0); rdna4_tb_push(&k->code, b_ok);
    uint32_t rem0 = rdna4_tcs_op2(k, 170, k->t_bool, rem, k->c_u32_0);
    rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, b_sm); rdna4_tb_push(&k->code, 0);
    rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, rem0); rdna4_tb_push(&k->code, b_sm); rdna4_tb_push(&k->code, b_unalign);

    // b_unalign: sh0 = rem * 8 (8/16/24), sh1 = 32 - sh0 (24/16/8)
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_unalign);
    uint32_t sh0 = rdna4_tcs_op2(k, 132, k->t_u32, rem, k->c_u32_8);
    uint32_t sh1 = rdna4_tcs_op2(k, 130, k->t_u32, k->c_u32_32, sh0);
    uint32_t lo = rdna4_tcs_op2(k, 194, k->t_u32, w0, sh0);
    uint32_t hi = rdna4_tcs_op2(k, 196, k->t_u32, w1, sh1);
    uint32_t combined = rdna4_tcs_op2(k, 197, k->t_u32, lo, hi);
    rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_sm);

    // b_sm: mask to len bytes via selects (zero variable shifts)
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_sm);
    uint32_t raw = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 245, 7); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, raw);
    rdna4_tb_push(&k->code, w0); rdna4_tb_push(&k->code, b_w1m);
    rdna4_tb_push(&k->code, combined); rdna4_tb_push(&k->code, b_unalign);
    uint32_t is1 = rdna4_tcs_op2(k, 170, k->t_bool, p_len, k->c_u32_1);
    uint32_t is2 = rdna4_tcs_op2(k, 170, k->t_bool, p_len, k->c_u32_2);
    uint32_t is3 = rdna4_tcs_op2(k, 170, k->t_bool, p_len, k->c_u32_3);
    uint32_t m34 = rdna4_tcs_sel(k, k->t_u32, is3, k->c_u32_ffffff, k->c_u32_all);
    uint32_t m24 = rdna4_tcs_sel(k, k->t_u32, is2, k->c_u32_ffff, m34);
    uint32_t mask = rdna4_tcs_sel(k, k->t_u32, is1, k->c_u32_ff, m24);
    uint32_t masked = rdna4_tcs_op2(k, 199, k->t_u32, raw, mask);
    rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_ret);

    // b_ret
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_ret);
    uint32_t rphi = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 245, 7); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, rphi);
    rdna4_tb_push(&k->code, masked); rdna4_tb_push(&k->code, b_sm);
    rdna4_tb_push(&k->code, k->c_u32_0); rdna4_tb_push(&k->code, b_entry);
    rdna4_tb_op(&k->code, 254, 2); rdna4_tb_push(&k->code, rphi);
    rdna4_tb_op(&k->code, 56, 1);
}

// Scales & clamps one tessellation factor: <=0 or NaN -> 0.0; optional Pow2 round-up; clamp to [1.0, max_factor].
static inline uint32_t rdna4_tcs_adjust_factor(rdna4_tcs_ctx_t *k, uint32_t raw_f, uint32_t scale_f,
                                               uint32_t max_f, int is_pow2) {
    uint32_t v = rdna4_tcs_op2(k, 133, k->t_f32, raw_f, scale_f);
    uint32_t pos = rdna4_tcs_op2(k, 186, k->t_bool, v, k->c_f32_0); // FOrdGreaterThan (NaN -> false)
    uint32_t clamped;
    if (is_pow2) {
        static const float thresholds[6] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
        static const float pow2s[7] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
        uint32_t r = rdna4_tcs_f32(k, pow2s[0]);
        for (int i = 0; i < 6; i++) {
            uint32_t gt = rdna4_tcs_op2(k, 186, k->t_bool, v, rdna4_tcs_f32(k, thresholds[i]));
            r = rdna4_tcs_sel(k, k->t_f32, gt, rdna4_tcs_f32(k, pow2s[i + 1]), r);
        }
        clamped = rdna4_tcs_ext2(k, k->t_f32, 37, r, max_f); // GLSL FMin
    } else {
        uint32_t cl = rdna4_tcs_id(k); // GLSL FClamp(v, 1.0, max_f)
        rdna4_tb_op(&k->code, 12, 8);
        rdna4_tb_push(&k->code, k->t_f32); rdna4_tb_push(&k->code, cl);
        rdna4_tb_push(&k->code, k->ext_glsl); rdna4_tb_push(&k->code, 43);
        rdna4_tb_push(&k->code, v); rdna4_tb_push(&k->code, k->c_f32_1); rdna4_tb_push(&k->code, max_f);
        clamped = cl;
    }
    return rdna4_tcs_sel(k, k->t_f32, pos, clamped, k->c_f32_0);
}

// Casts/converts 1..4 unpacked scalar IDs to match TES expected type, padding missing lanes with (0, 0, 0, 1).
static inline uint32_t rdna4_tcs_convert_attr(rdna4_tcs_ctx_t *k, const uint32_t *lanes, uint8_t nsrc,
                                              uint8_t src_scalar, uint8_t dst_scalar, uint8_t dst_comps) {
    uint32_t conv[4] = { 0, 0, 0, 0 };
    uint32_t dt = rdna4_tcs_scalar_type(k, dst_scalar);
    for (uint8_t c = 0; c < dst_comps; c++) {
        uint32_t sv;
        if (c < nsrc) {
            sv = lanes[c];
            if (src_scalar != dst_scalar) {
                int sc = rdna4_tess_scalar_class(src_scalar), dc = rdna4_tess_scalar_class(dst_scalar);
                if (sc == 1 && dc == 1) sv = rdna4_tcs_op1(k, 115, dt, sv);          // FConvert
                else if (sc != 1 && dc == 1) sv = rdna4_tcs_op1(k, (sc == 2) ? 111 : 112, dt, sv); // SToF / UToF
                else if (sc == 1 && dc != 1) sv = rdna4_tcs_op1(k, (dc == 2) ? 110 : 109, dt, sv); // FToS / FToU
                else if (dst_scalar == RDNA4_TSCALAR_S16 || dst_scalar == RDNA4_TSCALAR_U16)
                    sv = rdna4_tcs_op1(k, (dc == 2) ? 114 : 113, dt, sv);              // SConvert / UConvert
                else sv = rdna4_tcs_op1(k, 124, dt, sv);                               // Bitcast s32<->u32
            }
        } else {
            sv = rdna4_tcs_scalar_const_f(k, (c == 3) ? 1.0f : 0.0f, dst_scalar);
        }
        conv[c] = sv;
    }
    if (dst_comps == 1) return conv[0];
    uint32_t vt = rdna4_tcs_val_type(k, dst_scalar, dst_comps);
    uint32_t res = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 80, (uint16_t)(3 + dst_comps)); // OpCompositeConstruct
    rdna4_tb_push(&k->code, vt); rdna4_tb_push(&k->code, res);
    for (uint8_t c = 0; c < dst_comps; c++) rdna4_tb_push(&k->code, conv[c]);
    return res;
}

static inline uint32_t rdna4_tcs_fetch_and_unpack(rdna4_tcs_ctx_t *k, const rdna4_tess_input_t *in,
                                                  uint32_t base, uint32_t size, uint32_t elem_idx) {
    if (!in->has_attr) return rdna4_tcs_zero(k, in->tes_scalar, in->tes_comps);
    rdna4_tfmt_info_t fi;
    rdna4_tess_vfmt_info(in->vfmt, &fi);
    uint32_t off = rdna4_tcs_op2(k, 128, k->t_u64, rdna4_tcs_u64(k, in->attr_offset),
                                 rdna4_tcs_op2(k, 132, k->t_u64, elem_idx, rdna4_tcs_u64(k, in->buf_stride)));
    uint32_t w[4] = { 0, 0, 0, 0 };
    uint32_t nw = (fi.bytes + 3u) / 4u;
    for (uint32_t i = 0; i < nw; i++) {
        uint32_t rem = fi.bytes - i * 4u;
        uint32_t chunk = rem >= 4u ? 4u : rem;
        uint32_t chunk_off = i ? rdna4_tcs_op2(k, 128, k->t_u64, off, rdna4_tcs_u64(k, i * 4u)) : off;
        uint32_t lc = chunk == 4 ? k->c_u32_4 : chunk == 3 ? k->c_u32_3 : chunk == 2 ? k->c_u32_2 : k->c_u32_1;
        w[i] = rdna4_tcs_load_le32(k, base, size, chunk_off, lc);
    }
    uint32_t t_v2f = rdna4_tcs_val_type(k, RDNA4_TSCALAR_F32, 2);
    uint32_t t_v4f = rdna4_tcs_val_type(k, RDNA4_TSCALAR_F32, 4);
    uint32_t lanes[4] = { 0, 0, 0, 0 };
    uint32_t ext_id;
    switch (fi.enc) {
    case RDNA4_TFENC_F32:
        for (uint8_t i = 0; i < fi.comps; i++) lanes[i] = rdna4_tcs_op1(k, 124, k->t_f32, w[i]);
        break;
    case RDNA4_TFENC_S32:
        for (uint8_t i = 0; i < fi.comps; i++) lanes[i] = rdna4_tcs_op1(k, 124, k->t_i32, w[i]);
        break;
    case RDNA4_TFENC_U32:
        for (uint8_t i = 0; i < fi.comps; i++) lanes[i] = w[i];
        break;
    case RDNA4_TFENC_F16: case RDNA4_TFENC_U16N: case RDNA4_TFENC_S16N:
        ext_id = (fi.enc == RDNA4_TFENC_F16) ? 62u : (fi.enc == RDNA4_TFENC_U16N) ? 61u : 60u;
        for (uint8_t g = 0; g * 2 < fi.comps; g++) {
            uint32_t p2 = rdna4_tcs_ext1(k, t_v2f, ext_id, w[g]);
            for (uint8_t sub = 0; sub < 2 && g * 2 + sub < fi.comps; sub++) {
                uint32_t ex = rdna4_tcs_id(k);
                rdna4_tb_op(&k->code, 81, 5);
                rdna4_tb_push(&k->code, k->t_f32); rdna4_tb_push(&k->code, ex); rdna4_tb_push(&k->code, p2); rdna4_tb_push(&k->code, sub);
                lanes[g * 2 + sub] = ex;
            }
        }
        break;
    case RDNA4_TFENC_U8N: case RDNA4_TFENC_S8N: case RDNA4_TFENC_BGRA8N: {
        ext_id = (fi.enc == RDNA4_TFENC_S8N) ? 63u : 64u;
        uint32_t p4 = rdna4_tcs_ext1(k, t_v4f, ext_id, w[0]);
        for (uint8_t i = 0; i < fi.comps; i++) {
            uint32_t src_idx = (fi.enc == RDNA4_TFENC_BGRA8N) ? (i == 0 ? 2u : i == 2 ? 0u : i) : i;
            uint32_t ex = rdna4_tcs_id(k);
            rdna4_tb_op(&k->code, 81, 5);
            rdna4_tb_push(&k->code, k->t_f32); rdna4_tb_push(&k->code, ex); rdna4_tb_push(&k->code, p4); rdna4_tb_push(&k->code, src_idx);
            lanes[i] = ex;
        }
        break;
    }
    case RDNA4_TFENC_U8:
        for (uint8_t i = 0; i < fi.comps; i++) {
            uint32_t sh = i ? rdna4_tcs_op2(k, 194, k->t_u32, w[0], rdna4_tcs_u32(k, i * 8u)) : w[0];
            lanes[i] = rdna4_tcs_op2(k, 199, k->t_u32, sh, k->c_u32_ff);
        }
        break;
    case RDNA4_TFENC_S8:
        for (uint8_t i = 0; i < fi.comps; i++) {
            uint32_t shl = (i < 3) ? rdna4_tcs_op2(k, 196, k->t_u32, w[0], rdna4_tcs_u32(k, (3u - i) * 8u)) : w[0];
            uint32_t si = rdna4_tcs_op1(k, 124, k->t_i32, shl);
            lanes[i] = rdna4_tcs_op2(k, 195, k->t_i32, si, k->c_u32_24);
        }
        break;
    case RDNA4_TFENC_U16:
        for (uint8_t i = 0; i < fi.comps; i++) {
            uint32_t wu = w[i / 2u];
            uint32_t sh = (i & 1) ? rdna4_tcs_op2(k, 194, k->t_u32, wu, k->c_u32_16) : wu;
            lanes[i] = rdna4_tcs_op2(k, 199, k->t_u32, sh, k->c_u32_ffff);
        }
        break;
    case RDNA4_TFENC_S16:
        for (uint8_t i = 0; i < fi.comps; i++) {
            uint32_t wu = w[i / 2u];
            uint32_t shl = !(i & 1) ? rdna4_tcs_op2(k, 196, k->t_u32, wu, k->c_u32_16) : wu;
            uint32_t si = rdna4_tcs_op1(k, 124, k->t_i32, shl);
            lanes[i] = rdna4_tcs_op2(k, 195, k->t_i32, si, k->c_u32_16);
        }
        break;
    case RDNA4_TFENC_U1010102N: {
        uint32_t m10 = rdna4_tcs_u32(k, 0x3FFu);
        uint32_t inv10 = rdna4_tcs_f32(k, 1.0f / 1023.0f), inv2 = rdna4_tcs_f32(k, 1.0f / 3.0f);
        for (uint8_t i = 0; i < 4; i++) {
            uint32_t sh = i ? rdna4_tcs_op2(k, 194, k->t_u32, w[0], rdna4_tcs_u32(k, i * 10u)) : w[0];
            uint32_t masked = (i < 3) ? rdna4_tcs_op2(k, 199, k->t_u32, sh, m10) : sh;
            uint32_t fl = rdna4_tcs_op1(k, 112, k->t_f32, masked);
            lanes[i] = rdna4_tcs_op2(k, 133, k->t_f32, fl, i < 3 ? inv10 : inv2);
        }
        break;
    }
    case RDNA4_TFENC_S1010102N: {
        uint32_t inv10 = rdna4_tcs_f32(k, 1.0f / 511.0f), c22 = rdna4_tcs_u32(k, 22u), c30 = rdna4_tcs_u32(k, 30u);
        static const uint32_t shls[4] = { 22u, 12u, 2u, 0u };
        for (uint8_t i = 0; i < 4; i++) {
            uint32_t shl = shls[i] ? rdna4_tcs_op2(k, 196, k->t_u32, w[0], rdna4_tcs_u32(k, shls[i])) : w[0];
            uint32_t si = rdna4_tcs_op1(k, 124, k->t_i32, shl);
            uint32_t sx = rdna4_tcs_op2(k, 195, k->t_i32, si, i < 3 ? c22 : c30);
            uint32_t fl = rdna4_tcs_op1(k, 111, k->t_f32, sx);
            uint32_t scaled = (i < 3) ? rdna4_tcs_op2(k, 133, k->t_f32, fl, inv10) : fl;
            lanes[i] = rdna4_tcs_ext2(k, k->t_f32, 40, scaled, k->c_f32_neg1); // FMax(..., -1.0)
        }
        break;
    }
    }
    return rdna4_tcs_convert_attr(k, lanes, fi.comps, fi.raw_scalar, in->tes_scalar, in->tes_comps);
}

// Emits TessellationControl SPIR-V reading DrawParams SSBO + BDA vertex/index/factor buffers.
static inline int rdna4_tess_emit_tcs_spv(const rdna4_tess_cfg_t *c, uint32_t **out_words, size_t *out_nwords) {
    if (!c || !out_words || !out_nwords) return 0;
    rdna4_tcs_ctx_t ctx = {0};
    rdna4_tcs_ctx_t *k = &ctx;
    k->next_id = 1;
    uint32_t fn_main = rdna4_tcs_id(k);
    k->ext_glsl = rdna4_tcs_id(k);

    // Core types & constants
    k->t_void = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 19, 2); rdna4_tb_push(&k->types, k->t_void);
    k->t_bool = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 20, 2); rdna4_tb_push(&k->types, k->t_bool);
    k->t_u32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_u32);  rdna4_tb_push(&k->types, 32); rdna4_tb_push(&k->types, 0);
    k->t_i32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_i32);  rdna4_tb_push(&k->types, 32); rdna4_tb_push(&k->types, 1);
    k->t_u64  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 21, 4); rdna4_tb_push(&k->types, k->t_u64);  rdna4_tb_push(&k->types, 64); rdna4_tb_push(&k->types, 0);
    k->t_f32  = rdna4_tcs_id(k); rdna4_tb_op(&k->types, 22, 3); rdna4_tb_push(&k->types, k->t_f32);  rdna4_tb_push(&k->types, 32);

    k->c_u32_0 = rdna4_tcs_u32(k, 0); k->c_u32_1 = rdna4_tcs_u32(k, 1);
    k->c_u32_2 = rdna4_tcs_u32(k, 2); k->c_u32_3 = rdna4_tcs_u32(k, 3);
    k->c_u32_4 = rdna4_tcs_u32(k, 4); k->c_u32_8 = rdna4_tcs_u32(k, 8);
    k->c_u32_16 = rdna4_tcs_u32(k, 16); k->c_u32_24 = rdna4_tcs_u32(k, 24); k->c_u32_32 = rdna4_tcs_u32(k, 32);
    k->c_u32_ff = rdna4_tcs_u32(k, 0xffu); k->c_u32_ffff = rdna4_tcs_u32(k, 0xffffu);
    k->c_u32_ffffff = rdna4_tcs_u32(k, 0xffffffu); k->c_u32_all = rdna4_tcs_u32(k, 0xffffffffu);
    k->c_u64_0 = rdna4_tcs_u64(k, 0); k->c_u64_2 = rdna4_tcs_u64(k, 2); k->c_u64_4 = rdna4_tcs_u64(k, 4);
    k->c_f32_0 = rdna4_tcs_f32(k, 0.0f); k->c_f32_1 = rdna4_tcs_f32(k, 1.0f);
    k->c_f32_neg1 = rdna4_tcs_f32(k, -1.0f); k->c_f32_max = rdna4_tcs_f32(k, c->max_tess_factor);

    // Emit load_le32 helper before main so call precedes use cleanly.
    rdna4_tcs_emit_load32_fn(k);

    uint32_t t_fn_void = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 33, 3); rdna4_tb_push(&k->types, t_fn_void); rdna4_tb_push(&k->types, k->t_void);

    // DrawParams SSBO type (StorageBuffer = 12, Set = tess_set, Binding = 0, NonWritable)
    uint32_t c_u32_5 = rdna4_tcs_u32(k, 5), c_u32_6 = rdna4_tcs_u32(k, 6), c_u32_7 = rdna4_tcs_u32(k, 7);
    uint32_t c_u32_9 = rdna4_tcs_u32(k, 9), c_u32_10 = rdna4_tcs_u32(k, 10);
    uint32_t c_u32_31 = rdna4_tcs_u32(k, RDNA4_TESS_MAX_VBUFS);
    uint32_t t_arr_u64_31 = rdna4_tcs_arr(k, k->t_u64, c_u32_31);
    rdna4_tb_dec(&k->dec, t_arr_u64_31, 6, 1, 8); // ArrayStride 8
    uint32_t t_params = rdna4_tcs_id(k);
    rdna4_tb_op(&k->types, 30, 13); rdna4_tb_push(&k->types, t_params);
    for (int i = 0; i < 7; i++) rdna4_tb_push(&k->types, k->t_u64);
    rdna4_tb_push(&k->types, k->t_f32); rdna4_tb_push(&k->types, k->t_u32);
    rdna4_tb_push(&k->types, t_arr_u64_31); rdna4_tb_push(&k->types, t_arr_u64_31);
    rdna4_tb_dec(&k->dec, t_params, 2, 0, 0); // Block
    static const uint32_t moffs[11] = { 0, 8, 16, 24, 32, 40, 48, 56, 60, 64, 312 };
    for (uint32_t i = 0; i < 11; i++) rdna4_tb_mdec(&k->dec, t_params, i, 35, moffs[i]);
    uint32_t p_sb_params = rdna4_tcs_ptr(k, 12, t_params);
    uint32_t p_sb_u64 = rdna4_tcs_ptr(k, 12, k->t_u64);
    uint32_t p_sb_f32 = rdna4_tcs_ptr(k, 12, k->t_f32);
    uint32_t p_sb_u32 = rdna4_tcs_ptr(k, 12, k->t_u32);
    uint32_t v_params = rdna4_tcs_var(k, p_sb_params, 12);
    rdna4_tb_dec(&k->dec, v_params, 34, 1, c->tess_set); // DescriptorSet
    rdna4_tb_dec(&k->dec, v_params, 33, 1, 0);           // Binding 0
    rdna4_tb_dec(&k->dec, v_params, 24, 0, 0);           // NonWritable

    // Built-in & VS->TCS inputs
    uint32_t p_in_u32 = rdna4_tcs_ptr(k, 1, k->t_u32);
    uint32_t t_arr_u32_1 = rdna4_tcs_arr(k, k->t_u32, k->c_u32_1);
    uint32_t p_in_arr1 = rdna4_tcs_ptr(k, 1, t_arr_u32_1);
    uint32_t v_in_patch = rdna4_tcs_var(k, p_in_arr1, 1); rdna4_tb_dec(&k->dec, v_in_patch, 30, 1, 0);
    uint32_t v_in_inst = rdna4_tcs_var(k, p_in_arr1, 1);  rdna4_tb_dec(&k->dec, v_in_inst, 30, 1, 1);
    uint32_t v_inv = rdna4_tcs_var(k, p_in_u32, 1);       rdna4_tb_dec(&k->dec, v_inv, 11, 1, 8); // InvocationId

    // TessLevelOuter[4], TessLevelInner[2]
    uint32_t p_out_f32 = rdna4_tcs_ptr(k, 3, k->t_f32);
    uint32_t t_outer = rdna4_tcs_arr(k, k->t_f32, k->c_u32_4);
    uint32_t t_inner = rdna4_tcs_arr(k, k->t_f32, k->c_u32_2);
    uint32_t v_outer = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, t_outer), 3);
    rdna4_tb_dec(&k->dec, v_outer, 11, 1, 11); rdna4_tb_dec(&k->dec, v_outer, 15, 0, 0);
    uint32_t v_inner = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, t_inner), 3);
    rdna4_tb_dec(&k->dec, v_inner, 11, 1, 12); rdna4_tb_dec(&k->dec, v_inner, 15, 0, 0);

    // System per-patch outputs for [[patch_id]] / [[instance_id]]
    uint32_t v_out_patch_id = 0, v_out_inst_id = 0;
    if (c->has_patch_id) {
        v_out_patch_id = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, k->t_u32), 3);
        rdna4_tb_dec(&k->dec, v_out_patch_id, 30, 1, c->patch_id_loc);
        rdna4_tb_dec(&k->dec, v_out_patch_id, 15, 0, 0);
    }
    if (c->has_instance_id) {
        uint32_t it = rdna4_tcs_val_type(k, c->instance_id_scalar, c->instance_id_comps);
        v_out_inst_id = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, it), 3);
        rdna4_tb_dec(&k->dec, v_out_inst_id, 30, 1, c->instance_id_loc);
        rdna4_tb_dec(&k->dec, v_out_inst_id, 15, 0, 0);
    }

    uint32_t out_cp_count = c->control_point_count ? c->control_point_count : 1u;
    uint32_t c_out_cp = (out_cp_count == 1u) ? k->c_u32_1 : rdna4_tcs_u32(k, out_cp_count);
    uint32_t v_out_user[RDNA4_TESS_MAX_INPUTS] = {0};
    uint32_t p_out_elem[RDNA4_TESS_MAX_INPUTS] = {0};
    for (uint32_t i = 0; i < c->num_inputs; i++) {
        uint32_t vt = rdna4_tcs_val_type(k, c->inputs[i].tes_scalar, c->inputs[i].tes_comps);
        uint32_t pe = rdna4_tcs_ptr(k, 3, vt);
        p_out_elem[i] = pe;
        if (c->inputs[i].is_patch) {
            v_out_user[i] = rdna4_tcs_var(k, pe, 3);
            rdna4_tb_dec(&k->dec, v_out_user[i], 30, 1, c->inputs[i].location);
            rdna4_tb_dec(&k->dec, v_out_user[i], 15, 0, 0);
        } else {
            uint32_t at = rdna4_tcs_arr(k, vt, c_out_cp);
            v_out_user[i] = rdna4_tcs_var(k, rdna4_tcs_ptr(k, 3, at), 3);
            rdna4_tb_dec(&k->dec, v_out_user[i], 30, 1, c->inputs[i].location);
        }
    }

    // ---- main() ----
    uint32_t m_entry = rdna4_tcs_id(k), m_inv0 = rdna4_tcs_id(k), m_inv0_end = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 54, 5);
    rdna4_tb_push(&k->code, k->t_void); rdna4_tb_push(&k->code, fn_main); rdna4_tb_push(&k->code, 0); rdna4_tb_push(&k->code, t_fn_void);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, m_entry);

    uint32_t ac_dp = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_in_u32); rdna4_tb_push(&k->code, ac_dp); rdna4_tb_push(&k->code, v_in_patch); rdna4_tb_push(&k->code, k->c_u32_0);
    uint32_t draw_patch_idx = rdna4_tcs_op1(k, 61, k->t_u32, ac_dp);
    uint32_t ac_inst = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_in_u32); rdna4_tb_push(&k->code, ac_inst); rdna4_tb_push(&k->code, v_in_inst); rdna4_tb_push(&k->code, k->c_u32_0);
    uint32_t inst_id = rdna4_tcs_op1(k, 61, k->t_u32, ac_inst);
    uint32_t inv = rdna4_tcs_op1(k, 61, k->t_u32, v_inv);

    // Load patch_idx_bda / patch_idx_size and compute patchIndex
    #define SB_LOAD_U64(field_c, out_var) do { \
        uint32_t ac_ = rdna4_tcs_id(k); \
        rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_sb_u64); rdna4_tb_push(&k->code, ac_); rdna4_tb_push(&k->code, v_params); rdna4_tb_push(&k->code, field_c); \
        out_var = rdna4_tcs_op1(k, 61, k->t_u64, ac_); \
    } while (0)
    uint32_t pid_bda, pid_sz;
    SB_LOAD_U64(k->c_u32_3, pid_bda);
    SB_LOAD_U64(k->c_u32_4, pid_sz);
    uint32_t has_pid_buf = rdna4_tcs_op2(k, 171, k->t_bool, pid_bda, k->c_u64_0);
    uint32_t b_pid_t = rdna4_tcs_id(k), b_pid_m = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, b_pid_m); rdna4_tb_push(&k->code, 0);
    rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, has_pid_buf); rdna4_tb_push(&k->code, b_pid_t); rdna4_tb_push(&k->code, b_pid_m);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_pid_t);
    uint32_t dp64 = rdna4_tcs_op1(k, 113, k->t_u64, draw_patch_idx);
    uint32_t pid_off = rdna4_tcs_op2(k, 132, k->t_u64, dp64, k->c_u64_4);
    uint32_t fetched_pid = rdna4_tcs_load_le32(k, pid_bda, pid_sz, pid_off, k->c_u32_4);
    rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_pid_m);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_pid_m);
    uint32_t patch_idx = rdna4_tcs_id(k);
    rdna4_tb_op(&k->code, 245, 7); rdna4_tb_push(&k->code, k->t_u32); rdna4_tb_push(&k->code, patch_idx);
    rdna4_tb_push(&k->code, fetched_pid); rdna4_tb_push(&k->code, b_pid_t);
    rdna4_tb_push(&k->code, draw_patch_idx); rdna4_tb_push(&k->code, m_entry);

    // Helper: load vb_bda / vb_size for one slot once
    #define SB_LOAD_VB(slot, bda_v, sz_v) do { \
        uint32_t s_c = rdna4_tcs_u32(k, (slot)); \
        uint32_t ac1 = rdna4_tcs_id(k), ac2 = rdna4_tcs_id(k); \
        rdna4_tb_op(&k->code, 65, 6); rdna4_tb_push(&k->code, p_sb_u64); rdna4_tb_push(&k->code, ac1); rdna4_tb_push(&k->code, v_params); rdna4_tb_push(&k->code, c_u32_9); rdna4_tb_push(&k->code, s_c); \
        rdna4_tb_op(&k->code, 65, 6); rdna4_tb_push(&k->code, p_sb_u64); rdna4_tb_push(&k->code, ac2); rdna4_tb_push(&k->code, v_params); rdna4_tb_push(&k->code, c_u32_10); rdna4_tb_push(&k->code, s_c); \
        bda_v = rdna4_tcs_op1(k, 61, k->t_u64, ac1); \
        sz_v  = rdna4_tcs_op1(k, 61, k->t_u64, ac2); \
    } while (0)

    // Invocation 0: TessLevels + patch_id + instance_id + per-patch user attributes
    uint32_t is_inv0 = rdna4_tcs_op2(k, 170, k->t_bool, inv, k->c_u32_0);
    rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, m_inv0_end); rdna4_tb_push(&k->code, 0);
    rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, is_inv0); rdna4_tb_push(&k->code, m_inv0); rdna4_tb_push(&k->code, m_inv0_end);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, m_inv0);

    uint32_t f_bda, f_sz, f_stride;
    SB_LOAD_U64(k->c_u32_0, f_bda);
    SB_LOAD_U64(k->c_u32_1, f_sz);
    SB_LOAD_U64(k->c_u32_2, f_stride);
    uint32_t scale_f = k->c_f32_1;
    if (c->factor_scale_enabled) {
        uint32_t ac_sc = rdna4_tcs_id(k);
        rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_sb_f32); rdna4_tb_push(&k->code, ac_sc); rdna4_tb_push(&k->code, v_params); rdna4_tb_push(&k->code, c_u32_7);
        scale_f = rdna4_tcs_op1(k, 61, k->t_f32, ac_sc);
    }
    uint32_t dp_u64 = rdna4_tcs_op1(k, 113, k->t_u64, draw_patch_idx);
    uint32_t inst_u64 = rdna4_tcs_op1(k, 113, k->t_u64, inst_id);
    uint32_t f_bytes_c = rdna4_tcs_u64(k, rdna4_tess_factor_stride(c));
    uint32_t f_off = k->c_u64_0;
    if (c->factor_step == RDNA4_TESS_FSTEP_PER_PATCH)
        f_off = rdna4_tcs_op2(k, 132, k->t_u64, dp_u64, f_bytes_c);
    else if (c->factor_step == RDNA4_TESS_FSTEP_PER_INSTANCE)
        f_off = rdna4_tcs_op2(k, 132, k->t_u64, inst_u64, f_stride);
    else if (c->factor_step == RDNA4_TESS_FSTEP_PER_PATCH_AND_INSTANCE) {
        uint32_t io = rdna4_tcs_op2(k, 132, k->t_u64, inst_u64, f_stride);
        uint32_t po = rdna4_tcs_op2(k, 132, k->t_u64, dp_u64, f_bytes_c);
        f_off = rdna4_tcs_op2(k, 128, k->t_u64, io, po);
    }

    uint32_t nf_words = (c->domain == RDNA4_TESS_DOM_QUAD) ? 3u : 2u;
    uint32_t t_v2f = rdna4_tcs_val_type(k, RDNA4_TSCALAR_F32, 2);
    uint32_t tf_vals[6];
    int is_pow2 = (c->partition == RDNA4_TESS_PART_POW2);
    for (uint32_t w = 0; w < nf_words; w++) {
        uint32_t wo = w ? rdna4_tcs_op2(k, 128, k->t_u64, f_off, rdna4_tcs_u64(k, w * 4u)) : f_off;
        uint32_t pk = rdna4_tcs_load_le32(k, f_bda, f_sz, wo, k->c_u32_4);
        uint32_t unp = rdna4_tcs_ext1(k, t_v2f, 62, pk); // UnpackHalf2x16
        for (uint32_t sub = 0; sub < 2; sub++) {
            uint32_t raw = rdna4_tcs_id(k);
            rdna4_tb_op(&k->code, 81, 5); rdna4_tb_push(&k->code, k->t_f32); rdna4_tb_push(&k->code, raw); rdna4_tb_push(&k->code, unp); rdna4_tb_push(&k->code, sub);
            tf_vals[w * 2u + sub] = rdna4_tcs_adjust_factor(k, raw, scale_f, k->c_f32_max, is_pow2);
        }
    }
    uint32_t outer_vals[4] = { tf_vals[0], tf_vals[1], tf_vals[2], (c->domain == RDNA4_TESS_DOM_QUAD) ? tf_vals[3] : k->c_f32_0 };
    uint32_t inner_vals[2] = { (c->domain == RDNA4_TESS_DOM_QUAD) ? tf_vals[4] : tf_vals[3],
                               (c->domain == RDNA4_TESS_DOM_QUAD) ? tf_vals[5] : k->c_f32_0 };
    uint32_t idx_c[4] = { k->c_u32_0, k->c_u32_1, k->c_u32_2, k->c_u32_3 };
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ac = rdna4_tcs_id(k);
        rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_out_f32); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, v_outer); rdna4_tb_push(&k->code, idx_c[i]);
        rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, outer_vals[i]);
    }
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t ac = rdna4_tcs_id(k);
        rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_out_f32); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, v_inner); rdna4_tb_push(&k->code, idx_c[i]);
        rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, inner_vals[i]);
    }
    if (c->has_patch_id) { rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, v_out_patch_id); rdna4_tb_push(&k->code, patch_idx); }
    if (c->has_instance_id) {
        uint32_t iv = rdna4_tcs_convert_attr(k, &inst_id, 1, RDNA4_TSCALAR_U32, c->instance_id_scalar, c->instance_id_comps);
        rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, v_out_inst_id); rdna4_tb_push(&k->code, iv);
    }
    for (uint32_t i = 0; i < c->num_inputs; i++) {
        const rdna4_tess_input_t *in = &c->inputs[i];
        if (!in->is_patch) continue;
        uint32_t elem_idx = k->c_u64_0;
        if (in->has_attr && in->step_func == RDNA4_TESS_VSTEP_PER_PATCH) {
            elem_idx = rdna4_tcs_op1(k, 113, k->t_u64, patch_idx);
            if (in->step_rate > 1) elem_idx = rdna4_tcs_op2(k, 134, k->t_u64, elem_idx, rdna4_tcs_u64(k, in->step_rate));
        }
        uint32_t vb = k->c_u64_0, vs = k->c_u64_0;
        if (in->has_attr) SB_LOAD_VB(in->buf_idx, vb, vs);
        uint32_t val = rdna4_tcs_fetch_and_unpack(k, in, vb, vs, elem_idx);
        rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, v_out_user[i]); rdna4_tb_push(&k->code, val);
    }
    rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, m_inv0_end);
    rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, m_inv0_end);

    // Per-control-point outputs
    int any_cp = 0;
    for (uint32_t i = 0; i < c->num_inputs; i++) if (!c->inputs[i].is_patch) { any_cp = 1; break; }
    if (any_cp && c->control_point_count > 0) {
        uint32_t ac_ncp = rdna4_tcs_id(k);
        rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_sb_u32); rdna4_tb_push(&k->code, ac_ncp); rdna4_tb_push(&k->code, v_params); rdna4_tb_push(&k->code, k->c_u32_8);
        uint32_t num_cp = rdna4_tcs_op1(k, 61, k->t_u32, ac_ncp);
        uint32_t inv_ok = rdna4_tcs_op2(k, 176, k->t_bool, inv, num_cp);
        uint32_t b_cp_t = rdna4_tcs_id(k), b_cp_f = rdna4_tcs_id(k), b_cp_m = rdna4_tcs_id(k);
        rdna4_tb_op(&k->code, 247, 3); rdna4_tb_push(&k->code, b_cp_m); rdna4_tb_push(&k->code, 0);
        rdna4_tb_op(&k->code, 250, 4); rdna4_tb_push(&k->code, inv_ok); rdna4_tb_push(&k->code, b_cp_t); rdna4_tb_push(&k->code, b_cp_f);

        rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_cp_t);
        uint32_t linear_cp = rdna4_tcs_op2(k, 128, k->t_u64,
            rdna4_tcs_op2(k, 132, k->t_u64, rdna4_tcs_op1(k, 113, k->t_u64, patch_idx), rdna4_tcs_op1(k, 113, k->t_u64, num_cp)),
            rdna4_tcs_op1(k, 113, k->t_u64, inv));
        uint32_t cp_idx = linear_cp;
        if (c->cp_idx_type == RDNA4_TESS_CPIDX_U16 || c->cp_idx_type == RDNA4_TESS_CPIDX_U32) {
            uint32_t cp_bda, cp_sz;
            SB_LOAD_U64(c_u32_5, cp_bda);
            SB_LOAD_U64(c_u32_6, cp_sz);
            uint32_t stride64 = (c->cp_idx_type == RDNA4_TESS_CPIDX_U16) ? k->c_u64_2 : k->c_u64_4;
            uint32_t l_c = (c->cp_idx_type == RDNA4_TESS_CPIDX_U16) ? k->c_u32_2 : k->c_u32_4;
            uint32_t cp_off = rdna4_tcs_op2(k, 132, k->t_u64, linear_cp, stride64);
            uint32_t raw_idx = rdna4_tcs_load_le32(k, cp_bda, cp_sz, cp_off, l_c);
            uint32_t has_cpb = rdna4_tcs_op2(k, 171, k->t_bool, cp_bda, k->c_u64_0);
            cp_idx = rdna4_tcs_sel(k, k->t_u64, has_cpb, rdna4_tcs_op1(k, 113, k->t_u64, raw_idx), linear_cp);
        }
        uint32_t p64 = rdna4_tcs_op1(k, 113, k->t_u64, patch_idx);
        for (uint32_t i = 0; i < c->num_inputs; i++) {
            const rdna4_tess_input_t *in = &c->inputs[i];
            if (in->is_patch) continue;
            uint32_t elem_idx = k->c_u64_0;
            if (in->has_attr && in->step_func == RDNA4_TESS_VSTEP_PER_PATCH) {
                elem_idx = p64;
                if (in->step_rate > 1) elem_idx = rdna4_tcs_op2(k, 134, k->t_u64, elem_idx, rdna4_tcs_u64(k, in->step_rate));
            } else if (in->has_attr && in->step_func == RDNA4_TESS_VSTEP_PER_PATCH_CP) {
                elem_idx = cp_idx;
                if (in->step_rate > 1) elem_idx = rdna4_tcs_op2(k, 134, k->t_u64, elem_idx, rdna4_tcs_u64(k, in->step_rate));
            }
            uint32_t vb = k->c_u64_0, vs = k->c_u64_0;
            if (in->has_attr) SB_LOAD_VB(in->buf_idx, vb, vs);
            uint32_t val = rdna4_tcs_fetch_and_unpack(k, in, vb, vs, elem_idx);
            uint32_t ac = rdna4_tcs_id(k);
            rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_out_elem[i]); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, v_out_user[i]); rdna4_tb_push(&k->code, inv);
            rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, val);
        }
        rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_cp_m);

        rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_cp_f);
        for (uint32_t i = 0; i < c->num_inputs; i++) {
            if (c->inputs[i].is_patch) continue;
            uint32_t zv = rdna4_tcs_zero(k, c->inputs[i].tes_scalar, c->inputs[i].tes_comps);
            uint32_t ac = rdna4_tcs_id(k);
            rdna4_tb_op(&k->code, 65, 5); rdna4_tb_push(&k->code, p_out_elem[i]); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, v_out_user[i]); rdna4_tb_push(&k->code, inv);
            rdna4_tb_op(&k->code, 62, 3); rdna4_tb_push(&k->code, ac); rdna4_tb_push(&k->code, zv);
        }
        rdna4_tb_op(&k->code, 249, 2); rdna4_tb_push(&k->code, b_cp_m);
        rdna4_tb_op(&k->code, 248, 2); rdna4_tb_push(&k->code, b_cp_m);
    }

    #undef SB_LOAD_U64
    #undef SB_LOAD_VB

    rdna4_tb_op(&k->code, 253, 1);
    rdna4_tb_op(&k->code, 56, 1);

    // Assemble full module
    rdna4_tspv_buf_t mod = {0};
    rdna4_tb_push(&mod, 0x07230203u); rdna4_tb_push(&mod, 0x00010500u); rdna4_tb_push(&mod, 0);
    rdna4_tb_push(&mod, k->next_id); rdna4_tb_push(&mod, 0);
    // OpCapability
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 1);    // Shader
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 3);    // Tessellation
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 11);   // Int64
    rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 5347); // PhysicalStorageBufferAddresses
    if (k->need_f16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 9); }    // Float16
    if (k->need_i16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 22); }   // Int16
    if (k->need_f16 || k->need_i16) { rdna4_tb_op(&mod, 17, 2); rdna4_tb_push(&mod, 4436); } // StorageInputOutput16
    // OpExtension
    rdna4_tb_op(&mod, 10, (uint16_t)(1 + rdna4_tb_str_words("SPV_KHR_physical_storage_buffer")));
    rdna4_tb_str(&mod, "SPV_KHR_physical_storage_buffer");
    // OpExtInstImport
    rdna4_tb_op(&mod, 11, (uint16_t)(2 + rdna4_tb_str_words("GLSL.std.450")));
    rdna4_tb_push(&mod, k->ext_glsl); rdna4_tb_str(&mod, "GLSL.std.450");
    // OpMemoryModel PhysicalStorageBuffer64 GLSL450
    rdna4_tb_op(&mod, 14, 3); rdna4_tb_push(&mod, 5348); rdna4_tb_push(&mod, 1);
    // OpEntryPoint TessellationControl main iface...
    rdna4_tb_op(&mod, 15, (uint16_t)(3 + rdna4_tb_str_words("main") + k->niface));
    rdna4_tb_push(&mod, 1); rdna4_tb_push(&mod, fn_main); rdna4_tb_str(&mod, "main");
    for (uint32_t i = 0; i < k->niface; i++) rdna4_tb_push(&mod, k->iface[i]);
    // OpExecutionMode OutputVertices
    rdna4_tb_op(&mod, 16, 4); rdna4_tb_push(&mod, fn_main); rdna4_tb_push(&mod, 26); rdna4_tb_push(&mod, out_cp_count);

    rdna4_tb_append(&mod, &k->dec);
    rdna4_tb_append(&mod, &k->types);
    rdna4_tb_append(&mod, &k->vars);
    rdna4_tb_append(&mod, &k->code);

    rdna4_tb_free(&k->dec); rdna4_tb_free(&k->types); rdna4_tb_free(&k->vars); rdna4_tb_free(&k->code);
    if (mod.oom) { rdna4_tb_free(&mod); return 0; }
    *out_words = mod.w; *out_nwords = mod.len;
    return 1;
}

#endif
