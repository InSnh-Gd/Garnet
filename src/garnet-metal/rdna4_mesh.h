// rdna4_mesh.h: pure-C Metal 3 Mesh & Object shader pipeline validation, Footnote 10 checker,
// reflection JSON parser (0076/0078), SPIR-V execution-model inspector, emulated Object+Mesh
// scratch/indirect layout planner, and draw/indirect bounds validation.
// No Metal, no Foundation, no required Vulkan headers (adds Vulkan ABI asserts when vulkan.h is included).
#ifndef RDNA4_MESH_H
#define RDNA4_MESH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rdna4_devcaps.h"

// =================================================================================================
// 1. Locked C ABI shared with garnetxlate (0078) for GFX-ring emulated Object+Mesh
// =================================================================================================

#ifndef GARNETX_CAP_MESH_EMULATED_PAYLOAD
#define GARNETX_CAP_MESH_EMULATED_PAYLOAD (1u << 8)
#endif
#ifndef GARNETX_MESH_GRID_PARAMS_BINDING
#define GARNETX_MESH_GRID_PARAMS_BINDING  673u
#endif
#ifndef GARNETX_MESH_PAYLOAD_BINDING
#define GARNETX_MESH_PAYLOAD_BINDING      674u
#endif
#ifndef GARNETX_MESH_INDIRECT_BINDING
#define GARNETX_MESH_INDIRECT_BINDING     675u
#endif

#define RDNA4MESH_EMUL_BIT_GRID_PARAMS    (UINT32_C(1) << 0)
#define RDNA4MESH_EMUL_BIT_PAYLOAD        (UINT32_C(1) << 1)
#define RDNA4MESH_EMUL_BIT_INDIRECT       (UINT32_C(1) << 2)

#define RDNA4MESH_MAX_PAYLOAD_BYTES       RDNA4DC_MESH_MAX_PAYLOAD_BYTES
#define RDNA4MESH_MIN_GRID                RDNA4DC_MESH_MIN_GRID
#define RDNA4MESH_MAX_INVOCATIONS         RDNA4DC_MESH_MAX_INVOCATIONS
#define RDNA4MESH_MAX_VERTICES            RDNA4DC_MESH_MAX_VERTICES
#define RDNA4MESH_MAX_PRIMITIVES          RDNA4DC_MESH_MAX_PRIMITIVES
#define RDNA4MESH_PAYLOAD_MIN_STRIDE      UINT32_C(16)
#define RDNA4MESH_PAYLOAD_ALIGN           UINT32_C(16)
#define RDNA4MESH_GRID_PARAMS_BYTES       UINT32_C(16)
#define RDNA4MESH_INDIRECT_CMD_BYTES      UINT32_C(12)
#define RDNA4MESH_MAX_TG_MEM_BYTES        UINT32_C(32768)
#define RDNA4MESH_TG_MEM_ALIGN            UINT32_C(16)
#define RDNA4MESH_MAX_TG_MEM_INDICES      UINT32_C(31)
#define RDNA4MESH_MAX_BUFFERS             UINT32_C(31)
#define RDNA4MESH_MAX_TEXTURES            UINT32_C(128)
#define RDNA4MESH_MAX_SAMPLERS            UINT32_C(16)
#define RDNA4MESH_MAX_VARYINGS            UINT32_C(64)
#define RDNA4MESH_MAX_STAGE_OUTPUTS       UINT32_C(32)
#define RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS   UINT32_C(1024)
#define RDNA4MESH_EMUL_INDIRECT_BUDGET_BYTES (UINT64_C(16) << 20)
#define RDNA4MESH_JSON_MAX_DEPTH          64u

// SPIR-V 1.4+ numeric constants (verified against spirv 1.4.341 registry).
#define RDNA4MESH_SPV_MAGIC               UINT32_C(0x07230203)
#define RDNA4MESH_SPV_EM_FRAGMENT         UINT32_C(4)
#define RDNA4MESH_SPV_EM_GLCOMPUTE        UINT32_C(5)
#define RDNA4MESH_SPV_EM_TASK_EXT         UINT32_C(5364)
#define RDNA4MESH_SPV_EM_MESH_EXT         UINT32_C(5365)
#define RDNA4MESH_SPV_MODE_LOCAL_SIZE     UINT32_C(17)
#define RDNA4MESH_SPV_MODE_OUT_VERTICES   UINT32_C(26)
#define RDNA4MESH_SPV_MODE_OUT_POINTS     UINT32_C(27)
#define RDNA4MESH_SPV_MODE_OUT_LINES_EXT  UINT32_C(5269)
#define RDNA4MESH_SPV_MODE_OUT_PRIMS_EXT  UINT32_C(5270)
#define RDNA4MESH_SPV_MODE_OUT_TRIS_EXT   UINT32_C(5298)
#define RDNA4MESH_SPV_CAP_DRAW_PARAMS     UINT32_C(4427)
#define RDNA4MESH_SPV_CAP_RAY_QUERY_KHR   UINT32_C(4472)
#define RDNA4MESH_SPV_CAP_RAY_CULL_KHR    UINT32_C(4478)
#define RDNA4MESH_SPV_CAP_RAY_TRACING_KHR UINT32_C(4479)
#define RDNA4MESH_SPV_CAP_MESH_SHADING    UINT32_C(5283)

// Set-0 binding 673 (GARNETX_MESH_GRID_PARAMS_BINDING) GPU storage-buffer layout.
typedef struct {
    uint32_t obj_grid_x;
    uint32_t obj_grid_y;
    uint32_t obj_grid_z;
    uint32_t payload_stride_bytes;
} rdna4mesh_grid_params_t;
_Static_assert(sizeof(rdna4mesh_grid_params_t) == RDNA4MESH_GRID_PARAMS_BYTES,
               "rdna4mesh_grid_params_t must be 16 bytes");
_Static_assert(offsetof(rdna4mesh_grid_params_t, payload_stride_bytes) == 12u,
               "payload_stride_bytes offset");

// Set-0 binding 675 (GARNETX_MESH_INDIRECT_BINDING) / MTLDispatchThreadgroupsIndirectArguments /
// VkDrawMeshTasksIndirectCommandEXT 12-byte command layout.
typedef struct {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
} rdna4mesh_indirect_cmd_t;
_Static_assert(sizeof(rdna4mesh_indirect_cmd_t) == RDNA4MESH_INDIRECT_CMD_BYTES,
               "rdna4mesh_indirect_cmd_t must be 12 bytes");

#if defined(VK_EXT_MESH_SHADER_SPEC_VERSION)
_Static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == sizeof(rdna4mesh_indirect_cmd_t),
               "VkDrawMeshTasksIndirectCommandEXT size mismatch");
_Static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountX) ==
               offsetof(rdna4mesh_indirect_cmd_t, group_count_x), "groupCountX offset");
_Static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountY) ==
               offsetof(rdna4mesh_indirect_cmd_t, group_count_y), "groupCountY offset");
_Static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountZ) ==
               offsetof(rdna4mesh_indirect_cmd_t, group_count_z), "groupCountZ offset");
#endif

// =================================================================================================
// 2. Status codes, execution modes, and safe arithmetic
// =================================================================================================

typedef enum {
    RDNA4MESH_OK = 0,
    RDNA4MESH_ERR_INVALID_ARG,
    RDNA4MESH_ERR_UNSUPPORTED,
    RDNA4MESH_ERR_PAYLOAD_LIMIT,
    RDNA4MESH_ERR_GRID_LIMIT,
    RDNA4MESH_ERR_WORKGROUP_LIMIT,
    RDNA4MESH_ERR_OUTPUT_LIMIT,
    RDNA4MESH_ERR_TOPOLOGY,
    RDNA4MESH_ERR_STAGE_LINKAGE,
    RDNA4MESH_ERR_FOOTNOTE10_FN_PTR,
    RDNA4MESH_ERR_FOOTNOTE10_RT,
    RDNA4MESH_ERR_AMP_UNSUPPORTED,
    RDNA4MESH_ERR_ALIGNMENT,
    RDNA4MESH_ERR_BOUNDS,
    RDNA4MESH_ERR_OVERFLOW,
    RDNA4MESH_ERR_JSON,
    RDNA4MESH_ERR_SPIRV,
    RDNA4MESH_ERR_TG_MEM,
} rdna4mesh_status_t;

static inline const char *rdna4mesh_status_str(rdna4mesh_status_t st) {
    switch (st) {
    case RDNA4MESH_OK:                    return "ok";
    case RDNA4MESH_ERR_INVALID_ARG:       return "invalid mesh pipeline or encoder argument";
    case RDNA4MESH_ERR_UNSUPPORTED:       return "mesh shaders are not supported on this backend";
    case RDNA4MESH_ERR_PAYLOAD_LIMIT:     return "object-to-mesh payload exceeds 16,384-byte or descriptor limit";
    case RDNA4MESH_ERR_GRID_LIMIT:        return "mesh or object threadgroup grid exceeds device or pipeline limit";
    case RDNA4MESH_ERR_WORKGROUP_LIMIT:   return "object or mesh threadgroup dimensions exceed device or pipeline limit";
    case RDNA4MESH_ERR_OUTPUT_LIMIT:      return "mesh shader output vertices or primitives exceed device limit";
    case RDNA4MESH_ERR_TOPOLOGY:          return "unsupported or missing mesh output primitive topology";
    case RDNA4MESH_ERR_STAGE_LINKAGE:     return "mesh pipeline stage linkage mismatch (object/mesh/fragment)";
    case RDNA4MESH_ERR_FOOTNOTE10_FN_PTR:
        return "Footnote 10: mesh/object render pipelines cannot combine render function pointers or visible function tables";
    case RDNA4MESH_ERR_FOOTNOTE10_RT:
        return "Footnote 10: mesh/object render pipelines cannot combine render-stage ray tracing";
    case RDNA4MESH_ERR_AMP_UNSUPPORTED:   return "unsupported vertex amplification count in mesh pipeline";
    case RDNA4MESH_ERR_ALIGNMENT:         return "misaligned mesh buffer, indirect offset, or threadgroup memory length";
    case RDNA4MESH_ERR_BOUNDS:            return "mesh indirect buffer or scratch allocation out of bounds";
    case RDNA4MESH_ERR_OVERFLOW:          return "integer overflow computing mesh grid or scratch buffer size";
    case RDNA4MESH_ERR_JSON:              return "malformed mesh or object reflection JSON";
    case RDNA4MESH_ERR_SPIRV:             return "malformed or stage-mismatched mesh/object SPIR-V module";
    case RDNA4MESH_ERR_TG_MEM:            return "object threadgroup memory index, alignment, or total length invalid";
    }
    return "unknown mesh runtime error";
}

