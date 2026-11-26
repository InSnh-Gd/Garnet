// garnet_vram_alloc.cpp — Native BO lifecycle, VRAM/GTT/Host-import allocators,
// monitor A/B exclusive VRAM buffers, GemVa, and CPU memory mapping (split from native_s1c.cpp).
#include "garnet_submit_internal.hpp"

namespace amdgpu {

static PciBar gBars[kMaxPciBars];
static uint32_t gNBars;
static volatile UInt32 gBarsState;
static uint64_t gSysHeld;
static uint64_t gSysPending;
static uint64_t gHostPhysMem;
static volatile UInt32 gHostPhysLatch;

// ---- 3.5: dynamic host-system cap --------------------------------------------------------------------------------------------------------------------------
// Latch boot_args.PhysicalMemorySize once via PE_state; 0 falls back to kSysGlobalCap. The cap is never VRAM-derived.
static uint64_t host_physmem_bytes() {
    if (__atomic_load_n(&gHostPhysLatch, __ATOMIC_ACQUIRE) != 0u) return __atomic_load_n(&gHostPhysMem, __ATOMIC_ACQUIRE);
    uint64_t phys = 0ull;
    const boot_args *ba = reinterpret_cast<const boot_args *>(PE_state.bootArgs);
    if (ba != nullptr) phys = ba->PhysicalMemorySize;
    __atomic_store_n(&gHostPhysMem, phys, __ATOMIC_RELEASE);
    __atomic_store_n(&gHostPhysLatch, 1u, __ATOMIC_RELEASE);
    if (phys != 0ull) N1C_LOG("host budget: physical memory %llu MiB, reserve %llu MiB, effective host cap %llu MiB (dynamic, never VRAM-derived)",
                               (unsigned long long)(phys >> 20), (unsigned long long)(rdna4native::g2::kHostSysReserve >> 20),
                               (unsigned long long)(rdna4native::g2::host_effective_cap(phys) >> 20));
    else N1C_LOG("host budget: physical memory unknown (PE_state.bootArgs null/0): falling back to legacy static cap %llu MiB, never unlimited",
                 (unsigned long long)(rdna4native::g2::kSysGlobalCap >> 20));
    return phys;
}
static uint64_t host_cap_now() { return rdna4native::g2::host_effective_cap(host_physmem_bytes()); }

// ---- BOs ------------------------------------------------------------------------------------------------------------------------
// 0.0.612: the memory of a host-import BO. Called ONLY where the PTEs are already gone and the TLB flushed (kRelNormal: after pt_unmap + flush_vmid; kRelClosing: after program_root(park)),
// and NEVER in kRelLeak (bo_release's leak branch does not reach it: host_may_release). complete() before release(); the page list goes last.
static void host_release(Bo &b) {
    if (b.hmd != nullptr) { b.hmd->complete(); b.hmd->release(); b.hmd = nullptr; }
    if (b.hpages != nullptr) { IOFree(b.hpages, (vm_size_t)b.hpageCount * sizeof(uint64_t)); b.hpages = nullptr; }
}
// Fix A: release a non-contiguous kBoGtt buffer (same complete/release/IOFree lifecycle as host_release).
static void gtt_release(Bo &b) {
    if (b.gmd != nullptr) { b.gmd->complete(); b.gmd->release(); b.gmd = nullptr; }
    if (b.hpages != nullptr) { IOFree(b.hpages, (vm_size_t)b.hpageCount * sizeof(uint64_t)); b.hpages = nullptr; b.hpageCount = 0; }
}
// Fix A: allocate non-contiguous 4 KiB system pages OUTSIDE gCliLock and Session::lock (avoids vm_page_find_contiguous stalls).
static IOReturn gtt_alloc_pages(uint64_t size, IOBufferMemoryDescriptor **mdOut, uint64_t **pagesOut, uint32_t *countOut) {
    *mdOut = nullptr; *pagesOut = nullptr; *countOut = 0;
    const uint64_t nPages = size / kPage;
    if (nPages == 0 || nPages > (kGttMaxBo / kPage)) return kIOReturnNoMemory;
    IOBufferMemoryDescriptor *gmd = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryMapperNone, (vm_size_t)size, (vm_offset_t)kPage);
    if (gmd == nullptr) return kIOReturnNoMemory;
    if (gmd->prepare() != kIOReturnSuccess) { gmd->release(); return kIOReturnNoMemory; }
    void *cpu = gmd->getBytesNoCopy();
    if (cpu == nullptr || gmd->getLength() < size) { gmd->complete(); gmd->release(); return kIOReturnNoMemory; }
    bzero(cpu, (size_t)size);
    uint64_t *pages = static_cast<uint64_t *>(IOMalloc((vm_size_t)(nPages * sizeof(uint64_t))));
    if (pages == nullptr) { gmd->complete(); gmd->release(); return kIOReturnNoMemory; }
    struct GttSeg {
        IOMemoryDescriptor *md;
        uint64_t phys(uint64_t off, uint64_t *len) {
            IOByteCount seg = 0;
            const addr64_t pa = md->getPhysicalSegment((IOByteCount)off, &seg, kIOMemoryMapperNone);
            *len = pa != 0 ? (uint64_t)seg : 0ull;
            return (uint64_t)pa;
        }
    } seg { gmd };
    const uint32_t crc = collect_pages(seg, size, pages, kGttMaxBo / kPage);
    if (crc != kOk) {
        IOFree(pages, (vm_size_t)(nPages * sizeof(uint64_t)));
        gmd->complete(); gmd->release();
        return (IOReturn)crc;
    }
    *mdOut = gmd; *pagesOut = pages; *countOut = (uint32_t)nPages;
    return kIOReturnSuccess;
}
// The VRAM a BO held goes back to its pool: take it off the budget too (0.0.627). Caller holds gCliLock.
static inline void budget_release(Bo &b) {
    if (b.budgeted != 0u) { gVramOthers = gVramOthers >= b.size ? gVramOthers - b.size : 0ull; b.budgeted = 0u; }
    if (b.visCounted != 0u) { gVisOthers = gVisOthers >= b.size ? gVisOthers - b.size : 0ull; b.visCounted = 0u; }   // 0.0.641
}
// 0.0.629 (R4): the GTT pages / host import of a BO were really freed (released, never leaked): off the global total. Caller holds gCliLock.
static inline void sys_release(Bo &b) { if (b.sysCounted != 0u) { gSysHeld = gSysHeld >= b.size ? gSysHeld - b.size : 0ull; b.sysCounted = 0u; } }
// 3.5: return one armed pre-pinning reservation exactly once; held is untouched.
static inline void sys_pending_rollback(bool &reserved, uint64_t size) {
    if (!reserved) return;
    IOLockLock(gCliLock);
    gSysPending = rdna4native::g2::host_pending_release(gSysPending, size);
    reserved = false;
    IOLockUnlock(gCliLock);
}
// Drop every mapping, fence slot and the memory of handle h of session s. leak = HUNG: forget the handle and the software state, touch nothing else.
// Caller holds the session's lock AND gCliLock (the pools, the budget and the fence window are shared).
void bo_release(Session *s, uint32_t h, RelMode mode) {
    Bo &b = s->bo[h];
    if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;   // 0.0.603: the console before the memory, in every mode
    if (b.pinMask1 != 0u && scan_unpin1(h, b, mode)) b.pinLeak = 1u; // 0.0.662: and the monitor A's (instance 1) A before ITS slots' memory
    if (b.pinMask2 != 0u && scan_unpin2(h, b, mode)) b.pinLeak = 1u; // 0.0.661: and instance 2's A before ITS slots' memory
    if (b.pinLeak != 0u) mode = kRelLeak;             // a restore that did not verify: keep the memory and the PTEs, like HUNG
    const FenceWindow &win = gWin[s->slot];
    if (mode == kRelClosing) {
        // The whole tree is being discarded (the context is parked, the arena is freed by the caller) and the fence-window PTEs are cleared
        // by the caller in one pass: only the memory goes.
        // 0.0.640 (G4), R6 on the CLOSE path: read the descriptor's references BEFORE this record drops its own. A visible-VRAM BO that a process still has CPU-mapped (a reference
        // beyond the record's) must not go back to the shared pool: the mapping could read and write whoever is handed that range next. 0.0.641: keyed on apps_on() (BOTH latches); without
        // garnet-apps: as 0.0.632 (always freed). By now the client's close has already removed the mappings it could (n1c_unmap_for_close): what is still counted here is the fallback.
        const bool vramReturnOk = rdna4native::g4::close_vram_return_ok(apps_on(), b.kind == kBoVis, b.vmd != nullptr, b.vmd != nullptr ? (uint32_t)b.vmd->getRetainCount() : 0u);
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) { gtt_release(b); sys_release(b); }
        else if (b.kind == kBoHi) { gCtx->gmc.vram_alloc_hi.free(b.a); budget_release(b); }
        else if (b.kind == kBoVis && !vramReturnOk) {
            // LEAKED until reboot, and still COUNTED: gVramOthers keeps these bytes (they are never taken off the budget), so the shared allowance reflects what the pool really lost.
            gMappedLeakBytes += b.size; gMappedLeakBos++;
            N1C_LOG("close: slot %u BO %u (visible VRAM, %llu KiB at mc %#llx) is still CPU-mapped: its range is LEAKED until reboot (%u BO(s), %llu KiB leaked so far), not returned to the pool", s->slot, h,
                    (unsigned long long)(b.size >> 10), (unsigned long long)b.mc, gMappedLeakBos, (unsigned long long)(gMappedLeakBytes >> 10));
        }
        else if (b.kind == kBoVis) { gCtx->gmc.vram_alloc.free(b.a); budget_release(b); }
        else if (b.kind == kBoHost && host_may_release(kHostRelClosing)) { host_release(b); sys_release(b); }   // 0.0.612: the context is parked and the TLB flushed (the caller's program_root)
    } else if (mode == kRelNormal) {
        bool touched = false;
        PtMem mem{ s };
        for (uint32_t i = 0; i < kMaxMaps; i++) {
            const VaEnt &e = s->maps[i];
            if (va_ent_used(e) && e.handle == h) { pt_unmap(s->rootPa, e.start, e.size / kPage, mem); touched = true; }
        }
        bool memOk = true;   // review item D: false once a TLB flush did not acknowledge (HUNG): the memory of this BO is then leaked, like the HUNG leak
        if (touched) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(session_flush_if_bound_locked(s)) && memOk; }
        bool slotCleared = false;
        if (win.reserved) {
            for (uint32_t i = 0; i < RDNA4N_FENCE_SLOTS; i++) {
                if (s->fslot[i].used && s->fslot[i].handle == h) {
                    const uint64_t zero = 0;
                    bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
                    s->fslot[i] = FenceSlot{ 0, 0, 0 };
                    slotCleared = true;
                }
            }
        }
        if (slotCleared) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk; }
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) { if (memOk) { gtt_release(b); sys_release(b); } }   // review item D: a flush that did not acknowledge leaks the GTT pages (they stay counted, R4)
        else if (b.kind == kBoHi) { gCtx->gmc.vram_alloc_hi.free(b.a); budget_release(b); }
        else if (b.kind == kBoVis) { gCtx->gmc.vram_alloc.free(b.a); budget_release(b); }
        else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) { host_release(b); sys_release(b); }   // 0.0.612: AFTER pt_unmap and flush_vmid above (unmap before complete)
    } else {
        // Leak: the memory, the PTEs and any fence-slot PTE stay; only the software records go.
        for (uint32_t i = 0; i < RDNA4N_FENCE_SLOTS; i++) if (s->fslot[i].used && s->fslot[i].handle == h) s->fslot[i].used = 0;
    }
    va_remove_handle(s->maps, kMaxMaps, h);
    if (b.kind == kBoGtt) s->gttUsed -= b.size;
    else if (b.kind == kBoHost) s->importedBytes = import_after_free(s->importedBytes, b.size);   // 0.0.612 (a leaked host BO: its pages stay wired, only the software record goes)
    else s->vramBytes -= b.size;
    s->nBos--;
    s->boUsed[h] = 0;
    memset(&b, 0, sizeof(b));
}

