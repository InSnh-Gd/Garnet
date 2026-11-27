// Pure-C Metal 3 -> VK_KHR_acceleration_structure validation, sizing/alignment,
// instance descriptor conversion, and lifecycle state machine (host-testable).
#ifndef RDNA4_RT_ACCEL_H
#define RDNA4_RT_ACCEL_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "garnet_devcaps.h"

#define RDNA4RT_AS_SIZE_ALIGN               UINT64_C(256)
#define RDNA4RT_AABB_STRIDE_MIN             UINT64_C(24)
#define RDNA4RT_AABB_ALIGN                  UINT64_C(8)
#define RDNA4RT_TRANSFORM_SIZE              UINT64_C(48)
#define RDNA4RT_TRANSFORM_ALIGN             UINT64_C(16)
#define RDNA4RT_VK_INSTANCE_SIZE            UINT64_C(64)
#define RDNA4RT_VK_INSTANCE_ALIGN           UINT64_C(16)
#define RDNA4RT_MTL_INSTANCE_DEFAULT_SIZE   UINT64_C(64)
#define RDNA4RT_MTL_INSTANCE_USERID_SIZE    UINT64_C(68)
#define RDNA4RT_MTL_INSTANCE_INDIRECT_SIZE  UINT64_C(72)
#define RDNA4RT_MTL_INSTANCE_MOTION_SIZE    UINT64_C(44)
#define RDNA4RT_STAGING_POOL_MIN_BUCKET     UINT64_C(4096)
#define RDNA4RT_STAGING_POOL_MAX_BUCKET     (UINT64_C(64) << 20)
#define RDNA4RT_STAGING_POOL_MAX_SLABS      32u
#define RDNA4RT_PRIM_POOL_MIN_BUCKET        RDNA4RT_STAGING_POOL_MIN_BUCKET
#define RDNA4RT_PRIM_POOL_MAX_BUCKET        RDNA4RT_STAGING_POOL_MAX_BUCKET
#define RDNA4RT_PRIM_POOL_MAX_SLABS         RDNA4RT_STAGING_POOL_MAX_SLABS
#define RDNA4RT_QUERY_POOL_MAX_FREE         8u
#define RDNA4RT_INSTANCE_CUSTOM_INDEX_MAX   UINT32_C(0x00FFFFFF)
#define RDNA4RT_INSTANCE_SBT_OFFSET_MAX     UINT32_C(0x00FFFFFF)

/* Metal MTLAccelerationStructureUsage bit values (macOS 11..15). */
enum {
    RDNA4RT_USAGE_NONE                     = 0u,
    RDNA4RT_USAGE_REFIT                    = 1u << 0,
    RDNA4RT_USAGE_PREFER_FAST_BUILD        = 1u << 1,
    RDNA4RT_USAGE_EXTENDED_LIMITS          = 1u << 2,
    RDNA4RT_USAGE_PREFER_FAST_INTERSECTION = 1u << 4,
    RDNA4RT_USAGE_MINIMIZE_MEMORY          = 1u << 5,
    RDNA4RT_USAGE_KNOWN_MASK               = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5),
};

/* Metal MTLAccelerationStructureInstanceOptions bit values. */
enum {
    RDNA4RT_INST_OPT_NONE              = 0u,
    RDNA4RT_INST_OPT_DISABLE_CULLING   = 1u << 0,
    RDNA4RT_INST_OPT_CCW               = 1u << 1,
    RDNA4RT_INST_OPT_OPAQUE            = 1u << 2,
    RDNA4RT_INST_OPT_NON_OPAQUE        = 1u << 3,
    RDNA4RT_INST_OPT_KNOWN_MASK        = 0xFu,
};

/* Metal MTLAccelerationStructureInstanceDescriptorType values. */
typedef enum {
    RDNA4RT_INST_DESC_DEFAULT         = 0u,
    RDNA4RT_INST_DESC_USER_ID         = 1u,
    RDNA4RT_INST_DESC_MOTION          = 2u,
    RDNA4RT_INST_DESC_INDIRECT        = 3u,
    RDNA4RT_INST_DESC_INDIRECT_MOTION = 4u,
} rdna4rt_inst_desc_type_t;

/* Metal MTLMatrixLayout values (macOS 15+). */
typedef enum {
    RDNA4RT_MATRIX_COLUMN_MAJOR = 0u,
    RDNA4RT_MATRIX_ROW_MAJOR    = 1u,
} rdna4rt_matrix_layout_t;

/* Metal MTLMotionBorderMode values (macOS 12+). */
typedef enum {
    RDNA4RT_MOTION_BORDER_CLAMP  = 0u,
    RDNA4RT_MOTION_BORDER_VANISH = 1u,
} rdna4rt_motion_border_t;

/* Metal MTLAccelerationStructureRefitOptions values (macOS 13+). */
enum {
    RDNA4RT_REFIT_OPT_VERTEX_DATA        = 1u << 0,
    RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA = 1u << 1,
    RDNA4RT_REFIT_OPT_KNOWN_MASK         = 0x3u,
};

typedef enum {
    RDNA4RT_INDEX_NONE   = 0u,
    RDNA4RT_INDEX_UINT16 = 1u,
    RDNA4RT_INDEX_UINT32 = 2u,
} rdna4rt_index_type_t;

typedef enum {
    RDNA4RT_VFMT_FLOAT3       = 0u,
    RDNA4RT_VFMT_FLOAT2       = 1u,
    RDNA4RT_VFMT_HALF2        = 2u,
    RDNA4RT_VFMT_HALF4        = 3u,
    RDNA4RT_VFMT_SHORT2_NORM  = 4u,
    RDNA4RT_VFMT_SHORT4_NORM  = 5u,
    RDNA4RT_VFMT_USHORT2_NORM = 6u,
    RDNA4RT_VFMT_USHORT4_NORM = 7u,
    RDNA4RT_VFMT_COUNT        = 8u,
} rdna4rt_vfmt_t;

typedef enum {
    RDNA4RT_SIZE_TYPE_UINT32 = 4u,
    RDNA4RT_SIZE_TYPE_UINT64 = 8u,
} rdna4rt_size_type_t;

/* Vulkan VK_KHR_acceleration_structure flag mirrors (verified when vulkan.h is included). */
enum {
    RDNA4RT_VK_GEOM_OPAQUE               = 0x00000001u,
    RDNA4RT_VK_GEOM_NO_DUPLICATE_ANY_HIT = 0x00000002u,
};

enum {
    RDNA4RT_VK_INST_DISABLE_CULLING = 0x00000001u,
    RDNA4RT_VK_INST_FLIP_FACING     = 0x00000002u,
    RDNA4RT_VK_INST_FORCE_OPAQUE    = 0x00000004u,
    RDNA4RT_VK_INST_FORCE_NO_OPAQUE = 0x00000008u,
};

enum {
    RDNA4RT_VK_BUILD_ALLOW_UPDATE      = 0x00000001u,
    RDNA4RT_VK_BUILD_ALLOW_COMPACTION  = 0x00000002u,
    RDNA4RT_VK_BUILD_PREFER_FAST_TRACE = 0x00000004u,
    RDNA4RT_VK_BUILD_PREFER_FAST_BUILD = 0x00000008u,
    RDNA4RT_VK_BUILD_LOW_MEMORY        = 0x00000010u,
    RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS = 0x00000800u,
};

enum {
    RDNA4RT_VK_COPY_CLONE   = 0u,
    RDNA4RT_VK_COPY_COMPACT = 1u,
};

typedef enum {
    RDNA4RT_GEOM_TRIANGLES = 0u,
    RDNA4RT_GEOM_AABBS     = 1u,
    RDNA4RT_GEOM_INSTANCES = 2u,
    RDNA4RT_GEOM_CURVES    = 3u,
} rdna4rt_geom_kind_t;

typedef enum {
    RDNA4RT_KIND_UNKNOWN = 0u,
    RDNA4RT_KIND_BLAS    = 1u,
    RDNA4RT_KIND_TLAS    = 2u,
} rdna4rt_kind_t;

typedef enum {
    RDNA4RT_STATE_UNBUILT   = 0u,
    RDNA4RT_STATE_BUILT     = 1u,
    RDNA4RT_STATE_COMPACTED = 2u,
} rdna4rt_state_t;

typedef enum {
    RDNA4RT_OK = 0,
    RDNA4RT_ERR_INVALID_ARG,
    RDNA4RT_ERR_UNSUPPORTED,
    RDNA4RT_ERR_MOTION_UNSUPPORTED,
    RDNA4RT_ERR_LIMIT,
    RDNA4RT_ERR_OVERFLOW,
    RDNA4RT_ERR_ALIGNMENT,
    RDNA4RT_ERR_BOUNDS,
    RDNA4RT_ERR_CONFLICT,
    RDNA4RT_ERR_STATE,
    RDNA4RT_ERR_NAN_TRANSFORM,
    RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
} rdna4rt_err_t;

static inline const char *rdna4rt_err_str(rdna4rt_err_t e) {
    switch (e) {
    case RDNA4RT_OK:                     return "ok";
    case RDNA4RT_ERR_INVALID_ARG:        return "invalid acceleration structure argument";
    case RDNA4RT_ERR_UNSUPPORTED:        return "unsupported acceleration structure feature";
    case RDNA4RT_ERR_MOTION_UNSUPPORTED: return "primitive/instance motion blur is unsupported";
    case RDNA4RT_ERR_LIMIT:              return "acceleration structure exceeds backend limits";
    case RDNA4RT_ERR_OVERFLOW:           return "acceleration structure size or offset overflow";
    case RDNA4RT_ERR_ALIGNMENT:          return "acceleration structure buffer or stride misalignment";
    case RDNA4RT_ERR_BOUNDS:             return "acceleration structure buffer range out of bounds";
    case RDNA4RT_ERR_CONFLICT:           return "conflicting acceleration structure flags";
    case RDNA4RT_ERR_STATE:              return "invalid acceleration structure lifecycle transition";
    case RDNA4RT_ERR_NAN_TRANSFORM:      return "non-finite (NaN/Inf) transform matrix";
    case RDNA4RT_ERR_OUT_OF_RANGE_INDEX: return "instance accelerationStructureIndex or bitfield out of range";
    default:                             return "unknown acceleration structure error";
    }
}

/* Packed Metal / Vulkan wire structures. */
typedef struct {
    float columns[4][3];
} rdna4rt_packed_float4x3_t;

typedef struct {
    float matrix[3][4];
} rdna4rt_vk_transform_t;

typedef struct {
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;
} rdna4rt_vk_aabb_t;

/* Bit-exact 64-byte layout matching VkAccelerationStructureInstanceKHR on little-endian. */
typedef struct {
    rdna4rt_vk_transform_t transform;
    uint32_t custom_index_and_mask;  /* bits [0..23]: instanceCustomIndex, [24..31]: mask */
    uint32_t sbt_offset_and_flags;   /* bits [0..23]: instanceShaderBindingTableRecordOffset, [24..31]: flags */
    uint64_t accel_struct_reference; /* 64-bit BLAS VkDeviceAddress (0 = inactive) */
} rdna4rt_vk_instance_t;

typedef struct {
    rdna4rt_packed_float4x3_t transform;
    uint32_t options;
    uint32_t mask;
    uint32_t intersection_function_offset;
    uint32_t accel_struct_index;
} rdna4rt_mtl_instance_default_t;

typedef struct {
    rdna4rt_packed_float4x3_t transform;
    uint32_t options;
    uint32_t mask;
    uint32_t intersection_function_offset;
    uint32_t accel_struct_index;
    uint32_t user_id;
} rdna4rt_mtl_instance_userid_t;

typedef struct {
    rdna4rt_packed_float4x3_t transform;
    uint32_t options;
    uint32_t mask;
    uint32_t intersection_function_offset;
    uint32_t user_id;
    uint64_t accel_struct_id;
} rdna4rt_mtl_instance_indirect_t;

typedef struct {
    uint32_t options;
    uint32_t mask;
    uint32_t intersection_function_offset;
    uint32_t accel_struct_index;
    uint32_t user_id;
    uint32_t motion_transforms_start_index;
    uint32_t motion_transforms_count;
    uint32_t motion_start_border_mode;
    uint32_t motion_end_border_mode;
    float motion_start_time;
    float motion_end_time;
} rdna4rt_mtl_instance_motion_t;

