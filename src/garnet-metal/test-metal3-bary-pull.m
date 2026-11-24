// test-metal3-bary-pull.m: live RX 9060 XT / reference Metal verification for Task 2.4
// (Shader Barycentric Coordinates: [[barycentric_coord]] perspective + center_no_perspective,
// and Pull-Model Interpolation: interpolant<T>::interpolate_at_{center,centroid,sample,offset}
// at 4x MSAA for float2 perspective/no_perspective and half2 no_perspective vs analytical oracle).
// Build on macOS:
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-bary-pull.m -o "$TMPDIR/test-metal3-bary-pull"
// Run:
//   "$TMPDIR/test-metal3-bary-pull" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-bary-pull" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (GarnetTestAccelInit)
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

static id<MTLDevice> createGarnetDevice(Class cls) {
    id<MTLDevice> dev = nil;
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (iokit) {
        typedef void *(*io_matching_fn)(const char *);
        typedef uint32_t (*io_get_svc_fn)(uint32_t, void *);
        typedef int (*io_rel_fn)(uint32_t);
        io_matching_fn matchFn = (io_matching_fn)dlsym(iokit, "IOServiceMatching");
        io_get_svc_fn getSvcFn = (io_get_svc_fn)dlsym(iokit, "IOServiceGetMatchingService");
        io_rel_fn relFn = (io_rel_fn)dlsym(iokit, "IOObjectRelease");
        if (matchFn && getSvcFn) {
            void *matching = matchFn("GarnetAccelerator");
            uint32_t port = matching ? getSvcFn(0u /* kIOMainPortDefault */, matching) : 0u;
            if (port) {
                if ([cls instancesRespondToSelector:@selector(initWithAcceleratorPort:)])
                    dev = [[cls alloc] initWithAcceleratorPort:port];
                if (relFn) relFn(port);
            }
        }
    }
    if (!dev) {
        dev = [[cls alloc] init];
        if (dev && [dev respondsToSelector:sel_registerName("initLimits")])
            ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
    }
    return dev;
}

enum {
    kW = 16,
    kH = 16,
    kGuardBytes = 256,
    kGuardWords = kGuardBytes / 4,
};
static const uint32_t kHeadGuard = 0xDEADBEEFu;
static const uint32_t kTailGuard = 0xCAFEBABEu;

typedef struct {
    float bcP[4];
    float bcNP[4];
    float stdP[4];
    float stdNP[4];
    uint32_t covered;
    uint32_t pad[3];
} BaryPixel;

typedef struct {
    float pos[2];
    float npCenter[2];
    float npCentroid[2];
    float npSample[2];
    float npOffA[2];
    float npOffB[2];
    float pCenter[2];
    float pSample[2];
    float pOffA[2];
    float hOffA[2];
    uint32_t covered;
    uint32_t pad[3];
} PullSample;

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_BARY_PULL_FAIL %s\n", msg.UTF8String);
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

static void initGuardedBuffer(id<MTLBuffer> buf, NSUInteger payloadBytes) {
    uint8_t *base = (uint8_t *)buf.contents;
    uint32_t *head = (uint32_t *)base;
    uint32_t *tail = (uint32_t *)(base + kGuardBytes + payloadBytes);
    for (int i = 0; i < kGuardWords; i++) {
        head[i] = kHeadGuard;
        tail[i] = kTailGuard;
    }
    memset(base + kGuardBytes, 0, payloadBytes);
}

static void checkGuards(id<MTLBuffer> buf, NSUInteger payloadBytes, NSString *tag) {
    const uint8_t *base = (const uint8_t *)buf.contents;
    const uint32_t *head = (const uint32_t *)base;
    const uint32_t *tail = (const uint32_t *)(base + kGuardBytes + payloadBytes);
    for (int i = 0; i < kGuardWords; i++) {
        require(head[i] == kHeadGuard,
                [NSString stringWithFormat:@"%@: head guard[%d] clobbered (0x%08x)", tag, i, head[i]]);
        require(tail[i] == kTailGuard,
                [NSString stringWithFormat:@"%@: tail guard[%d] clobbered (0x%08x)", tag, i, tail[i]]);
    }
}

