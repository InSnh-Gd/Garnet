// Live GPU test for calculate_clamped_lod / calculate_unclamped_lod (MSL 2.2+) across
// direct and indirect argument-buffer resources, quad divergence, triangle edge helpers, and uniform flow.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4_ARGUMENT_TEST_PREFIX "ARGUMENT_QUERY_LOD"
#include "test-argument-render-support.h"

typedef struct { float baseScale; uint32_t enable, iters, pad; } FlowParams;

static id<MTLDevice> loadDevice(BOOL reference, const char *bundlePath) {
    if (reference) return MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    NSBundle *bundle = [NSBundle bundleWithPath:@(bundlePath)];
    require([bundle loadAndReturnError:&error], error.description ?: @"bundle unavailable");
    return [[[bundle classNamed:@"GarnetDevice"] alloc] init];
}

static id<MTLLibrary> compileMSL(id<MTLDevice> device, MTLLanguageVersion ver, NSString *src,
                                 NSString *dir, NSString *tag, NSError **outError) {
    NSString *srcPath = [dir stringByAppendingPathComponent:[tag stringByAppendingString:@".metal"]];
    NSError *werr = nil;
    require([src writeToFile:srcPath atomically:YES encoding:NSUTF8StringEncoding error:&werr], @"metal write failed");
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = ver;
    return [device newLibraryWithSource:src options:opts error:outError];
}

static id<MTLTexture> makeColorMipTex(id<MTLDevice> device, NSUInteger size) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:size height:size mipmapped:YES];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    require(t != nil && t.mipmapLevelCount > 1, @"mipmapped color texture unavailable");
    size_t bufBytes = size * size * 4;
    uint8_t *zeros = calloc(bufBytes, 1);
    require(zeros != NULL, @"zeros alloc failed");
    for (NSUInteger level = 0; level < t.mipmapLevelCount; ++level) {
        NSUInteger w = MAX((NSUInteger)1, size >> level);
        [t replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:level withBytes:zeros bytesPerRow:w * 4];
    }
    free(zeros);
    return t;
}

static id<MTLSamplerState> makeLodSampler(id<MTLDevice> device, float lodMin, float lodMax) {
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.normalizedCoordinates = YES;
    d.minFilter = MTLSamplerMinMagFilterLinear;
    d.magFilter = MTLSamplerMinMagFilterLinear;
    d.mipFilter = MTLSamplerMipFilterLinear;
    d.sAddressMode = MTLSamplerAddressModeClampToEdge;
    d.tAddressMode = MTLSamplerAddressModeClampToEdge;
    d.rAddressMode = MTLSamplerAddressModeClampToEdge;
    d.lodMinClamp = lodMin;
    d.lodMaxClamp = lodMax;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:d];
    require(s != nil, @"sampler unavailable");
    return s;
}

static void expectedLod(uint32_t img, uint32_t samp, uint32_t iters, float *outClamped, float *outUnclamped) {
    float baseUnclamped = img == 0 ? 3.0f : -1.0f;
    float lo = samp == 0 ? 0.0f : 1.0f;
    float hi = samp == 0 ? 4.0f : 2.0f;
    float cSum = 0.0f, uSum = 0.0f;
    for (uint32_t k = 0; k < iters; ++k) {
        float u = baseUnclamped + (float)k;
        float c = fminf(fmaxf(u, lo), hi);
        cSum += c;
        uSum += u;
    }
    *outClamped = cSum;
    *outUnclamped = uSum;
}