_Static_assert(sizeof(rdna4rt_packed_float4x3_t) == 48u, "packed float4x3 must be 48 bytes");
_Static_assert(sizeof(rdna4rt_vk_transform_t) == 48u, "VkTransformMatrixKHR must be 48 bytes");
_Static_assert(sizeof(rdna4rt_vk_aabb_t) == 24u, "VkAabbPositionsKHR must be 24 bytes");
_Static_assert(sizeof(rdna4rt_vk_instance_t) == 64u, "VkAccelerationStructureInstanceKHR must be 64 bytes");
_Static_assert(sizeof(rdna4rt_mtl_instance_default_t) == 64u, "MTLAccelerationStructureInstanceDescriptor must be 64 bytes");
_Static_assert(sizeof(rdna4rt_mtl_instance_userid_t) == 68u, "MTLAccelerationStructureUserIDInstanceDescriptor must be 68 bytes");
_Static_assert(sizeof(rdna4rt_mtl_instance_indirect_t) == 72u, "MTLIndirectAccelerationStructureInstanceDescriptor must be 72 bytes");
_Static_assert(sizeof(rdna4rt_mtl_instance_motion_t) == 44u, "MTLAccelerationStructureMotionInstanceDescriptor must be 44 bytes");

#ifdef VK_KHR_acceleration_structure
_Static_assert(sizeof(rdna4rt_vk_transform_t) == sizeof(VkTransformMatrixKHR), "VkTransformMatrixKHR size mismatch");
_Static_assert(sizeof(rdna4rt_vk_aabb_t) == sizeof(VkAabbPositionsKHR), "VkAabbPositionsKHR size mismatch");
_Static_assert(sizeof(rdna4rt_vk_instance_t) == sizeof(VkAccelerationStructureInstanceKHR), "VkAccelerationStructureInstanceKHR size mismatch");
_Static_assert(offsetof(rdna4rt_vk_instance_t, transform) == offsetof(VkAccelerationStructureInstanceKHR, transform), "instance transform offset");
_Static_assert(offsetof(rdna4rt_vk_instance_t, accel_struct_reference) == offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference), "instance AS ref offset");
_Static_assert(RDNA4RT_VK_GEOM_OPAQUE == (uint32_t)VK_GEOMETRY_OPAQUE_BIT_KHR, "VK_GEOMETRY_OPAQUE_BIT_KHR");
_Static_assert(RDNA4RT_VK_GEOM_NO_DUPLICATE_ANY_HIT == (uint32_t)VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR, "VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR");
_Static_assert(RDNA4RT_VK_INST_DISABLE_CULLING == (uint32_t)VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, "VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR");
_Static_assert(RDNA4RT_VK_INST_FLIP_FACING == (uint32_t)VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR, "VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR");
_Static_assert(RDNA4RT_VK_INST_FORCE_OPAQUE == (uint32_t)VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR, "VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR");
_Static_assert(RDNA4RT_VK_INST_FORCE_NO_OPAQUE == (uint32_t)VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR, "VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR");
_Static_assert(RDNA4RT_VK_BUILD_ALLOW_UPDATE == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR, "ALLOW_UPDATE");
_Static_assert(RDNA4RT_VK_BUILD_ALLOW_COMPACTION == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR, "ALLOW_COMPACTION");
_Static_assert(RDNA4RT_VK_BUILD_PREFER_FAST_TRACE == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, "PREFER_FAST_TRACE");
_Static_assert(RDNA4RT_VK_BUILD_PREFER_FAST_BUILD == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, "PREFER_FAST_BUILD");
_Static_assert(RDNA4RT_VK_BUILD_LOW_MEMORY == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR, "LOW_MEMORY");
#ifdef VK_KHR_ray_tracing_position_fetch
_Static_assert(RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS == (uint32_t)VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR, "ALLOW_DATA_ACCESS");
#endif
_Static_assert(RDNA4RT_VK_COPY_CLONE == (uint32_t)VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR, "COPY_CLONE");
_Static_assert(RDNA4RT_VK_COPY_COMPACT == (uint32_t)VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR, "COPY_COMPACT");
#endif

/* Overflow-safe arithmetic and alignment helpers. */
static inline int rdna4rt_is_pow2_u64(uint64_t x) {
    return x != 0u && (x & (x - 1u)) == 0u;
}

