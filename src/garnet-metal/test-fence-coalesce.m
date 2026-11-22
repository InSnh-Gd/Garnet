#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;

static void check(int ok, const char *name, const char *detail) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, detail ? ": " : "", detail ? detail : "");
    if (!ok) fails++;
}

static NSUInteger segment_count(id<MTLCommandBuffer> cb, BOOL *foundOut) {
    Ivar iv = class_getInstanceVariable([cb class], "_segments");
    if (!iv) {
        if (foundOut) *foundOut = NO;
        return 0;
    }
    if (foundOut) *foundOut = YES;
    id segs = object_getIvar(cb, iv);
    return [segs isKindOfClass:[NSArray class]] ? [(NSArray *)segs count] : 0;
}

static id<MTLTexture> make_iosurface_tex(id<MTLDevice> dev, NSUInteger w, NSUInteger h) {
    NSDictionary *props = @{
        (__bridge NSString *)kIOSurfaceWidth: @(w),
        (__bridge NSString *)kIOSurfaceHeight: @(h),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(w * 4),
        (__bridge NSString *)kIOSurfacePixelFormat: @((uint32_t)'BGRA')
    };
    IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surf) return nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
    CFRelease(surf);
    return tex;
}

static id<MTLRenderPipelineState> make_pso(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLFunction> vfn, NSString *fname) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vfn;
    d.fragmentFunction = [lib newFunctionWithName:fname];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso || err) {
        fprintf(stderr, "pipeline %s failed: %s\n", [fname UTF8String], err ? [[err localizedDescription] UTF8String] : "nil");
    }
    return pso;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        const char *bundlePath = argc > 1 ? argv[1] : "/Library/GPUBundles/GarnetMetal.bundle";
        setenv("RDNA4M_ALLOW", "1", 1);

        NSError *err = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundlePath]];
        if (!bundle || ![bundle loadAndReturnError:&err]) {
            fprintf(stderr, "FAIL load bundle %s: %s\n", bundlePath, err ? [[err description] UTF8String] : "nil");
            return 2;
        }
        Class devCls = objc_getClass("GarnetDevice");
        id<MTLDevice> dev = devCls ? [[devCls alloc] init] : nil;
        if (!dev) {
            fprintf(stderr, "FAIL GarnetDevice alloc/init returned nil\n");
            return 2;
        }

        NSString *msl =
            @"#include <metal_stdlib>\n"
            @"using namespace metal;\n"
            @"struct VOut { float4 pos [[position]]; };\n"
            @"vertex VOut vtx_quad(uint vid [[vertex_id]]) {\n"
            @"    const float2 p[6] = {\n"
            @"        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),\n"
            @"        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)\n"
            @"    };\n"
            @"    VOut o; o.pos = float4(p[vid], 0.0, 1.0); return o;\n"
            @"}\n"
            @"fragment float4 frag_seed_r() { return float4(0.125, 0.0, 0.0, 1.0); }\n"
            @"fragment float4 frag_add_r(texture2d<float> src [[texture(0)]]) {\n"
            @"    constexpr sampler s(coord::normalized, filter::nearest);\n"
            @"    float4 c = src.sample(s, float2(0.5, 0.5));\n"
            @"    return float4(c.r + 0.125, c.g, c.b, 1.0);\n"
            @"}\n"
            @"fragment float4 frag_seed_g() { return float4(0.0, 1.0, 0.0, 1.0); }\n"
            @"fragment float4 frag_add_b(texture2d<float> src [[texture(0)]]) {\n"
            @"    constexpr sampler s(coord::normalized, filter::nearest);\n"
            @"    float4 c = src.sample(s, float2(0.5, 0.5));\n"
            @"    return float4(c.r, c.g, c.b + 1.0, 1.0);\n"
            @"}\n";

        err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        check(lib != nil && err == nil, "compile MSL library", err ? [[err localizedDescription] UTF8String] : "ok");
        if (!lib) return 1;

        id<MTLFunction> vQuad = [lib newFunctionWithName:@"vtx_quad"];
        id<MTLRenderPipelineState> psoSeedR = make_pso(dev, lib, vQuad, @"frag_seed_r");
        id<MTLRenderPipelineState> psoAddR = make_pso(dev, lib, vQuad, @"frag_add_r");
        id<MTLRenderPipelineState> psoSeedG = make_pso(dev, lib, vQuad, @"frag_seed_g");
        id<MTLRenderPipelineState> psoAddB = make_pso(dev, lib, vQuad, @"frag_add_b");
        check(psoSeedR && psoAddR && psoSeedG && psoAddB, "create pipelines", "4 pipelines");
        if (!psoSeedR || !psoAddR || !psoSeedG || !psoAddB) return 1;

        const NSUInteger W = 64, H = 64, BPR = W * 4;
        id<MTLTexture> tex0 = make_iosurface_tex(dev, W, H);
        id<MTLTexture> tex1 = make_iosurface_tex(dev, W, H);
        id<MTLBuffer> readBuf = [dev newBufferWithLength:BPR * H options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        check(tex0 && tex1 && readBuf && q, "create IOSurface textures & queue", "64x64 BGRA8");
        if (!tex0 || !tex1 || !readBuf || !q) return 1;

        id<MTLFence> f[8];
        for (int i = 0; i < 8; i++) f[i] = [dev newFence];
        check(f[0] != nil && f[7] != nil, "create 8 MTLFences", "ok");

        // 1. 8 chained render/blit passes in 1 CB updating/waiting on fences -> must coalesce into 1 segment
        id<MTLCommandBuffer> cb1 = [q commandBuffer];
        for (int pass = 0; pass < 8; pass++) {
            if ((pass & 1) == 0) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = tex0;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                id<MTLRenderCommandEncoder> enc = [cb1 renderCommandEncoderWithDescriptor:rp];
                if (pass > 0) [enc waitForFence:f[pass - 1] beforeStages:MTLRenderStageFragment];
                [enc setRenderPipelineState:(pass == 0) ? psoSeedR : psoAddR];
                if (pass > 0) [enc setFragmentTexture:tex1 atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [enc updateFence:f[pass] afterStages:MTLRenderStageFragment];
                [enc endEncoding];
            } else {
                id<MTLBlitCommandEncoder> blit = [cb1 blitCommandEncoder];
                [blit waitForFence:f[pass - 1]];
                [blit copyFromTexture:tex0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(W, H, 1) toTexture:tex1 destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                if (pass == 7) {
                    [blit copyFromTexture:tex1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(W, H, 1) toBuffer:readBuf destinationOffset:0
                      destinationBytesPerRow:BPR destinationBytesPerImage:BPR * H];
                }
                [blit updateFence:f[pass]];
                [blit endEncoding];
            }
        }

        BOOL hasSegIvar = NO;
        NSUInteger segCount1 = segment_count(cb1, &hasSegIvar);
        char msg[128];
        snprintf(msg, sizeof(msg), "hasIvar=%d _segments.count=%lu", hasSegIvar ? 1 : 0, (unsigned long)segCount1);
        check(hasSegIvar && segCount1 == 1, "8 intra-CB fence passes coalesce into 1 segment", msg);

        [cb1 commit];
        [cb1 waitUntilCompleted];
        check(cb1.status == MTLCommandBufferStatusCompleted && cb1.error == nil,
              "cb1 8-pass fence chain completed",
              cb1.error ? [[cb1.error localizedDescription] UTF8String] : "completed");

        const uint32_t *px1 = (const uint32_t *)readBuf.contents;
        uint32_t v1 = px1[32 * W + 32];
        uint8_t b1 = (uint8_t)(v1 & 0xFF), g1 = (uint8_t)((v1 >> 8) & 0xFF),
                r1 = (uint8_t)((v1 >> 16) & 0xFF), a1 = (uint8_t)((v1 >> 24) & 0xFF);
        snprintf(msg, sizeof(msg), "RGBA=(%u,%u,%u,%u) expect (~128,0,0,255)", r1, g1, b1, a1);
        check(r1 >= 120 && r1 <= 136 && g1 == 0 && b1 == 0 && a1 == 255, "cb1 pixel output preserves pass order", msg);

        // 2. Waiting on an already-Ready fence in a fresh CB also coalesces into 1 segment
        id<MTLCommandBuffer> cbReady = [q commandBuffer];
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex0;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> enc = [cbReady renderCommandEncoderWithDescriptor:rp];
            [enc waitForFence:f[7] beforeStages:MTLRenderStageFragment];
            [enc setRenderPipelineState:psoAddR];
            [enc setFragmentTexture:tex1 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [enc endEncoding];

            id<MTLBlitCommandEncoder> blit = [cbReady blitCommandEncoder];
            [blit copyFromTexture:tex0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(W, H, 1) toBuffer:readBuf destinationOffset:0
              destinationBytesPerRow:BPR destinationBytesPerImage:BPR * H];
            [blit endEncoding];
        }
        NSUInteger segReady = segment_count(cbReady, &hasSegIvar);
        snprintf(msg, sizeof(msg), "_segments.count=%lu", (unsigned long)segReady);
        check(hasSegIvar && segReady == 1, "ready-fence wait coalesces into 1 segment", msg);

        [cbReady commit];
        [cbReady waitUntilCompleted];
        uint32_t vR = ((const uint32_t *)readBuf.contents)[32 * W + 32];
        uint8_t bR = (uint8_t)(vR & 0xFF), gR = (uint8_t)((vR >> 8) & 0xFF),
                rR = (uint8_t)((vR >> 16) & 0xFF), aR = (uint8_t)((vR >> 24) & 0xFF);
        snprintf(msg, sizeof(msg), "RGBA=(%u,%u,%u,%u) expect (~159,0,0,255)", rR, gR, bR, aR);
        check(cbReady.status == MTLCommandBufferStatusCompleted && rR >= 150 && rR <= 168 && gR == 0 && bR == 0 && aR == 255,
              "cbReady readback output matches", msg);

        // 3. Cross-CB MTLFence ordering across 2 in-flight command buffers (commit A, then B without CPU wait)
        id<MTLFence> cross = [dev newFence];
        id<MTLCommandBuffer> cbA = [q commandBuffer];
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex0;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> enc = [cbA renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:psoSeedG];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [enc updateFence:cross afterStages:MTLRenderStageFragment];
            [enc endEncoding];
        }

        id<MTLCommandBuffer> cbB = [q commandBuffer];
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex1;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> enc = [cbB renderCommandEncoderWithDescriptor:rp];
            [enc waitForFence:cross beforeStages:MTLRenderStageFragment];
            [enc setRenderPipelineState:psoAddB];
            [enc setFragmentTexture:tex0 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [enc endEncoding];

            id<MTLBlitCommandEncoder> blit = [cbB blitCommandEncoder];
            [blit copyFromTexture:tex1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(W, H, 1) toBuffer:readBuf destinationOffset:0
              destinationBytesPerRow:BPR destinationBytesPerImage:BPR * H];
            [blit endEncoding];
        }

        [cbA commit];
        [cbB commit];
        [cbA waitUntilCompleted];
        [cbB waitUntilCompleted];
        check(cbA.status == MTLCommandBufferStatusCompleted && cbB.status == MTLCommandBufferStatusCompleted && !cbA.error && !cbB.error,
              "cross-CB fence CBs completed", "ok");

        uint32_t vC = ((const uint32_t *)readBuf.contents)[32 * W + 32];
        uint8_t bC = (uint8_t)(vC & 0xFF), gC = (uint8_t)((vC >> 8) & 0xFF),
                rC = (uint8_t)((vC >> 16) & 0xFF), aC = (uint8_t)((vC >> 24) & 0xFF);
        snprintf(msg, sizeof(msg), "RGBA=(%u,%u,%u,%u) expect (0,255,255,255)", rC, gC, bC, aC);
        check(rC == 0 && gC == 255 && bC == 255 && aC == 255, "cross-CB fence ordering produces cyan", msg);
    }
    return fails ? 1 : 0;
}

