// garnet_rt_accel.m - GarnetMetal modular compilation unit: RT Vulkan dispatch, compute pipes, pools, and AS storage/handles.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_resources_internal.h"
#include "garnet_rt_internal.h"
#include "garnet_rt_spv.h"
#include "garnet_rt_spv_curves.h"
#include "garnet_rt_spv_motion.h"

// ---------------------------------------------------------------------------------------------------------------
// Hardware Ray Tracing Acceleration Structures (Task 7.1):
//   GarnetAccelerationStructure (MTLAccelerationStructure + MTLResource)
//   GarnetAccelerationStructureCommandEncoder (MTLAccelerationStructureCommandEncoder + MTLCommandEncoder)
//   Backed by VK_KHR_acceleration_structure + VK_KHR_deferred_host_operations.
//
// Wave 3 integration checklist (GarnetDevice.m, rdna4_command_buffer_impl.inc, rdna4_buffer_impl.inc):
//   1. GarnetDevice.m include order: place after `#include "rdna4_gl_drawable_impl.m"`
//      (and after `#include "rdna4_counters_impl.m"` so pass sampleBufferAttachments work):
//        #include "rdna4_rt_accel_impl.m"
//   2. GarnetDevice.m +load protocol registration & census (also auto-registered in +initialize):
//        { const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
//          rdna4_add_protocols([GarnetAccelerationStructure class], n, 2, "GarnetAccelerationStructure"); }
//        { const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" };
//          rdna4_add_protocols([GarnetAccelerationStructureCommandEncoder class], n, 2, "GarnetAccelerationStructureCommandEncoder"); }
//   3. GarnetDevice.m @implementation RDNA4CommandBuffer (Encoders):
//        - (id)accelerationStructureCommandEncoder {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoder");
//            return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:nil];
//        }
//        - (id)accelerationStructureCommandEncoderWithDescriptor:(id)d {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoderWithDescriptor:");
//            return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//   4. GarnetDevice.m @implementation GarnetDevice selectors:
//        - (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
//            return rdna4_rt_device_sizes(self, d);
//        }
//        - (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
//            return rdna4_rt_device_new_with_size(self, size);
//        }
//        - (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
//            return rdna4_rt_device_new_with_descriptor(self, d);
//        }
//      Keep supportsRaytracing / supportsRaytracingFromRender / supportsPrimitiveMotionBlur returning NO
//      until Task 7.3/8.2 verifies full compute/render ray-query execution (helpers:
//      rdna4_rt_device_supports_raytracing(), rdna4_rt_device_supports_primitive_motion_blur()).
//   5. rdna4_buffer_impl.inc (`rdna4_heap_buffer_create_info`):
//      OR `(RDNA4R.accelStruct ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0)`
//      into `.usage` so user vertex/index/AABB/transform buffers carry Vulkan AS build-input usage bits.
//   6. rdna4_command_buffer_impl.inc (`rdna4UseResource:usage:`):
//      Accept `GarnetAccelerationStructure` alongside `RDNA4IndirectCommandBuffer` and retain both
//      the acceleration structure and `[as rdna4CurrentHandle]` via `rdna4RetainEncoded:`.
// ---------------------------------------------------------------------------------------------------------------

_Static_assert(sizeof(MTLPackedFloat4x3) == sizeof(rdna4rt_packed_float4x3_t), "MTLPackedFloat4x3 size");
_Static_assert(sizeof(MTLAxisAlignedBoundingBox) == sizeof(rdna4rt_vk_aabb_t), "MTLAxisAlignedBoundingBox size");
_Static_assert(sizeof(MTLAccelerationStructureInstanceDescriptor) == sizeof(rdna4rt_mtl_instance_default_t), "MTLAccelerationStructureInstanceDescriptor size");
_Static_assert(sizeof(MTLAccelerationStructureUserIDInstanceDescriptor) == sizeof(rdna4rt_mtl_instance_userid_t), "MTLAccelerationStructureUserIDInstanceDescriptor size");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
_Static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == sizeof(rdna4rt_mtl_instance_indirect_t), "MTLIndirectAccelerationStructureInstanceDescriptor size");
#pragma clang diagnostic pop
_Static_assert((uint32_t)MTLAccelerationStructureUsageNone == RDNA4RT_USAGE_NONE, "UsageNone");
_Static_assert((uint32_t)MTLAccelerationStructureUsageRefit == RDNA4RT_USAGE_REFIT, "UsageRefit");
_Static_assert((uint32_t)MTLAccelerationStructureUsagePreferFastBuild == RDNA4RT_USAGE_PREFER_FAST_BUILD, "UsagePreferFastBuild");
_Static_assert((uint32_t)MTLAccelerationStructureUsageExtendedLimits == RDNA4RT_USAGE_EXTENDED_LIMITS, "UsageExtendedLimits");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionNone == RDNA4RT_INST_OPT_NONE, "InstOptNone");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionDisableTriangleCulling == RDNA4RT_INST_OPT_DISABLE_CULLING, "InstOptDisableCulling");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise == RDNA4RT_INST_OPT_CCW, "InstOptCCW");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionOpaque == RDNA4RT_INST_OPT_OPAQUE, "InstOptOpaque");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionNonOpaque == RDNA4RT_INST_OPT_NON_OPAQUE, "InstOptNonOpaque");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeDefault == RDNA4RT_INST_DESC_DEFAULT, "InstDescDefault");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeUserID == RDNA4RT_INST_DESC_USER_ID, "InstDescUserID");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeMotion == RDNA4RT_INST_DESC_MOTION, "InstDescMotion");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeIndirect == RDNA4RT_INST_DESC_INDIRECT, "InstDescIndirect");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeIndirectMotion == RDNA4RT_INST_DESC_INDIRECT_MOTION, "InstDescIndirectMotion");
#pragma clang diagnostic pop
_Static_assert((uint32_t)MTLMotionBorderModeClamp == RDNA4RT_MOTION_BORDER_CLAMP, "MotionBorderClamp");
_Static_assert((uint32_t)MTLMotionBorderModeVanish == RDNA4RT_MOTION_BORDER_VANISH, "MotionBorderVanish");

GARNET_INTERNAL RDNA4RTFnTable RDNA4RTFn;

GARNET_INTERNAL BOOL rdna4_rt_resolve_funcs(void) {
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.accelStruct || !RDNA4R.bufferAddress) return NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!vkGetDeviceProcAddr) return;
        RDNA4RTFn.getBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkGetAccelerationStructureBuildSizesKHR");
        RDNA4RTFn.createAS = (PFN_vkCreateAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCreateAccelerationStructureKHR");
        RDNA4RTFn.destroyAS = (PFN_vkDestroyAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkDestroyAccelerationStructureKHR");
        RDNA4RTFn.getASAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkGetAccelerationStructureDeviceAddressKHR");
        RDNA4RTFn.cmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdBuildAccelerationStructuresKHR");
        RDNA4RTFn.cmdCopyAS = (PFN_vkCmdCopyAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdCopyAccelerationStructureKHR");
        RDNA4RTFn.cmdWriteASProps = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
        RDNA4RTFn.cmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdCopyQueryPoolResults");
        if (!RDNA4RTFn.cmdCopyQueryPoolResults && rdna4_gipa && RDNA4R.inst) {
            RDNA4RTFn.cmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)
                rdna4_gipa(RDNA4R.inst, "vkCmdCopyQueryPoolResults");
        }
        RDNA4RTFn.cmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdUpdateBuffer");
        if (!RDNA4RTFn.cmdUpdateBuffer && rdna4_gipa && RDNA4R.inst) {
            RDNA4RTFn.cmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)
                rdna4_gipa(RDNA4R.inst, "vkCmdUpdateBuffer");
        }
        RDNA4RTFn.ready = RDNA4RTFn.getBuildSizes && RDNA4RTFn.createAS && RDNA4RTFn.destroyAS &&
                          RDNA4RTFn.getASAddress && RDNA4RTFn.cmdBuildAS && RDNA4RTFn.cmdCopyAS &&
                          RDNA4RTFn.cmdWriteASProps && RDNA4RTFn.cmdCopyQueryPoolResults &&
                          vkCreateQueryPool && vkDestroyQueryPool && vkCmdResetQueryPool &&
                          rdna4_vkGetBufferAddress;
        if (!RDNA4RTFn.ready)
            RDNA4LOG_WARN("rt-accel: one or more VK_KHR_acceleration_structure entry points are missing");
    });
    return RDNA4RTFn.ready;
}

