//
//  scanout_sdma.cpp — SDMA console scanout copies, bounded fastcopy staging,
//  double-buffered flip mode, and full scanout capture.
//
#include "../GarnetDriver.hpp"
#include "../GarnetDriverInternal.hpp"
#include "../GarnetDisplayOps.h"
#include "scanout_sdma.hpp"
#include "dcn.hpp"
#include "scanout_copy.h"
#include "gfx_flipmode.h"
#include "scanout_full.h"
#include "scanout_verify_pure.h"
#include "../regs.hpp"
#include "../amd/amdgpu_init.h"
#include "../amd/amdgpu_regs.h"
#include "../amd/amdgpu_sdma.h"
#include "../amd/amdgpu_sysmem.h"
#include "../amd/native_disp.h"
#include "../amd/native_display_identity.h"
#include "../amd/sdma_gcr.h"
#include "../amd/sdma_dcc.h"
#include "../amd/fastcopy.h"
#include "../amd/fastcopy89.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#define gBringup (garnet_bringup_ctx())

static IOLock    *gScanoutLock { nullptr };     // every QUEUE0 submission of this feature
static bool       gScanoutPcPassed { false };   // the interlock: positive control passed this boot
static uint32_t   gScanoutPcRuns { 0 }, gScanoutCopies { 0 }, gScanoutRefusals { 0 };
static uint32_t   gScanoutLastReason { 0 };
static uint32_t  *gScanoutSave { nullptr };     // the rectangle the positive control overwrote
static uint32_t   gScanoutSaveX { 0 }, gScanoutSaveY { 0 }, gScanoutSaveW { 0 }, gScanoutSaveH { 0 };
static constexpr uint32_t kScanPcX = 64, kScanPcY = 96, kScanPcW = 256, kScanPcH = 64;
static constexpr uint32_t kScanMaxRows = 256;     // 256*8 + 4 dwords, inside the 4096-dword ring
// 0.0.278: the flush copies are WHOLE FRAMES. The plan admits up to kScanCopyMaxRows rows and the packets go out
// in submissions of kScanChunkRows (200*8 + 4 = 1604 dwords, under scanout_sdma_copies' half-ring bound of 2048), each with
// its own fence, one after the other. The positive control keeps kScanMaxRows. Copy log lines have a budget: a WindowServer
// flushes on every update.
static constexpr uint32_t kScanCopyMaxRows = 2160, kScanChunkRows = 200, kScanCopyLogBudget = 12, kScanCopyFailLogBudget = 24;
static uint32_t   gScanoutCopyCalls { 0 }, gScanoutCopyFailLogs { 0 };
// build 0.0.543 item E3 (the 0.0.542 review's SHOULD-FIX): scanout WRITES into A or B, counted AT THE WRITE (under gScanoutLock,
// immediately before each writer's SDMA copy), not at the writer's entry: gScanoutCopyCalls++ runs before IOLockLock(gScanoutLock), so a
// writer that entered before `scanout full` sampled it and wrote after was invisible to RDNA4_SF_F_WRITTEN_DURING.
static volatile uint32_t gScanoutWrites { 0 };

// Refusal / status codes shared by both entry points (out[0]). 0 = done and verified.
enum : uint32_t {
	kScanStOk = 0, kScanStNoContext = 1, kScanStNoFramebuffer = 2, kScanStGeom = 3, kScanStMcBase = 4,
	kScanStQueue = 5, kScanStQueueBusy = 6, kScanStAlloc = 7, kScanStSourceCtl = 8, kScanStPreflight = 9,
	kScanStFence = 10, kScanStReadback = 11, kScanStPlan = 12, kScanStInterlock = 13, kScanStRing = 14,
	kScanStNoSave = 15,
	// 0.0.416 (an internal design note, G2): the named refusals of `accel scanout 7`'s source window.
	kScanStGcrAlign = 18, kScanStGcrRange = 19, kScanStGcrOverlap = 20,
	// build 0.0.518: a writer into the console refused because flip mode is ON (the display may be
	// scanning B, and A is flip mode's to write: only the flip path and its restore write A or B while the switch is ON).
	kScanStFlipMode = 21,
	// build 0.0.542: `scanout 9` / `scanout 10` refused; out[1] names why (dcn/scanout_full.h RDNA4_SF_*).
	kScanStFull = 22,
};

// build 0.0.518: FLIP MODE's state (dcn/gfx_flipmode.h; switch 74, DEFAULT OFF). Written only under
// gFmLock (lock order: gFmLock, then gScanoutLock inside the copies); `on` and `engaged` are also read lock-free by the other
// scanout writers and the readers (W4), and `restoreReq` is set lock-free by a withdrawal on the submit path.
static rdna4_fm  gFm {};
static IOLock *gFmLock { nullptr };

// build 0.0.518 (W4): the buffer the display is SCANNING, for our own readers (the `scanout 3` content read and FNV, the
// `scanout 4` thumbnail). Until flip mode has allocated B this boot it answers the console's offset without reading anything
// (0.0.517's readers exactly); afterwards it asks HUBP0's SURFACE_EARLIEST_INUSE (a read through the bound device) and answers
// B's offset when that names B, else the console's.
static uint64_t fm_displayed_off(uint64_t consoleOff, uint64_t consoleLen) {
	if (!__atomic_load_n(&gFm.haveB, __ATOMIC_ACQUIRE) || gFm.len < consoleLen) return consoleOff;
	uint32_t pending = 0; uint64_t e = 0;
	if (rdna4dcn::fmReadFront(&pending, &e) != 0) return consoleOff;
	return e == gFm.bMc ? gFm.bOff : consoleOff;
}

// G1: the pure header OWNS the packet, including the 14-dword tiled sub-window the kernel constant
// repeats. Pin them equal so the host-tested arithmetic cannot drift from what the kext emits.
static_assert(RDNA4_SDMA_TILED_SUB_WINDOW_DWORDS == amdgpu::kSDMATiledSubWindowDwords,
              "sdma_gcr.h's tiled sub-window size must equal amdgpu_sdma.h's");

// 0.0.338 — THE PATTERN IS SELF-ADDRESSING, so a wrong value NAMES the source pixel it came from.
// rdna4_pc_pixel puts bars[x/32] in the low 24 bits and (y & 0xff) in the top byte, so any dword read out of the
// destination decodes back to (source bar, source row) unless it is not a pattern value at all. Returns 0..7, or
// -1 when the low 24 bits are none of the eight bars. (Bar 7 is black, so a zeroed dword decodes as bar 7 —
// recorded in the notes as the one ambiguity in this decode, and it is not load-bearing for the displacement.)
static int scanout_bar_index(uint32_t v) {
	static const uint32_t bars[8] = { 0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
	                                  0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u };
	for (int b = 0; b < 8; b++) if ((v & 0x00ffffffu) == bars[b]) return b;
	return -1;
}

// All scanout consumers use the peer's verified primary and its atomic mode snapshot.
static uint32_t scanout_geometry(amdgpu::DeviceContext &dev, RDNA4ScanoutGeom *g) {
	bzero(g, sizeof(*g));
	if (!garnet_driver_pci()) return kScanStNoContext;
	IOService *primary = nullptr;
	rdna4peer::PrimaryView view {};
	if (!rdna4_display_primary(&primary, &view)) return kScanStNoFramebuffer;
	primary->release();
	g->fbPhys = view.consolePhysBase; g->fbLen = view.consoleLength;
	g->width = view.modeWidth; g->height = view.modeHeight;
	g->rowBytes = view.modeRowBytes; g->depth = view.modeDepth;
	g->bar0Phys = dev.bar0Phys; g->bar0Size = dev.bar0Size; g->vramBase = dev.vramBase;
	g->vramSize = dev.vramSizeBytes;
	return rdna4_scanout_geom_check(g, nullptr) == kScanGeomOk ? kScanStOk : kScanStGeom;
}

// build 0.0.514 B1/B4: the LIVE framebuffer size - RDNA4FB's Console,Width/Height through scanout_geometry,
// the one source every scanout path already reads. Read-only (registry properties; no register, no VRAM). 1 = both read and
// non-zero; else 0 and *w/*h are left as the caller set them (its fallback).
uint32_t rdna4_scanout_live_dims(uint32_t *w, uint32_t *h) {
	if (!w || !h || !gBringup.dev) return 0u;
	RDNA4ScanoutGeom g {};
	
	if (scanout_geometry(*gBringup.dev, &g) != kScanStOk || !g.width || !g.height) return 0u;
	*w = g.width; *h = g.height;
	return 1u;
}

// QUEUE0 must be up and idle: the engine's read pointer (write-back page, bytes) at our software wptr.
static uint32_t scanout_queue_check(const amdgpu::DeviceContext &dev, const amdgpu::SDMAInstance &inst,
                                    uint32_t *rptrOut) {
	*rptrOut = 0xffffffffu;
	if (!inst.inited || !inst.enabled) return kScanStQueue;
	const uint32_t wb = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBRptrOffset);
	*rptrOut = wb;
	// 0.0.343 — sdma-wptr-freerunning: inst.wptr no longer wraps at the ring size,
	// so BOTH sides of this comparison must be reduced to a ring index before they can be compared.
	// Comparing a free-running wptr against a masked rptr would report BUSY forever.
	if (((wb >> 2) & inst.ring_ptr_mask) != (inst.wptr & inst.ring_ptr_mask)) return kScanStQueueBusy;
	return kScanStOk;
}

// Emit `n` COPY_LINEAR packets (src/dst are MC addresses, `bytes` each) and one FENCE on SDMA0 QUEUE0,
// kick, poll up to `timeoutMs`. Caller holds gScanoutLock and has range-checked every address.
static uint32_t scanout_sdma_copies(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                    const uint64_t *srcMc, const uint64_t *dstMc, const uint32_t *bytes,
                                    uint32_t n, uint32_t fenceValue, uint32_t timeoutMs,
                                    uint64_t *usOut, uint32_t *lastFenceOut) {
	*usOut = 0; *lastFenceOut = 0;
	const uint32_t dwords = n * 8u + 4u;
	if (n == 0 || dwords > inst.ring_size_dwords / 2) return kScanStRing;
	uint32_t *pkt = static_cast<uint32_t *>(IOMalloc(dwords * sizeof(uint32_t)));
	if (!pkt) return kScanStAlloc;
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
		           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_LINEAR) | amdgpu::SDMA_PKT_HEADER_CPV(1);
		pkt[k++] = bytes[i] - 1;
		pkt[k++] = 0;
		pkt[k++] = static_cast<uint32_t>(srcMc[i]);
		pkt[k++] = static_cast<uint32_t>(srcMc[i] >> 32);
		pkt[k++] = static_cast<uint32_t>(dstMc[i]);
		pkt[k++] = static_cast<uint32_t>(dstMc[i] >> 32);
		pkt[k++] = 0;
	}
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	amdgpu::amdgpu_hdp_flush(dev);   // rule 39: the engine reads VRAM the CPU just wrote
	const uint32_t wrote = amdgpu::sdma_ring_write(dev, inst, pkt, k);
	IOFree(pkt, dwords * sizeof(uint32_t));
	if (wrote != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// =====================================================================================================
// build 0.0.496 (an internal design note) — SWITCH 63, THE RESIDENCY COPY THROUGH SDMA.
//
// amd/fastcopy.h holds the argument and every decision; this is its kernel half, beside the SDMA code it reuses:
//   - the staging buffer: ONE physically contiguous 1 MiB system-memory buffer (amdgpu::sysmem_alloc, snooped), bound into the
//     GART ONCE, the first time 63 is turned ON, through gmc_bind_existing - the csb page's own path (0.0.182: gart_bind_sysmem
//     hands out GART offset 0, Apple's write-back page) - and ONLY while the HIGH bump region is in force (rdna4-gart-high=1,
//     clear of Apple's pages); the bump offset before and after is logged. Never unbound and never freed.
//   - the positive control, at the first ON: a self-addressing pattern staged, copied by the SAME submission code into a scratch
//     buffer from OUR low allocator, read back IN FULL through the MM window; any wrong dword (or a fence that does not land)
//     latches the path OFF for the boot.
//   - the submission: COPY_LINEAR + FENCE (rdna4_fc_build_packet: scanout_sdma_copies' packet), on OUR SDMA0 QUEUE0 under
//     gScanoutLock (every QUEUE0 submission of ours is serialised by it), a fence in its OWN write-back dword
//     (RDNA4_FC_WB_FENCE_OFF 0x1C0, not pipeshim's kSDMAWBCSFenceOffset) with a value never used before, a bounded wait.
//   - LOCK ORDER: gFastCopyLock (staging fill through fence) THEN gScanoutLock (ring write + fence wait only). pipeshim never
//     takes gFastCopyLock. The MM verify runs after BOTH are released, against a snapshot taken under gFastCopyLock.
// Writes: the staging buffer's GART PTEs (once), the SDMA0 ring and doorbell, our scratch VRAM (control only) and the copy's own
// pre-checked VRAM range (by the engine). No register write is added; SDMA0_DCC_CNTL is READ per chunk (COMP_EN must be clear).
// =====================================================================================================
// (fastcopy.h included above)
static_assert(RDNA4_FC_SDMA_OP_COPY == amdgpu::SDMA_OP_COPY && RDNA4_FC_SDMA_OP_FENCE == amdgpu::SDMA_OP_FENCE &&
              RDNA4_FC_SDMA_SUBOP_COPY_LINEAR == amdgpu::SDMA_SUBOP_COPY_LINEAR,
              "fastcopy.h's packet must be amdgpu_sdma.h's (scanout_sdma_copies' COPY_LINEAR and FENCE)");
static_assert(RDNA4_FC_WB_FENCE_OFF >= amdgpu::kSDMAWBExtFenceOffset + 0x40u && RDNA4_FC_WB_FENCE_OFF + 4u <= amdgpu::kSDMAWBPageBytes,
              "the fast copy's fence dword is past every slot QUEUE0 and the external queues use, inside the write-back page");

static IOLock          *gFastCopyLock { nullptr };   // staging fill through fence (then gScanoutLock inside it)
static volatile uint32_t gFcMode { 0u };             // 0 OFF (boot), RDNA4_FC_M_SAMPLED or RDNA4_FC_M_FULL
// build 0.0.527 (gfx_sk82.h): switch 63's setting, for switch 82's state snapshot. Read only.
uint32_t rdna4_fc_mode_now(void) { return rdna4_fc_mode_of(__atomic_load_n(&gFcMode, __ATOMIC_RELAXED)); }
static rdna4_fc_state     gFc {};                      // under gFastCopyLock
static rdna4_fc_stats     gFcS {};                     // chunk totals under gFastCopyLock; fallbacks atomic
static amdgpu::SysMem   gFcStaging {};
static uint64_t         gFcStagingMc { 0 }, gFcBumpBefore { 0 }, gFcBumpAfter { 0 };
static uint32_t         gFcPcWrong { 0 }, gFcPcDone { 0 }, gFcLogN { 0 };
static uint64_t         gFcPcUs { 0 };
static uint64_t         gFcPcOff { 0 };           // build 0.0.534 (review M1): 63's control scratch (VRAM offset; 0 = none)
static volatile uint32_t gFcEverOn { 0u };           // 0.0.509: set by the first ON; until then the scope's close asks nothing

// Per-thread copy slots: the switch LATCHED at a copy's first chunk (pos 0) holds for its later chunks, and the copy's own
// totals feed its `via SDMA` line. Only the owning thread touches a slot's fields; claimed by compare-and-swap.
struct FcSlot {
    uintptr_t thread;
    uint32_t  mode, chunksSdma, chunksMm, lastWhy;
    uint64_t  stageUs, dmaUs, verifyUs, compared, bytes, mismatched;
    // build 0.0.509: the copy's first chunk's VRAM offset (its scope's close is matched by it), the gScanoutLock wait of its
    // submissions (item 1), and its verify's wait for gVramMmLock apart from its reads (item 4).
    uint64_t  at0, lockUs, vLockUs, vReadUs;
    // build 0.0.535 item 3: the COPIED # this copy will print if no other copy completes first (latched at its first chunk)
    uint64_t  copyNo;
};
static constexpr uint32_t kFcSlots = 16u;
static FcSlot gFcSlot[kFcSlots];

static IOLock *fc_lock_get() {
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	if (l) return l;
	IOLock *mine = IOLockAlloc();
	if (!mine) return nullptr;
	IOLock *want = nullptr;
	if (!__atomic_compare_exchange_n(&gFastCopyLock, &want, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { IOLockFree(mine); return want; }
	return mine;
}

static FcSlot *fc_slot_mine() {
	const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
	for (uint32_t i = 0; i < kFcSlots; i++)
		if (__atomic_load_n(&gFcSlot[i].thread, __ATOMIC_ACQUIRE) == me) return &gFcSlot[i];
	return nullptr;
}

// build 0.0.510 A1: the copy guard's poison table asked for the copy's range [lo, hi) - any live row (VALID, STICKY or being
// written) overlapping it. gfx_copyguard.h's rdna4_cg_poison_overlaps over this file's own gCgPoison; read only.
static uint32_t fc_cg_poisoned(uint64_t, uint64_t) { return 0u; }

// The latch. pos 0 (a copy's first chunk): read the switch ONCE; ON claims (or reuses) this thread's slot and resets its totals;
// OFF releases any slot this thread still holds (0.0.509: the copy scope's close releases it on every path; this remains the
// backstop). pos > 0: this thread's slot, whatever the switch reads now. nullptr = OFF for this chunk.
// build 0.0.510 A1: the copy's range whose copy-guard poison also forces FULL verify, so the retry of a copy a non-latching
// failure poisoned can clear it. build 0.0.511 (LOW-3): that range is the copy's WHOLE VRAM range [cgLo, cgHi)
// (the pre-flight's bounding range, the copy guard's own scope), not the first chunk's [dAt, dAt + wBytes): a copy whose later
// segment lies elsewhere in VRAM is asked over every byte it writes.
// build 0.0.521 Part D: `rpCand` - the copy is a resprov candidate (the copier's `retile`): FULL verify.
static volatile uint64_t gFcCopySeq { 0u };
static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {
	FcSlot *s = fc_slot_mine();
	if (pos != 0u) return s;
	const uint32_t mode = rdna4_fc_mode_of(__atomic_load_n(&gFcMode, __ATOMIC_ACQUIRE));
	if (!mode) {
		if (s) __atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);
		return nullptr;
	}
	if (!s) {
		const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
		for (uint32_t i = 0; i < kFcSlots && !s; i++) {
			uintptr_t want = 0;
			if (__atomic_compare_exchange_n(&gFcSlot[i].thread, &want, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) s = &gFcSlot[i];
		}
		if (!s) { __atomic_fetch_add(&gFcS.fallback[RDNA4_FC_MM_NO_SLOT], 1ull, __ATOMIC_RELAXED); return nullptr; }
	}
	const uint32_t cgp = fc_cg_poisoned(cgLo, cgHi);
	const uint32_t eff = rdna4_fc_latch_mode(mode, 0u, cgp, rpCand);
	if (eff != mode) __atomic_fetch_add(cgp ? &gFcS.forced_full_cg : &gFcS.forced_full_rp, 1ull, __ATOMIC_RELAXED);
	__atomic_fetch_add(eff == RDNA4_FC_M_FULL ? &gFcS.latched_full : &gFcS.latched_sampled, 1ull, __ATOMIC_RELAXED);   // 0.0.521 D
	s->mode = eff; s->chunksSdma = s->chunksMm = s->lastWhy = 0u;
	s->stageUs = s->dmaUs = s->verifyUs = s->compared = s->bytes = s->mismatched = 0ull;
	s->at0 = ~0ull; s->lockUs = s->vLockUs = s->vReadUs = 0ull;
	s->copyNo = __atomic_add_fetch(&gFcCopySeq, 1ull, __ATOMIC_RELAXED);
	return s;
}

static uint64_t fc_us(uint64_t a, uint64_t b) { uint64_t ns = 0; absolutetime_to_nanoseconds(b - a, &ns); return ns / 1000u; }

// SDMA0 QUEUE0 up and idle, and the MC base the scanout path proved. Takes gScanoutLock briefly (inside gFastCopyLock).
static uint32_t fc_sdma_up() {
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) return 0u;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	if (!inst.inited || !inst.enabled || !inst.wb_bus || gBringup.gmc.vram_start != dev.vramMcBase) return 0u;
	IOLockLock(gScanoutLock);
	uint32_t rptr = 0;
	const uint32_t st = scanout_queue_check(dev, inst, &rptr);
	IOLockUnlock(gScanoutLock);
	return st == kScanStOk ? 1u : 0u;
}

// SDMA0_DCC_CNTL as it reads now (a READ; 0xffffffff when it cannot be read, which has every COMP_EN bit set: refused).
static uint32_t fc_dcc_raw() {
	if (!gBringup.dev) return 0xffffffffu;
	const uint32_t gc = rdna4_gc_base();
	return gc ? rdna4_reg_read32(gc + RDNA4_SDMA_DCC_CNTL_OFF) : 0xffffffffu;
}

// The submission context: which destination check applies (a residency copy: the VRAM guard; the control: our scratch only).
struct FcSub {
	uint64_t dAt;          // the chunk's 0-based VRAM offset
	uint64_t pcLo, pcHi;   // control only: the scratch buffer [pcLo, pcHi) (0 = a residency copy)
	uint64_t tSubmit;      // uptime at the submission's entry (the end of the staging fill)
	uint64_t dmaUs;        // 0.0.509: the fence wait counted FROM THE KICK (0.0.508: from the entry, lock wait included)
	uint64_t lockUs;       // 0.0.509: the wait for gScanoutLock (lock taken minus entry)
	uint32_t lastFence;
	RDNA4FcFillFn fill; void *fillCtx;
};

// build 0.0.509 item 1: fastcopy.h's rdna4_fc_submit_wait holds the ORDER (entry, gScanoutLock, the lock wait, the kick, THEN
// the bound's clock); these are its kernel callbacks. fc_kick is 0.0.508's gate, packet, ring write and doorbell, unchanged.
static uint64_t fc_now_us(void *) { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }
static void fc_lock(void *) { IOLockLock(gScanoutLock); }
static void fc_unlock(void *) { IOLockUnlock(gScanoutLock); }
static uint32_t fc_fence_read(void *) { return amdgpu::sdma_wb_read32(*gBringup.dev, gBringup.sdma.instance[0], RDNA4_FC_WB_FENCE_OFF); }
static void fc_delay(void *, uint32_t d) { if (d >= 1000u) IOSleep(d / 1000u); else IODelay(d); }
static uint32_t fc_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	const uint32_t idle = (scanout_queue_check(dev, inst, &rptr) == kScanStOk && inst.wb_bus &&
	                       gBringup.gmc.vram_start == dev.vramMcBase && dstMc == gBringup.gmc.vram_start + c->dAt) ? 1u : 0u;
	// THE LAST GATE: the VRAM guard for exactly this chunk (a residency copy), or our own scratch (the control).
	const uint32_t destWhy = c->pcHi ? ((c->dAt >= c->pcLo && c->dAt + bytes <= c->pcHi) ? 0u : 99u)
	                                 : rdna4_vram_apple_dest_check(c->dAt, bytes);
	if (!rdna4_fc_submit_ok(destWhy, idle)) return RDNA4_FC_SUB_REFUSED;
	uint32_t pkt[RDNA4_FC_PKT_DWORDS];
	const uint32_t k = rdna4_fc_build_packet(pkt, RDNA4_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + RDNA4_FC_WB_FENCE_OFF, fence);
	if (k != RDNA4_FC_PKT_DWORDS) return RDNA4_FC_SUB_REFUSED;
	amdgpu::sysmem_wmb();            // the staged bytes are in memory before the engine is told to read them (snooped GART)
	amdgpu::amdgpu_hdp_flush(dev);   // as scanout_sdma_copies
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return RDNA4_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return RDNA4_FC_SUB_RING;
	return 0u;
}
static const rdna4_fc_wait_ops kFcWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc_kick, &fc_fence_read, &fc_delay };

static uint32_t fc_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	uint64_t t0 = 0; clock_get_uptime(&t0);
	c->tSubmit = t0; c->dmaUs = 0; c->lastFence = 0; c->lockUs = 0;
	if (!gBringup.dev || !gScanoutLock) return RDNA4_FC_SUB_REFUSED;
	rdna4_fc_wait_out w {};
	const uint32_t r = rdna4_fc_submit_wait(&kFcWaitOps, c, srcMc, dstMc, bytes, fence, &w);
	c->lockUs = w.lock_us;
	if (w.kicked) { c->dmaUs = w.el_us; c->lastFence = w.seen; }
	return r;
}

static int fc_fill(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {
	FcSub *c = static_cast<FcSub *>(vc);
	return c->fill ? c->fill(c->fillCtx, dst, off, take) : 0;
}
// build 0.0.509 item 4: the verify's read. rdna4_vram_read_mm's body (bounds, the MM-window priority yield, gVramMmLock,
// the reads), with the wait for gVramMmLock - the MM-priority yield included - timed apart from the reads. Only the fast copy's
// verify calls it (switch 63 ON); rdna4_vram_read_mm itself is unchanged.
struct FcVerifyT { uint64_t lockT, readT; };   // uptime ticks (whole-us rounding per 1-dword read would lose the answer)
static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {
	if (!gBringup.dev || !dst || dwords == 0 || dwords > 64) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	uint64_t w0 = 0ull, w1 = 0ull, r1 = 0ull;
	clock_get_uptime(&w0);
	if (gVramMmLock) IOLockLock(gVramMmLock);
	clock_get_uptime(&w1);
	for (uint32_t i = 0; i < dwords; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4);
	clock_get_uptime(&r1);
	if (gVramMmLock) IOLockUnlock(gVramMmLock);
	if (t) { t->lockT += w1 - w0; t->readT += r1 - w1; }
	return true;
}
static int fc_mm_read(void *vt, uint64_t vram, uint32_t *dst, uint32_t dwords) {
	return fc_vram_read_timed(vram, dst, dwords, static_cast<FcVerifyT *>(vt)) ? 1 : 0;
}
static bool fc_vram_read_sampled_timed(uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *dst, uint32_t ns, FcVerifyT *t) {
	if (!gBringup.dev || !dst || !gVramMmLock) return false;
	if ((dAt & 3) != 0 || n == 0u || (n & 3u) != 0u) return false;
	if (ns == 0u || ns > RDNA4_FC_SAMPLE_MAX || ns > rdna4_fc_sample_count(n)) return false;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (dAt > size || (uint64_t)n > size - dAt) return false;
	uint64_t w0 = 0ull, w1 = 0ull, r1 = 0ull;
	clock_get_uptime(&w0);
	IOLockLock(gVramMmLock);
	clock_get_uptime(&w1);
	for (uint32_t i = 0; i < ns; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, dAt + rdna4_fc_sample_at(n, rot, i));
	clock_get_uptime(&r1);
	IOLockUnlock(gVramMmLock);
	if (t) { t->lockT += w1 - w0; t->readT += r1 - w1; }
	return true;
}
static int fc_mm_read_plan(void *vt, uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
	return fc_vram_read_sampled_timed(dAt, n, rot, got, ns, static_cast<FcVerifyT *>(vt)) ? 1 : 0;
}

// The control's pattern: self-addressing (the low 24 bits name the dword), top byte 0xA5 - never the 0xDEADBEEF poison.
static inline uint32_t fc_pc_word(uint32_t i) { return 0xA5000000u | (i & 0x00FFFFFFu); }
static int fc_pc_fill(void *, uint8_t *dst, uint64_t off, uint32_t take) {
	for (uint32_t i = 0; i < take; i += 4u) { const uint32_t w = fc_pc_word((uint32_t)((off + i) / 4u)); memcpy(dst + i, &w, 4); }
	return 1;
}

// Staging, once (under gFastCopyLock). 0 = bound; else why not (logged).
static uint32_t fc_staging_bind() {
	if (gFc.staging_ok) return 0u;
	if (!gBringup.dev || !gBringup.gfxhubReady || !gBringup.gmc.inited) { RDNA4LOG("fastcopy63: STAGING REFUSED - no GART yet"); return 1u; }
	if (!gBringup.gmc.gart_high_bump) {
		RDNA4LOG("fastcopy63: STAGING REFUSED - the GART bump region is LOW (rdna4-gart-high is not 1): the staging buffer would be "
		       "bound among Apple's pages. The path stays OFF (every copy keeps the MM window)");
		return 2u;
	}
	if (amdgpu::sysmem_alloc(gFcStaging, RDNA4_FC_STAGING_BYTES, 4096) != KERN_SUCCESS || !gFcStaging.valid()) {
		RDNA4LOG("fastcopy63: STAGING REFUSED - no physically contiguous %u-byte system-memory buffer", RDNA4_FC_STAGING_BYTES);
		return 3u;
	}
	gFcBumpBefore = gBringup.gmc.gart_bump_offset;
	uint64_t mc = 0;
	const kern_return_t kr = amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFcStaging.bus, RDNA4_FC_STAGING_BYTES, &mc);
	gFcBumpAfter = gBringup.gmc.gart_bump_offset;
	if (kr != KERN_SUCCESS || !mc || mc == gBringup.gmc.gart_start) {
		RDNA4LOG("fastcopy63: STAGING REFUSED - gmc_bind_existing %#x, MC %#llx (GART page 0 is Apple's write-back page); the buffer "
		       "is kept, unbound", (unsigned)kr, (unsigned long long)mc);
		return 4u;
	}
	gFcStagingMc = mc;
	gFc.staging_ok = 1u;
	RDNA4LOG("fastcopy63: STAGING bound - %u bytes, sysmem bus %#llx cpu %p -> GART MC %#llx (%u PTEs, SYSMEM_RW, snooped); GART bump "
	       "offset (nextFreeOffset) %#llx -> %#llx of %#llx, bump floor %#llx (HIGH)", RDNA4_FC_STAGING_BYTES,
	       (unsigned long long)gFcStaging.bus, gFcStaging.cpu, (unsigned long long)mc, RDNA4_FC_STAGING_BYTES / 4096u,
	       (unsigned long long)gFcBumpBefore, (unsigned long long)gFcBumpAfter, (unsigned long long)gBringup.gmc.gart_size,
	       (unsigned long long)gBringup.gmc.gart_bump_start);
	return 0u;
}

