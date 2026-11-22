// dp_edid_aux.h — Pure DCN 4.1 active-output detector, spec-compliant DP AUX
// I2C-over-AUX E-DDC reader (blocks 0..3 with 0x30 segment pointer), and DPCD
// capability reader (DP 1.4 / DSC 1.2a / DP 2.1 UHBR).
// Header-only, C++17, no kernel/IOKit dependencies; host-tested in dp_edid_aux_test.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "edid_modes_pure.h"

namespace rdna4aux {

// DCN 4.1 DMU segment 2 (BASE_IDX 2, typically 0x34c0) relative dword offsets.
constexpr uint32_t kDefaultDmuSeg2Base = 0x34c0u;
constexpr uint32_t kOtgControl0        = 0x1b43u; // stride 0x80
constexpr uint32_t kOtgStride          = 0x0080u;
constexpr uint32_t kDigFeCntl0         = 0x2093u; // stride 0x124
constexpr uint32_t kDigFeEnCntl0       = 0x2095u;
constexpr uint32_t kDigBeClkCntl0      = 0x20bbu;
constexpr uint32_t kDigBeCntl0         = 0x20bcu;
constexpr uint32_t kDigBeEnCntl0       = 0x20bdu;
constexpr uint32_t kDigStreamMapper0   = 0x1f0du; // + dig
constexpr uint32_t kDigStride          = 0x0124u;
constexpr uint32_t kAuxBase0           = 0x16b2u; // stride 0x1c; regs 0..6
constexpr uint32_t kAuxStride          = 0x001cu;
constexpr uint32_t kHpdStatus0         = 0x175au; // stride 0x8
constexpr uint32_t kHpdStride          = 0x0008u;

constexpr uint32_t kAuxRegControl = 0u;
constexpr uint32_t kAuxRegSwCntl  = 1u;
constexpr uint32_t kAuxRegArb     = 2u;
constexpr uint32_t kAuxRegIntAck  = 3u;
constexpr uint32_t kAuxRegSwStat  = 4u;
constexpr uint32_t kAuxRegSwData  = 6u;

enum Status : uint32_t {
	kOk = 0u,
	kInvalid,
	kBusy,
	kDisconnected,
	kTimeout,
	kIoError,
	kNack,
	kDefer,
	kChecksum,
	kNotDp,
	kNoActiveOutput,
};

struct Io {
	void *cookie;
	uint32_t (*read)(void *cookie, uint32_t seg2Offset);
	bool (*write)(void *cookie, uint32_t seg2Offset, uint32_t value);
	uint64_t (*nowUs)(void *cookie);
	void (*delayUs)(void *cookie, uint32_t us);
};

struct ActiveOutput {
	bool found;
	bool isDp;
	uint32_t otg;
	uint32_t dig;
	uint32_t link;
	uint32_t digMode;
	uint32_t hpd;
	uint32_t aux;
	bool auxValid;
};

struct DpcdCaps {
	bool valid;
	uint8_t dpcdRev;            // 0x0000: 0x12=1.2, 0x14=1.4
	uint8_t maxLinkRate;        // 0x0001: 0x0a=HBR, 0x14=HBR2, 0x1e=HBR3
	uint8_t maxLaneCount;       // 0x0002 & 0x1f
	bool tps3Supported;         // 0x0002 bit 6
	bool enhancedFraming;       // 0x0002 bit 7
	uint8_t maxDownspread;      // 0x0003
	bool tps4Supported;         // 0x0003 bit 7
	uint8_t codingCap;          // 0x0006 (bit 1 = 128b/132b)
	uint8_t trainingAuxRdInt;   // 0x000e & 0x7f
	uint8_t extCapPresent;      // 0x000e bit 7
	uint8_t fecCap;             // 0x0090 (bit 0 = FEC_CAPABLE)
	uint8_t curLinkBwSet;       // 0x0100
	uint8_t curLaneCountSet;    // 0x0101 & 0x1f
	uint8_t curCodingSet;       // 0x0108
	uint8_t curDscEnable;       // 0x0160
	uint8_t laneStatus[6];      // 0x0202..0x0207
	bool dscSupported;          // 0x0060 bit 0
	uint8_t dscRev;             // 0x0061
	uint8_t dscRcBufBlkSize;    // 0x0062
	uint8_t dscRcBufSize;       // 0x0063
	uint8_t dscSliceCap1;       // 0x0064
	uint8_t dscLineBufDepth;    // 0x0065
	uint8_t dscBlockPred;       // 0x0066
	uint8_t dscColorFormatCap;  // 0x0069
	uint8_t dscColorDepthCap;   // 0x006a
	uint8_t dscPeakThroughput0; // 0x006b
	uint8_t dscMaxSliceWidth;   // 0x006c
	uint8_t dscSliceCap2;       // 0x006d
	uint8_t dscMinBppInc;       // 0x006f
	bool uhbrValid;
	uint8_t uhbrRates;          // 0x2215: bit0=UHBR10, bit1=UHBR20, bit2=UHBR13.5
	uint8_t extMaxLinkRate;     // 0x2201
	uint8_t extMaxLaneCount;    // 0x2202 & 0x1f
};

struct AuxDiag {
	uint32_t ctl;
	uint32_t arb;
	uint32_t sw;
	uint32_t sts;
	uint32_t hpd;
};

inline uint32_t linkCodeToRateKbps(uint8_t code) {
	// Per-lane raw symbol rate in kHz * 10 = raw kbps; 8b/10b payload = raw * 8 / 10 = code * 216000 kbps.
	switch (code) {
		case 0x06u: return 1620000u; // RBR  1.62 Gbps/lane
		case 0x0au: return 2700000u; // HBR  2.70 Gbps/lane
		case 0x14u: return 5400000u; // HBR2 5.40 Gbps/lane
		case 0x1eu: return 8100000u; // HBR3 8.10 Gbps/lane
		default: return 0u;
	}
}

// Effective uncompressed payload bandwidth (kbps) for 8b/10b link (80% efficiency).
inline uint64_t payloadKbps8b10b(uint8_t rateCode, uint8_t lanes) {
	const uint64_t raw = static_cast<uint64_t>(linkCodeToRateKbps(rateCode)) * lanes;
	return (raw * 8ULL) / 10ULL;
}

// Required uncompressed RGB payload bandwidth (kbps) at given bpc (usually 8 bpc -> 24 bpp).
inline uint64_t requiredRgbKbps(uint64_t pixelClockHz, uint32_t bpc) {
	return (pixelClockHz * static_cast<uint64_t>(bpc) * 3ULL + 999ULL) / 1000ULL;
}

inline uint32_t auxReg(uint32_t inst, uint32_t reg) {
	return kAuxBase0 + inst * kAuxStride + reg;
}

inline ActiveOutput detectActiveOutput(const Io &io) {
	ActiveOutput out {};
	if (!io.read) return out;

	uint32_t litOtg = 0xffffffffu;
	for (uint32_t i = 0u; i < 4u; ++i) {
		const uint32_t v = io.read(io.cookie, kOtgControl0 + i * kOtgStride);
		if (v != 0xffffffffu && (v & 1u) != 0u) {
			litOtg = i;
			break;
		}
	}
	if (litOtg == 0xffffffffu) return out;

	uint32_t activeDig = 0xffffffffu;
	uint32_t activeLink = 0xffffffffu;
	uint32_t beCntl = 0u;
	uint32_t beClk = 0xffffffffu;
	for (uint32_t j = 0u; j < 4u; ++j) {
		const uint32_t feEn = io.read(io.cookie, kDigFeEnCntl0 + j * kDigStride);
		const uint32_t feCntl = io.read(io.cookie, kDigFeCntl0 + j * kDigStride);
		if (feEn == 0xffffffffu || feCntl == 0xffffffffu) continue;
		if ((feEn & 1u) == 0u || (feCntl & 7u) != litOtg) continue;
		const uint32_t map = io.read(io.cookie, kDigStreamMapper0 + j);
		uint32_t candLinks[2] = { j, j };
		uint32_t candCount = 1u;
		if (map != 0xffffffffu && (map & 7u) < 4u && (map & 7u) != j) {
			candLinks[0] = map & 7u;
			candLinks[1] = j;
			candCount = 2u;
		}
		for (uint32_t ci = 0u; ci < candCount; ++ci) {
			const uint32_t link = candLinks[ci];
			const uint32_t beEn = io.read(io.cookie, kDigBeEnCntl0 + link * kDigStride);
			const uint32_t be = io.read(io.cookie, kDigBeCntl0 + link * kDigStride);
			const uint32_t bc = io.read(io.cookie, kDigBeClkCntl0 + link * kDigStride);
			if (beEn == 0xffffffffu || be == 0xffffffffu || (beEn & 1u) == 0u) continue;
			if (link != j && (be & (1u << (8u + j))) == 0u) continue;
			activeDig = j;
			activeLink = link;
			beCntl = be;
			beClk = bc;
			break;
		}
		if (activeDig != 0xffffffffu) break;
	}
	if (activeDig == 0xffffffffu) return out;

	out.found = true;
	out.otg = litOtg;
	out.dig = activeDig;
	out.link = activeLink;
	out.digMode = (beClk != 0xffffffffu && (beClk & 0x10u) != 0u) ? (beClk & 7u) : (beCntl & 7u);
	out.isDp = (out.digMode == 0u || out.digMode == 5u);
	out.hpd = (beCntl >> 28) & 7u;

	if (out.isDp) {
		// Prefer DP_AUX whose AUX_EN=1 and AUX_HPD_SEL == out.hpd.
		for (uint32_t k = 0u; k < 4u; ++k) {
			const uint32_t ctl = io.read(io.cookie, auxReg(k, kAuxRegControl));
			if (ctl == 0xffffffffu || (ctl & 1u) == 0u) continue;
			if (((ctl >> 20) & 7u) == out.hpd) {
				out.aux = k;
				out.auxValid = true;
				break;
			}
		}
		// Fallback: any enabled DP_AUX whose paired HPD reports connected & sensed.
		if (!out.auxValid) {
			for (uint32_t k = 0u; k < 4u; ++k) {
				const uint32_t ctl = io.read(io.cookie, auxReg(k, kAuxRegControl));
				if (ctl == 0xffffffffu || (ctl & 1u) == 0u) continue;
				const uint32_t h = (ctl >> 20) & 7u;
				if (h >= 4u) continue;
				const uint32_t hs = io.read(io.cookie, kHpdStatus0 + h * kHpdStride);
				if (hs != 0xffffffffu && (hs & 0x12u) == 0x12u) {
					out.aux = k;
					out.hpd = h;
					out.auxValid = true;
					break;
				}
			}
		}
	}
	return out;
}

struct Session {
	Io io;
	uint32_t inst;
	uint32_t hpd;
	bool owned;
	bool requested;
	uint64_t startUs;
	uint64_t deadlineUs;
	uint32_t lastFault;
	AuxDiag diag;
};

inline bool sessionTimeOk(Session &s) {
	const uint64_t now = s.io.nowUs(s.io.cookie);
	if (now < s.startUs || now >= s.deadlineUs) {
		s.lastFault = kTimeout;
		return false;
	}
	return true;
}

inline bool sessionRd(Session &s, uint32_t seg2Off, uint32_t *val) {
	const uint32_t v = s.io.read(s.io.cookie, seg2Off);
	if (v == 0xffffffffu) {
		s.lastFault = kIoError;
		return false;
	}
	*val = v;
	return true;
}

inline bool sessionWr(Session &s, uint32_t reg, uint32_t val) {
	if (!sessionTimeOk(s)) return false;
	if (!s.io.write(s.io.cookie, auxReg(s.inst, reg), val)) {
		s.lastFault = kIoError;
		return false;
	}
	return true;
}

inline bool sessionPresent(Session &s) {
	uint32_t ctl = 0u, hs = 0u;
	if (!sessionTimeOk(s) || !sessionRd(s, auxReg(s.inst, kAuxRegControl), &ctl)) return false;
	s.diag.ctl = ctl;
	if ((ctl & 1u) == 0u || ((ctl >> 20) & 7u) != s.hpd) {
		s.lastFault = kInvalid;
		return false;
	}
	if (!sessionRd(s, kHpdStatus0 + s.hpd * kHpdStride, &hs)) return false;
	s.diag.hpd = hs;
	// Require connected + sensed (0x12). Do not fail on stale boot HPD IRQ latch bits (0x101).
	if ((hs & 0x12u) != 0x12u) {
		s.lastFault = kDisconnected;
		return false;
	}
	return true;
}

inline uint32_t auxBegin(Session &s, uint32_t budgetUs) {
	s.owned = false;
	s.requested = false;
	s.lastFault = kInvalid;
	s.diag = AuxDiag {};
	if (!s.io.read || !s.io.write || !s.io.nowUs || !s.io.delayUs || s.inst >= 4u || s.hpd >= 4u)
		return s.lastFault;
	s.startUs = s.io.nowUs(s.io.cookie);
	s.deadlineUs = s.startUs + budgetUs;
	if (!sessionPresent(s)) return s.lastFault;

	// Wait briefly if firmware/DMUB (owner == 2, kDmcuCanAccess) or an active AUX_SW_GO holds the engine.
	// Note: UEFI GOP commonly leaves AUX_REG_RW_CNTL_STATUS == 1 (kSwCanAccess) and AUX_SW_USE_AUX_REG_REQ
	// set; matching Linux dce_aux.c acquire_engine, owner == 0 (free) OR owner == 1 (SW) is acquirable.
	uint32_t arb = 0u, sw = 0u;
	bool freeNow = false;
	for (uint32_t attempt = 0u; attempt < 40u; ++attempt) {
		if (!sessionRd(s, auxReg(s.inst, kAuxRegArb), &arb) ||
		    !sessionRd(s, auxReg(s.inst, kAuxRegSwCntl), &sw))
			return s.lastFault;
		s.diag.arb = arb;
		s.diag.sw = sw;
		const uint32_t owner = (arb >> 2) & 3u;
		if (owner != 2u && (sw & 1u) == 0u) {
			freeNow = true;
			break;
		}
		if (!sessionTimeOk(s)) return s.lastFault;
		s.io.delayUs(s.io.cookie, 250u);
	}
	if (!freeNow) return s.lastFault = kBusy;

	if (!sessionWr(s, kAuxRegArb, (arb & ~0x20000u) | 0x10000u)) return s.lastFault;
	s.requested = true;
	for (uint32_t poll = 0u; poll < 20u; ++poll) {
		if (!sessionRd(s, auxReg(s.inst, kAuxRegArb), &arb)) return s.lastFault;
		s.diag.arb = arb;
		if (((arb >> 2) & 3u) == 1u) {
			s.owned = true;
			return s.lastFault = kOk;
		}
		s.io.delayUs(s.io.cookie, 50u);
	}
	return s.lastFault = kBusy;
}

inline void auxEnd(Session &s) {
	if (!s.requested) {
		s.owned = false;
		return;
	}
	uint32_t arb = 0u;
	if (s.io.read && s.io.write) {
		arb = s.io.read(s.io.cookie, auxReg(s.inst, kAuxRegArb));
		if (arb != 0xffffffffu && ((arb & 0xcu) == 0u || (s.owned && (arb & 0xcu) == 4u))) {
			(void)s.io.write(s.io.cookie, auxReg(s.inst, kAuxRegArb), (arb & ~0x10000u) | 0x20000u);
		}
	}
	s.owned = false;
	s.requested = false;
}

// Single raw DP AUX transaction. Supports len=0 (0-byte I2C-over-AUX STOP) and short I2C reads.
// Returns DP AUX reply nibble (0=ACK, 1=NACK, 2=AUX_DEFER, 4=I2C_NACK, 8=I2C_DEFER), or -1 on transport fault.
inline int auxXferOnce(Session &s, uint8_t action, uint32_t address,
                       const uint8_t *txData, uint8_t len,
                       uint8_t *rxData, uint8_t rxCap, uint8_t *rxGot) {
	if (rxGot) *rxGot = 0u;
	s.lastFault = kInvalid;
	const bool isWrite = (action & 0x10u) == 0u;
	if (!s.owned || address > 0xfffffu || len > 16u || rxCap > 16u) return -1;
	if (isWrite && len > 0u && !txData) return -1;
	if (!isWrite && len > 0u && (!rxData || rxCap < len)) return -1;

	uint32_t arb = 0u, sw = 0u, sts = 0u;
	if (!sessionPresent(s) || !sessionRd(s, auxReg(s.inst, kAuxRegArb), &arb) ||
	    !sessionRd(s, auxReg(s.inst, kAuxRegSwCntl), &sw))
		return -1;
	s.diag.arb = arb;
	s.diag.sw = sw;
	if (((arb >> 2) & 3u) != 1u || (sw & 1u) != 0u) {
		s.lastFault = kBusy;
		return -1;
	}

	// Clear SW_DONE_ACK before launching.
	if (!sessionWr(s, kAuxRegIntAck, 2u)) return -1;
	uint32_t i = 0u;
	for (; i < 100u; ++i) {
		if (!sessionRd(s, auxReg(s.inst, kAuxRegSwStat), &sts)) return -1;
		s.diag.sts = sts;
		if ((sts & 1u) == 0u) break;
		s.io.delayUs(s.io.cookie, 10u);
	}
	if (i == 100u) {
		s.lastFault = kBusy;
		return -1;
	}

	const uint32_t hdrLen = (len > 0u) ? 4u : 3u;
	const uint32_t totalBytes = hdrLen + (isWrite ? static_cast<uint32_t>(len) : 0u);
	if (!sessionRd(s, auxReg(s.inst, kAuxRegSwCntl), &sw)) return -1;
	sw = (sw & ~0x1f0001u) | (totalBytes << 16);
	if (!sessionWr(s, kAuxRegSwCntl, sw)) return -1;

	uint8_t hdr[4];
	hdr[0] = static_cast<uint8_t>(action | ((address >> 16) & 0x0fu));
	hdr[1] = static_cast<uint8_t>((address >> 8) & 0xffu);
	hdr[2] = static_cast<uint8_t>(address & 0xffu);
	hdr[3] = static_cast<uint8_t>(len > 0u ? (len - 1u) : 0u);
	for (i = 0u; i < hdrLen; ++i) {
		const uint32_t word = (static_cast<uint32_t>(hdr[i]) << 8) | (i == 0u ? 0x80000000u : 0u);
		if (!sessionWr(s, kAuxRegSwData, word)) return -1;
	}
	if (isWrite) {
		for (i = 0u; i < len; ++i) {
			if (!sessionWr(s, kAuxRegSwData, static_cast<uint32_t>(txData[i]) << 8)) return -1;
		}
	}

	// Launch GO (bit 0).
	if (!sessionWr(s, kAuxRegSwCntl, sw | 1u)) return -1;
	for (i = 0u; i < 2000u; ++i) {
		if (!sessionRd(s, auxReg(s.inst, kAuxRegSwStat), &sts)) return -1;
		s.diag.sts = sts;
		if ((sts & 1u) != 0u) break;
		s.io.delayUs(s.io.cookie, 10u);
	}
	if (i == 2000u) {
		s.lastFault = kTimeout;
		return -1;
	}
	// Match Linux dce_aux.c get_channel_status: check HPD disconnect (bit 9) and RX timeout (bits [7:4]).
	if (sts & 0x200u) { s.lastFault = kDisconnected; return -1; }
	if (sts & 0xf0u)  { s.lastFault = kTimeout;      return -1; }

	const uint32_t replyCount = (sts >> 24) & 0x1fu;
	if (replyCount == 0u || replyCount > 17u) {
		s.lastFault = kIoError;
		return -1;
	}
	// Reset read FIFO pointer + read reply command byte.
	uint32_t word = 0u;
	if (!sessionWr(s, kAuxRegSwData, 0x80000001u) ||
	    !sessionRd(s, auxReg(s.inst, kAuxRegSwData), &word))
		return -1;

	const int code = static_cast<int>((word >> 12) & 0x0fu);
	if (code != 0) {
		s.lastFault = (code == 2 || code == 8) ? kDefer : kNack;
		return code;
	}
	const uint8_t payloadBytes = static_cast<uint8_t>(replyCount - 1u);
	if (!isWrite && rxData) {
		const uint8_t toCopy = (payloadBytes <= rxCap) ? payloadBytes : rxCap;
		for (i = 0u; i < toCopy; ++i) {
			if (!sessionRd(s, auxReg(s.inst, kAuxRegSwData), &word)) return -1;
			rxData[i] = static_cast<uint8_t>((word >> 8) & 0xffu);
		}
		if (rxGot) *rxGot = toCopy;
	}
	s.lastFault = kOk;
	return 0;
}

inline int auxXferWithDefer(Session &s, uint8_t action, uint32_t address,
                            const uint8_t *txData, uint8_t len,
                            uint8_t *rxData, uint8_t rxCap, uint8_t *rxGot) {
	for (uint32_t retry = 0u; retry < 7u; ++retry) {
		const int rc = auxXferOnce(s, action, address, txData, len, rxData, rxCap, rxGot);
		if (rc == 0) return 0;
		if (rc != 2 && rc != 8) return rc;
		if (retry != 6u) s.io.delayUs(s.io.cookie, 500u);
	}
	s.lastFault = kDefer;
	return 8;
}

inline uint32_t readEdidBlockOverAux(Session &s, uint32_t blockIdx, uint8_t out[128]) {
	if (!out || blockIdx >= rdna4edidmodes::MaxBlocks) return kInvalid;

	for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
		uint8_t buf[128] {};
		bool ok = true;

		// VESA E-DDC: write segment pointer to I2C 0x30 for blocks >= 2 (MOT=1).
		if (blockIdx >= 2u) {
			const uint8_t seg = static_cast<uint8_t>(blockIdx >> 1);
			if (auxXferWithDefer(s, 0x40u, 0x30u, &seg, 1u, nullptr, 0u, nullptr) != 0) {
				ok = false;
			}
		}
		// Write byte offset inside the 256-byte segment to I2C 0x50 (MOT=1).
		if (ok) {
			const uint8_t off = static_cast<uint8_t>((blockIdx & 1u) * 128u);
			if (auxXferWithDefer(s, 0x40u, 0x50u, &off, 1u, nullptr, 0u, nullptr) != 0) {
				ok = false;
			}
		}
		// Read 128 bytes in <=16-byte chunks, handling short I2C-over-AUX replies cleanly.
		size_t pos = 0u;
		uint32_t emptyReads = 0u;
		while (ok && pos < 128u) {
			const uint8_t want = static_cast<uint8_t>((128u - pos > 16u) ? 16u : (128u - pos));
			uint8_t got = 0u;
			if (auxXferWithDefer(s, 0x50u, 0x50u, nullptr, want, buf + pos, want, &got) != 0) {
				ok = false;
				break;
			}
			if (got == 0u) {
				if (++emptyReads >= 4u) {
					s.lastFault = kIoError;
					ok = false;
					break;
				}
				s.io.delayUs(s.io.cookie, 500u);
				continue;
			}
			emptyReads = 0u;
			pos += got;
		}
		// Always issue 0-byte I2C-over-AUX STOP (MOT=0) so the monitor releases its I2C bus,
		// preserving any prior read fault and diagnostics.
		const uint32_t priorFault = s.lastFault;
		const AuxDiag priorDiag = s.diag;
		(void)auxXferWithDefer(s, 0x10u, 0x50u, nullptr, 0u, nullptr, 0u, nullptr);
		if (!ok) {
			s.lastFault = priorFault;
			s.diag = priorDiag;
		}

		if (ok && pos == 128u) {
			const bool valid = (blockIdx == 0u) ? rdna4edidmodes::baseBlockValid(buf)
			                                    : rdna4edidmodes::blockChecksumOk(buf);
			if (valid) {
				memcpy(out, buf, 128u);
				return kOk;
			}
			s.lastFault = kChecksum;
		}
		if (attempt != 2u) s.io.delayUs(s.io.cookie, 2000u);
	}
	return s.lastFault ? s.lastFault : kChecksum;
}

