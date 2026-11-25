// garnet_submit_internal.hpp — Internal state, types, and cross-unit helpers
// shared across Garnet's native ring submission, session/VM, and VRAM/BO units.
#pragma once

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOUserClient.h>   // 0.0.641: IOUserClient::removeMappingForDescriptor (n1c_unmap_for_close)
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>

#include "native_s1c.h"
#include "rdna4_vmid_pool.h"
#include "rebar_pure.h"
#include "native_hostimport_pure.h"   // 0.0.612 (ABI 1.9): BoImportHost
#include "native_buffer_pure.h"
#include <IOKit/IOMemoryDescriptor.h>
#include "dcn/dcn.hpp"     // build 0.0.603: rdna4dcn::scan* (the scanout path behind selectors 9..14)
#include "dcn/scanout_pure.h"
#include "dcn/scanx_flow.h"   // 0.0.661 (M6 Stage 1b): teardown (the instances restored before any BO is touched); 0.0.662: teardown_all, three instances
#include "native_disp.h"          // 0.0.661: rdna4m6_latched_on / rdna4m6flip_latched_on (the selectors 22..26 latch)
#include "smu_dal.h"          // build 0.0.604: dal_run_step (selector 15, the DISPCLK/DPPCLK experiment)
#include "amdgpu_log.h"
#include "amdgpu_field_defs.h"
#include "amdgpu_gfx.h"
#include "amdgpu_pm4.h"
#include "cp_pm4_gfx12.h"
#include "native_d2pin_pure.h" // 0.0.652 (M5): the monitor B pair's pin / free / alloc decisions (host-tested)
#include "garnet_hang_recovery_pure.h"   // 0.0.623 (G1): hang recovery (boot-arg rdna4-g1=1, default OFF)
#include "garnet_multisession_pure.h"   // 0.0.627 (G2): several sessions (boot-arg garnet-multisession=1, default OFF)
#include "garnet_app_credit_pure.h"   // 0.0.650 (G5 Stage 1): the application credit (boot-arg rdna4-g5=1, default OFF)
#include "amdgpu_sysmem.h"
#include "garnet_app_policy_pure.h"   // 0.0.640 (G4): the APP role, the allow-list, the budget report, the close-path mapping rule (boot-arg garnet-apps=1, default OFF)
#include <pexpert/pexpert.h>  // 0.0.623: PE_parse_boot_argn (rdna4-g1)

#define N1C_LOG(fmt, ...) AMDGPU_LOG("native-s1c", fmt, ##__VA_ARGS__)