// THE POSITIVE CONTROL (under gFastCopyLock). 0 = passed.
static uint32_t fc_positive_control() {
	gFcPcDone = 1u;
	amdgpu::VRAMAllocation a {};
	bool have = false;
	uint64_t t0 = 0; clock_get_uptime(&t0);
	if (!gBringup.dev || !gScanoutLock) return 1u;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	IOLockLock(gScanoutLock);
	have = gBringup.gmc.vram_alloc.alloc(RDNA4_FC_STAGING_BYTES, 65536, &a);
	IOLockUnlock(gScanoutLock);
	if (!have) { RDNA4LOG("fastcopy63: CONTROL not run - no %u-byte scratch in our allocator", RDNA4_FC_STAGING_BYTES); return 2u; }
	const uint64_t off = a.gpu_va - gBringup.gmc.vram_start;
	gFcPcOff = off;                                         // build 0.0.534 (review M1): switch 89's control line names it
	if (!amdgpu::bar0_range_ok(dev, off, RDNA4_FC_STAGING_BYTES)) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		RDNA4LOG("fastcopy63: CONTROL not run - scratch VRAM %#llx is outside BAR0", (unsigned long long)off);
		return 3u;
	}
	for (uint32_t i = 0; i < RDNA4_FC_STAGING_BYTES; i += 4u) amdgpu::WBAR0_32(dev, off + i, 0xdeadbeefu);   // our scratch only
	amdgpu::amdgpu_hdp_flush(dev);
	FcSub sc {}; sc.dAt = off; sc.pcLo = off; sc.pcHi = off + RDNA4_FC_STAGING_BYTES; sc.fill = &fc_pc_fill; sc.fillCtx = nullptr;
	const rdna4_fc_ops ops { &sc, &fc_fill, &fc_submit };
	uint32_t *exp = static_cast<uint32_t *>(IOMalloc(RDNA4_FC_STAGING_BYTES));
	if (!exp) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		return 4u;
	}
	const uint32_t r = rdna4_fc_chunk_run(&ops, &gFc, static_cast<uint8_t *>(gFcStaging.cpu), gFcStagingMc, a.gpu_va,
	                                    RDNA4_FC_STAGING_BYTES, exp, RDNA4_FC_M_FULL, 0u, 0xffffffffull, nullptr);
	uint64_t cmp = 0; uint32_t rf = 0, first[4] = { 0 }, firstIdx[4] = { 0 }, nf = 0;
	gFcPcWrong = 0;
	if (r == RDNA4_FC_RUN_OK) {
		for (uint32_t i = 0; i < RDNA4_FC_STAGING_BYTES / 4u; i += 64u) {
			uint32_t buf[64];
			if (!rdna4_vram_read_mm(off + (uint64_t)i * 4u, buf, 64u)) { rf = 1u; break; }
			for (uint32_t j = 0; j < 64u; j++) {
				if (buf[j] == fc_pc_word(i + j) && buf[j] == exp[i + j]) continue;
				if (nf < 4u) { first[nf] = buf[j]; firstIdx[nf] = i + j; nf++; }
				gFcPcWrong++;
			}
			cmp += 64u;
		}
	}
	IOFree(exp, RDNA4_FC_STAGING_BYTES);
	uint64_t t1 = 0; clock_get_uptime(&t1);
	gFcPcUs = fc_us(t0, t1);
	// the scratch goes back only when the engine is known to be done with it (a landed fence)
	if (r == RDNA4_FC_RUN_OK) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); }
	const bool pass = (r == RDNA4_FC_RUN_OK) && !rf && cmp == RDNA4_FC_STAGING_BYTES / 4u && gFcPcWrong == 0u;
	RDNA4LOG("fastcopy63: POSITIVE CONTROL %s - %u-byte pattern staged at GART MC %#llx, SDMA0 QUEUE0 COPY_LINEAR -> our scratch "
	       "VRAM %#llx, fence %#010x (run %u, dma %llu us); MM-window read-back %llu of %u dwords, %u WRONG%s; first wrong "
	       "[%u]=%08x [%u]=%08x; %llu us in all", pass ? "PASSED" : "FAILED - the SDMA path is LATCHED OFF for this boot",
	       RDNA4_FC_STAGING_BYTES, (unsigned long long)gFcStagingMc, (unsigned long long)off, sc.lastFence, r,
	       (unsigned long long)sc.dmaUs, (unsigned long long)cmp, RDNA4_FC_STAGING_BYTES / 4u, gFcPcWrong,
	       rf ? " (the MM window refused a read)" : "", firstIdx[0], first[0], firstIdx[1], first[1], (unsigned long long)gFcPcUs);
	if (pass) { gFc.pc_passed = 1u; return 0u; }
	rdna4_fc_latch(&gFc, RDNA4_FC_LATCH_PC);
	return 5u;
}

// Fastcopy mode 63 verb (`M` = 1, 2, 3). 0 = done; 9 = the lock
// could not be allocated (unchanged); 12 = the switch is set but the path cannot copy (no staging, or the control did not pass).
uint32_t rdna4_fc_set(uint32_t m) {
	if (m == RDNA4_FC_M_OFF) { __atomic_store_n(&gFcMode, 0u, __ATOMIC_RELEASE); return 0u; }
	if (!rdna4_fc_mode_of(m)) return 11u;
	IOLock *l = fc_lock_get();
	if (!l) return 9u;
	IOLockLock(l);
	__atomic_store_n(&gFcEverOn, 1u, __ATOMIC_RELEASE);     // 0.0.509: from now on the copy scope's close asks the slot
	uint32_t st = 0u;
	if (!gFc.latched_off) {
		if (fc_staging_bind() != 0u) st = 12u;
		else if (!gFc.pc_passed && fc_positive_control() != 0u) st = 12u;
	} else st = 12u;
	__atomic_store_n(&gFcMode, m, __ATOMIC_RELEASE);
	IOLockUnlock(l);
	return st;
}

// =====================================================================================================
// build 0.0.534 ( ranked item 1; amd/fastcopy89.h) — SWITCH 89, A FULL CHUNK VERIFIED BY AN SDMA READ-BACK.
// fastcopy89.h holds the argument and the order (rdna4_fc89_chunk); this is its kernel half:
//   - the read-back buffer: ONE more physically contiguous 1 MiB system-memory buffer (amdgpu::sysmem_alloc: cached, snooped),
//     bound into the GART ONCE by gmc_bind_existing at the first `gfxneuter 89 | 1 << 8` - on the VERB thread, never in the copy
//     path - only while the HIGH bump region is in force, exactly as 63's staging buffer (fc_staging_bind). Never unbound, never freed.
//   - its positive control at that first ON: a self-addressing pattern written into scratch VRAM from OUR low allocator through BAR0
//     (63's control's own pattern and writes), the buffer poisoned with the pattern's complement, the scratch copied back by the SAME
//     read-back submission, every dword compared; a wrong dword or a fence that did not land retires the buffer for the boot.
//   - the read-back submission (fc89_kick): fc_kick's gate with the directions swapped - the SOURCE must be vram_start + the chunk's
//     own offset, the DESTINATION exactly the read-back buffer's MC and no longer than it - then fastcopy.h's packet, the ring write
//     and the doorbell on OUR SDMA0 QUEUE0 under gScanoutLock, the fence in 63's own write-back dword with a value never used before.
//   - LOCK ORDER: gFastCopyLock (poison through compare) THEN gScanoutLock (ring write + fence wait), as 63. The MM cross-check runs
//     after gFastCopyLock is released (fastcopy89.h step 5), through 63's one-acquire sampled read (fc_vram_read_sampled_timed).
// Writes: the read-back buffer's GART PTEs (once), our scratch VRAM (control only), the SDMA0 ring and doorbell. The engine writes only
// the read-back buffer (and the fence dword). No register write is added; SDMA0_DCC_CNTL is READ before each read-back.
// =====================================================================================================
// (fastcopy89.h included above)
static volatile uint32_t gFc89Mode { 0u };           // switch 89: 0 OFF (boot, default), RDNA4_FC89_M_ON (1) or RDNA4_FC89_M_SHADOW (3)
static uint64_t          gFc89PcOff { 0 };           // the previous switch-89 control's scratch (VRAM offset; 0 = none)
// 0.0.534 review item 2: rdna4_fc_chunk reads the mode ONCE per chunk, BEFORE its allocation and gFastCopyLock, and hands it on.
static uint32_t fc89_mode_now() { return rdna4_fc89_mode_of(__atomic_load_n(&gFc89Mode, __ATOMIC_ACQUIRE)); }
static rdna4_fc89_state    gFc89 {};                   // under gFastCopyLock
static rdna4_fc89_stats    gFc89S {};                  // under gFastCopyLock
static amdgpu::SysMem    gFc89Rb {};
static uint64_t          gFc89RbMc { 0 };
static uint32_t          gFc89LogN { 0 };

// build 0.0.535 item 3 (; fastcopy89.h rdna4_fc89_qwait): the bounded queue-idle wait's callbacks, and what the last
// kick answered (under gFastCopyLock, like gFc89S): the NOT TAKEN reason and the chunk's wait.
static uint32_t gFc89NtWhy { RDNA4_FC89_NT_OTHER };
static uint64_t gFc89WaitUs { 0 };
static uint32_t gFc89NtLines { 0 };
static uint32_t fc89_q_idle(void *) {
	uint32_t r = 0;
	return scanout_queue_check(*gBringup.dev, gBringup.sdma.instance[0], &r) == kScanStOk ? 1u : 0u;
}
static const rdna4_fc89_qwait_ops kFc89QWait = { nullptr, &fc89_q_idle, &fc_now_us, &fc_delay };
static uint32_t fc89_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	uint32_t qs = scanout_queue_check(dev, inst, &rptr);
	gFc89WaitUs = 0u;
	// build 0.0.535 item 3: QUEUE0 not idle right after the forward write's fence - wait for it, bounded (<= 2 ms, counted).
	if (qs == kScanStQueueBusy) {
		const uint32_t ok = rdna4_fc89_qwait(&kFc89QWait, RDNA4_FC89_QWAIT_US, &gFc89WaitUs);
		rdna4_fc89_qwait_note(&gFc89S, ok, gFc89WaitUs);
		if (ok) qs = kScanStOk;
	}
	gFc89NtWhy = qs == kScanStQueueBusy ? RDNA4_FC89_NT_QBUSY : RDNA4_FC89_NT_OTHER;
	const uint32_t idle = (qs == kScanStOk && inst.wb_bus &&
	                       gBringup.gmc.vram_start == dev.vramMcBase && srcMc == gBringup.gmc.vram_start + c->dAt) ? 1u : 0u;
	// THE LAST GATE: the engine may write ONLY our read-back buffer, and no more of it than it holds.
	const uint32_t destWhy = (gFc89Rb.valid() && gFc89RbMc && dstMc == gFc89RbMc && bytes != 0u && bytes <= RDNA4_FC_STAGING_BYTES) ? 0u : 98u;
	if (rdna4_fc_submit_ok(destWhy, idle) != 1u) return RDNA4_FC_SUB_REFUSED;
	uint32_t pkt[RDNA4_FC_PKT_DWORDS];
	const uint32_t k = rdna4_fc_build_packet(pkt, RDNA4_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + RDNA4_FC_WB_FENCE_OFF, fence);
	if (k != RDNA4_FC_PKT_DWORDS) return RDNA4_FC_SUB_REFUSED;
	amdgpu::sysmem_wmb();            // the poison is in memory before the engine is told to overwrite it (snooped GART)
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return RDNA4_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return RDNA4_FC_SUB_RING;
	return 0u;
}
static const rdna4_fc_wait_ops kFc89WaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc89_kick, &fc_fence_read, &fc_delay };
// The chunk's callbacks (fastcopy89.h rdna4_fc89_ops): `sub` the read-back's submission record, `l` gFastCopyLock, `vt` the cross-check's
// MM-lock / read timing.
struct Fc89Ctx { FcSub sub; IOLock *l; FcVerifyT vt; };
static uint32_t fc89_wait(FcSub *c, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	uint64_t t0 = 0; clock_get_uptime(&t0);
	c->tSubmit = t0; c->dmaUs = 0; c->lastFence = 0; c->lockUs = 0;
	if (!gBringup.dev || !gScanoutLock) return RDNA4_FC_SUB_REFUSED;
	rdna4_fc_wait_out w {};
	const uint32_t r = rdna4_fc_submit_wait(&kFc89WaitOps, c, srcMc, dstMc, bytes, fence, &w);
	c->lockUs = w.lock_us;
	if (w.kicked) { c->dmaUs = w.el_us; c->lastFence = w.seen; }
	amdgpu::sysmem_rmb();            // the fence was read landed: the buffer's reads come after it
	return r;
}
static uint32_t fc89_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	return fc89_wait(&static_cast<Fc89Ctx *>(vc)->sub, srcMc, dstMc, bytes, fence);
}
static void fc89_unlock(void *vc) { IOLockUnlock(static_cast<Fc89Ctx *>(vc)->l); }
static int fc89_plan(void *vc, uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
	return fc_vram_read_sampled_timed(dAt, n, rot, got, ns, &static_cast<Fc89Ctx *>(vc)->vt) ? 1 : 0;
}

// The read-back buffer, once (under gFastCopyLock, on the verb thread). 0 = bound; else why not (logged).
static uint32_t fc89_rb_bind() {
	if (gFc89Rb.valid() && gFc89RbMc) return 0u;
	if (!gBringup.dev || !gBringup.gfxhubReady || !gBringup.gmc.inited) { RDNA4LOG("fastcopy89: READ-BACK BUFFER REFUSED - no GART yet"); return 1u; }
	if (!(gBringup.gmc.gart_high_bump)) {
		RDNA4LOG("fastcopy89: READ-BACK BUFFER REFUSED - the GART bump region is LOW (rdna4-gart-high is not 1): switch 89 stays inert");
		return 2u;
	}
	if (!gFc89Rb.valid() && (amdgpu::sysmem_alloc(gFc89Rb, RDNA4_FC_STAGING_BYTES, 4096) != KERN_SUCCESS || !gFc89Rb.valid())) {
		RDNA4LOG("fastcopy89: READ-BACK BUFFER REFUSED - no physically contiguous %u-byte system-memory buffer", RDNA4_FC_STAGING_BYTES);
		return 3u;
	}
	const uint64_t b0 = gBringup.gmc.gart_bump_offset;
	uint64_t mc = 0;
	const kern_return_t kr = amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFc89Rb.bus, RDNA4_FC_STAGING_BYTES, &mc);
	const uint64_t b1 = gBringup.gmc.gart_bump_offset;
	if (kr != KERN_SUCCESS || !mc || mc == gBringup.gmc.gart_start) {
		RDNA4LOG("fastcopy89: READ-BACK BUFFER REFUSED - gmc_bind_existing %#x, MC %#llx; the buffer is kept, unbound", (unsigned)kr,
		       (unsigned long long)mc);
		return 4u;
	}
	gFc89RbMc = mc;
	RDNA4LOG("fastcopy89: READ-BACK BUFFER bound - %u bytes, sysmem bus %#llx cpu %p -> GART MC %#llx (%u PTEs, SYSMEM_RW, snooped); GART "
	       "bump offset %#llx -> %#llx of %#llx", RDNA4_FC_STAGING_BYTES, (unsigned long long)gFc89Rb.bus, gFc89Rb.cpu,
	       (unsigned long long)mc, RDNA4_FC_STAGING_BYTES / 4096u, (unsigned long long)b0, (unsigned long long)b1,
	       (unsigned long long)gBringup.gmc.gart_size);
	return 0u;
}

// THE READ DIRECTION's POSITIVE CONTROL (under gFastCopyLock). 0 = passed (gFc89.rb_ok = 1). A wrong dword, or a fence that did not
// land (fastcopy.h rdna4_fc_fence_outcome: 63's path latched OFF too), retires the buffer for the boot; "not run" leaves it for a retry.
static uint32_t fc89_control() {
	if (!gBringup.dev || !gScanoutLock || !gFc89Rb.valid() || !gFc89RbMc) return 1u;
	if (!fc_sdma_up()) { RDNA4LOG("fastcopy89: CONTROL not run - SDMA0 QUEUE0 not up or busy"); return 2u; }
	if (fc_dcc_raw() & RDNA4_DCC_NOPTE_COMP_EN_MASK) { RDNA4LOG("fastcopy89: CONTROL not run - SDMA0_DCC_CNTL has a COMP_EN bit set"); return 3u; }
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::VRAMAllocation a {};
	IOLockLock(gScanoutLock);
	const bool have = gBringup.gmc.vram_alloc.alloc(RDNA4_FC_STAGING_BYTES, 65536, &a);
	IOLockUnlock(gScanoutLock);
	if (!have) { RDNA4LOG("fastcopy89: CONTROL not run - no %u-byte scratch in our allocator", RDNA4_FC_STAGING_BYTES); return 4u; }
	const uint64_t off = a.gpu_va - gBringup.gmc.vram_start;
	if (off + RDNA4_FC_STAGING_BYTES > dev.bar0Size) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		RDNA4LOG("fastcopy89: CONTROL not run - scratch VRAM %#llx is outside BAR0", (unsigned long long)off);
		return 5u;
	}
	const uint32_t fence = rdna4_fc_fence_next(&gFc);
	if (!fence) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); return 6u; }
	gFc89.pc_done = 1u; gFc89.pc_wrong = 0u;
	// 0.0.534 review M1: a FRESH pattern - never 63's (fc_pc_word) and never the previous switch-89 control's - so a read-back that
	// returned stale VRAM from either can never pass.
	const uint32_t nonce = rdna4_fc89_pc_nonce(fence, gFc89.pc_nonce);
	const uint64_t prevOff = gFc89PcOff;
	gFc89.pc_nonce = nonce; gFc89PcOff = off;
	uint64_t t0 = 0; clock_get_uptime(&t0);
	for (uint32_t i = 0; i < RDNA4_FC_STAGING_BYTES; i += 4u) amdgpu::WBAR0_32(dev, off + i, rdna4_fc89_pc_word(i / 4u, nonce));   // our scratch only
	amdgpu::amdgpu_hdp_flush(dev);   // as 63's control: the BAR0 pattern is in VRAM before the engine reads it
	uint32_t *rb = static_cast<uint32_t *>(gFc89Rb.cpu);
	for (uint32_t i = 0; i < RDNA4_FC_STAGING_BYTES / 4u; i++) rb[i] = ~rdna4_fc89_pc_word(i, nonce);
	FcSub sc {}; sc.dAt = off;
	const uint32_t sub = fc89_wait(&sc, a.gpu_va, gFc89RbMc, RDNA4_FC_STAGING_BYTES, fence);
	const uint32_t run = rdna4_fc_fence_outcome(&gFc, sub);
	uint32_t first[2] = { 0u, 0u }, firstIdx[2] = { 0u, 0u };
	if (run == RDNA4_FC_RUN_OK) gFc89.pc_wrong = rdna4_fc89_pc_check(rb, RDNA4_FC_STAGING_BYTES / 4u, nonce, firstIdx, first);
	uint64_t t1 = 0; clock_get_uptime(&t1);
	// the scratch goes back only when the engine is known to be done with it (a landed fence) or never told of it (refused)
	if (run == RDNA4_FC_RUN_OK || run == RDNA4_FC_RUN_REFUSED) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); }
	const bool pass = run == RDNA4_FC_RUN_OK && gFc89.pc_wrong == 0u;
	if (pass) gFc89.rb_ok = 1u;
	else if (run != RDNA4_FC_RUN_REFUSED) gFc89.rb_retired = 1u;
	RDNA4LOG("fastcopy89: POSITIVE CONTROL %s - pattern 0x3C|i^%#08x BAR0-written to scratch VRAM %#llx (63's control %#llx, previous "
	       "89 control %#llx), SDMA -> GART MC %#llx (poisoned), fence %#010x (run %u, dma %llu us); %u of %u WRONG; first [%u]=%08x "
	       "[%u]=%08x; %llu us", pass ? "PASSED" : run == RDNA4_FC_RUN_REFUSED ? "NOT RUN - refused before the ring (retried at the next ON)"
	       : "FAILED - switch 89 stays inert this boot", nonce, (unsigned long long)off, (unsigned long long)gFcPcOff,
	       (unsigned long long)prevOff, (unsigned long long)gFc89RbMc, sc.lastFence, run, (unsigned long long)sc.dmaUs, gFc89.pc_wrong,
	       RDNA4_FC_STAGING_BYTES / 4u, firstIdx[0], first[0], firstIdx[1], first[1], (unsigned long long)fc_us(t0, t1));
	return pass ? 0u : 7u;
}

// Fastcopy mode 89 verb (`M` = 1, 2). 0 = done; 9 = no lock; 11 = an
// unknown M; 12 = the switch is set but the read-back cannot run (every FULL chunk keeps the MM verify).
uint32_t rdna4_fc89_set(uint32_t m) {
	if (m == RDNA4_FC89_M_OFF) { __atomic_store_n(&gFc89Mode, 0u, __ATOMIC_RELEASE); return 0u; }
	if (!rdna4_fc89_mode_of(m)) return 11u;
	IOLock *l = fc_lock_get();
	if (!l) return 9u;
	IOLockLock(l);
	uint32_t st = 0u;
	if (gFc89.rb_retired) st = 12u;
	else if (!gFc89.rb_ok) {
		if (fc89_rb_bind() != 0u) st = 12u;
		else if (fc89_control() != 0u) st = 12u;
	}
	__atomic_store_n(&gFc89Mode, m, __ATOMIC_RELEASE);   // RDNA4_FC89_M_ON or RDNA4_FC89_M_SHADOW
	IOLockUnlock(l);
	return st;
}

void rdna4_fc89_report(const char *why) {
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	if (l) IOLockLock(l);
	RDNA4LOG(RDNA4_FC89_FMT, RDNA4_FC89_ARGS(fc89_mode_now(), why, &gFc89, &gFc89S));
	RDNA4LOG(RDNA4_FC89_SH_FMT, (unsigned long long)gFc89S.sh_chunks, (unsigned long long)gFc89S.sh_agree,
	       (unsigned long long)gFc89S.sh_disagree, (unsigned long long)gFc89S.sh_dis_dwords, (unsigned long long)gFc89S.sh_noview,
	       (unsigned long long)gFc89S.sh_rb_bad, (unsigned long long)gFc89S.sh_mm_bad);
	RDNA4LOG(RDNA4_FC89_QW_FMT, RDNA4_FC89_QWAIT_US, (unsigned long long)gFc89S.qwaits, (unsigned long long)gFc89S.qwait_idle,   // 0.0.535
	       (unsigned long long)gFc89S.qwait_busy, (unsigned long long)gFc89S.qwait_max_us, (unsigned long long)gFc89S.nt_qbusy,
	       (unsigned long long)gFc89S.nt_other, gFc89NtLines, RDNA4_FC89_NT_LINES);
	if (l) IOLockUnlock(l);
}

// ONE FULL CHUNK WHOSE WRITE LANDED (rdna4_fc_chunk, under gFastCopyLock `l`). 0 = NOT TAKEN, `l` STILL HELD, nothing freed:
// rdna4_fc_chunk's own verify runs exactly as 0.0.533's. Else the chunk's result word with `l` RELEASED and `exp` freed.
// `m89`: the mode rdna4_fc_chunk read BEFORE its allocation; `exp` then carries rdna4_fc89_extra(m89, n) bytes past the n / 4
// snapshot dwords (0.0.534 review item 2: nothing is allocated here, under gFastCopyLock).
static __attribute__((noinline)) uint64_t fc89_shadow(FcSlot *s, IOLock *l, uint64_t dAt, uint64_t n, uint32_t *exp, size_t expBytes,
                                                      uint32_t rot, uint64_t stageUs, const FcSub *wsc);
// build 0.0.535 item 3: ONE CHUNK NOT TAKEN (under gFastCopyLock): counted by reason, and named - copy #, chunk
// index (the copy loop's chunks are RDNA4_FC_STAGING_BYTES apart), VRAM range, mode, reason, the chunk's queue wait - for the first
// RDNA4_FC89_NT_LINES of the boot.
static __attribute__((noinline)) void fc89_nt(const FcSlot *s, uint64_t pos, uint64_t dAt, uint64_t n, uint32_t m89, uint32_t why,
                                              uint64_t waitUs) {
	if (why == RDNA4_FC89_NT_QBUSY) gFc89S.nt_qbusy++; else gFc89S.nt_other++;
	if (!rdna4_fc89_nt_line(&gFc89NtLines)) return;
	RDNA4LOG(RDNA4_FC89_NT_FMT, (unsigned long long)s->copyNo, (unsigned long long)(pos / RDNA4_FC_STAGING_BYTES), (unsigned long long)dAt,
	       (unsigned long long)n, m89 == RDNA4_FC89_M_SHADOW ? "SHADOW" : "ON", rdna4_fc89_nt_name(why), (unsigned long long)waitUs,
	       (unsigned long long)gFc89S.qwait_max_us);
}
static __attribute__((noinline)) uint64_t fc89_chunk(FcSlot *s, IOLock *l, uint64_t pos, uint64_t dAt, uint64_t n, uint32_t *exp,
                                                     size_t expBytes, uint32_t rot, uint64_t stageUs, const FcSub *wsc, uint32_t m89) {
	if (!m89) return 0ull;                                                         // OFF: nothing read, nothing written
	if (!rdna4_fc89_use(1u, s->mode, &gFc89, gFc.latched_off) || (fc_dcc_raw() & RDNA4_DCC_NOPTE_COMP_EN_MASK) ||
	    expBytes < (size_t)(n + rdna4_fc89_extra(m89, n))) {
		gFc89S.not_taken++;                                                        // the read would decompress, or not usable
		fc89_nt(s, pos, dAt, n, m89, RDNA4_FC89_NT_OTHER, 0u);
		return 0ull;
	}
	gFc89NtWhy = RDNA4_FC89_NT_OTHER; gFc89WaitUs = 0u;                              // a refusal before the kick is "other"
	if (m89 == RDNA4_FC89_M_SHADOW) {
		const uint64_t rs = fc89_shadow(s, l, dAt, n, exp, expBytes, rot, stageUs, wsc);
		if (!rs) fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs);           // NOT TAKEN: `l` still held
		return rs;
	}
	uint32_t *got = exp + n / 4u;                                                  // allocated with exp, before the lock
	Fc89Ctx c {}; c.sub.dAt = dAt; c.l = l;
	const rdna4_fc89_ops ops { &c, &fc89_submit, &fc89_unlock, &fc89_plan };
	rdna4_fc89_out o {};
	uint64_t t0 = 0, t1 = 0; clock_get_uptime(&t0);
	const uint64_t r = rdna4_fc89_chunk(&ops, &gFc, &gFc89, static_cast<uint32_t *>(gFc89Rb.cpu), gFc89RbMc,
	                                  gBringup.gmc.vram_start + dAt, dAt, exp, got, (uint32_t)n, rot, &o);
	clock_get_uptime(&t1);
	if (!r) { gFc89S.not_taken++; fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs); return 0ull; }   // refused before its ring write: `l` held
	// `l` RELEASED by rdna4_fc89_chunk (step 5, or its failure exit).
	s->stageUs += stageUs; s->dmaUs += wsc->dmaUs; s->lockUs += wsc->lockUs;      // the WRITE's, as rdna4_fc_chunk adds them
	IOFree(exp, expBytes);
	const uint64_t vUs = fc_us(t0, t1), vLockUs = fc_us(0ull, c.vt.lockT), vReadUs = fc_us(0ull, c.vt.readT);
	if (o.run != RDNA4_FC89_OK) {
		__atomic_fetch_add(&gFcS.dead_ranges, 1ull, __ATOMIC_RELAXED);
		IOLockLock(l);
		if (o.run == RDNA4_FC89_TIMEOUT) gFc89S.timeouts++; else gFc89S.ring_fail++;
		IOLockUnlock(l);
		RDNA4LOG("fastcopy63: CHUNK FAILED at resource offset %#llx (VRAM %#llx, %#llx bytes): switch 89's READ-BACK run %u - %s; fence "
		       "want %#010x seen %#010x after %llu us from the kick; path LATCHED OFF for the boot; the range is a DEAD RANGE, poisoned "
		       "for the boot", (unsigned long long)pos, (unsigned long long)dAt, (unsigned long long)n, o.sub, rdna4_fc_fail_text(r),
		       o.fence, c.sub.lastFence, (unsigned long long)c.sub.dmaUs);
		return r;
	}
	const uint32_t failed = (r & RDNA4_FC_R_FAIL) ? 1u : 0u;                          // only the cross-check's refused MM read
	const uint64_t cmp = failed ? 0ull : ((r >> 32) & 0x3FFFFFFFull), bad = failed ? 0ull : (uint64_t)(uint32_t)r;
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	gFcS.full_dwords += cmp;
	if (o.xread_fail) { gFcS.verify_read_fail++; gFc89S.xread_fail++; }
	gFc89S.chunks++; gFc89S.bytes += n; gFc89S.mismatches += o.rb_bad; gFc89S.xmismatches += o.x_bad;
	gFc89S.us += vUs; gFc89S.dma_us += c.sub.dmaUs; gFc89S.xcheck_us += vLockUs + vReadUs;
	if (bad || o.rb_bad || o.x_bad) rdna4_fc_latch(&gFc, RDNA4_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it
	IOLockUnlock(l);
	if (bad || o.rb_bad || o.x_bad)
		RDNA4LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream (switch 89: SDMA read-back %llu, MM cross-check %llu of %u sampled): the copy is poisoned (read-back mismatch) and "
		       "the SDMA path is LATCHED OFF for the boot", (unsigned long long)(o.rb_bad + o.x_bad), (unsigned long long)(n / 4u),
		       (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)o.rb_bad, (unsigned long long)o.x_bad, o.ns);
	else if (gFc89LogN < 8u) {
		gFc89LogN++;
		RDNA4LOG("fastcopy89: chunk VRAM %#llx (%#llx bytes) READ BACK by SDMA in full (%llu dwords, dma %llu us) and cross-checked "
		       "through the MM window (%u sampled, %s): verify %llu us", (unsigned long long)dAt, (unsigned long long)n,
		       (unsigned long long)(n / 4u), (unsigned long long)c.sub.dmaUs, o.ns, o.xread_fail ? "READ REFUSED" : "match",
		       (unsigned long long)vUs);
	}
	return r;
}

