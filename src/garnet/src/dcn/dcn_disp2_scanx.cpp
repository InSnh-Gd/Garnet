#include "dcn_internal.hpp"

namespace rdna4dcn {

// =====================================================================================================================================================================================================
// build 0.0.631 (multi-monitor stage M4d; an internal design note "M4 design" / "M4b analysis"): verb 98 `disp2 timing|connect|off|status` - OTG1 -> ODM1 -> OPP1 (DPG1 pattern) -> DIG2 -> the
// DMUB-enabled PHY C. Behind boot-arg garnet-disp2=1 (OFF: RDNA4D2_OFF, nothing touched, not even a register read). Decisions, step lists and the instance guard: dcn/disp2.h; the sequence: dcn/disp2_flow.h
// (host test src/garnet/tests/native_disp2_test.cpp + native_disp2_plant.sh). Every write: the instance guard (rdna4d2_guard_write, address AND value) and then the DCN write allowlist, then ONE WREG32. One call at a time (gD2Busy).
// =====================================================================================================================================================================================================
namespace {
volatile uint32_t gD2Busy = 0;
uint32_t gD2Dpx[2][RDNA4D2_DPX_BUF];          // 0.0.634: the DP-plane reads before / after the op (single flight under gD2Busy); 0.0.635: 16 dwords (15 watch rows + the HUBP0 primary HIGH)
rdna4d2_plane gD2Pl[2];                       // 0.0.635 (M4c): the plane's state across ops (single flight under gD2Busy, except the glue's stage resets after it released gD2Busy); 0.0.655: PER INSTANCE - [0] the monitor A's (instance 1), [1] the monitor B's (instance 2): d2i()
rdna4scanx::ScanX gSX[3];                   // 0.0.661 (M6 Stage 1b) + 0.0.662 (Stage 2): the HDMI displays' scanout state, INDEXED BY INSTANCE NUMBER ([1] the monitor A's HUBP1 / OTG1, [2] the monitor B's HUBP2 / OTG2, [0] never used: the DP is instance-0 code); everything in it is inert unless the latches are ON and a native RDNA4N client Acquires; the lock of [i] is gSXLock[i] (dcn/scanx_flow.h). Never hold two of them.
IOLock *volatile gSXLock[3] = { nullptr, nullptr, nullptr };
uint32_t gD2Gate[RDNA4D2_GATES];              // 0.0.635: the op's final gate reads
uint32_t gD2Rec[2][RDNA4D2_REC_N];            // 0.0.638: the record page (op 8's recorded reads, the timed-out WAITs' register + last value); single flight under gD2Busy; 0.0.655: one per instance
uint32_t gD2W2[2][RDNA4D2_W2_BUF];            // 0.0.655: the monitor B watch of an MONA op (before / after); single flight under gD2Busy
uint64_t gD2Off[2][2];                      // 0.0.635: the VRAM (BAR0) byte offsets of A and B; 0.0.655: per instance
static inline uint32_t d2i(uint32_t inst) { return inst == RDNA4D2_INST_MONA ? 0u : 1u; }      // 0.0.655: the index of an instance's plane state / record page / offsets
rdna4d2_step gD2Steps[2][RDNA4D2_MAX_STEPS];    // the op's list and the rollback's: file-scope, single flight under gD2Busy (never on the kernel stack)
const char *d2_note_name(uint32_t w) { return w == rdna4d2::RDNA4D2_N_WRITE ? "wrote" : w == rdna4d2::RDNA4D2_N_REFUSED ? "REFUSED" : w == rdna4d2::RDNA4D2_N_WAIT_OK ? "wait ok" : w == rdna4d2::RDNA4D2_N_WAIT_TIMEOUT ? "WAIT TIMED OUT" : w == rdna4d2::RDNA4D2_N_REC ? "recorded (no gate)" : "delay (no symclk)"; }
struct D2KextEnv {
	uint32_t inst { RDNA4D2_INST_MONA };       // 0.0.633: which instance this call is for (its OWN guard judges every write)
	uint32_t rd(uint32_t abs) { return (abs >= gDcn.d.mmio_dwords) ? 0xFFFFFFFFu : gDcn.d.rreg(gDcn.d.cookie, abs); }
	bool wr(uint32_t abs, uint32_t v) {      // THE register write of this verb: the instance guard again, then the DCN write allowlist
		if (!dcn_display_txn_ok("disp2 register write")) return false;
		const char *why = nullptr;
		const uint32_t g = rdna4d2_guard_write_i(inst, abs, v, &why);
		if (g != RDNA4D2_G_OK) {
			RDNA4LOG("disp2 (instance %u): instance guard REFUSED write %#010x = %#010x (%s%s)", inst, abs, v, g == RDNA4D2_G_FORBIDDEN ? "forbidden: " : g == RDNA4D2_G_OTHER ? "the other instance: " : g == RDNA4D2_G_VALUE ? "a value this verb never writes there" : "not one of the verb's registers", why ? why : "");
			return false;
		}
		if (!dcn41_allow_write(&gDcn.allow, abs, v, "disp2")) {
			RDNA4LOG("disp2: the DCN allowlist REFUSED write %#010x = %#010x", abs, v);
			return false;
		}
		amdgpu::WREG32(*gDcn.dev, abs, v);
		return true;
	}
	void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }
	void sleep_ms(uint32_t ms) { /*nolock*/ IOSleep(ms); }   // no lock is held anywhere in disp2 (single flight is the atomic gD2Busy)
	void note(uint32_t i, const rdna4d2_step &s, uint32_t old, uint32_t v, uint32_t what) {
		if (s.kind == RDNA4D2_K_SET || s.kind == RDNA4D2_K_UPD || s.kind == RDNA4D2_K_COPY0)
			RDNA4LOG("disp2 step %u: %s %s %#06x %#010x -> %#010x (mask %#010x) %s", i, d2_note_name(what), s.reg, s.abs, old, v, s.mask, s.fn);
		else
			RDNA4LOG("disp2 step %u: %s %s %#06x (mask %#010x want %#010x) read %#010x after %u polls %s", i, d2_note_name(what), s.reg, s.abs, s.mask, s.val, old, v, s.fn);
	}
	rdna4d2_step *buf(unsigned k) { return gD2Steps[k & 1u]; }
	uint32_t *dpx_buf(unsigned k) { return gD2Dpx[k & 1u]; }
	// ---- 0.0.635 (M4c / M4d): the plane ops
	rdna4d2_plane *pl_of(uint32_t i) { return &gD2Pl[d2i(i)]; }      // 0.0.655: the plane state of an INSTANCE
	uint32_t *w2_buf(unsigned k) { return gD2W2[k & 1u]; }      // 0.0.655: the monitor B watch's two buffers
	bool pin() { return amdgpu::n1c_d2_pin(inst, gD2Pl[d2i(inst)].mc); }      // 0.0.652 (M5, op 14): the allocator pins exactly the pair the plane reports (the helper takes the native client lock itself; no display lock is held here: gD2Busy is an atomic flag); 0.0.655: of THIS instance
	uint32_t *gate_buf() { return gD2Gate; }
	uint32_t *rec_buf() { return gD2Rec[d2i(inst)]; }
	bool window(uint64_t *lo, uint64_t *hi) { *lo = gDcn.d.scanout_lo; *hi = gDcn.d.scanout_hi; return gDcn.d.scanout_hi != 0u; }
	bool desktop_acquired() { return __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u; }
	bool vfill(unsigned k, uint32_t byteOff, uint32_t pattern, uint32_t bytes) {      // bounds-checked HERE (bar0_memset_vram silently returns on an out-of-range request)
		const uint64_t off = k > 1u ? 0u : gD2Off[d2i(inst)][k];
		if (k > 1u || off == 0u || !gDcn.dev || (uint64_t)byteOff + bytes > rdna4d2_surf_get(inst)->buf_bytes || off + byteOff + bytes > gDcn.dev->bar0Size) return false;
		amdgpu::bar0_memset_vram(*gDcn.dev, off + byteOff, pattern, bytes);
		return true;
	}
	void vflush() { amdgpu::amdgpu_hdp_flush(*gDcn.dev); }
	bool vread(unsigned k, uint32_t byteOff, uint32_t *dw) { return k <= 1u && gD2Off[d2i(inst)][k] != 0u && rdna4_vram_read_mm(gD2Off[d2i(inst)][k] + byteOff, dw, 1u); }
};
}  // namespace


