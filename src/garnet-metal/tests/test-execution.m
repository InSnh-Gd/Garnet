// Execute the native Metal implementation through a test-only Vulkan backend.
// The VM test bundle uses MoltenVK over the VM's existing Metal device. The
// native RADV/kernel transport and physical GPU bring-up are not exercised.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdatomic.h>
#include <unistd.h>

static void fail(NSString *message) __attribute__((noreturn));
typedef id (*CIComputeCreator)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption,
                              MTLComputePipelineReflection **, NSError **) __attribute__((ns_returns_retained));
static CIComputeCreator ciComputeCreator;
static _Atomic unsigned ciDeferredPipelines;
static _Atomic unsigned ciActivePipelines;
static NSMutableArray *ciPlaceholders;

static id __attribute__((ns_returns_retained)) executionCICompute(id device, SEL selector, MTLComputePipelineDescriptor *descriptor,
                             MTLPipelineOption options, MTLComputePipelineReflection **reflection, NSError **error) {
    NSError *failure = nil;
    atomic_fetch_add(&ciActivePipelines, 1);
    id pipeline = ciComputeCreator(device, selector, descriptor, options, reflection, &failure);
    if (error) *error = failure;
    NSError *underlying = failure.userInfo[NSUnderlyingErrorKey];
    if (!pipeline && [failure.domain isEqualToString:@"GarnetMetal"] && failure.code == 100 &&
        [underlying.domain isEqualToString:@"GarnetMetal"] && underlying.code == 9)
        atomic_fetch_add(&ciDeferredPipelines, 1);
    SEL noop = sel_registerName("noop");
    if ([pipeline respondsToSelector:noop] && ((BOOL (*)(id, SEL))objc_msgSend)(pipeline, noop)) {
        @synchronized (ciPlaceholders) { [ciPlaceholders addObject:pipeline]; }
        atomic_fetch_add(&ciDeferredPipelines, 1);
    }
    atomic_fetch_sub(&ciActivePipelines, 1);
    return pipeline;
}

static void executionCIWait(double deadline) {
    for (;;) {
        BOOL pending = atomic_load(&ciActivePipelines) != 0;
        NSArray *placeholders;
        @synchronized (ciPlaceholders) { placeholders = [ciPlaceholders copy]; }
        for (id pipeline in placeholders)
            if (!((id (*)(id, SEL))objc_msgSend)(pipeline, sel_registerName("rdna4Real"))) pending = YES;
        if (!pending) return;
        if (NSProcessInfo.processInfo.systemUptime >= deadline) fail(@"Core Image pipeline preparation did not settle in 3 seconds");
        usleep(1000);
    }
}

static CIContext *executionCIContext(id<MTLCommandQueue> queue) {
    return [CIContext contextWithMTLCommandQueue:queue options:@{
        kCIContextUseSoftwareRenderer: @NO,
        kCIContextCacheIntermediates: @NO,
        kCIContextWorkingColorSpace: NSNull.null,
        kCIContextOutputColorSpace: NSNull.null,
        // The generic test device does not expose an IOAccel memory budget.
        kCIContextMemoryLimit: @64
    }];
}

static void fail(NSString *message) {
    fprintf(stderr, "EXECUTION_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void finish(id<MTLCommandBuffer> commands, NSString *operation) {
    if (!commands) fail([NSString stringWithFormat:@"%@ command buffer unavailable", operation]);
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@", operation, (unsigned long)commands.status, commands.error]);
}

static void poisonCITexture(id<MTLCommandQueue> queue, id<MTLTexture> texture) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) fail(@"Core Image sentinel clear unavailable");
    [encoder endEncoding];
    finish(commands, @"Core Image sentinel clear");
}

static NSData *readTexture(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                           NSUInteger width, NSUInteger height, NSString *operation) {
    NSUInteger length = width * height * 4;
    id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (!buffer) fail([NSString stringWithFormat:@"%@ readback buffer unavailable", operation]);
    memset(buffer.contents, 0xA5, length);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!commands || !blit) fail([NSString stringWithFormat:@"%@ blit encoder unavailable", operation]);
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:buffer destinationOffset:0 destinationBytesPerRow:width * 4
      destinationBytesPerImage:length];
    [blit endEncoding];
    finish(commands, [operation stringByAppendingString:@" readback"]);
    return [NSData dataWithBytes:buffer.contents length:length];
}

