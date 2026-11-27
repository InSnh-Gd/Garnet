// Path-cover regression for the icon-shape pipeline family.
//
// Core Animation draws a path (the app-icon squircle among others) with a
// triangle strip whose fragments read the coverage attachment back through
// [[color(n)]] and accumulate a signed winding from [[front_facing]]; a later
// stencil-and-cover pass (compare notEqual, pass op zero) keeps only covered
// pixels. IconServices renders app icons through exactly that family, and the
// rendered icon loses its coverage in fixed angular sectors, so this test
// reproduces the shape of that dance without Core Animation:
//
//   pass 1  fan as a triangle strip, fragment reads [[color(1)]] (RG16Float)
//           and adds +1/-1 by [[front_facing]]; attachment 0 is not written
//   pass 2  full-screen quad, stencil notEqual 0 with pass -> zero, writes green
//
// The covered area must equal the CPU point-in-polygon reference, for the fan
// drawn as a strip, as a list, and as a strip with vertexStart 1. Any missing
// sector, doubled sign or winding inversion fails a check.
//
// Run on the RDNA4 target against the installed bundle (root, and the shaders
// are not in spvcache so in-process translation must be allowed):
//   sudo env RDNA4M_TEST_INPROC=1 ./test-path-cover [/Library/GPUBundles/GarnetMetal.bundle]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static void check(int ok, const char *name, const char *detail) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, detail ? ": " : "", detail ? detail : "");
    if (!ok) fails++;
}

#define SIDE 128
#define RIM 24

static void rim_points(float *out) {
    for (int i = 0; i < RIM; i++) {
        double angle = 2.0 * M_PI * (double)i / (double)RIM;
        out[2 * i + 0] = (float)(cos(angle) * 0.6);
        out[2 * i + 1] = (float)(sin(angle) * 0.6);
    }
}

// fan as a strip with degenerate triangles: rim0, C, rim1, C, ... rim(N-1), C, rim0
static int build_strip(const float *rim, float *out) {
    int n = 0;
    for (int i = 0; i < RIM; i++) {
        out[2 * n + 0] = rim[2 * i + 0]; out[2 * n + 1] = rim[2 * i + 1]; n++;
        out[2 * n + 0] = 0.0f; out[2 * n + 1] = 0.0f; n++;
    }
    out[2 * n + 0] = rim[0]; out[2 * n + 1] = rim[1]; n++;
    return n;
}

