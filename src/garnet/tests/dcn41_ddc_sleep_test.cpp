// dcn41_ddc_sleep_test.cpp — Host tests for DCN 4.01 DDC pad switching and S3 sleep/wake state.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dcn41_allow.h"
#include "amd/rdna4_sleep_pure.h"
#include "dcn/dcn41_ddc_pad_pure.h"

namespace {

constexpr uint32_t kSeg2Base = 0x000034c0u;

int gChecks = 0;
int gFails = 0;

void check(bool cond, const char *msg) {
	++gChecks;
	if (!cond) {
		++gFails;
		std::printf("FAIL: %s\n", msg);
	}
}

struct RegOp {
	uint32_t offset;
	uint32_t value;
};

struct MockDdcHw {
	uint32_t regs[0x4000] {};
	std::vector<RegOp> writes;
	std::vector<uint32_t> sleepsMs;

	static uint32_t rd(void *cookie, uint32_t seg2Off) {
		const auto *self = static_cast<const MockDdcHw *>(cookie);
		const uint32_t absDw = kSeg2Base + seg2Off;
		check(!dcn41_is_read_hazard(absDw), "DDC pad read avoids read hazard");
		return (seg2Off < 0x4000u) ? self->regs[seg2Off] : 0xffffffffu;
	}

	static bool wr(void *cookie, uint32_t seg2Off, uint32_t val) {
		auto *self = static_cast<MockDdcHw *>(cookie);
		const uint32_t absDw = kSeg2Base + seg2Off;
		check(dcn41_allow_classify(absDw, 0x20000u, nullptr, nullptr) == DCN41_ALLOW_OK,
		      "DDC pad write within dcn41 allowlist");
		check(!dcn41_is_read_hazard(absDw), "DDC pad write avoids hazard");
		self->writes.push_back(RegOp { seg2Off, val });
		if (seg2Off < 0x4000u) self->regs[seg2Off] = val;
		return true;
	}

	static void sleep_ms(void *cookie, uint32_t ms) {
		static_cast<MockDdcHw *>(cookie)->sleepsMs.push_back(ms);
	}

	rdna4ddcpad::DdcPadHw hw() {
		return rdna4ddcpad::DdcPadHw { this, &MockDdcHw::rd, &MockDdcHw::wr, &MockDdcHw::sleep_ms };
	}