// SWITCH 89 SHADOW (857; 0.0.534 review item 3). Under gFastCopyLock `l`: phase A (fastcopy89.h rdna4_fc89_shadow_a) - the read-back
// into the buffer, copied out to `sd` (= exp + n / 4, allocated before the lock); 0 = NOT TAKEN, `l` STILL HELD (today's path runs).
// Else `l` is released and TODAY'S full MM verify runs - rdna4_fc_verify over the same fc_mm_read, only wrapped by rdna4_fc89_shadow_read,
// which records where the MM and SDMA views differ - and rdna4_fc_chunk's own tail follows, deciding by the MM verify ALONE
// (rdna4_fc89_shadow_decide): the same credit, the same mismatch latch and line, the same verify-read failure.
static __attribute__((noinline)) uint64_t fc89_shadow(FcSlot *s, IOLock *l, uint64_t dAt, uint64_t n, uint32_t *exp, size_t expBytes,
                                                      uint32_t rot, uint64_t stageUs, const FcSub *wsc) {
	uint32_t *sd = exp + n / 4u;
	Fc89Ctx c {}; c.sub.dAt = dAt; c.l = l;
	const rdna4_fc89_ops ops { &c, &fc89_submit, &fc89_unlock, &fc89_plan };
	rdna4_fc89_out o {};
	uint32_t sdOk = 0u;
	if (!rdna4_fc89_shadow_a(&ops, &gFc, &gFc89, static_cast<uint32_t *>(gFc89Rb.cpu), gFc89RbMc, gBringup.gmc.vram_start + dAt, exp, sd,
	                       (uint32_t)n, &o, &sdOk)) { gFc89S.not_taken++; return 0ull; }
	// `l` RELEASED. From here: rdna4_fc_chunk's tail, with the verify's read wrapped.
	s->stageUs += stageUs; s->dmaUs += wsc->dmaUs; s->lockUs += wsc->lockUs;
	uint64_t v0 = 0, v1 = 0, cmp = 0; uint32_t rf = 0;
	FcVerifyT vt { 0ull, 0ull };
	rdna4_fc89_shadow_rec rec {};
	rec.read = &fc_mm_read; rec.ctx = &vt; rec.sd = sd; rec.sd_ok = sdOk; rec.nd = (uint32_t)(n / 4u); rec.d_at = dAt;
	clock_get_uptime(&v0);
	const uint64_t bad = rdna4_fc_verify(&rdna4_fc89_shadow_read, nullptr, &rec, dAt, exp, nullptr, (uint32_t)n, RDNA4_FC_M_FULL, rot,
	                                   &cmp, &rf);
	clock_get_uptime(&v1);
	IOFree(exp, expBytes);
	const uint64_t r = rdna4_fc89_shadow_decide(bad, cmp, rf, o.run, o.rb_bad);
	const uint64_t vUs = fc_us(v0, v1), vLockUs = fc_us(0ull, vt.lockT), vReadUs = fc_us(0ull, vt.readT);
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	gFcS.full_dwords += cmp;
	if (rf) gFcS.verify_read_fail++;
	if (bad) rdna4_fc_latch(&gFc, RDNA4_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it this boot
	gFc89S.sh_chunks++;
	if (!sdOk) gFc89S.sh_noview++;
	else if (rec.dis) { gFc89S.sh_disagree++; gFc89S.sh_dis_dwords += rec.dis; }
	else gFc89S.sh_agree++;
	gFc89S.sh_rb_bad += o.rb_bad; gFc89S.sh_mm_bad += bad;
	if (o.run == RDNA4_FC89_TIMEOUT) gFc89S.timeouts++; else if (o.run == RDNA4_FC89_RING) gFc89S.ring_fail++;
	gFc89S.dma_us += c.sub.dmaUs;
	const uint32_t logDis = rec.dis && gFc89LogN < 64u;
	if (logDis) gFc89LogN++;
	IOLockUnlock(l);
	if (bad)
		RDNA4LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream: the copy is poisoned (read-back mismatch) and the SDMA path is LATCHED OFF for the boot",
		       (unsigned long long)bad, (unsigned long long)cmp, (unsigned long long)dAt, (unsigned long long)n);
	if (logDis)
		RDNA4LOG(RDNA4_FC89_SH_DIS_FMT, (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)rec.dis,
		       (unsigned long long)rec.compared, rec.off[0], rec.mm[0], rec.sdv[0], rec.off[1], rec.mm[1], rec.sdv[1], rec.off[2],
		       rec.mm[2], rec.sdv[2], rec.off[3], rec.mm[3], rec.sdv[3]);
	if (o.run != RDNA4_FC89_OK)
		RDNA4LOG("fastcopy89: SHADOW read-back of VRAM %#llx (%#llx bytes) did not land (run %u, fence want %#010x seen %#010x): the SDMA "
		       "path is LATCHED OFF and the read-back buffer retired; this chunk was decided by the MM verify", (unsigned long long)dAt,
		       (unsigned long long)n, o.sub, o.fence, c.sub.lastFence);
	return r;
}

// ONE CHUNK (GarnetAccelPeer.cpp fc_copy_chunk, per chunk of the residency copy, AFTER the copy loop's own VRAM-guard check of
// exactly [dAt, dAt + n)). Returns 0 (the MM loop runs this chunk: nothing staged, nothing written) or RDNA4_FC_R_TAKEN with the
// result (fastcopy.h). A FAILED result poisons through the copy loop's own FAILED path.
uint64_t rdna4_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, RDNA4FcFillFn fill, void *fillCtx,
                         uint8_t *lead, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {
	FcSlot *s = fc_slot_latch(pos, cgLo, cgHi, rpCand);
	if (!s || !s->mode) return 0ull;                       // OFF (latched): the MM loop, exactly 0.0.495's
	if (pos == 0u) s->at0 = dAt;                           // 0.0.509: the scope's close finds this copy's slot by it
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	const uint32_t full = (s->mode == RDNA4_FC_M_FULL) ? 1u : 0u;
	const bool shapeOk = n != 0u && n <= RDNA4_FC_STAGING_BYTES && (n & 3u) == 0u;
	// 0.0.514 A1: sampled - the snapshot (RDNA4_FC_SAMPLE_MAX dwords) and, after it, the plan read's destination (as many again)
	const uint32_t m89 = full ? fc89_mode_now() : 0u;      // build 0.0.534 (switch 89): read once, before the allocation
	const size_t expBytes = full ? (size_t)(n + rdna4_fc89_extra(m89, n)) : (size_t)RDNA4_FC_SAMPLE_MAX * 8u;
	uint32_t *exp = (l && shapeOk) ? static_cast<uint32_t *>(IOMalloc(expBytes)) : nullptr;
	uint32_t why = RDNA4_FC_MM_NO_STAGING;
	if (l) {
		IOLockLock(l);
		why = rdna4_fc_decide(1u, fc_sdma_up(), fc_dcc_raw(), gFc.pc_passed, gFc.latched_off, rdna4_fc_staging_usable(&gFc), wBytes, dAt, n);
		if (why == RDNA4_FC_SDMA && !exp) why = RDNA4_FC_MM_NO_MEM;
		if (why != RDNA4_FC_SDMA) IOLockUnlock(l);
	}
	if (why != RDNA4_FC_SDMA) {
		if (exp) IOFree(exp, expBytes);
		s->chunksMm++; s->lastWhy = why;
		if (why < RDNA4_FC_MM_REASONS) __atomic_fetch_add(&gFcS.fallback[why], 1ull, __ATOMIC_RELAXED);
		return 0ull;
	}
	// gFastCopyLock held: the staging buffer is this chunk's alone until the fence has landed (or the path is latched off).
	uint64_t t0 = 0; clock_get_uptime(&t0);
	FcSub sc {}; sc.dAt = dAt; sc.fill = fill; sc.fillCtx = fillCtx;
	const rdna4_fc_ops ops { &sc, &fc_fill, &fc_submit };
	const uint32_t rot = (uint32_t)gFcS.chunks_sdma;
	const uint32_t r = rdna4_fc_chunk_run(&ops, &gFc, static_cast<uint8_t *>(gFcStaging.cpu), gFcStagingMc,
	                                    gBringup.gmc.vram_start + dAt, (uint32_t)n, exp, s->mode, rot, pos, lead);
	const uint64_t stageUs = sc.tSubmit ? fc_us(t0, sc.tSubmit) : 0ull;
	if (r == RDNA4_FC_RUN_OK) { gFcS.chunks_sdma++; gFcS.bytes_sdma += n; gFcS.stage_us += stageUs; gFcS.dma_us += sc.dmaUs; }
	else if (r == RDNA4_FC_RUN_FILL) gFcS.fill_fail++;
	else if (r == RDNA4_FC_RUN_REFUSED) gFcS.refused++;
	else if (r == RDNA4_FC_RUN_RING) gFcS.ring_fail++;
	if (sc.tSubmit) {                                      // 0.0.509 item 1: the submission's wait for gScanoutLock
		gFcS.lock_waits++; gFcS.lock_wait_us += sc.lockUs;
		if (sc.lockUs > gFcS.lock_wait_max_us) gFcS.lock_wait_max_us = sc.lockUs;
	}
	const uint32_t latched = gFc.latched_off, timeouts = gFc.timeouts, lastFence = sc.lastFence, fenceWant = gFc.fence_last;
	// build 0.0.534 (switch 89, fastcopy89.h): a FULL chunk whose write landed is verified by an SDMA read-back + a sampled MM
	// cross-check. fc89_chunk answers 0 with gFastCopyLock STILL HELD (89 OFF, not usable, refused): everything below is 0.0.533's.
	if (r == RDNA4_FC_RUN_OK && full) { const uint64_t r89 = fc89_chunk(s, l, pos, dAt, n, exp, expBytes, rot, stageUs, &sc, m89); if (r89) return r89; }
	IOLockUnlock(l);
	s->stageUs += stageUs; s->dmaUs += sc.dmaUs; s->lockUs += sc.lockUs;
	if (r != RDNA4_FC_RUN_OK) {
		IOFree(exp, expBytes);
		// build 0.0.510 A1: refused before the ring was written at the copy's FIRST chunk - nothing of this copy is written -
		// so the MM loop runs this chunk (a declined chunk) instead of failing the copy: no poison is left for a retry to clear.
		if (rdna4_fc_refused_to_mm(r, pos)) {
			__atomic_fetch_add(&gFcS.refused_mm, 1ull, __ATOMIC_RELAXED);
			s->chunksMm++; s->lastWhy = RDNA4_FC_MM_NO_SDMA;
			if (gFcLogN < 64u) {
				gFcLogN++;
				RDNA4LOG("fastcopy63: FIRST CHUNK REFUSED before the ring was written (VRAM %#llx, %#llx bytes; gScanoutLock wait %llu us) - "
				       "nothing of this copy is written, so the MM window runs it instead of failing the copy (refused to MM %llu)",
				       (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)sc.lockUs,
				       (unsigned long long)__atomic_load_n(&gFcS.refused_mm, __ATOMIC_RELAXED));
			}
			return 0ull;
		}
		// build 0.0.509 item 2 (F-2): a fence that did not land (or a ring write/doorbell that failed part-way) leaves a
		// chunk a LATE DMA may still write: its VRAM range is poisoned for the rest of the boot - the copy guard's sticky row
		// and, for a copy that bumped 62, 62's sticky entry - BEFORE the copy loop's FAILED path closes the copy's scope.
		const uint32_t dead = rdna4_fc_run_dead(r);
		if (dead) {
			__atomic_fetch_add(&gFcS.dead_ranges, 1ull, __ATOMIC_RELAXED);
		}
		if (gFcLogN < 64u || r == RDNA4_FC_RUN_TIMEOUT || r == RDNA4_FC_RUN_RING) {
			gFcLogN++;
			RDNA4LOG("fastcopy63: CHUNK FAILED at resource offset %#llx (VRAM %#llx, %#llx bytes): run %u - %s; fence want %#010x seen "
			       "%#010x after %llu us from the kick (gScanoutLock wait %llu us before it); timeouts %u, path %s%s",
			       (unsigned long long)pos, (unsigned long long)dAt,
			       (unsigned long long)n, r, rdna4_fc_fail_text(rdna4_fc_result_fail(r)), fenceWant, lastFence,
			       (unsigned long long)sc.dmaUs, (unsigned long long)sc.lockUs, timeouts, latched ? "LATCHED OFF for the boot" : "still usable",
			       dead ? "; the range is a DEAD RANGE, poisoned for the boot" : "");
		}
		return rdna4_fc_result_fail(r);
	}
	// THE VERIFY, after both locks are released, against the snapshot taken under gFastCopyLock.
	uint64_t v0 = 0, v1 = 0, cmp = 0; uint32_t rf = 0;
	FcVerifyT vt { 0ull, 0ull };                           // 0.0.509 item 4: gVramMmLock wait apart from the reads
	clock_get_uptime(&v0);
	const uint64_t bad = rdna4_fc_verify(&fc_mm_read, &fc_mm_read_plan, &vt, dAt, exp, full ? nullptr : exp + RDNA4_FC_SAMPLE_MAX,
	                                   (uint32_t)n, s->mode, rot, &cmp, &rf);
	clock_get_uptime(&v1);
	IOFree(exp, expBytes);
	const uint64_t vUs = fc_us(v0, v1), vLockUs = fc_us(0ull, vt.lockT), vReadUs = fc_us(0ull, vt.readT);
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	if (full) gFcS.full_dwords += cmp; else gFcS.sampled += cmp;
	if (rf) gFcS.verify_read_fail++;
	if (bad) rdna4_fc_latch(&gFc, RDNA4_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it this boot
	IOLockUnlock(l);
	if (bad)
		RDNA4LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream: the copy is poisoned (read-back mismatch) and the SDMA path is LATCHED OFF for the boot",
		       (unsigned long long)bad, (unsigned long long)cmp, (unsigned long long)dAt, (unsigned long long)n);
	if (rf) return rdna4_fc_result_fail(RDNA4_FC_RUN_VERIFY_READ);
	return rdna4_fc_result_ok(cmp, bad);
}

// After the COPIED line (GarnetAccelPeer.cpp): this copy's transport, when switch 63 was ON for it. 0.0.509: the slot is NOT
// released here any more - the copy scope's close (rdna4_fc_scope_closed) reads it first, on every path, then releases it.
void rdna4_fc_copy_report(uint64_t copyNo) {
	FcSlot *s = fc_slot_mine();
	if (!s) return;
	if (s->mode && s->chunksSdma) {
		__atomic_fetch_add(&gFcS.copies_sdma, 1ull, __ATOMIC_RELAXED);
		const uint64_t us = s->stageUs + s->dmaUs + s->verifyUs + s->lockUs;   // 0.0.509: dma no longer holds the lock wait
		RDNA4LOG("residency-copy: #%llu was copied VIA SDMA (not the MM window): %u chunk(s) via SDMA, %u via the MM window; stage %llu "
		       "us, dma %llu us, verify %llu us (%s %llu dword(s), %llu MISMATCHED); %llu bytes, %llu MB/s; waits: scanout-lock %llu us, "
		       "verify MM-lock %llu us, reads %llu us", (unsigned long long)copyNo,
		       s->chunksSdma, s->chunksMm, (unsigned long long)s->stageUs, (unsigned long long)s->dmaUs,
		       (unsigned long long)s->verifyUs, s->mode == RDNA4_FC_M_FULL ? "full" : "sampled", (unsigned long long)s->compared,
		       (unsigned long long)s->mismatched, (unsigned long long)s->bytes, (unsigned long long)(us ? s->bytes / us : 0ull),
		       (unsigned long long)s->lockUs, (unsigned long long)s->vLockUs, (unsigned long long)s->vReadUs);
		IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);   // non-null: a chunk went via SDMA under it
		if (l) {                                           // 0.0.509 item 4: the boot's slowest verify per dword
			IOLockLock(l);
			(void)rdna4_fc_vmax_note(&gFcS, s->verifyUs, s->vLockUs, s->vReadUs, s->compared, s->bytes);
			IOLockUnlock(l);
		}
	} else if (s->mode && gFcLogN < 64u) {
		gFcLogN++;
		RDNA4LOG("residency-copy: #%llu stayed on the MM window with switch 63 ON - %u chunk(s), SDMA declined: %s", (unsigned long long)copyNo,
		       s->chunksMm, rdna4_fc_reason_name(s->lastWhy));
	}
}
// 0.0.345 — THE DETILE. ONE SDMA COPY_TILED_SUB_WINDOW packet (14 dwords, dcc 0, CPV 0) plus one FENCE
// (4 dwords) for the WHOLE frame, where the linear path emitted 1080 packets in six chunked submissions. detile = 1:
// the tiled surface is the SOURCE and the linear scanout is the DESTINATION.
//
// Everything in this function is a field copy out of the plan; the plan did the checking (rdna4_scanout_plan_tiled, and
// rdna4_scanout_tiled_dst_ok over EVERY byte the packet may write). tiledMc/linearMc are MC addresses the caller has
// already range-checked. CPV IS NEVER SET: found COPY_LINEAR grows a trailing dword only when CPV is set,
// and a 15-dword packet in a 14-dword slot desynchronises the ring for ever.
// 0.0.416 (an internal design note, G3): `gcr` prepends the five-dword GCR_REQ to the SAME submission as the
// copy — one ring write, one doorbell, one fence — so the cache rinse is ordered before the copy the engine then
// reads the tiled source with. Default false: every pre-0.0.416 caller is byte-identical (18 dwords, no GCR).
static uint32_t scanout_sdma_tiled_copy(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                        const RDNA4ScanoutTiledPlan *p, uint64_t tiledMc, uint64_t linearMc,
                                        bool detile, uint32_t fenceValue, uint32_t timeoutMs,
                                        uint64_t *usOut, uint32_t *lastFenceOut, bool gcr = false) {
	*usOut = 0; *lastFenceOut = 0;
	const uint32_t dwords = rdna4_sdma_tiled_copy_dwords(gcr);           // 18, or 23 with the GCR_REQ
	if (!p || dwords > inst.ring_size_dwords / 2) return kScanStRing;
	// The tiled surface base must be 64 KiB aligned in MC space too, not only as a VRAM offset: the block index
	// arithmetic the engine does is relative to this address, and a misaligned base would shift every block.
	if (tiledMc & 0xffffull) return kScanStRing;
	uint32_t pkt[RDNA4_SDMA_TILED_SUB_WINDOW_DWORDS + RDNA4_SDMA_FENCE_DWORDS + RDNA4_SDMA_GCR_REQ_DWORDS];
	uint32_t k = 0;
	if (gcr) k += rdna4_sdma_gcr_req(pkt + k);                           // the ONE emitter (G1)
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
	           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_TILED_SUB_WINDOW) |
	           amdgpu::SDMA_PKT_TILED_SW_TMZ(0) | amdgpu::SDMA_PKT_TILED_SW_DCC(0) |
	           amdgpu::SDMA_PKT_TILED_SW_DETILE(detile ? 1u : 0u);
	pkt[k++] = static_cast<uint32_t>(tiledMc);
	pkt[k++] = static_cast<uint32_t>(tiledMc >> 32);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_XY(p->srcX, p->srcY);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_Z_W(0, p->surfW - 1u);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_HD(p->surfH - 1u, 0);            // depth 1 -> depth-1 = 0
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_INFO(p->elementLog2, p->swizzle, 0, 0);
	pkt[k++] = static_cast<uint32_t>(linearMc);
	pkt[k++] = static_cast<uint32_t>(linearMc >> 32);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_XY(p->dstX, p->dstY);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_Z_PITCH(0, p->linPitch - 1u);
	pkt[k++] = p->linSlice - 1u;
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_RECT(p->w - 1u, p->h - 1u);
	pkt[k++] = 0;                                                          // rect depth - 1
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	if (k != dwords) return kScanStRing;
	amdgpu::amdgpu_hdp_flush(dev);
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// 0.0.416 (an internal design note, G2) — ONE COPY_LINEAR window plus, if asked, the GCR_REQ immediately before
// it in the SAME submission (one ring write, one doorbell, one fence). The COPY_LINEAR shape is scanout_sdma_copies'
// (header with CPV 1, bytes-1, src, dst), so mode 7's copy is the same packet the scanout path already proved.
static uint32_t scanout_sdma_copy_linear_gcr(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                             uint64_t srcMc, uint64_t dstMc, uint32_t bytes, bool gcr,
                                             uint32_t fenceValue, uint32_t timeoutMs,
                                             uint64_t *usOut, uint32_t *lastFenceOut) {
	*usOut = 0; *lastFenceOut = 0;
	if (bytes == 0 || bytes > amdgpu::kSDMACopyLinearMaxBytes) return kScanStPlan;
	const uint32_t dwords = rdna4_sdma_linear_copy_dwords(gcr);
	if (dwords > inst.ring_size_dwords / 2) return kScanStRing;
	uint32_t pkt[RDNA4_SDMA_COPY_LINEAR_DWORDS + RDNA4_SDMA_FENCE_DWORDS + RDNA4_SDMA_GCR_REQ_DWORDS];
	uint32_t k = 0;
	if (gcr) k += rdna4_sdma_gcr_req(pkt + k);                           // the ONE emitter (G1)
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
	           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_LINEAR) | amdgpu::SDMA_PKT_HEADER_CPV(1);
	pkt[k++] = bytes - 1;                                              // byte_count - 1
	pkt[k++] = 0;
	pkt[k++] = static_cast<uint32_t>(srcMc);
	pkt[k++] = static_cast<uint32_t>(srcMc >> 32);
	pkt[k++] = static_cast<uint32_t>(dstMc);
	pkt[k++] = static_cast<uint32_t>(dstMc >> 32);
	pkt[k++] = 0;
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	if (k != dwords) return kScanStRing;
	amdgpu::amdgpu_hdp_flush(dev);
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// 0.0.278: `n` row packets in submissions of at most kScanChunkRows, each fenced (fenceBase + chunk index) and
// waited for before the next. usOut sums the waits; the first failing chunk's status is returned.
static uint32_t scanout_sdma_copies_chunked(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                            const uint64_t *srcMc, const uint64_t *dstMc, const uint32_t *bytes,
                                            uint32_t n, uint32_t fenceBase, uint32_t timeoutMs,
                                            uint64_t *usOut, uint32_t *lastFenceOut, uint32_t *chunksOut) {
	*usOut = 0; *lastFenceOut = 0; *chunksOut = 0;
	uint32_t st = kScanStOk;
	for (uint32_t at = 0, c = 0; st == kScanStOk && at < n; at += kScanChunkRows, c++) {
		const uint32_t k = n - at < kScanChunkRows ? n - at : kScanChunkRows;
		uint32_t rptr = 0;
		if (c > 0) st = scanout_queue_check(dev, inst, &rptr);
		uint64_t us = 0;
		if (st == kScanStOk)
			st = scanout_sdma_copies(dev, inst, &srcMc[at], &dstMc[at], &bytes[at], k, fenceBase + c, timeoutMs, &us, lastFenceOut);
		*usOut += us;
		*chunksOut = c + 1;
	}
	return st;
}

static void scanout_free_vram(amdgpu::VRAMAllocation &a) {
	if (a.size) gBringup.gmc.vram_alloc.free(a);
	a.size = 0;
}

// =====================================================================================================
// 0.0.414 (an internal design note,) — THE FULL-GEOMETRY SDMA SELF-TEST.
//
// Mode 5 proves the 14-dword COPY_TILED_SUB_WINDOW packet on a 256x256 surface.'s live
// plane is 1920x1080, ADDR3 64KB_2D, pitch 1920, 15 blocks across and a partial 9th block row, and the
// one-packet copy of it reads black at most sampled points on the glass. This function measures the SAME
// packet, built by the SAME scanout_sdma_tiled_copy, on the LIVE geometry with our own buffers.
//
// S3/S4's method, parameterised by (w, h): the CPU writes a LINEAR probe in ASCENDING address order (the
// order proved SDMA reads correctly), SDMA TILES it (detile 0) into a tiled scratch, then SDMA DETILES
// that SDMA-written scratch (detile 1) into a poisoned linear destination. proved the SDMA-written tiled
// source detiles correctly on 256x256; this asks the same question on the live geometry.
//
// S5's bounded sample set (rdna4_tile_ss_*) is read through the MM window — the GPU's own view — for the TILE
// and DETILE verdicts, and the destination is read through BAR0 at the same set for a second opinion. S2's
// 16x16 cell map (display_pipe_guard.h) records where the detile is wrong. Buffers are our own pool's and are
// freed on every exit; nothing outside those three buffers is touched (S2/S8).
struct RDNA4FullCase {
	uint32_t w, h;
	uint32_t tileWrong, tileSampled;
	uint32_t detWrong, detSampled, poison, barMm, barSampled;
	uint64_t map[4];
	uint32_t nfirst;
	uint32_t fdx[8], fdy[8], fgot[8], fpx[8], fpy[8];
	uint64_t probeOff, tiledOff, destOff, probeMc, tiledMc, destMc, linBytes, tilBytes;
	uint64_t tileUs, detUs;
	uint32_t tileStatus, detStatus, tileFence, detFence;
	uint32_t st;
};

// S6's decoded batch: up to 8 wrong destination pixels as (dx,dy) got (sx,sy)|POISON, with the tiled address
// our equation gives for (dx,dy) and for (sx,sy). Kept well under the logger's 512-byte line.
static void scanout_full_decode(const RDNA4FullCase *r, char *out, unsigned cap) {
	unsigned o = 0;
	if (!cap) return;
	out[0] = 0;
	for (uint32_t i = 0; i < r->nfirst && o + 1u < cap; i++) {
		char one[96];
		int l;
		const unsigned long long aD = (unsigned long long)rdna4_addr3_64kb_2d_off_4bpe(r->fdx[i], r->fdy[i], r->w);
		if (r->fpx[i] == 0xffffffffu) {
			l = snprintf(one, sizeof(one), "(%u,%u)%s@%#llx; ", r->fdx[i], r->fdy[i], "POISON", aD);
		} else {
			const unsigned long long aS = (unsigned long long)rdna4_addr3_64kb_2d_off_4bpe(r->fpx[i], r->fpy[i], r->w);
			l = snprintf(one, sizeof(one), "(%u,%u)->(%u,%u)@%#llx/%#llx; ",
			             r->fdx[i], r->fdy[i], r->fpx[i], r->fpy[i], aD, aS);
		}
		if (l < 0 || o + (unsigned)l >= cap) break;
		memcpy(out + o, one, (size_t)l);
		o += (unsigned)l;
		out[o] = 0;
	}
}

// D7 (0.0.417, an internal design note): `uniform` selects the probe pattern. Mode 6 passes false and is
// byte-identical; mode 8 passes true for its 1920x1080 case only, so the CPU writes RDNA4_TILE_UNIFORM_PIXEL at
// every position and every expected value is that same value. Everything else - buffers, packet fields, sample
// set, report lines, POISON detection - is the same code the two modes share.
static inline uint32_t scanout_full_probe(uint32_t x, uint32_t y, bool uniform) {
	return uniform ? rdna4_tile_uniform_pixel(x, y) : rdna4_tile_probe_pixel(x, y);
}

