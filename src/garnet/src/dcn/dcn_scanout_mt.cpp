#include "dcn_internal.hpp"

namespace rdna4dcn {

// ---- build 0.0.603: native scanout (S2a). The plane is taken only while a native RDNA4N client holds it ------------------------------
//
// WHAT THIS WRITES: HUBP0's flip registers through dcn41_hubp_program_flip (the sequence proven at, addresses confined to
// the console and the registered slots by rdna4scan::flip_target_ok on top of dcn41's window), and the VUPDATE_NO_LOCK enable/ack bits of the
// lit OTG through dcn41_irq_set / dcn41_irq_ack (the allowlist gates all of it). Nothing else.
//
// THE CONSOLE IS RESTORED BY ONE FUNCTION, scanRestore, shared by: ScanoutRelease, the RDNA4N close / clientDied / stop path, a BoFree of a
// buffer the hardware is showing, `dcnflip 0` (scanEscape), and the watchdog thread (5 s idle, a rate-guard trip requested from the IRQ
// handler). It programs the console address, waits for the latch by POLLING (no interrupt needed), verifies, and only then drops the state.
//
// LOCKS: see ScanState. Every function here that takes gScan.lock does register I/O only under it; the waits are outside it.
IOLock *scan_lock() {
	if (gScan.lock == nullptr) {
		IOLock *l = IOLockAlloc();
		if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gScan.lock)) IOLockFree(l);
	}
	return gScan.lock;
}

// HUBP0's plane geometry, read from the hardware (5 register reads). Caller holds the lock or is single-threaded.
void scan_read_geom(ScanGeom *g) {
	const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  0u));
	const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    0u));
	const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   0u));
	const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, 0u));
	const uint32_t ctl   = dcn_rreg(&gDcn, hubp_reg(kOffSurfControl,  0u));
	g->w = dim & 0xFFFFu; g->h = (dim >> 16) & 0xFFFFu;
	g->pitchPx = (pitch & 0xFFFFu) + 1u;        /* hubp401 writes surface_pitch - 1 */
	g->fmt = cfg & 0x7Fu; g->sw = tile & 0x1Fu;
	g->dcc = (ctl & 0x2u) != 0u;                /* HUBPREQ0_DCSURF_SURFACE_CONTROL.PRIMARY_SURFACE_DCC_EN */
}

// The four fields that make the restore exact (rdna4scan::restore_exact). Acquire only.
struct ScanExact { uint32_t vmid; bool tmz; uint32_t flipType; uint32_t vpStart; };
static void scan_read_exact(ScanExact *x) {
	x->vmid     = dcn_rreg(&gDcn, hubp_reg(kOffVmidSettings, 0u)) & 0xFu;                 /* HUBPREQ0_VMID_SETTINGS_0.VMID */
	x->tmz      = (dcn_rreg(&gDcn, hubp_reg(kOffSurfControl, 0u)) & 0x1u) != 0u;          /* DCSURF_SURFACE_CONTROL.PRIMARY_SURFACE_TMZ */
	x->flipType = (dcn_rreg(&gDcn, hubp_reg(kOffFlipControl, 0u)) >> 1) & 0x1u;           /* DCSURF_FLIP_CONTROL.SURFACE_FLIP_TYPE */
	x->vpStart  = dcn_rreg(&gDcn, hubp_reg(kOffViewportStart, 0u));                       /* HUBP0_DCSURF_PRI_VIEWPORT_START (x and y) */
}

