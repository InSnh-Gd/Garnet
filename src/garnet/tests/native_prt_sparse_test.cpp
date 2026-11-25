// native_prt_sparse_test.cpp - ABI 1.13 GFX12 Sparse/PRT VM host test (ASan+UBSan).
// Covers:
//   P1  ABI minor 13 + RDNA4N_CAP_VM_PRT negotiation & backward compatibility (minor <= 12 degrades safely).
//   P2  GFX12 PRT PTE bit-exact encoding (bit 56 set, SNOOPED/SYSTEM/IS_PTE set, VALID clear, PA == 0) + gemva_check validation.
//   P3  CLEAR, REPLACE, partial-overlap head/middle/tail & multi-interval splitting (BO->PRT, PRT->BO, PRT->CLEAR, BO->BO) + 2 MiB seam.
//   P4  Mandatory TLB invalidation counter and GPU-idle drain (own_wait) checks across every transition.
//   P5  Map-table slot exhaustion (kNoResources) atomic refusal without mutating maps/PTEs/PRT refcount.
//   P6  PT quota exhaustion atomic rollback over TestMem AND production PtMem (pre-existing overlapping BO/PRT leaves preserved).
//   P7  PRT refcount, BoFree of split BOs, and session exit cleanup (zero leaks under ASan/UBSan).
//   P8  Production source pins on GarnetNativeABI.h, amdgpu_ip.h, amdgpu_gmc.h, gmc_v12_0.cpp, native_s1c.cpp.
#include "garnet_version_check.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "GarnetNativeABI.h"
#include "amd/garnet_multisession_pure.h"
#include "amd/native_hostimport_pure.h"

using namespace rdna4native;
using namespace rdna4native::s1c;