GARNET_INTERNAL void rdna4_rt_emit_barrier(VkCommandBuffer cmd, rdna4rt_barrier_kind_t kind, BOOL isComputeQueue) {
    rdna4rt_barrier_spec_t sp = rdna4rt_barrier_spec(kind, isComputeQueue ? 1 : 0);
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = (VkAccessFlags)sp.src_access,
        .dstAccessMask = (VkAccessFlags)sp.dst_access
    };
    vkCmdPipelineBarrier(cmd,
                         (VkPipelineStageFlags)sp.src_stage,
                         (VkPipelineStageFlags)sp.dst_stage,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

// ---------------------------------------------------------------------------------------------------------------
// Lazy push-constant-only BDA compute pipelines for GPU-driven RT AS builds (Wave 3.2):
//   - instPipe:  MTLStorageModePrivate instanceDescriptorBuffer (Default 64B / UserID 68B / Indirect 72B)
//                and zero-CPU-readback MTLIndirectInstanceAccelerationStructureDescriptor
//   - xformPipe: MTLStorageModePrivate transformationMatrixBuffer (column-major 4x3 / unaligned row-major 3x4)
// ---------------------------------------------------------------------------------------------------------------
typedef struct {
    pthread_mutex_t mu;
    VkDevice dev;
    VkPipelineLayout instLayout;
    VkPipeline instPipe;
    VkPipelineLayout xformLayout;
    VkPipeline xformPipe;
    VkPipelineLayout curveLayout;
    VkPipeline curvePipe;
    VkPipelineLayout motionBlasLayout;
    VkPipeline motionBlasPipe;
    VkPipelineLayout motionTlasLayout;
    VkPipeline motionTlasPipe;
} RDNA4RTComputePipes;

static RDNA4RTComputePipes g_rtPipes = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_rt_create_compute_pipe(
    const uint32_t *words, size_t byteSize, uint32_t pushBytes,
    VkPipelineLayout *outLayout, VkPipeline *outPipe, NSError **err) {
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0u,
        .size = pushBytes
    };
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0u,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges = &pcr
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineLayout(RDNA4R.dev, &lci, NULL, &layout);
    if (r != VK_SUCCESS || !layout) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt compute vkCreatePipelineLayout = %d", r]);
        return NO;
    }
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = byteSize,
        .pCode = words
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    r = vkCreateShaderModule(RDNA4R.dev, &smci, NULL, &mod);
    if (r != VK_SUCCESS || !mod) {
        vkDestroyPipelineLayout(RDNA4R.dev, layout, NULL);
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt compute vkCreateShaderModule = %d", r]);
        return NO;
    }
    VkComputePipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = mod,
            .pName = "main"
        },
        .layout = layout
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    r = vkCreateComputePipelines(RDNA4R.dev, RDNA4R.pc, 1u, &pci, NULL, &pipe);
    vkDestroyShaderModule(RDNA4R.dev, mod, NULL);
    if (r != VK_SUCCESS || !pipe) {
        vkDestroyPipelineLayout(RDNA4R.dev, layout, NULL);
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt compute vkCreateComputePipelines = %d", r]);
        return NO;
    }
    *outLayout = layout;
    *outPipe = pipe;
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_ensure_compute_pipes(
    VkPipelineLayout *instLayoutOut, VkPipeline *instPipeOut,
    VkPipelineLayout *xformLayoutOut, VkPipeline *xformPipeOut,
    NSError **err) {
    if (!RDNA4R.ok || !RDNA4R.dev) {
        if (err) *err = rdna4_err(140, @"rt compute pipelines: Vulkan device unavailable");
        return NO;
    }
    pthread_mutex_lock(&g_rtPipes.mu);
    if (g_rtPipes.dev == RDNA4R.dev && g_rtPipes.instPipe && g_rtPipes.xformPipe) {
        if (instLayoutOut) *instLayoutOut = g_rtPipes.instLayout;
        if (instPipeOut) *instPipeOut = g_rtPipes.instPipe;
        if (xformLayoutOut) *xformLayoutOut = g_rtPipes.xformLayout;
        if (xformPipeOut) *xformPipeOut = g_rtPipes.xformPipe;
        pthread_mutex_unlock(&g_rtPipes.mu);
        return YES;
    }
    g_rtPipes.dev = RDNA4R.dev;
    g_rtPipes.instLayout = VK_NULL_HANDLE;
    g_rtPipes.instPipe = VK_NULL_HANDLE;
    g_rtPipes.xformLayout = VK_NULL_HANDLE;
    g_rtPipes.xformPipe = VK_NULL_HANDLE;
    BOOL ok = rdna4_rt_create_compute_pipe(
                  rdna4rt_spv_inst_convert, sizeof(rdna4rt_spv_inst_convert),
                  (uint32_t)sizeof(rdna4rt_inst_convert_pc_t),
                  &g_rtPipes.instLayout, &g_rtPipes.instPipe, err) &&
              rdna4_rt_create_compute_pipe(
                  rdna4rt_spv_xform_transpose, sizeof(rdna4rt_spv_xform_transpose),
                  (uint32_t)sizeof(rdna4rt_xform_transpose_pc_t),
                  &g_rtPipes.xformLayout, &g_rtPipes.xformPipe, err);
    if (!ok) {
        if (g_rtPipes.instPipe) vkDestroyPipeline(RDNA4R.dev, g_rtPipes.instPipe, NULL);
        if (g_rtPipes.instLayout) vkDestroyPipelineLayout(RDNA4R.dev, g_rtPipes.instLayout, NULL);
        if (g_rtPipes.xformPipe) vkDestroyPipeline(RDNA4R.dev, g_rtPipes.xformPipe, NULL);
        if (g_rtPipes.xformLayout) vkDestroyPipelineLayout(RDNA4R.dev, g_rtPipes.xformLayout, NULL);
        g_rtPipes.instLayout = VK_NULL_HANDLE; g_rtPipes.instPipe = VK_NULL_HANDLE;
        g_rtPipes.xformLayout = VK_NULL_HANDLE; g_rtPipes.xformPipe = VK_NULL_HANDLE;
        g_rtPipes.curveLayout = VK_NULL_HANDLE; g_rtPipes.curvePipe = VK_NULL_HANDLE;
        g_rtPipes.motionBlasLayout = VK_NULL_HANDLE; g_rtPipes.motionBlasPipe = VK_NULL_HANDLE;
        g_rtPipes.motionTlasLayout = VK_NULL_HANDLE; g_rtPipes.motionTlasPipe = VK_NULL_HANDLE;
        g_rtPipes.dev = VK_NULL_HANDLE;
        pthread_mutex_unlock(&g_rtPipes.mu);
        return NO;
    }
    if (instLayoutOut) *instLayoutOut = g_rtPipes.instLayout;
    if (instPipeOut) *instPipeOut = g_rtPipes.instPipe;
    if (xformLayoutOut) *xformLayoutOut = g_rtPipes.xformLayout;
    if (xformPipeOut) *xformPipeOut = g_rtPipes.xformPipe;
    pthread_mutex_unlock(&g_rtPipes.mu);
    RDNA4LOGR("rt compute pipelines ready (inst_convert + xform_transpose)");
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_ensure_curve_pipe(
    VkPipelineLayout *curveLayoutOut, VkPipeline *curvePipeOut, NSError **err) {
    if (!RDNA4R.ok || !RDNA4R.dev) {
        if (err) *err = rdna4_err(140, @"rt curve pipeline: Vulkan device unavailable");
        return NO;
    }
    pthread_mutex_lock(&g_rtPipes.mu);
    if (g_rtPipes.dev == RDNA4R.dev && g_rtPipes.curvePipe) {
        if (curveLayoutOut) *curveLayoutOut = g_rtPipes.curveLayout;
        if (curvePipeOut) *curvePipeOut = g_rtPipes.curvePipe;
        pthread_mutex_unlock(&g_rtPipes.mu);
        return YES;
    }
    if (g_rtPipes.dev != RDNA4R.dev) {
        g_rtPipes.dev = RDNA4R.dev;
        g_rtPipes.instLayout = VK_NULL_HANDLE; g_rtPipes.instPipe = VK_NULL_HANDLE;
        g_rtPipes.xformLayout = VK_NULL_HANDLE; g_rtPipes.xformPipe = VK_NULL_HANDLE;
        g_rtPipes.curveLayout = VK_NULL_HANDLE; g_rtPipes.curvePipe = VK_NULL_HANDLE;
        g_rtPipes.motionBlasLayout = VK_NULL_HANDLE; g_rtPipes.motionBlasPipe = VK_NULL_HANDLE;
        g_rtPipes.motionTlasLayout = VK_NULL_HANDLE; g_rtPipes.motionTlasPipe = VK_NULL_HANDLE;
    }
    VkPipelineLayout cl = VK_NULL_HANDLE;
    VkPipeline cp = VK_NULL_HANDLE;
    BOOL ok = rdna4_rt_create_compute_pipe(
        rdna4rt_spv_curve_build, sizeof(rdna4rt_spv_curve_build),
        (uint32_t)sizeof(rdna4rt_curve_build_pc_t), &cl, &cp, err);
    if (!ok) {
        pthread_mutex_unlock(&g_rtPipes.mu);
        return NO;
    }
    g_rtPipes.curveLayout = cl;
    g_rtPipes.curvePipe = cp;
    if (curveLayoutOut) *curveLayoutOut = cl;
    if (curvePipeOut) *curvePipeOut = cp;
    pthread_mutex_unlock(&g_rtPipes.mu);
    RDNA4LOGR("rt compute pipeline ready (curve_build)");
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_ensure_motion_pipes(
    VkPipelineLayout *mBlasLayoutOut, VkPipeline *mBlasPipeOut,
    VkPipelineLayout *mTlasLayoutOut, VkPipeline *mTlasPipeOut,
    NSError **err) {
    if (!RDNA4R.ok || !RDNA4R.dev) {
        if (err) *err = rdna4_err(140, @"rt motion pipelines: Vulkan device unavailable");
        return NO;
    }
    pthread_mutex_lock(&g_rtPipes.mu);
    if (g_rtPipes.dev == RDNA4R.dev && g_rtPipes.motionBlasPipe && g_rtPipes.motionTlasPipe) {
        if (mBlasLayoutOut) *mBlasLayoutOut = g_rtPipes.motionBlasLayout;
        if (mBlasPipeOut) *mBlasPipeOut = g_rtPipes.motionBlasPipe;
        if (mTlasLayoutOut) *mTlasLayoutOut = g_rtPipes.motionTlasLayout;
        if (mTlasPipeOut) *mTlasPipeOut = g_rtPipes.motionTlasPipe;
        pthread_mutex_unlock(&g_rtPipes.mu);
        return YES;
    }
    if (g_rtPipes.dev != RDNA4R.dev) {
        g_rtPipes.dev = RDNA4R.dev;
        g_rtPipes.instLayout = VK_NULL_HANDLE; g_rtPipes.instPipe = VK_NULL_HANDLE;
        g_rtPipes.xformLayout = VK_NULL_HANDLE; g_rtPipes.xformPipe = VK_NULL_HANDLE;
        g_rtPipes.curveLayout = VK_NULL_HANDLE; g_rtPipes.curvePipe = VK_NULL_HANDLE;
        g_rtPipes.motionBlasLayout = VK_NULL_HANDLE; g_rtPipes.motionBlasPipe = VK_NULL_HANDLE;
        g_rtPipes.motionTlasLayout = VK_NULL_HANDLE; g_rtPipes.motionTlasPipe = VK_NULL_HANDLE;
    }
    if (!g_rtPipes.motionBlasPipe) {
        if (!rdna4_rt_create_compute_pipe(
                rdna4rt_spv_motion_blas_build, sizeof(rdna4rt_spv_motion_blas_build),
                (uint32_t)sizeof(rdna4rt_motion_blas_build_pc_t),
                &g_rtPipes.motionBlasLayout, &g_rtPipes.motionBlasPipe, err)) {
            pthread_mutex_unlock(&g_rtPipes.mu);
            return NO;
        }
    }
    if (!g_rtPipes.motionTlasPipe) {
        if (!rdna4_rt_create_compute_pipe(
                rdna4rt_spv_motion_tlas_build, sizeof(rdna4rt_spv_motion_tlas_build),
                (uint32_t)sizeof(rdna4rt_motion_tlas_build_pc_t),
                &g_rtPipes.motionTlasLayout, &g_rtPipes.motionTlasPipe, err)) {
            pthread_mutex_unlock(&g_rtPipes.mu);
            return NO;
        }
    }
    if (mBlasLayoutOut) *mBlasLayoutOut = g_rtPipes.motionBlasLayout;
    if (mBlasPipeOut) *mBlasPipeOut = g_rtPipes.motionBlasPipe;
    if (mTlasLayoutOut) *mTlasLayoutOut = g_rtPipes.motionTlasLayout;
    if (mTlasPipeOut) *mTlasPipeOut = g_rtPipes.motionTlasPipe;
    pthread_mutex_unlock(&g_rtPipes.mu);
    RDNA4LOGR("rt compute pipelines ready (motion_blas_build + motion_tlas_build)");
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// Fence-retired RT staging buffer pool (Wave 3.1): blocks return to pool in dealloc after segment fence retires.
// ---------------------------------------------------------------------------------------------------------------
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceAddress addr;
    uint64_t size;
    uint64_t allocSize;
} RDNA4RTStagingSlot;

typedef struct {
    pthread_mutex_t mu;
    VkDevice dev;
    uint32_t count;
    uint64_t totalBytes;
    uint64_t hits;
    uint64_t misses;
    RDNA4RTStagingSlot slots[RDNA4RT_STAGING_POOL_MAX_SLABS];
} RDNA4RTStagingPoolState;

static RDNA4RTStagingPoolState g_rtStagingPool = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_rt_staging_pool_pop(uint64_t needSize, RDNA4RTStagingSlot *out) {
    pthread_mutex_lock(&g_rtStagingPool.mu);
    if (g_rtStagingPool.dev != RDNA4R.dev) {
        g_rtStagingPool.dev = RDNA4R.dev;
        g_rtStagingPool.count = 0u;
        g_rtStagingPool.totalBytes = 0u;
    }
    uint32_t bestIdx = UINT32_MAX;
    uint64_t bestSize = UINT64_MAX;
    for (uint32_t i = 0; i < g_rtStagingPool.count; i++) {
        uint64_t sz = g_rtStagingPool.slots[i].size;
        if (sz >= needSize && sz < bestSize) {
            bestSize = sz;
            bestIdx = i;
            if (sz == needSize) break;
        }
    }
    if (bestIdx != UINT32_MAX) {
        *out = g_rtStagingPool.slots[bestIdx];
        g_rtStagingPool.totalBytes -= out->allocSize;
        g_rtStagingPool.slots[bestIdx] = g_rtStagingPool.slots[g_rtStagingPool.count - 1u];
        g_rtStagingPool.count--;
        g_rtStagingPool.hits++;
        pthread_mutex_unlock(&g_rtStagingPool.mu);
        return YES;
    }
    g_rtStagingPool.misses++;
    pthread_mutex_unlock(&g_rtStagingPool.mu);
    return NO;
}

static BOOL rdna4_rt_staging_pool_push(const RDNA4RTStagingSlot *slot) {
    if (!RDNA4R.ok || !RDNA4R.dev || !slot->buf || !slot->mem) return NO;
    pthread_mutex_lock(&g_rtStagingPool.mu);
    if (g_rtStagingPool.dev == VK_NULL_HANDLE) g_rtStagingPool.dev = RDNA4R.dev;
    if (g_rtStagingPool.dev != RDNA4R.dev ||
        g_rtStagingPool.count >= RDNA4RT_STAGING_POOL_MAX_SLABS ||
        g_rtStagingPool.totalBytes + slot->allocSize > RDNA4RT_STAGING_POOL_MAX_BUCKET) {
        pthread_mutex_unlock(&g_rtStagingPool.mu);
        return NO;
    }
    g_rtStagingPool.slots[g_rtStagingPool.count++] = *slot;
    g_rtStagingPool.totalBytes += slot->allocSize;
    pthread_mutex_unlock(&g_rtStagingPool.mu);
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// Retirement-owned Vulkan helpers: retained on RDNA4CommandBuffer segments until GPU fence completion.
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4RTStagingBuffer
- (instancetype)initWithSize:(uint64_t)size error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!size) size = 64u;
    uint64_t aligned = 0;
    if (!rdna4rt_align_up_u64(size, RDNA4RT_VK_INSTANCE_ALIGN, &aligned)) {
        if (err) *err = rdna4_err(140, @"RT staging buffer size overflow");
        return nil;
    }
    uint64_t allocTarget = aligned;
    uint64_t bucket = 0;
    BOOL poolable = rdna4rt_staging_bucket_size(aligned, &bucket) ? YES : NO;
    if (poolable) {
        RDNA4RTStagingSlot recycled;
        if (rdna4_rt_staging_pool_pop(bucket, &recycled)) {
            _buf = recycled.buf;
            _mem = recycled.mem;
            _map = recycled.map;
            _addr = recycled.addr;
            _size = recycled.size;
            _allocSize = recycled.allocSize;
            _pooled = YES;
            return self;
        }
        allocTarget = bucket;
    }
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = allocTarget,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT
    };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &_buf);
    if (r != VK_SUCCESS || !_buf) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkCreateBuffer = %d", r]);
        return nil;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, _buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(140, @"rt staging: no host-visible coherent memory type");
        return nil;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &_mem);
    if (r != VK_SUCCESS || !_mem) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkAllocateMemory = %d", r]);
        return nil;
    }
    _allocSize = mr.size;
    atomic_fetch_add(&rdna4_alloc_total, _allocSize);
    r = vkBindBufferMemory(RDNA4R.dev, _buf, _mem, 0);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkBindBufferMemory = %d", r]);
        return nil;
    }
    r = vkMapMemory(RDNA4R.dev, _mem, 0, VK_WHOLE_SIZE, 0, &_map);
    if (r != VK_SUCCESS || !_map) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkMapMemory = %d", r]);
        return nil;
    }
    VkBufferDeviceAddressInfo bda = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _buf
    };
    _addr = rdna4_vkGetBufferAddress(RDNA4R.dev, &bda);
    if (!_addr || (_addr % RDNA4RT_VK_INSTANCE_ALIGN) != 0u) {
        if (err) *err = rdna4_err(140, @"rt staging: invalid or misaligned buffer device address");
        return nil;
    }
    _size = allocTarget;
    _pooled = poolable;
    return self;
}

