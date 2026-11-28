//
//  disp2_flow.h - build 0.0.631 (multi-monitor stage M4d): the SEQUENCE of accel verb 98 `disp2 <op>`, written against an environment E so the host test (tests/native_disp2_test.cpp) runs the very
//  code the kext runs, over a register-file model of OTG0 / OTG1 / DIG1 / DIG2 with the REAL DCN write allowlist. C++17, no kernel header. The decisions, the step lists and the guard are disp2.h's.
//
//  Environment contract (the kext's glue is in dcn/dcn.cpp; the test's in tests/native_disp2_test.cpp):
//      uint32_t E::rd(uint32_t abs)                 ONE register read (absolute BAR5 dword); never writes
//      bool     E::wr(uint32_t abs, uint32_t v)     THE register write: the kext judges rdna4d2_guard_write AGAIN and then the DCN write allowlist; false = refused, nothing reached the card
//      void     E::delay_us(uint32_t) ; void E::sleep_ms(uint32_t)
//      void     E::note(uint32_t i, const rdna4d2_step &s, uint32_t old, uint32_t v, uint32_t what)   one line per step for the log (what: RDNA4D2_N_*)
//      uint32_t *E::dpx_buf(unsigned k)            0.0.634: two buffers of RDNA4D2_DPX_BUF dwords (k = 0 the DP-plane reads before the op, 1 after; 0.0.635: 16 dwords, the last the HUBP0 primary HIGH): file-scope in the kext (single flight), never on the kernel stack
//   0.0.635 (the plane ops 8..12; used by run_plane and by the DP watch's primary-address rule):
//      rdna4d2_plane *E::pl_of(uint32_t inst)        0.0.655: the plane state of an INSTANCE across ops (file-scope in the kext; instance 1 = the monitor A's, 2 = the monitor B's). The flow never asks anything else: no E::pl()
//      uint32_t *E::w2_buf(unsigned k)             0.0.655: two buffers of RDNA4D2_W2_BUF dwords (k = 0 the monitor B watch before an MONA op, 1 after; file-scope in the kext)
//      0.0.655: E::rec_buf(), E::pin(), E::vfill() and E::vread() act on the instance E::inst names (the flow checks E::inst == the op's instance before it starts a plane op)
//      uint32_t *E::gate_buf()                     RDNA4D2_GATES dwords: the final gate reads of the op (file-scope in the kext)
//      bool     E::window(uint64_t *lo, *hi)       the scanout window [lo, hi) in MC space (the flip layer's); false = unknown
//      bool     E::desktop_acquired()              the native scanout is acquired: HUBP0's primary address legitimately moves
//      bool     E::vfill(unsigned k, uint32_t byteOff, uint32_t pattern, uint32_t bytes)   fill bytes of buffer k (A = 0, B = 1) with a dword pattern (bounds-checked in the kext; false = refused)
//      void     E::vflush()                        the HDP flush;  bool E::vread(unsigned k, uint32_t byteOff, uint32_t *dw)   one dword of buffer k through the MM window (the GPU's view)
//   0.0.638: uint32_t *E::rec_buf()                 RDNA4D2_REC_N dwords: the record page (the recorded reads of op 8, the timed-out WAITs' register + last value); file-scope in the kext
//      rdna4d2_step *E::buf(unsigned k)              two scratch lists of RDNA4D2_MAX_STEPS steps (k = 0 the op, 1 the rollback): file-scope in the kext (single flight), never on the kernel stack
//
//  0.0.633: every function below takes the INSTANCE (default 1 = the monitor A: OTG1 / DIG2 / PHY C; 2 = the monitor B: OTG2 / DIG3 / PHY D) and reads its registers from the rdna4d2_inst table; the guard it applies is that instance's
//  (rdna4d2_guard_write_i / rdna4d2_guard_list_i). `connect` starts with the DCCG symbol-clock exception (SYMCLK<n>_CLOCK_ENABLE FE_EN / FE_SRC_SEL, the ONLY DCCG write), `off` ends the front end with its reverse; SYMCLKB (the
//  live DP's) is READ before and after every op like OTG0 and DIG1 and a change of it ends the op in RDNA4D2_DP_DISTURBED.
//
//  The order of timing / connect (off and status below):
//    1 build the op's step list and judge EVERY step with the instance guard - a refusal reads nothing and writes nothing
//    2 the DP snapshot (OTG0_CONTROL, DIG1 FE / BE, DIG1 mapper) and OTG0's frame counter; 50 ms; OTG0 must have counted (else nothing is written)
//    3 the op's prechecks (reads): timing needs OTG1's PHYPLL source = PLL2 and OTG1 not master-enabled; connect needs OTG1 running, DIG2's BE in DVI mode with clock and enable on, DIG2's FE disabled
//    4 the steps, in Linux's order; every write judged by the guard (address AND value) and by E::wr; a refusal stops the op and `off` runs at once (best effort)
//    5 50 ms; OTG0 must still count and the DP snapshot must be unchanged, else `off` runs at once and the status is RDNA4D2_DP_DISTURBED
//  off: 1, the steps best effort (a refused write is skipped and counted, the rest still run), 5 without a second off. status: reads only.
//
#ifndef RDNA4_DISP2_FLOW_H
#define RDNA4_DISP2_FLOW_H

#include <stdint.h>
#include "disp2.h"

