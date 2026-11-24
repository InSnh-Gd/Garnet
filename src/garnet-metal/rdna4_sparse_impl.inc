// ---------------------------------------------------------------------------------------------------------------
// Metal Sparse Textures & MTLResourceStateCommandEncoder (Task 5.3) over Vulkan vkQueueBindSparse / GFX12 PRT.
//
// CRITICAL: NEVER enable MTLGPUFamilyApple6 (1006). Sparse textures work on Mac2/Metal3 via explicit
// sparseTileSize* queries, sparse heaps, and MTLResourceStateCommandEncoder gated strictly on
// rdna4dc_metal3_sparse_texture_ok() (which itself requires kernel ABI 1.13 RDNA4N_CAP_VM_PRT + RADV 0028).
//
// Wave 3 integration notes (do NOT edit existing files in Wave 2B):
//   1. GarnetDevice.m:
//      a) Place `#include "rdna4_sparse_impl.inc"` after `#include "rdna4_gl_drawable_impl.inc"`
//         (after RDNA4Buffer, RDNA4Texture, RDNA4Heap, RDNA4Fence, RDNA4CommandBuffer, rdna4_oneshot,
//         and rdna4fw_wait_bounded are defined).
//      b) Register MTLResourceStateCommandEncoder (and optional census entry) in +[GarnetDevice load]:
//           { const char *n[] = { "MTLResourceStateCommandEncoder", "MTLCommandEncoder" };
//             rdna4_add_protocols([RDNA4ResourceStateEncoder class], n, 2, "RDNA4ResourceStateEncoder"); }
//      c) Wire MTLDevice sparse selectors in @implementation GarnetDevice (keep supportsFamily:Apple6 == NO!):
//           - (NSUInteger)sparseTileSizeInBytes { return rdna4_device_sparse_tile_size_in_bytes(); }
//           - (NSUInteger)sparseTileSizeInBytesForSparsePageSize:(NSInteger)sparsePageSize {
//               return rdna4_device_sparse_tile_size_for_page_size(sparsePageSize);
//           }
//           - (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
//                                        pixelFormat:(MTLPixelFormat)pixelFormat
//                                        sampleCount:(NSUInteger)sampleCount {
//               return rdna4_device_sparse_tile_size(textureType, pixelFormat, sampleCount);
//           }
//           - (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
//                                        pixelFormat:(MTLPixelFormat)pixelFormat
//                                        sampleCount:(NSUInteger)sampleCount
//                                     sparsePageSize:(NSInteger)sparsePageSize {
//               return rdna4_device_sparse_tile_size_with_page_size(textureType, pixelFormat, sampleCount, sparsePageSize);
//           }
//           - (void)convertSparsePixelRegions:(const MTLRegion *)pixelRegions
//                               toTileRegions:(MTLRegion *)tileRegions
//                                withTileSize:(MTLSize)tileSize
//                               alignmentMode:(MTLSparseTextureRegionAlignmentMode)mode
//                                  numRegions:(NSUInteger)numRegions {
//               rdna4_device_convert_sparse_pixel_regions(pixelRegions, tileRegions, tileSize, mode, numRegions);
//           }
//           - (void)convertSparseTileRegions:(const MTLRegion *)tileRegions
//                             toPixelRegions:(MTLRegion *)pixelRegions
//                               withTileSize:(MTLSize)tileSize
//                                 numRegions:(NSUInteger)numRegions {
//               rdna4_device_convert_sparse_tile_regions(tileRegions, pixelRegions, tileSize, numRegions);
//           }
//      d) Route MTLHeapTypeSparse (2) in -[GarnetDevice newHeapWithDescriptor:]:
//           if ((NSInteger)d.type == RDNA4SP_HEAP_SPARSE) return rdna4_sparse_heap_new(self, d, NULL);
//   2. RDNA4CommandBuffer (Encoders) / rdna4_command_buffer_impl.inc:
//      Add resource state encoder factories:
//           - (id)resourceStateCommandEncoder { return rdna4_cb_new_resource_state_encoder(self, nil); }
//           - (id)resourceStateCommandEncoderWithDescriptor:(id)d {
//               return rdna4_cb_new_resource_state_encoder(self, d);
//           }
//      Note: RDNA4ResourceStateEncoder uses rdna4EncodeOrderedHostAction: + binary VkSemaphore handoff
//      between vkQueueBindSparse and vkQueueSubmit, so no scheduler rewrite is required. Optional future
//      optimization: attach waitSemaphores directly onto RDNA4CommandSegment for zero-bubble pipelining.
//   3. rdna4_texture_impl.inc & rdna4_heap_impl.inc:
//      RDNA4SparseTexture subclasses RDNA4Texture (isSparse == YES, firstMipmapInTail, tailSizeInBytes),
//      so all existing render/compute/blit encoders accept it via isKindOfClass:[RDNA4Texture class].
//      RDNA4Heap (RDNA4SparseBacking) attaches a lazy 64 KiB backing pool for Private Automatic/Placement
//      heaps; RDNA4SparseHeap natively backs MTLHeapTypeSparse. For residency sets, relax the temporary
//      `isSparse` check in rdna4_residency_set_impl.inc once Wave 3 lands.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_SPARSE_IMPL_INC
#define RDNA4_SPARSE_IMPL_INC

#include "rdna4_sparse.h"

// Lock pure-C constants and wire structs to the real Metal and Vulkan headers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
_Static_assert((int)MTLSparseTextureMappingModeMap == RDNA4SP_MODE_MAP &&
               (int)MTLSparseTextureMappingModeUnmap == RDNA4SP_MODE_UNMAP,
               "MTLSparseTextureMappingMode values");
_Static_assert((int)MTLSparseTextureRegionAlignmentModeOutward == RDNA4SP_ALIGN_OUTWARD &&
               (int)MTLSparseTextureRegionAlignmentModeInward == RDNA4SP_ALIGN_INWARD,
               "MTLSparseTextureRegionAlignmentMode values");
_Static_assert((int)MTLHeapTypeAutomatic == RDNA4SP_HEAP_AUTOMATIC &&
               (int)MTLHeapTypePlacement == RDNA4SP_HEAP_PLACEMENT,
               "MTLHeapType automatic/placement values");
#if defined(__MAC_11_0)
_Static_assert((int)MTLHeapTypeSparse == RDNA4SP_HEAP_SPARSE, "MTLHeapTypeSparse value");
_Static_assert(sizeof(MTLMapIndirectArguments) == sizeof(rdna4sp_map_indirect_args) &&
               offsetof(MTLMapIndirectArguments, regionOriginX) == offsetof(rdna4sp_map_indirect_args, region_origin_x) &&
               offsetof(MTLMapIndirectArguments, regionOriginY) == offsetof(rdna4sp_map_indirect_args, region_origin_y) &&
               offsetof(MTLMapIndirectArguments, regionOriginZ) == offsetof(rdna4sp_map_indirect_args, region_origin_z) &&
               offsetof(MTLMapIndirectArguments, regionSizeWidth) == offsetof(rdna4sp_map_indirect_args, region_size_width) &&
               offsetof(MTLMapIndirectArguments, regionSizeHeight) == offsetof(rdna4sp_map_indirect_args, region_size_height) &&
               offsetof(MTLMapIndirectArguments, regionSizeDepth) == offsetof(rdna4sp_map_indirect_args, region_size_depth) &&
               offsetof(MTLMapIndirectArguments, mipMapLevel) == offsetof(rdna4sp_map_indirect_args, mip_map_level) &&
               offsetof(MTLMapIndirectArguments, sliceId) == offsetof(rdna4sp_map_indirect_args, slice_id),
               "MTLMapIndirectArguments layout");
#endif
#if defined(__MAC_13_0)
_Static_assert((int)MTLSparsePageSize16 == RDNA4SP_PAGE_SIZE_16 &&
               (int)MTLSparsePageSize64 == RDNA4SP_PAGE_SIZE_64 &&
               (int)MTLSparsePageSize256 == RDNA4SP_PAGE_SIZE_256,
               "MTLSparsePageSize values");
#endif
#pragma clang diagnostic pop
_Static_assert((unsigned)VK_IMAGE_ASPECT_COLOR_BIT == RDNA4SP_ASP_COLOR &&
               (unsigned)VK_IMAGE_ASPECT_DEPTH_BIT == RDNA4SP_ASP_DEPTH &&
               (unsigned)VK_IMAGE_ASPECT_STENCIL_BIT == RDNA4SP_ASP_STENCIL,
               "VkImageAspectFlagBits sparse values");
