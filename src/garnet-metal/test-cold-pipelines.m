// Exercise first-use pipeline compilation and its first GPU result, with no
// cached translations. VM execution uses a test-only MoltenVK bundle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void rdna4_cold_configure(const char *, int, int);
extern unsigned rdna4_cold_call_count(void);
extern char **environ;
static pid_t daemonPid;

static dispatch_semaphore_t concurrentGo[2];
static id concurrentDevice, concurrentFunction[2], concurrentPipeline[2];
static NSError *concurrentError[2];
static void *concurrentRequest(void *arg) {
    const int i = (int)(intptr_t)arg;
    dispatch_semaphore_wait(concurrentGo[i], DISPATCH_TIME_FOREVER);
    @autoreleasepool {
        NSError *error = nil;
        id pipeline = [concurrentDevice newComputePipelineStateWithFunction:concurrentFunction[i] error:&error];
        concurrentPipeline[i] = pipeline;
        concurrentError[i] = error;
    }
    return NULL;
}

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void stopDaemon(void) {
    if (daemonPid > 0) {
        kill(daemonPid, SIGTERM);
        while (waitpid(daemonPid, NULL, 0) < 0 && errno == EINTR) { }
        daemonPid = 0;
    }
}

@interface NSObject (RDNA4ColdPrivate)
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor completionHandler:(void (^)(id, NSError *))handler;
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "COLD_PIPELINE_FAIL %s\n", message.UTF8String);
    stopDaemon();
    _exit(1);
}