static uint32_t scanout_full_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                  uint32_t w, uint32_t h, uint32_t fenceBase, RDNA4FullCase *r, bool uniform) {
	amdgpu::VRAMAllocation pa {}, ta {}, da {};
	uint32_t st = kScanStOk;
	bzero(r, sizeof(*r));
	r->w = w; r->h = h;
	r->linBytes = (uint64_t)w * h * 4u;
	r->tilBytes = rdna4_addr3_64kb_2d_bytes_4bpe(w, h);
	{
		uint32_t rp = 0;
		st = scanout_queue_check(dev, inst, &rp);
		if (st != kScanStOk) { r->st = st; return st; }
	}
	if (!gBringup.gmc.vram_alloc.alloc(r->linBytes, 65536, &pa) ||
	    !gBringup.gmc.vram_alloc.alloc(r->tilBytes, 65536, &ta) ||
	    !gBringup.gmc.vram_alloc.alloc(r->linBytes, 65536, &da)) {
		// S2: refuse with a named status and log the pool's free size; do NOT grow the pool in this build.
		RDNA4LOG("scanout-full: %ux%u needs %llu (probe) + %llu (tiled) + %llu (dest) = %llu byte(s) from "
		       "gmc.vram_alloc; pool free now %llu byte(s) - REFUSING (VRAM allocation failed)",
		       w, h, (unsigned long long)r->linBytes, (unsigned long long)r->tilBytes,
		       (unsigned long long)r->linBytes,
		       (unsigned long long)(2ull * r->linBytes + r->tilBytes),
		       (unsigned long long)gBringup.gmc.vram_alloc.bytes_free());
		scanout_free_vram(pa); scanout_free_vram(ta); scanout_free_vram(da);
		r->st = kScanStAlloc;
		return kScanStAlloc;
	}
	r->probeOff = pa.gpu_va - gBringup.gmc.vram_start;
	r->tiledOff = ta.gpu_va - gBringup.gmc.vram_start;
	r->destOff  = da.gpu_va - gBringup.gmc.vram_start;
	r->probeMc = pa.gpu_va; r->tiledMc = ta.gpu_va; r->destMc = da.gpu_va;
	if (r->probeOff + r->linBytes > dev.bar0Size || r->destOff + r->linBytes > dev.bar0Size ||
	    r->tiledOff + r->tilBytes > dev.bar0Size || (r->tiledOff & 0xffffu) || (r->probeOff & 0xffffu) ||
	    (r->destOff & 0xffffu)) { st = kScanStAlloc; goto done; }
	// S3: the CPU writes the LINEAR probe in ASCENDING address order; pattern rdna4_tile_probe_pixel.
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			amdgpu::WBAR0_32(dev, r->probeOff + ((uint64_t)y * w + x) * 4u, scanout_full_probe(x, y, uniform));
	amdgpu::amdgpu_hdp_flush(dev);
	// The plan is mode 5's, with the LIVE field values: swizzle 3, element size 2, pitch = width.
	{
		RDNA4ScanoutTiledPlan p {};
		uint64_t us = 0; uint32_t last = 0;
		p.fbOff = r->destOff; p.srcOff = r->tiledOff; p.surfW = w; p.surfH = h;
		p.swizzle = amdgpu::kAddr3_64KB_2D; p.elementLog2 = 2u;
		p.srcX = 0; p.srcY = 0; p.dstX = 0; p.dstY = 0;
		p.linPitch = w; p.linSlice = w * h; p.w = w; p.h = h;
		// S3: SDMA TILES the verified linear probe into the tiled scratch (detile 0).
		st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + r->tiledOff,
		                             gBringup.gmc.vram_start + r->probeOff, false, fenceBase, 1000, &us, &last);
		r->tileUs = us; r->tileFence = last; r->tileStatus = st;
		if (st != kScanStOk) goto done;
		// TILE verdict (S3): the tiled scratch through the GPU's own MM window, against the equation.
		{
			const uint64_t n = rdna4_tile_ss_count(w, h);
			uint32_t bad = 0;
			for (uint64_t i = 0; i < n; i++) {
				uint32_t x = 0, y = 0, got = 0;
				if (!rdna4_tile_ss_at(w, h, i, &x, &y)) break;
				const uint64_t o = r->tiledOff + rdna4_addr3_64kb_2d_off_4bpe(x, y, w);
				if (!rdna4_vram_read_mm(o, &got, 1) || got != scanout_full_probe(x, y, uniform)) bad++;
			}
			r->tileWrong = bad; r->tileSampled = (uint32_t)n;
		}
		// S4: poison the whole linear destination, then ONE detile of the whole surface into it, pitch w.
		amdgpu::bar0_memset_vram(dev, r->destOff, RDNA4_TILE_PROBE_POISON, r->linBytes);
		amdgpu::amdgpu_hdp_flush(dev);
		st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + r->tiledOff,
		                             gBringup.gmc.vram_start + r->destOff, true, fenceBase + 1u, 1000, &us, &last);
		r->detUs = us; r->detFence = last; r->detStatus = st;
		if (st != kScanStOk) goto done;
		// DETILE verdict (S4/S5): the bounded sample set through MM, BAR0 at the same set counted separately.
		{
			const uint64_t n = rdna4_tile_ss_count(w, h);
			uint32_t bad = 0, poison = 0, barMm = 0, barSampled = 0;
			for (uint64_t i = 0; i < n; i++) {
				uint32_t dx = 0, dy = 0, got = 0;
				if (!rdna4_tile_ss_at(w, h, i, &dx, &dy)) break;
				const uint64_t o = r->destOff + ((uint64_t)dy * w + dx) * 4u;
				const bool mmOk = rdna4_vram_read_mm(o, &got, 1);
				barSampled++;
				if (mmOk && amdgpu::RBAR0_32(dev, o) != got) barMm++;
				if (mmOk && got == scanout_full_probe(dx, dy, uniform)) continue;
				if (mmOk && got == RDNA4_TILE_PROBE_POISON) poison++;
				rdna4_dpg_verify_map_set(r->map, rdna4_dpg_cell16(dx, dy, w, h));
				if (r->nfirst < 8u) {
					uint32_t sx = 0, sy = 0;
					r->fdx[r->nfirst] = dx; r->fdy[r->nfirst] = dy; r->fgot[r->nfirst] = got;
					if (rdna4_tile_decode(got, &sx, &sy)) { r->fpx[r->nfirst] = sx; r->fpy[r->nfirst] = sy; }
					else { r->fpx[r->nfirst] = 0xffffffffu; r->fpy[r->nfirst] = 0xffffffffu; }
					r->nfirst++;
				}
				bad++;
			}
			r->detWrong = bad; r->detSampled = (uint32_t)n; r->poison = poison;
			r->barMm = barMm; r->barSampled = barSampled;
		}
		// S6: one summary line, one map line, one decoded batch, each under 512 bytes.
		RDNA4LOG("scanout-full: %ux%u swizzle 3 (ADDR3_64KB_2D, 32 bpp, pitch %u) probe %#llx (MC %#llx) tiled %#llx "
		       "(MC %#llx, %llu B) dest %#llx (MC %#llx); TILE status %u fence %#010x %llu us; DETILE status %u "
		       "fence %#010x %llu us", w, h, w, (unsigned long long)r->probeOff, (unsigned long long)r->probeMc,
		       (unsigned long long)r->tiledOff, (unsigned long long)r->tiledMc, (unsigned long long)r->tilBytes,
		       (unsigned long long)r->destOff, (unsigned long long)r->destMc, r->tileStatus, r->tileFence,
		       (unsigned long long)r->tileUs, r->detStatus, r->detFence, (unsigned long long)r->detUs);
		RDNA4LOG("scanout-full: %ux%u TILE wrong %u of %u; DETILE wrong %u of %u; POISON (never written) %u of %u; "
		       "BAR0-vs-MM disagreements %u of %u%s", w, h, r->tileWrong, r->tileSampled, r->detWrong, r->detSampled,
		       r->poison, r->detSampled, r->barMm, r->barSampled,
		       (r->tileWrong || r->detWrong) ? " - MEASURED (not a refusal)" : " - CLEAN");
		RDNA4LOG("scanout-full: %ux%u detile wrong 16x16 map %016llx%016llx%016llx%016llx", w, h,
		       (unsigned long long)r->map[0], (unsigned long long)r->map[1],
		       (unsigned long long)r->map[2], (unsigned long long)r->map[3]);
		if (r->nfirst) {
			char dec[400];
			scanout_full_decode(r, dec, sizeof(dec));
			RDNA4LOG("scanout-full: %ux%u DECODE %u: %s", w, h, r->nfirst, dec);
		}
	}
done:
	scanout_free_vram(pa); scanout_free_vram(ta); scanout_free_vram(da);
	r->st = st;
	return st;
}

// =====================================================================================================
// 0.0.416 (an internal design note, G2;) — MODE 7: THE SDMA CACHE-RINSE INSTRUMENT.
//
// READ-ONLY on the source. ONE SDMA COPY_LINEAR moves a 256 KiB window from VRAM offset `vramOff` into a low
// scratch buffer of ours; if `gcr` is asked for, the SDMA GCR_REQ (GL2 write-back + invalidate) goes
// immediately BEFORE the copy in the SAME submission, so it is ordered before the engine reads the source.
// Fenced as every copy here is. Then the scratch (what the engine's copy delivered) and the source (what the
// GPU's own MM window sees) are BOTH read through the MM window and compared per 256-byte line: lines fully
// matching / partly / fully differing, a 64-bit map of which of the 64 4-KiB pages hold a difference, and the
// first 8 differing dwords (offset, MM source, SDMA scratch). `vramOff == 0` is the CONTROL: a low buffer of
// our own is written with a known pattern through the MM window and the SAME comparison runs on it.
//
// The question it asks: does the engine read the same bytes at these addresses as the GPU's own view? If a
// plane window differs plain but matches with the GCR_REQ, cache coherence is the mechanism ('s (A)).
// Every log line is under 512 bytes; every buffer is our own and freed on every exit path.
// =====================================================================================================
#define RDNA4_GCR_WINDOW_BYTES 0x40000u          // 256 KiB
#define RDNA4_GCR_LINE_DWORDS  64u               // one 256-byte line
#define RDNA4_GCR_LINES        (RDNA4_GCR_WINDOW_BYTES / (RDNA4_GCR_LINE_DWORDS * 4u))   // 1024
#define RDNA4_GCR_PAGE_LINES   16u               // 4 KiB / 256 B

