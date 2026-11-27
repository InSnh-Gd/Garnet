// garnet_metal_internal.h - Shared internal declarations for GarnetMetal's modular compilation units.
// Production builds compile each garnet_*.m independently with -DGARNET_METAL_MODULAR_BUILD=1 -fvisibility=hidden.
// Single-TU host/mock tests include GarnetDevice.m without GARNET_METAL_MODULAR_BUILD, where GARNET_INTERNAL
// collapses to `static` for historical single-translation-unit compatibility.
#ifndef GARNET_METAL_INTERNAL_H
#define GARNET_METAL_INTERNAL_H

#import <Foundation/Foundation.h>
#include <stdatomic.h>
#import <Metal/Metal.h>
#import <os/log.h>
#include <os/lock.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/event.h>
#include <crt_externs.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#import <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef GARNET_METAL_MODULAR_BUILD
#define GARNET_INTERNAL        __attribute__((visibility("hidden")))
#define GARNET_INTERNAL_EXTERN extern __attribute__((visibility("hidden")))
#else
#define GARNET_INTERNAL        static
#define GARNET_INTERNAL_EXTERN static
#endif

#ifndef RDNA4_LAZY
#define RDNA4_LAZY 1
#endif
#ifndef RDNA4_9D
#define RDNA4_9D 1
#endif
#ifndef RDNA4_RELEASE
#define RDNA4_RELEASE 0
#endif

#include "garnet_native_blit.h"
#include "garnet_heapalloc.h"
#include "garnet_heap_backing.h"
#include "garnet_devcaps.h"
#include "garnet_system_profile.h"
#include "garnet_gather_caps.h"
#include "garnet_buffer_ranges.h"
#include "garnet_buffer_registry.h"
#include "garnet_spvrefl.h"
#include "garnet_gpl.h"
#include "garnet_argument_scan.h"
#include "garnet_argument_descriptor_scan.h"
#include "garnet_argument_capacity.h"
#include "garnet_argument_epochs.h"
#include "garnet_vkpool.h"
#include "garnet_queue_pipe.h"
#include "garnet_bar_policy.h"
#include "garnet_async_compute.h"
#include "garnet_vram_surf.h"
#include "garnet_argument_summary.h"
#include "garnet_plane.h"
#include "garnet_pitch.h"
#include "garnet_texture_transfer.h"
#include "garnet_hotswap.h"
#include "garnet_dumpacl.h"
#include "garnet_scanabi.h"
#include "garnet_dispflip.h"
#include "garnet_display_commits.h"
#include "garnet_m6x.h"
#include "garnet_m6route.h"
#include "garnet_texdesc.h"
#include "garnet_depth.h"
#include "garnet_cienv.h"
#include "garnet_xlate.h"
#include "garnet_gate.h"
#include "garnet_crc.h"
#include "garnet_cblog.h"
#include "garnet_t1.h"
#include "garnet_impcache.h"
#include "garnet_ledger.h"
#include "garnet_ioalias.h"
#include "garnet_occ.h"
#include "garnet_intfmt.h"
#include "garnet_pool.h"
#include "garnet_nocopy.h"
#include "garnet_argument_limit_snapshot.h"
#include "garnet_drawcache.h"
#include "garnet_gl_surface_pure.h"
#include "garnet_tessellation.h"
#include "garnet_icb_abi.h"
#include "garnet_swizzle.h"
#include "garnet_binary_archive.h"
#include "garnet_dynlib_vft.h"
#include "garnet_rt_runtime.h"
#include "garnet_rt_accel.h"
#include "garnet_sampler.h"
#include "garnet_residency_set.h"
#include "garnet_io_queue.h"
#include "garnet_sparse.h"
#include "garnet_counters.h"
#include "garnet_fencewait.h"
#include "garnet_mesh.h"

// ---- Metal private superclass forward declarations (non-fragile ABI, no ivars) ----
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
- (void)lazyInitialize;
- (id)compiler;
@end

// Metal's generic resource base (exported, F8). Declared without ivars for the same reason.
@interface _MTLResource : NSObject
@end

// Metal's generic heap base (exported; Apple-silicon chain AGXG16GFamilyHeap <- IOGPUMetalHeap <- _MTLHeap <- _MTLAllocation <- _MTLObjectWithLabel). No ivars declared.
@interface _MTLHeap : NSObject
@end

// Metal's generic queue / command buffer / encoder / pipeline bases (exported, F8). No ivars declared.
@interface _MTLCommandQueue : NSObject
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc;
- (void)commandBufferDidComplete:(id)cb startTime:(uint64_t)st completionTime:(uint64_t)ct error:(NSError *)err;
@end
@interface _MTLCommandBuffer : NSObject
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r;
- (void)didScheduleWithStartTime:(uint64_t)st endTime:(uint64_t)et error:(NSError *)err;
- (void)setCurrentCommandEncoder:(id)e;
- (void)commit;
- (BOOL)commitAndWaitUntilSubmitted;
- (void)waitUntilScheduled;
- (void)waitUntilCompleted;
- (MTLCommandBufferStatus)status;
@end
@interface _MTLCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end
@interface _MTLRenderPipelineState : NSObject
@end

// Not an Apple GPU (0x1000013) nor any CPU type; no shipped metallib slice matches it (test-binary-archive checks).
#define RDNA4_TARGET_ARCH_CPUTYPE 0x01000044u
#define RDNA4_TARGET_ARCH_SUBTYPE 0x00000004u
typedef struct { uint16_t major; uint8_t minor; uint8_t update; } RDNA4TargetArchVersion;
_Static_assert(sizeof(RDNA4TargetArchVersion) == 4, "Metal target version ABI");

@interface GarnetDevice : MTLIOAccelDevice {
    NSString *_rdna4Model;
    rdna4dc_native_memory _rdna4NativeMemory;
    uint32_t _rdna4AcceleratorPort;
    uint64_t _rdna4RegistryID;
}
- (rdna4dc_caps)rdna4MemoryCaps;
- (rdna4_gather_caps)rdna4GatherCaps;
@end

// The system's AIRR reader preserves Metal binding names, types and struct layouts.
// These private initializers use an eight-byte flags aggregate on the target OS.
typedef struct { uint64_t bits; } RDNA4ReflectionFlags;
@interface NSObject (RDNA4ReflectionConstruction)
- (NSData *)newSerializedDescriptor;
- (instancetype)initWithSerializedData:(NSData *)data serializedStageInputDescriptor:(NSData *)descriptor device:(id)device options:(NSUInteger)options flags:(RDNA4ReflectionFlags)flags;
- (instancetype)initWithVertexData:(NSData *)vertex fragmentData:(NSData *)fragment serializedVertexDescriptor:(NSData *)descriptor device:(id)device options:(NSUInteger)options flags:(RDNA4ReflectionFlags)flags;
@end