- (void)dealloc {
    if (_pooled && _buf && _mem && RDNA4R.ok) {
        RDNA4RTStagingSlot slot = {
            .buf = _buf, .mem = _mem, .map = _map, .addr = _addr,
            .size = _size, .allocSize = _allocSize
        };
        if (rdna4_rt_staging_pool_push(&slot)) {
            _buf = VK_NULL_HANDLE;
            _mem = VK_NULL_HANDLE;
            _map = NULL;
            _addr = 0;
            return;
        }
    }
    if (RDNA4R.ok) {
        if (_buf) vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        if (_mem) {
            vkFreeMemory(RDNA4R.dev, _mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, _allocSize);
        }
    }
    _buf = VK_NULL_HANDLE;
    _mem = VK_NULL_HANDLE;
    _map = NULL;
    _addr = 0;
}
@end


typedef struct {
    pthread_mutex_t mu;
    VkDevice dev;
    rdna4rt_qp_pool_t pool;
} RDNA4RTQueryPoolCache;

static RDNA4RTQueryPoolCache g_rtQueryPool = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_rt_qp_pool_pop(VkQueryPool *out) {
    if (!RDNA4R.ok || !RDNA4R.dev || !out) return NO;
    pthread_mutex_lock(&g_rtQueryPool.mu);
    if (g_rtQueryPool.dev != RDNA4R.dev) {
        g_rtQueryPool.dev = RDNA4R.dev;
        rdna4rt_qp_pool_reset(&g_rtQueryPool.pool);
    }
    uint64_t h = 0u;
    int ok = rdna4rt_qp_pool_pop(&g_rtQueryPool.pool, &h);
    pthread_mutex_unlock(&g_rtQueryPool.mu);
    if (!ok) return NO;
    *out = (VkQueryPool)(uintptr_t)h;
    return YES;
}

