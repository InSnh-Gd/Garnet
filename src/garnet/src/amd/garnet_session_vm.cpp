// garnet_session_vm.cpp — Native VM page tables, VMID pool, session open/close,
// scanout pins/selectors, and display mode/DAL selectors (split from native_s1c.cpp).
#include "garnet_submit_internal.hpp"

namespace amdgpu {

uint8_t gSlotState[kMaxSessions];
VRAMAllocation gPt[kArenaReserve];
uint16_t gPtFree[kArenaReserve];
uint32_t gPtN, gPtFreeN;
static volatile UInt32 gBar0Conflict;
FenceWindow gWin[kMaxSessions];
ParkRoot gPark;
rdna4vmid::Pool gVmidPool;
bool gVmidPoolInited;
bool gVmid8Dead;
bool gVmidCtxReady[rdna4vmid::kSlots];
thread_t gG1OpenThread;

// Return a session's page-table blocks to the reserve (never to vram_alloc). Caller holds gCliLock.
void pt_release_blocks(Session *s) {
    for (uint32_t i = 0; i < s->nblk; i++) gPtFree[gPtFreeN++] = s->blk[i];
    s->nblk = 0;
}
uint32_t flush_vmid(uint32_t vmid) {
    if (gTlbLock != nullptr) IOLockLock(gTlbLock);
    const kern_return_t r = gmc_flush_gpu_tlb(*gCtx->dev, gCtx->gmc, gCtx->gmc.gfxhub, vmid, 0);
    if (gTlbLock != nullptr) IOLockUnlock(gTlbLock);
    if (r != kIOReturnSuccess) {
        N1C_LOG("TLB flush for VMID %u did not acknowledge (kr=%#x): treating as a hang", vmid, r);
        hang_from_wait();
        return kTimeout;
    }
    return kOk;
}
// The CONTEXTn register addresses of a session VMID (the same arithmetic as native_s1b.cpp, which programs CONTEXT8).
struct CtxRegs { uint32_t cntl, baseLo, baseHi, stLo, stHi, enLo, enHi; };
static CtxRegs ctx_regs(uint32_t vmid) {
    const HubContext &h = gCtx->gmc.gfxhub;
    DeviceContext &dev = *gCtx->dev;
    CtxRegs r;
    r.cntl   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_cntl) + (vmid - 1u) * h.ctx_distance;
    r.baseLo = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_lo) + vmid * h.ctx_addr_distance;
    r.baseHi = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_hi) + vmid * h.ctx_addr_distance;
    r.stLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_lo) + (vmid - 1u) * h.ctx_addr_distance;
    r.stHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_hi) + (vmid - 1u) * h.ctx_addr_distance;
    r.enLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_lo) + (vmid - 1u) * h.ctx_addr_distance;
    r.enHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_hi) + (vmid - 1u) * h.ctx_addr_distance;
    return r;
}
// DEPTH 3 / BLOCK 0 / RETRY 0 with the context enabled (the S1b geometry).
static inline bool ctx_geometry_ok(uint32_t cntl) {
    return (cntl & kCntlEnableMask) != 0u && ((cntl & kCntlDepthMask) >> kCntlDepthShift) == kCtxDepth && (cntl & kCntlRetryMask) == 0u &&
           ((cntl & kCntlBlockMask) >> kCntlBlockShift) == kCtxBlockSize;
}
// Point CONTEXT<vmid> at `rootPa` (VALID set, START 0, END max_pfn - 1) and flush that VMID's TLB. Caller holds gCliLock.
uint32_t program_root(uint32_t vmid, uint64_t rootPa) {
    DeviceContext &dev = *gCtx->dev;
    const CtxRegs r = ctx_regs(vmid);
    const uint64_t root = rootPa | kPteValid;
    WREG32(dev, r.baseLo, (uint32_t)(root & 0xFFFFFFFFu));
    WREG32(dev, r.baseHi, (uint32_t)(root >> 32));
    WREG32(dev, r.stLo, 0u);
    WREG32(dev, r.stHi, 0u);
    WREG32(dev, r.enLo, (uint32_t)((kMaxPfn - 1u) & 0xFFFFFFFFu));
    WREG32(dev, r.enHi, (uint32_t)((kMaxPfn - 1u) >> 32));
    return flush_vmid(vmid);
}