_Static_assert((unsigned)MTLPixelFormatA8Unorm == RDNA4SP_PF_A8Unorm &&
               (unsigned)MTLPixelFormatR8Unorm == RDNA4SP_PF_R8Unorm &&
               (unsigned)MTLPixelFormatR8Snorm == RDNA4SP_PF_R8Snorm &&
               (unsigned)MTLPixelFormatR8Uint == RDNA4SP_PF_R8Uint &&
               (unsigned)MTLPixelFormatR8Sint == RDNA4SP_PF_R8Sint &&
               (unsigned)MTLPixelFormatR16Unorm == RDNA4SP_PF_R16Unorm &&
               (unsigned)MTLPixelFormatR16Snorm == RDNA4SP_PF_R16Snorm &&
               (unsigned)MTLPixelFormatR16Uint == RDNA4SP_PF_R16Uint &&
               (unsigned)MTLPixelFormatR16Sint == RDNA4SP_PF_R16Sint &&
               (unsigned)MTLPixelFormatR16Float == RDNA4SP_PF_R16Float &&
               (unsigned)MTLPixelFormatRG8Unorm == RDNA4SP_PF_RG8Unorm &&
               (unsigned)MTLPixelFormatRG8Snorm == RDNA4SP_PF_RG8Snorm &&
               (unsigned)MTLPixelFormatRG8Uint == RDNA4SP_PF_RG8Uint &&
               (unsigned)MTLPixelFormatRG8Sint == RDNA4SP_PF_RG8Sint &&
               (unsigned)MTLPixelFormatR32Uint == RDNA4SP_PF_R32Uint &&
               (unsigned)MTLPixelFormatR32Sint == RDNA4SP_PF_R32Sint &&
               (unsigned)MTLPixelFormatR32Float == RDNA4SP_PF_R32Float &&
               (unsigned)MTLPixelFormatRG16Unorm == RDNA4SP_PF_RG16Unorm &&
               (unsigned)MTLPixelFormatRG16Snorm == RDNA4SP_PF_RG16Snorm &&
               (unsigned)MTLPixelFormatRG16Uint == RDNA4SP_PF_RG16Uint &&
               (unsigned)MTLPixelFormatRG16Sint == RDNA4SP_PF_RG16Sint &&
               (unsigned)MTLPixelFormatRG16Float == RDNA4SP_PF_RG16Float &&
               (unsigned)MTLPixelFormatRGBA8Unorm == RDNA4SP_PF_RGBA8Unorm &&
               (unsigned)MTLPixelFormatRGBA8Unorm_sRGB == RDNA4SP_PF_RGBA8Unorm_sRGB &&
               (unsigned)MTLPixelFormatRGBA8Snorm == RDNA4SP_PF_RGBA8Snorm &&
               (unsigned)MTLPixelFormatRGBA8Uint == RDNA4SP_PF_RGBA8Uint &&
               (unsigned)MTLPixelFormatRGBA8Sint == RDNA4SP_PF_RGBA8Sint &&
               (unsigned)MTLPixelFormatBGRA8Unorm == RDNA4SP_PF_BGRA8Unorm &&
               (unsigned)MTLPixelFormatBGRA8Unorm_sRGB == RDNA4SP_PF_BGRA8Unorm_sRGB &&
               (unsigned)MTLPixelFormatRGB10A2Unorm == RDNA4SP_PF_RGB10A2Unorm &&
               (unsigned)MTLPixelFormatRGB10A2Uint == RDNA4SP_PF_RGB10A2Uint &&
               (unsigned)MTLPixelFormatRG11B10Float == RDNA4SP_PF_RG11B10Float &&
               (unsigned)MTLPixelFormatRGB9E5Float == RDNA4SP_PF_RGB9E5Float &&
               (unsigned)MTLPixelFormatBGR10A2Unorm == RDNA4SP_PF_BGR10A2Unorm &&
               (unsigned)MTLPixelFormatRG32Uint == RDNA4SP_PF_RG32Uint &&
               (unsigned)MTLPixelFormatRG32Sint == RDNA4SP_PF_RG32Sint &&
               (unsigned)MTLPixelFormatRG32Float == RDNA4SP_PF_RG32Float &&
               (unsigned)MTLPixelFormatRGBA16Unorm == RDNA4SP_PF_RGBA16Unorm &&
               (unsigned)MTLPixelFormatRGBA16Snorm == RDNA4SP_PF_RGBA16Snorm &&
               (unsigned)MTLPixelFormatRGBA16Uint == RDNA4SP_PF_RGBA16Uint &&
               (unsigned)MTLPixelFormatRGBA16Sint == RDNA4SP_PF_RGBA16Sint &&
               (unsigned)MTLPixelFormatRGBA16Float == RDNA4SP_PF_RGBA16Float &&
               (unsigned)MTLPixelFormatRGBA32Uint == RDNA4SP_PF_RGBA32Uint &&
               (unsigned)MTLPixelFormatRGBA32Sint == RDNA4SP_PF_RGBA32Sint &&
               (unsigned)MTLPixelFormatRGBA32Float == RDNA4SP_PF_RGBA32Float &&
               (unsigned)MTLPixelFormatBC1_RGBA == RDNA4SP_PF_BC1_RGBA &&
               (unsigned)MTLPixelFormatBC1_RGBA_sRGB == RDNA4SP_PF_BC1_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC2_RGBA == RDNA4SP_PF_BC2_RGBA &&
               (unsigned)MTLPixelFormatBC2_RGBA_sRGB == RDNA4SP_PF_BC2_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC3_RGBA == RDNA4SP_PF_BC3_RGBA &&
               (unsigned)MTLPixelFormatBC3_RGBA_sRGB == RDNA4SP_PF_BC3_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC4_RUnorm == RDNA4SP_PF_BC4_RUnorm &&
               (unsigned)MTLPixelFormatBC4_RSnorm == RDNA4SP_PF_BC4_RSnorm &&
               (unsigned)MTLPixelFormatBC5_RGUnorm == RDNA4SP_PF_BC5_RGUnorm &&
               (unsigned)MTLPixelFormatBC5_RGSnorm == RDNA4SP_PF_BC5_RGSnorm &&
               (unsigned)MTLPixelFormatBC6H_RGBFloat == RDNA4SP_PF_BC6H_RGBFloat &&
               (unsigned)MTLPixelFormatBC6H_RGBUfloat == RDNA4SP_PF_BC6H_RGBUfloat &&
               (unsigned)MTLPixelFormatBC7_RGBAUnorm == RDNA4SP_PF_BC7_RGBAUnorm &&
               (unsigned)MTLPixelFormatBC7_RGBAUnorm_sRGB == RDNA4SP_PF_BC7_RGBAUnorm_sRGB &&
               (unsigned)MTLPixelFormatDepth16Unorm == RDNA4SP_PF_Depth16Unorm &&
               (unsigned)MTLPixelFormatDepth32Float == RDNA4SP_PF_Depth32Float &&
               (unsigned)MTLPixelFormatStencil8 == RDNA4SP_PF_Stencil8,
               "MTLPixelFormat sparse table values");

// Lazy Vulkan sparse entry point resolution (mirrors rdna4_counters_impl.inc).
typedef struct {
    PFN_vkQueueBindSparse queueBindSparse;
    PFN_vkGetImageSparseMemoryRequirements getImageSparseMemReqs;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties getPhysSparseImageFormatProps;
} rdna4_sparse_vk_fn_t;

static const rdna4_sparse_vk_fn_t *rdna4_sparse_vk_fns(void) {
    if (!RDNA4R.ok || !RDNA4R.dev) return NULL;
    static rdna4_sparse_vk_fn_t fns;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr) {
            fns.queueBindSparse = (PFN_vkQueueBindSparse)vkGetDeviceProcAddr(RDNA4R.dev, "vkQueueBindSparse");
            fns.getImageSparseMemReqs = (PFN_vkGetImageSparseMemoryRequirements)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetImageSparseMemoryRequirements");
        }
        if (rdna4_gipa && RDNA4R.inst) {
            if (!fns.queueBindSparse)
                fns.queueBindSparse = (PFN_vkQueueBindSparse)rdna4_gipa(RDNA4R.inst, "vkQueueBindSparse");
            if (!fns.getImageSparseMemReqs)
                fns.getImageSparseMemReqs = (PFN_vkGetImageSparseMemoryRequirements)rdna4_gipa(RDNA4R.inst, "vkGetImageSparseMemoryRequirements");
            fns.getPhysSparseImageFormatProps = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)
                rdna4_gipa(RDNA4R.inst, "vkGetPhysicalDeviceSparseImageFormatProperties");
        }
    });
    return &fns;
}