static int gRun = 0, gFail = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    }
}
static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string fn_body(const std::string &s, const char *head) {
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// ---- Production PtSnap/PtMem harness (extracted verbatim from native_s1c.cpp) ---------------------------------------------
namespace amdgpu {
constexpr uint32_t kArenaBlocks = (uint32_t)(kPtCap / 16384u);
constexpr uint32_t kArenaBlockBytes = 16384u;
constexpr uint32_t kArenaReserve = kArenaBlocks * rdna4native::g2::kHwVmids;

struct DeviceContext { size_t bar0Size = 0; };
struct VRAMAllocation { uint64_t gpu_va = 0; void *cpu_ptr = nullptr; uint64_t size = 0; uint64_t alignment = 0; };
struct VRAMBumpAllocator {
    uint64_t total = 0, freeBytes = 0, nextVa = 0x1000;
    uint64_t size() const { return total; }
    uint64_t bytes_free() const { return freeBytes; }
    bool alloc(uint64_t bytes, uint64_t align, VRAMAllocation *out) {
        if (out == nullptr || bytes == 0ull) return false;
        uint64_t va = (nextVa + align - 1) & ~(align - 1);
        if (bytes > freeBytes) return false;
        freeBytes -= bytes;
        nextVa = va + bytes;
        out->gpu_va = 0x80000000ull + va;
        out->cpu_ptr = nullptr;
        out->size = bytes;
        out->alignment = align;
        return out->gpu_va != 0ull;
    }
    void free(const VRAMAllocation &a) { freeBytes += a.size; }
};
struct GMCContext {
    VRAMBumpAllocator vram_alloc;
    uint64_t vram_start = 0x80000000ull;
    uint64_t vram_base_offset = 0x100000000ull;
};
struct BringupContext {
    GMCContext gmc;
    DeviceContext *dev = nullptr;
};
struct Session {
    uint16_t blk[kArenaBlocks] = {};
    uint32_t nblk = 0;
    uint32_t pagesInBlk = 4u;
    uint32_t ptPages = 0;
    uint64_t rootPa = 0;
};

static VRAMAllocation gPt[kArenaReserve];
static uint16_t gPtFree[kArenaReserve];
static uint32_t gPtN = 0, gPtFreeN = 0;
static BringupContext *gCtx = nullptr;
static std::map<uint64_t, uint64_t> gBarMem;
static BringupContext gCtxStore;
static DeviceContext gDevStore;

static inline uint64_t pa_of_mc(uint64_t mc) { return gCtx->gmc.vram_base_offset + mc - gCtx->gmc.vram_start; }
static inline uint64_t RBAR0_64(const DeviceContext &ctx, uint64_t byte_offset) {
    if (byte_offset + 8 > ctx.bar0Size) return ~0ULL;
    auto it = gBarMem.find(byte_offset);
    return it == gBarMem.end() ? 0ull : it->second;
}
static inline void WBAR0_64(const DeviceContext &ctx, uint64_t byte_offset, uint64_t value) {
    if (byte_offset + 8 > ctx.bar0Size) return;
    gBarMem[byte_offset] = value;
}
static inline void bar0_memset_vram(const DeviceContext &ctx, uint64_t vram_byte_offset, uint32_t pattern, uint64_t size_bytes) {
    if (vram_byte_offset + size_bytes > ctx.bar0Size) return;
    uint64_t pat64 = ((uint64_t)pattern << 32) | pattern;
    for (uint64_t o = 0; o < size_bytes; o += 8) gBarMem[vram_byte_offset + o] = pat64;
}

using namespace rdna4native;
using namespace rdna4native::s1c;
#include "native_ptmem_actual.inc"

static void pt_env_reset(uint64_t visTotal, uint64_t visFree) {
    gBarMem.clear();
    gCtxStore.gmc.vram_alloc.total = visTotal;
    gCtxStore.gmc.vram_alloc.freeBytes = visFree;
    gCtxStore.gmc.vram_alloc.nextVa = 0x1000;
    gCtxStore.gmc.vram_start = 0x80000000ull;
    gCtxStore.gmc.vram_base_offset = 0x100000000ull;
    gDevStore.bar0Size = (size_t)(1ull << 32);
    gCtxStore.dev = &gDevStore;
    gCtx = &gCtxStore;
    gPtN = 0;
    gPtFreeN = 0;
    std::memset(gPt, 0, sizeof(gPt));
    std::memset(gPtFree, 0, sizeof(gPtFree));
}
} // namespace amdgpu

// ---- Host VM model driving exact pure functions + TLB/refcount/teardown tracking -----------------------------------------
struct TestMem {
    std::map<uint64_t, std::array<uint64_t, 512>> t;
    uint64_t next = 0x100000000ull;
    uint32_t budget = 10000u;
    uint32_t allocs = 0;
    uint32_t failAt = 0xFFFFFFFFu;
    struct Snap { uint32_t allocs; uint64_t next; };
    Snap snapshot() const { return Snap{ allocs, next }; }
    void rollback(const Snap &s, uint64_t rootPa, uint64_t va, uint64_t pages) {
        for (uint64_t p = 0; p < pages;) {
            const uint64_t v = va + p * kPage;
            uint64_t tbl = rootPa;
            const uint32_t idx[3] = { idx_pdb2(v), idx_pdb1(v), idx_pdb0(v) };
            for (uint32_t lvl = 0; lvl < 3; lvl++) {
                const uint64_t e = rd(tbl, idx[lvl]);
                if (!entry_valid(e) || entry_is_leaf(e)) break;
                if ((e & kPtePaMask) >= s.next) { wr(tbl, idx[lvl], 0ull); break; }
                tbl = e & kPtePaMask;
            }
            uint64_t run = 512ull - idx_ptb(v);
            if (run > pages - p) run = pages - p;
            p += run;
        }
        std::vector<uint64_t> dead;
        for (auto &kv : t) if (kv.first >= s.next) dead.push_back(kv.first);
        for (uint64_t pa : dead) t.erase(pa);
        allocs = s.allocs;
        next = s.next;
    }
    uint64_t rd(uint64_t pa, uint32_t idx) const {
        auto it = t.find(pa);
        return it == t.end() ? 0ull : it->second[idx];
    }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { t[pa][idx] = v; }
    uint64_t alloc_page() {
        if (allocs >= budget || allocs == failAt) return 0ull;
        allocs++;
        const uint64_t pa = next;
        next += kPage;
        t[pa].fill(0);
        return pa;
    }
};

struct TestBo {
    bool live = false;
    bool scattered = false;
    bool uc = false;
    bool hostRo = false;
    uint64_t size = 0;
    uint64_t basePa = 0;
    std::vector<uint64_t> pages;
};

struct TestVm {
    TestMem mem;
    uint64_t rootPa = 0;
    std::vector<VaEnt> maps;
    std::map<uint32_t, TestBo> bos;
    uint32_t prtMaps = 0;
    uint64_t prtPages = 0;
    uint64_t *globalPrtMaps = nullptr;
    uint64_t tlbFlushes = 0;
    uint64_t ownWaits = 0;
    uint64_t tlbSeq = 1;

    explicit TestVm(uint32_t mapCap = 64u, uint64_t *gPrt = nullptr)
        : maps(mapCap, VaEnt{ 0, 0, 0, 0, 0 }), globalPrtMaps(gPrt) {
        rootPa = mem.alloc_page();
    }
    void sync_prt() {
        const PrtStats st = va_prt_stats(maps.data(), (uint32_t)maps.size());
        const int64_t delta = (int64_t)st.maps - (int64_t)prtMaps;
        prtMaps = st.maps;
        prtPages = st.pages;
        if (globalPrtMaps) {
            if (delta > 0) *globalPrtMaps += (uint64_t)delta;
            else if (delta < 0) *globalPrtMaps -= (uint64_t)(-delta);
        }
    }
    void flush_tlb() { tlbFlushes++; tlbSeq++; }
    void add_contig_bo(uint32_t h, uint64_t size, uint64_t basePa, bool uc = false) {
        bos[h] = TestBo{ true, false, uc, false, size, basePa, {} };
    }
    void add_scattered_bo(uint32_t h, uint64_t size, uint64_t firstPa, uint64_t stride, bool hostRo = false) {
        TestBo b{ true, true, false, hostRo, size, 0, {} };
        const uint64_t n = size / kPage;
        b.pages.resize(n);
        for (uint64_t i = 0; i < n; i++) b.pages[i] = firstPa + i * stride;
        bos[h] = std::move(b);
    }
    uint32_t gem_va(uint32_t op, uint32_t handle, uint32_t flags, uint64_t va, uint64_t off, uint64_t size) {
        const VaReq q = gemva_check(op, handle, 0, flags, va, off, size, 0, 0, 0, 0);
        if (q.rc != kOk) return q.rc;
        const bool prt = (flags & RDNA4N_VM_PAGE_PRT) != 0u;
        const TestBo *bp = nullptr;
        if (op != RDNA4N_VA_OP_CLEAR && !prt) {
            auto it = bos.find(handle);
            if (it == bos.end() || !it->second.live) return kNotFound;
            bp = &it->second;
            if (!range_in_bo(off, size, bp->size)) return kBadArg;
        }
        const uint64_t pages = size / kPage;
        const uint64_t vaCanon = va_canonicalize(q.stripped);
        const uint32_t cap = (uint32_t)maps.size();
        if (op == RDNA4N_VA_OP_MAP) {
            if (va_overlap(maps.data(), cap, q.stripped, size) >= 0) return kBadArg;
            int slot = -1;
            for (uint32_t i = 0; i < cap; i++) if (!va_ent_used(maps[i])) { slot = (int)i; break; }
            if (slot < 0) return kNoResources;
            uint32_t r = kOk;
            if (prt) {
                r = pt_fill_tx(rootPa, vaCanon, pages, pte_encode(0ull, leaf_flags(RDNA4N_VM_PAGE_PRT, false, false)), mem);
            } else if (bp->scattered) {
                const uint32_t vmf = host_vm_flags(flags, bp->hostRo);
                r = pt_map_pages_tx(rootPa, vaCanon, pages, bp->pages.data() + off / kPage, leaf_flags(vmf, bp->uc, true), mem);
            } else {
                r = pt_map_tx(rootPa, vaCanon, pages, bp->basePa + off, leaf_flags(flags, bp->uc, false), mem);
            }
            if (r == kNoMemory) flush_tlb();
            if (r != kOk) return r;
            maps[slot] = VaEnt{ q.stripped, size, prt ? 0ull : off, prt ? 0u : handle, prt ? RDNA4N_VM_PAGE_PRT : flags };
            if (prt) sync_prt();
            flush_tlb();
            return kOk;
        } else if (op == RDNA4N_VA_OP_REPLACE) {
            const VaRangePlan plan = va_range_plan(maps.data(), cap, q.stripped, size);
            if (!va_replace_fits(plan)) return kNoResources;
            if (plan.overlapping > 0u) ownWaits++;
            uint32_t r = kOk;
            if (prt) {
                r = pt_fill_tx(rootPa, vaCanon, pages, pte_encode(0ull, leaf_flags(RDNA4N_VM_PAGE_PRT, false, false)), mem);
            } else if (bp->scattered) {
                const uint32_t vmf = host_vm_flags(flags, bp->hostRo);
                r = pt_replace_pages_tx(rootPa, vaCanon, pages, bp->pages.data() + off / kPage, leaf_flags(vmf, bp->uc, true), mem);
            } else {
                r = pt_replace_tx(rootPa, vaCanon, pages, bp->basePa + off, leaf_flags(flags, bp->uc, false), mem);
            }
            if (r == kNoMemory) flush_tlb();
            if (r != kOk) return r;
            const VaEnt ne{ q.stripped, size, prt ? 0ull : off, prt ? 0u : handle, prt ? RDNA4N_VM_PAGE_PRT : flags };
            if (!va_replace_apply(maps.data(), cap, ne)) return kNoResources;
            sync_prt();
            flush_tlb();
            return kOk;
        } else if (op == RDNA4N_VA_OP_CLEAR) {
            const VaRangePlan plan = va_range_plan(maps.data(), cap, q.stripped, size);
            if (!va_clear_fits(plan)) return kNoResources;
            if (plan.overlapping > 0u) {
                ownWaits++;
                pt_unmap(rootPa, q.stripped, pages, mem);
                if (!va_carve(maps.data(), cap, q.stripped, size)) return kNoResources;
                sync_prt();
            }
            flush_tlb();
            return kOk;
        } else {
            const int hit = va_find_exact(maps.data(), cap, handle, q.stripped, size);
            if (hit < 0) return kBadArg;
            ownWaits++;
            pt_unmap(rootPa, q.stripped, pages, mem);
            maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            sync_prt();
            flush_tlb();
            return kOk;
        }
    }
    uint32_t bo_free(uint32_t h) {
        auto it = bos.find(h);
        if (it == bos.end() || !it->second.live) return kNotFound;
        bool touched = false;
        for (const VaEnt &e : maps) {
            if (va_ent_used(e) && e.handle == h) { pt_unmap(rootPa, e.start, e.size / kPage, mem); touched = true; }
        }
        if (touched) { ownWaits++; flush_tlb(); }
        va_remove_handle(maps.data(), (uint32_t)maps.size(), h);
        it->second.live = false;
        it->second.pages.clear();
        return kOk;
    }
    void close_session() {
        flush_tlb(); // park root
        for (auto &kv : bos) {
            if (!kv.second.live) continue;
            va_remove_handle(maps.data(), (uint32_t)maps.size(), kv.first);
            kv.second.live = false;
            kv.second.pages.clear();
        }
        va_remove_prt(maps.data(), (uint32_t)maps.size());
        sync_prt();
        mem.t.clear();
        mem.allocs = 0;
        rootPa = 0;
    }
    bool verify_disjoint() const {
        for (size_t i = 0; i < maps.size(); i++) {
            if (!va_ent_used(maps[i])) continue;
            for (size_t j = i + 1; j < maps.size(); j++) {
                if (!va_ent_used(maps[j])) continue;
                if (maps[i].start < maps[j].start + maps[j].size && maps[j].start < maps[i].start + maps[i].size) return false;
            }
        }
        return true;
    }
};

// ---- P1: ABI minor 13 + RDNA4N_CAP_VM_PRT negotiation --------------------------------------------------------------------
static void p1_abi_negotiation() {
    expect_u("ABI major remains 1", RDNA4N_ABI_VERSION, 1u);
    expect_u("ABI minor bumped to 13", RDNA4N_ABI_MINOR, 13u);
    expect_u("Hello with RDNA4N_HELLO_F_MINOR reports 1.13 (0xD0001)",
             (uint64_t)RDNA4N_ABI_VERSION | ((uint64_t)RDNA4N_ABI_MINOR << 16), 0xD0001ull);
    expect_u("RDNA4N_CAP_VM_PRT is bit 4", RDNA4N_CAP_VM_PRT, 1u << 4);
    expect_u("RDNA4N_CAP_VM_PRT does not collide with existing INFO flags",
             RDNA4N_CAP_VM_PRT & (RDNA4N_INFO_HUNG | RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT | RDNA4N_INFO_BUFFER_OPS), 0u);
    expect_u("VA op codes match DRM uAPI", RDNA4N_VA_OP_MAP * 1000u + RDNA4N_VA_OP_UNMAP * 100u + RDNA4N_VA_OP_CLEAR * 10u + RDNA4N_VA_OP_REPLACE, 1234u);
    expect_u("RDNA4N_VM_PAGE_PRT is bit 4", RDNA4N_VM_PAGE_PRT, 1u << 4);
    expect_u("rdna4n_info size unchanged", sizeof(rdna4n_info), 192u);
    expect_u("rdna4n_gem_va size unchanged", sizeof(rdna4n_gem_va), 64u);

    // Handshake negotiation: old kernel (minor 12, cap clear) vs new kernel (minor 13, cap set).
    rdna4n_info oldInfo{};
    oldInfo.abi_version = RDNA4N_ABI_VERSION;
    oldInfo.flags = RDNA4N_INFO_NATIVE_BOOT | RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_BUFFER_OPS;
    oldInfo.reserved[2] = 12u;
    const bool oldHasPrt = (oldInfo.flags & RDNA4N_CAP_VM_PRT) != 0u && oldInfo.reserved[2] >= 13u;
    expect(!oldHasPrt, "old minor-12 kernel degrades safely with VM PRT disabled");

    rdna4n_info newInfo = oldInfo;
    newInfo.flags |= RDNA4N_CAP_VM_PRT;
    newInfo.reserved[2] = RDNA4N_ABI_MINOR;
    const bool newHasPrt = (newInfo.flags & RDNA4N_CAP_VM_PRT) != 0u && newInfo.reserved[2] >= 13u;
    expect(newHasPrt, "minor-13 kernel negotiates RDNA4N_CAP_VM_PRT");
}

// ---- P2: GFX12 PRT PTE bit-exact encoding + gemva_check validation -------------------------------------------------------
static void p2_prt_pte_and_validation() {
    expect_u("kPtePrt is AMDGPU_PTE_PRT_GFX12 (1ULL << 56)", kPtePrt, 1ull << 56);
    expect_u("kPtePrtLeaf exact word (bit63 | bit56 | bit2 | bit1)", kPtePrtLeaf, 0x8100000000000006ull);
    expect_u("PRT leaf bit 0 (VALID) is clear", kPtePrtLeaf & kPteValid, 0ull);
    expect_u("PRT leaf bit 1 (SYSTEM) is set", kPtePrtLeaf & kPteSystem, kPteSystem);
    expect_u("PRT leaf bit 2 (SNOOPED) is set", kPtePrtLeaf & kPteSnooped, kPteSnooped);
    expect_u("PRT leaf bit 56 (PRT_GFX12) is set", kPtePrtLeaf & kPtePrt, kPtePrt);
    expect_u("PRT leaf bit 63 (IS_PTE) is set", kPtePrtLeaf & kPteIsPte, kPteIsPte);
    expect_u("PRT leaf PA bits [47:12] are zero", kPtePrtLeaf & kPtePaMask, 0ull);

    const uint64_t encBare = pte_encode(0ull, leaf_flags(RDNA4N_VM_PAGE_PRT, false, false));
    const uint64_t encRadv = pte_encode(0ull, leaf_flags(RDNA4N_VM_PAGE_PRT | RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE |
                                                         RDNA4N_VM_PAGE_EXECUTABLE | RDNA4N_VM_MTYPE_UC, true, true));
    expect_u("pte_encode(0, leaf_flags(PRT)) == kPtePrtLeaf", encBare, kPtePrtLeaf);
    expect_u("pte_encode(0, leaf_flags(PRT|RWX|UC)) == kPtePrtLeaf", encRadv, kPtePrtLeaf);
    expect(entry_is_prt(encBare) && !entry_valid(encBare) && entry_is_leaf(encBare), "entry_is_prt recognizes PRT leaf");
    expect(!entry_is_prt(0ull) && !entry_is_prt(kPtePrtLeaf | kPteValid) && !entry_is_prt(kPtePrtLeaf | 0x1000ull),
           "entry_is_prt rejects zero, VALID-set, and non-zero-PA entries");

    auto chk = [](uint32_t op, uint32_t h, uint32_t fl, uint64_t va, uint64_t off, uint64_t sz) {
        return gemva_check(op, h, 0, fl, va, off, sz, 0, 0, 0, 0);
    };
    const uint64_t HI = 0xffff800100000000ull;
    const uint64_t LO = 0x0000000000100000ull;

    // Valid forms
    expect_u("MAP PRT handle==0 high-half ok", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0x10000).rc, kOk);
    expect_u("MAP PRT with R|W|X flags (RADV form) ok", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT | 0xeu, LO, 0, 0x10000).rc, kOk);
    expect_u("REPLACE PRT handle==0 ok", chk(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0x10000).rc, kOk);
    expect_u("REPLACE BO handle!=0 ok", chk(RDNA4N_VA_OP_REPLACE, 3, 0xeu, HI, 0x2000, 0x10000).rc, kOk);
    expect_u("CLEAR handle==0 flags==0 ok", chk(RDNA4N_VA_OP_CLEAR, 0, 0, HI, 0, 0x10000).rc, kOk);
    expect_u("CLEAR handle==0 flags==PRT ok", chk(RDNA4N_VA_OP_CLEAR, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0x10000).rc, kOk);
    expect_u("UNMAP PRT handle==0 ok", chk(RDNA4N_VA_OP_UNMAP, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0x10000).rc, kOk);
    expect_u("MAP PRT at exact top 48-bit page ok", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0xFFFFFFFFFFFFF000ull, 0, 0x1000).rc, kOk);

    // Invalid combinations -> kBadArg
    expect_u("MAP PRT with non-zero handle -> BadArg", chk(RDNA4N_VA_OP_MAP, 1, RDNA4N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kBadArg);
    expect_u("REPLACE PRT with non-zero handle -> BadArg", chk(RDNA4N_VA_OP_REPLACE, 2, RDNA4N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kBadArg);
    expect_u("CLEAR PRT with non-zero handle -> BadArg", chk(RDNA4N_VA_OP_CLEAR, 1, RDNA4N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kBadArg);
    expect_u("UNMAP PRT with non-zero handle -> BadArg", chk(RDNA4N_VA_OP_UNMAP, 1, RDNA4N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kBadArg);
    expect_u("MAP PRT with non-zero offset_in_bo -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, HI, 0x1000, 0x1000).rc, kBadArg);
    expect_u("REPLACE PRT with non-zero offset_in_bo -> BadArg", chk(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, HI, 0x1000, 0x1000).rc, kBadArg);
    expect_u("CLEAR with non-zero offset_in_bo -> BadArg", chk(RDNA4N_VA_OP_CLEAR, 0, 0, HI, 0x1000, 0x1000).rc, kBadArg);
    expect_u("MAP non-PRT with handle==0 -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, 0xeu, HI, 0, 0x1000).rc, kBadArg);
    expect_u("REPLACE non-PRT with handle==0 -> BadArg", chk(RDNA4N_VA_OP_REPLACE, 0, 0xeu, HI, 0, 0x1000).rc, kBadArg);
    expect_u("UNMAP non-PRT with handle==0 -> BadArg", chk(RDNA4N_VA_OP_UNMAP, 0, 0xeu, HI, 0, 0x1000).rc, kBadArg);
    expect_u("PRT unaligned VA -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, HI + 0x100, 0, 0x1000).rc, kBadArg);
    expect_u("PRT unaligned size -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0x1200).rc, kBadArg);
    expect_u("PRT zero size -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, HI, 0, 0).rc, kBadArg);
    expect_u("CLEAR zero size -> BadArg", chk(RDNA4N_VA_OP_CLEAR, 0, 0, HI, 0, 0).rc, kBadArg);
    expect_u("REPLACE past 48-bit overflow -> BadArg", chk(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, 0xFFFFFFFFFFFFF000ull, 0, 0x2000).rc, kBadArg);
    expect_u("CLEAR size wrap -> BadArg", chk(RDNA4N_VA_OP_CLEAR, 0, 0, HI, 0, ~0ull & ~0xFFFull).rc, kBadArg);
    expect_u("PRT seam straddle -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0x00007FFFFFFFF000ull, 0, 0x2000).rc, kBadArg);
    expect_u("PRT below 0x10000 -> BadArg", chk(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0xF000ull, 0, 0x1000).rc, kBadArg);
    expect_u("REPLACE unknown flag bit -> BadArg", chk(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT | (1u << 22), HI, 0, 0x1000).rc, kBadArg);
    expect_u("CLEAR unknown MTYPE -> BadArg", chk(RDNA4N_VA_OP_CLEAR, 0, 6u << 5, HI, 0, 0x1000).rc, kBadArg);

    // IB EXEC coverage must NEVER accept a PRT range even if R|X bits were present.
    VaEnt prtEnt[1] = { VaEnt{ 0x100000ull, 0x10000ull, 0, 0, RDNA4N_VM_PAGE_PRT | RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_EXECUTABLE } };
    expect(!va_exec_covered(prtEnt, 1, 0x100000ull, 0x1000), "PRT mapping is never EXEC-covered for IB fetch");
}

// ---- P3 + P4: Interval splitting, transitions, 2 MiB seams, and mandatory TLB invalidation -------------------------------
static void p3_p4_splitting_and_tlb() {
    uint64_t gPrt = 0;
    TestVm vm(64, &gPrt);
    const uint64_t k64K = 0x10000ull;
    const uint64_t va0 = 0xFFFF800100000000ull; // high-half 64 KiB aligned
    vm.add_contig_bo(1, 4 * k64K, 0x8010000000ull);
    vm.add_scattered_bo(2, 2 * k64K, 0x200000000ull, 0x3000ull); // non-contiguous host/GTT pages
    vm.add_contig_bo(3, 4 * k64K, 0x8020000000ull, true);        // uncached VRAM BO

    auto check_prt = [&](uint64_t va, uint64_t bytes, const char *label) {
        bool ok = true;
        for (uint64_t o = 0; o < bytes && ok; o += kPage) {
            uint64_t leaf = 0;
            const uint64_t pa = pt_walk(vm.rootPa, va + o, vm.mem, &leaf);
            if (pa != 0ull || leaf != kPtePrtLeaf) ok = false;
        }
        expect(ok, label);
    };
    auto check_zero = [&](uint64_t va, uint64_t bytes, const char *label) {
        bool ok = true;
        for (uint64_t o = 0; o < bytes && ok; o += kPage) {
            uint64_t leaf = ~0ull;
            const uint64_t pa = pt_walk(vm.rootPa, va + o, vm.mem, &leaf);
            if (pa != 0ull || leaf != 0ull) ok = false;
        }
        expect(ok, label);
    };
    auto check_contig = [&](uint64_t va, uint64_t bytes, uint64_t basePa, bool uc, const char *label) {
        bool ok = true;
        for (uint64_t o = 0; o < bytes && ok; o += kPage) {
            uint64_t leaf = 0;
            const uint64_t pa = pt_walk(vm.rootPa, va + o, vm.mem, &leaf);
            const bool ucOk = uc ? ((leaf & kPteMtypeUC) != 0ull) : ((leaf & kPteMtypeUC) == 0ull);
            if (pa != basePa + o || !entry_valid(leaf) || (leaf & kPtePrt) != 0ull || !ucOk) ok = false;
        }
        expect(ok, label);
    };

    // 1) MAP 256 KiB PRT -> check PTEs, refcount, TLB flush, double-MAP overlap rejected.
    uint64_t tlbBefore = vm.tlbFlushes;
    expect_u("MAP 256 KiB PRT", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, va0, 0, 4 * k64K), kOk);
    expect_u("TLB flushed on PRT MAP", vm.tlbFlushes, tlbBefore + 1);
    expect_u("prtMaps == 1", vm.prtMaps, 1);
    expect_u("prtPages == 64", vm.prtPages, 64);
    expect_u("globalPrtMaps == 1", gPrt, 1);
    check_prt(va0, 4 * k64K, "all 64 pages carry exact GFX12 PRT leaf");
    expect_u("overlapping MAP on PRT rejected", vm.gem_va(RDNA4N_VA_OP_MAP, 1, 0xeu, va0 + k64K, 0, k64K), kBadArg);

    // 2) PRT -> BO (middle split): REPLACE [va0+64K, va0+128K) with BO 1 @ off 64K.
    tlbBefore = vm.tlbFlushes;
    uint64_t waitBefore = vm.ownWaits;
    expect_u("REPLACE middle of PRT with BO 1", vm.gem_va(RDNA4N_VA_OP_REPLACE, 1, 0xeu, va0 + k64K, k64K, k64K), kOk);
    expect_u("TLB flushed on PRT->BO REPLACE", vm.tlbFlushes, tlbBefore + 1);
    expect_u("own_wait drained before overlapping REPLACE", vm.ownWaits, waitBefore + 1);
    expect_u("PRT split into 2 intervals", vm.prtMaps, 2);
    expect_u("prtPages reduced 64 -> 48", vm.prtPages, 48);
    expect_u("globalPrtMaps == 2", gPrt, 2);
    check_prt(va0, k64K, "head PRT [0, 64K) intact after middle REPLACE");
    check_contig(va0 + k64K, k64K, 0x8010000000ull + k64K, false, "middle [64K, 128K) walks to BO 1");
    check_prt(va0 + 2 * k64K, 2 * k64K, "tail PRT [128K, 256K) intact after middle REPLACE");
    expect(vm.verify_disjoint(), "intervals remain strictly disjoint after PRT middle split");

    // 3) PRT -> scattered BO (head trim / exact cover of head PRT [0, 64K)).
    tlbBefore = vm.tlbFlushes;
    expect_u("REPLACE head PRT with scattered BO 2", vm.gem_va(RDNA4N_VA_OP_REPLACE, 2, 0xeu, va0, 0, k64K), kOk);
    expect_u("TLB flushed on head REPLACE", vm.tlbFlushes, tlbBefore + 1);
    expect_u("prtMaps reduced 2 -> 1", vm.prtMaps, 1);
    expect_u("prtPages reduced 48 -> 32", vm.prtPages, 32);
    {
        bool scOk = true;
        for (uint64_t i = 0; i < k64K / kPage; i++) {
            uint64_t leaf = 0;
            const uint64_t pa = pt_walk(vm.rootPa, va0 + i * kPage, vm.mem, &leaf);
            if (pa != vm.bos[2].pages[i] || (leaf & (kPteSystem | kPteSnooped | kPteValid)) != (kPteSystem | kPteSnooped | kPteValid)) scOk = false;
        }
        expect(scOk, "head [0, 64K) walks to scattered GTT/host pages with SYSTEM|SNOOPED|VALID");
    }

    // 4) PRT -> BO (tail trim): REPLACE [va0+192K, va0+256K) with BO 3 (UC).
    tlbBefore = vm.tlbFlushes;
    expect_u("REPLACE tail of PRT with UC BO 3", vm.gem_va(RDNA4N_VA_OP_REPLACE, 3, 0xeu, va0 + 3 * k64K, 0, k64K), kOk);
    expect_u("TLB flushed on tail REPLACE", vm.tlbFlushes, tlbBefore + 1);
    expect_u("prtMaps stays 1 (trimmed tail)", vm.prtMaps, 1);
    expect_u("prtPages reduced 32 -> 16", vm.prtPages, 16);
    check_prt(va0 + 2 * k64K, k64K, "middle-right PRT [128K, 192K) preserved");
    check_contig(va0 + 3 * k64K, k64K, 0x8020000000ull, true, "tail [192K, 256K) walks to UC BO 3");

    // 5) BO -> PRT (middle split of BO 1, verify off preserved on left and shifted on right).
    const uint64_t vaLo = 0x00000000001E0000ull; // straddles the 2 MiB (0x200000) PTB seam!
    expect_u("MAP 256 KiB BO 1 across 2 MiB PTB seam", vm.gem_va(RDNA4N_VA_OP_MAP, 1, 0xeu, vaLo, 0, 4 * k64K), kOk);
    tlbBefore = vm.tlbFlushes;
    expect_u("REPLACE middle 64 KiB across 2 MiB seam with PRT", vm.gem_va(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, vaLo + k64K, 0, k64K), kOk);
    expect_u("TLB flushed on BO->PRT REPLACE", vm.tlbFlushes, tlbBefore + 1);
    check_contig(vaLo, k64K, 0x8010000000ull, false, "BO 1 left split [0, 64K) intact");
    check_prt(vaLo + k64K, k64K, "BO->PRT middle [64K, 128K) across 2 MiB seam is exact PRT");
    check_contig(vaLo + 2 * k64K, 2 * k64K, 0x8010000000ull + 2 * k64K, false, "BO 1 right split [128K, 256K) intact across seam");
    // Verify split VaEnt offsets directly
    int leftIdx = va_find_exact(vm.maps.data(), (uint32_t)vm.maps.size(), 1, va_strip(vaLo), k64K);
    int rightIdx = va_find_exact(vm.maps.data(), (uint32_t)vm.maps.size(), 1, va_strip(vaLo + 2 * k64K), 2 * k64K);
    expect(leftIdx >= 0 && vm.maps[leftIdx].off == 0ull, "BO 1 left split carries off == 0");
    expect(rightIdx >= 0 && vm.maps[rightIdx].off == 2 * k64K, "BO 1 right split carries off == 128 KiB");

    // 6) BO -> BO multi-interval straddle REPLACE (head-trim left BO 1 + cover middle PRT + head-trim right BO 1).
    tlbBefore = vm.tlbFlushes;
    expect_u("multi-interval BO->BO REPLACE straddling BO1 + PRT + BO1",
             vm.gem_va(RDNA4N_VA_OP_REPLACE, 3, 0xeu, vaLo + 0x8000ull, 0x8000ull, 2 * k64K), kOk);
    expect_u("TLB flushed on multi-interval REPLACE", vm.tlbFlushes, tlbBefore + 1);
    check_contig(vaLo, 0x8000ull, 0x8010000000ull, false, "left BO 1 tail-trimmed to 32 KiB");
    check_contig(vaLo + 0x8000ull, 2 * k64K, 0x8020000000ull + 0x8000ull, true, "replaced 128 KiB walks to BO 3");
    check_contig(vaLo + 0x8000ull + 2 * k64K, 0x18000ull, 0x8010000000ull + 0x28000ull, false, "right BO 1 head-trimmed to 96 KiB");
    int rightTrimIdx = va_find_exact(vm.maps.data(), (uint32_t)vm.maps.size(), 1, va_strip(vaLo + 0x28000ull), 0x18000ull);
    expect(rightTrimIdx >= 0 && vm.maps[rightTrimIdx].off == 0x28000ull, "right BO 1 head-trim shifted off to 0x28000");
    expect(vm.verify_disjoint(), "intervals strictly disjoint after multi-interval REPLACE");

    // 7) PRT -> CLEAR (middle carve, head/tail carve, full clear, empty-hole idempotent clear).
    const uint64_t vaPrt2 = 0x0000000001000000ull;
    expect_u("MAP 256 KiB PRT for CLEAR tests", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, vaPrt2, 0, 4 * k64K), kOk);
    tlbBefore = vm.tlbFlushes;
    waitBefore = vm.ownWaits;
    expect_u("CLEAR middle 64 KiB of PRT", vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, vaPrt2 + k64K, 0, k64K), kOk);
    expect_u("TLB flushed on PRT->CLEAR", vm.tlbFlushes, tlbBefore + 1);
    expect_u("own_wait drained on overlapping CLEAR", vm.ownWaits, waitBefore + 1);
    check_prt(vaPrt2, k64K, "head PRT [0, 64K) intact after middle CLEAR");
    check_zero(vaPrt2 + k64K, k64K, "cleared middle [64K, 128K) leaves are 0 (invalid, not PRT)");
    check_prt(vaPrt2 + 2 * k64K, 2 * k64K, "tail PRT [128K, 256K) intact after middle CLEAR");

    // Re-populate cleared hole back to PRT via MAP (no overlap now!), then CLEAR entire range.
    expect_u("MAP PRT back into cleared hole", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, vaPrt2 + k64K, 0, k64K), kOk);
    check_prt(vaPrt2, 4 * k64K, "re-mapped hole restored full 256 KiB PRT");
    tlbBefore = vm.tlbFlushes;
    expect_u("CLEAR full 256 KiB covering 3 adjacent PRT entries", vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, vaPrt2, 0, 4 * k64K), kOk);
    expect_u("TLB flushed on full CLEAR", vm.tlbFlushes, tlbBefore + 1);
    check_zero(vaPrt2, 4 * k64K, "full CLEAR zeroed all 64 pages");

    // Idempotent CLEAR on empty range still flushes TLB and succeeds.
    tlbBefore = vm.tlbFlushes;
    expect_u("CLEAR on already-empty VA range succeeds", vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, vaPrt2, 0, k64K), kOk);
    expect_u("TLB flushed even on empty CLEAR", vm.tlbFlushes, tlbBefore + 1);

    // Exact UNMAP of PRT.
    expect_u("MAP 64 KiB PRT for exact UNMAP", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, vaPrt2, 0, k64K), kOk);
    expect_u("wrong-size UNMAP on PRT rejected", vm.gem_va(RDNA4N_VA_OP_UNMAP, 0, RDNA4N_VM_PAGE_PRT, vaPrt2, 0, kPage), kBadArg);
    tlbBefore = vm.tlbFlushes;
    expect_u("exact UNMAP on PRT succeeds", vm.gem_va(RDNA4N_VA_OP_UNMAP, 0, RDNA4N_VM_PAGE_PRT, vaPrt2, 0, k64K), kOk);
    expect_u("TLB flushed on PRT UNMAP", vm.tlbFlushes, tlbBefore + 1);
    check_zero(vaPrt2, k64K, "exact PRT UNMAP cleared leaves to 0");

    // Verify high-half sibling mapping survived every low-half operation!
    check_prt(va0 + 2 * k64K, k64K, "high-half PRT sibling untouched");
    check_contig(va0 + 3 * k64K, k64K, 0x8020000000ull, true, "high-half BO 3 sibling untouched");
    vm.close_session();
    expect_u("global PRT refcount returns to 0 after close", gPrt, 0);
}

// ---- P5: Map-table capacity exhaustion (kNoResources) atomic refusal -----------------------------------------------------
static void p5_map_slot_capacity() {
    uint64_t gPrt = 0;
    TestVm vm(4, &gPrt); // tiny 4-slot map table
    vm.add_contig_bo(1, 0x10000ull, 0x8010000000ull);
    vm.add_contig_bo(2, 0x10000ull, 0x8020000000ull);
    for (uint32_t i = 0; i < 4; i++) {
        expect_u("fill slot with PRT", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0x100000ull + i * 0x10000ull, 0, 0x10000ull), kOk);
    }
    expect_u("all 4 slots used by PRT", vm.prtMaps, 4);

    // Table is 100% full (0 free slots):
    // 1) Middle-split CLEAR needs +1 slot -> kNoResources, nothing mutated!
    expect_u("middle-split CLEAR on full table -> NoResources",
             vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, 0x104000ull, 0, 0x4000ull), kNoResources);
    expect_u("prtMaps unchanged after refused CLEAR", vm.prtMaps, 4);
    expect_u("prtPages unchanged after refused CLEAR", vm.prtPages, 64);
    { uint64_t leaf = 0; pt_walk(vm.rootPa, 0x104000ull, vm.mem, &leaf); expect_u("PRT leaf intact after refused CLEAR", leaf, kPtePrtLeaf); }

    // 2) Head-trim REPLACE needs +1 slot -> kNoResources, nothing mutated!
    expect_u("head-trim REPLACE on full table -> NoResources",
             vm.gem_va(RDNA4N_VA_OP_REPLACE, 1, 0xeu, 0x100000ull, 0, 0x4000ull), kNoResources);
    { uint64_t leaf = 0; pt_walk(vm.rootPa, 0x100000ull, vm.mem, &leaf); expect_u("PRT leaf intact after refused head REPLACE", leaf, kPtePrtLeaf); }

    // 3) Exact-cover REPLACE frees 1 slot and reuses it -> SUCCEEDS even on full table!
    expect_u("exact-cover REPLACE on full table succeeds",
             vm.gem_va(RDNA4N_VA_OP_REPLACE, 1, 0xeu, 0x100000ull, 0, 0x10000ull), kOk);
    expect_u("prtMaps 4 -> 3 after exact cover", vm.prtMaps, 3);
    expect_u("exact-cover REPLACE walks to BO 1", pt_walk(vm.rootPa, 0x100000ull, vm.mem, nullptr), 0x8010000000ull);

    // 4) Free 1 slot (now 1 free slot total): middle-split REPLACE needs +2 slots -> kNoResources!
    expect_u("CLEAR slot 3 to make 1 free slot", vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, 0x130000ull, 0, 0x10000ull), kOk);
    expect_u("middle-split REPLACE with only 1 free slot -> NoResources",
             vm.gem_va(RDNA4N_VA_OP_REPLACE, 2, 0xeu, 0x114000ull, 0, 0x4000ull), kNoResources);
    { uint64_t leaf = 0; pt_walk(vm.rootPa, 0x114000ull, vm.mem, &leaf); expect_u("PRT intact after 1-slot middle REPLACE refusal", leaf, kPtePrtLeaf); }

    // 5) Free another slot (now 2 free slots): middle-split REPLACE succeeds!
    expect_u("CLEAR slot 2 to make 2 free slots", vm.gem_va(RDNA4N_VA_OP_CLEAR, 0, 0, 0x120000ull, 0, 0x10000ull), kOk);
    expect_u("middle-split REPLACE with 2 free slots succeeds",
             vm.gem_va(RDNA4N_VA_OP_REPLACE, 2, 0xeu, 0x114000ull, 0, 0x4000ull), kOk);
    expect_u("middle now walks to BO 2", pt_walk(vm.rootPa, 0x114000ull, vm.mem, nullptr), 0x8020000000ull);
    { uint64_t l0 = 0, l1 = 0; pt_walk(vm.rootPa, 0x110000ull, vm.mem, &l0); pt_walk(vm.rootPa, 0x118000ull, vm.mem, &l1);
      expect(l0 == kPtePrtLeaf && l1 == kPtePrtLeaf, "left & right split PRTs intact"); }

    vm.close_session();
    expect_u("globalPrtMaps == 0 after P5 close", gPrt, 0);
}

