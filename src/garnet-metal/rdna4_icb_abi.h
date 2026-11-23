// Shared binary ABI between GarnetMetal (C/ObjC), GPU compute expander SPIR-V,
// and garnetxlate / metal2vulkan (Rust) for GPU-visible Indirect Command Buffers.
#ifndef RDNA4_ICB_ABI_H
#define RDNA4_ICB_ABI_H

#include <stddef.h>
#include <stdint.h>

#define RDNA4_ICB_MAGIC                  0x49434231u /* 'ICB1' */
#define RDNA4_ICB_HEADER_BYTES           64u
#define RDNA4_ICB_SLOT_BYTES             640u        /* 160 dwords, 64B aligned */
#define RDNA4_ICB_SLOT_DWORDS            160u
#define RDNA4_ICB_MAX_GPU_SLOTS          65536u
#define RDNA4_ICB_MAX_CANDIDATE_PIPELINES 32u
#define RDNA4_ICB_MAX_GPU_EXPANDED_DRAWS 131072u
#define RDNA4_ICB_VS_PARAMS_STRIDE       512u        /* 256B aligned dynamic SSBO stride */
#define RDNA4_ICB_TESS_PARAMS_STRIDE     768u        /* 256B aligned (>= 560B rdna4_tess_draw_params_t) */

#define RDNA4_ICB_PSO_TOKEN_MASK         UINT64_C(0xFFFFFFFF00000000)
#define RDNA4_ICB_PSO_TOKEN_TAG          UINT64_C(0x5244505300000000) /* 'RDPS' << 32 */

#define RDNA4_ICB_KIND_EMPTY                0u
#define RDNA4_ICB_KIND_DRAW                 1u
#define RDNA4_ICB_KIND_DRAW_INDEXED         2u
#define RDNA4_ICB_KIND_DRAW_PATCHES         3u
#define RDNA4_ICB_KIND_DRAW_INDEXED_PATCHES 4u

#define RDNA4_ICB_FLAG_INHERIT_PIPELINE  (1u << 0)
#define RDNA4_ICB_FLAG_INHERIT_BUFFERS   (1u << 1)

#define RDNA4_ICB_SLOT_FLAG_PIPELINE_SET (1u << 0)
#define RDNA4_ICB_SLOT_FLAG_INVALID      (1u << 1)

#define RDNA4_ICB_PRIM_POINT             0u
#define RDNA4_ICB_PRIM_LINE              1u
#define RDNA4_ICB_PRIM_LINE_STRIP        2u
#define RDNA4_ICB_PRIM_TRIANGLE          3u
#define RDNA4_ICB_PRIM_TRIANGLE_STRIP    4u

#define RDNA4_ICB_INDEX_UINT16           0u
#define RDNA4_ICB_INDEX_UINT32           1u

#define RDNA4_ICB_CMD_BIT_DRAW                 (1u << 0)
#define RDNA4_ICB_CMD_BIT_DRAW_INDEXED         (1u << 1)
#define RDNA4_ICB_CMD_BIT_DRAW_PATCHES         (1u << 2)
#define RDNA4_ICB_CMD_BIT_DRAW_INDEXED_PATCHES (1u << 3)
#define RDNA4_ICB_CMD_MASK                     0x0Fu

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

#endif