typedef enum {
    RDNA4MESH_MODE_UNSUPPORTED          = 0,
    RDNA4MESH_MODE_DIRECT_MESH_ONLY     = 1, // MeshEXT alone on GFX ring
    RDNA4MESH_MODE_NATIVE_TASK_MESH     = 2, // Native TaskEXT + MeshEXT
    RDNA4MESH_MODE_EMULATED_OBJECT_MESH = 3, // GLCompute Object + MeshEXT (GFX ring, taskShader=false)
} rdna4mesh_exec_mode_t;

typedef enum {
    RDNA4MESH_TOPO_INVALID  = 0,
    RDNA4MESH_TOPO_POINT    = 1,
    RDNA4MESH_TOPO_LINE     = 2,
    RDNA4MESH_TOPO_TRIANGLE = 3,
} rdna4mesh_topology_t;

typedef enum {
    RDNA4MESH_STAGE_UNKNOWN  = 0,
    RDNA4MESH_STAGE_VERTEX   = 1,
    RDNA4MESH_STAGE_FRAGMENT = 2,
    RDNA4MESH_STAGE_KERNEL   = 3,
    RDNA4MESH_STAGE_OBJECT   = 4,
    RDNA4MESH_STAGE_MESH     = 5,
} rdna4mesh_stage_t;

static inline int rdna4mesh_is_pow2_u64(uint64_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

static inline int rdna4mesh_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out || a > UINT64_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static inline int rdna4mesh_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out) return 0;
    if (a != 0u && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static inline int rdna4mesh_align_up_u64(uint64_t v, uint64_t align, uint64_t *out) {
    if (!out || !rdna4mesh_is_pow2_u64(align)) return 0;
    uint64_t mask = align - 1u;
    if (v > UINT64_MAX - mask) return 0;
    *out = (v + mask) & ~mask;
    return 1;
}

static inline rdna4mesh_status_t rdna4mesh_payload_stride(uint32_t payload_bytes,
                                                          uint32_t *stride_out) {
    if (!stride_out) return RDNA4MESH_ERR_INVALID_ARG;
    *stride_out = 0u;
    if (payload_bytes > RDNA4MESH_MAX_PAYLOAD_BYTES) return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    uint32_t base = payload_bytes < RDNA4MESH_PAYLOAD_MIN_STRIDE
        ? RDNA4MESH_PAYLOAD_MIN_STRIDE : payload_bytes;
    uint32_t stride = (base + (RDNA4MESH_PAYLOAD_ALIGN - 1u)) & ~(RDNA4MESH_PAYLOAD_ALIGN - 1u);
    if (stride > RDNA4MESH_MAX_PAYLOAD_BYTES) return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    *stride_out = stride;
    return RDNA4MESH_OK;
}

static inline int rdna4mesh_hw_mesh_baseline_ok(const rdna4dc_metal3_caps *m3) {
    return m3 && m3->mesh_shader &&
           m3->mesh_max_workgroup_total_count >= RDNA4MESH_MIN_GRID &&
           m3->mesh_max_workgroup_count[0] >= RDNA4MESH_MIN_GRID &&
           m3->mesh_max_workgroup_count[1] > 0u &&
           m3->mesh_max_workgroup_count[2] > 0u &&
           m3->mesh_max_workgroup_invocations > 0u &&
           m3->mesh_max_output_vertices > 0u &&
           m3->mesh_max_output_primitives > 0u;
}

static inline int rdna4mesh_hw_native_task_ok(const rdna4dc_metal3_caps *m3) {
    return rdna4mesh_hw_mesh_baseline_ok(m3) && m3->object_shader &&
           m3->mesh_max_payload_bytes >= RDNA4MESH_MAX_PAYLOAD_BYTES &&
           m3->object_max_workgroup_total_count > 0u &&
           m3->object_max_workgroup_count[0] > 0u &&
           m3->object_max_workgroup_count[1] > 0u &&
           m3->object_max_workgroup_count[2] > 0u &&
           m3->object_max_workgroup_invocations > 0u;
}

static inline rdna4mesh_exec_mode_t rdna4mesh_select_mode(const rdna4dc_metal3_caps *m3,
                                                          int has_object) {
    if (!rdna4mesh_hw_mesh_baseline_ok(m3)) return RDNA4MESH_MODE_UNSUPPORTED;
    if (!has_object) return RDNA4MESH_MODE_DIRECT_MESH_ONLY;
    if (rdna4mesh_hw_native_task_ok(m3)) return RDNA4MESH_MODE_NATIVE_TASK_MESH;
    // Live RX 9060 XT GFX ring 0 with mesa-patches/0029 exposes meshShader=1, taskShader=0:
    // emulate Object stage on GFX compute + indirect MeshEXT draw.
    return RDNA4MESH_MODE_EMULATED_OBJECT_MESH;
}

static inline int rdna4mesh_device_supports_mesh(const rdna4dc_metal3_caps *m3,
                                                 int allow_emulated_object) {
    if (!rdna4mesh_hw_mesh_baseline_ok(m3)) return 0;
    return allow_emulated_object ? 1 : rdna4mesh_hw_native_task_ok(m3);
}

// =================================================================================================
// 3. Footnote 10 mutual exclusion (rejects render fn-ptrs/VFT & RT; ALLOWS privateFunctions)
// =================================================================================================

enum {
    RDNA4MESH_FN10_PUBLIC_FUNCTIONS    = UINT32_C(1) << 0, // MTLLinkedFunctions.functions
    RDNA4MESH_FN10_BINARY_FUNCTIONS    = UINT32_C(1) << 1, // MTLLinkedFunctions.binaryFunctions
    RDNA4MESH_FN10_GROUPS              = UINT32_C(1) << 2, // MTLLinkedFunctions.groups
    RDNA4MESH_FN10_SUPPORT_ADD_BIN_FNS = UINT32_C(1) << 3, // supportAdding*BinaryFunctions
    RDNA4MESH_FN10_VFT_METADATA        = UINT32_C(1) << 4, // reflection visible_function_tables
    RDNA4MESH_FN10_RAYTRACING          = UINT32_C(1) << 5, // RT AS / IFT / RayQueryKHR / RayTracingKHR
    RDNA4MESH_FN10_PRIVATE_FUNCTIONS   = UINT32_C(1) << 6, // MTLLinkedFunctions.privateFunctions (ALLOWED)
};

static inline rdna4mesh_status_t rdna4mesh_check_footnote10(uint32_t flags, const char **why_out) {
    if (why_out) *why_out = NULL;
    const uint32_t fn_ptr_mask = RDNA4MESH_FN10_PUBLIC_FUNCTIONS |
                                 RDNA4MESH_FN10_BINARY_FUNCTIONS |
                                 RDNA4MESH_FN10_GROUPS |
                                 RDNA4MESH_FN10_SUPPORT_ADD_BIN_FNS |
                                 RDNA4MESH_FN10_VFT_METADATA;
    if (flags & fn_ptr_mask) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
        return RDNA4MESH_ERR_FOOTNOTE10_FN_PTR;
    }
    if (flags & RDNA4MESH_FN10_RAYTRACING) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT);
        return RDNA4MESH_ERR_FOOTNOTE10_RT;
    }
    // RDNA4MESH_FN10_PRIVATE_FUNCTIONS is explicitly permitted by Footnote 10 (inlined static helpers).
    return RDNA4MESH_OK;
}

// =================================================================================================
// 4. Pipeline descriptor validation
// =================================================================================================

typedef struct {
    int has_object_function;
    int has_mesh_function;
    int has_fragment_function;
    uint64_t payload_memory_length;
    uint64_t max_total_threadgroups_per_mesh_grid;
    uint64_t max_total_threads_per_object_threadgroup;
    uint64_t max_total_threads_per_mesh_threadgroup;
    int object_tg_size_mult_exec_width;
    int mesh_tg_size_mult_exec_width;
    uint64_t max_vertex_amplification_count;
    uint32_t footnote10_flags;
} rdna4mesh_desc_t;

static inline uint32_t rdna4mesh_effective_obj_max_invocations(const rdna4dc_caps *caps,
                                                               rdna4mesh_exec_mode_t mode) {
    if (!caps) return 0u;
    uint32_t raw = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH)
        ? caps->metal3.object_max_workgroup_invocations
        : caps->invocations;
    return raw > RDNA4MESH_MAX_INVOCATIONS ? RDNA4MESH_MAX_INVOCATIONS : raw;
}

static inline uint32_t rdna4mesh_effective_mesh_max_invocations(const rdna4dc_caps *caps) {
    if (!caps) return 0u;
    uint32_t raw = caps->metal3.mesh_max_workgroup_invocations;
    return raw > RDNA4MESH_MAX_INVOCATIONS ? RDNA4MESH_MAX_INVOCATIONS : raw;
}