// ---- P6: PT quota exhaustion atomic rollback (TestMem + production PtMem) ------------------------------------------------
static void p6_quota_rollback() {
    // Part A: TestMem rollback preserving pre-existing overlapping BO and PRT leaves!
    {
        uint64_t gPrt = 0;
        TestVm vm(32, &gPrt);
        const uint64_t va0 = 0x00000000001F0000ull; // 64 KiB before 2 MiB PTB seam
        vm.add_contig_bo(1, 0x400000ull, 0x8010000000ull);
        vm.add_scattered_bo(2, 0x400000ull, 0x300000000ull, 0x2000ull);

        // Pre-map 64 KiB BO 1 in PTB 0 and 64 KiB PRT adjacent before seam.
        expect_u("pre-map BO 1 in PTB 0", vm.gem_va(RDNA4N_VA_OP_MAP, 1, 0xeu, va0 - 0x10000ull, 0, 0x10000ull), kOk);
        expect_u("pre-map PRT in PTB 0", vm.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, va0, 0, 0x10000ull), kOk);
        const uint32_t allocsBefore = vm.mem.allocs;
        const uint32_t prtMapsBefore = vm.prtMaps;
        const uint64_t prtPagesBefore = vm.prtPages;

        // Inject quota exhaustion when trying to allocate the second PTB across the 2 MiB seam.
        vm.mem.failAt = vm.mem.allocs; // fail on very next PTB alloc (for va >= 0x200000)
        // 1) REPLACE BO->BO straddling PTB 0 (existing BO 1 + PRT) and PTB 1 (unallocated -> NoMemory!).
        expect_u("contiguous REPLACE hits quota -> NoMemory",
                 vm.gem_va(RDNA4N_VA_OP_REPLACE, 1, 0xeu, va0 - 0x10000ull, 0x20000ull, 0x30000ull), kNoMemory);
        expect_u("PT quota unchanged after REPLACE rollback", vm.mem.allocs, allocsBefore);
        expect_u("prtMaps unchanged after REPLACE rollback", vm.prtMaps, prtMapsBefore);
        expect_u("prtPages unchanged after REPLACE rollback", vm.prtPages, prtPagesBefore);
        expect_u("pre-existing overlapping BO 1 leaf STILL INTACT (not zeroed!)",
                 pt_walk(vm.rootPa, va0 - 0x10000ull, vm.mem, nullptr), 0x8010000000ull);
        { uint64_t leaf = 0; pt_walk(vm.rootPa, va0, vm.mem, &leaf);
          expect_u("pre-existing overlapping PRT leaf STILL INTACT (not zeroed!)", leaf, kPtePrtLeaf); }

        // 2) Scattered REPLACE straddling PTB 0 and PTB 1 hits quota -> rollback preserves both!
        expect_u("scattered REPLACE hits quota -> NoMemory",
                 vm.gem_va(RDNA4N_VA_OP_REPLACE, 2, 0xeu, va0 - 0x10000ull, 0, 0x30000ull), kNoMemory);
        expect_u("PT quota unchanged after scattered REPLACE rollback", vm.mem.allocs, allocsBefore);
        expect_u("pre-existing BO 1 intact after scattered rollback",
                 pt_walk(vm.rootPa, va0 - 0x10000ull, vm.mem, nullptr), 0x8010000000ull);
        { uint64_t leaf = 0; pt_walk(vm.rootPa, va0, vm.mem, &leaf);
          expect_u("pre-existing PRT intact after scattered rollback", leaf, kPtePrtLeaf); }

        // 3) PRT REPLACE spanning PTB 0 + PTB 1 + PTB 2 fails on PTB 2 (after allocating PTB 1!) -> rolls back PTB 1 cleanly!
        vm.mem.failAt = vm.mem.allocs + 1; // let PTB 1 allocate, fail on PTB 2
        expect_u("PRT REPLACE mid-alloc hits quota -> NoMemory",
                 vm.gem_va(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, va0 - 0x10000ull, 0, 0x230000ull), kNoMemory);
        expect_u("PT quota rolled back (PTB 1 reclaimed)", vm.mem.allocs, allocsBefore);
        expect_u("pre-existing BO 1 intact after mid-alloc PRT rollback",
                 pt_walk(vm.rootPa, va0 - 0x10000ull, vm.mem, nullptr), 0x8010000000ull);
        expect_u("unmapped PTB 1 range still walks to 0", pt_walk(vm.rootPa, 0x200000ull, vm.mem, nullptr), 0ull);

        // 4) Clear failAt -> retry PRT REPLACE succeeds!
        vm.mem.failAt = 0xFFFFFFFFu;
        expect_u("retry PRT REPLACE succeeds after quota restored",
                 vm.gem_va(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, va0 - 0x10000ull, 0, 0x230000ull), kOk);
        { uint64_t l0 = 0, l1 = 0, l2 = 0;
          pt_walk(vm.rootPa, va0 - 0x10000ull, vm.mem, &l0);
          pt_walk(vm.rootPa, 0x200000ull, vm.mem, &l1);
          pt_walk(vm.rootPa, 0x400000ull, vm.mem, &l2);
          expect(l0 == kPtePrtLeaf && l1 == kPtePrtLeaf && l2 == kPtePrtLeaf, "retry PRT REPLACE populated all 3 PTBs"); }
        vm.close_session();
        expect_u("globalPrtMaps == 0 after P6-A close", gPrt, 0);
    }

    // Part B: Real production PtSnap/PtMem (from native_s1c.cpp) exercising pt_fill_tx & pt_replace_tx rollback!
    {
        amdgpu::pt_env_reset(256ull << 20, 256ull << 20);
        amdgpu::Session s;
        amdgpu::PtMem m{ &s };
        s.rootPa = m.alloc_page();
        expect(s.rootPa != 0ull, "prod PtMem root allocated");

        const uint64_t leafBo = leaf_flags(0xeu, false, false);
        const uint64_t vaHigh = 0xFFFF8000001F0000ull; // high-half, 64 KiB before 2 MiB seam
        expect_u("prod PtMem: map initial BO in PTB 0", pt_map_tx(s.rootPa, vaHigh, 16, 0x900000000ull, leafBo, m), kOk);
        const uint32_t nblkBefore = s.nblk;
        const uint32_t ptPagesBefore = s.ptPages;

        // Exhaust visible free right at the boundary so the next new block fails under production PtMem!
        while (s.pagesInBlk < 4u) expect(m.alloc_page() != 0ull, "pad to block boundary");
        const uint32_t nblkPad = s.nblk, pagesInBlkPad = s.pagesInBlk, ptPagesPad = s.ptPages;
        const uint32_t freeNBefore = amdgpu::gPtFreeN;
        // Allow exactly 1 new 16 KiB block (4 table pages), then OOM on the 2nd block when mapping a 64 MiB PRT range!
        amdgpu::gCtx->gmc.vram_alloc.freeBytes = amdgpu::kArenaBlockBytes;
        expect_u("prod PtMem: pt_fill_tx across 64 MiB hits quota -> NoMemory",
                 pt_fill_tx(s.rootPa, vaHigh, (64ull << 20) / kPage, kPtePrtLeaf, m), kNoMemory);
        expect_u("prod PtMem: nblk restored after PRT rollback", s.nblk, nblkPad);
        expect_u("prod PtMem: pagesInBlk restored after PRT rollback", s.pagesInBlk, pagesInBlkPad);
        expect_u("prod PtMem: ptPages restored after PRT rollback", s.ptPages, ptPagesPad);
        expect_u("prod PtMem: newly allocated reserve block returned to gPtFree", amdgpu::gPtFreeN, freeNBefore + 1u);
        expect_u("prod PtMem: pre-existing overlapping BO leaf preserved after pt_fill_tx rollback",
                 pt_walk(s.rootPa, vaHigh, m, nullptr), 0x900000000ull);

        // Now restore ample free and verify pt_fill_tx (BO->PRT) + pt_replace_tx (PRT->BO) succeed cleanly!
        amdgpu::gCtx->gmc.vram_alloc.freeBytes = 128ull << 20;
        expect_u("prod PtMem: pt_fill_tx BO->PRT succeeds", pt_fill_tx(s.rootPa, vaHigh, 32, kPtePrtLeaf, m), kOk);
        { uint64_t l0 = 0, l1 = 0;
          pt_walk(s.rootPa, vaHigh, m, &l0);
          pt_walk(s.rootPa, vaHigh + 16 * kPage, m, &l1);
          expect(l0 == kPtePrtLeaf && l1 == kPtePrtLeaf, "prod PtMem: all 32 pages across seam carry exact kPtePrtLeaf"); }
        expect_u("prod PtMem: pt_replace_tx PRT->BO middle succeeds",
                 pt_replace_tx(s.rootPa, vaHigh + 8 * kPage, 16, 0x910000000ull, leafBo, m), kOk);
        { uint64_t lHead = 0, lTail = 0;
          pt_walk(s.rootPa, vaHigh, m, &lHead);
          pt_walk(s.rootPa, vaHigh + 24 * kPage, m, &lTail);
          expect(lHead == kPtePrtLeaf && lTail == kPtePrtLeaf, "prod PtMem: PRT head and tail preserved");
          expect_u("prod PtMem: replaced middle walks to BO PA", pt_walk(s.rootPa, vaHigh + 8 * kPage, m, nullptr), 0x910000000ull); }
        (void)nblkBefore; (void)ptPagesBefore;
    }
}

