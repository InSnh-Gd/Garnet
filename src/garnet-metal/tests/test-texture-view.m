// The same Metal view/clear/readback path runs on the reference and VM backend.
// RDNA4_VIEW_AUDIT instead checks the real RADV framebuffer contract, without GPU work.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <math.h>
#include <unistd.h>

#ifdef RDNA4_VIEW_AUDIT
extern unsigned rdna4_audit_invalid_attachments(void);
extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_image_view_count(void);
extern BOOL rdna4_audit_image_view(unsigned index, uint32_t fields[5]);
@interface NSObject (RDNA4ViewAudit)
- (BOOL)rdna4Finish:(NSError **)error;
@end
#endif

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "TEXTURE_VIEW_FAIL %s\n", message.UTF8String);
    _exit(1);
}

#ifdef RDNA4_VIEW_AUDIT
enum { RDNA4VkImageViewType2D = 1, RDNA4VkImageViewTypeCube = 3,
       RDNA4VkImageViewType2DArray = 5, RDNA4VkImageViewTypeCubeArray = 6 };

static void require_image_view(uint32_t type, uint32_t baseMip, uint32_t mipCount,
                               uint32_t baseLayer, uint32_t layerCount, NSString *label) {
    const uint32_t expected[] = { type, baseMip, mipCount, baseLayer, layerCount };
    uint32_t actual[5];
    for (unsigned i = 0; i < rdna4_audit_image_view_count(); ++i)
        if (rdna4_audit_image_view(i, actual) && memcmp(actual, expected, sizeof expected) == 0) return;
    fail([NSString stringWithFormat:@"%@ missing vkCreateImageView type=%u mip=%u+%u layer=%u+%u",
          label, type, baseMip, mipCount, baseLayer, layerCount]);
}
#endif

static void run(id<MTLCommandBuffer> commands) {
    if (!commands) fail(@"command buffer unavailable");
#ifdef RDNA4_VIEW_AUDIT
    NSError *error = nil;
    if (![(id)commands rdna4Finish:&error] || error || rdna4_audit_invalid_attachments() || rdna4_audit_submission_count())
        fail([NSString stringWithFormat:@"invalid attachments=%u submissions=%u error=%@",
              rdna4_audit_invalid_attachments(), rdna4_audit_submission_count(), error]);
#else
    [commands commit]; [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error) fail(commands.error.description);
#endif
}

static void run_rejected(id<MTLCommandBuffer> commands) {
#ifdef RDNA4_VIEW_AUDIT
    NSError *error = nil;
    if ([(id)commands rdna4Finish:&error] || !error || rdna4_audit_submission_count()) fail(@"invalid or overlapping texture aliases accepted");
#else
    [commands commit]; [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusError || !commands.error) fail(@"invalid or overlapping texture aliases accepted");
#endif
}

static void clear_level(id<MTLCommandQueue> queue, id<MTLTexture> texture, NSUInteger level, double red) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].level = level;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red / 255, 0, 0, 1);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) fail(@"mip render encoder unavailable");
    [encoder endEncoding];
    run(commands);
}

static void clear_slice(id<MTLCommandQueue> queue, id<MTLTexture> texture, NSUInteger level,
                        NSUInteger slice, double red) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].level = level;
    pass.colorAttachments[0].slice = slice;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red / 255, 0, 0, 1);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) fail(@"layered texture render encoder unavailable");
    [encoder endEncoding];
    run(commands);
}

#ifndef RDNA4_VIEW_AUDIT
static void check_sampled_view(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_view(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]], "
        "uint i [[thread_position_in_grid]]) { constexpr sampler s(filter::nearest, mip_filter::nearest); "
        "out[i] = t.sample(s, float2(0.5), level(float(i))); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_view"] error:&error];
    if (!pipeline) fail([NSString stringWithFormat:@"mip sampling pipeline: %@", error]);
    id<MTLBuffer> output = [device newBufferWithLength:3 * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    if (!output) fail(@"mip sampling buffer unavailable");
    memset(output.contents, 0xA5, output.length);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(3,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];
    run(commands);
    const float *values = output.contents;
    for (NSUInteger level = 0; level < 3; ++level) {
        float red = (level == 2 ? 64.0f : 192.0f) / 255.0f;
        if (!(fabsf(values[level * 4] - red) <= 0.00001f && values[level * 4 + 1] == 0 &&
            values[level * 4 + 2] == 0 && values[level * 4 + 3] == 1))
            fail([NSString stringWithFormat:@"sampled view mip %lu RGBA=%g,%g,%g,%g", (unsigned long)level,
                values[level * 4], values[level * 4 + 1], values[level * 4 + 2], values[level * 4 + 3]]);
    }
}

