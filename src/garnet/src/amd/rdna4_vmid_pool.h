//
//  rdna4_vmid_pool.h — Freestanding VMID pool with exact TLB sequence ack and quarantine.
//
//  Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (src/vmid.hpp and src/vmid.cpp,
//  commits 4386290d, 041c22ad, e106dc56, e5c09ce2, 87fe681e), Copyright (c) 2026 Sunneva N. Mariu,
//  licensed under the BSD 3-Clause License (see third-party/THIRD-PARTY-LICENSES.txt).
//
#pragma once

#include <stdint.h>

namespace rdna4vmid {

constexpr uint32_t kFirst      = 1u;   // VMID 0 is the kernel's physical context
constexpr uint32_t kLast       = 15u;
constexpr uint32_t kSlots      = kLast + 1u;
constexpr uint32_t kMaxDomains    = 4u;
constexpr uint32_t kDomainGfx     = 2u;
constexpr uint32_t kDomainCompute = 3u;

using FenceReached = bool (*)(void *context, uint32_t domain, uint64_t seq);

enum class Result {
	Ok,
	Busy,
	Exhausted,
};

struct Grant {
	uint32_t  vmid       { 0u };
	uint64_t  flushSeq   { 0u };
	bool      rebind     { false };
	bool      flush      { false };
	bool      stolen     { false };
	uintptr_t prevOwner  { 0u };
	uint32_t  waitDomain { 0u };
	uint64_t  waitSeq    { 0ull };
};

struct Stats {
	uint32_t reuses      { 0u };
	uint32_t binds       { 0u };
	uint32_t steals      { 0u };
	uint32_t flushes     { 0u };
	uint32_t busy        { 0u };
	uint32_t unbinds     { 0u };
	uint32_t quarantines { 0u };
};

class Pool {
public:
	void init(FenceReached cb, void *context, uint32_t reservedMask = 0u) {
		for (uint32_t i = 0u; i < kSlots; ++i) slot[i] = Slot {};
		reached = cb;
		ctx = context;
		reserved = reservedMask | 1u; // VMID 0 is never granted
		tick = 0u;
		starving = false;
		starveDomain = 0u;
		starveSeq = 0u;
		counters = Stats {};
	}

	bool idle(uint32_t v) {
		if (v >= kSlots) return true;
		Slot &s = slot[v];
		if (s.inFlight != 0u) return false;
		for (uint32_t d = 0u; d < kMaxDomains; ++d) {
			if (!s.pending[d]) continue;
			if (!reached || !reached(ctx, d, s.pendingSeq[d])) return false;
			s.pending[d] = false;
		}
		return true;
	}

	Result grab(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
		out = Grant {};
		if (!owner) return Result::Exhausted;
		// Reuse first: synchronously locking submissions reuse an owner's active VMID if bound.
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			Slot &s = slot[v];
			if (!usable(v) || s.quarantined || s.owner != owner || s.pd != pd) continue;
			out.vmid = v;
			if (s.boundSeq < tlbSeq) {
				if (!s.flushPending || tlbSeq > s.flushSeq) counters.flushes++;
				s.flushPending = true;
				s.flushSeq = tlbSeq;
			}
			out.flushSeq = s.flushSeq;
			out.flush = s.flushPending;
			s.lastUse = ++tick;
			counters.reuses++;
			return Result::Ok;
		}
		if (starved(out)) return Result::Busy;
		return takeNew(owner, pd, tlbSeq, false, out);
	}

	Result pin(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
		out = Grant {};
		if (!owner) return Result::Exhausted;
		if (starved(out)) return Result::Busy;
		return takeNew(owner, pd, tlbSeq, true, out);
	}

	// Exact sequence acknowledgement: an older in-flight invalidation cannot cover a newer tlbSeq.
	bool flushComplete(uintptr_t owner, uint32_t vmid, uint64_t coveredSeq) {
		if (!owner || vmid < kFirst || vmid > kLast) return false;
		Slot &s = slot[vmid];
		if (s.quarantined || s.owner != owner || !s.flushPending || s.flushSeq != coveredSeq) {
			return false;
		}
		s.boundSeq = coveredSeq;
		s.flushPending = false;
		return true;
	}