// The per-thread 2 s waits keep their bounded diagnostics, but stop may not
// unmap BAR0/BAR5 while any thread that can still reach those registers is
// alive. This final barrier has no timeout: a wedged counter keeps the mapping
// instead of allowing a post-unmap access. Every counted thread can leave
// after the shutdown restores without acquiring another display transaction.
void wait_for_dcn_threads_quiescent() {
	uint64_t ms = 0;
	uint32_t logs = 0;
	for (;;) {
		const uint32_t mtWd = __atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE);
		const uint32_t mtHold = __atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE);
		const uint32_t scan = __atomic_load_n(&gScan.watchdogAlive, __ATOMIC_ACQUIRE);
		const uint32_t sx1 = __atomic_load_n(&gSX[1].watchdogAlive, __ATOMIC_ACQUIRE);
		const uint32_t sx2 = __atomic_load_n(&gSX[2].watchdogAlive, __ATOMIC_ACQUIRE);
		if (mtWd == 0u && mtHold == 0u && scan == 0u && sx1 == 0u && sx2 == 0u) return;
		if ((ms == 0ull || ms % 1000ull == 0ull) && logs++ < 16u)
			RDNA4LOG("dcn: kext stop waiting for threads before unmap at %llu ms (mode wd %u, hold %u, scan %u, scanx %u/%u)",
			       (unsigned long long)ms, mtWd, mtHold, scan, sx1, sx2);
		IOSleep(1);
		ms++;
	}
}

// =====================================================================================================================================================================================================
// build 0.0.652 (multi-monitor stage M5; an internal design note "M5 build spec: GarnetFramebuffer for the monitor B"): what `fbpublish 2` (GarnetDisplayNub.cpp) needs from the display layer.
// disp2Held: the HELD plane (stage HELD, or the pair pinned): ONE atomic load of each word; no lock, no register. fbView: the software facts (no register read). fbLive: the plane's live gates (READS ONLY), re-judged by
// rdna4d2_hold_verdict. fbEdid: the monitor B's EDID blocks 0 and 1 over DDC line 3 through the SAME engine sequence as `accel ddcread` (rdna4dr::ddc_read: arbitration verdict first, bounded poll, always released; its only register
// writes are the DC_I2C engine's, through the DCN allowlist, exactly as ddcread). None of them writes a plane or pipe register.
// =====================================================================================================================================================================================================
bool disp2HeldInst(uint32_t inst) {      // 0.0.655: HELD is per instance
	return __atomic_load_n(&gD2Pl[d2i(inst)].stage, __ATOMIC_ACQUIRE) == RDNA4D2_PL_HELD || amdgpu::n1c_d2_pinned(inst);
}
bool disp2Held() {      // the MONB's (the M5 framebuffer and the monitor B's DMUB templates): unchanged
	return disp2HeldInst(RDNA4D2_INST_MONB);
}
void fbView(rdna4dcn::FbView *v, uint32_t inst) {      // 0.0.658: of an INSTANCE (2 = the monitor B: exactly the 0.0.657 values; 1 = the monitor A)
	const uint32_t pi = d2i(inst);
	v->held = __atomic_load_n(&gD2Pl[pi].stage, __ATOMIC_ACQUIRE) == RDNA4D2_PL_HELD;
	v->pinned = amdgpu::n1c_d2_pinned(inst);
	v->devOk = gDcn.dev != nullptr && __atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) != 0u;
	v->mc[0] = gD2Pl[pi].mc[0]; v->mc[1] = gD2Pl[pi].mc[1]; v->offA = gD2Off[pi][0];
	v->bar0Phys = gDcn.dev ? gDcn.dev->bar0Phys : 0u; v->bar0Size = gDcn.dev ? gDcn.dev->bar0Size : 0u;
	v->cur = gD2Pl[pi].cur;
	v->bufBytes = rdna4d2_surf_get(inst)->buf_bytes;
}
uint32_t fbLive(uint32_t inst, uint32_t *holdBad, uint32_t *gates) {
	*holdBad = 0xFFFFFFFFu;
	if (inst != RDNA4D2_INST_MONB && inst != RDNA4D2_INST_MONA) return RDNA4D2_BAD_ARG;      // 0.0.658: the two instances only (the caller maps the verb's argument through rdna4fb::inst_of_arg)
	if (!gDcn.dev || !__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return RDNA4D2_NO_DEVICE;
	if (__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL) != 0u) return RDNA4D2_BUSY;
	D2KextEnv env;
	env.inst = inst;
	rdna4d2_out o;
	for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
	uint32_t *const g = gD2Gate;
	rdna4d2_plane *const pl = &gD2Pl[d2i(inst)];
	rdna4d2::plane_gates(env, inst, pl, g, o);                   // READS ONLY
	*holdBad = rdna4scanx::hold_verdict_gpu(rdna4d2_hold_verdict(g, pl->cur, pl->mc[0]), scanXGpuHeld(inst));      // 0.0.661 (M6 Stage 1b): while instance 2 is acquired the plane is GPU-held: EARLIEST != A is not a latch failure; 0.0.662: the same for instance 1 (scanXGpuHeld answers 0 for any other instance and for a latch OFF)
	for (uint32_t i = 0; i < RDNA4D2_GATES; i++) gates[i] = g[i];
	__atomic_store_n(&gD2Busy, 0u, __ATOMIC_RELEASE);
	return RDNA4D2_OK;
}
uint32_t fbEdid(uint32_t ddcLine, uint8_t out[256]) {
	for (unsigned i = 0; i < 256u; i++) out[i] = 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return RDNA4DR_BUSY;
	if (ddcLine != GARNET_DISP_DDC_LINE && ddcLine != GARNET_DISPA_DDC_LINE) return RDNA4DR_NO_DEVICE;      // 0.0.658: the monitor B's line 3 or the monitor A's line 2 (the table, GarnetDisplayOps.h)
	if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) return RDNA4DR_NO_DEVICE;
	return dcn_ddc_edid_read(ddcLine, out);
}

// The dispatcher asks this BEFORE bind(): with garnet-disp2 absent (the default) verb 98 must not even bind the display layer.
bool disp2LatchedOn() { return d2_latched_on(); }

uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {
	if (!out || outCount < 13u) return RDNA4D2_BAD_ARG;
	for (unsigned i = 0; i < 13u; i++) out[i] = 0u;
	if (!d2_latched_on()) { out[0] = RDNA4D2_OFF; return RDNA4D2_OFF; }                                 // OFF: nothing touched, not even a read
	if (!rdna4d2_arg_ok(arg)) { out[0] = RDNA4D2_BAD_ARG; return RDNA4D2_BAD_ARG; }
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { out[0] = RDNA4D2_BUSY; return RDNA4D2_BUSY; }
	if (!gDcn.dev || !__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) { out[0] = RDNA4D2_NO_DEVICE; return RDNA4D2_NO_DEVICE; }
	if (gDcn.seg[1] != RDNA4D2_SEG1 || gDcn.seg[2] != RDNA4D2_SEG2 || gDcn.seg[3] != RDNA4D2_SEG3) { out[0] = RDNA4D2_BASES; return RDNA4D2_BASES; }   // 0.0.634: BASE_IDX 3 too (the DP-plane reads MPC_OUT0 / MPCC0 are absolute 0x9000 + offset)
	const uint32_t op = rdna4d2_arg_op(arg), inst = rdna4d2_arg_inst(arg);   // 0.0.633: bits 8..15 name the instance (0 / 1 = the monitor A as before, 2 = the monitor B); rdna4d2_arg_ok already refused anything else (0.0.655: ops 8..13 for either instance, op 14 for the monitor B alone)
	const uint32_t pi = d2i(inst);       // 0.0.655: the instance's plane state / record page / buffer offsets
	// 0.0.635 (M4c): the plane's two buffers are ALLOCATED BEFORE gD2Busy (lock order: the native client lock, which the helper takes, then the display lock) and FREED only after gD2Busy is released again. Nothing is written yet.
	// 0.0.652 (M5): a HELD plane (disp2 fbhold 2 ran: stage HELD, the pair pinned) refuses every op that could move, free or re-time it (rdna4d2_held_refuses; the monitor A's ops, the read-only status pages, crc and planerec are not affected) - BEFORE
	// anything is allocated, read or written: op 8 must not even reach the allocator.
	if (rdna4d2_held_refuses(op, inst) && disp2HeldInst(inst)) {      // 0.0.655: per instance - the monitor B's HELD plane never refuses the monitor A's ops and the reverse
		out[0] = RDNA4D2_HELD;
		RDNA4LOG("disp2 %s (instance %u): REFUSED, the plane is HELD for this boot (disp2 fbhold): %s", rdna4d2_op_name(op), inst, rdna4d2_status_name(RDNA4D2_HELD));
		return RDNA4D2_HELD;
	}
	uint64_t pmc[2] = { 0u, 0u }, poff[2] = { 0u, 0u };
	bool allocated = false, freeNow = false;
	if (op == RDNA4D2_OP_PLANE) {
		if (ctx == nullptr || amdgpu::n1c_d2_alloc(*ctx, inst, rdna4d2_surf_get(inst)->buf_bytes, pmc, poff) != kIOReturnSuccess) { out[0] = RDNA4D2_NO_BUFFER; return RDNA4D2_NO_BUFFER; }
		allocated = true;
	}
	if (__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL) != 0u) { if (allocated) amdgpu::n1c_d2_free(inst); out[0] = RDNA4D2_BUSY; return RDNA4D2_BUSY; }
	if (allocated) {
		if (gD2Pl[pi].stage != RDNA4D2_PL_NONE) { __atomic_store_n(&gD2Busy, 0u, __ATOMIC_RELEASE); amdgpu::n1c_d2_free(inst); out[0] = RDNA4D2_PLANE_STATE; return RDNA4D2_PLANE_STATE; }
		gD2Pl[pi].stage = RDNA4D2_PL_ALLOC; gD2Pl[pi].cur = 0u; gD2Pl[pi].odm_sticky = 0u; gD2Pl[pi].dp_bytes = 0u; gD2Pl[pi].mc[0] = pmc[0]; gD2Pl[pi].mc[1] = pmc[1]; gD2Off[pi][0] = poff[0]; gD2Off[pi][1] = poff[1];
	}
	D2KextEnv env;
	env.inst = inst;
	uint32_t st;
	if (op == RDNA4D2_OP_STATUS3) {         // 0.0.633: the third status page (reads only): the DIG's TMDS / FIFO / CRC registers, DIG1's two, SYMCLKB and the instance's SYMCLK
		st = rdna4d2::status3(env, out, inst);
		RDNA4LOG("disp2 status3 (instance %u): CLOCK_PATTERN %#010x TEST_PATTERN %#010x FIFO_CTRL1 %#010x HDMI_STATUS %#010x TMDS_CNTL %#010x TMDS_CONTROL_CHAR %#010x TMDS_CTL_BITS %#010x DCBALANCER %#010x",
		       inst, (uint32_t)out[1], (uint32_t)(out[1] >> 32), (uint32_t)out[2], (uint32_t)(out[2] >> 32), (uint32_t)out[3], (uint32_t)(out[3] >> 32), (uint32_t)out[4], (uint32_t)(out[4] >> 32));
		RDNA4LOG("disp2 status3 (instance %u): OUTPUT_CRC_CNTL %#010x OUTPUT_CRC_RESULT %#010x ; DIG1 FIFO_CTRL0 %#010x TMDS_CTL_BITS %#010x ; SYMCLKB %#010x SYMCLK<inst> %#010x",
		       inst, (uint32_t)out[5], (uint32_t)(out[5] >> 32), (uint32_t)out[6], (uint32_t)(out[6] >> 32), (uint32_t)out[7], (uint32_t)(out[7] >> 32));
	} else if (op == RDNA4D2_OP_STATUS2) {         // 0.0.632: the second status page (reads only)
		st = rdna4d2::status2(env, out, inst);
		RDNA4LOG("disp2 status2 (instance %u): SYMCLK<inst>_CLOCK_ENABLE %#010x ; OTG<inst>_PIXEL_RATE_CNTL %#010x ; OTG<inst>_V_TOTAL_CONTROL %#010x ; FMT CONTROL %#010x BIT_DEPTH %#010x DYN_EXP %#010x CLAMP_CNTL %#010x CLAMP R/G/B %#010x %#010x %#010x ; HUBP DCHUBP_CNTL %#010x HUBP_CLK_CNTL %#010x",
		       inst, (uint32_t)out[1], (uint32_t)(out[1] >> 32), (uint32_t)out[2], (uint32_t)(out[2] >> 32), (uint32_t)out[3], (uint32_t)(out[3] >> 32), (uint32_t)out[4], (uint32_t)(out[4] >> 32),
		       (uint32_t)out[5], (uint32_t)(out[5] >> 32), (uint32_t)out[6], (uint32_t)(out[6] >> 32));
	} else if (op == RDNA4D2_OP_STATUS) {
		st = rdna4d2::status(env, out, inst);
		if (st == RDNA4D2_OK && (gD2Pl[pi].stage == RDNA4D2_PL_HELD || amdgpu::n1c_d2_pinned(inst))) out[0] |= 1ull << RDNA4D2_STAT_HELD_BIT;      // 0.0.652 (M5): `disp2 status 2` reports the HELD plane (bit 52 of out[0]); 0.0.655: of either instance
		RDNA4LOG("disp2 status (instance %u): OTG<inst>_CONTROL %#010x frames %u -> %u ; DPG_CONTROL %#010x ; DIG FE_CNTL %#010x FE_CLK %#010x FE_EN %#x FIFO %#010x ; BE_CNTL %#010x BE_CLK %#010x BE_EN %#x ; mapper %#x ; PHYPLL<inst> %#x ; OTG0 frames %u -> %u",
		       inst, (uint32_t)out[1], (uint32_t)out[2], (uint32_t)(out[2] >> 32), (uint32_t)(out[5] >> 32), (uint32_t)(out[6] >> 32), (uint32_t)out[7], (uint32_t)(out[7] >> 32), (uint32_t)out[8], (uint32_t)(out[8] >> 32),
		       (uint32_t)out[9], (uint32_t)(out[9] >> 32), (uint32_t)out[10], (uint32_t)out[11], (uint32_t)(out[11] >> 32), (uint32_t)out[12]);
	} else if (op == RDNA4D2_OP_PLANEREC) {         // 0.0.638: the record page of the last plane op (reads NO register); 0.0.655: of the instance
		st = rdna4d2::planerec(env, out);
		RDNA4LOG("disp2 planerec (instance %u): valid %#x ; sample A (~1 ms after the HUBP clock enable) HUBP<inst>_HUBP_CLK_CNTL %#010x DCCG_GATE_DISABLE_CNTL6 %#010x DCCG_GATE_DISABLE_CNTL %#010x DOMAIN<inst>_PG_STATUS %#010x ; sample B (after the 2-frame wait) %#010x %#010x %#010x %#010x",
		       inst, gD2Rec[pi][RDNA4D2_RC_VALID], gD2Rec[pi][0], gD2Rec[pi][1], gD2Rec[pi][2], gD2Rec[pi][3], gD2Rec[pi][4], gD2Rec[pi][5], gD2Rec[pi][6], gD2Rec[pi][7]);
	} else if ((op >= RDNA4D2_OP_PLANE && op <= RDNA4D2_OP_PLANEOFF) || op == RDNA4D2_OP_FBHOLD) {         // 0.0.635 (M4c / M4d); 0.0.652: op 14 `fbhold` (the monitor B's alone, rdna4d2_arg_ok); 0.0.655: ops 8..12 for either instance
		const rdna4d2_out o = rdna4d2::run_plane(env, op, rdna4d2_arg_buf(arg), inst);
		rdna4d2_pack_plane(out, &o, gD2Gate);
		st = o.status;
		freeNow = o.free_bufs != 0u;
		RDNA4LOG("disp2 %s (instance %u): status %u (%s); steps written %u, failing step %u, wait timeouts %u, rolled back %u, buffers %s; plane stage %u (cur %u); DP changed %#x, DP-plane mask %#x%s; precheck %#x = %#010x; OTG0 frames %u -> %u, OTG2 frames %u -> %u",
		       rdna4d2_op_name(op), inst, st, rdna4d2_status_name(st), o.done, o.fail, o.timeouts, o.auto_off, o.free_bufs ? "RELEASED to the allocator" : (gD2Pl[pi].stage == RDNA4D2_PL_LEAKED ? "LEAKED (HUBP<inst> may still read them)" : "kept"),
		       gD2Pl[pi].stage, gD2Pl[pi].cur, o.dp_changed, o.dpx_changed, o.dpx_changed != 0u ? " *** DP DISTURBED ***" : "", o.pre_abs, o.pre_val, o.f0a, o.f0b, o.f1a, o.f1b);
		RDNA4LOG("disp2 %s gates: HUBP<inst>_DCHUBP_CNTL %#010x (underflow %#x SEG_ALLOC_ERR %u TIMEOUT %#x) HUBP<inst>_HUBP_CLK_CNTL %#010x ODM<inst>_OPTC_INPUT_GLOBAL_CONTROL %#010x (bits 10/13 %#x, stale before %#x) DCN_VM_FAULT_STATUS %#010x DET<inst>_CTRL %#010x (current %u)",
		       rdna4d2_op_name(op), gD2Gate[RDNA4D2_GT_HUBP_CNTL], (gD2Gate[RDNA4D2_GT_HUBP_CNTL] & RDNA4D2_HUBP_UNDERFLOW_MASK) >> 28, (gD2Gate[RDNA4D2_GT_HUBP_CNTL] & RDNA4D2_HUBP_SEG_ALLOC_ERR_MASK) ? 1u : 0u, (gD2Gate[RDNA4D2_GT_HUBP_CNTL] & RDNA4D2_HUBP_TIMEOUT_MASK) >> 20,
		       gD2Gate[RDNA4D2_GT_HUBP_CLK], gD2Gate[RDNA4D2_GT_ODM2], gD2Gate[RDNA4D2_GT_ODM2] & RDNA4D2_ODM_UNDERFLOW_MASK, gD2Pl[pi].odm_sticky, gD2Gate[RDNA4D2_GT_VMFAULT], gD2Gate[RDNA4D2_GT_DET2], (gD2Gate[RDNA4D2_GT_DET2] & RDNA4D2_DET_CUR_MASK) >> 8);
		RDNA4LOG("disp2 %s gates: EARLIEST_INUSE %#x:%#010x (A %#llx, B %#llx) FLIP_CONTROL %#010x (pending %u) MPCC<inst>_STATUS %#x MPC_OUT<inst>_MUX %#010x DIG<inst+1>_FIFO_CTRL0 %#010x ; CRC RG %#010x B %#010x ; dp_surface_bytes %llu",
		       rdna4d2_op_name(op), gD2Gate[RDNA4D2_GT_EARLY_HI] & 0xFFFFu, gD2Gate[RDNA4D2_GT_EARLY_LO], (unsigned long long)gD2Pl[pi].mc[0], (unsigned long long)gD2Pl[pi].mc[1], gD2Gate[RDNA4D2_GT_FLIP_CTL], (gD2Gate[RDNA4D2_GT_FLIP_CTL] & RDNA4D2_FLIP_PENDING_MASK) ? 1u : 0u,
		       gD2Gate[RDNA4D2_GT_MPCC_STATUS], gD2Gate[RDNA4D2_GT_MPC_MUX], gD2Gate[RDNA4D2_GT_FIFO], gD2Gate[RDNA4D2_GT_CRC_RG], gD2Gate[RDNA4D2_GT_CRC_B], (unsigned long long)gD2Pl[pi].dp_bytes);
		RDNA4LOG("disp2 %s recorded reads (no gate; valid %#x): after the HUBP clock enable HUBP<inst>_HUBP_CLK_CNTL %#010x DCCG_GATE_DISABLE_CNTL6 %#010x DCCG_GATE_DISABLE_CNTL %#010x DOMAIN<inst>_PG_STATUS %#010x ; after the 2-frame wait %#010x %#010x %#010x %#010x",
		       rdna4d2_op_name(op), gD2Rec[pi][RDNA4D2_RC_VALID], gD2Rec[pi][0], gD2Rec[pi][1], gD2Rec[pi][2], gD2Rec[pi][3], gD2Rec[pi][4], gD2Rec[pi][5], gD2Rec[pi][6], gD2Rec[pi][7]);
		RDNA4LOG("disp2 %s first timed-out WAIT %s %#06x last value %#010x ; rollback's first timed-out WAIT %s %#06x last value %#010x",
		       rdna4d2_op_name(op), rdna4d2_wait_reg_name(gD2Rec[pi][RDNA4D2_RC_WAIT_ABS]), gD2Rec[pi][RDNA4D2_RC_WAIT_ABS], gD2Rec[pi][RDNA4D2_RC_WAIT_VAL], rdna4d2_wait_reg_name(gD2Rec[pi][RDNA4D2_RC_RB_ABS]), gD2Rec[pi][RDNA4D2_RC_RB_ABS], gD2Rec[pi][RDNA4D2_RC_RB_VAL]);
		RDNA4LOG("disp2 %s DP plane watch%s (mask %#x): MPC_OUT0_MUX %#010x -> %#010x ; HUBP0_DCHUBP_CNTL %#010x -> %#010x ; HUBP0 primary %#x:%#010x -> %#x:%#010x ; ODM0 %#x -> %#x ; DPP_TOP0 %#010x -> %#010x ; MPCC0_LOCK_SEL %#x -> %#x ; OTG2_GLOBAL_CONTROL2 %#010x -> %#010x",
		       rdna4d2_op_name(op), o.dpx_changed != 0u ? " *** DP DISTURBED ***" : "", o.dpx_changed, gD2Dpx[0][0], gD2Dpx[1][0], gD2Dpx[0][4], gD2Dpx[1][4], gD2Dpx[0][15], gD2Dpx[0][14], gD2Dpx[1][15], gD2Dpx[1][14],
		       gD2Dpx[0][10], gD2Dpx[1][10], gD2Dpx[0][11], gD2Dpx[1][11], gD2Dpx[0][12], gD2Dpx[1][12], gD2Dpx[0][13], gD2Dpx[1][13]);
		if (inst == RDNA4D2_INST_MONA) RDNA4LOG(RDNA4D2_W2_LINE1_FMT, RDNA4D2_W2_LINE1_ARGS(rdna4d2_op_name(op), o.w2_changed != 0u ? " *** MONB DISTURBED ***" : "", o.w2_changed, gD2W2[0], gD2W2[1]));
		if (inst == RDNA4D2_INST_MONA) RDNA4LOG(RDNA4D2_W2_LINE2_FMT, RDNA4D2_W2_LINE2_ARGS(rdna4d2_op_name(op), gD2W2[0], gD2W2[1]));
	} else {
		const rdna4d2_out o = rdna4d2::run(env, op, inst);
		rdna4d2_pack(out, &o);
		st = o.status;
		RDNA4LOG("disp2 %s (instance %u): status %u (%s); steps %u/%u, failing step %u, wait timeouts %u, auto-off %u, DP changed %#x; FIFO reset: %s; OTG0 frames %u -> %u, OTG<inst> frames %u -> %u",
		       rdna4d2_op_name(op), inst, o.status, rdna4d2_status_name(o.status), o.done, o.total, o.fail, o.timeouts, o.auto_off, o.dp_changed, rdna4d2_symclk_report(&o), o.f0a, o.f0b, o.f1a, o.f1b);
		RDNA4LOG("disp2 %s (instance %u) after: SYMCLKB %#010x -> %#010x; precheck %#x = %#010x; OTG<inst>_CONTROL %#010x DIG FE_EN %#x BE_CNTL %#010x",
		       rdna4d2_op_name(op), inst, o.symclkb_a, o.symclkb_b, o.pre_abs, o.pre_val, o.otg1_ctl, o.fe_en, o.be_cntl);
		RDNA4LOG("disp2 %s (instance %u) DP plane watch%s (changed mask %#x): MPC_OUT0_MUX %#010x -> %#010x ; MPCC0 TOP %#x -> %#x BOT %#x -> %#x OPP %#x -> %#x ; HUBP0_DCHUBP_CNTL %#010x -> %#010x (underflow %#x -> %#x)",
		       rdna4d2_op_name(op), inst, o.dpx_changed != 0u ? " *** DP DISTURBED ***" : "", o.dpx_changed, gD2Dpx[0][0], gD2Dpx[1][0], gD2Dpx[0][1], gD2Dpx[1][1], gD2Dpx[0][2], gD2Dpx[1][2],
		       gD2Dpx[0][3], gD2Dpx[1][3], gD2Dpx[0][4], gD2Dpx[1][4], (gD2Dpx[0][4] & RDNA4D2_HUBP_UNDERFLOW_MASK) >> 28, (gD2Dpx[1][4] & RDNA4D2_HUBP_UNDERFLOW_MASK) >> 28);
		RDNA4LOG("disp2 %s (instance %u) DP plane watch: DPPCLK0_DTO_PARAM %#010x -> %#010x ; DPPCLK_CTRL bit0 %#x -> %#x ; DET0_CTRL %#010x -> %#010x ; COMPBUF_CTRL %#010x -> %#010x",
		       rdna4d2_op_name(op), inst, gD2Dpx[0][5], gD2Dpx[1][5], gD2Dpx[0][6], gD2Dpx[1][6], gD2Dpx[0][7], gD2Dpx[1][7], gD2Dpx[0][8], gD2Dpx[1][8]);
		RDNA4LOG("disp2 %s (instance %u) DP plane watch (15 rows): HUBP0_DCHUBP_CNTL strict %#010x -> %#010x ; ODM0 underflow %#x -> %#x ; DPP_TOP0 %#010x -> %#010x ; MPCC0_LOCK_SEL %#x -> %#x ; OTG2_GLOBAL_CONTROL2 %#010x -> %#010x ; HUBP0 primary %#x:%#010x -> %#x:%#010x",
		       rdna4d2_op_name(op), inst, gD2Dpx[0][9], gD2Dpx[1][9], gD2Dpx[0][10], gD2Dpx[1][10], gD2Dpx[0][11], gD2Dpx[1][11], gD2Dpx[0][12], gD2Dpx[1][12], gD2Dpx[0][13], gD2Dpx[1][13], gD2Dpx[0][15], gD2Dpx[0][14], gD2Dpx[1][15], gD2Dpx[1][14]);
		if (inst == RDNA4D2_INST_MONA) RDNA4LOG(RDNA4D2_W2_LINE1_FMT, RDNA4D2_W2_LINE1_ARGS(rdna4d2_op_name(op), o.w2_changed != 0u ? " *** MONB DISTURBED ***" : "", o.w2_changed, gD2W2[0], gD2W2[1]));
		if (inst == RDNA4D2_INST_MONA) RDNA4LOG(RDNA4D2_W2_LINE2_FMT, RDNA4D2_W2_LINE2_ARGS(rdna4d2_op_name(op), gD2W2[0], gD2W2[1]));
	}
	if ((op == RDNA4D2_OP_STATUS || op == RDNA4D2_OP_CRC) && (inst == RDNA4D2_INST_MONB || inst == RDNA4D2_INST_MONA)) {      // 0.0.661 (M6 Stage 1b, contract item 11) + 0.0.662 (Stage 2, item 7): GPU-held is reported as such, never as a latch failure
		const uint32_t gh = scanXGpuHeld(inst);
		if (gh != 0u) RDNA4LOG("disp2 %s (instance %u): GPU-HELD (this instance's scanout is acquired): %s - EARLIEST_INUSE differing from A is NOT a latch failure", rdna4d2_op_name(op), inst, gh <= 3u ? "the hardware fetches a client slot" : "EARLIEST_INUSE is not a registered slot (A, B or unknown)");
	}
	__atomic_store_n(&gD2Busy, 0u, __ATOMIC_RELEASE);
	if (freeNow && amdgpu::n1c_d2_pinned(inst)) {      // 0.0.652 (M5): defensive - a pinned pair is never freed and its stage never reset (no op that frees can run on a HELD plane: the refusal above)
		RDNA4LOG("disp2: free requested while instance %u's pair is pinned: IGNORED (stage %u kept)", inst, gD2Pl[pi].stage);
	} else if (freeNow) {      // 0.0.635: AFTER gD2Busy is released: the stage first (so no plane op can start on buffers about to be freed), then the allocator, under the native client lock
		gD2Pl[pi].stage = RDNA4D2_PL_NONE; gD2Pl[pi].mc[0] = 0u; gD2Pl[pi].mc[1] = 0u; gD2Off[pi][0] = 0u; gD2Off[pi][1] = 0u;
		amdgpu::n1c_d2_free(inst);
	}
	return st;
}