// ---- open / close -------------------------------------------------------------------------------------------------------------------
// 0.0.627 (G2): n1c_open's helpers, out of line so its frame stays within the stack rule (+0x40).
// CONTEXT<vmid> must carry the S1b geometry (DEPTH 3 / BLOCK 0 / RETRY 0): a context that lost it must not be pointed at a tree.
// VMIDs 9..15 still carry the boot geometry with RETRY 1 (hub_setup_vmid_config): their CNTL gets cntl_native on first bind, then is read back.
// CONTEXT8 itself is never written here (the S1b self-test owns it). Returns the CNTL read; *ok = usable.
__attribute__((noinline)) uint32_t ctx_prepare_vmid(uint32_t vmid, bool *ok) {
    DeviceContext &dev = *gCtx->dev;
    const CtxRegs r = ctx_regs(vmid);
    uint32_t cntl = RREG32(dev, r.cntl);
    if (vmid != kNativeVmid && !ctx_geometry_ok(cntl)) {
        const uint32_t before = cntl;
        WREG32(dev, r.cntl, cntl_native(before));
        cntl = RREG32(dev, r.cntl);
        N1C_LOG("CONTEXT%u CNTL %#010x -> %#010x (cntl_native: DEPTH 3 / BLOCK 0 / RETRY 0, as S1b set CONTEXT8)", vmid, before, cntl);
    }
    *ok = ctx_geometry_ok(cntl);
    if (!*ok) N1C_LOG("CONTEXT%u CNTL %#010x is not the native geometry (DEPTH 3 / BLOCK 0 / RETRY 0)", vmid, cntl);
    return cntl;
}
// Caller holds s->lock AND gCliLock. Records PT mutation epoch and flushes hardware TLB immediately only if a VMID is currently bound.
uint32_t session_flush_if_bound_locked(Session *s) {
    const uint64_t nextSeq = __atomic_load_n(&s->tlbSeq, __ATOMIC_RELAXED) + 1ull;
    __atomic_store_n(&s->tlbSeq, nextSeq, __ATOMIC_RELEASE);
    if (s->pinnedVmid) return flush_vmid(kNativeVmid);
    const uint32_t vmid = gVmidPool.vmidOf(reinterpret_cast<uintptr_t>(s));
    if (vmid == 0u) return kOk;
    const uint32_t rc = flush_vmid(vmid);
    if (rc == kOk) gVmidPool.noteFlushed(reinterpret_cast<uintptr_t>(s), vmid, nextSeq);
    else {
        (void)gVmidPool.quarantine(reinterpret_cast<uintptr_t>(s), vmid);
        __atomic_store_n(&s->vmid, 0u, __ATOMIC_RELEASE);
    }
    return rc;
}
// Caller holds s->lock only.
uint32_t session_pt_flush(Session *s) {
    IOLockLock(gCliLock);
    const uint32_t rc = session_flush_if_bound_locked(s);
    IOLockUnlock(gCliLock);
    return rc;
}
// Lazy 16-page GART fence window reservation, called under gCliLock on first kBoGtt fence resolve.
bool fence_window_reserve_locked(FenceWindow &win) {
    if (win.reserved) return true;
    GMCContext &gmc = gCtx->gmc;
    DeviceContext &dev = *gCtx->dev;
    const uint64_t bytes = (uint64_t)RDNA4N_FENCE_SLOTS * kPage;
    if (gmc.gart_size == 0 || gmc.gart_pt_bus == 0 || gmc.gart_bump_offset + bytes > gmc.gart_size) {
        N1C_LOG("fence window reserve failed (NoMemory): no GART room (%llu + %llu > %llu)",
                (unsigned long long)gmc.gart_bump_offset, (unsigned long long)bytes, (unsigned long long)gmc.gart_size);
        return false;
    }
    win.gartOff = gmc.gart_bump_offset;
    gmc.gart_bump_offset += bytes;
    win.mc = gmc.gart_start + win.gartOff;
    win.reserved = true;
    const uint64_t zero = 0;
    for (uint32_t i = 0; i < RDNA4N_FENCE_SLOTS; i++)
        bar0_memcpy_to_vram(dev, gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
    amdgpu_hdp_flush(dev);
    N1C_LOG("fence window reserved: %u GART pages at mc %#llx (offset %#llx), every PTE invalid until a GTT fence target binds it",
            RDNA4N_FENCE_SLOTS, (unsigned long long)win.mc, (unsigned long long)win.gartOff);
    return true;
}
// 0.0.629 (G2 review fix R5): READ the GCVM L2 protection-fault registers of the GFXHUB - STATUS_LO32 0x15d0, STATUS_HI32 0x15d1, ADDR_LO32 0x15d2, ADDR_HI32
// 0x15d3, the four native_s1b.cpp's fault leg reads - log them, and return rdna4native::g2::fault_word(STATUS_LO32, ADDR_LO32). NEVER written or cleared here (the
// S1b self-test's VMID-8 fault stays latched: a new fault is told only by a CHANGE of this word). Register reads only.
__attribute__((noinline)) uint64_t fault_snap(const char *when, uint32_t slot) {
    const uint32_t sLo = hub_rd(0x15d0), sHi = hub_rd(0x15d1), aLo = hub_rd(0x15d2), aHi = hub_rd(0x15d3);
    N1C_LOG("gcvm register snapshot (%s, slot %u): STATUS_LO32 %#010x (vmid %u) HI32 %#010x ADDR_LO32 %#010x HI32 %#010x (read only, never cleared)", when, slot, sLo,
            rdna4native::g2::fault_word_vmid(rdna4native::g2::fault_word(sLo, aLo)), sHi, aLo, aHi);
    return rdna4native::g2::fault_word(sLo, aLo);
}
static __attribute__((noinline)) void open_refused_log(bool multi, bool ws, uint32_t openNow, bool apps, bool wsOpen) {
    N1C_LOG("open refused (ExclusiveAccess): %s (%u session(s) open, switch %s)", !multi ? "the session slot is not free" : g1_holds() ? "a G1 test context holds the GPU" :
            apps ? (ws && wsOpen) ? "a WindowServer session is already open" : (!ws && !wsOpen) ? "the last free slot is reserved for WindowServer" : "every session slot is in use or DEAD" :
            (ws && openNow != 0u) ? "a WindowServer client opens only when no session is open" : "every session slot is in use or DEAD", openNow, multi ? "ON" : "OFF");
}
static __attribute__((noinline)) void opened_log(const Session *s, uint32_t cntl, uint64_t winMc) {
    const CPContext &cp = gCtx->cp;
    N1C_LOG("client opened: slot %u VMID %u (session %u%s), fresh root pa %#llx (visible pool), CONTEXT%u CNTL %#010x, ring %u dwords rptr %u wptr %u, fence window mc %#llx",
            s->slot, s->vmid, s->id, s->ws ? ", WindowServer" : "", (unsigned long long)s->rootPa, s->vmid, cntl, cp.ring_size_dwords, *cp.rptr_cpu, cp.wptr, (unsigned long long)winMc);
}
IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {
    using namespace rdna4native::g2;
    DeviceContext *dev = ctx.dev;
    const NativeS1bState &s1b = native_s1b_state();
    // (1) the ladder: stage 17 reached, CP up, the compute test passed.
    if (dev == nullptr || ctx.reached != BringupStage::ComputeDispatch || !ctx.cp.inited || !ctx.computePassed || ctx.cp.wb_cpu == nullptr ||
        ctx.cp.ring_cpu == nullptr || ctx.cp.rptr_cpu == nullptr || dev->bar0 == nullptr || dev->bar2 == nullptr ||
        !ctx.gmc.gfxhub.inited || !ctx.gmc.vram_alloc.is_inited()) {
        N1C_LOG("open refused (NotReady): the ladder did not reach stage 17 with CP, the compute test, BAR0/BAR2 and the GFXHUB up "
                "(reached %u, cp %d, compute %d)", (unsigned)ctx.reached, (int)ctx.cp.inited, (int)ctx.computePassed);
        return kIOReturnNotReady;
    }
    // (1b) 0.0.663 (ReBAR item 5): BAR0 as IOPCIFamily sees it and as config space / the ReBAR capability describe it must agree, or the host-import guard's BAR list cannot be trusted.
    if (__atomic_load_n(&gBar0Conflict, __ATOMIC_ACQUIRE) == 2u) {
        N1C_LOG("open refused (NotReady): IOPCIFamily and config space disagree on BAR0 (see the 'BAR0 latch' and 'pci bars: BAR0 union' lines)");
        return kIOReturnNotReady;
    }
    // (2) the S1b gate: accepted, ran, POSITIVE PASS, no latched stop.
    if (s1b.gate != kGateOn || !s1b.ran || !s1b.positivePass || s1b.stopped) {
        N1C_LOG("open refused (NotReady): native-s1b gate %u ran %d positive %d stopped %d (need gate ON, ran, POSITIVE PASS, not stopped)",
                s1b.gate, (int)s1b.ran, (int)s1b.positivePass, (int)s1b.stopped);
        return kIOReturnNotReady;
    }
    // (3) not HUNG this boot.
    if (gHang.hung) {
        N1C_LOG("open refused (NotReady): the GPU was declared HUNG earlier this boot; reboot");
        return kIOReturnNotReady;
    }
    // (4) exclusivity. Switch OFF: exactly one client, by this compare-and-swap (0.0.626). Switch ON (0.0.627, G2): open_pick under the client lock below.
    const bool multi = multi_on();
    if (!multi && !OSCompareAndSwap(0, 1, &gOpenFlag)) {
        N1C_LOG("open refused (ExclusiveAccess): a native client is already open");
        return kIOReturnExclusiveAccess;
    }
    lazy_lock(&gCliLock); lazy_lock(&gRingLock); lazy_lock(&gKcqRingLock); lazy_lock(&gHangLock); lazy_lock(&gTlbLock); lazy_lock(&gWaitLock);
    if (!gCliLock || !gRingLock || !gKcqRingLock || !gHangLock || !gTlbLock || !gWaitLock) { if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag); return kIOReturnNoMemory; }
    gCtx = &ctx;

    // The whole open runs under the client lock so a racing close cannot see a half-built session.
    IOLockLock(gCliLock);
    ensure_vmid_pool_locked();
    IOReturn rc = kIOReturnSuccess;
    Session *s = nullptr;
    do {
        CPContext &cp = ctx.cp;
        const uint32_t openNow = multi ? __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) : 0u;
        const bool appsNow = apps_on(), wsOpen = __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;   // 0.0.641: apps ON = a WindowServer slot is always available and the last free slot is its
        const bool g1Caller = __atomic_load_n(&gG1OpenThread, __ATOMIC_ACQUIRE) == current_thread();
        const bool pinnedVmid = !multi || ws || g1Caller;
        const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), appsNow, wsOpen);
        if (pk.rc != kOk) { open_refused_log(multi, ws, openNow, appsNow, wsOpen); rc = kIOReturnExclusiveAccess; break; }
        if (pinnedVmid && gVmid8Dead) { open_refused_log(multi, ws, openNow, appsNow, wsOpen); rc = kIOReturnExclusiveAccess; break; }
        const uint32_t slot = pk.slot;
        const uint32_t vmid = pinnedVmid ? kNativeVmid : 0u;
        // The ring must be idle for the FIRST session: S1b's (or the last session's) work has retired and the seqno restarts. 0.0.627: a later session
        // (switch ON) opens next to the running ones.
        if (openNow == 0u) {
            const uint64_t t0 = now_ns();
            for (;;) {
                sysmem_rmb();
                if ((*cp.rptr_cpu & cp.ring_ptr_mask) == cp.wptr) break;
                if (now_ns() - t0 >= kWaitCapNs) { N1C_LOG("open refused (NotReady): the kernel ring is not idle (rptr %u wptr %u)", *cp.rptr_cpu, cp.wptr); rc = kIOReturnNotReady; break; }
                IOSleep(1);
            }
            if (rc == kIOReturnSuccess && ctx.kcq.inited && ctx.kcq.mapped && ctx.kcq.rptr_cpu != nullptr) {
                const uint64_t tKcq = now_ns();
                for (;;) {
                    sysmem_rmb();
                    const uint64_t kcqEm = kcq_emitted_now();
                    const uint64_t kcqRet = kcq_retired_now(kcqEm);
                    if ((*ctx.kcq.rptr_cpu & ctx.kcq.ring_ptr_mask) == ctx.kcq.wptr && kcqRet >= kcqEm) break;
                    if (now_ns() - tKcq >= kWaitCapNs) {
                        N1C_LOG("open refused (NotReady): the compute ring is not idle (rptr %u wptr %u emitted %llu retired %llu)",
                                *ctx.kcq.rptr_cpu, ctx.kcq.wptr, (unsigned long long)kcqEm, (unsigned long long)kcqRet);
                        rc = kIOReturnNotReady;
                        break;
                    }
                    IOSleep(1);
                }
            }
        }
        if (rc != kIOReturnSuccess) break;
        uint32_t cntl = 0u;
        if (pinnedVmid) {
            bool ctxOk = false;
            cntl = ctx_prepare_vmid(vmid, &ctxOk);
            if (!ctxOk) { rc = kIOReturnNotReady; break; }
        }
        // The park root: a kernel-owned zeroed page a context is pointed at while no client owns a tree.
        if (!gPark.have) {
            if (!ctx.gmc.vram_alloc.alloc(kArenaBlockBytes, kArenaBlockBytes, &gPark.a)) { rc = kIOReturnNoMemory; break; }
            const uint64_t off = gPark.a.gpu_va - ctx.gmc.vram_start;
            if (!rdna4rebar::vis_vram_range_ok(gPark.a.gpu_va, gPark.a.size, ctx.gmc.vram_start, dev->vramLimit, dev->bar0Size, dev->vramSizeBytes)) { ctx.gmc.vram_alloc.free(gPark.a); rc = kIOReturnNoMemory; break; }
            bar0_memset_vram(*dev, off, 0, gPark.a.size);
            amdgpu_hdp_flush(*dev);
            gPark.pa = pa_of_mc(gPark.a.gpu_va);
            gPark.have = true;
        }
        if (!gWcInit) { gWc = cp.wptr; gWcInit = true; }   // the ring is idle: the counter starts at the hardware wptr and never restarts
        if (!gKcqWcInit && ctx.kcq.inited && ctx.kcq.mapped) { gKcqWc = ctx.kcq.wptr; gKcqWcInit = true; }
        // The session record. Slot 0 is the static gSess; slots 1..63 are allocated once (switch ON) and never freed. Its lock is kept across opens.
        if (gSlot[slot] == nullptr) {
            Session *ns = static_cast<Session *>(IOMalloc(sizeof(Session)));
            if (ns == nullptr) { rc = kIOReturnNoMemory; break; }
            bzero(ns, sizeof(Session));
            __atomic_store_n(&gSlot[slot], ns, __ATOMIC_RELEASE);
        }
        s = gSlot[slot];
        // 0.0.629 (G2 review fix R1): the record is wiped WITHOUT its lock field. A closing thread may still hold (and is about to release) that lock: it
        // was never NULL once created, and it is created exactly once per slot, here, before the slot's first open publishes the session.
        if (s->lock == nullptr) {
            IOLock *nl = IOLockAlloc();
            if (nl == nullptr) { s = nullptr; rc = kIOReturnNoMemory; break; }
            __atomic_store_n(&s->lock, nl, __ATOMIC_RELEASE);
        }
        rdna4native::g2::sess_wipe_keep_lock(s);
        s->slot = slot; s->vmid = vmid; s->ws = ws; s->pinnedVmid = pinnedVmid; s->tlbSeq = 1ull;
        s->faultAtOpen = fault_snap("open", slot);   // R5: read only
        s->pagesInBlk = 4u;   // forces the first block
        PtMem mem{ s };
        s->rootPa = mem.alloc_page();
        if (s->rootPa == 0ull) { N1C_LOG("open refused (NoMemory): no VRAM for the root table"); rc = kIOReturnNoMemory; break; }
        amdgpu_hdp_flush(*dev);
        if (openNow == 0u) {
            // The first session: zero the native seqno slot while the ring is idle, restart the count, and start the clock.
            *seq_slot() = 0ull;
            if (kcq_seq_slot() != nullptr) *kcq_seq_slot() = 0ull;
            sysmem_wmb();
            __atomic_store_n(&gEmitted, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&gKcqEmitted, 0ull, __ATOMIC_RELEASE);
            gVmidPool.resetFences();
            const uint64_t tNow = now_ns();
            gHang.lastRetired = 0; gHang.tProgress = tNow;
            gKcqHang.lastRetired = 0; gKcqHang.tProgress = tNow;
        }
        if (pinnedVmid && program_root(vmid, s->rootPa) != kOk) { rc = kIOReturnNotReady; break; }
        __atomic_store_n(&s->id, ++gSessSeqCounter, __ATOMIC_SEQ_CST);   // 0.0.609
        __atomic_store_n(&s->open, true, __ATOMIC_RELEASE);
        gSlotState[slot] = kSlotOpen;
        if (multi) __atomic_add_fetch(&gOpenFlag, 1u, __ATOMIC_SEQ_CST);
        if (ws) __atomic_store_n(&gSessWs, 1u, __ATOMIC_SEQ_CST);   // 0.0.623 (G1) note, 0.0.627: set with the open itself
        if (ws) __atomic_store_n(&gWsId, s->id, __ATOMIC_SEQ_CST);   // 0.0.650 (G5): WindowServer is found by its session id, never by its slot
        if (out != nullptr) { out->slot = slot; out->id = s->id; }
        opened_log(s, cntl, gWin[slot].reserved ? gWin[slot].mc : 0ull);
    } while (false);
    if (rc != kIOReturnSuccess) {
        if (s != nullptr) pt_release_blocks(s);
        if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag);
    }
    IOLockUnlock(gCliLock);
    return rc;
}

