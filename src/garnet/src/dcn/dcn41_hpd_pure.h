// dcn41_hpd_pure.h — Pure DCN 4.0.1 DisplayPort HPD & link-loss recovery state machine,
// DIG_FIFO_CTRL0 error-clear helper, and AUX-route restoration sequence.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stdint.h>

#include "dcn41_ddc_pad_pure.h"
#include "dcn41_modeset.h"
#include "dp_edid_aux.h"

namespace rdna4hpd {

// ---- DCN 4.0.1 DMU segment 2 (BASE_IDX 2, default 0x34c0) constants ----
constexpr uint32_t kDefaultDmuSeg2Base    = rdna4aux::kDefaultDmuSeg2Base; // 0x34c0
constexpr uint32_t kMaxHpdPins            = 4u;
constexpr uint32_t kMaxDigInsts           = 4u;

// HPD0..3 DC_HPD_INT_STATUS (seg2 + 0x175a + 8*i; abs 0x4c1a..0x4c32)
constexpr uint32_t kSeg2Hpd0IntStatus     = rdna4aux::kHpdStatus0; // 0x175a
constexpr uint32_t kSeg2Hpd0IntControl    = 0x175bu;
constexpr uint32_t kSeg2HpdStride         = rdna4aux::kHpdStride;  // 0x0008
// DMU regHPD_INTERRUPT_DEST (seg2 + 0x0161; abs 0x3621):
// bits [5:0] = HPD1..6_DEST, bits [13:8] = HPD1..6_RX_DEST (0 = Host IH, 1 = DMCUB).
constexpr uint32_t kSeg2HpdInterruptDest  = 0x0161u;

constexpr uint32_t kHpdIntStatusMask      = 0x00000001u; // bit 0: DC_HPD_INT_STATUS
constexpr uint32_t kHpdSenseMask          = 0x00000002u; // bit 1: DC_HPD_SENSE
constexpr uint32_t kHpdSenseDelayedMask   = 0x00000010u; // bit 4: DC_HPD_SENSE_DELAYED
constexpr uint32_t kHpdConnectedMask      = kHpdSenseMask | kHpdSenseDelayedMask; // 0x12
constexpr uint32_t kHpdRxIntStatusMask    = 0x00000100u; // bit 8: DC_HPD_RX_INT_STATUS
constexpr uint32_t kHpdAnyIrqStatusMask   = kHpdIntStatusMask | kHpdRxIntStatusMask;

constexpr uint32_t kHpdIntAckMask         = 0x00000001u; // bit 0:  DC_HPD_INT_ACK
constexpr uint32_t kHpdIntPolarityMask    = 0x00000100u; // bit 8:  DC_HPD_INT_POLARITY
constexpr uint32_t kHpdIntEnMask          = 0x00010000u; // bit 16: DC_HPD_INT_EN
constexpr uint32_t kHpdRxIntAckMask       = 0x00100000u; // bit 20: DC_HPD_RX_INT_ACK
constexpr uint32_t kHpdRxIntEnMask        = 0x01000000u; // bit 24: DC_HPD_RX_INT_EN

// DIG0..3 DIG_FIFO_CTRL0 (seg2 + 0x209b + 0x124*dig; DIG1 abs = 0x567f)
constexpr uint32_t kSeg2Dig0FifoCtrl0     = rdna4modeset::kSeg2Dig0FifoCtrl0; // 0x209b
constexpr uint32_t kSeg2DigStride         = rdna4modeset::kSeg2DigStride;     // 0x0124
constexpr uint32_t kDigFifoEnableMask     = 0x00000001u; // bit 0:    DIG_FIFO_ENABLE
constexpr uint32_t kDigFifoResetMask      = 0x00000002u; // bit 1:    DIG_FIFO_RESET
constexpr uint32_t kDigFifoReadStartMask  = 0x0000007Cu; // bits 6:2: DIG_FIFO_READ_START_LEVEL
constexpr uint32_t kDigFifoReadStartDef   = 0x0000001Cu; // level 7 << 2
constexpr uint32_t kDigFifoPixPerCycMask  = 0x00000300u; // bits 9:8: DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE
constexpr uint32_t kDigFifoPixPerCycShift = 8u;
constexpr uint32_t kDigFifoResetDoneMask  = 0x00100000u; // bit 20:   DIG_FIFO_RESET_DONE (RO)
constexpr uint32_t kDigFifoError0Mask     = 0x10000000u; // bit 28:   DIG_FIFO_ERROR[0]
constexpr uint32_t kDigFifoError1Mask     = 0x20000000u; // bit 29:   DIG_FIFO_ERROR[1]
constexpr uint32_t kDigFifoErrorMask      = 0x30000000u; // bits 29:28: DIG_FIFO_ERROR
// ERROR [29:28] and RESET_DONE [20] are never written 1.
constexpr uint32_t kDigFifoNeverWriteMask = kDigFifoErrorMask | kDigFifoResetDoneMask; // 0x30100000
constexpr uint32_t kDigFifoWriteClearMask =
    kDigFifoNeverWriteMask | kDigFifoPixPerCycMask | kDigFifoReadStartMask |
    kDigFifoResetMask | kDigFifoEnableMask; // 0x3010037f

// Navi 44 read/write hard-deny hazard (ODM_MEM_PWR_CTRL2, seg2 + 0x1e2f, abs 0x52ef).
constexpr uint32_t kHazardAbsDword        = 0x000052efu;

// IH DCE HPD source identifiers (matches src/dcn41/dcn41.h).
constexpr uint32_t kIhClientDce           = 0x04u;
constexpr uint32_t kIhSrcHpd              = 0x09u;

// DPCD addresses & timing defaults.
constexpr uint32_t kDpcdSetPowerState     = 0x0600u;
constexpr uint8_t  kDpPowerStateD0        = 0x01u;
constexpr uint32_t kDpcdLane01Status      = 0x0202u;
constexpr uint32_t kWakeAuxRetries        = 3u;
constexpr uint32_t kWakeSettleUs          = 20000u;
constexpr uint32_t kDigResetPolls         = 2000u; // 20 ms at 10 us/step
constexpr uint32_t kDigEnableSettleUs     = 100u;

// Rate-based HPD IRQ storm guard (sliding 1 s window).
constexpr uint64_t kHpdStormWindowNs      = 1000000000ULL;
constexpr uint32_t kHpdStormMaxIrqs       = 64u;

// Recovery policy defaults.
constexpr uint32_t kDefaultDebounceMs     = 150u;
constexpr uint32_t kDefaultMaxRetries     = 3u;
constexpr uint32_t kDefaultRetryBaseMs    = 500u;
constexpr uint32_t kDefaultRetryMaxMs     = 4000u;
constexpr uint32_t kDefaultCooldownMs     = 15000u;
constexpr uint32_t kDefaultWatchdogMs     = 1000u;

constexpr uint32_t hpd_status_reg(uint32_t hpd) {
	return kSeg2Hpd0IntStatus + hpd * kSeg2HpdStride;
}

constexpr uint32_t hpd_control_reg(uint32_t hpd) {
	return kSeg2Hpd0IntControl + hpd * kSeg2HpdStride;
}

constexpr uint32_t hpd_interrupt_dest_mask(uint32_t hpd) {
	return (hpd < kMaxHpdPins) ? ((1u << hpd) | (1u << (8u + hpd))) : 0u;
}

constexpr uint32_t dig_fifo_ctrl0_reg(uint32_t dig) {
	return kSeg2Dig0FifoCtrl0 + dig * kSeg2DigStride;
}

static_assert(kDefaultDmuSeg2Base + kSeg2HpdInterruptDest == 0x00003621u,
              "DMU regHPD_INTERRUPT_DEST absolute BAR5 dword must be 0x3621");
static_assert(kDefaultDmuSeg2Base + dig_fifo_ctrl0_reg(1u) == 0x0000567fu,
              "DIG1_DIG_FIFO_CTRL0 absolute BAR5 dword must be 0x567f");

constexpr bool is_hazard_abs(uint32_t absDw) {
	return absDw == kHazardAbsDword;
}

constexpr bool is_hazard_seg2(uint32_t seg2Off) {
	return seg2Off == rdna4modeset::kSeg2OdmMemPwrCtrl2;
}

// Boot-arg `garnet-hpd-recover` (default ON unless explicitly set to 0).
inline bool boot_arg_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}

