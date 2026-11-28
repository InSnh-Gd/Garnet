// garnet_commands_internal.h - Shared internal declarations across GarnetMetal's
// command queue, command buffer, counter, render, compute, and blit compilation units.
#ifndef GARNET_COMMANDS_INTERNAL_H
#define GARNET_COMMANDS_INTERNAL_H

#ifndef GARNET_INTERNAL_EXTERN
#define GARNET_INTERNAL_EXTERN static
#endif

#include "garnet_pipelines_internal.h"
#include "garnet_counters.h"

@interface RDNA4CommandSegment : NSObject {
@public
    VkCommandBuffer vk, linearInit, dispCopyVk;
    BOOL dispCopyPending, dispCopyVramImg;
    rdna4df_plan_t dispCopyPlan;
    RDNA4Texture *dispCopyTex;
    VkImageLayout dispCopySavedLayout;
    VkFence fence;
    NSMutableArray *waits;
    NSMutableArray *signals;
    NSMutableArray *fenceWaits;
    NSMutableArray *fenceUpdates;
    NSMutableArray *touches;   // IOSurface roots first touched in this segment
    NSMutableArray *argumentIntents;
    NSMutableSet *argumentRefs;
    NSMutableSet *commandRefs;
    const void *recentArgRefs[8];
    const void *recentCmdRefs[8];
    uint32_t recentArgPos, recentCmdPos;
    NSMutableArray *argumentPools;
    NSMutableArray *descriptorPools;
    NSArray *argumentSummaryAttempts;
    BOOL ended, submitted, retirementUnknown, done, hasWork, pruned, terminal, isComputeQueue;
    uint64_t argumentSubmissionEpoch;
    NSUInteger hostWritebackCopies;
    NSUInteger hostLinearWrites;
    NSMutableIndexSet *displaySourceWrites, *displaySourceReads;
    NSMutableDictionary<NSNumber *, RDNA4Texture *> *displaySourceOverwrites;
    NSHashTable<RDNA4Texture *> *displayImageWrites, *displayImageReads, *displayImageOverwrites;
    BOOL hostOnly, hostActionAccepted, hostActionRunning;
    BOOL (^hostAction)(NSError **);
}
@end

@interface RDNA4Counter : NSObject {
    NSString *_name;
}
- (instancetype)initWithName:(NSString *)name;
- (NSString *)name;
@end

@interface RDNA4CounterSet : NSObject {
    NSString *_name;
    NSArray<id<MTLCounter>> *_counters;
}
- (instancetype)initTimestampSet;
- (NSString *)name;
- (NSArray<id<MTLCounter>> *)counters;
@end

@interface RDNA4CounterSampleBuffer : NSObject <RDNA4CounterSampleBufferInternal> {
    id _dev;
    NSString *_lbl;
    VkQueryPool _qp;
    rdna4cnt_pool_state _state;
    pthread_mutex_t _mu;
    BOOL _muInit;
}
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLCounterSampleBufferDescriptor *)d
                         error:(NSError **)err;
- (id)device;
- (NSString *)label;
- (NSUInteger)sampleCount;
- (NSData *)resolveCounterRange:(NSRange)range;
@end

@interface RDNA4RenderHandles : NSObject {
    VkRenderPass _renderPass;
    VkFramebuffer _framebuffer;
}
- (instancetype)initWithRenderPass:(VkRenderPass)renderPass
                       framebuffer:(VkFramebuffer)framebuffer;
@end

