// test-metal3-counters-amp.m: live RX 9060 XT / reference Metal verification for Task 2.5
// (GPU Timestamp Counter Sampling: counterSets, supportsCounterSampling:, sampleTimestamps:gpuTimestamp:,
// render/compute/blit sampleCountersInBuffer:atSampleIndex:withBarrier:, pass-descriptor
// sampleBufferAttachments, resolveCounterRange:, blit resolveCounters:inRange:destinationBuffer:,
// and Vertex Amplification: supportsVertexAmplificationCount:, maxVertexAmplificationCount,
// setVertexAmplificationCount:viewMappings: viewport/layer offset execution & refusals).
// Build on macOS:
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal \
//     src/garnet-metal/test-metal3-counters-amp.m -o "$TMPDIR/test-metal3-counters-amp"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
//     "$TMPDIR/test-metal3-counters-amp" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-counters-amp" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_COUNTERS_AMP_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static void finishOK(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
}

static void finishExpectError(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusError && cb.error != nil,
            [NSString stringWithFormat:@"%@: expected error status, got status=%lu err=%@",
             tag, (unsigned long)cb.status, cb.error]);
}

static id<MTLCounterSet> findTimestampSet(id<MTLDevice> dev) {
    for (id<MTLCounterSet> set in dev.counterSets) {
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) return set;
    }
    return nil;
}

