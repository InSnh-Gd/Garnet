// Real GPU acceptance for production automatic heaps. This test must run from
// the GarnetMetal bundle on the target Mac for candidate semantics; --reference
// runs the public-API subset against MTLCreateSystemDefaultDevice. The simulated
// Vulkan harness is test-heap-execution.m and does not establish real backing.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Internal heap probes are candidate-only. --reference must never call them.
@interface NSObject (HeapGPUProbe)
- (uint64_t)rdna4BackingBytes;
- (uint64_t)rdna4BackingInUse;
- (uint64_t)rdna4BackingBlock;
- (uint64_t)rdna4BackingOffset;
@end

@interface HeapGPUCompletion : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end

@implementation HeapGPUCompletion
@end

static BOOL gReferenceMode;
static id<MTLComputePipelineState> gByteWritePipeline;
static id<MTLComputePipelineState> gByteCopyPipeline;

static void failf(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void failf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("HEAP_GPU_FAIL ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    _exit(1);
}

static void require(BOOL condition, const char *message) {
    if (!condition) failf("%s", message);
}

static const char *modeName(void) {
    return gReferenceMode ? "reference" : "candidate";
}

static void requireCandidateProbe(const char *probe) {
    if (gReferenceMode)
        failf("reference mode attempted candidate-only private probe %s", probe);
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment && !(alignment & (alignment - 1)),
            "heap alignment is not a power of two");
    require(value <= NSUIntegerMax - (alignment - 1), "heap size alignment overflow");
    return (value + alignment - 1) & ~(alignment - 1);
}

static NSUInteger checkedAdd(NSUInteger left, NSUInteger right, const char *what) {
    if (left > NSUIntegerMax - right) failf("%s size overflow", what);
    return left + right;
}

static uint64_t backingBytes(id<MTLHeap> heap) {
    requireCandidateProbe("rdna4BackingBytes");
    NSObject *probe = (NSObject *)heap;
    require([probe respondsToSelector:@selector(rdna4BackingBytes)],
            "test requires the GarnetMetal heap backing probe");
    return [probe rdna4BackingBytes];
}

static uint64_t backingInUse(id<MTLHeap> heap) {
    requireCandidateProbe("rdna4BackingInUse");
    NSObject *probe = (NSObject *)heap;
    require([probe respondsToSelector:@selector(rdna4BackingInUse)],
            "test requires the GarnetMetal heap in-use probe");
    return [probe rdna4BackingInUse];
}

static uint64_t backingBlock(id<MTLBuffer> buffer) {
    requireCandidateProbe("rdna4BackingBlock");
    NSObject *probe = (NSObject *)buffer;
    require([probe respondsToSelector:@selector(rdna4BackingBlock)],
            "test requires the GarnetMetal buffer block probe");
    return [probe rdna4BackingBlock];
}

static uint64_t backingOffset(id<MTLBuffer> buffer) {
    requireCandidateProbe("rdna4BackingOffset");
    NSObject *probe = (NSObject *)buffer;
    require([probe respondsToSelector:@selector(rdna4BackingOffset)],
            "test requires the GarnetMetal buffer offset probe");
    return [probe rdna4BackingOffset];
}

static void requireDeviceAllocation(id<MTLDevice> device, NSUInteger expected,
                                    const char *name) {
    NSUInteger actual = device.currentAllocatedSize;
    for (unsigned attempt = 0; actual != expected && attempt < 200; attempt++) {
        usleep(10 * 1000);
        actual = device.currentAllocatedSize;
    }
    if (actual != expected)
        failf("%s device currentAllocatedSize actual=%lu expected=%lu", name,
              (unsigned long)actual, (unsigned long)expected);
}

static id<MTLHeap> makeHeap(id<MTLDevice> device, NSUInteger size, NSString *label) {
    MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
    descriptor.type = MTLHeapTypeAutomatic;
    descriptor.size = size;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeDefault;
    id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
    require(heap != nil, "automatic heap creation");
    heap.label = label;
    return heap;
}

static MTLTextureDescriptor *textureDescriptor(MTLPixelFormat format,
                                                NSUInteger width, NSUInteger height) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:width
                                                          height:height
                                                        mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return descriptor;
}

