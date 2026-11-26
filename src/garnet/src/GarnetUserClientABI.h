//
//  GarnetUserClientABI.h — the kernel/userspace contract for GarnetDriver.
//
//  Included by BOTH the kext (src/GarnetUserClient.cpp) and userspace
//  diagnostic tools, so there is exactly one definition of
//  every selector and struct. Plain C, no IOKit types.
//
//  Stability: none. This is a bring-up interface for diagnostic tools; the
//  selector numbers change whenever it is convenient. The real accelerator
//  interface is a separate Apple-shaped interface.
//
#ifndef GARNET_USER_CLIENT_ABI_H
#define GARNET_USER_CLIENT_ABI_H

#include <stdint.h>

#define GARNET_UC_SERVICE_NAME "GarnetDriver"
#define RDNA4_UC_SERVICE_NAME  GARNET_UC_SERVICE_NAME
#define RDNA4_UC_ABI_VERSION  6u

enum RDNA4Selector {
    kRDNA4SelGetInfo     = 0,   // out struct RDNA4Info
    kRDNA4SelAllocVRAM   = 1,   // in: size, align[, flags]  out: gpu_va, handle
    kRDNA4SelFreeVRAM    = 2,   // in: handle
    kRDNA4SelWriteVRAM   = 3,   // in: gpu_va + struct bytes
    kRDNA4SelReadVRAM    = 4,   // in: gpu_va, len        out struct bytes
    kRDNA4SelSubmitIB    = 5,   // in: gpu_va, len_dw, vmid  out: fence
    kRDNA4SelWaitFence   = 6,   // in: fence, timeout_us  out: observed, elapsed_us
    kRDNA4SelRegRead     = 7,   // in: absolute dword     out: value
    kRDNA4SelSelfTest    = 8,   // in struct RDNA4SelfTestIn, out struct …Out
    kRDNA4SelGetCounters = 9,   // out struct RDNA4Counters
    kRDNA4SelReadLog     = 10,  // in: offset  out struct bytes; out scalar: total
    kRDNA4SelMetrics     = 11,  // out struct RDNA4Metrics (live PMFW telemetry)
    kRDNA4SelPowerState  = 12,  // in: state (0 auto, 1 low, 2 nominal, 3 high, 4 peak)
    // in: action (0 = status only, 1 = fire)
    // out: installed, ttl_total_calls, ttl_first_unsupported_slot
    //
    // Fires the Phase 4 accelerator experiment ON DEMAND rather than at boot.
    // This is a safety property, not a convenience: if Apple's accelerator
    // panics while driving our TTL, an experiment that ran automatically at
    // boot would panic again on the next boot, and again, with nobody at the
    // keyboard to pick a different OpenCore entry. Fired from userspace, a
    // panic costs one reboot and the machine comes back up clean.
    kRDNA4SelAccelExperiment = 13,
    //: drop the driver log so a measurement stands alone. rdna4_log_reset()
    // has existed since the log was written but was never reachable from
    // userspace, so a full buffer silently discarded every NEW line while
    // still reading back complete. That cost fifteen void captures.
    kRDNA4SelLogReset        = 14,  // no args
    // 0.0.276: drop the FIRST `upTo` bytes of the driver log - bytes a reader has already copied out -
    // keeping everything appended since. `rdna4test logstream` reads, appends to a file, F_FULLFSYNCs, then consumes,
    // so the ring never fills and a panic cannot take the log with it (rule 92).
    // in: upTo   out scalars: bytes still held, total consumed since boot, total bytes dropped while full, overflow flag
    kRDNA4SelLogConsume      = 15,
    // 0.0.282: the binary CAPTURE ring (amd/rdna4_cap.h), streamed exactly like the log. ReadCap: in offset, out struct
    // bytes (at most RDNA4_UC_MAX_XFER), out scalar held bytes. CapConsume: in upTo; out held, consumed total, dropped total,
    // records appended total.
    kRDNA4SelReadCap         = 16,
    kRDNA4SelCapConsume      = 17,
    // build 0.0.542 (dcn/scanout_full.h): the capture `accel scanout 9` (`scanout full`) published - its 256-byte header then
    // the whole scanned-out surface. in: offset; out struct bytes (at most RDNA4_UC_MAX_XFER); out scalars: the capture's total
    // bytes (0 = none published), its number this boot. Read-only; `accel scanout 10` frees it. An older kext answers unsupported.
    kRDNA4SelReadScanFull    = 18,
    kRDNA4SelCount
};

// Largest payload a single Read/WriteVRAM call moves. Keeps the IOKit
// structure transfer inside the "small" path and bounds the kernel copy.
#define RDNA4_UC_MAX_XFER 4096u