static void check_level_slice(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                              NSUInteger level, NSUInteger slice, uint8_t red) {
    NSUInteger width = MAX((NSUInteger)1, texture.width >> level);
    NSUInteger height = MAX((NSUInteger)1, texture.height >> level);
    id<MTLBuffer> result = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
    if (!result) fail(@"mip readback buffer unavailable");
    memset(result.contents, 0xA5, result.length);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) fail(@"mip readback encoder unavailable");
    [blit copyFromTexture:texture sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:result destinationOffset:0
        destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
    [blit endEncoding];
    run(commands);
    const uint8_t *bytes = result.contents;
    for (NSUInteger pixel = 0; pixel < width * height; ++pixel)
        if (bytes[pixel * 4] || bytes[pixel * 4 + 1] || bytes[pixel * 4 + 2] != red || bytes[pixel * 4 + 3] != 255)
            fail([NSString stringWithFormat:@"mip %lu slice %lu pixel %lu BGRA=%u,%u,%u,%u expected 0,0,%u,255",
                (unsigned long)level, (unsigned long)slice, (unsigned long)pixel, bytes[pixel * 4], bytes[pixel * 4 + 1],
                bytes[pixel * 4 + 2], bytes[pixel * 4 + 3], red]);
}

static void check_level(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                        NSUInteger level, uint8_t red) {
    check_level_slice(device, queue, texture, level, 0, red);
}

static void check_array_sampling(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                                 const uint8_t *expected, NSUInteger count) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_array(texture2d_array<float> t [[texture(0)]], device float4 *out [[buffer(0)]], "
        "uint i [[thread_position_in_grid]]) { constexpr sampler s(filter::nearest); "
        "out[i] = t.sample(s, float2(0.5), i); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"sample_array"];
    id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) fail([NSString stringWithFormat:@"array sampling pipeline: %@", error]);
    id<MTLBuffer> output = [device newBufferWithLength:count * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    if (!output) fail(@"array sampling output unavailable");
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];
    run(commands);
    const float *values = output.contents;
    for (NSUInteger i = 0; i < count; ++i)
        if (fabsf(values[i * 4] - expected[i] / 255.0f) > 0.00001f || values[i * 4 + 1] != 0 ||
            values[i * 4 + 2] != 0 || values[i * 4 + 3] != 1)
            fail([NSString stringWithFormat:@"array sample %lu RGBA=%g,%g,%g,%g expected red=%u",
                (unsigned long)i, values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3], expected[i]]);
}
#endif

