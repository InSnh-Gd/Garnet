// Behavioral checks for the DCN transaction boundaries around BO teardown and
// kext shutdown. The Makefile extracts the real function bodies from dcn.cpp;
// the hardware-facing calls below are fakes.
#include <cstdint>
#include <cstdio>

#define RDNA4LOG(...) ((void)0)

static int gRun;
static int gFail;

static void expect(bool ok, const char *what) {
	gRun++;
	if (!ok) {
		gFail++;
		std::printf("FAIL: %s\n", what);
	}
}

struct FakeLock {};
using IOLock = FakeLock;
using wait_result_t = int;
static FakeLock gLock;
static uint32_t gSleepCalls;
static bool gSleepHookPersistent;
static uint32_t gLockCalls;

static void IOLockLock(IOLock *) { gLockCalls++; }
static void IOLockUnlock(IOLock *) {}
static void (*gSleepHook)();
static void IOSleep(uint32_t) {
	gSleepCalls++;
	if (gSleepHook) {
		void (*hook)() = gSleepHook;
		if (!gSleepHookPersistent) gSleepHook = nullptr;
		hook();
	}
}

namespace rdna4peer {
static constexpr uint32_t kLeaseLocalDisplay = 5;
}

static bool gParentHeld;
static bool gLeaseHeld;
static bool gTxnAllowed = true;
static uint32_t gTxnBegins;
static uint32_t gTxnEnds;

namespace rdna4displaytxn {
static bool held() { return gParentHeld || gLeaseHeld; }
}  // namespace rdna4displaytxn

struct rdna4_display_txn {
	bool ok { false };
	explicit rdna4_display_txn(uint32_t) {
		gTxnBegins++;
		ok = gTxnAllowed;
		if (ok) gLeaseHeld = true;
	}
	~rdna4_display_txn() {
		if (ok) {
			gTxnEnds++;
			gLeaseHeld = false;
		}
	}
	rdna4_display_txn(const rdna4_display_txn &) = delete;
	rdna4_display_txn &operator=(const rdna4_display_txn &) = delete;
};

namespace rdna4scan {
static constexpr uint32_t kMaxSlots = 3u;
static constexpr uint32_t kOk = 0u;
static constexpr uint32_t kBusy = 7u;
struct Slot {
	uint64_t mc;
	uint64_t bytes;
	bool used;
};
struct Table {
	Slot s[kMaxSlots] {};
};
struct UnpinPlan {
	uint8_t dropMask;
	bool fullRelease;
};
static UnpinPlan unpin_plan(const Table &, uint8_t, bool, uint64_t, bool) {
	return UnpinPlan { 0u, false };
}
static bool slot_unregister(Table &, uint32_t, uint64_t, bool) { return true; }
static constexpr uint32_t kGeomOk = 0u;
static uint32_t geom_check(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool) { return kGeomOk; }
static bool idle_expired(uint64_t, uint64_t) { return false; }
}  // namespace rdna4scan

struct dcn41_state {};
struct DcnGlobals {
	dcn41_state d;
};
static DcnGlobals gDcn;

static uint32_t gRestoreCalls;
static uint32_t gMtShutdownCalls;
static uint32_t gXReleaseCalls;
static uint32_t gXBoGoneCalls;
static uint32_t gXWatchdogSteps;
static bool gXWatchdogStepResult;
static uint32_t gMtWdAlive;
static uint32_t gMtHoldAlive;
static uint32_t gScanPollCalls;
static uint32_t gScanGeomCalls;
static bool gScanActive;
static uint32_t gMtWatchdogBodies;
static constexpr int DCN41_OK = 0;

namespace rdna4mt {
struct Hw {};
struct State {};
struct Timing {};
static void watchdog_body(const Hw &, State &, const Timing &, uint32_t) {
	gMtWatchdogBodies++;
}
}  // namespace rdna4mt

namespace rdna4dcn {
struct SXEnv;
}

namespace rdna4scanx {
struct XDesc {
	uint32_t inst;
	const char *name;
};
struct ScanX {
	bool acquired;
	uint32_t gen;
	volatile uint32_t watchdogAlive;
};
static constexpr XDesc kMonA { 1u, "monitor A" };
static constexpr XDesc kMonB { 2u, "monitor B" };
static constexpr uint32_t kWatchMs = 50u;
static inline const XDesc *desc_of(uint32_t inst) {
	return inst == 1u ? &kMonA : inst == 2u ? &kMonB : nullptr;
}
static uint32_t bo_gone(rdna4dcn::SXEnv &, const XDesc &, ScanX &, uint32_t, uint32_t, bool, uint64_t *) {
	gXBoGoneCalls++;
	return 0u;
}
static uint32_t restore(rdna4dcn::SXEnv &, const XDesc &, ScanX &, const char *, uint32_t, uint64_t *out) {
	gXReleaseCalls++;
	if (out) { out[0] = 1u; out[1] = 0u; }
	return rdna4scan::kOk;
}
static bool watchdog_step(rdna4dcn::SXEnv &, const XDesc &, ScanX &, uint32_t) {
	gXWatchdogSteps++;
	return gXWatchdogStepResult;
}
}  // namespace rdna4scanx

