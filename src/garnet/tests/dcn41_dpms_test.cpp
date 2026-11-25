#include "dcn/dcn41_dpms_pure.h"

#include "dcn41_allow_ranges.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

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

constexpr uint32_t kSeg1 = 0x00c0u;
constexpr uint32_t kSeg2 = 0x34c0u;

bool absAllowed(uint32_t absDw) {
	for (uint32_t i = 0; i < DCN41_HARD_DENY_COUNT; ++i)
		if (absDw >= dcn41_hard_deny[i].abs_lo && absDw <= dcn41_hard_deny[i].abs_hi) return false;
	for (uint32_t i = 0; i < DCN41_DENY_BLOCK_COUNT; ++i)
		if (absDw >= dcn41_deny_blocks[i].abs_lo && absDw <= dcn41_deny_blocks[i].abs_hi) return false;
	for (uint32_t i = 0; i < DCN41_ALLOW_RANGE_COUNT; ++i)
		if (absDw >= dcn41_allow_ranges[i].abs_lo && absDw <= dcn41_allow_ranges[i].abs_hi) return true;
	return false;
}

struct MockDpms {
	std::unordered_map<uint32_t, uint32_t> regs;
	uint8_t dpcd[0x1000] {};
	uint8_t auxTx[32] {};
	uint8_t auxRx[32] {};
	uint32_t auxTxIdx = 0u;
	uint32_t auxRxIdx = 0u;
	uint32_t auxSts = 0u;
	uint32_t auxSw = 0u;
	uint32_t aux = 1u;
	uint32_t dig = 1u;
	uint32_t otg = 0u;
	uint64_t nowUs = 1000u;
	uint32_t deniedWrites = 0u;
	uint32_t phyDisableCalls = 0u;
	uint32_t auxFailAttempts = 0u;
	bool sawD3BeforePhyDisable = false;

	void initLive() {
		const uint32_t db = dig * rdna4modeset::kSeg2DigStride;
		const uint32_t ab = rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] = 0x00010201u; // active & enabled
		regs[kSeg2 + rdna4modeset::kSeg2Dp0SteerFifo + db] = 0x00000001u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0LinkCntl + db] = 0x00000011u;      // trained bit 4
		regs[kSeg2 + rdna4modeset::kSeg2Dp0TrainPatSel + db] = 0x00000001u;
		regs[kSeg2 + rdna4aux::kHpdStatus0 + aux * rdna4aux::kHpdStride] = 0x00000012u; // connected + sensed
		regs[kSeg2 + ab + rdna4aux::kAuxRegControl] = (aux << 20) | 0x00000001u;
		regs[kSeg2 + rdna4ddcpad::ddc_mask_reg(static_cast<uint8_t>(aux))] = 0u;
		regs[kSeg2 + rdna4ddcpad::kSeg2GpioAuxCtrl5] = rdna4ddcpad::ddc_i2c_mode_bit(static_cast<uint8_t>(aux));
		dpcd[rdna4dpms::kDpcdSetPowerState] = rdna4dpms::kDpPowerStateD0;
	}

	void handleAuxGo(uint32_t swVal) {
		if (auxFailAttempts > 0u) {
			--auxFailAttempts;
			auxSts = 0x00000005u; // done + timeout
			return;
		}
		if (auxTxIdx < 4u) { auxSts = 0x00000001u; return; }
		const uint8_t cmd = static_cast<uint8_t>(auxTx[0] >> 4);
		const uint32_t addr = (static_cast<uint32_t>(auxTx[0] & 0x0fu) << 16) |
		                      (static_cast<uint32_t>(auxTx[1]) << 8) | auxTx[2];
		const uint32_t len = static_cast<uint32_t>(auxTx[3]) + 1u;
		(void)swVal;
		auxRxIdx = 0u;
		if (cmd == 0x8u) { // native AUX write
			for (uint32_t i = 0u; i < len && (4u + i) < auxTxIdx; ++i) {
				if (addr + i < sizeof(dpcd)) dpcd[addr + i] = auxTx[4u + i];
			}
			auxRx[0] = 0x00u; // ACK
			auxSts = 0x00000001u | (1u << 24);
		} else {
			auxRx[0] = 0x00u;
			auxSts = 0x00000001u | (1u << 24);
		}
	}

	uint32_t rdAbs(uint32_t absDw) {
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
		if (!absAllowed(absDw)) {
			++deniedWrites;
			return false;
		}
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		if (absDw == ab + rdna4aux::kAuxRegSwData) {
			if (val & 1u) {
				auxRxIdx = 0u;
			} else {
				if (val & 0x80000000u) auxTxIdx = 0u;
				if (auxTxIdx < sizeof(auxTx)) auxTx[auxTxIdx++] = static_cast<uint8_t>((val >> 8) & 0xffu);
			}
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegSwCntl) {
			auxSw = val;
			if (val & 1u) handleAuxGo(val);
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
		const uint32_t db = dig * rdna4modeset::kSeg2DigStride;
		if (absDw == kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db) {
			regs[absDw] = (val & 1u) ? (val | 0x00010000u) : (val & ~0x00010000u);
			return true;
		}
		regs[absDw] = val;
		return true;
	}

	rdna4modeset::Hw makeHw() {
		rdna4modeset::Hw hw {};
		hw.cookie = this;
		hw.rdSeg = [](void *c, uint32_t seg, uint32_t off) -> uint32_t {
			return static_cast<MockDpms *>(c)->rdAbs((seg == 1u ? kSeg1 : kSeg2) + off);
		};
		hw.wrSeg = [](void *c, uint32_t seg, uint32_t off, uint32_t val) -> bool {
			return static_cast<MockDpms *>(c)->wrAbs((seg == 1u ? kSeg1 : kSeg2) + off, val);
		};
		hw.phyControl = [](void *c, uint32_t action, uint8_t, uint8_t, uint8_t, uint8_t) -> bool {
			auto *m = static_cast<MockDpms *>(c);
			if (action == 2u) {
				m->phyDisableCalls++;
				m->sawD3BeforePhyDisable = (m->dpcd[rdna4dpms::kDpcdSetPowerState] == rdna4dpms::kDpPowerStateD3);
			}
			return true;
		};
		hw.nowUs = [](void *c) -> uint64_t { return static_cast<MockDpms *>(c)->nowUs; };
		hw.delayUs = [](void *c, uint32_t us) { static_cast<MockDpms *>(c)->nowUs += us; };
		hw.auxIo = rdna4aux::Io {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockDpms *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockDpms *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c) -> uint64_t { return static_cast<MockDpms *>(c)->nowUs; },
			[](void *c, uint32_t us) { static_cast<MockDpms *>(c)->nowUs += us; },
		};
		return hw;
	}

	rdna4ddcpad::DdcPadHw makePadHw() {
		return rdna4ddcpad::DdcPadHw {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockDpms *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockDpms *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c, uint32_t ms) { static_cast<MockDpms *>(c)->nowUs += static_cast<uint64_t>(ms) * 1000ULL; },
		};
	}
};

}  // namespace

