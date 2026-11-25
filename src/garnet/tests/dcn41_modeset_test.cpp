// Host unit & simulated-DCN4.1 regression tests for dcn41_modeset.h.
// Covers: 3-tier mode planning (Current8bpc / Hbr3_8bpc / Hbr3_Dsc12),
// VESA DSC 1.2a integer RC + 128B PPS packing, OTG/VTG/MSA/DP_DTO golden
// verification against live RX 9060 XT + Mi Monitor 4K@60Hz capture,
// end-to-end 4K@60->75->120->160->60Hz switches with HUBP DLG/TTU scaling
// and DIG FE<->BE re-binding, failure rollbacks (CR/EQ fail, underflow,
// frame stall), and strict dcn41_allow_ranges.h enforcement on every write.

#include "dcn/dcn41_modeset.h"
#include "dcn41_allow_ranges.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace {

constexpr uint32_t kSeg1 = 0x00c0u;
constexpr uint32_t kSeg2 = 0x34c0u;

bool absAllowed(uint32_t absDw) {
	for (uint32_t i = 0u; i < DCN41_HARD_DENY_COUNT; ++i) {
		if (absDw >= dcn41_hard_deny[i].abs_lo && absDw <= dcn41_hard_deny[i].abs_hi) return false;
	}
	for (uint32_t i = 0u; i < DCN41_DENY_BLOCK_COUNT; ++i) {
		if (absDw >= dcn41_deny_blocks[i].abs_lo && absDw <= dcn41_deny_blocks[i].abs_hi) return false;
	}
	for (uint32_t i = 0u; i < DCN41_ALLOW_RANGE_COUNT; ++i) {
		if (absDw >= dcn41_allow_ranges[i].abs_lo && absDw <= dcn41_allow_ranges[i].abs_hi) return true;
	}
	return false;
}

struct MockDcn {
	std::unordered_map<uint32_t, uint32_t> regs; // absDw -> val
	std::unordered_map<uint32_t, uint8_t> dpcd;  // dpcdAddr -> byte
	uint64_t nowUs = 1000000ULL;
	uint32_t frameCount = 100u;
	uint32_t phyEnableCalls = 0u;
	uint32_t phyDisableCalls = 0u;
	uint32_t phyDriveCalls = 0u;
	uint8_t lastPhyRate = 0u;
	uint8_t lastPhyLanes = 0u;
	uint32_t lastDispClkKhz = 0u;
	uint32_t lastDppClkKhz = 0u;
	uint32_t deniedWrites = 0u;

	// Fault injection knobs
	bool failClocks = false;
	bool failLinkCr = false;
	bool failLinkEq = false;
	bool injectUnderflow = false;
	bool injectFrameStall = false;
	bool clobberDigOnPhyDisable = true;

	// AUX state machine (DPCD native read/write over DP_AUX1)
	uint8_t auxTx[20] {};
	uint32_t auxTxIdx = 0u;
	uint8_t auxRx[20] {};
	uint32_t auxRxLen = 0u;
	uint32_t auxRxIdx = 0u;
	uint32_t auxSw = 0u;
	uint32_t auxSts = 0u;

	uint32_t otg = 0u;
	uint32_t dig = 1u;
	uint32_t aux = 1u;
	uint32_t hpd = 1u;

