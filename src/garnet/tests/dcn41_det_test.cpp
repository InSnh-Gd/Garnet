#include "dcn/dcn41_det_pure.h"

#include "dcn41_allow_ranges.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

int gChecks = 0;
int gFail = 0;
void expect(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFail;
		std::printf("FAIL: %s\n", msg);
	}
}

constexpr uint32_t kSeg2 = 0x34c0u;

bool absAllowed(uint32_t absDw) {
	for (uint32_t i = 0; i < DCN41_HARD_DENY_COUNT; ++i)
		if (absDw >= dcn41_hard_deny[i].abs_lo && absDw <= dcn41_hard_deny[i].abs_hi) return false;
	for (uint32_t i = 0; i < DCN41_DENY_BLOCK_COUNT; ++i)
		if (absDw >= dcn41_deny_blocks[i].abs_lo && absDw <= dcn41_deny_blocks[i].abs_hi) return false;
	for (uint32_t i = 0; i < DCN41_ALLOW_RANGE_COUNT; ++i)
		if (absDw >= dcn41_allow_ranges[i].abs_lo && absDw <= dcn41_allow_ranges[i].abs_hi) return true;
	return false;
}

struct MockDet {
	std::unordered_map<uint32_t, uint32_t> regs;
	std::vector<uint32_t> writeOrder;
	uint32_t deniedWrites = 0u;
	bool stallDet2 = false;
	bool injectUnderflow = false;
	uint32_t maxLiveCrbSum = 0u;

	void initLiveRx9060Xt() {
		regs.clear();
		writeOrder.clear();
		deniedWrites = 0u;
		stallDet2 = false;
		injectUnderflow = false;
		maxLiveCrbSum = 0u;
		// Live RX 9060 XT boot readings:
		regs[kSeg2 + rdna4det::kSeg2Otg0Control] = 0x00010001u;
		regs[kSeg2 + rdna4det::kSeg2Otg0Control + 1u * rdna4det::kSeg2OtgStride] = 0u;
		regs[kSeg2 + rdna4det::kSeg2Otg0Control + 2u * rdna4det::kSeg2OtgStride] = 0u;
		regs[kSeg2 + rdna4det::kSeg2Otg0Control + 3u * rdna4det::kSeg2OtgStride] = 0u;
		regs[kSeg2 + rdna4det::kSeg2CompbufCtrl] = 0x00040101u;
		regs[kSeg2 + rdna4det::kSeg2Det0Ctrl + 0u] = 0x00000303u;
		regs[kSeg2 + rdna4det::kSeg2Det0Ctrl + 1u] = 0x00000003u;
		regs[kSeg2 + rdna4det::kSeg2Det0Ctrl + 2u] = 0x00000003u;
		regs[kSeg2 + rdna4det::kSeg2Det0Ctrl + 3u] = 0x00000003u;
		regs[kSeg2 + rdna4det::kSeg2Optc0Global] = 0x00000000u;
	}

	uint32_t liveCrbSum() const {
		uint32_t s = (regs.at(kSeg2 + rdna4det::kSeg2CompbufCtrl) >> 8) & 0x1fu;
		for (uint32_t p = 0; p < 4u; ++p) {
			s += (regs.at(kSeg2 + rdna4det::kSeg2Det0Ctrl + p) & 0x1fu);
		}
		return s;
	}

	uint32_t rd(uint32_t seg2Off) {
		const uint32_t absDw = kSeg2 + seg2Off;
		if (injectUnderflow && seg2Off == rdna4det::kSeg2Optc0Global) {
			return regs[absDw] | 0x2000u; // bit 13 UNDERFLOW_OCCURRED_CURRENT
		}
		auto it = regs.find(absDw);
		return (it != regs.end()) ? it->second : 0u;
	}

	bool wr(uint32_t seg2Off, uint32_t val) {
		const uint32_t absDw = kSeg2 + seg2Off;
		if (!absAllowed(absDw)) {
			++deniedWrites;
			return false;
		}
		writeOrder.push_back(seg2Off);
		if (seg2Off >= rdna4det::kSeg2Det0Ctrl && seg2Off < rdna4det::kSeg2Det0Ctrl + 4u) {
			const uint32_t pipe = seg2Off - rdna4det::kSeg2Det0Ctrl;
			const uint32_t req = val & rdna4det::kDetSizeMask;
			if (stallDet2 && pipe == 2u) {
				regs[absDw] = (regs[absDw] & ~rdna4det::kDetSizeMask) | req;
				// Simulate stuck DET2_SIZE_CURRENT != 0
				regs[absDw] |= (3u << 8);
			} else {
				regs[absDw] = (val & ~rdna4det::kDetCurMask) | (req << rdna4det::kDetCurShift);
			}
			const uint32_t sum = liveCrbSum();
			if (sum > maxLiveCrbSum) maxLiveCrbSum = sum;
			return true;
		}
		regs[absDw] = val;
		return true;
	}

