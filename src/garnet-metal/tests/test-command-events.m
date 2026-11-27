// Run in the macOS VM with RADV_DARWIN_MOCK. Encoding, events, queue ordering and
// callbacks are real; Vulkan submissions and fence waits are simulated.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <unistd.h>

extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_wait_count(void);

@interface NSObject (RDNA4EventTest)
- (void)rdna4Fail:(NSString *)reason;
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value timeout:(uint32_t)timeout;
@end

@interface RDNA4EventObservation : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end
@implementation RDNA4EventObservation
@end

static void fail(NSString *message) {
    fprintf(stderr, "EVENT_SEGMENT_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void fill(id<MTLCommandBuffer> command, id<MTLBuffer> buffer, uint32_t value) {
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:value];
    [encoder endEncoding];
}

static RDNA4EventObservation *observe(id<MTLCommandBuffer> command) {
    RDNA4EventObservation *observation = [RDNA4EventObservation new];
    observation->done = dispatch_semaphore_create(0);
    observation->status = MTLCommandBufferStatusNotEnqueued;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        observation->status = completed.status;
        observation->error = completed.error;
        atomic_fetch_add(&observation->callbacks, 1);
        dispatch_semaphore_signal(observation->done);
    }];
    return observation;
}

static void await(id<MTLCommandBuffer> command, RDNA4EventObservation *observation,
                  MTLCommandBufferStatus want, const char *name) {
    if (dispatch_semaphore_wait(observation->done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        fail([NSString stringWithFormat:@"%s: no completion callback", name]);
    [command waitUntilCompleted];
    usleep(20000);
    if (atomic_load(&observation->callbacks) != 1)
        fail([NSString stringWithFormat:@"%s: %u completion callbacks", name, atomic_load(&observation->callbacks)]);
    if (command.status != want)
        fail([NSString stringWithFormat:@"%s: status %ld, expected %ld", name, (long)command.status, (long)want]);
    if (want == MTLCommandBufferStatusCompleted && observation->error)
        fail([NSString stringWithFormat:@"%s: completion error %@", name, observation->error]);
    if (want == MTLCommandBufferStatusError && !observation->error)
        fail([NSString stringWithFormat:@"%s: error status without an NSError", name]);
}

static void success(id<MTLDevice> device) {
    id<MTLCommandQueue> q1 = [device newCommandQueue];
    id<MTLCommandQueue> q2 = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!q1 || !q2 || !buffer) fail(@"device resources");

    // Same command buffer: work -> signal -> wait -> work. The signal must advance
    // the second segment without waiting for terminal command-buffer completion.
    id<MTLSharedEvent> selfEvent = [device newSharedEvent];
    id<MTLCommandBuffer> same = [q1 commandBuffer];
    fill(same, buffer, 1);
    [same encodeSignalEvent:selfEvent value:1];
    [(id)same encodeWaitForEvent:selfEvent value:1 timeout:2000];
    fill(same, buffer, 2);
    RDNA4EventObservation *sameObservation = observe(same);
    [same commit];
    await(same, sameObservation, MTLCommandBufferStatusCompleted, "same-CB signal then wait");
    if (selfEvent.signaledValue != 1) fail(@"same-CB event value");

    id<MTLEvent> plain = [device newEvent];
    if (!plain) fail(@"plain event");
    id<MTLCommandBuffer> plainCommand = [q1 commandBuffer];
    [plainCommand encodeSignalEvent:plain value:1];
    [(id)plainCommand encodeWaitForEvent:plain value:1 timeout:2000];
    RDNA4EventObservation *plainObservation = observe(plainCommand);
    [plainCommand commit];
    await(plainCommand, plainObservation, MTLCommandBufferStatusCompleted, "plain event signal then wait");

    // Plain MTLEvent has no public signaledValue API. Its backend token must
    // still cross logical queues after the producer fence completes.
    id<MTLCommandBuffer> plainProducer = [q1 commandBuffer];
    fill(plainProducer, buffer, 3);
    [plainProducer encodeSignalEvent:plain value:2];
    RDNA4EventObservation *plainProducerObservation = observe(plainProducer);
    [plainProducer commit];
    id<MTLCommandBuffer> plainConsumer = [q2 commandBuffer];
    [(id)plainConsumer encodeWaitForEvent:plain value:2 timeout:2000];
    fill(plainConsumer, buffer, 4);
    RDNA4EventObservation *plainConsumerObservation = observe(plainConsumer);
    [plainConsumer commit];
    await(plainProducer, plainProducerObservation, MTLCommandBufferStatusCompleted, "plain event producer");
    await(plainConsumer, plainConsumerObservation, MTLCommandBufferStatusCompleted, "plain event consumer");

    // A late signal point must become visible before a later wait in the same
    // command buffer. Holding segment two on hostEvent keeps the CB incomplete.
    id<MTLSharedEvent> position = [device newSharedEvent];
    id<MTLSharedEvent> hostGate = [device newSharedEvent];
    id<MTLCommandBuffer> positioned = [q1 commandBuffer];
    fill(positioned, buffer, 5);
    [positioned encodeSignalEvent:position value:1];
    [(id)positioned encodeWaitForEvent:hostGate value:1 timeout:2000];
    fill(positioned, buffer, 6);
    RDNA4EventObservation *positionedObservation = observe(positioned);
    [positioned commit];
    if (![position waitUntilSignaledValue:1 timeoutMS:2000])
        fail(@"encoded-position signal did not reach the host");
    if (positioned.status == MTLCommandBufferStatusCompleted)
        fail(@"command buffer completed before its later event wait was released");
    hostGate.signaledValue = 1;
    await(positioned, positionedObservation, MTLCommandBufferStatusCompleted, "encoded-position host wait");

    // Consumer committed first, producer committed later from the same thread.
    // The waiting consumer must not occupy the shared submission path.
    id<MTLSharedEvent> cross = [device newSharedEvent];
    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    [(id)consumer encodeWaitForEvent:cross value:1 timeout:2000];
    fill(consumer, buffer, 7);
    RDNA4EventObservation *consumerObservation = observe(consumer);
    [consumer commit];
    usleep(50000);
    if (consumer.status == MTLCommandBufferStatusCompleted)
        fail(@"consumer completed without its producer");
    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    fill(producer, buffer, 8);
    [producer encodeSignalEvent:cross value:1];
    RDNA4EventObservation *producerObservation = observe(producer);
    [producer commit];
    await(producer, producerObservation, MTLCommandBufferStatusCompleted, "cross-queue producer");
    await(consumer, consumerObservation, MTLCommandBufferStatusCompleted, "cross-queue consumer");

    // A later command buffer on the same logical queue cannot pass a blocked head.
    id<MTLSharedEvent> headGate = [device newSharedEvent];
    id<MTLCommandBuffer> head = [q1 commandBuffer];
    [(id)head encodeWaitForEvent:headGate value:1 timeout:2000];
    RDNA4EventObservation *headObservation = observe(head);
    [head commit];
    id<MTLCommandBuffer> later = [q1 commandBuffer];
    fill(later, buffer, 9);
    RDNA4EventObservation *laterObservation = observe(later);
    [later commit];
    usleep(100000);
    if (later.status == MTLCommandBufferStatusCompleted)
        fail(@"later same-queue command buffer passed the blocked head");
    headGate.signaledValue = 1;
    await(head, headObservation, MTLCommandBufferStatusCompleted, "queue head");
    await(later, laterObservation, MTLCommandBufferStatusCompleted, "queue tail");

    printf("EVENT_SEGMENT_PASS mode=success submissions=%u waits=%u; simulated Vulkan only\n",
           rdna4_audit_submission_count(), rdna4_audit_wait_count());
}

static void failure(void) {
    id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
    if (!device) fail(@"device");
    id<MTLCommandQueue> q1 = [device newCommandQueue], q2 = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!q1 || !q2 || !buffer || !event) fail(@"failure resources");

    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    [(id)consumer encodeWaitForEvent:event value:1 timeout:2000];
    fill(consumer, buffer, 1);
    RDNA4EventObservation *consumerObservation = observe(consumer);
    [consumer commit];

    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    [producer encodeSignalEvent:event value:1];
    [(id)producer rdna4Fail:@"injected producer failure"];
    RDNA4EventObservation *producerObservation = observe(producer);
    [producer commit];

    await(producer, producerObservation, MTLCommandBufferStatusError, "failed producer");
    await(consumer, consumerObservation, MTLCommandBufferStatusError, "failed dependent");
    if (event.signaledValue != 0) fail(@"failed producer signaled its event");
    printf("EVENT_SEGMENT_PASS mode=failure submissions=%u waits=%u; simulated Vulkan only\n",
           rdna4_audit_submission_count(), rdna4_audit_wait_count());
}

