// Behavioral contract test for the real ihEntry() body in dcn/dcn.cpp.
// The Makefile extracts only that function; everything it calls is a fake
// here.  A successful call must hold the local-display lease while the
// watchdog restore, scanIrq(), fallback ACK and storm disable run.  A refused
// lease must consume the entry without any of those paths or ACK statistics.
#include "dcn/dcn41_vbl_irq_pure.h"
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

static constexpr int DCN41_OK = 0;
static constexpr uint32_t DCN41_IH_CLIENT_DCE = 4u;
static constexpr uint32_t DCN41_NUM_PIPES = 4u;
static constexpr uint8_t DCN41_IRQ_NONE = 0;
static constexpr uint8_t DCN41_IRQ_VSTARTUP = 4;
static constexpr uint8_t DCN41_IRQ_VUPDATE_NO_LOCK = 6;
static constexpr uint8_t DCN41_IRQ_KIND_COUNT = 8;

struct dcn41_irq {
	uint8_t kind;
	uint8_t inst;
};

namespace rdna4peer {
static constexpr uint32_t kLeaseLocalDisplay = 5;
}

static bool gTxnAllowed = true;
static bool gLeaseActive = false;
static uint32_t gTxnBegins;
static uint32_t gTxnEnds;

struct rdna4_display_txn {
	bool ok;
	explicit rdna4_display_txn(uint32_t) {
		gTxnBegins++;
		ok = gTxnAllowed;
		gLeaseActive = ok;
	}
	~rdna4_display_txn() {
		if (ok) {
			gTxnEnds++;
			gLeaseActive = false;
		}
	}
	rdna4_display_txn(const rdna4_display_txn &) = delete;
	rdna4_display_txn &operator=(const rdna4_display_txn &) = delete;
};

struct DcnState {
	bool armed { false };
	int d {};
	uint64_t dceEntries { 0 };
	uint64_t dceByKind[DCN41_IRQ_KIND_COUNT] {};
	uint64_t dceVupdate[DCN41_NUM_PIPES] {};
	uint64_t dceVstartup[DCN41_NUM_PIPES] {};
	uint64_t dceUnclassified { 0 };
	uint64_t ackFailures { 0 };
	uint64_t irqRefusals { 0 };
	uint32_t irqRefusalLogs { 0 };
	uint64_t firstEntryNs { 0 };
	uint64_t lastEntryNs { 0 };
	uint32_t logBudget { 0 };
	bool flipHeld { false };
	uint32_t heldFrames { 0 };
	uint32_t heldFrameCap { 0 };
	uint64_t watchdogRestores { 0 };
	bool srcEnabled { false };
	bool stormTripped { false };
	uint8_t srcKind { 0 };
	uint8_t srcInst { 0 };
	uint64_t entriesAtEnable { 0 };
	bool primaryVblEnabled { false };
	uint8_t primaryVblOtg { 0 };
	bool irqAckRecoveryNeeded { false };
	rdna4vbl::RateStormGuard primaryVblStorm {};
	const char *tag { "?" };
};

static DcnState gDcn;
static struct {
	uint32_t active;
} gScan;

static uint32_t gScanCalls;
static uint32_t gWatchdogCalls;
static uint32_t gAckCalls;
static uint32_t gIrqSetCalls;
static uint32_t gPrimaryVblCalls;
static uint32_t gPrimaryVblLastOtg;
static uint64_t gFakeNowNs = 42u;
static struct dcn41_irq gFakeIrq { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };

static uint64_t now_ns() { return gFakeNowNs; }

static void garnet_primary_fb_notify_vbl(uint32_t otg) {
	gPrimaryVblCalls++;
	gPrimaryVblLastOtg = otg;
	expect(!gLeaseActive, "primary FB VBL notification runs AFTER releasing the display lease");
}

static struct dcn41_irq dcn41_ih_to_irq(uint32_t, uint32_t, uint32_t) {
	return gFakeIrq;
}

static void flip_restore(const char *) {
	gWatchdogCalls++;
	expect(gLeaseActive, "watchdog plane restore runs under the acquired lease");
	gDcn.flipHeld = false;
}