namespace rdna4d2 {

enum : uint32_t { RDNA4D2_N_WRITE = 0, RDNA4D2_N_REFUSED = 1, RDNA4D2_N_WAIT_OK = 2, RDNA4D2_N_WAIT_TIMEOUT = 3, RDNA4D2_N_DELAY = 4, RDNA4D2_N_REC = 5, RDNA4D2_N_SKIP = 6 };

// Run steps [0, n). bestEffort (off): a refused write is skipped; otherwise the first refusal returns its status with o.fail = the step. Wait timeouts are counted, never fatal (Linux's REG_WAIT).
template <class E>
inline uint32_t run_steps(E &e, uint32_t inst, const rdna4d2_step *s, uint32_t n, bool bestEffort, rdna4d2_out &o, uint32_t *refusals)
{
    const rdna4d2_pr *const pr = rdna4d2_pr_get(inst);      // 0.0.655: the instance's HUBP / address registers (instance 2's are the constants this function always named)
    bool noOutTimedOut = false;      // 0.0.639: the NO_OUTSTANDING_REQ wait of THIS list timed out: a fetch may be in flight, so the primary address is NOT zeroed
    for (uint32_t i = 0; i < n; i++) {
        const rdna4d2_step &st = s[i];
        if (st.kind == RDNA4D2_K_SET && noOutTimedOut && st.val == 0u && (st.abs == pr->addr_hi || st.abs == pr->addr_lo)) {
            e.note(i, st, 0u, 0u, RDNA4D2_N_SKIP);      // 0.0.639: a zero address with a fetch in flight could VM-fault; the address stays, the buffers are leaked (plane_rollback)
            continue;
        }
        if (st.kind == RDNA4D2_K_SET || st.kind == RDNA4D2_K_UPD) {
            const uint32_t old = st.kind == RDNA4D2_K_UPD ? e.rd(st.abs) : 0u;
            const uint32_t v = rdna4d2_apply(&st, old);
            const uint32_t g = rdna4d2_guard_write_i(inst, st.abs, v, 0);
            if (g != RDNA4D2_G_OK || !e.wr(st.abs, v)) {
                e.note(i, st, old, v, RDNA4D2_N_REFUSED);
                if (refusals) (*refusals)++;
                if (bestEffort) continue;
                o.fail = i; o.guard = g;
                return g != RDNA4D2_G_OK ? (uint32_t)RDNA4D2_GUARD_REFUSED : (uint32_t)RDNA4D2_ALLOW_REFUSED;
            }
            o.done++;
            e.note(i, st, old, v, RDNA4D2_N_WRITE);
        } else if (st.kind == RDNA4D2_K_COPY0) {      // 0.0.635: read the PIPE-0 source, pin it to its census value (the drift tripwire), write it to pipe 2; the guard judges the destination AND the value
            const uint32_t sv = e.rd(st.cond_abs);
            if ((sv & st.mask) != (st.val & st.mask)) {
                e.note(i, st, sv, st.val, RDNA4D2_N_REFUSED);
                if (refusals) (*refusals)++;
                o.fail = i; o.pre_abs = st.cond_abs; o.pre_val = sv;
                if (bestEffort) continue;
                return (uint32_t)RDNA4D2_PRECHECK;
            }
            const uint32_t cv = sv & st.cond_mask;      // 0.0.637: only the pair's write mask is written (DSCL2_LB_MEMORY_CTRL: Linux's two fields)
            const uint32_t g = rdna4d2_guard_write_i(inst, st.abs, cv, 0);
            if (g != RDNA4D2_G_OK || !e.wr(st.abs, cv)) {
                e.note(i, st, sv, cv, RDNA4D2_N_REFUSED);
                if (refusals) (*refusals)++;
                if (bestEffort) continue;
                o.fail = i; o.guard = g;
                return g != RDNA4D2_G_OK ? (uint32_t)RDNA4D2_GUARD_REFUSED : (uint32_t)RDNA4D2_ALLOW_REFUSED;
            }
            o.done++;
            e.note(i, st, sv, cv, RDNA4D2_N_WRITE);
        } else if (st.kind == RDNA4D2_K_EXPECT) {     // 0.0.635: READ ONLY. A mismatch ends the list in RDNA4D2_PRECHECK (nothing was written by it)
            const uint32_t r = e.rd(st.abs);
            if ((r & st.mask) != st.val) {
                e.note(i, st, r, 0u, RDNA4D2_N_REFUSED);
                o.fail = i; o.pre_abs = st.abs; o.pre_val = r;
                return (uint32_t)RDNA4D2_PRECHECK;
            }
            e.note(i, st, r, 0u, RDNA4D2_N_WAIT_OK);
        } else if (st.kind == RDNA4D2_K_WAIT || st.kind == RDNA4D2_K_WAITIF) {
            if (st.kind == RDNA4D2_K_WAITIF && (e.rd(st.cond_abs) & st.cond_mask) == 0u) {   // enc35_reset_fifo: no symbol clock -> udelay(10), no wait
                e.delay_us(st.step_us);
                e.note(i, st, 0u, 0u, RDNA4D2_N_DELAY);
                if (st.kind == RDNA4D2_K_WAITIF) o.symclk_delay++;     // 0.0.632: the FIFO-reset report
                continue;
            }
            if (st.kind == RDNA4D2_K_WAITIF) o.symclk_wait++;
            uint32_t r = e.rd(st.abs), k = 0;
            while ((r & st.mask) != st.val && k < st.polls) { e.delay_us(st.step_us); r = e.rd(st.abs); k++; }
            if ((r & st.mask) != st.val) {
                o.timeouts++; o.wfail |= 1ull << (i & 63u);
                if (st.abs == pr->hubp_cntl && (st.mask & RDNA4D2_HUBP_NO_OUTSTANDING_MASK) != 0u && (st.val & RDNA4D2_HUBP_NO_OUTSTANDING_MASK) != 0u) noOutTimedOut = true;
                if (o.wait_abs == 0u) { o.wait_abs = st.abs; o.wait_val = r; }       // 0.0.638: the FIRST timed-out WAIT of the list: its register and the LAST value read
                e.note(i, st, r, k, RDNA4D2_N_WAIT_TIMEOUT);
            }
            else e.note(i, st, r, k, RDNA4D2_N_WAIT_OK);
        } else if (st.kind == RDNA4D2_K_REC) {         // 0.0.638: a RECORDED read: never a gate, never a write; stored in the record page
            if (!rdna4d2_rec_ok_i(inst, st.abs, st.val)) continue;
            if (st.step_us != 0u) e.delay_us(st.step_us);
            const uint32_t r = e.rd(st.abs);
            uint32_t *const rec = e.rec_buf();
            rec[st.val] = r; rec[RDNA4D2_RC_VALID] |= (st.val < 4u) ? RDNA4D2_RV_A : (st.val < 8u) ? RDNA4D2_RV_B : RDNA4D2_RV_DET0;
            e.note(i, st, r, st.val, RDNA4D2_N_REC);
        }
    }
    return 0u;
}

template <class E>
inline void dp_snapshot(E &e, uint32_t *d)
{
    for (uint32_t i = 0; i < RDNA4D2_DP_REGS; i++) d[i] = e.rd(rdna4d2_dp_regs[i]);
}

// 0.0.634 (stage M4a item 4): the DP's PLANE side, masked READS (rdna4d2_dpx_regs; 0.0.635: fifteen, plus the HUBP0 primary HIGH dword at index 15). Nothing here can write: E::rd is the only member used.
template <class E>
inline void dpx_snapshot(E &e, uint32_t *d)
{
    for (uint32_t i = 0; i < RDNA4D2_DPX_REGS; i++) d[i] = e.rd(rdna4d2_dpx_regs[i].abs) & rdna4d2_dpx_regs[i].mask;
    d[RDNA4D2_DPX_BUF - 1u] = e.rd(RDNA4D2_HUBPREQ0_PRIMARY_HIGH);
}
// 0.0.655: the MONB WATCH (rdna4d2_w2_regs): ten masked reads and OTG2's frame counter, taken before and after every MONA op. Nothing here can write: E::rd is the only member used.
template <class E>
inline void w2_snapshot(E &e, uint32_t *d)
{
    for (uint32_t i = 0; i < RDNA4D2_W2_DW; i++) d[i] = e.rd(rdna4d2_w2_regs[i].abs) & rdna4d2_w2_regs[i].mask;
    d[10] = e.rd(RDNA4D2_OTG2_FRAME_COUNT);
    d[11] = d[10];
}
// 0.0.635: the change mask of two snapshots. Rows 0..13 are plain masked differences. Row 14 (HUBP0's primary address) is a RULE: it is bad when it OVERLAPS A or B (0.0.636: rdna4d2_surface_overlaps at 64 KiB granularity; was an exact compare), when it lies outside the scanout window (when one is known), or
// when it CHANGED while the desktop scanout is not acquired (with the scanout acquired the desktop legitimately flips between its own visible-pool buffers). READ-ONLY.
template <class E>
inline uint32_t dpx_diff(E &e, const uint32_t *b, const uint32_t *a)
{
    uint32_t m = 0u;
    for (uint32_t i = 0; i < RDNA4D2_DPX_REGS; i++) if (i != RDNA4D2_DPX_RULE && a[i] != b[i]) m |= 1u << i;
    const uint64_t pb = ((uint64_t)b[RDNA4D2_DPX_BUF - 1u] << 32) | b[RDNA4D2_DPX_RULE], pa = ((uint64_t)a[RDNA4D2_DPX_BUF - 1u] << 32) | a[RDNA4D2_DPX_RULE];
    bool bad = false;
    uint64_t lo = 0u, hi = 0u;
    if (pa != pb && !e.desktop_acquired()) bad = true;
    if (e.window(&lo, &hi) && hi != 0u && (pa < lo || pa >= hi)) bad = true;
    for (uint32_t ii = RDNA4D2_INST_MONA; ii <= RDNA4D2_INST_MONB; ii++) {      // 0.0.655: BOTH instances' live pairs (the monitor A's A1 / B1 as well as the monitor B's A2 / B2); each judged at its own buffer size
        const rdna4d2_plane *const pl = e.pl_of(ii);
        const uint64_t sz = rdna4d2_surf_get(ii)->buf_bytes;
        if (pl->stage != RDNA4D2_PL_NONE && pl->stage != RDNA4D2_PL_FREE) for (uint32_t k = 0; k < 2u; k++) if (pl->mc[k] != 0u && rdna4d2_surface_overlaps_n(pa, pl->mc[k], pl->dp_bytes, sz)) bad = true;     // 0.0.636 (C3): an OVERLAP test, not an exact base compare
    }
    if (bad) m |= 1u << RDNA4D2_DPX_RULE;
    return m;
}

// `off` as a rollback inside another op (best effort; its own step count is not added to the op's).
template <class E>
inline void rollback(E &e, uint32_t inst, rdna4d2_out &o)
{
    rdna4d2_step *s = e.buf(1u);
    const uint32_t n = rdna4d2_build_i(inst, RDNA4D2_OP_OFF, s);
    rdna4d2_out scratch;
    for (unsigned i = 0; i < sizeof(scratch); i++) ((uint8_t *)&scratch)[i] = 0;
    uint32_t bad = 0;
    if (rdna4d2_guard_list_i(inst, s, n, &bad) != 0u) return;    // cannot happen (the host test proves the off list passes); never write a refused list
    (void)run_steps(e, inst, s, n, true, scratch, nullptr);
    o.auto_off = 1u;
    o.timeouts += scratch.timeouts;
}

template <class E>
inline void after_regs(E &e, const rdna4d2_inst &d, rdna4d2_out &o)
{
    o.otg1_ctl = e.rd(d.otg_control); o.fe_en = e.rd(d.fe_en); o.dpg_ctl = e.rd(d.dpg_ctl);
    o.be_cntl = e.rd(d.be_cntl); o.fifo = e.rd(d.fifo); o.fe_clk = e.rd(d.fe_clk);
}

// timing / connect / off. op is already a legal op (the glue judged the argument); inst is 1 (the monitor A) or 2 (the monitor B), anything else is BAD_ARG with nothing read or written.
template <class E>
inline rdna4d2_out run(E &e, uint32_t op, uint32_t inst = RDNA4D2_INST_MONA)
{
    rdna4d2_out o;
    for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
    o.op = op; o.fail = 0xFFu; o.status = RDNA4D2_BAD_ARG; o.inst = inst;
    const rdna4d2_inst *dp = rdna4d2_inst_get(inst);
    if (!dp) return o;
    const rdna4d2_inst &d = *dp;
    if (op != RDNA4D2_OP_TIMING && op != RDNA4D2_OP_TIMING1440 && op != RDNA4D2_OP_CONNECT && op != RDNA4D2_OP_OFF) return o;
    if (e.pl_of(inst)->stage == RDNA4D2_PL_HELD && rdna4d2_held_refuses(op, inst)) { o.status = RDNA4D2_HELD; return o; }     // 0.0.652 (M5): the plane is HELD for this boot: timing / connect / off / timing1440 would re-time or stop the instance's OTG under the framebuffer; nothing is read or written (0.0.655: per instance)
    const bool isTiming = op == RDNA4D2_OP_TIMING || op == RDNA4D2_OP_TIMING1440;     // 0.0.634: op 7 is op 1 over the monitor B's 1440p descriptor: every precheck and every step rule below is the same

    // ---- 1. the list and the guard: nothing read, nothing written on a refusal
    rdna4d2_step *s = e.buf(0u);
    const uint32_t n = rdna4d2_build_i(inst, op, s);
    o.total = n;
    uint32_t bad = 0;
    if (rdna4d2_guard_list_i(inst, s, n, &bad) != 0u) {
        o.status = RDNA4D2_GUARD_REFUSED; o.fail = bad;
        o.guard = bad < n ? rdna4d2_guard_write_i(inst, s[bad].abs, s[bad].val, 0) : 0u;
        return o;
    }

    // ---- 1b. 0.0.635: `off` for the monitor B would stop OTG2 under a running HUBP2: REFUSED while HUBP2's clock is on (run planeoff first). One read; nothing is written.
    //      0.0.636 (review item C4): the same for `timing` (1), `connect` (2) and `timing1440` (7) on the monitor B: they reprogram OTG2 / DIG3, which must never happen under a running HUBP2.
    //      0.0.655: the same for the MONA (HUBP1's clock): `off 1` would stop OTG1 under a running HUBP1.
    if (op == RDNA4D2_OP_OFF || op == RDNA4D2_OP_TIMING || op == RDNA4D2_OP_CONNECT || op == RDNA4D2_OP_TIMING1440) {
        const uint32_t hcReg = rdna4d2_pr_get(inst)->hubp_clk;
        const uint32_t hc = e.rd(hcReg);
        if ((hc & RDNA4D2_HUBP_CLK_WRITE_MASK & 1u) != 0u) { o.pre_abs = hcReg; o.pre_val = hc; o.status = RDNA4D2_PLANE_STATE; return o; }
    }

    // ---- 2. the DP side before, and (timing / connect) proof that OTG0 is counting
    dp_snapshot(e, o.dp_before);
    uint32_t *const dpxB = e.dpx_buf(0u), *const dpxA = e.dpx_buf(1u);
    dpx_snapshot(e, dpxB);     // 0.0.634: the DP's plane side
    uint32_t *const w2B = e.w2_buf(0u), *const w2A = e.w2_buf(1u);
    if (inst == RDNA4D2_INST_MONA) w2_snapshot(e, w2B);      // 0.0.655: the monitor B watch before an monitor A op
    o.symclkb_a = e.rd(RDNA4D2_SYMCLKB_CLOCK_ENABLE);
    o.f0a = e.rd(RDNA4D2_OTG0_FRAME_COUNT); o.f1a = e.rd(d.otg_frame);
    uint32_t fPre = o.f0a;
    if (op != RDNA4D2_OP_OFF) {
        e.sleep_ms(RDNA4D2_SETTLE_MS);
        fPre = e.rd(RDNA4D2_OTG0_FRAME_COUNT);
        if (fPre == o.f0a) { o.f0b = fPre; o.status = RDNA4D2_DP_NOT_COUNTING; return o; }
        if (inst == RDNA4D2_INST_MONA) w2B[11] = e.rd(RDNA4D2_OTG2_FRAME_COUNT);      // 0.0.655: was OTG2 (the monitor B) counting across the settle?
    }

    // ---- 3. the prechecks (reads only)
    if (isTiming) {
        const uint32_t pll = e.rd(d.phypll);
        if ((pll & RDNA4D2_PHYPLL_SOURCE_MASK) != d.phypll_pll) { o.pre_abs = d.phypll; o.pre_val = pll; o.status = RDNA4D2_NO_PIXCLK; return o; }
        const uint32_t ctl = e.rd(d.otg_control);
        if ((ctl & (RDNA4D2_OTG_MASTER_EN_MASK | RDNA4D2_OTG_CUR_MASTER_EN_MASK)) != 0u) { o.pre_abs = d.otg_control; o.pre_val = ctl; o.status = RDNA4D2_OTG1_BUSY; return o; }
    } else if (op == RDNA4D2_OP_CONNECT) {
        const uint32_t ctl = e.rd(d.otg_control);
        if ((ctl & (RDNA4D2_OTG_MASTER_EN_MASK | RDNA4D2_OTG_CUR_MASTER_EN_MASK)) != (RDNA4D2_OTG_MASTER_EN_MASK | RDNA4D2_OTG_CUR_MASTER_EN_MASK)) { o.pre_abs = d.otg_control; o.pre_val = ctl; o.status = RDNA4D2_NO_TIMING; return o; }
        const uint32_t beClk = e.rd(d.be_clk);
        if ((beClk & RDNA4D2_BE_MODE_MASK) != RDNA4D2_DVI_MODE || (beClk & RDNA4D2_BE_CLK_EN_MASK) == 0u) { o.pre_abs = d.be_clk; o.pre_val = beClk; o.status = RDNA4D2_NO_PHY; return o; }
        const uint32_t beEn = e.rd(d.be_en);
        if ((beEn & RDNA4D2_BE_ENABLE_MASK) == 0u) { o.pre_abs = d.be_en; o.pre_val = beEn; o.status = RDNA4D2_NO_PHY; return o; }
        const uint32_t feEn = e.rd(d.fe_en);
        if ((feEn & RDNA4D2_DIG_FE_ENABLE_MASK) != 0u) { o.pre_abs = d.fe_en; o.pre_val = feEn; o.status = RDNA4D2_FE_BUSY; return o; }
    }

    // ---- 4. the steps
    uint32_t refusals = 0;
    const uint32_t st = run_steps(e, inst, s, n, op == RDNA4D2_OP_OFF, o, &refusals);
    o.status = st;
    if (st != 0u && op != RDNA4D2_OP_OFF) rollback(e, inst, o);
    if (op == RDNA4D2_OP_OFF && refusals != 0u) o.status = RDNA4D2_ALLOW_REFUSED;

    // ---- 5. the DP side after
    e.sleep_ms(RDNA4D2_SETTLE_MS);
    o.f0b = e.rd(RDNA4D2_OTG0_FRAME_COUNT); o.f1b = e.rd(d.otg_frame);
    dp_snapshot(e, o.dp_after);
    dpx_snapshot(e, dpxA);
    o.symclkb_b = e.rd(RDNA4D2_SYMCLKB_CLOCK_ENABLE);
    for (uint32_t i = 0; i < RDNA4D2_DP_REGS; i++) if (o.dp_after[i] != o.dp_before[i]) o.dp_changed |= 1u << i;
    if (o.symclkb_b != o.symclkb_a) o.dp_changed |= 0x40u;     // 0.0.633: SYMCLKB (the live DP's symbol clock) must not move either
    if (o.f0b == fPre) o.dp_changed |= 0x80u;
    o.dpx_changed = dpx_diff(e, dpxB, dpxA);     // 0.0.634: MPC_OUT0_MUX, MPCC0, HUBP0, DPPCLK0, DET0, COMPBUF; 0.0.635: fifteen rows, row 14 the HUBP0 primary-address rule
    if (inst == RDNA4D2_INST_MONA) { w2_snapshot(e, w2A); o.w2_changed = rdna4d2_w2_diff(w2B, w2A); }      // 0.0.655: the monitor B watch after an monitor A op
    if (o.dp_changed != 0u || o.dpx_changed != 0u || o.w2_changed != 0u) {
        if (op != RDNA4D2_OP_OFF && !o.auto_off) rollback(e, inst, o);
        o.status = RDNA4D2_DP_DISTURBED;
        if (o.w2_changed != 0u) o.guard = rdna4d2_w2_first(o.w2_changed);      // 0.0.655: the 4-bit guard field carries (first changed monitor B-watch row + 1)
    }
    if (o.status == RDNA4D2_OK && o.timeouts != 0u) o.status = RDNA4D2_WAIT_TIMEOUT;
    after_regs(e, d, o);
    return o;
}

// status: reads only. v[0] = status | op << 8 | count << 16; v[1..12] = rdna4d2_stat_regs[] two per scalar. The two frame counters are read 50 ms apart.
template <class E>
inline uint32_t status(E &e, uint64_t *v, uint32_t inst = RDNA4D2_INST_MONA)
{
    uint32_t r[RDNA4D2_STAT_COUNT];
    const rdna4d2_stat_reg *tbl = rdna4d2_stat_tbl(inst);
    for (unsigned i = 0; i < 13u; i++) v[i] = 0u;
    r[RDNA4D2_STAT_OTG1_FC_A] = e.rd(tbl[RDNA4D2_STAT_OTG1_FC_A].abs);
    r[RDNA4D2_STAT_OTG0_FC_A] = e.rd(tbl[RDNA4D2_STAT_OTG0_FC_A].abs);
    e.sleep_ms(RDNA4D2_SETTLE_MS);
    for (uint32_t i = 0; i < RDNA4D2_STAT_COUNT; i++)
        if (i != RDNA4D2_STAT_OTG1_FC_A && i != RDNA4D2_STAT_OTG0_FC_A) r[i] = e.rd(tbl[i].abs);
    v[0] = (uint64_t)RDNA4D2_OK | ((uint64_t)RDNA4D2_OP_STATUS << 8) | ((uint64_t)RDNA4D2_STAT_COUNT << 16);
    for (uint32_t i = 0; i < RDNA4D2_STAT_COUNT; i++) v[1 + i / 2u] |= (uint64_t)r[i] << (32u * (i & 1u));
    return RDNA4D2_OK;
}

// status2 (0.0.632): reads only. v[0] = status | op << 8 | count << 16; v[1..6] = rdna4d2_stat2_regs[] two per scalar.
template <class E>
inline uint32_t status2(E &e, uint64_t *v, uint32_t inst = RDNA4D2_INST_MONA)
{
    const rdna4d2_stat_reg *tbl = rdna4d2_stat2_tbl(inst);
    for (unsigned i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)RDNA4D2_OK | ((uint64_t)RDNA4D2_OP_STATUS2 << 8) | ((uint64_t)RDNA4D2_STAT2_COUNT << 16);
    for (uint32_t i = 0; i < RDNA4D2_STAT2_COUNT; i++) v[1 + i / 2u] |= (uint64_t)e.rd(tbl[i].abs) << (32u * (i & 1u));
    return RDNA4D2_OK;
}

// status3 (0.0.633): reads only. v[0] = status | op << 8 | count << 16; v[1..7] = rdna4d2_stat3_tbl(inst)[] two per scalar.
template <class E>
inline uint32_t status3(E &e, uint64_t *v, uint32_t inst = RDNA4D2_INST_MONA)
{
    const rdna4d2_stat_reg *tbl = rdna4d2_stat3_tbl(inst);
    for (unsigned i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)RDNA4D2_OK | ((uint64_t)RDNA4D2_OP_STATUS3 << 8) | ((uint64_t)RDNA4D2_STAT3_COUNT << 16);
    for (uint32_t i = 0; i < RDNA4D2_STAT3_COUNT; i++) v[1 + i / 2u] |= (uint64_t)e.rd(tbl[i].abs) << (32u * (i & 1u));
    return RDNA4D2_OK;
}

// =====================================================================================================================================================================================================
// 0.0.635 (multi-monitor stages M4c / M4d): ops 8..12, the monitor B's plane. INSTANCE 2 ONLY. The order of the plane op (secondary plane bring-up):
//    0  the stage the op needs (no register is read);   1  the admission of the two buffers and EVERY list of the op judged by the guard (a refusal reads and writes nothing; op 8 gives the buffers back);
//    2  the DP side before (fifteen reads + the six) and proof that OTG0 and OTG2 are counting;   3  the prechecks (EXPECT pages, the equal pairs, neither HUBP0's primary NOR its EARLIEST_INUSE overlaps A or B);   4  the buffers drawn (op 8) and read back at 24 points each;
//  0.0.636, op 8 in Linux's order (clocks on BEFORE the programming; the 0.0.635 run showed DSCL2_LB_MEMORY_CTRL does not take with HUBP2 / DPP2 unclocked); 0.0.638 changes in the marked steps:
//    5  part A: DPPCLK2 DTO + DPPCLK_CTRL bit 6 + DPP_TOP2 clock, BLANK_EN = 1 READ BACK (the only pre-clock gate), pitch, address HIGH / LOW (A);   6  part B (0.0.639): the HUBP clock (a bare update), four RECORDED reads ~1 ms later + DET2_CTRL recorded (no gate);
//    7  part D: the 20 COPY0 pairs (DSCL2, HUBP2, HUBPREQ2), then the address HIGH / LOW AGAIN;   8  2 OTG2 frames (the double-buffered fields latch at OTG2's update), then the second recorded sample;   9  the READ-BACK of every register of parts A and D;
//   10  (0.0.643) the LOCK: OTG2's master update lock (Linux optc3_lock: select = 2, lock = 1, wait UPDATE_LOCK_STATUS = 1); there is NO FLIP_PENDING gate any more (a blanked HUBP never consumes a pending address);
//   11  (0.0.643) part C INSIDE the lock, back to back: address HIGH / LOW (A), MPCC2, MPC_OUT2_MUX, UNBLANK LAST; then the UNLOCK;   12  the post gates AFTER the unlock: FLIP_PENDING == 0 and EARLIEST_INUSE == A (polled up to 5 frames), (0.0.639) part E: DET2 current = 3 (100 x 1 ms), 3 frames, the health gate (underflow / TIMEOUT / ODM2 RECORDED only);
//       the lock is released on EVERY exit path (before any rollback).   13  the DP side after.
//    Any failure after part A began runs the ROLLBACK (the list of op 12) and the buffers are freed ONLY if HUBP2's clock reads off. Ops 9..11 never un-program the plane themselves (they roll back only on a DP disturbance).
// =====================================================================================================================================================================================================

// Frames on one OTG: its 24-bit frame counter must advance by n. Bounded: 40 x 1 ms per frame.
template <class E>
inline bool wait_frames(E &e, uint32_t reg, uint32_t n)
{
    const uint32_t start = e.rd(reg) & 0xFFFFFFu;
    for (uint32_t i = 0; i < 40u * n; i++) {
        e.sleep_ms(1u);
        if ((((e.rd(reg) & 0xFFFFFFu) - start) & 0xFFFFFFu) >= n) return true;
    }
    return false;
}

// The buffers' admission (pure arithmetic, NO register read): both 64 KiB aligned, inside the scanout window, each one HIGH dword, distinct and disjoint. 0.0.655: per instance (the monitor A's buffers are 8,323,072 bytes, the monitor B's 14,745,600), and neither
// buffer may overlap the OTHER instance's live pair (a pinned monitor B pair above all): the monitor A never gets a buffer the monitor B's HUBP2 reads, and the reverse.
template <class E>
inline bool plane_admit(E &e, uint32_t inst, const rdna4d2_plane *pl)
{
    uint64_t lo = 0u, hi = 0u;
    const uint64_t sz = rdna4d2_surf_get(inst)->buf_bytes;
    if (!e.window(&lo, &hi) || hi == 0u) return false;
    for (uint32_t k = 0; k < 2u; k++) {
        const uint64_t m = pl->mc[k];
        if (m == 0u || (m & 0xFFFFull) != 0u || m < lo || m >= hi || hi - m < sz) return false;
        if (((m + sz - 1u) >> 32) != (m >> 32)) return false;
    }
    const uint64_t a = pl->mc[0], b = pl->mc[1];
    if ((a >> 32) != (b >> 32)) return false;
    if ((a < b ? b - a : a - b) < sz) return false;
    {   // the other instance's pair (any stage that still owns buffers)
        const uint32_t oi = inst == RDNA4D2_INST_MONA ? RDNA4D2_INST_MONB : RDNA4D2_INST_MONA;
        const rdna4d2_plane *const op = e.pl_of(oi);
        const uint64_t osz = rdna4d2_surf_get(oi)->buf_bytes;
        if (op->stage != RDNA4D2_PL_NONE && op->stage != RDNA4D2_PL_FREE)
            for (uint32_t k = 0; k < 2u; k++) for (uint32_t j = 0; j < 2u; j++) {
                const uint64_t x = pl->mc[k], y = op->mc[j];
                if (y != 0u && x < y + osz && y < x + sz) return false;
            }
    }
    return true;
}

// Draw A and B (one run per bar per row) and verify them: HDP flush, then 24 points per buffer through the MM window (the GPU's view). false = refused (nothing was written to a register). 0.0.655: per instance (240-px bars over 1080 rows for the monitor A).
template <class E>
inline bool plane_fill_verify(E &e, uint32_t inst)
{
    const rdna4d2_surf *const sf = rdna4d2_surf_get(inst);
    for (uint32_t k = 0; k < 2u; k++)
        for (uint32_t row = 0; row < sf->h; row++)
            for (uint32_t bar = 0; bar < 8u; bar++)
                if (!e.vfill(k, row * sf->pitch_bytes + bar * (sf->bar_px * 4u), rdna4d2_bar_color(k, bar), sf->bar_px * 4u)) return false;
    e.vflush();
    for (uint32_t k = 0; k < 2u; k++)
        for (uint32_t p = 0; p < RDNA4D2_VPOINTS; p++) {
            uint32_t want = 0u, got = 0u;
            const uint32_t off = rdna4d2_vpoint_i(inst, k, p, &want);
            if (!e.vread(k, off, &got) || got != want) return false;
        }
    return true;
}

// 0.0.643: NOT CALLED any more (Linux does not gate on FLIP_PENDING before the unblank: a blanked HUBP never consumes the pending address; the address, MPCC and unblank go in ONE update lock). Kept for the record.
// 0.0.638: the PRE-UNBLANK gate: FLIP_PENDING == 0 and nothing else (a blanked HUBP does not fetch, so EARLIEST_INUSE is not judged before the unblank; an unblank with a flip still pending would let HUBP2 fetch its OLD address). Polls for up
// to `frames` OTG frames (bounded: 40 x 1 ms per frame, one read per millisecond). true = no flip pending. g[RDNA4D2_GT_FLIP_CTL] holds the last value read (the caller reports it when this returns false).
template <class E>
inline bool pending_gate(E &e, uint32_t reg, uint32_t frames, uint32_t *g)
{
    const uint32_t start = e.rd(reg) & 0xFFFFFFu;
    for (uint32_t i = 0; i <= 40u * frames; i++) {
        g[RDNA4D2_GT_FLIP_CTL] = e.rd(RDNA4D2_HUBPREQ2_DCSURF_FLIP_CONTROL);
        if ((g[RDNA4D2_GT_FLIP_CTL] & RDNA4D2_FLIP_PENDING_MASK) == 0u) return true;
        if ((((e.rd(reg) & 0xFFFFFFu) - start) & 0xFFFFFFu) >= frames) return false;
        e.sleep_ms(1u);
    }
    return false;
}

// 0.0.636: the latch gate (Linux's hubp2_is_flip_pending: FLIP_PENDING set, or EARLIEST_INUSE != the requested address, is still pending). Reads the gate dwords, judges ONLY the latch bits, and polls for up to `frames` OTG frames
// (bounded: 40 x 1 ms per frame, one read of the gates per millisecond). true = latched. The gate dwords stay filled for the report. 0.0.638: op 8 runs it AFTER the unblank (a blanked HUBP does not update EARLIEST_INUSE).
template <class E>
inline bool latch_gate(E &e, uint32_t inst, uint32_t reg, uint32_t frames, uint64_t wantMc, uint32_t *g)
{
    const rdna4d2_pr *const pr = rdna4d2_pr_get(inst);
    const uint32_t start = e.rd(reg) & 0xFFFFFFu;
    for (uint32_t i = 0; i <= 40u * frames; i++) {
        g[RDNA4D2_GT_FLIP_CTL] = e.rd(pr->flip_ctl);
        g[RDNA4D2_GT_EARLY_LO] = e.rd(pr->early_lo); g[RDNA4D2_GT_EARLY_HI] = e.rd(pr->early_hi);
        g[RDNA4D2_GT_HUBP_CNTL] = 0u; g[RDNA4D2_GT_ODM2] = 0u; g[RDNA4D2_GT_VMFAULT] = 0u; g[RDNA4D2_GT_DET2] = 3u << 8;     // only the latch bits are judged here
        if ((rdna4d2_health(g, 0u, wantMc, 1) & RDNA4D2_H_LATCH) == 0u) return true;
        if ((((e.rd(reg) & 0xFFFFFFu) - start) & 0xFFFFFFu) >= frames) return false;
        e.sleep_ms(1u);
    }
    return false;
}

// 0.0.638: a list boundary for the timed-out-WAIT record (the first timed-out WAIT of the list: its register and the LAST value read), and its report as the gate that failed (pre_abs / pre_val / pre_wait).
inline void wait_reset(rdna4d2_out &o) { o.wfail = 0u; o.wait_abs = 0u; o.wait_val = 0u; }
inline void wait_gate(rdna4d2_out &o) { o.pre_abs = o.wait_abs; o.pre_val = o.wait_val; o.pre_wait = 1u; }

// Judge one list with the instance's guard. false: the status is GUARD_REFUSED (or the list did not fit) and nothing has been read.
inline bool plane_judge(rdna4d2_out &o, uint32_t inst, const rdna4d2_step *s, uint32_t n)
{
    uint32_t bad = 0u;
    if (n == 0u || rdna4d2_guard_list_i(inst, s, n, &bad) != 0u) {
        o.status = RDNA4D2_GUARD_REFUSED; o.fail = bad < n ? bad : 0xFFu;
        o.guard = bad < n ? rdna4d2_guard_write_i(inst, s[bad].abs, s[bad].val, 0) : 0u;
        return false;
    }
    return true;
}
template <class E>
inline bool plane_judge_all(E &e, uint32_t inst, uint32_t op, uint32_t bufIdx, rdna4d2_out &o, const rdna4d2_plane *pl)
{
    rdna4d2_step *const s = e.buf(0u);
    bool ok = true;
    if (op == RDNA4D2_OP_PLANE) {
        for (uint32_t p = 0; p < rdna4d2_pre_pages(inst) && ok; p++) ok = plane_judge(o, inst, s, rdna4d2_build_pre_i(inst, p, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_a_i(inst, s, pl->mc[0]));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_b_i(inst, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_d_i(inst, s, pl->mc[0]));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_rec2_i(inst, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_lock_i(inst, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_c_i(inst, s, pl->mc[0]));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_unlock_i(inst, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_plane_e_i(inst, s));
    } else if (op == RDNA4D2_OP_SHOW) {
        ok = plane_judge(o, inst, s, rdna4d2_build_show1_i(inst, s));
        if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_show2_i(inst, s));
    } else if (op == RDNA4D2_OP_FLIP) {
        ok = plane_judge(o, inst, s, rdna4d2_build_flip_i(inst, s, pl->mc[bufIdx & 1u]));
    } else if (op == RDNA4D2_OP_CRC) {
        ok = plane_judge(o, inst, s, rdna4d2_build_crc_i(inst, s));
    }
    if (ok) ok = plane_judge(o, inst, s, rdna4d2_build_planeoff_i(inst, s));       // the rollback list, for every op
    return ok;
}

// Nothing was ever programmed (op 8 failed before its first write): the buffers may go back at once.
inline void plane_clean(uint32_t inst, rdna4d2_plane *pl, rdna4d2_out &o)
{
    pl->stage = RDNA4D2_PL_FREE; o.free_bufs = 1u;
    rdna4d2_aset_clear_i(inst);
}

// THE ROLLBACK (op 12, every failure of op 8 after its first write, and a DP disturbance in ops 9..11): the planeoff list, best effort (a refused write is skipped and counted). The buffers are released ONLY when HUBP2's clock reads off AND
// neither the NO_OUTSTANDING_REQ wait nor the clock-gated wait timed out; otherwise they are LEAKED (the engine may still read them) and the stage is LEAKED.
template <class E>
inline void plane_rollback(E &e, uint32_t inst, rdna4d2_out &o, rdna4d2_plane *pl)
{
    rdna4d2_step *const s = e.buf(1u);
    const rdna4d2_pr *const pr = rdna4d2_pr_get(inst);
    const uint32_t n = rdna4d2_build_planeoff_i(inst, s);
    rdna4d2_out sc;
    for (unsigned i = 0; i < sizeof(sc); i++) ((uint8_t *)&sc)[i] = 0;
    uint32_t bad = 0u;
    bool ran = false;
    if (rdna4d2_guard_list_i(inst, s, n, &bad) == 0u) { (void)run_steps(e, inst, s, n, true, sc, nullptr); ran = true; }
    o.auto_off = 1u; o.timeouts += sc.timeouts;
    if (sc.wait_abs != 0u) { uint32_t *const rec = e.rec_buf(); rec[RDNA4D2_RC_RB_ABS] = sc.wait_abs; rec[RDNA4D2_RC_RB_VAL] = sc.wait_val; rec[RDNA4D2_RC_VALID] |= RDNA4D2_RV_RB; }       // 0.0.638: the rollback's first timed-out WAIT: register + last value
    const bool clkOff = (e.rd(pr->hubp_clk) & 1u) == 0u;
    const bool outOk = (sc.wfail & (1ull << RDNA4D2_PO_I_NOOUT)) == 0u, clkOk = (sc.wfail & (1ull << RDNA4D2_PO_I_CLKOFF)) == 0u;
    if (!outOk) { uint32_t *const rec = e.rec_buf(); rec[RDNA4D2_RC_VALID] |= RDNA4D2_RV_ADDRKEPT; }       // 0.0.639: NO_OUTSTANDING_REQ timed out: the address registers were left alone (run_steps), the buffers are leaked
    o.wfail = sc.wfail;
    o.free_bufs = (ran && clkOff && outOk && clkOk) ? 1u : 0u;
    pl->stage = o.free_bufs ? (uint32_t)RDNA4D2_PL_FREE : (uint32_t)RDNA4D2_PL_LEAKED;
    rdna4d2_aset_clear_i(inst);
}

// Every write of a list read back as written: SET and COPY0 whole, UPDATE on its mask. The first mismatch is reported (pre_abs / pre_val / fail).
template <class E>
inline bool plane_readback(E &e, const rdna4d2_step *s, uint32_t n, rdna4d2_out &o)
{
    for (uint32_t i = 0; i < n; i++) {
        const rdna4d2_step &st = s[i];
        if (!rdna4d2_is_write(&st)) continue;
        const uint32_t r = e.rd(st.abs), m = st.kind == RDNA4D2_K_UPD ? st.mask : st.kind == RDNA4D2_K_COPY0 ? st.cond_mask : 0xFFFFFFFFu;      // 0.0.637: a COPY0 reads back its write mask
        if ((r & m) != (st.val & m)) { o.fail = i; o.pre_abs = st.abs; o.pre_val = r; e.note(i, st, r, st.val, RDNA4D2_N_REFUSED); return false; }
    }
    return true;
}

// The final gate reads (reads only): taken at the END of every op that got past its stage / guard checks.
template <class E>
inline void plane_gates(E &e, uint32_t inst, const rdna4d2_plane *pl, uint32_t *g, const rdna4d2_out &o)
{
    const rdna4d2_pr *const pr = rdna4d2_pr_get(inst);
    g[RDNA4D2_GT_HUBP_CNTL] = e.rd(pr->hubp_cntl);
    g[RDNA4D2_GT_HUBP_CLK] = e.rd(pr->hubp_clk);
    g[RDNA4D2_GT_ODM2] = e.rd(pr->odm_ctl);
    g[RDNA4D2_GT_VMFAULT] = e.rd(RDNA4D2_DCN_VM_FAULT_STATUS);
    g[RDNA4D2_GT_DET2] = e.rd(pr->det_ctrl);
    g[RDNA4D2_GT_EARLY_LO] = e.rd(pr->early_lo);
    g[RDNA4D2_GT_EARLY_HI] = e.rd(pr->early_hi);
    g[RDNA4D2_GT_FLIP_CTL] = e.rd(pr->flip_ctl);
    g[RDNA4D2_GT_MPCC_STATUS] = e.rd(pr->mpcc_status);
    g[RDNA4D2_GT_MPC_MUX] = e.rd(pr->mpc_mux);
    g[RDNA4D2_GT_A_LO] = (uint32_t)pl->mc[0]; g[RDNA4D2_GT_A_HI] = (uint32_t)(pl->mc[0] >> 32);
    g[RDNA4D2_GT_B_LO] = (uint32_t)pl->mc[1]; g[RDNA4D2_GT_B_HI] = (uint32_t)(pl->mc[1] >> 32);
    g[RDNA4D2_GT_FIFO] = e.rd(pr->fifo);
    g[RDNA4D2_GT_META] = (pl->stage & 0xFFu) | ((o.free_bufs & 1u) << 8) | ((pl->cur & 1u) << 12) | ((pl->odm_sticky & 0xFFFFu) << 16) | ((o.w2_changed & 0x1FFu) << 16);      // 0.0.655: bits 16..24 = the monitor B watch mask of an monitor A op (the sticky bits sit at 26 / 29)
}

// 0.0.643: the OTG's update lock (0.0.655: of the instance) released on an exit path (best effort: judged by the guard like every list; a refused write is skipped). NOT inlined: its rdna4d2_out scratch must not grow plane_exec's frame.
template <class E>
__attribute__((noinline)) inline void plane_unlock_best_effort(E &e, uint32_t inst)
{
    rdna4d2_step *const us = e.buf(1u);
    const uint32_t un = rdna4d2_build_plane_unlock_i(inst, us);
    rdna4d2_out sc;
    for (unsigned i = 0; i < sizeof(sc); i++) ((uint8_t *)&sc)[i] = 0;
    uint32_t ubad = 0u;
    if (rdna4d2_guard_list_i(inst, us, un, &ubad) == 0u) (void)run_steps(e, inst, us, un, true, sc, nullptr);
}

// 0.0.652 (M5), op 14 `fbhold 2`: READS ONLY (the gates), then the pin and the stage. The plane must be SHOWN (the caller checked the stage), buffer A the front buffer, nothing pending, no underflow, no VM fault: rdna4d2_hold_verdict. A refusal leaves the
// stage SHOWN (nothing happened). On success the allocator pins the pair (e.pin(): n1c_d2_pin, which also checks it holds exactly A and B), THEN the stage becomes HELD; from then on no op moves or frees the plane and the address set is empty.
// IRREVERSIBLE this boot. Writes NO register (the guard / allowlist are never asked: there is no list).
template <class E>
inline bool plane_hold(E &e, uint32_t inst, rdna4d2_plane *pl, uint32_t *g, rdna4d2_out &o)
{
    plane_gates(e, inst, pl, g, o);
    const uint32_t bad = rdna4d2_hold_verdict(g, pl->cur, pl->mc[0]);
    if (bad != 0u) { o.pre_abs = rdna4d2_pr_get(inst)->hubp_cntl; o.pre_val = bad; o.status = RDNA4D2_GATE; return true; }
    if (!e.pin()) { o.status = RDNA4D2_NO_BUFFER; return true; }
    pl->stage = RDNA4D2_PL_HELD;
    rdna4d2_aset_clear_i(inst);
    e.rec_buf()[RDNA4D2_RC_VALID] |= RDNA4D2_RV_HELD;
    g[RDNA4D2_GT_META] = (g[RDNA4D2_GT_META] & ~0xFFu) | (uint32_t)RDNA4D2_PL_HELD;
    o.status = RDNA4D2_OK;
    return true;
}

// The plane ops. Returns true when it got past the stage / admission / guard checks (the gates are then read by the caller).
template <class E>
inline bool plane_exec(E &e, uint32_t inst, uint32_t op, uint32_t bufIdx, rdna4d2_out &o, uint32_t *g)
{
    rdna4d2_plane *const pl = e.pl_of(inst);
    const rdna4d2_pr *const pr = rdna4d2_pr_get(inst);
    const uint32_t stg = pl->stage;
    // ---- 0. (0.0.652) a HELD plane refuses every op that could move it (rdna4d2_held_refuses); then the stage this op needs: no register is read
    if (stg == RDNA4D2_PL_HELD && rdna4d2_held_refuses(op, inst)) { o.status = RDNA4D2_HELD; return false; }
    const bool stOk = op == RDNA4D2_OP_PLANE ? stg == RDNA4D2_PL_ALLOC : op == RDNA4D2_OP_SHOW ? stg == RDNA4D2_PL_PLANE : op == RDNA4D2_OP_FLIP ? (stg == RDNA4D2_PL_PLANE || stg == RDNA4D2_PL_SHOWN) : op == RDNA4D2_OP_FBHOLD ? stg == RDNA4D2_PL_SHOWN : true;
    if (!stOk) { o.status = RDNA4D2_PLANE_STATE; return false; }
    if (stg == RDNA4D2_PL_ALLOC || stg == RDNA4D2_PL_PLANE || stg == RDNA4D2_PL_SHOWN) rdna4d2_aset_set_i(inst, pl->mc[0], pl->mc[1]); else rdna4d2_aset_clear_i(inst);
    if (op == RDNA4D2_OP_FBHOLD) return plane_hold(e, inst, pl, g, o);
    // ---- 1. admission (op 8) and the guard over EVERY list of this op: a refusal reads and writes nothing
    if (op == RDNA4D2_OP_PLANE && !plane_admit(e, inst, pl)) { o.status = RDNA4D2_NO_BUFFER; plane_clean(inst, pl, o); return false; }
    if (!plane_judge_all(e, inst, op, bufIdx, o, pl)) { if (op == RDNA4D2_OP_PLANE) plane_clean(inst, pl, o); return false; }
    rdna4d2_step *const s = e.buf(0u);
    const rdna4d2_inst &d = *rdna4d2_inst_get(inst);
    const uint32_t fc2 = d.otg_frame;       // the instance's OTG frame counter (OTG2's for the monitor B, OTG1's for the monitor A)
    // ---- 2. the DP side before; planeoff at stage NONE with HUBP2's clock off has nothing to do (reads only; status OK since 0.0.636)
    if (op == RDNA4D2_OP_PLANEOFF && stg == RDNA4D2_PL_NONE && (e.rd(pr->hubp_clk) & 1u) == 0u) { o.status = RDNA4D2_OK; plane_gates(e, inst, pl, g, o); return true; }     // 0.0.636: 'nothing to do' is OK (a runner may always call planeoff); the gates still report FLIP_CONTROL (FLIP_PENDING) and the rest
    dp_snapshot(e, o.dp_before);
    uint32_t *const pxB = e.dpx_buf(0u), *const pxA = e.dpx_buf(1u);
    dpx_snapshot(e, pxB);
    uint32_t *const w2B = e.w2_buf(0u), *const w2A = e.w2_buf(1u);
    if (inst == RDNA4D2_INST_MONA) w2_snapshot(e, w2B);      // 0.0.655: the monitor B watch before an monitor A op
    o.symclkb_a = e.rd(RDNA4D2_SYMCLKB_CLOCK_ENABLE);
    o.f0a = e.rd(RDNA4D2_OTG0_FRAME_COUNT); o.f1a = e.rd(fc2);
    uint32_t fPre = o.f0a, fPre1 = o.f1a;
    uint32_t st = RDNA4D2_OK;
    bool wrote = false;
    bool locked = false;       // 0.0.643: OTG2's master update lock is (possibly) held: every exit path of op 8 releases it below, BEFORE any rollback
    do {
        if (op != RDNA4D2_OP_PLANEOFF) {
            e.sleep_ms(RDNA4D2_SETTLE_MS);
            fPre = e.rd(RDNA4D2_OTG0_FRAME_COUNT); fPre1 = e.rd(fc2);
            if (fPre == o.f0a) { o.f0b = fPre; st = RDNA4D2_DP_NOT_COUNTING; break; }
            if (fPre1 == o.f1a) { o.pre_abs = fc2; o.pre_val = fPre1; st = RDNA4D2_PRECHECK; break; }       // the instance's OTG is not counting: no frame wait could ever complete
            if (inst == RDNA4D2_INST_MONA) w2B[11] = e.rd(RDNA4D2_OTG2_FRAME_COUNT);      // 0.0.655: was OTG2 (the monitor B) counting across the settle?
        }
        // ---- 3. the prechecks: reads only
        if (op == RDNA4D2_OP_PLANE) {
            uint32_t refusals = 0u;
            for (uint32_t p = 0; p < rdna4d2_pre_pages(inst) && st == RDNA4D2_OK; p++) { const uint32_t n = rdna4d2_build_pre_i(inst, p, s); st = run_steps(e, inst, s, n, false, o, &refusals); }
            if (st != RDNA4D2_OK) break;
            if (inst == RDNA4D2_INST_MONB) for (uint32_t i = 0; i < RDNA4D2_EQ_COUNT; i++) {      // 0.0.655: the monitor B's pipe-0 twin comparison; the monitor A's rows are literal EXPECT census values (rdna4d1_exp)
                const uint32_t a2 = e.rd(rdna4d2_eqpairs[i].a2), a0 = e.rd(rdna4d2_eqpairs[i].a0);
                if (a2 != a0) { o.pre_abs = rdna4d2_eqpairs[i].a2; o.pre_val = a2; o.fail = i; st = RDNA4D2_PRECHECK; break; }
            }
            if (st != RDNA4D2_OK) break;
            const uint64_t h0 = ((uint64_t)e.rd(RDNA4D2_HUBPREQ0_PRIMARY_HIGH) << 32) | e.rd(RDNA4D2_HUBPREQ0_PRIMARY_LOW);
            const uint64_t i0 = ((uint64_t)(e.rd(RDNA4D2_HUBPREQ0_EARLIEST_HIGH) & 0xFFFFu) << 32) | e.rd(RDNA4D2_HUBPREQ0_EARLIEST_LOW);
            pl->dp_bytes = rdna4d2_dp_surface_bytes(e.rd(RDNA4D2_HUBPREQ0_SURFACE_PITCH), e.rd(RDNA4D2_HUBP0_PRI_VIEWPORT_DIMENSION));      // 0.0.637 (C3): the DP surface may be larger than ours: max(ours, pitch x height), 64 KiB; 64 MiB when the reads are implausible
            const uint64_t bsz = rdna4d2_surf_get(inst)->buf_bytes;
            if (rdna4d2_surface_overlaps_n(h0, pl->mc[0], pl->dp_bytes, bsz) || rdna4d2_surface_overlaps_n(h0, pl->mc[1], pl->dp_bytes, bsz)) { o.pre_abs = RDNA4D2_HUBPREQ0_PRIMARY_LOW; o.pre_val = (uint32_t)h0; st = RDNA4D2_PRECHECK; break; }       // a buffer may never overlap the DP's live surface (0.0.636: overlap, not equality)
            if (rdna4d2_surface_overlaps_n(i0, pl->mc[0], pl->dp_bytes, bsz) || rdna4d2_surface_overlaps_n(i0, pl->mc[1], pl->dp_bytes, bsz)) { o.pre_abs = RDNA4D2_HUBPREQ0_EARLIEST_LOW; o.pre_val = (uint32_t)i0; st = RDNA4D2_PRECHECK; break; }     // ... nor HUBP0's EARLIEST_INUSE (the surface it is scanning NOW)
            pl->odm_sticky = e.rd(pr->odm_ctl) & RDNA4D2_ODM_UNDERFLOW_MASK;
            if (!plane_fill_verify(e, inst)) { st = RDNA4D2_NO_BUFFER; break; }                     // the only VRAM write; nothing has touched a register
            // ---- 5. part A (0.0.636, Linux's order): the DPP clocks, BLANK_EN = 1 READ BACK (the ONLY pre-clock gate), pitch, address HIGH / LOW (A). HUBP2 fetches nothing: it is blanked and its clock is still off.
            wrote = true;
            uint32_t n = rdna4d2_build_plane_a_i(inst, s, pl->mc[0]);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if ((o.wfail & (1ull << RDNA4D2_PA_I_BLANKWAIT)) != 0u) { o.fail = RDNA4D2_PA_I_BLANKWAIT; wait_gate(o); st = RDNA4D2_GATE; break; }      // BLANK_EN did not read 1: the HUBP clock is NOT enabled
            // ---- 6. part B (0.0.639): the HUBP clock (a bare update, as Linux's), four RECORDED reads ~1 ms later and a fifth of DET2_CTRL (all no gate). NO wait on the clock status bits (demand-gated: the 0.0.637 run timed out on them) and NO DET2 wait here (0.0.638 run: DET2 current stays 0 while blanked and unconnected; the wait is step 11)
            n = rdna4d2_build_plane_b_i(inst, s);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            // ---- 7. part D: the 20 COPY0 pairs with the clocks ON, then the address HIGH / LOW again
            n = rdna4d2_build_plane_d_i(inst, s, pl->mc[0]);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            // ---- 8. two OTG2 frames: the double-buffered fields latch at OTG2's update
            if (!wait_frames(e, fc2, 2u)) { o.pre_abs = fc2; st = RDNA4D2_GATE; break; }
            n = rdna4d2_build_plane_rec2_i(inst, s);      // 0.0.638: the second RECORDED sample (no gate)
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            // ---- 9. the READ-BACK of every register of parts A and D, AFTER the frame wait and BEFORE the pending gate
            n = rdna4d2_build_plane_a_i(inst, s, pl->mc[0]);
            if (!plane_readback(e, s, n, o)) { st = RDNA4D2_READBACK; break; }
            n = rdna4d2_build_plane_d_i(inst, s, pl->mc[0]);
            if (!plane_readback(e, s, n, o)) { st = RDNA4D2_READBACK; break; }
            // ---- 10. (0.0.643) the LOCK: Linux's optc3_lock for OTG2 (select = 2, lock = 1, wait UPDATE_LOCK_STATUS = 1). NO FLIP_PENDING gate here: a blanked HUBP never consumes a pending address (the 0.0.639 run), Linux takes the address,
            //          the MPCC insert and the unblank inside ONE lock and checks the flip only afterwards. `locked` is set BEFORE the list runs (the lock write may land even if the list stops): the common exit below releases it.
            locked = true;
            n = rdna4d2_build_plane_lock_i(inst, s);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if ((o.wfail & (1ull << RDNA4D2_LK_I_WAIT)) != 0u) { o.fail = RDNA4D2_LK_I_WAIT; wait_gate(o); st = RDNA4D2_GATE; break; }      // the lock never asserted: NOTHING (address, MPCC2, unblank) has been written inside it
            // ---- 11. part C INSIDE the lock, ONE list back to back: address HIGH / LOW (A), MPCC2 insert, the mux, UNBLANK (LAST); then the UNLOCK at once (optc1_unlock): the three take effect together at the next VUPDATE
            n = rdna4d2_build_plane_c_i(inst, s, pl->mc[0]);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            n = rdna4d2_build_plane_unlock_i(inst, s);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            locked = false;
            // ---- 12. the POST gates, AFTER the unlock: FLIP_PENDING == 0 AND EARLIEST_INUSE == A (polled up to 5 frames)
            if (!latch_gate(e, inst, fc2, 5u, pl->mc[0], g)) { o.pre_abs = pr->early_lo; o.pre_val = g[RDNA4D2_GT_EARLY_LO]; o.pre_wait = 1u; st = RDNA4D2_GATE; break; }
            // 0.0.639: DET2 current = 3, AFTER the unblank and the commit (the DET is allocated at the first VUPDATE; Linux waits for it only once the pipe is enabled), 100 x 1 ms
            n = rdna4d2_build_plane_e_i(inst, s);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if ((o.wfail & (1ull << RDNA4D2_PE_I_DETWAIT)) != 0u) { o.fail = RDNA4D2_PE_I_DETWAIT; wait_gate(o); st = RDNA4D2_GATE; break; }
            if (!wait_frames(e, fc2, 3u)) { o.pre_abs = fc2; st = RDNA4D2_GATE; break; }
            plane_gates(e, inst, pl, g, o);
            // 0.0.639: underflow, TIMEOUT and ODM2 underflow are RECORDED here, not gated (DPG2 still drives the monitor B's output until op 9, which clears them and requires 0 after 10 frames); VM fault, SEG_ALLOC_ERR, DET2 != 3 and the latch stay gates
            const uint32_t bad = rdna4d2_health(g, pl->odm_sticky, pl->mc[0], 1);
            { uint32_t *const rec = e.rec_buf(); rec[RDNA4D2_RC_UND_HUBP] = g[RDNA4D2_GT_HUBP_CNTL]; rec[RDNA4D2_RC_UND_ODM] = g[RDNA4D2_GT_ODM2]; rec[RDNA4D2_RC_VALID] |= RDNA4D2_RV_UND; }
            const uint32_t gate = bad & ~(uint32_t)(RDNA4D2_H_UNDERFLOW | RDNA4D2_H_TIMEOUT | RDNA4D2_H_ODM);
            if (gate != 0u) { o.pre_abs = pr->hubp_cntl; o.pre_val = gate; st = RDNA4D2_GATE; break; }
            pl->stage = RDNA4D2_PL_PLANE; pl->cur = 0u;
        } else if (op == RDNA4D2_OP_SHOW) {
            uint32_t refusals = 0u;
            wrote = true;
            uint32_t n = rdna4d2_build_show1_i(inst, s);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if (!wait_frames(e, fc2, 10u)) { o.pre_abs = fc2; st = RDNA4D2_GATE; break; }
            plane_gates(e, inst, pl, g, o);
            const uint32_t bad = rdna4d2_health(g, 0u, pl->mc[pl->cur & 1u], 1);
            if (bad != 0u) { o.pre_abs = pr->hubp_cntl; o.pre_val = bad; st = RDNA4D2_GATE; break; }      // the DPG stays ON: the pattern covers whatever the plane does
            n = rdna4d2_build_show2_i(inst, s);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if ((o.wfail & (1ull << RDNA4D2_SHOW2_I_WAIT)) != 0u) { o.fail = RDNA4D2_SHOW2_I_WAIT; wait_gate(o); st = RDNA4D2_GATE; break; }
            pl->stage = RDNA4D2_PL_SHOWN;
        } else if (op == RDNA4D2_OP_FLIP) {
            uint32_t refusals = 0u;
            wrote = true;
            const uint32_t n = rdna4d2_build_flip_i(inst, s, pl->mc[bufIdx & 1u]);
            wait_reset(o);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if ((o.wfail & (1ull << RDNA4D2_FLIP_I_WAIT)) != 0u) { o.fail = RDNA4D2_FLIP_I_WAIT; wait_gate(o); st = RDNA4D2_GATE; break; }      // FLIP_PENDING never cleared
            if ((o.wfail & (1ull << RDNA4D2_FLIP_I_WAIT_LO)) != 0u || (o.wfail & (1ull << RDNA4D2_FLIP_I_WAIT_HI)) != 0u) { o.fail = (o.wfail & (1ull << RDNA4D2_FLIP_I_WAIT_LO)) != 0u ? RDNA4D2_FLIP_I_WAIT_LO : RDNA4D2_FLIP_I_WAIT_HI; wait_gate(o); st = RDNA4D2_GATE; break; }      // 0.0.636 (C1): EARLIEST_INUSE never reached the requested address (hubp2_is_flip_pending)
            plane_gates(e, inst, pl, g, o);
            const uint32_t bad = rdna4d2_health(g, 0u, pl->mc[bufIdx & 1u], 1);
            if ((bad & RDNA4D2_H_LATCH) != 0u) { o.pre_abs = pr->early_lo; o.pre_val = g[RDNA4D2_GT_EARLY_LO]; st = RDNA4D2_GATE; break; }
            pl->cur = bufIdx & 1u;
        } else if (op == RDNA4D2_OP_CRC) {
            uint32_t refusals = 0u;
            wrote = true;
            const uint32_t n = rdna4d2_build_crc_i(inst, s);
            st = run_steps(e, inst, s, n, false, o, &refusals);
            if (st != RDNA4D2_OK) break;
            if (!wait_frames(e, fc2, 3u)) { o.pre_abs = fc2; st = RDNA4D2_GATE; break; }
            g[RDNA4D2_GT_CRC_RG] = e.rd(pr->crc_rg); g[RDNA4D2_GT_CRC_B] = e.rd(pr->crc_b);
        } else {       // RDNA4D2_OP_PLANEOFF: the rollback below is the whole op
            wrote = true;
        }
    } while (false);

    // ---- 0.0.638: the first timed-out WAIT of the last list run (register + the LAST value read) goes to the record page
    if (o.wait_abs != 0u) { uint32_t *const rec = e.rec_buf(); rec[RDNA4D2_RC_WAIT_ABS] = o.wait_abs; rec[RDNA4D2_RC_WAIT_VAL] = o.wait_val; rec[RDNA4D2_RC_VALID] |= RDNA4D2_RV_WAIT; }
    // ---- 0.0.643: OTG2's update lock is released on EVERY exit path that may hold it, BEFORE the rollback (best effort: a refused write is skipped; the lock write is judged by the guard like any other)
    if (locked) { plane_unlock_best_effort(e, inst); locked = false; }
    // ---- the outcome of the steps. Op 8 failures: before the first write the buffers go back at once; after it the ROLLBACK runs. Ops 9..11 never un-program the plane themselves.
    const uint32_t crcRG = g[RDNA4D2_GT_CRC_RG], crcB = g[RDNA4D2_GT_CRC_B];
    if (st != RDNA4D2_OK) {
        o.status = st;
        if (op == RDNA4D2_OP_PLANE) { if (wrote) plane_rollback(e, inst, o, pl); else plane_clean(inst, pl, o); }
    } else if (op == RDNA4D2_OP_PLANEOFF) {
        plane_rollback(e, inst, o, pl);
        o.status = o.free_bufs ? (uint32_t)RDNA4D2_OK : (uint32_t)RDNA4D2_GATE;      // LEAKED: HUBP2's clock did not read off or NO_OUTSTANDING_REQ timed out
    } else {
        o.status = RDNA4D2_OK;
    }

    // ---- failed before any write (the DP-not-counting, precheck, fill and admission refusals): nothing to compare, the status stands
    if (st != RDNA4D2_OK && !wrote) { plane_gates(e, inst, pl, g, o); return true; }

    // ---- the DP side after
    e.sleep_ms(RDNA4D2_SETTLE_MS);
    o.f0b = e.rd(RDNA4D2_OTG0_FRAME_COUNT); o.f1b = e.rd(fc2);
    dp_snapshot(e, o.dp_after);
    dpx_snapshot(e, pxA);
    o.symclkb_b = e.rd(RDNA4D2_SYMCLKB_CLOCK_ENABLE);
    for (uint32_t q = 0; q < RDNA4D2_DP_REGS; q++) if (o.dp_after[q] != o.dp_before[q]) o.dp_changed |= 1u << q;
    if (o.symclkb_a != o.symclkb_b) o.dp_changed |= 0x40u;
    if (o.f0b == fPre && op != RDNA4D2_OP_PLANEOFF) o.dp_changed |= 0x80u;
    o.dpx_changed = dpx_diff(e, pxB, pxA);
    if (inst == RDNA4D2_INST_MONA) { w2_snapshot(e, w2A); o.w2_changed = rdna4d2_w2_diff(w2B, w2A); }      // 0.0.655: the monitor B watch after an monitor A op
    if (o.dpx_changed != 0u || o.dp_changed != 0u || o.w2_changed != 0u) {
        const bool programmed = pl->stage == RDNA4D2_PL_PLANE || pl->stage == RDNA4D2_PL_SHOWN || pl->stage == RDNA4D2_PL_ALLOC;
        if (op != RDNA4D2_OP_PLANEOFF && !o.auto_off && programmed && wrote) plane_rollback(e, inst, o, pl);
        o.status = RDNA4D2_DP_DISTURBED;
        if (o.w2_changed != 0u) o.guard = rdna4d2_w2_first(o.w2_changed);      // 0.0.655: the 4-bit guard field carries (first changed monitor B-watch row + 1)
    }
    plane_gates(e, inst, pl, g, o);
    if (op == RDNA4D2_OP_CRC && st == RDNA4D2_OK) { g[RDNA4D2_GT_CRC_RG] = crcRG; g[RDNA4D2_GT_CRC_B] = crcB; }
    return true;
}

// ops 8..12 (and 14 for the monitor B). The glue allocated the buffers (stage ALLOC) before it took its lock and frees them after it released it when o.free_bufs says so. 0.0.655: both instances (1 = the monitor A, 2 = the monitor B); any other instance, or an
// environment whose E::inst is not the op's instance (the guard that judges E::wr is the environment's own), is BAD_ARG with nothing read. Instance 2 is the default: every pre-0.0.655 caller is the monitor B's.
template <class E>
inline rdna4d2_out run_plane(E &e, uint32_t op, uint32_t bufIdx = 0u, uint32_t inst = RDNA4D2_INST_MONB)
{
    rdna4d2_out o;
    for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
    o.op = op; o.fail = 0xFFu; o.status = RDNA4D2_BAD_ARG; o.inst = inst;
    uint32_t *const g = e.gate_buf();
    for (uint32_t i = 0; i < RDNA4D2_GATES; i++) g[i] = 0u;
    if (inst != RDNA4D2_INST_MONA && inst != RDNA4D2_INST_MONB) return o;
    if (e.inst != inst) return o;
    if (op < RDNA4D2_OP_PLANE || (op > RDNA4D2_OP_PLANEOFF && op != RDNA4D2_OP_FBHOLD) || (op == RDNA4D2_OP_FLIP && bufIdx > 1u) ) return o;      // 0.0.652: op 14 (fbhold) joins 8..12 (0.0.658: for either instance - the instance was checked above); op 13 (planerec) is not a plane op
    o.status = RDNA4D2_OK;
    rdna4d2_plane *const pl = e.pl_of(inst);
    {   // 0.0.638: the record page: op 8 starts it afresh (the two samples and the wait records); every other plane op clears only the wait records (the samples of the last op 8 stay)
        uint32_t *const rec = e.rec_buf();
        if (op == RDNA4D2_OP_PLANE) for (uint32_t i = 0; i < RDNA4D2_REC_N; i++) rec[i] = 0u;
        else { rec[RDNA4D2_RC_VALID] &= ~(RDNA4D2_RV_WAIT | RDNA4D2_RV_RB | RDNA4D2_RV_ADDRKEPT); rec[RDNA4D2_RC_WAIT_ABS] = rec[RDNA4D2_RC_WAIT_VAL] = rec[RDNA4D2_RC_RB_ABS] = rec[RDNA4D2_RC_RB_VAL] = 0u; }
    }
    if (!plane_exec(e, inst, op, bufIdx, o, g)) {
        const rdna4d2_plane &p = *pl;     // refused before any read: the meta dword still tells the stage
        g[RDNA4D2_GT_META] = (p.stage & 0xFFu) | ((o.free_bufs & 1u) << 8) | ((p.cur & 1u) << 12);
        g[RDNA4D2_GT_A_LO] = (uint32_t)p.mc[0]; g[RDNA4D2_GT_A_HI] = (uint32_t)(p.mc[0] >> 32); g[RDNA4D2_GT_B_LO] = (uint32_t)p.mc[1]; g[RDNA4D2_GT_B_HI] = (uint32_t)(p.mc[1] >> 32);
    }
    return o;
}

// 0.0.638, op 13 `planerec`: the record page of the last plane op, READ from the file-scope buffer (no register is read, nothing is written).
template <class E>
inline uint32_t planerec(E &e, uint64_t *v)
{
    rdna4d2_pack_rec(v, e.rec_buf());
    return RDNA4D2_OK;
}

}  // namespace rdna4d2

#endif /* RDNA4_DISP2_FLOW_H */
