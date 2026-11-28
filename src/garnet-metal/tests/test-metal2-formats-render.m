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

static int anyFail;
static BOOL softFail;
static void fail(NSString *message) {
    fprintf(stderr, "METAL2_81_FAIL %s\n", message.UTF8String);
    anyFail = 1;
    if (!softFail) _exit(1);
}
static void passCase(NSString *name) {
    printf("METAL2_81_PASS %s\n", name.UTF8String);
}

static void finish(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ nil command buffer", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@", op, (unsigned long)cb.status, cb.error]);
}

static id<MTLLibrary> compileLib(id<MTLDevice> dev, NSString *src, NSString *tag) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) fail([NSString stringWithFormat:@"compile %@: %@", tag, err]);
    return lib;
}

static void readTexBytes(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> tex,
                         NSUInteger slice, NSUInteger level, NSUInteger bpr, void *out, NSUInteger len) {
    id<MTLBuffer> buf = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    NSUInteger w = MAX((NSUInteger)1, tex.width >> level);
    NSUInteger h = MAX((NSUInteger)1, tex.height >> level);
    [blit copyFromTexture:tex sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
     destinationBytesPerRow:bpr destinationBytesPerImage:len];
    [blit endEncoding];
    finish(cb, @"readTexBytes");
    memcpy(out, buf.contents, len);
}

static void testUncompressedFormatsAndIntClears(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; float2 uv; };\n"
         "vertex VOut vs_quad(uint vid [[vertex_id]]) {\n"
         "  float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
         "  VOut o; o.pos = float4(p[vid], 0, 1); o.uv = float2(0.5, 0.5); return o;\n"
         "}\n"
         "fragment float4 fs_sample_f(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
         "  constexpr sampler s(filter::nearest);\n"
         "  return t.sample(s, in.uv);\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"fmt");
    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [lib newFunctionWithName:@"vs_quad"];
    rpd.fragmentFunction = [lib newFunctionWithName:@"fs_sample_f"];
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError *err = nil;
    id<MTLRenderPipelineState> samplePso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
    if (!samplePso) fail([NSString stringWithFormat:@"samplePso: %@", err]);

    MTLTextureDescriptor *rtDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
    rtDesc.usage = MTLTextureUsageRenderTarget;
    rtDesc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtDesc];

    struct NormCase { MTLPixelFormat pf; uint32_t bpp; uint8_t bytes[16]; float exp[4]; const char *name; } normCases[] = {
        { MTLPixelFormatR8Snorm, 1, { 0x40 }, { 64.0f/127.0f, 0, 0, 1 }, "R8Snorm" },
        { MTLPixelFormatRG8Snorm, 2, { 0x40, 0x81 }, { 64.0f/127.0f, -1.0f, 0, 1 }, "RG8Snorm" },
        { MTLPixelFormatRGBA8Snorm, 4, { 0x40, 0xC0, 0x7F, 0x80 }, { 64.0f/127.0f, -64.0f/127.0f, 1.0f, -1.0f }, "RGBA8Snorm" },
        { MTLPixelFormatR16Snorm, 2, { 0xFF, 0x3F }, { 16383.0f/32767.0f, 0, 0, 1 }, "R16Snorm" },
        { MTLPixelFormatRG16Snorm, 4, { 0xFF, 0x3F, 0x01, 0x80 }, { 16383.0f/32767.0f, -1.0f, 0, 1 }, "RG16Snorm" },
        { MTLPixelFormatRGBA16Snorm, 8, { 0xFF, 0x3F, 0x00, 0xC0, 0xFF, 0x7F, 0x01, 0x80 }, { 16383.0f/32767.0f, -16384.0f/32767.0f, 1.0f, -1.0f }, "RGBA16Snorm" },
    };
    for (size_t i = 0; i < sizeof normCases / sizeof *normCases; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:normCases[i].pf width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!tex) fail([NSString stringWithFormat:@"create %s failed", normCases[i].name]);
        uint8_t rowBuf[4 * 4 * 16] = {0};
        for (int p = 0; p < 16; p++) memcpy(rowBuf + p * normCases[i].bpp, normCases[i].bytes, normCases[i].bpp);
        [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:rowBuf bytesPerRow:4 * normCases[i].bpp];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:samplePso];
        [enc setFragmentTexture:tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @(normCases[i].name));
        float out[4 * 4 * 4] = {0};
        readTexBytes(dev, q, rt, 0, 0, 4 * 16, out, sizeof out);
        for (int c = 0; c < 4; c++) {
            if (fabsf(out[c] - normCases[i].exp[c]) > 0.02f)
                fail([NSString stringWithFormat:@"%s ch %d got %g expected %g", normCases[i].name, c, out[c], normCases[i].exp[c]]);
        }
    }

    // RGB9E5Float: exp=16 (biased 15+1 -> scale 2^-4 = 1/16 * 512.. wait: val = mant * 2^(e - 15 - 9) = mant * 2^(exp-24)).
    // For 1.5 = 384 * 2^(16-24), 0.5 = 256 * 2^(15-24): exp=16 -> R=384, G=128, B=64 -> (384/256, 128/256, 64/256) = (1.5, 0.5, 0.25).
    {
        uint32_t e5 = (16u << 27) | (64u << 18) | (128u << 9) | 384u;
        uint32_t pixels[16]; for (int i = 0; i < 16; i++) pixels[i] = e5;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB9E5Float width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!tex) fail(@"RGB9E5Float create failed");
        [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:samplePso];
        [enc setFragmentTexture:tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @"RGB9E5Float");
        float out[64] = {0};
        readTexBytes(dev, q, rt, 0, 0, 64, out, sizeof out);
        if (fabsf(out[0] - 1.5f) > 0.01f || fabsf(out[1] - 0.5f) > 0.01f || fabsf(out[2] - 0.25f) > 0.01f)
            fail([NSString stringWithFormat:@"RGB9E5Float got (%g,%g,%g)", out[0], out[1], out[2]]);
    }

    // All new UInt/SInt formats + clear clamping verification (8, 10/2, 16, 32-bit)
    struct IntClearCase { MTLPixelFormat pf; uint32_t bpp; double r, g, b, a; uint32_t exp0, exp1; const char *name; } intCases[] = {
        { MTLPixelFormatR8Uint, 1, 299.9, 0, 0, 0, 255u, 0, "R8Uint" },
        { MTLPixelFormatR8Sint, 1, -200.0, 0, 0, 0, 0x80u, 0, "R8Sint" },
        { MTLPixelFormatRG8Uint, 2, 250.9, -3.0, 0, 0, 0x00FAu, 0, "RG8Uint" },
        { MTLPixelFormatRG8Sint, 2, -5.9, 200.0, 0, 0, (uint32_t)(uint8_t)(-5) | ((uint32_t)127 << 8), 0, "RG8Sint" },
        { MTLPixelFormatRGBA8Uint, 4, 255.9, 1.5, -1.0, 300.0, (255u) | (1u << 8) | (0u << 16) | (255u << 24), 0, "RGBA8Uint" },
        { MTLPixelFormatRGBA8Sint, 4, -5.9, 127.9, -200.0, 3.5, (uint32_t)(uint8_t)(-5) | (127u << 8) | (0x80u << 16) | (3u << 24), 0, "RGBA8Sint" },
        { MTLPixelFormatR16Uint, 2, 70000.0, 0, 0, 0, 65535u, 0, "R16Uint" },
        { MTLPixelFormatR16Sint, 2, -40000.0, 0, 0, 0, (uint16_t)(-32768), 0, "R16Sint" },
        { MTLPixelFormatRG16Sint, 4, -19.8, 40000.0, 0, 0, (uint16_t)(-19) | ((uint32_t)32767 << 16), 0, "RG16Sint" },
        { MTLPixelFormatRGB10A2Uint, 4, 1500.0, 42.9, -1.0, 9.0, 1023u | (42u << 10) | (0u << 20) | (3u << 30), 0, "RGB10A2Uint" },
        { MTLPixelFormatRG32Uint, 8, 70000.5, 4294967295.0, 0, 0, 70000u, 0xFFFFFFFFu, "RG32Uint" },
        { MTLPixelFormatRG32Sint, 8, -70000.5, 123456.9, 0, 0, (uint32_t)(-70000), 123456u, "RG32Sint" },
        { MTLPixelFormatRGBA16Uint, 8, 12.9, 70000.0, 3.5, -2.0, 12u | (65535u << 16), 3u, "RGBA16Uint" },
        { MTLPixelFormatRGBA16Sint, 8, -12.9, 40000.0, 3.5, -40000.0, (uint16_t)(-12) | ((uint32_t)32767 << 16), 3u | ((uint32_t)(uint16_t)(-32768) << 16), "RGBA16Sint" },
        { MTLPixelFormatRGBA32Uint, 16, 1.0, 70000.5, 3.0, 4.0, 1u, 70000u, "RGBA32Uint" },
        { MTLPixelFormatRGBA32Sint, 16, -70000.5, 99.9, -1.0, 5.0, (uint32_t)(-70000), 99u, "RGBA32Sint" },
    };
    for (size_t i = 0; i < sizeof intCases / sizeof *intCases; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:intCases[i].pf width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!tex) fail([NSString stringWithFormat:@"create %s failed", intCases[i].name]);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(intCases[i].r, intCases[i].g, intCases[i].b, intCases[i].a);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        finish(cb, @(intCases[i].name));
        uint32_t px[4 * 4 * 4] = {0};
        readTexBytes(dev, q, tex, 0, 0, 4 * intCases[i].bpp, px, 16 * intCases[i].bpp);
        if (intCases[i].bpp == 1) {
            if ((px[0] & 0xFFu) != intCases[i].exp0)
                fail([NSString stringWithFormat:@"%s got 0x%x expected 0x%x", intCases[i].name, px[0] & 0xFFu, intCases[i].exp0]);
        } else if (intCases[i].bpp == 2) {
            if ((px[0] & 0xFFFFu) != intCases[i].exp0)
                fail([NSString stringWithFormat:@"%s got 0x%x expected 0x%x", intCases[i].name, px[0] & 0xFFFFu, intCases[i].exp0]);
        } else if (intCases[i].bpp == 4) {
            if (px[0] != intCases[i].exp0)
                fail([NSString stringWithFormat:@"%s got 0x%x expected 0x%x", intCases[i].name, px[0], intCases[i].exp0]);
        } else {
            if (px[0] != intCases[i].exp0 || px[1] != intCases[i].exp1)
                fail([NSString stringWithFormat:@"%s got (%u,%u) expected (%u,%u)", intCases[i].name, px[0], px[1], intCases[i].exp0, intCases[i].exp1]);
        }
    }
    passCase(@"uncompressed-formats-and-int-clears");
}