@interface RDNA4RenderEncoder : _MTLCommandEncoder {
    RDNA4CommandBuffer *_cb; BOOL _ended, _inPass, _begun, _hostWriteDropped; uint32_t _roWarnV, _roWarnF; uint32_t _w, _h, _na, _layers; NSString *_lbl;
    VkRenderPass _rp1; BOOL _rp1c; VkFramebuffer _fb, _fbf;
    RDNA4RenderHandles *_baseHandles, *_fetchHandles;
    RDNA4Texture *_tex[9]; BOOL _fetch;   // bundle 10: slot [_na] is the depth/stencil attachment when _nd == 1; 11e-2: _fetch = the current/next pass is the framebuffer-fetch shape
    uint32_t _nd; VkFormat _dsVkFmt; unsigned _encSamples;   // bundle 10: depth/stencil attachment present; its VkFormat (UNDEFINED = none); the pass's sample count (0 until the first attachment, then >= 1)
    id _rtex[8];   // bundle 10: per colour attachment the resolve texture
    id _drtex, _srtex; uint32_t _dlvl, _dslice, _drlvl, _drslice, _srlvl, _srslice; NSUInteger _dsact, _ssact, _dresFilter, _sresFilter;
    RDNA4DSInfo _dsi; uint32_t _sref[2]; float _bias[3];   // bundle 10: the bound MTLDepthStencilState, the stencil reference values (front, back) and depth bias (constant, clamp, slope)
    RDNA4RenderPipelineState *_pso; RDNA4RenderPSOFast _psoFast; MTLRenderPipelineReflection *_roReflection;
    RDNA4RenderScratch *_sc; BOOL _scOwned, _rbBorrowed;
    MTLViewport _vp, *_extraVps; uint32_t _nvp; MTLScissorRect _sr, *_extraSrs; uint32_t _nsr;
    MTLTriangleFillMode _fillMode; MTLDepthClipMode _depthClipMode;
    BOOL _hasSamplePos; MTLSamplePosition _samplePos[8]; uint32_t _samplePosCount;
    float _blend[4]; MTLCullMode _cull; MTLWinding _wind;
    id _tessFactorBuf; NSUInteger _tessFactorOff, _tessFactorStride; float _tessFactorScale;
    NSMutableSet *_samplerBindings;
    rdna4dc_cbcache *_dcCB; BOOL _dcOn;   // P5b: per-draw redundancy state (_dcCB = CB-wide segment cache)
    rdna4occ *_occ; VkQueryPool _qp; uint32_t _qbase;   // build 18 (P2): this pass's occlusion bookkeeping (owned by the command buffer), its query pool and the first of its RDNA4OCC_MAXQ reserved query indices
    uint32_t _clrm; NSUInteger _dvc;   // #12 D2: attachments whose load action was Clear and not yet consumed by a presentable draw; vertex/index count of the draw being prepared (diagnostic)
    NSMutableSet *_argumentResources, *_argumentWritable;
    RDNA4ResourceBindings *_resourceBindings;
    uint64_t _resourceSegmentSerial;
    uint32_t _ampCount, _ampLayerOff, _activeLayerOff;
    BOOL _ampHasMappings;
    rdna4amp_mapping _ampMappings[RDNA4AMP_MAX_SUPPORTED_COUNT];
    id<RDNA4CounterSampleBufferInternal> _csbEnd[RDNA4CNT_MAX_PASS_ATTACHMENTS];
    NSUInteger _csbEndV[RDNA4CNT_MAX_PASS_ATTACHMENTS], _csbEndF[RDNA4CNT_MAX_PASS_ATTACHMENTS];
    uint32_t _nCsbEnd;
    uint32_t _dynDirty, _dynReqMask;
    uint32_t _drawDirty;
    BOOL _hasDispAtt, _cleanFastOk, _resClean;
    VkPrimitiveTopology _lastTopo;
    uint32_t _vbDirtyMask;
    VkBuffer _boundIBuf;
    VkDeviceSize _boundIOff;
    VkIndexType _boundIType;
    uint64_t _handlesSnapCBId, _handlesSnapSegSer;
    __unsafe_unretained id _snapBaseHandles, _snapFetchHandles;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d;
- (RDNA4RenderScratch *)rdna4EnsureScratch;
- (void)rdna4ReleaseRenderHandles;
@end

@interface RDNA4ComputeEncoder : _MTLCommandEncoder { RDNA4CommandBuffer *_cb; BOOL _ended; NSString *_lbl; RDNA4ComputePipelineState *_pso; MTLComputePipelineReflection *_roReflection; RDNA4StageState _st; uint32_t _roWarn; NSMutableSet *_argumentResources, *_argumentWritable, *_samplerBindings; RDNA4ResourceBindings *_resourceBindings; NSUInteger _threadgroupBytes; NSUInteger _tgMemLengths[RDNA4_ICB_MAX_TG_MEM_INDICES]; MTLRegion _stageInRegion; BOOL _hasStageInRegion; id<RDNA4CounterSampleBufferInternal> _csbEnd[RDNA4CNT_MAX_PASS_ATTACHMENTS]; NSUInteger _csbEndIdx[RDNA4CNT_MAX_PASS_ATTACHMENTS]; uint32_t _nCsbEnd; VkPipeline _boundPipe; VkPipelineLayout _boundLayout; VkDescriptorSet _boundSet; uint32_t _boundLocal[3]; BOOL _boundIndirect; NSUInteger _boundTgBytes; uint32_t _boundPC[12]; BOOL _boundPCValid; uint64_t _compSegSerial; BOOL _compStateDirty; BOOL _boundPushValid; NSUInteger _dispatchType; BOOL _pendingSerialBarrier; }
- (instancetype)initWithCommandBuffer:(id)cb dispatchType:(NSUInteger)dispatchType;
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy;
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLComputePassDescriptor *)d;
@end

