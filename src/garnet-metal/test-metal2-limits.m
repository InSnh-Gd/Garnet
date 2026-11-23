#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int gChecks, gFails;
static void check(BOOL cond, NSString *name) {
    gChecks++;
    if (!cond) {
        gFails++;
        fprintf(stderr, "METAL2_LIMITS_FAIL %s\n", name.UTF8String);
    } else {
        printf("METAL2_LIMITS_PASS %s\n", name.UTF8String);
    }
}

static void finish(id<MTLCommandBuffer> cb, NSString *op, BOOL expectFail) {
    if (!cb) {
        check(expectFail, [NSString stringWithFormat:@"%@ nil command buffer", op]);
        return;
    }
    [cb commit];
    [cb waitUntilCompleted];
    if (expectFail) {
        check(cb.status == MTLCommandBufferStatusError || cb.error != nil,
              [NSString stringWithFormat:@"%@ rejected cleanly", op]);
    } else {
        check(cb.status == MTLCommandBufferStatusCompleted && cb.error == nil,
              [NSString stringWithFormat:@"%@ completed (%@)", op, cb.error]);
    }
}

static id<MTLLibrary> compileLib(id<MTLDevice> dev, NSString *src,
                                 MTLCompileOptions *opts, BOOL expectErr,
                                 NSString *tag) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (expectErr) {
        check(lib == nil || err != nil, [NSString stringWithFormat:@"%@ rejected", tag]);
    } else {
        check(lib != nil && err == nil,
              [NSString stringWithFormat:@"%@ compiled (%@)", tag, err]);
    }
    return lib;
}

static void readTexRGBA8(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> tex,
                         uint32_t *outPixel, NSUInteger x, NSUInteger y) {
    id<MTLBuffer> buf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(1, 1, 1)
                 toBuffer:buf destinationOffset:0
   destinationBytesPerRow:256 destinationBytesPerImage:256];
    [blit endEncoding];
    finish(cb, @"readTexRGBA8", NO);
    *outPixel = *(const uint32_t *)buf.contents;
}

static BOOL callBool0(id obj, SEL sel) {
    return [obj respondsToSelector:sel] && ((BOOL (*)(id, SEL))objc_msgSend)(obj, sel);
}

static BOOL callBool1(id obj, SEL sel, NSUInteger arg) {
    return [obj respondsToSelector:sel] && ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(obj, sel, arg);
}

static BOOL callBoolInt(id obj, SEL sel, NSInteger arg) {
    return [obj respondsToSelector:sel] && ((BOOL (*)(id, SEL, NSInteger))objc_msgSend)(obj, sel, arg);
}

static NSUInteger callUInt1(id obj, SEL sel, MTLPixelFormat pf) {
    return [obj respondsToSelector:sel] ? ((NSUInteger (*)(id, SEL, MTLPixelFormat))objc_msgSend)(obj, sel, pf) : 0;
}

