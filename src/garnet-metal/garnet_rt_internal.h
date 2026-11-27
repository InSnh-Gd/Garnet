// garnet_rt_internal.h - Shared internal declarations across GarnetMetal's ray-tracing
// acceleration-structure compilation units (garnet_rt_accel.m, garnet_rt_plan.m, garnet_rt_encoder.m).
#ifndef GARNET_RT_INTERNAL_H
#define GARNET_RT_INTERNAL_H

#include "garnet_rt_spv_abi.h"

/* Lazily resolved VK_KHR_acceleration_structure + core query-copy entry points. */
typedef struct {
    BOOL ready;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes;
    PFN_vkCreateAccelerationStructureKHR createAS;
    PFN_vkDestroyAccelerationStructureKHR destroyAS;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getASAddress;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAS;
    PFN_vkCmdCopyAccelerationStructureKHR cmdCopyAS;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteASProps;
    PFN_vkCmdCopyQueryPoolResults cmdCopyQueryPoolResults;
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer;
} RDNA4RTFnTable;

GARNET_INTERNAL_EXTERN RDNA4RTFnTable RDNA4RTFn;

GARNET_INTERNAL_EXTERN BOOL rdna4_rt_resolve_funcs(void);
GARNET_INTERNAL_EXTERN void rdna4_rt_emit_barrier(
    VkCommandBuffer cmd, rdna4rt_barrier_kind_t kind, BOOL isComputeQueue);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_ensure_compute_pipes(
    VkPipelineLayout *instLayoutOut, VkPipeline *instPipeOut,
    VkPipelineLayout *xformLayoutOut, VkPipeline *xformPipeOut,
    NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_ensure_curve_pipe(
    VkPipelineLayout *curveLayoutOut, VkPipeline *curvePipeOut, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_ensure_motion_pipes(
    VkPipelineLayout *mBlasLayoutOut, VkPipeline *mBlasPipeOut,
    VkPipelineLayout *mTlasLayoutOut, VkPipeline *mTlasPipeOut,
    NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_cmd_write_buf_bytes(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb, VkBuffer dstBuf,
    uint64_t dstOffset, const void *data, uint64_t bytes, NSError **err);
GARNET_INTERNAL_EXTERN VkDeviceAddress rdna4_rt_ensure_unit_box_blas(NSError **err);

typedef struct {
    int isCurve;
    rdna4rt_curve_geom_t desc;
    uint32_t subSplits;
    uint32_t aabbCount;
    uint32_t vkGeomFlags;
} RDNA4RTCurvePlanEntry;

typedef struct {
    int isMotion;
    uint32_t geomKind;
    uint32_t keyframeCount;
    uint32_t primCount;
    uint32_t startBorder;
    uint32_t endBorder;
    float startTime;
    float endTime;
    uint32_t primStride;
    uint32_t flags;
    uint64_t payloadBytes;
} RDNA4RTMotionGeomPlanEntry;

typedef struct {
    rdna4rt_kind_t kind;
    uint64_t usage;
    uint32_t vkBuildFlags;
    uint32_t geomCount;
    uint32_t geomIftBase;
    uint64_t topologyHash;
    VkAccelerationStructureGeometryKHR *vkGeoms;
    VkAccelerationStructureBuildRangeInfoKHR *vkRanges;
    uint32_t *maxPrimCounts;
    rdna4rt_prim_data_geom_plan_t *primPlans;
    VkBuffer *primSrcVkBufs;
    uint64_t *primSrcVkOffs;
    RDNA4RTCurvePlanEntry *curvePlans;
    RDNA4RTMotionGeomPlanEntry *motionPlans;
    VkBuffer tlasInstStgBuf;
    uint32_t tlasInstanceCount;
    VkAccelerationStructureBuildSizesInfoKHR vkSizes;
    rdna4rt_sizes_t metalSizes;
    BOOL hasInstDispatch;
    BOOL hasCurves;
    BOOL hasMotionGeoms;
    BOOL hasMotionTlas;
    rdna4rt_inst_convert_pc_t instPC;
    rdna4rt_xform_transpose_pc_t *xformPCs;
    uint32_t xformDispatchCount;
    rdna4rt_curve_build_pc_t *curvePCs;
    uint32_t curveDispatchCount;
    rdna4rt_motion_blas_build_pc_t *motionBlasPCs;
    uint32_t *motionBlasGeomIdx;
    uint32_t motionBlasDispatchCount;
    VkBuffer localAabbBuf;
    VkDeviceAddress localAabbAddr;
    rdna4rt_motion_tlas_build_pc_t motionTlasPC;
    rdna4rt_xform_type_t motionTlasXformType;
    VkBuffer motionTlasXformVkBuf;
    uint64_t motionTlasXformVkOff;
    uint32_t motionTlasXformCount;
} RDNA4RTBuildPlan;

GARNET_INTERNAL_EXTERN void rdna4_rt_plan_free(RDNA4RTBuildPlan *p);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_dispatch_pre_build(
    VkCommandBuffer cmd, const RDNA4RTBuildPlan *plan, BOOL isComputeQueue, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_check_buffer(
    id obj, id dev, BOOL writable, RDNA4Buffer **outBuf, NSString **why);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_build_plan_make_ex(
    id dev, MTLAccelerationStructureDescriptor *desc,
    BOOL requireBuffers, BOOL validatePrimData, BOOL collectPrimBuffers,
    BOOL useOverrideUsage, uint64_t overrideUsage,
    NSMutableArray *retainedObjects,
    NSMutableArray<GarnetASPrimData *> *tlasBlasPrimDatas,
    RDNA4RTBuildPlan *plan, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_build_plan_make(
    id dev, MTLAccelerationStructureDescriptor *desc, BOOL requireBuffers,
    BOOL useOverrideUsage, uint64_t overrideUsage,
    NSMutableArray *retainedObjects,
    RDNA4RTBuildPlan *plan, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_record_blas_prim_snapshot(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    const RDNA4RTBuildPlan *plan, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_prepare_blas_sidecar(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    RDNA4RTBuildPlan *plan, GarnetASPrimData *cloneSrcPd,
    BOOL updPrim, BOOL cloneTailCurveSegs,
    GarnetASPrimData **outPd, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_prepare_motion_tlas_sidecar(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    RDNA4RTBuildPlan *plan, NSArray<GarnetASPrimData *> *referencedBlasPrimData,
    GarnetASPrimData **outPd, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_clone_local_aabb(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *srcAS, GarnetAccelerationStructure *dstAS, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_record_tlas_prim_snapshot(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    const RDNA4RTBuildPlan *plan, NSArray<GarnetASPrimData *> *referencedBlasPrimData,
    NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_record_prim_data_clone(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *srcAS,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    NSError **err);

#endif /* GARNET_RT_INTERNAL_H */
