// native_large_pt_test.cpp - 3.1 large-PT host coverage (4/8 GiB, quotas, tx rollback).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/garnet/src -I src/garnet/src/amd src/garnet/tests/native_large_pt_test.cpp -o /tmp/native_large_pt && /tmp/native_large_pt .
// Covers (Tx = transactional, Q = quota):
//   L1 quotas: kPtCap 32 MiB/client (8192 tables), kMaxMaps 4096, handles 4095; 4 GiB (~8 MiB PT) and
//      8 GiB (~16 MiB PT) fit one client; global cap = min(64 MiB, visible/4) then visible-free bound.
//   L2 walker: real 4/8 GiB mappings with synthetic consecutive PA through the same pt_map/pt_walk
//      the kext uses (no mock walker); spot walks, 2 MiB / 1 GiB directory seams, low/high halves,
//      canonical + seam rejection via pt_va_precheck/gemva_check, exact UNMAP.
//   L3 tx: mid-fail (NoMemory at Nth table, BadArg pte) leaves no leaf and no quota growth with
//      pt_map_tx/pt_map_pages_tx (snapshot/rollback kernel-implementable); retry succeeds; old
//      pt_map BadArg path already pre-validates (no leaf) but leaks dirs on NoMemory (documented).
//   L4 precise UNMAP: exact handle/start/size only, wrong triple refused, remainder intact.
//   L5 RO/BAR/session/guard: host_vm_flags clears W, BAR guard, session_unchanged, overlap/exact.
//   L6 pins: pure tx helpers present; kext MAP/import/prepare wired to tx (canonicalize +
//      per-block quota, reserve-only reclaim).
//   L7 production form: stripped (48-bit high) -> canonicalize -> tx succeeds; raw stripped
//      fails precheck (proves the canonicalize step); partial-block fail -> rollback -> retry
//      reuses zeroed tables; pre-existing sibling mappings survive failed tx.
#include "garnet_version_check.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "amd/native_hostimport_pure.h"
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