	rdna4det::Hw makeHw() {
		return rdna4det::Hw {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockDet *>(c)->rd(off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockDet *>(c)->wr(off, val); },
			[](void *, uint32_t) {},
		};
	}
};

}  // namespace

int main() {
	using namespace rdna4det;

	// 1. Live RX 9060 XT boot state expands DET0 3 -> 9 (576 KiB), shrinking DET1..3 first.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		const auto r = expand_single_pipe_det0(m.makeHw());
		expect(r.status == kOk && r.det0Before == 3u && r.det0After == 9u && r.compbufCur == 1u,
		       "single-pipe DET0 expands 3 -> 9 segments (576 KiB)");
		expect(m.regs[kSeg2 + kSeg2Det0Ctrl + 0u] == 0x00000909u, "DET0_CTRL commits 0x909");
		expect(m.regs[kSeg2 + kSeg2Det0Ctrl + 1u] == 0u &&
		       m.regs[kSeg2 + kSeg2Det0Ctrl + 2u] == 0u &&
		       m.regs[kSeg2 + kSeg2Det0Ctrl + 3u] == 0u,
		       "DET1..3 zeroed cleanly");
		expect(m.writeOrder.size() == 4u && m.writeOrder[3] == kSeg2Det0Ctrl,
		       "DET1..3 shrunk BEFORE DET0 grows");
		expect(m.maxLiveCrbSum <= kUefiCrbBudgetSegs,
		       "live CRB sum never exceeds UEFI 13-segment budget at any write step");
		expect(m.deniedWrites == 0u, "all DET writes pass DCN41 allowlist");

		// Second call is idempotent (kAlreadyExpanded, zero writes).
		m.writeOrder.clear();
		const auto r2 = expand_single_pipe_det0(m.makeHw());
		expect(r2.status == kAlreadyExpanded && m.writeOrder.empty(),
		       "repeat call detects already-expanded DET0 without writing");
	}

	// 2. Multi-pipe active (OTG1 lit) skips safely with zero writes.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		m.regs[kSeg2 + kSeg2Otg0Control + 1u * kSeg2OtgStride] = 0x00010001u;
		const auto r = expand_single_pipe_det0(m.makeHw());
		expect(r.status == kSkippedMultiPipe && m.writeOrder.empty(),
		       "multi-pipe OTG1 active skips DET0 expansion untouched");
	}

	// 3. Budget overrun (e.g. compbufCur = 6 or programmed compbuf = 6 -> 6 + 9 = 15 > 13) skips safely.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		m.regs[kSeg2 + kSeg2CompbufCtrl] = 0x00040606u;
		const auto r = expand_single_pipe_det0(m.makeHw());
		expect(r.status == kSkippedBudget && m.writeOrder.empty(),
		       "CRB budget overrun skips DET0 expansion untouched");

		m.initLiveRx9060Xt();
		m.regs[kSeg2 + kSeg2CompbufCtrl] = 0x00040106u; // programmed=6, current=1
		const auto rProg = expand_single_pipe_det0(m.makeHw());
		expect(rProg.status == kSkippedBudget && m.writeOrder.empty(),
		       "programmed COMPBUF_SIZE budget overrun skips DET0 expansion untouched");
	}

	// 3b. COMPBUF CONFIG_ERROR bit 31 after DET0 commit reports kIoError.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		m.regs[kSeg2 + kSeg2CompbufCtrl] = 0x80040101u;
		const auto r = expand_single_pipe_det0(m.makeHw());
		expect(r.status == kIoError, "COMPBUF CONFIG_ERROR bit 31 flags kIoError");
	}

	// 4. Stalled DET2 shrink aborts BEFORE touching DET0.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		m.stallDet2 = true;
		const auto r = expand_single_pipe_det0(m.makeHw());
		expect(r.status == kPollTimeout && m.regs[kSeg2 + kSeg2Det0Ctrl] == 0x00000303u,
		       "DET2 shrink timeout aborts before modifying DET0");
	}

	// 5. OPTC0 underflow probe: clean vs underflowed vs soft-reset hazard guard.
	{
		MockDet m;
		m.initLiveRx9060Xt();
		uint32_t reg = 0u;
		expect(!probe_optc0_underflow(m.makeHw(), 250000u, &reg) && reg == 0u,
		       "clean OPTC0 returns no underflow");
		expect((m.regs[kSeg2 + kSeg2Optc0Global] & kOptcSoftResetBit) == 0u,
		       "OPTC0 probe never touches soft-reset bit 0");
		expect(m.deniedWrites == 0u, "OPTC0 clear writes pass DCN41 allowlist");

		m.injectUnderflow = true;
		expect(probe_optc0_underflow(m.makeHw(), 250000u, &reg) && (reg & kOptcUnderflowMask) != 0u,
		       "latched OPTC0 bit 13 underflow is detected");
	}

	std::printf("dcn41_det_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