static void test_host_views(id<MTLDevice> device, id<MTLCommandQueue> queue, BOOL reference) {
#ifdef RDNA4_VIEW_AUDIT
    (void)reference;
#endif
    for (NSUInteger kind = 0; kind < 2; ++kind) {
        IOSurfaceRef surface = NULL;
        id<MTLBuffer> backing = nil;
        const NSUInteger width = 64, height = 16, stride = 512, offset = kind ? 0 : 512;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        descriptor.storageMode = kind ? MTLStorageModeManaged : MTLStorageModeShared;
        id<MTLTexture> root;
        uint8_t *host;
        if (kind) {
            surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
                (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
                (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(stride),
                (id)kIOSurfaceAllocSize: @(stride * height), (id)kIOSurfacePixelFormat: @((uint32_t)0x42475241) });
            if (!surface) fail(@"host view IOSurface allocation");
            host = IOSurfaceGetBaseAddress(surface);
            root = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        } else {
            backing = [device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
            host = (uint8_t *)backing.contents + offset;
            root = [backing newTextureWithDescriptor:descriptor offset:offset bytesPerRow:stride];
        }
        if (!root || !host) fail(@"host view root unavailable");
        memset(host, 0xA5, stride * height);
        for (NSUInteger y = 0; y < height; ++y)
            for (NSUInteger x = 0; x < width; ++x) ((uint32_t *)(host + y * stride))[x] = 0xFF100000;
        id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat];
        MTLTextureSwizzleChannels swizzle = { MTLTextureSwizzleBlue, MTLTextureSwizzleGreen,
            MTLTextureSwizzleRed, MTLTextureSwizzleAlpha };
        id<MTLTexture> nested = [view newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
            levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:swizzle];
        if (!view || !nested) fail([NSString stringWithFormat:@"%@ texture view unavailable", kind ? @"IOSurface" : @"buffer-backed"]);
        if (view.parentTexture != root || nested.parentTexture != view || nested.iosurface != surface ||
            nested.buffer != backing || (!kind && (nested.bufferOffset != offset || nested.bufferBytesPerRow != stride)))
            fail(@"host view backing metadata mismatch");
#ifndef RDNA4_VIEW_AUDIT
        check_level(device, queue, nested, 0, 16);
        uint32_t cpu[64 * 16];
        for (NSUInteger i = 0; i < width * height; ++i) cpu[i] = 0xFF200000;
        [nested replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:cpu bytesPerRow:width * 4];
        check_level(device, queue, root, 0, 32);
        uint32_t upload[32 * 16];
        for (NSUInteger i = 0; i < 32 * height; ++i) upload[i] = 0xFF400000;
        id<MTLBuffer> left = [device newBufferWithBytes:upload length:sizeof upload options:MTLResourceStorageModeShared];
        for (NSUInteger i = 0; i < 32 * height; ++i) upload[i] = 0xFF800000;
        id<MTLBuffer> right = [device newBufferWithBytes:upload length:sizeof upload options:MTLResourceStorageModeShared];
        id<MTLBuffer> result = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
        if (!left || !right || !result) fail(@"host view copy buffers unavailable");
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromBuffer:left sourceOffset:0 sourceBytesPerRow:128 sourceBytesPerImage:sizeof upload
            sourceSize:MTLSizeMake(32,height,1) toTexture:root destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit copyFromBuffer:right sourceOffset:0 sourceBytesPerRow:128 sourceBytesPerImage:sizeof upload
            sourceSize:MTLSizeMake(32,height,1) toTexture:view destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(32,0,0)];
        [blit copyFromTexture:nested sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(width,height,1) toBuffer:result destinationOffset:0
            destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
        [blit endEncoding];
        run(commands);
        for (NSUInteger y = 0; y < height; ++y)
            for (NSUInteger x = 0; x < width; ++x)
                if (((uint32_t *)result.contents)[y * width + x] != (x < 32 ? 0xFF400000 : 0xFF800000))
                    fail(@"root/view/nested partial writes lost within one command buffer");
#endif
        clear_level(queue, nested, 0, 192);
#ifndef RDNA4_VIEW_AUDIT
        check_level(device, queue, root, 0, 192);
        uint32_t readback[64 * 16];
        [view getBytes:readback bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        for (NSUInteger i = 0; i < width * height; ++i)
            if (readback[i] != 0xFFC00000) fail(@"host view CPU readback mismatch");
#endif
        id<MTLTexture> srgb = [view newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
        if (!srgb || srgb.parentTexture != view || srgb.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB)
            fail(@"host sRGB format view unavailable");
        clear_level(queue, srgb, 0, 127.5);
#ifndef RDNA4_VIEW_AUDIT
        check_level(device, queue, root, 0, 188);
        for (NSUInteger y = 0; y < height; ++y)
            for (NSUInteger byte = width * 4; byte < stride; ++byte)
                if (host[y * stride + byte] != 0xA5) fail(@"host view overwrote row padding");
        printf("TEXTURE_HOST_VIEW_PIXEL_PASS reference=%d kind=%s nested metadata CPU-access alias-writes clear sRGB padding exact\n",
            reference, kind ? "IOSurface" : "buffer");
#else
        printf("TEXTURE_HOST_VIEW_AUDIT_PASS kind=%s nested metadata attachments sRGB submissions=0; pixels untested\n", kind ? "IOSurface" : "buffer");
#endif
        if (surface) CFRelease(surface);
    }
}

static void test_mip_views(id<MTLDevice> device, id<MTLCommandQueue> queue, BOOL reference) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:16 mipmapped:YES];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> root = [device newTextureWithDescriptor:descriptor];
    id<MTLTexture> whole = [root newTextureViewWithPixelFormat:root.pixelFormat];
    if (!root || !whole || whole.mipmapLevelCount != root.mipmapLevelCount)
        fail(@"whole-format view must expose every parent mip");
    id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(1, 3) slices:NSMakeRange(0, 1)];
    id<MTLTexture> nested = [view newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)];
    if (!view || !nested || view.width != 16 || view.height != 8 || view.mipmapLevelCount != 3 ||
        view.parentTexture != root || view.parentRelativeLevel != 1 || nested.width != 8 || nested.height != 4 ||
        nested.mipmapLevelCount != 1 || nested.parentTexture != view || nested.parentRelativeLevel != 1)
        fail(@"mip or nested view metadata mismatch");
    for (NSUInteger level = 0; level < root.mipmapLevelCount; ++level) clear_level(queue, root, level, 16 + level);
    clear_level(queue, view, 0, 64);
    clear_level(queue, nested, 0, 96);
    clear_level(queue, view, 2, 128);