// Execute vkQueueBindSparse on RDNA4R.q with direct VkFence completion + bounded wait.
// Never holds RDNA4R.qlock while waiting. Sets *unresolvedOut = YES on timeout/device-lost without destroying in-flight handles.
static BOOL rdna4_sparse_submit_bind(const VkBindSparseInfo *baseInfo, NSError **err, BOOL *unresolvedOut) {
    if (unresolvedOut) *unresolvedOut = NO;
    if (!baseInfo) { if (err) *err = rdna4_err(140, @"NULL VkBindSparseInfo"); return NO; }
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.q) {
        if (err) *err = rdna4_err(140, @"RADV device is not open for sparse bind");
        return NO;
    }
    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (!vk || !vk->queueBindSparse || !vkCreateFence || !vkDestroyFence || !vkWaitForFences) {
        if (err) *err = rdna4_err(140, @"Vulkan sparse bind or fence entry points unavailable");
        return NO;
    }
    if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        if (err) *err = rdna4_err(53, @"earlier GPU submission is unresolved; sparse bind disabled");
        return NO;
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VkResult vr = vkCreateFence(RDNA4R.dev, &fci, NULL, &fence);
    if (vr != VK_SUCCESS || fence == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(141, [NSString stringWithFormat:@"vkCreateFence(sparse) = %d", vr]);
        return NO;
    }

    VkBindSparseInfo bi = *baseInfo;
    bi.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bi.waitSemaphoreCount = 0u;
    bi.pWaitSemaphores = NULL;
    bi.signalSemaphoreCount = 0u;
    bi.pSignalSemaphores = NULL;

    BOOL bindSubmitted = NO, deviceLost = NO;
    rdna4_qlock();
    vr = (VkResult)atomic_load(&rdna4_submit_fault);
    if (vr == VK_SUCCESS) {
        vr = vk->queueBindSparse(RDNA4R.q, 1u, &bi, fence);
        if (vr == VK_SUCCESS) {
            bindSubmitted = YES;
        } else if (vr == VK_ERROR_DEVICE_LOST) {
            deviceLost = YES;
            atomic_store(&rdna4_submit_fault, (int)vr);
        }
    }
    pthread_mutex_unlock(&RDNA4R.qlock);

    if (!bindSubmitted) {
        if (deviceLost) {
            // Unresolved GPU state on VK_ERROR_DEVICE_LOST: do not destroy fence or release backing memory.
            if (unresolvedOut) *unresolvedOut = YES;
            if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"vkQueueBindSparse = %d", vr]);
        } else {
            vkDestroyFence(RDNA4R.dev, fence, NULL);
            if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"vkQueueBindSparse = %d", vr]);
        }
        return NO;
    }

    rdna4_fence_wait_ctx waitCtx = { .fence = fence, .cbid = 0u, .t1 = rdna4_now() };
    int slices = 0;
    vr = (VkResult)rdna4fw_wait_bounded(rdna4_fence_wait_slice, &waitCtx, 0,
                                        RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                                        rdna4_fence_wait_pending, &slices);
    if (vr != VK_SUCCESS) {
        atomic_store(&rdna4_submit_fault, (int)vr);
        if (unresolvedOut) *unresolvedOut = YES;
        if (err) *err = rdna4_err(54, [NSString stringWithFormat:@"sparse bind fence wait = %d (%d slice(s))", vr, slices]);
        return NO;
    }

    vkDestroyFence(RDNA4R.dev, fence, NULL);
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseBackingPool: manages 64 KiB physical tile chunks (<= 64 MiB non-composite VkDeviceMemory blocks)
// shared by both MTLHeapTypeSparse (RDNA4SparseHeap) and Private MTLHeapTypeAutomatic/Placement heaps.
// ---------------------------------------------------------------------------------------------------------------
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

@implementation RDNA4SparseBackingPool
- (instancetype)initWithCapacityBytes:(uint64_t)capacityBytes ownsAccounting:(BOOL)ownsAcct error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!rdna4sp_pool_init(&_pool, capacityBytes)) {
        if (err) *err = rdna4_err(143, [NSString stringWithFormat:@"invalid sparse tile pool capacity %llu", (unsigned long long)capacityBytes]);
        return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _capacityBytes = capacityBytes;
    _ownsAccounting = ownsAcct;
    return self;
}

- (void)dealloc {
    if (_muInit) pthread_mutex_lock(&_mu);
    if (RDNA4R.ok && RDNA4R.dev && vkFreeMemory) {
        for (size_t i = 0; i < _pool.n_chunks; i++) {
            VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)_pool.chunks[i].mem_handle;
            if (mem != VK_NULL_HANDLE) vkFreeMemory(RDNA4R.dev, mem, NULL);
        }
    }
    rdna4sp_pool_destroy(&_pool);
    if (_ownsAccounting && _capacityBytes) {
        atomic_fetch_sub(&rdna4_heap_reserved, _capacityBytes);
        atomic_fetch_sub(&rdna4_alloc_total, _capacityBytes);
    }
    if (_muInit) {
        pthread_mutex_unlock(&_mu);
        pthread_mutex_destroy(&_mu);
    }
}

- (rdna4sp_tile_pool *)pool { return &_pool; }
- (pthread_mutex_t *)mutex { return &_mu; }

- (NSUInteger)capacityBytes { return (NSUInteger)_capacityBytes; }
- (NSUInteger)usedBytes {
    pthread_mutex_lock(&_mu);
    uint64_t u = (uint64_t)_pool.used_tiles * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_mu);
    return (NSUInteger)u;
}
- (NSUInteger)availableBytes {
    pthread_mutex_lock(&_mu);
    uint64_t a = (uint64_t)rdna4sp_pool_total_available(&_pool) * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_mu);
    return (NSUInteger)a;
}

// Caller MUST hold _mu. Lazily allocates <= 64 MiB single non-composite DEVICE_LOCAL chunks up to capacity.
- (BOOL)ensureFreeTilesLocked:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err {
    if (rdna4sp_pool_free_backed(&_pool) >= need) return YES;
    if (rdna4sp_pool_total_available(&_pool) < need) {
        if (err) *err = rdna4_err(144, [NSString stringWithFormat:@"sparse heap out of tiles (need %llu, available %u)",
                                        (unsigned long long)need, rdna4sp_pool_total_available(&_pool)]);
        return NO;
    }
    if (!RDNA4R.ok || !RDNA4R.dev || !vkAllocateMemory || !vkFreeMemory) {
        if (err) *err = rdna4_err(144, @"RADV unavailable for sparse tile backing allocation");
        return NO;
    }
    const int mt = rdna4_find_mem(mtBits ? mtBits : UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(144, @"no device-local memory type for sparse tile backing");
        return NO;
    }
    while (rdna4sp_pool_free_backed(&_pool) < need) {
        const uint32_t unbacked = rdna4sp_pool_unbacked(&_pool);
        if (!unbacked) {
            if (err) *err = rdna4_err(144, @"sparse heap backing capacity exhausted");
            return NO;
        }
        uint64_t missing = need - rdna4sp_pool_free_backed(&_pool);
        // Grow in >= 4 MiB (64-tile) steps when remaining capacity allows, capped at 64 MiB per BO chunk.
        uint64_t chunkTiles = missing < 64u ? 64u : missing;
        if (chunkTiles > unbacked) chunkTiles = unbacked;
        const uint64_t maxChunkTiles = RDNA4SP_MAX_CHUNK_BYTES / RDNA4SP_TILE_BYTES;
        if (chunkTiles > maxChunkTiles) chunkTiles = maxChunkTiles;
        const uint64_t chunkBytes = chunkTiles * RDNA4SP_TILE_BYTES;
        VkMemoryAllocateInfo ma = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = chunkBytes,
            .memoryTypeIndex = (uint32_t)mt,
        };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult vr = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &mem);
        if (vr != VK_SUCCESS || mem == VK_NULL_HANDLE) {
            if (err) *err = rdna4_err(144, [NSString stringWithFormat:@"vkAllocateMemory(sparse chunk %llu B) = %d",
                                            (unsigned long long)chunkBytes, vr]);
            return NO;
        }
        if (!rdna4sp_pool_add_chunk(&_pool, (uint64_t)(uintptr_t)mem, chunkBytes)) {
            vkFreeMemory(RDNA4R.dev, mem, NULL);
            if (err) *err = rdna4_err(144, @"sparse tile pool rejected new chunk");
            return NO;
        }
    }
    return YES;
}