static NSString *byteTailShaderSource(void) {
    return @"#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "kernel void heap_gpu_byte_write(device uchar *target [[buffer(0)]],\n"
            "                                constant uint &offset [[buffer(1)]],\n"
            "                                constant uint &value [[buffer(2)]],\n"
            "                                uint tid [[thread_position_in_grid]]) {\n"
            "    if (tid == 0) target[offset] = uchar(value & 0xffu);\n"
            "}\n"
            "kernel void heap_gpu_byte_copy(device const uchar *source [[buffer(0)]],\n"
            "                               device uchar *destination [[buffer(1)]],\n"
            "                               constant uint &offset [[buffer(2)]],\n"
            "                               uint tid [[thread_position_in_grid]]) {\n"
            "    if (tid == 0) destination[offset] = source[offset];\n"
            "}\n";
}

static id<MTLComputePipelineState> makeComputePipeline(
    id<MTLDevice> device, id<MTLLibrary> library, NSString *functionName) {
    id<MTLFunction> function = [library newFunctionWithName:functionName];
    require(function != nil, "byte-tail shader function missing");
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline)
        failf("byte-tail pipeline %s: %s", functionName.UTF8String,
              error.localizedDescription.UTF8String ?: "creation failed");
    function = nil;
    return pipeline;
}

static void makeByteTailPipelines(id<MTLDevice> device) {
    NSError *error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:byteTailShaderSource() options:nil error:&error];
    require(library != nil, error.localizedDescription.UTF8String ?:
                                "byte-tail shader compilation failed");
    gByteWritePipeline =
        makeComputePipeline(device, library, @"heap_gpu_byte_write");
    gByteCopyPipeline =
        makeComputePipeline(device, library, @"heap_gpu_byte_copy");
    library = nil;
}

static HeapGPUCompletion *observe(id<MTLCommandBuffer> command) {
    require(command != nil, "command buffer creation");
    HeapGPUCompletion *observation = [HeapGPUCompletion new];
    observation->done = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        observation->status = completed.status;
        observation->error = completed.error;
        atomic_fetch_add(&observation->callbacks, 1);
        dispatch_semaphore_signal(observation->done);
    }];
    return observation;
}

static void awaitCommand(id<MTLCommandBuffer> command,
                         HeapGPUCompletion *observation, const char *name) {
    if (dispatch_semaphore_wait(observation->done,
                                dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)))
        failf("%s timed out", name);
    require(atomic_load(&observation->callbacks) == 1,
            "command completion callback count");
    if (command.status != MTLCommandBufferStatusCompleted ||
        observation->status != MTLCommandBufferStatusCompleted || observation->error)
        failf("%s status=%lu observed=%lu error=%s", name,
              (unsigned long)command.status, (unsigned long)observation->status,
              observation->error.localizedDescription.UTF8String ?: "none");
}

static void requirePending(id<MTLCommandBuffer> command,
                           HeapGPUCompletion *observation, const char *name) {
    usleep(150000);
    if (atomic_load(&observation->callbacks) != 0 ||
        command.status == MTLCommandBufferStatusCompleted)
        failf("%s completed before its shared-event gate", name);
}

static void requirePattern(id<MTLBuffer> buffer, NSUInteger length, uint8_t expected,
                           const char *name) {
    const uint8_t *bytes = buffer.contents;
    if (!bytes || length > buffer.length)
        failf("%s has no readable CPU range", name);
    for (NSUInteger i = 0; i < length; i++) {
        if (bytes[i] != expected)
            failf("%s first_bad_offset=%lu actual=0x%02x expected=0x%02x",
                  name, (unsigned long)i, bytes[i], expected);
    }
}

static void requireRepeatedBytes(id<MTLBuffer> buffer, NSUInteger length,
                                 const uint8_t *pixel, NSUInteger pixelBytes,
                                 const char *name) {
    const uint8_t *bytes = buffer.contents;
    if (!bytes || !pixelBytes || length > buffer.length || length % pixelBytes)
        failf("%s has an invalid readback extent", name);
    for (NSUInteger i = 0; i < length; i += pixelBytes) {
        if (memcmp(bytes + i, pixel, pixelBytes))
            failf("%s first_bad_pixel=%lu", name,
                  (unsigned long)(i / pixelBytes));
    }
}

