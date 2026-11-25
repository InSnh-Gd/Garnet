// dcn41_modeset.h — Pure DCN 4.1 hardware mode-switch planner, VESA DSC 1.2a
// integer PPS calculator, DP 1.4 link retraining engine (HBR2/HBR3 + FEC), and
// live OTG/DTO/MSA/HUBP/DSC switcher with automatic register rollback.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dp_edid_aux.h"
#include "edid_modes_pure.h"

namespace rdna4modeset {

// ---- DCN 4.1 register offsets (dcn_4_1_0_offset.h) ----
// Segment 1 (BASE_IDX 1, default 0xc0)
constexpr uint32_t kSeg1DscClk0DtoParam  = 0x006cu; // DSCCLK0_DTO_PARAM (stride 1)
constexpr uint32_t kSeg1OtgPixRateDiv    = 0x006fu; // OTG_PIXEL_RATE_DIV (DPDTO0_INT bits [4:1])
constexpr uint32_t kSeg1Otg0PixRateCntl  = 0x0080u; // OTG0_PIXEL_RATE_CNTL (stride 4)
constexpr uint32_t kSeg1DpDto0Phase      = 0x0081u; // DP_DTO0_PHASE (stride 4)
constexpr uint32_t kSeg1DpDto0Modulo     = 0x0082u; // DP_DTO0_MODULO (stride 4)
constexpr uint32_t kSeg1SymClkAClkEn     = 0x00a0u; // SYMCLKA_CLOCK_ENABLE (stride 1)
constexpr uint32_t kSeg1DscClkDtoCtrl    = 0x00a7u; // DSCCLK_DTO_CTRL

// Segment 2 (BASE_IDX 2, default 0x34c0)
constexpr uint32_t kSeg2Domain16PgCfg    = 0x0089u; // DOMAIN16_PG_CONFIG (DSC0..3 stride 2; FORCEON[0], GATE[8])
constexpr uint32_t kSeg2Domain16PgStat   = 0x008au; // DOMAIN16_PG_STATUS (PGFSM_PWR_STATUS[31:30]: 0=on, 2=off)
constexpr uint32_t kSeg2DcIpReqCntl      = 0x00a0u; // DC_IP_REQUEST_CNTL (IP_REQUEST_EN[0])
constexpr uint32_t kSeg2Vtg0Control      = 0x0530u; // VTG0_CONTROL (abs 0x39f0; stride 1)
constexpr uint32_t kSeg2Hubp0Cntl        = 0x05f4u; // HUBP0_DCHUBP_CNTL (abs 0x3ab4)
constexpr uint32_t kSeg2HubpReq0TtuQosWm = 0x0621u; // HUBPREQ0_DCN_TTU_QOS_WM (abs 0x3ae1)
constexpr uint32_t kSeg2HubpReq0SurfTtu0 = 0x0623u; // HUBPREQ0_DCN_SURF0_TTU_CNTL0 (abs 0x3ae3)
constexpr uint32_t kSeg2HubpReq0SurfTtu1 = 0x0624u; // HUBPREQ0_DCN_SURF0_TTU_CNTL1 (abs 0x3ae4)
constexpr uint32_t kSeg2HubpReq0Blank0   = 0x063bu; // HUBPREQ0_BLANK_OFFSET_0 (abs 0x3afb)
constexpr uint32_t kSeg2HubpReq0Blank1   = 0x063cu; // HUBPREQ0_BLANK_OFFSET_1 (abs 0x3afc)
constexpr uint32_t kSeg2HubpReq0DstDim   = 0x063du; // HUBPREQ0_DST_DIMENSIONS (abs 0x3afd)
constexpr uint32_t kSeg2HubpReq0AfterSc  = 0x063eu; // HUBPREQ0_DST_AFTER_SCALER (abs 0x3afe)
constexpr uint32_t kSeg2HubpReq0Vblank1  = 0x0642u; // HUBPREQ0_VBLANK_PARAMETERS_1 (abs 0x3b02)
constexpr uint32_t kSeg2HubpReq0Nom1     = 0x064au; // HUBPREQ0_NOM_PARAMETERS_1 (abs 0x3b0a)
constexpr uint32_t kSeg2HubpReq0PerLineP = 0x0651u; // HUBPREQ0_PER_LINE_DELIVERY_PRE (abs 0x3b11)
constexpr uint32_t kSeg2HubpReq0PerLine  = 0x0652u; // HUBPREQ0_PER_LINE_DELIVERY (abs 0x3b12)
constexpr uint32_t kSeg2HubpReq0RefToPix = 0x0654u; // HUBPREQ0_REF_FREQ_TO_PIX_FREQ (abs 0x3b14)
constexpr uint32_t kSeg2HubpStride       = 0x00dcu; // HUBP instance stride

constexpr uint32_t kSeg2Dscrm0Forward    = 0x1a64u; // DSCRM0_DSCRM_DSC_FORWARD_CONFIG (stride 1)
constexpr uint32_t kSeg2OptcInGlobal     = 0x1acau; // ODM0_OPTC_INPUT_GLOBAL_CONTROL (abs 0x4f8a, stride 0x10)
constexpr uint32_t kSeg2OptcDataSrcSel   = 0x1acbu; // ODM0_OPTC_DATA_SOURCE_SELECT
constexpr uint32_t kSeg2OptcDataFmt      = 0x1accu; // ODM0_OPTC_DATA_FORMAT_CONTROL
constexpr uint32_t kSeg2OptcBytesPerPx   = 0x1acdu; // ODM0_OPTC_BYTES_PER_PIXEL
constexpr uint32_t kSeg2OptcWidthCtrl    = 0x1aceu; // ODM0_OPTC_WIDTH_CONTROL
constexpr uint32_t kSeg2OptcInClkCtrl    = 0x1ad0u; // ODM0_OPTC_INPUT_CLOCK_CONTROL (GATE_DIS[0], EN[1], CLK_ON[2])
constexpr uint32_t kSeg2OptcMemCfg       = 0x1ad1u; // ODM0_OPTC_MEMORY_CONFIG
constexpr uint32_t kSeg2OdmStride        = 0x0010u; // ODM/OPTC instance stride (ODM1 = ODM0 + 0x10)

constexpr uint32_t kSeg2Otg0HTotal       = 0x1b2au; // OTG0_OTG_H_TOTAL (abs 0x4fea, stride 0x80)
constexpr uint32_t kSeg2Otg0HBlankSE     = 0x1b2bu; // OTG0_OTG_H_BLANK_START_END
constexpr uint32_t kSeg2Otg0HSyncA       = 0x1b2cu; // OTG0_OTG_H_SYNC_A
constexpr uint32_t kSeg2Otg0HSyncACntl   = 0x1b2du; // OTG0_OTG_H_SYNC_A_CNTL
constexpr uint32_t kSeg2Otg0HTimingCntl  = 0x1b2eu; // OTG0_OTG_H_TIMING_CNTL
constexpr uint32_t kSeg2Otg0VTotal       = 0x1b2fu; // OTG0_OTG_V_TOTAL
constexpr uint32_t kSeg2Otg0VTotalMin    = 0x1b30u; // OTG0_OTG_V_TOTAL_MIN
constexpr uint32_t kSeg2Otg0VTotalMax    = 0x1b31u; // OTG0_OTG_V_TOTAL_MAX
constexpr uint32_t kSeg2Otg0VBlankSE     = 0x1b38u; // OTG0_OTG_V_BLANK_START_END
constexpr uint32_t kSeg2Otg0VSyncA       = 0x1b39u; // OTG0_OTG_V_SYNC_A
constexpr uint32_t kSeg2Otg0VSyncACntl   = 0x1b3au; // OTG0_OTG_V_SYNC_A_CNTL
constexpr uint32_t kSeg2Otg0Control      = 0x1b43u; // OTG0_OTG_CONTROL (MASTER_EN[0], DIS_PT[9:8], CUR_EN[16])
constexpr uint32_t kSeg2Otg0StatusPos    = 0x1b4au; // OTG0_OTG_STATUS_POSITION (abs 0x500a)
constexpr uint32_t kSeg2Otg0StatusFrame  = 0x1b4du; // OTG0_OTG_STATUS_FRAME_COUNT (abs 0x500d)
constexpr uint32_t kSeg2Otg0DbCtrl       = 0x1b5cu; // OTG0_OTG_DOUBLE_BUFFER_CONTROL (abs 0x501c)
constexpr uint32_t kSeg2Otg0ClkCtrl      = 0x1b84u; // OTG0_OTG_CLOCK_CONTROL (EN[0], GATE_DIS[1], CLK_ON[8], BUSY[16])
constexpr uint32_t kSeg2Otg0VStartup     = 0x1b85u; // OTG0_OTG_VSTARTUP_PARAM (abs 0x5045)
constexpr uint32_t kSeg2Otg0VUpdate      = 0x1b86u; // OTG0_OTG_VUPDATE_PARAM (abs 0x5046)
constexpr uint32_t kSeg2Otg0VReady       = 0x1b87u; // OTG0_OTG_VREADY_PARAM (abs 0x5047)
constexpr uint32_t kSeg2Otg0MasterLock   = 0x1b89u; // OTG0_OTG_MASTER_UPDATE_LOCK (abs 0x5049)
constexpr uint32_t kSeg2Otg0PipeUpd      = 0x1b9eu; // OTG0_OTG_PIPE_UPDATE_STATUS (abs 0x505e)
constexpr uint32_t kSeg2Otg0PstateReg    = 0x1b9fu; // OTG0_OTG_PSTATE_REGISTER (abs 0x505f)
constexpr uint32_t kSeg2OtgStride        = 0x0080u;

// RX4Darwin/AMDRadeonRDNA4.kext PR #17 (docs/emu-linux.md, BSD 3-Clause,
// Copyright (c) 2026 Sunneva N. Mariu): reading ODM_MEM_PWR_CTRL2 (seg2 + 0x1e2f,
// abs 0x000052ef) causes an immediate Navi 44 (RX 9060 XT) bus-loss freeze.
constexpr uint32_t kSeg2OdmMemPwrCtrl2   = 0x1e2fu;
constexpr bool is_read_hazard_seg(uint32_t segIdx, uint32_t off) {
	return segIdx == 2u && off == kSeg2OdmMemPwrCtrl2;
}

constexpr uint32_t kSeg2Dig0StreamMap    = 0x1f0du; // DIG0_STREAM_MAPPER_CONTROL (stride 1)
constexpr uint32_t kSeg2Vpg0AccessCtrl   = 0x2068u; // VPG0_VPG_GENERIC_PACKET_ACCESS_CTRL (stride 0x124)
constexpr uint32_t kSeg2Vpg0PacketData   = 0x2069u; // VPG0_VPG_GENERIC_PACKET_DATA
constexpr uint32_t kSeg2Vpg0ImmUpdate    = 0x206bu; // VPG0_VPG_GSP_IMMEDIATE_UPDATE_CTRL
constexpr uint32_t kSeg2Vpg0Status       = 0x206cu; // VPG0_VPG_GENERIC_STATUS
constexpr uint32_t kSeg2Vpg0MemPwr       = 0x206du; // VPG0_VPG_MEM_PWR (LIGHT_SLEEP_DIS[0], FORCE[4], STATE[8])

constexpr uint32_t kSeg2Dig0FeCntl       = 0x2093u; // DIG0_DIG_FE_CNTL (stride 0x124)
constexpr uint32_t kSeg2Dig0FeClkCntl    = 0x2094u; // DIG0_DIG_FE_CLK_CNTL
constexpr uint32_t kSeg2Dig0FeEnCntl     = 0x2095u; // DIG0_DIG_FE_EN_CNTL
constexpr uint32_t kSeg2Dig0FifoCtrl0    = 0x209bu; // DIG0_DIG_FIFO_CTRL0
constexpr uint32_t kSeg2Dig0AfmtCntl     = 0x20bau; // DIG0_AFMT_CNTL (AFMT_AUDIO_CLOCK_EN bit 0)
constexpr uint32_t kSeg2Dig0BeClkCntl    = 0x20bbu; // DIG0_DIG_BE_CLK_CNTL
constexpr uint32_t kSeg2Dig0BeCntl       = 0x20bcu; // DIG0_DIG_BE_CNTL
constexpr uint32_t kSeg2Dig0BeEnCntl     = 0x20bdu; // DIG0_DIG_BE_EN_CNTL
constexpr uint32_t kSeg2Dp0LinkCntl      = 0x211eu; // DP0_DP_LINK_CNTL
constexpr uint32_t kSeg2Dp0PixelFormat   = 0x211fu; // DP0_DP_PIXEL_FORMAT
constexpr uint32_t kSeg2Dp0MsaColorim    = 0x2120u; // DP0_DP_MSA_COLORIMETRY
constexpr uint32_t kSeg2Dp0Config        = 0x2121u; // DP0_DP_CONFIG
constexpr uint32_t kSeg2Dp0VidStreamCntl = 0x2122u; // DP0_DP_VID_STREAM_CNTL
constexpr uint32_t kSeg2Dp0SteerFifo     = 0x2123u; // DP0_DP_STEER_FIFO
constexpr uint32_t kSeg2Dp0MsaMisc       = 0x2124u; // DP0_DP_MSA_MISC
constexpr uint32_t kSeg2Dp0DphyIntCtrl   = 0x2125u; // DP0_DP_DPHY_INTERNAL_CTRL
constexpr uint32_t kSeg2Dp0VidTiming     = 0x2126u; // DP0_DP_VID_TIMING
constexpr uint32_t kSeg2Dp0VidN          = 0x2127u; // DP0_DP_VID_N
constexpr uint32_t kSeg2Dp0VidM          = 0x2128u; // DP0_DP_VID_M
constexpr uint32_t kSeg2Dp0LinkFraming   = 0x2129u; // DP0_DP_LINK_FRAMING_CNTL
constexpr uint32_t kSeg2Dp0DphyCntl      = 0x212du; // DP0_DP_DPHY_CNTL (FEC bits 4/5/6, BYPASS bit 16)
constexpr uint32_t kSeg2Dp0TrainPatSel   = 0x212eu; // DP0_DP_DPHY_TRAINING_PATTERN_SEL
constexpr uint32_t kSeg2Dp0DphyPrbsCntl  = 0x2133u; // DP0_DP_DPHY_PRBS_CNTL
constexpr uint32_t kSeg2Dp0ScramCntl     = 0x2134u; // DP0_DP_DPHY_SCRAM_CNTL
constexpr uint32_t kSeg2Dp0SecCntl       = 0x2141u; // DP0_DP_SEC_CNTL
constexpr uint32_t kSeg2Dp0MsaParam1     = 0x2162u; // DP0_DP_MSA_TIMING_PARAM1 (DP1 = 0x5746)
constexpr uint32_t kSeg2Dp0MsaParam2     = 0x2163u; // DP0_DP_MSA_TIMING_PARAM2
constexpr uint32_t kSeg2Dp0MsaParam3     = 0x2164u; // DP0_DP_MSA_TIMING_PARAM3
constexpr uint32_t kSeg2Dp0MsaParam4     = 0x2165u; // DP0_DP_MSA_TIMING_PARAM4
constexpr uint32_t kSeg2Dp0SecCntl2      = 0x2169u; // DP0_DP_SEC_CNTL2 (GSP11_PPS)
constexpr uint32_t kSeg2Dp0MsaVbidMisc   = 0x2170u; // DP0_DP_MSA_VBID_MISC (VBID6_LINE_REF[15], LINE_NUM[31:16])
constexpr uint32_t kSeg2Dp0Gsp11Cntl     = 0x2177u; // DP0_DP_GSP11_CNTL
constexpr uint32_t kSeg2DigStride        = 0x0124u;

constexpr uint32_t kSeg2DscTop0Ctrl      = 0x3000u; // DSC_TOP0_DSC_TOP_CONTROL (stride 0x5c)
constexpr uint32_t kSeg2Dsccif0Cfg0      = 0x3005u; // DSCCIF0_DSCCIF_CONFIG0
constexpr uint32_t kSeg2Dscc0Cfg0        = 0x300au; // DSCC0_DSCC_CONFIG0
constexpr uint32_t kSeg2Dscc0Cfg1        = 0x300bu; // DSCC0_DSCC_CONFIG1
constexpr uint32_t kSeg2Dscc0Status      = 0x300du; // DSCC0_DSCC_STATUS
constexpr uint32_t kSeg2Dscc0Pps0        = 0x3012u; // DSCC0_DSCC_PPS_CONFIG0..22
constexpr uint32_t kSeg2DscStride        = 0x005cu;

enum class LinkTier : uint32_t {
	Unsupported = 0u,
	Current8bpc = 1u, // Keep current boot link rate, 8 bpc RGB uncompressed
	Hbr3_8bpc   = 2u, // Retrain 4-lane HBR3 (0x1e), 8 bpc RGB uncompressed
	Hbr3_Dsc12  = 3u, // Retrain 4-lane HBR3 (0x1e) + FEC + DSC 1.2a (8 bpc RGB, 12.0 bpp)
};

enum Status : uint32_t {
	kOk = 0u,
	kInvalid = 1u,
	kUnsupported = 2u,
	kClockFailed = 3u,
	kBlankTimeout = 4u,
	kLinkTrainFailed = 5u,
	kLockTimeout = 6u,
	kResyncFailed = 7u,
	kUnblankTimeout = 8u,
	kRateMismatch = 9u,
	kUnderflow = 10u,
};

struct ModePlan {
	LinkTier tier;
	uint8_t targetLinkRate;   // DPCD link BW code (0x14=HBR2, 0x1e=HBR3)
	uint8_t targetLanes;      // 1, 2, or 4
	bool useDsc;
	bool useFec;
	uint32_t dscSlicesH;      // 2 or 4 horizontal slices when useDsc
	uint32_t dscSliceHeight;  // e.g. 108 for 2160p
	uint32_t dscBppX16;       // 12 * 16 = 192 (12.0 bpp)
	uint32_t needDispClkKhz;
	uint32_t needDppClkKhz;
};

struct DscParams {
	uint32_t picWidth;
	uint32_t picHeight;
	uint32_t sliceWidth;
	uint32_t sliceHeight;
	uint32_t numSlicesH;
	uint32_t numSlicesV;
	uint32_t bpc;
	uint32_t bppX16;
	uint32_t lineBufDepth;
	uint32_t sliceChunkSize;
	uint32_t initialXmitDelay;
	uint32_t initialDecDelay;
	uint32_t initialScaleValue;
	uint32_t scaleIncrementInterval;
	uint32_t scaleDecrementInterval;
	uint32_t firstLineBpgOffset;
	uint32_t nflBpgOffset;
	uint32_t sliceBpgOffset;
	uint32_t initialOffset;
	uint32_t finalOffset;
	uint32_t flatnessMinQp;
	uint32_t flatnessMaxQp;
	uint32_t rcModelSize;
	uint32_t rcEdgeFactor;
	uint32_t rcQuantIncrLimit0;
	uint32_t rcQuantIncrLimit1;
	uint32_t rcTgtOffsetHi;
	uint32_t rcTgtOffsetLo;
	uint32_t rcBufferModelSize; // rc_bits for DSCC_CONFIG1
	uint32_t optcBytesPerPixel; // u3.28 fixed point for OPTC_DSC_BYTES_PER_PIXEL
	uint8_t rcBufThresh[14];
	uint8_t rangeMinQp[15];
	uint8_t rangeMaxQp[15];
	int8_t rangeBpgOffset[15];
};

// Choose horizontal slice count supported by sink DPCD 0x0064/0x006c for a given width.
inline uint32_t chooseDscSlicesH(uint32_t width, const rdna4aux::DpcdCaps &dpcd) {
	// DPCD 0x0064: bit 1 = 2 slices, bit 3 = 4 slices. DPCD 0x006c: max slice width * 320 px.
	const uint32_t maxSliceW = dpcd.dscMaxSliceWidth ? static_cast<uint32_t>(dpcd.dscMaxSliceWidth) * 320u : 2560u;
	if ((width % 4u) == 0u && (dpcd.dscSliceCap1 & 0x08u) != 0u && (width / 4u) <= maxSliceW) {
		return 4u;
	}
	if ((width % 2u) == 0u && (dpcd.dscSliceCap1 & 0x02u) != 0u && (width / 2u) <= maxSliceW) {
		return 2u;
	}
	// Fallback: if sink advertises DSC but left slice cap bits minimal, 4 slices on 4K (960px) is VESA standard.
	if ((width % 4u) == 0u && width >= 2560u) return 4u;
	if ((width % 2u) == 0u) return 2u;
	return 1u;
}

inline uint32_t chooseDscSliceHeight(uint32_t height) {
	static constexpr uint32_t kPreferred[] = { 108u, 120u, 96u, 72u, 60u, 48u, 36u, 32u, 16u, 8u };
	for (uint32_t h : kPreferred) {
		if (h <= height && (height % h) == 0u) return h;
	}
	return 8u;
}

// Plan the exact hardware link, clock, and compression tier for a target timing.
// ponytail: single-pipe 1-pixel/clock envelope and 12.0 bpp fixed DSC target (visually lossless 2:1);
// upgrade path for >maxDppClkKhz (e.g. 5K/8K@160+) is 2:1 ODM combine.
inline ModePlan planMode(const rdna4edidmodes::DetailedTiming &t,
                         const rdna4aux::DpcdCaps &dpcd,
                         uint32_t maxDispClkKhz, uint32_t maxDppClkKhz,
                         bool allowRetrain = true, bool allowDsc = true) {
	ModePlan p {};
	if (!t.width || !t.height || !t.pixelClockHz) return p;

	const uint32_t pclkKhz = static_cast<uint32_t>((t.pixelClockHz + 999ULL) / 1000ULL);
	p.needDispClkKhz = static_cast<uint32_t>((static_cast<uint64_t>(pclkKhz) * 105ULL + 99ULL) / 100ULL);
	p.needDppClkKhz  = static_cast<uint32_t>((static_cast<uint64_t>(pclkKhz) * 103ULL + 99ULL) / 100ULL);

	if (maxDispClkKhz > 0u && p.needDispClkKhz > maxDispClkKhz) return p;
	if (maxDppClkKhz > 0u && p.needDppClkKhz > maxDppClkKhz) return p;

	const uint8_t curRate = (dpcd.valid && dpcd.curLinkBwSet) ? dpcd.curLinkBwSet : 0x14u;
	const uint8_t curLanes = (dpcd.valid && dpcd.curLaneCountSet) ? dpcd.curLaneCountSet : 4u;
	const uint8_t baseMaxRate = (dpcd.valid && dpcd.maxLinkRate) ? dpcd.maxLinkRate : curRate;
	const uint8_t baseMaxLanes = (dpcd.valid && dpcd.maxLaneCount) ? dpcd.maxLaneCount : curLanes;
	const uint8_t maxRate = (dpcd.valid && dpcd.extMaxLinkRate > baseMaxRate) ? dpcd.extMaxLinkRate : baseMaxRate;
	const uint8_t maxLanes = (dpcd.valid && dpcd.extMaxLaneCount > baseMaxLanes) ? dpcd.extMaxLaneCount : baseMaxLanes;

	// Required 8 bpc RGB uncompressed bandwidth (kbps) with 0.5% framing overhead safety margin.
	const uint64_t rawRgbKbps = rdna4aux::requiredRgbKbps(t.pixelClockHz, 8u);
	const uint64_t curCapKbps = rdna4aux::payloadKbps8b10b(curRate, curLanes);
	if (curCapKbps > 0ULL && rawRgbKbps * 1005ULL <= curCapKbps * 1000ULL) {
		p.tier = LinkTier::Current8bpc;
		p.targetLinkRate = curRate;
		p.targetLanes = curLanes;
		return p;
	}

	// Tier 2: Uncompressed 8 bpc RGB over 4-lane HBR3 (25.92 Gbps payload -> up to ~1075 MHz = 4K@120Hz).
	if (allowRetrain && maxRate >= 0x1eu && maxLanes >= 4u) {
		const uint64_t hbr3CapKbps = rdna4aux::payloadKbps8b10b(0x1eu, 4u);
		if (rawRgbKbps * 1003ULL <= hbr3CapKbps * 1000ULL) {
			p.tier = LinkTier::Hbr3_8bpc;
			p.targetLinkRate = 0x1eu;
			p.targetLanes = 4u;
			return p;
		}
	}

	// Tier 3: 4-lane HBR3 + FEC (97% net = 25.14 Gbps) + DSC 1.2a @ 12.0 bpp (up to ~2050 MHz pixel clock!).
	if (allowRetrain && allowDsc && dpcd.valid && dpcd.dscSupported &&
	    maxRate >= 0x1eu && maxLanes >= 4u) {
		const uint64_t hbr3FecCapKbps = (rdna4aux::payloadKbps8b10b(0x1eu, 4u) * 97ULL) / 100ULL;
		const uint64_t dsc12Kbps = (t.pixelClockHz * 12ULL + 999ULL) / 1000ULL;
		if (dsc12Kbps * 102ULL <= hbr3FecCapKbps * 100ULL) {
			p.tier = LinkTier::Hbr3_Dsc12;
			p.targetLinkRate = 0x1eu;
			p.targetLanes = 4u;
			p.useDsc = true;
			p.useFec = (dpcd.fecCap & 1u) != 0u;
			p.dscSlicesH = chooseDscSlicesH(t.width, dpcd);
			p.dscSliceHeight = chooseDscSliceHeight(t.height);
			p.dscBppX16 = 12u * 16u;
			return p;
		}
	}

	return p;
}

// Compute the maximum pixel clock (Hz) the live GPU + DP sink link can genuinely drive.
inline uint64_t maxSupportedPixelClockHz(const rdna4aux::DpcdCaps &dpcd,
                                         uint32_t maxDispClkKhz, uint32_t maxDppClkKhz,
                                         bool allowRetrain = true, bool allowDsc = true) {
	// Clock ceiling (single-pipe 1 pix/clk with 5% DISPCLK / 3% DPPCLK margin).
	uint64_t clkCapHz = 1800000000ULL;
	if (maxDispClkKhz > 0u) {
		const uint64_t d = (static_cast<uint64_t>(maxDispClkKhz) * 1000ULL * 100ULL) / 105ULL;
		if (d < clkCapHz) clkCapHz = d;
	}
	if (maxDppClkKhz > 0u) {
		const uint64_t d = (static_cast<uint64_t>(maxDppClkKhz) * 1000ULL * 100ULL) / 103ULL;
		if (d < clkCapHz) clkCapHz = d;
	}

	const uint8_t curRate = (dpcd.valid && dpcd.curLinkBwSet) ? dpcd.curLinkBwSet : 0x14u;
	const uint8_t curLanes = (dpcd.valid && dpcd.curLaneCountSet) ? dpcd.curLaneCountSet : 4u;
	const uint8_t baseMaxRate = (dpcd.valid && dpcd.maxLinkRate) ? dpcd.maxLinkRate : curRate;
	const uint8_t baseMaxLanes = (dpcd.valid && dpcd.maxLaneCount) ? dpcd.maxLaneCount : curLanes;
	const uint8_t maxRate = (dpcd.valid && dpcd.extMaxLinkRate > baseMaxRate) ? dpcd.extMaxLinkRate : baseMaxRate;
	const uint8_t maxLanes = (dpcd.valid && dpcd.extMaxLaneCount > baseMaxLanes) ? dpcd.extMaxLaneCount : baseMaxLanes;

	uint64_t linkCapHz = (rdna4aux::payloadKbps8b10b(curRate, curLanes) * 1000ULL * 1000ULL) / (24ULL * 1005ULL);
	if (allowRetrain && maxRate >= 0x1eu && maxLanes >= 4u) {
		const uint64_t hbr3Hz = (rdna4aux::payloadKbps8b10b(0x1eu, 4u) * 1000ULL * 1000ULL) / (24ULL * 1003ULL);
		if (hbr3Hz > linkCapHz) linkCapHz = hbr3Hz;
		if (allowDsc && dpcd.valid && dpcd.dscSupported) {
			const uint64_t hbr3FecKbps = (rdna4aux::payloadKbps8b10b(0x1eu, 4u) * 97ULL) / 100ULL;
			const uint64_t dscHz = (hbr3FecKbps * 1000ULL * 100ULL) / (12ULL * 102ULL);
			if (dscHz > linkCapHz) linkCapHz = dscHz;
		}
	}
	return (linkCapHz < clkCapHz) ? linkCapHz : clkCapHz;
}

// ---- VESA DSC 1.2a integer PPS + RC parameter calculator (8 bpc RGB, 12.0 bpp) ----
inline bool computeDscParams(uint32_t picW, uint32_t picH,
                             uint32_t numSlicesH, uint32_t sliceH,
                             uint32_t sinkLineBufDepthBits,
                             DscParams *out) {
	if (!out || !picW || !picH || !numSlicesH || !sliceH) return false;
	if ((picW % numSlicesH) != 0u || (picH % sliceH) != 0u) return false;
	*out = DscParams {};

	out->picWidth = picW;
	out->picHeight = picH;
	out->numSlicesH = numSlicesH;
	out->numSlicesV = picH / sliceH;
	out->sliceWidth = picW / numSlicesH;
	out->sliceHeight = sliceH;
	out->bpc = 8u;
	out->bppX16 = 12u * 16u; // 12.0 bpp
	out->lineBufDepth = (sinkLineBufDepthBits >= 9u && sinkLineBufDepthBits <= 13u) ? sinkLineBufDepthBits : 13u;

	// VESA DSC 1.2a Table E-4 recommended parameters for 8 bpc RGB @ 12.0 bpp.
	out->initialXmitDelay = 341u;
	out->firstLineBpgOffset = 15u;
	out->initialOffset = 2048u;
	out->flatnessMinQp = 3u;
	out->flatnessMaxQp = 12u;
	out->rcModelSize = 8192u;
	out->rcEdgeFactor = 6u;
	out->rcQuantIncrLimit0 = 11u;
	out->rcQuantIncrLimit1 = 11u;
	out->rcTgtOffsetHi = 3u;
	out->rcTgtOffsetLo = 3u;

	static constexpr uint8_t kThresh[14] = {
		14u, 28u, 42u, 56u, 70u, 84u, 98u, 105u, 112u, 119u, 121u, 123u, 125u, 126u
	};
	static constexpr uint8_t kMinQp[15] = {
		0u, 0u, 1u, 1u, 3u, 3u, 3u, 3u, 3u, 3u, 5u, 5u, 5u, 7u, 10u
	};
	static constexpr uint8_t kMaxQp[15] = {
		2u, 4u, 5u, 6u, 7u, 7u, 7u, 8u, 8u, 9u, 9u, 9u, 9u, 10u, 11u
	};
	static constexpr int8_t kBpgOff[15] = {
		2, 0, 0, -2, -4, -6, -8, -8, -8, -10, -10, -12, -12, -12, -12
	};
	memcpy(out->rcBufThresh, kThresh, sizeof(kThresh));
	memcpy(out->rangeMinQp, kMinQp, sizeof(kMinQp));
	memcpy(out->rangeMaxQp, kMaxQp, sizeof(kMaxQp));
	memcpy(out->rangeBpgOffset, kBpgOff, sizeof(kBpgOff));

	// Exact VESA DSC 1.2a integer RC computation (matches Linux drm_dsc_compute_rc_parameters).
	const uint32_t groupsPerLine = (out->sliceWidth + 2u) / 3u;
	out->sliceChunkSize = static_cast<uint32_t>(
	    (static_cast<uint64_t>(out->sliceWidth) * out->bppX16 + 127ULL) / 128ULL);
	const uint32_t muxWordSize = 48u; // bpc <= 10
	uint32_t numExtraMuxBits = 3u * (muxWordSize + (4u * out->bpc + 4u) - 2u);
	const uint64_t sliceBits = 8ULL * out->sliceChunkSize * out->sliceHeight;
	while (numExtraMuxBits > 0u && ((sliceBits - numExtraMuxBits) % muxWordSize) != 0u) {
		--numExtraMuxBits;
	}

	uint32_t initScale = (8u * out->rcModelSize) / (out->rcModelSize - out->initialOffset);
	if (groupsPerLine < initScale - 8u) initScale = groupsPerLine + 8u;
	out->initialScaleValue = initScale;
	out->scaleDecrementInterval = (initScale > 8u) ? (groupsPerLine / (initScale - 8u)) : 4095u;

	out->finalOffset = out->rcModelSize - ((out->initialXmitDelay * out->bppX16 + 8u) / 16u) + numExtraMuxBits;
	if (out->finalOffset >= out->rcModelSize) return false;

	const uint32_t finalScale = (out->rcModelSize * 8u) / (out->rcModelSize - out->finalOffset);
	out->nflBpgOffset = (out->sliceHeight > 1u)
	                        ? (((out->firstLineBpgOffset << 11) + (out->sliceHeight - 2u)) / (out->sliceHeight - 1u))
	                        : 0u;
	const uint32_t groupsTotal = groupsPerLine * out->sliceHeight;
	out->sliceBpgOffset = static_cast<uint32_t>(
	    ((static_cast<uint64_t>(out->rcModelSize - out->initialOffset + numExtraMuxBits) << 11) +
	     (groupsTotal - 1u)) / groupsTotal);

	if (finalScale > 9u) {
		const uint64_t denom = static_cast<uint64_t>(out->nflBpgOffset + out->sliceBpgOffset) * (finalScale - 9u);
		out->scaleIncrementInterval = denom ? static_cast<uint32_t>((static_cast<uint64_t>(out->finalOffset) << 11) / denom) : 0u;
	} else {
		out->scaleIncrementInterval = 0u;
	}

	const uint32_t rbsMin = (out->rcModelSize - out->initialOffset) +
	                        ((out->initialXmitDelay * out->bppX16 + 15u) / 16u) +
	                        groupsPerLine * out->firstLineBpgOffset;
	const uint32_t hrdDelay = (rbsMin * 16u + out->bppX16 - 1u) / out->bppX16;
	out->rcBufferModelSize = (hrdDelay * out->bppX16) / 16u;
	out->initialDecDelay = (hrdDelay > out->initialXmitDelay) ? (hrdDelay - out->initialXmitDelay) : 0u;
	out->optcBytesPerPixel = static_cast<uint32_t>(
	    (static_cast<uint64_t>(out->sliceChunkSize) * 0x10000000ULL + (out->sliceWidth - 1u)) / out->sliceWidth);
	return true;
}

// Pack 128-byte VESA DSC 1.2a PPS SDP payload (big-endian per VESA DSC 1.2a Table 4-1).
inline void packDscPps128(const DscParams &d, uint8_t pps[128]) {
	memset(pps, 0, 128u);
	auto put16 = [&](size_t off, uint32_t v) {
		pps[off]     = static_cast<uint8_t>((v >> 8) & 0xffu);
		pps[off + 1] = static_cast<uint8_t>(v & 0xffu);
	};
	pps[0] = (1u << 4) | 2u;                                        // DSC v1.2
	pps[3] = static_cast<uint8_t>((d.bpc << 4) | (d.lineBufDepth & 0x0fu));
	pps[4] = static_cast<uint8_t>(((d.bppX16 >> 8) & 0x03u) | (1u << 4) | (1u << 5)); // convert_rgb=1, block_pred=1
	pps[5] = static_cast<uint8_t>(d.bppX16 & 0xffu);
	put16(6u, d.picHeight);
	put16(8u, d.picWidth);
	put16(10u, d.sliceHeight);
	put16(12u, d.sliceWidth);
	put16(14u, d.sliceChunkSize);
	put16(16u, d.initialXmitDelay & 0x03ffu);
	put16(18u, d.initialDecDelay);
	pps[21] = static_cast<uint8_t>(d.initialScaleValue & 0x3fu);
	put16(22u, d.scaleIncrementInterval);
	put16(24u, d.scaleDecrementInterval & 0x0fffu);
	pps[27] = static_cast<uint8_t>(d.firstLineBpgOffset & 0x1fu);
	put16(28u, d.nflBpgOffset);
	put16(30u, d.sliceBpgOffset);
	put16(32u, d.initialOffset);
	put16(34u, d.finalOffset);
	pps[36] = static_cast<uint8_t>(d.flatnessMinQp & 0x1fu);
	pps[37] = static_cast<uint8_t>(d.flatnessMaxQp & 0x1fu);
	put16(38u, d.rcModelSize);
	pps[40] = static_cast<uint8_t>(d.rcEdgeFactor & 0x0fu);
	pps[41] = static_cast<uint8_t>(d.rcQuantIncrLimit0 & 0x1fu);
	pps[42] = static_cast<uint8_t>(d.rcQuantIncrLimit1 & 0x1fu);
	pps[43] = static_cast<uint8_t>(((d.rcTgtOffsetHi & 0x0fu) << 4) | (d.rcTgtOffsetLo & 0x0fu));
	for (size_t i = 0u; i < 14u; ++i) pps[44u + i] = d.rcBufThresh[i];
	for (size_t i = 0u; i < 15u; ++i) {
		const uint32_t w = (static_cast<uint32_t>(d.rangeMinQp[i] & 0x1fu) << 11) |
		                   (static_cast<uint32_t>(d.rangeMaxQp[i] & 0x1fu) << 6) |
		                   (static_cast<uint32_t>(d.rangeBpgOffset[i]) & 0x3fu);
		put16(58u + i * 2u, w);
	}
}

// ---- Hardware callback interface (injectable for both kernel and host tests) ----
struct Hw {
	void *cookie;
	uint32_t (*rdSeg)(void *cookie, uint32_t segIdx, uint32_t off);
	bool (*wrSeg)(void *cookie, uint32_t segIdx, uint32_t off, uint32_t val);
	bool (*raiseClocks)(void *cookie, uint32_t dispClkKhz, uint32_t dppClkKhz);
	// Action: 1=ENABLE(rate,lanes), 2=DISABLE, 3=SET_DRIVE(swing,preEmph)
	bool (*phyControl)(void *cookie, uint32_t action, uint8_t rateCode, uint8_t lanes,
	                   uint8_t swing, uint8_t preEmph);
	uint64_t (*nowUs)(void *cookie);
	void (*delayUs)(void *cookie, uint32_t us);
	rdna4aux::Io auxIo;
};

struct TimingRegs {
	uint32_t hTotal, hBlankSE, hSyncA, hSyncACntl;
	uint32_t vTotal, vTotalMin, vTotalMax, vBlankSE, vSyncA, vSyncACntl;
	uint32_t vStartup, vUpdate, vReady, pstateReg, vtgCtrl;
	uint32_t msa1, msa2, msa3, msa4;
	uint32_t dtoPhase, dtoModulo, dtoInt;
	uint32_t hTotalVal, hBlankEnd, vBlankEnd;
};

// Compute OTG + VTG + MSA + DP_DTO register values for any timing (matches optc1_program_timing & dccg401_set_dp_dto).
inline bool computeTimingRegs(const rdna4edidmodes::DetailedTiming &t,
                              uint32_t dtoModuloHz,
                              uint32_t curVUpdate, uint32_t curVReady,
                              TimingRegs *out) {
	if (!out || !t.width || !t.height || !t.hBlank || !t.vBlank || !t.pixelClockHz || !dtoModuloHz)
		return false;
	if (t.hSyncOffset + t.hSyncWidth > t.hBlank || t.vSyncOffset + t.vSyncWidth > t.vBlank)
		return false;

	const uint32_t hTotal = static_cast<uint32_t>(t.hTotal());
	const uint32_t vTotal = static_cast<uint32_t>(t.vTotal());
	const uint32_t hBlankEnd = t.hBlank - t.hSyncOffset;
	const uint32_t hBlankStart = hBlankEnd + t.width;
	const uint32_t vBlankEnd = t.vBlank - t.vSyncOffset;
	const uint32_t vBlankStart = vBlankEnd + t.height;
	if (hTotal > 0x7fffu || vTotal > 0x7fffu || hBlankStart > hTotal || vBlankStart > vTotal)
		return false;

	*out = TimingRegs {};
	out->hTotalVal = hTotal;
	out->hBlankEnd = hBlankEnd;
	out->vBlankEnd = vBlankEnd;
	out->hTotal = hTotal - 1u;
	out->hSyncA = (t.hSyncWidth << 16);
	out->hBlankSE = (hBlankEnd << 16) | hBlankStart;
	out->hSyncACntl = t.hPositive ? 0u : 1u;
	out->vTotal = vTotal - 1u;
	out->vTotalMin = vTotal - 1u;
	out->vTotalMax = vTotal - 1u;
	out->vSyncA = (t.vSyncWidth << 16);
	out->vBlankSE = (vBlankEnd << 16) | vBlankStart;
	out->vSyncACntl = t.vPositive ? 0u : 1u;

	// VSTARTUP sits strictly inside [2, vBlankEnd - 2] (after VSYNC, before VACTIVE) for HUBP prefetch.
	uint32_t vStartup = (vBlankEnd > 6u) ? (vBlankEnd - 2u) : ((vBlankEnd > 2u) ? (vBlankEnd - 1u) : 1u);
	if (vStartup > 64u) vStartup = 64u;
	if (vStartup < 13u && vBlankEnd >= 15u) vStartup = 13u;
	out->vStartup = vStartup;
	out->vUpdate = curVUpdate ? curVUpdate : 0x014003e8u;
	out->vReady = curVReady ? curVReady : 0x0000012cu;
	out->pstateReg = (vStartup > 2u) ? (vStartup - 2u) : 1u;
	const uint32_t fp2 = (vStartup > vBlankEnd + 1u) ? (vStartup - (vBlankEnd + 1u)) : 1u;
	out->vtgCtrl = (vBlankStart << 16) | (fp2 & 0x7fffu);

	// DP1 MSA TIMING PARAM 1..4 (dcn_4_1_0_sh_mask.h).
	const uint32_t hPolBit = t.hPositive ? 0u : (1u << 31);
	const uint32_t vPolBit = t.vPositive ? 0u : (1u << 15);
	out->msa1 = (hTotal << 16) | vTotal;
	out->msa2 = (hBlankEnd << 16) | vBlankEnd;
	out->msa3 = hPolBit | (t.hSyncWidth << 16) | vPolBit | t.vSyncWidth;
	out->msa4 = (t.width << 16) | t.height;

	// DP_DTO (dccg401_set_dp_dto): pixclk = dtoInt * dtoModulo + dtoPhase.
	out->dtoModulo = dtoModuloHz;
	out->dtoInt = static_cast<uint32_t>(t.pixelClockHz / dtoModuloHz);
	out->dtoPhase = static_cast<uint32_t>(t.pixelClockHz - static_cast<uint64_t>(out->dtoInt) * dtoModuloHz);
	if (out->dtoInt > 15u) return false;
	return true;
}

// ---- DP 1.4 link training (Clock Recovery TPS1 + Channel EQ TPS4/TPS3/TPS2 + Passthrough + FEC) ----
inline bool trainDpLink(const Hw &hw, const rdna4aux::ActiveOutput &out,
                        const rdna4aux::DpcdCaps &dpcd,
                        uint8_t rateCode, uint8_t lanes, bool enableFec) {
	if (!hw.rdSeg || !hw.wrSeg || !hw.phyControl || !hw.delayUs) return false;
	if (!out.found || !out.isDp || !out.auxValid || lanes == 0u || lanes > 4u) return false;

	const uint32_t link = (out.link < 4u) ? out.link : out.dig;
	const uint32_t be = link * kSeg2DigStride;

	auto sendIt = [&](bool trained) -> bool {
		uint32_t lc = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0LinkCntl + be);
		if (lc == 0xffffffffu) return false;
		lc = trained ? (lc | (1u << 4)) : (lc & ~(1u << 4));
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0LinkCntl + be, lc)) return false;
		uint32_t dphy = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be);
		if (dphy != 0xffffffffu) {
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be, dphy & ~(1u << 16)); // DPHY_BYPASS = 0
		}
		uint32_t prbs = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyPrbsCntl + be);
		if (prbs != 0xffffffffu) {
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyPrbsCntl + be, prbs & ~1u); // DPHY_PRBS_EN = 0
		}
		return true;
	};

	auto setPattern = [&](uint32_t patIdx, bool video) -> bool {
		if (video) {
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyIntCtrl + be, 0u);
			uint32_t lf = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0LinkFraming + be);
			if (lf != 0xffffffffu) {
				lf = (lf & ~0x1103ffffu) | 0x2000u | (dpcd.enhancedFraming ? (1u << 28) : 0u);
				(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0LinkFraming + be, lf);
			}
			uint32_t sc = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0ScramCntl + be);
			if (sc != 0xffffffffu) {
				sc = (sc & ~(0x3ffu << 8)) | (0x1ffu << 8);
				(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0ScramCntl + be, sc);
			}
			return sendIt(true);
		}
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0TrainPatSel + be, patIdx)) return false;
		return sendIt(false);
	};

	auto phyOff = [&]() {
		(void)hw.phyControl(hw.cookie, 2u, rateCode, lanes, 0u, 0u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0TrainPatSel + be, 0u);
		uint32_t lc = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0LinkCntl + be);
		if (lc != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0LinkCntl + be, lc & ~(1u << 4));
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyIntCtrl + be, 0u);
	};

	auto sendDrive = [&](uint8_t swing, uint8_t preEmph) -> bool {
		for (uint8_t l = 0u; l < lanes; ++l) {
			if (!hw.phyControl(hw.cookie, 3u, rateCode, lanes, swing, preEmph)) return false;
		}
		return true;
	};

	static constexpr uint8_t kMaxPreForSwing[4] = { 3u, 2u, 1u, 0u };
	const uint32_t crWaitUs = (dpcd.dpcdRev >= 0x12u && dpcd.trainingAuxRdInt != 0u)
	                              ? (static_cast<uint32_t>(dpcd.trainingAuxRdInt) * 4000u)
	                              : 400u;
	const uint32_t eqWaitUs = (dpcd.trainingAuxRdInt != 0u)
	                              ? (static_cast<uint32_t>(dpcd.trainingAuxRdInt) * 4000u)
	                              : 4000u;
	const uint32_t eqWaitClamped = eqWaitUs > 16000u ? 16000u : eqWaitUs;
	const uint32_t crWaitClamped = crWaitUs > 16000u ? 16000u : crWaitUs;

	// Disable transmitter cleanly before the first training attempt (device.cpp: offFirst = true).
	phyOff();

	for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
		if (attempt > 0u) {
			const uint8_t d3 = 0x02u;
			(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0600u, &d3, 1u);
			phyOff();
			hw.delayUs(hw.cookie, attempt * 50000u);
		}

		// enc1_configure_encoder: lane count + scrambler advance, and FEC disabled during training.
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0Config + be, (lanes - 1u) & 3u)) return false;
		uint32_t scram = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0ScramCntl + be);
		if (scram != 0xffffffffu) {
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0ScramCntl + be, scram | (1u << 4));
		}
		uint32_t dphy = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be);
		if (dphy != 0xffffffffu) {
			dphy &= ~(1u << 4); // DPHY_FEC_EN = 0 during training
			if (enableFec) dphy |= (1u << 5); // DPHY_FEC_READY_SHADOW = 1
			else dphy &= ~(1u << 5);
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be, dphy);
		}
		if (!hw.phyControl(hw.cookie, 1u, rateCode, lanes, 0u, 0u)) return false;

		// Wake sink to D0, set/clear FEC_READY (DPCD 0x0120), exit training, configure coding/spread/link.
		const uint8_t d0 = 0x01u;
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0600u, &d0, 1u);
		if ((dpcd.fecCap & 1u) != 0u) {
			const uint8_t fecReady = enableFec ? 1u : 0u;
			(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0120u, &fecReady, 1u);
		}
		const uint8_t zero = 0x00u;
		const uint8_t coding8b10b = 0x01u;
		const uint8_t downspread = (dpcd.maxDownspread & 1u) ? 0x10u : 0x00u;
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0102u, &zero, 1u);
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0108u, &coding8b10b, 1u);
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0107u, &downspread, 1u);
		const uint8_t linkCfg[2] = {
			rateCode,
			static_cast<uint8_t>(lanes | (dpcd.enhancedFraming ? 0x80u : 0x00u))
		};
		if (!rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0100u, linkCfg, 2u)) return false;

		// ---- Stage 1: Clock Recovery (TPS1) ----
		if (!setPattern(0u, false)) return false; // TPS1
		uint8_t swing = 0u, preEmph = 0u;
		uint8_t dpcdTrain[5] = { 0x21u, 0u, 0u, 0u, 0u }; // TPS1 + scrambling disabled
		bool crOk = false;
		for (uint32_t iter = 0u; iter < 5u; ++iter) {
			if (!sendDrive(swing, preEmph)) return false;
			const uint8_t laneVal = static_cast<uint8_t>((swing & 3u) | (swing == 3u ? 0x04u : 0u) |
			                                             ((preEmph & 3u) << 3) | (preEmph == 3u ? 0x20u : 0u));
			for (uint8_t l = 0u; l < lanes; ++l) dpcdTrain[1u + l] = laneVal;
			const bool wOk = (iter == 0u)
			                     ? rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0102u, dpcdTrain, 1u + lanes)
			                     : rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0103u, dpcdTrain + 1u, lanes);
			if (!wOk) return false;
			hw.delayUs(hw.cookie, crWaitClamped);
			uint8_t st[6] {};
			if (!rdna4aux::dpcdRead(hw.auxIo, out.aux, out.hpd, 0x0202u, st, 6u)) return false;
			crOk = true;
			for (uint8_t l = 0u; l < lanes; ++l) {
				const uint8_t nib = (st[l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
				if ((nib & 0x01u) == 0u) crOk = false;
			}
			if (crOk) break;
			if (swing == 3u) break;
			uint8_t nextSw = 0u, nextPe = 0u;
			for (uint8_t l = 0u; l < lanes; ++l) {
				const uint8_t adj = (st[4u + l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
				const uint8_t sw = adj & 3u;
				const uint8_t pe = (adj >> 2) & 3u;
				if (sw > nextSw) nextSw = sw;
				if (pe > nextPe) nextPe = pe;
			}
			if (nextPe > kMaxPreForSwing[nextSw]) nextPe = kMaxPreForSwing[nextSw];
			swing = nextSw;
			preEmph = nextPe;
		}
		if (!crOk) {
			(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0102u, &zero, 1u);
			continue;
		}

		// ---- Stage 2: Channel Equalization (TPS4 for HBR3 if supported, else TPS3, else TPS2) ----
		const uint32_t eqHwPat = (rateCode >= 0x1eu && dpcd.tps4Supported) ? 3u
		                       : (dpcd.tps3Supported ? 2u : 1u);
		const uint8_t eqDpcdPat = (eqHwPat == 3u) ? 0x07u : (eqHwPat == 2u ? 0x23u : 0x22u);
		if (!setPattern(eqHwPat, false)) return false;
		dpcdTrain[0] = eqDpcdPat;

		bool eqOk = false;
		for (uint32_t iter = 0u; iter < 6u; ++iter) {
			if (!sendDrive(swing, preEmph)) return false;
			const uint8_t laneVal = static_cast<uint8_t>((swing & 3u) | (swing == 3u ? 0x04u : 0u) |
			                                             ((preEmph & 3u) << 3) | (preEmph == 3u ? 0x20u : 0u));
			for (uint8_t l = 0u; l < lanes; ++l) dpcdTrain[1u + l] = laneVal;
			const bool wOk = (iter == 0u)
			                     ? rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0102u, dpcdTrain, 1u + lanes)
			                     : rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0103u, dpcdTrain + 1u, lanes);
			if (!wOk) return false;
			hw.delayUs(hw.cookie, eqWaitClamped);
			uint8_t st[6] {};
			if (!rdna4aux::dpcdRead(hw.auxIo, out.aux, out.hpd, 0x0202u, st, 6u)) return false;
			bool crStill = true;
			eqOk = (st[2] & 0x01u) != 0u; // INTERLANE_ALIGN_DONE
			for (uint8_t l = 0u; l < lanes; ++l) {
				const uint8_t nib = (st[l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
				if ((nib & 0x01u) == 0u) crStill = false;
				if ((nib & 0x07u) != 0x07u) eqOk = false; // CR + EQ + SYM_LOCK
			}
			if (!crStill) { eqOk = false; break; }
			if (eqOk) break;
			uint8_t nextSw = 0u, nextPe = 0u;
			for (uint8_t l = 0u; l < lanes; ++l) {
				const uint8_t adj = (st[4u + l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
				const uint8_t sw = adj & 3u;
				const uint8_t pe = (adj >> 2) & 3u;
				if (sw > nextSw) nextSw = sw;
				if (pe > nextPe) nextPe = pe;
			}
			if (nextPe > kMaxPreForSwing[nextSw]) nextPe = kMaxPreForSwing[nextSw];
			swing = nextSw;
			preEmph = nextPe;
		}

		// End training pattern -> passthrough video idle mode.
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0102u, &zero, 1u);
		if (!eqOk) continue;

		if (!setPattern(0u, true)) return false;

		// Enable FEC on source after link training completes (enc2_fec_set_enable).
		if (enableFec) {
			uint32_t dphyNow = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be);
			if (dphyNow != 0xffffffffu) {
				dphyNow |= (1u << 4) | (1u << 5);
				(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be, dphyNow);
				for (uint32_t i = 0u; i < 20u; ++i) {
					const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be);
					if (r != 0xffffffffu && (r & (1u << 6)) != 0u) break;
					hw.delayUs(hw.cookie, 100u);
				}
			}
		}

		// Post-training link stability check (dptrain.cpp: wait 5ms, check LANE0_1..ALIGN).
		hw.delayUs(hw.cookie, 5000u);
		uint8_t post[3] {};
		if (rdna4aux::dpcdRead(hw.auxIo, out.aux, out.hpd, 0x0202u, post, 3u)) {
			bool up = (post[2] & 0x01u) != 0u;
			for (uint8_t l = 0u; l < lanes; ++l) {
				const uint8_t nib = (post[l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
				if ((nib & 0x07u) != 0x07u) up = false;
			}
			if (up) return true;
		}
	}
	return false;
}

// ---- Program OPTC DSC framing (must be called under OTG_MASTER_UPDATE_LOCK) ----
inline void configureOptcDsc(const Hw &hw, uint32_t otg, bool enable, const DscParams *d) {
	const uint32_t optcBase = otg * kSeg2OdmStride;
	uint32_t dfmt = hw.rdSeg(hw.cookie, 2u, kSeg2OptcDataFmt + optcBase);
	if (enable && d) {
		if (dfmt != 0xffffffffu) {
			dfmt = (dfmt & ~(3u << 4)) | (1u << 4); // OPTC_DSC_MODE = 1 (single-stream DSC)
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcDataFmt + optcBase, dfmt);
		}
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcBytesPerPx + optcBase, d->optcBytesPerPixel);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcWidthCtrl + optcBase,
		               (d->picWidth & 0x1fffu) | ((d->sliceWidth & 0x1fffu) << 16));
	} else {
		if (dfmt != 0xffffffffu) {
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcDataFmt + optcBase, dfmt & ~(3u << 4));
		}
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcBytesPerPx + optcBase, 0u);
	}
}