// One observation of HUBP0: resolves a pending Present into a latch. Caller holds gScan.lock. The latch is the RAW DCSURF_FLIP_CONTROL.SURFACE_FLIP_PENDING
// bit clearing (a VUPDATE accepted the address); EARLIEST_INUSE is read too but only feeds the reuse rule (see rdna4scan::latch_observe).
static bool scan_poll_locked(bool byIrq) {
	// Self-heal a latched DCN level-triggered VUPDATE_NO_LOCK interrupt if ihEntry()
	// ever hit a transient rdna4_display_txn collision and dropped an ACK.
	if (!byIrq && __atomic_load_n(&gDcn.irqAckRecoveryNeeded, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) {
		struct dcn41_irq vup { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
		gDcn.tag = "irqrecov";
		if (dcn41_irq_ack(&gDcn.d, vup) == DCN41_OK) {
			__atomic_store_n(&gDcn.irqAckRecoveryNeeded, false, __ATOMIC_RELEASE);
		}
	}
	uint32_t raw = 0;
	const uint64_t fcNow = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK ? rdna4scan::fc_extend(gScan.fc, raw) : gScan.fc.v;
	if (!gScan.tbl.pendActive) return false;
	const uint32_t fcr = dcn_rreg(&gDcn, hubp_reg(kOffFlipControl, 0u));
	if (fcr == 0xFFFFFFFFu) return false;                               // a dead read is "still pending", never a latch
	if (!rdna4scan::latch_observe(gScan.tbl, (fcr & 0x100u) != 0u, fcNow)) return false;
	const uint64_t t = now_ns();
	if (gScan.firstLatchNs == 0) gScan.firstLatchNs = t;
	gScan.lastLatchNs = t;
	if (byIrq) gScan.latchIrq++; else gScan.latchPoll++;
	return true;
}

// The interrupt path (see the declaration before ihEntry). VUPDATE_NO_LOCK: one per frame; here it is the latch witness and the rate guard's input.
bool scanIrq(const struct dcn41_irq &irq, uint64_t nowNs, int *ar) {
	// Primary framebuffer VSTARTUP has its own rate storm guard in ihEntry(); keep it out of scanout's budget.
	if (irq.kind == DCN41_IRQ_VSTARTUP) return false;
	IOLock *l = gScan.lock;
	if (l == nullptr) return false;
	IOLockLock(l);
	if (!gScan.acquired) { IOLockUnlock(l); return false; }        // released between the caller's test and the lock: the caller acks
	gDcn.tag = "irqack";
	*ar = dcn41_irq_ack(&gDcn.d, irq);
	if (rdna4scan::storm_note(gScan.storm, nowNs)) {
		// Disable what we enabled; the restore itself sleeps, so the WATCHDOG THREAD performs it (never this handler).
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "stormoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
		gDcn.stormTripped = true;
		if (off.kind == DCN41_IRQ_VUPDATE_NO_LOCK && off.inst == gDcn.primaryVblOtg) {
			__atomic_store_n(&gDcn.primaryVblEnabled, false, __ATOMIC_RELEASE);
			__atomic_store_n(&gDcn.primaryVblStorm.tripped, true, __ATOMIC_RELEASE);
		}
		gScan.stormTrips++;
		gScan.wantRestore = true; gScan.wantWhy = "IRQ storm guard (rate)";
		RDNA4LOG("rdna4scan: STORM GUARD - more than %u DCE interrupts in %llu ms; source disabled by the handler, console restore requested from the watchdog",
		       rdna4scan::kStormLimit, (unsigned long long)(rdna4scan::kStormWindowNs / 1000000ull));
	}
	if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK && irq.inst == 0u) {
		gScan.vupdates++;
		if (!gScan.restoring && !scan_poll_locked(true)) rdna4scan::note_vupdate_no_latch(gScan.tbl);
	}
	IOLockUnlock(l);
	return true;
}

// Restore the console plane. Idempotent. `force`: also act when nothing is acquired but HUBP0 is not on the console recorded earlier
// (the `dcnflip 0` escape after a restore that failed). `expectGen` nonzero: only if that acquisition is still the current one.
// out[0] = verified (1/0), out[1] = HUBP0's programmed address after. Returns 0.
static uint32_t scan_restore(const char *why, bool force, uint32_t expectGen, uint64_t *out) {
	using namespace rdna4scan;
	IOLock *l = gScan.lock;
	if (l == nullptr) { if (out) { out[0] = 1; out[1] = 0; } return 0; }
	IOLockLock(l);
	if (expectGen != 0u && gScan.gen != expectGen) { IOLockUnlock(l); if (out) { out[0] = 1; out[1] = 0; } return 0; }
	if (gScan.restoring) {
		// Another thread is restoring: wait for it (the lock is dropped around every wait), bounded at 2 s.
		for (uint32_t i = 0; i < 2000u && gScan.restoring; i++) { IOLockUnlock(l); IOSleep(1); IOLockLock(l); }
		uint64_t cur = 0;
		const bool ok = !gScan.restoring && gDcn.armed && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK && cur == gScan.consoleMc;
		IOLockUnlock(l);
		if (out) { out[0] = ok ? 1u : 0u; out[1] = cur; }
		return 0;
	}
	uint64_t plane = 0;
	const bool planeOk = gDcn.armed && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane) == DCN41_OK;
	const bool need = gScan.acquired || (force && gScan.haveConsole && planeOk && plane != gScan.consoleMc);
	if (!need) {
		// Nothing to do. The verdict is still honest: a console learned earlier that the plane is NOT on (a failed restore, or a stray
		// flip) reads as NOT verified, so an idempotent second Release never turns a failure into a success.
		const bool ok = !gScan.haveConsole || (planeOk && plane == gScan.consoleMc);
		if (gScan.haveConsole && planeOk) gScan.restoreFailed = !ok;
		IOLockUnlock(l);
		if (out) { out[0] = ok ? 1u : 0u; out[1] = planeOk ? plane : 0; }
		return 0;
	}
	gScan.restoring = true;
	const uint64_t consoleMc = gScan.consoleMc;
	const uint64_t t0 = now_ns();
	uint64_t pres = gScan.tbl.presents, lat = gScan.tbl.latched, rep = gScan.tbl.replaced, rpt = gScan.tbl.repeats;
	bool verified = false;
	// The machine (rdna4scan::restore_next) decides every step; this loop only performs them. The lock is held for register I/O only and is
	// dropped around every sleep, so the interrupt handler can never wait on a sleeper.
	RestoreSm sm;
	restore_begin(sm, consoleMc);
	RestoreAct act = restore_next(sm, false, 0ull, 0ull, true);
	while (act != kActDone) {
		if (act == kActProgram) {
			gDcn.tag = "scanback";
			const int pr = dcn41_hubp_program_flip(&gDcn.d, 0u, consoleMc, 0u, false, false);
			if (pr != DCN41_OK) RDNA4LOG("rdna4scan: RESTORE attempt %u: programming the console %#llx returned %d", sm.attempts, (unsigned long long)consoleMc, pr);
			act = kActPoll;
			continue;
		}
		IOLockUnlock(l);
		IOSleep(1);                       // a VUPDATE latches within one frame; polled, never waited for by interrupt
		IOLockLock(l);
		bool pend = true; uint64_t early = 0, cur = 0;
		const bool rd = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK;
		act = restore_next(sm, rd, cur, early, pend);
	}
	const uint32_t attempts = sm.attempts;
	// (3) verification read and the state teardown, under the lock
	bool pend = true; uint64_t early = 0, cur = 0;
	const bool rd = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK;
	verified = rd && restore_verified(cur, early, pend, consoleMc);
	if (gDcn.srcEnabled && gDcn.srcKind == (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK && gDcn.srcInst == 0u) {
		if (!(__atomic_load_n(&gDcn.primaryVblEnabled, __ATOMIC_ACQUIRE) && gDcn.primaryVblOtg == 0u)) {
			struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
			gDcn.tag = "scanirqoff";
			(void)dcn41_irq_set(&gDcn.d, off, false);
		}
		gDcn.srcEnabled = false;
	}
	table_init(gScan.tbl, consoleMc, gScan.consoleBytes);
	gScan.acquired = false;
	gScan.wantRestore = false; gScan.wantWhy = nullptr;
	gScan.restores++;
	if (!verified) gScan.restoreFailures++;
	gScan.restoreFailed = !verified;
	__atomic_store_n(&gScan.active, 0u, __ATOMIC_RELEASE);
	gScan.restoring = false;
	RDNA4LOG("rdna4scan: RESTORE (%s) -> console %#llx: plane reads %#llx, EARLIEST_INUSE %#llx, pending %d -> %s after %u attempt(s) in %llu us; "
	       "this session: presents %llu latched %llu replaced %llu repeats %llu",
	       why, (unsigned long long)consoleMc, (unsigned long long)cur, (unsigned long long)early, pend ? 1 : 0,
	       verified ? "VERIFIED" : "*** NOT VERIFIED - run `dcnflip 0` again, then reboot if it persists ***", attempts,
	       (unsigned long long)((now_ns() - t0) / 1000ull), (unsigned long long)pres, (unsigned long long)lat, (unsigned long long)rep, (unsigned long long)rpt);
	IOLockUnlock(l);
	if (out) { out[0] = verified ? 1u : 0u; out[1] = cur; }
	return 0;
}

// The watchdog thread: started by Acquire, one per acquisition. IRQ-INDEPENDENT: it polls the plane itself every 50 ms (which also keeps the
// Status counters honest when no interrupt arrives), restores after 5 s without a scanout call, and performs the restores the IRQ handler
// requests. It ends when its acquisition ends. (The pattern is the kext's other pollers: kernel_thread_start, IOSleep, fall off the end.)
static void scan_watchdog_loop(uint32_t myGen) {
	uint32_t refusals = 0;
	for (;;) {
		IOSleep(50);
		// Acquire before the scan lock: every hardware read in this round,
		// including the poll and geometry check, must be covered by the same
		// display transaction as a possible restore.
		rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
		if (!txn.ok) {
			if (!scanActive()) break;
			if (refusals++ < 8u)
				RDNA4LOG("rdna4scan: WATCHDOG round deferred: display transaction conflict; no register read attempted");
			continue;
		}
		IOLock *l = gScan.lock;
		if (l == nullptr) break;
		IOLockLock(l);
		if (!gScan.acquired || gScan.gen != myGen) { IOLockUnlock(l); break; }
		if (gScan.restoring) { IOLockUnlock(l); continue; }
		(void)scan_poll_locked(false);
		// The live plane must still be the one acquired (covers --hold, where no Present would notice): every 50 ms.
		ScanGeom g {};
		scan_read_geom(&g);
		const bool geomBad = rdna4scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != rdna4scan::kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx;
		const char *why = nullptr;
		if (gScan.wantRestore) why = gScan.wantWhy ? gScan.wantWhy : "requested";
		else if (geomBad) why = "live plane geometry changed";
		else if (rdna4scan::idle_expired(now_ns(), gScan.lastActivityNs)) why = "idle watchdog: 5 s without a scanout call";
		if (why) gScan.wdRestores++;
		IOLockUnlock(l);
		if (why) {
			RDNA4LOG("rdna4scan: WATCHDOG restoring the console: %s", why);
			(void)scan_restore(why, false, myGen, nullptr);
			break;
		}
	}
}
static void scan_watchdog(void *arg, wait_result_t) {
	scan_watchdog_loop((uint32_t)(uintptr_t)arg);
	__atomic_fetch_sub(&gScan.watchdogAlive, 1u, __ATOMIC_RELEASE);   // the thread touches nothing of ours after this
}

uint32_t scanGeneration() { return gScan.gen; }
bool scanActive() { return __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u; }

// 0.0.618 (V1): the vblank timing sample (see dcn.hpp). Reads only: OTG_CONTROL / H,V_TOTAL / OTG_STATUS_POSITION / OTG_V_BLANK_START_END / DP_DTO0, through gLr (its write callback writes nothing).
// The raster constants are cached for 250 ms (a mode change shows within that, and an incoherent position - outside the cached raster - refuses the sample on its own). The cache words are
// plain 32/64-bit stores: perform runs on one thread at a time and a torn cache can only fail the coherence checks.
namespace {
struct VblCache { uint64_t atNs; uint64_t periodNs; uint32_t otg, hTot, vTot, valid; };
VblCache gVbl {};
constexpr uint64_t kVblCacheNs = 250ull * 1000000ull;
bool vbl_refresh(uint64_t nowNs) {
	gVbl.valid = 0u;
	struct dcn41_dev *d = &gLr.d;
	int otg = -1;
	uint32_t width = 0u, height = 0u;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool en = false; uint32_t w = 0u, h = 0u;
		if (dcn41_otg_get_active_size(d, i, &en, &w, &h) != DCN41_OK) return false;
		if (en) { otg = (int)i; width = w; height = h; break; }
	}
	if (otg < 0) return false;
	uint32_t ht1 = 0u, vt1 = 0u;
	if (dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1) != DCN41_OK) return false;
	const uint32_t hTot = ht1 + 1u, vTot = vt1 + 1u;
	// Prefer the complete programmed DTO rate over a table's nominal clock.
	uint64_t pixHz = 0ull;
	if (!rdna4lr_dp_clock(&gLr, static_cast<uint32_t>(otg), &pixHz)) {
		uint32_t ha, va, ht, vt, hf, hs, vf, vs; uint64_t pc = 0;
		if (rdna4lr_live(&gLr, &ha, &va, &ht, &vt, &hf, &hs, &vf, &vs, &pc) != 0u && ht == hTot && vt == vTot) pixHz = pc;
	}
	if (!rdna4lr_raster_matches(&gLr, static_cast<uint32_t>(otg), width, height, hTot, vTot)) return false;
	const uint64_t p = rdna4disp::vbl_period_ns(hTot, vTot, pixHz);
	if (p == 0ull) return false;
	gVbl.otg = (uint32_t)otg; gVbl.hTot = hTot; gVbl.vTot = vTot; gVbl.periodNs = p; gVbl.atNs = nowNs; gVbl.valid = 1u;
	return true;
}
}  // namespace

bool vblSample(uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs) {
	if (!periodNs || !delayNs || !nowAbs || gLr.state != RDNA4LR_READY) return false;
	if (modeTrialBusy()) return false;                                  // the raster is being reprogrammed: nothing coherent to read
	const uint64_t nowNs = now_ns();
	if (!gVbl.valid || nowNs - gVbl.atNs > kVblCacheNs || nowNs < gVbl.atNs) { if (!vbl_refresh(nowNs)) return false; }
	uint32_t vbs = 0, vbe = 0, h = 0, v = 0;
	const uint64_t abs0 = mach_absolute_time();
	if (dcn41_otg_get_scanoutpos(&gLr.d, gVbl.otg, &vbs, &vbe, &h, &v) != DCN41_OK) return false;
	uint64_t delay = 0;
	if (!rdna4disp::vbl_delay_ns(v, h, vbs, gVbl.hTot, gVbl.vTot, gVbl.periodNs, &delay)) { gVbl.valid = 0u; return false; }   // incoherent (a mode change?): re-read the raster next time
	*periodNs = gVbl.periodNs; *delayNs = delay; *nowAbs = abs0;
	return true;
}   // 0.0.617 (K6): lock-free