static uint32_t scanout_gcr_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                 uint64_t arg, uint64_t *v) {
	amdgpu::VRAMAllocation sc {}, pat {};
	uint32_t st = kScanStOk;
	const bool control = (rdna4_scanout7_off(arg) == 0);
	const bool gcr = rdna4_scanout7_gcr(arg) != 0;
	uint64_t srcOff = rdna4_scanout7_off(arg);
	uint32_t rp = 0;
	st = scanout_queue_check(dev, inst, &rp);
	if (st != kScanStOk) { v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff; return st; }
	if (!control) {
		const uint32_t rr = rdna4_gcr_src_reason(srcOff, RDNA4_GCR_WINDOW_BYTES, dev.vramSizeBytes,
		                                       gBringup.gmc.vram_alloc.base(), gBringup.gmc.vram_alloc.size(),
		                                       gBringup.gmc.vram_alloc_hi.base(), gBringup.gmc.vram_alloc_hi.size());
		if (rr != kRDNA4GcrSrcOk) {
			RDNA4LOG("scanout-gcr: source window %#llx + %u byte(s) REFUSED (%s; allocation pools %#llx+%llu and "
			       "%#llx+%llu, VRAM %llu byte(s))", (unsigned long long)srcOff, (unsigned)RDNA4_GCR_WINDOW_BYTES,
			       rr == kRDNA4GcrSrcAlign ? "not 4 KiB aligned" : rr == kRDNA4GcrSrcRange ? "outside the card's VRAM"
			       : "overlaps one of our own allocations",
			       (unsigned long long)gBringup.gmc.vram_alloc.base(), (unsigned long long)gBringup.gmc.vram_alloc.size(),
			       (unsigned long long)gBringup.gmc.vram_alloc_hi.base(), (unsigned long long)gBringup.gmc.vram_alloc_hi.size(),
			       (unsigned long long)dev.vramSizeBytes);
			v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
			return rr == kRDNA4GcrSrcAlign ? kScanStGcrAlign : rr == kRDNA4GcrSrcRange ? kScanStGcrRange : kScanStGcrOverlap;
		}
	}
	if (!gBringup.gmc.vram_alloc.alloc(RDNA4_GCR_WINDOW_BYTES, 65536, &sc) ||
	    (control && !gBringup.gmc.vram_alloc.alloc(RDNA4_GCR_WINDOW_BYTES, 65536, &pat))) {
		RDNA4LOG("scanout-gcr: cannot allocate %u byte(s) scratch (+ control) from gmc.vram_alloc; pool free now %llu "
		       "byte(s) - REFUSING", (unsigned)RDNA4_GCR_WINDOW_BYTES, (unsigned long long)gBringup.gmc.vram_alloc.bytes_free());
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	const uint64_t scOff = sc.gpu_va - gBringup.gmc.vram_start;
	if (scOff + RDNA4_GCR_WINDOW_BYTES > dev.bar0Size ||
	    (control && (pat.gpu_va - gBringup.gmc.vram_start) + RDNA4_GCR_WINDOW_BYTES > dev.bar0Size)) {
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	if (control) {
		// The control source is OUR low buffer, written through the MM window (the GPU's own view) so the same
		// comparison runs on memory we own and know. The pattern is self-naming: index | 0x5a5a0000.
		srcOff = pat.gpu_va - gBringup.gmc.vram_start;
		for (uint64_t i = 0; i < RDNA4_GCR_WINDOW_BYTES / 4u; i += 64u) {
			uint32_t p[64];
			for (uint32_t x = 0; x < 64u; x++) p[x] = 0x5a5a0000u | (uint32_t)((i + x) & 0xffffu);
			if (!rdna4_vram_write_mm(srcOff + i * 4u, p, 64)) {
				RDNA4LOG("scanout-gcr: CONTROL pattern write through the MM window failed at +%#llx - REFUSING",
				       (unsigned long long)(i * 4u));
				scanout_free_vram(sc); scanout_free_vram(pat);
				v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
				return kScanStSourceCtl;
			}
		}
		amdgpu::amdgpu_hdp_flush(dev);
	}
	uint64_t us = 0; uint32_t last = 0;
	st = scanout_sdma_copy_linear_gcr(dev, inst, gBringup.gmc.vram_start + srcOff,
	                                  gBringup.gmc.vram_start + scOff, RDNA4_GCR_WINDOW_BYTES, gcr,
	                                  0x5CA12000u, 1000, &us, &last);
	uint32_t full = 0, part = 0, diff = 0, ndiff = 0, nfirst = 0;
	uint32_t nd[8] = { 0 }, nmm[8] = { 0 }, nsd[8] = { 0 };
	uint64_t pageMap = 0;
	if (st == kScanStOk) {
		for (uint32_t l = 0; l < RDNA4_GCR_LINES; l++) {
			const uint64_t off = (uint64_t)l * (RDNA4_GCR_LINE_DWORDS * 4u);
			uint32_t mm[64] = { 0 }, sd[64] = { 0 };
			const bool mok = rdna4_vram_read_mm(srcOff + off, mm, RDNA4_GCR_LINE_DWORDS);
			const bool sok = rdna4_vram_read_mm(scOff + off, sd, RDNA4_GCR_LINE_DWORDS);
			uint32_t bad = 0;
			for (uint32_t k = 0; k < RDNA4_GCR_LINE_DWORDS; k++) {
				if (mok && sok && mm[k] == sd[k]) continue;
				bad++; ndiff++;
				pageMap |= 1ull << (l / RDNA4_GCR_PAGE_LINES);
				if (nfirst < 8u) { nd[nfirst] = l * RDNA4_GCR_LINE_DWORDS + k; nmm[nfirst] = mm[k]; nsd[nfirst] = sd[k]; nfirst++; }
			}
			if (bad == 0) full++;
			else if (bad == RDNA4_GCR_LINE_DWORDS) diff++;
			else part++;
		}
	}
	// out: [1] gcr | control<<1, [2] source VRAM offset, [3] scratch VRAM offset, [4] fence us | copy dwords<<32,
	// [5] lines full | part<<16 | fully-differing<<32, [6] differing dwords, [7] the 64-page difference map.
	v[1] = (uint64_t)gcr | ((uint64_t)control << 1);
	v[2] = srcOff; v[3] = scOff;
	v[4] = us | ((uint64_t)rdna4_sdma_linear_copy_dwords(gcr) << 32);
	v[5] = (uint64_t)full | ((uint64_t)part << 16) | ((uint64_t)diff << 32);
	v[6] = ndiff; v[7] = pageMap;
	RDNA4LOG("scanout-gcr: mode 7 %s (%u-byte window), GCR_REQ %s; source %#llx (MC %#llx) -> scratch %#llx (MC %#llx); "
	       "status %u, fence %#010x after %llu us, COPY_LINEAR+GCR %u dwords; lines full %u, partly %u, fully differing %u "
	       "of %u; differing dwords %u; pages with a difference %#018llx", control ? "CONTROL" : "PLANE",
	       (unsigned)RDNA4_GCR_WINDOW_BYTES, gcr ? "ON" : "off", (unsigned long long)srcOff,
	       (unsigned long long)(gBringup.gmc.vram_start + srcOff), (unsigned long long)scOff,
	       (unsigned long long)(gBringup.gmc.vram_start + scOff), st, last, (unsigned long long)us,
	       (unsigned)rdna4_sdma_linear_copy_dwords(gcr), full, part, diff, (unsigned)RDNA4_GCR_LINES, ndiff,
	       (unsigned long long)pageMap);
	if (nfirst)
		RDNA4LOG("scanout-gcr: first %u differing dword(s) (offset, MM source, SDMA scratch): %u:%#010x/%#010x "
		       "%u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x "
		       "%u:%#010x/%#010x", nfirst, nd[0], nmm[0], nsd[0], nd[1], nmm[1], nsd[1], nd[2], nmm[2], nsd[2],
		       nd[3], nmm[3], nsd[3], nd[4], nmm[4], nsd[4], nd[5], nmm[5], nsd[5], nd[6], nmm[6], nsd[6],
		       nd[7], nmm[7], nsd[7]);
	scanout_free_vram(sc); scanout_free_vram(pat);
	return st;
}

// Mode 0 reads, 1 runs the positive control, 2 restores the rectangle the positive control overwrote.
// out[0] status, [1] geometry reason | plan reason<<8 | pre-flight BAR0 pass 2<<16 | BAR0 pass 3 (after the HDP
// flush)<<32 | poison (0xdeadbeef) mismatches<<48, [2] scanout VRAM offset, [3] rect x<<48|y<<32|w<<16|h,
// [4] pre-flight BAR0 pass 1 mismatches | MM-WINDOW mismatches<<32 (0.0.337: the MM window is the verdict),
// [5] pre-flight fence us<<32 | scanout fence us, [6] rect BAR0 mismatches | rect MM-window mismatches<<32,
// [7] first mismatch index<<32 | value read (the PRE-FLIGHT's when status is 9, the rectangle's otherwise),
// [8..11] samples expected<<32|read (row 0 x=16, row 0 x=112, row 0 x=240, row 63 x=16),
// [12] passed | runs<<8 | copies<<24 | refusals<<40.
static uint32_t rdna4_scanout_full(uint32_t mode, uint64_t *v);   // build 0.0.542: modes 9 and 10 (below, beside flip mode)
uint32_t rdna4_scanout_control(uint64_t arg, uint64_t *out, unsigned count) {
	uint64_t v[13] = { 0 };
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	// 0.0.416 (an internal design note, G2): mode 7 packs the mode, the GCR flag and the source VRAM offset
	// into the one ABI scalar (rdna4_scanout7_scalar). Modes 0..6 keep scalar == mode, byte-identical; any other
	// scalar with high bits set is refused exactly as it was before (kScanStPlan).
	const uint32_t mode = (uint32_t)(arg & RDNA4_SCANOUT_MODE_MASK);
	uint32_t st = kScanStOk;
	if (!txn.ok) {
		v[0] = kScanStNoContext;
		if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
		return kScanStNoContext;
	}
	amdgpu::VRAMAllocation a {}, b {}, c {};
	RDNA4ScanoutGeom g {};
	
	uint64_t fbOff = 0;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { st = kScanStNoContext; goto done_nolock; }
	if (arg != (uint64_t)mode && mode != 7u) { st = kScanStPlan; goto done_nolock; }
	// build 0.0.542: `scanout 9` (full) / `scanout 10` (release) take flip mode's lock BEFORE gScanoutLock (the lock order), so
	// they branch here, before this function's own gScanoutLock. v[1] carries scanout_full.h's reason; 0 captured / released.
	if (mode == RDNA4_SF_MODE || mode == RDNA4_SF_MODE_RELEASE) {
		st = rdna4_scanout_full(mode, v) ? kScanStFull : kScanStOk;
		goto done_nolock;
	}
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		st = scanout_geometry(dev, &g);
		if (st != kScanStOk) goto done;
		{
			const uint32_t gr = rdna4_scanout_geom_check(&g, &fbOff);
			v[1] = gr; v[2] = fbOff;
			RDNA4LOG("scanout: %s base %#llx len %#llx %ux%u rowBytes %u depth %u; our BAR0 %#llx size %#llx, "
			       "vramBase %#llx, vram_start(MC) %#llx vramMcBase %#llx -> geometry %s (reason %u), scanout VRAM "
			       "offset %#llx", "AMDGarnetPrimaryFramebuffer", (unsigned long long)g.fbPhys, (unsigned long long)g.fbLen, g.width,
			       g.height, g.rowBytes, g.depth, (unsigned long long)g.bar0Phys, (unsigned long long)g.bar0Size,
			       (unsigned long long)g.vramBase, (unsigned long long)gBringup.gmc.vram_start,
			       (unsigned long long)dev.vramMcBase, gr ? "REFUSED" : "OK", gr, (unsigned long long)fbOff);
			if (gr) { st = kScanStGeom; goto done; }
			if (gBringup.gmc.vram_start != dev.vramMcBase || dev.vramMcBase == 0) { st = kScanStMcBase; goto done; }
		}
		if (mode == 0) goto done;
		// build 0.0.518 (W4): the content read / FNV (3) and the thumbnail (4) read the buffer HUBP0 is scanning (A or flip
		// mode's B), not A. Both only read; every write mode below keeps the console's offset.
		if (mode == 3 || mode == 4) {
			const uint64_t rd = fm_displayed_off(fbOff, g.fbLen);
			if (rd != fbOff) RDNA4LOG("scanout: mode %u reads flip mode's B (vram+%#llx), the buffer SURFACE_EARLIEST_INUSE names",
			                        mode, (unsigned long long)rd);
			fbOff = rd;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		if (mode == 2) {
			if (!gScanoutSave) { st = kScanStNoSave; goto done; }
			for (uint32_t y = 0; y < gScanoutSaveH; y++)
				for (uint32_t x = 0; x < gScanoutSaveW; x++) {
					const uint64_t o = fbOff + (uint64_t)(gScanoutSaveY + y) * g.rowBytes + (uint64_t)(gScanoutSaveX + x) * 4u;
					if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, o, 4)) continue;
					amdgpu::WBAR0_32(dev, o, gScanoutSave[y * gScanoutSaveW + x]);
				}
			RDNA4LOG("scanout: RESTORED the %ux%u rectangle at (%u,%u) from the saved copy (CPU writes through BAR0, "
			       "inside the scanout only)", gScanoutSaveW, gScanoutSaveH, gScanoutSaveX, gScanoutSaveY);
			goto done;
		}
		// 0.0.279: mode 3 READS WHAT THE MONITOR SHOWS, writes nothing. CoreDisplay's legacy present copies
		// WindowServer's shadow into the kIOFBVRAMMemory mapping - RDNA4FB's getVRAMRange, this scanout - so the scanout itself
		// is the evidence. out[1] positive-control rectangle pixels still holding the kext bars (of out[2]), [3] FNV-1a over
		// every 4th pixel of every 4th row, [4] non-black sampled pixels, [5] value changes along the sampled rows, [6] samples,
		// [7] centre, [8] (16,16), [9] (w-17,16), [10] (16,h-17), [11] (w-17,h-17), [12] pixels differing from the bars in the rect.
		if (mode == 3) {
			uint32_t bars = 0, diff = 0;
			for (uint32_t y = 0; y < kScanPcH; y++)
				for (uint32_t x = 0; x < kScanPcW; x++) {
					const uint64_t o = fbOff + (uint64_t)(kScanPcY + y) * g.rowBytes + (uint64_t)(kScanPcX + x) * 4u;
					if (amdgpu::RBAR0_32(dev, o) == rdna4_pc_pixel(x, y, kScanPcW)) bars++; else diff++;
				}
			uint64_t h = 1469598103934665603ull;
			uint32_t nz = 0, changes = 0, samples = 0;
			for (uint32_t y = 0; y < g.height; y += 4) {
				uint32_t prev = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes);
				for (uint32_t x = 0; x < g.width; x += 4) {
					const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
					h = (h ^ px) * 1099511628211ull;
					if (px & 0x00ffffffu) nz++;
					if (px != prev) { changes++; prev = px; }
					samples++;
				}
			}
			const uint32_t sx[5] = { g.width / 2, 16, g.width - 17, 16, g.width - 17 }, sy[5] = { g.height / 2, 16, 16, g.height - 17, g.height - 17 };
			for (unsigned i = 0; i < 5; i++) v[7 + i] = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)sy[i] * g.rowBytes + (uint64_t)sx[i] * 4u);
			v[1] = bars; v[2] = kScanPcW * kScanPcH; v[3] = h; v[4] = nz; v[5] = changes; v[6] = samples; v[12] = diff;
			RDNA4LOG("scanout: CONTENT READ (writes nothing): control rectangle %ux%u at (%u,%u) still holds the kext bars at %u pixel(s), %u differ; "
			       "sampled %u pixel(s) (every 4th of every 4th row): non-black %u, value changes %u, FNV-1a %#018llx; centre %#010x, "
			       "inset corners TL %#010x TR %#010x BL %#010x BR %#010x", kScanPcW, kScanPcH, kScanPcX, kScanPcY, bars, diff, samples, nz,
			       changes, (unsigned long long)h, (uint32_t)v[7], (uint32_t)v[8], (uint32_t)v[9], (uint32_t)v[10], (uint32_t)v[11]);
			goto done;
		}
		// 0.0.281: mode 4 is a READ-ONLY THUMBNAIL - the scanout averaged into 64x36 cells (30x30 px at 1920x1080,
		// every 3rd pixel of every 3rd row per cell), one log line per cell row as 64 RRGGBB hex triplets, bracketed by BEGIN/END
		// lines; tools/scanout-thumb-png.py turns a log or stream into a PNG. out[1] cols, [2] rows, [3] FNV-1a of the cells,
		// [4] cells with any non-black average, [5] brightest cell luma, [6] mean luma * 1000.
		if (mode == 4) {
			static constexpr uint32_t cols = 64, rows = 36;
			const uint32_t cw = g.width / cols, chh = g.height / rows;
			uint64_t h = 1469598103934665603ull, lumaSum = 0;
			uint32_t lit = 0, maxLuma = 0;
			static uint64_t thumbSeq = 0;
			thumbSeq++;
			RDNA4LOG("scanout-thumb: BEGIN #%llu %ux%u cells of %ux%u px from the %ux%u scanout (writes nothing)", (unsigned long long)thumbSeq,
			       cols, rows, cw, chh, g.width, g.height);
			for (uint32_t r = 0; r < rows; r++) {
				char hex[cols * 6 + 1];
				for (uint32_t c = 0; c < cols; c++) {
					uint64_t sr = 0, sg = 0, sb = 0, n = 0;
					for (uint32_t y = r * chh; y < (r + 1) * chh; y += 3)
						for (uint32_t x = c * cw; x < (c + 1) * cw; x += 3) {
							const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
							sr += (px >> 16) & 0xffu; sg += (px >> 8) & 0xffu; sb += px & 0xffu; n++;
						}
					const uint32_t ar = n ? (uint32_t)(sr / n) : 0u, ag = n ? (uint32_t)(sg / n) : 0u, ab = n ? (uint32_t)(sb / n) : 0u;
					const uint32_t cell = (ar << 16) | (ag << 8) | ab;
					h = (h ^ cell) * 1099511628211ull;
					const uint32_t luma = (299u * ar + 587u * ag + 114u * ab) / 1000u;
					lumaSum += luma;
					if (cell) lit++;
					if (luma > maxLuma) maxLuma = luma;
					static const char hx[] = "0123456789abcdef";
					for (unsigned k = 0; k < 6; k++) hex[c * 6 + k] = hx[(cell >> (20 - 4 * k)) & 0xfu];
				}
				hex[cols * 6] = 0;
				RDNA4LOG("scanout-thumb: #%llu row %02u %s", (unsigned long long)thumbSeq, r, hex);
			}
			RDNA4LOG("scanout-thumb: END #%llu FNV-1a %#018llx, lit cells %u of %u, brightest luma %u, mean luma %llu.%03llu",
			       (unsigned long long)thumbSeq, (unsigned long long)h, lit, cols * rows, maxLuma,
			       (unsigned long long)(lumaSum / (cols * rows)), (unsigned long long)((lumaSum * 1000u / (cols * rows)) % 1000u));
			v[1] = cols; v[2] = rows; v[3] = h; v[4] = lit; v[5] = maxLuma; v[6] = lumaSum * 1000u / (cols * rows);
			goto done;
		}
		// 0.0.345 — MODE 5: THE UNARMED DETILE PROOF. Touches NOTHING outside three scratch VRAM buffers
		// from our own pool: it never reads or writes the scanout, never needs the positive-control interlock, needs no
		// WindowServer, no Apple accelerator and no display pipe, and costs neither a reboot nor a panic.
		// proved the fence fix this way before any armed run; this is the same discipline for the packet.
		//
		// Four measurements, in order, on a 256 x 256 (2 x 2 blocks of 128 x 128) surface at 32 bpp:
		//   1. THE VERDICT. The CPU lays down a KNOWN TILED image using rdna4_addr3_64kb_2d_off_4bpe (the address
		//      equation read out of vendored addrlib, scanout_copy.h). SDMA detiles it with the new 14-dword packet
		//      into a linear buffer. Every one of the 65536 destination pixels is compared, MM window (the GPU's own
		//      view,'s verdict) and BAR0. The pattern is rdna4_tile_probe_pixel = (y<<16)|x, so a wrong dword
		//      NAMES the source pixel it actually came from, and the first six are logged decoded.
		//   2. THE CROSS-CHECK, and it is the one that makes 1 mean anything about the HARDWARE. SDMA tiles the
		//      verified linear result back (detile = 0) into a third buffer, and every position is compared against
		//      the same equation. Agreement = the hardware's ADDR3_64KB_2D layout IS our equation, measured, not
		//      assumed. 1 alone could have passed with a wrong-but-self-consistent equation only if the hardware
		//      shared the same wrong equation; 2 is what closes that.
		//   3. THE NEGATIVE CONTROL. `differs from linear` counts the positions where the tiled address is NOT the
		//      linear one. If SDMA had ignored the swizzle field and done a straight copy, 1 would still pass and 2
		//      would fail at exactly these positions - so a non-zero count here with 2 passing is what rules out
		//      "the swizzle field did nothing".
		//   4. THE ASYMMETRIC FIELD CHECK. A second detile of the SUB-RECTANGLE at tiled (128,128), 128 x 128, into
		//      linear (0,0). A round trip cannot catch a field the hardware reads differently from us if the error
		//      is symmetric between tile and detile; a sub-window at a non-zero tiled origin can, because it moves
		//      tiled_x/tiled_y, the rect and the linear pitch independently of the surface extent.
		if (mode == 5) {
			static constexpr uint32_t kTW = 256u, kTH = 256u, kTN = kTW * kTH;
			const uint64_t linBytes = (uint64_t)kTN * 4u;
			const uint64_t tilBytes = rdna4_addr3_64kb_2d_bytes_4bpe(kTW, kTH);
			uint32_t rptr = 0;
			st = scanout_queue_check(dev, inst, &rptr);
			RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x -> %s",
			       inst.inited, inst.enabled, (unsigned long long)inst.wptr,
			       (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr,
			       st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
			if (st != kScanStOk) goto done;
			if (!gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &a) ||
			    !gBringup.gmc.vram_alloc.alloc(linBytes, 65536, &b) ||
			    !gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &c)) { st = kScanStAlloc; goto done; }
			const uint64_t tOff = a.gpu_va - gBringup.gmc.vram_start;     // tiled, CPU-written from the equation
			const uint64_t dOff = b.gpu_va - gBringup.gmc.vram_start;     // linear destination
			const uint64_t t2Off = c.gpu_va - gBringup.gmc.vram_start;    // tiled, SDMA-written (the cross-check)
			if (tOff + tilBytes > dev.bar0Size || dOff + linBytes > dev.bar0Size ||
			    t2Off + tilBytes > dev.bar0Size || (tOff & 0xffffu) || (t2Off & 0xffffu)) { st = kScanStAlloc; goto done; }
			// The known tiled image, plus poison everywhere the copies must write.
			uint32_t nonLinear = 0;
			for (uint32_t y = 0; y < kTH; y++)
				for (uint32_t x = 0; x < kTW; x++) {
					const uint64_t o = rdna4_addr3_64kb_2d_off_4bpe(x, y, kTW);
					if (o != ((uint64_t)y * kTW + x) * 4u) nonLinear++;
					amdgpu::WBAR0_32(dev, tOff + o, rdna4_tile_probe_pixel(x, y));
				}
			for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
			for (uint64_t i = 0; i < tilBytes / 4u; i++) amdgpu::WBAR0_32(dev, t2Off + i * 4u, 0xdeadbeefu);
			amdgpu::amdgpu_hdp_flush(dev);
			v[2] = tOff; v[3] = ((uint64_t)kTW << 16) | kTH; v[7] = nonLinear;
			// 0.0.346 — THE TWO CONTROLS 0.0.345 LEFT OUT, and they are not optional: had to add
			// exactly these to the row path after 24 runs could not tell "the copy is wrong" from "the buffer was
			// never what we thought". EVERY source position and EVERY destination dword, both ways, BEFORE the copy.
			{
				uint32_t srcMm = 0, srcBar = 0, poisonBad = 0, firstSrc = 0xffffffffu, firstSrcGot = 0;
				bool mmOk = true;
				for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(tOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k2 = 0; k2 < 64u; k2++) {
						// Which (x,y) owns tiled dword (i+k2)? Decode the value instead of inverting the equation:
						// the pattern is self-addressing, so re-deriving the address from it is the check.
						const uint32_t got = mm[k2];
						const uint32_t sx = got & 0xffffu, sy = got >> 16;
						const bool ok = sx < kTW && sy < kTH &&
						                rdna4_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)(i + k2) * 4u;
						if (!ok) {
							if (firstSrc == 0xffffffffu) { firstSrc = i + k2; firstSrcGot = got; }
							srcMm++;
						}
					}
				}
				for (uint32_t i = 0; i < kTN; i++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, tOff + (uint64_t)i * 4u);
					const uint32_t sx = got & 0xffffu, sy = got >> 16;
					if (!(sx < kTW && sy < kTH && rdna4_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)i * 4u)) srcBar++;
					if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
				}
				RDNA4LOG("scanout-tiled: buffers tiled %#llx (MC %#llx, %llu bytes) linear %#llx (MC %#llx) tiled2 %#llx; "
				       "surface %ux%u at 32 bpp, swizzle 3 (ADDR3_64KB_2D); %u of %u position(s) are NOT at their linear "
				       "address (the negative control)", (unsigned long long)tOff, (unsigned long long)a.gpu_va,
				       (unsigned long long)tilBytes, (unsigned long long)dOff, (unsigned long long)b.gpu_va,
				       (unsigned long long)t2Off, kTW, kTH, nonLinear, kTN);
				RDNA4LOG("scanout-tiled: CONTROLS BEFORE THE COPY, every dword both ways: the WHOLE tiled source holds the "
				       "self-addressing pattern - MM WINDOW %s %u of %u wrong, BAR0 %u wrong (first bad tiled dword %u reads "
				       "%#010x); the destination poison landed everywhere - %u of %u dword(s) were NOT 0xdeadbeef. A "
				       "non-zero here voids everything below", mmOk ? "read" : "UNREADABLE", srcMm, kTN, srcBar,
				       firstSrc == 0xffffffffu ? 0u : firstSrc, firstSrcGot, poisonBad, kTN);
				v[9] = (uint64_t)srcMm | ((uint64_t)srcBar << 16) | ((uint64_t)poisonBad << 32);
				if (!mmOk || srcMm || srcBar || poisonBad) { st = kScanStSourceCtl; goto done; }
			}
			RDNA4ScanoutTiledPlan p {};
			p.fbOff = dOff; p.srcOff = tOff; p.surfW = kTW; p.surfH = kTH;
			p.swizzle = amdgpu::kAddr3_64KB_2D; p.elementLog2 = 2u;
			p.srcX = 0; p.srcY = 0; p.dstX = 0; p.dstY = 0;
			p.linPitch = kTW; p.linSlice = kTN; p.w = kTW; p.h = kTH;
			uint32_t detileBad = 0xffffffffu, tileBad = 0xffffffffu, subBad = 0xffffffffu, discBad = 0xffffffffu;
			// 1. THE VERDICT — detile, and when it disagrees, WHERE the bytes came from.
			{
				uint64_t us = 0; uint32_t last = 0;
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D00u, 1000, &us, &last);
				v[4] = us;
				uint32_t mmBad = 0, barBad = 0, poison = 0, firstIdx = 0, firstGot = 0, ns = 0;
				uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 };
				uint32_t bitSet[18] = { 0 }, bucket[32] = { 0 };
				uint64_t loAddr = ~0ull, hiAddr = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!rdna4_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2, dx = idx % kTW, dy = idx / kTW;
							if (mm[k2] == rdna4_tile_probe_pixel(dx, dy)) continue;
							if (!mmBad) { firstIdx = idx; firstGot = mm[k2]; }
							mmBad++;
							if (mm[k2] == 0xdeadbeefu) poison++;
							if (ns < 6) { sIdx[ns] = idx; sGot[ns] = mm[k2]; ns++; }
							// THE MAP: every wrong destination pixel has ONE tiled source address it should have
							// come from. Which bits of that address are always set among the wrong ones NAMES the
							// fault - a single always-set bit is a bit we and the engine disagree about; a
							// contiguous range is a truncation.
							const uint64_t sa = rdna4_addr3_64kb_2d_off_4bpe(dx, dy, kTW);
							if (sa < loAddr) loAddr = sa;
							if (sa > hiAddr) hiAddr = sa;
							for (unsigned bit = 0; bit < 18u; bit++) if (sa & (1ull << bit)) bitSet[bit]++;
							bucket[(sa >> 13) & 31u]++;
						}
					}
					for (uint32_t i = 0; i < kTN; i++)
						if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != rdna4_tile_probe_pixel(i % kTW, i / kTW)) barBad++;
				}
				detileBad = (st == kScanStOk && mmOk) ? mmBad : 0xffffffffu;
				v[1] = (uint64_t)mmBad | ((uint64_t)barBad << 32);
				v[6] = ((uint64_t)firstIdx << 32) | firstGot;
				RDNA4LOG("scanout-tiled: DETILE (one 14-dword COPY_TILED_SUB_WINDOW, detile 1, dcc 0, CPV 0, + FENCE): status %u, "
				       "fence %#010x after %llu us; destination %ux%u compared EVERY pixel - MM WINDOW %s %u wrong of %u "
				       "(still poison %u), BAR0 %u wrong%s", st, last, (unsigned long long)us, kTW, kTH,
				       mmOk ? "read" : "UNREADABLE", mmBad, kTN, poison, barBad,
				       (st == kScanStOk && mmOk && !mmBad) ? " - THE DETILE IS CORRECT AT EVERY POSITION" : " - FAILED");
				if (ns) {
					// What does the SOURCE actually hold at the address each wrong pixel should have come from?
					// Source right + destination wrong = the engine read somewhere else. Both wrong = our write.
					uint32_t sv[6] = { 0 };
					uint64_t sa[6] = { 0 };
					for (unsigned i2 = 0; i2 < ns; i2++) {
						sa[i2] = rdna4_addr3_64kb_2d_off_4bpe(sIdx[i2] % kTW, sIdx[i2] / kTW, kTW);
						(void)rdna4_vram_read_mm(tOff + sa[i2], &sv[i2], 1);
					}
					RDNA4LOG("scanout-tiled: DETILE first %u mismatch(es) — destination (x,y): value READ / value WANTED / the "
					       "tiled source address it should have come from / what the SOURCE HOLDS there right now -- "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x", ns,
					       sIdx[0] % kTW, sIdx[0] / kTW, sGot[0], rdna4_tile_probe_pixel(sIdx[0] % kTW, sIdx[0] / kTW), (unsigned long long)sa[0], sv[0],
					       sIdx[1] % kTW, sIdx[1] / kTW, sGot[1], rdna4_tile_probe_pixel(sIdx[1] % kTW, sIdx[1] / kTW), (unsigned long long)sa[1], sv[1],
					       sIdx[2] % kTW, sIdx[2] / kTW, sGot[2], rdna4_tile_probe_pixel(sIdx[2] % kTW, sIdx[2] / kTW), (unsigned long long)sa[2], sv[2],
					       sIdx[3] % kTW, sIdx[3] / kTW, sGot[3], rdna4_tile_probe_pixel(sIdx[3] % kTW, sIdx[3] / kTW), (unsigned long long)sa[3], sv[3],
					       sIdx[4] % kTW, sIdx[4] / kTW, sGot[4], rdna4_tile_probe_pixel(sIdx[4] % kTW, sIdx[4] / kTW), (unsigned long long)sa[4], sv[4],
					       sIdx[5] % kTW, sIdx[5] / kTW, sGot[5], rdna4_tile_probe_pixel(sIdx[5] % kTW, sIdx[5] / kTW), (unsigned long long)sa[5], sv[5]);
					RDNA4LOG("scanout-tiled: DETILE MAP — of %u wrong pixel(s), the tiled source address spans %#07llx..%#07llx; "
					       "wrong pixels whose source address has bit N set, N=2..17: %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
					       "%u %u (a count equal to %u means EVERY wrong pixel has that bit set, 0 means none do)",
					       mmBad, (unsigned long long)(loAddr == ~0ull ? 0 : loAddr), (unsigned long long)hiAddr,
					       bitSet[2], bitSet[3], bitSet[4], bitSet[5], bitSet[6], bitSet[7], bitSet[8], bitSet[9],
					       bitSet[10], bitSet[11], bitSet[12], bitSet[13], bitSet[14], bitSet[15], bitSet[16], bitSet[17], mmBad);
					RDNA4LOG("scanout-tiled: DETILE MAP — wrong pixels per 8 KiB of tiled source address (32 buckets, %#x total): "
					       "%u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u",
					       (unsigned)tilBytes, bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5], bucket[6], bucket[7],
					       bucket[8], bucket[9], bucket[10], bucket[11], bucket[12], bucket[13], bucket[14], bucket[15],
					       bucket[16], bucket[17], bucket[18], bucket[19], bucket[20], bucket[21], bucket[22], bucket[23],
					       bucket[24], bucket[25], bucket[26], bucket[27], bucket[28], bucket[29], bucket[30], bucket[31]);
				}
				if (st != kScanStOk) goto done;
			}
			// 2. THE CROSS-CHECK — tile the verified linear result back and compare against the same equation.
			// 0.0.346: this RUNS EVEN WHEN THE DETILE FAILED. A detile that is wrong while the tile direction is right
			// is a different fault from both being wrong, and 0.0.345 threw that distinction away by aborting first.
			// The linear buffer is re-seeded by the CPU so the tile direction starts from a KNOWN image either way.
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r2 = 0;
				st = scanout_queue_check(dev, inst, &r2);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the detile (status %u, rptr %#x) - "
				                              "REFUSING the cross-check", st, r2); goto done; }
				for (uint32_t y = 0; y < kTH; y++)
					for (uint32_t x = 0; x < kTW; x++)
						amdgpu::WBAR0_32(dev, dOff + ((uint64_t)y * kTW + x) * 4u, rdna4_tile_probe_pixel(x, y));
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, false, 0x5CA11D01u, 1000, &us, &last);
				v[4] |= us << 32;
				uint32_t bad = 0, stillPoison = 0, fx = 0, fy = 0, fgot = 0;
				if (st == kScanStOk)
					for (uint32_t y = 0; y < kTH; y++)
						for (uint32_t x = 0; x < kTW; x++) {
							const uint32_t got = amdgpu::RBAR0_32(dev, t2Off + rdna4_addr3_64kb_2d_off_4bpe(x, y, kTW));
							if (got == rdna4_tile_probe_pixel(x, y)) continue;
							if (!bad) { fx = x; fy = y; fgot = got; }
							if (got == 0xdeadbeefu) stillPoison++;
							bad++;
						}
				tileBad = st == kScanStOk ? bad : 0xffffffffu;
				v[5] = bad;
				RDNA4LOG("scanout-tiled: TILE BACK (detile 0, same packet, same swizzle, from a CPU-written linear image): "
				       "status %u, fence %#010x after %llu us; %u of %u position(s) are NOT where the addrlib equation says "
				       "(%u still hold the 0xdeadbeef poison, i.e. never written) - %s. First disagreement (%u,%u) reads "
				       "%#010x. With %u position(s) differing from linear, a pass here also rules out SDMA ignoring the "
				       "swizzle field", st, last, (unsigned long long)us, bad, kTN, stillPoison,
				       (st == kScanStOk && !bad) ? "THE WRITE SIDE AGREES WITH THE EQUATION" : "FAILED", fx, fy, fgot, nonLinear);
				if (st != kScanStOk) goto done;
			}
			// 2b. 0.0.347 — THE DISCRIMINATOR. Detile the tiled image SDMA ITSELF just wrote, which
			// TILE BACK has verified byte-for-byte against the equation, into the same linear buffer. The ONLY thing
			// that changes from step 1 is WHO WROTE THE TILED SOURCE: the CPU through BAR0, or the engine through
			// its own path. Same packet, same fields, same swizzle, same destination, same addresses.
			//   clean here + dirty in step 1  -> the packet and the equation are correct and the fault is that the
			//                                    engine does not see the CPU's writes to that buffer
			//   dirty here too               -> the tiled READ path itself is wrong and the packet is not usable
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r4 = 0;
				st = scanout_queue_check(dev, inst, &r4);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle before the discriminator (status %u, "
				                              "rptr %#x) - REFUSING", st, r4); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D03u, 1000, &us, &last);
				uint32_t bad = 0, poison2 = 0, firstIdx = 0, firstGot = 0;
				bool mmOk = true;
				if (st == kScanStOk)
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!rdna4_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2;
							if (mm[k2] == rdna4_tile_probe_pixel(idx % kTW, idx / kTW)) continue;
							if (!bad) { firstIdx = idx; firstGot = mm[k2]; }
							if (mm[k2] == 0xdeadbeefu) poison2++;
							bad++;
						}
					}
				discBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[8] = bad;
				RDNA4LOG("scanout-tiled: DISCRIMINATOR — the SAME detile packet, the SAME destination, but the tiled source is "
				       "the one SDMA WROTE (%#llx, verified byte-for-byte by TILE BACK) instead of the one the CPU wrote "
				       "(%#llx): status %u, fence %#010x after %llu us; MM WINDOW %s %u of %u wrong (%u still poison), first "
				       "(%u,%u) reads %#010x. %s", (unsigned long long)t2Off, (unsigned long long)tOff, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, kTN, poison2,
				       firstIdx % kTW, firstIdx / kTW, firstGot,
				       (st == kScanStOk && mmOk && !bad)
				           ? "CLEAN: the packet and the equation are CORRECT; step 1's failure is the engine not seeing the "
				             "CPU's writes to that buffer, not the tiling"
				           : "ALSO DIRTY: the tiled READ path is wrong independently of who wrote the source");
				if (st != kScanStOk) goto done;
			}
			// 3. THE ASYMMETRIC FIELD CHECK — a sub-window at a non-zero tiled origin, out of the CPU-written source.
			{
				RDNA4ScanoutTiledPlan q = p;
				q.srcX = 128; q.srcY = 128; q.dstX = 0; q.dstY = 0; q.w = 128; q.h = 128;
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r3 = 0;
				st = scanout_queue_check(dev, inst, &r3);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the tile back (status %u, rptr %#x) - "
				                              "REFUSING the sub-window check", st, r3); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &q, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D02u, 1000, &us, &last);
				uint32_t bad = 0, outside = 0, fx = 0, fy = 0, fgot = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t y = 0; y < q.h && mmOk; y++)
						for (uint32_t x = 0; x < q.w; x += 64) {
							uint32_t mm[64] = { 0 };
							if (!rdna4_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + x) * 4u, mm, 64)) { mmOk = false; break; }
							for (uint32_t k2 = 0; k2 < 64u; k2++) {
								if (mm[k2] == rdna4_tile_probe_pixel(x + k2 + q.srcX, y + q.srcY)) continue;
								if (!bad) { fx = x + k2; fy = y; fgot = mm[k2]; }
								bad++;
							}
						}
					for (uint32_t y = 0; y < 4u; y++) {
						uint32_t one = 0;
						if (rdna4_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + q.w) * 4u, &one, 1) && one != 0xdeadbeefu) outside++;
					}
				}
				subBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[5] |= (uint64_t)bad << 32;
				RDNA4LOG("scanout-tiled: SUB-WINDOW detile of tiled (%u,%u) %ux%u into linear (0,0) pitch %u: status %u, fence "
				       "%#010x after %llu us; MM WINDOW %s %u of %u wrong%s (first (%u,%u) reads %#010x); bytes written past "
				       "the rectangle in the first 4 rows: %u of 4 (must be 0)", q.srcX, q.srcY, q.w, q.h, q.linPitch, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, q.w * q.h,
				       (st == kScanStOk && mmOk && !bad && !outside) ? " - EVERY PIXEL OF THE SUB-WINDOW IS CORRECT" : " - FAILED",
				       fx, fy, fgot, outside);
				v[10] = outside;
				if (st != kScanStOk) goto done;
			}
			RDNA4LOG("scanout-tiled: SUMMARY — detile (CPU-written source) %u wrong, tile back %u wrong, DISCRIMINATOR "
			       "(SDMA-written source) %u wrong, sub-window %u wrong, of %u each (0xffffffff = the step did not run). "
			       "None of them says whether Apple's composited surface is in THIS mode - that is what an armed run asks",
			       detileBad, tileBad, discBad, subBad, kTN);
			if (detileBad || tileBad || subBad || discBad || v[10]) st = kScanStReadback;
			goto done;
		}
		// 0.0.414 (an internal design note) — MODE 6: THE FULL-GEOMETRY SDMA SELF-TEST.
		// ONE call, two cases IN ORDER (S1): (a) 256x256, the control, and (b) 1920x1080, the live plane's
		// exact packet fields (swizzle 3, pitch 1920, 15 blocks wide, a partial 9th block row). Both run S3/S4's
		// method on OUR OWN three scratch buffers and never touch the scanout or Apple's surfaces. Mode 5's
		// 256x256 proof is byte-for-byte unchanged; this mode is purely additive. The maps and the decoded
		// pixels are in the `scanout-full:` driver-log lines (S6, every line < 512 bytes); out[] carries the
		// per-case counts: 1|2 case0 tile|detile wrong, poison|bar-mm, tile|detile sampled; 4|5|6 the same for
		// case1; 7|8 case tile-status|detile-status.
		if (mode == 6) {
			RDNA4FullCase c0 {}, c1 {};
			// build 0.0.514 B3: case (b) is the LIVE geometry (Console,Width/Height, read above into `g`),
			// 1920x1080 only when it is absent - 0.0.513's constant, so a 1080p boot runs exactly 0.0.513's case.
			const uint32_t lw = rdna4_live_dim(g.width, 1920u), lh = rdna4_live_dim(g.height, 1080u);
			const uint32_t s0 = scanout_full_case(dev, inst, 256u, 256u, 0x5CA11F00u, &c0, false);
			const uint32_t s1 = scanout_full_case(dev, inst, lw, lh, 0x5CA11F10u, &c1, false);
			v[1] = (uint64_t)c0.tileWrong | ((uint64_t)c0.detWrong << 32);
			v[2] = (uint64_t)c0.poison | ((uint64_t)c0.barMm << 32);
			v[3] = (uint64_t)c0.tileSampled | ((uint64_t)c0.detSampled << 32);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[7] = ((uint64_t)c0.tileStatus << 32) | c0.detStatus;
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s0 != kScanStOk ? s0 : (s1 != kScanStOk ? s1 : kScanStOk);
			// The 256x256 control must still pass. A dirty live case is a MEASUREMENT, not a refusal.
			if (st == kScanStOk && (c0.tileWrong || c0.detWrong || c0.poison)) st = kScanStReadback;
			RDNA4LOG("scanout-full: SUMMARY mode 6 — control 256x256 tile %u/%u, detile %u/%u, poison %u; live "
			       "%ux%u tile %u/%u, detile %u/%u, poison %u; statuses %u/%u and %u/%u -> %u",
			       c0.tileWrong, c0.tileSampled, c0.detWrong, c0.detSampled, c0.poison, lw, lh,
			       c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled, c1.poison,
			       c0.tileStatus, c0.detStatus, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// D7 (0.0.417, an internal design note) — MODE 8: THE UNIFORM PROBE. Mode 6's live 1920x1080 case
		// EXACTLY - same buffers, same packet fields, same bounded sample set, same `scanout-full:` report lines and
		// the same POISON detection - except the CPU writes RDNA4_TILE_UNIFORM_PIXEL (0xff00ff00) everywhere and every
		// expected value is that value. It is the write-compression half of the DCC fault: with our no-PTE write
		// compression ON a constant 256-byte block is stored as a code the raw MM window reads instead of the pixels;
		// after `sdmadcc 1` the same probe must read 0 wrong. out[4..6] are the unused control slots (0); the live
		// counts are out[7..9] and the statuses out[11], exactly where mode 6 leaves its case-1 counts.
		if (mode == 8) {
			RDNA4FullCase c1 {};
			const uint32_t s1 = scanout_full_case(dev, inst, 1920u, 1080u, 0x5CA11F10u, &c1, true);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s1;
			RDNA4LOG("scanout-full: SUMMARY mode 8 (UNIFORM probe %#010x) 1920x1080 tile %u/%u, detile %u/%u, poison %u; "
			       "statuses %u/%u -> %u; the same test with `sdmadcc 1` must read 0 wrong",
			       (unsigned)RDNA4_TILE_UNIFORM_PIXEL, c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled,
			       c1.poison, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// 0.0.416 (an internal design note, G2) — MODE 7: the SDMA cache-rinse instrument. READ-ONLY on the
		// source; our own low scratch only; the same interlock-free proof discipline as modes 5/6 (three/four
		// scratch buffers, no WindowServer, no Apple accelerator, no reboot). out: 1 gcr|control<<1, 2 source
		// VRAM offset, 3 scratch VRAM offset, 4 fence us | copy dwords<<32, 5 lines full|part<<16|diff<<32,
		// 6 differing dwords, 7 the 64-page difference map. The maps and first-8 decode are in the log.
		if (mode == 7) {
			st = scanout_gcr_case(dev, inst, arg, v);
			goto done;
		}
		if (mode != 1) { st = kScanStPlan; goto done; }
		gScanoutPcRuns++;
		uint32_t rptr = 0;
		st = scanout_queue_check(dev, inst, &rptr);
		RDNA4LOG("scanout: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x (bytes) -> %s", inst.inited,
		       inst.enabled, (unsigned long long)inst.wptr, (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr, st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
		if (st != kScanStOk) goto done;
		const uint32_t bytes = kScanPcW * kScanPcH * 4u;
		if (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &a) || !gBringup.gmc.vram_alloc.alloc(bytes, 16384, &b)) {
			st = kScanStAlloc; goto done;
		}
		const uint64_t aOff = a.gpu_va - gBringup.gmc.vram_start, bOff = b.gpu_va - gBringup.gmc.vram_start;
		if (aOff + bytes > dev.bar0Size || bOff + bytes > dev.bar0Size) { st = kScanStAlloc; goto done; }
		// The pattern, and a poisoned scratch buffer.
		for (uint32_t y = 0; y < kScanPcH; y++)
			for (uint32_t x = 0; x < kScanPcW; x++) {
				amdgpu::WBAR0_32(dev, aOff + ((uint64_t)y * kScanPcW + x) * 4u, rdna4_pc_pixel(x, y, kScanPcW));
				amdgpu::WBAR0_32(dev, bOff + ((uint64_t)y * kScanPcW + x) * 4u, 0xdeadbeefu);
			}
		amdgpu::amdgpu_hdp_flush(dev);
		// Source positive control, CPU view (every pixel) and the GPU's own view (MM window, row 0).
		{
			uint32_t bad = 0, mm[64] = { 0 }, mmBad = 0;
			for (uint32_t i = 0; i < kScanPcW * kScanPcH; i++)
				if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad++;
			const bool mmOk = rdna4_vram_read_mm(aOff, mm, 64);
			for (uint32_t x = 0; x < 64; x++) if (mm[x] != rdna4_pc_pixel(x, 0, kScanPcW)) mmBad++;
			RDNA4LOG("scanout: pattern buffer VRAM %#llx (MC %#llx), scratch VRAM %#llx (MC %#llx), %u bytes each; source "
			       "control: BAR0 mismatches %u of %u, MM-window row 0 %s mismatches %u of 64 (mm[16] %#010x)",
			       (unsigned long long)aOff, (unsigned long long)a.gpu_va, (unsigned long long)bOff,
			       (unsigned long long)b.gpu_va, bytes, bad, kScanPcW * kScanPcH, mmOk ? "read" : "UNREADABLE", mmBad, mm[16]);
			if (bad || !mmOk || mmBad) { st = kScanStSourceCtl; goto done; }
		}
		// Pre-flight: pattern -> scratch, nothing near the scanout.
		//
		// 0.0.337 — THE INSTRUMENT. left this as the unresolved limitation: the compare was a bare
		// `bad++` with no index, no value and no second opinion, so for 24 runs "our SDMA copy corrupted 7904 of 16384
		// pixels" and "the CPU's BAR0 view of a perfectly copied buffer is stale" were indistinguishable — two faults
		// with nothing in common and no shared fix. The same bytes are now read FOUR ways:
		//   1. BAR0, first pass, with every mismatch CLASSIFIED — 0xdeadbeef is the poison this function wrote before
		//      the copy (so: never written, or a stale read of the pre-copy value); the right colour with a wrong row
		//      byte is an addressing fault; anything else is real corruption — and the first six logged with index,
		//      value read and value expected.
		//   2. BAR0 again immediately, nothing in between: a difference means the first pass was early or stale.
		//   3. BAR0 once more after amdgpu_hdp_flush — the SAME call this function already makes at the top of this
		//      block and again inside scanout_sdma_copies. No new register and no new register offset is written.
		//   4. THE MM WINDOW over the whole buffer — MM_INDEX/MM_DATA, the GPU's own view of VRAM, which this file
		//      already designates as the right instrument for memory written by SDMA rather than by a CPU (the comment
		//      on rdna4_vram_read_mm) and which the source control 20 lines above already uses on this same buffer.
		// THE MM WINDOW IS THE VERDICT. That is not a relaxation of the interlock. What the interlock protects is that
		// OUR SDMA0 QUEUE0 copy puts the right bytes in VRAM, and the MM window reads VRAM; a BAR0 aperture read of
		// memory a GPU engine has just written, in a driver that issues no HDP read-cache invalidate anywhere, is the
		// weaker instrument, not the stronger one. Every BAR0 number is still measured, logged and returned, so the two
		// paths can never disagree silently, and if the copy is genuinely wrong BOTH fail and the interlock still holds.
		{
			uint64_t us = 0; uint32_t last = 0;
			uint32_t bad = 0, bad2 = 0, bad3 = 0, mmBad = 0, poison = 0, colour = 0, other = 0;
			uint32_t firstIdx = 0, firstGot = 0, ns = 0;
			uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 }, sWant[6] = { 0 };
			bool mmOk = true;
			const uint32_t total = kScanPcW * kScanPcH;
			const uint64_t s1 = a.gpu_va, d1 = b.gpu_va; const uint32_t n1 = bytes;
			// 0.0.338: two gaps 0.0.337 left open, closed BEFORE the copy runs.
			//  - `poisonBad` — nothing ever verified that the 0xdeadbeef poison actually LANDED. "poison 0
			//    after the copy" only means "the copy wrote everywhere" if the poison was everywhere first.
			//  - `srcAfter` (below, after the copy) — nothing ever re-read the SOURCE afterwards. If anything
			//    else on this machine writes VRAM under us, the source is where it would show.
			uint32_t poisonBad = 0, srcAfter = 0;
			for (uint32_t i = 0; i < total; i++)
				if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
			uint32_t alien = 0, dRowOther = 0, dRowHist[7] = { 0 }, dBarHist[16] = { 0 }, rowBad[kScanPcH] = { 0 };
			char map[kScanPcH + 1];
			st = scanout_sdma_copies(dev, inst, &s1, &d1, &n1, 1, 0x5CA11ED0u, 500, &us, &last);
			if (st == kScanStOk) {
				for (uint32_t i = 0; i < total; i++) {
					const uint32_t want = rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW);
					const uint32_t got = amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u);
					if (got == want) continue;
					if (!bad) { firstIdx = i; firstGot = got; }
					bad++;
					if (got == 0xdeadbeefu) poison++;
					else if ((got & 0x00ffffffu) == (want & 0x00ffffffu)) colour++;
					else other++;
					if (ns < 6) { sIdx[ns] = i; sGot[ns] = got; sWant[ns] = want; ns++; }
					// Where did this dword come from? Decode the value back into the source pixel that owns it.
					rowBad[i / kScanPcW]++;
					const int sb = scanout_bar_index(got);
					const uint32_t sy = got >> 24;
					if (sb < 0 || sy >= kScanPcH) { alien++; continue; }
					const int dRow = (int)sy - (int)(i / kScanPcW);
					const int dBar = sb - (int)((i % kScanPcW) / 32u);
					if (dRow >= -3 && dRow <= 3) dRowHist[dRow + 3]++; else dRowOther++;
					dBarHist[(dBar + 8) & 15]++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad2++;
				amdgpu::amdgpu_hdp_flush(dev);
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad3++;
				for (uint32_t i = 0; i < total && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(bOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != rdna4_pc_pixel((i + k) % kScanPcW, (i + k) / kScanPcW, kScanPcW)) mmBad++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) srcAfter++;
			}
			for (uint32_t y = 0; y < kScanPcH; y++)
				map[y] = rowBad[y] == 0 ? '.' : (rowBad[y] >= kScanPcW ? '#' : "0123456789abcdef"[(rowBad[y] * 16u) / kScanPcW]);
			map[kScanPcH] = 0;
			v[4] = (uint64_t)bad | ((uint64_t)mmBad << 32);
			v[5] = us << 32;
			v[1] |= ((uint64_t)(bad2 & 0xffffu) << 16) | ((uint64_t)(bad3 & 0xffffu) << 32) |
			        ((uint64_t)(poison & 0xffffu) << 48);
			v[7] = ((uint64_t)firstIdx << 32) | firstGot;    // pre-flight only; the rectangle readback overwrites it if we get there
			RDNA4LOG("scanout: PRE-FLIGHT copy pattern -> scratch: status %u, fence %#010x after %llu us; BAR0 pass 1 %u of "
			       "%u wrong (poison 0xdeadbeef %u, right colour wrong row byte %u, other %u), BAR0 pass 2 (nothing in "
			       "between) %u, BAR0 pass 3 (after amdgpu_hdp_flush) %u; MM WINDOW %s %u of %u wrong. THE MM WINDOW IS "
			       "THE VERDICT : it is the GPU's own view of the VRAM the copy wrote, and no HDP "
			       "read-cache invalidate exists on the BAR0 path", st, last, (unsigned long long)us, bad, total,
			       poison, colour, other, bad2, bad3, mmOk ? "read" : "UNREADABLE", mmBad, total);
			RDNA4LOG("scanout: PRE-FLIGHT MAP, one char per 256-pixel row of the scratch buffer ('.' = that row is entirely "
			       "correct, '#' = all 256 wrong, otherwise the hex of wrong*16/256): %s", map);
			RDNA4LOG("scanout: PRE-FLIGHT DISPLACEMENT — every pattern value names its own source (colour = source x/32, top "
			       "byte = source row), so a wrong dword says where it came from: alien (not any bar value) %u; source-row "
			       "delta -3..+3 = %u %u %u %u %u %u %u and |delta|>3 %u; source-bar delta -8..+7 = %u %u %u %u %u %u %u %u "
			       "%u %u %u %u %u %u %u %u. CONTROLS THIS RUN: poison landed everywhere? %u of %u dwords were NOT 0xdeadbeef "
			       "before the copy; source still intact after the copy? %u of %u wrong",
			       alien, dRowHist[0], dRowHist[1], dRowHist[2], dRowHist[3], dRowHist[4], dRowHist[5], dRowHist[6], dRowOther,
			       dBarHist[0], dBarHist[1], dBarHist[2], dBarHist[3], dBarHist[4], dBarHist[5], dBarHist[6], dBarHist[7],
			       dBarHist[8], dBarHist[9], dBarHist[10], dBarHist[11], dBarHist[12], dBarHist[13], dBarHist[14], dBarHist[15],
			       poisonBad, total, srcAfter, total);
			RDNA4LOG("scanout: PRE-FLIGHT first %u mismatch(es) as index: read / expected -- %u: %#010x/%#010x, %u: %#010x/%#010x, "
			       "%u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x", ns,
			       sIdx[0], sGot[0], sWant[0], sIdx[1], sGot[1], sWant[1], sIdx[2], sGot[2], sWant[2],
			       sIdx[3], sGot[3], sWant[3], sIdx[4], sGot[4], sWant[4], sIdx[5], sGot[5], sWant[5]);
			if (st != kScanStOk) goto done;
			if (!mmOk || mmBad) { st = kScanStPreflight; goto done; }
		}
		// Plan the rectangle, save what is there, copy, read every pixel back.
		RDNA4ScanoutPlan plan {};
		{
			const uint32_t pr = rdna4_scanout_plan_rows(&g, aOff, bytes, kScanPcW, kScanPcH, kScanPcW * 4u, 0, 0,
			                                          kScanPcX, kScanPcY, kScanPcW, kScanPcH, kScanMaxRows, &plan);
			v[1] |= (uint64_t)pr << 8;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != kScanPcW || plan.h != kScanPcH) { st = kScanStPlan; goto done; }
		}
		if (!gScanoutSave) gScanoutSave = static_cast<uint32_t *>(IOMalloc(kScanPcW * kScanPcH * sizeof(uint32_t)));
		if (gScanoutSave) {
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++)
					gScanoutSave[y * plan.w + x] = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
			gScanoutSaveX = plan.dstX; gScanoutSaveY = plan.dstY; gScanoutSaveW = plan.w; gScanoutSaveH = plan.h;
		}
		{
			uint64_t srcMc[kScanPcH], dstMc[kScanPcH]; uint32_t nb[kScanPcH];
			for (uint32_t i = 0; i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; goto done; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0;
			st = scanout_sdma_copies(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11ED1u, 1000, &us, &last);
			v[5] |= us;
			RDNA4LOG("scanout: POSITIVE CONTROL copy %u rows x %u bytes, MC %#llx.. -> MC %#llx.. (rect %u,%u %ux%u): "
			       "status %u, fence %#010x after %llu us", plan.h, plan.rowBytes, (unsigned long long)srcMc[0],
			       (unsigned long long)dstMc[0], plan.dstX, plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us);
			if (st != kScanStOk) goto done;
		}
		{
			uint32_t bad = 0, firstIdx = 0, firstVal = 0;
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
					if (got != rdna4_pc_pixel(x, y, kScanPcW)) { if (!bad) { firstIdx = y * plan.w + x; firstVal = got; } bad++; }
				}
			// 0.0.337: the same second opinion the pre-flight now takes. The rectangle is inside
			// RDNA4FB's scanout, written by the same SDMA0 QUEUE0 copy and read back through the same BAR0
			// aperture — so if the pre-flight's BAR0 view was stale this one is too. The MM window decides here
			// as well; the BAR0 count and the four named samples are still measured and still reported, so a
			// disagreement between the two paths is visible, never silent.
			uint32_t mmBad = 0; bool mmOk = true;
			for (uint32_t y = 0; y < plan.h && mmOk; y++) {
				const uint64_t rowOff = rdna4_scanout_row_dst(&plan, g.rowBytes, y);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(rowOff + (uint64_t)x * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != rdna4_pc_pixel(x + k, y, kScanPcW)) mmBad++;
				}
			}
			v[6] = (uint64_t)bad | ((uint64_t)mmBad << 32); v[7] = ((uint64_t)firstIdx << 32) | firstVal;
			static const uint32_t sx[4] = { 16, 112, 240, 16 }, sy[4] = { 0, 0, 0, 63 };
			for (unsigned i = 0; i < 4; i++) {
				const uint32_t got = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, sy[i]) + (uint64_t)sx[i] * 4u);
				v[8 + i] = ((uint64_t)rdna4_pc_pixel(sx[i], sy[i], kScanPcW) << 32) | got;
			}
			RDNA4LOG("scanout: READBACK of the scanout rectangle: BAR0 %u of %u pixels differ from the bars; MM WINDOW %s "
			       "%u of %u differ%s; samples through BAR0 (expected/read) row0 x16 %#010x/%#010x, row0 x112 "
			       "%#010x/%#010x, row0 x240 %#010x/%#010x, row63 x16 %#010x/%#010x", bad, plan.w * plan.h,
			       mmOk ? "read" : "UNREADABLE", mmBad, plan.w * plan.h,
			       (!mmOk || mmBad) ? " - FAILED" : " - EVERY PIXEL MATCHES IN THE GPU'S OWN VIEW",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], (uint32_t)(v[11] >> 32), (uint32_t)v[11]);
			if (!mmOk || mmBad) { st = kScanStReadback; goto done; }
			gScanoutPcPassed = true;
			gScanoutCopies++;
		}
	}