static inline rdna4mesh_status_t rdna4mesh_validate_desc(const rdna4mesh_desc_t *d,
                                                         const rdna4dc_caps *caps,
                                                         uint32_t max_amp_supported,
                                                         rdna4mesh_exec_mode_t *mode_out,
                                                         const char **why_out) {
    if (mode_out) *mode_out = RDNA4MESH_MODE_UNSUPPORTED;
    if (why_out) *why_out = NULL;
    if (!d || !caps) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_INVALID_ARG);
        return RDNA4MESH_ERR_INVALID_ARG;
    }
    if (!d->has_mesh_function) {
        if (why_out) *why_out = "mesh pipeline descriptor requires a non-nil meshFunction";
        return RDNA4MESH_ERR_INVALID_ARG;
    }

    rdna4mesh_status_t fn10 = rdna4mesh_check_footnote10(d->footnote10_flags, why_out);
    if (fn10 != RDNA4MESH_OK) return fn10;

    uint64_t req_amp = d->max_vertex_amplification_count ? d->max_vertex_amplification_count : 1u;
    uint32_t amp_cap = max_amp_supported ? max_amp_supported : 1u;
    if (req_amp > amp_cap) {
        if (why_out) *why_out = "maxVertexAmplificationCount > 1 is not supported in mesh pipelines";
        return RDNA4MESH_ERR_AMP_UNSUPPORTED;
    }

    rdna4mesh_exec_mode_t mode = rdna4mesh_select_mode(&caps->metal3, d->has_object_function);
    if (mode == RDNA4MESH_MODE_UNSUPPORTED) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_UNSUPPORTED);
        return RDNA4MESH_ERR_UNSUPPORTED;
    }

    if (!d->has_object_function && d->payload_memory_length > 0u) {
        if (why_out) *why_out = "payloadMemoryLength must be 0 when objectFunction is nil";
        return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    }
    if (d->payload_memory_length > RDNA4MESH_MAX_PAYLOAD_BYTES) {
        if (why_out) *why_out = "payloadMemoryLength exceeds 16,384 bytes";
        return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    }
    if (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH &&
        d->payload_memory_length > caps->metal3.mesh_max_payload_bytes) {
        if (why_out) *why_out = "payloadMemoryLength exceeds backend maxTaskPayloadSize";
        return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    }

    if (d->max_total_threadgroups_per_mesh_grid > caps->metal3.mesh_max_workgroup_total_count) {
        if (why_out) *why_out = "maxTotalThreadgroupsPerMeshGrid exceeds backend maxMeshWorkGroupTotalCount";
        return RDNA4MESH_ERR_GRID_LIMIT;
    }

    uint32_t mesh_inv = rdna4mesh_effective_mesh_max_invocations(caps);
    if (mesh_inv == 0u || d->max_total_threads_per_mesh_threadgroup > mesh_inv) {
        if (why_out) *why_out = "maxTotalThreadsPerMeshThreadgroup exceeds backend limit";
        return RDNA4MESH_ERR_WORKGROUP_LIMIT;
    }

    if (d->has_object_function) {
        uint32_t obj_inv = rdna4mesh_effective_obj_max_invocations(caps, mode);
        if (obj_inv == 0u || d->max_total_threads_per_object_threadgroup > obj_inv) {
            if (why_out) *why_out = "maxTotalThreadsPerObjectThreadgroup exceeds backend limit";
            return RDNA4MESH_ERR_WORKGROUP_LIMIT;
        }
    } else if (d->max_total_threads_per_object_threadgroup > RDNA4MESH_MAX_INVOCATIONS) {
        if (why_out) *why_out = "maxTotalThreadsPerObjectThreadgroup exceeds 1024";
        return RDNA4MESH_ERR_WORKGROUP_LIMIT;
    }

    if (mode_out) *mode_out = mode;
    return RDNA4MESH_OK;
}

// Pick a legal initial LocalSize for pipeline creation when draw-time threadsPer*Threadgroup
// is not yet known (specialized lazily on draw if a different local size is requested).
static inline void rdna4mesh_initial_local_size(uint32_t desc_max_threads,
                                                uint32_t hw_max_invocations,
                                                uint32_t hw_max_x,
                                                uint32_t exec_width,
                                                int mult_exec_width,
                                                uint32_t out_local[3]) {
    uint32_t ew = exec_width ? exec_width : 32u;
    uint32_t limit = hw_max_invocations ? hw_max_invocations : 32u;
    if (limit > RDNA4MESH_MAX_INVOCATIONS) limit = RDNA4MESH_MAX_INVOCATIONS;
    if (hw_max_x && hw_max_x < limit) limit = hw_max_x;
    if (desc_max_threads && desc_max_threads < limit) limit = desc_max_threads;
    uint32_t x = limit < ew ? limit : ew;
    if (mult_exec_width && x >= ew) x = (x / ew) * ew;
    if (x == 0u) x = 1u;
    out_local[0] = x;
    out_local[1] = 1u;
    out_local[2] = 1u;
}

// =================================================================================================
// 5. Reflection JSON parser for 0076 / 0078 ("object", "mesh", "varyings", Footnote 10 metadata)
// =================================================================================================

typedef struct {
    uint32_t location;
    int per_primitive;
} rdna4mesh_varying_t;

typedef struct {
    int has_payload;
    int has_payload_size;
    uint32_t payload_size;
    int has_payload_stride;
    uint32_t payload_stride;
    int emulated_payload;
    int has_object_grid_dims;
    uint32_t object_grid_dims[3];
    int has_max_mesh_grid;
    uint32_t max_total_threadgroups_per_mesh_grid;
} rdna4mesh_object_refl_t;

typedef struct {
    rdna4mesh_topology_t topology;
    uint32_t max_vertices;
    uint32_t max_primitives;
    int has_payload;
    int has_payload_size;
    uint32_t payload_size;
    int has_payload_stride;
    uint32_t payload_stride;
    int emulated_payload;
    int writes_position;
    int writes_cull_primitive;
    int writes_layer;
    int writes_viewport_index;
    rdna4mesh_varying_t vertex_outputs[RDNA4MESH_MAX_STAGE_OUTPUTS];
    uint32_t vertex_output_count;
    uint64_t vertex_out_mask;
    rdna4mesh_varying_t primitive_outputs[RDNA4MESH_MAX_STAGE_OUTPUTS];
    uint32_t primitive_output_count;
    uint64_t primitive_out_mask;
} rdna4mesh_mesh_refl_t;

typedef struct {
    rdna4mesh_stage_t stage;
    char entry_point[128];
    int has_local_size;
    uint32_t local_size[3];
    int has_max_work_group_size;
    uint32_t max_work_group_size;
    int has_object;
    rdna4mesh_object_refl_t obj;
    int has_mesh;
    rdna4mesh_mesh_refl_t mesh;
    rdna4mesh_varying_t varyings[RDNA4MESH_MAX_VARYINGS];
    uint32_t varying_count;
    int has_vft;
    int has_raytracing;
    int has_sampler_coord_flags;
    int has_argument_tables;
} rdna4mesh_refl_t;

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} rdna4mj_cur_t;

