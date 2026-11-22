// vmid_pool_test.cpp — Host tests for rdna4vmid::Pool and rdna4sparsept.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "amd/rdna4_sparse_pt_pure.h"
#include "amd/rdna4_vmid_pool.h"

namespace {

int gChecks = 0;
int gFails = 0;

void check(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFails;
		std::printf("FAIL: %s\n", msg);
	}
}

struct FenceMock {
	uint32_t completed[rdna4vmid::kMaxDomains] {};
	static bool reached(void *ctx, uint32_t domain, uint32_t seq) {
		const auto *self = static_cast<const FenceMock *>(ctx);
		return domain < rdna4vmid::kMaxDomains && self->completed[domain] >= seq;
	}
};

void test_vmid_pool_exact_ack_and_quarantine() {
	FenceMock fence {};
	rdna4vmid::Pool pool;
	// Reserve VMID 8 (e.g. kernel bring-up self-test VMID) and always VMID 0.
	pool.init(&FenceMock::reached, &fence, 1u << 8);

	rdna4vmid::Grant g {};
	check(pool.grab(0u, 0x1000u, 1u, g) == rdna4vmid::Result::Exhausted, "zero owner rejected");

	const uintptr_t clientA = 0x1001u;
	const uintptr_t clientB = 0x2002u;
	check(pool.grab(clientA, 0xaaaa0000u, 10u, g) == rdna4vmid::Result::Ok, "clientA initial bind");
	const uint32_t vmidA = g.vmid;
	check(vmidA >= 1u && vmidA <= 15u && vmidA != 8u, "reservedMask and VMID 0 excluded");
	check(g.rebind && g.flush && g.flushSeq == 10u, "initial bind requests rebind + exact flushSeq");

	// Stale/wrong/non-matching flushComplete rejected.
	check(!pool.flushComplete(clientB, vmidA, 10u), "wrong owner flushComplete rejected");
	check(!pool.flushComplete(clientA, vmidA, 9u), "older seq flushComplete rejected");
	check(!pool.flushComplete(clientA, vmidA, 11u), "future seq flushComplete rejected");
	check(!pool.flushComplete(clientA, 0u, 10u), "VMID 0 flushComplete rejected");

	// Client removes a PTE while first flush was still in flight -> tlbSeq jumps to 11.
	rdna4vmid::Grant g2 {};
	check(pool.grab(clientA, 0xaaaa0000u, 11u, g2) == rdna4vmid::Result::Ok, "clientA reuse with newer tlbSeq");
	check(g2.vmid == vmidA && !g2.rebind && g2.flush && g2.flushSeq == 11u,
	      "reuse preserves VMID and updates pending flushSeq to 11");
	// Completion of the older seq=10 must NOT mark seq=11 flushed!
	check(!pool.flushComplete(clientA, vmidA, 10u), "stale in-flight ack (10) rejected against flushSeq=11");
	check(pool.flushComplete(clientA, vmidA, 11u), "exact ack (11) accepted");
	check(!pool.flushComplete(clientA, vmidA, 11u), "duplicate ack rejected when nothing pending");

	// Reuse at same tlbSeq requires no flush.
	check(pool.grab(clientA, 0xaaaa0000u, 11u, g2) == rdna4vmid::Result::Ok, "clean reuse");
	check(!g2.flush && !g2.rebind && g2.vmid == vmidA, "clean reuse skips flush and rebind");

	// Quarantine on uncertain hardware state permanently blocks VMID until unbindAll().
	check(!pool.quarantine(clientB, vmidA), "wrong owner cannot quarantine");
	check(pool.quarantine(clientA, vmidA), "owner quarantines uncertain VMID");
	check(pool.isQuarantined(vmidA) && pool.ownerOf(vmidA) == 0u, "quarantined VMID detached from owner");
	check(pool.stats().quarantines == 1u, "quarantine counter incremented");

	// Reserve all remaining usable VMIDs except vmidA is quarantined -> pin/grab cannot reuse vmidA.
	std::vector<uint32_t> grabbed;
	for (uint32_t k = 1u; k <= 13u; ++k) {
		rdna4vmid::Grant gk {};
		const uintptr_t owner = 0x3000u + k;
		const auto res = pool.grab(owner, 0x50000u + k * 0x1000u, 1u, gk);
		check(res == rdna4vmid::Result::Ok, "fill remaining unreserved/unquarantined VMIDs");
		check(gk.vmid != vmidA && gk.vmid != 8u, "quarantined/reserved VMID never granted");
		pool.noteSubmit(gk.vmid, rdna4vmid::kDomainGfx, 100u + k);
		grabbed.push_back(gk.vmid);
	}

	// All 13 usable slots are now in-flight (8 is reserved, vmidA is quarantined) -> Busy + starvation gate.
	check(pool.grab(clientB, 0xbbbb0000u, 1u, g) == rdna4vmid::Result::Busy, "pool reports Busy when all usable slots in flight");
	check(g.waitDomain == rdna4vmid::kDomainGfx && g.waitSeq == 101u, "Busy identifies oldest pending fence");
	// Complete first fence on 101; starvation clears, next LRU steals freed slot.
	fence.completed[rdna4vmid::kDomainGfx] = 101u;
	check(pool.grab(clientB, 0xbbbb0000u, 1u, g) == rdna4vmid::Result::Ok && g.stolen,
	      "once waiter fence completes, LRU idle slot is stolen cleanly");

	// forget(force=false) refuses while in flight, forget(force=true) force-releases, idle forget(false) succeeds.
	uint32_t releasedVmid = 0u;
	pool.noteSubmit(g.vmid, rdna4vmid::kDomainGfx, 500u);
	check(!pool.forget(clientB, false, releasedVmid), "forget(false) refuses while in-flight");
	check(pool.forget(clientB, true, releasedVmid) && releasedVmid == g.vmid, "forget(true) force-releases");
	rdna4vmid::Grant gIdle {};
	check(pool.grab(clientB, 0xcccc0000u, 1u, gIdle) == rdna4vmid::Result::Ok, "re-grab after forget succeeds");
	check(pool.forget(clientB, false, releasedVmid) && releasedVmid == gIdle.vmid, "idle forget(false) succeeds");

	// unbindAll() clears quarantine after GPU reset.
	pool.unbindAll();
	check(!pool.isQuarantined(vmidA), "unbindAll clears quarantined state after reset");
}

