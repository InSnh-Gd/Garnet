#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "ICB_LIFECYCLE_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void passCase(NSString *name) {
    printf("ICB_LIFECYCLE_CASE_PASS %s\n", name.UTF8String);
}

static void finish(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ cb nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@", op, (unsigned long)cb.status, cb.error]);
}

static void expectCommandError(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ cb nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusError || !cb.error)
        fail([NSString stringWithFormat:@"%@ did not error cleanly (status=%lu)", op, (unsigned long)cb.status]);
}

static id<MTLTexture> newTarget(id<MTLDevice> dev) {
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:16 height:16 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:td];
}

static NSData *readTexture(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> tex, NSString *op) {
    NSUInteger len = tex.width * tex.height * 4;
    id<MTLBuffer> dst = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(tex.width, tex.height, 1)
                 toBuffer:dst destinationOffset:0
        destinationBytesPerRow:tex.width * 4 destinationBytesPerImage:len];
    [blit endEncoding];
    finish(cb, [op stringByAppendingString:@" readback"]);
    return [NSData dataWithBytes:dst.contents length:len];
}

static void expectColor(NSData *img, uint8_t r, uint8_t g, uint8_t b, uint8_t a, NSString *op) {
    const uint8_t *p = img.bytes;
    const uint8_t want[4] = { r, g, b, a };
    for (NSUInteger i = 0; i < img.length / 4; i++) {
        for (int c = 0; c < 4; c++) {
            int got = p[i * 4 + c];
            if (got < want[c] - 2 || got > want[c] + 2)
                fail([NSString stringWithFormat:@"%@ px%lu ch%d got %d expected %d",
                      op, (unsigned long)i, c, got, want[c]]);
        }
    }
}

static id<MTLRenderCommandEncoder> beginRender(id<MTLCommandQueue> q, id<MTLTexture> target,
                                                id<MTLCommandBuffer> *outCB) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
    *outCB = cb;
    return enc;
}

static const char *kLifecycleMSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float2 p [[attribute(0)]]; };\n"
"struct VOut { float4 pos [[position]]; };\n"
"vertex VOut lc_vs(VIn in [[stage_in]]) { VOut o; o.pos = float4(in.p, 0, 1); return o; }\n"
"fragment float4 lc_fs(constant float4 &col [[buffer(0)]]) { return col; }\n"
"struct EncArgs {\n"
"    command_buffer icb;\n"
"    render_pipeline_state pso;\n"
"    device float2 *vtx;\n"
"    constant float4 *col;\n"
"    device uint *indRange;\n"
"    uint loc;\n"
"    uint len;\n"
"};\n"
"kernel void k_encode_icb(device EncArgs &a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"    if (gid == 0u && a.indRange) {\n"
"        a.indRange[0] = a.loc;\n"
"        a.indRange[1] = a.len;\n"
"    }\n"
"    render_command c(a.icb, gid);\n"
"    c.reset();\n"
"    if (gid == 0u) {\n"
"        c.set_render_pipeline_state(a.pso);\n"
"        c.set_vertex_buffer(a.vtx, 0);\n"
"        c.set_fragment_buffer(a.col, 0);\n"
"        c.draw_primitives(primitive_type::triangle, 0u, 3u, 1u, 0u);\n"
"    }\n"
"}\n";

__attribute__((ns_returns_retained))
static id<MTLRenderPipelineState> makePSO(id<MTLDevice> dev, id<MTLLibrary> lib) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"lc_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"lc_fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.supportIndirectCommandBuffers = YES;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2;
    vd.attributes[0].bufferIndex = 0;
    vd.layouts[0].stride = 8;
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    d.vertexDescriptor = vd;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fail([NSString stringWithFormat:@"lc pso: %@", err]);
    return pso;
}

__attribute__((ns_returns_retained))
static id<MTLIndirectCommandBuffer> makeGPUICB(id<MTLDevice> dev, NSUInteger count) {
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeDraw;
    d.inheritPipelineState = NO;
    d.inheritBuffers = NO;
    d.maxVertexBufferBindCount = 2;
    d.maxFragmentBufferBindCount = 2;
    id<MTLIndirectCommandBuffer> icb =
        [dev newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                            options:MTLResourceStorageModePrivate];
    if (!icb) fail(@"lc icb nil");
    return icb;
}

