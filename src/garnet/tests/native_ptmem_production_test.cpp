// native_ptmem_production_test.cpp - 3.1 production-PtMem host coverage (actual PtMem, stub boundary).
// Build:
//   src/garnet/tests/test-native-ptmem-production.py "$TMPDIR/native_ptmem_actual.inc" && \
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/garnet/src -I "$TMPDIR" \
//       src/garnet/tests/native_ptmem_production_test.cpp -o "$TMPDIR/native_ptmem_prod" && \
//       "$TMPDIR/native_ptmem_prod" . "$TMPDIR/native_ptmem_actual.inc"
// The test does NOT copy the PtMem algorithm. Production PtSnap+PtMem come verbatim from
// native_s1c.cpp via test-native-ptmem-production.py (`struct PtSnap {` .. `// Return a
// session's page-table blocks`, exclusive) into native_ptmem_actual.inc, included below.
// Stubs provide only the boundary with real kernel names (Session/VRAMAllocation/
// BringupContext/DeviceContext/GMCContext/VRAMBumpAllocator/gCtx/gPt/gPtFree/gPtN/gPtFreeN/
// kArenaBlocks/kArenaBlockBytes/kArenaReserve/pa_of_mc/RBAR0_64/WBAR0_64/bar0_memset_vram)
// over host memory. Covers with the ACTUAL object: partial-block fail->rollback->retry
// (zeroed reuse), global-cap-first exhaustion with reserve reuse (no new visible bytes),
// client exact cap (32MiB), existing mappings preserved (high-half via canonicalize).
#include "garnet_version_check.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "amd/garnet_submit_pure.h"
#include "amd/garnet_multisession_pure.h"
#include "GarnetNativeABI.h"

using namespace rdna4native;
using namespace rdna4native::s1c;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

namespace amdgpu {
// ---- boundary stubs with real kernel names (host memory, same call shape) ----
constexpr uint32_t kArenaBlocks = (uint32_t)(kPtCap / 16384u);
constexpr uint32_t kArenaBlockBytes = 16384u;
constexpr uint32_t kArenaReserve = kArenaBlocks * rdna4native::g2::kHwVmids;
static_assert(kArenaReserve <= 0xFFFFu, "page-table block indices fit the uint16_t lists");

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
        if (out->gpu_va == 0ull) return false;
        return true;
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
static VRAMBumpAllocator gAllocStore;

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

static void test_reset(uint64_t visTotal, uint64_t visFree) {
    gBarMem.clear();
    gAllocStore.total = visTotal;
    gAllocStore.freeBytes = visFree;
    gAllocStore.nextVa = 0x1000;
    gCtxStore.gmc.vram_alloc = gAllocStore;
    // Preserve allocator object identity: PtMem uses gCtx->gmc.vram_alloc methods mutating freeBytes/nextVa.
    // Keep the live object in gCtxStore; snapshot freeBytes via the same object.
    gCtxStore.gmc.vram_start = 0x80000000ull;
    gCtxStore.gmc.vram_base_offset = 0x100000000ull;
    gDevStore.bar0Size = (size_t)(1ull << 32);
    gCtxStore.dev = &gDevStore;
    gCtx = &gCtxStore;
    gPtN = 0; gPtFreeN = 0;
    memset(gPt, 0, sizeof(gPt));
    memset(gPtFree, 0, sizeof(gPtFree));
}
} // namespace amdgpu

using amdgpu::gCtx;
using amdgpu::gPt;
using amdgpu::gPtFree;
using amdgpu::gPtN;
using amdgpu::gPtFreeN;

static void l0_generated(const std::string &root, const std::string &fragment) {
    std::string eng = slurp(root + "/src/garnet/src/amd/garnet_submit_internal.hpp");
    std::string gen = slurp(fragment);
    expect(!eng.empty() && !gen.empty(), "sources + generated fragment readable");
    size_t si = eng.find("struct PtSnap {");
    size_t ei = eng.find("// Return a session's page-table blocks", si);
    expect(si != std::string::npos && ei != std::string::npos && ei > si, "extraction range found in native_s1c.cpp");
    if (si == std::string::npos || ei == std::string::npos) return;
    std::string want = eng.substr(si, ei - si);
    // Trim trailing whitespace like the generator (rstrip + newline).
    while (!want.empty() && (want.back() == ' ' || want.back() == '\n' || want.back() == '\r' || want.back() == '\t')) want.pop_back();
    want += "\n";
    std::string have = gen;
    // Strip the 3-line generator header.
    size_t ln = 0;
    for (int i = 0; i < 3; i++) { size_t p = have.find('\n', ln); if (p == std::string::npos) break; ln = p + 1; }
    std::string fragBody = have.substr(ln);
    expect(fragBody == want, "native_ptmem_actual.inc is the exact slice (rerun the script if stale)");
}

