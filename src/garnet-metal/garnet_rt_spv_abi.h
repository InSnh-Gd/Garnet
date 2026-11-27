// garnet_rt_spv_abi.h - Shared push-constant ABI structs and packing helpers for
// GarnetMetal GPU-driven ray-tracing compute kernels.
#ifndef GARNET_RT_SPV_ABI_H
#define GARNET_RT_SPV_ABI_H

#include <stdint.h>

typedef struct {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint64_t blas_table_addr;
    uint64_t count_addr;
    uint32_t max_count;
    uint32_t src_stride;
    uint32_t desc_type;
    uint32_t blas_count;
} rdna4rt_inst_convert_pc_t;

typedef struct {
    uint64_t bda;
    uint32_t geom_ift_base;
    uint32_t _pad;
} rdna4rt_blas_table_entry_t;

typedef struct {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t layout_mode;
    uint32_t _pad;
} rdna4rt_xform_transpose_pc_t;

_Static_assert(sizeof(rdna4rt_inst_convert_pc_t) == 48u, "inst convert push constant size");
_Static_assert(sizeof(rdna4rt_blas_table_entry_t) == 16u, "blas table entry size");
_Static_assert(sizeof(rdna4rt_xform_transpose_pc_t) == 24u, "xform transpose push constant size");

typedef struct {
    uint64_t cp_addr;
    uint64_t rad_addr;
    uint64_t idx_addr;
    uint64_t aabb_dst_addr;
    uint64_t seg_dst_addr;
    uint32_t segment_count;
    uint32_t control_point_count;
    uint32_t cp_stride;
    uint32_t rad_stride;
    uint32_t geom_index;
    uint32_t packed_flags;
} rdna4rt_curve_build_pc_t;

static inline uint32_t rdna4rt_curve_pack_flags(
    uint32_t cp_fmt, uint32_t rad_fmt, uint32_t idx_type,
    uint32_t curve_basis, uint32_t seg_cp_count, uint32_t sub_splits) {
    return (cp_fmt & 0xFu) |
           ((rad_fmt & 0xFu) << 4u) |
           ((idx_type & 0xFu) << 8u) |
           ((curve_basis & 0xFu) << 12u) |
           ((seg_cp_count & 0xFu) << 16u) |
           ((sub_splits & 0xFu) << 20u);
}

_Static_assert(sizeof(rdna4rt_curve_build_pc_t) == 64u, "curve build push constant size");

typedef struct {
    uint64_t kf_addrs_bda;
    uint64_t idx_addr;
    uint64_t xform_addr;
    uint64_t aabb_dst_addr;
    uint64_t payload_dst_addr;
    uint64_t blas_aabb_addr;
    uint32_t prim_count;
    uint32_t keyframe_count;
    uint32_t src_stride;
    uint32_t packed_flags;
} rdna4rt_motion_blas_build_pc_t;

static inline uint32_t rdna4rt_motion_blas_pack_flags(
    uint32_t vfmt, uint32_t idx_type, uint32_t geom_kind, uint32_t xform_row_major) {
    return (vfmt & 0xFu) |
           ((idx_type & 0xFu) << 4u) |
           ((geom_kind & 0xFu) << 8u) |
           ((xform_row_major & 0x1u) << 12u);
}

typedef struct {
    uint64_t blas_bda;
    uint64_t local_aabb_bda;
    uint32_t geom_ift_base;
    uint32_t _pad;
} rdna4rt_motion_blas_entry_t;

typedef struct {
    uint64_t inst_src_addr;
    uint64_t xform_src_addr;
    uint64_t blas_table_addr;
    uint64_t count_addr;
    uint64_t vk_inst_dst_addr;
    uint64_t inst_meta_dst_addr;
    uint64_t unit_box_blas_bda;
    uint32_t max_count;
    uint32_t src_stride;
    uint32_t blas_count;
    uint32_t xform_count;
    uint32_t packed_flags;
    uint32_t _pad;
} rdna4rt_motion_tlas_build_pc_t;

_Static_assert(sizeof(rdna4rt_motion_blas_build_pc_t) == 64u, "motion blas build push constant size");
_Static_assert(sizeof(rdna4rt_motion_blas_entry_t) == 24u, "motion blas entry size");
_Static_assert(sizeof(rdna4rt_motion_tlas_build_pc_t) == 80u, "motion tlas build push constant size");

#endif /* GARNET_RT_SPV_ABI_H */