static inline void rdna4mj_ws(rdna4mj_cur_t *cur) {
    while (cur->pos < cur->len) {
        char ch = cur->s[cur->pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') cur->pos++;
        else break;
    }
}

static inline int rdna4mj_peek(rdna4mj_cur_t *cur) {
    rdna4mj_ws(cur);
    return cur->pos < cur->len ? (int)(unsigned char)cur->s[cur->pos] : -1;
}

static inline int rdna4mj_consume(rdna4mj_cur_t *cur, char ch) {
    rdna4mj_ws(cur);
    if (cur->pos < cur->len && cur->s[cur->pos] == ch) {
        cur->pos++;
        return 1;
    }
    return 0;
}

static inline int rdna4mj_match_lit(rdna4mj_cur_t *cur, const char *lit) {
    rdna4mj_ws(cur);
    size_t n = strlen(lit);
    if (cur->len - cur->pos < n || memcmp(cur->s + cur->pos, lit, n) != 0) return 0;
    cur->pos += n;
    return 1;
}

static inline int rdna4mj_hex(char ch, uint32_t *out) {
    if (ch >= '0' && ch <= '9') { *out = (uint32_t)(ch - '0'); return 1; }
    if (ch >= 'a' && ch <= 'f') { *out = (uint32_t)(10 + (ch - 'a')); return 1; }
    if (ch >= 'A' && ch <= 'F') { *out = (uint32_t)(10 + (ch - 'A')); return 1; }
    return 0;
}

static inline int rdna4mj_parse_string(rdna4mj_cur_t *cur, char *out, size_t cap, size_t *out_len) {
    if (!rdna4mj_consume(cur, '"')) return 0;
    size_t n = 0;
    while (cur->pos < cur->len) {
        unsigned char ch = (unsigned char)cur->s[cur->pos++];
        if (ch == '"') {
            if (out && cap > 0u) {
                if (n >= cap) return 0;
                out[n] = '\0';
            }
            if (out_len) *out_len = n;
            return 1;
        }
        if (ch < 0x20u) return 0;
        if (ch != '\\') {
            if (out) {
                if (n + 1u >= cap) return 0;
                out[n] = (char)ch;
            }
            n++;
            continue;
        }
        if (cur->pos >= cur->len) return 0;
        char esc = cur->s[cur->pos++];
        char mapped = 0;
        switch (esc) {
        case '"':  mapped = '"'; break;
        case '\\': mapped = '\\'; break;
        case '/':  mapped = '/'; break;
        case 'b':  mapped = '\b'; break;
        case 'f':  mapped = '\f'; break;
        case 'n':  mapped = '\n'; break;
        case 'r':  mapped = '\r'; break;
        case 't':  mapped = '\t'; break;
        case 'u': {
            if (cur->len - cur->pos < 4u) return 0;
            uint32_t cp = 0;
            for (size_t k = 0; k < 4u; k++) {
                uint32_t d = 0;
                if (!rdna4mj_hex(cur->s[cur->pos + k], &d)) return 0;
                cp = (cp << 4) | d;
            }
            cur->pos += 4u;
            if (cp == 0u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
            uint8_t utf8[3];
            size_t ulen = 0;
            if (cp <= 0x7Fu) {
                utf8[0] = (uint8_t)cp; ulen = 1u;
            } else if (cp <= 0x7FFu) {
                utf8[0] = (uint8_t)(0xC0u | (cp >> 6));
                utf8[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
                ulen = 2u;
            } else {
                utf8[0] = (uint8_t)(0xE0u | (cp >> 12));
                utf8[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                utf8[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
                ulen = 3u;
            }
            if (out) {
                if (n + ulen >= cap) return 0;
                memcpy(out + n, utf8, ulen);
            }
            n += ulen;
            continue;
        }
        default:
            return 0;
        }
        if (out) {
            if (n + 1u >= cap) return 0;
            out[n] = mapped;
        }
        n++;
    }
    return 0;
}

static inline int rdna4mj_parse_bool(rdna4mj_cur_t *cur, int *out) {
    int c = rdna4mj_peek(cur);
    if (c == 't' && rdna4mj_match_lit(cur, "true")) { *out = 1; return 1; }
    if (c == 'f' && rdna4mj_match_lit(cur, "false")) { *out = 0; return 1; }
    return 0;
}

static inline int rdna4mj_parse_u32(rdna4mj_cur_t *cur, uint32_t *out) {
    rdna4mj_ws(cur);
    if (cur->pos >= cur->len) return 0;
    char first = cur->s[cur->pos];
    if (first < '0' || first > '9') return 0;
    uint64_t val = 0;
    if (first == '0') {
        cur->pos++;
        if (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') return 0;
    } else {
        while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') {
            val = val * 10u + (uint64_t)(cur->s[cur->pos++] - '0');
            if (val > UINT32_MAX) return 0;
        }
    }
    if (cur->pos < cur->len &&
        (cur->s[cur->pos] == '.' || cur->s[cur->pos] == 'e' || cur->s[cur->pos] == 'E'))
        return 0;
    *out = (uint32_t)val;
    return 1;
}

static inline int rdna4mj_parse_opt_u32(rdna4mj_cur_t *cur, int *present, uint32_t *out) {
    if (rdna4mj_peek(cur) == 'n') {
        if (!rdna4mj_match_lit(cur, "null")) return 0;
        *present = 0;
        *out = 0u;
        return 1;
    }
    if (!rdna4mj_parse_u32(cur, out)) return 0;
    *present = 1;
    return 1;
}

static inline int rdna4mj_parse_u32_3(rdna4mj_cur_t *cur, uint32_t out[3]) {
    if (!rdna4mj_consume(cur, '[')) return 0;
    for (size_t i = 0; i < 3u; i++) {
        if (!rdna4mj_parse_u32(cur, &out[i])) return 0;
        if (i < 2u) {
            if (!rdna4mj_consume(cur, ',')) return 0;
        }
    }
    return rdna4mj_consume(cur, ']');
}

static inline int rdna4mj_skip_value(rdna4mj_cur_t *cur, unsigned depth) {
    if (depth > RDNA4MESH_JSON_MAX_DEPTH) return 0;
    int c = rdna4mj_peek(cur);
    if (c < 0) return 0;
    if (c == '"') return rdna4mj_parse_string(cur, NULL, 0u, NULL);
    if (c == '{') {
        cur->pos++;
        if (rdna4mj_consume(cur, '}')) return 1;
        for (;;) {
            if (!rdna4mj_parse_string(cur, NULL, 0u, NULL)) return 0;
            if (!rdna4mj_consume(cur, ':')) return 0;
            if (!rdna4mj_skip_value(cur, depth + 1u)) return 0;
            if (rdna4mj_consume(cur, '}')) return 1;
            if (!rdna4mj_consume(cur, ',')) return 0;
        }
    }
    if (c == '[') {
        cur->pos++;
        if (rdna4mj_consume(cur, ']')) return 1;
        for (;;) {
            if (!rdna4mj_skip_value(cur, depth + 1u)) return 0;
            if (rdna4mj_consume(cur, ']')) return 1;
            if (!rdna4mj_consume(cur, ',')) return 0;
        }
    }
    if (c == 't') return rdna4mj_match_lit(cur, "true");
    if (c == 'f') return rdna4mj_match_lit(cur, "false");
    if (c == 'n') return rdna4mj_match_lit(cur, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (cur->s[cur->pos] == '-') cur->pos++;
        if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
        if (cur->s[cur->pos] == '0') {
            cur->pos++;
            if (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') return 0;
        } else {
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        if (cur->pos < cur->len && cur->s[cur->pos] == '.') {
            cur->pos++;
            if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        if (cur->pos < cur->len && (cur->s[cur->pos] == 'e' || cur->s[cur->pos] == 'E')) {
            cur->pos++;
            if (cur->pos < cur->len && (cur->s[cur->pos] == '+' || cur->s[cur->pos] == '-')) cur->pos++;
            if (cur->pos >= cur->len || cur->s[cur->pos] < '0' || cur->s[cur->pos] > '9') return 0;
            while (cur->pos < cur->len && cur->s[cur->pos] >= '0' && cur->s[cur->pos] <= '9') cur->pos++;
        }
        return 1;
    }
    return 0;
}

static inline int rdna4mj_parse_varying_item(rdna4mj_cur_t *cur, rdna4mesh_varying_t *out,
                                             int default_per_prim) {
    memset(out, 0, sizeof *out);
    out->per_primitive = default_per_prim;
    if (!rdna4mj_consume(cur, '{')) return 0;
    int got_loc = 0;
    if (!rdna4mj_consume(cur, '}')) {
        char key[64];
        for (;;) {
            if (!rdna4mj_parse_string(cur, key, sizeof key, NULL) || !rdna4mj_consume(cur, ':'))
                return 0;
            if (strcmp(key, "location") == 0) {
                if (!rdna4mj_parse_u32(cur, &out->location)) return 0;
                got_loc = 1;
            } else if (strcmp(key, "per_primitive") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->per_primitive)) return 0;
            } else {
                if (!rdna4mj_skip_value(cur, 3u)) return 0;
            }
            if (rdna4mj_consume(cur, '}')) break;
            if (!rdna4mj_consume(cur, ',')) return 0;
        }
    }
    return got_loc;
}

static inline rdna4mesh_status_t rdna4mj_parse_object_section(rdna4mj_cur_t *cur,
                                                              rdna4mesh_object_refl_t *out) {
    memset(out, 0, sizeof *out);
    if (!rdna4mj_consume(cur, '{')) return RDNA4MESH_ERR_JSON;
    int seen_has_payload = 0;
    if (!rdna4mj_consume(cur, '}')) {
        char key[64];
        for (;;) {
            if (!rdna4mj_parse_string(cur, key, sizeof key, NULL) || !rdna4mj_consume(cur, ':'))
                return RDNA4MESH_ERR_JSON;
            if (strcmp(key, "has_payload") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->has_payload)) return RDNA4MESH_ERR_JSON;
                seen_has_payload = 1;
            } else if (strcmp(key, "payload_size") == 0) {
                if (!rdna4mj_parse_opt_u32(cur, &out->has_payload_size, &out->payload_size))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "payload_stride") == 0) {
                if (!rdna4mj_parse_opt_u32(cur, &out->has_payload_stride, &out->payload_stride))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "emulated_payload") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->emulated_payload)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "object_grid_dims") == 0) {
                if (rdna4mj_peek(cur) == 'n') {
                    if (!rdna4mj_match_lit(cur, "null")) return RDNA4MESH_ERR_JSON;
                    out->has_object_grid_dims = 0;
                } else {
                    if (!rdna4mj_parse_u32_3(cur, out->object_grid_dims)) return RDNA4MESH_ERR_JSON;
                    out->has_object_grid_dims = 1;
                }
            } else if (strcmp(key, "max_total_threadgroups_per_mesh_grid") == 0) {
                if (!rdna4mj_parse_opt_u32(cur, &out->has_max_mesh_grid,
                                           &out->max_total_threadgroups_per_mesh_grid))
                    return RDNA4MESH_ERR_JSON;
            } else {
                if (!rdna4mj_skip_value(cur, 2u)) return RDNA4MESH_ERR_JSON;
            }
            if (rdna4mj_consume(cur, '}')) break;
            if (!rdna4mj_consume(cur, ',')) return RDNA4MESH_ERR_JSON;
        }
    }
    return seen_has_payload ? RDNA4MESH_OK : RDNA4MESH_ERR_JSON;
}

static inline rdna4mesh_status_t rdna4mj_parse_mesh_section(rdna4mj_cur_t *cur,
                                                            rdna4mesh_mesh_refl_t *out) {
    memset(out, 0, sizeof *out);
    if (!rdna4mj_consume(cur, '{')) return RDNA4MESH_ERR_JSON;
    int seen_topo = 0, seen_mv = 0, seen_mp = 0, seen_hp = 0;
    if (!rdna4mj_consume(cur, '}')) {
        char key[64];
        for (;;) {
            if (!rdna4mj_parse_string(cur, key, sizeof key, NULL) || !rdna4mj_consume(cur, ':'))
                return RDNA4MESH_ERR_JSON;
            if (strcmp(key, "topology") == 0) {
                char topo[32];
                if (!rdna4mj_parse_string(cur, topo, sizeof topo, NULL)) return RDNA4MESH_ERR_JSON;
                if (strcmp(topo, "Point") == 0) out->topology = RDNA4MESH_TOPO_POINT;
                else if (strcmp(topo, "Line") == 0) out->topology = RDNA4MESH_TOPO_LINE;
                else if (strcmp(topo, "Triangle") == 0) out->topology = RDNA4MESH_TOPO_TRIANGLE;
                else return RDNA4MESH_ERR_TOPOLOGY;
                seen_topo = 1;
            } else if (strcmp(key, "max_vertices") == 0) {
                if (!rdna4mj_parse_u32(cur, &out->max_vertices)) return RDNA4MESH_ERR_JSON;
                seen_mv = 1;
            } else if (strcmp(key, "max_primitives") == 0) {
                if (!rdna4mj_parse_u32(cur, &out->max_primitives)) return RDNA4MESH_ERR_JSON;
                seen_mp = 1;
            } else if (strcmp(key, "has_payload") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->has_payload)) return RDNA4MESH_ERR_JSON;
                seen_hp = 1;
            } else if (strcmp(key, "payload_size") == 0) {
                if (!rdna4mj_parse_opt_u32(cur, &out->has_payload_size, &out->payload_size))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "payload_stride") == 0) {
                if (!rdna4mj_parse_opt_u32(cur, &out->has_payload_stride, &out->payload_stride))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "emulated_payload") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->emulated_payload)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "writes_position") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->writes_position)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "writes_cull_primitive") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->writes_cull_primitive)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "writes_layer") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->writes_layer)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "writes_viewport_index") == 0) {
                if (!rdna4mj_parse_bool(cur, &out->writes_viewport_index)) return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "vertex_outputs") == 0 || strcmp(key, "primitive_outputs") == 0) {
                int is_prim = (key[0] == 'p');
                if (!rdna4mj_consume(cur, '[')) return RDNA4MESH_ERR_JSON;
                if (!rdna4mj_consume(cur, ']')) {
                    for (;;) {
                        rdna4mesh_varying_t item;
                        if (!rdna4mj_parse_varying_item(cur, &item, is_prim))
                            return RDNA4MESH_ERR_JSON;
                        if (is_prim) {
                            if (out->primitive_output_count >= RDNA4MESH_MAX_STAGE_OUTPUTS)
                                return RDNA4MESH_ERR_BOUNDS;
                            out->primitive_outputs[out->primitive_output_count++] = item;
                            if (item.location < 64u)
                                out->primitive_out_mask |= (UINT64_C(1) << item.location);
                        } else {
                            if (out->vertex_output_count >= RDNA4MESH_MAX_STAGE_OUTPUTS)
                                return RDNA4MESH_ERR_BOUNDS;
                            out->vertex_outputs[out->vertex_output_count++] = item;
                            if (item.location < 64u)
                                out->vertex_out_mask |= (UINT64_C(1) << item.location);
                        }
                        if (rdna4mj_consume(cur, ']')) break;
                        if (!rdna4mj_consume(cur, ',')) return RDNA4MESH_ERR_JSON;
                    }
                }
            } else {
                if (!rdna4mj_skip_value(cur, 2u)) return RDNA4MESH_ERR_JSON;
            }
            if (rdna4mj_consume(cur, '}')) break;
            if (!rdna4mj_consume(cur, ',')) return RDNA4MESH_ERR_JSON;
        }
    }
    return (seen_topo && seen_mv && seen_mp && seen_hp) ? RDNA4MESH_OK : RDNA4MESH_ERR_JSON;
}