static id<MTLTexture> runPass(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pso,
                              id<MTLBuffer> args, NSUInteger argsLen, NSData *argsSnap,
                              id<MTLTexture> __strong images[2], id<MTLSamplerState> __strong directSamp,
                              id<MTLBuffer> flowBuf, BOOL isDirect, uint32_t directImgIndex) {
    id<MTLTexture> target = newTarget(device);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(-99.0, -99.0, -99.0, -99.0);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    require(cb && enc, @"render encoder unavailable");
    [enc setRenderPipelineState:pso];
    [enc setViewport:(MTLViewport){0, 0, kTargetSize, kTargetSize, 0, 1}];
    [enc setScissorRect:(MTLScissorRect){0, 0, kTargetSize, kTargetSize}];
    if (isDirect) {
        [enc setFragmentTexture:images[directImgIndex] atIndex:0];
        [enc setFragmentSamplerState:directSamp atIndex:0];
    } else {
        [enc setFragmentBuffer:args offset:kArgumentOffset atIndex:3];
        if (flowBuf) [enc setFragmentBuffer:flowBuf offset:kControlOffset atIndex:0];
        for (unsigned i = 0; i < 2; ++i)
            [enc useResource:images[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
        cb.error.description ?: @"query-lod draw did not complete");
    if (!isDirect) {
        requireGuard(args, kArgumentOffset, argsLen, kArgumentGuard, @"query-lod arg guards");
        require(!memcmp(args.contents, argsSnap.bytes, args.length), @"query-lod arg storage changed");
    }
    return target;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 4 && (!strcmp(argv[1], "--bundle") || !strcmp(argv[1], "--reference")),
            @"usage: test-argument-query-lod --bundle BUNDLE OUTPUT | --reference - OUTPUT");
        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = loadDevice(reference, argv[2]);
        require(device != nil, @"device unavailable");
        NSString *dir = @(argv[3]);
        NSError *err = nil;
        require([[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                           attributes:nil error:&err], @"output dir unavailable");
        if (!reference) require(device.argumentBuffersSupport == MTLArgumentBuffersTier2, @"pre-open argumentBuffersSupport should be Tier2");
        if (@available(macOS 11.0, *)) {
            if (!reference) require(!device.supportsQueryTextureLOD, @"supportsQueryTextureLOD opened backend eagerly");
            id<MTLCommandQueue> probeQueue = [device newCommandQueue];
            require(probeQueue != nil, @"queue unavailable");
            require(device.supportsQueryTextureLOD, @"supportsQueryTextureLOD should be YES after open");
            if (!reference) require(device.argumentBuffersSupport == MTLArgumentBuffersTier2, @"post-open argumentBuffersSupport should stay Tier2");
            (void)probeQueue;
        } else {
            require(NO, @"macOS 11+ required for supportsQueryTextureLOD");
        }

        NSString *msl =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct Args { array<texture2d<float>, 2> images [[id(0)]]; array<sampler, 2> samplers [[id(2)]]; };\n"
             "struct FlowParams { float baseScale; uint enable; uint iters; uint pad; };\n"
             "vertex float4 q_full_vs(uint vid [[vertex_id]]) {\n"
             "  float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));\n"
             "  return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);\n"
             "}\n"
             "vertex float4 q_edge_vs(uint vid [[vertex_id]]) {\n"
             "  float2 p = float2(vid == 1u ? 1.0f : -1.0f, vid == 2u ? 1.0f : -1.0f);\n"
             "  return float4(p, 0.0f, 1.0f);\n"
             "}\n"
             "fragment float4 q_direct_fs(float4 p [[position]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
             "  float2 uv = p.xy * (1.0f / 16.0f);\n"
             "  return float4(t.calculate_clamped_lod(s, uv), t.calculate_unclamped_lod(s, uv), 0.0f, 1.0f);\n"
             "}\n"
             "fragment float4 q_div_fs(float4 p [[position]], constant Args &a [[buffer(3)]]) {\n"
             "  uint i = uint(p.x) & 1u;\n"
             "  uint j = uint(p.y) & 1u;\n"
             "  float2 uv = p.xy * (1.0f / 16.0f);\n"
             "  return float4(a.images[i].calculate_clamped_lod(a.samplers[j], uv),\n"
             "                a.images[i].calculate_unclamped_lod(a.samplers[j], uv), 0.0f, 1.0f);\n"
             "}\n"
             "fragment float4 q_flow_fs(float4 p [[position]], constant Args &a [[buffer(3)]], constant FlowParams &fp [[buffer(0)]]) {\n"
             "  uint i = uint(p.x) & 1u;\n"
             "  uint j = uint(p.y) & 1u;\n"
             "  float c = 0.0f, u = 0.0f;\n"
             "  if (fp.enable != 0u) {\n"
             "    for (uint k = 0u; k < fp.iters; ++k) {\n"
             "      float2 uv = p.xy * (fp.baseScale * float(1u << k));\n"
             "      c += a.images[i].calculate_clamped_lod(a.samplers[j], uv);\n"
             "      u += a.images[i].calculate_unclamped_lod(a.samplers[j], uv);\n"
             "    }\n"
             "  }\n"
             "  return float4(c, u, 0.0f, 1.0f);\n"
             "}\n";

        err = nil;
        id<MTLLibrary> lib21 = compileMSL(device, MTLLanguageVersion2_1, msl, dir, @"query-lod-21", &err);
        require(lib21 == nil, @"MSL 2.1 unexpectedly accepted calculate_clamped_lod / calculate_unclamped_lod");

        err = nil;
        id<MTLLibrary> lib = compileMSL(device, MTLLanguageVersion2_2, msl, dir, @"query-lod-22", &err);
        require(lib != nil, err.description ?: @"MSL 2.2 compile failed");

        id<MTLFunction> vsFull = [lib newFunctionWithName:@"q_full_vs"];
        id<MTLFunction> vsEdge = [lib newFunctionWithName:@"q_edge_vs"];
        id<MTLFunction> fsDirect = [lib newFunctionWithName:@"q_direct_fs"];
        id<MTLFunction> fsDiv = [lib newFunctionWithName:@"q_div_fs"];
        id<MTLFunction> fsFlow = [lib newFunctionWithName:@"q_flow_fs"];
        require(vsFull && vsEdge && fsDirect && fsDiv && fsFlow, @"missing functions");
        writeAIR(fsDirect, dir);
        writeAIR(fsDiv, dir);
        writeAIR(fsFlow, dir);

        id<MTLRenderPipelineState> psoDirect = newPipeline(device, vsFull, fsDirect, @"direct pso", &err);
        id<MTLRenderPipelineState> psoDiv = newPipeline(device, vsFull, fsDiv, @"divergent pso", &err);
        id<MTLRenderPipelineState> psoEdge = newPipeline(device, vsEdge, fsDiv, @"edge pso", &err);
        id<MTLRenderPipelineState> psoFlow = newPipeline(device, vsFull, fsFlow, @"flow pso", &err);

        id<MTLTexture> images[2] = { makeColorMipTex(device, 128), makeColorMipTex(device, 8) };
        id<MTLSamplerState> samplers[2] = { makeLodSampler(device, 0.0f, 4.0f), makeLodSampler(device, 1.0f, 2.0f) };
        id<MTLArgumentEncoder> ae = [fsDiv newArgumentEncoderWithBufferIndex:3];
        require(ae != nil, @"argument encoder unavailable");
        NSUInteger argsLen = alignUp(ae.encodedLength, 16);
        id<MTLBuffer> args = newSharedBuffer(device, kArgumentOffset + argsLen + kArgumentTail, kArgumentGuard, @"args buffer");
        [ae setArgumentBuffer:args offset:kArgumentOffset];
        [ae setTexture:images[0] atIndex:0];
        [ae setTexture:images[1] atIndex:1];
        [ae setSamplerState:samplers[0] atIndex:2];
        [ae setSamplerState:samplers[1] atIndex:3];
        NSData *argsSnap = [NSData dataWithBytes:args.contents length:args.length];

        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");

        // 1. Direct (tex0, samp1) and (tex1, samp1)
        for (uint32_t img = 0; img < 2; ++img) {
            id<MTLTexture> target = runPass(device, queue, psoDirect, args, argsLen, argsSnap, images, samplers[1], nil, YES, img);
            float pixels[kTargetSize * kTargetSize * 4], wc = 0, wu = 0;
            expectedLod(img, 1, 1, &wc, &wu);
            [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
                  fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
            for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
                float ac = pixels[4 * (y * kTargetSize + x) + 0], au = pixels[4 * (y * kTargetSize + x) + 1];
                require(fabsf(ac - wc) <= 1e-3f && fabsf(au - wu) <= 1e-3f,
                    [NSString stringWithFormat:@"direct img=%u (%u,%u) got=(%g,%g) want=(%g,%g)", img, x, y, ac, au, wc, wu]);
            }
        }

        // 2. Quad-divergent indirect (images[x&1], samplers[y&1])
        {
            id<MTLTexture> target = runPass(device, queue, psoDiv, args, argsLen, argsSnap, images, nil, nil, NO, 0);
            float pixels[kTargetSize * kTargetSize * 4];
            [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
                  fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
            for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
                float wc = 0, wu = 0;
                expectedLod(x & 1u, y & 1u, 1, &wc, &wu);
                float ac = pixels[4 * (y * kTargetSize + x) + 0], au = pixels[4 * (y * kTargetSize + x) + 1];
                require(fabsf(ac - wc) <= 1e-3f && fabsf(au - wu) <= 1e-3f,
                    [NSString stringWithFormat:@"divergent (%u,%u) got=(%g,%g) want=(%g,%g)", x, y, ac, au, wc, wu]);
            }
        }

        // 3. Edge triangle with half-covered 2x2 quads (inactive helper lanes)
        uint32_t edgeCovered = 0;
        {
            id<MTLTexture> target = runPass(device, queue, psoEdge, args, argsLen, argsSnap, images, nil, nil, NO, 0);
            float pixels[kTargetSize * kTargetSize * 4];
            [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
                  fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
            for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
                float r = pixels[4 * (y * kTargetSize + x) + 0], g = pixels[4 * (y * kTargetSize + x) + 1];
                float b = pixels[4 * (y * kTargetSize + x) + 2], a = pixels[4 * (y * kTargetSize + x) + 3];
                if (a > 0.0f) {
                    edgeCovered++;
                    float wc = 0, wu = 0;
                    expectedLod(x & 1u, y & 1u, 1, &wc, &wu);
                    require(fabsf(r - wc) <= 1e-3f && fabsf(g - wu) <= 1e-3f,
                        [NSString stringWithFormat:@"edge covered (%u,%u) got=(%g,%g) want=(%g,%g)", x, y, r, g, wc, wu]);
                } else {
                    require(r == -99.0f && g == -99.0f && b == -99.0f && a == -99.0f,
                        [NSString stringWithFormat:@"edge uncovered (%u,%u) dirty (%g,%g,%g,%g)", x, y, r, g, b, a]);
                }
            }
            require(edgeCovered >= 28 && edgeCovered <= 36,
                [NSString stringWithFormat:@"unexpected triangle coverage %u", edgeCovered]);
        }

        // 4. Uniform branch + uniform loop accumulating LOD queries across 2 scales
        {
            FlowParams fp = { 1.0f / 16.0f, 1u, 2u, 0u };
            id<MTLBuffer> flowBuf = newSharedBuffer(device, kControlOffset + sizeof fp + kControlTail, kControlGuard, @"flow buf");
            memcpy((uint8_t *)flowBuf.contents + kControlOffset, &fp, sizeof fp);
            id<MTLTexture> target = runPass(device, queue, psoFlow, args, argsLen, argsSnap, images, nil, flowBuf, NO, 0);
            requireGuard(flowBuf, kControlOffset, sizeof fp, kControlGuard, @"flow buf guards");
            float pixels[kTargetSize * kTargetSize * 4];
            [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
                  fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
            for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
                float wc = 0, wu = 0;
                expectedLod(x & 1u, y & 1u, 2, &wc, &wu);
                float ac = pixels[4 * (y * kTargetSize + x) + 0], au = pixels[4 * (y * kTargetSize + x) + 1];
                require(fabsf(ac - wc) <= 1e-3f && fabsf(au - wu) <= 1e-3f,
                    [NSString stringWithFormat:@"uniform-flow (%u,%u) got=(%g,%g) want=(%g,%g)", x, y, ac, au, wc, wu]);
            }
        }

        printf("ARGUMENT_QUERY_LOD_PASS backend=%s msl=2.2 cases=4 direct=2 divergent=64 edge_helper=%u uniform_flow=64 guards=argument gpu_execution=1\n",
            reference ? "reference" : "bundle", edgeCovered);
    }
    return 0;
}