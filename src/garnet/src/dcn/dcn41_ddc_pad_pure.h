//
//  dcn41_ddc_pad_pure.h — Pure DCN 4.01 DP/DDC pad AUX<->I2C mode switching helpers.
//
//  Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (src/device.cpp ddcPadMode,
//  commits 7c8cf3a5, bb4c5145, aefc2385), Copyright (c) 2026 Sunneva N. Mariu,
//  licensed under the BSD 3-Clause License (see third-party/THIRD-PARTY-LICENSES.txt).
//
#pragma once

#include <stdint.h>

namespace rdna4ddcpad {

constexpr uint8_t  kMaxDdcLines       = 4u;
constexpr uint32_t kSeg2GpioDdc1Mask  = 0x28d0u; // DC_GPIO_DDC1_MASK (+ 4 * line; within allowlist 0x2868..0x2961)
constexpr uint32_t kGpioDdcClkPdEn    = 1u << 4;
constexpr uint32_t kGpioDdcDataPdEn   = 1u << 12;
constexpr uint32_t kGpioDdcPdEn       = kGpioDdcClkPdEn | kGpioDdcDataPdEn;
constexpr uint32_t kGpioDdcAuxPadMode = 1u << 16;
constexpr uint32_t kSeg2GpioAuxCtrl5  = 0x291du; // DC_GPIO_AUX_CTRL_5 (DDC_PADn_I2CMODE at bit 12 + line)
constexpr uint32_t kSeg2PhyAuxCntl    = 0x28ffu; // PHY_AUX_CNTL (AUXn_PAD_RXSEL at [11:10] + 2 * line)

constexpr uint32_t ddc_mask_reg(uint8_t line) {
	return kSeg2GpioDdc1Mask + 4u * static_cast<uint32_t>(line);
}

constexpr uint32_t ddc_i2c_mode_bit(uint8_t line) {
	return 1u << (12u + static_cast<uint32_t>(line));
}

constexpr uint32_t aux_rxsel_shift(uint8_t line) {
	return 10u + 2u * static_cast<uint32_t>(line);
}

struct DdcPadHw {
	void *cookie { nullptr };
	uint32_t (*rdSeg2)(void *cookie, uint32_t seg2Offset) { nullptr };
	bool (*wrSeg2)(void *cookie, uint32_t seg2Offset, uint32_t value) { nullptr };
	void (*sleepMs)(void *cookie, uint32_t ms) { nullptr };
};

// Switches `line` (0..3) between DisplayPort AUX mode (`i2cMode = false`) and passive
// DP++/HDMI I2C mode (`i2cMode = true`), matching DCN 4.01 hw_ddc sequencing.
inline bool set_ddc_pad_mode(const DdcPadHw &hw, uint8_t line, bool i2cMode) {
	if (line >= kMaxDdcLines || !hw.rdSeg2 || !hw.wrSeg2) return false;

	const uint32_t rMask = ddc_mask_reg(line);
	const uint32_t was = hw.rdSeg2(hw.cookie, rMask);
	if (was == 0xffffffffu) return false;

	if (!i2cMode) {
		if (!(was & kGpioDdcAuxPadMode)) {
			if (!hw.wrSeg2(hw.cookie, rMask, was | kGpioDdcAuxPadMode)) return false;
		}
		const uint32_t ctrl5 = hw.rdSeg2(hw.cookie, kSeg2GpioAuxCtrl5);
		if (ctrl5 == 0xffffffffu) return false;
		return hw.wrSeg2(hw.cookie, kSeg2GpioAuxCtrl5, ctrl5 & ~ddc_i2c_mode_bit(line));
	}

	if ((was & kGpioDdcPdEn) != kGpioDdcPdEn) {
		if (!hw.wrSeg2(hw.cookie, rMask, was | kGpioDdcPdEn)) return false;
		if (hw.sleepMs) hw.sleepMs(hw.cookie, 3u);
	}

	const uint32_t afterPd = hw.rdSeg2(hw.cookie, rMask);
	if (afterPd == 0xffffffffu) return false;
	if (afterPd & kGpioDdcAuxPadMode) {
		if (hw.sleepMs) hw.sleepMs(hw.cookie, 2u);
		const uint32_t cur = hw.rdSeg2(hw.cookie, rMask);
		if (cur == 0xffffffffu) return false;
		if (!hw.wrSeg2(hw.cookie, rMask, cur & ~kGpioDdcAuxPadMode)) return false;
	}

	const uint32_t ctrl5 = hw.rdSeg2(hw.cookie, kSeg2GpioAuxCtrl5);
	if (ctrl5 == 0xffffffffu) return false;
	if (!hw.wrSeg2(hw.cookie, kSeg2GpioAuxCtrl5, ctrl5 | ddc_i2c_mode_bit(line))) return false;

	const uint32_t cntl = hw.rdSeg2(hw.cookie, kSeg2PhyAuxCntl);
	if (cntl == 0xffffffffu) return false;
	const uint32_t shift = aux_rxsel_shift(line);
	return hw.wrSeg2(hw.cookie, kSeg2PhyAuxCntl, (cntl & ~(3u << shift)) | (1u << shift));
}

} // namespace rdna4ddcpad
