// Shared binary ABI between GarnetMetal (C/ObjC), GPU compute expander SPIR-V,
// and garnetxlate / metal2vulkan (Rust) for GPU-visible Indirect Command Buffers.
#ifndef RDNA4_ICB_ABI_H
#define RDNA4_ICB_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "garnet_buffer_ranges.h"

#define RDNA4_ICB_MAGIC                   0x49434231u /* 'ICB1' */
#define RDNA4_ICB_HEADER_BYTES            64u
#define RDNA4_ICB_SLOT_BYTES              640u        /* 160 dwords, 64B aligned */
#define RDNA4_ICB_SLOT_DWORDS             160u
#define RDNA4_ICB_MAX_GPU_SLOTS           65536u
#define RDNA4_ICB_MAX_CANDIDATE_PIPELINES 32u
#define RDNA4_ICB_MAX_GPU_EXPANDED_DRAWS  131072u
#define RDNA4_ICB_MAX_GPU_EXPANDED_DISPATCHES 131072u
#define RDNA4_ICB_MAX_TG_MEM_INDICES      32u
#define RDNA4_ICB_MAX_COMPUTE_BUCKETS     256u
#define RDNA4_ICB_VS_PARAMS_STRIDE        512u        /* 256B aligned dynamic SSBO stride */
#define RDNA4_ICB_TESS_PARAMS_STRIDE      768u        /* 256B aligned (>= 560B rdna4_tess_draw_params_t) */

#define RDNA4_ICB_PSO_TOKEN_MASK          UINT64_C(0xFFFFFFFF00000000)
#define RDNA4_ICB_PSO_TOKEN_TAG           UINT64_C(0x5244505300000000) /* 'RDPS' << 32 */
#define RDNA4_ICB_COMPUTE_PSO_BIT         UINT64_C(0x0000000080000000)

#define RDNA4_ICB_KIND_EMPTY                      0u
#define RDNA4_ICB_KIND_DRAW                       1u
#define RDNA4_ICB_KIND_DRAW_INDEXED               2u
#define RDNA4_ICB_KIND_DRAW_PATCHES               3u
#define RDNA4_ICB_KIND_DRAW_INDEXED_PATCHES       4u
#define RDNA4_ICB_KIND_CONCURRENT_DISPATCH        5u
#define RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS 6u

#define RDNA4_ICB_FLAG_INHERIT_PIPELINE   (1u << 0)
#define RDNA4_ICB_FLAG_INHERIT_BUFFERS    (1u << 1)

#define RDNA4_ICB_SLOT_FLAG_PIPELINE_SET    (1u << 0)
#define RDNA4_ICB_SLOT_FLAG_INVALID         (1u << 1)
#define RDNA4_ICB_SLOT_FLAG_BARRIER         (1u << 2)
#define RDNA4_ICB_SLOT_FLAG_STAGE_IN_REGION (1u << 3)

#define RDNA4_ICB_PRIM_POINT              0u
#define RDNA4_ICB_PRIM_LINE               1u
#define RDNA4_ICB_PRIM_LINE_STRIP         2u
#define RDNA4_ICB_PRIM_TRIANGLE           3u
#define RDNA4_ICB_PRIM_TRIANGLE_STRIP     4u

#define RDNA4_ICB_INDEX_UINT16            0u
#define RDNA4_ICB_INDEX_UINT32            1u

#define RDNA4_ICB_CMD_BIT_DRAW                       (1u << 0)
#define RDNA4_ICB_CMD_BIT_DRAW_INDEXED               (1u << 1)
#define RDNA4_ICB_CMD_BIT_DRAW_PATCHES               (1u << 2)
#define RDNA4_ICB_CMD_BIT_DRAW_INDEXED_PATCHES       (1u << 3)
#define RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH        (1u << 5)
#define RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH_THREADS (1u << 6)
#define RDNA4_ICB_RENDER_CMD_MASK                    0x0Fu
#define RDNA4_ICB_COMPUTE_CMD_MASK                   0x60u
#define RDNA4_ICB_CMD_MASK                           0x6Fu

#define RDNA4_ICB_COMPUTE_BUCKET_ALLOW_FALLBACK      (1u << 0)

#define RDNA4_ICB_TABLE_BINDING          676u
#define RDNA4_ICB_VS_PARAMS_BINDING      677u
#define RDNA4_ICB_TABLE_V2_STRIDE        768u        /* sizeof(rdna4_buffer_table), 256B aligned */

typedef struct {
    uint32_t magic;
    uint32_t max_command_count;
    uint32_t command_types;
    uint32_t flags;
    uint32_t max_vertex_binds;
    uint32_t max_fragment_binds;
    uint32_t slot_stride;
    uint32_t reserved[9];
} rdna4_icb_header_t;

typedef struct {
    uint32_t kind;                 /* +0: 0=Empty, 1=Draw, 2=DrawIndexed, 3=Patches, 4=IndexedPatches */
    uint32_t primitive_type;       /* +4: MTLPrimitiveType 0..4 */
    uint32_t index_type;           /* +8: 0=UInt16, 1=UInt32 */
    uint32_t flags;                /* +12: bit0=pipeline_set, bit1=invalid_mutation */
    uint64_t pipeline_token;       /* +16: RDNA4_ICB_PSO_TOKEN_TAG | id, or 0 */
    uint32_t vertex_bind_mask;     /* +24: bits 0..30 */
    uint32_t fragment_bind_mask;   /* +28: bits 0..30 */
    uint32_t count_or_patch_count; /* +32: vertex/index/patch count */
    uint32_t instance_count;       /* +36 */
    uint32_t first_or_patch_start; /* +40: vertexStart / patchStart */
    uint32_t base_instance;        /* +44 */
    int32_t  base_vertex;          /* +48 */
    uint32_t patch_control_points; /* +52 */
    uint32_t tess_instance_stride; /* +56 */
    float    tess_factor_scale;    /* +60 */
    uint64_t index_or_cp_bda;      /* +64: indexBuffer (indexed draw) or cpIndexBuffer (indexed patch) */
    uint64_t patch_index_bda;      /* +72 */
    uint64_t tess_factor_bda;      /* +80 */
    uint64_t reserved_bda;         /* +88 */
    uint64_t vertex_bda[32];       /* +96: slots 0..30 (+96..343), [31]=0 */
    uint64_t fragment_bda[32];     /* +352: slots 0..30 (+352..599), [31]=0 */
    uint32_t pad[8];               /* +608..639 */
} rdna4_icb_slot_t;

typedef struct {
    uint32_t kind;                 /* +0: 1=Draw, 2=DrawIndexed */
    uint32_t index_type;           /* +4: 0=UInt16, 1=UInt32 */
    int32_t  base_vertex;          /* +8 */
    uint32_t primitive_type;       /* +12: MTLPrimitiveType 0..4 */
    uint64_t index_bda;            /* +16 */
    uint64_t pad0;                 /* +24 */
    uint64_t vb_bda[32];           /* +32..287 */
} rdna4_icb_vs_params_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rdna4_icb_header_t) == RDNA4_ICB_HEADER_BYTES, "rdna4_icb_header_t must be 64 bytes");
_Static_assert(offsetof(rdna4_icb_header_t, max_command_count) == 4, "header max_command_count offset");
_Static_assert(offsetof(rdna4_icb_header_t, command_types) == 8, "header command_types offset");
_Static_assert(offsetof(rdna4_icb_header_t, flags) == 12, "header flags offset");
_Static_assert(offsetof(rdna4_icb_header_t, max_vertex_binds) == 16, "header max_vertex_binds offset");
_Static_assert(offsetof(rdna4_icb_header_t, max_fragment_binds) == 20, "header max_fragment_binds offset");
_Static_assert(offsetof(rdna4_icb_header_t, slot_stride) == 24, "header slot_stride offset");

_Static_assert(sizeof(rdna4_icb_slot_t) == RDNA4_ICB_SLOT_BYTES, "rdna4_icb_slot_t must be 640 bytes");
_Static_assert(offsetof(rdna4_icb_slot_t, kind) == 0, "slot kind offset");
_Static_assert(offsetof(rdna4_icb_slot_t, primitive_type) == 4, "slot primitive_type offset");
_Static_assert(offsetof(rdna4_icb_slot_t, index_type) == 8, "slot index_type offset");
_Static_assert(offsetof(rdna4_icb_slot_t, flags) == 12, "slot flags offset");
_Static_assert(offsetof(rdna4_icb_slot_t, pipeline_token) == 16, "slot pipeline_token offset");
_Static_assert(offsetof(rdna4_icb_slot_t, vertex_bind_mask) == 24, "slot vertex_bind_mask offset");
_Static_assert(offsetof(rdna4_icb_slot_t, fragment_bind_mask) == 28, "slot fragment_bind_mask offset");
_Static_assert(offsetof(rdna4_icb_slot_t, count_or_patch_count) == 32, "slot count_or_patch_count offset");
_Static_assert(offsetof(rdna4_icb_slot_t, instance_count) == 36, "slot instance_count offset");
_Static_assert(offsetof(rdna4_icb_slot_t, first_or_patch_start) == 40, "slot first_or_patch_start offset");
_Static_assert(offsetof(rdna4_icb_slot_t, base_instance) == 44, "slot base_instance offset");
_Static_assert(offsetof(rdna4_icb_slot_t, base_vertex) == 48, "slot base_vertex offset");
_Static_assert(offsetof(rdna4_icb_slot_t, patch_control_points) == 52, "slot patch_control_points offset");
_Static_assert(offsetof(rdna4_icb_slot_t, tess_instance_stride) == 56, "slot tess_instance_stride offset");
_Static_assert(offsetof(rdna4_icb_slot_t, tess_factor_scale) == 60, "slot tess_factor_scale offset");
_Static_assert(offsetof(rdna4_icb_slot_t, index_or_cp_bda) == 64, "slot index_or_cp_bda offset");
_Static_assert(offsetof(rdna4_icb_slot_t, patch_index_bda) == 72, "slot patch_index_bda offset");
_Static_assert(offsetof(rdna4_icb_slot_t, tess_factor_bda) == 80, "slot tess_factor_bda offset");
_Static_assert(offsetof(rdna4_icb_slot_t, vertex_bda) == 96, "slot vertex_bda offset");
_Static_assert(offsetof(rdna4_icb_slot_t, fragment_bda) == 352, "slot fragment_bda offset");

_Static_assert(sizeof(rdna4_icb_vs_params_t) == 288, "rdna4_icb_vs_params_t must be 288 bytes");
_Static_assert(offsetof(rdna4_icb_vs_params_t, kind) == 0, "vs_params kind offset");
_Static_assert(offsetof(rdna4_icb_vs_params_t, index_type) == 4, "vs_params index_type offset");
_Static_assert(offsetof(rdna4_icb_vs_params_t, base_vertex) == 8, "vs_params base_vertex offset");
_Static_assert(offsetof(rdna4_icb_vs_params_t, primitive_type) == 12, "vs_params primitive_type offset");
_Static_assert(offsetof(rdna4_icb_vs_params_t, index_bda) == 16, "vs_params index_bda offset");
_Static_assert(offsetof(rdna4_icb_vs_params_t, vb_bda) == 32, "vs_params vb_bda offset");
#endif

