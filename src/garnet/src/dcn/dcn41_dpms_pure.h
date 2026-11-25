// dcn41_dpms_pure.h — Pure DCN 4.1 DisplayPort DPMS sleep/wake helpers.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stdint.h>

#include "dcn41_ddc_pad_pure.h"
#include "dcn41_modeset.h"

namespace rdna4dpms {

constexpr uint32_t kDpcdSetPowerState   = 0x0600u;
constexpr uint8_t  kDpPowerStateD0      = 0x01u;
constexpr uint8_t  kDpPowerStateD3      = 0x02u;
constexpr uint32_t kStreamIdlePolls     = 5000u;   // 50 ms at 10 us/step
constexpr uint32_t kBlankSettleUs       = 20000u;  // 20 ms idle pattern settle
constexpr uint32_t kWakeSettleUs        = 20000u;  // 20 ms sink D0 wakeup settle
constexpr uint32_t kWakeAuxRetries      = 3u;

// Boot-arg `garnet-dpms` (default ON unless explicitly set to 0).
inline bool boot_arg_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}

// Effective display power demand from IOFramebuffer kIOPowerAttribute (0/1=off, 2=on)
// and kConnectionPower (0=off, non-zero=on).
inline bool should_be_on(uint32_t fbPowerState, uint32_t connPower) {
	return fbPowerState >= 2u && connPower != 0u;
}

struct OffResult {
	bool streamBlanked { false };
	bool phyDisabled { false };
	bool sinkD3Written { false };
};

// Blank DP video stream, disable PHY transmitter via DMUB, clear link training/enable
// bits, and place the DP sink into D3 power state (DPCD 0x00600 = 0x02).
// Does NOT stop OTG/DTO registers so captureSnapshot() stays valid for clean wake retrain.
inline OffResult dpmsOff(const rdna4modeset::Hw &hw, const rdna4aux::ActiveOutput &out) {
	OffResult res {};
	if (!hw.rdSeg || !hw.wrSeg || !hw.delayUs || !out.found || !out.isDp ||
	    out.dig >= 4u || out.otg >= 4u) {
		return res;
	}
	const uint32_t link = (out.link < 4u) ? out.link : out.dig;
	const uint32_t db = out.dig * rdna4modeset::kSeg2DigStride;
	const uint32_t be = link * rdna4modeset::kSeg2DigStride;

	// 1. Blank DP video stream and wait for DP_VID_STREAM_STATUS (bit 16) == 0.
	uint32_t v = hw.rdSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0VidStreamCntl + db);
	if (v != 0xffffffffu) {
		v = (v & ~(1u | (3u << 8))) | (2u << 8); // ENABLE=0, DIS_DEFER=2
		if (hw.wrSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0VidStreamCntl + db, v)) {
			for (uint32_t i = 0u; i < kStreamIdlePolls; ++i) {
				const uint32_t r = hw.rdSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0VidStreamCntl + db);
				if (r != 0xffffffffu && (r & (1u << 16)) == 0u) {
					res.streamBlanked = true;
					break;
				}
				hw.delayUs(hw.cookie, 10u);
			}
		}
	}
	if (res.streamBlanked) {
		const uint32_t sf = hw.rdSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0SteerFifo + db);
		if (sf != 0xffffffffu) {
			(void)hw.wrSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0SteerFifo + db, sf | 2u);
		}
		hw.delayUs(hw.cookie, kBlankSettleUs);
	}

	// 2. Notify sink to enter D3 sleep (DPCD 0x00600 = 0x02) BEFORE dropping PHY link
	//    (matching upstream AMD DC dp_disable_link_phy order so sinks see explicit D3 before LOS).
	if (out.auxValid && hw.auxIo.read && hw.auxIo.write) {
		const uint8_t d3 = kDpPowerStateD3;
		res.sinkD3Written = rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd,
		                                        kDpcdSetPowerState, &d3, 1u);
	}

	// 3. Disable PHY transmitter via DMUB DIG1_TRANSMITTER_CONTROL (action=2 -> DISABLE).
	if (hw.phyControl) {
		res.phyDisabled = hw.phyControl(hw.cookie, 2u, 0u, 0u, 0u, 0u);
	}
	(void)hw.wrSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0TrainPatSel + be, 0u);
	const uint32_t lc = hw.rdSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0LinkCntl + be);
	if (lc != 0xffffffffu) {
		(void)hw.wrSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0LinkCntl + be, lc & ~(1u << 4));
	}
	(void)hw.wrSeg(hw.cookie, 2u, rdna4modeset::kSeg2Dp0DphyIntCtrl + be, 0u);
	return res;
}

struct WakePrepResult {
	bool auxPadRestored { false };
	bool sinkD0Written { false };
};

// Prepare a DP sink for link retraining on DPMS wake: restore AUX pads, write
// DPCD 0x00600 = 0x01 (D0) with bounded retries, and wait 20 ms for sink wakeup.
inline WakePrepResult dpmsWakePrepare(const rdna4modeset::Hw &hw,
                                      const rdna4aux::ActiveOutput &out,
                                      const rdna4ddcpad::DdcPadHw *padHw) {
	WakePrepResult res {};
	if (!hw.delayUs || !out.found || !out.isDp || !out.auxValid) return res;

	if (padHw && out.aux < rdna4ddcpad::kMaxDdcLines) {
		res.auxPadRestored = rdna4ddcpad::set_ddc_pad_mode(*padHw, static_cast<uint8_t>(out.aux), false);
	}
	if (hw.auxIo.read && hw.auxIo.write) {
		const uint8_t d0 = kDpPowerStateD0;
		for (uint32_t attempt = 0u; attempt < kWakeAuxRetries; ++attempt) {
			if (rdna4aux::dpcdWrite(hw.auxIo, out.aux, out.hpd, kDpcdSetPowerState, &d0, 1u)) {
				res.sinkD0Written = true;
				break;
			}
			hw.delayUs(hw.cookie, 5000u);
		}
	}
	hw.delayUs(hw.cookie, kWakeSettleUs);
	return res;
}

}  // namespace rdna4dpms
