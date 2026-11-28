// dcn41_hpd_test.cpp — Host ASan+UBSan unit tests for DCN 4.0.1 HPD / link-loss recovery,
// DIG_FIFO_CTRL0 error clearing, AUX route restoration, retry/cooldown bounds, DPMS/sleep
// suppression, kill-switch policy, and 0x52ef hard-deny preservation.
#include "dcn/dcn41_hpd_pure.h"
#include "dcn41_allow.h"
#include "dcn41_allow_ranges.h"

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

int gChecks = 0;
int gFail = 0;

void expect(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFail;
		std::printf("FAIL: %s\n", msg);
	}
}

constexpr uint32_t kSeg2 = rdna4hpd::kDefaultDmuSeg2Base; // 0x34c0

struct MockHpdHw {
	std::unordered_map<uint32_t, uint32_t> regs;
	uint8_t dpcd[0x1000] {};
	uint8_t auxTx[32] {};
	uint8_t auxRx[32] {};
	uint32_t auxTxIdx { 0u };
	uint32_t auxRxIdx { 0u };
	uint32_t auxSts { 0u };
	uint32_t auxSw { 0u };
	uint32_t aux { 1u };
	uint32_t dig { 1u };
	uint32_t hpd { 1u };
	uint64_t nowUs { 1000u };
	uint32_t deniedWrites { 0u };
	uint32_t hazardTouches { 0u };
	bool digResetTimeout { false };
	bool digErrorSticky { false };
	bool hpdDestReadOnly { false };
	uint32_t auxFailOnce { 0u };
	std::vector<uint32_t> digFifoWrites;
	struct dcn41_allow_state allow {};

	MockHpdHw() {
		const uint32_t seg[5] = { 0x12u, 0xc0u, kSeg2, 0x9000u, 0x02403c00u };
		const int rc = dcn41_allow_init(&allow, seg, DCN41_ALLOW_BAR5_DWORDS);
		expect(rc == DCN41_ALLOW_OK, "dcn41_allow_init arms cleanly on Navi44/48 DMU bases");
	}

