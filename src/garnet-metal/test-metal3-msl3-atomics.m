// test-metal3-msl3-atomics.m: live RX 9060 XT / reference Metal verification for Task 2.6
// (MSL 2.2-3.2: device atomic<float> add/sub/exchange/load/store/CAS min+max,
// threadgroup atomic_uint/atomic_int, 64-bit integer atomics atomic_ulong/atomic<int64_t>
// via Int64Atomics, bfloat / bfloat4 FMA, math & conversions, storage-texture atomics on
// R32Uint/R32Sint, and simd_shuffle_and_fill_{up,down} with default and explicit modulo vs CPU oracle).
// Build on macOS:
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-msl3-atomics.m -o "$TMPDIR/test-metal3-msl3-atomics"
// Run:
//   "$TMPDIR/test-metal3-msl3-atomics" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-msl3-atomics" --reference
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

enum {
    kGuardBytes = 256,
    kGuardWords = kGuardBytes / 4,
};
static const uint32_t kHeadGuard = 0xDEADBEEFu;
static const uint32_t kTailGuard = 0xCAFEBABEu;

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_MSL3_ATOMICS_FAIL %s\n", msg.UTF8String);
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

static void initGuardedBuffer(id<MTLBuffer> buf, NSUInteger payloadBytes) {
    uint8_t *base = (uint8_t *)buf.contents;
    uint32_t *head = (uint32_t *)base;
    uint32_t *tail = (uint32_t *)(base + kGuardBytes + payloadBytes);
    for (int i = 0; i < kGuardWords; i++) {
        head[i] = kHeadGuard;
        tail[i] = kTailGuard;
    }
    memset(base + kGuardBytes, 0, payloadBytes);
}

static void checkGuards(id<MTLBuffer> buf, NSUInteger payloadBytes, NSString *tag) {
    const uint8_t *base = (const uint8_t *)buf.contents;
    const uint32_t *head = (const uint32_t *)base;
    const uint32_t *tail = (const uint32_t *)(base + kGuardBytes + payloadBytes);
    for (int i = 0; i < kGuardWords; i++) {
        require(head[i] == kHeadGuard,
                [NSString stringWithFormat:@"%@: head guard[%d] clobbered (0x%08x)", tag, i, head[i]]);
        require(tail[i] == kTailGuard,
                [NSString stringWithFormat:@"%@: tail guard[%d] clobbered (0x%08x)", tag, i, tail[i]]);
    }
}

static uint16_t f32_to_bf16(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof u);
    if ((u & 0x7FFFFFFFu) > 0x7F800000u) return (uint16_t)((u >> 16) | 0x0040u); // quiet NaN
    // IEEE-754 round-to-nearest-even from binary32 to bfloat16.
    uint32_t lsb = (u >> 16) & 1u;
    uint32_t rounded = u + 0x7FFFu + lsb;
    return (uint16_t)(rounded >> 16);
}

static float bf16_to_f32(uint16_t b) {
    uint32_t u = (uint32_t)b << 16;
    float f = 0.0f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint32_t bf16_ulp_dist(uint16_t a, uint16_t b) {
    if (a == b) return 0u;
    int32_t sa = (a & 0x8000u) ? -(int32_t)(a & 0x7FFFu) : (int32_t)(a & 0x7FFFu);
    int32_t sb = (b & 0x8000u) ? -(int32_t)(b & 0x7FFFu) : (int32_t)(b & 0x7FFFu);
    int32_t d = sa - sb;
    return (uint32_t)(d < 0 ? -d : d);
}

static id<MTLComputePipelineState> buildComputePSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                                   NSString *fnName, NSString *tag) {
    id<MTLFunction> fn = [lib newFunctionWithName:fnName];
    require(fn != nil, [NSString stringWithFormat:@"%@: function %@ missing", tag, fnName]);
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    require(pso != nil, [NSString stringWithFormat:@"%@: pipeline %@ failed: %@", tag, fnName, err]);
    return pso;
}