// =====================================================================================================================================================================================================
// build 0.0.661 (M6 Stage 1b, ABI 1.11; an internal design note "Q2: Stage 1b build contract") + 0.0.662 (M6 Stage 2, ABI 1.12; "Stage 2 build contract (monitor A, instance 1)"): the HDMI displays' scanout - INSTANCE 2 = the
// monitor B (HUBP2 / OTG2) and INSTANCE 1 = the monitor A (HUBP1 / OTG1), poll-only. THE SEQUENCES ARE dcn/scanx_flow.h (one XDesc per instance); this block is the environment (register read / write, clock, lock, thread) and the entry points.
// Each instance writes exactly two registers, ITS OWN HUBPREQn_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH then ..._ADDRESS (instance 2: 0x3c83 / 0x3c82; instance 1: 0x3ba7 / 0x3ba6), and only through rdna4scanx::write_addr (which judges the whole
// 64-bit address); the DCN write allowlist is asked again here. Behind garnet-m6=1 AND garnet-m6flip=1 (instance 2) and additionally garnet-m6flip1=1 (instance 1), all read ONCE: with a latch OFF nothing here touches a register,
// a lock or memory for that instance (the wrappers return before the environment is built).
// LOCK ORDER: the native client lock -> gScan.lock -> gSXLock[i]; each gSXLock[i] is a LEAF (nothing else is locked under it; no sleep under it) and NO CODE EVER HOLDS TWO OF THEM: the other display's ranges are snapshotted before this one's lock
// is taken (sx_excl_snapshot). Instance 0's Register may ask scanXClash under gScan.lock; no flow of an HDMI instance ever takes gScan.lock.
// =====================================================================================================================================================================================================