// Partial-block fail -> rollback -> retry with the ACTUAL PtMem; sibling intact.
static void l1_partial_retry() {
    amdgpu::test_reset(256ull << 20, 256ull << 20);
    amdgpu::Session s;
    amdgpu::PtMem m{ &s };
    s.rootPa = m.alloc_page();
    expect(s.rootPa != 0ull, "root allocated");
    const uint64_t leaf = leaf_flags(0xe, false, false);
    expect_u("sibling low maps", pt_map_tx(s.rootPa, 0x10000000ull, 1, 0x900000000ull, leaf, m), kOk);
    while (!(s.nblk >= 1u && s.pagesInBlk <= 2u)) {
        if (s.pagesInBlk < 4u) {
            uint64_t fill = m.alloc_page();
            expect(fill != 0ull, "fill to block boundary");
            for (uint32_t e = 0; e < 512u; e++) m.wr(fill, e, 0ull);
        } else {
            uint64_t nb = m.alloc_page();
            expect(nb != 0ull, "take fresh block for partial state");
            for (uint32_t e = 0; e < 512u; e++) m.wr(nb, e, 0ull);
            break;
        }
        if (s.pagesInBlk == 1u) break;
    }
    expect(s.nblk >= 1u && s.pagesInBlk <= 2u, "partial state with 2 slots in old block");
    amdgpu::PtSnap snap = m.snapshot();
    uint32_t blocksBefore = s.nblk;
    uint64_t p1 = m.alloc_page();
    uint64_t p2 = m.alloc_page();
    expect(p1 != 0ull && p2 != 0ull, "two tail pages handed out");
    expect_u("both tails in old block (no new block)", s.nblk, blocksBefore);
    for (uint32_t e = 0; e < 512u; e++) { m.wr(p1, e, pde_encode(0xA000000ull + (uint64_t)e * 0x1000ull)); m.wr(p2, e, 0xDEADBEEF00000001ull); }
    m.rollback(snap, s.rootPa, 0x20000000ull, 2);
    expect_u("nblk restored", s.nblk, snap.nblk);
    expect_u("pagesInBlk restored", s.pagesInBlk, snap.pagesInBlk);
    bool zeroed = true;
    for (uint32_t e = 0; e < 512u; e++) if (m.rd(p1, e) != 0ull || m.rd(p2, e) != 0ull) zeroed = false;
    expect(zeroed, "rolled-back tail pages scrubbed to zero");
    uint64_t q1 = m.alloc_page();
    uint64_t q2 = m.alloc_page();
    expect_u("retry reuses first tail page", q1, p1);
    expect_u("retry reuses second tail page", q2, p2);
    bool stillZero = true;
    for (uint32_t e = 0; e < 512u; e++) if (m.rd(q1, e) != 0ull || m.rd(q2, e) != 0ull) stillZero = false;
    expect(stillZero, "reused tail pages hand out zeroed");
    expect_u("retry tx maps", pt_map_tx(s.rootPa, 0x20000000ull, 2, 0x910000000ull, leaf, m), kOk);
    expect_u("retry walks", pt_walk(s.rootPa, 0x20000000ull, m, nullptr), 0x910000000ull);
    expect_u("sibling intact after retry", pt_walk(s.rootPa, 0x10000000ull, m, nullptr), 0x900000000ull);
    // New-block span: fresh whole blocks are zeroed on next take (whole-block memset).
    amdgpu::PtSnap snap2 = m.snapshot();
    while (s.pagesInBlk < 4u) {
        uint64_t f = m.alloc_page();
        for (uint32_t e = 0; e < 512u; e++) m.wr(f, e, 0ull);
    }
    snap2 = m.snapshot();
    uint64_t n1 = m.alloc_page();
    expect(n1 != 0ull, "new-block page handed out");
    for (uint32_t e = 0; e < 512u; e++) m.wr(n1, e, 0xBAD0000000000001ull);
    m.rollback(snap2, s.rootPa, 0x30000000ull, 1);
    uint64_t r1 = m.alloc_page();
    bool rzero = true;
    for (uint32_t e = 0; e < 512u; e++) if (m.rd(r1, e) != 0ull) rzero = false;
    expect(rzero, "new-block reuse is zeroed on take (whole-block memset)");

    while (s.pagesInBlk < 4u) expect(m.alloc_page() != 0, "fill before quarantine");
    const amdgpu::PtSnap quarantineSnap = m.snapshot();
    const uint32_t reserveBefore = gPtFreeN;
    const uint64_t quarantined = m.alloc_page();
    expect(quarantined != 0, "table allocated before failed invalidate");
    const uint32_t heldBefore = gPtN;
    m.rollback(quarantineSnap, s.rootPa, 0x50000000ull, 1);
    expect_u("rollback offers one block to reserve", gPtFreeN, reserveBefore + 1u);
    m.quarantine_rollback();
    expect_u("failed invalidate removes returned block", gPtFreeN, reserveBefore);
    expect_u("quarantine remains physically counted", gPtN, heldBefore);
    expect(m.alloc_page() != quarantined, "other mapping cannot reuse quarantined table");
}

