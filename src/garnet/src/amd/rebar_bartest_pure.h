// rebar_bartest_pure.h - pure high-VRAM direct CPU BAR <-> GPU bidirectional test logic.
// Header-only, C++17, no kernel types, no hardware access; compiled by both Garnet.kext and host unit tests.
//
// Invariants:
//   - Direct CPU BAR0 access ONLY (WC drain + HDP flush). NEVER falls back to MM_INDEX.
//   - Fails closed if offset is outside [visBase, min(bar0Size, vramLimit, vramBytes)), overlaps console/cursor,
//     overflows 64-bit arithmetic, or BAR0 does not cover the requested target.
//   - Covers 4 GiB, 8 GiB, 12 GiB boundaries (straddling blocks [boundary - 64 KiB, boundary + 64 KiB))
//     plus highest allocatable offset below vramLimit.
//   - Verifies both directions (CPU BAR0 write -> GPU read; GPU write -> CPU BAR0 read),
//     surrounding guards (before/after payload), and a low-address canary (< 4 GiB) against 32-bit wrap/aliasing.
#pragma once
#include <stdint.h>
#include "rebar_pure.h"

namespace rdna4rebar {

constexpr uint32_t kActBarTest           = 109u;
constexpr uint64_t kBarTestBlockBytes    = 128ull << 10;              // 128 KiB exclusive BO (64 KiB aligned)
constexpr uint64_t kBarTestHalfBlock     = 64ull << 10;               // midpoint = exact 4/8/12 GiB boundary
constexpr uint32_t kBarTestGuardBytes    = 4096u;                     // 4 KiB leading guard (+0) and trailing guard (+124 KiB)
constexpr uint64_t kBarTestTailGuardOff  = kBarTestBlockBytes - kBarTestGuardBytes;
constexpr uint64_t kBarTestPayloadOff    = kBarTestHalfBlock - 2048u; // 4 KiB payload straddling the 64 KiB midpoint
constexpr uint32_t kBarTestPayloadBytes  = 4096u;                     // 1024 dwords (512 below boundary, 512 at/above)
constexpr uint32_t kBarTestPayloadDwords = kBarTestPayloadBytes / 4u;
constexpr uint32_t kBarTestGuardDwords   = kBarTestGuardBytes / 4u;
constexpr uint32_t kBarTestCanaryBytes   = 64ull << 10;               // 64 KiB low canary / GPU scratch BO
constexpr uint32_t kBarTestCanaryDwords  = 64u;
constexpr uint32_t kMaxBarTestTargets    = 4u;

constexpr bool bartest_is_action(uint32_t action) { return action == kActBarTest; }
constexpr bool bartest_arg_ok(uint64_t arg) { return arg <= 1ull; }   // 0 = full-BAR 4/8/12 GiB + top; 1 = top valid point of current BAR

enum BarTestStatus : uint32_t {
    kBtOk                      = 0,
    kBtBadArg                  = 1,
    kBtNoBar0                  = 2,
    kBtNotFullBar              = 3,
    kBtBadAperture             = 4,
    kBtBelowVisBase            = 5,
    kBtBeyondLimit             = 6,
    kBtConsoleOverlap          = 7,
    kBtOverflow                = 8,
    kBtAllocFail               = 9,
    kBtSdmaNotReady            = 10,
    kBtSdmaFailed              = 11,
    kBtCpuWriteGpuReadMismatch = 12,
    kBtGpuWriteCpuReadMismatch = 13,
    kBtGuardCorrupt            = 14,
    kBtCanaryCorrupt           = 15,
    kBtNoTargets               = 16,
    kBtOverlapSelf             = 17,
};

inline const char *bartest_status_name(uint32_t st) {
    switch (st) {
    case kBtOk: return "ok";
    case kBtBadArg: return "bad argument";
    case kBtNoBar0: return "BAR0 not mapped";
    case kBtNotFullBar: return "full BAR mode not active (pass garnet-full-bar=1 with 16 GiB BAR0)";
    case kBtBadAperture: return "BAR0 aperture / vramLimit inconsistent";
    case kBtBelowVisBase: return "target offset below visible allocator base";
    case kBtBeyondLimit: return "target offset exceeds mapped BAR0 / vramLimit / valid VRAM";
    case kBtConsoleOverlap: return "target overlaps console or cursor";
    case kBtOverflow: return "target offset arithmetic overflow";
    case kBtAllocFail: return "allocator could not grant exclusive test region";
    case kBtSdmaNotReady: return "SDMA0 QUEUE0 not ready";
    case kBtSdmaFailed: return "SDMA copy or fence failed";
    case kBtCpuWriteGpuReadMismatch: return "CPU BAR0 write -> GPU read mismatch";
    case kBtGpuWriteCpuReadMismatch: return "GPU write -> CPU BAR0 read mismatch";
    case kBtGuardCorrupt: return "surrounding guard word corrupted";
    case kBtCanaryCorrupt: return "low-address canary corrupted (possible 32-bit BAR0 wrap/alias)";
    case kBtNoTargets: return "no valid high-BAR test targets";
    case kBtOverlapSelf: return "test buffers overlap each other";
    default: return "?";
    }
}

// Fail-closed validation for a direct-BAR test range [off, off + len).
constexpr uint32_t bartest_range_ok(uint64_t off, uint64_t len, uint64_t visBase,
                                    uint64_t vramLimit, uint64_t bar0Size, uint64_t vramBytes,
                                    uint64_t consoleOff, uint64_t consoleLen) {
    if (bar0Size == 0ull || vramLimit == 0ull) return kBtNoBar0;
    if (vramLimit > bar0Size || (vramBytes != 0ull && vramLimit > vramBytes)) return kBtBadAperture;
    if (len == 0ull || off > ~0ull - len) return kBtOverflow;
    if (off < visBase) return kBtBelowVisBase;
    uint64_t ceil = bar0Size < vramLimit ? bar0Size : vramLimit;
    if (vramBytes != 0ull && vramBytes < ceil) ceil = vramBytes;
    if (off > ceil || len > ceil - off) return kBtBeyondLimit;
    if (consoleOff != kNoConsole && consoleLen != 0ull) {
        if (consoleLen > ~0ull - kCursorBytes || consoleOff > ~0ull - (consoleLen + kCursorBytes)) return kBtOverflow;
        const uint64_t cEnd = consoleOff + consoleLen + kCursorBytes;
        if (off < cEnd && off + len > consoleOff) return kBtConsoleOverlap;
    }
    return kBtOk;
}

// Plan direct-BAR test targets.
// arg == 0: full-BAR mode required; produces 4 GiB, 8 GiB, 12 GiB straddling blocks + highest valid block below vramLimit.
// arg == 1: single highest valid block below vramLimit (works on 256 MiB / 1 GiB / 16 GiB for regression checks).
constexpr uint32_t bartest_plan_targets(bool fullBarMode, uint64_t arg, uint64_t visBase,
                                        uint64_t vramLimit, uint64_t bar0Size, uint64_t vramBytes,
                                        uint64_t consoleOff, uint64_t consoleLen,
                                        uint64_t out[kMaxBarTestTargets], uint32_t *outCount) {
    if (outCount) *outCount = 0u;
    if (!out || !outCount || !bartest_arg_ok(arg)) return kBtBadArg;
    if (bar0Size == 0ull || vramLimit == 0ull) return kBtNoBar0;
    if (vramLimit > bar0Size || (vramBytes != 0ull && vramLimit > vramBytes)) return kBtBadAperture;
    if (arg == 0ull && !fullBarMode) return kBtNotFullBar;
    if (vramLimit < kBarTestBlockBytes) return kBtBeyondLimit;
    const uint64_t top = (vramLimit - kBarTestBlockBytes) & ~(kLimitAlign - 1ull);
    uint32_t n = 0u;
    if (arg == 0ull) {
        const uint64_t boundaries[3] = {
            4ull * kGiB - kBarTestHalfBlock,
            8ull * kGiB - kBarTestHalfBlock,
            12ull * kGiB - kBarTestHalfBlock,
        };
        for (uint32_t i = 0; i < 3u; i++) {
            const uint32_t st = bartest_range_ok(boundaries[i], kBarTestBlockBytes, visBase,
                                                 vramLimit, bar0Size, vramBytes, consoleOff, consoleLen);
            if (st != kBtOk) return st;   // fail closed: full-BAR mode must cover 4/8/12 GiB boundaries
            out[n++] = boundaries[i];
        }
    }
    const uint32_t topSt = bartest_range_ok(top, kBarTestBlockBytes, visBase,
                                            vramLimit, bar0Size, vramBytes, consoleOff, consoleLen);
    if (topSt != kBtOk) return topSt;
    bool dup = false;
    for (uint32_t i = 0; i < n; i++) if (out[i] == top) dup = true;
    if (!dup && n < kMaxBarTestTargets) out[n++] = top;
    if (n == 0u) return kBtNoTargets;
    *outCount = n;
    return kBtOk;
}

// 64-bit-offset-tagged patterns (upper 32 bits mixed in so 4/8/12 GiB never alias low VRAM).
constexpr uint32_t bartest_pat_fwd(uint64_t blockOff, uint32_t dwordIdx) {
    const uint32_t hi = static_cast<uint32_t>(blockOff >> 32);
    const uint32_t lo = static_cast<uint32_t>(blockOff >> 16);
    return (0x42A10000u ^ (hi * 0x9E3779B9u) ^ (lo << 12)) + dwordIdx;
}
constexpr uint32_t bartest_pat_rev(uint64_t blockOff, uint32_t dwordIdx) {
    return (~bartest_pat_fwd(blockOff, dwordIdx)) ^ 0x5A3C96A5u;
}
constexpr uint32_t bartest_guard_word(uint64_t blockOff, bool trailing, uint32_t dwordIdx) {
    const uint32_t hi = static_cast<uint32_t>(blockOff >> 32);
    return (trailing ? 0xE7100000u : 0xD3200000u) ^ (hi * 0x85EBCA6Bu) ^ (dwordIdx * 0x101u);
}
constexpr uint32_t bartest_canary_word(uint32_t dwordIdx) {
    return 0xC4AA0000u ^ (dwordIdx * 0x01010101u);
}

struct BarTestPointIn {
    uint64_t vramStart;
    uint64_t visBase;
    uint64_t vramLimit;
    uint64_t bar0Size;
    uint64_t vramBytes;
    uint64_t consoleOff;
    uint64_t consoleLen;
    uint64_t targetOff;
    uint64_t lowCanaryOff;
    uint64_t gpuScratchOff;
};

struct BarTestPointOut {
    uint32_t status;
    uint32_t firstBadIdx;
    uint32_t want;
    uint32_t got;
};

// Direct CPU BAR0 <-> GPU bidirectional verification on one exclusive target block.
// Ops contract:
//   void     bar0_write32(uint64_t off, uint32_t val)
//   uint32_t bar0_read32(uint64_t off)
//   void     wc_and_hdp_flush()
//   void     hdp_flush()
//   bool     gpu_copy(uint64_t srcMc, uint64_t dstMc, uint32_t bytes)
template <class Ops>
inline BarTestPointOut bartest_run_point(Ops &ops, const BarTestPointIn &in) {
    BarTestPointOut res { kBtOk, 0u, 0u, 0u };
    uint32_t st = bartest_range_ok(in.targetOff, kBarTestBlockBytes, in.visBase,
                                   in.vramLimit, in.bar0Size, in.vramBytes, in.consoleOff, in.consoleLen);
    if (st != kBtOk) { res.status = st; return res; }
    st = bartest_range_ok(in.lowCanaryOff, kBarTestCanaryBytes, in.visBase,
                          in.vramLimit, in.bar0Size, in.vramBytes, in.consoleOff, in.consoleLen);
    if (st != kBtOk) { res.status = st; return res; }
    st = bartest_range_ok(in.gpuScratchOff, kBarTestCanaryBytes, in.visBase,
                          in.vramLimit, in.bar0Size, in.vramBytes, in.consoleOff, in.consoleLen);
    if (st != kBtOk) { res.status = st; return res; }
    if (ranges_overlap_64(in.targetOff, kBarTestBlockBytes, in.lowCanaryOff, kBarTestCanaryBytes) ||
        ranges_overlap_64(in.targetOff, kBarTestBlockBytes, in.gpuScratchOff, kBarTestCanaryBytes) ||
        ranges_overlap_64(in.lowCanaryOff, kBarTestCanaryBytes, in.gpuScratchOff, kBarTestCanaryBytes)) {
        res.status = kBtOverlapSelf;
        return res;
    }
    const uint64_t payloadOff = in.targetOff + kBarTestPayloadOff;
    if (in.vramStart > ~0ull - (in.targetOff + kBarTestBlockBytes) ||
        in.vramStart > ~0ull - (in.gpuScratchOff + kBarTestCanaryBytes)) {
        res.status = kBtOverflow;
        return res;
    }

    // 1) CPU writes low canary, surrounding guards, forward high-VRAM payload, and scratch poison via bar0Virt ONLY.
    for (uint32_t i = 0; i < kBarTestCanaryDwords; i++)
        ops.bar0_write32(in.lowCanaryOff + static_cast<uint64_t>(i) * 4u, bartest_canary_word(i));
    for (uint32_t i = 0; i < kBarTestGuardDwords; i++) {
        ops.bar0_write32(in.targetOff + static_cast<uint64_t>(i) * 4u, bartest_guard_word(in.targetOff, false, i));
        ops.bar0_write32(in.targetOff + kBarTestTailGuardOff + static_cast<uint64_t>(i) * 4u, bartest_guard_word(in.targetOff, true, i));
    }
    for (uint32_t i = 0; i < kBarTestPayloadDwords; i++) {
        ops.bar0_write32(payloadOff + static_cast<uint64_t>(i) * 4u, bartest_pat_fwd(in.targetOff, i));
        ops.bar0_write32(in.gpuScratchOff + static_cast<uint64_t>(i) * 4u, 0xDEADBEEFu);
    }
    ops.wc_and_hdp_flush();

    // 2) Immediate low-canary check catches CPU BAR0 32-bit wrap/aliasing before GPU touches anything.
    for (uint32_t i = 0; i < kBarTestCanaryDwords; i++) {
        const uint32_t want = bartest_canary_word(i);
        const uint32_t got  = ops.bar0_read32(in.lowCanaryOff + static_cast<uint64_t>(i) * 4u);
        if (got != want) { res = { kBtCanaryCorrupt, i, want, got }; return res; }
    }

    // 3) Direction 1 (CPU BAR0 write -> GPU read): GPU copies high-VRAM payload into low scratch.
    if (!ops.gpu_copy(in.vramStart + payloadOff, in.vramStart + in.gpuScratchOff, kBarTestPayloadBytes)) {
        res.status = kBtSdmaFailed;
        return res;
    }
    ops.hdp_flush();
    for (uint32_t i = 0; i < kBarTestPayloadDwords; i++) {
        const uint32_t want = bartest_pat_fwd(in.targetOff, i);
        const uint32_t got  = ops.bar0_read32(in.gpuScratchOff + static_cast<uint64_t>(i) * 4u);
        if (got != want) { res = { kBtCpuWriteGpuReadMismatch, i, want, got }; return res; }
    }

    // 4) Direction 2 (GPU write -> CPU BAR0 read): stage transformed pattern, GPU copies into high-VRAM payload, CPU reads via bar0Virt.
    for (uint32_t i = 0; i < kBarTestPayloadDwords; i++)
        ops.bar0_write32(in.gpuScratchOff + static_cast<uint64_t>(i) * 4u, bartest_pat_rev(in.targetOff, i));
    ops.wc_and_hdp_flush();
    if (!ops.gpu_copy(in.vramStart + in.gpuScratchOff, in.vramStart + payloadOff, kBarTestPayloadBytes)) {
        res.status = kBtSdmaFailed;
        return res;
    }
    ops.hdp_flush();
    for (uint32_t i = 0; i < kBarTestPayloadDwords; i++) {
        const uint32_t want = bartest_pat_rev(in.targetOff, i);
        const uint32_t got  = ops.bar0_read32(payloadOff + static_cast<uint64_t>(i) * 4u);
        if (got != want) { res = { kBtGpuWriteCpuReadMismatch, i, want, got }; return res; }
    }

    // 5) Surrounding guards + low-address canary untouched after both directions.
    for (uint32_t i = 0; i < kBarTestGuardDwords; i++) {
        const uint32_t wantL = bartest_guard_word(in.targetOff, false, i);
        const uint32_t gotL  = ops.bar0_read32(in.targetOff + static_cast<uint64_t>(i) * 4u);
        if (gotL != wantL) { res = { kBtGuardCorrupt, i, wantL, gotL }; return res; }
        const uint32_t wantT = bartest_guard_word(in.targetOff, true, i);
        const uint32_t gotT  = ops.bar0_read32(in.targetOff + kBarTestTailGuardOff + static_cast<uint64_t>(i) * 4u);
        if (gotT != wantT) { res = { kBtGuardCorrupt, kBarTestGuardDwords + i, wantT, gotT }; return res; }
    }
    for (uint32_t i = 0; i < kBarTestCanaryDwords; i++) {
        const uint32_t want = bartest_canary_word(i);
        const uint32_t got  = ops.bar0_read32(in.lowCanaryOff + static_cast<uint64_t>(i) * 4u);
        if (got != want) { res = { kBtCanaryCorrupt, i, want, got }; return res; }
    }

    // 6) Scrub target payload & guards before returning the exclusive block to the allocator.
    for (uint32_t i = 0; i < kBarTestPayloadDwords; i++)
        ops.bar0_write32(payloadOff + static_cast<uint64_t>(i) * 4u, 0u);
    for (uint32_t i = 0; i < kBarTestGuardDwords; i++) {
        ops.bar0_write32(in.targetOff + static_cast<uint64_t>(i) * 4u, 0u);
        ops.bar0_write32(in.targetOff + kBarTestTailGuardOff + static_cast<uint64_t>(i) * 4u, 0u);
    }
    ops.wc_and_hdp_flush();
    return res;
}

} // namespace rdna4rebar
