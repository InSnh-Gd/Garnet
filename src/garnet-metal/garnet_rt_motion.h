/* garnet_rt_motion.h: Metal 3.1 motion-blur geometry/TLAS validation, math, and CPU oracles. */
#ifndef GARNET_RT_MOTION_H
#define GARNET_RT_MOTION_H

#ifndef RDNA4_RT_ACCEL_H
#include "garnet_rt_accel.h"
#endif

/* =========================================================================
 * Wave 3.1: Primitive & Instance Motion Blur Pure-C Core (Task 4.1)
 * ========================================================================= */

#define RDNA4RT_MOTION_GEOM_MAGIC     UINT32_C(0x52544D47) /* 'RTMG' */
#define RDNA4RT_MOTION_GEOM_TRIANGLES UINT32_C(0)
#define RDNA4RT_MOTION_GEOM_AABBS     UINT32_C(1)
#define RDNA4RT_MOTION_TRI_STRIDE     UINT32_C(36) /* 3 x packed vec3 (9 floats) */
#define RDNA4RT_MOTION_AABB_STRIDE    UINT32_C(24) /* min_xyz, max_xyz (6 floats) */
#define RDNA4RT_MOTION_TLAS_MAGIC     UINT32_C(0x52544D54) /* 'RTMT' */

typedef struct {
    uint32_t magic;
    uint32_t geom_kind;
    uint32_t keyframe_count;
    uint32_t prim_count;
    uint32_t start_border;
    uint32_t end_border;
    float start_time;
    float end_time;
    uint64_t data_off;
    uint32_t prim_stride;
    uint32_t flags;
} rdna4rt_motion_geom_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t instance_count;
    uint32_t xform_type;
    uint32_t xform_count;
    uint64_t inst_meta_off;
    uint64_t xform_data_off;
} rdna4rt_motion_tlas_hdr_t;

typedef struct {
    uint64_t blas_bda;
    uint32_t xform_start;
    uint32_t xform_count;
    uint32_t start_border;
    uint32_t end_border;
    float start_time;
    float end_time;
    uint32_t options;
    uint32_t ift_offset;
    uint32_t user_id;
    uint32_t mask;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
} rdna4rt_motion_tlas_inst_t;

_Static_assert(sizeof(rdna4rt_motion_geom_hdr_t) == 48u, "motion_geom_hdr must be 48 bytes");
_Static_assert(offsetof(rdna4rt_motion_geom_hdr_t, magic) == 0u, "motion_geom_hdr magic offset");
_Static_assert(offsetof(rdna4rt_motion_geom_hdr_t, start_time) == 24u, "motion_geom_hdr start_time offset");
_Static_assert(offsetof(rdna4rt_motion_geom_hdr_t, data_off) == 32u, "motion_geom_hdr data_off offset");
_Static_assert(offsetof(rdna4rt_motion_geom_hdr_t, prim_stride) == 40u, "motion_geom_hdr prim_stride offset");
_Static_assert(sizeof(rdna4rt_motion_tlas_hdr_t) == 32u, "motion_tlas_hdr must be 32 bytes");
_Static_assert(offsetof(rdna4rt_motion_tlas_hdr_t, inst_meta_off) == 16u, "motion_tlas_hdr inst_meta_off offset");
_Static_assert(offsetof(rdna4rt_motion_tlas_hdr_t, xform_data_off) == 24u, "motion_tlas_hdr xform_data_off offset");
_Static_assert(sizeof(rdna4rt_motion_tlas_inst_t) == 64u, "motion_tlas_inst must be 64 bytes");
_Static_assert(offsetof(rdna4rt_motion_tlas_inst_t, xform_start) == 8u, "motion_tlas_inst xform_start offset");
_Static_assert(offsetof(rdna4rt_motion_tlas_inst_t, start_time) == 24u, "motion_tlas_inst start_time offset");
_Static_assert(offsetof(rdna4rt_motion_tlas_inst_t, mask) == 44u, "motion_tlas_inst mask offset");

/* Evaluate motion timestamp into bracketing keyframe indices (k0, k1, alpha in [0,1]).
 * Returns 1 when active, 0 when vanished outside border bounds, -1 on invalid args. */