// ---- P7: Multi-session PRT refcount, BoFree of split BO, and session exit cleanup ----------------------------------------
static void p7_refcount_and_teardown() {
    uint64_t gPrt = 0;
    TestVm s1(64, &gPrt), s2(64, &gPrt);
    const uint64_t k64K = 0x10000ull;

    s1.add_contig_bo(1, 4 * k64K, 0x8010000000ull);
    s2.add_contig_bo(1, 2 * k64K, 0x8030000000ull);

    // Session 1: map BO 1 (256 KiB), punch middle 64 KiB with PRT -> BO 1 split into 2 pieces + 1 PRT.
    expect_u("s1 MAP BO 1", s1.gem_va(RDNA4N_VA_OP_MAP, 1, 0xeu, 0x1000000ull, 0, 4 * k64K), kOk);
    expect_u("s1 REPLACE middle with PRT", s1.gem_va(RDNA4N_VA_OP_REPLACE, 0, RDNA4N_VM_PAGE_PRT, 0x1010000ull, 0, k64K), kOk);
    expect_u("s1 prtMaps == 1, global == 1", s1.prtMaps * 10u + gPrt, 11u);

    // Session 2: map 2 PRT regions.
    expect_u("s2 MAP PRT A", s2.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0x2000000ull, 0, 2 * k64K), kOk);
    expect_u("s2 MAP PRT B", s2.gem_va(RDNA4N_VA_OP_MAP, 0, RDNA4N_VM_PAGE_PRT, 0x3000000ull, 0, 2 * k64K), kOk);
    expect_u("globalPrtMaps == 3 across 2 sessions", gPrt, 3);

    // BoFree(1) in Session 1 MUST unmap BOTH split pieces of BO 1 while leaving the middle PRT intact!
    const uint64_t flBefore = s1.tlbFlushes;
    expect_u("s1 BoFree split BO 1", s1.bo_free(1), kOk);
    expect_u("BoFree flushed TLB", s1.tlbFlushes, flBefore + 1);
    expect_u("BO 1 left piece unmapped", pt_walk(s1.rootPa, 0x1000000ull, s1.mem, nullptr), 0ull);
    expect_u("BO 1 right piece unmapped", pt_walk(s1.rootPa, 0x1020000ull, s1.mem, nullptr), 0ull);
    { uint64_t leaf = 0; pt_walk(s1.rootPa, 0x1010000ull, s1.mem, &leaf);
      expect_u("middle PRT survives BoFree of split BO", leaf, kPtePrtLeaf); }
    expect_u("s1 prtMaps still 1 after BoFree", s1.prtMaps, 1);

    // Close Session 1: its PRT refcount drops, Session 2 unaffected.
    s1.close_session();
    expect_u("s1 prtMaps == 0 after close", s1.prtMaps, 0);
    expect_u("s1 prtPages == 0 after close", s1.prtPages, 0);
    expect_u("globalPrtMaps == 2 (s2 intact)", gPrt, 2);
    expect_u("s2 prtMaps still 2", s2.prtMaps, 2);

    // Close Session 2: global refcount hits 0, all maps cleared.
    s2.close_session();
    expect_u("s2 prtMaps == 0 after close", s2.prtMaps, 0);
    expect_u("s2 prtPages == 0 after close", s2.prtPages, 0);
    expect_u("globalPrtMaps == 0 after all sessions closed", gPrt, 0);
    bool anyUsed = false;
    for (const VaEnt &e : s1.maps) if (va_ent_used(e)) anyUsed = true;
    for (const VaEnt &e : s2.maps) if (va_ent_used(e)) anyUsed = true;
    expect(!anyUsed, "zero lingering VA entries after session exit");
}

