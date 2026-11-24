// test-metal3-raytracing.m: live macOS / RX 9060 XT verification for Task 7.3
// (Hardware Ray Tracing Runtime Binding & MTLIntersectionFunctionTable:
//  honest capability queries, compute ray query hit/miss/mask/instance transform/userID/barycentrics,
//  compacted & refit BLAS/TLAS traversal, argument-buffer 64-bit BDA AS traversal,
//  fragment-stage shadow ray query, custom AABB MTLIntersectionFunctionTable + triangle any-hit
//  filter + dynamic opaque triangle swap + null/OOB/early-ARC-release safety).
//
// Build on macOS (verify BOTH -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-raytracing.m -o "$TMPDIR/test-metal3-raytracing"
// Run:
//   RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 "$TMPDIR/test-metal3-raytracing" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-raytracing" --reference
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

@interface NSObject (GarnetTestRTInit)
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
    fprintf(stderr, "METAL3_RT_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static BOOL nearf(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

static void finishOK(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
}

static MTLPackedFloat4x3 makeTransform(float tx, float ty, float tz, float sx) {
    MTLPackedFloat4x3 m;
    m.columns[0] = MTLPackedFloat3Make(sx, 0.0f, 0.0f);
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
} RayInput;

typedef struct {
    uint32_t hitType;      // 0=none, 1=triangle, 2=bounding_box
    uint32_t instanceId;
    uint32_t userId;
    float distance;
    float baryX, baryY;
    uint32_t pad0, pad1;
} RayResult;

static NSString *const kRTShaders = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace metal::raytracing;\n"
    "\n"
    "struct RayInput {\n"
    "    float4 origin_tmin;\n"
    "    float4 dir_tmax;\n"
    "    uint mask; uint p0; uint p1; uint p2;\n"
    "};\n"
    "struct RayResult {\n"
    "    uint hitType;\n"
    "    uint instanceId;\n"
    "    uint userId;\n"
    "    float distance;\n"
    "    float baryX;\n"
    "    float baryY;\n"
    "    uint p0; uint p1;\n"
    "};\n"
    "\n"
    "kernel void trace_direct(\n"
    "    device const RayInput *rays [[buffer(0)]],\n"
    "    device RayResult *out       [[buffer(1)]],\n"
    "    instance_acceleration_structure scene [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, instancing> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask);\n"
    "    RayResult o = {0, 0, 0, -1.0f, 0.0f, 0.0f, 0, 0};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.userId = res.user_instance_id;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "struct SceneArgBuf {\n"
    "    instance_acceleration_structure scene [[id(0)]];\n"
    "};\n"
    "\n"
    "kernel void trace_argbuf(\n"
    "    constant SceneArgBuf &args  [[buffer(0)]],\n"
    "    device const RayInput *rays [[buffer(1)]],\n"
    "    device RayResult *out       [[buffer(2)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, instancing> it;\n"
    "    auto res = it.intersect(r, args.scene, rays[tid].mask);\n"
    "    RayResult o = {0, 0, 0, -1.0f, 0.0f, 0.0f, 0, 0};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.userId = res.user_instance_id;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n"
    "\n"
    "struct VSOut {\n"
    "    float4 pos [[position]];\n"
    "    float2 uv;\n"
    "};\n"
    "vertex VSOut shadow_vs(uint vid [[vertex_id]]) {\n"
    "    float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };\n"
    "    VSOut o;\n"
    "    o.pos = float4(p[vid], 0.0f, 1.0f);\n"
    "    o.uv = p[vid] * 0.5f + 0.5f;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 shadow_fs(VSOut in [[stage_in]],\n"
    "                          instance_acceleration_structure scene [[buffer(0)]]) {\n"
    "    ray r;\n"
    "    // Left half (uv.x < 0.5) traces through triangle at (0.3, 0.3, -3); right half misses at x=3.0.\n"
    "    r.origin = (in.uv.x < 0.5f) ? float3(0.3f, 0.3f, 0.0f) : float3(3.0f, 0.3f, 0.0f);\n"
    "    r.direction = float3(0.0f, 0.0f, -1.0f);\n"
    "    r.min_distance = 0.001f;\n"
    "    r.max_distance = 100.0f;\n"
    "    intersector<triangle_data, instancing> it;\n"
    "    it.accept_any_intersection(true);\n"
    "    auto res = it.intersect(r, scene, 0xFF);\n"
    "    bool occluded = (res.type != intersection_type::none);\n"
    "    return occluded ? float4(0.2f, 0.2f, 0.8f, 1.0f) : float4(0.9f, 0.9f, 0.1f, 1.0f);\n"
    "}\n"
    "\n"
    "struct SphereHit {\n"
    "    bool accept [[accept_intersection]];\n"
    "    float distance [[distance]];\n"
    "};\n"
    "[[intersection(bounding_box, instancing)]]\n"
    "SphereHit aabb_sphere(float3 origin [[origin]],\n"
    "                      float3 dir [[direction]],\n"
    "                      float tmin [[min_distance]],\n"
    "                      float tmax [[max_distance]])\n"
    "{\n"
    "    float a = dot(dir, dir);\n"
    "    float b = 2.0f * dot(origin, dir);\n"
    "    float c = dot(origin, origin) - 0.25f; // radius = 0.5\n"
    "    float disc = b * b - 4.0f * a * c;\n"
    "    SphereHit h = { false, 0.0f };\n"
    "    if (disc < 0.0f) return h;\n"
    "    float t = (-b - sqrt(disc)) / (2.0f * a);\n"
    "    if (t >= tmin && t <= tmax) {\n"
    "        h.accept = true;\n"
    "        h.distance = t;\n"
    "    }\n"
    "    return h;\n"
    "}\n"
    "[[intersection(triangle, triangle_data, instancing)]]\n"
    "bool tri_anyhit_filter(float2 bary [[barycentric_coord]]) {\n"
    "    return bary.x >= 0.3f;\n"
    "}\n"
    "kernel void trace_ift(\n"
    "    device const RayInput *rays [[buffer(0)]],\n"
    "    device RayResult *out       [[buffer(1)]],\n"
    "    instance_acceleration_structure scene [[buffer(2)]],\n"
    "    intersection_function_table<triangle_data, instancing> ift [[buffer(3)]],\n"
    "    uint tid [[thread_position_in_grid]])\n"
    "{\n"
    "    ray r;\n"
    "    r.origin = rays[tid].origin_tmin.xyz;\n"
    "    r.direction = rays[tid].dir_tmax.xyz;\n"
    "    r.min_distance = rays[tid].origin_tmin.w;\n"
    "    r.max_distance = rays[tid].dir_tmax.w;\n"
    "    intersector<triangle_data, instancing> it;\n"
    "    auto res = it.intersect(r, scene, rays[tid].mask, ift);\n"
    "    RayResult o = {0, 0, 0, -1.0f, 0.0f, 0.0f, 0, 0};\n"
    "    if (res.type == intersection_type::triangle) {\n"
    "        o.hitType = 1;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.distance = res.distance;\n"
    "        o.baryX = res.triangle_barycentric_coord.x;\n"
    "        o.baryY = res.triangle_barycentric_coord.y;\n"
    "    } else if (res.type == intersection_type::bounding_box) {\n"
    "        o.hitType = 2;\n"
    "        o.instanceId = res.instance_id;\n"
    "        o.distance = res.distance;\n"
    "    }\n"
    "    out[tid] = o;\n"
    "}\n";

static id<MTLAccelerationStructure> buildTriBLAS(id<MTLDevice> dev,
                                                 id<MTLCommandQueue> q,
                                                 id<MTLBuffer> vbuf,
                                                 BOOL opaque,
                                                 MTLAccelerationStructureUsage usage,
                                                 NSString *tag) {
    MTLAccelerationStructureTriangleGeometryDescriptor *g =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    g.vertexBuffer = vbuf;
    g.vertexBufferOffset = 0;
    g.vertexStride = sizeof(float) * 3;
    g.triangleCount = 1;
    g.opaque = opaque;

    MTLPrimitiveAccelerationStructureDescriptor *d =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    d.geometryDescriptors = @[ g ];
    d.usage = usage;

    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:d];
    require(sz.accelerationStructureSize > 0 && sz.buildScratchBufferSize > 0,
            [NSString stringWithFormat:@"%@: invalid BLAS sizes", tag]);
    id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
    require(as != nil && scratch != nil, [NSString stringWithFormat:@"%@: BLAS alloc failed", tag]);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return as;
}

static id<MTLAccelerationStructure> buildAabbBLAS(id<MTLDevice> dev,
                                                  id<MTLCommandQueue> q,
                                                  NSString *tag) {
    float aabb[6] = { -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    id<MTLBuffer> bbuf = [dev newBufferWithBytes:aabb length:sizeof(aabb) options:MTLResourceStorageModeShared];
    require(bbuf != nil, @"aabb buffer alloc failed");

    MTLAccelerationStructureBoundingBoxGeometryDescriptor *g =
        [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
    g.boundingBoxBuffer = bbuf;
    g.boundingBoxBufferOffset = 0;
    g.boundingBoxStride = sizeof(aabb);
    g.boundingBoxCount = 1;
    g.opaque = NO;

    MTLPrimitiveAccelerationStructureDescriptor *d =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    d.geometryDescriptors = @[ g ];
    d.usage = MTLAccelerationStructureUsageNone;

    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:d];
    id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
    require(as != nil && scratch != nil, @"aabb BLAS alloc failed");

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return as;
}

static id<MTLAccelerationStructure> buildUserIDTLAS(id<MTLDevice> dev,
                                                    id<MTLCommandQueue> q,
                                                    NSArray<id<MTLAccelerationStructure>> *instanced,
                                                    const MTLAccelerationStructureUserIDInstanceDescriptor *descs,
                                                    NSUInteger count,
                                                    MTLAccelerationStructureUsage usage,
                                                    NSString *tag) {
    id<MTLBuffer> ibuf = [dev newBufferWithBytes:descs
                                          length:count * sizeof(*descs)
                                         options:MTLResourceStorageModeShared];
    require(ibuf != nil, [NSString stringWithFormat:@"%@: instance buffer alloc failed", tag]);

    MTLInstanceAccelerationStructureDescriptor *d =
        [MTLInstanceAccelerationStructureDescriptor descriptor];
    d.instancedAccelerationStructures = instanced;
    d.instanceCount = count;
    d.instanceDescriptorBuffer = ibuf;
    d.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    d.usage = usage;

    MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:d];
    id<MTLAccelerationStructure> tlas = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
    require(tlas != nil && scratch != nil, [NSString stringWithFormat:@"%@: TLAS alloc failed", tag]);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:tlas descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    finishOK(cb, tag);
    return tlas;
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
            setenv("RDNA4M_TEST_INPROC", "1", 1);
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

        // 1. Honest capability assertions.
        require(dev.supportsRaytracing, @"supportsRaytracing must be YES");
        require(dev.supportsRaytracingFromRender, @"supportsRaytracingFromRender must be YES");
        if (!reference) {
            require(!dev.supportsPrimitiveMotionBlur,
                    @"Garnet supportsPrimitiveMotionBlur must honestly be NO on RADV GFX12");
        }
        printf("PASS capabilities (rt=%d renderRT=%d motionBlur=%d)\n",
               dev.supportsRaytracing ? 1 : 0,
               dev.supportsRaytracingFromRender ? 1 : 0,
               dev.supportsPrimitiveMotionBlur ? 1 : 0);

        // Compile MSL 2.4 ray tracing shaders.
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_4;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kRTShaders options:opts error:&err];
        require(lib != nil, [NSString stringWithFormat:@"MSL compile failed: %@", err]);

        id<MTLFunction> fnDirect = [lib newFunctionWithName:@"trace_direct"];
        id<MTLFunction> fnArgBuf = [lib newFunctionWithName:@"trace_argbuf"];
        id<MTLFunction> fnVS     = [lib newFunctionWithName:@"shadow_vs"];
        id<MTLFunction> fnFS     = [lib newFunctionWithName:@"shadow_fs"];
        id<MTLFunction> fnIFT    = [lib newFunctionWithName:@"trace_ift"];
        id<MTLFunction> fnSphere = [lib newFunctionWithName:@"aabb_sphere"];
        id<MTLFunction> fnAnyHit = [lib newFunctionWithName:@"tri_anyhit_filter"];
        require(fnDirect && fnArgBuf && fnVS && fnFS && fnIFT && fnSphere && fnAnyHit,
                @"missing shader entry point(s)");

        id<MTLComputePipelineState> psoDirect = [dev newComputePipelineStateWithFunction:fnDirect error:&err];
        require(psoDirect != nil, [NSString stringWithFormat:@"psoDirect failed: %@", err]);
        id<MTLComputePipelineState> psoArgBuf = [dev newComputePipelineStateWithFunction:fnArgBuf error:&err];
        require(psoArgBuf != nil, [NSString stringWithFormat:@"psoArgBuf failed: %@", err]);

        // 2. Compute ray query: hit/miss/mask/instance transform/userID/barycentrics.
        float triVerts[9] = { 0.0f, 0.0f, 0.0f,  2.0f, 0.0f, 0.0f,  0.0f, 2.0f, 0.0f };
        id<MTLBuffer> triVBuf = [dev newBufferWithBytes:triVerts length:sizeof(triVerts) options:MTLResourceStorageModeShared];
        require(triVBuf != nil, @"triVBuf alloc failed");

        id<MTLAccelerationStructure> refitBLAS = buildTriBLAS(
            dev, q, triVBuf, YES, MTLAccelerationStructureUsageRefit, @"build-refit-blas");

        MTLAccelerationStructureUserIDInstanceDescriptor insts[2];
        memset(insts, 0, sizeof(insts));
        insts[0].transformationMatrix = makeTransform(0.0f, 0.0f, -2.0f, 1.0f);
        insts[0].options = MTLAccelerationStructureInstanceOptionOpaque;
        insts[0].mask = 0x01u;
        insts[0].accelerationStructureIndex = 0u;
        insts[0].userID = 101u;

        insts[1].transformationMatrix = makeTransform(0.0f, 0.0f, -5.0f, 2.0f);
        insts[1].options = MTLAccelerationStructureInstanceOptionOpaque;
        insts[1].mask = 0x02u;
        insts[1].accelerationStructureIndex = 0u;
        insts[1].userID = 202u;

        id<MTLAccelerationStructure> mainTLAS = buildUserIDTLAS(
            dev, q, @[ refitBLAS ], insts, 2, MTLAccelerationStructureUsageRefit, @"build-main-tlas");

        RayInput rays[4] = {
            { 0.5f, 0.5f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0xFFu, 0, 0, 0 }, // hits inst 0 @ t=2, bary=(0.25,0.25)
            { 0.5f, 0.5f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x02u, 0, 0, 0 }, // mask->inst 1 @ t=5, bary=(0.125,0.25)
            { 0.5f, 0.5f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x04u, 0, 0, 0 }, // mask miss
            { 5.0f, 5.0f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0xFFu, 0, 0, 0 }, // geometric miss
        };
        id<MTLBuffer> rayBuf = [dev newBufferWithBytes:rays length:sizeof(rays) options:MTLResourceStorageModeShared];
        id<MTLBuffer> outBuf = [dev newBufferWithLength:sizeof(RayResult) * 4 options:MTLResourceStorageModeShared];
        require(rayBuf != nil && outBuf != nil, @"ray/out buffers alloc failed");

        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:psoDirect];
            [ce setBuffer:rayBuf offset:0 atIndex:0];
            [ce setBuffer:outBuf offset:0 atIndex:1];
            [ce setAccelerationStructure:mainTLAS atBufferIndex:2];
            [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
            [ce endEncoding];
            finishOK(cb, @"compute-direct-trace");
        }
        const RayResult *res = (const RayResult *)outBuf.contents;
        require(res[0].hitType == 1 && res[0].instanceId == 0 && res[0].userId == 101 &&
                nearf(res[0].distance, 2.0f, 1e-3f) && nearf(res[0].baryX, 0.25f, 1e-3f) && nearf(res[0].baryY, 0.25f, 1e-3f),
                [NSString stringWithFormat:@"ray0 mismatch: type=%u id=%u uid=%u t=%.3f bary=(%.3f,%.3f)",
                 res[0].hitType, res[0].instanceId, res[0].userId, res[0].distance, res[0].baryX, res[0].baryY]);
        require(res[1].hitType == 1 && res[1].instanceId == 1 && res[1].userId == 202 &&
                nearf(res[1].distance, 5.0f, 1e-3f) && nearf(res[1].baryX, 0.125f, 1e-3f) && nearf(res[1].baryY, 0.25f, 1e-3f),
                [NSString stringWithFormat:@"ray1 mismatch: type=%u id=%u uid=%u t=%.3f bary=(%.3f,%.3f)",
                 res[1].hitType, res[1].instanceId, res[1].userId, res[1].distance, res[1].baryX, res[1].baryY]);
        require(res[2].hitType == 0 && res[3].hitType == 0,
                [NSString stringWithFormat:@"miss rays unexpectedly hit: r2=%u r3=%u", res[2].hitType, res[3].hitType]);
        printf("PASS compute direct ray query (hit/miss/mask/transform/userID/bary)\n");

        // 3. Compacted BLAS traversal.
        id<MTLAccelerationStructure> compactSrc = buildTriBLAS(
            dev, q, triVBuf, YES, MTLAccelerationStructureUsageNone, @"build-compact-src-blas");
        id<MTLBuffer> cszBuf = [dev newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
        require(cszBuf != nil, @"compact size buffer alloc failed");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc writeCompactedAccelerationStructureSize:compactSrc toBuffer:cszBuf offset:0 sizeDataType:MTLDataTypeULong];
            [enc endEncoding];
            finishOK(cb, @"write-compacted-size");
        }
        uint64_t compactSize = *(const uint64_t *)cszBuf.contents;
        require(compactSize > 0, @"compacted size must be > 0");
        id<MTLAccelerationStructure> compactedBLAS = [dev newAccelerationStructureWithSize:(NSUInteger)compactSize];
        require(compactedBLAS != nil, @"compactedBLAS alloc failed");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc copyAndCompactAccelerationStructure:compactSrc toAccelerationStructure:compactedBLAS];
            [enc endEncoding];
            finishOK(cb, @"copy-and-compact");
        }
        MTLAccelerationStructureUserIDInstanceDescriptor compactInst;
        memset(&compactInst, 0, sizeof(compactInst));
        compactInst.transformationMatrix = makeTransform(0.0f, 0.0f, -3.0f, 1.0f);
        compactInst.options = MTLAccelerationStructureInstanceOptionOpaque;
        compactInst.mask = 0xFFu;
        compactInst.accelerationStructureIndex = 0u;
        compactInst.userID = 303u;
        id<MTLAccelerationStructure> compactTLAS = buildUserIDTLAS(
            dev, q, @[ compactedBLAS ], &compactInst, 1, MTLAccelerationStructureUsageNone, @"build-compact-tlas");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:psoDirect];
            [ce setBuffer:rayBuf offset:0 atIndex:0];
            [ce setBuffer:outBuf offset:0 atIndex:1];
            [ce setAccelerationStructure:compactTLAS atBufferIndex:2];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            finishOK(cb, @"trace-compacted");
        }
        require(res[0].hitType == 1 && res[0].userId == 303 && nearf(res[0].distance, 3.0f, 1e-3f),
                [NSString stringWithFormat:@"compacted trace mismatch: type=%u uid=%u t=%.3f",
                 res[0].hitType, res[0].userId, res[0].distance]);
        printf("PASS compacted BLAS/TLAS traversal (t=%.3f)\n", res[0].distance);

        // 4. Refit BLAS + TLAS traversal (shift triangle vertices by z=-1.5 -> t shifts 2.0 -> 3.5).
        float refitVerts[9] = { 0.0f, 0.0f, -1.5f,  2.0f, 0.0f, -1.5f,  0.0f, 2.0f, -1.5f };
        memcpy(triVBuf.contents, refitVerts, sizeof(refitVerts));
        {
            MTLAccelerationStructureTriangleGeometryDescriptor *g =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            g.vertexBuffer = triVBuf;
            g.vertexStride = sizeof(float) * 3;
            g.triangleCount = 1;
            g.opaque = YES;
            MTLPrimitiveAccelerationStructureDescriptor *bd =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            bd.geometryDescriptors = @[ g ];
            bd.usage = MTLAccelerationStructureUsageRefit;
            MTLAccelerationStructureSizes bsz = [dev accelerationStructureSizesWithDescriptor:bd];
            id<MTLBuffer> bscratch = [dev newBufferWithLength:MAX(bsz.refitScratchBufferSize, (NSUInteger)256)
                                                      options:MTLResourceStorageModePrivate];

            id<MTLBuffer> ibuf = [dev newBufferWithBytes:insts length:sizeof(insts) options:MTLResourceStorageModeShared];
            MTLInstanceAccelerationStructureDescriptor *td =
                [MTLInstanceAccelerationStructureDescriptor descriptor];
            td.instancedAccelerationStructures = @[ refitBLAS ];
            td.instanceCount = 2;
            td.instanceDescriptorBuffer = ibuf;
            td.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
            td.usage = MTLAccelerationStructureUsageRefit;
            MTLAccelerationStructureSizes tsz = [dev accelerationStructureSizesWithDescriptor:td];
            id<MTLBuffer> tscratch = [dev newBufferWithLength:MAX(tsz.refitScratchBufferSize, (NSUInteger)256)
                                                      options:MTLResourceStorageModePrivate];

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            [enc refitAccelerationStructure:refitBLAS descriptor:bd destination:refitBLAS
                              scratchBuffer:bscratch scratchBufferOffset:0];
            [enc refitAccelerationStructure:mainTLAS descriptor:td destination:mainTLAS
                              scratchBuffer:tscratch scratchBufferOffset:0];
            [enc endEncoding];
            finishOK(cb, @"refit-blas-tlas");
        }
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:psoDirect];
            [ce setBuffer:rayBuf offset:0 atIndex:0];
            [ce setBuffer:outBuf offset:0 atIndex:1];
            [ce setAccelerationStructure:mainTLAS atBufferIndex:2];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            finishOK(cb, @"trace-refit");
        }
        require(res[0].hitType == 1 && res[0].userId == 101 && nearf(res[0].distance, 3.5f, 1e-3f),
                [NSString stringWithFormat:@"refit trace mismatch: type=%u uid=%u t=%.3f",
                 res[0].hitType, res[0].userId, res[0].distance]);
        printf("PASS refit BLAS/TLAS traversal (t=%.3f)\n", res[0].distance);

        // 5. Argument-buffer 64-bit BDA (`u64_device_address`) AS traversal + early ARC release safety.
        {
            id<MTLArgumentEncoder> argEnc = [fnArgBuf newArgumentEncoderWithBufferIndex:0];
            require(argEnc != nil && argEnc.encodedLength >= sizeof(uint64_t), @"argEnc unavailable");
            id<MTLBuffer> argBuf = [dev newBufferWithLength:argEnc.encodedLength options:MTLResourceStorageModeShared];
            require(argBuf != nil, @"argBuf alloc failed");

            id<MTLCommandBuffer> cb = [q commandBuffer];
            @autoreleasepool {
                id<MTLAccelerationStructure> ephemeralTLAS = buildUserIDTLAS(
                    dev, q, @[ compactedBLAS ], &compactInst, 1, MTLAccelerationStructureUsageNone, @"ephemeral-tlas");
                [argEnc setArgumentBuffer:argBuf offset:0];
                [argEnc setAccelerationStructure:ephemeralTLAS atIndex:0];

                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:psoArgBuf];
                [ce useResource:ephemeralTLAS usage:MTLResourceUsageRead];
                [ce useResource:compactedBLAS usage:MTLResourceUsageRead];
                [ce setBuffer:argBuf offset:0 atIndex:0];
                [ce setBuffer:rayBuf offset:0 atIndex:1];
                [ce setBuffer:outBuf offset:0 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [ce endEncoding];
                ephemeralTLAS = nil; // drop strong ref before commit to verify GPU retirement retention
            }
            finishOK(cb, @"trace-argbuf-bda");
        }
        require(res[0].hitType == 1 && res[0].userId == 303 && nearf(res[0].distance, 3.0f, 1e-3f),
                [NSString stringWithFormat:@"argbuf BDA trace mismatch: type=%u uid=%u t=%.3f",
                 res[0].hitType, res[0].userId, res[0].distance]);
        printf("PASS argument-buffer 64-bit BDA AS traversal + early ARC release\n");

        // 6. Fragment-stage shadow ray query.
        {
            MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
            rpd.vertexFunction = fnVS;
            rpd.fragmentFunction = fnFS;
            rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
            require(rpso != nil, [NSString stringWithFormat:@"shadow render PSO failed: %@", err]);

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:32 height:32 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> rtTex = [dev newTextureWithDescriptor:td];
            require(rtTex != nil, @"shadow render target alloc failed");

            const NSUInteger bpr = 32 * 4;
            const NSUInteger rbBytes = bpr * 32;
            id<MTLBuffer> readback = [dev newBufferWithLength:rbBytes options:MTLResourceStorageModeShared];
            require(readback != nil && readback.contents != NULL, @"shadow readback buffer alloc failed");
            memset(readback.contents, 0x5A, rbBytes);

            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = rtTex;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            require(re != nil, @"shadow render encoder nil");
            [re setRenderPipelineState:rpso];
            [re setFragmentAccelerationStructure:compactTLAS atBufferIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];

            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            require(blit != nil, @"shadow readback blit encoder nil");
            [blit copyFromTexture:rtTex sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(32, 32, 1)
                         toBuffer:readback destinationOffset:0
             destinationBytesPerRow:bpr destinationBytesPerImage:rbBytes];
            [blit endEncoding];
            finishOK(cb, @"fragment-shadow-ray");

            const uint8_t *px = (const uint8_t *)readback.contents;
            const uint8_t *leftPx  = &px[(16 * 32 + 8) * 4];  // occluded: ~(51, 51, 204, 255)
            const uint8_t *rightPx = &px[(16 * 32 + 24) * 4]; // lit:      ~(230, 230, 26, 255)
            require(leftPx[2] > 180 && leftPx[0] < 80,
                    [NSString stringWithFormat:@"fragment occluded pixel mismatch: (%u,%u,%u,%u)",
                     leftPx[0], leftPx[1], leftPx[2], leftPx[3]]);
            require(rightPx[0] > 200 && rightPx[1] > 200 && rightPx[2] < 60,
                    [NSString stringWithFormat:@"fragment lit pixel mismatch: (%u,%u,%u,%u)",
                     rightPx[0], rightPx[1], rightPx[2], rightPx[3]]);
            printf("PASS fragment-stage shadow ray query (occluded=(%u,%u,%u) lit=(%u,%u,%u))\n",
                   leftPx[0], leftPx[1], leftPx[2], rightPx[0], rightPx[1], rightPx[2]);
        }

        // 7. Custom AABB MTLIntersectionFunctionTable + triangle any-hit filter + dynamic opaque swap + null/OOB safety.
        {
            MTLComputePipelineDescriptor *cpd = [MTLComputePipelineDescriptor new];
            cpd.computeFunction = fnIFT;
            MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
            lf.functions = @[ fnSphere, fnAnyHit ];
            cpd.linkedFunctions = lf;
            id<MTLComputePipelineState> psoIFT = [dev newComputePipelineStateWithDescriptor:cpd
                                                                                    options:MTLPipelineOptionNone
                                                                                 reflection:nil
                                                                                      error:&err];
            require(psoIFT != nil, [NSString stringWithFormat:@"psoIFT creation failed: %@", err]);

            id<MTLFunctionHandle> hSphere = [psoIFT functionHandleWithFunction:fnSphere];
            id<MTLFunctionHandle> hAnyHit = [psoIFT functionHandleWithFunction:fnAnyHit];
            require(hSphere != nil && hAnyHit != nil, @"IFT function handles missing");

            MTLIntersectionFunctionTableDescriptor *iftd =
                [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
            iftd.functionCount = 3;
            id<MTLIntersectionFunctionTable> ift = [psoIFT newIntersectionFunctionTableWithDescriptor:iftd];
            require(ift != nil, @"newIntersectionFunctionTableWithDescriptor failed");
            if (@available(macOS 13.0, *)) {
                require(ift.gpuResourceID._impl != 0, @"IFT gpuResourceID must be non-zero");
            }
            [ift setFunction:hSphere atIndex:0];
            [ift setFunction:hAnyHit atIndex:1];
            [ift setFunction:nil atIndex:2];

            id<MTLAccelerationStructure> aabbBLAS = buildAabbBLAS(dev, q, @"build-aabb-blas");
            float nonOpaqueVerts[9] = { 0.0f, 0.0f, 0.0f,  2.0f, 0.0f, 0.0f,  0.0f, 2.0f, 0.0f };
            id<MTLBuffer> noVBuf = [dev newBufferWithBytes:nonOpaqueVerts length:sizeof(nonOpaqueVerts) options:MTLResourceStorageModeShared];
            id<MTLAccelerationStructure> noTriBLAS = buildTriBLAS(
                dev, q, noVBuf, NO, MTLAccelerationStructureUsageNone, @"build-nonopaque-tri-blas");

            MTLAccelerationStructureUserIDInstanceDescriptor iftInsts[2];
            memset(iftInsts, 0, sizeof(iftInsts));
            iftInsts[0].transformationMatrix = makeTransform(0.0f, 0.0f, -4.0f, 1.0f);
            iftInsts[0].options = MTLAccelerationStructureInstanceOptionNonOpaque;
            iftInsts[0].mask = 0x01u;
            iftInsts[0].intersectionFunctionTableOffset = 0u;
            iftInsts[0].accelerationStructureIndex = 0u;
            iftInsts[0].userID = 401u;

            iftInsts[1].transformationMatrix = makeTransform(0.0f, 0.0f, -2.0f, 1.0f);
            iftInsts[1].options = MTLAccelerationStructureInstanceOptionNonOpaque;
            iftInsts[1].mask = 0x02u;
            iftInsts[1].intersectionFunctionTableOffset = 1u;
            iftInsts[1].accelerationStructureIndex = 1u;
            iftInsts[1].userID = 402u;

            id<MTLAccelerationStructure> iftTLAS = buildUserIDTLAS(
                dev, q, @[ aabbBLAS, noTriBLAS ], iftInsts, 2, MTLAccelerationStructureUsageNone, @"build-ift-tlas");

            RayInput iftRays[4] = {
                { 0.0f, 0.0f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x01u, 0, 0, 0 }, // AABB sphere hit @ t=3.5
                { 0.8f, 0.8f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x01u, 0, 0, 0 }, // inside AABB, outside r=0.5 -> miss
                { 0.2f, 0.2f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x02u, 0, 0, 0 }, // tri bary.x=0.1 < 0.3 -> any-hit reject
                { 1.0f, 0.2f, 0.0f, 0.001f,  0.0f, 0.0f, -1.0f, 100.0f, 0x02u, 0, 0, 0 }, // tri bary.x=0.5 >= 0.3 -> any-hit accept @ t=2
            };
            memcpy(rayBuf.contents, iftRays, sizeof(iftRays));

            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:psoIFT];
                [ce setBuffer:rayBuf offset:0 atIndex:0];
                [ce setBuffer:outBuf offset:0 atIndex:1];
                [ce setAccelerationStructure:iftTLAS atBufferIndex:2];
                [ce setIntersectionFunctionTable:ift atBufferIndex:3];
                [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
                [ce endEncoding];
                finishOK(cb, @"trace-ift-custom");
            }
            require(res[0].hitType == 2 && nearf(res[0].distance, 3.5f, 1e-3f),
                    [NSString stringWithFormat:@"AABB sphere hit mismatch: type=%u t=%.3f", res[0].hitType, res[0].distance]);
            require(res[1].hitType == 0,
                    [NSString stringWithFormat:@"AABB corner ray should miss sphere, got type=%u", res[1].hitType]);
            require(res[2].hitType == 0,
                    [NSString stringWithFormat:@"tri any-hit filter should reject bary.x=0.1, got type=%u", res[2].hitType]);
            require(res[3].hitType == 1 && nearf(res[3].distance, 2.0f, 1e-3f),
                    [NSString stringWithFormat:@"tri any-hit filter should accept bary.x=0.5, got type=%u t=%.3f",
                     res[3].hitType, res[3].distance]);

            // Swap slot 1 to opaque triangle signature -> Ray 2 (bary.x=0.1) must now HIT at t=2.0.
            [ift setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignatureInstancing |
                                                                     MTLIntersectionFunctionSignatureTriangleData)
                                                            atIndex:1];
            // Clear slot 0 to nil -> Ray 0 (AABB) must safely MISS (no GPU fault).
            [ift setFunction:nil atIndex:0];
            {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:psoIFT];
                [ce setBuffer:rayBuf offset:0 atIndex:0];
                [ce setBuffer:outBuf offset:0 atIndex:1];
                [ce setAccelerationStructure:iftTLAS atBufferIndex:2];
                [ce setIntersectionFunctionTable:ift atBufferIndex:3];
                [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
                [ce endEncoding];
                finishOK(cb, @"trace-ift-opaque-and-null");
            }
            require(res[0].hitType == 0, @"null AABB IFT slot must safely miss");
            require(res[2].hitType == 1 && nearf(res[2].distance, 2.0f, 1e-3f),
                    [NSString stringWithFormat:@"opaque triangle IFT slot must confirm Ray 2, got type=%u t=%.3f",
                     res[2].hitType, res[2].distance]);

            // Empty (0-slot) IFT OOB safety + early ARC release of IFT before commit (Garnet OOB ABI guarantee).
            if (!reference) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                @autoreleasepool {
                    MTLIntersectionFunctionTableDescriptor *emptyDesc =
                        [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
                    emptyDesc.functionCount = 0;
                    id<MTLIntersectionFunctionTable> emptyIFT =
                        [psoIFT newIntersectionFunctionTableWithDescriptor:emptyDesc];
                    require(emptyIFT != nil, @"empty IFT creation failed");

                    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setComputePipelineState:psoIFT];
                    [ce setBuffer:rayBuf offset:0 atIndex:0];
                    [ce setBuffer:outBuf offset:0 atIndex:1];
                    [ce setAccelerationStructure:iftTLAS atBufferIndex:2];
                    [ce setIntersectionFunctionTable:emptyIFT atBufferIndex:3];
                    [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
                    [ce endEncoding];
                    emptyIFT = nil;
                }
                finishOK(cb, @"trace-ift-empty-oob");
                require(res[0].hitType == 0 && res[2].hitType == 1,
                        @"empty IFT OOB: AABB safely misses and triangle defaults to confirm");
            }
            printf("PASS MTLIntersectionFunctionTable (AABB sphere + tri any-hit + opaque swap + null/OOB)\n");
        }

        printf("METAL3_RT_PASS\n");
    }
    return 0;
}
