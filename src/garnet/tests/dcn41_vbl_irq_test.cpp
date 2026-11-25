#include "dcn/dcn41_vbl_irq_pure.h"

#include <cstdio>

namespace {
int gChecks = 0;
int gFailures = 0;
void expect(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFailures;
		std::printf("FAIL: %s\n", msg);
	}
}
}  // namespace

int main() {
	using namespace rdna4vbl;

	// Boot-arg kill-switch latch: default ON unless explicitly 0.
	expect(boot_arg_enabled(false, 0u), "absent garnet-vbl-irq defaults ON");
	expect(boot_arg_enabled(true, 1u), "garnet-vbl-irq=1 enables HW VBL");
	expect(!boot_arg_enabled(true, 0u), "garnet-vbl-irq=0 disables HW VBL");

	// Watchdog cadence vs fallback period.
	expect(initial_timer_us(16667u, false) == 16667u, "timer mode uses exact 60Hz period");
	expect(initial_timer_us(6250u, false) == 6250u, "timer mode uses exact 160Hz period");
	expect(initial_timer_us(16667u, true) == kWatchdogMinUs, "HW 60Hz clamps watchdog to 100ms");
	expect(initial_timer_us(6250u, true) == kWatchdogMinUs, "HW 160Hz clamps watchdog to 100ms");
	expect(initial_timer_us(33333u, true) == 133332u, "HW 30Hz uses 4x period watchdog");
	expect(initial_timer_us(100000u, true) == kWatchdogMaxUs, "HW watchdog caps at 250ms");

	// Timer suppression while HW VBL is healthy vs fallback when stalled.
	expect(!should_suppress_timer(false, 20000000ULL, 10000000ULL, 16667u),
	       "software mode never suppresses timer");
	expect(!should_suppress_timer(true, 20000000ULL, 0ULL, 16667u),
	       "no HW VBL observed yet does not suppress timer");
	expect(should_suppress_timer(true, 25000000ULL, 10000000ULL, 16667u),
	       "recent HW VBL (< 2 periods) suppresses duplicate timer callback");
	expect(!should_suppress_timer(true, 50000000ULL, 10000000ULL, 16667u),
	       "stale HW VBL (>= 2 periods) allows fallback timer callback");
	expect(!should_suppress_timer(true, 5000000ULL, 10000000ULL, 16667u),
	       "clock regression safely fails open to timer");
	expect(rearm_timer_us(16667u, true, true) == kWatchdogMinUs,
	       "suppressed tick stays on watchdog cadence");
	expect(rearm_timer_us(16667u, true, false) == 16667u,
	       "fallback tick switches immediately to normal refresh cadence");

	// Rate-based storm guard: steady 160 Hz forever never trips; > 1000/s trips cleanly.
	RateStormGuard g {};
	uint64_t t = 1000000000ULL;
	for (uint32_t sec = 0; sec < 5u; ++sec) {
		for (uint32_t f = 0; f < 160u; ++f) {
			expect(!record_irq(g, t), "160 Hz steady stream never trips storm guard");
			t += 6250000ULL;
		}
	}
	expect(!g.tripped, "storm guard stays clear across multi-second 160Hz stream");

	// Burst > 1000 within 1 second trips on the 1001st IRQ.
	reset_storm(g);
	const uint64_t burstBase = 9000000000ULL;
	for (uint32_t i = 0; i < kStormWindowMaxIrqs; ++i) {
		expect(!record_irq(g, burstBase + i * 500000ULL), "1000 IRQs within 1s allowed");
	}
	expect(record_irq(g, burstBase + 600000000ULL), "1001st IRQ within 1s trips storm guard");
	expect(g.tripped, "storm tripped flag latches");
	expect(record_irq(g, burstBase + 5000000000ULL), "tripped storm stays latched until reset");
	reset_storm(g);
	expect(!g.tripped && !record_irq(g, burstBase + 6000000000ULL), "reset clears storm guard");

	std::printf("dcn41_vbl_irq_test: %d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