	void initLiveMiMonitor4k60() {
		regs.clear();
		dpcd.clear();
		const uint32_t ob = otg * rdna4modeset::kSeg2OtgStride;
		const uint32_t db = dig * rdna4modeset::kSeg2DigStride;
		const uint32_t hb = otg * rdna4modeset::kSeg2HubpStride;
		const uint32_t odmB = otg * rdna4modeset::kSeg2OdmStride;

		// Exact hardware-captured RX 9060 XT + Mi Monitor 4K@60Hz registers
		regs[kSeg1 + rdna4modeset::kSeg1DpDto0Phase + otg * 4u]  = 533250000u;
		regs[kSeg1 + rdna4modeset::kSeg1DpDto0Modulo + otg * 4u] = 720000000u;
		regs[kSeg1 + rdna4modeset::kSeg1OtgPixRateDiv]           = 0x00000000u;
		regs[kSeg1 + rdna4modeset::kSeg1SymClkAClkEn + dig]      = 0x00000001u;
		regs[kSeg1 + rdna4modeset::kSeg1DscClkDtoCtrl]           = 0x00000000u;

		regs[kSeg2 + rdna4modeset::kSeg2Otg0HTotal + ob]       = 0x00000f9fu; // 4000 - 1
		regs[kSeg2 + rdna4modeset::kSeg2Otg0HSyncA + ob]       = 0x00200000u; // width 32
		regs[kSeg2 + rdna4modeset::kSeg2Otg0HBlankSE + ob]     = 0x00700f70u; // end 112, start 3952
		regs[kSeg2 + rdna4modeset::kSeg2Otg0HSyncACntl + ob]   = 0x00000000u; // H+
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotal + ob]       = 0x000008adu; // 2222 - 1
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotalMin + ob]    = 0x000008adu;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotalMax + ob]    = 0x000008adu;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VSyncA + ob]       = 0x00050000u; // width 5
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VBlankSE + ob]     = 0x003b08abu; // end 59, start 2219
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VSyncACntl + ob]   = 0x00000001u; // V-
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VStartup + ob]     = 0x0000000du;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VUpdate + ob]      = 0x014003e8u;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0VReady + ob]       = 0x0000012cu;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0PstateReg + ob]    = 0x0000000bu;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0DbCtrl + ob]       = 0x02000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0MasterLock + ob]   = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Otg0StatusFrame + ob]  = frameCount;
		regs[kSeg2 + rdna4modeset::kSeg2Vtg0Control + otg]     = 0x88ab0001u;

		// HUBP0 4K@60Hz baseline DLG/TTU values
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Blank0 + hb]   = (0x3bu << 16) | 0x0015u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Blank1 + hb]   = 0x0000171cu;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0DstDim + hb]   = 0x00018000u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0AfterSc + hb]  = 0x00000020u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Vblank1 + hb]  = 0x00000100u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Nom1 + hb]     = 0x00002000u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0PerLineP + hb] = 0x00000180u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0PerLine + hb]  = 0x00000180u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0RefToPix + hb] = 0x00012000u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0SurfTtu0 + hb] = 0x08002800u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0SurfTtu1 + hb] = 0x00002800u;
		regs[kSeg2 + rdna4modeset::kSeg2HubpReq0TtuQosWm + hb] = 0x06000000u;

		// ODM0/OPTC
		regs[kSeg2 + rdna4modeset::kSeg2OptcInGlobal + odmB]   = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2OptcDataFmt + odmB]    = 0x00000000u;

		// DIG1 + DP1 (UEFI GOP boot: 4-lane HBR2, 10 bpc RGB, DP SST)
		regs[kSeg2 + rdna4modeset::kSeg2Dig0StreamMap + dig]   = dig;
		regs[kSeg2 + rdna4modeset::kSeg2Dig0BeClkCntl + db]    = 0x00000010u; // DP SST, CLK_EN=1
		regs[kSeg2 + rdna4modeset::kSeg2Dig0BeCntl + db]       = (hpd << 28) | ((1u << dig) << 8);
		regs[kSeg2 + rdna4modeset::kSeg2Dig0BeEnCntl + db]     = 0x00000001u;
		regs[kSeg2 + rdna4modeset::kSeg2Dig0FeClkCntl + db]    = 0x00000010u;
		regs[kSeg2 + rdna4modeset::kSeg2Dig0FeCntl + db]       = otg;
		regs[kSeg2 + rdna4modeset::kSeg2Dig0FeEnCntl + db]     = 0x00000001u;
		regs[kSeg2 + rdna4modeset::kSeg2Dig0FifoCtrl0 + db]    = 0x0000001du;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0LinkCntl + db]      = 0x00000110u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0PixelFormat + db]   = 0x00000200u; // 10 bpc RGB
		regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaColorim + db]    = 0x40000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0Config + db]        = 0x00000003u; // 4 lanes
		regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] = 0x00010001u; // ENABLE + STATUS
		regs[kSeg2 + rdna4modeset::kSeg2Dp0SteerFifo + db]     = 0x00000001u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0VidTiming + db]     = 0x00000100u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0LinkFraming + db]   = 0x10002000u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0DphyCntl + db]      = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaParam1 + db]     = (4000u << 16) | 2222u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaParam2 + db]     = (112u << 16) | 59u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaParam3 + db]     = (32u << 16) | (1u << 15) | 5u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaParam4 + db]     = (3840u << 16) | 2160u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0SecCntl + db]       = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0SecCntl2 + db]      = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2Dp0Gsp11Cntl + db]     = 0x00000000u;
		regs[kSeg2 + rdna4modeset::kSeg2DscTop0Ctrl]           = 0x00000000u;

		// HPD1 connected + sensed (SENSE | SENSE_DELAYED = 0x12) + AUX1 pre-owned by UEFI SW
		regs[kSeg2 + rdna4aux::kHpdStatus0 + hpd * rdna4aux::kHpdStride] = 0x00000012u;
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		regs[ab + rdna4aux::kAuxRegControl] = 0x00000001u | (hpd << 20);
		regs[ab + rdna4aux::kAuxRegArb]     = 0x00010004u; // SW owns AUX (the exact GOP state!)

		// Sink DPCD state (HBR3 + DSC 1.2a + FEC + TPS3/TPS4)
		dpcd[0x0100u] = 0x14u;
		dpcd[0x0101u] = 0x84u;
		dpcd[0x0160u] = 0x00u;
		dpcd[0x0202u] = 0x77u;
		dpcd[0x0203u] = 0x77u;
		dpcd[0x0204u] = 0x01u;
		dpcd[0x0206u] = 0x11u; // ask swing=1 on first CR try if needed
		dpcd[0x0207u] = 0x11u;
	}

	uint64_t livePixClkHz() const {
		const uint32_t ph = regs.count(kSeg1 + rdna4modeset::kSeg1DpDto0Phase + otg * 4u)
		                        ? regs.at(kSeg1 + rdna4modeset::kSeg1DpDto0Phase + otg * 4u) : 0u;
		const uint32_t md = regs.count(kSeg1 + rdna4modeset::kSeg1DpDto0Modulo + otg * 4u)
		                        ? regs.at(kSeg1 + rdna4modeset::kSeg1DpDto0Modulo + otg * 4u) : 1u;
		const uint32_t dv = regs.count(kSeg1 + rdna4modeset::kSeg1OtgPixRateDiv)
		                        ? ((regs.at(kSeg1 + rdna4modeset::kSeg1OtgPixRateDiv) >> (1u + otg * 8u)) & 0x0fu) : 0u;
		return static_cast<uint64_t>(dv) * md + ph;
	}

	void advanceTime(uint32_t us) {
		nowUs += us;
		if (injectFrameStall) return;
		const uint32_t ob = otg * rdna4modeset::kSeg2OtgStride;
		const uint64_t hTot = (regs[kSeg2 + rdna4modeset::kSeg2Otg0HTotal + ob] & 0x7fffu) + 1ULL;
		const uint64_t vTot = (regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotal + ob] & 0x7fffu) + 1ULL;
		const uint64_t pclk = livePixClkHz();
		if (hTot && vTot && pclk && us >= 1000u) {
			const uint64_t frames = (pclk * us) / (hTot * vTot * 1000000ULL);
			frameCount += static_cast<uint32_t>(frames);
			regs[kSeg2 + rdna4modeset::kSeg2Otg0StatusFrame + ob] = frameCount;
		}
	}

	void handleAuxGo(uint32_t swVal) {
		const uint32_t numBytes = (swVal >> 16) & 0x1fu;
		auxRxLen = 0u;
		auxRxIdx = 0u;
		if (numBytes < 3u) {
			auxSts = 0x100u; // DONE
			return;
		}
		const uint8_t cmd = (auxTx[0] >> 4) & 0x0fu;
		const uint32_t addr = (static_cast<uint32_t>(auxTx[0] & 0x0fu) << 16) |
		                      (static_cast<uint32_t>(auxTx[1]) << 8) | auxTx[2];
		const uint32_t len = (numBytes >= 4u) ? (static_cast<uint32_t>(auxTx[3]) + 1u) : 0u;
		if (cmd == 0x8u) { // Native DPCD write
			for (uint32_t i = 0u; i < len && (4u + i) < sizeof(auxTx); ++i) {
				dpcd[addr + i] = auxTx[4u + i];
			}
			// Update simulated link training status when DPCD 0x0102 is written
			if (addr <= 0x0102u && addr + len > 0x0102u) {
				const uint8_t pat = dpcd[0x0102u] & 0x0fu;
				if (pat == 1u) { // TPS1 CR
					dpcd[0x0202u] = failLinkCr ? 0x00u : 0x11u;
					dpcd[0x0203u] = failLinkCr ? 0x00u : 0x11u;
					dpcd[0x0204u] = 0x00u;
				} else if (pat == 2u || pat == 3u || pat == 7u) { // EQ
					dpcd[0x0202u] = failLinkEq ? 0x11u : 0x77u;
					dpcd[0x0203u] = failLinkEq ? 0x11u : 0x77u;
					dpcd[0x0204u] = failLinkEq ? 0x00u : 0x01u;
				}
			}
			auxRx[0] = 0x00u; // ACK
			auxRxLen = 1u;
		} else if (cmd == 0x9u) { // Native DPCD read
			auxRx[0] = 0x00u; // ACK
			for (uint32_t i = 0u; i < len && (1u + i) < sizeof(auxRx); ++i) {
				auxRx[1u + i] = dpcd.count(addr + i) ? dpcd[addr + i] : 0u;
			}
			auxRxLen = 1u + len;
		} else {
			auxRx[0] = 0x10u; // NACK
			auxRxLen = 1u;
		}
		auxSts = 0x1u | ((auxRxLen & 0x1fu) << 24); // bit 0 = AUX_SW_DONE
	}

	uint32_t rdAbs(uint32_t absDw) {
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		if (absDw == ab + rdna4aux::kAuxRegSwStat) return auxSts;
		if (absDw == ab + rdna4aux::kAuxRegSwCntl) return auxSw & ~1u;
		if (absDw == ab + rdna4aux::kAuxRegSwData) {
			const uint8_t b = (auxRxIdx < auxRxLen) ? auxRx[auxRxIdx++] : 0u;
			return static_cast<uint32_t>(b) << 8;
		}
		if (injectUnderflow && absDw == kSeg2 + rdna4modeset::kSeg2OptcInGlobal + otg * rdna4modeset::kSeg2OdmStride) {
			return regs[absDw] | 0x0400u; // UNDERFLOW_OCCURRED_STATUS
		}
		return regs.count(absDw) ? regs[absDw] : 0u;
	}

	bool wrAbs(uint32_t absDw, uint32_t val) {
		if (!absAllowed(absDw)) {
			++deniedWrites;
			return false;
		}
		const uint32_t ab = kSeg2 + rdna4aux::kAuxBase0 + aux * rdna4aux::kAuxStride;
		if (absDw == ab + rdna4aux::kAuxRegSwData) {
			if (val & 1u) {
				auxRxIdx = 0u; // read-mode FIFO pointer reset (0x80000001)
			} else {
				if (val & 0x80000000u) auxTxIdx = 0u; // write-mode autoincrement reset with byte 0
				if (auxTxIdx < sizeof(auxTx)) {
					auxTx[auxTxIdx++] = static_cast<uint8_t>((val >> 8) & 0xffu);
				}
			}
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegSwCntl) {
			auxSw = val;
			if (val & 1u) handleAuxGo(val);
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegIntAck) {
			if (val & 0x2u) auxSts &= ~0x1u;
			return true;
		}
		if (absDw == ab + rdna4aux::kAuxRegArb) {
			if (val & 0x10000u) regs[absDw] = 0x00010004u;
			else if (val & 0x20000u) regs[absDw] = 0x00000000u;
			return true;
		}

		const uint32_t ob = otg * rdna4modeset::kSeg2OtgStride;
		const uint32_t db = dig * rdna4modeset::kSeg2DigStride;
		if (absDw == kSeg2 + rdna4modeset::kSeg2Otg0MasterLock + ob) {
			regs[absDw] = (val & 1u) ? 0x00000101u : 0x00000000u; // mirror UPDATE_LOCK_STATUS
			return true;
		}
		if (absDw == kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db) {
			regs[absDw] = (val & 1u) ? (val | 0x00010000u) : (val & ~0x00010000u); // mirror STATUS
			return true;
		}
		if (absDw == kSeg2 + rdna4modeset::kSeg2Dig0FifoCtrl0 + db) {
			regs[absDw] = (val & 2u) ? (val | (1u << 20)) : (val & ~(1u << 20)); // mirror RESET_DONE
			return true;
		}
		regs[absDw] = val;
		return true;
	}

	rdna4modeset::Hw makeHw() {
		rdna4modeset::Hw hw {};
		hw.cookie = this;
		hw.rdSeg = [](void *c, uint32_t seg, uint32_t off) -> uint32_t {
			auto *m = static_cast<MockDcn *>(c);
			return m->rdAbs((seg == 1u ? kSeg1 : kSeg2) + off);
		};
		hw.wrSeg = [](void *c, uint32_t seg, uint32_t off, uint32_t val) -> bool {
			auto *m = static_cast<MockDcn *>(c);
			return m->wrAbs((seg == 1u ? kSeg1 : kSeg2) + off, val);
		};
		hw.raiseClocks = [](void *c, uint32_t disp, uint32_t dpp) -> bool {
			auto *m = static_cast<MockDcn *>(c);
			if (m->failClocks) return false;
			m->lastDispClkKhz = disp;
			m->lastDppClkKhz = dpp;
			return true;
		};
		hw.phyControl = [](void *c, uint32_t action, uint8_t rate, uint8_t lanes, uint8_t, uint8_t) -> bool {
			auto *m = static_cast<MockDcn *>(c);
			if (action == 2u) {
				++m->phyDisableCalls;
				if (m->clobberDigOnPhyDisable) {
					// Simulate DMUB TRANSMITTER_CONTROL_DISABLE resetting DIG FE/BE enable bits!
					const uint32_t db = m->dig * rdna4modeset::kSeg2DigStride;
					m->regs[kSeg2 + rdna4modeset::kSeg2Dig0BeEnCntl + db] = 0u;
					m->regs[kSeg2 + rdna4modeset::kSeg2Dig0FeEnCntl + db] = 0u;
					m->regs[kSeg2 + rdna4modeset::kSeg2Dig0StreamMap + m->dig] = 0u;
				}
			} else if (action == 1u) {
				++m->phyEnableCalls;
				m->lastPhyRate = rate;
				m->lastPhyLanes = lanes;
			} else if (action == 3u) {
				++m->phyDriveCalls;
			}
			return true;
		};
		hw.nowUs = [](void *c) -> uint64_t { return static_cast<MockDcn *>(c)->nowUs; };
		hw.delayUs = [](void *c, uint32_t us) { static_cast<MockDcn *>(c)->advanceTime(us); };
		hw.auxIo = rdna4aux::Io {
			this,
			[](void *c, uint32_t off) -> uint32_t { return static_cast<MockDcn *>(c)->rdAbs(kSeg2 + off); },
			[](void *c, uint32_t off, uint32_t val) -> bool { return static_cast<MockDcn *>(c)->wrAbs(kSeg2 + off, val); },
			[](void *c) -> uint64_t { return static_cast<MockDcn *>(c)->nowUs; },
			[](void *c, uint32_t us) { static_cast<MockDcn *>(c)->advanceTime(us); },
		};
		return hw;
	}
};