static bool scanIrq(const struct dcn41_irq &, uint64_t, int *ar) {
	gScanCalls++;
	expect(gLeaseActive, "scanIrq runs under the acquired lease");
	*ar = DCN41_OK;
	return true;
}

static int dcn41_irq_ack(int *, struct dcn41_irq) {
	gAckCalls++;
	expect(gLeaseActive, "fallback DCN ACK runs under the acquired lease");
	return DCN41_OK;
}

static int dcn41_irq_set(int *, struct dcn41_irq, bool) {
	gIrqSetCalls++;
	expect(gLeaseActive, "storm-guard source disable runs under the acquired lease");
	return DCN41_OK;
}

static constexpr uint64_t kDceStormCap = 200000u;

#include "dcn_irq_actual.inc"

static void reset_world() {
	gDcn = DcnState {};
	gScan.active = 0;
	gScanCalls = gWatchdogCalls = gAckCalls = gIrqSetCalls = 0;
	gPrimaryVblCalls = gPrimaryVblLastOtg = 0;
	gFakeNowNs = 42u;
	gFakeIrq = dcn41_irq { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
	gTxnAllowed = true;
	gLeaseActive = false;
	gTxnBegins = gTxnEnds = 0;
}

static void contract_checks() {
	reset_world();
	expect(!ihEntry(0u, 1u, 2u), "a non-DCE entry falls through");
	expect(gTxnBegins == 0u, "a non-DCE entry never attempts a display transaction");

	reset_world();
	gTxnAllowed = false;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 7u, 8u),
	       "an unarmed DCN entry keeps the original consumed result");
	expect(gTxnBegins == 0u, "an unarmed DCN entry keeps the original no-lease behavior");
	expect(gDcn.dceEntries == 1u && gDcn.dceUnclassified == 1u,
	       "an unarmed DCN entry keeps the original counters");

	reset_world();
	gDcn.armed = true;
	gScan.active = 1u;
	gDcn.flipHeld = true;
	gDcn.heldFrames = 0u;
	gDcn.heldFrameCap = 0u;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 7u, 8u),
	       "a leased DCN entry is consumed");
	expect(gTxnBegins == 1u && gTxnEnds == 1u && !gLeaseActive,
	       "the successful path creates and releases exactly one lease");
	expect(gScanCalls == 1u, "an active scanout path calls scanIrq once");
	expect(gWatchdogCalls == 1u && gDcn.watchdogRestores == 1u,
	       "the VUPDATE watchdog restore runs once");
	expect(gAckCalls == 0u, "scanIrq owns the ACK when it handles the entry");
	expect(gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK] == 1u,
	       "a successfully acknowledged entry keeps the normal kind count");
	expect(gDcn.ackFailures == 0u && gDcn.irqRefusals == 0u,
	       "the successful path changes neither failure counter");

	reset_world();
	gDcn.armed = true;
	gDcn.dceEntries = kDceStormCap + 1u;
	gDcn.srcEnabled = true;
	gDcn.srcKind = DCN41_IRQ_VSTARTUP;
	gDcn.srcInst = 1u;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 9u, 10u),
	       "the fallback ACK path consumes the entry");
	expect(gScanCalls == 0u && gAckCalls == 1u,
	       "an inactive scanout path runs the fallback ACK once");
	expect(gIrqSetCalls == 1u, "the cumulative storm guard disables its source once");
	expect(gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK] == 1u,
	       "the fallback ACK path keeps the normal kind count");
	expect(gDcn.ackFailures == 0u,
	       "a successful fallback ACK does not increment ackFailures");

	reset_world();
	gTxnAllowed = false;
	gDcn.armed = true;
	gDcn.flipHeld = true;
	gDcn.heldFrames = 1u;
	gDcn.heldFrameCap = 0u;
	gDcn.dceEntries = kDceStormCap + 1u;
	gDcn.srcEnabled = true;
	gDcn.srcKind = DCN41_IRQ_VSTARTUP;
	gDcn.srcInst = 1u;
	const uint64_t entryBefore = gDcn.dceEntries;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 11u, 12u),
	       "a refused lease consumes the DCN entry");
	expect(gTxnBegins == 1u && gTxnEnds == 0u && !gLeaseActive,
	       "the refused path attempts one lease and holds none");
	expect(gScanCalls == 0u && gWatchdogCalls == 0u && gAckCalls == 0u &&
	       gIrqSetCalls == 0u,
	       "a refused lease performs no ACK, plane, scanIrq or source write");
	expect(gDcn.flipHeld, "a refused lease does not pretend the plane was restored");
	expect(gDcn.dceEntries == entryBefore + 1u && gDcn.irqRefusals == 1u,
	       "the refused entry has its own count");
	expect(gDcn.irqRefusalLogs == 1u, "the refused entry has its own bounded log count");
	expect(gDcn.ackFailures == 0u,
	       "a refused lease is not reported as a failed ACK");
	for (uint8_t kind = 0; kind < DCN41_IRQ_KIND_COUNT; kind++)
		expect(gDcn.dceByKind[kind] == 0u,
		       "a refused lease does not increment an ACK kind counter");
	expect(gDcn.dceVupdate[0] == 0u && gDcn.dceVstartup[1] == 0u &&
	       gDcn.dceUnclassified == 0u,
	       "a refused lease does not increment per-source or unclassified ACK counters");
	expect(gDcn.irqAckRecoveryNeeded,
	       "a refused lease arms the watchdog ACK-recovery flag");

	// Stage 3A: primary VBL on VUPDATE_NO_LOCK dispatches AFTER releasing rdna4_display_txn
	// and works both standalone (gScan.active == 0) and shared with native scanout (gScan.active == 1).
	reset_world();
	gDcn.armed = true;
	gDcn.primaryVblEnabled = true;
	gDcn.primaryVblOtg = 0u;
	gDcn.irqAckRecoveryNeeded = true;
	gFakeIrq = dcn41_irq { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
	gFakeNowNs = 1000000000ULL;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 87u, 0u), "VUPDATE_NO_LOCK entry is consumed");
	expect(gAckCalls == 1u && gPrimaryVblCalls == 1u && gPrimaryVblLastOtg == 0u,
	       "primary VBL VUPDATE_NO_LOCK ACKs under lease and notifies FB outside lease");
	expect(!gDcn.irqAckRecoveryNeeded,
	       "successful VUPDATE_NO_LOCK ACK clears the ACK-recovery flag");
	expect(gTxnBegins == 1u && gTxnEnds == 1u && !gLeaseActive,
	       "primary VBL path releases the display lease cleanly");

	reset_world();
	gDcn.armed = true;
	gScan.active = 1u;
	gDcn.primaryVblEnabled = true;
	gDcn.primaryVblOtg = 0u;
	gFakeIrq = dcn41_irq { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
	gFakeNowNs = 1500000000ULL;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 87u, 0u), "shared scanout+primary VUPDATE_NO_LOCK is consumed");
	expect(gScanCalls == 1u && gAckCalls == 0u && gPrimaryVblCalls == 1u && gPrimaryVblLastOtg == 0u,
	       "shared VUPDATE_NO_LOCK serves scanIrq under lease AND notifies primary FB outside lease");

	// Stage 3A: primary VBL rate-based storm guard (> 1000/s) disables inside lease and skips notify.
	reset_world();
	gDcn.armed = true;
	gDcn.primaryVblEnabled = true;
	gDcn.primaryVblOtg = 0u;
	gDcn.primaryVblStorm.windowStartNs = 2000000000ULL;
	gDcn.primaryVblStorm.windowCount = rdna4vbl::kStormWindowMaxIrqs;
	gFakeIrq = dcn41_irq { DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
	gFakeNowNs = 2100000000ULL;
	expect(ihEntry(DCN41_IH_CLIENT_DCE, 87u, 0u), "storm-tripping VUPDATE_NO_LOCK is consumed");
	expect(gIrqSetCalls == 1u && !gDcn.primaryVblEnabled && gDcn.primaryVblStorm.tripped,
	       "primary VBL storm guard disables DCN41_IRQ_VUPDATE_NO_LOCK inside lease when scanout is inactive");
	expect(gPrimaryVblCalls == 0u, "storm-tripping VUPDATE_NO_LOCK suppresses FB notification");
}

int main() {
	contract_checks();
	std::printf("dcn_irq_contract_test: %d checks, %d failed\n", gRun, gFail);
	return gFail == 0 ? 0 : 1;
}