namespace rdna4dcn {
struct SXEnv {
	explicit SXEnv(const rdna4scanx::XDesc &) {}
};
struct ScanGeom {
	uint32_t w, h, pitchPx, fmt, sw;
	bool dcc;
};
static rdna4mt::Hw kMtHw;
static rdna4mt::State gMt;
static rdna4mt::Timing mt_timing() { return rdna4mt::Timing {}; }
struct ScanGlobals {
	IOLock *lock;
	bool acquired;
	uint32_t gen;
	bool restoreFailed;
	bool restoring;
	bool wantRestore;
	const char *wantWhy;
	uint32_t width, height, pitchPx;
	uint64_t lastActivityNs;
	uint64_t wdRestores;
	rdna4scan::Table tbl;
	volatile uint32_t watchdogAlive;
};
static ScanGlobals gScan;
static rdna4scanx::ScanX gSX[3];
static IOLock *gSXLock[3];

static bool sx_latched_inst(uint32_t inst) { return inst == 1u || inst == 2u; }
static bool scanActive() { return gScanActive; }
static uint64_t now_ns() { return 1000u; }
static void scan_poll_locked(bool) { gScanPollCalls++; }
static void scan_read_geom(ScanGeom *g) {
	gScanGeomCalls++;
	*g = ScanGeom { gScan.width, gScan.height, gScan.pitchPx, 0u, 0u, false };
}
static int dcn41_hubp_is_flip_pending(dcn41_state *, uint32_t, bool *, uint64_t *) { return 0; }
static uint32_t scan_restore(const char *, bool, uint32_t, uint64_t *) {
	gRestoreCalls++;
	return 0u;
}
static void mt_shutdown() { gMtShutdownCalls++; }
void scanXShutdown();

#include "dcn_txn_boundaries_actual.inc"

}  // namespace rdna4dcn

static void reset_world() {
	gParentHeld = false;
	gLeaseHeld = false;
	gTxnAllowed = true;
	gTxnBegins = gTxnEnds = 0;
	gRestoreCalls = gMtShutdownCalls = gXReleaseCalls = gXBoGoneCalls = gXWatchdogSteps = 0;
	gXWatchdogStepResult = false;
	gMtWdAlive = gMtHoldAlive = gSleepCalls = 0;
	gScanPollCalls = gScanGeomCalls = 0;
	gScanActive = false;
	gMtWatchdogBodies = 0;
	gLockCalls = 0;
	gSleepHook = nullptr;
	gSleepHookPersistent = false;
	rdna4dcn::gScan = rdna4dcn::ScanGlobals {};
	rdna4dcn::gScan.lock = &gLock;
	rdna4dcn::gSX[1] = rdna4scanx::ScanX {};
	rdna4dcn::gSX[2] = rdna4scanx::ScanX {};
	rdna4dcn::gSXLock[1] = nullptr;
	rdna4dcn::gSXLock[2] = nullptr;
}

static void bo_gone_checks() {
	reset_world();
	gTxnAllowed = false;
	uint64_t out[2] = { 11u, 22u };
	expect(rdna4dcn::scanBoGone(1u, 7u, true, out) == 2u,
	       "a DP BO teardown without a transaction is reported unverified");
	expect(out[0] == 0u && out[1] == 0u,
	       "a refused DP BO teardown clears both restore outputs");
	expect(gRestoreCalls == 0u, "a refused DP BO teardown does not attempt a restore");

	reset_world();
	gTxnAllowed = false;
	rdna4dcn::gSXLock[2] = &gLock;
	out[0] = 11u;
	out[1] = 22u;
	expect(rdna4dcn::scanXBoGone(2u, 1u, 3u, true, out) == 2u,
	       "an HDMI BO teardown without a transaction is reported unverified");
	expect(out[0] == 0u && out[1] == 0u,
	       "a refused HDMI BO teardown clears both restore outputs");
	expect(gXBoGoneCalls == 0u, "a refused HDMI BO teardown does not enter the scanx flow");

	reset_world();
	rdna4dcn::gSXLock[2] = &gLock;
	expect(rdna4dcn::scanXBoGone(2u, 1u, 3u, true, out) == 0u,
	       "an admitted HDMI BO teardown reaches the scanx flow");
	expect(gXBoGoneCalls == 1u && gTxnBegins == 1u && gTxnEnds == 1u,
	       "the admitted HDMI BO teardown owns exactly one transaction");
}