// 0.0.659 (M6 Stage 1a): the vblank sample of ONE non-DP display, from THAT display's own OTG (inst 1 = the monitor A = OTG1, inst 2 = the monitor B = OTG2; rdna4m6::otg_of_inst), with its own cache (gVbl per instance).
// The raster must be lit with exactly the display's active size (the geometry table's w x h); the period comes from the OTG's totals and the table's pixel clock. Reads only (the same registers as vblSample).
// The DP's sample stays vblSample above, byte for byte.
namespace {
VblCache gVblX[2] {};          // [inst - 1]
// 0.0.660 (T1): the acceptance rule (lit, and EXACTLY the display's active size) is rdna4m6::raster_flow, host-tested over a fake of these two accessors.
struct InstRasterEnv {
	bool active_size(uint32_t otg, bool *en, uint32_t *aw, uint32_t *ah) { return dcn41_otg_get_active_size(&gLr.d, otg, en, aw, ah) == DCN41_OK; }
	bool totals(uint32_t otg, uint32_t *ht1, uint32_t *vt1) { return dcn41_otg_get_totals(&gLr.d, otg, ht1, vt1) == DCN41_OK; }
};
bool vbl_refresh_inst(VblCache &c, uint32_t otg, uint32_t w, uint32_t h, uint64_t pixHz, uint64_t nowNs) {
	c.valid = 0u;
	InstRasterEnv renv; uint32_t hTot = 0u, vTot = 0u;
	if (!rdna4m6::raster_flow(renv, otg, w, h, &hTot, &vTot)) return false;
	const uint64_t p = rdna4disp::vbl_period_ns(hTot, vTot, pixHz);
	if (p == 0ull) return false;
	c.otg = otg; c.hTot = hTot; c.vTot = vTot; c.periodNs = p; c.atNs = nowNs; c.valid = 1u;
	return true;
}
}  // namespace

bool vblSampleInst(uint32_t inst, uint32_t w, uint32_t h, uint64_t pixHz, uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs) {
	if ((inst != rdna4m6::kInstMonA && inst != rdna4m6::kInstMonB) || !periodNs || !delayNs || !nowAbs || gLr.state != RDNA4LR_READY) return false;
	if (modeTrialBusy()) return false;
	VblCache &c = gVblX[inst - 1u];
	const uint32_t otg = rdna4m6::otg_of_inst(inst);
	const uint64_t nowNs = now_ns();
	if (!c.valid || c.otg != otg || nowNs - c.atNs > kVblCacheNs || nowNs < c.atNs) { if (!vbl_refresh_inst(c, otg, w, h, pixHz, nowNs)) return false; }
	uint32_t vbs = 0, vbe = 0, hh = 0, v = 0;
	const uint64_t abs0 = mach_absolute_time();
	if (dcn41_otg_get_scanoutpos(&gLr.d, c.otg, &vbs, &vbe, &hh, &v) != DCN41_OK) return false;
	uint64_t delay = 0;
	if (!rdna4disp::vbl_delay_ns(v, hh, vbs, c.hTot, c.vTot, c.periodNs, &delay)) { c.valid = 0u; return false; }
	*periodNs = c.periodNs; *delayNs = delay; *nowAbs = abs0;
	return true;
}

void scanEscape(const char *why) {
	if (gScan.lock == nullptr) return;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		RDNA4LOG("rdna4scan: REFUSED escape (%s): no display transaction", why ? why : "?");
		return;
	}
	(void)scan_restore(why, true, 0u, nullptr);
}

void scanLogState() {
	if (gScan.acquires == 0) return;
	RDNA4LOG("dcn-state: rdna4scan acquires %llu restores %llu (failed %llu) acquired %d gen %u console %#llx; this session presents %llu latched %llu replaced %llu "
	       "repeats %llu vupdates %llu (latched by irq %llu poll %llu) refused %llu geom-refused %llu watchdog restores %llu storm trips %llu",
	       (unsigned long long)gScan.acquires, (unsigned long long)gScan.restores, (unsigned long long)gScan.restoreFailures, gScan.acquired ? 1 : 0, gScan.gen,
	       (unsigned long long)gScan.consoleMc, (unsigned long long)gScan.tbl.presents, (unsigned long long)gScan.tbl.latched, (unsigned long long)gScan.tbl.replaced,
	       (unsigned long long)gScan.tbl.repeats, (unsigned long long)gScan.vupdates, (unsigned long long)gScan.latchIrq, (unsigned long long)gScan.latchPoll,
	       (unsigned long long)gScan.refused, (unsigned long long)gScan.geomRefused, (unsigned long long)gScan.wdRestores, (unsigned long long)gScan.stormTrips);
}

// ScanoutQuery: read-only, valid before Acquire. 0 or an IOReturn.
uint32_t scanQuery(struct rdna4n_scan_query *o) {
	using namespace rdna4scan;
	if (!gDcn.armed) return kNotReady;
	IOLock *l = scan_lock();
	if (l == nullptr) return kNoResources;
	IOLockLock(l);
	bzero(o, sizeof(*o));
	const int otg = lit_otg();
	ScanGeom g {};
	scan_read_geom(&g);
	uint32_t flags = 0;
	if (otg >= 0) flags |= RDNA4N_SCANQ_LIT;
	if (amdgpu::native_s1b_state().gate == rdna4native::kGateOn) flags |= RDNA4N_SCANQ_NATIVE;
	if (gScan.acquired) flags |= RDNA4N_SCANQ_ACQUIRED;
	if (otg == 0 && geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) == kGeomOk) flags |= RDNA4N_SCANQ_GEOM_OK;
	struct dcn41_otg_timing tm {};
	struct dcn41_timing_decoded td {};
	if (otg >= 0 && dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &tm) == DCN41_OK) {
		dcn41_otg_timing_decode(&tm, &td);
		o->h_active = td.h_active; o->v_active = td.v_active; o->h_total = td.h_total; o->v_total = td.v_total;
	}
	uint64_t pixHz = 0ull;
	if (otg >= 0 && rdna4lr_dp_clock(&gLr, static_cast<uint32_t>(otg), &pixHz)) flags |= RDNA4N_SCANQ_DTO_VALID;
	else {
		uint32_t ha, va, ht, vt, hf, hs, vf, vs; uint64_t pc = 0;
		if (otg >= 0 && liveRaster(&ha, &va, &ht, &vt, &hf, &hs, &vf, &vs, &pc) != 0u) pixHz = pc;
	}
	if (otg >= 0 && !rdna4lr_raster_matches(&gLr, static_cast<uint32_t>(otg), td.h_active, td.v_active, td.h_total, td.v_total)) {
		IOLockUnlock(l);
		return kNotReady;
	}
	if (rdna4m6_latched_on()) flags |= RDNA4N_SCANQ_M6;       // 0.0.659 (M6): the bundle's second channel for the latch (the nub property is the first)
	if (rdna4m6_latched_on() && rdna4m6flip_latched_on() && rdna4m6flip1_latched_on()) flags |= RDNA4N_SCANQ_M6FLIP1;   // 0.0.662 (Stage 2): garnet-m6flip1=1 as well: ABI 1.12's instance 1 (the monitor A) is live
	if (rdna4m6_latched_on() && rdna4m6flip_latched_on()) flags |= RDNA4N_SCANQ_M6FLIP;   // 0.0.661 (M6 Stage 1b): garnet-m6flip=1 (with garnet-m6=1) is latched ON: ABI 1.11's selectors 22..26 (instance 2) are live
	o->pix_clk_khz = (uint32_t)(pixHz / 1000ull);
	o->refresh_mhz = (uint32_t)refresh_mhz(pixHz, td.h_total, td.v_total);
	o->pitch_px = g.pitchPx; o->hubp_format = g.fmt; o->sw_mode = g.sw; o->otg = otg >= 0 ? (uint32_t)otg : 0xFFFFFFFFu;
	o->dcc_en = g.dcc ? 1u : 0u;
	o->flags = flags;
	uint32_t raw = 0;
	const bool haveFc = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK;
	o->frame_count = gScan.acquired ? (haveFc ? fc_extend(gScan.fc, raw) : gScan.fc.v) : (uint64_t)raw;
	uint64_t plane = 0, early = 0; bool pend = false;
	(void)dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane);
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early);
	o->console_mc = gScan.haveConsole ? gScan.consoleMc : plane;
	o->plane_mc = plane; o->earliest_mc = early;
	o->acquired = gScan.acquired ? 1u : 0u;
	o->plane_w = g.w; o->plane_h = g.h;
	IOLockUnlock(l);
	return kOk;
}