// HPD sense helpers.
constexpr bool hpd_sense_delayed(uint32_t statusReg) {
	return statusReg != 0xffffffffu && (statusReg & kHpdSenseDelayedMask) != 0u;
}

constexpr bool hpd_sense_connected(uint32_t statusReg) {
	return statusReg != 0xffffffffu && (statusReg & kHpdConnectedMask) == kHpdConnectedMask;
}

// DIG_FIFO_CTRL0 helpers.
constexpr bool dig_fifo_has_error(uint32_t regVal) {
	return regVal == 0xffffffffu || (regVal & kDigFifoErrorMask) != 0u;
}

constexpr uint32_t dig_fifo_pix_per_cycle(uint32_t regVal) {
	return (regVal & kDigFifoPixPerCycMask) >> kDigFifoPixPerCycShift;
}

constexpr uint32_t dig_fifo_expected_pix_per_cycle(uint64_t pixelClockHz, uint32_t dtoModuloHz) {
	if (dtoModuloHz == 0u) return 0u;
	if (pixelClockHz > 4ULL * static_cast<uint64_t>(dtoModuloHz)) return 3u;
	if (pixelClockHz > 2ULL * static_cast<uint64_t>(dtoModuloHz)) return 2u;
	if (pixelClockHz > static_cast<uint64_t>(dtoModuloHz)) return 1u;
	return 0u;
}