// Global cap binds BEFORE visible free (small visible pool), reserve reuse needs no new bytes,
// client exact cap (32MiB) with ample global.
static void l2_global_reserve() {
    // Case A: visible 64MiB -> cap min(64MiB,16MiB)=16MiB. Give ample free (64MiB) so the cap,
    // not free, binds first. 16MiB = 1024 blocks of 16KiB.
    amdgpu::test_reset(64ull << 20, 64ull << 20);
    expect_u("cap for 64MiB visible is 16MiB", pt_global_cap(64ull << 20, rdna4native::g2::kAppKernelReserve), 16ull << 20);
    {
        amdgpu::Session s;
        amdgpu::PtMem m{ &s };
        s.rootPa = m.alloc_page();
        expect(s.rootPa != 0ull, "root with ample free");
        uint32_t allocs = 1;
        while (m.alloc_page() != 0ull) {
            allocs++;
            if (allocs > 5000) break;
        }
        // Global held counts every reserve block including free-list ones (never double-freed).
        uint64_t held = (uint64_t)gPtN * amdgpu::kArenaBlockBytes;
        expect_u("global held is exactly the 16MiB cap", held, 16ull << 20);
        expect_u("session blocks match reserve blocks (single session)", (uint64_t)s.nblk * amdgpu::kArenaBlockBytes + (uint64_t)gPtFreeN * amdgpu::kArenaBlockBytes, held);
        // Next new block must refuse from the cap even though visible free is still ample.
        expect(gCtx->gmc.vram_alloc.bytes_free() > amdgpu::kArenaBlockBytes, "visible free still ample when cap binds");
        s.pagesInBlk = 4u; // force new-block path
        expect(m.alloc_page() == 0ull, "new block refused by global cap (not by free)");
        // Reserve reuse: return one block to the free list; reuse must succeed with held==cap.
        expect(s.nblk > 0u, "session holds blocks to return");
        uint32_t victim = s.blk[s.nblk - 1];
        s.nblk--;
        s.pagesInBlk = 4u;
        gPtFree[gPtFreeN++] = (uint16_t)victim;
        uint64_t heldBefore = (uint64_t)gPtN * amdgpu::kArenaBlockBytes;
        uint64_t reuse = m.alloc_page();
        expect(reuse != 0ull, "free-reserve reuse succeeds at cap (no new visible bytes)");
        expect_u("reuse consumes no new global", (uint64_t)gPtN * amdgpu::kArenaBlockBytes, heldBefore);
        bool rzero = true;
        for (uint32_t e = 0; e < 512u; e++) if (m.rd(reuse, e) != 0ull) rzero = false;
        expect(rzero, "reused reserve page is zeroed");
    }
    // Case B: client exact cap. Visible 16GiB -> cap 64MiB > client 32MiB, so the client binds.
    amdgpu::test_reset(16ull << 30, 16ull << 30);
    expect_u("cap for 16GiB visible is 64MiB (capped)", pt_global_cap(16ull << 30, rdna4native::g2::kAppKernelReserve), 64ull << 20);
    {
        amdgpu::Session s;
        amdgpu::PtMem m{ &s };
        s.rootPa = m.alloc_page();
        expect(s.rootPa != 0ull, "root with huge visible");
        // Drive the session to exactly 2048 blocks (32MiB) via real alloc_page calls.
        uint32_t guard = 0;
        while (s.nblk < amdgpu::kArenaBlocks && guard++ < 10000) {
            s.pagesInBlk = 4u; // force new-block path each time
            // Ensure a free-list entry or fresh reserve is available: don't pre-seed; let
            // alloc_page consume global (64MiB allows the full 32MiB client).
            uint64_t pa = m.alloc_page();
            if (pa == 0ull) break;
            for (uint32_t e = 0; e < 512u; e++) m.wr(pa, e, 0ull);
        }
        expect_u("session reaches exactly 2048 blocks (32MiB)", s.nblk, amdgpu::kArenaBlocks);
        s.pagesInBlk = 4u;
        expect(m.alloc_page() == 0ull, "2049th block refused by client cap (32MiB not expanded)");
    }
}

