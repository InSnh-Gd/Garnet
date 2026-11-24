// Live macOS / RX 9060 XT test for Metal 3 Compute Indirect Command Buffers:
// CPU-encoded and GPU-encoded multi-pipeline compute ICBs (ConcurrentDispatch +
// ConcurrentDispatchThreads), per-slot setBarrier ordering, threadgroup memory,
// stageInRegion, indirect execution ranges, zero CPU readback, and negative paths.
//
// Build (macOS):
//   clang -fobjc-arc -framework Foundation -framework Metal -framework QuartzCore -o test-metal3-compute-icb src/garnet-metal/test-metal3-compute-icb.m
// Run:
//   sudo ./test-metal3-compute-icb /path/to/GarnetMetal.bundle
//   ./test-metal3-compute-icb --reference

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MTLAttributeStrideStatic
#define MTLAttributeStrideStatic ((NSUInteger)NSUIntegerMax)
#endif

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "COMPUTE_ICB_LIVE_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void passCase(NSString *name) {
    printf("COMPUTE_ICB_LIVE_CASE_PASS %s\n", name.UTF8String);
}

static uint64_t cpuReadbackCount(id<MTLIndirectCommandBuffer> icb) {
    SEL sel = sel_registerName("rdna4ICBCPUReadbackCount");
    if (![icb respondsToSelector:sel]) return 0;
    return ((uint64_t (*)(id, SEL))objc_msgSend)(icb, sel);
}

static void assertZeroReadback(id<MTLIndirectCommandBuffer> icb, BOOL isRef, NSString *where) {
    if (isRef) return;
    uint64_t c = cpuReadbackCount(icb);
    if (c != 0)
        fail([NSString stringWithFormat:@"%@ triggered %llu CPU ICB readbacks", where, (unsigned long long)c]);
}

static void finish(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ command buffer nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@", op, (unsigned long)cb.status, cb.error]);
}

static void expectCommandError(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ command buffer nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusError || !cb.error)
        fail([NSString stringWithFormat:@"%@ did not fail cleanly (status=%lu)", op, (unsigned long)cb.status]);
}