// Compute the clean write-base value for DIG_FIFO_CTRL0:
// masks out RO/error bits (29:28, 20), clears ENABLE/RESET/READ_START/PIX_PER_CYCLE,
// and installs READ_START_LEVEL=7 + requested PIXEL_PER_CYCLE code.
constexpr uint32_t dig_fifo_clean_base(uint32_t cur, uint32_t pixPerCycleCode) {
	return (cur & ~kDigFifoWriteClearMask) |
	       kDigFifoReadStartDef |
	       ((pixPerCycleCode & 3u) << kDigFifoPixPerCycShift);
}

// DPCD 0x0202..0x0204 link status check (CR + Channel EQ + Symbol Lock + Interlane Align).
inline bool dpcd_link_ok(const uint8_t laneStatus[3], uint8_t lanes) {
	if (!laneStatus || lanes == 0u || lanes > 4u) return false;
	if ((laneStatus[2] & 0x01u) == 0u) return false; // INTERLANE_ALIGN_DONE
	for (uint8_t l = 0u; l < lanes; ++l) {
		const uint8_t nib = (laneStatus[l / 2u] >> ((l & 1u) * 4u)) & 0x0fu;
		if ((nib & 0x07u) != 0x07u) return false;
	}
	return true;
}

// ---- Rate-based HPD IRQ storm guard (matches rdna4vbl::RateStormGuard contract) ----
struct HpdStormGuard {
	uint64_t windowStartNs { 0u };
	uint32_t windowCount { 0u };
	bool     tripped { false };
};

inline void reset_hpd_storm(HpdStormGuard &g) {
	g.windowStartNs = 0u;
	g.windowCount = 0u;
	g.tripped = false;
}

inline bool record_hpd_irq(HpdStormGuard &g, uint64_t nowNs) {
	if (g.tripped) return true;
	if (g.windowStartNs == 0u || nowNs < g.windowStartNs ||
	    (nowNs - g.windowStartNs) >= kHpdStormWindowNs) {
		g.windowStartNs = nowNs;
		g.windowCount = 1u;
		return false;
	}
	g.windowCount++;
	if (g.windowCount > kHpdStormMaxIrqs) {
		g.tripped = true;
		return true;
	}
	return false;
}

// ---- HPD / Link-Loss Recovery State Machine ----
enum class State : uint8_t {
	Connected = 0u,
	Disconnected = 1u,
	Debouncing = 2u,
	Recovering = 3u,
	Cooldown = 4u,
};

enum class Reason : uint8_t {
	None = 0u,
	HpdReconnect = 1u,
	HpdRxShortPulse = 2u,
	LinkLoss = 3u,
};

struct Config {
	uint32_t debounceMs { kDefaultDebounceMs };
	uint32_t maxRetries { kDefaultMaxRetries };
	uint32_t retryBaseBackoffMs { kDefaultRetryBaseMs };
	uint32_t retryMaxBackoffMs { kDefaultRetryMaxMs };
	uint32_t cooldownMs { kDefaultCooldownMs };
};

struct Action {
	bool     runRecovery { false };
	uint32_t scheduleTimerMs { 0u };
	bool     cancelTimer { false };
	Reason   reason { Reason::None };
};

struct StateMachine {
	Config   cfg {};
	State    state { State::Connected };
	Reason   pendingReason { Reason::None };
	bool     enabled { true };
	bool     dpmsOff { false };
	bool     systemSleeping { false };
	bool     lastSenseHigh { true };
	bool     retriggerPending { false };
	uint32_t retryCount { 0u };
	uint64_t debounceDeadlineMs { 0u };
	uint64_t cooldownUntilMs { 0u };
	uint32_t attemptsTotal { 0u };
	uint32_t recoveriesSucceeded { 0u };
	uint32_t recoveriesFailed { 0u };
	uint32_t cooldownTrips { 0u };