static inline uint64_t rdna4_icb_backing_bytes(uint32_t max_commands) {
    uint64_t raw = (uint64_t)RDNA4_ICB_HEADER_BYTES +
                   (uint64_t)max_commands * (uint64_t)RDNA4_ICB_SLOT_BYTES;
    return (raw + 255u) & ~UINT64_C(255);
}

/* Compute overlay of the exact same 640-byte rdna4_icb_slot_t written by
 * metal2vulkan patch 0072 (air.*_compute_command). */
typedef struct {
    uint32_t kind;                       /* +0: 0=Empty, 5=ConcurrentDispatch, 6=ConcurrentDispatchThreads */
    uint32_t reserved0;                  /* +4: 0 */
    uint32_t reserved1;                  /* +8: 0 */
    uint32_t flags;                      /* +12: bit0=pipeline, bit1=invalid, bit2=barrier, bit3=stage_in */
    uint64_t pipeline_token;             /* +16: RDNA4_ICB_PSO_TOKEN_TAG | RDNA4_ICB_COMPUTE_PSO_BIT | id */
    uint32_t kernel_bind_mask;           /* +24: bits 0..30 */
    uint32_t threadgroup_mem_mask;       /* +28: bits 0..31 */
    uint32_t grid_size[3];               /* +32..43: threadgroupsPerGrid or threadsPerGrid */
    uint32_t threadgroup_size[3];        /* +44..55: threadsPerThreadgroup */
    uint32_t stage_in_origin[3];         /* +56..67: stageInRegion.origin */
    uint32_t stage_in_size[3];           /* +68..79: stageInRegion.size */
    uint64_t reserved_bda[2];            /* +80..95 */
    uint64_t kernel_bda[32];             /* +96..351: slots 0..30 (+96..343), [31]=0 */
    uint32_t threadgroup_mem_length[32]; /* +352..479: indices 0..31 (4B each) */
    uint32_t pad[40];                    /* +480..639 */
} rdna4_icb_compute_slot_t;

typedef struct {
    uint64_t token;                      /* +0  (dwords 0..1) */
    uint32_t local_x;                    /* +8  (dword 2) */
    uint32_t local_y;                    /* +12 (dword 3) */
    uint32_t local_z;                    /* +16 (dword 4) */
    uint32_t flags;                      /* +20 (dword 5): bit0 = allow_threads_fallback */
    uint32_t pad0;                       /* +24 (dword 6) */
    uint32_t pad1;                       /* +28 (dword 7) */
} rdna4_icb_compute_bucket_t;

typedef struct {
    uint32_t direct_location;            /* +0  (dword 0) */
    uint32_t direct_length;              /* +4  (dword 1) */
    uint32_t use_indirect_range;         /* +8  (dword 2) */
    uint32_t max_output_slots;           /* +12 (dword 3) */
    uint32_t inherit_pipeline;           /* +16 (dword 4) */
    uint32_t inherit_buffers;            /* +20 (dword 5) */
    uint32_t num_buckets;                /* +24 (dword 6) */
    uint32_t _pad0;                      /* +28 (dword 7) */
    uint64_t parent_pipeline_token;      /* +32 (dwords 8..9) */
    uint64_t _pad1;                      /* +40 (dwords 10..11) */
    uint32_t _pad2[4];                   /* +48 (dwords 12..15) */
    rdna4_icb_compute_bucket_t buckets[RDNA4_ICB_MAX_COMPUTE_BUCKETS]; /* +64 (dwords 16..2063) */
    rdna4_buffer_table parent_k_table;   /* +8256 (dwords 2064..2255) */
} rdna4_icb_compute_expand_cfg_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rdna4_icb_compute_slot_t) == RDNA4_ICB_SLOT_BYTES, "rdna4_icb_compute_slot_t must be 640 bytes");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kind) == 0, "compute slot kind offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, flags) == 12, "compute slot flags offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, pipeline_token) == 16, "compute slot pipeline_token offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kernel_bind_mask) == 24, "compute slot kernel_bind_mask offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_mem_mask) == 28, "compute slot threadgroup_mem_mask offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, grid_size) == 32, "compute slot grid_size offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_size) == 44, "compute slot threadgroup_size offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, stage_in_origin) == 56, "compute slot stage_in_origin offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, stage_in_size) == 68, "compute slot stage_in_size offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, kernel_bda) == 96, "compute slot kernel_bda offset");
_Static_assert(offsetof(rdna4_icb_compute_slot_t, threadgroup_mem_length) == 352, "compute slot threadgroup_mem_length offset");
_Static_assert(sizeof(rdna4_icb_compute_bucket_t) == 32, "rdna4_icb_compute_bucket_t must be 32 bytes");
_Static_assert(sizeof(rdna4_icb_compute_expand_cfg_t) == 9024, "rdna4_icb_compute_expand_cfg_t must be 9024 bytes");
_Static_assert(offsetof(rdna4_icb_compute_expand_cfg_t, parent_pipeline_token) == 32, "compute cfg parent_pipeline_token offset");
_Static_assert(offsetof(rdna4_icb_compute_expand_cfg_t, buckets) == 64, "compute cfg buckets offset");
_Static_assert(offsetof(rdna4_icb_compute_expand_cfg_t, parent_k_table) == 8256, "compute cfg parent_k_table offset");
#endif

static inline int rdna4_icb_is_render_types(uint32_t command_types) {
    return (command_types & RDNA4_ICB_RENDER_CMD_MASK) != 0u &&
           (command_types & ~RDNA4_ICB_RENDER_CMD_MASK) == 0u;
}

static inline int rdna4_icb_is_compute_types(uint32_t command_types) {
    return (command_types & RDNA4_ICB_COMPUTE_CMD_MASK) != 0u &&
           (command_types & ~RDNA4_ICB_COMPUTE_CMD_MASK) == 0u;
}

/* Metal forbids mixing render and compute command types on the same ICB. */
static inline int rdna4_icb_command_types_valid(uint32_t command_types) {
    return rdna4_icb_is_render_types(command_types) || rdna4_icb_is_compute_types(command_types);
}

static inline int rdna4_icb_compute_tg_mem_ok(uint32_t index, uint64_t length, uint32_t max_shared_mem) {
    if (index >= RDNA4_ICB_MAX_TG_MEM_INDICES) return 0;
    if ((length & 15u) != 0u || length > UINT32_MAX) return 0;
    if (max_shared_mem && length > (uint64_t)max_shared_mem) return 0;
    return 1;
}

static inline int rdna4_icb_compute_stage_in_ok(const uint64_t origin[3], const uint64_t size[3]) {
    if (!origin || !size) return 0;
    for (int d = 0; d < 3; d++) {
        if (origin[d] > UINT32_MAX || size[d] > UINT32_MAX) return 0;
        if (origin[d] > UINT32_MAX - size[d]) return 0;
    }
    return 1;
}

static inline int rdna4_icb_compute_dispatch_groups(uint32_t kind,
                                                    const uint32_t grid[3],
                                                    const uint32_t tg[3],
                                                    uint32_t out_groups[3],
                                                    uint32_t out_active_threads[3]) {
    if (!grid || !tg || !out_groups) return 0;
    if (kind != RDNA4_ICB_KIND_CONCURRENT_DISPATCH &&
        kind != RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS)
        return 0;
    if (!tg[0] || !tg[1] || !tg[2]) return 0;
    if (!grid[0] || !grid[1] || !grid[2]) {
        out_groups[0] = out_groups[1] = out_groups[2] = 0u;
        if (out_active_threads) out_active_threads[0] = out_active_threads[1] = out_active_threads[2] = 0u;
        return 1;
    }
    for (int d = 0; d < 3; d++) {
        if (kind == RDNA4_ICB_KIND_CONCURRENT_DISPATCH) {
            out_groups[d] = grid[d];
            if (out_active_threads) {
                uint64_t prod = (uint64_t)grid[d] * (uint64_t)tg[d];
                out_active_threads[d] = prod > UINT32_MAX ? UINT32_MAX : (uint32_t)prod;
            }
        } else {
            out_groups[d] = (grid[d] - 1u) / tg[d] + 1u;
            if (out_active_threads) out_active_threads[d] = grid[d];
        }
    }
    return 1;
}

/* Pure-C compute slot mutators mirroring metal2vulkan patch 0072 semantics. */
static inline void rdna4_icb_compute_slot_reset(rdna4_icb_compute_slot_t *slot) {
    if (slot) memset(slot, 0, sizeof *slot);
}

static inline void rdna4_icb_compute_slot_mark_invalid(rdna4_icb_compute_slot_t *slot) {
    if (!slot) return;
    slot->flags |= RDNA4_ICB_SLOT_FLAG_INVALID;
    slot->kind = RDNA4_ICB_KIND_EMPTY;
}

static inline int rdna4_icb_compute_slot_set_pipeline(rdna4_icb_compute_slot_t *slot,
                                                      uint32_t hdr_flags,
                                                      uint64_t token) {
    if (!slot) return 0;
    if ((hdr_flags & RDNA4_ICB_FLAG_INHERIT_PIPELINE) || !token) {
        rdna4_icb_compute_slot_mark_invalid(slot);
        return 0;
    }
    slot->pipeline_token = token;
    slot->flags |= RDNA4_ICB_SLOT_FLAG_PIPELINE_SET;
    return 1;
}

static inline int rdna4_icb_compute_slot_set_kernel_buffer(rdna4_icb_compute_slot_t *slot,
                                                           uint32_t hdr_flags,
                                                           uint32_t max_kernel_binds,
                                                           uint32_t index,
                                                           uint64_t bda) {
    if (!slot) return 0;
    if ((hdr_flags & RDNA4_ICB_FLAG_INHERIT_BUFFERS) ||
        index >= max_kernel_binds || index >= 31u) {
        rdna4_icb_compute_slot_mark_invalid(slot);
        return 0;
    }
    slot->kernel_bda[index] = bda;
    if (bda) slot->kernel_bind_mask |= (1u << index);
    else slot->kernel_bind_mask &= ~(1u << index);
    return 1;
}

static inline int rdna4_icb_compute_slot_set_tg_mem(rdna4_icb_compute_slot_t *slot,
                                                    uint32_t index,
                                                    uint32_t length) {
    if (!slot) return 0;
    if (index >= RDNA4_ICB_MAX_TG_MEM_INDICES) {
        rdna4_icb_compute_slot_mark_invalid(slot);
        return 0;
    }
    slot->threadgroup_mem_length[index] = length;
    slot->threadgroup_mem_mask |= (1u << index);
    return 1;
}

static inline int rdna4_icb_compute_slot_set_stage_in(rdna4_icb_compute_slot_t *slot,
                                                      const uint32_t origin[3],
                                                      const uint32_t size[3]) {
    if (!slot || !origin || !size) return 0;
    for (int d = 0; d < 3; d++) {
        slot->stage_in_origin[d] = origin[d];
        slot->stage_in_size[d] = size[d];
    }
    slot->flags |= RDNA4_ICB_SLOT_FLAG_STAGE_IN_REGION;
    return 1;
}

