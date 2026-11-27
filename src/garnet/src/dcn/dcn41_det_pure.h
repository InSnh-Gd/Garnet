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
constexpr uint32_t kCrbSegmentBytes       = 65536u;    // 64 KiB per CRB segment
constexpr uint32_t kDefaultCompbufSegs    = 5u;        // DCN4_01_DEFAULT_DET_SIZE (320 KiB = 5 segs)
constexpr uint32_t kDisp2Det0Segs         = 3u;        // disp2 invariant: DET0 = 0x303
constexpr uint32_t kDisp2PipeDetSegs      = 3u;        // disp2 invariant: DET1/DET2 = 3
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
	if (compRaw == 0xffffffffu || det0Raw == 0xffffffffu) { res.status = kIoError; return res; }
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
	uint32_t compbufReserved64B { 64u }; // 2048 B / 32 = 64 (4096 B)
	bool disp2Preserved { false };
};

// Distribute 21-segment (1344 KiB) hardware CRB across active pipes and COMPBUF,
// preserving disp2 DET0=0x303 and DET1/2=3 invariants when `disp2Active` is true.
inline CrbPlan plan_crb_distribution(uint32_t activePipeMask,
                                     uint32_t compbufSegs = kDefaultCompbufSegs,
                                     uint32_t crbBudgetSegs = kHardwareCrbBudgetSegs,
                                     bool disp2Active = false) {
	CrbPlan plan {};
	const uint32_t mask = activePipeMask & ((1u << kNumPipes) - 1u);
	if (mask == 0u || compbufSegs >= crbBudgetSegs || crbBudgetSegs > 31u) {
		plan.status = kSkippedBudget;
		return plan;
	}

	uint32_t activeCount = 0u;
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if ((mask & (1u << p)) != 0u) ++activeCount;
	}
	uint32_t remaining = crbBudgetSegs - compbufSegs;

	if (disp2Active) {
		// Preserve UEFI/disp2 invariant: keep COMPBUF modest if needed, DET0 >= 3 (0x303),
		// and DET1/DET2 == 3 on active disp2 pipes.
		if (remaining < activeCount * kDisp2PipeDetSegs) {
			plan.status = kSkippedBudget;
			return plan;
		}
		if ((mask & 1u) != 0u) {
			plan.detSegs[0] = kDisp2Det0Segs;
			remaining -= kDisp2Det0Segs;
		}
		for (uint32_t p = 1u; p < kNumPipes; ++p) {
			if ((mask & (1u << p)) != 0u) {
				plan.detSegs[p] = kDisp2PipeDetSegs;
				remaining -= kDisp2PipeDetSegs;
			}
		}
		// Give any remaining budget above the 3-seg floor only to non-disp2 pipes or DET0 if idle,
		// keeping secondary disp2 pipes strictly at 3 segments.
		if ((mask & 1u) != 0u && remaining > 0u) {
			plan.detSegs[0] += remaining;
		}
		plan.compbufSegs = compbufSegs;
		plan.disp2Preserved = true;
		plan.status = kOk;
		return plan;
	}

	const uint32_t base = remaining / activeCount;
	uint32_t extra = remaining % activeCount;
	if (base == 0u) {
		plan.status = kSkippedBudget;
		return plan;
	}
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if ((mask & (1u << p)) == 0u) continue;
		uint32_t seg = base + (extra > 0u ? 1u : 0u);
		if (extra > 0u) --extra;
		if (seg > kDetSizeMask) seg = kDetSizeMask;
		plan.detSegs[p] = seg;
	}
	plan.compbufSegs = compbufSegs;
	plan.status = kOk;
	return plan;
}