static void testCapabilityQueries(id<MTLDevice> dev) {
    // Family & FeatureSet queries (numeric IDs avoid SDK availability errors on -Werror)
    check(callBoolInt(dev, @selector(supportsFamily:), 2002 /* Mac2 */),
          @"query supportsFamily:Mac2 == YES");
    check(callBoolInt(dev, @selector(supportsFamily:), 3001 /* Common1 */) &&
          callBoolInt(dev, @selector(supportsFamily:), 3002 /* Common2 */) &&
          callBoolInt(dev, @selector(supportsFamily:), 3003 /* Common3 */),
          @"query supportsFamily:Common1..3 == YES");
    check(!callBoolInt(dev, @selector(supportsFamily:), 1001 /* Apple1 */) &&
          !callBoolInt(dev, @selector(supportsFamily:), 1008 /* Apple8 */) &&
          !callBoolInt(dev, @selector(supportsFamily:), 2001 /* Mac1 */) &&
          !callBoolInt(dev, @selector(supportsFamily:), 5001 /* Metal3 */) &&
          !callBoolInt(dev, @selector(supportsFamily:), 5002 /* Metal4 */) &&
          !callBoolInt(dev, @selector(supportsFamily:), 0),
          @"query out-of-scope families == NO");

    check(callBool1(dev, @selector(supportsFeatureSet:), 10000) &&
          callBool1(dev, @selector(supportsFeatureSet:), 10001) &&
          callBool1(dev, @selector(supportsFeatureSet:), 10002) &&
          callBool1(dev, @selector(supportsFeatureSet:), 10003) &&
          callBool1(dev, @selector(supportsFeatureSet:), 10004) &&
          callBool1(dev, @selector(supportsFeatureSet:), 10005),
          @"query supportsFeatureSet:macOS 10000..10005 == YES");
    check(!callBool1(dev, @selector(supportsFeatureSet:), 0) &&
          !callBool1(dev, @selector(supportsFeatureSet:), 1) &&
          !callBool1(dev, @selector(supportsFeatureSet:), 10006) &&
          !callBool1(dev, @selector(supportsFeatureSet:), 30000),
          @"query non-macOS feature sets == NO");
    check(callBool1(dev, @selector(deviceSupportsFeatureSet:), 10005) &&
          !callBool1(dev, @selector(deviceSupportsFeatureSet:), 10006),
          @"query deviceSupportsFeatureSet: matches supportsFeatureSet:");

    // Core Metal 2 Mac2 capability queries
    check(dev.argumentBuffersSupport == MTLArgumentBuffersTier2,
          @"query argumentBuffersSupport == Tier2");
    check(dev.readWriteTextureSupport == MTLReadWriteTextureTier2,
          @"query readWriteTextureSupport == Tier2");
    check(callBool0(dev, @selector(supportsBCTextureCompression)), @"query supportsBCTextureCompression == YES");
    check(callBool0(dev, @selector(supports32BitFloatFiltering)), @"query supports32BitFloatFiltering == YES");
    check(callBool0(dev, @selector(supports32BitMSAA)), @"query supports32BitMSAA == YES");
    check(callBool0(dev, @selector(areRasterOrderGroupsSupported)), @"query areRasterOrderGroupsSupported == YES");
    check([dev areProgrammableSamplePositionsSupported],
          @"query areProgrammableSamplePositionsSupported == YES");
    check(![dev isDepth24Stencil8PixelFormatSupported],
          @"query isDepth24Stencil8PixelFormatSupported == NO");
    check(callBool0(dev, @selector(supportsQueryTextureLOD)), @"query supportsQueryTextureLOD == YES");
    check(callBool0(dev, @selector(supportsUnalignedVertexFetch)), @"query supportsUnalignedVertexFetch == YES");
    check([dev supportsTextureSampleCount:1] && [dev supportsTextureSampleCount:2] &&
          [dev supportsTextureSampleCount:4] && [dev supportsTextureSampleCount:8] &&
          ![dev supportsTextureSampleCount:0] && ![dev supportsTextureSampleCount:3] &&
          ![dev supportsTextureSampleCount:16],
          @"query supportsTextureSampleCount:{1,2,4,8} vs invalid");
    check(callBool1(dev, @selector(supportsVertexAmplificationCount:), 1) &&
          !callBool1(dev, @selector(supportsVertexAmplificationCount:), 2) &&
          !callBool1(dev, @selector(supportsVertexAmplificationCount:), 0),
          @"query supportsVertexAmplificationCount:(1->YES, others->NO)");

    MTLSamplePosition pos[8] = {{0}};
    [dev getDefaultSamplePositions:pos count:4];
    BOOL nonzero = NO;
    for (int i = 0; i < 4; i++) {
        if (pos[i].x > 0.0f && pos[i].x < 1.0f && pos[i].y > 0.0f && pos[i].y < 1.0f)
            nonzero = YES;
    }
    check(nonzero, @"query getDefaultSamplePositions:count:4 returns valid pattern");

    // Explicit out-of-scope NO overrides
    check(!callBool0(dev, @selector(supportsRaytracing)), @"query supportsRaytracing == NO");
    check(!callBool0(dev, @selector(supportsRaytracingFromRender)), @"query supportsRaytracingFromRender == NO");
    check(!callBool0(dev, @selector(supportsPrimitiveMotionBlur)), @"query supportsPrimitiveMotionBlur == NO");
    check(!callBool0(dev, @selector(supportsFunctionPointers)), @"query supportsFunctionPointers == NO");
    check(!callBool0(dev, @selector(supportsFunctionPointersFromRender)), @"query supportsFunctionPointersFromRender == NO");
    check(!callBool0(dev, @selector(supportsPullModelInterpolation)), @"query supportsPullModelInterpolation == NO");
    check(!callBool0(dev, @selector(supportsShaderBarycentricCoordinates)), @"query supportsShaderBarycentricCoordinates == NO");
    check(!callBool0(dev, @selector(supportsDynamicLibraries)) &&
          !callBool0(dev, @selector(supportsRenderDynamicLibraries)) &&
          !callBool0(dev, @selector(supportsBinaryArchives)),
          @"query dynamic libraries & binary archives == NO");
    check(!dev.hasUnifiedMemory, @"query hasUnifiedMemory == NO");

    // Alignment & size properties (L07, L14, L22)
    check(dev.maxThreadgroupMemoryLength >= 32768, @"L07 maxThreadgroupMemoryLength >= 32768");
    MTLSize tgMax = dev.maxThreadsPerThreadgroup;
    check(tgMax.width >= 1024 && tgMax.height >= 1024 && tgMax.depth >= 64,
          @"L06 maxThreadsPerThreadgroup >= {1024,1024,64}");
    check(callUInt1(dev, @selector(minimumTextureBufferAlignmentForPixelFormat:), MTLPixelFormatR32Float) == 256,
          @"L14 minimumTextureBufferAlignment == 256");
    check(callUInt1(dev, @selector(minimumLinearTextureAlignmentForPixelFormat:), MTLPixelFormatRGBA8Unorm) == 256,
          @"L14 minimumLinearTextureAlignment == 256");
    NSUInteger iosAlign = [dev respondsToSelector:@selector(iosurfaceTextureAlignmentBytes)]
        ? ((NSUInteger (*)(id, SEL))objc_msgSend)(dev, @selector(iosurfaceTextureAlignmentBytes))
        : 0;
    check(iosAlign == 32, @"iosurfaceTextureAlignmentBytes == 32");
    check(dev.maxBufferLength > 0 && dev.recommendedMaxWorkingSetSize >= dev.maxBufferLength,
          @"L22 maxBufferLength & recommendedMaxWorkingSetSize valid");
}

