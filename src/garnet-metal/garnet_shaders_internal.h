// garnet_shaders_internal.h - Shared internal declarations across GarnetMetal's shader,
// binary-archive, and dynamic-library/VFT compilation units (garnet_shaders.m,
// garnet_binary_archive.m, garnet_dynlib_vft.m).
#ifndef GARNET_SHADERS_INTERNAL_H
#define GARNET_SHADERS_INTERNAL_H

#include "garnet_binary_archive.h"
#include "garnet_dynlib_vft.h"

@protocol RDNA4DynamicLibraryInternal <NSObject>
- (id)device;
- (NSString *)installName;
- (NSArray<NSDictionary *> *)rdna4PayloadModules;
- (NSArray<NSString *> *)rdna4ExportedSymbols;
@end

@protocol RDNA4RenderPipelineArchiveInit
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err;
- (VkPipeline)vkPipeline;
@end

@interface RDNA4LinkCtx : NSObject {
    @public
    id fn;
    NSString *lsha;
    NSArray<NSDictionary *> *deps;
    NSArray<NSDictionary *> *dynlibs;
    NSArray<NSDictionary *> *visibleFns;
    NSArray<NSDictionary *> *intersectionFns;
    BOOL useStage2;
    BOOL useStage3;
    BOOL requireStage2;
    NSError *linkageError;
}
@end