@interface RDNA4BlitEncoder : _MTLCommandEncoder { RDNA4CommandBuffer *_cb; BOOL _ended; NSString *_lbl; NSMutableSet *_argumentResources, *_argumentWritable; id<RDNA4CounterSampleBufferInternal> _csbEnd[RDNA4CNT_MAX_PASS_ATTACHMENTS]; NSUInteger _csbEndIdx[RDNA4CNT_MAX_PASS_ATTACHMENTS]; uint32_t _nCsbEnd; }
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy;
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLBlitPassDescriptor *)d;
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size;
@end

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

@interface RDNA4ComputeEncoder (RDNA4InternalAccess)
- (void)rdna4SnapshotResources;
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind fragment:(BOOL)fragment index:(NSUInteger)index;
- (void)rdna4ICBComputeBarrier;
@end

@interface RDNA4BlitEncoder (GarnetInternalMethods)
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)getType;
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi;
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg;
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg;
- (void)fillBuffer:(id)buf range:(NSRange)r value:(uint8_t)v;
- (void)updateFence:(id)f;
- (void)waitForFence:(id)f;
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier;
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer inRange:(NSRange)range destinationBuffer:(id<MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset;
- (void)generateMipmapsForTexture:(id)tex;
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc;
- (void)copyFromTexture:(id)src toTexture:(id)dst;
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)o;
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o;
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o;
- (void)synchronizeResource:(id)r;
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l;
- (void)pushDebugGroup:(NSString *)s;
- (void)popDebugGroup;
- (void)endEncoding;
- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range;
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source sourceRange:(NSRange)sourceRange destination:(id<MTLIndirectCommandBuffer>)destination destinationIndex:(NSUInteger)destinationIndex;
- (void)optimizeIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range;
@end

@interface RDNA4CommandSegment (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ComputeEncoder (GarnetInternalMethods)
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)getType;
- (NSUInteger)dispatchType;
- (void)setComputePipelineState:(id)p;
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setBuffer:(id)b offset:(NSUInteger)o attributeStride:(NSUInteger)stride atIndex:(NSUInteger)i;
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r;
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i;
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setTexture:(id)t atIndex:(NSUInteger)i;
- (void)setTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r;
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b override:(BOOL)override atIndex:(NSUInteger)i;
- (void)setSamplerState:(id)s atIndex:(NSUInteger)i;
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i;
- (void)setSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r;
- (void)setSamplerStates:(const id __unsafe_unretained *)s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r;
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setStageInRegion:(MTLRegion)region;
- (void)memoryBarrierWithScope:(NSUInteger)s;
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n;
- (void)updateFence:(id)f;
- (void)waitForFence:(id)f;
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier;
- (void)pushDebugGroup:(NSString *)s;
- (void)popDebugGroup;
- (void)insertDebugSignpost:(NSString *)s;
- (void)rdna4Dispatch:(MTLSize)grid tpt:(MTLSize)tpt exactThreads:(BOOL)exact indirect:(RDNA4Buffer *)indirect offset:(NSUInteger)offset;
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t;
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t;
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)t;
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange;
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)endEncoding;
- (void)rdna4ExecuteComputeICBGPU:(RDNA4IndirectCommandBuffer *)icb directRange:(NSRange)range indirectBuffer:(id)indirectBuf indirectOffset:(NSUInteger)indirectOff useIndirect:(BOOL)useIndirect;
@end