// Program multi-pipe DET & COMPBUF across all 4 pipes using strict shrink-before-grow ordering.
inline Status program_multi_pipe_det(const Hw &hw, const CrbPlan &plan,
                                     uint32_t crbBudgetSegs = kHardwareCrbBudgetSegs) {
	if (!hw.rdSeg2 || !hw.wrSeg2 || plan.status != kOk) return kIoError;
	uint32_t sum = plan.compbufSegs;
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if (plan.detSegs[p] > kDetSizeMask) return kSkippedBudget;
		sum += plan.detSegs[p];
	}
	if (sum > crbBudgetSegs) return kSkippedBudget;

	const uint32_t compRaw = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
	if (compRaw == 0xffffffffu) return kIoError;
	const uint32_t compCur = (compRaw & kDetCurMask) >> kDetCurShift;

	uint32_t detRaw[kNumPipes] {};
	uint32_t detCur[kNumPipes] {};
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		detRaw[p] = hw.rdSeg2(hw.cookie, kSeg2Det0Ctrl + p);
		if (detRaw[p] == 0xffffffffu) return kIoError;
		detCur[p] = (detRaw[p] & kDetCurMask) >> kDetCurShift;
	}

	// Phase 1: shrink COMPBUF or any DET pipe whose target < current first.
	if (plan.compbufSegs < compCur) {
		if (!hw.wrSeg2(hw.cookie, kSeg2CompbufCtrl,
		               (compRaw & ~kDetSizeMask) | (plan.compbufSegs & kDetSizeMask))) {
			return kIoError;
		}
		if (!wait_crb_current(hw, kSeg2CompbufCtrl, plan.compbufSegs)) return kPollTimeout;
	}
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if (plan.detSegs[p] < detCur[p] ||
		    (plan.detSegs[p] == 0u && (detRaw[p] & kDetSizeMask) != 0u)) {
			if (!hw.wrSeg2(hw.cookie, kSeg2Det0Ctrl + p,
			               (detRaw[p] & ~kDetSizeMask) | (plan.detSegs[p] & kDetSizeMask))) {
				return kIoError;
			}
			if (!wait_det_current(hw, p, plan.detSegs[p])) return kPollTimeout;
		}
	}

	// Phase 2: grow COMPBUF and DET pipes, then set COMPBUF_RESERVED_SPACE and verify CONFIG_ERROR == 0.
	if (plan.compbufSegs > compCur || (compRaw & kDetSizeMask) != plan.compbufSegs) {
		const uint32_t cLatest = hw.rdSeg2(hw.cookie, kSeg2CompbufCtrl);
		if (cLatest == 0xffffffffu ||
		    !hw.wrSeg2(hw.cookie, kSeg2CompbufCtrl,
		               (cLatest & ~kDetSizeMask) | (plan.compbufSegs & kDetSizeMask))) {
			return kIoError;
		}
		if (!wait_crb_current(hw, kSeg2CompbufCtrl, plan.compbufSegs)) return kPollTimeout;
	}
	for (uint32_t p = 0u; p < kNumPipes; ++p) {
		if (plan.detSegs[p] > detCur[p]) {
			const uint32_t latest = hw.rdSeg2(hw.cookie, kSeg2Det0Ctrl + p);
			if (latest == 0xffffffffu ||
			    !hw.wrSeg2(hw.cookie, kSeg2Det0Ctrl + p,
			               (latest & ~kDetSizeMask) | (plan.detSegs[p] & kDetSizeMask))) {
				return kIoError;
			}
			if (!wait_det_current(hw, p, plan.detSegs[p])) return kPollTimeout;
		}
	}
	(void)hw.wrSeg2(hw.cookie, kSeg2CompbufRsvSpace, plan.compbufReserved64B & 0xfffu);
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
	bool selectSetB { false };
};

// Program DCHUBBUB ARB urgent, stutter, UCLK/FCLK P-state (sets A & B) and trigger
// atomic watermark change request (dcn401_hubbub.c layout: Set B sits +0x13 above Set A).
inline bool program_hubbub_watermarks(const Hw &hw, const HubbubWatermarks &wm) {
	if (!hw.rdSeg2 || !hw.wrSeg2) return false;
	auto progSet = [&](uint32_t baseDelta, const WatermarkSet &s) -> bool {
		bool ok = true;
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmUrgA + baseDelta, s.urgent & 0x3fffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmUsrA + baseDelta, s.usrRetrain & 0x3fffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmTripMemA + baseDelta, s.refcycPerTripToMem & 0x3fffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmMetaTripA + baseDelta, s.refcycPerMetaTrip & 0x3fffu);
		for (uint32_t i = 0u; i < 4u; ++i) {
			ok &= hw.wrSeg2(hw.cookie, kSeg2WmSrEnterA + baseDelta + i * 2u, s.srEnter & 0xffffu);
			ok &= hw.wrSeg2(hw.cookie, kSeg2WmSrExitA + baseDelta + i * 2u, s.srExit & 0xffffu);
		}
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmUclkPstateA + baseDelta, s.uclkPstate & 0xffffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmUclkPstate1A + baseDelta, s.uclkPstate1 & 0xffffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmFclkPstateA + baseDelta, s.fclkPstate & 0xffffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmFclkPstate1A + baseDelta, s.fclkPstate1 & 0xffffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmFracNomA + baseDelta, s.fracUrgBwNom & 0x3ffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmFracFlipA + baseDelta, s.fracUrgBwFlip & 0x3ffu);
		ok &= hw.wrSeg2(hw.cookie, kSeg2WmFracMallA + baseDelta, s.fracUrgBwMall & 0x3ffu);
		return ok;
	};
	if (!progSet(0u, wm.setA) || !progSet(0x13u, wm.setB)) return false;

	const uint32_t selMask = (1u << 0) | (1u << 25) | (1u << 26); // WM/UCLK/FCLK set select
	const uint32_t cur = hw.rdSeg2(hw.cookie, kSeg2WmChangeCntl);
	const uint32_t next = (cur == 0xffffffffu ? 0u : cur) & ~selMask;
	const uint32_t armed = next | (wm.selectSetB ? selMask : 0u);
	if (!hw.wrSeg2(hw.cookie, kSeg2WmChangeCntl, armed)) return false;
	return hw.wrSeg2(hw.cookie, kSeg2WmChangeCntl, armed | (1u << 8)); // WATERMARK_CHANGE_REQUEST
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
	if (!hw.wrSeg2 || pipe >= kNumPipes) return false;
	const uint32_t hb = pipe * kSeg2HubpStride;
	bool ok = true;
	ok &= hw.wrSeg2(hw.cookie, kSeg2HubpReq0TtuQosWm + hb,
	                (t.qosLevelLowWm & 0x3fffu) | ((t.qosLevelHighWm & 0x3fffu) << 16));
	ok &= hw.wrSeg2(hw.cookie, kSeg2HubpReq0Ttu0 + hb,
	                (t.refcycPerReqDelivery & 0x7fffffu) |
	                ((t.qosLevelFixed & 0xfu) << 24) |
	                (t.qosRampDisable ? (1u << 28) : 0u));
	ok &= hw.wrSeg2(hw.cookie, kSeg2HubpReq0Ttu1 + hb, t.refcycPerReqDeliveryPre & 0x7fffffu);
	return ok;
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
