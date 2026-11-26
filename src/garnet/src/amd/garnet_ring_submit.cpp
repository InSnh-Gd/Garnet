// garnet_ring_submit.cpp — Native GFX ring submission, HUNG latch, G2/G4/G5
// multisession/application credit policy, and G1 hang recovery (split from native_s1c.cpp).
//
//  0.0.623 (GPU-apps G1, boot-arg rdna4-g1=1, default OFF): the hang-recovery verbs (n1c_g1_verb, the block at the end of this file). With the boot-arg ON they ALSO write:
//  kernel GFX ring dwords outside the live range (NOPs) and, after an ACKED MES unmap, the whole ring (NOPs), the CP write-back rptr / wptr words and the MQD memory
//  (cp_gfx_mqd_init); they send MES RESET / REMOVE_QUEUE / ADD_QUEUE frames for GFX pipe 0 queue 0 (the doorbell writes are mes_submit_pkt's and ring_emit's own); and they clear the HUNG latch after a verified probe. REGISTER WRITES (0.0.626 correction of the 0.0.623 text "No MMIO register write", which was wrong): none of their own, but the paths they call write registers, all in the graphics block, the VM/TLB block or HDP: the doorbells (MES and ring), GRBM_GFX_CNTL twice (cp_map_gfx_kgq_mes -> cp_dump_gfx_hqd), the VMID-8 TLB-invalidate registers (setup_bo -> flush_vmid), the CONTEXT8 registers (program_root at open and close) and the HDP flush (amdgpu_hdp_flush). Nothing in DCN, PSP or SMU.
#include "garnet_submit_internal.hpp"
#include "GarnetMetalNub.hpp"        // 0.0.612: hungLatched() writes Garnet,Ready = 0

namespace amdgpu {

static Session gSess;          // slot 0: STATIC and never freed: a selector racing a close can never touch freed memory
Session *gSlot[kMaxSessions] = { &gSess };
uint64_t gWc;
bool gWcInit;
uint64_t gEmitted;
uint64_t gKcqWc;
bool gKcqWcInit;
uint64_t gKcqEmitted;
BringupContext *gCtx;
volatile UInt32 gOpenFlag;
Hang gHang;
Hang gKcqHang;
IOLock *gCliLock, *gRingLock, *gKcqRingLock, *gHangLock, *gTlbLock, *gWaitLock;
static volatile uint64_t gIrqEpoch;
static uint8_t gIrqEvent;
static volatile UInt32 gIrqWaitLatch = rdna4native::irqwait::kLatchUnset;
static volatile uint64_t gIrqWaitSleeps, gIrqWaitWakes;
static volatile UInt32 gDccLatch;   // 0 = unset, 1 = OFF (garnet-dcc=0), 2 = ON (default)
static volatile UInt32 gHugeLatch;  // 0 = unset, 1 = OFF (garnet-hugepage=0), 2 = ON (default)
static volatile UInt32 gAsyncComputeLatch; // 0 = unset, 1 = OFF (garnet-async-compute=0), 2 = ON (default)

bool dcc_on() {
    UInt32 l = gDccLatch;
    if (l == 0u) {
        uint32_t v = 1u;
        const bool present = PE_parse_boot_argn("garnet-dcc", &v, sizeof(v));
        const bool en = rdna4native::s1c::dcc_boot_enabled(present, v);
        if (OSCompareAndSwap(0u, en ? 2u : 1u, &gDccLatch)) {
            N1C_LOG("boot-arg garnet-dcc %s: GFX12 PTE.DCC is %s",
                    present ? (v == 0u ? "=0" : "nonzero") : "absent (default ON)",
                    en ? "ENABLED" : "DISABLED");
        }
        l = gDccLatch;
    }
    return l == 2u;
}

bool hugepage_on() {
    UInt32 l = gHugeLatch;
    if (l == 0u) {
        uint32_t v = 1u;
        const bool present = PE_parse_boot_argn("garnet-hugepage", &v, sizeof(v));
        const bool en = rdna4native::s1c::hugepage_boot_enabled(present, v);
        if (OSCompareAndSwap(0u, en ? 2u : 1u, &gHugeLatch)) {
            N1C_LOG("boot-arg garnet-hugepage %s: 2 MiB PDB0 huge pages are %s",
                    present ? (v == 0u ? "=0" : "nonzero") : "absent (default ON)",
                    en ? "ENABLED" : "DISABLED");
        }
        l = gHugeLatch;
    }
    return l == 2u;
}

bool async_compute_on() {
    if (gCtx == nullptr || !gCtx->kcqMapped || !gCtx->kcq.mapped || !gCtx->kcqTestPassed ||
        gCtx->kcq.ring_cpu == nullptr || gCtx->kcq.wb_cpu == nullptr || gCtx->kcq.rptr_cpu == nullptr) {
        return false;
    }
    UInt32 l = gAsyncComputeLatch;
    if (l == 0u) {
        uint32_t v = 1u;
        const bool present = PE_parse_boot_argn("garnet-async-compute", &v, sizeof(v));
        const bool en = rdna4native::s1c::async_compute_boot_enabled(present, v);
        if (OSCompareAndSwap(0u, en ? 2u : 1u, &gAsyncComputeLatch)) {
            N1C_LOG("boot-arg garnet-async-compute %s: MEC async compute ring (ctx.kcq) is %s",
                    present ? (v == 0u ? "=0" : "nonzero") : "absent (default ON)",
                    en ? "ENABLED" : "DISABLED");
        }
        l = gAsyncComputeLatch;
    }
    return rdna4native::s1c::async_compute_active(gCtx->kcq.mapped, gCtx->kcqTestPassed, l == 2u);
}

static bool irq_wait_boot_on() {
    UInt32 l = gIrqWaitLatch;
    if (l == rdna4native::irqwait::kLatchUnset) {
        uint32_t v = 1u;
        const bool present = PE_parse_boot_argn("garnet-irq-wait", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::irqwait::kLatchUnset,
                             rdna4native::irqwait::latch_value(present, v), &gIrqWaitLatch)) {
            N1C_LOG("boot-arg garnet-irq-wait %s: EOP IRQ fence wakeup is %s",
                    present ? (v == 0u ? "=0" : "nonzero") : "absent (default ON)",
                    rdna4native::irqwait::latch_is_on(gIrqWaitLatch) ? "ENABLED (when MSI active)" : "DISABLED (legacy spin)");
        }
        l = gIrqWaitLatch;
    }
    return rdna4native::irqwait::latch_is_on(l);
}

bool n1c_irq_wait_enabled() {
    if (gCtx == nullptr) return false;
    return rdna4native::irqwait::active(gCtx->wantInterrupts, gCtx->ih.enabled,
                                        gCtx->ih.enable_cpu_intr, irq_wait_boot_on());
}

void n1c_irq_wait_stats(uint64_t *sleeps, uint64_t *wakes) {
    if (sleeps) *sleeps = __atomic_load_n(&gIrqWaitSleeps, __ATOMIC_RELAXED);
    if (wakes) *wakes = __atomic_load_n(&gIrqWaitWakes, __ATOMIC_RELAXED);
}

static inline uint64_t irq_epoch_now() {
    return __atomic_load_n(&gIrqEpoch, __ATOMIC_ACQUIRE);
}

void n1c_irq_notify() {
    IOLock *wl = __atomic_load_n(&gWaitLock, __ATOMIC_ACQUIRE);
    if (wl == nullptr) {
        __atomic_fetch_add(&gIrqEpoch, 1ull, __ATOMIC_ACQ_REL);
        return;
    }
    IOLockLock(wl);
    __atomic_fetch_add(&gIrqEpoch, 1ull, __ATOMIC_ACQ_REL);
    IOLockWakeup(wl, &gIrqEvent, false);
    __atomic_fetch_add(&gIrqWaitWakes, 1ull, __ATOMIC_RELAXED);
    IOLockUnlock(wl);
}

static void irq_arm_ring_eop(uint32_t *ring, uint64_t startWc, uint32_t n, uint32_t mask, const uint32_t *dw) {
    if (!n1c_irq_wait_enabled()) return;
    if (!rdna4native::irqwait::has_trailing_release_mem(dw, n)) return;
    ring[ring_idx(startWc + (uint64_t)(n - 6u), mask)] = rdna4native::irqwait::dw2_with_eop_int(dw[n - 6u]);
}

static void n1c_wait_relax(uint64_t elapsedNs, uint64_t remainingNs, uint64_t epochBeforeCheck) {
    const bool irqOn = n1c_irq_wait_enabled();
    const rdna4native::irqwait::WaitStep step = rdna4native::irqwait::next_step(irqOn, elapsedNs, remainingNs);
    if (step.action == rdna4native::irqwait::kActDelay1us) {
        IODelay(1);
        return;
    }
    if (step.action == rdna4native::irqwait::kActSleep1ms) {
        IOSleep(1);
        return;
    }
    IOLock *wl = lazy_lock(&gWaitLock);
    if (wl == nullptr) {
        IOSleep(1);
        return;
    }
    uint64_t deltaAbs = 0, nowAbs = 0;
    nanoseconds_to_absolutetime(step.sleepNs, &deltaAbs);
    clock_get_uptime(&nowAbs);
    const AbsoluteTime deadline = static_cast<AbsoluteTime>(nowAbs + deltaAbs);
    IOLockLock(wl);
    if (rdna4native::irqwait::should_sleep_for_epoch(epochBeforeCheck, irq_epoch_now())) {
        __atomic_fetch_add(&gIrqWaitSleeps, 1ull, __ATOMIC_RELAXED);
        (void)IOLockSleepDeadline(wl, &gIrqEvent, deadline, THREAD_UNINT);
    }
    IOLockUnlock(wl);
}
uint32_t gSessSeqCounter = 0;
volatile UInt32 gSessWs;
static bool gLegacyLogged[8];
static uint32_t gSubmitBuf[280];
static uint32_t gKcqSubmitBuf[280];
static rdna4native::g2::OwnerLog gOwners;
static BufferFlight gBufferFlights[kMaxSessions];
static volatile UInt32 gRecovering;
uint64_t gVramOthers;
volatile UInt32 gScanOwner;
static G2Stat gG2;
static uint64_t gFaultPrev;
static uint64_t gAppAllow[rdna4native::g4::kMaxEntries];
static uint64_t gAppDeny[rdna4native::g4::kMaxDenyEntries];
uint64_t gMappedLeakBytes;
uint32_t gMappedLeakBos;
uint64_t gVisOthers;
static rdna4native::g4::StrikeTab gStrikes;
volatile UInt32 gWsId;
uint64_t gWsLastNs;
static uint64_t gG5Since[rdna4native::g2::kMaxSessions];
static rdna4native::g5::Stat gG5;
static rdna4native::g5::AdmitIn gG5In;
static uint64_t gLogGate;
uint64_t gPrtTotalMaps;

static bool g5_on();
static uint32_t g5_credit();

// ---- helpers ------------------------------------------------------------------------------------------------------------------
IOLock *lazy_lock(IOLock **slot) {
    if (*slot == nullptr) {
        IOLock *l = IOLockAlloc();
        if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)slot)) IOLockFree(l);
    }
    return *slot;
}
uint64_t now_ns() {
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns;
}
bool vmid_fence_reached(void *, uint32_t domain, uint64_t seq) {
    if (domain == rdna4vmid::kDomainCompute) return kcq_retired_now(kcq_emitted_now()) >= seq_raw(seq);
    return retired_now(emitted_now()) >= seq;
}
// Take the caller's session lock and re-check under it. Returns the session LOCKED, or null with nothing held.
Session *sess_lock(const N1cRef &r, bool needHello) {
    Session *s = slot_ptr(r.slot);
    IOLock *l = s != nullptr ? __atomic_load_n(&s->lock, __ATOMIC_ACQUIRE) : nullptr;
    if (l == nullptr) return nullptr;
    IOLockLock(l);
    if (!s->open || r.id == 0u || s->id != r.id || (needHello && !s->hello)) { IOLockUnlock(l); return nullptr; }
    return s;
}
// Sync session PRT refcount (maps + pages) and global atomic PRT count from s->maps. Caller holds s->lock.
void session_sync_prt(Session *s) {
    const PrtStats st = va_prt_stats(s->maps, kMaxMaps);
    const int64_t delta = (int64_t)st.maps - (int64_t)s->prtMaps;
    s->prtMaps = st.maps;
    s->prtPages = st.pages;
    if (delta > 0) __atomic_add_fetch(&gPrtTotalMaps, (uint64_t)delta, __ATOMIC_RELAXED);
    else if (delta < 0) __atomic_sub_fetch(&gPrtTotalMaps, (uint64_t)(-delta), __ATOMIC_RELAXED);
}