uint32_t scanAcquire(uint64_t out[2], uint32_t sess) {
	using namespace rdna4scan;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return kBusy;
	if (!gDcn.armed) return kNotReady;
	if (amdgpu::native_s1b_state().gate != rdna4native::kGateOn) return kNotReady;      // native boots only
	IOLock *l = scan_lock();
	if (l == nullptr) return kNoResources;
	IOLockLock(l);
	uint32_t rc = kOk;
	bool irqOn = false;
	do {
		if (gScan.acquired || gScan.restoring) { rc = kBusy; break; }
		if (__atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u && !rdna4mt::hold_allows_acquire(gMt, sess)) { RDNA4LOG("rdna4scan: Acquire refused: a mode trial is running"); rc = kBusy; break; }   // 0.0.605; 0.0.609: except while a row-120 hold is UP and this session may take it (atomic words only, never the trial engine lock)
		if (gDcn.flipHeld || gDcn.srcEnabled) { RDNA4LOG("rdna4scan: Acquire refused: dcnflip's pattern or a DCN interrupt source is already in use"); rc = kBusy; break; }
		if (lit_otg() != 0) { RDNA4LOG("rdna4scan: Acquire refused: OTG0 is not the lit OTG"); rc = kNotReady; break; }
		ScanGeom g {};
		scan_read_geom(&g);
		const uint32_t gw = geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc);
		if (gw != kGeomOk) {
			RDNA4LOG("rdna4scan: Acquire refused: plane %ux%u pitch %u px fmt %u SW_MODE %u DCC_EN %d fails the geometry check (reason %u: 1 dims 2 format 3 tiling 4 DCC 5 pitch)",
			       g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc ? 1 : 0, gw);
			rc = kUnsupported; break;
		}
		ScanExact ex {};
		scan_read_exact(&ex);
		if (!restore_exact(ex.vmid, ex.tmz, ex.flipType, ex.vpStart)) {
			RDNA4LOG("rdna4scan: Acquire refused: the console plane is not restorable exactly (VMID %u, TMZ %d, FLIP_TYPE %u, PRI_VIEWPORT_START %#x; all must be 0)", ex.vmid, ex.tmz ? 1 : 0, ex.flipType, ex.vpStart);
			rc = kUnsupported; break;
		}
		if (gDcn.d.scanout_hi == 0ull) { RDNA4LOG("rdna4scan: Acquire refused: no frame-buffer window was read at bind (fail closed)"); rc = kNotReady; break; }
		uint64_t cur = 0;
		if (dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) != DCN41_OK || cur == 0ull) { RDNA4LOG("rdna4scan: Acquire refused: HUBP0's primary address is unreadable (%#llx)", (unsigned long long)cur); rc = kNotReady; break; }
		if (cur < gDcn.d.scanout_lo || cur >= gDcn.d.scanout_hi) { RDNA4LOG("rdna4scan: Acquire refused: the console %#llx is outside the flip window, so it could not be restored", (unsigned long long)cur); rc = kNotReady; break; }
		if (gScan.haveConsole && cur != gScan.consoleMc) {
			RDNA4LOG("rdna4scan: Acquire refused: HUBP0 reads %#llx, not the console %#llx learned earlier - run `dcnflip 0` first", (unsigned long long)cur, (unsigned long long)gScan.consoleMc);
			rc = kBusy; break;
		}
		// HUBP0 must be showing what it was last told, with nothing pending: seed the request record from the programmed address (as fmSetExact).
		gDcn.d.hubp_request_addr[0] = cur; gDcn.d.hubp_request_valid[0] = 1;
		bool pend = true; uint64_t early = 0;
		if (dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) != DCN41_OK || pend || early != cur) {
			RDNA4LOG("rdna4scan: Acquire refused: a flip is pending or HUBP0 is not fetching its programmed address (pending %d, EARLIEST_INUSE %#llx, programmed %#llx)",
			       pend ? 1 : 0, (unsigned long long)early, (unsigned long long)cur);
			rc = kBusy; break;
		}
		// The VUPDATE source (the latch witness). One source at a time, as dcnvbl.
		struct dcn41_irq on { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
		gDcn.tag = "scanirq";
		if (dcn41_irq_set(&gDcn.d, on, true) != DCN41_OK) { RDNA4LOG("rdna4scan: Acquire refused: enabling VUPDATE_NO_LOCK on OTG0 failed"); rc = kNotReady; break; }
		irqOn = true;
		gDcn.srcEnabled = true; gDcn.srcKind = on.kind; gDcn.srcInst = on.inst;
		gDcn.enabledAtNs = now_ns(); gDcn.entriesAtEnable = gDcn.dceEntries; gDcn.stormTripped = false;
		gDcn.firstEntryNs = 0; gDcn.lastEntryNs = 0; gDcn.logBudget = kDceLogBudget;

		gScan.haveConsole = true;
		gScan.consoleMc = cur;
		gScan.consoleBytes = (uint64_t)g.pitchPx * 4ull * g.h;
		gScan.width = g.w; gScan.height = g.h; gScan.pitchPx = g.pitchPx;
		table_init(gScan.tbl, cur, gScan.consoleBytes);
		gScan.fc = FcExt{ 0, false };
		gScan.storm = Storm{ 0, 0, false };
		gScan.presentSeq = 0; gScan.vupdates = 0; gScan.latchIrq = 0; gScan.latchPoll = 0; gScan.refused = 0; gScan.geomRefused = 0;
		gScan.wdRestores = 0; gScan.stormTrips = 0;
		gScan.firstLatchNs = 0; gScan.lastLatchNs = 0; gScan.geomWarned = false;
		gScan.wantRestore = false; gScan.wantWhy = nullptr;
		uint32_t raw = 0;
		if (dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK) (void)fc_extend(gScan.fc, raw);
		gScan.lastActivityNs = now_ns();
		gScan.restoreFailed = false;
		gScan.gen++;
		gScan.acquires++;
		gScan.acquired = true;
		__atomic_store_n(&gScan.active, 1u, __ATOMIC_RELEASE);
		thread_t th = nullptr;
		__atomic_fetch_add(&gScan.watchdogAlive, 1u, __ATOMIC_ACQ_REL);
		const kern_return_t kr = kernel_thread_start(&scan_watchdog, (void *)(uintptr_t)gScan.gen, &th);
		if (kr != KERN_SUCCESS) {
			__atomic_fetch_sub(&gScan.watchdogAlive, 1u, __ATOMIC_ACQ_REL);
			RDNA4LOG("rdna4scan: Acquire refused: the watchdog thread did not start (kr %#x)", kr);
			gScan.acquired = false;
			__atomic_store_n(&gScan.active, 0u, __ATOMIC_RELEASE);
			rc = kNotReady; break;
		}
		thread_deallocate(th);
		irqOn = false;   // owned by the acquisition now
		rdna4mt::hold_take_owner(gMt, sess);   // 0.0.609: a HANDOFF hold with no owner passes to this session (atomic words only); a no-op when no hold is up
		out[0] = cur; out[1] = gScan.fc.v;
		RDNA4LOG("rdna4scan: ACQUIRED gen %u: plane %ux%u pitch %u px ARGB8888 linear, console %#llx (%llu bytes), window [%#llx, %#llx), VUPDATE_NO_LOCK enabled on OTG0, frame %llu",
		       gScan.gen, g.w, g.h, g.pitchPx, (unsigned long long)cur, (unsigned long long)gScan.consoleBytes,
		       (unsigned long long)gDcn.d.scanout_lo, (unsigned long long)gDcn.d.scanout_hi, (unsigned long long)gScan.fc.v);
	} while (false);
	if (rc != kOk && irqOn) {
		struct dcn41_irq off { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
		gDcn.tag = "scanirqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
	}
	IOLockUnlock(l);
	return rc;
}

uint32_t scanRegister(bool boVis, uint64_t boMc, uint64_t boSize, uint64_t offset, uint32_t pitchBytes, uint32_t width, uint32_t height,
                      uint32_t format, uint64_t out[2]) {
	using namespace rdna4scan;
	IOLock *l = gScan.lock;
	if (l == nullptr) return kNotReady;
	IOLockLock(l);
	uint32_t rc = kOk;
	do {
		if (!gScan.acquired || gScan.restoring || gScan.wantRestore) { rc = kNotReady; break; }
		gScan.lastActivityNs = now_ns();
		ScanGeom g {};
		scan_read_geom(&g);
		if (geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx) { rc = kNotReady; break; }
		if (width != g.w || format != kFmtArgb8888) { rc = kBadArg; break; }
		const Fb fb { gDcn.d.scanout_lo, gDcn.d.scanout_hi, gScan.consoleMc, gScan.consoleBytes };
		uint64_t mc = 0, bytes = 0;
		rc = reg_bounds(BoRef{ boVis, boMc, boSize }, offset, pitchBytes, height, g.pitchPx * 4u, g.h, fb, &mc, &bytes);
		if (rc != kOk) break;
		if (rdna4m6_latched_on() && rdna4m6flip_latched_on() && scanXClash(mc, bytes)) { RDNA4LOG("rdna4scan: REGISTER refused: MC %#llx overlaps an HDMI display's buffers or slots (0.0.661 the monitor B's; 0.0.662 also the monitor A's with garnet-m6flip1)", (unsigned long long)mc); rc = kBadArg; break; }   // 0.0.661: an instance-0 slot never aliases the monitor B's memory (with the latches OFF this line is not reached: 0.0.660's path)
		uint32_t slot = kNoSlot;
		rc = slot_register(gScan.tbl, mc, bytes, &slot);
		if (rc != kOk) break;
		out[0] = slot; out[1] = mc;
		RDNA4LOG("rdna4scan: REGISTER slot %u = MC %#llx (%llu bytes, BO offset %#llx)", slot, (unsigned long long)mc, (unsigned long long)bytes, (unsigned long long)offset);
	} while (false);
	IOLockUnlock(l);
	return rc;
}