// 0.0.263 — the driver log ring's capacity, in ONE place.
//
// It lived as two independent literals: kRDNA4LogBytes in amd/rdna4_log.h sized the ring, and
// rdna4test.c re-hardcoded `512u*1024u - 1024u` to decide when to print "AT CAPACITY".
// Raising one without the other would have made the warning lie - and in the alarming
// direction, claiming a 2 MiB ring was full at 511 KiB. Both now derive from this.
//
// Raised 512 KiB -> 2 MiB because r96 AND r97 both hit the cap mid-run (~524,266 bytes) and
// dropped their late-boot lines, so neither run's log could be claimed complete - twice
// flagged, and the next question may live in exactly those dropped lines. A run is ~1300
// lines averaging well under 200 bytes; the instruments added in 0.0.261-0.0.263 (the
// unfiltered packet dump and its raw-dword lines) are what pushed it over. This is a static
// array in the kext, so the cost is 1.5 MiB of wired memory on a bring-up driver.
#define RDNA4_LOG_BYTES (2048u * 1024u)

struct RDNA4Info {
    uint32_t abi_version;
    uint32_t stage_reached;      // BringupStage the ladder got to
    uint32_t stage_result;       // kern_return_t of the last stage
    uint32_t gc_version;         // (major << 16) | (minor << 8) | rev
    uint64_t vram_total_bytes;
    uint64_t vram_free_bytes;
    uint64_t vram_start_mc;      // MC address of VRAM byte 0
    uint32_t flags;              // see kRDNA4Flag*
    uint32_t doorbell_gfx_ring0;
    uint32_t cp_ring_size_dwords;
    // Device-only pool above the BAR0 aperture (ABI 2). The CPU reaches it
    // only through the MM_INDEX window, so staging into it is slow; the GPU
    // sees no difference.
    uint64_t vram_hi_total_bytes;
    uint64_t vram_hi_free_bytes;
    uint32_t reserved[1];
};

// AllocVRAM flags (third scalar input; absent means 0).
#define kRDNA4AllocDeviceOnly (1u << 0)   // take it from the pool above BAR0

#define kRDNA4FlagCPReady      (1u << 0)   // GFX ring mapped and proven
#define kRDNA4FlagMESReady     (1u << 1)
#define kRDNA4FlagSDMAReady    (1u << 2)
#define kRDNA4FlagIRQArmed     (1u << 3)
#define kRDNA4FlagComputeOK    (1u << 4)   // stage 17 passed this boot

enum RDNA4SelfTestID {
    kRDNA4TestNopSubmit   = 0,  // N × (NOP + EOP fence + doorbell + wait)
    kRDNA4TestWriteData   = 1,  // N × (WRITE_DATA magic to VRAM, verify)
    kRDNA4TestFenceBurst  = 2,  // N fences in flight before waiting
    kRDNA4TestCompute     = 3,  // N × the full stage-17 compute dispatch
    kRDNA4TestIDCount
};

struct RDNA4SelfTestIn {
    uint32_t test_id;
    uint32_t iterations;
    uint32_t timeout_us;         // per iteration
    uint32_t reserved;
};

struct RDNA4SelfTestOut {
    uint32_t test_id;
    uint32_t iterations_run;
    uint32_t failures;
    uint32_t first_failure_iter;
    uint64_t elapsed_us;         // whole run, loop-step estimate
    uint64_t last_fence;
    uint32_t last_observed;      // test-specific witness value
    uint32_t kr;                 // first non-success kern_return_t
};

// A readable slice of PMFW's metrics table. Clocks in MHz, temperatures in
// degrees C, power in watts, activity in percent — the raw table's units.
struct RDNA4Metrics {
    uint32_t gfxclk_mhz, socclk_mhz, uclk_mhz, fclk_mhz;
    uint32_t temp_edge_c, temp_hotspot_c, temp_mem_c;
    uint32_t socket_power_w, board_power_w;
    uint32_t gfx_activity_pct, mem_activity_pct;
    uint32_t fan_rpm, fan_pwm_pct;
    uint32_t pcie_gen, pcie_width;
    uint32_t metrics_counter;
    // PMFW driver-IF versions 0x2E..0x33 share the verified 260-byte layout;
    // unknown versions stay flagged unverified and rely on values_plausible.
    uint32_t if_version;        // what the firmware reported
    uint32_t layout_verified;   // 1 if if_version is in the verified range
    uint32_t values_plausible;  // 1 if the decoded values pass range checks
    uint32_t reserved[1];
};

struct RDNA4Counters {
    uint64_t irq_count;
    uint64_t irq_entries;
    uint64_t irq_eop;
    uint64_t irq_faults;
    uint64_t irq_cp_errors;
    uint64_t irq_other;
    uint64_t submits;
    uint64_t fences_completed;
    uint64_t fence_timeouts;
};

#endif /* GARNET_USER_CLIENT_ABI_H */