bool n1c_is_open() { return __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u; }

bool n1c_refuse_legacy(uint32_t site) {
    if (!native_s1b_latched()) return false;
    if (site < 8 && !gLegacyLogged[site]) {
        gLegacyLogged[site] = true;
        N1C_LOG("REFUSING legacy client site %u (%s): the native VM self-test ran on this boot; the legacy GFX-ring writers and the "
                "unlocked VRAM pool users are off (reboot without rdna4-native=1 for that path)", site,
                site == kN1cSiteSelfTest ? "SelfTest" : site == kN1cSiteAllocVRAM ? "AllocVRAM" : site == kN1cSiteFreeVRAM ? "FreeVRAM" :
                site == kN1cSiteWriteVRAM ? "WriteVRAM" : "?");
    }
    return true;
}

// ---- the HUNG latch -----------------------------------------------------------------------------------------------------------
static void hang_log(uint64_t emitted, uint64_t retired) {
    CPContext &cp = gCtx->cp;
    N1C_LOG("HUNG: seq emitted %llu retired %llu rptr %u wptr %u CP_STAT %#x GCVM_L2_PROTECTION_FAULT_STATUS_LO32 %#x",
            (unsigned long long)emitted, (unsigned long long)retired, cp.rptr_cpu ? *cp.rptr_cpu : 0u, cp.wptr,
            RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, IPBlock::GC, 0, 0x0F40u)), hub_rd(0x15d0));   // CP_STAT, GC base idx 0
}
// 0.0.612 (W3): the ONE place a latch is announced. Every latch site calls it only after hang_detect / hang_latch_wait returned true (gHang.hung is set),
// and it writes RDNA4,Ready = 0 on the Metal nub (sticky: never back to 1 this boot). Called with no hang lock held.
static void hang_announce(uint64_t emitted, uint64_t retired) {
    hang_log(emitted, retired);
    GarnetMetalNub::hungLatched();
    n1c_irq_notify();
}
static bool stall_handle(uint64_t em, uint64_t re);   // 0.0.627 (G2): below the ring
// Rule 6.3(b), on any GPU-dependent call: true when THIS call latched the hang. 0.0.627 (G2): with the switch ON a detected stall goes to stall_handle
// first (which may recover a non-WindowServer session instead of latching); with it OFF this is the 0.0.626 function.
static bool hang_poll() {
    const uint64_t em = emitted_now(), re = retired_now(em), now = now_ns();
    if (multi_on()) {
        bool expired = false;
        IOLockLock(gHangLock);
        if (!gHang.hung && __atomic_load_n(&gRecovering, __ATOMIC_SEQ_CST) == 0u) {
            hang_note(gHang, re, em, now);
            expired = hang_expired(gHang, re, em, now);
            if (expired) __atomic_store_n(&gRecovering, 1u, __ATOMIC_SEQ_CST);   // this call owns the stall
        }
        IOLockUnlock(gHangLock);
        return expired ? stall_handle(em, re) : false;
    }
    IOLockLock(gHangLock);
    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
    return det;
}
// Rule 6.3(a): a bounded internal wait reached 2 s. Used as is for a TLB flush that did not acknowledge (never attributable to a session).
void hang_from_wait() {
    const uint64_t em = emitted_now(), re = retired_now(em);
    IOLockLock(gHangLock);
    const bool det = hang_latch_wait(gHang);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
}
// 0.0.627 (G2): a wait for the RING (an own-seqno wait or the ring-space wait) reached its bound. Switch OFF: hang_from_wait, as 0.0.626 (returns kBoundStalled).
// Switch ON (0.0.629, G2 review fix R2): the 2 s of THAT wait blame nobody by themselves. The hang clock is noted and the stall goes to stall_handle ONLY when
// hang_expired (no change of the retired seqno for kHangNs, measured from the last observed change or idle kick, not from the start of the wait); otherwise
// kBoundNoBlame: the caller returns Timeout and nobody is reset or poisoned. kBoundBusy: HUNG is latched or a recovery already runs (the caller re-checks).
uint32_t stall_from_wait() {
    if (!multi_on()) { hang_from_wait(); return rdna4native::g2::kBoundStalled; }
    const uint64_t em = emitted_now(), re = retired_now(em), now = now_ns();
    bool mine = false;
    IOLockLock(gHangLock);
    const bool busy = gHang.hung || __atomic_load_n(&gRecovering, __ATOMIC_SEQ_CST) != 0u;
    if (!busy) {
        hang_note(gHang, re, em, now);
        mine = hang_expired(gHang, re, em, now);   // R2: the hang threshold since the last progress, never the waiter's own 2 s
        if (mine) __atomic_store_n(&gRecovering, 1u, __ATOMIC_SEQ_CST);   // this call owns the stall
    }
    IOLockUnlock(gHangLock);
    if (mine) (void)stall_handle(em, re);
    return rdna4native::g2::bound_verdict(busy, mine);
}
static bool kcq_hang_poll() {
    if (!async_compute_on()) return false;
    const uint64_t em = kcq_emitted_now();
    if (em == 0ull) return false;
    const uint64_t re = kcq_retired_now(em), now = now_ns();
    bool det = false;
    IOLockLock(gHangLock);
    if (!gHang.hung) {
        hang_note(gKcqHang, re, em, now);
        if (hang_expired(gKcqHang, re, em, now)) {
            det = hang_latch_wait(gHang);
            (void)hang_latch_wait(gKcqHang);
        }
    }
    IOLockUnlock(gHangLock);
    if (det) {
        N1C_LOG("KCQ HUNG: compute seq emitted %llu retired %llu rptr %u wptr %u",
                (unsigned long long)em, (unsigned long long)re,
                gCtx->kcq.rptr_cpu ? *gCtx->kcq.rptr_cpu : 0u, gCtx->kcq.wptr);
        hang_announce(emitted_now(), retired_now(emitted_now()));
    }
    return det;
}
// The prologue of every GPU-dependent call: Aborted after a latch (or for a poisoned session), Timeout for the call that detects it, else kOk.
uint32_t gpu_gate(const Session *s) {
    if (hung_now()) return hang_rc(true, false);
    if (s != nullptr && sess_poisoned(s)) return rdna4native::g2::poison_rc(true);
    const uint32_t rc = hang_rc(false, hang_poll() || kcq_hang_poll());
    if (rc == kOk && s != nullptr && sess_poisoned(s)) return rdna4native::g2::poison_rc(true);   // this very call's stall named the caller guilty
    return rc;
}
// 0.0.627 (G2): wait for retired >= THIS session's own last seqno (the ring runs in order, so that is sufficient), bounded at 2 s, plus up to one recovery's
// bound when the 2 s ran out (a stall that another session caused may be recovered meanwhile). Called with the session's own lock only. true = idle.
// false = not idle (already hung, this wait latched the hang, or the work did not retire): the caller leaks. 0.0.629 (R2): *noBlame (optional) is set when the
// 2 s ran out while the GPU was still making progress (switch ON only): nobody was blamed, the work is merely not retired yet (BoFree then keeps the BO).
constexpr uint64_t kRecoverWaitNs = 6000000000ull;   // RESET 2 s + ring space 2 s + verify 1 s + slack
bool own_wait(Session *s, bool *noBlame) {
    const uint64_t own = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE);
    const uint64_t ownKcq = __atomic_load_n(&s->emittedKcq, __ATOMIC_ACQUIRE);
    const uint64_t start = now_ns();
    bool stalled = false;
    if (noBlame != nullptr) *noBlame = false;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        if (rdna4native::g2::own_idle(retired_now(emitted_now()), own) &&
            (ownKcq == 0ull || rdna4native::g2::own_idle(kcq_retired_now(kcq_emitted_now()), ownKcq))) return true;
        if (hung_now()) return false;
        const uint64_t el = now_ns() - start;
        if (!stalled && el >= kWaitCapNs) {
            if (!multi_on()) { hang_from_wait(); return false; }   // switch OFF: exactly the 0.0.626 idle wait (latch, not idle)
            if (ownKcq != 0ull && !rdna4native::g2::own_idle(kcq_retired_now(kcq_emitted_now()), ownKcq)) {
                if (kcq_hang_poll()) return false;
                if (noBlame != nullptr) *noBlame = true;
                return false;
            }
            if (stall_from_wait() == rdna4native::g2::kBoundNoBlame) { if (noBlame != nullptr) *noBlame = true; return false; }   // R2: progress within kHangNs: Timeout, no blame
            stalled = true; continue;
        }
        const uint64_t limit = stalled ? (kWaitCapNs + kRecoverWaitNs) : kWaitCapNs;
        if (stalled && el >= limit) return false;
        n1c_wait_relax(el, el < limit ? (limit - el) : 0ull, ep);
    }
}

// ---- the ring -------------------------------------------------------------------------------------------------------------------
// Everything that goes on the ring goes through here: the space check, the write, the owner record, the emitted-seq update and the kick. Caller holds
// gRingLock. Returns kOk, or kTimeout when the space wait reached 2 s (the CALLER decides what that means, after dropping the lock).
static uint32_t ring_put_locked(const uint32_t *dw, uint32_t n, uint64_t seq, uint8_t ownerSlot, uint32_t ownerId, Session *owner) {
    DeviceContext &dev = *gCtx->dev;
    CPContext &cp = gCtx->cp;
    const uint32_t mask = cp.ring_ptr_mask;
    if (n > cp.ring_size_dwords / 2u) return kBadArg;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    const uint64_t t0 = now_ns();
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        sysmem_rmb();
        const uint32_t r = *cp.rptr_cpu & mask;
        if (ring_has_space(ring_free64(r, gWc, mask), n)) break;
        const uint64_t el = now_ns() - t0;
        if (el >= kWaitCapNs) {
            N1C_LOG("ring space: need %u dwords (+%u slack), rptr %u wptr %u, no room in 2 s", n, kRingSlack, r, (uint32_t)(gWc & mask));
            return kTimeout;
        }
        n1c_wait_relax(el, kWaitCapNs - el, ep);
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    const uint64_t startWc = gWc;
    uint64_t wc = gWc;
    for (uint32_t i = 0; i < n; i++) { ring[ring_idx(wc, mask)] = dw[i]; wc++; }
    irq_arm_ring_eop(ring, startWc, n, mask, dw);
    gWc = wc;
    cp.wptr = ring_idx(wc, mask);
    // The clock for rule 6.3(b) starts at the kick that makes an idle ring busy; emitted goes up BEFORE the doorbell so retired can never
    // be observed above emitted. 0.0.627: the owner is recorded before the seqno is published.
    const uint64_t emBefore = emitted_now();
    const uint64_t reNow = retired_now(emBefore);
    IOLockLock(gHangLock);
    hang_kick(gHang, reNow, emBefore, now_ns());
    IOLockUnlock(gHangLock);
    const uint8_t ownerVmid = owner != nullptr ? (uint8_t)__atomic_load_n(&owner->vmid, __ATOMIC_ACQUIRE) : 0u;
    rdna4native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc, ownerVmid);
    if (owner != nullptr && owner->bufferSubmitting) gBufferFlights[ownerSlot] = BufferFlight{seq, ownerId};
    if (owner != nullptr) __atomic_store_n(&owner->emitted, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&gEmitted, seq, __ATOMIC_RELEASE);
    amdgpu_hdp_flush(dev);
    const uint64_t wptr64 = wc;   // the FULL 64-bit counter, never masked
    *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = wptr64;
    sysmem_wmb();
    const uint64_t off = (uint64_t)cp.doorbell_index * kCPDoorbellStride;
    WDOORBELL64(dev, off, wptr64);
    (void)RDOORBELL32(dev, off);
    return kOk;
}
// One submission of session `s` (its VMID): the seqno is assigned, the packet built and put on the ring under ONE hold of gRingLock, so two sessions can
// never take the same seqno. Returns kOk (*seqOut set), BadArg / InternalError for an unusable packet, or Timeout when the space wait reached 2 s (that
// latches HUNG, or with the switch ON goes to stall_handle, AFTER the ring lock is dropped and before anything was written).
static uint32_t ring_submit(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    IOLockLock(gRingLock);
    const uint64_t seq = emitted_now() + 1ull;
    const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,
                                    fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
    if (n == 0u) { IOLockUnlock(gRingLock); return (uint32_t)kIOReturnInternalError; }
    const uint32_t er = ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);
    IOLockUnlock(gRingLock);
    if (er == kTimeout) { (void)stall_from_wait(); return kTimeout; }   // 0.0.629 (R2): blames only when the hang clock expired; Timeout either way
    if (er != kOk) return er;
    *seqOut = seq;
    return kOk;
}