	void clearLog() {
		writes.clear();
		sleepsMs.clear();
	}
};

void test_ddc_pad_modes_all_lines() {
	for (uint8_t line = 0u; line < rdna4ddcpad::kMaxDdcLines; ++line) {
		MockDdcHw mock;
		const uint32_t rMask = rdna4ddcpad::ddc_mask_reg(line);
		// Start with AUX pad mode set and pull-downs off; switch to I2C mode.
		mock.regs[rMask] = rdna4ddcpad::kGpioDdcAuxPadMode;
		mock.regs[rdna4ddcpad::kSeg2GpioAuxCtrl5] = 0u;
		mock.regs[rdna4ddcpad::kSeg2PhyAuxCntl] = 0xffffffffu & ~0x3ffffu;

		check(rdna4ddcpad::set_ddc_pad_mode(mock.hw(), line, true), "switch to I2C succeeds");
		check(mock.sleepsMs.size() == 2u && mock.sleepsMs[0] == 3u && mock.sleepsMs[1] == 2u,
		      "I2C switch sleeps 3ms after PD enable and 2ms before clearing AUX_PAD_MODE");
		check((mock.regs[rMask] & rdna4ddcpad::kGpioDdcPdEn) == rdna4ddcpad::kGpioDdcPdEn,
		      "PD enable bits remain set");
		check((mock.regs[rMask] & rdna4ddcpad::kGpioDdcAuxPadMode) == 0u,
		      "AUX_PAD_MODE cleared for I2C");
		check((mock.regs[rdna4ddcpad::kSeg2GpioAuxCtrl5] & rdna4ddcpad::ddc_i2c_mode_bit(line)) != 0u,
		      "AUX_CTRL_5 I2C mode bit set");
		const uint32_t shift = rdna4ddcpad::aux_rxsel_shift(line);
		check(((mock.regs[rdna4ddcpad::kSeg2PhyAuxCntl] >> shift) & 3u) == 1u,
		      "PHY_AUX_CNTL RXSEL set to 1");

		// Switch back to AUX mode.
		mock.clearLog();
		check(rdna4ddcpad::set_ddc_pad_mode(mock.hw(), line, false), "switch to AUX succeeds");
		check(mock.sleepsMs.empty(), "AUX switch needs no discharge sleeps");
		check((mock.regs[rMask] & rdna4ddcpad::kGpioDdcAuxPadMode) != 0u,
		      "AUX_PAD_MODE restored");
		check((mock.regs[rdna4ddcpad::kSeg2GpioAuxCtrl5] & rdna4ddcpad::ddc_i2c_mode_bit(line)) == 0u,
		      "AUX_CTRL_5 I2C mode bit cleared");
	}

	// Refusal cases: out-of-range line, 0xffffffff readback.
	MockDdcHw badMock;
	badMock.regs[rdna4ddcpad::ddc_mask_reg(0u)] = 0xffffffffu;
	check(!rdna4ddcpad::set_ddc_pad_mode(badMock.hw(), 4u, false), "line >= 4 refused");
	check(!rdna4ddcpad::set_ddc_pad_mode(badMock.hw(), 0u, false), "0xffffffff mask readback refused");
	check(!rdna4ddcpad::set_ddc_pad_mode(rdna4ddcpad::DdcPadHw {}, 0u, false), "null callbacks refused");
}

void test_sleep_state_machine() {
	rdna4sleep::State st;

	// Normal sleep -> abort -> resume.
	auto a1 = st.on_message(rdna4sleep::Message::WillSleep, true, 0u);
	check(st.phase == rdna4sleep::Phase::Sleeping && st.vramMark == rdna4sleep::kVramSentinel,
	      "WillSleep enters Sleeping with sentinel mark");
	check(a1.logPci && a1.writeVramMark && a1.quiesceEngines && !a1.recoverDisplayPads,
	      "WillSleep actions");
	auto aAbort = st.on_message(rdna4sleep::Message::WillNotSleep, false, 0u);
	check(st.phase == rdna4sleep::Phase::Awake && st.vramMark == 0u && st.abortedSleepCount == 1u,
	      "WillNotSleep returns to Awake");
	check(aAbort.resumeEngines && !aAbort.recoverDisplayPads, "WillNotSleep resumes without wake recovery");

	// Full S3 sleep -> wake with VRAM retained.
	(void)st.on_message(rdna4sleep::Message::WillSleep, true, 0u);
	auto aWillPow = st.on_message(rdna4sleep::Message::WillPowerOn, false, 0u);
	check(st.phase == rdna4sleep::Phase::Waking && st.coldWakePending && aWillPow.logPci,
	      "WillPowerOn arms coldWakePending");
	auto aHasPow = st.on_message(rdna4sleep::Message::HasPoweredOn, false, rdna4sleep::kVramSentinel);
	check(st.phase == rdna4sleep::Phase::Awake && !st.coldWakePending && st.wakeCount == 1u,
	      "HasPoweredOn restores Awake");
	check(aHasPow.logPci && aHasPow.checkVramMark && aHasPow.resumeEngines && aHasPow.recoverDisplayPads,
	      "HasPoweredOn requests pad recovery and VRAM verification");
	check(st.vramRetainedCount == 1u && st.vramLostCount == 0u && st.vramMark == 0u,
	      "VRAM retained recorded and mark cleared");

	// Full S3 sleep -> wake with VRAM lost.
	(void)st.on_message(rdna4sleep::Message::WillSleep, true, 0u);
	(void)st.on_message(rdna4sleep::Message::WillPowerOn, false, 0u);
	auto aLost = st.on_message(rdna4sleep::Message::HasPoweredOn, false, 0xffffffffu);
	check(aLost.checkVramMark && st.vramLostCount == 1u && st.vramRetainedCount == 1u,
	      "VRAM loss detected cleanly");
}

} // namespace

int main() {
	test_ddc_pad_modes_all_lines();
	test_sleep_state_machine();
	std::printf("dcn41_ddc_sleep_test: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