// ---- Power-ungate DSC0..3 domain (dcn32_dsc_pg_control: DOMAIN16..19_PG_CONFIG/STATUS) ----
inline void setDscPowerGate(const Hw &hw, uint32_t dscInst, bool powerOn) {
	if (!hw.rdSeg || !hw.wrSeg || !hw.delayUs || dscInst >= 4u) return;
	const uint32_t pgCfgReg  = kSeg2Domain16PgCfg + dscInst * 2u;
	const uint32_t pgStatReg = kSeg2Domain16PgStat + dscInst * 2u;
	const uint32_t origReq = hw.rdSeg(hw.cookie, 2u, kSeg2DcIpReqCntl);
	if (origReq != 0xffffffffu && (origReq & 1u) == 0u) {
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2DcIpReqCntl, origReq | 1u);
	}
	uint32_t cfg = hw.rdSeg(hw.cookie, 2u, pgCfgReg);
	if (cfg != 0xffffffffu) {
		if (powerOn) {
			cfg = (cfg & ~(1u << 8)) | 1u; // DOMAIN_POWER_GATE = 0, DOMAIN_POWER_FORCEON = 1
		} else {
			cfg = (cfg & ~1u) | (1u << 8); // DOMAIN_POWER_FORCEON = 0, DOMAIN_POWER_GATE = 1
		}
		(void)hw.wrSeg(hw.cookie, 2u, pgCfgReg, cfg);
		const uint32_t wantState = powerOn ? 0u : 2u;
		for (uint32_t i = 0u; i < 1000u; ++i) {
			const uint32_t st = hw.rdSeg(hw.cookie, 2u, pgStatReg);
			if (st != 0xffffffffu && ((st >> 30) & 3u) == wantState) break;
			hw.delayUs(hw.cookie, 1u);
		}
	}
	if (origReq != 0xffffffffu && (origReq & 1u) == 0u) {
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2DcIpReqCntl, origReq & ~1u);
	}
}

