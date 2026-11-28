// garnet_icb_internal.h - Shared internal declarations across GarnetMetal's indirect
// command buffer compilation units (garnet_icb.m, garnet_icb_execute.m).
#ifndef GARNET_ICB_INTERNAL_H
#define GARNET_ICB_INTERNAL_H

#ifndef GARNET_INTERNAL_EXTERN
#define GARNET_INTERNAL_EXTERN static
#endif

// Mac2 requires executeCommandsInBuffer(_:range:) to contain at most
// 0x4000 (16,384) commands; Apple silicon families have no such limit.
// Source: Apple, executeCommandsInBuffer(_:range:) (accessed 2026-11-05):
// https://developer.apple.com/documentation/metal/mtlrendercommandencoder/executecommandsinbuffer(_:range:)
#ifdef GARNET_METAL_INTERNAL_H
#include "garnet_commands_internal.h"
#endif

#define RDNA4ICB_MAX_EXECUTION_RANGE ((NSUInteger)0x4000)
#define RDNA4ICB_MAX_BUFFER_BINDINGS ((NSUInteger)31)
#define RDNA4ICB_MAX_TG_MEM_INDICES ((NSUInteger)32)
#define RDNA4ICB_TYPE_DRAW_PATCHES ((MTLIndirectCommandType)(1u << 2))
#define RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES ((MTLIndirectCommandType)(1u << 3))
#define RDNA4ICB_TYPE_CONCURRENT_DISPATCH ((MTLIndirectCommandType)(1u << 5))
#define RDNA4ICB_TYPE_CONCURRENT_DISPATCH_THREADS ((MTLIndirectCommandType)(1u << 6))
#define RDNA4ICB_RENDER_TYPES (MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed | RDNA4ICB_TYPE_DRAW_PATCHES | RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES)
#define RDNA4ICB_COMPUTE_TYPES (RDNA4ICB_TYPE_CONCURRENT_DISPATCH | RDNA4ICB_TYPE_CONCURRENT_DISPATCH_THREADS)

typedef NS_ENUM(NSUInteger, RDNA4ICBCommandKind) {
    RDNA4ICBCommandEmpty = 0,
    RDNA4ICBCommandDraw,
    RDNA4ICBCommandDrawIndexed,
    RDNA4ICBCommandDrawPatches,
    RDNA4ICBCommandDrawIndexedPatches,
    RDNA4ICBCommandConcurrentDispatch,
    RDNA4ICBCommandConcurrentDispatchThreads,
};

@class RDNA4ICBBind;
@class RDNA4ICBRecord;
@class RDNA4IndirectCommandBuffer;
@class RDNA4IndirectRenderCommand;
@class RDNA4IndirectComputeCommand;
@class RDNA4CommandBuffer;

#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4CommandBuffer (RDNA4OrderedHostAction)
- (id<MTLDevice>)device;
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error;
#ifdef VK_VERSION_1_0
- (void)rdna4RetainEncoded:(id)object;
- (VkCommandBuffer)vk;
- (BOOL)rdna4HasArgumentWrites;
- (void)rdna4ArgumentCheckpoint;
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l;
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm;
- (BOOL)rdna4PrepareArguments:(const RDNA4ArgumentTables *)tables resources:(NSSet *)resources writable:(NSSet *)writable scan:(const RDNA4ArgumentScanPrepared *)scan textures:(void (^)(RDNA4Texture *, VkImageLayout, BOOL))prepare;
- (void)rdna4CopyArgumentSets:(VkDescriptorSet [3])outSets;
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo;
#endif
@end
#endif

#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4RenderEncoder (RDNA4IndirectCommands)
- (id)rdna4ICBDevice;
- (RDNA4CommandBuffer *)rdna4ICBCommandBuffer;
- (RDNA4RenderPipelineState *)rdna4ICBPipeline;
- (BOOL)rdna4ICBCompatiblePipeline:(id)pipeline;
- (BOOL)rdna4ICBInheritedBuffersSatisfyPipeline:(id)pipeline;
- (BOOL)rdna4ICBExplicitBuffersSatisfyRecord:(RDNA4ICBRecord *)record pipeline:(id)pipeline;
- (void)rdna4ICBClearExplicitBuffers;
- (void)rdna4ICBRetainResource:(id)resource;
@end
#endif

