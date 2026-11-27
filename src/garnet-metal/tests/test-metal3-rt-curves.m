// test-metal3-rt-curves.m: live macOS / RX 9060 XT verification for Wave 2.3
// (MTLAccelerationStructureCurveGeometryDescriptor: Linear/Bezier/CatmullRom/BSpline bases,
//  Round & Flat curve types, None/Disk/Sphere end caps, Float3/Half3 control points, Float/Half radii,
//  10,000-ray randomized exact-oracle comparison against garnet_rt_curves.h CPU analytic solver,
//  non-opaque [[intersection(curve, curve_data, instancing)]] IFT filtering, and copy/compact/refit).
//
// Build on macOS (verify BOTH -O0 and -O2):
//   clang -arch x86_64 -mmacosx-version-min=14.0 -fobjc-arc -O2 -Wall -Wextra -Werror \
//     -Isrc/garnet-metal -framework Foundation -framework Metal \
//     src/garnet-metal/test-metal3-rt-curves.m -lm -o "$TMPDIR/test-metal3-rt-curves"
// Run:
//   GARNET_RT_CURVES=1 RDNA4M_ALLOW=1 GARNET_ALLOW=1 RDNA4M_TEST_INPROC=1 \
//     "$TMPDIR/test-metal3-rt-curves" /path/to/GarnetMetal.bundle
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

@interface NSObject (GarnetTestCurvesInit)
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
    fprintf(stderr, "METAL3_RT_CURVES_FAIL %s\n", msg.UTF8String);
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