namespace {
IOLock *sx_lock(uint32_t inst) {
	if (inst != 1u && inst != 2u) return nullptr;
	if (gSXLock[inst] == nullptr) {
		IOLock *l = IOLockAlloc();
		if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSXLock[inst])) IOLockFree(l);
	}
	return gSXLock[inst];
}
// the latches of an instance: 2 needs garnet-m6 + garnet-m6flip (0.0.661), 1 additionally garnet-m6flip1 (0.0.662); any other number has none
static inline bool sx_latched() { return rdna4m6_latched_on() && rdna4m6flip_latched_on(); }
static inline bool sx_latched_inst(uint32_t inst) { return inst == 2u ? sx_latched() : inst == 1u ? (sx_latched() && rdna4m6flip1_latched_on()) : false; }
void sx_watchdog(void *arg, wait_result_t);
struct SXEnv {
	const rdna4scanx::XDesc &d;
	explicit SXEnv(const rdna4scanx::XDesc &dd) : d(dd) {}
	uint32_t gate() {
		if (!sx_latched_inst(d.inst)) return rdna4scan::kUnsupported;
		if (!gDcn.dev || !__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return rdna4scan::kNotReady;
		if (amdgpu::native_s1b_state().gate != rdna4native::kGateOn) return rdna4scan::kNotReady;      // native boots only
		if (amdgpu::n1c_hung()) return rdna4scan::kNotReady;
		if (__atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u) return rdna4scan::kBusy;             // a mode trial / hold is running
		if (sx_lock(d.inst) == nullptr) return rdna4scan::kNoResources;
		return 0u;
	}
	uint32_t rd(uint32_t abs) { return (abs >= gDcn.d.mmio_dwords) ? 0xFFFFFFFFu : gDcn.d.rreg(gDcn.d.cookie, abs); }
	bool wr(uint32_t abs, uint32_t v) {        // THE register write of this instance: only ITS two address registers, and the DCN allowlist again
		if (!dcn_display_txn_ok("scanx register write")) return false;
		if (!rdna4scanx::write_reg_ok(d, abs)) { RDNA4LOG("scanx: write of %#06x REFUSED (instance %u): not this HUBP's primary address HIGH / LOW", abs, d.inst); return false; }
		if (!dcn41_allow_write(&gDcn.allow, abs, v, "scanx")) { RDNA4LOG("scanx: the DCN allowlist REFUSED write %#010x = %#010x (instance %u)", abs, v, d.inst); return false; }
		amdgpu::WREG32(*gDcn.dev, abs, v);
		return true;
	}
	uint64_t now_ns() { return rdna4dcn::now_ns(); }
	void sleep_ms(uint32_t ms) { IOSleep(ms); }   // never called with this instance's lock held (the flows drop it first)
	void lock() { IOLockLock(gSXLock[d.inst]); }
	void unlock() { IOLockUnlock(gSXLock[d.inst]); }
	uint32_t d2_stage() { return __atomic_load_n(&gD2Pl[d.d2idx].stage, __ATOMIC_ACQUIRE); }
	uint32_t d2_cur() { return gD2Pl[d.d2idx].cur; }
	uint64_t d2_mc(uint32_t i) { return gD2Pl[d.d2idx].mc[i & 1u]; }
	void d2_set_cur_a() { gD2Pl[d.d2idx].cur = 0u; }
	bool window(uint64_t *lo, uint64_t *hi) { *lo = gDcn.d.scanout_lo; *hi = gDcn.d.scanout_hi; return *hi != 0u; }   // the flip layer's window, unknown when hi is 0
	bool start_watchdog(uint32_t gen) {
		thread_t th = nullptr;
		__atomic_fetch_add(&gSX[d.inst].watchdogAlive, 1u, __ATOMIC_ACQ_REL);
		const kern_return_t kr = kernel_thread_start(&sx_watchdog, (void *)(((uintptr_t)d.inst << 32) | (uintptr_t)gen), &th);     // the thread's argument carries the INSTANCE as well as the generation
		if (kr != KERN_SUCCESS) { __atomic_fetch_sub(&gSX[d.inst].watchdogAlive, 1u, __ATOMIC_ACQ_REL); RDNA4LOG("scanx: Acquire(%s) refused: the watchdog thread did not start (kr %#x)", d.name, kr); return false; }
		thread_deallocate(th);
		return true;
	}
	void note(const char *f) { ::amdgpu::rdna4_logf("%s", f); }
	template <class T, class... A> void note(const char *f, T t, A... a) { ::amdgpu::rdna4_logf(f, t, a...); }
};
void sx_watchdog(void *arg, wait_result_t) {
	const uint32_t inst = (uint32_t)((uintptr_t)arg >> 32);
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr) return;                                    // cannot happen: the thread is only started by SXEnv::start_watchdog with a valid instance
	SXEnv env(*d);
	const uint32_t gen = (uint32_t)((uintptr_t)arg & 0xFFFFFFFFu);
	uint32_t refusals = 0;
	for (;;) {
		bool done = false;
		{
			rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
			if (!txn.ok) {
				IOLock *l = gSXLock[inst];
				bool current = false;
				if (l != nullptr) {
					IOLockLock(l);
					current = gSX[inst].acquired && gSX[inst].gen == gen;
					IOLockUnlock(l);
				}
				if (!current) break;
				if (refusals++ < 8u)
					RDNA4LOG("scanx: WATCHDOG(%s) deferred: display transaction conflict", d->name);
				IOSleep(rdna4scanx::kWatchMs);
				continue;
			}
			done = rdna4scanx::watchdog_step(env, *d, gSX[inst], gen);
		}
		if (done) break;
		IOSleep(rdna4scanx::kWatchMs);
	}
	__atomic_fetch_sub(&gSX[inst].watchdogAlive, 1u, __ATOMIC_RELEASE);   // the thread touches nothing of ours after this
}
// disp2's per-buffer allocation size of an HDMI instance (the pair's exclusion size): the descriptor's constant is checked against it by the host test
static inline uint64_t sx_pair_bytes(uint32_t inst) { const rdna4d2_surf *sf = rdna4d2_surf_get(inst); return sf != nullptr ? sf->buf_bytes : 0ull; }
// The ranges instance `inst`'s Register must not overlap: the DP console and its slots (gScan, under its lock, released before this instance's lock is taken), the OTHER HDMI display's pair (disp2's plane buffers, ALLOCATION size) and
// its registered slots (read under THAT instance's leaf lock, taken and released here: never together with this instance's lock - the caller takes its own only afterwards, under the native client lock).
void sx_excl_snapshot(uint32_t inst, rdna4scanx::Excl *ex) {
	ex->n = 0u;
	auto add = [&](uint64_t lo, uint64_t bytes) { if (lo != 0ull && bytes != 0ull && ex->n < rdna4scanx::kMaxExcl) { ex->lo[ex->n] = lo; ex->bytes[ex->n] = bytes; ex->n++; } };
	IOLock *l = scan_lock();
	if (l != nullptr) {
		IOLockLock(l);
		if (gScan.haveConsole) add(gScan.consoleMc, gScan.consoleBytes);
		else {
			uint64_t cur = 0;
			if (gDcn.armed && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK) { ScanGeom g {}; scan_read_geom(&g); add(cur, (uint64_t)g.pitchPx * 4ull * g.h); }
		}
		for (uint32_t i = 0; i < rdna4scan::kMaxSlots; i++) if (gScan.tbl.s[i].used) add(gScan.tbl.s[i].mc, gScan.tbl.s[i].bytes);
		IOLockUnlock(l);
	}
	const uint32_t other = inst == 2u ? 1u : 2u;                     // the other HDMI display (instance 1's other is the monitor B, instance 2's is the monitor A)
	const rdna4scanx::XDesc *od = rdna4scanx::desc_of(other);
	if (od != nullptr && __atomic_load_n(&gD2Pl[od->d2idx].stage, __ATOMIC_ACQUIRE) != RDNA4D2_PL_NONE) { add(gD2Pl[od->d2idx].mc[0], sx_pair_bytes(other)); add(gD2Pl[od->d2idx].mc[1], sx_pair_bytes(other)); }
	IOLock *ol = gSXLock[other];                                     // 0.0.662: and the other display's registered scanout slots (nothing when it never acquired: no lock yet)
	if (ol != nullptr) {
		IOLockLock(ol);
		for (uint32_t i = 0; i < rdna4scan::kMaxSlots; i++) if (gSX[other].tbl.s[i].used) add(gSX[other].tbl.s[i].mc, gSX[other].tbl.s[i].bytes);
		IOLockUnlock(ol);
	}
}
}  // namespace