// ---- Program DCN 4.1 DSC0 + DSCCLK0 + DSCRM0 + VPG PPS SDP + VBID6 + DPCD 0x0160 ----
inline bool configureDscHardware(const Hw &hw, const rdna4aux::ActiveOutput &out,
                                 bool enable, const DscParams *d) {
	if (!hw.rdSeg || !hw.wrSeg) return false;
	const uint32_t otg = out.otg;
	const uint32_t digBase = out.dig * kSeg2DigStride;
	const uint32_t dscInst = otg; // 1:1 DSC0 <-> OTG0/OPP0
	const uint32_t dscBase = dscInst * kSeg2DscStride;

	if (!enable) {
		// Disable DSC on sink first, then stream encoder (GSP11/VBID6), DSCRM, DSC_TOP, DSCCLK.
		const uint8_t dscOff = 0u;
		(void)rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0160u, &dscOff, 1u);
		uint32_t gsp = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0Gsp11Cntl + digBase);
		if (gsp != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0Gsp11Cntl + digBase, gsp & ~(1u << 4));
		uint32_t sec2 = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0SecCntl2 + digBase);
		if (sec2 != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SecCntl2 + digBase, sec2 & ~(1u << 28));
		uint32_t vbid = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + digBase);
		if (vbid != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + digBase, vbid & ~0xffff8000u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dscrm0Forward + dscInst, 0u);
		uint32_t top = hw.rdSeg(hw.cookie, 2u, kSeg2DscTop0Ctrl + dscBase);
		if (top != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2DscTop0Ctrl + dscBase, top & ~1u);
		uint32_t dscClk = hw.rdSeg(hw.cookie, 1u, kSeg1DscClkDtoCtrl);
		if (dscClk != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 1u, kSeg1DscClkDtoCtrl, dscClk & ~(1u << dscInst));
		(void)hw.wrSeg(hw.cookie, 1u, kSeg1DscClk0DtoParam + dscInst, 0u);
		return true;
	}
	if (!d) return false;

	// 0. Power-ungate DSC domain BEFORE writing any DSC registers (UEFI GOP leaves DOMAIN16 gated off!).
	setDscPowerGate(hw, dscInst, true);

	// 1. Enable DSCCLK DTO (dccg401_set_dto_dscclk: PHASE=1, MODULO=1, EN=1).
	if (!hw.wrSeg(hw.cookie, 1u, kSeg1DscClk0DtoParam + dscInst, (1u << 16) | 1u)) return false;
	uint32_t dscClk = hw.rdSeg(hw.cookie, 1u, kSeg1DscClkDtoCtrl);
	if (dscClk == 0xffffffffu) return false;
	if (!hw.wrSeg(hw.cookie, 1u, kSeg1DscClkDtoCtrl, dscClk | (1u << dscInst))) return false;

	// 2. Enable DSC_TOP clock.
	uint32_t top = hw.rdSeg(hw.cookie, 2u, kSeg2DscTop0Ctrl + dscBase);
	if (top == 0xffffffffu) return false;
	if (!hw.wrSeg(hw.cookie, 2u, kSeg2DscTop0Ctrl + dscBase, top | 1u)) return false;

	// 3. Program DSCCIF0 + DSCC0 CONFIG0/1 + PPS_CONFIG0..22 (dsc401_set_config).
	if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dsccif0Cfg0 + dscBase, (d->bpc << 16))) return false;
	const uint32_t ichReset = (d->numSlicesH > 1u) ? 0x0fu : 0u;
	// DSC v1.2 requires ALTERNATE_ICH_ENCODING_EN = 1 (bit 8) per Linux dcn20_dsc.c / dcn401_dsc.c!
	const uint32_t cfg0 = ichReset | (((d->numSlicesH - 1u) & 3u) << 4) | (1u << 8) | ((d->numSlicesV - 1u) << 16);
	if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dscc0Cfg0 + dscBase, cfg0)) return false;
	if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dscc0Cfg1 + dscBase, d->rcBufferModelSize & 0x3ffffu)) return false;

	const uint32_t ppsBase = kSeg2Dscc0Pps0 + dscBase;
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 0u, 2u | (1u << 4) | (d->lineBufDepth << 24) | (d->bpc << 28));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 1u, (d->bppX16 & 0x3ffu) | (1u << 12) | (1u << 13) | (d->sliceChunkSize << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 2u, (d->picWidth & 0xffffu) | (d->picHeight << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 3u, (d->sliceWidth & 0xffffu) | (d->sliceHeight << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 4u, (d->initialXmitDelay & 0x3ffu) | (d->initialDecDelay << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 5u, (d->initialScaleValue & 0x3fu) | (d->scaleIncrementInterval << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 6u, (d->scaleDecrementInterval & 0xfffu) | (d->firstLineBpgOffset << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 7u, (d->nflBpgOffset & 0xffffu) | (d->sliceBpgOffset << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 8u, 0u);
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 9u, (d->initialOffset & 0xffffu) | (d->finalOffset << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 10u, (d->flatnessMinQp & 0x1fu) | ((d->flatnessMaxQp & 0x1fu) << 8) | (d->rcModelSize << 16));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 11u, (d->rcEdgeFactor & 0xfu) | ((d->rcQuantIncrLimit0 & 0x1fu) << 8) |
	               ((d->rcQuantIncrLimit1 & 0x1fu) << 16) | ((d->rcTgtOffsetLo & 0xfu) << 24) | ((d->rcTgtOffsetHi & 0xfu) << 28));
	auto pack4 = [](const uint8_t *p) -> uint32_t {
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
		       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
	};
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 12u, pack4(d->rcBufThresh + 0u));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 13u, pack4(d->rcBufThresh + 4u));
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 14u, pack4(d->rcBufThresh + 8u));
	auto packRange = [&](size_t idx) -> uint32_t {
		return (static_cast<uint32_t>(d->rangeMinQp[idx] & 0x1fu)) |
		       (static_cast<uint32_t>(d->rangeMaxQp[idx] & 0x1fu) << 5) |
		       ((static_cast<uint32_t>(d->rangeBpgOffset[idx]) & 0x3fu) << 10);
	};
	(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 15u, static_cast<uint32_t>(d->rcBufThresh[12]) |
	               (static_cast<uint32_t>(d->rcBufThresh[13]) << 8) | (packRange(0u) << 16));
	for (size_t r = 0u; r < 7u; ++r) {
		(void)hw.wrSeg(hw.cookie, 2u, ppsBase + 16u + static_cast<uint32_t>(r),
		               packRange(1u + r * 2u) | (packRange(2u + r * 2u) << 16));
	}

	// 4. Route OPP -> DSC (DSCRM). (OPTC DSC framing is programmed while OTG is stopped / locked.)
	if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dscrm0Forward + dscInst, 1u | (otg << 4))) return false;

	// 5. Power on VPG SRAM (vpg31_poweron) and upload 128-byte VESA PPS SDP into packets 11..14 + VBID[6].
	uint32_t vpgPwr = hw.rdSeg(hw.cookie, 2u, kSeg2Vpg0MemPwr + digBase);
	if (vpgPwr != 0xffffffffu) {
		vpgPwr = (vpgPwr & ~(1u << 4)) | 1u; // VPG_GSP_LIGHT_SLEEP_FORCE=0, VPG_GSP_MEM_LIGHT_SLEEP_DIS=1
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0MemPwr + digBase, vpgPwr);
	}
	uint8_t pps[128] {};
	packDscPps128(*d, pps);
	uint32_t sec2 = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0SecCntl2 + digBase);
	if (sec2 != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SecCntl2 + digBase, sec2 | (1u << 28)); // DP_SEC_GSP11_PPS
	uint32_t afmt = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0AfmtCntl + digBase);
	if (afmt != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0AfmtCntl + digBase, afmt | 1u); // AFMT_AUDIO_CLOCK_EN
	for (uint32_t pIdx = 0u; pIdx < 4u; ++pIdx) {
		const uint32_t pkt = 11u + pIdx;
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0Status + digBase, 1u << 4); // VPG_GENERIC_CONFLICT_CLR
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0AccessCtrl + digBase, pkt * 9u);
		// SDP header: hb0=0, hb1=0x10 (PPS), hb2=127, hb3=0
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0PacketData + digBase, (0x10u << 8) | (127u << 16));
		for (uint32_t dw = 0u; dw < 8u; ++dw) {
			const uint8_t *src = pps + pIdx * 32u + dw * 4u;
			const uint32_t word = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
			                      (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0PacketData + digBase, word);
		}
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vpg0ImmUpdate + digBase, 1u << pkt);
	}
	// VBID[6] CompressedStream_Flag line number MUST be > PPS transmit line number (2 < 3).
	uint32_t gsp = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0Gsp11Cntl + digBase);
	if (gsp != 0xffffffffu) {
		gsp = (gsp & ~0xffff0020u) | (2u << 16); // LINE_NUM=2, LINE_REFERENCE=0
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0Gsp11Cntl + digBase, gsp);
	}
	uint32_t vbid = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + digBase);
	if (vbid != 0xffffffffu) {
		vbid = (vbid & ~0xffff8000u) | (3u << 16); // DP_VBID6_LINE_REFERENCE=0, DP_VBID6_LINE_NUM=3
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + digBase, vbid);
	}
	if (gsp != 0xffffffffu) {
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0Gsp11Cntl + digBase, gsp | (1u << 4)); // DP_SEC_GSP11_ENABLE=1
	}
	uint32_t sec = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0SecCntl + digBase);
	if (sec != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SecCntl + digBase, sec | 1u); // DP_SEC_STREAM_ENABLE=1

	// 6. Enable DSC on sink (DPCD 0x0160 = 1) before unblanking the compressed stream.
	const uint8_t dscOn = 1u;
	return rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, 0x0160u, &dscOn, 1u);
}

