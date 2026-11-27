// Shared fixed render target, guarded argument storage and authored AIR capture.
#ifndef RDNA4_TEST_ARGUMENT_RENDER_SUPPORT_H
#define RDNA4_TEST_ARGUMENT_RENDER_SUPPORT_H

enum {
    kTargetSize = 8,
    kViewportOrigin = 2,
    kViewportSize = 4,
    kTextureSize = 8,
    kMipLevels = 4,
};

static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kArgumentTail = 256;
static const NSUInteger kControlOffset = 32;
static const NSUInteger kControlTail = 64;
static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kControlGuard = 0x5a;
static const float kTargetGuard = 99.0f;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, RDNA4_ARGUMENT_TEST_PREFIX "_FAIL %s\n",
            message.UTF8String);
        exit(1);
    }
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"argument encoder alignment is zero");
    return (value + alignment - 1) / alignment * alignment;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length,
                                     uint8_t fill, NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length
        options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
        [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSUInteger offset,
                         NSUInteger length, uint8_t guard,
                         NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        length <= buffer.length - offset,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = 0; i < offset; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ prefix changed at %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = offset + length; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ suffix changed at %lu", description,
                (unsigned long)i]);
}

static inline id<MTLTexture> newMipTexture(id<MTLDevice> device, uint32_t base,
                                    NSString *description) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:kTextureSize height:kTextureSize
            mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil && texture.mipmapLevelCount == kMipLevels,
        [description stringByAppendingString:@" unavailable"]);
    float texels[kTextureSize * kTextureSize * 4];
    for (NSUInteger level = 0; level < kMipLevels; ++level) {
        NSUInteger size = kTextureSize >> level;
        float value = (float)(base + level);
        for (NSUInteger i = 0; i < size * size * 4; ++i)
            texels[i] = value;
        [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
            mipmapLevel:level withBytes:texels
            bytesPerRow:size * 4 * sizeof(float)];
    }
    return texture;
}

static id<MTLTexture> newTarget(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:kTargetSize height:kTargetSize
            mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    require(target != nil, @"quad margin target unavailable");
    return target;
}

static inline id<MTLSamplerState> newNearestSampler(id<MTLDevice> device,
                                              float maxLod,
                                              NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.lodMinClamp = 0.0f;
    descriptor.lodMaxClamp = maxLod;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil,
        [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static id<MTLRenderPipelineState> newPipeline(id<MTLDevice> device,
                                               id<MTLFunction> vertex,
                                               id<MTLFunction> fragment,
                                               NSString *description,
                                               NSError **error) {
    MTLRenderPipelineDescriptor *descriptor =
        [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    NSError *failure = error ? *error : nil;
    require(pipeline != nil, failure.description ?: description);
    return pipeline;
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
    printf(RDNA4_ARGUMENT_TEST_PREFIX "_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String,
        (unsigned long)air.length);
}

#endif