static void shutdown_checks() {
	reset_world();
	gParentHeld = true;
	rdna4dcn::gSXLock[1] = &gLock;
	uint64_t releaseOut[2] = { 0u, 0u };
	expect(rdna4dcn::scanXRelease(1u, "parent stop", releaseOut) == rdna4scan::kOk,
	       "scanXRelease runs its restore under a same-thread parent transaction");
	expect(gTxnBegins == 0u && gXReleaseCalls == 1u,
	       "scanXRelease does not attempt a different-kind nested transaction");

	reset_world();
	rdna4dcn::gSXLock[1] = &gLock;
	expect(rdna4dcn::scanXRelease(1u, "standalone", releaseOut) == rdna4scan::kOk,
	       "standalone scanXRelease reaches the restore");
	expect(gTxnBegins == 1u && gTxnEnds == 1u && gXReleaseCalls == 1u,
	       "standalone scanXRelease owns exactly one transaction");

	reset_world();
	gTxnAllowed = false;
	rdna4dcn::gSXLock[1] = &gLock;
	expect(rdna4dcn::scanXRelease(1u, "refused", releaseOut) == rdna4scan::kBusy,
	       "refused standalone scanXRelease reports busy");
	expect(gTxnBegins == 1u && gTxnEnds == 0u && gXReleaseCalls == 0u,
	       "refused standalone scanXRelease performs no restore");

	reset_world();
	gParentHeld = true;
	rdna4dcn::gSXLock[1] = &gLock;
	rdna4dcn::scanShutdown();
	expect(gTxnBegins == 0u, "scanShutdown rides a same-thread parent lease without begin()");
	expect(gMtShutdownCalls == 1u && gXReleaseCalls == 1u && gRestoreCalls == 1u,
	       "scanShutdown completes the mode-trial, HDMI and DP shutdown bodies under the parent lease");

	reset_world();
	rdna4dcn::gSXLock[1] = &gLock;
	rdna4dcn::scanShutdown();
	expect(gTxnBegins == 1u && gTxnEnds == 1u,
	       "scanShutdown starts and releases one LocalDisplay transaction when no parent exists");
	expect(gMtShutdownCalls == 1u && gXReleaseCalls == 1u && gRestoreCalls == 1u,
	       "scanShutdown completes its shutdown bodies under the LocalDisplay transaction");

	reset_world();
	gTxnAllowed = false;
	rdna4dcn::gSXLock[1] = &gLock;
	rdna4dcn::scanShutdown();
	expect(gTxnBegins == 1u && gTxnEnds == 0u,
	       "scanShutdown attempts exactly one transaction when none is held");
	expect(gMtShutdownCalls == 0u && gXReleaseCalls == 0u && gRestoreCalls == 0u,
	       "scanShutdown performs no shutdown work when its transaction is refused");

	reset_world();
	gParentHeld = true;
	rdna4dcn::gSXLock[2] = &gLock;
	rdna4dcn::scanXShutdown();
	expect(gTxnBegins == 0u && gXReleaseCalls == 1u,
	       "scanXShutdown rides a same-thread parent lease and still releases HDMI state");

	reset_world();
	rdna4dcn::gSXLock[2] = &gLock;
	rdna4dcn::scanXShutdown();
	expect(gTxnBegins == 1u && gTxnEnds == 1u && gXReleaseCalls == 1u,
	       "standalone scanXShutdown owns exactly one transaction");

	reset_world();
	gTxnAllowed = false;
	rdna4dcn::gSXLock[2] = &gLock;
	rdna4dcn::scanXShutdown();
	expect(gTxnBegins == 1u && gTxnEnds == 0u && gXReleaseCalls == 0u,
	       "scanXShutdown performs no HDMI release when its transaction is refused");
}

static void shutdown_barrier_checks() {
	reset_world();
	rdna4dcn::wait_for_dcn_threads_quiescent();
	expect(gSleepCalls == 0u, "the shutdown barrier returns immediately when no worker is alive");

	reset_world();
	gMtWdAlive = 1u;
	gMtHoldAlive = 1u;
	rdna4dcn::gScan.watchdogAlive = 1u;
	rdna4dcn::gSX[1].watchdogAlive = 1u;
	rdna4dcn::gSX[2].watchdogAlive = 1u;
	gSleepHookPersistent = true;
	gSleepHook = [] {
		if (gMtWdAlive) gMtWdAlive--;
		else if (gMtHoldAlive) gMtHoldAlive--;
		else if (rdna4dcn::gScan.watchdogAlive) rdna4dcn::gScan.watchdogAlive--;
		else if (rdna4dcn::gSX[1].watchdogAlive) rdna4dcn::gSX[1].watchdogAlive--;
		else if (rdna4dcn::gSX[2].watchdogAlive) rdna4dcn::gSX[2].watchdogAlive--;
	};
	rdna4dcn::wait_for_dcn_threads_quiescent();
	expect(gMtWdAlive == 0u && gMtHoldAlive == 0u && rdna4dcn::gScan.watchdogAlive == 0u &&
	       rdna4dcn::gSX[1].watchdogAlive == 0u && rdna4dcn::gSX[2].watchdogAlive == 0u,
	       "the shutdown barrier does not return until every DCN worker is gone");
}

