// test-metal3-residency-set.m: live macOS / RX 9060 XT verification for Task 4.2
// (MTLResidencySet: staged vs committed mutations, immutable snapshots across in-flight
// GPU work, queue addResidencySet: and command-buffer useResidencySet:, early ARC release
// of indirect BDA buffers and heap-backed resources at -O0/-O2, and leak-free reclamation).
//
// Build on macOS (verify BOTH -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-residency-set.m -o "$TMPDIR/test-metal3-residency-set"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-residency-set" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-residency-set" --reference
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
    fprintf(stderr, "METAL3_RESIDENCY_SET_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
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
            dev = createGarnetDevice(cls);
        }
        require(dev != nil, @"device unavailable");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        // Compute shader reading an indirect buffer via BDA pointer stored in an argument struct.
        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct IndirectArgs { device const uint *src; };\n"
             "kernel void read_indirect(constant IndirectArgs &args [[buffer(0)]],\n"
             "                          device uint *dst [[buffer(1)]],\n"
             "                          uint i [[thread_position_in_grid]]) {\n"
             "    dst[i] = args.src[i] ^ 0xA55A0000u;\n"
             "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"compile shader: %@", err]);
        id<MTLComputePipelineState> pso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_indirect"] error:&err];
        require(pso != nil, [NSString stringWithFormat:@"pso failed: %@", err]);

        // 1. Staged vs committed accounting & queries.
        MTLResidencySetDescriptor *rsd = [MTLResidencySetDescriptor new];
        rsd.label = @"garnet.test.residency";
        rsd.initialCapacity = 16;
        id<MTLResidencySet> rset = [dev newResidencySetWithDescriptor:rsd error:&err];
        require(rset != nil, [NSString stringWithFormat:@"newResidencySet failed: %@", err]);
        require([rset.label isEqualToString:@"garnet.test.residency"], @"residency label mismatch");
        require(rset.allocationCount == 0 && rset.allocatedSize == 0, @"fresh set must be empty");

        @autoreleasepool {
            id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            id<MTLBuffer> b = [dev newBufferWithLength:8192 options:MTLResourceStorageModePrivate];
            require(a && b, @"test buffers nil");

            [rset addAllocation:a];
            [rset addAllocation:b];
            require(rset.allocationCount == 0 && ![rset containsAllocation:a],
                    @"uncommitted adds must not appear before commit");
            [rset commit];
            require(rset.allocationCount == 2 &&
                    [rset containsAllocation:a] && [rset containsAllocation:b] &&
                    rset.allocatedSize >= 4096 + 8192 &&
                    rset.allAllocations.count == 2,
                    @"committed residency set counts/membership mismatch");

            [rset requestResidency];
            [rset removeAllocation:a];
            require([rset containsAllocation:a] && rset.allocationCount == 2,
                    @"uncommitted remove must not mutate committed set before commit");
            [rset commit];
            require(![rset containsAllocation:a] && [rset containsAllocation:b] && rset.allocationCount == 1,
                    @"committed remove failed");
            [rset removeAllAllocations];
            require(rset.allocationCount == 1, @"uncommitted removeAll mutated committed count");
            [rset commit];
            require(rset.allocationCount == 0 && rset.allocatedSize == 0, @"removeAll+commit must empty set");
            [rset endResidency];
        }

        // 2. Queue-level addResidencySet: keeps indirect BDA buffer alive after early app ARC release
        //    AND after removeAllocation:+commit while GPU work is gated in-flight, then reclaims cleanly.
        const NSUInteger kCount = 256;
        const NSUInteger kBytes = kCount * sizeof(uint32_t);
        id<MTLBuffer> outBuf = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> ptrBuf = [dev newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
        id<MTLSharedEvent> gate = [dev newSharedEvent];
        require(outBuf && ptrBuf && gate, @"shared test buffers/event nil");

        NSUInteger baseAlloc = dev.currentAllocatedSize;
        __weak id weakIndirect = nil;
        id<MTLCommandBuffer> inFlightCB = nil;

        @autoreleasepool {
            id<MTLBuffer> srcStage = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> indirectPriv = [dev newBufferWithLength:kBytes options:MTLResourceStorageModePrivate];
            require(srcStage && indirectPriv, @"indirect buffer alloc failed");
            uint32_t *sp = srcStage.contents;
            for (NSUInteger i = 0; i < kCount; i++) sp[i] = 0x13570000u ^ (uint32_t)i;
            {
                id<MTLCommandBuffer> upCB = [q commandBuffer];
                id<MTLBlitCommandEncoder> blit = [upCB blitCommandEncoder];
                [blit copyFromBuffer:srcStage sourceOffset:0 toBuffer:indirectPriv destinationOffset:0 size:kBytes];
                [blit endEncoding];
                finishGPU(upCB, @"upload_indirect_priv");
            }
            *(uint64_t *)ptrBuf.contents = indirectPriv.gpuAddress;
            weakIndirect = indirectPriv;

            [rset addAllocation:indirectPriv];
            [rset commit];
            [q addResidencySet:rset];

            // Note: deliberately do NOT call [enc useResource:indirectPriv]!
            inFlightCB = [q commandBuffer];
            [inFlightCB encodeWaitForEvent:gate value:1];
            id<MTLComputeCommandEncoder> enc = [inFlightCB computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:ptrBuf offset:0 atIndex:0];
            [enc setBuffer:outBuf offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake(kCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc endEncoding];
            [inFlightCB commit];

            // Remove from residency set and re-commit WHILE inFlightCB is still gated on `gate`!
            [rset removeAllocation:indirectPriv];
            [rset commit];
            // Local strong ref `indirectPriv` drops when leaving this @autoreleasepool!
        }

        // Snapshot retained by inFlightCB MUST still keep indirectPriv alive right now.
        if (!reference) {
            require(weakIndirect != nil, @"indirectPriv deallocated prematurely while GPU work was in flight");
        }

        // Release the event gate and verify bit-exact GPU readback.
        memset(outBuf.contents, 0, kBytes);
        gate.signaledValue = 1;
        [inFlightCB waitUntilCompleted];
        require(inFlightCB.status == MTLCommandBufferStatusCompleted && !inFlightCB.error,
                [NSString stringWithFormat:@"inFlightCB failed: status=%lu err=%@",
                 (unsigned long)inFlightCB.status, inFlightCB.error]);
        inFlightCB = nil;
        [q removeResidencySet:rset];

        const uint32_t *got = outBuf.contents;
        for (NSUInteger i = 0; i < kCount; i++) {
            uint32_t want = (0x13570000u ^ (uint32_t)i) ^ 0xA55A0000u;
            require(got[i] == want,
                    [NSString stringWithFormat:@"indirect read[%lu] mismatch: got 0x%08x want 0x%08x",
                     (unsigned long)i, got[i], want]);
        }

        // After completion, removed allocation must reclaim without VRAM leak.
        if (!reference) {
            for (int t = 0; t < 200 && (weakIndirect != nil || dev.currentAllocatedSize > baseAlloc); t++)
                usleep(5000);
            require(weakIndirect == nil, @"indirectPriv leaked after remove+commit and GPU retirement");
            require(dev.currentAllocatedSize <= baseAlloc,
                    [NSString stringWithFormat:@"VRAM leak: currentAllocatedSize=%lu > base=%lu",
                     (unsigned long)dev.currentAllocatedSize, (unsigned long)baseAlloc]);
        }

        // 3. Command-buffer useResidencySet: with an MTLHeap + early ARC release of both heap and sub-buffer.
        id<MTLSharedEvent> gate2 = [dev newSharedEvent];
        __weak id weakHeap = nil;
        __weak id weakHeapBuf = nil;
        id<MTLCommandBuffer> cb2 = nil;

        @autoreleasepool {
            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.type = MTLHeapTypeAutomatic;
            hd.storageMode = MTLStorageModeShared;
            hd.size = 65536;
            id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
            require(heap != nil, @"heap alloc failed");
            id<MTLBuffer> hbuf = [heap newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
            require(hbuf != nil, @"heap buffer alloc failed");
            uint32_t *hp = hbuf.contents;
            for (NSUInteger i = 0; i < kCount; i++) hp[i] = 0x24680000u ^ (uint32_t)i;
            *(uint64_t *)ptrBuf.contents = hbuf.gpuAddress;
            weakHeap = heap;
            weakHeapBuf = hbuf;

            id<MTLResidencySet> cbSet = [dev newResidencySetWithDescriptor:rsd error:&err];
            require(cbSet != nil, @"cbSet alloc failed");
            [cbSet addAllocation:heap];
            [cbSet commit];

            cb2 = [q commandBuffer];
            [cb2 useResidencySet:cbSet];
            [cb2 encodeWaitForEvent:gate2 value:1];
            id<MTLComputeCommandEncoder> enc = [cb2 computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:ptrBuf offset:0 atIndex:0];
            [enc setBuffer:outBuf offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake(kCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc endEncoding];
            [cb2 commit];
            // Drop cbSet, heap, AND hbuf strong references while cb2 is still gated!
        }

        if (!reference) {
            require(weakHeap != nil && weakHeapBuf != nil,
                    @"heap or heap sub-buffer deallocated prematurely before cb2 completion");
        }
        memset(outBuf.contents, 0, kBytes);
        gate2.signaledValue = 1;
        [cb2 waitUntilCompleted];
        require(cb2.status == MTLCommandBufferStatusCompleted && !cb2.error,
                [NSString stringWithFormat:@"cb2 failed: status=%lu err=%@",
                 (unsigned long)cb2.status, cb2.error]);
        cb2 = nil;

        for (NSUInteger i = 0; i < kCount; i++) {
            uint32_t want = (0x24680000u ^ (uint32_t)i) ^ 0xA55A0000u;
            require(got[i] == want,
                    [NSString stringWithFormat:@"heap indirect read[%lu] mismatch: got 0x%08x want 0x%08x",
                     (unsigned long)i, got[i], want]);
        }
        if (!reference) {
            for (int t = 0; t < 200 && (weakHeap != nil || weakHeapBuf != nil); t++) usleep(5000);
            require(weakHeap == nil && weakHeapBuf == nil,
                    @"heap or sub-buffer leaked after cb2 retirement");
        }

        // 4. Negative check: excessive initialCapacity returns nil + NSError.
        if (!reference) {
            MTLResidencySetDescriptor *badDesc = [MTLResidencySetDescriptor new];
            badDesc.initialCapacity = NSUIntegerMax;
            err = nil;
            id<MTLResidencySet> badSet = [dev newResidencySetWithDescriptor:badDesc error:&err];
            require(badSet == nil && err != nil, @"excessive initialCapacity must return nil + NSError");
        }

        printf("METAL3_RESIDENCY_SET_PASS (%s)\n", reference ? "reference" : "garnet");
        return 0;
    }
}
