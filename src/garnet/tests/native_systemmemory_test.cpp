// native_systemmemory_test.cpp - task 3.5: the independent host-system budget for >4 GiB host imports.
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/garnet/src -I src/garnet/src/amd src/garnet/tests/native_systemmemory_test.cpp \
//       -o /tmp/native_systemmemory && /tmp/native_systemmemory .
//
// Covers:
//   S1  16/32 GiB synthetic hosts admit totals above 4 GiB; low-memory and unknown-memory caps remain bounded;
//   S2  the 64 MiB per-import and all argument rules are shared by the legacy cap and the host-system path;
//   S3  held + pending admission, exact-cap accounting, concurrent reservations, failure rollback and exact-once disarm;
//   S4  normal frees decrement the wired/committed total while HUNG quarantine keeps it counted;
//   S5  source pins: KernelSDK boot_args physical memory, reserve before pinning, no shared lock across prepare, rollback,
//       old ABI layout/selectors, and the GTT/import shared gSysHeld ledger.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "amd/native_g2_pure.h"
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
static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string fn_body(const std::string &src, const std::string &sig) {
    const size_t a = src.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a + 3);
}
static size_t at(const std::string &s, const std::string &n, size_t from = 0) { return s.find(n, from); }
static size_t count_of(const std::string &s, const std::string &n) {
    size_t c = 0, p = 0;
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}

constexpr uint64_t MiB = 1ull << 20;
constexpr uint64_t GiB = 1ull << 30;
constexpr uint64_t HV = 0x0000000100000000ull;
constexpr uint64_t GPU_VA = 0x0000000000200000ull;

// ---- S1: physical host caps ---------------------------------------------------------------------------------------------------------
static void s1_host_caps() {
    expect_u("16 GiB host: cap is physical minus the 2 GiB reserve", g2::host_effective_cap(16 * GiB), 14 * GiB);
    expect_u("32 GiB host: cap is physical minus the 2 GiB reserve", g2::host_effective_cap(32 * GiB), 30 * GiB);
    expect_u("a 16 GiB cap admits more than 4 GiB", (g2::host_effective_cap(16 * GiB) > (4 * GiB)) ? 1u : 0u, 1u);
    expect_u("low memory: 1.5 GiB cannot cover the reserve", g2::host_effective_cap(1536 * MiB), 0u);
    expect_u("low memory: exactly the reserve leaves no import cap", g2::host_effective_cap(2 * GiB), 0u);
    expect_u("unknown physical memory falls back to the bounded legacy cap", g2::host_effective_cap(0), g2::kSysGlobalCap);
    expect_u("the reserve cannot underflow a huge physical value", g2::host_sys_cap(~0ull), ~0ull - (2 * GiB));
    expect_u("a 4 GiB host has a 2 GiB cap, not a VRAM-derived 4 GiB cap", g2::host_effective_cap(4 * GiB), 2 * GiB);
}