#ifndef RDNA4_VIEW_AUDIT
    for (NSUInteger level = 0; level < root.mipmapLevelCount; ++level)
        check_level(device, queue, root, level, level == 1 ? 64 : level == 2 ? 96 : level == 3 ? 128 : 16 + level);
    check_level(device, queue, view, 0, 64);
    check_level(device, queue, nested, 0, 96);
#endif
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit generateMipmapsForTexture:view];
    [blit endEncoding];
    run(commands);
#ifndef RDNA4_VIEW_AUDIT
    for (NSUInteger level = 0; level < root.mipmapLevelCount; ++level)
        check_level(device, queue, root, level, level >= 1 && level <= 3 ? 64 : 16 + level);
#endif
    MTLTextureDescriptor *destinationDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:root.pixelFormat width:64 height:32 mipmapped:YES];
    destinationDescriptor.usage = descriptor.usage;
    destinationDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> destinationRoot = [device newTextureWithDescriptor:destinationDescriptor];
    id<MTLTexture> destinationView = [destinationRoot newTextureViewWithPixelFormat:root.pixelFormat
        textureType:MTLTextureType2D levels:NSMakeRange(2, 3) slices:NSMakeRange(0, 1)];
    if (!destinationRoot || !destinationView) fail(@"mip copy destination unavailable");
    for (NSUInteger level = 0; level < destinationRoot.mipmapLevelCount; ++level)
        clear_level(queue, destinationRoot, level, 32 + level);
    uint32_t pixels[16 * 8];
    for (NSUInteger pixel = 0; pixel < sizeof pixels / sizeof *pixels; ++pixel) pixels[pixel] = 0xFFC00000;
    id<MTLBuffer> upload = [device newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared];
    if (!upload) fail(@"mip upload buffer unavailable");
    commands = [queue commandBuffer];
    blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:16 * 4 sourceBytesPerImage:sizeof pixels
        sourceSize:MTLSizeMake(16,8,1) toTexture:view destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,4,1)
        toTexture:nested destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit copyFromTexture:view sourceSlice:0 sourceLevel:0 toTexture:destinationView
        destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:3];
    [blit endEncoding];
    run(commands);
#ifndef RDNA4_VIEW_AUDIT
    check_level(device, queue, root, 1, 192);
    check_level(device, queue, root, 2, 192);
    for (NSUInteger level = 0; level < destinationRoot.mipmapLevelCount; ++level)
        check_level(device, queue, destinationRoot, level, level == 2 || level == 3 ? 192 : level == 4 ? 64 : 32 + level);
    check_sampled_view(device, queue, view);
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> cpuRoot = [device newTextureWithDescriptor:descriptor];
    id<MTLTexture> cpuView = [cpuRoot newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(1, 3) slices:NSMakeRange(0, 1)];
    id<MTLTexture> cpuNested = [cpuView newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)];
    if (!cpuRoot || !cpuView || !cpuNested) fail(@"CPU mip view unavailable");
    [cpuNested replaceRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0 withBytes:pixels bytesPerRow:8 * 4];
    uint32_t readback[8 * 4];
    memset(readback, 0xA5, sizeof readback);
    [cpuView getBytes:readback bytesPerRow:8 * 4 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:1];
    if (memcmp(readback, pixels, sizeof readback)) fail(@"CPU mip view readback mismatch");
    check_level(device, queue, cpuRoot, 2, 192);