static void encodeProducerIntoCB(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLLibrary> lib,
                                 id<MTLIndirectCommandBuffer> icb, id<MTLRenderPipelineState> pso,
                                 id<MTLBuffer> vtx, id<MTLBuffer> col, id<MTLBuffer> indRange,
                                 uint32_t loc, uint32_t len, BOOL callUseResource, id<MTLFence> signalFence) {
    NSError *err = nil;
    id<MTLFunction> kf = [lib newFunctionWithName:@"k_encode_icb"];
    id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:kf error:&err];
    if (!cpso) fail([NSString stringWithFormat:@"cpso: %@", err]);
    id<MTLArgumentEncoder> ae = [kf newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0];
    [ae setIndirectCommandBuffer:icb atIndex:0];
    [ae setRenderPipelineState:pso atIndex:1];
    [ae setBuffer:vtx offset:0 atIndex:2];
    [ae setBuffer:col offset:0 atIndex:3];
    [ae setBuffer:indRange offset:0 atIndex:4];
    *(uint32_t *)[ae constantDataAtIndex:5] = loc;
    *(uint32_t *)[ae constantDataAtIndex:6] = len;

    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:cpso];
    [ce setBuffer:args offset:0 atIndex:0];
    if (callUseResource) {
        [ce useResource:icb usage:MTLResourceUsageWrite];
        [ce useResource:vtx usage:MTLResourceUsageRead];
        [ce useResource:col usage:MTLResourceUsageRead];
        if (indRange) [ce useResource:indRange usage:MTLResourceUsageWrite];
    }
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    if (signalFence) [ce updateFence:signalFence];
    [ce endEncoding];
}