rdna4aux::DpcdCaps makeMiMonitorDpcd(uint8_t curRate = 0x14u) {
	rdna4aux::DpcdCaps d {};
	d.valid = true;
	d.dpcdRev = 0x14u;
	d.maxLinkRate = 0x1eu; // HBR3
	d.maxLaneCount = 4u;
	d.tps3Supported = true;
	d.tps4Supported = true;
	d.enhancedFraming = true;
	d.trainingAuxRdInt = 1u;
	d.fecCap = 0x01u;
	d.curLinkBwSet = curRate;
	d.curLaneCountSet = 4u;
	d.dscSupported = true;
	d.dscRev = 0x12u;
	d.dscSliceCap1 = 0x0au; // 2 and 4 slices
	d.dscLineBufDepth = 0x05u; // 8 + 5 = 13 bits
	d.dscMaxSliceWidth = 0x08u; // 8 * 320 = 2560 px
	return d;
}

rdna4aux::ActiveOutput makeOut() {
	rdna4aux::ActiveOutput out {};
	out.found = true;
	out.isDp = true;
	out.otg = 0u;
	out.dig = 1u;
	out.link = 1u;
	out.digMode = 0u;
	out.hpd = 1u;
	out.aux = 1u;
	out.auxValid = true;
	return out;
}