// 0.0.650: the verdict's inputs, read under ONE hold of gRingLock (the caller holds it): the ring space, the retired / emitted seqnos, WindowServer's id and last submit, the
// waiters' clocks. A separate function and a file-scope static keep ring_submit_app_g5's stack frame small.
__attribute__((noinline)) static void g5_fill_in_locked(const Session *s, uint32_t K, uint32_t need) {
    const uint64_t now = now_ns();
    sysmem_rmb();
    const uint32_t mask = gCtx->cp.ring_ptr_mask;
    const uint32_t rp = *gCtx->cp.rptr_cpu & mask;
    const uint64_t em = emitted_now(), re = retired_now(em);
    const uint32_t wsId = __atomic_load_n(&gWsId, __ATOMIC_SEQ_CST);
    gG5In = rdna4native::g5::AdmitIn{ true, false, wsId != 0u, __atomic_load_n(&gWsLastNs, __ATOMIC_ACQUIRE), now, K, re, em, wsId, ring_free64(rp, gWc, mask), need, gG5Since, s->slot };
}
// 0.0.650 (G5 Stage 1; rdna4-g5=1 only): the Submit of a session that is NOT WindowServer. Called with ONLY the session's own lock held (the lock order of own_wait).
// The admission verdict (rdna4native::g5::admission: ring reserve, the credit while WindowServer is active, first come first served) and the seqno are taken in ONE hold of
// gRingLock; the wait for a Wait verdict runs OUTSIDE it (no sleep under the lock), bounded by kG5WaitBoundNs. Every exit clears this slot's gG5Since entry.
static uint32_t ring_submit_g5_body(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    using namespace rdna4native::g5;
    const uint32_t need = submit_dwords(nIbs, hasFence);
    const uint32_t K = g5_credit();
    uint32_t rc = kOk;
    bool waiting = false, stallDone = false, timedOut = false;
    uint64_t t0 = 0;
    for (;;) {
        IOLockLock(gRingLock);
        g5_fill_in_locked(s, K, need);
        const uint64_t now = gG5In.now;
        if (admission(gOwners, gG5In) == kAdmit) {
            const uint64_t waitedNs = gG5Since[s->slot] != 0ull && now > gG5Since[s->slot] ? now - gG5Since[s->slot] : 0ull;
            since_clear(gG5Since, s->slot); waiting = false;
            const uint64_t seq = emitted_now() + 1ull;
            const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,
                                            fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
            if (n == 0u) { IOLockUnlock(gRingLock); rc = (uint32_t)kIOReturnInternalError; break; }
            const uint32_t er = ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);
            if (er == kOk) stat_admit(gG5, waitedNs);
            IOLockUnlock(gRingLock);
            if (er == kTimeout) { (void)stall_from_wait(); rc = kTimeout; break; }   // as ring_submit (R2): blames only when the hang clock expired
            if (er != kOk) { rc = er; break; }
            *seqOut = seq;
            break;
        }
        since_mark(gG5Since, s->slot, now); waiting = true; t0 = gG5Since[s->slot];
        IOLockUnlock(gRingLock);
        if (hung_now()) { rc = hang_rc(true, false); break; }
        if (sess_poisoned(s)) { rc = rdna4native::g2::poison_rc(true); break; }
        const uint64_t el = now_ns() - t0;
        const uint32_t step = wait_step(el, stallDone);
        if (step == kGiveUp) { timedOut = true; rc = kTimeout; break; }   // the app gets DEVICE_LOST, but only after a stall that recovery did not clear
        if (step == kStallNow) { stallDone = true; (void)stall_from_wait(); continue; }   // kBoundNoBlame / Busy: keep waiting
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    if (waiting) {   // the single exit of every path that left while still waiting
        IOLockLock(gRingLock);
        since_clear(gG5Since, s->slot);
        if (timedOut) stat_timeout(gG5);
        IOLockUnlock(gRingLock);
    }
    return rc;
}
// Forward declaration for shared pooled submission helper.
static uint32_t kcq_submit(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut);

// Pooled submission for non-pinned multi-session clients (GFXHUB VMIDs 9..15): binds/flushes a pool VMID under gCliLock,
// holds it against eviction via beginSubmit/endSubmit while emitting on GFX or KCQ, then records the emitted seqno.
static uint32_t submit_pooled(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr,
                              bool isCompute, bool useG5, uint64_t *seqOut) {
    uint64_t busyStart = 0ull;
    bool stallDone = false;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        if (hung_now()) return hang_rc(true, false);
        if (sess_poisoned(s)) return rdna4native::g2::poison_rc(true);
        rdna4vmid::Grant grant {};
        IOLockLock(gCliLock);
        ensure_vmid_pool_locked();
        const uint64_t seqNeed = __atomic_load_n(&s->tlbSeq, __ATOMIC_ACQUIRE);
        const rdna4vmid::Result pr = gVmidPool.grab(reinterpret_cast<uintptr_t>(s), s->rootPa, seqNeed, grant);
        if (pr == rdna4vmid::Result::Exhausted) {
            IOLockUnlock(gCliLock);
            N1C_LOG("%ssubmit refused (NoResources): pooled VMID pool exhausted (slot %u)", isCompute ? "kcq " : "", s->slot);
            return kNoResources;
        }
        if (pr == rdna4vmid::Result::Ok) {
            if (grant.stolen && grant.prevOwner != 0u) {
                Session *prev = reinterpret_cast<Session *>(grant.prevOwner);
                if (__atomic_load_n(&prev->vmid, __ATOMIC_ACQUIRE) == grant.vmid) __atomic_store_n(&prev->vmid, 0u, __ATOMIC_RELEASE);
            }
            if (!gVmidCtxReady[grant.vmid]) {
                bool ctxOk = false;
                (void)ctx_prepare_vmid(grant.vmid, &ctxOk);
                if (!ctxOk) {
                    (void)gVmidPool.quarantine(reinterpret_cast<uintptr_t>(s), grant.vmid);
                    __atomic_store_n(&s->vmid, 0u, __ATOMIC_RELEASE);
                    IOLockUnlock(gCliLock);
                    return kNotReady;
                }
                gVmidCtxReady[grant.vmid] = true;
            }
            if (grant.rebind) {
                if (program_root(grant.vmid, s->rootPa) != kOk) {
                    (void)gVmidPool.quarantine(reinterpret_cast<uintptr_t>(s), grant.vmid);
                    __atomic_store_n(&s->vmid, 0u, __ATOMIC_RELEASE);
                    IOLockUnlock(gCliLock);
                    return kTimeout;
                }
                __atomic_store_n(&s->vmid, grant.vmid, __ATOMIC_RELEASE);
                (void)gVmidPool.flushComplete(reinterpret_cast<uintptr_t>(s), grant.vmid, grant.flushSeq);
            } else {
                __atomic_store_n(&s->vmid, grant.vmid, __ATOMIC_RELEASE);
                if (grant.flush) {
                    amdgpu_hdp_flush(*gCtx->dev);
                    if (flush_vmid(grant.vmid) != kOk) {
                        (void)gVmidPool.quarantine(reinterpret_cast<uintptr_t>(s), grant.vmid);
                        __atomic_store_n(&s->vmid, 0u, __ATOMIC_RELEASE);
                        IOLockUnlock(gCliLock);
                        return kTimeout;
                    }
                    (void)gVmidPool.flushComplete(reinterpret_cast<uintptr_t>(s), grant.vmid, grant.flushSeq);
                }
            }
            const uint32_t vmid = grant.vmid;
            gVmidPool.beginSubmit(vmid);
            IOLockUnlock(gCliLock);
            uint64_t seq = 0ull;
            const uint32_t er = isCompute ? kcq_submit(s, va, by, nIbs, hasFence, fenceAddr, &seq)
                                          : (useG5 ? ring_submit_g5_body(s, va, by, nIbs, hasFence, fenceAddr, &seq)
                                                   : ring_submit(s, va, by, nIbs, hasFence, fenceAddr, &seq));
            IOLockLock(gCliLock);
            if (er == kOk) {
                if (gVmidPool.ownerOf(vmid) == reinterpret_cast<uintptr_t>(s)) {
                    gVmidPool.noteSubmit(vmid, isCompute ? rdna4vmid::kDomainCompute : rdna4vmid::kDomainGfx,
                                         isCompute ? seq_raw(seq) : seq);
                }
                *seqOut = seq;
            }
            gVmidPool.endSubmit(vmid);
            IOLockUnlock(gCliLock);
            return er;
        }
        IOLockUnlock(gCliLock);
        const uint64_t now = now_ns();
        if (busyStart == 0ull) busyStart = now;
        const uint64_t el = now - busyStart;
        if (el >= kWaitCapNs + kRecoverWaitNs) {
            N1C_LOG("%ssubmit refused (Timeout): pooled VMID wait exceeded bound (slot %u)", isCompute ? "kcq " : "", s->slot);
            return kTimeout;
        }
        if (!stallDone && el >= kWaitCapNs) {
            stallDone = true;
            (void)stall_from_wait();
            (void)kcq_hang_poll();
            continue;
        }
        n1c_wait_relax(el, (kWaitCapNs + kRecoverWaitNs) - el, ep);
    }
}
static uint32_t ring_submit_pooled(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, bool useG5, uint64_t *seqOut) {
    return submit_pooled(s, va, by, nIbs, hasFence, fenceAddr, false, useG5, seqOut);
}
static uint32_t ring_submit_app_g5(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    if (!s->pinnedVmid) return ring_submit_pooled(s, va, by, nIbs, hasFence, fenceAddr, true, seqOut);
    return ring_submit_g5_body(s, va, by, nIbs, hasFence, fenceAddr, seqOut);
}