// 1. Early ARC release (-O0 and -O2) + waitUntilCompleted
static void testEarlyRelease(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, id<MTLTexture> target) {
    __weak id weakICB = nil, weakPSO = nil, weakVtx = nil, weakCol = nil, weakRange = nil;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        @autoreleasepool {
            const float tri[6] = { -1, -1, 3, -1, -1, 3 };
            const float green[4] = { 0, 1, 0, 1 };
            id<MTLIndirectCommandBuffer> icb = makeGPUICB(dev, 2);
            id<MTLRenderPipelineState> pso = makePSO(dev, lib);
            id<MTLBuffer> vtx = [dev newBufferWithBytes:tri length:sizeof tri options:MTLResourceStorageModeShared];
            id<MTLBuffer> col = [dev newBufferWithBytes:green length:sizeof green options:MTLResourceStorageModeShared];
            id<MTLBuffer> ind = [dev newBufferWithLength:16 options:MTLResourceStorageModePrivate];
            weakICB = icb; weakPSO = pso; weakVtx = vtx; weakCol = col; weakRange = ind;

            encodeProducerIntoCB(dev, cb, lib, icb, pso, vtx, col, ind, 0, 1, YES, nil);

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
            [enc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
            [enc useResource:icb usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [enc useResource:vtx usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [enc useResource:col usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
            [enc executeCommandsInBuffer:icb indirectBuffer:ind indirectBufferOffset:0];
            [enc endEncoding];
        }
        // Caller references were dropped before commit; command buffer must keep everything alive until completion!
        finish(cb, @"early-release");
    }
    @autoreleasepool {
        expectColor(readTexture(dev, q, target, @"early-release"), 0, 255, 0, 255, @"early-release-green");
    }
    if (weakICB || weakPSO || weakVtx || weakCol || weakRange)
        fail(@"retired command buffer leaked encoded ICB lifecycle objects");
    passCase(@"early-arc-release-and-retirement");
}

// 2. MTLHeap + makeAliasable + useResource:
static void testHeapAndAliasReuse(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = 256 * 1024;
    hd.storageMode = MTLStorageModeShared;
    hd.hazardTrackingMode = MTLHazardTrackingModeDefault;
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    if (!heap) fail(@"heap nil");

    const float tri[6] = { -1, -1, 3, -1, -1, 3 };
    const float red[4] = { 1, 0, 0, 1 };
    const float blue[4] = { 0, 0, 1, 1 };
    id<MTLRenderPipelineState> pso = makePSO(dev, lib);
    id<MTLIndirectCommandBuffer> icb = makeGPUICB(dev, 1);
    id<MTLTexture> target = newTarget(dev);

    id<MTLBuffer> vtx = [heap newBufferWithLength:sizeof tri options:MTLResourceStorageModeShared];
    id<MTLBuffer> col = [heap newBufferWithLength:sizeof red options:MTLResourceStorageModeShared];
    memcpy(vtx.contents, tri, sizeof tri);
    memcpy(col.contents, red, sizeof red);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    encodeProducerIntoCB(dev, cb, lib, icb, pso, vtx, col, nil, 0, 1, YES, nil);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
    [enc useHeap:heap stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [enc useResource:icb usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    finish(cb, @"heap-pass1");
    expectColor(readTexture(dev, q, target, @"heap-pass1"), 255, 0, 0, 255, @"heap-pass1-red");

    [vtx makeAliasable];
    [col makeAliasable];
    vtx = nil; col = nil;

    id<MTLBuffer> vtx2 = [heap newBufferWithLength:sizeof tri options:MTLResourceStorageModeShared];
    id<MTLBuffer> col2 = [heap newBufferWithLength:sizeof blue options:MTLResourceStorageModeShared];
    memcpy(vtx2.contents, tri, sizeof tri);
    memcpy(col2.contents, blue, sizeof blue);

    cb = [q commandBuffer];
    encodeProducerIntoCB(dev, cb, lib, icb, pso, vtx2, col2, nil, 0, 1, YES, nil);
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
    [enc useHeap:heap stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [enc useResource:icb usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    finish(cb, @"heap-pass2");
    expectColor(readTexture(dev, q, target, @"heap-pass2"), 0, 0, 255, 255, @"heap-pass2-blue");
    passCase(@"heap-aliasable-and-use-resource");
}

// 3. Cross-queue MTLEvent / MTLSharedEvent + MTLFence + Repeat Reset/Reuse
static void testSyncAndRepeatReuse(id<MTLDevice> dev, id<MTLCommandQueue> q1, id<MTLCommandQueue> q2, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> pso = makePSO(dev, lib);
    id<MTLIndirectCommandBuffer> icb = makeGPUICB(dev, 2);
    const float tri[6] = { -1, -1, 3, -1, -1, 3 };
    const float colors[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
    id<MTLBuffer> vtx = [dev newBufferWithBytes:tri length:sizeof tri options:MTLResourceStorageModeShared];
    id<MTLTexture> target = newTarget(dev);

    id<MTLEvent> ev = [dev newEvent];
    id<MTLSharedEvent> sev = [dev newSharedEvent];
    uint64_t val = 1;

    for (int iter = 0; iter < 3; iter++) {
        id<MTLBuffer> col = [dev newBufferWithBytes:colors[iter] length:sizeof colors[iter] options:MTLResourceStorageModeShared];
        // Queue 1: blit reset -> compute encode -> signal ev
        id<MTLCommandBuffer> cb1 = [q1 commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb1 blitCommandEncoder];
        [bl resetCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
        [bl endEncoding];
        encodeProducerIntoCB(dev, cb1, lib, icb, pso, vtx, col, nil, 0, 1, YES, nil);
        [cb1 encodeSignalEvent:ev value:val];
        [cb1 encodeSignalEvent:sev value:val];
        [cb1 commit];

        // Queue 2: wait ev + sev -> render ICB + verify output
        id<MTLCommandBuffer> rcb = [q2 commandBuffer];
        [rcb encodeWaitForEvent:ev value:val];
        [rcb encodeWaitForEvent:sev value:val];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> renc = [rcb renderCommandEncoderWithDescriptor:rp];
        [renc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
        [renc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
        [renc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
        [renc endEncoding];
        [rcb commit];
        [cb1 waitUntilCompleted];
        [rcb waitUntilCompleted];
        if (cb1.status != MTLCommandBufferStatusCompleted || rcb.status != MTLCommandBufferStatusCompleted)
            fail(@"cross-queue sync failed");
        expectColor(readTexture(dev, q2, target, @"sync-iter"),
                    iter == 0 ? 255 : 0, iter == 1 ? 255 : 0, iter == 2 ? 255 : 0, 255, @"cross-queue-sync");
        val++;
    }

    // Same-CB MTLFence compute -> render
    id<MTLFence> fence = [dev newFence];
    id<MTLBuffer> cY = [dev newBufferWithBytes:(const float[4]){ 1, 1, 0, 1 } length:16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q1 commandBuffer];
    encodeProducerIntoCB(dev, cb, lib, icb, pso, vtx, cY, nil, 0, 1, YES, fence);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, 16, 16 }];
    [enc waitForFence:fence beforeStages:MTLRenderStageVertex];
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    finish(cb, @"fence-compute-to-render");
    expectColor(readTexture(dev, q1, target, @"fence"), 255, 255, 0, 255, @"fence-yellow");
    passCase(@"cross-queue-events-fence-and-repeat-reuse");
}

// 4. Partial-submit failure quarantine
static void testPartialSubmitQuarantine(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> pso = makePSO(dev, lib);
    id<MTLIndirectCommandBuffer> icb = makeGPUICB(dev, 2);
    const float tri[6] = { -1, -1, 3, -1, -1, 3 };
    const float red[4] = { 1, 0, 0, 1 };
    id<MTLBuffer> vtx = [dev newBufferWithBytes:tri length:sizeof tri options:MTLResourceStorageModeShared];
    id<MTLBuffer> col = [dev newBufferWithBytes:red length:sizeof red options:MTLResourceStorageModeShared];

    // Commit valid Red slot
    id<MTLCommandBuffer> cb0 = [q commandBuffer];
    encodeProducerIntoCB(dev, cb0, lib, icb, pso, vtx, col, nil, 0, 1, YES, nil);
    finish(cb0, @"quarantine-seed");

    // Failed CB1: encodes GPU blit reset on [0..2), then injects an invalid range -> must NOT run reset on GPU!
    id<MTLCommandBuffer> cb1 = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb1 blitCommandEncoder];
    [bl resetCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
    [bl resetCommandsInBuffer:icb withRange:NSMakeRange(5, 1)]; // out of range!
    [bl endEncoding];
    expectCommandError(cb1, @"quarantine-failed-cb1");

    // CB2 must still see Red intact!
    id<MTLTexture> target = newTarget(dev);
    id<MTLCommandBuffer> cb2 = nil;
    id<MTLRenderCommandEncoder> enc = beginRender(q, target, &cb2);
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
    [enc endEncoding];
    finish(cb2, @"quarantine-cb2");
    expectColor(readTexture(dev, q, target, @"quarantine"), 255, 0, 0, 255, @"quarantine-intact-red");

    // Support query consistency check
    id<MTLFunction> kf = [lib newFunctionWithName:@"k_encode_icb"];
    NSError *err = nil;
    id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:kf error:&err];
    if (!pso.supportIndirectCommandBuffers || cpso.supportIndirectCommandBuffers)
        fail(@"supportIndirectCommandBuffers query inconsistent");
    MTLIndirectCommandBufferDescriptor *cd = [MTLIndirectCommandBufferDescriptor new];
    cd.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    if ([dev newIndirectCommandBufferWithDescriptor:cd maxCommandCount:2 options:MTLResourceStorageModePrivate])
        fail(@"compute ICB descriptor accepted unexpectedly");

    passCase(@"partial-submit-quarantine-and-support-queries");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-icb-lifecycle BUNDLE|--reference");
        return 0;
    }
    if (argc != 2) return 2;
    BOOL ref = !strcmp(argv[1], "--reference");
    if (!ref && geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        if (ref) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                fail(err.description);
            dev = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        if (!dev) fail(@"device nil");
        id<MTLCommandQueue> q1 = [dev newCommandQueue];
        id<MTLCommandQueue> q2 = [dev newCommandQueue];
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_1;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kLifecycleMSL) options:opts error:&err];
        if (!lib) fail([NSString stringWithFormat:@"lc lib: %@", err]);

        id<MTLTexture> earlyTarget = newTarget(dev);
        testEarlyRelease(dev, q1, lib, earlyTarget);
        testHeapAndAliasReuse(dev, q1, lib);
        testSyncAndRepeatReuse(dev, q1, q2, lib);
        if (!ref) testPartialSubmitQuarantine(dev, q1, lib);
        printf("ICB_LIFECYCLE_SUITE_PASS\n");
    }
    return 0;
}