static inline rdna4mesh_status_t rdna4mesh_parse_reflection_json(const char *json, size_t len,
                                                                 rdna4mesh_refl_t *out) {
    if (!json || len == 0u || !out) return RDNA4MESH_ERR_INVALID_ARG;
    rdna4mesh_refl_t tmp;
    memset(&tmp, 0, sizeof tmp);

    rdna4mj_cur_t cur = { json, len, 0u };
    if (!rdna4mj_consume(&cur, '{')) return RDNA4MESH_ERR_JSON;
    if (!rdna4mj_consume(&cur, '}')) {
        char key[128];
        for (;;) {
            if (!rdna4mj_parse_string(&cur, key, sizeof key, NULL) || !rdna4mj_consume(&cur, ':'))
                return RDNA4MESH_ERR_JSON;
            if (strcmp(key, "stage") == 0) {
                char st[32];
                if (!rdna4mj_parse_string(&cur, st, sizeof st, NULL)) return RDNA4MESH_ERR_JSON;
                if (strcmp(st, "Object") == 0) tmp.stage = RDNA4MESH_STAGE_OBJECT;
                else if (strcmp(st, "Mesh") == 0) tmp.stage = RDNA4MESH_STAGE_MESH;
                else if (strcmp(st, "Fragment") == 0) tmp.stage = RDNA4MESH_STAGE_FRAGMENT;
                else if (strcmp(st, "Vertex") == 0) tmp.stage = RDNA4MESH_STAGE_VERTEX;
                else if (strcmp(st, "Kernel") == 0) tmp.stage = RDNA4MESH_STAGE_KERNEL;
                else tmp.stage = RDNA4MESH_STAGE_UNKNOWN;
            } else if (strcmp(key, "entry_point") == 0) {
                if (!rdna4mj_parse_string(&cur, tmp.entry_point, sizeof tmp.entry_point, NULL))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "local_size") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_parse_u32_3(&cur, tmp.local_size)) return RDNA4MESH_ERR_JSON;
                    tmp.has_local_size = 1;
                }
            } else if (strcmp(key, "max_work_group_size") == 0) {
                if (!rdna4mj_parse_opt_u32(&cur, &tmp.has_max_work_group_size,
                                           &tmp.max_work_group_size))
                    return RDNA4MESH_ERR_JSON;
            } else if (strcmp(key, "object") == 0) {
                if (tmp.has_object) return RDNA4MESH_ERR_JSON;
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    rdna4mesh_status_t st = rdna4mj_parse_object_section(&cur, &tmp.obj);
                    if (st != RDNA4MESH_OK) return st;
                    tmp.has_object = 1;
                }
            } else if (strcmp(key, "mesh") == 0) {
                if (tmp.has_mesh) return RDNA4MESH_ERR_JSON;
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    rdna4mesh_status_t st = rdna4mj_parse_mesh_section(&cur, &tmp.mesh);
                    if (st != RDNA4MESH_OK) return st;
                    tmp.has_mesh = 1;
                }
            } else if (strcmp(key, "varyings") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_consume(&cur, '[')) return RDNA4MESH_ERR_JSON;
                    if (!rdna4mj_consume(&cur, ']')) {
                        for (;;) {
                            if (tmp.varying_count >= RDNA4MESH_MAX_VARYINGS)
                                return RDNA4MESH_ERR_BOUNDS;
                            if (!rdna4mj_parse_varying_item(&cur,
                                                            &tmp.varyings[tmp.varying_count], 0))
                                return RDNA4MESH_ERR_JSON;
                            tmp.varying_count++;
                            if (rdna4mj_consume(&cur, ']')) break;
                            if (!rdna4mj_consume(&cur, ',')) return RDNA4MESH_ERR_JSON;
                        }
                    }
                }
            } else if (strcmp(key, "visible_function_tables") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_skip_value(&cur, 1u)) return RDNA4MESH_ERR_JSON;
                    tmp.has_vft = 1;
                }
            } else if (strcmp(key, "intersection_function_tables") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_skip_value(&cur, 1u)) return RDNA4MESH_ERR_JSON;
                    tmp.has_raytracing = 1;
                }
            } else if (strcmp(key, "acceleration_structures") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_consume(&cur, '[')) return RDNA4MESH_ERR_JSON;
                    if (!rdna4mj_consume(&cur, ']')) {
                        tmp.has_raytracing = 1;
                        for (;;) {
                            if (!rdna4mj_skip_value(&cur, 2u)) return RDNA4MESH_ERR_JSON;
                            if (rdna4mj_consume(&cur, ']')) break;
                            if (!rdna4mj_consume(&cur, ',')) return RDNA4MESH_ERR_JSON;
                        }
                    }
                }
            } else if (strcmp(key, "argument_resource_tables") == 0) {
                if (rdna4mj_peek(&cur) == 'n') {
                    if (!rdna4mj_match_lit(&cur, "null")) return RDNA4MESH_ERR_JSON;
                } else {
                    if (!rdna4mj_skip_value(&cur, 1u)) return RDNA4MESH_ERR_JSON;
                    tmp.has_argument_tables = 1;
                }
            } else if (strcmp(key, "bindings") == 0) {
                if (!rdna4mj_consume(&cur, '[')) return RDNA4MESH_ERR_JSON;
                if (!rdna4mj_consume(&cur, ']')) {
                    for (;;) {
                        if (!rdna4mj_consume(&cur, '{')) return RDNA4MESH_ERR_JSON;
                        if (!rdna4mj_consume(&cur, '}')) {
                            char bkey[64];
                            for (;;) {
                                if (!rdna4mj_parse_string(&cur, bkey, sizeof bkey, NULL) ||
                                    !rdna4mj_consume(&cur, ':'))
                                    return RDNA4MESH_ERR_JSON;
                                if (strcmp(bkey, "kind") == 0 && rdna4mj_peek(&cur) == '"') {
                                    char kval[64];
                                    if (!rdna4mj_parse_string(&cur, kval, sizeof kval, NULL))
                                        return RDNA4MESH_ERR_JSON;
                                    if (strcmp(kval, "SamplerCoordinateFlags") == 0 ||
                                        strcmp(kval, "StageResourceMetadata") == 0)
                                        tmp.has_sampler_coord_flags = 1;
                                } else {
                                    if (!rdna4mj_skip_value(&cur, 3u)) return RDNA4MESH_ERR_JSON;
                                }
                                if (rdna4mj_consume(&cur, '}')) break;
                                if (!rdna4mj_consume(&cur, ',')) return RDNA4MESH_ERR_JSON;
                            }
                        }
                        if (rdna4mj_consume(&cur, ']')) break;
                        if (!rdna4mj_consume(&cur, ',')) return RDNA4MESH_ERR_JSON;
                    }
                }
            } else {
                if (!rdna4mj_skip_value(&cur, 1u)) return RDNA4MESH_ERR_JSON;
            }
            if (rdna4mj_consume(&cur, '}')) break;
            if (!rdna4mj_consume(&cur, ',')) return RDNA4MESH_ERR_JSON;
        }
    }
    rdna4mj_ws(&cur);
    if (cur.pos != cur.len) return RDNA4MESH_ERR_JSON;
    *out = tmp;
    return RDNA4MESH_OK;
}

// =================================================================================================
// 6. Lightweight SPIR-V execution-model / capability / mesh execution-mode inspector
// =================================================================================================

typedef struct {
    int has_ep;
    uint32_t ep_model;
    uint32_t ep_id;
    int has_local_size;
    uint32_t local_size[3];
    uint32_t output_vertices;
    uint32_t output_primitives;
    rdna4mesh_topology_t topology;
    int has_mesh_shading_cap;
    int has_draw_params_cap;
    int has_raytracing_cap;
} rdna4mesh_spv_info_t;