static void testBCFormatsAnd3DBC(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; };\n"
         "vertex VOut vs_bc(uint vid [[vertex_id]]) {\n"
         "  float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
         "  VOut o; o.pos = float4(p[vid], 0, 1); return o;\n"
         "}\n"
         "fragment float4 fs_bc2d(texture2d<float> t [[texture(0)]]) {\n"
         "  constexpr sampler s(filter::nearest);\n"
         "  return t.sample(s, float2(0.5, 0.5));\n"
         "}\n"
         "fragment float4 fs_bc3d(texture3d<float> t [[texture(0)]], constant float &zcoord [[buffer(0)]]) {\n"
         "  constexpr sampler s(filter::nearest);\n"
         "  return t.sample(s, float3(0.5, 0.5, zcoord));\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"bc");
    MTLRenderPipelineDescriptor *rpd2 = [MTLRenderPipelineDescriptor new];
    rpd2.vertexFunction = [lib newFunctionWithName:@"vs_bc"];
    rpd2.fragmentFunction = [lib newFunctionWithName:@"fs_bc2d"];
    rpd2.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso2d = [dev newRenderPipelineStateWithDescriptor:rpd2 error:&err];
    if (!pso2d) fail([NSString stringWithFormat:@"pso2d: %@", err]);

    MTLRenderPipelineDescriptor *rpd3 = [MTLRenderPipelineDescriptor new];
    rpd3.vertexFunction = [lib newFunctionWithName:@"vs_bc"];
    rpd3.fragmentFunction = [lib newFunctionWithName:@"fs_bc3d"];
    rpd3.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pso3d = [dev newRenderPipelineStateWithDescriptor:rpd3 error:&err];
    if (!pso3d) fail([NSString stringWithFormat:@"pso3d: %@", err]);

    MTLTextureDescriptor *rtDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
    rtDesc.usage = MTLTextureUsageRenderTarget;
    rtDesc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtDesc];

    // All 14 BC formats + check isCompressed + upload + sample + tail mip
    MTLPixelFormat bcFmts[14] = {
        MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB,
        MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB,
        MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB,
        MTLPixelFormatBC4_RUnorm, MTLPixelFormatBC4_RSnorm,
        MTLPixelFormatBC5_RGUnorm, MTLPixelFormatBC5_RGSnorm,
        MTLPixelFormatBC6H_RGBFloat, MTLPixelFormatBC6H_RGBUfloat,
        MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB
    };
    for (int i = 0; i < 14; i++) {
        MTLPixelFormat pf = bcFmts[i];
        NSUInteger blockBytes = (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC1_RGBA_sRGB ||
                                 pf == MTLPixelFormatBC4_RUnorm || pf == MTLPixelFormatBC4_RSnorm) ? 8 : 16;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:8 height:8 mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!tex) fail([NSString stringWithFormat:@"BC format %lu create failed", (unsigned long)pf]);
        if (!((BOOL (*)(id, SEL))objc_msgSend)(tex, @selector(isCompressed)))
            fail([NSString stringWithFormat:@"BC format %lu isCompressed == NO", (unsigned long)pf]);

        uint8_t blk[4 * 16];
        memset(blk, 0, sizeof blk);
        if (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC1_RGBA_sRGB) {
            // color0 = 0x07E0 (pure green RGB565), color1 = 0x07E0, indices = 0 -> (0, 1, 0, 1)
            for (int b = 0; b < 4; b++) { blk[b*8+0] = 0xE0; blk[b*8+1] = 0x07; blk[b*8+2] = 0xE0; blk[b*8+3] = 0x07; }
        } else if (pf == MTLPixelFormatBC2_RGBA || pf == MTLPixelFormatBC2_RGBA_sRGB) {
            for (int b = 0; b < 4; b++) { memset(blk + b*16, 0xFF, 8); blk[b*16+8] = 0xE0; blk[b*16+9] = 0x07; blk[b*16+10] = 0xE0; blk[b*16+11] = 0x07; }
        } else if (pf == MTLPixelFormatBC3_RGBA || pf == MTLPixelFormatBC3_RGBA_sRGB) {
            for (int b = 0; b < 4; b++) { blk[b*16+0] = 0xFF; blk[b*16+1] = 0xFF; blk[b*16+8] = 0xE0; blk[b*16+9] = 0x07; blk[b*16+10] = 0xE0; blk[b*16+11] = 0x07; }
        } else if (pf == MTLPixelFormatBC4_RUnorm || pf == MTLPixelFormatBC5_RGUnorm) {
            for (int b = 0; b < 4; b++) { blk[b*blockBytes] = 0xFF; blk[b*blockBytes+1] = 0xFF; }
        } else if (pf == MTLPixelFormatBC4_RSnorm || pf == MTLPixelFormatBC5_RGSnorm) {
            for (int b = 0; b < 4; b++) { blk[b*blockBytes] = 0x7F; blk[b*blockBytes+1] = 0x7F; }
        } else if (pf == MTLPixelFormatBC6H_RGBFloat || pf == MTLPixelFormatBC6H_RGBUfloat) {
            // mode 3 (00011b), delta=0 -> endpoints 0 (zeros give 0, check no NaN/crash + exact zero readback);
            for (int b = 0; b < 4; b++) blk[b*16] = 0x03;
        } else {
            // BC7 mode 6 (bit 6 set = 0x40): 7-bit endpoints. Pure green/white check via constant solid block works cleanly, or check all-0xFF mode 6/7 -> just check deterministic block!
            // Actually BC7 byte[0]=0x40 followed by all 0xFF sets all endpoints to 0x7F -> outputs (1, 1, 1, 1)!
            for (int b = 0; b < 4; b++) { memset(blk + b*16, 0xFF, 16); blk[b*16] = 0x40; }
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:blk bytesPerRow:2 * blockBytes];
        [tex replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:2 withBytes:blk bytesPerRow:blockBytes];
        uint8_t rb[16] = {0};
        [tex getBytes:rb bytesPerRow:blockBytes fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:2];
        if (memcmp(rb, blk, blockBytes) != 0)
            fail([NSString stringWithFormat:@"BC format %lu 2x2 mip getBytes mismatch", (unsigned long)pf]);

        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso2d];
        [enc setFragmentTexture:tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @"BC2D sample");
        float out[64] = {0};
        readTexBytes(dev, q, rt, 0, 0, 64, out, sizeof out);
        if (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC2_RGBA || pf == MTLPixelFormatBC3_RGBA) {
            if (fabsf(out[0]) > 0.02f || fabsf(out[1] - 1.0f) > 0.02f || fabsf(out[2]) > 0.02f)
                fail([NSString stringWithFormat:@"BC format %lu got (%g,%g,%g)", (unsigned long)pf, out[0], out[1], out[2]]);
        } else if (pf == MTLPixelFormatBC4_RUnorm || pf == MTLPixelFormatBC4_RSnorm) {
            if (fabsf(out[0] - 1.0f) > 0.02f)
                fail([NSString stringWithFormat:@"BC4 format %lu r=%g", (unsigned long)pf, out[0]]);
        } else if (pf == MTLPixelFormatBC7_RGBAUnorm || pf == MTLPixelFormatBC7_RGBAUnorm_sRGB) {
            if (fabsf(out[0] - 1.0f) > 0.05f || fabsf(out[1] - 1.0f) > 0.05f || fabsf(out[2] - 1.0f) > 0.05f)
                fail([NSString stringWithFormat:@"BC7 format %lu got (%g,%g,%g)", (unsigned long)pf, out[0], out[1], out[2]]);
        }
    }

    // 3D BC1_RGBA blit copyFromBuffer:toTexture: + slice 2 sample (T07)
    {
        MTLTextureDescriptor *td3 = [MTLTextureDescriptor new];
        td3.textureType = MTLTextureType3D;
        td3.pixelFormat = MTLPixelFormatBC1_RGBA;
        td3.width = 8; td3.height = 8; td3.depth = 4;
        td3.usage = MTLTextureUsageShaderRead;
        td3.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex3 = [dev newTextureWithDescriptor:td3];
        if (!tex3) fail(@"3D BC1 create failed");

        // 2x2 blocks per slice * 8 bytes = 32 bytes per slice, 4 slices = 128 bytes
        uint8_t srcBytes[128] = {0};
        // slice 2 = pure blue (0x001F), other slices = pure red (0xF800)
        for (int s = 0; s < 4; s++) {
            uint16_t col = (s == 2) ? 0x001Fu : 0xF800u;
            for (int b = 0; b < 4; b++) {
                uint8_t *p = srcBytes + s * 32 + b * 8;
                p[0] = col & 0xFF; p[1] = col >> 8; p[2] = p[0]; p[3] = p[1];
            }
        }
        id<MTLBuffer> up = [dev newBufferWithBytes:srcBytes length:sizeof srcBytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:32
                  sourceSize:MTLSizeMake(8, 8, 4) toTexture:tex3 destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        float zc = 2.5f / 4.0f;
        [enc setRenderPipelineState:pso3d];
        [enc setFragmentTexture:tex3 atIndex:0];
        [enc setFragmentBytes:&zc length:sizeof zc atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @"3D BC1");
        float out[64] = {0};
        readTexBytes(dev, q, rt, 0, 0, 64, out, sizeof out);
        if (fabsf(out[0]) > 0.02f || fabsf(out[1]) > 0.02f || fabsf(out[2] - 1.0f) > 0.02f)
            fail([NSString stringWithFormat:@"3D BC1 slice 2 got (%g,%g,%g)", out[0], out[1], out[2]]);
    }

    // Check explicit refusals on BC RT / D24S8 / X32S8 / GBGR422
    MTLTextureDescriptor *badRT = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA width:4 height:4 mipmapped:NO];
    badRT.usage = MTLTextureUsageRenderTarget;
    if ([dev newTextureWithDescriptor:badRT]) fail(@"BC1 render target should be refused");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ([dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth24Unorm_Stencil8 width:4 height:4 mipmapped:NO]])
        fail(@"D24S8 should be refused");