static int build_list(const float *rim, float *out) {
    int n = 0;
    for (int i = 0; i < RIM; i++) {
        int j = (i + 1) % RIM;
        out[2 * n + 0] = rim[2 * i + 0]; out[2 * n + 1] = rim[2 * i + 1]; n++;
        out[2 * n + 0] = 0.0f; out[2 * n + 1] = 0.0f; n++;
        out[2 * n + 0] = rim[2 * j + 0]; out[2 * n + 1] = rim[2 * j + 1]; n++;
    }
    return n;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        const char *bundlePath = argc > 1 ? argv[1] : "/Library/GPUBundles/GarnetMetal.bundle";
        setenv("RDNA4M_ALLOW", "1", 1);
        NSError *err = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundlePath]];
        if (!bundle || ![bundle loadAndReturnError:&err]) {
            fprintf(stderr, "FAIL load bundle %s\n", bundlePath);
            return 2;
        }
        Class deviceClass = objc_getClass("GarnetDevice");
        id<MTLDevice> dev = deviceClass ? [[deviceClass alloc] init] : nil;
        if (!dev) { fprintf(stderr, "FAIL GarnetDevice alloc/init returned nil\n"); return 2; }

        NSString *msl =
            @"#include <metal_stdlib>\n"
            @"using namespace metal;\n"
            @"struct VOut { float4 pos [[position]]; };\n"
            @"vertex VOut vbuf(uint vid [[vertex_id]], const device float2 *pts [[buffer(0)]]) {\n"
            @"    VOut o; o.pos = float4(pts[vid], 0.0, 1.0); return o;\n"
            @"}\n"
            // reads the coverage attachment back and accumulates the signed winding
            @"fragment float2 faccum(float2 prev [[color(1)]], bool facing [[front_facing]]) {\n"
            @"    return prev + (facing ? float2(0.0, 1.0) : float2(0.0, -1.0));\n"
            @"}\n"
            @"fragment float4 fgreen() { return float4(0.0, 1.0, 0.0, 1.0); }\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        check(lib != nil, "compile path-cover MSL library", err ? [[err localizedDescription] UTF8String] : "ok");
        if (!lib) return 1;

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vbuf"];
        pd.fragmentFunction = [lib newFunctionWithName:@"faccum"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        pd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
        pd.colorAttachments[1].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        err = nil;
        id<MTLRenderPipelineState> psoAccum = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        check(psoAccum != nil, "create coverage-accumulation pipeline", err ? [[err localizedDescription] UTF8String] : "ok");

        MTLRenderPipelineDescriptor *pdCover = [MTLRenderPipelineDescriptor new];
        pdCover.vertexFunction = [lib newFunctionWithName:@"vbuf"];
        pdCover.fragmentFunction = [lib newFunctionWithName:@"fgreen"];
        pdCover.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pdCover.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pdCover.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        err = nil;
        id<MTLRenderPipelineState> psoCover = [dev newRenderPipelineStateWithDescriptor:pdCover error:&err];
        check(psoCover != nil && psoAccum != nil, "create stencil cover pipeline", err ? [[err localizedDescription] UTF8String] : "ok");
        if (!psoAccum || !psoCover) return 1;

        MTLDepthStencilDescriptor *dsAccumDescription = [MTLDepthStencilDescriptor new];
        MTLStencilDescriptor *stencilAccum = [MTLStencilDescriptor new];
        stencilAccum.stencilCompareFunction = MTLCompareFunctionAlways;
        stencilAccum.stencilFailureOperation = MTLStencilOperationKeep;
        stencilAccum.depthFailureOperation = MTLStencilOperationKeep;
        stencilAccum.depthStencilPassOperation = MTLStencilOperationReplace;
        stencilAccum.readMask = 0xffu;
        stencilAccum.writeMask = 0xffu;
        dsAccumDescription.frontFaceStencil = stencilAccum;
        dsAccumDescription.backFaceStencil = stencilAccum;
        dsAccumDescription.depthCompareFunction = MTLCompareFunctionAlways;
        dsAccumDescription.depthWriteEnabled = NO;
        id<MTLDepthStencilState> dsAccum = [dev newDepthStencilStateWithDescriptor:dsAccumDescription];

        MTLDepthStencilDescriptor *dsCoverDescription = [MTLDepthStencilDescriptor new];
        MTLStencilDescriptor *stencilCover = [MTLStencilDescriptor new];
        stencilCover.stencilCompareFunction = MTLCompareFunctionNotEqual;
        stencilCover.stencilFailureOperation = MTLStencilOperationKeep;
        stencilCover.depthFailureOperation = MTLStencilOperationKeep;
        stencilCover.depthStencilPassOperation = MTLStencilOperationZero;
        stencilCover.readMask = 0xffu;
        stencilCover.writeMask = 0xffu;
        dsCoverDescription.frontFaceStencil = stencilCover;
        dsCoverDescription.backFaceStencil = stencilCover;
        dsCoverDescription.depthCompareFunction = MTLCompareFunctionAlways;
        dsCoverDescription.depthWriteEnabled = NO;
        id<MTLDepthStencilState> dsCover = [dev newDepthStencilStateWithDescriptor:dsCoverDescription];
        check(dsAccum != nil && dsCover != nil, "create stencil states", NULL);
        if (!dsAccum || !dsCover) return 1;

        float rim[2 * RIM]; rim_points(rim);
        static float strip[2 * (2 * RIM + 1)], list[2 * (3 * RIM)], stripShift[2 * (2 * RIM + 2)];
        int stripCount = build_strip(rim, strip), listCount = build_list(rim, list);
        // The same strip one vertex later: a leading duplicate keeps the vertex set (and so the
        // triangle set) identical while every triangle moves to the other strip parity.
        stripShift[0] = strip[0]; stripShift[1] = strip[1];
        memcpy(stripShift + 2, strip, sizeof(float) * 2 * stripCount);
        // a full-screen quad for the cover pass (two triangles)
        static float quad[12] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };

        id<MTLBuffer> stripBuffer = [dev newBufferWithBytes:strip length:sizeof(float) * 2 * stripCount options:MTLResourceStorageModeShared];
        id<MTLBuffer> shiftBuffer = [dev newBufferWithBytes:stripShift length:sizeof(float) * 2 * (stripCount + 1) options:MTLResourceStorageModeShared];
        id<MTLBuffer> listBuffer = [dev newBufferWithBytes:list length:sizeof(float) * 2 * listCount options:MTLResourceStorageModeShared];
        id<MTLBuffer> quadBuffer = [dev newBufferWithBytes:quad length:sizeof(quad) options:MTLResourceStorageModeShared];

        MTLTextureDescriptor *colourDescription = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:SIDE height:SIDE mipmapped:NO];
        colourDescription.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        MTLTextureDescriptor *coverDescription = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:SIDE height:SIDE mipmapped:NO];
        coverDescription.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        MTLTextureDescriptor *dsDescription = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:SIDE height:SIDE mipmapped:NO];
        dsDescription.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        dsDescription.storageMode = MTLStorageModePrivate;

        const char *names[3] = { "triangle strip", "triangle list", "strip vertexStart 1" };
        int covered[3] = { 0, 0, 0 };
        for (int variant = 0; variant < 3; variant++) {
            id<MTLCommandQueue> queue = [dev newCommandQueue];
            id<MTLTexture> colour = [dev newTextureWithDescriptor:colourDescription];
            id<MTLTexture> cover = [dev newTextureWithDescriptor:coverDescription];
            id<MTLTexture> ds = [dev newTextureWithDescriptor:dsDescription];
            for (int pass = 0; pass < 2; pass++) {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = colour;
                rp.colorAttachments[1].texture = cover;
                rp.colorAttachments[0].loadAction = pass == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.colorAttachments[1].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[1].storeAction = MTLStoreActionStore;
                rp.depthAttachment.texture = ds;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
                rp.stencilAttachment.texture = ds;
                rp.stencilAttachment.loadAction = pass == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.clearStencil = 0;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:rp];
                if (pass == 0) {
                    [encoder setRenderPipelineState:psoAccum];
                    [encoder setDepthStencilState:dsAccum];
                    [encoder setStencilReferenceValue:1];
                    [encoder setCullMode:MTLCullModeNone];
                    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
                    BOOL useList = variant == 1;
                    [encoder setVertexBuffer:(useList ? listBuffer : variant == 2 ? shiftBuffer : stripBuffer) offset:0 atIndex:0];
                    if (useList) [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)listCount];
                    else [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:(variant == 2 ? 1 : 0) vertexCount:(NSUInteger)stripCount];
                } else {
                    [encoder setRenderPipelineState:psoCover];
                    [encoder setDepthStencilState:dsCover];
                    [encoder setStencilReferenceValue:0];
                    [encoder setCullMode:MTLCullModeNone];
                    [encoder setVertexBuffer:quadBuffer offset:0 atIndex:0];
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                }
                [encoder endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                check(cb.error == nil, "command buffer completes", cb.error ? [[cb.error localizedDescription] UTF8String] : NULL);
            }
            uint32_t *pixels = malloc(SIDE * SIDE * 4);
            [colour getBytes:pixels bytesPerRow:SIDE * 4 fromRegion:MTLRegionMake2D(0, 0, SIDE, SIDE) mipmapLevel:0];
            for (int i = 0; i < SIDE * SIDE; i++) {
                uint32_t value = pixels[i];
                if (((value >> 8) & 0xff) >= 0x80 && ((value >> 16) & 0xff) < 0x40) covered[variant]++;
            }
            free(pixels);
        }
        int reference = (int)(M_PI * 0.6 * 0.6 / 4.0 * SIDE * SIDE);
        printf("covered pixels: strip %d, list %d, strip-start-1 %d (reference disc %d)\n",
               covered[0], covered[1], covered[2], reference);
        for (int variant = 0; variant < 3; variant++) {
            char label[96];
            snprintf(label, sizeof label, "%s covers the fan without holes", names[variant]);
            check(covered[variant] >= reference * 9 / 10 && covered[variant] <= reference * 11 / 10, label, NULL);
            snprintf(label, sizeof label, "%s matches the triangle list", names[variant]);
            check(abs(covered[variant] - covered[1]) <= reference / 100, label, NULL);
        }
        printf("\n%d failure(s)\n", fails);
        return fails ? 1 : 0;
    }
}