#ifdef VK_VERSION_1_0
#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4RenderEncoder (RDNA4InternalAccess)
- (void)rdna4SyncResourceSegment;
- (void)rdna4SnapshotResources;
- (void)rdna4EndPass;
- (void)rdna4BeginPass;
- (void)rdna4OccDraw;
- (void)rdna4Need:(RDNA4Texture *)t layout:(VkImageLayout)l;
- (void)rdna4EachTex:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dev:(id)dev;
- (void)setTessellationFactorBuffer:(id)b offset:(NSUInteger)off instanceStride:(NSUInteger)st;
- (void)drawPatches:(NSUInteger)cp patchStart:(NSUInteger)ps patchCount:(NSUInteger)pc patchIndexBuffer:(id)pib patchIndexBufferOffset:(NSUInteger)pio instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi;
- (void)drawIndexedPatches:(NSUInteger)cp patchStart:(NSUInteger)ps patchCount:(NSUInteger)pc patchIndexBuffer:(id)pib patchIndexBufferOffset:(NSUInteger)pio controlPointIndexBuffer:(id)cpib controlPointIndexBufferOffset:(NSUInteger)cpio instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi;
@end
#endif

#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4ComputeEncoder (RDNA4InternalAccess)
- (void)rdna4SnapshotResources;
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind fragment:(BOOL)fragment index:(NSUInteger)index;
- (void)rdna4ICBComputeBarrier;
@end
#endif

#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4ComputeEncoder (RDNA4IndirectCommands)
- (id)rdna4ICBDevice;
- (RDNA4CommandBuffer *)rdna4ICBCommandBuffer;
- (RDNA4ComputePipelineState *)rdna4ICBComputePipeline;
- (BOOL)rdna4ICBInheritedComputeBuffersSatisfyPipeline:(id)pipeline;
- (BOOL)rdna4ICBExplicitComputeBuffersSatisfyRecord:(RDNA4ICBRecord *)record pipeline:(id)pipeline;
- (void)rdna4ICBClearExplicitComputeBuffers;
- (void)rdna4ICBResetPerSlotComputeState;
- (void)rdna4ICBRetainResource:(id)resource;
- (void)rdna4ExecuteComputeICBGPU:(RDNA4IndirectCommandBuffer *)icb
                      directRange:(NSRange)range
                   indirectBuffer:(id)indirectBuf
                   indirectOffset:(NSUInteger)indirectOff
                      useIndirect:(BOOL)useIndirect;
@end
#endif
#endif

@interface RDNA4ICBBind : NSObject
@property(nonatomic, strong) RDNA4Buffer *buffer;
@property(nonatomic) NSUInteger offset;
- (instancetype)initWithBuffer:(RDNA4Buffer *)buffer offset:(NSUInteger)offset;
@end


@interface RDNA4ICBRecord : NSObject
@property(nonatomic) RDNA4ICBCommandKind kind;
@property(nonatomic, copy) NSString *error;
@property(nonatomic, strong) RDNA4RenderPipelineState *pipeline;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *vertexBinds;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *fragmentBinds;
@property(nonatomic) MTLPrimitiveType primitiveType;
@property(nonatomic) NSUInteger vertexStart;
@property(nonatomic) NSUInteger vertexCount;
@property(nonatomic) NSUInteger instanceCount;
@property(nonatomic) NSUInteger baseInstance;
@property(nonatomic) NSUInteger indexCount;
@property(nonatomic) MTLIndexType indexType;
@property(nonatomic, strong) RDNA4Buffer *indexBuffer;
@property(nonatomic) NSUInteger indexBufferOffset;
@property(nonatomic) NSInteger baseVertex;
@property(nonatomic) NSUInteger patchControlPoints;
@property(nonatomic) NSUInteger patchStart;
@property(nonatomic) NSUInteger patchCount;
@property(nonatomic, strong) RDNA4Buffer *patchIndexBuffer;
@property(nonatomic) NSUInteger patchIndexBufferOffset;
@property(nonatomic, strong) RDNA4Buffer *controlPointIndexBuffer;
@property(nonatomic) NSUInteger controlPointIndexBufferOffset;
@property(nonatomic, strong) RDNA4Buffer *tessellationFactorBuffer;
@property(nonatomic) NSUInteger tessellationFactorBufferOffset;
@property(nonatomic) NSUInteger tessellationFactorInstanceStride;
@property(nonatomic, strong) id computePipeline;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *kernelBinds;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, NSNumber *> *threadgroupMemLengths;
@property(nonatomic) BOOL hasBarrier;
@property(nonatomic) BOOL hasStageInRegion;
@property(nonatomic) MTLRegion stageInRegion;
@property(nonatomic) MTLSize gridSize;
@property(nonatomic) MTLSize threadgroupSize;
- (void)reset;
- (void)reject:(NSString *)reason;
- (RDNA4ICBRecord *)rdna4Copy;
@end