static uint64_t hashBytes(const uint8_t *bytes, NSUInteger length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (NSUInteger i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void runIOSurfacePitchTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    const NSUInteger sizes[][2] = { {88,60}, {3752,60}, {1446,1250}, {1754,998} };
    for (NSUInteger size = 0; size < sizeof sizes / sizeof sizes[0]; ++size) {
        NSUInteger w = sizes[size][0], h = sizes[size][1], pitch = (w*4+63)&~(NSUInteger)63;
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
            (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(pitch),
            (id)kIOSurfaceAllocSize: @(pitch*h), (id)kIOSurfacePixelFormat: @((uint32_t)0x42475241) });
        if (!surface) fail(@"pitched IOSurface unavailable");
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        if (!texture) fail(@"pitched IOSurface texture unavailable");
        for (NSUInteger frame = 0; frame < 2; ++frame) {
            IOSurfaceLock(surface, 0, NULL);
            uint8_t *base = IOSurfaceGetBaseAddress(surface);
            for (NSUInteger y = 0; y < h; ++y) for (NSUInteger x = 0; x < w; ++x)
                for (NSUInteger c = 0; c < 4; ++c)
                    base[y*pitch+x*4+c] = c == 3 ? 255 : (uint8_t)(x*17+y*31+c*43+frame*71);
            IOSurfaceUnlock(surface, 0, NULL);
            NSData *read = readTexture(device, queue, texture, w, h, @"pitched IOSurface");
            const uint8_t *bytes = read.bytes;
            for (NSUInteger y = 0; y < h; ++y) for (NSUInteger x = 0; x < w; ++x)
                for (NSUInteger c = 0; c < 4; ++c) {
                    uint8_t expected = c == 3 ? 255 : (uint8_t)(x*17+y*31+c*43+frame*71);
                    if (bytes[(y*w+x)*4+c] != expected) fail([NSString stringWithFormat:
                        @"pitched IOSurface %lux%lu frame=%lu x=%lu y=%lu channel=%lu got=%u expected=%u",
                        (unsigned long)w, (unsigned long)h, (unsigned long)frame,
                        (unsigned long)x, (unsigned long)y, (unsigned long)c, bytes[(y*w+x)*4+c], expected]);
                }
        }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(32.0/255, 96.0/255, 160.0/255, 1);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) fail(@"pitched IOSurface clear unavailable");
        [encoder endEncoding]; finish(commands, @"pitched IOSurface clear");
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *base = IOSurfaceGetBaseAddress(surface), expected[4] = {160,96,32,255};
        for (NSUInteger y = 0; y < h; ++y) for (NSUInteger x = 0; x < w; ++x)
            if (memcmp(base+y*pitch+x*4, expected, 4)) fail(@"pitched IOSurface GPU writeback differs");
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        printf("EXECUTION_IOSURFACE_PITCH_PASS %lux%lu pitch=%lu frames=2 writeback=exact\n",
            (unsigned long)w, (unsigned long)h, (unsigned long)pitch);
        CFRelease(surface);
    }
}

static id<MTLRenderPipelineState> renderPipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                  NSString *fragmentName, NSError **error) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"execution_fullscreen_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction)
        fail([NSString stringWithFormat:@"render functions unavailable for %@", fragmentName]);
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

