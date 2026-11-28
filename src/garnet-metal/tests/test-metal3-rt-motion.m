// test-metal3-rt-motion.m: live macOS / RX 9060 XT verification for Metal 3 motion blur ray tracing
// (MTLAccelerationStructureMotionTriangleGeometryDescriptor,
//  MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor + IFT,
//  MTLAccelerationStructureMotionInstanceDescriptor + packed float4x3 & MTLComponentTransform SLERP,
//  Clamp & Vanish border modes, world transforms & compact intersector, and copy/compact/refit).
//
// Build on macOS:
//   clang -arch x86_64 -mmacosx-version-min=15.0 -fobjc-arc -O2 -Wall -Wextra -Werror \
//     -Isrc/garnet-metal -framework Foundation -framework Metal \
//     src/garnet-metal/test-metal3-rt-motion.m -lm -o "$TMPDIR/test-metal3-rt-motion"
// Run:
//   GARNET_RT_MOTION=1 RDNA4M_ALLOW=1 GARNET_ALLOW=1 RDNA4M_TEST_INPROC=1 \
//     "$TMPDIR/test-metal3-rt-motion" /path/to/GarnetMetal.bundle
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

#include "garnet_rt_accel.h"
#include "garnet_rt_motion.h"

@interface NSObject (GarnetTestMotionInit)
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
            uint32_t port = matching ? getSvcFn(0u, matching) : 0u;
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
    fprintf(stderr, "METAL3_RT_MOTION_FAIL %s\n", msg.UTF8String);
    fflush(stderr);
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

static MTLPackedFloat4x3 makeTranslate4x3(float tx, float ty, float tz) {
    MTLPackedFloat4x3 m;
    m.columns[0] = MTLPackedFloat3Make(1.0f, 0.0f, 0.0f);
    m.columns[1] = MTLPackedFloat3Make(0.0f, 1.0f, 0.0f);
    m.columns[2] = MTLPackedFloat3Make(0.0f, 0.0f, 1.0f);
    m.columns[3] = MTLPackedFloat3Make(tx, ty, tz);
    return m;
}

typedef struct {
    float ox, oy, oz, tmin;
    float dx, dy, dz, tmax;
    float rayTime;
    uint32_t mask;
    uint32_t pad0, pad1;
} MotionRayInput;

typedef struct {
    uint32_t hitType;      // 0=none, 1=triangle, 2=bounding_box
    uint32_t instanceId;
    uint32_t primitiveId;
    uint32_t frontFacing;
    float distance;
    float baryX, baryY;
    float worldTx;
} MotionRayResult;