static BOOL rdna4_rt_qp_pool_push(VkQueryPool qp) {
    if (!RDNA4R.ok || !RDNA4R.dev || !qp) return NO;
    pthread_mutex_lock(&g_rtQueryPool.mu);
    if (g_rtQueryPool.dev == VK_NULL_HANDLE) g_rtQueryPool.dev = RDNA4R.dev;
    int ok = (g_rtQueryPool.dev == RDNA4R.dev) &&
             rdna4rt_qp_pool_push(&g_rtQueryPool.pool, (uint64_t)(uintptr_t)qp);
    pthread_mutex_unlock(&g_rtQueryPool.mu);
    return ok ? YES : NO;
}

@implementation RDNA4RTQueryPool
- (instancetype)initCompactedSizePoolWithError:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (rdna4_rt_qp_pool_pop(&_qp)) return self;
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        .queryCount = 1u
    };
    VkResult r = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &_qp);
    if (r != VK_SUCCESS || !_qp) {
        if (err) *err = rdna4_err(141, [NSString stringWithFormat:@"rt query pool vkCreateQueryPool = %d", r]);
        return nil;
    }
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok && _qp) {
        if (rdna4_rt_qp_pool_push(_qp)) {
            _qp = VK_NULL_HANDLE;
            return;
        }
        if (vkDestroyQueryPool) vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
    }
    _qp = VK_NULL_HANDLE;
}
@end

// ---------------------------------------------------------------------------------------------------------------
// Wave 4.1B: Device-wide GPU hash table (RDNA4RTPrimDataTable) + per-AS snapshot buffer (GarnetASPrimData).
// ---------------------------------------------------------------------------------------------------------------
typedef struct {
    pthread_mutex_t mu;
    VkDevice dev;
    VkBuffer buf;
    VkDeviceMemory mem;
    rdna4rt_prim_slot_t *slots;
    VkDeviceAddress bda;
    uint64_t allocSize;
    rdna4rt_prim_slot_t shadow[RDNA4RT_PRIM_TABLE_CAP];
} RDNA4RTPrimDataTableState;

static RDNA4RTPrimDataTableState g_rtPrimTable = { .mu = PTHREAD_MUTEX_INITIALIZER };

GARNET_INTERNAL BOOL rdna4_rt_cmd_write_buf_bytes(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb, VkBuffer dstBuf,
    uint64_t dstOffset, const void *data, uint64_t bytes, NSError **err)
{
    if (bytes == 0u) return YES;
    if (bytes <= RDNA4RT_PRIM_UPDATE_BUF_MAX && (dstOffset % 4u) == 0u &&
        (bytes % 4u) == 0u && RDNA4RTFn.cmdUpdateBuffer) {
        RDNA4RTFn.cmdUpdateBuffer(cmd, dstBuf, dstOffset, bytes, data);
        return YES;
    }
    RDNA4RTStagingBuffer *stg = [[RDNA4RTStagingBuffer alloc] initWithSize:bytes error:err];
    if (!stg) return NO;
    memcpy(stg->_map, data, (size_t)bytes);
    [cb rdna4RetainEncoded:stg];
    VkBufferCopy copy = { .srcOffset = 0u, .dstOffset = dstOffset, .size = bytes };
    vkCmdCopyBuffer(cmd, stg->_buf, dstBuf, 1u, &copy);
    return YES;
}

static BOOL rdna4_rt_prim_table_ensure_locked(NSError **err) {
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.bufferAddress || !rdna4_vkGetBufferAddress) {
        if (err) *err = rdna4_err(146, @"RT primitive data table requires Vulkan buffer device address");
        return NO;
    }
    if (g_rtPrimTable.dev == RDNA4R.dev && g_rtPrimTable.buf && g_rtPrimTable.slots && g_rtPrimTable.bda)
        return YES;
    g_rtPrimTable.dev = VK_NULL_HANDLE;
    g_rtPrimTable.buf = VK_NULL_HANDLE;
    g_rtPrimTable.mem = VK_NULL_HANDLE;
    g_rtPrimTable.slots = NULL;
    g_rtPrimTable.bda = 0u;
    g_rtPrimTable.allocSize = 0u;
    memset(g_rtPrimTable.shadow, 0, sizeof(g_rtPrimTable.shadow));

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = RDNA4RT_PRIM_TABLE_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &buf);
    if (r != VK_SUCCESS || !buf) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"rt prim table vkCreateBuffer = %d", r]);
        return NO;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        vkDestroyBuffer(RDNA4R.dev, buf, NULL);
        if (err) *err = rdna4_err(146, @"rt prim table: no host-visible coherent memory type");
        return NO;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS || !mem) {
        vkDestroyBuffer(RDNA4R.dev, buf, NULL);
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"rt prim table vkAllocateMemory = %d", r]);
        return NO;
    }
    r = vkBindBufferMemory(RDNA4R.dev, buf, mem, 0);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(RDNA4R.dev, buf, NULL);
        vkFreeMemory(RDNA4R.dev, mem, NULL);
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"rt prim table vkBindBufferMemory = %d", r]);
        return NO;
    }
    void *map = NULL;
    r = vkMapMemory(RDNA4R.dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
    if (r != VK_SUCCESS || !map) {
        vkDestroyBuffer(RDNA4R.dev, buf, NULL);
        vkFreeMemory(RDNA4R.dev, mem, NULL);
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"rt prim table vkMapMemory = %d", r]);
        return NO;
    }
    memset(map, 0, RDNA4RT_PRIM_TABLE_BYTES);
    VkBufferDeviceAddressInfo bdaInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buf
    };
    VkDeviceAddress bda = rdna4_vkGetBufferAddress(RDNA4R.dev, &bdaInfo);
    if (!bda) {
        vkUnmapMemory(RDNA4R.dev, mem);
        vkDestroyBuffer(RDNA4R.dev, buf, NULL);
        vkFreeMemory(RDNA4R.dev, mem, NULL);
        if (err) *err = rdna4_err(146, @"rt prim table: zero buffer device address");
        return NO;
    }
    atomic_fetch_add(&rdna4_alloc_total, mr.size);
    g_rtPrimTable.dev = RDNA4R.dev;
    g_rtPrimTable.buf = buf;
    g_rtPrimTable.mem = mem;
    g_rtPrimTable.slots = (rdna4rt_prim_slot_t *)map;
    g_rtPrimTable.bda = bda;
    g_rtPrimTable.allocSize = mr.size;
    RDNA4LOGR("RDNA4RTPrimDataTable ready: %u slots (%zu bytes) at bda 0x%llx",
              RDNA4RT_PRIM_TABLE_CAP, RDNA4RT_PRIM_TABLE_BYTES, (unsigned long long)bda);
    return YES;
}

