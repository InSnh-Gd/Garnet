// ---------------------------------------------------------------------------------------------------------------
// bundle 9: rdna4_texdesc.h carries the numeric Metal / Vulkan values (it includes neither header); these asserts tie them to the real enums.
_Static_assert((int)MTLTextureType1D == RDNA4TD_MTL_1D && (int)MTLTextureType1DArray == RDNA4TD_MTL_1DARRAY && (int)MTLTextureType2D == RDNA4TD_MTL_2D && (int)MTLTextureType2DArray == RDNA4TD_MTL_2DARRAY &&
               (int)MTLTextureType2DMultisample == RDNA4TD_MTL_2DMS && (int)MTLTextureTypeCube == RDNA4TD_MTL_CUBE && (int)MTLTextureTypeCubeArray == RDNA4TD_MTL_CUBEARRAY && (int)MTLTextureType3D == RDNA4TD_MTL_3D, "MTLTextureType values");
_Static_assert((int)MTLTextureUsageShaderWrite == RDNA4TD_USE_SHADERWRITE && (int)MTLTextureUsageRenderTarget == RDNA4TD_USE_RENDERTARGET, "MTLTextureUsage values");
_Static_assert((int)VK_IMAGE_TYPE_1D == RDNA4TD_VK_IMAGE_1D && (int)VK_IMAGE_TYPE_2D == RDNA4TD_VK_IMAGE_2D && (int)VK_IMAGE_TYPE_3D == RDNA4TD_VK_IMAGE_3D, "VkImageType values");
_Static_assert((int)VK_IMAGE_VIEW_TYPE_1D == RDNA4TD_VK_VIEW_1D && (int)VK_IMAGE_VIEW_TYPE_2D == RDNA4TD_VK_VIEW_2D && (int)VK_IMAGE_VIEW_TYPE_3D == RDNA4TD_VK_VIEW_3D && (int)VK_IMAGE_VIEW_TYPE_CUBE == RDNA4TD_VK_VIEW_CUBE &&
               (int)VK_IMAGE_VIEW_TYPE_1D_ARRAY == RDNA4TD_VK_VIEW_1D_ARRAY && (int)VK_IMAGE_VIEW_TYPE_2D_ARRAY == RDNA4TD_VK_VIEW_2D_ARRAY &&
               (int)VK_IMAGE_VIEW_TYPE_CUBE_ARRAY == RDNA4TD_VK_VIEW_CUBE_ARRAY, "VkImageViewType values");
_Static_assert((unsigned)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE, "VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT");
_Static_assert((int)MTLTextureTypeTextureBuffer == RDNA4TD_MTL_BUFFER, "MTLTextureTypeTextureBuffer value");
// RDNA4Texture: VkImage (optimal, DEVICE_LOCAL), always TRANSFER_SRC|DST (10c).
// ---------------------------------------------------------------------------------------------------------------
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
    BOOL _led;   // build 16 (P5 Step 0): entered in the live-import ledger (removed in -dealloc)
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
@end

@implementation RDNA4Buffer (Textures)
// Linear textures over this buffer (public -newTextureWithDescriptor:offset:bytesPerRow:, SPI -newLinearTexture..., -newTiledTexture...): see RDNA4Texture initWithDevice:descriptor:buffer:
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    NSError *e = nil; id t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:d buffer:self offset:off bytesPerRow:bpr error:&e];
    if (!t) RDNA4LOG("buffer newTextureWithDescriptor:offset:bytesPerRow: NIL: %s; pf %lu type %lu usage 0x%lx storage %lu %lux%lu", e.localizedDescription.UTF8String,
                   (unsigned long)d.pixelFormat, (unsigned long)d.textureType, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height);
    if (t) [[self rdna4ResourceHeap] rdna4TrackResource:t];
    return t;
}
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi { (void)bpi; return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
- (id)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr { return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
@end

static void rdna4_image_transition(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout oldLayout, VkImageLayout newLayout) {
    if (oldLayout == newLayout) return;
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = oldLayout, .newLayout = newLayout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = [t vkImage], .subresourceRange = { rdna4dp_barrier_aspect([t rdna4Aspects]), 0, [t rdna4Levels], 0, [t rdna4ImageLayers] } };   // bundle 10: a depth/stencil image's barrier names its own aspects
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    [t setLayout:newLayout];
}
static VkImageLayout rdna4_use_layout(RDNA4Texture *texture, VkImageLayout preferred) {
    return [texture rdna4IOSLinear] ? VK_IMAGE_LAYOUT_GENERAL : preferred;
}
// Layout transition recorded into cmd; the texture object tracks the layout at RECORD time (command buffers execute in order).
static void rdna4_tex_to(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout nl) {
    if ([t rdna4IsTexel]) {
        RDNA4Buffer *buffer = [t rdna4TexelBuffer];
        VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = [buffer vkBuffer], .offset = [buffer rdna4VkOffset] + [t bufferOffset],
            .size = [t width] * [t bytesPerPixel]};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, NULL, 1, &barrier, 0, NULL);
        [t setLayout:nl];
        return;
    }
    if (![t rdna4IOSLinear]) rdna4_image_transition(cmd, t, [t layout], nl);
}
static BOOL rdna4_region_ok(RDNA4Texture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const char *what) {
    if (level >= [t mipmapLevelCount] || slice >= [t rdna4Layers]) { RDNA4LOG("%s: mip level %lu / slice %lu: the texture has %lu level(s) and %u slice(s); refused", what, (unsigned long)level, (unsigned long)slice, (unsigned long)[t mipmapLevelCount], [t rdna4Layers]); return NO; }
    NSUInteger lw = MAX((NSUInteger)1, t.width >> level), lh = MAX((NSUInteger)1, t.height >> level), ld = MAX((NSUInteger)1, t.depth >> level);   // depth is 1 for everything but 3D
    if (!r.size.width || !r.size.height || !r.size.depth || r.origin.x > lw || r.size.width > lw - r.origin.x ||
        r.origin.y > lh || r.size.height > lh - r.origin.y || r.origin.z > ld || r.size.depth > ld - r.origin.z) {
        RDNA4LOG("%s: region origin %lu,%lu,%lu size %lux%lux%lu outside the %lux%lux%lu level %lu; refused", what, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
               (unsigned long)r.origin.z, (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)r.size.depth, (unsigned long)lw, (unsigned long)lh, (unsigned long)ld, (unsigned long)level); return NO; }
    if ([t isCompressed] &&
        ((r.origin.x & 3) || (r.origin.y & 3) ||
         ((r.size.width & 3) && r.origin.x + r.size.width != lw) ||
         ((r.size.height & 3) && r.origin.y + r.size.height != lh))) {
        RDNA4LOG("%s: block-compressed region origin/size (%lu,%lu %lux%lu) is not 4x4 block aligned on %lux%lu level; refused", what, (unsigned long)r.origin.x, (unsigned long)r.origin.y, (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)lw, (unsigned long)lh);
        return NO;
    }
    return YES;
}

typedef struct {
    const RDNA4Fmt *fmt;
    rdna4td_t td;
    BOOL isDS, is1D, is3D, arr1D, canRT, wantSt;
    NSUInteger levels, depth, aspects, samples;
    VkImageCreateInfo create;
} rdna4_heap_image_plan;

