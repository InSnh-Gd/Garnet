// Run in the macOS VM, linked with the test-only librdna4featureaudit.dylib.
// Metal encoding/callbacks are real; Vulkan submissions and fence waits are simulated.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_wait_count(void);
extern unsigned rdna4_audit_fence_destroys(void);
extern unsigned rdna4_audit_command_frees(void);
extern unsigned rdna4_audit_buffer_destroys(void);

@interface NSObject (RDNA4LifetimeTest)
- (void)rdna4Retain:(id)object;
@end

@interface NSObject (RDNA4PrivateCommit)
- (BOOL)commitAndWaitUntilSubmitted;
@end

static BOOL classifiedFrame(id object, SEL selector) {
    (void)object; (void)selector;
    return YES;
}

static _Atomic unsigned released, callbacks;
@interface RDNA4LifetimeSentinel : NSObject
@end
@implementation RDNA4LifetimeSentinel
- (void)dealloc { atomic_fetch_add(&released, 1); }
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-command-completion BUNDLE reject|submit-error|fence-error|complete|wait-submitted|wait-frame-submitted|surface-schedule|lost|timeout|early-timeout|early-hung|wait-error [replace|get]");
            return 0;
        }
        if ((argc != 3 && argc != 4) || (strcmp(argv[2], "reject") && strcmp(argv[2], "complete") &&
            strcmp(argv[2], "wait-submitted") &&
            strcmp(argv[2], "wait-frame-submitted") &&
            strcmp(argv[2], "surface-schedule") &&
            strcmp(argv[2], "lost") && strcmp(argv[2], "timeout") &&
            strcmp(argv[2], "early-timeout") && strcmp(argv[2], "early-hung") && strcmp(argv[2], "wait-error") &&
            strcmp(argv[2], "submit-error") && strcmp(argv[2], "fence-error"))) return 2;
        if (argc == 4 && strcmp(argv[3], "replace") && strcmp(argv[3], "get")) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        const char *mode = argv[2];
        BOOL beforeSubmit = !strcmp(mode, "fence-error");
        BOOL submitFailure = !strcmp(mode, "reject") || !strcmp(mode, "submit-error");
        BOOL frameSubmitted = !strcmp(mode, "wait-frame-submitted");
        BOOL waitSubmitted = !strcmp(mode, "wait-submitted") || frameSubmitted;
        BOOL surfaceSchedule = !strcmp(mode, "surface-schedule");
        BOOL unresolved = submitFailure || !strcmp(mode, "lost") || !strcmp(mode, "timeout") ||
                          !strcmp(mode, "early-hung") || !strcmp(mode, "wait-error");
        setenv("RDNA4_AUDIT_SUBMIT", (waitSubmitted || surfaceSchedule) ? "complete" : mode, 1);
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        unsetenv("RADV_DARWIN_HOST_BO");
        unsetenv("RADV_DARWIN_CHIP");
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        unsetenv("RDNA4M_TEST_TIMEOUT_MS");
        if (!strcmp(mode, "early-timeout") || !strcmp(mode, "early-hung")) setenv("RDNA4M_TEST_TIMEOUT_MS", "1", 1);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "FAIL bundle: %s\n", error.description.UTF8String);
            return 1;
        }
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        if (argc == 4) {
            __weak id<MTLTexture> weakTexture;
            BOOL stagingLifetime = YES;
            for (unsigned iteration = 0; iteration < (unresolved ? 2u : 1u); iteration++) {
                @autoreleasepool {
                    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared;
                    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
                    if (!texture) return 1;
                    if (!iteration) weakTexture = texture;
                    uint32_t pixels[64] = {0};
                    if (!strcmp(argv[3], "replace"))
                        [texture replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:pixels bytesPerRow:32];
                    else [texture getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
                }
                if (!iteration) stagingLifetime = rdna4_audit_buffer_destroys() == (unresolved ? 0u : 1u);
            }
            unsigned submits = rdna4_audit_submission_count(), waits = rdna4_audit_wait_count();
            unsigned destroys = rdna4_audit_fence_destroys(), frees = rdna4_audit_command_frees();
            BOOL retained = weakTexture != nil;
            BOOL ok = submits == (beforeSubmit ? 0u : 1u) && retained == unresolved && stagingLifetime &&
                waits == (beforeSubmit || submitFailure ? 0u : !strcmp(mode, "timeout") ? 30u : 1u) &&
                destroys == (unresolved || beforeSubmit ? 0u : 1u) && frees == (unresolved ? 0u : 1u);
            printf("%s mode=%s operation=%s submits=%u waits=%u texture_retained=%d staging_lifetime=%d fence_destroyed=%u command_freed=%u; simulated GPU, pixel contents untested\n",
                   ok ? "ONESHOT_PASS" : "ONESHOT_FAIL", mode, argv[3], submits, waits, retained, stagingLifetime, destroys, frees);
            _exit(ok ? 0 : 1);
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue) return 1;
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        dispatch_semaphore_t scheduled = dispatch_semaphore_create(0);
        __block MTLCommandBufferStatus status;
        __block NSError *completionError;
        __block unsigned waitAtSchedule = UINT_MAX;
        BOOL privateWaitReturned = NO;
        MTLCommandBufferStatus privateReturnStatus = MTLCommandBufferStatusNotEnqueued;
        @autoreleasepool {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            if (frameSubmitted) {
                SEL selector = sel_registerName("rdna4IsFrame");
                Method method = class_getInstanceMethod([commands class], selector);
                if (!method) return 1;
                class_replaceMethod([commands class], selector, (IMP)classifiedFrame, method_getTypeEncoding(method));
            }
            RDNA4LifetimeSentinel *sentinel = [RDNA4LifetimeSentinel new];
            [(id)commands rdna4Retain:sentinel];
            id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
            if (!commands || !encoder) return 1;
            if (surfaceSchedule) {
                id<MTLBuffer> backing = [device newBufferWithLength:320 * 64 options:MTLResourceStorageModeShared];
                MTLTextureDescriptor *sourceDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
                sourceDescriptor.storageMode = MTLStorageModeShared;
                id<MTLTexture> source = [device newTextureWithDescriptor:sourceDescriptor];
                MTLTextureDescriptor *surfaceDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
                surfaceDescriptor.storageMode = MTLStorageModeManaged;
                surfaceDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                id<MTLTexture> destination = [backing newTextureWithDescriptor:surfaceDescriptor offset:0 bytesPerRow:320];
                if (!backing || !source || !destination) return 1;
                uint32_t pixels[64 * 64];
                memset(pixels, 0x5a, sizeof pixels);
                [source replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0
                            withBytes:pixels bytesPerRow:64 * 4];
                [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(64, 64, 1) toTexture:destination destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            } else {
                id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
                if (!buffer) return 1;
                [encoder fillBuffer:buffer range:NSMakeRange(0, 256) value:42];
            }
            [encoder endEncoding];
            [commands addScheduledHandler:^(id<MTLCommandBuffer> cb) {
                (void)cb;
                waitAtSchedule = rdna4_audit_wait_count();
                dispatch_semaphore_signal(scheduled);
            }];
            [commands addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                status = cb.status;
                completionError = cb.error;
                atomic_fetch_add(&callbacks, 1);
                dispatch_semaphore_signal(completed);
            }];
            if (waitSubmitted) {
                privateWaitReturned = [(id)commands commitAndWaitUntilSubmitted];
                privateReturnStatus = commands.status;
            } else {
                [commands commit];
            }
            if (dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
                fprintf(stderr, "FAIL no completion callback\n");
                _exit(1);
            }
            [commands waitUntilCompleted];
            if (dispatch_semaphore_wait(scheduled, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
                fprintf(stderr, "FAIL no scheduled callback\n");
                _exit(1);
            }
        }
        // The callback can precede release of the completion block's strong capture.
        Ivar completionIvar = class_getInstanceVariable([queue class], "_cq");
        if (!completionIvar) return 1;
        dispatch_queue_t completionQueue = object_getIvar(queue, completionIvar);
        dispatch_sync(completionQueue, ^{});
        unsigned submits = rdna4_audit_submission_count(), waits = rdna4_audit_wait_count();
        unsigned destroys = rdna4_audit_fence_destroys(), frees = rdna4_audit_command_frees();
        unsigned releaseCount = atomic_load(&released), callbackCount = atomic_load(&callbacks);
        BOOL success = !strcmp(mode, "complete") || waitSubmitted || surfaceSchedule;
        unsigned expectedSubmits = surfaceSchedule ? 2u : (beforeSubmit ? 0u : 1u);
        unsigned expectedResources = surfaceSchedule ? 2u : 1u;
        BOOL ok = submits == expectedSubmits && callbackCount == 1 &&
            status == (success ? MTLCommandBufferStatusCompleted : MTLCommandBufferStatusError) &&
            (success ? completionError == nil : completionError != nil);
        if (waitSubmitted) {
            const char *name = getprogname();
            BOOL windowServer = name && !strcmp(name, "WindowServer");
            ok &= privateWaitReturned && (!windowServer || !frameSubmitted || privateReturnStatus == MTLCommandBufferStatusCompleted);
        }
        if (surfaceSchedule)
            ok &= waitAtSchedule == waits;
        if (surfaceSchedule) ok &= waits == 2;
        else if (beforeSubmit || submitFailure) ok &= waits == 0;
        else if (!strcmp(mode, "timeout") || !strcmp(mode, "early-hung")) ok &= waits == 30;
        else if (!strcmp(mode, "early-timeout")) ok &= waits == 2;
        else ok &= waits == 1;
        if (unresolved) ok &= !releaseCount && !destroys && !frees;
        else ok &= releaseCount == 1 && destroys == (beforeSubmit ? 0u : expectedResources) &&
            frees == expectedResources;
        printf("%s mode=%s callbacks=%u status=%lu error=%ld submits=%u waits=%u sentinel_released=%u fence_destroyed=%u command_freed=%u private_return=%d private_status=%lu wait_at_schedule=%u; simulated GPU\n",
               ok ? "COMPLETION_PASS" : "COMPLETION_FAIL", mode, callbackCount,
               (unsigned long)status, (long)completionError.code, submits, waits, releaseCount, destroys, frees,
               privateWaitReturned, (unsigned long)privateReturnStatus, waitAtSchedule);
        if (unresolved) {
            id<MTLCommandQueue> otherQueue = [device newCommandQueue];
            dispatch_semaphore_t rejected = dispatch_semaphore_create(0);
            __block BOOL rejectedWithError = NO;
            @autoreleasepool {
                id<MTLCommandBuffer> later = [otherQueue commandBuffer];
                [(id)later rdna4Retain:[RDNA4LifetimeSentinel new]];
                [later addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                    rejectedWithError = cb.status == MTLCommandBufferStatusError && cb.error != nil;
                    atomic_fetch_add(&callbacks, 1);
                    dispatch_semaphore_signal(rejected);
                }];
                [later commit];
                if (dispatch_semaphore_wait(rejected, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) _exit(1);
                [later waitUntilCompleted];
            }
            dispatch_sync((dispatch_queue_t)object_getIvar(otherQueue, completionIvar), ^{});
            BOOL blocked = rejectedWithError && atomic_load(&callbacks) == 2 &&
                rdna4_audit_submission_count() == submits && rdna4_audit_wait_count() == waits &&
                atomic_load(&released) == releaseCount + 1 &&
                rdna4_audit_command_frees() == frees + 1 && rdna4_audit_fence_destroys() == destroys;
            printf("%s mode=%s second_queue_submits=%u\n", blocked ? "FAULT_GUARD_PASS" : "FAULT_GUARD_FAIL",
                   mode, rdna4_audit_submission_count() - submits);
            ok &= blocked;
        }
        // The generic device lacks accelerator teardown state.
        _exit(ok ? 0 : 1);
    }
}