static inline void rdna4_icb_compute_slot_set_barrier(rdna4_icb_compute_slot_t *slot, int enable) {
    if (!slot) return;
    if (enable) slot->flags |= RDNA4_ICB_SLOT_FLAG_BARRIER;
    else slot->flags &= ~RDNA4_ICB_SLOT_FLAG_BARRIER;
}

static inline int rdna4_icb_compute_slot_set_dispatch(rdna4_icb_compute_slot_t *slot,
                                                      uint32_t hdr_cmd_types,
                                                      uint32_t hdr_flags,
                                                      uint32_t kind,
                                                      const uint32_t grid[3],
                                                      const uint32_t tg[3]) {
    if (!slot || !grid || !tg) return 0;
    uint32_t cmd_bit = 0u;
    if (kind == RDNA4_ICB_KIND_CONCURRENT_DISPATCH) cmd_bit = RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH;
    else if (kind == RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS) cmd_bit = RDNA4_ICB_CMD_BIT_CONCURRENT_DISPATCH_THREADS;
    else {
        rdna4_icb_compute_slot_mark_invalid(slot);
        return 0;
    }
    int pipe_ok = ((hdr_flags & RDNA4_ICB_FLAG_INHERIT_PIPELINE) != 0u) ||
                  ((slot->flags & RDNA4_ICB_SLOT_FLAG_PIPELINE_SET) != 0u);
    int not_inv = (slot->flags & RDNA4_ICB_SLOT_FLAG_INVALID) == 0u;
    int tg_ok = (tg[0] != 0u && tg[1] != 0u && tg[2] != 0u);
    if (!((hdr_cmd_types & cmd_bit) != 0u && pipe_ok && not_inv && tg_ok)) {
        rdna4_icb_compute_slot_mark_invalid(slot);
        return 0;
    }
    slot->kind = kind;
    slot->reserved0 = 0u;
    slot->reserved1 = 0u;
    for (int d = 0; d < 3; d++) {
        slot->grid_size[d] = grid[d];
        slot->threadgroup_size[d] = tg[d];
    }
    return 1;
}

/* ---- Pure-C SPIR-V 1.5 generator for GPU Compute ICB expansion ---- */
typedef struct {
    uint32_t *w;
    size_t len, cap;
    int oom;
} rdna4_icbc_buf_t;

static inline void rdna4_icbc_push(rdna4_icbc_buf_t *b, uint32_t x) {
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t nc = b->cap ? b->cap * 2u : 512u;
        uint32_t *nw = (uint32_t *)realloc(b->w, nc * sizeof(uint32_t));
        if (!nw) { b->oom = 1; return; }
        b->w = nw; b->cap = nc;
    }
    b->w[b->len++] = x;
}

static inline void rdna4_icbc_op(rdna4_icbc_buf_t *b, uint16_t op, uint16_t wc) {
    rdna4_icbc_push(b, ((uint32_t)wc << 16) | op);
}

static inline void rdna4_icbc_str(rdna4_icbc_buf_t *b, const char *s) {
    size_t n = strlen(s) + 1u;
    size_t nw = (n + 3u) / 4u;
    for (size_t i = 0; i < nw; i++) {
        uint32_t w = 0u;
        for (size_t k = 0; k < 4u && (i * 4u + k) < n; k++)
            w |= (uint32_t)(uint8_t)s[i * 4u + k] << (k * 8u);
        rdna4_icbc_push(b, w);
    }
}

static inline void rdna4_icbc_dec(rdna4_icbc_buf_t *b, uint32_t target, uint32_t dec, int has_arg, uint32_t arg) {
    rdna4_icbc_op(b, 71, has_arg ? 4 : 3);
    rdna4_icbc_push(b, target); rdna4_icbc_push(b, dec);
    if (has_arg) rdna4_icbc_push(b, arg);
}

static inline void rdna4_icbc_mdec(rdna4_icbc_buf_t *b, uint32_t st, uint32_t mi, uint32_t dec, uint32_t arg) {
    rdna4_icbc_op(b, 72, 5);
    rdna4_icbc_push(b, st); rdna4_icbc_push(b, mi); rdna4_icbc_push(b, dec); rdna4_icbc_push(b, arg);
}

typedef struct {
    rdna4_icbc_buf_t dec, types, vars, code;
    uint32_t next_id;
    uint32_t t_void, t_bool, t_u32, t_v3u32, t_fn;
    uint32_t t_p_in_v3u32, t_p_sb_u32, t_rt_u32, t_st_buf, t_p_sb_st;
    uint32_t v_gid, v_buf[5];
    uint32_t c_u32[68];
} rdna4_icbc_ctx_t;

static inline uint32_t rdna4_icbc_id(rdna4_icbc_ctx_t *k) { return k->next_id++; }

static inline uint32_t rdna4_icbc_const(rdna4_icbc_ctx_t *k, uint32_t val) {
    if (val < 68u && k->c_u32[val]) return k->c_u32[val];
    uint32_t id = rdna4_icbc_id(k);
    rdna4_icbc_op(&k->types, 43, 4);
    rdna4_icbc_push(&k->types, k->t_u32); rdna4_icbc_push(&k->types, id); rdna4_icbc_push(&k->types, val);
    if (val < 68u) k->c_u32[val] = id;
    return id;
}

static inline uint32_t rdna4_icbc_op1(rdna4_icbc_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a) {
    uint32_t id = rdna4_icbc_id(k);
    rdna4_icbc_op(&k->code, op, 4);
    rdna4_icbc_push(&k->code, rt); rdna4_icbc_push(&k->code, id); rdna4_icbc_push(&k->code, a);
    return id;
}

static inline uint32_t rdna4_icbc_op2(rdna4_icbc_ctx_t *k, uint16_t op, uint32_t rt, uint32_t a, uint32_t b) {
    uint32_t id = rdna4_icbc_id(k);
    rdna4_icbc_op(&k->code, op, 5);
    rdna4_icbc_push(&k->code, rt); rdna4_icbc_push(&k->code, id); rdna4_icbc_push(&k->code, a); rdna4_icbc_push(&k->code, b);
    return id;
}

static inline uint32_t rdna4_icbc_sel(rdna4_icbc_ctx_t *k, uint32_t rt, uint32_t c, uint32_t tv, uint32_t fv) {
    uint32_t id = rdna4_icbc_id(k);
    rdna4_icbc_op(&k->code, 169, 6);
    rdna4_icbc_push(&k->code, rt); rdna4_icbc_push(&k->code, id);
    rdna4_icbc_push(&k->code, c); rdna4_icbc_push(&k->code, tv); rdna4_icbc_push(&k->code, fv);
    return id;
}

static inline uint32_t rdna4_icbc_ld(rdna4_icbc_ctx_t *k, uint32_t var_id, uint32_t idx_id) {
    uint32_t ac = rdna4_icbc_id(k), val = rdna4_icbc_id(k);
    uint32_t c0 = rdna4_icbc_const(k, 0u);
    rdna4_icbc_op(&k->code, 65, 6);
    rdna4_icbc_push(&k->code, k->t_p_sb_u32); rdna4_icbc_push(&k->code, ac);
    rdna4_icbc_push(&k->code, var_id); rdna4_icbc_push(&k->code, c0); rdna4_icbc_push(&k->code, idx_id);
    rdna4_icbc_op(&k->code, 61, 4);
    rdna4_icbc_push(&k->code, k->t_u32); rdna4_icbc_push(&k->code, val); rdna4_icbc_push(&k->code, ac);
    return val;
}

static inline uint32_t rdna4_icbc_ld_c(rdna4_icbc_ctx_t *k, uint32_t var_id, uint32_t off) {
    return rdna4_icbc_ld(k, var_id, rdna4_icbc_const(k, off));
}

static inline void rdna4_icbc_st(rdna4_icbc_ctx_t *k, uint32_t var_id, uint32_t idx_id, uint32_t val_id) {
    uint32_t ac = rdna4_icbc_id(k);
    uint32_t c0 = rdna4_icbc_const(k, 0u);
    rdna4_icbc_op(&k->code, 65, 6);
    rdna4_icbc_push(&k->code, k->t_p_sb_u32); rdna4_icbc_push(&k->code, ac);
    rdna4_icbc_push(&k->code, var_id); rdna4_icbc_push(&k->code, c0); rdna4_icbc_push(&k->code, idx_id);
    rdna4_icbc_op(&k->code, 62, 3);
    rdna4_icbc_push(&k->code, ac); rdna4_icbc_push(&k->code, val_id);
}

static inline void rdna4_icbc_lbl(rdna4_icbc_ctx_t *k, uint32_t lbl) {
    rdna4_icbc_op(&k->code, 248, 2); rdna4_icbc_push(&k->code, lbl);
}

static inline void rdna4_icbc_br(rdna4_icbc_ctx_t *k, uint32_t lbl) {
    rdna4_icbc_op(&k->code, 249, 2); rdna4_icbc_push(&k->code, lbl);
}

static inline void rdna4_icbc_smerge(rdna4_icbc_ctx_t *k, uint32_t merge_lbl) {
    rdna4_icbc_op(&k->code, 247, 3); rdna4_icbc_push(&k->code, merge_lbl); rdna4_icbc_push(&k->code, 0u);
}

static inline void rdna4_icbc_lmerge(rdna4_icbc_ctx_t *k, uint32_t merge_lbl, uint32_t cont_lbl) {
    rdna4_icbc_op(&k->code, 246, 4); rdna4_icbc_push(&k->code, merge_lbl); rdna4_icbc_push(&k->code, cont_lbl); rdna4_icbc_push(&k->code, 0u);
}

static inline void rdna4_icbc_bcond(rdna4_icbc_ctx_t *k, uint32_t cond, uint32_t t_lbl, uint32_t f_lbl) {
    rdna4_icbc_op(&k->code, 250, 4); rdna4_icbc_push(&k->code, cond); rdna4_icbc_push(&k->code, t_lbl); rdna4_icbc_push(&k->code, f_lbl);
}

static inline uint32_t rdna4_icbc_phi2(rdna4_icbc_ctx_t *k, uint32_t rt, uint32_t v0, uint32_t l0, uint32_t v1, uint32_t l1) {
    uint32_t id = rdna4_icbc_id(k);
    rdna4_icbc_op(&k->code, 245, 7);
    rdna4_icbc_push(&k->code, rt); rdna4_icbc_push(&k->code, id);
    rdna4_icbc_push(&k->code, v0); rdna4_icbc_push(&k->code, l0);
    rdna4_icbc_push(&k->code, v1); rdna4_icbc_push(&k->code, l1);
    return id;
}

/* Bindings (set 0):
 *  0: icbBacking (read-only)
 *  1: rdna4_icb_compute_expand_cfg_t (read-only)
 *  2: indirectRange { uint32_t location, length } (read-only)
 *  3: outDispatches VkDispatchIndirectCommand[max_output_slots * num_buckets] (write-only, 3 dwords/cmd)
 *  4: outKTable rdna4_buffer_table[max_output_slots] (write-only, 192 dwords/slot; slot[31] carries active_threads[0..2] + slot_flags) */