static inline int rdna4rt_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out || b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static inline int rdna4rt_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out) return 0;
    if (a != 0u && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static inline int rdna4rt_align_up_u64(uint64_t val, uint64_t align, uint64_t *out) {
    if (!out || !rdna4rt_is_pow2_u64(align)) return 0;
    if (val > UINT64_MAX - (align - 1u)) return 0;
    *out = (val + (align - 1u)) & ~(align - 1u);
    return 1;
}

/* Capability gates (honest: never claim hardware support not probed). */
static inline int rdna4rt_accel_supported(const rdna4dc_metal3_caps *caps) {
    return caps && caps->deferred_host_ops && caps->acceleration_structure;
}

static inline int rdna4rt_raytracing_supported(const rdna4dc_metal3_caps *caps) {
    return rdna4dc_metal3_raytracing_supported(caps);
}

static inline int rdna4rt_motion_blur_supported(const rdna4dc_metal3_caps *caps) {
    (void)caps;
    return 0; /* RADV GFX12 does not expose VK_NV_ray_tracing_motion_blur. */
}

static inline uint32_t rdna4rt_scratch_align(const rdna4dc_metal3_caps *caps) {
    if (!rdna4rt_accel_supported(caps)) return 0u;
    uint32_t a = caps->rt_min_scratch_alignment;
    return (a != 0u && rdna4rt_is_pow2_u64((uint64_t)a)) ? a : (uint32_t)RDNA4RT_AS_SIZE_ALIGN;
}

/* Usage -> VkBuildAccelerationStructureFlagsKHR. Always sets ALLOW_COMPACTION because
 * Metal permits writeCompactedAccelerationStructureSize/copyAndCompact on any built AS. */
static inline rdna4rt_err_t rdna4rt_usage_to_vk_build_flags(uint64_t usage, uint32_t *vk_flags_out) {
    if (!vk_flags_out) return RDNA4RT_ERR_INVALID_ARG;
    if (usage & ~(uint64_t)RDNA4RT_USAGE_KNOWN_MASK) return RDNA4RT_ERR_INVALID_ARG;
    if ((usage & RDNA4RT_USAGE_PREFER_FAST_BUILD) && (usage & RDNA4RT_USAGE_PREFER_FAST_INTERSECTION))
        return RDNA4RT_ERR_CONFLICT;
    uint32_t flags = RDNA4RT_VK_BUILD_ALLOW_COMPACTION;
    if (usage & RDNA4RT_USAGE_REFIT) flags |= RDNA4RT_VK_BUILD_ALLOW_UPDATE;
    if (usage & RDNA4RT_USAGE_PREFER_FAST_BUILD) flags |= RDNA4RT_VK_BUILD_PREFER_FAST_BUILD;
    else flags |= RDNA4RT_VK_BUILD_PREFER_FAST_TRACE;
    if (usage & RDNA4RT_USAGE_MINIMIZE_MEMORY) flags |= RDNA4RT_VK_BUILD_LOW_MEMORY;
    *vk_flags_out = flags;
    return RDNA4RT_OK;
}

static inline uint32_t rdna4rt_blas_build_flags_with_caps(
    uint32_t base_flags, int has_triangles, const rdna4dc_metal3_caps *caps) {
    if (has_triangles && rdna4dc_metal3_rt_position_fetch_supported(caps))
        base_flags |= RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS;
    return base_flags;
}

typedef struct {
    uint64_t acceleration_structure_size;
    uint64_t build_scratch_size;
    uint64_t refit_scratch_size;
} rdna4rt_sizes_t;

/* Pads scratch sizes by (min_scratch_align - 1) so any client scratchBufferOffset can
 * be aligned up to minAccelerationStructureScratchOffsetAlignment in-bounds. */
static inline rdna4rt_err_t rdna4rt_compute_sizes(
    uint64_t vk_as_size, uint64_t vk_build_scratch, uint64_t vk_update_scratch,
    uint64_t usage, uint32_t min_scratch_align, rdna4rt_sizes_t *out) {
    if (!out) return RDNA4RT_ERR_INVALID_ARG;
    uint32_t unused_flags = 0;
    rdna4rt_err_t ue = rdna4rt_usage_to_vk_build_flags(usage, &unused_flags);
    if (ue != RDNA4RT_OK) return ue;
    if (!rdna4rt_is_pow2_u64((uint64_t)min_scratch_align)) return RDNA4RT_ERR_ALIGNMENT;

    uint64_t as_raw = vk_as_size ? vk_as_size : RDNA4RT_AS_SIZE_ALIGN;
    uint64_t as_sz = 0;
    if (!rdna4rt_align_up_u64(as_raw, RDNA4RT_AS_SIZE_ALIGN, &as_sz)) return RDNA4RT_ERR_OVERFLOW;

    uint64_t build_sz = 0;
    if (vk_build_scratch > 0u) {
        uint64_t padded = 0;
        if (!rdna4rt_add_u64(vk_build_scratch, (uint64_t)(min_scratch_align - 1u), &padded) ||
            !rdna4rt_align_up_u64(padded, (uint64_t)min_scratch_align, &build_sz))
            return RDNA4RT_ERR_OVERFLOW;
    }

    uint64_t refit_sz = 0;
    if ((usage & RDNA4RT_USAGE_REFIT) && vk_update_scratch > 0u) {
        uint64_t padded = 0;
        if (!rdna4rt_add_u64(vk_update_scratch, (uint64_t)(min_scratch_align - 1u), &padded) ||
            !rdna4rt_align_up_u64(padded, (uint64_t)min_scratch_align, &refit_sz))
            return RDNA4RT_ERR_OVERFLOW;
    }

    out->acceleration_structure_size = as_sz;
    out->build_scratch_size = build_sz;
    out->refit_scratch_size = refit_sz;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_resolve_scratch_addr(
    uint64_t buf_gpu_addr, uint64_t buf_len, uint64_t offset,
    uint64_t required_vk_scratch, uint32_t min_scratch_align, uint64_t *aligned_addr_out) {
    if (!aligned_addr_out) return RDNA4RT_ERR_INVALID_ARG;
    if (!rdna4rt_is_pow2_u64((uint64_t)min_scratch_align)) return RDNA4RT_ERR_ALIGNMENT;
    if (required_vk_scratch == 0u) {
        if (offset > buf_len) return RDNA4RT_ERR_BOUNDS;
        if (buf_gpu_addr == 0u) { *aligned_addr_out = 0u; return RDNA4RT_OK; }
        uint64_t raw0 = 0, al0 = 0;
        if (!rdna4rt_add_u64(buf_gpu_addr, offset, &raw0) ||
            !rdna4rt_align_up_u64(raw0, (uint64_t)min_scratch_align, &al0))
            return RDNA4RT_ERR_OVERFLOW;
        if (al0 - raw0 > buf_len - offset) return RDNA4RT_ERR_BOUNDS;
        *aligned_addr_out = al0;
        return RDNA4RT_OK;
    }
    if (buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (offset > buf_len) return RDNA4RT_ERR_BOUNDS;
    uint64_t raw = 0, aligned = 0;
    if (!rdna4rt_add_u64(buf_gpu_addr, offset, &raw) ||
        !rdna4rt_align_up_u64(raw, (uint64_t)min_scratch_align, &aligned))
        return RDNA4RT_ERR_OVERFLOW;
    uint64_t pad = aligned - raw;
    uint64_t avail = buf_len - offset;
    if (pad > avail || required_vk_scratch > avail - pad) return RDNA4RT_ERR_BOUNDS;
    *aligned_addr_out = aligned;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_validate_compacted_size_write(
    uint64_t buf_len, uint64_t offset, uint32_t size_type_bytes) {
    if (size_type_bytes != RDNA4RT_SIZE_TYPE_UINT32 && size_type_bytes != RDNA4RT_SIZE_TYPE_UINT64)
        return RDNA4RT_ERR_INVALID_ARG;
    if ((offset % (uint64_t)size_type_bytes) != 0u) return RDNA4RT_ERR_ALIGNMENT;
    if (offset > buf_len || (uint64_t)size_type_bytes > buf_len - offset) return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

/* Vertex format properties. */
static inline int rdna4rt_vfmt_info(rdna4rt_vfmt_t fmt, uint32_t *elem_bytes_out, uint32_t *align_out) {
    uint32_t sz = 0, al = 0;
    switch (fmt) {
    case RDNA4RT_VFMT_FLOAT3:       sz = 12u; al = 4u; break;
    case RDNA4RT_VFMT_FLOAT2:       sz = 8u;  al = 4u; break;
    case RDNA4RT_VFMT_HALF2:        sz = 4u;  al = 2u; break;
    case RDNA4RT_VFMT_HALF4:        sz = 8u;  al = 2u; break;
    case RDNA4RT_VFMT_SHORT2_NORM:  sz = 4u;  al = 2u; break;
    case RDNA4RT_VFMT_SHORT4_NORM:  sz = 8u;  al = 2u; break;
    case RDNA4RT_VFMT_USHORT2_NORM: sz = 4u;  al = 2u; break;
    case RDNA4RT_VFMT_USHORT4_NORM: sz = 8u;  al = 2u; break;
    default: return 0;
    }
    if (elem_bytes_out) *elem_bytes_out = sz;
    if (align_out) *align_out = al;
    return 1;
}

static inline uint32_t rdna4rt_geom_flags_to_vk(int opaque, int allow_duplicate_any_hit) {
    uint32_t f = 0u;
    if (opaque) f |= RDNA4RT_VK_GEOM_OPAQUE;
    if (!allow_duplicate_any_hit) f |= RDNA4RT_VK_GEOM_NO_DUPLICATE_ANY_HIT;
    return f;
}

/* Triangle BLAS geometry descriptor & validator. */
typedef struct {
    rdna4rt_vfmt_t vertex_format;
    int has_vertex_buf;
    uint64_t vertex_buf_gpu_addr;
    uint64_t vertex_buf_len;
    uint64_t vertex_buf_offset;
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
} rdna4rt_tri_desc_t;

typedef struct {
    uint32_t vertex_elem_bytes;
    uint32_t vertex_align;
    uint64_t vertex_stride;
    uint64_t vertex_addr;
    uint32_t max_vertex;
    uint32_t primitive_count;
    rdna4rt_index_type_t effective_index_type;
    uint64_t index_addr;
    uint64_t transform_addr;
    int needs_transform_staging;
    uint32_t vk_geom_flags;
} rdna4rt_tri_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_triangle_geom(
    const rdna4rt_tri_desc_t *d, int require_buffers, const rdna4dc_metal3_caps *caps,
    rdna4rt_tri_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint32_t elem_bytes = 0, comp_align = 0;
    if (!rdna4rt_vfmt_info(d->vertex_format, &elem_bytes, &comp_align))
        return RDNA4RT_ERR_UNSUPPORTED;
    if (d->triangle_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_primitives > 0u && d->triangle_count > caps->rt_max_primitives)
        return RDNA4RT_ERR_LIMIT;

    uint64_t stride = d->vertex_stride ? d->vertex_stride : (uint64_t)elem_bytes;
    if (stride < (uint64_t)elem_bytes || stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((stride % (uint64_t)comp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    /* Metal defaults indexType to UInt32 even when indexBuffer == nil. */
    rdna4rt_index_type_t idx_type = d->has_index_buf ? d->index_type : RDNA4RT_INDEX_NONE;
    uint64_t idx_elem = 0;
    if (idx_type == RDNA4RT_INDEX_UINT16) idx_elem = 2u;
    else if (idx_type == RDNA4RT_INDEX_UINT32) idx_elem = 4u;
    else if (idx_type != RDNA4RT_INDEX_NONE) return RDNA4RT_ERR_INVALID_ARG;

    uint64_t tri_verts = 0;
    if (!rdna4rt_mul_u64(d->triangle_count, 3u, &tri_verts)) return RDNA4RT_ERR_OVERFLOW;

    uint64_t v_addr = 0, i_addr = 0, t_addr = 0;
    uint32_t max_vertex = 0;
    if (d->triangle_count > 0u) {
        if (require_buffers && !d->has_vertex_buf) return RDNA4RT_ERR_INVALID_ARG;
        if (d->has_vertex_buf) {
            if (require_buffers && d->vertex_buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (d->vertex_buf_offset > d->vertex_buf_len) return RDNA4RT_ERR_BOUNDS;
            if (d->vertex_buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(d->vertex_buf_gpu_addr, d->vertex_buf_offset, &v_addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((v_addr % (uint64_t)comp_align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            } else if ((d->vertex_buf_offset % (uint64_t)comp_align) != 0u) {
                return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t v_avail = d->vertex_buf_len - d->vertex_buf_offset;
            if (idx_type == RDNA4RT_INDEX_NONE) {
                if (tri_verts - 1u > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
                uint64_t span = 0, need = 0;
                if (!rdna4rt_mul_u64(tri_verts - 1u, stride, &span) ||
                    !rdna4rt_add_u64(span, (uint64_t)elem_bytes, &need))
                    return RDNA4RT_ERR_OVERFLOW;
                if (need > v_avail) return RDNA4RT_ERR_BOUNDS;
                max_vertex = (uint32_t)(tri_verts - 1u);
            } else {
                if (v_avail < (uint64_t)elem_bytes) return RDNA4RT_ERR_BOUNDS;
                uint64_t mv = (v_avail - (uint64_t)elem_bytes) / stride;
                uint64_t mv_cap = (idx_type == RDNA4RT_INDEX_UINT16) ? UINT16_MAX : UINT32_MAX;
                max_vertex = (uint32_t)(mv > mv_cap ? mv_cap : mv);
            }
        } else {
            /* Size-query mode without bound vertex buffer: estimate maxVertex from triangleCount. */
            if (tri_verts - 1u > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
            max_vertex = (uint32_t)(tri_verts - 1u);
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
            uint64_t idx_bytes = 0;
            if (!rdna4rt_mul_u64(tri_verts, idx_elem, &idx_bytes)) return RDNA4RT_ERR_OVERFLOW;
            if (idx_bytes > d->index_buf_len - d->index_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
    }

    int needs_staging = 0;
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
        /* Vulkan transformData is 3x4 row-major and 16-byte aligned; Metal defaults to column-major 4x3. */
        if (d->transform_layout == RDNA4RT_MATRIX_COLUMN_MAJOR ||
            (t_addr % RDNA4RT_TRANSFORM_ALIGN) != 0u)
            needs_staging = 1;
    }

    out->vertex_elem_bytes = elem_bytes;
    out->vertex_align = comp_align;
    out->vertex_stride = stride;
    out->vertex_addr = v_addr;
    out->max_vertex = max_vertex;
    out->primitive_count = (uint32_t)d->triangle_count;
    out->effective_index_type = idx_type;
    out->index_addr = i_addr;
    out->transform_addr = t_addr;
    out->needs_transform_staging = needs_staging;
    out->vk_geom_flags = rdna4rt_geom_flags_to_vk(d->opaque, d->allow_duplicate_any_hit);
    return RDNA4RT_OK;
}

/* Bounding-box (AABB) BLAS geometry descriptor & validator. */
typedef struct {
    int has_buf;
    uint64_t buf_gpu_addr;
    uint64_t buf_len;
    uint64_t buf_offset;
    uint64_t stride;
    uint64_t box_count;
    int opaque;
    int allow_duplicate_any_hit;
} rdna4rt_aabb_desc_t;

typedef struct {
    uint64_t stride;
    uint64_t data_addr;
    uint32_t primitive_count;
    uint32_t vk_geom_flags;
} rdna4rt_aabb_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_aabb_geom(
    const rdna4rt_aabb_desc_t *d, int require_buffers, const rdna4dc_metal3_caps *caps,
    rdna4rt_aabb_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (d->box_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_primitives > 0u && d->box_count > caps->rt_max_primitives)
        return RDNA4RT_ERR_LIMIT;

    uint64_t stride = d->stride ? d->stride : RDNA4RT_AABB_STRIDE_MIN;
    if (stride < RDNA4RT_AABB_STRIDE_MIN || stride > UINT32_MAX) return RDNA4RT_ERR_INVALID_ARG;
    if ((stride % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    uint64_t addr = 0;
    if (d->box_count > 0u) {
        if (require_buffers && !d->has_buf) return RDNA4RT_ERR_INVALID_ARG;
        if (d->has_buf) {
            if (require_buffers && d->buf_gpu_addr == 0u) return RDNA4RT_ERR_INVALID_ARG;
            if (d->buf_offset > d->buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((d->buf_offset % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (d->buf_gpu_addr != 0u) {
                if (!rdna4rt_add_u64(d->buf_gpu_addr, d->buf_offset, &addr))
                    return RDNA4RT_ERR_OVERFLOW;
                if ((addr % RDNA4RT_AABB_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            }
            uint64_t span = 0, need = 0;
            if (!rdna4rt_mul_u64(d->box_count - 1u, stride, &span) ||
                !rdna4rt_add_u64(span, RDNA4RT_AABB_STRIDE_MIN, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > d->buf_len - d->buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
    }

    out->stride = stride;
    out->data_addr = addr;
    out->primitive_count = (uint32_t)d->box_count;
    out->vk_geom_flags = rdna4rt_geom_flags_to_vk(d->opaque, d->allow_duplicate_any_hit);
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_validate_blas_totals(
    uint64_t geom_count, uint64_t total_primitives, uint64_t usage,
    const rdna4dc_metal3_caps *caps) {
    uint32_t vk_flags = 0;
    rdna4rt_err_t ue = rdna4rt_usage_to_vk_build_flags(usage, &vk_flags);
    if (ue != RDNA4RT_OK) return ue;
    if (geom_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_geometries > 0u && geom_count > caps->rt_max_geometries)
        return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_primitives > 0u && total_primitives > caps->rt_max_primitives)
        return RDNA4RT_ERR_LIMIT;
    return RDNA4RT_OK;
}

/* Motion blur parameter validator & honest capability gate. */
typedef struct {
    int is_motion;
    uint32_t keyframe_count;
    uint32_t keyframe_buffer_count;
    uint32_t start_border_mode;
    uint32_t end_border_mode;
    float start_time;
    float end_time;
} rdna4rt_motion_params_t;

static inline rdna4rt_err_t rdna4rt_validate_motion_params(
    const rdna4rt_motion_params_t *mp, int allow_motion_blur) {
    if (!mp) return RDNA4RT_ERR_INVALID_ARG;
    if (!mp->is_motion) return RDNA4RT_OK;
    if (!isfinite(mp->start_time) || !isfinite(mp->end_time) || mp->start_time > mp->end_time)
        return RDNA4RT_ERR_INVALID_ARG;
    if (mp->start_border_mode > RDNA4RT_MOTION_BORDER_VANISH ||
        mp->end_border_mode > RDNA4RT_MOTION_BORDER_VANISH)
        return RDNA4RT_ERR_INVALID_ARG;
    if (mp->keyframe_count < 2u) return RDNA4RT_ERR_INVALID_ARG;
    if (mp->keyframe_buffer_count != 0u && mp->keyframe_buffer_count != mp->keyframe_count)
        return RDNA4RT_ERR_INVALID_ARG;
    if (!allow_motion_blur) return RDNA4RT_ERR_MOTION_UNSUPPORTED;
    return RDNA4RT_OK;
}

/* Transform matrix validation & exact 4x3 column-major -> 3x4 row-major transpose. */
static inline rdna4rt_err_t rdna4rt_transpose_4x3_to_3x4(
    const rdna4rt_packed_float4x3_t *src, rdna4rt_vk_transform_t *dst) {
    if (!src || !dst) return RDNA4RT_ERR_INVALID_ARG;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 3; r++) {
            float v = src->columns[c][r];
            if (!isfinite(v)) return RDNA4RT_ERR_NAN_TRANSFORM;
            dst->matrix[r][c] = v;
        }
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_validate_transform_3x4(
    const rdna4rt_vk_transform_t *src, rdna4rt_vk_transform_t *dst) {
    if (!src || !dst) return RDNA4RT_ERR_INVALID_ARG;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            float v = src->matrix[r][c];
            if (!isfinite(v)) return RDNA4RT_ERR_NAN_TRANSFORM;
            dst->matrix[r][c] = v;
        }
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_instance_options_to_vk(uint32_t mtl_opts, uint8_t *vk_flags_out) {
    if (!vk_flags_out) return RDNA4RT_ERR_INVALID_ARG;
    if (mtl_opts & ~(uint32_t)RDNA4RT_INST_OPT_KNOWN_MASK) return RDNA4RT_ERR_INVALID_ARG;
    if ((mtl_opts & RDNA4RT_INST_OPT_OPAQUE) && (mtl_opts & RDNA4RT_INST_OPT_NON_OPAQUE))
        return RDNA4RT_ERR_CONFLICT;
    uint8_t f = 0u;
    if (mtl_opts & RDNA4RT_INST_OPT_DISABLE_CULLING) f |= (uint8_t)RDNA4RT_VK_INST_DISABLE_CULLING;
    if (mtl_opts & RDNA4RT_INST_OPT_CCW)             f |= (uint8_t)RDNA4RT_VK_INST_FLIP_FACING;
    if (mtl_opts & RDNA4RT_INST_OPT_OPAQUE)          f |= (uint8_t)RDNA4RT_VK_INST_FORCE_OPAQUE;
    if (mtl_opts & RDNA4RT_INST_OPT_NON_OPAQUE)      f |= (uint8_t)RDNA4RT_VK_INST_FORCE_NO_OPAQUE;
    *vk_flags_out = f;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_pack_vk_instance(
    const rdna4rt_vk_transform_t *xform, uint32_t custom_index, uint32_t mask,
    uint32_t sbt_offset, uint32_t mtl_options, uint64_t blas_addr,
    rdna4rt_vk_instance_t *dst) {
    if (!xform || !dst) return RDNA4RT_ERR_INVALID_ARG;
    if (custom_index > RDNA4RT_INSTANCE_CUSTOM_INDEX_MAX ||
        sbt_offset > RDNA4RT_INSTANCE_SBT_OFFSET_MAX)
        return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    uint8_t vk_flags = 0;
    rdna4rt_err_t oe = rdna4rt_instance_options_to_vk(mtl_options, &vk_flags);
    if (oe != RDNA4RT_OK) return oe;
    dst->transform = *xform;
    /* Metal uses the low 8 bits of instance mask (matching Vulkan's 8-bit mask). */
    dst->custom_index_and_mask = (custom_index & RDNA4RT_INSTANCE_CUSTOM_INDEX_MAX) |
                                 ((mask & 0xFFu) << 24);
    dst->sbt_offset_and_flags = (sbt_offset & RDNA4RT_INSTANCE_SBT_OFFSET_MAX) |
                                ((uint32_t)vk_flags << 24);
    dst->accel_struct_reference = blas_addr;
    return RDNA4RT_OK;
}

/* TLAS descriptor validator. */
typedef struct {
    rdna4rt_inst_desc_type_t descriptor_type;
    uint64_t usage;
    uint64_t instance_count;
    int has_inst_buf;
    uint64_t inst_buf_len;
    uint64_t inst_buf_offset;
    uint64_t inst_stride;
    uint64_t blas_count;
    int has_motion_transform_buf;
    uint64_t motion_transform_buf_len;
    uint64_t motion_transform_buf_offset;
    uint64_t motion_transform_count;
} rdna4rt_tlas_desc_t;

typedef struct {
    uint64_t elem_size;
    uint64_t stride;
    uint32_t instance_count;
    uint32_t vk_build_flags;
} rdna4rt_tlas_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_tlas_desc(
    const rdna4rt_tlas_desc_t *d, int require_buffers, int allow_motion_blur,
    const rdna4dc_metal3_caps *caps, rdna4rt_tlas_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint32_t vk_flags = 0;
    rdna4rt_err_t ue = rdna4rt_usage_to_vk_build_flags(d->usage, &vk_flags);
    if (ue != RDNA4RT_OK) return ue;
    if (d->instance_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_instances > 0u && d->instance_count > caps->rt_max_instances)
        return RDNA4RT_ERR_LIMIT;

    uint64_t min_size = 0;
    switch (d->descriptor_type) {
    case RDNA4RT_INST_DESC_DEFAULT:
        min_size = RDNA4RT_MTL_INSTANCE_DEFAULT_SIZE;
        break;
    case RDNA4RT_INST_DESC_USER_ID:
        min_size = RDNA4RT_MTL_INSTANCE_USERID_SIZE;
        break;
    case RDNA4RT_INST_DESC_MOTION:
        if (!allow_motion_blur) return RDNA4RT_ERR_MOTION_UNSUPPORTED;
        min_size = RDNA4RT_MTL_INSTANCE_MOTION_SIZE;
        break;
    case RDNA4RT_INST_DESC_INDIRECT:
        min_size = RDNA4RT_MTL_INSTANCE_INDIRECT_SIZE;
        break;
    case RDNA4RT_INST_DESC_INDIRECT_MOTION:
        return allow_motion_blur ? RDNA4RT_ERR_UNSUPPORTED : RDNA4RT_ERR_MOTION_UNSUPPORTED;
    default:
        return RDNA4RT_ERR_INVALID_ARG;
    }

    uint64_t stride = d->inst_stride ? d->inst_stride : min_size;
    if (stride < min_size || (stride % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    if (d->instance_count > 0u) {
        if (require_buffers && !d->has_inst_buf) return RDNA4RT_ERR_INVALID_ARG;
        if (d->has_inst_buf) {
            if (d->inst_buf_offset > d->inst_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((d->inst_buf_offset % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            uint64_t span = 0, need = 0;
            if (!rdna4rt_mul_u64(d->instance_count - 1u, stride, &span) ||
                !rdna4rt_add_u64(span, min_size, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > d->inst_buf_len - d->inst_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
        if (d->descriptor_type == RDNA4RT_INST_DESC_MOTION &&
            (require_buffers || d->has_motion_transform_buf)) {
            if (!d->has_motion_transform_buf || d->motion_transform_count == 0u)
                return RDNA4RT_ERR_INVALID_ARG;
            if (d->motion_transform_buf_offset > d->motion_transform_buf_len)
                return RDNA4RT_ERR_BOUNDS;
            if ((d->motion_transform_buf_offset % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            uint64_t mbytes = 0;
            if (!rdna4rt_mul_u64(d->motion_transform_count, RDNA4RT_TRANSFORM_SIZE, &mbytes))
                return RDNA4RT_ERR_OVERFLOW;
            if (mbytes > d->motion_transform_buf_len - d->motion_transform_buf_offset)
                return RDNA4RT_ERR_BOUNDS;
        }
    }

    out->elem_size = min_size;
    out->stride = stride;
    out->instance_count = (uint32_t)d->instance_count;
    out->vk_build_flags = vk_flags;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_resolve_blas_ref(
    uint32_t accel_index, uint32_t mask, const uint64_t *blas_addrs, uint32_t blas_count,
    uint64_t *ref_out) {
    if (!ref_out) return RDNA4RT_ERR_INVALID_ARG;
    if (accel_index >= blas_count || !blas_addrs) return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    uint64_t addr = blas_addrs[accel_index];
    if ((mask & 0xFFu) != 0u && addr == 0u) return RDNA4RT_ERR_STATE;
    *ref_out = addr;
    return RDNA4RT_OK;
}

/* Indirect TLAS descriptor validator (MTLIndirectInstanceAccelerationStructureDescriptor). */
typedef struct {
    rdna4rt_inst_desc_type_t descriptor_type;
    uint64_t usage;
    uint64_t max_instance_count;
    int has_inst_buf;
    uint64_t inst_buf_len;
    uint64_t inst_buf_offset;
    uint64_t inst_stride;
    int has_count_buf;
    uint64_t count_buf_len;
    uint64_t count_buf_offset;
} rdna4rt_indirect_tlas_desc_t;

static inline rdna4rt_err_t rdna4rt_validate_indirect_tlas_desc(
    const rdna4rt_indirect_tlas_desc_t *d, int require_buffers, int allow_motion_blur,
    const rdna4dc_metal3_caps *caps, rdna4rt_tlas_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint32_t vk_flags = 0;
    rdna4rt_err_t ue = rdna4rt_usage_to_vk_build_flags(d->usage, &vk_flags);
    if (ue != RDNA4RT_OK) return ue;
    if (d->max_instance_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if (caps && caps->rt_max_instances > 0u && d->max_instance_count > caps->rt_max_instances)
        return RDNA4RT_ERR_LIMIT;

    if (d->descriptor_type == RDNA4RT_INST_DESC_INDIRECT_MOTION)
        return allow_motion_blur ? RDNA4RT_ERR_UNSUPPORTED : RDNA4RT_ERR_MOTION_UNSUPPORTED;
    if (d->descriptor_type != RDNA4RT_INST_DESC_INDIRECT)
        return RDNA4RT_ERR_INVALID_ARG;

    uint64_t min_size = RDNA4RT_MTL_INSTANCE_INDIRECT_SIZE;
    uint64_t stride = d->inst_stride ? d->inst_stride : min_size;
    if (stride < min_size || (stride % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    if (d->max_instance_count > 0u) {
        if (require_buffers && (!d->has_inst_buf || !d->has_count_buf))
            return RDNA4RT_ERR_INVALID_ARG;
        if (d->has_inst_buf) {
            if (d->inst_buf_offset > d->inst_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((d->inst_buf_offset % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            uint64_t span = 0, need = 0;
            if (!rdna4rt_mul_u64(d->max_instance_count - 1u, stride, &span) ||
                !rdna4rt_add_u64(span, min_size, &need))
                return RDNA4RT_ERR_OVERFLOW;
            if (need > d->inst_buf_len - d->inst_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
        if (d->has_count_buf) {
            if (d->count_buf_offset > d->count_buf_len) return RDNA4RT_ERR_BOUNDS;
            if ((d->count_buf_offset % 4u) != 0u) return RDNA4RT_ERR_ALIGNMENT;
            if (4u > d->count_buf_len - d->count_buf_offset) return RDNA4RT_ERR_BOUNDS;
        }
    }

    out->elem_size = min_size;
    out->stride = stride;
    out->instance_count = (uint32_t)d->max_instance_count;
    out->vk_build_flags = vk_flags;
    return RDNA4RT_OK;
}

/* Staging pool bucket rounding: power-of-2 buckets in [4 KiB, 64 MiB].
 * Returns 1 with bucket size if poolable, 0 if allocation exceeds pool cap (allocate directly). */
static inline int rdna4rt_staging_bucket_size(uint64_t size, uint64_t *bucket_out) {
    if (!bucket_out) return 0;
    if (size > RDNA4RT_STAGING_POOL_MAX_BUCKET) return 0;
    uint64_t b = RDNA4RT_STAGING_POOL_MIN_BUCKET;
    while (b < size && b < RDNA4RT_STAGING_POOL_MAX_BUCKET) b <<= 1;
    *bucket_out = b;
    return 1;
}

/* Targeted Vulkan pipeline barrier stage/access specifications (computeQ-safe: zero graphics
 * stage bits when is_compute_queue != 0). */
enum {
    RDNA4RT_VK_STAGE_VERTEX_SHADER   = 0x00000008u,
    RDNA4RT_VK_STAGE_FRAGMENT_SHADER = 0x00000080u,
    RDNA4RT_VK_STAGE_COMPUTE_SHADER  = 0x00000800u,
    RDNA4RT_VK_STAGE_TRANSFER        = 0x00001000u,
    RDNA4RT_VK_STAGE_HOST            = 0x00004000u,
    RDNA4RT_VK_STAGE_ALL_GRAPHICS    = 0x00008000u,
    RDNA4RT_VK_STAGE_AS_BUILD        = 0x02000000u,

    RDNA4RT_VK_ACCESS_SHADER_READ    = 0x00000020u,
    RDNA4RT_VK_ACCESS_SHADER_WRITE   = 0x00000040u,
    RDNA4RT_VK_ACCESS_TRANSFER_READ  = 0x00000800u,
    RDNA4RT_VK_ACCESS_TRANSFER_WRITE = 0x00001000u,
    RDNA4RT_VK_ACCESS_HOST_READ      = 0x00002000u,
    RDNA4RT_VK_ACCESS_AS_READ        = 0x00200000u,
    RDNA4RT_VK_ACCESS_AS_WRITE       = 0x00400000u,
};

#ifdef VK_KHR_acceleration_structure
_Static_assert(RDNA4RT_VK_STAGE_VERTEX_SHADER   == (uint32_t)VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, "STAGE_VERTEX");
_Static_assert(RDNA4RT_VK_STAGE_FRAGMENT_SHADER == (uint32_t)VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, "STAGE_FRAGMENT");
_Static_assert(RDNA4RT_VK_STAGE_COMPUTE_SHADER  == (uint32_t)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, "STAGE_COMPUTE");
_Static_assert(RDNA4RT_VK_STAGE_TRANSFER        == (uint32_t)VK_PIPELINE_STAGE_TRANSFER_BIT, "STAGE_TRANSFER");
_Static_assert(RDNA4RT_VK_STAGE_HOST            == (uint32_t)VK_PIPELINE_STAGE_HOST_BIT, "STAGE_HOST");
_Static_assert(RDNA4RT_VK_STAGE_ALL_GRAPHICS    == (uint32_t)VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, "STAGE_ALL_GRAPHICS");
_Static_assert(RDNA4RT_VK_STAGE_AS_BUILD        == (uint32_t)VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, "STAGE_AS_BUILD");
_Static_assert(RDNA4RT_VK_ACCESS_SHADER_READ    == (uint32_t)VK_ACCESS_SHADER_READ_BIT, "ACCESS_SHADER_READ");
_Static_assert(RDNA4RT_VK_ACCESS_SHADER_WRITE   == (uint32_t)VK_ACCESS_SHADER_WRITE_BIT, "ACCESS_SHADER_WRITE");
_Static_assert(RDNA4RT_VK_ACCESS_TRANSFER_READ  == (uint32_t)VK_ACCESS_TRANSFER_READ_BIT, "ACCESS_TRANSFER_READ");
_Static_assert(RDNA4RT_VK_ACCESS_TRANSFER_WRITE == (uint32_t)VK_ACCESS_TRANSFER_WRITE_BIT, "ACCESS_TRANSFER_WRITE");
_Static_assert(RDNA4RT_VK_ACCESS_HOST_READ      == (uint32_t)VK_ACCESS_HOST_READ_BIT, "ACCESS_HOST_READ");
_Static_assert(RDNA4RT_VK_ACCESS_AS_READ        == (uint32_t)VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, "ACCESS_AS_READ");
_Static_assert(RDNA4RT_VK_ACCESS_AS_WRITE       == (uint32_t)VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, "ACCESS_AS_WRITE");
#endif

typedef enum {
    RDNA4RT_BARRIER_PRE_BUILD = 0,
    RDNA4RT_BARRIER_COMP_TO_BUILD,
    RDNA4RT_BARRIER_POST_BUILD,
    RDNA4RT_BARRIER_TRANSFER_TO_HOST,
} rdna4rt_barrier_kind_t;

typedef struct {
    uint32_t src_stage;
    uint32_t dst_stage;
    uint32_t src_access;
    uint32_t dst_access;
} rdna4rt_barrier_spec_t;

static inline rdna4rt_barrier_spec_t rdna4rt_barrier_spec(rdna4rt_barrier_kind_t kind, int is_compute_queue) {
    rdna4rt_barrier_spec_t s = { 0u, 0u, 0u, 0u };
    switch (kind) {
    case RDNA4RT_BARRIER_PRE_BUILD:
        s.src_stage = RDNA4RT_VK_STAGE_COMPUTE_SHADER | RDNA4RT_VK_STAGE_TRANSFER | RDNA4RT_VK_STAGE_AS_BUILD |
                      (is_compute_queue ? 0u : RDNA4RT_VK_STAGE_ALL_GRAPHICS);
        s.dst_stage = RDNA4RT_VK_STAGE_AS_BUILD | RDNA4RT_VK_STAGE_COMPUTE_SHADER | RDNA4RT_VK_STAGE_TRANSFER;
        s.src_access = RDNA4RT_VK_ACCESS_SHADER_WRITE | RDNA4RT_VK_ACCESS_TRANSFER_WRITE | RDNA4RT_VK_ACCESS_AS_WRITE;
        s.dst_access = RDNA4RT_VK_ACCESS_SHADER_READ | RDNA4RT_VK_ACCESS_AS_READ | RDNA4RT_VK_ACCESS_AS_WRITE |
                       RDNA4RT_VK_ACCESS_TRANSFER_READ | RDNA4RT_VK_ACCESS_TRANSFER_WRITE;
        break;
    case RDNA4RT_BARRIER_COMP_TO_BUILD:
        s.src_stage = RDNA4RT_VK_STAGE_COMPUTE_SHADER;
        s.dst_stage = RDNA4RT_VK_STAGE_AS_BUILD | RDNA4RT_VK_STAGE_TRANSFER;
        s.src_access = RDNA4RT_VK_ACCESS_SHADER_WRITE;
        s.dst_access = RDNA4RT_VK_ACCESS_SHADER_READ | RDNA4RT_VK_ACCESS_AS_READ |
                       RDNA4RT_VK_ACCESS_TRANSFER_READ | RDNA4RT_VK_ACCESS_TRANSFER_WRITE;
        break;
    case RDNA4RT_BARRIER_POST_BUILD:
        s.src_stage = RDNA4RT_VK_STAGE_AS_BUILD | RDNA4RT_VK_STAGE_TRANSFER | RDNA4RT_VK_STAGE_COMPUTE_SHADER;
        s.dst_stage = RDNA4RT_VK_STAGE_COMPUTE_SHADER | RDNA4RT_VK_STAGE_AS_BUILD | RDNA4RT_VK_STAGE_TRANSFER |
                      (is_compute_queue ? 0u : (RDNA4RT_VK_STAGE_VERTEX_SHADER | RDNA4RT_VK_STAGE_FRAGMENT_SHADER));
        s.src_access = RDNA4RT_VK_ACCESS_AS_WRITE | RDNA4RT_VK_ACCESS_TRANSFER_WRITE | RDNA4RT_VK_ACCESS_SHADER_WRITE;
        s.dst_access = RDNA4RT_VK_ACCESS_AS_READ | RDNA4RT_VK_ACCESS_AS_WRITE |
                       RDNA4RT_VK_ACCESS_SHADER_READ | RDNA4RT_VK_ACCESS_TRANSFER_READ;
        break;
    case RDNA4RT_BARRIER_TRANSFER_TO_HOST:
    default:
        s.src_stage = RDNA4RT_VK_STAGE_TRANSFER | RDNA4RT_VK_STAGE_AS_BUILD;
        s.dst_stage = RDNA4RT_VK_STAGE_HOST | RDNA4RT_VK_STAGE_TRANSFER | RDNA4RT_VK_STAGE_COMPUTE_SHADER |
                      (is_compute_queue ? 0u : (RDNA4RT_VK_STAGE_VERTEX_SHADER | RDNA4RT_VK_STAGE_FRAGMENT_SHADER));
        s.src_access = RDNA4RT_VK_ACCESS_TRANSFER_WRITE | RDNA4RT_VK_ACCESS_AS_WRITE;
        s.dst_access = RDNA4RT_VK_ACCESS_HOST_READ | RDNA4RT_VK_ACCESS_TRANSFER_READ | RDNA4RT_VK_ACCESS_SHADER_READ;
        break;
    }
    return s;
}

/* Validate and accumulate BLAS per-geometry intersectionFunctionTableOffset values.
 * Metal evaluates ift_slot = instance.iftOffset + geometry[g].iftOffset + fto.
 * Vulkan ray query evaluates instanceSBT + geometryIndex + fto, which matches
 * Metal exactly for every geometry g whenever geometry[g].iftOffset == base + g
 * (including every single-geometry BLAS). Sets non_consecutive = 1 if any
 * multi-geometry BLAS deviates from base + g so runtime can log a diagnostic. */
typedef struct {
    uint32_t count;
    uint32_t base;
    int non_consecutive;
} rdna4rt_geom_ift_acc_t;

static inline rdna4rt_err_t rdna4rt_geom_ift_acc_add(
    rdna4rt_geom_ift_acc_t *acc, uint64_t geom_ift_offset) {
    if (!acc) return RDNA4RT_ERR_INVALID_ARG;
    if (geom_ift_offset > (uint64_t)RDNA4RT_INSTANCE_SBT_OFFSET_MAX)
        return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    if (acc->count == 0u) {
        acc->base = (uint32_t)geom_ift_offset;
    } else if (geom_ift_offset != (uint64_t)acc->base + (uint64_t)acc->count) {
        acc->non_consecutive = 1;
    }
    acc->count++;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_compute_geom_ift_base(
    const uint64_t *geom_ift_offsets, uint32_t geom_count,
    uint32_t *base_out, int *non_consecutive_out) {
    if (!base_out) return RDNA4RT_ERR_INVALID_ARG;
    *base_out = 0u;
    if (non_consecutive_out) *non_consecutive_out = 0;
    if (geom_count == 0u) return RDNA4RT_OK;
    if (!geom_ift_offsets) return RDNA4RT_ERR_INVALID_ARG;
    rdna4rt_geom_ift_acc_t acc = { 0u, 0u, 0 };
    for (uint32_t g = 0u; g < geom_count; g++) {
        rdna4rt_err_t e = rdna4rt_geom_ift_acc_add(&acc, geom_ift_offsets[g]);
        if (e != RDNA4RT_OK) return e;
    }
    *base_out = acc.base;
    if (non_consecutive_out) *non_consecutive_out = acc.non_consecutive;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_combine_sbt_offset(
    uint32_t inst_ift_offset, uint32_t geom_ift_base, uint32_t *out) {
    if (!out) return RDNA4RT_ERR_INVALID_ARG;
    uint64_t sum = (uint64_t)inst_ift_offset + (uint64_t)geom_ift_base;
    if (sum > (uint64_t)RDNA4RT_INSTANCE_SBT_OFFSET_MAX)
        return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    *out = (uint32_t)sum;
    return RDNA4RT_OK;
}

/* Convert Default (64B), UserID (68B), or Indirect (72B) Metal instance descriptor buffer -> Vulkan 64B instances.
 * When blas_ift_offsets != NULL (Default/UserID), adds blas_ift_offsets[accel_struct_index] into
 * VkAccelerationStructureInstanceKHR.instanceShaderBindingTableRecordOffset with 24-bit overflow checks. */
static inline rdna4rt_err_t rdna4rt_convert_instances_ex(
    rdna4rt_inst_desc_type_t type, const uint8_t *src_bytes, size_t src_len,
    uint64_t src_offset, uint64_t stride, uint32_t count,
    const uint64_t *blas_addrs, const uint32_t *blas_ift_offsets,
    uint32_t blas_count, rdna4rt_vk_instance_t *dst) {
    if (count == 0u) return RDNA4RT_OK;
    if (!src_bytes || !dst) return RDNA4RT_ERR_INVALID_ARG;
    uint64_t min_size = (type == RDNA4RT_INST_DESC_DEFAULT) ? RDNA4RT_MTL_INSTANCE_DEFAULT_SIZE :
                        (type == RDNA4RT_INST_DESC_USER_ID) ? RDNA4RT_MTL_INSTANCE_USERID_SIZE :
                        (type == RDNA4RT_INST_DESC_INDIRECT) ? RDNA4RT_MTL_INSTANCE_INDIRECT_SIZE : 0u;
    if (!min_size) return RDNA4RT_ERR_UNSUPPORTED;
    if (stride == 0u) stride = min_size;
    if (stride < min_size || (stride % 4u) != 0u || (src_offset % 4u) != 0u)
        return RDNA4RT_ERR_ALIGNMENT;
    uint64_t span = 0, need = 0;
    if (!rdna4rt_mul_u64((uint64_t)(count - 1u), stride, &span) ||
        !rdna4rt_add_u64(span, min_size, &need))
        return RDNA4RT_ERR_OVERFLOW;
    if (src_offset > (uint64_t)src_len || need > (uint64_t)src_len - src_offset)
        return RDNA4RT_ERR_BOUNDS;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = src_bytes + (size_t)(src_offset + (uint64_t)i * stride);
        if (type == RDNA4RT_INST_DESC_INDIRECT) {
            rdna4rt_mtl_instance_indirect_t ind;
            memcpy(&ind, p, sizeof(ind));
            rdna4rt_vk_transform_t xf;
            rdna4rt_err_t te = rdna4rt_transpose_4x3_to_3x4(&ind.transform, &xf);
            if (te != RDNA4RT_OK) return te;
            rdna4rt_err_t pe = rdna4rt_pack_vk_instance(
                &xf, ind.user_id, ind.mask, ind.intersection_function_offset,
                ind.options, ind.accel_struct_id, &dst[i]);
            if (pe != RDNA4RT_OK) return pe;
            continue;
        }
        rdna4rt_mtl_instance_userid_t raw;
        memset(&raw, 0, sizeof(raw));
        memcpy(&raw, p, (size_t)min_size);
        uint32_t custom_id = (type == RDNA4RT_INST_DESC_USER_ID) ? raw.user_id : 0u;
        uint64_t blas_ref = 0;
        rdna4rt_err_t re = rdna4rt_resolve_blas_ref(
            raw.accel_struct_index, raw.mask, blas_addrs, blas_count, &blas_ref);
        if (re != RDNA4RT_OK) return re;
        uint32_t geom_ift = blas_ift_offsets ? blas_ift_offsets[raw.accel_struct_index] : 0u;
        uint32_t combined_sbt = 0u;
        rdna4rt_err_t se = rdna4rt_combine_sbt_offset(
            raw.intersection_function_offset, geom_ift, &combined_sbt);
        if (se != RDNA4RT_OK) return se;
        rdna4rt_vk_transform_t xf;
        rdna4rt_err_t te = rdna4rt_transpose_4x3_to_3x4(&raw.transform, &xf);
        if (te != RDNA4RT_OK) return te;
        rdna4rt_err_t pe = rdna4rt_pack_vk_instance(
            &xf, custom_id, raw.mask, combined_sbt, raw.options, blas_ref, &dst[i]);
        if (pe != RDNA4RT_OK) return pe;
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_convert_instances(
    rdna4rt_inst_desc_type_t type, const uint8_t *src_bytes, size_t src_len,
    uint64_t src_offset, uint64_t stride, uint32_t count,
    const uint64_t *blas_addrs, uint32_t blas_count, rdna4rt_vk_instance_t *dst) {
    return rdna4rt_convert_instances_ex(
        type, src_bytes, src_len, src_offset, stride, count, blas_addrs, NULL, blas_count, dst);
}

/* Convert one Metal motion instance descriptor at eval_time -> packed 64B Vulkan instance
 * (for interpolated motion-blur paths and pure-C validation). */
static inline rdna4rt_err_t rdna4rt_convert_instance_motion(
    const rdna4rt_mtl_instance_motion_t *src,
    const rdna4rt_packed_float4x3_t *motion_transforms, uint32_t motion_transform_count,
    float eval_time, const uint64_t *blas_addrs, uint32_t blas_count,
    rdna4rt_vk_instance_t *dst) {
    if (!src || !motion_transforms || !dst) return RDNA4RT_ERR_INVALID_ARG;
    if (!isfinite(eval_time) || !isfinite(src->motion_start_time) || !isfinite(src->motion_end_time) ||
        src->motion_start_time > src->motion_end_time)
        return RDNA4RT_ERR_INVALID_ARG;
    if (src->motion_start_border_mode > RDNA4RT_MOTION_BORDER_VANISH ||
        src->motion_end_border_mode > RDNA4RT_MOTION_BORDER_VANISH)
        return RDNA4RT_ERR_INVALID_ARG;
    if (src->motion_transforms_count == 0u) return RDNA4RT_ERR_INVALID_ARG;
    uint64_t end_idx = (uint64_t)src->motion_transforms_start_index + (uint64_t)src->motion_transforms_count;
    if (end_idx > (uint64_t)motion_transform_count) return RDNA4RT_ERR_BOUNDS;

    uint64_t blas_ref = 0;
    rdna4rt_err_t re = rdna4rt_resolve_blas_ref(
        src->accel_struct_index, src->mask, blas_addrs, blas_count, &blas_ref);
    if (re != RDNA4RT_OK) return re;

    int vanished = 0;
    float clamped = eval_time;
    if (eval_time < src->motion_start_time) {
        if (src->motion_start_border_mode == RDNA4RT_MOTION_BORDER_VANISH) vanished = 1;
        clamped = src->motion_start_time;
    } else if (eval_time > src->motion_end_time) {
        if (src->motion_end_border_mode == RDNA4RT_MOTION_BORDER_VANISH) vanished = 1;
        clamped = src->motion_end_time;
    }

    uint32_t k0 = 0, k1 = 0;
    float alpha = 0.0f;
    if (src->motion_transforms_count > 1u && src->motion_end_time > src->motion_start_time) {
        double u = ((double)clamped - (double)src->motion_start_time) /
                   ((double)src->motion_end_time - (double)src->motion_start_time);
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        uint32_t last = src->motion_transforms_count - 1u;
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

    const rdna4rt_packed_float4x3_t *m0 = &motion_transforms[src->motion_transforms_start_index + k0];
    const rdna4rt_packed_float4x3_t *m1 = &motion_transforms[src->motion_transforms_start_index + k1];
    rdna4rt_packed_float4x3_t interp;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 3; r++) {
            float v0 = m0->columns[c][r], v1 = m1->columns[c][r];
            if (!isfinite(v0) || !isfinite(v1)) return RDNA4RT_ERR_NAN_TRANSFORM;
            interp.columns[c][r] = v0 + (v1 - v0) * alpha;
        }
    }
    rdna4rt_vk_transform_t xf;
    rdna4rt_err_t te = rdna4rt_transpose_4x3_to_3x4(&interp, &xf);
    if (te != RDNA4RT_OK) return te;

    return rdna4rt_pack_vk_instance(
        &xf, src->user_id, vanished ? 0u : src->mask,
        src->intersection_function_offset, src->options,
        vanished ? 0u : blas_ref, dst);
}

/* Compaction / refit / copy lifecycle state machine. */
typedef struct {
    rdna4rt_kind_t kind;
    rdna4rt_state_t state;
    uint64_t capacity_bytes;
    uint64_t built_size_bytes;
    uint64_t usage;
    uint32_t allow_refit;
    uint32_t allow_compaction;
    uint32_t geometry_count;
    uint64_t topology_hash;
    uint32_t compacted_queried;
    uint32_t geom_ift_base;
} rdna4rt_accel_meta_t;

static inline void rdna4rt_meta_init(
    rdna4rt_accel_meta_t *m, uint64_t capacity_bytes, rdna4rt_kind_t initial_kind) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->kind = initial_kind;
    m->state = RDNA4RT_STATE_UNBUILT;
    m->capacity_bytes = capacity_bytes;
}

static inline uint64_t rdna4rt_fnv_mix_u64(uint64_t h, uint64_t v) {
    for (unsigned i = 0u; i < 8u; i++) {
        h ^= (v >> (i * 8u)) & 0xFFu;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static inline uint32_t rdna4rt_geom_sig(
    rdna4rt_geom_kind_t kind, rdna4rt_index_type_t idx_type,
    rdna4rt_vfmt_t vfmt, uint32_t vk_geom_flags) {
    return ((uint32_t)kind & 0xFu) |
           (((uint32_t)idx_type & 0xFu) << 4) |
           (((uint32_t)vfmt & 0xFFu) << 8) |
           ((vk_geom_flags & 0xFFFFu) << 16);
}

static inline uint64_t rdna4rt_topology_hash(
    const uint32_t *geom_kinds, const uint32_t *prim_counts, uint32_t geom_count) {
    uint64_t h = UINT64_C(14695981039346656037);
    h = rdna4rt_fnv_mix_u64(h, (uint64_t)geom_count);
    for (uint32_t i = 0; i < geom_count; i++) {
        h = rdna4rt_fnv_mix_u64(h, (uint64_t)(geom_kinds ? geom_kinds[i] : 0u));
        h = rdna4rt_fnv_mix_u64(h, (uint64_t)(prim_counts ? prim_counts[i] : 0u));
    }
    return h;
}

static inline rdna4rt_err_t rdna4rt_state_can_build(
    const rdna4rt_accel_meta_t *dst, uint64_t required_as_size) {
    if (!dst || required_as_size == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (dst->capacity_bytes < required_as_size) return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_build(
    rdna4rt_accel_meta_t *dst, rdna4rt_kind_t kind, uint64_t required_as_size,
    uint64_t usage, uint32_t geom_count, uint64_t topology_hash) {
    if (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS) return RDNA4RT_ERR_INVALID_ARG;
    rdna4rt_err_t e = rdna4rt_state_can_build(dst, required_as_size);
    if (e != RDNA4RT_OK) return e;
    uint32_t vk_flags = 0;
    e = rdna4rt_usage_to_vk_build_flags(usage, &vk_flags);
    if (e != RDNA4RT_OK) return e;
    dst->kind = kind;
    dst->state = RDNA4RT_STATE_BUILT;
    dst->built_size_bytes = required_as_size;
    dst->usage = usage;
    dst->allow_refit = (usage & RDNA4RT_USAGE_REFIT) ? 1u : 0u;
    dst->allow_compaction = 1u;
    dst->geometry_count = geom_count;
    dst->topology_hash = topology_hash;
    dst->compacted_queried = 0u;
    dst->geom_ift_base = 0u;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_build_ex(
    rdna4rt_accel_meta_t *dst, rdna4rt_kind_t kind, uint64_t required_as_size,
    uint64_t usage, uint32_t geom_count, uint64_t topology_hash,
    uint32_t geom_ift_base) {
    if (geom_ift_base > RDNA4RT_INSTANCE_SBT_OFFSET_MAX)
        return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    rdna4rt_err_t e = rdna4rt_state_apply_build(
        dst, kind, required_as_size, usage, geom_count, topology_hash);
    if (e != RDNA4RT_OK) return e;
    dst->geom_ift_base = (kind == RDNA4RT_KIND_BLAS) ? geom_ift_base : 0u;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_can_write_compacted_size(
    const rdna4rt_accel_meta_t *src) {
    if (!src) return RDNA4RT_ERR_INVALID_ARG;
    if (src->state == RDNA4RT_STATE_UNBUILT || !src->allow_compaction) return RDNA4RT_ERR_STATE;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_write_compacted_size(rdna4rt_accel_meta_t *src) {
    rdna4rt_err_t e = rdna4rt_state_can_write_compacted_size(src);
    if (e != RDNA4RT_OK) return e;
    src->compacted_queried = 1u;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_can_copy(
    const rdna4rt_accel_meta_t *src, const rdna4rt_accel_meta_t *dst, int same_object) {
    if (!src || !dst) return RDNA4RT_ERR_INVALID_ARG;
    if (same_object) return RDNA4RT_ERR_CONFLICT;
    if (src->state == RDNA4RT_STATE_UNBUILT || src->kind == RDNA4RT_KIND_UNKNOWN)
        return RDNA4RT_ERR_STATE;
    if (dst->capacity_bytes < src->built_size_bytes || dst->capacity_bytes == 0u)
        return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_copy(
    const rdna4rt_accel_meta_t *src, rdna4rt_accel_meta_t *dst, int same_object) {
    rdna4rt_err_t e = rdna4rt_state_can_copy(src, dst, same_object);
    if (e != RDNA4RT_OK) return e;
    uint64_t cap = dst->capacity_bytes;
    *dst = *src;
    dst->capacity_bytes = cap;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_can_compact(
    const rdna4rt_accel_meta_t *src, const rdna4rt_accel_meta_t *dst,
    int same_object, uint64_t known_compacted_size) {
    if (!src || !dst) return RDNA4RT_ERR_INVALID_ARG;
    if (same_object) return RDNA4RT_ERR_CONFLICT;
    if (src->state != RDNA4RT_STATE_BUILT || !src->allow_compaction ||
        src->kind == RDNA4RT_KIND_UNKNOWN)
        return RDNA4RT_ERR_STATE;
    if (dst->capacity_bytes == 0u) return RDNA4RT_ERR_BOUNDS;
    if (known_compacted_size > 0u && dst->capacity_bytes < known_compacted_size)
        return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_compact(
    const rdna4rt_accel_meta_t *src, rdna4rt_accel_meta_t *dst,
    int same_object, uint64_t known_compacted_size) {
    rdna4rt_err_t e = rdna4rt_state_can_compact(src, dst, same_object, known_compacted_size);
    if (e != RDNA4RT_OK) return e;
    dst->kind = src->kind;
    dst->state = RDNA4RT_STATE_COMPACTED;
    dst->built_size_bytes = known_compacted_size ? known_compacted_size : dst->capacity_bytes;
    dst->usage = src->usage & ~(uint64_t)RDNA4RT_USAGE_REFIT;
    dst->allow_refit = 0u; /* Compacted BVH cannot be refitted in Vulkan. */
    dst->allow_compaction = 1u;
    dst->geometry_count = src->geometry_count;
    dst->topology_hash = src->topology_hash;
    dst->compacted_queried = 0u;
    dst->geom_ift_base = src->geom_ift_base;
    return RDNA4RT_OK;
}

static inline int rdna4rt_refit_updates_vertices(uint64_t refit_options) {
    return (refit_options & RDNA4RT_REFIT_OPT_VERTEX_DATA) != 0u;
}

static inline int rdna4rt_refit_updates_prim_data(uint64_t refit_options) {
    return (refit_options & RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) != 0u;
}

static inline int rdna4rt_refit_is_prim_data_only(uint64_t refit_options) {
    return refit_options == RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA;
}

static inline rdna4rt_err_t rdna4rt_state_can_refit(
    const rdna4rt_accel_meta_t *src, const rdna4rt_accel_meta_t *dst,
    rdna4rt_kind_t kind, uint64_t required_as_size, uint32_t geom_count,
    uint64_t topology_hash, uint64_t refit_options) {
    if (!src || !dst) return RDNA4RT_ERR_INVALID_ARG;
    if (refit_options == 0u || (refit_options & ~(uint64_t)RDNA4RT_REFIT_OPT_KNOWN_MASK) != 0u)
        return RDNA4RT_ERR_INVALID_ARG;
    const int prim_only = rdna4rt_refit_is_prim_data_only(refit_options);
    const uint64_t need_size = prim_only
        ? (required_as_size ? required_as_size : src->built_size_bytes)
        : required_as_size;
    if (need_size == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (src->state != RDNA4RT_STATE_BUILT || !src->allow_refit) return RDNA4RT_ERR_STATE;
    if (src->kind != kind || src->geometry_count != geom_count) return RDNA4RT_ERR_STATE;
    if (!prim_only && src->topology_hash != topology_hash) return RDNA4RT_ERR_STATE;
    if (prim_only && topology_hash != 0u && src->topology_hash != topology_hash)
        return RDNA4RT_ERR_STATE;
    if (dst->capacity_bytes < need_size || dst->capacity_bytes < src->built_size_bytes)
        return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_refit(
    const rdna4rt_accel_meta_t *src, rdna4rt_accel_meta_t *dst,
    rdna4rt_kind_t kind, uint64_t required_as_size, uint32_t geom_count,
    uint64_t topology_hash, uint64_t refit_options) {
    rdna4rt_err_t e = rdna4rt_state_can_refit(
        src, dst, kind, required_as_size, geom_count, topology_hash, refit_options);
    if (e != RDNA4RT_OK) return e;
    const int prim_only = rdna4rt_refit_is_prim_data_only(refit_options);
    uint64_t cap = dst->capacity_bytes;
    *dst = *src;
    dst->capacity_bytes = cap;
    dst->state = RDNA4RT_STATE_BUILT;
    dst->built_size_bytes = prim_only ? src->built_size_bytes : required_as_size;
    dst->allow_refit = 1u;
    dst->allow_compaction = 1u;
    dst->compacted_queried = 0u;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_refit_ex(
    const rdna4rt_accel_meta_t *src, rdna4rt_accel_meta_t *dst,
    rdna4rt_kind_t kind, uint64_t required_as_size, uint32_t geom_count,
    uint64_t topology_hash, uint64_t refit_options, uint32_t geom_ift_base) {
    if (geom_ift_base > RDNA4RT_INSTANCE_SBT_OFFSET_MAX)
        return RDNA4RT_ERR_OUT_OF_RANGE_INDEX;
    rdna4rt_err_t e = rdna4rt_state_apply_refit(
        src, dst, kind, required_as_size, geom_count, topology_hash, refit_options);
    if (e != RDNA4RT_OK) return e;
    if (kind == RDNA4RT_KIND_BLAS) dst->geom_ift_base = geom_ift_base;
    return RDNA4RT_OK;
}

/* Wave 4.1B: BLAS primitiveDataBuffer validation, GPU snapshot layout, and copy/compact header rebase. */
#define RDNA4RT_PRIM_DATA_ALIGN       UINT64_C(4)
#define RDNA4RT_PRIM_SLICE_ALIGN      UINT64_C(16)
#define RDNA4RT_PRIM_UPDATE_BUF_MAX   UINT64_C(65536)

#ifndef RDNA4RT_GEOM_PRIM_ENTRY_DEFINED
#define RDNA4RT_GEOM_PRIM_ENTRY_DEFINED 1
typedef struct {
    uint64_t data_bda;
    uint32_t stride;
    uint32_t prim_count;
} rdna4rt_geom_prim_entry_t;
#endif

_Static_assert(sizeof(rdna4rt_geom_prim_entry_t) == 16u, "rdna4rt_geom_prim_entry_t must be 16 bytes");

typedef struct {
    int has_buf;
    uint64_t buf_len;
    uint64_t buf_offset;
    uint64_t element_size;
    uint64_t stride;
    uint64_t prim_count;
} rdna4rt_prim_data_geom_desc_t;

typedef struct {
    int present;
    uint32_t stride;
    uint32_t element_size;
    uint32_t prim_count;
    uint64_t src_offset;
    uint64_t copy_bytes;
} rdna4rt_prim_data_geom_plan_t;

static inline rdna4rt_err_t rdna4rt_validate_prim_data_geom(
    const rdna4rt_prim_data_geom_desc_t *d, int require_buffers, rdna4rt_prim_data_geom_plan_t *out) {
    if (!d || !out) return RDNA4RT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!d->has_buf) return RDNA4RT_OK;
    if (d->prim_count > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if ((d->buf_offset % RDNA4RT_PRIM_DATA_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;
    if (d->element_size == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (d->element_size > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if ((d->element_size % RDNA4RT_PRIM_DATA_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    uint64_t stride = d->stride ? d->stride : d->element_size;
    if (stride < d->element_size) return RDNA4RT_ERR_INVALID_ARG;
    if (stride > UINT32_MAX) return RDNA4RT_ERR_LIMIT;
    if ((stride % RDNA4RT_PRIM_DATA_ALIGN) != 0u) return RDNA4RT_ERR_ALIGNMENT;

    uint64_t copy_bytes = 0u;
    if (d->prim_count > 0u) {
        uint64_t span = 0u;
        if (!rdna4rt_mul_u64(d->prim_count - 1u, stride, &span) ||
            !rdna4rt_add_u64(span, d->element_size, &copy_bytes))
            return RDNA4RT_ERR_OVERFLOW;
    }
    if (require_buffers || d->buf_len > 0u) {
        if (d->buf_offset > d->buf_len) return RDNA4RT_ERR_BOUNDS;
        if (copy_bytes > d->buf_len - d->buf_offset) return RDNA4RT_ERR_BOUNDS;
    }

    out->present = 1;
    out->stride = (uint32_t)stride;
    out->element_size = (uint32_t)d->element_size;
    out->prim_count = (uint32_t)d->prim_count;
    out->src_offset = d->buf_offset;
    out->copy_bytes = copy_bytes;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_blas_prim_layout_compute(
    const rdna4rt_prim_data_geom_plan_t *geoms, uint32_t geom_count,
    uint64_t *rel_offsets_out, uint64_t *header_bytes_out,
    uint64_t *total_bytes_out, int *any_prim_data_out) {
    if (!header_bytes_out || !total_bytes_out || !any_prim_data_out) return RDNA4RT_ERR_INVALID_ARG;
    *header_bytes_out = 0u;
    *total_bytes_out = 0u;
    *any_prim_data_out = 0;
    if (geom_count == 0u) return RDNA4RT_OK;
    if (!geoms || !rel_offsets_out) return RDNA4RT_ERR_INVALID_ARG;

    int any = 0;
    for (uint32_t i = 0u; i < geom_count; i++) {
        rel_offsets_out[i] = 0u;
        if (geoms[i].present && geoms[i].prim_count > 0u && geoms[i].copy_bytes > 0u) any = 1;
    }
    if (!any) return RDNA4RT_OK;

    uint64_t hdr_bytes = 0u;
    if (!rdna4rt_mul_u64((uint64_t)geom_count, (uint64_t)sizeof(rdna4rt_geom_prim_entry_t), &hdr_bytes))
        return RDNA4RT_ERR_OVERFLOW;
    uint64_t cursor = hdr_bytes;
    for (uint32_t i = 0u; i < geom_count; i++) {
        if (!geoms[i].present || geoms[i].prim_count == 0u || geoms[i].copy_bytes == 0u) {
            rel_offsets_out[i] = 0u;
            continue;
        }
        uint64_t aligned_off = 0u;
        if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &aligned_off))
            return RDNA4RT_ERR_OVERFLOW;
        rel_offsets_out[i] = aligned_off;
        uint64_t next = 0u;
        if (!rdna4rt_add_u64(aligned_off, geoms[i].copy_bytes, &next))
            return RDNA4RT_ERR_OVERFLOW;
        cursor = next;
    }
    uint64_t final_bytes = 0u;
    if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &final_bytes))
        return RDNA4RT_ERR_OVERFLOW;

    *header_bytes_out = hdr_bytes;
    *total_bytes_out = final_bytes;
    *any_prim_data_out = 1;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_blas_prim_build_headers(
    uint64_t base_bda, const rdna4rt_prim_data_geom_plan_t *geoms,
    const uint64_t *rel_offsets, uint32_t geom_count,
    rdna4rt_geom_prim_entry_t *headers_out) {
    if (geom_count == 0u) return RDNA4RT_OK;
    if (!base_bda || !geoms || !rel_offsets || !headers_out) return RDNA4RT_ERR_INVALID_ARG;
    for (uint32_t i = 0u; i < geom_count; i++) {
        if (!geoms[i].present || geoms[i].prim_count == 0u || geoms[i].copy_bytes == 0u) {
            headers_out[i] = (rdna4rt_geom_prim_entry_t){ 0u, 0u, 0u };
            continue;
        }
        uint64_t addr = 0u;
        if (!rdna4rt_add_u64(base_bda, rel_offsets[i], &addr)) return RDNA4RT_ERR_OVERFLOW;
        headers_out[i] = (rdna4rt_geom_prim_entry_t){
            .data_bda = addr,
            .stride = geoms[i].stride,
            .prim_count = geoms[i].prim_count
        };
    }
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_blas_prim_rebase_headers(
    const rdna4rt_geom_prim_entry_t *src_headers, uint32_t geom_count,
    uint64_t src_base_bda, uint64_t dst_base_bda, uint64_t total_bytes,
    rdna4rt_geom_prim_entry_t *dst_headers) {
    if (geom_count == 0u) return RDNA4RT_OK;
    if (!src_headers || !dst_headers || !src_base_bda || !dst_base_bda || total_bytes == 0u)
        return RDNA4RT_ERR_INVALID_ARG;
    uint64_t hdr_bytes = 0u;
    if (!rdna4rt_mul_u64((uint64_t)geom_count, (uint64_t)sizeof(rdna4rt_geom_prim_entry_t), &hdr_bytes) ||
        hdr_bytes > total_bytes)
        return RDNA4RT_ERR_BOUNDS;
    for (uint32_t i = 0u; i < geom_count; i++) {
        dst_headers[i].stride = src_headers[i].stride;
        dst_headers[i].prim_count = src_headers[i].prim_count;
        if (src_headers[i].data_bda == 0u) {
            dst_headers[i].data_bda = 0u;
            continue;
        }
        if (src_headers[i].data_bda < src_base_bda) return RDNA4RT_ERR_BOUNDS;
        uint64_t rel = src_headers[i].data_bda - src_base_bda;
        if (rel < hdr_bytes || rel >= total_bytes) return RDNA4RT_ERR_BOUNDS;
        uint64_t new_bda = 0u;
        if (!rdna4rt_add_u64(dst_base_bda, rel, &new_bda)) return RDNA4RT_ERR_OVERFLOW;
        dst_headers[i].data_bda = new_bda;
    }
    return RDNA4RT_OK;
}

/* Wave 4.2: MTLHeap placement & automatic acceleration structure size/align/offset validation. */
#define RDNA4RT_HEAP_AS_ALIGN         RDNA4RT_AS_SIZE_ALIGN
#define RDNA4RT_HEAP_STORAGE_SHARED   UINT32_C(0)
#define RDNA4RT_HEAP_STORAGE_MANAGED  UINT32_C(1)
#define RDNA4RT_HEAP_STORAGE_PRIVATE  UINT32_C(2)
#define RDNA4RT_HEAP_TYPE_AUTOMATIC   UINT32_C(0)
#define RDNA4RT_HEAP_TYPE_PLACEMENT   UINT32_C(1)

typedef struct {
    uint64_t size;
    uint64_t align;
} rdna4rt_heap_size_align_t;

static inline rdna4rt_err_t rdna4rt_heap_as_size_align(
    uint64_t requested_size, uint64_t vk_req_size, uint64_t vk_req_align,
    uint64_t buffer_image_granularity, rdna4rt_heap_size_align_t *out) {
    if (!out) return RDNA4RT_ERR_INVALID_ARG;
    out->size = 0u;
    out->align = 0u;
    if (requested_size == 0u || vk_req_size == 0u || vk_req_align == 0u)
        return RDNA4RT_ERR_INVALID_ARG;
    uint64_t gran = buffer_image_granularity ? buffer_image_granularity : 1u;
    if (!rdna4rt_is_pow2_u64(vk_req_align) || !rdna4rt_is_pow2_u64(gran))
        return RDNA4RT_ERR_ALIGNMENT;
    uint64_t align = vk_req_align;
    if (align < RDNA4RT_HEAP_AS_ALIGN) align = RDNA4RT_HEAP_AS_ALIGN;
    if (align < gran) align = gran;
    uint64_t base_sz = vk_req_size > requested_size ? vk_req_size : requested_size;
    uint64_t extent = 0u;
    if (!rdna4rt_align_up_u64(base_sz, RDNA4RT_HEAP_AS_ALIGN, &extent))
        return RDNA4RT_ERR_OVERFLOW;
    if (gran > RDNA4RT_HEAP_AS_ALIGN && !rdna4rt_align_up_u64(extent, gran, &extent))
        return RDNA4RT_ERR_OVERFLOW;
    out->size = extent;
    out->align = align;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_heap_as_validate_alloc(
    uint32_t heap_storage_mode, uint32_t heap_type, uint64_t heap_size,
    uint64_t requested_size, int has_offset, uint64_t offset, uint64_t required_align,
    uint64_t *aligned_size_out) {
    if (aligned_size_out) *aligned_size_out = 0u;
    if (heap_storage_mode != RDNA4RT_HEAP_STORAGE_PRIVATE) return RDNA4RT_ERR_UNSUPPORTED;
    if (has_offset ? (heap_type != RDNA4RT_HEAP_TYPE_PLACEMENT)
                   : (heap_type != RDNA4RT_HEAP_TYPE_AUTOMATIC))
        return RDNA4RT_ERR_CONFLICT;
    if (requested_size == 0u || heap_size == 0u) return RDNA4RT_ERR_INVALID_ARG;
    uint64_t align = required_align < RDNA4RT_HEAP_AS_ALIGN ? RDNA4RT_HEAP_AS_ALIGN : required_align;
    if (!rdna4rt_is_pow2_u64(align)) return RDNA4RT_ERR_ALIGNMENT;
    uint64_t backing_sz = 0u;
    if (!rdna4rt_align_up_u64(requested_size, RDNA4RT_HEAP_AS_ALIGN, &backing_sz))
        return RDNA4RT_ERR_OVERFLOW;
    if (backing_sz > heap_size) return RDNA4RT_ERR_BOUNDS;
    if (has_offset) {
        if ((offset % align) != 0u) return RDNA4RT_ERR_ALIGNMENT;
        if (offset >= heap_size || backing_sz > heap_size - offset) return RDNA4RT_ERR_BOUNDS;
    }
    if (aligned_size_out) *aligned_size_out = backing_sz;
    return RDNA4RT_OK;
}

/* Fence-retired device-local buffer pool (GarnetASPrimData) & compacted-size query pool free list. */
typedef struct {
    uint64_t buf_handle;
    uint64_t mem_handle;
    uint64_t bda;
    uint64_t cap;
    uint64_t alloc_size;
} rdna4rt_buf_pool_entry_t;

typedef struct {
    uint32_t count;
    uint64_t total_bytes;
    uint64_t hits;
    uint64_t misses;
    rdna4rt_buf_pool_entry_t slots[RDNA4RT_PRIM_POOL_MAX_SLABS];
} rdna4rt_buf_pool_t;

static inline void rdna4rt_buf_pool_reset(rdna4rt_buf_pool_t *p) {
    if (!p) return;
    p->count = 0u;
    p->total_bytes = 0u;
}

/* Best-fit pop within [need_size, 2 * need_size] so tiny persistent BLAS snapshots never pin huge slabs. */
static inline int rdna4rt_buf_pool_pop(
    rdna4rt_buf_pool_t *p, uint64_t need_size, rdna4rt_buf_pool_entry_t *out) {
    if (!p || !out || need_size == 0u) return 0;
    uint64_t max_cap = (need_size <= UINT64_MAX / 2u) ? (need_size * 2u) : UINT64_MAX;
    uint32_t best_idx = UINT32_MAX;
    uint64_t best_cap = UINT64_MAX;
    for (uint32_t i = 0u; i < p->count; i++) {
        uint64_t c = p->slots[i].cap;
        if (c >= need_size && c <= max_cap && c < best_cap) {
            best_cap = c;
            best_idx = i;
        }
    }
    if (best_idx != UINT32_MAX) {
        *out = p->slots[best_idx];
        p->total_bytes = (p->total_bytes >= out->alloc_size) ? (p->total_bytes - out->alloc_size) : 0u;
        p->slots[best_idx] = p->slots[p->count - 1u];
        p->count--;
        p->hits++;
        return 1;
    }
    p->misses++;
    return 0;
}

static inline int rdna4rt_buf_pool_push(
    rdna4rt_buf_pool_t *p, const rdna4rt_buf_pool_entry_t *entry) {
    if (!p || !entry || !entry->buf_handle || !entry->mem_handle || !entry->bda ||
        entry->cap == 0u || entry->alloc_size == 0u)
        return 0;
    if (p->count >= RDNA4RT_PRIM_POOL_MAX_SLABS) return 0;
    if (entry->alloc_size > RDNA4RT_PRIM_POOL_MAX_BUCKET ||
        p->total_bytes > RDNA4RT_PRIM_POOL_MAX_BUCKET - entry->alloc_size)
        return 0;
    p->slots[p->count++] = *entry;
    p->total_bytes += entry->alloc_size;
    return 1;
}

typedef struct {
    uint32_t count;
    uint64_t hits;
    uint64_t misses;
    uint64_t handles[RDNA4RT_QUERY_POOL_MAX_FREE];
} rdna4rt_qp_pool_t;

static inline void rdna4rt_qp_pool_reset(rdna4rt_qp_pool_t *p) {
    if (!p) return;
    p->count = 0u;
}

static inline int rdna4rt_qp_pool_pop(rdna4rt_qp_pool_t *p, uint64_t *handle_out) {
    if (!p || !handle_out) return 0;
    if (p->count > 0u) {
        *handle_out = p->handles[--p->count];
        p->hits++;
        return 1;
    }
    p->misses++;
    return 0;
}

static inline int rdna4rt_qp_pool_push(rdna4rt_qp_pool_t *p, uint64_t handle) {
    if (!p || handle == 0u || p->count >= RDNA4RT_QUERY_POOL_MAX_FREE) return 0;
    p->handles[p->count++] = handle;
    return 1;
}

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

static inline float rdna4rt_v3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
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

    /* Refine local minimum of G(u) = (P_u^2 + P_v^2 - r^2) or UV distance using Phantom Newton steps. */
    enum { N_SEEDS = 32 };
    for (int seg = 0; seg < N_SEEDS; seg++) {
        float u = ((float)seg + 0.5f) / (float)N_SEEDS;
        for (int it = 0; it < 8; it++) {
            rdna4rt_vec4_t p, d;
            rdna4rt_curve_eval_bezier(br, u, &p, &d);
            float f, df;
            if (type == RDNA4RT_CURVE_TYPE_FLAT) {
                /* Closest approach in ray-perpendicular (U, V) plane. */
                f = p.x * d.x + p.y * d.y;
                df = d.x * d.x + d.y * d.y;
            } else {
                /* Phantom envelope condition: P.xy * dP.xy - r * dr == 0. */
                f = p.x * d.x + p.y * d.y - p.w * d.w;
                df = d.x * d.x + d.y * d.y - d.w * d.w;
            }
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
        float d2_uv = p.x * p.x + p.y * p.y;
        float r = p.w;
        if (r < 0.0f || d2_uv > r * r * (1.0f + 1e-5f)) continue;
        if (type == RDNA4RT_CURVE_TYPE_FLAT) {
            if (p.z >= smin && p.z <= smax && (!hit || p.z < best_s)) {
                hit = 1;
                best_s = p.z;
                best_u = u;
            }
        } else {
            /* Project ray intersection against tangent cylinder at P(u), radius r(u). */
            float d2_3d = d.x * d.x + d.y * d.y + d.z * d.z;
            float dz_frac = (d2_3d > 1e-12f) ? (d.z * d.z / d2_3d) : 0.0f;
            float sin_theta2 = 1.0f - dz_frac;
            if (sin_theta2 < 1e-6f) sin_theta2 = 1e-6f;
            float rad_sq = r * r - d2_uv;
            float dz = (rad_sq > 0.0f) ? sqrtf(rad_sq / sin_theta2) : 0.0f;
            float cands[2] = { p.z - dz, p.z + dz };
            for (int k = 0; k < 2; k++) {
                float s = cands[k];
                if (s >= smin && s <= smax && (!hit || s < best_s)) {
                    hit = 1;
                    best_s = s;
                    best_u = u;
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
    if (!header_bytes_out || !aux_curve_off_out || !total_bytes_out ||
        !any_prim_data_out || !any_curve_out)
        return RDNA4RT_ERR_INVALID_ARG;
    *header_bytes_out = 0u;
    *aux_curve_off_out = 0u;
    *total_bytes_out = 0u;
    *any_prim_data_out = 0;
    *any_curve_out = 0;
    if (geom_count == 0u) return RDNA4RT_OK;
    if ((geoms && !prim_rel_offsets_out) || (curve_aabb_counts && !curve_seg_offsets_out))
        return RDNA4RT_ERR_INVALID_ARG;

    int any_prim = 0, any_curve = 0;
    for (uint32_t i = 0u; i < geom_count; i++) {
        if (prim_rel_offsets_out) prim_rel_offsets_out[i] = 0u;
        if (curve_seg_offsets_out) curve_seg_offsets_out[i] = 0u;
        if (geoms && geoms[i].present && geoms[i].prim_count > 0u && geoms[i].copy_bytes > 0u)
            any_prim = 1;
        if (curve_aabb_counts && curve_aabb_counts[i] > 0u)
            any_curve = 1;
    }
    if (!any_prim && !any_curve) return RDNA4RT_OK;

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

    uint64_t final_bytes = 0u;
    if (!rdna4rt_align_up_u64(cursor, RDNA4RT_PRIM_SLICE_ALIGN, &final_bytes))
        return RDNA4RT_ERR_OVERFLOW;

    *header_bytes_out = hdr_bytes;
    *total_bytes_out = final_bytes;
    *any_prim_data_out = any_prim;
    *any_curve_out = any_curve;
    return RDNA4RT_OK;
}

#endif /* RDNA4_RT_ACCEL_H */
