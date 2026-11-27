#import <Foundation/Foundation.h>
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

static uint32_t expected_bgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
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
            @"vertex VOut vtx_left(uint vid [[vertex_id]]) {\n"
            @"    const float2 p[6] = {\n"
            @"        float2(-1.0, -1.0), float2(0.0, -1.0), float2(-1.0,  1.0),\n"
            @"        float2(-1.0,  1.0), float2(0.0, -1.0), float2( 0.0,  1.0)\n"
            @"    };\n"
            @"    VOut o; o.pos = float4(p[vid], 0.0, 1.0); return o;\n"
            @"}\n"
            @"vertex VOut vtx_full(uint vid [[vertex_id]]) {\n"
            @"    const float2 p[6] = {\n"
            @"        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),\n"
            @"        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)\n"
            @"    };\n"
            @"    VOut o; o.pos = float4(p[vid], 0.0, 1.0); return o;\n"
            @"}\n"
            @"fragment float4 frag_green() { return float4(0.0, 1.0, 0.0, 1.0); }\n";

        err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        check(lib != nil && err == nil, "compile MSL library", err ? [[err localizedDescription] UTF8String] : "ok");
        if (!lib) return 1;

        id<MTLFunction> vLeft = [lib newFunctionWithName:@"vtx_left"];
        id<MTLFunction> vFull = [lib newFunctionWithName:@"vtx_full"];
        id<MTLFunction> fGreen = [lib newFunctionWithName:@"frag_green"];
        check(vLeft && vFull && fGreen, "load MSL functions", "vtx_left/vtx_full/frag_green");
        if (!vLeft || !vFull || !fGreen) return 1;

        // 1. Stencil-only vertex-only pipeline (fragmentFunction = nil, Invalid color) + reflection
        MTLRenderPipelineDescriptor *dStencilOnly = [MTLRenderPipelineDescriptor new];
        dStencilOnly.vertexFunction = vLeft;
        dStencilOnly.fragmentFunction = nil;
        dStencilOnly.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;

        err = nil;
        id<MTLRenderPipelineState> psoStencilOnly = [dev newRenderPipelineStateWithDescriptor:dStencilOnly error:&err];
        check(psoStencilOnly != nil && err == nil,
              "vertex-only stencil pipeline (no color)",
              err ? [[err localizedDescription] UTF8String] : "created");

        err = nil;
        MTLRenderPipelineReflection *reflStencilOnly = nil;
        id<MTLRenderPipelineState> psoStencilOnlyRefl =
            [dev newRenderPipelineStateWithDescriptor:dStencilOnly
                                              options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                           reflection:&reflStencilOnly
                                                error:&err];
        check(psoStencilOnlyRefl != nil && reflStencilOnly != nil && err == nil,
              "vertex-only stencil pipeline with reflection (no color)",
              err ? [[err localizedDescription] UTF8String] : "created");

        // 2. RenderBox-style BGRA8 + Stencil8 vertex-only pipeline with default writeMask = All
        MTLRenderPipelineDescriptor *dRB = [MTLRenderPipelineDescriptor new];
        dRB.vertexFunction = vLeft;
        dRB.fragmentFunction = nil;
        dRB.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        dRB.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
        dRB.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;

        err = nil;
        id<MTLRenderPipelineState> psoRB = [dev newRenderPipelineStateWithDescriptor:dRB error:&err];
        check(psoRB != nil && err == nil,
              "vertex-only stencil pipeline (BGRA8 + writeMask=All)",
              err ? [[err localizedDescription] UTF8String] : "created");

        err = nil;
        MTLRenderPipelineReflection *reflRB = nil;
        id<MTLRenderPipelineState> psoRBRefl =
            [dev newRenderPipelineStateWithDescriptor:dRB
                                              options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                           reflection:&reflRB
                                                error:&err];
        check(psoRBRefl != nil && reflRB != nil && err == nil,
              "vertex-only stencil pipeline with reflection (BGRA8 + Stencil8)",
              err ? [[err localizedDescription] UTF8String] : "created");

        // Explicit writeMask = None variant
        MTLRenderPipelineDescriptor *dRBNone = [dRB copy];
        dRBNone.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        err = nil;
        id<MTLRenderPipelineState> psoRBNone = [dev newRenderPipelineStateWithDescriptor:dRBNone error:&err];
        check(psoRBNone != nil && err == nil,
              "vertex-only stencil pipeline (BGRA8 + writeMask=None)",
              err ? [[err localizedDescription] UTF8String] : "created");

        // 3. rasterizationEnabled = NO + fragmentFunction = nil
        MTLRenderPipelineDescriptor *dRastOff = [dRB copy];
        dRastOff.rasterizationEnabled = NO;
        err = nil;
        id<MTLRenderPipelineState> psoRastOff = [dev newRenderPipelineStateWithDescriptor:dRastOff error:&err];
        check(psoRastOff != nil && err == nil,
              "vertex-only pipeline (rasterizationEnabled=NO)",
              err ? [[err localizedDescription] UTF8String] : "created");

        // Draw pipeline with fragment shader for stencil verification pass
        MTLRenderPipelineDescriptor *dDraw = [MTLRenderPipelineDescriptor new];
        dDraw.vertexFunction = vFull;
        dDraw.fragmentFunction = fGreen;
        dDraw.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        dDraw.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
        err = nil;
        id<MTLRenderPipelineState> psoDraw = [dev newRenderPipelineStateWithDescriptor:dDraw error:&err];
        check(psoDraw != nil && err == nil, "stencil-tested draw pipeline",
              err ? [[err localizedDescription] UTF8String] : "created");

        if (!psoRB || !psoDraw) return 1;

        // DepthStencil states
        MTLDepthStencilDescriptor *dsWriteDesc = [MTLDepthStencilDescriptor new];
        MTLStencilDescriptor *stWrite = [MTLStencilDescriptor new];
        stWrite.stencilCompareFunction = MTLCompareFunctionAlways;
        stWrite.stencilFailureOperation = MTLStencilOperationKeep;
        stWrite.depthFailureOperation = MTLStencilOperationKeep;
        stWrite.depthStencilPassOperation = MTLStencilOperationReplace;
        stWrite.readMask = 0xFF;
        stWrite.writeMask = 0xFF;
        dsWriteDesc.frontFaceStencil = stWrite;
        dsWriteDesc.backFaceStencil = stWrite;
        id<MTLDepthStencilState> dsWrite = [dev newDepthStencilStateWithDescriptor:dsWriteDesc];

        MTLDepthStencilDescriptor *dsEqDesc = [MTLDepthStencilDescriptor new];
        MTLStencilDescriptor *stEq = [MTLStencilDescriptor new];
        stEq.stencilCompareFunction = MTLCompareFunctionEqual;
        stEq.stencilFailureOperation = MTLStencilOperationKeep;
        stEq.depthFailureOperation = MTLStencilOperationKeep;
        stEq.depthStencilPassOperation = MTLStencilOperationKeep;
        stEq.readMask = 0xFF;
        stEq.writeMask = 0x00;
        dsEqDesc.frontFaceStencil = stEq;
        dsEqDesc.backFaceStencil = stEq;
        id<MTLDepthStencilState> dsEq = [dev newDepthStencilStateWithDescriptor:dsEqDesc];

        // Textures & readback buffer (64x64)
        const NSUInteger W = 64, H = 64, BPR = W * 4;
        MTLTextureDescriptor *cDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        cDesc.storageMode = MTLStorageModePrivate;
        cDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> colorTex = [dev newTextureWithDescriptor:cDesc];

        MTLTextureDescriptor *sDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:W height:H mipmapped:NO];
        sDesc.storageMode = MTLStorageModePrivate;
        sDesc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> stencilTex = [dev newTextureWithDescriptor:sDesc];

        id<MTLBuffer> readBuf = [dev newBufferWithLength:BPR * H options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        check(colorTex && stencilTex && readBuf && q && dsWrite && dsEq, "create GPU resources", "64x64 color+stencil+readBuf");
        if (!colorTex || !stencilTex || !readBuf || !q || !dsWrite || !dsEq) return 1;

        const uint32_t kRed = expected_bgra(255, 0, 0, 255);
        const uint32_t kGreen = expected_bgra(0, 255, 0, 255);

        // Pass 1: Clear color=red, stencil=0, run vertex-only stencil fill on left half.
        // Verify color remains red across both halves (wm=0 suppressed color writes).
        id<MTLCommandBuffer> cb1 = [q commandBuffer];
        MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
        rp1.colorAttachments[0].texture = colorTex;
        rp1.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp1.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp1.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
        rp1.stencilAttachment.texture = stencilTex;
        rp1.stencilAttachment.loadAction = MTLLoadActionClear;
        rp1.stencilAttachment.storeAction = MTLStoreActionStore;
        rp1.stencilAttachment.clearStencil = 0;

        id<MTLRenderCommandEncoder> enc1 = [cb1 renderCommandEncoderWithDescriptor:rp1];
        [enc1 setRenderPipelineState:psoRB];
        [enc1 setDepthStencilState:dsWrite];
        [enc1 setStencilReferenceValue:1];
        [enc1 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc1 endEncoding];

        id<MTLBlitCommandEncoder> blit1 = [cb1 blitCommandEncoder];
        [blit1 copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(W, H, 1) toBuffer:readBuf destinationOffset:0
           destinationBytesPerRow:BPR destinationBytesPerImage:BPR * H];
        [blit1 endEncoding];
        [cb1 commit];
        [cb1 waitUntilCompleted];

        check(cb1.status == MTLCommandBufferStatusCompleted && cb1.error == nil,
              "pass1 vertex-only stencil fill completed",
              cb1.error ? [[cb1.error localizedDescription] UTF8String] : "completed");

        const uint32_t *px1 = (const uint32_t *)readBuf.contents;
        uint32_t p1Left = px1[32 * W + 8];
        uint32_t p1Right = px1[32 * W + 48];
        char msg[128];
        snprintf(msg, sizeof(msg), "left=0x%08x right=0x%08x expect=0x%08x", p1Left, p1Right, kRed);
        check(p1Left == kRed && p1Right == kRed, "vertex-only pipeline preserves color target (wm=0)", msg);

        // Pass 2: Load color & stencil, draw full green quad where stencil == 1.
        id<MTLCommandBuffer> cb2 = [q commandBuffer];
        MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
        rp2.colorAttachments[0].texture = colorTex;
        rp2.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp2.stencilAttachment.texture = stencilTex;
        rp2.stencilAttachment.loadAction = MTLLoadActionLoad;
        rp2.stencilAttachment.storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc2 = [cb2 renderCommandEncoderWithDescriptor:rp2];
        [enc2 setRenderPipelineState:psoDraw];
        [enc2 setDepthStencilState:dsEq];
        [enc2 setStencilReferenceValue:1];
        [enc2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc2 endEncoding];

        id<MTLBlitCommandEncoder> blit2 = [cb2 blitCommandEncoder];
        [blit2 copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(W, H, 1) toBuffer:readBuf destinationOffset:0
           destinationBytesPerRow:BPR destinationBytesPerImage:BPR * H];
        [blit2 endEncoding];
        [cb2 commit];
        [cb2 waitUntilCompleted];

        check(cb2.status == MTLCommandBufferStatusCompleted && cb2.error == nil,
              "pass2 stencil-tested draw completed",
              cb2.error ? [[cb2.error localizedDescription] UTF8String] : "completed");

        const uint32_t *px2 = (const uint32_t *)readBuf.contents;
        uint32_t p2Left = px2[32 * W + 8];
        uint32_t p2Right = px2[32 * W + 48];
        snprintf(msg, sizeof(msg), "left=0x%08x (expect green 0x%08x) right=0x%08x (expect red 0x%08x)",
                 p2Left, kGreen, p2Right, kRed);
        check(p2Left == kGreen && p2Right == kRed, "stencil mask from vertex-only pipeline clips fragment draw", msg);
    }
    return fails ? 1 : 0;
}