static void fillCopyReadback(id<MTLCommandQueue> queue, id<MTLBuffer> source,
                             id<MTLBuffer> readback, NSUInteger length,
                             uint8_t pattern, const char *name) {
    @autoreleasepool {
        require(length && !(length & 3), "fill range must be four-byte aligned");
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, "buffer fill blit encoder");
        [blit fillBuffer:source range:NSMakeRange(0, length) value:pattern];
        [blit copyFromBuffer:source sourceOffset:0
                        toBuffer:readback destinationOffset:0
                            size:length];
        [blit endEncoding];
        blit = nil;
        HeapGPUCompletion *completion = observe(command);
        [command commit];
        awaitCommand(command, completion, name);
        requirePattern(readback, length, pattern, name);
        command = nil;
        completion = nil;
    }
}

static void textureClearReadback(id<MTLCommandQueue> queue, id<MTLTexture> texture,
                                 id<MTLBuffer> readback, MTLClearColor color,
                                 const uint8_t *pixel, NSUInteger pixelBytes,
                                 const char *name) {
    @autoreleasepool {
        NSUInteger bytesPerRow = texture.width * pixelBytes;
        NSUInteger length = bytesPerRow * texture.height;
        require(readback.length >= length, "texture readback is too small");
        id<MTLCommandBuffer> command = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = color;
        id<MTLRenderCommandEncoder> render =
            [command renderCommandEncoderWithDescriptor:pass];
        require(render != nil, "heap texture clear encoder");
        [render endEncoding];
        render = nil;
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, "heap texture readback encoder");
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                     toBuffer:readback
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:readback.length];
        [blit endEncoding];
        blit = nil;
        HeapGPUCompletion *completion = observe(command);
        [command commit];
        awaitCommand(command, completion, name);
        requireRepeatedBytes(readback, length, pixel, pixelBytes, name);
        command = nil;
        completion = nil;
    }
}

