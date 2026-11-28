// garnet_rt_internal.h - Shared internal declarations across GarnetMetal's ray-tracing
// acceleration-structure compilation units (garnet_rt_accel.m, garnet_rt_plan.m, garnet_rt_encoder.m).
#ifndef GARNET_RT_INTERNAL_H
#define GARNET_RT_INTERNAL_H

#include "garnet_rt_accel.h"
#include "garnet_rt_spv_abi.h"

@protocol GarnetAccelerationStructureRuntimeInternal <NSObject>
- (id)device;
- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure;
- (VkDeviceAddress)rdna4DeviceAddress;
- (VkDeviceAddress)rdna4BindlessGPUAddress;
- (id)rdna4CurrentHandle;
- (id)rdna4CurrentPrimData;
- (id)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (uint32_t)rdna4AccelKind;
- (uint32_t)rdna4GeomIftBase;
- (BOOL)rdna4IsBuilt;
@end

@interface RDNA4IntersectionFunctionTable : _MTLResource {
    id _dev;
    NSString *_lbl;
    uint64_t _ownerToken;
    NSUInteger _stage;
    rdna4rtr_ift_table_t _table;
    uint8_t *_handleKinds; // indexed by handle_id (1.._table.max_handle_id), 0xFF = invalid
    RDNA4Buffer *_backing;
    // shortcut: 0077 Stage 3 lowers intersection function data through shader buffer bindings and
    // argument buffers; table-attached buffers/VFTs are validated, stored, and retained here until
    // metal2vulkan adds per-IFT buffer-table lowering (upgrade to a secondary BDA sidecar buffer then).
    RDNA4Buffer *_attachedBuffers[RDNA4RTR_MAX_ATTACH_BUFFERS];
    NSUInteger _attachedBufferOffsets[RDNA4RTR_MAX_ATTACH_BUFFERS];
    id _nestedVFTs[RDNA4RTR_MAX_NESTED_VFTS];
    NSLock *_lock;
    RDNA4_RES_IVARS
}
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                          refl:(const rdna4rtr_ift_reflection_t *)refl
                         error:(NSError **)error;
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor
                    ownerToken:(uint64_t)token
                          refl:(const rdna4rtr_ift_reflection_t *)refl;
- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index;
- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range;
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                   atIndex:(NSUInteger)index;
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                 withRange:(NSRange)range;
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                atIndex:(NSUInteger)index;
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                              withRange:(NSRange)range;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range;
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable
                  atBufferIndex:(NSUInteger)bufferIndex;
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables
                 withBufferRange:(NSRange)bufferRange;
- (RDNA4Buffer *)rdna4BackingBuffer;
- (uint64_t)rdna4BackingGPUAddress;
- (NSUInteger)rdna4FunctionCount;
- (uint64_t)rdna4OwnerToken;
- (NSUInteger)rdna4Stage;
- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index;
- (rdna4rtr_ift_slot_mode_t)rdna4SlotModeAtIndex:(NSUInteger)index;
- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut;
- (id)rdna4NestedVFTAtBufferIndex:(NSUInteger)index;
- (void)rdna4RetainIntoCommandBuffer:(id)cb;
@end

@interface RDNA4RTArgOwners : NSObject
@property NSMutableSet *accelStructs;
@property NSMutableSet *iftTables;
@property NSMutableSet *nestedBuffers;
@end

@interface RDNA4RTStagingBuffer : NSObject {
@public
    VkBuffer _buf;
    VkDeviceMemory _mem;
    void *_map;
    VkDeviceAddress _addr;
    uint64_t _size;
    uint64_t _allocSize;
    BOOL _pooled;
}
- (instancetype)initWithSize:(uint64_t)size error:(NSError **)err;
@end

@interface RDNA4RTQueryPool : NSObject {
@public
    VkQueryPool _qp;
}
- (instancetype)initCompactedSizePoolWithError:(NSError **)err;
@end

@interface GarnetASPrimData : NSObject {
@public
    VkBuffer _buf;
    VkDeviceMemory _mem;
    VkDeviceAddress _bda;
    uint64_t _totalBytes;
    uint64_t _capacity;
    uint64_t _allocatedSize;
    BOOL _pooled;
    uint32_t _kind;
    uint32_t _count;
    uint32_t _entryStride;
    uint32_t _refOffset;
    rdna4rt_geom_prim_entry_t *_blasHeaders;
    NSArray<GarnetASPrimData *> *_referencedPrimData;
}
- (instancetype)initWithSize:(uint64_t)totalBytes kind:(uint32_t)kind count:(uint32_t)count
                 entryStride:(uint32_t)entryStride refOffset:(uint32_t)refOffset error:(NSError **)err;
