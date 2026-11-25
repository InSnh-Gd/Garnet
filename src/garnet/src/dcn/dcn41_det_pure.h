// dcn41_det_pure.h — Pure DCN 4.1 single-pipe CRB DET0 expansion (192 KiB -> 576 KiB)
// and OPTC0 underflow verification helper.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stdint.h>

namespace rdna4det {

// DMU Segment 2 (BASE_IDX 2, default 0x34c0) relative dword offsets.
// All DET/COMPBUF offsets (abs 0x397a..0x397e) fall inside dcn41_allow_ranges.h [0x392f..0x3de1].
constexpr uint32_t kSeg2CompbufCtrl     = 0x04bau; // DCHUBBUB_COMPBUF_CTRL (abs 0x397a)
constexpr uint32_t kSeg2Det0Ctrl        = 0x04bbu; // DCHUBBUB_DET0_CTRL    (abs 0x397b, stride 1)
constexpr uint32_t kSeg2Optc0Global     = 0x1acau; // ODM0_OPTC_INPUT_GLOBAL_CONTROL (abs 0x4f8a)
constexpr uint32_t kSeg2Otg0Control     = 0x1b43u; // OTG0_OTG_CONTROL (abs 0x5003, stride 0x80)
constexpr uint32_t kSeg2OtgStride       = 0x0080u;
constexpr uint32_t kNumPipes            = 4u;

constexpr uint32_t kDetSizeMask         = 0x0000001Fu; // bits [4:0]
constexpr uint32_t kDetCurShift         = 8u;          // bits [12:8]
constexpr uint32_t kDetCurMask          = 0x00001F00u;

constexpr uint32_t kOtgActiveMask       = 0x00010001u; // MASTER_EN[0] | CURRENT_MASTER_EN[16]

// ODM0_OPTC_INPUT_GLOBAL_CONTROL fields (NEVER touch bit 0 OPTC_INPUT_SOFT_RESET!)
constexpr uint32_t kOptcSoftResetBit    = 0x00000001u;
constexpr uint32_t kOptcUnderflowMask   = 0x00002400u; // bit 10 OCCURRED_STATUS | bit 13 OCCURRED_CURRENT
constexpr uint32_t kOptcUnderflowClear  = 0x00001000u; // bit 12 OPTC_UNDERFLOW_CLEAR
constexpr uint32_t kOptcWriteClearMask  = 0x80003C00u;

// UEFI GOP allocates 1 compbuf + 4*3 DET = 13 segments (832 KiB) on Navi 44.
// Expanding DET0 to 9 segments (576 KiB) after zeroing DET1..3 stays strictly
// inside UEFI's proven 13-segment CRB budget (1 + 9 = 10 <= 13).
constexpr uint32_t kUefiCrbBudgetSegs   = 13u;
constexpr uint32_t kTargetDet0Segs      = 9u;
constexpr uint32_t kDetPollMaxIters     = 2000u;       // 20 ms at 10 us/step

struct Hw {
	void *cookie { nullptr };
	uint32_t (*rdSeg2)(void *cookie, uint32_t seg2Off) { nullptr };
	bool (*wrSeg2)(void *cookie, uint32_t seg2Off, uint32_t val) { nullptr };
	void (*delayUs)(void *cookie, uint32_t us) { nullptr };
};

enum Status : uint32_t {
	kOk = 0u,
	kAlreadyExpanded,
	kSkippedMultiPipe,
	kSkippedBudget,
	kPollTimeout,
	kIoError,
};

struct ExpandResult {
	Status status { kIoError };
	uint32_t compbufCur { 0u };
	uint32_t det0Before { 0u };
	uint32_t det0After { 0u };
};

inline bool wait_det_current(const Hw &hw, uint32_t pipe, uint32_t wantSegs) {
	const uint32_t reg = kSeg2Det0Ctrl + pipe;
	for (uint32_t i = 0u; i < kDetPollMaxIters; ++i) {
		const uint32_t v = hw.rdSeg2(hw.cookie, reg);
		if (v == 0xffffffffu) return false;
		if (((v & kDetCurMask) >> kDetCurShift) == wantSegs) return true;
		if (hw.delayUs) hw.delayUs(hw.cookie, 10u);
	}
	return false;
}

// Safely shrink unused DET1..3 to 0 and expand DET0 to `targetSegs` (default 9 = 576 KiB)
// when ONLY OTG0 is active, following upstream dcn401_hubbub.c shrink-before-grow order.
inline ExpandResult expand_single_pipe_det0(const Hw &hw, uint32_t targetSegs = kTargetDet0Segs) {
	ExpandResult res {};
	if (!hw.rdSeg2 || !hw.wrSeg2 || targetSegs == 0u || targetSegs > kDetSizeMask) return res;

	// 1. Confirm OTG0 is lit and OTG1..3 are completely off.
	const uint32_t otg0 = hw.rdSeg2(hw.cookie, kSeg2Otg0Control);
	if (otg0 == 0xffffffffu) { res.status = kIoError; return res; }
	if ((otg0 & kOtgActiveMask) == 0u) { res.status = kSkippedMultiPipe; return res; }
	for (uint32_t p = 1u; p < kNumPipes; ++p) {
		const uint32_t otgp = hw.rdSeg2(hw.cookie, kSeg2Otg0Control + p * kSeg2OtgStride);
		if (otgp == 0xffffffffu) { res.status = kIoError; return res; }
		if ((otgp & kOtgActiveMask) != 0u) { res.status = kSkippedMultiPipe; return res; }
	}

	// 2. Check CRB budget against live COMPBUF_SIZE_CURRENT and DET0_SIZE_CURRENT.
	const uint32_t compRaw = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
	const uint32_t det0Raw = hw.rdSeg2(hw.cookie, kSeg2Det0Ctrl);
	if (compRaw == 0xffffffffu || det0Raw == 0xffffffffu) { res.status = kIoError; return res; }
	res.compbufCur = (compRaw & kDetCurMask) >> kDetCurShift;
	res.det0Before = (det0Raw & kDetCurMask) >> kDetCurShift;
	res.det0After = res.det0Before;
	if (res.det0Before >= targetSegs) {
		res.status = kAlreadyExpanded;
		return res;
	}
	if (res.compbufCur + targetSegs > kUefiCrbBudgetSegs) {
		res.status = kSkippedBudget;
		return res;
	}

	// 3. Shrink inactive pipes DET1..3 to 0 FIRST so total CRB segments never exceed budget.
	for (uint32_t p = 1u; p < kNumPipes; ++p) {
		const uint32_t reg = kSeg2Det0Ctrl + p;
		const uint32_t cur = hw.rdSeg2(hw.cookie, reg);
		if (cur == 0xffffffffu) { res.status = kIoError; return res; }
		if ((cur & kDetSizeMask) != 0u || ((cur & kDetCurMask) >> kDetCurShift) != 0u) {
			if (!hw.wrSeg2(hw.cookie, reg, cur & ~kDetSizeMask)) {
				res.status = kIoError;
				return res;
			}
			if (!wait_det_current(hw, p, 0u)) {
				res.status = kPollTimeout;
				return res;
			}
		}
	}

	// 4. Grow DET0 to targetSegs and wait for hardware DET0_SIZE_CURRENT commit.
	const uint32_t nextDet0 = (det0Raw & ~kDetSizeMask) | (targetSegs & kDetSizeMask);
	if (!hw.wrSeg2(hw.cookie, kSeg2Det0Ctrl, nextDet0)) {
		res.status = kIoError;
		return res;
	}
	if (!wait_det_current(hw, 0u, targetSegs)) {
		const uint32_t after = hw.rdSeg2(hw.cookie, kSeg2Det0Ctrl);
		if (after != 0xffffffffu) res.det0After = (after & kDetCurMask) >> kDetCurShift;
		res.status = kPollTimeout;
		return res;
	}
	res.det0After = targetSegs;
	res.status = kOk;
	return res;
}

// Clear OPTC0 underflow latch (without touching soft-reset bit 0), dwell `dwellUs`,
// and return true if ANY OPTC0 underflow bit (0x2400) latched (fail-safe true on I/O error).
inline bool probe_optc0_underflow(const Hw &hw, uint32_t dwellUs, uint32_t *outReg = nullptr) {
	if (!hw.rdSeg2 || !hw.wrSeg2) return true;
	const uint32_t pre = hw.rdSeg2(hw.cookie, kSeg2Optc0Global);
	if (pre == 0xffffffffu || (pre & kOptcSoftResetBit) != 0u) {
		if (outReg) *outReg = pre;
		return true;
	}
	const uint32_t base = pre & ~kOptcWriteClearMask;
	if (!hw.wrSeg2(hw.cookie, kSeg2Optc0Global, base | kOptcUnderflowClear)) return true;
	if (!hw.wrSeg2(hw.cookie, kSeg2Optc0Global, base)) return true;
	if (hw.delayUs && dwellUs > 0u) hw.delayUs(hw.cookie, dwellUs);
	const uint32_t after = hw.rdSeg2(hw.cookie, kSeg2Optc0Global);
	if (outReg) *outReg = after;
	return after == 0xffffffffu || (after & kOptcUnderflowMask) != 0u;
}

}  // namespace rdna4det