// ---- the scanout pins (0.0.603) -------------------------------------------------------------------------------------------------
// A BO that hosts scanout slots (ScanoutRegister) carries pinMask / pinGen, written ONLY under gCliLock. The console plane goes back BEFORE
// such a BO is freed or leaked, on every path: BoFree, the HUNG leak, and the close of the whole session (lock order gCliLock -> the DCN lock).
// 0.0.661 (M6 Stage 1b): BOTH instances are restored (instance 0, then instance 2's A) before any BO is touched: rdna4scanx::teardown_both (host-tested with a call recorder) owns the order.
// 0.0.662 (M6 Stage 2): ALL THREE (instance 0, the monitor A's A, the monitor B's A): rdna4scanx::teardown_all.
struct TeardownEnv {
    Session *s;
    void restore0(const char *how, uint64_t r[2]) { (void)rdna4dcn::scanRelease(how, r); }
    void restore1(const char *how, uint64_t r[2]) { (void)rdna4dcn::scanXRelease(1u, how, r); }   // 0.0.662: inert (no register, no lock) with garnet-m6flip1 (or a Stage-1b latch) OFF
    void restore2(const char *how, uint64_t r[2]) { (void)rdna4dcn::scanXRelease(2u, how, r); }   // inert (no register, no lock) with a Stage-1b latch OFF
    uint32_t nbo() { return RDNA4N_MAX_BOS; }
    bool used(uint32_t h) { return s->boUsed[h]; }
    uint32_t pin0(uint32_t h) { return s->bo[h].pinMask; }
    uint32_t pin1(uint32_t h) { return s->bo[h].pinMask1; }
    uint32_t pin2(uint32_t h) { return s->bo[h].pinMask2; }
    void mark_leak(uint32_t h) { s->bo[h].pinLeak = 1u; }
    void clear_pins(uint32_t h) { s->bo[h].pinMask = 0; s->bo[h].pinMask1 = 0; s->bo[h].pinMask2 = 0; }
};
static void scan_teardown(Session *s, const char *how, uint64_t r[2]) {
    r[0] = 1; r[1] = 0;
    uint64_t r1[2] = { 1, 0 }, r2[2] = { 1, 0 };
    TeardownEnv te { s };
    rdna4scanx::teardown_all(te, how, r, r1, r2);
    if (r[0] == 0ull) N1C_LOG("scanout: the console restore at %s was NOT verified (plane %#llx): run `rdna4test accel dcnflip 0`, reboot if it persists", how, (unsigned long long)r[1]);
    if (r1[0] == 0ull) N1C_LOG("scanx: the restore of instance 1's (the monitor A's) A at %s was NOT verified (plane %#llx): its BOs are LEAKED, reboot if it persists", how, (unsigned long long)r1[1]);
    if (r2[0] == 0ull) N1C_LOG("scanx: the restore of instance 2's A at %s was NOT verified (plane %#llx): its BOs are LEAKED, reboot if it persists", how, (unsigned long long)r2[1]);
}
// BoFree / release of a BO with pins. rdna4scan::unpin_plan (via rdna4dcn::scanBoGone) decides: Normal - a slot that is neither shown nor pending is
// simply dropped, one the hardware is fetching (or has pending) needs the full restore first. Leak (HUNG) and Closing: ALWAYS the full restore
// first, then the caller leaks or frees. Either way the console is back before the BO's memory is freed or leaked.
bool scan_unpin(uint32_t h, Bo &b, RelMode mode) {
    uint64_t r[2] = { 1, 0 };
    const uint32_t res = rdna4dcn::scanBoGone(b.pinMask, b.pinGen, mode != kRelNormal, r);
    if (res != 0u) {
        N1C_LOG("BO %u hosted scanout slot(s) %#x and is going away (%s): the console restore %s", h, (unsigned)b.pinMask,
                mode == kRelNormal ? "BoFree while shown or pending" : "leak / close", res == 1u ? "ran FIRST and VERIFIED" : "did NOT verify: the BO is LEAKED, not freed");
    }
    b.pinMask = 0;
    return res == 2u;   // true = the plane may still scan this BO: leak it
}
// 0.0.661 / 0.0.662: the same for an HDMI instance's slots (pinMask1 / pinMask2): rdna4scanx::bo_gone, judged with THAT instance's EARLIEST and generation. true = A is not verified back: LEAK the BO.
static bool scan_unpinx(uint32_t inst, uint32_t h, Bo &b, RelMode mode) {
    uint8_t &mask = inst == 1u ? b.pinMask1 : b.pinMask2;
    const uint32_t pgen = inst == 1u ? b.pinGen1 : b.pinGen2;
    uint64_t r[2] = { 1, 0 };
    const uint32_t res = rdna4dcn::scanXBoGone(inst, mask, pgen, mode != kRelNormal, r);
    if (res != 0u) N1C_LOG("BO %u hosted instance-%u slot(s) %#x and is going away (%s): the restore of A %s", h, (unsigned)inst, (unsigned)mask,
                           mode == kRelNormal ? "BoFree while shown or pending" : "leak / close", res == 1u ? "ran FIRST and VERIFIED" : "did NOT verify: the BO is LEAKED, not freed");
    mask = 0;
    return res == 2u;
}
bool scan_unpin1(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(1u, h, b, mode); }
bool scan_unpin2(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(2u, h, b, mode); }