// ---- Shared internal macros ----
enum {
    RDNA4_LOG_DEBUG = 0,
    RDNA4_LOG_DEFAULT = 1,
    RDNA4_LOG_ERROR = 2,
};
#define RDNA4LOG_ERR(fmt, ...)  rdna4_log_lvl(RDNA4_LOG_ERROR, fmt, ##__VA_ARGS__)
#define RDNA4LOG_WARN(fmt, ...) rdna4_log_lvl(RDNA4_LOG_DEFAULT, fmt, ##__VA_ARGS__)
#define RDNA4LOG_KEY(fmt, ...)  rdna4_log_lvl(RDNA4_LOG_DEFAULT, fmt, ##__VA_ARGS__)
#if RDNA4_RELEASE
#define RDNA4LOG(fmt, ...) do { if (rdna4_debug_log_active()) rdna4_log(fmt, ##__VA_ARGS__); } while (0)
#define RDNA4LOGR(fmt, ...) do { if (rdna4_debug_log_active()) { static _Atomic uint64_t n_; uint64_t k_ = atomic_fetch_add_explicit(&n_, 1, memory_order_relaxed); if (k_ < 8 || (k_ & 1023) == 0) rdna4_log(fmt, ##__VA_ARGS__); } } while (0)
#else
#define RDNA4LOG(fmt, ...) rdna4_log(fmt, ##__VA_ARGS__)
#define RDNA4LOGR(fmt, ...) do { static _Atomic uint64_t n_; uint64_t k_ = atomic_fetch_add_explicit(&n_, 1, memory_order_relaxed); if (k_ < 8 || (k_ & 1023) == 0) rdna4_log(fmt, ##__VA_ARGS__); } while (0)
#endif
#define RDNA4_ONCE(fmt, ...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) RDNA4LOG(fmt, ##__VA_ARGS__); } while (0)
#define RDNA4_ONCE_WARN(fmt, ...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) RDNA4LOG_WARN(fmt, ##__VA_ARGS__); } while (0)
#if RDNA4_RELEASE
#define RDNA4_DNR(cls) \
    - (void)doesNotRecognizeSelector:(SEL)sel { \
        os_log_error(rdna4_oslog(), "GarnetMetal: UNRECOGNIZED selector %{public}s on " #cls, sel_getName(sel)); \
        RDNA4LOG("UNRECOGNIZED selector %s on " #cls, sel_getName(sel)); \
        [super doesNotRecognizeSelector:sel]; }
#else
#define RDNA4_DNR(cls) \
    - (void)doesNotRecognizeSelector:(SEL)sel { \
        RDNA4LOG("UNRECOGNIZED selector %s on " #cls, sel_getName(sel)); \
        [super doesNotRecognizeSelector:sel]; }
#endif
#define RDNA4_KILL_FILE          "/private/tmp/garnet-off"
#define RDNA4_LEGACY_KILL_FILE   "/private/tmp/rdna4m-off"
#define RDNA4_STARTS_FILE        "/private/tmp/garnet-starts"
#define RDNA4_OK_FILE            "/private/tmp/garnet-ok"
#define RDNA4_HEADLESS_NO        "/private/tmp/garnet-headless-no"
#define RDNA4_LEGACY_HEADLESS_NO "/private/tmp/rdna4m-headless-no"
#define RDNA4_NOPLANES_FILE      "/private/tmp/garnet-noplanes"
GARNET_INTERNAL_EXTERN const char *garnet_getenv(const char *newName, const char *legacyName);
#define RDNA4_CBLOG_INV_LOGGED 20
#define RDNA4S_FOURCC_BGRA 0x42475241u   // 'BGRA'
#define RDNA4_OPT_NOZERO (1ULL << 40)   // internal marker in RDNA4Buffer's options: the contents need not be zeroed (the upload ring)
#define RDNA4_RES_IVARS NSUInteger _prot; int _respPid; BOOL _respSet; _Atomic uint64_t _uid; RDNA4Heap *_hpHeap; NSUInteger _hpOff, _hpSize; uint64_t _hpId; BOOL _hpAl; \
    uint64_t _hpBlk, _hpLease, _hpBindOff, _hpPhysSize; BOOL _hpDedicated; RDNA4HeapBacking *_hpBacking;
#define RDNA4_HEAP_RETIRE do { if (_hpHeap && (_hpId || _hpLease) && !_hpAl) { _hpAl = YES; [_hpHeap rdna4RetireResource:self logical:_hpId lease:_hpLease]; } } while (0)
#define RDNA4_HEAP_DEALLOC do { \
    RDNA4_HEAP_RETIRE; \
    if (_hpHeap && _hpBlk) { \
        uint64_t b_ = _hpBlk; \
        [_hpHeap rdna4DetachBacking:_hpBacking block:b_ lease:_hpLease]; \
        _hpBacking = nil; \
        _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0; _hpDedicated = NO; \
    } \
} while (0)
#define RDNA4_RESOURCE_SPI_CUSTOM_UID(uidExpr) \
    - (id)heap { return [self rdna4ResourceHeap]; } \
    - (NSUInteger)heapOffset { return [self rdna4ResourceHeapOffset]; } \
    - (BOOL)isAliasable { return [self rdna4ResourceAliasable]; } \
    - (void)makeAliasable { [self rdna4ResourceMakeAliasable]; } \
    - (NSUInteger)setPurgeableState:(NSUInteger)st { (void)st; return 2; } \
    - (BOOL)isPurgeable { return NO; } \
    - (int)setOwnerWithIdentity:(uint32_t)o { (void)o; return (int)0xE00002C2; } \
    - (BOOL)doesAliasResource:(id)o { return [self rdna4ResourceAliases:o]; } \
    - (BOOL)doesAliasAnyResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { \
        if (!o) return NO; for (NSUInteger i_ = 0; i_ < n; i_++) if ([self doesAliasResource:o[i_]]) return YES; return NO; } \
    - (BOOL)doesAliasAllResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { \
        if (!o || !n) return NO; for (NSUInteger i_ = 0; i_ < n; i_++) if (![self doesAliasResource:o[i_]]) return NO; return YES; } \
    - (BOOL)isComplete { return YES; } \
    - (BOOL)isWriteComplete { return YES; } \
    - (void)waitUntilComplete {} \
    - (NSUInteger)protectionOptions { return _prot; } \
    - (int)responsibleProcess { return _respSet ? _respPid : (int)getpid(); } \
    - (void)setResponsibleProcess:(int)p { _respPid = p; _respSet = YES; } \
    - (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; } \
    - (uint64_t)uniqueIdentifier { return (uidExpr); } \
    - (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) { MTLResourceID identifier; identifier._impl = self.uniqueIdentifier; return identifier; }
#define RDNA4_RESOURCE_SPI RDNA4_RESOURCE_SPI_CUSTOM_UID(rdna4_resource_uid(&_uid))
#define RDNA4_ENCODER_SHARED \
    - (void)useResource:(id)r usage:(NSUInteger)u { if (_ended) { [_cb rdna4Fail:@"useResource after endEncoding"]; return; } [_cb rdna4UseResource:r usage:u]; if (r && ![_cb rdna4Error]) { if (u & MTLResourceUsageWrite) { if (!_argumentWritable) _argumentWritable = [NSMutableSet set]; [_argumentWritable addObject:r]; } if (!_argumentResources) _argumentResources = [NSMutableSet set]; [_argumentResources addObject:r]; } } \
    - (void)useResource:(id)r usage:(NSUInteger)u stages:(NSUInteger)st { (void)st; [self useResource:r usage:u]; } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u { if (n && !r) { [_cb rdna4Fail:@"useResources has a NULL resource array"]; return; } for (NSUInteger i = 0; i < n; ++i) [self useResource:r[i] usage:u]; } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u stages:(NSUInteger)st { (void)st; [self useResources:r count:n usage:u]; } \
    - (void)useHeap:(id)h { if (_ended) { [_cb rdna4Fail:@"useHeap after endEncoding"]; return; } [_cb rdna4UseHeap:h]; if (h && ![_cb rdna4Error]) { if (!_argumentWritable) _argumentWritable = [NSMutableSet set]; [_argumentWritable addObject:h]; if (!_argumentResources) _argumentResources = [NSMutableSet set]; [_argumentResources addObject:h]; } } \
    - (void)useHeap:(id)h stages:(NSUInteger)st { (void)st; [self useHeap:h]; } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n { if (n && !h) { [_cb rdna4Fail:@"useHeaps has a NULL heap array"]; return; } for (NSUInteger i = 0; i < n; ++i) [self useHeap:h[i]]; } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n stages:(NSUInteger)st { (void)st; [self useHeaps:h count:n]; } \
    - (void)useResourceGroup:(id)g usage:(NSUInteger)u stages:(NSUInteger)st { (void)g; (void)u; (void)st; RDNA4_ONCE("useResourceGroup: ignored"); }
#define RDNA4F_A8   1u
#define RDNA4F_SINT 8u
#define RDNA4F_UINT 4u   // bundle 19: an unsigned-integer colour format (RG16Uint): clears fill .uint32, the magenta fallback is masked off, /private/tmp/garnet-nouint restores the old refusal. (2u is RDNA4F_DS below.)
#define RDNA4F_BC   16u  // 4x4 block-compressed format; bpp holds bytes per 4x4 block (8 or 16)
#define RDNA4_HAS_MULTI_DRAW_INDIRECT 1
#define RDNA4_HAS_MESH_PIPELINE_QUERY 1
#define RDNA4_COMPUTE_ICB_ENABLED 1
#define RDNA4_BINARY_ARCHIVE_IMPL_INC 1
#define RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS ((NSUInteger)1u << 2)
#define RDNA4X_STAGE3_FFI_DEFINED 1
#define RDNA4_LK_SCOPE rdna4_lkscope lks_ __attribute__((cleanup(rdna4_lk_scope_end))) = { 0 }
#define RDNA4_ARGUMENT_NULL_IMAGES 48u
#define RDNA4_VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkGetDescriptorSetLayoutSupport) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdDraw) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdCopyBufferToImage) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCreateComputePipelines) X(vkCmdSetBlendConstants) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkResetFences) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetImageSubresourceLayout) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceImageFormatProperties2) X(vkGetDeviceProcAddr) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkGetQueryPoolResults) X(vkGetFenceStatus) /* build 18 (P2) */ \
    X(vkCmdResolveImage) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) \
    X(vkCreateRenderPass2) X(vkCmdBeginRenderPass2) X(vkCmdEndRenderPass2)
#define RDNA4XI(inst) (RDNA4X.i[(inst)])

// ---- Forward ObjC class declarations ----
@class GarnetASHandle;
@class GarnetASPrimData;
@class GarnetASStorage;
@class GarnetAccelerationStructure;
@class GarnetAccelerationStructureCommandEncoder;
@class GarnetDevice;
@class GarnetGLDrawable;
@class RDNA4ArgumentCompletedWrite;
@class RDNA4ArgumentEncoder;
@class RDNA4ArgumentField;
@class RDNA4ArgumentLayout;
@class RDNA4ArgumentPool;
@class RDNA4ArgumentResourceToken;
@class RDNA4ArgumentSamplerAlias;
@class RDNA4ArgumentSamplerOwners;
@class RDNA4ArgumentWriteIntent;
@class RDNA4BinaryArchive;
@class RDNA4BlitEncoder;
@class RDNA4Buffer;
@class RDNA4CommandBuffer;
@class RDNA4CommandQueue;
@class RDNA4CommandSegment;
@class RDNA4ComputeEncoder;
@class RDNA4ComputePipelineState;
@class RDNA4Counter;
@class RDNA4CounterSampleBuffer;
@class RDNA4CounterSet;
@class RDNA4DepthStencilState;
@class RDNA4DynamicLibrary;
@class RDNA4Fence;
@class RDNA4FenceCheckpoint;
@class RDNA4FunctionHandle;
@class RDNA4Heap;
@class RDNA4HeapBacking;
@class RDNA4ICBArgOwners;
@class RDNA4ICBRecord;
@class RDNA4IOCommand;
@class RDNA4IOCommandBuffer;
@class RDNA4IOCommandQueue;
@class RDNA4IOFileHandle;
@class RDNA4IndirectCommandBuffer;
@class RDNA4IntersectionFunctionTable;
@class RDNA4LinkCtx;
@class RDNA4MeshEncoderState;
@class RDNA4MeshRenderPipelineState;
@class RDNA4RTArgOwners;
@class RDNA4RTQueryPool;
@class RDNA4RTStagingBuffer;
@class RDNA4ReflectionCompiler;
@class RDNA4RenderEncoder;
@class RDNA4RenderHandles;
@class RDNA4RenderPipelineState;
@class RDNA4ResidencySet;
@class RDNA4ResidencySnapshot;
@class RDNA4ResourceBindings;
@class RDNA4ResourceStateEncoder;
@class RDNA4SamplerBinding;
@class RDNA4SamplerState;
@class RDNA4SamplerStorage;
@class RDNA4SparseBackingPool;
@class RDNA4SparseHeap;
@class RDNA4SparsePendingOp;
@class RDNA4SparseTexture;
@class RDNA4Texture;
@class RDNA4VisibleFunctionTable;

// ---- Shared internal protocols ----
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

@protocol RDNA4ICBInternal <NSObject>
- (id<MTLDevice>)device;
- (uint64_t)rdna4EnsureGPUBackingAddress;
- (id)rdna4GPUBacking;
- (BOOL)rdna4PrepareForGPUProducerOnCommandBuffer:(id)cb;
- (BOOL)rdna4RegisterCandidatePipeline:(id)pipeline;
@end

@protocol RDNA4ComputeICBInternal <RDNA4ICBInternal>
- (BOOL)rdna4IsComputeICB;
- (BOOL)rdna4UseGPUExecution;
- (NSUInteger)rdna4ExecuteComputeOnEncoder:(id)encoder range:(NSRange)range reason:(NSString **)reason;
- (void)rdna4ExecuteComputeGPUOnEncoder:(id)encoder
                            directRange:(NSRange)range
                         indirectBuffer:(id)indirectBuf
                         indirectOffset:(NSUInteger)indirectOff
                            useIndirect:(BOOL)useIndirect;
@end

@protocol GarnetASInternal <NSObject>
- (GarnetASHandle *)rdna4CurrentHandle;
- (GarnetASPrimData *)rdna4CurrentPrimData;
@end

@protocol RDNA4ResidencySnapshotInternal <NSObject>
- (NSArray *)rdna4Allocations;
- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device;
@end

@protocol RDNA4ResidencySetInternal <NSObject>
- (id)device;
- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot;
@end

@protocol RDNA4CounterSampleBufferInternal <NSObject>
- (id)device;
- (NSUInteger)sampleCount;
- (MTLStorageMode)rdna4StorageMode;
- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb
                   vkCommandBuffer:(VkCommandBuffer)cmd
                       sampleIndex:(NSUInteger)sampleIndex
                           barrier:(BOOL)barrier
                        stageFlags:(VkPipelineStageFlagBits)stage
                               who:(const char *)who;
- (BOOL)rdna4ResolveRange:(NSRange)range
                intoBytes:(uint64_t *)dst
                    error:(NSError **)err;
@end

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


// ---- Shared internal C structs & typedefs ----
enum { RDNA4NC_ALIGN_STORAGE, RDNA4NC_ALIGN_UNIFORM, RDNA4NC_ALIGN_INDEX, RDNA4NC_ALIGN_INDIRECT, RDNA4NC_ALIGN_IMAGE };
typedef NS_ENUM(int, RDNA4FenceCheckpointState) {
    RDNA4FenceCheckpointPending = 0,
    RDNA4FenceCheckpointReady = 1,
    RDNA4FenceCheckpointFailed = 2,
};
enum {
    RDNA4_RESOURCE_PIPELINE = 1,
    RDNA4_RESOURCE_DEPTH_STENCIL = 2,
    RDNA4_RESOURCE_ATTACHMENT = 3,
    RDNA4_RESOURCE_RESOLVE = 4,
    RDNA4_RESOURCE_BUFFER = 5,
    RDNA4_RESOURCE_TEXTURE = 6,
    RDNA4_RESOURCE_ACCEL = 7,
};
typedef struct { MTLPixelFormat mtl; VkFormat vk; uint32_t bpp; uint32_t flags; } RDNA4Fmt;
typedef struct {
    __unsafe_unretained id as[RDNA4RTR_MAX_BUFFER_SLOTS];
    __unsafe_unretained id ift[RDNA4RTR_MAX_BUFFER_SLOTS];
    uint32_t iftMask;
} RDNA4RTStageSlots;
typedef struct {
    __unsafe_unretained RDNA4Buffer *b;
    NSUInteger off;
    VkBuffer vkBuf;
    VkDeviceSize vkOff;
    NSUInteger len;
    __unsafe_unretained id dev;
} RDNA4BufSlot;
typedef struct {
    RDNA4BufSlot buf[31]; __unsafe_unretained RDNA4Texture *tex[128]; __unsafe_unretained RDNA4SamplerBinding *smp[16]; __unsafe_unretained RDNA4Texture *att[8];   // att: the colour attachments (input attachments, 11e-2)
    uint32_t roBufMask;   // bit i set iff buf[i].b is a host-read-only no-copy buffer
    uint32_t bufMask;     // bit i set iff buf[i].b != nil
    uint64_t texMaskLo, texMaskHi; // bit i set iff tex[i] != nil
    uint16_t smpMask;     // bit i set iff smp[i] != nil
    uint8_t attMask;      // bit i set iff att[i] != nil
    uint32_t asMask;      // bit i set iff rt.as[i] != nil
    RDNA4RTStageSlots rt;
    const rdna4rtr_as_reflection_t *rtAsRefl;
    const rdna4_argument_scan_reflection_plan *scanPlan;
} RDNA4StageState;
typedef struct {
    const RDNA4StageState *state;
    rdna4_as_work *works;
    size_t count;
    rdna4_as_work_stats stats;
} RDNA4ArgumentScanStage;
typedef struct {
    RDNA4ArgumentScanStage stages[2];
    size_t count;
} RDNA4ArgumentScanPrepared;
typedef struct { int kind; VkDeviceMemory mem; uint8_t *map; uint64_t slab, off, size; uint32_t key; } RDNA4Mem;   // kind 0 = own allocation, 1 = slab range, 2 = whole (recycled / recyclable)
typedef struct {
    const RDNA4Fmt *fmt;
    rdna4td_t td;
    BOOL isDS, is1D, is3D, arr1D, canRT, wantSt;
    NSUInteger levels, depth, aspects, samples;
    VkImageCreateInfo create;
} rdna4_heap_image_plan;
typedef struct { unsigned long cmp; int write; rdna4ds_face_in_t f, b; uint32_t fRead, fWrite, bRead, bWrite; } RDNA4DSInfo;
typedef int (*rdna4s_query_t)(VkDevice, struct rdna4n_scan_query *);
typedef int (*rdna4s_acquire_t)(VkDevice, uint64_t *, uint64_t *);
typedef int (*rdna4s_register_t)(VkDevice, VkDeviceMemory, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint64_t *);
typedef int (*rdna4s_present_t)(VkDevice, uint32_t, uint64_t[3]);
typedef int (*rdna4s_status_t)(VkDevice, struct rdna4n_scan_status *);
typedef int (*rdna4s_release_t)(VkDevice, uint64_t[2]);
typedef int (*rdna4x_call_t)(VkDevice, uint32_t, const uint64_t *, uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
typedef int (*rdna4x_bohandle_t)(VkDevice, VkDeviceMemory, uint32_t *);
typedef struct {
    rdna4df_t sm;
    int enabled;                                     // decided at bind (rdna4x_enabled)
    VkDeviceMemory mem[RDNA4DF_MAX_SLOTS]; VkBuffer buf[RDNA4DF_MAX_SLOTS]; uint32_t sid[RDNA4DF_MAX_SLOTS];   // sid = the TAGGED kernel slot id (tag | k)
    struct rdna4n_scan_status st; uint32_t gen; int gen_known;      // the last Status and its table generation
    rdna4x_stats_t xs;
    uint64_t plan_count, presents_seen;
} rdna4xi_t;
typedef struct { IOSurfaceRef ios; const uint8_t *base; size_t pitch, rowbytes; uint32_t h, step, sid; int slot, unst; uint64_t seq; rdna4crc_samp_t a1; } rdna4crc_rec_t;
typedef struct {
    const uint8_t *name;
    size_t name_len;
    const uint8_t *ll;
    size_t ll_len;
} rdna4x_cdynlib;
typedef struct {
    const uint8_t *symbol;
    size_t symbol_len;
    const uint8_t *ll;
    size_t ll_len;
    uint32_t kind;
} rdna4x_cift_fn;
typedef int (*rdna4x_detect_stage_fn)(
    const uint8_t *ll, size_t ll_len,
    uint32_t *out_stage_kind,
    char *err, size_t err_len);
typedef int (*rdna4x_list_exported_symbols_fn)(
    const uint8_t *ll, size_t ll_len,
    uint8_t **out_json, size_t *out_json_len,
    char *err, size_t err_len);
typedef int (*rdna4x_translate_linked_libraries_fn)(
    const uint8_t *ll, size_t ll_len,
    const uint32_t *local_xyz,
    const rdna4x_cdep *deps, size_t ndeps,
    const rdna4x_cdynlib *dynlibs, size_t ndynlibs,
    const rdna4x_cdep *visible_fns, size_t nvisible_fns,
    const rdna4x_backend_capabilities *capabilities,
    uint8_t **spv, size_t *spv_len,
    uint8_t **meta, size_t *meta_len,
    char *err, size_t err_len);
typedef int (*rdna4x_translate_stage3_fn)(
    const uint8_t *ll, size_t ll_len,
    const uint32_t *local_xyz,
    const rdna4x_cdep *deps, size_t ndeps,
    const rdna4x_cdynlib *dynlibs, size_t ndynlibs,
    const rdna4x_cdep *visible_fns, size_t nvisible_fns,
    const rdna4x_cift_fn *intersection_fns, size_t nintersection_fns,
    const rdna4x_backend_capabilities *capabilities,
    uint8_t **spv, size_t *spv_len,
    uint8_t **meta, size_t *meta_len,
    char *err, size_t err_len);
typedef struct { char unused; } rdna4_lkscope;
typedef struct {
    const char *name;
    const uint8_t *bytes;
    size_t len;
    uint32_t kind;
} rdna4x_stage2_in_t;
typedef struct {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;
    VkSampler stat;
    BOOL addressTable;
    uint32_t addressTableVersion;
    BOOL embedded;
    uint32_t embeddedBuffer;
    uint32_t embeddedOffset;
    BOOL comparisonSampler;
    BOOL gpuArguments, argumentTable, argumentRoot;
    BOOL normalizedCoordinates;
    BOOL samplerCoordinateFlags;
    BOOL pixelCoordinates;
    BOOL stageResourceMetadata, stageTokenTable;
    BOOL rtPrimDataTable;
    uint8_t imageSampleKinds;
    uint8_t texelComponentKinds;
    uint16_t nullShapeCount;
    uint32_t nullShapeRemap[64];
    uint8_t nullShapeAccess[64];
} RDNA4PB;
typedef enum {
    RDNA4CoordinateFlagOK,
    RDNA4CoordinateFlagRange,
    RDNA4CoordinateFlagConflict,
} RDNA4CoordinateFlagResult;
typedef struct {
    VkPipelineCache owned;
    VkPipelineCache prev;
} rdna4_ba_vkcache_scope_t;
typedef enum {
    RDNA4NC_COPY_BUFFER, RDNA4NC_TEXTURE_BUFFER, RDNA4NC_BUFFER_TEXTURE, RDNA4NC_COPY_TEXTURE,
    RDNA4NC_FILL, RDNA4NC_MIPMAPS, RDNA4NC_WRITEBACK, RDNA4NC_ATTACHMENT, RDNA4NC_RESOLVE,
    RDNA4NC_COMPUTE, RDNA4NC_VERTEX, RDNA4NC_FRAGMENT, RDNA4NC_VISIBILITY, RDNA4NC_OP_COUNT
} RDNA4NCOp;
typedef struct { uint32_t local[3], groups[3], tbase[3], gbase[3]; } RDNA4Region;
typedef struct {
    VkFence fence;
    uint64_t cbid;
    uint64_t t1;
} rdna4_fence_wait_ctx;
typedef struct {
    VkDescriptorSetLayout dsl[3];
    uint32_t capacity[3];
    BOOL variable[3];
    BOOL active;
    BOOL imageQueries;
    BOOL comparisonSamplers;
    BOOL samplerCoordinates;
    uint64_t nullImages;
    BOOL shapeRegistry;
    BOOL abi10;
    BOOL texel[2];
    uint8_t texelComponents[2];
    BOOL dynamicTexel[2];
    BOOL texelVariable[2];
    uint32_t texelCapacity[2];
    BOOL imageTable[3];
    uint32_t texelStages[2];
    uint32_t tableStages[3];
    rdna4_argument_descriptor_usage ordinaryUsage;
    uint64_t fragmentColorAttachments;
    BOOL descriptorBudgetReady;
    rdna4ns_registry nullShapes;
    BOOL tokenTableSeen, tokenTableNormalized;
} RDNA4ArgumentTables;
typedef enum {
    RDNA4IOCMD_LOAD_BYTES = 1,
    RDNA4IOCMD_LOAD_BUFFER,
    RDNA4IOCMD_LOAD_TEXTURE,
    RDNA4IOCMD_BARRIER,
    RDNA4IOCMD_WAIT_EVENT,
    RDNA4IOCMD_SIGNAL_EVENT,
    RDNA4IOCMD_COPY_STATUS,
} RDNA4IOCommandKind;

// ---- Shared global state struct types ----
typedef struct {
    BOOL tried; BOOL ok; NSError *err;
    void *lib; VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp; pthread_mutex_t qlock; VkPhysicalDeviceLimits lim; VkCommandPool opool; VkPipelineCache pc;   // pc: T2 persisted pipeline cache (VK_NULL_HANDLE = none)
    BOOL occPrecise, occOK;   // build 18 (P2): occlusionQueryPrecise enabled on the device; every query entry point resolved
    BOOL dbClamp;   // bundle 10: depthBiasClamp enabled on the device (a non-zero clamp in setDepthBias is passed on only then)
    BOOL hostExt, drmModExt; uint64_t hostAlign; uint64_t impTotal;   // host pointer import and explicit DRM-modifier linear images
    BOOL orderedColorAccess, indirectFirstInstance, cubeArray, bufferAddress, shaderInt64;
    BOOL fragInterlock;
    BOOL dualSrcBlend, depthClamp, fillModeNonSolid, multiViewport, bcCompression, sampleRateShading;
    BOOL storageImageExtFormats, storageImageMultisample;
    BOOL sampleLocationsExt, variableSampleLocations;
    VkSampleCountFlags sampleLocationCounts; VkExtent2D sampleLocationGrid; float sampleLocationRange[2];
    VkResolveModeFlags depthResolveModes, stencilResolveModes;
    BOOL dsResolveIndep, dsResolveIndepNone;
    uint32_t maxViewports;
    BOOL tessellation; uint32_t maxTessGenLevel, maxTessPatchSize;
    BOOL descriptorIndexing, nonUniformSampledImages, nonUniformStorageImages, nonUniformBuffers;
    BOOL gpuArgumentTables, nullDescriptors, samplerAnisotropy, samplerMirrorClamp;
    BOOL robustTexelBuffers;
    BOOL uniformTexelBuffers, storageTexelBuffers;
    VkPhysicalDeviceVulkan12Properties descriptorLimits;
    uint32_t maxPerSetDescriptors;
    BOOL barycentric, atomicFloat, atomicFloat2, atomicInt64;
    BOOL meshShader, objectShader, multiDrawIndirect;
    BOOL deferredHostOps, accelStruct, rayQuery, rtPositionFetch;
    BOOL timestampQuery, timestampComputeAndGraphics;
    float timestampPeriod;
    uint32_t timestampValidBits;
    BOOL sparseBinding, sparseResidencyBuffer, sparseResidencyImage2D, sparseResidencyImage3D;
    // Stage 2B: real Vulkan entry points captured at radv open; pooling bypasses when test hooks override them.
    PFN_vkCreateFence realCreateFence;
    PFN_vkDestroyFence realDestroyFence;
    PFN_vkResetFences realResetFences;
    PFN_vkAllocateCommandBuffers realAllocCmd;
    PFN_vkFreeCommandBuffers realFreeCmd;
    PFN_vkResetCommandBuffer realResetCmd;
    PFN_vkFreeMemory realFreeMem;
    PFN_vkCreateFramebuffer realCreateFB;
    PFN_vkDestroyFramebuffer realDestroyFB;
    PFN_vkAllocateDescriptorSets realAllocDesc;
    BOOL pushDescExt;
    uint32_t maxPushDesc;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescSet;
    garnet_bar_policy_t barPolicy;
    VkQueue computeQ; uint32_t computeQfi; uint32_t sharedQfis[2]; pthread_mutex_t computeQlock; BOOL asyncComputeOn;
    PFN_vkWaitSemaphores waitSemaphores;
    // GPL fast-link + Vulkan 1.3 / EDS3 dynamic states (zero Mesa changes).
    BOOL gplExt, eds3PolyMode, eds3DepthClamp;
    PFN_vkCmdSetCullMode cmdSetCullMode;
    PFN_vkCmdSetFrontFace cmdSetFrontFace;
    PFN_vkCmdSetPrimitiveTopology cmdSetPrimitiveTopology;
    PFN_vkCmdSetPrimitiveRestartEnable cmdSetPrimitiveRestartEnable;
    PFN_vkCmdSetDepthTestEnable cmdSetDepthTestEnable;
    PFN_vkCmdSetDepthWriteEnable cmdSetDepthWriteEnable;
    PFN_vkCmdSetDepthCompareOp cmdSetDepthCompareOp;
    PFN_vkCmdSetStencilTestEnable cmdSetStencilTestEnable;
    PFN_vkCmdSetStencilOp cmdSetStencilOp;
    PFN_vkCmdSetPolygonModeEXT cmdSetPolygonMode;
    PFN_vkCmdSetDepthClampEnableEXT cmdSetDepthClampEnable;
} RDNA4BackendState;
typedef struct { rdna4t_hist spv, shm, gfx, cmp, imp, enc, gpu, wake, qlk, fgap, fgpu; } RDNA4T1State;
typedef struct { BOOL on; BOOL auditSubmit; pthread_mutex_t mu; rdna4p_pool p; BOOL reqOK; uint64_t lastTrim, lastSig; unsigned unchanged;
    PFN_vkGetBufferMemoryRequirements2 bmr2; PFN_vkGetImageMemoryRequirements2 imr2; PFN_vkResetDescriptorPool rdp;
    garnet_vkpool_t fences; garnet_vkpool_t oneshotCmds; } RDNA4PoolState;
typedef struct { BOOL on; pthread_mutex_t mu; rdna4dc_rpcache *rp; _Atomic uint64_t draws, setsReused, setsAlloc, pipesSkipped, pipesBound, pcHits, rpHits, rpCreated, rpUncached; uint64_t lastSig; } RDNA4DrawOptState;
typedef struct { pthread_mutex_t mu; rdna4l L; int ucMode, iaActs; _Atomic uint64_t iaFlush, iaReup, iaWsFlush, iaWsReup, f1Fallbacks, ucTake, ucRelease; uint64_t lastSig; } RDNA4LedgerState;
typedef struct {
    pthread_mutex_t mu; pthread_cond_t holdCond; rdna4df_hold_adapt_t holdAdapt; rdna4df_t sm; int bound;
    rdna4_display_commit *commits;
    NSMutableIndexSet *incompleteSources;
    NSHashTable *incompleteImages;
    uint64_t incompleteSinceNs, incompleteLastFallbackNs;
    BOOL incompleteCapLogged;
    rdna4s_query_t fq; rdna4s_acquire_t fa; rdna4s_register_t fg; rdna4s_present_t fp; rdna4s_status_t fs; rdna4s_release_t fr;
    uint32_t pw, ph, pitch; uint64_t slotBytes, preallocBytes; int vtype;
    VkDeviceMemory mem[RDNA4DF_MAX_SLOTS]; VkBuffer buf[RDNA4DF_MAX_SLOTS]; uint32_t sid[RDNA4DF_MAX_SLOTS];
    uint64_t allocBytes[RDNA4DF_MAX_SLOTS], reqBytes[RDNA4DF_MAX_SLOTS], alignBytes[RDNA4DF_MAX_SLOTS]; int memType[RDNA4DF_MAX_SLOTS];
    rdna4cc cc; int ndisp, ncand; dispatch_source_t timer; uint64_t tStart, tLog; int exitSet;
    struct rdna4n_scan_status st;
    void *map[RDNA4DF_MAX_SLOTS];   // CRC diagnostic: persistent CPU mapping of each (host-visible) slot, made on first use
    dispatch_queue_t pq; _Atomic uint64_t seq, multi, cbno;   // ONE serial present queue; the frame sequence counter (taken under the submit lock)
    rdna4m6_t m6;   // bundle 11 (M6 Stage 1a): the kernel latch and its IOSurface ID -> display table; written under mu
    struct { uint32_t sid, w, h, bpr; } sg[64]; unsigned sgi;   // bundle 15: the geometry of every classified display surface (written by the classifier, read by the size gate); under mu
} RDNA4ScanoutState;
typedef struct { rdna4x_call_t call; rdna4x_bohandle_t bohandle; rdna4xi_t i[3]; } RDNA4XScanoutState;
typedef struct { pthread_mutex_t mu; rdna4crc_stats_t st; _Atomic int on; _Atomic uint32_t step; _Atomic uint64_t next; _Atomic uint64_t logged_p; _Atomic int noslot; int warmed, mapfail; struct { uint32_t sid; uint64_t seq; } surf[8]; } RDNA4CRCState;
typedef struct { _Atomic int on; int fd; uint64_t bytes; int capped; uint64_t inv_logged; unsigned tick; pthread_mutex_t mu; rdna4cb_t trk; rdna4cba_t activity; } RDNA4CBLogState;
typedef struct {
    pthread_mutex_t mu;
    int gateDone, killed, envDirty;       // read once per process (never in WindowServer)
    int dirState;                         // 0 not set up, 1 disk ready, 2 Tier-2 64 MiB memory-only fallback, -1 unavailable
    int dirTries; uint64_t dirNextNs;     // bounded retries of a failed setup
    char dir[RDNA4X_PATH_MAX];
    NSString *dirNS;
    int memcacheInit; rdna4x_memcache_t memcache;
    int engineInit; rdna4x_engine eng;
    int icbXlibTried; rdna4x_xlib icbXlib; char icbXlibErr[256];
    rdna4x_detect_stage_fn detectStage;
    rdna4x_list_exported_symbols_fn listExportedSymbols;
    rdna4x_translate_linked_libraries_fn translateLinkedLibraries;
    rdna4x_translate_stage3_fn translateStage3;
    int stage2ReaderTried; rdna4x_reader stage2Reader; char stage2ReaderErr[256];
    int sandboxedLatched;
} RDNA4XlateState;

// ---- Shared ObjC @interface declarations ----
@interface RDNA4HeapBacking : NSObject
- (instancetype)initWithBuffer:(VkBuffer)buffer image:(VkImage)image view:(VkImageView)view;
- (void)addView:(VkImageView)view;
- (void)destroyHandles;
@end

@interface RDNA4Heap : _MTLHeap
- (MTLHeapType)type;
- (NSUInteger)size;
- (void)rdna4Free:(uint64_t)bid;
- (BOOL)rdna4AcquireBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                           dedicated:(BOOL)dedicated hostImport:(BOOL)hostImport
                                 mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                               block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                               error:(NSError **)err;
- (BOOL)rdna4AcquirePlacementBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                                       offset:(NSUInteger)placeOffset hostImport:(BOOL)hostImport
                            requiresDedicated:(BOOL)requiresDedicated
                                          mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                                        block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                                        error:(NSError **)err;
- (BOOL)rdna4BindSucceeded:(uint64_t)block lease:(uint64_t)lease owner:(id)owner;
- (void)rdna4AbandonBacking:(uint64_t)lease;
- (void)rdna4CloseBackingHandle:(uint64_t)block;
- (void)rdna4DetachBacking:(RDNA4HeapBacking *)backing block:(uint64_t)block lease:(uint64_t)lease;
- (void)rdna4RetireResource:(id)resource logical:(uint64_t)bid lease:(uint64_t)lease;
- (BOOL)rdna4ReserveRequirements:(VkMemoryRequirements)mr offset:(NSUInteger *)offset id:(uint64_t *)bid;
- (VkDeviceMemory)rdna4BackingMemory:(uint64_t)block;
- (uint8_t *)rdna4BackingMap:(uint64_t)block;
- (BOOL)rdna4Block:(uint64_t)block offset:(uint64_t)offset size:(uint64_t)size aliases:(id)other;
- (uint64_t)rdna4BackingBytes;
- (uint64_t)rdna4BackingInUse;
- (void)rdna4Reap;
- (void)rdna4TrackResource:(id)resource;
- (NSArray *)rdna4LiveResources;
@end

@interface RDNA4Buffer : _MTLResource {
    id _dev; NSUInteger _len; void *_ptr; VkBuffer _vkbuf; VkDeviceMemory _vkmem; NSString *_lbl; NSUInteger _opts; NSUInteger _asz; uint64_t _gpuAddress;
    VkDeviceSize _vkoff; uint64_t _impSpan, _ownedHostSpan;   // client range within the imported buffer; never a pool/heap memory-binding offset
    RDNA4_RES_IVARS
    BOOL _noacct;   // heap resource: the heap's size is in rdna4_alloc_total, not this buffer's memory
    RDNA4Mem _pm; BOOL _pooled;   // P1: the memory comes from the pool (slab range / recycled allocation / own allocation made through rdna4_mem_alloc); _vkmem is then NULL
    void (^_deallocator)(void *, NSUInteger); BOOL _imported; BOOL _hostReadOnly;   // no-copy buffer: client memory imported as host memory; the block is called once, at release
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap offset:(NSUInteger)offset error:(NSError **)err;
- (RDNA4Heap *)rdna4ResourceHeap;
- (NSUInteger)rdna4ResourceHeapOffset;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err;
- (instancetype)rdna4ImportHostPointer:(void *)p length:(NSUInteger)len ownedSpan:(uint64_t)ownedSpan deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err;
- (BOOL)rdna4Imported;
- (BOOL)rdna4HostReadOnly;
- (void)rdna4CheckHostReadOnly:(const void *)p length:(NSUInteger)len;
- (VkDeviceMemory)rdna4NativeMemory:(uint64_t *)offset;
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid;
- (VkBuffer)vkBuffer;
- (VkDeviceSize)rdna4VkOffset;
- (id)device;
- (NSUInteger)length;
- (uint64_t)gpuAddress;
- (uint64_t)rdna4BackingBlock;
- (uint64_t)rdna4BackingOffset;
- (uint64_t)rdna4BackingSize;
- (void)rdna4AbortHeapConstruction;
- (void)rdna4DestroyRetiredBacking;
- (BOOL)rdna4ZeroLargePrivate:(NSUInteger)len unresolved:(BOOL *)unresolved error:(NSError **)err;
@end

static inline void rdna4_stage_set_buf(RDNA4StageState *s, NSUInteger i, RDNA4Buffer *b, NSUInteger off) {
    s->buf[i].b = b;
    s->buf[i].off = off;
    if (b) {
        s->buf[i].vkBuf = [b vkBuffer];
        s->buf[i].vkOff = [b rdna4VkOffset];
        s->buf[i].len = [b length];
        s->buf[i].dev = [b device];
        s->bufMask |= (1u << i);
        if ([b rdna4HostReadOnly]) s->roBufMask |= (1u << i);
        else s->roBufMask &= ~(1u << i);
    } else {
        s->buf[i].vkBuf = VK_NULL_HANDLE;
        s->buf[i].vkOff = 0;
        s->buf[i].len = 0;
        s->buf[i].dev = nil;
        s->bufMask &= ~(1u << i);
        s->roBufMask &= ~(1u << i);
    }
}
static inline void rdna4_stage_set_tex(RDNA4StageState *s, NSUInteger i, RDNA4Texture *t) {
    s->tex[i] = t;
    if (i < 64u) {
        if (t) s->texMaskLo |= (1ULL << i); else s->texMaskLo &= ~(1ULL << i);
    } else {
        const unsigned bit = (unsigned)(i - 64u);
        if (t) s->texMaskHi |= (1ULL << bit); else s->texMaskHi &= ~(1ULL << bit);
    }
}
static inline void rdna4_stage_set_smp(RDNA4StageState *s, NSUInteger i, RDNA4SamplerBinding *b) {
    s->smp[i] = b;
    if (b) s->smpMask |= (uint16_t)(1u << i);
    else s->smpMask &= (uint16_t)~(1u << i);
}
static inline void rdna4_stage_set_att(RDNA4StageState *s, uint32_t i, RDNA4Texture *t) {
    s->att[i] = t;
    if (t) s->attMask |= (uint8_t)(1u << i);
    else s->attMask &= (uint8_t)~(1u << i);
}
static inline void rdna4_stage_reset(RDNA4StageState *s) {
    uint32_t bm = s->bufMask;
    while (bm) {
        unsigned i = (unsigned)__builtin_ctz(bm);
        bm &= bm - 1u;
        s->buf[i] = (RDNA4BufSlot){ 0 };
    }
    s->bufMask = 0u;
    s->roBufMask = 0u;
    uint64_t tlo = s->texMaskLo;
    while (tlo) {
        unsigned i = (unsigned)__builtin_ctzll(tlo);
        tlo &= tlo - 1ULL;
        s->tex[i] = nil;
    }
    s->texMaskLo = 0ULL;
    uint64_t thi = s->texMaskHi;
    while (thi) {
        unsigned i = (unsigned)__builtin_ctzll(thi);
        thi &= thi - 1ULL;
        s->tex[64u + i] = nil;
    }
    s->texMaskHi = 0ULL;
    uint16_t sm = s->smpMask;
    while (sm) {
        unsigned i = (unsigned)__builtin_ctz((unsigned)sm);
        sm &= (uint16_t)(sm - 1u);
        s->smp[i] = nil;
    }
    s->smpMask = 0u;
    uint8_t am = s->attMask;
    while (am) {
        unsigned i = (unsigned)__builtin_ctz((unsigned)am);
        am &= (uint8_t)(am - 1u);
        s->att[i] = nil;
    }
    s->attMask = 0u;
    uint32_t asm_ = s->asMask;
    while (asm_) {
        unsigned i = (unsigned)__builtin_ctz(asm_);
        asm_ &= asm_ - 1u;
        s->rt.as[i] = nil;
    }
    s->asMask = 0u;
    s->rtAsRefl = NULL;
    s->scanPlan = NULL;
}

typedef struct {
    RDNA4StageState sv, sf;
    rdna4dc_enc dc;
    VkClearValue cvs[9];
    VkAttachmentDescription ads[9];
    VkImageView views[9];
    VkImageLayout curLay[9];
    uint32_t slvl[8], sslice[8], rlvl[8], rslice[8];
    NSUInteger sact[8];
} RDNA4RenderScratch;

typedef struct {
    VkRenderPass rp;
    VkImageView view;
    uint32_t w, h;
    VkFramebuffer fb;
} RDNA4TexFBCacheEnt;

typedef struct {
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl[2];
    const RDNA4PB *pbV, *pbF;
    uint32_t npbV, npbF;
    const uint32_t *need;
    uint32_t nNeed;
    const RDNA4ArgumentTables *argTables;
    VkFormat dsVk;
    uint32_t sampleCount, maxAmp, drawBits, fetchMax;
    uint8_t hasFrag, hasDS, hasDepth, fetch, isFallback, isTess, isMesh, hasAddrReg, useGpl;
    uint8_t pushStage[2];
    uint8_t hasEmbeddedTex[2];
} RDNA4RenderPSOFast;

@interface RDNA4Texture : _MTLResource {
    id _dev; NSUInteger _w, _h; MTLPixelFormat _pf; const RDNA4Fmt *_fmt; NSUInteger _usage; NSUInteger _opts;
    VkImage _img; VkDeviceMemory _mem; VkImageView _view; VkImageLayout _layout; NSString *_lbl; NSUInteger _size;
    // 11h.6 IOSurface backing: the surface's pages are imported (VK_EXT_external_memory_host) as _imem. Path (a) _iosLinear: the VkImage is LINEAR and bound
    // to _imem itself. Path (b): the VkImage is optimal/device-local and _ibuf (a VkBuffer over _imem) is copied to/from it around each command buffer.
    IOSurfaceRef _ios; NSUInteger _iosPlane; BOOL _iosLinear, _hostReadOnly; VkDeviceMemory _imem; VkBuffer _ibuf; uint8_t *_ibase; size_t _ialloc, _ibpr; NSUInteger _ioff;   // P2: _ioff = the plane's byte offset inside the imported allocation (0 for plane 0 / single-plane)
    // Views retain their immediate parent and address its shared image through absolute mip and slice offsets.
    RDNA4Texture *_root; uint32_t _baseLevel, _baseSlice, _parentSlice; MTLTextureSwizzleChannels _swz;
    unsigned _imageFlags;
    unsigned _aspects, _samples;   // bundle 10: _aspects = every VkImageAspect of a depth/stencil format (0 = colour: colour, view, IOSurface- and buffer-backed textures); _samples = the sample count (0 = 1)
    unsigned _tdk;   // bundle 9: RDNA4TD_K_* (0 = 2D, so IOSurface-, buffer-backed and view textures stay 2D); _layers is also set for cube (6) and 2DArray (arrayLength)
    BOOL _is1D, _arr1D; NSUInteger _layers;   // m11h3: MTLTextureType1D / 1DArray (VkImageType 1D, arrayLayers = _layers); 2D textures keep _is1D NO, _layers 0
    // m11h9: mip levels (2D and 3D), 3D textures (VkImageType 3D, _depth slices) and A8 (the view carries a swizzle, so an identity view per level is made on demand for attachments / storage)
    NSUInteger _levels, _depth; BOOL _is3D, _viewSwz; NSMutableDictionary<NSNumber *, NSNumber *> *_lvViews;
    RDNA4Buffer *_hostBuf; NSUInteger _hoff;   // no-copy: the texture is backed by this buffer's memory at _hoff (copy path b, as an IOSurface texture)
    VkBufferView _texelView; BOOL _texel, _texelOwnedBuffer, _texelStorage;
    RDNA4_RES_IVARS
    uint64_t _acct;   // bytes added to rdna4_alloc_total for this texture's image memory
    RDNA4Mem _pm; BOOL _pooled;   // P1: _mem's role is played by _pm (slab range / recycled / own allocation through rdna4_mem_alloc); _mem is then NULL
    BOOL _impShared, _iosNoBase;   // P4: _imem is a shared import-cache entry (give it back with rdna4_imp_put, never vkFreeMemory); _iosNoBase = a protected IOSurface with no CPU mapping: a GPU-only texture (no _imem/_ibuf/_ibase, rdna4IsIOS is NO)
    BOOL _disp;   // S5.2a: CoreDisplay display surface (D-copy source)
    BOOL _iosLinearSubmitted;
    BOOL _iosLinearCached;   // Phase A4: _img is borrowed from RDNA4IC.li (release via rdna4_linimg_put, never vkDestroyImage)
    BOOL _vramSurfCached;    // Phase B: _img/_ibuf are borrowed from RDNA4VS (320 GB/s OPTIMAL hi-pool VRAM)
    uint64_t _vramSurfId;
    _Atomic uint64_t _pathBSeedState; // Wave 4: packed (1<<32)|seed for WindowServer Path (b) upload elision
    BOOL _led;   // build 16 (P5 Step 0): entered in the live-import ledger (removed in -dealloc)
    os_unfair_lock _fbLock;
    RDNA4TexFBCacheEnt _fbCache[4];
    uint8_t _fbCacheN;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset error:(NSError **)err;
- (RDNA4Texture *)rdna4ImageRoot;
- (RDNA4Heap *)rdna4ResourceHeap;
- (NSUInteger)rdna4ResourceHeapOffset;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane error:(NSError **)err;
- (instancetype)initViewOf:(RDNA4Texture *)root pixelFormat:(MTLPixelFormat)pf type:(MTLTextureType)type
                     levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(RDNA4Buffer *)b offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr error:(NSError **)err;
- (instancetype)initTextureBufferWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(RDNA4Buffer *)b offset:(NSUInteger)off error:(NSError **)err;
- (BOOL)rdna4IsTexel;
- (VkBufferView)rdna4TexelView;
- (RDNA4Buffer *)rdna4TexelBuffer;
- (BOOL)rdna4TexelWritable;
- (NSUInteger)bufferOffset;
- (NSUInteger)rdna4IOSOffset;
- (IOSurfaceRef)rdna4UCSurface;   // build 16 (F3): the retained surface (also for a GPU-only texture), NULL for a buffer-backed one
- (void)rdna4AliasKey:(uint32_t *)sid off:(uint64_t *)off;   // build 16 (P1): the range this texture covers: (IOSurface id, plane offset), or (0, host address) for a buffer-backed texture
- (BOOL)rdna4IsIOS;
- (BOOL)rdna4IsDisp;
- (VkBuffer)rdna4DispBuf;
- (BOOL)rdna4DispImgOnly;   // P6: a display surface with no CPU mapping (protected): its copy source is the VkImage, not a buffer
- (unsigned)rdna4DispSid;   // P6: IOSurfaceGetID of the retained surface for any display texture (rdna4IOSRef is nil for a base-less one)
- (const uint8_t *)rdna4IBase;
- (IOSurfaceRef)rdna4IOSRef;
- (BOOL)rdna4IOSLinear;
- (BOOL)rdna4IOSLinearNeedsInit;
- (void)rdna4IOSLinearDidSubmit;
- (BOOL)rdna4VramSurfCached;
- (uint64_t)rdna4VramSurfId;
- (uint64_t)rdna4PathBSeedState;
- (void)rdna4PathBSetSeed:(uint32_t)seed;
- (void)rdna4PathBInvalidate;
- (BOOL)rdna4HostReadOnly;
- (VkBuffer)rdna4IOSBuffer;
- (size_t)rdna4IOSBytesPerRow;
- (VkImage)vkImage;
- (VkImageView)vkView;
- (uint32_t)rdna4Layers;
- (uint32_t)rdna4ImageLayers;
- (uint32_t)rdna4Levels;
- (uint32_t)rdna4MipLevel:(NSUInteger)level;
- (uint32_t)rdna4Slice:(NSUInteger)slice;
- (unsigned)rdna4Aspects;   // bundle 10: VkImageAspectFlags of every barrier / attachment view of this texture (colour for the colour world)
- (BOOL)rdna4IsDS;
- (BOOL)rdna4IsUInt;   // bundle 19: an unsigned-integer colour format (RG16Uint)
- (BOOL)rdna4IsSInt;
- (unsigned)rdna4UIntBits;
- (void)rdna4IntBitsRGB:(unsigned *)rgb alpha:(unsigned *)alpha;
- (BOOL)rdna4IsView;
- (BOOL)isCompressed;
- (NSUInteger)rdna4Samples;
- (unsigned)rdna4CopyAspect;   // the aspect of a buffer<->image copy (0 = refuse: a combined depth/stencil format)
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv;
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly;   // bundle 9: cube / 2D array render targets and attachments are per-layer 2D views
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly layerCount:(uint32_t)lc;
- (VkImageView)rdna4StorageView;
- (VkFormat)vkFormat;
- (uint32_t)bytesPerPixel;
- (VkImageLayout)layout;
- (void)setLayout:(VkImageLayout)l;
- (NSUInteger)width;
- (NSUInteger)height;
- (NSUInteger)depth;
- (NSUInteger)mipmapLevelCount;
- (MTLTextureType)textureType;
- (MTLStorageMode)storageMode;
- (uint64_t)rdna4BackingBlock;
- (uint64_t)rdna4BackingOffset;
- (uint64_t)rdna4BackingSize;
- (void)rdna4AbortHeapConstruction;
- (void)rdna4DestroyRetiredBacking;
- (VkFramebuffer)rdna4CachedFramebufferForRP:(VkRenderPass)rp view:(VkImageView)view width:(uint32_t)w height:(uint32_t)h;
@end

@interface RDNA4SamplerStorage : NSObject {
@public
    RDNA4SampCfg config;
    VkSampler normal, comparison, normalizedNormal;
    NSUInteger references, argumentReferences;
    uint32_t handles;
    BOOL argumentReserved;
}
@end

@interface RDNA4SamplerBinding : NSObject {
@public
    RDNA4SamplerStorage *_storage;
    float _lodMinClamp, _lodMaxClamp;
    BOOL _lodClampOverride;
}
+ (instancetype)bindingWithBaseStorage:(RDNA4SamplerStorage *)base
                         lodMinClamp:(float)lodMin
                         lodMaxClamp:(float)lodMax
                               error:(NSError **)err;
- (RDNA4SamplerStorage *)rdna4Storage;
- (BOOL)rdna4HasLODClampOverride;
- (float)rdna4LODMinClamp;
- (float)rdna4LODMaxClamp;
- (VkSampler)vkSampler;
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err;
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison
               normalizedCoordinates:(BOOL)normalized
                               error:(NSError **)err;
- (BOOL)rdna4PixelCoordinates;
@end

@interface RDNA4SamplerState : NSObject {
    id _dev; NSString *_lbl; RDNA4SamplerStorage *_storage; RDNA4SamplerBinding *_baseBinding;
    BOOL _argumentBuffers;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLSamplerDescriptor *)d error:(NSError **)err;
- (RDNA4SamplerStorage *)rdna4ArgumentIdentity;
- (RDNA4SamplerBinding *)rdna4Binding:(NSError **)err;
- (RDNA4SamplerBinding *)rdna4BindingWithLODMinClamp:(float)lodMin
                                          lodMaxClamp:(float)lodMax
                                                error:(NSError **)err;
- (VkSampler)vkSampler;
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err;
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison normalizedCoordinates:(BOOL)normalized error:(NSError **)err;
- (BOOL)rdna4PixelCoordinates;
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0));
@end

@interface RDNA4DepthStencilState : NSObject { id _dev; NSString *_lbl; MTLDepthStencilDescriptor *_d; RDNA4DSInfo _info; }
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d;
- (MTLDepthStencilDescriptor *)rdna4Descriptor;
- (RDNA4DSInfo)rdna4Info;
@end

@interface RDNA4FenceCheckpoint : NSObject {
@public
    uint64_t generation;
    uint64_t producerCBId;
    _Atomic int state;
    _Atomic int fault;
}
- (void)rdna4Ready;
- (void)rdna4Fail:(VkResult)fault;
@end

@interface RDNA4Fence : NSObject {
    id _dev;
    NSString *_lbl;
    pthread_mutex_t _mu;
    uint64_t _latestGeneration;
    RDNA4FenceCheckpoint *_latestCheckpoint;
}
- (instancetype)initWithDevice:(id)dev;
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpoint;
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpointForCB:(uint64_t)cbid;
- (RDNA4FenceCheckpoint *)rdna4WaitCheckpoint;
@end

@interface RDNA4ResidencySnapshot : NSObject <RDNA4ResidencySnapshotInternal> {
    NSArray *_allocations;
    uint64_t _allocatedSize;
    uint64_t _generation;
}
- (instancetype)initWithAllocations:(NSArray *)allocations
                      allocatedSize:(uint64_t)allocatedSize
                         generation:(uint64_t)generation;
- (uint64_t)rdna4AllocatedSize;
- (uint64_t)rdna4Generation;
@end

@interface RDNA4ResidencySet : NSObject <RDNA4ResidencySetInternal> {
    id _dev;
    NSString *_lbl;
    pthread_mutex_t _mu;
    rdna4rs_set_t _state;
    NSMutableDictionary<NSNumber *, id> *_stagedObjects;
    NSMutableDictionary<NSNumber *, id> *_committedObjects;
    RDNA4ResidencySnapshot *_snapshot;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (BOOL)rdna4IsResidentRequested;
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

@interface RDNA4CommandQueue : _MTLCommandQueue {
    VkCommandPool _pool; dispatch_queue_t _cq; id _ndev;
    NSUInteger _gprio, _bprio; id _compQ;   // gap census: SkyLight's priority / completion-queue setters (recorded only)
    NSMutableArray<RDNA4CommandBuffer *> *_pending;
    NSMutableArray *_residencySets;
    pthread_mutex_t _qmu;
    garnet_vkpool_t _cmdPool;
    VkCommandPool _computePool;
    garnet_vkpool_t _computeCmdPool;
    VkSemaphore _orderSem;
    garnet_async_qorder_t _order;
    _Atomic int _orderLastWasCompute;
    garnet_qp_t _qp;
    NSUInteger _launchedCount;
    BOOL _submitting, _scheduleKickNeeded, _deferTimerArmed;
    _Atomic uint32_t _residencySetCount;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (VkCommandPool)pool;
- (VkCommandPool)computePool;
- (VkCommandBuffer)rdna4TakeCmdBuf:(NSError **)err;
- (VkCommandBuffer)rdna4TakeCmdBufLocked:(NSError **)err;
- (void)rdna4GiveCmdBuf:(VkCommandBuffer)cmd;
- (VkCommandBuffer)rdna4TakeComputeCmdBuf:(NSError **)err;
- (void)rdna4GiveComputeCmdBuf:(VkCommandBuffer)cmd;
- (VkSemaphore)rdna4OrderSemaphore;
- (BOOL)rdna4OrderLastWasCompute;
- (garnet_async_submit_order_t)rdna4NextOrderForCompute:(BOOL)isCompute;
- (BOOL)rdna4HasResidencySets;
- (dispatch_queue_t)rdna4CompletionQueue;
- (void)rdna4Kick;
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb;
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb failed:(BOOL)failed;
- (void)rdna4EnqueueOrderedCompletion:(RDNA4CommandBuffer *)cb failed:(BOOL)failed block:(void (^)(void))block;
- (void)rdna4NoteElidedDCopy;
- (BOOL)rdna4PendingHasNewerSid:(uint32_t)sid after:(RDNA4CommandBuffer *)cb;
- (BOOL)rdna4AdvanceHead;
- (void)addResidencySet:(id)residencySet;
- (void)addResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (void)removeResidencySet:(id)residencySet;
- (void)removeResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (NSArray *)rdna4ResidencySetSnapshots;
@end

@interface RDNA4CommandBuffer : _MTLCommandBuffer {
    VkCommandBuffer _vk; VkFence _fence; BOOL _ended; RDNA4CommandQueue *_nq;
    NSMutableArray *_refs; RDNA4Buffer *_ring; NSUInteger _ringUsed; VkDescriptorPool _pool; NSError *_encErr, *_prepareErr;
    VkDescriptorSet _argumentSets[3]; uint32_t _argumentTokenCounts[5];
    RDNA4Buffer *_argumentTable; NSUInteger _argumentTableOffset, _argumentTableBytes;
    __weak NSSet<RDNA4Buffer *> *_bufferAddressResources;
    NSMutableArray<RDNA4CommandSegment *> *_segments; RDNA4CommandSegment *_curSegment;
    uint64_t _segmentSerial;
    NSUInteger _nextSegment; uint64_t _startTime, _readyTime; BOOL _prepared, _hasEvents, _inFlight, _submitAttempted, _cbSubmitLogged, _dispSeqLogged;
    BOOL _scheduleAfterPublish;
    pthread_mutex_t _smu;   // serializes scheduler advancement against completion-queue state changes
    _Atomic int _scheduledDelivered, _terminalClaimed;
    NSMutableArray *_iot, *_iow, *_eventDirty;   // 11h.6: whole-CB roots touched / display candidates written; IOSurface writes not yet flushed at a segment boundary
    uint64_t _dseq;   // S5.2a/#12: frame sequence, taken at submission (under the submit lock) = GPU order
    uint32_t _dsid;   // #12: IOSurface id of the display surface this cb writes (0 = unknown)
    int _dcls; uint64_t _dcid, _dbid;   // #12 D2: write class of the presented frame; chain id of this frame and of its base (0 = full copy)
    int _dslot;   // S5.2a: the scanout slot this command buffer's appended copy targets (-1 = none)
    int _dinst;   // bundle 13: the instance that slot belongs to (0 = the DP's, 2 = the monitor B's)
    BOOL _dcrc, _dlate; RDNA4Texture *_dtex;   // CRC source or a slotless display write awaiting a deferred copy
    BOOL _shaderFallback, _displayIncomplete;
    rdna4_display_commit _displayCommits[4];
    struct rdna4dw { __unsafe_unretained RDNA4Texture *t; uint32_t mask; const void *lastpso; char fns[128]; rdna4df_wr_t wr; } _dw[4]; int _ndw;   // native #12 P1: per display surface written, how it was written
    uint64_t _tBeg; BOOL _frame;   // native #12 T1: creation time; wrote a display surface (a "frame")
    uint64_t _cbid, _tcommit; uint32_t _cbw[RDNA4CB_MAXW], _cbr[RDNA4CB_MAXR], _ncbw, _ncbr; int _cbcls; uint32_t _cbdsid;   // Stage 0b: log id, commit time, IOSurface ids written / read (sampled or loaded), display write class+1 and sid
    NSUInteger _prot;   // gap census: protection options
    uint64_t _pser;   // P1: fence serial taken when this command buffer was created (0 = pool OFF); closed when its fence completed / it failed to submit / it was dropped unsubmitted
    struct rdna4ojob { rdna4occ *o; VkQueryPool qp; uint32_t base; } *_oj; uint32_t _noj, _coj; NSMutableArray *_ojb; NSMutableArray *_qpools; uint32_t _qpUsed; VkQueryPool _qpCur;   // build 18 (P2): per-pass occlusion jobs (+ their visibility buffers), the query pools of this command buffer, the chunk cursor of the newest one
    rdna4uc_list _uc; BOOL _ucLive; rdna4ia _ia;   // build 16: F3 the surfaces this command buffer use-counts while in flight (rdna4UCRelease gives them back, exactly once); P1 the per-range aliasing state
    NSMutableArray<NSNumber *> *_vsActive;   // Phase B: active RDNA4VS entry IDs held by this CB until completion/dealloc
    RDNA4NativeJournal _native;
    rdna4dc_cbcache _dcCache;
    NSMutableArray *_residencySnapshots;
    NSMutableSet *_residencyHeapResources;
    RDNA4RenderScratch *_renderScratch;
    RDNA4ResourceBindings *_reusableBindings;
    BOOL _renderScratchInUse, _reusableBindingsInUse;
@public
    BOOL _launchedAsBarrier, _fifoDoneFailed;
    void (^_fifoDoneBlock)(void);
    BOOL _syncCommit;
}
- (VkCommandBuffer)rdna4BlitVK;
- (void)rdna4NativeRecord:(RDNA4NativeOp)op;
- (BOOL)rdna4NativeEligible;
- (int)rdna4NativeExecute:(bool *)fallback;
- (void)rdna4PoolDone;
- (void)rdna4ArgumentWrite:(id)buffer range:(NSRange)range candidates:(NSSet *)candidates fillOnly:(BOOL)fillOnly;
- (void)rdna4ArgumentWriteRanges:(id)buffer ranges:(NSArray<NSValue *> *)ranges candidates:(NSSet *)candidates;
- (void)rdna4ArgumentIntent:(id)intent;
- (void)rdna4CancelUnsubmittedArgumentIntents;
- (rdna4occ *)rdna4OccNew:(RDNA4Buffer *)vb accumulate:(int)acc pool:(VkQueryPool *)qp base:(uint32_t *)base;   // build 18 (P2)
- (void)rdna4OccResolve;   // build 18 (P2): called from rdna4PoolDone
- (void)rdna4PrepareForSubmission;
- (BOOL)rdna4PipelineEligible;
- (BOOL)rdna4HasEvents;
- (BOOL)rdna4MayDeferFinal;
- (uint64_t)rdna4FinalDeferNs;
- (uint32_t)rdna4DisplayTargetSid;
- (BOOL)rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue;
- (BOOL)rdna4OpenSegment:(NSError **)err;
- (BOOL)rdna4EndCurrentSegment:(NSError **)err;
- (BOOL)rdna4CurrentSegmentIsCompute;
- (BOOL)rdna4TrySwitchToComputeQueue;
- (void)rdna4EnsureGfxSegment;
- (BOOL)rdna4HasArgumentWrites;
- (void)rdna4ArgumentCheckpoint;
- (void)rdna4UpdateFence:(id)f;
- (void)rdna4WaitForFence:(id)f;
- (int)rdna4WaitsReady:(RDNA4CommandSegment *)segment error:(NSError **)err;
- (void)rdna4SignalSegment:(RDNA4CommandSegment *)segment;
- (void)rdna4ReleaseSegmentDescriptorPools:(RDNA4CommandSegment *)segment;
- (void)rdna4FlushEventWrites:(VkCommandBuffer)vk;
- (void)rdna4FailPendingSignals;
- (void)rdna4FailPendingFenceUpdates:(VkResult)fault;
- (void)rdna4EnsureScheduledWithError:(NSError *)err;
- (BOOL)rdna4ClaimTerminal;
- (void)rdna4DeliverTerminalSuccessWithQueue:(RDNA4CommandQueue *)queue;
- (void)rdna4DeliverTerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault;
- (void)rdna4TerminalSuccessWithQueue:(RDNA4CommandQueue *)queue;
- (void)rdna4TerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault;
- (void)rdna4FenceWait:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue;
- (void)rdna4FenceDone:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue r:(VkResult)r t1:(uint64_t)t1 slices:(int)slices delivered:(BOOL)delivered werr:(NSError *)werr;
- (uint64_t)rdna4TBegin;
- (BOOL)rdna4IsFrame;
- (BOOL)rdna4IOFirstTouch:(RDNA4Texture *)t;
- (rdna4ia_act)rdna4IOAlias:(RDNA4Texture *)t need:(BOOL)need write:(BOOL)w;   // build 16 (P1): the aliasing decision for this touch
- (RDNA4Texture *)rdna4IOTex:(uintptr_t)key;
- (void)rdna4IOUnwrite:(RDNA4Texture *)t;
- (void)rdna4UCRelease;
- (void)rdna4IOWritten:(RDNA4Texture *)t;
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use;
- (void)rdna4NoteHostWriteback:(RDNA4Texture *)texture;
- (void)rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level;
- (void)rdna4CBRead:(RDNA4Texture *)t;   // Stage 0b: t's IOSurface is sampled / loaded by this command buffer
- (void)rdna4CBSubmit:(uint64_t)tq;    // Stage 0b: call with the submit lock held, right before vkQueueSubmit (after rdna4DispSeq)
- (void)rdna4CBDropWithError:(NSError *)error fault:(VkResult)fault;
- (void)rdna4CBFence:(int)vkr;
- (void)rdna4DispDraw:(RDNA4Texture *)t bits:(uint32_t)bits rect:(rdna4df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(RDNA4Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc;   // native #12 D1: the extent of one draw
- (void)rdna4DispNote:(RDNA4Texture *)t bits:(uint32_t)bits pso:(RDNA4RenderPipelineState *)p;   // native #12 P1: a write of kind `bits` to t (ignored unless t is a display surface)
- (void)rdna4NoteShaderFallback;
- (VkCommandBuffer)vk;
- (BOOL)rdna4Finish:(NSError **)err;      // vkEndCommandBuffer
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error;
- (void)rdna4ClearHostActions;
- (void)rdna4DispSeq;
- (void)rdna4DisplayCommit;
- (void)rdna4DisplayUncommit;
- (void)rdna4DispDone:(VkResult)r;     // S5.2a: the appended copy ended (r) or never ran; presents on VK_SUCCESS only; idempotent
- (VkFence)fence;
- (void)setFence:(VkFence)f;
- (void)rdna4Retain:(id)o;
- (void)rdna4RetainArgument:(id)object;
- (void)rdna4RetainEncoded:(id)object;
- (uint64_t)rdna4SegmentSerial;
- (uint64_t)rdna4CBId;
- (BOOL)rdna4HasResidency;
- (void)rdna4UseResource:(id)resource usage:(NSUInteger)usage;
- (void)rdna4UseHeap:(id)heap;
- (void)useResidencySet:(id)residencySet;
- (void)useResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (void)rdna4AttachResidencySnapshot:(id)snapshot;
- (void)rdna4AttachQueueResidencySnapshots;
- (NSSet *)rdna4ResidencyDeclaredResources;
- (void)rdna4Fail:(NSString *)why;
- (NSError *)rdna4Error;
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo;
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l;
- (rdna4dc_cbcache *)rdna4DrawCache;
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm;
- (BOOL)rdna4FillOrPushSet:(VkDescriptorSet)set pushCmd:(VkCommandBuffer)pushCmd pushLayout:(VkPipelineLayout)pushLayout pushSet:(uint32_t)pushSet bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm fromRender:(BOOL)fromRender;
- (RDNA4RenderScratch *)rdna4AcquireRenderScratch:(BOOL *)ownedOut;
- (void)rdna4ReleaseRenderScratch:(RDNA4RenderScratch *)sc owned:(BOOL)owned;
- (RDNA4ResourceBindings *)rdna4AcquireResourceBindings:(BOOL *)borrowedOut;
- (void)rdna4ReleaseResourceBindings:(RDNA4ResourceBindings *)rb borrowed:(BOOL)borrowed;
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

@interface RDNA4ArgumentPool : NSObject {
@public
    VkDescriptorPool handle;
@private
    uint64_t _descriptorCount;
}
- (instancetype)initWithInfo:(const VkDescriptorPoolCreateInfo *)info error:(NSError **)error;
@end

@interface RDNA4ICBArgOwners : NSObject
@property NSMutableSet *icbs;
@property NSMutableSet *pipelines;
@property NSMutableSet *nestedBuffers;
@end

@interface RDNA4ReflectionCompiler : NSObject
@end

@interface RDNA4ArgumentField : NSObject
@property NSUInteger offset, size;
@property MTLDataType type;
@property RDNA4ArgumentLayout *nested;
@end

@interface RDNA4ArgumentLayout : NSObject {
    NSDictionary<NSNumber *, RDNA4ArgumentField *> *_fields;
    NSUInteger _length, _alignment;
    id _device;
}
- (instancetype)initWithType:(MTLStructType *)type device:(id)device depth:(unsigned)depth;
- (instancetype)initWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments device:(id)device;
- (RDNA4ArgumentField *)field:(NSUInteger)index;
- (NSDictionary<NSNumber *, RDNA4ArgumentField *> *)fields;
- (NSUInteger)encodedLength;
- (NSUInteger)alignment;
@end

@interface RDNA4ArgumentEncoder : NSObject {
    RDNA4ArgumentLayout *_layout;
    id _device;
    RDNA4Buffer *_buffer;
    NSUInteger _offset;
    NSString *_label;
}
- (instancetype)initWithLayout:(RDNA4ArgumentLayout *)layout device:(id)device;
- (RDNA4ArgumentLayout *)rdna4Layout;
- (void)rdna4ValidateRange:(NSRange)range type:(MTLDataType)type;
@end

@interface RDNA4ArgumentSamplerAlias : NSObject
@property (nonatomic, weak) id object;
@property (nonatomic, weak) id device;
@property BOOL bindlessQueried;
@end

@interface RDNA4ArgumentResourceToken : NSObject
@property (nonatomic, weak) id object;     // texture; sampler wrappers live in aliases
@property (nonatomic, weak) id identity;   // object or the sampler storage that owns the token
@property (nonatomic, weak) id device;
@property BOOL texture;
@property NSMutableArray<RDNA4ArgumentSamplerAlias *> *aliases;
@end

@interface RDNA4ArgumentWriteIntent : NSObject {
@public
    __weak id buffer;
    uint64_t serial;
    uint64_t submissionEpoch;
    uint64_t hostGeneration;
    NSRange range;
    NSArray<NSValue *> *writeRanges;
    NSMutableArray<NSValue *> *hostOverlaps;
    NSSet *candidates;
    BOOL fillOnly, active, submitted;
}
@end

@interface RDNA4ArgumentCompletedWrite : NSObject {
@public
    uint64_t serial;
    uint64_t submissionEpoch;
    uint64_t hostGeneration;
    NSRange range;
    NSSet *candidates;
    BOOL fillOnly, exclusive;
}
@end

@interface RDNA4ArgumentSamplerOwners : NSObject
@property NSMutableDictionary<NSNumber *, NSArray *> *slots;
@property NSMutableDictionary<NSValue *, NSSet *> *possible;
@property NSMutableArray<RDNA4ArgumentWriteIntent *> *writers;
@property NSMutableArray<RDNA4ArgumentCompletedWrite *> *completed;
@property uint64_t hostGeneration;
@property BOOL hostGenerationSaturated;
@property uint64_t latestSummaryEpoch;
@property uint64_t lastCompletedEpoch;
@end

@interface RDNA4IOFileHandle : NSObject {
    id _dev;
    int _fd;
    uint64_t _fileSize;
    NSString *_lbl;
}
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url
             compressionMethod:(NSInteger)method compressed:(BOOL)compressed
                         error:(NSError **)err;
- (id)rdna4Device;
- (uint64_t)rdna4FileSize;
- (BOOL)rdna4ReadInto:(void *)dst size:(size_t)size offset:(uint64_t)offset error:(NSError **)err;
@end

@interface RDNA4IOCommand : NSObject {
@public
    RDNA4IOCommandKind kind;
    RDNA4IOFileHandle *handle;
    RDNA4Buffer *dstBuffer;
    RDNA4Texture *dstTexture;
    id event;
    void *dstPtr;
    uint64_t dstOffset;
    uint64_t size;
    uint64_t srcOffset;
    uint64_t eventValue;
    NSUInteger slice, level;
    MTLOrigin origin;
    MTLSize texSize;
    rdna4io_tex_plan_t texPlan;
}
@end

@interface RDNA4IOCommandQueue : NSObject {
    id _dev;
    NSString *_lbl;
    id _scratchAllocator;
    dispatch_queue_t _workQ;
    dispatch_queue_t _compQ;
    pthread_mutex_t _qmu;
    rdna4io_qsched_t _sched;
    NSMapTable<NSNumber *, RDNA4IOCommandBuffer *> *_enqueuedWeak;
    NSMutableDictionary<NSNumber *, RDNA4IOCommandBuffer *> *_committedStrong;
    NSMutableArray<RDNA4Buffer *> *_stagingPool;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (id)device;
- (dispatch_queue_t)rdna4IOCompletionQueue;
- (uint64_t)rdna4IOReserveSlotForBuffer:(RDNA4IOCommandBuffer *)cb;
- (BOOL)rdna4IOCommitBuffer:(RDNA4IOCommandBuffer *)cb slot:(uint64_t)slot;
- (BOOL)rdna4IOCancelSlotIfPending:(uint64_t)slot;
- (void)rdna4IOAbandonUncommittedSlot:(uint64_t)slot;
- (void)rdna4IOSlotDone:(uint64_t)slot;
- (RDNA4Buffer *)rdna4IOAcquireStaging:(NSUInteger)bytes customScratch:(id __strong *)customOut error:(NSError **)err;
- (void)rdna4IOReleaseStaging:(RDNA4Buffer *)buf customScratch:(id)custom;
@end

@interface RDNA4IOCommandBuffer : NSObject {
    RDNA4IOCommandQueue *_q;
    id _dev;
    BOOL _retained;
    NSString *_lbl;
    pthread_mutex_t _mu;
    pthread_cond_t _cond;
    rdna4io_cb_state_t _state;
    BOOL _finishing;
    uint64_t _slotId;
    NSError *_err;
    NSMutableArray<RDNA4IOCommand *> *_cmds;
    NSMutableArray<void (^)(id)> *_handlers;
    dispatch_semaphore_t _waitEventSem;
}
- (instancetype)initWithQueue:(RDNA4IOCommandQueue *)queue retainedReferences:(BOOL)retained;
- (void)rdna4IOExecuteOnWorkerWithSlot:(uint64_t)slot;
@end

@interface RDNA4SparseBackingPool : NSObject {
    pthread_mutex_t _mu;
    BOOL _muInit;
    rdna4sp_tile_pool _pool;
    uint64_t _capacityBytes;
    BOOL _ownsAccounting;
}
- (instancetype)initWithCapacityBytes:(uint64_t)capacityBytes ownsAccounting:(BOOL)ownsAcct error:(NSError **)err;
- (BOOL)ensureFreeTiles:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err;
- (rdna4sp_tile_pool *)pool;
- (pthread_mutex_t *)mutex;
- (NSUInteger)capacityBytes;
- (NSUInteger)usedBytes;
- (NSUInteger)availableBytes;
@end

@interface RDNA4SparseHeap : _MTLHeap {
    id _dev;
    NSString *_lbl;
    NSUInteger _size;
    NSUInteger _purge;
    unsigned _cc, _hz;
    RDNA4SparseBackingPool *_pool;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d error:(NSError **)err;
- (RDNA4SparseBackingPool *)rdna4SparseBackingPool;
@end

@interface RDNA4SparseTexture : RDNA4Texture {
    id _spHeap;
    RDNA4SparseBackingPool *_spPool;
    rdna4sp_texture_state _spState;
    uint32_t _spMemTypeBits;
    pthread_mutex_t _spMu;
    BOOL _spMuInit;
}
- (instancetype)initSparseWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(id)heap error:(NSError **)err;
- (const rdna4sp_texture_state *)rdna4SparseState;
- (BOOL)rdna4ValidateRegions:(const rdna4sp_region *)regions mipLevels:(const uint64_t *)mips
                      slices:(const uint64_t *)slices count:(size_t)count
                        mode:(MTLSparseTextureMappingMode)mode error:(NSError **)err;
- (BOOL)rdna4ExecuteSparseMode:(MTLSparseTextureMappingMode)mode
                       regions:(const rdna4sp_region *)regions
                     mipLevels:(const uint64_t *)mips
                        slices:(const uint64_t *)slices
                         count:(size_t)count
                         error:(NSError **)err;
@end

@interface RDNA4SparsePendingOp : NSObject {
@public
    RDNA4SparseTexture *tex;
    MTLSparseTextureMappingMode mode;
    BOOL indirect;
    RDNA4Buffer *indirectBuf;
    NSUInteger indirectOffset;
    rdna4sp_region *regions;
    uint64_t *mips;
    uint64_t *slices;
    size_t count;
}
@end

@interface RDNA4ResourceStateEncoder : _MTLCommandEncoder {
    RDNA4CommandBuffer *_cb;
    BOOL _ended;
    NSString *_lbl;
    NSMutableArray<RDNA4SparsePendingOp *> *_pendingOps;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)desc;
- (void)updateTextureMappings:(id<MTLTexture>)texture
                         mode:(MTLSparseTextureMappingMode)mode
                      regions:(const MTLRegion *)regions
                    mipLevels:(const NSUInteger *)mipLevels
                       slices:(const NSUInteger *)slices
                   numRegions:(NSUInteger)numRegions;
- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
                      region:(MTLRegion)region
                    mipLevel:(NSUInteger)mipLevel
                       slice:(NSUInteger)slice;
- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
              indirectBuffer:(id<MTLBuffer>)indirectBuffer
        indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)updateFence:(id<MTLFence>)fence;
- (void)waitForFence:(id<MTLFence>)fence;
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

@interface RDNA4MeshRenderPipelineState : RDNA4RenderPipelineState {
    rdna4mesh_exec_mode_t _meshExecMode;
    BOOL _hasObject;
    rdna4mesh_linked_info_t _linkedInfo;
    rdna4mesh_refl_t _objRefl, _meshRefl, _fragRefl;

    // Initial shader modules + runtime LocalSize specialization caches.
    VkShaderModule _objMod, _meshMod;
    char _oep[128], _mep[128];
    uint32_t _initObjLocal[3], _initMeshLocal[3];
    id _objFn, _meshFn;
    NSArray<NSDictionary *> *_objDeps, *_objDynlibs, *_meshDeps, *_meshDynlibs;
    NSMutableDictionary<NSString *, NSValue *> *_objModCache;  // "x,y,z" -> VkShaderModule
    NSMutableDictionary<NSString *, NSValue *> *_meshModCache; // "x,y,z" -> VkShaderModule

    // Object stage descriptors: native TaskEXT uses graphics set _objSet; emulated uses _objCompDsl/_objCompPl.
    VkDescriptorSetLayout _dslObj;
    uint32_t _objSet;
    VkDescriptorSetLayout _objCompDsl;
    VkPipelineLayout _objCompPl;
    NSMutableDictionary<NSString *, NSValue *> *_objCompVariants; // "x,y,z" -> VkPipeline (compute)
    NSMutableDictionary<NSString *, NSValue *> *_meshVariants;    // graphics pipeline variants

    RDNA4PB *_pbO, *_pbM;
    uint32_t _npbO, _npbM;
    uint32_t _emulObjMask, _emulMeshMask;

    uint32_t _maxObjThreads, _maxMeshThreads;
    uint32_t _objExecWidth, _meshExecWidth;
    BOOL _objMultExecW, _meshMultExecW;
    BOOL _alphaToOne;
}
- (instancetype)initWithDevice:(id)dev meshDescriptor:(id)d options:(NSUInteger)options error:(NSError **)err;
- (BOOL)rdna4IsMeshPipeline;
- (rdna4mesh_exec_mode_t)rdna4MeshExecMode;
- (BOOL)rdna4HasObjectStage;
- (const rdna4mesh_linked_info_t *)rdna4MeshLinkedInfo;
- (const rdna4mesh_refl_t *)rdna4ObjRefl;
- (const rdna4mesh_refl_t *)rdna4MeshRefl;
- (uint32_t)rdna4ObjectSet;
- (VkDescriptorSetLayout)rdna4ObjectDsl;
- (VkDescriptorSetLayout)rdna4ObjectComputeDsl;
- (VkPipelineLayout)rdna4ObjectComputeLayout;
- (const RDNA4PB *)rdna4PbO; - (uint32_t)rdna4NpbO;
- (const RDNA4PB *)rdna4PbM; - (uint32_t)rdna4NpbM;
- (uint32_t)rdna4EmulObjMask;
- (uint32_t)rdna4EmulMeshMask;
- (BOOL)rdna4ObjMultExecWidth;
- (BOOL)rdna4MeshMultExecWidth;
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup;
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup;
- (NSUInteger)objectThreadExecutionWidth;
- (NSUInteger)meshThreadExecutionWidth;
- (VkPipeline)rdna4ObjectComputePipelineForLocal:(const uint32_t[3])objLocal error:(NSError **)err;
- (VkPipeline)rdna4MeshPipelineForCull:(VkCullModeFlags)cull front:(VkFrontFace)front
                                    ds:(uint32_t)dk rflags:(uint32_t)rflags
                              objLocal:(const uint32_t[3])objLocal
                             meshLocal:(const uint32_t[3])meshLocal
                                 error:(NSError **)err;
@end

@interface RDNA4MeshEncoderState : NSObject {
    @public
    RDNA4StageState objState;
    RDNA4StageState meshState;
    uint64_t objTgMem[RDNA4MESH_MAX_TG_MEM_INDICES];
    NSMutableSet *samplers;
}
@end

@interface GarnetGLDrawable : NSObject {
    uint32_t _sid;
}
@end

@interface RDNA4CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
- (id)blitCommandEncoder;
- (id)computeCommandEncoder;
- (id)resourceStateCommandEncoder;
- (id)resourceStateCommandEncoderWithDescriptor:(id)d;
- (id)accelerationStructureCommandEncoder;
- (id)accelerationStructureCommandEncoderWithDescriptor:(id)d;
@end

@interface RDNA4CommandBuffer (RDNA4OrderedHostAction)
- (id<MTLDevice>)device;
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error;
#ifdef VK_VERSION_1_0
- (void)rdna4RetainEncoded:(id)object;
- (VkCommandBuffer)vk;
- (BOOL)rdna4HasArgumentWrites;
- (void)rdna4ArgumentCheckpoint;
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l;
- (rdna4dc_cbcache *)rdna4DrawCache;
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm;
- (BOOL)rdna4PrepareArguments:(const RDNA4ArgumentTables *)tables resources:(NSSet *)resources writable:(NSSet *)writable scan:(const RDNA4ArgumentScanPrepared *)scan textures:(void (^)(RDNA4Texture *, VkImageLayout, BOOL))prepare;
- (void)rdna4CopyArgumentSets:(VkDescriptorSet [3])outSets;
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo;
#endif
@end

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

@interface RDNA4BlitEncoder (RDNA4IndirectCommands)
@end

@interface GarnetDevice (RDNA4IndirectCommandBuffers)
- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCommandCount
                                                                options:(MTLResourceOptions)options;
@end

@interface RDNA4Heap (RDNA4SparseBacking)
- (RDNA4SparseBackingPool *)rdna4SparseBackingPoolWithError:(NSError **)err;
@end

@interface RDNA4RenderPipelineState (RDNA4MeshQuery)
- (BOOL)rdna4IsMeshPipeline;
@end


// ---- Auto-declared cross-TU ObjC methods for internal classes ----
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

@interface GarnetDevice (GarnetInternalMethods)
- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor maxCommandCount:(NSUInteger)maxCommandCount options:(MTLResourceOptions)options;
- (id)compiler;
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error;
- (id)targetDeviceArchitecture;
- (BOOL)supportsQueryTextureLOD;
- (BOOL)supportsDynamicLibraries;
- (BOOL)supportsRenderDynamicLibraries;
- (BOOL)supportsBinaryArchives;
- (id<MTLDynamicLibrary>)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error;
- (id<MTLDynamicLibrary>)newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **)error;
- (id)newArchiveWithURL:(NSURL *)url error:(NSError **)error;
- (id)newIndirectArgumentBufferLayoutWithStructType:(MTLStructType *)type;
- (id)newArgumentEncoderWithLayout:(id)layout;
- (id)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments;
- (id)newIndirectArgumentEncoderWithLayout:(id)layout;
- (id)newArgumentEncoderWithBufferBinding:(id<MTLBufferBinding>)binding API_AVAILABLE(macos(13.0));
- (NSDictionary *)rdna4ScanoutStats;
- (NSDictionary *)rdna4ScanoutRelease;
- (void)lazyInitialize;
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options;
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d;
- (id)rdna4NewComputePipeline:(id)fn descriptor:(MTLComputePipelineDescriptor *)desc options:(NSUInteger)opts error:(NSError **)error;
- (id)rdna4NewComputePipeline:(id)fn error:(NSError **)error;
- (id)newComputePipelineStateWithFunction:(id)fn error:(NSError **)error;
- (id)rdna4NewComputePipeline:(id)fn descriptor:(MTLComputePipelineDescriptor *)desc stageInputDescriptor:(MTLStageInputOutputDescriptor *)descriptor options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error;
- (id)rdna4NewComputePipeline:(id)fn stageInputDescriptor:(MTLStageInputOutputDescriptor *)descriptor options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error;
- (id)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error;
- (void)newComputePipelineStateWithFunction:(id)fn completionHandler:(void (^)(id, NSError *))h;
- (void)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h;
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error;
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h;
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error;
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane;
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)len options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))d;
- (id)rdna4NoCopyTexture:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr what:(const char *)what;
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr;
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr;
- (id)newTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *, NSUInteger))dl;
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d;
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)len options:(MTLResourceOptions)o;
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d;
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size;
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithDescriptor:(MTLAccelerationStructureDescriptor *)d;
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d;
- (id)newFence;
- (NSUInteger)currentAllocatedSize;
- (NSUInteger)maxThreadgroupMemoryLength;
- (NSUInteger)minConstantBufferAlignmentBytes;
- (MTLSize)maxThreadsPerThreadgroup;
- (BOOL)supportsSampleCount:(NSUInteger)n;
- (BOOL)supportsTextureSampleCount:(NSUInteger)n;
- (NSUInteger)iosurfaceTextureAlignmentBytes;
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)pf;
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf;
- (BOOL)isDepth24Stencil8PixelFormatSupported;
- (BOOL)supportsUnalignedVertexFetch;
- (MTLReadWriteTextureTier)readWriteTextureSupport;
- (BOOL)areProgrammableSamplePositionsSupported;
- (BOOL)areRasterOrderGroupsSupported;
- (BOOL)supportsBCTextureCompression;
- (BOOL)supports32BitFloatFiltering;
- (BOOL)supports32BitMSAA;
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count;
- (BOOL)supportsRaytracing;
- (BOOL)supportsRaytracingFromRender;
- (BOOL)supportsPrimitiveMotionBlur;
- (BOOL)supportsFunctionPointers;
- (BOOL)supportsFunctionPointersFromRender;
- (BOOL)supportsPullModelInterpolation;
- (BOOL)supportsShaderBarycentricCoordinates;
- (NSArray<id<MTLCounterSet>> *)counterSets;
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)samplingPoint;
- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)descriptor error:(NSError **)error;
- (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp;
- (id)newIOFileHandleWithURL:(NSURL *)url error:(NSError **)error;
- (id)newIOFileHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error;
- (id)newIOHandleWithURL:(NSURL *)url error:(NSError **)error;
- (id)newIOHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error;
- (id)newIOCommandQueueWithDescriptor:(id)descriptor error:(NSError **)error;
- (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error;
- (NSUInteger)sparseTileSizeInBytes;
- (NSUInteger)sparseTileSizeInBytesForSparsePageSize:(NSInteger)sparsePageSize;
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType pixelFormat:(MTLPixelFormat)pixelFormat sampleCount:(NSUInteger)sampleCount;
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType pixelFormat:(MTLPixelFormat)pixelFormat sampleCount:(NSUInteger)sampleCount sparsePageSize:(NSInteger)sparsePageSize;
- (void)convertSparsePixelRegions:(const MTLRegion *)pixelRegions toTileRegions:(MTLRegion *)tileRegions withTileSize:(MTLSize)tileSize alignmentMode:(MTLSparseTextureRegionAlignmentMode)mode numRegions:(NSUInteger)numRegions;
- (void)convertSparseTileRegions:(const MTLRegion *)tileRegions toPixelRegions:(MTLRegion *)pixelRegions withTileSize:(MTLSize)tileSize numRegions:(NSUInteger)numRegions;
- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d;
- (void)getDefaultSamplePositions:(MTLSamplePosition *)positions count:(NSUInteger)count;
- (id)newGLDrawable;
- (int)getSurfaceMode:(unsigned long long *)mode width:(unsigned *)width height:(unsigned *)height forId:(unsigned)sid;
- (void)_purgeDevice;
- (id)rdna4NewQueue:(id)desc;
- (id)newCommandQueue;
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n;
- (id)newCommandQueueWithDescriptor:(id)d;
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts error:(NSError **)error;
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error;
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error;
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error;
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error;
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h;
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts completionHandler:(void (^)(id, id, NSError *))h;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error;
- (void)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts completionHandler:(void (^)(id, id, NSError *))h;
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
- (BOOL)supportLazyInitialization;
- (NSString *)name;
- (NSString *)vendorName;
- (NSString *)familyName;
- (NSString *)productName;
- (uint64_t)registryID;
- (MTLDeviceLocation)location;
- (NSUInteger)locationNumber;
- (uint64_t)maxTransferRate;
- (NSUInteger)maxArgumentBufferSamplerCount;
- (NSUInteger)featureProfile;
- (int)llvmVersion;
- (BOOL)supportsFamily:(NSInteger)family;
- (BOOL)supportsFeatureSet:(NSUInteger)featureSet;
- (BOOL)deviceSupportsFeatureSet:(NSUInteger)featureSet;
- (NSUInteger)argumentBuffersSupport;
- (NSUInteger)maxBufferLength;
- (uint64_t)recommendedMaxWorkingSetSize;
- (BOOL)hasUnifiedMemory;
- (BOOL)isHeadless;
- (BOOL)isLowPower;
- (BOOL)isRemovable;
@end

@interface GarnetGLDrawable (GarnetInternalMethods)
- (int)setDrawableSurface:(unsigned)surface mode:(unsigned long long)mode colorDepthMode:(unsigned)depth face:(unsigned)face level:(unsigned)level volatile:(unsigned)vol fixedSource:(unsigned)fixed scaleOptions:(unsigned)scale scaledWidth:(unsigned)w scaledHeight:(unsigned)h;
- (IOSurfaceRef)lookupIOSurfaceAtIndex:(unsigned long long)i;
- (unsigned)width;
- (unsigned)height;
- (unsigned)surfaceWidth;
- (unsigned)surfaceHeight;
- (int)clearDrawable;
- (int)setSwapInterval:(int)interval limit:(int)limit;
- (int)setSwapRectX:(unsigned)x y:(unsigned)y w:(unsigned)w h:(unsigned)h;
- (unsigned long long)windowMode;
- (int)signalSharedEvent:(id)e value:(unsigned long long)v operation:(unsigned long long)op;
@end

@interface RDNA4ArgumentEncoder (GarnetInternalMethods)
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (NSUInteger)encodedLength;
- (NSUInteger)alignment;
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset;
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)startOffset arrayElement:(NSUInteger)element;
- (void *)rdna4Destination:(NSUInteger)index type:(MTLDataType)type;
- (uint64_t)rdna4BufferAddress:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (uint64_t)rdna4TextureToken:(id<MTLTexture>)texture;
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (uint64_t)rdna4SamplerToken:(id<MTLSamplerState>)sampler;
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained *)textures withRange:(NSRange)range;
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)samplers withRange:(NSRange)range;
- (uint64_t)rdna4IndirectCommandBufferAddress:(id<MTLIndirectCommandBuffer>)icb;
- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer atIndex:(NSUInteger)index;
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained *)buffers withRange:(NSRange)range;
- (uint64_t)rdna4RenderPipelineToken:(id<MTLRenderPipelineState>)pipeline;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index;
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range;
- (uint64_t)rdna4ComputePipelineToken:(id<MTLComputePipelineState>)pipeline;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index;
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range;
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure atIndex:(NSUInteger)index;
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)intersectionFunctionTable atIndex:(NSUInteger)index;
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)intersectionFunctionTables withRange:(NSRange)range;
- (void *)constantDataAtIndex:(NSUInteger)index;
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index;
@end

