#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void finish(id<MTLCommandBuffer> cb, NSString *tag, BOOL *okOut) {
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error) {
        printf("PROBE_EXEC_FAIL %s status=%lu err=%s\n", tag.UTF8String,
               (unsigned long)cb.status, cb.error.localizedDescription.UTF8String);
        *okOut = NO;
    } else {
        *okOut = YES;
    }
}

static void probeComputeCase(id<MTLDevice> dev, id<MTLCommandQueue> q, NSString *name,
                             NSString *src, NSString *fnName, MTLFunctionConstantValues *fcv,
                             NSUInteger threads, void (^verify)(const uint32_t *out, NSUInteger tew)) {
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion2_1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (!lib) {
        printf("PROBE_STATUS %-28s METAL_COMPILE_FAIL %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        return;
    }
    id<MTLFunction> fn = fcv ? [lib newFunctionWithName:fnName constantValues:fcv error:&err]
                             : [lib newFunctionWithName:fnName];
    if (!fn) {
        printf("PROBE_STATUS %-28s FN_CREATE_FAIL %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        return;
    }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
        printf("PROBE_STATUS %-28s TRANSLATE_OR_PSO_FAIL %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        return;
    }
    id<MTLBuffer> out = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> aux = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    memset(out.contents, 0, 4096);
    memset(aux.contents, 0, 4096);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setBuffer:out offset:0 atIndex:0];
    [ce setBuffer:aux offset:0 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
    [ce endEncoding];
    BOOL ok = NO;
    finish(cb, name, &ok);
    if (!ok) {
        printf("PROBE_STATUS %-28s EXEC_FAIL\n", name.UTF8String);
        return;
    }
    verify((const uint32_t *)out.contents, pso.threadExecutionWidth);
}

static void probeRenderCase(id<MTLDevice> dev, id<MTLCommandQueue> q, NSString *name,
                            NSString *src, NSString *vsName, NSString *fsName,
                            NSUInteger sampleCount) {
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion2_1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (!lib) {
        printf("PROBE_STATUS %-28s METAL_COMPILE_FAIL %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        return;
    }
    id<MTLFunction> vs = [lib newFunctionWithName:vsName];
    id<MTLFunction> fs = [lib newFunctionWithName:fsName];
    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = vs; rpd.fragmentFunction = fs;
    rpd.rasterSampleCount = sampleCount;
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
    if (!pso) {
        printf("PROBE_STATUS %-28s TRANSLATE_OR_PSO_FAIL %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        return;
    }
    MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    if (sampleCount > 1) { rtd.textureType = MTLTextureType2DMultisample; rtd.sampleCount = sampleCount; }
    rtd.usage = MTLTextureUsageRenderTarget; rtd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
    MTLTextureDescriptor *resd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    resd.usage = MTLTextureUsageRenderTarget; resd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> res = sampleCount > 1 ? [dev newTextureWithDescriptor:resd] : rt;

    id<MTLBuffer> storage = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    memset(storage.contents, 0, 256);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    if (sampleCount > 1) {
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[0].resolveTexture = res;
    } else {
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setFragmentBuffer:storage offset:0 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    BOOL ok = NO;
    finish(cb, name, &ok);
    if (!ok) {
        printf("PROBE_STATUS %-28s EXEC_FAIL\n", name.UTF8String);
        return;
    }
    id<MTLBuffer> rb = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb2 = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb2 blitCommandEncoder];
    [bl copyFromTexture:res sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(4,4,1) toBuffer:rb destinationOffset:0
     destinationBytesPerRow:16 destinationBytesPerImage:64];
    [bl endEncoding];
    BOOL ok2 = NO;
    finish(cb2, [name stringByAppendingString:@"_rb"], &ok2);
    const uint8_t *px = rb.contents;
    printf("PROBE_STATUS %-28s PASS pixel0=(%u,%u,%u,%u)\n", name.UTF8String, px[0], px[1], px[2], px[3]);
}

int main(int argc, const char *argv[]) {
    if (argc != 2 || geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(90);
    @autoreleasepool {
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
        NSError *loadError = nil;
        if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&loadError]) return 1;
        id<MTLDevice> dev = [[objc_getClass("GarnetDevice") alloc] init];
        ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        id<MTLCommandQueue> q = [dev newCommandQueue];

        // 1. SIMD reductions + prefix scans
        probeComputeCase(dev, q, @"simd_reduce_prefix",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]],\n"
             "              uint sg_sz [[threads_per_simdgroup]], uint sg_idx [[simdgroup_index_in_threadgroup]]) {\n"
             "  uint v = lane + 1;\n"
             "  uint s = simd_sum(v);\n"
             "  uint mn = simd_min(v);\n"
             "  uint mx = simd_max(v);\n"
             "  uint pe = simd_prefix_exclusive_sum(1u);\n"
             "  uint pi = simd_prefix_inclusive_sum(1u);\n"
             "  if (lane < 8) { out[lane] = pe; out[16 + lane] = pi; }\n"
             "  if (simd_is_first()) { out[32] = s; out[33] = mn; out[34] = mx; out[35] = sg_sz; out[36] = sg_idx; }\n"
             "}\n", @"k", nil, 64, ^(const uint32_t *o, NSUInteger tew) {
                printf("PROBE_STATUS %-28s sum=%u(exp 528) min=%u max=%u sz=%u tew=%lu pe0..3=%u,%u,%u,%u pi0..3=%u,%u,%u,%u\n",
                       "simd_reduce_prefix", o[32], o[33], o[34], o[35], (unsigned long)tew, o[0],o[1],o[2],o[3], o[16],o[17],o[18],o[19]);
                if (tew != o[35]) {
                    printf("PROBE_MISMATCH simd_reduce_prefix tew=%lu != threads_per_simdgroup=%u\n", (unsigned long)tew, o[35]);
                }
            });

        // 2. Individual SIMD shuffles / broadcasts / rotates + votes
        probeComputeCase(dev, q, @"simd_shuf_basic",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]]) {\n"
             "  uint v = lane * 10 + 3;\n"
             "  uint a = simd_broadcast(v, 5);\n"
             "  uint b = simd_shuffle(v, lane ^ 1);\n"
             "  uint c = simd_shuffle_xor(v, 2);\n"
             "  uint d = simd_shuffle_down(v, 1);\n"
             "  uint e = simd_shuffle_up(v, 1);\n"
             "  if (lane == 2) { out[0]=a; out[1]=b; out[2]=c; out[3]=d; out[4]=e; }\n"
             "}\n", @"k", nil, 32, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s bcast5=%u shuf3=%u xor0=%u down3=%u up1=%u\n",
                       "simd_shuf_basic", o[0],o[1],o[2],o[3],o[4]);
            });
        probeComputeCase(dev, q, @"simd_rotate_down",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]]) {\n"
             "  out[lane] = simd_shuffle_rotate_down(lane, 1);\n"
             "}\n", @"k", nil, 32, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s rdown0=%u rdown31=%u\n", "simd_rotate_down", o[0], o[31]);
            });
        probeComputeCase(dev, q, @"simd_rotate_up",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]]) {\n"
             "  out[lane] = simd_shuffle_rotate_up(lane, 1);\n"
             "}\n", @"k", nil, 32, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s rup0=%u rup1=%u\n", "simd_rotate_up", o[0], o[1]);
            });

        // 3. SIMD ballot / any / all / active mask
        probeComputeCase(dev, q, @"simd_vote_basic",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]]) {\n"
             "  bool a = simd_any(lane == 7);\n"
             "  bool b = simd_all(lane < 64);\n"
             "  uint64_t bal = uint64_t(simd_vote::vote_t(simd_ballot(lane < 4)));\n"
             "  simdgroup_barrier(mem_flags::mem_none);\n"
             "  if (lane == 0) { out[0]=a?1:0; out[1]=b?1:0; out[2]=uint(bal); out[3]=uint(bal>>32); }\n"
             "}\n", @"k", nil, 32, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s any=%u all=%u ballot=0x%08x_%08x\n",
                       "simd_vote_basic", o[0],o[1],o[3],o[2]);
            });
        probeComputeCase(dev, q, @"simd_active_mask",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]]) {\n"
             "  uint64_t act = uint64_t(simd_vote::vote_t(simd_active_threads_mask()));\n"
             "  if (lane == 0) { out[0]=uint(act); out[1]=uint(act>>32); }\n"
             "}\n", @"k", nil, 32, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s act=0x%08x_%08x\n", "simd_active_mask", o[1], o[0]);
            });

        // 4. Quad operations (Table 35)
        probeComputeCase(dev, q, @"quad_ops",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]], uint qlane [[thread_index_in_quadgroup]], uint qcnt [[quadgroups_per_threadgroup]], uint gid [[thread_position_in_grid]]) {\n"
             "  uint v = gid + 1;\n"
             "  uint b = quad_broadcast(v, 3);\n"
             "  uint x = quad_shuffle_xor(v, 1);\n"
             "  uint u = quad_shuffle_up(v, 1);\n"
             "  uint d = quad_shuffle_down(v, 1);\n"
             "  uint ru = quad_shuffle_rotate_up(v, 1);\n"
             "  uint s = quad_sum(v);\n"
             "  uint mn = quad_min(v);\n"
             "  uint mx = quad_max(v);\n"
             "  if (gid == 0) { out[0]=b; out[1]=x; out[2]=s; out[3]=mn; out[4]=mx; out[5]=qlane; out[6]=qcnt; out[7]=u; out[8]=d; out[9]=ru; }\n"
             "}\n", @"k", nil, 8, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s b3=%u xor1=%u up1=%u down1=%u rotup1=%u sum=%u min=%u max=%u qcnt=%u\n",
                       "quad_ops", o[0],o[1],o[7],o[8],o[9],o[2],o[3],o[4],o[6]);
            });

        // 5. Device & threadgroup atomics + barriers
        probeComputeCase(dev, q, @"atomics_tg",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device atomic_uint *out [[buffer(0)]], uint tid [[thread_index_in_threadgroup]]) {\n"
             "  threadgroup atomic_uint tg;\n"
             "  if (tid == 0) atomic_store_explicit(&tg, 10u, memory_order_relaxed);\n"
             "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
             "  atomic_fetch_add_explicit(&tg, 1u, memory_order_relaxed);\n"
             "  atomic_fetch_max_explicit(&out[2], tid + 1u, memory_order_relaxed);\n"
             "  uint expected = 0u;\n"
             "  if (tid == 5) atomic_compare_exchange_weak_explicit(&out[3], &expected, 99u, memory_order_relaxed, memory_order_relaxed);\n"
             "  threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);\n"
             "  if (tid == 0) { atomic_store_explicit(&out[0], atomic_load_explicit(&tg, memory_order_relaxed), memory_order_relaxed); }\n"
             "}\n", @"k", nil, 16, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s tg_sum=%u(exp 26) max=%u(exp 16) cas=%u(exp 99)\n", "atomics_tg", o[0], o[2], o[3]);
            });

        // 6. Function constants specialization
        NSString *fcSrc =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "constant uint K_MODE [[function_constant(0)]];\n"
             "kernel void k(device uint *out [[buffer(0)]]) { out[0] = K_MODE * 100 + 7; }\n";
        MTLFunctionConstantValues *f1 = [MTLFunctionConstantValues new];
        uint32_t v1 = 3; [f1 setConstantValue:&v1 type:MTLDataTypeUInt atIndex:0];
        probeComputeCase(dev, q, @"func_const_3", fcSrc, @"k", f1, 1, ^(const uint32_t *o, NSUInteger tew) {
            (void)tew;
            printf("PROBE_STATUS %-28s out0=%u(exp 307)\n", "func_const_3", o[0]);
        });
        MTLFunctionConstantValues *f2 = [MTLFunctionConstantValues new];
        uint32_t v2 = 9; [f2 setConstantValue:&v2 type:MTLDataTypeUInt atIndex:0];
        probeComputeCase(dev, q, @"func_const_9", fcSrc, @"k", f2, 1, ^(const uint32_t *o, NSUInteger tew) {
            (void)tew;
            printf("PROBE_STATUS %-28s out0=%u(exp 907)\n", "func_const_9", o[0]);
        });

        // 7. 64-bit / 16-bit / bit-ops
        probeComputeCase(dev, q, @"int64_bitops",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *out [[buffer(0)]]) {\n"
             "  uint64_t x = 0x123456789ABCDEF0ULL;\n"
             "  uint64_t y = x + 0x10ULL;\n"
             "  out[0] = uint(y); out[1] = uint(y >> 32);\n"
             "  out[2] = popcount(0xF0F0u);\n"
             "  out[3] = clz(0x00100000u);\n"
             "  out[4] = ctz(0x00100000u);\n"
             "  out[5] = reverse_bits(0x80000001u);\n"
             "  out[6] = extract_bits(0xABCDEF12u, 8u, 8u);\n"
             "  out[7] = insert_bits(0u, 0xABu, 8u, 8u);\n"
             "}\n", @"k", nil, 1, ^(const uint32_t *o, NSUInteger tew) {
                (void)tew;
                printf("PROBE_STATUS %-28s lo=0x%x hi=0x%x pop=%u clz=%u ctz=%u rev=0x%x ext=0x%x ins=0x%x\n",
                       "int64_bitops", o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7]);
            });

        // 8. Render: [[clip_distance]], [[sample_id]], [[sample_mask]], [[front_facing]]
        probeRenderCase(dev, q, @"clip_dist_front_facing",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct V { float4 pos [[position]]; float cd [[clip_distance]]; };\n"
             "vertex V vs(uint v [[vertex_id]]) {\n"
             "  float2 p[6]={{-1,-1},{3,-1},{-1,3}, {-1,-1},{3,-1},{-1,3}};\n"
             "  V o; o.pos=float4(p[v],0,1); o.cd = (v < 3) ? 1.0 : -1.0; return o;\n"
             "}\n"
             "fragment float4 fs(bool ff [[front_facing]]) { return ff ? float4(0,1,0,1) : float4(1,0,0,1); }\n",
            @"vs", @"fs", 1);

        probeRenderCase(dev, q, @"sample_id_and_mask",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct V { float4 pos [[position]]; };\n"
             "vertex V vs(uint v [[vertex_id]]) { float2 p[3]={{-1,-1},{3,-1},{-1,3}}; V o; o.pos=float4(p[v],0,1); return o; }\n"
             "struct FOut { float4 col [[color(0)]]; uint sm [[sample_mask]]; };\n"
             "fragment FOut fs(uint sid [[sample_id]], uint inMask [[sample_mask]]) {\n"
             "  FOut o; o.col = (sid < 2) ? float4(1,0,0,1) : float4(0,1,0,1); o.sm = inMask; return o;\n"
             "}\n",
            @"vs", @"fs", 4);

        // 9. Render: [[raster_order_group(0)]] overlapping-primitive RMW + void fragment
        probeRenderCase(dev, q, @"raster_order_group",
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct V { float4 pos [[position]]; };\n"
             "vertex V vs(uint v [[vertex_id]]) { float2 p[3]={{-1,-1},{3,-1},{-1,3}}; V o; o.pos=float4(p[v],0,1); return o; }\n"
             "fragment float4 fs(device uint *buf [[buffer(0), raster_order_group(0)]]) {\n"
             "  buf[0] += 1u; return float4(0,1,0,1);\n"
             "}\n",
            @"vs", @"fs", 1);
        {
            MTLCompileOptions *ropts = [MTLCompileOptions new];
            ropts.languageVersion = MTLLanguageVersion2_1;
            NSError *rerr = nil;
            NSString *rogSrc =
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "struct V { float4 pos [[position]]; };\n"
                 "vertex V vs_rog2(uint v [[vertex_id]]) { float2 p[3]={{-1,-1},{3,-1},{-1,3}}; V o; o.pos=float4(p[v],0,1); return o; }\n"
                 "fragment float4 fs_rog2(device uint *buf [[buffer(0), raster_order_group(0)]], float4 pos [[position]]) {\n"
                 "  uint idx = uint(pos.y) * 4u + uint(pos.x);\n"
                 "  uint old = buf[idx];\n"
                 "  buf[idx] = old + 1u;\n"
                 "  return (old == 0u) ? float4(1,0,0,1) : float4(0,1,0,1);\n"
                 "}\n"
                 "fragment void fs_rog_void(device uint *buf [[buffer(0), raster_order_group(0)]]) {\n"
                 "  buf[16] += 1u;\n"
                 "}\n";
            id<MTLLibrary> rlib = [dev newLibraryWithSource:rogSrc options:ropts error:&rerr];
            if (rlib) {
                MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
                rpd.vertexFunction = [rlib newFunctionWithName:@"vs_rog2"];
                rpd.fragmentFunction = [rlib newFunctionWithName:@"fs_rog2"];
                rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&rerr];
                MTLRenderPipelineDescriptor *vpd = [MTLRenderPipelineDescriptor new];
                vpd.vertexFunction = [rlib newFunctionWithName:@"vs_rog2"];
                vpd.fragmentFunction = [rlib newFunctionWithName:@"fs_rog_void"];
                vpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                id<MTLRenderPipelineState> vpso = [dev newRenderPipelineStateWithDescriptor:vpd error:&rerr];
                if (rpso && vpso) {
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
                    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
                    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
                    id<MTLBuffer> storage = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
                    memset(storage.contents, 0, 256);
                    id<MTLCommandBuffer> rcb = [q commandBuffer];
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> enc = [rcb renderCommandEncoderWithDescriptor:rp];
                    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
                    [enc setRenderPipelineState:rpso];
                    [enc setFragmentBuffer:storage offset:0 atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [enc setRenderPipelineState:vpso];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [enc endEncoding];
                    BOOL rok = NO; finish(rcb, @"rog_overlap_void", &rok);
                    const uint32_t *st = storage.contents;
                    printf("PROBE_STATUS %-28s %s count0=%u(exp 2) void16=%u(exp 16)\n",
                           "rog_overlap_and_void", rok && st[0] == 2 && st[16] == 16 ? "PASS" : "FAIL", st[0], st[16]);
                } else {
                    printf("PROBE_STATUS %-28s TRANSLATE_OR_PSO_FAIL %s\n", "rog_overlap_and_void", rerr.localizedDescription.UTF8String);
                }
            }
        }

        printf("MSL_PROBE_DONE\n");
        return 0;
    }
}
