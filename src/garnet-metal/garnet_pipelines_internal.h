// garnet_pipelines_internal.h - Shared internal declarations across GarnetMetal's
// render pipeline, compute pipeline, and pipeline resource-binding compilation units.
#ifndef GARNET_PIPELINES_INTERNAL_H
#define GARNET_PIPELINES_INTERNAL_H

#ifndef GARNET_INTERNAL_EXTERN
#define GARNET_INTERNAL_EXTERN static
#endif

#include "garnet_shaders_internal.h"
#include "garnet_tessellation.h"
#include "garnet_icb_abi.h"
#include "garnet_intfmt.h"

@interface RDNA4RenderPipelineState : _MTLRenderPipelineState {
    id _dev; NSString *_lbl; VkShaderModule _vs, _tcs, _tes, _fs; VkPipelineLayout _pl; VkRenderPass _rp; VkDescriptorSetLayout _dsl[2], _tessDsl;
    BOOL _isTess; uint32_t _tessSet; rdna4_tess_cfg_t _tessCfg; uint64_t _tessKey;
    RDNA4PB *_pbV, *_pbF; uint32_t _npbV, _npbF; BOOL _fetch; uint32_t _fetchMax;   // 11e-2: fragment reads [[color(n)]] (input attachments 0.._fetchMax)
    VkVertexInputBindingDescription _vib[32]; VkVertexInputAttributeDescription _via[32]; uint32_t _nvib, _nvia; uint32_t _need[32]; uint32_t _nneed;
    VkPipelineColorBlendAttachmentState _cba[8]; uint32_t _na; char _vep[128], _fep[128];
    NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _fallback, _icb, _hasFrag, _rastOff;
    uint64_t _icbToken;
    NSData *_icbVsSpv, *_icbFsSpv;
    garnetx_icb_vattr_t _icbVAttrs[32]; uint32_t _nIcbVAttrs;
    uint32_t _pcs; VkShaderStageFlags _pcf;
    BOOL _icbGpuReady, _icbHasVsParams;
    VkShaderModule _icbVs, _icbFs, _icbTes;
    VkDescriptorSetLayout _icbDsl[2], _icbTessDsl;
    VkPipelineLayout _icbPl;
    RDNA4PB *_icbPbV, *_icbPbF; uint32_t _icbNpbV, _icbNpbF;
    NSMutableDictionary *_icbVariants;
    char _vnm[64], _fnm[64];   // native #12 P1: vertex / fragment function names (CoreDisplay's final pass = fragment "GPUPass")
    uint32_t _drawBits;        // cached rdna4df_draw_bits(_fnm)
    _Atomic uint64_t _fastKey;
    _Atomic uintptr_t _fastPipe;   // lock-free single-variant fast-path (published once, immutable until dealloc)
    rdna4pc_t _pc; MTLPixelFormat _dpf, _spf; VkFormat _dsVk; unsigned _dsAsp;   // bundle 10: fixed config (samples, dynamic states, depth bias), the depth / stencil attachment formats, the attachment's VkFormat (UNDEFINED = none) and aspects
    MTLRenderPipelineDescriptor *_hsDesc; _Atomic(uintptr_t) _realp;   // hot-swap: creation descriptor (fallback only); the published real object (+1, written once)
    MTLRenderPipelineReflection *_reflection;
    RDNA4ArgumentTables _argumentTables;
    rdna4_argument_scan_reflection_plan _scanV, _scanF;
    uint32_t _maxAmp;
    uint64_t _vftOwnerToken;
    rdna4vft_reflection_t _vftV, _vftF;
    uint64_t _iftOwnerToken;
    rdna4rtr_reflection_t _rtV, _rtF;
    BOOL _hasAddressRegistry;
    uint8_t _pushStage[2];
    uint8_t _hasEmbeddedTex[2];
    BOOL _useGpl;
    VkPipelineLayout _gplVsPl, _gplFsPl;
    VkPipeline _gplFsLib, _gplFoLib;
    NSMutableDictionary *_gplVsLibs;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)options noFallback:(BOOL)nofb error:(NSError **)err;
