// dp_edid_aux_test.cpp — Host tests for DCN 4.1 DP AUX E-DDC EDID + DPCD reader and CTA/CVT mode helpers.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dcn/dp_edid_aux.h"
#include "dcn/primary_fb_pure.h"

namespace {

int gRun = 0;
int gFail = 0;

void expect(bool cond, const char *what) {
	++gRun;
	if (!cond) {
		++gFail;
		std::printf("FAIL: %s\n", what);
	}
}

void expect_u(const char *what, uint64_t got, uint64_t want) {
	++gRun;
	if (got != want) {
		++gFail;
		std::printf("FAIL: %s (got=%llu want=%llu)\n", what,
		            static_cast<unsigned long long>(got),
		            static_cast<unsigned long long>(want));
	}
}

void fixChecksum(uint8_t blk[128]) {
	uint32_t sum = 0u;
	for (size_t i = 0u; i < 127u; ++i) sum += blk[i];
	blk[127] = static_cast<uint8_t>(0u - sum);
}

struct FakeAuxHw {
	// Simulated DCN41 seg2 registers we care about.
	uint32_t otgCtrl[4] {};
	uint32_t digFeCntl[4] {}, digFeEn[4] {}, digBeCntl[4] {}, digBeEn[4] {};
	uint32_t hpdStatus[4] {};
	uint32_t auxCtrl[4] {}, auxSwCntl[4] {}, auxArb[4] {}, auxSwStat[4] {};

	// Simulated DP AUX FIFO & sink state.
	uint8_t txBuf[24] {};
	uint32_t txPos = 0u;
	uint8_t rxBuf[24] {};
	uint32_t rxLen = 0u;
	uint32_t rxPos = 0u;

	uint8_t edid[512] {};
	size_t edidBlocks = 3u;
	uint8_t i2cSeg = 0u;
	uint8_t i2cOff = 0u;
	bool ignoreSegment = false;
	uint32_t deferEveryN = 0u;
	uint32_t xferCount = 0u;
	uint32_t shortReadBytes = 0u; // if non-zero, cap I2C reads to this chunk size
	bool corruptNextBlockOnce = false;
	bool stopSeen = false;

	uint8_t dpcd00[16] {};
	uint8_t dpcd60[16] {};
	uint8_t dpcd100[16] {};
	uint8_t dpcd202[6] {};
	uint8_t dpcd2200[16] {};
	uint8_t dpcd2215 = 0u;

	uint64_t timeUs = 1000u;

