// test-metal3-placement-heap.m: live RX 9060 XT / reference Metal verification for Task 2.2
// (MTLHeapTypePlacement: newBufferWithLength:options:offset:, newTextureWithDescriptor:offset:,
// heapOffset, explicit-offset aliasing after sync, alignment/bounds refusal, accounting & teardown).
// Build on macOS (verify both -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-placement-heap.m -o "$TMPDIR/test-metal3-placement-heap"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-placement-heap" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-placement-heap" --reference
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
    fprintf(stderr, "METAL3_PLACEMENT_HEAP_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment > 0 && !(alignment & (alignment - 1)), @"non-power-of-two alignment");
    require(value <= NSUIntegerMax - (alignment - 1), @"alignUp overflow");
    return (value + alignment - 1) & ~(alignment - 1);
}

static void finish(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
}

static void fillBufferGPU(id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                          id<MTLBuffer> buf, uint32_t value, NSUInteger count, NSString *tag) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: encoder nil", tag]);
    [enc setComputePipelineState:pso];
    [enc setBuffer:buf offset:0 atIndex:0];
    [enc setBytes:&value length:sizeof value atIndex:1];
    [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finish(cb, tag);
}

static void readBufferGPU(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLBuffer> src,
                          void *dst, NSUInteger bytes, NSString *tag) {
    id<MTLBuffer> stg = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(stg != nil, [NSString stringWithFormat:@"%@: staging buffer nil", tag]);
    memset(stg.contents, 0xA5, bytes);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    require(blit != nil, [NSString stringWithFormat:@"%@: blit encoder nil", tag]);
    [blit copyFromBuffer:src sourceOffset:0 toBuffer:stg destinationOffset:0 size:bytes];
    [blit endEncoding];
    finish(cb, tag);
    memcpy(dst, stg.contents, bytes);
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
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void fill_u32(device uint *dst [[buffer(0)]],\n"
             "                     constant uint &val [[buffer(1)]],\n"
             "                     uint i [[thread_position_in_grid]]) {\n"
             "    dst[i] = val ^ i;\n"
             "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"library compile: %@", err]);
        id<MTLComputePipelineState> fillPso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_u32"] error:&err];
        require(fillPso != nil, [NSString stringWithFormat:@"fillPso: %@", err]);

        // 1. Private placement heap: disjoint buffers + texture + view + explicit-offset aliasing.
        NSUInteger baseAlloc = dev.currentAllocatedSize;
        @autoreleasepool {
            enum { kPrivCount = 256 };
            const NSUInteger count = kPrivCount;
            const NSUInteger bufBytes = count * sizeof(uint32_t);
            const MTLResourceOptions privOpts = MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
            MTLSizeAndAlign saBuf = [dev heapBufferSizeAndAlignWithLength:bufBytes options:privOpts];
            require(saBuf.size >= bufBytes && saBuf.align > 0, @"invalid buffer SizeAndAlign");

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:32 height:32 mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            MTLSizeAndAlign saTex = [dev heapTextureSizeAndAlignWithDescriptor:td];
            require(saTex.size > 0 && saTex.align > 0, @"invalid texture SizeAndAlign");

            NSUInteger off0 = 0;
            NSUInteger off1 = alignUp(off0 + saBuf.size, saBuf.align);
            NSUInteger off2 = alignUp(off1 + saBuf.size, saTex.align);
            NSUInteger needSize = alignUp(off2 + saTex.size, 65536);

            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.type = MTLHeapTypePlacement;
            hd.storageMode = MTLStorageModePrivate;
            hd.cpuCacheMode = MTLCPUCacheModeDefaultCache;
            hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
            hd.size = needSize;
            id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
            require(heap != nil, @"Private placement heap creation failed");
            require(heap.type == MTLHeapTypePlacement, @"heap.type != MTLHeapTypePlacement");
            require(heap.size >= needSize && heap.currentAllocatedSize == heap.size, @"heap size accounting");
            if (!reference) {
                require(heap.usedSize == 0, @"empty placement heap usedSize != 0");
                require(dev.currentAllocatedSize == baseAlloc + heap.size, @"device currentAllocatedSize mismatch after heap create");
            }

            id<MTLBuffer> b0 = [heap newBufferWithLength:bufBytes options:privOpts offset:off0];
            id<MTLBuffer> b1 = [heap newBufferWithLength:bufBytes options:privOpts offset:off1];
            id<MTLTexture> tex = [heap newTextureWithDescriptor:td offset:off2];
            require(b0 != nil && b1 != nil && tex != nil, @"disjoint placement allocations failed");
            require(b0.heap == heap && b1.heap == heap && tex.heap == heap, @"resource.heap mismatch");
            require(b0.heapOffset == off0 && b1.heapOffset == off1 && tex.heapOffset == off2, @"resource.heapOffset mismatch");
            if (!reference) {
                require(![b0 doesAliasResource:b1] && ![b0 doesAliasResource:tex] && ![b1 doesAliasResource:tex],
                        @"disjoint placement resources reported aliasing");
                require(dev.currentAllocatedSize == baseAlloc + heap.size, @"placement resources double-counted device allocation");
            }

            id<MTLTexture> tView = [tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
            require(tView != nil && tView.heap == heap && tView.heapOffset == off2, @"texture view lost placement heapOffset");

            // Populate b0, b1 and clear tex; verify disjoint data integrity.
            fillBufferGPU(q, fillPso, b0, 0x11220000u, count, @"fill_b0");
            fillBufferGPU(q, fillPso, b1, 0x33440000u, count, @"fill_b1");
            {
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = tex;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.5, 1.0);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:rpd];
                require(renc != nil, @"tex clear encoder nil");
                [renc endEncoding];
                finish(cb, @"clear_placed_tex");
            }

            uint32_t out0[kPrivCount], out1[kPrivCount];
            readBufferGPU(dev, q, b0, out0, bufBytes, @"read_b0");
            readBufferGPU(dev, q, b1, out1, bufBytes, @"read_b1");
            for (NSUInteger i = 0; i < count; i++) {
                require(out0[i] == (0x11220000u ^ (uint32_t)i), @"b0 data corrupted");
                require(out1[i] == (0x33440000u ^ (uint32_t)i), @"b1 data corrupted");
            }

            // Explicit aliasing at off0 after completed GPU sync.
            id<MTLBuffer> bAlias = [heap newBufferWithLength:bufBytes options:privOpts offset:off0];
            require(bAlias != nil && bAlias.heapOffset == off0, @"explicit alias placement failed");
            if (!reference) require([b0 doesAliasResource:bAlias] && [bAlias doesAliasResource:b0], @"alias predicate false");
            fillBufferGPU(q, fillPso, bAlias, 0xCAFE0000u, count, @"fill_bAlias");
            uint32_t outAlias[kPrivCount];
            readBufferGPU(dev, q, bAlias, outAlias, bufBytes, @"read_bAlias");
            readBufferGPU(dev, q, b1, out1, bufBytes, @"read_b1_after_alias");
            for (NSUInteger i = 0; i < count; i++) {
                require(outAlias[i] == (0xCAFE0000u ^ (uint32_t)i), @"aliased write not visible");
                require(out1[i] == (0x33440000u ^ (uint32_t)i), @"disjoint b1 corrupted by alias at off0");
            }

            // Verify placed texture pixels survived aliased buffer reuse at off0.
            id<MTLBuffer> texRead = [dev newBufferWithLength:32 * 32 * 4 options:MTLResourceStorageModeShared];
            require(texRead != nil, @"texRead buffer nil");
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(32, 32, 1) toBuffer:texRead destinationOffset:0
               destinationBytesPerRow:32 * 4 destinationBytesPerImage:32 * 32 * 4];
                [blit endEncoding];
                finish(cb, @"read_placed_tex");
            }
            const uint8_t *px = texRead.contents;
            for (NSUInteger i = 0; i < 32 * 32; i++) {
                require(px[i * 4 + 0] == 255 && px[i * 4 + 1] == 0 &&
                        (px[i * 4 + 2] == 127 || px[i * 4 + 2] == 128) && px[i * 4 + 3] == 255,
                        @"placed texture pixel corrupted");
            }

            // Negative checks (non-offset on placement heap, misaligned, OOB, overflow, wrong options).
            if (!reference) {
                NSUInteger usedBeforeNeg = heap.usedSize;
                require([heap newBufferWithLength:bufBytes options:privOpts] == nil,
                        @"non-offset buffer accepted on placement heap");
                require([heap newTextureWithDescriptor:td] == nil,
                        @"non-offset texture accepted on placement heap");
                require([heap newBufferWithLength:0 options:privOpts offset:0] == nil,
                        @"zero-length placement buffer accepted");
                if (saBuf.align > 1) {
                    require([heap newBufferWithLength:bufBytes options:privOpts offset:1] == nil,
                            @"misaligned placement buffer accepted");
                }
                if (saTex.align > 1) {
                    require([heap newTextureWithDescriptor:td offset:1] == nil,
                            @"misaligned placement texture accepted");
                }
                require([heap newBufferWithLength:bufBytes options:privOpts offset:heap.size] == nil,
                        @"OOB placement buffer at heap.size accepted");
                require([heap newBufferWithLength:bufBytes options:privOpts offset:(NSUIntegerMax & ~(saBuf.align - 1))] == nil,
                        @"overflow offset placement buffer accepted");
                require([heap newBufferWithLength:bufBytes options:(MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked) offset:0] == nil,
                        @"storage-mode mismatch accepted on placement heap");
                require(heap.usedSize == usedBeforeNeg, @"failed placements mutated heap.usedSize");

                // Automatic heap still refuses offset placements.
                MTLHeapDescriptor *ahd = [MTLHeapDescriptor new];
                ahd.type = MTLHeapTypeAutomatic;
                ahd.storageMode = MTLStorageModePrivate;
                ahd.size = 65536;
                id<MTLHeap> autoHeap = [dev newHeapWithDescriptor:ahd];
                require(autoHeap != nil && autoHeap.type == MTLHeapTypeAutomatic, @"automatic heap control failed");
                require([autoHeap newBufferWithLength:256 options:privOpts offset:0] == nil,
                        @"automatic heap accepted placement buffer offset");
                require([autoHeap newTextureWithDescriptor:td offset:0] == nil,
                        @"automatic heap accepted placement texture offset");

                // makeAliasable retires accounting cleanly.
                [b0 makeAliasable];
                [bAlias makeAliasable];
                [b1 makeAliasable];
                [tex makeAliasable];
                require(b0.isAliasable && bAlias.isAliasable && b1.isAliasable && tex.isAliasable, @"isAliasable false");
                require(heap.usedSize == 0, @"usedSize != 0 after all resources made aliasable");
                require([heap maxAvailableSizeWithAlignment:saBuf.align] >= needSize, @"maxAvailableSize did not recover");
            }
            puts("METAL3_PLACEMENT_HEAP_CASE_PASS private_disjoint_alias_and_refusals");
        }
        if (!reference) {
            for (int tries = 0; tries < 50 && dev.currentAllocatedSize != baseAlloc; tries++) usleep(10000);
            require(dev.currentAllocatedSize == baseAlloc, @"device currentAllocatedSize leaked after Private placement heap teardown");
        }

        // 2. Shared placement heap: CPU-visible explicit offset aliasing + GPU readback.
        @autoreleasepool {
            const NSUInteger count = 128;
            const NSUInteger bufBytes = count * sizeof(uint32_t);
            const MTLResourceOptions sharedOpts = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked;
            MTLSizeAndAlign sa = [dev heapBufferSizeAndAlignWithLength:bufBytes options:sharedOpts];
            require(sa.size >= bufBytes && sa.align > 0, @"invalid Shared buffer SizeAndAlign");
            NSUInteger off1 = alignUp(sa.size, sa.align);
            NSUInteger heapSz = alignUp(off1 + sa.size, 65536);

            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.type = MTLHeapTypePlacement;
            hd.storageMode = MTLStorageModeShared;
            hd.cpuCacheMode = MTLCPUCacheModeDefaultCache;
            hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
            hd.size = heapSz;
            id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
            require(heap != nil, @"Shared placement heap creation failed");

            id<MTLBuffer> s0 = [heap newBufferWithLength:bufBytes options:sharedOpts offset:0];
            id<MTLBuffer> sAlias = [heap newBufferWithLength:bufBytes options:sharedOpts offset:0];
            id<MTLBuffer> s1 = [heap newBufferWithLength:bufBytes options:sharedOpts offset:off1];
            require(s0 != nil && sAlias != nil && s1 != nil, @"Shared placement buffers failed");
            require(s0.contents != NULL && sAlias.contents == s0.contents && s1.contents != s0.contents,
                    @"Shared placement CPU mapping aliasing mismatch");
            require(s0.heapOffset == 0 && sAlias.heapOffset == 0 && s1.heapOffset == off1, @"Shared heapOffset mismatch");

            uint32_t *p0 = (uint32_t *)s0.contents;
            uint32_t *pAlias = (uint32_t *)sAlias.contents;
            uint32_t *p1 = (uint32_t *)s1.contents;
            for (NSUInteger i = 0; i < count; i++) {
                p0[i] = 0x55AA0000u ^ (uint32_t)i;
                p1[i] = 0x77BB0000u ^ (uint32_t)i;
            }
            for (NSUInteger i = 0; i < count; i++) {
                require(pAlias[i] == (0x55AA0000u ^ (uint32_t)i), @"Shared alias did not observe CPU write");
                require(p1[i] == (0x77BB0000u ^ (uint32_t)i), @"Shared disjoint buffer corrupted");
            }
            fillBufferGPU(q, fillPso, sAlias, 0x99CC0000u, count, @"fill_shared_alias");
            for (NSUInteger i = 0; i < count; i++) {
                require(p0[i] == (0x99CC0000u ^ (uint32_t)i), @"s0 did not observe GPU write through sAlias");
                require(p1[i] == (0x77BB0000u ^ (uint32_t)i), @"s1 corrupted by GPU write to sAlias");
            }
            puts("METAL3_PLACEMENT_HEAP_CASE_PASS shared_alias_and_gpu_coherency");
        }
        if (!reference) {
            for (int tries = 0; tries < 50 && dev.currentAllocatedSize != baseAlloc; tries++) usleep(10000);
            require(dev.currentAllocatedSize == baseAlloc, @"device currentAllocatedSize leaked after Shared placement heap teardown");
        }

        printf("METAL3_PLACEMENT_HEAP_PASS reference=%d\n", reference ? 1 : 0);
        return 0;
    }
}