#pragma clang diagnostic pop
    MTLTextureDescriptor *badX32 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatX32_Stencil8 width:4 height:4 mipmapped:NO];
    badX32.storageMode = MTLStorageModePrivate;
    if ([dev newTextureWithDescriptor:badX32]) fail(@"standalone X32_Stencil8 should be refused");
    if ([dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatGBGR422 width:4 height:4 mipmapped:NO]])
        fail(@"GBGR422 should stay refused on RADV");
    passCase(@"bc1-bc7-and-3d-bc");
}

static void testX32Stencil8View(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; };\n"
         "vertex VOut vs_s(uint vid [[vertex_id]]) {\n"
         "  float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
         "  VOut o; o.pos = float4(p[vid], 0, 1); return o;\n"
         "}\n"
         "fragment uint4 fs_s(texture2d<uint> st [[texture(0)]]) {\n"
         "  return uint4(st.read(uint2(1, 1)).r, 0, 0, 1);\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"x32s8");
    MTLTextureDescriptor *dsd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:8 height:8 mipmapped:NO];
    dsd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    dsd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> ds = [dev newTextureWithDescriptor:dsd];
    if (!ds) fail(@"D32S8 view parent create failed");
    id<MTLTexture> sview = [ds newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    if (!sview) fail(@"X32_Stencil8 view creation failed");

    MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:8 mipmapped:NO];
    rtd.usage = MTLTextureUsageRenderTarget;
    rtd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];

    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [lib newFunctionWithName:@"vs_s"];
    rpd.fragmentFunction = [lib newFunctionWithName:@"fs_s"];
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
    if (!pso) fail([NSString stringWithFormat:@"x32s8 pso: %@", err]);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp1.depthAttachment.texture = ds; rp1.depthAttachment.loadAction = MTLLoadActionClear; rp1.depthAttachment.storeAction = MTLStoreActionStore; rp1.depthAttachment.clearDepth = 0.25;
    rp1.stencilAttachment.texture = ds; rp1.stencilAttachment.loadAction = MTLLoadActionClear; rp1.stencilAttachment.storeAction = MTLStoreActionStore; rp1.stencilAttachment.clearStencil = 0x5Au;
    [[cb renderCommandEncoderWithDescriptor:rp1] endEncoding];

    MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp2.colorAttachments[0].texture = rt; rp2.colorAttachments[0].loadAction = MTLLoadActionClear; rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp2];
    [enc setRenderPipelineState:pso];
    [enc setFragmentTexture:sview atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    finish(cb, @"X32_Stencil8 read");
    uint32_t out[64] = {0};
    readTexBytes(dev, q, rt, 0, 0, 8 * 4, out, sizeof out);
    if (out[0] != 0x5Au) fail([NSString stringWithFormat:@"X32_Stencil8 read got 0x%x expected 0x5a", out[0]]);
    passCase(@"x32-stencil8-view");
}