static id<MTLLibrary> compileMSL(id<MTLDevice> dev) {
    NSString *src = @""
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "\n"
        "kernel void k_mul_add(device const uint *inBuf [[buffer(0)]],\n"
        "                      device uint *outBuf [[buffer(1)]],\n"
        "                      uint gid [[thread_position_in_grid]],\n"
        "                      uint gridW [[threads_per_grid]]) {\n"
        "    outBuf[gid] = inBuf[gid] * 3u + 7u + (gridW & 0xffffu);\n"
        "}\n"
        "\n"
        "kernel void k_barrier_add(device const uint *inBuf [[buffer(0)]],\n"
        "                          device uint *outBuf [[buffer(1)]],\n"
        "                          uint gid [[thread_position_in_grid]]) {\n"
        "    outBuf[gid] = inBuf[gid] + 1000u + gid;\n"
        "}\n"
        "\n"
        "kernel void k_tg_pair_sum(device const uint *inBuf [[buffer(0)]],\n"
        "                          device uint *outBuf [[buffer(1)]],\n"
        "                          threadgroup uint *scratch [[threadgroup(0)]],\n"
        "                          uint gid [[thread_position_in_grid]],\n"
        "                          uint lid [[thread_position_in_threadgroup]],\n"
        "                          uint tgs [[threads_per_threadgroup]]) {\n"
        "    scratch[lid] = inBuf[gid];\n"
        "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
        "    uint partner = lid ^ 1u;\n"
        "    outBuf[gid] = scratch[lid] + (partner < tgs ? scratch[partner] : 0u);\n"
        "}\n"
        "\n"
        "struct ComputeICBArgs {\n"
        "    command_buffer icb [[id(0)]];\n"
        "    compute_pipeline_state psoA [[id(1)]];\n"
        "    compute_pipeline_state psoB [[id(2)]];\n"
        "    device const uint *inBuf [[id(3)]];\n"
        "    device uint *midBuf [[id(4)]];\n"
        "    device uint *auxBuf [[id(5)]];\n"
        "};\n"
        "\n"
        "kernel void k_gpu_encode_compute_icb(constant ComputeICBArgs &args [[buffer(0)]],\n"
        "                                     uint tid [[thread_position_in_grid]]) {\n"
        "    if (tid != 0u) return;\n"
        "    // Slot 0: psoA, ConcurrentDispatch 2x32 = 64 threads -> midBuf[0..63]\n"
        "    compute_command c0(args.icb, 0);\n"
        "    c0.reset();\n"
        "    c0.set_compute_pipeline_state(args.psoA);\n"
        "    c0.set_kernel_buffer(args.inBuf, 0);\n"
        "    c0.set_kernel_buffer(args.midBuf, 1);\n"
        "    c0.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
        "\n"
        "    // Slot 1: psoB + set_barrier(), non-multiple ConcurrentDispatchThreads(45, tg=32)\n"
        "    compute_command c1(args.icb, 1);\n"
        "    c1.reset();\n"
        "    c1.set_compute_pipeline_state(args.psoB);\n"
        "    c1.set_kernel_buffer(args.midBuf, 0);\n"
        "    c1.set_kernel_buffer(args.midBuf, 1);\n"
        "    c1.set_barrier();\n"
        "    c1.concurrent_dispatch_threads(uint3(45, 1, 1), uint3(32, 1, 1));\n"
        "\n"
        "    // Slot 2: explicit reset (no-op)\n"
        "    compute_command c2(args.icb, 2);\n"
        "    c2.reset();\n"
        "\n"
        "    // Slot 3: copy_command(c0), redirect output to auxBuf, dispatch 17 threads\n"
        "    compute_command c3(args.icb, 3);\n"
        "    c3.copy_command(c0);\n"
        "    c3.set_kernel_buffer(args.auxBuf, 1);\n"
        "    c3.concurrent_dispatch_threads(uint3(17, 1, 1), uint3(32, 1, 1));\n"
        "\n"
        "    // Slot 4: outside indirect range [0, +4); must never run when range.length == 4\n"
        "    compute_command c4(args.icb, 4);\n"
        "    c4.reset();\n"
        "    c4.set_compute_pipeline_state(args.psoB);\n"
        "    c4.set_kernel_buffer(args.auxBuf, 0);\n"
        "    c4.set_kernel_buffer(args.auxBuf, 1);\n"
        "    c4.set_barrier();\n"
        "    c4.concurrent_dispatch_threadgroups(uint3(1, 1, 1), uint3(32, 1, 1));\n"
        "}\n"
        "\n"
        "struct InheritICBArgs {\n"
        "    command_buffer icb [[id(0)]];\n"
        "};\n"
        "\n"
        "kernel void k_gpu_encode_inherit_icb(constant InheritICBArgs &args [[buffer(0)]],\n"
        "                                     uint tid [[thread_position_in_grid]]) {\n"
        "    if (tid != 0u) return;\n"
        "    compute_command c0(args.icb, 0);\n"
        "    c0.reset();\n"
        "    c0.concurrent_dispatch_threads(uint3(29, 1, 1), uint3(32, 1, 1));\n"
        "}\n"
        "\n"
        "kernel void k_write_range(device uint2 *rangeOut [[buffer(0)]],\n"
        "                          uint tid [[thread_position_in_grid]]) {\n"
        "    if (tid == 0u) rangeOut[0] = uint2(0u, 4u);\n"
        "}\n";

    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_0;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (!lib) fail([NSString stringWithFormat:@"newLibraryWithSource failed: %@", err]);
    return lib;
}