// ---- Register snapshot for atomic rollback ----
struct Snapshot {
	bool valid;
	uint32_t otg, dig, link;
	uint8_t linkRate, lanes;
	bool dscWasOn, fecWasOn;
	uint32_t dtoPhase, dtoModulo, pixRateDiv;
	uint32_t symClkEn, digBeClk, digBeCntl, digBeEn, digFeClk, digFeCntl, digFeEn, digStreamMap;
	uint32_t hTotal, hBlankSE, hSyncA, hSyncACntl;
	uint32_t vTotal, vTotalMin, vTotalMax, vBlankSE, vSyncA, vSyncACntl;
	uint32_t vStartup, vUpdate, vReady, pstateReg, vtgCtrl;
	uint32_t optcDataSrc, optcMemCfg, optcDataFmt, optcBytesPerPx, optcWidthCtrl;
	uint32_t msa1, msa2, msa3, msa4, pixelFmt, msaColorim, vidTiming, vidN, vidM, msaVbidMisc;
	uint32_t hubpBlank0;
};

inline bool captureSnapshot(const Hw &hw, const rdna4aux::ActiveOutput &out,
                            const rdna4aux::DpcdCaps &dpcd, Snapshot *s) {
	if (!s || !hw.rdSeg || !out.found || !out.isDp) return false;
	*s = Snapshot {};
	s->otg = out.otg;
	s->dig = out.dig;
	s->link = (out.link < 4u) ? out.link : out.dig;
	s->linkRate = dpcd.curLinkBwSet ? dpcd.curLinkBwSet : 0x14u;
	s->lanes = dpcd.curLaneCountSet ? dpcd.curLaneCountSet : 4u;
	const uint32_t ob = out.otg * kSeg2OtgStride;
	const uint32_t odmB = out.otg * kSeg2OdmStride;
	const uint32_t db = out.dig * kSeg2DigStride;
	const uint32_t be = s->link * kSeg2DigStride;
	const uint32_t hb = out.otg * kSeg2HubpStride;

	s->dtoPhase   = hw.rdSeg(hw.cookie, 1u, kSeg1DpDto0Phase + out.otg * 4u);
	s->dtoModulo  = hw.rdSeg(hw.cookie, 1u, kSeg1DpDto0Modulo + out.otg * 4u);
	s->pixRateDiv = hw.rdSeg(hw.cookie, 1u, kSeg1OtgPixRateDiv);
	s->symClkEn     = hw.rdSeg(hw.cookie, 1u, kSeg1SymClkAClkEn + out.dig);
	s->digBeClk     = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeClkCntl + be);
	s->digBeCntl    = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeCntl + be);
	s->digBeEn      = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeEnCntl + be);
	s->digFeClk     = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeClkCntl + db);
	s->digFeCntl    = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeCntl + db);
	s->digFeEn      = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeEnCntl + db);
	s->digStreamMap = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0StreamMap + out.dig);
	s->hTotal     = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0HTotal + ob);
	s->hBlankSE   = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0HBlankSE + ob);
	s->hSyncA     = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0HSyncA + ob);
	s->hSyncACntl = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0HSyncACntl + ob);
	s->vTotal     = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VTotal + ob);
	s->vTotalMin  = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VTotalMin + ob);
	s->vTotalMax  = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VTotalMax + ob);
	s->vBlankSE   = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VBlankSE + ob);
	s->vSyncA     = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VSyncA + ob);
	s->vSyncACntl = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VSyncACntl + ob);
	s->vStartup   = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VStartup + ob);
	s->vUpdate    = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VUpdate + ob);
	s->vReady     = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0VReady + ob);
	s->pstateReg  = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0PstateReg + ob);
	s->vtgCtrl    = hw.rdSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg);
	s->optcDataSrc    = hw.rdSeg(hw.cookie, 2u, kSeg2OptcDataSrcSel + odmB);
	s->optcMemCfg     = hw.rdSeg(hw.cookie, 2u, kSeg2OptcMemCfg + odmB);
	s->optcDataFmt    = hw.rdSeg(hw.cookie, 2u, kSeg2OptcDataFmt + odmB);
	s->optcBytesPerPx = hw.rdSeg(hw.cookie, 2u, kSeg2OptcBytesPerPx + odmB);
	s->optcWidthCtrl  = hw.rdSeg(hw.cookie, 2u, kSeg2OptcWidthCtrl + odmB);
	s->msa1        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaParam1 + db);
	s->msa2        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaParam2 + db);
	s->msa3        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaParam3 + db);
	s->msa4        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaParam4 + db);
	s->pixelFmt    = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0PixelFormat + db);
	s->msaColorim  = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaColorim + db);
	s->vidTiming   = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidTiming + db);
	s->vidN        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidN + db);
	s->vidM        = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidM + db);
	s->msaVbidMisc = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + db);
	s->hubpBlank0  = hw.rdSeg(hw.cookie, 2u, kSeg2HubpReq0Blank0 + hb);
	const uint32_t dphy = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0DphyCntl + be);
	s->dscWasOn = (s->pixelFmt != 0xffffffffu) && ((s->pixelFmt & 1u) != 0u);
	s->fecWasOn = (dphy != 0xffffffffu) && ((dphy & (1u << 4)) != 0u);
	s->valid = (s->dtoModulo != 0u && s->dtoModulo != 0xffffffffu && s->hTotal != 0xffffffffu);
	return s->valid;
}