	void initLiveHealthy() {
		const uint32_t ab = rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		const uint32_t db = dig * rdna4hpd::kSeg2DigStride;
		regs[kSeg2 + rdna4hpd::dig_fifo_ctrl0_reg(dig)] = 0x0000011du; // PIX_PER_CYCLE=1, LEVEL=7, EN=1
		regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] = 0x00010201u; // active & enabled
		regs[kSeg2 + rdna4hpd::hpd_status_reg(hpd)] = rdna4hpd::kHpdConnectedMask;
		regs[kSeg2 + ab + rdna4aux::kAuxRegControl] = (hpd << 20) | 1u;
		regs[kSeg2 + rdna4ddcpad::ddc_mask_reg(static_cast<uint8_t>(aux))] = rdna4ddcpad::kGpioDdcAuxPadMode;
		regs[kSeg2 + rdna4ddcpad::kSeg2GpioAuxCtrl5] = 0u;
		dpcd[0x0202] = 0x77u; // lanes 0,1 CR+EQ+SYM
		dpcd[0x0203] = 0x77u; // lanes 2,3 CR+EQ+SYM
		dpcd[0x0204] = 0x01u; // INTERLANE_ALIGN_DONE
		dpcd[rdna4hpd::kDpcdSetPowerState] = rdna4hpd::kDpPowerStateD0;
	}

	void handleAuxGo() {
		if (auxFailOnce > 0u) {
			--auxFailOnce;
			auxSts = 0x00000011u; // done + RX timeout
			return;
		}
		if (auxTxIdx < 4u) {
			auxSts = 0x00000001u;
			return;
		}
		const uint8_t cmd = static_cast<uint8_t>(auxTx[0] >> 4);
		const uint32_t addr = (static_cast<uint32_t>(auxTx[0] & 0x0fu) << 16) |
		                      (static_cast<uint32_t>(auxTx[1]) << 8) | auxTx[2];
		const uint32_t len = static_cast<uint32_t>(auxTx[3]) + 1u;
		auxRxIdx = 0u;
		if (cmd == 0x8u) { // native AUX write
			for (uint32_t i = 0u; i < len && (4u + i) < auxTxIdx; ++i) {
				if (addr + i < sizeof(dpcd)) dpcd[addr + i] = auxTx[4u + i];
			}
			auxRx[0] = 0x00u; // ACK
			auxSts = 0x00000001u | (1u << 24);
		} else if (cmd == 0x9u) { // native AUX read
			auxRx[0] = 0x00u; // ACK
			for (uint32_t i = 0u; i < len && (1u + i) < sizeof(auxRx); ++i) {
				auxRx[1u + i] = (addr + i < sizeof(dpcd)) ? dpcd[addr + i] : 0u;
			}
			auxSts = 0x00000001u | ((1u + len) << 24);
		} else {
			auxRx[0] = 0x00u;
			auxSts = 0x00000001u | (1u << 24);
		}
	}

	uint32_t rdAbs(uint32_t absDw) {
		if (dcn41_is_read_hazard(absDw)) {
			++hazardTouches;
			return 0xffffffffu;
		}
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		if (absDw == ab + rdna4aux::kAuxRegSwStat) return auxSts;
		if (absDw == ab + rdna4aux::kAuxRegSwCntl) return auxSw & ~1u;
		if (absDw == ab + rdna4aux::kAuxRegSwData) {
			const uint8_t b = (auxRxIdx < sizeof(auxRx)) ? auxRx[auxRxIdx++] : 0u;
			return static_cast<uint32_t>(b) << 8;
		}
		auto it = regs.find(absDw);
		return (it != regs.end()) ? it->second : 0u;
	}

	bool wrAbs(uint32_t absDw, uint32_t val) {
		if (dcn41_is_read_hazard(absDw)) {
			++hazardTouches;
		}
		if (!dcn41_allow_write(&allow, absDw, val, "hpdtest")) {
			++deniedWrites;
			return false;
		}
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		if (absDw == ab + rdna4aux::kAuxRegSwData) {
			if (val & 1u) {
				auxRxIdx = 0u;
			} else {
				if (val & 0x80000000u) auxTxIdx = 0u;
				if (auxTxIdx < sizeof(auxTx)) {
					auxTx[auxTxIdx++] = static_cast<uint8_t>((val >> 8) & 0xffu);
				}
			}
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegSwCntl) {
			auxSw = val;
			if (val & 1u) handleAuxGo();
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegIntAck) {
			if (val & 2u) auxSts &= ~1u;
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegArb) {
			if (val & 0x10000u) regs[absDw] = 0x00010004u;
			else if (val & 0x20000u) regs[absDw] = 0u;
			return true;
		}
		const uint32_t fifoAbs = kSeg2 + rdna4hpd::dig_fifo_ctrl0_reg(dig);
		if (absDw == fifoAbs) {
			digFifoWrites.push_back(val);
			// Verify hardware invariant: ERROR [29:28] and RESET_DONE [20] are never written 1.
			expect((val & rdna4hpd::kDigFifoNeverWriteMask) == 0u,
			       "DIG_FIFO_CTRL0 write never sets RO bits 29:28 or 20");
			uint32_t next = val;
			if (val & rdna4hpd::kDigFifoResetMask) {
				if (!digResetTimeout) next |= rdna4hpd::kDigFifoResetDoneMask;
			} else {
				next &= ~rdna4hpd::kDigFifoResetDoneMask;
			}
			if (digErrorSticky) next |= rdna4hpd::kDigFifoError1Mask;
			regs[absDw] = next;
			return true;
		}
		if (absDw == kSeg2 + rdna4hpd::kSeg2HpdInterruptDest && hpdDestReadOnly) {
			return true; // Simulate PSP/DMCUB write-protected regHPD_INTERRUPT_DEST
		}
		regs[absDw] = val;
		return true;
	}

	rdna4hpd::Seg2Hw makeSeg2Hw() {
		return rdna4hpd::Seg2Hw {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockHpdHw *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockHpdHw *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c, uint32_t us) { static_cast<MockHpdHw *>(c)->nowUs += us; },
		};
	}

	rdna4ddcpad::DdcPadHw makePadHw() {
		return rdna4ddcpad::DdcPadHw {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockHpdHw *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockHpdHw *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c, uint32_t ms) { static_cast<MockHpdHw *>(c)->nowUs += static_cast<uint64_t>(ms) * 1000ULL; },
		};
	}

	rdna4aux::Io makeAuxIo() {
		return rdna4aux::Io {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockHpdHw *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockHpdHw *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c) -> uint64_t { return static_cast<MockHpdHw *>(c)->nowUs; },
			[](void *c, uint32_t us) { static_cast<MockHpdHw *>(c)->nowUs += us; },
		};
	}
};

