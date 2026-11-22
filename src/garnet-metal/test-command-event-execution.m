#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <unistd.h>

enum { bytes = 1280, guard = 256, payload = 768 };

@interface EventCompletion : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned count;
    NSError *error;
    MTLCommandBufferStatus status;
}
@end
@implementation EventCompletion
@end

static void require(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "EVENT_EXECUTION_FAIL %s\n", message); _exit(1); }
}

static BOOL pattern(id<MTLBuffer> buffer, NSUInteger start, NSUInteger size, uint8_t value) {
    const uint8_t *p = buffer.contents;
    if (!p || start > buffer.length || size > buffer.length - start) return NO;
    for (NSUInteger i = start; i < start + size; i++) if (p[i] != value) return NO;
    return YES;
}

static EventCompletion *observe(id<MTLCommandBuffer> command) {
    require(command != nil, "command buffer creation");
    EventCompletion *completion = [EventCompletion new];
    completion->done = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        completion->error = cb.error; completion->status = cb.status;
        atomic_fetch_add(&completion->count, 1);
        dispatch_semaphore_signal(completion->done);
    }];
    return completion;
}

static void await(EventCompletion *completion) {
    require(!dispatch_semaphore_wait(completion->done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)),
            "bounded command completion");
    require(atomic_load(&completion->count) == 1 && !completion->error &&
            completion->status == MTLCommandBufferStatusCompleted, "successful completion exactly once");
}

static void fill(id<MTLCommandBuffer> command, id<MTLBuffer> buffer, uint8_t value) {
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    require(encoder != nil, "blit encoder creation");
    [encoder fillBuffer:buffer range:NSMakeRange(guard, payload) value:value];
    [encoder endEncoding];
}

static void copy(id<MTLCommandBuffer> command, id<MTLBuffer> src, id<MTLBuffer> dst) {
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    require(encoder != nil, "copy encoder creation");
    [encoder copyFromBuffer:src sourceOffset:guard toBuffer:dst destinationOffset:guard size:payload];
    [encoder endEncoding];
}