void n1c_close(const N1cRef &ref, const char *how) {
    if (gCliLock == nullptr) return;
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return;
    IOLock *const sl = s->lock;   // 0.0.629 (G2 review fix R1): the lock this thread holds, kept LOCAL: after the last gCliLock below the record may be re-opened
    using namespace rdna4native::g2;
    const bool multi = multi_on();
    DeviceContext &dev = *gCtx->dev;
    IOLockLock(gCliLock);
    // 0.0.603: the console plane FIRST - before the own wait, before the HUNG decision, before anything is freed or leaked. 0.0.627 (G2): with the switch ON only the
    // session that holds the plane restores it (another client's close never takes WindowServer's plane away).
    if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr); if (multi) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE); }
    rdna4dcn::modeHoldSessionClosed(s->id);   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
    IOLockUnlock(gCliLock);
    bool leak = hung_now();
    if (!leak && !own_wait(s)) leak = true;            // a timeout here latched the hang (switch ON: or left this slot's work unproven)
    const bool ownIdle = !leak;
    uint32_t nBos = 0, nHost = 0; uint64_t vramB = 0, gttB = 0, hostB = 0;
    bool hadVisDcc = false;
    for (uint32_t h = 1; h < RDNA4N_MAX_BOS; h++) {
        if (!s->boUsed[h]) continue;
        nBos++;
        if (s->bo[h].kind == kBoGtt) gttB += s->bo[h].size;
        else if (s->bo[h].kind == kBoHost) { hostB += s->bo[h].size; nHost++; }   // 0.0.612
        else {
            vramB += s->bo[h].size;
            if (rdna4native::s1c::bo_needs_free_gl2_inv(s->bo[h].kind == kBoVis, s->bo[h].dcc)) hadVisDcc = true;
        }
    }
    if (!leak && hadVisDcc && bo_gpu_gl2_inv(s) != kOk) leak = true;
    const uint64_t seqs = s->emitted;
    const uint32_t ptPages = s->ptPages;
    IOLockLock(gCliLock);
    bool parked = false;
    const uint32_t closeVmid = s->pinnedVmid ? kNativeVmid : gVmidPool.vmidOf(reinterpret_cast<uintptr_t>(s));
    if (s->pinnedVmid) __atomic_store_n(&s->vmid, closeVmid, __ATOMIC_RELAXED);
    if (!leak) {
        // No client owns this tree from here: park the context on the kernel's zeroed page, then free everything.
        if (s->vmid == 0u) parked = true;
        else { if (program_root(s->vmid, gPark.pa) != kOk) leak = true; else parked = true; }
    }
    if (!leak) {
        for (uint32_t h = 1; h < RDNA4N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(s, h, kRelClosing);
        (void)va_remove_prt(s->maps, kMaxMaps);
        session_sync_prt(s);
        bool slots = false;
        const FenceWindow &win = gWin[s->slot];
        if (win.reserved) {
            for (uint32_t i = 0; i < RDNA4N_FENCE_SLOTS; i++) {
                if (!s->fslot[i].used) continue;
                const uint64_t zero = 0;
                bar0_memcpy_to_vram(dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
                s->fslot[i].used = 0; slots = true;
            }
        }
        if (slots) { amdgpu_hdp_flush(dev); (void)flush_vmid(0); }
        pt_release_blocks(s);   // into the reserve, not vram_alloc
        N1C_LOG("client closed (%s): slot %u VMID %u: %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, freed", how, s->slot, closeVmid, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) unmapped, then completed and released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    } else {
        // HUNG (or this slot's own work not proven retired): nothing the GPU might still read is freed; the context stays as it is, and the slot is DEAD.
        (void)va_remove_prt(s->maps, kMaxMaps);
        session_sync_prt(s);
        N1C_LOG("client closed (%s): slot %u VMID %u: %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, LEAKED (HUNG)", how, s->slot, closeVmid, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) LEAKED (HUNG): pages stay wired, never completed or released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    }
    if (s->pinnedVmid) {
        if (!(parked && ownIdle)) gVmid8Dead = true;
    } else if (closeVmid != 0u) {
        if (!leak) { uint32_t rv = 0u; (void)gVmidPool.forget(reinterpret_cast<uintptr_t>(s), true, rv); }
        else (void)gVmidPool.quarantine(reinterpret_cast<uintptr_t>(s), closeVmid);
    }
    __atomic_store_n(&s->vmid, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s->hello, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s->open, false, __ATOMIC_RELEASE);          // the Session itself is static (slot 0) or never freed (slots 1..63)
    gSlotState[s->slot] = slot_after_close(parked, ownIdle);       // 0.0.627 (G2): a DEAD slot stays retired; pooled VMIDs are quarantined individually
    if (s->ws) __atomic_store_n(&gSessWs, 0u, __ATOMIC_SEQ_CST);   // 0.0.623 (G1): no WindowServer session any more
    if (s->ws) __atomic_store_n(&gWsId, 0u, __ATOMIC_SEQ_CST);     // 0.0.650 (G5)
    if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag);
    else __atomic_sub_fetch(&gOpenFlag, 1u, __ATOMIC_SEQ_CST);
    IOLockUnlock(gCliLock);
    IOLockUnlock(sl);   // R1: through the local, never the record: a racing n1c_open may already be re-initialising it (it never writes the lock field)
}