// Constructors and SizeAndAlign queries share the same Vulkan image description.
static BOOL rdna4_heap_image_plan_make(MTLTextureDescriptor *d, BOOL aliasable, rdna4_heap_image_plan *p, NSError **err) {
    if (!d || !p) return NO;
    memset(p, 0, sizeof *p);
    if (!d.width || !d.height || d.width > UINT32_MAX || d.height > UINT32_MAX ||
        d.depth > UINT32_MAX || d.mipmapLevelCount > UINT32_MAX ||
        d.arrayLength > UINT32_MAX || d.sampleCount > UINT32_MAX) {
        if (err) *err = rdna4_err(31, @"texture geometry is zero or exceeds Vulkan's 32-bit fields");
        return NO;
    }
    p->fmt = rdna4_fmt(d.pixelFormat);
    if (!p->fmt && rdna4dp_is_dsformat((unsigned long)d.pixelFormat)) {
        rdna4dp_fmt_t df;
        if (!rdna4dp_fmt((unsigned long)d.pixelFormat, &df)) {
            if (err) *err = rdna4_err(30, [NSString stringWithFormat:@"pixel format %lu: %s", (unsigned long)d.pixelFormat, df.why]);
            return NO;
        }
        p->fmt = rdna4_fmt_ds(d.pixelFormat);
        p->isDS = p->fmt != NULL;
        p->aspects = df.aspects;
    }
    if (!p->fmt) {
        if (err) *err = rdna4_err(30, [NSString stringWithFormat:@"pixel format %lu not supported", (unsigned long)d.pixelFormat]);
        return NO;
    }
    if (!rdna4td_map((unsigned long)d.textureType, d.width, d.height, d.depth, d.arrayLength, d.sampleCount, d.mipmapLevelCount, (unsigned long)d.usage, &p->td)) {
        if (err) *err = rdna4_err(31, [NSString stringWithFormat:@"%s (type %lu %lux%lu depth %lu mips %lu array %lu samples %lu usage 0x%lx)", p->td.why,
            (unsigned long)d.textureType, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount,
            (unsigned long)d.arrayLength, (unsigned long)d.sampleCount, (unsigned long)d.usage]);
        return NO;
    }
    p->is1D = p->td.is1D;
    p->is3D = p->td.is3D;
    p->arr1D = d.textureType == MTLTextureType1DArray;
    p->levels = p->td.levels;
    p->depth = p->td.depth;
    p->samples = p->td.samples > 1 ? p->td.samples : 0;
    NSUInteger md = MAX(MAX(d.width, d.height), p->is3D ? p->depth : (NSUInteger)1), maxLevels = 1;
    while (md > 1) { md >>= 1; maxLevels++; }
    if (p->levels > maxLevels) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%lu mip levels for a %lux%lux%lu texture (at most %lu)",
            (unsigned long)p->levels, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->depth, (unsigned long)maxLevels]);
        return NO;
    }
    if ((p->fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) {
        if (err) *err = rdna4_err(44, @"A8Unorm render targets are not supported (an R8 attachment cannot take the fragment's alpha)");
        return NO;
    }
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, 0, 0, &swzWhy)) {
            if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"%s", swzWhy]);
            return NO;
        }
    }
    if (rdna4_fmt_is_bc(p->fmt)) {
        if (p->is1D || p->samples > 1 || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))) {
            if (err) *err = rdna4_err(44, @"block-compressed textures do not support 1D, multisample, render-target, or ShaderWrite usage");
            return NO;
        }
    }
    if (p->is3D && (d.usage & MTLTextureUsageRenderTarget)) {
        if (err) *err = rdna4_err(45, @"3D render targets are not supported");
        return NO;
    }
    if (!rdna4_radv_open(err)) return NO;
    if (rdna4_fmt_is_bc(p->fmt) && !RDNA4R.bcCompression) {
        if (err) *err = rdna4_err(30, @"RADV does not support BC texture compression");
        return NO;
    }
    if (d.textureType == MTLTextureTypeCubeArray && !RDNA4R.cubeArray) {
        if (err) *err = rdna4_err(31, @"RADV does not support cube-array image views");
        return NO;
    }
    if (p->samples > 1 && d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(31, [NSString stringWithFormat:@"multisample textures are Private storage only (storage mode %lu)", (unsigned long)d.storageMode]);
        return NO;
    }
    if (p->is1D && (d.height != 1 || d.width > RDNA4R.lim.maxImageDimension1D || p->td.layersIvar > RDNA4R.lim.maxImageArrayLayers)) {
        if (err) *err = rdna4_err(37, [NSString stringWithFormat:@"1D texture %lux%lu array %lu outside RADV limits (width <= %u, layers <= %u, height 1)",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->td.layersIvar, RDNA4R.lim.maxImageDimension1D, RDNA4R.lim.maxImageArrayLayers]);
        return NO;
    }
    if (p->is3D && (d.width > RDNA4R.lim.maxImageDimension3D || d.height > RDNA4R.lim.maxImageDimension3D || p->depth > RDNA4R.lim.maxImageDimension3D)) {
        if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"3D texture %lux%lux%lu outside the RADV limit %u",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->depth, RDNA4R.lim.maxImageDimension3D]);
        return NO;
    }
    VkFormatFeatureFlags ff = rdna4_fmt_feats(p->fmt);
    const VkFormatFeatureFlags need = p->isDS ? 0 : (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    if (p->isDS) {
        const char *why = NULL;
        if (!rdna4dp_check((unsigned long)d.textureType, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned)ff, &why)) {
            if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"%s (pixel format %lu vk %d features 0x%x type %lu storage %lu usage 0x%lx)",
                why, (unsigned long)d.pixelFormat, p->fmt->vk, ff, (unsigned long)d.textureType, (unsigned long)d.storageMode, (unsigned long)d.usage]);
            return NO;
        }
    }
    if ((ff & need) != need) {
        if (err) *err = rdna4_err(38, [NSString stringWithFormat:@"pixel format %lu (vk %d): RADV features 0x%x lack sampled/transfer",
            (unsigned long)d.pixelFormat, p->fmt->vk, ff]);
        return NO;
    }
    p->canRT = p->isDS ? ((ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                       : (!p->is1D && !p->is3D && !rdna4_fmt_is_bc(p->fmt) && !(p->fmt->flags & RDNA4F_A8) && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    if ((d.usage & MTLTextureUsageRenderTarget) && !p->canRT) {
        if (err) *err = rdna4_err(39, [NSString stringWithFormat:@"pixel format %lu (vk %d) type %lu cannot be a render target on RADV (features 0x%x)",
            (unsigned long)d.pixelFormat, p->fmt->vk, (unsigned long)d.textureType, ff]);
        return NO;
    }
    if (!rdna4swz_is_identity(rdna4_swz_from_mtl(d.swizzle))) p->canRT = NO;
    p->wantSt = (d.usage & MTLTextureUsageShaderWrite) != 0;
    if (p->wantSt && !(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"pixel format %lu (vk %d) has no storage-image support on RADV (features 0x%x)",
            (unsigned long)d.pixelFormat, p->fmt->vk, ff]);
        return NO;
    }
    VkImageUsageFlags iu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        (p->canRT ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
        (p->wantSt ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (p->isDS) iu = ((ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
        ((ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
        ((ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
        (p->canRT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    VkImageCreateFlags icf = ((d.usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) | p->td.flags |
        (aliasable ? VK_IMAGE_CREATE_ALIAS_BIT : 0) |
        ((RDNA4R.sampleLocationsExt && p->isDS && p->td.samples > 1) ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0);
    VkImageType vit = (VkImageType)p->td.imageType;
    VkImageFormatProperties ifp = {0};
    VkResult qr = vkGetPhysicalDeviceImageFormatProperties ? vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, p->fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp) : VK_SUCCESS;
    if (qr != VK_SUCCESS && p->canRT && !(d.usage & MTLTextureUsageRenderTarget)) {
        iu &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, p->fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp);
    }
    if (qr != VK_SUCCESS) {
        if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"RADV refuses image type %s format %d usage 0x%x (vkGetPhysicalDeviceImageFormatProperties = %d)",
            p->is1D ? "1D" : p->is3D ? "3D" : "2D", p->fmt->vk, iu, qr]);
        return NO;
    }
    if (p->td.samples > 1) {
        unsigned fc = vkGetPhysicalDeviceImageFormatProperties ? (unsigned)ifp.sampleCounts : 0u;
        if (!rdna4ms_texture_ok(p->td.samples, fc, p->isDS ? rdna4_ms_mask() : (unsigned)(RDNA4R.lim.framebufferColorSampleCounts))) {
            if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"%u samples are not supported for pixel format %lu (vk %d): the format offers 0x%x, the device's framebuffer limits 0x%x",
                p->td.samples, (unsigned long)d.pixelFormat, p->fmt->vk, fc, p->isDS ? rdna4_ms_mask() : (unsigned)RDNA4R.lim.framebufferColorSampleCounts]);
            return NO;
        }
    }
    if (vkGetPhysicalDeviceImageFormatProperties && p->td.layers > ifp.maxArrayLayers) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"%u array layers (%s) > RADV's %u for this format",
            p->td.layers, p->td.kind == RDNA4TD_K_CUBE ? "cube: 6 faces" : "array", ifp.maxArrayLayers]);
        return NO;
    }
    if (vkGetPhysicalDeviceImageFormatProperties && p->levels > ifp.maxMipLevels) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%lu mip levels > RADV's %u for this format", (unsigned long)p->levels, ifp.maxMipLevels]);
        return NO;
    }
    if (vkGetPhysicalDeviceImageFormatProperties &&
        (d.width > ifp.maxExtent.width || d.height > ifp.maxExtent.height ||
         (p->is3D && p->depth > ifp.maxExtent.depth))) {
        if (err) *err = rdna4_err(37, @"texture geometry exceeds the format's Vulkan image extent");
        return NO;
    }
    p->create = (VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = vit, .format = p->fmt->vk,
        .extent = { (uint32_t)d.width, (uint32_t)d.height, p->is3D ? (uint32_t)p->depth : 1 },
        .mipLevels = (uint32_t)p->levels, .arrayLayers = p->td.layers, .samples = (VkSampleCountFlagBits)p->td.samples,
        .flags = icf, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .usage = iu };
    return YES;
}

@implementation RDNA4Texture
#include "rdna4_texture_buffer_impl.inc"
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err {
    if (d.textureType == MTLTextureTypeTextureBuffer)
        return [self initTextureBufferWithDevice:dev descriptor:d buffer:nil offset:0 error:err];
    return [self initWithDevice:dev descriptor:d heap:nil error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap error:(NSError **)err {
    return [self initWithDevice:dev descriptor:d heap:heap offset:0 hasOffset:NO error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset error:(NSError **)err {
    return [self initWithDevice:dev descriptor:d heap:heap offset:offset hasOffset:YES error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _hpHeap = heap;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, heap != nil, &plan, err)) return nil;
    _fmt = plan.fmt;
    BOOL isDS = plan.isDS, is1D = plan.is1D, is3D = plan.is3D;
    rdna4td_t ti = plan.td;
    NSUInteger levels = plan.levels, depth = plan.depth;
    _is1D = is1D; _arr1D = plan.arr1D; _layers = ti.layersIvar; _tdk = ti.kind; _is3D = is3D; _levels = levels; _depth = depth; _samples = plan.samples; _aspects = plan.aspects;
    VkImageCreateInfo ic = plan.create;
    _imageFlags = ic.flags;
    VkResult r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(32, [NSString stringWithFormat:@"vkCreateImage = %d", r]); return nil; }
    VkMemoryRequirements mr; BOOL prefersDedicated = NO, requiresDedicated = NO;
    if (!rdna4_requirements2(VK_NULL_HANDLE, _img, &mr, &prefersDedicated, &requiresDedicated, heap != nil)) {
        if (err) *err = rdna4_err(22, @"heap textures require vkGetImageMemoryRequirements2 and dedicated requirements");
        [self rdna4AbortHeapConstruction];
        return nil;
    }
    BOOL dedicated = prefersDedicated || requiresDedicated;
    int mt = -1;
    if (heap) {
        VkDeviceMemory hmem = VK_NULL_HANDLE; uint64_t bindOff = 0, rangeSize = 0; uint8_t *map = NULL;
        if (hasOffset) {
            _hpOff = offset;
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquirePlacementBackingForBuffer:VK_NULL_HANDLE image:_img requirements:mr offset:offset
                                                  hostImport:NO requiresDedicated:requiresDedicated
                                                         mem:&hmem offset:&bindOff map:&map block:&_hpBlk
                                                       lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            dedicated = NO;
        } else {
            if (![heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]) {
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"heap has no logical range for a %llu-byte image requirement", (unsigned long long)mr.size]);
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquireBackingForBuffer:VK_NULL_HANDLE image:_img requirements:mr dedicated:dedicated hostImport:NO
                                                mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        (void)map;
        _hpBindOff = bindOff; _hpPhysSize = rangeSize; _hpDedicated = dedicated;
        _size = (NSUInteger)mr.size; _acct = 0;
        r = vkBindImageMemory(RDNA4R.dev, _img, hmem, bindOff);
        if (r != VK_SUCCESS) {
            if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d (heap block, offset %llu)", r, (unsigned long long)bindOff]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
    } else {
        mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) { if (err) *err = rdna4_err(33, @"no device-local memory type"); return nil; }
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        if (RDNA4P.on) {   // P1
            r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d (pool)", r]); return nil; }
            _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
        } else {
        r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_mem);
        if (r != VK_SUCCESS) { if (err) *err = rdna4_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d", r]); return nil; }
        _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
        if (r != VK_SUCCESS) { if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d", r]); return nil; }
        }
    }
    VkComponentMapping cm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&cm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img,
        .viewType = (VkImageViewType)ti.viewType,
        .format = _fmt->vk, .components = cm, .subresourceRange = { isDS ? rdna4dp_view_aspect(_aspects) : VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, ti.layers } };   // bundle 10: a depth/stencil sampling view has ONE aspect
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(36, [NSString stringWithFormat:@"vkCreateImageView = %d", r]);
        if (heap) [self rdna4AbortHeapConstruction];
        return nil;
    }
    if (heap) {
        _hpBacking = [[RDNA4HeapBacking alloc] initWithBuffer:VK_NULL_HANDLE image:_img view:_view];
        if (!_hpBacking || ![heap rdna4BindSucceeded:_hpBlk lease:_hpLease owner:_hpBacking]) {
            if (err) *err = rdna4_err(22, @"cannot retain the heap texture's lifecycle backing");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
    }
    RDNA4LOGR("RDNA4Texture %p: %lux%lu%s pf %lu vkformat %d levels %lu (alloc %llu, memtype %d)%s", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, is3D ? [NSString stringWithFormat:@"x%lu (3D)", (unsigned long)_depth].UTF8String : "",
           (unsigned long)_pf, _fmt->vk, (unsigned long)_levels, (unsigned long long)mr.size, mt, _viewSwz ? " [sampled through a swizzled view]" : "");
    return self;
}
// A single-level, identity-swizzle view for attachments, input attachments and storage images. Level 0 of a one-level, unswizzled texture is the sampling view itself.
// Bundle 9: a cube or 2D-array texture is attached one layer at a time (a plain 2D view of layer ly); every other texture has only layer 0.
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv { return [self rdna4AttViewLevel:lv layer:0]; }
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly {
    BOOL sameAsp = !_aspects || _aspects == rdna4dp_view_aspect(_aspects);   // bundle 10: the sampling view is the attachment view unless a combined depth/stencil format needs both aspects
    if (lv == 0 && ly == 0 && _levels <= 1 && !_viewSwz && !rdna4td_is_layered2d(_tdk) && sameAsp) return _view;
    RDNA4Texture *imageRoot = self; while (imageRoot->_root) imageRoot = imageRoot->_root;
    rdna4td_att_t av; if (!rdna4td_att_view(imageRoot->_tdk, (unsigned)imageRoot->_layers, [self rdna4Slice:ly], &av)) { RDNA4LOG("rdna4AttViewLevel %u layer %u: the texture has %u layer(s); falling back to the sampling view", lv, ly, [self rdna4Layers]); return _view; }
    @synchronized (self) {
        if (!_lvViews) _lvViews = [NSMutableDictionary dictionary];
        NSNumber *key = @(((unsigned long long)ly << 32) | lv);
        NSNumber *have = _lvViews[key]; if (have) return (VkImageView)(uintptr_t)have.unsignedLongLongValue;
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [self vkImage],
            .viewType = (VkImageViewType)av.viewType, .format = _fmt->vk,
            .subresourceRange = { rdna4dp_barrier_aspect(_aspects), [self rdna4MipLevel:lv], 1, av.baseLayer, av.layerCount } };   // bundle 10: colour, or every depth/stencil aspect of the format
        VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &v);
        if (r != VK_SUCCESS) { RDNA4LOG("rdna4AttViewLevel %u layer %u: vkCreateImageView = %d; falling back to the sampling view", lv, ly, r); return _view; }
        _lvViews[key] = @((unsigned long long)(uintptr_t)v);
        [_hpBacking addView:v];
        return v;
    }
}
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly layerCount:(uint32_t)lc {
    if (lc <= 1) return [self rdna4AttViewLevel:lv layer:ly];
    @synchronized (self) {
        if (!_lvViews) _lvViews = [NSMutableDictionary dictionary];
        unsigned long long rawKey = (1ULL << 62) | ((unsigned long long)lc << 40) | ((unsigned long long)ly << 16) | (unsigned long long)lv;
        NSNumber *key = @(rawKey);
        NSNumber *have = _lvViews[key]; if (have) return (VkImageView)(uintptr_t)have.unsignedLongLongValue;
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [self vkImage],
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = _fmt->vk,
            .subresourceRange = { rdna4dp_barrier_aspect(_aspects), [self rdna4MipLevel:lv], 1, [self rdna4Slice:ly], lc } };
        VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &v);
        if (r != VK_SUCCESS) { RDNA4LOG("rdna4AttViewLevel %u layer %u+%u: vkCreateImageView = %d; falling back to single layer", lv, ly, lc, r); return [self rdna4AttViewLevel:lv layer:ly]; }
        _lvViews[key] = @((unsigned long long)(uintptr_t)v);
        [_hpBacking addView:v];
        return v;
    }
}
- (VkImageView)rdna4StorageView {
    if (!_viewSwz && _levels <= 1 && !_aspects) return _view;
    if (rdna4td_is_layered2d(_tdk)) return [self rdna4AttViewLevel:0 layer:0 layerCount:[self rdna4Layers]];
    return [self rdna4AttViewLevel:0];
}
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (h) _hpPhysSize = sz;
    if (_acct) { atomic_fetch_sub(&rdna4_alloc_total, _acct); _acct = 0; }   // the heap's size is what counts
}
- (RDNA4Texture *)rdna4ImageRoot {
    RDNA4Texture *root = self;
    while (root->_root) root = root->_root;
    return root;
}
- (RDNA4Heap *)rdna4ResourceHeap { RDNA4Texture *root = [self rdna4ImageRoot]; return root->_hpHeap ?: [root->_hostBuf rdna4ResourceHeap]; }
- (NSUInteger)rdna4ResourceHeapOffset {
    if (_texel) return [_hostBuf rdna4ResourceHeapOffset];
    RDNA4Texture *root = [self rdna4ImageRoot];
    if (root->_hpHeap) return [root->_hpHeap type] == MTLHeapTypePlacement ? root->_hpOff : 0;
    return root->_hostBuf ? [root->_hostBuf rdna4ResourceHeapOffset] : 0;
}
- (BOOL)rdna4ResourceAliasable { return _texel ? [_hostBuf isAliasable] : _root ? [_root rdna4ResourceAliasable] : _hpAl; }
- (void)rdna4ResourceMakeAliasable {
    if (_root) {
        RDNA4_ONCE("makeAliasable on a texture view: prohibited by Metal; ignored without releasing the base resource");
        return;
    }
    RDNA4_HEAP_RETIRE;
}
- (BOOL)rdna4ResourceAliases:(id)other {
    if (_texel) return [_hostBuf rdna4ResourceAliases:other];
    RDNA4Texture *root = [self rdna4ImageRoot];
    return root->_hpHeap && root->_hpBlk &&
        [root->_hpHeap rdna4Block:root->_hpBlk offset:root->_hpBindOff size:root->_hpPhysSize aliases:other];
}
- (uint64_t)rdna4BackingBlock { return _texel ? [_hostBuf rdna4BackingBlock] : _root ? [_root rdna4BackingBlock] : _hpBlk; }
- (uint64_t)rdna4BackingOffset { return _texel ? [_hostBuf rdna4BackingOffset] : _root ? [_root rdna4BackingOffset] : _hpBindOff; }
- (uint64_t)rdna4BackingSize { return _texel ? [_hostBuf rdna4BackingSize] : _root ? [_root rdna4BackingSize] : _hpPhysSize; }
- (void)rdna4AbortHeapConstruction {
    RDNA4Heap *heap = _hpHeap;
    uint64_t bid = _hpId, block = _hpBlk, lease = _hpLease;
    const BOOL bound = _hpBacking != nil;
    [self rdna4DestroyRetiredBacking];
    _hpBacking = nil;
    if (heap && block && bound) [heap rdna4CloseBackingHandle:block];
    if (heap && lease) [heap rdna4AbandonBacking:lease];
    if (heap && bid) [heap rdna4Free:bid];
    _hpHeap = nil;
    _hpId = _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0;
    _hpDedicated = _hpAl = NO; _hpOff = _hpSize = 0; _size = 0; _acct = 0;
}
- (void)rdna4DestroyRetiredBacking {
    if (_texelView && RDNA4R.ok) vkDestroyBufferView(RDNA4R.dev, _texelView, NULL);
    _texelView = VK_NULL_HANDLE;
    if (_hpBacking) [_hpBacking destroyHandles];
    else if (RDNA4R.ok) {
        for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        if (_view) vkDestroyImageView(RDNA4R.dev, _view, NULL);
        if (_img && !_root) vkDestroyImage(RDNA4R.dev, _img, NULL);
    }
    [_lvViews removeAllObjects];
    _view = VK_NULL_HANDLE; if (!_root) _img = VK_NULL_HANDLE;
}
- (void)dealloc {
    const BOOL heapBacked = _hpHeap && _hpBlk;
    RDNA4_HEAP_DEALLOC;
    if (RDNA4R.ok && !heapBacked) {
        if (_texelView) vkDestroyBufferView(RDNA4R.dev, _texelView, NULL);
        for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        if (_view) vkDestroyImageView(RDNA4R.dev, _view, NULL);
        if (_img && !_root) vkDestroyImage(RDNA4R.dev, _img, NULL);
        if (_pooled) { rdna4_mem_free(&_pm); atomic_fetch_sub(&rdna4_alloc_total, _acct); }   // P1
        else if (_mem) { vkFreeMemory(RDNA4R.dev, _mem, NULL); atomic_fetch_sub(&rdna4_alloc_total, _acct); }
        if (_ibuf) vkDestroyBuffer(RDNA4R.dev, _ibuf, NULL);
        if (_imem && _impShared) rdna4_imp_put(_imem);   // P4: the shared import goes back to the cache (kept 2 s; freed later through the fence)
        else if (_imem) { vkFreeMemory(RDNA4R.dev, _imem, NULL); @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _ialloc; } }
    }
    if (_led) rdna4_led_del(self);   // build 16 (P5 Step 0)
    if (_ios) CFRelease(_ios);   // build 16 (F3): no use count is held for the texture's life any more; a command buffer holds it while in flight
}
RDNA4_DNR(RDNA4Texture)
// ---- 11h.6: IOSurface-backed texture ----
// Accepts a 2D, page-aligned (P2: any plane of a 2/3-plane surface, via rdna4_plane.h), page-multiple 32 bpp surface (BGRA8Unorm / RGBA8Unorm descriptors); anything else returns nil + log.
// The surface is retained for the texture's lifetime (build 16, F3: NOT use-counted for its life; a command buffer use-counts it while in flight, rdna4_ledger.h); its pages are imported once (whole allocation).
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    #define IOSFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG("newTexture(iosurface): refused: %s", m_.UTF8String); if (err) *err = rdna4_err(code, m_); return nil; } while (0)
    if (!s) IOSFAIL(70, @"NULL IOSurface");
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED; _iosPlane = plane;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = rdna4_fmt(_pf);
    // m11h9: any pixel format of the table whose bytes per pixel equal the surface's bytes per element (BGR10A2Unorm 'l10r' and RGBA16Float 'RGhA' are what the compositor's
    // display surfaces use); an A8 / render-target mismatch is refused below by the same rules as an ordinary texture.
    if (!_fmt) IOSFAIL(71, @"pixel format %lu is not in the format table", (unsigned long)_pf);
    if (rdna4_fmt_is_bc(_fmt)) IOSFAIL(71, @"block-compressed IOSurface textures are not supported");
    if ((_fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) IOSFAIL(71, @"A8Unorm render targets are not supported");
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(_swz), (unsigned long)d.usage, 0, 0, &swzWhy))
            IOSFAIL(71, @"%s", swzWhy);
    }
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72, @"only single 2D textures (no mips/array/MSAA)");
    size_t pc = IOSurfaceGetPlaneCount(s);
    // P2: a plane p < planeCount of a 2- or 3-plane surface is accepted (CoreAnimation makes one texture per plane of a bi-planar YUV surface). The kill file
    // /private/tmp/rdna4m-noplanes (latched once per process) restores the old refusal of every multi-plane surface.
    static int noplanes = -1; if (noplanes < 0) { noplanes = access(RDNA4_NOPLANES_FILE, F_OK) == 0; if (noplanes) RDNA4LOG("P2 planes: multi-plane IOSurfaces REFUSED (kill file " RDNA4_NOPLANES_FILE " exists; latched for the life of this process)"); }
    if (pc > 1 && noplanes) IOSFAIL(73, @"IOSurface has %zu planes; single-plane surfaces only", pc);
    size_t pidx = pc > 1 ? plane : 0;   // pc <= 1: any plane != 0 is refused by rdna4pl_geom below, exactly as before
    size_t sw = pc ? IOSurfaceGetWidthOfPlane(s, pidx) : IOSurfaceGetWidth(s), sh = pc ? IOSurfaceGetHeightOfPlane(s, pidx) : IOSurfaceGetHeight(s);
    size_t bpe = pc ? IOSurfaceGetBytesPerElementOfPlane(s, pidx) : IOSurfaceGetBytesPerElement(s);
    size_t bpr = pc ? IOSurfaceGetBytesPerRowOfPlane(s, pidx) : IOSurfaceGetBytesPerRow(s);
    rdna4pl_in pin = { .pc = pc, .plane = plane, .pw = sw, .ph = sh, .pbpe = bpe, .pbpr = bpr, .poff = 0, .alloc = 0, .dw = _w, .dh = _h, .dbpp = _fmt->bpp };
    rdna4pl_out pout;
    if (!rdna4pl_geom(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    if (!rdna4_radv_open(err)) return nil;
    {   // the colour-attachment / sampled features of this format (RADV decides; before m11h9 every surface was 32 bpp BGRA8/RGBA8 and this was implied)
        VkFormatFeatureFlags ff = rdna4_fmt_feats(_fmt), nd = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            ((d.usage & MTLTextureUsageRenderTarget) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((ff & nd) != nd) IOSFAIL(92, @"pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)_pf, _fmt->vk, ff, nd & ~ff);
    }
    if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) IOSFAIL(78, @"VK_EXT_external_memory_host is not available on this RADV");
    IOSurfaceLock(s, kIOSurfaceLockAvoidSync, NULL);
    void *base = IOSurfaceGetBaseAddress(s);   // P2: the whole allocation is imported from the surface base; the plane is used at its offset
    void *pbase = pc ? IOSurfaceGetBaseAddressOfPlane(s, pidx) : base;
    IOSurfaceUnlock(s, kIOSurfaceLockAvoidSync, NULL);
    size_t alloc = IOSurfaceGetAllocSize(s);
    uint32_t spf = IOSurfaceGetPixelFormat(s);
    RDNA4LOGR("newTexture(iosurface): id %u %zux%zu bpr %zu allocSize %zu base %p pixelFormat 0x%08x planes %zu", IOSurfaceGetID(s), sw, sh, bpr, alloc, base, spf, pc);
    // F1 (build 16): when the import of the surface is refused (out of the kernel's import budget, an unimportable range) this init does NOT return nil (SkyLight aborts on a nil texture: AbortWithTextureInfo). It jumps to
    // the "baseless" branch below (same GPU-only image, same classification) with f1 set: the content captured through this wrapper is blank, the process stays up. The import cache is flushed and the import retried
    // first (rdna4_imp_get). The branch itself is not edited for this except that a size difference between descriptor and plane no longer refuses and no "protected" note is logged.
    BOOL f1 = NO;
    #define RDNA4F1_FALL(...) do { f1 = YES; atomic_fetch_add(&RDNA4LED.f1Fallbacks, 1); static _Atomic int f1log_; if (atomic_fetch_add(&f1log_, 1) < 16) { NSString *w_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG("F1: the import of IOSurface %u was refused (%s): GPU-only texture instead of nil (the content captured through this wrapper is blank; the process stays up)", IOSurfaceGetID(s), w_.UTF8String); } goto nobase_path; } while (0)
    if (!pbase || !base) {   // P4: a protected IOSurface has no CPU mapping (CoreDisplay's DisplaySurface::GetMTLTexture cannot survive a nil texture). Not refused: a device-local optimal image of the descriptor's
        // size/format that is NOT backed by the surface's memory (the surface's contents are not shared). No import, no _ibuf, no _ibase: rdna4IsIOS is NO, so no upload/write-back copy and no CPU path touches a NULL base.
        // P6: classified like any other IOSurface texture (below, before return): with only the VkImage as D-copy source (vkCmdCopyImageToBuffer).
      nobase_path:
        if (!f1) rdna4_baseless_note(IOSurfaceGetID(s));
        if (!f1 && (_w != sw || _h != sh)) IOSFAIL(79, @"IOSurface has no CPU mapping and the descriptor %lux%lu differs from the plane %zux%zu", (unsigned long)_w, (unsigned long)_h, sw, sh);
        VkFormatFeatureFlags nbf = rdna4_fmt_feats(_fmt);
        BOOL nbAtt = !(_fmt->flags & RDNA4F_A8) && (nbf & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        VkImageUsageFlags nbu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (nbAtt ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                                ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        VkImageCreateInfo nic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = nbu, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkResult nr = vkCreateImage(RDNA4R.dev, &nic, NULL, &_img);
        if (nr != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal, no CPU mapping) = %d", nr); }
        VkMemoryRequirements nmr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &nmr);
        int nmt = rdna4_find_mem(nmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (nmt < 0) IOSFAIL(86, @"no device-local memory type");
        if (RDNA4P.on) {
            nr = rdna4_mem_alloc((uint32_t)nmt, nmr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (nr != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", nr);
            _pooled = YES; _acct = nmr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            nr = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", nr, (unsigned long long)_pm.off);
        } else {
            VkMemoryAllocateInfo nma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = nmr.size, .memoryTypeIndex = (uint32_t)nmt };
            nr = vkAllocateMemory(RDNA4R.dev, &nma, NULL, &_mem);
            if (nr != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", nr); }
            _acct = nmr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            nr = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", nr);
        }
        _levels = 1; _depth = 1; _size = nmr.size;
        VkComponentMapping nbm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&nbm);
        VkImageViewCreateInfo nvc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = _fmt->vk, .components = nbm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        nr = vkCreateImageView(RDNA4R.dev, &nvc, NULL, &_view);
        if (nr != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", nr);
        _iosNoBase = YES; _ios = (IOSurfaceRef)CFRetain(s); rdna4_led_add(self, s, 0, spf, (uint32_t)pc, YES); _led = YES;   // retained (F3: not use-counted for its life) and entered in the ledger; set last so a failed init above never releases
        RDNA4LOG("newTexture(iosurface) %p: plane %lu %zux%zu bpr %zu pf %lu -> GPU-ONLY (no CPU mapping): optimal VkImage, not backed by the surface's memory", (__bridge void *)self, (unsigned long)plane, sw, sh, bpr, (unsigned long)_pf);
        // P6: same rule as the mapped surfaces (rdna4s_classify: geometry/pitch vs the plane + the DisplaySurface::GetMTLTexture backtrace). Inputs: width/height/pixel format/bytesPerRow/allocSize from the IOSurface
        // object (none needs a CPU mapping); alloc 0 -> bpr*height (rdna4df_nobase_alloc); the cache key's base is 0, which no mapped surface has (a mapped one passed the !base test above).
        // The image is the copy source, so the format must be 4 bytes per texel (the slot's pitch is in 4-byte texels) and the image must be the plane's size.
        if (pc <= 1 && _fmt->bpp == 4 && rdna4s_classify(sw, sh, spf, d.usage, bpr, (size_t)rdna4df_nobase_alloc(alloc, bpr, sh), IOSurfaceGetID(s), NULL)) {
            _disp = YES;
            static _Atomic int nblog; if (atomic_fetch_add(&nblog, 1) < 8) RDNA4LOG("scanout: display surface %u has no CPU mapping (protected): its copies into the scanout slot come from the VkImage (vkCmdCopyImageToBuffer, TRANSFER_SRC)", IOSurfaceGetID(s));
        }
        return self;
    }
    if (((uintptr_t)base & (RDNA4R.hostAlign - 1)) || !alloc) RDNA4F1_FALL(@"base %p / allocSize %zu: base not aligned to %llu (page-aligned surfaces only)", base, alloc, (unsigned long long)RDNA4R.hostAlign);
    pin.alloc = alloc; pin.poff = ((uintptr_t)pbase >= (uintptr_t)base) ? (uint64_t)((uintptr_t)pbase - (uintptr_t)base) : UINT64_MAX;
    if (!rdna4pl_bound(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    const NSUInteger poff = (NSUInteger)pout.off;
    if (pc > 1) RDNA4LOG("newTexture(iosurface): plane %lu of %zu: offset %lu size %zux%zu bpe %zu bpr %zu (bufferRowLength %zu texels)", (unsigned long)plane, pc, (unsigned long)poff, sw, sh, bpe, bpr, pout.rowlen);
    // m11h6: IOSurface allocations are page-granular in the VM map, but IOSurfaceGetAllocSize reports the exact byte size (e.g. 5760, 303360, 1840000).
    // Import the whole pages that hold the surface: round up to the host-pointer alignment. The extra tail bytes are the surface's own page, never another object's.
    const size_t ialloc = (alloc + (size_t)RDNA4R.hostAlign - 1) & ~((size_t)RDNA4R.hostAlign - 1);
    if (ialloc != alloc) RDNA4LOG("newTexture(iosurface): allocSize %zu rounded up to %zu for the page-granular import", alloc, ialloc);
    if (ialloc > 64u << 20) RDNA4F1_FALL(@"allocSize %zu > 64 MiB (the RDNA4N per-BO import limit)", ialloc);
    // Import (whole allocation). The kernel refuses BAR pages, unaligned ranges and > 256 MiB per client; that arrives as a vkAllocateMemory error.
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = rdna4_now();
    VkResult r = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) RDNA4F1_FALL(@"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    int hmt = rdna4_find_mem(hp.memoryTypeBits, 0);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = base };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = ialloc, .memoryTypeIndex = (uint32_t)hmt };
    BOOL impHit = NO;
    if (RDNA4IC.on) { r = rdna4_imp_get(s, base, ialloc, &ma, &_imem, &impHit); _impShared = YES; }   // P4: shared import (a reused one is a hit)
    else r = rdna4_import_alloc(&ma, &_imem);
    rdna4t_add(&T1.imp, rdna4_now() - timp_);
    if (rdna4f1_action((int)r) == RDNA4F1_FALLBACK) { _imem = VK_NULL_HANDLE; _impShared = NO; RDNA4F1_FALL(@"vkAllocateMemory(import %zu B at %p) = %d, running import total %llu B", ialloc, base, r, (unsigned long long)RDNA4R.impTotal); }
    int protection = rdna4nc_host_readonly(base, ialloc);
    _hostReadOnly = protection != 0;
    if (protection < 0) RDNA4LOG("RDNA4Texture %p: NO-COPY protection unknown, treating as read-only", (__bridge void *)self);
    if (_hostReadOnly) RDNA4LOG("RDNA4Texture %p: NO-COPY %zu bytes at %p imported READ-ONLY (GPU writes will be dropped)", (__bridge void *)self, ialloc, base);
    if (impHit) { static _Atomic int nlog; if (atomic_fetch_add(&nlog, 1) < 8) RDNA4LOG("newTexture(iosurface): id %u import REUSED from the cache (%zu B)", IOSurfaceGetID(s), ialloc); }
    else @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += ialloc; RDNA4LOG("newTexture(iosurface): imported %zu B (host memory type %d, bits 0x%x); running import total %llu B", ialloc, hmt, hp.memoryTypeBits, (unsigned long long)RDNA4R.impTotal); }
    _ialloc = ialloc; _ibase = (uint8_t *)base + poff; _ioff = poff; _ibpr = bpr; _ios = (IOSurfaceRef)CFRetain(s); rdna4_led_add(self, s, ialloc, spf, (uint32_t)pc, NO); _led = YES; _size = ialloc;   // F3: retained, not use-counted for its life; entered in the ledger
    VkFormatFeatureFlags offs = rdna4_fmt_feats(_fmt);
    BOOL attOK = !(_fmt->flags & RDNA4F_A8) && (offs & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (attOK ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                            ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const char *force = getenv("RDNA4M_IOS_PATH");   // test hook: "b" forces the copy path
    // The first submitted segment restores the first-use snapshot with vkCmdCopyBufferToImage into the
    // LINEAR image bound to these host pages, even for a sampled-only use. RO imports need path (b).
    BOOL tryA = !_hostReadOnly && !(force && force[0] == 'b');
    if (tryA) {   // path (a) needs the LINEAR-tiling features for every usage asked for (RADV does not check at vkCreateImage); otherwise the copy path (b)
        VkFormatProperties lfp = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, _fmt->vk, &lfp);
        VkFormatFeatureFlags lneed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            (attOK ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((lfp.linearTilingFeatures & lneed) != lneed) { RDNA4LOG("newTexture(iosurface): path (a) unavailable: LINEAR features 0x%x lack 0x%x for vk format %d", lfp.linearTilingFeatures, lneed & ~lfp.linearTilingFeatures, _fmt->vk); tryA = NO; }
        else RDNA4LOG("newTexture(iosurface): LINEAR features 0x%x cover 0x%x for vk format %d", lfp.linearTilingFeatures, lneed, _fmt->vk);
    }
    if (tryA) {
        VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .flags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage li = VK_NULL_HANDLE; r = vkCreateImage(RDNA4R.dev, &ic, NULL, &li);
        if (r != VK_SUCCESS) RDNA4LOG("newTexture(iosurface): path (a) unavailable: linear vkCreateImage = %d", r);
        else {
            VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(RDNA4R.dev, li, &sr, &sl);
            VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, li, &mr);
            const VkDeviceSize naturalRowPitch = sl.rowPitch;
            const BOOL naturalPitchOK = sl.rowPitch == bpr && sl.offset == 0;
            BOOL pitchOK = naturalPitchOK;
            BOOL explicitOK = NO, alreadyBound = NO;
            int explicitReason = rdna4p_explicit_pitch_reason(bpr, _w, _fmt->bpp, sl.rowPitch, ic.flags, ic.mipLevels, ic.arrayLayers, ic.samples, RDNA4R.drmModExt);
            if (sl.rowPitch == bpr && sl.offset != 0) explicitReason = RDNA4P_EXPLICIT_OFFSET;
            if (!pitchOK && explicitReason == RDNA4P_EXPLICIT_TRY) {
                VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierQuery = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
                    .drmFormatModifier = 0,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
                VkPhysicalDeviceImageFormatInfo2 formatQuery = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                    .pNext = &modifierQuery, .format = ic.format, .type = ic.imageType,
                    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = ic.usage, .flags = ic.flags };
                VkImageFormatProperties2 formatProps = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
                VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(RDNA4R.pd, &formatQuery, &formatProps);
                vkDestroyImage(RDNA4R.dev, li, NULL); li = VK_NULL_HANDLE;
                if (qr == VK_SUCCESS) {
                    VkSubresourceLayout planeLayout = { .offset = 0, .size = 0, .rowPitch = bpr };
                    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                        .pNext = &emi, .drmFormatModifier = 0,
                        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &planeLayout };
                    VkImageCreateInfo explicitIc = ic;
                    explicitIc.pNext = &modifierInfo;
                    explicitIc.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                    r = vkCreateImage(RDNA4R.dev, &explicitIc, NULL, &li);
                    if (r == VK_SUCCESS) {
                        VkImageSubresource explicitSr = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
                        vkGetImageSubresourceLayout(RDNA4R.dev, li, &explicitSr, &sl);
                        vkGetImageMemoryRequirements(RDNA4R.dev, li, &mr);
                        pitchOK = sl.rowPitch == bpr && sl.offset == 0;
                        BOOL explicitFits = pitchOK && rdna4pl_bind_ok(poff, mr.size, mr.alignment, ialloc) &&
                            (mr.alignment == 0 || ((uintptr_t)base % mr.alignment) == 0) && (mr.memoryTypeBits & (1u << hmt));
                        if (explicitFits) {
                            r = vkBindImageMemory(RDNA4R.dev, li, _imem, poff);
                            explicitFits = r == VK_SUCCESS;
                            alreadyBound = explicitFits;
                        }
                        explicitOK = explicitFits;
                        if (!explicitFits) {
                            explicitReason = pitchOK ? RDNA4P_EXPLICIT_BIND : RDNA4P_EXPLICIT_LAYOUT;
                            pitchOK = NO;
                            vkDestroyImage(RDNA4R.dev, li, NULL); li = VK_NULL_HANDLE;
                        }
                    } else explicitReason = RDNA4P_EXPLICIT_CREATE_FAILED;
                } else explicitReason = RDNA4P_EXPLICIT_UNSUPPORTED;
            }
            static _Atomic int explicitLogCount;
            if (!naturalPitchOK && atomic_fetch_add(&explicitLogCount, 1) < 8)
                RDNA4LOG("newTexture(iosurface): natural pitch %llu vs IOSurface bpr %zu -> explicit pitch %s%s%s%s",
                    (unsigned long long)naturalRowPitch, bpr, explicitOK ? "OK" : "refused",
                    explicitOK ? "" : " (", explicitOK ? "" : rdna4p_explicit_pitch_reason_text(explicitReason), explicitOK ? "" : ")");
            NSString *pitchResult = !naturalPitchOK ? [NSString stringWithFormat:@" -> explicit pitch %@%@",
                explicitOK ? @"OK" : @"refused", explicitOK ? @"" : [NSString stringWithFormat:@" (%s)", rdna4p_explicit_pitch_reason_text(explicitReason)]] : @"";
            RDNA4LOG("newTexture(iosurface): linear layout for %lux%lu (descriptor; surface %zux%zu): rowPitch %llu offset %llu size %llu vs IOSurface bytesPerRow %zu -> %s%s; image req size %llu align %llu typeBits 0x%x (host bits 0x%x)",
                   (unsigned long)_w, (unsigned long)_h, sw, sh, (unsigned long long)sl.rowPitch, (unsigned long long)sl.offset, (unsigned long long)sl.size, bpr, naturalPitchOK ? "MATCH" : "MISMATCH",
                   pitchResult.UTF8String,
                   (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits, hp.memoryTypeBits);
            BOOL fits = pitchOK && rdna4pl_bind_ok(poff, mr.size, mr.alignment, ialloc) && (mr.alignment == 0 || ((uintptr_t)base % mr.alignment) == 0) && (mr.memoryTypeBits & (1u << hmt));
            if (pitchOK && !fits && poff) RDNA4LOG("newTexture(iosurface): plane offset %lu: LINEAR bind at that offset not possible (image align %llu size %llu, import %zu) -> copy path (b)", (unsigned long)poff, (unsigned long long)mr.alignment, (unsigned long long)mr.size, ialloc);
            if (pitchOK && !fits) RDNA4LOG("newTexture(iosurface): path (a) unavailable: image requirements do not fit the import (size/alignment/memory type)");
            if (fits && !alreadyBound) { r = vkBindImageMemory(RDNA4R.dev, li, _imem, poff); if (r != VK_SUCCESS) { RDNA4LOG("newTexture(iosurface): path (a) unavailable: vkBindImageMemory = %d", r); fits = NO; } }
            if (fits && li) { _img = li; _iosLinear = YES; } else if (li) vkDestroyImage(RDNA4R.dev, li, NULL);
        }
    }
    if (!_iosLinear) {   // path (b): optimal device-local image + a VkBuffer over the import
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
        if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal) = %d", r); }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
        int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) IOSFAIL(86, @"no device-local memory type");
        VkMemoryAllocateInfo m2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        if (RDNA4P.on) {   // P1 (the optimal device-local image of path (b); the import stays as it was)
            r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (r != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", r);
            _pooled = YES; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
        } else {
        r = vkAllocateMemory(RDNA4R.dev, &m2, NULL, &_mem);
        if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", r); }
        _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
        if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", r);
        }
    }
    // Both paths need a buffer alias: first-use preservation and scanout for (a), uploads/writeback for (b).
    VkExternalMemoryBufferCreateInfo emb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb,
        .size = alloc, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_ibuf);
    if (r != VK_SUCCESS) { _ibuf = VK_NULL_HANDLE; IOSFAIL(89, @"vkCreateBuffer(import) = %d", r); }
    r = vkBindBufferMemory(RDNA4R.dev, _ibuf, _imem, 0);
    if (r != VK_SUCCESS) IOSFAIL(90, @"vkBindBufferMemory(import) = %d", r);
    _levels = 1; _depth = 1;
    VkComponentMapping icm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&icm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = _fmt->vk, .components = icm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", r);
    RDNA4LOGR("newTexture(iosurface) %p: plane %lu offset %lu %zux%zu bpr %zu pf %lu -> PATH (%s): %s", (__bridge void *)self, (unsigned long)plane, (unsigned long)poff, sw, sh, bpr, (unsigned long)_pf, _iosLinear ? "a" : "b",
           _iosLinear ? "LINEAR VkImage bound directly to the imported IOSurface pages" : "optimal VkImage + copies to/from a VkBuffer over the imported pages (bufferRowLength = bytesPerRow/4)");
    // S5.2a: is this one of CoreDisplay's display surfaces (D-copy source)? Both mapped paths have the import-buffer alias.
    if (pc <= 1 && _w == sw && _h == sh && rdna4s_classify(sw, sh, spf, d.usage, bpr, alloc, IOSurfaceGetID(s), base)) {   // build 7: only an exact-size texture can be a display surface
        _disp = YES;
    }
    #undef IOSFAIL
    return self;
}
// A texture over a buffer (Metal's linear texture; QuartzCore's client-memory backing stores). Single-level 2D, formats of the table, Shared/Managed buffer.
// Path (b) of the IOSurface design: an optimal device-local image plus copies between it and the buffer around each command buffer (upload at the first use,
// write-back at the end), so the buffer memory (the client's memory for a no-copy buffer) is always the CPU-visible truth.
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(RDNA4Buffer *)b offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr error:(NSError **)err {
    if (d.textureType == MTLTextureTypeTextureBuffer) {
        self = [self initTextureBufferWithDevice:dev descriptor:d buffer:b offset:off error:err];
        if (self) _ibpr = bpr ?: _w * _fmt->bpp;
        return self;
    }
    self = [super init]; if (!self) return nil;
    #define BFAIL(code, ...) do { if (err) *err = rdna4_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = rdna4_fmt(_pf);
    if (!_fmt) BFAIL(120, @"pixel format %lu not supported", (unsigned long)_pf);
    if (rdna4_fmt_is_bc(_fmt)) BFAIL(120, @"block-compressed buffer-backed textures are not supported");
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) BFAIL(121, @"only single 2D textures (no mips/array/MSAA)");
    if ((_fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) BFAIL(121, @"A8Unorm render targets are not supported");
    if (![b isKindOfClass:[RDNA4Buffer class]] || ![b contents]) BFAIL(122, @"the buffer has no CPU mapping (Private storage)");
    uint32_t bpp = _fmt->bpp;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(_w, _h, 1, bpp, bpr, 0, &transfer) || bpr % bpp)
        BFAIL(123, @"bytesPerRow %lu or geometry invalid for a %lu-wide %u-byte format", (unsigned long)bpr, (unsigned long)_w, bpp);
    if (off % 4 || off % bpp) BFAIL(124, @"offset %lu must be a multiple of 4 and of the pixel size", (unsigned long)off);
    uint64_t vkoff;
    if (__builtin_add_overflow([b rdna4VkOffset], (uint64_t)off, &vkoff) ||
        !rdna4nc_vk_offset_ok(vkoff, bpp, RDNA4NC_ALIGN_IMAGE)) BFAIL(124, @"buffer-backed texture Vulkan offset overflows or violates pixel alignment");
    if (!rdna4_buffer_range([b length], off, transfer.span)) BFAIL(125, @"offset %lu + %lu rows of %lu bytes exceed the %lu-byte buffer", (unsigned long)off, (unsigned long)_h, (unsigned long)bpr, (unsigned long)[b length]);
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, NO, &plan, err)) return nil;
    VkImageCreateInfo ic = plan.create;
    VkResult r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; BFAIL(126, @"vkCreateImage = %d", r); }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) BFAIL(127, @"no device-local memory type");
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (RDNA4P.on) {   // P1
        r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
        if (r != VK_SUCCESS) BFAIL(128, @"vkAllocateMemory = %d (pool)", r);
        _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
        if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
    } else {
    r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_mem);
    if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; BFAIL(128, @"vkAllocateMemory = %d", r); }
    _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
    r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
    if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d", r);
    }
    _levels = 1; _depth = 1;
    VkComponentMapping bcm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&bcm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = _fmt->vk, .components = bcm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) BFAIL(130, @"vkCreateImageView = %d", r);
    _hostBuf = b; _hoff = off; _ibase = (uint8_t *)[b contents] + off; _ibpr = bpr;
    RDNA4LOG("RDNA4Texture %p: %lux%lu pf %lu over buffer %p offset %lu bytesPerRow %lu (%s memory; copy path b)", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, (unsigned long)_pf,
           (__bridge void *)b, (unsigned long)off, (unsigned long)bpr, [b rdna4Imported] ? "client" : "bundle");
    #undef BFAIL
    return self;
}
- (BOOL)rdna4IsDisp { return _root ? [_root rdna4IsDisp] : _disp; }
- (const uint8_t *)rdna4IBase { return _root ? [_root rdna4IBase] : _ibase; }
- (IOSurfaceRef)rdna4IOSRef { return _root ? [_root rdna4IOSRef] : (_iosNoBase ? NULL : _ios); }   // P4: a base-less texture is not an IOSurface-coherent texture (sid tracking, CRC)
- (VkBuffer)rdna4DispBuf { return _root ? [_root rdna4DispBuf] : _ibuf; }
- (BOOL)rdna4DispImgOnly { return _root ? [_root rdna4DispImgOnly] : (_disp && _iosNoBase && !_ibuf); }
- (unsigned)rdna4DispSid { return _root ? [_root rdna4DispSid] : ((_disp && _ios) ? (unsigned)IOSurfaceGetID(_ios) : 0); }
- (BOOL)rdna4IsIOS { return _root ? [_root rdna4IsIOS] : ((_ios != NULL && !_iosNoBase) || (_hostBuf != nil && !_texel)); }
- (NSUInteger)rdna4IOSOffset { return _root ? [_root rdna4IOSOffset] : (_hostBuf ? [_hostBuf rdna4VkOffset] + _hoff : _ioff); }
- (IOSurfaceRef)rdna4UCSurface { return _root ? [_root rdna4UCSurface] : _ios; }
- (void)rdna4AliasKey:(uint32_t *)sid off:(uint64_t *)off {
    if (_root) { [_root rdna4AliasKey:sid off:off]; return; }
    if (_hostBuf) { *sid = 0; *off = (uint64_t)(uintptr_t)[_hostBuf contents] + (uint64_t)_hoff; }
    else { *sid = _ios ? (uint32_t)IOSurfaceGetID(_ios) : 0; *off = (uint64_t)_ioff; }
}
- (BOOL)rdna4HostReadOnly { return _root ? [_root rdna4HostReadOnly] : (_hostReadOnly || [_hostBuf rdna4HostReadOnly]); }
- (BOOL)rdna4IOSLinear { return _root ? [_root rdna4IOSLinear] : _iosLinear; }
- (BOOL)rdna4IOSLinearNeedsInit { return _iosLinear && !_iosLinearSubmitted; }
- (void)rdna4IOSLinearDidSubmit { _iosLinearSubmitted = YES; }
- (VkBuffer)rdna4IOSBuffer { return _root ? [_root rdna4IOSBuffer] : (_hostBuf ? [_hostBuf vkBuffer] : _ibuf); }
- (size_t)rdna4IOSBytesPerRow { return _root ? [_root rdna4IOSBytesPerRow] : _ibpr; }
// 11e CPU access. Shared and Managed textures accept replaceRegion:/getBytes: (Managed has no separate CPU copy here: both are the one
// DEVICE_LOCAL image reached through a host-visible staging buffer + a one-shot copy that is WAITED, so the data is in place when the call
// returns). Private textures are REFUSED (Metal does not allow CPU access to them): there is no NSError channel in these selectors, so the
// refusal is an RDNA4LOG line and a no-op. Rows are re-packed tightly into the staging buffer, so any bytesPerRow is accepted.
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (!bytes) { RDNA4LOG("replaceRegion: NULL bytes; refused"); return; }
    if ([self rdna4HostReadOnly]) { RDNA4LOG("replaceRegion: read-only backing; CPU write refused"); return; }
    if (!rdna4_region_ok(self, r, lvl, slice, "replaceRegion")) return;
    BOOL bc = rdna4_fmt_is_bc(_fmt);
    NSUInteger bw = bc ? (r.size.width + 3) / 4 : r.size.width;
    NSUInteger bh = bc ? (r.size.height + 3) / 4 : r.size.height;
    NSUInteger rowBytes = 0;
    if (__builtin_mul_overflow(bw, (NSUInteger)_fmt->bpp, &rowBytes)) return;
    if ((_is1D || _texel) && bpr < rowBytes) bpr = rowBytes;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(bw, bh, r.size.depth,
            _fmt->bpp, bpr, bpi, &transfer) ||
        !rdna4_texture_transfer_pointer(bytes, transfer.span)) {
        RDNA4LOG("replaceRegion: CPU row/image layout is too small or overflows; refused"); return;
    }
    if (_texel) {
        if (![_hostBuf contents]) { RDNA4LOG("replaceRegion: Private texture buffer; refused"); return; }
        memcpy((uint8_t *)[_hostBuf contents] + _hoff + r.origin.x * _fmt->bpp, bytes, rowBytes);
        return;
    }
    if ([self rdna4IsIOS]) {   // 11h.6: the IOSurface (or no-copy buffer) memory IS the CPU copy; the GPU copy (path b) is re-uploaded at the first use in each command buffer
        uint8_t *base = (uint8_t *)[self rdna4IBase]; size_t stride = [self rdna4IOSBytesPerRow];
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy(base + (r.origin.y + y) * stride + r.origin.x * _fmt->bpp, (const uint8_t *)bytes + y * bpr, rowBytes);
        RDNA4LOGR("replaceRegion: %lux%lu into the IOSurface memory", (unsigned long)r.size.width, (unsigned long)r.size.height); return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { RDNA4LOG("replaceRegion: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    NSUInteger nz = r.size.depth;
    NSError *err = nil;
    NSUInteger opts = MTLResourceStorageModeShared | RDNA4_OPT_NOZERO;
    RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:transfer.packed_bytes options:opts error:&err];
    if (!stg) { RDNA4LOG("replaceRegion: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < bh; y++)
        memcpy((uint8_t *)[stg contents] + (z * bh + y) * rowBytes, (const uint8_t *)bytes + z * bpi + y * bpr, rowBytes);
    BOOL ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [stg rdna4VkOffset], .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [self rdna4MipLevel:lvl], [self rdna4Slice:slice], 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyBufferToImage(cmd, [stg vkBuffer], [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }, NULL);
    RDNA4LOGR("replaceRegion: %lux%lux%lu at %lu,%lu,%lu level %lu (%s, storageMode %lu): %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)r.origin.x,
           (unsigned long)r.origin.y, (unsigned long)r.origin.z, (unsigned long)lvl, ok ? "uploaded" : "FAILED", (unsigned long)[self storageMode], ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr {
    NSUInteger bpi = 0;
    NSUInteger bh = rdna4_fmt_is_bc(_fmt) ? (r.size.height + 3) / 4 : r.size.height;
    if (r.size.depth > 1 && __builtin_mul_overflow(bpr, bh, &bpi)) {
        RDNA4LOG("replaceRegion: image stride overflows; refused"); return;
    }
    [self replaceRegion:r mipmapLevel:lvl slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:bpi];
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    if (!dst) { RDNA4LOG("getBytes: NULL destination; refused"); return; }
    if (!rdna4_region_ok(self, r, lvl, slice, "getBytes")) return;
    BOOL bc = rdna4_fmt_is_bc(_fmt);
    NSUInteger bw = bc ? (r.size.width + 3) / 4 : r.size.width;
    NSUInteger bh = bc ? (r.size.height + 3) / 4 : r.size.height;
    NSUInteger rowBytes = 0;
    if (__builtin_mul_overflow(bw, (NSUInteger)_fmt->bpp, &rowBytes)) return;
    if ((_is1D || _texel) && bpr < rowBytes) bpr = rowBytes;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(bw, bh, r.size.depth,
            _fmt->bpp, bpr, bpi, &transfer) ||
        !rdna4_texture_transfer_pointer(dst, transfer.span)) {
        RDNA4LOG("getBytes: CPU row/image layout is too small or overflows; refused"); return;
    }
    if (_texel) {
        if (![_hostBuf contents]) { RDNA4LOG("getBytes: Private texture buffer; refused"); return; }
        memcpy(dst, (const uint8_t *)[_hostBuf contents] + _hoff + r.origin.x * _fmt->bpp, rowBytes);
        return;
    }
    if ([self rdna4IsIOS]) {   // 11h.6: contents are current in the IOSurface / buffer memory once the writing command buffer has completed
        const uint8_t *base = [self rdna4IBase]; size_t stride = [self rdna4IOSBytesPerRow];
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy((uint8_t *)dst + y * bpr, base + (r.origin.y + y) * stride + r.origin.x * _fmt->bpp, rowBytes);
        return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { RDNA4LOG("getBytes: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    NSUInteger nz = r.size.depth;
    NSError *err = nil;
    RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:transfer.packed_bytes options:MTLResourceStorageModeShared error:&err];
    if (!stg) { RDNA4LOG("getBytes: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    VkImageLayout was = [self layout];
    BOOL ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [stg rdna4VkOffset], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [self rdna4MipLevel:lvl], [self rdna4Slice:slice], 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyImageToBuffer(cmd, [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [stg vkBuffer], 1, &bic);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) rdna4_tex_to(cmd, self, was);
    }, NULL);
    if (ok) for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < bh; y++)
        memcpy((uint8_t *)dst + z * bpi + y * bpr, (const uint8_t *)[stg contents] + (z * bh + y) * rowBytes, rowBytes);
    RDNA4LOGR("getBytes: %lux%lux%lu level %lu: %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)lvl, ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl {
    NSUInteger bpi = 0;
    NSUInteger bh = rdna4_fmt_is_bc(_fmt) ? (r.size.height + 3) / 4 : r.size.height;
    if (r.size.depth > 1 && __builtin_mul_overflow(bpr, bh, &bpi)) {
        RDNA4LOG("getBytes: image stride overflows; refused"); return;
    }
    [self getBytes:dst bytesPerRow:bpr bytesPerImage:bpi fromRegion:r mipmapLevel:lvl slice:0];
}
- (VkImage)vkImage { return _root ? [_root vkImage] : _img; }
- (VkImageView)vkView { return _view; }
- (uint32_t)rdna4Layers { return rdna4td_nlayers((unsigned)_layers); }
- (uint32_t)rdna4ImageLayers { return _root ? [_root rdna4ImageLayers] : rdna4td_nlayers((unsigned)_layers); }
- (uint32_t)rdna4Levels { return _root ? [_root rdna4Levels] : (uint32_t)(_levels ? _levels : 1); }
- (uint32_t)rdna4MipLevel:(NSUInteger)level { return _baseLevel + (uint32_t)level; }
- (uint32_t)rdna4Slice:(NSUInteger)slice { return rdna4td_base_layer(_is3D, (NSUInteger)_baseSlice + slice); }
- (unsigned)rdna4Aspects { return rdna4dp_barrier_aspect(_root ? [_root rdna4Aspects] : _aspects); }
- (BOOL)rdna4IsDS { return (_root ? [_root rdna4Aspects] : _aspects) != 0 && (_root ? [_root rdna4Aspects] : _aspects) != RDNA4DP_ASP_COLOR; }
- (BOOL)rdna4IsUInt { return _fmt != NULL && (_fmt->flags & RDNA4F_UINT) != 0; }
- (BOOL)rdna4IsSInt { return _fmt != NULL && (_fmt->flags & RDNA4F_SINT) != 0; }
- (unsigned)rdna4UIntBits { unsigned rgb = 16, a = 16; rdna4_int_bits(_pf, &rgb, &a); return rgb; }
- (void)rdna4IntBitsRGB:(unsigned *)rgb alpha:(unsigned *)alpha { unsigned r = 16, a = 16; rdna4_int_bits(_pf, &r, &a); if (rgb) *rgb = r; if (alpha) *alpha = a; }
- (BOOL)rdna4IsView { return _root != nil; }
- (NSUInteger)rdna4Samples { return _root ? 1 : (_samples ? _samples : 1); }
- (unsigned)rdna4CopyAspect { return [self rdna4IsDS] ? rdna4dp_copy_aspect([self rdna4Aspects]) : RDNA4DP_ASP_COLOR; }
- (VkFormat)vkFormat { return _fmt->vk; }
- (uint32_t)bytesPerPixel { return _fmt->bpp; }
- (VkImageLayout)layout { return _root ? [_root layout] : _iosLinear ? VK_IMAGE_LAYOUT_GENERAL : _layout; }
- (void)setLayout:(VkImageLayout)l { if (_root) [_root setLayout:l]; else if (!_iosLinear) _layout = l; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (MTLTextureType)textureType { return _texel ? MTLTextureTypeTextureBuffer : (MTLTextureType)rdna4td_mtl_type(_tdk); }
- (MTLPixelFormat)pixelFormat { return _pf; }
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (NSUInteger)depth { return _is3D ? _depth : 1; }
- (NSUInteger)mipmapLevelCount { return _levels ? _levels : 1; }
- (NSUInteger)sampleCount { return [self rdna4Samples]; }
- (NSUInteger)arrayLength { return rdna4td_array_length(_tdk, (unsigned)_layers); }
- (NSUInteger)usage { return _usage; }
- (BOOL)isFramebufferOnly { return NO; }
- (NSUInteger)firstMipmapInTail { return 0; }
- (NSUInteger)tailSizeInBytes { return 0; }
- (BOOL)isSparse { return NO; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (id)parentTexture { return _root; }
- (NSUInteger)parentRelativeLevel { return _root ? _baseLevel - _root->_baseLevel : 0; }
- (NSUInteger)parentRelativeSlice { return _parentSlice; }
- (id)buffer { return _root ? [_root buffer] : (_texelOwnedBuffer ? nil : _hostBuf); }
- (NSUInteger)bufferOffset { return _root ? [_root bufferOffset] : _hoff; }
- (NSUInteger)bufferBytesPerRow { return _root ? [_root bufferBytesPerRow] : (_hostBuf ? _ibpr : 0); }
- (IOSurfaceRef)iosurface { return _root ? [_root iosurface] : _ios; }
- (NSUInteger)iosurfacePlane { return _root ? [_root iosurfacePlane] : _iosPlane; }
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_root ? [_root allocatedSize] : _size); }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
// ---- gap census: MTLResource / MTLTexture surface sent by QuartzCore, SkyLight and Metal ----
RDNA4_RESOURCE_SPI
- (id)rootResource { return _root ? [_root rootResource] : self; }
- (BOOL)isShareable { return NO; }
- (id)newSharedTextureHandle { RDNA4_ONCE("newSharedTextureHandle: shared texture handles are not implemented; nil"); return nil; }
- (id)remoteStorageTexture { return nil; }
- (id)newRemoteTextureViewForDevice:(id)d { (void)d; return nil; }
- (MTLTextureSwizzleChannels)swizzle { return _swz; }
- (BOOL)isCompressed { return rdna4_fmt_is_bc(_fmt); }
- (BOOL)isDrawable { return NO; }
- (void)didModifyData {}
- (BOOL)canGenerateMipmapLevels { return _levels > 1; }
- (void)generateMipmapLevel:(NSUInteger)l slice:(NSUInteger)s { (void)l; (void)s; }
- (uint32_t)swizzleKey { return rdna4swz_key(rdna4_swz_from_mtl(_swz)); }
- (NSUInteger)numFaces { return 1; }
- (NSUInteger)resourceIndex { return 0; }
- (void *)virtualAddress { return [self rdna4IsIOS] ? (void *)[self rdna4IBase] : NULL; }
// Views expose mip and slice ranges of 2D, array and cube textures, with optional format reinterpretation and swizzle.
// A format change requires MTLTextureUsagePixelFormatView; host coherency tracks the shared root, not each view.
- (id)rdna4View:(MTLPixelFormat)pf type:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    NSUInteger count = [self mipmapLevelCount];
    if (!lv.length || lv.location >= count || lv.length > count - lv.location) {
        RDNA4LOG("newTextureView: type %lu levels %lu+%lu slices %lu+%lu: mip range outside the parent; nil", (unsigned long)tt, (unsigned long)lv.location,
               (unsigned long)lv.length, (unsigned long)sl.location, (unsigned long)sl.length); return nil; }
    NSError *e = nil; RDNA4Texture *v = [[RDNA4Texture alloc] initViewOf:self pixelFormat:pf type:tt levels:lv slices:sl swizzle:sw error:&e];
    if (!v) RDNA4LOG("newTextureView: refused: %s", e.localizedDescription.UTF8String);
    return v;
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf {
    return [self rdna4View:pf type:[self textureType] levels:NSMakeRange(0, [self mipmapLevelCount]) slices:NSMakeRange(0, [self rdna4Layers]) swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl {
    return [self rdna4View:pf type:tt levels:lv slices:sl swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    return [self rdna4View:pf type:tt levels:lv slices:sl swizzle:sw];
}
- (instancetype)initViewOf:(RDNA4Texture *)root pixelFormat:(MTLPixelFormat)pf type:(MTLTextureType)type
                     levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define VFAIL(code, ...) do { if (err) *err = rdna4_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    if (!root) VFAIL(92, @"a view needs a parent texture");
    if ([root rdna4IsTexel]) VFAIL(98, @"texture buffers do not support image views");
    if (root->_samples > 1) VFAIL(100, @"views of multisample textures are not implemented");
    rdna4td_view_t view;
    if (!rdna4td_view_map(root->_tdk, [root rdna4Layers], (unsigned long)type, (unsigned long)slices.location, (unsigned long)slices.length, &view))
        VFAIL(98, @"texture type %lu with slice range %lu+%lu is not a compatible view of type %lu", (unsigned long)root.textureType,
              (unsigned long)slices.location, (unsigned long)slices.length, (unsigned long)type);
    RDNA4Texture *imageRoot = root; while (imageRoot->_root) imageRoot = imageRoot->_root;
    if ((view.kind == RDNA4TD_K_CUBE || view.kind == RDNA4TD_K_CUBEARRAY) && !(imageRoot->_imageFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
        VFAIL(98, @"the parent image was not created as cube-compatible");
    if (view.kind == RDNA4TD_K_CUBEARRAY && !RDNA4R.cubeArray) VFAIL(98, @"RADV does not support cube-array image views");
    const RDNA4Fmt *f = rdna4_fmt(pf);
    unsigned viewAspects = root->_aspects;
    if (root->_aspects) {   // bundle 10 + Metal 2 F29: Depth32Float_Stencil8 allows X32_Stencil8 sampling views; other DS reinterpretation stays refused
        if (root->_pf == MTLPixelFormatDepth32Float_Stencil8 && pf == MTLPixelFormatX32_Stencil8) {
            f = root->_fmt;
            viewAspects = RDNA4DP_ASP_STENCIL;
        } else if (pf != root->_pf) {
            VFAIL(101, @"pixel-format views of a depth/stencil texture (%lu -> %lu) are not supported", (unsigned long)root->_pf, (unsigned long)pf);
        } else {
            f = root->_fmt;
        }
    }
    if (!f) VFAIL(94, @"view pixel format %lu not supported", (unsigned long)pf);
    if (!root->_aspects && pf != root->_pf) {
        if (rdna4_fmt_is_bc(f) != rdna4_fmt_is_bc(root->_fmt)) VFAIL(95, @"cannot reinterpret between block-compressed and uncompressed formats (%lu -> %lu)", (unsigned long)root->_pf, (unsigned long)pf);
        if (f->bpp != root->_fmt->bpp) VFAIL(95, @"view pixel format %lu has %u bytes per pixel, the texture has %u", (unsigned long)pf, f->bpp, root->_fmt->bpp);
        if (!(root->_usage & MTLTextureUsagePixelFormatView)) VFAIL(96, @"the texture was not created with MTLTextureUsagePixelFormatView");
    }
    _root = root; _baseLevel = [root rdna4MipLevel:levels.location]; _baseSlice = [root rdna4Slice:slices.location]; _parentSlice = (uint32_t)slices.location; _dev = root->_dev;
    _imageFlags = imageRoot->_imageFlags; _tdk = view.kind; _layers = view.layersIvar;
    _w = MAX((NSUInteger)1, root->_w >> levels.location); _h = MAX((NSUInteger)1, root->_h >> levels.location);
    _pf = pf; _fmt = f; _usage = root->_usage; _opts = root->_opts;
    rdna4swz_t composed;
    if (!rdna4swz_compose(rdna4_swz_from_mtl(root->_swz), rdna4_swz_from_mtl(swz), &composed))
        VFAIL(97, @"bad swizzle");
    const char *swzWhy = NULL;
    if (!rdna4swz_check(composed, (unsigned long)root->_usage, 0, 1, &swzWhy))
        VFAIL(97, @"%s", swzWhy);
    _swz = rdna4_swz_to_mtl(composed);
    if (root->_is3D) VFAIL(99, @"views of 3D textures are not implemented");
    VkComponentMapping vcm = rdna4_view_map(f, _swz);   // composes the client swizzle with the format's own (A8: (0,0,0,R))
    // Attachments and storage images require identity mapping, including views.
    _levels = levels.length; _depth = 1; _aspects = viewAspects; _viewSwz = !rdna4_is_identity_map(&vcm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [root vkImage], .viewType = (VkImageViewType)view.viewType, .format = f->vk,
        .components = vcm, .subresourceRange = { viewAspects ? rdna4dp_view_aspect(viewAspects) : VK_IMAGE_ASPECT_COLOR_BIT,
            _baseLevel, (uint32_t)_levels, _baseSlice, rdna4td_nlayers(view.layersIvar) } };
    VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) VFAIL(98, @"vkCreateImageView = %d", r);
    [[self rdna4ResourceHeap] rdna4TrackResource:self];
    RDNA4LOGR("RDNA4Texture view %p of %p: pf %lu -> %lu (vk format %d -> %d), swizzle %u%u%u%u", (__bridge void *)self, (__bridge void *)root, (unsigned long)root->_pf, (unsigned long)pf, root->_fmt->vk, f->vk,
           swz.red, swz.green, swz.blue, swz.alpha);
    #undef VFAIL
    return self;
}
@end