static id<MTLComputePipelineState> newComputePSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                                 NSString *name, BOOL supportICB) {
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) fail([NSString stringWithFormat:@"missing function %@", name]);
    MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
    desc.computeFunction = fn;
    desc.supportIndirectCommandBuffers = supportICB;
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithDescriptor:desc
                                                                         options:MTLPipelineOptionNone
                                                                      reflection:nil
                                                                           error:&err];
    if (!pso) fail([NSString stringWithFormat:@"newComputePipelineState(%@, icb=%d) failed: %@", name, supportICB, err]);
    if (pso.supportIndirectCommandBuffers != supportICB)
        fail([NSString stringWithFormat:@"%@ supportIndirectCommandBuffers mismatch", name]);
    return pso;
}

static void checkBuffersEqual(const uint32_t *got, const uint32_t *want, uint32_t count, NSString *tag) {
    for (uint32_t i = 0; i < count; i++) {
        if (got[i] != want[i]) {
            fail([NSString stringWithFormat:@"%@ [%u]: got %u (0x%x) expected %u (0x%x)",
                  tag, i, got[i], got[i], want[i], want[i]]);
        }
    }
}

static void testCPUEncodedComputeICB(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                                     id<MTLComputePipelineState> psoA,
                                     id<MTLComputePipelineState> psoB,
                                     id<MTLComputePipelineState> psoTG) {
    enum : uint32_t { N = 128u };
    id<MTLBuffer> bIn = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bMid = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bTG = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bPost = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    uint32_t *inP = bIn.contents, *midP = bMid.contents, *tgP = bTG.contents, *postP = bPost.contents;
    for (uint32_t i = 0; i < N; i++) {
        inP[i] = i + 5u;
        midP[i] = 0xdeadbeefu;
        tgP[i] = 0xfeedfaceu;
        postP[i] = 0x11111111u;
    }

    MTLIndirectCommandBufferDescriptor *desc = [MTLIndirectCommandBufferDescriptor new];
    desc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads;
    desc.inheritPipelineState = NO;
    desc.inheritBuffers = NO;
    desc.maxKernelBufferBindCount = 4;

    id<MTLIndirectCommandBuffer> icb = [dev newIndirectCommandBufferWithDescriptor:desc
                                                                   maxCommandCount:6
                                                                           options:MTLResourceStorageModeShared];
    if (!icb) fail(@"CPU compute ICB creation failed");

    /* Slot 0: psoA, ConcurrentDispatch (2 groups x 32 threads = 64 threads) */
    id<MTLIndirectComputeCommand> c0 = [icb indirectComputeCommandAtIndex:0];
    [c0 setComputePipelineState:psoA];
    [c0 setKernelBuffer:bIn offset:0 atIndex:0];
    [c0 setKernelBuffer:bMid offset:0 atIndex:1];
    [c0 concurrentDispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    /* Slot 1: psoB + setBarrier, non-multiple ConcurrentDispatchThreads (45 threads, tg=32) */
    id<MTLIndirectComputeCommand> c1 = [icb indirectComputeCommandAtIndex:1];
    [c1 setComputePipelineState:psoB];
    [c1 setKernelBuffer:bMid offset:0 atIndex:0];
    if ([c1 respondsToSelector:@selector(setKernelBuffer:offset:attributeStride:atIndex:)]) {
        [c1 setKernelBuffer:bMid offset:0 attributeStride:MTLAttributeStrideStatic atIndex:1];
    } else {
        [c1 setKernelBuffer:bMid offset:0 atIndex:1];
    }
    [c1 setBarrier];
    [c1 concurrentDispatchThreads:MTLSizeMake(45, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    /* Slot 2: psoTG + threadgroup memory (128 bytes for 32 uints) + stageInRegion + clearBarrier */
    id<MTLIndirectComputeCommand> c2 = [icb indirectComputeCommandAtIndex:2];
    [c2 setComputePipelineState:psoTG];
    [c2 setKernelBuffer:bIn offset:0 atIndex:0];
    [c2 setKernelBuffer:bTG offset:0 atIndex:1];
    [c2 setThreadgroupMemoryLength:32 * sizeof(uint32_t) atIndex:0];
    [c2 setStageInRegion:MTLRegionMake3D(0, 0, 0, 32, 1, 1)];
    [c2 setBarrier];
    [c2 clearBarrier];
    [c2 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    /* Verify CPU-side command inspection getters */
    SEL hasBarrierSel = sel_registerName("hasBarrier");
    if ([c1 respondsToSelector:hasBarrierSel] && !((BOOL (*)(id, SEL))objc_msgSend)(c1, hasBarrierSel))
        fail(@"c1 hasBarrier expected YES");
    if ([c2 respondsToSelector:hasBarrierSel] && ((BOOL (*)(id, SEL))objc_msgSend)(c2, hasBarrierSel))
        fail(@"c2 clearBarrier expected NO");
    SEL getTgLenSel = sel_registerName("getThreadgroupMemoryLengthAtIndex:");
    if ([c2 respondsToSelector:getTgLenSel] &&
        ((NSUInteger (*)(id, SEL, NSUInteger))objc_msgSend)(c2, getTgLenSel, 0) != 32 * sizeof(uint32_t))
        fail(@"c2 threadgroup memory length getter mismatch");

    /* Slot 3: encode then reset to verify resetWithRange clears it */
    id<MTLIndirectComputeCommand> c3 = [icb indirectComputeCommandAtIndex:3];
    [c3 setComputePipelineState:psoB];
    [c3 setKernelBuffer:bTG offset:0 atIndex:0];
    [c3 setKernelBuffer:bTG offset:0 atIndex:1];
    [c3 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [icb resetWithRange:NSMakeRange(3, 1)];

    /* Execute slots [0, +4) AND verify parent compute encoder state is preserved afterwards */
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:psoA];
    [ce setBuffer:bIn offset:0 atIndex:0];
    [ce setBuffer:bPost offset:0 atIndex:1];
    [ce executeCommandsInBuffer:icb withRange:NSMakeRange(0, 4)];
    /* Parent encoder must still have psoA + bIn + bPost bound */
    [ce dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    [ce endEncoding];
    finish(cb, @"cpu-compute-icb");

    uint32_t wantMid[N], wantTG[N], wantPost[N];
    for (uint32_t i = 0; i < N; i++) {
        if (i < 64u) {
            uint32_t v = inP[i] * 3u + 7u + 64u;
            if (i < 45u) v += 1000u + i;
            wantMid[i] = v;
        } else {
            wantMid[i] = 0xdeadbeefu;
        }
        wantTG[i] = (i < 32u) ? (inP[i] + inP[i ^ 1u]) : 0xfeedfaceu;
        wantPost[i] = (i < 16u) ? (inP[i] * 3u + 7u + 16u) : 0x11111111u;
    }
    checkBuffersEqual(midP, wantMid, N, @"cpu-mid");
    checkBuffersEqual(tgP, wantTG, N, @"cpu-tg");
    checkBuffersEqual(postP, wantPost, N, @"cpu-parent-preserved");
    passCase(@"cpu-encoded-multi-pipeline-barrier-tgmem-state-preserve");
}

static void testGPUEncodedComputeICB(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                                     id<MTLLibrary> lib,
                                     id<MTLComputePipelineState> psoA,
                                     id<MTLComputePipelineState> psoB,
                                     BOOL isRef) {
    id<MTLFunction> fnEnc = [lib newFunctionWithName:@"k_gpu_encode_compute_icb"];
    id<MTLFunction> fnInh = [lib newFunctionWithName:@"k_gpu_encode_inherit_icb"];
    id<MTLComputePipelineState> psoEnc = newComputePSO(dev, lib, @"k_gpu_encode_compute_icb", NO);
    id<MTLComputePipelineState> psoInh = newComputePSO(dev, lib, @"k_gpu_encode_inherit_icb", NO);
    id<MTLComputePipelineState> psoRng = newComputePSO(dev, lib, @"k_write_range", NO);

    enum : uint32_t { N = 128u };
    id<MTLBuffer> bIn = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bMid = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bAux = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bRng = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    uint32_t *inP = bIn.contents, *midP = bMid.contents, *auxP = bAux.contents;
    for (uint32_t i = 0; i < N; i++) {
        inP[i] = i + 11u;
        midP[i] = 0xdeadbeefu;
        auxP[i] = 0xcafebabeu;
    }

    MTLIndirectCommandBufferDescriptor *desc = [MTLIndirectCommandBufferDescriptor new];
    desc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads;
    desc.inheritPipelineState = NO;
    desc.inheritBuffers = NO;
    desc.maxKernelBufferBindCount = 4;
    id<MTLIndirectCommandBuffer> icb = [dev newIndirectCommandBufferWithDescriptor:desc
                                                                   maxCommandCount:6
                                                                           options:MTLResourceStorageModePrivate];
    if (!icb) fail(@"GPU compute ICB creation failed");

    id<MTLArgumentEncoder> ae = [fnEnc newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> argBuf = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:argBuf offset:0];
    [ae setIndirectCommandBuffer:icb atIndex:0];
    [ae setComputePipelineState:psoA atIndex:1];
    [ae setComputePipelineState:psoB atIndex:2];
    [ae setBuffer:bIn offset:0 atIndex:3];
    [ae setBuffer:bMid offset:0 atIndex:4];
    [ae setBuffer:bAux offset:0 atIndex:5];

    /* 1. GPU producer encodes ICB + indirect range {0, 4}; compute encoder executes via indirectBuffer */
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce useResource:icb usage:MTLResourceUsageWrite];
    [ce useResource:bIn usage:MTLResourceUsageRead];
    [ce useResource:bMid usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [ce useResource:bAux usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [ce setComputePipelineState:psoEnc];
    [ce setBuffer:argBuf offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce setComputePipelineState:psoRng];
    [ce setBuffer:bRng offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [ce executeCommandsInBuffer:icb indirectBuffer:bRng indirectBufferOffset:0];
    [ce endEncoding];
    finish(cb, @"gpu-compute-icb-indirect-range");
    assertZeroReadback(icb, isRef, @"gpu-compute-icb-indirect-range");

    uint32_t wantMid[N], wantAux[N];
    for (uint32_t i = 0; i < N; i++) {
        if (i < 64u) {
            uint32_t v = inP[i] * 3u + 7u + 64u;
            if (i < 45u) v += 1000u + i;
            wantMid[i] = v;
        } else {
            wantMid[i] = 0xdeadbeefu;
        }
        wantAux[i] = (i < 17u) ? (inP[i] * 3u + 7u + 17u) : 0xcafebabeu;
    }
    checkBuffersEqual(midP, wantMid, N, @"gpu-mid-indirect");
    checkBuffersEqual(auxP, wantAux, N, @"gpu-aux-indirect");
    passCase(@"gpu-encoded-multi-pipeline-barrier-copy-indirect-range-zero-readback");

    /* 2. Direct range [0, +4) re-execution on the same GPU-encoded ICB */
    for (uint32_t i = 0; i < N; i++) { midP[i] = 0xdeadbeefu; auxP[i] = 0xcafebabeu; }
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce2 = [cb2 computeCommandEncoder];
    [ce2 useResource:icb usage:MTLResourceUsageRead];
    [ce2 useResource:bIn usage:MTLResourceUsageRead];
    [ce2 useResource:bMid usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [ce2 useResource:bAux usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [ce2 executeCommandsInBuffer:icb withRange:NSMakeRange(0, 4)];
    [ce2 endEncoding];
    finish(cb2, @"gpu-compute-icb-direct-range");
    assertZeroReadback(icb, isRef, @"gpu-compute-icb-direct-range");
    checkBuffersEqual(midP, wantMid, N, @"gpu-mid-direct");
    checkBuffersEqual(auxP, wantAux, N, @"gpu-aux-direct");
    passCase(@"gpu-encoded-direct-range-zero-readback");

    /* 3. GPU-encoded compute ICB with inheritPipelineState=YES + inheritBuffers=YES */
    MTLIndirectCommandBufferDescriptor *inhDesc = [MTLIndirectCommandBufferDescriptor new];
    inhDesc.commandTypes = MTLIndirectCommandTypeConcurrentDispatchThreads;
    inhDesc.inheritPipelineState = YES;
    inhDesc.inheritBuffers = YES;
    id<MTLIndirectCommandBuffer> inhICB = [dev newIndirectCommandBufferWithDescriptor:inhDesc
                                                                      maxCommandCount:2
                                                                              options:MTLResourceStorageModePrivate];
    if (!inhICB) fail(@"GPU inherit compute ICB creation failed");

    id<MTLArgumentEncoder> iae = [fnInh newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> iargBuf = [dev newBufferWithLength:iae.encodedLength options:MTLResourceStorageModeShared];
    [iae setArgumentBuffer:iargBuf offset:0];
    [iae setIndirectCommandBuffer:inhICB atIndex:0];

    for (uint32_t i = 0; i < N; i++) auxP[i] = 0x77777777u;
    id<MTLCommandBuffer> cb3 = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce3 = [cb3 computeCommandEncoder];
    [ce3 useResource:inhICB usage:MTLResourceUsageWrite | MTLResourceUsageRead];
    [ce3 setComputePipelineState:psoInh];
    [ce3 setBuffer:iargBuf offset:0 atIndex:0];
    [ce3 dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce3 memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [ce3 setComputePipelineState:psoA];
    [ce3 setBuffer:bIn offset:0 atIndex:0];
    [ce3 setBuffer:bAux offset:0 atIndex:1];
    [ce3 executeCommandsInBuffer:inhICB withRange:NSMakeRange(0, 1)];
    [ce3 endEncoding];
    finish(cb3, @"gpu-compute-icb-inherit");
    assertZeroReadback(inhICB, isRef, @"gpu-compute-icb-inherit");

    uint32_t wantInh[N];
    for (uint32_t i = 0; i < N; i++)
        wantInh[i] = (i < 29u) ? (inP[i] * 3u + 7u + 29u) : 0x77777777u;
    checkBuffersEqual(auxP, wantInh, N, @"gpu-inherit");
    passCase(@"gpu-encoded-inherit-pipeline-and-buffers-zero-readback");
}

static void testComputeICBNegativePaths(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                                        id<MTLComputePipelineState> psoNoICB,
                                        id<MTLComputePipelineState> psoICB) {
    /* 1. Mixing render + compute commandTypes refused */
    MTLIndirectCommandBufferDescriptor *mix = [MTLIndirectCommandBufferDescriptor new];
    mix.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeConcurrentDispatch;
    if ([dev newIndirectCommandBufferWithDescriptor:mix maxCommandCount:4 options:MTLResourceStorageModeShared] != nil)
        fail(@"mixed render+compute ICB descriptor was not refused");

    /* 2. maxKernelBufferBindCount > 31 refused */
    MTLIndirectCommandBufferDescriptor *oobBinds = [MTLIndirectCommandBufferDescriptor new];
    oobBinds.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    oobBinds.inheritPipelineState = NO;
    oobBinds.inheritBuffers = NO;
    oobBinds.maxKernelBufferBindCount = 32;
    if ([dev newIndirectCommandBufferWithDescriptor:oobBinds maxCommandCount:4 options:MTLResourceStorageModeShared] != nil)
        fail(@"maxKernelBufferBindCount=32 was not refused");

    /* 3. Render ICB executed on compute encoder fails cleanly */
    MTLIndirectCommandBufferDescriptor *rDesc = [MTLIndirectCommandBufferDescriptor new];
    rDesc.commandTypes = MTLIndirectCommandTypeDraw;
    rDesc.inheritPipelineState = YES;
    rDesc.inheritBuffers = YES;
    id<MTLIndirectCommandBuffer> rICB = [dev newIndirectCommandBufferWithDescriptor:rDesc maxCommandCount:2 options:MTLResourceStorageModeShared];
    if (!rICB) fail(@"render ICB for mismatch negative test nil");
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce executeCommandsInBuffer:rICB withRange:NSMakeRange(0, 1)];
        [ce endEncoding];
        expectCommandError(cb, @"render-icb-on-compute-encoder");
    }

    /* 4. Compute ICB executed on render encoder fails cleanly */
    MTLIndirectCommandBufferDescriptor *cDesc = [MTLIndirectCommandBufferDescriptor new];
    cDesc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    cDesc.inheritPipelineState = NO;
    cDesc.inheritBuffers = NO;
    cDesc.maxKernelBufferBindCount = 2;
    id<MTLIndirectCommandBuffer> cICB = [dev newIndirectCommandBufferWithDescriptor:cDesc maxCommandCount:2 options:MTLResourceStorageModeShared];
    if (!cICB) fail(@"compute ICB for negative tests nil");
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re executeCommandsInBuffer:cICB withRange:NSMakeRange(0, 1)];
        [re endEncoding];
        expectCommandError(cb, @"compute-icb-on-render-encoder");
    }

    /* 5. Out-of-range execution fails cleanly */
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce executeCommandsInBuffer:cICB withRange:NSMakeRange(1, 3)];
        [ce endEncoding];
        expectCommandError(cb, @"compute-icb-out-of-range");
    }

    /* 6. Non-ICB compute pipeline rejected */
    {
        id<MTLIndirectComputeCommand> cmd = [cICB indirectComputeCommandAtIndex:0];
        [cmd reset];
        [cmd setComputePipelineState:psoNoICB];
        [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce executeCommandsInBuffer:cICB withRange:NSMakeRange(0, 1)];
        [ce endEncoding];
        expectCommandError(cb, @"compute-icb-non-icb-pso");
    }

    /* 7. Unaligned threadgroup memory length rejected */
    {
        id<MTLIndirectComputeCommand> cmd = [cICB indirectComputeCommandAtIndex:0];
        [cmd reset];
        [cmd setComputePipelineState:psoICB];
        [cmd setThreadgroupMemoryLength:12 atIndex:0];
        [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce executeCommandsInBuffer:cICB withRange:NSMakeRange(0, 1)];
        [ce endEncoding];
        expectCommandError(cb, @"compute-icb-unaligned-tgmem");
    }

    /* 8. TBDR imageblock dimensions rejected cleanly */
    {
        id<MTLIndirectComputeCommand> cmd = [cICB indirectComputeCommandAtIndex:0];
        [cmd reset];
        [cmd setComputePipelineState:psoICB];
        [cmd setImageblockWidth:16 height:16];
        [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce executeCommandsInBuffer:cICB withRange:NSMakeRange(0, 1)];
        [ce endEncoding];
        expectCommandError(cb, @"compute-icb-imageblock-unsupported");
    }

    passCase(@"compute-icb-negative-boundaries");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-metal3-compute-icb BUNDLE|--reference");
        return 0;
    }
    if (argc != 2) return 2;
    BOOL ref = !strcmp(argv[1], "--reference");
    if (!ref && geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        if (ref) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                fail(err.description);
            dev = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        if (!dev) fail(@"device nil");
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!queue) fail(@"queue nil");
        id<MTLLibrary> lib = compileMSL(dev);

        id<MTLComputePipelineState> psoA = newComputePSO(dev, lib, @"k_mul_add", YES);
        id<MTLComputePipelineState> psoB = newComputePSO(dev, lib, @"k_barrier_add", YES);
        id<MTLComputePipelineState> psoTG = newComputePSO(dev, lib, @"k_tg_pair_sum", YES);
        id<MTLComputePipelineState> psoNoICB = newComputePSO(dev, lib, @"k_mul_add", NO);

        testCPUEncodedComputeICB(dev, queue, psoA, psoB, psoTG);
        testGPUEncodedComputeICB(dev, queue, lib, psoA, psoB, ref);
        if (!ref) testComputeICBNegativePaths(dev, queue, psoNoICB, psoA);

        printf("COMPUTE_ICB_LIVE_SUITE_PASS cpu_readback=0\n");
    }
    return 0;
}