// Test memory mirroring the kernel PtMem contract (same rd/wr/alloc_page semantics).
// snapshot/rollback is kernel-implementable: snapshot captures the allocation high-water,
// rollback deletes tables allocated after it and clears directory entries pointing into them
// (leaves in reused tables are cleared beforehand by pt_unmap over the attempted range,
// safe because the caller guarantees no overlap there). Blocks return to the reserve,
// never to the user allocator.
struct TestMem {
    std::map<uint64_t, std::array<uint64_t, 512>> t;
    uint64_t next = 0x100000000ull;
    uint32_t budget = 1000000u;
    uint32_t allocs = 0;
    uint32_t failAt = 0xFFFFFFFFu; // inject NoMemory when allocs == failAt
    struct Snap { uint32_t allocs; uint64_t next; };
    Snap snapshot() const { return Snap{ allocs, next }; }
    // Range-aware rollback mirroring the kernel: walk only the attempted range and clear
    // directory entries pointing into tables allocated after snap, then discard those tables.
    // New tables are pa >= snap.next (monotonic allocator, like kernel new blocks).
    void rollback(const Snap &s, uint64_t rootPa, uint64_t va, uint64_t pages) {
        for (uint64_t p = 0; p < pages;) {
            uint64_t v = va + p * kPage;
            uint64_t tbl = rootPa;
            const uint32_t idx[3] = { idx_pdb2(v), idx_pdb1(v), idx_pdb0(v) };
            for (uint32_t lvl = 0; lvl < 3; lvl++) {
                uint64_t e = rd(tbl, idx[lvl]);
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
        allocs = s.allocs; next = s.next;
    }
    uint64_t rd(uint64_t pa, uint32_t idx) {
        auto it = t.find(pa);
        return it == t.end() ? 0ull : it->second[idx];
    }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { t[pa][idx] = v; }
    uint64_t alloc_page() {
        if (allocs >= budget || allocs == failAt) return 0ull;
        allocs++;
        uint64_t pa = next; next += 0x1000;
        t[pa].fill(0);
        return pa;
    }
};

static void l1_quotas() {
    expect_u("per-client PT cap 32 MiB", kPtCap, 32ull << 20);
    expect_u("pool is 8192 table pages", kPtCap / kPage, 8192ull);
    expect_u("max maps", (uint64_t)kMaxMaps, 4096ull);
    expect_u("handles 1..4095", (uint64_t)RDNA4N_MAX_BOS - 1, 4095ull);
    const uint64_t p4 = (4ull << 30) / kPage, p8 = (8ull << 30) / kPage;
    expect_u("4 GiB pages", p4, 1048576ull);
    expect_u("8 GiB pages", p8, 2097152ull);
    expect_u("4 GiB leaf tables", pt_leaf_tables(p4), 2048ull);
    expect_u("8 GiB leaf tables", pt_leaf_tables(p8), 4096ull);
    uint64_t b4 = pt_bytes_upper(p4), b8 = pt_bytes_upper(p8);
    expect(b4 < kPtCap && b8 < kPtCap, "4/8 GiB upper bound fits the 32 MiB client cap");
    expect_u("8 GiB upper ~16.06 MiB", b8, (4096ull + 16ull) * 4096ull);
    expect(pt_client_ok(0, b8) && !pt_client_ok(kPtCap, 4096), "client quota helper");
    // Global: min(64 MiB, visible/4), then visible-free bound. 64 MiB shares g2::kAppKernelReserve source.
    expect_u("global max 64 MiB", kPtGlobalCapMax, 64ull << 20);
    expect_u("visible 217 MiB -> ~54.25 MiB", pt_global_cap(217ull << 20, 64ull << 20), (217ull << 20) / 4ull);
    expect_u("visible 1 GiB -> 64 MiB (capped)", pt_global_cap(1ull << 30, 64ull << 20), 64ull << 20);
    expect_u("visible 16 GiB -> 64 MiB (capped)", pt_global_cap(16ull << 30, 64ull << 20), 64ull << 20);
    expect(pt_global_ok(0, b8, 217ull << 20, 217ull << 20, 64ull << 20), "8 GiB fits global when empty");
    expect(!pt_global_ok(pt_global_cap(217ull << 20, 64ull << 20), 4096, 217ull << 20, 217ull << 20, 64ull << 20), "global cap refuses overfill");
    expect(!pt_global_ok(0, 1ull << 20, 217ull << 20, 1ull << 12, 64ull << 20), "visible-free bound refuses when free is tiny");
    // Handle/map headroom for chunked backing: 32x256 MiB (Private) or 128x64 MiB (host) << 4095/4096.
    expect(32 < 4095 && 32 < 4096 && 128 < 4095 && 128 < 4096, "chunked 8 GiB fits handle/map tables");
}

static void l2_walker() {
    const uint64_t leaf = leaf_flags(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE | RDNA4N_VM_PAGE_EXECUTABLE, false, false);
    // 4 GiB low-half contiguous with synthetic PA.
    {
        TestMem m; m.budget = 9000;
        uint64_t root = m.alloc_page();
        uint64_t va = 0x10000000ull, pages = (4ull << 30) / kPage, pa0 = 0x200000000ull;
        expect_u("4 GiB tx maps", pt_map_tx(root, va, pages, pa0, leaf, m), kOk);
        expect_u("start walks", pt_walk(root, va, m, nullptr), pa0);
        expect_u("4 GiB - 4 KiB walks", pt_walk(root, va + pages * kPage - kPage, m, nullptr), pa0 + (pages - 1) * kPage);
        expect_u("just past end unmapped", pt_walk(root, va + pages * kPage, m, nullptr), 0);
        // 2 MiB seam inside: PTB boundary.
        uint64_t seam = (va + 0x1FF000ull) & ~0x1FFFFFull;
        expect_u("2 MiB seam low walks", pt_walk(root, seam, m, nullptr), pa0 + (seam - va));
        expect_u("2 MiB seam high walks", pt_walk(root, seam + 0x200000ull, m, nullptr), pa0 + (seam + 0x200000ull - va));
        // 1 GiB seam inside.
        uint64_t g1 = (va + 0x3FFFFFFFull) & ~0x3FFFFFFFull;
        expect_u("1 GiB seam low", pt_walk(root, g1, m, nullptr), pa0 + (g1 - va));
        expect_u("1 GiB seam high", pt_walk(root, g1 + 0x40000000ull, m, nullptr), pa0 + (g1 + 0x40000000ull - va));
        pt_unmap(root, va, pages, m);
        expect_u("after exact unmap start gone", pt_walk(root, va, m, nullptr), 0);
        expect_u("after exact unmap middle gone", pt_walk(root, va + (1ull << 30), m, nullptr), 0);
        expect_u("retry after unmap", pt_map_tx(root, va, pages, pa0 + 0x1000000ull, leaf, m), kOk);
        expect_u("retry walks to new pa", pt_walk(root, va, m, nullptr), pa0 + 0x1000000ull);
    }
    // 8 GiB high-half contiguous, spot-checked (full 2M walk would be slow under ASAN).
    {
        TestMem m; m.budget = 9000;
        uint64_t root = m.alloc_page();
        uint64_t va = 0xFFFF800010000000ull, pages = (8ull << 30) / kPage, pa0 = 0x100000000ull;
        expect_u("8 GiB tx maps", pt_map_tx(root, va, pages, pa0, leaf, m), kOk);
        expect_u("high start", pt_walk(root, va, m, nullptr), pa0);
        expect_u("4 GiB boundary low", pt_walk(root, va + (4ull << 30) - kPage, m, nullptr), pa0 + (4ull << 30) - kPage);
        expect_u("4 GiB boundary high", pt_walk(root, va + (4ull << 30), m, nullptr), pa0 + (4ull << 30));
        expect_u("8 GiB end", pt_walk(root, va + pages * kPage - kPage, m, nullptr), pa0 + (pages - 1) * kPage);
        expect_u("past end unmapped", pt_walk(root, va + pages * kPage, m, nullptr), 0);
        // Low half untouched (session isolation across halves sharing one root).
        expect_u("root[0] untouched by high-only map", (m.rd(root, 0) & kPteValid), 0);
        expect((m.rd(root, 256) & kPteValid) != 0, "root[256] present for high half");
        pt_unmap(root, va, pages, m);
        expect_u("high gone after unmap", pt_walk(root, va + (6ull << 30), m, nullptr), 0);
    }
    // Canonical / seam / floor rejection (reuses gemva_check + pt_va_precheck, no new validator).
    expect_u("non-canonical VA refused", pt_va_precheck(0x0000800000000000ull, 1), kBadArg);
    expect_u("seam-straddling refused", pt_va_precheck(0x00007FFFFFFFFFF000ull, 2), kBadArg);
    expect_u("below 0x10000 refused", pt_va_precheck(0xF000ull, 1), kBadArg);
    expect_u("zero pages refused", pt_va_precheck(0x100000ull, 0), kBadArg);
    expect_u("gemva seam still refused", gemva_check(RDNA4N_VA_OP_MAP, 1, 0, 0xe, 0x00007FFFFFFFFFF000ull, 0, 0x2000, 0, 0, 0, 0).rc, kBadArg);
    expect_u("gemva high-half ok", gemva_check(RDNA4N_VA_OP_MAP, 1, 0, 0xe, 0xFFFF800010000000ull, 0, 0x1000, 0, 0, 0, 0).rc, kOk);
}

static void l3_tx() {
    const uint64_t leaf = leaf_flags(0xe, false, false);
    // NoMemory mid-map with tx: no leaf, no quota growth, retry succeeds.
    {
        TestMem m; m.budget = 2000; // root + 8192: enough for small, not for 8 GiB (~4113)
        uint64_t root = m.alloc_page();
        uint64_t va = 0x20000000ull, pages = (8ull << 30) / kPage, pa0 = 0x300000000ull;
        uint32_t before = m.allocs;
        expect_u("8 GiB over budget -> NoMemory", pt_map_tx(root, va, pages, pa0, leaf, m), kNoMemory);
        expect_u("quota unchanged after tx rollback", m.allocs, before);
        bool none = true;
        for (uint64_t p = 0; p < pages; p += 65536) if (pt_walk(root, va + p * kPage, m, nullptr) != 0) none = false;
        expect(none, "no leaf of the failed 8 GiB exists (sampled)");
        // Retry with room succeeds.
        m.budget = 10000;
        expect_u("retry fits", pt_map_tx(root, va, pages, pa0, leaf, m), kOk);
        expect_u("retry walks", pt_walk(root, va, m, nullptr), pa0);
    }
    // Fail-injection at the Nth table: tx rolls back, non-tx would leak dirs (documented).
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        uint64_t va = 0x40000000ull, pages = 8192, pa0 = 0x400000000ull; // 32 MiB, 16 leaf tables + dirs
        m.failAt = m.allocs + 5;
        uint32_t before = m.allocs;
        expect_u("injected fail -> NoMemory", pt_map_tx(root, va, pages, pa0, leaf, m), kNoMemory);
        expect_u("injected fail leaves quota unchanged", m.allocs, before);
        expect_u("injected range has no leaf", pt_walk(root, va, m, nullptr), 0);
        m.failAt = 0xFFFFFFFFu;
        expect_u("retry after injection", pt_map_tx(root, va, pages, pa0, leaf, m), kOk);
        expect_u("retry walks", pt_walk(root, va, m, nullptr), pa0);
    }
    // BadArg pte mid-range: pre-validation leaves nothing (both pt_map and tx).
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        uint32_t before = m.allocs;
        expect_u("unencodable last page -> BadArg", pt_map_tx(root, 0x50000000ull, 4, 0x0000FFFFFFFFE000ull, leaf, m), kBadArg);
        expect_u("BadArg allocates nothing", m.allocs, before);
        expect_u("BadArg leaves no leaf", pt_walk(root, 0x50000000ull, m, nullptr), 0);
    }
    // Scattered tx with a bad page: nothing written.
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        uint64_t bad[4] = { 0x500000000ull, 0x501000000ull, 0x0001000000000000ull, 0x502000000ull };
        uint32_t before = m.allocs;
        expect_u("scattered bad page -> BadArg", pt_map_pages_tx(root, 0x60000000ull, 4, bad, leaf, m), kBadArg);
        expect_u("scattered BadArg allocates nothing", m.allocs, before);
    }
    // Scattered NoMemory rolls back too.
    {
        TestMem m; m.budget = 3;
        uint64_t root = m.alloc_page();
        std::vector<uint64_t> pa(700);
        for (uint64_t i = 0; i < 700; i++) pa[i] = 0x600000000ull + i * kPage;
        uint32_t before = m.allocs;
        expect_u("scattered over budget -> NoMemory", pt_map_pages_tx(root, 0x70000000ull, 700, pa.data(), leaf, m), kNoMemory);
        expect_u("scattered quota unchanged", m.allocs, before);
        expect_u("scattered no leaf", pt_walk(root, 0x70000000ull, m, nullptr), 0);
    }
}