done:
	scanout_free_vram(a);
	scanout_free_vram(b);
	scanout_free_vram(c);
	if (st != kScanStOk && mode != 0) { gScanoutRefusals++; gScanoutLastReason = st; }
	IOLockUnlock(gScanoutLock);
done_nolock:
	v[0] = st;
	if (mode != 3 && mode != 4 && mode != RDNA4_SF_MODE && mode != RDNA4_SF_MODE_RELEASE)   // 0.0.280: modes 3/4 (0.0.542: 9/10) keep their own out[12]
		v[12] = (gScanoutPcPassed ? 1u : 0u) | ((uint64_t)(gScanoutPcRuns & 0xffff) << 8) |
		        ((uint64_t)(gScanoutCopies & 0xffff) << 24) | ((uint64_t)(gScanoutRefusals & 0xffff) << 40);
	RDNA4LOG("scanout: mode %u -> status %u; interlock %s (positive-control runs %u, verified copies %u, refusals %u)",
	       mode, st, gScanoutPcPassed ? "PASSED" : "not passed", gScanoutPcRuns, gScanoutCopies, gScanoutRefusals);
	if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
	return st;
}

// 0.0.412 — S2's RICHER READBACK. One sample per 16x16 destination cell (256 samples), compared with the
// source at the same geometric position: the tiled path detiles with the SAME equation the copy used, the linear path
// reads (stride, x). Read-only. The caller has already bounded how often this runs (rdna4_dpg_verify_due). Prints ONE
// line, under 480 body bytes: how many samples differ, the 256-bit cell map as four 64-bit words, and the cell and
// dst/src values of the first 8 differing samples. The cells, centres and bit packing are display_pipe_guard.h's pure
// helpers; this function only does the reads.
struct RDNA4VerifySource {
	uint64_t off; uint32_t stride; uint32_t surfW; uint32_t srcX, srcY; int tiled;
};
static void rdna4_scanout_verify_cells(amdgpu::DeviceContext &dev, uint64_t fbOff, uint32_t dstX, uint32_t dstY,
                                     uint32_t w, uint32_t h, uint32_t linPitch, const RDNA4VerifySource &s)
{
	if (w == 0 || h == 0 || linPitch == 0) return;
	uint64_t map[RDNA4_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
	uint32_t differ = 0, fx[RDNA4_DPG_VERIFY_FIRST] = { 0 }, fd[RDNA4_DPG_VERIFY_FIRST] = { 0 },
	         fs[RDNA4_DPG_VERIFY_FIRST] = { 0 };
	for (uint32_t cy = 0; cy < RDNA4_DPG_VERIFY_CELLS; cy++) {
		for (uint32_t cx = 0; cx < RDNA4_DPG_VERIFY_CELLS; cx++) {
			uint32_t x = 0, y = 0;
			rdna4_dpg_cell16_centre(cx, cy, w, h, &x, &y);
			uint32_t sv = 0;
			const uint64_t so = s.tiled
			    ? s.off + rdna4_addr3_64kb_2d_off_4bpe(s.srcX + x, s.srcY + y, s.surfW)
			    : s.off + (uint64_t)(s.srcY + y) * s.stride + (uint64_t)(s.srcX + x) * 4u;
			const bool sok = rdna4_vram_read_mm(so, &sv, 1);
			const uint64_t d = fbOff + ((uint64_t)(dstY + y) * linPitch + (dstX + x)) * 4u;
			const uint32_t dv = amdgpu::RBAR0_32(dev, d);
			if (!sok || dv != sv) {
				rdna4_dpg_verify_map_set(map, rdna4_dpg_cell16(x, y, w, h));
				if (differ < RDNA4_DPG_VERIFY_FIRST) { fx[differ] = cy * RDNA4_DPG_VERIFY_CELLS + cx; fd[differ] = dv; fs[differ] = sv; }
				differ++;
			}
		}
	}
	RDNA4LOG("scanout-copy: S2 16x16 %s %ux%u: %u of %u samples differ; map %016llx%016llx%016llx%016llx; first %u: "
	       "c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x "
	       "(cell=dst/src, src=%s)", s.tiled ? "tiled" : "linear", w, h, differ, RDNA4_DPG_VERIFY_SAMPLES,
	       (unsigned long long)map[0], (unsigned long long)map[1], (unsigned long long)map[2], (unsigned long long)map[3],
	       differ, fx[0], fd[0], fs[0], fx[1], fd[1], fs[1], fx[2], fd[2], fs[2], fx[3], fd[3], fs[3],
	       fx[4], fd[4], fs[4], fx[5], fd[5], fs[5], fx[6], fd[6], fs[6], fx[7], fd[7], fs[7],
	       s.tiled ? "tiled equation" : "linear rows");
}

// The surface copy, called from the flush hook (GarnetAccelPeer.cpp) AFTER Apple's own selector 10 returned.
// Source = a VRAM buffer [srcOff, srcOff+srcLen) holding srcW x srcH pixels at srcStride. Copies the rectangle
// (0,0,w,h) of it to (dstX,dstY), verifies rows 0 and h-1 in full (destination through BAR0, source through
// BAR0 when it is inside the aperture, else the MM window) and nine samples against the test client's pattern.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] row mismatches, [6] rows compared (px),
// [7] client-pattern matches<<32 | samples, [8..10] samples dst<<32|src (0,0), (w/2,h/2), (w-1,h-1).
uint32_t rdna4_scanout_copy_vram(uint64_t srcOff, uint64_t srcLen, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                                  uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                  bool verify) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	bool logit = false;   // 0.0.278
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutPlan plan {};
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_rows(&g, srcOff, srcLen, srcW, srcH, srcStride, 0, 0, dstX, dstY, w, h,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11E00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				RDNA4LOG("scanout-copy: surface VRAM %#llx (+%#llx) %ux%u stride %u -> rect (%u,%u) %ux%u: %u packet(s) in %u "
				       "submission(s), first MC %#llx -> %#llx; status %u, last fence %#010x, %llu us waited (copy call #%u)",
				       (unsigned long long)srcOff, (unsigned long long)srcLen, srcW, srcH, srcStride, plan.dstX, plan.dstY,
				       plan.w, plan.h, plan.h, chunks, (unsigned long long)(srcMc ? srcMc[0] : 0),
				       (unsigned long long)(dstMc ? dstMc[0] : 0), st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// 0.0.288: the pipe shim's present asks for the copy WITHOUT this readback (a BAR0 read is ~0.57 MiB/s,).
		// The copy above - plan, per-row rdna4_scanout_row_dst_ok, fence - is identical either way; only the comparison
		// after it is skipped, and an unverified copy does NOT count towards gScanoutCopies.
		if (st == kScanStOk && !verify) {
			v[5] = 0; v[6] = 0; v[7] = 0;
		} else if (st == kScanStOk) {
			// Rows 0 and h-1 in full, 64 dwords per source read.
			const bool srcInBar = srcOff + srcLen <= dev.bar0Size;
			uint32_t bad = 0, compared = 0, clientOk = 0, samples = 0;
			const uint32_t rows[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				uint64_t so = rdna4_scanout_row_src(&plan, rows[r]);
				const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rows[r]);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t chunk[64] = { 0 };
					const uint32_t nw = plan.w - x < 64 ? plan.w - x : 64;
					bool okr = true;
					if (srcInBar) { for (uint32_t i = 0; i < nw; i++) chunk[i] = amdgpu::RBAR0_32(dev, so + (uint64_t)(x + i) * 4u); }
					else okr = rdna4_vram_read_mm(so + (uint64_t)x * 4u, chunk, nw);
					for (uint32_t i = 0; i < nw; i++) {
						compared++;
						if (!okr || amdgpu::RBAR0_32(dev, dor + (uint64_t)(x + i) * 4u) != chunk[i]) bad++;
					}
				}
				(void)so;
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				const uint64_t so = rdna4_scanout_row_src(&plan, py[i]) + (uint64_t)px[i] * 4u;
				if (srcInBar) sv = amdgpu::RBAR0_32(dev, so); else rdna4_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				samples++;
				if (sv == rdna4_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | samples;
			if (logit || bad)
			RDNA4LOG("scanout-copy: READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against source (%s): "
			       "%u differ%s; samples dst/src (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; "
			       "the source holds the test client's pattern at %u of %u samples", plan.h - 1, compared,
			       srcInBar ? "BAR0" : "MM window", bad, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk, samples);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source read linearly (this path's
			// own addressing). The linear copy is byte-faithful, so a uniform source reads uniform here whatever the
			// source's true layout - which is exactly what makes S1's forced-linear result decidable.
			{
				RDNA4VerifySource vs;
				vs.off = plan.srcOff; vs.stride = plan.srcStride; vs.surfW = 0; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 0;
				rdna4_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, g.rowBytes / 4u, vs);
			}
			// 0.0.278: WHAT the frame holds (read from the scanout, which the readback just matched to the source):
			// non-black pixels and value changes in rows 0, h/2 and h-1.
			if (logit && !bad) {
				const uint32_t rowsC[3] = { 0, plan.h / 2, plan.h - 1 };
				uint32_t nz[3] = { 0 }, ch3[3] = { 0 };
				for (unsigned r = 0; r < 3; r++) {
					const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rowsC[r]);
					uint32_t prev = amdgpu::RBAR0_32(dev, dor);
					for (uint32_t x = 0; x < plan.w; x++) {
						const uint32_t pxv = amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u);
						if (pxv & 0x00ffffffu) nz[r]++;
						if (pxv != prev) { ch3[r]++; prev = pxv; }
					}
				}
				RDNA4LOG("scanout-copy: FRAME CONTENT (scanout after the copy): rows 0/%u/%u non-black pixels %u/%u/%u of %u, value "
				       "changes along the row %u/%u/%u", plan.h / 2, plan.h - 1, nz[0], nz[1], nz[2], plan.w, ch3[0], ch3[1], ch3[2]);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// 0.0.345 — THE TILED SURFACE COPY, the shim's present path. Same contract as rdna4_scanout_copy_vram
// above (same interlock, same lock, same geometry, same fence), but ONE COPY_TILED_SUB_WINDOW packet for the whole
// frame instead of one COPY_LINEAR per row, because CONFIRMED the source is tiled and no row address into it is
// meaningful. `swizzle` is the GFX12 ADDR3 enum, `surfW`/`surfH` are Apple's CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT + 1 as
// the shim read them out of the resource - both come from the caller, neither is guessed here.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] readback mismatches, [6] pixels compared,
// [7] swizzle | surfW<<16 | surfH<<32, [8..10] samples dst<<32|src at (0,0), (w/2,h/2), (w-1,h-1).
// 0.0.416 (an internal design note, G3): `gcr` prepends the five-dword GCR_REQ to the SAME submission as the
// tiled copy (and so grows the ring traffic from 18 to 23 dwords). false is the 0.0.415 behaviour, byte for byte.
uint32_t rdna4_scanout_copy_tiled(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                   uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                   bool verify, bool gcr) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	v[7] = (uint64_t)swizzle | ((uint64_t)surfW << 16) | ((uint64_t)surfH << 32);
	bool logit = false;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutTiledPlan plan {};
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_tiled(&g, srcOff, srcLen, surfW, surfH, swizzle, 0, 0,
			                                           dstX, dstY, w, h, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		// The emission-time check, repeated here exactly as the row path repeats rdna4_scanout_row_dst_ok per packet:
		// every byte this packet may write must lie inside [fbOff, fbOff + fbLen).
		if (st == kScanStOk && !rdna4_scanout_tiled_dst_ok(&plan, g.fbLen)) st = kScanStPlan;
		uint64_t us = 0; uint32_t last = 0;
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk)
			st = scanout_sdma_tiled_copy(dev, inst, &plan, gBringup.gmc.vram_start + plan.srcOff,
			                             gBringup.gmc.vram_start + plan.fbOff, true, 0x5CA11E80u, 1000, &us, &last, gcr);
		v[4] = us;
		if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
			RDNA4LOG("scanout-copy: TILED surface VRAM %#llx (+%#llx) %ux%u swizzle %u -> rect (%u,%u) %ux%u: ONE 14-dword "
			       "COPY_TILED_SUB_WINDOW (detile 1, dcc 0, CPV 0) + FENCE%s, %u dwords in all, tiled MC %#llx -> linear MC "
			       "%#llx pitch %u; status %u, last fence %#010x, %llu us waited (copy call #%u)", (unsigned long long)srcOff,
			       (unsigned long long)srcLen, surfW, surfH, swizzle, plan.dstX, plan.dstY, plan.w, plan.h,
			       gcr ? " + GCR_REQ (GL2 WB+INV) before it in the same submission" : "",
			       (unsigned)rdna4_sdma_tiled_copy_dwords(gcr),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.srcOff),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.fbOff), plan.linPitch, st, last,
			       (unsigned long long)us, gScanoutCopyCalls);
		if (st == kScanStOk && verify) {
			// Rows 0 and h-1 of the DESTINATION against the TILED source read through the same address equation.
			// Note what this can and cannot settle: it reads a LIVE source AFTER the copy, so a difference is
			// "the copy is wrong" OR "the source was re-rendered in between" ( left exactly that unresolved).
			uint32_t bad = 0, compared = 0;
			const uint32_t rows[2] = { 0, plan.h - 1u };
			for (unsigned r = 0; r < 2; r++)
				for (uint32_t x = 0; x < plan.w; x++) {
					uint32_t sv = 0;
					const uint64_t so = plan.srcOff + rdna4_addr3_64kb_2d_off_4bpe(x + plan.srcX, rows[r] + plan.srcY, plan.surfW);
					const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + rows[r]) * plan.linPitch + plan.dstX + x) * 4u;
					if (!rdna4_vram_read_mm(so, &sv, 1)) { bad++; compared++; continue; }
					compared++;
					if (amdgpu::RBAR0_32(dev, d) != sv) bad++;
				}
			const uint32_t px[3] = { 0, plan.w / 2u, plan.w - 1u }, py[3] = { 0, plan.h / 2u, plan.h - 1u };
			for (unsigned i = 0; i < 3; i++) {
				uint32_t sv = 0;
				const uint64_t so = plan.srcOff + rdna4_addr3_64kb_2d_off_4bpe(px[i] + plan.srcX, py[i] + plan.srcY, plan.surfW);
				const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + py[i]) * plan.linPitch + plan.dstX + px[i]) * 4u;
				(void)rdna4_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)amdgpu::RBAR0_32(dev, d) << 32) | sv;
			}
			v[5] = bad; v[6] = compared;
			RDNA4LOG("scanout-copy: TILED READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against the tiled "
			       "source detiled by our own addrlib equation (MM window): %u differ%s; samples dst/src (0,0) "
			       "%#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x", plan.h - 1u, compared, bad,
			       bad ? " - FAILED" : " - EVERY COMPARED PIXEL MATCHES", (uint32_t)(v[8] >> 32), (uint32_t)v[8],
			       px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10]);
			// Same counter semantics as the row path (0.0.288): an UNVERIFIED copy does not count towards
			// gScanoutCopies, so `verified copies` keeps meaning what its name says.
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source detiled by the SAME addrlib
			// equation the copy was built from. This is the map that must reproduce the 1/16 grid on the glass when
			// S1 is off, and read uniform when S1 forces the linear row copy.
			{
				RDNA4VerifySource vs;
				vs.off = plan.srcOff; vs.stride = 0; vs.surfW = plan.surfW; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 1;
				rdna4_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, plan.linPitch, vs);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// =====================================================================================================================
// build 0.0.518 — FLIP MODE, switch 74, DEFAULT OFF. The decisions and their ORDER are
// dcn/gfx_flipmode.h's, host-tested against a display model (tests/gfx_flipmode_test.cpp). Below is only what the pure code
// is given: the I/O (rdna4_fm_ops over the bound rdna4dcn device and the SDMA copies), the engage, the verb, the unarmed A/B test.
// Lock order: gFmLock, then gScanoutLock (taken inside the copies). Nothing here holds gScanoutLock while it waits on the display.

static uint32_t gFmLogs { 0 };                    // present-side event lines (flips, holds, OFF), budgeted
static char     gFmTestLine[4][256];              // the A/B test's four per-flip lines (under gFmLock; kept off the stack)
// build 0.0.519: the A/B test's CRC witness and fill tallies (under gFmLock; kept off the stack).
static struct { uint32_t on, a, b0, b1, aOk, bOk, diff, prev, n, aVar, fillMin, fillMax, fills, ll[4]; } gFmTestW;
static constexpr uint32_t kFmLogBudget = 24;
static uint64_t fm_now_us() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }

// One present's detile INTO A OR B: rdna4_scanout_copy_tiled's contract (interlock, gScanoutLock, geometry, the
// COPY_TILED_SUB_WINDOW packet, fence) with the destination a parameter. rdna4_scanout_plan_tiled_to admits exactly the console
// (A) or flip mode's registered B, and the emission check repeats it over every byte the packet may write. `verify` runs S2's
// 16x16 cell map on the copy's OWN destination (the buffer about to become the front).
static uint32_t fm_copy_tiled_to(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                 uint32_t w, uint32_t h, uint64_t dstOff, uint64_t *out, unsigned count, bool verify, bool gcr) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	v[7] = (uint64_t)swizzle | ((uint64_t)surfW << 16) | ((uint64_t)surfH << 32);
	const bool logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (!rdna4_fm_dst_ok(&gFm, dstOff)) { v[0] = kScanStPlan; goto fin; }
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t dstLen = 0;
		RDNA4ScanoutTiledPlan plan {};
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_tiled_to(&g, gFm.bOff, gFm.len, dstOff, srcOff, srcLen, surfW, surfH, swizzle,
			                                              0, 0, 0, 0, w, h, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		// The emission-time check: the destination is still exactly A or B, and every byte the packet may write lies in it.
		if (st == kScanStOk && (plan.fbOff != dstOff || rdna4_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, plan.fbOff, &dstLen) ||
		                        !rdna4_scanout_tiled_dst_ok(&plan, dstLen)))
			st = kScanStPlan;
		uint64_t us = 0; uint32_t last = 0;
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk)
			st = scanout_sdma_tiled_copy(dev, inst, &plan, gBringup.gmc.vram_start + plan.srcOff,
			                             gBringup.gmc.vram_start + plan.fbOff, true, 0x5CA11E90u, 1000, &us, &last, gcr);
		v[4] = us;
		if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
			RDNA4LOG("flipmode74: TILED surface VRAM %#llx (+%#llx) %ux%u swizzle %u -> %s (vram+%#llx) rect %ux%u: ONE "
			       "COPY_TILED_SUB_WINDOW + FENCE%s, tiled MC %#llx -> linear MC %#llx pitch %u; status %u, fence %#010x, %llu us",
			       (unsigned long long)srcOff, (unsigned long long)srcLen, surfW, surfH, swizzle,
			       dstOff == gFm.bOff ? "B" : "A", (unsigned long long)dstOff, plan.w, plan.h, gcr ? " + GCR_REQ" : "",
			       (unsigned long long)(gBringup.gmc.vram_start + plan.srcOff),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.fbOff), plan.linPitch, st, last, (unsigned long long)us);
		if (st == kScanStOk && verify) {
			RDNA4VerifySource vs;
			vs.off = plan.srcOff; vs.stride = 0; vs.surfW = plan.surfW; vs.srcX = plan.srcX; vs.srcY = plan.srcY; vs.tiled = 1;
			rdna4_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, plan.linPitch, vs);
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// A whole-buffer COPY_LINEAR between A and B (the restore's B -> A, the engage's A -> B seed): up to 8 packets of 4 MiB and one
// fence on SDMA0 QUEUE0. Source and destination must each be exactly A or B (rdna4_fm_dst_ok and rdna4_scanout_flip_dst_check),
// distinct, and every packet's destination range lies inside the destination buffer.
static uint32_t fm_copy_linear(uint64_t srcOff, uint64_t dstOff, uint64_t *usOut) {
	*usOut = 0;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) return kScanStNoContext;
	if (!gScanoutPcPassed) return kScanStInterlock;
	if (!rdna4_fm_dst_ok(&gFm, dstOff) || !rdna4_fm_dst_ok(&gFm, srcOff) || srcOff == dstOff) return kScanStPlan;
	uint32_t st = kScanStOk, last = 0;
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t dstLen = 0, srcLen = 0;
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && (rdna4_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, dstOff, &dstLen) ||
		                        rdna4_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, srcOff, &srcLen)))
			st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		const uint64_t bytes = gFm.len;
		const uint32_t chunk = amdgpu::kSDMACopyLinearMaxBytes;
		const uint32_t n = (uint32_t)((bytes + chunk - 1u) / chunk);
		if (st == kScanStOk && (bytes == 0 || bytes > dstLen || bytes > srcLen || n == 0 || n > 8u)) st = kScanStPlan;
		uint64_t srcMc[8] = { 0 }, dstMc[8] = { 0 };
		uint32_t nb[8] = { 0 };
		for (uint32_t i = 0; st == kScanStOk && i < n; i++) {
			const uint64_t o = (uint64_t)i * chunk;
			const uint32_t b = (uint32_t)(bytes - o < chunk ? bytes - o : chunk);
			if (!rdna4_scanout_row_dst_ok(dstOff, dstLen, dstOff + o, b)) { st = kScanStPlan; break; }
			srcMc[i] = gBringup.gmc.vram_start + srcOff + o;
			dstMc[i] = gBringup.gmc.vram_start + dstOff + o;
			nb[i] = b;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk) st = scanout_sdma_copies(dev, inst, srcMc, dstMc, nb, n, 0x5CA11EA0u, 1000, usOut, &last);
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
	}
	IOLockUnlock(gScanoutLock);
	RDNA4LOG("flipmode74: linear copy %s -> %s (vram+%#llx -> vram+%#llx, %llu bytes) status %u, fence %#010x, %llu us",
	       srcOff == gFm.bOff ? "B" : "A", dstOff == gFm.bOff ? "B" : "A", (unsigned long long)srcOff,
	       (unsigned long long)dstOff, (unsigned long long)gFm.len, st, last, (unsigned long long)*usOut);
	return st;
}