inline uint32_t scaleU32Ratio(uint32_t val, uint64_t num, uint64_t den) {
	if (!den || val == 0u || val == 0xffffffffu) return val;
	const uint64_t r = (static_cast<uint64_t>(val) * num + den / 2ULL) / den;
	return (r > 0xffffffffULL) ? 0xffffffffu : static_cast<uint32_t>(r);
}

inline uint32_t scaleField(uint32_t reg, uint32_t shift, uint32_t widthMask, uint64_t num, uint64_t den) {
	if (reg == 0xffffffffu || !den) return reg;
	const uint32_t raw = (reg >> shift) & widthMask;
	uint32_t scaled = scaleU32Ratio(raw, num, den);
	if (raw > 0u && scaled == 0u) scaled = 1u;
	if (scaled > widthMask) scaled = widthMask;
	return (reg & ~(widthMask << shift)) | ((scaled & widthMask) << shift);
}

struct ApplyResult {
	uint32_t status;
	uint32_t measuredMilliHz;
	LinkTier tierUsed;
	bool rolledBack;
};

// Apply a target timing to the live DCN 4.1 pipe (OTG -> DIG -> DP) with automatic rollback on failure.
inline ApplyResult applyMode(const Hw &hw, const rdna4aux::ActiveOutput &out,
                             const rdna4aux::DpcdCaps &dpcd,
                             const rdna4edidmodes::DetailedTiming &target,
                             uint32_t maxDispClkKhz, uint32_t maxDppClkKhz,
                             bool allowRetrain = true, bool allowDsc = true,
                             bool forceRetrain = false) {
	ApplyResult res { kInvalid, 0u, LinkTier::Unsupported, false };
	if (!hw.rdSeg || !hw.wrSeg || !hw.nowUs || !hw.delayUs || !out.found || !out.isDp || out.otg >= 4u || out.dig >= 4u)
		return res;

	const ModePlan plan = planMode(target, dpcd, maxDispClkKhz, maxDppClkKhz, allowRetrain, allowDsc);
	if (plan.tier == LinkTier::Unsupported) {
		res.status = kUnsupported;
		return res;
	}
	res.tierUsed = plan.tier;

	Snapshot snap {};
	if (!captureSnapshot(hw, out, dpcd, &snap)) {
		res.status = kInvalid;
		return res;
	}

	TimingRegs tr {};
	if (!computeTimingRegs(target, snap.dtoModulo, snap.vUpdate, snap.vReady, &tr)) {
		res.status = kInvalid;
		return res;
	}

	DscParams dscParams {};
	if (plan.useDsc) {
		const uint32_t lbDepth = dpcd.dscLineBufDepth ? static_cast<uint32_t>((dpcd.dscLineBufDepth & 0x0fu) + 8u) : 13u;
		if (!computeDscParams(target.width, target.height, plan.dscSlicesH, plan.dscSliceHeight, lbDepth, &dscParams)) {
			res.status = kUnsupported;
			return res;
		}
	}

	// 1. Raise SMU DISPCLK / DPPCLK BEFORE touching display timing registers.
	if (hw.raiseClocks && !hw.raiseClocks(hw.cookie, plan.needDispClkKhz, plan.needDppClkKhz)) {
		res.status = kClockFailed;
		return res;
	}

	const uint32_t link = (out.link < 4u) ? out.link : out.dig;
	const uint32_t ob = out.otg * kSeg2OtgStride;
	const uint32_t odmB = out.otg * kSeg2OdmStride;
	const uint32_t db = out.dig * kSeg2DigStride;
	const uint32_t be = link * kSeg2DigStride;
	const uint32_t hb = out.otg * kSeg2HubpStride;
	const uint32_t divShift = 1u + out.otg * 5u;
	const uint32_t divMask = 0x0fu << divShift;

	auto stopOtg = [&]() {
		// Hardware-verified optc401_disable_crtc + optc1_enable_optc_clock(false) (modeset.cpp:268-276).
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcDataSrcSel + odmB, 0xffff0000u);
		uint32_t mc = hw.rdSeg(hw.cookie, 2u, kSeg2OptcMemCfg + odmB);
		if (mc != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcMemCfg + odmB, mc & ~0xffffu);
		uint32_t oc = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0Control + ob);
		if (oc != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0Control + ob, oc & ~1u);
		uint32_t vt = hw.rdSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg);
		if (vt != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg, vt & ~(1u << 31));
		for (uint32_t i = 0u; i < 1500u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0Control + ob);
			if (r == 0xffffffffu || (r & (1u << 16)) == 0u) break;
			hw.delayUs(hw.cookie, 100u);
		}
		for (uint32_t i = 0u; i < 1500u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob);
			if (r == 0xffffffffu || (r & (1u << 16)) == 0u) break;
			hw.delayUs(hw.cookie, 100u);
		}
		uint32_t clk = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob);
		if (clk != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob, clk & ~3u);
		uint32_t iclk = hw.rdSeg(hw.cookie, 2u, kSeg2OptcInClkCtrl + odmB);
		if (iclk != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcInClkCtrl + odmB, iclk & ~3u);
	};

	auto startOtgClocks = [&]() {
		uint32_t iclk = hw.rdSeg(hw.cookie, 2u, kSeg2OptcInClkCtrl + odmB);
		if (iclk != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcInClkCtrl + odmB, iclk | 3u);
		for (uint32_t i = 0u; i < 100u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2OptcInClkCtrl + odmB);
			if (r == 0xffffffffu || (r & 4u) != 0u) break;
			hw.delayUs(hw.cookie, 10u);
		}
		uint32_t clk = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob);
		if (clk != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob, clk | 3u);
		for (uint32_t i = 0u; i < 100u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0ClkCtrl + ob);
			if (r == 0xffffffffu || (r & (1u << 8)) != 0u) break;
			hw.delayUs(hw.cookie, 10u);
		}
	};

	auto startOtg = [&]() {
		// Hardware-verified optc401_enable_crtc (modeset.cpp:338-342).
		const uint32_t srcSel = (snap.optcDataSrc != 0xffffffffu && (snap.optcDataSrc & 0x000f0000u) != 0x000f0000u)
		                            ? snap.optcDataSrc
		                            : (0xfff00000u | (out.otg << 16));
		if (snap.optcMemCfg != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcMemCfg + odmB, snap.optcMemCfg);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcDataSrcSel + odmB, srcSel);
		uint32_t vt = hw.rdSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg);
		if (vt != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg, vt | (1u << 31));
		uint32_t oc = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0Control + ob);
		if (oc != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0Control + ob, (oc & ~(0x301u | (3u << 20))) | (2u << 8) | 1u);
		uint32_t fe = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeCntl + db);
		if (fe != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FeCntl + db, (fe & ~7u) | (out.otg & 7u));
	};

	auto blankStream = [&]() -> bool {
		uint32_t v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
		if (v == 0xffffffffu) return false;
		v = (v & ~(1u | (3u << 8))) | (2u << 8); // ENABLE=0, DIS_DEFER=2
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db, v)) return false;
		bool idle = false;
		for (uint32_t i = 0u; i < 5000u; ++i) {
			v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
			if (v != 0xffffffffu && (v & (1u << 16)) == 0u) { idle = true; break; }
			hw.delayUs(hw.cookie, 10u);
		}
		if (!idle) return false;
		uint32_t sf = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db);
		if (sf != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db, sf | 2u);
		hw.delayUs(hw.cookie, 20000u); // 20ms idle pattern for clean sink transition
		return true;
	};

	auto programVidMN = [&](uint64_t pixClkHz, uint8_t linkRateCode, uint32_t pixPerCycleCode) {
		const uint32_t symClkKhz = rdna4aux::linkCodeToRateKbps(linkRateCode) / 10u;
		const uint32_t pixClkKhz = static_cast<uint32_t>((pixClkHz + 500ULL) / 1000ULL);
		if (!symClkKhz || !pixClkKhz) return;
		const uint32_t mVid = static_cast<uint32_t>((0x8000ULL * pixClkKhz) / symClkKhz);
		uint32_t vt = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidTiming + db);
		if (vt != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidTiming + db, vt & ~(1u << 8));
		uint32_t vn = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidN + db);
		if (vn != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidN + db, (vn & ~0xffffffu) | 0x8000u);
		uint32_t vm = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidM + db);
		if (vm != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidM + db, (vm & ~0xffffffu) | (mVid & 0xffffffu));
		if (vt != 0xffffffffu) {
			// DP_VID_N_INTERVAL is bits [15:14] (0=1px, 1=2px, 2=4px, 3=8px per cycle).
			vt = (vt & ~0x0000c000u) | ((pixPerCycleCode & 3u) << 14) | (1u << 8);
			(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidTiming + db, vt);
		}
	};

	auto resyncAndUnblank = [&](uint32_t pixPerCycleCode) -> bool {
		// Exact hardware-verified DCN 4.1 DP unblank sequence (modeset.cpp:365-380 / enc401_stream_encoder_dp_unblank).
		uint32_t v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
		if (v == 0xffffffffu) return false;
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db, v & ~1u);
		for (uint32_t i = 0u; i < 500u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
			if (r != 0xffffffffu && (r & (1u << 16)) == 0u) break;
			hw.delayUs(hw.cookie, 100u);
		}

		uint32_t sf = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db);
		if (sf == 0xffffffffu) return false;
		sf &= ~0x74u;
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db, sf | 2u);
		hw.delayUs(hw.cookie, 10u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db, sf & ~2u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0SteerFifo + db, (sf & ~2u) | 1u);

		// Start video stream before resetting DIG FIFO so FIFO primes cleanly (modeset.cpp:371-380).
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
		if (v == 0xffffffffu) return false;
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db, (v & ~0x301u) | 0x201u)) return false;
		hw.delayUs(hw.cookie, 200u);

		// enc401_set_dig_input_mode: DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE [9:8] MUST match OTG output rate
		// (0=1px, 1=2px when pixclk > DPREFCLK, 2=4px when > 2*DPREFCLK, 3=8px when > 4*DPREFCLK).
		uint32_t df = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db);
		if (df == 0xffffffffu) return false;
		df = (df & ~0x3010037fu) | (7u << 2) | ((pixPerCycleCode & 3u) << 8);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db, df | 2u);
		for (uint32_t i = 0u; i < 2000u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db);
			if (r != 0xffffffffu && (r & (1u << 20)) != 0u) break;
			hw.delayUs(hw.cookie, 10u);
		}
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db, df & ~2u);
		for (uint32_t i = 0u; i < 2000u; ++i) {
			const uint32_t r = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db);
			if (r != 0xffffffffu && (r & (1u << 20)) == 0u) break;
			hw.delayUs(hw.cookie, 10u);
		}
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FifoCtrl0 + db, (df & ~2u) | 1u);
		hw.delayUs(hw.cookie, 100u);

		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
		if (v == 0xffffffffu) return false;
		if (!hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db, v | 1u)) return false;
		for (uint32_t i = 0u; i < 1000u; ++i) {
			v = hw.rdSeg(hw.cookie, 2u, kSeg2Dp0VidStreamCntl + db);
			if (v != 0xffffffffu && (v & 0x10001u) == 0x10001u) return true;
			hw.delayUs(hw.cookie, 100u);
		}
		return false;
	};

	auto rebindDigStream = [&]() {
		// Exact hardware-verified DCN 4.1 stream-to-link binding (dpphy.cpp::streamSteps).
		uint32_t v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeClkCntl + be);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0BeClkCntl + be, (v & ~0x7u) | 0x10u);
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeEnCntl + be);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0BeEnCntl + be, v | 1u);
		v = hw.rdSeg(hw.cookie, 1u, kSeg1SymClkAClkEn + out.dig);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 1u, kSeg1SymClkAClkEn + out.dig, (v & ~0x710u) | (link << 8) | 0x10u);
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0BeCntl + be);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0BeCntl + be, v | (0x100u << out.dig));
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeClkCntl + db);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FeClkCntl + db, (v & ~0x17u) | 0x10u);
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeEnCntl + db);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FeEnCntl + db, v | 1u);
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0StreamMap + out.dig);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0StreamMap + out.dig, (v & ~7u) | (link & 7u));
		v = hw.rdSeg(hw.cookie, 2u, kSeg2Dig0FeCntl + db);
		if (v != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dig0FeCntl + db, (v & ~7u) | (out.otg & 7u));
	};

	auto computePixPerCycleCode = [&](uint64_t pixClkHz) -> uint32_t {
		if (!snap.dtoModulo) return 0u;
		if (pixClkHz > 4ULL * snap.dtoModulo) return 3u; // 8 px/cycle
		if (pixClkHz > 2ULL * snap.dtoModulo) return 2u; // 4 px/cycle
		if (pixClkHz > static_cast<uint64_t>(snap.dtoModulo)) return 1u; // 2 px/cycle
		return 0u; // 1 px/cycle
	};
	const uint32_t snapDtoInt = (snap.pixRateDiv >> divShift) & 0x0fu;
	const uint64_t snapPixHz = static_cast<uint64_t>(snapDtoInt) * snap.dtoModulo + snap.dtoPhase;
	const uint32_t snapPixCode = computePixPerCycleCode(snapPixHz);
	const uint32_t targetPixCode = computePixPerCycleCode(target.pixelClockHz);

	auto rollback = [&]() {
		res.rolledBack = true;
		(void)blankStream();
		stopOtg();
		if (plan.useDsc && !snap.dscWasOn) (void)configureDscHardware(hw, out, false, nullptr);
		if (plan.targetLinkRate != snap.linkRate || plan.useFec != snap.fecWasOn) {
			(void)trainDpLink(hw, out, dpcd, snap.linkRate, snap.lanes, snap.fecWasOn);
			rebindDigStream();
		}
		(void)hw.wrSeg(hw.cookie, 1u, kSeg1DpDto0Phase + out.otg * 4u, snap.dtoPhase);
		(void)hw.wrSeg(hw.cookie, 1u, kSeg1DpDto0Modulo + out.otg * 4u, snap.dtoModulo);
		(void)hw.wrSeg(hw.cookie, 1u, kSeg1OtgPixRateDiv, snap.pixRateDiv);
		startOtgClocks();
		if (snap.optcDataFmt != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcDataFmt + odmB, snap.optcDataFmt);
		if (snap.optcBytesPerPx != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcBytesPerPx + odmB, snap.optcBytesPerPx);
		if (snap.optcWidthCtrl != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcWidthCtrl + odmB, snap.optcWidthCtrl);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HTotal + ob, snap.hTotal);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HBlankSE + ob, snap.hBlankSE);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HSyncA + ob, snap.hSyncA);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HSyncACntl + ob, snap.hSyncACntl);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotal + ob, snap.vTotal);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotalMin + ob, snap.vTotalMin);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotalMax + ob, snap.vTotalMax);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VBlankSE + ob, snap.vBlankSE);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VSyncA + ob, snap.vSyncA);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VSyncACntl + ob, snap.vSyncACntl);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VStartup + ob, snap.vStartup);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VUpdate + ob, snap.vUpdate);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VReady + ob, snap.vReady);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0PstateReg + ob, snap.pstateReg);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg, snap.vtgCtrl & ~(1u << 31));
		startOtg();
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0MasterLock + ob, 1u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2HubpReq0Blank0 + hb, snap.hubpBlank0);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0MasterLock + ob, 0u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam1 + db, snap.msa1);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam2 + db, snap.msa2);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam3 + db, snap.msa3);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam4 + db, snap.msa4);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0PixelFormat + db, snap.pixelFmt);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaColorim + db, snap.msaColorim);
		if (snap.msaVbidMisc != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaVbidMisc + db, snap.msaVbidMisc);
		if (snap.vidN != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidN + db, snap.vidN);
		if (snap.vidM != 0xffffffffu) (void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidM + db, snap.vidM);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0VidTiming + db, snap.vidTiming);
		(void)resyncAndUnblank(snapPixCode);
	};

	// 2. Blank DP stream and stop OTG cleanly before changing link / DTO / timing / DSC (modeset.cpp:258-276).
	if (!blankStream()) {
		res.status = kBlankTimeout;
		return res;
	}
	stopOtg();

	// 3. Retrain DP link if rate or FEC state changed, or when forced (e.g. DPMS wake), then re-bind DIG FE <-> BE.
	if (forceRetrain || plan.targetLinkRate != snap.linkRate || plan.useFec != snap.fecWasOn) {
		if (!trainDpLink(hw, out, dpcd, plan.targetLinkRate, plan.targetLanes, plan.useFec)) {
			res.status = kLinkTrainFailed;
			rollback();
			return res;
		}
		rebindDigStream();
	}

	// 4. Configure or disable DSC pipeline (DOMAIN16 PG + DSCCLK + DSCC + DSCRM + VPG SRAM + PPS + VBID6 + DPCD 0x0160).
	if (plan.useDsc != snap.dscWasOn || plan.useDsc) {
		if (!configureDscHardware(hw, out, plan.useDsc, plan.useDsc ? &dscParams : nullptr)) {
			res.status = kResyncFailed;
			rollback();
			return res;
		}
	}

	// 5. Program DP_DTO0 while OTG clocks are off (modeset.cpp:278-285 / dccg401_set_dp_dto).
	(void)hw.wrSeg(hw.cookie, 1u, kSeg1DpDto0Phase + out.otg * 4u, tr.dtoPhase);
	(void)hw.wrSeg(hw.cookie, 1u, kSeg1DpDto0Modulo + out.otg * 4u, tr.dtoModulo);
	const uint32_t nextDiv = (snap.pixRateDiv & ~divMask) | ((tr.dtoInt << divShift) & divMask);
	(void)hw.wrSeg(hw.cookie, 1u, kSeg1OtgPixRateDiv, nextDiv);

	// 6. Re-enable OPTC/OTG clocks, program OTG timing + VTG + OPTC DSC while OTG is stopped, then enable OTG.
	startOtgClocks();
	if (plan.useDsc != snap.dscWasOn || plan.useDsc) {
		configureOptcDsc(hw, out.otg, plan.useDsc, plan.useDsc ? &dscParams : nullptr);
	}
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HTotal + ob, tr.hTotal);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HSyncA + ob, tr.hSyncA);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HBlankSE + ob, tr.hBlankSE);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0HSyncACntl + ob, tr.hSyncACntl);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotal + ob, tr.vTotal);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotalMin + ob, tr.vTotalMin);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VTotalMax + ob, tr.vTotalMax);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VSyncA + ob, tr.vSyncA);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VBlankSE + ob, tr.vBlankSE);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VSyncACntl + ob, tr.vSyncACntl);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VStartup + ob, tr.vStartup);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VUpdate + ob, tr.vUpdate);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0VReady + ob, tr.vReady);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0PstateReg + ob, tr.pstateReg);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Vtg0Control + out.otg, tr.vtgCtrl & 0x7fff7fffu);
	startOtg();

	// Update HUBPREQ0_BLANK_OFFSET_0 (exact 50 MHz DCHUB ref clock formula from modeset.hpp:145) under OTG lock.
	const uint32_t pixClkKhz = static_cast<uint32_t>((target.pixelClockHz + 500ULL) / 1000ULL);
	if (pixClkKhz > 0u) {
		const uint32_t refHBlankEnd = static_cast<uint32_t>(
		    (static_cast<uint64_t>(tr.hBlankEnd) * 50000ULL) / pixClkKhz);
		const uint32_t dlgBlank = ((tr.vBlankEnd & 0x7fffu) << 16) | (refHBlankEnd & 0x1fffu);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0MasterLock + ob, 1u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2HubpReq0Blank0 + hb, dlgBlank);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2Otg0MasterLock + ob, 0u);
	}

	// 7. Program DP MSA timing + 8 bpc RGB (or DSC compressed) format + initial DP_VID_M/N.
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam1 + db, tr.msa1);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam2 + db, tr.msa2);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam3 + db, tr.msa3);
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaParam4 + db, tr.msa4);

	uint32_t pf = (snap.pixelFmt & ~0x00010731u) | (plan.useDsc ? 1u : 0u) | (1u << 8); // 8 bpc RGB, DSC bit 0
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0PixelFormat + db, pf);
	uint32_t col = (snap.msaColorim & ~0xff000000u) | (0x20u << 24); // MISC0: 8 bpc RGB
	(void)hw.wrSeg(hw.cookie, 2u, kSeg2Dp0MsaColorim + db, col);
	programVidMN(target.pixelClockHz, plan.targetLinkRate, targetPixCode);

	// 8. Resync FIFOs (with matching DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE) and unblank DP stream.
	if (!resyncAndUnblank(targetPixCode)) {
		res.status = kUnblankTimeout;
		rollback();
		return res;
	}

	// 9. Clear any transition underflow latch, then verify live OTG frame progression & rate.
	uint32_t optcG = hw.rdSeg(hw.cookie, 2u, kSeg2OptcInGlobal + odmB);
	if (optcG != 0xffffffffu && (optcG & 1u) == 0u) {
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcInGlobal + odmB, (optcG & ~0x80003c00u) | 0x1000u);
		(void)hw.wrSeg(hw.cookie, 2u, kSeg2OptcInGlobal + odmB, optcG & ~0x80003c00u);
	}
	// Sample OTG_STATUS_FRAME_COUNT over 150 ms to verify frames advance at target refresh rate.
	const uint64_t t0 = hw.nowUs(hw.cookie);
	const uint32_t f0 = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0StatusFrame + ob);
	hw.delayUs(hw.cookie, 150000u);
	const uint64_t t1 = hw.nowUs(hw.cookie);
	const uint32_t f1 = hw.rdSeg(hw.cookie, 2u, kSeg2Otg0StatusFrame + ob);
	const uint64_t dtUs = (t1 > t0) ? (t1 - t0) : 0ULL;
	const uint32_t df = f1 - f0;
	if (df == 0u || dtUs == 0ULL) {
		res.status = kRateMismatch;
		rollback();
		return res;
	}
	res.measuredMilliHz = static_cast<uint32_t>((static_cast<uint64_t>(df) * 1000000000ULL) / dtUs);
	const uint32_t wantMilliHz = target.refreshMilliHz();
	const uint32_t diff = (res.measuredMilliHz >= wantMilliHz)
	                          ? (res.measuredMilliHz - wantMilliHz)
	                          : (wantMilliHz - res.measuredMilliHz);
	// Allow +/- 15% over an 80 ms window (integer frame quantization is +/- 1 frame = ~12.5 Hz).
	if (static_cast<uint64_t>(diff) * 100ULL > static_cast<uint64_t>(wantMilliHz) * 15ULL + 1500000ULL) {
		res.status = kRateMismatch;
		rollback();
		return res;
	}

	optcG = hw.rdSeg(hw.cookie, 2u, kSeg2OptcInGlobal + odmB);
	if (optcG != 0xffffffffu && (optcG & 0x2c00u) != 0u) {
		res.status = kUnderflow;
		rollback();
		return res;
	}

	res.status = kOk;
	return res;
}

}  // namespace rdna4modeset
