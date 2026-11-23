#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    float x, y;
} Float2;

typedef struct __attribute__((packed)) {
    uint8_t r, g, b, a;
} RGBA8;

static void require(BOOL ok, NSString *msg) {
    if (!ok) {
        fprintf(stderr, "TESSELLATION_FAIL: %s\n", msg.UTF8String);
        exit(1);
    }
}

static uint16_t f32_to_f16(float f) {
    __fp16 h = (__fp16)f;
    uint16_t bits = 0;
    memcpy(&bits, &h, 2);
    return bits;
}

static id<MTLDevice> load_garnet_device(const char *arg) {
    if (!strcmp(arg, "--reference")) return MTLCreateSystemDefaultDevice();
    setenv("RDNA4M_ALLOW", "1", 1);
    setenv("RDNA4M_TEST_INPROC", "1", 1);
    NSError *err = nil;
    require([[NSBundle bundleWithPath:@(arg)] loadAndReturnError:&err],
            err.localizedDescription ?: @"bundle load failed");
    Class cls = objc_getClass("GarnetDevice");
    require(cls != Nil, @"GarnetDevice missing");
    id dev = [[cls alloc] init];
    if (dev) ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
    return dev;
}

static id<MTLTexture> make_rt(id<MTLDevice> dev, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

static NSData *readback_rt(id<MTLCommandQueue> q, id<MTLTexture> rt) {
    NSUInteger w = rt.width, h = rt.height, bpr = w * 4;
    id<MTLBuffer> dst = [rt.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(w, h, 1) toBuffer:dst destinationOffset:0
       destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted, cb.error.localizedDescription ?: @"readback blit failed");
    return [NSData dataWithBytes:dst.contents length:bpr * h];
}

static RGBA8 pixel_at(NSData *img, NSUInteger w, NSUInteger x, NSUInteger y) {
    const RGBA8 *p = (const RGBA8 *)img.bytes;
    return p[y * w + x];
}

static NSUInteger count_non_clear(NSData *img, RGBA8 clr) {
    const RGBA8 *p = (const RGBA8 *)img.bytes;
    NSUInteger n = img.length / 4, hit = 0;
    for (NSUInteger i = 0; i < n; i++)
        if (p[i].r != clr.r || p[i].g != clr.g || p[i].b != clr.b || p[i].a != clr.a) hit++;
    return hit;
}

static id<MTLRenderPipelineState> make_tess_pso(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *vertName,
                                                NSString *fragName, MTLVertexDescriptor *vd,
                                                void (^cfg)(MTLRenderPipelineDescriptor *d), NSError **outErr) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vertName];
    d.fragmentFunction = [lib newFunctionWithName:fragName];
    require(d.vertexFunction != nil && d.fragmentFunction != nil, @"missing shader function");
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.vertexDescriptor = vd;
    if (cfg) cfg(d);
    return [dev newRenderPipelineStateWithDescriptor:d error:outErr];
}