IOReturn n1c_bo_create(const N1cRef &ref, const rdna4n_gem_create_in *in, uint64_t out[4]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    const Place p = place_decide(in->bo_size, in->alignment, in->domains, in->domain_flags);
    if (p.rc != kOk) return (IOReturn)p.rc;
    uint32_t seq0 = 0u;
    bool multi0 = false, ws0 = false, gttReserved = false, gttTaken = false;
    IOBufferMemoryDescriptor *gmd = nullptr;
    uint64_t *gpages = nullptr;
    uint32_t gpageCount = 0u;
    if (p.kind != kPlaceVis) {
        if (p.size > kGttMaxBo) return kIOReturnNoMemory;
        Session *s0 = sess_peek(ref);
        if (s0 == nullptr || !__atomic_load_n(&s0->hello, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        seq0 = __atomic_load_n(&s0->id, __ATOMIC_ACQUIRE);
        multi0 = multi_on();
        ws0 = s0->ws;
        if (would_exceed(__atomic_load_n(&s0->gttUsed, __ATOMIC_RELAXED), p.size, kGttCap)) return kIOReturnNoMemory;
        if (multi0 && !ws0) {
            IOLockLock(gCliLock);
            const uint64_t hcap = host_cap_now();
            if (!rdna4native::g2::host_reserve_ok(gSysHeld, gSysPending, p.size, hcap)) {
                const uint64_t heldPend = rdna4native::g2::sat_add(gSysHeld, gSysPending);
                N1C_LOG("BoCreate refused (NoMemory): slot %u: GTT + host imports of all sessions would pass the host cap %llu MiB (%llu MiB held+pending, %llu KiB asked; phys %llu MiB)", s0->slot,
                        (unsigned long long)(hcap >> 20), (unsigned long long)(heldPend >> 20), (unsigned long long)(p.size >> 10),
                        (unsigned long long)(host_physmem_bytes() >> 20));
                IOLockUnlock(gCliLock);
                return kIOReturnNoMemory;
            }
            gSysPending = rdna4native::g2::host_pending_acquire(gSysPending, p.size);
            gttReserved = true;
            IOLockUnlock(gCliLock);
        }
        const IOReturn grc = gtt_alloc_pages(p.size, &gmd, &gpages, &gpageCount);
        if (grc != kIOReturnSuccess) {
            sys_pending_rollback(gttReserved, p.size);
            return grc;
        }
    }
    Session *s = sess_lock(ref, true);
    if (s == nullptr) {
        if (gmd != nullptr) { gmd->complete(); gmd->release(); }
        if (gpages != nullptr) IOFree(gpages, (vm_size_t)gpageCount * sizeof(uint64_t));
        sys_pending_rollback(gttReserved, p.size);
        return kIOReturnNotReady;   // closed while we waited for the lock
    }
    DeviceContext &dev = *gCtx->dev;
    IOReturn rc = (IOReturn)gpu_gate(s);
    const bool multi = multi_on();
    IOLockLock(gCliLock);   // the VRAM pools and the budget are shared
    do {
        if (rc != kIOReturnSuccess) break;
        const uint32_t h = lowest_free(s->boUsed, 1, RDNA4N_MAX_BOS - 1);
        if (h == 0) { rc = kIOReturnNoResources; break; }
        Bo b {};
        b.uc = p.uc; b.dcc = p.dcc && dcc_on(); b.size = p.size;
        uint32_t placed = RDNA4N_GEM_DOMAIN_GTT, bits = 0;
        if (p.kind == kPlaceVis) {
            // 0.0.627 (G2): non-WindowServer sessions together stay within the shared allowance (switch ON; always true with it OFF).
            // 2.1: pools, free bytes and page-table blocks from the same snapshot (gCliLock is already held here).
            const uint64_t poolTotal = rdna4native::g2::sat_add(gCtx->gmc.vram_alloc.size(), gCtx->gmc.vram_alloc_hi.is_inited() ? gCtx->gmc.vram_alloc_hi.size() : 0ull);
            const uint64_t poolFree = rdna4native::g2::sat_add(gCtx->gmc.vram_alloc.bytes_free(), gCtx->gmc.vram_alloc_hi.is_inited() ? gCtx->gmc.vram_alloc_hi.bytes_free() : 0ull);
            const uint64_t ptHeld = rdna4native::g2::pt_held_bytes(gPtN, kArenaBlockBytes);
            if (!rdna4native::g2::app_vram_budget_ok(multi, s->ws, gVramOthers, p.size, poolTotal, poolFree, ptHeld)) {
                N1C_LOG("BoCreate refused (NoMemory): slot %u is over the shared VRAM allowance for non-WindowServer sessions (%llu MiB held of %llu MiB, %llu KiB asked)", s->slot,
                        (unsigned long long)(gVramOthers >> 20), (unsigned long long)(rdna4native::g2::app_vram_cap(poolTotal, poolFree, gVramOthers, ptHeld) >> 20), (unsigned long long)(p.size >> 10));
                rc = kIOReturnNoMemory; break;
            }
            VRAMAllocation a {};
            // 0.0.641 (G4 review) + small-BAR WS fix: on small BAR (!fullBar), ALL NO_CPU_ACCESS BOs (WindowServer and apps alike) go to the hi pool FIRST so ~200 MiB visible VRAM
            // stays free for scanout, CPU-mapped BOs, descriptor pools and GPU page tables (PtMem); with apps ON, non-WindowServer CPU-visible placements stay under appvis_cap() (default 64 MiB).
            // On a full-BAR (> 1 GiB) boot, visible pool is nearly all usable VRAM (hi pool is tiny), so hi_first and appvis_active both stay OFF while g2::app_vram_budget_ok still binds.
            const bool fullBar = rdna4rebar::is_full_bar_aperture(dev.bar0Size, dev.vramLimit);
            const bool appsNow = rdna4native::g4::appvis_active(apps_on(), fullBar);
            // Opportunistic 2 MiB alignment only for exact 2 MiB multiples (zero rounding waste in VRAMBumpAllocator); falls back to p.align on fragmentation.
            const uint64_t hugeAlign = rdna4native::s1c::bo_alloc_align(p.size, p.align, hugepage_on());
            bool got = false;
            bool hiDone = rdna4native::g4::hi_first(fullBar, p.hiOk) && gCtx->gmc.vram_alloc_hi.is_inited() &&
                          ((hugeAlign > p.align && gCtx->gmc.vram_alloc_hi.alloc(p.size, hugeAlign, &a)) ||
                           gCtx->gmc.vram_alloc_hi.alloc(p.size, p.align, &a));
            if (!hiDone) {
                if (!rdna4native::g4::appvis_ok(appsNow, s->ws, gVisOthers, p.size, appsNow ? appvis_cap() : 0ull)) {
                    N1C_LOG("BoCreate refused (NoMemory): slot %u is over the visible-VRAM cap for non-WindowServer sessions (%llu KiB held of %llu MiB, %llu KiB asked)", s->slot,
                            (unsigned long long)(gVisOthers >> 10), (unsigned long long)(appvis_cap() >> 20), (unsigned long long)(p.size >> 10));
                    rc = kIOReturnNoMemory; break;
                }
                if (hugeAlign > p.align && gCtx->gmc.vram_alloc.alloc(p.size, hugeAlign, &a)) {
                    if (rdna4rebar::vis_vram_range_ok(a.gpu_va, a.size, gCtx->gmc.vram_start, dev.vramLimit, dev.bar0Size, dev.vramSizeBytes))
                        got = true;
                    else
                        gCtx->gmc.vram_alloc.free(a);
                }
                if (!got) {
                    got = gCtx->gmc.vram_alloc.alloc(p.size, p.align, &a);
                    if (got) {
                        if (!rdna4rebar::vis_vram_range_ok(a.gpu_va, a.size, gCtx->gmc.vram_start, dev.vramLimit, dev.bar0Size, dev.vramSizeBytes)) { gCtx->gmc.vram_alloc.free(a); got = false; }
                    }
                }
            }
            if (got) {
                b.kind = kBoVis; b.a = a; b.mc = a.gpu_va;
                bar0_memset_vram(dev, a.gpu_va - gCtx->gmc.vram_start, 0, a.size);
                amdgpu_hdp_flush(dev);
                placed = RDNA4N_GEM_DOMAIN_VRAM; bits = (b.dcc ? 0u : RDNA4N_PLACED_CPU_MAPPABLE) | RDNA4N_PLACED_ZEROED;
                if (rdna4native::g4::appvis_counts(appsNow, s->ws, true)) { b.visCounted = 1u; gVisOthers += p.size; }
            } else if (hiDone || (p.hiOk && gCtx->gmc.vram_alloc_hi.is_inited() &&
                                  ((hugeAlign > p.align && gCtx->gmc.vram_alloc_hi.alloc(p.size, hugeAlign, &a)) ||
                                   gCtx->gmc.vram_alloc_hi.alloc(p.size, p.align, &a)))) {
                b.kind = kBoHi; b.a = a; b.mc = a.gpu_va;
                placed = RDNA4N_GEM_DOMAIN_VRAM; bits = RDNA4N_PLACED_HI_POOL;
            } else { rc = kIOReturnNoMemory; break; }
            if (multi && !s->ws) { b.budgeted = 1u; gVramOthers += p.size; }
        } else {
            if (!session_unchanged(seq0, s->id) || multi != multi0 || s->ws != ws0) { rc = kIOReturnNotReady; break; }
            if (p.size > kGttMaxBo || would_exceed(s->gttUsed, p.size, kGttCap)) { rc = kIOReturnNoMemory; break; }
            b.kind = kBoGtt; b.gmd = gmd; b.hpages = gpages; b.hpageCount = gpageCount;
            s->gttUsed += p.size;
            if (multi) {
                if (s->ws) { b.sysCounted = 1u; gSysHeld = rdna4native::g2::sat_add(gSysHeld, p.size); }
                else {
                    const rdna4native::g2::HostCommit hc = rdna4native::g2::host_commit(gSysHeld, gSysPending, p.size);
                    gSysHeld = hc.held; gSysPending = hc.pending;
                    gttReserved = false;
                    b.sysCounted = 1u;
                }
            }
            gttTaken = true;
            bits = RDNA4N_PLACED_CPU_MAPPABLE | RDNA4N_PLACED_ZEROED;
        }
        if (b.kind != kBoGtt) s->vramBytes += p.size;
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        out[0] = h; out[1] = p.size; out[2] = placed; out[3] = bits;
    } while (false);
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    if (!gttTaken) {
        if (gmd != nullptr) { gmd->complete(); gmd->release(); }
        if (gpages != nullptr) IOFree(gpages, (vm_size_t)gpageCount * sizeof(uint64_t));
        sys_pending_rollback(gttReserved, p.size);
    }
    return rc;
}

// ---- 0.0.635 (M4c): the monitor B plane's two scanout buffers (see native_s1c.h). 0.0.655: ONE PAIR PER INSTANCE (slot 0 = the monitor A, instance 1; slot 1 = the monitor B, instance 2): each instance holds, frees and pins its own pair; the monitor B's held
// (or pinned) pair never blocks the monitor A's allocation and the reverse; a second pair for the SAME instance is refused. Everything here runs under gCliLock, the lock every user of the shared VRAM pools holds; no register, no memory is touched. ----
static VRAMAllocation gD2Buf[2][2];
static bool gD2Held[2];            // under gCliLock
static bool gD2Pinned[2];          // 0.0.652 (M5), under gCliLock: `disp2 fbhold` pinned the pair for the rest of the boot (a reboot is the release); never cleared
static BringupContext *gD2Ctx[2];  // the context the pair came from (under gCliLock)
static inline uint32_t d2slot(uint32_t inst) { return inst == 1u ? 0u : 1u; }
IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]) {
    if ((inst != 1u && inst != 2u) || bytes == 0 || (bytes & 0xFFFFull) != 0 || lazy_lock(&gCliLock) == nullptr) return kIOReturnBadArgument;
    const uint32_t sl = d2slot(inst);
    IOReturn rc = kIOReturnSuccess;
    IOLockLock(gCliLock);
    do {
        if (rdna4d2pin::alloc_refused(gD2Held[sl])) { rc = kIOReturnExclusiveAccess; break; }
        if (!ctx.dev || !ctx.gmc.vram_alloc.is_inited()) { rc = kIOReturnNotReady; break; }
        VRAMAllocation a[2] {};
        uint32_t got = 0;
        bool ok = true;
        for (uint32_t k = 0; k < 2 && ok; k++) {
            if (!ctx.gmc.vram_alloc.alloc(bytes, 65536, &a[k])) { ok = false; break; }
            got = k + 1;
            ok = (a[k].gpu_va & 0xFFFFull) == 0 && rdna4rebar::vis_vram_range_ok(a[k].gpu_va, a[k].size, ctx.gmc.vram_start, ctx.dev->vramLimit, ctx.dev->bar0Size, ctx.dev->vramSizeBytes);     // 64 KiB aligned and INSIDE valid visible BAR0, else give it back
        }
        if (!ok) {
            for (uint32_t k = 0; k < got; k++) ctx.gmc.vram_alloc.free(a[k]);
            rc = kIOReturnNoMemory;
            break;
        }
        gD2Buf[sl][0] = a[0]; gD2Buf[sl][1] = a[1]; gD2Held[sl] = true; gD2Ctx[sl] = &ctx;
        for (uint32_t k = 0; k < 2; k++) { mc[k] = a[k].gpu_va; off[k] = a[k].gpu_va - ctx.gmc.vram_start; }
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}
bool n1c_vram_alloc_excl(BringupContext &ctx, uint64_t bytes, uint64_t align, VRAMAllocation *out) {
    if (!out || bytes == 0 || !ctx.dev || lazy_lock(&gCliLock) == nullptr) return false;
    bool ok = false;
    IOLockLock(gCliLock);
    if (ctx.gmc.vram_alloc.is_inited() && ctx.gmc.vram_alloc.alloc(bytes, align, out)) {
        if (rdna4rebar::vis_vram_range_ok(out->gpu_va, out->size, ctx.gmc.vram_start, ctx.dev->vramLimit, ctx.dev->bar0Size, ctx.dev->vramSizeBytes))
            ok = true;
        else
            ctx.gmc.vram_alloc.free(*out);
    }
    IOLockUnlock(gCliLock);
    return ok;
}
bool n1c_vram_alloc_excl_at(BringupContext &ctx, uint64_t gpuVa, uint64_t bytes, VRAMAllocation *out) {
    if (!out || bytes == 0 || !ctx.dev || lazy_lock(&gCliLock) == nullptr) return false;
    bool ok = false;
    IOLockLock(gCliLock);
    if (ctx.gmc.vram_alloc.is_inited() &&
        rdna4rebar::vis_vram_range_ok(gpuVa, bytes, ctx.gmc.vram_start, ctx.dev->vramLimit, ctx.dev->bar0Size, ctx.dev->vramSizeBytes) &&
        ctx.gmc.vram_alloc.alloc_at(gpuVa, bytes, out)) {
        ok = true;
    }
    IOLockUnlock(gCliLock);
    return ok;
}
void n1c_vram_free_excl(BringupContext &ctx, const VRAMAllocation &a) {
    if (a.size == 0 || gCliLock == nullptr) return;
    IOLockLock(gCliLock);
    if (ctx.gmc.vram_alloc.is_inited()) ctx.gmc.vram_alloc.free(a);
    IOLockUnlock(gCliLock);
}

void n1c_d2_free(uint32_t inst) {
    if ((inst != 1u && inst != 2u) || gCliLock == nullptr) return;
    const uint32_t sl = d2slot(inst);
    IOLockLock(gCliLock);
    if (rdna4d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == rdna4d2pin::kFreePinned) {      // 0.0.652 (M5): the framebuffer scans buffer A and WindowServer maps it: a freed buffer that is still mapped would let its CPU writes corrupt whatever reuses that VRAM. A logged NO-OP while pinned.
        N1C_LOG("d2 free REFUSED: instance %u's pair is pinned (disp2 fbhold) for the rest of this boot; nothing was returned to the allocator", inst);
        IOLockUnlock(gCliLock);
        return;
    }
    if (rdna4d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == rdna4d2pin::kFreeDo) {
        gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][0]);
        gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][1]);
        gD2Buf[sl][0] = VRAMAllocation {}; gD2Buf[sl][1] = VRAMAllocation {};
        gD2Held[sl] = false; gD2Ctx[sl] = nullptr;
    }
    IOLockUnlock(gCliLock);
}