static inline rdna4mesh_status_t rdna4mesh_inspect_spv(const uint32_t *words, size_t word_count,
                                                       rdna4mesh_spv_info_t *out) {
    if (!words || word_count < 5u || !out) return RDNA4MESH_ERR_SPIRV;
    memset(out, 0, sizeof *out);
    if (words[0] != RDNA4MESH_SPV_MAGIC) return RDNA4MESH_ERR_SPIRV;

    size_t p = 5u;
    while (p < word_count) {
        uint32_t hdr = words[p];
        uint32_t op = hdr & 0xFFFFu;
        uint32_t wc = hdr >> 16;
        if (wc == 0u || (size_t)wc > word_count - p) return RDNA4MESH_ERR_SPIRV;
        const uint32_t *a = words + p + 1u;
        if (op == 17u && wc == 2u) { // OpCapability
            if (a[0] == RDNA4MESH_SPV_CAP_MESH_SHADING) out->has_mesh_shading_cap = 1;
            else if (a[0] == RDNA4MESH_SPV_CAP_DRAW_PARAMS) out->has_draw_params_cap = 1;
            else if (a[0] == RDNA4MESH_SPV_CAP_RAY_QUERY_KHR ||
                     a[0] == RDNA4MESH_SPV_CAP_RAY_TRACING_KHR ||
                     a[0] == RDNA4MESH_SPV_CAP_RAY_CULL_KHR)
                out->has_raytracing_cap = 1;
        } else if (op == 15u && wc >= 4u) { // OpEntryPoint
            if (!out->has_ep) {
                out->has_ep = 1;
                out->ep_model = a[0];
                out->ep_id = a[1];
            }
        } else if (op == 16u && wc >= 3u) { // OpExecutionMode
            uint32_t mode = a[1];
            if (mode == RDNA4MESH_SPV_MODE_LOCAL_SIZE && wc >= 6u) {
                out->has_local_size = 1;
                out->local_size[0] = a[2];
                out->local_size[1] = a[3];
                out->local_size[2] = a[4];
            } else if (mode == RDNA4MESH_SPV_MODE_OUT_VERTICES && wc >= 4u) {
                out->output_vertices = a[2];
            } else if (mode == RDNA4MESH_SPV_MODE_OUT_PRIMS_EXT && wc >= 4u) {
                out->output_primitives = a[2];
            } else if (mode == RDNA4MESH_SPV_MODE_OUT_POINTS) {
                out->topology = RDNA4MESH_TOPO_POINT;
            } else if (mode == RDNA4MESH_SPV_MODE_OUT_LINES_EXT) {
                out->topology = RDNA4MESH_TOPO_LINE;
            } else if (mode == RDNA4MESH_SPV_MODE_OUT_TRIS_EXT) {
                out->topology = RDNA4MESH_TOPO_TRIANGLE;
            }
        }
        p += wc;
    }
    return out->has_ep ? RDNA4MESH_OK : RDNA4MESH_ERR_SPIRV;
}

// =================================================================================================
// 7. Cross-stage linkage validation (Object <-> Mesh <-> Fragment + payload + Footnote 10)
// =================================================================================================

typedef struct {
    uint32_t effective_payload_bytes;
    uint32_t effective_payload_stride;
    uint32_t effective_max_mesh_grid;
    rdna4mesh_topology_t topology;
    uint32_t max_vertices;
    uint32_t max_primitives;
} rdna4mesh_linked_info_t;

static inline rdna4mesh_status_t rdna4mesh_validate_stage_linkage(
    rdna4mesh_exec_mode_t mode,
    const rdna4dc_caps *caps,
    const rdna4mesh_desc_t *desc,
    const rdna4mesh_refl_t *obj_refl,
    const rdna4mesh_spv_info_t *obj_spv,
    const rdna4mesh_refl_t *mesh_refl,
    const rdna4mesh_spv_info_t *mesh_spv,
    const rdna4mesh_refl_t *frag_refl,
    const rdna4mesh_spv_info_t *frag_spv,
    rdna4mesh_linked_info_t *out,
    const char **why_out)
{
    if (why_out) *why_out = NULL;
    if (out) memset(out, 0, sizeof *out);
    if (!caps || !desc || !mesh_spv) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_INVALID_ARG);
        return RDNA4MESH_ERR_INVALID_ARG;
    }

    // Re-check Footnote 10 including reflection & SPIR-V capabilities across all active stages.
    uint32_t fn10 = desc->footnote10_flags;
    if ((obj_refl && obj_refl->has_vft) ||
        (mesh_refl && mesh_refl->has_vft) ||
        (frag_refl && frag_refl->has_vft))
        fn10 |= RDNA4MESH_FN10_VFT_METADATA;
    if ((obj_refl && obj_refl->has_raytracing) ||
        (mesh_refl && mesh_refl->has_raytracing) ||
        (frag_refl && frag_refl->has_raytracing) ||
        (obj_spv && obj_spv->has_raytracing_cap) ||
        mesh_spv->has_raytracing_cap ||
        (frag_spv && frag_spv->has_raytracing_cap))
        fn10 |= RDNA4MESH_FN10_RAYTRACING;
    rdna4mesh_status_t fn10_st = rdna4mesh_check_footnote10(fn10, why_out);
    if (fn10_st != RDNA4MESH_OK) return fn10_st;

    // Validate SPIR-V execution models match selected execution mode.
    if (mesh_spv->ep_model != RDNA4MESH_SPV_EM_MESH_EXT) {
        if (why_out) *why_out = "meshFunction SPIR-V entry point is not MeshEXT";
        return RDNA4MESH_ERR_SPIRV;
    }
    if (desc->has_object_function) {
        if (!obj_spv) {
            if (why_out) *why_out = "missing objectFunction SPIR-V info";
            return RDNA4MESH_ERR_SPIRV;
        }
        uint32_t want_obj_em = (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
            ? RDNA4MESH_SPV_EM_GLCOMPUTE : RDNA4MESH_SPV_EM_TASK_EXT;
        if (obj_spv->ep_model != want_obj_em) {
            if (why_out) *why_out = (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
                ? "emulated objectFunction SPIR-V entry point is not GLCompute"
                : "native objectFunction SPIR-V entry point is not TaskEXT";
            return RDNA4MESH_ERR_SPIRV;
        }
    }
    if (desc->has_fragment_function && frag_spv && frag_spv->ep_model != RDNA4MESH_SPV_EM_FRAGMENT) {
        if (why_out) *why_out = "fragmentFunction SPIR-V entry point is not Fragment";
        return RDNA4MESH_ERR_SPIRV;
    }

    // Mesh output topology & vertex/primitive limits.
    rdna4mesh_topology_t topo = mesh_spv->topology;
    if (mesh_refl && mesh_refl->has_mesh) {
        if (topo != RDNA4MESH_TOPO_INVALID && mesh_refl->mesh.topology != topo) {
            if (why_out) *why_out = "mesh reflection topology disagrees with SPIR-V execution mode";
            return RDNA4MESH_ERR_TOPOLOGY;
        }
        if (topo == RDNA4MESH_TOPO_INVALID) topo = mesh_refl->mesh.topology;
    }
    if (topo == RDNA4MESH_TOPO_INVALID) {
        if (why_out) *why_out = "mesh shader declares no output topology (Point/Line/Triangle)";
        return RDNA4MESH_ERR_TOPOLOGY;
    }
    uint32_t max_v = mesh_spv->output_vertices ? mesh_spv->output_vertices
        : ((mesh_refl && mesh_refl->has_mesh) ? mesh_refl->mesh.max_vertices : 0u);
    uint32_t max_p = mesh_spv->output_primitives ? mesh_spv->output_primitives
        : ((mesh_refl && mesh_refl->has_mesh) ? mesh_refl->mesh.max_primitives : 0u);
    uint32_t lim_v = caps->metal3.mesh_max_output_vertices;
    if (lim_v > RDNA4MESH_MAX_VERTICES) lim_v = RDNA4MESH_MAX_VERTICES;
    uint32_t lim_p = caps->metal3.mesh_max_output_primitives;
    if (lim_p > RDNA4MESH_MAX_PRIMITIVES) lim_p = RDNA4MESH_MAX_PRIMITIVES;
    if ((max_v == 0u && max_p == 0u) || max_v > lim_v || max_p > lim_p) {
        if (why_out) *why_out = "mesh output vertices/primitives outside hardware limits";
        return RDNA4MESH_ERR_OUTPUT_LIMIT;
    }

    // Payload linkage between descriptor, Object stage, and Mesh stage.
    int mesh_uses_payload = (mesh_refl && mesh_refl->has_mesh && mesh_refl->mesh.has_payload);
    if (mesh_uses_payload && !desc->has_object_function) {
        if (why_out) *why_out = "meshFunction reads [[payload]] but descriptor has no objectFunction";
        return RDNA4MESH_ERR_STAGE_LINKAGE;
    }
    uint32_t obj_psize = (obj_refl && obj_refl->has_object && obj_refl->obj.has_payload_size)
        ? obj_refl->obj.payload_size : 0u;
    uint32_t mesh_psize = (mesh_refl && mesh_refl->has_mesh && mesh_refl->mesh.has_payload_size)
        ? mesh_refl->mesh.payload_size : 0u;
    uint32_t payload_limit = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH &&
                              caps->metal3.mesh_max_payload_bytes < RDNA4MESH_MAX_PAYLOAD_BYTES)
        ? caps->metal3.mesh_max_payload_bytes : RDNA4MESH_MAX_PAYLOAD_BYTES;
    if (obj_psize > payload_limit || mesh_psize > payload_limit) {
        if (why_out) *why_out = "object or mesh declared payload_size exceeds 16,384 bytes";
        return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    }
    if (desc->has_object_function && obj_psize > 0u && mesh_psize > obj_psize) {
        if (why_out) *why_out = "mesh payload_size exceeds object payload_size";
        return RDNA4MESH_ERR_STAGE_LINKAGE;
    }
    uint32_t shader_psize = obj_psize > mesh_psize ? obj_psize : mesh_psize;
    if (desc->payload_memory_length > 0u && shader_psize > desc->payload_memory_length) {
        if (why_out) *why_out = "descriptor payloadMemoryLength is smaller than shader payload_size";
        return RDNA4MESH_ERR_PAYLOAD_LIMIT;
    }
    uint32_t eff_psize = desc->payload_memory_length > 0u
        ? (uint32_t)desc->payload_memory_length : shader_psize;
    uint32_t eff_stride = 0u;
    if (desc->has_object_function) {
        rdna4mesh_status_t ps = rdna4mesh_payload_stride(eff_psize, &eff_stride);
        if (ps != RDNA4MESH_OK) {
            if (why_out) *why_out = rdna4mesh_status_str(ps);
            return ps;
        }
        // Respect a larger explicit stride reported by 0078 reflection (still bounded <= 16 KiB).
        if (obj_refl && obj_refl->has_object && obj_refl->obj.has_payload_stride) {
            if (obj_refl->obj.payload_stride > RDNA4MESH_MAX_PAYLOAD_BYTES ||
                (obj_refl->obj.payload_stride & (RDNA4MESH_PAYLOAD_ALIGN - 1u)) != 0u) {
                if (why_out) *why_out = "object payload_stride invalid or > 16,384 bytes";
                return RDNA4MESH_ERR_PAYLOAD_LIMIT;
            }
            if (obj_refl->obj.payload_stride > eff_stride) eff_stride = obj_refl->obj.payload_stride;
        }
        if (mesh_refl && mesh_refl->has_mesh && mesh_refl->mesh.has_payload_stride) {
            if (mesh_refl->mesh.payload_stride > RDNA4MESH_MAX_PAYLOAD_BYTES ||
                (mesh_refl->mesh.payload_stride & (RDNA4MESH_PAYLOAD_ALIGN - 1u)) != 0u) {
                if (why_out) *why_out = "mesh payload_stride invalid or > 16,384 bytes";
                return RDNA4MESH_ERR_PAYLOAD_LIMIT;
            }
            if (mesh_refl->mesh.payload_stride > eff_stride) eff_stride = mesh_refl->mesh.payload_stride;
        }
    }

    // Effective max mesh grid cap (descriptor + object shader attribute + device limit).
    uint32_t eff_grid = caps->metal3.mesh_max_workgroup_total_count;
    if (desc->max_total_threadgroups_per_mesh_grid > 0u &&
        desc->max_total_threadgroups_per_mesh_grid < eff_grid)
        eff_grid = (uint32_t)desc->max_total_threadgroups_per_mesh_grid;
    if (obj_refl && obj_refl->has_object && obj_refl->obj.has_max_mesh_grid) {
        if (obj_refl->obj.max_total_threadgroups_per_mesh_grid == 0u ||
            obj_refl->obj.max_total_threadgroups_per_mesh_grid >
                caps->metal3.mesh_max_workgroup_total_count) {
            if (why_out) *why_out = "object max_total_threadgroups_per_mesh_grid exceeds device limit";
            return RDNA4MESH_ERR_GRID_LIMIT;
        }
        if (obj_refl->obj.max_total_threadgroups_per_mesh_grid < eff_grid)
            eff_grid = obj_refl->obj.max_total_threadgroups_per_mesh_grid;
    }

    // Fragment PerPrimitiveEXT vs per-vertex varying linkage check.
    if (desc->has_fragment_function && frag_refl && frag_refl->varying_count > 0u &&
        mesh_refl && mesh_refl->has_mesh) {
        for (uint32_t i = 0; i < frag_refl->varying_count; i++) {
            uint32_t loc = frag_refl->varyings[i].location;
            if (loc >= 64u) continue;
            uint64_t bit = UINT64_C(1) << loc;
            if (frag_refl->varyings[i].per_primitive) {
                if (!(mesh_refl->mesh.primitive_out_mask & bit)) {
                    if (why_out) *why_out = "fragment PerPrimitiveEXT input location is not emitted as a mesh primitive output";
                    return RDNA4MESH_ERR_STAGE_LINKAGE;
                }
            } else {
                // Reject if fragment expects a per-vertex input at a location the mesh emits ONLY as per-primitive.
                if ((mesh_refl->mesh.primitive_out_mask & bit) &&
                    !(mesh_refl->mesh.vertex_out_mask & bit)) {
                    if (why_out) *why_out = "fragment per-vertex input location conflicts with mesh PerPrimitiveEXT output";
                    return RDNA4MESH_ERR_STAGE_LINKAGE;
                }
            }
        }
    }

    if (out) {
        out->effective_payload_bytes = eff_psize;
        out->effective_payload_stride = eff_stride;
        out->effective_max_mesh_grid = eff_grid;
        out->topology = topo;
        out->max_vertices = max_v;
        out->max_primitives = max_p;
    }
    return RDNA4MESH_OK;
}