void test_kill_switch_and_hazard() {
	using namespace rdna4hpd;

	expect(boot_arg_enabled(false, 0u), "absent garnet-hpd-recover defaults ON");
	expect(boot_arg_enabled(true, 1u), "garnet-hpd-recover=1 enables HPD recovery");
	expect(!boot_arg_enabled(true, 0u), "garnet-hpd-recover=0 disables HPD recovery");

	// Hard hardware safety invariant: 0x52ef MUST remain hard-denied in dcn41_allow!
	const char *why = nullptr;
	const int cls = dcn41_allow_classify(kHazardAbsDword, DCN41_ALLOW_BAR5_DWORDS, nullptr, &why);
	expect(cls == DCN41_ALLOW_INDIRECT, "0x52ef remains hard-denied (DCN41_ALLOW_INDIRECT)");
	expect(dcn41_is_read_hazard(kHazardAbsDword), "dcn41_is_read_hazard flags 0x52ef");
	expect(is_hazard_abs(0x000052efu) && is_hazard_seg2(0x1e2fu), "pure hazard predicates flag 0x52ef");

	struct dcn41_allow_state st {};
	const uint32_t seg[5] = { 0x12u, 0xc0u, kSeg2, 0x9000u, 0x02403c00u };
	expect(dcn41_allow_init(&st, seg, DCN41_ALLOW_BAR5_DWORDS) == DCN41_ALLOW_OK, "allowlist arms");
	expect(!dcn41_allow_write(&st, kHazardAbsDword, 0u, "hazard"), "dcn41_allow_write refuses 0x52ef");
	expect(dcn41_allow_write(&st, kSeg2 + dig_fifo_ctrl0_reg(1u), 0x11du, "fifo"),
	       "DIG1_DIG_FIFO_CTRL0 (0x567f) is allowlisted");
	for (uint32_t h = 0u; h < kMaxHpdPins; ++h) {
		expect(dcn41_allow_classify(kSeg2 + hpd_control_reg(h), DCN41_ALLOW_BAR5_DWORDS, nullptr, nullptr) == DCN41_ALLOW_OK,
		       "DC_HPD_INT_CONTROL is allowlisted for HPD0..3");
	}

	// Disabled state machine ignores all events.
	StateMachine sm;
	sm.init(false);
	auto a1 = sm.onHpdIrq(false, 100u);
	auto a2 = sm.onHpdIrq(true, 200u);
	auto a3 = sm.onHpdRxShortPulse(true, 300u);
	auto a4 = sm.onLinkFaultDetected(true, 400u);
	auto a5 = sm.onTimer(1000u);
	expect(!a1.runRecovery && !a2.runRecovery && !a3.runRecovery && !a4.runRecovery && !a5.runRecovery,
	       "garnet-hpd-recover=0 never schedules or triggers recovery");
	expect(a2.scheduleTimerMs == 0u && a3.scheduleTimerMs == 0u && a4.scheduleTimerMs == 0u,
	       "garnet-hpd-recover=0 never arms debounce timer");
}

void test_debounce_coalescing_and_long_replug() {
	using namespace rdna4hpd;

	StateMachine sm;
	sm.init(true);

	// 1. Monitor input switch away (HPD low) -> Disconnected.
	auto a = sm.onHpdIrq(false, 1000u);
	expect(sm.state == State::Disconnected && a.cancelTimer && !a.runRecovery,
	       "HPD low transitions Connected -> Disconnected and cancels timer");

	// 2. Rapid monitor switch-back bounces coalesce into ONE debounced recovery.
	a = sm.onHpdIrq(true, 5000u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "first HPD high enters Debouncing with 150ms quiet window");
	a = sm.onHpdIrq(false, 5040u);
	expect(sm.state == State::Disconnected && a.cancelTimer,
	       "bounce low cancels debounce cleanly");
	a = sm.onHpdIrq(true, 5060u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "second HPD high restarts 150ms debounce");
	a = sm.onHpdIrq(true, 5120u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "third HPD high extends quiet window to 5270ms");

	// Early timer tick before quiet window expires must NOT run recovery yet.
	a = sm.onTimer(5210u);
	expect(!a.runRecovery && a.scheduleTimerMs == 60u && sm.state == State::Debouncing,
	       "early timer tick waits remaining 60ms of debounce window");

	// Quiet window expires at 5270ms -> Recovering.
	a = sm.onTimer(5270u);
	expect(a.runRecovery && a.reason == Reason::HpdReconnect && sm.state == State::Recovering,
	       "quiet window expiration triggers single HpdReconnect recovery");
	expect(sm.attemptsTotal == 1u, "only 1 recovery attempt issued for all bounces");

	// Recovery succeeds -> Connected.
	a = sm.onRecoveryResult(true, true, 5600u);
	expect(sm.state == State::Connected && a.cancelTimer && sm.recoveriesSucceeded == 1u && sm.retryCount == 0u,
	       "successful recovery returns cleanly to Connected");
}