namespace amdgpu {

using namespace rdna4native;
using namespace rdna4native::s1c;

// The pure header's codes are the kernel's.
static_assert(kOk == (uint32_t)kIOReturnSuccess && kBadArg == (uint32_t)kIOReturnBadArgument && kNotFound == (uint32_t)kIOReturnNotFound &&
              kUnsupported == (uint32_t)kIOReturnUnsupported && kNoMemory == (uint32_t)kIOReturnNoMemory &&
              kNoResources == (uint32_t)kIOReturnNoResources && kNotReady == (uint32_t)kIOReturnNotReady &&
              kExclusive == (uint32_t)kIOReturnExclusiveAccess && kNotPrivileged == (uint32_t)kIOReturnNotPrivileged &&
              kNotPermitted == (uint32_t)kIOReturnNotPermitted && kTimeout == (uint32_t)kIOReturnTimeout &&
              kAborted == (uint32_t)kIOReturnAborted, "the contract's return codes are the IOReturn.h values");
static_assert(kNativeVmid == 8u, "S1c runs on VMID 8");
static_assert(rdna4native::g2::kBusy == (uint32_t)kIOReturnBusy, "R6's refusal is kIOReturnBusy");
static_assert(rdna4scan::kOk == (uint32_t)kIOReturnSuccess && rdna4scan::kBadArg == (uint32_t)kIOReturnBadArgument && rdna4scan::kNotFound == (uint32_t)kIOReturnNotFound &&
              rdna4scan::kUnsupported == (uint32_t)kIOReturnUnsupported && rdna4scan::kNoResources == (uint32_t)kIOReturnNoResources &&
              rdna4scan::kNotReady == (uint32_t)kIOReturnNotReady && rdna4scan::kBusy == (uint32_t)kIOReturnBusy &&
              rdna4scan::kNotPermitted == (uint32_t)kIOReturnNotPermitted && rdna4scan::kNoDevice == (uint32_t)kIOReturnNoDevice,
              "the scanout pure header's codes are the IOReturn.h values");
static_assert(PTEFlags::PRT_GFX12 == kPtePrt && PTEFlags::PRT_LEAF == kPtePrtLeaf && (PTEFlags::PRT_LEAF & PTEFlags::VALID) == 0ULL,
              "GFX12 PRT PTE encoding matches Linux amdgpu_vm.h/gmc_v12_0.c");

// ---- state --------------------------------------------------------------------------------------------------------------------
enum : uint8_t { kBoNone = 0, kBoVis = 1, kBoHi = 2, kBoGtt = 3, kBoHost = 4 };   // kBoHost: 0.0.612, an import of the caller's memory (hmd / hpages)
struct Bo {
    uint8_t   kind;
    bool      uc;          // created UNCACHED
    uint64_t  size;        // 4 KiB rounded (what the caller asked for)
    uint64_t  mc;          // VRAM MC address (vis / hi)
    VRAMAllocation a;      // vis / hi
    uint8_t   pinMask;     // 0.0.603: bit i = this BO hosts scanout slot i (ScanoutRegister). Set and cleared ONLY under gCliLock; a set bit whose pinGen is older than rdna4dcn::scanGeneration() is STALE (its acquisition ended)
    uint32_t  pinGen;      // the scan generation the pins were recorded under
    uint8_t   pinMask1;    // 0.0.662 (M6 Stage 2): bit k = this BO hosts INSTANCE 1's (the monitor A's) slot 0x10|k (SCANX_REGISTER); under gCliLock; stale when pinGen1 != rdna4dcn::scanXGeneration(1)
    uint32_t  pinGen1;
    uint8_t   pinMask2;    // 0.0.661 (M6 Stage 1b): bit k = this BO hosts INSTANCE 2's slot 0x20|k (SCANX_REGISTER); under gCliLock; stale when pinGen2 != rdna4dcn::scanXGeneration(2)
    uint32_t  pinGen2;
    uint8_t   pinLeak;     // the console restore did NOT verify while this BO was pinned: it may still be scanned, so it is LEAKED (like HUNG), never freed
    uint8_t   budgeted;    // 0.0.627 (G2): this VRAM BO is counted in gVramOthers (a non-WindowServer session, switch ON); taken off when its memory is freed
    uint8_t   sysCounted;  // 0.0.629 (R4): this GTT / host-import BO is counted in gSysHeld (switch ON, any session); taken off only when its memory is really freed
    uint8_t   visCounted;  // 0.0.641 (G4 review): this VISIBLE-pool BO is counted in gVisOthers (a non-WindowServer session, apps ON); taken off when its memory is freed
    IOBufferMemoryDescriptor *gmd;   // gtt: non-contiguous page descriptor (Fix A); page list kept in hpages / hpageCount
    IODeviceMemory *vmd;   // vis: the ONE CPU-map descriptor of this BO (0.0.602), created on first clientMemoryForType, released at BoFree / close, leaked with the BO when HUNG
    IOMemoryDescriptor *hmd;   // host (0.0.612): the prepare()d descriptor of the caller's range; complete() + release() at BoFree / close, LEAKED when HUNG
    uint64_t *hpages;          // host / gtt: the physical page list (heap, hpageCount entries), freed with the descriptor
    uint32_t  hpageCount;
    uint8_t   hro;             // host (b43): 1 = the caller's range was only wired READ (InOut refused: e.g. CoreAnimation's shared IOSurface pages). GPU WRITE leaves are clamped off at map time (host_vm_flags).
};
using rdna4native::g2::kMaxSessions;
constexpr uint32_t kArenaBlocks = (uint32_t)(kPtCap / 16384ull);   // 2048 blocks of 16 KiB (the pool's minimum allocation): one session's page-table cap
constexpr uint32_t kArenaBlockBytes = 16384u;
constexpr uint32_t kArenaReserve = kArenaBlocks * rdna4native::g2::kHwVmids;   // page-table reserve scaled to the hardware VMID budget
static_assert(kArenaReserve <= 0xFFFFu, "page-table block indices fit the uint16_t lists");
struct Session {
    bool      hello;
    uint8_t   boUsed[RDNA4N_MAX_BOS];
    Bo        bo[RDNA4N_MAX_BOS];
    VaEnt     maps[kMaxMaps];
    uint8_t   ctxUsed[RDNA4N_MAX_CTX + 1];
    FenceSlot fslot[RDNA4N_FENCE_SLOTS];
    uint16_t  blk[kArenaBlocks];   // indices into gPt (the per-boot page-table reserve)
    uint32_t  nblk;
    uint32_t  pagesInBlk;      // 4 KiB pages handed out from the newest block (4 = full)
    uint32_t  ptPages;         // table pages handed out (the root included)
    uint64_t  rootPa;
    uint64_t  bufferIbPa;      // protected page in the session's kernel PT arena; recycled only after retirement
    bool      bufferSubmitting; // session lock held; ring_put_locked records this operation before publishing its sequence
    uint64_t  gttUsed;
    uint64_t  vramBytes;       // vis + hi bytes held
    uint64_t  importedBytes;   // 0.0.612: host-import bytes held (cap kImportCap, separate from the GTT cap)
    uint32_t  nBos;
    uint64_t  emitted;         // seqno of THIS session's last Submit (0.0.627: the global count is gEmitted). Written under gRingLock; atomics
    // 0.0.627 (G2): identity. lock / slot / vmid never change once the slot exists; id / open / ws are written under gCliLock at open and close.
    IOLock   *lock;            // THIS session's lock: its tables and counters (the old client lock's job for one session). Allocated with the slot, never freed
    uint32_t  slot, vmid, id;  // id: unique per open (1, 2, 3 ...), the client's N1cRef carries it; 0 = never opened; vmid is pinned (8) or dynamically pooled (9..15, 0 = unbound)
    bool      open, ws;        // ws: admitted by the uid-88 (WindowServer) rule
    bool      pinnedVmid;      // true: pinned to VMID 8 (!multi || WindowServer || G1 test); false: pooled VMID 9..15
    bool      app;             // 0.0.640 (G4): admitted by the allow-list rule (an ordinary user application); set once by n1c_mark_app right after the open, never ReadRegs / scanout
    uint64_t  appKey;          // 0.0.641: the executable-name key (RDNA4AppKey.h) of an APP session, for the strike count; written once with `app`
    volatile UInt32 poisoned;  // 1 once an automatic recovery named this session guilty (switch ON): Submit refused, waits and GPU calls Aborted
    uint64_t  faultAtOpen;     // 0.0.629 (R5): rdna4native::g2::fault_word read (never cleared) at this session's open, for sessstat
    uint64_t  tlbSeq;          // monotonically bumped on PT mutations / unmaps; synced into gVmidPool boundSeq
    uint32_t  prtMaps;         // ABI 1.13: active PRT VA entries in s->maps (refcount)
    uint64_t  prtPages;        // ABI 1.13: active 4 KiB PRT pages in s->maps
};

struct FenceWindow { bool reserved; uint64_t gartOff; uint64_t mc; };
struct ParkRoot { bool have; VRAMAllocation a; uint64_t pa; };
struct G2Stat { uint64_t recoveries, failed, lastGuiltySeq, collateralTotal; uint32_t lastGuiltySlot; };
enum RelMode : uint32_t { kRelNormal = 0, kRelLeak = 1, kRelClosing = 2 };
static_assert(kRelNormal == kHostRelNormal && kRelLeak == kHostRelLeak && kRelClosing == kHostRelClosing, "the pure header's release modes are RelMode");

// Shared globals across garnet_ring_submit.cpp, garnet_session_vm.cpp, and garnet_vram_alloc.cpp.
extern Session *gSlot[kMaxSessions];
extern uint8_t gSlotState[kMaxSessions];
extern VRAMAllocation gPt[kArenaReserve];
extern uint16_t gPtFree[kArenaReserve];
extern uint32_t gPtN, gPtFreeN;
extern uint64_t gWc;
extern bool gWcInit;
extern uint64_t gEmitted;
extern BringupContext *gCtx;
extern volatile UInt32 gOpenFlag;
extern Hang gHang;
// THE LOCK MAP (0.0.627, G2). Order: a session's own lock -> gCliLock -> gRingLock -> gHangLock; gTlbLock is a leaf (nothing is taken under it).
//   Session::lock  one per session: that session's BO / map / ctx / fence-slot tables, its counters and hello. Held for a whole selector body, INCLUDING
//                  its own-seqno waits (own_wait), and NEVER while another session's lock is wanted.
//   gCliLock       the SHARED state, never held across a GPU wait: the slot table (open / close, VMID hand-out, gOpenFlag with the switch ON, gSessWs),
//                  the page-table reserve (gPt / gPtFree, PtMem::alloc_page), the VRAM pools (vram_alloc / vram_alloc_hi alloc and free) and the VRAM
//                  budget, the GART fence windows (reservation and PTE writes), CONTEXTn programming and the park root, the scanout pins and owner.
//                  The G1 block (a kernel-owned test context, exclusive by construction) still uses it as before.
//   gRingLock      the ring: seqno assignment (gEmitted), gSubmitBuf, the ring dwords, gWc, the owner log, a session's `emitted`, the doorbell. The space
//                  wait (2 s) runs under it, as before. An automatic recovery holds it for its whole bounded run (emission frozen).
//   gHangLock      gHang and gRecovering.
//   gTlbLock       gmc_flush_gpu_tlb (one invalidation engine for every VMID; 0.0.626 only ever flushed under the one client lock).
// A GPU-dependent check that may RUN a recovery (gpu_gate, hang_poll, own_wait, stall_from_wait) is called with at most the caller's own session lock:
// never with gCliLock or gRingLock held.
extern IOLock *gCliLock, *gRingLock, *gHangLock, *gTlbLock;
extern uint32_t gSessSeqCounter;
extern volatile UInt32 gSessWs;
extern FenceWindow gWin[kMaxSessions];
extern ParkRoot gPark;
extern rdna4vmid::Pool gVmidPool;
extern bool gVmidPoolInited;
extern bool gVmid8Dead;
extern bool gVmidCtxReady[rdna4vmid::kSlots];
extern thread_t gG1OpenThread;
extern uint64_t gVramOthers;
extern volatile UInt32 gScanOwner;
extern uint64_t gMappedLeakBytes;
extern uint32_t gMappedLeakBos;
extern uint64_t gVisOthers;
extern volatile UInt32 gWsId;
extern uint64_t gWsLastNs;
extern uint64_t gPrtTotalMaps;

// Cross-unit helpers.
IOLock *lazy_lock(IOLock **slot);
uint64_t now_ns();
bool vmid_fence_reached(void *, uint32_t, uint64_t seq);
bool multi_on();
bool apps_on();
uint64_t appvis_cap();
bool g1_holds();
Session *sess_lock(const N1cRef &r, bool needHello);
void session_sync_prt(Session *s);
void hang_from_wait();
uint32_t stall_from_wait();
uint32_t gpu_gate(const Session *s);
bool own_wait(Session *s, bool *noBlame = nullptr);
uint32_t flush_vmid(uint32_t vmid);
uint32_t program_root(uint32_t vmid, uint64_t rootPa);
uint32_t ctx_prepare_vmid(uint32_t vmid, bool *ok);
uint32_t session_flush_if_bound_locked(Session *s);
uint32_t session_pt_flush(Session *s);
bool fence_window_reserve_locked(FenceWindow &win);
uint64_t fault_snap(const char *when, uint32_t slot);
bool scan_unpin(uint32_t h, Bo &b, RelMode mode);
bool scan_unpin1(uint32_t h, Bo &b, RelMode mode);
bool scan_unpin2(uint32_t h, Bo &b, RelMode mode);
void bo_release(Session *s, uint32_t h, RelMode mode);

static inline uint64_t pa_of_mc(uint64_t mc) { return gCtx->gmc.vram_base_offset + mc - gCtx->gmc.vram_start; }
static inline volatile uint64_t *seq_slot() { return reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(gCtx->cp.wb_cpu) + kWbOffsetNativeSeq); }
static inline uint32_t hub_rd(uint32_t off) {
    const HubContext &h = gCtx->gmc.gfxhub;
    return RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, h.ip, h.base_idx, off));
}
static inline uint64_t emitted_now() { return __atomic_load_n(&gEmitted, __ATOMIC_ACQUIRE); }
static inline uint64_t retired_now(uint64_t emitted) {
    sysmem_rmb();
    return retired_clamped(*seq_slot(), emitted);
}
static inline bool hung_now() { return __atomic_load_n(&gHang.hung, __ATOMIC_ACQUIRE); }
static inline void ensure_vmid_pool_locked() {
    if (!gVmidPoolInited) {
        gVmidPool.init(&vmid_fence_reached, nullptr, 0x1FFu);   // reserve 0..8; pool 9..15
        gVmidPoolInited = true;
    }
}
static inline Session *slot_ptr(uint32_t slot) { return slot < kMaxSessions ? __atomic_load_n(&gSlot[slot], __ATOMIC_ACQUIRE) : nullptr; }
// The caller's session without a lock: open and still the SAME session (the id of its open). A Session is never freed, so a stale reference reads a
// closed or re-opened record and is refused.
static inline Session *sess_peek(const N1cRef &r) {
    Session *s = slot_ptr(r.slot);
    if (s == nullptr || r.id == 0u || !__atomic_load_n(&s->open, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->id, __ATOMIC_ACQUIRE) != r.id) return nullptr;
    return s;
}
static inline bool sess_hello(const N1cRef &r) { Session *g = sess_peek(r); return g != nullptr && __atomic_load_n(&g->hello, __ATOMIC_ACQUIRE); }
static inline void sess_unlock(Session *s) { IOLockUnlock(s->lock); }
static inline bool sess_poisoned(const Session *s) { return __atomic_load_n(&s->poisoned, __ATOMIC_SEQ_CST) != 0u; }
// 0.0.640 (G4): defence in depth. The client class already keeps an APP session off ReadRegs, the scanout set and selectors 15..20 (native_open_policy_pure.h
// selector_allowed_for_app); each engine function behind them asks again, so a future mistake in the dispatch cannot open them. True = refuse (NotPrivileged).
static inline bool app_refused(const N1cRef &r, uint32_t call) {
    const Session *g = sess_peek(r);
    return g != nullptr && !rdna4native::g4::app_call_allowed(__atomic_load_n(&g->app, __ATOMIC_ACQUIRE), call);
}