#endif
    if (!reference) {
        const NSRange invalid[] = { { root.mipmapLevelCount, 1 }, { 0, 0 }, { 1, NSUIntegerMax }, { NSUIntegerMax, 1 } };
        for (NSUInteger i = 0; i < sizeof invalid / sizeof *invalid; ++i)
            if ([root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
                    levels:invalid[i] slices:NSMakeRange(0, 1)]) fail(@"invalid mip range accepted");
        for (NSUInteger rangeCopy = 0; rangeCopy < 2; ++rangeCopy) {
            commands = [queue commandBuffer];
            blit = [commands blitCommandEncoder];
            if (rangeCopy) [blit copyFromTexture:view sourceSlice:0 sourceLevel:1 toTexture:nested
                destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
            else [blit copyFromTexture:view sourceSlice:0 sourceLevel:1 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(8,4,1) toTexture:nested destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [blit endEncoding];
            run_rejected(commands);
        }
    }
#ifdef RDNA4_VIEW_AUDIT
    require_image_view(RDNA4VkImageViewType2D, 1, 3, 0, 1, @"mip view");
    require_image_view(RDNA4VkImageViewType2D, 2, 1, 0, 1, @"nested mip view");
    puts("TEXTURE_MIP_VIEW_AUDIT_PASS whole nested attachments mipmap-generation buffer-copy texture-copy alias-rejection; pixels untested");
#else
    printf("TEXTURE_MIP_VIEW_PIXEL_PASS reference=%d whole nested attachments mipmap-generation buffer-copy texture-copy CPU-access sampling exact\n", reference);
#endif
}

static void test_layered_views(id<MTLDevice> device, id<MTLCommandQueue> queue, BOOL reference) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16 height:16 mipmapped:YES];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 6;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> root = [device newTextureWithDescriptor:descriptor];
    if (!root) fail(@"2D-array root unavailable");
    MTLTextureDescriptor *plainDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
    plainDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    plainDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> plain = [device newTextureWithDescriptor:plainDescriptor];
    id<MTLTexture> plainArray = [plain newTextureViewWithPixelFormat:plain.pixelFormat textureType:MTLTextureType2DArray
        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
    if (!plainArray || plainArray.textureType != MTLTextureType2DArray || plainArray.arrayLength != 1)
        fail(@"2D to one-slice array view conversion failed");
    id<MTLTexture> whole = [root newTextureViewWithPixelFormat:root.pixelFormat];
    if (!whole || whole.textureType != MTLTextureType2DArray || whole.arrayLength != 6 ||
        whole.mipmapLevelCount != root.mipmapLevelCount)
        fail(@"TEXTURE_LAYERED_VIEW_FAIL whole array-format view metadata mismatch");
    id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2DArray
        levels:NSMakeRange(1, 3) slices:NSMakeRange(1, 4)];
    id<MTLTexture> nested = [view newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(1, 1) slices:NSMakeRange(2, 1)];
    if (!view || !nested || view.textureType != MTLTextureType2DArray || view.arrayLength != 4 ||
        view.width != 8 || view.height != 8 || view.mipmapLevelCount != 3 || view.parentRelativeLevel != 1 ||
        view.parentRelativeSlice != 1 || nested.textureType != MTLTextureType2D || nested.width != 4 ||
        nested.height != 4 || nested.mipmapLevelCount != 1 || nested.parentTexture != view ||
        nested.parentRelativeLevel != 1 || nested.parentRelativeSlice != 2)
        fail(@"TEXTURE_LAYERED_VIEW_FAIL array or nested slice-view metadata mismatch");

    id<MTLTexture> cubeFromArray = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureTypeCube
        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 6)];
    id<MTLTexture> arrayCubeFace = [cubeFromArray newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(0, 1) slices:NSMakeRange(4, 1)];
    if (!cubeFromArray || cubeFromArray.textureType != MTLTextureTypeCube || cubeFromArray.parentRelativeSlice != 0 ||
        !arrayCubeFace || arrayCubeFace.parentRelativeSlice != 4)
        fail(@"square array to cube and nested face conversion failed");
    for (NSUInteger slice = 0; slice < 6; ++slice) clear_slice(queue, root, 0, slice, 64);
    clear_slice(queue, arrayCubeFace, 0, 0, 128);
#ifndef RDNA4_VIEW_AUDIT
    check_level_slice(device, queue, root, 0, 4, 128);
    check_level_slice(device, queue, root, 0, 3, 64);