static inline int rdna4rt_motion_time_eval(
    float eval_time, float start_time, float end_time,
    uint32_t start_border, uint32_t end_border, uint32_t keyframe_count,
    uint32_t *k0_out, uint32_t *k1_out, float *alpha_out) {
    if (!k0_out || !k1_out || !alpha_out || keyframe_count == 0u) return -1;
    if (!isfinite(eval_time) || !isfinite(start_time) || !isfinite(end_time) ||
        start_time > end_time)
        return -1;
    if (start_border > RDNA4RT_MOTION_BORDER_VANISH ||
        end_border > RDNA4RT_MOTION_BORDER_VANISH)
        return -1;

    float clamped = eval_time;
    if (eval_time < start_time) {
        if (start_border == RDNA4RT_MOTION_BORDER_VANISH) return 0;
        clamped = start_time;
    } else if (eval_time > end_time) {
        if (end_border == RDNA4RT_MOTION_BORDER_VANISH) return 0;
        clamped = end_time;
    }

    uint32_t k0 = 0u, k1 = 0u;
    float alpha = 0.0f;
    if (keyframe_count > 1u && end_time > start_time) {
        double u = ((double)clamped - (double)start_time) / ((double)end_time - (double)start_time);
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        uint32_t last = keyframe_count - 1u;
        double scaled = u * (double)last;
        if (scaled >= (double)last) {
            k0 = last;
            k1 = last;
            alpha = 0.0f;
        } else {
            k0 = (uint32_t)scaled;
            k1 = k0 + 1u;
            alpha = (float)(scaled - (double)k0);
        }
    }
    *k0_out = k0;
    *k1_out = k1;
    *alpha_out = alpha;
    return 1;
}

static inline int rdna4rt_quat_slerp(const float q0[4], const float q1[4], float t, float out[4]) {
    if (!q0 || !q1 || !out || !isfinite(t)) return 0;
    for (int k = 0; k < 4; k++) {
        if (!isfinite(q0[k]) || !isfinite(q1[k])) return 0;
    }
    float n0 = sqrtf(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    float n1 = sqrtf(q1[0] * q1[0] + q1[1] * q1[1] + q1[2] * q1[2] + q1[3] * q1[3]);
    if (!(n0 > 1e-12f) || !(n1 > 1e-12f)) return 0;

    float a[4] = { q0[0] / n0, q0[1] / n0, q0[2] / n0, q0[3] / n0 };
    float b[4] = { q1[0] / n1, q1[1] / n1, q1[2] / n1, q1[3] / n1 };
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        for (int k = 0; k < 4; k++) b[k] = -b[k];
        dot = -dot;
    }
    if (dot > 1.0f) dot = 1.0f;

    float res[4];
    if (dot > 0.9995f) {
        for (int k = 0; k < 4; k++) res[k] = a[k] + t * (b[k] - a[k]);
    } else {
        float theta0 = acosf(dot);
        float sin0 = sinf(theta0);
        if (!(fabsf(sin0) > 1e-12f)) return 0;
        float theta = theta0 * t;
        float sint = sinf(theta);
        float s0 = cosf(theta) - dot * (sint / sin0);
        float s1 = sint / sin0;
        for (int k = 0; k < 4; k++) res[k] = s0 * a[k] + s1 * b[k];
    }
    float rn = sqrtf(res[0] * res[0] + res[1] * res[1] + res[2] * res[2] + res[3] * res[3]);
    if (!(rn > 1e-12f)) return 0;
    for (int k = 0; k < 4; k++) out[k] = res[k] / rn;
    return 1;
}

