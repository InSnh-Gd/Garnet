// dcn41_vbl_irq_pure.h — Pure DCN 4.1 hardware VBLANK IRQ policy, rate-based
// storm guard, and watchdog timer fallback calculations.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
#pragma once

#include <stdint.h>

namespace rdna4vbl {

constexpr uint32_t kStormWindowMaxIrqs = 1000u;         // > 1000 VSTARTUP/s trips storm guard
constexpr uint64_t kStormWindowNs      = 1000000000ULL; // 1 second sliding window
constexpr uint32_t kWatchdogMinUs      = 100000u;       // 100 ms (10 Hz max watchdog cadence)
constexpr uint32_t kWatchdogMaxUs      = 250000u;       // 250 ms upper cap

// Boot-arg `garnet-vbl-irq` (default ON unless explicitly set to 0).
inline bool boot_arg_enabled(bool present, uint32_t value) {
	return !present || value != 0u;
}

struct RateStormGuard {
	uint64_t windowStartNs { 0u };
	uint32_t windowCount { 0u };
	bool     tripped { false };
};

inline void reset_storm(RateStormGuard &g) {
	g.windowStartNs = 0u;
	g.windowCount = 0u;
	g.tripped = false;
}

// Records one hardware VSTARTUP IRQ at monotonic timestamp `nowNs`.
// Returns true once the rate storm limit (> 1000 IRQs in 1 s) has tripped.
inline bool record_irq(RateStormGuard &g, uint64_t nowNs) {
	if (g.tripped) return true;
	if (g.windowStartNs == 0u || nowNs < g.windowStartNs ||
	    (nowNs - g.windowStartNs) >= kStormWindowNs) {
		g.windowStartNs = nowNs;
		g.windowCount = 1u;
		return false;
	}
	g.windowCount++;
	if (g.windowCount > kStormWindowMaxIrqs) {
		g.tripped = true;
		return true;
	}
	return false;
}

// Watchdog cadence when hardware VBLANK IRQ is armed; normal periodUs otherwise.
inline uint32_t initial_timer_us(uint32_t periodUs, bool hwActive) {
	if (!hwActive) return periodUs;
	uint64_t w = static_cast<uint64_t>(periodUs) * 4ULL;
	if (w < kWatchdogMinUs) w = kWatchdogMinUs;
	if (w > kWatchdogMaxUs) w = kWatchdogMaxUs;
	return static_cast<uint32_t>(w);
}

// Suppresses the software timer callback when hardware VBLANK delivered within 2 refresh periods.
inline bool should_suppress_timer(bool hwActive, uint64_t nowNs, uint64_t lastHwNs,
                                  uint32_t periodUs) {
	if (!hwActive || lastHwNs == 0u || nowNs < lastHwNs || periodUs == 0u) return false;
	const uint64_t freshNs = static_cast<uint64_t>(periodUs) * 2000ULL;
	return (nowNs - lastHwNs) < freshNs;
}

// Rearm interval after onVblTimer: stay at watchdog cadence only if hardware VBL suppressed it;
// if hardware VBL stalled and timer had to fire, drop immediately to full periodUs fallback cadence.
inline uint32_t rearm_timer_us(uint32_t periodUs, bool hwActive, bool suppressedByHw) {
	return (hwActive && suppressedByHw) ? initial_timer_us(periodUs, true) : periodUs;
}

}  // namespace rdna4vbl