- (RDNA4RenderPipelineState *)rdna4Real;   // the hot-swapped real pipeline, or nil
- (BOOL)rdna4HotRebuild;
- (BOOL)rdna4IsRenderFallback;
- (void)rdna4FillFast:(RDNA4RenderPSOFast *)out;
- (VkPipeline)vkPipeline;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f ds:(uint32_t)dk;   // bundle 10: dk = rdna4DSKey: (0 for a colour-only pipeline)
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f ds:(uint32_t)dk rflags:(uint32_t)rf;
- (BOOL)rdna4PrepareICBGPUVariant:(NSError **)err;
- (VkPipeline)icbPipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f ds:(uint32_t)dk;
- (uint64_t)rdna4ICBToken;
- (BOOL)rdna4ICBHasVsParams;
- (VkPipelineLayout)icbLayout;
- (VkDescriptorSetLayout)icbDsl:(int)i;
- (VkDescriptorSetLayout)icbTessDsl;
- (const RDNA4PB *)icbPbV; - (uint32_t)icbNpbV; - (const RDNA4PB *)icbPbF; - (uint32_t)icbNpbF;
- (uint32_t)rdna4DSKey:(const void *)dsinfo;   // RDNA4DSInfo *, NULL = the default state (Always, no write, no stencil)
- (BOOL)rdna4HasDS; - (BOOL)rdna4HasDepth; - (BOOL)rdna4HasStencil; - (VkFormat)rdna4DSVk; - (unsigned)rdna4SampleCount;
- (VkPipelineLayout)layout;
- (VkDescriptorSetLayout)dsl:(int)i;
- (const RDNA4ArgumentTables *)argumentTables;
- (const rdna4_argument_scan_reflection_plan *)scanPlanV;
- (const rdna4_argument_scan_reflection_plan *)scanPlanF;
- (BOOL)rdna4HasAddressRegistry;
- (const RDNA4PB *)pbV; - (uint32_t)npbV; - (const RDNA4PB *)pbF; - (uint32_t)npbF;
- (const uint32_t *)needBindings; - (uint32_t)nNeed;
- (BOOL)fetch; - (uint32_t)fetchMax;
- (const char *)rdna4VName; - (const char *)rdna4FName; - (uint32_t)rdna4DrawBits; - (BOOL)rdna4HasFragment;
- (id)reflection;
- (void)rdna4SetReflection:(MTLRenderPipelineReflection *)reflection;
- (BOOL)rdna4IsTessellation;
- (const rdna4_tess_cfg_t *)tessCfg;
- (VkDescriptorSetLayout)tessDsl;
- (uint32_t)tessSet;
- (uint64_t)tessCacheKey;
- (uint32_t)rdna4MaxVertexAmplificationCount;
- (BOOL)rdna4IsMeshPipeline;
- (const rdna4rtr_as_reflection_t *)rdna4RtAsReflV;
- (const rdna4rtr_as_reflection_t *)rdna4RtAsReflF;
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage;
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage;
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage;
@end

@interface RDNA4ComputePipelineState : NSObject {
    id _dev; NSString *_lbl; VkShaderModule _sm; VkPipelineLayout _pl; VkDescriptorSetLayout _dsl; RDNA4PB *_pb; uint32_t _npb; uint32_t _pcSize;
    uint32_t _local[3]; int _mode; uint32_t _pcOff; char _ep[128]; NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _noop;
    id _hsFn; _Atomic(uintptr_t) _realp;   // hot-swap (placeholder only): the kernel function to rebuild from; the published real object (+1, written once)
    RDNA4LinkCtx *_hsLk;   // build 18 (P3): the kernel's link context at creation (nil = none); the rebuild on the watcher thread installs it
    MTLComputePipelineReflection *_reflection;
    NSData *_spv;
    BOOL _lumaWorkgroup;
    RDNA4ArgumentTables _argumentTables;
    rdna4_argument_scan_reflection_plan _scan;
    VkDescriptorSetLayout _emptySet;
    _Atomic uint64_t _uid;
    BOOL _icb, _icbGpuReady, _icbAllowFallback;
    uint64_t _icbToken;
    NSData *_icbSpv;
    VkDescriptorSetLayout _icbDsl;
    VkPipelineLayout _icbPl;
    RDNA4PB *_icbPb;
    uint32_t _icbNpb;
    NSMutableDictionary *_icbVariants;
    uint32_t _compiledLocals[32][3];
    uint32_t _nCompiledLocals;
    uint64_t _vftOwnerToken;
    rdna4vft_reflection_t _vft;
    uint64_t _iftOwnerToken;
    rdna4rtr_reflection_t _rt;
    BOOL _hasAddressRegistry;
    BOOL _pushDesc;
}
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev function:(id)fn spv:(NSData *)spv meta:(NSDictionary *)meta fname:(NSString *)fname error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLComputePipelineDescriptor *)descriptor options:(NSUInteger)options error:(NSError **)err;
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn;   // #12 R1: valid state whose dispatches encode nothing (hot-swapped when the .spv + .meta.json appear)
- (RDNA4ComputePipelineState *)rdna4Real;
- (BOOL)rdna4HotRebuild;
- (BOOL)noop;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect threadgroupBytes:(NSUInteger)bytes;
- (BOOL)rdna4PrepareICBGPUVariant:(NSError **)err;
- (VkPipeline)icbPipelineForLocal:(const uint32_t *)l;
- (uint64_t)rdna4ICBToken;
- (BOOL)rdna4ICBAllowFallback;
- (VkPipelineLayout)icbLayout;
- (VkDescriptorSetLayout)icbDsl;
- (const RDNA4PB *)icbPb;
- (uint32_t)icbNpb;
- (uint32_t)rdna4CopyCompiledLocals:(uint32_t (*)[3])outLocals maxCount:(uint32_t)maxCount;
- (BOOL)rdna4LumaWorkgroup;
- (VkPipelineLayout)layout; - (VkDescriptorSetLayout)dsl; - (const RDNA4PB *)pb; - (uint32_t)npb; - (BOOL)pushDesc;
- (const RDNA4ArgumentTables *)argumentTables;
- (const rdna4_argument_scan_reflection_plan *)scanPlan;
- (BOOL)rdna4HasAddressRegistry;
- (const uint32_t *)nominalLocal; - (int)mode; - (uint32_t)pcOffset; - (uint32_t)pcSize;
- (id)reflection;
- (void)rdna4SetReflection:(MTLComputePipelineReflection *)reflection;
- (const rdna4rtr_as_reflection_t *)rdna4RtAsRefl;
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function;
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor;
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor;
@end