static void testComputeAndFunctionConstantLimits(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    // L02 (128 tex), L03 (16 smp), L05 (14 inline cb), L06/L07 (1024 threads, 32 KiB TG),
    // L08 (64 KiB constant space), L10 (65536 function constants)
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "constant uint kFcHi [[function_constant(65535)]];\n"
         "constant uint4 kConstBig[4096] = { uint4(0x12345678u, 0, 0, 0) };\n"
         "kernel void k_fc_and_tg(device uint *out [[buffer(0)]],\n"
         "                        constant uint *c30 [[buffer(30)]],\n"
         "                        uint tid [[thread_position_in_threadgroup]]) {\n"
         "  threadgroup uint tg[8192];\n"
         "  uint i = tid;\n"
         "  while (i < 8192) { tg[i] = i + kFcHi; i += 1024; }\n"
         "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
         "  if (tid == 0) {\n"
         "    out[0] = tg[8191];\n"
         "    out[1] = kConstBig[0].x ^ c30[0];\n"
         "    out[2] = kFcHi;\n"
         "  }\n"
         "}\n";
    MTLFunctionConstantValues *fcv = [MTLFunctionConstantValues new];
    uint32_t fcVal = 0x55AA1234u;
    [fcv setConstantValue:&fcVal type:MTLDataTypeUInt atIndex:65535];
    id<MTLLibrary> lib = compileLib(dev, src, nil, NO, @"compute-fc-tg");
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:@"k_fc_and_tg" constantValues:fcv error:&err];
    check(fn != nil && err == nil, [NSString stringWithFormat:@"L10 specialize index 65535 (%@)", err]);
    if (!fn) return;
    id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:fn error:&err];
    check(cps != nil && err == nil && cps.threadExecutionWidth == 32 &&
          cps.maxTotalThreadsPerThreadgroup <= 1024,
          [NSString stringWithFormat:@"L06/L07 CPS (tew=%lu max=%lu err=%@)",
           (unsigned long)cps.threadExecutionWidth,
           (unsigned long)cps.maxTotalThreadsPerThreadgroup, err]);
    if (!cps) return;

    id<MTLBuffer> outBuf = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    uint32_t tag30 = 0x0F0F0F0Fu;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:cps];
    [ce setBuffer:outBuf offset:0 atIndex:0];
    [ce setBytes:&tag30 length:sizeof(tag30) atIndex:30];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(1024, 1, 1)];
    [ce endEncoding];
    finish(cb, @"L06/L07/L08/L10 compute 1024 threads + 32KiB TG + FC 65535", NO);
    const uint32_t *res = (const uint32_t *)outBuf.contents;
    check(res[0] == 8191u + fcVal && res[1] == (0x12345678u ^ tag30) && res[2] == fcVal,
          [NSString stringWithFormat:@"L07/L08/L10 outputs matched (0x%x 0x%x 0x%x)",
           res[0], res[1], res[2]]);

    // L01..L03 negative slot boundaries (compute buffer[31], tex[128], smp[16])
    id<MTLCommandBuffer> badBufCb = [q commandBuffer];
    id<MTLComputeCommandEncoder> badCe = [badBufCb computeCommandEncoder];
    [badCe setComputePipelineState:cps];
    [badCe setBuffer:outBuf offset:0 atIndex:31];
    [badCe endEncoding];
    finish(badBufCb, @"L01 compute buffer index 31", YES);

    id<MTLCommandBuffer> badTexCb = [q commandBuffer];
    id<MTLComputeCommandEncoder> badTexCe = [badTexCb computeCommandEncoder];
    [badTexCe setTexture:nil atIndex:128];
    [badTexCe endEncoding];
    finish(badTexCb, @"L02 compute texture index 128", YES);

    id<MTLCommandBuffer> badSmpCb = [q commandBuffer];
    id<MTLComputeCommandEncoder> badSmpCe = [badSmpCb computeCommandEncoder];
    [badSmpCe setSamplerState:nil atIndex:16];
    [badSmpCe endEncoding];
    finish(badSmpCb, @"L03 compute sampler index 16", YES);
}

