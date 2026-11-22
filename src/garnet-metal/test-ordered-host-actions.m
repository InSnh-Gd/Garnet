// Run in the macOS VM with RADV_DARWIN_MOCK. The command scheduler, event
// ordering and completion callbacks are real; Vulkan submission and fence
// waits are simulated.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_wait_count(void);

typedef BOOL (^RDNA4OrderedHostAction)(NSError **);

@protocol RDNA4OrderedHostActionEncoding <NSObject>
- (BOOL)rdna4EncodeOrderedHostAction:(RDNA4OrderedHostAction)action
                               error:(NSError **)error;
@end

@interface RDNA4HostActionObservation : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end

@implementation RDNA4HostActionObservation
@end

@interface RDNA4HostActionProbe : NSObject {
@public
    _Atomic unsigned calls;
    unsigned submissionsAtRun;
    unsigned waitsAtRun;
}
@end

@implementation RDNA4HostActionProbe
@end

static void fail(NSString *message) {
    fprintf(stderr, "HOST_ACTION_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static BOOL encodeOrdered(id<MTLCommandBuffer> command,
                          RDNA4OrderedHostAction action, NSError **error) {
    return [(id<RDNA4OrderedHostActionEncoding>)command
        rdna4EncodeOrderedHostAction:action error:error];
}

static void fill(id<MTLCommandBuffer> command, id<MTLBuffer> buffer,
                 uint32_t value) {
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    if (!encoder) fail(@"blit encoder");
    [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:value];
    [encoder endEncoding];
}

static RDNA4HostActionObservation *observe(id<MTLCommandBuffer> command) {
    RDNA4HostActionObservation *observation = [RDNA4HostActionObservation new];
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

static void await(id<MTLCommandBuffer> command,
                  RDNA4HostActionObservation *observation,
                  MTLCommandBufferStatus want, const char *name) {
    if (dispatch_semaphore_wait(observation->done,
                                dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
        fail([NSString stringWithFormat:@"%s: no completion callback", name]);
    [command waitUntilCompleted];
    usleep(20000);
    unsigned callbacks = atomic_load(&observation->callbacks);
    if (callbacks != 1)
        fail([NSString stringWithFormat:@"%s: %u completion callbacks", name, callbacks]);
    if (command.status != want)
        fail([NSString stringWithFormat:@"%s: status %ld, expected %ld", name,
              (long)command.status, (long)want]);
    if (want == MTLCommandBufferStatusCompleted && observation->error)
        fail([NSString stringWithFormat:@"%s: completion error %@", name,
              observation->error]);
    if (want == MTLCommandBufferStatusError && !observation->error)
        fail([NSString stringWithFormat:@"%s: error status without NSError", name]);
}

static void ordering(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256
                                               options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    if (!queue || !buffer || !gate) fail(@"ordering resources");

    unsigned submissionsBefore = rdna4_audit_submission_count();
    unsigned waitsBefore = rdna4_audit_wait_count();
    RDNA4HostActionProbe *probe = [RDNA4HostActionProbe new];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    NSError *encodingError = nil;
    [command encodeWaitForEvent:gate value:1];
    __weak NSObject *weakPayload = nil;
    @autoreleasepool {
        NSObject *payload = [NSObject new];
        weakPayload = payload;
        BOOL encoded = encodeOrdered(command, ^BOOL(NSError **error) {
            (void)error;
            atomic_fetch_add(&probe->calls, 1);
            probe->submissionsAtRun = rdna4_audit_submission_count();
            probe->waitsAtRun = rdna4_audit_wait_count();
            if (gate.signaledValue != 1)
                fail(@"ordered host action ran before its preceding event wait");
            if (!payload)
                fail(@"ordered host action payload was released before execution");
            return YES;
        }, &encodingError);
        if (!encoded || encodingError)
            fail([NSString stringWithFormat:@"order encoding: %@", encodingError]);
    }
    fill(command, buffer, 0x31);
    if (!weakPayload) fail(@"ordered host action did not retain its block capture");

    RDNA4HostActionObservation *observation = observe(command);
    [command commit];
    usleep(100000);
    if (atomic_load(&probe->calls) != 0)
        fail(@"ordered host action ran while its event wait was blocked");
    if (rdna4_audit_submission_count() != submissionsBefore)
        fail(@"blocked event-wait segment was submitted early");

    gate.signaledValue = 1;
    await(command, observation, MTLCommandBufferStatusCompleted,
          "wait then ordered host action");
    usleep(30000);
    if (atomic_load(&probe->calls) != 1)
        fail(@"ordered host action did not run exactly once after its wait");
    if (probe->waitsAtRun <= waitsBefore ||
        probe->submissionsAtRun != submissionsBefore + 1)
        fail(@"ordered host action ran before the preceding segment fence completed");
    if (weakPayload)
        fail(@"ordered host action block capture survived terminal success");
    if (rdna4_audit_submission_count() != submissionsBefore + 2)
        fail(@"successor segment did not run after the ordered host action");
    printf("HOST_ACTION_CASE_PASS wait_then_action once=%u submissions=%u waits=%u\n",
           atomic_load(&probe->calls), rdna4_audit_submission_count(),
           rdna4_audit_wait_count());
}

static void successBarrier(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256
                                               options:MTLResourceStorageModeShared];
    if (!queue || !buffer) fail(@"success barrier resources");

    dispatch_semaphore_t entered = dispatch_semaphore_create(0);
    dispatch_semaphore_t release = dispatch_semaphore_create(0);
    unsigned submissionsBefore = rdna4_audit_submission_count();
    unsigned waitsBefore = rdna4_audit_wait_count();
    RDNA4HostActionProbe *probe = [RDNA4HostActionProbe new];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    fill(command, buffer, 0x42);
    NSError *encodingError = nil;
    BOOL encoded = encodeOrdered(command, ^BOOL(NSError **error) {
        atomic_fetch_add(&probe->calls, 1);
        probe->submissionsAtRun = rdna4_audit_submission_count();
        probe->waitsAtRun = rdna4_audit_wait_count();
        dispatch_semaphore_signal(entered);
        if (dispatch_semaphore_wait(release,
                                    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            if (error)
                *error = [NSError errorWithDomain:@"RDNA4OrderedHostActionTest"
                                             code:81 userInfo:nil];
            return NO;
        }
        return YES;
    }, &encodingError);
    if (!encoded || encodingError)
        fail([NSString stringWithFormat:@"barrier encoding: %@", encodingError]);
    fill(command, buffer, 0x43);

    RDNA4HostActionObservation *observation = observe(command);
    [command commit];
    if (dispatch_semaphore_wait(entered,
                                dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        fail(@"ordered host action never reached its success barrier");
    if (atomic_load(&probe->calls) != 1)
        fail(@"ordered host action entered more than once");
    if (probe->waitsAtRun <= waitsBefore ||
        probe->submissionsAtRun != submissionsBefore + 1)
        fail(@"ordered host action passed its predecessor segment");
    usleep(50000);
    if (rdna4_audit_submission_count() != submissionsBefore + 1)
        fail(@"successor segment ran before the ordered host action accepted");

    dispatch_semaphore_signal(release);
    await(command, observation, MTLCommandBufferStatusCompleted,
          "successful host action barrier");
    usleep(30000);
    if (atomic_load(&probe->calls) != 1)
        fail(@"successful ordered host action was replayed");
    if (rdna4_audit_submission_count() != submissionsBefore + 2)
        fail(@"successor segment did not advance after action success");
    printf("HOST_ACTION_CASE_PASS success_barrier once=%u submissions=%u waits=%u\n",
           atomic_load(&probe->calls), rdna4_audit_submission_count(),
           rdna4_audit_wait_count());
}

static void failure(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:256
                                               options:MTLResourceStorageModeShared];
    if (!queue || !buffer) fail(@"failure resources");
    unsigned submissionsBefore = rdna4_audit_submission_count();
    RDNA4HostActionProbe *first = [RDNA4HostActionProbe new];
    RDNA4HostActionProbe *second = [RDNA4HostActionProbe new];
    __weak NSObject *weakPayload = nil;
    NSError *intended = [NSError errorWithDomain:@"RDNA4OrderedHostActionTest"
                                            code:82 userInfo:nil];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    fill(command, buffer, 0x51);
    NSError *encodingError = nil;
    BOOL encoded = encodeOrdered(command, ^BOOL(NSError **error) {
        atomic_fetch_add(&first->calls, 1);
        if (error) *error = intended;
        return NO;
    }, &encodingError);
    if (!encoded || encodingError)
        fail([NSString stringWithFormat:@"failure encoding: %@", encodingError]);

    @autoreleasepool {
        NSObject *payload = [NSObject new];
        weakPayload = payload;
        encodingError = nil;
        encoded = encodeOrdered(command, ^BOOL(NSError **error) {
            (void)error;
            (void)payload;
            atomic_fetch_add(&second->calls, 1);
            return YES;
        }, &encodingError);
        if (!encoded || encodingError)
            fail([NSString stringWithFormat:@"cleanup encoding: %@", encodingError]);
    }
    if (!weakPayload) fail(@"unexecuted action did not retain its capture");
    fill(command, buffer, 0x52);

    RDNA4HostActionObservation *observation = observe(command);
    [command commit];
    await(command, observation, MTLCommandBufferStatusError,
          "failed ordered host action");
    usleep(30000);
    if (atomic_load(&first->calls) != 1)
        fail(@"failed ordered host action was replayed");
    if (atomic_load(&second->calls) != 0)
        fail(@"successor host action ran after predecessor rejection");
    if (observation->error.code != 82 ||
        ![observation->error.domain isEqualToString:@"RDNA4OrderedHostActionTest"])
        fail([NSString stringWithFormat:@"terminal error was not the action error: %@",
              observation->error]);
    if (weakPayload)
        fail(@"unexecuted action block survived terminal failure");
    if (rdna4_audit_submission_count() != submissionsBefore + 1)
        fail(@"a segment submitted after terminal host-action failure");
    printf("HOST_ACTION_CASE_PASS terminal_failure once=%u successor=%u submissions=%u\n",
           atomic_load(&first->calls), atomic_load(&second->calls),
           rdna4_audit_submission_count());
}

static void lifetime(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) fail(@"lifetime queue");
    RDNA4HostActionProbe *probe = [RDNA4HostActionProbe new];
    __weak NSObject *weakPayload = nil;
    @autoreleasepool {
        NSObject *payload = [NSObject new];
        weakPayload = payload;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        NSError *encodingError = nil;
        BOOL encoded = encodeOrdered(command, ^BOOL(NSError **error) {
            (void)error;
            (void)payload;
            atomic_fetch_add(&probe->calls, 1);
            return YES;
        }, &encodingError);
        if (!encoded || encodingError)
            fail([NSString stringWithFormat:@"lifetime encoding: %@", encodingError]);
        if (!weakPayload) fail(@"unsubmitted action did not retain its capture");
        command = nil;
    }
    usleep(50000);
    if (atomic_load(&probe->calls) != 0)
        fail(@"unsubmitted ordered host action executed");
    if (weakPayload)
        fail(@"command-buffer dealloc did not release its queued action");
    printf("HOST_ACTION_CASE_PASS dealloc_cleanup calls=%u\n",
           atomic_load(&probe->calls));
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        const char *mode = argv[2];
        if (strcmp(mode, "ordering") && strcmp(mode, "success") &&
            strcmp(mode, "failure") && strcmp(mode, "lifetime")) return 2;
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4_AUDIT_SUBMIT", "complete", 1);
        NSError *loadError = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&loadError])
            fail([NSString stringWithFormat:@"bundle: %@", loadError]);
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        if (!device) fail(@"device");
        if (!strcmp(mode, "ordering")) ordering(device);
        else if (!strcmp(mode, "success")) successBarrier(device);
        else if (!strcmp(mode, "failure")) failure(device);
        else lifetime(device);
        puts("HOST_ACTION_PASS simulated Vulkan scheduler only");
        return 0;
    }
}