static uint16_t f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x007FFFFFu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t out = 0u;
    if (exp == 0u) {
        if (mant == 0u) out = sign;
        else {
            int32_t e = -14;
            while ((mant & 0x0400u) == 0u) { mant <<= 1; e--; }
            mant &= 0x03FFu;
            out = sign | ((uint32_t)(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f = 0.0f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

static uint32_t rng_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static float rng_f(uint32_t *s, float lo, float hi) {
    float u = (float)(rng_next(s) >> 8) / 16777216.0f;
    return lo + (hi - lo) * u;
}

static MTLPackedFloat4x3 makeTranslate(float tx, float ty, float tz) {
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
    uint32_t mask;
    uint32_t pad0, pad1, pad2;
} CurveRayInput;

typedef struct {
    uint32_t hitType;      // 0=none, 3=curve
    uint32_t instanceId;
    uint32_t primitiveId;
    float distance;
    float curveParam;
    uint32_t pad0, pad1, pad2;
} CurveRayResult;

static NSString *const kCurveShaders = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace metal::raytracing;\n"
    "\n"
    "struct CurveRayInput {\n"
    "    float4 origin_tmin;\n"
    "    float4 dir_tmax;\n"
    "    uint mask; uint p0; uint p1; uint p2;\n"
    "};\n"
    "struct CurveRayResult {\n"
    "    uint hitType;\n"
    "    uint instanceId;\n"
    "    uint primitiveId;\n"
    "    float distance;\n"
    "    float curveParam;\n"
    "    uint p0; uint p1; uint p2;\n"
    "};\n"
    "\n"
    "kernel void trace_curves(\n"
    "    device const CurveRayInput *rays [[buffer(0)]],\n"
    "    device CurveRayResult *out       [[buffer(1)]],\n"
    "    instance_acceleration_structure scene [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<curve_data, instancing> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask);\n"
    "    CurveRayResult o = {0, 0, 0, -1.0f, -1.0f, 0, 0, 0};\n"
    "    if (res.type == intersection_type::curve) {\n"
    "        o.hitType = 3;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.distance = res.distance;\n"
    "        o.curveParam = res.curve_parameter;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "[[intersection(curve, curve_data, instancing)]]\n"
    "bool curve_accept_upper_half(float u [[curve_parameter]]) {\n"
    "    return u >= 0.45f;\n"
    "}\n"
    "\n"
    "kernel void trace_curves_ift(\n"
    "    device const CurveRayInput *rays [[buffer(0)]],\n"
    "    device CurveRayResult *out       [[buffer(1)]],\n"
    "    instance_acceleration_structure scene [[buffer(2)]],\n"
    "    intersection_function_table<curve_data, instancing> ift [[buffer(3)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<curve_data, instancing> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask, ift);\n"
    "    CurveRayResult o = {0, 0, 0, -1.0f, -1.0f, 0, 0, 0};\n"
    "    if (res.type == intersection_type::curve) {\n"
    "        o.hitType = 3;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.primitiveId = res.primitive_id;\n"
    "        o.distance = res.distance;\n"
    "        o.curveParam = res.curve_parameter;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n";

static id<MTLAccelerationStructure> buildCurveBLAS(
    id<MTLDevice> dev, id<MTLCommandQueue> q,
    MTLAccelerationStructureCurveGeometryDescriptor *cg,
    MTLAccelerationStructureUsage usage,
    NSString *tag)
{
    MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    d.geometryDescriptors = @[ cg ];
    d.usage = usage;

    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:d];
    require(sz.accelerationStructureSize > 0 && sz.buildScratchBufferSize > 0,
            [NSString stringWithFormat:@"%@: invalid curve BLAS sizes", tag]);
    id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
    require(as != nil && scratch != nil, [NSString stringWithFormat:@"%@: curve BLAS alloc failed", tag]);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return as;
}

static id<MTLAccelerationStructure> buildTLAS(
    id<MTLDevice> dev, id<MTLCommandQueue> q,
    NSArray<id<MTLAccelerationStructure>> *blasList,
    const MTLAccelerationStructureInstanceDescriptor *descs,
    NSUInteger count,
    MTLAccelerationStructureUsage usage,
    NSString *tag)
{
    id<MTLBuffer> ib = [dev newBufferWithBytes:descs
                                        length:count * sizeof(*descs)
                                       options:MTLResourceStorageModeShared];
    MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
    td.instancedAccelerationStructures = blasList;
    td.instanceCount = count;
    td.instanceDescriptorBuffer = ib;
    td.instanceDescriptorStride = sizeof(*descs);
    td.usage = usage;

    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:td];
    require(sz.accelerationStructureSize > 0 && sz.buildScratchBufferSize > 0,
            [NSString stringWithFormat:@"%@: invalid TLAS sizes", tag]);
    id<MTLAccelerationStructure> tlas = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
    require(tlas != nil && scratch != nil, [NSString stringWithFormat:@"%@: TLAS alloc failed", tag]);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:tlas descriptor:td scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return tlas;
}

static void dispatchCurveTrace(
    id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
    id<MTLAccelerationStructure> scene, id<MTLIntersectionFunctionTable> ift,
    const CurveRayInput *rays, CurveRayResult *outResults, uint32_t count,
    NSString *tag)
{
    id<MTLBuffer> rbuf = [dev newBufferWithBytes:rays
                                          length:count * sizeof(*rays)
                                         options:MTLResourceStorageModeShared];
    id<MTLBuffer> obuf = [dev newBufferWithLength:count * sizeof(*outResults)
                                          options:MTLResourceStorageModeShared];
    memset([obuf contents], 0xEE, count * sizeof(*outResults));

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setBuffer:rbuf offset:0 atIndex:0];
    [ce setBuffer:obuf offset:0 atIndex:1];
    [ce setAccelerationStructure:scene atBufferIndex:2];
    if (ift) [ce setIntersectionFunctionTable:ift atBufferIndex:3];
    [ce dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce endEncoding];
    finishOK(cb, tag);
    memcpy(outResults, [obuf contents], count * sizeof(*outResults));
}

int main(int argc, char **argv) {
    @autoreleasepool {
        BOOL checkGatingOnly = NO;
        NSString *bundlePath = nil;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--check-gating") == 0) checkGatingOnly = YES;
            else bundlePath = [NSString stringWithUTF8String:argv[i]];
        }
        require(bundlePath != nil, @"usage: test-metal3-rt-curves [--check-gating] /path/to/GarnetMetal.bundle");

        NSBundle *bundle = [NSBundle bundleWithPath:bundlePath];
        require(bundle != nil && [bundle load], [NSString stringWithFormat:@"failed to load bundle %@", bundlePath]);
        Class devCls = NSClassFromString(@"GarnetDevice");
        require(devCls != nil, @"GarnetDevice missing");
        id<MTLDevice> dev = createGarnetDevice(devCls);
        require(dev != nil, @"createGarnetDevice returned nil");
        require([dev supportsRaytracing], @"supportsRaytracing must be YES on RX 9060 XT");

        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"newCommandQueue returned nil");

        if (checkGatingOnly) {
            float cp[6] = { -1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f };
            float rad[2] = { 0.25f, 0.25f };
            MTLAccelerationStructureCurveGeometryDescriptor *cg =
                [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
            cg.controlPointBuffer = [dev newBufferWithBytes:cp length:sizeof(cp) options:MTLResourceStorageModeShared];
            cg.controlPointCount = 2;
            cg.controlPointStride = 12;
            cg.controlPointFormat = MTLAttributeFormatFloat3;
            cg.radiusBuffer = [dev newBufferWithBytes:rad length:sizeof(rad) options:MTLResourceStorageModeShared];
            cg.radiusStride = 4;
            cg.radiusFormat = MTLAttributeFormatFloat;
            cg.segmentCount = 1;
            cg.segmentControlPointCount = 2;
            cg.curveType = MTLCurveTypeRound;
            cg.curveBasis = MTLCurveBasisLinear;
            cg.curveEndCaps = MTLCurveEndCapsSphere;
            MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            pd.geometryDescriptors = @[ cg ];
            MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:pd];
            require(sz.accelerationStructureSize == 0,
                    @"GARNET_RT_CURVES=0 must reject curve BLAS size query with 0");
            printf("METAL3_RT_CURVES_GATING_PASS\n");
            return 0;
        }

        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kCurveShaders options:opts error:&err];
        require(lib != nil, [NSString stringWithFormat:@"newLibraryWithSource failed: %@", err]);

        id<MTLFunction> fnTrace = [lib newFunctionWithName:@"trace_curves"];
        id<MTLFunction> fnTraceIft = [lib newFunctionWithName:@"trace_curves_ift"];
        id<MTLFunction> fnIftAccept = [lib newFunctionWithName:@"curve_accept_upper_half"];
        require(fnTrace && fnTraceIft && fnIftAccept, @"missing MSL 3.1 curve kernel/intersection functions");

        id<MTLComputePipelineState> psoTrace = [dev newComputePipelineStateWithFunction:fnTrace error:&err];
        require(psoTrace != nil, [NSString stringWithFormat:@"trace_curves PSO failed: %@", err]);

        MTLComputePipelineDescriptor *cpd = [MTLComputePipelineDescriptor new];
        cpd.computeFunction = fnTraceIft;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
        lf.functions = @[ fnIftAccept ];
        cpd.linkedFunctions = lf;
        id<MTLComputePipelineState> psoTraceIft = [dev newComputePipelineStateWithDescriptor:cpd
                                                                                   options:MTLPipelineOptionNone
                                                                                reflection:nil
                                                                                     error:&err];
        require(psoTraceIft != nil, [NSString stringWithFormat:@"trace_curves_ift PSO failed: %@", err]);

        // --- 1. Four curve bases at distinct world positions ---
        // Segment 0 (inst 0, Linear Round Sphere, tx = 0): (-1,0,0,r=0.25) -> (1,0,0,r=0.15)
        const float cpLin[6] = { -1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f };
        const float rLin[2]  = { 0.25f, 0.15f };
        const uint16_t idxLin[2] = { 0, 1 };
        MTLAccelerationStructureCurveGeometryDescriptor *cgLin = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgLin.controlPointBuffer = [dev newBufferWithBytes:cpLin length:sizeof(cpLin) options:MTLResourceStorageModeShared];
        cgLin.controlPointCount = 2; cgLin.controlPointStride = 12; cgLin.controlPointFormat = MTLAttributeFormatFloat3;
        cgLin.radiusBuffer = [dev newBufferWithBytes:rLin length:sizeof(rLin) options:MTLResourceStorageModeShared];
        cgLin.radiusStride = 4; cgLin.radiusFormat = MTLAttributeFormatFloat;
        cgLin.indexBuffer = [dev newBufferWithBytes:idxLin length:sizeof(idxLin) options:MTLResourceStorageModeShared];
        cgLin.indexType = MTLIndexTypeUInt16;
        cgLin.segmentCount = 1; cgLin.segmentControlPointCount = 2;
        cgLin.curveType = MTLCurveTypeRound; cgLin.curveBasis = MTLCurveBasisLinear; cgLin.curveEndCaps = MTLCurveEndCapsSphere;
        cgLin.opaque = YES;
        id<MTLAccelerationStructure> blasLin = buildCurveBLAS(
            dev, q, cgLin, MTLAccelerationStructureUsageRefit, @"blas-linear");

        // Segment 1 (inst 1, Cubic Bezier Round Sphere, tx = 10)
        const float cpBez[12] = {
            -1.0f,  0.0f, 0.0f,
            -0.3f,  0.9f, 0.2f,
             0.3f, -0.9f,-0.2f,
             1.0f,  0.0f, 0.0f
        };
        const float rBez[4] = { 0.20f, 0.28f, 0.22f, 0.16f };
        MTLAccelerationStructureCurveGeometryDescriptor *cgBez = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgBez.controlPointBuffer = [dev newBufferWithBytes:cpBez length:sizeof(cpBez) options:MTLResourceStorageModeShared];
        cgBez.controlPointCount = 4; cgBez.controlPointStride = 12; cgBez.controlPointFormat = MTLAttributeFormatFloat3;
        cgBez.radiusBuffer = [dev newBufferWithBytes:rBez length:sizeof(rBez) options:MTLResourceStorageModeShared];
        cgBez.radiusStride = 4; cgBez.radiusFormat = MTLAttributeFormatFloat;
        cgBez.segmentCount = 1; cgBez.segmentControlPointCount = 4;
        cgBez.curveType = MTLCurveTypeRound; cgBez.curveBasis = MTLCurveBasisBezier; cgBez.curveEndCaps = MTLCurveEndCapsSphere;
        cgBez.opaque = YES;
        id<MTLAccelerationStructure> blasBez = buildCurveBLAS(dev, q, cgBez, MTLAccelerationStructureUsageNone, @"blas-bezier");

        // Segment 2 (inst 2, Catmull-Rom Round None, Half3 + Half + UInt32 index, tx = 20)
        const float cpCatF[12] = {
            -2.0f, -0.4f, 0.0f,
            -1.0f,  0.4f, 0.1f,
             1.0f,  0.4f,-0.1f,
             2.0f, -0.4f, 0.0f
        };
        const float rCatF[4] = { 0.15f, 0.24f, 0.24f, 0.15f };
        uint16_t cpCatH[12];
        uint16_t rCatH[4];
        for (int i = 0; i < 12; i++) cpCatH[i] = f32_to_f16(cpCatF[i]);
        for (int i = 0; i < 4; i++) rCatH[i] = f32_to_f16(rCatF[i]);
        const uint32_t idxCat[4] = { 0, 1, 2, 3 };
        MTLAccelerationStructureCurveGeometryDescriptor *cgCat = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgCat.controlPointBuffer = [dev newBufferWithBytes:cpCatH length:sizeof(cpCatH) options:MTLResourceStorageModeShared];
        cgCat.controlPointCount = 4; cgCat.controlPointStride = 6; cgCat.controlPointFormat = MTLAttributeFormatHalf3;
        cgCat.radiusBuffer = [dev newBufferWithBytes:rCatH length:sizeof(rCatH) options:MTLResourceStorageModeShared];
        cgCat.radiusStride = 2; cgCat.radiusFormat = MTLAttributeFormatHalf;
        cgCat.indexBuffer = [dev newBufferWithBytes:idxCat length:sizeof(idxCat) options:MTLResourceStorageModeShared];
        cgCat.indexType = MTLIndexTypeUInt32;
        cgCat.segmentCount = 1; cgCat.segmentControlPointCount = 4;
        cgCat.curveType = MTLCurveTypeRound; cgCat.curveBasis = MTLCurveBasisCatmullRom; cgCat.curveEndCaps = MTLCurveEndCapsNone;
        cgCat.opaque = YES;
        id<MTLAccelerationStructure> blasCat = buildCurveBLAS(dev, q, cgCat, MTLAccelerationStructureUsageNone, @"blas-catmull");

        // Segment 3 (inst 3, BSpline Round Disk, tx = 30)
        const float cpBsp[12] = {
            -1.4f, -0.5f,  0.1f,
            -0.5f,  0.7f, -0.2f,
             0.5f,  0.7f,  0.2f,
             1.4f, -0.5f, -0.1f
        };
        const float rBsp[4] = { 0.18f, 0.25f, 0.25f, 0.18f };
        MTLAccelerationStructureCurveGeometryDescriptor *cgBsp = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgBsp.controlPointBuffer = [dev newBufferWithBytes:cpBsp length:sizeof(cpBsp) options:MTLResourceStorageModeShared];
        cgBsp.controlPointCount = 4; cgBsp.controlPointStride = 12; cgBsp.controlPointFormat = MTLAttributeFormatFloat3;
        cgBsp.radiusBuffer = [dev newBufferWithBytes:rBsp length:sizeof(rBsp) options:MTLResourceStorageModeShared];
        cgBsp.radiusStride = 4; cgBsp.radiusFormat = MTLAttributeFormatFloat;
        cgBsp.segmentCount = 1; cgBsp.segmentControlPointCount = 4;
        cgBsp.curveType = MTLCurveTypeRound; cgBsp.curveBasis = MTLCurveBasisBSpline; cgBsp.curveEndCaps = MTLCurveEndCapsDisk;
        cgBsp.opaque = YES;
        id<MTLAccelerationStructure> blasBsp = buildCurveBLAS(dev, q, cgBsp, MTLAccelerationStructureUsageNone, @"blas-bspline");

        // Segment 4 (inst 4, Linear Flat None, tx = 40)
        const float cpFlat[6] = { -1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f };
        const float rFlat[2]  = { 0.22f, 0.22f };
        MTLAccelerationStructureCurveGeometryDescriptor *cgFlat = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgFlat.controlPointBuffer = [dev newBufferWithBytes:cpFlat length:sizeof(cpFlat) options:MTLResourceStorageModeShared];
        cgFlat.controlPointCount = 2; cgFlat.controlPointStride = 12; cgFlat.controlPointFormat = MTLAttributeFormatFloat3;
        cgFlat.radiusBuffer = [dev newBufferWithBytes:rFlat length:sizeof(rFlat) options:MTLResourceStorageModeShared];
        cgFlat.radiusStride = 4; cgFlat.radiusFormat = MTLAttributeFormatFloat;
        cgFlat.segmentCount = 1; cgFlat.segmentControlPointCount = 2;
        cgFlat.curveType = MTLCurveTypeFlat; cgFlat.curveBasis = MTLCurveBasisLinear; cgFlat.curveEndCaps = MTLCurveEndCapsNone;
        cgFlat.opaque = YES;
        id<MTLAccelerationStructure> blasFlat = buildCurveBLAS(dev, q, cgFlat, MTLAccelerationStructureUsageNone, @"blas-flat");

        NSArray<id<MTLAccelerationStructure>> *blasList = @[ blasLin, blasBez, blasCat, blasBsp, blasFlat ];
        MTLAccelerationStructureInstanceDescriptor idescs[5];
        memset(idescs, 0, sizeof(idescs));
        for (uint32_t k = 0; k < 5; k++) {
            idescs[k].transformationMatrix = makeTranslate((float)k * 10.0f, 0.0f, 0.0f);
            idescs[k].mask = 0xFFu;
            idescs[k].accelerationStructureIndex = k;
        }
        id<MTLAccelerationStructure> tlas = buildTLAS(dev, q, blasList, idescs, 5, MTLAccelerationStructureUsageNone, @"tlas-curves");

        // Prepare Bezier-converted CPU oracle data per instance
        rdna4rt_vec4_t cpuLin[2] = {
            { cpLin[0], cpLin[1], cpLin[2], rLin[0] },
            { cpLin[3], cpLin[4], cpLin[5], rLin[1] }
        };
        rdna4rt_vec4_t cpuBezRaw[4], cpuBez[4];
        rdna4rt_vec4_t cpuCatRaw[4], cpuCat[4];
        rdna4rt_vec4_t cpuBspRaw[4], cpuBsp[4];
        for (int k = 0; k < 4; k++) {
            cpuBezRaw[k] = (rdna4rt_vec4_t){ cpBez[3*k], cpBez[3*k+1], cpBez[3*k+2], rBez[k] };
            cpuCatRaw[k] = (rdna4rt_vec4_t){ f16_to_f32(cpCatH[3*k]), f16_to_f32(cpCatH[3*k+1]), f16_to_f32(cpCatH[3*k+2]), f16_to_f32(rCatH[k]) };
            cpuBspRaw[k] = (rdna4rt_vec4_t){ cpBsp[3*k], cpBsp[3*k+1], cpBsp[3*k+2], rBsp[k] };
        }
        require(rdna4rt_curve_to_bezier_4(RDNA4RT_CURVE_BASIS_BEZIER, 4u, cpuBezRaw, cpuBez) == RDNA4RT_OK, @"cpu bezier conv");
        require(rdna4rt_curve_to_bezier_4(RDNA4RT_CURVE_BASIS_CATMULL_ROM, 4u, cpuCatRaw, cpuCat) == RDNA4RT_OK, @"cpu catmull conv");
        require(rdna4rt_curve_to_bezier_4(RDNA4RT_CURVE_BASIS_BSPLINE, 4u, cpuBspRaw, cpuBsp) == RDNA4RT_OK, @"cpu bspline conv");
        rdna4rt_vec4_t cpuFlat[2] = {
            { cpFlat[0], cpFlat[1], cpFlat[2], rFlat[0] },
            { cpFlat[3], cpFlat[4], cpFlat[5], rFlat[1] }
        };

        // --- 2. Generate 10,000 randomized rays (2,000 per curve instance) and compare against CPU oracle ---
        enum { NRAYS = 10000 };
        CurveRayInput *rays = (CurveRayInput *)calloc(NRAYS, sizeof(*rays));
        CurveRayResult *gpuRes = (CurveRayResult *)calloc(NRAYS, sizeof(*gpuRes));
        require(rays && gpuRes, @"alloc rays");

        uint32_t seed = 0xC0FFE23u;
        for (uint32_t r = 0; r < NRAYS; r++) {
            uint32_t inst = r % 5u;
            float cx = (float)inst * 10.0f;
            float ox = cx + rng_f(&seed, -1.6f, 1.6f);
            float oy = rng_f(&seed, -1.1f, 1.1f);
            float oz = (r & 1u) ? 4.0f : -4.0f;
            float tx = cx + rng_f(&seed, -1.3f, 1.3f);
            float ty = rng_f(&seed, -0.7f, 0.7f);
            float tz = rng_f(&seed, -0.4f, 0.4f);
            float dx = tx - ox, dy = ty - oy, dz = tz - oz;
            float il = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz);
            rays[r] = (CurveRayInput){
                .ox = ox, .oy = oy, .oz = oz, .tmin = 0.01f,
                .dx = dx * il, .dy = dy * il, .dz = dz * il, .tmax = 20.0f,
                .mask = 0xFFu
            };
        }

        dispatchCurveTrace(dev, q, psoTrace, tlas, nil, rays, gpuRes, NRAYS, @"trace-10k-curves");

        int cpuHits = 0, gpuHits = 0, exactMatches = 0, hitMismatch = 0;
        float maxDistDiff = 0.0f, maxParamDiff = 0.0f;
        int loggedMismatches = 0;
        for (uint32_t r = 0; r < NRAYS; r++) {
            uint32_t inst = r % 5u;
            float cx = (float)inst * 10.0f;
            float lo[3] = { rays[r].ox - cx, rays[r].oy, rays[r].oz };
            float ld[3] = { rays[r].dx, rays[r].dy, rays[r].dz };
            float ct = -1.0f, cu = -1.0f;
            int ch = 0;
            if (inst == 0u) {
                ch = rdna4rt_cpu_intersect_curve_linear(lo, ld, rays[r].tmin, rays[r].tmax,
                                                        &cpuLin[0], &cpuLin[1],
                                                        RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE, &ct, &cu);
            } else if (inst == 1u) {
                ch = rdna4rt_cpu_intersect_curve_cubic(lo, ld, rays[r].tmin, rays[r].tmax,
                                                       cpuBez, RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE, &ct, &cu);
            } else if (inst == 2u) {
                ch = rdna4rt_cpu_intersect_curve_cubic(lo, ld, rays[r].tmin, rays[r].tmax,
                                                       cpuCat, RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_NONE, &ct, &cu);
            } else if (inst == 3u) {
                ch = rdna4rt_cpu_intersect_curve_cubic(lo, ld, rays[r].tmin, rays[r].tmax,
                                                       cpuBsp, RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_DISK, &ct, &cu);
            } else {
                ch = rdna4rt_cpu_intersect_curve_linear(lo, ld, rays[r].tmin, rays[r].tmax,
                                                        &cpuFlat[0], &cpuFlat[1],
                                                        RDNA4RT_CURVE_TYPE_FLAT, RDNA4RT_CURVE_ENDCAPS_NONE, &ct, &cu);
            }
            int gh = (gpuRes[r].hitType == 3u) ? 1 : 0;
            if (ch) cpuHits++;
            if (gh) gpuHits++;
            if (ch != gh) {
                hitMismatch++;
                if (loggedMismatches < 10) {
                    fprintf(stderr, "MISMATCH ray=%u inst=%u cpu=%d(t=%.4f,u=%.4f) gpu=%u(type=%u,t=%.4f,u=%.4f)\n",
                            r, inst, ch, ct, cu, gh, gpuRes[r].hitType, gpuRes[r].distance, gpuRes[r].curveParam);
                    loggedMismatches++;
                }
            } else if (ch && gh) {
                require(gpuRes[r].instanceId == inst && gpuRes[r].primitiveId == 0u,
                        [NSString stringWithFormat:@"ray %u bad inst/prim (%u/%u vs %u/0)",
                                                    r, gpuRes[r].instanceId, gpuRes[r].primitiveId, inst]);
                float dd = fabsf(gpuRes[r].distance - ct);
                float pd = fabsf(gpuRes[r].curveParam - cu);
                if (dd > maxDistDiff) maxDistDiff = dd;
                if (pd > maxParamDiff) maxParamDiff = pd;
                if (dd <= 2e-3f && pd <= 5e-3f) exactMatches++;
                else if (loggedMismatches < 10) {
                    fprintf(stderr, "VALDIFF ray=%u inst=%u cpu(t=%.5f,u=%.5f) gpu(t=%.5f,u=%.5f) dd=%.5f pd=%.5f\n",
                            r, inst, ct, cu, gpuRes[r].distance, gpuRes[r].curveParam, dd, pd);
                    loggedMismatches++;
                }
            } else {
                exactMatches++;
            }
        }
        printf("curve-oracle-10k: cpuHits=%d gpuHits=%d hitMismatch=%d exactMatches=%d/%d maxDistDiff=%.6f maxParamDiff=%.6f\n",
               cpuHits, gpuHits, hitMismatch, exactMatches, NRAYS, maxDistDiff, maxParamDiff);
        fflush(stdout);
        require(cpuHits > 1000 && gpuHits > 1000, @"expected > 1000 curve hits in 10k ray sweep");
        // Allow small silhouette grazing FP epsilon (<= 0.5% of 10k rays = 50)
        require(hitMismatch <= 50,
                [NSString stringWithFormat:@"too many curve hit/miss mismatches: %d", hitMismatch]);
        require(maxDistDiff <= 2e-2f && maxParamDiff <= 2e-2f,
                [NSString stringWithFormat:@"oracle value drift too large (dd=%.5f, pd=%.5f)", maxDistDiff, maxParamDiff]);

        free(rays);
        free(gpuRes);

        // --- 3. Custom IFT callback [[intersection(curve, curve_data, instancing)]] ---
        MTLAccelerationStructureCurveGeometryDescriptor *cgNonOp = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgNonOp.controlPointBuffer = [dev newBufferWithBytes:cpLin length:sizeof(cpLin) options:MTLResourceStorageModeShared];
        cgNonOp.controlPointCount = 2; cgNonOp.controlPointStride = 12; cgNonOp.controlPointFormat = MTLAttributeFormatFloat3;
        cgNonOp.radiusBuffer = [dev newBufferWithBytes:rLin length:sizeof(rLin) options:MTLResourceStorageModeShared];
        cgNonOp.radiusStride = 4; cgNonOp.radiusFormat = MTLAttributeFormatFloat;
        cgNonOp.segmentCount = 1; cgNonOp.segmentControlPointCount = 2;
        cgNonOp.curveType = MTLCurveTypeRound; cgNonOp.curveBasis = MTLCurveBasisLinear; cgNonOp.curveEndCaps = MTLCurveEndCapsSphere;
        cgNonOp.opaque = NO;
        id<MTLAccelerationStructure> blasNonOp = buildCurveBLAS(dev, q, cgNonOp, MTLAccelerationStructureUsageNone, @"blas-nonopaque");
        MTLAccelerationStructureInstanceDescriptor iftInst;
        memset(&iftInst, 0, sizeof(iftInst));
        iftInst.transformationMatrix = makeTranslate(0, 0, 0);
        iftInst.mask = 0xFFu;
        iftInst.accelerationStructureIndex = 0;
        iftInst.intersectionFunctionTableOffset = 0;
        id<MTLAccelerationStructure> tlasIft = buildTLAS(dev, q, @[ blasNonOp ], &iftInst, 1, MTLAccelerationStructureUsageNone, @"tlas-ift");

        MTLIntersectionFunctionTableDescriptor *iftd = [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
        iftd.functionCount = 2;
        id<MTLIntersectionFunctionTable> ift = [psoTraceIft newIntersectionFunctionTableWithDescriptor:iftd];
        require(ift != nil, @"newIntersectionFunctionTableWithDescriptor failed");
        id<MTLFunctionHandle> fhAccept = [psoTraceIft functionHandleWithFunction:fnIftAccept];
        require(fhAccept != nil, @"functionHandleWithFunction for curve IFT failed");
        [ift setFunction:fhAccept atIndex:0];

        CurveRayInput iftRays[2] = {
            // u ~ 0.25 -> rejected by curve_accept_upper_half
            { -0.5f, 0.0f, 3.0f, 0.01f,  0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 },
            // u ~ 0.75 -> accepted
            {  0.5f, 0.0f, 3.0f, 0.01f,  0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 },
        };
        CurveRayResult iftOut[2];
        dispatchCurveTrace(dev, q, psoTraceIft, tlasIft, ift, iftRays, iftOut, 2, @"trace-curve-ift");
        require(iftOut[0].hitType == 0u && iftOut[1].hitType == 3u && iftOut[1].curveParam > 0.65f,
                [NSString stringWithFormat:@"curve IFT filter failed: r0=%u r1=%u(u=%.3f)",
                                            iftOut[0].hitType, iftOut[1].hitType, iftOut[1].curveParam]);

        // --- 4. Copy, copyAndCompact, and refit of curve BLAS ---
        id<MTLAccelerationStructure> blasCopy = [dev newAccelerationStructureWithSize:blasLin.size];
        id<MTLBuffer> compSzBuf = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAccelerationStructure:blasLin toAccelerationStructure:blasCopy];
            [enc writeCompactedAccelerationStructureSize:blasLin toBuffer:compSzBuf offset:0];
            [enc endEncoding];
            finishOK(cb, @"curve-copy-size");
        }
        uint64_t compactSz = *(const uint64_t *)[compSzBuf contents];
        require(compactSz > 0 && compactSz <= blasLin.size, @"invalid compacted curve BLAS size");
        id<MTLAccelerationStructure> blasComp = [dev newAccelerationStructureWithSize:compactSz];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAndCompactAccelerationStructure:blasLin toAccelerationStructure:blasComp];
            [enc endEncoding];
            finishOK(cb, @"curve-compact");
        }
        // Shift Linear curve by +3.0 along Y and refit blasLin -> blasRefit
        const float cpRefit[6] = { -1.0f, 3.0f, 0.0f,   1.0f, 3.0f, 0.0f };
        MTLAccelerationStructureCurveGeometryDescriptor *cgRefit = [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        cgRefit.controlPointBuffer = [dev newBufferWithBytes:cpRefit length:sizeof(cpRefit) options:MTLResourceStorageModeShared];
        cgRefit.controlPointCount = 2; cgRefit.controlPointStride = 12; cgRefit.controlPointFormat = MTLAttributeFormatFloat3;
        cgRefit.radiusBuffer = cgLin.radiusBuffer; cgRefit.radiusStride = 4; cgRefit.radiusFormat = MTLAttributeFormatFloat;
        cgRefit.indexBuffer = cgLin.indexBuffer; cgRefit.indexType = MTLIndexTypeUInt16;
        cgRefit.segmentCount = 1; cgRefit.segmentControlPointCount = 2;
        cgRefit.curveType = MTLCurveTypeRound; cgRefit.curveBasis = MTLCurveBasisLinear; cgRefit.curveEndCaps = MTLCurveEndCapsSphere;
        cgRefit.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *pdRefit = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdRefit.geometryDescriptors = @[ cgRefit ];
        pdRefit.usage = MTLAccelerationStructureUsageRefit;
        MTLAccelerationStructureSizes refSz = [dev accelerationStructureSizesWithDescriptor:pdRefit];
        id<MTLBuffer> refScr = [dev newBufferWithLength:refSz.refitScratchBufferSize options:MTLResourceStorageModePrivate];
        id<MTLAccelerationStructure> blasRefit = [dev newAccelerationStructureWithSize:blasLin.size];
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc refitAccelerationStructure:blasLin descriptor:pdRefit destination:blasRefit scratchBuffer:refScr scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"curve-refit");
        }

        MTLAccelerationStructureInstanceDescriptor lcDescs[3];
        memset(lcDescs, 0, sizeof(lcDescs));
        lcDescs[0].transformationMatrix = makeTranslate(0, 0, 0);   lcDescs[0].mask = 0xFFu; lcDescs[0].accelerationStructureIndex = 0; // copy @ y=0
        lcDescs[1].transformationMatrix = makeTranslate(10, 0, 0);  lcDescs[1].mask = 0xFFu; lcDescs[1].accelerationStructureIndex = 1; // compact @ x=10,y=0
        lcDescs[2].transformationMatrix = makeTranslate(20, 0, 0);  lcDescs[2].mask = 0xFFu; lcDescs[2].accelerationStructureIndex = 2; // refit @ x=20,y=3
        id<MTLAccelerationStructure> tlasLc = buildTLAS(dev, q, @[ blasCopy, blasComp, blasRefit ], lcDescs, 3,
                                                        MTLAccelerationStructureUsageNone, @"tlas-curve-lifecycle");
        CurveRayInput lcRays[4] = {
            {  0.0f, 0.0f, 3.0f, 0.01f, 0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 }, // hits copy (inst 0)
            { 10.0f, 0.0f, 3.0f, 0.01f, 0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 }, // hits compact (inst 1)
            { 20.0f, 0.0f, 3.0f, 0.01f, 0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 }, // misses old refit y=0
            { 20.0f, 3.0f, 3.0f, 0.01f, 0.0f, 0.0f, -1.0f, 10.0f, 0xFFu, 0, 0, 0 }, // hits refit (inst 2, y=3)
        };
        CurveRayResult lcOut[4];
        dispatchCurveTrace(dev, q, psoTrace, tlasLc, nil, lcRays, lcOut, 4, @"trace-curve-lifecycle");
        require(lcOut[0].hitType == 3u && lcOut[0].instanceId == 0u &&
                lcOut[1].hitType == 3u && lcOut[1].instanceId == 1u &&
                lcOut[2].hitType == 0u &&
                lcOut[3].hitType == 3u && lcOut[3].instanceId == 2u,
                [NSString stringWithFormat:@"curve lifecycle mismatch: %u/%u %u/%u %u %u/%u",
                                            lcOut[0].hitType, lcOut[0].instanceId,
                                            lcOut[1].hitType, lcOut[1].instanceId,
                                            lcOut[2].hitType,
                                            lcOut[3].hitType, lcOut[3].instanceId]);

        printf("METAL3_RT_CURVES_PASS oracle=%d/10000 mism=%d ift=ok lifecycle=ok\n", exactMatches, hitMismatch);
    }
    return 0;
}