static void readTextureSlice(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> tex,
                             NSUInteger slice, uint32_t w, uint32_t h, uint8_t *out, NSString *tag) {
    NSUInteger bytes = (NSUInteger)w * (NSUInteger)h * 4u;
    id<MTLBuffer> stg = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(stg != nil, [NSString stringWithFormat:@"%@: staging nil", tag]);
    memset(stg.contents, 0xA5, bytes);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    require(blit != nil, [NSString stringWithFormat:@"%@: blit nil", tag]);
    [blit copyFromTexture:tex sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:stg destinationOffset:0
   destinationBytesPerRow:(NSUInteger)w * 4u destinationBytesPerImage:bytes];
    [blit endEncoding];
    finishOK(cb, tag);
    memcpy(out, stg.contents, bytes);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc != 2) {
            fprintf(stderr, "Usage: %s BUNDLE|--reference\n", argv[0]);
            return 2;
        }
        BOOL reference = strcmp(argv[1], "--reference") == 0;
        id<MTLDevice> dev = nil;
        if (reference) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            setenv("RDNA4M_ALLOW", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                failf([NSString stringWithFormat:@"bundle load: %@", err]);
            Class cls = objc_getClass("GarnetDevice");
            require(cls != Nil, @"GarnetDevice missing");
            dev = [[cls alloc] init];
            if ([dev respondsToSelector:sel_registerName("initLimits")])
                ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        require(dev != nil, @"device unavailable");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void burn_and_fill(device uint *dst [[buffer(0)]],\n"
             "                          uint i [[thread_position_in_grid]]) {\n"
             "    uint v = i * 2654435761u;\n"
             "    for (uint k = 0; k < 64u; k++) v = (v ^ (v >> 13)) * 1597334677u + k;\n"
             "    dst[i] = v;\n"
             "}\n"
             "struct VOut { float4 pos [[position]]; };\n"
             "vertex VOut vs_quad(uint vid [[vertex_id]]) {\n"
             "    const float2 pts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
             "    VOut o; o.pos = float4(pts[vid % 3], 0.0, 1.0); return o;\n"
             "}\n"
             "fragment float4 fs_red() { return float4(1.0, 0.0, 0.0, 1.0); }\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"library compile: %@", err]);
        id<MTLComputePipelineState> compPso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"burn_and_fill"] error:&err];
        require(compPso != nil, [NSString stringWithFormat:@"compute pipeline: %@", err]);

        // ------------------------------------------------------------------
        // Part 1: GPU Timestamp Counter Sampling
        // ------------------------------------------------------------------
        id<MTLCounterSet> tsSet = findTimestampSet(dev);
        require(tsSet != nil, @"MTLCommonCounterSetTimestamp missing from device.counterSets");
        BOOL foundTsCounter = NO;
        for (id<MTLCounter> c in tsSet.counters) {
            if ([c.name isEqualToString:MTLCommonCounterTimestamp]) foundTsCounter = YES;
        }
        require(foundTsCounter, @"MTLCommonCounterTimestamp missing from timestamp counterSet");

        BOOL supStage = [dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
        BOOL supDraw = [dev supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary];
        BOOL supDispatch = [dev supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary];
        BOOL supBlit = [dev supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary];
        BOOL supTile = [dev supportsCounterSampling:MTLCounterSamplingPointAtTileDispatchBoundary];
        require(supStage, @"supportsCounterSampling(AtStageBoundary) == NO");
        if (!reference) {
            require(supDraw && supDispatch && supBlit, @"Garnet should support Draw/Dispatch/Blit timestamp boundaries");
            require(!supTile, @"EX04: supportsCounterSampling(AtTileDispatchBoundary) must remain NO on Garnet");
            require(![dev supportsCounterSampling:(MTLCounterSamplingPoint)99], @"unknown sampling point must be NO");
        }

        // Calibrated device timestamps.
        MTLTimestamp cpu0 = 0, gpu0 = 0, cpu1 = 0, gpu1 = 0;
        [dev sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
        require(cpu0 > 0 && gpu0 > 0, @"first sampleTimestamps returned zero");

        // 1A. Compute timestamp sampling (explicit draw/dispatch boundary + pass descriptor boundary).
        {
            MTLCounterSampleBufferDescriptor *csd = [MTLCounterSampleBufferDescriptor new];
            csd.counterSet = tsSet;
            csd.storageMode = MTLStorageModeShared;
            csd.sampleCount = 8;
            csd.label = @"compute-ts";
            id<MTLCounterSampleBuffer> csb = [dev newCounterSampleBufferWithDescriptor:csd error:&err];
            require(csb != nil && csb.sampleCount == 8, [NSString stringWithFormat:@"create Shared CSB failed: %@", err]);

            const NSUInteger nElem = 4096;
            id<MTLBuffer> outBuf = [dev newBufferWithLength:nElem * sizeof(uint32_t) options:MTLResourceStorageModeShared];
            require(outBuf != nil, @"compute output buffer nil");
            memset(outBuf.contents, 0, outBuf.length);

            MTLComputePassDescriptor *cpd = [MTLComputePassDescriptor computePassDescriptor];
            cpd.sampleBufferAttachments[0].sampleBuffer = csb;
            cpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
            cpd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 3;

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoderWithDescriptor:cpd];
            require(enc != nil, @"computeCommandEncoderWithDescriptor nil");
            if (supDispatch) [enc sampleCountersInBuffer:csb atSampleIndex:1 withBarrier:NO];
            [enc setComputePipelineState:compPso];
            [enc setBuffer:outBuf offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(nElem, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            if (supDispatch) [enc sampleCountersInBuffer:csb atSampleIndex:2 withBarrier:YES];
            [enc endEncoding];
            finishOK(cb, @"compute_timestamps");

            // Verify compute actually executed.
            uint32_t exp0 = 0;
            for (uint32_t k = 0; k < 64u; k++) exp0 = (exp0 ^ (exp0 >> 13)) * 1597334677u + k;
            const uint32_t *got = (const uint32_t *)outBuf.contents;
            require(got[0] == exp0, @"compute burn_and_fill output mismatch");

            NSData *res = [csb resolveCounterRange:NSMakeRange(0, 6)];
            require(res != nil && res.length == 6 * sizeof(MTLCounterResultTimestamp), @"compute resolveCounterRange failed");
            const MTLCounterResultTimestamp *ts = (const MTLCounterResultTimestamp *)res.bytes;
            require(ts[0].timestamp != MTLCounterErrorValue && ts[0].timestamp > 0, @"compute start stage ts invalid");
            require(ts[3].timestamp != MTLCounterErrorValue && ts[3].timestamp >= ts[0].timestamp,
                    @"compute end stage ts not monotonic");
            if (supDispatch) {
                require(ts[1].timestamp != MTLCounterErrorValue && ts[1].timestamp > 0, @"compute dispatch start ts invalid");
                require(ts[2].timestamp != MTLCounterErrorValue && ts[2].timestamp >= ts[1].timestamp,
                        @"compute dispatch end ts not monotonic");
            }
            if (!reference) {
                require(ts[4].timestamp == MTLCounterErrorValue && ts[5].timestamp == MTLCounterErrorValue,
                        @"unsampled slots must resolve to MTLCounterErrorValue");
            }
            puts("METAL3_COUNTERS_CASE_PASS compute_timestamps");
        }

        // 1B. Blit timestamp sampling + Private CSB resolved via blit resolveCounters:inRange:destinationBuffer:.
        {
            MTLCounterSampleBufferDescriptor *csd = [MTLCounterSampleBufferDescriptor new];
            csd.counterSet = tsSet;
            csd.storageMode = MTLStorageModePrivate;
            csd.sampleCount = 6;
            csd.label = @"blit-private-ts";
            id<MTLCounterSampleBuffer> csb = [dev newCounterSampleBufferWithDescriptor:csd error:&err];
            require(csb != nil && csb.sampleCount == 6, [NSString stringWithFormat:@"create Private CSB failed: %@", err]);
            if (!reference) {
                require([csb resolveCounterRange:NSMakeRange(0, 2)] == nil,
                        @"CPU resolveCounterRange must fail on Private counter sample buffer");
            }

            const NSUInteger copyBytes = 256 * 1024;
            id<MTLBuffer> src = [dev newBufferWithLength:copyBytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> dst = [dev newBufferWithLength:copyBytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> resShared = [dev newBufferWithLength:6 * sizeof(uint64_t) options:MTLResourceStorageModeShared];
            id<MTLBuffer> resPriv = [dev newBufferWithLength:6 * sizeof(uint64_t) options:MTLResourceStorageModePrivate];
            id<MTLBuffer> resReadback = [dev newBufferWithLength:6 * sizeof(uint64_t) options:MTLResourceStorageModeShared];
            require(src && dst && resShared && resPriv && resReadback, @"blit buffers nil");
            memset(src.contents, 0x5A, copyBytes);
            memset(dst.contents, 0x00, copyBytes);
            memset(resShared.contents, 0x00, resShared.length);
            memset(resReadback.contents, 0x00, resReadback.length);

            MTLBlitPassDescriptor *bpd = [MTLBlitPassDescriptor blitPassDescriptor];
            bpd.sampleBufferAttachments[0].sampleBuffer = csb;
            bpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
            bpd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 3;

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoderWithDescriptor:bpd];
            require(blit != nil, @"blitCommandEncoderWithDescriptor nil");
            if (supBlit) [blit sampleCountersInBuffer:csb atSampleIndex:1 withBarrier:NO];
            [blit copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:copyBytes];
            if (supBlit) [blit sampleCountersInBuffer:csb atSampleIndex:2 withBarrier:YES];
            [blit endEncoding];

            id<MTLBlitCommandEncoder> resBlit = [cb blitCommandEncoder];
            [resBlit resolveCounters:csb inRange:NSMakeRange(0, 5) destinationBuffer:resShared destinationOffset:0];
            [resBlit resolveCounters:csb inRange:NSMakeRange(0, 5) destinationBuffer:resPriv destinationOffset:0];
            [resBlit copyFromBuffer:resPriv sourceOffset:0 toBuffer:resReadback destinationOffset:0 size:5 * sizeof(uint64_t)];
            [resBlit endEncoding];
            finishOK(cb, @"blit_timestamps_and_resolve");

            require(memcmp(src.contents, dst.contents, copyBytes) == 0, @"blit copy bytes mismatch");
            const uint64_t *tsS = (const uint64_t *)resShared.contents;
            const uint64_t *tsP = (const uint64_t *)resReadback.contents;
            require(memcmp(tsS, tsP, 5 * sizeof(uint64_t)) == 0, @"Shared vs Private blit resolveCounters mismatch");
            require(tsS[0] != MTLCounterErrorValue && tsS[0] > 0 && tsS[3] != MTLCounterErrorValue && tsS[3] >= tsS[0],
                    @"blit stage timestamps invalid or non-monotonic");
            if (supBlit) {
                require(tsS[1] != MTLCounterErrorValue && tsS[1] > 0 && tsS[2] != MTLCounterErrorValue && tsS[2] >= tsS[1],
                        @"blit boundary timestamps invalid or non-monotonic");
            }
            if (!reference) require(tsS[4] == MTLCounterErrorValue, @"unsampled Private slot must resolve to error sentinel");
            puts("METAL3_COUNTERS_CASE_PASS blit_private_timestamps_and_resolve");
        }

        // 1C. Render timestamps (pass-descriptor stage boundaries + in-pass draw boundaries).
        MTLRenderPipelineDescriptor *rpdBase = [MTLRenderPipelineDescriptor new];
        rpdBase.vertexFunction = [lib newFunctionWithName:@"vs_quad"];
        rpdBase.fragmentFunction = [lib newFunctionWithName:@"fs_red"];
        rpdBase.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rpdBase.maxVertexAmplificationCount = 1;
        id<MTLRenderPipelineState> renderPso = [dev newRenderPipelineStateWithDescriptor:rpdBase error:&err];
        require(renderPso != nil, [NSString stringWithFormat:@"render pipeline failed: %@", err]);

        {
            MTLCounterSampleBufferDescriptor *csd = [MTLCounterSampleBufferDescriptor new];
            csd.counterSet = tsSet;
            csd.storageMode = MTLStorageModeShared;
            csd.sampleCount = 8;
            id<MTLCounterSampleBuffer> csb = [dev newCounterSampleBufferWithDescriptor:csd error:&err];
            require(csb != nil, [NSString stringWithFormat:@"render CSB failed: %@", err]);

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:32 height:32 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            require(rt != nil, @"render target nil");

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
            rp.sampleBufferAttachments[0].sampleBuffer = csb;
            rp.sampleBufferAttachments[0].startOfVertexSampleIndex = 0;
            rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = 1;
            rp.sampleBufferAttachments[0].endOfVertexSampleIndex = 4;
            rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = 5;

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
            require(renc != nil, @"render encoder nil");
            [renc setRenderPipelineState:renderPso];
            if (supDraw) [renc sampleCountersInBuffer:csb atSampleIndex:2 withBarrier:NO];
            [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            if (supDraw) [renc sampleCountersInBuffer:csb atSampleIndex:3 withBarrier:YES];
            [renc endEncoding];
            finishOK(cb, @"render_timestamps");

            uint8_t px[32 * 32 * 4] = {0};
            readTextureSlice(dev, q, rt, 0, 32, 32, px, @"read_render_ts_rt");
            require(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255, @"render draw pixel mismatch");

            NSData *res = [csb resolveCounterRange:NSMakeRange(0, 6)];
            require(res != nil && res.length == 6 * sizeof(MTLCounterResultTimestamp), @"render resolveCounterRange failed");
            const MTLCounterResultTimestamp *ts = (const MTLCounterResultTimestamp *)res.bytes;
            require(ts[0].timestamp != MTLCounterErrorValue && ts[0].timestamp > 0 &&
                    ts[4].timestamp != MTLCounterErrorValue && ts[4].timestamp >= ts[0].timestamp &&
                    ts[5].timestamp != MTLCounterErrorValue && ts[5].timestamp >= ts[1].timestamp &&
                    ts[5].timestamp >= ts[0].timestamp,
                    @"render stage timestamps invalid or non-monotonic");
            if (supDraw) {
                require(ts[2].timestamp != MTLCounterErrorValue && ts[2].timestamp > 0 &&
                        ts[3].timestamp != MTLCounterErrorValue && ts[3].timestamp >= ts[2].timestamp,
                        @"render draw boundary timestamps invalid or non-monotonic");
            }
            puts("METAL3_COUNTERS_CASE_PASS render_timestamps");
        }

        [dev sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
        require(cpu1 >= cpu0 && gpu1 >= gpu0 && gpu1 > 0, @"device sampleTimestamps not monotonic across GPU work");
        puts("METAL3_COUNTERS_CASE_PASS device_sample_timestamps_monotonic");

        // 1D. Negative counter tests (candidate-strict).
        if (!reference) {
            MTLCounterSampleBufferDescriptor *bad = [MTLCounterSampleBufferDescriptor new];
            bad.counterSet = tsSet;
            bad.storageMode = MTLStorageModeShared;
            bad.sampleCount = 0;
            err = nil;
            require([dev newCounterSampleBufferWithDescriptor:bad error:&err] == nil && err != nil,
                    @"sampleCount=0 must fail");

            bad.sampleCount = 4;
            bad.counterSet = nil;
            err = nil;
            require([dev newCounterSampleBufferWithDescriptor:bad error:&err] == nil && err != nil,
                    @"nil counterSet must fail");

            bad.counterSet = tsSet;
            bad.storageMode = MTLStorageModeManaged;
            err = nil;
            require([dev newCounterSampleBufferWithDescriptor:bad error:&err] == nil && err != nil,
                    @"Managed storageMode must fail");

            bad.storageMode = MTLStorageModeMemoryless;
            err = nil;
            require([dev newCounterSampleBufferWithDescriptor:bad error:&err] == nil && err != nil,
                    @"Memoryless storageMode must fail");

            bad.storageMode = MTLStorageModeShared;
            bad.sampleCount = 1000000;
            err = nil;
            require([dev newCounterSampleBufferWithDescriptor:bad error:&err] == nil && err != nil,
                    @"oversized sampleCount must fail");

            bad.sampleCount = 4;
            id<MTLCounterSampleBuffer> csb = [dev newCounterSampleBufferWithDescriptor:bad error:&err];
            require(csb != nil, @"valid 4-slot CSB failed");
            require([csb resolveCounterRange:NSMakeRange(0, 0)] == nil, @"zero-length resolve range must return nil");
            require([csb resolveCounterRange:NSMakeRange(4, 1)] == nil, @"OOB start resolve range must return nil");
            require([csb resolveCounterRange:NSMakeRange(3, 2)] == nil, @"OOB end resolve range must return nil");
            require([csb resolveCounterRange:NSMakeRange(NSUIntegerMax, 1)] == nil, @"overflow resolve range must return nil");

            // Out-of-range sampleIndex on compute encoder fails command buffer cleanly.
            id<MTLCommandBuffer> badCb = [q commandBuffer];
            id<MTLComputeCommandEncoder> badEnc = [badCb computeCommandEncoder];
            [badEnc sampleCountersInBuffer:csb atSampleIndex:4 withBarrier:NO];
            [badEnc endEncoding];
            finishExpectError(badCb, @"compute_oob_sample_index");

            // Out-of-range pass attachment index refuses encoder creation.
            MTLComputePassDescriptor *badCpd = [MTLComputePassDescriptor computePassDescriptor];
            badCpd.sampleBufferAttachments[0].sampleBuffer = csb;
            badCpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 4;
            id<MTLCommandBuffer> badPassCb = [q commandBuffer];
            require([badPassCb computeCommandEncoderWithDescriptor:badCpd] == nil,
                    @"compute pass with OOB sampleBufferAttachment must return nil");
            puts("METAL3_COUNTERS_CASE_PASS negative_counter_refusals");
        }

        // ------------------------------------------------------------------
        // Part 2: Vertex Amplification
        // ------------------------------------------------------------------
        require([dev supportsVertexAmplificationCount:1], @"supportsVertexAmplificationCount(1) must be YES");
        require(![dev supportsVertexAmplificationCount:0], @"supportsVertexAmplificationCount(0) must be NO");
        if (!reference) {
            require(![dev supportsVertexAmplificationCount:2],
                    @"supportsVertexAmplificationCount(2) must stay honest NO while max=1");
        }

        // 2A. count == 1 viewportArrayIndexOffset = 1 renders into right half only.
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:64 height:32 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            require(rt != nil, @"amp viewport RT nil");

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);

            MTLViewport vps[2] = {
                { 0.0, 0.0, 32.0, 32.0, 0.0, 1.0 },
                { 32.0, 0.0, 32.0, 32.0, 0.0, 1.0 }
            };
            MTLVertexAmplificationViewMapping vm = { .viewportArrayIndexOffset = 1, .renderTargetArrayIndexOffset = 0 };

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
            require(renc != nil, @"amp viewport encoder nil");
            [renc setRenderPipelineState:renderPso];
            [renc setViewports:vps count:2];
            [renc setVertexAmplificationCount:1 viewMappings:&vm];
            [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [renc endEncoding];
            finishOK(cb, @"amp_viewport_offset");

            uint8_t px[64 * 32 * 4] = {0};
            readTextureSlice(dev, q, rt, 0, 64, 32, px, @"read_amp_vp_rt");
            const uint8_t *left = &px[(16 * 64 + 8) * 4];
            const uint8_t *right = &px[(16 * 64 + 48) * 4];
            require(left[0] == 0 && left[1] == 0 && left[2] == 255 && left[3] == 255,
                    @"left half should keep blue clear color under viewportArrayIndexOffset=1");
            require(right[0] == 255 && right[1] == 0 && right[2] == 0 && right[3] == 255,
                    @"right half should receive red draw under viewportArrayIndexOffset=1");
            puts("METAL3_AMP_CASE_PASS count1_viewport_offset");
        }

        // 2B. count == 1 renderTargetArrayIndexOffset = 1 renders into slice 1 only (slice 0 stays clear).
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:16 height:16 mipmapped:NO];
            td.textureType = MTLTextureType2DArray;
            td.arrayLength = 2;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> arr = [dev newTextureWithDescriptor:td];
            require(arr != nil, @"amp 2DArray RT nil");

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = arr;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
            rp.renderTargetArrayLength = 2;

            MTLVertexAmplificationViewMapping vm = { .viewportArrayIndexOffset = 0, .renderTargetArrayIndexOffset = 1 };

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
            require(renc != nil, @"amp layer encoder nil");
            [renc setRenderPipelineState:renderPso];
            [renc setVertexAmplificationCount:1 viewMappings:&vm];
            [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [renc endEncoding];
            finishOK(cb, @"amp_layer_offset");

            uint8_t s0[16 * 16 * 4] = {0}, s1[16 * 16 * 4] = {0};
            readTextureSlice(dev, q, arr, 0, 16, 16, s0, @"read_amp_slice0");
            readTextureSlice(dev, q, arr, 1, 16, 16, s1, @"read_amp_slice1");
            require(s0[0] == 0 && s0[1] == 0 && s0[2] == 255 && s0[3] == 255,
                    @"slice 0 should retain blue clear color under renderTargetArrayIndexOffset=1");
            require(s1[0] == 255 && s1[1] == 0 && s1[2] == 0 && s1[3] == 255,
                    @"slice 1 should receive red draw under renderTargetArrayIndexOffset=1");
            puts("METAL3_AMP_CASE_PASS count1_layer_offset");
        }

        // 2C. Negative amplification refusals (candidate-strict).
        if (!reference) {
            MTLRenderPipelineDescriptor *badP = [rpdBase copy];
            badP.maxVertexAmplificationCount = 0;
            err = nil;
            require([dev newRenderPipelineStateWithDescriptor:badP error:&err] == nil && err != nil,
                    @"maxVertexAmplificationCount=0 must be refused");

            badP.maxVertexAmplificationCount = 2;
            err = nil;
            require([dev newRenderPipelineStateWithDescriptor:badP error:&err] == nil && err != nil,
                    @"unsupported maxVertexAmplificationCount=2 must be refused");

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:16 height:16 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            require(rt != nil, @"amp neg RT nil");

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;

            // Unsupported count=2 on encoder -> CB fails cleanly.
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
                [renc setVertexAmplificationCount:2 viewMappings:NULL];
                [renc endEncoding];
                finishExpectError(cb, @"amp_count2_encoder");
            }
            // Out-of-bounds layer offset -> CB fails cleanly.
            {
                MTLVertexAmplificationViewMapping badLayer = { .viewportArrayIndexOffset = 0, .renderTargetArrayIndexOffset = 1 };
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
                [renc setVertexAmplificationCount:1 viewMappings:&badLayer];
                [renc endEncoding];
                finishExpectError(cb, @"amp_oob_layer");
            }
            // Out-of-bounds viewport offset relative to bound viewport count (1) at draw time -> CB fails cleanly.
            {
                MTLVertexAmplificationViewMapping badVp = { .viewportArrayIndexOffset = 1, .renderTargetArrayIndexOffset = 0 };
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rp];
                [renc setRenderPipelineState:renderPso];
                [renc setVertexAmplificationCount:1 viewMappings:&badVp];
                [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [renc endEncoding];
                finishExpectError(cb, @"amp_oob_viewport_draw");
            }
            puts("METAL3_AMP_CASE_PASS negative_amp_refusals");
        }

        printf("METAL3_COUNTERS_AMP_PASS reference=%d\n", reference ? 1 : 0);
        return 0;
    }
}