@end

@interface GarnetASStorage : NSObject {
@public
    VkBuffer _buf;
    VkDeviceMemory _mem;
    VkDeviceAddress _deviceAddress;
    uint64_t _backingSize;
    uint64_t _allocatedSize;
    RDNA4Heap *_hpHeap;
    NSUInteger _hpOff, _hpSize;
    uint64_t _hpId, _hpBlk, _hpLease, _hpBindOff, _hpPhysSize;
    BOOL _hpDedicated;
    RDNA4HeapBacking *_hpBacking;
}
- (instancetype)initWithBackingSize:(uint64_t)backingSize error:(NSError **)err;
- (instancetype)initWithBackingSize:(uint64_t)backingSize heap:(RDNA4Heap *)heap
                             offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err;
@end

@interface GarnetASHandle : NSObject {
@public
    GarnetASStorage *_storage;
    VkAccelerationStructureKHR _vkAS;
    VkDeviceAddress _deviceAddress;
    rdna4rt_kind_t _kind;
    RDNA4RTStagingBuffer *_localAabbBuf;
}
- (instancetype)initWithStorage:(GarnetASStorage *)storage kind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (VkDeviceAddress)rdna4EnsureLocalAabbAddress:(NSError **)err;
@end

@interface GarnetAccelerationStructure : _MTLResource {
    id _dev;
    NSString *_lbl;
    NSUInteger _size;
    GarnetASStorage *_storage;
    GarnetASHandle *_handle;
    GarnetASPrimData *_primData;
    uint64_t _registeredAsBda;
    uint64_t _registeredPayloadBda;
    rdna4rt_accel_meta_t _meta;
    pthread_mutex_t _mu;
    BOOL _muInit;
    RDNA4_RES_IVARS
}
- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind
                          heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset
                         error:(NSError **)err;
- (GarnetASHandle *)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (GarnetASHandle *)rdna4CurrentHandle;
- (GarnetASPrimData *)rdna4CurrentPrimData;
- (BOOL)rdna4SetPrimData:(GarnetASPrimData *)primData forAsAddress:(uint64_t)asBda
           commandBuffer:(RDNA4CommandBuffer *)cb vkCmdBuffer:(VkCommandBuffer)cmd error:(NSError **)err;
- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure;
- (VkDeviceAddress)rdna4DeviceAddress;
- (VkDeviceAddress)rdna4BindlessGPUAddress;
- (rdna4rt_accel_meta_t)rdna4MetaSnapshot;
- (void)rdna4SetMeta:(rdna4rt_accel_meta_t)meta;
- (uint32_t)rdna4AccelKind;
- (uint32_t)rdna4GeomIftBase;
- (BOOL)rdna4IsBuilt;
- (BOOL)rdna4AllowsRefit;
- (BOOL)rdna4AllowsCompaction;
- (RDNA4Heap *)rdna4ResourceHeap;
- (NSUInteger)rdna4ResourceHeapOffset;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
- (uint64_t)rdna4BackingBlock;
- (uint64_t)rdna4BackingOffset;
- (uint64_t)rdna4BackingSize;
@end

@interface GarnetAccelerationStructureCommandEncoder : _MTLCommandEncoder {
    RDNA4CommandBuffer *_cb;
    BOOL _ended;
    NSString *_lbl;
    id _csbEnd[4];
    NSUInteger _csbEndIdx[4];
    uint32_t _nCsbEnd;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)d;
@end

@interface GarnetAccelerationStructure (GarnetInternalMethods)
+ (void)initialize;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)size;
- (NSUInteger)allocatedSize;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (NSUInteger)resourceOptions;
- (id)heap;
- (NSUInteger)heapOffset;
- (BOOL)isAliasable;
- (void)makeAliasable;
- (NSUInteger)setPurgeableState:(NSUInteger)st;
- (BOOL)isPurgeable;
- (int)setOwnerWithIdentity:(uint32_t)o;
- (BOOL)doesAliasResource:(id)o;
- (BOOL)doesAliasAnyResources:(const id __unsafe_unretained *)o count:(NSUInteger)n;
- (BOOL)doesAliasAllResources:(const id __unsafe_unretained *)o count:(NSUInteger)n;
- (BOOL)isComplete;
- (BOOL)isWriteComplete;
- (void)waitUntilComplete;
- (NSUInteger)protectionOptions;
- (int)responsibleProcess;
- (void)setResponsibleProcess:(int)p;
- (NSUInteger)unfilteredResourceOptions;
- (uint64_t)uniqueIdentifier;
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0));
@end