@interface RDNA4CounterSampleBuffer (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev descriptor:(MTLCounterSampleBufferDescriptor *)d error:(NSError **)err;
- (MTLStorageMode)rdna4StorageMode;
- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb vkCommandBuffer:(VkCommandBuffer)cmd sampleIndex:(NSUInteger)sampleIndex barrier:(BOOL)barrier stageFlags:(VkPipelineStageFlagBits)stage who:(const char *)who;
- (BOOL)rdna4ResolveRange:(NSRange)range intoBytes:(uint64_t *)dst error:(NSError **)err;
@end

@interface RDNA4RenderEncoder (GarnetInternalMethods)
- (BOOL)rdna4CheckDSResolve:(BOOL)isDepth;
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind fragment:(BOOL)fragment index:(NSUInteger)index;
- (void)rdna4OccInit:(MTLRenderPassDescriptor *)d;
- (void)rdna4OccEndQuery;
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)getType;
- (BOOL)rdna4CheckResolve:(uint32_t)i;
- (void)rdna4RunDSResolveInto:(RDNA4Texture *)dst level:(uint32_t)dlvl slice:(uint32_t)dslice depthMode:(VkResolveModeFlagBits)dMode stencilMode:(VkResolveModeFlagBits)sMode;
- (BOOL)rdna4ResolveAll;
- (void)setRenderPipelineState:(id)pso;
- (void)setCullMode:(MTLCullMode)m;
- (void)setFrontFacingWinding:(MTLWinding)w;
- (void)setViewport:(MTLViewport)v;
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n;
- (void)setScissorRect:(MTLScissorRect)r;
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n;
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)viewMappings;
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier;
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a;
- (void)setTriangleFillMode:(MTLTriangleFillMode)m;
- (void)setDepthClipMode:(NSUInteger)m;
- (void)setDepthBias:(float)a slopeScale:(float)b clamp:(float)c;
- (void)setStencilReferenceValue:(uint32_t)v;
- (void)setDepthStencilState:(id)s;
- (void)rdna4Barrier:(const char *)what;
- (void)memoryBarrierWithScope:(NSUInteger)sc afterStages:(NSUInteger)a beforeStages:(NSUInteger)b;
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n afterStages:(NSUInteger)a beforeStages:(NSUInteger)b;
- (void)textureBarrier;
- (void)updateFence:(id)f afterStages:(NSUInteger)st;
- (void)waitForFence:(id)f beforeStages:(NSUInteger)st;
- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i;
- (void)setColorStoreActionOptions:(NSUInteger)a atIndex:(NSUInteger)i;
- (void)setDepthStoreAction:(NSUInteger)a;
- (void)setDepthStoreActionOptions:(NSUInteger)a;
- (void)setStencilStoreAction:(NSUInteger)a;
- (void)setStencilStoreActionOptions:(NSUInteger)a;
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b;
- (void)setLineWidth:(float)w;
- (void)pushDebugGroup:(NSString *)s;
- (void)popDebugGroup;
- (void)insertDebugSignpost:(NSString *)s;
- (RDNA4StageState *)rdna4Stage:(BOOL)frag;
- (void)rdna4SetBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i frag:(BOOL)f;
- (void)rdna4SetBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i frag:(BOOL)f;
- (void)rdna4SetTexture:(id)t index:(NSUInteger)i frag:(BOOL)f;
- (void)rdna4SetSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b override:(BOOL)override index:(NSUInteger)i frag:(BOOL)f;
- (void)rdna4SetSampler:(id)s index:(NSUInteger)i frag:(BOOL)f;
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setVertexBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r;
- (void)setFragmentBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r;
- (void)rdna4SetVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f;
- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)rdna4SetAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i frag:(BOOL)f;
- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i;
- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i;
- (void)rdna4SetIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f;
- (void)setVertexIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setVertexIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setFragmentIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i;
- (void)setFragmentIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i;
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i;
- (void)setVertexTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r;
- (void)setFragmentTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r;
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i;
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i;
- (void)setVertexSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i;
- (void)setFragmentSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i;
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r;
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r;
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r;
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r;
- (void)rdna4NeedTex:(RDNA4Texture *)t layout:(VkImageLayout)l writable:(BOOL)writable;
- (BOOL)rdna4DcStage:(int)idx pb:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dsl:(VkDescriptorSetLayout)dsl name:(const char *)nm cmd:(VkCommandBuffer)cmd;
- (BOOL)rdna4PrepareDraw:(VkPrimitiveTopology)topo;
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi;
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c;
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic;
- (BOOL)rdna4IndirectBuffer:(id)buffer offset:(NSUInteger)off bytes:(NSUInteger)bytes;
- (void)drawPrimitives:(MTLPrimitiveType)t indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)ioff indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off;
- (void)setTessellationFactorScale:(float)scale;
- (BOOL)rdna4PreparePatchDraw:(NSUInteger)numCP indexed:(BOOL)indexed patchIndex:(id)patchBuf patchIndexOffset:(NSUInteger)patchOff controlIndex:(id)cpBuf controlIndexOffset:(NSUInteger)cpOff indirect:(BOOL)indirect;
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)endEncoding;
- (void)rdna4ExecuteICBGPU:(RDNA4IndirectCommandBuffer *)icb directRange:(NSRange)range indirectBuffer:(id)indirectBuf indirectOffset:(NSUInteger)indirectOff useIndirect:(BOOL)useIndirect;
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange;
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)rdna4SetMeshStageBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i isMesh:(BOOL)isMesh;
- (void)rdna4SetMeshStageBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i isMesh:(BOOL)isMesh;
- (void)rdna4SetMeshStageTexture:(id)t index:(NSUInteger)i isMesh:(BOOL)isMesh;
- (void)rdna4SetMeshStageSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b override:(BOOL)override index:(NSUInteger)i isMesh:(BOOL)isMesh;
- (void)setObjectBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setObjectBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setObjectBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r;
- (void)setObjectBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)setObjectTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i;
- (void)setObjectTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r;
- (void)setObjectSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i;
- (void)setObjectSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i;
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r;
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r;
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setMeshBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setMeshBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i;
- (void)setMeshBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r;
- (void)setMeshBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)setMeshTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i;
- (void)setMeshTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r;
- (void)setMeshSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i;
- (void)setMeshSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i;
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r;
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r;
- (void)setObjectVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i;
- (void)setObjectVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setMeshVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i;
- (void)setMeshVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setObjectIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i;
- (void)setObjectIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setMeshIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i;
- (void)setMeshIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r;
- (void)setObjectAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i;
- (void)setMeshAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i;
- (void)rdna4DrawMeshGroups:(MTLSize)grid objLocal:(MTLSize)objTg meshLocal:(MTLSize)meshTg indirect:(BOOL)isIndirect indirectBuffer:(id)indirectBuf indirectOffset:(NSUInteger)indirectOff;
- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup;
- (void)drawMeshThreads:(MTLSize)threadsPerGrid threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup;
- (void)drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup;
@end