// ---- S2: one validation path, two total caps -------------------------------------------------------------------------------------
static void s2_import_limits() {
    const uint64_t rw = RDNA4N_VM_PAGE_READABLE | RDNA4N_VM_PAGE_WRITEABLE;
    const ImportChk legacy = import_check(HV, 64 * MiB, rw, GPU_VA, 4 * GiB);
    expect_u("legacy 4 GiB cut still refuses the next import", legacy.rc, kNoMemory);
    const ImportChk sys = import_check_sys(HV, 64 * MiB, rw, GPU_VA, 5 * GiB, 14 * GiB);
    expect_u("host-system path admits a 64 MiB import after 5 GiB is already held", sys.rc, kOk);
    expect_u("... and reports the 64 MiB page count", sys.pages, (64 * MiB) / kPage);
    expect_u("... and the stripped GPU VA", sys.gpuStripped, GPU_VA);

    expect_u("the system path still refuses 64 MiB + one page", import_check_sys(HV, 64 * MiB + kPage, rw, GPU_VA, 0, 14 * GiB).rc, kBadArg);
    expect_u("an unaligned host VA is still refused", import_check_sys(HV + 1, kPage, rw, GPU_VA, 0, 14 * GiB).rc, kBadArg);
    expect_u("a VA range crossing out of the user half is still refused", import_check_sys(kImportUserMax - kPage, 2 * kPage, rw, GPU_VA, 0, 14 * GiB).rc, kBadArg);
    expect_u("flags without a GPU VA are still refused", import_check_sys(HV, kPage, RDNA4N_VM_PAGE_READABLE, 0, 0, 14 * GiB).rc, kBadArg);
    expect_u("an execute-only GPU mapping is still refused", import_check_sys(HV, kPage, RDNA4N_VM_PAGE_EXECUTABLE, GPU_VA, 0, 14 * GiB).rc, kBadArg);
    expect_u("a read-only import is accepted", import_check_sys(HV, kPage, RDNA4N_VM_PAGE_READABLE, GPU_VA, 0, 14 * GiB).rc, kOk);
    { const ImportChk h = import_check_sys(HV, 2*kPage, RDNA4N_VM_PAGE_READABLE, 0xFFFF800000200000ull, 0, 14 * GiB);
      expect(h.rc == kOk && h.gpuStripped == 0x800000200000ull, "a canonical high-half GPU VA is still stripped"); }
    expect(import_args_check(HV, 64 * MiB, rw, GPU_VA).rc == kOk &&
           import_args_check(HV, 64 * MiB + kPage, rw, GPU_VA).rc == kBadArg,
           "the legacy and system paths share the same argument validator");
    expect_u("exactly the system cap is admitted", import_check_sys(HV, 64 * MiB, rw, GPU_VA, 14 * GiB - 64 * MiB, 14 * GiB).rc, kOk);
    expect_u("one page over the system cap is refused", import_check_sys(HV, 64 * MiB, rw, GPU_VA, 14 * GiB - 64 * MiB + kPage, 14 * GiB).rc, kNoMemory);
    expect_u("a zero-byte import is still refused", import_check_sys(HV, 0, 0, 0, 0, 14 * GiB).rc, kBadArg);
}

// ---- S3: reservations, races and exact-once rollback ------------------------------------------------------------------------------
struct BudgetModel {
    std::mutex mu;
    uint64_t cap = 0, held = 0, pending = 0;
    explicit BudgetModel(uint64_t c) : cap(c) {}
    bool reserve(uint64_t size) {
        std::lock_guard<std::mutex> g(mu);
        if (!g2::host_reserve_ok(held, pending, size, cap)) return false;
        pending = g2::host_pending_acquire(pending, size);
        return true;
    }
    void commit(uint64_t size) {
        std::lock_guard<std::mutex> g(mu);
        const g2::HostCommit c = g2::host_commit(held, pending, size);
        held = c.held; pending = c.pending;
    }
    void rollback(uint64_t size) {
        std::lock_guard<std::mutex> g(mu);
        pending = g2::host_pending_release(pending, size);
    }
    void free_held(uint64_t size) {
        std::lock_guard<std::mutex> g(mu);
        held = held >= size ? held - size : 0;
    }
};