void testPlanModeTiers() {
	const auto dpcd = makeMiMonitorDpcd(0x14u); // booted on HBR2
	// RX 9060 XT real SMU DPM ceilings: DISPCLK ~1900 MHz, DPPCLK ~1800 MHz
	constexpr uint32_t kDispMax = 1900000u;
	constexpr uint32_t kDppMax  = 1800000u;

	const auto t60  = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 60u, 352000u, 1420000000ULL);
	const auto t75  = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 75u, 352000u, 1420000000ULL);
	const auto t120 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 120u, 352000u, 1420000000ULL);
	const auto t144 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 144u, 352000u, 1420000000ULL);
	const auto t160 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 160u, 352000u, 1420000000ULL);

	const auto p60 = rdna4modeset::planMode(t60, dpcd, kDispMax, kDppMax);
	assert(p60.tier == rdna4modeset::LinkTier::Current8bpc);
	assert(!p60.useDsc && !p60.useFec && p60.targetLinkRate == 0x14u);

	const auto p75 = rdna4modeset::planMode(t75, dpcd, kDispMax, kDppMax);
	assert(p75.tier == rdna4modeset::LinkTier::Current8bpc);
	assert(!p75.useDsc && p75.targetLinkRate == 0x14u);

	const auto p120 = rdna4modeset::planMode(t120, dpcd, kDispMax, kDppMax);
	assert(p120.tier == rdna4modeset::LinkTier::Hbr3_8bpc);
	assert(!p120.useDsc && !p120.useFec && p120.targetLinkRate == 0x1eu && p120.targetLanes == 4u);

	const auto p144 = rdna4modeset::planMode(t144, dpcd, kDispMax, kDppMax);
	assert(p144.tier == rdna4modeset::LinkTier::Hbr3_Dsc12);
	assert(p144.useDsc && p144.useFec && p144.dscSlicesH == 4u && p144.dscSliceHeight == 108u);

	const auto p160 = rdna4modeset::planMode(t160, dpcd, kDispMax, kDppMax);
	assert(p160.tier == rdna4modeset::LinkTier::Hbr3_Dsc12);
	assert(p160.useDsc && p160.useFec && p160.targetLinkRate == 0x1eu);
	assert(p160.dscSlicesH == 4u && p160.dscSliceHeight == 108u && p160.dscBppX16 == 192u);

	// Fail closed when DSC is disabled or clock ceiling is too low
	const auto p160NoDsc = rdna4modeset::planMode(t160, dpcd, kDispMax, kDppMax, true, false);
	assert(p160NoDsc.tier == rdna4modeset::LinkTier::Unsupported);
	const auto p160LowClk = rdna4modeset::planMode(t160, dpcd, 1000000u, 1000000u);
	assert(p160LowClk.tier == rdna4modeset::LinkTier::Unsupported);

	const uint64_t maxClk = rdna4modeset::maxSupportedPixelClockHz(dpcd, kDispMax, kDppMax);
	assert(maxClk >= t160.pixelClockHz);
}