static void watchdog_checks() {
	reset_world();
	gTxnAllowed = false;
	gMtWdAlive = 1u;
	rdna4dcn::mt_watchdog(nullptr, 0);
	expect(gMtWatchdogBodies == 0u && gMtWdAlive == 0u,
	       "a transaction-refused mode watchdog performs no body and clears its alive count");

	reset_world();
	gMtWdAlive = 1u;
	rdna4dcn::mt_watchdog(nullptr, 0);
	expect(gTxnBegins == 1u && gTxnEnds == 1u && gMtWatchdogBodies == 1u && gMtWdAlive == 0u,
	       "an admitted mode watchdog owns one transaction and publishes its exit");

	reset_world();
	gTxnAllowed = false;
	rdna4dcn::gSX[2].acquired = true;
	rdna4dcn::gSX[2].gen = 9u;
	rdna4dcn::gSX[2].watchdogAlive = 1u;
	rdna4dcn::gSXLock[2] = &gLock;
	gSleepHook = [] { rdna4dcn::gSX[2].acquired = false; };
	rdna4dcn::sx_watchdog((void *)((2ull << 32) | 9ull), 0);
	expect(gTxnBegins != 0u && gXWatchdogSteps == 0u,
	       "a transaction-refused HDMI watchdog performs no watchdog step");
	expect(gXWatchdogSteps == 0u && rdna4dcn::gSX[2].watchdogAlive == 0u,
	       "an HDMI watchdog exits after stop clears its acquisition despite the parent Stop lease");

	reset_world();
	gXWatchdogStepResult = true;
	rdna4dcn::gSX[2].acquired = true;
	rdna4dcn::gSX[2].gen = 10u;
	rdna4dcn::gSX[2].watchdogAlive = 1u;
	rdna4dcn::gSXLock[2] = &gLock;
	rdna4dcn::sx_watchdog((void *)((2ull << 32) | 10ull), 0);
	expect(gXWatchdogSteps == 1u && gTxnBegins == 1u && gTxnEnds == 1u,
	       "an admitted HDMI watchdog step runs under exactly one transaction");
	expect(rdna4dcn::gSX[2].watchdogAlive == 0u,
	       "the admitted HDMI watchdog publishes its exit for the stop join");
}

static void scan_watchdog_checks() {
	reset_world();
	gTxnAllowed = false;
	gScanActive = true;
	rdna4dcn::gScan.lock = &gLock;
	rdna4dcn::gScan.acquired = true;
	rdna4dcn::gScan.gen = 7u;
	rdna4dcn::gScan.watchdogAlive = 1u;
	gSleepHook = [] { gScanActive = false; };
	rdna4dcn::scan_watchdog((void *)7u, 0);
	expect(gTxnBegins != 0u && gScanPollCalls == 0u && gScanGeomCalls == 0u && gRestoreCalls == 0u && gLockCalls == 0u,
	       "a transaction-refused scan watchdog performs no register read or restore");
	expect(rdna4dcn::gScan.watchdogAlive == 0u,
	       "a transaction-refused scan watchdog clears its alive count before return");

	reset_world();
	gScanActive = true;
	rdna4dcn::gScan.lock = &gLock;
	rdna4dcn::gScan.acquired = true;
	rdna4dcn::gScan.gen = 8u;
	rdna4dcn::gScan.wantRestore = true;
	rdna4dcn::gScan.watchdogAlive = 1u;
	rdna4dcn::scan_watchdog((void *)8u, 0);
	expect(gTxnBegins == 1u && gTxnEnds == 1u,
	       "an admitted scan watchdog round owns one transaction");
	expect(gScanPollCalls == 1u && gScanGeomCalls == 1u && gRestoreCalls == 1u,
	       "the scan watchdog holds that transaction across poll, geometry read and restore");
	expect(rdna4dcn::gScan.watchdogAlive == 0u,
	       "the admitted scan watchdog publishes its exit for the stop join");
}

int main() {
	bo_gone_checks();
	shutdown_checks();
	shutdown_barrier_checks();
	watchdog_checks();
	scan_watchdog_checks();
	std::printf("dcn_txn_boundary_test: %d checks, %d failed\n", gRun, gFail);
	return gFail == 0 ? 0 : 1;
}