@interface RDNA4IndirectCommandBuffer : NSObject <RDNA4ComputeICBInternal> {
#else
@interface RDNA4IndirectCommandBuffer : NSObject {
#endif
@public
    id _dev;
    NSString *_label;
    MTLResourceOptions _options;
    MTLPurgeableState _purge;
    NSUInteger _commandCount;
    NSUInteger _encodedSize;
    _Atomic uint64_t _uid;
    MTLIndirectCommandBufferDescriptor *_descriptor;
    MTLIndirectCommandType _commandTypes;
    BOOL _isCompute;
    BOOL _inheritPipelineState;
    BOOL _inheritBuffers;
    NSUInteger _maxVertexBufferBindCount;
    NSUInteger _maxFragmentBufferBindCount;
    NSUInteger _maxKernelBufferBindCount;
    NSMutableDictionary<NSNumber *, RDNA4ICBRecord *> *_records;
    NSMutableDictionary<NSNumber *, RDNA4IndirectRenderCommand *> *_commands;
    NSMutableDictionary<NSNumber *, RDNA4IndirectComputeCommand *> *_computeCommands;
    RDNA4ICBRecord *_emptyRecord;
    RDNA4IndirectRenderCommand *_invalidCommand;
    RDNA4IndirectComputeCommand *_invalidComputeCommand;
    NSString *_fatalError;
    pthread_mutex_t _mu;
    BOOL _muReady;
#ifdef VK_VERSION_1_0
    RDNA4Buffer *_gpuBacking;
    BOOL _gpuProduced;
    NSMutableSet<NSNumber *> *_cpuDirtySlots;
    BOOL _cpuFullReset;
    NSMutableArray<NSValue *> *_cpuResetRanges;
    NSMutableArray *_candidatePipelines;
    NSMutableSet *_gpuRetained;
    _Atomic uint64_t _cpuReadbackCount;
#endif
}
- (instancetype)initWithDevice:(id)device
                    descriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
              maxCommandCount:(NSUInteger)maxCommandCount
                       options:(MTLResourceOptions)options
                         error:(NSError **)error;
- (NSUInteger)size;
- (BOOL)rdna4IsComputeICB;
- (RDNA4ICBRecord *)rdna4LockedRecordAtIndex:(NSUInteger)index;
- (void)rdna4SetPipelineState:(id)pipeline atIndex:(NSUInteger)index;
- (void)rdna4SetBuffer:(id)buffer
                offset:(NSUInteger)offset
           bufferIndex:(NSUInteger)bufferIndex
          commandIndex:(NSUInteger)commandIndex
              fragment:(BOOL)fragment;
- (void)rdna4SetComputePipelineState:(id)pipeline atIndex:(NSUInteger)index;
- (void)rdna4SetKernelBuffer:(id)buffer
                      offset:(NSUInteger)offset
                 bufferIndex:(NSUInteger)bufferIndex
                commandIndex:(NSUInteger)commandIndex;
- (void)rdna4SetComputeDispatchAtIndex:(NSUInteger)index
                              gridSize:(MTLSize)gridSize
                       threadgroupSize:(MTLSize)tgSize
                          exactThreads:(BOOL)exactThreads;
- (void)rdna4SetBarrier:(BOOL)enable atIndex:(NSUInteger)index;
- (void)rdna4SetThreadgroupMemoryLength:(NSUInteger)length
                                atIndex:(NSUInteger)tgIndex
                           commandIndex:(NSUInteger)commandIndex;
- (void)rdna4SetStageInRegion:(MTLRegion)region atIndex:(NSUInteger)index;
- (void)rdna4RejectComputeAtIndex:(NSUInteger)index reason:(NSString *)reason;
- (MTLIndirectCommandType)rdna4CommandTypeAtIndex:(NSUInteger)index;
- (BOOL)rdna4HasBarrierAtIndex:(NSUInteger)index;
- (NSUInteger)rdna4ThreadgroupMemoryLengthAtIndex:(NSUInteger)tgIndex commandIndex:(NSUInteger)index;
- (MTLRegion)rdna4StageInRegionAtIndex:(NSUInteger)index;
- (MTLSize)rdna4GridSizeAtIndex:(NSUInteger)index;
- (id)rdna4ComputePipelineAtIndex:(NSUInteger)index;
- (id)rdna4KernelBufferAtIndex:(NSUInteger)bufferIndex commandIndex:(NSUInteger)index;
- (void)rdna4SetDrawAtIndex:(NSUInteger)index
               primitiveType:(MTLPrimitiveType)primitiveType
                 vertexStart:(NSUInteger)vertexStart
                 vertexCount:(NSUInteger)vertexCount
               instanceCount:(NSUInteger)instanceCount
                baseInstance:(NSUInteger)baseInstance;
- (void)rdna4SetIndexedDrawAtIndex:(NSUInteger)index
                      primitiveType:(MTLPrimitiveType)primitiveType
                         indexCount:(NSUInteger)indexCount
                          indexType:(MTLIndexType)indexType
                        indexBuffer:(id)indexBuffer
                  indexBufferOffset:(NSUInteger)indexBufferOffset
                      instanceCount:(NSUInteger)instanceCount
                         baseVertex:(NSInteger)baseVertex
                       baseInstance:(NSUInteger)baseInstance;
- (void)rdna4SetPatchDrawAtIndex:(NSUInteger)index
                 patchControlPoints:(NSUInteger)patchControlPoints
                        patchStart:(NSUInteger)patchStart
                        patchCount:(NSUInteger)patchCount
                    patchIndexBuffer:(id)patchIndexBuffer
              patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
                     instanceCount:(NSUInteger)instanceCount
                      baseInstance:(NSUInteger)baseInstance
            tessellationFactorBuffer:(id)tessellationFactorBuffer
      tessellationFactorBufferOffset:(NSUInteger)tessellationFactorBufferOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)tessellationFactorBufferInstanceStride
                           indexed:(BOOL)indexed
            controlPointIndexBuffer:(id)controlPointIndexBuffer
      controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset;