// ---- MEC async compute ring (ctx.kcq) ------------------------------------------------------------------------------------------
// Runs completely independently of gRingLock so GFX and Compute submissions never serialize on ring writes or GPU execution.
static uint32_t kcq_put_locked(const uint32_t *dw, uint32_t n, uint64_t rawSeq, Session *owner) {
    DeviceContext &dev = *gCtx->dev;
    ComputeQueue &cq = gCtx->kcq;
    const uint32_t mask = cq.ring_ptr_mask;
    if (n > cq.ring_size_dwords / 2u) return kBadArg;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    const uint64_t t0 = now_ns();
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        sysmem_rmb();
        const uint32_t r = *cq.rptr_cpu & mask;
        const uint64_t emCheck = kcq_emitted_now();
        if (ring_has_space(ring_free64(r, gKcqWc, mask), n) || kcq_retired_now(emCheck) >= emCheck) break;
        const uint64_t el = now_ns() - t0;
        if (el >= kWaitCapNs) {
            N1C_LOG("kcq ring space: need %u dwords (+%u slack), rptr %u wptr %u, no room in 2 s",
                    n, kRingSlack, r, (uint32_t)(gKcqWc & mask));
            return kTimeout;
        }
        n1c_wait_relax(el, kWaitCapNs - el, ep);
    }
    auto *ring = static_cast<uint32_t *>(cq.ring_cpu);
    const uint64_t startWc = gKcqWc;
    uint64_t wc = gKcqWc;
    for (uint32_t i = 0; i < n; i++) { ring[ring_idx(wc, mask)] = dw[i]; wc++; }
    irq_arm_ring_eop(ring, startWc, n, mask, dw);
    gKcqWc = wc;
    cq.wptr = ring_idx(wc, mask);
    const uint64_t emBefore = kcq_emitted_now();
    const uint64_t reNow = kcq_retired_now(emBefore);
    IOLockLock(gHangLock);
    hang_kick(gKcqHang, reNow, emBefore, now_ns());
    IOLockUnlock(gHangLock);
    if (owner != nullptr) __atomic_store_n(&owner->emittedKcq, rawSeq, __ATOMIC_RELEASE);
    __atomic_store_n(&gKcqEmitted, rawSeq, __ATOMIC_RELEASE);
    amdgpu_hdp_flush(dev);
    const uint64_t wptr64 = wc;
    *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cq.wb_cpu) + kCPWBOffsetWptr) = wptr64;
    sysmem_wmb();
    const uint64_t off = (uint64_t)cq.doorbell_index * kCPDoorbellStride;
    WDOORBELL64(dev, off, wptr64);
    (void)RDOORBELL32(dev, off);
    return kOk;
}
static uint32_t kcq_submit(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    IOLockLock(gKcqRingLock);
    const uint64_t rawSeq = kcq_emitted_now() + 1ull;
    const uint64_t taggedSeq = seq_with_ring(rawSeq, true);
    const uint32_t n = build_compute_submit(gKcqSubmitBuf, (uint32_t)(sizeof(gKcqSubmitBuf) / sizeof(gKcqSubmitBuf[0])),
                                            va, by, nIbs, __atomic_load_n(&s->vmid, __ATOMIC_ACQUIRE), hasFence,
                                            fenceAddr, taggedSeq, gCtx->kcq.wb_bus + kWbOffsetNativeSeq, rawSeq);
    if (n == 0u) { IOLockUnlock(gKcqRingLock); return (uint32_t)kIOReturnInternalError; }
    const uint32_t er = kcq_put_locked(gKcqSubmitBuf, n, rawSeq, s);
    IOLockUnlock(gKcqRingLock);
    if (er == kTimeout) { (void)kcq_hang_poll(); return kTimeout; }
    if (er != kOk) return er;
    *seqOut = taggedSeq;
    return kOk;
}
static uint32_t kcq_submit_pooled(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    return submit_pooled(s, va, by, nIbs, hasFence, fenceAddr, true, false, seqOut);
}

// ---- the automatic recovery (0.0.627, G2; switch ON only) ----------------------------------------------------------------------------
// NOP every ring dword outside the live range [liveStart, gWc) - the G1 scrub rule (the older dwords have run; the free space must not hold stale
// packets if the RESET moves the read pointer). Caller holds gRingLock.
static void g2_scrub_locked(uint64_t liveStart) {
    CPContext &cp = gCtx->cp;
    const uint32_t mask = cp.ring_ptr_mask;
    auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
    uint32_t n = 0;
    if (gWc - liveStart <= (uint64_t)cp.ring_size_dwords) {
        for (uint32_t i = 0; i < cp.ring_size_dwords; i++)
            if (!rdna4native::g1::in_live_range(i, liveStart, gWc, mask)) { ring[i] = cp_p3_nop1(); n++; }
    }
    sysmem_wmb();
    N1C_LOG("g2 recovery: ring scrub: %u of %u dwords outside the live range [%llu, %llu) set to NOP", n, cp.ring_size_dwords, (unsigned long long)liveStart, (unsigned long long)gWc);
}
// The G1 recovery (MES RESET reset_legacy_gfx, proven by G1 on this firmware: notes), run on the kernel's own initiative for a
// guilty NON-WindowServer session, then proven by a kernel-only probe fence (one RELEASE_MEM of the next seqno: no IB, no VMID). gRingLock is held for the
// whole run, so no session emits meanwhile. The guilty session is poisoned BEFORE the RESET (its seqno can retire only after it, so its waiter sees the
// poison first). Returns recovery_ok.
static bool g2_recover(const rdna4native::g2::Guilty &g, Session *gs) {
    using namespace rdna4native::g2;
    DeviceContext &dev = *gCtx->dev;
    CPContext &cp = gCtx->cp;
    IOLockLock(gRingLock);
    const uint64_t emAt = emitted_now();
    const uint64_t reAt = retired_now(emAt);
    // A RESET may drop collateral work, then its probe advances the global timeline.
    // Fail sessions with pending buffer operations before that probe can look successful.
    for (uint32_t slot = 0; slot < kMaxSessions; ++slot) {
        Session *pending = slot_ptr(slot);
        if (pending != nullptr && buffer_reset_loses(gBufferFlights[slot], __atomic_load_n(&pending->id, __ATOMIC_ACQUIRE), reAt))
            __atomic_store_n(&pending->poisoned, 1u, __ATOMIC_SEQ_CST);
    }
    g2_scrub_locked(g.startWc);
    __atomic_store_n(&gs->poisoned, 1u, __ATOMIC_SEQ_CST);
    n1c_irq_notify();
    const uint32_t resetVmid = (g.vmid >= kNativeVmid && g.vmid < rdna4vmid::kSlots) ? g.vmid : __atomic_load_n(&gs->vmid, __ATOMIC_ACQUIRE);
    const uint64_t t0 = now_ns();
    const kern_return_t r = mes_reset_legacy_gfx_queue(dev, gCtx->mes, 0u, 0u, resetVmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);
    const uint64_t actNs = now_ns() - t0;
    bool put = false, ret = false;
    uint64_t probe = 0, verifyNs = 0;
    if (r == kIOReturnSuccess) {
        uint32_t dw[8];
        probe = emAt + 1ull;
        (void)emit_release_mem(dw, cp.wb_bus + kWbOffsetNativeSeq, probe);
        put = ring_put_locked(dw, 8u, probe, kOwnerKernel, 0u, nullptr) == kOk;
        const uint64_t t1 = now_ns();
        while (put && !ret) {
            const uint64_t ep = irq_epoch_now();
            ret = retired_now(emitted_now()) >= probe;
            verifyNs = now_ns() - t1;
            if (!ret && verifyNs >= kVerifyNs) break;
            if (!ret) n1c_wait_relax(verifyNs, kVerifyNs - verifyNs, ep);
        }
    }
    IOLockUnlock(gRingLock);
    const bool ok = recovery_ok(r == kIOReturnSuccess, put, ret);
    if (ok) { gG2.recoveries++; gG2.lastGuiltySlot = g.slot; gG2.lastGuiltySeq = g.seq; gG2.collateralTotal += collateral(g.seq, emAt); }
    else gG2.failed++;
    N1C_LOG("g2 recovery: MES RESET(reset_legacy_gfx, vmid %u) %s (%#x, %llu us); probe seq %llu %s (%llu us); guilty slot %u session %u seq %llu POISONED; "
            "%llu later submission(s) of other sessions were queued behind it (executed or dropped: not known): %s", resetVmid, r == kIOReturnSuccess ? "ACKED" : "NOT acked", r,
            (unsigned long long)(actNs / 1000ull), (unsigned long long)probe, ret ? "RETIRED" : put ? "did NOT retire" : "not emitted", (unsigned long long)(verifyNs / 1000ull),
            g.slot, g.sessId, (unsigned long long)g.seq, (unsigned long long)collateral(g.seq, emAt), ok ? "RECOVERED, the other sessions continue" : "FAILED: latching HUNG");
    return ok;
}
// A stall was detected and THIS call owns it (it set gRecovering). The owner of the oldest unretired seqno is the guilty session; a non-WindowServer user
// session is recovered (switch ON), everything else latches HUNG exactly as before. Clears gRecovering. Returns true when THIS call latched HUNG.
// Called with at most the caller's own session lock (never gCliLock / gRingLock).
static bool stall_handle(uint64_t em, uint64_t re) {
    using namespace rdna4native::g2;
    const Guilty g = guilty_of(gOwners, re, em);
    Session *gs = (g.known && g.slot < kMaxSessions) ? slot_ptr(g.slot) : nullptr;
    const bool user = gs != nullptr && __atomic_load_n(&gs->open, __ATOMIC_ACQUIRE) && __atomic_load_n(&gs->id, __ATOMIC_ACQUIRE) == g.sessId &&
                      __atomic_load_n(&gs->hello, __ATOMIC_ACQUIRE);
    const bool ws = user && __atomic_load_n(&gs->ws, __ATOMIC_ACQUIRE);
    const uint64_t tried = gG2.recoveries + gG2.failed;   // 0.0.629 (R3): attempts this boot (gRecovering, held by this call, serialises the counters)
    const uint32_t act = stall_action_budgeted(multi_on(), g.known, g.slot, user, ws, mes_timeout_seen(), g1_holds(), tried);
    N1C_LOG("stall: seq emitted %llu retired %llu; the oldest unretired seq %llu belongs to %s (slot %u, session %u%s): %s", (unsigned long long)em, (unsigned long long)re,
            (unsigned long long)g.seq, !g.known ? "an unknown owner" : g.slot == kOwnerKernel ? "the kernel" : user ? "a user session" : "a non-user context", g.slot, g.sessId,
            ws ? ", WindowServer" : "", act == kStallRecover ? "recovering that session alone" : !recovery_budget_left(tried) ? "latching HUNG (the automatic-recovery budget of this boot is spent)" : "latching HUNG (today's behaviour)");
    if (act == kStallRecover && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {   // 0.0.641: one strike against the executable name of an APP session an automatic recovery is blamed on (gs->app is set only with apps ON)
        const uint32_t strikes = rdna4native::g4::strike_add(gStrikes, gs->appKey);
        N1C_LOG("stall: strike %u of %u for the application key %#llx (session %u)%s", strikes, rdna4native::g4::kStrikeLimit, (unsigned long long)gs->appKey, g.sessId,
                rdna4native::g4::strike_refused(strikes) ? ": that name is now REFUSED at open for the rest of this boot" : "");
    }
    if (act == kStallRecover && g2_recover(g, gs)) {
        const uint64_t em2 = emitted_now(), re2 = retired_now(em2);
        IOLockLock(gHangLock);
        gHang.lastRetired = re2; gHang.tProgress = now_ns();
        __atomic_store_n(&gRecovering, 0u, __ATOMIC_SEQ_CST);
        IOLockUnlock(gHangLock);
        return false;
    }
    IOLockLock(gHangLock);
    const bool det = hang_latch_wait(gHang);
    __atomic_store_n(&gRecovering, 0u, __ATOMIC_SEQ_CST);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
    return det;
}

IOReturn n1c_ctx(const N1cRef &ref, const rdna4n_ctx *in, rdna4n_ctx *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    memset(out, 0, sizeof(*out));
    IOReturn rc = kIOReturnSuccess;
    switch (in->op) {
    case RDNA4N_CTX_OP_ALLOC: {
        const uint32_t id = lowest_free(s->ctxUsed, 1, RDNA4N_MAX_CTX);
        if (id == 0) { rc = kIOReturnNoResources; break; }
        s->ctxUsed[id] = 1;
        out->op = id; out->flags = 0;          // dword0 = ctx_id, dword1 = 0
        break;
    }
    case RDNA4N_CTX_OP_FREE:
        if (!id_live(s->ctxUsed, RDNA4N_MAX_CTX, in->ctx_id)) rc = kIOReturnNotFound;
        else s->ctxUsed[in->ctx_id] = 0;
        break;
    case RDNA4N_CTX_OP_QUERY_STATE2: {
        if (!id_live(s->ctxUsed, RDNA4N_MAX_CTX, in->ctx_id)) { rc = kIOReturnNotFound; break; }
        const uint64_t fl = rdna4native::g2::ctx_state2(hung_now(), sess_poisoned(s));   // 0.0.627: also RESET | GUILTY for a poisoned session
        out->op = (uint32_t)(fl & 0xFFFFFFFFu); out->flags = (uint32_t)(fl >> 32);   // qword0
        break;
    }
    default:
        rc = kIOReturnBadArgument;
    }
    sess_unlock(s);
    return rc;
}

// The VMID-0 address of a user-fence target (contract 4.3): a VRAM BO by its MC address, a GTT BO through the slot's fence window. Caller holds the
// session's lock and gCliLock (the GART table and the VMID-0 TLB are shared).
static uint32_t resolve_fence(Session *s, uint32_t handle, uint32_t off, uint64_t *addr) {
    const Bo &b = s->bo[handle];
    FenceWindow &win = gWin[s->slot];
    if (b.kind == kBoHost) return kNotPermitted;   // 0.0.612: an imported range of the caller's memory is never a kernel-written fence target
    if (b.kind != kBoGtt) { *addr = fence_addr_vram(b.mc, off); return kOk; }
    const uint32_t page = off / (uint32_t)kPage;
    if (page >= b.hpageCount || b.hpages == nullptr) return kBadArg;
    bool isNew = false;
    const int slot = fence_slot_pick(s->fslot, RDNA4N_FENCE_SLOTS, handle, page, &isNew);
    if (slot < 0) return kNoResources;
    if (!fence_window_reserve_locked(win)) return kNoMemory;
    if (isNew) {
        // Bind only the targeted non-contiguous GTT page into the session's lazy 16-page GART window.
        const uint64_t pte = (b.hpages[page] & ~0xFFFull) | PTEFlags::SYSMEM_RW;
        bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + (uint64_t)slot) * 8ull, &pte, sizeof(pte));
        amdgpu_hdp_flush(*gCtx->dev);
        const uint32_t f = flush_vmid(0);
        if (f != kOk) return f;
        s->fslot[slot] = FenceSlot{ handle, page, 1 };
    }
    *addr = fence_addr_gtt(win.mc, (uint32_t)slot, off);
    return kOk;
}