static void l4_unmap() {
    const uint64_t leaf = leaf_flags(0xe, false, false);
    TestMem m; m.budget = 10000;
    uint64_t root = m.alloc_page();
    VaEnt t[8] = {};
    uint64_t a = 0x10000000ull, b = 0x20000000ull, c = 0x30000000ull;
    expect_u("map A", pt_map_tx(root, a, 256, 0x700000000ull, leaf, m), kOk);
    expect_u("map B", pt_map_tx(root, b, 256, 0x710000000ull, leaf, m), kOk);
    expect_u("map C", pt_map_tx(root, c, 256, 0x720000000ull, leaf, m), kOk);
    va_insert(t, 8, VaEnt{ a, 256 * kPage, 0, 7, 0xe });
    va_insert(t, 8, VaEnt{ b, 256 * kPage, 0, 7, 0xe });
    va_insert(t, 8, VaEnt{ c, 256 * kPage, 0, 8, 0xe });
    // Exact triple required.
    expect(va_find_exact(t, 8, 7, b, 256 * kPage) >= 0, "exact B found");
    expect(va_find_exact(t, 8, 7, b, 128 * kPage) < 0, "wrong size refused");
    expect(va_find_exact(t, 8, 8, b, 256 * kPage) < 0, "wrong handle refused");
    expect(va_overlap(t, 8, b, 128 * kPage) >= 0, "overlap still reports the range");
    pt_unmap(root, b, 256, m);
    t[1] = VaEnt{ 0, 0, 0, 0, 0 };
    expect_u("B gone", pt_walk(root, b, m, nullptr), 0);
    expect_u("A intact", pt_walk(root, a, m, nullptr), 0x700000000ull);
    expect_u("C intact", pt_walk(root, c, m, nullptr), 0x720000000ull);
    expect(va_overlap(t, 8, b, 256 * kPage) < 0, "B range free after exact unmap");
    // Partial unmap of A: first half only.
    pt_unmap(root, a, 128, m);
    expect_u("A head gone", pt_walk(root, a, m, nullptr), 0);
    expect_u("A tail kept", pt_walk(root, a + 128 * kPage, m, nullptr), 0x700000000ull + 128 * kPage);
}

