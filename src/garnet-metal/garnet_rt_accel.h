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
#define RDNA4RT_MTL_INSTANCE_MOTION_SIZE    UINT64_C(44)
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
};

enum {
    RDNA4RT_VK_COPY_CLONE   = 0u,
    RDNA4RT_VK_COPY_COMPACT = 1u,
};

typedef enum {
    RDNA4RT_GEOM_TRIANGLES = 0u,
    RDNA4RT_GEOM_AABBS     = 1u,
    RDNA4RT_GEOM_INSTANCES = 2u,
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
    case RDNA4RT_INST_DESC_INDIRECT_MOTION:
        return RDNA4RT_ERR_UNSUPPORTED;
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

/* Convert Default (64B) or UserID (68B) Metal instance descriptor buffer -> Vulkan 64B instances. */
static inline rdna4rt_err_t rdna4rt_convert_instances(
    rdna4rt_inst_desc_type_t type, const uint8_t *src_bytes, size_t src_len,
    uint64_t src_offset, uint64_t stride, uint32_t count,
    const uint64_t *blas_addrs, uint32_t blas_count, rdna4rt_vk_instance_t *dst) {
    if (count == 0u) return RDNA4RT_OK;
    if (!src_bytes || !dst) return RDNA4RT_ERR_INVALID_ARG;
    uint64_t min_size = (type == RDNA4RT_INST_DESC_DEFAULT) ? RDNA4RT_MTL_INSTANCE_DEFAULT_SIZE :
                        (type == RDNA4RT_INST_DESC_USER_ID) ? RDNA4RT_MTL_INSTANCE_USERID_SIZE : 0u;
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
        rdna4rt_mtl_instance_userid_t raw;
        memset(&raw, 0, sizeof(raw));
        memcpy(&raw, p, (size_t)min_size);
        uint32_t custom_id = (type == RDNA4RT_INST_DESC_USER_ID) ? raw.user_id : 0u;
        uint64_t blas_ref = 0;
        rdna4rt_err_t re = rdna4rt_resolve_blas_ref(
            raw.accel_struct_index, raw.mask, blas_addrs, blas_count, &blas_ref);
        if (re != RDNA4RT_OK) return re;
        rdna4rt_vk_transform_t xf;
        rdna4rt_err_t te = rdna4rt_transpose_4x3_to_3x4(&raw.transform, &xf);
        if (te != RDNA4RT_OK) return te;
        rdna4rt_err_t pe = rdna4rt_pack_vk_instance(
            &xf, custom_id, raw.mask, raw.intersection_function_offset, raw.options, blas_ref, &dst[i]);
        if (pe != RDNA4RT_OK) return pe;
    }
    return RDNA4RT_OK;
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
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_can_refit(
    const rdna4rt_accel_meta_t *src, const rdna4rt_accel_meta_t *dst,
    rdna4rt_kind_t kind, uint64_t required_as_size, uint32_t geom_count,
    uint64_t topology_hash, uint64_t refit_options) {
    if (!src || !dst || required_as_size == 0u) return RDNA4RT_ERR_INVALID_ARG;
    if (refit_options & ~(uint64_t)RDNA4RT_REFIT_OPT_KNOWN_MASK) return RDNA4RT_ERR_INVALID_ARG;
    if (src->state != RDNA4RT_STATE_BUILT || !src->allow_refit) return RDNA4RT_ERR_STATE;
    if (src->kind != kind || src->geometry_count != geom_count ||
        src->topology_hash != topology_hash)
        return RDNA4RT_ERR_STATE;
    if (dst->capacity_bytes < required_as_size) return RDNA4RT_ERR_BOUNDS;
    return RDNA4RT_OK;
}

static inline rdna4rt_err_t rdna4rt_state_apply_refit(
    const rdna4rt_accel_meta_t *src, rdna4rt_accel_meta_t *dst,
    rdna4rt_kind_t kind, uint64_t required_as_size, uint32_t geom_count,
    uint64_t topology_hash, uint64_t refit_options) {
    rdna4rt_err_t e = rdna4rt_state_can_refit(
        src, dst, kind, required_as_size, geom_count, topology_hash, refit_options);
    if (e != RDNA4RT_OK) return e;
    uint64_t cap = dst->capacity_bytes;
    *dst = *src;
    dst->capacity_bytes = cap;
    dst->state = RDNA4RT_STATE_BUILT;
    dst->built_size_bytes = required_as_size;
    dst->allow_refit = 1u;
    dst->allow_compaction = 1u;
    dst->compacted_queried = 0u;
    return RDNA4RT_OK;
}

#endif /* RDNA4_RT_ACCEL_H */