int main() {
	using namespace rdna4dpms;

	expect(boot_arg_enabled(false, 0u), "absent garnet-dpms defaults ON");
	expect(boot_arg_enabled(true, 1u), "garnet-dpms=1 enables HW DPMS");
	expect(!boot_arg_enabled(true, 0u), "garnet-dpms=0 disables HW DPMS");

	expect(should_be_on(2u, 1u), "power=2 & conn=1 -> display ON");
	expect(!should_be_on(1u, 1u), "power=1 -> display OFF");
	expect(!should_be_on(0u, 1u), "power=0 -> display OFF");
	expect(!should_be_on(2u, 0u), "conn=0 -> display OFF");
	expect(!should_be_on(0u, 0u), "power=0 & conn=0 -> display OFF");

	rdna4aux::ActiveOutput out {};
	out.found = true;
	out.isDp = true;
	out.otg = 0u;
	out.dig = 1u;
	out.link = 1u;
	out.hpd = 1u;
	out.aux = 1u;
	out.auxValid = true;

	MockDpms mock;
	mock.initLive();
	const auto off = dpmsOff(mock.makeHw(), out);
	const uint32_t db = out.dig * rdna4modeset::kSeg2DigStride;
	expect(off.streamBlanked && off.phyDisabled && off.sinkD3Written,
	       "dpmsOff blanks stream, disables PHY via DMUB, and writes DPCD D3");
	expect((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] & 0x10001u) == 0u,
	       "DP_VID_STREAM_CNTL enable and status bits cleared");
	expect((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0SteerFifo + db] & 2u) != 0u,
	       "DP_STEER_FIFO reset asserted after blank");
	expect((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0LinkCntl + db] & (1u << 4)) == 0u,
	       "DP_LINK_CNTL trained bit cleared");
	expect(mock.phyDisableCalls == 1u, "DMUB PHY disable invoked once");
	expect(mock.sawD3BeforePhyDisable, "DPCD 0x0600=D3 written BEFORE DMUB PHY disable");
	expect(mock.dpcd[kDpcdSetPowerState] == kDpPowerStateD3, "DPCD 0x0600 set to D3 (0x02)");
	expect(mock.deniedWrites == 0u, "all dpmsOff writes pass DCN41 allowlist");

	// Wake prepare retries AUX once and restores AUX pad mode + DPCD D0.
	mock.auxFailAttempts = 1u;
	const auto padHw = mock.makePadHw();
	const auto wake = dpmsWakePrepare(mock.makeHw(), out, &padHw);
	expect(wake.auxPadRestored && wake.sinkD0Written,
	       "dpmsWakePrepare restores AUX pads and retries DPCD D0 wake");
	expect(mock.dpcd[kDpcdSetPowerState] == kDpPowerStateD0, "DPCD 0x0600 restored to D0 (0x01)");
	expect((mock.regs[kSeg2 + rdna4ddcpad::ddc_mask_reg(static_cast<uint8_t>(out.aux))] &
	        rdna4ddcpad::kGpioDdcAuxPadMode) != 0u &&
	       (mock.regs[kSeg2 + rdna4ddcpad::kSeg2GpioAuxCtrl5] &
	        rdna4ddcpad::ddc_i2c_mode_bit(static_cast<uint8_t>(out.aux))) == 0u,
	       "AUX pad registers returned to AUX mode");
	expect(mock.deniedWrites == 0u, "all dpmsWakePrepare writes pass DCN41 allowlist");

	// Non-DP output safely no-ops without MMIO.
	rdna4aux::ActiveOutput hdmi = out;
	hdmi.isDp = false;
	const auto hdmiOff = dpmsOff(mock.makeHw(), hdmi);
	expect(!hdmiOff.streamBlanked && !hdmiOff.phyDisabled && !hdmiOff.sinkD3Written,
	       "non-DP output safely skips DPMS-off");

	std::printf("dcn41_dpms_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
