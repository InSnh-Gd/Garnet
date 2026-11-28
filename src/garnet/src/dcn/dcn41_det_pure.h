// dcn41_det_pure.h — Pure DCN 4.1 21-segment CRB/DET controller (1344 KiB),
// watermark/TTU programming helpers, and OPTC0 underflow verification helper.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stdint.h>

namespace rdna4det {

// DMU Segment 2 (BASE_IDX 2, default 0x34c0) relative dword offsets.
// All DCHUBBUB/HUBP/HUBPREQ offsets (abs 0x397a..0x39e5, 0x3ae1..0x3ae4) fall inside dcn41_allow_ranges.h [0x392f..0x3de1].
constexpr uint32_t kSeg2CompbufCtrl     = 0x04bau; // DCHUBBUB_COMPBUF_CTRL (abs 0x397a)
constexpr uint32_t kSeg2Det0Ctrl        = 0x04bbu; // DCHUBBUB_DET0_CTRL    (abs 0x397b, stride 1)
constexpr uint32_t kSeg2CompbufRsvSpace = 0x04c4u; // COMPBUF_RESERVED_SPACE (abs 0x3984)
constexpr uint32_t kSeg2ArbSatLevel     = 0x04fau; // DCHUBBUB_ARB_SAT_LEVEL (abs 0x39ba)
constexpr uint32_t kSeg2WmUrgA          = 0x04feu; // DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A
constexpr uint32_t kSeg2WmUsrA          = 0x04ffu; // DCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A
constexpr uint32_t kSeg2WmTripMemA      = 0x0500u; // DCHUBBUB_ARB_REFCYC_PER_TRIP_TO_MEMORY_A
constexpr uint32_t kSeg2WmMetaTripA     = 0x0501u; // DCHUBBUB_ARB_REFCYC_PER_META_TRIP_A
constexpr uint32_t kSeg2WmSrEnterA      = 0x0502u; // DCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A (..3 step +2)
constexpr uint32_t kSeg2WmSrExitA       = 0x0503u; // DCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A  (..3 step +2)
constexpr uint32_t kSeg2WmUclkPstateA   = 0x050au; // DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A
constexpr uint32_t kSeg2WmUclkPstate1A  = 0x050bu; // DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK1_A
constexpr uint32_t kSeg2WmFclkPstateA   = 0x050cu; // DCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A
constexpr uint32_t kSeg2WmFclkPstate1A  = 0x050du; // DCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK1_A
constexpr uint32_t kSeg2WmFracNomA      = 0x050eu; // DCHUBBUB_ARB_FRAC_URG_BW_NOM_A
constexpr uint32_t kSeg2WmFracFlipA     = 0x050fu; // DCHUBBUB_ARB_FRAC_URG_BW_FLIP_A
constexpr uint32_t kSeg2WmFracMallA     = 0x0510u; // DCHUBBUB_ARB_FRAC_URG_BW_MALL_A
constexpr uint32_t kSeg2WmUrgB          = 0x0511u; // DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_B (matches set B order)
constexpr uint32_t kSeg2WmChangeCntl    = 0x0524u; // DCHUBBUB_ARB_WATERMARK_CHANGE_CNTL (abs 0x39e4)
constexpr uint32_t kSeg2ArbMallCntl     = 0x0525u; // DCHUBBUB_ARB_MALL_CNTL (abs 0x39e5)

constexpr uint32_t kSeg2HubpReq0TtuQosWm = 0x0621u; // HUBPREQ0_DCN_TTU_QOS_WM
constexpr uint32_t kSeg2HubpReq0Ttu0     = 0x0623u; // HUBPREQ0_DCN_SURF0_TTU_CNTL0
constexpr uint32_t kSeg2HubpReq0Ttu1     = 0x0624u; // HUBPREQ0_DCN_SURF0_TTU_CNTL1
constexpr uint32_t kSeg2HubpStride       = 0x00dcu; // HUBPREQ instance stride

constexpr uint32_t kSeg2Optc0Global     = 0x1acau; // ODM0_OPTC_INPUT_GLOBAL_CONTROL (abs 0x4f8a)
constexpr uint32_t kSeg2Otg0Control     = 0x1b43u; // OTG0_OTG_CONTROL (abs 0x5003, stride 0x80)
constexpr uint32_t kSeg2OtgStride       = 0x0080u;
constexpr uint32_t kNumPipes            = 4u;

constexpr uint32_t kDetSizeMask         = 0x0000001Fu; // bits [4:0]
constexpr uint32_t kDetCurShift         = 8u;          // bits [12:8]
constexpr uint32_t kDetCurMask          = 0x00001F00u;
constexpr uint32_t kCompbufIntEnable    = 0x00010000u;
constexpr uint32_t kCompbufCfgErrMask   = 0x80000000u; // DCHUBBUB_COMPBUF_CTRL.CONFIG_ERROR [31]

constexpr uint32_t kOtgActiveMask       = 0x00010001u; // MASTER_EN[0] | CURRENT_MASTER_EN[16]

// ODM0_OPTC_INPUT_GLOBAL_CONTROL fields (NEVER touch bit 0 OPTC_INPUT_SOFT_RESET!)
constexpr uint32_t kOptcSoftResetBit    = 0x00000001u;
constexpr uint32_t kOptcUnderflowMask   = 0x00002400u; // bit 10 OCCURRED_STATUS | bit 13 OCCURRED_CURRENT
constexpr uint32_t kOptcUnderflowClear  = 0x00001000u; // bit 12 OPTC_UNDERFLOW_CLEAR
constexpr uint32_t kOptcWriteClearMask  = 0x80003C00u;

// DCN 4.01 (dcn401_hubbub.h) full hardware CRB budget = 21 x 64 KiB segments (1344 KiB).
// UEFI GOP conservative budget = 13 segments (832 KiB) for single-pipe legacy expand_single_pipe_det0.
constexpr uint32_t kHardwareCrbBudgetSegs = 21u;       // DCN4_01_CRB_SIZE_KB (1344) / 64
constexpr uint32_t kDisp2DetSegs          = 3u;
constexpr uint32_t kUefiCrbBudgetSegs     = 13u;
constexpr uint32_t kTargetDet0Segs        = 9u;
constexpr uint32_t kDetPollMaxIters       = 2000u;     // 20 ms at 10 us/step

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

inline bool wait_crb_current(const Hw &hw, uint32_t reg, uint32_t wantSegs) {
	for (uint32_t i = 0u; i < kDetPollMaxIters; ++i) {
		const uint32_t v = hw.rdSeg2(hw.cookie, reg);
		if (v == 0xffffffffu) return false;
		if (((v & kDetCurMask) >> kDetCurShift) == wantSegs) return true;
		if (hw.delayUs) hw.delayUs(hw.cookie, 10u);
	}
	return false;
}

inline bool wait_det_current(const Hw &hw, uint32_t pipe, uint32_t wantSegs) {
	return wait_crb_current(hw, kSeg2Det0Ctrl + pipe, wantSegs);
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

	// 2. Check CRB budget against max(COMPBUF_SIZE, COMPBUF_SIZE_CURRENT) and DET0_SIZE_CURRENT.
	const uint32_t compRaw = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
	const uint32_t det0Raw = hw.rdSeg2(hw.cookie, kSeg2Det0Ctrl);
	if (compRaw == 0xffffffffu || (compRaw & kCompbufCfgErrMask) != 0u || det0Raw == 0xffffffffu) {
		res.status = kIoError;
		return res;
	}
	const uint32_t compProg = compRaw & kDetSizeMask;
	const uint32_t compCur = (compRaw & kDetCurMask) >> kDetCurShift;
	res.compbufCur = (compProg > compCur) ? compProg : compCur;
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
	const uint32_t compAfter = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
	if (compAfter == 0xffffffffu || (compAfter & kCompbufCfgErrMask) != 0u) {
		res.status = kIoError;
		return res;
	}
	res.det0After = targetSegs;
	res.status = kOk;
	return res;
}

struct CrbPlan {
	Status status { kSkippedBudget };
	uint32_t compbufSegs { 0u };
	uint32_t detSegs[kNumPipes] {};
	bool disp2Preserved { false };
};

// DET and COMPBUF sizes come from the solved mode; pipe counts alone cannot determine them.
inline CrbPlan plan_crb_distribution(uint32_t activePipeMask, uint32_t compbufSegs,
                                     const uint32_t (&modeDetSegs)[kNumPipes], bool disp2Active = false) {
	CrbPlan plan {};
	if (activePipeMask == 0u || activePipeMask >= (1u << kNumPipes) || compbufSegs >= kHardwareCrbBudgetSegs) return plan;
	uint32_t used = compbufSegs;
	for (uint32_t pipe = 0u; pipe < kNumPipes; ++pipe) {
		const bool active = (activePipeMask & (1u << pipe)) != 0u;
		const bool preserve = disp2Active && pipe < 3u;
		const uint32_t target = modeDetSegs[pipe];
		if (target > kHardwareCrbBudgetSegs || active != (target != 0u)) return plan;
		if (preserve && target > kDisp2DetSegs) {
			plan.status = kSkippedMultiPipe;
			return plan;
		}
		plan.detSegs[pipe] = preserve ? kDisp2DetSegs : target;
		used += plan.detSegs[pipe];
	}
	if (used > kHardwareCrbBudgetSegs) return plan;
	plan.compbufSegs = compbufSegs;
	plan.disp2Preserved = disp2Active;
	plan.status = kOk;
	return plan;
}

// Caller owns the pipe state and must validate the target allocation against the mode.
// COMPBUF_RESERVED_SPACE is set at CRB initialization from the pixel chunk size, not here.
inline Status program_multi_pipe_det(const Hw &hw, const CrbPlan &plan) {
	if (!hw.rdSeg2 || !hw.wrSeg2 || plan.status != kOk) return kIoError;
	if (plan.compbufSegs > kHardwareCrbBudgetSegs) return kSkippedBudget;
	uint32_t sum = plan.compbufSegs;
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if (plan.detSegs[p] > kDetSizeMask) return kSkippedBudget;
		sum += plan.detSegs[p];
	}
	if (sum > kHardwareCrbBudgetSegs) return kSkippedBudget;