#endif

    clear_slice(queue, root, 2, 0, 64);
    clear_slice(queue, root, 2, 5, 192);
    const uint8_t layerRed[] = { 64, 96, 128, 160 };
    for (NSUInteger slice = 0; slice < 4; ++slice) clear_slice(queue, view, 0, slice, layerRed[slice]);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit generateMipmapsForTexture:view];
    [blit endEncoding];
    run(commands);
#ifndef RDNA4_VIEW_AUDIT
    check_level_slice(device, queue, root, 2, 0, 64);
    check_level_slice(device, queue, root, 2, 1, 64);
    check_level_slice(device, queue, root, 2, 4, 160);
    check_level_slice(device, queue, root, 2, 5, 192);
    check_array_sampling(device, queue, view, layerRed, 4);
#endif
    clear_slice(queue, nested, 0, 0, 192);
#ifndef RDNA4_VIEW_AUDIT
    check_level_slice(device, queue, root, 2, 3, 192);
    check_level_slice(device, queue, root, 1, 2, 96);
    uint32_t blitPixels[8 * 8], blitResult[8 * 8];
    for (NSUInteger i = 0; i < 8 * 8; ++i) blitPixels[i] = 0xFFC00000;
    id<MTLBuffer> blitSource = [device newBufferWithBytes:blitPixels length:sizeof blitPixels options:MTLResourceStorageModeShared];
    id<MTLBuffer> blitDestination = [device newBufferWithLength:sizeof blitResult options:MTLResourceStorageModeShared];
    if (!blitSource || !blitDestination) fail(@"layered view blit buffers unavailable");
    commands = [queue commandBuffer];
    blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:blitSource sourceOffset:0 sourceBytesPerRow:8 * 4 sourceBytesPerImage:sizeof blitPixels
        sourceSize:MTLSizeMake(8,8,1) toTexture:view destinationSlice:2 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit copyFromTexture:view sourceSlice:2 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(8,8,1) toBuffer:blitDestination destinationOffset:0 destinationBytesPerRow:8 * 4 destinationBytesPerImage:sizeof blitResult];
    [blit endEncoding];
    run(commands);
    if (memcmp(blitDestination.contents, blitPixels, sizeof blitPixels)) fail(@"layered view blit did not address its parent slice");
    check_level_slice(device, queue, root, 1, 3, 192);
#endif

    MTLTextureDescriptor *cpuDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
    cpuDescriptor.textureType = MTLTextureType2DArray;
    cpuDescriptor.arrayLength = 4;
    cpuDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    cpuDescriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> cpuRoot = [device newTextureWithDescriptor:cpuDescriptor];
    id<MTLTexture> cpuView = [cpuRoot newTextureViewWithPixelFormat:cpuRoot.pixelFormat
        textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(1, 3)];
    if (!cpuRoot || !cpuView) fail(@"shared array CPU view unavailable");
#ifndef RDNA4_VIEW_AUDIT
    uint32_t neighborBefore[8 * 8], after[8 * 8], pixels[8 * 8];
    uint32_t neighborAfter[8 * 8];
    memset(neighborBefore, 0xA5, sizeof neighborBefore);
    [cpuRoot getBytes:neighborBefore bytesPerRow:8 * 4 bytesPerImage:8 * 8 * 4
        fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 slice:1];
    for (NSUInteger i = 0; i < 8 * 8; ++i) pixels[i] = 0xFFB00000;
    [cpuView replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 slice:1
        withBytes:pixels bytesPerRow:8 * 4 bytesPerImage:sizeof pixels];
    memset(after, 0, sizeof after);
    [cpuRoot getBytes:after bytesPerRow:8 * 4 bytesPerImage:8 * 8 * 4
        fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 slice:2];
    if (memcmp(after, pixels, sizeof after)) fail(@"CPU array view slice upload did not reach its absolute parent slice");
    memset(neighborAfter, 0, sizeof neighborAfter);
    [cpuRoot getBytes:neighborAfter bytesPerRow:8 * 4 bytesPerImage:8 * 8 * 4
        fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 slice:1];
    if (memcmp(neighborAfter, neighborBefore, sizeof neighborBefore)) fail(@"CPU array view write modified an adjacent parent slice");