static void analyticalBary(float x, float y, float outLinear[3], float outPersp[3]) {
    // Triangle vertices in 16x16 window coordinates:
    //   v0 = (2, 2),  w0 = 1.0
    //   v1 = (2, 14), w1 = 2.0
    //   v2 = (14, 2), w2 = 4.0
    float l2 = (x - 2.0f) / 12.0f;
    float l1 = (y - 2.0f) / 12.0f;
    float l0 = 1.0f - l1 - l2;
    outLinear[0] = l0;
    outLinear[1] = l1;
    outLinear[2] = l2;

    float q0 = l0 / 1.0f;
    float q1 = l1 / 2.0f;
    float q2 = l2 / 4.0f;
    float invSum = 1.0f / (q0 + q1 + q2);
    outPersp[0] = q0 * invSum;
    outPersp[1] = q1 * invSum;
    outPersp[2] = q2 * invSum;
}

static void checkFloat2(const float got[2], float ex, float ey, float tol, NSString *tag) {
    if (!isfinite(got[0]) || !isfinite(got[1]) || fabsf(got[0] - ex) > tol || fabsf(got[1] - ey) > tol) {
        failf([NSString stringWithFormat:@"%@: got=(%g,%g) expected=(%g,%g) tol=%g",
               tag, got[0], got[1], ex, ey, tol]);
    }
}