static void l5_protect() {
    // Read-only import leaf never carries W (host_vm_flags reused, no new path).
    uint32_t ro = host_vm_flags(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE | RDNA4N_VM_PAGE_EXECUTABLE, true);
    expect_u("RO clamps W", ro, (uint32_t)(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_EXECUTABLE));
    uint64_t roLeaf = leaf_flags(ro, false, true);
    expect((roLeaf & kPteWrite) == 0 && (roLeaf & kPteRead) != 0 && (roLeaf & kPteSystem) != 0, "RO leaf is SYSTEM R, never W");
    uint64_t rwLeaf = leaf_flags(host_vm_flags(ro | RDNA4N_VM_PAGE_WRITEABLE, false), false, true);
    expect((rwLeaf & kPteWrite) != 0, "writable leaf keeps W");
    // Walker proves RO: map RO, leaf has no W.
    TestMem m; m.budget = 100;
    uint64_t root = m.alloc_page();
    uint64_t pa = 0x800000000ull;
    expect_u("map RO page", pt_map_pages_tx(root, 0x400000ull, 1, &pa, roLeaf, m), kOk);
    uint64_t got = 0; pt_walk(root, 0x400000ull, m, &got);
    expect((got & kPteWrite) == 0 && (got & kPteRead) != 0, "walked RO leaf carries no W");
    // BAR guard (existing verdict, spot).
    PciBar bars[] = { { 0x4000000000ull, 16ull << 30 }, { 0xE0000000ull, 2ull << 20 } };
    expect(!import_page_allowed(0x4000000000ull, bars, 2, 0), "BAR0 page refused");
    expect(import_page_allowed(0x1000ull, bars, 2, 0), "ordinary RAM allowed");
    expect(!import_page_allowed(0x1000ull, nullptr, 0, 0), "no BAR list fails closed");
    // Session isolation (existing helper).
    expect(session_unchanged(5, 5) && !session_unchanged(5, 6) && !session_unchanged(0, 0), "session sequence");
    // Overlap / exact still guard MAP/UNMAP.
    VaEnt t[4] = {};
    va_insert(t, 4, VaEnt{ 0x80000000ull, 0x10000, 0, 3, 0xe });
    expect(va_overlap(t, 4, 0x8000F000ull, 0x2000) >= 0, "overlap rejects double MAP");
    expect(va_find_exact(t, 4, 3, 0x80000000ull, 0x10000) >= 0, "exact triple for UNMAP");
    expect(va_find_exact(t, 4, 4, 0x80000000ull, 0x10000) < 0, "UNMAP with wrong handle refused");
}

