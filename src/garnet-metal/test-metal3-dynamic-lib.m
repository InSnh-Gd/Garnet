// test-metal3-dynamic-lib.m: live RX 9060 XT / reference Metal verification for Task 3.2
// (MTLDynamicLibrary: newDynamicLibrary:error:, serializeToURL:error: ('GRDL' on Garnet),
// newDynamicLibraryWithURL:error: reload, compute + vertex + fragment dynamic library linking via
// preloadedLibraries / vertexPreloadedLibraries / fragmentPreloadedLibraries, MTLLinkedFunctions.binaryFunctions,
// and negative checks for non-dynamic library, missing symbol, and corrupt/truncated/foreign URLs).
// Build on macOS (verify both -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-dynamic-lib.m -o "$TMPDIR/test-metal3-dynamic-lib"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-dynamic-lib" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-dynamic-lib" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
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
    fprintf(stderr, "METAL3_DYNAMIC_LIB_FAIL %s\n", msg.UTF8String);
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

static void runCompute(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                       const uint32_t *inWords, uint32_t *outWords, NSUInteger count, NSString *tag) {
    const NSUInteger bytes = count * sizeof(uint32_t);
    id<MTLBuffer> inBuf = [dev newBufferWithBytes:inWords length:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outBuf = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(inBuf && outBuf && outBuf.contents, [NSString stringWithFormat:@"%@: buffers nil", tag]);
    memset(outBuf.contents, 0xA5, bytes);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: compute encoder nil", tag]);
    [enc setComputePipelineState:pso];
    [enc setBuffer:inBuf offset:0 atIndex:0];
    [enc setBuffer:outBuf offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finish(cb, tag);
    memcpy(outWords, outBuf.contents, bytes);
}

static void runRender(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso,
                      uint8_t *outPixels, NSUInteger w, NSUInteger h, NSString *tag) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    require(rt != nil, [NSString stringWithFormat:@"%@: render target nil", tag]);

    const NSUInteger bpr = w * 4;
    const NSUInteger bytes = bpr * h;
    id<MTLBuffer> readback = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(readback && readback.contents, [NSString stringWithFormat:@"%@: readback buffer nil", tag]);
    memset(readback.contents, 0x5A, bytes);

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    require(enc != nil, [NSString stringWithFormat:@"%@: render encoder nil", tag]);
    [enc setRenderPipelineState:pso];
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

static void expectDynlibLoadRejected(id<MTLDevice> dev, NSURL *url, NSString *tag) {
    NSError *err = nil;
    id<MTLDynamicLibrary> bad = [dev newDynamicLibraryWithURL:url error:&err];
    require(bad == nil && err != nil && [err.domain isEqualToString:MTLDynamicLibraryDomain] &&
            err.code != MTLDynamicLibraryErrorNone,
            [NSString stringWithFormat:@"%@: expected MTLDynamicLibraryDomain rejection, got lib=%@ err=%@",
             tag, bad, err]);
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

        NSString *installName = @"@executable_path/libgarnet_dyn_test.metallib";
        NSString *dynSrc =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "[[visible]] uint dyn_compute_xform(uint v, uint idx) {\n"
             "    return v * 5u + idx * 3u + 11u;\n"
             "}\n"
             "[[visible]] float4 dyn_vertex_pos(uint vid) {\n"
             "    float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };\n"
             "    return float4(p[vid], 0.0f, 1.0f);\n"
             "}\n"
             "[[visible]] float4 dyn_fragment_shade(float4 base) {\n"
             "    return float4(base.r * 0.5f + 0.25f, base.g + 0.5f, base.b * 0.25f + 0.75f, 1.0f);\n"
             "}\n";

        MTLCompileOptions *dynOpts = [MTLCompileOptions new];
        dynOpts.languageVersion = MTLLanguageVersion2_4;
        dynOpts.libraryType = MTLLibraryTypeDynamic;
        dynOpts.installName = installName;
        NSError *err = nil;
        id<MTLLibrary> dynMtllib = [dev newLibraryWithSource:dynSrc options:dynOpts error:&err];
        require(dynMtllib != nil, [NSString stringWithFormat:@"compile dynamic MTLLibrary failed: %@", err]);

        id<MTLDynamicLibrary> dynLib = [dev newDynamicLibrary:dynMtllib error:&err];
        require(dynLib != nil, [NSString stringWithFormat:@"newDynamicLibrary:error: failed: %@", err]);
        require([dynLib.installName isEqualToString:installName],
                [NSString stringWithFormat:@"installName mismatch: %@", dynLib.installName]);
        require(dynLib.device == dev, @"dynLib.device mismatch");
        dynLib.label = @"garnet.test.dynlib";
        require([dynLib.label isEqualToString:@"garnet.test.dynlib"], @"dynLib.label mismatch");

        // Serialize to disk and reload from URL.
        NSString *tmpDir = [NSTemporaryDirectory() stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"garnet-dynlib-%d", (int)getpid()]];
        require([[NSFileManager defaultManager] createDirectoryAtPath:tmpDir
                                          withIntermediateDirectories:YES attributes:nil error:&err],
                [NSString stringWithFormat:@"mkdir tmpDir failed: %@", err]);
        NSURL *grdlUrl = [NSURL fileURLWithPath:[tmpDir stringByAppendingPathComponent:@"libtest.grdl"]];
        require([dynLib serializeToURL:grdlUrl error:&err],
                [NSString stringWithFormat:@"serializeToURL:error: failed: %@", err]);
        if (!reference) {
            NSData *raw = [NSData dataWithContentsOfURL:grdlUrl];
            require(raw.length >= 64 && memcmp(raw.bytes, "GRDL", 4) == 0,
                    @"Garnet serialized dynamic library missing GRDL magic");
        }

        id<MTLDynamicLibrary> reloaded = [dev newDynamicLibraryWithURL:grdlUrl error:&err];
        require(reloaded != nil, [NSString stringWithFormat:@"newDynamicLibraryWithURL:error: failed: %@", err]);
        require([reloaded.installName isEqualToString:installName],
                [NSString stringWithFormat:@"reloaded installName mismatch: %@", reloaded.installName]);

        // 1. Compute + Vertex + Fragment caller library using dynamic library symbols.
        NSString *declQual = reference ? @"extern" : @"[[visible]]";
        NSString *callerSrc = [NSString stringWithFormat:
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "%@ uint dyn_compute_xform(uint v, uint idx);\n"
             "%@ float4 dyn_vertex_pos(uint vid);\n"
             "%@ float4 dyn_fragment_shade(float4 base);\n"
             "kernel void dl_compute_main(device const uint *inA [[buffer(0)]],\n"
             "                            device uint *outB [[buffer(1)]],\n"
             "                            uint gid [[thread_position_in_grid]]) {\n"
             "    outB[gid] = dyn_compute_xform(inA[gid], gid);\n"
             "}\n"
             "vertex float4 dl_vs_main(uint vid [[vertex_id]]) {\n"
             "    return dyn_vertex_pos(vid);\n"
             "}\n"
             "fragment float4 dl_fs_main(float4 pos [[position]]) {\n"
             "    return dyn_fragment_shade(float4(0.5f, 0.0f, 0.5f, 1.0f));\n"
             "}\n", declQual, declQual, declQual];

        MTLCompileOptions *callerOpts = [MTLCompileOptions new];
        callerOpts.languageVersion = MTLLanguageVersion2_4;
        if (reference) callerOpts.libraries = @[ reloaded ];
        id<MTLLibrary> callerLib = [dev newLibraryWithSource:callerSrc options:callerOpts error:&err];
        require(callerLib != nil, [NSString stringWithFormat:@"compile caller library failed: %@", err]);

        id<MTLFunction> computeFn = [callerLib newFunctionWithName:@"dl_compute_main"];
        id<MTLFunction> vertexFn = [callerLib newFunctionWithName:@"dl_vs_main"];
        id<MTLFunction> fragmentFn = [callerLib newFunctionWithName:@"dl_fs_main"];
        require(computeFn && vertexFn && fragmentFn, @"caller functions missing");

        // Compute pipeline with preloadedLibraries.
        MTLComputePipelineDescriptor *cpd = [MTLComputePipelineDescriptor new];
        cpd.computeFunction = computeFn;
        cpd.preloadedLibraries = @[ reloaded ];
        id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithDescriptor:cpd options:0 reflection:nil error:&err];
        require(cpso != nil, [NSString stringWithFormat:@"compute PSO with preloadedLibraries failed: %@", err]);

        #define DL_TEST_N 64u
        uint32_t inA[DL_TEST_N], outB[DL_TEST_N];
        for (uint32_t i = 0; i < DL_TEST_N; i++) inA[i] = i * 7u + 3u;
        runCompute(dev, q, cpso, inA, outB, DL_TEST_N, @"dynlib-compute");
        for (uint32_t i = 0; i < DL_TEST_N; i++) {
            uint32_t exp = inA[i] * 5u + i * 3u + 11u;
            require(outB[i] == exp,
                    [NSString stringWithFormat:@"dynlib-compute[%u]: got %u expected %u", i, outB[i], exp]);
        }

        // Render pipeline with vertexPreloadedLibraries & fragmentPreloadedLibraries.
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = vertexFn;
        rpd.fragmentFunction = fragmentFn;
        rpd.vertexPreloadedLibraries = @[ reloaded ];
        rpd.fragmentPreloadedLibraries = @[ reloaded ];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        require(rpso != nil, [NSString stringWithFormat:@"render PSO with preloadedLibraries failed: %@", err]);

        #define DL_TEST_W 16u
        #define DL_TEST_H 16u
        uint8_t pixels[DL_TEST_W * DL_TEST_H * 4u];
        runRender(dev, q, rpso, pixels, DL_TEST_W, DL_TEST_H, @"dynlib-render");
        // Expected: (0.5*0.5+0.25, 0+0.5, 0.5*0.25+0.75, 1.0) = (0.5, 0.5, 0.875, 1.0) -> (128, 128, 223, 255)
        const int expRgba[4] = { 128, 128, 223, 255 };
        for (NSUInteger p = 0; p < (NSUInteger)DL_TEST_W * (NSUInteger)DL_TEST_H; p++) {
            for (int ch = 0; ch < 4; ch++) {
                int diff = (int)pixels[p * 4 + (NSUInteger)ch] - expRgba[ch];
                if (diff < 0) diff = -diff;
                require(diff <= 1,
                        [NSString stringWithFormat:@"dynlib-render px %lu ch %d: got %u expected %d",
                         (unsigned long)p, ch, pixels[p * 4 + (NSUInteger)ch], expRgba[ch]]);
            }
        }

        // 2. MTLLinkedFunctions.binaryFunctions compute test.
        NSString *binSrc =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "[[visible]] uint bin_mix(uint x) { return (x ^ 0x5Au) + 7u; }\n";
        MTLCompileOptions *stdOpts = [MTLCompileOptions new];
        stdOpts.languageVersion = MTLLanguageVersion2_4;
        id<MTLLibrary> binLib = [dev newLibraryWithSource:binSrc options:stdOpts error:&err];
        require(binLib != nil, [NSString stringWithFormat:@"compile binaryFunctions lib failed: %@", err]);
        id<MTLFunction> binFn = [binLib newFunctionWithName:@"bin_mix"];
        require(binFn != nil, @"bin_mix function missing");

        NSString *binCallerSrc =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "[[visible]] uint bin_mix(uint x);\n"
             "kernel void bin_caller(device const uint *inA [[buffer(0)]],\n"
             "                       device uint *outB [[buffer(1)]],\n"
             "                       uint gid [[thread_position_in_grid]]) {\n"
             "    outB[gid] = bin_mix(inA[gid]);\n"
             "}\n";
        id<MTLLibrary> binCallerLib = [dev newLibraryWithSource:binCallerSrc options:stdOpts error:&err];
        require(binCallerLib != nil, [NSString stringWithFormat:@"compile binCallerLib failed: %@", err]);
        id<MTLFunction> binCallerFn = [binCallerLib newFunctionWithName:@"bin_caller"];
        require(binCallerFn != nil, @"bin_caller function missing");

        MTLComputePipelineDescriptor *bcpd = [MTLComputePipelineDescriptor new];
        bcpd.computeFunction = binCallerFn;
        MTLLinkedFunctions *blf = [MTLLinkedFunctions linkedFunctions];
        if (!reference) blf.binaryFunctions = @[ binFn ];
        else blf.functions = @[ binFn ];
        bcpd.linkedFunctions = blf;
        id<MTLComputePipelineState> bcpso = [dev newComputePipelineStateWithDescriptor:bcpd options:0 reflection:nil error:&err];
        require(bcpso != nil, [NSString stringWithFormat:@"compute PSO with binaryFunctions failed: %@", err]);
        runCompute(dev, q, bcpso, inA, outB, DL_TEST_N, @"binary-functions-compute");
        for (uint32_t i = 0; i < DL_TEST_N; i++) {
            uint32_t exp = (inA[i] ^ 0x5Au) + 7u;
            require(outB[i] == exp,
                    [NSString stringWithFormat:@"binaryFunctions[%u]: got %u expected %u", i, outB[i], exp]);
        }

        // 3. Negative checks: non-dynamic library, missing symbol, corrupt/truncated/foreign/missing URLs.
        {
            NSError *ne = nil;
            id<MTLDynamicLibrary> nonDyn = [dev newDynamicLibrary:binLib error:&ne];
            require(nonDyn == nil && ne != nil && [ne.domain isEqualToString:MTLDynamicLibraryDomain],
                    [NSString stringWithFormat:@"expected non-dynamic library rejection, got lib=%@ err=%@", nonDyn, ne]);
        }
        if (!reference) {
            NSString *wrongDynSrc =
                @"#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "[[visible]] uint unrelated_sym(uint x) { return x + 1u; }\n";
            MTLCompileOptions *wrongOpts = [MTLCompileOptions new];
            wrongOpts.languageVersion = MTLLanguageVersion2_4;
            wrongOpts.libraryType = MTLLibraryTypeDynamic;
            wrongOpts.installName = @"@executable_path/libwrong.metallib";
            id<MTLLibrary> wrongMtllib = [dev newLibraryWithSource:wrongDynSrc options:wrongOpts error:&err];
            require(wrongMtllib != nil, @"compile wrong dynamic library failed");
            id<MTLDynamicLibrary> wrongDyn = [dev newDynamicLibrary:wrongMtllib error:&err];
            require(wrongDyn != nil, @"newDynamicLibrary for wrong library failed");

            MTLComputePipelineDescriptor *badCpd = [MTLComputePipelineDescriptor new];
            badCpd.computeFunction = computeFn;
            badCpd.preloadedLibraries = @[ wrongDyn ];
            NSError *me = nil;
            id<MTLComputePipelineState> badPso =
                [dev newComputePipelineStateWithDescriptor:badCpd options:0 reflection:nil error:&me];
            require(badPso == nil && me != nil,
                    [NSString stringWithFormat:@"missing symbol must fail pipeline creation, got pso=%@ err=%@", badPso, me]);
        }
        {
            NSURL *missingUrl = [NSURL fileURLWithPath:[tmpDir stringByAppendingPathComponent:@"missing.grdl"]];
            expectDynlibLoadRejected(dev, missingUrl, @"missing-url");

            NSData *goodRaw = [NSData dataWithContentsOfURL:grdlUrl];
            require(goodRaw.length >= 64, @"serialized file too short");

            NSMutableData *tampered = [goodRaw mutableCopy];
            ((uint8_t *)tampered.mutableBytes)[tampered.length - 1] ^= 0xFFu;
            NSURL *tamperUrl = [NSURL fileURLWithPath:[tmpDir stringByAppendingPathComponent:@"tampered.grdl"]];
            require([tampered writeToURL:tamperUrl atomically:YES], @"write tampered file");
            expectDynlibLoadRejected(dev, tamperUrl, @"tampered-url");

            NSData *trunc = [goodRaw subdataWithRange:NSMakeRange(0, 32)];
            NSURL *truncUrl = [NSURL fileURLWithPath:[tmpDir stringByAppendingPathComponent:@"truncated.grdl"]];
            require([trunc writeToURL:truncUrl atomically:YES], @"write truncated file");
            expectDynlibLoadRejected(dev, truncUrl, @"truncated-url");

            const uint8_t foreignBytes[64] = { 'M', 'T', 'L', 'B', 1, 0, 2, 0 };
            NSData *foreign = [NSData dataWithBytes:foreignBytes length:sizeof(foreignBytes)];
            NSURL *foreignUrl = [NSURL fileURLWithPath:[tmpDir stringByAppendingPathComponent:@"foreign.metallib"]];
            require([foreign writeToURL:foreignUrl atomically:YES], @"write foreign MTLB file");
            expectDynlibLoadRejected(dev, foreignUrl, @"foreign-mtlb-url");
        }

        [[NSFileManager defaultManager] removeItemAtPath:tmpDir error:nil];
        printf("METAL3_DYNAMIC_LIB_PASS mode=%s compute=%u render=%ux%u binaryFunctions=ok negative=ok\n",
               reference ? "reference" : "garnet", DL_TEST_N, DL_TEST_W, DL_TEST_H);
        _exit(0);
    }
}
