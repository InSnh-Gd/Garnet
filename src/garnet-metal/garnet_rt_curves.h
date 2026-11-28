/* garnet_rt_curves.h: Metal 3.1 curve geometry validation, basis conversion, and CPU solver. */
#ifndef GARNET_RT_CURVES_H
#define GARNET_RT_CURVES_H

#ifndef RDNA4_RT_ACCEL_H
#include "garnet_rt_accel.h"
#endif

/* =========================================================================
 * Wave 2.1: Metal 3.1 Curve Geometry Validation, Basis Conversion & Solver
 * ========================================================================= */

/* Exact Apple Metal SDK enums (verified against macOS 26.7.2 Metal.framework). */
typedef enum {
    RDNA4RT_CURVE_TYPE_ROUND = 0u,
    RDNA4RT_CURVE_TYPE_FLAT  = 1u,
} rdna4rt_curve_type_t;

typedef enum {
    RDNA4RT_CURVE_BASIS_BSPLINE     = 0u,
    RDNA4RT_CURVE_BASIS_CATMULL_ROM = 1u,
    RDNA4RT_CURVE_BASIS_LINEAR      = 2u,
    RDNA4RT_CURVE_BASIS_BEZIER      = 3u,
} rdna4rt_curve_basis_t;

typedef enum {
    RDNA4RT_CURVE_ENDCAPS_NONE   = 0u,
    RDNA4RT_CURVE_ENDCAPS_DISK   = 1u,
    RDNA4RT_CURVE_ENDCAPS_SPHERE = 2u,
} rdna4rt_curve_endcaps_t;

typedef enum {
    RDNA4RT_CURVE_CP_FLOAT2 = 0u,
    RDNA4RT_CURVE_CP_FLOAT3 = 1u,
    RDNA4RT_CURVE_CP_FLOAT4 = 2u,
    RDNA4RT_CURVE_CP_HALF2  = 3u,
    RDNA4RT_CURVE_CP_HALF3  = 4u,
    RDNA4RT_CURVE_CP_HALF4  = 5u,
} rdna4rt_curve_cp_fmt_t;

typedef enum {
    RDNA4RT_CURVE_RAD_FLOAT = 0u,
    RDNA4RT_CURVE_RAD_HALF  = 1u,
} rdna4rt_curve_rad_fmt_t;

typedef struct {
    float x, y, z, w;
} rdna4rt_vec4_t;

static inline int rdna4rt_curve_cp_fmt_info(rdna4rt_curve_cp_fmt_t fmt,
                                            uint32_t *elem_bytes_out,
                                            uint32_t *align_out,
                                            uint32_t *comps_out) {
    uint32_t sz = 0u, al = 0u, nc = 0u;
    switch (fmt) {
    case RDNA4RT_CURVE_CP_FLOAT2: sz = 8u;  al = 4u; nc = 2u; break;
    case RDNA4RT_CURVE_CP_FLOAT3: sz = 12u; al = 4u; nc = 3u; break;
    case RDNA4RT_CURVE_CP_FLOAT4: sz = 16u; al = 4u; nc = 4u; break;
    case RDNA4RT_CURVE_CP_HALF2:  sz = 4u;  al = 2u; nc = 2u; break;
    case RDNA4RT_CURVE_CP_HALF3:  sz = 6u;  al = 2u; nc = 3u; break;
    case RDNA4RT_CURVE_CP_HALF4:  sz = 8u;  al = 2u; nc = 4u; break;
    default: return 0;
    }
    if (elem_bytes_out) *elem_bytes_out = sz;
    if (align_out) *align_out = al;
    if (comps_out) *comps_out = nc;
    return 1;
}

static inline int rdna4rt_curve_rad_fmt_info(rdna4rt_curve_rad_fmt_t fmt,
                                             uint32_t *elem_bytes_out,
                                             uint32_t *align_out) {
    uint32_t sz = 0u, al = 0u;
    switch (fmt) {
    case RDNA4RT_CURVE_RAD_FLOAT: sz = 4u; al = 4u; break;
    case RDNA4RT_CURVE_RAD_HALF:  sz = 2u; al = 2u; break;
    default: return 0;
    }
    if (elem_bytes_out) *elem_bytes_out = sz;
    if (align_out) *align_out = al;
    return 1;
}

static inline int rdna4rt_curve_valid_seg_cp_count(rdna4rt_curve_basis_t basis,
                                                   uint32_t seg_cp_count) {
    switch (basis) {
    case RDNA4RT_CURVE_BASIS_LINEAR:      return seg_cp_count == 2u;
    case RDNA4RT_CURVE_BASIS_CATMULL_ROM: return seg_cp_count == 4u;
    case RDNA4RT_CURVE_BASIS_BSPLINE:
    case RDNA4RT_CURVE_BASIS_BEZIER:      return seg_cp_count == 3u || seg_cp_count == 4u;
    default:                              return 0;
    }
}

typedef struct {
    rdna4rt_curve_type_t curve_type;
    rdna4rt_curve_basis_t curve_basis;
    rdna4rt_curve_endcaps_t curve_end_caps;
    uint32_t segment_control_point_count;
    uint64_t segment_count;
    uint64_t control_point_count;
    rdna4rt_curve_cp_fmt_t control_point_format;
    int has_control_point_buf;
    uint64_t control_point_buf_gpu_addr;
    uint64_t control_point_buf_len;
    uint64_t control_point_buf_offset;
    uint64_t control_point_stride;
    rdna4rt_curve_rad_fmt_t radius_format;
    int has_radius_buf;
    uint64_t radius_buf_gpu_addr;
    uint64_t radius_buf_len;
    uint64_t radius_buf_offset;
    uint64_t radius_stride;
    int has_index_buf;
    rdna4rt_index_type_t index_type;
    uint64_t index_buf_gpu_addr;
    uint64_t index_buf_len;
    uint64_t index_buf_offset;
    int opaque;
    int allow_duplicate_any_hit;
} rdna4rt_curve_geom_t;