	void init(bool enableRecover, const Config &c = Config {}) {
		cfg = c;
		if (cfg.maxRetries == 0u) cfg.maxRetries = 1u;
		if (cfg.retryBaseBackoffMs == 0u) cfg.retryBaseBackoffMs = 100u;
		if (cfg.retryMaxBackoffMs < cfg.retryBaseBackoffMs) {
			cfg.retryMaxBackoffMs = cfg.retryBaseBackoffMs;
		}
		state = State::Connected;
		pendingReason = Reason::None;
		enabled = enableRecover;
		dpmsOff = false;
		systemSleeping = false;
		lastSenseHigh = true;
		retriggerPending = false;
		retryCount = 0u;
		debounceDeadlineMs = 0u;
		cooldownUntilMs = 0u;
		attemptsTotal = 0u;
		recoveriesSucceeded = 0u;
		recoveriesFailed = 0u;
		cooldownTrips = 0u;
	}

	bool suppressed() const {
		return !enabled || dpmsOff || systemSleeping;
	}

	bool shouldRunWatchdog() const {
		return !suppressed() &&
		       (state == State::Connected || state == State::Disconnected || state == State::Cooldown);
	}

	uint32_t computeBackoffMs(uint32_t failedAttempt) const {
		const uint32_t shift = (failedAttempt > 1u) ? ((failedAttempt - 1u) > 8u ? 8u : (failedAttempt - 1u)) : 0u;
		uint64_t ms = static_cast<uint64_t>(cfg.retryBaseBackoffMs) << shift;
		if (ms > cfg.retryMaxBackoffMs) ms = cfg.retryMaxBackoffMs;
		return static_cast<uint32_t>(ms);
	}

	// Merge reasons so an HpdReconnect (full retrain) is never downgraded to HpdRxShortPulse.
	static Reason mergeReason(Reason cur, Reason incoming) {
		if (cur == Reason::HpdReconnect || incoming == Reason::HpdReconnect) return Reason::HpdReconnect;
		if (cur == Reason::LinkLoss || incoming == Reason::LinkLoss) return Reason::LinkLoss;
		return incoming;
	}