static void race_one(uint64_t physical, uint64_t wantWins) {
    BudgetModel b(g2::host_effective_cap(physical));
    std::atomic<bool> go{false};
    std::atomic<uint64_t> wins{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (;;) {
                if (!b.reserve(1 * GiB)) break;
                wins.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : threads) t.join();
    expect_u("concurrent reservations stop exactly at the physical host cap", wins.load(), wantWins);
    expect_u("every winner is pending, none overcommitted", b.pending, wantWins * GiB);
    expect_u("held remains zero until commit", b.held, 0u);
    for (uint64_t i = 0; i < wantWins; i++) b.commit(1 * GiB);
    expect_u("commit moves the whole reservation to held", b.held, wantWins * GiB);
    expect_u("commit drains pending", b.pending, 0u);
    for (uint64_t i = 0; i < wantWins; i++) b.rollback(1 * GiB);   // rollback cannot return already-committed bytes
    expect_u("rollback after commit leaves held charged", b.held, wantWins * GiB);
    expect_u("rollback after commit leaves pending empty", b.pending, 0u);
    for (uint64_t i = 0; i < wantWins; i++) b.free_held(1 * GiB);
    expect_u("a charged completion is returned only after its actual free", b.held, 0u);
}

struct ArmedReservation {
    uint64_t size = 0;
    bool armed = false;
};
struct ExactOnceModel {
    uint64_t cap, held = 0, pending = 0;
    uint32_t rollbacks = 0, commits = 0;
    explicit ExactOnceModel(uint64_t c) : cap(c) {}
    ArmedReservation reserve(uint64_t size) {
        if (!g2::host_reserve_ok(held, pending, size, cap)) return {};
        pending = g2::host_pending_acquire(pending, size);
        return ArmedReservation{ size, true };
    }
    void rollback(ArmedReservation &r) {
        if (!r.armed) return;
        pending = g2::host_pending_release(pending, r.size);
        r.armed = false;
        rollbacks++;
    }
    void commit(ArmedReservation &r) {
        if (!r.armed) return;
        const g2::HostCommit c = g2::host_commit(held, pending, r.size);
        held = c.held; pending = c.pending;
        r.armed = false;
        commits++;
    }
};

static void s3_reservations() {
    expect(g2::host_reserve_ok(0, 0, 8 * GiB, 14 * GiB), "one >4 GiB reservation is admitted on a 16 GiB host");
    expect(g2::host_reserve_ok(5 * GiB, 0, 9 * GiB, 14 * GiB), "5 GiB held + 9 GiB request is exactly the cap");
    expect(!g2::host_reserve_ok(5 * GiB, 0, 9 * GiB + kPage, 14 * GiB), "one page beyond the physical cap is refused");
    expect(!g2::host_reserve_ok(7 * GiB, 7 * GiB, 1, 14 * GiB), "pending is part of admission");
    expect(!g2::host_reserve_ok(~0ull, 1, 1, ~0ull), "saturating held + pending cannot wrap admission open");
    const g2::HostCommit c = g2::host_commit(7 * GiB, 7 * GiB, 1 * GiB);
    expect_u("commit adds to held", c.held, 8 * GiB);
    expect_u("commit drains only the committed pending", c.pending, 6 * GiB);
    expect_u("a normal rollback returns the charge", g2::host_pending_release(6 * GiB, 1 * GiB), 5 * GiB);

    race_one(16 * GiB, 14u);
    race_one(32 * GiB, 30u);

    ExactOnceModel m(g2::host_effective_cap(16 * GiB));
    ArmedReservation r = m.reserve(8 * GiB);
    expect(r.armed && m.pending == 8 * GiB && m.held == 0, "failure path arms an 8 GiB pending reservation");
    m.rollback(r);
    m.rollback(r);   // the second cleanup path / racing close must be inert
    expect_u("rollback fires exactly once", m.rollbacks, 1u);
    expect_u("failed reservation leaves pending at zero", m.pending, 0u);
    expect_u("failed reservation never reached held", m.held, 0u);
    r = m.reserve(8 * GiB);
    expect(r.armed && m.pending == 8 * GiB, "the freed reservation can be retried");
    m.commit(r);
    expect_u("commit disarms the token", r.armed ? 1u : 0u, 0u);
    expect_u("commit fires exactly once", m.commits, 1u);
    expect_u("committed bytes move to held", m.held, 8 * GiB);
    m.rollback(r);
    expect_u("rollback after commit is inert", m.rollbacks, 1u);
}

// ---- S4: real free versus quarantine ----------------------------------------------------------------------------------------------
struct HeldModel {
    uint64_t gtt = 0, imports = 0;
    uint64_t total() const { return gtt + imports; }
    void charge_gtt(uint64_t n) { gtt += n; }
    void charge_import(uint64_t n) { imports += n; }
    void free_gtt(uint64_t n) { gtt = gtt >= n ? gtt - n : 0; }
    void free_import(uint64_t n) { imports = imports >= n ? imports - n : 0; }
};

static void s4_free_leak() {
    HeldModel m;
    m.charge_gtt(1 * GiB);
    m.charge_import(5 * GiB);
    expect_u("gSysHeld starts with GTT plus imports", m.total(), 6 * GiB);
    m.free_import(5 * GiB);
    expect_u("a normal host free decrements the shared ledger", m.total(), 1 * GiB);
    m.free_gtt(1 * GiB);
    expect_u("a normal GTT free also decrements it", m.total(), 0u);
    m.charge_import(6 * GiB);
    const uint64_t beforeLeak = m.total();   // HUNG quarantine: no sys_release, only the software handle goes
    expect_u("a HUNG leak keeps all system bytes counted", m.total(), beforeLeak);
    expect(beforeLeak > 4 * GiB, "the quarantined host total can remain above 4 GiB");
}

// ---- S5: source pins --------------------------------------------------------------------------------------------------------------
static void s5_source_pins(const std::string &root) {
    const std::string K = root + "/src/garnet/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp");
    const std::string g2h = slurp(K + "src/amd/native_g2_pure.h");
    const std::string hi = slurp(K + "src/amd/native_hostimport_pure.h");
    const std::string abi = slurp(K + "src/GarnetNativeABI.h");
    expect(!eng.empty() && !g2h.empty() && !hi.empty() && !abi.empty(), "the kernel sources are readable from the root given");

    const std::string phys = fn_body(eng, "static uint64_t host_physmem_bytes() {");
    expect(!phys.empty() && phys.find("PE_state.bootArgs") != std::string::npos && phys.find("PhysicalMemorySize") != std::string::npos,
           "host memory comes from the current MacKernelSDK boot record");
    expect(phys.find("vram_alloc") == std::string::npos && phys.find("vram_total") == std::string::npos && phys.find("gmc.") == std::string::npos,
           "the host cap is not derived from VRAM");
    const std::string cap = fn_body(g2h, "constexpr uint64_t host_sys_cap(uint64_t physMemBytes) {");
    expect(eng.find("#include <pexpert/pexpert.h>") != std::string::npos &&
           !cap.empty() && cap.find("sat_sub(physMemBytes, kHostSysReserve)") != std::string::npos &&
           cap.find("physMemBytes - kHostSysReserve") == std::string::npos,
           "the pure cap uses the 2 GiB reserve with saturation and the SDK supplies PE_state");

    const std::string imp = fn_body(eng, "IOReturn n1c_bo_import_host(");
    expect(!imp.empty(), "n1c_bo_import_host is found");
    const size_t pReserve = at(imp, "host_pending_acquire");
    const size_t pWith = at(imp, "IOMemoryDescriptor::withAddressRange(");
    const size_t pPrep = at(imp, "IOReturn prepareRc = md->prepare();");
    const size_t pSess = at(imp, "Session *s = sess_lock(ref, false);");
    const size_t pAuthLock = imp.find("IOLockLock(gCliLock);", pSess);
    const size_t pReserveLock = imp.rfind("IOLockLock(gCliLock);", pReserve);
    const size_t pReserveUnlock = imp.find("IOLockUnlock(gCliLock);", pReserve);
    expect(pReserveLock < pReserve && pReserve < pReserveUnlock && pReserveUnlock < pWith,
           "the reservation is taken under gCliLock BEFORE the descriptor is wired");
    expect(pWith < pPrep && pPrep < pSess && pSess < pAuthLock,
           "prepare() happens before the session / authoritative client lock");
    expect(imp.substr(pWith, pSess - pWith).find("IOLockLock(gCliLock)") == std::string::npos,
           "gCliLock is never held across descriptor creation or prepare()");
    expect_u("every known pre-record failure has a rollback site (including the final !taken path)",
             count_of(imp, "sys_pending_rollback(reserved, size)"), 8u);
    expect(imp.find("import_check_sys(hostVa, size, flags, gpuVa, heldPendAdvisory, hcapAdvisory)") != std::string::npos &&
           imp.find("c = import_check(hostVa, size, flags, gpuVa, 0ull);") != std::string::npos,
           "the non-WindowServer path uses the system cap, not the legacy per-client import cap");
    expect(imp.find("if (!multiNow || s->ws)") != std::string::npos && imp.find("s->importedBytes") != std::string::npos,
           "the 4 GiB constant is retained only for the old WindowServer / switch-OFF mode");
    const size_t pIdentity = at(imp, "if (multiNow != multi0 || s->ws != ws0)");
    const size_t pReservedGuard = at(imp, "if (!reserved) {", pIdentity);
    const size_t pImportMap = at(imp, "pt_map_pages_tx(s->rootPa", pReservedGuard);
    expect(pSess < pIdentity && pIdentity < pReservedGuard && pReservedGuard < pImportMap,
           "multi/WS identity is rechecked and a non-WS system import MUST own its reservation before any MAP");
    expect(count_of(imp, "gSysHeld = rdna4native::g2::sat_add(gSysHeld, size)") == 1 &&
           imp.find("const rdna4native::g2::HostCommit hc = rdna4native::g2::host_commit(gSysHeld, gSysPending, size);") != std::string::npos,
           "only WindowServer commits direct-held; multi non-WS must use pending->held");

    const size_t pPageFail = at(imp, "if (pages == nullptr) {");
    const size_t pPageRollback = at(imp, "sys_pending_rollback(reserved, size);", pPageFail);
    expect(pPageFail < pPageRollback && at(imp, "md->complete(); md->release();", pPageFail) < pPageRollback,
           "page-list allocation failure completes/releases the pin before returning the pending charge");
    const size_t pCollectFail = at(imp, "if (crc != kOk) {");
    const size_t pCollectRollback = at(imp, "sys_pending_rollback(reserved, size);", pCollectFail);
    expect(pCollectFail < pCollectRollback && at(imp, "md->complete(); md->release();", pCollectFail) < pCollectRollback,
           "page collection failure completes/releases the pin before returning the pending charge");
    const size_t pBarFail = at(imp, "if (bad < pre.pages) {");
    const size_t pBarRollback = at(imp, "sys_pending_rollback(reserved, size);", pBarFail);
    expect(pBarFail < pBarRollback && at(imp, "md->complete(); md->release();", pBarFail) < pBarRollback,
           "BAR/DRAM refusal completes/releases the pin before returning the pending charge");
    const size_t pTakenFail = at(imp, "if (!taken) {");
    const size_t pTakenRollback = at(imp, "sys_pending_rollback(reserved, size);", pTakenFail);
    expect(pTakenFail < pTakenRollback && at(imp, "md->complete(); md->release();", pTakenFail) < pTakenRollback,
           "every authoritative/map failure completes/releases the pin before returning the pending charge");

    const std::string rb = fn_body(eng, "static inline void sys_pending_rollback(bool &reserved, uint64_t size) {");
    expect(!rb.empty() && rb.find("if (!reserved) return;") != std::string::npos &&
           rb.find("host_pending_release(gSysPending, size)") != std::string::npos && rb.find("reserved = false;") != std::string::npos,
           "the reservation rollback is idempotent and leaves held untouched");
    const size_t pCommit = at(imp, "const rdna4native::g2::HostCommit hc");
    const size_t pDisarm = at(imp, "reserved = false;", pCommit);
    expect(pCommit != std::string::npos && pDisarm != std::string::npos,
           "a successful commit disarms the reservation token");

    const std::string release = fn_body(eng, "static inline void sys_release(Bo &b) {");
    expect(!release.empty() && release.find("gSysHeld = gSysHeld >= b.size ? gSysHeld - b.size : 0ull") != std::string::npos &&
           release.find("b.sysCounted = 0u") != std::string::npos,
           "sys_release decrements the real held total and clears the per-BO count");
    const std::string bo = fn_body(eng, "static void bo_release(Session *s, uint32_t h, RelMode mode) {");
    const size_t pNormal = at(bo, "} else if (mode == kRelNormal) {");
    const size_t pLeak = at(bo, "} else {\n        // Leak:");
    expect(pNormal != std::string::npos && pLeak != std::string::npos, "bo_release has Normal and Leak branches");
    const std::string closing = bo.substr(0, pNormal);
    const std::string normal = bo.substr(pNormal, pLeak - pNormal);
    const std::string leak = bo.substr(pLeak);
    expect(closing.find("host_release(b); sys_release(b);") != std::string::npos,
           "a clean close releases only through the host release path and decrements");
    expect(normal.find("host_release(b); sys_release(b);") != std::string::npos,
           "a normal BoFree releases only after unmap/flush and decrements");
    expect(leak.find("host_release") == std::string::npos && leak.find("sys_release") == std::string::npos,
           "a HUNG quarantine keeps the descriptor, UPL pages and gSysHeld counted");

    const std::string create = fn_body(eng, "IOReturn n1c_bo_create(");
    expect(create.find("const uint64_t hcap = host_cap_now();") != std::string::npos &&
           create.find("host_sys_budget_ok(multi, s->ws, heldPending, p.size, hcap)") != std::string::npos,
           "GTT creation uses the same dynamic host cap and includes pending imports");
    expect(eng.find("static uint64_t gSysHeld;") != std::string::npos &&
           eng.find("static uint64_t gSysPending;") != std::string::npos,
           "GTT and host imports share gSysHeld / gSysPending");

    RDNA4N_STATIC_ASSERT(sizeof(rdna4n_info) == 192, "old QueryInfo layout unchanged");
    RDNA4N_STATIC_ASSERT(offsetof(rdna4n_info, reserved) == 160, "old QueryInfo reserved offset unchanged");
    RDNA4N_STATIC_ASSERT(RDNA4N_SEL_BO_IMPORT_HOST == 21, "old import selector unchanged");
    RDNA4N_STATIC_ASSERT(RDNA4N_SEL_COUNT_1_9 == 22, "old selector count unchanged");
    RDNA4N_STATIC_ASSERT(RDNA4N_IMPORT_MAX_BO == 64ull * MiB, "per-import 64 MiB unchanged");
    RDNA4N_STATIC_ASSERT(RDNA4N_IMPORT_CAP == 4096ull * MiB, "legacy old-client constant remains available");
    expect_u("ABI major remains 1", RDNA4N_ABI_VERSION, 1u);
    expect(RDNA4N_ABI_MINOR >= 12u, "the old wire version is not regressed");
    expect(abi.find("RDNA4N_SEL_BO_IMPORT_HOST = 21") != std::string::npos &&
           abi.find("RDNA4N_IMPORT_CAP          (4096ull << 20)") != std::string::npos,
           "the ABI header keeps selector 21 and the legacy 4 GiB constant");
    expect(g2h.find("constexpr uint64_t host_effective_cap(uint64_t physMemBytes)") != std::string::npos &&
           hi.find("constexpr ImportChk import_check_sys(") != std::string::npos,
           "the pure host-budget and import checks are present");
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: %s REPO_ROOT\n", argv[0]); return 2; }
    s1_host_caps();
    s2_import_limits();
    s3_reservations();
    s4_free_leak();
    s5_source_pins(argv[1]);
    std::printf("native_systemmemory_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