void testDscParamsAndPps() {
	rdna4modeset::DscParams d {};
	assert(rdna4modeset::computeDscParams(3840u, 2160u, 4u, 108u, 13u, &d));
	assert(d.sliceWidth == 960u);
	assert(d.numSlicesV == 20u);
	assert(d.sliceChunkSize == 1440u); // 960 * 12 / 8 = 1440 bytes exactly
	assert(d.initialXmitDelay == 341u);
	assert(d.initialScaleValue == 10u); // 8 * 8192 / (8192 - 2048) = 10
	assert(d.scaleDecrementInterval == 160u); // 320 / (10 - 8) = 160
	assert(d.finalOffset < d.rcModelSize);
	assert(d.initialDecDelay > 0u);
	assert(d.optcBytesPerPixel == 0x18000000u); // 1.5 bytes/px in u3.28 fixed point (12 bpp!)

	uint8_t pps[128] {};
	rdna4modeset::packDscPps128(d, pps);
	assert(pps[0] == 0x12u); // DSC 1.2
	assert(pps[3] == 0x8du); // 8 bpc, 13-bit linebuf
	assert(pps[4] == 0x30u); // convert_rgb=1, block_pred=1, bpp_hi=0
	assert(pps[5] == 0xc0u); // 192 = 12.0 bpp
	assert(((pps[6] << 8) | pps[7]) == 2160u);
	assert(((pps[8] << 8) | pps[9]) == 3840u);
	assert(((pps[10] << 8) | pps[11]) == 108u);
	assert(((pps[12] << 8) | pps[13]) == 960u);
	assert(((pps[14] << 8) | pps[15]) == 1440u);
}

