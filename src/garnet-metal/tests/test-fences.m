// Exercise the production GarnetDevice fence objects and command-buffer
// scheduler with the VM's simulated RADV submission path. This checks object
// identity, encoded-position gating, generation capture and failure
// propagation; it does not claim physical-GPU data visibility.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <unistd.h>

@interface RDNA4FenceObservation : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end
@implementation RDNA4FenceObservation
@end

@interface NSObject (RDNA4FenceTest)
- (void)rdna4Fail:(NSString *)reason;
@end

extern unsigned rdna4_audit_submission_count(void);

static void fail(NSString *message) {
    fprintf(stderr, "FENCE_SEGMENT_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static id newDevice(void) {
    id device = [[objc_getClass("GarnetDevice") alloc] init];
    if (!device) fail(@"GarnetDevice creation");
    return device;
}

static RDNA4FenceObservation *observe(id<MTLCommandBuffer> command) {
    RDNA4FenceObservation *observation = [RDNA4FenceObservation new];
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

static void await(id<MTLCommandBuffer> command, RDNA4FenceObservation *observation,
                  MTLCommandBufferStatus want, const char *name) {
    if (dispatch_semaphore_wait(observation->done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        fail([NSString stringWithFormat:@"%s: no completion callback", name]);
    [command waitUntilCompleted];
    usleep(20000);
    if (atomic_load(&observation->callbacks) != 1)
        fail([NSString stringWithFormat:@"%s: %u callbacks", name, atomic_load(&observation->callbacks)]);
    if (command.status != want)
        fail([NSString stringWithFormat:@"%s: status %ld expected %ld", name,
              (long)command.status, (long)want]);
    if (want == MTLCommandBufferStatusCompleted && observation->error)
        fail([NSString stringWithFormat:@"%s: error %@", name, observation->error]);
    if (want == MTLCommandBufferStatusError && !observation->error)
        fail([NSString stringWithFormat:@"%s: error status without NSError", name]);
}

static void requirePending(id<MTLCommandBuffer> command, RDNA4FenceObservation *observation,
                           const char *name) {
    usleep(150000);
    if (atomic_load(&observation->callbacks) || command.status == MTLCommandBufferStatusCompleted)
        fail([NSString stringWithFormat:@"%s: completed before its dependency", name]);
}

static void fill(id<MTLCommandBuffer> command, id<MTLBuffer> buffer, uint8_t value) {
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    if (!encoder) fail(@"blit encoder");
    [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:value];
    [encoder endEncoding];
}

static void identity(id<MTLDevice> first, id<MTLDevice> second) {
    id<MTLCommandQueue> queue = [first newCommandQueue];
    id<MTLFence> fence = [first newFence];
    id<MTLFence> foreign = [second newFence];
    id<MTLBuffer> buffer = [first newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!queue || !fence || !foreign || !buffer) fail(@"identity resources");
    if (fence.device != first || foreign.device != second) fail(@"newFence device identity");
    fence.label = @"fence-object-boundary";
    if (![fence.label isEqualToString:@"fence-object-boundary"]) fail(@"fence label");
    if ([fence isEqual:foreign]) fail(@"distinct devices returned the same fence");

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:7];
    [encoder updateFence:foreign];
    [encoder endEncoding];
    RDNA4FenceObservation *observation = observe(command);
    [command commit];
    await(command, observation, MTLCommandBufferStatusError, "foreign-device fence");
    printf("FENCE_OBJECT_PASS identity label foreign_device_rejected\n");
}

static void producerConsumer(id<MTLDevice> device, id<MTLCommandQueue> q1,
                             id<MTLCommandQueue> q2) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    id<MTLFence> fence = [device newFence];
    if (!buffer || !gate || !fence) fail(@"cross-command-buffer resources");

    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    fill(producer, buffer, 1);
    id<MTLBlitCommandEncoder> produce = [producer blitCommandEncoder];
    [produce updateFence:fence];
    [produce endEncoding];
    [producer encodeWaitForEvent:gate value:1];
    RDNA4FenceObservation *producerObservation = observe(producer);
    [producer commit];

    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    id<MTLBlitCommandEncoder> consume = [consumer blitCommandEncoder];
    [consume waitForFence:fence];
    [consume fillBuffer:buffer range:NSMakeRange(0, 256) value:2];
    [consume endEncoding];
    RDNA4FenceObservation *consumerObservation = observe(consumer);
    [consumer commit];

    await(consumer, consumerObservation, MTLCommandBufferStatusCompleted,
          "consumer before producer terminal work");
    requirePending(producer, producerObservation, "producer terminal gate");
    gate.signaledValue = 1;
    await(producer, producerObservation, MTLCommandBufferStatusCompleted, "producer terminal");
    printf("FENCE_SEGMENT_PASS producer_first update_before_wait terminal_independent\n");
}

static void sameCommandBuffer(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!buffer || !fence || !command) fail(@"same-command-buffer resources");
    id<MTLBlitCommandEncoder> producer = [command blitCommandEncoder];
    [producer fillBuffer:buffer range:NSMakeRange(0, 256) value:3];
    [producer updateFence:fence];
    [producer endEncoding];
    id<MTLBlitCommandEncoder> consumer = [command blitCommandEncoder];
    [consumer waitForFence:fence];
    [consumer fillBuffer:buffer range:NSMakeRange(0, 128) value:4];
    [consumer endEncoding];
    RDNA4FenceObservation *observation = observe(command);
    buffer = nil;
    fence = nil;
    [command commit];
    await(command, observation, MTLCommandBufferStatusCompleted, "same command buffer with released objects");
    printf("FENCE_SEGMENT_PASS same_command_buffer released_buffer_and_fence\n");
}

static void freshWaitNoUpdate(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!buffer || !fence || !command) fail(@"fresh-wait resources");
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder waitForFence:fence];
    [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:0x5c];
    [encoder endEncoding];
    RDNA4FenceObservation *observation = observe(command);
    unsigned submissions = rdna4_audit_submission_count();
    [command commit];
    await(command, observation, MTLCommandBufferStatusCompleted,
          "fresh fence wait without update");
    unsigned actual = rdna4_audit_submission_count() - submissions;
    if (!actual) fail(@"fresh fence wait did not advance the audit submission count");
    printf("FENCE_FRESH_WAIT_PASS no_prior_update completed callbacks=1 submissions=%u\n",
           actual);
}