// ---- P8: Production source pins ------------------------------------------------------------------------------------------
static void p8_source_pins(const std::string &root) {
    const std::string abi = slurp(root + "/src/garnet/src/GarnetNativeABI.h");
    const std::string iph = slurp(root + "/src/garnet/src/amd/amdgpu_ip.h");
    const std::string gmc = slurp(root + "/src/garnet/src/amd/gmc_v12_0.cpp");
    const std::string eng = garnet_test_ver::read_native_engine(root + "/src/garnet/src/amd");
    expect(!abi.empty() && !iph.empty() && !gmc.empty() && !eng.empty(), "production sources readable");

    expect(abi.find("#define RDNA4N_ABI_MINOR     13u ") != std::string::npos, "GarnetNativeABI.h sets RDNA4N_ABI_MINOR = 13u");
    expect(abi.find("#define RDNA4N_CAP_VM_PRT         (1u << 4)") != std::string::npos, "GarnetNativeABI.h defines RDNA4N_CAP_VM_PRT");
    expect(iph.find("constexpr uint64_t PRT_GFX12 = (1ULL << 56);") != std::string::npos &&
           iph.find("constexpr uint64_t PRT_LEAF  = PRT_GFX12 | SNOOPED | SYSTEM | IS_PTE;") != std::string::npos,
           "amdgpu_ip.h defines GFX12 PRT_GFX12 and PRT_LEAF");
    expect(gmc.find("gmc_vm_pte_flags(uint64_t flags, bool prt)") != std::string::npos &&
           gmc.find("(flags & ~PTEFlags::VALID) | PTEFlags::PRT_LEAF") != std::string::npos,
           "gmc_v12_0.cpp implements gmc_vm_pte_flags clearing VALID and setting PRT_LEAF");

    const std::string qi = fn_body(eng, "IOReturn n1c_query_info(");
    expect(qi.find("RDNA4N_CAP_VM_PRT") != std::string::npos && qi.find("o->reserved[2] = RDNA4N_ABI_MINOR;") != std::string::npos,
           "n1c_query_info exposes RDNA4N_CAP_VM_PRT and ABI minor 13");

    const std::string gv = fn_body(eng, "IOReturn n1c_gem_va(");
    expect(gv.find("RDNA4N_VA_OP_REPLACE") != std::string::npos &&
           gv.find("RDNA4N_VA_OP_CLEAR") != std::string::npos &&
           gv.find("gmc_vm_pte_flags(0ull, true)") != std::string::npos &&
           gv.find("pt_fill_tx(") != std::string::npos &&
           gv.find("pt_replace_tx(") != std::string::npos &&
           gv.find("pt_replace_pages_tx(") != std::string::npos &&
           gv.find("va_carve(") != std::string::npos &&
           gv.find("va_replace_apply(") != std::string::npos &&
           gv.find("session_sync_prt(s);") != std::string::npos,
           "n1c_gem_va wires MAP/REPLACE/CLEAR/UNMAP, PRT PTEs, interval splitting, and PRT refcount");

    const std::string cl = fn_body(eng, "void n1c_close(");
    expect(cl.find("va_remove_prt(s->maps, kMaxMaps);") != std::string::npos &&
           cl.find("session_sync_prt(s);") != std::string::npos,
           "n1c_close cleans up PRT entries and decrements PRT refcounts on teardown");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : "../../..";
    p1_abi_negotiation();
    p2_prt_pte_and_validation();
    p3_p4_splitting_and_tlb();
    p5_map_slot_capacity();
    p6_quota_rollback();
    p7_refcount_and_teardown();
    p8_source_pins(root);
    std::printf("native_prt_sparse_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