@interface GarnetAccelerationStructureCommandEncoder (GarnetInternalMethods)
+ (void)initialize;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)getType;
- (BOOL)rdna4GuardReady:(const char *)who;
- (void)buildAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure descriptor:(MTLAccelerationStructureDescriptor *)descriptor scratchBuffer:(id<MTLBuffer>)scratchBuffer scratchBufferOffset:(NSUInteger)scratchBufferOffset;
- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure descriptor:(MTLAccelerationStructureDescriptor *)descriptor destination:(id<MTLAccelerationStructure>)destinationAccelerationStructure scratchBuffer:(id<MTLBuffer>)scratchBuffer scratchBufferOffset:(NSUInteger)scratchBufferOffset;
- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure descriptor:(MTLAccelerationStructureDescriptor *)descriptor destination:(id<MTLAccelerationStructure>)destinationAccelerationStructure scratchBuffer:(id<MTLBuffer>)scratchBuffer scratchBufferOffset:(NSUInteger)scratchBufferOffset options:(NSUInteger)options;
- (void)copyAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure toAccelerationStructure:(id<MTLAccelerationStructure>)destinationAccelerationStructure;
- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)accelerationStructure toBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset;
- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)accelerationStructure toBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset sizeDataType:(MTLDataType)sizeDataType;
- (void)copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure toAccelerationStructure:(id<MTLAccelerationStructure>)destinationAccelerationStructure;
- (void)updateFence:(id)fence;
- (void)waitForFence:(id)fence;
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage;
- (void)useResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage;
- (void)useHeap:(id)heap;
- (void)useHeaps:(const id __unsafe_unretained *)heaps count:(NSUInteger)count;
- (void)sampleCountersInBuffer:(id)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier;
- (void)insertDebugSignpost:(NSString *)s;
- (void)pushDebugGroup:(NSString *)s;
- (void)popDebugGroup;
- (void)endEncoding;
@end

@interface RDNA4IntersectionFunctionTable (GarnetInternalMethods)
+ (void)initialize;
- (BOOL)rdna4SyncBackingLocked;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor ownerToken:(uint64_t)token stage:(NSUInteger)stage refl:(const rdna4rtr_ift_reflection_t *)refl error:(NSError **)error;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor ownerToken:(uint64_t)token refl:(const rdna4rtr_ift_reflection_t *)refl;
- (BOOL)rdna4ValidateHandleLocked:(id)function handleIdOut:(uint32_t *)hidOut kindOut:(uint32_t *)kindOut;
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature atIndex:(NSUInteger)index;
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature withRange:(NSRange)range;
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature atIndex:(NSUInteger)index;
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature withRange:(NSRange)range;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex;
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables withBufferRange:(NSRange)bufferRange;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (MTLResourceOptions)resourceOptions;
- (NSUInteger)allocatedSize;
- (RDNA4Heap *)rdna4ResourceHeap;
- (NSUInteger)rdna4ResourceHeapOffset;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
- (id)heap;
- (NSUInteger)heapOffset;
- (BOOL)isAliasable;
- (void)makeAliasable;
- (NSUInteger)setPurgeableState:(NSUInteger)st;
- (BOOL)isPurgeable;
- (int)setOwnerWithIdentity:(uint32_t)o;
- (BOOL)doesAliasResource:(id)o;
- (BOOL)doesAliasAnyResources:(const id __unsafe_unretained *)o count:(NSUInteger)n;
- (BOOL)doesAliasAllResources:(const id __unsafe_unretained *)o count:(NSUInteger)n;
- (BOOL)isComplete;
- (BOOL)isWriteComplete;
- (void)waitUntilComplete;
- (NSUInteger)protectionOptions;
- (int)responsibleProcess;
- (void)setResponsibleProcess:(int)p;
- (NSUInteger)unfilteredResourceOptions;
- (uint64_t)uniqueIdentifier;
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0));
@end

@interface RDNA4RTArgOwners (GarnetInternalMethods)
- (instancetype)init;
@end

GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_raytracing(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_raytracing_from_render(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_primitive_motion_blur(void);
GARNET_INTERNAL_EXTERN uint32_t rdna4_rt_descriptor_pool_append(
    VkDescriptorPoolSize *sizes,
    uint32_t count,
    uint32_t capacity,
    uint32_t divisor);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_encoder_bind_accel(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    id accel,
    NSUInteger bufferIndex);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_encoder_bind_ift(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    id ift,
    NSUInteger bufferIndex,
    RDNA4Buffer * __autoreleasing *backingBufOut);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_encoder_bind_ifts(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    const id<MTLIntersectionFunctionTable> __unsafe_unretained *tables,
    NSRange range,
    void (^bindBacking)(RDNA4Buffer *buf, NSUInteger bufferIndex));
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_fill_accel_descriptor(
    id cb,
    id dev,
    const char *stageName,
    rdna4rtr_stage_t stage,
    uint32_t binding,
    uint32_t count,
    const RDNA4RTStageSlots *rtSlots,
    const rdna4rtr_as_reflection_t *asRefl,
    VkWriteDescriptorSet *dstWrite,
    VkWriteDescriptorSetAccelerationStructureKHR *asWriteSlot,
    VkAccelerationStructureKHR *asHandlesOut,
    uint32_t *asHandleCursor,
    uint32_t asHandleCap);
GARNET_INTERNAL_EXTERN void rdna4_rt_dc_push_accel_sig(
    rdna4dc_sig *sig,
    uint32_t binding,
    uint32_t count,
    const RDNA4RTStageSlots *rtSlots);
GARNET_INTERNAL_EXTERN void rdna4_rt_arg_note(
    RDNA4Buffer *dst, id accelStruct, RDNA4IntersectionFunctionTable *ift, RDNA4Buffer *nestedBuf);
GARNET_INTERNAL_EXTERN void rdna4_rt_retain_argbuf_resources(RDNA4Buffer *buf, id cb);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_arg_is_rt_data_type(MTLDataType dt);
GARNET_INTERNAL_EXTERN uint64_t rdna4_rt_arg_resolve_as_address(
    id device,
    id accel,
    MTLDataType fieldType,
    NSUInteger fieldByteOffset,
    NSUInteger fieldSize,
    NSUInteger bufLength);
GARNET_INTERNAL_EXTERN uint64_t rdna4_rt_arg_resolve_ift_address(
    id device,
    id ift,
    MTLDataType fieldType,
    NSUInteger fieldByteOffset,
    NSUInteger fieldSize,
    NSUInteger bufLength);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_parse_meta(
    id meta, rdna4rtr_reflection_t *out, NSError **err);
GARNET_INTERNAL_EXTERN id<MTLFunctionHandle> rdna4_rt_pipeline_function_handle(
    id pso, id dev, uint64_t iftOwnerToken, const rdna4rtr_ift_reflection_t *refl,
    id<MTLFunction> function, NSUInteger stage);
GARNET_INTERNAL_EXTERN id<MTLIntersectionFunctionTable> rdna4_rt_pipeline_new_ift(
    id pso, id dev, uint64_t iftOwnerToken, const rdna4rtr_ift_reflection_t *refl,
    MTLIntersectionFunctionTableDescriptor *descriptor, NSUInteger stage);
GARNET_INTERNAL_EXTERN MTLAccelerationStructureSizes rdna4_rt_device_sizes(
    id dev, MTLAccelerationStructureDescriptor *descriptor);
GARNET_INTERNAL_EXTERN id<MTLAccelerationStructure> rdna4_rt_device_new_with_size(
    id dev, NSUInteger size);
GARNET_INTERNAL_EXTERN id<MTLAccelerationStructure> rdna4_rt_device_new_with_descriptor(
    id dev, MTLAccelerationStructureDescriptor *descriptor);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_fill_stage_prim_table(
    id cb, id dev, const RDNA4RTStageSlots *rtSlots, rdna4rt_stage_prim_table_t *out);
GARNET_INTERNAL_EXTERN MTLSizeAndAlign rdna4_rt_device_heap_as_size_align_with_size(
    id dev, NSUInteger size);
GARNET_INTERNAL_EXTERN MTLSizeAndAlign rdna4_rt_device_heap_as_size_align_with_descriptor(
    id dev, MTLAccelerationStructureDescriptor *descriptor);

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

enum { RDNA4RT_INLINE_GEOMS = 4 };

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
    struct {
        VkAccelerationStructureGeometryKHR vkGeoms[RDNA4RT_INLINE_GEOMS];
        VkAccelerationStructureBuildRangeInfoKHR vkRanges[RDNA4RT_INLINE_GEOMS];
        uint32_t maxPrimCounts[RDNA4RT_INLINE_GEOMS];
        rdna4rt_prim_data_geom_plan_t primPlans[RDNA4RT_INLINE_GEOMS];
        VkBuffer primSrcVkBufs[RDNA4RT_INLINE_GEOMS];
        uint64_t primSrcVkOffs[RDNA4RT_INLINE_GEOMS];
        RDNA4RTCurvePlanEntry curvePlans[RDNA4RT_INLINE_GEOMS];
        RDNA4RTMotionGeomPlanEntry motionPlans[RDNA4RT_INLINE_GEOMS];
    } small;
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
