// garnet_power_policy_pure.h — Pure GFX12 clock-gating bit transforms and
// Stage 3C power/DET0/UCLK/GFXOFF boot-arg policy latches.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
// Register offsets and masks verified against upstream Linux torvalds/linux:
//   drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h
//   drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h
//   drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c
#pragma once

#include <stdint.h>

namespace rdna4pwr {

// Boot-arg kill-switches / latches:
//   garnet-gfx-cg=0     disables GFX12 CGCG/CGLS/MGCG/FGCG (default ON)
//   garnet-det0=0       disables single-pipe DCN41 DET0 expansion (default ON)
//   garnet-uclk-pin=1   forces UCLK pinned at top DPM (default AUTO after DET0 expand)
//   garnet-gfxoff=1     opt-in SMU AllowGfxOff (default OFF)
//   garnet-idle-clamp=1 opt-in 200 MHz GFXCLK Low clamp at boot (default OFF: SMUPowerState::Auto)
inline bool gfx_cg_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}
inline bool det0_expand_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}
inline bool uclk_force_pin(bool present, uint32_t value) {
	return present && value == 1u;
}
inline bool gfxoff_opt_in(bool present, uint32_t value) {
	return present && value != 0u;
}
inline bool idle_clamp_enabled(bool present, uint32_t value) {
	return present && value != 0u;
}
inline bool fams2_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}

constexpr uint8_t kFams21Version = (2u << 5) | 1u;

struct Fams2ModeProof {
	bool allowed;
	bool dalActive;
	bool dmlPstateSupported;
	bool configCompleted;
	bool mallProgrammed;
	uint8_t fwVersion;
	uint64_t configGeneration;
	uint64_t liveModeGeneration;
	bool underflowLatched;
};

// Minimum DET0 buffer time and VBLANK duration required to safely absorb GDDR6 UCLK
// P-state switch blackouts (~150-250 us) without DMUB FAMS2 / DML2 watermark programming.
// Live RX 9060 XT verification:
//   4K@60Hz  (533.25 MHz, non-DSC, 576 KiB DET0): detBuf=288 us, vblank=465 us -> 0 underflows
//   4K@160Hz (1479 MHz,   DSC,     576 KiB DET0): detBuf=103 us, vblank=108 us -> OPTC0 underflow
constexpr uint32_t kUclkUnpinMinDetBufUs = 250u;
constexpr uint32_t kUclkUnpinMinVblankUs = 350u;
constexpr uint32_t kDet0ExpandedBytes    = 9u * 65536u; // 576 KiB (9 x 64 KiB CRB segments)

inline uint32_t det0_buffer_us(uint32_t detBytes, uint32_t hActive, uint32_t hTotal,
                               uint64_t pixelClockHz, uint32_t bppBytes = 4u) {
	if (detBytes == 0u || hActive == 0u || hTotal < hActive || pixelClockHz == 0u || bppBytes == 0u)
		return 0u;
	const uint64_t num = static_cast<uint64_t>(detBytes) * static_cast<uint64_t>(hTotal);
	if (num > (UINT64_MAX / 1000000ULL)) return 0u;
	const uint64_t rowBytes = static_cast<uint64_t>(hActive) * static_cast<uint64_t>(bppBytes);
	if (rowBytes == 0u || pixelClockHz > (UINT64_MAX / rowBytes)) return 0u;
	const uint64_t denom = rowBytes * pixelClockHz;
	const uint64_t us = (num * 1000000ULL) / denom;
	return us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
}

inline uint32_t vblank_us(uint32_t vActive, uint32_t vTotal, uint32_t hTotal,
                          uint64_t pixelClockHz) {
	if (vActive == 0u || vTotal <= vActive || hTotal == 0u || pixelClockHz == 0u) return 0u;
	const uint64_t blankPx = static_cast<uint64_t>(vTotal - vActive) * static_cast<uint64_t>(hTotal);
	if (blankPx > (UINT64_MAX / 1000000ULL)) return 0u;
	const uint64_t us = (blankPx * 1000000ULL) / pixelClockHz;
	return us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
}

inline bool uclk_unpin_safe_for_mode(bool uclkForcePin, bool det0Expanded, bool dscEnabled,
                                     uint32_t detBufUs, uint32_t vblankUs,
                                     const Fams2ModeProof &proof = {}) {
	if (uclkForcePin || proof.underflowLatched) return false;
	if (proof.allowed && proof.dalActive && proof.dmlPstateSupported && proof.configCompleted &&
	    proof.mallProgrammed && proof.fwVersion == kFams21Version && proof.configGeneration != 0u &&
	    proof.configGeneration == proof.liveModeGeneration) return true;
	return det0Expanded && !dscEnabled && detBufUs >= kUclkUnpinMinDetBufUs &&
	       vblankUs >= kUclkUnpinMinVblankUs;
}

// RLC_SAFE_MODE (GC BASE_IDX 1, offset 0x0980)
constexpr uint32_t kRlcSafeModeCmdMask    = 0x00000001u;
constexpr uint32_t kRlcSafeModeMsgShift   = 1u;
constexpr uint32_t kRlcSafeModeEnterVal   = kRlcSafeModeCmdMask | (1u << kRlcSafeModeMsgShift);
constexpr uint32_t kRlcSafeModeExitVal    = kRlcSafeModeCmdMask;

