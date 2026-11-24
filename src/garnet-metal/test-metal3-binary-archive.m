// test-metal3-binary-archive.m: live RX 9060 XT / reference Metal verification for Task 3.1
// (MTLBinaryArchive: addComputePipelineFunctionsWithDescriptor:, addRenderPipelineFunctionsWithDescriptor:,
// addFunctionWithDescriptor:library:error:, tile pipeline refusal, serializeToURL:, reload in fresh archive,
// MTLPipelineOptionFailOnBinaryArchiveMiss hit & miss, bit-exact compute+render GPU execution, and corrupt/tampered rejection).
// Build on macOS (verify both -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-binary-archive.m -o "$TMPDIR/test-metal3-binary-archive"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-binary-archive" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-binary-archive" --reference
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
    fprintf(stderr, "METAL3_BINARY_ARCHIVE_FAIL %s\n", msg.UTF8String);
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

static void expectArchiveLoadRejected(id<MTLDevice> dev, NSURL *url, NSString *tag) {
    MTLBinaryArchiveDescriptor *bd = [MTLBinaryArchiveDescriptor new];
    bd.url = url;
    NSError *err = nil;
    id<MTLBinaryArchive> bad = [dev newBinaryArchiveWithDescriptor:bd error:&err];
    require(bad == nil && err != nil && [err.domain isEqualToString:MTLBinaryArchiveDomain] &&
            err.code != MTLBinaryArchiveErrorNone,
            [NSString stringWithFormat:@"%@: expected MTLBinaryArchiveDomain rejection, got archive=%@ err=%@",
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

        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void ba_compute_add(device const uint *inA [[buffer(0)]],\n"
             "                           device uint *outB [[buffer(1)]],\n"
             "                           uint i [[thread_position_in_grid]]) {\n"
             "    outB[i] = inA[i] * 3u + 17u + i;\n"
             "}\n"
             "kernel void ba_compute_miss(device uint *outB [[buffer(0)]],\n"
             "                            uint i [[thread_position_in_grid]]) {\n"
             "    outB[i] = i ^ 0x55AAu;\n"
             "}\n"
             "struct VSOut { float4 pos [[position]]; };\n"
             "vertex VSOut ba_vs(uint vid [[vertex_id]]) {\n"
             "    const float2 pts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
             "    VSOut o; o.pos = float4(pts[vid], 0.0, 1.0); return o;\n"
             "}\n"
             "fragment float4 ba_fs(VSOut in [[stage_in]]) {\n"
             "    (void)in;\n"
             "    return float4(0.25, 0.5, 0.75, 1.0);\n"
             "}\n";

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"library compile: %@", err]);
        id<MTLFunction> fnComp = [lib newFunctionWithName:@"ba_compute_add"];
        id<MTLFunction> fnMiss = [lib newFunctionWithName:@"ba_compute_miss"];
        id<MTLFunction> fnVS = [lib newFunctionWithName:@"ba_vs"];
        id<MTLFunction> fnFS = [lib newFunctionWithName:@"ba_fs"];
        require(fnComp && fnMiss && fnVS && fnFS, @"shader functions missing");

        // 1. Online reference compute + render execution.
        enum { kCount = 64, kW = 16, kH = 16 };
        uint32_t inWords[kCount], refCompOut[kCount], archCompOut[kCount];
        for (uint32_t i = 0; i < kCount; i++) inWords[i] = i * 11u + 5u;

        MTLComputePipelineDescriptor *compDesc = [MTLComputePipelineDescriptor new];
        compDesc.computeFunction = fnComp;
        id<MTLComputePipelineState> refCompPSO =
            [dev newComputePipelineStateWithDescriptor:compDesc options:MTLPipelineOptionNone reflection:NULL error:&err];
        require(refCompPSO != nil, [NSString stringWithFormat:@"ref compute pipeline: %@", err]);
        runCompute(dev, q, refCompPSO, inWords, refCompOut, kCount, @"ref-compute");
        for (uint32_t i = 0; i < kCount; i++) {
            uint32_t exp = inWords[i] * 3u + 17u + i;
            require(refCompOut[i] == exp, [NSString stringWithFormat:@"ref compute[%u]=%u != %u", i, refCompOut[i], exp]);
        }

        MTLRenderPipelineDescriptor *rendDesc = [MTLRenderPipelineDescriptor new];
        rendDesc.vertexFunction = fnVS;
        rendDesc.fragmentFunction = fnFS;
        rendDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> refRendPSO = [dev newRenderPipelineStateWithDescriptor:rendDesc error:&err];
        require(refRendPSO != nil, [NSString stringWithFormat:@"ref render pipeline: %@", err]);
        uint8_t refPixels[kW * kH * 4], archPixels[kW * kH * 4];
        runRender(dev, q, refRendPSO, refPixels, kW, kH, @"ref-render");
        require(refPixels[0] >= 63 && refPixels[0] <= 65 &&
                refPixels[1] >= 127 && refPixels[1] <= 129 &&
                refPixels[2] >= 190 && refPixels[2] <= 192 &&
                refPixels[3] == 255,
                [NSString stringWithFormat:@"ref pixel unexpected (%u,%u,%u,%u)",
                 refPixels[0], refPixels[1], refPixels[2], refPixels[3]]);

        // 2. Populate MTLBinaryArchive with compute + render + standalone function and serialize.
        NSString *tmpRoot = [NSTemporaryDirectory() stringByAppendingPathComponent:
                             [NSString stringWithFormat:@"metal3-ba-%d", getpid()]];
        NSFileManager *fm = [NSFileManager defaultManager];
        require([fm createDirectoryAtPath:tmpRoot withIntermediateDirectories:YES attributes:nil error:&err],
                [NSString stringWithFormat:@"mkdir tmpRoot: %@", err]);
        NSURL *archiveURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"pipelines.grba"]];

        MTLBinaryArchiveDescriptor *bd = [MTLBinaryArchiveDescriptor new];
        id<MTLBinaryArchive> archive = [dev newBinaryArchiveWithDescriptor:bd error:&err];
        require(archive != nil, [NSString stringWithFormat:@"new empty archive: %@", err]);
        archive.label = @"metal3-binary-archive-test";
        require([archive.label isEqualToString:@"metal3-binary-archive-test"] && archive.device == dev,
                @"archive label/device mismatch");

        require([archive addComputePipelineFunctionsWithDescriptor:compDesc error:&err],
                [NSString stringWithFormat:@"addComputePipelineFunctions: %@", err]);
        require([archive addRenderPipelineFunctionsWithDescriptor:rendDesc error:&err],
                [NSString stringWithFormat:@"addRenderPipelineFunctions: %@", err]);

        MTLFunctionDescriptor *fd = [MTLFunctionDescriptor functionDescriptor];
        fd.name = @"ba_compute_add";
        require([archive addFunctionWithDescriptor:fd library:lib error:&err],
                [NSString stringWithFormat:@"addFunctionWithDescriptor: %@", err]);

        // Discrete AMD RDNA 4 (EX04): tile render pipelines must be rejected cleanly with MTLBinaryArchiveDomain.
        if (!reference) {
            MTLTileRenderPipelineDescriptor *td = [MTLTileRenderPipelineDescriptor new];
            NSError *tileErr = nil;
            BOOL tileOK = [archive addTileRenderPipelineFunctionsWithDescriptor:td error:&tileErr];
            require(!tileOK && tileErr != nil && [tileErr.domain isEqualToString:MTLBinaryArchiveDomain] &&
                    tileErr.code != MTLBinaryArchiveErrorNone,
                    [NSString stringWithFormat:@"tile archive entry must fail cleanly, ok=%d err=%@", tileOK, tileErr]);
        }

        require([archive serializeToURL:archiveURL error:&err],
                [NSString stringWithFormat:@"serializeToURL: %@", err]);
        NSData *rawArchive = [NSData dataWithContentsOfURL:archiveURL];
        require(rawArchive.length >= 128, [NSString stringWithFormat:@"serialized archive too short (%lu)", (unsigned long)rawArchive.length]);
        if (!reference) {
            require(memcmp(rawArchive.bytes, "GRBA", 4) == 0, @"serialized Garnet archive missing GRBA magic");
        }
        archive = nil;

        // 3. Reload into a fresh archive instance and hit with MTLPipelineOptionFailOnBinaryArchiveMiss.
        MTLBinaryArchiveDescriptor *loadDesc = [MTLBinaryArchiveDescriptor new];
        loadDesc.url = archiveURL;
        id<MTLBinaryArchive> reloaded = [dev newBinaryArchiveWithDescriptor:loadDesc error:&err];
        require(reloaded != nil, [NSString stringWithFormat:@"reload archive: %@", err]);

        MTLComputePipelineDescriptor *hitCompDesc = [MTLComputePipelineDescriptor new];
        hitCompDesc.computeFunction = fnComp;
        hitCompDesc.binaryArchives = @[ reloaded ];
        hitCompDesc.label = @"archive-hit-compute"; // label must not affect key match
        id<MTLComputePipelineState> hitCompPSO =
            [dev newComputePipelineStateWithDescriptor:hitCompDesc
                                               options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                            reflection:NULL error:&err];
        require(hitCompPSO != nil, [NSString stringWithFormat:@"archive compute hit failed: %@", err]);
        runCompute(dev, q, hitCompPSO, inWords, archCompOut, kCount, @"archive-compute");
        require(memcmp(refCompOut, archCompOut, sizeof refCompOut) == 0, @"compute archive output mismatch");

        MTLRenderPipelineDescriptor *hitRendDesc = [MTLRenderPipelineDescriptor new];
        hitRendDesc.vertexFunction = fnVS;
        hitRendDesc.fragmentFunction = fnFS;
        hitRendDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        hitRendDesc.binaryArchives = @[ reloaded ];
        hitRendDesc.label = @"archive-hit-render";
        id<MTLRenderPipelineState> hitRendPSO =
            [dev newRenderPipelineStateWithDescriptor:hitRendDesc
                                              options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                           reflection:NULL error:&err];
        require(hitRendPSO != nil, [NSString stringWithFormat:@"archive render hit failed: %@", err]);
        runRender(dev, q, hitRendPSO, archPixels, kW, kH, @"archive-render");
        require(memcmp(refPixels, archPixels, sizeof refPixels) == 0, @"render archive output mismatch");

        // 4. Fail-on-miss negative tests (uncached compute kernel, missing binaryArchives, mismatched render format).
        {
            MTLComputePipelineDescriptor *missComp = [MTLComputePipelineDescriptor new];
            missComp.computeFunction = fnMiss;
            missComp.binaryArchives = @[ reloaded ];
            NSError *merr = nil;
            id<MTLComputePipelineState> p =
                [dev newComputePipelineStateWithDescriptor:missComp
                                                   options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                reflection:NULL error:&merr];
            require(p == nil && merr != nil, [NSString stringWithFormat:@"expected fail-on-miss for uncached kernel, p=%@ err=%@", p, merr]);

            MTLComputePipelineDescriptor *noArchComp = [MTLComputePipelineDescriptor new];
            noArchComp.computeFunction = fnComp;
            merr = nil;
            p = [dev newComputePipelineStateWithDescriptor:noArchComp
                                                   options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                reflection:NULL error:&merr];
            require(p == nil && merr != nil, [NSString stringWithFormat:@"expected fail-on-miss without binaryArchives, p=%@ err=%@", p, merr]);

            MTLRenderPipelineDescriptor *missRend = [MTLRenderPipelineDescriptor new];
            missRend.vertexFunction = fnVS;
            missRend.fragmentFunction = fnFS;
            missRend.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; // differs from archived RGBA8Unorm
            missRend.binaryArchives = @[ reloaded ];
            merr = nil;
            id<MTLRenderPipelineState> rp =
                [dev newRenderPipelineStateWithDescriptor:missRend
                                                  options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                               reflection:NULL error:&merr];
            require(rp == nil && merr != nil, [NSString stringWithFormat:@"expected fail-on-miss for mismatched render format, rp=%@ err=%@", rp, merr]);
        }

        // 5. Corrupt, truncated, version-tampered, foreign MTLB, and missing archive rejection tests.
        {
            NSMutableData *tampered = [rawArchive mutableCopy];
            ((uint8_t *)tampered.mutableBytes)[tampered.length - 1] ^= 0xFFu;
            NSURL *tamperURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"tampered.grba"]];
            require([tampered writeToURL:tamperURL atomically:YES], @"write tampered");
            expectArchiveLoadRejected(dev, tamperURL, @"tampered-payload");

            NSMutableData *badVer = [rawArchive mutableCopy];
            if (badVer.length >= 8) ((uint8_t *)badVer.mutableBytes)[4] ^= 0x03u;
            NSURL *verURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"badver.grba"]];
            require([badVer writeToURL:verURL atomically:YES], @"write badver");
            expectArchiveLoadRejected(dev, verURL, @"bad-header-version");

            NSData *trunc = [rawArchive subdataWithRange:NSMakeRange(0, 64)];
            NSURL *truncURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"trunc.grba"]];
            require([trunc writeToURL:truncURL atomically:YES], @"write trunc");
            expectArchiveLoadRejected(dev, truncURL, @"truncated");

            const uint8_t foreignBytes[160] = { 'M', 'T', 'L', 'B', 1, 0, 0, 0 };
            NSURL *foreignURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"foreign.metallib"]];
            require([[NSData dataWithBytes:foreignBytes length:sizeof foreignBytes] writeToURL:foreignURL atomically:YES], @"write foreign");
            expectArchiveLoadRejected(dev, foreignURL, @"foreign-mtlb");

            NSURL *missingURL = [NSURL fileURLWithPath:[tmpRoot stringByAppendingPathComponent:@"missing.grba"]];
            expectArchiveLoadRejected(dev, missingURL, @"missing-file");
        }

        [fm removeItemAtPath:tmpRoot error:NULL];
        printf("METAL3_BINARY_ARCHIVE_PASS mode=%s compute=%u render=%ux%u\n",
               reference ? "reference" : "garnet", (unsigned)kCount, (unsigned)kW, (unsigned)kH);
        return 0;
    }
}