void testTimingRegsMatchesHardware4k60Golden() {
	// Exact Mi Monitor 4K@60Hz timing captured on live RX 9060 XT
	rdna4edidmodes::DetailedTiming t {};
	t.pixelClockHz = 533250000ULL;
	t.width = 3840u;
	t.hBlank = 160u;
	t.hSyncOffset = 48u;
	t.hSyncWidth = 32u;
	t.height = 2160u;
	t.vBlank = 62u;
	t.vSyncOffset = 3u;
	t.vSyncWidth = 5u;
	t.hPositive = true;
	t.vPositive = false;

	rdna4modeset::TimingRegs tr {};
	assert(rdna4modeset::computeTimingRegs(t, 720000000u, 0x014003e8u, 0x0000012cu, &tr));
	assert(tr.hTotal == 0x0f9fu);
	assert(tr.hSyncA == 0x00200000u);
	assert(tr.hBlankSE == 0x00700f70u);
	assert(tr.hSyncACntl == 0u);
	assert(tr.vTotal == 0x08adu);
	assert(tr.vSyncA == 0x00050000u);
	assert(tr.vBlankSE == 0x003b08abu);
	assert(tr.vSyncACntl == 1u);
	assert(tr.vtgCtrl == 0x08ab0001u);
	assert(tr.dtoInt == 0u && tr.dtoPhase == 533250000u && tr.dtoModulo == 720000000u);
	assert(tr.msa1 == ((4000u << 16) | 2222u));
	assert(tr.msa2 == ((112u << 16) | 59u));
	assert(tr.msa3 == ((32u << 16) | (1u << 15) | 5u));
	assert(tr.msa4 == ((3840u << 16) | 2160u));
}