typedef struct {
    uint64_t key;
    CFTypeRef obj;
    uint8_t snapshotted;
} RDNA4ResBindEntry;

@interface RDNA4ResourceBindings : NSObject {
    RDNA4ResBindEntry *_entries;
    RDNA4ResBindEntry _inline[8];
    uint32_t _count, _cap;
    uint64_t _lastCBId, _lastSegmentSerial;
    BOOL _hasUnsnapshotted;
}
- (void)setObject:(id)object slot:(NSNumber *)slot;
- (void)setObject:(id)object key:(uint64_t)key;
- (void)clear;
- (NSUInteger)count;
- (void)snapshotToCommandBuffer:(RDNA4CommandBuffer *)commandBuffer;
@end

@interface RDNA4RenderPipelineState (RDNA4MeshQuery)
- (BOOL)rdna4IsMeshPipeline;
@end

@interface RDNA4RenderPipelineState (GarnetInternalMethods)
- (const char *)rdna4VName;
- (const char *)rdna4FName;
- (BOOL)rdna4HasFragment;
- (RDNA4RenderPipelineState *)rdna4Real;
- (BOOL)rdna4HasDS;
- (BOOL)rdna4HasDepth;
- (BOOL)rdna4HasStencil;
- (VkFormat)rdna4DSVk;
- (unsigned)rdna4SampleCount;
- (void)rdna4DestroyICBGPUState;
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk error:(NSError **)err;
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk icb:(BOOL)icb error:(NSError **)err;
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk rflags:(uint32_t)rflags icb:(BOOL)icb error:(NSError **)err;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk;
- (const RDNA4PB *)icbPbV;
- (BOOL)fetch;
- (const RDNA4PB *)pbV;
- (const uint32_t *)needBindings;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)maxTotalThreadsPerThreadgroup;
- (NSUInteger)threadgroupSizeMatchesTileSize;
- (NSUInteger)imageblockSampleLength;
- (BOOL)supportIndirectCommandBuffers;
@end

@interface RDNA4ComputePipelineState (GarnetInternalMethods)
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn;
- (void)rdna4EnableICBForDescriptor:(MTLComputePipelineDescriptor *)descriptor;
- (void)rdna4DestroyICBGPUState;
- (VkPipelineLayout)layout;
- (const uint32_t *)nominalLocal;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)maxTotalThreadsPerThreadgroup;
- (NSUInteger)threadExecutionWidth;
- (NSUInteger)staticThreadgroupMemoryLength;
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d;
- (BOOL)supportIndirectCommandBuffers;
- (uint64_t)uniqueIdentifier;
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0));
@end

@interface RDNA4ResourceBindings (GarnetInternalMethods)
- (instancetype)init;
@end