IOReturn n1c_submit(const N1cRef &ref, const uint8_t *in, uint32_t size, uint64_t *seqOut) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    CsView v;
    const uint32_t prc = cs_parse(in, size, &v, async_compute_on());
    if (prc != kOk) return (IOReturn)prc;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    do {
        const uint32_t gate = gpu_gate(s);   // 0.0.627: also Aborted for a poisoned session
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        if (!id_live(s->ctxUsed, RDNA4N_MAX_CTX, v.ctx)) { rc = kIOReturnNotFound; break; }
        const bool hasFence = (v.flags & RDNA4N_CS_HAS_FENCE) != 0u;
        if (hasFence) {
            if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, v.fenceHandle)) { rc = kIOReturnNotFound; break; }
            if (!cs_fence_in_bo(v.fenceOffset, s->bo[v.fenceHandle].size)) { rc = kIOReturnBadArgument; break; }
        }
        // Every IB must sit entirely in EXEC-mapped pages of this client: an unmapped fetch could halt the CP.
        uint64_t va[RDNA4N_MAX_IBS]; uint32_t by[RDNA4N_MAX_IBS];
        bool bad = false;
        for (uint32_t i = 0; i < v.nIbs; i++) {
            cs_ib(in, i, &va[i], &by[i]);
            const uint64_t st = va_strip(va[i]);
            if (st < kVaFirst || (st >> 47) != ((st + by[i] - 1ull) >> 47) || !va_exec_covered(s->maps, kMaxMaps, st, by[i])) { bad = true; break; }
        }
        if (bad) { rc = kIOReturnBadArgument; break; }
        uint64_t fenceAddr = 0;
        if (hasFence) {
            IOLockLock(gCliLock);
            const uint32_t fr = resolve_fence(s, v.fenceHandle, v.fenceOffset, &fenceAddr);
            IOLockUnlock(gCliLock);
            if (fr != kOk) { rc = (IOReturn)fr; break; }
        }
        if (v.ipType == RDNA4N_HW_IP_COMPUTE) {
            const uint32_t cer = s->pinnedVmid ? kcq_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut)
                                               : kcq_submit_pooled(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);
            if (cer != kOk) rc = (IOReturn)cer;
            break;
        }
        uint32_t er;
        if (!g5_on()) er = s->pinnedVmid ? ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut) : ring_submit_pooled(s, va, by, v.nIbs, hasFence, fenceAddr, false, seqOut);   // G5 OFF
        else if (s->ws) { __atomic_store_n(&gWsLastNs, now_ns(), __ATOMIC_RELEASE); er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut); }   // 0.0.650 (G5): WindowServer is stamped, never gated
        else er = ring_submit_app_g5(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // 0.0.650 (G5): every other session passes the admission
        if (er != kOk) { rc = (IOReturn)er; break; }
    } while (false);
    sess_unlock(s);
    return rc;
}

struct BufferEnv {
    Session *s;
    uint32_t gate() { return gpu_gate(s); }
    BufferBo bo(uint32_t h) {
        if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, h)) return BufferBo{0, false, false};
        return BufferBo{s->bo[h].size, true, s->bo[h].kind == kBoHost};
    }
    const VaEnt *maps() { return s->maps; }
    uint32_t map_count() { return kMaxMaps; }
    uint32_t idle() {
        if (own_wait(s)) return kOk;
        return sess_poisoned(s) || hung_now() ? kAborted : kTimeout;
    }
    uint32_t prepare() {
        if (s->bufferIbPa == 0) {
            // 3.1: transactional single-page map for the kernel IB. kBufferIbVa (0x1000) is
            // below the user floor (kVaFirst) so pt_va_precheck must not apply; snapshot +
            // pt_map + rollback on failure still avoids leaking tables and returns blocks to
            // the pt reserve (never vram_alloc). alloc_page already enforces the per-block
            // client/global quota (free-reserve reuse bypasses global/visible).
            IOLockLock(gCliLock);
            PtMem mem{ s };
            const PtSnap snap = mem.snapshot();
            const uint64_t pa = mem.alloc_page();
            uint32_t rc = kNoMemory;
            if (pa != 0) {
                rc = pt_map(s->rootPa, kBufferIbVa, 1, pa,
                            leaf_flags(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_EXECUTABLE, true, false), mem);
                if (rc != kOk) {
                    pt_unmap(s->rootPa, kBufferIbVa, 1, mem);
                    mem.rollback(snap, s->rootPa, kBufferIbVa, 1);
                    amdgpu_hdp_flush(*gCtx->dev);
                    const uint32_t f = session_flush_if_bound_locked(s);
                    if (f != kOk) { mem.quarantine_rollback(); rc = f; }
                }
            }
            IOLockUnlock(gCliLock);
            if (rc != kOk) return rc;
            amdgpu_hdp_flush(*gCtx->dev);
            rc = session_pt_flush(s);
            if (rc != kOk) return rc;
            s->bufferIbPa = pa;
        }
        return kOk;
    }
    uint32_t submit(const uint32_t *packet, uint32_t n, uint64_t *seqOut) {
        bar0_memcpy_to_vram(*gCtx->dev, s->bufferIbPa - gCtx->gmc.vram_base_offset, packet, n * 4);
        amdgpu_hdp_flush(*gCtx->dev);
        const uint64_t va = kBufferIbVa;
        const uint32_t bytes = n * 4;
        uint32_t rc;
        s->bufferSubmitting = true;
        if (!g5_on()) rc = s->pinnedVmid ? ring_submit(s, &va, &bytes, 1, false, 0, seqOut) : ring_submit_pooled(s, &va, &bytes, 1, false, 0, false, seqOut);
        else if (s->ws) {
            __atomic_store_n(&gWsLastNs, now_ns(), __ATOMIC_RELEASE);
            rc = ring_submit(s, &va, &bytes, 1, false, 0, seqOut);
        } else rc = ring_submit_app_g5(s, &va, &bytes, 1, false, 0, seqOut);
        s->bufferSubmitting = false;
        return rc;
    }
};

IOReturn n1c_buffer_op(const N1cRef &ref, const rdna4n_buffer_op *in, bool fill, uint64_t *seqOut) {
    if (!in || !seqOut) return kIOReturnBadArgument;
    *seqOut = 0;
    if (!sess_hello(ref)) return kIOReturnNotReady;
    const rdna4n_buffer_op q = *in;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    BufferEnv env{s};
    const uint32_t rc = buffer_run(env, q, fill, seqOut);
    sess_unlock(s);
    return (IOReturn)rc;
}

static IOReturn n1c_wait_kcq(Session *s, uint64_t target, uint64_t timeoutNs, uint64_t out[3]) {
    if (!async_compute_on()) return kIOReturnBadArgument;
    const uint64_t rawTarget = seq_raw(target);
    uint64_t em = kcq_emitted_now();
    if (rawTarget > em) return kIOReturnBadArgument;
    const uint64_t to = wait_timeout_ns(timeoutNs);
    const uint64_t start = now_ns();
    uint64_t re = 0;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        em = kcq_emitted_now();
        re = kcq_retired_now(em);
        if (sess_poisoned(s)) return kIOReturnAborted;
        if (rawTarget == 0ull || re >= rawTarget) {
            out[0] = 0;
            out[1] = seq_with_ring(re, true);
            out[2] = seq_with_ring(em, true);
            return kIOReturnSuccess;
        }
        if (hung_now()) return kIOReturnAborted;
        const uint64_t el = now_ns() - start;
        if (el >= to) break;
        n1c_wait_relax(el, to - el, ep);
    }
    if (kcq_hang_poll()) return kIOReturnTimeout;
    if (sess_poisoned(s)) return kIOReturnAborted;
    out[0] = 1;
    out[1] = seq_with_ring(re, true);
    out[2] = seq_with_ring(em, true);
    return kIOReturnSuccess;
}

IOReturn n1c_wait(const N1cRef &ref, uint64_t target, uint64_t timeoutNs, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (sess_poisoned(s)) return kIOReturnAborted;   // 0.0.627 (G2): a poisoned session's fences are failed
    if (seq_is_compute(target)) return n1c_wait_kcq(s, target, timeoutNs, out);
    uint64_t res = 0;
    uint64_t em = emitted_now();
    const uint32_t rrc = rdna4native::g2::own_wait_resolve(target, em, __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE), &res);   // 0.0.627: LAST = this session's own last seqno
    if (rrc != kOk) return (IOReturn)rrc;
    const uint64_t to = wait_timeout_ns(timeoutNs);
    const uint64_t start = now_ns();
    uint64_t re = 0;
    if (n1c_irq_wait_enabled()) (void)lazy_lock(&gWaitLock);
    for (;;) {
        const uint64_t ep = irq_epoch_now();
        em = emitted_now();
        re = retired_now(em);
        if (sess_poisoned(s)) return kIOReturnAborted;
        if (res == 0ull || re >= res) { out[0] = 0; out[1] = re; out[2] = em; return kIOReturnSuccess; }
        if (hung_now()) return kIOReturnAborted;
        const uint64_t el = now_ns() - start;
        if (el >= to) break;
        n1c_wait_relax(el, to - el, ep);
    }
    if (hang_poll()) return kIOReturnTimeout;                // this call's wait detected the hang
    if (sess_poisoned(s)) return kIOReturnAborted;           // 0.0.627: this call's stall named the caller guilty
    out[0] = 1; out[1] = re; out[2] = em;
    return kIOReturnSuccess;
}

// ==== 0.0.627: GPU-apps stage G2, several sessions (boot-arg garnet-multisession=1; default OFF) =================================================
// The pure half is garnet_multisession_pure.h (host-tested by tests/native_g2_test.cpp). The latch and the stat verb (accel action 103 `sessstat <0..8>`).
static volatile UInt32 gMultiLatch = rdna4native::g2::kLatchUnset;
bool multi_on() {
    UInt32 l = gMultiLatch;
    if (l == rdna4native::g2::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-multisession", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::g2::kLatchUnset, rdna4native::g2::latch_value(present, v), &gMultiLatch))
            N1C_LOG("boot-arg garnet-multisession %s: native sessions are %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
					 rdna4native::g2::latch_is_on(gMultiLatch) ? "MULTIPLE (up to 64 sessions, VMID 8 pinned + 9..15 pooled)" : "SINGLE (VMID 8, one open at a time)");
        l = gMultiLatch;
    }
    return rdna4native::g2::latch_is_on(l);
}
bool n1c_multi_latched_on() { return multi_on(); }