// GPU-timeline publish: allocates/updates the slot in the CPU shadow table, marks a previously
// EMPTY GPU slot as TOMBSTONE on CPU (so an abandoned CB never breaks collision chains without
// disturbing any live GPU slot), and records the 32B slot publish into `cmd` before POST_BUILD.
static BOOL rdna4_rt_prim_table_register_cmd(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    uint64_t asBda, uint64_t payloadBda, uint32_t count,
    uint32_t entryStride, uint32_t refOffset, uint32_t kind, NSError **err)
{
    uint32_t slotIdx = 0u;
    int wasEmpty = 0;
    VkBuffer tableBuf = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rtPrimTable.mu);
    if (!rdna4_rt_prim_table_ensure_locked(err)) {
        pthread_mutex_unlock(&g_rtPrimTable.mu);
        return NO;
    }
    rdna4rtr_status_t st = rdna4rt_prim_table_upsert_slot(
        g_rtPrimTable.shadow, RDNA4RT_PRIM_TABLE_CAP,
        asBda, payloadBda, count, entryStride, refOffset, kind,
        &slotIdx, &wasEmpty);
    if (st == RDNA4RTR_OK) {
        if (wasEmpty) {
            __atomic_store_n(&g_rtPrimTable.slots[slotIdx].as_bda,
                             RDNA4RT_PRIM_SLOT_TOMBSTONE, __ATOMIC_RELEASE);
        }
        tableBuf = g_rtPrimTable.buf;
    }
    pthread_mutex_unlock(&g_rtPrimTable.mu);
    if (st != RDNA4RTR_OK) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"RT primitive data table register failed: %s",
                                        rdna4rtr_status_str(st)]);
        return NO;
    }
    rdna4rt_prim_slot_t slotVal = {
        .as_bda = asBda,
        .payload_bda = payloadBda,
        .count = count,
        .entry_stride = entryStride,
        .ref_offset = refOffset,
        .kind = kind
    };
    return rdna4_rt_cmd_write_buf_bytes(
        cmd, cb, tableBuf, (uint64_t)slotIdx * (uint64_t)sizeof(rdna4rt_prim_slot_t),
        &slotVal, sizeof(slotVal), err);
}

// GPU-timeline clear (for encode-time rebuild without primData or AS BDA switch): tombstones the
// matching entry in shadow[] and records an 8B TOMBSTONE write into `cmd` so in-flight GPU work
// prior to this command keeps seeing the previous live entry.
static BOOL rdna4_rt_prim_table_clear_cmd(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    uint64_t asBda, uint64_t expectedPayloadBda, NSError **err)
{
    if (asBda <= RDNA4RT_PRIM_SLOT_TOMBSTONE) return YES;
    uint32_t slotIdx = 0u;
    int removed = 0;
    VkBuffer tableBuf = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rtPrimTable.mu);
    if (g_rtPrimTable.dev == RDNA4R.dev && g_rtPrimTable.buf) {
        removed = rdna4rt_prim_table_remove_if_match_slot(
            g_rtPrimTable.shadow, RDNA4RT_PRIM_TABLE_CAP,
            asBda, expectedPayloadBda, &slotIdx);
        if (removed) tableBuf = g_rtPrimTable.buf;
    }
    pthread_mutex_unlock(&g_rtPrimTable.mu);
    if (!removed) return YES;
    const uint64_t tomb = RDNA4RT_PRIM_SLOT_TOMBSTONE;
    return rdna4_rt_cmd_write_buf_bytes(
        cmd, cb, tableBuf, (uint64_t)slotIdx * (uint64_t)sizeof(rdna4rt_prim_slot_t),
        &tomb, sizeof(tomb), err);
}

// Dealloc-time CPU unregister: safe on CPU because all command buffers retaining the AS have retired.
static void rdna4_rt_prim_table_unregister_cpu(uint64_t asBda, uint64_t expectedPayloadBda) {
    if (asBda <= RDNA4RT_PRIM_SLOT_TOMBSTONE) return;
    pthread_mutex_lock(&g_rtPrimTable.mu);
    if (g_rtPrimTable.dev == RDNA4R.dev && g_rtPrimTable.slots) {
        uint32_t slotIdx = 0u;
        if (rdna4rt_prim_table_remove_if_match_slot(
                g_rtPrimTable.shadow, RDNA4RT_PRIM_TABLE_CAP,
                asBda, expectedPayloadBda, &slotIdx)) {
            __atomic_store_n(&g_rtPrimTable.slots[slotIdx].as_bda,
                             RDNA4RT_PRIM_SLOT_TOMBSTONE, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&g_rtPrimTable.mu);
}

GARNET_INTERNAL __attribute__((unused)) BOOL rdna4_rt_fill_stage_prim_table(
    id cb, id dev, const RDNA4RTStageSlots *rtSlots, rdna4rt_stage_prim_table_t *out)
{
    if (!cb || !out) return NO;
    NSError *err = nil;
    pthread_mutex_lock(&g_rtPrimTable.mu);
    BOOL ok = rdna4_rt_prim_table_ensure_locked(&err);
    uint64_t globalBda = ok ? (uint64_t)g_rtPrimTable.bda : 0u;
    pthread_mutex_unlock(&g_rtPrimTable.mu);
    if (!ok || !globalBda) {
        RDNA4LOG_ERR("rt prim table fill failed: %s", err ? err.localizedDescription.UTF8String : "unavailable");
        return NO;
    }
    rdna4rt_stage_prim_table_init(out, globalBda, RDNA4RT_PRIM_TABLE_MASK);
    if (rtSlots) {
        SEL retainSel = sel_registerName("rdna4RetainEncoded:");
        const BOOL canRetain = [cb respondsToSelector:retainSel];
        for (uint32_t i = 0; i < RDNA4RTR_MAX_BUFFER_SLOTS; i++) {
            id raw = rtSlots->as[i];
            if (!raw || ![raw isKindOfClass:[GarnetAccelerationStructure class]]) continue;
            GarnetAccelerationStructure *as = (GarnetAccelerationStructure *)raw;
            if (dev && [as device] != dev) continue;
            out->slot_as_bda[i] = (uint64_t)[as rdna4DeviceAddress];
            if (canRetain) ((void (*)(id, SEL, id))objc_msgSend)(cb, retainSel, raw);
            id handle = [as rdna4CurrentHandle];
            if (handle && canRetain) ((void (*)(id, SEL, id))objc_msgSend)(cb, retainSel, handle);
            id primData = [as rdna4CurrentPrimData];
            if (primData && canRetain) ((void (*)(id, SEL, id))objc_msgSend)(cb, retainSel, primData);
        }
    }
    return YES;
}

typedef struct {
    pthread_mutex_t mu;
    VkDevice dev;
    rdna4rt_buf_pool_t pool;
} RDNA4RTPrimDataPoolState;

static RDNA4RTPrimDataPoolState g_rtPrimPool = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_rt_prim_pool_pop(uint64_t needSize, rdna4rt_buf_pool_entry_t *out) {
    if (!RDNA4R.ok || !RDNA4R.dev || !out) return NO;
    pthread_mutex_lock(&g_rtPrimPool.mu);
    if (g_rtPrimPool.dev != RDNA4R.dev) {
        g_rtPrimPool.dev = RDNA4R.dev;
        rdna4rt_buf_pool_reset(&g_rtPrimPool.pool);
    }
    int ok = rdna4rt_buf_pool_pop(&g_rtPrimPool.pool, needSize, out);
    pthread_mutex_unlock(&g_rtPrimPool.mu);
    return ok ? YES : NO;
}

static BOOL rdna4_rt_prim_pool_push(const rdna4rt_buf_pool_entry_t *entry) {
    if (!RDNA4R.ok || !RDNA4R.dev || !entry) return NO;
    pthread_mutex_lock(&g_rtPrimPool.mu);
    if (g_rtPrimPool.dev == VK_NULL_HANDLE) g_rtPrimPool.dev = RDNA4R.dev;
    int ok = (g_rtPrimPool.dev == RDNA4R.dev) && rdna4rt_buf_pool_push(&g_rtPrimPool.pool, entry);
    pthread_mutex_unlock(&g_rtPrimPool.mu);
    return ok ? YES : NO;
}

@implementation GarnetASPrimData
- (instancetype)initWithSize:(uint64_t)totalBytes kind:(uint32_t)kind count:(uint32_t)count
                 entryStride:(uint32_t)entryStride refOffset:(uint32_t)refOffset error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!totalBytes || !RDNA4R.ok || !RDNA4R.dev || !rdna4_vkGetBufferAddress) {
        if (err) *err = rdna4_err(146, @"GarnetASPrimData: invalid size or Vulkan device unavailable");
        return nil;
    }
    uint64_t alignedSize = 0u;
    if (!rdna4rt_align_up_u64(totalBytes, RDNA4RT_PRIM_DATA_ALIGN, &alignedSize)) {
        if (err) *err = rdna4_err(146, @"GarnetASPrimData: size overflow");
        return nil;
    }
    uint64_t allocTarget = alignedSize;
    uint64_t bucket = 0u;
    BOOL poolable = rdna4rt_staging_bucket_size(alignedSize, &bucket) ? YES : NO;
    if (poolable) {
        rdna4rt_buf_pool_entry_t recycled;
        if (rdna4_rt_prim_pool_pop(bucket, &recycled)) {
            _buf = (VkBuffer)(uintptr_t)recycled.buf_handle;
            _mem = (VkDeviceMemory)(uintptr_t)recycled.mem_handle;
            _bda = (VkDeviceAddress)recycled.bda;
            _capacity = recycled.cap;
            _allocatedSize = recycled.alloc_size;
            _pooled = YES;
            _totalBytes = totalBytes;
            _kind = kind;
            _count = count;
            _entryStride = entryStride;
            _refOffset = refOffset;
            return self;
        }
        allocTarget = bucket;
    }
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = allocTarget,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &_buf);
    if (r != VK_SUCCESS || !_buf) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"GarnetASPrimData vkCreateBuffer = %d", r]);
        return nil;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, _buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(146, @"GarnetASPrimData: no device-local memory type");
        return nil;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &_mem);
    if (r != VK_SUCCESS || !_mem) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"GarnetASPrimData vkAllocateMemory = %d", r]);
        return nil;
    }
    _allocatedSize = mr.size;
    atomic_fetch_add(&rdna4_alloc_total, _allocatedSize);
    r = vkBindBufferMemory(RDNA4R.dev, _buf, _mem, 0);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"GarnetASPrimData vkBindBufferMemory = %d", r]);
        return nil;
    }
    VkBufferDeviceAddressInfo bdaInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _buf
    };
    _bda = rdna4_vkGetBufferAddress(RDNA4R.dev, &bdaInfo);
    if (!_bda) {
        if (err) *err = rdna4_err(146, @"GarnetASPrimData: zero buffer device address");
        return nil;
    }
    _capacity = allocTarget;
    _pooled = poolable;
    _totalBytes = totalBytes;
    _kind = kind;
    _count = count;
    _entryStride = entryStride;
    _refOffset = refOffset;
    return self;
}