void n1c_latch_bar0_conflict(bool conflict) {
    if (OSCompareAndSwap(0, conflict ? 2u : 1u, &gBar0Conflict)) N1C_LOG("BAR0 latch: IOPCIFamily and config space %s on BAR0", conflict ? "DISAGREE (native client opens are refused)" : "agree");
}

// 0.0.641 (G4 review HIGH 1b): called by the client class BEFORE n1c_close, with apps ON only. For each visible-VRAM BO of the session that still has CPU mappings (descriptor references beyond the
// record's own) the mappings that IOUserClient keeps are removed (removeMappingForDescriptor returns each map retained; releasing it drops the map's reference on the descriptor and unmaps it from
// the task if that is still alive), so that n1c_close sees the record's reference alone and frees the range. No lock is held across the removal. A mapping that cannot be removed (it was handed to
// another task as a send right) keeps the count above 1 and n1c_close leaks the range, as before.
void n1c_unmap_for_close(const N1cRef &ref, IOUserClient *uc) {
    if (uc == nullptr || !apps_on()) return;
    uint32_t removed = 0;
    for (uint32_t h = 1; h < RDNA4N_MAX_BOS; h++) {
        Session *s = sess_lock(ref, false);
        if (s == nullptr) break;
        IODeviceMemory *vmd = s->boUsed[h] ? s->bo[h].vmd : nullptr;
        const uint32_t n = rdna4native::g4::unmap_attempts(true, vmd != nullptr && s->bo[h].kind == kBoVis, vmd != nullptr, vmd != nullptr ? (uint32_t)vmd->getRetainCount() : 0u);
        if (n != 0u) vmd->retain();   // our own reference while the lock is dropped (a racing BoFree may release the record's)
        sess_unlock(s);
        if (n == 0u) continue;
        for (uint32_t i = 0; i < n; i++) {
            IOMemoryMap *m = uc->removeMappingForDescriptor(vmd);
            if (m == nullptr) break;
            m->release(); removed++;
        }
        vmd->release();
    }
    if (removed != 0u) N1C_LOG("close: slot %u: %u CPU mapping(s) of visible-VRAM BOs removed before the session's BOs are released", ref.slot, removed);
}

