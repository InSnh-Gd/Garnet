// garnet_sdma_fastcopy.cpp — SDMA console copy primitives, fastcopy/fastcopy89 staging,
// full scanout capture, and lock initialization (split from scanout_sdma.cpp).
#include "dcn_scanout_internal.hpp"

IOLock    *gScanoutLock { nullptr };     // every QUEUE0 submission of this feature
bool       gScanoutPcPassed { false };   // the interlock: positive control passed this boot
uint32_t   gScanoutPcRuns { 0 }, gScanoutCopies { 0 }, gScanoutRefusals { 0 };
uint32_t   gScanoutLastReason { 0 };
uint32_t  *gScanoutSave { nullptr };     // the rectangle the positive control overwrote
uint32_t   gScanoutSaveX { 0 }, gScanoutSaveY { 0 }, gScanoutSaveW { 0 }, gScanoutSaveH { 0 };
uint32_t   gScanoutCopyCalls { 0 }, gScanoutCopyFailLogs { 0 };
// build 0.0.543 item E3 (the 0.0.542 review's SHOULD-FIX): scanout WRITES into A or B, counted AT THE WRITE (under gScanoutLock,
// immediately before each writer's SDMA copy), not at the writer's entry: gScanoutCopyCalls++ runs before IOLockLock(gScanoutLock), so a
// writer that entered before `scanout full` sampled it and wrote after was invisible to RDNA4_SF_F_WRITTEN_DURING.
volatile uint32_t gScanoutWrites { 0 };
// build 0.0.518: FLIP MODE's state (dcn/gfx_flipmode.h; switch 74, DEFAULT OFF). Written only under
// gFmLock (lock order: gFmLock, then gScanoutLock inside the copies); `on` and `engaged` are also read lock-free by the other
// scanout writers and the readers (W4), and `restoreReq` is set lock-free by a withdrawal on the submit path.
rdna4_fm  gFm {};
IOLock *gFmLock { nullptr };

// build 0.0.518 (W4): the buffer the display is SCANNING, for our own readers (the `scanout 3` content read and FNV, the
// `scanout 4` thumbnail). Until flip mode has allocated B this boot it answers the console's offset without reading anything
// (0.0.517's readers exactly); afterwards it asks HUBP0's SURFACE_EARLIEST_INUSE (a read through the bound device) and answers
// B's offset when that names B, else the console's.
uint64_t fm_displayed_off(uint64_t consoleOff, uint64_t consoleLen) {
	if (!__atomic_load_n(&gFm.haveB, __ATOMIC_ACQUIRE) || gFm.len < consoleLen) return consoleOff;
	uint32_t pending = 0; uint64_t e = 0;
	if (rdna4dcn::fmReadFront(&pending, &e) != 0) return consoleOff;
	return e == gFm.bMc ? gFm.bOff : consoleOff;
}

// G1: the pure header OWNS the packet, including the 14-dword tiled sub-window the kernel constant
// repeats. Pin them equal so the host-tested arithmetic cannot drift from what the kext emits.
static_assert(RDNA4_SDMA_TILED_SUB_WINDOW_DWORDS == amdgpu::kSDMATiledSubWindowDwords,
              "sdma_gcr.h's tiled sub-window size must equal amdgpu_sdma.h's");

// All scanout consumers use the peer's verified primary and its atomic mode snapshot.
uint32_t scanout_geometry(amdgpu::DeviceContext &dev, RDNA4ScanoutGeom *g) {
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
uint32_t scanout_queue_check(const amdgpu::DeviceContext &dev, const amdgpu::SDMAInstance &inst,
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
uint32_t scanout_sdma_copies(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
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
	if (!gc) return 0xffffffffu;
	amdgpu::ScopedGfxOffGuard gfxOffGuard(*gBringup.dev);
	if (!gfxOffGuard.ok()) return 0xffffffffu;
	return rdna4_reg_read32(gc + RDNA4_SDMA_DCC_CNTL_OFF);
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
	if (!amdgpu::bar0_range_ok(dev, off, RDNA4_FC_STAGING_BYTES)) {
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
uint32_t scanout_sdma_tiled_copy(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                        const RDNA4ScanoutTiledPlan *p, uint64_t tiledMc, uint64_t linearMc,
                                        bool detile, uint32_t fenceValue, uint32_t timeoutMs,
                                        uint64_t *usOut, uint32_t *lastFenceOut, bool gcr) {
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
uint32_t scanout_sdma_copy_linear_gcr(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
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
uint32_t scanout_sdma_copies_chunked(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
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

void scanout_free_vram(amdgpu::VRAMAllocation &a) {
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

uint32_t scanout_full_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
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
	if (!amdgpu::bar0_range_ok(dev, r->probeOff, r->linBytes) || !amdgpu::bar0_range_ok(dev, r->destOff, r->linBytes) ||
	    !amdgpu::bar0_range_ok(dev, r->tiledOff, r->tilBytes) || (r->tiledOff & 0xffffu) || (r->probeOff & 0xffffu) ||
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
uint32_t rdna4_scanout_full(uint32_t mode, uint64_t *v) {
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