- (BOOL)rdna4RecordCanCommitDraw:(RDNA4ICBRecord *)record
                        commandType:(MTLIndirectCommandType)type
                            problem:(NSString **)problem;
- (void)rdna4ResetOneAtIndex:(NSUInteger)index;
- (BOOL)rdna4RecordResetRange:(NSRange)range
                commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                       reason:(NSString **)reason;
- (BOOL)rdna4RecordCopyRange:(NSRange)sourceRange
                ofBuffer:(RDNA4IndirectCommandBuffer *)source
           toDestination:(RDNA4IndirectCommandBuffer *)destination
        destinationIndex:(NSUInteger)destinationIndex
               commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                  reason:(NSString **)reason;
- (BOOL)rdna4ValidateRange:(NSRange)range reason:(NSString **)reason;
#ifdef VK_VERSION_1_0
- (BOOL)rdna4SyncCPUWritesToGPU:(NSError **)err;
- (uint64_t)rdna4ICBCPUReadbackCount;
#endif
- (BOOL)rdna4PublishResetRangeLocked:(NSRange)range error:(NSError **)error;
- (BOOL)rdna4PublishCopyRangeLocked:(NSRange)sourceRange
                           toBuffer:(RDNA4IndirectCommandBuffer *)destination
                   destinationIndex:(NSUInteger)destinationIndex
                              error:(NSError **)error;
- (BOOL)rdna4ApplyResetRangeAtExecution:(NSRange)range error:(NSError **)error;
- (BOOL)rdna4ApplyCopyRangeAtExecution:(NSRange)sourceRange
                                toBuffer:(RDNA4IndirectCommandBuffer *)destination
                        destinationIndex:(NSUInteger)destinationIndex
                                   error:(NSError **)error;
- (NSUInteger)rdna4ExecuteOnEncoder:(RDNA4RenderEncoder *)encoder
                              range:(NSRange)range
                             reason:(NSString **)reason;
- (BOOL)rdna4SameConfigurationAs:(RDNA4IndirectCommandBuffer *)other;
@end

@interface RDNA4IndirectRenderCommand : NSObject {
@private
    __weak RDNA4IndirectCommandBuffer *_owner;
    NSUInteger _index;
    BOOL _validIndex;
}
- (instancetype)initWithOwner:(RDNA4IndirectCommandBuffer *)owner index:(NSUInteger)index valid:(BOOL)valid;
- (void)rdna4Reject:(NSString *)reason;
- (BOOL)rdna4CanMutate;
@end

@interface RDNA4IndirectComputeCommand : NSObject {
@private
    __weak RDNA4IndirectCommandBuffer *_owner;
    NSUInteger _index;
    BOOL _validIndex;
}
- (instancetype)initWithOwner:(RDNA4IndirectCommandBuffer *)owner index:(NSUInteger)index valid:(BOOL)valid;
- (void)rdna4Reject:(NSString *)reason;
- (BOOL)rdna4CanMutate;
@end

GARNET_INTERNAL_EXTERN NSString *rdna4icb_range_problem(NSRange range, NSUInteger count, BOOL execution);
GARNET_INTERNAL_EXTERN BOOL rdna4icb_stage_buffers_ok(const RDNA4PB *bindings, uint32_t count,
                                                      const RDNA4StageState *state, id device);
GARNET_INTERNAL_EXTERN BOOL rdna4icb_read_uint(id object, SEL selector, NSUInteger *value);
#ifdef VK_VERSION_1_0
GARNET_INTERNAL_EXTERN BOOL rdna4_icb_get_compute_expander(VkDescriptorSetLayout *outDsl, VkPipelineLayout *outPl, VkPipeline *outPipe, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_icb_get_expander(VkDescriptorSetLayout *outDsl, VkPipelineLayout *outPl, VkPipeline *outPipe, NSError **err);
#endif

#endif /* GARNET_ICB_INTERNAL_H */
