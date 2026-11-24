// test-metal3-swizzle.m: live RX 9060 XT / reference Metal verification for Task 2.1
// (MTLTextureDescriptor.swizzle, newTextureViewWithPixelFormat:...:swizzle:, composition,
// Zero/One/BGRA/A8/Depth32Float sampling, and illegal swizzle usage refusal).
// Build on macOS:
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-swizzle.m -o "$TMPDIR/test-metal3-swizzle"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-swizzle" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-swizzle" --reference
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

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_SWIZZLE_FAIL %s\n", msg.UTF8String);
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

static void sampleTex(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                      id<MTLTexture> tex, float out[4], NSString *tag) {
    id<MTLBuffer> buf = [dev newBufferWithLength:4 * sizeof(float) options:MTLResourceStorageModeShared];
    require(buf != nil, [NSString stringWithFormat:@"%@: output buffer nil", tag]);
    memset(buf.contents, 0xA5, buf.length);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: compute encoder nil", tag]);
    [enc setComputePipelineState:pso];
    [enc setTexture:tex atIndex:0];
    [enc setBuffer:buf offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finish(cb, tag);
    memcpy(out, buf.contents, 4 * sizeof(float));
}

static void checkVec4(const float got[4], const float exp[4], float tol, NSString *tag) {
    for (int i = 0; i < 4; i++) {
        if (fabsf(got[i] - exp[i]) > tol) {
            failf([NSString stringWithFormat:@"%@: ch %d got %g expected %g (got=[%g,%g,%g,%g] exp=[%g,%g,%g,%g])",
                   tag, i, got[i], exp[i], got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3]]);
        }
    }
}

