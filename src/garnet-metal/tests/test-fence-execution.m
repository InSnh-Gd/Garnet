// Execute real data through production GarnetDevice fences. Run this in the
// macOS VM against the real RADV/kernel path; the mock scheduler test is
// test-fences.m and does not establish GPU visibility.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

@interface FenceExecutionCompletion : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end
@implementation FenceExecutionCompletion
@end

static void require(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "FENCE_EXECUTION_FAIL %s\n", message); _exit(1); }
}

static FenceExecutionCompletion *observe(id<MTLCommandBuffer> command) {
    require(command != nil, "command buffer creation");
    FenceExecutionCompletion *observation = [FenceExecutionCompletion new];
    observation->done = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        observation->status = completed.status;
        observation->error = completed.error;
        atomic_fetch_add(&observation->callbacks, 1);
        dispatch_semaphore_signal(observation->done);
    }];
    return observation;
}

static void await(id<MTLCommandBuffer> command, FenceExecutionCompletion *observation,
                  const char *name) {
    if (dispatch_semaphore_wait(observation->done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
        require(NO, name);
    [command waitUntilCompleted];
    usleep(20000);
    require(atomic_load(&observation->callbacks) == 1 &&
            command.status == MTLCommandBufferStatusCompleted && !observation->error, name);
}

static void requirePending(id<MTLCommandBuffer> command, FenceExecutionCompletion *observation,
                           const char *name) {
    usleep(150000);
    require(!atomic_load(&observation->callbacks) &&
            command.status != MTLCommandBufferStatusCompleted, name);
}

static void requirePattern(id<MTLBuffer> buffer, NSUInteger start, NSUInteger size,
                           uint8_t value, const char *name) {
    const uint8_t *bytes = buffer.contents;
    if (!bytes || start > buffer.length || size > buffer.length - start) {
        fprintf(stderr,
                "FENCE_EXECUTION_FAIL %s range=[%lu,%lu) length=%lu expected=0x%02x\n",
                name, (unsigned long)start, (unsigned long)(start + size),
                (unsigned long)buffer.length, value);
        _exit(1);
    }
    for (NSUInteger i = start; i < start + size; ++i) {
        if (bytes[i] != value) {
            fprintf(stderr,
                    "FENCE_EXECUTION_FAIL %s first_bad_offset=%lu actual=0x%02x expected=0x%02x\n",
                    name, (unsigned long)i, bytes[i], value);
            _exit(1);
        }
    }
}

static void requirePixels(id<MTLBuffer> buffer, uint8_t blue, uint8_t green,
                          uint8_t red, uint8_t alpha, const char *name) {
    const uint8_t *bytes = buffer.contents;
    require(bytes && !(buffer.length & 3), name);
    const uint8_t expected[4] = { blue, green, red, alpha };
    for (NSUInteger i = 0; i < buffer.length; i += 4) {
        if (memcmp(bytes + i, expected, 4)) {
            fprintf(stderr,
                    "FENCE_EXECUTION_FAIL %s first_bad_offset=%lu actual=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",
                    name, (unsigned long)i, bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3],
                    expected[0], expected[1], expected[2], expected[3]);
            _exit(1);
        }
    }
}

static id<MTLLibrary> executionLibrary(id<MTLDevice> device) {
    NSString *source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct FenceOut { float4 position [[position]]; };\n"
         "vertex FenceOut fence_fullscreen_vs(uint vid [[vertex_id]]) {\n"
         "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
         "  FenceOut out; out.position = float4(p[vid], 0.0, 1.0); return out;\n"
         "}\n"
         "fragment float4 fence_solid_fs(FenceOut in [[stage_in]], constant float4 &color [[buffer(0)]]) {\n"
         "  return color;\n"
         "}\n"
         "kernel void fence_increment(device const uint *src [[buffer(0)]],\n"
         "                            device uint *dst [[buffer(1)]],\n"
         "                            uint gid [[thread_position_in_grid]]) {\n"
         "  dst[gid] = src[gid] + 1;\n"
         "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil && error == nil, "shader library compilation");
    return library;
}

static void renderToBlit(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         id<MTLLibrary> library) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"fence_fullscreen_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fence_solid_fs"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    require(pipeline && !error, "render pipeline");

    const NSUInteger width = 16, height = 16;
    MTLTextureDescriptor *textureDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:width height:height mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModePrivate;
    textureDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:textureDescriptor];
    id<MTLBuffer> readback = [device newBufferWithLength:width * height * 4
                                                options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    require(target && readback && fence, "render-to-blit resources");
    memset(readback.contents, 0xa5, readback.length);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
    require(render != nil, "render encoder");
    float color[4] = { 0.125f, 0.375f, 0.625f, 1.0f };
    [render setRenderPipelineState:pipeline];
    [render setViewport:(MTLViewport){ 0, 0, width, height, 0, 1 }];
    [render setScissorRect:(MTLScissorRect){ 0, 0, width, height }];
    [render setFragmentBytes:color length:sizeof color atIndex:0];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render updateFence:fence afterStages:MTLRenderStageFragment];
    [render endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(blit != nil, "render consumer blit encoder");
    [blit waitForFence:fence];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:readback destinationOffset:0 destinationBytesPerRow:width * 4
      destinationBytesPerImage:readback.length];
    [blit endEncoding];
    FenceExecutionCompletion *completion = observe(command);
    [command commit];
    await(command, completion, "render-to-blit completion");

    const uint8_t *pixels = readback.contents;
    const uint8_t expected[4] = { 159, 96, 32, 255 };
    for (NSUInteger i = 0; i < width * height; ++i)
        require(!memcmp(pixels + i * 4, expected, 4), "render-to-blit pixels");
    printf("FENCE_EXECUTION_CASE_PASS render_to_blit pixels=%lu\n",
           (unsigned long)(width * height));
}