uint32_t scanXGeneration(uint32_t inst) { return (inst == 1u || inst == 2u) ? gSX[inst].gen : 0u; }
uint32_t scanXGpuHeld(uint32_t inst) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || !sx_latched_inst(inst) || gSXLock[inst] == nullptr) return 0u;
	IOLockLock(gSXLock[inst]);
	uint32_t code = 0u;
	if (gSX[inst].acquired) { SXEnv env(*d); uint64_t p = 0, e = 0; bool pend = false; (void)rdna4scanx::read_plane(env, *d, &p, &e, &pend); code = rdna4scanx::gpu_held_code(true, gSX[inst].tbl, e); }
	IOLockUnlock(gSXLock[inst]);
	return code;
}

// out: [0] A [1] extended OTG frame count [2] the M6 table generation [3] 0 (the native client fills the free visible-VRAM figure) [4] the instance's geometry: w | h << 16 | pitchPx << 32 (the bundle cross-checks it against its own constants)
uint32_t scanXAcquire(uint32_t inst, uint64_t out[5]) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || d->inst != inst) return rdna4scan::kBadArg;
	if (!sx_latched_inst(inst)) return rdna4scan::kUnsupported;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return rdna4scan::kBusy;
	SXEnv env(*d);
	uint64_t o[2] = { 0, 0 };
	const uint32_t rc = rdna4scanx::acquire(env, *d, gSX[inst], o);
	if (rc == rdna4scan::kOk) { out[0] = o[0]; out[1] = o[1]; out[2] = (uint64_t)rdna4disp_m6_gen(); out[3] = 0ull; out[4] = (uint64_t)d->w | ((uint64_t)d->h << 16) | ((uint64_t)d->pitchPx << 32); }
	return rc;
}
uint32_t scanXRegister(uint32_t inst, bool boVis, uint64_t boMc, uint64_t boSize, uint64_t offset, uint32_t pitchBytes, uint32_t width, uint32_t height, uint32_t format, uint64_t out[2], bool boDcc) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || d->inst != inst) return rdna4scan::kBadArg;
	if (!sx_latched_inst(inst)) return rdna4scan::kUnsupported;
	SXEnv env(*d);
	rdna4scanx::Excl ex;
	sx_excl_snapshot(inst, &ex);
	const rdna4scanx::RegIn in { boVis, boMc, boSize, offset, pitchBytes, width, height, format, boDcc };
	return rdna4scanx::register_slot(env, *d, gSX[inst], in, ex, out);
}
uint32_t scanXPresent(uint32_t inst, uint64_t slotId, uint64_t flags, uint64_t out[3]) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || d->inst != inst) return rdna4scan::kBadArg;
	if (!sx_latched_inst(inst)) return rdna4scan::kUnsupported;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) return rdna4scan::kBusy;
	SXEnv env(*d);
	return rdna4scanx::present(env, *d, gSX[inst], slotId, flags, out);
}
uint32_t scanXStatus(uint32_t inst, struct rdna4n_scan_status *o, uint64_t out[4]) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || d->inst != inst) return rdna4scan::kBadArg;
	if (!sx_latched_inst(inst)) return rdna4scan::kUnsupported;
	SXEnv env(*d);
	rdna4scanx::StatusOut st;
	if (rdna4m6_latched_on() && rdna4m6flip_latched_on()) rdna4disp_m6_flush();   // 0.0.661 (R5): the keep-alive flushes a deferred table publish
	const uint32_t rc = rdna4scanx::status(env, *d, gSX[inst], &st);
	if (rc != rdna4scan::kOk) return rc;
	bzero(o, sizeof(*o));
	o->acquired = st.acquired; o->flags = st.flags; o->front_slot = st.frontSlot; o->pending_slot = st.pendingSlot;
	o->frame_count = st.frameCount; o->console_mc = st.consoleMc; o->plane_mc = st.planeMc; o->earliest_mc = st.earliestMc;
	o->presents = st.presents; o->latched = st.latched; o->replaced = st.replaced; o->repeats = st.repeats;
	o->vupdates = 0; o->latch_irq = 0; o->latch_poll = st.latchPoll; o->refused = st.refused;          // the HDMI instances are poll-only: no VUPDATE source, no IRQ latches
	o->first_latch_ns = st.firstLatchNs; o->last_latch_ns = st.lastLatchNs;
	o->idle_ms = st.idleMs; o->watchdog_restores = st.wdRestores; o->storm_trips = 0; o->geom_refused = st.geomRefused;
	for (uint32_t i = 0; i < rdna4scan::kMaxSlots; i++) {
		rdna4n_scan_slot &sl = o->slot[i];
		sl.mc = st.slot[i].mc; sl.latched_frame = st.slot[i].latchedFrame; sl.used = st.slot[i].used; sl.flags = st.slot[i].flags; sl.presents = st.slot[i].presents; sl.latches = st.slot[i].latches;
	}
	out[0] = (uint64_t)rdna4disp_m6_gen(); out[1] = st.reuseInuseRefused; out[2] = st.restores | (st.restoreFailures << 32); out[3] = st.writeRefused | (st.writeFailed << 32);
	return rdna4scan::kOk;
}
static uint32_t scan_x_release_under_txn(uint32_t inst, const rdna4scanx::XDesc *d, const char *why, uint64_t out[2]) {
	SXEnv env(*d);
	return rdna4scanx::restore(env, *d, gSX[inst], why, 0u, out);
}
uint32_t scanXRelease(uint32_t inst, const char *why, uint64_t out[2]) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || !sx_latched_inst(inst) || gSXLock[inst] == nullptr) { if (out) { out[0] = 1; out[1] = 0; } return 0u; }       // nothing was ever acquired (a latch is OFF, or no Acquire ran): no register, no lock
	if (rdna4displaytxn::held()) return scan_x_release_under_txn(inst, d, why, out);
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { if (out) { out[0] = 0; out[1] = 0; } return rdna4scan::kBusy; }
	return scan_x_release_under_txn(inst, d, why, out);
}
uint32_t scanXBoGone(uint32_t inst, uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out) {
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	if (d == nullptr || !sx_latched_inst(inst) || gSXLock[inst] == nullptr) return 0u;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		if (out) { out[0] = 0; out[1] = 0; }
		return 2u;
	}
	SXEnv env(*d);
	return rdna4scanx::bo_gone(env, *d, gSX[inst], pinMask, pinGen, alwaysFull, out);
}
// instance 0's Register asks this under gScan.lock (latches m6 + m6flip ON): does [mc, mc + bytes) overlap an HDMI display's A, B or a registered slot? One display's lock at a time, never together.
bool scanXClash(uint64_t mc, uint64_t bytes) {
	if (!sx_latched() || bytes == 0ull) return false;
	bool clash = false;
	for (uint32_t inst = 1u; inst <= 2u; inst++) {
		const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
		if (d == nullptr || !sx_latched_inst(inst)) continue;        // instance 1 only with garnet-m6flip1: with it OFF this is 0.0.661's check exactly
		if (__atomic_load_n(&gD2Pl[d->d2idx].stage, __ATOMIC_ACQUIRE) != RDNA4D2_PL_NONE) {
			const uint64_t sb = sx_pair_bytes(inst);
			for (uint32_t i = 0; i < 2u; i++) if (gD2Pl[d->d2idx].mc[i] != 0ull && rdna4scan::ranges_overlap(mc, bytes, gD2Pl[d->d2idx].mc[i], sb)) clash = true;
		}
		IOLock *l = gSXLock[inst];
		if (l != nullptr) {
			IOLockLock(l);
			for (uint32_t i = 0; i < rdna4scan::kMaxSlots; i++) if (gSX[inst].tbl.s[i].used && rdna4scan::ranges_overlap(mc, bytes, gSX[inst].tbl.s[i].mc, gSX[inst].tbl.s[i].bytes)) clash = true;
			IOLockUnlock(l);
		}
	}
	return clash;
}
static void scan_x_shutdown_under_txn() {
	for (uint32_t inst = 1u; inst <= 2u; inst++) {
		if (gSXLock[inst] == nullptr) continue;
		uint64_t r[2] = { 1, 0 };
		const uint32_t rc = scanXRelease(inst, "kext stop", r);
		if (rc != rdna4scan::kOk || r[0] == 0ull)
			RDNA4LOG("scanx: kext stop: instance %u release status %u, restore verified %llu", inst, rc, (unsigned long long)r[0]);
		for (uint32_t i = 0; i < 2000u && __atomic_load_n(&gSX[inst].watchdogAlive, __ATOMIC_ACQUIRE) != 0u; i++) /*nolock*/ IOSleep(1);
		if (__atomic_load_n(&gSX[inst].watchdogAlive, __ATOMIC_ACQUIRE) != 0u) RDNA4LOG("scanx: kext stop: the instance-%u watchdog thread is still alive after 2 s", inst);
	}
}