@interface RDNA4RenderHandles (GarnetInternalMethods)
- (instancetype)initWithRenderPass:(VkRenderPass)renderPass framebuffer:(VkFramebuffer)framebuffer;
@end

GARNET_INTERNAL_EXTERN NSArray<id<MTLCounterSet>> *rdna4_device_counter_sets(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_device_supports_counter_sampling(MTLCounterSamplingPoint point);
GARNET_INTERNAL_EXTERN id<MTLCounterSampleBuffer> rdna4_device_new_counter_sample_buffer(
    id dev, MTLCounterSampleBufferDescriptor *descriptor, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_device_sample_timestamps(MTLTimestamp *cpuTimestamp, MTLTimestamp *gpuTimestamp);
GARNET_INTERNAL_EXTERN VkPrimitiveTopology rdna4_topo(MTLPrimitiveType t);

static inline void rdna4_free_ptr(void **p) { free(*p); }

static inline void rdna4_transfer_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

GARNET_INTERNAL_EXTERN BOOL rdna4_ios_writeback_ex(VkCommandBuffer cmd, RDNA4Texture *t, BOOL forceSync);
static inline BOOL rdna4_ios_writeback(VkCommandBuffer cmd, RDNA4Texture *t) {
    return rdna4_ios_writeback_ex(cmd, t, NO);
}
static inline void rdna4_ios_use(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write) {
    rdna4_ios_usek(cb, t, needContents, write, 0);
}

GARNET_INTERNAL_EXTERN id<RDNA4CounterSampleBufferInternal> rdna4_counter_buffer_check(
    id sampleBuffer, id expectedDevice, NSUInteger sampleIndex,
    BOOL checkIndex, NSString **whyOut);
GARNET_INTERNAL_EXTERN BOOL rdna4_counter_pass_index_valid(
    id<RDNA4CounterSampleBufferInternal> csb, NSUInteger index);

#endif /* GARNET_COMMANDS_INTERNAL_H */