static void clearOnlyProducer(id<MTLDevice> device, id<MTLCommandQueue> producerQueue,
                              id<MTLCommandQueue> consumerQueue,
                              id<MTLCommandQueue> setupQueue) {
    const NSUInteger width = 16, height = 16;
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    id<MTLBuffer> readback =
        [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
    id<MTLFence> clearFence = [device newFence];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(target && readback && clearFence && gate,
            "clear-only fence resources");

    MTLRenderPassDescriptor *setupPass = [MTLRenderPassDescriptor renderPassDescriptor];
    setupPass.colorAttachments[0].texture = target;
    setupPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    setupPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    setupPass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
    id<MTLCommandBuffer> setup = [setupQueue commandBuffer];
    id<MTLRenderCommandEncoder> setupEncoder =
        [setup renderCommandEncoderWithDescriptor:setupPass];
    require(setupEncoder != nil, "clear-only setup encoder");
    [setupEncoder endEncoding];
    FenceExecutionCompletion *setupCompletion = observe(setup);
    [setup commit];
    await(setup, setupCompletion, "clear-only setup");

    MTLRenderPassDescriptor *producePass = [MTLRenderPassDescriptor renderPassDescriptor];
    producePass.colorAttachments[0].texture = target;
    producePass.colorAttachments[0].loadAction = MTLLoadActionClear;
    producePass.colorAttachments[0].storeAction = MTLStoreActionStore;
    producePass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
    id<MTLCommandBuffer> producer = [producerQueue commandBuffer];
    id<MTLRenderCommandEncoder> produce =
        [producer renderCommandEncoderWithDescriptor:producePass];
    require(produce != nil, "clear-only producer encoder");
    [produce updateFence:clearFence afterStages:MTLRenderStageFragment];
    [produce endEncoding];
    [producer encodeWaitForEvent:gate value:1];
    FenceExecutionCompletion *producerCompletion = observe(producer);
    [producer commit];

    id<MTLCommandBuffer> consumer = [consumerQueue commandBuffer];
    id<MTLBlitCommandEncoder> consume = [consumer blitCommandEncoder];
    [consume waitForFence:clearFence];
    [consume copyFromTexture:target sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                      toBuffer:readback destinationOffset:0
        destinationBytesPerRow:width * 4
     destinationBytesPerImage:readback.length];
    [consume endEncoding];
    FenceExecutionCompletion *consumerCompletion = observe(consumer);
    [consumer commit];

    await(consumer, consumerCompletion, "clear-only fence consumer");
    requirePending(producer, producerCompletion, "clear-only producer gate");
    requirePixels(readback, 255, 0, 0, 255, "clear-only load preceded fence update");

    gate.signaledValue = 1;
    await(producer, producerCompletion, "clear-only producer completion");
    printf("FENCE_EXECUTION_CASE_PASS clear_only_producer pixels=%lu event_gate\n",
           (unsigned long)(width * height));
}

static void freshFenceWaitNoUpdate(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    enum { bytes = 1024 };
    id<MTLBuffer> source =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> destination =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    require(source && destination && fence, "fresh-fence wait resources");
    memset(source.contents, 0xa5, source.length);
    memset(destination.contents, 0x5a, destination.length);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    require(encoder != nil, "fresh-fence wait blit encoder");
    [encoder waitForFence:fence];
    [encoder fillBuffer:source range:NSMakeRange(0, bytes) value:0x6d];
    [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:bytes];
    [encoder endEncoding];
    FenceExecutionCompletion *completion = observe(command);
    [command commit];
    await(command, completion, "fresh-fence wait completion");
    requirePattern(source, 0, bytes, 0x6d, "fresh-fence wait source fill");
    requirePattern(destination, 0, bytes, 0x6d, "fresh-fence wait full copy");
    printf("FENCE_EXECUTION_CASE_PASS fresh_fence_wait no_prior_update bytes=%d\n", bytes);
}

static void blitComputeBlit(id<MTLDevice> device, id<MTLCommandQueue> queue,
                            id<MTLLibrary> library) {
    const NSUInteger words = 64, bytes = words * sizeof(uint32_t);
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"fence_increment"]
                                              error:&error];
    require(pipeline && !error, "compute pipeline");
    id<MTLBuffer> seed = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> input = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> output = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> readback = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLFence> afterCopy = [device newFence];
    id<MTLFence> afterCompute = [device newFence];
    require(seed && input && output && readback && afterCopy && afterCompute,
            "blit-compute-blit resources");
    for (NSUInteger i = 0; i < words; ++i) ((uint32_t *)seed.contents)[i] = 1;
    memset(readback.contents, 0xa5, bytes);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
    [copy copyFromBuffer:seed sourceOffset:0 toBuffer:input destinationOffset:0 size:bytes];
    [copy updateFence:afterCopy];
    [copy endEncoding];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    [compute waitForFence:afterCopy];
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:input offset:0 atIndex:0];
    [compute setBuffer:output offset:0 atIndex:1];
    [compute dispatchThreads:MTLSizeMake(words, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(words, 1, 1)];
    [compute updateFence:afterCompute];
    [compute endEncoding];
    copy = [command blitCommandEncoder];
    [copy waitForFence:afterCompute];
    [copy copyFromBuffer:output sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];
    [copy endEncoding];
    FenceExecutionCompletion *completion = observe(command);
    [command commit];
    await(command, completion, "blit-compute-blit completion");

    for (NSUInteger i = 0; i < words; ++i)
        require(((uint32_t *)readback.contents)[i] == 2, "blit-compute-blit values");
    printf("FENCE_EXECUTION_CASE_PASS blit_compute_blit words=%lu\n", (unsigned long)words);
}