static NSString *const kMotionShaders = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace metal::raytracing;\n"
    "\n"
    "struct MotionRayInput {\n"
    "    float4 origin_tmin;\n"
    "    float4 dir_tmax;\n"
    "    float rayTime;\n"
    "    uint mask; uint p0; uint p1;\n"
    "};\n"
    "struct MotionRayResult {\n"
    "    uint hitType;\n"
    "    uint instanceId;\n"
    "    uint primitiveId;\n"
    "    uint frontFacing;\n"
    "    float distance;\n"
    "    float baryX; float baryY;\n"
    "    float worldTx;\n"
    "};\n"
    "\n"
    "kernel void trace_motion_prim(\n"
    "    device const MotionRayInput *rays [[buffer(0)]],\n"
    "    device MotionRayResult *out       [[buffer(1)]],\n"
    "    acceleration_structure<primitive_motion> scene [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, primitive_motion> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].rayTime);\n"
    "    MotionRayResult o = {0, 0, 0, 0, -1.0f, 0.0f, 0.0f, 0.0f};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.frontFacing = res.triangle_front_facing ? 1u : 0u;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "struct BoxHit { bool accept [[accept_intersection]]; float distance [[distance]]; };\n"
    "[[intersection(bounding_box, instancing, primitive_motion)]]\n"
    "BoxHit box_any_unit(float3 origin [[origin]], float3 dir [[direction]], float tmin [[min_distance]], float tmax [[max_distance]]) {\n"
    "    BoxHit h = {false, -1.0f};\n"
    "    float t = -origin.z / dir.z;\n"
    "    if (t >= tmin && t <= tmax) {\n"
    "        h.accept = true;\n"
    "        h.distance = t;\n"
    "    }\n"
    "    return h;\n"
    "}\n"
    "\n"
    "kernel void trace_motion_aabb(\n"
    "    device const MotionRayInput *rays [[buffer(0)]],\n"
    "    device MotionRayResult *out       [[buffer(1)]],\n"
    "    acceleration_structure<instancing, primitive_motion> scene [[buffer(2)]],\n"
    "    intersection_function_table<instancing, primitive_motion> ift [[buffer(3)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<instancing, primitive_motion> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask, rays[tid].rayTime, ift);\n"
    "    MotionRayResult o = {0, 0, 0, 0, -1.0f, 0.0f, 0.0f, 0.0f};\n"
    "    if (res.type == intersection_type::bounding_box) {\n"
    "        o.hitType = 2;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.distance = res.distance;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "kernel void trace_motion_both(\n"
    "    device const MotionRayInput *rays [[buffer(0)]],\n"
    "    device MotionRayResult *out       [[buffer(1)]],\n"
    "    acceleration_structure<instancing, primitive_motion, instance_motion> scene [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, instancing, primitive_motion, instance_motion, world_space_data> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask, rays[tid].rayTime);\n"
    "    MotionRayResult o = {0, 0, 0, 0, -1.0f, 0.0f, 0.0f, 0.0f};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.frontFacing = res.triangle_front_facing ? 1u : 0u;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "        o.worldTx = res.object_to_world_transform[3][0];\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "kernel void trace_motion_compact(\n"
    "    device const MotionRayInput *rays [[buffer(0)]],\n"
    "    device MotionRayResult *out       [[buffer(1)]],\n"
    "    acceleration_structure<instancing, primitive_motion, instance_motion> scene [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, instancing, primitive_motion, instance_motion> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask, rays[tid].rayTime);\n"
    "    MotionRayResult o = {0, 0, 0, 0, -1.0f, 0.0f, 0.0f, 0.0f};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.frontFacing = res.triangle_front_facing ? 1u : 0u;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n";

static id<MTLAccelerationStructure> buildBLAS(id<MTLDevice> dev, id<MTLCommandQueue> q,
                                              MTLPrimitiveAccelerationStructureDescriptor *pd,
                                              NSString *tag) {
    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:pd];
    require(sz.accelerationStructureSize > 0, [NSString stringWithFormat:@"%@: size == 0", tag]);
    id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scr = [dev newBufferWithLength:MAX(sz.buildScratchBufferSize, (NSUInteger)64)
                                         options:MTLResourceStorageModePrivate];
    require(as && scr, [NSString stringWithFormat:@"%@: alloc failed", tag]);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:pd scratchBuffer:scr scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return as;
}