static void runRenderTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSString *source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct ExecutionOut { float4 position [[position]]; float2 uv; };\n"
         "vertex ExecutionOut execution_fullscreen_vs(uint vid [[vertex_id]]) {\n"
         "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
         "  ExecutionOut out; out.position = float4(p[vid], 0.0, 1.0); out.uv = p[vid] * 0.5 + 0.5; return out;\n"
         "}\n"
         "fragment float4 execution_solid_fs(ExecutionOut in [[stage_in]], constant float4 &color [[buffer(0)]]) { return color; }\n"
         "fragment float4 execution_sample_fs(ExecutionOut in [[stage_in]], texture2d<float> source [[texture(0)]], sampler s [[sampler(0)]]) { return source.sample(s, in.uv); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) fail([NSString stringWithFormat:@"render source library: %@", error]);
    id<MTLRenderPipelineState> solid = renderPipeline(device, library, @"execution_solid_fs", &error);
    if (!solid) fail([NSString stringWithFormat:@"solid render pipeline: %@", error]);
    id<MTLRenderPipelineState> sampled = renderPipeline(device, library, @"execution_sample_fs", &error);
    if (!sampled) fail([NSString stringWithFormat:@"sample render pipeline: %@", error]);

    const NSUInteger width = 32, height = 32;
    MTLTextureDescriptor *targetDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:width height:height mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModePrivate;
    targetDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
    if (!target) fail(@"render target unavailable");

    float color[4] = { 0.125f, 0.375f, 0.625f, 1.0f };
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!commands || !encoder) fail(@"solid render encoder unavailable");
    [encoder setRenderPipelineState:solid];
    [encoder setViewport:(MTLViewport){ 0, 0, width, height, 0, 1 }];
    [encoder setScissorRect:(MTLScissorRect){ 0, 0, width, height }];
    [encoder setFragmentBytes:color length:sizeof color atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    finish(commands, @"solid render");
    NSData *solidData = readTexture(device, queue, target, width, height, @"solid render");
    const uint8_t *solidPixels = solidData.bytes;
    uint8_t expectedSolid[4] = { 159, 96, 32, 255 };
    NSUInteger solidBad = 0;
    for (NSUInteger i = 0; i < width * height; i++)
        if (memcmp(solidPixels + i * 4, expectedSolid, sizeof expectedSolid)) solidBad++;
    if (solidBad) fail([NSString stringWithFormat:@"solid render has %lu bad pixels", (unsigned long)solidBad]);
    printf("EXECUTION_RENDER_SOLID_PASS pixels=%lu hash=%016llx\n",
           (unsigned long)(width * height), hashBytes(solidPixels, solidData.length));

    MTLTextureDescriptor *sourceDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:4 height:4 mipmapped:NO];
    sourceDescriptor.storageMode = MTLStorageModeShared;
    sourceDescriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sourceTexture = [device newTextureWithDescriptor:sourceDescriptor];
    if (!sourceTexture) fail(@"sample source texture unavailable");
    uint8_t sourcePixels[4 * 4 * 4];
    for (NSUInteger i = 0; i < 4 * 4; i++) {
        sourcePixels[i * 4 + 0] = 12;
        sourcePixels[i * 4 + 1] = 34;
        sourcePixels[i * 4 + 2] = 56;
        sourcePixels[i * 4 + 3] = 255;
    }
    [sourceTexture replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0
                         withBytes:sourcePixels bytesPerRow:4 * 4];
    MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
    if (!sampler) fail(@"sample sampler unavailable");
    commands = [queue commandBuffer];
    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!commands || !encoder) fail(@"sample render encoder unavailable");
    [encoder setRenderPipelineState:sampled];
    [encoder setViewport:(MTLViewport){ 0, 0, width, height, 0, 1 }];
    [encoder setScissorRect:(MTLScissorRect){ 0, 0, width, height }];
    [encoder setFragmentTexture:sourceTexture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    finish(commands, @"texture render");
    NSData *sampledData = readTexture(device, queue, target, width, height, @"texture render");
    const uint8_t *sampledPixels = sampledData.bytes;
    uint8_t expectedSample[4] = { 56, 34, 12, 255 };
    NSUInteger sampledBad = 0;
    for (NSUInteger i = 0; i < width * height; i++)
        if (memcmp(sampledPixels + i * 4, expectedSample, sizeof expectedSample)) sampledBad++;
    if (sampledBad) fail([NSString stringWithFormat:@"texture render has %lu bad pixels", (unsigned long)sampledBad]);
    printf("EXECUTION_RENDER_TEXTURE_PASS pixels=%lu hash=%016llx\n",
           (unsigned long)(width * height), hashBytes(sampledPixels, sampledData.length));

    MTLRenderPipelineDescriptor *blendDescriptor = [MTLRenderPipelineDescriptor new];
    blendDescriptor.vertexFunction = [library newFunctionWithName:@"execution_fullscreen_vs"];
    blendDescriptor.fragmentFunction = [library newFunctionWithName:@"execution_solid_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *attachment = blendDescriptor.colorAttachments[0];
    attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
    attachment.blendingEnabled = YES;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> blend = [device newRenderPipelineStateWithDescriptor:blendDescriptor error:&error];
    if (!blend) fail([NSString stringWithFormat:@"blend render pipeline: %@", error]);
    commands = [queue commandBuffer];
    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!commands || !encoder) fail(@"blend render encoder unavailable");
    [encoder setRenderPipelineState:blend];
    [encoder setViewport:(MTLViewport){ 0, 0, width, height, 0, 1 }];
    [encoder setScissorRect:(MTLScissorRect){ 0, 0, width, height }];
    float firstColor[4] = { 0.8f, 0.2f, 0.1f, 0.5f };
    float secondColor[4] = { 0.1f, 0.6f, 0.9f, 0.25f };
    [encoder setFragmentBytes:firstColor length:sizeof firstColor atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setFragmentBytes:secondColor length:sizeof secondColor atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    finish(commands, @"blend render");
    NSData *blendData = readTexture(device, queue, target, width, height, @"blend render");
    const uint8_t *blendPixels = blendData.bytes;
    uint8_t expectedBlend[4] = { 249, 191, 179, 255 };
    NSUInteger blendBad = 0;
    for (NSUInteger i = 0; i < width * height; i++) {
        const uint8_t *pixel = blendPixels + i * 4;
        BOOL ok = YES;
        for (NSUInteger c = 0; c < 4; c++) if (abs((int)pixel[c] - (int)expectedBlend[c]) > 1) ok = NO;
        if (!ok) blendBad++;
    }
    if (blendBad) fail([NSString stringWithFormat:@"blend render has %lu bad pixels (first=%u,%u,%u,%u)",
                        (unsigned long)blendBad, blendPixels[0], blendPixels[1], blendPixels[2], blendPixels[3]]);
    printf("EXECUTION_RENDER_BLEND_PASS pixels=%lu hash=%016llx tolerance=1\n",
           (unsigned long)(width * height), hashBytes(blendPixels, blendData.length));
}

static void runCoreImageTest(id<MTLDevice> device, id<MTLCommandQueue> queue, BOOL reference, NSString *resultsPath) {
    const NSUInteger width = 16, height = 16;
    Method ciMethod = reference ? NULL : class_getInstanceMethod([device class],
        @selector(newComputePipelineStateWithDescriptor:options:reflection:error:));
    const char *oldFallback = getenv("RDNA4M_TEST_FALLBACK_AS_WS");
    NSString *savedFallback = oldFallback ? @(oldFallback) : nil;
    if (ciMethod) {
        // Exercise the admitted application's existing placeholder/hot-swap policy.
        setenv("RDNA4M_TEST_FALLBACK_AS_WS", "1", 1);
        ciPlaceholders = [NSMutableArray array];
        ciComputeCreator = (CIComputeCreator)method_setImplementation(ciMethod, (IMP)executionCICompute);
    }
    MTLTextureDescriptor *inputDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:width height:height mipmapped:NO];
    inputDescriptor.storageMode = MTLStorageModeShared;
    inputDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> input = [device newTextureWithDescriptor:inputDescriptor];
    MTLTextureDescriptor *outputDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:width height:height mipmapped:NO];
    outputDescriptor.storageMode = MTLStorageModePrivate;
    outputDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    id<MTLTexture> output = [device newTextureWithDescriptor:outputDescriptor];
    if (!input || !output) fail(@"Core Image textures unavailable");
    uint8_t inputPixels[16 * 16 * 4];
    for (NSUInteger i = 0; i < width * height; i++) {
        inputPixels[i * 4 + 0] = 32;
        inputPixels[i * 4 + 1] = 64;
        inputPixels[i * 4 + 2] = 96;
        inputPixels[i * 4 + 3] = 255;
    }
    [input replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
                withBytes:inputPixels bytesPerRow:width * 4];
    CIContext *context = executionCIContext(queue);
    if (!context) fail(@"Core Image context unavailable");
    CIImage *image = [[CIImage alloc] initWithMTLTexture:input options:@{ kCIImageColorSpace: NSNull.null }];
    CIImage *filtered = [image imageByApplyingFilter:@"CIColorInvert"];
    if (!image || !filtered) fail(@"Core Image invert graph unavailable");
    NSError *prepareError = nil;
    CIRenderDestination *destination = [[CIRenderDestination alloc] initWithMTLTexture:output commandBuffer:nil];
    if (!destination || ![context prepareRender:filtered fromRect:filtered.extent
                                   toDestination:destination atPoint:CGPointZero error:&prepareError])
        fail([NSString stringWithFormat:@"Core Image prepare: %@", prepareError]);
    executionCIWait(NSProcessInfo.processInfo.systemUptime + 3.0);
    poisonCITexture(queue, output);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
    [context render:filtered toMTLTexture:output commandBuffer:commands
             bounds:CGRectMake(0, 0, width, height) colorSpace:colorSpace];
    CGColorSpaceRelease(colorSpace);
    finish(commands, @"Core Image render");
    NSData *data = readTexture(device, queue, output, width, height, @"Core Image");
    const uint8_t *pixels = data.bytes;
    uint8_t expected[4] = { 223, 191, 159, 255 };
    NSUInteger bad = 0;
    for (NSUInteger i = 0; i < width * height; i++)
        if (memcmp(pixels + i * 4, expected, sizeof expected)) bad++;
    if (bad) fail([NSString stringWithFormat:@"Core Image invert has %lu bad pixels (first=%u,%u,%u,%u)",
                   (unsigned long)bad, pixels[0], pixels[1], pixels[2], pixels[3]]);
    printf("EXECUTION_CORE_IMAGE_PASS pixels=%lu hash=%016llx\n",
           (unsigned long)(width * height), hashBytes(pixels, data.length));
    if (!resultsPath) {
        if (ciMethod) {
            method_setImplementation(ciMethod, (IMP)ciComputeCreator);
            if (savedFallback) setenv("RDNA4M_TEST_FALLBACK_AS_WS", savedFallback.UTF8String, 1);
            else unsetenv("RDNA4M_TEST_FALLBACK_AS_WS");
        }
        return;
    }

    for (NSUInteger y = 0; y < height; y++) for (NSUInteger x = 0; x < width; x++) {
        NSUInteger i = y * width + x;
        inputPixels[i * 4 + 0] = (uint8_t)(16 + x * 11);
        inputPixels[i * 4 + 1] = (uint8_t)(32 + y * 9);
        inputPixels[i * 4 + 2] = (uint8_t)(48 + (x ^ y) * 7);
    }
    [input replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
                withBytes:inputPixels bytesPerRow:width * 4];
    NSError *error = nil;
    if (reference && ![NSFileManager.defaultManager createDirectoryAtPath:resultsPath
                                              withIntermediateDirectories:YES attributes:nil error:&error])
        fail([NSString stringWithFormat:@"Core Image reference directory: %@", error]);
    for (NSString *filter in @[ @"builtin", @"cikl", @"blur", @"transform", @"composite" ]) {
        double prepareDeadline = NSProcessInfo.processInfo.systemUptime + 3.0;
        for (NSUInteger attempt = 0; ; attempt++) {
            if (attempt && NSProcessInfo.processInfo.systemUptime >= prepareDeadline)
                fail([NSString stringWithFormat:@"Core Image %@ compilation did not settle in 3 seconds", filter]);
            unsigned deferredBefore = atomic_load(&ciDeferredPipelines);
            if ([filter isEqualToString:@"cikl"]) {
                CIColorKernel *kernel = ((id (*)(id, SEL, id))objc_msgSend)(CIColorKernel.class,
                    sel_registerName("kernelWithString:"),
                    @"kernel vec4 execution_ci_invert(__sample c) { return vec4(vec3(1.0)-c.rgb,c.a); }");
                if (!kernel) fail(@"Core Image CIKL parser unavailable");
                filtered = [kernel applyWithExtent:image.extent arguments:@[ image ]];
            } else if ([filter isEqualToString:@"blur"])
                filtered = [[[image imageByClampingToExtent] imageByApplyingFilter:@"CIGaussianBlur"
                                  withInputParameters:@{ kCIInputRadiusKey: @2.0 }] imageByCroppingToRect:image.extent];
            else if ([filter isEqualToString:@"transform"])
                filtered = [[[image imageByClampingToExtent] imageByApplyingTransform:CGAffineTransformMakeTranslation(0.5, 0.25)]
                               imageByCroppingToRect:image.extent];
            else if ([filter isEqualToString:@"composite"]) {
                CIImage *translucent = [image imageByApplyingFilter:@"CIColorMatrix" withInputParameters:@{
                    @"inputAVector": [CIVector vectorWithX:0 Y:0 Z:0 W:0.5]
                }];
                filtered = [translucent imageByCompositingOverImage:[image imageByApplyingFilter:@"CIColorInvert"]];
            } else filtered = [image imageByApplyingFilter:@"CIColorInvert"];
            if (!filtered) fail([NSString stringWithFormat:@"Core Image %@ graph unavailable", filter]);
            error = nil;
            BOOL prepared = [context prepareRender:filtered fromRect:filtered.extent
                                       toDestination:destination atPoint:CGPointZero error:&error];
            executionCIWait(prepareDeadline);
            if (!prepared && atomic_load(&ciDeferredPipelines) == deferredBefore)
                fail([NSString stringWithFormat:@"Core Image %@ prepare: %@", filter, error]);
            NSData *result = nil;
            if (prepared) {
                poisonCITexture(queue, output);
                colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
                destination.colorSpace = colorSpace;
                CGColorSpaceRelease(colorSpace);
                CIRenderTask *task = [context startTaskToRender:filtered fromRect:filtered.extent
                    toDestination:destination atPoint:CGPointZero error:&error];
                CIRenderInfo *info = [task waitUntilCompletedAndReturnError:&error];
                executionCIWait(prepareDeadline);
                if (info) result = readTexture(device, queue, output, width, height, @"Core Image differential");
                else if (atomic_load(&ciDeferredPipelines) == deferredBefore)
                    fail([NSString stringWithFormat:@"Core Image %@ render: %@", filter, error]);
            }
            unsigned deferred = atomic_load(&ciDeferredPipelines) - deferredBefore;
            if (deferred) {
                printf("EXECUTION_CORE_IMAGE_PREPARE_DEFERRED filter=%s attempt=%lu pipelines=%u pixels=unvalidated\n",
                       filter.UTF8String, (unsigned long)attempt + 1, deferred);
                if (NSProcessInfo.processInfo.systemUptime >= prepareDeadline)
                    fail([NSString stringWithFormat:@"Core Image %@ compilation did not settle in 3 seconds", filter]);
                continue;
            }
            NSString *path = [resultsPath stringByAppendingPathComponent:[filter stringByAppendingString:@".rgba"]];
            if (reference) {
                if (![result writeToFile:path options:NSDataWritingAtomic error:&error])
                    fail([NSString stringWithFormat:@"Core Image %@ reference write: %@", filter, error]);
                printf("EXECUTION_CORE_IMAGE_REFERENCE filter=%s pixels=%lu hash=%016llx\n",
                       filter.UTF8String, (unsigned long)(width * height), hashBytes(result.bytes, result.length));
            } else {
                NSData *expectedData = [NSData dataWithContentsOfFile:path options:0 error:&error];
                if (!expectedData || expectedData.length != result.length)
                    fail([NSString stringWithFormat:@"Core Image %@ reference data: %@", filter, error]);
                const uint8_t *actualBytes = result.bytes, *expectedBytes = expectedData.bytes;
                NSUInteger different = 0;
                int maxDifference = 0;
                for (NSUInteger i = 0; i < result.length; i++) {
                    int difference = abs((int)actualBytes[i] - (int)expectedBytes[i]);
                    if (difference > maxDifference) maxDifference = difference;
                    if (difference > 1) different++;
                }
                if (different) fail([NSString stringWithFormat:@"Core Image %@ has %lu bytes beyond 1 LSB, max=%d",
                                    filter, (unsigned long)different, maxDifference]);
                printf("EXECUTION_CORE_IMAGE_DIFFERENTIAL_PASS filter=%s pixels=%lu max_difference=%d hash=%016llx\n",
                       filter.UTF8String, (unsigned long)(width * height), maxDifference, hashBytes(result.bytes, result.length));
            }
            break;
        }
    }
    if (ciMethod) {
        printf("EXECUTION_CORE_IMAGE_HOTSWAP_COVERAGE placeholders=%lu all_real=1 cold_fallback=%s\n",
               (unsigned long)ciPlaceholders.count, ciPlaceholders.count ? "covered" : "not-observed");
        method_setImplementation(ciMethod, (IMP)ciComputeCreator);
        if (savedFallback) setenv("RDNA4M_TEST_FALLBACK_AS_WS", savedFallback.UTF8String, 1);
        else unsetenv("RDNA4M_TEST_FALLBACK_AS_WS");
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-execution VM_TEST_BUNDLE|--reference [CI_RESULTS_DIR]\n       test-execution RADV_BUNDLE --expect-no-backend\nExecutes compute, render, texture, blit and Core Image pixel tests in a macOS VM. CI_RESULTS_DIR writes reference pixels or compares native pixels for five Core Image filters. Native mode requires root and a test bundle containing an executable Vulkan ICD. --expect-no-backend checks queue creation failure using the shipping RADV bundle on a VM without the native GPU service.");
            return 0;
        }
        if (argc < 2 || argc > 3) return 2;
        BOOL reference = !strcmp(argv[1], "--reference");
        BOOL noBackend = argc == 3 && !strcmp(argv[2], "--expect-no-backend");
        if ((reference && noBackend) || (argc == 3 && argv[2][0] == '-' && !noBackend) ||
            (!reference && (argv[1][0] == '-' || geteuid() != 0))) return 2;
        NSString *resultsPath = argc == 3 && !noBackend ? @(argv[2]) : nil;
        alarm(60);
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
            setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
            NSError *error = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
            device = [[objc_getClass("GarnetDevice") alloc] init];
            // Generic init isolates these tests from accelerator-port admission.
            ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) fail(@"Metal device unavailable");
        if (noBackend) {
            if ([device newCommandQueue]) fail(@"unexpected queue without a native GPU backend");
            if ([device newCommandQueueWithMaxCommandBufferCount:4]) fail(@"unexpected bounded queue without a native GPU backend");
            MTLCommandQueueDescriptor *descriptor = [MTLCommandQueueDescriptor new];
            if ([device newCommandQueueWithDescriptor:descriptor]) fail(@"unexpected descriptor queue without a native GPU backend");
            puts("EXECUTION_QUEUE_UNAVAILABLE_PASS variants=3 no_crash=1");
            _exit(0);
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) fail(@"command queue unavailable");
        printf("EXECUTION_DEVICE class=%s name=%s reference=%d\n", class_getName([device class]), device.name.UTF8String, reference);
        runIOSurfacePitchTests(device, queue);
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void execute_add(device uint *out [[buffer(0)]], const device uint *in [[buffer(1)]], "
            "constant uint &increment [[buffer(2)]], uint i [[thread_position_in_grid]]) "
            "{ if (i < 65) out[i] = in[i] * 3u + increment; }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"execute_add"];
        if (!function) fail([NSString stringWithFormat:@"source library: %@", error]);
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) fail([NSString stringWithFormat:@"compute pipeline: %@", error]);
        uint32_t values[65], sentinel[65];
        for (unsigned i = 0; i < 65; i++) { values[i] = i * 17u; sentinel[i] = 0xfeedbeef; }
        id<MTLBuffer> input = [device newBufferWithBytes:values length:sizeof values options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithBytes:sentinel length:sizeof sentinel options:MTLResourceStorageModeShared];
        if (!input || !output) fail(@"shared buffers unavailable");
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        if (!commands || !encoder) fail(@"compute encoder unavailable");
        uint32_t increment = 23;
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:input offset:0 atIndex:1];
        [encoder setBytes:&increment length:sizeof increment atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(65, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding];
        finish(commands, @"compute");
        const uint32_t *actual = output.contents;
        if (!actual) fail(@"completed output has no CPU mapping");
        for (unsigned i = 0; i < 65; i++) if (actual[i] != values[i] * 3u + increment)
            fail([NSString stringWithFormat:@"compute[%u]=%u expected=%u", i, actual[i], values[i] * 3u + increment]);
        puts("EXECUTION_COMPUTE_PASS values=65 nonuniform_dispatch=1 constant_bytes=1");
        runRenderTests(device, queue);
        runCoreImageTest(device, queue, reference, resultsPath);
        puts("EXECUTION_PASS native Metal API and test Vulkan execution backend; native hardware transport untested");
        // The generic device does not have accelerator teardown state.
        _exit(0);
    }
}