	Action setDpmsOff(bool off, uint64_t nowMs) {
		(void)nowMs;
		dpmsOff = off;
		Action act {};
		if (off) {
			retriggerPending = false;
			if (state == State::Debouncing) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				pendingReason = Reason::None;
			}
			act.cancelTimer = true;
		} else {
			// DPMS-on already executed link retrain; clear stale retry/cooldown debt.
			retryCount = 0u;
			cooldownUntilMs = 0u;
			if (state == State::Cooldown || state == State::Debouncing) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				pendingReason = Reason::None;
				act.cancelTimer = true;
			}
		}
		return act;
	}

	Action setSystemSleeping(bool sleeping, uint64_t nowMs) {
		(void)nowMs;
		systemSleeping = sleeping;
		Action act {};
		if (sleeping) {
			retriggerPending = false;
			if (state == State::Debouncing) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				pendingReason = Reason::None;
			}
			act.cancelTimer = true;
		} else {
			retryCount = 0u;
			cooldownUntilMs = 0u;
			if (state == State::Cooldown) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				pendingReason = Reason::None;
			}
		}
		return act;
	}

	Action scheduleDebounce(Reason reason, uint64_t nowMs) {
		Action act {};
		if (suppressed()) {
			act.cancelTimer = true;
			return act;
		}
		if (state == State::Recovering) {
			pendingReason = mergeReason(pendingReason, reason);
			retriggerPending = true;
			return act;
		}
		if (state == State::Cooldown) {
			if (nowMs < cooldownUntilMs) return act;
			retryCount = 0u;
			cooldownUntilMs = 0u;
		}
		pendingReason = (state == State::Debouncing) ? mergeReason(pendingReason, reason) : reason;
		state = State::Debouncing;
		debounceDeadlineMs = nowMs + cfg.debounceMs;
		act.scheduleTimerMs = cfg.debounceMs;
		act.reason = pendingReason;
		return act;
	}

	Action onHpdIrq(bool senseHigh, uint64_t nowMs) {
		Action act {};
		lastSenseHigh = senseHigh;
		if (!enabled) return act;
		if (!senseHigh) {
			// Genuine disconnect cancels debounce and clears retry/cooldown debt for next replug.
			retryCount = 0u;
			cooldownUntilMs = 0u;
			retriggerPending = false;
			pendingReason = Reason::None;
			if (state != State::Recovering) {
				state = State::Disconnected;
			}
			act.cancelTimer = true;
			return act;
		}
		if (suppressed()) {
			act.cancelTimer = true;
			return act;
		}
		return scheduleDebounce(Reason::HpdReconnect, nowMs);
	}

	Action onHpdRxShortPulse(bool senseHigh, uint64_t nowMs) {
		if (!enabled) return Action {};
		if (!senseHigh) return onHpdIrq(false, nowMs);
		lastSenseHigh = true;
		if (suppressed()) return Action { false, 0u, true, Reason::None };
		// A short pulse arriving while we thought cable was disconnected upgrades to full reconnect.
		const Reason r = (state == State::Disconnected) ? Reason::HpdReconnect : Reason::HpdRxShortPulse;
		return scheduleDebounce(r, nowMs);
	}

	Action onLinkFaultDetected(bool senseHigh, uint64_t nowMs) {
		if (!enabled) return Action {};
		if (!senseHigh) return onHpdIrq(false, nowMs);
		lastSenseHigh = true;
		if (suppressed()) return Action { false, 0u, true, Reason::None };
		return scheduleDebounce(Reason::LinkLoss, nowMs);
	}

	// Periodic MMIO watchdog tick: catches missed HPD edges, latched HPD status,
	// or DIG_FIFO_ERROR / inactive-stream faults even if Host IH delivery was lost.
	Action onWatchdogTick(bool senseHigh, bool irqLatched, bool linkFault, uint64_t nowMs) {
		if (suppressed()) return Action { false, 0u, true, Reason::None };
		if (state == State::Recovering || state == State::Debouncing) return Action {};
		if (state == State::Cooldown) {
			if (!senseHigh) {
				(void)onHpdIrq(false, nowMs);
				return Action { false, kDefaultWatchdogMs, false, Reason::None };
			}
			if (nowMs < cooldownUntilMs) {
				return Action { false, kDefaultWatchdogMs, false, Reason::None };
			}
			retryCount = 0u;
			cooldownUntilMs = 0u;
			state = State::Connected;
		}
		if (!senseHigh) {
			if (state != State::Disconnected || lastSenseHigh) {
				(void)onHpdIrq(false, nowMs);
			}
			return Action { false, kDefaultWatchdogMs, false, Reason::None };
		}
		if (state == State::Disconnected || !lastSenseHigh) {
			return onHpdIrq(true, nowMs);
		}
		if (linkFault) {
			return onLinkFaultDetected(true, nowMs);
		}
		if (irqLatched) {
			return onHpdRxShortPulse(true, nowMs);
		}
		return Action { false, kDefaultWatchdogMs, false, Reason::None };
	}

	Action onTimer(uint64_t nowMs) {
		Action act {};
		if (suppressed()) {
			if (state == State::Debouncing) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				pendingReason = Reason::None;
			}
			act.cancelTimer = true;
			return act;
		}
		if (state == State::Cooldown) {
			if (nowMs >= cooldownUntilMs) {
				state = lastSenseHigh ? State::Connected : State::Disconnected;
				retryCount = 0u;
				cooldownUntilMs = 0u;
				act.cancelTimer = true;
			}
			return act;
		}
		if (state != State::Debouncing) return act;
		if (!lastSenseHigh) {
			state = State::Disconnected;
			pendingReason = Reason::None;
			act.cancelTimer = true;
			return act;
		}
		if (nowMs < debounceDeadlineMs) {
			const uint64_t rem = debounceDeadlineMs - nowMs;
			act.scheduleTimerMs = (rem > 0xffffffffULL) ? 0xffffffffu : static_cast<uint32_t>(rem);
			act.reason = pendingReason;
			return act;
		}
		state = State::Recovering;
		retriggerPending = false;
		attemptsTotal++;
		act.runRecovery = true;
		act.reason = (pendingReason == Reason::None) ? Reason::HpdReconnect : pendingReason;
		return act;
	}

	Action onRecoveryResult(bool succeeded, bool senseStillHigh, uint64_t nowMs) {
		Action act {};
		if (state != State::Recovering) return act;
		lastSenseHigh = senseStillHigh;

		if (suppressed()) {
			state = senseStillHigh ? State::Connected : State::Disconnected;
			pendingReason = Reason::None;
			retriggerPending = false;
			act.cancelTimer = true;
			return act;
		}
		if (!senseStillHigh) {
			state = State::Disconnected;
			pendingReason = Reason::None;
			retriggerPending = false;
			retryCount = 0u;
			cooldownUntilMs = 0u;
			act.cancelTimer = true;
			return act;
		}
		if (succeeded) {
			retryCount = 0u;
			cooldownUntilMs = 0u;
			recoveriesSucceeded++;
			if (retriggerPending) {
				retriggerPending = false;
				state = State::Debouncing;
				debounceDeadlineMs = nowMs + cfg.debounceMs;
				act.scheduleTimerMs = cfg.debounceMs;
				act.reason = pendingReason;
				return act;
			}
			state = State::Connected;
			pendingReason = Reason::None;
			act.cancelTimer = true;
			return act;
		}

		// Recovery attempt failed while sink is still connected -> bounded retry + cooldown.
		retryCount++;
		recoveriesFailed++;
		retriggerPending = false;
		if (retryCount >= cfg.maxRetries) {
			state = State::Cooldown;
			cooldownUntilMs = nowMs + cfg.cooldownMs;
			cooldownTrips++;
			pendingReason = Reason::None;
			act.cancelTimer = true;
			return act;
		}
		const uint32_t backoff = computeBackoffMs(retryCount);
		state = State::Debouncing;
		debounceDeadlineMs = nowMs + backoff;
		act.scheduleTimerMs = backoff;
		act.reason = pendingReason;
		return act;
	}
};