static void runFloatAtomics(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "static inline void atomic_max_f32_dev(device atomic_float *p, float v) {\n"
         "    float cur = atomic_load_explicit(p, memory_order_relaxed);\n"
         "    while (v > cur && !atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed, memory_order_relaxed)) {}\n"
         "}\n"
         "static inline void atomic_min_f32_dev(device atomic_float *p, float v) {\n"
         "    float cur = atomic_load_explicit(p, memory_order_relaxed);\n"
         "    while (v < cur && !atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed, memory_order_relaxed)) {}\n"
         "}\n"
         "kernel void k_float_atomics(device atomic_float *devSlots [[buffer(0)]],\n"
         "                            device float *xchgOld [[buffer(1)]],\n"
         "                            device uint *tgOutU [[buffer(2)]],\n"
         "                            device int *tgOutS [[buffer(3)]],\n"
         "                            uint gid [[thread_position_in_grid]],\n"
         "                            uint lid [[thread_position_in_threadgroup]],\n"
         "                            uint tgid [[threadgroup_position_in_grid]]) {\n"
         "    threadgroup atomic_uint tgU[4];\n"
         "    threadgroup atomic_int tgS[4];\n"
         "    if (lid == 0u) {\n"
         "        atomic_store_explicit(&tgU[0], 0u, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgU[1], 5000u, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgU[2], 0u, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgU[3], 0xFFFFFFFFu, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgS[0], 0, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgS[1], 1000, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgS[2], -1000, memory_order_relaxed);\n"
         "        atomic_store_explicit(&tgS[3], 1000, memory_order_relaxed);\n"
         "    }\n"
         "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
         "\n"
         "    // Contended device atomic<float> ops across 256 threads (MSL 3.0 atomic<float> is device-only).\n"
         "    atomic_fetch_add_explicit(&devSlots[0], float(gid + 1u), memory_order_relaxed);\n"
         "    atomic_fetch_sub_explicit(&devSlots[1], 0.5f * float(gid + 1u), memory_order_relaxed);\n"
         "    xchgOld[gid] = atomic_exchange_explicit(&devSlots[2], float(gid + 1001u), memory_order_relaxed);\n"
         "    float cand = float(int(gid) - 120) * 0.25f;\n"
         "    atomic_max_f32_dev(&devSlots[3], cand);\n"
         "    atomic_min_f32_dev(&devSlots[4], cand);\n"
         "\n"
         "    // Contended threadgroup atomic_uint / atomic_int ops across 64 threads per group.\n"
         "    atomic_fetch_add_explicit(&tgU[0], lid + 1u, memory_order_relaxed);\n"
         "    atomic_fetch_sub_explicit(&tgU[1], lid + 1u, memory_order_relaxed);\n"
         "    atomic_fetch_max_explicit(&tgU[2], lid * 3u + 5u, memory_order_relaxed);\n"
         "    atomic_fetch_min_explicit(&tgU[3], 500u - lid * 5u, memory_order_relaxed);\n"
         "    atomic_fetch_add_explicit(&tgS[0], int(lid) - 30, memory_order_relaxed);\n"
         "    atomic_fetch_sub_explicit(&tgS[1], int(lid) - 20, memory_order_relaxed);\n"
         "    atomic_fetch_max_explicit(&tgS[2], int(lid) * 2 - 50, memory_order_relaxed);\n"
         "    atomic_fetch_min_explicit(&tgS[3], int(lid) * 2 - 50, memory_order_relaxed);\n"
         "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
         "    if (lid == 0u) {\n"
         "        uint sumU = atomic_load_explicit(&tgU[0], memory_order_relaxed);\n"
         "        tgOutU[tgid * 4u + 0u] = sumU;\n"
         "        tgOutU[tgid * 4u + 1u] = atomic_load_explicit(&tgU[1], memory_order_relaxed);\n"
         "        tgOutU[tgid * 4u + 2u] = atomic_load_explicit(&tgU[2], memory_order_relaxed);\n"
         "        tgOutU[tgid * 4u + 3u] = atomic_load_explicit(&tgU[3], memory_order_relaxed);\n"
         "        tgOutS[tgid * 4u + 0u] = atomic_load_explicit(&tgS[0], memory_order_relaxed);\n"
         "        tgOutS[tgid * 4u + 1u] = atomic_load_explicit(&tgS[1], memory_order_relaxed);\n"
         "        tgOutS[tgid * 4u + 2u] = atomic_load_explicit(&tgS[2], memory_order_relaxed);\n"
         "        tgOutS[tgid * 4u + 3u] = atomic_load_explicit(&tgS[3], memory_order_relaxed);\n"
         "        if (tgid == 0u) {\n"
         "            float base = atomic_load_explicit(&devSlots[6], memory_order_relaxed);\n"
         "            atomic_store_explicit(&devSlots[5], base + float(sumU), memory_order_relaxed);\n"
         "        }\n"
         "    }\n"
         "}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    require(lib != nil, [NSString stringWithFormat:@"float_atomics compile: %@", err]);
    id<MTLComputePipelineState> pso = buildComputePSO(dev, lib, @"k_float_atomics", @"float_atomics");

    const NSUInteger kThreads = 256;
    const NSUInteger kTgSize = 64;
    const NSUInteger kNumTg = kThreads / kTgSize;
    const NSUInteger devBytes = 8 * sizeof(float);
    const NSUInteger xchgBytes = kThreads * sizeof(float);
    const NSUInteger tgUBytes = kNumTg * 4 * sizeof(uint32_t);
    const NSUInteger tgSBytes = kNumTg * 4 * sizeof(int32_t);

    id<MTLBuffer> devBuf = [dev newBufferWithLength:devBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> xchgBuf = [dev newBufferWithLength:xchgBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> tgBufU = [dev newBufferWithLength:tgUBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> tgBufS = [dev newBufferWithLength:tgSBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(devBuf && xchgBuf && tgBufU && tgBufS, @"float_atomics buffers nil");
    initGuardedBuffer(devBuf, devBytes);
    initGuardedBuffer(xchgBuf, xchgBytes);
    initGuardedBuffer(tgBufU, tgUBytes);
    initGuardedBuffer(tgBufS, tgSBytes);

    float *initDev = (float *)((uint8_t *)devBuf.contents + kGuardBytes);
    initDev[0] = 0.0f;
    initDev[1] = 50000.0f;
    initDev[2] = 1000.0f;
    initDev[3] = -1000.0f;
    initDev[4] = 1000.0f;
    initDev[5] = -999.0f;
    initDev[6] = 42.5f;

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    require(ce != nil, @"float_atomics encoder nil");
    [ce setComputePipelineState:pso];
    [ce setBuffer:devBuf offset:kGuardBytes atIndex:0];
    [ce setBuffer:xchgBuf offset:kGuardBytes atIndex:1];
    [ce setBuffer:tgBufU offset:kGuardBytes atIndex:2];
    [ce setBuffer:tgBufS offset:kGuardBytes atIndex:3];
    [ce dispatchThreadgroups:MTLSizeMake(kNumTg, 1, 1) threadsPerThreadgroup:MTLSizeMake(kTgSize, 1, 1)];
    [ce endEncoding];
    finishOK(cb, @"float_atomics");

    checkGuards(devBuf, devBytes, @"float_atomics dev");
    checkGuards(xchgBuf, xchgBytes, @"float_atomics xchg");
    checkGuards(tgBufU, tgUBytes, @"float_atomics tgU");
    checkGuards(tgBufS, tgSBytes, @"float_atomics tgS");

    const float *gotDev = (const float *)((const uint8_t *)devBuf.contents + kGuardBytes);
    const float *gotXchg = (const float *)((const uint8_t *)xchgBuf.contents + kGuardBytes);
    const uint32_t *gotTgU = (const uint32_t *)((const uint8_t *)tgBufU.contents + kGuardBytes);
    const int32_t *gotTgS = (const int32_t *)((const uint8_t *)tgBufS.contents + kGuardBytes);

    require(gotDev[0] == 32896.0f, [NSString stringWithFormat:@"float dev add=%g exp=32896", gotDev[0]]);
    require(gotDev[1] == 33552.0f, [NSString stringWithFormat:@"float dev sub=%g exp=33552", gotDev[1]]);
    require(gotDev[3] == 33.75f, [NSString stringWithFormat:@"float dev max=%g exp=33.75", gotDev[3]]);
    require(gotDev[4] == -30.0f, [NSString stringWithFormat:@"float dev min=%g exp=-30", gotDev[4]]);
    require(gotDev[5] == 2122.5f && gotDev[6] == 42.5f,
            [NSString stringWithFormat:@"float dev load/store=(%g,%g) exp=(2122.5,42.5)", gotDev[5], gotDev[6]]);

    // Every value in {1000..1256} must appear exactly once across xchgOld[0..255] + final devSlots[2].
    uint8_t seen[257] = {0};
    for (NSUInteger i = 0; i < kThreads; i++) {
        int v = (int)gotXchg[i];
        require((float)v == gotXchg[i] && v >= 1000 && v <= 1256,
                [NSString stringWithFormat:@"float xchg[%lu]=%g out of range", (unsigned long)i, gotXchg[i]]);
        seen[v - 1000]++;
    }
    int fin = (int)gotDev[2];
    require((float)fin == gotDev[2] && fin >= 1000 && fin <= 1256,
            [NSString stringWithFormat:@"float xchg final=%g out of range", gotDev[2]]);
    seen[fin - 1000]++;
    for (int k = 0; k <= 256; k++) {
        require(seen[k] == 1u, [NSString stringWithFormat:@"float xchg value %d count=%u != 1", 1000 + k, seen[k]]);
    }

    for (NSUInteger g = 0; g < kNumTg; g++) {
        require(gotTgU[g * 4 + 0] == 2080u && gotTgU[g * 4 + 1] == 2920u &&
                gotTgU[g * 4 + 2] == 194u && gotTgU[g * 4 + 3] == 185u,
                [NSString stringWithFormat:@"tgU[%lu]=(%u,%u,%u,%u) exp=(2080,2920,194,185)",
                 (unsigned long)g, gotTgU[g * 4 + 0], gotTgU[g * 4 + 1], gotTgU[g * 4 + 2], gotTgU[g * 4 + 3]]);
        require(gotTgS[g * 4 + 0] == 96 && gotTgS[g * 4 + 1] == 264 &&
                gotTgS[g * 4 + 2] == 76 && gotTgS[g * 4 + 3] == -50,
                [NSString stringWithFormat:@"tgS[%lu]=(%d,%d,%d,%d) exp=(96,264,76,-50)",
                 (unsigned long)g, gotTgS[g * 4 + 0], gotTgS[g * 4 + 1], gotTgS[g * 4 + 2], gotTgS[g * 4 + 3]]);
    }
    puts("METAL3_MSL3_ATOMICS_CASE_PASS atomic_float");
}

static void runInt64Atomics(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    // Subcase 2a (always required, MSL 2.4+): device atomic_ulong min/max with 64-bit values > 1<<40.
    NSString *srcMinMax =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void k_u64_minmax(device atomic_ulong *slots [[buffer(0)]],\n"
         "                         uint gid [[thread_position_in_grid]]) {\n"
         "    ulong hi = ulong((gid * 37u + 13u) & 255u) << 36;\n"
         "    ulong v = hi | (ulong(gid ^ 0x5Au) << 8) | ulong(gid);\n"
         "    atomic_min_explicit(&slots[0], v, memory_order_relaxed);\n"
         "    atomic_max_explicit(&slots[1], v, memory_order_relaxed);\n"
         "    ulong sameHi = 0x0000123400000000UL | ulong((gid * 97u + 7u) & 0xFFFFu);\n"
         "    atomic_min_explicit(&slots[2], sameHi, memory_order_relaxed);\n"
         "    atomic_max_explicit(&slots[3], sameHi, memory_order_relaxed);\n"
         "}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:srcMinMax options:nil error:&err];
    require(lib != nil, [NSString stringWithFormat:@"int64_minmax compile: %@", err]);
    id<MTLComputePipelineState> pso = buildComputePSO(dev, lib, @"k_u64_minmax", @"int64_minmax");

    const NSUInteger kThreads = 256;
    const NSUInteger bytes = 4 * sizeof(uint64_t);
    id<MTLBuffer> buf = [dev newBufferWithLength:bytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(buf != nil, @"int64_minmax buffer nil");
    initGuardedBuffer(buf, bytes);
    uint64_t *slots = (uint64_t *)((uint8_t *)buf.contents + kGuardBytes);
    slots[0] = UINT64_MAX;
    slots[1] = 0ULL;
    slots[2] = 0x00001234FFFFFFFFULL;
    slots[3] = 0x0000123400000000ULL;

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    require(ce != nil, @"int64_minmax encoder nil");
    [ce setComputePipelineState:pso];
    [ce setBuffer:buf offset:kGuardBytes atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce endEncoding];
    finishOK(cb, @"int64_minmax");
    checkGuards(buf, bytes, @"int64_minmax");

    uint64_t expMin0 = UINT64_MAX, expMax1 = 0ULL;
    uint64_t expMin2 = 0x00001234FFFFFFFFULL, expMax3 = 0x0000123400000000ULL;
    for (uint32_t gid = 0; gid < kThreads; gid++) {
        uint64_t v = ((uint64_t)((gid * 37u + 13u) & 255u) << 36) | ((uint64_t)(gid ^ 0x5Au) << 8) | (uint64_t)gid;
        if (v < expMin0) expMin0 = v;
        if (v > expMax1) expMax1 = v;
        uint64_t sameHi = 0x0000123400000000ULL | (uint64_t)((gid * 97u + 7u) & 0xFFFFu);
        if (sameHi < expMin2) expMin2 = sameHi;
        if (sameHi > expMax3) expMax3 = sameHi;
    }
    require(slots[0] == expMin0 && slots[1] == expMax1 && slots[2] == expMin2 && slots[3] == expMax3,
            [NSString stringWithFormat:@"u64 minmax got=(0x%llx,0x%llx,0x%llx,0x%llx) exp=(0x%llx,0x%llx,0x%llx,0x%llx)",
             (unsigned long long)slots[0], (unsigned long long)slots[1],
             (unsigned long long)slots[2], (unsigned long long)slots[3],
             (unsigned long long)expMin0, (unsigned long long)expMax1,
             (unsigned long long)expMin2, (unsigned long long)expMax3]);
    puts("METAL3_MSL3_ATOMICS_CASE_PASS atomic_u64_minmax");

    // Subcase 2b (extended 64-bit add/sub/bitwise/CAS/signed min+max if host MSL stdlib exposes them).
    NSString *srcExt =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void k_i64_ext(device atomic<uint64_t> *uSlots [[buffer(0)]],\n"
         "                      device atomic<int64_t> *sSlots [[buffer(1)]],\n"
         "                      uint gid [[thread_position_in_grid]]) {\n"
         "    uint64_t big = (1ULL << 34) + uint64_t(gid + 1u);\n"
         "    atomic_fetch_add_explicit(&uSlots[0], big, memory_order_relaxed);\n"
         "    atomic_fetch_sub_explicit(&uSlots[1], uint64_t(gid + 1u), memory_order_relaxed);\n"
         "    atomic_fetch_or_explicit(&uSlots[2], 1ULL << (gid & 63u), memory_order_relaxed);\n"
         "    atomic_fetch_and_explicit(&uSlots[3], ~(1ULL << (gid & 31u)), memory_order_relaxed);\n"
         "    atomic_fetch_xor_explicit(&uSlots[4], (1ULL << 40) | uint64_t(gid), memory_order_relaxed);\n"
         "    int64_t sv = -(1LL << 35) + int64_t(gid) * 1000LL;\n"
         "    atomic_fetch_min_explicit(&sSlots[0], sv, memory_order_relaxed);\n"
         "    atomic_fetch_max_explicit(&sSlots[1], sv, memory_order_relaxed);\n"
         "}\n";
    NSError *extErr = nil;
    id<MTLLibrary> extLib = [dev newLibraryWithSource:srcExt options:nil error:&extErr];
    if (!extLib) {
        printf("METAL3_MSL3_ATOMICS_NOTE extended_i64_stdlib_unavailable (%s)\n",
               extErr ? extErr.localizedDescription.UTF8String : "nil");
        return;
    }
    id<MTLComputePipelineState> extPso = buildComputePSO(dev, extLib, @"k_i64_ext", @"int64_ext");
    const NSUInteger uBytes = 8 * sizeof(uint64_t);
    const NSUInteger sBytes = 4 * sizeof(int64_t);
    id<MTLBuffer> uBuf = [dev newBufferWithLength:uBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> sBuf = [dev newBufferWithLength:sBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(uBuf && sBuf, @"int64_ext buffers nil");
    initGuardedBuffer(uBuf, uBytes);
    initGuardedBuffer(sBuf, sBytes);
    uint64_t *uSlots = (uint64_t *)((uint8_t *)uBuf.contents + kGuardBytes);
    int64_t *sSlots = (int64_t *)((uint8_t *)sBuf.contents + kGuardBytes);
    uSlots[0] = 0ULL;
    uSlots[1] = (1ULL << 40);
    uSlots[2] = 0ULL;
    uSlots[3] = UINT64_MAX;
    uSlots[4] = 0ULL;
    sSlots[0] = INT64_MAX;
    sSlots[1] = INT64_MIN;

    id<MTLCommandBuffer> cb2 = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce2 = [cb2 computeCommandEncoder];
    [ce2 setComputePipelineState:extPso];
    [ce2 setBuffer:uBuf offset:kGuardBytes atIndex:0];
    [ce2 setBuffer:sBuf offset:kGuardBytes atIndex:1];
    [ce2 dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce2 endEncoding];
    finishOK(cb2, @"int64_ext");
    checkGuards(uBuf, uBytes, @"int64_ext u");
    checkGuards(sBuf, sBytes, @"int64_ext s");

    uint64_t expAdd = 0, expSub = (1ULL << 40), expOr = 0, expAnd = UINT64_MAX, expXor = 0;
    int64_t expSMin = INT64_MAX, expSMax = INT64_MIN;
    for (uint32_t gid = 0; gid < kThreads; gid++) {
        expAdd += (1ULL << 34) + (uint64_t)(gid + 1u);
        expSub -= (uint64_t)(gid + 1u);
        expOr |= (1ULL << (gid & 63u));
        expAnd &= ~(1ULL << (gid & 31u));
        expXor ^= ((1ULL << 40) | (uint64_t)gid);
        int64_t sv = -(1LL << 35) + (int64_t)gid * 1000LL;
        if (sv < expSMin) expSMin = sv;
        if (sv > expSMax) expSMax = sv;
    }
    require(uSlots[0] == expAdd && uSlots[1] == expSub && uSlots[2] == expOr &&
            uSlots[3] == expAnd && uSlots[4] == expXor &&
            sSlots[0] == expSMin && sSlots[1] == expSMax,
            @"int64_ext oracle mismatch");
    puts("METAL3_MSL3_ATOMICS_CASE_PASS atomic_i64_ext");
}

static void runBfloat(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void k_bfloat(const device bfloat4 *inA [[buffer(0)]],\n"
         "                     const device bfloat4 *inB [[buffer(1)]],\n"
         "                     const device bfloat4 *inC [[buffer(2)]],\n"
         "                     device bfloat4 *outFma [[buffer(3)]],\n"
         "                     device ushort4 *outAux [[buffer(4)]],\n"
         "                     device uint *outUint [[buffer(5)]],\n"
         "                     uint i [[thread_position_in_grid]]) {\n"
         "    bfloat4 a = inA[i];\n"
         "    bfloat4 b = inB[i];\n"
         "    bfloat4 c = inC[i];\n"
         "    bfloat4 f = a * b + c;\n"
         "    outFma[i] = f;\n"
         "    half h = half(f.x);\n"
         "    bfloat back = bfloat(h);\n"
         "    bfloat e = bfloat(exp(clamp(float(a.x), -2.0f, 2.0f)));\n"
         "    bfloat sat = bfloat(saturate(float(a.y)));\n"
         "    bfloat fi = bfloat(int(i) - 32);\n"
         "    outAux[i] = ushort4(as_type<ushort>(back), as_type<ushort>(e), as_type<ushort>(sat), as_type<ushort>(fi));\n"
         "    outUint[i] = uint(bfloat(float(i + 5u)));\n"
         "}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    require(lib != nil, [NSString stringWithFormat:@"bfloat compile: %@", err]);
    id<MTLComputePipelineState> pso = buildComputePSO(dev, lib, @"k_bfloat", @"bfloat");

    const NSUInteger kN = 64;
    const NSUInteger vecBytes = kN * 4 * sizeof(uint16_t);
    const NSUInteger uBytes = kN * sizeof(uint32_t);
    id<MTLBuffer> bufA = [dev newBufferWithLength:vecBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufB = [dev newBufferWithLength:vecBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufC = [dev newBufferWithLength:vecBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outFma = [dev newBufferWithLength:vecBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outAux = [dev newBufferWithLength:vecBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outU = [dev newBufferWithLength:uBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(bufA && bufB && bufC && outFma && outAux && outU, @"bfloat buffers nil");
    initGuardedBuffer(outFma, vecBytes);
    initGuardedBuffer(outAux, vecBytes);
    initGuardedBuffer(outU, uBytes);

    uint16_t *pA = (uint16_t *)bufA.contents;
    uint16_t *pB = (uint16_t *)bufB.contents;
    uint16_t *pC = (uint16_t *)bufC.contents;
    for (NSUInteger i = 0; i < kN; i++) {
        float t = ((float)i - 32.0f) * 0.125f;
        float va[4] = { t * 0.5f, (float)i * 0.04f - 0.5f, 1.5f + (float)(i & 7u) * 0.25f, -2.0f + (float)(i & 15u) * 0.25f };
        float vb[4] = { 1.25f, -0.75f, 2.0f, 0.5f };
        float vc[4] = { 0.5f, 1.0f, -0.25f, 3.0f };
        for (int c = 0; c < 4; c++) {
            pA[i * 4 + c] = f32_to_bf16(va[c]);
            pB[i * 4 + c] = f32_to_bf16(vb[c]);
            pC[i * 4 + c] = f32_to_bf16(vc[c]);
        }
    }

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setBuffer:bufA offset:0 atIndex:0];
    [ce setBuffer:bufB offset:0 atIndex:1];
    [ce setBuffer:bufC offset:0 atIndex:2];
    [ce setBuffer:outFma offset:kGuardBytes atIndex:3];
    [ce setBuffer:outAux offset:kGuardBytes atIndex:4];
    [ce setBuffer:outU offset:kGuardBytes atIndex:5];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kN, 1, 1)];
    [ce endEncoding];
    finishOK(cb, @"bfloat");

    checkGuards(outFma, vecBytes, @"bfloat fma");
    checkGuards(outAux, vecBytes, @"bfloat aux");
    checkGuards(outU, uBytes, @"bfloat uint");

    const uint16_t *gotFma = (const uint16_t *)((const uint8_t *)outFma.contents + kGuardBytes);
    const uint16_t *gotAux = (const uint16_t *)((const uint8_t *)outAux.contents + kGuardBytes);
    const uint32_t *gotU = (const uint32_t *)((const uint8_t *)outU.contents + kGuardBytes);

    for (NSUInteger i = 0; i < kN; i++) {
        for (int c = 0; c < 4; c++) {
            float fa = bf16_to_f32(pA[i * 4 + c]);
            float fb = bf16_to_f32(pB[i * 4 + c]);
            float fc = bf16_to_f32(pC[i * 4 + c]);
            // MSL 3.1/3.2 lacks fma(bfloat4, ...), so k_bfloat computes a * b + c;
            // allow either unfused bf16 mul+add (separate bf16 roundings) or fast-math fused FMA.
            uint16_t expMulAdd = f32_to_bf16(bf16_to_f32(f32_to_bf16(fa * fb)) + fc);
            uint16_t expFma = f32_to_bf16(fmaf(fa, fb, fc));
            uint16_t g = gotFma[i * 4 + c];
            require(bf16_ulp_dist(g, expMulAdd) <= 1u || bf16_ulp_dist(g, expFma) <= 1u,
                    [NSString stringWithFormat:@"bfloat muladd[%lu][%d] got=0x%04x(%g) expMulAdd=0x%04x(%g) expFma=0x%04x(%g)",
                     (unsigned long)i, c, g, bf16_to_f32(g),
                     expMulAdd, bf16_to_f32(expMulAdd), expFma, bf16_to_f32(expFma)]);
        }
        // back = bfloat(half(fma.x)): since fma.x is well within half range, bf16 -> fp16 -> bf16 stays within 1 ULP.
        uint16_t expBack = gotFma[i * 4 + 0];
        require(bf16_ulp_dist(gotAux[i * 4 + 0], expBack) <= 1u,
                [NSString stringWithFormat:@"bfloat->half->bfloat[%lu] got=0x%04x exp=0x%04x",
                 (unsigned long)i, gotAux[i * 4 + 0], expBack]);

        float ax = bf16_to_f32(pA[i * 4 + 0]);
        if (ax < -2.0f) ax = -2.0f;
        if (ax > 2.0f) ax = 2.0f;
        uint16_t expE = f32_to_bf16(expf(bf16_to_f32(f32_to_bf16(ax))));
        require(bf16_ulp_dist(gotAux[i * 4 + 1], expE) <= 2u,
                [NSString stringWithFormat:@"bfloat exp[%lu] got=0x%04x(%g) exp=0x%04x(%g)",
                 (unsigned long)i, gotAux[i * 4 + 1], bf16_to_f32(gotAux[i * 4 + 1]), expE, bf16_to_f32(expE)]);

        float ay = bf16_to_f32(pA[i * 4 + 1]);
        float satRef = ay < 0.0f ? 0.0f : (ay > 1.0f ? 1.0f : ay);
        uint16_t expSat = f32_to_bf16(satRef);
        require(bf16_ulp_dist(gotAux[i * 4 + 2], expSat) == 0u,
                [NSString stringWithFormat:@"bfloat saturate[%lu] got=0x%04x exp=0x%04x",
                 (unsigned long)i, gotAux[i * 4 + 2], expSat]);

        uint16_t expFi = f32_to_bf16((float)((int)i - 32));
        require(bf16_ulp_dist(gotAux[i * 4 + 3], expFi) == 0u,
                [NSString stringWithFormat:@"bfloat sitofp[%lu] got=0x%04x exp=0x%04x",
                 (unsigned long)i, gotAux[i * 4 + 3], expFi]);

        uint32_t expU = (uint32_t)bf16_to_f32(f32_to_bf16((float)(i + 5u)));
        require(gotU[i] == expU,
                [NSString stringWithFormat:@"bfloat fptoui[%lu] got=%u exp=%u", (unsigned long)i, gotU[i], expU]);
    }
    puts("METAL3_MSL3_ATOMICS_CASE_PASS bfloat");
}

static id<MTLLibrary> compileTextureAtomicLib(id<MTLDevice> dev, NSString **usedVariantOut) {
    struct Variant {
        const char *name;
        const char *uAdd;
        const char *uSub;
        const char *uMin;
        const char *uMax;
        const char *uAnd;
        const char *uOr;
        const char *uXor;
        const char *uXchg;
        const char *sMin;
        const char *sMax;
    };
    static const struct Variant kVariants[] = {
        {
            "member_coord_vec4",
            "texU.atomic_fetch_add(uint2(0,0), uint4(gid + 1u, 0, 0, 0))",
            "texU.atomic_fetch_sub(uint2(1,0), uint4(gid + 1u, 0, 0, 0))",
            "texU.atomic_fetch_min(uint2(2,0), uint4(5000u - gid * 31u, 0, 0, 0))",
            "texU.atomic_fetch_max(uint2(3,0), uint4(100u + gid * 17u, 0, 0, 0))",
            "texU.atomic_fetch_and(uint2(0,1), uint4(~(1u << (gid & 15u)), 0, 0, 0))",
            "texU.atomic_fetch_or(uint2(1,1), uint4(1u << (gid & 31u), 0, 0, 0))",
            "texU.atomic_fetch_xor(uint2(2,1), uint4(0x01010101u ^ gid, 0, 0, 0))",
            "texU.atomic_exchange(uint2(3,1), uint4(gid + 1001u, 0, 0, 0)).x",
            "texS.atomic_fetch_min(uint2(0,0), int4(int(gid) * 10 - 300, 0, 0, 0))",
            "texS.atomic_fetch_max(uint2(1,0), int4(int(gid) * 10 - 300, 0, 0, 0))",
        },
        {
            "member_explicit_coord_vec4",
            "texU.atomic_fetch_add_explicit(uint2(0,0), uint4(gid + 1u, 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_sub_explicit(uint2(1,0), uint4(gid + 1u, 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_min_explicit(uint2(2,0), uint4(5000u - gid * 31u, 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_max_explicit(uint2(3,0), uint4(100u + gid * 17u, 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_and_explicit(uint2(0,1), uint4(~(1u << (gid & 15u)), 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_or_explicit(uint2(1,1), uint4(1u << (gid & 31u), 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_fetch_xor_explicit(uint2(2,1), uint4(0x01010101u ^ gid, 0, 0, 0), memory_order_relaxed)",
            "texU.atomic_exchange_explicit(uint2(3,1), uint4(gid + 1001u, 0, 0, 0), memory_order_relaxed).x",
            "texS.atomic_fetch_min_explicit(uint2(0,0), int4(int(gid) * 10 - 300, 0, 0, 0), memory_order_relaxed)",
            "texS.atomic_fetch_max_explicit(uint2(1,0), int4(int(gid) * 10 - 300, 0, 0, 0), memory_order_relaxed)",
        },
        {
            "member_vec4_coord",
            "texU.atomic_fetch_add(uint4(gid + 1u, 0, 0, 0), uint2(0,0))",
            "texU.atomic_fetch_sub(uint4(gid + 1u, 0, 0, 0), uint2(1,0))",
            "texU.atomic_fetch_min(uint4(5000u - gid * 31u, 0, 0, 0), uint2(2,0))",
            "texU.atomic_fetch_max(uint4(100u + gid * 17u, 0, 0, 0), uint2(3,0))",
            "texU.atomic_fetch_and(uint4(~(1u << (gid & 15u)), 0, 0, 0), uint2(0,1))",
            "texU.atomic_fetch_or(uint4(1u << (gid & 31u), 0, 0, 0), uint2(1,1))",
            "texU.atomic_fetch_xor(uint4(0x01010101u ^ gid, 0, 0, 0), uint2(2,1))",
            "texU.atomic_exchange(uint4(gid + 1001u, 0, 0, 0), uint2(3,1)).x",
            "texS.atomic_fetch_min(int4(int(gid) * 10 - 300, 0, 0, 0), uint2(0,0))",
            "texS.atomic_fetch_max(int4(int(gid) * 10 - 300, 0, 0, 0), uint2(1,0))",
        },
        {
            "member_explicit_vec4_coord",
            "texU.atomic_fetch_add_explicit(uint4(gid + 1u, 0, 0, 0), uint2(0,0), memory_order_relaxed)",
            "texU.atomic_fetch_sub_explicit(uint4(gid + 1u, 0, 0, 0), uint2(1,0), memory_order_relaxed)",
            "texU.atomic_fetch_min_explicit(uint4(5000u - gid * 31u, 0, 0, 0), uint2(2,0), memory_order_relaxed)",
            "texU.atomic_fetch_max_explicit(uint4(100u + gid * 17u, 0, 0, 0), uint2(3,0), memory_order_relaxed)",
            "texU.atomic_fetch_and_explicit(uint4(~(1u << (gid & 15u)), 0, 0, 0), uint2(0,1), memory_order_relaxed)",
            "texU.atomic_fetch_or_explicit(uint4(1u << (gid & 31u), 0, 0, 0), uint2(1,1), memory_order_relaxed)",
            "texU.atomic_fetch_xor_explicit(uint4(0x01010101u ^ gid, 0, 0, 0), uint2(2,1), memory_order_relaxed)",
            "texU.atomic_exchange_explicit(uint4(gid + 1001u, 0, 0, 0), uint2(3,1), memory_order_relaxed).x",
            "texS.atomic_fetch_min_explicit(int4(int(gid) * 10 - 300, 0, 0, 0), uint2(0,0), memory_order_relaxed)",
            "texS.atomic_fetch_max_explicit(int4(int(gid) * 10 - 300, 0, 0, 0), uint2(1,0), memory_order_relaxed)",
        },
        {
            "member_coord_scalar",
            "texU.atomic_fetch_add(uint2(0,0), gid + 1u)",
            "texU.atomic_fetch_sub(uint2(1,0), gid + 1u)",
            "texU.atomic_fetch_min(uint2(2,0), 5000u - gid * 31u)",
            "texU.atomic_fetch_max(uint2(3,0), 100u + gid * 17u)",
            "texU.atomic_fetch_and(uint2(0,1), ~(1u << (gid & 15u)))",
            "texU.atomic_fetch_or(uint2(1,1), 1u << (gid & 31u))",
            "texU.atomic_fetch_xor(uint2(2,1), 0x01010101u ^ gid)",
            "texU.atomic_exchange(uint2(3,1), gid + 1001u)",
            "texS.atomic_fetch_min(uint2(0,0), int(gid) * 10 - 300)",
            "texS.atomic_fetch_max(uint2(1,0), int(gid) * 10 - 300)",
        },
        {
            "member_explicit_coord_scalar",
            "texU.atomic_fetch_add_explicit(uint2(0,0), gid + 1u, memory_order_relaxed)",
            "texU.atomic_fetch_sub_explicit(uint2(1,0), gid + 1u, memory_order_relaxed)",
            "texU.atomic_fetch_min_explicit(uint2(2,0), 5000u - gid * 31u, memory_order_relaxed)",
            "texU.atomic_fetch_max_explicit(uint2(3,0), 100u + gid * 17u, memory_order_relaxed)",
            "texU.atomic_fetch_and_explicit(uint2(0,1), ~(1u << (gid & 15u)), memory_order_relaxed)",
            "texU.atomic_fetch_or_explicit(uint2(1,1), 1u << (gid & 31u), memory_order_relaxed)",
            "texU.atomic_fetch_xor_explicit(uint2(2,1), 0x01010101u ^ gid, memory_order_relaxed)",
            "texU.atomic_exchange_explicit(uint2(3,1), gid + 1001u, memory_order_relaxed)",
            "texS.atomic_fetch_min_explicit(uint2(0,0), int(gid) * 10 - 300, memory_order_relaxed)",
            "texS.atomic_fetch_max_explicit(uint2(1,0), int(gid) * 10 - 300, memory_order_relaxed)",
        },
    };
    NSError *lastErr = nil;
    for (size_t i = 0; i < sizeof(kVariants) / sizeof(kVariants[0]); i++) {
        const struct Variant *v = &kVariants[i];
        NSString *src = [NSString stringWithFormat:
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void k_tex_atomics(texture2d<uint, access::read_write> texU [[texture(0)]],\n"
             "                          texture2d<int, access::read_write> texS [[texture(1)]],\n"
             "                          device uint *xchgOld [[buffer(0)]],\n"
             "                          uint gid [[thread_position_in_grid]]) {\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "    xchgOld[gid] = %s;\n"
             "    (void)%s;\n"
             "    (void)%s;\n"
             "}\n",
            v->uAdd, v->uSub, v->uMin, v->uMax, v->uAnd, v->uOr, v->uXor, v->uXchg, v->sMin, v->sMax];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (lib) {
            if (usedVariantOut) *usedVariantOut = @(v->name);
            return lib;
        }
        lastErr = err;
    }
    failf([NSString stringWithFormat:@"tex_atomics compile failed across all MSL 3.1 spellings: %@", lastErr]);
}

static void runTextureAtomics(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *variant = nil;
    id<MTLLibrary> lib = compileTextureAtomicLib(dev, &variant);
    id<MTLComputePipelineState> pso = buildComputePSO(dev, lib, @"k_tex_atomics", @"tex_atomics");

    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (@available(macOS 14.0, *)) {
        usage |= MTLTextureUsageShaderAtomic;
    }
    MTLTextureDescriptor *tdu = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                   width:4 height:4 mipmapped:NO];
    tdu.usage = usage;
    tdu.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texU = [dev newTextureWithDescriptor:tdu];
    require(texU != nil, @"tex_atomics texU nil");

    MTLTextureDescriptor *tds = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Sint
                                                                                   width:4 height:4 mipmapped:NO];
    tds.usage = usage;
    tds.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texS = [dev newTextureWithDescriptor:tds];
    require(texS != nil, @"tex_atomics texS nil");

    const NSUInteger texBytes = 4 * 4 * sizeof(uint32_t);
    id<MTLBuffer> stgU = [dev newBufferWithLength:texBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> stgS = [dev newBufferWithLength:texBytes options:MTLResourceStorageModeShared];
    const NSUInteger kThreads = 64;
    const NSUInteger xchgBytes = kThreads * sizeof(uint32_t);
    id<MTLBuffer> xchgBuf = [dev newBufferWithLength:xchgBytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(stgU && stgS && xchgBuf, @"tex_atomics staging buffers nil");
    initGuardedBuffer(xchgBuf, xchgBytes);

    uint32_t initU[16];
    int32_t initS[16];
    for (int i = 0; i < 16; i++) {
        initU[i] = 0x5A5A0000u + (uint32_t)i;
        initS[i] = -100000 - i;
    }
    initU[0] = 10u;
    initU[1] = 100000u;
    initU[2] = 0xFFFFFFFFu;
    initU[3] = 0u;
    initU[4] = 0xFFFFFFFFu;
    initU[5] = 0u;
    initU[6] = 0x12345678u;
    initU[7] = 1000u;
    initS[0] = 500;
    initS[1] = -500;
    memcpy(stgU.contents, initU, texBytes);
    memcpy(stgS.contents, initS, texBytes);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromBuffer:stgU sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:texBytes
            sourceSize:MTLSizeMake(4, 4, 1) toTexture:texU destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl copyFromBuffer:stgS sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:texBytes
            sourceSize:MTLSizeMake(4, 4, 1) toTexture:texS destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl endEncoding];

    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setTexture:texU atIndex:0];
    [ce setTexture:texS atIndex:1];
    [ce setBuffer:xchgBuf offset:kGuardBytes atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kThreads, 1, 1)];
    [ce endEncoding];

    id<MTLBuffer> rbU = [dev newBufferWithLength:texBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> rbS = [dev newBufferWithLength:texBytes options:MTLResourceStorageModeShared];
    memset(rbU.contents, 0xCC, texBytes);
    memset(rbS.contents, 0xCC, texBytes);
    id<MTLBlitCommandEncoder> bl2 = [cb blitCommandEncoder];
    [bl2 copyFromTexture:texU sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(4, 4, 1) toBuffer:rbU destinationOffset:0
  destinationBytesPerRow:16 destinationBytesPerImage:texBytes];
    [bl2 copyFromTexture:texS sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(4, 4, 1) toBuffer:rbS destinationOffset:0
  destinationBytesPerRow:16 destinationBytesPerImage:texBytes];
    [bl2 endEncoding];
    finishOK(cb, @"tex_atomics");
    checkGuards(xchgBuf, xchgBytes, @"tex_atomics xchg");

    uint32_t expAdd = 10u, expSub = 100000u, expMin = 0xFFFFFFFFu, expMax = 0u;
    uint32_t expAnd = 0xFFFFFFFFu, expOr = 0u, expXor = 0x12345678u;
    int32_t expSMin = 500, expSMax = -500;
    for (uint32_t gid = 0; gid < kThreads; gid++) {
        expAdd += gid + 1u;
        expSub -= gid + 1u;
        uint32_t cmin = 5000u - gid * 31u;
        if (cmin < expMin) expMin = cmin;
        uint32_t cmax = 100u + gid * 17u;
        if (cmax > expMax) expMax = cmax;
        expAnd &= ~(1u << (gid & 15u));
        expOr |= (1u << (gid & 31u));
        expXor ^= (0x01010101u ^ gid);
        int32_t sv = (int32_t)gid * 10 - 300;
        if (sv < expSMin) expSMin = sv;
        if (sv > expSMax) expSMax = sv;
    }

    const uint32_t *gotU = (const uint32_t *)rbU.contents;
    const int32_t *gotS = (const int32_t *)rbS.contents;
    const uint32_t *gotXchg = (const uint32_t *)((const uint8_t *)xchgBuf.contents + kGuardBytes);

    require(gotU[0] == expAdd && gotU[1] == expSub && gotU[2] == expMin && gotU[3] == expMax &&
            gotU[4] == expAnd && gotU[5] == expOr && gotU[6] == expXor,
            [NSString stringWithFormat:@"texU got=(%u,%u,%u,%u,0x%x,0x%x,0x%x) exp=(%u,%u,%u,%u,0x%x,0x%x,0x%x)",
             gotU[0], gotU[1], gotU[2], gotU[3], gotU[4], gotU[5], gotU[6],
             expAdd, expSub, expMin, expMax, expAnd, expOr, expXor]);
    require(gotS[0] == expSMin && gotS[1] == expSMax,
            [NSString stringWithFormat:@"texS got=(%d,%d) exp=(%d,%d)", gotS[0], gotS[1], expSMin, expSMax]);

    // Verify exchange permutation on texU(3,1) across 64 threads.
    uint8_t seen[65] = {0};
    for (NSUInteger i = 0; i < kThreads; i++) {
        uint32_t v = gotXchg[i];
        require(v >= 1000u && v <= 1064u, [NSString stringWithFormat:@"texU xchg[%lu]=%u out of range", (unsigned long)i, v]);
        seen[v - 1000u]++;
    }
    require(gotU[7] >= 1000u && gotU[7] <= 1064u, [NSString stringWithFormat:@"texU xchg final=%u out of range", gotU[7]]);
    seen[gotU[7] - 1000u]++;
    for (int k = 0; k <= 64; k++) {
        require(seen[k] == 1u, [NSString stringWithFormat:@"texU xchg value %d count=%u != 1", 1000 + k, seen[k]]);
    }

    // Verify untouched guard texels in rows 2..3 (and texS slots 2..15).
    for (int i = 8; i < 16; i++) {
        require(gotU[i] == initU[i], [NSString stringWithFormat:@"texU guard[%d] got=0x%x exp=0x%x", i, gotU[i], initU[i]]);
    }
    for (int i = 2; i < 16; i++) {
        require(gotS[i] == initS[i], [NSString stringWithFormat:@"texS guard[%d] got=%d exp=%d", i, gotS[i], initS[i]]);
    }
    printf("METAL3_MSL3_ATOMICS_CASE_PASS texture_atomics variant=%s\n", variant.UTF8String);
}

static void runSimdShuffleFill(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct ShuffleOut {\n"
         "    uint up1;\n"
         "    uint up3;\n"
         "    uint dn1;\n"
         "    uint dn5;\n"
         "    uint up2m8;\n"
         "    uint dn3m8;\n"
         "    uint sgSize;\n"
         "    uint lane;\n"
         "};\n"
         "kernel void k_simd_fill(device ShuffleOut *out [[buffer(0)]],\n"
         "                        uint gid [[thread_position_in_grid]],\n"
         "                        uint lane [[thread_index_in_simdgroup]],\n"
         "                        uint sg [[simdgroup_index_in_threadgroup]],\n"
         "                        uint sgSize [[threads_per_simdgroup]]) {\n"
         "    uint data = (sg + 1u) * 1000u + lane;\n"
         "    uint fill = (sg + 1u) * 9000u + lane;\n"
         "    ShuffleOut o;\n"
         "    o.up1 = simd_shuffle_and_fill_up(data, fill, 1);\n"
         "    o.up3 = simd_shuffle_and_fill_up(data, fill, 3);\n"
         "    o.dn1 = simd_shuffle_and_fill_down(data, fill, 1);\n"
         "    o.dn5 = simd_shuffle_and_fill_down(data, fill, 5);\n"
         "    o.up2m8 = simd_shuffle_and_fill_up(data, fill, 2, 8);\n"
         "    o.dn3m8 = simd_shuffle_and_fill_down(data, fill, 3, 8);\n"
         "    o.sgSize = sgSize;\n"
         "    o.lane = lane;\n"
         "    out[gid] = o;\n"
         "}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    require(lib != nil, [NSString stringWithFormat:@"simd_fill compile: %@", err]);
    id<MTLComputePipelineState> pso = buildComputePSO(dev, lib, @"k_simd_fill", @"simd_fill");

    const NSUInteger kThreads = 64;
    const NSUInteger tew = pso.threadExecutionWidth;
    require(tew >= 8 && tew <= kThreads && (tew & (tew - 1)) == 0,
            [NSString stringWithFormat:@"simd_fill unexpected threadExecutionWidth=%lu", (unsigned long)tew]);

    typedef struct {
        uint32_t up1, up3, dn1, dn5, up2m8, dn3m8, sgSize, lane;
    } ShuffleOut;
    const NSUInteger bytes = kThreads * sizeof(ShuffleOut);
    id<MTLBuffer> buf = [dev newBufferWithLength:bytes + 2 * kGuardBytes options:MTLResourceStorageModeShared];
    require(buf != nil, @"simd_fill buffer nil");
    initGuardedBuffer(buf, bytes);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso];
    [ce setBuffer:buf offset:kGuardBytes atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kThreads, 1, 1)];
    [ce endEncoding];
    finishOK(cb, @"simd_fill");
    checkGuards(buf, bytes, @"simd_fill");

    const ShuffleOut *out = (const ShuffleOut *)((const uint8_t *)buf.contents + kGuardBytes);
    for (uint32_t gid = 0; gid < kThreads; gid++) {
        uint32_t sgSize = out[gid].sgSize;
        uint32_t lane = out[gid].lane;
        uint32_t sg = gid / sgSize;
        require(sgSize == (uint32_t)tew && lane == (gid % sgSize),
                [NSString stringWithFormat:@"simd_fill[%u] sgSize=%u(tew=%lu) lane=%u",
                 gid, sgSize, (unsigned long)tew, lane]);
        uint32_t dBase = (sg + 1u) * 1000u;
        uint32_t fBase = (sg + 1u) * 9000u;

        uint32_t expUp1 = lane >= 1u ? (dBase + lane - 1u) : (fBase + sgSize + lane - 1u);
        uint32_t expUp3 = lane >= 3u ? (dBase + lane - 3u) : (fBase + sgSize + lane - 3u);
        uint32_t expDn1 = (lane + 1u < sgSize) ? (dBase + lane + 1u) : (fBase + lane + 1u - sgSize);
        uint32_t expDn5 = (lane + 5u < sgSize) ? (dBase + lane + 5u) : (fBase + lane + 5u - sgSize);

        uint32_t subBase = (lane / 8u) * 8u;
        uint32_t subLocal = lane % 8u;
        uint32_t expUp2m8 = subLocal >= 2u ? (dBase + subBase + subLocal - 2u) : (fBase + subBase + 8u + subLocal - 2u);
        uint32_t expDn3m8 = (subLocal + 3u < 8u) ? (dBase + subBase + subLocal + 3u) : (fBase + subBase + subLocal + 3u - 8u);

        require(out[gid].up1 == expUp1 && out[gid].up3 == expUp3 &&
                out[gid].dn1 == expDn1 && out[gid].dn5 == expDn5 &&
                out[gid].up2m8 == expUp2m8 && out[gid].dn3m8 == expDn3m8,
                [NSString stringWithFormat:@"simd_fill[%u] lane=%u got=(%u,%u,%u,%u,%u,%u) exp=(%u,%u,%u,%u,%u,%u)",
                 gid, lane, out[gid].up1, out[gid].up3, out[gid].dn1, out[gid].dn5, out[gid].up2m8, out[gid].dn3m8,
                 expUp1, expUp3, expDn1, expDn5, expUp2m8, expDn3m8]);
    }
    printf("METAL3_MSL3_ATOMICS_CASE_PASS simd_shuffle_and_fill tew=%lu\n", (unsigned long)tew);
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

        runFloatAtomics(dev, q);
        runInt64Atomics(dev, q);
        runBfloat(dev, q);
        runTextureAtomics(dev, q);
        runSimdShuffleFill(dev, q);

        puts("METAL3_MSL3_ATOMICS_PASS");
        return 0;
    }
}