static void capturedGeneration(id<MTLDevice> device, id<MTLCommandQueue> q1,
                               id<MTLCommandQueue> q2, id<MTLCommandQueue> q3) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    id<MTLFence> fence = [device newFence];
    if (!buffer || !gate || !fence) fail(@"generation resources");

    id<MTLCommandBuffer> first = [q1 commandBuffer];
    [first encodeWaitForEvent:gate value:1];
    fill(first, buffer, 1);
    id<MTLBlitCommandEncoder> firstUpdate = [first blitCommandEncoder];
    [firstUpdate updateFence:fence];
    [firstUpdate endEncoding];
    RDNA4FenceObservation *firstObservation = observe(first);
    [first commit];

    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    id<MTLBlitCommandEncoder> wait = [consumer blitCommandEncoder];
    [wait waitForFence:fence];
    [wait fillBuffer:buffer range:NSMakeRange(0, 256) value:2];
    [wait endEncoding];
    RDNA4FenceObservation *consumerObservation = observe(consumer);
    [consumer commit];

    id<MTLCommandBuffer> second = [q3 commandBuffer];
    fill(second, buffer, 3);
    id<MTLBlitCommandEncoder> secondUpdate = [second blitCommandEncoder];
    [secondUpdate updateFence:fence];
    [secondUpdate endEncoding];
    RDNA4FenceObservation *secondObservation = observe(second);
    [second commit];

    await(second, secondObservation, MTLCommandBufferStatusCompleted,
          "later fence update");
    requirePending(consumer, consumerObservation, "wait captured generation one");

    gate.signaledValue = 1;
    await(first, firstObservation, MTLCommandBufferStatusCompleted,
          "first fence update");
    await(consumer, consumerObservation, MTLCommandBufferStatusCompleted,
          "wait after captured generation");
    printf("FENCE_GENERATION_PASS later_update_does_not_retarget\n");
}

static void timeoutThenSuccess(id<MTLDevice> device, id<MTLCommandQueue> queue,
                               id<MTLCommandQueue> consumerQueue) {
    enum { bytes = 384, step = 128 };
    id<MTLBuffer> buffer =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLFence> first = [device newFence];
    id<MTLFence> second = [device newFence];
    id<MTLFence> third = [device newFence];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!buffer || !first || !second || !third || !command)
        fail(@"timeout resources");

    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder fillBuffer:buffer range:NSMakeRange(0, step) value:0x11];
    [encoder updateFence:first];
    [encoder endEncoding];

    encoder = [command blitCommandEncoder];
    [encoder waitForFence:first];
    [encoder fillBuffer:buffer range:NSMakeRange(step, step) value:0x22];
    [encoder updateFence:second];
    [encoder endEncoding];

    encoder = [command blitCommandEncoder];
    [encoder waitForFence:second];
    [encoder fillBuffer:buffer range:NSMakeRange(2 * step, step) value:0x33];
    [encoder updateFence:third];
    [encoder endEncoding];

    id<MTLCommandBuffer> consumer = [consumerQueue commandBuffer];
    id<MTLBlitCommandEncoder> waiting = [consumer blitCommandEncoder];
    [waiting waitForFence:third];
    [waiting endEncoding];

    unsigned submissions = rdna4_audit_submission_count();
    RDNA4FenceObservation *observation = observe(command);
    RDNA4FenceObservation *consumerObservation = observe(consumer);
    [command commit];
    [consumer commit];
    await(command, observation, MTLCommandBufferStatusCompleted,
          "first fence timeout then success");
    await(consumer, consumerObservation, MTLCommandBufferStatusCompleted,
          "third fence consumer");
    unsigned actual = rdna4_audit_submission_count() - submissions;
    printf("FENCE_TIMEOUT_SUBMISSIONS actual=%u\n", actual);
    usleep(100000);
    if (atomic_load(&observation->callbacks) != 1)
        fail(@"fence timeout delivered completion more than once");
    if (atomic_load(&consumerObservation->callbacks) != 1)
        fail(@"third fence consumer completion delivered more than once");
    printf("FENCE_TIMEOUT_PASS first_timeout_then_success third_fence_consumer "
           "callbacks=2\n");
}