// 0.0.610 (ABI 1.8): read-only views for the Metal nub gate (that source file is the only user). 0.0.627: Hello done on ANY open session (one with the switch OFF).
bool n1c_hello_done() {
    for (uint32_t i = 0; i < kMaxSessions; i++) { Session *g = slot_ptr(i); if (g != nullptr && __atomic_load_n(&g->open, __ATOMIC_ACQUIRE) && __atomic_load_n(&g->hello, __ATOMIC_ACQUIRE)) return true; }
    return false;
}
bool n1c_hung() { return hung_now(); }

IOReturn n1c_hello(const N1cRef &ref, uint64_t clientAbi, uint64_t flags, uint64_t out[4]) {
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return kIOReturnNotReady;
    IOReturn rc = kIOReturnSuccess;
    if ((flags & ~(uint64_t)RDNA4N_HELLO_F_MINOR) != 0ull) rc = kIOReturnBadArgument;
    else if (clientAbi != RDNA4N_ABI_VERSION) { N1C_LOG("hello: client ABI %llu, kernel ABI %u: refused", (unsigned long long)clientAbi, RDNA4N_ABI_VERSION); rc = kIOReturnUnsupported; }
    else {
        __atomic_store_n(&s->hello, true, __ATOMIC_RELEASE);
        // ABI 1.1: the minor is reported only when the client asks for it; without the flag out[0] is exactly the major, as in 1.0.
        out[0] = ((flags & RDNA4N_HELLO_F_MINOR) != 0ull) ? (uint64_t)RDNA4N_ABI_VERSION | ((uint64_t)RDNA4N_ABI_MINOR << 16) : (uint64_t)RDNA4N_ABI_VERSION;
        out[1] = kN1cKextBuild; out[2] = RDNA4N_MAX_IBS; out[3] = __atomic_load_n(&s->vmid, __ATOMIC_ACQUIRE);   // 0.0.627: pinned (8) or dynamic (9..15, 0 = unbound)
    }
    sess_unlock(s);
    return rc;
}

IOReturn n1c_query_info(const N1cRef &ref, rdna4n_info *o) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    BringupContext &c = *gCtx;
    memset(o, 0, sizeof(*o));
    const IPVersion v = c.dev->ip.getVersion(IPBlock::GC);
    const uint64_t em = emitted_now();
    o->abi_version = RDNA4N_ABI_VERSION; o->kext_build = kN1cKextBuild;
    o->flags = RDNA4N_INFO_BUFFER_OPS | RDNA4N_CAP_VM_PRT | (dcc_on() ? RDNA4N_CAP_GFX12_DCC : 0u) | (async_compute_on() ? RDNA4N_CAP_ASYNC_COMPUTE : 0u) |
               (hung_now() ? RDNA4N_INFO_HUNG : 0u) | (native_s1b_state().positivePass ? RDNA4N_INFO_S1B_POSITIVE : 0u) |
               (native_s1b_state().gate == kGateOn ? RDNA4N_INFO_NATIVE_BOOT : 0u);
    o->vmid = __atomic_load_n(&s->vmid, __ATOMIC_ACQUIRE);
    o->gb_addr_config = RREG32(*c.dev, SOC15_REG_OFFSET_BIDX(*c.dev, IPBlock::GC, GFXRegBaseIdx::GB_ADDR_CONFIG, GFXRegs::GB_ADDR_CONFIG));
    o->gc_version = ((uint32_t)v.major << 16) | ((uint32_t)v.minor << 8) | v.rev;
    o->ring_size_dw = c.cp.ring_size_dwords; o->max_ibs = RDNA4N_MAX_IBS;
    uint64_t visTotal, visFree, hiTotal, hiFree, othersHeld, ptHeld;
    IOLockLock(gCliLock);   // one snapshot for the pool numbers and the budget below (no session lock is held here, never across a GPU wait)
    visTotal = c.gmc.vram_alloc.size(); visFree = c.gmc.vram_alloc.bytes_free();
    hiTotal = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.size() : 0;
    hiFree = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.bytes_free() : 0;
    othersHeld = gVramOthers; ptHeld = rdna4native::g2::pt_held_bytes(gPtN, kArenaBlockBytes);
    IOLockUnlock(gCliLock);
    o->vram_vis_total = visTotal; o->vram_vis_free = visFree;
    o->vram_hi_total = hiTotal; o->vram_hi_free = hiFree;
    o->gtt_cap = kGttCap; o->gtt_used = s->gttUsed; o->gtt_max_bo = kGttMaxBo;
    o->pt_cap = kPtCap; o->pt_used = (uint64_t)s->nblk * kArenaBlockBytes;
    o->reserved[0] = (uint32_t)(gWc & 0xFFFFFFFFu); o->reserved[1] = (uint32_t)(gWc >> 32);   // the native write counter (a diagnostic; the contract leaves reserved[] unused)
    o->reserved[2] = RDNA4N_ABI_MINOR;                                                          // ABI 1.1 (0.0.603): the minor
    o->seq_emitted = em; o->seq_retired = retired_now(em);
    {   // 0.0.640 (G4), ABI 1.10: the session's VRAM budget (reserved[3..7]); all zero for a WindowServer session and with the switch OFF.
        const uint64_t poolTotal = rdna4native::g2::sat_add(visTotal, hiTotal);
        const uint64_t poolFree = rdna4native::g2::sat_add(visFree, hiFree);
        rdna4native::g4::budget_pack(rdna4native::g4::budget_report(multi_on(), s->ws, __atomic_load_n(&s->app, __ATOMIC_ACQUIRE), othersHeld, poolTotal, poolFree, ptHeld), o->reserved);
    }
    o->va_low_first = kVaFirst; o->va_low_last = 0x00007FFFFFFFFFFFull;
    o->va_high_first = 0xFFFF800000000000ull; o->va_high_last = 0xFFFFFFFFFFFFFFFFull;
    o->max_bos = RDNA4N_MAX_BOS; o->fence_slots = RDNA4N_FENCE_SLOTS;
    return kIOReturnSuccess;
}