inline uint32_t readEdidOverAux(const Io &io, uint32_t auxInst, uint32_t hpdIdx,
                                uint8_t *outBuf, size_t maxBlocks, size_t *outLen,
                                bool *outExtWrapped = nullptr, AuxDiag *outDiag = nullptr) {
	if (outLen) *outLen = 0u;
	if (outExtWrapped) *outExtWrapped = false;
	if (outDiag) *outDiag = AuxDiag {};
	if (!outBuf || maxBlocks == 0u) return kInvalid;
	if (maxBlocks > rdna4edidmodes::MaxBlocks) maxBlocks = rdna4edidmodes::MaxBlocks;

	Session s {};
	s.io = io;
	s.inst = auxInst;
	s.hpd = hpdIdx;
	// 250 ms bounded total budget for up to 4 blocks + retries.
	const uint32_t beginRc = auxBegin(s, 250000u);
	if (beginRc != kOk) {
		if (outDiag) *outDiag = s.diag;
		auxEnd(s);
		return beginRc;
	}

	uint32_t rc = readEdidBlockOverAux(s, 0u, outBuf);
	if (rc != kOk) {
		if (outDiag) *outDiag = s.diag;
		auxEnd(s);
		return rc;
	}

	size_t totalBlocks = 1u;
	size_t declaredExt = outBuf[126];
	if (declaredExt > maxBlocks - 1u) declaredExt = maxBlocks - 1u;

	for (size_t b = 1u; b <= declaredExt; ++b) {
		uint8_t *dst = outBuf + b * rdna4edidmodes::BlockSize;
		const uint32_t extRc = readEdidBlockOverAux(s, static_cast<uint32_t>(b), dst);
		if (extRc != kOk) break;
		// Guard against sinks that ignore segment 0x30 and repeat block 0 for block 2.
		if (b >= 2u && rdna4edidmodes::baseBlockValid(dst) &&
		    memcmp(dst, outBuf, rdna4edidmodes::BlockSize) == 0) {
			if (outExtWrapped) *outExtWrapped = true;
			break;
		}
		totalBlocks = b + 1u;
	}

	if (outDiag) *outDiag = s.diag;
	auxEnd(s);
	if (outLen) *outLen = totalBlocks * rdna4edidmodes::BlockSize;
	return kOk;
}