void scanXShutdown() {
	if (rdna4displaytxn::held()) {
		scan_x_shutdown_under_txn();
		return;
	}
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) {
		RDNA4LOG("scanx: kext stop REFUSED: no display transaction");
		return;
	}
	scan_x_shutdown_under_txn();
}

// accel `m6xstat [page]` (action 107): READ-ONLY. The argument is page | instance << 8 (instance 0 = the default, 2 = the monitor B; 1 = the monitor A). Page 0 the state and counters, page 1 the slots, the refusal counters and the writer's record, page 2 the live
// HUBPn / OTGn registers (the run kit's per-minute health read: underflow, ODMn bit 10, the VM fault, FLIP_PENDING, EARLIESTn). With a latch OFF only v[0] bits 0..1 / 11 (the latch words) are filled and NOTHING is read.
// v[0] bit 11: garnet-m6flip1 latched, bits 12..13: the instance reported.
uint32_t scanXReport(uint64_t arg, uint64_t *v, unsigned n) {
	if (!v || n < 13u) return rdna4scan::kBadArg;
	for (unsigned i = 0; i < 13u; i++) v[i] = 0ull;
	const uint64_t page = arg & 0xFFull;
	uint32_t inst = (uint32_t)((arg >> 8) & 0xFFull);
	if (inst == 0u) inst = 2u;
	const rdna4scanx::XDesc *d = rdna4scanx::desc_of(inst);
	const bool l1 = rdna4m6_latched_on(), l2 = rdna4m6flip_latched_on(), l3 = rdna4m6flip1_latched_on();
	v[0] = (l1 ? 1ull : 0ull) | (l2 ? 2ull : 0ull) | (l3 ? (1ull << 11) : 0ull) | ((uint64_t)(inst & 3u) << 12);
	if (d == nullptr || (arg >> 16) != 0ull || page > 2ull) return rdna4scan::kBadArg;
	if (!l1 || !l2 || (inst == 1u && !l3)) return rdna4scan::kOk;
	SXEnv env(*d);
	if (page == 2ull) {
		if (!gDcn.dev || !__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return rdna4scan::kNotReady;
		v[0] |= 4ull;
		v[1] = (uint64_t)env.rd(d->regHubpCntl) | ((uint64_t)env.rd(d->regVmFault) << 32);
		v[2] = (uint64_t)env.rd(d->regOdmGlobal) | ((uint64_t)env.rd(d->regOtgFrame) << 32);
		v[3] = (uint64_t)env.rd(d->regFlipControl) | ((uint64_t)env.rd(d->regViewDim) << 32);
		v[4] = (uint64_t)env.rd(d->regAddr) | ((uint64_t)env.rd(d->regAddrHigh) << 32);
		v[5] = (uint64_t)env.rd(d->regEarlyLo) | ((uint64_t)env.rd(d->regEarlyHi) << 32);
		v[6] = (uint64_t)env.rd(d->regPitch) | ((uint64_t)env.rd(d->regSurfConfig) << 32);
		v[7] = (uint64_t)env.rd(d->regSurfControl) | ((uint64_t)env.rd(d->regVmid) << 32);
		v[8] = (uint64_t)env.rd(d->regOtgControl);
		v[9] = (uint64_t)gD2Pl[d->d2idx].stage | ((uint64_t)gD2Pl[d->d2idx].cur << 8) | ((uint64_t)(amdgpu::n1c_d2_pinned(inst) ? 1u : 0u) << 16);
		v[10] = gD2Pl[d->d2idx].mc[0]; v[11] = gD2Pl[d->d2idx].mc[1];
		return rdna4scan::kOk;
	}
	if (gSXLock[inst] == nullptr) return rdna4scan::kOk;     // nothing was ever acquired
	IOLockLock(gSXLock[inst]);
	const rdna4scanx::ScanX &x = gSX[inst];
	if (page == 0ull) {
		v[0] |= (x.acquired ? 4ull : 0ull) | (x.restoring ? 8ull : 0ull) | (x.wantRestore ? 16ull : 0ull) | (x.restoreFailed ? 32ull : 0ull) | (x.haveConsole ? 64ull : 0ull);
		if (x.acquired) { uint64_t pp = 0, ee = 0; bool pd = false; (void)rdna4scanx::read_plane(env, *d, &pp, &ee, &pd); v[0] |= (uint64_t)rdna4scanx::gpu_held_code(true, x.tbl, ee) << 8; }      // bits 8..10: GPU-held code (0 not, 1..3 slot k+1 is fetched, 4 not a slot)
		v[1] = (uint64_t)x.gen | (x.acquires << 32);
		v[2] = x.bufA; v[3] = x.bufB;
		v[4] = x.tbl.presents | (x.tbl.latched << 32);
		v[5] = x.tbl.replaced | (x.refused << 32);
		v[6] = x.latchPoll | (x.reuseInuseRefused << 32);
		v[7] = x.wdRestores | (x.restores << 32);
		v[8] = x.restoreFailures | (x.acquireRefused << 32);
		v[9] = (uint64_t)x.lastAcquireWhy | ((uint64_t)rdna4disp_m6_gen() << 32);
		v[10] = x.lastActivityNs ? (rdna4dcn::now_ns() - x.lastActivityNs) / 1000000ull : 0ull;
		v[11] = x.lastRestoreUs; v[12] = x.lastWritten;
	} else {
		for (uint32_t i = 0; i < rdna4scan::kMaxSlots; i++) { v[1 + i] = x.tbl.s[i].used ? x.tbl.s[i].mc : 0ull; v[4 + i] = (uint64_t)x.tbl.s[i].presents | ((uint64_t)x.tbl.s[i].latches << 32); }
		v[7] = x.writeRefused | (x.writeFailed << 32);
		v[8] = x.untaggedRefused | (x.geomRefused << 32);
		v[9] = x.firstLatchNs; v[10] = x.lastLatchNs;
		v[11] = (uint64_t)(x.tbl.front == rdna4scan::kNoSlot ? 0xFFFFFFFFu : rdna4scanx::id_of(*d, x.tbl.front)) | ((uint64_t)(x.tbl.pendActive ? rdna4scanx::id_of(*d, x.tbl.pendSlot) : 0xFFFFFFFFu) << 32);
		v[12] = x.lastWritten;
	}
	IOLockUnlock(gSXLock[inst]);
	return rdna4scan::kOk;
}

}  // namespace rdna4dcn