void testEndToEndSwitchesAndAllowlist() {
	MockDcn mock;
	mock.initLiveMiMonitor4k60();
	const auto hw = mock.makeHw();
	const auto out = makeOut();
	auto dpcd = makeMiMonitorDpcd(0x14u);

	// 1. 4K@60Hz -> 4K@75Hz (Tier 1: keep HBR2, switch to 8 bpc, no retrain, no DSC)
	const auto t75 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 75u, 352000u, 1420000000ULL);
	const auto r75 = rdna4modeset::applyMode(hw, out, dpcd, t75, 1900000u, 1800000u);
	assert(r75.status == rdna4modeset::kOk);
	assert(r75.tierUsed == rdna4modeset::LinkTier::Current8bpc);
	assert(!r75.rolledBack);
	assert(mock.phyEnableCalls == 0u);
	assert(mock.deniedWrites == 0u);

	// 2. 4K@75Hz -> 4K@120Hz (Tier 2: HBR3 retrain, 8 bpc, no DSC, dtoInt == 1)
	const auto t120 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 120u, 352000u, 1420000000ULL);
	const auto r120 = rdna4modeset::applyMode(hw, out, dpcd, t120, 1900000u, 1800000u);
	if (r120.status != rdna4modeset::kOk) {
		fprintf(stderr, "r120 failed: status=%u measured=%u tier=%u deniedWrites=%u\n",
		        r120.status, r120.measuredMilliHz, static_cast<unsigned>(r120.tierUsed), mock.deniedWrites);
	}
	assert(r120.status == rdna4modeset::kOk);
	assert(r120.tierUsed == rdna4modeset::LinkTier::Hbr3_8bpc);
	assert(mock.phyEnableCalls == 1u && mock.lastPhyRate == 0x1eu && mock.lastPhyLanes == 4u);
	assert(mock.dpcd[0x0100u] == 0x1eu);
	assert(mock.dpcd[0x0160u] == 0x00u);
	// Verify DIG FE/BE were re-bound even though PHY disable clobbered them!
	const uint32_t db = out.dig * rdna4modeset::kSeg2DigStride;
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0BeEnCntl + db] & 1u) == 1u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0FeEnCntl + db] & 1u) == 1u);
	assert(mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0StreamMap + out.dig] == out.dig);
	assert(((mock.regs[kSeg1 + rdna4modeset::kSeg1OtgPixRateDiv] >> 1) & 0x0fu) == 1u);
	assert(((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0FifoCtrl0 + db] >> 8) & 3u) == 1u); // 2 px/cycle (>720MHz)
	assert(((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidTiming + db] >> 14) & 3u) == 1u); // DP_VID_N_INTERVAL=1
	assert(mock.deniedWrites == 0u);
	dpcd.curLinkBwSet = mock.dpcd[0x0100u];

	// 3. 4K@120Hz -> 4K@160Hz (Tier 3: HBR3 + FEC + DSC 1.2a + PPS SDP + DPCD 0x0160=1)
	const auto t160 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 160u, 352000u, 1420000000ULL);
	const auto r160 = rdna4modeset::applyMode(hw, out, dpcd, t160, 1900000u, 1800000u);
	assert(r160.status == rdna4modeset::kOk);
	assert(r160.tierUsed == rdna4modeset::LinkTier::Hbr3_Dsc12);
	assert(mock.dpcd[0x0120u] == 0x01u); // FEC_READY
	assert(mock.dpcd[0x0160u] == 0x01u); // DSC_ENABLE
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0DphyCntl + db] & 0x30u) == 0x30u); // FEC_EN + FEC_READY_SHADOW
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0PixelFormat + db] & 1u) == 1u); // DSC enabled in stream
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0SecCntl2 + db] & (1u << 28)) != 0u); // GSP11_PPS
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0AfmtCntl + db] & 1u) != 0u); // AFMT_AUDIO_CLOCK_EN
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0Gsp11Cntl + db] & 0x10u) != 0u); // GSP11_ENABLE
	assert(((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaVbidMisc + db] >> 16) & 0xffffu) == 3u); // VBID6_LINE_NUM=3 > 2
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidN + db] & 0xffffffu) == 0x8000u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidM + db] & 0xffffffu) > 0u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2OptcDataFmt] & 0x30u) == 0x10u); // OPTC_DSC_MODE=1
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dscc0Cfg0] & 0x13fu) == 0x13fu); // ICH_RESET=0xf, 4 slices=3, ALT_ICH=1
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Domain16PgCfg] & 0x101u) == 0x001u); // DSC0 un-power-gated
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Vpg0MemPwr + db] & 0x11u) == 0x01u); // VPG SRAM powered on
	// Verify HUBP BLANK_OFFSET_0 DLG_V_BLANK_END matches new OTG_V_BLANK_END, and VSTARTUP < vBlankEnd!
	const uint32_t newVBlankEnd = (mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0VBlankSE] >> 16) & 0x7fffu;
	const uint32_t hubpVBlankEnd = (mock.regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Blank0] >> 16) & 0x7fffu;
	const uint32_t newVStartup = mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0VStartup] & 0x3ffu;
	assert(newVBlankEnd == hubpVBlankEnd);
	assert(newVStartup > 0u && newVStartup < newVBlankEnd);
	assert(mock.phyDriveCalls >= 8u); // per-lane drive calls during CR + EQ
	assert(mock.deniedWrites == 0u);
	dpcd.curLinkBwSet = mock.dpcd[0x0100u];
	dpcd.curDscEnable = mock.dpcd[0x0160u];

	// 4. 4K@160Hz -> 4K@60Hz (Clean DSC teardown + FEC off!)
	const auto t60 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 60u, 352000u, 1420000000ULL);
	const auto r60 = rdna4modeset::applyMode(hw, out, dpcd, t60, 1900000u, 1800000u);
	assert(r60.status == rdna4modeset::kOk);
	assert(mock.dpcd[0x0160u] == 0x00u);
	assert(mock.dpcd[0x0120u] == 0x00u); // FEC_READY cleared on FEC teardown
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0MsaVbidMisc + db] & 0xffff8000u) == 0u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0PixelFormat + db] & 1u) == 0u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2OptcDataFmt] & 0x30u) == 0u);
	assert((mock.regs[kSeg2 + rdna4modeset::kSeg2DscTop0Ctrl] & 1u) == 0u);
	assert(((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0FifoCtrl0 + db] >> 8) & 3u) == 0u); // back to 1 px/cycle (<=720MHz)
	assert(((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidTiming + db] >> 14) & 3u) == 0u);
	assert(mock.deniedWrites == 0u);
}