uint32_t scanPresent(uint64_t slotIn, uint64_t flags, uint64_t out[3]) {
	using namespace rdna4scan;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return kBusy;
	if (flags != 0ull || slotIn >= kMaxSlots) return kBadArg;
	IOLock *l = gScan.lock;
	if (l == nullptr) return kNotReady;
	IOLockLock(l);
	uint32_t rc = kOk;
	do {
		if (!gScan.acquired) { rc = kNotReady; break; }
		if (gScan.restoring || gScan.wantRestore) { gScan.refused++; rc = kNotReady; break; }
		gScan.lastActivityNs = now_ns();
		ScanGeom g {};
		scan_read_geom(&g);
		if (geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx) {
			gScan.geomRefused++; gScan.refused++;
			if (!gScan.geomWarned) { gScan.geomWarned = true; RDNA4LOG("rdna4scan: Present refused: the live plane %ux%u pitch %u fmt %u SW_MODE %u DCC %d is no longer the acquired geometry", g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc ? 1 : 0); }
			rc = kNotReady; break;
		}
		(void)scan_poll_locked(false);                      // resolve the previous Present first: a latch that the IRQ has not delivered yet is not "replaced"
		if (!gScan.tbl.s[slotIn].used) { rc = kNotFound; break; }
		uint64_t plane = 0, early = 0; bool pend = true;
		const bool planeOk = dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane) == DCN41_OK;
		const bool earlyOk = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK;
		if (!present_target_reusable(gScan.tbl, (uint32_t)slotIn, plane, early, planeOk, earlyOk)) {
			gScan.refused++;
			rc = planeOk && earlyOk ? kBusy : kNotReady;
			if (gScan.refused <= 8ull || (gScan.refused & 255ull) == 0ull)
				RDNA4LOG("rdna4scan: Present reuse refused: slot %llu MC %#llx plane %#llx early %#llx front %u pending_slot %u hw_pending %d read_ok %d/%d refused %llu",
				       (unsigned long long)slotIn, (unsigned long long)gScan.tbl.s[slotIn].mc, (unsigned long long)plane, (unsigned long long)early,
				       gScan.tbl.front, gScan.tbl.pendActive ? gScan.tbl.pendSlot : kNoSlot, pend ? 1 : 0, planeOk ? 1 : 0, earlyOk ? 1 : 0, (unsigned long long)gScan.refused);
			break;
		}
		// The prior flip can latch after our poll while EARLIEST_INUSE still lags. Keep its slot protected until observed.
		if (gScan.tbl.pendActive) { gScan.refused++; rc = kBusy; break; }
		const Table saved = gScan.tbl;
		uint32_t raw = 0;
		const uint64_t fcNow = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK ? fc_extend(gScan.fc, raw) : gScan.fc.v;
		const uint64_t id = gScan.presentSeq + 1ull;
		const uint64_t target = fcNow + 1ull;
		const uint64_t mc = present_begin(gScan.tbl, (uint32_t)slotIn, id, target);
		if (mc == 0ull || !flip_target_ok(gScan.tbl, mc)) { gScan.tbl = saved; gScan.refused++; rc = kBadArg; break; }
		gDcn.tag = "scanflip";
		const int fr = dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false);
		if (fr != DCN41_OK) { gScan.tbl = saved; gScan.refused++; RDNA4LOG("rdna4scan: Present of slot %llu refused by the flip layer (%d)", (unsigned long long)slotIn, fr); rc = kNotReady; break; }
		gScan.presentSeq = id;
		out[0] = id; out[1] = target; out[2] = gScan.vupdates;
	} while (false);
	IOLockUnlock(l);
	return rc;
}

uint32_t scanStatus(struct rdna4n_scan_status *o) {
	using namespace rdna4scan;
	IOLock *l = gScan.lock;
	if (l == nullptr || !gDcn.armed) return kNotReady;
	IOLockLock(l);
	bzero(o, sizeof(*o));
	if (gScan.acquired && !gScan.restoring) { gScan.lastActivityNs = now_ns(); (void)scan_poll_locked(false); }
	uint64_t plane = 0, early = 0; bool pend = false;
	const bool earlyOk = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK;
	const bool planeOk = dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane) == DCN41_OK;
	uint32_t raw = 0;
	const bool haveFc = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK;
	o->acquired = gScan.acquired ? 1u : 0u;
	o->flags = (gScan.restoring ? RDNA4N_SCANST_RESTORING : 0u) | (gScan.storm.tripped ? RDNA4N_SCANST_STORM : 0u) | (gScan.wantRestore ? RDNA4N_SCANST_WANT_RESTORE : 0u);
	o->front_slot = gScan.tbl.front;
	o->pending_slot = gScan.tbl.pendActive ? gScan.tbl.pendSlot : kNoSlot;
	o->frame_count = gScan.acquired ? (haveFc ? fc_extend(gScan.fc, raw) : gScan.fc.v) : (uint64_t)raw;
	o->console_mc = gScan.haveConsole ? gScan.consoleMc : plane; o->plane_mc = plane; o->earliest_mc = early;
	o->presents = gScan.tbl.presents; o->latched = gScan.tbl.latched; o->replaced = gScan.tbl.replaced; o->repeats = gScan.tbl.repeats;
	o->vupdates = gScan.vupdates; o->latch_irq = gScan.latchIrq; o->latch_poll = gScan.latchPoll; o->refused = gScan.refused;
	o->first_latch_ns = gScan.firstLatchNs; o->last_latch_ns = gScan.lastLatchNs;
	const uint64_t t = now_ns();
	o->idle_ms = (gScan.acquired && t >= gScan.lastActivityNs) ? (t - gScan.lastActivityNs) / 1000000ull : 0ull;
	o->watchdog_restores = gScan.wdRestores; o->storm_trips = gScan.stormTrips; o->geom_refused = gScan.geomRefused;
	for (uint32_t i = 0; i < kMaxSlots; i++) {
		const Slot &s = gScan.tbl.s[i];
		rdna4n_scan_slot &d = o->slot[i];
		d.mc = s.mc; d.latched_frame = s.latchedFrame; d.used = s.used ? 1u : 0u; d.presents = s.presents; d.latches = s.latches;
		if (!s.used) continue;
		d.flags = ((gScan.tbl.pendActive && gScan.tbl.pendSlot == i) ? RDNA4N_SCANSLOT_PENDING : 0u) | (((earlyOk && early == s.mc) || (planeOk && plane == s.mc)) ? RDNA4N_SCANSLOT_INUSE : 0u) |
		          (present_target_reusable(gScan.tbl, i, plane, early, planeOk, earlyOk) ? RDNA4N_SCANSLOT_REUSABLE : 0u);
	}
	IOLockUnlock(l);
	return kOk;
}

uint32_t scanRelease(const char *why, uint64_t out[2]) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { if (out) { out[0] = 0; out[1] = 0; } return rdna4scan::kBusy; }
	return scan_restore(why, false, 0u, out);
}

// A BO hosting scanout slots is going away (BoFree, the HUNG leak, the close of the session). rdna4scan::unpin_plan decides: alwaysFull (leak / close)
// or ANY slot that is shown (EARLIEST_INUSE) or pending forces the full restore FIRST, otherwise the slots are just dropped. pinGen is the
// scanGeneration() the pins were recorded under; a stale pin (an earlier acquisition) is a no-op. Returns 1 when the console was restored here.
// out (may be null) receives the restore's [verified, plane] when it ran.
uint32_t scanBoGone(uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out) {
	using namespace rdna4scan;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	// A refused transaction cannot prove the BO is no longer being scanned.
	// Report it as an unverified restore so the caller leaks the BO.
	if (!txn.ok) {
		if (out) { out[0] = 0; out[1] = 0; }
		return 2u;
	}
	IOLock *l = gScan.lock;
	if (l == nullptr) return 0u;
	IOLockLock(l);
	bool full = false;
	uint32_t rc = 0u;
	if (!gScan.acquired && gScan.gen == pinGen && gScan.restoreFailed) rc = 2u;   // an earlier restore did not verify: the plane may still scan this BO
	if (gScan.acquired && gScan.gen == pinGen) {
		uint64_t early = 0; bool pend = false;
		const bool ok = !gScan.restoring && dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK;
		const UnpinPlan plan = unpin_plan(gScan.tbl, (uint8_t)pinMask, alwaysFull || gScan.restoring, early, ok);
		full = plan.fullRelease;
		if (!full) {
			for (uint32_t i = 0; i < kMaxSlots; i++) {
				if (((plan.dropMask >> i) & 1u) == 0u) continue;
				(void)slot_unregister(gScan.tbl, i, early, ok);
				RDNA4LOG("rdna4scan: slot %u unregistered (its BO was freed while neither shown nor pending)", i);
			}
		}
	}
	IOLockUnlock(l);
	if (!full) return rc;
	uint64_t r[2] = { 1, 0 };
	(void)scan_restore("a BO with a scanout slot is being released", false, pinGen, r);
	if (out) { out[0] = r[0]; out[1] = r[1]; }
	return r[0] != 0ull ? 1u : 2u;
}

// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half ------------------------------------------------------------------------------
//
// WHAT THIS WRITES: through rdna4mt::Hw.wr = mt_wr = dcn41_allow_write + WREG32, i.e. THROUGH THE DCN ALLOWLIST, exactly the registers of modetrial_tables.h (kCap): the DP DTO
// trio, the OTG timing + global sync set, the DP1 MSA timing, and for row 120 the HUBP0 DLG/TTU/prefetch set. Nothing else, and never a clock: the clocks are only READ (DFS / DENTIST,
// decoded with the C1 DID table), and a row whose DML needs more than the readback says is DENIED. Every decision and every sequence (deny gate, plan, Linux write order, dwell, THE
// restore, the watchdog body, the latch) is in dcn/modetrial_pure.h and is driven by tests/native_s2d_test.cpp; this block only supplies the hardware.
// 0.0.609 (HELD mode, row 120): LOCK ORDER — gMtLock is a LEAF and is never held together with the scan lock.
static bool mt_ensure_lock() {
	if (gMtLock == nullptr) {
		IOLock *l = IOLockAlloc();
		if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gMtLock)) IOLockFree(l);
	}
	return gMtLock != nullptr;
}
static void mt_lock(void *) { IOLockLock(gMtLock); }
static void mt_unlock(void *) { IOLockUnlock(gMtLock); }
static uint32_t mt_rd(void *, uint32_t abs) {
	if (!gDcn.armed || gDcn.dev == nullptr) return 0xFFFFFFFFu;
	return dcn_rreg(&gDcn, abs);
}
// The allowlisted write, with the answer dcn_wreg does not give: false = the allowlist refused it (nothing reached the hardware).
static bool mt_wr(void *, uint32_t abs, uint32_t v) {
	if (!gDcn.armed || gDcn.dev == nullptr) return false;
	if (!dcn_display_txn_ok("mode-trial register write")) return false;
	gDcn.tag = "modetrial";
	if (!dcn41_allow_write(&gDcn.allow, abs, v, gDcn.tag)) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs, gDcn.allow.mmio_dwords, nullptr, &why);
		RDNA4LOG("mode-trial: REFUSED write %#010x = %#010x - allowlist says %s: %s", abs, v, dcn41_allow_reason_name(reason), why ? why : "(no text)");
		return false;
	}
	amdgpu::WREG32(*gDcn.dev, abs, v);
	return true;
}
static void mt_sleep(void *, uint32_t ms) { /*nolock*/ IOSleep(ms); }   // never called with the scanout lock held; 0.0.606: the resync's msleep(60) runs under gMtLock (see the lock rank above)
static void mt_delay_us(void *, uint32_t us) { IODelay(us); }         // 0.0.606: the polls of the sequences (a few us to 200 us each)
static uint64_t mt_now_us(void *) { return now_ns() / 1000ull; }
static bool mt_frame(void *, uint32_t *fc) { return gDcn.armed && dcn41_otg_get_frame_count(&gDcn.d, 0u, fc) == DCN41_OK; }
static bool mt_gate_ok(void *) {
	const amdgpu::NativeS1bState &s = amdgpu::native_s1b_state();
	return s.gate == rdna4native::kGateOn && s.positivePass;
}
static bool mt_armed(void *) { return gDcn.armed && gDcn.dev != nullptr; }
static bool mt_plane_acquired(void *) {
	IOLock *l = scan_lock();
	if (l == nullptr) return true;    // fail closed
	IOLockLock(l);
	const bool a = gScan.acquired || gScan.restoring;
	IOLockUnlock(l);
	return a;
}
static bool mt_in_use(void *) { return gDcn.flipHeld || gDcn.srcEnabled || amdgpu::dal_busy(); }   // 0.0.605: a running DAL step counts as "in use"
// OTG0 is the ONLY lit OTG, and HUBP0 scans the 2560x1440 linear ARGB8888 plane the DML goldens were computed for.
static bool mt_otg_ok(void *) {
	if (!gDcn.armed) return false;
	uint32_t lit = 0, mask = 0;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool en = false; uint32_t w = 0, h = 0;
		if (dcn41_otg_get_active_size(&gDcn.d, i, &en, &w, &h) == DCN41_OK && en) { lit++; mask |= 1u << i; }
	}
	if (lit != 1u || mask != 1u) return false;
	ScanGeom g {};
	scan_read_geom(&g);
	return g.w == 2560u && g.h == 1440u && g.pitchPx == 2560u && rdna4scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) == rdna4scan::kGeomOk;
}
static bool mt_clocks(void *, rdna4dal::Decoded *d) {
	if (gDcn.dev == nullptr) return false;
	*d = rdna4dal::decode_clocks(amdgpu::RREG32(*gDcn.dev, rdna4dal::kRegPllReq), amdgpu::RREG32(*gDcn.dev, rdna4dal::kRegDfs0),
	                           amdgpu::RREG32(*gDcn.dev, rdna4dal::kRegDfs1), amdgpu::RREG32(*gDcn.dev, rdna4dal::kRegDentist));
	return true;
}
static void mt_log(void *, uint32_t code, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
static bool mt_start_watchdog(void *, uint32_t trialId);
// 0.0.607, P4: the clock hold. These four talk to the PMFW through the amd/ layer and SLEEP: they are called by the trial thread, `dcnmode 0` and the kext stop with NO lock held (not gMtLock, not the scanout
// lock), never by the watchdog. They take no lock of this file and reference neither (the host test pins that).
static uint32_t mt_hold_pre(void *) { return amdgpu::dal_hold_pre(); }
static uint32_t mt_hold_raise(void *, uint32_t needDispKhz, uint32_t needDppKhz, rdna4dal::HoldRep *rep, rdna4dal::Decoded *after) {
	if (gDcn.dev == nullptr) return rdna4dal::kHrNoIo;
	return amdgpu::dal_hold_raise(*gDcn.dev, needDispKhz, needDppKhz, rep, after);
}
static uint32_t mt_hold_release(void *, bool restoreBad, rdna4dal::HoldRep *rep) {
	if (gDcn.dev == nullptr) return rdna4dal::kRlNotHeld;
	return amdgpu::dal_hold_release(*gDcn.dev, restoreBad, rep);
}
static uint32_t mt_hold_state(void *) { return amdgpu::dal_hold_state(); }
// 0.0.609: the scanout plane back to the console before a hold's timing is restored. scan_restore takes the scan lock itself and sleeps with it dropped; called with NO lock held (the pure engine guarantees it).
// 0 = nothing was acquired and the console is on the plane, 1 = acquired and the console is back and VERIFIED, 2 = the console is NOT verified back (this put-back, or an earlier one that failed).
static uint32_t mt_scan_release(void *) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return 2u;
	const bool acq = mt_plane_acquired(nullptr);
	uint64_t o[2] = { 1, 0 };
	(void)scan_restore("row-120 hold ending", true, 0u, o);   // force: also acts when an EARLIER put-back (owner close, idle watchdog) failed and left acquired false with the plane off the console
	return o[0] != 1ull ? 2u : (acq ? 1u : 0u);
}
static const rdna4mt::Hw kMtHw = { nullptr, mt_rd, mt_wr, mt_sleep, mt_now_us, mt_frame, mt_lock, mt_unlock, mt_gate_ok, mt_armed, mt_plane_acquired, mt_in_use, mt_otg_ok, mt_clocks,
                                 mt_start_watchdog, mt_log, mt_delay_us, mt_hold_pre, mt_hold_raise, mt_hold_release, mt_hold_state, mt_scan_release };