// RLC_CGTT_MGCG_OVERRIDE (GC BASE_IDX 1, offset 0x4c48)
constexpr uint32_t kCgttRlcRepeaterFgcgOverride   = 0x00000001u; // bit 0
constexpr uint32_t kCgttRlcSclkOverride           = 0x00000002u; // bit 1
constexpr uint32_t kCgttGfxipMgcgOverride         = 0x00000004u; // bit 2
constexpr uint32_t kCgttGfxipCgcgOverride         = 0x00000008u; // bit 3
constexpr uint32_t kCgttGfxipCglsOverride         = 0x00000010u; // bit 4
constexpr uint32_t kCgttGrbmSclkOverride          = 0x00000020u; // bit 5
// bit 6 = GFXIP_MGLS_OVERRIDE (not enabled on GFX12 in upstream soc24.c)
constexpr uint32_t kCgttGfxipGfx3dCgOverride      = 0x00000080u; // bit 7
constexpr uint32_t kCgttGfxipFgcgOverride         = 0x00000100u; // bit 8
constexpr uint32_t kCgttGfxipRepeaterFgcgOverride = 0x00000200u; // bit 9
constexpr uint32_t kCgttPerfmonClockState         = 0x00000400u; // bit 10

constexpr uint32_t kCgttEnableClearMask =
	kCgttRlcRepeaterFgcgOverride | kCgttRlcSclkOverride | kCgttGfxipMgcgOverride |
	kCgttGfxipCgcgOverride | kCgttGfxipCglsOverride | kCgttGrbmSclkOverride |
	kCgttGfxipGfx3dCgOverride | kCgttGfxipFgcgOverride |
	kCgttGfxipRepeaterFgcgOverride | kCgttPerfmonClockState; // 0x000007BFu

constexpr uint32_t kCgttDisableSetMask =
	kCgttRlcRepeaterFgcgOverride | kCgttRlcSclkOverride | kCgttGfxipMgcgOverride |
	kCgttGrbmSclkOverride | kCgttGfxipFgcgOverride |
	kCgttGfxipRepeaterFgcgOverride | kCgttPerfmonClockState; // 0x00000727u

// RLC_CGCG_CGLS_CTRL (0x4c49) & RLC_CGCG_CGLS_CTRL_3D (0x4cc5)
constexpr uint32_t kCgcgEnMask              = 0x00000001u;
constexpr uint32_t kCglsEnMask              = 0x00000002u;
constexpr uint32_t kCglsRepCompDelayMask    = 0x000000FCu; // shift 2, value 0x0F
constexpr uint32_t kCgcgIdleThreshMask      = 0x07FFFF00u; // shift 8, value 0x36
constexpr uint32_t kCgcgCglsEnableBits      =
	kCgcgEnMask | kCglsEnMask | (0x0Fu << 2) | (0x36u << 8); // 0x0000363Fu

// CP_RB_WPTR_POLL_CNTL (GC BASE_IDX 0, offset 0x0f62): IDLE_POLL_COUNT=0x0090, POLL_FREQ=0x0100
constexpr uint32_t kCpRbWptrPollVal = (0x0090u << 16) | 0x0100u; // 0x00900100u

// CP_INT_CNTL (0x1de9) & CP_INT_CNTL_RING0 (0x1e0a): bits 18..21
constexpr uint32_t kCpIntIdleBitsMask =
	(1u << 18) | (1u << 19) | (1u << 20) | (1u << 21); // CMP_BUSY|CNTX_BUSY|CNTX_EMPTY|GFX_IDLE

// SDMA0_RLC_CGCG_CTRL (0x0055) & SDMA1_RLC_CGCG_CTRL (0x0655): bit 1 = CGCG_INT_ENABLE
constexpr uint32_t kSdmaRlcCgcgIntEnMask = 0x00000002u;

inline uint32_t gfx12_cgtt_mgcg_override(uint32_t cur, bool enable) {
	return enable ? (cur & ~kCgttEnableClearMask) : (cur | kCgttDisableSetMask);
}

inline uint32_t gfx12_cgcg_cgls_ctrl(uint32_t cur, bool enable) {
	if (!enable) return cur & ~(kCgcgEnMask | kCglsEnMask);
	return (cur & ~(kCglsRepCompDelayMask | kCgcgIdleThreshMask)) | kCgcgCglsEnableBits;
}

inline uint32_t gfx12_cp_rb_wptr_poll_cntl(uint32_t) {
	return kCpRbWptrPollVal;
}

inline uint32_t gfx12_cp_int_cntl_idle(uint32_t cur, bool enable) {
	return enable ? (cur | kCpIntIdleBitsMask) : (cur & ~kCpIntIdleBitsMask);
}

inline uint32_t gfx12_sdma_rlc_cgcg_ctrl(uint32_t cur, bool enable) {
	return enable ? (cur | kSdmaRlcCgcgIntEnMask) : (cur & ~kSdmaRlcCgcgIntEnMask);
}

}  // namespace rdna4pwr