	// Synchronous PT mutation flushed directly on an active owner's bound VMID.
	void noteFlushed(uintptr_t owner, uint32_t vmid, uint64_t coveredSeq) {
		if (!owner || vmid < kFirst || vmid > kLast) return;
		Slot &s = slot[vmid];
		if (s.quarantined || s.owner != owner) return;
		if (coveredSeq > s.boundSeq) s.boundSeq = coveredSeq;
		if (s.flushPending && coveredSeq >= s.flushSeq) s.flushPending = false;
	}

	uint32_t vmidOf(uintptr_t owner) const {
		if (!owner) return 0u;
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			if (usable(v) && !slot[v].quarantined && slot[v].owner == owner) return v;
		}
		return 0u;
	}

	// Clear pending fence trackers when the ring seqno restarts; keep quarantined slots poisoned.
	void resetFences() {
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			Slot &s = slot[v];
			if (s.quarantined) continue;
			for (uint32_t d = 0u; d < kMaxDomains; ++d) {
				s.pending[d] = false;
				s.pendingSeq[d] = 0ull;
			}
		}
		starving = false;
		starveDomain = 0u;
		starveSeq = 0ull;
	}

	// Permanently quarantine a VMID whose hardware invalidation/reset state is uncertain until unbindAll().
	bool quarantine(uintptr_t owner, uint32_t vmid) {
		if (!owner || vmid < kFirst || vmid > kLast) return false;
		Slot &s = slot[vmid];
		if (s.owner != owner || s.quarantined) return false;
		s = Slot {};
		s.pinned = true;
		s.quarantined = true;
		counters.quarantines++;
		return true;
	}

	void noteSubmit(uint32_t vmid, uint32_t domain, uint64_t seq) {
		if (vmid < kFirst || vmid > kLast || domain >= kMaxDomains || slot[vmid].quarantined) {
			return;
		}
		Slot &s = slot[vmid];
		s.pending[domain] = true;
		s.pendingSeq[domain] = seq;
		s.lastUse = ++tick;
	}

	void beginSubmit(uint32_t vmid) {
		if (vmid >= kFirst && vmid <= kLast && !slot[vmid].quarantined) slot[vmid].inFlight++;
	}

	void endSubmit(uint32_t vmid) {
		if (vmid >= kFirst && vmid <= kLast && slot[vmid].inFlight > 0u) slot[vmid].inFlight--;
	}

	bool forget(uintptr_t owner, bool force, uint32_t &vmid) {
		vmid = 0u;
		if (!owner) return true;
		if (!force) {
			for (uint32_t v = kFirst; v <= kLast; ++v) {
				if (slot[v].owner == owner && !idle(v)) return false;
			}
		}
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			if (slot[v].owner != owner) continue;
			if (!vmid) vmid = v;
			slot[v] = Slot {};
			counters.unbinds++;
		}
		return true;
	}

	void unbindAll() {
		for (uint32_t v = kFirst; v <= kLast; ++v) slot[v] = Slot {};
		starving = false;
		starveDomain = 0u;
		starveSeq = 0u;
	}

	uintptr_t ownerOf(uint32_t vmid) const {
		return vmid < kSlots ? slot[vmid].owner : 0u;
	}
	bool pinned(uint32_t vmid) const {
		return vmid < kSlots && slot[vmid].pinned;
	}
	bool isQuarantined(uint32_t vmid) const {
		return vmid >= kFirst && vmid <= kLast && slot[vmid].quarantined;
	}
	const Stats &stats() const { return counters; }