- (BOOL)ensureFreeTiles:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err {
    pthread_mutex_lock(&_mu);
    BOOL ok = [self ensureFreeTilesLocked:need memoryTypeBits:mtBits error:err];
    pthread_mutex_unlock(&_mu);
    return ok;
}
@end

@class RDNA4SparseTexture;
static RDNA4SparseBackingPool *rdna4_heap_sparse_pool(id heap, NSError **err);
static RDNA4SparseTexture *rdna4_sparse_texture_new(id dev, MTLTextureDescriptor *d, id heap, NSError **err);

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseHeap: MTLHeap implementation for MTLHeapTypeSparse (64 KiB GFX12 PRT tile pool, Private only).
// ---------------------------------------------------------------------------------------------------------------
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

@implementation RDNA4SparseHeap
+ (void)initialize {
    if (self == [RDNA4SparseHeap class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLHeap" };
            rdna4_add_protocols(self, n, 1, "RDNA4SparseHeap");
        });
    }
}
RDNA4_DNR(RDNA4SparseHeap)

- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || !d) {
        if (err) *err = rdna4_err(145, @"sparse heap requires a valid device and descriptor");
        return nil;
    }
    if ((NSInteger)d.type != RDNA4SP_HEAP_SPARSE) {
        if (err) *err = rdna4_err(145, @"RDNA4SparseHeap requires MTLHeapTypeSparse");
        return nil;
    }
    if (d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(145, @"sparse heaps require MTLStorageModePrivate");
        return nil;
    }
    if (!d.size) {
        if (err) *err = rdna4_err(145, @"sparse heap size must be non-zero");
        return nil;
    }
    if ((unsigned)d.hazardTrackingMode > 2u) {
        if (err) *err = rdna4_err(145, @"invalid hazardTrackingMode on sparse heap");
        return nil;
    }
    SEL spSel = sel_registerName("sparsePageSize");
    if ([d respondsToSelector:spSel]) {
        NSInteger psize = ((NSInteger (*)(id, SEL))objc_msgSend)(d, spSel);
        if (psize != RDNA4SP_PAGE_SIZE_DEFAULT && psize != RDNA4SP_PAGE_SIZE_16 &&
            psize != RDNA4SP_PAGE_SIZE_64) {
            if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported MTLSparsePageSize %ld (GFX12 PRT uses 64 KiB tiles)", (long)psize]);
            return nil;
        }
        if (psize == RDNA4SP_PAGE_SIZE_16)
            RDNA4_ONCE("sparse heap: MTLSparsePageSize16 default descriptor mapped to GFX12 64 KiB physical tiles");
    }
    if (!rdna4_radv_open(err)) return nil;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_sparse_texture_ok(&m3)) {
        if (err) *err = rdna4_err(145, @"sparse textures are unavailable on this RADV/kernel configuration");
        return nil;
    }
    uint64_t req = (uint64_t)d.size;
    if (req > UINT64_MAX - (RDNA4SP_TILE_BYTES - 1u)) {
        if (err) *err = rdna4_err(145, @"sparse heap size overflow");
        return nil;
    }
    uint64_t aligned = (req + RDNA4SP_TILE_BYTES - 1u) & ~(RDNA4SP_TILE_BYTES - 1u);
    rdna4dc_caps caps = [(GarnetDevice *)dev rdna4MemoryCaps];
    if (aligned > caps.allocation_budget || !rdna4_heap_reserve_capacity(aligned, caps.allocation_budget)) {
        if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"sparse heap size %llu exceeds VRAM budget %llu",
                                        (unsigned long long)aligned, (unsigned long long)caps.allocation_budget]);
        return nil;
    }
    atomic_fetch_add(&rdna4_alloc_total, aligned);
    RDNA4SparseBackingPool *pool = [[RDNA4SparseBackingPool alloc] initWithCapacityBytes:aligned ownsAccounting:YES error:err];
    if (!pool) {
        atomic_fetch_sub(&rdna4_heap_reserved, aligned);
        atomic_fetch_sub(&rdna4_alloc_total, aligned);
        return nil;
    }
    _dev = dev;
    _size = (NSUInteger)aligned;
    _cc = (unsigned)d.cpuCacheMode;
    _hz = d.hazardTrackingMode ? (unsigned)d.hazardTrackingMode : 1u;
    _purge = 2u;
    _pool = pool;
    RDNA4LOG("RDNA4SparseHeap %p: %lu bytes (%llu x 64 KiB tiles)", (__bridge void *)self,
             (unsigned long)_size, (unsigned long long)(aligned / RDNA4SP_TILE_BYTES));
    return self;
}

- (RDNA4SparseBackingPool *)rdna4SparseBackingPool { return _pool; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    NSError *e = nil;
    RDNA4SparseTexture *t = rdna4_sparse_texture_new(_dev, td, self, &e);
    if (!t) RDNA4LOG("sparseHeap newTextureWithDescriptor NIL: %s", e.localizedDescription.UTF8String);
    return t;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    (void)len; (void)o;
    RDNA4_ONCE("sparseHeap newBufferWithLength:options: sparse heaps back sparse textures only; nil");
    return nil;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off {
    (void)l; (void)o; (void)off; return nil;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off {
    (void)l; (void)o; (void)off; return nil;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off { (void)d; (void)off; return nil; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off { (void)d; (void)off; return nil; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al { (void)al; return [_pool availableBytes]; }
- (NSUInteger)setPurgeableState:(NSUInteger)st { NSUInteger p = _purge; if (st >= 2 && st <= 4) _purge = st; return p; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { return [_pool usedBytes]; }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)allocatedSize { return _size; }
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)_cc; }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)_hz; }
- (NSUInteger)resourceOptions { return ((NSUInteger)MTLStorageModePrivate << 4) | _cc | ((NSUInteger)_hz << 8); }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
- (MTLHeapType)type { return (MTLHeapType)RDNA4SP_HEAP_SPARSE; }
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)gpuAddress { return 0; }
- (uint64_t)memoryPoolId { return 0; }
- (NSString *)formattedDescription:(NSUInteger)indent {
    (void)indent;
    return [NSString stringWithFormat:@"<RDNA4SparseHeap: %p> size %lu used %lu label %@",
            (__bridge void *)self, (unsigned long)_size, (unsigned long)[self usedSize], _lbl];
}
@end

// Backing-pool helper for Private MTLHeapTypeAutomatic / MTLHeapTypePlacement heaps used for sparse tiles.
static const void *const rdna4_heap_sparse_pool_key = &rdna4_heap_sparse_pool_key;

@interface RDNA4Heap (RDNA4SparseBacking)
- (RDNA4SparseBackingPool *)rdna4SparseBackingPoolWithError:(NSError **)err;
@end