// 0.0.652 (M5): `disp2 fbhold 2` pins the pair (0.0.655: of the instance). Under gCliLock: the allocator must hold exactly the pair the plane reports (A = mc[0], B = mc[1]) and it must not be pinned already. false = nothing changed.
bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]) {
    if ((inst != 1u && inst != 2u) || mc == nullptr || gCliLock == nullptr) return false;
    const uint32_t sl = d2slot(inst);
    bool ok = false;
    IOLockLock(gCliLock);
    if (rdna4d2pin::pin_ok(gD2Held[sl], gD2Ctx[sl] != nullptr, gD2Pinned[sl], mc[0] != 0u && gD2Buf[sl][0].gpu_va == mc[0], mc[1] != 0u && gD2Buf[sl][1].gpu_va == mc[1])) { gD2Pinned[sl] = true; ok = true; }
    IOLockUnlock(gCliLock);
    if (ok) N1C_LOG("d2 pair of instance %u PINNED (disp2 fbhold): A %#llx B %#llx stay allocated for the rest of this boot", inst, (unsigned long long)mc[0], (unsigned long long)mc[1]);
    return ok;
}
bool n1c_d2_pinned(uint32_t inst) { return (inst == 1u || inst == 2u) && __atomic_load_n(&gD2Pinned[d2slot(inst)], __ATOMIC_ACQUIRE); }

