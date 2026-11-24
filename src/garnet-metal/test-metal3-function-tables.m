// test-metal3-function-tables.m: live RX 9060 XT / reference Metal verification for Task 3.3
// (MTLVisibleFunctionTable: compute + fragment functionHandleWithFunction:, newVisibleFunctionTableWithDescriptor:,
// setFunction:atIndex:, setFunctions:withRange:, dynamic swap on the SAME PSO, null & OOB slot safety without
// GPU hang, gpuResourceID, ARC early-release lifetime, and Footnote 10 mesh + render function pointer rejection).
// Build on macOS (verify both -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-function-tables.m -o "$TMPDIR/test-metal3-function-tables"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-function-tables" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-function-tables" --reference
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
    fprintf(stderr, "METAL3_VFT_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static void finish(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
}

static void runVftCompute(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                          id<MTLVisibleFunctionTable> vft, const uint32_t *inVals, const uint32_t *slotIdx,
                          uint32_t *outVals, NSUInteger count, NSString *tag) {
    const NSUInteger bytes = count * sizeof(uint32_t);
    id<MTLBuffer> inBuf = [dev newBufferWithBytes:inVals length:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> idxBuf = [dev newBufferWithBytes:slotIdx length:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outBuf = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(inBuf && idxBuf && outBuf && outBuf.contents, [NSString stringWithFormat:@"%@: buffers nil", tag]);
    memset(outBuf.contents, 0xCC, bytes);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: compute encoder nil", tag]);
    [enc setComputePipelineState:pso];
    [enc setBuffer:inBuf offset:0 atIndex:0];
    [enc setBuffer:idxBuf offset:0 atIndex:1];
    [enc setBuffer:outBuf offset:0 atIndex:2];
    [enc setVisibleFunctionTable:vft atBufferIndex:3];
    [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finish(cb, tag);
    memcpy(outVals, outBuf.contents, bytes);
}

static void runVftRender(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso,
                         id<MTLVisibleFunctionTable> vft, uint32_t selectedSlot,
                         uint8_t *outPixels, NSUInteger w, NSUInteger h, NSString *tag) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    require(rt != nil, [NSString stringWithFormat:@"%@: render target nil", tag]);

    id<MTLBuffer> slotBuf = [dev newBufferWithBytes:&selectedSlot length:sizeof(uint32_t) options:MTLResourceStorageModeShared];
    const NSUInteger bpr = w * 4;
    const NSUInteger bytes = bpr * h;
    id<MTLBuffer> readback = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(slotBuf && readback && readback.contents, [NSString stringWithFormat:@"%@: buffers nil", tag]);
    memset(readback.contents, 0x77, bytes);

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    require(enc != nil, [NSString stringWithFormat:@"%@: render encoder nil", tag]);
    [enc setRenderPipelineState:pso];
    [enc setFragmentVisibleFunctionTable:vft atBufferIndex:0];
    [enc setFragmentBuffer:slotBuf offset:0 atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    require(blit != nil, [NSString stringWithFormat:@"%@: blit encoder nil", tag]);
    [blit copyFromTexture:rt sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:readback destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bytes];
    [blit endEncoding];
    finish(cb, tag);
    memcpy(outPixels, readback.contents, bytes);
}

static void verifySolidColor(const uint8_t *pixels, NSUInteger w, NSUInteger h,
                             int r, int g, int b, int a, NSString *tag) {
    const int expRgba[4] = { r, g, b, a };
    for (NSUInteger p = 0; p < w * h; p++) {
        for (int ch = 0; ch < 4; ch++) {
            int diff = (int)pixels[p * 4 + (NSUInteger)ch] - expRgba[ch];
            if (diff < 0) diff = -diff;
            require(diff <= 1,
                    [NSString stringWithFormat:@"%@ px %lu ch %d: got %u expected %d",
                     tag, (unsigned long)p, ch, pixels[p * 4 + (NSUInteger)ch], expRgba[ch]]);
        }
    }
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
             "[[visible]] uint op_add(uint x) { return x + 100u; }\n"
             "[[visible]] uint op_mul(uint x) { return x * 7u; }\n"
             "[[visible]] uint op_xor(uint x) { return x ^ 0x5A5Au; }\n"
             "kernel void vft_compute(\n"
             "    device const uint *inVals [[buffer(0)]],\n"
             "    device const uint *slotIdx [[buffer(1)]],\n"
             "    device uint *outVals [[buffer(2)]],\n"
             "    visible_function_table<uint(uint)> fnTable [[buffer(3)]],\n"
             "    uint gid [[thread_position_in_grid]]) {\n"
             "    outVals[gid] = fnTable[slotIdx[gid]](inVals[gid]);\n"
             "}\n"
             "[[visible]] float4 color_red(float4 c) { return float4(1.0f, 0.0f, 0.0f, c.a); }\n"
             "[[visible]] float4 color_green(float4 c) { return float4(0.0f, 1.0f, 0.0f, c.a); }\n"
             "vertex float4 vft_vs(uint vid [[vertex_id]]) {\n"
             "    float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };\n"
             "    return float4(p[vid], 0.0f, 1.0f);\n"
             "}\n"
             "fragment float4 vft_fs(\n"
             "    float4 pos [[position]],\n"
             "    visible_function_table<float4(float4)> colorTable [[buffer(0)]],\n"
             "    constant uint &slot [[buffer(1)]]) {\n"
             "    return colorTable[slot](float4(0.0f, 0.0f, 0.0f, 1.0f));\n"
             "}\n";

        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_4;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:opts error:&err];
        require(lib != nil, [NSString stringWithFormat:@"compile VFT library failed: %@", err]);

        id<MTLFunction> fAdd = [lib newFunctionWithName:@"op_add"];
        id<MTLFunction> fMul = [lib newFunctionWithName:@"op_mul"];
        id<MTLFunction> fXor = [lib newFunctionWithName:@"op_xor"];
        id<MTLFunction> fKernel = [lib newFunctionWithName:@"vft_compute"];
        id<MTLFunction> fRed = [lib newFunctionWithName:@"color_red"];
        id<MTLFunction> fGreen = [lib newFunctionWithName:@"color_green"];
        id<MTLFunction> fVs = [lib newFunctionWithName:@"vft_vs"];
        id<MTLFunction> fFs = [lib newFunctionWithName:@"vft_fs"];
        require(fAdd && fMul && fXor && fKernel && fRed && fGreen && fVs && fFs, @"VFT functions missing");

        // 1. Compute VFT: populate -> dispatch -> dynamic swap on SAME PSO -> null/OOB safety -> ARC lifetime.
        MTLComputePipelineDescriptor *cpd = [MTLComputePipelineDescriptor new];
        cpd.computeFunction = fKernel;
        MTLLinkedFunctions *clf = [MTLLinkedFunctions linkedFunctions];
        clf.functions = @[ fAdd, fMul, fXor ];
        cpd.linkedFunctions = clf;
        id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithDescriptor:cpd options:0 reflection:nil error:&err];
        require(cpso != nil, [NSString stringWithFormat:@"compute VFT PSO failed: %@", err]);

        id<MTLFunctionHandle> hAdd = [cpso functionHandleWithFunction:fAdd];
        id<MTLFunctionHandle> hMul = [cpso functionHandleWithFunction:fMul];
        id<MTLFunctionHandle> hXor = [cpso functionHandleWithFunction:fXor];
        require(hAdd && hMul && hXor, @"compute function handles nil");
        require(hAdd.device == dev && [hAdd.name isEqualToString:@"op_add"], @"hAdd properties mismatch");

        MTLVisibleFunctionTableDescriptor *cvd = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
        cvd.functionCount = 4;
        id<MTLVisibleFunctionTable> cvft = [cpso newVisibleFunctionTableWithDescriptor:cvd];
        require(cvft != nil, @"compute VFT creation failed");
        cvft.label = @"garnet.test.cvft";
        require([cvft.label isEqualToString:@"garnet.test.cvft"], @"compute VFT label mismatch");
        if (@available(macOS 13.0, *)) {
            if ([cvft respondsToSelector:@selector(gpuResourceID)]) {
                MTLResourceID rid = [cvft gpuResourceID];
                require(rid._impl != 0, @"compute VFT gpuResourceID must be non-zero");
            }
        }

        [cvft setFunction:hAdd atIndex:0];
        [cvft setFunction:hMul atIndex:1];
        [cvft setFunction:hXor atIndex:2];
        [cvft setFunction:nil atIndex:3];

        const uint32_t inVals[6] = { 10u, 11u, 12u, 13u, 14u, 15u };
        const uint32_t slotsGarnet[6] = { 0u, 1u, 2u, 3u, 4u, 99u }; // includes null slot (3) & OOB slots (4, 99)
        const uint32_t slotsRef[6] = { 0u, 1u, 2u, 0u, 1u, 2u };
        const uint32_t *slots = reference ? slotsRef : slotsGarnet;
        uint32_t outVals[6] = {0};

        runVftCompute(dev, q, cpso, cvft, inVals, slots, outVals, 6u, @"vft-compute-initial");
        require(outVals[0] == (10u + 100u), @"initial slot 0 (op_add) mismatch");
        require(outVals[1] == (11u * 7u), @"initial slot 1 (op_mul) mismatch");
        require(outVals[2] == (12u ^ 0x5A5Au), @"initial slot 2 (op_xor) mismatch");
        if (!reference) {
            require(outVals[3] == 0u, @"null slot 3 must safely return 0");
            require(outVals[4] == 0u, @"OOB slot 4 must safely return 0");
            require(outVals[5] == 0u, @"OOB slot 99 must safely return 0");
        }

        // Dynamic swap on the SAME compute PSO and SAME VFT via setFunctions:withRange:.
        const id<MTLFunctionHandle> __unsafe_unretained swapped[3] = { hXor, hAdd, hMul };
        [cvft setFunctions:swapped withRange:NSMakeRange(0, 3)];
        runVftCompute(dev, q, cpso, cvft, inVals, slots, outVals, 6u, @"vft-compute-swapped");
        require(outVals[0] == (10u ^ 0x5A5Au), @"swapped slot 0 (op_xor) mismatch");
        require(outVals[1] == (11u + 100u), @"swapped slot 1 (op_add) mismatch");
        require(outVals[2] == (12u * 7u), @"swapped slot 2 (op_mul) mismatch");

        // ARC early-release lifetime test: allocate VFT inside inner pool, encode into cb,
        // drop all strong refs before commit/wait so command buffer retention is exercised.
        {
            const NSUInteger bytes = 6u * sizeof(uint32_t);
            id<MTLBuffer> inBuf = [dev newBufferWithBytes:inVals length:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> idxBuf = [dev newBufferWithBytes:slots length:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> outBuf = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            require(inBuf && idxBuf && outBuf && outBuf.contents, @"vft-compute-arc: buffers nil");
            memset(outBuf.contents, 0xCC, bytes);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            @autoreleasepool {
                id<MTLVisibleFunctionTable> ephemeral = [cpso newVisibleFunctionTableWithDescriptor:cvd];
                require(ephemeral != nil, @"ephemeral VFT nil");
                [ephemeral setFunction:hMul atIndex:0];
                [ephemeral setFunction:hXor atIndex:1];
                [ephemeral setFunction:hAdd atIndex:2];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                require(enc != nil, @"vft-compute-arc: compute encoder nil");
                [enc setComputePipelineState:cpso];
                [enc setBuffer:inBuf offset:0 atIndex:0];
                [enc setBuffer:idxBuf offset:0 atIndex:1];
                [enc setBuffer:outBuf offset:0 atIndex:2];
                [enc setVisibleFunctionTable:ephemeral atBufferIndex:3];
                [enc dispatchThreads:MTLSizeMake(6u, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [enc endEncoding];
                ephemeral = nil;
            }
            finish(cb, @"vft-compute-arc");
            memcpy(outVals, outBuf.contents, bytes);
        }
        require(outVals[0] == (10u * 7u) && outVals[1] == (11u ^ 0x5A5Au) && outVals[2] == (12u + 100u),
                @"ephemeral VFT ARC lifetime output mismatch");

        // 2. Fragment VFT (MTLRenderStageFragment): red -> dynamic swap to green on SAME render PSO -> null/OOB guard.
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = fVs;
        rpd.fragmentFunction = fFs;
        MTLLinkedFunctions *flf = [MTLLinkedFunctions linkedFunctions];
        flf.functions = @[ fRed, fGreen ];
        rpd.fragmentLinkedFunctions = flf;
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        require(rpso != nil, [NSString stringWithFormat:@"render VFT PSO failed: %@", err]);

        id<MTLFunctionHandle> hRed = [rpso functionHandleWithFunction:fRed stage:MTLRenderStageFragment];
        id<MTLFunctionHandle> hGreen = [rpso functionHandleWithFunction:fGreen stage:MTLRenderStageFragment];
        require(hRed && hGreen, @"fragment function handles nil");

        MTLVisibleFunctionTableDescriptor *fvd = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
        fvd.functionCount = 2;
        id<MTLVisibleFunctionTable> fvft = [rpso newVisibleFunctionTableWithDescriptor:fvd stage:MTLRenderStageFragment];
        require(fvft != nil, @"fragment VFT creation failed");

        [fvft setFunction:hRed atIndex:0];
        [fvft setFunction:hGreen atIndex:1];

        #define VFT_TEST_W 16u
        #define VFT_TEST_H 16u
        uint8_t pixels[VFT_TEST_W * VFT_TEST_H * 4u];
        runVftRender(dev, q, rpso, fvft, 0u, pixels, VFT_TEST_W, VFT_TEST_H, @"vft-frag-red");
        verifySolidColor(pixels, VFT_TEST_W, VFT_TEST_H, 255, 0, 0, 255, @"vft-frag-red");

        // Dynamic swap on the SAME render PSO: slot 0 -> green, slot 1 -> nil.
        [fvft setFunction:hGreen atIndex:0];
        [fvft setFunction:nil atIndex:1];
        runVftRender(dev, q, rpso, fvft, 0u, pixels, VFT_TEST_W, VFT_TEST_H, @"vft-frag-green-swapped");
        verifySolidColor(pixels, VFT_TEST_W, VFT_TEST_H, 0, 255, 0, 255, @"vft-frag-green-swapped");

        if (!reference) {
            runVftRender(dev, q, rpso, fvft, 1u, pixels, VFT_TEST_W, VFT_TEST_H, @"vft-frag-null-slot");
            verifySolidColor(pixels, VFT_TEST_W, VFT_TEST_H, 0, 0, 0, 0, @"vft-frag-null-slot");

            runVftRender(dev, q, rpso, fvft, 7u, pixels, VFT_TEST_W, VFT_TEST_H, @"vft-frag-oob-slot");
            verifySolidColor(pixels, VFT_TEST_W, VFT_TEST_H, 0, 0, 0, 0, @"vft-frag-oob-slot");
        }

        // 3. Footnote 10 mutual exclusion: mesh pipeline + render function pointers must be rejected.
        BOOL fn10Checked = NO;
        Class meshDescCls = NSClassFromString(@"MTLMeshRenderPipelineDescriptor");
        SEL meshPsoSel = sel_registerName("newRenderPipelineStateWithMeshDescriptor:options:reflection:error:");
        if (meshDescCls && [dev respondsToSelector:meshPsoSel]) {
            id mdesc = [[meshDescCls alloc] init];
            SEL setFragFn = sel_registerName("setFragmentFunction:");
            SEL setFragLink = sel_registerName("setFragmentLinkedFunctions:");
            if ([mdesc respondsToSelector:setFragFn] && [mdesc respondsToSelector:setFragLink]) {
                ((void (*)(id, SEL, id))objc_msgSend)(mdesc, setFragFn, fFs);
                ((void (*)(id, SEL, id))objc_msgSend)(mdesc, setFragLink, flf);
                NSError * __autoreleasing me = nil;
                void *rawMeshPso = ((void *(*)(id, SEL, id, NSUInteger, id __autoreleasing *, NSError * __autoreleasing *))objc_msgSend)(
                    dev, meshPsoSel, mdesc, 0, NULL, &me);
                id badMeshPso = rawMeshPso ? CFBridgingRelease(rawMeshPso) : nil;
                require(badMeshPso == nil && me != nil,
                        [NSString stringWithFormat:@"Footnote 10: mesh + fragmentLinkedFunctions must fail, got pso=%@ err=%@",
                         badMeshPso, me]);
                fn10Checked = YES;
            }
        }

        printf("METAL3_VFT_PASS mode=%s computeSwap=ok fragSwap=ok oobGuard=%s footnote10=%s\n",
               reference ? "reference" : "garnet",
               reference ? "n/a" : "ok",
               fn10Checked ? "ok" : "sdk-unavailable");
        _exit(0);
    }
}