void test_short_pulse_and_retrigger_mid_recovery() {
	using namespace rdna4hpd;

	StateMachine sm;
	sm.init(true);

	auto a = sm.onHpdRxShortPulse(true, 1000u);
	expect(sm.state == State::Debouncing && a.reason == Reason::HpdRxShortPulse && a.scheduleTimerMs == kDefaultDebounceMs,
	       "HPD_RX short pulse schedules debounced HpdRxShortPulse check");
	a = sm.onTimer(1150u);
	expect(a.runRecovery && a.reason == Reason::HpdRxShortPulse && sm.state == State::Recovering,
	       "HPD_RX short pulse transitions to Recovering");

	// Another HPD edge arrives mid-recovery -> queued as single debounced retrigger after completion.
	auto mid = sm.onHpdIrq(true, 1200u);
	expect(!mid.runRecovery && mid.scheduleTimerMs == 0u && sm.state == State::Recovering && sm.retriggerPending,
	       "mid-recovery HPD edge does not overlap recovery; marks retriggerPending");
	a = sm.onRecoveryResult(true, true, 1400u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs && a.reason == Reason::HpdReconnect,
	       "post-recovery retrigger schedules one debounced reconnect");

	// Cable unplugged mid-recovery -> drops straight to Disconnected with no retry.
	a = sm.onTimer(1550u);
	expect(a.runRecovery && sm.state == State::Recovering, "retriggered recovery starts");
	a = sm.onRecoveryResult(false, false, 1700u);
	expect(sm.state == State::Disconnected && a.cancelTimer && sm.retryCount == 0u,
	       "unplug during recovery enters Disconnected without burning retries");
}

void test_retry_ceiling_exponential_backoff_and_cooldown() {
	using namespace rdna4hpd;

	StateMachine sm;
	sm.init(true);

	(void)sm.onHpdIrq(false, 100u);
	(void)sm.onHpdIrq(true, 200u);

	// Attempt 1 fails -> backoff 500ms.
	auto a = sm.onTimer(350u);
	expect(a.runRecovery && sm.state == State::Recovering, "attempt 1 starts");
	a = sm.onRecoveryResult(false, true, 500u);
	expect(sm.state == State::Debouncing && sm.retryCount == 1u && a.scheduleTimerMs == 500u,
	       "failure 1 schedules 500ms backoff");

	// Attempt 2 fails -> backoff 1000ms.
	a = sm.onTimer(1000u);
	expect(a.runRecovery && sm.state == State::Recovering, "attempt 2 starts after 500ms");
	a = sm.onRecoveryResult(false, true, 1200u);
	expect(sm.state == State::Debouncing && sm.retryCount == 2u && a.scheduleTimerMs == 1000u,
	       "failure 2 schedules 1000ms exponential backoff");

	// Attempt 3 fails -> retry ceiling (3) reached -> Cooldown for 15000ms!
	a = sm.onTimer(2200u);
	expect(a.runRecovery && sm.state == State::Recovering, "attempt 3 starts after 1000ms");
	a = sm.onRecoveryResult(false, true, 2400u);
	expect(sm.state == State::Cooldown && sm.retryCount == 3u && sm.cooldownTrips == 1u && a.cancelTimer,
	       "3rd consecutive failure enters Cooldown and stops retrying");

	// Events during cooldown are suppressed (no infinite loop / storm).
	a = sm.onHpdRxShortPulse(true, 5000u);
	expect(sm.state == State::Cooldown && !a.runRecovery && a.scheduleTimerMs == 0u,
	       "HPD_RX during cooldown is suppressed");
	a = sm.onLinkFaultDetected(true, 10000u);
	expect(sm.state == State::Cooldown && !a.runRecovery && a.scheduleTimerMs == 0u,
	       "link fault during cooldown is suppressed");

	// Once cooldown expires (2400 + 15000 = 17400ms), new event can recover again.
	a = sm.onHpdRxShortPulse(true, 17400u);
	expect(sm.state == State::Debouncing && sm.retryCount == 0u && a.scheduleTimerMs == kDefaultDebounceMs,
	       "cooldown expiration allows fresh recovery");

	// Fresh physical unplug resets cooldown immediately (user swapping bad cable).
	(void)sm.onTimer(17550u);
	(void)sm.onRecoveryResult(false, true, 17600u);
	(void)sm.onTimer(18100u);
	(void)sm.onRecoveryResult(false, true, 18200u);
	(void)sm.onTimer(19200u);
	(void)sm.onRecoveryResult(false, true, 19300u);
	expect(sm.state == State::Cooldown, "re-entered Cooldown after 3 failures");
	a = sm.onHpdIrq(false, 20000u);
	expect(sm.state == State::Disconnected && sm.retryCount == 0u,
	       "physical unplug clears Cooldown for fresh replug");
	a = sm.onHpdIrq(true, 20500u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "replug after unplug recovers immediately without waiting out old cooldown");
}