static void finish(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail([NSString stringWithFormat:@"command status=%lu error=%@", (unsigned long)commands.status, commands.error]);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: sudo test-cold-pipelines TEST_BUNDLE REAL_TRANSLATOR sync-compute|sync-render|async-function|async-function-reflection|async-descriptor|async-descriptor-reflection|async-render|async-render-reflection|refuse-compute|refuse-render|timeout-compute|timeout-render|concurrent-compute\nChecks cold first-use compilation, timeout retry and concurrent first-use results, with actual compute/pixels. Test transport only.");
            return 0;
        }
        if ((argc != 4 && argc != 5) || geteuid() != 0) return 2;
        NSString *mode = @(argv[3]);
        NSArray *modes = @[@"sync-compute", @"sync-render", @"async-function", @"async-function-reflection",
            @"async-descriptor", @"async-descriptor-reflection", @"async-render", @"async-render-reflection",
            @"refuse-compute", @"refuse-render", @"daemon-compute", @"daemon-render", @"timeout-compute", @"timeout-render",
            @"concurrent-compute",
            @"refuse-async-function", @"refuse-async-function-reflection", @"refuse-async-descriptor",
            @"refuse-async-descriptor-reflection", @"refuse-async-render", @"refuse-async-render-reflection"];
        if (![modes containsObject:mode]) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        alarm(90);
        BOOL asynchronous = [mode containsString:@"async-"], refuse = [mode hasPrefix:@"refuse-"];
        BOOL timeout = [mode hasPrefix:@"timeout-"];
        BOOL concurrent = [mode isEqualToString:@"concurrent-compute"];
        BOOL daemon = [mode hasPrefix:@"daemon-"];
        if ((argc == 5) != daemon) return 2;
        BOOL render = [mode containsString:@"render"], reflection = [mode hasSuffix:@"-reflection"];
        rdna4_cold_configure(argv[2], timeout ? 4 : refuse ? 0 : 1, refuse);
        char scratch[PATH_MAX];
        const char *scratchRoot = getenv("TMPDIR");
        if (!scratchRoot || !scratchRoot[0]) scratchRoot = "/private/tmp";
        int length = snprintf(scratch, sizeof scratch, "%s/rdna4-cold.XXXXXX", scratchRoot);
        if (length < 0 || (size_t)length >= sizeof scratch || !mkdtemp(scratch)) fail(@"mkdtemp");
        setenv("GARNET_ALLOW", "1", 1);
        setenv("GARNET_TEST_INPROC", "1", 1);
        setenv("GARNET_TEST_USERCACHE", scratch, 1);
        setenv("GARNET_TEST_SIDE_DIR", scratch, 1);
        setenv("GARNET_TEST_HIDE_BUNDLE_SPV", "1", 1);
        // Reproduce the admitted application's former fallback policy.
        if (!refuse && !daemon && !timeout && !concurrent) setenv("GARNET_TEST_FALLBACK_AS_WS", "1", 1);
        else if (concurrent) { unsetenv("GARNET_TEST_FALLBACK_AS_WS"); unsetenv("GARNET_FORCE_FALLBACK"); unsetenv("RDNA4M_TEST_FALLBACK_AS_WS"); unsetenv("RDNA4M_FORCE_FALLBACK"); }
        if (daemon) {
            char *args[] = { (char *)argv[4], "--source", "/private/tmp/garnet-xlate", "--output", scratch,
                "--library", (char *)argv[2], NULL };
            if (posix_spawn(&daemonPid, argv[4], NULL, NULL, args, environ)) fail(@"spawn translator daemon");
            setprogname("WindowServer");
        }
        setenv("MVK_CONFIG_LOG_LEVEL", "1", 1);
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
        NSError *error = nil;
        if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        if (!device) fail(@"device unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void cold_write(device uint *p [[buffer(0)]], uint i [[thread_position_in_grid]]) { p[i] = i * 13u + 7u; }\n"
            "kernel void cold_write_second(device uint *p [[buffer(0)]], uint i [[thread_position_in_grid]]) { p[i] = i * 17u + 11u; }\n"
            "vertex float4 cold_vertex(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[i],0,1); }\n"
            "fragment float4 cold_fragment() { return float4(0.0,1.0,0.0,1.0); }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) fail(error.description);
        id<MTLFunction> function = [library newFunctionWithName:@"cold_write"];
        MTLComputePipelineDescriptor *compute = [MTLComputePipelineDescriptor new];
        compute.computeFunction = function;
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"cold_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"cold_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        __block id pipeline = nil;
        __block NSError *resultError = nil;
        __block unsigned callbacks = 0;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        pthread_t caller = pthread_self();
        void (^completed)(id, id, NSError *) = ^(id value, id details, NSError *failure) {
            if (asynchronous && pthread_equal(caller, pthread_self())) fail(@"asynchronous compiler ran its callback on the calling thread");
            if (reflection && value && !details) fail(@"requested reflection missing");
            pipeline = value; resultError = failure; callbacks++;
            dispatch_semaphore_signal(done);
        };
        double started = seconds();
        if (concurrent) {
            concurrentDevice = device;
            concurrentFunction[0] = function;
            concurrentFunction[1] = [library newFunctionWithName:@"cold_write_second"];
            if (!concurrentFunction[0] || !concurrentFunction[1]) fail(@"compute function unavailable");
            // Open RADV before the race so both threads reach the translator gate together.
            if (![device newCommandQueue]) fail(@"command queue unavailable before concurrent requests");
            for (int i = 0; i < 2; i++) concurrentGo[i] = dispatch_semaphore_create(0);
            pthread_t threads[2];
            for (int i = 0; i < 2; i++)
                if (pthread_create(&threads[i], NULL, concurrentRequest, (void *)(intptr_t)i))
                    fail(@"concurrent request thread");
            dispatch_semaphore_signal(concurrentGo[0]);
            const double translatorDeadline = seconds() + 0.5;
            while (!rdna4_cold_call_count() && seconds() < translatorDeadline) usleep(1000);
            if (rdna4_cold_call_count() != 1) fail(@"first translator job did not start");
            dispatch_semaphore_signal(concurrentGo[1]);
            for (int i = 0; i < 2; i++) pthread_join(threads[i], NULL);
            for (unsigned request = 0; request < 2; request++)
                if (!concurrentPipeline[request] || concurrentError[request])
                    fail([NSString stringWithFormat:@"first pipeline request %u: %@", request, concurrentError[request]]);
            if (rdna4_cold_call_count() != 2) fail(@"expected one translation for each function");
            NSString *cacheRoot = [@(scratch) stringByAppendingPathComponent:@"org.garnet.xlate"];
            NSFileManager *fm = [NSFileManager defaultManager];
            unsigned cached = 0;
            for (NSString *key in [fm contentsOfDirectoryAtPath:cacheRoot error:NULL])
                for (NSString *file in [fm contentsOfDirectoryAtPath:[cacheRoot stringByAppendingPathComponent:key] error:NULL])
                    if ([file hasSuffix:@".spv"]) cached++;
            if (cached != 2) fail([NSString stringWithFormat:@"expected two installed cold translations, found %u", cached]);
            pipeline = concurrentPipeline[0];
        } else if (!asynchronous) {
            if (render) pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&resultError];
            else pipeline = [device newComputePipelineStateWithFunction:function error:&resultError];
        } else if (render) {
            if (reflection) [device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionBindingInfo completionHandler:completed];
            else [device newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id p, NSError *e) { completed(p, nil, e); }];
        } else if ([mode containsString:@"descriptor"]) {
            if (reflection) [device newComputePipelineStateWithDescriptor:compute options:MTLPipelineOptionBindingInfo completionHandler:completed];
            else [(id)device newComputePipelineStateWithDescriptor:compute completionHandler:^(id p, NSError *e) { completed(p, nil, e); }];
        } else {
            if (reflection) [device newComputePipelineStateWithFunction:function options:MTLPipelineOptionBindingInfo completionHandler:completed];
            else [device newComputePipelineStateWithFunction:function completionHandler:^(id p, NSError *e) { completed(p, nil, e); }];
        }
        if (asynchronous) {
            // The worker must own descriptor contents after this API returns.
            compute.computeFunction = nil;
            descriptor.vertexFunction = nil; descriptor.fragmentFunction = nil;
            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 70 * NSEC_PER_SEC))) fail(@"callback timed out");
            if (callbacks != 1) fail(@"callback count is not one");
        }
        double elapsed = seconds() - started;
        printf("COLD_COMPILATION_RESULT mode=%s seconds=%.6f\n", argv[3], elapsed);
        if (!daemon && !rdna4_cold_call_count()) fail(@"test translator was never invoked");
        if (!daemon && elapsed > 3.75) fail(@"pipeline call exceeded the bounded application wait budget");
        if (refuse) {
            if (pipeline || !resultError) fail(@"translator failure became a successful pipeline");
            if (![resultError.description containsString:@"test translator refusal"])
                fail(@"refusal did not come from the test translator");
            NSError *underlying = resultError.userInfo[NSUnderlyingErrorKey];
            if (!underlying || underlying.code != 6 || (render && resultError.code != 41))
                fail(@"translation refusal lost its underlying error or changed the render cache-miss code");
            printf("COLD_REFUSAL_PASS mode=%s error=%ld underlying=%ld translator_calls=%u\n",
                argv[3], (long)resultError.code, (long)underlying.code, rdna4_cold_call_count());
            _exit(0);
        }
        if (timeout) {
            if (pipeline || !resultError) fail(@"compilation deadline became a successful pipeline");
            if (elapsed < 2.5) fail(@"timeout did not exercise the wait budget");
            NSError *underlying = resultError.userInfo[NSUnderlyingErrorKey];
            if (!underlying || underlying.code != 9 || (render && resultError.code != 41))
                fail(@"translation timeout lost its underlying error or changed the render cache-miss code");
            puts("COLD_TIMEOUT_REJECTED retrying_the_same_running_translation=1");
            resultError = nil;
            if (render) pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&resultError];
            else pipeline = [device newComputePipelineStateWithFunction:function error:&resultError];
        }
        if (!pipeline || resultError) fail([NSString stringWithFormat:@"pipeline=%@ error=%@", pipeline, resultError]);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> output = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!queue || !output || !output.contents) fail(@"execution resources unavailable");
        memset(output.contents, 0xa5, 256);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (render) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
            td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [device newTextureWithDescriptor:td];
            if (!target) fail(@"target unavailable");
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:pipeline];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
                        toBuffer:output destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
            [blit endEncoding];
        } else {
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
            [encoder endEncoding];
        }
        finish(commands);
        const uint32_t *words = output.contents;
        for (unsigned i = 0; i < 64; i++) {
            uint32_t expected = render ? 0xff00ff00u : i * 13u + 7u;
            if (words[i] != expected) fail([NSString stringWithFormat:@"first result[%u]=%#x expected=%#x", i, words[i], expected]);
        }
        if (concurrent) {
            memset(output.contents, 0xa5, 256);
            commands = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:concurrentPipeline[1]];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
            [encoder endEncoding];
            finish(commands);
            for (unsigned word = 0; word < 64; word++)
                if (words[word] != word * 17u + 11u) fail(@"second concurrent pipeline did not write its first result");
            printf("COLD_CONCURRENT_PASS first_pipelines=2 translations=2 result_words=128\n");
        }
        printf("COLD_PIPELINE_PASS mode=%s first_result_words=64 native_hardware_transport=untested\n", argv[3]);
        stopDaemon();
        _exit(0);
    }
}