// ==== 0.0.640: GPU-apps stage G4, ordinary user applications (boot-arg garnet-apps=1 AND garnet-multisession=1; default OFF) ====================================
// The pure half is garnet_app_policy_pure.h (host-tested by tests/native_g4_test.cpp). The latch, the allow-list, the role mark and the verb (accel action 104 `appallow`).
static volatile UInt32 gAppsLatch = rdna4native::g4::kLatchUnset;
bool apps_on() {
    UInt32 l = gAppsLatch;
    if (l == rdna4native::g4::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-apps", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::g4::kLatchUnset, rdna4native::g4::latch_value(present, v), &gAppsLatch))
            N1C_LOG("boot-arg garnet-apps %s: user applications are %s for this boot (they also need garnet-multisession=1, which is %s)", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    rdna4native::g4::latch_is_on(gAppsLatch) ? "ADMITTED when allow-listed" : "NOT admitted", multi_on() ? "ON" : "OFF");
        l = gAppsLatch;
    }
    return rdna4native::g4::apps_enabled(rdna4native::g4::latch_is_on(l), multi_on());
}
bool n1c_apps_enabled() { return apps_on(); }
static volatile UInt32 gAppsAllLatch = rdna4native::g4::kLatchUnset;
static bool apps_all_on() {
    UInt32 l = gAppsAllLatch;
    if (l == rdna4native::g4::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-apps-all", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::g4::kLatchUnset, rdna4native::g4::latch_value(present, v), &gAppsAllLatch))
            N1C_LOG("boot-arg garnet-apps-all %s: application allow-list bypass %s (requires garnet-apps=1 AND garnet-multisession=1; strike limit still applies)", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    rdna4native::g4::latch_is_on(gAppsAllLatch) ? "ON" : "OFF");
        l = gAppsAllLatch;
    }
    return rdna4native::g4::latch_is_on(l);
}
// ==== 0.0.650: GPU-apps stage G5 Stage 1, the application credit (boot-arg rdna4-g5=1 AND garnet-multisession=1; default OFF) ====================================
// The pure half is garnet_app_credit_pure.h (host-tested by tests/native_g5_test.cpp). The latches and the stat page (accel 103 `sessstat 9`).
static volatile UInt32 gG5Latch = rdna4native::g2::kLatchUnset;
static bool g5_on() {
    UInt32 l = gG5Latch;
    if (l == rdna4native::g2::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-g5", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::g2::kLatchUnset, rdna4native::g2::latch_value(present, v), &gG5Latch))
            N1C_LOG("boot-arg garnet-g5 %s: the application credit is %s for this boot (it also needs garnet-multisession=1, which is %s)", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    rdna4native::g2::latch_is_on(gG5Latch) ? "ARMED" : "OFF", multi_on() ? "ON" : "OFF");
        l = gG5Latch;
    }
    return rdna4native::g5::on_effective(rdna4native::g2::latch_is_on(l), multi_on());
}
bool n1c_g5_latched_on() { return g5_on(); }
static volatile UInt32 gG5CreditLatch;   // 0 = unread, 1 = latched
static uint32_t gG5Credit = rdna4native::g5::kCreditDefault;
static uint32_t g5_credit() {
    if (__atomic_load_n(&gG5CreditLatch, __ATOMIC_ACQUIRE) == 0u) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-g5credit", &v, sizeof(v));
        gG5Credit = rdna4native::g5::credit_value(present, v);   // idempotent: every racing first reader computes the same value
        __atomic_store_n(&gG5CreditLatch, 1u, __ATOMIC_RELEASE);
        N1C_LOG("boot-arg garnet-g5credit %s: at most %u application job(s) in flight ahead of WindowServer while it is active (G5 ON only)", !present ? "absent" : (v >= 1u && v <= rdna4native::g5::kCreditMax) ? "read" : "out of range 1..4 (default used)", gG5Credit);
    }
    return gG5Credit;
}
// accel 103 `sessstat 9`. Read-only. The caller (accelExperiment) checked rdna4native::g5::args_ok. o[4] and o[5] are read under ONE hold of gRingLock (never nested in gCliLock).
IOReturn n1c_g5_stat(uint64_t *o) {
    using namespace rdna4native::g5;
    for (uint32_t i = 0; i < rdna4native::g2::kOutN; i++) o[i] = 0;
    if (!g5_on()) { o[0] = rdna4native::g2::kStatBadArg; return kIOReturnBadArgument; }
    StatView v {};
    v.K = g5_credit();
    const uint64_t now = now_ns();
    const uint64_t last = __atomic_load_n(&gWsLastNs, __ATOMIC_ACQUIRE);
    const uint32_t wsId = __atomic_load_n(&gWsId, __ATOMIC_SEQ_CST);
    v.wsOpen = wsId != 0u;
    v.wsActive = ws_active(v.wsOpen, last, now, kWsWindowNs);
    v.sinceWsMs = (last != 0ull && now >= last) ? (now - last) / 1000000ull : kNever;
    if (gRingLock != nullptr && gCtx != nullptr) {
        IOLockLock(gRingLock);
        const uint64_t em = emitted_now(), re = retired_now(em);
        v.appInFlight = app_in_flight(gOwners, re, em, wsId, rdna4native::g2::kOwnerN);
        v.wsOutstanding = ws_outstanding(gOwners, re, em, wsId);
        v.st = gG5;
        IOLockUnlock(gRingLock);
    }
    stat_out(v, o);
    return kIOReturnSuccess;
}
bool n1c_ws_is_open() { return __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u; }   // 0.0.641: a WindowServer session is open (the open policy's alreadyOpen input with apps ON)
// 0.0.641: the non-WindowServer visible-pool cap, boot-arg rdna4-appvis=<MiB> (16..128; default 64), read once.
static volatile UInt32 gAppVisLatch;   // 0 = unread, 1 = latched
static uint64_t gAppVisCap;
uint64_t appvis_cap() {
    if (__atomic_load_n(&gAppVisLatch, __ATOMIC_ACQUIRE) == 0u) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-appvis", &v, sizeof(v));
        gAppVisCap = rdna4native::g4::appvis_cap_bytes(present, v);   // idempotent: every racing first reader computes the same value
        __atomic_store_n(&gAppVisLatch, 1u, __ATOMIC_RELEASE);
        N1C_LOG("boot-arg garnet-appvis %s: non-WindowServer sessions may hold %llu MiB of the visible VRAM pool together (apps ON only)", !present ? "absent" : (v >= rdna4native::g4::kAppVisMinMiB && v <= rdna4native::g4::kAppVisMaxMiB) ? "read" : "out of range 16..128 (default used)", (unsigned long long)(gAppVisCap >> 20));
    }
    return gAppVisCap;
}
// Is this process name listed (or garnet-apps-all=1) AND not struck out? (false at once with the role disabled.) *struckOut: the name is eligible but has
// rdna4native::g4::kStrikeLimit strikes (automatic recoveries blamed on an APP session of that name this boot): refused at open, with kReasonAppNotAllowed, like an unlisted name.
bool n1c_app_allowed(const char *comm, bool *struckOut) {
    *struckOut = false;
    if (!apps_on()) return false;
    const uint64_t key = rdna4a_comm_key(comm);
    const bool listed = rdna4native::g4::al_contains(gAppAllow, key);
    const bool denied = rdna4native::g4::dl_contains(gAppDeny, key);
    const bool all = apps_all_on();
    *struckOut = !denied && (listed || all) && rdna4native::g4::strike_refused(rdna4native::g4::strike_count(gStrikes, key));
    return rdna4native::g4::app_admit(listed, rdna4native::g4::strike_count(gStrikes, key), all, denied);
}
// Mark the session the client was just opened with as an APP. Called by create() right after n1c_open, before the client is returned (so no selector can have run). A stale reference
// or a disabled role is refused; the client then closes the session again. 0.0.641: `key` = the opener's executable-name key (for the strike count).
IOReturn n1c_mark_app(const N1cRef &ref, uint64_t key) {
    if (!apps_on()) return kIOReturnNotPermitted;
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return kIOReturnNotReady;
    s->appKey = key;
    __atomic_store_n(&s->app, true, __ATOMIC_RELEASE);
    sess_unlock(s);
    return kIOReturnSuccess;
}

// 0.0.641: the "open refused" log gate (rdna4native::g4::log_gate_step). true = log this line; *suppressed = lines dropped in the previous window (report them once). Log-only.
bool n1c_log_gate(uint32_t *suppressed) {
    bool admit = false;
    const uint64_t sec = now_ns() / 1000000000ull;
    uint64_t w = __atomic_load_n(&gLogGate, __ATOMIC_RELAXED);
    for (;;) {
        const uint64_t nw = rdna4native::g4::log_gate_step(w, sec, &admit, suppressed);
        if (__atomic_compare_exchange_n(&gLogGate, &w, nw, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
    }
    return admit;
}
// accel 104 `appallow`. The caller (accelExperiment) checked the role and the argument; this runs the pure verb on the kernel's list and logs the change.
IOReturn n1c_app_verb(uint64_t arg, uint64_t *o) {
    using namespace rdna4native::g4;
    const uint32_t rc = verb_run(apps_on(), gAppAllow, arg, o, &gStrikes);
    if (rc == kAllowOk && o[2] != RDNA4A_OP_LIST) N1C_LOG("appallow: %s key %#llx -> %u entr%s on the allow-list", o[2] == RDNA4A_OP_ADD ? "ADDED" : "REMOVED", (unsigned long long)o[4], (unsigned)o[1], o[1] == 1ull ? "y" : "ies");
    return rc == kAllowOk || rc == kAllowExists || rc == kAllowNotFound ? kIOReturnSuccess : rc == kAllowFull ? kIOReturnNoSpace : rc == kAllowOff ? kIOReturnNotPermitted : kIOReturnBadArgument;
}
// accel 110 `appdeny`. Lock-free 64-entry denylist overriding garnet-apps-all=1 and appallow.
IOReturn n1c_app_deny_verb(uint64_t arg, uint64_t *o) {
    using namespace rdna4native::g4;
    const uint32_t rc = deny_verb_run(apps_on(), gAppDeny, arg, o);
    if (rc == kAllowOk && o[2] != RDNA4A_OP_LIST) N1C_LOG("appdeny: %s key %#llx -> %u entr%s on the denylist", o[2] == RDNA4A_OP_ADD ? "ADDED" : o[2] == RDNA4A_OP_CLEAR ? "CLEARED" : "REMOVED", (unsigned long long)o[4], (unsigned)o[1], o[1] == 1ull ? "y" : "ies");
    return rc == kAllowOk || rc == kAllowExists || rc == kAllowNotFound ? kIOReturnSuccess : rc == kAllowFull ? kIOReturnNoSpace : rc == kAllowOff ? kIOReturnNotPermitted : kIOReturnBadArgument;
}
// accel 103 `sessstat <page>`: page 0..7 or 100..163 one session slot, page 8 the global page. Read-only. The caller (accelExperiment) checked the latch and the argument.
IOReturn n1c_g2_stat(uint64_t page, uint64_t *o) {
    using namespace rdna4native::g2;
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0;
    if (!multi_on()) { o[0] = kStatOff; return kIOReturnSuccess; }
    if (!args_ok(kActSessStat, page)) { o[0] = kStatBadArg; return kIOReturnBadArgument; }
    if (gCliLock == nullptr || gCtx == nullptr) { o[0] = kStatOk; o[1] = stat_slot_of_page(page); return kIOReturnSuccess; }   // nothing ever opened this boot
    if (page == (uint64_t)kStatGlobal) {
        GlobalView g {};
        IOLockLock(gCliLock);
        g.open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST);
        for (uint32_t i = 0; i < kMaxSessions; i++) if (gSlotState[i] == kSlotDead) g.dead++;
        g.othersVram = gVramOthers;
        {   // the shared allowance from the same snapshot: pools, held bytes and page-table blocks (all under gCliLock here)
            const uint64_t poolTotal = sat_add(gCtx->gmc.vram_alloc.size(), gCtx->gmc.vram_alloc_hi.is_inited() ? gCtx->gmc.vram_alloc_hi.size() : 0ull);
            const uint64_t poolFree = sat_add(gCtx->gmc.vram_alloc.bytes_free(), gCtx->gmc.vram_alloc_hi.is_inited() ? gCtx->gmc.vram_alloc_hi.bytes_free() : 0ull);
            g.othersCap = app_vram_cap(poolTotal, poolFree, gVramOthers, pt_held_bytes(gPtN, kArenaBlockBytes));
        }
        const bool fullBar = gCtx->dev != nullptr && rdna4rebar::is_full_bar_aperture(gCtx->dev->bar0Size, gCtx->dev->vramLimit);
        if (rdna4native::g4::appvis_active(apps_on(), fullBar)) { g.appVisBytes = gVisOthers; g.appVisCapMiB = (uint32_t)(appvis_cap() >> 20); }   // 0.0.641: the visible-pool use of non-WindowServer sessions and its cap (small BAR only)
        IOLockUnlock(gCliLock);
        g.hung = hung_now(); g.emitted = emitted_now(); g.retired = retired_now(g.emitted);
        g.mappedLeakBos = gMappedLeakBos;   // 0.0.640 (G4): BOs leaked at close because a CPU mapping outlived the session (read without gCliLock: a diagnostic)
        g.recoveries = gG2.recoveries; g.lastGuiltySlot = gG2.lastGuiltySlot; g.lastGuiltySeq = gG2.lastGuiltySeq; g.collateralTotal = gG2.collateralTotal;
        g.faultNow = fault_snap("sessstat", kStatGlobal);   // R5: read only
        IOLockLock(gCliLock);
        g.faultPrev = gFaultPrev; gFaultPrev = g.faultNow;  // the previous call's word, then this call's is kept for the next
        IOLockUnlock(gCliLock);
        if (fault_changed(g.faultPrev, g.faultNow)) N1C_LOG("sessstat: the gcvm fault word CHANGED since the previous sessstat 8: %#llx -> %#llx", (unsigned long long)g.faultPrev, (unsigned long long)g.faultNow);
        global_out(g, o);
        return kIOReturnSuccess;
    }
    const uint32_t slot = stat_slot_of_page(page);
    SessView v {};
    IOLockLock(gCliLock);
    v.state = gSlotState[slot];
    Session *s = slot_ptr(slot);
    if (s != nullptr && __atomic_load_n(&s->open, __ATOMIC_ACQUIRE)) {   // the counters are read under gCliLock (a racing selector may be mid-update: a diagnostic)
        v.vmid = __atomic_load_n(&s->vmid, __ATOMIC_ACQUIRE);
        v.ws = s->ws; v.app = s->app; v.hello = s->hello; v.poisoned = sess_poisoned(s); v.id = s->id;   // 0.0.640 (G4): app
        v.vram = s->vramBytes; v.gtt = s->gttUsed; v.imported = s->importedBytes; v.ptBytes = (uint64_t)s->nblk * kArenaBlockBytes;
        v.lastSeq = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE); v.nBos = s->nBos;
        v.faultAtOpen = s->faultAtOpen;   // 0.0.629 (R5)
    }
    IOLockUnlock(gCliLock);
    sess_out(slot, v, o);
    return kIOReturnSuccess;
}