void test_dpms_and_sleep_suppression() {
	using namespace rdna4hpd;

	StateMachine sm;
	sm.init(true);

	// Enter DPMS-off while debouncing -> cancels timer and suppresses HPD/HPD_RX.
	(void)sm.onHpdIrq(true, 1000u);
	expect(sm.state == State::Debouncing, "started debouncing");
	auto a = sm.setDpmsOff(true, 1050u);
	expect(a.cancelTimer && sm.state == State::Connected, "DPMS-off cancels pending debounce");
	a = sm.onHpdIrq(false, 1100u);
	expect(sm.state == State::Disconnected, "DPMS-off still tracks disconnect passively");
	a = sm.onHpdIrq(true, 1200u);
	expect(!a.runRecovery && a.scheduleTimerMs == 0u && sm.state != State::Debouncing,
	       "DPMS-off suppresses HPD reconnect recovery");
	a = sm.onHpdRxShortPulse(true, 1300u);
	expect(!a.runRecovery && a.scheduleTimerMs == 0u, "DPMS-off suppresses HPD_RX recovery");
	a = sm.onTimer(2000u);
	expect(!a.runRecovery && a.cancelTimer, "timer firing during DPMS-off never runs recovery");

	// DPMS-on clears suppression cleanly.
	(void)sm.setDpmsOff(false, 3000u);
	a = sm.onHpdRxShortPulse(true, 3100u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "DPMS-on re-enables HPD recovery");

	// System sleep suppresses recovery identically.
	a = sm.setSystemSleeping(true, 3150u);
	expect(a.cancelTimer, "system sleep cancels pending debounce");
	a = sm.onHpdIrq(true, 3200u);
	expect(!a.runRecovery && a.scheduleTimerMs == 0u, "system sleep suppresses HPD recovery");
	(void)sm.setSystemSleeping(false, 4000u);
	a = sm.onHpdIrq(true, 4100u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs,
	       "system wake re-enables HPD recovery");
}