@implementation RDNA4Heap (RDNA4SparseBacking)
- (RDNA4SparseBackingPool *)rdna4SparseBackingPoolWithError:(NSError **)err {
    if ([self storageMode] != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(145, @"sparse tile backing requires a Private MTLHeap");
        return nil;
    }
    @synchronized (self) {
        RDNA4SparseBackingPool *existing = objc_getAssociatedObject(self, rdna4_heap_sparse_pool_key);
        if (existing) return existing;
        uint64_t cap = (uint64_t)[self size] & ~(RDNA4SP_TILE_BYTES - 1u);
        if (!cap) {
            if (err) *err = rdna4_err(145, @"heap size is smaller than one 64 KiB sparse tile");
            return nil;
        }
        // RDNA4Heap already accounted for _size in rdna4_heap_reserved / rdna4_alloc_total.
        RDNA4SparseBackingPool *created = [[RDNA4SparseBackingPool alloc] initWithCapacityBytes:cap ownsAccounting:NO error:err];
        if (created)
            objc_setAssociatedObject(self, rdna4_heap_sparse_pool_key, created, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return created;
    }
}
@end

static RDNA4SparseBackingPool *rdna4_heap_sparse_pool(id heap, NSError **err) {
    if (!heap) {
        if (err) *err = rdna4_err(145, @"sparse texture requires a backing MTLHeap");
        return nil;
    }
    if ([heap isKindOfClass:[RDNA4SparseHeap class]])
        return [(RDNA4SparseHeap *)heap rdna4SparseBackingPool];
    if ([heap isKindOfClass:[RDNA4Heap class]])
        return [(RDNA4Heap *)heap rdna4SparseBackingPoolWithError:err];
    if (err) *err = rdna4_err(145, @"unsupported heap class for sparse tile backing");
    return nil;
}

static __attribute__((unused)) id rdna4_sparse_heap_new(id dev, MTLHeapDescriptor *d, NSError **err) {
    NSError *localErr = nil;
    RDNA4SparseHeap *h = [[RDNA4SparseHeap alloc] initWithDevice:dev descriptor:d error:&localErr];
    if (!h) {
        RDNA4LOG("newHeapWithDescriptor(Sparse) NIL: %s", localErr.localizedDescription.UTF8String);
        if (err) *err = localErr;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseTexture: sparse subclass of RDNA4Texture (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | SPARSE_RESIDENCY_BIT).
// ---------------------------------------------------------------------------------------------------------------
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

@implementation RDNA4SparseTexture
- (instancetype)initSparseWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(id)heap error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || !d || !heap) {
        if (err) *err = rdna4_err(146, @"sparse texture requires device, descriptor, and backing heap");
        return nil;
    }
    if (d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(146, @"sparse textures require MTLStorageModePrivate");
        return nil;
    }
    if (!rdna4_radv_open(err)) return nil;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    const NSUInteger sc = d.sampleCount ? d.sampleCount : 1u;
    if (!rdna4sp_caps_support_texture(&m3, (uint64_t)d.textureType, (uint64_t)sc)) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"sparse texture type %lu samples %lu not supported by RADV/kernel",
                                        (unsigned long)d.textureType, (unsigned long)sc]);
        return nil;
    }
    if (!rdna4sp_fmt_lookup((uint64_t)d.pixelFormat)) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"pixel format %lu is not supported for sparse textures",
                                        (unsigned long)d.pixelFormat]);
        return nil;
    }
    RDNA4SparseBackingPool *pool = rdna4_heap_sparse_pool(heap, err);
    if (!pool) return nil;

    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (!vk || !vk->queueBindSparse || !vk->getImageSparseMemReqs) {
        if (err) *err = rdna4_err(146, @"Vulkan sparse image entry points are unavailable");
        return nil;
    }

    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, NO, &plan, err)) return nil;

    VkImageCreateInfo ic = plan.create;
    ic.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (m3.sparse_residency_aliased) ic.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;

    if (vkGetPhysicalDeviceImageFormatProperties) {
        VkImageFormatProperties ifp = {0};
        VkResult qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, ic.format, ic.imageType,
                                                               ic.tiling, ic.usage, ic.flags, &ifp);
        if (qr != VK_SUCCESS && plan.canRT && !(d.usage & MTLTextureUsageRenderTarget)) {
            ic.usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, ic.format, ic.imageType,
                                                          ic.tiling, ic.usage, ic.flags, &ifp);
        }
        if (qr != VK_SUCCESS) {
            if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"RADV refuses sparse image format %d type %d usage 0x%x (vk %d)",
                                            ic.format, ic.imageType, ic.usage, qr]);
            return nil;
        }
    }

    _dev = dev;
    _w = d.width;
    _h = d.height;
    _pf = d.pixelFormat;
    _fmt = plan.fmt;
    _usage = d.usage;
    _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | (NSUInteger)d.cpuCacheMode | ((NSUInteger)d.hazardTrackingMode << 8);
    _is1D = plan.is1D;
    _arr1D = plan.arr1D;
    _layers = plan.td.layersIvar;
    _tdk = plan.td.kind;
    _is3D = plan.is3D;
    _levels = plan.levels;
    _depth = plan.depth;
    _samples = plan.samples;
    _aspects = plan.aspects;
    _imageFlags = ic.flags;

    VkResult vr = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (vr != VK_SUCCESS || _img == VK_NULL_HANDLE) {
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, [NSString stringWithFormat:@"vkCreateImage(sparse) = %d", vr]);
        return nil;
    }

    VkMemoryRequirements mr = {0};
    vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
    _spMemTypeBits = mr.memoryTypeBits;

    uint32_t reqCount = 0u;
    vk->getImageSparseMemReqs(RDNA4R.dev, _img, &reqCount, NULL);
    if (reqCount == 0u || reqCount > 8u) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, [NSString stringWithFormat:@"vkGetImageSparseMemoryRequirements returned count %u", reqCount]);
        return nil;
    }
    VkSparseImageMemoryRequirements reqs[8];
    memset(reqs, 0, sizeof reqs);
    vk->getImageSparseMemReqs(RDNA4R.dev, _img, &reqCount, reqs);

    const uint32_t wantAspect = plan.isDS ? rdna4dp_view_aspect((unsigned)plan.aspects) : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkSparseImageMemoryRequirements *chosen = NULL;
    for (uint32_t i = 0; i < reqCount; i++) {
        if (reqs[i].formatProperties.aspectMask & wantAspect) { chosen = &reqs[i]; break; }
    }
    if (!chosen) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, @"no matching aspect in VkSparseImageMemoryRequirements");
        return nil;
    }

    rdna4sp_image_layout spLay;
    if (!rdna4sp_compute_layout((uint64_t)d.textureType, (uint64_t)d.pixelFormat,
                                (uint32_t)d.width, (uint32_t)d.height, (uint32_t)plan.depth,
                                (uint32_t)plan.levels, plan.td.layers, plan.td.samples,
                                (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT) != 0,
                                (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0,
                                &spLay) ||
        !rdna4sp_layout_adopt_vk_reqs(&spLay,
                                      chosen->formatProperties.imageGranularity.width,
                                      chosen->formatProperties.imageGranularity.height,
                                      chosen->formatProperties.imageGranularity.depth,
                                      wantAspect,
                                      chosen->imageMipTailFirstLod,
                                      (uint64_t)chosen->imageMipTailSize,
                                      (uint64_t)chosen->imageMipTailOffset,
                                      (uint64_t)chosen->imageMipTailStride,
                                      (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0) ||
        !rdna4sp_tex_init(&_spState, &spLay)) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, @"failed to initialize sparse texture residency table");
        return nil;
    }

    VkComponentMapping cm = rdna4_view_map(_fmt, _swz);
    _viewSwz = !rdna4_is_identity_map(&cm);
    VkImageViewCreateInfo vc = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = _img,
        .viewType = (VkImageViewType)plan.td.viewType,
        .format = _fmt->vk,
        .components = cm,
        .subresourceRange = { wantAspect, 0u, (uint32_t)_levels, 0u, plan.td.layers },
    };
    vr = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (vr != VK_SUCCESS || _view == VK_NULL_HANDLE) {
        _view = VK_NULL_HANDLE;
        rdna4sp_tex_destroy(&_spState);
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(148, [NSString stringWithFormat:@"vkCreateImageView(sparse) = %d", vr]);
        return nil;
    }

    pthread_mutex_init(&_spMu, NULL);
    _spMuInit = YES;
    _spHeap = heap;
    _spPool = pool;
    RDNA4LOGR("RDNA4SparseTexture %p: %lux%lux%lu pf %lu mips %lu layers %u tile %llux%llux%llu firstTail %u tailBytes %llu",
              (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, (unsigned long)_depth,
              (unsigned long)_pf, (unsigned long)_levels, plan.td.layers,
              (unsigned long long)_spState.layout.granularity.width,
              (unsigned long long)_spState.layout.granularity.height,
              (unsigned long long)_spState.layout.granularity.depth,
              _spState.layout.mip_tail_first_lod, (unsigned long long)_spState.layout.mip_tail_size);
    return self;
}