	void handleGo(uint32_t inst) {
		++xferCount;
		const uint32_t totalBytes = (auxSwCntl[inst] >> 16) & 0x1fu;
		auxSwCntl[inst] &= ~1u;
		if (totalBytes < 3u || totalBytes > 20u) {
			auxSwStat[inst] = 0x1u | 0x400u;
			return;
		}
		const uint8_t cmd = txBuf[0] & 0xf0u;
		const uint32_t addr = (static_cast<uint32_t>(txBuf[0] & 0x0fu) << 16) |
		                      (static_cast<uint32_t>(txBuf[1]) << 8) | txBuf[2];
		const uint8_t len = (totalBytes >= 4u) ? static_cast<uint8_t>(txBuf[3] + 1u) : 0u;

		if (deferEveryN && (xferCount % deferEveryN) == 0u) {
			rxBuf[0] = (cmd & 0x80u) ? 0x20u : 0x80u; // AUX_DEFER (2) or I2C_DEFER (8)
			rxLen = 1u;
			rxPos = 0u;
			auxSwStat[inst] = 1u | (rxLen << 24);
			return;
		}

		if (cmd == 0x90u) { // Native DPCD read
			rxBuf[0] = 0x00u; // AUX_ACK
			for (uint8_t i = 0u; i < len; ++i) {
				const uint32_t a = addr + i;
				uint8_t v = 0u;
				if (a < 0x10u) v = dpcd00[a];
				else if (a >= 0x60u && a < 0x70u) v = dpcd60[a - 0x60u];
				else if (a >= 0x100u && a < 0x110u) v = dpcd100[a - 0x100u];
				else if (a >= 0x202u && a < 0x208u) v = dpcd202[a - 0x202u];
				else if (a >= 0x2200u && a < 0x2210u) v = dpcd2200[a - 0x2200u];
				else if (a == 0x2215u) v = dpcd2215;
				rxBuf[1u + i] = v;
			}
			rxLen = 1u + len;
		} else if (cmd == 0x40u || cmd == 0x00u) { // I2C write
			if (addr == 0x30u && len == 1u && !ignoreSegment) i2cSeg = txBuf[4];
			if (addr == 0x50u && len == 1u) i2cOff = txBuf[4];
			if (cmd == 0x00u && len == 0u) { i2cSeg = 0u; stopSeen = true; }
			rxBuf[0] = 0x00u;
			rxLen = 1u;
		} else if (cmd == 0x50u || cmd == 0x10u) { // I2C read / STOP
			if (cmd == 0x10u && len == 0u) {
				i2cSeg = 0u;
				stopSeen = true;
				rxBuf[0] = 0x00u;
				rxLen = 1u;
			} else {
				uint8_t retLen = len;
				if (shortReadBytes && retLen > shortReadBytes) retLen = static_cast<uint8_t>(shortReadBytes);
				rxBuf[0] = 0x00u;
				for (uint8_t i = 0u; i < retLen; ++i) {
					const size_t segBase = static_cast<size_t>(i2cSeg) * 256u;
					const size_t idx = segBase + i2cOff;
					uint8_t b = (idx < edidBlocks * 128u) ? edid[idx] : 0u;
					if (corruptNextBlockOnce && i2cOff == 127u) {
						b ^= 0x55u;
						corruptNextBlockOnce = false;
					}
					rxBuf[1u + i] = b;
					++i2cOff;
				}
				rxLen = 1u + retLen;
			}
		} else {
			rxBuf[0] = 0x10u; // NACK
			rxLen = 1u;
		}
		rxPos = 0u;
		auxSwStat[inst] = 1u | (rxLen << 24);
	}
};

uint32_t fakeRead(void *cookie, uint32_t off) {
	auto *hw = static_cast<FakeAuxHw *>(cookie);
	for (uint32_t i = 0u; i < 4u; ++i) {
		if (off == rdna4aux::kOtgControl0 + i * rdna4aux::kOtgStride) return hw->otgCtrl[i];
		if (off == rdna4aux::kDigFeCntl0 + i * rdna4aux::kDigStride) return hw->digFeCntl[i];
		if (off == rdna4aux::kDigFeEnCntl0 + i * rdna4aux::kDigStride) return hw->digFeEn[i];
		if (off == rdna4aux::kDigBeCntl0 + i * rdna4aux::kDigStride) return hw->digBeCntl[i];
		if (off == rdna4aux::kDigBeEnCntl0 + i * rdna4aux::kDigStride) return hw->digBeEn[i];
		if (off == rdna4aux::kHpdStatus0 + i * rdna4aux::kHpdStride) return hw->hpdStatus[i];
		const uint32_t base = rdna4aux::auxReg(i, 0u);
		if (off >= base && off < base + 7u) {
			const uint32_t r = off - base;
			if (r == rdna4aux::kAuxRegControl) return hw->auxCtrl[i];
			if (r == rdna4aux::kAuxRegSwCntl) return hw->auxSwCntl[i];
			if (r == rdna4aux::kAuxRegArb) return hw->auxArb[i];
			if (r == rdna4aux::kAuxRegSwStat) return hw->auxSwStat[i];
			if (r == rdna4aux::kAuxRegSwData) {
				if (hw->rxPos >= hw->rxLen) return 0u;
				const uint8_t b = hw->rxBuf[hw->rxPos++];
				return static_cast<uint32_t>(b) << 8;
			}
		}
	}
	return 0xffffffffu;
}

bool fakeWrite(void *cookie, uint32_t off, uint32_t val) {
	auto *hw = static_cast<FakeAuxHw *>(cookie);
	for (uint32_t i = 0u; i < 4u; ++i) {
		const uint32_t base = rdna4aux::auxReg(i, 0u);
		if (off >= base && off < base + 7u) {
			const uint32_t r = off - base;
			if (r == rdna4aux::kAuxRegArb) {
				if (val & 0x10000u) hw->auxArb[i] = 0x10004u; // SW granted
				if (val & 0x20000u) hw->auxArb[i] = 0x00000u; // released
				return true;
			}
			if (r == rdna4aux::kAuxRegIntAck) {
				if (val & 2u) hw->auxSwStat[i] &= ~1u;
				return true;
			}
			if (r == rdna4aux::kAuxRegSwCntl) {
				hw->auxSwCntl[i] = val;
				if (val & 1u) hw->handleGo(i);
				return true;
			}
			if (r == rdna4aux::kAuxRegSwData) {
				if (val & 0x80000000u) {
					if (val & 1u) hw->rxPos = 0u;
					else { hw->txPos = 0u; hw->txBuf[hw->txPos++] = static_cast<uint8_t>(val >> 8); }
				} else if (hw->txPos < sizeof(hw->txBuf)) {
					hw->txBuf[hw->txPos++] = static_cast<uint8_t>(val >> 8);
				}
				return true;
			}
		}
	}
	return false;
}

uint64_t fakeNowUs(void *cookie) { return static_cast<FakeAuxHw *>(cookie)->timeUs; }
void fakeDelayUs(void *cookie, uint32_t us) { static_cast<FakeAuxHw *>(cookie)->timeUs += us; }

void buildMiMonitor3BlockEdid(uint8_t edid[384]) {
	std::memset(edid, 0, 384);
	// Block 0: Base EDID with 3840x2160@60 DTD, Mi Monitor name, 48-160 Hz limits, 2 extensions.
	static constexpr uint8_t kHdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	std::memcpy(edid, kHdr, 8);
	edid[8] = 0x61u; edid[9] = 0xa9u;   // XMI
	edid[10] = 0xb4u; edid[11] = 0x27u; // 10164
	edid[18] = 1u; edid[19] = 4u;
	// DTD 0: 3840x2160@60 (533.25 MHz, Hblank=160, Vblank=62)
	uint8_t *d = edid + 54;
	const uint16_t clk = 53325u;
	d[0] = clk & 0xffu; d[1] = clk >> 8;
	d[2] = 3840u & 0xffu; d[3] = 160u & 0xffu; d[4] = ((3840u >> 8) << 4) | (160u >> 8);
	d[5] = 2160u & 0xffu; d[6] = 62u & 0xffu; d[7] = ((2160u >> 8) << 4) | (62u >> 8);
	d[8] = 48u; d[9] = 32u; d[10] = (3u << 4) | 5u; d[17] = 0x1au;
	// Descriptor 1: Monitor Name 'Mi Monitor'
	uint8_t *n = edid + 72;
	n[3] = 0xfcu;
	std::memcpy(n + 5, "Mi Monitor\x0a  ", 13);
	// Descriptor 2: Range Limits 48-160 Hz, H 352 kHz (97 + 255 via byte 4 = 0x0c), max dotclock 1420 MHz
	uint8_t *r = edid + 90;
	r[3] = 0xfdu; r[4] = 0x0cu; r[5] = 48u; r[6] = 160u; r[7] = 97u; r[8] = 97u; r[9] = 142u;
	edid[126] = 2u; // 2 extension blocks!
	fixChecksum(edid);

	// Block 1: CTA-861 with Video Data Block (VIC 16=1080p60, VIC 63=1080p120, VIC 118=4K120).
	uint8_t *cta = edid + 128;
	cta[0] = 0x02u; cta[1] = 0x03u; cta[2] = 8u; cta[3] = 0xf1u;
	cta[4] = (2u << 5) | 3u; // Video Data Block, 3 VICs
	cta[5] = 16u;
	cta[6] = 63u;
	cta[7] = 118u;
	fixChecksum(cta);

	// Block 2: DisplayID 2.0 Type VII timing: 3840x2160@160Hz (1408.000 MHz, hTotal=4000, vTotal=2200).
	uint8_t *did = edid + 256;
	did[0] = 0x70u; // DisplayID extension tag
	did[1] = 0x20u; // v2.0
	did[2] = 23u;   // payload bytes (3 header + 20 timing)
	did[3] = 0x00u; did[4] = 0x00u;
	did[5] = 0x22u; // Type VII Detailed Timing
	did[6] = 0x00u; // rev 0
	did[7] = 20u;   // 1 entry = 20 bytes
	uint8_t *e = did + 8;
	const uint32_t pclkK = 1408000u - 1u;
	e[0] = pclkK & 0xffu; e[1] = (pclkK >> 8) & 0xffu; e[2] = (pclkK >> 16) & 0xffu;
	e[3] = 0x80u; // preferred
	const uint16_t w1 = 3840u - 1u, hb1 = 160u - 1u, hso1 = 48u - 1u, hsw1 = 32u - 1u;
	const uint16_t h1 = 2160u - 1u, vb1 = 40u - 1u, vso1 = 3u - 1u, vsw1 = 5u - 1u;
	e[4] = w1 & 0xffu; e[5] = w1 >> 8;
	e[6] = hb1 & 0xffu; e[7] = hb1 >> 8;
	e[8] = hso1 & 0xffu; e[9] = (hso1 >> 8) | 0x80u; // H+
	e[10] = hsw1 & 0xffu; e[11] = hsw1 >> 8;
	e[12] = h1 & 0xffu; e[13] = h1 >> 8;
	e[14] = vb1 & 0xffu; e[15] = vb1 >> 8;
	e[16] = vso1 & 0xffu; e[17] = vso1 >> 8; // V-
	e[18] = vsw1 & 0xffu; e[19] = vsw1 >> 8;
	uint32_t inner = 0u;
	for (size_t i = 1u; i < 5u + 23u; ++i) inner += did[i];
	did[5u + 23u] = static_cast<uint8_t>(0u - inner);
	fixChecksum(did);
}

}  // namespace