// ---- Pure Hardware Helpers (injectable callbacks for kernel + host tests) ----
struct Seg2Hw {
	void *cookie { nullptr };
	uint32_t (*rdSeg2)(void *cookie, uint32_t seg2Off) { nullptr };
	bool (*wrSeg2)(void *cookie, uint32_t seg2Off, uint32_t val) { nullptr };
	void (*delayUs)(void *cookie, uint32_t us) { nullptr };
};

struct HpdDestResult {
	bool     ok { false };
	bool     changed { false };
	bool     hostRouted { false };
	uint32_t before { 0xffffffffu };
	uint32_t after { 0xffffffffu };
};

// Clear HPDn and HPDn_RX destination bits in regHPD_INTERRUPT_DEST (0x3621)
// so hardware delivers HPD events to Host IH (0) instead of UEFI GOP's DMCUB (1).
inline HpdDestResult route_hpd_to_host(const Seg2Hw &hw, uint32_t hpd) {
	HpdDestResult res {};
	const uint32_t mask = hpd_interrupt_dest_mask(hpd);
	if (mask == 0u || !hw.rdSeg2 || !hw.wrSeg2 || is_hazard_seg2(kSeg2HpdInterruptDest)) {
		return res;
	}
	const uint32_t before = hw.rdSeg2(hw.cookie, kSeg2HpdInterruptDest);
	res.before = before;
	res.after = before;
	if (before == 0xffffffffu) return res;
	if ((before & mask) != 0u) {
		if (!hw.wrSeg2(hw.cookie, kSeg2HpdInterruptDest, before & ~mask)) return res;
		res.after = hw.rdSeg2(hw.cookie, kSeg2HpdInterruptDest);
		res.changed = (res.after != before);
	}
	res.hostRouted = (res.after != 0xffffffffu) && ((res.after & mask) == 0u);
	res.ok = true;
	return res;
}

struct ClearFifoResult {
	bool     ok { false };
	bool     resetDoneSeen { false };
	bool     resetClearSeen { false };
	uint32_t before { 0xffffffffu };
	uint32_t after { 0xffffffffu };
};