struct SparsePtMock {
	static constexpr uint32_t kNumPts = 16u;
	uint64_t ptPa[kNumPts] {};
	uint64_t ptes[kNumPts][rdna4sparsept::kPtesPerPt] {};
	bool ptAllocated[kNumPts] {};
	uint32_t allocCalls { 0u };
	uint32_t freeCalls { 0u };
	uint32_t syncCalls { 0u };
	uint32_t failSyncAtCall { 0u }; // 1-based sync failure trigger

	static bool ensure_pt(void *c, uint32_t slot, uint64_t *outPa, bool *outNew) {
		auto *self = static_cast<SparsePtMock *>(c);
		if (slot >= kNumPts) return false;
		if (self->ptAllocated[slot]) {
			*outPa = self->ptPa[slot];
			*outNew = false;
			return true;
		}
		self->allocCalls++;
		self->ptAllocated[slot] = true;
		self->ptPa[slot] = 0x8001000000ull + static_cast<uint64_t>(slot) * 0x1000ull;
		*outPa = self->ptPa[slot];
		*outNew = true;
		return true;
	}

	static bool write_pte(void *c, uint32_t slot, uint32_t idx, uint64_t val) {
		auto *self = static_cast<SparsePtMock *>(c);
		if (slot >= kNumPts || idx >= rdna4sparsept::kPtesPerPt) return false;
		self->ptes[slot][idx] = val;
		return true;
	}

	static bool sync_pt(void *c, uint32_t slot, uint64_t ptPa) {
		auto *self = static_cast<SparsePtMock *>(c);
		if (slot >= kNumPts || ptPa != self->ptPa[slot]) return false;
		self->syncCalls++;
		return self->failSyncAtCall == 0u || self->syncCalls != self->failSyncAtCall;
	}