int main() {
	FakeAuxHw hw {};
	hw.otgCtrl[0] = 0x00011201u;
	hw.digBeEn[1] = 1u;
	hw.digFeEn[1] = 1u;
	hw.digFeCntl[1] = 0u;            // OTG0
	hw.digBeCntl[1] = 0x10000200u;   // DP SST (mode 0), HPD1
	hw.auxCtrl[1] = 0x01140101u;     // AUX_EN=1, HPD_SEL=1
	hw.hpdStatus[1] = 0x00000113u;   // connected + sensed (0x12) WITH stale boot IRQ bits (0x101)
	buildMiMonitor3BlockEdid(hw.edid);

	// Populate DPCD: rev 1.4, HBR3 max 4 lanes, HBR2 currently active, DSC 1.2a supported, UHBR10+13.5.
	hw.dpcd00[0] = 0x14u; hw.dpcd00[1] = 0x1eu; hw.dpcd00[2] = 0x84u; hw.dpcd00[6] = 0x03u; hw.dpcd00[14] = 0x80u;
	hw.dpcd60[0] = 0x01u; hw.dpcd60[1] = 0x12u; hw.dpcd60[4] = 0x0fu;
	hw.dpcd100[0] = 0x14u; hw.dpcd100[1] = 0x84u;
	hw.dpcd202[0] = 0x77u; hw.dpcd202[1] = 0x77u; hw.dpcd202[2] = 0x01u;
	hw.dpcd2200[1] = 0x1eu; hw.dpcd2200[2] = 0x84u;
	hw.dpcd2215 = 0x05u; // UHBR10 + UHBR13.5

	const rdna4aux::Io io { &hw, fakeRead, fakeWrite, fakeNowUs, fakeDelayUs };

	// 1. Active output discovery.
	const rdna4aux::ActiveOutput out = rdna4aux::detectActiveOutput(io);
	expect(out.found && out.isDp && out.auxValid, "detectActiveOutput finds live DP output");
	expect_u("active OTG", out.otg, 0u);
	expect_u("active DIG", out.dig, 1u);
	expect_u("active HPD", out.hpd, 1u);
	expect_u("active AUX", out.aux, 1u);

	// 2. 3-block E-DDC read under DEFERs, 4-byte short reads, and 1 corrupted checksum retry.
	hw.deferEveryN = 3u;
	hw.shortReadBytes = 4u;
	hw.corruptNextBlockOnce = true;
	uint8_t edid[512] {};
	size_t edidLen = 0u;
	bool wrapped = false;
	const uint32_t rc = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, edid, 4u, &edidLen, &wrapped);
	expect_u("3-block E-DDC read succeeds despite DEFERs, short reads, and 1 checksum retry", rc, rdna4aux::kOk);
	expect_u("3-block E-DDC length is 384 bytes", edidLen, 384u);
	expect(!wrapped, "3-block read did not wrap");
	expect(hw.stopSeen, "I2C-over-AUX 0-byte STOP issued");
	expect_u("AUX arbitration released after EDID read", hw.auxArb[1], 0u);
	expect(std::memcmp(edid, hw.edid, 384u) == 0, "384-byte EDID matches sink byte-for-byte");

	char name[14] {};
	rdna4aux::extractMonitorName(edid, edidLen, name);
	expect(std::strcmp(name, "Mi Monitor") == 0, "monitor name extracted cleanly");

	// 3. Parse all 3 blocks (DTD + CTA VICs + DisplayID 2.0 4K@160Hz).
	rdna4edidmodes::ModeList modes {};
	const size_t nModes = rdna4edidmodes::build(edid, edidLen, 3840u, 2160u, 3840u * 4u, modes);
	expect(nModes >= 5u, "EDID + CTA VICs + DisplayID yields >= 5 modes");
	bool has4k160 = false, has4k120 = false, has1080p120 = false;
	for (size_t i = 0u; i < nModes; ++i) {
		const auto &t = modes.modes[i].timing;
		const uint32_t hz = (t.refreshMilliHz() + 500u) / 1000u;
		if (t.width == 3840u && t.height == 2160u && hz == 160u) has4k160 = true;
		if (t.width == 3840u && t.height == 2160u && hz == 120u) has4k120 = true;
		if (t.width == 1920u && t.height == 1080u && hz == 120u) has1080p120 = true;
	}
	expect(has4k160, "DisplayID 2.0 4K@160Hz parsed from Block 2");
	expect(has4k120, "CTA-861 VIC 118 4K@120Hz parsed from Block 1");
	expect(has1080p120, "CTA-861 VIC 63 1080p@120Hz parsed from Block 1");

	// 4. Pixel-clock budget filter excludes 4K@160Hz/4K@120Hz when capped at 600 MHz.
	rdna4fbprimary::ModeTable capped {};
	rdna4fbprimary::buildModeTable(edid, edidLen, 3840u, 2160u, 3840u * 4u, &capped, 600000000ULL);
	bool anyOver600 = false;
	for (size_t i = 0u; i < capped.list.count; ++i) {
		if (capped.list.modes[i].timing.pixelClockHz > 600000000ULL) anyOver600 = true;
	}
	expect(!anyOver600, "600 MHz budget cap filters out high-clock modes safely");
	rdna4fbprimary::ModeInfoSummary miActive {}, miOther {};
	expect(rdna4fbprimary::modeInfoForId(capped, static_cast<int32_t>(capped.activeModeId), &miActive), "active mode info");
	expect((miActive.flags & 0x4u) != 0u, "active mode carries Default flag");
	if (capped.list.count >= 2u) {
		const uint32_t otherId = (capped.list.modes[0].modeId == capped.activeModeId)
		                             ? capped.list.modes[1].modeId
		                             : capped.list.modes[0].modeId;
		expect(rdna4fbprimary::modeInfoForId(capped, static_cast<int32_t>(otherId), &miOther), "non-active mode info");
		expect((miOther.flags & 0x4u) == 0u, "non-active mode does not carry Default flag");
	}

	// 5. Sink ignoring segment 0x30 (wrapping block 2 back to block 0) is detected and truncated to 256B.
	hw.deferEveryN = 0u;
	hw.shortReadBytes = 0u;
	hw.ignoreSegment = true;
	edidLen = 0u;
	wrapped = false;
	const uint32_t rcWrap = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, edid, 4u, &edidLen, &wrapped);
	expect_u("wrapped-segment sink still succeeds with base+CTA blocks", rcWrap, rdna4aux::kOk);
	expect_u("wrapped-segment EDID truncated cleanly to 256B", edidLen, 256u);
	expect(wrapped, "wrapped-segment condition flagged");

	// 6. DPCD capability decode (HBR3 + DSC + UHBR).
	rdna4aux::DpcdCaps dpcd {};
	const uint32_t rcDpcd = rdna4aux::readDpcdCaps(io, out.aux, out.hpd, &dpcd);
	expect_u("DPCD caps read succeeds", rcDpcd, rdna4aux::kOk);
	expect(dpcd.valid && dpcd.dpcdRev == 0x14u && dpcd.maxLinkRate == 0x1eu && dpcd.maxLaneCount == 4u, "DPCD rev 1.4 HBR3 4-lane");
	expect(dpcd.curLinkBwSet == 0x14u && dpcd.curLaneCountSet == 4u, "active link HBR2 4-lane");
	expect(dpcd.dscSupported && dpcd.dscRev == 0x12u, "DSC 1.2a decoded");
	expect(dpcd.uhbrValid && dpcd.uhbrRates == 0x05u, "UHBR10+13.5 decoded");
	expect_u("HBR2 4-lane payload kbps", rdna4aux::payloadKbps8b10b(0x14u, 4u), 17280000ULL);
	expect_u("HBR3 4-lane payload kbps", rdna4aux::payloadKbps8b10b(0x1eu, 4u), 25920000ULL);

	// 7. CVT-RBv2 generator sanity check (3840x2160@120Hz fits HBR3 uncompressed; 4K@160Hz compacts into 352 kHz / 1420 MHz).
	const rdna4edidmodes::DetailedTiming cvt120 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 120u);
	expect(cvt120.refreshMilliHz() >= 119900u && cvt120.refreshMilliHz() <= 120100u, "CVT-RBv2 4K@120Hz rate accurate");
	expect(rdna4aux::requiredRgbKbps(cvt120.pixelClockHz, 8u) <= rdna4aux::payloadKbps8b10b(0x1eu, 4u),
	       "CVT-RBv2 4K@120Hz fits HBR3 4-lane uncompressed");
	const rdna4edidmodes::DetailedTiming cvt160 = rdna4edidmodes::makeCvtRb2Timing(3840u, 2160u, 160u, 352000u, 1420000000ULL);
	expect(cvt160.refreshMilliHz() >= 159900u && cvt160.refreshMilliHz() <= 160100u, "CVT-RBv3 4K@160Hz rate accurate");
	expect(cvt160.pixelClockHz <= 1420000000ULL && cvt160.vTotal() * 160ULL <= 352000ULL,
	       "CVT-RBv3 4K@160Hz respects 352 kHz H-sync and 1420 MHz pixel clock limits");

	// 8. Regression test for real Navi44 hardware: UEFI GOP leaves AUX_ARB_CONTROL = 0x00010004 (kSwCanAccess).
	hw.ignoreSegment = false;
	hw.auxArb[1] = 0x00010004u;
	edidLen = 0u;
	const uint32_t rcSwOwned = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, edid, 4u, &edidLen, &wrapped);
	expect_u("UEFI GOP pre-owned SW AUX arbitration (0x00010004) succeeds", rcSwOwned, rdna4aux::kOk);
	expect_u("UEFI GOP pre-owned SW read returns full 384B", edidLen, 384u);
	expect_u("AUX arbitration released to idle after SW-owned read", hw.auxArb[1], 0u);

	// 9. DMUB/firmware-owned AUX arbitration (owner == 2 = 0x8) MUST refuse with kBusy.
	hw.auxArb[1] = 0x01000008u;
	edidLen = 0u;
	rdna4aux::AuxDiag diag {};
	const uint32_t rcFwOwned = rdna4aux::readEdidOverAux(io, out.aux, out.hpd, edid, 4u, &edidLen, &wrapped, &diag);
	expect_u("firmware/DMUB-owned AUX arbitration returns kBusy", rcFwOwned, rdna4aux::kBusy);
	expect_u("diag captures exact AUX_ARB_CONTROL", diag.arb, 0x01000008u);
	hw.auxArb[1] = 0u;

	// 10. Synthesize high-refresh steps with sameSizeOnly=true (native 4K@50/60/75/100/120/144/160Hz).
	rdna4fbprimary::ModeTable hiRef {};
	rdna4fbprimary::buildModeTable(edid, 384u, 3840u, 2160u, 3840u * 4u, &hiRef, 1450000000ULL, true, true);
	bool all4k = true, has75 = false, has144 = false, has160 = false;
	for (size_t i = 0u; i < hiRef.list.count; ++i) {
		const auto &t = hiRef.list.modes[i].timing;
		if (t.width != 3840u || t.height != 2160u) all4k = false;
		const uint32_t hz = (t.refreshMilliHz() + 500u) / 1000u;
		if (hz == 75u) has75 = true;
		if (hz == 144u) has144 = true;
		if (hz == 160u) has160 = true;
	}
	expect(all4k && has75 && has144 && has160, "sameSizeOnly+synthesize exposes 4K@60/75/100/120/144/160Hz");

	std::printf("dp_edid_aux_test: %d checks, %d failed\n", gRun, gFail);
	return gFail ? 1 : 0;
}