static void testDualSrcFillDepthClipViewportsAndLayers(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; float zval; uint vp [[viewport_array_index]]; uint lay [[render_target_array_index]]; };\n"
         "struct DualOut { float4 c0 [[color(0), index(0)]]; float4 c1 [[color(0), index(1)]]; };\n"
         "vertex VOut vs_main(uint vid [[vertex_id]], constant float4 &cfg [[buffer(0)]]) {\n"
         "  float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
         "  VOut o; o.pos = float4(p[vid], cfg.x, 1); o.zval = cfg.x;\n"
         "  o.vp = uint(cfg.y); o.lay = uint(cfg.z); return o;\n"
         "}\n"
         "fragment DualOut fs_dual() { DualOut d; d.c0 = float4(0.5, 0.5, 0.5, 1); d.c1 = float4(0.4, 0.2, 0.0, 1); return d; }\n"
         "fragment float4 fs_color(constant float4 &col [[buffer(0)]]) { return col; }\n"
         "fragment float4 fs_vp_color(VOut in [[stage_in]]) {\n"
         "  if (in.vp == 0) return float4(1,0,0,1);\n"
         "  if (in.vp == 1) return float4(0,1,0,1);\n"
         "  if (in.vp == 2) return float4(0,0,1,1);\n"
         "  return float4(1,1,0,1);\n"
         "}\n"
         "fragment float4 fs_lay_color(VOut in [[stage_in]]) {\n"
         "  if (in.lay == 0) return float4(1,0,0,1);\n"
         "  if (in.lay == 1) return float4(0,1,0,1);\n"
         "  if (in.lay == 2) return float4(0,0,1,1);\n"
         "  return float4(1,1,1,1);\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"render2");
    NSError *err = nil;

    // 1. Dual-source blend (R03)
    {
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_dual"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        rpd.colorAttachments[0].blendingEnabled = YES;
        rpd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSource1Color;
        rpd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        rpd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        rpd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        rpd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) { printf("METAL2_81_TRANSLATOR_GAP dual_src_blend error=%s\n", err.localizedDescription.UTF8String); fail([NSString stringWithFormat:@"dual-src pipeline: %@", err]); }
        else {

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        float cfg[4] = { 0, 0, 0, 0 };
        [enc setRenderPipelineState:pso];
        [enc setVertexBytes:cfg length:sizeof cfg atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @"dual-source blend");
        float out[64] = {0};
        readTexBytes(dev, q, rt, 0, 0, 64, out, sizeof out);
        if (fabsf(out[0] - 0.3f) > 0.02f || fabsf(out[1] - 0.3f) > 0.02f || fabsf(out[2] - 0.3f) > 0.02f)
            fail([NSString stringWithFormat:@"dual-src got (%g,%g,%g)", out[0], out[1], out[2]]);
        else passCase(@"dual-source-blend");
        }
    }

    // 2. DepthClipModeClamp + TriangleFillModeLines (R02)
    {
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_color"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rpd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) fail([NSString stringWithFormat:@"clip/fill pso: %@", err]);
        MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionLessEqual;
        dsd.depthWriteEnabled = YES;
        id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget; cd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> col = [dev newTextureWithDescriptor:cd];
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:32 height:32 mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> dep = [dev newTextureWithDescriptor:dd];

        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = col; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.depthAttachment.texture = dep; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc setDepthStencilState:dss];
        // Draw z=2.0 with Clip -> must clip (stay black); then z=2.0 with Clamp + Lines -> clamps to z=1.0, wireframe leaves center black and border green!
        float cfg[4] = { 2.0f, 0, 0, 0 }; float red[4] = { 1, 0, 0, 1 }; float green[4] = { 0, 1, 0, 1 };
        [enc setDepthClipMode:MTLDepthClipModeClip];
        [enc setTriangleFillMode:MTLTriangleFillModeFill];
        [enc setVertexBytes:cfg length:sizeof cfg atIndex:0];
        [enc setFragmentBytes:red length:sizeof red atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        [enc setDepthClipMode:MTLDepthClipModeClamp];
        [enc setTriangleFillMode:MTLTriangleFillModeLines];
        [enc setFragmentBytes:green length:sizeof green atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        finish(cb, @"depthClip+fillLines");
        uint8_t px[32 * 32 * 4] = {0};
        readTexBytes(dev, q, col, 0, 0, 32 * 4, px, sizeof px);
        // Check no red anywhere (Clip rejected z=2.0), interior (10,10) stays black (Lines wireframe!), and (0,1) border has green!
        int foundGreen = 0;
        for (int p = 0; p < 32 * 32; p++) {
            if (px[p*4 + 0] > 10) fail([NSString stringWithFormat:@"depthClip failed at px %d (got red %u)", p, px[p*4+0]]);
            if (px[p*4 + 1] > 200) foundGreen++;
        }
        int center = (6 * 32 + 6) * 4;
        if (!foundGreen || px[center + 1] > 10)
            fail([NSString stringWithFormat:@"fillLines failed foundGreen=%d centerGreen=%u", foundGreen, px[center+1]]);
        passCase(@"depth-clip-and-triangle-fill-lines");
    }

    // 3. Multi-viewport & Multi-scissor (R05, L12)
    {
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_vp_color"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) { printf("METAL2_81_TRANSLATOR_GAP multi_viewport error=%s\n", err.localizedDescription.UTF8String); fail([NSString stringWithFormat:@"vp pso: %@", err]); }

        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget; cd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> col = [dev newTextureWithDescriptor:cd];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = col; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        MTLViewport vps[4] = {
            { 0, 0, 16, 16, 0, 1 }, { 16, 0, 16, 16, 0, 1 },
            { 0, 16, 16, 16, 0, 1 }, { 16, 16, 16, 16, 0, 1 }
        };
        MTLScissorRect srs[4] = {
            { 0, 0, 16, 16 }, { 16, 0, 16, 16 },
            { 0, 16, 8, 16 }, { 16, 16, 16, 16 } // scissor on vp2 restricts right half!
        };
        [enc setRenderPipelineState:pso];
        [enc setViewports:vps count:4];
        [enc setScissorRects:srs count:4];
        for (int v = 0; v < 4; v++) {
            float cfg[4] = { 0, (float)v, 0, 0 };
            [enc setVertexBytes:cfg length:sizeof cfg atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding];
        finish(cb, @"multi-viewport");
        uint8_t px[32 * 32 * 4] = {0};
        readTexBytes(dev, q, col, 0, 0, 32 * 4, px, sizeof px);
        // Q0(4,4)=R, Q1(20,4)=G, Q2_in(4,20)=B, Q2_clipped(12,20)=0, Q3(20,20)=R+G
        int i0 = (4 * 32 + 4) * 4, i1 = (4 * 32 + 20) * 4, i2a = (20 * 32 + 4) * 4, i2b = (20 * 32 + 12) * 4, i3 = (20 * 32 + 20) * 4;
        if (px[i0] < 250 || px[i1+1] < 250 || px[i2a+2] < 250 || px[i2b+2] > 5 || px[i3] < 250 || px[i3+1] < 250)
            fail([NSString stringWithFormat:@"multi-vp mismatch (%u,%u,%u,%u) clipped=%u", px[i0], px[i1+1], px[i2a+2], px[i3], px[i2b+2]]);
        passCase(@"multi-viewport-and-scissor");
    }

    // 4. Layered rendering (renderTargetArrayLength = 4, R05)
    {
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_lay_color"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) { printf("METAL2_81_TRANSLATOR_GAP layered_render error=%s\n", err.localizedDescription.UTF8String); fail([NSString stringWithFormat:@"layer pso: %@", err]); }

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.textureType = MTLTextureType2DArray; td.arrayLength = 4;
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> arr = [dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = arr; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.renderTargetArrayLength = 4;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        for (int l = 0; l < 4; l++) {
            float cfg[4] = { 0, 0, (float)l, 0 };
            [enc setVertexBytes:cfg length:sizeof cfg atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding];
        finish(cb, @"layered render");
        uint8_t px[16 * 16 * 4];
        for (int l = 0; l < 4; l++) {
            readTexBytes(dev, q, arr, (NSUInteger)l, 0, 16 * 4, px, sizeof px);
            uint8_t exp[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255} };
            if (px[0] != exp[l][0] || px[1] != exp[l][1] || px[2] != exp[l][2])
                fail([NSString stringWithFormat:@"layer %d got (%u,%u,%u)", l, px[0], px[1], px[2]]);
        }
        passCase(@"layered-rendering");
    }
}

static void testDSResolveAndSamplePositions(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct VOut { float4 pos [[position]]; };\n"
         "vertex VOut vs_half(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
         "  // rect = (x0, y0, x1, y1) in clip space [-1, 1], z = rect_z (from buf[1])\n"
         "  float2 p[3] = { float2(rect.x, rect.y), float2(rect.z, rect.y), float2(rect.x, rect.w) };\n"
         "  VOut o; o.pos = float4(p[vid], 0.25, 1); return o;\n"
         "}\n"
         "vertex VOut vs_z(uint vid [[vertex_id]], constant float &z [[buffer(0)]]) {\n"
         "  float2 p[3] = { float2(-1, -1), float2(1, -1), float2(-1, 1) };\n"
         "  VOut o; o.pos = float4(p[vid], z, 1); return o;\n"
         "}\n"
         "fragment float4 fs_green() { return float4(0, 1, 0, 1); }\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"msaa");
    NSError *err = nil;

    // 1. Depth resolve Min vs Max + Stencil Sample0 (R04)
    {
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_z"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_green"];
        rpd.rasterSampleCount = 4;
        rpd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        rpd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) fail([NSString stringWithFormat:@"ds resolve pso: %@", err]);

        MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionAlways;
        dsd.depthWriteEnabled = YES;
        MTLStencilDescriptor *st = [MTLStencilDescriptor new];
        st.stencilCompareFunction = MTLCompareFunctionAlways;
        st.depthStencilPassOperation = MTLStencilOperationReplace;
        dsd.frontFaceStencil = st;
        dsd.backFaceStencil = st;
        id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

        MTLTextureDescriptor *msd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:8 height:8 mipmapped:NO];
        msd.textureType = MTLTextureType2DMultisample; msd.sampleCount = 4;
        msd.usage = MTLTextureUsageRenderTarget; msd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ms = [dev newTextureWithDescriptor:msd];

        MTLTextureDescriptor *rsd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:8 height:8 mipmapped:NO];
        rsd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; rsd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> resMin = [dev newTextureWithDescriptor:rsd];
        id<MTLTexture> resMax = [dev newTextureWithDescriptor:rsd];

        // Clear depth to 0.8, stencil to 0x11; draw diagonal half triangle at z=0.2, stencil=0x77 -> diagonal pixels have depth {0.2, 0.8} and stencil {0x77, 0x11}
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
        rp1.depthAttachment.texture = ms; rp1.depthAttachment.loadAction = MTLLoadActionClear; rp1.depthAttachment.clearDepth = 0.8;
        rp1.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve; rp1.depthAttachment.resolveTexture = resMin;
        rp1.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterMin;
        rp1.stencilAttachment.texture = ms; rp1.stencilAttachment.loadAction = MTLLoadActionClear; rp1.stencilAttachment.clearStencil = 0x11;
        rp1.stencilAttachment.storeAction = MTLStoreActionUnknown; rp1.stencilAttachment.resolveTexture = resMin;
        rp1.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp1];
        float z = 0.2f;
        [enc setRenderPipelineState:pso];
        [enc setDepthStencilState:dss];
        [enc setStencilReferenceValue:0x77];
        [enc setVertexBytes:&z length:sizeof z atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc setDepthStoreAction:MTLStoreActionStoreAndMultisampleResolve];
        [enc setStencilStoreAction:MTLStoreActionMultisampleResolve];
        [enc endEncoding];

        // Resolve same MSAA texture with Max filter in another pass (Load)
        MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
        rp2.depthAttachment.texture = ms; rp2.depthAttachment.loadAction = MTLLoadActionLoad;
        rp2.depthAttachment.storeAction = MTLStoreActionMultisampleResolve; rp2.depthAttachment.resolveTexture = resMax;
        rp2.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterMax;
        rp2.stencilAttachment.texture = ms; rp2.stencilAttachment.loadAction = MTLLoadActionLoad; rp2.stencilAttachment.storeAction = MTLStoreActionDontCare;
        [[cb renderCommandEncoderWithDescriptor:rp2] endEncoding];
        finish(cb, @"ds resolve");

        // Sample depth of resMin and resMax via shader (since Vulkan buffer copy on combined D32S8 is restricted in blit)
        NSString *sSrc =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct V { float4 pos [[position]]; };\n"
             "vertex V vs(uint v [[vertex_id]]) { float2 p[3]={{-1,-1},{3,-1},{-1,3}}; V o; o.pos=float4(p[v],0,1); return o; }\n"
             "fragment float4 fs(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]], texture2d<uint> s [[texture(2)]]) {\n"
             "  // Diagonal pixel (0,0) splits the triangle: sample 0 (0.375,0.125) is outside, bottom samples inside => min=0.2, max=0.8;\n"
             "  // Interior pixel (1,7) is fully inside => stencil=0x77.\n"
             "  return float4(a.read(uint2(0,0)).r, b.read(uint2(0,0)).r, float(s.read(uint2(1,7)).r), float(s.read(uint2(0,0)).r));\n"
             "}\n";
        id<MTLLibrary> slib = compileLib(dev, sSrc, @"dsread");
        MTLRenderPipelineDescriptor *srpd = [MTLRenderPipelineDescriptor new];
        srpd.vertexFunction = [slib newFunctionWithName:@"vs"]; srpd.fragmentFunction = [slib newFunctionWithName:@"fs"];
        srpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> spso = [dev newRenderPipelineStateWithDescriptor:srpd error:&err];
        MTLTextureDescriptor *outd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
        outd.usage = MTLTextureUsageRenderTarget; outd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> outt = [dev newTextureWithDescriptor:outd];
        id<MTLTexture> sView = [resMin newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLCommandBuffer> cb2 = [q commandBuffer];
        MTLRenderPassDescriptor *rpo = [MTLRenderPassDescriptor renderPassDescriptor];
        rpo.colorAttachments[0].texture = outt; rpo.colorAttachments[0].loadAction = MTLLoadActionClear; rpo.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> oe = [cb2 renderCommandEncoderWithDescriptor:rpo];
        [oe setRenderPipelineState:spso];
        [oe setFragmentTexture:resMin atIndex:0];
        [oe setFragmentTexture:resMax atIndex:1];
        [oe setFragmentTexture:sView atIndex:2];
        [oe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [oe endEncoding];
        finish(cb2, @"ds resolve verify");
        float out[64] = {0};
        readTexBytes(dev, q, outt, 0, 0, 64, out, sizeof out);
        if (fabsf(out[0] - 0.2f) > 0.01f || fabsf(out[1] - 0.8f) > 0.01f ||
            fabsf(out[2] - (float)0x77) > 0.5f || fabsf(out[3] - (float)0x11) > 0.5f)
            fail([NSString stringWithFormat:@"ds resolve min=%g max=%g stencil_in=%g stencil_out=%g", out[0], out[1], out[2], out[3]]);
        passCase(@"depth-stencil-msaa-resolve");
    }

    // 2. Programmable sample positions (R04)
    {
        if (![dev areProgrammableSamplePositionsSupported]) fail(@"areProgrammableSamplePositionsSupported == NO");
        MTLSamplePosition defs[4];
        [dev getDefaultSamplePositions:defs count:4];
        if (defs[0].x <= 0 || defs[0].x >= 1) fail(@"getDefaultSamplePositions invalid");

        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_half"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_green"];
        rpd.rasterSampleCount = 4;
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) fail([NSString stringWithFormat:@"samplePos pso: %@", err]);

        MTLTextureDescriptor *msd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
        msd.textureType = MTLTextureType2DMultisample; msd.sampleCount = 4;
        msd.usage = MTLTextureUsageRenderTarget; msd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ms = [dev newTextureWithDescriptor:msd];
        MTLTextureDescriptor *rsd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
        rsd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; rsd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> resMiss = [dev newTextureWithDescriptor:rsd];
        id<MTLTexture> resHit = [dev newTextureWithDescriptor:rsd];

        // For a 2x2 render target in NDC: left half x in [0, 1] maps to clip x in [-1, 0].
        // Wait: draw a single pixel-quad covering right half of every pixel?! No, draw a triangle covering x in [0.55, 2.0] across pixel 1 (clip [0.55, 1.0]) or just over 1x1 target!
        // On a 1x1 target, pixel [0..1, 0..1] IS the whole screen! Left half of pixel = x in [0, 0.5] -> clip [-1, 0]!
        // Triangle spanning whole screen right half: x in [0.05, 1.0], y in [-1.0, 3.0] in NDC covers ONLY pixel x in [0.525, 1.0]!
        MTLTextureDescriptor *ms1d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        ms1d.textureType = MTLTextureType2DMultisample; ms1d.sampleCount = 4;
        ms1d.usage = MTLTextureUsageRenderTarget; ms1d.storageMode = MTLStorageModePrivate;
        ms = [dev newTextureWithDescriptor:ms1d];
        MTLTextureDescriptor *rs1d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        rs1d.usage = MTLTextureUsageRenderTarget; rs1d.storageMode = MTLStorageModePrivate;
        resMiss = [dev newTextureWithDescriptor:rs1d];
        resHit = [dev newTextureWithDescriptor:rs1d];

        float rightRect[4] = { 0.05f, -1.0f, 3.0f, 3.0f }; // covers x in [0.525, 1.0]
        MTLSamplePosition leftPos[4] = { {0.25f, 0.5f}, {0.25f, 0.5f}, {0.25f, 0.5f}, {0.25f, 0.5f} };
        MTLSamplePosition rightPos[4] = { {0.75f, 0.5f}, {0.75f, 0.5f}, {0.75f, 0.5f}, {0.75f, 0.5f} };

        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rpm = [MTLRenderPassDescriptor renderPassDescriptor];
        rpm.colorAttachments[0].texture = ms; rpm.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpm.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve; rpm.colorAttachments[0].resolveTexture = resMiss;
        rpm.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        [rpm setSamplePositions:leftPos count:4];
        id<MTLRenderCommandEncoder> em = [cb renderCommandEncoderWithDescriptor:rpm];
        [em setRenderPipelineState:pso];
        [em setVertexBytes:rightRect length:sizeof rightRect atIndex:0];
        [em drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [em endEncoding];

        MTLRenderPassDescriptor *rph = [MTLRenderPassDescriptor renderPassDescriptor];
        rph.colorAttachments[0].texture = ms; rph.colorAttachments[0].loadAction = MTLLoadActionClear;
        rph.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve; rph.colorAttachments[0].resolveTexture = resHit;
        rph.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        [rph setSamplePositions:rightPos count:4];
        id<MTLRenderCommandEncoder> eh = [cb renderCommandEncoderWithDescriptor:rph];
        [eh setRenderPipelineState:pso];
        [eh setVertexBytes:rightRect length:sizeof rightRect atIndex:0];
        [eh drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [eh endEncoding];
        finish(cb, @"programmable sample positions");

        uint8_t pMiss[4] = {0}, pHit[4] = {0};
        readTexBytes(dev, q, resMiss, 0, 0, 4, pMiss, 4);
        readTexBytes(dev, q, resHit, 0, 0, 4, pHit, 4);
        if (pMiss[1] != 0 || pHit[1] < 250)
            fail([NSString stringWithFormat:@"samplePositions missG=%u hitG=%u", pMiss[1], pHit[1]]);
        passCase(@"programmable-sample-positions");
    }
}