static inline rdna4rt_err_t rdna4rt_component_transform_to_3x4(
    const rdna4rt_component_transform_t *srt, rdna4rt_vk_transform_t *out) {
    if (!srt || !out) return RDNA4RT_ERR_INVALID_ARG;
    for (int k = 0; k < 3; k++) {
        if (!isfinite(srt->scale[k]) || !isfinite(srt->shear[k]) ||
            !isfinite(srt->pivot[k]) || !isfinite(srt->translation[k]))
            return RDNA4RT_ERR_NAN_TRANSFORM;
    }
    for (int k = 0; k < 4; k++) {
        if (!isfinite(srt->rotation[k])) return RDNA4RT_ERR_NAN_TRANSFORM;
    }
    float qlen = sqrtf(srt->rotation[0] * srt->rotation[0] + srt->rotation[1] * srt->rotation[1] +
                       srt->rotation[2] * srt->rotation[2] + srt->rotation[3] * srt->rotation[3]);
    if (!(qlen > 1e-12f)) return RDNA4RT_ERR_NAN_TRANSFORM;

    float qx = srt->rotation[0] / qlen, qy = srt->rotation[1] / qlen;
    float qz = srt->rotation[2] / qlen, qw = srt->rotation[3] / qlen;
    float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    float xx = qx * x2, xy = qx * y2, xz = qx * z2;
    float yy = qy * y2, yz = qy * z2, zz = qz * z2;
    float wx = qw * x2, wy = qw * y2, wz = qw * z2;

    float rot[3][3] = {
        { 1.0f - (yy + zz), xy - wz,          xz + wy },
        { xy + wz,          1.0f - (xx + zz), yz - wx },
        { xz - wy,          yz + wx,          1.0f - (xx + yy) },
    };
    float ss[3][3] = {
        { srt->scale[0], srt->shear[0], srt->shear[1] },
        { 0.0f,          srt->scale[1], srt->shear[2] },
        { 0.0f,          0.0f,          srt->scale[2] },
    };

    for (int rw = 0; rw < 3; rw++) {
        for (int cl = 0; cl < 3; cl++) {
            out->matrix[rw][cl] =
                rot[rw][0] * ss[0][cl] + rot[rw][1] * ss[1][cl] + rot[rw][2] * ss[2][cl];
        }
        out->matrix[rw][3] = rot[rw][0] * srt->pivot[0] +
                             rot[rw][1] * srt->pivot[1] +
                             rot[rw][2] * srt->pivot[2] +
                             srt->translation[rw];
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_component_transform_interp(
    const rdna4rt_component_transform_t *a, const rdna4rt_component_transform_t *b,
    float alpha, rdna4rt_vk_transform_t *out) {
    if (!a || !b || !out) return RDNA4RT_ERR_INVALID_ARG;
    if (!isfinite(alpha)) return RDNA4RT_ERR_INVALID_ARG;
    rdna4rt_component_transform_t mix_t;
    for (int k = 0; k < 3; k++) {
        if (!isfinite(a->scale[k]) || !isfinite(b->scale[k]) ||
            !isfinite(a->shear[k]) || !isfinite(b->shear[k]) ||
            !isfinite(a->pivot[k]) || !isfinite(b->pivot[k]) ||
            !isfinite(a->translation[k]) || !isfinite(b->translation[k]))
            return RDNA4RT_ERR_NAN_TRANSFORM;
        mix_t.scale[k]       = a->scale[k]       + (b->scale[k]       - a->scale[k])       * alpha;
        mix_t.shear[k]       = a->shear[k]       + (b->shear[k]       - a->shear[k])       * alpha;
        mix_t.pivot[k]       = a->pivot[k]       + (b->pivot[k]       - a->pivot[k])       * alpha;
        mix_t.translation[k] = a->translation[k] + (b->translation[k] - a->translation[k]) * alpha;
    }
    if (!rdna4rt_quat_slerp(a->rotation, b->rotation, alpha, mix_t.rotation))
        return RDNA4RT_ERR_NAN_TRANSFORM;
    return rdna4rt_component_transform_to_3x4(&mix_t, out);
}

static inline rdna4rt_err_t rdna4rt_transform_3x4_lerp(
    const rdna4rt_vk_transform_t *a, const rdna4rt_vk_transform_t *b,
    float alpha, rdna4rt_vk_transform_t *out) {
    if (!a || !b || !out) return RDNA4RT_ERR_INVALID_ARG;
    if (!isfinite(alpha)) return RDNA4RT_ERR_INVALID_ARG;
    for (int rw = 0; rw < 3; rw++) {
        for (int cl = 0; cl < 4; cl++) {
            float va = a->matrix[rw][cl], vb = b->matrix[rw][cl];
            if (!isfinite(va) || !isfinite(vb)) return RDNA4RT_ERR_NAN_TRANSFORM;
            out->matrix[rw][cl] = va + (vb - va) * alpha;
        }
    }
    return RDNA4RT_OK;
}

static inline int rdna4rt_transform_3x4_invert(
    const rdna4rt_vk_transform_t *m, rdna4rt_vk_transform_t *inv_out) {
    if (!m || !inv_out) return 0;
    for (int rw = 0; rw < 3; rw++) {
        for (int cl = 0; cl < 4; cl++) {
            if (!isfinite(m->matrix[rw][cl])) return 0;
        }
    }
    float a00 = m->matrix[0][0], a01 = m->matrix[0][1], a02 = m->matrix[0][2];
    float a10 = m->matrix[1][0], a11 = m->matrix[1][1], a12 = m->matrix[1][2];
    float a20 = m->matrix[2][0], a21 = m->matrix[2][1], a22 = m->matrix[2][2];

    float c00 = a11 * a22 - a12 * a21;
    float c01 = a12 * a20 - a10 * a22;
    float c02 = a10 * a21 - a11 * a20;
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (!isfinite(det) || fabsf(det) <= 1e-12f) return 0;
    float idet = 1.0f / det;

    inv_out->matrix[0][0] = c00 * idet;
    inv_out->matrix[0][1] = (a02 * a21 - a01 * a22) * idet;
    inv_out->matrix[0][2] = (a01 * a12 - a02 * a11) * idet;
    inv_out->matrix[1][0] = c01 * idet;
    inv_out->matrix[1][1] = (a00 * a22 - a02 * a20) * idet;
    inv_out->matrix[1][2] = (a02 * a10 - a00 * a12) * idet;
    inv_out->matrix[2][0] = c02 * idet;
    inv_out->matrix[2][1] = (a01 * a20 - a00 * a21) * idet;
    inv_out->matrix[2][2] = (a00 * a11 - a01 * a10) * idet;

    float tx = m->matrix[0][3], ty = m->matrix[1][3], tz = m->matrix[2][3];
    for (int rw = 0; rw < 3; rw++) {
        inv_out->matrix[rw][3] = -(inv_out->matrix[rw][0] * tx +
                                   inv_out->matrix[rw][1] * ty +
                                   inv_out->matrix[rw][2] * tz);
    }
    return 1;
}

static inline void rdna4rt_aabb_union(rdna4rt_vk_aabb_t *acc, const rdna4rt_vk_aabb_t *b) {
    if (!acc || !b) return;
    if (b->min_x < acc->min_x) acc->min_x = b->min_x;
    if (b->min_y < acc->min_y) acc->min_y = b->min_y;
    if (b->min_z < acc->min_z) acc->min_z = b->min_z;
    if (b->max_x > acc->max_x) acc->max_x = b->max_x;
    if (b->max_y > acc->max_y) acc->max_y = b->max_y;
    if (b->max_z > acc->max_z) acc->max_z = b->max_z;
}

static inline void rdna4rt_transform_aabb_3x4(
    const rdna4rt_vk_aabb_t *local_aabb, const rdna4rt_vk_transform_t *m,
    rdna4rt_vk_aabb_t *world_out) {
    if (!local_aabb || !m || !world_out) return;
    const float xs[2] = { local_aabb->min_x, local_aabb->max_x };
    const float ys[2] = { local_aabb->min_y, local_aabb->max_y };
    const float zs[2] = { local_aabb->min_z, local_aabb->max_z };
    for (int corner = 0; corner < 8; corner++) {
        float px = xs[corner & 1];
        float py = ys[(corner >> 1) & 1];
        float pz = zs[(corner >> 2) & 1];
        float wx = m->matrix[0][0] * px + m->matrix[0][1] * py + m->matrix[0][2] * pz + m->matrix[0][3];
        float wy = m->matrix[1][0] * px + m->matrix[1][1] * py + m->matrix[1][2] * pz + m->matrix[1][3];
        float wz = m->matrix[2][0] * px + m->matrix[2][1] * py + m->matrix[2][2] * pz + m->matrix[2][3];
        if (corner == 0) {
            *world_out = (rdna4rt_vk_aabb_t){ wx, wy, wz, wx, wy, wz };
        } else {
            rdna4rt_vk_aabb_t pt = { wx, wy, wz, wx, wy, wz };
            rdna4rt_aabb_union(world_out, &pt);
        }
    }
}

/* Motion triangle BLAS geometry descriptor & validator. */
typedef struct {
    rdna4rt_vfmt_t vertex_format;
    uint32_t keyframe_count;
    const int *has_vertex_bufs;
    const uint64_t *vertex_buf_gpu_addrs;
    const uint64_t *vertex_buf_lens;
    const uint64_t *vertex_buf_offsets;
    uint64_t vertex_stride;
    int has_index_buf;
    rdna4rt_index_type_t index_type;
    uint64_t index_buf_gpu_addr;
    uint64_t index_buf_len;
    uint64_t index_buf_offset;
    uint64_t triangle_count;
    int has_transform_buf;
    uint64_t transform_buf_gpu_addr;
    uint64_t transform_buf_len;
    uint64_t transform_buf_offset;
    rdna4rt_matrix_layout_t transform_layout;
    int opaque;
    int allow_duplicate_any_hit;
} rdna4rt_motion_tri_geom_desc_t;

typedef struct {
    uint32_t vertex_elem_bytes;
    uint32_t vertex_align;
    uint64_t vertex_stride;
    uint32_t keyframe_count;
    uint32_t primitive_count;
    rdna4rt_index_type_t effective_index_type;
    uint64_t index_addr;
    uint64_t transform_addr;
    uint64_t payload_bytes;
    uint32_t vk_geom_flags;
} rdna4rt_motion_tri_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_motion_tri_geom(
    const rdna4rt_motion_tri_geom_desc_t *d, int require_buffers, int allow_motion_blur,
    const rdna4dc_metal3_caps *caps, rdna4rt_motion_tri_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!allow_motion_blur) return RDNA4RT_ERR_MOTION_UNSUPPORTED;
    if (d->keyframe_count < 2u) return RDNA4RT_ERR_INVALID_ARG;

    uint32_t elem_bytes = 0u, comp_align = 0u;
    if (!rdna4rt_vfmt_info(d->vertex_format, &elem_bytes, &comp_align))
        return RDNA4RT_ERR_UNSUPPORTED;
    if (d->triangle_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_primitives > 0u && d->triangle_count > caps->rt_max_primitives)
        return RDNA4RT_ERR_LIMIT;

    uint64_t stride = d->vertex_stride ? d->vertex_stride : (uint64_t)elem_bytes;
    if (stride < (uint64_t)elem_bytes || stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((stride % (uint64_t)comp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    rdna4rt_index_type_t idx_type = d->has_index_buf ? d->index_type : RDNA4RT_INDEX_NONE;
    uint64_t idx_elem = 0u;
    if (idx_type == RDNA4RT_INDEX_UINT16) idx_elem = 2u;
    else if (idx_type == RDNA4RT_INDEX_UINT32) idx_elem = 4u;
    else if (idx_type != RDNA4RT_INDEX_NONE) return RDNA4RT_ERR_INVALID_ARG;

    uint64_t tri_verts = 0u;
    if (!rdna4rt_mul_u64(d->triangle_count, 3u, &tri_verts)) return RDNA4RT_ERR_OVERFLOW;

    uint64_t i_addr = 0u, t_addr = 0u;
    if (d->triangle_count > 0u) {
        if (require_buffers && (!d->has_vertex_bufs || !d->vertex_buf_gpu_addrs ||
                                !d->vertex_buf_lens || !d->vertex_buf_offsets))
            return RDNA4RT_ERR_INVALID_ARG;
        for (uint32_t kf = 0u; kf < d->keyframe_count; kf++) {
            int has_vb = d->has_vertex_bufs ? d->has_vertex_bufs[kf] : 0;
            if (require_buffers && !has_vb) return RDNA4RT_ERR_INVALID_ARG;
            if (!has_vb) continue;
            uint64_t addr = d->vertex_buf_gpu_addrs ? d->vertex_buf_gpu_addrs[kf] : 0u;
            uint64_t len = d->vertex_buf_lens ? d->vertex_buf_lens[kf] : 0u;
            uint64_t off = d->vertex_buf_offsets ? d->vertex_buf_offsets[kf] : 0u;
            if (require_buffers && addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (off > len) return RDNA4RT_ERR_BOUNDS;
            if (addr != 0u) {
                uint64_t va = 0u;
                if (!rdna4rt_add_u64(addr, off, &va)) return RDNA4RT_ERR_OVERFLOW;
                if ((va % (uint64_t)comp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            } else if ((off % (uint64_t)comp_align) != 0u) {
                return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t v_avail = len - off;
            if (idx_type == RDNA4RT_INDEX_NONE) {
                if (tri_verts - 1u > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
                uint64_t span = 0u, need = 0u;
                if (!rdna4rt_mul_u64(tri_verts - 1u, stride, &span) ||
                    !rdna4rt_add_u64(span, (uint64_t)elem_bytes, &need))
                    return RDNA4RT_ERR_OVERFLOW;
                if (need > v_avail) return RDNA4RT_ERR_BOUNDS;
            } else if (v_avail < (uint64_t)elem_bytes) {
                return RDNA4RT_ERR_BOUNDS;
            }
        }
        if (d->has_index_buf) {
            if (require_buffers && d->index_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (d->index_buf_offset > d->index_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((d->index_buf_offset % idx_elem) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (d->index_buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(d->index_buf_gpu_addr, d->index_buf_offset, &i_addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((i_addr % idx_elem) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t idx_bytes = 0u;
            if (!rdna4rt_mul_u64(tri_verts, idx_elem, &idx_bytes)) return RDNA4RT_ERR_OVERFLOW;
            if (idx_bytes > d->index_buf_len - d->index_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
    }

    if (d->has_transform_buf) {
        if (d->transform_layout != RDNA4RT_MATRIX_COLUMN_MAJOR &&
            d->transform_layout != RDNA4RT_MATRIX_ROW_MAJOR)
            return RDNA4RT_ERR_INVALID_ARG;
        if (d->transform_buf_offset > d->transform_buf_len ||
            RDNA4RT_TRANSFORM_SIZE > d->transform_buf_len - d->transform_buf_offset)
            return RDNA4RT_ERR_BOUNDS;
        if (require_buffers && d->transform_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
        if (d->transform_buf_gpu_addr != 0u) {
            if (!rdna4rt_add_u64(d->transform_buf_gpu_addr, d->transform_buf_offset, &t_addr))
                return RDNA4RT_ERR_OVERFLOW;
        }
    }

    uint64_t prim_kf = 0u, pay_bytes = 0u;
    if (!rdna4rt_mul_u64(d->triangle_count, (uint64_t)d->keyframe_count, &prim_kf) ||
        !rdna4rt_mul_u64(prim_kf, RDNA4RT_MOTION_TRI_STRIDE, &pay_bytes))
        return RDNA4RT_ERR_OVERFLOW;

    out->vertex_elem_bytes = elem_bytes;
    out->vertex_align = comp_align;
    out->vertex_stride = stride;
    out->keyframe_count = d->keyframe_count;
    out->primitive_count = (uint32_t)d->triangle_count;
    out->effective_index_type = idx_type;
    out->index_addr = i_addr;
    out->transform_addr = t_addr;
    out->payload_bytes = pay_bytes;
    out->vk_geom_flags = rdna4rt_geom_flags_to_vk(d->opaque, d->allow_duplicate_any_hit);
    return RDNA4RT_OK;
}

/* Motion bounding-box (AABB) BLAS geometry descriptor & validator. */
typedef struct {
    uint32_t keyframe_count;
    const int *has_bufs;
    const uint64_t *buf_gpu_addrs;
    const uint64_t *buf_lens;
    const uint64_t *buf_offsets;
    uint64_t stride;
    uint64_t box_count;
    int opaque;
    int allow_duplicate_any_hit;
} rdna4rt_motion_aabb_geom_desc_t;

typedef struct {
    uint64_t stride;
    uint32_t keyframe_count;
    uint32_t primitive_count;
    uint64_t payload_bytes;
    uint32_t vk_geom_flags;
} rdna4rt_motion_aabb_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_motion_aabb_geom(
    const rdna4rt_motion_aabb_geom_desc_t *d, int require_buffers, int allow_motion_blur,
    const rdna4dc_metal3_caps *caps, rdna4rt_motion_aabb_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!allow_motion_blur) return RDNA4RT_ERR_MOTION_UNSUPPORTED;
    if (d->keyframe_count < 2u) return RDNA4RT_ERR_INVALID_ARG;
    if (d->box_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_primitives > 0u && d->box_count > caps->rt_max_primitives)
        return RDNA4RT_ERR_LIMIT;

    uint64_t stride = d->stride ? d->stride : RDNA4RT_AABB_STRIDE_MIN;
    if (stride < RDNA4RT_AABB_STRIDE_MIN || stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((stride % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    if (d->box_count > 0u) {
        if (require_buffers && (!d->has_bufs || !d->buf_gpu_addrs || !d->buf_lens || !d->buf_offsets))
            return RDNA4RT_ERR_INVALID_ARG;
        for (uint32_t kf = 0u; kf < d->keyframe_count; kf++) {
            int has_b = d->has_bufs ? d->has_bufs[kf] : 0;
            if (require_buffers && !has_b) return RDNA4RT_ERR_INVALID_ARG;
            if (!has_b) continue;
            uint64_t addr = d->buf_gpu_addrs ? d->buf_gpu_addrs[kf] : 0u;
            uint64_t len = d->buf_lens ? d->buf_lens[kf] : 0u;
            uint64_t off = d->buf_offsets ? d->buf_offsets[kf] : 0u;
            if (require_buffers && addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (off > len) return RDNA4RT_ERR_BOUNDS;
            if ((off % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (addr != 0u) {
                uint64_t aa = 0u;
                if (!rdna4rt_add_u64(addr, off, &aa)) return RDNA4RT_ERR_OVERFLOW;
                if ((aa % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t span = 0u, need = 0u;
            if (!rdna4rt_mul_u64(d->box_count - 1u, stride, &span) ||
                !rdna4rt_add_u64(span, RDNA4RT_AABB_STRIDE_MIN, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > len - off) return RDNA4RT_ERR_BOUNDS;
        }
    }

    uint64_t prim_kf = 0u, pay_bytes = 0u;
    if (!rdna4rt_mul_u64(d->box_count, (uint64_t)d->keyframe_count, &prim_kf) ||
        !rdna4rt_mul_u64(prim_kf, RDNA4RT_MOTION_AABB_STRIDE, &pay_bytes))
        return RDNA4RT_ERR_OVERFLOW;

    out->stride = stride;
    out->keyframe_count = d->keyframe_count;
    out->primitive_count = (uint32_t)d->box_count;
    out->payload_bytes = pay_bytes;
    out->vk_geom_flags = rdna4rt_geom_flags_to_vk(d->opaque, d->allow_duplicate_any_hit);
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_motion_tlas_layout_compute(
    uint32_t instance_count,
    uint32_t xform_count,
    rdna4rt_xform_type_t xform_type,
    uint64_t *hdr_off_out,
    uint64_t *inst_meta_off_out,
    uint64_t *xform_data_off_out,
    uint64_t *total_bytes_out) {
    if (!hdr_off_out || !inst_meta_off_out || !xform_data_off_out || !total_bytes_out)
        return RDNA4RT_ERR_INVALID_ARG;
    if (xform_type > RDNA4RT_XFORM_COMPONENT) return RDNA4RT_ERR_INVALID_ARG;

    uint64_t vk_inst_bytes = 0u, hdr_off = 0u, inst_off = 0u, xform_off = 0u;
    uint64_t inst_bytes = 0u, xf_elem = 0u, xf_bytes = 0u, end_off = 0u, tot = 0u;
    if (!rdna4rt_mul_u64((uint64_t)instance_count, RDNA4RT_VK_INSTANCE_SIZE, &vk_inst_bytes) ||
        !rdna4rt_align_up_u64(vk_inst_bytes, RDNA4RT_PRIM_SLICE_ALIGN, &hdr_off) ||
        !rdna4rt_add_u64(hdr_off, (uint64_t)sizeof(rdna4rt_motion_tlas_hdr_t), &inst_off) ||
        !rdna4rt_align_up_u64(inst_off, RDNA4RT_PRIM_SLICE_ALIGN, &inst_off) ||
        !rdna4rt_mul_u64((uint64_t)instance_count, (uint64_t)sizeof(rdna4rt_motion_tlas_inst_t), &inst_bytes) ||
        !rdna4rt_add_u64(inst_off, inst_bytes, &xform_off) ||
        !rdna4rt_align_up_u64(xform_off, RDNA4RT_PRIM_SLICE_ALIGN, &xform_off))
        return RDNA4RT_ERR_OVERFLOW;

    xf_elem = (xform_type == RDNA4RT_XFORM_COMPONENT)
        ? RDNA4RT_MTL_COMPONENT_TRANSFORM_SIZE : RDNA4RT_TRANSFORM_SIZE;
    if (!rdna4rt_mul_u64((uint64_t)xform_count, xf_elem, &xf_bytes) ||
        !rdna4rt_add_u64(xform_off, xf_bytes, &end_off) ||
        !rdna4rt_align_up_u64(end_off, RDNA4RT_PRIM_SLICE_ALIGN, &tot))
        return RDNA4RT_ERR_OVERFLOW;

    *hdr_off_out = hdr_off;
    *inst_meta_off_out = inst_off;
    *xform_data_off_out = xform_off;
    *total_bytes_out = tot;
    return RDNA4RT_OK;
}

typedef struct {
    float v[3][3];
} rdna4rt_tri_verts_t;

static inline void rdna4rt_v3_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline int rdna4rt_cpu_intersect_tri(
    const float ro[3], const float rd[3], float tmin, float tmax,
    const rdna4rt_tri_verts_t *tri, int cw_winding, uint32_t cull_mode,
    float *t_out, float *u_out, float *v_out, int *front_out) {
    if (!ro || !rd || !tri || tmax < tmin) return 0;
    float e1[3] = { tri->v[1][0] - tri->v[0][0], tri->v[1][1] - tri->v[0][1], tri->v[1][2] - tri->v[0][2] };
    float e2[3] = { tri->v[2][0] - tri->v[0][0], tri->v[2][1] - tri->v[0][1], tri->v[2][2] - tri->v[0][2] };
    float n[3];
    rdna4rt_v3_cross(e1, e2, n);
    float facing = rdna4rt_v3_dot(n, rd);
    /* Vulkan treats dot((v1-v0) x (v2-v0), D) < 0 as CCW front-facing; invert for Metal CW. */
    int ccw_front = (facing < 0.0f) ? 1 : 0;
    int is_front = cw_winding ? !ccw_front : ccw_front;
    if (cull_mode == 1u && is_front) return 0;  /* MTLCullModeFront */
    if (cull_mode == 2u && !is_front) return 0; /* MTLCullModeBack */

    float h[3];
    rdna4rt_v3_cross(rd, e2, h);
    float a = rdna4rt_v3_dot(e1, h);
    if (fabsf(a) < 1e-10f) return 0;
    float f = 1.0f / a;
    float s[3] = { ro[0] - tri->v[0][0], ro[1] - tri->v[0][1], ro[2] - tri->v[0][2] };
    float u = f * rdna4rt_v3_dot(s, h);
    if (u < 0.0f || u > 1.0f) return 0;
    float q[3];
    rdna4rt_v3_cross(s, e1, q);
    float v = f * rdna4rt_v3_dot(rd, q);
    if (v < 0.0f || u + v > 1.0f) return 0;
    float t_hit = f * rdna4rt_v3_dot(e2, q);
    if (t_hit < tmin || t_hit > tmax) return 0;
    if (t_out) *t_out = t_hit;
    if (u_out) *u_out = u;
    if (v_out) *v_out = v;
    if (front_out) *front_out = is_front;
    return 1;
}

static inline int rdna4rt_cpu_intersect_motion_tri(
    const float ro[3], const float rd[3], float tmin, float tmax, float ray_time,
    const rdna4rt_tri_verts_t *kf_tris, uint32_t kf_count,
    float start_time, float end_time, uint32_t start_border, uint32_t end_border,
    int cw_winding, uint32_t cull_mode,
    float *t_out, float *u_out, float *v_out, int *front_out) {
    if (!kf_tris || kf_count == 0u) return 0;
    uint32_t k0 = 0u, k1 = 0u;
    float alpha = 0.0f;
    if (rdna4rt_motion_time_eval(ray_time, start_time, end_time,
                                 start_border, end_border, kf_count,
                                 &k0, &k1, &alpha) != 1)
        return 0;
    rdna4rt_tri_verts_t interp;
    for (int vi = 0; vi < 3; vi++) {
        for (int ci = 0; ci < 3; ci++) {
            float a = kf_tris[k0].v[vi][ci], b = kf_tris[k1].v[vi][ci];
            interp.v[vi][ci] = a + (b - a) * alpha;
        }
    }
    return rdna4rt_cpu_intersect_tri(ro, rd, tmin, tmax, &interp, cw_winding, cull_mode,
                                     t_out, u_out, v_out, front_out);
}

static inline void rdna4rt_transform_3x4_point_vec(
    const rdna4rt_vk_transform_t *m, const float in_p[3], const float in_d[3],
    float out_p[3], float out_d[3]) {
    if (out_p && in_p) {
        for (int rw = 0; rw < 3; rw++) {
            out_p[rw] = m->matrix[rw][0] * in_p[0] + m->matrix[rw][1] * in_p[1] +
                        m->matrix[rw][2] * in_p[2] + m->matrix[rw][3];
        }
    }
    if (out_d && in_d) {
        for (int rw = 0; rw < 3; rw++) {
            out_d[rw] = m->matrix[rw][0] * in_d[0] + m->matrix[rw][1] * in_d[1] +
                        m->matrix[rw][2] * in_d[2];
        }
    }
}

static inline int rdna4rt_cpu_intersect_motion_inst_tri(
    const float ro[3], const float rd[3], float tmin, float tmax, float ray_time,
    rdna4rt_xform_type_t xform_type, const void *xform_keys, uint32_t inst_kf_count,
    float inst_start_time, float inst_end_time,
    uint32_t inst_start_border, uint32_t inst_end_border,
    const rdna4rt_tri_verts_t *prim_kf_tris, uint32_t prim_kf_count,
    float prim_start_time, float prim_end_time,
    uint32_t prim_start_border, uint32_t prim_end_border,
    int cw_winding, uint32_t cull_mode,
    float *t_out, float *u_out, float *v_out, int *front_out) {
    if (!xform_keys || inst_kf_count == 0u) return 0;
    uint32_t k0 = 0u, k1 = 0u;
    float alpha = 0.0f;
    if (rdna4rt_motion_time_eval(ray_time, inst_start_time, inst_end_time,
                                 inst_start_border, inst_end_border, inst_kf_count,
                                 &k0, &k1, &alpha) != 1)
        return 0;

    rdna4rt_vk_transform_t world_from_obj;
    if (xform_type == RDNA4RT_XFORM_COMPONENT) {
        const rdna4rt_component_transform_t *keys =
            (const rdna4rt_component_transform_t *)xform_keys;
        if (rdna4rt_component_transform_interp(&keys[k0], &keys[k1], alpha, &world_from_obj) != RDNA4RT_OK)
            return 0;
    } else if (xform_type == RDNA4RT_XFORM_PACKED_FLOAT4X3) {
        const rdna4rt_packed_float4x3_t *keys =
            (const rdna4rt_packed_float4x3_t *)xform_keys;
        rdna4rt_vk_transform_t m0, m1;
        if (rdna4rt_transpose_4x3_to_3x4(&keys[k0], &m0) != RDNA4RT_OK ||
            rdna4rt_transpose_4x3_to_3x4(&keys[k1], &m1) != RDNA4RT_OK ||
            rdna4rt_transform_3x4_lerp(&m0, &m1, alpha, &world_from_obj) != RDNA4RT_OK)
            return 0;
    } else {
        return 0;
    }

    rdna4rt_vk_transform_t obj_from_world;
    if (!rdna4rt_transform_3x4_invert(&world_from_obj, &obj_from_world)) return 0;
    float obj_ro[3], obj_rd[3];
    rdna4rt_transform_3x4_point_vec(&obj_from_world, ro, rd, obj_ro, obj_rd);
    return rdna4rt_cpu_intersect_motion_tri(
        obj_ro, obj_rd, tmin, tmax, ray_time,
        prim_kf_tris, prim_kf_count, prim_start_time, prim_end_time,
        prim_start_border, prim_end_border,
        cw_winding, cull_mode, t_out, u_out, v_out, front_out);
}


#endif /* GARNET_RT_MOTION_H */