static inline int rdna4_icb_emit_compute_expand_spv(uint32_t **out_words, size_t *out_nwords) {
    if (!out_words || !out_nwords) return 0;
    rdna4_icbc_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.next_id = 1u;

    uint32_t fn_main = rdna4_icbc_id(&ctx);
    ctx.t_void = rdna4_icbc_id(&ctx);
    ctx.t_bool = rdna4_icbc_id(&ctx);
    ctx.t_u32 = rdna4_icbc_id(&ctx);
    ctx.t_v3u32 = rdna4_icbc_id(&ctx);
    ctx.t_fn = rdna4_icbc_id(&ctx);
    ctx.t_p_in_v3u32 = rdna4_icbc_id(&ctx);
    ctx.t_p_sb_u32 = rdna4_icbc_id(&ctx);
    ctx.t_rt_u32 = rdna4_icbc_id(&ctx);
    ctx.t_st_buf = rdna4_icbc_id(&ctx);
    ctx.t_p_sb_st = rdna4_icbc_id(&ctx);
    ctx.v_gid = rdna4_icbc_id(&ctx);
    for (int i = 0; i < 5; i++) ctx.v_buf[i] = rdna4_icbc_id(&ctx);

    rdna4_icbc_op(&ctx.types, 19, 2); rdna4_icbc_push(&ctx.types, ctx.t_void);
    rdna4_icbc_op(&ctx.types, 20, 2); rdna4_icbc_push(&ctx.types, ctx.t_bool);
    rdna4_icbc_op(&ctx.types, 21, 4); rdna4_icbc_push(&ctx.types, ctx.t_u32); rdna4_icbc_push(&ctx.types, 32u); rdna4_icbc_push(&ctx.types, 0u);
    rdna4_icbc_op(&ctx.types, 23, 4); rdna4_icbc_push(&ctx.types, ctx.t_v3u32); rdna4_icbc_push(&ctx.types, ctx.t_u32); rdna4_icbc_push(&ctx.types, 3u);
    rdna4_icbc_op(&ctx.types, 33, 3); rdna4_icbc_push(&ctx.types, ctx.t_fn); rdna4_icbc_push(&ctx.types, ctx.t_void);
    rdna4_icbc_op(&ctx.types, 32, 4); rdna4_icbc_push(&ctx.types, ctx.t_p_in_v3u32); rdna4_icbc_push(&ctx.types, 1u); rdna4_icbc_push(&ctx.types, ctx.t_v3u32);
    rdna4_icbc_op(&ctx.types, 32, 4); rdna4_icbc_push(&ctx.types, ctx.t_p_sb_u32); rdna4_icbc_push(&ctx.types, 12u); rdna4_icbc_push(&ctx.types, ctx.t_u32);
    rdna4_icbc_op(&ctx.types, 29, 3); rdna4_icbc_push(&ctx.types, ctx.t_rt_u32); rdna4_icbc_push(&ctx.types, ctx.t_u32);
    rdna4_icbc_op(&ctx.types, 30, 3); rdna4_icbc_push(&ctx.types, ctx.t_st_buf); rdna4_icbc_push(&ctx.types, ctx.t_rt_u32);
    rdna4_icbc_op(&ctx.types, 32, 4); rdna4_icbc_push(&ctx.types, ctx.t_p_sb_st); rdna4_icbc_push(&ctx.types, 12u); rdna4_icbc_push(&ctx.types, ctx.t_st_buf);

    rdna4_icbc_dec(&ctx.dec, ctx.v_gid, 11u, 1, 28u);
    rdna4_icbc_dec(&ctx.dec, ctx.t_rt_u32, 6u, 1, 4u);
    rdna4_icbc_mdec(&ctx.dec, ctx.t_st_buf, 0u, 35u, 0u);
    rdna4_icbc_dec(&ctx.dec, ctx.t_st_buf, 2u, 0, 0u);
    for (uint32_t i = 0; i < 5u; i++) {
        rdna4_icbc_dec(&ctx.dec, ctx.v_buf[i], 34u, 1, 0u);
        rdna4_icbc_dec(&ctx.dec, ctx.v_buf[i], 33u, 1, i);
        if (i < 3u) rdna4_icbc_dec(&ctx.dec, ctx.v_buf[i], 24u, 0, 0u);
        else rdna4_icbc_dec(&ctx.dec, ctx.v_buf[i], 25u, 0, 0u);
    }

    rdna4_icbc_op(&ctx.vars, 59, 4); rdna4_icbc_push(&ctx.vars, ctx.t_p_in_v3u32); rdna4_icbc_push(&ctx.vars, ctx.v_gid); rdna4_icbc_push(&ctx.vars, 1u);
    for (int i = 0; i < 5; i++) {
        rdna4_icbc_op(&ctx.vars, 59, 4); rdna4_icbc_push(&ctx.vars, ctx.t_p_sb_st); rdna4_icbc_push(&ctx.vars, ctx.v_buf[i]); rdna4_icbc_push(&ctx.vars, 12u);
    }

    uint32_t c0 = rdna4_icbc_const(&ctx, 0u), c1 = rdna4_icbc_const(&ctx, 1u), c2 = rdna4_icbc_const(&ctx, 2u);
    uint32_t c3 = rdna4_icbc_const(&ctx, 3u), c4 = rdna4_icbc_const(&ctx, 4u), c5 = rdna4_icbc_const(&ctx, 5u);
    uint32_t c6 = rdna4_icbc_const(&ctx, 6u), c16 = rdna4_icbc_const(&ctx, 16u), c31 = rdna4_icbc_const(&ctx, 31u);
    uint32_t c32 = rdna4_icbc_const(&ctx, 32u), c64 = rdna4_icbc_const(&ctx, 64u);
    uint32_t c_magic = rdna4_icbc_const(&ctx, RDNA4_ICB_MAGIC);
    uint32_t c_64g_hi = rdna4_icbc_const(&ctx, 0x10u);
    uint32_t c_tb_str = rdna4_icbc_const(&ctx, RDNA4_ICB_TABLE_V2_STRIDE / 4u);
    uint32_t c_slot_dw = rdna4_icbc_const(&ctx, RDNA4_ICB_SLOT_DWORDS);
    uint32_t c_128 = rdna4_icbc_const(&ctx, 128u);
    uint32_t c_pk_base = rdna4_icbc_const(&ctx, (uint32_t)(offsetof(rdna4_icb_compute_expand_cfg_t, parent_k_table) / 4u));
    uint32_t c_false_id = rdna4_icbc_id(&ctx);
    rdna4_icbc_op(&ctx.types, 42, 3); rdna4_icbc_push(&ctx.types, ctx.t_bool); rdna4_icbc_push(&ctx.types, c_false_id);

    uint32_t b_entry = rdna4_icbc_id(&ctx), b_act = rdna4_icbc_id(&ctx), b_slot = rdna4_icbc_id(&ctx);
    uint32_t b_hdr_v = rdna4_icbc_id(&ctx), b_hdr_body = rdna4_icbc_id(&ctx), b_hdr_c = rdna4_icbc_id(&ctx), b_hdr_d = rdna4_icbc_id(&ctx);
    uint32_t b_emit = rdna4_icbc_id(&ctx);
    uint32_t b_p1_h = rdna4_icbc_id(&ctx), b_p1_b = rdna4_icbc_id(&ctx), b_p1_c = rdna4_icbc_id(&ctx), b_p1_d = rdna4_icbc_id(&ctx);
    uint32_t b_p2_h = rdna4_icbc_id(&ctx), b_p2_b = rdna4_icbc_id(&ctx), b_p2_c = rdna4_icbc_id(&ctx), b_p2_d = rdna4_icbc_id(&ctx);
    uint32_t b_ret = rdna4_icbc_id(&ctx);

    rdna4_icbc_op(&ctx.code, 54, 5);
    rdna4_icbc_push(&ctx.code, ctx.t_void); rdna4_icbc_push(&ctx.code, fn_main); rdna4_icbc_push(&ctx.code, 0u); rdna4_icbc_push(&ctx.code, ctx.t_fn);
    rdna4_icbc_lbl(&ctx, b_entry);

    uint32_t gid_v = rdna4_icbc_id(&ctx), k = rdna4_icbc_id(&ctx);
    rdna4_icbc_op(&ctx.code, 61, 4); rdna4_icbc_push(&ctx.code, ctx.t_v3u32); rdna4_icbc_push(&ctx.code, gid_v); rdna4_icbc_push(&ctx.code, ctx.v_gid);
    rdna4_icbc_op(&ctx.code, 81, 5); rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, k); rdna4_icbc_push(&ctx.code, gid_v); rdna4_icbc_push(&ctx.code, 0u);

    uint32_t max_out = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 3u);
    uint32_t k_in = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, k, max_out);
    rdna4_icbc_smerge(&ctx, b_ret);
    rdna4_icbc_bcond(&ctx, k_in, b_act, b_ret);

    rdna4_icbc_lbl(&ctx, b_act);
    uint32_t dir_loc = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 0u);
    uint32_t dir_len = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 1u);
    uint32_t use_ind = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 2u);
    uint32_t inh_pipe = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 4u);
    uint32_t inh_buf = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 5u);
    uint32_t num_bkt = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 6u);
    uint32_t ptok_lo = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 8u);
    uint32_t ptok_hi = rdna4_icbc_ld_c(&ctx, ctx.v_buf[1], 9u);
    uint32_t ind_loc = rdna4_icbc_ld_c(&ctx, ctx.v_buf[2], 0u);
    uint32_t ind_len = rdna4_icbc_ld_c(&ctx, ctx.v_buf[2], 1u);
    uint32_t u_ind_b = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, use_ind, c0);
    uint32_t loc = rdna4_icbc_sel(&ctx, ctx.t_u32, u_ind_b, ind_loc, dir_loc);
    uint32_t rlen = rdna4_icbc_sel(&ctx, ctx.t_u32, u_ind_b, ind_len, dir_len);
    uint32_t magic = rdna4_icbc_ld_c(&ctx, ctx.v_buf[0], 0u);
    uint32_t max_cmd = rdna4_icbc_ld_c(&ctx, ctx.v_buf[0], 1u);
    uint32_t cmd_types = rdna4_icbc_ld_c(&ctx, ctx.v_buf[0], 2u);

    uint32_t mag_ok = rdna4_icbc_op2(&ctx, 170, ctx.t_bool, magic, c_magic);
    uint32_t c_len = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, k, rlen);
    uint32_t c_loc = rdna4_icbc_op2(&ctx, 178, ctx.t_bool, loc, max_cmd);
    uint32_t rem_cmd = rdna4_icbc_op2(&ctx, 130, ctx.t_u32, max_cmd, loc);
    uint32_t c_rem = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, k, rem_cmd);
    uint32_t act0 = rdna4_icbc_op2(&ctx, 167, ctx.t_bool, mag_ok, c_len);
    uint32_t act1 = rdna4_icbc_op2(&ctx, 167, ctx.t_bool, c_loc, c_rem);
    uint32_t in_range = rdna4_icbc_op2(&ctx, 167, ctx.t_bool, act0, act1);
    uint32_t tbbase = rdna4_icbc_op2(&ctx, 132, ctx.t_u32, k, c_tb_str);

    rdna4_icbc_smerge(&ctx, b_emit);
    rdna4_icbc_bcond(&ctx, in_range, b_slot, b_emit);

    /* b_slot: inspect slot[loc + k] and build outKTable[k] */
    rdna4_icbc_lbl(&ctx, b_slot);
    uint32_t slot_idx = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, loc, k);
    uint32_t sbase = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, c16, rdna4_icbc_op2(&ctx, 132, ctx.t_u32, slot_idx, c_slot_dw));

    uint32_t kind = rdna4_icbc_ld(&ctx, ctx.v_buf[0], sbase);
    uint32_t sflags = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, c3));
    uint32_t stok_lo = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, c4));
    uint32_t stok_hi = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, c5));
    uint32_t gx = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 8u)));
    uint32_t gy = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 9u)));
    uint32_t gz = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 10u)));
    uint32_t tx = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 11u)));
    uint32_t ty = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 12u)));
    uint32_t tz = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_const(&ctx, 13u)));

    uint32_t is_tg = rdna4_icbc_op2(&ctx, 170, ctx.t_bool, kind, c5);
    uint32_t is_th = rdna4_icbc_op2(&ctx, 170, ctx.t_bool, kind, c6);
    uint32_t k_ok = rdna4_icbc_op2(&ctx, 166, ctx.t_bool, is_tg, is_th);
    uint32_t f_inv0 = rdna4_icbc_op2(&ctx, 170, ctx.t_bool, rdna4_icbc_op2(&ctx, 199, ctx.t_u32, sflags, c2), c0);
    uint32_t inh_p_b = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, inh_pipe, c0);
    uint32_t f_ps1 = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, rdna4_icbc_op2(&ctx, 199, ctx.t_u32, sflags, c1), c0);
    uint32_t pso_ok = rdna4_icbc_op2(&ctx, 166, ctx.t_bool, inh_p_b, f_ps1);
    uint32_t cbit = rdna4_icbc_sel(&ctx, ctx.t_u32, is_th, c64, c32);
    uint32_t c_perm1 = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, rdna4_icbc_op2(&ctx, 199, ctx.t_u32, cmd_types, cbit), c0);
    uint32_t g_nz = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, rdna4_icbc_op2(&ctx, 171, ctx.t_bool, gx, c0), rdna4_icbc_op2(&ctx, 171, ctx.t_bool, gy, c0)),
        rdna4_icbc_op2(&ctx, 171, ctx.t_bool, gz, c0));
    uint32_t t_nz = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, rdna4_icbc_op2(&ctx, 171, ctx.t_bool, tx, c0), rdna4_icbc_op2(&ctx, 171, ctx.t_bool, ty, c0)),
        rdna4_icbc_op2(&ctx, 171, ctx.t_bool, tz, c0));
    uint32_t s_valid = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, rdna4_icbc_op2(&ctx, 167, ctx.t_bool, k_ok, f_inv0), rdna4_icbc_op2(&ctx, 167, ctx.t_bool, pso_ok, c_perm1)),
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, g_nz, t_nz));

    uint32_t s_tok_lo = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_p_b, ptok_lo, stok_lo);
    uint32_t s_tok_hi = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_p_b, ptok_hi, stok_hi);
    uint32_t atx = rdna4_icbc_sel(&ctx, ctx.t_u32, is_th, gx, rdna4_icbc_op2(&ctx, 132, ctx.t_u32, gx, tx));
    uint32_t aty = rdna4_icbc_sel(&ctx, ctx.t_u32, is_th, gy, rdna4_icbc_op2(&ctx, 132, ctx.t_u32, gy, ty));
    uint32_t atz = rdna4_icbc_sel(&ctx, ctx.t_u32, is_th, gz, rdna4_icbc_op2(&ctx, 132, ctx.t_u32, gz, tz));

    /* Copy kernel buffer table slots 0..30 */
    uint32_t inh_buf_b = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, inh_buf, c0);
    uint32_t s_var = rdna4_icbc_id(&ctx), s_next = rdna4_icbc_id(&ctx);
    rdna4_icbc_br(&ctx, b_hdr_v);

    rdna4_icbc_lbl(&ctx, b_hdr_v);
    rdna4_icbc_op(&ctx.code, 245, 7);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, s_var);
    rdna4_icbc_push(&ctx.code, c0); rdna4_icbc_push(&ctx.code, b_slot);
    rdna4_icbc_push(&ctx.code, s_next); rdna4_icbc_push(&ctx.code, b_hdr_c);
    uint32_t s_lt31 = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, s_var, c31);
    rdna4_icbc_lmerge(&ctx, b_hdr_d, b_hdr_c);
    rdna4_icbc_bcond(&ctx, s_lt31, b_hdr_body, b_hdr_d);

    rdna4_icbc_lbl(&ctx, b_hdr_body);
    uint32_t s2 = rdna4_icbc_op2(&ctx, 196, ctx.t_u32, s_var, c1);
    uint32_t sk_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sbase, rdna4_icbc_op2(&ctx, 128, ctx.t_u32, rdna4_icbc_const(&ctx, 24u), s2));
    uint32_t sk_lo = rdna4_icbc_ld(&ctx, ctx.v_buf[0], sk_off);
    uint32_t sk_hi = rdna4_icbc_ld(&ctx, ctx.v_buf[0], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, sk_off, c1));

    uint32_t pka_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, c_pk_base, s2);
    uint32_t pkl_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, pka_off, c64);
    uint32_t pkc_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, pka_off, c_128);
    uint32_t pk_lo = rdna4_icbc_ld(&ctx, ctx.v_buf[1], pka_off);
    uint32_t pk_hi = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, pka_off, c1));
    uint32_t pkl_lo = rdna4_icbc_ld(&ctx, ctx.v_buf[1], pkl_off);
    uint32_t pkl_hi = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, pkl_off, c1));
    uint32_t pkc_lo = rdna4_icbc_ld(&ctx, ctx.v_buf[1], pkc_off);
    uint32_t pkc_hi = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, pkc_off, c1));

    uint32_t sk_nz = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, rdna4_icbc_op2(&ctx, 198, ctx.t_u32, sk_lo, sk_hi), c0);
    uint32_t ek_lo = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pk_lo, sk_lo);
    uint32_t ek_hi = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pk_hi, sk_hi);
    uint32_t ekl_lo = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pkl_lo, c0);
    uint32_t ekl_hi = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pkl_hi, rdna4_icbc_sel(&ctx, ctx.t_u32, sk_nz, c_64g_hi, c0));
    uint32_t ekc_lo = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pkc_lo, rdna4_icbc_sel(&ctx, ctx.t_u32, sk_nz, c3, c0));
    uint32_t ekc_hi = rdna4_icbc_sel(&ctx, ctx.t_u32, inh_buf_b, pkc_hi, c0);

    uint32_t ta_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, tbbase, s2);
    uint32_t tl_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, ta_off, c64);
    uint32_t tc_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, ta_off, c_128);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], ta_off, ek_lo);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, ta_off, c1), ek_hi);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], tl_off, ekl_lo);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, tl_off, c1), ekl_hi);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], tc_off, ekc_lo);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, tc_off, c1), ekc_hi);

    rdna4_icbc_br(&ctx, b_hdr_c);
    rdna4_icbc_lbl(&ctx, b_hdr_c);
    rdna4_icbc_op(&ctx.code, 128, 5);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, s_next); rdna4_icbc_push(&ctx.code, s_var); rdna4_icbc_push(&ctx.code, c1);
    rdna4_icbc_br(&ctx, b_hdr_v);

    /* Store slot[31] metadata tail: address[31]={atx,aty}, length[31]={atz,sflags}, access[31]={0,0} */
    rdna4_icbc_lbl(&ctx, b_hdr_d);
    uint32_t t31_a = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, tbbase, rdna4_icbc_const(&ctx, 62u));
    uint32_t t31_l = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, t31_a, c64);
    uint32_t t31_c = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, t31_a, c_128);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], t31_a, rdna4_icbc_sel(&ctx, ctx.t_u32, s_valid, atx, c0));
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, t31_a, c1), rdna4_icbc_sel(&ctx, ctx.t_u32, s_valid, aty, c0));
    rdna4_icbc_st(&ctx, ctx.v_buf[4], t31_l, rdna4_icbc_sel(&ctx, ctx.t_u32, s_valid, atz, c0));
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, t31_l, c1), rdna4_icbc_sel(&ctx, ctx.t_u32, s_valid, sflags, c0));
    rdna4_icbc_st(&ctx, ctx.v_buf[4], t31_c, c0);
    rdna4_icbc_st(&ctx, ctx.v_buf[4], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, t31_c, c1), c0);
    rdna4_icbc_br(&ctx, b_emit);

    /* b_emit: merge [b_hdr_d, b_act], find exact bucket match, then emit VkDispatchIndirectCommand per bucket */
    rdna4_icbc_lbl(&ctx, b_emit);
    uint32_t p_valid = rdna4_icbc_phi2(&ctx, ctx.t_bool, s_valid, b_hdr_d, c_false_id, b_act);
    uint32_t p_is_th = rdna4_icbc_phi2(&ctx, ctx.t_bool, is_th, b_hdr_d, c_false_id, b_act);
    uint32_t p_tlo = rdna4_icbc_phi2(&ctx, ctx.t_u32, s_tok_lo, b_hdr_d, c0, b_act);
    uint32_t p_thi = rdna4_icbc_phi2(&ctx, ctx.t_u32, s_tok_hi, b_hdr_d, c0, b_act);
    uint32_t p_gx = rdna4_icbc_phi2(&ctx, ctx.t_u32, gx, b_hdr_d, c0, b_act);
    uint32_t p_gy = rdna4_icbc_phi2(&ctx, ctx.t_u32, gy, b_hdr_d, c0, b_act);
    uint32_t p_gz = rdna4_icbc_phi2(&ctx, ctx.t_u32, gz, b_hdr_d, c0, b_act);
    uint32_t p_tx = rdna4_icbc_phi2(&ctx, ctx.t_u32, tx, b_hdr_d, c0, b_act);
    uint32_t p_ty = rdna4_icbc_phi2(&ctx, ctx.t_u32, ty, b_hdr_d, c0, b_act);
    uint32_t p_tz = rdna4_icbc_phi2(&ctx, ctx.t_u32, tz, b_hdr_d, c0, b_act);

    /* Pass 1: check if any bucket matches (token, tg) exactly */
    uint32_t b1_var = rdna4_icbc_id(&ctx), b1_next = rdna4_icbc_id(&ctx);
    uint32_t ae_var = rdna4_icbc_id(&ctx), ae_next = rdna4_icbc_id(&ctx);
    rdna4_icbc_br(&ctx, b_p1_h);

    rdna4_icbc_lbl(&ctx, b_p1_h);
    rdna4_icbc_op(&ctx.code, 245, 7);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, b1_var);
    rdna4_icbc_push(&ctx.code, c0); rdna4_icbc_push(&ctx.code, b_emit);
    rdna4_icbc_push(&ctx.code, b1_next); rdna4_icbc_push(&ctx.code, b_p1_c);
    rdna4_icbc_op(&ctx.code, 245, 7);
    rdna4_icbc_push(&ctx.code, ctx.t_bool); rdna4_icbc_push(&ctx.code, ae_var);
    rdna4_icbc_push(&ctx.code, c_false_id); rdna4_icbc_push(&ctx.code, b_emit);
    rdna4_icbc_push(&ctx.code, ae_next); rdna4_icbc_push(&ctx.code, b_p1_c);
    uint32_t b1_lt = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, b1_var, num_bkt);
    rdna4_icbc_lmerge(&ctx, b_p1_d, b_p1_c);
    rdna4_icbc_bcond(&ctx, b1_lt, b_p1_b, b_p1_d);

    rdna4_icbc_lbl(&ctx, b_p1_b);
    uint32_t b1_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, c16, rdna4_icbc_op2(&ctx, 196, ctx.t_u32, b1_var, c3));
    uint32_t b1_tlo = rdna4_icbc_ld(&ctx, ctx.v_buf[1], b1_off);
    uint32_t b1_thi = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b1_off, c1));
    uint32_t b1_lx = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b1_off, c2));
    uint32_t b1_ly = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b1_off, c3));
    uint32_t b1_lz = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b1_off, c4));
    uint32_t p1_tok = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tlo, b1_tlo),
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_thi, b1_thi));
    uint32_t p1_tg = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tx, b1_lx), rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_ty, b1_ly)),
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tz, b1_lz));
    uint32_t p1_hit = rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p_valid, rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p1_tok, p1_tg));
    rdna4_icbc_op(&ctx.code, 166, 5);
    rdna4_icbc_push(&ctx.code, ctx.t_bool); rdna4_icbc_push(&ctx.code, ae_next); rdna4_icbc_push(&ctx.code, ae_var); rdna4_icbc_push(&ctx.code, p1_hit);
    rdna4_icbc_br(&ctx, b_p1_c);

    rdna4_icbc_lbl(&ctx, b_p1_c);
    rdna4_icbc_op(&ctx.code, 128, 5);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, b1_next); rdna4_icbc_push(&ctx.code, b1_var); rdna4_icbc_push(&ctx.code, c1);
    rdna4_icbc_br(&ctx, b_p1_h);

    /* Pass 2: emit VkDispatchIndirectCommand (3 dwords) for every bucket */
    rdna4_icbc_lbl(&ctx, b_p1_d);
    uint32_t not_any_exact = rdna4_icbc_op1(&ctx, 168, ctx.t_bool, ae_var);
    uint32_t b2_var = rdna4_icbc_id(&ctx), b2_next = rdna4_icbc_id(&ctx);
    rdna4_icbc_br(&ctx, b_p2_h);

    rdna4_icbc_lbl(&ctx, b_p2_h);
    rdna4_icbc_op(&ctx.code, 245, 7);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, b2_var);
    rdna4_icbc_push(&ctx.code, c0); rdna4_icbc_push(&ctx.code, b_p1_d);
    rdna4_icbc_push(&ctx.code, b2_next); rdna4_icbc_push(&ctx.code, b_p2_c);
    uint32_t b2_lt = rdna4_icbc_op2(&ctx, 176, ctx.t_bool, b2_var, num_bkt);
    rdna4_icbc_lmerge(&ctx, b_p2_d, b_p2_c);
    rdna4_icbc_bcond(&ctx, b2_lt, b_p2_b, b_p2_d);

    rdna4_icbc_lbl(&ctx, b_p2_b);
    uint32_t b2_off = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, c16, rdna4_icbc_op2(&ctx, 196, ctx.t_u32, b2_var, c3));
    uint32_t b2_tlo = rdna4_icbc_ld(&ctx, ctx.v_buf[1], b2_off);
    uint32_t b2_thi = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b2_off, c1));
    uint32_t b2_lx = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b2_off, c2));
    uint32_t b2_ly = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b2_off, c3));
    uint32_t b2_lz = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b2_off, c4));
    uint32_t b2_flg = rdna4_icbc_ld(&ctx, ctx.v_buf[1], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, b2_off, c5));

    uint32_t p2_tok = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tlo, b2_tlo),
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_thi, b2_thi));
    uint32_t p2_tg = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tx, b2_lx), rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_ty, b2_ly)),
        rdna4_icbc_op2(&ctx, 170, ctx.t_bool, p_tz, b2_lz));
    uint32_t exact_m = rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p_valid, rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p2_tok, p2_tg));
    uint32_t fb_ok = rdna4_icbc_op2(&ctx, 171, ctx.t_bool, rdna4_icbc_op2(&ctx, 199, ctx.t_u32, b2_flg, c1), c0);
    uint32_t fb_m = rdna4_icbc_op2(&ctx, 167, ctx.t_bool,
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p_valid, p_is_th),
        rdna4_icbc_op2(&ctx, 167, ctx.t_bool, not_any_exact, rdna4_icbc_op2(&ctx, 167, ctx.t_bool, p2_tok, fb_ok)));
    uint32_t matched = rdna4_icbc_op2(&ctx, 166, ctx.t_bool, exact_m, fb_m);

    uint32_t slx = rdna4_icbc_sel(&ctx, ctx.t_u32, rdna4_icbc_op2(&ctx, 171, ctx.t_bool, b2_lx, c0), b2_lx, c1);
    uint32_t sly = rdna4_icbc_sel(&ctx, ctx.t_u32, rdna4_icbc_op2(&ctx, 171, ctx.t_bool, b2_ly, c0), b2_ly, c1);
    uint32_t slz = rdna4_icbc_sel(&ctx, ctx.t_u32, rdna4_icbc_op2(&ctx, 171, ctx.t_bool, b2_lz, c0), b2_lz, c1);
    uint32_t sgx = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, p_gx, c1);
    uint32_t sgy = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, p_gy, c1);
    uint32_t sgz = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, p_gz, c1);
    uint32_t th_x = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, rdna4_icbc_op2(&ctx, 134, ctx.t_u32, rdna4_icbc_op2(&ctx, 130, ctx.t_u32, sgx, c1), slx), c1);
    uint32_t th_y = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, rdna4_icbc_op2(&ctx, 134, ctx.t_u32, rdna4_icbc_op2(&ctx, 130, ctx.t_u32, sgy, c1), sly), c1);
    uint32_t th_z = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, rdna4_icbc_op2(&ctx, 134, ctx.t_u32, rdna4_icbc_op2(&ctx, 130, ctx.t_u32, sgz, c1), slz), c1);
    uint32_t ox = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, rdna4_icbc_sel(&ctx, ctx.t_u32, p_is_th, th_x, p_gx), c0);
    uint32_t oy = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, rdna4_icbc_sel(&ctx, ctx.t_u32, p_is_th, th_y, p_gy), c0);
    uint32_t oz = rdna4_icbc_sel(&ctx, ctx.t_u32, matched, rdna4_icbc_sel(&ctx, ctx.t_u32, p_is_th, th_z, p_gz), c0);

    uint32_t disp_idx = rdna4_icbc_op2(&ctx, 128, ctx.t_u32, rdna4_icbc_op2(&ctx, 132, ctx.t_u32, k, num_bkt), b2_var);
    uint32_t dbase = rdna4_icbc_op2(&ctx, 132, ctx.t_u32, disp_idx, c3);
    rdna4_icbc_st(&ctx, ctx.v_buf[3], dbase, ox);
    rdna4_icbc_st(&ctx, ctx.v_buf[3], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, dbase, c1), oy);
    rdna4_icbc_st(&ctx, ctx.v_buf[3], rdna4_icbc_op2(&ctx, 128, ctx.t_u32, dbase, c2), oz);

    rdna4_icbc_br(&ctx, b_p2_c);
    rdna4_icbc_lbl(&ctx, b_p2_c);
    rdna4_icbc_op(&ctx.code, 128, 5);
    rdna4_icbc_push(&ctx.code, ctx.t_u32); rdna4_icbc_push(&ctx.code, b2_next); rdna4_icbc_push(&ctx.code, b2_var); rdna4_icbc_push(&ctx.code, c1);
    rdna4_icbc_br(&ctx, b_p2_h);

    rdna4_icbc_lbl(&ctx, b_p2_d);
    rdna4_icbc_br(&ctx, b_ret);
    rdna4_icbc_lbl(&ctx, b_ret);
    rdna4_icbc_op(&ctx.code, 253, 1);
    rdna4_icbc_op(&ctx.code, 56, 1);

    rdna4_icbc_buf_t spv = {0};
    rdna4_icbc_push(&spv, 0x07230203u);
    rdna4_icbc_push(&spv, 0x00010500u);
    rdna4_icbc_push(&spv, 0u);
    rdna4_icbc_push(&spv, ctx.next_id);
    rdna4_icbc_push(&spv, 0u);
    rdna4_icbc_op(&spv, 17, 2); rdna4_icbc_push(&spv, 1u);
    rdna4_icbc_op(&spv, 14, 3); rdna4_icbc_push(&spv, 0u); rdna4_icbc_push(&spv, 1u);
    rdna4_icbc_op(&spv, 15, (uint16_t)(3u + 2u + 6u));
    rdna4_icbc_push(&spv, 5u);
    rdna4_icbc_push(&spv, fn_main);
    rdna4_icbc_str(&spv, "main");
    rdna4_icbc_push(&spv, ctx.v_gid);
    for (int i = 0; i < 5; i++) rdna4_icbc_push(&spv, ctx.v_buf[i]);
    rdna4_icbc_op(&spv, 16, 6);
    rdna4_icbc_push(&spv, fn_main); rdna4_icbc_push(&spv, 17u);
    rdna4_icbc_push(&spv, 64u); rdna4_icbc_push(&spv, 1u); rdna4_icbc_push(&spv, 1u);
    for (size_t i = 0; i < ctx.dec.len; i++) rdna4_icbc_push(&spv, ctx.dec.w[i]);
    for (size_t i = 0; i < ctx.types.len; i++) rdna4_icbc_push(&spv, ctx.types.w[i]);
    for (size_t i = 0; i < ctx.vars.len; i++) rdna4_icbc_push(&spv, ctx.vars.w[i]);
    for (size_t i = 0; i < ctx.code.len; i++) rdna4_icbc_push(&spv, ctx.code.w[i]);

    free(ctx.dec.w); free(ctx.types.w); free(ctx.vars.w); free(ctx.code.w);
    if (ctx.dec.oom || ctx.types.oom || ctx.vars.oom || ctx.code.oom || spv.oom) {
        free(spv.w); return 0;
    }
    *out_words = spv.w;
    *out_nwords = spv.len;
    return 1;
}