// =================================================================================================
// 8. Draw-time validation: threadgroups, drawMeshThreads, indirect buffers, threadgroup memory
// =================================================================================================

static inline rdna4mesh_status_t rdna4mesh_validate_tg_size(
    uint64_t tx, uint64_t ty, uint64_t tz,
    const uint32_t hw_max_axis[3],
    uint32_t max_invocations,
    uint32_t shader_max_wg_size,
    uint32_t exec_width,
    int require_mult_exec_width,
    const char **why_out)
{
    if (why_out) *why_out = NULL;
    if (!tx || !ty || !tz) {
        if (why_out) *why_out = "threadgroup dimension must be non-zero";
        return RDNA4MESH_ERR_WORKGROUP_LIMIT;
    }
    uint32_t cap = max_invocations;
    if (cap > RDNA4MESH_MAX_INVOCATIONS) cap = RDNA4MESH_MAX_INVOCATIONS;
    if (shader_max_wg_size > 0u && shader_max_wg_size < cap) cap = shader_max_wg_size;
    if (cap == 0u) {
        if (why_out) *why_out = "effective threadgroup invocation limit is zero";
        return RDNA4MESH_ERR_WORKGROUP_LIMIT;
    }
    const uint64_t dims[3] = { tx, ty, tz };
    uint64_t total = 1u;
    for (size_t i = 0; i < 3u; i++) {
        uint32_t axis_lim = (hw_max_axis && hw_max_axis[i]) ? hw_max_axis[i] : cap;
        if (dims[i] > axis_lim || dims[i] > (uint64_t)cap / total) {
            if (why_out) *why_out = "threadsPerThreadgroup exceeds device or pipeline limit";
            return RDNA4MESH_ERR_WORKGROUP_LIMIT;
        }
        total *= dims[i];
    }
    if (require_mult_exec_width) {
        uint32_t ew = exec_width ? exec_width : 32u;
        if ((total % ew) != 0u) {
            if (why_out) *why_out = "threadsPerThreadgroup is not a multiple of threadExecutionWidth";
            return RDNA4MESH_ERR_WORKGROUP_LIMIT;
        }
    }
    return RDNA4MESH_OK;
}

static inline rdna4mesh_status_t rdna4mesh_validate_grid(
    uint64_t gx, uint64_t gy, uint64_t gz,
    const uint32_t max_count[3],
    uint32_t max_total,
    int *is_noop_out,
    uint64_t *total_out,
    const char **why_out)
{
    if (is_noop_out) *is_noop_out = 0;
    if (total_out) *total_out = 0u;
    if (why_out) *why_out = NULL;
    if (!gx || !gy || !gz) {
        if (is_noop_out) *is_noop_out = 1;
        return RDNA4MESH_OK;
    }
    if (!max_count || max_total == 0u) {
        if (why_out) *why_out = "grid dispatch limit is zero";
        return RDNA4MESH_ERR_GRID_LIMIT;
    }
    const uint64_t dims[3] = { gx, gy, gz };
    uint64_t total = 1u;
    for (size_t i = 0; i < 3u; i++) {
        if (dims[i] > max_count[i] || dims[i] > (uint64_t)max_total / total) {
            if (why_out) *why_out = "threadgroupsPerGrid exceeds device or pipeline grid limit";
            return RDNA4MESH_ERR_GRID_LIMIT;
        }
        total *= dims[i];
    }
    if (total_out) *total_out = total;
    return RDNA4MESH_OK;
}

static inline rdna4mesh_status_t rdna4mesh_threads_to_threadgroups(
    uint64_t threads_x, uint64_t threads_y, uint64_t threads_z,
    uint64_t tg_x, uint64_t tg_y, uint64_t tg_z,
    uint64_t out_groups[3],
    int *is_noop_out,
    const char **why_out)
{
    if (is_noop_out) *is_noop_out = 0;
    if (why_out) *why_out = NULL;
    if (!out_groups) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_INVALID_ARG);
        return RDNA4MESH_ERR_INVALID_ARG;
    }
    out_groups[0] = out_groups[1] = out_groups[2] = 0u;
    if (!tg_x || !tg_y || !tg_z) {
        if (why_out) *why_out = "drawMeshThreads: threadsPerThreadgroup dimension cannot be zero";
        return RDNA4MESH_ERR_WORKGROUP_LIMIT;
    }
    if (!threads_x || !threads_y || !threads_z) {
        if (is_noop_out) *is_noop_out = 1;
        return RDNA4MESH_OK;
    }
    const uint64_t th[3] = { threads_x, threads_y, threads_z };
    const uint64_t tg[3] = { tg_x, tg_y, tg_z };
    for (size_t i = 0; i < 3u; i++) {
        out_groups[i] = ((th[i] - 1u) / tg[i]) + 1u;
    }
    return RDNA4MESH_OK;
}

static inline rdna4mesh_status_t rdna4mesh_validate_indirect_buffer(
    uint64_t buf_length,
    uint64_t offset,
    uint64_t vk_base_offset,
    uint64_t *vk_offset_out,
    const char **why_out)
{
    if (vk_offset_out) *vk_offset_out = 0u;
    if (why_out) *why_out = NULL;
    if ((offset & 3u) != 0u) {
        if (why_out) *why_out = "indirectBufferOffset must be a multiple of 4 bytes";
        return RDNA4MESH_ERR_ALIGNMENT;
    }
    if (offset > buf_length || (buf_length - offset) < RDNA4MESH_INDIRECT_CMD_BYTES) {
        if (why_out) *why_out = "indirect mesh draw buffer range is smaller than 12 bytes";
        return RDNA4MESH_ERR_BOUNDS;
    }
    uint64_t vk_off = 0u;
    if (!rdna4mesh_add_u64(vk_base_offset, offset, &vk_off)) {
        if (why_out) *why_out = "indirect mesh draw Vulkan buffer offset overflows uint64";
        return RDNA4MESH_ERR_OVERFLOW;
    }
    if ((vk_off & 3u) != 0u) {
        if (why_out) *why_out = "indirect mesh draw Vulkan buffer offset must be 4-byte aligned";
        return RDNA4MESH_ERR_ALIGNMENT;
    }
    if (vk_offset_out) *vk_offset_out = vk_off;
    return RDNA4MESH_OK;
}