#endif

    MTLTextureDescriptor *cubeDescriptor = [MTLTextureDescriptor
        textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:8 mipmapped:NO];
    cubeDescriptor.usage = descriptor.usage;
    cubeDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> cube = [device newTextureWithDescriptor:cubeDescriptor];
    id<MTLTexture> cubeWhole = [cube newTextureViewWithPixelFormat:cube.pixelFormat];
    id<MTLTexture> face = [cube newTextureViewWithPixelFormat:cube.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(0, 1) slices:NSMakeRange(4, 1)];
    if (!cube || !cubeWhole || cubeWhole.textureType != MTLTextureTypeCube || cubeWhole.arrayLength != 1 ||
        !face || face.parentRelativeSlice != 4)
        fail(@"cube and cube-face views unavailable or have incorrect metadata");
    for (NSUInteger slice = 0; slice < 6; ++slice) clear_slice(queue, cube, 0, slice, 64);
    clear_slice(queue, face, 0, 0, 128);
#ifndef RDNA4_VIEW_AUDIT
    check_level_slice(device, queue, cube, 0, 4, 128);
    check_level_slice(device, queue, cube, 0, 3, 64);
#endif

    MTLTextureDescriptor *cubeArrayDescriptor = [[MTLTextureDescriptor alloc] init];
    cubeArrayDescriptor.textureType = MTLTextureTypeCubeArray;
    cubeArrayDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    cubeArrayDescriptor.width = 8;
    cubeArrayDescriptor.height = 8;
    cubeArrayDescriptor.depth = 1;
    cubeArrayDescriptor.arrayLength = 2;
    cubeArrayDescriptor.mipmapLevelCount = 1;
    cubeArrayDescriptor.sampleCount = 1;
    cubeArrayDescriptor.usage = descriptor.usage;
    cubeArrayDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> cubeArray = [device newTextureWithDescriptor:cubeArrayDescriptor];
    id<MTLTexture> secondCube = [cubeArray newTextureViewWithPixelFormat:cubeArray.pixelFormat
        textureType:MTLTextureTypeCube levels:NSMakeRange(0, 1) slices:NSMakeRange(6, 6)];
    id<MTLTexture> cubeArrayWhole = [cubeArray newTextureViewWithPixelFormat:cubeArray.pixelFormat];
    if (!cubeArray || !secondCube || secondCube.textureType != MTLTextureTypeCube ||
        secondCube.parentRelativeSlice != 6 || !cubeArrayWhole || cubeArrayWhole.textureType != MTLTextureTypeCubeArray ||
        cubeArrayWhole.arrayLength != 2)
        fail(@"cube-array views unavailable or have incorrect metadata");
    for (NSUInteger slice = 0; slice < 12; ++slice) clear_slice(queue, cubeArray, 0, slice, 64);
    id<MTLTexture> secondFace = [secondCube newTextureViewWithPixelFormat:secondCube.pixelFormat
        textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(4, 1)];
    if (!secondFace || secondFace.parentRelativeSlice != 4) fail(@"nested cube-array face view unavailable");
    clear_slice(queue, secondFace, 0, 0, 192);
#ifndef RDNA4_VIEW_AUDIT
    check_level_slice(device, queue, cubeArray, 0, 10, 192);
    check_level_slice(device, queue, cubeArray, 0, 4, 64);