	static void free_pt(void *c, uint32_t slot, uint64_t ptPa) {
		auto *self = static_cast<SparsePtMock *>(c);
		if (slot < kNumPts && self->ptAllocated[slot] && self->ptPa[slot] == ptPa) {
			self->ptAllocated[slot] = false;
			self->ptPa[slot] = 0u;
			self->freeCalls++;
		}
	}

	rdna4sparsept::PtOps ops() {
		return rdna4sparsept::PtOps {
			this,
			&SparsePtMock::ensure_pt,
			&SparsePtMock::write_pte,
			&SparsePtMock::sync_pt,
			&SparsePtMock::free_pt,
		};
	}
};

void test_sparse_pt_rollback_and_range() {
	// Upfront range validation checks.
	check(!rdna4sparsept::validate_range(0x1001ull, 1u, 1024u), "unaligned VA rejected");
	check(!rdna4sparsept::validate_range(0x1000ull, 0u, 1024u), "zero pages rejected");
	check(!rdna4sparsept::validate_range(0x100000ull, 100u, 256u), "past maxPfn rejected");
	check(!rdna4sparsept::validate_range(0xfffffffffffff000ull, UINT64_MAX, UINT64_MAX), "uint64 pfn sum overflow rejected");
	check(!rdna4sparsept::validate_range(0xfffffffffffff000ull, 2u, 1ull << 52), "page count past maxPfn at 64-bit top rejected");
	check(rdna4sparsept::validate_range(0x1fe000ull, 4u, 2048u), "aligned range crossing PT boundary accepted");

	// Span crossing PT 0 (last 2 entries: 510, 511) -> PT 1 (first 2 entries: 0, 1).
	SparsePtMock mock;
	// Pre-populate PT 0 to confirm rollback frees ONLY newly allocated PT 1 while preserving PT 0.
	mock.ptAllocated[0] = true;
	mock.ptPa[0] = 0x8000fff000ull;
	mock.failSyncAtCall = 2u; // Fail the second PT transfer!

	const uint64_t rootPa = 0x8000aa0000ull;
	const uint64_t ptes[4] = { 0x10001u, 0x20001u, 0x30001u, 0x40001u };
	rdna4sparsept::TxState tx {};
	const auto rFail = rdna4sparsept::map_sparse_pages(
	    mock.ops(), rootPa, 510ull * 4096ull, 4u, 4096u, ptes, tx);

	check(!rFail.ok && rFail.syncFailed && rFail.failedSegment == 1u, "2nd PT transfer failure reported");
	check(rFail.tlbSeqBumpNeeded, "partial PT sync failure bumps TLB seq");
	check(tx.rootPa == rootPa, "rootPa preserved");
	check(mock.allocCalls == 1u && mock.freeCalls == 1u, "single PT alloc + single PT free");
	check(mock.ptAllocated[0] && !mock.ptAllocated[1], "pre-existing PT0 kept, newly allocated PT1 freed");
	check(mock.ptes[0][510] == 0u && mock.ptes[0][511] == 0u &&
	          mock.ptes[1][0] == 0u && mock.ptes[1][1] == 0u,
	      "all written PTEs cleared on rollback");

	// Idempotent second rollback guard: calling rollback again does NOT double-free.
	rdna4sparsept::rollback(mock.ops(), tx);
	check(mock.freeCalls == 1u, "duplicate rollback is a no-op (no double-free)");

	// Retry without failure succeeds cleanly and preserves new PT1 physical address.
	mock.failSyncAtCall = 0u;
	rdna4sparsept::TxState tx2 {};
	const auto rOk = rdna4sparsept::map_sparse_pages(
	    mock.ops(), rootPa, 510ull * 4096ull, 4u, 4096u, ptes, tx2);
	check(rOk.ok && tx2.segmentCount == 2u, "retry after rollback succeeds");
	check(tx2.segments[1].ptPa == 0x8001001000ull, "newly allocated PT PA preserved directly");
	check(mock.ptes[0][510] == ptes[0] && mock.ptes[1][1] == ptes[3], "PTEs populated");
}

} // namespace

int main() {
	test_vmid_pool_exact_ack_and_quarantine();
	test_sparse_pt_rollback_and_range();
	std::printf("vmid_pool_test: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