// Reset DIG_FIFO_CTRL0 to clear latched DIG_FIFO_ERROR (bits 29:28) and restore
// PIXEL_PER_CYCLE + READ_START_LEVEL=7 + ENABLE=1 without ever writing RO bits 29:28/20.
inline ClearFifoResult clear_dig_fifo_error(const Seg2Hw &hw, uint32_t dig, uint32_t pixPerCycleCode) {
	ClearFifoResult res {};
	if (dig >= kMaxDigInsts || !hw.rdSeg2 || !hw.wrSeg2) return res;
	const uint32_t reg = dig_fifo_ctrl0_reg(dig);
	if (is_hazard_seg2(reg)) return res;

	const uint32_t cur = hw.rdSeg2(hw.cookie, reg);
	res.before = cur;
	if (cur == 0xffffffffu) return res;

	const uint32_t base = dig_fifo_clean_base(cur, pixPerCycleCode);
	if (!hw.wrSeg2(hw.cookie, reg, base | kDigFifoResetMask)) return res;
	for (uint32_t i = 0u; i < kDigResetPolls; ++i) {
		const uint32_t r = hw.rdSeg2(hw.cookie, reg);
		if (r != 0xffffffffu && (r & kDigFifoResetDoneMask) != 0u) {
			res.resetDoneSeen = true;
			break;
		}
		if (hw.delayUs) hw.delayUs(hw.cookie, 10u);
	}

	// Always release RESET even if RESET_DONE timed out so the FIFO is never left stuck in reset.
	if (!hw.wrSeg2(hw.cookie, reg, base & ~kDigFifoResetMask)) return res;
	for (uint32_t i = 0u; i < kDigResetPolls; ++i) {
		const uint32_t r = hw.rdSeg2(hw.cookie, reg);
		if (r != 0xffffffffu && (r & kDigFifoResetDoneMask) == 0u) {
			res.resetClearSeen = true;
			break;
		}
		if (hw.delayUs) hw.delayUs(hw.cookie, 10u);
	}

	if (!hw.wrSeg2(hw.cookie, reg, (base & ~kDigFifoResetMask) | kDigFifoEnableMask)) return res;
	if (hw.delayUs) hw.delayUs(hw.cookie, kDigEnableSettleUs);

	const uint32_t after = hw.rdSeg2(hw.cookie, reg);
	res.after = after;
	res.ok = (after != 0xffffffffu) &&
	         ((after & kDigFifoEnableMask) != 0u) &&
	         !dig_fifo_has_error(after) &&
	         (dig_fifo_pix_per_cycle(after) == (pixPerCycleCode & 3u));
	return res;
}

struct AuxRouteResult {
	bool padRestored { false };
	bool auxCtlOk { false };
	uint32_t auxCtlBefore { 0xffffffffu };
	uint32_t auxCtlAfter { 0xffffffffu };
};

// Restore dcn-dp AUX routing: switch DDC pad back to AUX mode (reusing dcn41_ddc_pad_pure.h)
// and ensure DP_AUXn_AUX_CONTROL has AUX_EN=1 and AUX_HPD_SEL == hpd.
inline AuxRouteResult restore_aux_routing(const rdna4ddcpad::DdcPadHw &padHw,
                                          const rdna4aux::Io &auxIo,
                                          uint32_t aux, uint32_t hpd) {
	AuxRouteResult res {};
	if (aux >= rdna4ddcpad::kMaxDdcLines || hpd >= kMaxHpdPins) return res;

	res.padRestored = rdna4ddcpad::set_ddc_pad_mode(padHw, static_cast<uint8_t>(aux), false);
	if (!auxIo.read || !auxIo.write) return res;

	const uint32_t ctlReg = rdna4aux::auxReg(aux, rdna4aux::kAuxRegControl);
	if (is_hazard_seg2(ctlReg)) return res;
	const uint32_t ctl = auxIo.read(auxIo.cookie, ctlReg);
	res.auxCtlBefore = ctl;
	res.auxCtlAfter = ctl;
	if (ctl == 0xffffffffu) return res;

	const uint32_t wantSel = (hpd & 7u) << 20;
	if ((ctl & 1u) == 0u || (ctl & (7u << 20)) != wantSel) {
		const uint32_t next = (ctl & ~(7u << 20)) | wantSel | 1u;
		if (!auxIo.write(auxIo.cookie, ctlReg, next)) return res;
		res.auxCtlAfter = auxIo.read(auxIo.cookie, ctlReg);
	}
	res.auxCtlOk = (res.auxCtlAfter != 0xffffffffu) &&
	               ((res.auxCtlAfter & 1u) != 0u) &&
	               (((res.auxCtlAfter >> 20) & 7u) == hpd);
	return res;
}

struct ProbeResult {
	bool     auxPadRestored { false };
	bool     auxCtlOk { false };
	bool     hpdConnected { false };
	bool     sinkD0Written { false };
	bool     dpcdReadOk { false };
	bool     dpcdLinkOk { false };
	bool     streamActive { false };
	bool     digFifoError { false };
	bool     pixPerCycleMismatch { false };
	bool     needsRetrain { false };
	uint32_t hpdStatusReg { 0xffffffffu };
	uint32_t digFifoReg { 0xffffffffu };
	uint32_t vidStreamReg { 0xffffffffu };
	uint8_t  laneStatus[3] {};
};