	uint32_t raw[kNumPipes + 1u] {};
	uint32_t allocated[kNumPipes + 1u] {};
	uint32_t liveSum = 0u;
	for (uint32_t block = 0u; block <= kNumPipes; ++block) {
		const bool comp = block == kNumPipes;
		const uint32_t reg = comp ? kSeg2CompbufCtrl : kSeg2Det0Ctrl + block;
		raw[block] = hw.rdSeg2(hw.cookie, reg);
		if (raw[block] == 0xffffffffu || (comp && (raw[block] & kCompbufCfgErrMask) != 0u)) return kIoError;
		const uint32_t request = raw[block] & kDetSizeMask;
		const uint32_t current = (raw[block] & kDetCurMask) >> kDetCurShift;
		if (plan.disp2Preserved && block < 3u) {
			if (plan.detSegs[block] != kDisp2DetSegs || request != kDisp2DetSegs ||
			    (current != kDisp2DetSegs && (block == 0u || current != 0u))) return kSkippedMultiPipe;
		}
		allocated[block] = request > current ? request : current;
		liveSum += allocated[block];
	}
	if (liveSum > kHardwareCrbBudgetSegs) return kSkippedBudget;

	// Shrinking must be confirmed before any growth can reuse the released segments.
	for (uint32_t phase = 0u; phase < 2u; ++phase) {
		const bool grow = phase == 1u;
		for (uint32_t block = 0u; block <= kNumPipes; ++block) {
			if (plan.disp2Preserved && block < 3u) continue;
			const bool comp = block == kNumPipes;
			const uint32_t target = comp ? plan.compbufSegs : plan.detSegs[block];
			if ((target > allocated[block]) != grow) continue;
			const uint32_t expected = target | (target << kDetCurShift);
			if ((raw[block] & (kDetSizeMask | kDetCurMask)) == expected) continue;
			const uint32_t reg = comp ? kSeg2CompbufCtrl : kSeg2Det0Ctrl + block;
			const uint32_t write = (comp ? raw[block] & kCompbufIntEnable : 0u) | target;
			if (!hw.wrSeg2(hw.cookie, reg, write)) return kIoError;
			bool matched = false;
			for (uint32_t attempt = 0u; attempt < kDetPollMaxIters; ++attempt) {
				const uint32_t value = hw.rdSeg2(hw.cookie, reg);
				if (value == 0xffffffffu || (comp && (value & kCompbufCfgErrMask) != 0u)) return kIoError;
				if ((value & (kDetSizeMask | kDetCurMask)) == expected) {
					matched = true;
					break;
				}
				if (hw.delayUs) hw.delayUs(hw.cookie, 10u);
			}
			if (!matched) return kPollTimeout;
			if (!comp) {
				const uint32_t compValue = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
				if (compValue == UINT32_MAX || (compValue & kCompbufCfgErrMask) != 0u) return kIoError;
			}
		}
	}
	const uint32_t compAfter = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
	return (compAfter != 0xffffffffu && (compAfter & kCompbufCfgErrMask) == 0u) ? kOk : kIoError;
}