static void crossCommandBuffer(id<MTLDevice> device, id<MTLCommandQueue> producerQueue,
                               id<MTLCommandQueue> consumerQueue) {
    enum { bytes = 1024, guard = 256, payload = 512 };
    id<MTLBuffer> src = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(src && dst && fence && gate, "cross-command-buffer resources");
    memset(src.contents, 0xa5, bytes);
    memset(dst.contents, 0x5a, bytes);

    id<MTLCommandBuffer> producer = [producerQueue commandBuffer];
    id<MTLBlitCommandEncoder> produce = [producer blitCommandEncoder];
    [produce fillBuffer:src range:NSMakeRange(guard, payload) value:0x37];
    [produce updateFence:fence];
    [produce endEncoding];
    [producer encodeWaitForEvent:gate value:1];
    FenceExecutionCompletion *producerCompletion = observe(producer);
    [producer commit];

    id<MTLCommandBuffer> consumer = [consumerQueue commandBuffer];
    id<MTLBlitCommandEncoder> consume = [consumer blitCommandEncoder];
    [consume waitForFence:fence];
    [consume copyFromBuffer:src sourceOffset:guard toBuffer:dst destinationOffset:guard size:payload];
    [consume endEncoding];
    FenceExecutionCompletion *consumerCompletion = observe(consumer);
    [consumer commit];

    await(consumer, consumerCompletion, "cross-command-buffer consumer");
    requirePattern(dst, guard, payload, 0x37, "cross-command-buffer payload");
    requirePending(producer, producerCompletion, "producer terminal fence");
    gate.signaledValue = 1;
    await(producer, producerCompletion, "cross-command-buffer producer terminal");
    requirePattern(src, 0, guard, 0xa5, "cross-command-buffer source guard");
    requirePattern(src, guard + payload, bytes - guard - payload, 0xa5,
                   "cross-command-buffer source tail");
    requirePattern(dst, 0, guard, 0x5a, "cross-command-buffer destination guard");
    requirePattern(dst, guard + payload, bytes - guard - payload, 0x5a,
                   "cross-command-buffer destination tail");
    printf("FENCE_EXECUTION_CASE_PASS cross_command_buffer producer_first update_before_wait guards\n");
}