inline bool readDpcdBytes(Session &s, uint32_t addr, uint8_t *out, uint8_t len) {
	uint8_t got = 0u;
	return auxXferWithDefer(s, 0x90u, addr, nullptr, len, out, len, &got) == 0 && got == len;
}

inline bool writeDpcdBytes(Session &s, uint32_t addr, const uint8_t *data, uint8_t len) {
	return auxXferWithDefer(s, 0x80u, addr, data, len, nullptr, 0u, nullptr) == 0;
}

inline bool dpcdRead(const Io &io, uint32_t auxInst, uint32_t hpdIdx,
                     uint32_t addr, uint8_t *out, size_t len) {
	if (!out || len == 0u) return false;
	Session s {};
	s.io = io;
	s.inst = auxInst;
	s.hpd = hpdIdx;
	if (auxBegin(s, 100000u) != kOk) {
		auxEnd(s);
		return false;
	}
	bool ok = true;
	for (size_t pos = 0u; pos < len; ) {
		const uint8_t chunk = (len - pos > 16u) ? 16u : static_cast<uint8_t>(len - pos);
		if (!readDpcdBytes(s, addr + static_cast<uint32_t>(pos), out + pos, chunk)) {
			ok = false;
			break;
		}
		pos += chunk;
	}
	auxEnd(s);
	return ok;
}