private:
	struct Slot {
		uintptr_t owner { 0u };
		uint64_t  pd { 0u };
		uint64_t  boundSeq { 0u };
		uint64_t  flushSeq { 0u };
		uint64_t  lastUse { 0u };
		bool      flushPending { false };
		bool      pinned { false };
		bool      quarantined { false };
		uint32_t  inFlight { 0u };
		bool      pending[kMaxDomains] {};
		uint64_t  pendingSeq[kMaxDomains] {};
	};

	Slot         slot[kSlots] {};
	uint32_t     reserved { 1u };
	FenceReached reached { nullptr };
	void        *ctx { nullptr };
	uint64_t     tick { 0u };
	bool         starving { false };
	uint32_t     starveDomain { 0u };
	uint64_t     starveSeq { 0ull };
	Stats        counters {};

	bool usable(uint32_t v) const {
		return v >= kFirst && v <= kLast && !(reserved & (1u << v));
	}

	bool firstPending(uint32_t v, uint32_t &domain, uint64_t &seq) {
		const Slot &s = slot[v];
		for (uint32_t d = 0u; d < kMaxDomains; ++d) {
			if (s.pending[d] && !(reached && reached(ctx, d, s.pendingSeq[d]))) {
				domain = d;
				seq = s.pendingSeq[d];
				return true;
			}
		}
		return false;
	}

	bool starved(Grant &out) {
		if (!starving) return false;
		if (reached && reached(ctx, starveDomain, starveSeq)) {
			starving = false;
			return false;
		}
		out.waitDomain = starveDomain;
		out.waitSeq = starveSeq;
		counters.busy++;
		return true;
	}

	void dropOtherSlotsOf(uintptr_t owner, uint32_t keep) {
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			Slot &s = slot[v];
			if (v != keep && s.owner == owner && !s.pinned && !s.quarantined && idle(v)) {
				s = Slot {};
				counters.unbinds++;
			}
		}
	}

	void bind(uint32_t v, uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
		Slot &s = slot[v];
		out.stolen = (s.owner != 0u && s.owner != owner);
		out.prevOwner = (s.owner != owner) ? s.owner : 0u;
		s.owner = owner;
		s.pd = pd;
		s.boundSeq = 0u;
		s.flushSeq = tlbSeq;
		s.flushPending = true;
		s.quarantined = false;
		s.lastUse = ++tick;
		for (uint32_t d = 0u; d < kMaxDomains; ++d) s.pending[d] = false;
		out.vmid = v;
		out.flushSeq = tlbSeq;
		out.rebind = true;
		out.flush = true;
		counters.binds++;
		counters.flushes++;
		if (out.stolen) counters.steals++;
		dropOtherSlotsOf(owner, v);
	}

	Result takeNew(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, bool pinIt, Grant &out) {
		uint32_t bestFree = 0u;
		uint32_t bestIdle = 0u;
		uint32_t oldestBusy = 0u;
		bool any = false;
		for (uint32_t v = kFirst; v <= kLast; ++v) {
			if (!usable(v) || slot[v].pinned || slot[v].quarantined) continue;
			any = true;
			const Slot &s = slot[v];
			if (!s.owner) {
				if (!bestFree || s.lastUse < slot[bestFree].lastUse) bestFree = v;
			} else if (idle(v)) {
				if (!bestIdle || s.lastUse < slot[bestIdle].lastUse) bestIdle = v;
			} else if (!oldestBusy || s.lastUse < slot[oldestBusy].lastUse) {
				oldestBusy = v;
			}
		}
		const uint32_t pick = bestFree ? bestFree : bestIdle;
		if (pick) {
			bind(pick, owner, pd, tlbSeq, out);
			slot[pick].pinned = pinIt;
			return Result::Ok;
		}
		if (!any || !oldestBusy) return Result::Exhausted;
		uint32_t d = 0u;
		uint64_t q = 0ull;
		if (!firstPending(oldestBusy, d, q)) {
			if (slot[oldestBusy].inFlight != 0u) {
				counters.busy++;
				return Result::Busy;
			}
			bind(oldestBusy, owner, pd, tlbSeq, out);
			slot[oldestBusy].pinned = pinIt;
			return Result::Ok;
		}
		starving = true;
		starveDomain = out.waitDomain = d;
		starveSeq = out.waitSeq = q;
		counters.busy++;
		return Result::Busy;
	}
};

} // namespace rdna4vmid