// Existing mappings preserved across failed tx, high-half via production canonicalize.
static void l3_sibling() {
    amdgpu::test_reset(256ull << 20, 256ull << 20);
    amdgpu::Session s;
    amdgpu::PtMem m{ &s };
    s.rootPa = m.alloc_page();
    const uint64_t leaf = leaf_flags(0xe, false, false);
    const uint64_t highCanon = 0xFFFF800030000000ull;
    const VaReq q = gemva_check(RDNA4N_VA_OP_MAP, 1, 0, 0xe, highCanon, 0, 0x2000, 0, 0, 0, 0);
    expect_u("gemva high-half ok", q.rc, kOk);
    const uint64_t vaCanon = va_canonicalize(q.stripped);
    expect_u("canonicalize restores high", vaCanon, highCanon);
    expect_u("raw stripped fails precheck", pt_map_tx(s.rootPa, q.stripped, 2, 0x930000000ull, leaf, m), kBadArg);
    expect_u("canonicalized high maps", pt_map_tx(s.rootPa, vaCanon, 2, 0x930000000ull, leaf, m), kOk);
    expect_u("low sibling maps", pt_map_tx(s.rootPa, 0x10000000ull, 2, 0x940000000ull, leaf, m), kOk);
    expect_u("bad tx refused", pt_map_tx(s.rootPa, 0x20000000ull, 2, 0x0001000000000000ull, leaf, m), kBadArg);
    expect_u("high intact after bad tx", pt_walk(s.rootPa, highCanon, m, nullptr), 0x930000000ull);
    expect_u("low intact after bad tx", pt_walk(s.rootPa, 0x10000000ull, m, nullptr), 0x940000000ull);
    // NoMemory elsewhere (force by taking all remaining free then needing a new table).
    amdgpu::PtSnap snap = m.snapshot();
    (void)snap;
    // Drain visible free to force a NoMemory on a range needing a fresh table.
    gCtx->gmc.vram_alloc.freeBytes = 0;
    // A range in a fresh PTB needs new tables; reuse-only ranges could still succeed, so use
    // a far range (fresh PDB1 entry) to force new-block demand.
    expect_u("nomem tx refused", pt_map_tx(s.rootPa, 0x40000000ull, 600, 0x950000000ull, leaf, m), kNoMemory);
    expect_u("high intact after nomem", pt_walk(s.rootPa, highCanon, m, nullptr), 0x930000000ull);
    expect_u("low intact after nomem", pt_walk(s.rootPa, 0x10000000ull, m, nullptr), 0x940000000ull);
}