static const char *kTessMSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct FragIn { float4 pos [[position]]; float4 col; };\n"
"\n"
"struct TriCPIn { float2 xy [[attribute(0)]]; float4 tint [[attribute(1)]]; };\n"
"struct TriPatchIn {\n"
"    patch_control_point<TriCPIn> cp;\n"
"    float4 perPatch [[attribute(2)]];\n"
"    float4 constBias [[attribute(3)]];\n"
"};\n"
"[[patch(triangle, 3)]]\n"
"vertex FragIn tri_tes(TriPatchIn in [[stage_in]],\n"
"                      float3 bary [[position_in_patch]],\n"
"                      uint pid [[patch_id]],\n"
"                      uint iid [[instance_id]]) {\n"
"    FragIn o;\n"
"    float2 p = bary.x * in.cp[0].xy + bary.y * in.cp[1].xy + bary.z * in.cp[2].xy;\n"
"    float4 ct = bary.x * in.cp[0].tint + bary.y * in.cp[1].tint + bary.z * in.cp[2].tint;\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.col = float4(saturate(ct.r + in.constBias.r + float(pid) * 0.2),\n"
"                   saturate(in.perPatch.g + float(iid) * 0.15),\n"
"                   bary.z, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"struct QuadCPIn { float2 xy [[attribute(0)]]; };\n"
"struct QuadPatchIn { patch_control_point<QuadCPIn> cp; };\n"
"[[patch(quad, 4)]]\n"
"vertex FragIn quad_wave_tes(QuadPatchIn in [[stage_in]], float2 uv [[position_in_patch]]) {\n"
"    FragIn o;\n"
"    float2 top = mix(in.cp[2].xy, in.cp[3].xy, uv.x);\n"
"    float2 bot = mix(in.cp[0].xy, in.cp[1].xy, uv.x);\n"
"    float2 p = mix(bot, top, uv.y);\n"
"    float wave = 4.0 * uv.x * (1.0 - uv.x);\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.col = float4(wave, uv.y, 1.0 - uv.x, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"[[patch(triangle, 0)]]\n"
"vertex FragIn zero_cp_tes(float3 bary [[position_in_patch]], uint pid [[patch_id]], uint iid [[instance_id]]) {\n"
"    FragIn o;\n"
"    float2 p0 = float2(-0.8, -0.8), p1 = float2(0.8, -0.8), p2 = float2(0.0, 0.8);\n"
"    float2 p = bary.x * p0 + bary.y * p1 + bary.z * p2;\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.col = float4(0.2 + 0.3 * float(pid), 0.1 + 0.25 * float(iid), 0.75, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"vertex FragIn plain_vs(uint vid [[vertex_id]]) {\n"
"    FragIn o; o.pos = float4(0, 0, 0, 1); o.col = float4(1); return o;\n"
"}\n"
"\n"
"fragment float4 pass_fs(FragIn in [[stage_in]]) { return in.col; }\n"
"\n"
"kernel void gen_factors_and_indirect(device half *factors [[buffer(0)]],\n"
"                                     device uint *indirect [[buffer(1)]],\n"
"                                     constant float &edgeVal [[buffer(2)]],\n"
"                                     constant uint &patchStrideHalves [[buffer(3)]]) {\n"
"    half e = half(edgeVal);\n"
"    // patch slot 0: culled (0), patch slot 1: edgeVal\n"
"    for (uint i = 0; i < patchStrideHalves; i++) factors[i] = half(0.0);\n"
"    for (uint i = 0; i < patchStrideHalves; i++) factors[patchStrideHalves + i] = e;\n"
"    indirect[0] = 1u; // patchCount\n"
"    indirect[1] = 1u; // instanceCount\n"
"    indirect[2] = 1u; // patchStart\n"
"    indirect[3] = 2u; // baseInstance\n"
"}\n";

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-tessellation BUNDLE|--reference");
            return 0;
        }
        if (argc != 2) return 2;
        id<MTLDevice> dev = load_garnet_device(argv[1]);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kTessMSL) options:nil error:&err];
        require(lib != nil, err.localizedDescription ?: @"library compile failed");
        id<MTLTexture> rt = make_rt(dev, 64, 64);

        // Shared quad corners [-1..1] for patch 0 and patch 1
        Float2 quadVerts[8] = {
            {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
            {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
        };
        id<MTLBuffer> quadVB = [dev newBufferWithBytes:quadVerts length:sizeof quadVerts options:MTLResourceStorageModeShared];
        MTLVertexDescriptor *quadVD = [MTLVertexDescriptor vertexDescriptor];
        quadVD.attributes[0].format = MTLVertexFormatFloat2; quadVD.attributes[0].offset = 0; quadVD.attributes[0].bufferIndex = 0;
        quadVD.layouts[0].stride = sizeof(Float2);
        quadVD.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
        quadVD.layouts[0].stepRate = 1;

        // Triangle layout with PerPatchControlPoint(0,1), PerPatch(2), Constant(3)
        MTLVertexDescriptor *triVD = [MTLVertexDescriptor vertexDescriptor];
        triVD.attributes[0].format = MTLVertexFormatFloat2; triVD.attributes[0].offset = 0; triVD.attributes[0].bufferIndex = 0;
        triVD.attributes[1].format = MTLVertexFormatUChar4Normalized; triVD.attributes[1].offset = 8; triVD.attributes[1].bufferIndex = 0;
        triVD.layouts[0].stride = 12; triVD.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint; triVD.layouts[0].stepRate = 1;
        triVD.attributes[2].format = MTLVertexFormatFloat4; triVD.attributes[2].offset = 0; triVD.attributes[2].bufferIndex = 1;
        triVD.layouts[1].stride = 16; triVD.layouts[1].stepFunction = MTLVertexStepFunctionPerPatch; triVD.layouts[1].stepRate = 1;
        triVD.attributes[3].format = MTLVertexFormatFloat4; triVD.attributes[3].offset = 0; triVD.attributes[3].bufferIndex = 2;
        triVD.layouts[2].stride = 16; triVD.layouts[2].stepFunction = MTLVertexStepFunctionConstant; triVD.layouts[2].stepRate = 0;

        struct __attribute__((packed)) TriVert { float x, y; uint8_t r, g, b, a; };
        // 4 patches * 3 control points = 12 verts (plus extra for indexed reuse)
        struct TriVert triCP[12];
        memset(triCP, 0, sizeof triCP);
        // Patch 0: left half
        triCP[0] = (struct TriVert){ -0.9f, -0.9f, 25, 0, 0, 255 };
        triCP[1] = (struct TriVert){  0.0f, -0.9f, 25, 0, 0, 255 };
        triCP[2] = (struct TriVert){ -0.45f, 0.9f, 25, 0, 0, 255 };
        // Patch 3 (verts 9..11): full screen centered triangle so pixel (32, 32) is definitely inside
        triCP[9]  = (struct TriVert){ -1.0f, -1.0f, 51, 0, 0, 255 };
        triCP[10] = (struct TriVert){  1.0f, -1.0f, 51, 0, 0, 255 };
        triCP[11] = (struct TriVert){  0.0f,  1.0f, 51, 0, 0, 255 };
        float perPatchData[4 * 4] = {
            0, 0.20f, 0, 1,
            0, 0.30f, 0, 1,
            0, 0.40f, 0, 1,
            0, 0.50f, 0, 1,
        };
        float constBiasData[4] = { 0.10f, 0.0f, 0.0f, 0.0f };
        id<MTLBuffer> triVB0 = [dev newBufferWithBytes:triCP length:sizeof triCP options:MTLResourceStorageModeShared];
        id<MTLBuffer> triVB1 = [dev newBufferWithBytes:perPatchData length:sizeof perPatchData options:MTLResourceStorageModeShared];
        id<MTLBuffer> triVB2 = [dev newBufferWithBytes:constBiasData length:sizeof constBiasData options:MTLResourceStorageModeShared];

        NSData *(^run_pass)(void (^)(id<MTLRenderCommandEncoder>)) = ^NSData *(void (^encode)(id<MTLRenderCommandEncoder> enc)) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            encode(enc);
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            require(cb.status == MTLCommandBufferStatusCompleted, cb.error.localizedDescription ?: @"render CB failed");
            return readback_rt(q, rt);
        };

        // ---- 1. Triangle patch direct draw + patch_id + instance_id + Constant/PerPatch/PerPatchCP ----
        {
            id<MTLRenderPipelineState> pso = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                d.maxTessellationFactor = 16;
            }, &err);
            require(pso != nil, err.localizedDescription ?: @"tri pso failed");
            MTLTriangleTessellationFactorsHalf tf[4];
            for (int i = 0; i < 4; i++) tf[i] = (MTLTriangleTessellationFactorsHalf){ { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, f32_to_f16(2) };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:tf length:sizeof tf options:MTLResourceStorageModeShared];
            NSData *img = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:pso];
                [enc setVertexBuffer:triVB0 offset:0 atIndex:0];
                [enc setVertexBuffer:triVB1 offset:0 atIndex:1];
                [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:3 patchStart:3 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:2];
            });
            RGBA8 p = pixel_at(img, 64, 32, 32);
            // Expect R = (51/255=0.2) + 0.1 + pid(3)*0.2 = 0.90 (~230), G = 0.50 + iid(2)*0.15 = 0.80 (~204), A = 255
            require(p.a == 255 && abs((int)p.r - 230) <= 6 && abs((int)p.g - 204) <= 6,
                    [NSString stringWithFormat:@"Scenario 1 mismatch: (%u,%u,%u,%u)", p.r, p.g, p.b, p.a]);
            printf("Scenario 1 (tri direct + stepFuncs + patch_id/instance_id): PASS (%u,%u,%u,%u)\n", p.r, p.g, p.b, p.a);
        }

        // ---- 2. Quad patch direct draw ----
        {
            id<MTLRenderPipelineState> pso = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
                d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
                d.maxTessellationFactor = 16;
            }, &err);
            require(pso != nil, err.localizedDescription ?: @"quad pso failed");
            MTLQuadTessellationFactorsHalf tf = { { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, { f32_to_f16(2), f32_to_f16(2) } };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:&tf length:sizeof tf options:MTLResourceStorageModeShared];
            NSData *img = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:pso];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            // Center x~32 has uv.x~0.5 => wave = 4*0.5*0.5 = 1.0 -> R ~ 255
            RGBA8 center = pixel_at(img, 64, 32, 32);
            require(center.a == 255 && center.r >= 245,
                    [NSString stringWithFormat:@"Scenario 2 quad interior not subdivided: R=%u", center.r]);
            printf("Scenario 2 (quad direct interior wave@center): PASS R=%u\n", center.r);
        }

        // ---- 3. Compute-generated half factors + tessellationFactorScaleEnabled ----
        {
            id<MTLFunction> kf = [lib newFunctionWithName:@"gen_factors_and_indirect"];
            id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:kf error:&err];
            require(cpso != nil, err.localizedDescription ?: @"compute pso failed");
            id<MTLRenderPipelineState> psoScaled = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
                d.tessellationFactorScaleEnabled = YES;
                d.maxTessellationFactor = 16;
            }, &err);
            require(psoScaled != nil, err.localizedDescription ?: @"scaled pso failed");
            id<MTLRenderPipelineState> psoUnscaled = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
                d.tessellationFactorScaleEnabled = NO;
                d.maxTessellationFactor = 16;
            }, &err);
            require(psoUnscaled != nil, err.localizedDescription ?: @"unscaled pso failed");

            id<MTLBuffer> fbuf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
            id<MTLBuffer> ibuf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
            float edge = 1.0f; uint32_t strideH = 6u;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cpso];
            [ce setBuffer:fbuf offset:0 atIndex:0];
            [ce setBuffer:ibuf offset:0 atIndex:1];
            [ce setBytes:&edge length:sizeof edge atIndex:2];
            [ce setBytes:&strideH length:sizeof strideH atIndex:3];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:psoScaled];
            [re setVertexBuffer:quadVB offset:0 atIndex:0];
            [re setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
            [re setTessellationFactorScale:2.0f]; // 1.0 * 2.0 -> 2.0 (wave@center ~ 255)
            [re drawPatches:4 patchStart:1 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            [re endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            require(cb.status == MTLCommandBufferStatusCompleted, cb.error.localizedDescription ?: @"Scenario 3 scaled failed");
            NSData *imgScaled = readback_rt(q, rt);

            NSData *imgUnscaled = run_pass(^(id<MTLRenderCommandEncoder> re2) {
                [re2 setRenderPipelineState:psoUnscaled];
                [re2 setVertexBuffer:quadVB offset:0 atIndex:0];
                [re2 setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [re2 setTessellationFactorScale:2.0f]; // ignored because factorScaleEnabled == NO -> factor stays 1.0 -> wave@center ~ 0
                [re2 drawPatches:4 patchStart:1 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            RGBA8 cs = pixel_at(imgScaled, 64, 32, 32), cu = pixel_at(imgUnscaled, 64, 32, 32);
            require(cs.a == 255 && cs.r >= 245 && cu.a == 255 && cu.r <= 10,
                    [NSString stringWithFormat:@"Scenario 3 mismatch: scaled=(%u,%u,%u,%u) unscaled=(%u,%u,%u,%u)",
                        cs.r, cs.g, cs.b, cs.a, cu.r, cu.g, cu.b, cu.a]);
            printf("Scenario 3 (compute factor + factorScaleEnabled): PASS scaled_R=%u unscaled_R=%u\n", cs.r, cu.r);
        }

        // ---- 4. All 4 partition modes + same-MTLFunction pipeline isolation ----
        {
            MTLQuadTessellationFactorsHalf tf = { { f32_to_f16(2.5f), f32_to_f16(2.5f), f32_to_f16(2.5f), f32_to_f16(2.5f) },
                                                 { f32_to_f16(2.5f), f32_to_f16(2.5f) } };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:&tf length:sizeof tf options:MTLResourceStorageModeShared];
            MTLTessellationPartitionMode modes[4] = {
                MTLTessellationPartitionModePow2,
                MTLTessellationPartitionModeInteger,
                MTLTessellationPartitionModeFractionalOdd,
                MTLTessellationPartitionModeFractionalEven,
            };
            RGBA8 centerPx[4], quarterPx[4];
            for (int m = 0; m < 4; m++) {
                MTLTessellationPartitionMode pm = modes[m];
                id<MTLRenderPipelineState> pso = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                    d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
                    d.tessellationPartitionMode = pm;
                    d.maxTessellationFactor = 16;
                }, &err);
                require(pso != nil, err.localizedDescription ?: @"partition pso failed");
                NSData *img = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:pso];
                    [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                    [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                    [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
                });
                centerPx[m] = pixel_at(img, 64, 32, 32);
                quarterPx[m] = pixel_at(img, 64, 16, 32);
                require(centerPx[m].a == 255, @"partition draw transparent");
            }
            // Pow2(2.5 -> 4): u=0.5 has vertex => 255, u=0.25 has vertex => ~191.
            // FracOdd(2.5 -> 3): u=0.5 interpolated between outer nodes => < 245.
            // FracEven(2.5 -> 4 symm): u=0.5 has vertex => 255, outer ring moved => quarterPx != Pow2.
            require(centerPx[0].r >= 245 && centerPx[3].r >= 245 && centerPx[2].r < 245 && quarterPx[0].r > quarterPx[2].r,
                    [NSString stringWithFormat:@"Scenario 4 partition mismatch: center=(%u,%u,%u,%u) quarter=(%u,%u,%u,%u)",
                        centerPx[0].r, centerPx[1].r, centerPx[2].r, centerPx[3].r,
                        quarterPx[0].r, quarterPx[1].r, quarterPx[2].r, quarterPx[3].r]);
            printf("Scenario 4 (4 partition modes + same-fn isolation): PASS center=(%u,%u,%u,%u) q=(%u,%u,%u,%u)\n",
                   centerPx[0].r, centerPx[1].r, centerPx[2].r, centerPx[3].r,
                   quarterPx[0].r, quarterPx[1].r, quarterPx[2].r, quarterPx[3].r);
        }

        // ---- 5. Winding order & culling ----
        {
            id<MTLRenderPipelineState> psoCW = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationOutputWindingOrder = MTLWindingClockwise;
            }, &err);
            id<MTLRenderPipelineState> psoCCW = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
            }, &err);
            require(psoCW != nil && psoCCW != nil, @"winding psos failed");
            MTLQuadTessellationFactorsHalf tf = { { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, { f32_to_f16(2), f32_to_f16(2) } };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:&tf length:sizeof tf options:MTLResourceStorageModeShared];
            NSData *visCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoCW]; [enc setFrontFacingWinding:MTLWindingClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0]; [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *culCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoCW]; [enc setFrontFacingWinding:MTLWindingClockwise]; [enc setCullMode:MTLCullModeFront];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0]; [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *culCCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoCCW]; [enc setFrontFacingWinding:MTLWindingClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0]; [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *visCCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoCCW]; [enc setFrontFacingWinding:MTLWindingCounterClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0]; [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSUInteger hVisCW = count_non_clear(visCW, (RGBA8){0,0,0,0});
            NSUInteger hCulCW = count_non_clear(culCW, (RGBA8){0,0,0,0});
            NSUInteger hCulCCW = count_non_clear(culCCW, (RGBA8){0,0,0,0});
            NSUInteger hVisCCW = count_non_clear(visCCW, (RGBA8){0,0,0,0});
            require(hVisCW > 0 && hCulCW == 0 && hCulCCW == 0 && hVisCCW > 0,
                    [NSString stringWithFormat:@"Scenario 5 winding mismatch: visCW=%lu culCW=%lu culCCW=%lu visCCW=%lu",
                        (unsigned long)hVisCW, (unsigned long)hCulCW, (unsigned long)hCulCCW, (unsigned long)hVisCCW]);
            // Triangle winding parity check under LOWER_LEFT domain origin
            id<MTLRenderPipelineState> triCW = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
                d.tessellationOutputWindingOrder = MTLWindingClockwise;
            }, &err);
            id<MTLRenderPipelineState> triCCW = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
                d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
            }, &err);
            require(triCW != nil && triCCW != nil, @"Scenario 5 tri winding psos failed");
            MTLTriangleTessellationFactorsHalf triTF = { { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, f32_to_f16(2) };
            id<MTLBuffer> triFB = [dev newBufferWithBytes:&triTF length:sizeof triTF options:MTLResourceStorageModeShared];
            NSData *triVisCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:triCW]; [enc setFrontFacingWinding:MTLWindingClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:triVB0 offset:0 atIndex:0]; [enc setVertexBuffer:triVB1 offset:0 atIndex:1]; [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                [enc setTessellationFactorBuffer:triFB offset:0 instanceStride:0];
                [enc drawPatches:3 patchStart:3 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *triCulCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:triCW]; [enc setFrontFacingWinding:MTLWindingCounterClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:triVB0 offset:0 atIndex:0]; [enc setVertexBuffer:triVB1 offset:0 atIndex:1]; [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                [enc setTessellationFactorBuffer:triFB offset:0 instanceStride:0];
                [enc drawPatches:3 patchStart:3 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *triVisCCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:triCCW]; [enc setFrontFacingWinding:MTLWindingCounterClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:triVB0 offset:0 atIndex:0]; [enc setVertexBuffer:triVB1 offset:0 atIndex:1]; [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                [enc setTessellationFactorBuffer:triFB offset:0 instanceStride:0];
                [enc drawPatches:3 patchStart:3 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            NSData *triCulCCW = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:triCCW]; [enc setFrontFacingWinding:MTLWindingClockwise]; [enc setCullMode:MTLCullModeBack];
                [enc setVertexBuffer:triVB0 offset:0 atIndex:0]; [enc setVertexBuffer:triVB1 offset:0 atIndex:1]; [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                [enc setTessellationFactorBuffer:triFB offset:0 instanceStride:0];
                [enc drawPatches:3 patchStart:3 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            require(count_non_clear(triVisCW, (RGBA8){0,0,0,0}) > 0 && count_non_clear(triCulCW, (RGBA8){0,0,0,0}) == 0 &&
                    count_non_clear(triVisCCW, (RGBA8){0,0,0,0}) > 0 && count_non_clear(triCulCCW, (RGBA8){0,0,0,0}) == 0,
                    @"Scenario 5 triangle winding mismatch");

            // Asymmetric quad edge[1] (v=0 bottom) vs edge[3] (v=1 top) check
            MTLQuadTessellationFactorsHalf asymTF = {
                { f32_to_f16(1), f32_to_f16(4), f32_to_f16(1), f32_to_f16(1) },
                { f32_to_f16(1), f32_to_f16(1) }
            };
            id<MTLBuffer> asymFB = [dev newBufferWithBytes:&asymTF length:sizeof asymTF options:MTLResourceStorageModeShared];
            id<MTLRenderPipelineState> asymPSO = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
            }, &err);
            require(asymPSO != nil, @"asym pso failed");
            NSData *asymImg = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:asymPSO];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:asymFB offset:0 instanceStride:0];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            });
            RGBA8 botPx = pixel_at(asymImg, 64, 32, 63), topPx = pixel_at(asymImg, 64, 32, 0);
            require(botPx.a == 255 && topPx.a == 255 && botPx.r > 200 && topPx.r < 20,
                    [NSString stringWithFormat:@"Scenario 5 asym quad edge[1]/edge[3] mismatch: bot_y63=%u top_y0=%u", botPx.r, topPx.r]);
            printf("Scenario 5 (quad+tri winding CW/CCW + cull + asym edge[1]/edge[3]): PASS visCW=%lu botR=%u topR=%u\n",
                   (unsigned long)hVisCW, botPx.r, topPx.r);
        }

        // ---- 6. Factor step functions (PerInstance + PerPatchAndPerInstance) + baseInstance + patchStart + instanceStride ----
        {
            MTLQuadTessellationFactorsHalf tfGrid[4 * 2]; // stride=24B per instance (2 patches * 12B); row=instance, col=patch
            memset(tfGrid, 0, sizeof tfGrid);
            // Put valid 2.0 factors ONLY at instance 2, patch 1! All others 0.0 (culled)!
            // With stride = 24 bytes (2 Quad factors per instance), instance 2 starts at element index 2*2=4, patch 1 = element 5.
            tfGrid[5] = (MTLQuadTessellationFactorsHalf){ { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) },
                                                          { f32_to_f16(2), f32_to_f16(2) } };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:tfGrid length:sizeof tfGrid options:MTLResourceStorageModeShared];
            id<MTLRenderPipelineState> psoPPI = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatchAndPerInstance;
            }, &err);
            require(psoPPI != nil, err.localizedDescription ?: @"PPI pso failed");
            NSData *imgHit = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoPPI];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:24];
                [enc drawPatches:4 patchStart:1 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:2];
            });
            NSData *imgMiss = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoPPI];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:24];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:2];
            });
            // Also test PerInstance
            id<MTLRenderPipelineState> psoPI = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerInstance;
            }, &err);
            require(psoPI != nil, err.localizedDescription ?: @"PI pso failed");
            MTLQuadTessellationFactorsHalf tfInst[3];
            memset(tfInst, 0, sizeof tfInst);
            tfInst[2] = tfGrid[5];
            id<MTLBuffer> fibuf = [dev newBufferWithBytes:tfInst length:sizeof tfInst options:MTLResourceStorageModeShared];
            NSData *imgPI = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoPI];
                [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:fibuf offset:0 instanceStride:sizeof(MTLQuadTessellationFactorsHalf)];
                [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:2];
            });
            require(count_non_clear(imgHit, (RGBA8){0,0,0,0}) > 0 &&
                    count_non_clear(imgMiss, (RGBA8){0,0,0,0}) == 0 &&
                    count_non_clear(imgPI, (RGBA8){0,0,0,0}) > 0, @"Scenario 6 step function mismatch");
            printf("Scenario 6 (PerPatchAndPerInstance + PerInstance + instanceStride + baseInstance): PASS\n");
        }

        // ---- 7. drawIndexedPatches: UInt16 & UInt32 + patchIndexBuffer != nil ----
        {
            uint32_t patchIdxVals[2] = { 0u, 3u }; // drawPatchIndex 1 -> patchIndex = 3
            uint16_t cpIdx16[12] = { 0,1,2, 0,1,2, 0,1,2, 9,10,11 };
            uint32_t cpIdx32[12] = { 0,1,2, 0,1,2, 0,1,2, 9,10,11 };
            id<MTLBuffer> pbuf = [dev newBufferWithBytes:patchIdxVals length:sizeof patchIdxVals options:MTLResourceStorageModeShared];
            id<MTLBuffer> cp16 = [dev newBufferWithBytes:cpIdx16 length:sizeof cpIdx16 options:MTLResourceStorageModeShared];
            id<MTLBuffer> cp32 = [dev newBufferWithBytes:cpIdx32 length:sizeof cpIdx32 options:MTLResourceStorageModeShared];

            MTLTriangleTessellationFactorsHalf tf[2];
            memset(tf, 0, sizeof tf); // slot 0 culled, slot 1 = drawPatchIndex 1 active (proving PerPatch uses drawPatchIndex!)
            tf[1] = (MTLTriangleTessellationFactorsHalf){ { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, f32_to_f16(2) };
            id<MTLBuffer> fbuf = [dev newBufferWithBytes:tf length:sizeof tf options:MTLResourceStorageModeShared];

            for (int use32 = 0; use32 <= 1; use32++) {
                id<MTLRenderPipelineState> pso = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                    d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                    d.tessellationControlPointIndexType = use32 ? MTLTessellationControlPointIndexTypeUInt32 : MTLTessellationControlPointIndexTypeUInt16;
                }, &err);
                require(pso != nil, err.localizedDescription ?: @"indexed pso failed");
                NSData *img = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:pso];
                    [enc setVertexBuffer:triVB0 offset:0 atIndex:0];
                    [enc setVertexBuffer:triVB1 offset:0 atIndex:1];
                    [enc setVertexBuffer:triVB2 offset:0 atIndex:2];
                    [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                    [enc drawIndexedPatches:3 patchStart:1 patchCount:1 patchIndexBuffer:pbuf patchIndexBufferOffset:0
                        controlPointIndexBuffer:(use32 ? cp32 : cp16) controlPointIndexBufferOffset:0
                        instanceCount:1 baseInstance:1];
                });
                RGBA8 p = pixel_at(img, 64, 32, 32);
                // patchIndex=3 => triCP[9..11](R=0.2)+bias(0.1)+pid3(0.6)=0.90 (~230), G=perPatch[3](0.50)+iid1(0.15)=0.65 (~166)
                require(p.a == 255 && abs((int)p.r - 230) <= 6 && abs((int)p.g - 166) <= 6,
                        [NSString stringWithFormat:@"Scenario 7 (%s) mismatch: (%u,%u,%u,%u)",
                            use32 ? "UInt32" : "UInt16", p.r, p.g, p.b, p.a]);
            }
            printf("Scenario 7 (drawIndexedPatches UInt16 + UInt32 + patchIndexBuffer): PASS\n");
        }

        // ---- 8. Indirect patch draws (direct & indexed, GPU compute-written MTLDrawPatchIndirectArguments) ----
        {
            id<MTLFunction> kf = [lib newFunctionWithName:@"gen_factors_and_indirect"];
            id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:kf error:&err];
            id<MTLRenderPipelineState> psoDir = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
            }, &err);
            id<MTLRenderPipelineState> psoIdx = make_tess_pso(dev, lib, @"tri_tes", @"pass_fs", triVD, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeUInt32;
            }, &err);
            require(cpso != nil && psoDir != nil && psoIdx != nil, @"Scenario 8 psos failed");

            uint32_t patchIdxVals[2] = { 0u, 3u };
            uint32_t cpIdx32[12] = { 0,1,2, 0,1,2, 0,1,2, 9,10,11 };
            id<MTLBuffer> pbuf = [dev newBufferWithBytes:patchIdxVals length:sizeof patchIdxVals options:MTLResourceStorageModeShared];
            id<MTLBuffer> cp32 = [dev newBufferWithBytes:cpIdx32 length:sizeof cpIdx32 options:MTLResourceStorageModeShared];
            id<MTLBuffer> fbuf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
            id<MTLBuffer> ibuf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
            float edge = 2.0f; uint32_t strideH = 4u;

            for (int idxMode = 0; idxMode <= 1; idxMode++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:cpso];
                [ce setBuffer:fbuf offset:0 atIndex:0];
                [ce setBuffer:ibuf offset:0 atIndex:1];
                [ce setBytes:&edge length:sizeof edge atIndex:2];
                [ce setBytes:&strideH length:sizeof strideH atIndex:3];
                [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [ce endEncoding];

                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:(idxMode ? psoIdx : psoDir)];
                [re setVertexBuffer:triVB0 offset:0 atIndex:0];
                [re setVertexBuffer:triVB1 offset:0 atIndex:1];
                [re setVertexBuffer:triVB2 offset:0 atIndex:2];
                [re setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                if (idxMode) {
                    [re drawIndexedPatches:3 patchIndexBuffer:pbuf patchIndexBufferOffset:0
                        controlPointIndexBuffer:cp32 controlPointIndexBufferOffset:0
                        indirectBuffer:ibuf indirectBufferOffset:0];
                } else {
                    [re drawPatches:3 patchIndexBuffer:pbuf patchIndexBufferOffset:0
                        indirectBuffer:ibuf indirectBufferOffset:0];
                }
                [re endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                require(cb.status == MTLCommandBufferStatusCompleted, cb.error.localizedDescription ?: @"Scenario 8 CB failed");
                NSData *img = readback_rt(q, rt);
                RGBA8 p = pixel_at(img, 64, 32, 32);
                // compute wrote patchStart=1 -> patchIndex=3 -> triCP[9..11], perPatch[3], baseInstance=2 => R~230, G~204
                require(p.a == 255 && abs((int)p.r - 230) <= 6 && abs((int)p.g - 204) <= 6,
                        [NSString stringWithFormat:@"Scenario 8 (%s) mismatch: (%u,%u,%u,%u)",
                            idxMode ? "indexed" : "direct", p.r, p.g, p.b, p.a]);
            }
            printf("Scenario 8 (indirect direct + indexed patch draws): PASS\n");
        }

        // ---- 9. Culling (0/neg/NaN), zero-CP patch, negative rejections ----
        {
            id<MTLRenderPipelineState> psoQ = make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", quadVD, nil, &err);
            uint16_t cullVals[3] = { 0x0000u, 0xBC00u /* -1.0 */, 0x7E00u /* NaN */ };
            for (int c = 0; c < 3; c++) {
                MTLQuadTessellationFactorsHalf tf = { { cullVals[c], f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) },
                                                     { f32_to_f16(2), f32_to_f16(2) } };
                id<MTLBuffer> fbuf = [dev newBufferWithBytes:&tf length:sizeof tf options:MTLResourceStorageModeShared];
                NSData *img = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:psoQ];
                    [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                    [enc setTessellationFactorBuffer:fbuf offset:0 instanceStride:0];
                    [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
                });
                require(count_non_clear(img, (RGBA8){0,0,0,0}) == 0,
                        [NSString stringWithFormat:@"Scenario 9 cull case %d drew non-zero pixels", c]);
            }

            // Zero-CP patch
            id<MTLRenderPipelineState> psoZ = make_tess_pso(dev, lib, @"zero_cp_tes", @"pass_fs", nil, ^(MTLRenderPipelineDescriptor *d) {
                d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
            }, &err);
            require(psoZ != nil, err.localizedDescription ?: @"zero_cp pso failed");
            MTLTriangleTessellationFactorsHalf ztf = { { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) }, f32_to_f16(2) };
            id<MTLBuffer> zfb = [dev newBufferWithBytes:&ztf length:sizeof ztf options:MTLResourceStorageModeShared];
            NSData *zimg = run_pass(^(id<MTLRenderCommandEncoder> enc) {
                [enc setRenderPipelineState:psoZ];
                [enc setTessellationFactorBuffer:zfb offset:0 instanceStride:0];
                [enc drawPatches:0 patchStart:1 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:2];
            });
            RGBA8 zp = pixel_at(zimg, 64, 32, 32);
            require(zp.a == 255 && abs((int)zp.r - 128) <= 6 && abs((int)zp.g - 153) <= 6,
                    [NSString stringWithFormat:@"Scenario 9 zero-CP mismatch: (%u,%u,%u,%u)", zp.r, zp.g, zp.b, zp.a]);

            // Negative rejections (Garnet strict contract checks)
            if (strcmp(argv[1], "--reference")) {
                NSError *ne = nil;
                MTLVertexDescriptor *badVD = [MTLVertexDescriptor vertexDescriptor];
                badVD.attributes[0].format = MTLVertexFormatFloat2; badVD.attributes[0].bufferIndex = 0;
                badVD.layouts[0].stride = 8; badVD.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
                require(make_tess_pso(dev, lib, @"quad_wave_tes", @"pass_fs", badVD, nil, &ne) == nil,
                        @"expected PerVertex stepFunction rejection");

                id<MTLRenderPipelineState> plainPso = make_tess_pso(dev, lib, @"plain_vs", @"pass_fs", nil, nil, &ne);
                require(plainPso != nil, @"plain pso failed");

                void (^expect_cb_fail)(void (^)(id<MTLRenderCommandEncoder>), NSString *) = ^(void (^encBlock)(id<MTLRenderCommandEncoder> enc), NSString *label) {
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                    encBlock(enc);
                    [enc endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                    require(cb.status == MTLCommandBufferStatusError, label);
                };
                expect_cb_fail(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:psoQ];
                    [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                    [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
                }, @"missing factor buffer should fail CB");
                expect_cb_fail(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:psoQ];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }, @"drawPrimitives on tess PSO should fail CB");
                expect_cb_fail(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:plainPso];
                    [enc setTessellationFactorBuffer:zfb offset:0 instanceStride:0];
                    [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
                }, @"drawPatches on non-tess PSO should fail CB");
                uint32_t dummyIdx[4] = { 0, 1, 2, 3 };
                id<MTLBuffer> idxb = [dev newBufferWithBytes:dummyIdx length:sizeof dummyIdx options:MTLResourceStorageModeShared];
                expect_cb_fail(^(id<MTLRenderCommandEncoder> enc) {
                    [enc setRenderPipelineState:psoQ];
                    [enc setVertexBuffer:quadVB offset:0 atIndex:0];
                    [enc setTessellationFactorBuffer:zfb offset:0 instanceStride:0];
                    [enc drawIndexedPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0
                        controlPointIndexBuffer:idxb controlPointIndexBufferOffset:0 instanceCount:1 baseInstance:0];
                }, @"drawIndexedPatches with cp_idx_type=None should fail CB");
            }
            printf("Scenario 9 (0/neg/NaN cull, 0-CP patch, negative validations): PASS\n");
        }

        printf("ALL_TESSELLATION_TESTS_PASS\n");
        return 0;
    }
}
