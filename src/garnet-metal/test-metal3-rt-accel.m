// test-metal3-rt-accel.m: live macOS / RX 9060 XT verification for Task 7.1
// (RDNA4AccelerationStructure & MTLAccelerationStructureCommandEncoder:
//  size queries, triangle/AABB BLAS build, Default & UserID TLAS build,
//  compacted size readback UInt32/UInt64 + copyAndCompact, in-place/out-of-place
//  refit, clone copy, early ARC release safety, and controlled negative rejections).
//
// Build on macOS (verify BOTH -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-rt-accel.m -o "$TMPDIR/test-metal3-rt-accel"
// Run:
//   sudo RDNA4M_ALLOW=1 "$TMPDIR/test-metal3-rt-accel" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-rt-accel" --reference
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
    fprintf(stderr, "METAL3_RT_ACCEL_FAIL %s\n", msg.UTF8String);
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

static void finishExpectError(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusError || cb.error != nil,
            [NSString stringWithFormat:@"%@: expected error status, got status=%lu err=%@",
             tag, (unsigned long)cb.status, cb.error]);
}

static MTLPackedFloat4x3 makeTransform(float tx, float ty, float tz, float sx) {
    MTLPackedFloat4x3 m;
    m.columns[0] = MTLPackedFloat3Make(sx, 0.0f, 0.0f);
    m.columns[1] = MTLPackedFloat3Make(0.0f, 1.0f, 0.0f);
    m.columns[2] = MTLPackedFloat3Make(0.0f, 0.0f, 1.0f);
    m.columns[3] = MTLPackedFloat3Make(tx, ty, tz);
    return m;
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

        // 1. Triangle BLAS (indexed + refittable) size query (including nil-buffer query).
        MTLAccelerationStructureTriangleGeometryDescriptor *triGeom =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        triGeom.triangleCount = 2;
        triGeom.vertexStride = sizeof(float) * 3;
        triGeom.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *triDesc =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        triDesc.geometryDescriptors = @[ triGeom ];
        triDesc.usage = MTLAccelerationStructureUsageRefit;

        MTLAccelerationStructureSizes triSizes = [dev accelerationStructureSizesWithDescriptor:triDesc];
        require(triSizes.accelerationStructureSize > 0 && triSizes.buildScratchBufferSize > 0 &&
                triSizes.refitScratchBufferSize > 0,
                [NSString stringWithFormat:@"invalid refittable tri sizes: as=%lu build=%lu refit=%lu",
                 (unsigned long)triSizes.accelerationStructureSize,
                 (unsigned long)triSizes.buildScratchBufferSize,
                 (unsigned long)triSizes.refitScratchBufferSize]);

        // Without UsageRefit, refitScratchBufferSize must be 0.
        MTLPrimitiveAccelerationStructureDescriptor *staticTriDesc =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        staticTriDesc.geometryDescriptors = @[ triGeom ];
        staticTriDesc.usage = MTLAccelerationStructureUsageNone;
        MTLAccelerationStructureSizes staticTriSizes =
            [dev accelerationStructureSizesWithDescriptor:staticTriDesc];
        require(staticTriSizes.accelerationStructureSize > 0 &&
                staticTriSizes.buildScratchBufferSize > 0 &&
                staticTriSizes.refitScratchBufferSize == 0,
                @"static triangle BLAS must report refitScratchBufferSize == 0");

        // Populate actual triangle vertices + uint16 indices.
        const float verts[4][3] = {
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }
        };
        const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        id<MTLBuffer> vbuf = [dev newBufferWithBytes:verts length:sizeof(verts)
                                             options:MTLResourceStorageModeShared];
        id<MTLBuffer> ibuf = [dev newBufferWithBytes:indices length:sizeof(indices)
                                             options:MTLResourceStorageModeShared];
        require(vbuf && ibuf, @"triangle buffers nil");
        triGeom.vertexBuffer = vbuf;
        triGeom.vertexBufferOffset = 0;
        triGeom.indexBuffer = ibuf;
        triGeom.indexBufferOffset = 0;
        triGeom.indexType = MTLIndexTypeUInt16;

        triSizes = [dev accelerationStructureSizesWithDescriptor:triDesc];
        id<MTLAccelerationStructure> triBlas =
            [dev newAccelerationStructureWithSize:triSizes.accelerationStructureSize];
        require(triBlas != nil && triBlas.size >= triSizes.accelerationStructureSize,
                @"triangle BLAS allocation failed");
        triBlas.label = @"test.tri.blas";
        require([triBlas.label isEqualToString:@"test.tri.blas"], @"BLAS label mismatch");

        // Build triangle BLAS with an unaligned scratch offset (16 bytes) to exercise scratch alignment padding.
        id<MTLBuffer> triScratch =
            [dev newBufferWithLength:triSizes.buildScratchBufferSize + 64
                             options:MTLResourceStorageModePrivate];
        require(triScratch != nil, @"triangle scratch buffer nil");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            require(enc != nil, @"accelerationStructureCommandEncoder nil");
            [enc buildAccelerationStructure:triBlas
                                 descriptor:triDesc
                              scratchBuffer:triScratch
                        scratchBufferOffset:16];
            [enc endEncoding];
            finishOK(cb, @"build_tri_blas");
        }

        // 2. Bounding-box (AABB) BLAS build via newAccelerationStructureWithDescriptor:.
        const MTLAxisAlignedBoundingBox boxes[2] = {
            { MTLPackedFloat3Make(-1.0f, -1.0f, -1.0f), MTLPackedFloat3Make(0.0f, 0.0f, 0.0f) },
            { MTLPackedFloat3Make( 0.5f,  0.5f,  0.5f), MTLPackedFloat3Make(2.0f, 2.0f, 2.0f) }
        };
        id<MTLBuffer> aabbBuf = [dev newBufferWithBytes:boxes length:sizeof(boxes)
                                                options:MTLResourceStorageModeShared];
        require(aabbBuf != nil, @"AABB buffer nil");
        MTLAccelerationStructureBoundingBoxGeometryDescriptor *aabbGeom =
            [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
        aabbGeom.boundingBoxBuffer = aabbBuf;
        aabbGeom.boundingBoxBufferOffset = 0;
        aabbGeom.boundingBoxStride = sizeof(MTLAxisAlignedBoundingBox);
        aabbGeom.boundingBoxCount = 2;
        aabbGeom.opaque = NO;

        MTLPrimitiveAccelerationStructureDescriptor *aabbDesc =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        aabbDesc.geometryDescriptors = @[ aabbGeom ];
        aabbDesc.usage = MTLAccelerationStructureUsageNone;
        MTLAccelerationStructureSizes aabbSizes =
            [dev accelerationStructureSizesWithDescriptor:aabbDesc];
        require(aabbSizes.accelerationStructureSize > 0 && aabbSizes.buildScratchBufferSize > 0,
                @"AABB BLAS sizes invalid");
        id<MTLAccelerationStructure> aabbBlas =
            [dev newAccelerationStructureWithDescriptor:aabbDesc];
        require(aabbBlas != nil, @"newAccelerationStructureWithDescriptor: failed for AABB BLAS");
        id<MTLBuffer> aabbScratch =
            [dev newBufferWithLength:aabbSizes.buildScratchBufferSize
                             options:MTLResourceStorageModePrivate];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:aabbBlas
                                 descriptor:aabbDesc
                              scratchBuffer:aabbScratch
                        scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"build_aabb_blas");
        }

        // 3. Compacted size readback (UInt32 + UInt64 with guard words) + copyAndCompact.
        id<MTLBuffer> sizeBuf32 = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLBuffer> sizeBuf64 = [dev newBufferWithLength:24 options:MTLResourceStorageModeShared];
        require(sizeBuf32 && sizeBuf64, @"compacted size buffers nil");
        uint32_t *p32 = (uint32_t *)sizeBuf32.contents;
        p32[0] = 0xDEADBEEFu; p32[1] = 0u; p32[2] = 0xCAFEBABEu; p32[3] = 0x11223344u;
        uint64_t *p64 = (uint64_t *)sizeBuf64.contents;
        p64[0] = UINT64_C(0xDEADBEEFCAFEBABE); p64[1] = 0u; p64[2] = UINT64_C(0x1122334455667788);
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc writeCompactedAccelerationStructureSize:triBlas toBuffer:sizeBuf32 offset:4];
            [enc writeCompactedAccelerationStructureSize:triBlas toBuffer:sizeBuf64 offset:8
                                            sizeDataType:MTLDataTypeULong];
            [enc endEncoding];
            finishOK(cb, @"write_compacted_size");
        }
        require(p32[0] == 0xDEADBEEFu && p32[2] == 0xCAFEBABEu && p32[3] == 0x11223344u,
                @"sizeBuf32 guard corrupted");
        require(p64[0] == UINT64_C(0xDEADBEEFCAFEBABE) && p64[2] == UINT64_C(0x1122334455667788),
                @"sizeBuf64 guard corrupted");
        require(p32[1] > 0u && p64[1] == (uint64_t)p32[1] &&
                p64[1] <= triSizes.accelerationStructureSize,
                [NSString stringWithFormat:@"compacted size mismatch: u32=%u u64=%llu orig=%lu",
                 p32[1], (unsigned long long)p64[1], (unsigned long)triSizes.accelerationStructureSize]);

        id<MTLAccelerationStructure> compactTriBlas =
            [dev newAccelerationStructureWithSize:(NSUInteger)p64[1]];
        require(compactTriBlas != nil, @"compact BLAS allocation failed");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAndCompactAccelerationStructure:triBlas toAccelerationStructure:compactTriBlas];
            [enc endEncoding];
            finishOK(cb, @"copy_and_compact");
        }

        // 4. Clone copy (built BLAS and compacted BLAS).
        id<MTLAccelerationStructure> clonedBlas =
            [dev newAccelerationStructureWithSize:triSizes.accelerationStructureSize];
        id<MTLAccelerationStructure> clonedCompact =
            [dev newAccelerationStructureWithSize:(NSUInteger)p64[1]];
        require(clonedBlas && clonedCompact, @"clone BLAS allocation failed");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAccelerationStructure:triBlas toAccelerationStructure:clonedBlas];
            [enc copyAccelerationStructure:compactTriBlas toAccelerationStructure:clonedCompact];
            [enc endEncoding];
            finishOK(cb, @"copy_blas");
        }

        // 5. Refit (in-place and out-of-place) on refittable BLAS.
        float *vptr = (float *)vbuf.contents;
        vptr[2] = 0.25f;
        vptr[5] = 0.50f;
        id<MTLBuffer> refitScratch =
            [dev newBufferWithLength:triSizes.refitScratchBufferSize
                             options:MTLResourceStorageModePrivate];
        id<MTLAccelerationStructure> refitDst =
            [dev newAccelerationStructureWithSize:triSizes.accelerationStructureSize];
        require(refitScratch && refitDst, @"refit resources nil");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc refitAccelerationStructure:triBlas
                                 descriptor:triDesc
                                destination:nil
                              scratchBuffer:refitScratch
                        scratchBufferOffset:0];
            [enc refitAccelerationStructure:triBlas
                                 descriptor:triDesc
                                destination:refitDst
                              scratchBuffer:refitScratch
                        scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"refit_blas");
        }

        // 6. TLAS build (Default + UserID instance descriptors) + TLAS refit.
        MTLAccelerationStructureUserIDInstanceDescriptor insts[2];
        memset(insts, 0, sizeof(insts));
        insts[0].transformationMatrix = makeTransform(1.0f, 2.0f, 3.0f, 1.0f);
        insts[0].options = MTLAccelerationStructureInstanceOptionOpaque;
        insts[0].mask = 0xFFu;
        insts[0].intersectionFunctionTableOffset = 0u;
        insts[0].accelerationStructureIndex = 0u;
        insts[0].userID = 101u;

        insts[1].transformationMatrix = makeTransform(-2.0f, 0.5f, 4.0f, 2.0f);
        insts[1].options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling |
                           MTLAccelerationStructureInstanceOptionNonOpaque;
        insts[1].mask = 0x0Fu;
        insts[1].intersectionFunctionTableOffset = 1u;
        insts[1].accelerationStructureIndex = 1u;
        insts[1].userID = 202u;

        id<MTLBuffer> instBuf = [dev newBufferWithBytes:insts length:sizeof(insts)
                                                options:MTLResourceStorageModeShared];
        require(instBuf != nil, @"instance buffer nil");
        MTLInstanceAccelerationStructureDescriptor *tlasDesc =
            [MTLInstanceAccelerationStructureDescriptor descriptor];
        tlasDesc.instancedAccelerationStructures = @[ compactTriBlas, aabbBlas ];
        tlasDesc.instanceCount = 2;
        tlasDesc.instanceDescriptorBuffer = instBuf;
        tlasDesc.instanceDescriptorBufferOffset = 0;
        tlasDesc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
        tlasDesc.usage = MTLAccelerationStructureUsageRefit;

        MTLAccelerationStructureSizes tlasSizes =
            [dev accelerationStructureSizesWithDescriptor:tlasDesc];
        require(tlasSizes.accelerationStructureSize > 0 && tlasSizes.buildScratchBufferSize > 0,
                @"TLAS sizes invalid");
        id<MTLAccelerationStructure> tlas =
            [dev newAccelerationStructureWithSize:tlasSizes.accelerationStructureSize];
        id<MTLBuffer> tlasScratch =
            [dev newBufferWithLength:MAX(tlasSizes.buildScratchBufferSize, tlasSizes.refitScratchBufferSize)
                             options:MTLResourceStorageModePrivate];
        require(tlas && tlasScratch, @"TLAS allocation failed");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:tlas
                                 descriptor:tlasDesc
                              scratchBuffer:tlasScratch
                        scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"build_tlas");
        }
        // Refit TLAS with updated translation.
        MTLAccelerationStructureUserIDInstanceDescriptor *iptr =
            (MTLAccelerationStructureUserIDInstanceDescriptor *)instBuf.contents;
        iptr[0].transformationMatrix = makeTransform(5.0f, 6.0f, 7.0f, 1.0f);
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc refitAccelerationStructure:tlas
                                 descriptor:tlasDesc
                                destination:tlas
                              scratchBuffer:tlasScratch
                        scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"refit_tlas");
        }

        // 7. Early ARC release safety: release inputs/scratch/temp AS before waiting on GPU.
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            @autoreleasepool {
                id<MTLBuffer> tmpVerts = [dev newBufferWithBytes:verts length:sizeof(verts)
                                                         options:MTLResourceStorageModeShared];
                MTLAccelerationStructureTriangleGeometryDescriptor *tmpGeom =
                    [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                tmpGeom.vertexBuffer = tmpVerts;
                tmpGeom.vertexStride = sizeof(float) * 3;
                tmpGeom.triangleCount = 1;
                MTLPrimitiveAccelerationStructureDescriptor *tmpDesc =
                    [MTLPrimitiveAccelerationStructureDescriptor descriptor];
                tmpDesc.geometryDescriptors = @[ tmpGeom ];
                MTLAccelerationStructureSizes tmpSz =
                    [dev accelerationStructureSizesWithDescriptor:tmpDesc];
                id<MTLAccelerationStructure> tmpAS =
                    [dev newAccelerationStructureWithSize:tmpSz.accelerationStructureSize];
                id<MTLBuffer> tmpScratch =
                    [dev newBufferWithLength:tmpSz.buildScratchBufferSize
                                     options:MTLResourceStorageModePrivate];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc buildAccelerationStructure:tmpAS
                                     descriptor:tmpDesc
                                  scratchBuffer:tmpScratch
                            scratchBufferOffset:0];
                [enc endEncoding];
            }
            finishOK(cb, @"early_arc_release");
        }

        // 8. Negative error handling (only enforce Garnet strict checks when not --reference,
        //    since Apple's Metal API validation aborts on invalid args when validation is enabled).
        if (!reference) {
            require([dev newAccelerationStructureWithSize:0] == nil, @"size 0 AS must return nil");

            // Undersized AS destination.
            id<MTLAccelerationStructure> tinyAS = [dev newAccelerationStructureWithSize:16];
            require(tinyAS != nil, @"16B AS alloc for negative test");
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc buildAccelerationStructure:tinyAS
                                     descriptor:triDesc
                                  scratchBuffer:triScratch
                            scratchBufferOffset:0];
                [enc endEncoding];
                finishExpectError(cb, @"neg_undersized_as");
            }

            // Undersized scratch buffer.
            id<MTLBuffer> tinyScratch = [dev newBufferWithLength:16 options:MTLResourceStorageModePrivate];
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc buildAccelerationStructure:triBlas
                                     descriptor:triDesc
                                  scratchBuffer:tinyScratch
                            scratchBufferOffset:0];
                [enc endEncoding];
                finishExpectError(cb, @"neg_undersized_scratch");
            }

            // Refit non-refittable AABB BLAS.
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc refitAccelerationStructure:aabbBlas
                                     descriptor:aabbDesc
                                    destination:aabbBlas
                                  scratchBuffer:aabbScratch
                            scratchBufferOffset:0];
                [enc endEncoding];
                finishExpectError(cb, @"neg_refit_nonrefittable");
            }

            // Refit compacted BLAS.
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc refitAccelerationStructure:compactTriBlas
                                     descriptor:triDesc
                                    destination:compactTriBlas
                                  scratchBuffer:refitScratch
                            scratchBufferOffset:0];
                [enc endEncoding];
                finishExpectError(cb, @"neg_refit_compacted");
            }

            // Self-copy.
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc copyAccelerationStructure:triBlas toAccelerationStructure:triBlas];
                [enc endEncoding];
                finishExpectError(cb, @"neg_self_copy");
            }

            // writeCompactedSize on unbuilt AS.
            id<MTLAccelerationStructure> unbuiltAS =
                [dev newAccelerationStructureWithSize:triSizes.accelerationStructureSize];
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
                [enc writeCompactedAccelerationStructureSize:unbuiltAS toBuffer:sizeBuf32 offset:0];
                [enc endEncoding];
                finishExpectError(cb, @"neg_compact_size_unbuilt");
            }

            // Honest motion-blur gating when supportsPrimitiveMotionBlur == NO.
            if (!dev.supportsPrimitiveMotionBlur) {
                MTLPrimitiveAccelerationStructureDescriptor *mDesc =
                    [MTLPrimitiveAccelerationStructureDescriptor descriptor];
                mDesc.geometryDescriptors = @[ triGeom ];
                mDesc.motionKeyframeCount = 2;
                mDesc.motionStartTime = 0.0f;
                mDesc.motionEndTime = 1.0f;
                MTLAccelerationStructureSizes mSz =
                    [dev accelerationStructureSizesWithDescriptor:mDesc];
                require(mSz.accelerationStructureSize == 0,
                        @"motion descriptor must return 0 sizes when supportsPrimitiveMotionBlur == NO");
            }
        }

        printf("METAL3_RT_ACCEL_PASS tri=%lu aabb=%lu compact=%llu tlas=%lu\n",
               (unsigned long)triSizes.accelerationStructureSize,
               (unsigned long)aabbSizes.accelerationStructureSize,
               (unsigned long long)p64[1],
               (unsigned long)tlasSizes.accelerationStructureSize);
        return 0;
    }
}