struct WatermarkSet {
	uint32_t urgent;
	uint32_t fracUrgBwNom;
	uint32_t fracUrgBwFlip;
	uint32_t fracUrgBwMall;
	uint32_t refcycPerTripToMem;
	uint32_t refcycPerMetaTrip;
	uint32_t usrRetrain;
	uint32_t srEnter;
	uint32_t srExit;
	uint32_t uclkPstate;
	uint32_t uclkPstate1;
	uint32_t fclkPstate;
	uint32_t fclkPstate1;
};

struct HubbubWatermarks {
	WatermarkSet setA;
	WatermarkSet setB;
};

// Inputs are mode-validated reference-cycle and bandwidth values. DCN 4.01 programs both banks
// without changing the running bank selector or requesting a switch.
inline bool program_hubbub_watermarks(const Hw &hw, const HubbubWatermarks &wm, bool safeToLower = false) {
	if (!hw.rdSeg2 || !hw.wrSeg2) return false;
	constexpr uint32_t kBankRegs = kSeg2WmUrgB - kSeg2WmUrgA;
	const uint32_t values[2][kBankRegs] {
		{ wm.setA.urgent, wm.setA.usrRetrain, wm.setA.refcycPerTripToMem, wm.setA.refcycPerMetaTrip,
		  wm.setA.srEnter, wm.setA.srExit, wm.setA.srEnter, wm.setA.srExit,
		  wm.setA.srEnter, wm.setA.srExit, wm.setA.srEnter, wm.setA.srExit,
		  wm.setA.uclkPstate, wm.setA.uclkPstate1, wm.setA.fclkPstate, wm.setA.fclkPstate1,
		  wm.setA.fracUrgBwNom, wm.setA.fracUrgBwFlip, wm.setA.fracUrgBwMall },
		{ wm.setB.urgent, wm.setB.usrRetrain, wm.setB.refcycPerTripToMem, wm.setB.refcycPerMetaTrip,
		  wm.setB.srEnter, wm.setB.srExit, wm.setB.srEnter, wm.setB.srExit,
		  wm.setB.srEnter, wm.setB.srExit, wm.setB.srEnter, wm.setB.srExit,
		  wm.setB.uclkPstate, wm.setB.uclkPstate1, wm.setB.fclkPstate, wm.setB.fclkPstate1,
		  wm.setB.fracUrgBwNom, wm.setB.fracUrgBwFlip, wm.setB.fracUrgBwMall },
	};
	uint32_t unchanged[2][kBankRegs] {};
	for (uint32_t bank = 0u; bank < 2u; ++bank) {
		for (uint32_t idx = 0u; idx < kBankRegs; ++idx) {
			const uint32_t reg = kSeg2WmUrgA + idx;
			const uint32_t mask = reg < kSeg2WmSrEnterA ? 0x3fffu : reg < kSeg2WmFracNomA ? 0xffffu : 0x3ffu;
			const uint32_t old = hw.rdSeg2(hw.cookie, reg + bank * kBankRegs);
			if (old == UINT32_MAX || values[bank][idx] > mask ||
			    (!safeToLower && values[bank][idx] < (old & mask))) return false;
			unchanged[bank][idx] = old & ~mask;
		}
	}
	for (uint32_t bank = 0u; bank < 2u; ++bank) {
		for (uint32_t idx = 0u; idx < kBankRegs; ++idx) {
			const uint32_t reg = kSeg2WmUrgA + bank * kBankRegs + idx;
			const uint32_t value = unchanged[bank][idx] | values[bank][idx];
			if (!hw.wrSeg2(hw.cookie, reg, value) || hw.rdSeg2(hw.cookie, reg) != value) return false;
		}
	}
	return true;
}