static void failure(id<MTLDevice> device, id<MTLCommandQueue> q1, id<MTLCommandQueue> q2) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    if (!buffer || !fence) fail(@"failure resources");

    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    fill(producer, buffer, 5);
    id<MTLBlitCommandEncoder> update = [producer blitCommandEncoder];
    [update updateFence:fence];
    [update endEncoding];
    [(id)producer rdna4Fail:@"injected fence producer failure"];
    RDNA4FenceObservation *producerObservation = observe(producer);

    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    id<MTLBlitCommandEncoder> wait = [consumer blitCommandEncoder];
    [wait waitForFence:fence];
    [wait fillBuffer:buffer range:NSMakeRange(0, 256) value:6];
    [wait endEncoding];
    RDNA4FenceObservation *consumerObservation = observe(consumer);

    [producer commit];
    [consumer commit];
    await(producer, producerObservation, MTLCommandBufferStatusError, "failed producer");
    await(consumer, consumerObservation, MTLCommandBufferStatusError, "failed fence consumer");
    printf("FENCE_FAILURE_PASS encode_failure_propagated\n");
}

static void lost(id<MTLDevice> device, id<MTLCommandQueue> q1, id<MTLCommandQueue> q2) {
    id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [device newFence];
    if (!buffer || !fence) fail(@"lost resources");

    id<MTLCommandBuffer> producer = [q1 commandBuffer];
    fill(producer, buffer, 8);
    id<MTLBlitCommandEncoder> update = [producer blitCommandEncoder];
    [update updateFence:fence];
    [update endEncoding];
    RDNA4FenceObservation *producerObservation = observe(producer);

    id<MTLCommandBuffer> consumer = [q2 commandBuffer];
    id<MTLBlitCommandEncoder> wait = [consumer blitCommandEncoder];
    [wait waitForFence:fence];
    [wait fillBuffer:buffer range:NSMakeRange(0, 256) value:9];
    [wait endEncoding];
    RDNA4FenceObservation *consumerObservation = observe(consumer);

    [producer commit];
    [consumer commit];
    await(producer, producerObservation, MTLCommandBufferStatusError, "lost producer fence");
    await(consumer, consumerObservation, MTLCommandBufferStatusError, "lost consumer fence");
    printf("FENCE_LOST_PASS producer_failure_propagated\n");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: test-fences BUNDLE success|failure|lost|fence-timeout\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        if (strcmp(argv[2], "success") && strcmp(argv[2], "failure") &&
            strcmp(argv[2], "lost") && strcmp(argv[2], "fence-timeout"))
            return 2;
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4_AUDIT_SUBMIT",
               !strcmp(argv[2], "lost") ? "lost" :
               !strcmp(argv[2], "fence-timeout") ? "early-timeout" : "complete", 1);
        if (!strcmp(argv[2], "fence-timeout"))
            setenv("RDNA4M_TEST_TIMEOUT_MS", "1", 1);
        else
            unsetenv("RDNA4M_TEST_TIMEOUT_MS");
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error])
            fail([NSString stringWithFormat:@"bundle: %@", error]);

        id<MTLDevice> first = newDevice();
        id<MTLDevice> second = newDevice();
        id<MTLCommandQueue> q1 = [first newCommandQueue];
        id<MTLCommandQueue> q2 = [first newCommandQueue];
        id<MTLCommandQueue> q3 = [first newCommandQueue];
        if (!q1 || !q2 || !q3) fail(@"command queues");

        if (!strcmp(argv[2], "fence-timeout")) {
            timeoutThenSuccess(first, q1, q2);
            return 0;
        }
        if (!strcmp(argv[2], "failure")) {
            failure(first, q1, q2);
            return 0;
        }
        if (!strcmp(argv[2], "lost")) {
            lost(first, q1, q2);
            return 0;
        }
        identity(first, second);
        sameCommandBuffer(first, q1);
        freshWaitNoUpdate(first, q1);
        producerConsumer(first, q1, q2);
        capturedGeneration(first, q1, q2, q3);
        return 0;
    }
}