IOReturn n1c_bo_free(const N1cRef &ref, uint64_t handle) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (handle > 0xFFFFFFFFull || !id_live(s->boUsed, RDNA4N_MAX_BOS - 1, (uint32_t)handle)) rc = kIOReturnNotFound;
    else {
        const uint32_t h = (uint32_t)handle;
        RelMode m = kRelNormal;
        bool noBlame = false;
        const Bo &b0 = s->bo[h];
        // 0.0.629 (G2 review fix R6), switch ON: a visible-VRAM BO whose descriptor still has a user mapping (a reference beyond the record's own) is NOT freed:
        // Busy, nothing touched, the handle stays valid (unmap, then free again). Under HUNG the memory is leaked anyway, so the check is not needed there.
        const uint32_t mrc = hung_now() ? kOk : rdna4native::g2::bofree_mapping_rc(multi_on(), b0.kind == kBoVis, b0.vmd != nullptr, b0.vmd != nullptr ? (uint32_t)b0.vmd->getRetainCount() : 0u);
        if (mrc != kOk) {
            N1C_LOG("BoFree refused (Busy): slot %u BO %u (visible VRAM, %llu KiB) is still CPU-mapped (%d descriptor references): unmap it first", s->slot, h,
                    (unsigned long long)(b0.size >> 10), b0.vmd->getRetainCount());
            rc = (IOReturn)mrc;
        } else {
            if (hung_now()) m = kRelLeak;                                    // HUNG: drop the handle, leak the memory and the PTEs, succeed
            else if (!own_wait(s, &noBlame)) { m = kRelLeak; rc = kIOReturnTimeout; }  // 0.0.627: THIS session's work only. Not retired: the handle is dropped and leaked
            if (noBlame) {
                // 0.0.629 (R2), switch ON: the 2 s ran out while the GPU was still making progress. Nobody was blamed; the BO is KEPT (handle valid, memory and PTEs
                // as they were) and the caller gets Timeout. Its close frees it after its own work retired.
                N1C_LOG("BoFree: slot %u BO %u: own work not retired within 2 s while the GPU is progressing: Timeout, the BO is kept (no blame)", s->slot, h);
            } else {
                IOLockLock(gCliLock);
                bo_release(s, h, m);
                IOLockUnlock(gCliLock);
            }
        }
    }
    sess_unlock(s);
    return rc;
}