void test_dig_fifo_clear_and_aux_probe() {
	using namespace rdna4hpd;

	// Verify exact hardware-observed DIG1_DIG_FIFO_CTRL0 values from deferred #2:
	// Boot clean: 0x0000011d (PIXEL_PER_CYCLE=1, LEVEL=7, EN=1, ERROR=0)
	// Post-switch fault: 0x2000001d (bit 29 DIG_FIFO_ERROR set, PIXEL_PER_CYCLE=0!)
	expect(!dig_fifo_has_error(0x0000011du) && dig_fifo_pix_per_cycle(0x0000011du) == 1u,
	       "0x0000011d decoded as clean with PIXEL_PER_CYCLE=1");
	expect(dig_fifo_has_error(0x2000001du) && dig_fifo_pix_per_cycle(0x2000001du) == 0u,
	       "0x2000001d decoded as DIG_FIFO_ERROR set with PIXEL_PER_CYCLE=0");
	expect(dig_fifo_expected_pix_per_cycle(1485000000ULL, 1000000000u) == 1u,
	       "4K@160Hz pclk (1.485 GHz) over 1.0 GHz DPREFCLK expects PIXEL_PER_CYCLE=1");
	expect(dig_fifo_expected_pix_per_cycle(594000000ULL, 1000000000u) == 0u,
	       "4K@60Hz pclk (594 MHz) over 1.0 GHz DPREFCLK expects PIXEL_PER_CYCLE=0");
	expect((dig_fifo_clean_base(0x2000001du, 1u) & kDigFifoNeverWriteMask) == 0u &&
	       dig_fifo_clean_base(0x2000001du, 1u) == 0x0000011cu,
	       "dig_fifo_clean_base clears error/RO bits and restores LEVEL=7 + PIXEL_PER_CYCLE=1");

	MockHpdHw mock;
	mock.initLiveHealthy();
	mock.regs[kSeg2 + dig_fifo_ctrl0_reg(1u)] = 0x2000001du; // observed fault state

	const auto cleared = clear_dig_fifo_error(mock.makeSeg2Hw(), 1u, 1u);
	expect(cleared.ok && cleared.resetDoneSeen && cleared.resetClearSeen,
	       "clear_dig_fifo_error resets DIG1 FIFO, clears bit 29, and restores PIXEL_PER_CYCLE=1");
	expect(cleared.before == 0x2000001du && cleared.after == 0x0000011du,
	       "DIG1_DIG_FIFO_CTRL0 transitions 0x2000001d -> 0x0000011d");
	expect(mock.deniedWrites == 0u && mock.hazardTouches == 0u,
	       "all DIG_FIFO_CTRL0 clear writes pass allowlist with zero hazard touches");

	// Even if RESET_DONE times out, RESET bit MUST be deasserted (never leave FIFO wedged in reset).
	mock.digFifoWrites.clear();
	mock.digResetTimeout = true;
	mock.digErrorSticky = true;
	mock.regs[kSeg2 + dig_fifo_ctrl0_reg(1u)] = 0x2000001du;
	const auto stuck = clear_dig_fifo_error(mock.makeSeg2Hw(), 1u, 1u);
	expect(!stuck.ok && !stuck.resetDoneSeen, "sticky FIFO error accurately reports failure");
	expect(mock.digFifoWrites.size() == 3u &&
	       (mock.digFifoWrites[0] & kDigFifoResetMask) != 0u &&
	       (mock.digFifoWrites[1] & kDigFifoResetMask) == 0u &&
	       (mock.digFifoWrites[2] & kDigFifoEnableMask) != 0u,
	       "timeout path still deasserts DIG_FIFO_RESET and re-enables DIG_FIFO_ENABLE");

	// Full probe sequence tests:
	// Case A: Benign HPD_RX short pulse with healthy link + clean FIFO -> needsRetrain == false (no flicker!).
	MockHpdHw healthy;
	healthy.initLiveHealthy();
	rdna4aux::ActiveOutput out {};
	out.found = true; out.isDp = true; out.otg = 0u; out.dig = 1u; out.link = 1u;
	out.hpd = 1u; out.aux = 1u; out.auxValid = true;
	// Corrupt pad mode to I2C and clear AUX_EN to verify AUX route restoration!
	healthy.regs[kSeg2 + rdna4ddcpad::ddc_mask_reg(1u)] = 0u;
	healthy.regs[kSeg2 + rdna4ddcpad::kSeg2GpioAuxCtrl5] = rdna4ddcpad::ddc_i2c_mode_bit(1u);
	healthy.regs[kSeg2 + rdna4aux::auxReg(1u, rdna4aux::kAuxRegControl)] = 0u;

	auto pHealthy = prepare_recovery_aux_and_probe(healthy.makeSeg2Hw(), healthy.makePadHw(),
	                                               healthy.makeAuxIo(), out, 4u, 1u,
	                                               Reason::HpdRxShortPulse);
	expect(pHealthy.auxPadRestored && pHealthy.auxCtlOk && pHealthy.hpdConnected,
	       "probe restores DDC AUX pad mode and DP_AUX_CONTROL routing");
	expect(pHealthy.dpcdReadOk && pHealthy.dpcdLinkOk && pHealthy.streamActive &&
	       !pHealthy.digFifoError && !pHealthy.pixPerCycleMismatch && !pHealthy.needsRetrain,
	       "benign HPD_RX short pulse with healthy link does not trigger unnecessary retrain");
	expect(healthy.deniedWrites == 0u && healthy.hazardTouches == 0u,
	       "healthy probe stays strictly inside dcn41 allowlist");

	// Case B: HPD_RX short pulse with DPCD CR/EQ loss -> needsRetrain == true.
	healthy.dpcd[0x0202] = 0x11u; // lost EQ/SYM lock on lanes 0..1
	auto pLoss = prepare_recovery_aux_and_probe(healthy.makeSeg2Hw(), healthy.makePadHw(),
	                                            healthy.makeAuxIo(), out, 4u, 1u,
	                                            Reason::HpdRxShortPulse);
	expect(pLoss.dpcdReadOk && !pLoss.dpcdLinkOk && pLoss.needsRetrain,
	       "HPD_RX short pulse with DPCD lane loss triggers retrain");

	// Case C: HPD_RX short pulse with DIG_FIFO_CTRL0 bit 29 set (even if DPCD looks ok) -> needsRetrain == true.
	healthy.dpcd[0x0202] = 0x77u;
	healthy.regs[kSeg2 + dig_fifo_ctrl0_reg(1u)] = 0x2000001du;
	auto pFifo = prepare_recovery_aux_and_probe(healthy.makeSeg2Hw(), healthy.makePadHw(),
	                                            healthy.makeAuxIo(), out, 4u, 1u,
	                                            Reason::HpdRxShortPulse);
	expect(pFifo.digFifoError && pFifo.pixPerCycleMismatch && pFifo.needsRetrain,
	       "DIG_FIFO_CTRL0 error or PIXEL_PER_CYCLE mismatch triggers retrain");

	// Case D: Full HPD reconnect wakes sink D0 (retrying transient AUX timeout) and ALWAYS requires retrain.
	healthy.initLiveHealthy();
	healthy.dpcd[kDpcdSetPowerState] = 0x02u; // sink in D3
	healthy.auxFailOnce = 1u;
	auto pReconn = prepare_recovery_aux_and_probe(healthy.makeSeg2Hw(), healthy.makePadHw(),
	                                              healthy.makeAuxIo(), out, 4u, 1u,
	                                              Reason::HpdReconnect);
	expect(pReconn.hpdConnected && pReconn.sinkD0Written && pReconn.needsRetrain &&
	       healthy.dpcd[kDpcdSetPowerState] == kDpPowerStateD0,
	       "HpdReconnect wakes sink D0 with retry and requests full retrain");

	// Case E: Disconnected HPD pin aborts cleanly without touching DPCD.
	healthy.regs[kSeg2 + hpd_status_reg(1u)] = 0u;
	auto pDisc = prepare_recovery_aux_and_probe(healthy.makeSeg2Hw(), healthy.makePadHw(),
	                                            healthy.makeAuxIo(), out, 4u, 1u,
	                                            Reason::HpdReconnect);
	expect(!pDisc.hpdConnected && !pDisc.needsRetrain,
	       "disconnected HPD pin aborts probe cleanly");
}