struct HubpTtuTiming {
	uint32_t qosLevelLowWm;
	uint32_t qosLevelHighWm;
	uint32_t refcycPerReqDelivery;
	uint32_t qosLevelFixed;
	bool qosRampDisable;
	uint32_t refcycPerReqDeliveryPre;
};

// Program per-pipe HUBPREQ TTU timing & QoS watermarks within DCHUBBUB..CURSOR0 allowlist.
inline bool program_hubp_ttu(const Hw &hw, uint32_t pipe, const HubpTtuTiming &t) {
	if (!hw.rdSeg2 || !hw.wrSeg2 || pipe >= kNumPipes ||
	    t.qosLevelLowWm > t.qosLevelHighWm || t.qosLevelHighWm > 0x3fffu ||
	    t.refcycPerReqDelivery > 0x7fffffu || t.qosLevelFixed > 0xfu ||
	    t.refcycPerReqDeliveryPre > 0x7fffffu) return false;
	const uint32_t hb = pipe * kSeg2HubpStride;
	const uint32_t regs[] { kSeg2HubpReq0TtuQosWm + hb, kSeg2HubpReq0Ttu0 + hb, kSeg2HubpReq0Ttu1 + hb };
	const uint32_t masks[] { 0x3fff3fffu, 0x1f7fffffu, 0x007fffffu };
	uint32_t values[] {
		t.qosLevelLowWm | (t.qosLevelHighWm << 16),
		t.refcycPerReqDelivery | (t.qosLevelFixed << 24) | (t.qosRampDisable ? (1u << 28) : 0u),
		t.refcycPerReqDeliveryPre,
	};
	for (uint32_t idx = 0u; idx < 3u; ++idx) {
		const uint32_t old = hw.rdSeg2(hw.cookie, regs[idx]);
		if (old == UINT32_MAX) return false;
		values[idx] |= old & ~masks[idx];
	}
	for (uint32_t idx = 0u; idx < 3u; ++idx) {
		if (!hw.wrSeg2(hw.cookie, regs[idx], values[idx]) || hw.rdSeg2(hw.cookie, regs[idx]) != values[idx]) return false;
	}
	return true;
}