// 0.0.612 (review item A): the top of DRAM is NOT read from a kernel symbol: max_mem / mem_actual / sane_size exist in the kernel's own symbol table but are not in any KPI header or export list
// the SDK carries, so a reference could leave the kext unloadable. The kext's own derivation (the EFI memory map, gfx_ramtop.h) is used instead; 0 = it was not read cleanly = unknown.
void n1c_latch_pci_bars(const uint64_t *base, const uint64_t *size, uint32_t n) {
    if (base == nullptr || size == nullptr || n > kMaxPciBars) return;
    if (!OSCompareAndSwap(0, 2, &gBarsState)) return;          // first writer wins; 2 = being written
    for (uint32_t i = 0; i < n; i++) { gBars[i].base = base[i]; gBars[i].size = size[i]; }
    gNBars = n;
    __atomic_store_n(&gBarsState, 1u, __ATOMIC_RELEASE);
    N1C_LOG("pci bars latched: %u BAR range(s) will never be imported as host pages", n);
}

// ---- BoImportHost (0.0.612, ABI 1.9) ----------------------------------------------------------------------------------------------------
// The pure collect_pages' segment source over a prepare()d descriptor: the physical address of byte `off` and the length of the contiguous run there. CPU physical == bus (no IOMMU on this platform).
struct DescSeg {
    IOMemoryDescriptor *md;
    uint64_t phys(uint64_t off, uint64_t *len) {
        IOByteCount seg = 0;
        const addr64_t pa = md->getPhysicalSegment((IOByteCount)off, &seg, kIOMemoryMapperNone);
        *len = pa != 0 ? (uint64_t)seg : 0ull;
        return (uint64_t)pa;
    }
};
// Order: (1) argument and advisory cap check, no lock, nothing touched; (2) wire the caller's range and read its physical pages OUTSIDE the client lock (prepare() may fault pages in and
// sleep: a racing close must not wait for a page-in); (3) under the session lock and gCliLock: the HUNG gate, the AUTHORITATIVE cap check, the handle, the optional map, the record. A failure before (3)
// completes leaves nothing mapped, so the descriptor is completed and released even under the HUNG latch (no PTE ever named those pages).
IOReturn n1c_bo_import_host(const N1cRef &ref, task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]) {
    const uint32_t seq0 = ref.id;   // review item C: the session this import belongs to, fixed BEFORE anything is wired (0.0.627: the client's own reference)
    if (!sess_hello(ref)) {
        N1C_LOG("BoImportHost refused at hello: session not ready (range %#llx + %llu KiB)", (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        return kIOReturnNotReady;
    }
    if (task == nullptr || out == nullptr) {
        N1C_LOG("BoImportHost refused at arguments: null task/output (task %p, output %p)", (void *)task, (void *)out);
        return kIOReturnBadArgument;
    }
    Session *s0 = sess_peek(ref);
    if (s0 == nullptr) {
        N1C_LOG("BoImportHost refused at session lookup: no live session (id %u)", seq0);
        return kIOReturnNotReady;
    }
    // 3.5: WS/OFF use the legacy per-client advisory; other multi sessions use held+pending against the host cap.
    // Per-BO 64 MiB and the RO/BAR/owner checks below are shared.
    const bool multi0 = multi_on();
    const bool ws0 = s0->ws;
    ImportChk pre { kBadArg, 0, 0 };
    if (!multi0 || ws0) {
        pre = import_check(hostVa, size, flags, gpuVa, __atomic_load_n(&s0->importedBytes, __ATOMIC_RELAXED));
        if (pre.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(__atomic_load_n(&s0->importedBytes, __ATOMIC_RELAXED) >> 20), (unsigned long long)(size >> 10));   // 0.0.620
        if (pre.rc != kOk) {
            if (pre.rc != kNoMemory) N1C_LOG("BoImportHost refused at argument check: rc %#x, va %#llx, size %llu KiB, flags %#llx, gpuVa %#llx",
                                             pre.rc, (unsigned long long)hostVa, (unsigned long long)(size >> 10), (unsigned long long)flags, (unsigned long long)gpuVa);
            return (IOReturn)pre.rc;
        }
    } else {
        const uint64_t heldPendAdvisory = rdna4native::g2::sat_add(__atomic_load_n(&gSysHeld, __ATOMIC_RELAXED), __atomic_load_n(&gSysPending, __ATOMIC_RELAXED));
        const uint64_t hcapAdvisory = host_cap_now();
        pre = import_check_sys(hostVa, size, flags, gpuVa, heldPendAdvisory, hcapAdvisory);
        if (pre.rc == kNoMemory) N1C_LOG("import refused (advisory): host cap %llu MiB reached (%llu MiB held+pending, %llu KiB asked; phys %llu MiB)",
                                         (unsigned long long)(hcapAdvisory >> 20), (unsigned long long)(heldPendAdvisory >> 20),
                                         (unsigned long long)(size >> 10), (unsigned long long)(host_physmem_bytes() >> 20));
        if (pre.rc != kOk) {
            if (pre.rc != kNoMemory) N1C_LOG("BoImportHost refused at argument check: rc %#x, va %#llx, size %llu KiB, flags %#llx, gpuVa %#llx",
                                             pre.rc, (unsigned long long)hostVa, (unsigned long long)(size >> 10), (unsigned long long)flags, (unsigned long long)gpuVa);
            return (IOReturn)pre.rc;
        }
    }
    // 3.5: reserve before pinning, never across prepare(). Pending stays charged until the descriptor is unpinned, then commit or rollback.
    bool reserved = false;
    if (multi0 && !ws0) {
        IOLockLock(gCliLock);
        const uint64_t hcap = host_cap_now();
        if (!rdna4native::g2::host_reserve_ok(gSysHeld, gSysPending, size, hcap)) {
            const uint64_t heldPend = rdna4native::g2::sat_add(gSysHeld, gSysPending);
            N1C_LOG("import refused (NoMemory): slot %u: GTT + host imports of all sessions would pass the host cap %llu MiB (%llu MiB held+pending, %llu KiB asked; phys %llu MiB)", s0->slot,
                    (unsigned long long)(hcap >> 20), (unsigned long long)(heldPend >> 20), (unsigned long long)(size >> 10),
                    (unsigned long long)(host_physmem_bytes() >> 20));
            IOLockUnlock(gCliLock);
            return kIOReturnNoMemory;
        }
        gSysPending = rdna4native::g2::host_pending_acquire(gSysPending, size);
        reserved = true;
        IOLockUnlock(gCliLock);
    }
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)hostVa, (mach_vm_size_t)size, kIODirectionInOut, task);
    if (md == nullptr) {
        N1C_LOG("BoImportHost refused at withAddressRange: returned null (va %#llx, size %llu KiB)", (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        sys_pending_rollback(reserved, size);
        return kIOReturnNoMemory;
    }
    // Read-only CA backing cannot be wired for device writes. Out wires for device reads;
    // both GPU mapping paths must keep these pages read-only too.
    IOReturn prepareRc = md->prepare();
    uint8_t hro = 0u;
    if (prepareRc == KERN_PROTECTION_FAILURE) {
        const IOReturn rcInOut = prepareRc;
        md->release();
        md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)hostVa, (mach_vm_size_t)size, kIODirectionOut, task);
        if (md == nullptr) {
            N1C_LOG("BoImportHost refused at withAddressRange(read): returned null after InOut %#x (va %#llx, size %llu KiB)", (unsigned int)rcInOut, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            sys_pending_rollback(reserved, size);
            return kIOReturnNoMemory;
        }
        prepareRc = md->prepare();
        if (prepareRc != kIOReturnSuccess) {
            N1C_LOG("BoImportHost refused at prepare: IOReturn %#x (InOut %#x first, va %#llx, size %llu KiB)", (unsigned int)prepareRc, (unsigned int)rcInOut, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            md->release();
            sys_pending_rollback(reserved, size);
            return kIOReturnBadArgument;   // the range is not (fully) readable/mapped in the caller
        }
        hro = 1u;
        N1C_LOG("BoImportHost: InOut prepare refused %#x; wired read-only (va %#llx, size %llu KiB)", (unsigned int)rcInOut, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
    }
    if (prepareRc != kIOReturnSuccess) {
        N1C_LOG("BoImportHost refused at prepare: IOReturn %#x (va %#llx, size %llu KiB)", (unsigned int)prepareRc, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        md->release();
        sys_pending_rollback(reserved, size);
        return kIOReturnBadArgument;
    }
    uint64_t *pages = static_cast<uint64_t *>(IOMalloc((vm_size_t)(pre.pages * sizeof(uint64_t))));
    if (pages == nullptr) {
        N1C_LOG("BoImportHost refused at page-list allocation: %llu bytes (va %#llx, size %llu KiB)",
                (unsigned long long)(pre.pages * sizeof(uint64_t)), (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        md->complete(); md->release();
        sys_pending_rollback(reserved, size);
        return kIOReturnNoMemory;
    }
    DescSeg seg { md };
    const uint32_t crc = collect_pages(seg, size, pages, kImportMaxPages);
    if (crc != kOk) {
        IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
        md->complete(); md->release();
        N1C_LOG("import refused (%#x): the caller's range %#llx + %llu KiB has no usable physical page list", crc, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        sys_pending_rollback(reserved, size);
        return (IOReturn)crc;
    }
    {   // review item A: a page of device memory (any of this GPU's BARs, or above the top of DRAM when known) is never imported. Nothing is mapped yet, so the descriptor is completed and released.
        const bool barsOk = __atomic_load_n(&gBarsState, __ATOMIC_ACQUIRE) == 1u;
        const uint64_t dramTop = amdgpu::sysmem_ramtop_derived_or_zero();
        const uint64_t bad = import_first_refused(pages, pre.pages, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop);
        if (bad < pre.pages) {
            const uint64_t badPa = pages[bad];
            const PageVerdict v = import_page_verdict(badPa, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop);
            IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
            md->complete(); md->release();
            N1C_LOG("import refused: device page (page %llu of %llu at pa %#llx: %s)", (unsigned long long)bad, (unsigned long long)pre.pages, (unsigned long long)badPa,
                    v == kPageInBar ? "inside a PCI BAR of this GPU" : v == kPageAboveDram ? "above the top of DRAM" : "no BAR list latched");
            sys_pending_rollback(reserved, size);
            return kIOReturnBadArgument;
        }
    }
    IOReturn rc = kIOReturnSuccess;
    bool taken = false;
    Session *s = sess_lock(ref, false);
    const uint32_t gate = (s != nullptr && s->hello) ? gpu_gate(s) : kOk;   // 0.0.627: the HUNG / poison gate with only the session lock held (a stall poll may run a recovery: never under gCliLock)
    if (s != nullptr) IOLockLock(gCliLock);   // the page-table reserve is shared
    do {
        if (s == nullptr || !s->hello) {
            N1C_LOG("BoImportHost refused after wiring: session closed before lock (id %u, va %#llx, size %llu KiB)", seq0, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            rc = kIOReturnNotReady; break;
        }   // closed while we waited for the lock
        if (!session_unchanged(seq0, s->id)) {
            N1C_LOG("BoImportHost refused after wiring: session changed from %u to %u (va %#llx, size %llu KiB)", seq0, s->id, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            rc = kIOReturnNotReady; break;
        }   // review item C: a different (or no) session now: these pages are the old client's
        if (gate != kOk) {
            N1C_LOG("BoImportHost refused by GPU gate: rc %#x (slot %u, va %#llx, size %llu KiB)", gate, s->slot, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            rc = (IOReturn)gate; break;
        }
        // 3.5 authoritative: multi/WS identity is immutable for this session id and must match the advisory path.
        const bool multiNow = multi_on();
        if (multiNow != multi0 || s->ws != ws0) {
            N1C_LOG("BoImportHost refused at identity invariant: multi %d->%d, ws %d->%d (slot %u, id %u)",
                    multi0 ? 1 : 0, multiNow ? 1 : 0, ws0 ? 1 : 0, s->ws ? 1 : 0, s->slot, seq0);
            rc = kIOReturnInternalError; break;
        }
        // WS/OFF use the legacy cap. Multi non-WS must still own its pre-pinning reservation before any MAP; the pending charge is
        // already the budget, so only the arguments are revalidated here. RO/BAR/owner checks remain unchanged below.
        ImportChk c { kBadArg, 0, 0 };
        if (!multiNow || s->ws) {
            c = import_check(hostVa, size, flags, gpuVa, s->importedBytes);   // authoritative: the total as of now
            if (c.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached under the lock (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(s->importedBytes >> 20), (unsigned long long)(size >> 10));   // 0.0.620
            if (c.rc != kOk) {
                if (c.rc != kNoMemory) N1C_LOG("BoImportHost refused at locked argument check: rc %#x (slot %u, va %#llx, size %llu KiB)", c.rc, s->slot, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
                rc = (IOReturn)c.rc; break;
            }
            if (!rdna4native::g2::sys_budget_ok(multi_on(), s->ws, gSysHeld, size)) {   // 0.0.629 (R4): under gCliLock, before anything is mapped
                N1C_LOG("import refused (NoMemory): slot %u: GTT + host imports of all sessions would pass the global %llu MiB (%llu MiB held, %llu KiB asked)", s->slot,
                        (unsigned long long)(rdna4native::g2::kSysGlobalCap >> 20), (unsigned long long)(gSysHeld >> 20), (unsigned long long)(size >> 10));
                rc = kIOReturnNoMemory; break;
            }
        } else {
            if (!reserved) {
                N1C_LOG("BoImportHost refused at system-budget invariant: multi non-WS import reached MAP without a reservation (slot %u, id %u)", s->slot, seq0);
                rc = kIOReturnInternalError; break;
            }
            c = import_check(hostVa, size, flags, gpuVa, 0ull);   // args only: size <= 64 MiB so the legacy per-client cap always passes; system already reserved
            if (c.rc != kOk) {
                N1C_LOG("BoImportHost refused at locked argument check (3.5 args-only): rc %#x (slot %u, va %#llx, size %llu KiB)", c.rc, s->slot, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
                rc = (IOReturn)c.rc; break;
            }
            // No second system-cap check here: reservation (pending) IS the charge. Re-checking held+pending+size would double-count
            // our own reservation. Commit below converts pending -> held exactly once.
        }
        const uint32_t h = lowest_free(s->boUsed, 1, RDNA4N_MAX_BOS - 1);
        if (h == 0) {
            N1C_LOG("BoImportHost refused: no free BO handle (slot %u, va %#llx, size %llu KiB)", s->slot, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
            rc = kIOReturnNoResources; break;
        }
        uint32_t slot = 0xFFFFFFFFu;
        bool mapped = false;
        if (c.gpuStripped != 0ull) {
            if (va_overlap(s->maps, kMaxMaps, c.gpuStripped, size) >= 0) {
                N1C_LOG("BoImportHost refused: GPU VA overlaps an existing map (slot %u, gpuVa %#llx, size %llu KiB)", s->slot, (unsigned long long)c.gpuStripped, (unsigned long long)(size >> 10));
                rc = kIOReturnBadArgument; break;
            }
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) {
                N1C_LOG("BoImportHost refused: no free GPU map slot (slot %u, gpuVa %#llx, size %llu KiB)", s->slot, (unsigned long long)c.gpuStripped, (unsigned long long)(size >> 10));
                rc = kIOReturnNoResources; break;
            }
            PtMem mem{ s };
            // 3.1: transactional map at the canonical form of the stripped VA. gemva_check
            // (via import_check) already validated the range; the public canonical check stays
            // strict, so canonicalize here. s->maps keeps the stripped form (existing contract).
            // Quota gates only in alloc_page per new block (global = min(64MiB, visible/4);
            // free-reserve reuse bypasses global/visible; client stays 32MiB); upper-bound
            // helpers are diagnostics, never a reuse gate. Failure returns blocks to the
            // pt reserve, never to vram_alloc.
            const uint64_t vaCanon = va_canonicalize(c.gpuStripped);
            uint32_t r = pt_map_pages_tx(s->rootPa, vaCanon, c.pages, pages, leaf_flags(host_vm_flags((uint32_t)flags, hro != 0u), false, true), mem);   // SYSTEM | SNOOPED (+ R/X as asked; W only when the caller's pages are writable)
            if (r == kNoMemory) {
                amdgpu_hdp_flush(*gCtx->dev);
                const uint32_t f = session_flush_if_bound_locked(s);   // invalidate rolled-back directory pointers before another session reuses the reserve
                if (f != kOk) { mem.quarantine_rollback(); r = f; }
            }
            if (r != kOk) {
                N1C_LOG("BoImportHost refused at page-table map: rc %#x (slot %u, gpuVa %#llx, pages %llu)", r, s->slot, (unsigned long long)c.gpuStripped, (unsigned long long)c.pages);
                rc = (IOReturn)r; break;
            }
            mapped = true;
        }
        Bo b {};
        b.kind = kBoHost; b.size = size; b.hmd = md; b.hpages = pages; b.hpageCount = (uint32_t)c.pages; b.hro = hro;
        if (multiNow) {   // 3.5: commit the BEFORE-pinning reservation (pending -> held, exactly once, no double account)
            if (s->ws) { b.sysCounted = 1u; gSysHeld = rdna4native::g2::sat_add(gSysHeld, size); }   // WS never reserves; direct held (still counts)
            else {
                const rdna4native::g2::HostCommit hc = rdna4native::g2::host_commit(gSysHeld, gSysPending, size);
                gSysHeld = hc.held; gSysPending = hc.pending;
                reserved = false;
                b.sysCounted = 1u;
            }
        }
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        s->importedBytes += size;
        taken = true;                                             // md and pages now belong to the BO record
        if (mapped) {
            s->maps[slot] = VaEnt{ c.gpuStripped, size, 0, h, (uint32_t)flags };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = session_flush_if_bound_locked(s);   // synchronous: the mapping is complete on return
            if (f != kOk) rc = (IOReturn)f;                        // a hang was latched: the BO stays recorded (leaked at close)
        }
        out[0] = h; out[1] = size; out[2] = mapped ? va_canonicalize(c.gpuStripped) : 0ull; out[3] = RDNA4N_PLACED_HOST_IMPORT;
    } while (false);
    if (s != nullptr) { IOLockUnlock(gCliLock); sess_unlock(s); }
    if (!taken) {
        IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
        md->complete(); md->release();                            // never mapped: safe even when HUNG
        sys_pending_rollback(reserved, size);   // 3.5: rollback the reservation exactly once (held untouched)
    }
    return rc;
}

IOReturn n1c_gem_va(const N1cRef &ref, const rdna4n_gem_va *in) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    const VaReq q = gemva_check(in->operation, in->handle, in->_pad, in->flags, in->va_address, in->offset_in_bo, in->map_size,
                                in->vm_timeline_point, in->vm_timeline_syncobj_out, in->num_syncobj_handles, in->input_fence_syncobj_handles);
    if (q.rc != kOk) return (IOReturn)q.rc;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    do {
        const bool prt = (in->flags & RDNA4N_VM_PAGE_PRT) != 0u;
        if (in->operation == RDNA4N_VA_OP_UNMAP && hung_now()) {
            // HUNG: skip the wait and the tables; forget the software mapping (its PTEs are leaked with the BO).
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit >= 0) { s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 }; session_sync_prt(s); }
            break;
        }
        if (in->operation == RDNA4N_VA_OP_CLEAR && hung_now()) {
            if (!va_carve(s->maps, kMaxMaps, q.stripped, in->map_size)) { rc = kIOReturnNoResources; break; }
            session_sync_prt(s);
            break;
        }
        const uint32_t gate = gpu_gate(s);
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        Bo *bp = nullptr;
        if (in->operation != RDNA4N_VA_OP_CLEAR && !prt) {
            if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
            bp = &s->bo[in->handle];
            if (!range_in_bo(in->offset_in_bo, in->map_size, bp->size)) { rc = kIOReturnBadArgument; break; }
        }
        PtMem mem{ s };
        const uint64_t pages = in->map_size / kPage;
        const uint64_t vaCanon = va_canonicalize(q.stripped);
        if (in->operation == RDNA4N_VA_OP_MAP) {
            if (va_overlap(s->maps, kMaxMaps, q.stripped, in->map_size) >= 0) { rc = kIOReturnBadArgument; break; }
            uint32_t slot = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) { rc = kIOReturnNoResources; break; }
            if (!prt && (bp->kind == kBoHost || bp->kind == kBoGtt) &&
                (bp->hpages == nullptr || (in->offset_in_bo / kPage) + pages > bp->hpageCount)) { rc = kIOReturnBadArgument; break; }
            IOLockLock(gCliLock);   // 0.0.627: the page-table reserve is shared
            // 3.1: transactional maps at the canonical form of q.stripped (48-bit high for the
            // high half would otherwise fail pt_va_precheck's strict canonical check; the
            // public gemva_check stays strict). s->maps keeps q.stripped. Quota gates only in
            // alloc_page per new block; upper-bound helpers never gate reuse. Failure returns
            // blocks to the pt reserve, never to vram_alloc.
            uint32_t r = kOk;
            if (prt) {
                r = pt_fill_tx(s->rootPa, vaCanon, pages, gmc_vm_pte_flags(0ull, true), mem);
            } else {
                Bo &b = *bp;
                const bool host = b.kind == kBoHost;                      // 0.0.612: scattered pages (b.hpages), mapped SYSTEM|SNOOPED like a GTT BO
                const bool scattered = host || b.kind == kBoGtt;        // Fix A: kBoGtt also stores non-contiguous pages in b.hpages
                const bool sys = scattered;
                const uint64_t basePa = scattered ? 0ull : pa_of_mc(b.mc) + in->offset_in_bo;
                const uint32_t vmf = host ? host_vm_flags(in->flags, b.hro != 0u) : in->flags;
                r = scattered ? pt_map_pages_tx(s->rootPa, vaCanon, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(vmf, b.uc, sys), mem)
                              : pt_map_tx(s->rootPa, vaCanon, in->map_size / kPage, basePa, leaf_flags(vmf, b.uc, sys, b.dcc), mem, hugepage_on());
            }
            if (r == kNoMemory) {
                amdgpu_hdp_flush(*gCtx->dev);
                const uint32_t f = session_flush_if_bound_locked(s);   // gCliLock prevents reserve reuse until the directory invalidation completes
                if (f != kOk) { mem.quarantine_rollback(); r = f; }
            }
            IOLockUnlock(gCliLock);
            if (r != kOk) { rc = (IOReturn)r; break; }
            s->maps[slot] = VaEnt{ q.stripped, in->map_size, prt ? 0ull : in->offset_in_bo, prt ? 0u : in->handle, prt ? RDNA4N_VM_PAGE_PRT : in->flags };
            if (prt) session_sync_prt(s);
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = session_pt_flush(s);      // synchronous when bound; otherwise recorded in tlbSeq
            if (f != kOk) rc = (IOReturn)f;
        } else if (in->operation == RDNA4N_VA_OP_REPLACE) {
            const VaRangePlan plan = va_range_plan(s->maps, kMaxMaps, q.stripped, in->map_size);
            if (!va_replace_fits(plan)) { rc = kIOReturnNoResources; break; }
            if (!prt && (bp->kind == kBoHost || bp->kind == kBoGtt) &&
                (bp->hpages == nullptr || (in->offset_in_bo / kPage) + pages > bp->hpageCount)) { rc = kIOReturnBadArgument; break; }
            if (plan.overlapping > 0u && !own_wait(s)) { rc = kIOReturnTimeout; break; }
            IOLockLock(gCliLock);
            uint32_t r = kOk;
            if (prt) {
                r = pt_fill_tx(s->rootPa, vaCanon, pages, gmc_vm_pte_flags(0ull, true), mem);
            } else {
                Bo &b = *bp;
                const bool host = b.kind == kBoHost;
                const bool scattered = host || b.kind == kBoGtt;
                const bool sys = scattered;
                const uint64_t basePa = scattered ? 0ull : pa_of_mc(b.mc) + in->offset_in_bo;
                const uint32_t vmf = host ? host_vm_flags(in->flags, b.hro != 0u) : in->flags;
                r = scattered ? pt_replace_pages_tx(s->rootPa, vaCanon, pages, b.hpages + in->offset_in_bo / kPage, leaf_flags(vmf, b.uc, sys), mem)
                              : pt_replace_tx(s->rootPa, vaCanon, pages, basePa, leaf_flags(vmf, b.uc, sys, b.dcc), mem, hugepage_on());
            }
            if (r == kNoMemory) {
                amdgpu_hdp_flush(*gCtx->dev);
                const uint32_t f = session_flush_if_bound_locked(s);
                if (f != kOk) { mem.quarantine_rollback(); r = f; }
            }
            IOLockUnlock(gCliLock);
            if (r != kOk) { rc = (IOReturn)r; break; }
            const VaEnt newEnt{ q.stripped, in->map_size, prt ? 0ull : in->offset_in_bo, prt ? 0u : in->handle, prt ? RDNA4N_VM_PAGE_PRT : in->flags };
            (void)va_replace_apply(s->maps, kMaxMaps, newEnt);
            session_sync_prt(s);
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = session_pt_flush(s);
            if (f != kOk) rc = (IOReturn)f;
        } else if (in->operation == RDNA4N_VA_OP_CLEAR) {
            const VaRangePlan plan = va_range_plan(s->maps, kMaxMaps, q.stripped, in->map_size);
            if (!va_clear_fits(plan)) { rc = kIOReturnNoResources; break; }
            if (plan.overlapping > 0u && !own_wait(s)) { rc = kIOReturnTimeout; break; }
            if (plan.overlapping > 0u) {
                IOLockLock(gCliLock);
                uint32_t r = pt_unmap_tx(s->rootPa, vaCanon, pages, mem);
                if (r == kNoMemory) {
                    amdgpu_hdp_flush(*gCtx->dev);
                    const uint32_t f = session_flush_if_bound_locked(s);
                    if (f != kOk) { mem.quarantine_rollback(); r = f; }
                }
                IOLockUnlock(gCliLock);
                if (r != kOk) { rc = (IOReturn)r; break; }
                (void)va_carve(s->maps, kMaxMaps, q.stripped, in->map_size);
                session_sync_prt(s);
            }
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = session_pt_flush(s);
            if (f != kOk) rc = (IOReturn)f;
        } else {
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit < 0) { rc = kIOReturnBadArgument; break; }
            if (!own_wait(s)) {
                // Not idle: leave the tables alone. This call's wait latched the hang, so it returns Timeout.
                rc = kIOReturnTimeout;
                break;
            }
            // Exact UNMAP matches a whole VaEnt (any partial CLEAR/REPLACE already split boundary 2 MiB leaves),
            // so pt_unmap clears full 2 MiB huge leaves and 4 KiB PTEs with zero PT allocations (no gCliLock).
            pt_unmap(s->rootPa, q.stripped, in->map_size / kPage, mem);
            s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            session_sync_prt(s);
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = session_pt_flush(s);
            if (f != kOk) rc = (IOReturn)f;
        }
    } while (false);
    sess_unlock(s);
    return rc;
}

IOReturn n1c_memory_for_handle(const N1cRef &ref, uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (memory == nullptr) return kIOReturnBadArgument;
    *memory = nullptr;
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (!id_live(s->boUsed, RDNA4N_MAX_BOS - 1, handle)) rc = kIOReturnNotFound;
    else {
        Bo &b = s->bo[handle];
        if (b.kind == kBoHi || b.kind == kBoHost) rc = kIOReturnNotPermitted;   // 0.0.612: a host import is already in the caller's own address space
        else if (b.dcc) rc = kIOReturnNotPermitted;   // GFX12 DCC BOs are never CPU-mapped through raw BAR0
        else if (b.kind == kBoGtt) {
            b.gmd->retain();
            *memory = b.gmd;
        } else {
            // ONE descriptor per BO (0.0.602): IOConnectUnmapMemory64 finds the existing mapping by descriptor identity, so a fresh object per call
            // could never be unmapped. Created on first use, kept in the BO record, retained for each caller, released once at BoFree / close.
            if (b.vmd == nullptr) {
                uint64_t phys = 0;
                if (!gCtx || !gCtx->dev ||
                    !rdna4rebar::cpu_map_vram_ok(b.mc, b.size, gCtx->gmc.vram_start, gCtx->dev->vramBase,
                                                 rdna4rebar::kVisAllocOffset, gCtx->dev->vramLimit, gCtx->dev->bar0Size,
                                                 gCtx->dev->bar0Phys, gCtx->dev->vramSizeBytes, &phys)) {
                    rc = kIOReturnNotPermitted;
                } else {
                    IODeviceMemory *created = IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)b.size);
                    if (created != nullptr && created->getLength() != b.size) { created->release(); created = nullptr; }
                    b.vmd = created;
                }
            }
            if (rc == kIOReturnSuccess) {
                IODeviceMemory *md = b.vmd;
                if (md == nullptr) rc = kIOReturnNoMemory;
                else { md->retain(); *memory = md; if (options) *options |= kIOMapWriteCombineCache; }   // NOTE: XNU's mapClientMemory64 replaces the cache bits with the caller's own mapFlags, so a caller that wants write-combining passes kIOMapWriteCombineCache itself
            }
        }
    }
    sess_unlock(s);
    return rc;
}

} // namespace amdgpu