void test_hpd_irq_storm_guard() {
	using namespace rdna4hpd;

	HpdStormGuard g {};
	const uint64_t t0 = 5000000000ULL;
	for (uint32_t i = 0u; i < kHpdStormMaxIrqs; ++i) {
		expect(!record_hpd_irq(g, t0 + static_cast<uint64_t>(i) * 1000000ULL),
		       "HPD IRQs up to 64/s allowed");
	}
	expect(record_hpd_irq(g, t0 + 700000000ULL), "65th HPD IRQ within 1s trips storm guard");
	expect(g.tripped && record_hpd_irq(g, t0 + 9000000000ULL),
	       "tripped HPD storm guard stays latched until explicit reset");
	reset_hpd_storm(g);
	expect(!g.tripped && !record_hpd_irq(g, t0 + 9000000000ULL),
	       "reset clears HPD storm guard");
}

void test_hpd_interrupt_dest_routing() {
	using namespace rdna4hpd;

	expect(hpd_interrupt_dest_mask(0u) == 0x00000101u, "HPD0 dest mask covers HPD1 + HPD1_RX");
	expect(hpd_interrupt_dest_mask(1u) == 0x00000202u, "HPD1 dest mask matches live UEFI GOP 0x0202");
	expect(hpd_interrupt_dest_mask(3u) == 0x00000808u, "HPD3 dest mask covers HPD4 + HPD4_RX");
	expect(hpd_interrupt_dest_mask(4u) == 0u, "out-of-range HPD pin returns 0 mask");

	MockHpdHw mock;
	mock.regs[kSeg2 + kSeg2HpdInterruptDest] = 0x00000202u; // Exact UEFI GOP state on RX 9060 XT
	const auto r1 = route_hpd_to_host(mock.makeSeg2Hw(), 1u);
	expect(r1.ok && r1.changed && r1.hostRouted && r1.before == 0x00000202u && r1.after == 0x00000000u,
	       "route_hpd_to_host clears HPD1/HPD1_RX from 0x0202 -> 0x0000");
	expect(mock.deniedWrites == 0u && mock.hazardTouches == 0u,
	       "regHPD_INTERRUPT_DEST (0x3621) passes allowlist with zero hazard touches");

	// Already host-routed is a read-only no-op.
	const auto rNoop = route_hpd_to_host(mock.makeSeg2Hw(), 1u);
	expect(rNoop.ok && !rNoop.changed && rNoop.hostRouted && rNoop.after == 0u,
	       "already host-routed regHPD_INTERRUPT_DEST needs no write");

	// Clearing HPD1 preserves unrelated HPD pins.
	mock.regs[kSeg2 + kSeg2HpdInterruptDest] = 0x00000707u;
	const auto rPreserve = route_hpd_to_host(mock.makeSeg2Hw(), 1u);
	expect(rPreserve.ok && rPreserve.changed && rPreserve.hostRouted && rPreserve.after == 0x00000505u,
	       "clearing HPD1 preserves HPD0 and HPD2 bits");

	// Read-only / security-protected register reports hostRouted == false cleanly.
	mock.regs[kSeg2 + kSeg2HpdInterruptDest] = 0x00000202u;
	mock.hpdDestReadOnly = true;
	const auto rRo = route_hpd_to_host(mock.makeSeg2Hw(), 1u);
	expect(rRo.ok && !rRo.changed && !rRo.hostRouted && rRo.after == 0x00000202u,
	       "write-protected regHPD_INTERRUPT_DEST reports hostRouted=false cleanly");
}

