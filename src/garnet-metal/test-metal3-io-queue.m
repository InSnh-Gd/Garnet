// test-metal3-io-queue.m: live macOS / RX 9060 XT verification for Task 4.1
// (MTLIOFileHandle, MTLIOCommandQueue, MTLIOCommandBuffer: async loadBuffer/loadTexture
// into Private resources, guard preservation, early handle release, MTLSharedEvent handoff,
// enqueueBarrier / serial ordering, 8192-command limit, EOF/missing-file/compression/tryCancel).
//
// Build on macOS (verify both -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-io-queue.m -o "$TMPDIR/test-metal3-io-queue"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-io-queue" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-io-queue" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_IO_QUEUE_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static uint8_t patternByte(NSUInteger idx) {
    return (uint8_t)(((idx * 37u + 0x5Du) ^ (idx >> 5)) & 0xFFu);
}

static void finishGPU(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
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
            dev = [[cls alloc] init];
            if ([dev respondsToSelector:sel_registerName("initLimits")])
                ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        require(dev != nil, @"device unavailable");
        id<MTLCommandQueue> gpuQ = [dev newCommandQueue];
        require(gpuQ != nil, @"GPU command queue unavailable");

        // Create deterministic source asset file in TMPDIR.
        const NSUInteger kFileSize = 65536;
        NSMutableData *asset = [NSMutableData dataWithLength:kFileSize];
        uint8_t *ab = asset.mutableBytes;
        for (NSUInteger i = 0; i < kFileSize; i++) ab[i] = patternByte(i);
        NSString *tmpDir = NSTemporaryDirectory();
        NSString *filePath = [tmpDir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"garnet-io-test-%d.bin", (int)getpid()]];
        NSError *err = nil;
        require([asset writeToFile:filePath options:NSDataWritingAtomic error:&err],
                [NSString stringWithFormat:@"write asset file: %@", err]);
        NSURL *fileURL = [NSURL fileURLWithPath:filePath];

        // Compute shader: reads Private buffer bytes + Private mipmapped texture texels (mip 0 & mip 1)
        // into a Shared verification buffer.
        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void verify_io(device const uchar *srcBuf [[buffer(0)]],\n"
             "                      device uchar *outBuf [[buffer(1)]],\n"
             "                      texture2d<uint, access::read> tex [[texture(0)]],\n"
             "                      device uint *outMip0 [[buffer(2)]],\n"
             "                      device uint *outMip1 [[buffer(3)]],\n"
             "                      uint tid [[thread_position_in_grid]]) {\n"
             "    if (tid < 2048) outBuf[tid] = srcBuf[tid];\n"
             "    if (tid < 256) {\n"
             "        uint2 p0(tid & 15u, tid >> 4u);\n"
             "        uint4 v0 = tex.read(p0, 0);\n"
             "        outMip0[tid] = v0.r | (v0.g << 8) | (v0.b << 16) | (v0.a << 24);\n"
             "    }\n"
             "    if (tid < 64) {\n"
             "        uint2 p1(tid & 7u, tid >> 3u);\n"
             "        uint4 v1 = tex.read(p1, 1);\n"
             "        outMip1[tid] = v1.r | (v1.g << 8) | (v1.b << 16) | (v1.a << 24);\n"
             "    }\n"
             "}\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"compile shader: %@", err]);
        id<MTLComputePipelineState> verifyPso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"verify_io"] error:&err];
        require(verifyPso != nil, [NSString stringWithFormat:@"verifyPso: %@", err]);

        // 1. Concurrent MTLIOCommandQueue: async load Private buffer (with guards) + Private mipmapped texture
        //    (mip 0 subrect with padded bpr + mip 1) + early handle release + MTLSharedEvent handoff to GPU queue.
        MTLIOCommandQueueDescriptor *qd = [MTLIOCommandQueueDescriptor new];
        qd.type = MTLIOCommandQueueTypeConcurrent;
        qd.priority = MTLIOPriorityNormal;
        qd.maxCommandsInFlight = 4;
        id<MTLIOCommandQueue> ioQ = [dev newIOCommandQueueWithDescriptor:qd error:&err];
        require(ioQ != nil, [NSString stringWithFormat:@"newIOCommandQueue failed: %@", err]);
        ioQ.label = @"garnet.test.io.concurrent";

        const NSUInteger kBufTotal = 2048;
        const NSUInteger kGuardHead = 256;
        const NSUInteger kLoadOffset = 256;
        const NSUInteger kLoadSize = 1536;
        const NSUInteger kSrcBufOff = 1337;
        const uint8_t kSentinel = 0xA5;

        id<MTLBuffer> privBuf = [dev newBufferWithLength:kBufTotal options:MTLResourceStorageModePrivate];
        id<MTLBuffer> outBuf = [dev newBufferWithLength:kBufTotal options:MTLResourceStorageModeShared];
        id<MTLBuffer> outMip0 = [dev newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> outMip1 = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> statusBuf = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        require(privBuf && outBuf && outMip0 && outMip1 && statusBuf, @"buffer alloc failed");
        memset(statusBuf.contents, 0xFF, 16);

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
                                                                                      width:16 height:16 mipmapped:YES];
        td.mipmapLevelCount = 2;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> privTex = [dev newTextureWithDescriptor:td];
        require(privTex != nil, @"Private mipmapped texture alloc failed");

        // Pre-fill Private buffer & mip 0/1 of Private texture with sentinel 0xA5 to detect guard overwrites.
        {
            id<MTLBuffer> initTex = [dev newBufferWithLength:16 * 16 * 4 options:MTLResourceStorageModeShared];
            memset(initTex.contents, kSentinel, 16 * 16 * 4);
            id<MTLCommandBuffer> initCB = [gpuQ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [initCB blitCommandEncoder];
            [blit fillBuffer:privBuf range:NSMakeRange(0, kBufTotal) value:kSentinel];
            [blit copyFromBuffer:initTex sourceOffset:0 sourceBytesPerRow:16 * 4 sourceBytesPerImage:16 * 16 * 4
                      sourceSize:MTLSizeMake(16, 16, 1) toTexture:privTex destinationSlice:0 destinationLevel:0
               destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit copyFromBuffer:initTex sourceOffset:0 sourceBytesPerRow:8 * 4 sourceBytesPerImage:8 * 8 * 4
                      sourceSize:MTLSizeMake(8, 8, 1) toTexture:privTex destinationSlice:0 destinationLevel:1
               destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            finishGPU(initCB, @"init_guards");
        }

        // Mip 0 subrect: origin (4, 4), size 8x8 with padded sourceBytesPerRow = 48 (row_bytes = 32).
        const NSUInteger kMip0SrcOff = 4096;
        const NSUInteger kMip0Bpr = 48;
        // Mip 1 full 8x8 tightly packed (bpr = 32).
        const NSUInteger kMip1SrcOff = 8192;
        const NSUInteger kMip1Bpr = 32;

        id<MTLSharedEvent> ioDone = [dev newSharedEvent];
        require(ioDone != nil, @"newSharedEvent failed");

        __block BOOL handlerFired = NO;
        __block MTLIOStatus handlerStatus = MTLIOStatusPending;
        dispatch_semaphore_t handlerSem = dispatch_semaphore_create(0);

        id<MTLIOCommandBuffer> ioCB = [ioQ commandBuffer];
        require(ioCB != nil, @"io commandBuffer nil");
        @autoreleasepool {
            id<MTLIOFileHandle> fh = [dev newIOFileHandleWithURL:fileURL error:&err];
            require(fh != nil, [NSString stringWithFormat:@"newIOFileHandleWithURL failed: %@", err]);
            [ioCB loadBuffer:privBuf offset:kLoadOffset size:kLoadSize
                sourceHandle:fh sourceHandleOffset:kSrcBufOff];
            [ioCB addBarrier];
            [ioCB loadTexture:privTex slice:0 level:0 size:MTLSizeMake(8, 8, 1)
            sourceBytesPerRow:kMip0Bpr sourceBytesPerImage:kMip0Bpr * 8
            destinationOrigin:MTLOriginMake(4, 4, 0)
                 sourceHandle:fh sourceHandleOffset:kMip0SrcOff];
            [ioCB loadTexture:privTex slice:0 level:1 size:MTLSizeMake(8, 8, 1)
            sourceBytesPerRow:kMip1Bpr sourceBytesPerImage:kMip1Bpr * 8
            destinationOrigin:MTLOriginMake(0, 0, 0)
                 sourceHandle:fh sourceHandleOffset:kMip1SrcOff];
            [ioCB copyStatusToBuffer:statusBuf offset:0];
            [ioCB signalEvent:ioDone value:1];
            [ioCB addCompletedHandler:^(id<MTLIOCommandBuffer> b) {
                handlerFired = YES;
                handlerStatus = b.status;
                dispatch_semaphore_signal(handlerSem);
            }];
            // Drop local fh strong reference BEFORE commit to prove handle retention!
        }

        // Enqueue GPU verification command buffer BEFORE committing I/O to prove real event handoff.
        id<MTLCommandBuffer> gpuCB = [gpuQ commandBuffer];
        [gpuCB encodeWaitForEvent:ioDone value:1];
        id<MTLComputeCommandEncoder> enc = [gpuCB computeCommandEncoder];
        [enc setComputePipelineState:verifyPso];
        [enc setBuffer:privBuf offset:0 atIndex:0];
        [enc setBuffer:outBuf offset:0 atIndex:1];
        [enc setTexture:privTex atIndex:0];
        [enc setBuffer:outMip0 offset:0 atIndex:2];
        [enc setBuffer:outMip1 offset:0 atIndex:3];
        [enc dispatchThreads:MTLSizeMake(2048, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [gpuCB commit];

        [ioCB commit];
        [ioCB waitUntilCompleted];
        require(ioCB.status == MTLIOStatusComplete && ioCB.error == nil,
                [NSString stringWithFormat:@"ioCB failed: status=%ld err=%@", (long)ioCB.status, ioCB.error]);
        require(dispatch_semaphore_wait(handlerSem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0 &&
                handlerFired && handlerStatus == MTLIOStatusComplete,
                @"completedHandler did not report MTLIOStatusComplete");
        require(*(const int64_t *)statusBuf.contents == (int64_t)MTLIOStatusComplete,
                @"copyStatusToBuffer did not record MTLIOStatusComplete");

        [gpuCB waitUntilCompleted];
        require(gpuCB.status == MTLCommandBufferStatusCompleted && !gpuCB.error,
                [NSString stringWithFormat:@"gpuCB failed: status=%lu err=%@", (unsigned long)gpuCB.status, gpuCB.error]);

        // Verify bit-exact Private buffer contents and untouched guards.
        const uint8_t *gotBuf = outBuf.contents;
        for (NSUInteger i = 0; i < kBufTotal; i++) {
            uint8_t want = (i >= kGuardHead && i < kGuardHead + kLoadSize)
                ? ab[kSrcBufOff + (i - kGuardHead)] : kSentinel;
            require(gotBuf[i] == want,
                    [NSString stringWithFormat:@"buffer byte %lu mismatch: got 0x%02x want 0x%02x",
                     (unsigned long)i, gotBuf[i], want]);
        }

        // Verify mip 0 (8x8 subrect at (4,4) + surrounding sentinel guard texels) and mip 1.
        const uint32_t *gotM0 = outMip0.contents;
        const uint32_t sentinelPx = 0xA5A5A5A5u;
        for (NSUInteger y = 0; y < 16; y++) {
            for (NSUInteger x = 0; x < 16; x++) {
                uint32_t want = sentinelPx;
                if (x >= 4 && x < 12 && y >= 4 && y < 12) {
                    NSUInteger off = kMip0SrcOff + (y - 4) * kMip0Bpr + (x - 4) * 4;
                    memcpy(&want, ab + off, 4);
                }
                require(gotM0[y * 16 + x] == want,
                        [NSString stringWithFormat:@"mip0 (%lu,%lu) mismatch: got 0x%08x want 0x%08x",
                         (unsigned long)x, (unsigned long)y, gotM0[y * 16 + x], want]);
            }
        }
        const uint32_t *gotM1 = outMip1.contents;
        for (NSUInteger y = 0; y < 8; y++) {
            for (NSUInteger x = 0; x < 8; x++) {
                uint32_t want = 0;
                memcpy(&want, ab + kMip1SrcOff + y * kMip1Bpr + x * 4, 4);
                require(gotM1[y * 8 + x] == want,
                        [NSString stringWithFormat:@"mip1 (%lu,%lu) mismatch: got 0x%08x want 0x%08x",
                         (unsigned long)x, (unsigned long)y, gotM1[y * 8 + x], want]);
            }
        }

        // 2. Serial MTLIOCommandQueue + enqueueBarrier + loadBytes into host memory.
        {
            MTLIOCommandQueueDescriptor *sqd = [MTLIOCommandQueueDescriptor new];
            sqd.type = MTLIOCommandQueueTypeSerial;
            sqd.priority = MTLIOPriorityHigh;
            id<MTLIOCommandQueue> serialQ = [dev newIOCommandQueueWithDescriptor:sqd error:&err];
            require(serialQ != nil, [NSString stringWithFormat:@"serial IO queue failed: %@", err]);
            id<MTLIOFileHandle> fh = [dev newIOFileHandleWithURL:fileURL error:&err];
            require(fh != nil, @"reopen handle for serial test failed");

            uint8_t hostDst[512];
            memset(hostDst, 0xEE, sizeof hostDst);
            id<MTLIOCommandBuffer> cbA = [serialQ commandBuffer];
            [cbA loadBytes:hostDst + 64 size:256 sourceHandle:fh sourceHandleOffset:100];
            [cbA enqueue];
            [serialQ enqueueBarrier];
            id<MTLIOCommandBuffer> cbB = [serialQ commandBuffer];
            [cbB loadBytes:hostDst + 128 size:128 sourceHandle:fh sourceHandleOffset:500];
            [cbB enqueue];
            // Commit B before A: serial + enqueueBarrier must still execute A before B.
            [cbB commit];
            [cbA commit];
            [cbB waitUntilCompleted];
            [cbA waitUntilCompleted];
            require(cbA.status == MTLIOStatusComplete && cbB.status == MTLIOStatusComplete,
                    @"serial/barrier IO command buffers failed");
            for (NSUInteger i = 0; i < sizeof hostDst; i++) {
                uint8_t want = 0xEE;
                if (i >= 128 && i < 256) want = ab[500 + (i - 128)];
                else if (i >= 64 && i < 320) want = ab[100 + (i - 64)];
                require(hostDst[i] == want, @"serial/barrier loadBytes ordering or guard mismatch");
            }
        }

        // 3. Negative checks: missing file, unsupported compression, EOF overrun, >8192 commands, tryCancel.
        {
            NSURL *missing = [NSURL fileURLWithPath:@"/nonexistent-garnet-io-path-999999.bin"];
            err = nil;
            id<MTLIOFileHandle> badMissing = [dev newIOFileHandleWithURL:missing error:&err];
            require(badMissing == nil && err != nil, @"missing file must fail with NSError");

            if (!reference) {
                err = nil;
                id<MTLIOFileHandle> badComp = [dev newIOFileHandleWithURL:fileURL
                                                        compressionMethod:MTLIOCompressionMethodLZ4
                                                                    error:&err];
                require(badComp == nil && err != nil, @"unsupported compression must fail cleanly with NSError");
            }

            id<MTLIOFileHandle> fh = [dev newIOFileHandleWithURL:fileURL error:&err];
            require(fh != nil, @"reopen handle failed");

            // EOF overrun.
            id<MTLBuffer> smallBuf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
            memset(smallBuf.contents, 0xCC, 256);
            id<MTLIOCommandBuffer> oobCB = [ioQ commandBuffer];
            [oobCB loadBuffer:smallBuf offset:0 size:128 sourceHandle:fh sourceHandleOffset:kFileSize - 32];
            [oobCB commit];
            [oobCB waitUntilCompleted];
            require(oobCB.status == MTLIOStatusError && oobCB.error != nil,
                    [NSString stringWithFormat:@"EOF overrun must fail (status=%ld err=%@)",
                     (long)oobCB.status, oobCB.error]);

            // 8,192 commands pass; 8,193rd fails.
            id<MTLIOCommandBuffer> okMax = [ioQ commandBuffer];
            for (NSUInteger i = 0; i < 8192; i++) [okMax addBarrier];
            [okMax commit];
            [okMax waitUntilCompleted];
            require(okMax.status == MTLIOStatusComplete, @"8192 commands must succeed");

            if (!reference) {
                id<MTLIOCommandBuffer> overMax = [ioQ commandBuffer];
                for (NSUInteger i = 0; i < 8193; i++) [overMax addBarrier];
                [overMax commit];
                [overMax waitUntilCompleted];
                require(overMax.status == MTLIOStatusError && overMax.error != nil,
                        @"8193rd command must fail with MTLIOStatusError and non-nil error");
            }

            // tryCancel before commit & while waiting on an unsignaled event.
            id<MTLIOCommandBuffer> cancelPre = [ioQ commandBuffer];
            [cancelPre loadBuffer:smallBuf offset:0 size:64 sourceHandle:fh sourceHandleOffset:0];
            [cancelPre tryCancel];
            [cancelPre commit];
            [cancelPre waitUntilCompleted];
            require(cancelPre.status == MTLIOStatusCancelled,
                    [NSString stringWithFormat:@"pre-commit cancel status=%ld", (long)cancelPre.status]);
            if (!reference) require(cancelPre.error != nil, @"cancelled buffer must populate error");

            id<MTLSharedEvent> neverEvent = [dev newSharedEvent];
            id<MTLIOCommandBuffer> cancelWait = [ioQ commandBuffer];
            [cancelWait waitForEvent:neverEvent value:42];
            [cancelWait loadBuffer:smallBuf offset:0 size:64 sourceHandle:fh sourceHandleOffset:0];
            [cancelWait commit];
            usleep(20000);
            [cancelWait tryCancel];
            [cancelWait waitUntilCompleted];
            require(cancelWait.status == MTLIOStatusCancelled,
                    [NSString stringWithFormat:@"wait-event cancel status=%ld", (long)cancelWait.status]);
        }

        unlink(filePath.fileSystemRepresentation);
        printf("METAL3_IO_QUEUE_PASS (%s)\n", reference ? "reference" : "garnet");
        return 0;
    }
}