void testFailureRollbacksRestoreExactState() {
	const auto out = makeOut();
	const auto dpcd = makeMiMonitorDpcd(0x14u);
	const auto t160 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 160u, 352000u, 1420000000ULL);

	// Case A: Link EQ training failure rolls back to HBR2 + 4K@60Hz untouched
	{
		MockDcn mock;
		mock.initLiveMiMonitor4k60();
		const uint32_t origHTot = mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0HTotal];
		const uint32_t origDtoP = mock.regs[kSeg1 + rdna4modeset::kSeg1DpDto0Phase];
		const uint32_t origHubpB0 = mock.regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Blank0];
		mock.failLinkEq = true;
		const auto r = rdna4modeset::applyMode(mock.makeHw(), out, dpcd, t160, 1900000u, 1800000u);
		assert(r.status == rdna4modeset::kLinkTrainFailed && r.rolledBack);
		assert(mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0HTotal] == origHTot);
		assert(mock.regs[kSeg1 + rdna4modeset::kSeg1DpDto0Phase] == origDtoP);
		assert(mock.regs[kSeg2 + rdna4modeset::kSeg2HubpReq0Blank0] == origHubpB0);
		assert(mock.dpcd[0x0100u] == 0x14u);
		assert(mock.dpcd[0x0160u] == 0x00u);
		assert(mock.deniedWrites == 0u);
	}

	// Case B: OPTC underflow after unblank rolls back completely (including DSC disable & HBR2 restore)
	{
		MockDcn mock;
		mock.initLiveMiMonitor4k60();
		const uint32_t origVTot = mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotal];
		const uint32_t origPfmt = mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0PixelFormat + rdna4modeset::kSeg2DigStride];
		mock.injectUnderflow = true;
		const auto r = rdna4modeset::applyMode(mock.makeHw(), out, dpcd, t160, 1900000u, 1800000u);
		assert(r.status == rdna4modeset::kUnderflow && r.rolledBack);
		assert(mock.regs[kSeg2 + rdna4modeset::kSeg2Otg0VTotal] == origVTot);
		assert(mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0PixelFormat + rdna4modeset::kSeg2DigStride] == origPfmt);
		assert(mock.dpcd[0x0100u] == 0x14u);
		assert(mock.dpcd[0x0160u] == 0x00u);
		assert(mock.deniedWrites == 0u);
	}

	// Case C: OTG frame stall triggers kRateMismatch and full rollback
	{
		MockDcn mock;
		mock.initLiveMiMonitor4k60();
		const uint32_t origDtoP = mock.regs[kSeg1 + rdna4modeset::kSeg1DpDto0Phase];
		mock.injectFrameStall = true;
		const auto r = rdna4modeset::applyMode(mock.makeHw(), out, dpcd, t160, 1900000u, 1800000u);
		assert(r.status == rdna4modeset::kRateMismatch && r.rolledBack);
		assert(mock.regs[kSeg1 + rdna4modeset::kSeg1DpDto0Phase] == origDtoP);
		assert(mock.dpcd[0x0100u] == 0x14u);
		assert(mock.dpcd[0x0160u] == 0x00u);
		assert(mock.deniedWrites == 0u);
	}

	// Case D (Stage 3B): forceRetrain=true retrains DP link & rebinds DIG even on same rate (DPMS wake)
	{
		MockDcn mock;
		mock.initLiveMiMonitor4k60();
		const auto t60 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 60u, 352000u, 1420000000ULL);
		const uint32_t db = out.dig * rdna4modeset::kSeg2DigStride;
		// Simulate post-DPMS-off blanked stream & cleared DIG enables
		mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] = 0x00000200u;
		mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0BeEnCntl + db] = 0u;
		mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0FeEnCntl + db] = 0u;
		const auto r = rdna4modeset::applyMode(mock.makeHw(), out, dpcd, t60, 1900000u, 1800000u,
		                                       /*allowRetrain=*/true, /*allowDsc=*/true,
		                                       /*forceRetrain=*/true);
		assert(r.status == rdna4modeset::kOk && !r.rolledBack);
		assert(mock.phyEnableCalls == 1u && mock.lastPhyRate == 0x14u);
		assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0BeEnCntl + db] & 1u) != 0u);
		assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dig0FeEnCntl + db] & 1u) != 0u);
		assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] & 0x10001u) == 0x10001u);
		assert(mock.deniedWrites == 0u);
	}

	// Case E (Stage 3B review C3): forceRetrain=true with link training failure does NOT roll back & unblank dead PHY
	{
		MockDcn mock;
		mock.initLiveMiMonitor4k60();
		const auto t60 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 60u, 352000u, 1420000000ULL);
		const uint32_t db = out.dig * rdna4modeset::kSeg2DigStride;
		mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] = 0x00000200u;
		mock.failLinkEq = true;
		const auto r = rdna4modeset::applyMode(mock.makeHw(), out, dpcd, t60, 1900000u, 1800000u,
		                                       /*allowRetrain=*/true, /*allowDsc=*/true,
		                                       /*forceRetrain=*/true);
		assert(r.status == rdna4modeset::kLinkTrainFailed && !r.rolledBack);
		assert((mock.regs[kSeg2 + rdna4modeset::kSeg2Dp0VidStreamCntl + db] & 0x10001u) == 0u);
		assert(mock.deniedWrites == 0u);
	}
}

}  // namespace

int main() {
	testPlanModeTiers();
	testDscParamsAndPps();
	testTimingRegsMatchesHardware4k60Golden();
	testEndToEndSwitchesAndAllowlist();
	testFailureRollbacksRestoreExactState();
	printf("dcn41_modeset_test: ALL PASS\n");
	return 0;
}