static void testReadWriteTextures(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    if ([dev readWriteTextureSupport] != MTLReadWriteTextureTier2)
        fail([NSString stringWithFormat:@"readWriteTextureSupport got %lu expected Tier2 (2)", (unsigned long)[dev readWriteTextureSupport]]);
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void rw_float(texture2d<float, access::read_write> t [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
         "  float4 v = t.read(gid);\n"
         "  t.write(v + float4(0.25, 0.5, 0.0, 0.0), gid);\n"
         "}\n"
         "kernel void rw_uint(texture2d<uint, access::read_write> t [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
         "  uint4 v = t.read(gid);\n"
         "  t.write(v + uint4(10, 20, 30, 40), gid);\n"
         "}\n";
    id<MTLLibrary> lib = compileLib(dev, src, @"rwtex");
    NSError *err = nil;
    id<MTLComputePipelineState> psoF = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rw_float"] error:&err];
    if (!psoF) { printf("METAL2_81_TRANSLATOR_GAP rw_float error=%s\n", err.localizedDescription.UTF8String); fail([NSString stringWithFormat:@"rw_float pso: %@", err]); }
    id<MTLComputePipelineState> psoU = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rw_uint"] error:&err];
    if (!psoU) { printf("METAL2_81_TRANSLATOR_GAP rw_uint error=%s\n", err.localizedDescription.UTF8String); fail([NSString stringWithFormat:@"rw_uint pso: %@", err]); }

    MTLPixelFormat rwFmts[3] = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA32Float };
    for (int i = 0; i < 3; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:rwFmts[i] width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!tex) fail([NSString stringWithFormat:@"rw fmt %lu failed", (unsigned long)rwFmts[i]]);
        // Clear to (0.25, 0.25, 0.5, 1.0) via render pass or upload, wait: test RGBA32Float/RGBA16Float/RGBA8Unorm via render pass clear then RW dispatch!
        td.usage |= MTLTextureUsageRenderTarget;
        tex = [dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.25, 0.5, 1.0);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:psoF];
        [ce setTexture:tex atIndex:0];
        [ce dispatchThreads:MTLSizeMake(4, 4, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
        [ce endEncoding];
        finish(cb, @"rw_float");
        // Verify result: (0.5, 0.75, 0.5, 1.0)
        if (rwFmts[i] == MTLPixelFormatRGBA32Float) {
            float out[64] = {0};
            readTexBytes(dev, q, tex, 0, 0, 4 * 16, out, sizeof out);
            if (fabsf(out[0] - 0.5f) > 0.01f || fabsf(out[1] - 0.75f) > 0.01f || fabsf(out[2] - 0.5f) > 0.01f)
                fail([NSString stringWithFormat:@"rw RGBA32Float got (%g,%g,%g)", out[0], out[1], out[2]]);
        } else if (rwFmts[i] == MTLPixelFormatRGBA8Unorm) {
            uint8_t out[64] = {0};
            readTexBytes(dev, q, tex, 0, 0, 16, out, sizeof out);
            if (abs((int)out[0] - 128) > 2 || abs((int)out[1] - 191) > 2)
                fail([NSString stringWithFormat:@"rw RGBA8Unorm got (%u,%u)", out[0], out[1]]);
        }
    }
    // Tier 2 UInt RW (R32Uint / RGBA8Uint / RG32Uint / R8Uint / RGBA16Uint)
    MTLPixelFormat uFmts[3] = { MTLPixelFormatR32Uint, MTLPixelFormatRGBA8Uint, MTLPixelFormatRGBA16Uint };
    for (int i = 0; i < 3; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:uFmts[i] width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(5, 6, 7, 8);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:psoU];
        [ce setTexture:tex atIndex:0];
        [ce dispatchThreads:MTLSizeMake(4, 4, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
        [ce endEncoding];
        finish(cb, @"rw_uint");
        uint32_t bpr = uFmts[i] == MTLPixelFormatRGBA16Uint ? 32 : 16;
        uint8_t out[128] = {0};
        readTexBytes(dev, q, tex, 0, 0, bpr, out, bpr * 4);
        if (uFmts[i] == MTLPixelFormatR32Uint && *(uint32_t *)out != 15u)
            fail([NSString stringWithFormat:@"rw R32Uint got %u", *(uint32_t *)out]);
        if (uFmts[i] == MTLPixelFormatRGBA8Uint && (out[0] != 15 || out[1] != 26 || out[2] != 37 || out[3] != 48))
            fail([NSString stringWithFormat:@"rw RGBA8Uint got (%u,%u,%u,%u)", out[0], out[1], out[2], out[3]]);
    }
    passCase(@"read-write-textures-tier2");
}

int main(int argc, const char *argv[]) {
    if (argc < 2 || argc > 3) return 2;
    if (argc == 3 && !strcmp(argv[2], "--continue")) softFail = YES;
    BOOL ref = !strcmp(argv[1], "--reference");
    if (!ref && geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);
    @autoreleasepool {
        @try {
            id<MTLDevice> dev = nil;
            if (ref) {
                dev = MTLCreateSystemDefaultDevice();
            } else {
                setenv("RDNA4M_ALLOW", "1", 1);
                setenv("RDNA4M_TEST_INPROC", "1", 1);
                setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
                setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
                NSError *loadError = nil;
                if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&loadError])
                    fail(loadError.description);
                dev = [[objc_getClass("GarnetDevice") alloc] init];
                ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
            }
            if (!dev) fail(@"device unavailable");
            id<MTLCommandQueue> q = [dev newCommandQueue];
            if (!q) fail(@"queue unavailable");

            testUncompressedFormatsAndIntClears(dev, q);
            testBCFormatsAnd3DBC(dev, q);
            testX32Stencil8View(dev, q);
            testDualSrcFillDepthClipViewportsAndLayers(dev, q);
            testDSResolveAndSamplePositions(dev, q);
            testReadWriteTextures(dev, q);

            if (anyFail) {
                printf("METAL2_81_SUITE_PARTIAL_FAIL production_bundle=%u\n", ref ? 0u : 1u);
                return 1;
            }
            printf("METAL2_81_SUITE_PASS production_bundle=%u\n", ref ? 0u : 1u);
            return 0;
        } @catch (NSException *e) {
            fprintf(stderr, "METAL2_81_EXCEPTION %s\n", e.description.UTF8String);
            return 1;
        }
    }
}