- (void)dealloc {
    // Destroy image views & sparse VkImage FIRST so RADV tears down the PRT VA range
    // before physical 64 KiB slots are returned to _spPool for reuse.
    if (RDNA4R.ok && RDNA4R.dev) {
        for (NSNumber *lvv in _lvViews.allValues)
            vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        [_lvViews removeAllObjects];
        if (_view != VK_NULL_HANDLE) { vkDestroyImageView(RDNA4R.dev, _view, NULL); _view = VK_NULL_HANDLE; }
        if (_img != VK_NULL_HANDLE) { vkDestroyImage(RDNA4R.dev, _img, NULL); _img = VK_NULL_HANDLE; }
    }
    if (_spMuInit) pthread_mutex_lock(&_spMu);
    if (_spPool) {
        pthread_mutex_t *pmu = [_spPool mutex];
        pthread_mutex_lock(pmu);
        rdna4sp_tex_release_all_slots(&_spState, [_spPool pool]);
        pthread_mutex_unlock(pmu);
    } else {
        rdna4sp_tex_release_all_slots(&_spState, NULL);
    }
    rdna4sp_tex_destroy(&_spState);
    if (_spMuInit) {
        pthread_mutex_unlock(&_spMu);
        pthread_mutex_destroy(&_spMu);
    }
}

- (BOOL)isSparse { return YES; }
- (NSUInteger)firstMipmapInTail { return (NSUInteger)_spState.layout.mip_tail_first_lod; }
- (NSUInteger)tailSizeInBytes { return (NSUInteger)_spState.layout.mip_tail_size; }
- (NSUInteger)allocatedSize {
    if (!_spMuInit) return 0;
    pthread_mutex_lock(&_spMu);
    uint64_t bytes = _spState.mapped_tile_count * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_spMu);
    return (NSUInteger)bytes;
}
- (id)heap { return _spHeap; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (const rdna4sp_texture_state *)rdna4SparseState { return &_spState; }

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice
            withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    (void)r; (void)lvl; (void)slice; (void)bytes; (void)bpr; (void)bpi;
    RDNA4LOG("replaceRegion: refused on Private sparse texture %p", (__bridge void *)self);
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
      fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    (void)dst; (void)bpr; (void)bpi; (void)r; (void)lvl; (void)slice;
    RDNA4LOG("getBytes: refused on Private sparse texture %p", (__bridge void *)self);
}

- (BOOL)rdna4ValidateRegions:(const rdna4sp_region *)regions mipLevels:(const uint64_t *)mips
                      slices:(const uint64_t *)slices count:(size_t)count
                        mode:(MTLSparseTextureMappingMode)mode error:(NSError **)err {
    const char *why = NULL;
    uint64_t needed = 0u;
    pthread_mutex_lock(&_spMu);
    int rc = rdna4sp_count_needed_tiles(&_spState, (uint32_t)mode, regions, mips, slices, count, &needed, &why);
    pthread_mutex_unlock(&_spMu);
    if (rc != RDNA4SP_OK) {
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "invalid sparse mapping region"]);
        return NO;
    }
    return YES;
}

- (BOOL)rdna4ExecuteSparseMode:(MTLSparseTextureMappingMode)mode
                       regions:(const rdna4sp_region *)regions
                     mipLevels:(const uint64_t *)mips
                        slices:(const uint64_t *)slices
                         count:(size_t)count
                         error:(NSError **)err {
    if (count == 0u) return YES;
    if (!_spPool || _img == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(149, @"sparse texture has no live Vulkan image or backing pool");
        return NO;
    }

    rdna4sp_bind_plan plan;
    rdna4sp_plan_init(&plan);
    const char *why = NULL;
    pthread_mutex_t *pmu = [_spPool mutex];

    pthread_mutex_lock(&_spMu);
    uint64_t need = 0u;
    int rc = rdna4sp_count_needed_tiles(&_spState, (uint32_t)mode, regions, mips, slices, count, &need, &why);
    if (rc != RDNA4SP_OK) {
        pthread_mutex_unlock(&_spMu);
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "invalid sparse region"]);
        return NO;
    }
    pthread_mutex_lock(pmu);
    if (mode == MTLSparseTextureMappingModeMap && need > 0u &&
        ![_spPool ensureFreeTilesLocked:need memoryTypeBits:_spMemTypeBits error:err]) {
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        return NO;
    }
    rc = rdna4sp_plan_prepare(&plan, &_spState, [_spPool pool], (uint32_t)mode,
                              regions, mips, slices, count, &why);
    if (rc != RDNA4SP_OK) {
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        rdna4sp_plan_destroy(&plan);
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "sparse bind planning failed"]);
        return NO;
    }
    if (plan.n_image_ops == 0u && plan.n_opaque_ops == 0u) {
        rdna4sp_plan_commit(&plan, [_spPool pool]);
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        rdna4sp_plan_destroy(&plan);
        return YES;
    }
    // Release pool mutex before submitting/waiting on GPU; _spMu stays held to serialize per-texture binds.
    pthread_mutex_unlock(pmu);

    VkSparseImageMemoryBind *vkImgBinds = NULL;
    VkSparseMemoryBind *vkOpaqueBinds = NULL;
    BOOL ok = YES;
    if (plan.n_image_ops > 0u) {
        vkImgBinds = (VkSparseImageMemoryBind *)malloc(plan.n_image_ops * sizeof(*vkImgBinds));
        if (!vkImgBinds) ok = NO;
        else {
            for (size_t i = 0; i < plan.n_image_ops; i++)
                vkImgBinds[i] = rdna4sp_to_vk_image_bind(&plan.image_ops[i]);
        }
    }
    if (ok && plan.n_opaque_ops > 0u) {
        vkOpaqueBinds = (VkSparseMemoryBind *)malloc(plan.n_opaque_ops * sizeof(*vkOpaqueBinds));
        if (!vkOpaqueBinds) ok = NO;
        else {
            for (size_t i = 0; i < plan.n_opaque_ops; i++)
                vkOpaqueBinds[i] = rdna4sp_to_vk_opaque_bind(&plan.opaque_ops[i]);
        }
    }

    BOOL unresolved = NO;
    if (!ok) {
        if (err) *err = rdna4_err(149, @"out of host memory building VkSparseImageMemoryBind arrays");
    } else {
        VkSparseImageMemoryBindInfo imgBindInfo = {
            .image = _img,
            .bindCount = (uint32_t)plan.n_image_ops,
            .pBinds = vkImgBinds,
        };
        VkSparseImageOpaqueMemoryBindInfo opaqueBindInfo = {
            .image = _img,
            .bindCount = (uint32_t)plan.n_opaque_ops,
            .pBinds = vkOpaqueBinds,
        };
        VkBindSparseInfo bi = rdna4sp_make_bind_sparse_info(
            plan.n_image_ops ? &imgBindInfo : NULL,
            plan.n_opaque_ops ? &opaqueBindInfo : NULL,
            0u, NULL, 0u, NULL);
        ok = rdna4_sparse_submit_bind(&bi, err, &unresolved);
    }

    free(vkImgBinds);
    free(vkOpaqueBinds);

    pthread_mutex_lock(pmu);
    if (ok) {
        rdna4sp_plan_commit(&plan, [_spPool pool]);
    } else if (!unresolved) {
        rdna4sp_plan_rollback(&plan, &_spState, [_spPool pool]);
    } else {
        // Keep texture & pool retained on unresolved GPU fault so backing memory cannot be freed under flight.
        (void)CFBridgingRetain(self);
        (void)CFBridgingRetain(_spPool);
        plan.active = 0;
    }
    pthread_mutex_unlock(pmu);
    pthread_mutex_unlock(&_spMu);

    rdna4sp_plan_destroy(&plan);
    return ok;
}
@end

static RDNA4SparseTexture *rdna4_sparse_texture_new(id dev, MTLTextureDescriptor *d, id heap, NSError **err) {
    return [[RDNA4SparseTexture alloc] initSparseWithDevice:dev descriptor:d heap:heap error:err];
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4ResourceStateEncoder: MTLResourceStateCommandEncoder implementation backed by ordered vkQueueBindSparse.
// ---------------------------------------------------------------------------------------------------------------
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

@implementation RDNA4SparsePendingOp
- (void)dealloc {
    free(regions);
    free(mips);
    free(slices);
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

@implementation RDNA4ResourceStateEncoder
+ (void)initialize {
    if (self == [RDNA4ResourceStateEncoder class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLResourceStateCommandEncoder", "MTLCommandEncoder" };
            rdna4_add_protocols(self, n, 2, "RDNA4ResourceStateEncoder");
        });
    }
}
RDNA4_DNR(RDNA4ResourceStateEncoder)

- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)desc {
    (void)desc;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    _pendingOps = [NSMutableArray array];
    RDNA4LOGR("RDNA4ResourceStateEncoder %p", (__bridge void *)self);
    return self;
}