IOReturn n1c_read_regs(const N1cRef &ref, uint64_t off, uint64_t count, uint64_t instance, uint32_t *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallReadRegs)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): an APP session never reads registers
    if (count < 1 || count > 16 || instance != 0xFFFFFFFFull) return kIOReturnBadArgument;
    if (!regs_allowed(off, count) || (off + count) * 4ull > gCtx->dev->rmmioSize) return kIOReturnBadArgument;   // allowlist 0x263e..0x2641 only
    for (uint64_t i = 0; i < count; i++) out[i] = RREG32(*gCtx->dev, (uint32_t)(off + i));
    return kIOReturnSuccess;
}

// ---- ABI 1.1: native scanout (0.0.603) --------------------------------------------------------------------------------------------
// Lock order: the session lock, gCliLock, then the scanout lock inside rdna4dcn::scan*. Acquire / Register / Release take gCliLock (they touch the session's
// pins, and a racing close must not leave the plane taken); Query / Present / Status take only the scanout lock (a Present in flight either
// happened before the close's restore, which covers it, or is refused after it). 0.0.627 (G2), switch ON: one session holds the plane (gScanOwner);
// another session's Acquire is ExclusiveAccess and its Register / Present / Release are NotPermitted. Query and Status stay readable by all.
static inline bool scan_foreign(const Session *s) {
    if (!multi_on()) return false;
    const uint32_t o = __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE);
    return o != 0u && o != s->id;
}
IOReturn n1c_scan_query(const N1cRef &ref, rdna4n_scan_query *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    return (IOReturn)rdna4dcn::scanQuery(out);
}
IOReturn n1c_scan_acquire(const N1cRef &ref, uint64_t flags, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnExclusiveAccess;
    if (!scan_foreign(s)) {
        rc = (IOReturn)rdna4dcn::scanAcquire(out, s->id);   // 0.0.609: the session id lets a row-120 hold be Acquired
        if (rc == kIOReturnSuccess && multi_on()) __atomic_store_n(&gScanOwner, s->id, __ATOMIC_RELEASE);
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scan_register(const N1cRef &ref, const rdna4n_scan_reg *in, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (in->reserved0 != 0u) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    do {
        if (scan_foreign(s)) { rc = kIOReturnNotPermitted; break; }
        if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }   // 0.0.612: system pages of the caller are never a scanout slot (HUBP flips VRAM only)
        const uint32_t gen = rdna4dcn::scanGeneration();
        if (b.pinMask != 0u && b.pinGen != gen) b.pinMask = 0u;                // a pin of an acquisition that has ended is stale
        uint64_t o2[2] = { 0, 0 };
        const uint32_t r = rdna4dcn::scanRegister(b.kind == kBoVis, b.mc, b.size, in->offset, in->pitch_bytes, in->width, in->height, in->format, o2);
        if (r != kOk) { rc = (IOReturn)r; break; }
        b.pinMask = (uint8_t)(b.pinMask | (1u << (o2[0] & 7u))); b.pinGen = gen;   // recorded under gCliLock, released in bo_release / close / Release
        out[0] = o2[0]; out[1] = o2[1];
    } while (false);
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scan_present(const N1cRef &ref, uint64_t slot, uint64_t flags, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s)) return kIOReturnNotPermitted;
    return (IOReturn)rdna4dcn::scanPresent(slot, flags, out);
}
IOReturn n1c_scan_status(const N1cRef &ref, rdna4n_scan_status *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    rdna4disp_m6_flush();   // 0.0.661 (review M1): the DP's 1 s keep-alive also flushes a deferred table publish (instance 2 may never be acquired); inert unless both latches are ON
    return (IOReturn)rdna4dcn::scanStatus(out);
}
IOReturn n1c_scan_release(const N1cRef &ref, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    if (scan_foreign(s)) rc = kIOReturnNotPermitted;
    else {
        uint64_t r[2] = { 1, 0 };
        scan_teardown(s, "ScanoutRelease", r);                                     // idempotent; the verdict is the restore's own
        if (multi_on()) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE);
        out[0] = r[0]; out[1] = r[1];
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}

