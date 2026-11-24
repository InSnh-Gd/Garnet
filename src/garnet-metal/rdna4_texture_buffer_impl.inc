// Texture buffers share their VkBuffer storage directly; they never enter the image-copy path.
- (instancetype)initTextureBufferWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d
    buffer:(RDNA4Buffer *)buffer offset:(NSUInteger)off error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define TBFAIL(...) do { if (err) *err = rdna4_err(131, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    const RDNA4Fmt *format = rdna4_fmt(d.pixelFormat);
    NSUInteger bytes;
    if (!d || d.textureType != MTLTextureTypeTextureBuffer || !d.width || d.height != 1 ||
        d.depth != 1 || d.arrayLength != 1 || d.sampleCount != 1 || d.mipmapLevelCount != 1 ||
        (d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)) ||
        !format || (format->flags & (RDNA4F_A8 | RDNA4F_BC)) ||
        __builtin_mul_overflow(d.width, (NSUInteger)format->bpp, &bytes))
        TBFAIL(@"invalid texture-buffer geometry, format, or usage");
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, RDNA4SWZ_FMT_TEXEL, 0, &swzWhy))
            TBFAIL(@"%s", swzWhy);
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [buffer device] != dev ||
        [buffer isAliasable] || !rdna4_buffer_range(buffer.length, off, bytes)))
        TBFAIL(@"texture-buffer backing is foreign, aliasable, or too small");
    if (buffer && [buffer rdna4HostReadOnly] && (d.usage & MTLTextureUsageShaderWrite))
        TBFAIL(@"texture-buffer write usage requires writable backing");
    if (!rdna4_radv_open(err)) return nil;
    if (!RDNA4R.robustTexelBuffers) TBFAIL(@"texture buffers require robustBufferAccess2 for defined out-of-bounds access");
    VkFormatProperties features = {0};
    vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, format->vk, &features);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
        ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : 0);
    uint64_t absolute;
    if ((features.bufferFeatures & required) != required || !vkCreateBufferView || !vkDestroyBufferView ||
        !RDNA4R.lim.maxTexelBufferElements || d.width > RDNA4R.lim.maxTexelBufferElements ||
        __builtin_add_overflow((uint64_t)[buffer rdna4VkOffset], (uint64_t)off, &absolute) ||
        absolute > UINT64_MAX - bytes ||
        off % 256 || absolute % format->bpp ||
        (RDNA4R.lim.minTexelBufferOffsetAlignment && absolute % RDNA4R.lim.minTexelBufferOffsetAlignment))
        TBFAIL(@"texture-buffer format, size, or offset exceeds Vulkan requirements");
    if (!buffer) {
        NSUInteger options = ((NSUInteger)d.storageMode << 4) | d.cpuCacheMode |
            ((NSUInteger)d.hazardTrackingMode << 8);
        buffer = [[RDNA4Buffer alloc] initWithDevice:dev length:bytes options:options error:err];
        if (!buffer) return nil;
        absolute = [buffer rdna4VkOffset]; _texelOwnedBuffer = YES;
        if (absolute > UINT64_MAX - bytes || absolute % format->bpp ||
            (RDNA4R.lim.minTexelBufferOffsetAlignment && absolute % RDNA4R.lim.minTexelBufferOffsetAlignment))
            TBFAIL(@"allocated texture-buffer backing has an invalid Vulkan offset");
    }
    VkBufferViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = [buffer vkBuffer], .format = format->vk, .offset = absolute, .range = bytes};
    VkResult result = vkCreateBufferView(RDNA4R.dev, &info, NULL, &_texelView);
    if (result != VK_SUCCESS) { _texelView = VK_NULL_HANDLE; TBFAIL(@"vkCreateBufferView = %d", result); }
    _texel = YES; _dev = dev; _hostBuf = buffer; _hoff = off;
    _texelStorage = (features.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0 &&
        (!d.usage || (d.usage & MTLTextureUsageShaderWrite));
    _w = d.width; _h = _depth = _levels = 1; _pf = d.pixelFormat; _fmt = format;
    _usage = d.usage; _opts = [buffer resourceOptions]; _size = [buffer allocatedSize];
    _swz = (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
        MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    #undef TBFAIL
    return self;
}
- (BOOL)rdna4IsTexel { return _texel; }
- (VkBufferView)rdna4TexelView { return _texelView; }
- (RDNA4Buffer *)rdna4TexelBuffer { return _texel ? _hostBuf : nil; }
- (BOOL)rdna4TexelWritable { return _texelStorage && ![self rdna4HostReadOnly]; }