inline bool dpcdWrite(const Io &io, uint32_t auxInst, uint32_t hpdIdx,
                      uint32_t addr, const uint8_t *data, size_t len) {
	if (!data || len == 0u) return false;
	Session s {};
	s.io = io;
	s.inst = auxInst;
	s.hpd = hpdIdx;
	if (auxBegin(s, 100000u) != kOk) {
		auxEnd(s);
		return false;
	}
	bool ok = true;
	for (size_t pos = 0u; pos < len; ) {
		const uint8_t chunk = (len - pos > 16u) ? 16u : static_cast<uint8_t>(len - pos);
		if (!writeDpcdBytes(s, addr + static_cast<uint32_t>(pos), data + pos, chunk)) {
			ok = false;
			break;
		}
		pos += chunk;
	}
	auxEnd(s);
	return ok;
}

inline uint32_t readDpcdCaps(const Io &io, uint32_t auxInst, uint32_t hpdIdx, DpcdCaps *out) {
	if (!out) return kInvalid;
	*out = DpcdCaps {};

	Session s {};
	s.io = io;
	s.inst = auxInst;
	s.hpd = hpdIdx;
	const uint32_t beginRc = auxBegin(s, 100000u);
	if (beginRc != kOk) {
		auxEnd(s);
		return beginRc;
	}

	uint8_t rev[16] {};
	if (!readDpcdBytes(s, 0x0000u, rev, 16u)) {
		auxEnd(s);
		return s.lastFault ? s.lastFault : kIoError;
	}
	out->valid = true;
	out->dpcdRev = rev[0];
	out->maxLinkRate = rev[1];
	out->maxLaneCount = rev[2] & 0x1fu;
	out->tps3Supported = (rev[2] & 0x40u) != 0u;
	out->enhancedFraming = (rev[2] & 0x80u) != 0u;
	out->maxDownspread = rev[3];
	out->tps4Supported = (rev[3] & 0x80u) != 0u;
	out->codingCap = rev[6];
	out->trainingAuxRdInt = rev[14] & 0x7fu;
	out->extCapPresent = (rev[14] & 0x80u) != 0u ? 1u : 0u;

	uint8_t fec = 0u;
	if (readDpcdBytes(s, 0x0090u, &fec, 1u)) {
		out->fecCap = fec;
	}

	uint8_t cfg[16] {};
	if (readDpcdBytes(s, 0x0100u, cfg, 16u)) {
		out->curLinkBwSet = cfg[0];
		out->curLaneCountSet = cfg[1] & 0x1fu;
		out->curCodingSet = cfg[8];
	}
	uint8_t dscEn = 0u;
	if (readDpcdBytes(s, 0x0160u, &dscEn, 1u)) {
		out->curDscEnable = dscEn;
	}
	(void)readDpcdBytes(s, 0x0202u, out->laneStatus, 6u);

	uint8_t dsc[16] {};
	if (readDpcdBytes(s, 0x0060u, dsc, 16u)) {
		out->dscSupported = (dsc[0] & 1u) != 0u;
		out->dscRev = dsc[1];
		out->dscRcBufBlkSize = dsc[2];
		out->dscRcBufSize = dsc[3];
		out->dscSliceCap1 = dsc[4];
		out->dscLineBufDepth = dsc[5];
		out->dscBlockPred = dsc[6];
		out->dscColorFormatCap = dsc[9];
		out->dscColorDepthCap = dsc[10];
		out->dscPeakThroughput0 = dsc[11];
		out->dscMaxSliceWidth = dsc[12];
		out->dscSliceCap2 = dsc[13];
		out->dscMinBppInc = dsc[15];
	}

	if (out->extCapPresent) {
		uint8_t ext[16] {};
		if (readDpcdBytes(s, 0x2200u, ext, 16u)) {
			out->extMaxLinkRate = ext[1];
			out->extMaxLaneCount = ext[2] & 0x1fu;
		}
		uint8_t uhbr = 0u;
		if (readDpcdBytes(s, 0x2215u, &uhbr, 1u)) {
			out->uhbrValid = true;
			out->uhbrRates = uhbr;
		}
	}

	auxEnd(s);
	return kOk;
}

// Extract ASCII monitor name from EDID 0xFC descriptor for logging.
inline void extractMonitorName(const uint8_t *edid, size_t len, char out[14]) {
	out[0] = '\0';
	if (!edid || len < rdna4edidmodes::BlockSize || !rdna4edidmodes::baseBlockValid(edid)) return;
	for (size_t i = 0u; i < 4u; ++i) {
		const uint8_t *d = edid + 54u + i * 18u;
		if (d[0] == 0u && d[1] == 0u && d[2] == 0u && d[3] == 0xfcu && d[4] == 0u) {
			size_t n = 0u;
			for (size_t j = 0u; j < 13u; ++j) {
				const uint8_t c = d[5u + j];
				if (c == 0x0au || c == 0x00u) break;
				out[n++] = (c >= 0x20u && c <= 0x7eu) ? static_cast<char>(c) : '?';
			}
			while (n > 0u && out[n - 1u] == ' ') --n;
			out[n] = '\0';
			return;
		}
	}
}

}  // namespace rdna4aux