// Production PtMem rollback over 2 MiB huge-page splits (both 2-slot SplitUndo on CLEAR and
// >2 split huge leaves restored via child PTE[0] during PRT REPLACE OOM).
static void l4_huge_2m_rollback() {
    amdgpu::test_reset(256ull << 20, 256ull << 20);
    amdgpu::Session s;
    amdgpu::PtMem m{ &s };
    s.rootPa = m.alloc_page();
    const uint64_t lfDcc = leaf_flags(0xe, false, false, true);
    const uint64_t va = 0x40000000ull, pa = 0x500000000ull;
    // Map 8 MiB = 4x 2 MiB huge pages (uses root + PDB1 + PDB0 = 3 pages inside block 0, leaving 1 page free in block 0!).
    expect_u("prod PtMem 8 MiB huge map", pt_map_tx(s.rootPa, va, (8ull << 20) / kPage, pa, lfDcc, m, true), kOk);
    expect_u("only 3 PT pages in block 0", s.ptPages, 3u);
    expect_u("pagesInBlk == 3 (1 tail slot left)", s.pagesInBlk, 3u);
    uint64_t orig[4] = {};
    for (int i = 0; i < 4; i++) pt_walk(s.rootPa, va + (uint64_t)i * kHuge2mBytes, m, &orig[i]);

    // Case A: Drain visible free so block 1 cannot allocate; partial CLEAR across 2 huge pages
    // consumes the 1 tail slot for head split, then fails on tail split -> restores BOTH huge leaves & scrubs tail!
    gCtx->gmc.vram_alloc.freeBytes = 0;
    expect_u("prod PtMem partial CLEAR OOM on 2nd split", pt_unmap_tx(s.rootPa, va + 0x10000ull, (2ull << 20) / kPage, m), kNoMemory);
    expect_u("nblk restored after CLEAR OOM", s.nblk, 1u);
    expect_u("pagesInBlk restored after CLEAR OOM", s.pagesInBlk, 3u);
    expect_u("ptPages restored after CLEAR OOM", s.ptPages, 3u);
    for (int i = 0; i < 4; i++) {
        uint64_t got = 0;
        expect_u("huge page intact after CLEAR OOM", pt_walk(s.rootPa, va + (uint64_t)i * kHuge2mBytes + 0x123000ull, m, &got),
                 pa + (uint64_t)i * kHuge2mBytes + 0x123000ull);
        expect_u("huge leaf bit-exact after CLEAR OOM", got, orig[i]);
    }

    // Case B: Give room for EXACTLY 1 new 16 KiB block? Actually 1 tail slot + 0 new blocks = 1 page;
    // give room for 1 new block (16 KiB = 4 pages -> total 5 pages? Too many for 4 slots! Give 0 free -> 1 page;
    // OR consume tail page first via sibling, then give 16 KiB = 4 pages and map 10 MiB = 5 huge pages!).
    gCtx->gmc.vram_alloc.freeBytes = 16384; // allows 1 new block (4 pages) + 1 existing tail = 5 pages -> 6-huge-page PRT REPLACE splits 5 and fails on 6th!
    expect_u("prod PtMem map 2 more huge pages (total 12 MiB = 6 huge)", pt_map_tx(s.rootPa, va + (8ull << 20), (4ull << 20) / kPage, pa + (8ull << 20), lfDcc, m, true), kOk);
    expect_u("still 3 PT pages (same PDB0)", s.ptPages, 3u);
    uint64_t orig6[6] = {};
    for (int i = 0; i < 6; i++) pt_walk(s.rootPa, va + (uint64_t)i * kHuge2mBytes, m, &orig6[i]);

    // 12 MiB PRT REPLACE needs 6 PTBs (splitting all 6 huge pages), but only 5 PT pages are available -> fails on 6th!
    expect_u("prod PtMem 6-huge PRT REPLACE hits kNoMemory on 6th split", pt_fill_tx(s.rootPa, va, (12ull << 20) / kPage, kPtePrtLeaf, m), kNoMemory);
    expect_u("nblk restored to 1 after 5-split rollback", s.nblk, 1u);
    expect_u("pagesInBlk restored to 3 after 5-split rollback", s.pagesInBlk, 3u);
    expect_u("ptPages restored to 3 after 5-split rollback", s.ptPages, 3u);
    for (int i = 0; i < 6; i++) {
        uint64_t got = 0;
        expect_u("all 6 huge pages walk after 5-split rollback", pt_walk(s.rootPa, va + (uint64_t)i * kHuge2mBytes + 0x77000ull, m, &got),
                 pa + (uint64_t)i * kHuge2mBytes + 0x77000ull);
        expect_u("all 6 huge leaves bit-exact after 5-split rollback", got, orig6[i]);
    }

    // Retry with plenty of visible free succeeds cleanly.
    gCtx->gmc.vram_alloc.freeBytes = 128ull << 20;
    expect_u("retry 12 MiB PRT REPLACE succeeds", pt_fill_tx(s.rootPa, va, (12ull << 20) / kPage, kPtePrtLeaf, m), kOk);
    uint64_t prtLeaf = 0;
    expect_u("PRT walks invalid after retry", pt_walk(s.rootPa, va + 0x123000ull, m, &prtLeaf), 0ull);
    expect(entry_is_prt(prtLeaf), "PRT leaf active after retry");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <repo> <generated-include>\n", argv[0]);
        return 2;
    }
    std::string root = argc > 1 ? argv[1] : ".";
    l0_generated(root, argv[2]); l1_partial_retry(); l2_global_reserve(); l3_sibling(); l4_huge_2m_rollback();
    std::printf("native_ptmem_production_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