static void writeAndReadByteTail(id<MTLCommandQueue> queue,
                                 id<MTLBuffer> source,
                                 id<MTLBuffer> readback,
                                 NSUInteger offset, uint8_t value,
                                 const char *name) {
    @autoreleasepool {
        require(offset < source.length && offset < readback.length,
                "byte-tail extent exceeds a buffer");
        require(offset <= UINT32_MAX, "byte-tail offset does not fit the shader ABI");
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(compute != nil, "byte-tail compute encoder");

        [compute setComputePipelineState:gByteWritePipeline];
        [compute setBuffer:source offset:0 atIndex:0];
        uint32_t shaderOffset = (uint32_t)offset;
        uint32_t shaderValue = value;
        [compute setBytes:&shaderOffset length:sizeof shaderOffset atIndex:1];
        [compute setBytes:&shaderValue length:sizeof shaderValue atIndex:2];
        [compute dispatchThreads:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

        [compute memoryBarrierWithScope:MTLBarrierScopeBuffers];
        [compute setComputePipelineState:gByteCopyPipeline];
        [compute setBuffer:source offset:0 atIndex:0];
        [compute setBuffer:readback offset:0 atIndex:1];
        [compute setBytes:&shaderOffset length:sizeof shaderOffset atIndex:2];
        [compute dispatchThreads:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        compute = nil;
        HeapGPUCompletion *completion = observe(command);
        [command commit];
        awaitCommand(command, completion, name);
        command = nil;
        completion = nil;
    }
}

static void testMixedRequirements(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSUInteger deviceBaseline = device.currentAllocatedSize;
    @autoreleasepool {
        // The final byte is 4-byte-tail misaligned: blit only fills the legal
        // prefix and the compute kernel supplies/readbacks the one-byte tail.
        const NSUInteger bufferLength = (64 * 1024) + 5;
        const NSUInteger blitLength = bufferLength & ~(NSUInteger)3;
        require(bufferLength - blitLength == 1,
                "byte-tail fixture must leave exactly one byte");
        MTLResourceOptions privateOptions =
            (MTLResourceOptions)MTLResourceStorageModePrivate;
        MTLSizeAndAlign compactSize =
            [device heapBufferSizeAndAlignWithLength:1000 options:privateOptions];
        MTLSizeAndAlign bufferSize =
            [device heapBufferSizeAndAlignWithLength:bufferLength
                                             options:privateOptions];
        MTLTextureDescriptor *bgraDescriptor =
            textureDescriptor(MTLPixelFormatBGRA8Unorm, 16, 16);
        MTLTextureDescriptor *halfDescriptor =
            textureDescriptor(MTLPixelFormatRGBA16Float, 8, 8);
        MTLSizeAndAlign bgraSize =
            [device heapTextureSizeAndAlignWithDescriptor:bgraDescriptor];
        MTLSizeAndAlign halfSize =
            [device heapTextureSizeAndAlignWithDescriptor:halfDescriptor];
        require(compactSize.size >= 1000 && compactSize.align &&
                    !(compactSize.align & (compactSize.align - 1)),
                "invalid compact heap buffer SizeAndAlign");
        require(bufferSize.size >= bufferLength && bufferSize.align &&
                    !(bufferSize.align & (bufferSize.align - 1)),
                "invalid heap buffer SizeAndAlign");
        require(bgraSize.size && bgraSize.align && halfSize.size && halfSize.align,
                "invalid heap texture SizeAndAlign");

        NSUInteger required = checkedAdd(compactSize.size, bufferSize.size,
                                         "mixed heap resources");
        required = checkedAdd(required, bgraSize.size, "mixed heap resources");
        required = checkedAdd(required, halfSize.size, "mixed heap resources");
        NSUInteger heapSize = alignUp(checkedAdd(required, 1 << 20,
                                                 "mixed heap padding"),
                                      16 * 1024);
        id<MTLHeap> heap = makeHeap(device, heapSize, @"heap-gpu-mixed");
        if (!gReferenceMode)
            require(backingBytes(heap) == 0 && heap.usedSize == 0,
                    "new heap acquired backing eagerly");
        else
            require(heap.usedSize == 0, "new reference heap has nonzero usedSize");
        require(heap.currentAllocatedSize == heapSize,
                "heap currentAllocatedSize is not its capacity");

        id<MTLBuffer> compact =
            [heap newBufferWithLength:1000 options:privateOptions];
        require(compact != nil, "compact heap resource allocation");
        NSUInteger usedAfterCompact = heap.usedSize;
        NSUInteger availableAfterCompact =
            [heap maxAvailableSizeWithAlignment:256];
        id<MTLBuffer> buffer =
            [heap newBufferWithLength:bufferLength options:privateOptions];
        id<MTLTexture> bgra = [heap newTextureWithDescriptor:bgraDescriptor];
        id<MTLTexture> half = [heap newTextureWithDescriptor:halfDescriptor];
        require(buffer && bgra && half, "mixed heap resource allocation");
        require(heap.usedSize > 0, "mixed heap usage was not reported");
        NSUInteger heapUsed = heap.usedSize;
        NSUInteger heapAvailable =
            [heap maxAvailableSizeWithAlignment:bufferSize.align];
        int64_t layoutBackingBytes = -1;
        if (!gReferenceMode) {
            uint64_t physical = backingBytes(heap);
            uint64_t physicalInUse = backingInUse(heap);
            require(physical > 0 && physicalInUse > 0 &&
                        physicalInUse <= physical,
                    "mixed heap physical backing accounting");
            layoutBackingBytes = (int64_t)physical;
        }

        // One machine-readable line distinguishes public policy from private
        // backing. In reference mode -1 means the probe was deliberately skipped.
        printf("HEAP_GPU_LAYOUT mode=%s buffer_length=%lu "
               "query_1000_size=%lu query_1000_align=%lu "
               "resource_1000_allocatedSize=%lu "
               "heap_used_after_1000=%lu heap_available_after_1000_align_256=%lu "
               "query_buffer_size=%lu query_buffer_align=%lu "
               "resource_buffer_allocatedSize=%lu "
               "query_bgra_size=%lu query_bgra_align=%lu "
               "resource_bgra_allocatedSize=%lu "
               "query_half_size=%lu query_half_align=%lu "
               "resource_half_allocatedSize=%lu "
               "heap_size=%lu heap_usedSize=%lu "
               "heap_availableSize=%lu heap_availableAlignment=%lu "
               "heap_currentAllocatedSize=%lu heap_backingBytes=%lld "
               "backing_probe=%s\n",
               modeName(), (unsigned long)bufferLength,
               (unsigned long)compactSize.size, (unsigned long)compactSize.align,
               (unsigned long)compact.allocatedSize,
               (unsigned long)usedAfterCompact, (unsigned long)availableAfterCompact,
               (unsigned long)bufferSize.size, (unsigned long)bufferSize.align,
               (unsigned long)buffer.allocatedSize,
               (unsigned long)bgraSize.size, (unsigned long)bgraSize.align,
               (unsigned long)bgra.allocatedSize,
               (unsigned long)halfSize.size, (unsigned long)halfSize.align,
               (unsigned long)half.allocatedSize,
               (unsigned long)heapSize, (unsigned long)heapUsed,
               (unsigned long)heapAvailable, (unsigned long)bufferSize.align,
               (unsigned long)heap.currentAllocatedSize,
               (long long)layoutBackingBytes,
               gReferenceMode ? "skipped-reference" : "candidate-private");
        compact = nil;

        id<MTLBuffer> bufferReadback =
            [device newBufferWithLength:bufferLength
                                options:MTLResourceStorageModeShared];
        id<MTLBuffer> bgraReadback =
            [device newBufferWithLength:16 * 16 * 4
                                options:MTLResourceStorageModeShared];
        id<MTLBuffer> halfReadback =
            [device newBufferWithLength:8 * 8 * 8
                                options:MTLResourceStorageModeShared];
        require(bufferReadback && bgraReadback && halfReadback,
                "mixed heap CPU readback buffers");
        NSUInteger deviceSetup = device.currentAllocatedSize;

        fillCopyReadback(queue, buffer, bufferReadback, blitLength, 0x5b,
                         "heap mixed buffer fill");
        const uint8_t bgraPixel[4] = {191, 128, 64, 255};
        textureClearReadback(queue, bgra, bgraReadback,
                             MTLClearColorMake(0.25, 0.5, 0.75, 1.0),
                             bgraPixel, sizeof bgraPixel,
                             "heap BGRA8 clear");
        const uint8_t halfPixel[8] = {0x00, 0x38, 0x00, 0x34,
                                      0x00, 0x00, 0x00, 0x3c};
        textureClearReadback(queue, half, halfReadback,
                             MTLClearColorMake(0.5, 0.25, 0.0, 1.0),
                             halfPixel, sizeof halfPixel,
                             "heap RGBA16Float clear");
        writeAndReadByteTail(queue, buffer, bufferReadback, blitLength, 0xa7,
                            "heap buffer byte-tail write+readback");
        requirePattern(bufferReadback, blitLength, 0x5b,
                       "heap buffer prefix after byte tail");
        require(((const uint8_t *)bufferReadback.contents)[blitLength] == 0xa7,
                "byte-tail compute oracle returned the wrong final byte");
        require(device.currentAllocatedSize == deviceSetup,
                "heap-backed GPU work changed device-wide allocation");

        buffer = nil;
        bgra = nil;
        half = nil;
        bufferReadback = nil;
        bgraReadback = nil;
        halfReadback = nil;
        require(heap.usedSize == 0, "mixed heap resources did not release usage");
        heap = nil;
    }
    requireDeviceAllocation(device, deviceBaseline, "mixed heap cleanup");
    printf("HEAP_GPU_CASE_PASS mode=%s "
           "mixed=byte-tail-buffer+BGRA8+RGBA16Float "
           "requirements=query-driven gpu=fill+clear+byte-tail\n",
           modeName());
}

static void testCapacityFailureAndAliasReuse(id<MTLDevice> device,
                                             id<MTLCommandQueue> queue) {
    NSUInteger deviceBaseline = device.currentAllocatedSize;
    @autoreleasepool {
        const NSUInteger length = (64 * 1024) + 4;
        MTLResourceOptions options =
            (MTLResourceOptions)MTLResourceStorageModePrivate;
        MTLSizeAndAlign sizing =
            [device heapBufferSizeAndAlignWithLength:length options:options];
        require(sizing.size >= length && sizing.align,
                "capacity heap buffer SizeAndAlign");
        id<MTLHeap> heap = makeHeap(device, sizing.size, @"heap-gpu-capacity");
        id<MTLBuffer> first = [heap newBufferWithLength:length options:options];
        require(first != nil, "capacity heap first buffer");
        uint64_t block = 0, offset = 0, physical = 0;
        if (!gReferenceMode) {
            block = backingBlock(first);
            offset = backingOffset(first);
            require(block != 0, "capacity heap first buffer has no backing block");
        }
        NSUInteger used = heap.usedSize;
        if (!gReferenceMode) physical = backingBytes(heap);
        id<MTLBuffer> readback =
            [device newBufferWithLength:length
                                options:MTLResourceStorageModeShared];
        require(readback != nil, "capacity heap readback");
        NSUInteger deviceSetup = device.currentAllocatedSize;

        fillCopyReadback(queue, first, readback, length, 0x41,
                         "capacity heap first fill");
        id<MTLBuffer> blocked = [heap newBufferWithLength:length options:options];
        require(blocked == nil, "full heap accepted a second live buffer");
        require(heap.usedSize == used, "failed allocation changed usedSize");
        if (!gReferenceMode)
            require(backingBytes(heap) == physical,
                    "failed allocation changed physical backing");

        [first makeAliasable];
        require(first.isAliasable && heap.usedSize == 0,
                "completed buffer did not become aliasable");
        NSUInteger available = [heap maxAvailableSizeWithAlignment:sizing.align];
        require(available >= sizing.size,
                "aliasable complete range did not return to the heap");
        id<MTLBuffer> reused = [heap newBufferWithLength:length options:options];
        require(reused != nil, "completed alias reuse allocation");
        if (!gReferenceMode)
            require(backingBlock(reused) == block &&
                        backingOffset(reused) == offset,
                    "completed alias did not reuse its physical range");
        fillCopyReadback(queue, reused, readback, length, 0x42,
                         "capacity heap reused fill");
        if (!gReferenceMode)
            require(backingBytes(heap) == physical,
                    "alias reuse allocated more physical backing");
        require(device.currentAllocatedSize == deviceSetup,
                "alias reuse changed device-wide allocation");

        first = nil;
        reused = nil;
        readback = nil;
        require(heap.usedSize == 0, "capacity heap leaked logical usage");
        heap = nil;
    }
    requireDeviceAllocation(device, deviceBaseline, "capacity heap cleanup");
    if (gReferenceMode)
        puts("HEAP_GPU_CASE_PASS mode=reference "
             "capacity=second-live-refused rollback=unchanged "
             "alias=completed+reused physical=unprobed-reference");
    else
        puts("HEAP_GPU_CASE_PASS mode=candidate "
             "capacity=second-live-refused rollback=unchanged "
             "alias=completed+reused physical=unchanged");
}

static void testInFlightAlias(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSUInteger deviceBaseline = device.currentAllocatedSize;
    @autoreleasepool {
        const NSUInteger length = (64 * 1024) + 4;
        MTLResourceOptions options =
            (MTLResourceOptions)MTLResourceStorageModePrivate;
        MTLSizeAndAlign sizing =
            [device heapBufferSizeAndAlignWithLength:length options:options];
        require(sizing.size >= length && sizing.align,
                "in-flight heap buffer SizeAndAlign");
        id<MTLHeap> heap = makeHeap(device, sizing.size, @"heap-gpu-inflight");
        id<MTLBuffer> source = [heap newBufferWithLength:length options:options];
        id<MTLBuffer> readback =
            [device newBufferWithLength:length
                                options:MTLResourceStorageModeShared];
        id<MTLSharedEvent> gate = [device newSharedEvent];
        require(source && readback && gate, "in-flight alias resources");
        uint64_t block = 0, offset = 0, physical = 0;
        if (!gReferenceMode) {
            block = backingBlock(source);
            offset = backingOffset(source);
            physical = backingBytes(heap);
        }
        NSUInteger deviceSetup = device.currentAllocatedSize;
        NSUInteger availableWhileLive = [heap maxAvailableSizeWithAlignment:sizing.align];
        gate.signaledValue = 0;

        @autoreleasepool {
            id<MTLCommandBuffer> producer = [queue commandBuffer];
            [producer encodeWaitForEvent:gate value:1];
            id<MTLBlitCommandEncoder> blit = [producer blitCommandEncoder];
            require(blit != nil, "in-flight alias blit encoder");
            [blit fillBuffer:source range:NSMakeRange(0, length) value:0x6c];
            [blit copyFromBuffer:source sourceOffset:0
                            toBuffer:readback destinationOffset:0
                                size:length];
            [blit endEncoding];
            blit = nil;
            HeapGPUCompletion *completion = observe(producer);
            [producer commit];
            requirePending(producer, completion, "in-flight alias producer");

            if (!gReferenceMode) {
                // Candidate policy is deliberately stricter than the public API:
                // refusal is observable while the GPU still owns the old range.
                [source makeAliasable];
                require(source.isAliasable && heap.usedSize == 0,
                        "in-flight buffer did not release logical usage");
                require(backingInUse(heap) == physical,
                        "in-flight alias released physical backing too early");
                require([heap maxAvailableSizeWithAlignment:sizing.align] == availableWhileLive,
                        "in-flight alias range was exposed as available");
                source = nil;
                id<MTLBuffer> blocked =
                    [heap newBufferWithLength:length options:options];
                require(blocked == nil, "in-flight alias range was reused");
                require(heap.usedSize == 0 && backingBytes(heap) == physical,
                        "in-flight alias rejection changed heap accounting");
            }

            gate.signaledValue = 1;
            awaitCommand(producer, completion, "in-flight alias producer completion");
            requirePattern(readback, length, 0x6c, "in-flight alias producer data");
            producer = nil;
            completion = nil;
        }

        if (gReferenceMode) {
            // Public reference semantics do not impose the candidate's custom
            // refusal. Alias only after the data oracle has completed.
            [source makeAliasable];
            require(source.isAliasable && heap.usedSize == 0,
                    "reference completed buffer did not become aliasable");
        }
        NSUInteger available = [heap maxAvailableSizeWithAlignment:sizing.align];
        require(available >= sizing.size,
                "completed in-flight alias did not become reusable");
        source = nil;
        BOOL retainedBacking = !gReferenceMode && backingBytes(heap) != 0;
        id<MTLBuffer> replacement =
            [heap newBufferWithLength:length options:options];
        require(replacement != nil,
                "completed in-flight alias replacement allocation");
        if (retainedBacking)
            require(backingBlock(replacement) == block &&
                        backingOffset(replacement) == offset,
                    "completed in-flight alias did not reuse the range");
        fillCopyReadback(queue, replacement, readback, length, 0x7d,
                         "in-flight alias replacement fill");
        if (!gReferenceMode)
            require(backingBytes(heap) == physical,
                    "in-flight alias replacement grew physical backing");
        require(device.currentAllocatedSize == deviceSetup,
                "in-flight alias replacement grew device allocation");

        replacement = nil;
        readback = nil;
        gate = nil;
        require(heap.usedSize == 0, "in-flight heap leaked logical usage");
        heap = nil;
    }
    requireDeviceAllocation(device, deviceBaseline, "in-flight heap cleanup");
    if (gReferenceMode)
        puts("HEAP_GPU_CASE_PASS mode=reference "
             "in_flight=completion-gated data=producer+replacement "
             "alias=reused-public-accounting");
    else
        puts("HEAP_GPU_CASE_PASS mode=candidate "
             "in_flight=refused_until_completion "
             "retirement=completion alias=reclaimed-or-reused gpu_execution=1");
}

static void testAliasCycles(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    // Deliberately before heap/readback allocation. The inner pool must drain
    // every command/resource before this exact baseline is checked.
    NSUInteger deviceBaseline = device.currentAllocatedSize;
    @autoreleasepool {
        const NSUInteger length = (64 * 1024) + 4;
        const unsigned cycles = 4;
        MTLResourceOptions options =
            (MTLResourceOptions)MTLResourceStorageModePrivate;
        MTLSizeAndAlign sizing =
            [device heapBufferSizeAndAlignWithLength:length options:options];
        require(sizing.size >= length && sizing.align,
                "alias-cycle heap buffer SizeAndAlign");
        id<MTLHeap> heap = makeHeap(device, sizing.size, @"heap-gpu-cycles");
        id<MTLBuffer> readback =
            [device newBufferWithLength:length
                                options:MTLResourceStorageModeShared];
        require(readback != nil, "alias-cycle readback");
        NSUInteger deviceSetup = device.currentAllocatedSize;
        NSMutableArray<id<MTLBuffer>> *aliases = [NSMutableArray array];
        uint64_t block = 0, offset = 0, physical = 0;

        for (unsigned cycle = 0; cycle < cycles; cycle++) {
            id<MTLBuffer> buffer =
                [heap newBufferWithLength:length options:options];
            require(buffer != nil, "alias-cycle buffer allocation");
            if (!cycle) {
                if (!gReferenceMode) {
                    block = backingBlock(buffer);
                    offset = backingOffset(buffer);
                    physical = backingBytes(heap);
                    require(block != 0 && physical > 0,
                            "alias-cycle first backing identity");
                }
            } else if (!gReferenceMode) {
                require(backingBlock(buffer) == block &&
                            backingOffset(buffer) == offset,
                        "alias cycle changed physical identity");
            }
            fillCopyReadback(queue, buffer, readback, length,
                             (uint8_t)(0x20 + cycle), "alias-cycle GPU fill");
            if (!gReferenceMode)
                require(backingBytes(heap) == physical,
                        "alias-cycle fill allocated more backing");
            require(device.currentAllocatedSize == deviceSetup,
                    "alias-cycle GPU work changed device allocation");
            [buffer makeAliasable];
            require(buffer.isAliasable && heap.usedSize == 0,
                    "alias-cycle resource did not become aliasable");
            require([heap maxAvailableSizeWithAlignment:sizing.align] >= sizing.size,
                    "alias-cycle range did not remain reusable");
            [aliases addObject:buffer];
            buffer = nil;
            require(device.currentAllocatedSize == deviceSetup,
                    "alias-cycle retirement changed device allocation");
        }

        [aliases removeAllObjects];
        aliases = nil;
        require(heap.usedSize == 0, "alias-cycle heap leaked logical usage");
        if (!gReferenceMode)
            require(backingBytes(heap) == 0,
                    "released alias handles did not reclaim physical backing");
        heap = nil;
        readback = nil;
    }
    requireDeviceAllocation(device, deviceBaseline, "alias cycles cleanup");
    printf("HEAP_GPU_CASE_PASS mode=%s alias_cycles=4 "
           "range=%s device_growth=0 gpu_execution=1\n",
           modeName(), gReferenceMode ? "public-accounting" : "one-physical-block");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-heap-gpu BUNDLE|--reference\n"
                 "Runs real automatic-heap allocation, aliasing and GPU data "
                 "tests. BUNDLE uses candidate semantics and private probes; "
                 "--reference uses MTLCreateSystemDefaultDevice and public APIs.");
            return 0;
        }
        if (argc != 2) {
            fprintf(stderr, "Usage: test-heap-gpu BUNDLE|--reference\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        gReferenceMode = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = nil;
        if (gReferenceMode) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            if (!bundle || ![bundle loadAndReturnError:&error])
                failf("bundle loading: %s",
                      error.localizedDescription.UTF8String ?: "unknown error");
            Class deviceClass = [bundle classNamed:@"GarnetDevice"];
            require(deviceClass != Nil, "bundle does not export GarnetDevice");
            unsetenv("RDNA4M_IOS_PATH");
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RADV_DARWIN_MOCK", "0", 1);
            device = [[deviceClass alloc] init];
        }
        require(device != nil, "Metal device creation");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, "command queue creation");
        makeByteTailPipelines(device);

        NSString *deviceName = device.name;
        printf("HEAP_GPU_BEGIN mode=%s device=%s\n", modeName(),
               deviceName.UTF8String ?: "unknown");
        testMixedRequirements(device, queue);
        testCapacityFailureAndAliasReuse(device, queue);
        testInFlightAlias(device, queue);
        testAliasCycles(device, queue);
        printf("HEAP_GPU_PASS mode=%s real_gpu=1 cases=4 layout=1 "
               "backing=%s alias=retirement-gated\n",
               modeName(), gReferenceMode ? "public-reference" :
                                           "requirements-driven");
        gByteWritePipeline = nil;
        gByteCopyPipeline = nil;
        queue = nil;
        device = nil;
        _exit(0);
    }
}
