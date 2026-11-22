//
//  rdna4_sparse_pt_pure.h — Pure upfront range check, single-PA lookup, and idempotent rollback
//                           for multi-PT sparse GPU page-table updates.
//
//  Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (commits a0f57abc, 7401ff36,
//  45645d61, 4282415a, b930257f), Copyright (c) 2026 Sunneva N. Mariu,
//  licensed under the BSD 3-Clause License (see third-party/THIRD-PARTY-LICENSES.txt).
//
#pragma once

#include <stdint.h>

namespace rdna4sparsept {

constexpr uint64_t kPageSize         = 4096ull;
constexpr uint32_t kPtesPerPt        = 512u;
constexpr uint32_t kMaxPtSegments    = 64u;

struct PtOps {
	void *cookie { nullptr };
	// Allocate or look up the PT covering `ptSlot`; must return `outPtPa` directly on allocation
	// so callers never issue a second VA->PA lookup, and set `outNewlyAllocated`.
	bool (*ensurePtPage)(void *cookie, uint32_t ptSlot, uint64_t *outPtPa, bool *outNewlyAllocated) { nullptr };
	bool (*writePte)(void *cookie, uint32_t ptSlot, uint32_t pteIdx, uint64_t pteValue) { nullptr };
	bool (*syncPtPage)(void *cookie, uint32_t ptSlot, uint64_t ptPa) { nullptr };
	void (*freePtPage)(void *cookie, uint32_t ptSlot, uint64_t ptPa) { nullptr };
};

struct SegmentTx {
	uint32_t ptSlot { 0u };
	uint64_t ptPa { 0u };
	uint32_t startPte { 0u };
	uint32_t pteCount { 0u };
	uint32_t writtenCount { 0u };
	bool     newlyAllocated { false };
	bool     synced { false };
};

struct TxState {
	uint64_t  rootPa { 0u };
	uint32_t  segmentCount { 0u };
	SegmentTx segments[kMaxPtSegments] {};
	bool      rolledBack { false };
	bool      anyHardwareSync { false };
};

struct MapResult {
	bool ok { false };
	bool rangeRejected { false };
	bool syncFailed { false };
	bool tlbSeqBumpNeeded { false };
	uint32_t failedSegment { 0u };
};

// Validates virtual address alignment and PFN bounds completely upfront before touching any page table.
inline bool validate_range(uint64_t va, uint64_t pageCount, uint64_t maxPfn) {
	if (pageCount == 0u || maxPfn == 0u) return false;
	if ((va & (kPageSize - 1ull)) != 0ull) return false;
	const uint64_t firstPfn = va / kPageSize;
	if (firstPfn >= maxPfn) return false;
	return pageCount <= (maxPfn - firstPfn);
}

// Idempotent rollback: clears newly written PTEs in reverse order and frees only newly allocated
// PT pages using their preserved physical addresses, leaving `rootPa` and pre-existing PTs intact.
inline void rollback(const PtOps &ops, TxState &tx) {
	if (tx.rolledBack) return;
	tx.rolledBack = true;

	for (uint32_t i = tx.segmentCount; i > 0u; --i) {
		const SegmentTx &seg = tx.segments[i - 1u];
		if (ops.writePte) {
			for (uint32_t k = seg.writtenCount; k > 0u; --k) {
				(void)ops.writePte(ops.cookie, seg.ptSlot, seg.startPte + (k - 1u), 0ull);
			}
		}
		if (seg.newlyAllocated && ops.freePtPage) {
			ops.freePtPage(ops.cookie, seg.ptSlot, seg.ptPa);
		}
	}
}

// Maps `pageCount` contiguous 4 KiB pages starting at `va` using caller-supplied `pteValues[0..pageCount-1]`
// across PT boundaries. Rolls back atomically and exactly once on any allocation, write, or PT sync failure.
inline MapResult map_sparse_pages(const PtOps &ops,
                                  uint64_t rootPa,
                                  uint64_t va,
                                  uint64_t pageCount,
                                  uint64_t maxPfn,
                                  const uint64_t *pteValues,
                                  TxState &tx) {
	MapResult res {};
	tx = TxState {};
	tx.rootPa = rootPa;

	if (!rootPa || !pteValues || !ops.ensurePtPage || !ops.writePte || !ops.syncPtPage) {
		res.rangeRejected = true;
		return res;
	}
	if (!validate_range(va, pageCount, maxPfn)) {
		res.rangeRejected = true;
		return res;
	}

	const uint64_t firstPfn = va / kPageSize;
	const uint64_t endPfn = firstPfn + pageCount;
	const uint64_t firstPt64 = firstPfn / kPtesPerPt;
	const uint64_t lastPt64 = (endPfn - 1u) / kPtesPerPt;
	if (lastPt64 - firstPt64 + 1u > kMaxPtSegments || lastPt64 > UINT32_MAX) {
		res.rangeRejected = true;
		return res;
	}

	uint64_t cursorPfn = firstPfn;
	uint64_t valIdx = 0u;
	while (cursorPfn < endPfn) {
		const uint32_t ptSlot = static_cast<uint32_t>(cursorPfn / kPtesPerPt);
		const uint32_t startPte = static_cast<uint32_t>(cursorPfn % kPtesPerPt);
		const uint64_t leftInPt = static_cast<uint64_t>(kPtesPerPt - startPte);
		const uint64_t remaining = endPfn - cursorPfn;
		const uint32_t count = static_cast<uint32_t>(remaining < leftInPt ? remaining : leftInPt);

		SegmentTx &seg = tx.segments[tx.segmentCount];
		seg = SegmentTx {};
		seg.ptSlot = ptSlot;
		seg.startPte = startPte;
		seg.pteCount = count;

		if (!ops.ensurePtPage(ops.cookie, ptSlot, &seg.ptPa, &seg.newlyAllocated) || !seg.ptPa) {
			res.failedSegment = tx.segmentCount;
			rollback(ops, tx);
			res.tlbSeqBumpNeeded = tx.anyHardwareSync;
			return res;
		}
		tx.segmentCount++;

		for (uint32_t k = 0u; k < count; ++k) {
			if (!ops.writePte(ops.cookie, ptSlot, startPte + k, pteValues[valIdx++])) {
				res.failedSegment = tx.segmentCount - 1u;
				rollback(ops, tx);
				res.tlbSeqBumpNeeded = tx.anyHardwareSync;
				return res;
			}
			seg.writtenCount++;
		}

		if (!ops.syncPtPage(ops.cookie, ptSlot, seg.ptPa)) {
			res.syncFailed = true;
			res.failedSegment = tx.segmentCount - 1u;
			tx.anyHardwareSync = true;
			rollback(ops, tx);
			res.tlbSeqBumpNeeded = true;
			return res;
		}
		seg.synced = true;
		tx.anyHardwareSync = true;

		cursorPfn += count;
	}

	res.ok = true;
	res.tlbSeqBumpNeeded = true;
	return res;
}

} // namespace rdna4sparsept