static void checkSwzEq(MTLTextureSwizzleChannels got, MTLTextureSwizzleChannels exp, NSString *tag) {
    require(got.red == exp.red && got.green == exp.green && got.blue == exp.blue && got.alpha == exp.alpha,
            [NSString stringWithFormat:@"%@: swizzle (%u,%u,%u,%u) != expected (%u,%u,%u,%u)",
             tag, (unsigned)got.red, (unsigned)got.green, (unsigned)got.blue, (unsigned)got.alpha,
             (unsigned)exp.red, (unsigned)exp.green, (unsigned)exp.blue, (unsigned)exp.alpha]);
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

        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void sample_rgba(texture2d<float> t [[texture(0)]],\n"
             "                        device float4 *out [[buffer(0)]]) {\n"
             "    constexpr sampler s(filter::nearest);\n"
             "    out[0] = t.sample(s, float2(0.5, 0.5));\n"
             "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"library compile: %@", err]);
        id<MTLFunction> fn = [lib newFunctionWithName:@"sample_rgba"];
        require(fn != nil, @"sample_rgba function missing");
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
        require(pso != nil, [NSString stringWithFormat:@"compute pipeline: %@", err]);

        const uint8_t rgbaPx[4] = { 51, 102, 153, 204 }; // R=0.2, G=0.4, B=0.6, A=0.8
        const uint8_t bgraPx[4] = { 153, 102, 51, 204 }; // BGRA bytes -> R=0.2, G=0.4, B=0.6, A=0.8
        const float tol = 0.01f;

        // 1. MTLTextureDescriptor.swizzle on RGBA8Unorm (channel swap + Zero/One).
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:4 height:4 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            td.storageMode = MTLStorageModeShared;
            MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleBlue, MTLTextureSwizzleZero, MTLTextureSwizzleOne, MTLTextureSwizzleRed);
            td.swizzle = sw;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            require(tex != nil, @"RGBA8 descriptor swizzle texture nil");
            checkSwzEq(tex.swizzle, sw, @"RGBA8 descriptor swizzle");
            uint8_t img[4 * 4 * 4];
            for (int i = 0; i < 16; i++) memcpy(img + i * 4, rgbaPx, 4);
            [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:img bytesPerRow:16];

            float got[4] = {0};
            sampleTex(dev, q, pso, tex, got, @"desc_rgba_b01r");
            const float exp[4] = { 0.6f, 0.0f, 1.0f, 0.2f };
            checkVec4(got, exp, tol, @"desc_rgba_b01r");

            // Underlying bytes must remain untouched by swizzle.
            uint8_t raw[4 * 4 * 4] = {0};
            [tex getBytes:raw bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
            require(memcmp(raw, img, sizeof raw) == 0, @"descriptor swizzle mutated raw texel bytes");

            // Non-swizzle view overload inherits parent swizzle.
            id<MTLTexture> inheritView = [tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
            require(inheritView != nil, @"inheritView nil");
            checkSwzEq(inheritView.swizzle, sw, @"inheritView swizzle");
            sampleTex(dev, q, pso, inheritView, got, @"inherit_view");
            checkVec4(got, exp, tol, @"inherit_view");
            puts("METAL3_SWIZZLE_CASE_PASS descriptor_and_inherit");
        }

        // 2. Nested swizzle composition (Apple doc example: parent=[R,1,1,G], view=[R,G,A,B] -> [R,1,G,1])
        //    plus BGRA8Unorm double-swap back to identity.
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:4 height:4 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            td.storageMode = MTLStorageModeShared;
            MTLTextureSwizzleChannels pSwz = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleGreen);
            td.swizzle = pSwz;
            id<MTLTexture> base = [dev newTextureWithDescriptor:td];
            require(base != nil, @"BGRA8 base swizzle texture nil");
            uint8_t img[4 * 4 * 4];
            for (int i = 0; i < 16; i++) memcpy(img + i * 4, bgraPx, 4);
            [base replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:img bytesPerRow:16];

            MTLTextureSwizzleChannels vSwz = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleAlpha, MTLTextureSwizzleBlue);
            MTLTextureSwizzleChannels expectedComposed = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleGreen, MTLTextureSwizzleOne);
            id<MTLTexture> view = [base newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                          textureType:MTLTextureType2D
                                                               levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(0, 1)
                                                              swizzle:vSwz];
            require(view != nil, @"nested composed view nil");
            checkSwzEq(view.swizzle, expectedComposed, @"Apple doc composed swizzle");
            float got[4] = {0};
            sampleTex(dev, q, pso, view, got, @"composed_apple_example");
            const float exp[4] = { 0.2f, 1.0f, 0.4f, 1.0f };
            checkVec4(got, exp, tol, @"composed_apple_example");

            // Double BGRA swap returns original RGBA channels.
            MTLTextureDescriptor *td2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:4 height:4 mipmapped:NO];
            td2.usage = MTLTextureUsageShaderRead;
            td2.storageMode = MTLStorageModeShared;
            MTLTextureSwizzleChannels swapRB = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
            td2.swizzle = swapRB;
            id<MTLTexture> tSwap = [dev newTextureWithDescriptor:td2];
            require(tSwap != nil, @"tSwap nil");
            [tSwap replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:img bytesPerRow:16];
            id<MTLTexture> tUnSwap = [tSwap newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              textureType:MTLTextureType2D
                                                                   levels:NSMakeRange(0, 1)
                                                                   slices:NSMakeRange(0, 1)
                                                                  swizzle:swapRB];
            require(tUnSwap != nil, @"tUnSwap nil");
            checkSwzEq(tUnSwap.swizzle, MTLTextureSwizzleChannelsDefault, @"double swap -> identity");
            sampleTex(dev, q, pso, tUnSwap, got, @"double_swap_identity");
            const float expOrig[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
            checkVec4(got, expOrig, tol, @"double_swap_identity");
            puts("METAL3_SWIZZLE_CASE_PASS nested_composition_and_bgra");
        }

        // 3. A8Unorm swizzle ({A,A,A,1}) and Depth32Float swizzle ({R,R,R,1}) + RT parent view.
        {
            MTLTextureDescriptor *a8d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm
                                                                                           width:4 height:4 mipmapped:NO];
            a8d.usage = MTLTextureUsageShaderRead;
            a8d.storageMode = MTLStorageModeShared;
            a8d.swizzle = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleAlpha, MTLTextureSwizzleAlpha, MTLTextureSwizzleAlpha, MTLTextureSwizzleOne);
            id<MTLTexture> a8 = [dev newTextureWithDescriptor:a8d];
            require(a8 != nil, @"A8 swizzle texture nil");
            uint8_t a8Bytes[16];
            memset(a8Bytes, 153, sizeof a8Bytes); // ~0.6
            [a8 replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:a8Bytes bytesPerRow:4];
            float got[4] = {0};
            sampleTex(dev, q, pso, a8, got, @"a8_aaa1");
            const float expA8[4] = { 0.6f, 0.6f, 0.6f, 1.0f };
            checkVec4(got, expA8, tol, @"a8_aaa1");

            // Render-target parent (identity swizzle) -> render clear -> swizzled sampling view.
            MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:4 height:4 mipmapped:NO];
            rtd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            rtd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
            require(rt != nil, @"RT parent nil");
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rpd];
            require(renc != nil, @"render encoder nil");
            [renc endEncoding];
            finish(cb, @"clear_rt_parent");

            id<MTLTexture> rtSwz = [rt newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         textureType:MTLTextureType2D
                                                              levels:NSMakeRange(0, 1)
                                                              slices:NSMakeRange(0, 1)
                                                             swizzle:MTLTextureSwizzleChannelsMake(
                                                                 MTLTextureSwizzleAlpha, MTLTextureSwizzleBlue,
                                                                 MTLTextureSwizzleGreen, MTLTextureSwizzleRed)];
            require(rtSwz != nil, @"swizzled view of RT|Read parent nil");
            sampleTex(dev, q, pso, rtSwz, got, @"rt_swizzled_view");
            const float expRtSwz[4] = { 0.8f, 0.6f, 0.4f, 0.2f };
            checkVec4(got, expRtSwz, tol, @"rt_swizzled_view");

            // Unswizzled sibling view of rt still reads original RGBA.
            id<MTLTexture> rtId = [rt newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
            require(rtId != nil, @"identity sibling view nil");
            sampleTex(dev, q, pso, rtId, got, @"rt_identity_sibling");
            const float expOrig[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
            checkVec4(got, expOrig, tol, @"rt_identity_sibling");

            // Depth32Float cleared to 0.75, sampled through {R, R, Zero, One} swizzled view.
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:4 height:4 mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];
            require(depth != nil, @"Depth32Float parent nil");
            MTLRenderPassDescriptor *drpd = [MTLRenderPassDescriptor renderPassDescriptor];
            drpd.depthAttachment.texture = depth;
            drpd.depthAttachment.loadAction = MTLLoadActionClear;
            drpd.depthAttachment.storeAction = MTLStoreActionStore;
            drpd.depthAttachment.clearDepth = 0.75;
            cb = [q commandBuffer];
            renc = [cb renderCommandEncoderWithDescriptor:drpd];
            require(renc != nil, @"depth render encoder nil");
            [renc endEncoding];
            finish(cb, @"clear_depth_parent");

            id<MTLTexture> dView = [depth newTextureViewWithPixelFormat:MTLPixelFormatDepth32Float
                                                            textureType:MTLTextureType2D
                                                                 levels:NSMakeRange(0, 1)
                                                                 slices:NSMakeRange(0, 1)
                                                                swizzle:MTLTextureSwizzleChannelsMake(
                                                                    MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                                    MTLTextureSwizzleZero, MTLTextureSwizzleOne)];
            require(dView != nil, @"Depth32Float swizzled view nil");
            sampleTex(dev, q, pso, dView, got, @"depth_rr01");
            const float expDepth[4] = { 0.75f, 0.75f, 0.0f, 1.0f };
            checkVec4(got, expDepth, tol, @"depth_rr01");
            puts("METAL3_SWIZZLE_CASE_PASS a8_rt_and_depth");
        }

        // 4. Negative validation (candidate-strict checks; Apple validation layer aborts on invalid args).
        if (!reference) {
            MTLTextureSwizzleChannels swapRB = MTLTextureSwizzleChannelsMake(
                MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
            MTLTextureSwizzleChannels badCh = { (MTLTextureSwizzle)6, MTLTextureSwizzleGreen,
                MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };

            MTLTextureDescriptor *dBadEnum = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                width:4 height:4 mipmapped:NO];
            dBadEnum.usage = MTLTextureUsageShaderRead;
            dBadEnum.swizzle = badCh;
            require([dev newTextureWithDescriptor:dBadEnum] == nil, @"bad swizzle enum accepted on descriptor");

            MTLTextureDescriptor *dRT = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:4 height:4 mipmapped:NO];
            dRT.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            dRT.swizzle = swapRB;
            require([dev newTextureWithDescriptor:dRT] == nil, @"non-identity descriptor swizzle accepted on RenderTarget");

            MTLTextureDescriptor *dWrite = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:4 height:4 mipmapped:NO];
            dWrite.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            dWrite.swizzle = swapRB;
            require([dev newTextureWithDescriptor:dWrite] == nil, @"non-identity descriptor swizzle accepted on ShaderWrite");

            MTLTextureDescriptor *dUnk = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                            width:4 height:4 mipmapped:NO];
            dUnk.usage = MTLTextureUsageUnknown;
            dUnk.swizzle = swapRB;
            require([dev newTextureWithDescriptor:dUnk] == nil, @"non-identity descriptor swizzle accepted on UsageUnknown");

            // Device-created and buffer-backed MTLTextureTypeTextureBuffer must refuse non-identity and bad-enum swizzle.
            MTLTextureDescriptor *dTexel = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                  width:16
                                                                                        resourceOptions:MTLResourceStorageModeShared
                                                                                                  usage:MTLTextureUsageShaderRead];
            id<MTLBuffer> tbBuf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
            require(tbBuf != nil, @"texel backing buffer nil");
            dTexel.swizzle = swapRB;
            require([dev newTextureWithDescriptor:dTexel] == nil, @"non-identity swizzle accepted on device texture buffer");
            require([tbBuf newTextureWithDescriptor:dTexel offset:0 bytesPerRow:0] == nil,
                    @"non-identity swizzle accepted on buffer-backed texture buffer");
            dTexel.swizzle = badCh;
            require([dev newTextureWithDescriptor:dTexel] == nil, @"bad swizzle enum accepted on device texture buffer");

            // Writable parent -> swizzled view must be refused.
            MTLTextureDescriptor *dSt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:4 height:4 mipmapped:NO];
            dSt.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> stTex = [dev newTextureWithDescriptor:dSt];
            require(stTex != nil, @"storage parent nil");
            require([stTex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                             textureType:MTLTextureType2D
                                                  levels:NSMakeRange(0, 1)
                                                  slices:NSMakeRange(0, 1)
                                                 swizzle:swapRB] == nil,
                    @"non-identity swizzle view accepted on ShaderWrite parent");
            require([stTex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                             textureType:MTLTextureType2D
                                                  levels:NSMakeRange(0, 1)
                                                  slices:NSMakeRange(0, 1)
                                                 swizzle:badCh] == nil,
                    @"bad swizzle enum accepted on view");

            // RenderTarget-only parent (no ShaderRead) -> swizzled view must be refused.
            MTLTextureDescriptor *dRtOnly = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                               width:4 height:4 mipmapped:NO];
            dRtOnly.usage = MTLTextureUsageRenderTarget;
            dRtOnly.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rtOnly = [dev newTextureWithDescriptor:dRtOnly];
            require(rtOnly != nil, @"RT-only parent nil");
            require([rtOnly newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                              textureType:MTLTextureType2D
                                                   levels:NSMakeRange(0, 1)
                                                   slices:NSMakeRange(0, 1)
                                                  swizzle:swapRB] == nil,
                    @"non-identity swizzle view accepted on RT-only parent without ShaderRead");
            puts("METAL3_SWIZZLE_CASE_PASS negative_refusals");
        }

        printf("METAL3_SWIZZLE_PASS reference=%d\n", reference ? 1 : 0);
        return 0;
    }
}