// ---- ABI 1.11 (0.0.661, M6 Stage 1b) + 1.12 (0.0.662, M6 Stage 2): the HDMI instances (2 = the monitor B; 1 = the monitor A, additionally behind garnet-m6flip1). Every call: Hello, not an APP, the latches (Unsupported with garnet-m6 / garnet-m6flip OFF,
// before anything else), the instance (2, or 1 with garnet-m6flip1; anything else - and 1 with that latch OFF - is BadArgument, exactly 0.0.661's answer) and the flag words, the session must hold instance 0 (the plane Acquire of selector 10), and a foreign
// session is refused - ALL before any register read. Lock order as selectors 9..14: the session lock, gCliLock, then the instance's lock inside rdna4dcn::scanX*.
static inline bool scanx_on() { return rdna4m6_latched_on() && rdna4m6flip_latched_on(); }
static inline bool scanx_inst_ok(uint64_t inst) { return rdna4scanx::inst_admitted(inst, rdna4m6flip1_latched_on()); }
static inline bool scanx_owner(const Session *s) { return multi_on() ? __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id : rdna4dcn::scanActive(); }
IOReturn n1c_scanx_acquire(const N1cRef &ref, uint64_t inst, uint64_t flags, uint64_t out[5]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst) || flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnNotPermitted;
    if (!scan_foreign(s) && scanx_owner(s)) { rc = (IOReturn)rdna4dcn::scanXAcquire((uint32_t)inst, out); if (rc == kIOReturnSuccess) out[3] = gCtx->gmc.vram_alloc.bytes_free(); }   // review S4: the free visible-VRAM figure (under gCliLock, the lock every user of the pool holds)
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scanx_register(const N1cRef &ref, uint64_t inst, const rdna4n_scan_reg *in, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst) || in->reserved0 != 0u) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    do {
        if (scan_foreign(s) || !scanx_owner(s)) { rc = kIOReturnNotPermitted; break; }
        if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }
        const uint32_t gen = rdna4dcn::scanXGeneration((uint32_t)inst);       // THAT instance's generation: pinGen1 is judged against instance 1's, pinGen2 against instance 2's
        uint8_t &mask = inst == 1ull ? b.pinMask1 : b.pinMask2;
        uint32_t &pgen = inst == 1ull ? b.pinGen1 : b.pinGen2;
        mask = rdna4scanx::pins_live(mask, pgen, gen);                        // a pin of an acquisition that has ended is stale
        uint64_t o2[2] = { 0, 0 };
        const uint32_t r = rdna4dcn::scanXRegister((uint32_t)inst, b.kind == kBoVis, b.mc, b.size, in->offset, in->pitch_bytes, in->width, in->height, in->format, o2);
        if (r != kOk) { rc = (IOReturn)r; break; }
        const rdna4scanx::XDesc *xd = rdna4scanx::desc_of((uint32_t)inst);
        if (xd == nullptr || !rdna4scanx::id_ok(*xd, o2[0])) {               // the kernel returned an id that is not this instance's tag (cannot happen unless a descriptor table is wrong): the slot would be unpinned, so the instance is RELEASED (A back) and, if that is not verified, the BO leaked
            uint64_t rr[2] = { 1, 0 };
            (void)rdna4dcn::scanXRelease((uint32_t)inst, "a registered slot id of another instance", rr);
            if (rr[0] == 0ull) b.pinLeak = 1u;
            rc = kIOReturnInternalError; break;
        }
        mask = (uint8_t)(mask | (1u << (rdna4scanx::slot_of(*xd, o2[0]) & 7u))); pgen = gen;
        out[0] = o2[0]; out[1] = o2[1];
    } while (false);
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scanx_present(const N1cRef &ref, uint64_t inst, uint64_t slot, uint64_t flags, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s) || !scanx_owner(s)) return kIOReturnNotPermitted;
    return (IOReturn)rdna4dcn::scanXPresent((uint32_t)inst, slot, flags, out);
}
IOReturn n1c_scanx_status(const N1cRef &ref, uint64_t inst, rdna4n_scan_status *out, uint64_t sout[4]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s)) return kIOReturnNotPermitted;
    return (IOReturn)rdna4dcn::scanXStatus((uint32_t)inst, out, sout);
}
IOReturn n1c_scanx_release(const N1cRef &ref, uint64_t inst, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    if (scan_foreign(s)) rc = kIOReturnNotPermitted;
    else {
        uint64_t r[2] = { 1, 0 };
        (void)rdna4dcn::scanXRelease((uint32_t)inst, "SCANX_RELEASE", r);
        for (uint32_t h = 1; h < RDNA4N_MAX_BOS; h++) if (s->boUsed[h]) {
            uint8_t &mask = inst == 1ull ? s->bo[h].pinMask1 : s->bo[h].pinMask2;      // only THIS instance's pins are judged and cleared
            if (r[0] == 0ull && mask != 0u) s->bo[h].pinLeak = 1u;
            mask = 0;
        }
        out[0] = r[0]; out[1] = r[1];
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}

// ---- ABI 1.2: the DAL experiment (0.0.604). One named DAL experiment step per call. The client lock is NOT held: a step
// sleeps for up to ~65 s (E4's dwell) and a racing close must not wait for it; smu_dal.cpp's own busy flag refuses a second concurrent step, and
// its latch, gate and allowlist do all the refusing. Nothing here touches a session field.
IOReturn n1c_dal_step(const N1cRef &ref, uint64_t step, uint64_t flags, rdna4n_dal_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull || step < RDNA4N_DAL_STEP_E1B || step > RDNA4N_DAL_STEP_E4 || out == nullptr) return kIOReturnBadArgument;
    return dal_run_step(*gCtx->dev, (uint32_t)step, out);
}

// ---- ABI 1.3: the timed mode trial (0.0.605). The client lock is NOT held (the trial sleeps for up to dwell + ~12 s and a racing close must not wait for it); dcn/dcn.cpp's
// busy flag, latch, gate and deny checks do all the refusing. Nothing here touches a session field.
IOReturn n1c_mode_trial(const N1cRef &ref, uint64_t row, uint64_t dwellMs, uint64_t flags, rdna4n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)RDNA4N_MODE_TF_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;   // 0.0.606: the flags word carries RDNA4N_MODE_TF_* (0.0.605 required 0)
    return (IOReturn)rdna4dcn::modeTrial(row, dwellMs, flags, out);
}

// ---- ABI 1.7: the HELD mode of row 120 (0.0.609). Like the trial: no client lock is held (the launch waits up to ~20 s, the release up to ~30 s); the session id names the owner.
IOReturn n1c_mode_hold(const N1cRef &ref, uint64_t maxMs, uint64_t flags, rdna4n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)RDNA4N_HOLD_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)rdna4dcn::modeHold(ref.id, maxMs, flags, out);   // 0.0.627: the caller's own session id
}
IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, rdna4n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, rdna4native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)RDNA4N_REL_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)rdna4dcn::modeRelease(flags, out);
}

} // namespace amdgpu
