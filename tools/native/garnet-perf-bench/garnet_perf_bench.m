// Standalone Metal microbenchmark and CoreGraphics refresh-rate helper for
// Garnet physical RX 9060 XT A/B performance baselines (macOS x86_64).
// Compiled into ~/rdna4-build/perf-bench/ with Apple CLT system frameworks only.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <mach/mach_time.h>
#import <sys/resource.h>
#import <math.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (mach_absolute_time() * tb.numer) / tb.denom;
}

static double proc_cpu_ms(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    double u = (double)ru.ru_utime.tv_sec * 1e3 + (double)ru.ru_utime.tv_usec / 1e3;
    double s = (double)ru.ru_stime.tv_sec * 1e3 + (double)ru.ru_stime.tv_usec / 1e3;
    return u + s;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb ? 1 : 0);
}

static int cmd_display_info(void) {
    CGDirectDisplayID disp = CGMainDisplayID();
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(disp);
    if (!cur) {
        fprintf(stderr, "garnet_perf_bench: CGDisplayCopyDisplayMode failed\n");
        return 1;
    }
    printf("display_id=%u\n", (unsigned)disp);
    printf("pixel_width=%zu\n", CGDisplayModeGetPixelWidth(cur));
    printf("pixel_height=%zu\n", CGDisplayModeGetPixelHeight(cur));
    printf("logical_width=%zu\n", CGDisplayModeGetWidth(cur));
    printf("logical_height=%zu\n", CGDisplayModeGetHeight(cur));
    printf("refresh_hz=%.3f\n", CGDisplayModeGetRefreshRate(cur));
    CGDisplayModeRelease(cur);
    return 0;
}

static int cmd_set_refresh(double targetHz) {
    CGDirectDisplayID disp = CGMainDisplayID();
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(disp);
    if (!cur) {
        fprintf(stderr, "garnet_perf_bench: cannot read current mode\n");
        return 1;
    }
    size_t wantPW = CGDisplayModeGetPixelWidth(cur);
    size_t wantPH = CGDisplayModeGetPixelHeight(cur);
    size_t wantLW = CGDisplayModeGetWidth(cur);
    size_t wantLH = CGDisplayModeGetHeight(cur);
    double curHz = CGDisplayModeGetRefreshRate(cur);
    CGDisplayModeRelease(cur);

    if (fabs(curHz - targetHz) < 1.0) {
        printf("refresh_unchanged=%.3f\n", curHz);
        return 0;
    }

    NSDictionary *opts = @{ (__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes : @YES };
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(disp, (__bridge CFDictionaryRef)opts);
    if (!modes) {
        fprintf(stderr, "garnet_perf_bench: CGDisplayCopyAllDisplayModes failed\n");
        return 1;
    }

    CGDisplayModeRef best = NULL;
    double bestDiff = 1e9;
    CFIndex n = CFArrayGetCount(modes);
    for (CFIndex i = 0; i < n; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        if (CGDisplayModeGetPixelWidth(m) != wantPW || CGDisplayModeGetPixelHeight(m) != wantPH)
            continue;
        if (CGDisplayModeGetWidth(m) != wantLW || CGDisplayModeGetHeight(m) != wantLH)
            continue;
        double hz = CGDisplayModeGetRefreshRate(m);
        double diff = fabs(hz - targetHz);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = m;
        }
    }
    if (!best || bestDiff > 2.5) {
        fprintf(stderr, "garnet_perf_bench: no %zux%zu (logical %zux%zu) mode near %.2f Hz (bestDiff=%.2f)\n",
                wantPW, wantPH, wantLW, wantLH, targetHz, bestDiff);
        CFRelease(modes);
        return 2;
    }

    CGDisplayConfigRef cfg = NULL;
    CGError err = CGBeginDisplayConfiguration(&cfg);
    if (err == kCGErrorSuccess) {
        err = CGConfigureDisplayWithDisplayMode(cfg, disp, best, NULL);
        if (err == kCGErrorSuccess) {
            err = CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession);
        } else {
            CGCancelDisplayConfiguration(cfg);
        }
    }
    double gotHz = CGDisplayModeGetRefreshRate(best);
    CFRelease(modes);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "garnet_perf_bench: CGCompleteDisplayConfiguration(%.2f Hz) err=%d\n", gotHz, (int)err);
        return 3;
    }
    printf("refresh_set=%.3f\n", gotHz);
    return 0;
}