@interface RDNA4ArgumentLayout (GarnetInternalMethods)
- (id)device;
- (BOOL)bufferLayoutMatchesFrontEndLayout;
- (void)setStructType:(MTLStructType *)type withDevice:(id)device;
- (NSUInteger)constantOffsetAtIndex:(NSUInteger)index;
- (id)bufferLayoutForResourceAtIndex:(NSUInteger)index;
@end

@interface RDNA4ArgumentResourceToken (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ArgumentSamplerOwners (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4BinaryArchive (GarnetInternalMethods)
- (BOOL)rdna4StoreEntry:(const rdna4ba_entry_view_t *)view error:(NSError **)error;
- (BOOL)rdna4LookupKind:(uint32_t)kind keyU64:(uint64_t)keyU64 keySha:(const uint8_t[32])keySha spv0:(NSData **)spv0 meta0:(NSData **)meta0 spv1:(NSData **)spv1 meta1:(NSData **)meta1 vkCache:(NSData **)vkCache;
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

@interface RDNA4Buffer (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err;
- (void *)contents;
- (NSUInteger)length;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)resourceOptions;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (void)didModifyRange:(NSRange)r;
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
- (NSUInteger)allocatedSize;
- (void *)virtualAddress;
- (void)addDebugMarker:(NSString *)m range:(NSRange)r;
- (void)removeAllDebugMarkers;
- (IOSurfaceRef)iosurface;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr;
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi;
- (id)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr;
@end

@interface RDNA4CommandBuffer (GarnetInternalMethods)
- (void)rdna4CBRead:(RDNA4Texture *)t;
- (void)commit;
- (void)commitAndHold;
- (BOOL)commitAndWaitUntilSubmitted;
- (void)rdna4CBSubmit:(uint64_t)tq;
- (void)rdna4DispNote:(RDNA4Texture *)t bits:(uint32_t)bits pso:(RDNA4RenderPipelineState *)p;
- (void)rdna4DispDraw:(RDNA4Texture *)t bits:(uint32_t)bits rect:(rdna4df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(RDNA4Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc;
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r;
- (void)encodeSignalEvent:(id)e value:(uint64_t)v;
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v;
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v timeout:(uint32_t)ms;
- (NSUInteger)protectionOptions;
- (void)setProtectionOptions:(NSUInteger)o;
- (void)rdna4DispDone:(VkResult)r;
- (void)rdna4DispSourceWrites:(RDNA4CommandSegment *)segment;
- (BOOL)rdna4PrepareLinearImages:(RDNA4CommandSegment *)segment error:(NSError **)error;
- (void)rdna4OccResolve;
- (void)rdna4BindArguments:(const RDNA4ArgumentTables *)tables point:(VkPipelineBindPoint)point layout:(VkPipelineLayout)layout;
- (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d;
- (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d;
- (id)computeCommandEncoderWithDispatchType:(NSUInteger)t;
@end

@interface RDNA4CommandQueue (GarnetInternalMethods)
- (id)device;
- (BOOL)setGPUPriority:(NSUInteger)p;
- (BOOL)setGPUPriority:(NSUInteger)p offset:(uint16_t)o;
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p;
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p offset:(uint16_t)o;
- (BOOL)_setGPUPriority:(NSUInteger)p backgroundPriority:(NSUInteger)b;
- (NSUInteger)getGPUPriority;
- (NSUInteger)getBackgroundGPUPriority;
- (void)setCompletionQueue:(id)q;
- (id)commandBuffer;
- (id)commandBufferWithUnretainedReferences;
- (id)commandBufferWithDescriptor:(id)d;
- (void)submitCommandBuffers:(id __unsafe_unretained const *)cbs count:(NSUInteger)n;
- (void)completeCommandBuffers:(id __unsafe_unretained *)cbs count:(NSUInteger)n;
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

@interface RDNA4CounterSampleBuffer (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev descriptor:(MTLCounterSampleBufferDescriptor *)d error:(NSError **)err;
- (MTLStorageMode)rdna4StorageMode;
- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb vkCommandBuffer:(VkCommandBuffer)cmd sampleIndex:(NSUInteger)sampleIndex barrier:(BOOL)barrier stageFlags:(VkPipelineStageFlagBits)stage who:(const char *)who;
- (BOOL)rdna4ResolveRange:(NSRange)range intoBytes:(uint64_t *)dst error:(NSError **)err;
@end

@interface RDNA4DepthStencilState (GarnetInternalMethods)
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
@end

@interface RDNA4DynamicLibrary (GarnetInternalMethods)
- (void)rdna4RebuildCachesLocked;
@end

@interface RDNA4Fence (GarnetInternalMethods)
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
@end

@interface RDNA4FunctionHandle (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev ownerPipeline:(id)pso ownerToken:(uint64_t)token stage:(NSUInteger)stage functionType:(MTLFunctionType)ftype name:(NSString *)name handleId:(uint32_t)handleId;
@end

@interface RDNA4Heap (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d;
- (void)rdna4FreeBlockLocked:(rdna4hb_reclaim)block;
- (void)rdna4CollectLocked;
- (BOOL)rdna4AcquireBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr dedicated:(BOOL)dedicated hostImport:(BOOL)hostImport mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize error:(NSError **)err;
- (BOOL)rdna4AcquirePlacementBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr offset:(NSUInteger)placeOffset hostImport:(BOOL)hostImport requiresDedicated:(BOOL)requiresDedicated mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize error:(NSError **)err;
- (BOOL)rdna4OptionsOK:(NSUInteger)o what:(const char *)what;
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td;
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off;
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size offset:(NSUInteger)off;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size atOffset:(NSUInteger)off;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d offset:(NSUInteger)off;
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d atOffset:(NSUInteger)off;
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al;
- (NSUInteger)setPurgeableState:(NSUInteger)st;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)usedSize;
- (NSUInteger)currentAllocatedSize;
- (NSUInteger)allocatedSize;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (NSUInteger)resourceOptions;
- (NSUInteger)unfilteredResourceOptions;
- (NSUInteger)protectionOptions;
- (uint64_t)gpuAddress;
- (uint64_t)memoryPoolId;
- (NSString *)formattedDescription:(NSUInteger)indent;
@end

@interface RDNA4ICBArgOwners (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4IOCommandBuffer (GarnetInternalMethods)
+ (void)initialize;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (BOOL)retainedReferences;
- (NSInteger)status;
- (NSError *)error;
- (void)rdna4IOLatchLocked:(rdna4io_reason_t)reason mtlErr:(NSInteger)code detail:(NSString *)detail;
- (BOOL)rdna4IORecordCommandLocked:(RDNA4IOCommand *)cmd;
- (void)addCompletedHandler:(void (^)(id))block;
- (void)loadBytes:(void *)pointer size:(NSUInteger)size sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)loadBuffer:(id)buffer offset:(NSUInteger)offset size:(NSUInteger)size sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)loadTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level size:(MTLSize)size sourceBytesPerRow:(NSUInteger)sourceBytesPerRow sourceBytesPerImage:(NSUInteger)sourceBytesPerImage destinationOrigin:(MTLOrigin)destinationOrigin sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)copyStatusToBuffer:(id)buffer offset:(NSUInteger)offset;
- (void)addBarrier;
- (void)waitForEvent:(id)event value:(uint64_t)value;
- (void)signalEvent:(id)event value:(uint64_t)value;
- (void)pushDebugGroup:(NSString *)string;
- (void)popDebugGroup;
- (void)enqueue;
- (BOOL)rdna4IOWriteStatus:(int64_t)statusVal toBuffer:(RDNA4Buffer *)dst offset:(uint64_t)off;
- (void)rdna4IOFinishWithReason:(rdna4io_reason_t)reason mtlErr:(NSInteger)mtlErr detail:(NSString *)detail startIdx:(NSUInteger)startIdx releaseSlot:(uint64_t)releaseSlot;
- (void)commit;
- (void)tryCancel;
- (void)waitUntilCompleted;
- (BOOL)rdna4IOCancelledNow;
- (BOOL)rdna4IOExecLoadBuffer:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled;
- (BOOL)rdna4IOExecLoadTexture:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled;
- (BOOL)rdna4IOExecWaitEvent:(id)event value:(uint64_t)value error:(NSError **)err cancelled:(BOOL *)cancelled;
@end

@interface RDNA4IOCommandQueue (GarnetInternalMethods)
+ (void)initialize;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (id)commandBuffer;
- (id)commandBufferWithUnretainedReferences;
- (void)rdna4IODispatchLocked;
- (void)enqueueBarrier;
@end

@interface RDNA4IOFileHandle (GarnetInternalMethods)
+ (void)initialize;
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url compressionMethod:(NSInteger)method compressed:(BOOL)compressed error:(NSError **)err;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
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

@interface RDNA4MeshRenderPipelineState (GarnetInternalMethods)
+ (void)initialize;
- (const RDNA4PB *)rdna4PbO;
- (uint32_t)rdna4NpbO;
- (const RDNA4PB *)rdna4PbM;
- (uint32_t)rdna4NpbM;
- (VkPipeline)vkPipeline;
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage;
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage;
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage;
- (VkShaderModule)rdna4ShaderModuleForObjectLocalLocked:(const uint32_t[3])local error:(NSError **)err;
- (VkShaderModule)rdna4ShaderModuleForMeshLocalLocked:(const uint32_t[3])local error:(NSError **)err;
- (VkPipeline)rdna4BuildObjectComputeLocked:(const uint32_t[3])objLocal error:(NSError **)err;
- (VkPipeline)rdna4BuildMeshGfxLocked:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk rflags:(uint32_t)rflags objLocal:(const uint32_t[3])objLocal meshLocal:(const uint32_t[3])meshLocal error:(NSError **)err;
- (VkPipeline)rdna4MeshPipelineForCull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk rflags:(uint32_t)rflags objLocal:(const uint32_t[3])objLocal meshLocal:(const uint32_t[3])meshLocal error:(NSError **)err;
@end

@interface RDNA4RTArgOwners (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ReflectionCompiler (GarnetInternalMethods)
- (NSUInteger)compilerFlags;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync binaryArchives:(id)archives completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync pipelineLibrary:(id)library completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
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
- (RDNA4MeshEncoderState *)rdna4MeshState;
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

@interface RDNA4ResidencySet (GarnetInternalMethods)
+ (void)initialize;
- (id)device;
- (NSString *)label;
- (uint64_t)allocatedSize;
- (NSUInteger)allocationCount;
- (NSArray *)allAllocations;
- (BOOL)containsAllocation:(id)allocation;
- (void)addAllocation:(id)allocation;
- (void)addAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count;
- (void)removeAllocation:(id)allocation;
- (void)removeAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count;
- (void)removeAllAllocations;
- (void)commit;
- (void)requestResidency;
- (void)endResidency;
- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot;
@end

@interface RDNA4ResidencySnapshot (GarnetInternalMethods)
- (instancetype)initWithAllocations:(NSArray *)allocations allocatedSize:(uint64_t)allocatedSize generation:(uint64_t)generation;
- (NSArray *)rdna4Allocations;
- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device;
@end

@interface RDNA4ResourceBindings (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ResourceStateEncoder (GarnetInternalMethods)
+ (void)initialize;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)getType;
- (void)insertDebugSignpost:(NSString *)s;
- (void)pushDebugGroup:(NSString *)s;
- (void)popDebugGroup;
- (void)rdna4FlushPendingOps;
- (void)updateTextureMappings:(id<MTLTexture>)texture mode:(MTLSparseTextureMappingMode)mode regions:(const MTLRegion *)regions mipLevels:(const NSUInteger *)mipLevels slices:(const NSUInteger *)slices numRegions:(NSUInteger)numRegions;
- (void)updateTextureMapping:(id<MTLTexture>)texture mode:(MTLSparseTextureMappingMode)mode region:(MTLRegion)region mipLevel:(NSUInteger)mipLevel slice:(NSUInteger)slice;
- (void)updateTextureMapping:(id<MTLTexture>)texture mode:(MTLSparseTextureMappingMode)mode indirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset;
- (void)endEncoding;
@end

@interface RDNA4SamplerBinding (GarnetInternalMethods)
+ (instancetype)bindingWithBaseStorage:(RDNA4SamplerStorage *)base lodMinClamp:(float)lodMin lodMaxClamp:(float)lodMax error:(NSError **)err;
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison normalizedCoordinates:(BOOL)normalized error:(NSError **)err;
@end

@interface RDNA4SamplerState (GarnetInternalMethods)
- (RDNA4SamplerBinding *)rdna4BindingWithLODMinClamp:(float)lodMin lodMaxClamp:(float)lodMax error:(NSError **)err;
- (BOOL)supportArgumentBuffers;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
@end

@interface RDNA4SparseBackingPool (GarnetInternalMethods)
- (BOOL)ensureFreeTilesLocked:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err;
@end

@interface RDNA4SparseHeap (GarnetInternalMethods)
+ (void)initialize;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td;
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o;
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off;
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off;
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al;
- (NSUInteger)setPurgeableState:(NSUInteger)st;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (NSUInteger)size;
- (NSUInteger)usedSize;
- (NSUInteger)currentAllocatedSize;
- (NSUInteger)allocatedSize;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (NSUInteger)resourceOptions;
- (NSUInteger)unfilteredResourceOptions;
- (MTLHeapType)type;
- (NSUInteger)protectionOptions;
- (uint64_t)gpuAddress;
- (uint64_t)memoryPoolId;
- (NSString *)formattedDescription:(NSUInteger)indent;
@end

@interface RDNA4SparseTexture (GarnetInternalMethods)
- (BOOL)isSparse;
- (NSUInteger)firstMipmapInTail;
- (NSUInteger)tailSizeInBytes;
- (NSUInteger)allocatedSize;
- (id)heap;
- (NSUInteger)heapOffset;
- (BOOL)isAliasable;
- (void)makeAliasable;
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi;
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice;
- (BOOL)rdna4ValidateRegions:(const rdna4sp_region *)regions mipLevels:(const uint64_t *)mips slices:(const uint64_t *)slices count:(size_t)count mode:(MTLSparseTextureMappingMode)mode error:(NSError **)err;
- (BOOL)rdna4ExecuteSparseMode:(MTLSparseTextureMappingMode)mode regions:(const rdna4sp_region *)regions mipLevels:(const uint64_t *)mips slices:(const uint64_t *)slices count:(size_t)count error:(NSError **)err;
@end

@interface RDNA4Texture (GarnetInternalMethods)
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err;
- (BOOL)rdna4DispImgOnly;
- (unsigned)rdna4DispSid;
- (IOSurfaceRef)rdna4UCSurface;
- (void)rdna4AliasKey:(uint32_t *)sid off:(uint64_t *)off;
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi;
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr;
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice;
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl;
- (unsigned)rdna4Aspects;
- (unsigned)rdna4CopyAspect;
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (MTLPixelFormat)pixelFormat;
- (NSUInteger)sampleCount;
- (NSUInteger)arrayLength;
- (NSUInteger)usage;
- (BOOL)isFramebufferOnly;
- (NSUInteger)firstMipmapInTail;
- (NSUInteger)tailSizeInBytes;
- (BOOL)isSparse;
- (BOOL)allowGPUOptimizedContents;
- (id)parentTexture;
- (NSUInteger)parentRelativeLevel;
- (NSUInteger)parentRelativeSlice;
- (id)buffer;
- (NSUInteger)bufferBytesPerRow;
- (IOSurfaceRef)iosurface;
- (NSUInteger)iosurfacePlane;
- (NSUInteger)allocatedSize;
- (NSUInteger)resourceOptions;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
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
- (id)rootResource;
- (BOOL)isShareable;
- (id)newSharedTextureHandle;
- (id)remoteStorageTexture;
- (id)newRemoteTextureViewForDevice:(id)d;
- (MTLTextureSwizzleChannels)swizzle;
- (BOOL)isDrawable;
- (void)didModifyData;
- (BOOL)canGenerateMipmapLevels;
- (void)generateMipmapLevel:(NSUInteger)l slice:(NSUInteger)s;
- (uint32_t)swizzleKey;
- (NSUInteger)numFaces;
- (NSUInteger)resourceIndex;
- (void *)virtualAddress;
- (id)rdna4View:(MTLPixelFormat)pf type:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw;
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf;
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl;
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw;
- (instancetype)initViewOf:(RDNA4Texture *)root pixelFormat:(MTLPixelFormat)pf type:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err;
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


// ---- Shared Vulkan function pointers ----
#define X(n) GARNET_INTERNAL_EXTERN PFN_##n n;
RDNA4_VK_FUNCS(X)
#undef X

// ---- Shared internal globals ----
GARNET_INTERNAL_EXTERN RDNA4BackendState RDNA4R;
GARNET_INTERNAL_EXTERN RDNA4T1State T1;
GARNET_INTERNAL_EXTERN RDNA4PoolState RDNA4P;
GARNET_INTERNAL_EXTERN RDNA4DrawOptState RDNA4DO;
GARNET_INTERNAL_EXTERN RDNA4LedgerState RDNA4LED;
GARNET_INTERNAL_EXTERN RDNA4ScanoutState RDNA4S;
GARNET_INTERNAL_EXTERN RDNA4XScanoutState RDNA4X;
GARNET_INTERNAL_EXTERN RDNA4CRCState RDNA4C;
GARNET_INTERNAL_EXTERN RDNA4CBLogState RDNA4CB;
GARNET_INTERNAL_EXTERN RDNA4XlateState RDNA4XL;
GARNET_INTERNAL_EXTERN _Atomic int rdna4_app_admitted;
GARNET_INTERNAL_EXTERN PFN_vkGetInstanceProcAddr rdna4_gipa;
GARNET_INTERNAL_EXTERN PFN_vkGetMemoryHostPointerPropertiesEXT rdna4_vkGetMHPP;
GARNET_INTERNAL_EXTERN PFN_vkGetBufferDeviceAddress rdna4_vkGetBufferAddress;
GARNET_INTERNAL_EXTERN PFN_vkCmdSetSampleLocationsEXT rdna4_vkCmdSetSampleLocationsEXT;
GARNET_INTERNAL_EXTERN PFN_vkQueueWaitIdle rdna4_vkQueueWaitIdle;
GARNET_INTERNAL_EXTERN RDNA4NativeBridge rdna4_native_bridge;
GARNET_INTERNAL_EXTERN rdna4_argument_limit_snapshot RDNA4ALS;
GARNET_INTERNAL_EXTERN _Atomic int rdna4_argument_limit_full_open_state;
GARNET_INTERNAL_EXTERN _Atomic int rdna4_submit_fault;
GARNET_INTERNAL_EXTERN rdna4dc_caps RDNA4Caps;
GARNET_INTERNAL_EXTERN _Atomic int rdna4_caps_ready;
GARNET_INTERNAL_EXTERN _Atomic uint64_t rdna4_t1_frames, rdna4_t1_lastframe, rdna4_pipes_new;
GARNET_INTERNAL_EXTERN _Atomic int rdna4_dw_logged;
GARNET_INTERNAL_EXTERN _Atomic uint64_t rdna4_alloc_total;
GARNET_INTERNAL_EXTERN _Atomic uint64_t rdna4_heap_reserved;
GARNET_INTERNAL_EXTERN uint32_t rdna4_acc_port;
GARNET_INTERNAL_EXTERN _Atomic uint64_t rdna4s_imgcopies;
GARNET_INTERNAL_EXTERN _Atomic uint64_t rdna4cb_ids;
GARNET_INTERNAL_EXTERN char rdna4_rt_arg_owners_key;

// ---- Shared internal function prototypes ----
GARNET_INTERNAL_EXTERN VkPipelineCache rdna4_ba_vkcache(void);
GARNET_INTERNAL_EXTERN os_log_t rdna4_oslog(void);
GARNET_INTERNAL_EXTERN void rdna4_log_lvl(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
GARNET_INTERNAL_EXTERN void rdna4_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
GARNET_INTERNAL_EXTERN BOOL rdna4_verbose_log(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_t1_log_enabled(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_debug_log_active(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_sb_can_stat(const char *path);
GARNET_INTERNAL_EXTERN BOOL rdna4_sb_can_read(const char *path);
GARNET_INTERNAL_EXTERN BOOL rdna4_sb_can_write(const char *path);
GARNET_INTERNAL_EXTERN int rdna4_sb_stat(const char *path, struct stat *st);
GARNET_INTERNAL_EXTERN int rdna4_sb_lstat(const char *path, struct stat *st);
GARNET_INTERNAL_EXTERN BOOL rdna4_sb_access_exists(const char *path);
GARNET_INTERNAL_EXTERN int rdna4_sb_open_ro(const char *path, int flags);
GARNET_INTERNAL_EXTERN BOOL rdna4_is_ws(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_allow(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_force_fallback(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_test_fb_as_ws(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_fallback_ok(void);
GARNET_INTERNAL_EXTERN void rdna4_mark_clean(void);
GARNET_INTERNAL_EXTERN NSString *rdna4_model_name(uint32_t port);
GARNET_INTERNAL_EXTERN BOOL rdna4_number_u64(CFTypeRef field, uint64_t *out);
GARNET_INTERNAL_EXTERN rdna4dc_native_memory rdna4_nub_memory(uint32_t port);
GARNET_INTERNAL_EXTERN const char *rdna4_admit(uint32_t port);
GARNET_INTERNAL_EXTERN BOOL rdna4_headless(void);
GARNET_INTERNAL_EXTERN void rdna4_argument_limit_prepare(const rdna4dc_native_memory *nativeMemory);
GARNET_INTERNAL_EXTERN rdna4x_backend_capabilities rdna4_query_caps(void);
GARNET_INTERNAL_EXTERN rdna4dc_caps rdna4_device_caps(void);
GARNET_INTERNAL_EXTERN rdna4_gather_caps rdna4_device_gather_caps(void);
GARNET_INTERNAL_EXTERN NSError *rdna4_err(NSInteger code, NSString *msg);
GARNET_INTERNAL_EXTERN void rdna4_quarantine(id resources, VkResult fault);
GARNET_INTERNAL_EXTERN BOOL rdna4_radv_open(NSError **err);
GARNET_INTERNAL_EXTERN int rdna4_find_mem(uint32_t bits, VkMemoryPropertyFlags want);
GARNET_INTERNAL_EXTERN void rdna4_pool_latch(void);
GARNET_INTERNAL_EXTERN void rdna4_pool_tick(void);
GARNET_INTERNAL_EXTERN void rdna4_pool_report(unsigned tick);
GARNET_INTERNAL_EXTERN uint64_t rdna4_fence_open(void);
GARNET_INTERNAL_EXTERN void rdna4_fence_close(uint64_t s);
GARNET_INTERNAL_EXTERN BOOL rdna4_dp_give(VkDescriptorPool dp);
GARNET_INTERNAL_EXTERN void rdna4_dp_created(void);
GARNET_INTERNAL_EXTERN VkDescriptorPool rdna4_dp_take(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_cmd_pool_active(void);
GARNET_INTERNAL_EXTERN VkResult rdna4_fence_take(VkFence *out);
GARNET_INTERNAL_EXTERN void rdna4_fence_give(VkFence f);
GARNET_INTERNAL_EXTERN VkResult rdna4_oneshot_cmd_take(VkCommandBuffer *out);
GARNET_INTERNAL_EXTERN void rdna4_oneshot_cmd_give(VkCommandBuffer cmd);
GARNET_INTERNAL_EXTERN void rdna4nc_note_readonly_buffer(void);
GARNET_INTERNAL_EXTERN BOOL rdna4nc_any_readonly_buffer(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_icb_any_arg_owners(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_any_arg_owners(void);
GARNET_INTERNAL_EXTERN void rdna4_drawopt_latch(void);
GARNET_INTERNAL_EXTERN void rdna4_drawopt_report(unsigned tick);
GARNET_INTERNAL_EXTERN void rdna4_imp_latch(void);
GARNET_INTERNAL_EXTERN void rdna4_imp_tick(void);
GARNET_INTERNAL_EXTERN void rdna4_imp_report(unsigned tick);
GARNET_INTERNAL_EXTERN void rdna4_vs_latch(void);
GARNET_INTERNAL_EXTERN void rdna4_vs_tick(void);
GARNET_INTERNAL_EXTERN void rdna4_vs_report(unsigned tick);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_enabled(void);
GARNET_INTERNAL_EXTERN void rdna4_vs_note_pathb(BOOL skipped);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_sync_wb_on(void);
GARNET_INTERNAL_EXTERN VkImageLayout rdna4_vs_get_layout(uint64_t vsId);
GARNET_INTERNAL_EXTERN void rdna4_vs_set_layout(uint64_t vsId, VkImageLayout l);
GARNET_INTERNAL_EXTERN void rdna4_vs_cb_begin(uint64_t vsId);
GARNET_INTERNAL_EXTERN void rdna4_vs_cb_end(uint64_t vsId);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_peek_needs_upload(uint64_t vsId, uint32_t seed);
GARNET_INTERNAL_EXTERN void rdna4_vs_note_upload_skip(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_needs_upload(uint64_t vsId, uint32_t seed, BOOL needContents, BOOL willWrite, BOOL fullOverwrite);
GARNET_INTERNAL_EXTERN void rdna4_vs_note_full_clear(uint64_t vsId, uint32_t seed);
GARNET_INTERNAL_EXTERN void rdna4_vs_note_write(uint64_t vsId, uint32_t seed, BOOL syncWriteback);
GARNET_INTERNAL_EXTERN void rdna4_vs_invalidate_cpu(uint64_t vsId);
GARNET_INTERNAL_EXTERN void rdna4_vs_observe_ws_write(RDNA4Texture *t);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_flush_entry_sync(uint64_t vsId);
GARNET_INTERNAL_EXTERN void rdna4s_record_copy_img_preserve(VkCommandBuffer cmd, RDNA4Texture *t, const rdna4df_plan_t *pl, VkImageLayout savedLayout);
GARNET_INTERNAL_EXTERN uint64_t rdna4_resource_uid(_Atomic uint64_t *uid);
GARNET_INTERNAL_EXTERN BOOL rdna4nc_vk_offset_ok(VkDeviceSize off, VkDeviceSize align, unsigned use);
GARNET_INTERNAL_EXTERN BOOL rdna4_requirements2(VkBuffer buffer, VkImage image, VkMemoryRequirements *mr,
                                BOOL *prefersDedicated, BOOL *requiresDedicated,
                                BOOL requireDedicatedInfo);
GARNET_INTERNAL_EXTERN VkBufferCreateInfo rdna4_heap_buffer_create_info(NSUInteger len, VkExternalMemoryBufferCreateInfo *external);
GARNET_INTERNAL_EXTERN void rdna4_image_transition(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout oldLayout, VkImageLayout newLayout);
GARNET_INTERNAL_EXTERN VkImageLayout rdna4_use_layout(RDNA4Texture *texture, VkImageLayout preferred);
GARNET_INTERNAL_EXTERN void rdna4_tex_to(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout nl);
GARNET_INTERNAL_EXTERN void rdna4_tex_to_compute(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout nl);
GARNET_INTERNAL_EXTERN BOOL rdna4_region_ok(RDNA4Texture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const char *what);
GARNET_INTERNAL_EXTERN BOOL rdna4_heap_image_plan_make(MTLTextureDescriptor *d, BOOL aliasable, rdna4_heap_image_plan *p, NSError **err);
GARNET_INTERNAL_EXTERN NSUInteger rdna4_alup(NSUInteger v, NSUInteger a);
GARNET_INTERNAL_EXTERN BOOL rdna4_heap_reserve_capacity(uint64_t size, uint64_t limit);
GARNET_INTERNAL_EXTERN VkSampler rdna4_make_sampler(const RDNA4SampCfg *c, BOOL comparison,
                                    BOOL normalizedCoordinates,
                                    RDNA4SamplerStorage *budgetStorage,
                                    NSError **err);
GARNET_INTERNAL_EXTERN void rdna4_destroy_sampler(VkSampler sampler,
                                  RDNA4SamplerStorage *budgetStorage);
GARNET_INTERNAL_EXTERN RDNA4DSInfo rdna4_dsinfo_default(void);
GARNET_INTERNAL_EXTERN void rdna4_led_latch(void);
GARNET_INTERNAL_EXTERN void rdna4_led_add(id t, IOSurfaceRef s, uint64_t bytes, uint32_t pf, uint32_t planes, BOOL nobase);
GARNET_INTERNAL_EXTERN void rdna4_led_del(id t);
GARNET_INTERNAL_EXTERN void rdna4_led_touch(id t, int delta);
GARNET_INTERNAL_EXTERN void rdna4_uc_inc(void *s, void *c);
GARNET_INTERNAL_EXTERN void rdna4_uc_dec(void *s, void *c);
GARNET_INTERNAL_EXTERN BOOL rdna4_ioalias_acts(void);
GARNET_INTERNAL_EXTERN void rdna4_led_report(unsigned tick);
GARNET_INTERNAL_EXTERN BOOL rdna4_oneshot(NSError **err, void (^rec)(VkCommandBuffer cmd),
                          BOOL *unresolved);
GARNET_INTERNAL_EXTERN void rdna4_m6_report(void);
GARNET_INTERNAL_EXTERN BOOL rdna4s_testmode(void);
GARNET_INTERNAL_EXTERN void rdna4_scan_cc_counters(uint64_t *h, uint64_t *m, uint64_t *mm);
GARNET_INTERNAL_EXTERN BOOL rdna4s_classify(size_t w, size_t h, uint32_t spf, NSUInteger usage, size_t bpr, size_t alloc, unsigned sid, const void *base);
GARNET_INTERNAL_EXTERN BOOL rdna4s_disp_account(uint32_t mask, uint32_t sid, const char *fns, uint64_t cbno, const rdna4df_wr_t *wr, int *pinst, int *ptent);
GARNET_INTERNAL_EXTERN void rdna4s_prebind_on_radv_open(void);
GARNET_INTERNAL_EXTERN void rdna4_pc_save(const char *why);
GARNET_INTERNAL_EXTERN void rdna4_pc_maybe_save_after_compile(uint64_t compile_ns, const char *why);
GARNET_INTERNAL_EXTERN void rdna4_gpl_prewarm_start(void);
GARNET_INTERNAL_EXTERN int rdna4s_damage_on(void);
GARNET_INTERNAL_EXTERN int rdna4s_plan(const rdna4df_wr_t *wr, int inst, int tent, rdna4df_plan_t *pl);
GARNET_INTERNAL_EXTERN void rdna4s_record_copy(VkCommandBuffer cmd, VkBuffer src, VkDeviceSize srcOffset, const rdna4df_plan_t *pl);
GARNET_INTERNAL_EXTERN BOOL rdna4s_crc_on(void);
GARNET_INTERNAL_EXTERN void rdna4s_crc_note(uint32_t sid, uint64_t seq);
GARNET_INTERNAL_EXTERN void rdna4cbl_line_locked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
GARNET_INTERNAL_EXTERN void rdna4cbl_sids(char *o, size_t cap, const uint32_t *a, uint32_t k);
GARNET_INTERNAL_EXTERN void rdna4cbl_start(void);
GARNET_INTERNAL_EXTERN void rdna4cbl_skip(uint32_t sid, uint64_t seq, int slot, const char *why, int vr);
GARNET_INTERNAL_EXTERN void rdna4s_complete(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, rdna4crc_rec_t *rec, int cls, uint64_t tcommit);
GARNET_INTERNAL_EXTERN NSDictionary *rdna4s_stats(void);
GARNET_INTERNAL_EXTERN NSDictionary *rdna4s_release_request(void);
GARNET_INTERNAL_EXTERN void rdna4s_late_complete(RDNA4Texture *texture, int inst, uint64_t seq, uint32_t sid, int cls, uint64_t commit);
GARNET_INTERNAL_EXTERN void rdna4_capture_surfaces(NSArray *textures, uint64_t cbid);
GARNET_INTERNAL_EXTERN NSString *rdna4_sha256hex(NSData *d);
GARNET_INTERNAL_EXTERN int rdna4_dump_dir_ready(char *sub, size_t subn);
GARNET_INTERNAL_EXTERN int rdna4_dump_write(const char *path, NSData *data);
GARNET_INTERNAL_EXTERN void rdna4_dump_function(id fn, const char *role, NSMutableArray *paths, NSMutableArray *notes);
GARNET_INTERNAL_EXTERN NSError *rdna4_dump_pipeline(MTLRenderPipelineDescriptor *d, BOOL vertexMissing, BOOL fragmentMissing);
GARNET_INTERNAL_EXTERN BOOL rdna4x_active(void);
GARNET_INTERNAL_EXTERN NSString *rdna4x_user_dir(void);
GARNET_INTERNAL_EXTERN BOOL rdna4x_memcache_active(void);
GARNET_INTERNAL_EXTERN BOOL rdna4x_memcache_lookup(NSString *sha, NSData **outSpv, NSDictionary **outMeta, NSString **outRefusal);
GARNET_INTERNAL_EXTERN BOOL rdna4x_memcache_store(NSString *sha, const void *spv, size_t spvLen, const void *meta, size_t metaLen);
GARNET_INTERNAL_EXTERN void rdna4x_touch_hit(NSString *path, NSString *sha);
GARNET_INTERNAL_EXTERN void rdna4_lk_install(int slot, RDNA4LinkCtx *c);
GARNET_INTERNAL_EXTERN void rdna4_lk_scope_end(rdna4_lkscope *sc);
GARNET_INTERNAL_EXTERN RDNA4LinkCtx *rdna4_lk_for(id fn);
GARNET_INTERNAL_EXTERN NSError *rdna4_lk_error(id fn);
GARNET_INTERNAL_EXTERN BOOL rdna4_sync_begin(BOOL share);
GARNET_INTERNAL_EXTERN void rdna4_sync_end(BOOL found, BOOL timedout);
GARNET_INTERNAL_EXTERN NSString *rdna4x_cached_refusal(NSString *dir, NSString *sha);
GARNET_INTERNAL_EXTERN void rdna4x_engine_prepare(void);
GARNET_INTERNAL_EXTERN const rdna4x_xlib *rdna4x_ensure_xlib_raw(NSError **err);
GARNET_INTERNAL_EXTERN const rdna4x_xlib *rdna4x_ensure_xlib(NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4x_stage2_symbols_available(void);
GARNET_INTERNAL_EXTERN BOOL rdna4x_ensure_stage3_xlib(
    const rdna4x_xlib **xlibOut,
    rdna4x_translate_stage3_fn *translateOut,
    NSError **err);
GARNET_INTERNAL_EXTERN int rdna4x_run_on_worker_stack(void *(*fn)(void *), void *arg);
GARNET_INTERNAL_EXTERN int rdna4x_module_to_llvm_text(const uint8_t *bytes, size_t len, uint32_t payloadKind,
                                      char **outText, size_t *outLen, char *errBuf, size_t errCap);
GARNET_INTERNAL_EXTERN BOOL rdna4x_parse_vft_from_meta(NSDictionary *meta, rdna4vft_reflection_t *outVft, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4x_translate_fn_with_error(id fn, const char *role, uint64_t deadlineNs, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4x_translate_fn_with_samples(id fn, const char *role, uint64_t deadlineNs, uint32_t rasterSamples, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4x_array_nonempty(id obj, const char *prop);
GARNET_INTERNAL_EXTERN id rdna4x_prop(id obj, const char *prop);
GARNET_INTERNAL_EXTERN BOOL rdna4x_bool_prop(id obj, const char *prop);
GARNET_INTERNAL_EXTERN BOOL rdna4x_stage2_link_collect(id linked, id preloaded,
                                       NSArray<NSDictionary *> **depsOut,
                                       NSArray<NSDictionary *> **visibleOut,
                                       NSArray<NSDictionary *> **dynlibsOut,
                                       NSString **why);
GARNET_INTERNAL_EXTERN void rdna4x_linkage_render(id d);
GARNET_INTERNAL_EXTERN void rdna4x_linkage_compute(id d);
GARNET_INTERNAL_EXTERN NSArray<NSString *> *rdna4_spv_dirs(void);
GARNET_INTERNAL_EXTERN NSDictionary *rdna4_meta_for_spv(NSString *path);
GARNET_INTERNAL_EXTERN const char *rdna4_argument_scan_status_name(rdna4_as_status status);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_scan_plan_build(rdna4_argument_scan_reflection_plan *out,
                                           NSDictionary *beforeLowering,
                                           NSDictionary *afterLowering,
                                           const char *role,
                                           NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_coordinate_band_index(const RDNA4PB *source, uint32_t element,
                                        uint64_t *index);
GARNET_INTERNAL_EXTERN RDNA4CoordinateFlagResult rdna4_coordinate_flag_merge(
        uint32_t flags[32], BOOL assigned[32], uint64_t index, BOOL pixel);
GARNET_INTERNAL_EXTERN NSData *rdna4_spv_set(NSData *spv, uint32_t set);
GARNET_INTERNAL_EXTERN BOOL rdna4_stage_dsl(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, NSError **err);
GARNET_INTERNAL_EXTERN BOOL rdna4_stage_dsl_ex(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, BOOL allowPush, BOOL *usedPush, NSError **err);
GARNET_INTERNAL_EXTERN VkFormat rdna4_vkvfmt(MTLVertexFormat f);
GARNET_INTERNAL_EXTERN VkBlendFactor rdna4_bf(MTLBlendFactor f, BOOL *bad);
GARNET_INTERNAL_EXTERN VkBlendOp rdna4_bo(MTLBlendOperation o, BOOL *bad);
GARNET_INTERNAL_EXTERN NSString *rdna4_fn_sha(id fn);
GARNET_INTERNAL_EXTERN void rdna4hs_register(id obj, NSArray<NSString *> *shas, BOOL metaRequired, NSString *name, BOOL (^build)(id));
GARNET_INTERNAL_EXTERN BOOL rdna4_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta);
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
GARNET_INTERNAL_EXTERN uint64_t rdna4_argument_resource_token_bindless(id object, id device, BOOL texture, BOOL markBindless);
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
GARNET_INTERNAL_EXTERN uint64_t rdna4_now(void);
GARNET_INTERNAL_EXTERN VkComponentMapping rdna4_view_map(const RDNA4Fmt *f, MTLTextureSwizzleChannels swz);
GARNET_INTERNAL_EXTERN BOOL rdna4_is_identity_map(const VkComponentMapping *m);
GARNET_INTERNAL_EXTERN const RDNA4Fmt *rdna4_fmt(MTLPixelFormat f);
GARNET_INTERNAL_EXTERN const RDNA4Fmt *rdna4_fmt_ds(MTLPixelFormat f);
GARNET_INTERNAL_EXTERN unsigned rdna4_ms_mask(void);
GARNET_INTERNAL_EXTERN VkFormatFeatureFlags rdna4_fmt_feats(const RDNA4Fmt *f);
GARNET_INTERNAL_EXTERN void rdna4_add_protocols(Class c, const char **names, unsigned n, const char *tag);
GARNET_INTERNAL_EXTERN BOOL rdna4_event_is_shared(id event);
GARNET_INTERNAL_EXTERN uint64_t rdna4_event_value(id event);
GARNET_INTERNAL_EXTERN void rdna4_event_set_value(id event, uint64_t value);
GARNET_INTERNAL_EXTERN void rdna4_event_fail_signals(NSArray *signals);
GARNET_INTERNAL_EXTERN BOOL rdna4_event_failed(id event, uint64_t value);
GARNET_INTERNAL_EXTERN void rdna4_schedule_kick(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_ios_touch(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL need, BOOL write, void (^endPass)(void));
GARNET_INTERNAL_EXTERN void rdna4_ios_usek(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write, uint32_t kind);
GARNET_INTERNAL_EXTERN int rdna4_fence_wait_slice(void *ctx, uint64_t timeoutNs);
GARNET_INTERNAL_EXTERN void rdna4_fence_wait_pending(void *ctx, int slices);
GARNET_INTERNAL_EXTERN NSArray<id<MTLCounterSet>> *rdna4_device_counter_sets(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_device_supports_counter_sampling(MTLCounterSamplingPoint point);
GARNET_INTERNAL_EXTERN id<MTLCounterSampleBuffer> rdna4_device_new_counter_sample_buffer(
    id dev, MTLCounterSampleBufferDescriptor *descriptor, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_device_sample_timestamps(MTLTimestamp *cpuTimestamp, MTLTimestamp *gpuTimestamp);
GARNET_INTERNAL_EXTERN void rdna4_full_barrier(VkCommandBuffer cb);
GARNET_INTERNAL_EXTERN VkPrimitiveTopology rdna4_topo(MTLPrimitiveType t);
GARNET_INTERNAL_EXTERN VkRenderPass rdna4_mk_rp(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch);
GARNET_INTERNAL_EXTERN VkShaderStageFlags rdna4_argument_stage_flags(uint32_t stages,
                                                     BOOL compute);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_shape(unsigned shape, VkImageViewType *viewType, BOOL *depth);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_reflection_version(NSDictionary *metadata,
                                              BOOL *present,
                                              uint64_t *version);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_normalized_metadata(NSDictionary *metadata,
                                                BOOL *normalized,
                                                NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_uses_shape_registry(NSDictionary *metadata);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_stages_compatible(NSDictionary *a,
                                                    NSDictionary *b);
GARNET_INTERNAL_EXTERN NSData *rdna4_gpu_argument_spv(NSData *spv, NSDictionary **metadata, uint64_t deadline, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_metadata(
    const RDNA4sRefl *reflection, int stageSet, NSDictionary *metadata,
    NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_metadata_apply(
    RDNA4ArgumentTables *tables, const RDNA4sRefl *reflection,
    int stageSet, NSDictionary *metadata, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_descriptor_validate(
    const rdna4_argument_descriptor_usage *usage, BOOL uab, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_layouts(RDNA4ArgumentTables *tables, const RDNA4sRefl *vertex,
    const RDNA4sRefl *fragment, BOOL compute, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_argument_tables_destroy(RDNA4ArgumentTables *tables);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_stage_remap(const RDNA4ArgumentTables *tables,
    RDNA4PB *bindings, uint32_t count, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_argbuf_icb_snapshot(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_scalar(MTLDataType type, NSUInteger *size, NSUInteger *alignment);
GARNET_INTERNAL_EXTERN id rdna4_argument_resource_for_token(uint64_t token, BOOL texture, id device);
GARNET_INTERNAL_EXTERN NSArray *rdna4_argument_resource_snapshot(id device, NSSet *declared, NSSet *samplers);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_intent_matches(id token, id buffer, NSRange range,
                                                   BOOL fillOnly);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_intent_matches_ranges(
        id token, id buffer, NSRange range, NSArray<NSValue *> *writeRanges,
        BOOL fillOnly);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_store(id buffer, NSUInteger offset, uint64_t value, id sampler);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_write(id buffer, NSUInteger offset, id sampler, uint64_t token);
GARNET_INTERNAL_EXTERN void rdna4_argument_note_dirty_range(id buffer, NSRange range);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_scope(
        id device, const RDNA4ArgumentScanPrepared *scan,
        NSSet *declaredBuffers, NSSet *writable,
        NSMutableSet *samplers, NSMutableArray<NSDictionary *> *writes,
        NSString **why);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent_ranges(id buffer, NSRange range,
        NSArray<NSValue *> *writeRanges, NSSet *candidates, BOOL fillOnly);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent(id buffer, NSRange range,
        NSSet *candidates, BOOL fillOnly);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_add_candidates(id token, NSSet *candidates);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_copy(id source, NSUInteger sourceOffset,
        id destination, NSUInteger offset, NSUInteger size);
GARNET_INTERNAL_EXTERN uint64_t rdna4_argument_next_submission_epoch(void);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_submit(id token, uint64_t submissionEpoch);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_abandon(id token);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_complete(id token);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_reflection(id<MTLFunction> function, NSArray<MTLArgument *> *arguments);
#pragma clang diagnostic pop
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_needed(NSArray *intents);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_finish(
    id attempt, BOOL fenceSucceeded, uint64_t submissionEpoch,
    NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_encode(
    NSArray<RDNA4ArgumentWriteIntent *> *intents,
    VkCommandBuffer command,
    NSArray * __strong *outAttempts,
    NSError **error);
GARNET_INTERNAL_EXTERN id rdna4_sparse_heap_new(id dev, MTLHeapDescriptor *d, NSError **err);
GARNET_INTERNAL_EXTERN id rdna4_cb_new_resource_state_encoder(RDNA4CommandBuffer *cb, id desc);
GARNET_INTERNAL_EXTERN NSUInteger rdna4_device_sparse_tile_size_in_bytes(void);
GARNET_INTERNAL_EXTERN NSUInteger rdna4_device_sparse_tile_size_for_page_size(NSInteger sparsePageSize);
GARNET_INTERNAL_EXTERN MTLSize rdna4_device_sparse_tile_size(MTLTextureType textureType,
                                                                     MTLPixelFormat pixelFormat,
                                                                     NSUInteger sampleCount);
GARNET_INTERNAL_EXTERN MTLSize rdna4_device_sparse_tile_size_with_page_size(MTLTextureType textureType,
                                                                                    MTLPixelFormat pixelFormat,
                                                                                    NSUInteger sampleCount,
                                                                                    NSInteger sparsePageSize);
GARNET_INTERNAL_EXTERN void rdna4_device_convert_sparse_pixel_regions(const MTLRegion *pixelRegions,
                                                                              MTLRegion *tileRegions,
                                                                              MTLSize tileSize,
                                                                              MTLSparseTextureRegionAlignmentMode mode,
                                                                              NSUInteger numRegions);
GARNET_INTERNAL_EXTERN void rdna4_device_convert_sparse_tile_regions(const MTLRegion *tileRegions,
                                                                             MTLRegion *pixelRegions,
                                                                             MTLSize tileSize,
                                                                             NSUInteger numRegions);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_raytracing(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_raytracing_from_render(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_runtime_supports_primitive_motion_blur(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_curves_enabled(void);
GARNET_INTERNAL_EXTERN BOOL rdna4_rt_motion_enabled(void);
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
GARNET_INTERNAL_EXTERN id<MTLAccelerationStructure> rdna4_rt_heap_new_as_with_size(
    RDNA4Heap *heap, id dev, NSUInteger size, BOOL hasOffset, NSUInteger offset);
GARNET_INTERNAL_EXTERN id<MTLAccelerationStructure> rdna4_rt_heap_new_as_with_descriptor(
    RDNA4Heap *heap, id dev, MTLAccelerationStructureDescriptor *descriptor, BOOL hasOffset, NSUInteger offset);
GARNET_INTERNAL_EXTERN id rdna4_mesh_device_new_pipeline(
    id dev, id desc, NSUInteger options, MTLRenderPipelineReflection **refl, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_mesh_device_new_pipeline_async(
    id dev, id desc, NSUInteger options, void (^handler)(id, id, NSError *));
GARNET_INTERNAL_EXTERN void rdna4sp_register(id<MTLDevice> device, io_service_t accelerator);
GARNET_INTERNAL_EXTERN NSData *rdna4_function_reflection_data(id function);

// ---- Shared static inline helpers ----
static inline rdna4dc_metal3_caps rdna4_metal3_caps(void) {
    return rdna4_device_caps().metal3;
}
static inline void rdna4_qlock(void) { uint64_t t_ = rdna4_now(); pthread_mutex_lock(&(RDNA4R.qlock)); rdna4t_add(&T1.qlk, rdna4_now() - t_); }
static inline void rdna4_compute_qlock(void) { pthread_mutex_lock(&(RDNA4R.computeQlock)); }
static inline int rdna4cbl_on(void) { return atomic_load_explicit(&RDNA4CB.on, memory_order_relaxed); }
static inline BOOL rdna4_fmt_is_bc(const RDNA4Fmt *f) { return f != NULL && (f->flags & RDNA4F_BC) != 0; }
static inline void rdna4_int_bits(MTLPixelFormat pf, unsigned *rgb, unsigned *a) {
    switch (pf) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
        *rgb = 8; *a = 8; return;
    case MTLPixelFormatRGB10A2Uint:
        *rgb = 10; *a = 2; return;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
        *rgb = 32; *a = 32; return;
    default:
        *rgb = 16; *a = 16; return;
    }
}
static inline rdna4swz_t rdna4_swz_from_mtl(MTLTextureSwizzleChannels swz) {
    return (rdna4swz_t){ (uint8_t)swz.red, (uint8_t)swz.green, (uint8_t)swz.blue, (uint8_t)swz.alpha };
}
static inline MTLTextureSwizzleChannels rdna4_swz_to_mtl(rdna4swz_t swz) {
    return (MTLTextureSwizzleChannels){ (MTLTextureSwizzle)swz.r, (MTLTextureSwizzle)swz.g, (MTLTextureSwizzle)swz.b, (MTLTextureSwizzle)swz.a };
}

#endif // GARNET_METAL_INTERNAL_H
