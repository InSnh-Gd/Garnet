// garnet_flipmode.cpp — Double-buffered scanout flip mode and A/B witness
// (split from scanout_sdma.cpp).
#include "dcn_scanout_internal.hpp"

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