#ifdef RDNA4_SPVREFL_H
static inline void rdna4s_icb_compute_inspect(const uint32_t *words, size_t count,
                                              int *out_has_barrier, int *out_allow_fallback) {
    int has_barrier = 0, uses_wg = 0;
    uint32_t wg_size_id = 0u;
    if (words && count >= 5u && words[0] == 0x07230203u) {
        int in_fn = 0;
        size_t pos = 5u;
        while (pos < count) {
            uint32_t sz = words[pos] >> 16, op = words[pos] & 0xffffu;
            if (!sz || pos + sz > count) break;
            if (op == 54u) in_fn = 1;
            else if (op == 56u) in_fn = 0;
            else if (op == 224u) { has_barrier = 1; uses_wg = 1; }
            else if (op == 59u && sz >= 4u && words[pos + 3u] == 4u) uses_wg = 1; /* Workgroup */
            else if (op == 71u && sz >= 4u && words[pos + 2u] == 11u) {
                uint32_t bi = words[pos + 3u];
                if (bi == 24u || bi == 26u || bi == 27u || bi == 29u) uses_wg = 1;
                else if (bi == 25u) wg_size_id = words[pos + 1u]; /* WorkgroupSize: only blocks fallback if read in code */
            }
            if (in_fn && wg_size_id && !uses_wg) {
                for (uint32_t j = 1u; j < sz; j++) {
                    if (words[pos + j] == wg_size_id) { uses_wg = 1; break; }
                }
            }
            pos += sz;
        }
    }
    if (out_has_barrier) *out_has_barrier = has_barrier;
    if (out_allow_fallback) *out_allow_fallback = !uses_wg;
}