static void surfaceHandshake(id<MTLDevice> device, id<MTLCommandQueue> queue, BOOL readAgain) {
    NSDictionary *properties = @{ (id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @8,
        (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256,
        (id)kIOSurfaceAllocSize: @4096, (id)kIOSurfacePixelFormat: @0x42475241 };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    require(surface != NULL, "IOSurface creation");
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:64 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    id<MTLBuffer> src = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    require(texture && src.contents && dst.contents, "IOSurface handshake resources");
    memset(src.contents, 0x24, 4096); memset(dst.contents, 0x5e, 4096);
    id<MTLSharedEvent> event = [device newSharedEvent];
    require(event != nil, "IOSurface shared event");
    MTLSharedEventListener *listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("rdna4.event.surface", DISPATCH_QUEUE_SERIAL)];
    __block BOOL sawGPU = NO;
    [event notifyListener:listener atValue:1 block:^(id<MTLSharedEvent> shared, uint64_t value) {
        (void)value;
        require(IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface CPU lock");
        uint8_t *base = IOSurfaceGetBaseAddress(surface);
        sawGPU = base != NULL;
        for (unsigned i = 0; i < 2048 && sawGPU; i++) if (base[i] != 0x24) sawGPU = NO;
        if (base) memset(base, 0x82, 2048);
        require(IOSurfaceUnlock(surface, 0, NULL) == kIOReturnSuccess, "IOSurface CPU unlock");
        shared.signaledValue = 2;
    }];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    require(encoder != nil, "IOSurface write encoder");
    [encoder copyFromBuffer:src sourceOffset:256 sourceBytesPerRow:256 sourceBytesPerImage:2048
        sourceSize:MTLSizeMake(64, 8, 1) toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
    [command encodeSignalEvent:event value:1];
    [command encodeWaitForEvent:event value:2];
    if (readAgain) {
        encoder = [command blitCommandEncoder];
        require(encoder != nil, "IOSurface read encoder");
        [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(64, 8, 1) toBuffer:dst destinationOffset:256
            destinationBytesPerRow:256 destinationBytesPerImage:2048];
        [encoder endEncoding];
    }
    EventCompletion *done = observe(command);
    [command commit]; await(done);
    require(sawGPU, "IOSurface CPU notification sees writes at the signal position");
    if (readAgain) require(pattern(dst, 256, 2048, 0x82), "later segment uploads CPU changes to the same IOSurface");
    require(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, "final IOSurface CPU lock");
    const uint8_t *base = IOSurfaceGetBaseAddress(surface);
    BOOL preserved = base != NULL;
    for (unsigned i = 0; i < 2048 && preserved; i++) if (base[i] != 0x82) preserved = NO;
    require(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, "final IOSurface CPU unlock");
    require(preserved, "terminal completion preserves CPU changes after an early signal");
    require(pattern(dst, 0, 256, 0x5e) && pattern(dst, 2304, 1792, 0x5e), "IOSurface copy guards");
    CFRelease(surface);
    printf("EVENT_EXECUTION_CASE_PASS iosurface_cpu_handshake later_texture_use=%d guards\n", readAgain);
}

static void run(id<MTLDevice> device) {
    id<MTLCommandQueue> q1 = [device newCommandQueue], q2 = [device newCommandQueue];
    id<MTLBuffer> src = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> tail = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(q1 && q2 && src.contents && dst.contents && tail.contents, "shared resources");
    memset(src.contents, 0xa5, bytes); memset(dst.contents, 0x5a, bytes); memset(tail.contents, 0x6b, bytes);

    id<MTLSharedEvent> shared = [device newSharedEvent];
    require(shared != nil, "shared event creation");
    MTLSharedEventListener *listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("rdna4.event.execution", DISPATCH_QUEUE_SERIAL)];
    __block BOOL hostSawGPU = NO;
    __block unsigned notifications = 0;
    [shared notifyListener:listener atValue:2 block:^(id<MTLSharedEvent> event, uint64_t value) {
        hostSawGPU = value >= 2 && pattern(src, guard, payload, 0x37);
        notifications++;
        memset((uint8_t *)src.contents + guard, 0x94, payload / 2);
        event.signaledValue = 3;
    }];
    id<MTLCommandBuffer> same = [q1 commandBuffer];
    fill(same, src, 0x37);
    [same encodeSignalEvent:shared value:2];
    [same encodeWaitForEvent:shared value:3];
    copy(same, src, dst);
    EventCompletion *sameDone = observe(same);
    [same commit]; await(sameDone);
    require(hostSawGPU && notifications == 1, "CPU notification sees prior GPU writes");
    require(pattern(dst, guard, payload / 2, 0x94) && pattern(dst, guard + payload / 2, payload / 2, 0x37),
            "GPU reads CPU writes after shared-event wake");
    puts("EVENT_EXECUTION_CASE_PASS shared_cpu_handshake order=signal_cpu_wait_copy");

    id<MTLEvent> plain = [device newEvent];
    id<MTLCommandBuffer> plainCommand = [q1 commandBuffer];
    require(plain != nil, "plain event creation");
    fill(plainCommand, src, 0x48);
    [plainCommand encodeSignalEvent:plain value:1];
    [plainCommand encodeWaitForEvent:plain value:1];
    copy(plainCommand, src, dst);
    EventCompletion *plainDone = observe(plainCommand);
    [plainCommand commit]; await(plainDone);
    require(pattern(dst, guard, payload, 0x48), "same-CB plain-event output");
    puts("EVENT_EXECUTION_CASE_PASS plain_same_cb");

    id<MTLSharedEvent> ready = [device newSharedEvent], gate = [device newSharedEvent];
    require(ready && gate, "cross-queue events");
    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    [consumer encodeWaitForEvent:ready value:1]; copy(consumer, src, dst);
    EventCompletion *consumerDone = observe(consumer);
    [consumer commit];
    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    fill(producer, src, 0xc3);
    [producer encodeSignalEvent:ready value:1];
    [producer encodeWaitForEvent:gate value:1];
    EventCompletion *producerDone = observe(producer);
    [producer commit];
    id<MTLCommandBuffer> later = [q1 commandBuffer]; fill(later, tail, 0x7d);
    EventCompletion *laterDone = observe(later); [later commit];
    await(consumerDone);
    require(pattern(dst, guard, payload, 0xc3), "consumer-first cross-queue output");
    require(!atomic_load(&producerDone->count) && !atomic_load(&laterDone->count),
            "early signal and same-queue head order");
    require(pattern(tail, 0, bytes, 0x6b), "later same-queue work stayed blocked");
    gate.signaledValue = 1;
    await(producerDone); await(laterDone);
    require(pattern(tail, guard, payload, 0x7d), "same-queue tail progresses after CPU wake");
    require(pattern(src, 0, guard, 0xa5) && pattern(src, guard + payload, guard, 0xa5) &&
            pattern(dst, 0, guard, 0x5a) && pattern(dst, guard + payload, guard, 0x5a) &&
            pattern(tail, 0, guard, 0x6b) && pattern(tail, guard + payload, guard, 0x6b), "all buffer guards");
    puts("EVENT_EXECUTION_CASE_PASS consumer_first early_signal queue_head guards");
    surfaceHandshake(device, q1, YES);
    surfaceHandshake(device, q1, NO);
    puts("EVENT_EXECUTION_PASS GPU buffer data and CPU shared-event handshake");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        id<MTLDevice> device;
        if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RADV_DARWIN_MOCK", "0", 1);
            NSError *error = nil;
            require([[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error], "bundle loading");
            device = [[objc_getClass("GarnetDevice") alloc] init];
        }
        require(device != nil, "device creation");
        run(device);
        _exit(0);
    }
}