- (id)device { return [_cb device]; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 4; }
- (void)insertDebugSignpost:(NSString *)s { (void)s; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}

// Flush queued sparse mapping ops as one ordered command-buffer host action so preceding GPU
// segments complete before vkQueueBindSparse and following segments observe the new PTEs.
- (void)rdna4FlushPendingOps {
    if (!_pendingOps.count || [_cb rdna4Error]) {
        [_pendingOps removeAllObjects];
        return;
    }
    NSArray<RDNA4SparsePendingOp *> *batch = [_pendingOps copy];
    [_pendingOps removeAllObjects];
    NSError *queueErr = nil;
    if (![_cb rdna4EncodeOrderedHostAction:^BOOL(NSError **actionErr) {
        for (RDNA4SparsePendingOp *op in batch) {
            if (!op->indirect) {
                if (![op->tex rdna4ExecuteSparseMode:op->mode regions:op->regions
                                           mipLevels:op->mips slices:op->slices
                                               count:op->count error:actionErr])
                    return NO;
                continue;
            }
            rdna4sp_map_indirect_args rawArgs;
            memset(&rawArgs, 0, sizeof rawArgs);
            if ([op->indirectBuf contents]) {
                memcpy(&rawArgs, (const uint8_t *)[op->indirectBuf contents] + op->indirectOffset, sizeof rawArgs);
            } else {
                VkDeviceSize srcOff = 0;
                if (__builtin_add_overflow([op->indirectBuf rdna4VkOffset], (VkDeviceSize)op->indirectOffset, &srcOff)) {
                    if (actionErr) *actionErr = rdna4_err(150, @"sparse indirect buffer Vulkan offset overflow");
                    return NO;
                }
                RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:[op->indirectBuf device]
                                                                length:sizeof(rawArgs)
                                                               options:MTLResourceStorageModeShared
                                                                 error:actionErr];
                if (!stg || ![stg contents]) {
                    if (actionErr && !*actionErr)
                        *actionErr = rdna4_err(150, @"sparse indirect readback staging buffer allocation failed");
                    return NO;
                }
                BOOL unresolved = NO;
                BOOL copyOk = rdna4_oneshot(actionErr, ^(VkCommandBuffer cmd) {
                    VkBufferCopy bc = {
                        .srcOffset = srcOff,
                        .dstOffset = [stg rdna4VkOffset],
                        .size = sizeof(rawArgs),
                    };
                    vkCmdCopyBuffer(cmd, [op->indirectBuf vkBuffer], [stg vkBuffer], 1u, &bc);
                    VkMemoryBarrier mb = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                    };
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                         0u, 1u, &mb, 0u, NULL, 0u, NULL);
                }, &unresolved);
                if (!copyOk) {
                    if (unresolved) {
                        (void)CFBridgingRetain(stg);
                        (void)CFBridgingRetain(op->indirectBuf);
                    }
                    return NO;
                }
                memcpy(&rawArgs, [stg contents], sizeof rawArgs);
            }
            rdna4sp_region reg;
            uint64_t mip = 0u, slice = 0u;
            const char *why = NULL;
            int rc = rdna4sp_validate_indirect_args([op->tex rdna4SparseState], (uint32_t)op->mode,
                                                    &rawArgs, &reg, &mip, &slice, &why);
            if (rc != RDNA4SP_OK) {
                if (actionErr) *actionErr = rdna4_err(150, [NSString stringWithUTF8String:why ?: "invalid indirect sparse mapping arguments"]);
                return NO;
            }
            if (![op->tex rdna4ExecuteSparseMode:op->mode regions:&reg mipLevels:&mip slices:&slice count:1u error:actionErr])
                return NO;
        }
        return YES;
    } error:&queueErr]) {
        if (![_cb rdna4Error])
            [_cb rdna4Fail:queueErr.localizedDescription ?: @"failed to enqueue sparse bind action"];
    }
}

- (void)updateTextureMappings:(id<MTLTexture>)texture
                         mode:(MTLSparseTextureMappingMode)mode
                      regions:(const MTLRegion *)regions
                    mipLevels:(const NSUInteger *)mipLevels
                       slices:(const NSUInteger *)slices
                   numRegions:(NSUInteger)numRegions {
    if (_ended) { [_cb rdna4Fail:@"updateTextureMappings after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![texture isKindOfClass:[RDNA4SparseTexture class]] || [(RDNA4Texture *)texture device] != [_cb device] || ![texture isSparse]) {
        [_cb rdna4Fail:@"updateTextureMappings requires a sparse texture from this device"];
        return;
    }
    if (mode != MTLSparseTextureMappingModeMap && mode != MTLSparseTextureMappingModeUnmap) {
        [_cb rdna4Fail:@"updateTextureMappings: invalid MTLSparseTextureMappingMode"];
        return;
    }
    if (numRegions == 0u) return;
    if (numRegions > RDNA4SP_MAX_BATCH_REGIONS) {
        [_cb rdna4Fail:@"updateTextureMappings: numRegions exceeds maximum batch limit"];
        return;
    }
    if (!regions || !mipLevels || !slices) {
        [_cb rdna4Fail:@"updateTextureMappings: NULL regions, mipLevels, or slices pointer"];
        return;
    }

    RDNA4SparseTexture *stex = (RDNA4SparseTexture *)texture;
    RDNA4SparsePendingOp *op = [RDNA4SparsePendingOp new];
    op->tex = stex;
    op->mode = mode;
    op->count = (size_t)numRegions;
    op->regions = (rdna4sp_region *)malloc(op->count * sizeof(*op->regions));
    op->mips = (uint64_t *)malloc(op->count * sizeof(*op->mips));
    op->slices = (uint64_t *)malloc(op->count * sizeof(*op->slices));
    if (!op->regions || !op->mips || !op->slices) {
        [_cb rdna4Fail:@"updateTextureMappings: out of host memory"];
        return;
    }
    for (size_t i = 0; i < op->count; i++) {
        op->regions[i] = (rdna4sp_region){
            .origin = { (uint64_t)regions[i].origin.x, (uint64_t)regions[i].origin.y, (uint64_t)regions[i].origin.z },
            .size   = { (uint64_t)regions[i].size.width, (uint64_t)regions[i].size.height, (uint64_t)regions[i].size.depth },
        };
        op->mips[i] = (uint64_t)mipLevels[i];
        op->slices[i] = (uint64_t)slices[i];
    }

    NSError *valErr = nil;
    if (![stex rdna4ValidateRegions:op->regions mipLevels:op->mips slices:op->slices count:op->count mode:mode error:&valErr]) {
        [_cb rdna4Fail:valErr.localizedDescription ?: @"updateTextureMappings: region validation failed"];
        return;
    }
    [_cb rdna4RetainEncoded:stex];
    if ([stex heap]) [_cb rdna4RetainEncoded:[stex heap]];
    [_pendingOps addObject:op];
}

- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
                      region:(MTLRegion)region
                    mipLevel:(NSUInteger)mipLevel
                       slice:(NSUInteger)slice {
    [self updateTextureMappings:texture mode:mode regions:&region mipLevels:&mipLevel slices:&slice numRegions:1u];
}

- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
              indirectBuffer:(id<MTLBuffer>)indirectBuffer
        indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (_ended) { [_cb rdna4Fail:@"updateTextureMapping(indirect) after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![texture isKindOfClass:[RDNA4SparseTexture class]] || [(RDNA4Texture *)texture device] != [_cb device] || ![texture isSparse]) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect) requires a sparse texture from this device"];
        return;
    }
    if (mode != MTLSparseTextureMappingModeMap && mode != MTLSparseTextureMappingModeUnmap) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect): invalid MTLSparseTextureMappingMode"];
        return;
    }
    if (![indirectBuffer isKindOfClass:[RDNA4Buffer class]] ||
        [(RDNA4Buffer *)indirectBuffer device] != [_cb device] ||
        [(RDNA4Buffer *)indirectBuffer isAliasable]) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect) requires a non-aliasable RDNA4Buffer from this device"];
        return;
    }
    RDNA4Buffer *buf = (RDNA4Buffer *)indirectBuffer;
    if ((indirectBufferOffset & 3u) != 0u ||
        !rdna4_buffer_range([buf length], indirectBufferOffset, sizeof(rdna4sp_map_indirect_args))) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect): indirectBufferOffset misaligned or out of bounds"];
        return;
    }
    RDNA4SparseTexture *stex = (RDNA4SparseTexture *)texture;
    RDNA4SparsePendingOp *op = [RDNA4SparsePendingOp new];
    op->tex = stex;
    op->mode = mode;
    op->indirect = YES;
    op->indirectBuf = buf;
    op->indirectOffset = indirectBufferOffset;
    [_cb rdna4RetainEncoded:stex];
    if ([stex heap]) [_cb rdna4RetainEncoded:[stex heap]];
    [_cb rdna4RetainEncoded:buf];
    [_pendingOps addObject:op];
}