GARNET_INTERNAL_EXTERN NSData *rdna4_spv_lookup(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta);
GARNET_INTERNAL_EXTERN NSData *rdna4_spv_lookup_samples(id fn, const char *role, uint32_t rasterSamples, NSError **err, NSString **fname, NSDictionary **meta);
GARNET_INTERNAL_EXTERN VkShaderModule rdna4_spv_module(const void *code, size_t bytes, const char *role, NSError **err);
GARNET_INTERNAL_EXTERN NSArray *rdna4_icb_compute_reg_for_device(id dev);
GARNET_INTERNAL_EXTERN id rdna4_ba_compile_compute_state(id dev, id fn, NSData *spv, NSDictionary *meta, NSString *fname, NSError **err);
GARNET_INTERNAL_EXTERN VkSampler rdna4_sampler_object_handle(id object, BOOL comparison,
                                             BOOL normalized, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_sampler_object_pixel(id object);
GARNET_INTERNAL_EXTERN void rdna4_argument_scan_prepared_destroy(RDNA4ArgumentScanPrepared *prepared);
GARNET_INTERNAL_EXTERN uint8_t rdna4_argument_scan_storage(MTLStorageMode mode);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_scan_writable(NSSet *writable, RDNA4Buffer *buffer);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_scan_prepare(RDNA4ArgumentScanPrepared *out,
                                        const RDNA4StageState *const stages[],
                                        size_t stageCount, NSSet *writable,
                                        id device, NSString **why);
GARNET_INTERNAL_EXTERN id rdna4_embedded_resource(const RDNA4PB *p, uint32_t element, const RDNA4StageState *st, BOOL texture, id device);
GARNET_INTERNAL_EXTERN BOOL rdna4nc_drop_write(id resource, RDNA4NCOp op, const char *operation, const char *encoder);
GARNET_INTERNAL_EXTERN NSArray *rdna4nc_stage_arguments(id reflection, RDNA4NCOp stage);
GARNET_INTERNAL_EXTERN BOOL rdna4nc_stage_writes(const RDNA4StageState *state, NSArray *arguments, RDNA4NCOp op,
                                 const char *stage, uint32_t *warned);
GARNET_INTERNAL_EXTERN void rdna4_argument_stage_resources(NSMutableSet *resources, NSMutableSet *writable,
        const RDNA4PB *bindings, uint32_t count, const RDNA4StageState *state, NSArray *arguments);
GARNET_INTERNAL_EXTERN RDNA4Buffer *rdna4_dummy_buffer(id dev);
GARNET_INTERNAL_EXTERN RDNA4Texture *rdna4_dummy_texture(id dev);
GARNET_INTERNAL_EXTERN RDNA4Texture *rdna4_dummy_texture_write(id dev);
GARNET_INTERNAL_EXTERN RDNA4Texture *rdna4_dummy_null_texture(id dev, uint32_t shape);
GARNET_INTERNAL_EXTERN RDNA4Texture *rdna4_dummy_null_image(id dev, const rdna4ns_shape *shape);
GARNET_INTERNAL_EXTERN VkBufferView rdna4_dummy_null_texel(id dev, const rdna4ns_shape *shape,
                                          const char **why);
GARNET_INTERNAL_EXTERN RDNA4SamplerState *rdna4_dummy_sampler(id dev);
GARNET_INTERNAL_EXTERN void rdna4_each_tex(const RDNA4PB *pb, uint32_t n, const RDNA4StageState *st, id dev, void (^fn)(RDNA4Texture *t, VkImageLayout l, BOOL writable));
GARNET_INTERNAL_EXTERN int rdna4_plan(const uint32_t threads[3], const uint32_t nom[3], RDNA4Region *out);
GARNET_INTERNAL_EXTERN uint64_t rdna4_resource_slot_key(uint32_t kind, BOOL fragment,
                                     NSUInteger index);
GARNET_INTERNAL_EXTERN BOOL rdna4_encoder_take_segment_change(uint64_t *tracked,
                                              uint64_t current);

GARNET_INTERNAL_EXTERN BOOL rdna4_icb_build_stage_dsl(
    const RDNA4PB *origPb, uint32_t origN, const RDNA4sRefl *refl, uint32_t set,
    VkShaderStageFlags stage, BOOL wantVsParams,
    VkDescriptorSetLayout *outDsl, RDNA4PB **outPb, uint32_t *outN, NSError **err);

#endif /* GARNET_PIPELINES_INTERNAL_H */