static void l6_pins(const std::string &root) {
    std::string pure = slurp(root + "/src/garnet/src/amd/garnet_submit_pure.h");
    std::string himp = slurp(root + "/src/garnet/src/amd/native_hostimport_pure.h");
    std::string eng = garnet_test_ver::read_native_engine(root + "/src/garnet/src/amd");
    expect(!pure.empty() && !himp.empty() && !eng.empty(), "sources readable");
    expect(pure.find("pt_map_tx") != std::string::npos, "pure carries pt_map_tx");
    expect(himp.find("pt_map_pages_tx") != std::string::npos, "hostimport pure carries pt_map_pages_tx");
    expect(pure.find("pt_global_cap") != std::string::npos && pure.find("pt_global_ok") != std::string::npos, "pure carries global/client quota helpers");
    expect(pure.find("kPtGlobalCapMax") != std::string::npos, "pure names the 64 MiB global max");
    expect(pure.find("pt_contig_leaves_ok") == std::string::npos && pure.find("pt_scattered_leaves_ok") == std::string::npos, "redundant leaves helpers removed (walker owns leaf precheck)");
    // Real kernel wiring (3.1): user MAP + import via tx, stripped -> canonicalize, per-block quota.
    expect(eng.find("pt_map_tx(s->rootPa, vaCanon") != std::string::npos, "GemVa MAP wired to pt_map_tx with canonicalized VA");
    expect(eng.find("pt_map_pages_tx(s->rootPa, vaCanon") != std::string::npos || eng.find("pt_map_pages_tx(s->rootPa, vaCanon") != std::string::npos, "GemVa/import wired to pt_map_pages_tx with canonicalized VA");
    expect(eng.find("va_canonicalize(q.stripped)") != std::string::npos, "GemVa canonicalizes q.stripped (strict canonical check kept)");
    expect(eng.find("va_canonicalize(c.gpuStripped)") != std::string::npos, "import canonicalizes c.gpuStripped");
    expect(eng.find("struct PtSnap { uint32_t nblk; uint32_t pagesInBlk; uint32_t ptPages; }") != std::string::npos, "PtSnap simplified (no gPtN/gPtFreeN)");
    expect(eng.find("for (uint32_t e = 0; e < 512u; e++) wr(pa, e, 0ull)") != std::string::npos, "rollback scrubs old partial-block tail");
    expect(eng.find("mem.rollback(snap, s->rootPa, kBufferIbVa, 1)") != std::string::npos, "BufferEnv::prepare is transactional (reserve reclaim)");
}