// ==== 0.0.623: GPU-apps stage G1, hang recovery (boot-arg rdna4-g1=1; default OFF) ================================================================
// The pure half is garnet_hang_recovery_pure.h (decisions, plan, bounds, the run templates; host-tested by tests/native_g1_test.cpp). This is the kernel
// environment those templates run against. What it may touch, and nothing else:
//   - a kernel-owned TEST CONTEXT: the ordinary native session (n1c_open: every gate, the exclusivity CAS, a fresh VMID-8 tree) that no user client
//     owns (hello stays false, so no selector reaches it) with ONE 16 KiB visible-VRAM BO mapped R|W|X|UC at g1::kTestVa;
//   - the kernel GFX ring through ring_submit (the one ring writer; 0.0.623-0.0.626: ring_emit), and, for the recovery methods only: ring memory outside the live range (NOPs), the
//     MES RESET / REMOVE_QUEUE / ADD_QUEUE frames for GFX pipe 0 queue 0 (the queue amdgpu_init's PM4Test mapped with map_legacy_kq), the MQD memory
//     (cp_gfx_mqd_init: memory only), the CP write-back rptr / wptr words and the software ring counters;
//   - the HUNG latch: set through hang_from_wait (the client path), cleared ONLY by G1Env::clear_latch, which only recover_run calls after a VERIFIED probe.
// It writes no register ITSELF (no register-write macro appears in this block), but the calls it makes do (0.0.626 correction; see the header comment): the MES and ring doorbells, GRBM_GFX_CNTL (cp_dump_gfx_hqd), the TLB invalidate (flush_vmid), CONTEXT8 (program_root) and the HDP flush. It never touches PSP, SMU, DCN or a GRBM soft reset.
static volatile UInt32 gG1Latch = rdna4native::g1::kLatchUnset;
bool n1c_g1_latched_on() {
    UInt32 l = gG1Latch;
    if (l == rdna4native::g1::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("garnet-g1", &v, sizeof(v));
        if (OSCompareAndSwap(rdna4native::g1::kLatchUnset, rdna4native::g1::latch_value(present, v), &gG1Latch))
            N1C_LOG("boot-arg garnet-g1 %s: the G1 hang-recovery verbs (hangtest / hangrecover / hangstat) are %s for this boot",
                    present ? (v == 1u ? "=1" : "present but not 1") : "absent", rdna4native::g1::latch_is_on(gG1Latch) ? "ENABLED" : "OFF");
        l = gG1Latch;
    }
    return rdna4native::g1::latch_is_on(l);
}

namespace {
struct G1State {
    bool holds;                    // the test context holds the native session (written only under gG1Lock)
    bool resetAcked;               // the MOST RECENT MES RESET on THIS held hang was acked (remap then adds remove_queue_after_reset, sched >= 0x5a; a standalone remap needs it)
    uint64_t hungSeq;              // the seqno of the hang IB
    uint64_t liveStart;            // the write counter at the start of the oldest unretired submission (the hang IB's, or 0 after a remap)
    uint64_t boOff;                // the test BO's BAR0 offset
    uint32_t attempts;             // attempts made on this held hang
    rdna4native::g1::Attempt att[rdna4native::g1::kMaxAttempts];
    uint32_t attRptr[rdna4native::g1::kMaxAttempts], attCpStat[rdna4native::g1::kMaxAttempts];
    uint32_t lastCode, lastMethod, tests, recovered, failedRuns;
    N1cRef   ref;                  // 0.0.627: the test context's session (slot 0, VMID 8)
};
G1State gG1;
IOLock *gG1Lock;
}

bool g1_holds() { return gG1.holds; }   // 0.0.627 (G2): n1c_open refuses every open while a test context holds the GPU (switch ON)
static uint32_t g1_cp_stat() { return RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, IPBlock::GC, 0, 0x0F40u)); }   // CP_STAT (read only), as hang_log
static uint32_t g1_rptr() { sysmem_rmb(); return gCtx->cp.rptr_cpu ? *gCtx->cp.rptr_cpu : 0u; }

// NOP every ring dword outside the live range [gG1.liveStart, gWc). Caller holds gCliLock; the ring lock is taken here.
static void g1_scrub_ring() {
    CPContext &cp = gCtx->cp;
    IOLockLock(gRingLock);
    const uint32_t mask = cp.ring_ptr_mask;
    auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
    uint32_t n = 0;
    if (gWc - gG1.liveStart <= (uint64_t)cp.ring_size_dwords) {
        for (uint32_t i = 0; i < cp.ring_size_dwords; i++)
            if (!rdna4native::g1::in_live_range(i, gG1.liveStart, gWc, mask)) { ring[i] = cp_p3_nop1(); n++; }
    }
    sysmem_wmb();
    IOLockUnlock(gRingLock);
    N1C_LOG("g1: ring scrub: %u of %u dwords outside the live range [%llu, %llu) set to NOP", n, cp.ring_size_dwords,
            (unsigned long long)gG1.liveStart, (unsigned long long)gWc);
}

struct G1Env {
    BringupContext *ctx;
    uint64_t now_ns() { return amdgpu::now_ns(); }
    void relax(uint64_t el) { if (el < 5000000ull) IODelay(1); else IOSleep(1); }
    uint64_t retired() { return retired_now(emitted_now()); }