#endif

    if (!reference) {
        const struct { MTLTextureType type; NSRange slices; } invalid[] = {
            { MTLTextureType2D, NSMakeRange(0, 2) },
            { MTLTextureType2DArray, NSMakeRange(6, 1) },
            { MTLTextureTypeCube, NSMakeRange(1, 6) },
            { MTLTextureTypeCube, NSMakeRange(0, 5) },
            { MTLTextureTypeCubeArray, NSMakeRange(0, 7) },
        };
        for (NSUInteger i = 0; i < sizeof invalid / sizeof *invalid; ++i)
            if ([root newTextureViewWithPixelFormat:root.pixelFormat textureType:invalid[i].type
                    levels:NSMakeRange(0, 1) slices:invalid[i].slices])
                fail(@"invalid layered texture view range or cube alignment accepted");
        commands = [queue commandBuffer];
        blit = [commands blitCommandEncoder];
        [blit copyFromTexture:view sourceSlice:2 sourceLevel:1 toTexture:nested
            destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
        [blit endEncoding];
        run_rejected(commands);
    }
#ifdef RDNA4_VIEW_AUDIT
    require_image_view(RDNA4VkImageViewType2DArray, 1, 3, 1, 4, @"2D-array view");
    require_image_view(RDNA4VkImageViewType2D, 2, 1, 3, 1, @"nested array slice view");
    require_image_view(RDNA4VkImageViewType2DArray, 0, 1, 0, 1, @"2D-to-array view");
    require_image_view(RDNA4VkImageViewTypeCube, 0, 1, 0, 6, @"cube view of array");
    require_image_view(RDNA4VkImageViewTypeCube, 0, 1, 0, 6, @"whole cube view");
    require_image_view(RDNA4VkImageViewType2D, 0, 1, 4, 1, @"cube face view");
    require_image_view(RDNA4VkImageViewTypeCube, 0, 1, 6, 6, @"cube-array cube view");
    require_image_view(RDNA4VkImageViewTypeCubeArray, 0, 1, 0, 12, @"whole cube-array view");
    require_image_view(RDNA4VkImageViewType2D, 0, 1, 10, 1, @"nested cube-array face view");
    puts("TEXTURE_LAYERED_VIEW_AUDIT_PASS array cube cube-array nested metadata attachments mipmaps blit ranges alias-rejection; submissions=0; pixels untested");
#else
    printf("TEXTURE_LAYERED_VIEW_PIXEL_PASS reference=%d array cube cube-array nested attachments mipmaps CPU-access blit alias-check sampling exact\n", reference);
#endif
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-texture-view BUNDLE|--reference\nChecks clear and pixel readback through identity, swizzled, mip and nested texture views. "
                 "RDNA4_VIEW_AUDIT builds check RADV attachment validity only, with mock BOs and no submissions.");
            return 0;
        }
        if (argc != 2 || (argv[1][0] == '-' && strcmp(argv[1], "--reference"))) return 2;
        BOOL reference = !strcmp(argv[1], "--reference");
#ifdef RDNA4_VIEW_AUDIT
        if (reference) return 2;
#endif
        if (!reference && geteuid() != 0) return 2;
        alarm(60);
        id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
#ifdef RDNA4_VIEW_AUDIT
            setenv("RADV_DARWIN_MOCK", "1", 1);
            setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
#else
            setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
            setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
#endif
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *error = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
            device = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) fail(@"device unavailable");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) fail(@"queue unavailable");
        for (NSUInteger i = 0; i < 2; i++) {
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            descriptor.storageMode = MTLStorageModePrivate;
            id<MTLTexture> root = [device newTextureWithDescriptor:descriptor];
            MTLTextureSwizzleChannels swizzle = { i ? MTLTextureSwizzleBlue : MTLTextureSwizzleRed,
                MTLTextureSwizzleGreen, i ? MTLTextureSwizzleRed : MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };
            id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D
                levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:swizzle];
            if (!root || !view) fail(@"render-target view unavailable");
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = view;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(32.0/255, 96.0/255, 160.0/255, 1);
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (!encoder) fail(@"render encoder unavailable");
            [encoder endEncoding];
            run(commands);
#ifdef RDNA4_VIEW_AUDIT
            printf("TEXTURE_VIEW_AUDIT_PASS swizzled=%lu invalid_attachments=0 submissions=0; pixels untested\n", (unsigned long)i);
#else
            id<MTLBuffer> result = [device newBufferWithLength:32*32*4 options:MTLResourceStorageModeShared];
            if (!result) fail(@"readback buffer unavailable");
            memset(result.contents, 0xA5, result.length);
            commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            if (!blit) fail(@"readback encoder unavailable");
            [blit copyFromTexture:root sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(32,32,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];
            [blit endEncoding];
            run(commands);
            const uint8_t *bytes = result.contents;
            for (NSUInteger pixel = 0; pixel < 32*32; pixel++) {
                if (bytes[pixel*4] != 160 || bytes[pixel*4+1] != 96 || bytes[pixel*4+2] != 32 || bytes[pixel*4+3] != 255)
                    fail([NSString stringWithFormat:@"swizzled=%lu pixel=%lu BGRA=%u,%u,%u,%u", (unsigned long)i,
                          (unsigned long)pixel, bytes[pixel*4], bytes[pixel*4+1], bytes[pixel*4+2], bytes[pixel*4+3]]);
            }
            printf("TEXTURE_VIEW_PIXEL_PASS reference=%d swizzled=%lu pixels=1024 exact\n", reference, (unsigned long)i);
#endif
        }
        test_mip_views(device, queue, reference);
        test_layered_views(device, queue, reference);
        test_host_views(device, queue, reference);
        _exit(0);
    }
}