static inline void rdna4_icbc_emit_op(rdna4_icbc_buf_t *out, uint16_t op, const uint32_t *args, size_t nargs) {
    rdna4_icbc_op(out, op, (uint16_t)(nargs + 1u));
    for (size_t i = 0; i < nargs; i++) rdna4_icbc_push(out, args[i]);
}

/* Rewrites an icb_stage-lowered compute SPIR-V module for GPU indirect dispatch:
 * - ThreadsDynamic push-constant block (12 u32 words) -> active_threads from
 *   binding 676 slot[31] + BuiltIn NumWorkgroups (0 for tbase/gbase).
 * - For barrier-free shaders with binding 676, guards entry with gid < active_threads
 *   so non-multiple ConcurrentDispatchThreads never executes out-of-range lanes. */
static inline int rdna4s_icb_compute_indirect(const uint32_t *words, size_t count,
                                              uint32_t offset, int is_dynamic,
                                              const uint32_t local[3],
                                              uint32_t **output, size_t *output_count,
                                              char *why, size_t why_size) {
    RDNA4sMod module = {0};
    uint32_t *slots = NULL;
    rdna4_icbc_buf_t out = {0};
    int failed = 1;
    *output = NULL;
    *output_count = 0;
#define RDNA4IC_FAIL(msg) do { if (why && why_size) snprintf(why, why_size, "%s", (msg)); goto done; } while (0)
    if (!words || count < 5u || count > 1000000u || !local || !local[0] || !local[1] || !local[2])
        RDNA4IC_FAIL("invalid compute ICB module or local size");
    if (rdna4s_parse(words, count, &module, why, why_size)) goto done;
    if (!module.has_ep || module.ep_model != 5u) RDNA4IC_FAIL("ICB compute module is not GLCompute");

    int has_barrier = 0;
    rdna4s_icb_compute_inspect(words, count, &has_barrier, NULL);

    uint32_t push = 0u, scalar = 0u, bool_ty = 0u, vector = 0u, pointer = 0u, groups = 0u, gid_var = 0u;
    uint32_t table_var = 0u, table_sc = 12u, tbl_elem = 0u, ptr_sb_tbl_elem = 0u;
    int tbl_is_vec2 = 0;

    if (is_dynamic) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *v = &module.id[id];
            if (v->op != 59u || v->sc != 9u) continue;
            if (push) RDNA4IC_FAIL("multiple compute push-constant blocks");
            RDNA4sId *ty = &module.id[module.id[v->elem].elem];
            if (ty->op != 30u || ty->n != 12u || !ty->moff)
                RDNA4IC_FAIL("compute ICB dispatch metadata is not twelve scalar words");
            scalar = ty->mem[0];
            if (module.id[scalar].op != 21u || module.id[scalar].width != 32u)
                RDNA4IC_FAIL("compute ICB dispatch metadata is not 32-bit integer data");
            for (uint32_t m = 0u; m < 12u; ++m)
                if (ty->mem[m] != scalar || ty->moff[m] != offset + 4u * m)
                    RDNA4IC_FAIL("compute ICB dispatch metadata layout differs from translator ABI");
            push = id;
        }
    }
    if (!scalar) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *t = &module.id[id];
            if (t->op == 21u && t->width == 32u && t->signedness == 0u) { scalar = id; break; }
        }
    }
    for (uint32_t id = 1u; id < module.bound; ++id) {
        RDNA4sId *v = &module.id[id];
        if (v->op != 59u || (v->sc != 12u && v->sc != 2u)) continue;
        if ((v->set == 0 || v->set < 0) && v->bind == (int)RDNA4_ICB_TABLE_BINDING) {
            uint32_t st_id = module.id[v->elem].elem;
            if (st_id <= module.bound && module.id[st_id].op == 30u && module.id[st_id].n >= 1u) {
                uint32_t rt_id = module.id[st_id].mem[0];
                int arr_ok = (rt_id <= module.bound &&
                              (module.id[rt_id].op == 29u ||
                               (module.id[rt_id].op == 28u && module.id[rt_id].n <= module.bound &&
                                module.id[module.id[rt_id].n].op == 43u &&
                                module.id[module.id[rt_id].n].cval >= 64u)));
                if (arr_ok) {
                    uint32_t el_id = module.id[rt_id].elem;
                    if (el_id <= module.bound) {
                        RDNA4sId *el = &module.id[el_id];
                        if (el->op == 21u && el->width == 64u) {
                            table_var = id; table_sc = v->sc; tbl_elem = el_id; tbl_is_vec2 = 0; break;
                        } else if (el->op == 23u && el->n == 2u && el->elem <= module.bound &&
                                   module.id[el->elem].op == 21u && module.id[el->elem].width == 32u) {
                            table_var = id; table_sc = v->sc; tbl_elem = el_id; tbl_is_vec2 = 1; break;
                        }
                    }
                }
            }
        }
    }

    int add_guard = (!has_barrier && table_var != 0u);
    if (!push && !add_guard) {
        uint32_t *copy = (uint32_t *)malloc(count * sizeof(uint32_t));
        if (!copy) RDNA4IC_FAIL("compute ICB transformation allocation failed");
        memcpy(copy, words, count * sizeof(uint32_t));
        *output = copy;
        *output_count = count;
        failed = 0;
        goto done;
    }

    for (uint32_t id = 1u; id < module.bound; ++id) {
        if (module.id[id].op == 20u) { bool_ty = id; break; }
    }
    if (scalar) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *t = &module.id[id];
            if (t->op == 23u && t->elem == scalar && t->n == 3u) { vector = id; break; }
        }
    }
    if (vector) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *t = &module.id[id];
            if (t->op == 32u && t->sc == 1u && t->elem == vector) { pointer = id; break; }
        }
    }
    if (pointer) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *v = &module.id[id];
            if (v->op == 59u && v->sc == 1u && v->elem == pointer) {
                if (v->builtin == 24) groups = id;
                if (v->builtin == 28) gid_var = id;
            }
        }
    }
    if (tbl_elem) {
        for (uint32_t id = 1u; id < module.bound; ++id) {
            RDNA4sId *t = &module.id[id];
            if (t->op == 32u && t->sc == table_sc && t->elem == tbl_elem) { ptr_sb_tbl_elem = id; break; }
        }
    }

    int need_table_loads = (table_var != 0u) && (add_guard || push != 0u);
    int add_scalar = !scalar;
    int add_bool = add_guard && !bool_ty;
    int add_vector = !vector;
    int add_pointer = !pointer;
    int add_groups = (push != 0u) && !groups;
    int add_gid = add_guard && !gid_var;
    int add_ptr_tbl = need_table_loads && !ptr_sb_tbl_elem;

    uint32_t next = words[3];
    if (add_scalar) scalar = next++;
    if (add_bool) bool_ty = next++;
    if (add_vector) vector = next++;
    if (add_pointer) pointer = next++;
    if (add_groups) groups = next++;
    if (add_gid) gid_var = next++;
    if (add_ptr_tbl) ptr_sb_tbl_elem = next++;

    uint32_t c0 = next++, c31 = 0u, c32 = 0u, c63 = 0u;
    uint32_t locals[3] = {0u, 0u, 0u};
    if (need_table_loads) {
        c31 = next++;
        c63 = next++;
        if (!tbl_is_vec2) c32 = next++;
    }
    if (push && !table_var) {
        for (unsigned d = 0u; d < 3u; ++d) locals[d] = next++;
    }

    uint32_t ret_lbl = add_guard ? next++ : 0u;
    uint32_t cont_lbl = add_guard ? next++ : 0u;
    uint32_t orig_entry_lbl = 0u;
    size_t guard_insert_pos = 0u;

    /* Locate the entry block label and the exact position after all Function OpVariables. */
    if (add_guard) {
        int in_ep = 0, in_eb = 0;
        for (size_t pos = 5u; pos < count;) {
            uint32_t sz = words[pos] >> 16, op = words[pos] & 0xffffu;
            if (!sz || pos + sz > count) break;
            if (op == 54u && sz >= 3u && words[pos + 2u] == module.ep_id) in_ep = 1;
            else if (op == 56u) { if (in_ep) break; }
            else if (in_ep && op == 248u && sz == 2u) {
                if (!orig_entry_lbl) {
                    orig_entry_lbl = words[pos + 1u];
                    guard_insert_pos = pos + sz;
                    in_eb = 1;
                } else {
                    break;
                }
            } else if (in_eb && op == 59u && sz >= 4u && words[pos + 3u] == 7u) {
                guard_insert_pos = pos + sz;
            }
            pos += sz;
        }
        if (!orig_entry_lbl || !guard_insert_pos) RDNA4IC_FAIL("compute ICB entry function has no entry block label");
    }

    slots = (uint32_t *)calloc((size_t)module.bound + 1u, sizeof(*slots));
    if (!slots) RDNA4IC_FAIL("compute ICB transformation allocation failed");

    for (size_t i = 0; i < 5u; i++) rdna4_icbc_push(&out, words[i]);

    int decorated = 0, declared = 0, entries = 0;
    int in_ep = 0, guard_emitted = 0;
    for (size_t pos = 5u; pos < count;) {
        const uint32_t *inst = words + pos;
        uint32_t sz = inst[0] >> 16, op = inst[0] & 0xffffu;
        if (!sz || pos + sz > count) RDNA4IC_FAIL("truncated SPIR-V instruction");

        if (!decorated && op >= 19u && op <= 39u) {
            if (add_groups) {
                uint32_t a[3] = { groups, 11u, 24u };
                rdna4_icbc_emit_op(&out, 71, a, 3u);
            }
            if (add_gid) {
                uint32_t a[3] = { gid_var, 11u, 28u };
                rdna4_icbc_emit_op(&out, 71, a, 3u);
            }
            decorated = 1;
        }
        if (!declared && op == 54u) {
            if (add_scalar) { uint32_t a[3] = { scalar, 32u, 0u }; rdna4_icbc_emit_op(&out, 21, a, 3u); }
            if (add_bool) { uint32_t a[1] = { bool_ty }; rdna4_icbc_emit_op(&out, 20, a, 1u); }
            if (add_vector) { uint32_t a[3] = { vector, scalar, 3u }; rdna4_icbc_emit_op(&out, 23, a, 3u); }
            if (add_pointer) { uint32_t a[3] = { pointer, 1u, vector }; rdna4_icbc_emit_op(&out, 32, a, 3u); }
            if (add_ptr_tbl) { uint32_t a[3] = { ptr_sb_tbl_elem, table_sc, tbl_elem }; rdna4_icbc_emit_op(&out, 32, a, 3u); }
            if (add_groups) { uint32_t a[3] = { pointer, groups, 1u }; rdna4_icbc_emit_op(&out, 59, a, 3u); }
            if (add_gid) { uint32_t a[3] = { pointer, gid_var, 1u }; rdna4_icbc_emit_op(&out, 59, a, 3u); }
            { uint32_t a[3] = { scalar, c0, 0u }; rdna4_icbc_emit_op(&out, 43, a, 3u); }
            if (need_table_loads) {
                uint32_t a31[3] = { scalar, c31, 31u }; rdna4_icbc_emit_op(&out, 43, a31, 3u);
                uint32_t a63[3] = { scalar, c63, 63u }; rdna4_icbc_emit_op(&out, 43, a63, 3u);
                if (!tbl_is_vec2) { uint32_t a32[3] = { scalar, c32, 32u }; rdna4_icbc_emit_op(&out, 43, a32, 3u); }
            }
            if (push && !table_var) {
                for (unsigned d = 0u; d < 3u; ++d) {
                    uint32_t al[3] = { scalar, locals[d], local[d] };
                    rdna4_icbc_emit_op(&out, 43, al, 3u);
                }
            }
            declared = 1;
        }
        if (op == 15u) {
            if (++entries != 1) RDNA4IC_FAIL("multiple entry points in compute ICB module");
            uint32_t extra = (uint32_t)(add_groups + add_gid);
            rdna4_icbc_push(&out, inst[0] + (extra << 16));
            for (uint32_t i = 1u; i < sz; i++) rdna4_icbc_push(&out, inst[i]);
            if (add_groups) rdna4_icbc_push(&out, groups);
            if (add_gid) rdna4_icbc_push(&out, gid_var);
            pos += sz;
            continue;
        }
        if (op == 54u && sz >= 3u && inst[2] == module.ep_id) {
            in_ep = 1;
        } else if (op == 56u) {
            in_ep = 0;
        }

        /* Emit entry bounds guard right after all Function-scope OpVariable declarations. */
        if (add_guard && !guard_emitted && pos == guard_insert_pos) {
            uint32_t ac31 = next++, v31 = next++, ac63 = next++, v63 = next++;
            uint32_t atx = next++, aty = next++, atz = next++;
            { uint32_t a[5] = { ptr_sb_tbl_elem, ac31, table_var, c0, c31 }; rdna4_icbc_emit_op(&out, 65, a, 5u); }
            { uint32_t a[3] = { tbl_elem, v31, ac31 }; rdna4_icbc_emit_op(&out, 61, a, 3u); }
            { uint32_t a[5] = { ptr_sb_tbl_elem, ac63, table_var, c0, c63 }; rdna4_icbc_emit_op(&out, 65, a, 5u); }
            { uint32_t a[3] = { tbl_elem, v63, ac63 }; rdna4_icbc_emit_op(&out, 61, a, 3u); }
            if (tbl_is_vec2) {
                uint32_t ax[4] = { scalar, atx, v31, 0u }; rdna4_icbc_emit_op(&out, 81, ax, 4u);
                uint32_t ay[4] = { scalar, aty, v31, 1u }; rdna4_icbc_emit_op(&out, 81, ay, 4u);
                uint32_t az[4] = { scalar, atz, v63, 0u }; rdna4_icbc_emit_op(&out, 81, az, 4u);
            } else {
                uint32_t v31hi = next++;
                uint32_t ax[3] = { scalar, atx, v31 }; rdna4_icbc_emit_op(&out, 113, ax, 3u);
                uint32_t sh[4] = { tbl_elem, v31hi, v31, c32 }; rdna4_icbc_emit_op(&out, 194, sh, 4u);
                uint32_t ay[3] = { scalar, aty, v31hi }; rdna4_icbc_emit_op(&out, 113, ay, 3u);
                uint32_t az[3] = { scalar, atz, v63 }; rdna4_icbc_emit_op(&out, 113, az, 3u);
            }
            uint32_t gv = next++, gx = next++, gy = next++, gz = next++;
            uint32_t okx = next++, oky = next++, okz = next++, okxy = next++, okall = next++, oob = next++;
            { uint32_t a[3] = { vector, gv, gid_var }; rdna4_icbc_emit_op(&out, 61, a, 3u); }
            { uint32_t a[4] = { scalar, gx, gv, 0u }; rdna4_icbc_emit_op(&out, 81, a, 4u); }
            { uint32_t a[4] = { scalar, gy, gv, 1u }; rdna4_icbc_emit_op(&out, 81, a, 4u); }
            { uint32_t a[4] = { scalar, gz, gv, 2u }; rdna4_icbc_emit_op(&out, 81, a, 4u); }
            { uint32_t a[4] = { bool_ty, okx, gx, atx }; rdna4_icbc_emit_op(&out, 176, a, 4u); }
            { uint32_t a[4] = { bool_ty, oky, gy, aty }; rdna4_icbc_emit_op(&out, 176, a, 4u); }
            { uint32_t a[4] = { bool_ty, okz, gz, atz }; rdna4_icbc_emit_op(&out, 176, a, 4u); }
            { uint32_t a[4] = { bool_ty, okxy, okx, oky }; rdna4_icbc_emit_op(&out, 167, a, 4u); }
            { uint32_t a[4] = { bool_ty, okall, okxy, okz }; rdna4_icbc_emit_op(&out, 167, a, 4u); }
            { uint32_t a[3] = { bool_ty, oob, okall }; rdna4_icbc_emit_op(&out, 168, a, 3u); }
            { uint32_t a[2] = { cont_lbl, 0u }; rdna4_icbc_emit_op(&out, 247, a, 2u); }
            { uint32_t a[3] = { oob, ret_lbl, cont_lbl }; rdna4_icbc_emit_op(&out, 250, a, 3u); }
            { uint32_t a[1] = { ret_lbl }; rdna4_icbc_emit_op(&out, 248, a, 1u); }
            rdna4_icbc_emit_op(&out, 253, NULL, 0u);
            { uint32_t a[1] = { cont_lbl }; rdna4_icbc_emit_op(&out, 248, a, 1u); }
            guard_emitted = 1;
        }

        if (push && (op == 65u || op == 66u) && sz >= 4u && inst[3] == push) {
            if (sz != 5u || inst[2] >= module.bound || inst[4] >= module.bound ||
                module.id[inst[4]].op != 43u || module.id[inst[4]].cval >= 12u)
                RDNA4IC_FAIL("unsupported access to compute ICB dispatch metadata");
            slots[inst[2]] = module.id[inst[4]].cval + 1u;
        }
        if (push && op == 61u && sz >= 4u && inst[3] < module.bound && slots[inst[3]]) {
            if (sz != 4u || inst[1] != scalar)
                RDNA4IC_FAIL("unsupported load from compute ICB dispatch metadata");
            uint32_t member = slots[inst[3]] - 1u, id = inst[2];
            if (member >= 3u && member < 9u) {
                uint32_t a[3] = { scalar, id, c0 };
                rdna4_icbc_emit_op(&out, 83, a, 3u);
            } else if (member < 3u && table_var != 0u) {
                uint32_t idx_c = (member < 2u) ? c31 : c63;
                uint32_t ac = next++, raw = next++;
                { uint32_t a[5] = { ptr_sb_tbl_elem, ac, table_var, c0, idx_c }; rdna4_icbc_emit_op(&out, 65, a, 5u); }
                { uint32_t a[3] = { tbl_elem, raw, ac }; rdna4_icbc_emit_op(&out, 61, a, 3u); }
                if (tbl_is_vec2) {
                    uint32_t lane = (member == 1u) ? 1u : 0u;
                    uint32_t a[4] = { scalar, id, raw, lane };
                    rdna4_icbc_emit_op(&out, 81, a, 4u);
                } else if (member == 1u) {
                    uint32_t hi = next++;
                    uint32_t sh[4] = { tbl_elem, hi, raw, c32 }; rdna4_icbc_emit_op(&out, 194, sh, 4u);
                    uint32_t cv[3] = { scalar, id, hi }; rdna4_icbc_emit_op(&out, 113, cv, 3u);
                } else {
                    uint32_t cv[3] = { scalar, id, raw }; rdna4_icbc_emit_op(&out, 113, cv, 3u);
                }
            } else {
                uint32_t loaded = next++, dim = member % 3u;
                { uint32_t a[3] = { vector, loaded, groups }; rdna4_icbc_emit_op(&out, 61, a, 3u); }
                if (member < 3u) {
                    uint32_t extracted = next++;
                    uint32_t ae[4] = { scalar, extracted, loaded, dim }; rdna4_icbc_emit_op(&out, 81, ae, 4u);
                    uint32_t am[4] = { scalar, id, extracted, locals[dim] }; rdna4_icbc_emit_op(&out, 132, am, 4u);
                } else {
                    uint32_t ae[4] = { scalar, id, loaded, dim }; rdna4_icbc_emit_op(&out, 81, ae, 4u);
                }
            }
        } else {
            if (push && op == 61u && sz >= 4u && inst[3] == push)
                RDNA4IC_FAIL("whole-block compute ICB dispatch metadata load is unsupported");
            if (add_guard && in_ep && op == 245u && sz >= 5u) {
                for (uint32_t j = 0u; j < sz; j++) {
                    uint32_t w = inst[j];
                    if (j >= 4u && ((j - 4u) & 1u) == 0u && w == orig_entry_lbl) w = cont_lbl;
                    rdna4_icbc_push(&out, w);
                }
            } else {
                for (uint32_t j = 0u; j < sz; j++) rdna4_icbc_push(&out, inst[j]);
            }
        }
        pos += sz;
    }

    if (!decorated || !declared || (add_guard && !guard_emitted) || out.oom)
        RDNA4IC_FAIL("compute ICB SPIR-V transform failed or OOM");
    out.w[3] = next;
    *output = out.w;
    *output_count = out.len;
    out.w = NULL;
    failed = 0;
done:
    free(out.w);
    free(slots);
    rdna4s_free(&module);
#undef RDNA4IC_FAIL
    return failed;
}
#endif /* RDNA4_SPVREFL_H */

#endif