static void lost(void) {
    id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
    if (!device) fail(@"device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!queue || !buffer || !event) fail(@"lost resources");

    id<MTLCommandBuffer> command = [queue commandBuffer];
    fill(command, buffer, 9);
    [command encodeSignalEvent:event value:7];
    RDNA4EventObservation *observation = observe(command);
    [command commit];
    await(command, observation, MTLCommandBufferStatusError, "lost fence");
    if (event.signaledValue != 0) fail(@"lost fence signaled the segment event");
    printf("EVENT_SEGMENT_PASS mode=lost submissions=%u waits=%u; simulated Vulkan only\n",
           rdna4_audit_submission_count(), rdna4_audit_wait_count());
}

static void timeout(void) {
    id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
    if (!device) fail(@"device");
    id<MTLCommandQueue> q1 = [device newCommandQueue], q2 = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!q1 || !q2 || !buffer || !event) fail(@"timeout resources");

    id<MTLCommandBuffer> blocked = [q1 commandBuffer];
    [(id)blocked encodeWaitForEvent:event value:1 timeout:50];
    RDNA4EventObservation *blockedObservation = observe(blocked);
    [blocked commit];
    await(blocked, blockedObservation, MTLCommandBufferStatusError, "event wait timeout");

    // A wait that failed before accepting any GPU work must not poison an
    // unrelated logical queue.
    id<MTLCommandBuffer> independent = [q2 commandBuffer];
    fill(independent, buffer, 11);
    RDNA4EventObservation *independentObservation = observe(independent);
    [independent commit];
    await(independent, independentObservation, MTLCommandBufferStatusCompleted,
          "independent queue after pre-submit timeout");
    printf("EVENT_SEGMENT_PASS mode=timeout submissions=%u waits=%u; simulated Vulkan only\n",
           rdna4_audit_submission_count(), rdna4_audit_wait_count());
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: test-command-events BUNDLE success|failure|lost|timeout\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        const char *mode = argv[2];
        if (strcmp(mode, "success") && strcmp(mode, "failure") && strcmp(mode, "lost") &&
            strcmp(mode, "timeout")) return 2;
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4_AUDIT_SUBMIT", !strcmp(mode, "lost") ? "lost" : "complete", 1);
        unsetenv("RDNA4M_TEST_TIMEOUT_MS");
        NSError *loadError = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&loadError])
            fail([NSString stringWithFormat:@"bundle: %@", loadError]);
        if (!strcmp(mode, "lost")) { lost(); return 0; }
        if (!strcmp(mode, "failure")) { failure(); return 0; }
        if (!strcmp(mode, "timeout")) { timeout(); return 0; }
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        if (!device) fail(@"device");
        success(device);
        return 0;
    }
}