typedef struct {
    uint32_t cp_elem_bytes;
    uint32_t cp_align;
    uint64_t cp_stride;
    uint64_t cp_addr;
    uint32_t rad_elem_bytes;
    uint32_t rad_align;
    uint64_t rad_stride;
    uint64_t rad_addr;
    rdna4rt_index_type_t effective_index_type;
    uint64_t index_addr;
    uint32_t segment_count;
    uint32_t segment_cp_count;
    uint32_t vk_geom_flags;
} rdna4rt_curve_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_curve_geom_ex(
    const rdna4rt_curve_geom_t *g, uint64_t max_primitives,
    int require_buffers, rdna4rt_curve_plan_t *out) {
    if (!g) return RDNA4RT_ERR_INVALID_ARG;
    if (out) memset(out, 0, sizeof(*out));
    if (g->curve_type != RDNA4RT_CURVE_TYPE_ROUND && g->curve_type != RDNA4RT_CURVE_TYPE_FLAT)
        return RDNA4RT_ERR_INVALID_ARG;
    if ((uint32_t)g->curve_end_caps > RDNA4RT_CURVE_ENDCAPS_SPHERE)
        return RDNA4RT_ERR_INVALID_ARG;
    if (!rdna4rt_curve_valid_seg_cp_count(g->curve_basis, g->segment_control_point_count))
        return RDNA4RT_ERR_INVALID_ARG;

    uint32_t cp_elem = 0u, cp_align = 0u, cp_comps = 0u;
    if (!rdna4rt_curve_cp_fmt_info(g->control_point_format, &cp_elem, &cp_align, &cp_comps))
        return RDNA4RT_ERR_UNSUPPORTED;
    uint32_t rad_elem = 0u, rad_align = 0u;
    if (!rdna4rt_curve_rad_fmt_info(g->radius_format, &rad_elem, &rad_align))
        return RDNA4RT_ERR_UNSUPPORTED;
    (void)cp_comps;

    if (g->segment_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (max_primitives > 0u && g->segment_count > max_primitives) return RDNA4RT_ERR_LIMIT;
    if (g->control_point_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;

    uint64_t cp_stride = g->control_point_stride ? g->control_point_stride : (uint64_t)cp_elem;
    if (cp_stride < (uint64_t)cp_elem || cp_stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((cp_stride % (uint64_t)cp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    uint64_t rad_stride = g->radius_stride ? g->radius_stride : (uint64_t)rad_elem;
    if (rad_stride < (uint64_t)rad_elem || rad_stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((rad_stride % (uint64_t)rad_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    rdna4rt_index_type_t idx_type = g->has_index_buf ? g->index_type : RDNA4RT_INDEX_NONE;
    uint64_t idx_elem = 0u;
    if (idx_type == RDNA4RT_INDEX_UINT16) idx_elem = 2u;
    else if (idx_type == RDNA4RT_INDEX_UINT32) idx_elem = 4u;
    else if (idx_type != RDNA4RT_INDEX_NONE) return RDNA4RT_ERR_INVALID_ARG;

    uint64_t cp_addr = 0u, rad_addr = 0u, idx_addr = 0u;
    if (g->segment_count > 0u) {
        if (idx_type == RDNA4RT_INDEX_NONE) {
            uint64_t needed_cp = 0u;
            if (!rdna4rt_mul_u64(g->segment_count, (uint64_t)g->segment_control_point_count, &needed_cp))
                return RDNA4RT_ERR_OVERFLOW;
            if (g->control_point_count < needed_cp) return RDNA4RT_ERR_BOUNDS;
        } else {
            if (g->control_point_count < (uint64_t)g->segment_control_point_count)
                return RDNA4RT_ERR_BOUNDS;
            if (idx_type == RDNA4RT_INDEX_UINT16 && g->control_point_count > (uint64_t)UINT16_MAX + 1u)
                return RDNA4RT_ERR_LIMIT;
        }

        if (require_buffers && (!g->has_control_point_buf || !g->has_radius_buf))
            return RDNA4RT_ERR_INVALID_ARG;

        if (g->has_control_point_buf) {
            if (require_buffers && g->control_point_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (g->control_point_buf_offset > g->control_point_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((g->control_point_buf_offset % (uint64_t)cp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (g->control_point_buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(g->control_point_buf_gpu_addr, g->control_point_buf_offset, &cp_addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((cp_addr % (uint64_t)cp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t span = 0u, need = 0u;
            if (!rdna4rt_mul_u64(g->control_point_count - 1u, cp_stride, &span) ||
                !rdna4rt_add_u64(span, (uint64_t)cp_elem, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > g->control_point_buf_len - g->control_point_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }

        if (g->has_radius_buf) {
            if (require_buffers && g->radius_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (g->radius_buf_offset > g->radius_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((g->radius_buf_offset % (uint64_t)rad_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (g->radius_buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(g->radius_buf_gpu_addr, g->radius_buf_offset, &rad_addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((rad_addr % (uint64_t)rad_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t span = 0u, need = 0u;
            if (!rdna4rt_mul_u64(g->control_point_count - 1u, rad_stride, &span) ||
                !rdna4rt_add_u64(span, (uint64_t)rad_elem, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > g->radius_buf_len - g->radius_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }

        if (g->has_index_buf) {
            if (require_buffers && g->index_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (g->index_buf_offset > g->index_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((g->index_buf_offset % idx_elem) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (g->index_buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(g->index_buf_gpu_addr, g->index_buf_offset, &idx_addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((idx_addr % idx_elem) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t idx_bytes = 0u;
            if (!rdna4rt_mul_u64(g->segment_count, idx_elem, &idx_bytes)) return RDNA4RT_ERR_OVERFLOW;
            if (idx_bytes > g->index_buf_len - g->index_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
    }

    if (out) {
        out->cp_elem_bytes = cp_elem;
        out->cp_align = cp_align;
        out->cp_stride = cp_stride;
        out->cp_addr = cp_addr;
        out->rad_elem_bytes = rad_elem;
        out->rad_align = rad_align;
        out->rad_stride = rad_stride;
        out->rad_addr = rad_addr;
        out->effective_index_type = idx_type;
        out->index_addr = idx_addr;
        out->segment_count = (uint32_t)g->segment_count;
        out->segment_cp_count = g->segment_control_point_count;
        out->vk_geom_flags = rdna4rt_geom_flags_to_vk(g->opaque, g->allow_duplicate_any_hit);
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_validate_curve_geom(const rdna4rt_curve_geom_t *g,
                                                        uint64_t max_primitives) {
    return rdna4rt_validate_curve_geom_ex(g, max_primitives, 1, NULL);
}

static inline rdna4rt_vec4_t rdna4rt_v4_lerp(rdna4rt_vec4_t a, rdna4rt_vec4_t b, float t) {
    return (rdna4rt_vec4_t){
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t,
    };
}

static inline rdna4rt_err_t rdna4rt_curve_to_bezier_4(rdna4rt_curve_basis_t basis,
                                                      uint32_t cp_count,
                                                      const rdna4rt_vec4_t *cps,
                                                      rdna4rt_vec4_t out_b[4]) {
    if (!cps || !out_b || !rdna4rt_curve_valid_seg_cp_count(basis, cp_count))
        return RDNA4RT_ERR_INVALID_ARG;
    for (uint32_t i = 0u; i < cp_count; i++) {
        if (!isfinite(cps[i].x) || !isfinite(cps[i].y) ||
            !isfinite(cps[i].z) || !isfinite(cps[i].w))
            return RDNA4RT_ERR_INVALID_ARG;
    }

    switch (basis) {
    case RDNA4RT_CURVE_BASIS_LINEAR:
        out_b[0] = cps[0];
        out_b[1] = rdna4rt_v4_lerp(cps[0], cps[1], 1.0f / 3.0f);
        out_b[2] = rdna4rt_v4_lerp(cps[0], cps[1], 2.0f / 3.0f);
        out_b[3] = cps[1];
        return RDNA4RT_OK;

    case RDNA4RT_CURVE_BASIS_BEZIER:
        if (cp_count == 3u) {
            out_b[0] = cps[0];
            out_b[1] = rdna4rt_v4_lerp(cps[0], cps[1], 2.0f / 3.0f);
            out_b[2] = rdna4rt_v4_lerp(cps[2], cps[1], 2.0f / 3.0f);
            out_b[3] = cps[2];
        } else {
            for (uint32_t i = 0u; i < 4u; i++) out_b[i] = cps[i];
        }
        return RDNA4RT_OK;

    case RDNA4RT_CURVE_BASIS_BSPLINE:
        if (cp_count == 3u) {
            /* Quadratic uniform B-spline [Q0,Q1,Q2] -> degree-elevated cubic Bezier. */
            const float inv6 = 1.0f / 6.0f;
            out_b[0] = (rdna4rt_vec4_t){
                0.5f * (cps[0].x + cps[1].x), 0.5f * (cps[0].y + cps[1].y),
                0.5f * (cps[0].z + cps[1].z), 0.5f * (cps[0].w + cps[1].w),
            };
            out_b[1] = (rdna4rt_vec4_t){
                (cps[0].x + 5.0f * cps[1].x) * inv6, (cps[0].y + 5.0f * cps[1].y) * inv6,
                (cps[0].z + 5.0f * cps[1].z) * inv6, (cps[0].w + 5.0f * cps[1].w) * inv6,
            };
            out_b[2] = (rdna4rt_vec4_t){
                (5.0f * cps[1].x + cps[2].x) * inv6, (5.0f * cps[1].y + cps[2].y) * inv6,
                (5.0f * cps[1].z + cps[2].z) * inv6, (5.0f * cps[1].w + cps[2].w) * inv6,
            };
            out_b[3] = (rdna4rt_vec4_t){
                0.5f * (cps[1].x + cps[2].x), 0.5f * (cps[1].y + cps[2].y),
                0.5f * (cps[1].z + cps[2].z), 0.5f * (cps[1].w + cps[2].w),
            };
        } else {
            const float inv6 = 1.0f / 6.0f;
            out_b[0] = (rdna4rt_vec4_t){
                (cps[0].x + 4.0f * cps[1].x + cps[2].x) * inv6,
                (cps[0].y + 4.0f * cps[1].y + cps[2].y) * inv6,
                (cps[0].z + 4.0f * cps[1].z + cps[2].z) * inv6,
                (cps[0].w + 4.0f * cps[1].w + cps[2].w) * inv6,
            };
            out_b[1] = (rdna4rt_vec4_t){
                (4.0f * cps[1].x + 2.0f * cps[2].x) * inv6,
                (4.0f * cps[1].y + 2.0f * cps[2].y) * inv6,
                (4.0f * cps[1].z + 2.0f * cps[2].z) * inv6,
                (4.0f * cps[1].w + 2.0f * cps[2].w) * inv6,
            };
            out_b[2] = (rdna4rt_vec4_t){
                (2.0f * cps[1].x + 4.0f * cps[2].x) * inv6,
                (2.0f * cps[1].y + 4.0f * cps[2].y) * inv6,
                (2.0f * cps[1].z + 4.0f * cps[2].z) * inv6,
                (2.0f * cps[1].w + 4.0f * cps[2].w) * inv6,
            };
            out_b[3] = (rdna4rt_vec4_t){
                (cps[1].x + 4.0f * cps[2].x + cps[3].x) * inv6,
                (cps[1].y + 4.0f * cps[2].y + cps[3].y) * inv6,
                (cps[1].z + 4.0f * cps[2].z + cps[3].z) * inv6,
                (cps[1].w + 4.0f * cps[2].w + cps[3].w) * inv6,
            };
        }
        return RDNA4RT_OK;

    case RDNA4RT_CURVE_BASIS_CATMULL_ROM: {
        const float inv6 = 1.0f / 6.0f;
        out_b[0] = cps[1];
        out_b[1] = (rdna4rt_vec4_t){
            cps[1].x + (cps[2].x - cps[0].x) * inv6,
            cps[1].y + (cps[2].y - cps[0].y) * inv6,
            cps[1].z + (cps[2].z - cps[0].z) * inv6,
            cps[1].w + (cps[2].w - cps[0].w) * inv6,
        };
        out_b[2] = (rdna4rt_vec4_t){
            cps[2].x - (cps[3].x - cps[1].x) * inv6,
            cps[2].y - (cps[3].y - cps[1].y) * inv6,
            cps[2].z - (cps[3].z - cps[1].z) * inv6,
            cps[2].w - (cps[3].w - cps[1].w) * inv6,
        };
        out_b[3] = cps[2];
        return RDNA4RT_OK;
    }

    default:
        return RDNA4RT_ERR_INVALID_ARG;
    }
}

static inline void rdna4rt_curve_eval_bezier(const rdna4rt_vec4_t b[4], float u,
                                             rdna4rt_vec4_t *pos_out,
                                             rdna4rt_vec4_t *deriv_out) {
    rdna4rt_vec4_t q0 = rdna4rt_v4_lerp(b[0], b[1], u);
    rdna4rt_vec4_t q1 = rdna4rt_v4_lerp(b[1], b[2], u);
    rdna4rt_vec4_t q2 = rdna4rt_v4_lerp(b[2], b[3], u);
    rdna4rt_vec4_t r0 = rdna4rt_v4_lerp(q0, q1, u);
    rdna4rt_vec4_t r1 = rdna4rt_v4_lerp(q1, q2, u);
    if (pos_out) *pos_out = rdna4rt_v4_lerp(r0, r1, u);
    if (deriv_out) {
        *deriv_out = (rdna4rt_vec4_t){
            3.0f * (r1.x - r0.x), 3.0f * (r1.y - r0.y),
            3.0f * (r1.z - r0.z), 3.0f * (r1.w - r0.w),
        };
    }
}

static inline void rdna4rt_curve_split_bezier(const rdna4rt_vec4_t b[4], float u,
                                              rdna4rt_vec4_t left_out[4],
                                              rdna4rt_vec4_t right_out[4]) {
    rdna4rt_vec4_t q0 = rdna4rt_v4_lerp(b[0], b[1], u);
    rdna4rt_vec4_t q1 = rdna4rt_v4_lerp(b[1], b[2], u);
    rdna4rt_vec4_t q2 = rdna4rt_v4_lerp(b[2], b[3], u);
    rdna4rt_vec4_t r0 = rdna4rt_v4_lerp(q0, q1, u);
    rdna4rt_vec4_t r1 = rdna4rt_v4_lerp(q1, q2, u);
    rdna4rt_vec4_t m  = rdna4rt_v4_lerp(r0, r1, u);
    if (left_out) {
        left_out[0] = b[0]; left_out[1] = q0; left_out[2] = r0; left_out[3] = m;
    }
    if (right_out) {
        right_out[0] = m; right_out[1] = r1; right_out[2] = q2; right_out[3] = b[3];
    }
}

static inline rdna4rt_err_t rdna4rt_curve_bezier_sub_aabb(const rdna4rt_vec4_t b[4],
                                                          float t0, float t1,
                                                          rdna4rt_vk_aabb_t *aabb_out) {
    if (!b || !aabb_out || !isfinite(t0) || !isfinite(t1)) return RDNA4RT_ERR_INVALID_ARG;
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 > 1.0f) t1 = 1.0f;
    if (t1 < t0) { float tmp = t0; t0 = t1; t1 = tmp; }

    rdna4rt_vec4_t sub[4];
    for (int i = 0; i < 4; i++) sub[i] = b[i];
    if (t1 == t0) {
        rdna4rt_vec4_t p;
        rdna4rt_curve_eval_bezier(b, t0, &p, NULL);
        for (int i = 0; i < 4; i++) sub[i] = p;
    } else {
        rdna4rt_vec4_t tmp_left[4], tmp_right[4];
        if (t0 > 0.0f) {
            rdna4rt_curve_split_bezier(sub, t0, tmp_left, tmp_right);
            for (int i = 0; i < 4; i++) sub[i] = tmp_right[i];
        }
        if (t1 < 1.0f) {
            float local_u = (t1 - t0) / (1.0f - t0);
            rdna4rt_curve_split_bezier(sub, local_u, tmp_left, tmp_right);
            for (int i = 0; i < 4; i++) sub[i] = tmp_left[i];
        }
    }

    float minx = sub[0].x, maxx = sub[0].x;
    float miny = sub[0].y, maxy = sub[0].y;
    float minz = sub[0].z, maxz = sub[0].z;
    float maxr = sub[0].w > 0.0f ? sub[0].w : 0.0f;
    for (int i = 1; i < 4; i++) {
        if (sub[i].x < minx) minx = sub[i].x;
        if (sub[i].x > maxx) maxx = sub[i].x;
        if (sub[i].y < miny) miny = sub[i].y;
        if (sub[i].y > maxy) maxy = sub[i].y;
        if (sub[i].z < minz) minz = sub[i].z;
        if (sub[i].z > maxz) maxz = sub[i].z;
        if (sub[i].w > maxr) maxr = sub[i].w;
    }
    float ext = fmaxf(fmaxf(fabsf(minx), fabsf(maxx)),
                      fmaxf(fmaxf(fabsf(miny), fabsf(maxy)),
                            fmaxf(fabsf(minz), fabsf(maxz))));
    float pad = maxr + 1e-5f * (1.0f + ext);
    aabb_out->min_x = minx - pad;
    aabb_out->min_y = miny - pad;
    aabb_out->min_z = minz - pad;
    aabb_out->max_x = maxx + pad;
    aabb_out->max_y = maxy + pad;
    aabb_out->max_z = maxz + pad;
    return RDNA4RT_OK;
}

static inline int rdna4rt_curve_build_ray_frame(const float rd[3],
                                                float u[3], float v[3], float w[3],
                                                float *inv_len_out) {
    float d2 = rdna4rt_v3_dot(rd, rd);
    if (!(d2 > 0.0f) || !isfinite(d2)) return 0;
    float dlen = sqrtf(d2);
    float inv_l = 1.0f / dlen;
    w[0] = rd[0] * inv_l; w[1] = rd[1] * inv_l; w[2] = rd[2] * inv_l;
    /* Choose major non-collinear axis for stable orthonormal tangent frame. */
    if (fabsf(w[0]) < 0.9f) {
        /* w x (1,0,0) = (0, w.z, -w.y) */
        u[0] = 0.0f; u[1] = w[2]; u[2] = -w[1];
    } else {
        /* w x (0,1,0) = (-w.z, 0, w.x) */
        u[0] = -w[2]; u[1] = 0.0f; u[2] = w[0];
    }
    float ulen = sqrtf(rdna4rt_v3_dot(u, u));
    if (!(ulen > 0.0f)) return 0;
    u[0] /= ulen; u[1] /= ulen; u[2] /= ulen;
    /* v = w x u */
    v[0] = w[1] * u[2] - w[2] * u[1];
    v[1] = w[2] * u[0] - w[0] * u[2];
    v[2] = w[0] * u[1] - w[1] * u[0];
    *inv_len_out = inv_l;
    return 1;
}

static inline void rdna4rt_curve_test_cap_sphere(const float ro[3], const float wn[3],
                                                 float smin, float smax,
                                                 const rdna4rt_vec4_t *c, float u_val,
                                                 float *best_s, float *best_u, int *hit) {
    if (c->w <= 0.0f) return;
    float oc[3] = { ro[0] - c->x, ro[1] - c->y, ro[2] - c->z };
    float b = rdna4rt_v3_dot(oc, wn);
    float cc = rdna4rt_v3_dot(oc, oc) - c->w * c->w;
    float disc = b * b - cc;
    if (disc < 0.0f) return;
    float sq = sqrtf(disc);
    float roots[2] = { -b - sq, -b + sq };
    for (int i = 0; i < 2; i++) {
        float s = roots[i];
        if (s >= smin && s <= smax && (!(*hit) || s < *best_s)) {
            *hit = 1;
            *best_s = s;
            *best_u = u_val;
        }
    }
}

static inline void rdna4rt_curve_test_cap_disk(const float ro[3], const float wn[3],
                                               float smin, float smax,
                                               const rdna4rt_vec4_t *c, const float norm[3],
                                               float u_val, float *best_s, float *best_u, int *hit) {
    if (c->w <= 0.0f) return;
    float denom = rdna4rt_v3_dot(wn, norm);
    if (fabsf(denom) < 1e-8f) return;
    float pc[3] = { c->x - ro[0], c->y - ro[1], c->z - ro[2] };
    float s = rdna4rt_v3_dot(pc, norm) / denom;
    if (s < smin || s > smax || (*hit && s >= *best_s)) return;
    float hx = ro[0] + wn[0] * s - c->x;
    float hy = ro[1] + wn[1] * s - c->y;
    float hz = ro[2] + wn[2] * s - c->z;
    if (hx * hx + hy * hy + hz * hz <= c->w * c->w * (1.0f + 1e-5f)) {
        *hit = 1;
        *best_s = s;
        *best_u = u_val;
    }
}

static inline int rdna4rt_cpu_intersect_curve_linear(
    const float ro[3], const float rd[3], float tmin, float tmax,
    const rdna4rt_vec4_t *p0, const rdna4rt_vec4_t *p1,
    rdna4rt_curve_type_t type, rdna4rt_curve_endcaps_t caps,
    float *t_out, float *u_out) {
    if (!ro || !rd || !p0 || !p1 || tmax < tmin) return 0;
    float u_ax[3], v_ax[3], wn[3], inv_dlen = 1.0f;
    if (!rdna4rt_curve_build_ray_frame(rd, u_ax, v_ax, wn, &inv_dlen)) return 0;
    float dlen = 1.0f / inv_dlen;
    float smin = tmin * dlen, smax = tmax * dlen;

    float delta[3] = { p1->x - p0->x, p1->y - p0->y, p1->z - p0->z };
    float d2 = rdna4rt_v3_dot(delta, delta);
    float dr = p1->w - p0->w;

    int hit = 0;
    float best_s = smax, best_u = 0.0f;

    if (d2 <= 1e-12f) {
        if (type == RDNA4RT_CURVE_TYPE_ROUND && caps == RDNA4RT_CURVE_ENDCAPS_SPHERE) {
            rdna4rt_vec4_t sp = *p0;
            if (p1->w > sp.w) sp.w = p1->w;
            rdna4rt_curve_test_cap_sphere(ro, wn, smin, smax, &sp, 0.0f, &best_s, &best_u, &hit);
        }
        if (hit) {
            if (t_out) *t_out = best_s * inv_dlen;
            if (u_out) *u_out = best_u;
        }
        return hit;
    }

    if (type == RDNA4RT_CURVE_TYPE_FLAT) {
        /* 2D closest point of ray origin in ray-perpendicular (u_ax, v_ax) plane to projected segment. */
        float a0[3] = { p0->x - ro[0], p0->y - ro[1], p0->z - ro[2] };
        float a1[3] = { p1->x - ro[0], p1->y - ro[1], p1->z - ro[2] };
        float p0u = rdna4rt_v3_dot(a0, u_ax), p0v = rdna4rt_v3_dot(a0, v_ax), p0w = rdna4rt_v3_dot(a0, wn);
        float p1u = rdna4rt_v3_dot(a1, u_ax), p1v = rdna4rt_v3_dot(a1, v_ax), p1w = rdna4rt_v3_dot(a1, wn);
        float su = p1u - p0u, sv = p1v - p0v;
        float s2 = su * su + sv * sv;
        float cand_u = (s2 > 1e-12f) ? (-(p0u * su + p0v * sv) / s2) : 0.5f;
        if (cand_u < 0.0f) cand_u = 0.0f;
        if (cand_u > 1.0f) cand_u = 1.0f;
        float cu = p0u + cand_u * su;
        float cv = p0v + cand_u * sv;
        float r = p0->w + cand_u * dr;
        float w_s = p0w + cand_u * (p1w - p0w);
        if (r >= 0.0f && (cu * cu + cv * cv) <= r * r && w_s >= smin && w_s <= smax) {
            hit = 1;
            best_s = w_s;
            best_u = cand_u;
        }
    } else {
        /* ROUND swept-sphere cone body: d/du(|ro + s*wn - C(u)|^2 - r(u)^2) == 0. */
        float denom = d2 - dr * dr;
        float oc0[3] = { ro[0] - p0->x, ro[1] - p0->y, ro[2] - p0->z };
        if (fabsf(denom) > 1e-10f) {
            float dw = rdna4rt_v3_dot(wn, delta);
            float oc_d = rdna4rt_v3_dot(oc0, delta);
            float r0 = p0->w;
            /* u(s) = k0 + k1 * s */
            float k0 = (oc_d + r0 * dr) / denom;
            float k1 = dw / denom;
            /* Q(s) = oc0 + s*wn - k0*delta - k1*delta*s = p_q + v_q * s */
            float pq[3] = {
                oc0[0] - k0 * delta[0], oc0[1] - k0 * delta[1], oc0[2] - k0 * delta[2],
            };
            float vq[3] = {
                wn[0] - k1 * delta[0], wn[1] - k1 * delta[1], wn[2] - k1 * delta[2],
            };
            float rq0 = r0 + k0 * dr;
            float rq1 = k1 * dr;
            float qa = rdna4rt_v3_dot(vq, vq) - rq1 * rq1;
            float qb = rdna4rt_v3_dot(pq, vq) - rq0 * rq1;
            float qc = rdna4rt_v3_dot(pq, pq) - rq0 * rq0;
            float disc = qb * qb - qa * qc;
            if (disc >= 0.0f) {
                float sq = sqrtf(disc);
                float roots[2];
                int nroots = 0;
                if (fabsf(qa) > 1e-10f) {
                    roots[0] = (-qb - sq) / qa;
                    roots[1] = (-qb + sq) / qa;
                    nroots = 2;
                } else if (fabsf(qb) > 1e-10f) {
                    roots[0] = -qc / (2.0f * qb);
                    nroots = 1;
                }
                for (int i = 0; i < nroots; i++) {
                    float s = roots[i];
                    float cu = k0 + k1 * s;
                    if (s >= smin && s <= smax && cu >= 0.0f && cu <= 1.0f && (!hit || s < best_s)) {
                        hit = 1;
                        best_s = s;
                        best_u = cu;
                    }
                }
            }
        }

        if (caps == RDNA4RT_CURVE_ENDCAPS_SPHERE) {
            rdna4rt_curve_test_cap_sphere(ro, wn, smin, smax, p0, 0.0f, &best_s, &best_u, &hit);
            rdna4rt_curve_test_cap_sphere(ro, wn, smin, smax, p1, 1.0f, &best_s, &best_u, &hit);
        } else if (caps == RDNA4RT_CURVE_ENDCAPS_DISK) {
            float inv_len_seg = 1.0f / sqrtf(d2);
            float n0[3] = { -delta[0] * inv_len_seg, -delta[1] * inv_len_seg, -delta[2] * inv_len_seg };
            float n1[3] = {  delta[0] * inv_len_seg,  delta[1] * inv_len_seg,  delta[2] * inv_len_seg };
            rdna4rt_curve_test_cap_disk(ro, wn, smin, smax, p0, n0, 0.0f, &best_s, &best_u, &hit);
            rdna4rt_curve_test_cap_disk(ro, wn, smin, smax, p1, n1, 1.0f, &best_s, &best_u, &hit);
        }
    }

    if (hit) {
        if (t_out) *t_out = best_s * inv_dlen;
        if (u_out) *u_out = best_u;
    }
    return hit;
}

static inline int rdna4rt_cpu_intersect_curve_cubic(
    const float ro[3], const float rd[3], float tmin, float tmax,
    const rdna4rt_vec4_t b[4], rdna4rt_curve_type_t type, rdna4rt_curve_endcaps_t caps,
    float *t_out, float *u_out) {
    if (!ro || !rd || !b || tmax < tmin) return 0;
    float u_ax[3], v_ax[3], wn[3], inv_dlen = 1.0f;
    if (!rdna4rt_curve_build_ray_frame(rd, u_ax, v_ax, wn, &inv_dlen)) return 0;
    float dlen = 1.0f / inv_dlen;
    float smin = tmin * dlen, smax = tmax * dlen;

    /* Transform Bezier control points into ray-centered (u, v, w) frame keeping radius in .w. */
    rdna4rt_vec4_t br[4];
    for (int i = 0; i < 4; i++) {
        float rel[3] = { b[i].x - ro[0], b[i].y - ro[1], b[i].z - ro[2] };
        br[i] = (rdna4rt_vec4_t){
            rdna4rt_v3_dot(rel, u_ax),
            rdna4rt_v3_dot(rel, v_ax),
            rdna4rt_v3_dot(rel, wn),
            b[i].w,
        };
    }

    int hit = 0;
    float best_s = smax, best_u = 0.0f;

    /* Refine curve root via Phantom tangent-cone swept-sphere iteration (Round) or UV closest approach (Flat). */
    enum { N_SEEDS = 32 };
    for (int seg = 0; seg < N_SEEDS; seg++) {
        float u = ((float)seg + 0.5f) / (float)N_SEEDS;
        for (int it = 0; it < 8; it++) {
            rdna4rt_vec4_t p, d;
            rdna4rt_curve_eval_bezier(br, u, &p, &d);
            float du = 0.0f;
            if (type != RDNA4RT_CURVE_TYPE_FLAT) {
                float denom = d.x * d.x + d.y * d.y + d.z * d.z - d.w * d.w;
                int denom_ok = fabsf(denom) > 1e-10f;
                float safe_d = denom_ok ? denom : 1.0f;
                float k0 = denom_ok ? ((p.w * d.w - (p.x * d.x + p.y * d.y)) / safe_d) : 0.0f;
                float k1 = denom_ok ? (d.z / safe_d) : 0.0f;
                float tx = p.x + k0 * d.x;
                float ty = p.y + k0 * d.y;
                float tz = k0 * d.z;
                float tr0 = p.w + k0 * d.w;
                float a = denom_ok ? ((d.x * d.x + d.y * d.y - d.w * d.w) / safe_d) : 0.0f;
                float c = tx * tx + ty * ty + tz * tz - tr0 * tr0;
                float disc = tz * tz - a * c;
                if (denom_ok && fabsf(a) > 1e-8f && disc >= 0.0f) {
                    float sq = sqrtf(disc);
                    float tau0 = (tz - sq) / a;
                    float tau1 = (tz + sq) / a;
                    float t_lo = tau0 < tau1 ? tau0 : tau1;
                    float t_hi = tau0 < tau1 ? tau1 : tau0;
                    float tau_pick = (p.z + t_lo >= smin) ? t_lo : t_hi;
                    du = k0 + k1 * tau_pick;
                    if (du > 0.25f) du = 0.25f;
                    if (du < -0.25f) du = -0.25f;
                    u += du;
                    if (u < 0.0f) u = 0.0f;
                    if (u > 1.0f) u = 1.0f;
                    if (fabsf(du) < 1e-6f) break;
                    continue;
                }
            }
            float f = (type == RDNA4RT_CURVE_TYPE_FLAT)
                ? (p.x * d.x + p.y * d.y)
                : (p.x * d.x + p.y * d.y - p.w * d.w);
            float df = (type == RDNA4RT_CURVE_TYPE_FLAT)
                ? (d.x * d.x + d.y * d.y)
                : (d.x * d.x + d.y * d.y - d.w * d.w);
            if (fabsf(df) < 1e-10f) break;
            float step = f / df;
            if (step > 0.2f) step = 0.2f;
            if (step < -0.2f) step = -0.2f;
            u -= step;
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;
            if (fabsf(step) < 1e-6f) break;
        }
        rdna4rt_vec4_t p, d;
        rdna4rt_curve_eval_bezier(br, u, &p, &d);
        if (type == RDNA4RT_CURVE_TYPE_FLAT) {
            float d2_uv = p.x * p.x + p.y * p.y;
            float r = p.w;
            if (r >= 0.0f && d2_uv <= r * r * (1.0f + 1e-5f) &&
                p.z >= smin && p.z <= smax && (!hit || p.z < best_s)) {
                hit = 1;
                best_s = p.z;
                best_u = u;
            }
        } else {
            float denom = d.x * d.x + d.y * d.y + d.z * d.z - d.w * d.w;
            if (fabsf(denom) <= 1e-10f) continue;
            float k0 = (p.w * d.w - (p.x * d.x + p.y * d.y)) / denom;
            float k1 = d.z / denom;
            float tx = p.x + k0 * d.x;
            float ty = p.y + k0 * d.y;
            float tz = k0 * d.z;
            float tr0 = p.w + k0 * d.w;
            float a = (d.x * d.x + d.y * d.y - d.w * d.w) / denom;
            float c = tx * tx + ty * ty + tz * tz - tr0 * tr0;
            float disc = tz * tz - a * c;
            if (fabsf(a) <= 1e-8f || disc < 0.0f) continue;
            float sq = sqrtf(disc);
            float taus[2] = { (tz - sq) / a, (tz + sq) / a };
            for (int k = 0; k < 2; k++) {
                float tau = taus[k];
                float s = p.z + tau;
                float du = k0 + k1 * tau;
                float u_raw = u + du;
                float r_hit = p.w + du * d.w;
                if (r_hit >= 0.0f && fabsf(du) <= 0.05f &&
                    u_raw >= -1e-4f && u_raw <= 1.0001f &&
                    s >= smin && s <= smax && (!hit || s < best_s)) {
                    hit = 1;
                    best_s = s;
                    best_u = (u_raw < 0.0f) ? 0.0f : ((u_raw > 1.0f) ? 1.0f : u_raw);
                }
            }
        }
    }

    if (type == RDNA4RT_CURVE_TYPE_ROUND) {
        if (caps == RDNA4RT_CURVE_ENDCAPS_SPHERE) {
            rdna4rt_curve_test_cap_sphere(ro, wn, smin, smax, &b[0], 0.0f, &best_s, &best_u, &hit);
            rdna4rt_curve_test_cap_sphere(ro, wn, smin, smax, &b[3], 1.0f, &best_s, &best_u, &hit);
        } else if (caps == RDNA4RT_CURVE_ENDCAPS_DISK) {
            rdna4rt_vec4_t p0, d0, p1, d1;
            rdna4rt_curve_eval_bezier(b, 0.0f, &p0, &d0);
            rdna4rt_curve_eval_bezier(b, 1.0f, &p1, &d1);
            float n0_len2 = d0.x * d0.x + d0.y * d0.y + d0.z * d0.z;
            float n1_len2 = d1.x * d1.x + d1.y * d1.y + d1.z * d1.z;
            if (n0_len2 > 1e-12f) {
                float il = 1.0f / sqrtf(n0_len2);
                float n0[3] = { -d0.x * il, -d0.y * il, -d0.z * il };
                rdna4rt_curve_test_cap_disk(ro, wn, smin, smax, &p0, n0, 0.0f, &best_s, &best_u, &hit);
            }
            if (n1_len2 > 1e-12f) {
                float il = 1.0f / sqrtf(n1_len2);
                float n1[3] = { d1.x * il, d1.y * il, d1.z * il };
                rdna4rt_curve_test_cap_disk(ro, wn, smin, smax, &p1, n1, 1.0f, &best_s, &best_u, &hit);
            }
        }
    }

    if (hit) {
        if (t_out) *t_out = best_s * inv_dlen;
        if (u_out) *u_out = best_u;
    }
    return hit;
}

/* GPU sidecar metadata structs for Wave 2.2/2.3 compute AABB expansion & SPIR-V curve solver. */
#define RDNA4RT_CURVE_META_MAGIC UINT32_C(0x52544356) /* 'RTCV' */

typedef struct {
    uint32_t magic;
    uint32_t curve_type;
    uint32_t curve_basis;
    uint32_t curve_end_caps;
    uint32_t segment_cp_count;
    uint32_t segment_count;
    uint32_t sub_splits;
    uint32_t flags;
    uint64_t seg_table_off;
    uint64_t sub_map_off;
} rdna4rt_curve_meta_hdr_t;

typedef struct {
    float b[4][4]; /* Cubic Bezier control points: (x, y, z, radius) */
    float t0;
    float t1;
    uint32_t segment_id;
    uint32_t geom_index;
} rdna4rt_curve_seg_gpu_t;

_Static_assert(sizeof(rdna4rt_curve_meta_hdr_t) == 48u, "curve_meta_hdr must be 48 bytes");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, magic) == 0u, "curve_meta_hdr magic offset");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, curve_type) == 4u, "curve_meta_hdr curve_type offset");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, curve_end_caps) == 12u, "curve_meta_hdr end_caps offset");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, segment_count) == 20u, "curve_meta_hdr seg_count offset");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, sub_splits) == 24u, "curve_meta_hdr sub_splits offset");
_Static_assert(offsetof(rdna4rt_curve_meta_hdr_t, seg_table_off) == 32u, "curve_meta_hdr seg_table_off offset");
_Static_assert(sizeof(rdna4rt_curve_seg_gpu_t) == 80u, "curve_seg_gpu must be 80 bytes");
_Static_assert(offsetof(rdna4rt_curve_seg_gpu_t, b) == 0u, "curve_seg_gpu b offset");
_Static_assert(offsetof(rdna4rt_curve_seg_gpu_t, t0) == 64u, "curve_seg_gpu t0 offset");
_Static_assert(offsetof(rdna4rt_curve_seg_gpu_t, t1) == 68u, "curve_seg_gpu t1 offset");
_Static_assert(offsetof(rdna4rt_curve_seg_gpu_t, segment_id) == 72u, "curve_seg_gpu segment_id offset");
_Static_assert(offsetof(rdna4rt_curve_seg_gpu_t, geom_index) == 76u, "curve_seg_gpu geom_index offset");

static inline uint32_t rdna4rt_curve_sub_splits(rdna4rt_curve_basis_t basis) {
    return (basis == RDNA4RT_CURVE_BASIS_LINEAR) ? 1u : 2u;
}

static inline rdna4rt_err_t rdna4rt_curve_aabb_count(uint32_t seg_count,
                                                     uint32_t sub_splits,
                                                     uint32_t *out) {
    if (!out || sub_splits == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (seg_count > UINT32_MAX / sub_splits) return RDNA4RT_ERR_OVERFLOW;
    *out = seg_count * sub_splits;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_blas_sidecar_layout_compute(
    const rdna4rt_prim_data_geom_plan_t *geoms,
    const uint32_t *curve_aabb_counts,
    const uint64_t *motion_payload_bytes,
    uint32_t geom_count,
    uint64_t *prim_rel_offsets_out,
    uint64_t *header_bytes_out,
    uint64_t *aux_curve_off_out,
    uint64_t *curve_seg_offsets_out,
    uint64_t *aux_motion_off_out,
    uint64_t *motion_geom_offsets_out,
    uint64_t *total_bytes_out,
    int *any_prim_data_out,
    int *any_curve_out,
    int *any_motion_out) {
    if (!header_bytes_out || !aux_curve_off_out || !total_bytes_out ||
        !any_prim_data_out || !any_curve_out)
        return RDNA4RT_ERR_INVALID_ARG;
    *header_bytes_out = 0u;
    *aux_curve_off_out = 0u;
    if (aux_motion_off_out) *aux_motion_off_out = 0u;
    *total_bytes_out = 0u;
    *any_prim_data_out = 0;
    *any_curve_out = 0;
    if (any_motion_out) *any_motion_out = 0;
    if (geom_count == 0u) return RDNA4RT_OK;
    if ((geoms && !prim_rel_offsets_out) ||
        (curve_aabb_counts && !curve_seg_offsets_out) ||
        (motion_payload_bytes && !motion_geom_offsets_out))
        return RDNA4RT_ERR_INVALID_ARG;

    int any_prim = 0, any_curve = 0, any_motion = 0;
    for (uint32_t i = 0u; i < geom_count; i++) {
        if (prim_rel_offsets_out) prim_rel_offsets_out[i] = 0u;
        if (curve_seg_offsets_out) curve_seg_offsets_out[i] = 0u;
        if (motion_geom_offsets_out) motion_geom_offsets_out[i] = 0u;
        if (geoms && geoms[i].present && geoms[i].prim_count > 0u && geoms[i].copy_bytes > 0u)
            any_prim = 1;
        if (curve_aabb_counts && curve_aabb_counts[i] > 0u)
            any_curve = 1;
        if (motion_payload_bytes && motion_payload_bytes[i] > 0u)
            any_motion = 1;
    }
    if (!any_prim && !any_curve && !any_motion) return RDNA4RT_OK;

    uint64_t hdr_bytes = 0u;
    if (!rdna4rt_mul_u64((uint64_t)geom_count, (uint64_t)sizeof(rdna4rt_geom_prim_entry_t), &hdr_bytes))
        return RDNA4RT_ERR_OVERFLOW;
    uint64_t cursor = hdr_bytes;

    if (any_prim && geoms) {
        for (uint32_t i = 0u; i < geom_count; i++) {
            if (!geoms[i].present || geoms[i].prim_count == 0u || geoms[i].copy_bytes == 0u) continue;
            uint64_t aligned_off = 0u, next = 0u;
            if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &aligned_off) ||
                !rdna4rt_add_u64(aligned_off, geoms[i].copy_bytes, &next))
                return RDNA4RT_ERR_OVERFLOW;
            prim_rel_offsets_out[i] = aligned_off;
            cursor = next;
        }
    }

    if (any_curve && curve_aabb_counts) {
        uint64_t aux_off = 0u, curve_hdr_bytes = 0u, next = 0u;
        if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &aux_off) ||
            !rdna4rt_mul_u64((uint64_t)geom_count, (uint64_t)sizeof(rdna4rt_curve_meta_hdr_t), &curve_hdr_bytes) ||
            !rdna4rt_add_u64(aux_off, curve_hdr_bytes, &next))
            return RDNA4RT_ERR_OVERFLOW;
        *aux_curve_off_out = aux_off;
        cursor = next;

        for (uint32_t i = 0u; i < geom_count; i++) {
            if (curve_aabb_counts[i] == 0u) continue;
            uint64_t seg_off = 0u, seg_bytes = 0u, next_seg = 0u;
            if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &seg_off) ||
                !rdna4rt_mul_u64((uint64_t)curve_aabb_counts[i], (uint64_t)sizeof(rdna4rt_curve_seg_gpu_t), &seg_bytes) ||
                !rdna4rt_add_u64(seg_off, seg_bytes, &next_seg))
                return RDNA4RT_ERR_OVERFLOW;
            curve_seg_offsets_out[i] = seg_off;
            cursor = next_seg;
        }
    }

    if (any_motion && motion_payload_bytes) {
        if (!aux_motion_off_out) return RDNA4RT_ERR_INVALID_ARG;
        uint64_t mot_off = 0u, mot_hdr_bytes = 0u, next = 0u;
        if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &mot_off) ||
            !rdna4rt_mul_u64((uint64_t)geom_count, UINT64_C(48), &mot_hdr_bytes) ||
            !rdna4rt_add_u64(mot_off, mot_hdr_bytes, &next))
            return RDNA4RT_ERR_OVERFLOW;
        *aux_motion_off_out = mot_off;
        cursor = next;

        for (uint32_t i = 0u; i < geom_count; i++) {
            if (motion_payload_bytes[i] == 0u) continue;
            uint64_t m_off = 0u, next_m = 0u;
            if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &m_off) ||
                !rdna4rt_add_u64(m_off, motion_payload_bytes[i], &next_m))
                return RDNA4RT_ERR_OVERFLOW;
            motion_geom_offsets_out[i] = m_off;
            cursor = next_m;
        }
    }

    uint64_t final_bytes = 0u;
    if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &final_bytes))
        return RDNA4RT_ERR_OVERFLOW;

    *header_bytes_out = hdr_bytes;
    *total_bytes_out = final_bytes;
    *any_prim_data_out = any_prim;
    *any_curve_out = any_curve;
    if (any_motion_out) *any_motion_out = any_motion;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_blas_curve_layout_compute(
    const rdna4rt_prim_data_geom_plan_t *geoms,
    const uint32_t *curve_aabb_counts,
    uint32_t geom_count,
    uint64_t *prim_rel_offsets_out,
    uint64_t *header_bytes_out,
    uint64_t *aux_curve_off_out,
    uint64_t *curve_seg_offsets_out,
    uint64_t *total_bytes_out,
    int *any_prim_data_out,
    int *any_curve_out) {
    return rdna4rt_blas_sidecar_layout_compute(
        geoms, curve_aabb_counts, NULL, geom_count,
        prim_rel_offsets_out, header_bytes_out, aux_curve_off_out, curve_seg_offsets_out,
        NULL, NULL, total_bytes_out, any_prim_data_out, any_curve_out, NULL);
}


#endif /* GARNET_RT_CURVES_H */