- (void)dealloc {
    free(_blasHeaders);
    _blasHeaders = NULL;
    _referencedPrimData = nil;
    if (_pooled && _buf && _mem && _bda && RDNA4R.ok) {
        rdna4rt_buf_pool_entry_t entry = {
            .buf_handle = (uint64_t)(uintptr_t)_buf,
            .mem_handle = (uint64_t)(uintptr_t)_mem,
            .bda = (uint64_t)_bda,
            .cap = _capacity,
            .alloc_size = _allocatedSize
        };
        if (rdna4_rt_prim_pool_push(&entry)) {
            _buf = VK_NULL_HANDLE;
            _mem = VK_NULL_HANDLE;
            _bda = 0u;
            _capacity = 0u;
            _allocatedSize = 0u;
            _pooled = NO;
            return;
        }
    }
    if (RDNA4R.ok) {
        if (_buf) vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        if (_mem) {
            vkFreeMemory(RDNA4R.dev, _mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, _allocatedSize);
        }
    }
    _buf = VK_NULL_HANDLE;
    _mem = VK_NULL_HANDLE;
    _bda = 0u;
    _capacity = 0u;
    _pooled = NO;
}
@end

// Backing Private VkBuffer + VkDeviceMemory (standalone or MTLHeap suballocated). Held strongly by
// GarnetASHandle so Vulkan always destroys VkAccelerationStructureKHR before its storage VkBuffer
// and VkDeviceMemory (and before RDNA4Heap detaches the backing block).

@implementation GarnetASStorage
- (void)rdna4AbortHeapConstruction {
    RDNA4Heap *heap = _hpHeap;
    uint64_t bid = _hpId, block = _hpBlk, lease = _hpLease;
    const BOOL bound = _hpBacking != nil;
    if (_hpBacking) {
        [_hpBacking destroyHandles];
        _buf = VK_NULL_HANDLE;
    } else if (RDNA4R.ok && _buf) {
        vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        _buf = VK_NULL_HANDLE;
    }
    _hpBacking = nil;
    if (heap && block && bound) [heap rdna4CloseBackingHandle:block];
    if (heap && lease) [heap rdna4AbandonBacking:lease];
    if (heap && bid) [heap rdna4Free:bid];
    _hpHeap = nil;
    _hpId = _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0u;
    _hpDedicated = NO;
    _hpOff = _hpSize = 0u;
}

- (instancetype)initWithBackingSize:(uint64_t)backingSize error:(NSError **)err {
    return [self initWithBackingSize:backingSize heap:nil offset:0 hasOffset:NO error:err];
}

- (instancetype)initWithBackingSize:(uint64_t)backingSize heap:(RDNA4Heap *)heap
                             offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _hpHeap = heap;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = backingSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &_buf);
    if (r != VK_SUCCESS || !_buf) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkCreateBuffer = %d", r]);
        return nil;
    }
    if (heap) {
        if ([heap storageMode] != MTLStorageModePrivate) {
            if (err) *err = rdna4_err(142, @"heap acceleration structures require MTLStorageModePrivate heap");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        VkMemoryRequirements mr;
        BOOL prefersDedicated = NO, requiresDedicated = NO;
        if (!rdna4_requirements2(_buf, VK_NULL_HANDLE, &mr, &prefersDedicated, &requiresDedicated, YES)) {
            if (err) *err = rdna4_err(142, @"heap acceleration structures require vkGetBufferMemoryRequirements2");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        (void)prefersDedicated;
        if (requiresDedicated) {
            if (err) *err = rdna4_err(142, @"acceleration structure storage requires dedicated memory and cannot be suballocated from a heap");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        if (mr.alignment < RDNA4RT_HEAP_AS_ALIGN) mr.alignment = RDNA4RT_HEAP_AS_ALIGN;
        uint64_t alignedMrSize = 0u;
        if (!rdna4rt_align_up_u64((uint64_t)mr.size, RDNA4RT_HEAP_AS_ALIGN, &alignedMrSize)) {
            if (err) *err = rdna4_err(142, @"heap acceleration structure requirement size overflow");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        mr.size = alignedMrSize;

        VkDeviceMemory hmem = VK_NULL_HANDLE;
        uint64_t bindOff = 0u, rangeSize = 0u;
        uint8_t *map = NULL;
        if (hasOffset) {
            _hpOff = offset;
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquirePlacementBackingForBuffer:_buf image:VK_NULL_HANDLE requirements:mr
                                                      offset:offset hostImport:NO requiresDedicated:NO
                                                         mem:&hmem offset:&bindOff map:&map block:&_hpBlk
                                                       lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        } else {
            if (![heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]) {
                if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"heap has no logical range for a %llu-byte acceleration structure",
                                                (unsigned long long)mr.size]);
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquireBackingForBuffer:_buf image:VK_NULL_HANDLE requirements:mr
                                          dedicated:NO hostImport:NO
                                                mem:&hmem offset:&bindOff map:&map block:&_hpBlk
                                              lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        (void)map;
        _hpBindOff = bindOff;
        _hpPhysSize = rangeSize;
        _hpDedicated = NO;
        _allocatedSize = mr.size;
        r = vkBindBufferMemory(RDNA4R.dev, _buf, hmem, bindOff);
        if (r != VK_SUCCESS) {
            if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkBindBufferMemory = %d (heap offset %llu)",
                                            r, (unsigned long long)bindOff]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        VkBufferDeviceAddressInfo bdaInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = _buf
        };
        _deviceAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &bdaInfo);
        if (!_deviceAddress) {
            if (err) *err = rdna4_err(142, @"heap accel storage: zero buffer device address");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        RDNA4HeapBacking *backing = [[RDNA4HeapBacking alloc] initWithBuffer:_buf image:VK_NULL_HANDLE view:VK_NULL_HANDLE];
        if (!backing || ![heap rdna4BindSucceeded:_hpBlk lease:_hpLease owner:backing]) {
            if (backing) {
                [backing destroyHandles];
                _buf = VK_NULL_HANDLE;
            }
            if (err) *err = rdna4_err(142, @"cannot retain heap acceleration structure backing");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        _hpBacking = backing;
        _backingSize = backingSize;
        return self;
    }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, _buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(142, @"accel storage: no device-local memory type");
        return nil;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &_mem);
    if (r != VK_SUCCESS || !_mem) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkAllocateMemory = %d", r]);
        return nil;
    }
    _allocatedSize = mr.size;
    atomic_fetch_add(&rdna4_alloc_total, _allocatedSize);
    r = vkBindBufferMemory(RDNA4R.dev, _buf, _mem, 0);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkBindBufferMemory = %d", r]);
        return nil;
    }
    VkBufferDeviceAddressInfo bdaInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _buf
    };
    _deviceAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &bdaInfo);
    _backingSize = backingSize;
    return self;
}