static void checkFloat3(const float got[3], const float exp[3], float tol, NSString *tag) {
    for (int i = 0; i < 3; i++) {
        if (!isfinite(got[i]) || fabsf(got[i] - exp[i]) > tol) {
            failf([NSString stringWithFormat:@"%@: ch%d got=%g expected=%g got=(%g,%g,%g) exp=(%g,%g,%g)",
                   tag, i, got[i], exp[i], got[0], got[1], got[2], exp[0], exp[1], exp[2]]);
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
            dev = createGarnetDevice(cls);
        }
        require(dev != nil, @"device unavailable");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        // Trigger backend initialization before probing runtime Vulkan-backed Metal 3 caps.
        id<MTLBuffer> prime = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        require(prime != nil, @"prime buffer unavailable");

        BOOL hasBary = dev.supportsShaderBarycentricCoordinates;
        BOOL hasPull = dev.supportsPullModelInterpolation;
        printf("METAL3_BARY_PULL_CAPS bary=%d pull=%d\n", (int)hasBary, (int)hasPull);
        if (!reference) {
            require(hasBary, @"Garnet RX 9060 XT must report supportsShaderBarycentricCoordinates == YES");
            require(hasPull, @"Garnet RX 9060 XT must report supportsPullModelInterpolation == YES");
            require(![dev supportsFamily:(MTLGPUFamily)1006], @"MTLGPUFamilyApple6 must stay NO");
        }

        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "\n"
             "struct BaryPixel {\n"
             "    float4 bcP;\n"
             "    float4 bcNP;\n"
             "    float4 stdP;\n"
             "    float4 stdNP;\n"
             "    uint covered;\n"
             "    uint pad0;\n"
             "    uint pad1;\n"
             "    uint pad2;\n"
             "};\n"
             "\n"
             "struct BaryVOut {\n"
             "    float4 pos [[position]];\n"
             "    float3 attrP;\n"
             "    float3 attrNP [[center_no_perspective]];\n"
             "};\n"
             "\n"
             "vertex BaryVOut vs_bary(uint vid [[vertex_id]]) {\n"
             "    const float4 clip[3] = {\n"
             "        float4(-0.75f,  0.75f, 0.5f, 1.0f),\n"
             "        float4(-1.50f, -1.50f, 1.0f, 2.0f),\n"
             "        float4( 3.00f,  3.00f, 2.0f, 4.0f),\n"
             "    };\n"
             "    const float3 attrs[3] = {\n"
             "        float3(1.0f, 0.0f, 0.0f),\n"
             "        float3(0.0f, 1.0f, 0.0f),\n"
             "        float3(0.0f, 0.0f, 1.0f),\n"
             "    };\n"
             "    uint i = vid % 3u;\n"
             "    BaryVOut o;\n"
             "    o.pos = clip[i];\n"
             "    o.attrP = attrs[i];\n"
             "    o.attrNP = attrs[i];\n"
             "    return o;\n"
             "}\n"
             "\n"
             "fragment float4 fs_bary(BaryVOut in [[stage_in]],\n"
             "                        float3 bcP [[barycentric_coord]],\n"
             "                        float3 bcNP [[barycentric_coord, center_no_perspective]],\n"
             "                        device BaryPixel *out [[buffer(0)]]) {\n"
             "    uint px = uint(in.pos.x);\n"
             "    uint py = uint(in.pos.y);\n"
             "    if (px < 16u && py < 16u) {\n"
             "        uint idx = py * 16u + px;\n"
             "        out[idx].bcP = float4(bcP, bcP.x + bcP.y + bcP.z);\n"
             "        out[idx].bcNP = float4(bcNP, bcNP.x + bcNP.y + bcNP.z);\n"
             "        out[idx].stdP = float4(in.attrP, 1.0f);\n"
             "        out[idx].stdNP = float4(in.attrNP, 1.0f);\n"
             "        out[idx].covered = 1u;\n"
             "    }\n"
             "    return float4(bcP, 1.0f);\n"
             "}\n"
             "\n"
             "struct PullSample {\n"
             "    float2 pos;\n"
             "    float2 npCenter;\n"
             "    float2 npCentroid;\n"
             "    float2 npSample;\n"
             "    float2 npOffA;\n"
             "    float2 npOffB;\n"
             "    float2 pCenter;\n"
             "    float2 pSample;\n"
             "    float2 pOffA;\n"
             "    float2 hOffA;\n"
             "    uint covered;\n"
             "    uint pad0;\n"
             "    uint pad1;\n"
             "    uint pad2;\n"
             "};\n"
             "\n"
             "struct PullVOut {\n"
             "    float4 pos [[position]];\n"
             "    float2 uv_p [[user(uv_p)]];\n"
             "    float2 uv_np [[user(uv_np)]];\n"
             "    half2 uv_h [[user(uv_h)]];\n"
             "};\n"
             "\n"
             "struct PullFIn {\n"
             "    interpolant<float2, interpolation::perspective> uv_p [[user(uv_p)]];\n"
             "    interpolant<float2, interpolation::no_perspective> uv_np [[user(uv_np)]];\n"
             "    interpolant<half2, interpolation::no_perspective> uv_h [[user(uv_h)]];\n"
             "};\n"
             "\n"
             "vertex PullVOut vs_pull(uint vid [[vertex_id]]) {\n"
             "    const float4 clip[3] = {\n"
             "        float4(-0.75f,  0.75f, 0.5f, 1.0f),\n"
             "        float4(-1.50f, -1.50f, 1.0f, 2.0f),\n"
             "        float4( 3.00f,  3.00f, 2.0f, 4.0f),\n"
             "    };\n"
             "    const float2 win[3] = {\n"
             "        float2( 2.0f,  2.0f),\n"
             "        float2( 2.0f, 14.0f),\n"
             "        float2(14.0f,  2.0f),\n"
             "    };\n"
             "    const float2 uv[3] = {\n"
             "        float2(0.0f, 0.0f),\n"
             "        float2(0.0f, 1.0f),\n"
             "        float2(1.0f, 0.0f),\n"
             "    };\n"
             "    uint i = vid % 3u;\n"
             "    PullVOut o;\n"
             "    o.pos = clip[i];\n"
             "    o.uv_p = uv[i];\n"
             "    o.uv_np = win[i];\n"
             "    o.uv_h = half2(win[i]);\n"
             "    return o;\n"
             "}\n"
             "\n"
             "fragment float4 fs_pull(PullFIn in [[stage_in]],\n"
             "                        float4 pos [[position]],\n"
             "                        uint sid [[sample_id]],\n"
             "                        device PullSample *out [[buffer(0)]]) {\n"
             "    uint px = uint(pos.x);\n"
             "    uint py = uint(pos.y);\n"
             "    if (px < 16u && py < 16u && sid < 4u) {\n"
             "        uint idx = (py * 16u + px) * 4u + sid;\n"
             "        out[idx].pos = pos.xy;\n"
             "        out[idx].npCenter = in.uv_np.interpolate_at_center();\n"
             "        out[idx].npCentroid = in.uv_np.interpolate_at_centroid();\n"
             "        out[idx].npSample = in.uv_np.interpolate_at_sample(sid);\n"
             "        out[idx].npOffA = in.uv_np.interpolate_at_offset(float2(0.25f, 0.75f));\n"
             "        out[idx].npOffB = in.uv_np.interpolate_at_offset(float2(0.75f, 0.25f));\n"
             "        out[idx].pCenter = in.uv_p.interpolate_at_center();\n"
             "        out[idx].pSample = in.uv_p.interpolate_at_sample(sid);\n"
             "        out[idx].pOffA = in.uv_p.interpolate_at_offset(float2(0.25f, 0.75f));\n"
             "        out[idx].hOffA = float2(in.uv_h.interpolate_at_offset(float2(0.25f, 0.75f)));\n"
             "        out[idx].covered = 1u;\n"
             "    }\n"
             "    return float4(0.0f, 1.0f, 0.0f, 1.0f);\n"
             "}\n";

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"library compile: %@", err]);

        // Case 1: [[barycentric_coord]] (perspective + center_no_perspective).
        if (hasBary) {
            MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
            rpd.vertexFunction = [lib newFunctionWithName:@"vs_bary"];
            rpd.fragmentFunction = [lib newFunctionWithName:@"fs_bary"];
            rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            require(rpd.vertexFunction != nil && rpd.fragmentFunction != nil, @"bary functions missing");
            id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
            require(pso != nil, [NSString stringWithFormat:@"bary pipeline: %@", err]);

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:kW height:kH mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            require(rt != nil, @"bary RT nil");

            NSUInteger payloadBytes = (NSUInteger)(kW * kH) * sizeof(BaryPixel);
            id<MTLBuffer> outBuf = [dev newBufferWithLength:payloadBytes + 2 * kGuardBytes
                                                    options:MTLResourceStorageModeShared];
            require(outBuf != nil, @"bary outBuf nil");
            initGuardedBuffer(outBuf, payloadBytes);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            require(enc != nil, @"bary encoder nil");
            [enc setViewport:(MTLViewport){ 0.0, 0.0, (double)kW, (double)kH, 0.0, 1.0 }];
            [enc setCullMode:MTLCullModeNone];
            [enc setRenderPipelineState:pso];
            [enc setFragmentBuffer:outBuf offset:kGuardBytes atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            finishOK(cb, @"bary_draw");

            checkGuards(outBuf, payloadBytes, @"bary");
            const BaryPixel *pxs = (const BaryPixel *)((const uint8_t *)outBuf.contents + kGuardBytes);
            int interiorChecked = 0;
            int exteriorChecked = 0;
            for (int y = 0; y < kH; y++) {
                for (int x = 0; x < kW; x++) {
                    const BaryPixel *p = &pxs[y * kW + x];
                    if (x >= 3 && y >= 3 && x + y <= 13) {
                        require(p->covered == 1u,
                                [NSString stringWithFormat:@"bary interior (%d,%d) uncovered", x, y]);
                        float expNP[3] = {0}, expP[3] = {0};
                        analyticalBary((float)x + 0.5f, (float)y + 0.5f, expNP, expP);
                        require(fabsf(p->bcP[3] - 1.0f) < 2e-3f && fabsf(p->bcNP[3] - 1.0f) < 2e-3f,
                                [NSString stringWithFormat:@"bary (%d,%d) sumP=%g sumNP=%g", x, y, p->bcP[3], p->bcNP[3]]);
                        checkFloat3(p->bcP, expP, 2e-3f,
                                    [NSString stringWithFormat:@"bary persp (%d,%d)", x, y]);
                        checkFloat3(p->bcNP, expNP, 2e-3f,
                                    [NSString stringWithFormat:@"bary nopersp (%d,%d)", x, y]);
                        checkFloat3(p->bcP, p->stdP, 2e-3f,
                                    [NSString stringWithFormat:@"bary persp vs std (%d,%d)", x, y]);
                        checkFloat3(p->bcNP, p->stdNP, 2e-3f,
                                    [NSString stringWithFormat:@"bary nopersp vs std (%d,%d)", x, y]);
                        interiorChecked++;
                    } else if (x <= 0 || y <= 0 || x + y >= 18) {
                        require(p->covered == 0u,
                                [NSString stringWithFormat:@"bary exterior (%d,%d) unexpectedly covered", x, y]);
                        exteriorChecked++;
                    }
                }
            }
            const BaryPixel *mid = &pxs[5 * kW + 5];
            require(fabsf(mid->bcP[0] - mid->bcNP[0]) > 0.15f,
                    [NSString stringWithFormat:@"bary (5,5) persp=%g nopersp=%g failed to diverge",
                     mid->bcP[0], mid->bcNP[0]]);
            require(interiorChecked == 36 && exteriorChecked > 0,
                    [NSString stringWithFormat:@"bary coverage count interior=%d exterior=%d",
                     interiorChecked, exteriorChecked]);
            printf("METAL3_BARY_PULL_CASE_PASS barycentric interior=%d exterior=%d diff55=%.4f\n",
                   interiorChecked, exteriorChecked, fabsf(mid->bcP[0] - mid->bcNP[0]));
        }

        // Case 2: Pull-model interpolant<T> at 4x MSAA.
        if (hasPull) {
            MTLSamplePosition defPos[4] = { {0, 0}, {0, 0}, {0, 0}, {0, 0} };
            [dev getDefaultSamplePositions:defPos count:4];
            for (int s = 0; s < 4; s++) {
                require(defPos[s].x > 0.0f && defPos[s].x < 1.0f && defPos[s].y > 0.0f && defPos[s].y < 1.0f,
                        [NSString stringWithFormat:@"default sample pos[%d] out of (0,1): (%g,%g)",
                         s, defPos[s].x, defPos[s].y]);
            }

            MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
            rpd.vertexFunction = [lib newFunctionWithName:@"vs_pull"];
            rpd.fragmentFunction = [lib newFunctionWithName:@"fs_pull"];
            rpd.rasterSampleCount = 4;
            rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            require(rpd.vertexFunction != nil && rpd.fragmentFunction != nil, @"pull functions missing");
            id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
            require(pso != nil, [NSString stringWithFormat:@"pull pipeline: %@", err]);

            MTLTextureDescriptor *msd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:kW height:kH mipmapped:NO];
            msd.textureType = MTLTextureType2DMultisample;
            msd.sampleCount = 4;
            msd.usage = MTLTextureUsageRenderTarget;
            msd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> msrt = [dev newTextureWithDescriptor:msd];
            require(msrt != nil, @"pull MSAA RT nil");

            MTLTextureDescriptor *resd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                            width:kW height:kH mipmapped:NO];
            resd.usage = MTLTextureUsageRenderTarget;
            resd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> resolved = [dev newTextureWithDescriptor:resd];
            require(resolved != nil, @"pull resolved RT nil");

            NSUInteger payloadBytes = (NSUInteger)(kW * kH * 4) * sizeof(PullSample);
            id<MTLBuffer> outBuf = [dev newBufferWithLength:payloadBytes + 2 * kGuardBytes
                                                    options:MTLResourceStorageModeShared];
            require(outBuf != nil, @"pull outBuf nil");
            initGuardedBuffer(outBuf, payloadBytes);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = msrt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            rp.colorAttachments[0].resolveTexture = resolved;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            require(enc != nil, @"pull encoder nil");
            [enc setViewport:(MTLViewport){ 0.0, 0.0, (double)kW, (double)kH, 0.0, 1.0 }];
            [enc setCullMode:MTLCullModeNone];
            [enc setRenderPipelineState:pso];
            [enc setFragmentBuffer:outBuf offset:kGuardBytes atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            finishOK(cb, @"pull_msaa4_draw");

            checkGuards(outBuf, payloadBytes, @"pull");
            const PullSample *samples = (const PullSample *)((const uint8_t *)outBuf.contents + kGuardBytes);

            int interiorSamples = 0;
            int partialEdgePixels = 0;
            for (int y = 0; y < kH; y++) {
                for (int x = 0; x < kW; x++) {
                    if (x >= 3 && y >= 3 && x + y <= 13) {
                        float meanX = 0.0f, meanY = 0.0f;
                        float sPos[4][2];
                        float expNP[3] = {0}, expP[3] = {0};
                        analyticalBary((float)x + 0.5f, (float)y + 0.5f, expNP, expP);
                        float expOffNP[3] = {0}, expOffP[3] = {0};
                        analyticalBary((float)x + 0.25f, (float)y + 0.75f, expOffNP, expOffP);

                        for (int s = 0; s < 4; s++) {
                            const PullSample *ps = &samples[(y * kW + x) * 4 + s];
                            require(ps->covered == 1u,
                                    [NSString stringWithFormat:@"pull interior (%d,%d) s=%d uncovered", x, y, s]);
                            checkFloat2(ps->npCenter, (float)x + 0.5f, (float)y + 0.5f, 2e-3f,
                                        [NSString stringWithFormat:@"pull npCenter (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->npCentroid, (float)x + 0.5f, (float)y + 0.5f, 2e-3f,
                                        [NSString stringWithFormat:@"pull npCentroid interior (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->npSample, ps->pos[0], ps->pos[1], 2e-3f,
                                        [NSString stringWithFormat:@"pull npSample vs pos (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->npOffA, (float)x + 0.25f, (float)y + 0.75f, 5e-3f,
                                        [NSString stringWithFormat:@"pull npOffA (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->npOffB, (float)x + 0.75f, (float)y + 0.25f, 5e-3f,
                                        [NSString stringWithFormat:@"pull npOffB (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->hOffA, (float)x + 0.25f, (float)y + 0.75f, 2e-2f,
                                        [NSString stringWithFormat:@"pull half2 offA (%d,%d) s=%d", x, y, s]);

                            // Perspective interpolant<float2> checks (uv = (b2, b1)).
                            checkFloat2(ps->pCenter, expP[2], expP[1], 2e-3f,
                                        [NSString stringWithFormat:@"pull pCenter (%d,%d) s=%d", x, y, s]);
                            checkFloat2(ps->pOffA, expOffP[2], expOffP[1], 5e-3f,
                                        [NSString stringWithFormat:@"pull pOffA (%d,%d) s=%d", x, y, s]);
                            float expSmpNP[3] = {0}, expSmpP[3] = {0};
                            analyticalBary(ps->pos[0], ps->pos[1], expSmpNP, expSmpP);
                            checkFloat2(ps->pSample, expSmpP[2], expSmpP[1], 2e-3f,
                                        [NSString stringWithFormat:@"pull pSample (%d,%d) s=%d", x, y, s]);

                            float fx = ps->npSample[0] - (float)x;
                            float fy = ps->npSample[1] - (float)y;
                            require(fx > 0.0f && fx < 1.0f && fy > 0.0f && fy < 1.0f,
                                    [NSString stringWithFormat:@"pull sample(%d) frac out of (0,1): (%g,%g)", s, fx, fy]);
                            sPos[s][0] = fx;
                            sPos[s][1] = fy;
                            meanX += fx;
                            meanY += fy;
                            interiorSamples++;
                        }
                        // All 4 MSAA samples must be distinct and average to pixel center (0.5, 0.5).
                        for (int a = 0; a < 4; a++) {
                            for (int b = a + 1; b < 4; b++) {
                                float dx = sPos[a][0] - sPos[b][0];
                                float dy = sPos[a][1] - sPos[b][1];
                                require(dx * dx + dy * dy > 1e-2f,
                                        [NSString stringWithFormat:@"pull (%d,%d) samples %d and %d collapsed", x, y, a, b]);
                            }
                        }
                        float avg[2] = { meanX * 0.25f, meanY * 0.25f };
                        checkFloat2(avg, 0.5f, 0.5f, 2e-3f,
                                    [NSString stringWithFormat:@"pull (%d,%d) 4-sample mean", x, y]);
                    } else if (x >= 3 && x <= 12 && x + y == 15) {
                        // Diagonal hypotenuse x + y == 16 cuts right through pixel center (x+0.5, y+0.5).
                        // Exactly the top-left subset of MSAA samples is covered, so centroid must shift
                        // strictly inside the triangle (centroid.x + centroid.y < 16.0 - 0.05).
                        int covCount = 0;
                        for (int s = 0; s < 4; s++) {
                            const PullSample *ps = &samples[(y * kW + x) * 4 + s];
                            if (ps->covered) {
                                covCount++;
                                require(ps->npCentroid[0] + ps->npCentroid[1] < 15.95f,
                                        [NSString stringWithFormat:@"pull edge (%d,%d) s=%d centroid (%g,%g) did not shift inside triangle",
                                         x, y, s, ps->npCentroid[0], ps->npCentroid[1]]);
                            }
                        }
                        require(covCount > 0 && covCount < 4,
                                [NSString stringWithFormat:@"pull edge (%d,%d) expected partial MSAA coverage, got %d/4",
                                 x, y, covCount]);
                        partialEdgePixels++;
                    } else if (x <= 0 || y <= 0 || x + y >= 18) {
                        for (int s = 0; s < 4; s++) {
                            const PullSample *ps = &samples[(y * kW + x) * 4 + s];
                            require(ps->covered == 0u,
                                    [NSString stringWithFormat:@"pull exterior (%d,%d) s=%d unexpectedly covered", x, y, s]);
                        }
                    }
                }
            }
            require(interiorSamples == 36 * 4 && partialEdgePixels == 10,
                    [NSString stringWithFormat:@"pull counts interiorSamples=%d partialEdgePixels=%d",
                     interiorSamples, partialEdgePixels]);
            printf("METAL3_BARY_PULL_CASE_PASS pull_msaa4 interiorSamples=%d partialEdgePixels=%d\n",
                   interiorSamples, partialEdgePixels);
        }

        puts("METAL3_BARY_PULL_PASS");
        return 0;
    }
}