static void dispatchMotionTrace(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                              id<MTLAccelerationStructure> scene, id<MTLIntersectionFunctionTable> ift,
                              const MotionRayInput *rays, MotionRayResult *outRes, uint32_t count,
                              NSString *tag) {
    id<MTLBuffer> rBuf = [dev newBufferWithBytes:rays length:sizeof(MotionRayInput) * count options:MTLResourceStorageModeShared];
    id<MTLBuffer> oBuf = [dev newBufferWithLength:sizeof(MotionRayResult) * count options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setBuffer:rBuf offset:0 atIndex:0];
    [ce setBuffer:oBuf offset:0 atIndex:1];
    [ce setAccelerationStructure:scene atBufferIndex:2];
    if (ift) [ce setIntersectionFunctionTable:ift atBufferIndex:3];
    [ce dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding];
    finishOK(cb, tag);
    memcpy(outRes, [oBuf contents], sizeof(MotionRayResult) * count);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        BOOL checkGatingOnly = NO;
        NSString *bundlePath = nil;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--check-gating") == 0) checkGatingOnly = YES;
            else bundlePath = [NSString stringWithUTF8String:argv[i]];
        }
        require(bundlePath != nil, @"usage: test-metal3-rt-motion [--check-gating] /path/to/GarnetMetal.bundle");

        NSBundle *bundle = [NSBundle bundleWithPath:bundlePath];
        require(bundle != nil && [bundle load], [NSString stringWithFormat:@"failed to load bundle %@", bundlePath]);
        Class devCls = NSClassFromString(@"GarnetDevice");
        require(devCls != nil, @"GarnetDevice missing");
        id<MTLDevice> dev = createGarnetDevice(devCls);
        require(dev != nil && [dev supportsRaytracing], @"supportsRaytracing required on RX 9060 XT");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"newCommandQueue returned nil");

        if (checkGatingOnly) {
            float v0[9] = { -1, -1, 0,  1, -1, 0,  0, 1, 0 };
            float v1[9] = { -1,  0, 0,  1,  0, 0,  0, 2, 0 };
            MTLMotionKeyframeData *k0 = [MTLMotionKeyframeData data];
            k0.buffer = [dev newBufferWithBytes:v0 length:sizeof(v0) options:MTLResourceStorageModeShared];
            MTLMotionKeyframeData *k1 = [MTLMotionKeyframeData data];
            k1.buffer = [dev newBufferWithBytes:v1 length:sizeof(v1) options:MTLResourceStorageModeShared];
            MTLAccelerationStructureMotionTriangleGeometryDescriptor *mg =
                [MTLAccelerationStructureMotionTriangleGeometryDescriptor descriptor];
            mg.vertexBuffers = @[ k0, k1 ];
            mg.vertexStride = 12;
            mg.triangleCount = 1;
            MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            pd.geometryDescriptors = @[ mg ];
            MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:pd];
            require(sz.accelerationStructureSize == 0, @"GARNET_RT_MOTION=0 must reject motion BLAS with size=0");
            printf("METAL3_RT_MOTION_GATING_PASS\n");
            fflush(stdout);
            return 0;
        }

        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kMotionShaders options:opts error:&err];
        require(lib != nil, [NSString stringWithFormat:@"newLibraryWithSource failed: %@", err]);

        id<MTLFunction> fnPrim = [lib newFunctionWithName:@"trace_motion_prim"];
        id<MTLFunction> fnAabb = [lib newFunctionWithName:@"trace_motion_aabb"];
        id<MTLFunction> fnBoxAny = [lib newFunctionWithName:@"box_any_unit"];
        id<MTLFunction> fnBoth = [lib newFunctionWithName:@"trace_motion_both"];
        id<MTLFunction> fnCompact = [lib newFunctionWithName:@"trace_motion_compact"];
        require(fnPrim && fnAabb && fnBoxAny && fnBoth && fnCompact, @"missing Metal 3.1 motion kernel functions");

        id<MTLComputePipelineState> psoPrim = [dev newComputePipelineStateWithFunction:fnPrim error:&err];
        require(psoPrim != nil, [NSString stringWithFormat:@"psoPrim failed: %@", err]);

        MTLComputePipelineDescriptor *cpdAabb = [MTLComputePipelineDescriptor new];
        cpdAabb.computeFunction = fnAabb;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
        lf.functions = @[ fnBoxAny ];
        cpdAabb.linkedFunctions = lf;
        id<MTLComputePipelineState> psoAabb = [dev newComputePipelineStateWithDescriptor:cpdAabb options:0 reflection:nil error:&err];
        require(psoAabb != nil, [NSString stringWithFormat:@"psoAabb failed: %@", err]);

        MTLIntersectionFunctionTableDescriptor *iftDesc = [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
        iftDesc.functionCount = 1;
        id<MTLIntersectionFunctionTable> iftAabb = [psoAabb newIntersectionFunctionTableWithDescriptor:iftDesc];
        [iftAabb setFunction:[psoAabb functionHandleWithFunction:fnBoxAny] atIndex:0];

        id<MTLComputePipelineState> psoBoth = [dev newComputePipelineStateWithFunction:fnBoth error:&err];
        require(psoBoth != nil, [NSString stringWithFormat:@"psoBoth failed: %@", err]);
        id<MTLComputePipelineState> psoCompact = [dev newComputePipelineStateWithFunction:fnCompact error:&err];
        require(psoCompact != nil, [NSString stringWithFormat:@"psoCompact failed: %@", err]);

        // 1. Motion triangle BLAS (2 keyframes shifting Y by +2.0 over [0.0, 1.0]) + Clamp vs Vanish oracle
        const rdna4rt_tri_verts_t triKfs[2] = {
            { .v = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } } },
            { .v = { { -1.0f,  1.0f, 0.0f }, { 1.0f,  1.0f, 0.0f }, { 0.0f, 3.0f, 0.0f } } },
        };
        MTLMotionKeyframeData *tk0 = [MTLMotionKeyframeData data];
        tk0.buffer = [dev newBufferWithBytes:&triKfs[0] length:sizeof(triKfs[0]) options:MTLResourceStorageModeShared];
        MTLMotionKeyframeData *tk1 = [MTLMotionKeyframeData data];
        tk1.buffer = [dev newBufferWithBytes:&triKfs[1] length:sizeof(triKfs[1]) options:MTLResourceStorageModeShared];
        MTLAccelerationStructureMotionTriangleGeometryDescriptor *mgTri =
            [MTLAccelerationStructureMotionTriangleGeometryDescriptor descriptor];
        mgTri.vertexBuffers = @[ tk0, tk1 ];
        mgTri.vertexStride = 12;
        mgTri.triangleCount = 1;
        mgTri.opaque = YES;

        MTLPrimitiveAccelerationStructureDescriptor *pdClamp = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdClamp.geometryDescriptors = @[ mgTri ];
        pdClamp.motionKeyframeCount = 2;
        pdClamp.motionStartTime = 0.0f;
        pdClamp.motionEndTime = 1.0f;
        pdClamp.motionStartBorderMode = MTLMotionBorderModeClamp;
        pdClamp.motionEndBorderMode = MTLMotionBorderModeClamp;
        pdClamp.usage = MTLAccelerationStructureUsageRefit;
        id<MTLAccelerationStructure> blasTriClamp = buildBLAS(dev, q, pdClamp, @"blas-motion-tri-clamp");

        MTLPrimitiveAccelerationStructureDescriptor *pdVanish = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdVanish.geometryDescriptors = @[ mgTri ];
        pdVanish.motionKeyframeCount = 2;
        pdVanish.motionStartTime = 0.0f;
        pdVanish.motionEndTime = 1.0f;
        pdVanish.motionStartBorderMode = MTLMotionBorderModeVanish;
        pdVanish.motionEndBorderMode = MTLMotionBorderModeVanish;
        id<MTLAccelerationStructure> blasTriVanish = buildBLAS(dev, q, pdVanish, @"blas-motion-tri-vanish");

        const float testTimes[5] = { -0.25f, 0.0f, 0.5f, 1.0f, 1.25f };
        MotionRayInput primRays[5];
        MotionRayResult clampOut[5], vanishOut[5];
        for (int i = 0; i < 5; i++) {
            float targetY = (testTimes[i] <= 0.0f) ? 0.0f : ((testTimes[i] >= 1.0f) ? 2.0f : 2.0f * testTimes[i]);
            primRays[i] = (MotionRayInput){ 0.0f, targetY, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, testTimes[i], 0xFFu, 0, 0 };
        }
        dispatchMotionTrace(dev, q, psoPrim, blasTriClamp, nil, primRays, clampOut, 5, @"trace-prim-clamp");
        dispatchMotionTrace(dev, q, psoPrim, blasTriVanish, nil, primRays, vanishOut, 5, @"trace-prim-vanish");

        for (int i = 0; i < 5; i++) {
            float expT = 0, expU = 0, expV = 0;
            int expFf = 0;
            int expHitC = rdna4rt_cpu_intersect_motion_tri(
                &primRays[i].ox, &primRays[i].dx, primRays[i].tmin, primRays[i].tmax, testTimes[i],
                triKfs, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_CLAMP,
                1, 0u, &expT, &expU, &expV, &expFf);
            require((clampOut[i].hitType == 1u) == (expHitC == 1),
                    [NSString stringWithFormat:@"clamp tri[%d] hit mismatch", i]);
            if (expHitC == 1) {
                require(fabsf(clampOut[i].distance - expT) < 1e-4f &&
                        fabsf(clampOut[i].baryX - expU) < 1e-4f &&
                        fabsf(clampOut[i].baryY - expV) < 1e-4f,
                        [NSString stringWithFormat:@"clamp tri[%d] oracle mismatch", i]);
            }
            int expHitV = rdna4rt_cpu_intersect_motion_tri(
                &primRays[i].ox, &primRays[i].dx, primRays[i].tmin, primRays[i].tmax, testTimes[i],
                triKfs, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_VANISH, RDNA4RT_MOTION_BORDER_VANISH,
                1, 0u, &expT, &expU, &expV, &expFf);
            require((vanishOut[i].hitType == 1u) == (expHitV == 1),
                    [NSString stringWithFormat:@"vanish tri[%d] hit mismatch (time=%f)", i, testTimes[i]]);
        }

        // 2. Motion Bounding Box BLAS + IFT
        const float boxKf0[6] = { -1.0f, -1.0f, -0.5f, 1.0f, 1.0f, 0.5f };
        const float boxKf1[6] = {  3.0f, -1.0f, -0.5f, 5.0f, 1.0f, 0.5f };
        MTLMotionKeyframeData *bk0 = [MTLMotionKeyframeData data];
        bk0.buffer = [dev newBufferWithBytes:boxKf0 length:sizeof(boxKf0) options:MTLResourceStorageModeShared];
        MTLMotionKeyframeData *bk1 = [MTLMotionKeyframeData data];
        bk1.buffer = [dev newBufferWithBytes:boxKf1 length:sizeof(boxKf1) options:MTLResourceStorageModeShared];
        MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor *mgBox =
            [MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor descriptor];
        mgBox.boundingBoxBuffers = @[ bk0, bk1 ];
        mgBox.boundingBoxStride = 24;
        mgBox.boundingBoxCount = 1;
        MTLPrimitiveAccelerationStructureDescriptor *pdBox = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdBox.geometryDescriptors = @[ mgBox ];
        pdBox.motionKeyframeCount = 2;
        pdBox.motionStartTime = 0.0f;
        pdBox.motionEndTime = 1.0f;
        id<MTLAccelerationStructure> blasBox = buildBLAS(dev, q, pdBox, @"blas-motion-aabb");

        MTLAccelerationStructureInstanceDescriptor statInst;
        memset(&statInst, 0, sizeof(statInst));
        statInst.transformationMatrix = makeTranslate4x3(0.0f, 0.0f, 0.0f);
        statInst.mask = 0xFFu;
        MTLInstanceAccelerationStructureDescriptor *tlasBoxDesc = [MTLInstanceAccelerationStructureDescriptor descriptor];
        tlasBoxDesc.instancedAccelerationStructures = @[ blasBox ];
        tlasBoxDesc.instanceCount = 1;
        tlasBoxDesc.instanceDescriptorBuffer = [dev newBufferWithBytes:&statInst length:sizeof(statInst) options:MTLResourceStorageModeShared];
        MTLAccelerationStructureSizes boxTlasSz = [dev accelerationStructureSizesWithDescriptor:tlasBoxDesc];
        id<MTLAccelerationStructure> tlasBox = [dev newAccelerationStructureWithSize:boxTlasSz.accelerationStructureSize];
        {
            id<MTLBuffer> scr = [dev newBufferWithLength:MAX(boxTlasSz.buildScratchBufferSize, (NSUInteger)64) options:MTLResourceStorageModePrivate];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:tlasBox descriptor:tlasBoxDesc scratchBuffer:scr scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"tlas-motion-aabb");
        }
        MotionRayInput boxRays[2] = {
            { 2.0f, 0.0f, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, 0.5f, 0xFFu, 0, 0 }, // hits center x=2 at t=0.5
            { 0.0f, 0.0f, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, 0.9f, 0xFFu, 0, 0 }, // misses x=[2.6, 4.6] at t=0.9
        };
        MotionRayResult boxOut[2];
        dispatchMotionTrace(dev, q, psoAabb, tlasBox, iftAabb, boxRays, boxOut, 2, @"trace-motion-aabb");
        require(boxOut[0].hitType == 2u && fabsf(boxOut[0].distance - 5.0f) < 1e-4f && boxOut[1].hitType == 0u,
                [NSString stringWithFormat:@"motion AABB mismatch: %u(%f) %u", boxOut[0].hitType, boxOut[0].distance, boxOut[1].hitType]);

        // 3. Instance Motion TLAS (MTLComponentTransform SLERP + MTLPackedFloat4x3 + combined primitive+instance motion + compact)
        const MTLComponentTransform compKfs[2] = {
            { .scale = {{{1,1,1}}}, .shear = {{{0,0,0}}}, .pivot = {{{0,0,0}}}, .rotation = {0,0,0,1}, .translation = {{{0,0,0}}} },
            { .scale = {{{1,1,1}}}, .shear = {{{0,0,0}}}, .pivot = {{{0,0,0}}}, .rotation = {0,0,0,1}, .translation = {{{4,0,0}}} },
        };
        MTLAccelerationStructureMotionInstanceDescriptor mInst;
        memset(&mInst, 0, sizeof(mInst));
        mInst.mask = 0xFFu;
        mInst.accelerationStructureIndex = 0u;
        mInst.motionTransformsStartIndex = 0u;
        mInst.motionTransformsCount = 2u;
        mInst.motionStartBorderMode = MTLMotionBorderModeClamp;
        mInst.motionEndBorderMode = MTLMotionBorderModeVanish;
        mInst.motionStartTime = 0.0f;
        mInst.motionEndTime = 1.0f;

        MTLInstanceAccelerationStructureDescriptor *mtDesc = [MTLInstanceAccelerationStructureDescriptor descriptor];
        mtDesc.instancedAccelerationStructures = @[ blasTriClamp ];
        mtDesc.instanceCount = 1;
        mtDesc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeMotion;
        mtDesc.instanceDescriptorStride = sizeof(MTLAccelerationStructureMotionInstanceDescriptor);
        mtDesc.instanceDescriptorBuffer = [dev newBufferWithBytes:&mInst length:sizeof(mInst) options:MTLResourceStorageModeShared];
        mtDesc.motionTransformBuffer = [dev newBufferWithBytes:compKfs length:sizeof(compKfs) options:MTLResourceStorageModeShared];
        mtDesc.motionTransformCount = 2;
        mtDesc.motionTransformType = MTLTransformTypeComponent;
        MTLAccelerationStructureSizes mtSz = [dev accelerationStructureSizesWithDescriptor:mtDesc];
        require(mtSz.accelerationStructureSize > 0, @"motion TLAS size == 0");
        id<MTLAccelerationStructure> tlasMotion = [dev newAccelerationStructureWithSize:mtSz.accelerationStructureSize];
        {
            id<MTLBuffer> scr = [dev newBufferWithLength:MAX(mtSz.buildScratchBufferSize, (NSUInteger)64) options:MTLResourceStorageModePrivate];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:tlasMotion descriptor:mtDesc scratchBuffer:scr scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"tlas-motion-inst");
        }

        // At t=0.5: primitive shifts Y=+1.0, instance shifts X=+2.0 -> hit center at (2.0, 1.0, 0). At t=1.25: vanished.
        MotionRayInput bothRays[2] = {
            { 2.0f, 1.0f, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, 0.5f, 0xFFu, 0, 0 },
            { 4.0f, 2.0f, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, 1.25f, 0xFFu, 0, 0 },
        };
        MotionRayResult bothOut[2], compactOut[2];
        dispatchMotionTrace(dev, q, psoBoth,    tlasMotion, nil, bothRays, bothOut,    2, @"trace-motion-both");
        dispatchMotionTrace(dev, q, psoCompact, tlasMotion, nil, bothRays, compactOut, 2, @"trace-motion-compact");

        rdna4rt_component_transform_t cpuComp[2];
        memcpy(cpuComp, compKfs, sizeof(cpuComp));
        float oT = 0, oU = 0, oV = 0; int oFf = 0;
        int oHit = rdna4rt_cpu_intersect_motion_inst_tri(
            &bothRays[0].ox, &bothRays[0].dx, bothRays[0].tmin, bothRays[0].tmax, 0.5f,
            RDNA4RT_XFORM_COMPONENT, cpuComp, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_VANISH,
            triKfs, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_CLAMP,
            1, 0u, &oT, &oU, &oV, &oFf);
        require(oHit == 1 && bothOut[0].hitType == 1u && compactOut[0].hitType == 1u &&
                fabsf(bothOut[0].distance - oT) < 1e-3f && fabsf(compactOut[0].distance - oT) < 1e-3f &&
                fabsf(compactOut[0].baryX - oU) < 1e-3f && fabsf(compactOut[0].baryY - oV) < 1e-3f &&
                fabsf(bothOut[0].worldTx - 2.0f) < 1e-3f &&
                bothOut[1].hitType == 0u && compactOut[1].hitType == 0u,
                [NSString stringWithFormat:@"combined inst+prim/compact mismatch: both=%u(%f,tx=%f) compact=%u(%f,uv=%f,%f) miss=%u/%u",
                                            bothOut[0].hitType, bothOut[0].distance, bothOut[0].worldTx,
                                            compactOut[0].hitType, compactOut[0].distance, compactOut[0].baryX, compactOut[0].baryY,
                                            bothOut[1].hitType, compactOut[1].hitType]);

        // 3b. PackedFloat4x3 Motion Instance TLAS
        const MTLPackedFloat4x3 packedKfs[2] = {
            makeTranslate4x3(0.0f, 0.0f, 0.0f),
            makeTranslate4x3(4.0f, 0.0f, 0.0f),
        };
        MTLInstanceAccelerationStructureDescriptor *mtPackedDesc = [MTLInstanceAccelerationStructureDescriptor descriptor];
        mtPackedDesc.instancedAccelerationStructures = @[ blasTriClamp ];
        mtPackedDesc.instanceCount = 1;
        mtPackedDesc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeMotion;
        mtPackedDesc.instanceDescriptorStride = sizeof(MTLAccelerationStructureMotionInstanceDescriptor);
        mtPackedDesc.instanceDescriptorBuffer = mtDesc.instanceDescriptorBuffer;
        mtPackedDesc.motionTransformBuffer = [dev newBufferWithBytes:packedKfs length:sizeof(packedKfs) options:MTLResourceStorageModeShared];
        mtPackedDesc.motionTransformCount = 2;
        mtPackedDesc.motionTransformType = MTLTransformTypePackedFloat4x3;
        MTLAccelerationStructureSizes mtPackedSz = [dev accelerationStructureSizesWithDescriptor:mtPackedDesc];
        require(mtPackedSz.accelerationStructureSize > 0, @"packed4x3 motion TLAS size == 0");
        id<MTLAccelerationStructure> tlasMotionPacked = [dev newAccelerationStructureWithSize:mtPackedSz.accelerationStructureSize];
        {
            id<MTLBuffer> scr = [dev newBufferWithLength:MAX(mtPackedSz.buildScratchBufferSize, (NSUInteger)64) options:MTLResourceStorageModePrivate];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:tlasMotionPacked descriptor:mtPackedDesc scratchBuffer:scr scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"tlas-motion-packed4x3");
        }
        MotionRayResult packedOut[2];
        dispatchMotionTrace(dev, q, psoBoth, tlasMotionPacked, nil, bothRays, packedOut, 2, @"trace-motion-packed4x3");
        rdna4rt_packed_float4x3_t cpuPacked[2];
        memcpy(cpuPacked, packedKfs, sizeof(cpuPacked));
        float pT = 0, pU = 0, pV = 0; int pFf = 0;
        int pHit = rdna4rt_cpu_intersect_motion_inst_tri(
            &bothRays[0].ox, &bothRays[0].dx, bothRays[0].tmin, bothRays[0].tmax, 0.5f,
            RDNA4RT_XFORM_PACKED_FLOAT4X3, cpuPacked, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_VANISH,
            triKfs, 2, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_CLAMP,
            1, 0u, &pT, &pU, &pV, &pFf);
        require(pHit == 1 && packedOut[0].hitType == 1u &&
                fabsf(packedOut[0].distance - pT) < 1e-3f &&
                fabsf(packedOut[0].worldTx - 2.0f) < 1e-3f &&
                packedOut[1].hitType == 0u,
                [NSString stringWithFormat:@"packed4x3 inst+prim mismatch: out=%u(%f,tx=%f) miss=%u",
                                            packedOut[0].hitType, packedOut[0].distance, packedOut[0].worldTx,
                                            packedOut[1].hitType]);

        // 4. Lifecycle: copy, compact, refit on blasTriClamp
        id<MTLAccelerationStructure> blasCopy = [dev newAccelerationStructureWithSize:blasTriClamp.size];
        id<MTLBuffer> compSzBuf = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAccelerationStructure:blasTriClamp toAccelerationStructure:blasCopy];
            [enc writeCompactedAccelerationStructureSize:blasTriClamp toBuffer:compSzBuf offset:0];
            [enc endEncoding];
            finishOK(cb, @"motion-copy-size");
        }
        uint64_t compactSz = *(const uint64_t *)[compSzBuf contents];
        require(compactSz > 0 && compactSz <= blasTriClamp.size, @"invalid compacted motion BLAS size");
        id<MTLAccelerationStructure> blasComp = [dev newAccelerationStructureWithSize:compactSz];
        id<MTLAccelerationStructure> blasRefit = [dev newAccelerationStructureWithSize:blasTriClamp.size];
        const rdna4rt_tri_verts_t triRefitKfs[2] = {
            { .v = { { -1.0f, 4.0f, 0.0f }, { 1.0f, 4.0f, 0.0f }, { 0.0f, 6.0f, 0.0f } } },
            { .v = { { -1.0f, 6.0f, 0.0f }, { 1.0f, 6.0f, 0.0f }, { 0.0f, 8.0f, 0.0f } } },
        };
        MTLMotionKeyframeData *tkRef0 = [MTLMotionKeyframeData data];
        tkRef0.buffer = [dev newBufferWithBytes:&triRefitKfs[0] length:sizeof(triRefitKfs[0]) options:MTLResourceStorageModeShared];
        MTLMotionKeyframeData *tkRef1 = [MTLMotionKeyframeData data];
        tkRef1.buffer = [dev newBufferWithBytes:&triRefitKfs[1] length:sizeof(triRefitKfs[1]) options:MTLResourceStorageModeShared];
        MTLAccelerationStructureMotionTriangleGeometryDescriptor *mgRefit =
            [MTLAccelerationStructureMotionTriangleGeometryDescriptor descriptor];
        mgRefit.vertexBuffers = @[ tkRef0, tkRef1 ];
        mgRefit.vertexStride = 12;
        mgRefit.triangleCount = 1;
        mgRefit.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *pdRefit = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdRefit.geometryDescriptors = @[ mgRefit ];
        pdRefit.motionKeyframeCount = 2;
        pdRefit.motionStartTime = 0.0f;
        pdRefit.motionEndTime = 1.0f;
        pdRefit.motionStartBorderMode = MTLMotionBorderModeClamp;
        pdRefit.motionEndBorderMode = MTLMotionBorderModeClamp;
        pdRefit.usage = MTLAccelerationStructureUsageRefit;
        MTLAccelerationStructureSizes refSz = [dev accelerationStructureSizesWithDescriptor:pdRefit];
        id<MTLBuffer> refScr = [dev newBufferWithLength:MAX(refSz.refitScratchBufferSize, (NSUInteger)64) options:MTLResourceStorageModePrivate];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAndCompactAccelerationStructure:blasTriClamp toAccelerationStructure:blasComp];
            [enc refitAccelerationStructure:blasTriClamp descriptor:pdRefit destination:blasRefit scratchBuffer:refScr scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"motion-compact-refit");
        }
        MotionRayInput refRay = { 0.0f, 5.0f, -5.0f, 0.01f, 0.0f, 0.0f, 1.0f, 20.0f, 0.0f, 0xFFu, 0, 0 };
        MotionRayResult lcRes[3];
        dispatchMotionTrace(dev, q, psoPrim, blasCopy,  nil, &primRays[0], &lcRes[0], 1, @"trace-motion-copy");
        dispatchMotionTrace(dev, q, psoPrim, blasComp,  nil, &primRays[0], &lcRes[1], 1, @"trace-motion-compact");
        dispatchMotionTrace(dev, q, psoPrim, blasRefit, nil, &refRay,      &lcRes[2], 1, @"trace-motion-refit");
        require(lcRes[0].hitType == 1u && lcRes[1].hitType == 1u && lcRes[2].hitType == 1u &&
                fabsf(lcRes[2].distance - 5.0f) < 1e-3f,
                [NSString stringWithFormat:@"motion lifecycle trace failure: copy=%u(%.4f) compact=%u(%.4f) refit=%u(%.4f)",
                                            lcRes[0].hitType, lcRes[0].distance,
                                            lcRes[1].hitType, lcRes[1].distance,
                                            lcRes[2].hitType, lcRes[2].distance]);

        printf("METAL3_RT_MOTION_PASS prim=ok aabb=ok inst_srt=ok inst_4x3=ok compact=ok lifecycle=ok\n");
        fflush(stdout);
    }
    return 0;
}