// 0.0.607: the timing table. Row 120 stays DENIED (kProd.row120 = false) unless the boot-arg garnet-row120=1 is present.
static rdna4mt::Timing mt_timing() {
	rdna4mt::Timing t = rdna4mt::kProd;
	uint32_t v = 0;
	t.row120 = PE_parse_boot_argn("garnet-row120", &v, sizeof(v)) && v == 1u;
	return t;
}
static void mt_watchdog(void *arg, wait_result_t) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		RDNA4LOG("mode-trial: watchdog could not acquire a display transaction; no watchdog write attempted");
		__atomic_fetch_sub(&gMtWdAlive, 1u, __ATOMIC_RELEASE);
		return;
	}
	rdna4mt::watchdog_body(kMtHw, gMt, mt_timing(), (uint32_t)(uintptr_t)arg);
	__atomic_fetch_sub(&gMtWdAlive, 1u, __ATOMIC_RELEASE);      // the thread touches nothing of ours after this
}
static bool mt_start_watchdog(void *, uint32_t trialId) {
	thread_t th = nullptr;
	__atomic_fetch_add(&gMtWdAlive, 1u, __ATOMIC_ACQ_REL);
	const kern_return_t kr = kernel_thread_start(&mt_watchdog, (void *)(uintptr_t)trialId, &th);
	if (kr != KERN_SUCCESS) {
		__atomic_fetch_sub(&gMtWdAlive, 1u, __ATOMIC_ACQ_REL);
		RDNA4LOG("mode-trial: the watchdog thread did not start (kr %#x): the trial is refused", kr);
		return false;
	}
	thread_deallocate(th);
	return true;
}
static void mt_log(void *, uint32_t code, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	using namespace rdna4mt;
	switch (code) {
	case kLogWrite:    RDNA4LOG("mode-trial: WRITE   %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;
	case kLogRestore:  RDNA4LOG("mode-trial: RESTORE %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;
	case kLogDeny:     RDNA4LOG("mode-trial: DENIED row %llu, reason %llu (nothing written)", (unsigned long long)b, (unsigned long long)a); break;
	case kLogWatchdog: RDNA4LOG("mode-trial: *** WATCHDOG restored %llu registers (mismatch %llu, %llu pass(es)) ***", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c); break;
	case kLogVerdict:  RDNA4LOG("mode-trial: verdict %llu flags %#llx mismatch %llu trial rate %llu mHz", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d); break;
	case kLogSeq:      RDNA4LOG("mode-trial: SEQ     %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;   // 0.0.606
	case kLogSeqEnd: {  // which: 1 blank 2 unblank 3 lock 4 unlock 5 pending 6 latch 7 drr 8 uf-clear 9 stuck-lock 10 resync
		static const char *const kSeqName[] = { "?", "blank", "unblank", "lock", "unlock", "pending", "latch", "drr", "uf-clear", "stuck-lock", "resync" };
		RDNA4LOG("mode-trial: %s: rc %llu in %llu us (%llu)", a < 11ull ? kSeqName[a] : "?", (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
		break;
	}
	default: break;
	}
}

// Bind time (native boots only): the full golden copy. Read-only. Never after a write (golden_take refuses), never over an earlier copy.
void mt_golden_at_bind() {
	if (amdgpu::native_s1b_state().gate != rdna4native::kGateOn || !mt_ensure_lock()) return;
	if (rdna4mt::golden_take(kMtHw, gMt, true)) RDNA4LOG("dcn: GOLDEN mode-trial register copy taken at bind on a native boot (%u registers, before any native write)", rdna4mt::kCapN);
	else RDNA4LOG("dcn: the mode-trial golden copy was NOT taken at bind (a register read as all-ones, or a write already happened); the first trial retries it read-only");
}

// `dcnmode 0` on a native boot: end a running trial (it restores and answers ABORT), then put every register of the golden set that differs back. True when it acted (a golden copy exists).
bool mt_emergency_restore(const char *why) {
	if (amdgpu::native_s1b_state().gate != rdna4native::kGateOn || !mt_ensure_lock() || !gDcn.armed) return false;
	rdna4mt::request_abort(kMtHw, gMt);
	rdna4mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: a running hold's scanout plane goes back FIRST (no lock held here)
	const bool idle = rdna4mt::wait_idle(kMtHw, gMt, 20000u, 50u);
	if (!gMt.everWrote) {                  // 0.0.605 review: no trial ever wrote anything this boot, so there is nothing of ours to put back (as mt_shutdown)
		// 0.0.607: but a hold raised by a trial that died before its first write is still HELD: nothing was written, the display is at 60 Hz, so it comes off after 3 s of the 60 Hz rate reads back.
		uint32_t prc0 = 0u;
		const bool healthy0 = rdna4mt::health_now(kMtHw, &prc0);
		const uint32_t rel0 = rdna4mt::emergency_release(kMtHw, gMt, mt_timing(), idle, 0u, healthy0, 3000u);
		if (rel0 != rdna4dal::kRlNotHeld) RDNA4LOG("mode-trial: %s: clock hold release (nothing written) -> %u; hold state %s", why, rel0, rdna4dal::hold_state_name(amdgpu::dal_hold_state()));
		return false;
	}
	rdna4mt::Recover rc {};
	rdna4mt::emergency_recover(kMtHw, gMt, &rc);   // 0.0.606: the golden restore, then the DP1 resync when the display back end is unhealthy
	RDNA4LOG("mode-trial: %s: full golden restore - %s, %u register(s) written, %u still differ, trial %s", why, rc.have ? "copy present" : "NO COPY (nothing touched)", rc.restored, rc.bad, idle ? "idle" : "STILL RUNNING");
	// 0.0.607 (P4 caller `dcnmode 0`): a clock hold a dead trial left HELD is released after the golden restore + resync VERIFIED, including 3 s of the 60 Hz rate. A restore that did not verify never releases.
	if (rc.have) {
		const uint32_t rel = rdna4mt::emergency_release(kMtHw, gMt, mt_timing(), idle, rc.bad, rc.healthyAfter, 3000u);
		if (rel != rdna4dal::kRlNotHeld) RDNA4LOG("mode-trial: %s: clock hold release -> %u (%s); hold state %s", why, rel, rel == rdna4dal::kRlOk ? "released" : rel == rdna4dal::kRlBadRestore ? "NOT released: the restore did not verify" : "FAILED", rdna4dal::hold_state_name(amdgpu::dal_hold_state()));
	}
	if (rc.have) RDNA4LOG("mode-trial: %s: display back end %s before; resync %s (rc %u); %s after, DIO_ERROR_COUNT growth %u since", why, rc.healthyBefore ? "healthy" : "UNHEALTHY", rc.resynced ? "ran" : "not needed", rc.rc, rc.healthyAfter ? "healthy" : "STILL UNHEALTHY", rc.dioGrowth);
	return rc.have;
}

// The selector body (n1c_mode_trial). Success unless the arguments are malformed; the verdict is inside *o.
uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct rdna4n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return rdna4scan::kNoResources;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return rdna4scan::kBusy;
	if (row == 120u) {
		const int otg = lit_otg();
		dcn_log_dp_preflight(otg >= 0 ? static_cast<uint32_t>(otg) : rdna4dp::unknown,
		                     {2560u, 1440u, 2720u, 1525u, 497750000ull}, "trial-reference-only-not-current-EDID");
	}
	rdna4mt::run_trial(kMtHw, gMt, mt_timing(), row > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)row, dwellMs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dwellMs, flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags, o);
	RDNA4LOG("mode-trial: row %u dwell %u ms -> %s (verdict %u, deny %u, flags %#x): wrote %u, skipped %u; watchdog %u; latched %u",
	       o->row, o->dwell_req_ms, o->verdict == RDNA4N_MODE_V_PASS ? "PASS" : o->verdict == RDNA4N_MODE_V_DENIED ? "DENIED" : "FAILED", o->verdict, o->deny, o->flags, o->nwrite, o->nskip, o->wd_fired, o->latched);
	RDNA4LOG("mode-trial: OTG %u -> %u mHz (expected %u), after %u mHz; DIO fifo %u -> %u count %u -> %u; registers still differing %u",
	       o->rate_before_mhz, o->rate_trial_mhz, o->rate_expect_mhz, o->rate_after_mhz, o->fifo_before, o->fifo_after, o->errcnt_before, o->errcnt_after, o->mismatch);
	RDNA4LOG("mode-trial: underflow HUBP %#x/%#x/%#x OPTC %#x/%#x/%#x (before/settle/after), max status %u timeout %u, %u samples, %u clears, first at %u ms",
	       o->ext.uf_hubp_before, o->ext.uf_hubp_settle, o->ext.uf_hubp_after, o->ext.uf_optc_before, o->ext.uf_optc_settle, o->ext.uf_optc_after, o->ext.uf_hubp_max, o->ext.uf_timeout_max, o->ext.uf_samples, o->ext.uf_clears, o->ext.uf_first_ms);
	RDNA4LOG("mode-trial: resync runs %u attempts %u rc %u/%u blank %u us unblank %u us DIO growth %u restore rc %u / %u; lock rc %u wait %u us pending %#x vert max %u db %#x/%#x/%#x drr %u/%u/%u",
	       o->ext.rs_runs, o->ext.rs_attempts, o->ext.rs_rc_first, o->ext.rs_rc_last, o->ext.rs_blank_us, o->ext.rs_unblank_us, o->ext.rs_dio_errs, o->ext.rs_restore_rc, o->ext.rs_restore_dio_errs,
	       o->ext.lk_rc, o->ext.lk_wait_us, o->ext.lk_pending_seen, o->ext.lk_vert_max, o->ext.lk_db_before, o->ext.lk_db_during, o->ext.lk_db_after, o->ext.lk_drr_before, o->ext.lk_drr_during, o->ext.lk_drr_after);
	if (o->row == RDNA4N_MODE_ROW_120 || o->ext.hold_state != 0u || o->ext.hold_pre != 0u)
		RDNA4LOG("mode-trial: clock hold %s: raise rc %u (pre %u) in %u ms, DFS %u/%u kHz; release rc %u in %u ms, DFS %u/%u kHz; %u DAL msgs; sampled %u (lost %u), lowest %u/%u kHz; flags %#x",
		       rdna4dal::hold_state_name(o->ext.hold_state), o->ext.hold_rc, o->ext.hold_pre, o->ext.hold_raise_ms, o->ext.hold_khz_raised[0], o->ext.hold_khz_raised[1], o->ext.hold_rel_rc, o->ext.hold_release_ms,
		       o->ext.hold_khz_released[0], o->ext.hold_khz_released[1], o->ext.hold_msgs, o->ext.hold_samples, o->ext.hold_lost, o->ext.hold_dfs_min[0], o->ext.hold_dfs_min[1], o->ext.hold_flags);
	return rdna4scan::kOk;
}

bool modeTrialBusy() { return __atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u; }

// ---- 0.0.609: the HELD mode (row 120): the launcher, the runner thread, the release and the session hook -----------------------------------------------------------------------------------------
// The whole trial runs on ONE kernel thread (mt_hold_runner) with a HoldCfg; the selector's caller only starts it and waits for the entry snapshot (or the end, when the trial was denied or failed). Nothing here
// takes a lock but through mt_lock / mt_unlock (the copies of the result buffers); the waits are IOSleep with nothing held.
static rdna4mt::HoldCfg gMtHoldCfg { 0u, 0u };
static void mt_hold_runner(void *, wait_result_t) {
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		rdna4mt::hold_runner_done(gMt);
		RDNA4LOG("mode-hold: runner could not acquire a display transaction");
		__atomic_fetch_sub(&gMtHoldAlive, 1u, __ATOMIC_RELEASE);
		return;
	}
	const rdna4mt::HoldCfg cfg = gMtHoldCfg;
	rdna4mt::run_trial(kMtHw, gMt, mt_timing(), RDNA4N_MODE_ROW_120, cfg.maxMs, 0u, &gMt.heldFinal, &cfg);
	RDNA4LOG("mode-hold: ENDED verdict %u (end %u, flags %#x, hold_flags %#x): held %u ms, %u register(s) still differ, latched %u", gMt.heldFinal.verdict, gMt.heldFinal.hold_end, gMt.heldFinal.flags, gMt.heldFinal.hold_flags,
	       gMt.heldFinal.dwell_done_ms, gMt.heldFinal.mismatch, gMt.heldFinal.latched);
	rdna4mt::hold_runner_done(gMt);                                   // the result buffer is final; the hold slot is free
	__atomic_fetch_sub(&gMtHoldAlive, 1u, __ATOMIC_RELEASE);        // the thread touches nothing of ours after this
}
static void mt_result_denied(struct rdna4n_mode_result *o, uint32_t why) {
	rdna4mt::detail::zero(o);
	o->row = RDNA4N_MODE_ROW_120;
	rdna4mt::detail::deny(kMtHw, gMt, o, why);
}
static void mt_copy_result(struct rdna4n_mode_result *o, const struct rdna4n_mode_result &src) { mt_lock(nullptr); *o = src; mt_unlock(nullptr); }
uint32_t modeHold(uint32_t sess, uint64_t maxMs, uint64_t flags, struct rdna4n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return rdna4scan::kNoResources;
	const uint64_t want = maxMs == 0ull ? (uint64_t)RDNA4N_MODE_DEFAULT_HOLD_MS : maxMs;
	const rdna4mt::HoldCfg cfg { want > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)want, flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags };
	if (!rdna4mt::hold_cfg_ok(cfg)) { mt_result_denied(o, RDNA4N_MODE_D_BAD_DWELL); return rdna4scan::kOk; }
	if (!rdna4mt::hold_launch_claim(gMt)) { mt_result_denied(o, RDNA4N_MODE_D_BUSY); return rdna4scan::kOk; }   // one hold at a time, from the claim until its runner is done
	rdna4mt::hold_launch_prepare(gMt, sess, cfg);
	gMtHoldCfg = cfg;
	__atomic_fetch_add(&gMtHoldAlive, 1u, __ATOMIC_ACQ_REL);
	thread_t th = nullptr;
	const kern_return_t kr = kernel_thread_start(&mt_hold_runner, nullptr, &th);
	if (kr != KERN_SUCCESS) {
		__atomic_fetch_sub(&gMtHoldAlive, 1u, __ATOMIC_ACQ_REL);
		rdna4mt::hold_runner_done(gMt);
		RDNA4LOG("mode-hold: the hold thread did not start (kr %#x)", kr);
		mt_result_denied(o, RDNA4N_MODE_D_WATCHDOG);
		return rdna4scan::kOk;
	}
	thread_deallocate(th);
	RDNA4LOG("mode-hold: STARTED by session %u: max %u ms, flags %#x (HANDOFF %d)", sess, cfg.maxMs, cfg.flags, (cfg.flags & RDNA4N_HOLD_F_HANDOFF) != 0u ? 1 : 0);
	// Wait (bounded, ~90 s: the pre-hold phases take ~12 s) for the hold to be UP or for the trial to have ended without one.
	for (uint32_t i = 0; i < 9000u; i++) {
		if (rdna4mt::ld(gMt.held) != 0u) { mt_copy_result(o, gMt.heldEntry); return rdna4scan::kOk; }
		if (rdna4mt::ld(gMt.runnerDone) != 0u) { mt_copy_result(o, gMt.heldFinal); return rdna4scan::kOk; }
		/*nolock*/ IOSleep(10);
	}
	RDNA4LOG("mode-hold: the hold neither entered nor ended in 90 s (it continues on its own; the watchdog bounds it)");
	return rdna4scan::kBusy;
}
// ModeRelease: end a running hold (unless QUERY), wait for the whole end sequence, hand back the FINAL result; DENIED (not held) when no hold ever ran this boot.
uint32_t modeRelease(uint64_t flags, struct rdna4n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return rdna4scan::kNoResources;
	if ((flags & ~(uint64_t)RDNA4N_REL_F_MASK) != 0ull) return rdna4scan::kBadArg;
	const bool query = (flags & RDNA4N_REL_F_QUERY) != 0ull;
	if (rdna4mt::ld(gMt.held) != 0u && query) { mt_copy_result(o, gMt.heldEntry); return rdna4scan::kOk; }
	if (rdna4mt::ld(gMt.launching) == 0u) {                            // no hold is running: the last one's final result, if there was one
		if (gMt.heldFinal.verdict == 0u) { mt_result_denied(o, RDNA4N_MODE_D_NOT_HELD); return rdna4scan::kOk; }
		mt_copy_result(o, gMt.heldFinal);
		return rdna4scan::kOk;
	}
	if (query) { mt_result_denied(o, RDNA4N_MODE_D_NOT_HELD); return rdna4scan::kOk; }   // a hold is starting but is not up yet
	if (rdna4mt::hold_request_release(gMt)) RDNA4LOG("mode-hold: RELEASE requested");
	for (uint32_t i = 0; i < 4000u; i++) {                           // the end sequence is ~10 s at the worst
		if (rdna4mt::ld(gMt.runnerDone) != 0u && rdna4mt::ld(gMt.launching) == 0u) { mt_copy_result(o, gMt.heldFinal); return rdna4scan::kOk; }
		/*nolock*/ IOSleep(10);
	}
	return rdna4scan::kBusy;
}
// The native client's close (n1c_close, AFTER its scanout plane was put back, under the client lock): atomic words only.
void modeHoldSessionClosed(uint32_t sess) { rdna4mt::hold_session_closed(kMtHw, gMt, sess); }
bool modeHoldActive() { return rdna4mt::ld(gMt.launching) != 0u || rdna4mt::ld(gMt.held) != 0u; }

// Kext stop: end a running trial (restore + ABORT), put the golden set back, wait (bounded) for the watchdog threads. Called first thing in scanShutdown, before the registers are unmapped.
static void mt_shutdown() {
	if (gMtLock == nullptr) return;
	rdna4mt::request_abort(kMtHw, gMt);
	rdna4mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: as in mt_emergency_restore
	const bool idle = rdna4mt::wait_idle(kMtHw, gMt, 20000u, 50u);
	if (gMt.everWrote) {
		uint32_t restored = 0; bool have = false;
		const uint32_t bad = rdna4mt::restore_from_golden(kMtHw, gMt, &restored, &have);
		// 0.0.607 (P4 caller: the kext stop): after a VERIFIED golden restore (registers equal and the back end healthy) a clock hold still HELD is released; otherwise it stays (STUCK).
		uint32_t prc = 0u;
		const bool healthy = rdna4mt::health_now(kMtHw, &prc);
		if (have) (void)rdna4mt::emergency_release(kMtHw, gMt, mt_timing(), idle, bad, healthy, 0u);
	}
	for (uint32_t i = 0; i < 2000u && (__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u || __atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u); i++) /*nolock*/ IOSleep(1);
	if (__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u) RDNA4LOG("mode-trial: kext stop: a watchdog thread is still alive after 2 s");
	if (__atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u) RDNA4LOG("mode-trial: kext stop: a hold thread is still alive after 2 s");
}

static void scan_shutdown_under_txn() {
	mt_shutdown();                           // build 0.0.605: the mode trial ends and the golden set is put back first (before the scan-lock early return: a trial needs no scan lock)
	scanXShutdown();                        // 0.0.661 (M6 Stage 1b): instance 2's A is restored (and its watchdog waited for) before the registers go away; inert when the latches are OFF
	if (gScan.lock != nullptr) {
		uint64_t r[2] = { 1, 0 };
		(void)scan_restore("kext stop", true, 0u, r);
		for (uint32_t i = 0; i < 2000u && __atomic_load_n(&gScan.watchdogAlive, __ATOMIC_ACQUIRE) != 0u; i++) /*nolock*/ IOSleep(1);
		if (__atomic_load_n(&gScan.watchdogAlive, __ATOMIC_ACQUIRE) != 0u) RDNA4LOG("rdna4scan: kext stop: a watchdog thread is still alive after 2 s");
	}
	wait_for_dcn_threads_quiescent();
}

// Kext stop: restore the console (idempotent), then use a final unbounded
// quiescence barrier before the caller unmaps the registers. GarnetDriver::stop
// already holds the drain-exclusive Stop lease: ride it instead of attempting
// a differently-kinded LocalDisplay lease.
void scanShutdown() {
	if (rdna4displaytxn::held()) {
		scan_shutdown_under_txn();
		return;
	}
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		RDNA4LOG("scanShutdown REFUSED: no display transaction");
		return;
	}
	scan_shutdown_under_txn();
}

}  // namespace rdna4dcn