inline bool clear_optc0_underflow_before_unpin(const Hw &hw) {
	if (!hw.rdSeg2 || !hw.wrSeg2) return false;
	const uint32_t initial = hw.rdSeg2(hw.cookie, kSeg2Optc0Global);
	if (initial == UINT32_MAX || (initial & kOptcSoftResetBit)) return false;
	const uint32_t base = initial & ~kOptcWriteClearMask;
	if (!hw.wrSeg2(hw.cookie, kSeg2Optc0Global, base | kOptcUnderflowClear) ||
	    !hw.wrSeg2(hw.cookie, kSeg2Optc0Global, base)) return false;
	const uint32_t armed = hw.rdSeg2(hw.cookie, kSeg2Optc0Global);
	return (armed & (kOptcUnderflowMask | kOptcUnderflowClear | kOptcSoftResetBit)) == 0u;
}

inline bool probe_optc0_underflow(const Hw &hw, uint32_t dwellUs, uint32_t *outReg = nullptr) {
	if (!hw.rdSeg2 || (dwellUs && !hw.delayUs)) return true;
	for (;;) {
		const uint32_t observed = hw.rdSeg2(hw.cookie, kSeg2Optc0Global);
		if (outReg) *outReg = observed;
		if (observed & (kOptcUnderflowMask | kOptcUnderflowClear | kOptcSoftResetBit)) return true;
		if (!dwellUs) return false;
		const uint32_t stepUs = dwellUs < 1000u ? dwellUs : 1000u;
		hw.delayUs(hw.cookie, stepUs);
		dwellUs -= stepUs;
	}
}

}  // namespace rdna4det