void test_watchdog_tick_and_linkloss_d0() {
	using namespace rdna4hpd;

	StateMachine sm;
	sm.init(true);
	expect(sm.shouldRunWatchdog(), "Connected active state enables watchdog");

	// Healthy tick keeps polling every 1000 ms with no recovery.
	auto a = sm.onWatchdogTick(true, false, false, 1000u);
	expect(!a.runRecovery && a.scheduleTimerMs == kDefaultWatchdogMs && a.reason == Reason::None,
	       "healthy watchdog tick reschedules 1000 ms without recovery");

	// Missed disconnect edge -> Disconnected while continuing 1000 ms poll.
	a = sm.onWatchdogTick(false, false, true, 2000u);
	expect(sm.state == State::Disconnected && a.scheduleTimerMs == kDefaultWatchdogMs && a.reason == Reason::None,
	       "watchdog disconnect transitions to Disconnected and ignores linkFault while unplugged");

	// Missed reconnect edge -> schedules 150 ms HpdReconnect recovery!
	a = sm.onWatchdogTick(true, false, false, 3000u);
	expect(sm.state == State::Debouncing && a.scheduleTimerMs == kDefaultDebounceMs && a.reason == Reason::HpdReconnect,
	       "watchdog missed reconnect schedules debounced HpdReconnect");
	expect(!sm.shouldRunWatchdog(), "Debouncing suspends watchdog until recovery finishes");
	a = sm.onTimer(3150u);
	expect(a.runRecovery && a.reason == Reason::HpdReconnect && !sm.shouldRunWatchdog(),
	       "Recovering suspends watchdog");
	(void)sm.onRecoveryResult(true, true, 3400u);
	expect(sm.state == State::Connected && sm.shouldRunWatchdog(), "recovery completion re-enables watchdog");

	// Connected link fault (e.g. DIG_FIFO_CTRL0 0x2000001d) -> debounced Reason::LinkLoss.
	a = sm.onWatchdogTick(true, false, true, 4000u);
	expect(sm.state == State::Debouncing && a.reason == Reason::LinkLoss && a.scheduleTimerMs == kDefaultDebounceMs,
	       "watchdog link fault triggers debounced LinkLoss");
	(void)sm.onTimer(4150u);
	(void)sm.onRecoveryResult(true, true, 4400u);

	// Latched HPD status without IH -> non-disruptive HpdRxShortPulse probe.
	a = sm.onWatchdogTick(true, true, false, 5000u);
	expect(sm.state == State::Debouncing && a.reason == Reason::HpdRxShortPulse && a.scheduleTimerMs == kDefaultDebounceMs,
	       "watchdog latched HPD status triggers HpdRxShortPulse probe");
	(void)sm.onTimer(5150u);

	// Drive to Cooldown (3 consecutive failures) and verify watchdog respects cooldown.
	(void)sm.onRecoveryResult(false, true, 5200u);
	(void)sm.onTimer(5700u);
	(void)sm.onRecoveryResult(false, true, 5800u);
	(void)sm.onTimer(6800u);
	(void)sm.onRecoveryResult(false, true, 6900u);
	expect(sm.state == State::Cooldown && sm.shouldRunWatchdog(), "Cooldown keeps watchdog alive for expiry/unplug");
	a = sm.onWatchdogTick(true, false, true, 10000u);
	expect(sm.state == State::Cooldown && a.reason == Reason::None && a.scheduleTimerMs == kDefaultWatchdogMs,
	       "watchdog suppresses linkFault recovery during cooldown");
	// Cooldown expires at 6900 + 15000 = 21900 ms -> watchdog immediately catches active linkFault.
	a = sm.onWatchdogTick(true, false, true, 21900u);
	expect(sm.state == State::Debouncing && a.reason == Reason::LinkLoss && sm.retryCount == 0u,
	       "watchdog resumes recovery once cooldown expires");
	// DPMS-off suppresses watchdog and cancels timer even from Connected.
	(void)sm.onTimer(22050u);
	(void)sm.onRecoveryResult(true, true, 22200u);
	a = sm.setDpmsOff(true, 23000u);
	expect(a.cancelTimer && !sm.shouldRunWatchdog(), "DPMS-off cancels watchdog from Connected state");
	a = sm.onWatchdogTick(true, true, true, 24000u);
	expect(a.cancelTimer && a.scheduleTimerMs == 0u && a.reason == Reason::None,
	       "suppressed watchdog tick never triggers recovery");

	// Reason::LinkLoss wakes sink D0 (DPCD 0x00600 = 0x01) just like HpdReconnect.
	MockHpdHw mock;
	mock.initLiveHealthy();
	mock.dpcd[kDpcdSetPowerState] = 0x02u; // sink in D3
	mock.regs[kSeg2 + dig_fifo_ctrl0_reg(1u)] = 0x2000001du;
	rdna4aux::ActiveOutput out {};
	out.found = true; out.isDp = true; out.otg = 0u; out.dig = 1u; out.link = 1u;
	out.hpd = 1u; out.aux = 1u; out.auxValid = true;
	const auto pLink = prepare_recovery_aux_and_probe(mock.makeSeg2Hw(), mock.makePadHw(),
	                                                  mock.makeAuxIo(), out, 4u, 1u,
	                                                  Reason::LinkLoss);
	expect(pLink.sinkD0Written && mock.dpcd[kDpcdSetPowerState] == kDpPowerStateD0 &&
	       pLink.digFifoError && pLink.pixPerCycleMismatch && pLink.needsRetrain,
	       "Reason::LinkLoss wakes sink D0 and flags retrain on 0x2000001d");
}

}  // namespace

int main() {
	test_kill_switch_and_hazard();
	test_debounce_coalescing_and_long_replug();
	test_short_pulse_and_retrigger_mid_recovery();
	test_retry_ceiling_exponential_backoff_and_cooldown();
	test_dpms_and_sleep_suppression();
	test_dig_fifo_clear_and_aux_probe();
	test_hpd_irq_storm_guard();
	test_hpd_interrupt_dest_routing();
	test_watchdog_tick_and_linkloss_d0();
	std::printf("dcn41_hpd_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