static void testRenderAndTextureLimits(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    // L15..L17 Texture dimensions and Depth24 negative
    MTLTextureDescriptor *d16k = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:16384 height:1 mipmapped:NO];
    d16k.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t16k = [dev newTextureWithDescriptor:d16k];
    check(t16k != nil && t16k.width == 16384, @"L15 16384x1 2D texture supported");

    MTLTextureDescriptor *dTooWide = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:16385 height:1 mipmapped:NO];
    check([dev newTextureWithDescriptor:dTooWide] == nil, @"L15 16385 width rejected");

    MTLTextureDescriptor *d3d = [MTLTextureDescriptor new];
    d3d.textureType = MTLTextureType3D; d3d.pixelFormat = MTLPixelFormatR8Unorm;
    d3d.width = 2048; d3d.height = 4; d3d.depth = 4;
    check([dev newTextureWithDescriptor:d3d] != nil, @"L16 2048 3D texture supported");
    d3d.depth = 2049;
    check([dev newTextureWithDescriptor:d3d] == nil, @"L16 2049 3D texture rejected");

    MTLTextureDescriptor *dArr = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                    width:4 height:4 mipmapped:NO];
    dArr.textureType = MTLTextureType2DArray; dArr.arrayLength = 2048;
    check([dev newTextureWithDescriptor:dArr] != nil, @"L17 2048 array layers supported");
    dArr.arrayLength = 2049;
    check([dev newTextureWithDescriptor:dArr] == nil, @"L17 2049 array layers rejected");

    MTLTextureDescriptor *d24 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth24Unorm_Stencil8
                                                                                   width:16 height:16 mipmapped:NO];
    check([dev newTextureWithDescriptor:d24] == nil, @"F30/L23 Depth24Stencil8 rejected");

    // L12 Viewports/scissors (16), L18 8 Color attachments, L19 point size 511, L01/L02/L03/L05/L09
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; float psize [[point_size]]; };\n"
         "vertex VOut vs_pts(constant float *c0 [[buffer(0)]],\n"
         "                   constant float *c30 [[buffer(30)]],\n"
         "                   texture2d<float> t0 [[texture(0)]],\n"
         "                   texture2d<float> t127 [[texture(127)]],\n"
         "                   sampler s0 [[sampler(0)]],\n"
         "                   sampler s15 [[sampler(15)]]) {\n"
         "  VOut o;\n"
         "  o.pos = float4(0.0, 0.0, 0.0, 1.0);\n"
         "  float v = t0.sample(s0, float2(0.5)).x + t127.sample(s15, float2(0.5)).x;\n"
         "  o.psize = c0[0] + c30[0] + 0.0 * v;\n"
         "  return o;\n"
         "}\n"
         "struct MRTOut {\n"
         "  float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]];\n"
         "  float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; float4 c6 [[color(6)]]; float4 c7 [[color(7)]];\n"
         "};\n"
         "fragment MRTOut fs_mrt8(constant float4 *col [[buffer(13)]]) {\n"
         "  MRTOut o;\n"
         "  o.c0 = col[0] * float4(1,0,0,1); o.c1 = float4(0,1,0,1);\n"
         "  o.c2 = float4(0,0,1,1); o.c3 = float4(1,1,0,1);\n"
         "  o.c4 = float4(1,0,1,1); o.c5 = float4(0,1,1,1);\n"
         "  o.c6 = float4(0.5,0.5,0.5,1); o.c7 = col[0];\n"
         "  return o;\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, nil, NO, @"limits-render");
    if (!lib) return;

    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [lib newFunctionWithName:@"vs_pts"];
    rpd.fragmentFunction = [lib newFunctionWithName:@"fs_mrt8"];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:512 height:512 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> rts[8];
    for (int i = 0; i < 8; i++) {
        rts[i] = [dev newTextureWithDescriptor:td];
        rpd.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA8Unorm;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
    check(rps != nil && err == nil,
          [NSString stringWithFormat:@"L01/L02/L03/L18/L19 render PSO (%@)", err]);
    if (!rps) return;

    MTLTextureDescriptor *smTd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:4 height:4 mipmapped:NO];
    smTd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> dummyTex = [dev newTextureWithDescriptor:smTd];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 8; i++) {
        rp.colorAttachments[i].texture = rts[i];
        rp.colorAttachments[i].loadAction = MTLLoadActionClear;
        rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    }

    MTLViewport vps[16];
    MTLScissorRect srs[16];
    for (int i = 0; i < 16; i++) {
        vps[i] = (MTLViewport){0.0, 0.0, 512.0, 512.0, 0.0, 1.0};
        srs[i] = (MTLScissorRect){0, 0, 512, 512};
    }

    float p0 = 256.0f, p30 = 255.0f; // L19 maxPointSize = 511.0
    uint8_t inline4k[4096];
    memset(inline4k, 0, sizeof(inline4k));
    float rgba[4] = {1.0f, 0.25f, 0.5f, 1.0f};
    memcpy(inline4k, rgba, sizeof(rgba));

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:rps];
    [re setViewports:vps count:16];
    [re setScissorRects:srs count:16];
    [re setVertexBytes:&p0 length:sizeof(p0) atIndex:0];
    [re setVertexBytes:&p30 length:sizeof(p30) atIndex:30];
    [re setVertexTexture:dummyTex atIndex:0];
    [re setVertexTexture:dummyTex atIndex:127];
    [re setVertexSamplerState:smp atIndex:0];
    [re setVertexSamplerState:smp atIndex:15];
    // L05: 14 distinct 4 KiB inline constant buffers
    for (NSUInteger i = 0; i < 14; i++)
        [re setFragmentBytes:inline4k length:sizeof(inline4k) atIndex:i];
    [re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
    [re endEncoding];
    finish(cb, @"L01..L05/L12/L18/L19 render draw", NO);

    uint32_t corner0 = 0, farCorner7 = 0;
    // Point size 511 centered at (256, 256) covers (10, 10) and (500, 500)
    readTexRGBA8(dev, q, rts[0], &corner0, 10, 10);
    readTexRGBA8(dev, q, rts[7], &farCorner7, 500, 500);
    check(corner0 == 0xFF0000FFu && (farCorner7 & 0xFF0000FFu) == 0xFF0000FFu,
          [NSString stringWithFormat:@"L18/L19 MRT8 + point size 511 pixels (rt0=0x%08x rt7=0x%08x)",
           corner0, farCorner7]);

    // L12 out-of-range viewport count 17 rejection
    id<MTLCommandBuffer> badVp = [q commandBuffer];
    id<MTLRenderCommandEncoder> badRe = [badVp renderCommandEncoderWithDescriptor:rp];
    MTLViewport vps17[17];
    for (int i = 0; i < 17; i++) vps17[i] = vps[0];
    [badRe setViewports:vps17 count:17];
    [badRe endEncoding];
    finish(badVp, @"L12 setViewports count 17", YES);

    // L13 raster_order_group(8) compiler refusal
    NSString *badRogSrc =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "fragment float4 fs_bad_rog(device uint *b [[buffer(0), raster_order_group(8)]]) {\n"
         "  return float4(float(b[0]), 0, 0, 1);\n"
         "}\n";
    compileLib(dev, badRogSrc, nil, YES, @"L13 raster_order_group(8)");

    // L21 32768 fence objects without failure
    @autoreleasepool {
        BOOL fenceOk = YES;
        for (int i = 0; i < 32768; i++) {
            @autoreleasepool {
                id<MTLFence> f = [dev newFence];
                if (!f) { fenceOk = NO; break; }
            }
        }
        check(fenceOk, @"L21 32768 fence objects created cleanly");
    }
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        check(dev != nil, @"MTLCreateSystemDefaultDevice != nil");
        if (!dev) return 1;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        check(q != nil, @"newCommandQueue != nil");
        testCapabilityQueries(dev);
        testComputeAndFunctionConstantLimits(dev, q);
        testRenderAndTextureLimits(dev, q);
        printf("METAL2_LIMITS_SUMMARY checks=%d fails=%d\n", gChecks, gFails);
        return gFails ? 1 : 0;
    }
}