// ---- the I/O flip mode's pure code is given --------------------------------------------------------------------------
struct FmPresentArgs {
	uint64_t phys, len; uint32_t surfW, surfH, swz, pw, ph; bool verify, gcr; uint64_t *cv; unsigned cvCount;
};
static int fm_io_read_front(void *, uint32_t *pending, uint64_t *earliest) { return rdna4dcn::fmReadFront(pending, earliest); }
static int fm_io_read_primary(void *, uint64_t *mc) { return rdna4dcn::fmReadPrimary(mc); }
static void fm_io_delay(void *, uint32_t us) { IODelay(us); }
static uint32_t fm_io_copy_to(void *c, uint64_t dstOff) {
	const FmPresentArgs *a = static_cast<const FmPresentArgs *>(c);
	if (!a) return kScanStNoContext;
	return fm_copy_tiled_to(a->phys, a->len, a->surfW, a->surfH, a->swz, a->pw, a->ph, dstOff, a->cv, a->cvCount, a->verify,
	                        a->gcr);
}
static uint32_t fm_io_copy_b_to_a(void *) { uint64_t us = 0; return fm_copy_linear(gFm.bOff, gFm.aOff, &us); }
static int fm_io_flip(void *, uint64_t mc) { return rdna4dcn::fmFlip(mc, "fmflip"); }
static void fm_io_release(void *) { (void)rdna4dcn::fmSetExact(0u, 0u, 0u); }
// build 0.0.519: may the restore flip to A after an earlier B -> A copy failed or timed out? Only when no
// write into A can still be running: SDMA0 QUEUE0 is idle (its read pointer is at our write pointer, scanout_queue_check) AND
// its fence dword is non-zero (every QUEUE0 submission of ours zeroes it first and its own FENCE packet writes it last, after
// every copy packet before it on the in-order queue). Polled under gScanoutLock for at most 100 ms. 1 = settled. Read-only.
static uint32_t fm_io_a_settled(void *) {
	if (!gBringup.dev || !gScanoutLock) return 0u;
	uint32_t ok = 0u, fence = 0u, rptr = 0u;
	IOLockLock(gScanoutLock);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	for (uint32_t us = 0; us <= 100000u; us += 1000u) {
		fence = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (scanout_queue_check(dev, inst, &rptr) == kScanStOk && fence != 0u) { ok = 1u; break; }
		IOSleep(1);
	}
	IOLockUnlock(gScanoutLock);
	RDNA4LOG("flipmode74: A-SETTLED check after a failed B->A copy: %s (QUEUE0 rptr %#x, fence dword %#010x)",
	       ok ? "SETTLED - no write into A can still be running" : "*** NOT SETTLED *** - the restore does not flip to A", rptr, fence);
	return ok;
}
static rdna4_fm_ops fm_ops(void *ctx) {
	rdna4_fm_ops o { ctx, fm_io_read_front, fm_io_read_primary, fm_io_delay, fm_io_copy_to, fm_io_copy_b_to_a, fm_io_flip,
	               fm_io_release, fm_io_a_settled };
	return o;
}

// ENGAGE: A = the console (RDNA4FB's Console,* geometry, rdna4_scanout_geom_check), B = one console-shaped allocation
// (gmc.vram_alloc, 64 KiB aligned, allocated ONCE per boot and kept), HUBP0 must be scanning A and be console-shaped, then the
// device's exact flip set becomes {A, B}. `seedB` (the switch; not the A/B test) first copies A into B by SDMA so a partial
// present can never show stale VRAM. 0 = engaged; else the refusal (logged, counted).
static uint32_t fm_engage_locked(bool seedB) {
	if (gFm.engaged) return 0u;
	uint32_t why = 0u, hw = 0u, hh = 0u, hp = 0u, hf = 0u, hs = 0u, pend = 1u;
	uint64_t aOff = 0, prim = 0, early = 0;
	const char *what = "";
	RDNA4ScanoutGeom g {};
	
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock || !garnet_driver_service()) { why = 1u; what = "no device context"; }
	else if (seedB && !gScanoutPcPassed) { why = 2u; what = "the scanout positive control has not passed this boot"; }
	else if ((void)rdna4dcn::bind(static_cast<GarnetDriver *>(garnet_driver_service())), !rdna4dcn::fmArmed()) {   // gBringupSelf = this (start)
		why = 3u; what = "the DCN display layer is not bound";
	}
	else if (rdna4dcn::fmTestFlipHeld()) { why = 4u; what = "dcnflip's test pattern is on screen"; }
	if (!why) {
		IOLockLock(gScanoutLock);
		if (scanout_geometry(*gBringup.dev, &g) != kScanStOk || rdna4_scanout_geom_check(&g, &aOff) != kScanGeomOk ||
		    gBringup.gmc.vram_start != gBringup.dev->vramMcBase) {
			why = 5u; what = "no usable console geometry";
		} else if (rdna4dcn::fmHubpGeom(&hw, &hh, &hp, &hf, &hs) != 0 || !rdna4_fm_hubp_ok(hw, hh, hp, hf, hs, g.width, g.height, g.rowBytes)) {
			why = 6u; what = "HUBP0 is not scanning a console-shaped linear 32-bit plane (or OTG0 is not lit)";
		} else {
			const uint64_t len = (uint64_t)g.rowBytes * g.height;
			if (!gFm.haveB) {
				amdgpu::VRAMAllocation b {};
				if (!gBringup.gmc.vram_alloc.alloc(len, 65536, &b) || b.gpu_va < gBringup.gmc.vram_start) {
					if (b.size) gBringup.gmc.vram_alloc.free(b);
					why = 7u; what = "the back buffer could not be allocated";
				} else {
					gFm.bOff = b.gpu_va - gBringup.gmc.vram_start;
					gFm.len = len;
					__atomic_store_n(&gFm.haveB, 1u, __ATOMIC_RELEASE);
					RDNA4LOG("flipmode74: BACK BUFFER B allocated ONCE: MC %#llx = vram+%#llx, %llu bytes (%ux%u, RowBytes %u, the "
					       "console's shape); A (the console) MC %#llx = vram+%#llx", (unsigned long long)b.gpu_va,
					       (unsigned long long)gFm.bOff, (unsigned long long)len, g.width, g.height, g.rowBytes,
					       (unsigned long long)(gBringup.gmc.vram_start + aOff), (unsigned long long)aOff);
				}
			} else if (gFm.len != len) { why = 8u; what = "the console's shape changed since B was allocated"; }
			if (!why) {
				const uint32_t br = rdna4_fm_buffers_ok(aOff, gFm.bOff, len, gBringup.dev->bar0Size, gBringup.dev->vramBase,
				                                      gBringup.gmc.vram_start);
				if (br) { why = 9u; what = br == 5u ? "a buffer ends above 256 MiB (ADDRESS_HIGH could change)"
				                              : "A/B placement (overlap, alignment, BAR0 or the MC high dword)"; }
			}
			if (!why) {
				gFm.aOff = aOff;
				gFm.aMc = gBringup.gmc.vram_start + aOff;
				gFm.bMc = gBringup.gmc.vram_start + gFm.bOff;
			}
		}
		IOLockUnlock(gScanoutLock);
	}
	if (!why && (rdna4dcn::fmReadPrimary(&prim) != 0 || prim != gFm.aMc)) { why = 10u; what = "HUBP0's programmed address is not A"; }
	if (!why && (rdna4dcn::fmSetExact(gFm.aMc, gFm.bMc, 2u) != 0)) { why = 13u; what = "the device refused the exact flip set {A, B}"; }
	if (!why && (rdna4dcn::fmReadFront(&pend, &early) != 0 || pend || early != gFm.aMc)) {
		why = 11u; what = "the display is not scanning A (a flip is pending or EARLIEST_INUSE is not A)";
	}
	if (!why && seedB) {
		uint64_t us = 0;
		if (fm_copy_linear(gFm.aOff, gFm.bOff, &us) != kScanStOk) { why = 12u; what = "the A -> B seed copy failed"; }
	}
	if (why) {
		if (why >= 11u) (void)rdna4dcn::fmSetExact(0u, 0u, 0u);
		gFm.engageRefused++;
		RDNA4LOG("flipmode74: ENGAGE REFUSED (%u: %s); HUBP0 %ux%u pitch %u fmt %u SW_MODE %u, programmed %#llx, EARLIEST_INUSE %#llx "
		       "pending %u; A MC %#llx B MC %#llx - flip mode stays OFF", why, what, hw, hh, hp, hf, hs,
		       (unsigned long long)prim, (unsigned long long)early, pend, (unsigned long long)gFm.aMc, (unsigned long long)gFm.bMc);
		return why;
	}
	gFm.bHoldsPresent = seedB ? 1u : 0u;   // 0.0.519 F2: B holds a real picture only when seeded from A
	gFm.consecTimeouts = 0u;               // 0.0.519 F1: a fresh engage starts a fresh timeout run
	__atomic_store_n(&gFm.engaged, 1u, __ATOMIC_RELEASE);
	gFm.engages++;
	gFm.front = early;
	RDNA4LOG("flipmode74: ENGAGED - A MC %#llx, B MC %#llx (vram+%#llx, %llu bytes), HUBP0 %ux%u pitch %u fmt %u linear; the "
	       "device flips ONLY to A or B now (dcn41 exact set)%s", (unsigned long long)gFm.aMc, (unsigned long long)gFm.bMc,
	       (unsigned long long)gFm.bOff, (unsigned long long)gFm.len, hw, hh, hp, hf, seedB ? "; B seeded from A" : "");
	return 0u;
}

static void fm_report(const char *how) {
	RDNA4LOG(RDNA4_FM_REPORT1_FMT, gFm.on ? "ON" : "OFF (default)", how, gFm.engaged, (unsigned long long)gFm.aMc,
	       (unsigned long long)gFm.bMc, (unsigned long long)gFm.bOff, (unsigned long long)gFm.len, (unsigned long long)gFm.front,
	       rdna4_fm_off_name(gFm.offWhy), __atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE));
	RDNA4LOG(RDNA4_FM_REPORT2_FMT, (unsigned long long)gFm.flips, (unsigned long long)gFm.flipsToB, (unsigned long long)gFm.flipsToA,
	       (unsigned long long)gFm.presents, (unsigned long long)gFm.waits, (unsigned long long)gFm.waitHist[0],
	       (unsigned long long)gFm.waitHist[1], (unsigned long long)gFm.waitHist[2], (unsigned long long)gFm.waitHist[3],
	       (unsigned long long)gFm.waitHist[4], (unsigned long long)gFm.waitHist[5], (unsigned long long)gFm.waitHist[6],
	       gFm.waitMaxUs, (unsigned long long)gFm.timeouts, (unsigned long long)gFm.copyFails);
	RDNA4LOG(RDNA4_FM_REPORT3_FMT, (unsigned long long)gFm.foreign, (unsigned long long)gFm.highChanged,
	       (unsigned long long)gFm.flipFails, (unsigned long long)gFm.readFails, (unsigned long long)gFm.restores,
	       (unsigned long long)gFm.restoreFails, (unsigned long long)gFm.restoreCopies,
	       (unsigned long long)__atomic_load_n(&gFm.restoreRequests, __ATOMIC_RELAXED), (unsigned long long)gFm.engages,
	       (unsigned long long)gFm.engageRefused, (unsigned long long)__atomic_load_n(&gFm.writersHeld, __ATOMIC_RELAXED),
	       (unsigned long long)gFm.testRuns);
	RDNA4LOG(RDNA4_FM_REPORT4_FMT, gFm.consecTimeouts, (unsigned long long)gFm.stuckRestores,   // build 0.0.519
	       (unsigned long long)gFm.timeoutForeign, (unsigned long long)gFm.restoreCopyFails,
	       (unsigned long long)gFm.restoreCopySkipped, (unsigned long long)gFm.restoreUnsettled, gFm.bHoldsPresent,
	       gFm.aCopyPending);
}

static void fm_restore_locked(uint32_t why, uint32_t force) {
	if (force && !gFm.engaged && gFm.haveB && gFm.aMc && gFm.bMc && rdna4dcn::fmArmed() &&
	    rdna4dcn::fmSetExact(gFm.aMc, gFm.bMc, 2u) == 0)
		__atomic_store_n(&gFm.engaged, 1u, __ATOMIC_RELEASE);   // the force verb restores even what it did not see engaged
	const uint32_t wasEngaged = gFm.engaged;
	rdna4_fm_ops o = fm_ops(nullptr);
	const uint32_t ok = rdna4_fm_restore(&gFm, &o, why, force);
	RDNA4LOG("flipmode74: RESTORE (%s)%s -> %s; front %#llx (A %#llx); restores verified %llu failed %llu; flip mode OFF: %s "
	       "(B->A copy failures %llu, A copy pending %u)",
	       rdna4_fm_off_name(why), wasEngaged ? "" : " - nothing of ours was engaged", ok ? "A VERIFIED (EARLIEST_INUSE and the "
	       "programmed address)" : "*** NOT VERIFIED ***", (unsigned long long)gFm.front, (unsigned long long)gFm.aMc,
	       (unsigned long long)gFm.restores, (unsigned long long)gFm.restoreFails, rdna4_fm_off_name(gFm.offWhy),
	       (unsigned long long)gFm.restoreCopyFails, gFm.aCopyPending);
}

uint32_t rdna4_fm_on(void) { return __atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE); }

// dpg_perform's question, asked ONLY while rdna4_fm_on() and only AFTER switch 73's hold check. 0 = a copy was made (its scanout
// status in *cstOut: flipped, or a failed copy that held the flip); 1 = held, no copy and no flip.
uint32_t rdna4_fm_present(uint64_t phys, uint64_t len, uint32_t surfW, uint32_t surfH, uint32_t swz, uint32_t pw, uint32_t ph,
                           bool linear, bool verify, bool gcr, uint64_t presentNo, uint64_t *cv, unsigned cvCount,
                           uint32_t *cstOut) {
	*cstOut = 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok || !gFmLock) return 1u;
	uint32_t held = 1u;
	IOLockLock(gFmLock);
	if (gFm.on) {
		if (linear || swz != 3u) {
			fm_restore_locked(RDNA4_FM_OFF_NOTTILED, 0u);
		} else {
			FmPresentArgs a { phys, len, surfW, surfH, swz, pw, ph, verify, gcr, cv, cvCount };
			rdna4_fm_ops o = fm_ops(&a);
			const uint64_t to0 = gFm.timeouts;
			const uint32_t r = rdna4_fm_present(&gFm, &o, cstOut);
			held = (r == RDNA4_FM_P_FLIPPED || r == RDNA4_FM_P_COPYFAIL) ? 0u : 1u;
			if (gFmLogs < kFmLogBudget || r == RDNA4_FM_P_OFF) {
				gFmLogs++;
				RDNA4LOG("flipmode74: present #%llu (plane VRAM %#llx) -> %s; front was %#llx (%s); copy status %u; flips %llu, "
				       "timeouts %llu%s", (unsigned long long)presentNo, (unsigned long long)phys,
				       r == RDNA4_FM_P_FLIPPED ? "FLIPPED" : r == RDNA4_FM_P_COPYFAIL ? "HELD (copy failed, no flip)"
				       : r == RDNA4_FM_P_HELD ? (gFm.timeouts != to0 ? "HELD (the previous flip did not latch in 2 frames)" : "HELD")
				       : "FLIP MODE OFF", (unsigned long long)gFm.front,
				       gFm.front == gFm.aMc ? "A" : gFm.front == gFm.bMc ? "B" : "FOREIGN", *cstOut,
				       (unsigned long long)gFm.flips, (unsigned long long)gFm.timeouts,
				       r == RDNA4_FM_P_OFF ? " - flip mode is OFF, last reason below" : "");
				if (r == RDNA4_FM_P_OFF) fm_report(" - turned OFF by the present path");
			}
		}
	}
	IOLockUnlock(gFmLock);
	return held;
}

// A token / guard / keystone withdrawal on the submit path: only a flag (no lock, no I/O); the next present or verb restores.
void rdna4_fm_note_withdrawal(void) { (void)rdna4_fm_request_restore(&gFm, RDNA4_FM_OFF_WITHDRAWAL); }

// build 0.0.525 (switch 80): the buffer the last present's copy went into (rdna4_fm_present copies into the one that is NOT
// `front`, the last EARLIEST_INUSE it read). Read-only and lock-free: presents are one at a time and this is asked right after one.
uint32_t rdna4_fm_copy_buf(void) {
	if (!__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) return 0u;
	const uint64_t f = __atomic_load_n(&gFm.front, __ATOMIC_RELAXED);
	return f == gFm.aMc ? 2u : f == gFm.bMc ? 1u : 3u;
}

// build 0.0.538 (switch 95, gfx_p95.h): may a replayed present go through flip mode now? 1 = no: a restore is requested
// (served by the next present, which then holds), a failed restore's B -> A copy is pending (A may still be written), or A/B are
// still engaged with flip mode OFF (the failed restore 842 retries). A flip whose latch is pending is rdna4_fm_present's own
// bounded wait (it holds without a copy). Read-only and lock-free, like rdna4_fm_copy_buf.
uint32_t rdna4_fm_replay_blocked(void) {
	if (__atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE)) return 1u;
	if (__atomic_load_n(&gFm.aCopyPending, __ATOMIC_ACQUIRE)) return 1u;
	if (__atomic_load_n(&gFm.engaged, __ATOMIC_ACQUIRE) && !__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) return 1u;
	return 0u;
}

// The commit arm's disarm verb (`gfxneuter 4 | 2 << 8`): back to A, flip mode OFF. Verb context.
void rdna4_fm_disarm(void) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok || !gFmLock) return;
	IOLockLock(gFmLock);
	if (gFm.on || gFm.engaged) fm_restore_locked(RDNA4_FM_OFF_DISARM, 0u);
	IOLockUnlock(gFmLock);
}

// `gfxneuter 74 | M << 8`: M 1 ON (= 330: engage, then every tiled present flips), M 2 OFF (= 586, the default and the boot
// value: restore to A), M 3 FORCE RESTORE (= 842: copy B into A if B is the front, flip to A and verify, flip mode OFF; allowed
// at ANY time, even mid-arm, like every other way back), bare 74 reads. `contRefused` = the continuous mid-arm guard's answer
// (the caller's; M 1 and M 2 only). Returns the verb status: 0, 5 mid-arm refused, 11 unknown M, 12 the engage refused.
uint32_t rdna4_fm_control(uint32_t m, uint32_t contRefused) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok || !gFmLock) return 1u;
	uint32_t st = 0u;
	const char *how = " (read only, unchanged)";
	IOLockLock(gFmLock);
	if (m == 0u) {
		if (__atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE)) { fm_restore_locked(gFm.restoreReq, 0u); how = " - a pending restore was served"; }
	} else if (m == 3u) {
		fm_restore_locked(RDNA4_FM_OFF_FORCE, 1u);
		how = " - FORCE RESTORE BY THIS VERB";
	} else if (m != 1u && m != 2u) {
		st = 11u; how = " - REFUSED (unknown M), unchanged";
	} else if (contRefused) {
		st = 5u; how = " - REFUSED: a continuous arm stands, unchanged";
	} else if (m == 1u) {
		if (gFm.on) how = " - already ON";
		else if (gFm.engaged) { st = 12u; how = " - ON REFUSED: a failed restore left A/B engaged (842 retries it), unchanged"; }   // 0.0.520
		else if (fm_engage_locked(true) != 0u) { st = 12u; how = " - ON REFUSED (the engage failed, line above), unchanged"; }
		else { gFm.offWhy = RDNA4_FM_OFF_NONE; __atomic_store_n(&gFm.on, 1u, __ATOMIC_RELEASE); how = " - CHANGED BY THIS VERB"; }
	} else {
		if (gFm.on || gFm.engaged) { fm_restore_locked(RDNA4_FM_OFF_VERB, 0u); how = " - CHANGED BY THIS VERB (restored to A)"; }
		else how = " - already OFF";
	}
	fm_report(how);
	IOLockUnlock(gFmLock);
	return st;
}

// `dcnflip 1000 + N` (N 2..240): flip mode's UNARMED A/B test (PC T-F1). Refused while an arm stands or flip mode is ON.
// Engages without the SDMA seed (B is filled by the CPU through BAR0, as dcnflip's pattern is), then N flips alternating
// B, A, B, ...: before each flip to B it is filled with one of two distinct dark greys (alternating, so every visit to B shows
// NEW content); each flip is VUPDATE-latched and waited for (<= 3 frames). Reported: SURFACE_FLIP_PENDING set on the first read
// after the write, the latch time, EARLIEST_INUSE == the target per flip, OTG0's frame count before/after. Ends on A, verified
// (EARLIEST_INUSE and the programmed address; one more flip to A if not). A is never written.
// out[0] status, [1] N, [2] latched, [3] EARLIEST_INUSE matches, [4] pending-set on the first read, [5] latch us min | max << 32,
// [6] latch us sum, [7] frame count before | after << 32, [8] elapsed us, [9] ended on A verified, [10] A MC, [11] B MC,
// [12] fill us total.
uint32_t rdna4_fm_test(uint32_t n, uint64_t *out, unsigned count) {
	uint64_t v[13] = { 0 };
	uint32_t st = 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	v[1] = n;
	if (!txn.ok) st = 13u;
	else if (n < RDNA4_FM_TEST_MIN || n > RDNA4_FM_TEST_MAX) st = 1u;
	else if (!gFmLock) st = 3u;
	if (st) {
		RDNA4LOG("fmtest: A/B test of %u flip(s) REFUSED (%u: %s)", n, st,
		       st == 1u ? "N outside 2..240" : "no lock");
	} else {
		IOLockLock(gFmLock);
		uint32_t r = 0u;
		if (gFm.on) { st = 4u; RDNA4LOG("fmtest: REFUSED - flip mode (switch 74) is ON; turn it OFF first"); }
		else if (gFm.engaged) { st = 7u; RDNA4LOG("fmtest: REFUSED - a failed restore left A/B engaged; `gfxneuter 842` first"); }   // 0.0.520
		else if ((r = fm_engage_locked(false)) != 0u) { st = 0x10u | r; }
		else {
			gFm.testRuns++;
			rdna4_fm_ops o = fm_ops(nullptr);
			amdgpu::DeviceContext &dev = *gBringup.dev;
			uint32_t fc0 = 0, fc1 = 0, latched = 0, match = 0, pset = 0, lmin = 0xffffffffu, lmax = 0;
			uint64_t lsum = 0, fillUs = 0;
			// build 0.0.519: the witness nobody has to watch - OTG0's CRC read after each flip has
			// latched and a frame of the new buffer has been scanned (2 frames), and the fill time per visit to B.
			auto &W = gFmTestW;
			memset(&W, 0, sizeof(W));
			W.fillMin = 0xffffffffu;
			char (&line)[4][256] = gFmTestLine;
			line[0][0] = 0; line[1][0] = 0; line[2][0] = 0; line[3][0] = 0;
			gFm.bHoldsPresent = 0u;   // 0.0.519 F2: B is about to hold test fills, never a present
			(void)rdna4dcn::fmFrameCount(&fc0);
			W.on = rdna4dcn::fmCrcBegin() == 0 ? 1u : 0u;
			const uint64_t t0 = fm_now_us();
			for (uint32_t i = 0; i < n && st == 0u; i++) {
				const bool toB = (i & 1u) == 0u;
				const uint64_t target = toB ? gFm.bMc : gFm.aMc;
				uint32_t fillThis = 0u;
				if (toB) {
					const uint32_t fill = ((i >> 1) & 1u) ? 0xFF101018u : 0xFF101010u;
					const uint64_t f0 = fm_now_us();
					volatile uint32_t *px = reinterpret_cast<volatile uint32_t *>(dev.bar0 + gFm.bOff);
					for (uint64_t k = 0; k < gFm.len / 4u; k++) px[k] = fill;
					amdgpu::storeFence();
					amdgpu::amdgpu_hdp_flush(dev);   // 0.0.519 F4: as fc_positive_control, after the CPU fill
					fillThis = (uint32_t)(fm_now_us() - f0);
					fillUs += fillThis; W.fills++;
					if (fillThis < W.fillMin) W.fillMin = fillThis;
					if (fillThis > W.fillMax) W.fillMax = fillThis;
				}
				if (!rdna4_fm_flip_ok(&gFm, target) || rdna4dcn::fmFlip(target, "fmtest") != 0) { st = 5u; break; }
				uint32_t pend = 0, us = 0, polls = 0; uint64_t e = 0;
				if (o.read_front(o.ctx, &pend, &e) == 0 && pend) pset++;
				const uint32_t w = rdna4_fm_wait_latch(&o, RDNA4_FM_RESTORE_WAIT_US, &e, &us, &polls);
				if (w == RDNA4_FM_W_LATCHED) { latched++; lsum += us; if (us < lmin) lmin = us; if (us > lmax) lmax = us; }
				if (e == target) match++;
				if (toB) gFm.flipsToB++; else gFm.flipsToA++;
				uint32_t crg = 0u, cb = 0u, crc = 0u;
				if (W.on && rdna4dcn::fmCrcRead(2u, &crg, &cb) == 0) {
					crc = crg ^ (cb * 0x9E3779B1u);   // one word per frame (R/G and B both enter it: the greys differ in B only)
					W.n++;
					if (W.n > 1u && crc != W.prev) W.diff++;
					W.prev = crc;
					if (!toB) { if (!W.aOk) { W.a = crc; W.aOk = 1u; } else if (crc != W.a) W.aVar++; }
					else if (((i >> 1) & 1u) == 0u) { if (!(W.bOk & 1u)) W.b0 = crc; W.bOk |= 1u; }
					else { if (!(W.bOk & 2u)) W.b1 = crc; W.bOk |= 2u; }
				}
				if (i < 16u) {
					char *L = line[i / 4u];
					uint32_t &at = W.ll[i / 4u];
					const int k = snprintf(L + at, sizeof(line[0]) - at, " %c%s/%uus/fill %uus/crc %08x", toB ? 'B' : 'A',
					                       w == RDNA4_FM_W_LATCHED ? (e == target ? "" : "!inuse") : "!late", us, fillThis, crc);
					if (k > 0 && at + (uint32_t)k < sizeof(line[0])) at += (uint32_t)k;
				}
			}
			if (W.on) rdna4dcn::fmCrcEnd();
			// End on A, verified - unconditionally, like dcnflip's restore.
			uint32_t endOk = 0u;
			for (uint32_t attempt = 0; attempt < 2u && !endOk; attempt++) {
				uint64_t e = 0, prim = 0; uint32_t us = 0, polls = 0;
				if (rdna4dcn::fmFlip(gFm.aMc, "fmtestend") != 0) continue;
				(void)rdna4_fm_wait_latch(&o, RDNA4_FM_RESTORE_WAIT_US, &e, &us, &polls);
				endOk = (e == gFm.aMc && rdna4dcn::fmReadPrimary(&prim) == 0 && prim == gFm.aMc) ? 1u : 0u;
				gFm.front = e;
			}
			const uint64_t el = fm_now_us() - t0;
			(void)rdna4dcn::fmFrameCount(&fc1);
			(void)rdna4dcn::fmSetExact(0u, 0u, 0u);
			__atomic_store_n(&gFm.engaged, 0u, __ATOMIC_RELEASE);
			if (!endOk && st == 0u) st = 6u;
			uint64_t refused = 0;
			const uint64_t allowed = rdna4dcn::fmAllowCounts(&refused);
			RDNA4LOG("fmtest: A/B test %u flip(s): latched %u, EARLIEST_INUSE == target %u, SURFACE_FLIP_PENDING set on the first "
			       "read %u; latch us min %u avg %llu max %u; OTG0 frame count %u -> %u in %llu us; B fills %llu us; ended on A %s "
			       "(front %#llx); status %u; DCN allowlist allowed %llu refused %llu", n, latched, match, pset,
			       lmin == 0xffffffffu ? 0u : lmin, latched ? (unsigned long long)(lsum / latched) : 0ull, lmax, fc0, fc1,
			       (unsigned long long)el, (unsigned long long)fillUs, endOk ? "VERIFIED" : "*** NOT VERIFIED ***",
			       (unsigned long long)gFm.front, st, (unsigned long long)allowed, (unsigned long long)refused);
			// build 0.0.519: the CRC witness and the fill time per visit.
			RDNA4LOG("fmtest: CRC WITNESS %s: %u read(s); A %08x (A visits that differ from it %u); B grey-1 %08x, B grey-2 %08x; the CRC "
			       "changed on %u of %u consecutive reads (ALTERNATION %s); B fill per visit min %u us avg %llu us max %u us over %u "
			       "visit(s)", W.on ? "ON (OTG0 CRC0 over the active raster, 2 frames after each latch)" : "NOT ENABLED (OTG0 not lit)",
			       W.n, W.a, W.aVar, W.b0, W.b1, W.diff, W.n ? W.n - 1u : 0u,
			       (W.n > 1u && W.diff == W.n - 1u && W.aVar == 0u && W.aOk && (W.bOk & 1u) && W.b0 != W.a) ? "SEEN" : "NOT SEEN",
			       W.fillMin == 0xffffffffu ? 0u : W.fillMin, W.fills ? (unsigned long long)(fillUs / W.fills) : 0ull, W.fillMax, W.fills);
			for (uint32_t q = 0; q < 4u && q * 4u < n; q++) RDNA4LOG("fmtest: flips %u-%u:%s", q * 4u + 1u, q * 4u + 4u, line[q]);
			v[2] = latched; v[3] = match; v[4] = pset; v[5] = (uint64_t)(lmin == 0xffffffffu ? 0u : lmin) | ((uint64_t)lmax << 32);
			v[6] = lsum; v[7] = (uint64_t)fc0 | ((uint64_t)fc1 << 32); v[8] = el; v[9] = endOk; v[10] = gFm.aMc;
			v[11] = gFm.bMc; v[12] = fillUs;
		}
		IOLockUnlock(gFmLock);
	}
	v[0] = st;
	if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
	return st;
}