- (void)updateFence:(id<MTLFence>)fence {
    if (_ended) { [_cb rdna4Fail:@"resourceState updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [self rdna4FlushPendingOps];
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:fence];
}

- (void)waitForFence:(id<MTLFence>)fence {
    if (_ended) { [_cb rdna4Fail:@"resourceState waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [self rdna4FlushPendingOps];
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:fence];
}

- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    [self rdna4FlushPendingOps];
    RDNA4LOGR("RDNA4ResourceStateEncoder endEncoding");
    [super endEncoding];
}
@end

static __attribute__((unused)) id rdna4_cb_new_resource_state_encoder(RDNA4CommandBuffer *cb, id desc) {
    if (!cb) return nil;
    RDNA4LOGR("cb resourceStateCommandEncoder");
    return [[RDNA4ResourceStateEncoder alloc] initWithCommandBuffer:cb descriptor:desc];
}

// ---------------------------------------------------------------------------------------------------------------
// MTLDevice sparse query & coordinate conversion helpers (called by GarnetDevice in Wave 3).
// Never opens RADV unexpectedly and NEVER claims MTLGPUFamilyApple6.
// ---------------------------------------------------------------------------------------------------------------
static __attribute__((unused)) NSUInteger rdna4_device_sparse_tile_size_in_bytes(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return (NSUInteger)rdna4sp_tile_size_in_bytes(&m3);
}

static __attribute__((unused)) NSUInteger rdna4_device_sparse_tile_size_for_page_size(NSInteger sparsePageSize) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return (NSUInteger)rdna4sp_tile_size_in_bytes_for_page_size(&m3, (int64_t)sparsePageSize);
}

static __attribute__((unused)) MTLSize rdna4_device_sparse_tile_size(MTLTextureType textureType,
                                                                     MTLPixelFormat pixelFormat,
                                                                     NSUInteger sampleCount) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    rdna4sp_size sz = { 0u, 0u, 0u };
    if (!rdna4sp_tile_size_gated(&m3, (uint64_t)textureType, (uint64_t)pixelFormat,
                                 (uint64_t)sampleCount, &sz))
        return MTLSizeMake(0, 0, 0);

    const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup((uint64_t)pixelFormat);
    const RDNA4Fmt *fmt = rdna4_fmt(pixelFormat);
    if (!fmt) fmt = rdna4_fmt_ds(pixelFormat);
    if (!fmt || !fi) return MTLSizeMake(0, 0, 0);

    // Cross-check against RADV's authoritative sparse image granularity when RADV is open.
    // Never fall back to the static table if RADV reports zero matching sparse format properties.
    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (RDNA4R.ok && RDNA4R.pd && vk && vk->getPhysSparseImageFormatProps) {
        VkImageType vit = textureType == MTLTextureType3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        uint32_t propCount = 0u;
        vk->getPhysSparseImageFormatProps(RDNA4R.pd, fmt->vk, vit, (VkSampleCountFlagBits)sampleCount,
                                          usage, VK_IMAGE_TILING_OPTIMAL, &propCount, NULL);
        if (propCount == 0u) return MTLSizeMake(0, 0, 0);
        uint32_t cap = propCount > 8u ? 8u : propCount;
        VkSparseImageFormatProperties props[8];
        memset(props, 0, sizeof props);
        vk->getPhysSparseImageFormatProps(RDNA4R.pd, fmt->vk, vit, (VkSampleCountFlagBits)sampleCount,
                                          usage, VK_IMAGE_TILING_OPTIMAL, &cap, props);
        for (uint32_t i = 0u; i < cap; i++) {
            if ((props[i].aspectMask & fi->aspect_mask) == fi->aspect_mask &&
                props[i].imageGranularity.width && props[i].imageGranularity.height &&
                props[i].imageGranularity.depth) {
                return MTLSizeMake(props[i].imageGranularity.width,
                                   props[i].imageGranularity.height,
                                   props[i].imageGranularity.depth);
            }
        }
        return MTLSizeMake(0, 0, 0);
    }
    return MTLSizeMake((NSUInteger)sz.width, (NSUInteger)sz.height, (NSUInteger)sz.depth);
}

static __attribute__((unused)) MTLSize rdna4_device_sparse_tile_size_with_page_size(MTLTextureType textureType,
                                                                                    MTLPixelFormat pixelFormat,
                                                                                    NSUInteger sampleCount,
                                                                                    NSInteger sparsePageSize) {
    if (sparsePageSize != RDNA4SP_PAGE_SIZE_64) return MTLSizeMake(0, 0, 0);
    return rdna4_device_sparse_tile_size(textureType, pixelFormat, sampleCount);
}

static __attribute__((unused)) void rdna4_device_convert_sparse_pixel_regions(const MTLRegion *pixelRegions,
                                                                              MTLRegion *tileRegions,
                                                                              MTLSize tileSize,
                                                                              MTLSparseTextureRegionAlignmentMode mode,
                                                                              NSUInteger numRegions) {
    if (!numRegions || !pixelRegions || !tileRegions) return;
    rdna4sp_size ts = { (uint64_t)tileSize.width, (uint64_t)tileSize.height, (uint64_t)tileSize.depth };
    for (NSUInteger i = 0; i < numRegions; i++) {
        rdna4sp_region inR = {
            .origin = { (uint64_t)pixelRegions[i].origin.x, (uint64_t)pixelRegions[i].origin.y, (uint64_t)pixelRegions[i].origin.z },
            .size   = { (uint64_t)pixelRegions[i].size.width, (uint64_t)pixelRegions[i].size.height, (uint64_t)pixelRegions[i].size.depth },
        };
        rdna4sp_region outR = { {0, 0, 0}, {0, 0, 0} };
        if (rdna4sp_convert_pixel_regions_to_tiles(&inR, &outR, ts, (uint32_t)mode, 1u)) {
            tileRegions[i] = MTLRegionMake3D((NSUInteger)outR.origin.x, (NSUInteger)outR.origin.y, (NSUInteger)outR.origin.z,
                                             (NSUInteger)outR.size.width, (NSUInteger)outR.size.height, (NSUInteger)outR.size.depth);
        } else {
            tileRegions[i] = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
        }
    }
}

static __attribute__((unused)) void rdna4_device_convert_sparse_tile_regions(const MTLRegion *tileRegions,
                                                                             MTLRegion *pixelRegions,
                                                                             MTLSize tileSize,
                                                                             NSUInteger numRegions) {
    if (!numRegions || !tileRegions || !pixelRegions) return;
    rdna4sp_size ts = { (uint64_t)tileSize.width, (uint64_t)tileSize.height, (uint64_t)tileSize.depth };
    for (NSUInteger i = 0; i < numRegions; i++) {
        rdna4sp_region inR = {
            .origin = { (uint64_t)tileRegions[i].origin.x, (uint64_t)tileRegions[i].origin.y, (uint64_t)tileRegions[i].origin.z },
            .size   = { (uint64_t)tileRegions[i].size.width, (uint64_t)tileRegions[i].size.height, (uint64_t)tileRegions[i].size.depth },
        };
        rdna4sp_region outR = { {0, 0, 0}, {0, 0, 0} };
        if (rdna4sp_convert_tile_regions_to_pixels(&inR, &outR, ts, 1u)) {
            pixelRegions[i] = MTLRegionMake3D((NSUInteger)outR.origin.x, (NSUInteger)outR.origin.y, (NSUInteger)outR.origin.z,
                                              (NSUInteger)outR.size.width, (NSUInteger)outR.size.height, (NSUInteger)outR.size.depth);
        } else {
            pixelRegions[i] = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
        }
    }
}

#endif /* RDNA4_SPARSE_IMPL_INC */