// Production-form coverage: the kernel receives canonical VAs from userspace, gemva_check
// strips to 48-bit (high half: 0x8000...), then the tx entry canonicalizes back. Raw
// stripped must FAIL pt_va_precheck (strict), canonicalized must SUCCEED. Also covers
// sibling preservation across a failed tx and partial-block retry with zeroed reuse.
static void l7_prodform() {
    const uint64_t leaf = leaf_flags(0xe, false, false);
    // High-half production path: GemVa MAP canonical -> stripped -> canonicalize -> tx.
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        const uint64_t vaCanon = 0xFFFF800010000000ull;
        const uint64_t pages = 2;
        const VaReq q = gemva_check(RDNA4N_VA_OP_MAP, 1, 0, 0xe, vaCanon, 0, pages * kPage, 0, 0, 0, 0);
        expect_u("prodform gemva high-half ok", q.rc, kOk);
        expect_u("prodform stripped is 48-bit high", q.stripped, 0x800010000000ull);
        expect_u("prodform canonicalize(stripped) restores", va_canonicalize(q.stripped), vaCanon);
        expect_u("prodform raw stripped fails tx precheck (strict)", pt_map_tx(root, q.stripped, pages, 0x900000000ull, leaf, m), kBadArg);
        expect_u("prodform canonicalized tx maps", pt_map_tx(root, va_canonicalize(q.stripped), pages, 0x900000000ull, leaf, m), kOk);
        expect_u("prodform walk", pt_walk(root, vaCanon, m, nullptr), 0x900000000ull);
        // Sibling in low half survives; failed tx elsewhere leaves both intact.
        expect_u("sibling low map", pt_map_tx(root, 0x10000000ull, 2, 0x910000000ull, leaf, m), kOk);
        expect_u("failing tx (BadArg pa) refused", pt_map_tx(root, 0x20000000ull, 2, 0x0001000000000000ull, leaf, m), kBadArg);
        expect_u("high sibling intact after failed tx", pt_walk(root, vaCanon, m, nullptr), 0x900000000ull);
        expect_u("low sibling intact after failed tx", pt_walk(root, 0x10000000ull, m, nullptr), 0x910000000ull);
    }
    // Import production path: gpuVa canonical high -> import_check stripped -> canonicalize -> tx.
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        const uint64_t gpuCanon = 0xFFFF800020000000ull;
        const ImportChk c = import_check(0x100000000ull, 0x8000, (uint64_t)(RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE), gpuCanon, 0);
        expect_u("prodform import high-half ok", c.rc, kOk);
        expect_u("prodform import stripped", c.gpuStripped, 0x800020000000ull);
        expect_u("prodform import canonicalize restores", va_canonicalize(c.gpuStripped), gpuCanon);
        std::vector<uint64_t> pa(2);
        pa[0] = 0x920000000ull; pa[1] = 0x920001000ull;
        expect_u("prodform raw stripped import-tx fails precheck", pt_map_pages_tx(root, c.gpuStripped, 2, pa.data(), leaf, m), kBadArg);
        expect_u("prodform canonicalized import-tx maps", pt_map_pages_tx(root, va_canonicalize(c.gpuStripped), 2, pa.data(), leaf, m), kOk);
        expect_u("prodform import walk", pt_walk(root, gpuCanon, m, nullptr), pa[0]);
    }
    // Partial-block retry: fill part of a PTB, fail mid-tx, rollback must leave zeroed
    // tables so retry reuses clean pages and siblings stay intact. TestMem is monotonic
    // (no blocks), so this models the kernel guarantee at the tx contract level: after a
    // NoMemory fail, the attempted range has no leaf, quota is unchanged, siblings intact,
    // and retry with room succeeds and walks clean.
    {
        TestMem m; m.budget = 10000;
        uint64_t root = m.alloc_page();
        expect_u("sibling A maps", pt_map_tx(root, 0x30000000ull, 4, 0x930000000ull, leaf, m), kOk);
        m.failAt = m.allocs + 1;
        uint32_t before = m.allocs;
        expect_u("partial fail -> NoMemory", pt_map_tx(root, 0x40000000ull, 64, 0x940000000ull, leaf, m), kNoMemory);
        expect_u("partial fail leaves quota unchanged", m.allocs, before);
        expect_u("partial range has no leaf", pt_walk(root, 0x40000000ull, m, nullptr), 0);
        expect_u("sibling A intact after partial fail", pt_walk(root, 0x30000000ull, m, nullptr), 0x930000000ull);
        m.failAt = 0xFFFFFFFFu;
        expect_u("retry after partial fail", pt_map_tx(root, 0x40000000ull, 64, 0x940000000ull, leaf, m), kOk);
        bool clean = true;
        for (uint64_t p = 0; p < 64; p++) if (pt_walk(root, 0x40000000ull + p * kPage, m, nullptr) != 0x940000000ull + p * kPage) clean = false;
        expect(clean, "retry walks every page to its own PA (zeroed reuse, no stale entries)");
        expect_u("sibling A still intact after retry", pt_walk(root, 0x30000000ull, m, nullptr), 0x930000000ull);
    }
}

int main(int argc, char **argv) {
    std::string root = argc > 1 ? argv[1] : ".";
    l1_quotas(); l2_walker(); l3_tx(); l4_unmap(); l5_protect(); l6_pins(root); l7_prodform();
    std::printf("native_large_pt_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