// 0.0.273: the STAGED surface copy. flush2 measured the test client's flushed buffer as a VidMemory with NO
// VRAM placement (getPhysicalSegment 0/0) while the client's pixels sat in the resource's system-memory backing
// (IOAccelResource2 +0x80, the IOAccelSysMemory lockForCPUAccess maps, 0x145a81ef/0x145a81fd). The GPU cannot reach that
// memory from VMID 0, so the rows are staged by the CPU into a BAR0-pool VRAM buffer (never the scanout) and copied into the
// scanout by SDMA0 QUEUE0 through the same checked plan, fence and readback as the VRAM path. Every scanout write is still
// an SDMA packet whose destination passed rdna4_scanout_row_dst_ok.
// out as rdna4_scanout_copy_vram, plus [2] the staging VRAM offset and status 17 (backing not prepared / too short).
uint32_t rdna4_scanout_copy_staged(IOMemoryDescriptor *md, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                    uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	amdgpu::VRAMAllocation stg {};
	uint32_t *row = nullptr;
	bool prepared = false;
	bool logit = false;   // 0.0.278
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutPlan plan {};
		const uint32_t cw = w < srcW ? w : srcW, ch = h < srcH ? h : srcH;
		const uint64_t need = (uint64_t)(srcH - 1) * srcStride + (uint64_t)srcW * 4u;
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk && (!md || md->getLength() < need || cw == 0 || ch == 0 || srcStride < srcW * 4u))
			st = 17;
		// 0.0.274: flush3 measured the backing as an IOBufferMemoryDescriptor with preparationID 0 (not wired).
		// The IOKit contract for reading such memory is prepare() first and complete() after, balanced, from thread
		// context; readBytes walks physical segments that exist only while it is prepared.
		if (st == kScanStOk) {
			const IOReturn pr = md->prepare();
			prepared = (pr == kIOReturnSuccess);
			if (logit || !prepared)
				RDNA4LOG("scanout-staged: backing %p prepare() -> %#x, preparationID now %#llx", md, pr,
				       (unsigned long long)md->getPreparationID());
			if (!prepared) st = 17;
		}
		const uint32_t bytes = cw * ch * 4u;
		if (st == kScanStOk && (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &stg) ||
		                        stg.gpu_va - gBringup.gmc.vram_start + bytes > dev.bar0Size))
			st = kScanStAlloc;
		const uint64_t stgOff = stg.size ? stg.gpu_va - gBringup.gmc.vram_start : 0;
		v[2] = stgOff;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_rows(&g, stgOff, bytes, cw, ch, cw * 4u, 0, 0, dstX, dstY, cw, ch,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != cw || plan.h != ch) st = kScanStPlan;
		}
		// Stage: read each source row from the backing and write it to the staging buffer through BAR0.
		if (st == kScanStOk) {
			row = static_cast<uint32_t *>(IOMalloc(cw * sizeof(uint32_t)));
			if (!row) st = kScanStAlloc;
			for (uint32_t y = 0; st == kScanStOk && y < ch; y++) {
				if (md->readBytes((IOByteCount)y * srcStride, row, cw * 4u) != cw * 4u) { st = 17; break; }
				amdgpu::bar0_memcpy_to_vram(dev, stgOff + (uint64_t)y * cw * 4u, row, cw * 4u);
			}
			amdgpu::amdgpu_hdp_flush(dev);
			// Source control: the staging buffer reads back as the backing's first and last rows.
			if (st == kScanStOk) {
				uint32_t bad = 0;
				const uint32_t rows2[2] = { 0, ch - 1 };
				for (unsigned r = 0; r < 2; r++) {
					md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
					for (uint32_t x = 0; x < cw; x++)
						if (amdgpu::RBAR0_32(dev, stgOff + ((uint64_t)rows2[r] * cw + x) * 4u) != row[x]) bad++;
				}
				if (logit || bad)
					RDNA4LOG("scanout-staged: backing %p (%s, length %#llx) %ux%u stride %u -> staging VRAM %#llx (MC %#llx) %ux%u: "
					       "staged rows 0 and %u read back with %u mismatch(es)", md, md->getMetaClass()->getClassName(),
					       (unsigned long long)md->getLength(), srcW, srcH, srcStride, (unsigned long long)stgOff,
					       (unsigned long long)stg.gpu_va, cw, ch, ch - 1, bad);
				if (bad) st = kScanStSourceCtl;
				// 0.0.278: WHAT the frame holds, so the report can say what the screen shows - non-zero pixels and
				// distinct values in five rows, and the value at the centre and at four inset corners.
				if (logit && st == kScanStOk) {
					const uint32_t rowsC[5] = { 0, ch / 4, ch / 2, (3 * ch) / 4, ch - 1 };
					uint32_t nz[5] = { 0 }, first[5] = { 0 }, distinctish[5] = { 0 };
					for (unsigned r = 0; r < 5; r++) {
						md->readBytes((IOByteCount)rowsC[r] * srcStride, row, cw * 4u);
						first[r] = row[0];
						uint32_t prev = row[0];
						for (uint32_t x = 0; x < cw; x++) {
							if (row[x] & 0x00ffffffu) nz[r]++;
							if (row[x] != prev) { distinctish[r]++; prev = row[x]; }
						}
					}
					const uint32_t sx[5] = { cw / 2, 16, cw - 17, 16, cw - 17 }, sy[5] = { ch / 2, 16, 16, ch - 17, ch - 17 };
					uint32_t sv[5] = { 0 };
					for (unsigned i = 0; i < 5; i++) md->readBytes((IOByteCount)sy[i] * srcStride + (IOByteCount)sx[i] * 4u, &sv[i], 4);
					RDNA4LOG("scanout-staged: FRAME CONTENT (backing, BGRA little-endian dwords): rows 0/%u/%u/%u/%u non-black pixels "
					       "%u/%u/%u/%u/%u of %u, value changes along the row %u/%u/%u/%u/%u, first pixel %#010x/%#010x/%#010x/%#010x/%#010x; "
					       "centre %#010x, inset corners TL %#010x TR %#010x BL %#010x BR %#010x", ch / 4, ch / 2, (3 * ch) / 4, ch - 1,
					       nz[0], nz[1], nz[2], nz[3], nz[4], cw, distinctish[0], distinctish[1], distinctish[2], distinctish[3],
					       distinctish[4], first[0], first[1], first[2], first[3], first[4], sv[0], sv[1], sv[2], sv[3], sv[4]);
				}
			}
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11F00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				RDNA4LOG("scanout-staged: SDMA copy %u rows x %u bytes in %u submission(s), staging MC %#llx.. -> scanout MC %#llx.. "
				       "(rect %u,%u %ux%u): status %u, last fence %#010x, %llu us waited (copy call #%u)", plan.h, plan.rowBytes,
				       chunks, (unsigned long long)(srcMc ? srcMc[0] : 0), (unsigned long long)(dstMc ? dstMc[0] : 0), plan.dstX,
				       plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// Readback: the scanout rows 0 and h-1 against the BACKING (the client's own bytes), and nine samples against the
		// client's pattern.
		if (st == kScanStOk) {
			uint32_t bad = 0, compared = 0, clientOk = 0;
			const uint32_t rows2[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
				const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rows2[r]);
				for (uint32_t x = 0; x < plan.w; x++) { compared++; if (amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u) != row[x]) bad++; }
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				md->readBytes((IOByteCount)py[i] * srcStride + (IOByteCount)px[i] * 4u, &sv, 4);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				if (sv == rdna4_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | 3u;
			if (logit || bad)
			RDNA4LOG("scanout-staged: READBACK rows 0 and %u of the scanout (BAR0) against the surface BACKING: %u of %u pixel(s) "
			       "differ%s; samples dst/backing (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; the backing holds "
			       "the test client's pattern at %u of 3 samples", plan.h - 1, bad, compared, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
		}
		if (row) IOFree(row, cw * sizeof(uint32_t));
		if (prepared) md->complete();
		scanout_free_vram(stg);
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

bool rdna4_scanout_pc_passed(void) { return gScanoutPcPassed; }

// =====================================================================================================================
// build 0.0.542 (dcn/scanout_full.h; the push condition's full-res readback) — `accel scanout 9` / `scanout full`: THE
// SURFACE THE DISPLAY ENGINE IS SCANNING OUT, WHOLE, into a kernel capture buffer the CLI pulls through kRDNA4SelReadScanFull.
// scanout_full.h holds the decision, the gate, the copy's order and the header; this is only the I/O it is given:
//   - the display: rdna4dcn::roScanSurface (the READ-ONLY device; nothing here writes a DCN register);
//   - the console: RDNA4FB's Console,* geometry (scanout_geometry, rdna4_scanout_geom_check), as every scanout path reads it;
//   - flip mode's record, read under gFmLock, which this verb TRY-locks (a present or a restore running = FLIP_BUSY) and holds
//     for the whole copy, so flip mode can neither flip nor write A or B under it;
//   - the staging: switch 89's GART read-back buffer (gFc89Rb, 1 MiB, its read-direction positive control passed), under
//     the fast-copy lock (fc_lock_get), TRY-locked (busy = STAGING_BUSY). Not yet bound this boot: REFUSED as STAGING (0.0.543 E1:
//     never bound here, under gFmLock; `gfxneuter 345` binds and proves it);
//   - each chunk: fastcopy.h's rdna4_fc_submit_wait (gScanoutLock inside it; the fence wait bounded from the doorbell) with sf_kick
//     below (fc89_kick's packet with this verb's gate), the fence in 63's write-back dword with a value never used before; a
//     timeout RETIRES the read-back buffer (89 goes inert) and latches 63 off (rdna4_fc_fence_outcome), as 89's control does.
// Lock order: gFmLock (try) -> the fast-copy lock (try) -> gScanoutLock (per chunk, inside rdna4_fc_submit_wait); gSfLock only around
// the published capture's pointer. OFF / inert: nothing below runs unless `scanout 9`, `scanout 10` or the read selector is called.
// =====================================================================================================================
static_assert(RDNA4_SF_CHUNK_BYTES == RDNA4_FC_STAGING_BYTES, "a scanout-full chunk is exactly switch 89's read-back buffer");
static_assert(RDNA4_SF_SUB_LANDED == RDNA4_FC_SUB_LANDED && RDNA4_SF_SUB_TIMEOUT == RDNA4_FC_SUB_TIMEOUT &&
              RDNA4_SF_SUB_REFUSED == RDNA4_FC_SUB_REFUSED && RDNA4_SF_SUB_RING == RDNA4_FC_SUB_RING,
              "scanout_full.h's submission answers are fastcopy.h's");
static IOLock   *gSfLock { nullptr };     // the published capture (allocated at start())
static uint8_t  *gSfBuf { nullptr };
static uint64_t  gSfLen { 0 }, gSfSeq { 0 }, gSfRuns { 0 }, gSfRefused { 0 };
static uint32_t  gSfDead { 0 };           // a fence that did not land: no further capture this boot
struct SfCtx { const rdna4_sf_plan *p; uint8_t *dst; uint64_t dmaUs; uint32_t lastFence; };
static uint32_t sf_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	const SfCtx *c = static_cast<const SfCtx *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	uint32_t qs = scanout_queue_check(dev, inst, &rptr);
	if (qs == kScanStQueueBusy) {   // the bounded queue-idle wait 89 uses (<= RDNA4_FC89_QWAIT_US), without 89's counters
		uint64_t w = 0;
		if (rdna4_fc89_qwait(&kFc89QWait, RDNA4_FC89_QWAIT_US, &w)) qs = kScanStOk;
	}
	const uint32_t idle = (qs == kScanStOk && inst.wb_bus && gBringup.gmc.vram_start == dev.vramMcBase) ? 1u : 0u;
	// THE LAST GATE, at the ring: the source inside the planned surface, the destination exactly the read-back buffer.
	const uint32_t destWhy = (gFc89Rb.valid() && gFc89RbMc && rdna4_sf_kick_ok(srcMc, dstMc, bytes, c->p, gFc89RbMc, RDNA4_FC_STAGING_BYTES))
	                         ? 0u : 97u;
	if (rdna4_fc_submit_ok(destWhy, idle) != 1u) return RDNA4_FC_SUB_REFUSED;
	uint32_t pkt[RDNA4_FC_PKT_DWORDS];
	const uint32_t k = rdna4_fc_build_packet(pkt, RDNA4_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + RDNA4_FC_WB_FENCE_OFF, fence);
	if (k != RDNA4_FC_PKT_DWORDS) return RDNA4_FC_SUB_REFUSED;
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return RDNA4_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return RDNA4_FC_SUB_RING;
	return 0u;
}
static const rdna4_fc_wait_ops kSfWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &sf_kick, &fc_fence_read, &fc_delay };
static uint32_t sf_fence_next(void *) { return rdna4_fc_fence_next(&gFc); }   // under the fast-copy lock (held)
static uint32_t sf_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	SfCtx *c = static_cast<SfCtx *>(vc);
	rdna4_fc_wait_out w {};
	const uint32_t r = rdna4_fc_submit_wait(&kSfWaitOps, c, srcMc, dstMc, bytes, fence, &w);
	if (w.kicked) { c->dmaUs += w.el_us; c->lastFence = w.seen; }
	amdgpu::sysmem_rmb();            // the fence was read landed: the buffer's reads come after it
	return r;
}
static uint64_t  gSfRetires { 0 };        // build 0.0.543 item E2: captures that retired the read-back buffer (under the fast-copy lock)
static void sf_retire(void *, uint32_t sub) {
	(void)rdna4_fc_fence_outcome(&gFc, sub);   // 63's path latched off, as 89's own control does on a fence that did not land
	gFc89.rb_retired = 1u;                   // the engine may still write the read-back buffer: never handed out again
	gSfDead = 1u;
	// build 0.0.543 item E2 (the 0.0.542 review's SHOULD-FIX): counted in 89's own stats (as its chunk path counts a fence that did
	// not land) and in this verb's, and named on a line: until 0.0.542 the latch and the retirement happened with no line at all.
	if (sub == RDNA4_FC_SUB_RING) gFc89S.ring_fail++; else gFc89S.timeouts++;
	gSfRetires++;
	RDNA4LOG("scanfull542: a chunk's fence did NOT land (%s): switch 63's fast-copy path LATCHED OFF, switch 89's read-back buffer RETIRED "
	       "for the boot (retires %llu; 63 latched %u why %u; 89 timeouts %llu ring %llu)",
	       sub == RDNA4_FC_SUB_RING ? "ring write / doorbell failed" : "fence timeout", (unsigned long long)gSfRetires,
	       gFc.latched_off, gFc.latch_why, (unsigned long long)gFc89S.timeouts, (unsigned long long)gFc89S.ring_fail);
}
static void sf_copy_out(void *vc, uint64_t off, uint32_t bytes) {
	SfCtx *c = static_cast<SfCtx *>(vc);
	memcpy(c->dst + RDNA4_SF_HDR_BYTES + off, gFc89Rb.cpu, bytes);
}
// Flip mode's record as the capture sees it (lock-free for the pre-size; under gFmLock for the decision that counts).
static void sf_ctx_fill(rdna4_sf_ctx *c) {
	c->fm_have_b = __atomic_load_n(&gFm.haveB, __ATOMIC_ACQUIRE);
	c->fm_on = __atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE);
	c->fm_engaged = __atomic_load_n(&gFm.engaged, __ATOMIC_ACQUIRE);
	c->fm_restore_req = __atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE);
	c->fm_a_copy_pending = __atomic_load_n(&gFm.aCopyPending, __ATOMIC_ACQUIRE);
	c->fm_test_held = rdna4dcn::fmTestFlipHeld();
	c->b_off = gFm.bOff; c->b_len = gFm.len; c->b_mc = c->fm_have_b ? gBringup.gmc.vram_start + gFm.bOff : 0u;
	c->fm_front = __atomic_load_n(&gFm.front, __ATOMIC_RELAXED);
	c->armed = 0u;
}
static uint64_t sf_now_us() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }

// out: [0] status (the caller's), [1] reason (RDNA4_SF_*), [2] surface MC, [3] payload bytes, [4] width << 32 | height,
// [5] pitch bytes << 32 | format << 8 | SW_MODE, [6] which | flags << 8, [7] copy us, [8] chunks, [9] FNV-1a, [10] seq,
// [11] frame count before << 32 | after, [12] total bytes published (header + payload).
static uint32_t rdna4_scanout_full(uint32_t mode, uint64_t *v) {
	if (!gSfLock) return RDNA4_SF_R_NO_CONTEXT;
	if (mode == RDNA4_SF_MODE_RELEASE) {
		IOLockLock(gSfLock);
		uint8_t *b = gSfBuf; const uint64_t n = gSfLen;
		gSfBuf = nullptr; gSfLen = 0;
		IOLockUnlock(gSfLock);
		if (b) IOFree(b, n);
		RDNA4LOG("scanfull542: RELEASED capture #%llu (%llu bytes freed)", (unsigned long long)gSfSeq, (unsigned long long)(b ? n : 0));
		return RDNA4_SF_OK;
	}
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock || !gFmLock) return RDNA4_SF_R_NO_CONTEXT;
	gSfRuns++;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	rdna4_sf_ctx c {}; rdna4_sf_dcn d0 {}, d1 {}; rdna4_sf_plan p {};
	RDNA4ScanoutGeom g {};  uint64_t aOff = 0;
	uint8_t *buf = nullptr; uint64_t need = 0;
	uint32_t r = RDNA4_SF_OK, chunks = 0, w0 = 0, w1 = 0;
	uint64_t t0 = 0, t1 = 0;
	clock_sec_t cs = 0; clock_usec_t cu = 0;
	IOLock *fl = nullptr;
	bool fmHeld = false, fcHeld = false;
	SfCtx sc {};
	if (gSfDead) { r = RDNA4_SF_R_RETIRED; goto out; }
	// 1. the console and the display (registry and read-only reads), to size the capture buffer before any lock.
	c.con_ok = (scanout_geometry(dev, &g) == kScanStOk && rdna4_scanout_geom_check(&g, &aOff) == kScanGeomOk &&
	            gBringup.gmc.vram_start == dev.vramMcBase) ? 1u : 0u;
	c.con_w = g.width; c.con_h = g.height; c.con_row_bytes = g.rowBytes;
	c.a_off = aOff; c.a_mc = gBringup.gmc.vram_start + aOff; c.a_len = g.fbLen;
	sf_ctx_fill(&c);
	(void)rdna4dcn::roScanSurface(&d0);
	r = rdna4_sf_decide(&d0, &c, &p);
	if (r) goto out;
	need = RDNA4_SF_HDR_BYTES + p.bytes;
	buf = static_cast<uint8_t *>(IOMalloc(need));
	if (!buf) { r = RDNA4_SF_R_ALLOC; goto out; }
	// 2. flip mode's lock, then the fast-copy lock: both TRY-locked, so this verb never waits behind a present or a copy.
	if (!IOLockTryLock(gFmLock)) { r = RDNA4_SF_R_FLIP_BUSY; goto out; }
	fmHeld = true;
	fl = fc_lock_get();
	if (!fl || !IOLockTryLock(fl)) { r = fl ? RDNA4_SF_R_STAGING_BUSY : RDNA4_SF_R_NO_CONTEXT; goto out; }
	fcHeld = true;
	if (gFc89.rb_retired) { r = RDNA4_SF_R_RETIRED; goto out; }
	// build 0.0.543 item E1 (the 0.0.542 review's SHOULD-FIX; option "refuse", chosen over binding before gFmLock): the read-back
	// buffer is NEVER bound here. fc89_rb_bind + fc89_control take ~58 ms, and under gFmLock that stalled every present; 89 ON
	// (`gfxneuter 345`, rdna4_fc89_set) binds and proves it under the fast-copy lock alone. Unbound = STAGING, nothing waited on.
	if (!(gFc89Rb.valid() && gFc89RbMc && gFc89.rb_ok)) { r = RDNA4_SF_R_STAGING; goto out; }
	if (!fc_sdma_up()) { r = RDNA4_SF_R_QUEUE; goto out; }
	if (fc_dcc_raw() & RDNA4_DCC_NOPTE_COMP_EN_MASK) { r = RDNA4_SF_R_DCC; goto out; }
	// 3. THE DECISION THAT COUNTS, under gFmLock: flip mode is still, the display is read again.
	sf_ctx_fill(&c);
	(void)rdna4dcn::roScanSurface(&d0);
	{
		rdna4_sf_plan p2 {};
		r = rdna4_sf_decide(&d0, &c, &p2);
		if (r) goto out;
		if (p2.bytes != p.bytes) { r = RDNA4_SF_R_CHANGED; goto out; }
		p = p2;
	}
	// 4. the copy (rdna4_sf_run: per chunk the gate, a fence value, ONE bounded submission, the copy-out).
	sc.p = &p; sc.dst = buf;
	w0 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: the writes counted AT the write
	clock_get_calendar_microtime(&cs, &cu);
	t0 = sf_now_us();
	{
		const rdna4_sf_ops ops { &sc, &sf_fence_next, &sf_submit, &sf_retire, &sf_copy_out };
		r = rdna4_sf_run(&ops, &p, gFc89RbMc, &chunks);
	}
	t1 = sf_now_us();
	w1 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);
	// build 0.0.543 item E5: the fast copy's state as this copy left it (the fast-copy lock is still held), for the header.
	c.fc63_latched = gFc.latched_off; c.fc63_why = gFc.latched_off ? gFc.latch_why : 0u;
	c.fc89_state = (gFc89.rb_ok ? 1u : 0u) | (gFc89.rb_retired ? 2u : 0u) | (gFc89.pc_done ? 4u : 0u);
	c.fc89_mode = __atomic_load_n(&gFc89Mode, __ATOMIC_ACQUIRE);
	// 5. the display again: the same surface, no flip pending (else the capture is not one picture).
	if (!r) {
		(void)rdna4dcn::roScanSurface(&d1);
		if (!d1.dcn_ok || d1.pending || d1.earliest != d0.earliest || d1.primary != d0.primary) r = RDNA4_SF_R_CHANGED;
	}
out:
	if (fcHeld) IOLockUnlock(fl);
	if (fmHeld) IOLockUnlock(gFmLock);
	uint32_t fnv = 0;
	if (!r && buf) {
		rdna4_sf_hdr h {};
		rdna4_sf_hdr_fill(&h, &d0, &c, &p);
		if (w1 != w0) h.flags |= RDNA4_SF_F_WRITTEN_DURING;
		h.writers_during = w1 - w0;
		h.uptime_us = t0; h.cal_sec = (uint64_t)cs; h.cal_usec = (uint32_t)cu;
		h.copy_us = (uint32_t)(t1 - t0); h.chunks = chunks;
		h.fc_before = d0.frame_count; h.fc_after = d1.frame_count;
		fnv = rdna4_sf_fnv32(buf + RDNA4_SF_HDR_BYTES, p.bytes);
		h.fnv32 = fnv;
		IOLockLock(gSfLock);
		h.seq = ++gSfSeq;
		memcpy(buf, &h, sizeof(h));
		uint8_t *old = gSfBuf; const uint64_t oldLen = gSfLen;
		gSfBuf = buf; gSfLen = need;
		IOLockUnlock(gSfLock);
		buf = nullptr;
		if (old) IOFree(old, oldLen);
		v[10] = h.seq; v[12] = need;
		v[7] = h.copy_us; v[8] = chunks; v[9] = fnv; v[6] = (uint64_t)p.which | ((uint64_t)h.flags << 8);
		v[11] = ((uint64_t)d0.frame_count << 32) | d1.frame_count;
	} else {
		gSfRefused++;
		v[6] = (uint64_t)p.which | ((uint64_t)p.flags << 8);
	}
	if (buf) IOFree(buf, need);
	v[1] = r; v[2] = d0.earliest; v[3] = p.bytes; v[4] = ((uint64_t)d0.vp_w << 32) | d0.vp_h;
	v[5] = ((uint64_t)d0.pitch_px * 4u << 32) | ((uint64_t)d0.fmt << 8) | d0.sw_mode;
	RDNA4LOG("scanfull542: %s (%u: %s); runs %llu refused %llu", r ? "REFUSED" : "CAPTURED", r, rdna4_sf_reason_name(r),
	       (unsigned long long)gSfRuns, (unsigned long long)gSfRefused);
	RDNA4LOG("scanfull542: surface %s MC %#llx (programmed %#llx, pending %u, OTG %d) %ux%u pitch %u fmt %u SW_MODE %u; %llu bytes in %u "
	       "chunk(s), %llu us (dma %llu us, fence %#010x), FNV %08x; flip mode %s engaged %u front %#llx; frames %u -> %u; writers %u",
	       p.which == 1u ? "A" : p.which == 2u ? "B" : "-", (unsigned long long)d0.earliest, (unsigned long long)d0.primary,
	       d0.pending, (int)d0.lit_otg, d0.vp_w, d0.vp_h, d0.pitch_px * 4u, d0.fmt, d0.sw_mode, (unsigned long long)p.bytes, chunks,
	       (unsigned long long)(t1 - t0), (unsigned long long)sc.dmaUs, sc.lastFence, fnv, c.fm_on ? "ON" : "OFF", c.fm_engaged,
	       (unsigned long long)c.fm_front, d0.frame_count, d1.frame_count, w1 - w0);
	return r;
}

// kRDNA4SelReadScanFull (GarnetUserClient.cpp): up to `max` bytes of the published capture from `off`. Read-only. Returns the bytes
// copied; *total the capture's size (0 = none published), *seq its number.
uint32_t rdna4_scanfull_read(uint64_t off, void *dst, uint32_t max, uint64_t *total, uint64_t *seq) {
	*total = 0; *seq = 0;
	if (!gSfLock || !dst) return 0u;
	uint32_t n = 0;
	IOLockLock(gSfLock);
	if (gSfBuf && off < gSfLen) {
		const uint64_t left = gSfLen - off;
		n = left < max ? (uint32_t)left : max;
		memcpy(dst, gSfBuf + off, n);
	}
	*total = gSfBuf ? gSfLen : 0u;
	*seq = gSfBuf ? gSfSeq : 0u;
	IOLockUnlock(gSfLock);
	return n;
}


// Serialized synchronous single COPY_LINEAR + FENCE on SDMA0 QUEUE0 (used by direct-BAR diagnostic tests).
// Never touches scanout surfaces or gScanoutWrites. Returns kScanStOk (0) on verified fence completion.
uint32_t rdna4_sdma_copy_linear_sync(uint64_t srcMc, uint64_t dstMc, uint32_t bytes,
                                     uint32_t fenceVal, uint32_t timeoutMs) {
	if (!gBringup.dev || !gScanoutLock) return kScanStNoContext;
	if (bytes == 0u || bytes > RDNA4_SDMA_COPY_MAX_BYTES) return kScanStPlan;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	IOLockLock(gScanoutLock);
	uint32_t rptr = 0u;
	uint32_t st = scanout_queue_check(dev, inst, &rptr);
	if (st == kScanStOk) {
		uint64_t us = 0u;
		uint32_t lastFence = 0u;
		st = scanout_sdma_copies(dev, inst, &srcMc, &dstMc, &bytes, 1u, fenceVal, timeoutMs, &us, &lastFence);
	}
	IOLockUnlock(gScanoutLock);
	return st;
}

void rdna4_scanout_sdma_init_locks(void) {
	if (!gScanoutLock) gScanoutLock = IOLockAlloc(); // 0.0.272: never freed, same reason
	if (!gFmLock) gFmLock = IOLockAlloc();           // build 0.0.518 (switch 74, flip mode): never freed, same reason
	if (!gSfLock) gSfLock = IOLockAlloc();           // build 0.0.542 (`scanout full`'s published capture): never freed, same reason
}