static id<MTLDevice> pick_garnet_device(void) {
    NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
    for (id<MTLDevice> d in devs) {
        NSString *name = d.name ?: @"";
        if ([name rangeOfString:@"Garnet" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [name rangeOfString:@"Radeon" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [name rangeOfString:@"9060" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [name rangeOfString:@"AMD" options:NSCaseInsensitiveSearch].location != NSNotFound) {
            return d;
        }
    }
    return MTLCreateSystemDefaultDevice();
}

static int cmd_run_bench(void) {
    @autoreleasepool {
        id<MTLDevice> dev = pick_garnet_device();
        if (!dev) {
            fprintf(stderr, "garnet_perf_bench: no Metal device\n");
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!q) {
            fprintf(stderr, "garnet_perf_bench: newCommandQueue failed\n");
            return 1;
        }

        // Warm up command queue once.
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) {
                fprintf(stderr, "garnet_perf_bench: warmup cb error: %s\n",
                        cb.error.localizedDescription.UTF8String);
                return 2;
            }
        }

        // 1. Trivial submit-to-completion roundtrip latency (500 iterations).
        enum { kSubmitN = 500 };
        uint64_t lats[kSubmitN];
        double cpu0 = proc_cpu_ms();
        uint64_t wall0 = now_ns();
        for (int i = 0; i < kSubmitN; i++) {
            uint64_t t0 = now_ns();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
            lats[i] = now_ns() - t0;
            if (cb.error) {
                fprintf(stderr, "garnet_perf_bench: submit #%d error: %s\n",
                        i, cb.error.localizedDescription.UTF8String);
                return 3;
            }
        }
        double submitWallMs = (double)(now_ns() - wall0) / 1e6;
        double submitCpuMs = proc_cpu_ms() - cpu0;
        qsort(lats, kSubmitN, sizeof(uint64_t), cmp_u64);
        double p50Us = (double)lats[kSubmitN / 2] / 1e3;
        double p95Us = (double)lats[(kSubmitN * 95) / 100] / 1e3;
        double p99Us = (double)lats[(kSubmitN * 99) / 100] / 1e3;
        double meanUs = (submitWallMs * 1e3) / (double)kSubmitN;

        // 2. Pipeline compile latency (cold MSL + warm repeated pipeline).
        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "vertex float4 v_main(uint vid [[vertex_id]]) {\n"
             "    float2 p = float2((vid == 1) ? 1.0f : -1.0f, (vid == 2) ? 1.0f : -1.0f);\n"
             "    return float4(p, 0.0f, 1.0f);\n"
             "}\n"
             "fragment float4 f_main() { return float4(0.25f, 0.5f, 0.75f, 1.0f); }\n"
             "kernel void k_fill(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
             "    out[gid] = gid ^ 0x5A5A5A5Au;\n"
             "}\n";

        NSError *err = nil;
        uint64_t tComp0 = now_ns();
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "garnet_perf_bench: newLibraryWithSource failed: %s\n",
                    err.localizedDescription.UTF8String);
            return 4;
        }
        id<MTLFunction> vf = [lib newFunctionWithName:@"v_main"];
        id<MTLFunction> ff = [lib newFunctionWithName:@"f_main"];
        id<MTLFunction> kf = [lib newFunctionWithName:@"k_fill"];

        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = vf;
        rpd.fragmentFunction = ff;
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!rpso) {
            fprintf(stderr, "garnet_perf_bench: newRenderPipelineState failed: %s\n",
                    err.localizedDescription.UTF8String);
            return 5;
        }
        id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:kf error:&err];
        if (!cpso) {
            fprintf(stderr, "garnet_perf_bench: newComputePipelineState failed: %s\n",
                    err.localizedDescription.UTF8String);
            return 6;
        }
        double coldPipeMs = (double)(now_ns() - tComp0) / 1e6;

        uint64_t tWarm0 = now_ns();
        id<MTLRenderPipelineState> rpso2 = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        id<MTLComputePipelineState> cpso2 = [dev newComputePipelineStateWithFunction:kf error:&err];
        double warmPipeMs = (double)(now_ns() - tWarm0) / 1e6;
        (void)rpso2;
        (void)cpso2;

        // 3. Draw-call throughput: 10,000 draws across 20 command buffers (500 draws/CB).
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:256
                                                                                     height:256
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        if (!rt) {
            fprintf(stderr, "garnet_perf_bench: render target alloc failed\n");
            return 7;
        }

        enum { kDrawCBs = 20, kDrawsPerCB = 500 };
        double drawCpu0 = proc_cpu_ms();
        uint64_t drawWall0 = now_ns();
        double drawEncodeMs = 0.0;
        id<MTLCommandBuffer> lastDrawCB = nil;
        for (int b = 0; b < kDrawCBs; b++) {
            uint64_t te0 = now_ns();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:rpso];
            for (int d = 0; d < kDrawsPerCB; d++) {
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [enc endEncoding];
            [cb commit];
            drawEncodeMs += (double)(now_ns() - te0) / 1e6;
            lastDrawCB = cb;
        }
        [lastDrawCB waitUntilCompleted];
        double drawWallMs = (double)(now_ns() - drawWall0) / 1e6;
        double drawCpuMs = proc_cpu_ms() - drawCpu0;
        if (lastDrawCB.error) {
            fprintf(stderr, "garnet_perf_bench: draw cb error: %s\n",
                    lastDrawCB.error.localizedDescription.UTF8String);
            return 8;
        }

        // 4. Compute dispatch throughput: 100 dispatches over 4 MiB private buffer.
        const NSUInteger compBytes = 4u * 1024u * 1024u;
        const NSUInteger compElems = compBytes / sizeof(uint32_t);
        id<MTLBuffer> compBuf = [dev newBufferWithLength:compBytes options:MTLResourceStorageModePrivate];
        if (!compBuf) {
            fprintf(stderr, "garnet_perf_bench: compute buffer alloc failed\n");
            return 9;
        }
        enum { kCompIters = 100 };
        uint64_t compWall0 = now_ns();
        id<MTLCommandBuffer> compCB = [q commandBuffer];
        id<MTLComputeCommandEncoder> cenc = [compCB computeCommandEncoder];
        [cenc setComputePipelineState:cpso];
        [cenc setBuffer:compBuf offset:0 atIndex:0];
        MTLSize tg = MTLSizeMake(64, 1, 1);
        MTLSize grid = MTLSizeMake(compElems, 1, 1);
        for (int i = 0; i < kCompIters; i++) {
            [cenc dispatchThreads:grid threadsPerThreadgroup:tg];
        }
        [cenc endEncoding];
        [compCB commit];
        [compCB waitUntilCompleted];
        double compWallMs = (double)(now_ns() - compWall0) / 1e6;
        if (compCB.error) {
            fprintf(stderr, "garnet_perf_bench: compute cb error: %s\n",
                    compCB.error.localizedDescription.UTF8String);
            return 10;
        }
        double compGBs = ((double)compBytes * (double)kCompIters / 1e9) / (compWallMs / 1e3);

        // 5. Blit copy throughput: 50 copies of 16 MiB shared -> private buffer.
        const NSUInteger copyBytes = 16u * 1024u * 1024u;
        id<MTLBuffer> srcBuf = [dev newBufferWithLength:copyBytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> dstBuf = [dev newBufferWithLength:copyBytes options:MTLResourceStorageModePrivate];
        if (!srcBuf || !dstBuf) {
            fprintf(stderr, "garnet_perf_bench: blit buffer alloc failed\n");
            return 11;
        }
        memset(srcBuf.contents, 0xA5, copyBytes);
        enum { kBlitIters = 50 };
        uint64_t blitWall0 = now_ns();
        id<MTLCommandBuffer> blitCB = [q commandBuffer];
        id<MTLBlitCommandEncoder> benc = [blitCB blitCommandEncoder];
        for (int i = 0; i < kBlitIters; i++) {
            [benc copyFromBuffer:srcBuf sourceOffset:0 toBuffer:dstBuf destinationOffset:0 size:copyBytes];
        }
        [benc endEncoding];
        [blitCB commit];
        [blitCB waitUntilCompleted];
        double blitWallMs = (double)(now_ns() - blitWall0) / 1e6;
        if (blitCB.error) {
            fprintf(stderr, "garnet_perf_bench: blit cb error: %s\n",
                    blitCB.error.localizedDescription.UTF8String);
            return 12;
        }
        double blitGBs = ((double)copyBytes * (double)kBlitIters / 1e9) / (blitWallMs / 1e3);

        printf("device_name=%s\n", dev.name.UTF8String);
        printf("submit_count=%d\n", kSubmitN);
        printf("submit_p50_us=%.2f\n", p50Us);
        printf("submit_p95_us=%.2f\n", p95Us);
        printf("submit_p99_us=%.2f\n", p99Us);
        printf("submit_mean_us=%.2f\n", meanUs);
        printf("submit_wall_ms=%.2f\n", submitWallMs);
        printf("submit_cpu_ms=%.2f\n", submitCpuMs);
        printf("pipe_cold_ms=%.2f\n", coldPipeMs);
        printf("pipe_warm_ms=%.2f\n", warmPipeMs);
        printf("draw_total_calls=%d\n", kDrawCBs * kDrawsPerCB);
        printf("draw_encode_ms=%.2f\n", drawEncodeMs);
        printf("draw_wall_ms=%.2f\n", drawWallMs);
        printf("draw_cpu_ms=%.2f\n", drawCpuMs);
        printf("compute_wall_ms=%.2f\n", compWallMs);
        printf("compute_gb_s=%.2f\n", compGBs);
        printf("blit_wall_ms=%.2f\n", blitWallMs);
        printf("blit_gb_s=%.2f\n", blitGBs);
        return 0;
    }
}

int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: garnet_perf_bench --display-info | --set-refresh <hz> | --run-bench\n");
        return 2;
    }
    if (strcmp(argv[1], "--display-info") == 0) return cmd_display_info();
    if (strcmp(argv[1], "--set-refresh") == 0 && argc >= 3) return cmd_set_refresh(atof(argv[2]));
    if (strcmp(argv[1], "--run-bench") == 0) return cmd_run_bench();
    fprintf(stderr, "Unknown option: %s\n", argv[1]);
    return 2;
}