static inline rdna4mesh_status_t rdna4mesh_validate_tg_memory(
    const uint64_t current_lengths[RDNA4MESH_MAX_TG_MEM_INDICES],
    uint32_t index,
    uint64_t new_length,
    uint64_t device_limit_bytes,
    uint64_t *new_total_out,
    const char **why_out)
{
    if (new_total_out) *new_total_out = 0u;
    if (why_out) *why_out = NULL;
    if (!current_lengths || index >= RDNA4MESH_MAX_TG_MEM_INDICES) {
        if (why_out) *why_out = "object threadgroup memory index >= 31";
        return RDNA4MESH_ERR_TG_MEM;
    }
    if ((new_length & (RDNA4MESH_TG_MEM_ALIGN - 1u)) != 0u) {
        if (why_out) *why_out = "object threadgroup memory length must be 16-byte aligned";
        return RDNA4MESH_ERR_ALIGNMENT;
    }
    uint64_t lim = device_limit_bytes ? device_limit_bytes : RDNA4MESH_MAX_TG_MEM_BYTES;
    if (lim > RDNA4MESH_MAX_TG_MEM_BYTES) lim = RDNA4MESH_MAX_TG_MEM_BYTES;
    uint64_t sum = 0u;
    for (uint32_t i = 0; i < RDNA4MESH_MAX_TG_MEM_INDICES; i++) {
        uint64_t v = (i == index) ? new_length : current_lengths[i];
        if (!rdna4mesh_add_u64(sum, v, &sum)) {
            if (why_out) *why_out = "object threadgroup memory total length overflows";
            return RDNA4MESH_ERR_OVERFLOW;
        }
        if (sum > lim) {
            if (why_out) *why_out = "object threadgroup memory total length exceeds device limit";
            return RDNA4MESH_ERR_TG_MEM;
        }
    }
    if (new_total_out) *new_total_out = sum;
    return RDNA4MESH_OK;
}

// =================================================================================================
// 9. Emulated Object+Mesh scratch buffer sizing & overflow planner (0078 ABI)
// =================================================================================================

typedef struct {
    int is_noop;
    uint32_t total_object_groups;
    rdna4mesh_grid_params_t grid_params;
    uint64_t grid_params_offset;
    uint64_t indirect_offset;
    uint64_t indirect_bytes;
    uint64_t indirect_fill_bytes;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t total_scratch_bytes;
} rdna4mesh_emul_plan_t;

static inline rdna4mesh_status_t rdna4mesh_plan_emulated_layout(
    uint32_t total_groups,
    uint32_t gx, uint32_t gy, uint32_t gz,
    uint32_t payload_bytes,
    uint64_t max_buffer_bytes,
    uint64_t max_storage_range,
    uint32_t max_draw_indirect_count,
    uint64_t storage_align,
    rdna4mesh_emul_plan_t *out,
    const char **why_out)
{
    if (why_out) *why_out = NULL;
    if (!out) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_INVALID_ARG);
        return RDNA4MESH_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (total_groups == 0u) {
        out->is_noop = 1;
        return RDNA4MESH_OK;
    }
    if (max_draw_indirect_count == 0u || total_groups > max_draw_indirect_count) {
        if (why_out) *why_out = "emulated object group count exceeds maxDrawIndirectCount";
        return RDNA4MESH_ERR_GRID_LIMIT;
    }
    uint32_t stride = 0u;
    rdna4mesh_status_t pst = rdna4mesh_payload_stride(payload_bytes, &stride);
    if (pst != RDNA4MESH_OK) {
        if (why_out) *why_out = rdna4mesh_status_str(pst);
        return pst;
    }
    uint64_t align = storage_align ? storage_align : 16u;
    if (!rdna4mesh_is_pow2_u64(align)) {
        if (why_out) *why_out = "minStorageBufferOffsetAlignment must be a power of two";
        return RDNA4MESH_ERR_ALIGNMENT;
    }
    if (align < 16u) align = 16u;

    uint64_t ind_bytes = 0u, pay_bytes = 0u, ind_fill = 0u;
    if (!rdna4mesh_mul_u64((uint64_t)total_groups, RDNA4MESH_INDIRECT_CMD_BYTES, &ind_bytes) ||
        !rdna4mesh_mul_u64((uint64_t)total_groups, (uint64_t)stride, &pay_bytes) ||
        !rdna4mesh_align_up_u64(ind_bytes, 4u, &ind_fill)) {
        if (why_out) *why_out = "emulated object+mesh scratch buffer byte count overflows";
        return RDNA4MESH_ERR_OVERFLOW;
    }
    if (max_storage_range > 0u &&
        (ind_bytes > max_storage_range || pay_bytes > max_storage_range)) {
        if (why_out) *why_out = "emulated object+mesh payload or indirect buffer exceeds maxStorageBufferRange";
        return RDNA4MESH_ERR_BOUNDS;
    }

    uint64_t ind_off = 0u, pay_off = 0u, end_off = 0u, total_sz = 0u;
    if (!rdna4mesh_align_up_u64(RDNA4MESH_GRID_PARAMS_BYTES, align, &ind_off) ||
        !rdna4mesh_add_u64(ind_off, ind_fill, &end_off) ||
        !rdna4mesh_align_up_u64(end_off, align, &pay_off) ||
        !rdna4mesh_add_u64(pay_off, pay_bytes, &end_off) ||
        !rdna4mesh_align_up_u64(end_off, 256u, &total_sz)) {
        if (why_out) *why_out = "emulated object+mesh scratch buffer offset layout overflows";
        return RDNA4MESH_ERR_OVERFLOW;
    }
    if (max_buffer_bytes == 0u || total_sz > max_buffer_bytes) {
        if (why_out) *why_out = "emulated object+mesh scratch buffer exceeds device buffer length limit";
        return RDNA4MESH_ERR_BOUNDS;
    }

    out->is_noop = 0;
    out->total_object_groups = total_groups;
    out->grid_params = (rdna4mesh_grid_params_t){ gx, gy, gz, stride };
    out->grid_params_offset = 0u;
    out->indirect_offset = ind_off;
    out->indirect_bytes = ind_bytes;
    out->indirect_fill_bytes = ind_fill;
    out->payload_offset = pay_off;
    out->payload_bytes = pay_bytes;
    out->total_scratch_bytes = total_sz;
    return RDNA4MESH_OK;
}

static inline rdna4mesh_status_t rdna4mesh_plan_emulated_draw(
    uint64_t gx, uint64_t gy, uint64_t gz,
    uint32_t payload_bytes,
    uint64_t max_buffer_bytes,
    uint64_t max_storage_range,
    uint32_t max_draw_indirect_count,
    uint64_t storage_align,
    rdna4mesh_emul_plan_t *out,
    const char **why_out)
{
    if (why_out) *why_out = NULL;
    if (!out) {
        if (why_out) *why_out = rdna4mesh_status_str(RDNA4MESH_ERR_INVALID_ARG);
        return RDNA4MESH_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (!gx || !gy || !gz) {
        out->is_noop = 1;
        return RDNA4MESH_OK;
    }
    if (gx > UINT32_MAX || gy > UINT32_MAX || gz > UINT32_MAX) {
        if (why_out) *why_out = "object grid dimension exceeds uint32";
        return RDNA4MESH_ERR_OVERFLOW;
    }
    uint64_t xy = 0u, total = 0u;
    if (!rdna4mesh_mul_u64(gx, gy, &xy) || !rdna4mesh_mul_u64(xy, gz, &total) ||
        total > UINT32_MAX) {
        if (why_out) *why_out = "total object threadgroups overflows uint32";
        return RDNA4MESH_ERR_OVERFLOW;
    }
    return rdna4mesh_plan_emulated_layout((uint32_t)total,
                                          (uint32_t)gx, (uint32_t)gy, (uint32_t)gz,
                                          payload_bytes, max_buffer_bytes, max_storage_range,
                                          max_draw_indirect_count, storage_align, out, why_out);
}

// GPU-only indirect emulated Object+Mesh capacity planner (no CPU readback of GPU indirect args).
static inline rdna4mesh_status_t rdna4mesh_plan_emulated_indirect_draw(
    uint32_t payload_bytes,
    uint64_t max_buffer_bytes,
    uint64_t max_storage_range,
    uint32_t max_draw_indirect_count,
    uint64_t storage_align,
    uint32_t max_groups_cap,
    rdna4mesh_emul_plan_t *out,
    const char **why_out)
{
    if (why_out) *why_out = NULL;
    uint32_t stride = 0u;
    rdna4mesh_status_t pst = rdna4mesh_payload_stride(payload_bytes, &stride);
    if (pst != RDNA4MESH_OK) {
        if (why_out) *why_out = rdna4mesh_status_str(pst);
        return pst;
    }
    uint64_t budget = RDNA4MESH_EMUL_INDIRECT_BUDGET_BYTES;
    if (max_buffer_bytes > 0u && max_buffer_bytes < budget) budget = max_buffer_bytes;
    if (max_storage_range > 0u && max_storage_range < budget) budget = max_storage_range;
    uint64_t per_group = (uint64_t)stride + RDNA4MESH_INDIRECT_CMD_BYTES;
    uint64_t align_reserve = storage_align > 256u ? (storage_align * 4u) : 1024u;
    if (budget <= align_reserve + per_group) {
        if (why_out) *why_out = "device buffer budget too small for indirect emulated object+mesh scratch";
        return RDNA4MESH_ERR_BOUNDS;
    }
    uint64_t by_budget = (budget - align_reserve) / per_group;
    uint32_t cap = max_groups_cap ? max_groups_cap : RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS;
    if (max_draw_indirect_count > 0u && max_draw_indirect_count < cap)
        cap = max_draw_indirect_count;
    if (by_budget < cap) cap = (uint32_t)by_budget;
    if (cap == 0u) {
        if (why_out) *why_out = "indirect emulated object+mesh group capacity is zero";
        return RDNA4MESH_ERR_BOUNDS;
    }
    return rdna4mesh_plan_emulated_layout(cap, 0u, 0u, 0u, payload_bytes,
                                          max_buffer_bytes, max_storage_range,
                                          max_draw_indirect_count, storage_align, out, why_out);
}

#endif // RDNA4_MESH_H