@interface RDNA4BinaryArchive : NSObject {
    id _dev;
    NSString *_lbl;
    NSLock *_lock;
    rdna4ba_fingerprint_t _fp;
    rdna4ba_archive_t _archive;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (NSUInteger)rdna4EntryCount;
- (const rdna4ba_fingerprint_t *)rdna4Fingerprint;
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error;
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor error:(NSError **)error;
- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(id)descriptor error:(NSError **)error;
- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor library:(id<MTLLibrary>)library error:(NSError **)error;
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error;
- (BOOL)rdna4LookupKind:(uint32_t)kind keyU64:(uint64_t)keyU64 keySha:(const uint8_t[32])keySha
                   spv0:(NSData **)spv0 meta0:(NSData **)meta0
                   spv1:(NSData **)spv1 meta1:(NSData **)meta1
                vkCache:(NSData **)vkCache;
@end

@interface RDNA4DynamicLibrary : NSObject <RDNA4DynamicLibraryInternal> {
    id _dev;
    NSString *_lbl;
    NSString *_installName;
    rdna4dl_container_t _container;
    NSArray<NSDictionary *> *_modulesCache;
    NSArray<NSString *> *_symbolsCache;
    NSLock *_lock;
}
- (instancetype)initWithDevice:(id)dev library:(id<MTLLibrary>)library error:(NSError **)error;
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url error:(NSError **)error;
- (id)device;
- (NSString *)installName;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error;
- (NSArray<NSDictionary *> *)rdna4PayloadModules;
- (NSArray<NSString *> *)rdna4ExportedSymbols;
- (BOOL)rdna4HasExportedSymbol:(NSString *)symbol;
@end

@interface RDNA4FunctionHandle : NSObject {
    id _dev;
    __weak id _ownerPipeline;
    uint64_t _ownerToken;
    NSUInteger _stage;
    MTLFunctionType _functionType;
    NSString *_name;
    uint32_t _handleId;
}
- (instancetype)initWithDevice:(id)dev
                 ownerPipeline:(id)pso
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                  functionType:(MTLFunctionType)ftype
                          name:(NSString *)name
                      handleId:(uint32_t)handleId;
- (id)device;
- (MTLFunctionType)functionType;
- (NSString *)name;
- (uint32_t)rdna4HandleId;
- (uint64_t)rdna4OwnerToken;
- (NSUInteger)rdna4Stage;
- (id)rdna4OwnerPipeline;
@end

@interface RDNA4VisibleFunctionTable : _MTLResource {
    id _dev;
    NSString *_lbl;
    uint64_t _ownerToken;
    NSUInteger _stage;
    rdna4vft_table_t _table;
    RDNA4Buffer *_backing;
    RDNA4Buffer *_attachedBuffers[RDNA4VFT_MAX_ATTACH_BUFFERS];
    NSUInteger _attachedBufferOffsets[RDNA4VFT_MAX_ATTACH_BUFFERS];
    id _nestedTables[RDNA4VFT_MAX_NESTED_TABLES];
    NSLock *_lock;
    RDNA4_RES_IVARS
}
- (instancetype)initWithDevice:(id)dev
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                 functionCount:(NSUInteger)count
                   maxHandleId:(uint32_t)maxHandleId
                         error:(NSError **)error;
- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index;
- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex;
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables withBufferRange:(NSRange)bufferRange;
- (RDNA4Buffer *)rdna4BackingBuffer;
- (uint64_t)rdna4BackingGPUAddress;
- (NSUInteger)rdna4FunctionCount;
- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index;
- (uint64_t)rdna4OwnerToken;
- (NSUInteger)rdna4Stage;
- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut;
- (id)rdna4NestedTableAtBufferIndex:(NSUInteger)index;
- (void)rdna4RetainIntoCommandBuffer:(id)cb;
@end

@interface RDNA4BinaryArchive (GarnetInternalMethods)
- (BOOL)rdna4StoreEntry:(const rdna4ba_entry_view_t *)view error:(NSError **)error;
- (BOOL)rdna4LookupKind:(uint32_t)kind keyU64:(uint64_t)keyU64 keySha:(const uint8_t[32])keySha spv0:(NSData **)spv0 meta0:(NSData **)meta0 spv1:(NSData **)spv1 meta1:(NSData **)meta1 vkCache:(NSData **)vkCache;
@end

@interface RDNA4DynamicLibrary (GarnetInternalMethods)
- (void)rdna4RebuildCachesLocked;
@end

@interface RDNA4FunctionHandle (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev ownerPipeline:(id)pso ownerToken:(uint64_t)token stage:(NSUInteger)stage functionType:(MTLFunctionType)ftype name:(NSString *)name handleId:(uint32_t)handleId;
@end

@interface RDNA4VisibleFunctionTable (GarnetInternalMethods)
- (BOOL)rdna4SyncBackingLocked;
- (instancetype)initWithDevice:(id)dev ownerToken:(uint64_t)token stage:(NSUInteger)stage functionCount:(NSUInteger)count maxHandleId:(uint32_t)maxHandleId error:(NSError **)error;
- (BOOL)rdna4ValidateHandleLocked:(id)function handleIdOut:(uint32_t *)hidOut;
- (void)rdna4RetainIntoCommandBuffer:(id)cb depth:(unsigned)depth;
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

GARNET_INTERNAL_EXTERN VkPipelineCache rdna4_ba_vkcache(void);
GARNET_INTERNAL_EXTERN BOOL rdna4x_parse_vft_from_meta(NSDictionary *meta, rdna4vft_reflection_t *outVft, NSError **err);
GARNET_INTERNAL_EXTERN void rdna4_ba_vkcache_scope_begin(rdna4_ba_vkcache_scope_t *sc, NSData *initialData,
                                         const rdna4ba_fingerprint_t *fp);
GARNET_INTERNAL_EXTERN void rdna4_ba_vkcache_scope_end(rdna4_ba_vkcache_scope_t *sc);
GARNET_INTERNAL_EXTERN BOOL rdna4_ba_lookup_compute(id dev, MTLComputePipelineDescriptor *d, NSUInteger options,
                                    NSData **spvOut, NSDictionary **metaOut, NSData **vkCacheOut,
                                    rdna4ba_fingerprint_t *fpOut, BOOL *hitOut, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_ba_lookup_render(id dev, MTLRenderPipelineDescriptor *d, NSUInteger options,
                                   NSData **vsSpvOut, NSDictionary **vsMetaOut,
                                   NSData **fsSpvOut, NSDictionary **fsMetaOut,
                                   NSData **vkCacheOut, rdna4ba_fingerprint_t *fpOut,
                                   BOOL *hitOut, NSError **err);
GARNET_INTERNAL_EXTERN uint64_t rdna4_vft_next_owner_token(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_dynlib_vft_translator_ready(void);
GARNET_INTERNAL_EXTERN id<MTLFunctionHandle> rdna4_vft_pipeline_handle(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    id<MTLFunction> function, NSUInteger stage);
GARNET_INTERNAL_EXTERN id<MTLVisibleFunctionTable> rdna4_vft_pipeline_new_table(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    MTLVisibleFunctionTableDescriptor *descriptor, NSUInteger stage);
GARNET_INTERNAL_EXTERN BOOL rdna4_vft_validate_render_footnote10(
    id desc, BOOL isMeshDesc, BOOL hasVftMeta, BOOL hasRenderRT, NSError **error);


GARNET_INTERNAL_EXTERN NSString *rdna4x_xlib_path(void);
GARNET_INTERNAL_EXTERN rdna4x_backend_capabilities rdna4_rt_xlate_caps(void);
GARNET_INTERNAL_EXTERN BOOL rdna4x_list_exported_symbols_for_module(
    const uint8_t *bytes, size_t len, uint32_t payloadKind,
    NSArray<NSString *> **outSymbols, NSError **err);

GARNET_INTERNAL_EXTERN BOOL rdna4x_sandboxed(void);
GARNET_INTERNAL_EXTERN void rdna4_sync_note_async_success(void);

#endif /* GARNET_SHADERS_INTERNAL_H */
