#ifdef RDNA4_NATIVE_ROUTE_GLUE
#include "GarnetDevice.m"

static char routeTrace[128];
static unsigned routeLength, routeOps, routeBad;
static const char *routeMode;
static VkDeviceMemory routeSrc, routeDst;
static uint64_t routeSrcOffset, routeDstOffset;
static PFN_vkQueueSubmit routeSubmit;
static dispatch_semaphore_t routeEntered, routeRelease;

static void routeNote(char c) {
    int r = pthread_mutex_trylock(&RDNA4R.qlock);
    if (r != EBUSY) { routeBad++; if (!r) pthread_mutex_unlock(&RDNA4R.qlock); }
    if (routeLength + 1 < sizeof routeTrace) {
        routeTrace[routeLength++] = c; routeTrace[routeLength] = 0;
    } else routeBad++;
}
static VkResult routeIdle(VkQueue q) {
    routeNote('I'); if (q != RDNA4R.q) routeBad++;
    return !strcmp(routeMode, "idle-error") ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult routeVulkan(VkQueue q, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    routeNote('V'); return routeSubmit(q, count, info, fence);
}
static int routeResult(uint64_t *seq) {
    routeOps++;
    if (!strcmp(routeMode, "unsupported")) return -ENOSYS;
    if (!strcmp(routeMode, "submit-error")) return -EIO;
    if (!strcmp(routeMode, "partial") && routeOps == 2) return -ENOSYS;
    if (!strcmp(routeMode, "zero-seq")) return 0;
    *seq = routeOps;
    if (!strcmp(routeMode, "unsupported-seq")) return -ENOSYS;
    return 0;
}
static int routeFill(VkDevice dev, VkDeviceMemory dst, uint64_t off, uint64_t size, uint32_t pattern, uint64_t *seq) {
    routeNote('F');
    if (dev != RDNA4R.dev || dst != routeDst || off != routeDstOffset + 16 || size != 64 || pattern != 0x2a2a2a2a) routeBad++;
    return routeResult(seq);
}
static int routeCopy(VkDevice dev, VkDeviceMemory src, uint64_t so, VkDeviceMemory dst, uint64_t off, uint64_t size, uint64_t *seq) {
    routeNote('C');
    if (dev != RDNA4R.dev || src != routeSrc || dst != routeDst || so != routeSrcOffset + 4 || off != routeDstOffset + 8 || size != 64) routeBad++;
    return routeResult(seq);
}
static int routeWait(VkDevice dev, uint64_t seq, uint64_t timeout, bool *busy) {
    routeNote('W');
    if (!strcmp(routeMode, "concurrent") && seq == 1) {
        dispatch_semaphore_signal(routeEntered);
        if (dispatch_semaphore_wait(routeRelease, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) routeBad++;
    }
    if (dev != RDNA4R.dev || seq != routeOps || timeout != 2000000000ULL) routeBad++;
    if (!strcmp(routeMode, "wait-error")) return -EIO;
    *busy = !strcmp(routeMode, "timeout");
    return !strcmp(routeMode, "wait-timeout") ? -ETIMEDOUT : 0;
}
void rdna4_route_install(const char *mode, RDNA4Buffer *src, RDNA4Buffer *dst) {
    routeMode = mode;
    routeEntered = dispatch_semaphore_create(0); routeRelease = dispatch_semaphore_create(0);
    Ivar pm = class_getInstanceVariable([src class], "_pm");
    RDNA4Mem *sm = (RDNA4Mem *)((char *)(__bridge void *)src + ivar_getOffset(pm));
    RDNA4Mem *dm = (RDNA4Mem *)((char *)(__bridge void *)dst + ivar_getOffset(pm));
    routeSrc = sm->mem; routeSrcOffset = sm->off; routeDst = dm->mem; routeDstOffset = dm->off;
    routeSubmit = vkQueueSubmit; vkQueueSubmit = routeVulkan;
    rdna4_vkQueueWaitIdle = routeIdle;
    rdna4_native_bridge = (RDNA4NativeBridge){routeCopy, routeFill, routeWait};
    if (!strcmp(mode, "missing")) rdna4_native_bridge.wait = NULL;
    if (!strcmp(mode, "no-idle")) rdna4_vkQueueWaitIdle = NULL;
    if (!strcmp(mode, "host")) {
        Ivar iv = class_getInstanceVariable([src class], "_imported");
        *(BOOL *)((char *)(__bridge void *)src + ivar_getOffset(iv)) = YES;
    }
    if (!routeSrc || !routeDst || !routeSrcOffset || !routeDstOffset ||
        (uint64_t)routeSrc == (uint64_t)[src vkBuffer] || (uint64_t)routeDst == (uint64_t)[dst vkBuffer]) routeBad++;
}
const char *rdna4_route_trace(void) { return routeTrace; }
unsigned rdna4_route_bad(void) { return routeBad; }
void rdna4_route_offsets(uint64_t *src, uint64_t *dst) { *src = routeSrcOffset; *dst = routeDstOffset; }
bool rdna4_route_await(void) { return !dispatch_semaphore_wait(routeEntered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)); }
void rdna4_route_release(void) { dispatch_semaphore_signal(routeRelease); }

#else
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <unistd.h>

@interface NSObject (RDNA4RouteTest)
- (void)rdna4Retain:(id)object;
@end
static _Atomic unsigned released;
@interface RDNA4RouteSentinel : NSObject
@end
@implementation RDNA4RouteSentinel
- (void)dealloc { atomic_fetch_add(&released, 1); }
@end
static void check(BOOL pass, const char *scenario) {
    if (!pass) { fprintf(stderr, "NATIVE_ROUTE_FAIL %s\n", scenario); _exit(1); }
}
static void drain(id<MTLCommandQueue> queue) {
    Ivar iv = class_getInstanceVariable([queue class], "_cq");
    check(iv != NULL, "completion queue exists");
    dispatch_sync(object_getIvar(queue, iv), ^{});
}
static id<MTLCommandBuffer> vulkan(id<MTLCommandQueue> queue) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder]; [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    return cb;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        const char *mode = argv[2];
        setenv("RADV_DARWIN_MOCK", "1", 1); setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RDNA4M_ALLOW", "1", 1); setenv("RDNA4_AUDIT_SUBMIT", "complete", 1);
        unsetenv("RDNA4M_TEST_TIMEOUT_MS");
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])]; NSError *error = nil;
        check([bundle loadAndReturnError:&error], "bundle load");
        void *handle = dlopen(bundle.executablePath.UTF8String, RTLD_NOW);
        void (*install)(const char *, id, id) = dlsym(handle, "rdna4_route_install");
        const char *(*trace)(void) = dlsym(handle, "rdna4_route_trace");
        unsigned (*bad)(void) = dlsym(handle, "rdna4_route_bad");
        void (*offsets)(uint64_t *, uint64_t *) = dlsym(handle, "rdna4_route_offsets");
        bool (*awaitNative)(void) = dlsym(handle, "rdna4_route_await");
        void (*releaseNative)(void) = dlsym(handle, "rdna4_route_release");
        check(install && trace && bad && offsets && awaitNative && releaseNative, "test call table exports");
        id<MTLDevice> dev = [[objc_getClass("GarnetDevice") alloc] init];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLBuffer> pad = [dev newBufferWithLength:256 options:0];
        id<MTLBuffer> src = [dev newBufferWithLength:256 options:0];
        id<MTLBuffer> dst = [dev newBufferWithLength:256 options:0];
        __weak id<MTLBuffer> weakSrc = src, weakDst = dst;
        check(queue && pad && src && dst, "mock RADV resources");
        install(mode, src, dst);
        uint64_t so, doff; offsets(&so, &doff);
        BOOL ordering = !strcmp(mode, "order");
        if (ordering) check(vulkan(queue).status == MTLCommandBufferStatusCompleted, "prior Vulkan completion");
        __block unsigned callbacks = 0; __block BOOL completionOK = NO;
        BOOL failure = !strcmp(mode,"partial") || !strcmp(mode,"timeout") || !strcmp(mode,"wait-timeout") ||
            !strcmp(mode,"wait-error") || !strcmp(mode,"submit-error") || !strcmp(mode,"zero-seq") ||
            !strcmp(mode,"unsupported-seq") || !strcmp(mode,"idle-error");
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [queue commandBufferWithUnretainedReferences];
            [(id)cb rdna4Retain:[RDNA4RouteSentinel new]];
            id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];
            [enc fillBuffer:dst range:NSMakeRange(!strcmp(mode,"byte-fill") ? 17 : 16, 64) value:42];
            [enc copyFromBuffer:src sourceOffset:4 toBuffer:dst destinationOffset:8 size:64];
            if (!strcmp(mode, "texture")) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
                id<MTLTexture> tex = [dev newTextureWithDescriptor:td]; check(tex != nil, "texture allocation");
                [enc copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:64 sourceSize:MTLSizeMake(4,4,1)
                    toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            }
            [enc endEncoding];
            if (!strcmp(mode,"compute")) { id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder]; [c endEncoding]; }
            if (!strcmp(mode,"render")) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> tex = [dev newTextureWithDescriptor:td]; check(tex != nil, "render target allocation");
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = tex;
                id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:rp]; [r endEncoding];
            }
            check(!strcmp(trace(), ordering ? "V" : ""), "no native call before commit");
            src = nil; dst = nil;
            check(weakSrc && weakDst, "encoded buffers retained without caller ownership");
            check(atomic_load(&released) == 0, "resource retained before commit");
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                callbacks++;
                completionOK = done.status == (failure ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted) &&
                    (failure ? done.error != nil : done.error == nil) && atomic_load(&released) == 0 && weakSrc && weakDst;
            }];
            if (!strcmp(mode,"concurrent")) {
                id<MTLCommandQueue> other = [dev newCommandQueue];
                id<MTLCommandBuffer> later = [other commandBuffer];
                id<MTLComputeCommandEncoder> c = [later computeCommandEncoder]; [c endEncoding];
                dispatch_group_t commits = dispatch_group_create();
                dispatch_group_async(commits, dispatch_get_global_queue(QOS_CLASS_DEFAULT,0), ^{ [cb commit]; });
                check(awaitNative(), "native wait reached");
                dispatch_semaphore_t attempted = dispatch_semaphore_create(0);
                dispatch_group_async(commits, dispatch_get_global_queue(QOS_CLASS_DEFAULT,0), ^{
                    dispatch_semaphore_signal(attempted); [later commit];
                });
                check(!dispatch_semaphore_wait(attempted, dispatch_time(DISPATCH_TIME_NOW, 5*NSEC_PER_SEC)), "concurrent commit started");
                usleep(20000);
                check(!strcmp(trace(),"IFW"), "later Vulkan cannot submit during native wait");
                releaseNative();
                [cb waitUntilCompleted]; [later waitUntilCompleted];
                check(later.status == MTLCommandBufferStatusCompleted, "concurrent later completion");
                check(!dispatch_group_wait(commits, dispatch_time(DISPATCH_TIME_NOW, 5*NSEC_PER_SEC)), "commit calls returned");
            } else { [cb commit]; [cb waitUntilCompleted]; }
        }
        drain(queue);
        check(callbacks == 1 && completionOK, "exactly one completion with correct status/error and live resource");
        check(atomic_load(&released) == (failure ? 0u : 1u), "completion release or quarantine lifetime");
        check((weakSrc != nil) == failure && (weakDst != nil) == failure, "actual buffer completion release or quarantine");
        if (failure) {
            NSString *before = @(trace());
            id<MTLCommandQueue> other = [dev newCommandQueue];
            check(vulkan(other).status == MTLCommandBufferStatusError, "cross queue fault guard");
            id<MTLCommandBuffer> later = [other commandBuffer];
            id<MTLBuffer> b = [dev newBufferWithLength:256 options:0];
            id<MTLBlitCommandEncoder> e = [later blitCommandEncoder];
            [e fillBuffer:b range:NSMakeRange(16,64) value:42]; [e endEncoding];
            [later commit]; [later waitUntilCompleted];
            check(later.status == MTLCommandBufferStatusError, "native route also blocked by fault guard");
            check([before isEqualToString:@(trace())], "no Vulkan replay or post fault submission");
        }
        if (ordering) check(vulkan(queue).status == MTLCommandBufferStatusCompleted, "later Vulkan completion");
        const char *expected = "IFWCW";
        if (ordering) expected = "VIFWCWV";
        else if (!strcmp(mode,"concurrent")) expected = "IFWCWV";
        else if (!strcmp(mode,"unsupported")) expected = "IFV";
        else if (!strcmp(mode,"partial")) expected = "IFWC";
        else if (!strcmp(mode,"idle-error")) expected = "I";
        else if (!strcmp(mode,"submit-error") || !strcmp(mode,"zero-seq") || !strcmp(mode,"unsupported-seq")) expected = "IF";
        else if (failure) expected = "IFW";
        else if (strcmp(mode,"complete")) expected = "V";
        check(!strcmp(trace(), expected) && !bad(), "exact call order, qlock ownership, ABI memory offsets and pattern");
        printf("NATIVE_ROUTE_PASS mode=%s trace=%s pooled_offsets=%llu,%llu callbacks=%u released=%u; production glue, simulated transport, no physical GPU\n",
            mode, trace(), (unsigned long long)so, (unsigned long long)doff, callbacks, atomic_load(&released));
        _exit(0);
    }
}
#endif