- (void)dealloc {
    if (_hpHeap && _hpBlk) {
        RDNA4Heap *heap = _hpHeap;
        RDNA4HeapBacking *backing = _hpBacking;
        uint64_t blk = _hpBlk, lease = _hpLease;
        _hpBacking = nil;
        _hpHeap = nil;
        _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = _hpId = 0u;
        _buf = VK_NULL_HANDLE;
        [heap rdna4DetachBacking:backing block:blk lease:lease];
        return;
    }
    if (RDNA4R.ok) {
        if (_buf) vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        if (_mem) {
            vkFreeMemory(RDNA4R.dev, _mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, _allocatedSize);
        }
    }
    _buf = VK_NULL_HANDLE;
    _mem = VK_NULL_HANDLE;
}
@end


@implementation GarnetASHandle
- (instancetype)initWithStorage:(GarnetASStorage *)storage kind:(rdna4rt_kind_t)kind error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!storage || !storage->_buf || (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS)) {
        if (err) *err = rdna4_err(143, @"accel handle: invalid storage or AS kind");
        return nil;
    }
    if (!rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(143, @"accel handle: VK_KHR_acceleration_structure unavailable");
        return nil;
    }
    _storage = storage;
    _kind = kind;
    VkAccelerationStructureCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = storage->_buf,
        .offset = 0,
        .size = storage->_backingSize,
        .type = (kind == RDNA4RT_KIND_TLAS)
            ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };
    VkResult r = RDNA4RTFn.createAS(RDNA4R.dev, &ci, NULL, &_vkAS);
    if (r != VK_SUCCESS || _vkAS == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(143, [NSString stringWithFormat:@"vkCreateAccelerationStructureKHR = %d", r]);
        return nil;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = _vkAS
    };
    _deviceAddress = RDNA4RTFn.getASAddress(RDNA4R.dev, &ai);
    if (!_deviceAddress) {
        if (err) *err = rdna4_err(143, @"vkGetAccelerationStructureDeviceAddressKHR returned 0");
        return nil;
    }
    return self;
}

- (VkDeviceAddress)rdna4EnsureLocalAabbAddress:(NSError **)err {
    if (!_localAabbBuf) {
        RDNA4RTStagingBuffer *buf = [[RDNA4RTStagingBuffer alloc] initWithSize:24u error:err];
        if (!buf) return 0u;
        static const uint32_t emptyOrd[6] = {
            0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu,
            0x00800000u, 0x00800000u, 0x00800000u
        };
        memcpy(buf->_map, emptyOrd, sizeof(emptyOrd));
        _localAabbBuf = buf;
    }
    return _localAabbBuf->_addr;
}

- (void)dealloc {
    if (RDNA4R.ok && _vkAS != VK_NULL_HANDLE && RDNA4RTFn.destroyAS)
        RDNA4RTFn.destroyAS(RDNA4R.dev, _vkAS, NULL);
    _vkAS = VK_NULL_HANDLE;
    _deviceAddress = 0;
    _localAabbBuf = nil;
    _storage = nil;
}
@end

static pthread_mutex_t g_rtUnitBoxMu = PTHREAD_MUTEX_INITIALIZER;
static VkDevice g_rtUnitBoxDev = VK_NULL_HANDLE;
static GarnetASStorage *g_rtUnitBoxStorage = nil;
static GarnetASHandle *g_rtUnitBoxHandle = nil;

GARNET_INTERNAL VkDeviceAddress rdna4_rt_ensure_unit_box_blas(NSError **err) {
    if (!RDNA4R.ok || !RDNA4R.dev || !rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(145, @"unit-box proxy BLAS: Vulkan RT unavailable");
        return 0u;
    }
    pthread_mutex_lock(&g_rtUnitBoxMu);
    if (g_rtUnitBoxDev == RDNA4R.dev && g_rtUnitBoxHandle && g_rtUnitBoxHandle->_deviceAddress) {
        VkDeviceAddress bda = g_rtUnitBoxHandle->_deviceAddress;
        pthread_mutex_unlock(&g_rtUnitBoxMu);
        return bda;
    }
    if (g_rtUnitBoxDev != RDNA4R.dev) {
        g_rtUnitBoxHandle = nil;
        g_rtUnitBoxStorage = nil;
        g_rtUnitBoxDev = RDNA4R.dev;
    }
    RDNA4RTStagingBuffer *aabbStg = [[RDNA4RTStagingBuffer alloc] initWithSize:24u error:err];
    if (!aabbStg) {
        pthread_mutex_unlock(&g_rtUnitBoxMu);
        return 0u;
    }
    static const float unitBox[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    memcpy(aabbStg->_map, unitBox, sizeof(unitBox));

    VkAccelerationStructureGeometryKHR geom = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.aabbs = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
            .data.deviceAddress = aabbStg->_addr,
            .stride = 24u
        }
    };
    uint32_t primCount = 1u;
    VkAccelerationStructureBuildGeometryInfoKHR sizeQuery = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1u,
        .pGeometries = &geom
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    RDNA4RTFn.getBuildSizes(RDNA4R.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                            &sizeQuery, &primCount, &sizes);
    uint64_t asSize = sizes.accelerationStructureSize ? sizes.accelerationStructureSize : 256u;
    uint64_t scrSize = sizes.buildScratchSize ? sizes.buildScratchSize : 256u;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    uint32_t scrAlign = rdna4rt_scratch_align(&m3);
    if (scrAlign < 16u) scrAlign = 16u;

    GarnetASStorage *stg = [[GarnetASStorage alloc] initWithBackingSize:asSize error:err];
    GarnetASHandle *hdl = stg ? [[GarnetASHandle alloc] initWithStorage:stg kind:RDNA4RT_KIND_BLAS error:err] : nil;
    RDNA4RTStagingBuffer *scrStg = hdl ? [[RDNA4RTStagingBuffer alloc] initWithSize:scrSize + (uint64_t)scrAlign error:err] : nil;
    if (!hdl || !scrStg) {
        pthread_mutex_unlock(&g_rtUnitBoxMu);
        return 0u;
    }
    uint64_t scrAddr = ((uint64_t)scrStg->_addr + (uint64_t)(scrAlign - 1u)) & ~((uint64_t)(scrAlign - 1u));
    VkAccelerationStructureBuildGeometryInfoKHR binfo = sizeQuery;
    binfo.dstAccelerationStructure = hdl->_vkAS;
    binfo.scratchData.deviceAddress = scrAddr;
    VkAccelerationStructureBuildRangeInfoKHR brng = { .primitiveCount = 1u };
    const VkAccelerationStructureBuildRangeInfoKHR *pBrng = &brng;

    BOOL unresolved = NO;
    BOOL built = rdna4_oneshot(err, ^(VkCommandBuffer ocmd) {
        (void)aabbStg; (void)scrStg; (void)stg; (void)hdl;
        rdna4_rt_emit_barrier(ocmd, RDNA4RT_BARRIER_PRE_BUILD, NO);
        RDNA4RTFn.cmdBuildAS(ocmd, 1u, &binfo, &pBrng);
        rdna4_rt_emit_barrier(ocmd, RDNA4RT_BARRIER_POST_BUILD, NO);
    }, &unresolved);
    if (!built) {
        pthread_mutex_unlock(&g_rtUnitBoxMu);
        return 0u;
    }
    g_rtUnitBoxStorage = stg;
    g_rtUnitBoxHandle = hdl;
    VkDeviceAddress bda = hdl->_deviceAddress;
    pthread_mutex_unlock(&g_rtUnitBoxMu);
    return bda;
}

// ---------------------------------------------------------------------------------------------------------------
// GarnetAccelerationStructure (MTLAccelerationStructure + MTLResource)
// ---------------------------------------------------------------------------------------------------------------

@implementation GarnetAccelerationStructure
+ (void)initialize {
    if (self == [GarnetAccelerationStructure class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
            rdna4_add_protocols(self, n, 2, "GarnetAccelerationStructure");
        });
    }
}
RDNA4_DNR(GarnetAccelerationStructure)

- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind error:(NSError **)err {
    return [self initWithDevice:dev size:size initialKind:kind heap:nil offset:0 hasOffset:NO error:err];
}

- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind
                          heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset
                         error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || size == 0) {
        if (err) *err = rdna4_err(144, @"acceleration structure requires non-nil device and size > 0");
        return nil;
    }
    if (!rdna4_radv_open(err)) return nil;
    if (!rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(144, @"VK_KHR_acceleration_structure is unavailable on this backend");
        return nil;
    }
    uint64_t backingSize = 0;
    if (!rdna4rt_align_up_u64((uint64_t)size, RDNA4RT_AS_SIZE_ALIGN, &backingSize)) {
        if (err) *err = rdna4_err(144, @"acceleration structure size overflows 256-byte alignment");
        return nil;
    }
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, backingSize)) {
        if (err) *err = rdna4_err(144, @"acceleration structure size exceeds device buffer limit");
        return nil;
    }
    GarnetASStorage *storage = [[GarnetASStorage alloc] initWithBackingSize:backingSize
                                                                       heap:heap
                                                                     offset:offset
                                                                  hasOffset:hasOffset
                                                                      error:err];
    if (!storage) return nil;
    _storage = storage;
    if (heap) {
        _hpHeap = storage->_hpHeap;
        _hpOff = storage->_hpOff;
        _hpSize = storage->_hpSize;
        _hpId = storage->_hpId;
        _hpBlk = storage->_hpBlk;
        _hpLease = storage->_hpLease;
        _hpBindOff = storage->_hpBindOff;
        _hpPhysSize = storage->_hpPhysSize;
        _hpDedicated = storage->_hpDedicated;
    }
    GarnetASHandle *handle = nil;
    if (kind == RDNA4RT_KIND_BLAS || kind == RDNA4RT_KIND_TLAS) {
        handle = [[GarnetASHandle alloc] initWithStorage:storage kind:kind error:err];
        if (!handle) return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _dev = dev;
    _size = size;
    _handle = handle;
    rdna4rt_meta_init(&_meta, (uint64_t)size, kind);
    RDNA4LOGR("GarnetAccelerationStructure %p: size %lu (backing %llu, alloc %llu, kind %u, heap %p off %lu)",
              (__bridge void *)self, (unsigned long)size,
              (unsigned long long)backingSize, (unsigned long long)storage->_allocatedSize,
              (unsigned)kind, (__bridge void *)heap, (unsigned long)(heap ? _hpOff : 0u));
    return self;
}

- (void)dealloc {
    uint64_t regAs = _registeredAsBda;
    uint64_t regPayload = _registeredPayloadBda;
    _registeredAsBda = 0u;
    _registeredPayloadBda = 0u;
    if (regAs) rdna4_rt_prim_table_unregister_cpu(regAs, regPayload);
    _primData = nil;
    _handle = nil;
    RDNA4_HEAP_RETIRE;
    _storage = nil;
    if (_muInit) pthread_mutex_destroy(&_mu);
}

- (GarnetASHandle *)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err {
    if (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS) {
        if (err) *err = rdna4_err(144, @"invalid acceleration structure kind");
        return nil;
    }
    pthread_mutex_lock(&_mu);
    if (_handle && _handle->_kind == kind) {
        GarnetASHandle *existing = _handle;
        pthread_mutex_unlock(&_mu);
        return existing;
    }
    GarnetASStorage *storage = _storage;
    pthread_mutex_unlock(&_mu);
    GarnetASHandle *created = [[GarnetASHandle alloc] initWithStorage:storage kind:kind error:err];
    if (!created) return nil;
    pthread_mutex_lock(&_mu);
    if (!_handle || _handle->_kind != kind) _handle = created;
    GarnetASHandle *result = _handle;
    pthread_mutex_unlock(&_mu);
    return result;
}

- (GarnetASHandle *)rdna4CurrentHandle {
    pthread_mutex_lock(&_mu);
    GarnetASHandle *h = _handle;
    pthread_mutex_unlock(&_mu);
    return h;
}

- (GarnetASPrimData *)rdna4CurrentPrimData {
    pthread_mutex_lock(&_mu);
    GarnetASPrimData *pd = _primData;
    pthread_mutex_unlock(&_mu);
    return pd;
}

- (BOOL)rdna4SetPrimData:(GarnetASPrimData *)primData forAsAddress:(uint64_t)asBda
           commandBuffer:(RDNA4CommandBuffer *)cb vkCmdBuffer:(VkCommandBuffer)cmd error:(NSError **)err {
    pthread_mutex_lock(&_mu);
    GarnetASPrimData *oldPrim = _primData;
    uint64_t oldAsBda = _registeredAsBda;
    uint64_t oldPayloadBda = _registeredPayloadBda;
    if (!primData) {
        _primData = nil;
        _registeredAsBda = 0u;
        _registeredPayloadBda = 0u;
        pthread_mutex_unlock(&_mu);
        if (oldPrim && cb) [cb rdna4RetainEncoded:oldPrim];
        if (oldAsBda)
            return rdna4_rt_prim_table_clear_cmd(cmd, cb, oldAsBda, oldPayloadBda, err);
        return YES;
    }
    pthread_mutex_unlock(&_mu);
    if (!rdna4_rt_prim_table_register_cmd(cmd, cb, asBda, (uint64_t)primData->_bda, primData->_count,
                                          primData->_entryStride, primData->_refOffset, primData->_kind, err))
        return NO;
    pthread_mutex_lock(&_mu);
    oldPrim = _primData;
    oldAsBda = _registeredAsBda;
    oldPayloadBda = _registeredPayloadBda;
    _primData = primData;
    _registeredAsBda = asBda;
    _registeredPayloadBda = (uint64_t)primData->_bda;
    pthread_mutex_unlock(&_mu);
    if (oldPrim && oldPrim != primData && cb) [cb rdna4RetainEncoded:oldPrim];
    if (oldAsBda && oldAsBda != asBda)
        return rdna4_rt_prim_table_clear_cmd(cmd, cb, oldAsBda, oldPayloadBda, err);
    return YES;
}

- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure {
    GarnetASHandle *h = [self rdna4CurrentHandle];
    return h ? h->_vkAS : VK_NULL_HANDLE;
}

- (VkDeviceAddress)rdna4DeviceAddress {
    GarnetASHandle *h = [self rdna4CurrentHandle];
    return h ? h->_deviceAddress : 0u;
}

- (VkDeviceAddress)rdna4BindlessGPUAddress {
    GarnetASHandle *h = [self rdna4CurrentHandle];
    if (h && h->_deviceAddress) return h->_deviceAddress;
    return _storage ? _storage->_deviceAddress : 0u;
}

- (rdna4rt_accel_meta_t)rdna4MetaSnapshot {
    pthread_mutex_lock(&_mu);
    rdna4rt_accel_meta_t snap = _meta;
    pthread_mutex_unlock(&_mu);
    return snap;
}

- (void)rdna4SetMeta:(rdna4rt_accel_meta_t)meta {
    pthread_mutex_lock(&_mu);
    _meta = meta;
    pthread_mutex_unlock(&_mu);
}

- (uint32_t)rdna4AccelKind { return (uint32_t)[self rdna4MetaSnapshot].kind; }
- (uint32_t)rdna4GeomIftBase { return [self rdna4MetaSnapshot].geom_ift_base; }
- (BOOL)rdna4IsBuilt { return [self rdna4MetaSnapshot].state != RDNA4RT_STATE_UNBUILT; }
- (BOOL)rdna4AllowsRefit { return [self rdna4MetaSnapshot].allow_refit != 0u; }
- (BOOL)rdna4AllowsCompaction { return [self rdna4MetaSnapshot].allow_compaction != 0u; }

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)allocatedSize { return _storage ? (NSUInteger)_storage->_allocatedSize : _size; }
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode {
    return _hpHeap ? [_hpHeap hazardTrackingMode] : MTLHazardTrackingModeTracked;
}
- (NSUInteger)resourceOptions {
    return MTLResourceStorageModePrivate | ((NSUInteger)[self hazardTrackingMode] << 8);
}
- (RDNA4Heap *)rdna4ResourceHeap { return _hpHeap; }
- (NSUInteger)rdna4ResourceHeapOffset {
    return (_hpHeap && [_hpHeap type] == MTLHeapTypePlacement) ? _hpOff : 0;
}
- (BOOL)rdna4ResourceAliasable { return _hpAl; }
- (void)rdna4ResourceMakeAliasable { RDNA4_HEAP_RETIRE; }
- (BOOL)rdna4ResourceAliases:(id)other {
    return _hpHeap && _hpBlk && [_hpHeap rdna4Block:_hpBlk offset:_hpBindOff size:_hpPhysSize aliases:other];
}
- (uint64_t)rdna4BackingBlock { return _hpBlk; }
- (uint64_t)rdna4BackingOffset { return _hpBindOff; }
- (uint64_t)rdna4BackingSize { return _hpPhysSize; }
// Bridge 3 (Metal 3 Bindless): return 64-bit GPU BDA (valid before & after build via Wave 0 Invariant #2).
RDNA4_RESOURCE_SPI_CUSTOM_UID([self rdna4BindlessGPUAddress] ?: rdna4_resource_uid(&_uid))
@end