// Pre-recovery probe: restore AUX route, verify HPD sense, wake sink D0 on full reconnect,
// and inspect DIG_FIFO_CTRL0 + DP_VID_STREAM_CNTL + DPCD 0x0202..0x0204 to decide retrain.
inline ProbeResult prepare_recovery_aux_and_probe(const Seg2Hw &seg2Hw,
                                                  const rdna4ddcpad::DdcPadHw &padHw,
                                                  const rdna4aux::Io &auxIo,
                                                  const rdna4aux::ActiveOutput &out,
                                                  uint8_t expectedLanes,
                                                  uint32_t expectedPixPerCycle,
                                                  Reason reason) {
	ProbeResult res {};
	if (!seg2Hw.rdSeg2 || !out.found || !out.isDp || !out.auxValid ||
	    out.dig >= kMaxDigInsts || out.hpd >= kMaxHpdPins || out.aux >= rdna4ddcpad::kMaxDdcLines) {
		return res;
	}

	const AuxRouteResult route = restore_aux_routing(padHw, auxIo, out.aux, out.hpd);
	res.auxPadRestored = route.padRestored;
	res.auxCtlOk = route.auxCtlOk;

	const uint32_t hpdOff = hpd_status_reg(out.hpd);
	if (is_hazard_seg2(hpdOff)) return res;
	res.hpdStatusReg = seg2Hw.rdSeg2(seg2Hw.cookie, hpdOff);
	res.hpdConnected = hpd_sense_connected(res.hpdStatusReg);
	if (!res.hpdConnected) return res;

	// On full HPD reconnect or link-loss fault, wake DP sink into D0 (DPCD 0x00600 = 0x01) with bounded retries.
	if ((reason == Reason::HpdReconnect || reason == Reason::LinkLoss) && auxIo.read && auxIo.write) {
		const uint8_t d0 = kDpPowerStateD0;
		for (uint32_t attempt = 0u; attempt < kWakeAuxRetries; ++attempt) {
			if (rdna4aux::dpcdWrite(auxIo, out.aux, out.hpd, kDpcdSetPowerState, &d0, 1u)) {
				res.sinkD0Written = true;
				break;
			}
			if (seg2Hw.delayUs) seg2Hw.delayUs(seg2Hw.cookie, 5000u);
		}
		if (seg2Hw.delayUs) seg2Hw.delayUs(seg2Hw.cookie, kWakeSettleUs);
	}

	const uint32_t fifoOff = dig_fifo_ctrl0_reg(out.dig);
	const uint32_t vidOff = rdna4modeset::kSeg2Dp0VidStreamCntl + out.dig * kSeg2DigStride;
	if (!is_hazard_seg2(fifoOff)) res.digFifoReg = seg2Hw.rdSeg2(seg2Hw.cookie, fifoOff);
	if (!is_hazard_seg2(vidOff)) res.vidStreamReg = seg2Hw.rdSeg2(seg2Hw.cookie, vidOff);

	res.digFifoError = dig_fifo_has_error(res.digFifoReg);
	res.pixPerCycleMismatch = (res.digFifoReg != 0xffffffffu) &&
	                          (expectedPixPerCycle <= 3u) &&
	                          (dig_fifo_pix_per_cycle(res.digFifoReg) != expectedPixPerCycle);
	res.streamActive = (res.vidStreamReg != 0xffffffffu) &&
	                   ((res.vidStreamReg & 0x00010001u) == 0x00010001u);

	const uint8_t lanes = (expectedLanes >= 1u && expectedLanes <= 4u) ? expectedLanes : 4u;
	if (auxIo.read && auxIo.write &&
	    rdna4aux::dpcdRead(auxIo, out.aux, out.hpd, kDpcdLane01Status, res.laneStatus, 3u)) {
		res.dpcdReadOk = true;
		res.dpcdLinkOk = dpcd_link_ok(res.laneStatus, lanes);
	}

	if (reason == Reason::HpdReconnect) {
		res.needsRetrain = true;
	} else {
		res.needsRetrain = !res.dpcdReadOk || !res.dpcdLinkOk ||
		                   !res.streamActive || res.digFifoError || res.pixPerCycleMismatch;
	}
	return res;
}

}  // namespace rdna4hpd