// ---- the page tables ------------------------------------------------------------------------------------------------------------
struct PtSnap { uint32_t nblk; uint32_t pagesInBlk; uint32_t ptPages; };
struct PtMem {
    Session *s;   // 0.0.627: the session whose tree (and page-table cap) this is
    uint32_t rollbackReserveBegin = 0;
    uint64_t rd(uint64_t pa, uint32_t idx) { return RBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u); }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { WBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u, v); }
    // A zeroed 4 KiB table page from the session's arena (16 KiB blocks of the visible pool: the pool's minimum allocation), or 0 at the cap.
    // 0.0.627: the reserve and vram_alloc are shared: the caller holds gCliLock. 3.1: a new reserve
    // block additionally respects the global PT cap (min 64 MiB, visible/4) and true visible free;
    // free-list reuse needs no new visible bytes and bypasses that gate.
    uint64_t alloc_page() {
        if (s->pagesInBlk >= 4u) {
            if (s->nblk >= kArenaBlocks || would_exceed((uint64_t)s->nblk * kArenaBlockBytes, kArenaBlockBytes, kPtCap)) return 0ull;
            uint32_t idx;
            if (gPtFreeN > 0) idx = gPtFree[--gPtFreeN];
            else {
                const uint64_t globalHeld = (uint64_t)gPtN * kArenaBlockBytes;
                const uint64_t visiblePool = gCtx->gmc.vram_alloc.size();
                const uint64_t visibleFree = gCtx->gmc.vram_alloc.bytes_free();
                if (!pt_global_ok(globalHeld, kArenaBlockBytes, visiblePool, visibleFree, rdna4native::g2::kAppKernelReserve)) return 0ull;
                if (gPtN >= kArenaReserve) return 0ull;
                VRAMAllocation a {};
                if (!gCtx->gmc.vram_alloc.alloc(kArenaBlockBytes, kArenaBlockBytes, &a)) return 0ull;
                const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
                if (a.size < kArenaBlockBytes || a.gpu_va < gCtx->gmc.vram_start || off > gCtx->dev->bar0Size || a.size > gCtx->dev->bar0Size - off) { gCtx->gmc.vram_alloc.free(a); return 0ull; }
                gPt[gPtN] = a;
                idx = gPtN++;
            }
            bar0_memset_vram(*gCtx->dev, gPt[idx].gpu_va - gCtx->gmc.vram_start, 0, kArenaBlockBytes);   // a reused block is zeroed again
            s->blk[s->nblk++] = (uint16_t)idx;
            s->pagesInBlk = 0;
        }
        const uint64_t pa = pa_of_mc(gPt[s->blk[s->nblk - 1]].gpu_va) + (uint64_t)s->pagesInBlk * kPage;
        s->pagesInBlk++;
        s->ptPages++;
        return pa;
    }
    PtSnap snapshot() const { return PtSnap{ s->nblk, s->pagesInBlk, s->ptPages }; }
    bool is_new_table(uint64_t pa, const PtSnap &snap) const {
        for (uint32_t i = snap.nblk; i < s->nblk; i++) {
            const uint64_t base = pa_of_mc(gPt[s->blk[i]].gpu_va);
            const uint32_t limit = (i + 1u < s->nblk) ? 4u : s->pagesInBlk;
            if (pa >= base && pa < base + (uint64_t)limit * kPage && ((pa - base) & (kPage - 1ull)) == 0ull) return true;
        }
        if (snap.nblk > 0u && snap.pagesInBlk < 4u) {
            const uint32_t oldBlk = s->blk[snap.nblk - 1u];
            const uint64_t base = pa_of_mc(gPt[oldBlk].gpu_va);
            const uint32_t end = (s->nblk > snap.nblk) ? 4u : s->pagesInBlk;
            for (uint32_t k = snap.pagesInBlk; k < end; k++) if (pa == base + (uint64_t)k * kPage) return true;
        }
        return false;
    }
    // Roll back the directory growth of one attempted range: clear only entries pointing into
    // tables allocated after snap, then return those new blocks to the pt reserve (never to
    // vram_alloc). Caller clears leaves first via pt_unmap over the range (safe: no overlap).
    // gCliLock serialises concurrent MAPs, so no other range can interleave. Caller holds gCliLock.
    // 3.1: also scrub the tail of the old partial block handed out after snap. alloc_page
    // zeroes a whole block on take but hands out partial-block tails without zeroing, so a
    // retry after rollback would otherwise reuse a dirty table page (stale VALID entries).
    // Whole new blocks returned below are zeroed on next take in alloc_page, no scrub needed.
    void rollback(const PtSnap &snap, uint64_t rootPa, uint64_t va, uint64_t pages) {
        for (uint64_t p = 0; p < pages;) {
            const uint64_t v = va + p * kPage;
            uint64_t tbl = rootPa;
            const uint32_t idx[3] = { idx_pdb2(v), idx_pdb1(v), idx_pdb0(v) };
            for (uint32_t lvl = 0; lvl < 3; lvl++) {
                const uint64_t e = rd(tbl, idx[lvl]);
                if (!entry_valid(e) || entry_is_leaf(e)) break;
                if (is_new_table(e & kPtePaMask, snap)) { wr(tbl, idx[lvl], 0ull); break; }
                tbl = e & kPtePaMask;
            }
            const uint64_t run = 512ull - idx_ptb(v);
            p += run < pages - p ? run : pages - p;
        }
        if (snap.nblk > 0u && snap.nblk <= s->nblk && snap.pagesInBlk < 4u) {
            const uint32_t oldBlk = s->blk[snap.nblk - 1u];
            const uint64_t base = pa_of_mc(gPt[oldBlk].gpu_va);
            const uint32_t curEnd = (s->nblk > snap.nblk) ? 4u : s->pagesInBlk;
            for (uint32_t k = snap.pagesInBlk; k < curEnd; k++) {
                const uint64_t pa = base + (uint64_t)k * kPage;
                for (uint32_t e = 0; e < 512u; e++) wr(pa, e, 0ull);
            }
        }
        rollbackReserveBegin = gPtFreeN;
        for (uint32_t i = snap.nblk; i < s->nblk; i++) gPtFree[gPtFreeN++] = s->blk[i];
        s->nblk = snap.nblk;
        s->pagesInBlk = snap.pagesInBlk;
        s->ptPages = snap.ptPages;
    }
    // A failed invalidate leaves possible hardware references. Keep those blocks counted
    // but unreachable from the free list until reboot. Caller still holds gCliLock.
    void quarantine_rollback() { gPtFreeN = rollbackReserveBegin; }
};

// Return a session's page-table blocks to the reserve (never to vram_alloc). Caller holds gCliLock.
void pt_release_blocks(Session *s);

} // namespace amdgpu