static void iosurfaceFence(id<MTLDevice> device, id<MTLCommandQueue> producerQueue,
                           id<MTLCommandQueue> consumerQueue) {
    const NSUInteger width = 64, height = 8, pitch = 256;
    const NSUInteger imageBytes = pitch * height, bytes = imageBytes + 512, offset = 256;
    NSDictionary *properties = @{
        (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(pitch),
        (id)kIOSurfaceAllocSize: @(pitch * height), (id)kIOSurfacePixelFormat: @0x42475241
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    require(surface != NULL, "IOSurface creation");
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    id<MTLBuffer> src = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    id<MTLSharedEvent> producerReady = [device newSharedEvent];
    id<MTLSharedEvent> cpuReady = [device newSharedEvent];
    id<MTLSharedEvent> producerGate = [device newSharedEvent];
    require(texture && src && dst && fence && producerReady && cpuReady && producerGate,
            "IOSurface fence resources");
    memset(src.contents, 0x37, bytes);
    memset(dst.contents, 0x5e, bytes);
    require(IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface initial lock");
    memset(IOSurfaceGetBaseAddress(surface), 0x24, imageBytes);
    require(IOSurfaceUnlock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface initial unlock");

    id<MTLCommandBuffer> producer = [producerQueue commandBuffer];
    id<MTLBlitCommandEncoder> write = [producer blitCommandEncoder];
    [write copyFromBuffer:src sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:imageBytes
        sourceSize:MTLSizeMake(width, height, 1) toTexture:texture destinationSlice:0
        destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [write updateFence:fence];
    [write endEncoding];
    [producer encodeSignalEvent:producerReady value:1];
    [producer encodeWaitForEvent:producerGate value:1];
    FenceExecutionCompletion *producerCompletion = observe(producer);
    [producer commit];

    id<MTLCommandBuffer> consumer = [consumerQueue commandBuffer];
    id<MTLBlitCommandEncoder> wait = [consumer blitCommandEncoder];
    [wait waitForFence:fence];
    [wait endEncoding];
    [consumer encodeWaitForEvent:cpuReady value:1];
    id<MTLBlitCommandEncoder> readback = [consumer blitCommandEncoder];
    [readback copyFromTexture:texture sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:dst destinationOffset:offset destinationBytesPerRow:pitch
          destinationBytesPerImage:imageBytes];
    [readback endEncoding];
    FenceExecutionCompletion *consumerCompletion = observe(consumer);
    [consumer commit];

    require([producerReady waitUntilSignaledValue:1 timeoutMS:5000],
            "producerReady notification");
    require(IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface GPU-result lock");
    uint8_t *base = IOSurfaceGetBaseAddress(surface);
    require(base != NULL, "IOSurface mapping");
    for (NSUInteger i = 0; i < imageBytes; ++i) require(base[i] == 0x37, "IOSurface GPU writeback");
    memset(base, 0x82, imageBytes / 2);
    require(IOSurfaceUnlock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface CPU-result unlock");

    cpuReady.signaledValue = 1;
    await(consumer, consumerCompletion, "IOSurface fence consumer");
    requirePending(producer, producerCompletion, "IOSurface producer terminal gate");
    requirePattern(dst, offset, imageBytes / 2, 0x82,
                   "IOSurface CPU update readback");
    requirePattern(dst, offset + imageBytes / 2, imageBytes / 2, 0x37,
                   "IOSurface untouched half");
    requirePattern(dst, 0, offset, 0x5e, "IOSurface destination guard");
    requirePattern(dst, offset + imageBytes, bytes - offset - imageBytes, 0x5e,
                   "IOSurface destination tail");
    producerGate.signaledValue = 1;
    await(producer, producerCompletion, "IOSurface producer terminal");
    printf("FENCE_EXECUTION_CASE_PASS iosurface_fence path=%s producer_first cpu_update guards\n",
           getenv("RDNA4M_IOS_PATH") ? getenv("RDNA4M_IOS_PATH") : "a");
    CFRelease(surface);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: test-fence-execution BUNDLE|--reference a|b\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        if (strcmp(argv[2], "a") && strcmp(argv[2], "b")) return 2;
        id<MTLDevice> device = nil;
        if (!strcmp(argv[1], "--reference")) {
            unsetenv("RDNA4M_IOS_PATH");
            device = MTLCreateSystemDefaultDevice();
        } else {
            if (!strcmp(argv[2], "b")) setenv("RDNA4M_IOS_PATH", "b", 1);
            else unsetenv("RDNA4M_IOS_PATH");
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RADV_DARWIN_MOCK", "0", 1);
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], "bundle loading");
            device = [[objc_getClass("GarnetDevice") alloc] init];
        }
        require(device != nil, "device creation");
        id<MTLCommandQueue> q1 = [device newCommandQueue];
        id<MTLCommandQueue> q2 = [device newCommandQueue];
        require(q1 && q2, "command queues");
        id<MTLLibrary> library = executionLibrary(device);
        renderToBlit(device, q1, library);
        clearOnlyProducer(device, q1, q2, q2);
        freshFenceWaitNoUpdate(device, q1);
        blitComputeBlit(device, q1, library);
        crossCommandBuffer(device, q1, q2);
        iosurfaceFence(device, q1, q2);
        printf("FENCE_EXECUTION_PASS real GPU data dependencies path=%s\n", argv[2]);
        _exit(0);
    }
}