    // ---- hangtest ----
    uint32_t open_test() {
        __atomic_store_n(&gG1OpenThread, current_thread(), __ATOMIC_RELEASE);
        const IOReturn rc = n1c_open(*ctx, false, &gG1.ref);
        __atomic_store_n(&gG1OpenThread, nullptr, __ATOMIC_RELEASE);
        if (rc == kIOReturnSuccess) { gG1.holds = true; gG1.resetAcked = false; gG1.attempts = 0; }
        return (uint32_t)rc;
    }
    uint32_t setup_bo() {
        using namespace rdna4native::g1;
        DeviceContext &dev = *gCtx->dev;
        IOLockLock(gCliLock);
        Session *s = &gSess;
        uint32_t rc = kOk;
        do {
            if (!s->open || s->id != gG1.ref.id || gG1.ref.slot != 0u || s->hello || s->nBos != 0u) { rc = kNotReady; break; }   // the context is ours: fresh, no client, no BO
            VRAMAllocation a {};
            if (!gCtx->gmc.vram_alloc.alloc(kTestBytes, kArenaBlockBytes, &a)) { rc = kNoMemory; break; }
            const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
            if (a.size < kTestBytes || !rdna4rebar::vis_vram_range_ok(a.gpu_va, a.size, gCtx->gmc.vram_start, dev.vramLimit, dev.bar0Size, dev.vramSizeBytes)) { gCtx->gmc.vram_alloc.free(a); rc = kNoMemory; break; }
            bar0_memset_vram(dev, off, 0, kTestBytes);
            Bo b {};
            b.kind = kBoVis; b.uc = true; b.size = kTestBytes; b.mc = a.gpu_va; b.a = a;
            s->bo[1] = b; s->boUsed[1] = 1; s->nBos = 1; s->vramBytes += kTestBytes;   // an ordinary BO of the session: close frees it, HUNG leaks it
            gG1.boOff = off;
            const uint32_t vmf = RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE | RDNA4N_VM_PAGE_EXECUTABLE | RDNA4N_VM_MTYPE_UC;
            PtMem mem{ s };
            rc = pt_map(s->rootPa, kTestVa, kTestBytes / kPage, pa_of_mc(a.gpu_va), leaf_flags(vmf, true, false), mem);
            if (rc != kOk) break;
            s->maps[0] = VaEnt{ kTestVa, kTestBytes, 0, 1u, vmf };
            amdgpu_hdp_flush(dev);
            rc = flush_vmid(kNativeVmid);
        } while (false);
        IOLockUnlock(gCliLock);
        N1C_LOG("g1 hangtest: test BO %s (rc %#x): 16 KiB visible VRAM at BAR0 %#llx mapped R|W|X|UC at VMID-8 VA %#llx",
                rc == kOk ? "ready" : "NOT ready", rc, (unsigned long long)gG1.boOff, (unsigned long long)kTestVa);
        return rc;
    }
    rdna4native::g1::ProbeSub submit_hang() {
        using namespace rdna4native::g1;
        DeviceContext &dev = *gCtx->dev;
        ProbeSub ps{ kOk, 0ull, 0u };
        IOLockLock(gCliLock);
        uint32_t ib[kWaitDw];
        const uint32_t ndw = emit_wait_never(ib, kTestVa + kPollOff);
        bar0_memcpy_to_vram(dev, gG1.boOff + kHangIbOff, ib, ndw * 4u);
        amdgpu_hdp_flush(dev);
        const uint64_t va = kTestVa + kHangIbOff;
        const uint32_t by = ndw * 4u;
        uint64_t seq = 0;
        gG1.liveStart = gWc;
        ps.rc = ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq);   // 0.0.627: the one submit path (seqno, packet and ring under gRingLock), VMID 8 (slot 0)
        ps.seq = seq;
        gG1.hungSeq = seq;
        IOLockUnlock(gCliLock);
        N1C_LOG("g1 hangtest: WAIT_REG_MEM(equal %#x, poll VA %#llx) IB at VA %#llx submitted as seq %llu (rc %#x), ring [%llu, %llu)",
                kWaitRefNever, (unsigned long long)(kTestVa + kPollOff), (unsigned long long)va, (unsigned long long)seq, ps.rc,
                (unsigned long long)gG1.liveStart, (unsigned long long)gWc);
        return ps;
    }
    void latch_hang() {
        N1C_LOG("g1 hangtest: seq %llu did not retire in 2 s: latching HUNG through the client path (rptr %u, CP_STAT %#x)",
                (unsigned long long)gG1.hungSeq, g1_rptr(), g1_cp_stat());
        hang_from_wait();
    }
    void close_test(const char *how) {
        n1c_close(gG1.ref, how);
        gG1.holds = false;
    }

    // ---- hangrecover ----
    uint32_t act(uint32_t method, bool resetAcked) {
        using namespace rdna4native::g1;
        DeviceContext &dev = *gCtx->dev;
        CPContext &cp = gCtx->cp;
        MESContext &mes = gCtx->mes;
        if (method == kMethRelease) {
            IOLockLock(gCliLock);
            const uint32_t v = kWaitRefNever;
            bar0_memcpy_to_vram(dev, gG1.boOff + kPollOff, &v, sizeof(v));
            amdgpu_hdp_flush(dev);
            IOLockUnlock(gCliLock);
            N1C_LOG("g1 hangrecover release: the CPU wrote %#x to the poll dword (a harness control: the WAIT_REG_MEM may now end)", v);
            return kOk;
        }
        if (method == kMethMesReset) {
            IOLockLock(gCliLock);
            g1_scrub_ring();
            IOLockUnlock(gCliLock);
            const kern_return_t r = mes_reset_legacy_gfx_queue(dev, mes, 0u, 0u, kNativeVmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);
            gG1.resetAcked = (r == kIOReturnSuccess);   // 0.0.626 (F2): the MOST RECENT RESET's verdict (a later failed RESET clears it), never sticky-true
            N1C_LOG("g1 hangrecover mesreset: MES RESET(reset_legacy_gfx, pipe 0 queue 0 vmid %u) %s (%#x); rptr %u CP_STAT %#x", kNativeVmid,
                    r == kIOReturnSuccess ? "ACKED" : "NOT acked", r, g1_rptr(), g1_cp_stat());
            return (uint32_t)r;
        }
        if (method == kMethRemap) {
            const bool after = unmap_after_reset(resetAcked || gG1.resetAcked, mes.sched_version & kMES_VERSION_MASK);
            const uint32_t fl = kRemoveQueueFlag_unmap_legacy_queue | (after ? kRemoveQueueFlag_remove_after_reset : 0u);
            kern_return_t r = mes_remove_hw_queue_flags(dev, mes, kMESQueueType_GFX, 0u, 0u, cp.doorbell_index, fl);
            N1C_LOG("g1 hangrecover remap: MES REMOVE_QUEUE(GFX pipe 0 queue 0, flags %#x) %s (%#x)", fl, r == kIOReturnSuccess ? "ACKED" : "NOT acked", r);
            if (r != kIOReturnSuccess) return (uint32_t)r;    // the queue's state is unknown: no re-init, no ADD_QUEUE
            cp.kgq_mapped = false; gCtx->kgqMapped = false;
            // amdgpu_gfx_mqd_reset_restore: ring->wptr = 0, *wptr_cpu_addr = 0, amdgpu_ring_clear_ring (the whole ring to NOP).
            IOLockLock(gCliLock);
            IOLockLock(gRingLock);
            auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
            for (uint32_t i = 0; i < cp.ring_size_dwords; i++) ring[i] = cp_p3_nop1();
            *cp.rptr_cpu = 0u;
            *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = 0ull;
            gWc = 0ull; cp.wptr = 0u; gG1.liveStart = 0ull;
            sysmem_wmb();
            IOLockUnlock(gRingLock);
            IOLockUnlock(gCliLock);
            r = cp_gfx_mqd_init(dev, cp);                       // memory only: the MQD as the boot built it (rptr 0, wptr 0, active 1)
            if (r == kIOReturnSuccess) r = cp_map_gfx_kgq_mes(dev, cp, mes);   // MES ADD_QUEUE(map_legacy_kq), as PM4Test at boot
            if (r == kIOReturnSuccess) gCtx->kgqMapped = true;
            N1C_LOG("g1 hangrecover remap: ring cleared, counters 0, MQD re-initialised, ADD_QUEUE(map_legacy_kq) %s (%#x); rptr %u CP_STAT %#x",
                    r == kIOReturnSuccess ? "ACKED" : "NOT acked", r, g1_rptr(), g1_cp_stat());
            return (uint32_t)r;
        }
        return kBadArg;
    }
    rdna4native::g1::ProbeSub submit_probe(uint32_t idx) {
        using namespace rdna4native::g1;
        DeviceContext &dev = *gCtx->dev;
        ProbeSub ps{ kOk, 0ull, probe_magic(idx) };
        IOLockLock(gCliLock);
        const uint32_t zero = 0u;
        bar0_memcpy_to_vram(dev, gG1.boOff + kProbeOff, &zero, sizeof(zero));
        uint32_t ib[5];
        ib[0] = pm4_header(kPM4OpWriteData, 3u);
        ib[1] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
        ib[2] = (uint32_t)((kTestVa + kProbeOff) & 0xFFFFFFFFull);
        ib[3] = (uint32_t)((kTestVa + kProbeOff) >> 32);
        ib[4] = ps.magic;
        bar0_memcpy_to_vram(dev, gG1.boOff + kProbeIbOff, ib, sizeof(ib));
        amdgpu_hdp_flush(dev);
        const uint64_t va = kTestVa + kProbeIbOff;
        const uint32_t by = (uint32_t)sizeof(ib);
        uint64_t seq = 0;
        ps.rc = ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq);   // 0.0.627: the one submit path
        ps.seq = seq;
        IOLockUnlock(gCliLock);
        return ps;
    }
    uint32_t probe_data() { return RBAR0_32(*gCtx->dev, gG1.boOff + rdna4native::g1::kProbeOff); }
    // The ONLY place HUNG is cleared. recover_run calls it only when may_clear (this attempt's probe retired AND read back).
    void clear_latch() {
        const uint64_t em = emitted_now(), re = retired_now(em);
        IOLockLock(gHangLock);
        gHang.hung = false;
        gHang.lastRetired = re;
        gHang.tProgress = amdgpu::now_ns();
        IOLockUnlock(gHangLock);
        N1C_LOG("g1: HUNG CLEARED after a verified probe (seq emitted %llu retired %llu); Garnet,Ready on the Metal nub stays 0 until reboot (sticky)",
                (unsigned long long)em, (unsigned long long)re);
    }
    void note(const rdna4native::g1::Attempt &a) {
        const uint32_t k = gG1.attempts;
        if (k < rdna4native::g1::kMaxAttempts) { gG1.att[k] = a; gG1.attRptr[k] = g1_rptr(); gG1.attCpStat[k] = g1_cp_stat(); gG1.attempts = k + 1u; }
        N1C_LOG("g1 attempt %u: method %u status %u act rc %#x (%llu us), probe rc %#x seq %llu retired %d data %d (%llu us); rptr %u CP_STAT %#x",
                k, a.method, a.status, a.actRc, (unsigned long long)(a.actNs / 1000ull), a.probeRc, (unsigned long long)a.probeSeq, (int)a.retired,
                (int)a.dataOk, (unsigned long long)(a.verifyNs / 1000ull), g1_rptr(), g1_cp_stat());
    }
};

static void g1_summary(uint64_t *o) {
    rdna4native::g1::Summary s {};
    const uint64_t em = emitted_now();
    s.lastCode = gG1.lastCode; s.phase = gG1.holds ? 1u : 0u; s.hung = hung_now(); s.testHolds = gG1.holds;
    s.hungSeq = gG1.hungSeq; s.emitted = em; s.retired = gCtx ? retired_now(em) : 0ull; s.wc = gWc;
    s.attempts = gG1.attempts; s.lastMethod = gG1.lastMethod; s.tests = gG1.tests; s.recovered = gG1.recovered; s.failedRuns = gG1.failedRuns;
    rdna4native::g1::summary_out(s, o);
}

// The three verbs (accel actions 100..102). The caller (accelExperiment) has checked the latch and the action bound; `o` has kOutN scalars.
// Returns the IOReturn for the user client; the verdict rides in o[0].
IOReturn n1c_g1_verb(uint32_t action, uint64_t arg, BringupContext &ctx, uint64_t *o) {
    using namespace rdna4native::g1;
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0;
    if (!args_ok(action, arg)) return kIOReturnBadArgument;
    if (lazy_lock(&gG1Lock) == nullptr) return kIOReturnNoMemory;
    IOLockLock(gG1Lock);
    IOReturn ret = kIOReturnSuccess;
    if (action == kActHangStat) {
        if (gCtx == nullptr) { o[0] = 0; }
        else if (arg == 0ull) g1_summary(o);
        else {
            const uint32_t k = (uint32_t)arg - 1u;
            if (k < gG1.attempts) attempt_out(gG1.att[k], gG1.attRptr[k], gG1.attCpStat[k], o);
        }
    } else if (action == kActHangTest) {
        const bool open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u;
        const bool ws = open && __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;
        const uint32_t d = test_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, open, ws, hung_now());
        if (d != kTestOk) {
            N1C_LOG("g1 hangtest REFUSED (%u): %s", d, d == kTestWsSession ? "a WindowServer native session is open" : d == kTestActive ? "a test context already holds the GPU"
                    : d == kTestSessionOpen ? "a native session is open" : d == kTestHung ? "the GPU is HUNG" : d == kTestBadMode ? "unknown mode" : "boot-arg rdna4-g1 is not 1");
            o[0] = d; gG1.lastCode = d;
        } else {
            gG1.tests++;
            G1Env e{ &ctx };
            const TestRep t = hangtest_run(e);
            o[0] = t.outcome; o[1] = t.rc; o[2] = t.seq;
            gG1.lastCode = t.outcome;
            if (gCtx != nullptr) {
                const uint64_t em = emitted_now();
                o[3] = em; o[4] = retired_now(em); o[5] = g1_cp_stat(); o[6] = g1_rptr(); o[7] = gWc; o[8] = hub_rd(0x15d0);
            }
            N1C_LOG("g1 hangtest: outcome %u (rc %#x, seq %llu)", t.outcome, t.rc, (unsigned long long)t.seq);
        }
    } else {   // kActHangRecover
        const Plan p = plan_for((uint32_t)arg);
        const uint32_t d = recover_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, hung_now(), gG1.attempts, p.n, mes_timeout_seen(), gG1.resetAcked);   // 0.0.626 F2/F3
        if (d != kRecOk) {
            N1C_LOG("g1 hangrecover %llu REFUSED (%u): %s", (unsigned long long)arg, d, d == kRecNoTest ? "no test context holds a hang (run hangtest 1 first)"
                    : d == kRecNotHung ? "HUNG is not latched" : d == kRecTooMany ? "the attempt budget for this hang is spent (abandon, or reboot)"
                    : d == kRecMesTimedOut ? "a MES frame timed out earlier in this boot: no further MES frame (only release and abandon remain)"
                    : d == kRecRemapNeedsReset ? "remap needs the most recent MES RESET on this hang to have been acknowledged (run mesreset first)"
                    : d == kRecBadMethod ? "unknown method" : "boot-arg rdna4-g1 is not 1");
            o[0] = d; gG1.lastCode = d;
        } else if ((uint32_t)arg == kMethAbandon) {
            G1Env e{ &ctx };
            e.close_test("g1 hangrecover: abandoned (HUNG stays latched)");
            o[0] = kRecAbandoned; gG1.lastCode = kRecAbandoned; gG1.lastMethod = kMethAbandon; gG1.failedRuns++;
        } else {
            G1Env e{ &ctx };
            const RecoverRep r = recover_run(e, (uint32_t)arg, gG1.attempts);
            gG1.lastMethod = (uint32_t)arg; gG1.lastCode = r.outcome;
            if (r.cleared) gG1.recovered++; else gG1.failedRuns++;
            o[0] = r.outcome; o[1] = r.n;
            for (uint32_t i = 0; i < r.n && i < kMaxPlan; i++) { o[2 + 3 * i] = r.a[i].status; o[3 + 3 * i] = r.a[i].actRc; o[4 + 3 * i] = r.a[i].verifyNs / 1000ull; }
            const uint64_t em = emitted_now();
            o[8] = r.cleared ? 1u : 0u; o[9] = hung_now() ? 1u : 0u; o[10] = em; o[11] = retired_now(em); o[12] = gG1.attempts;
            N1C_LOG("g1 hangrecover %llu: outcome %u after %u attempt(s); HUNG %s", (unsigned long long)arg, r.outcome, r.n,
                    hung_now() ? "STILL LATCHED (the test context keeps the GPU; try another method, hangrecover 4, or reboot)" : "cleared");
        }
    }
    IOLockUnlock(gG1Lock);
    return ret;
}

} // namespace amdgpu
