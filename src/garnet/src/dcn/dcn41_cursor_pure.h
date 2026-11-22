//
//  dcn41_cursor_pure.h — Pure DCN 4.01 128x128 hardware cursor helpers for Garnet.
//
//  Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (src/cursor.cpp,
//  commits 95b0b43a and 52af5417), Copyright (c) 2026 Sunneva N. Mariu,
//  licensed under the BSD 3-Clause License (see third-party/THIRD-PARTY-LICENSES.txt).
//
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdna4cursor {

constexpr uint32_t kCursorMaxDim    = 128u;
constexpr uint32_t kCursorPitch     = 128u;
constexpr uint32_t kCursorPixels    = kCursorMaxDim * kCursorMaxDim;
constexpr uint32_t kCursorSlotBytes = kCursorPixels * 4u; // 64 KiB fixed 128x128 ARGB8888 slot
constexpr uint32_t kCursorSlotAlign = 65536u;
constexpr uint32_t kCursorMaxPipes  = 4u;

// DMU seg2 dword bases & instance strides (all within src/dcn41/dcn41_allow_ranges.h).
constexpr uint32_t kSeg2HubpStride            = 0x00dcu;
constexpr uint32_t kSeg2HubpMallConfig        = 0x05f7u; // DCHUBP_MALL_CONFIG (bit 2 USE_MALL_FOR_CURSOR)
constexpr uint32_t kSeg2HubpPrimaryAddr       = 0x060au; // DCSURF_PRIMARY_SURFACE_ADDRESS
constexpr uint32_t kSeg2HubpPrimaryAddrHigh   = 0x060bu; // DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH
constexpr uint32_t kSeg2HubpCursorSettings    = 0x0653u; // CURSOR_SETTINGS (CHUNK_HDL_ADJUST bits 9:8)
constexpr uint32_t kSeg2HubpCursorControl     = 0x0679u; // CURSOR0_CURSOR_CONTROL
constexpr uint32_t kSeg2HubpCursorAddr        = 0x067au; // CURSOR0_CURSOR_SURFACE_ADDRESS
constexpr uint32_t kSeg2HubpCursorAddrHigh    = 0x067bu; // CURSOR0_CURSOR_SURFACE_ADDRESS_HIGH
constexpr uint32_t kSeg2HubpCursorSize        = 0x067cu; // CURSOR0_CURSOR_SIZE
constexpr uint32_t kSeg2HubpCursorPosition    = 0x067du; // CURSOR0_CURSOR_POSITION
constexpr uint32_t kSeg2HubpCursorHotSpot     = 0x067eu; // CURSOR0_CURSOR_HOT_SPOT
constexpr uint32_t kSeg2HubpCursorDstOffset   = 0x0680u; // CURSOR0_CURSOR_DST_OFFSET

constexpr uint32_t kSeg2DppStride             = 0x016bu;
constexpr uint32_t kSeg2DppCursorCmControl    = 0x0cf1u; // CM_CUR0_CONTROL
constexpr uint32_t kSeg2DppCursorCmScaleGY    = 0x0cf4u; // CURSOR0_COLOR_KEYER / FP16 scale G/Y
constexpr uint32_t kSeg2DppCursorCmScaleRB    = 0x0cf5u; // CURSOR0 FP16 scale R/B
constexpr uint32_t kSeg2DppCursorCmMatrix     = 0x0cf6u; // CURSOR0 CM matrix / bias mode

constexpr uint32_t kSeg2OtgStride             = 0x0080u;
constexpr uint32_t kSeg2OtgMasterUpdateLock   = 0x1b89u; // OTG0_OTG_MASTER_UPDATE_LOCK

constexpr uint32_t kMallUseForCursorBit       = (1u << 2);
constexpr uint32_t kCursorSettingsDefault     = (3u << 8); // CHUNK_HDL_ADJUST = 3
constexpr uint32_t kFp16One                   = 0x3c00u;

struct CursorSlotPlan {
	uint64_t slotOffset;
	uint64_t physAddr;
	uint64_t mcAddr;
	uint32_t sizeBytes;
};

struct EncodedPos {
	uint32_t px;
	uint32_t py;
	uint32_t hx;
	uint32_t hy;
	uint32_t posReg;
	uint32_t hotReg;
	uint32_t dstOffset;
};

struct CursorPlaneConfig {
	uint32_t pipe;
	uint64_t mcAddr;
	uint32_t width;
	uint32_t height;
	int32_t  x;
	int32_t  y;
	uint32_t hotX;
	uint32_t hotY;
	uint32_t pixClkKhz;
	bool     enabled;
};

struct CursorHw {
	void *cookie;
	uint32_t (*rdSeg2)(void *cookie, uint32_t seg2Offset);
	bool     (*wrSeg2)(void *cookie, uint32_t seg2Offset, uint32_t value);
};

inline uint32_t hubp_reg(uint32_t pipe, uint32_t baseOff) {
	return baseOff + pipe * kSeg2HubpStride;
}

inline uint32_t dpp_reg(uint32_t pipe, uint32_t baseOff) {
	return baseOff + pipe * kSeg2DppStride;
}

inline uint32_t otg_reg(uint32_t pipe, uint32_t baseOff) {
	return baseOff + pipe * kSeg2OtgStride;
}

inline uint32_t cursor_control_word(bool enabled) {
	// bit 0: CURSOR_ENABLE, bit 2: CURSOR_REQ_MODE=1, bits 9:8: MODE=2 (ARGB premult),
	// bits 17:16: PITCH=1 (128 px), bits 25:24: LINES_PER_CHUNK=2 (4 lines/chunk).
	return (enabled ? 1u : 0u) | (1u << 2) | (2u << 8) | (1u << 16) | (2u << 24);
}

inline uint32_t cursor_cm_control_word(bool enabled) {
	// bit 0: CUR0_EN, bit 2: CUR0_EXPANSION_MODE, bits 5:4: CUR0_ROM_EN/MODE=2, bit 7: CUR0_PRE_MULT_ALPHA=1.
	return (enabled ? 1u : 0u) | (1u << 2) | (2u << 4) | (1u << 7);
}

inline uint32_t premultiply_argb(uint32_t argb) {
	const uint32_t a = (argb >> 24) & 0xffu;
	if (a == 0u) return 0u;
	if (a == 255u) return argb;
	const uint32_t r = (((argb >> 16) & 0xffu) * a + 127u) / 255u;
	const uint32_t g = (((argb >> 8) & 0xffu) * a + 127u) / 255u;
	const uint32_t b = ((argb & 0xffu) * a + 127u) / 255u;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

inline void pack_cursor_128x128(const uint32_t *src, uint32_t srcW, uint32_t srcH, uint32_t *dst) {
	if (!dst) return;
	for (uint32_t i = 0u; i < kCursorPixels; ++i) dst[i] = 0u;
	if (!src || srcW == 0u || srcH == 0u) return;
	const uint32_t copyW = (srcW < kCursorMaxDim) ? srcW : kCursorMaxDim;
	const uint32_t copyH = (srcH < kCursorMaxDim) ? srcH : kCursorMaxDim;
	for (uint32_t y = 0u; y < copyH; ++y) {
		const uint32_t *srcRow = src + static_cast<size_t>(y) * srcW;
		uint32_t *dstRow = dst + static_cast<size_t>(y) * kCursorPitch;
		for (uint32_t x = 0u; x < copyW; ++x) {
			dstRow[x] = premultiply_argb(srcRow[x]);
		}
	}
}

inline bool plan_cursor_slot(uint64_t bar0Phys, uint64_t bar0Size,
                             uint64_t consolePhys, uint64_t consoleLen,
                             uint64_t hubpPrimaryMc, CursorSlotPlan *out) {
	if (!out) return false;
	*out = CursorSlotPlan {};
	if (!bar0Phys || bar0Size < kCursorSlotBytes || !consolePhys || consoleLen < 4096ull) {
		return false;
	}
	if (bar0Size > UINT64_MAX - bar0Phys) return false;
	if (consolePhys < bar0Phys || consoleLen > bar0Size) return false;
	const uint64_t consoleOff = consolePhys - bar0Phys;
	if (consoleOff > bar0Size - consoleLen) return false;
	if (hubpPrimaryMc < consoleOff) return false;

	const uint64_t consoleEnd = consoleOff + consoleLen;
	const uint64_t rem = consoleEnd & (kCursorSlotAlign - 1ull);
	const uint64_t pad = rem ? (kCursorSlotAlign - rem) : 0ull;
	if (pad > bar0Size - consoleEnd) return false;

	const uint64_t slotOff = consoleEnd + pad;
	if (kCursorSlotBytes > bar0Size - slotOff) return false;

	const uint64_t fbMcBase = hubpPrimaryMc - consoleOff;
	if (bar0Phys > UINT64_MAX - slotOff || fbMcBase > UINT64_MAX - slotOff) return false;
	if (bar0Phys + slotOff > UINT64_MAX - kCursorSlotBytes ||
	    fbMcBase + slotOff > UINT64_MAX - kCursorSlotBytes) return false;

	out->slotOffset = slotOff;
	out->physAddr = bar0Phys + slotOff;
	out->mcAddr = fbMcBase + slotOff;
	out->sizeBytes = kCursorSlotBytes;
	return true;
}

inline void encode_cursor_position(int32_t x, int32_t y,
                                   uint32_t curW, uint32_t curH,
                                   uint32_t hotX, uint32_t hotY,
                                   uint32_t pixClkKhz, EncodedPos *out) {
	if (!out) return;
	const uint32_t w = (curW == 0u) ? 1u : ((curW > kCursorMaxDim) ? kCursorMaxDim : curW);
	const uint32_t h = (curH == 0u) ? 1u : ((curH > kCursorMaxDim) ? kCursorMaxDim : curH);

	const int64_t tlX = static_cast<int64_t>(x) - static_cast<int64_t>(hotX);
	const int64_t tlY = static_cast<int64_t>(y) - static_cast<int64_t>(hotY);

	uint32_t px = 0u;
	uint32_t hx = 0u;
	if (tlX >= 0) {
		px = (tlX > 0x7fffll) ? 0x7fffu : static_cast<uint32_t>(tlX);
	} else {
		const uint64_t mag = static_cast<uint64_t>(-tlX);
		hx = (mag > w - 1u) ? (w - 1u) : static_cast<uint32_t>(mag);
	}

	uint32_t py = 0u;
	uint32_t hy = 0u;
	if (tlY >= 0) {
		py = (tlY > 0x7fffll) ? 0x7fffu : static_cast<uint32_t>(tlY);
	} else {
		const uint64_t mag = static_cast<uint64_t>(-tlY);
		hy = (mag > h - 1u) ? (h - 1u) : static_cast<uint32_t>(mag);
	}

	out->px = px;
	out->py = py;
	out->hx = hx;
	out->hy = hy;
	out->posReg = (py & 0x7fffu) | ((px & 0x7fffu) << 15);
	out->hotReg = (hy & 0x7fu) | ((hx & 0x7fu) << 16);
	out->dstOffset = (pixClkKhz > 0u)
	                     ? static_cast<uint32_t>((static_cast<uint64_t>(px) * 50000ull) / pixClkKhz)
	                     : 0u;
}

inline bool program_cursor_plane(const CursorHw &hw, const CursorPlaneConfig &cfg) {
	if (!hw.rdSeg2 || !hw.wrSeg2) return false;
	if (cfg.pipe >= kCursorMaxPipes || cfg.mcAddr == 0u ||
	    cfg.width == 0u || cfg.height == 0u ||
	    cfg.width > kCursorMaxDim || cfg.height > kCursorMaxDim) {
		return false;
	}
	EncodedPos pos {};
	encode_cursor_position(cfg.x, cfg.y, cfg.width, cfg.height,
	                       cfg.hotX, cfg.hotY, cfg.pixClkKhz, &pos);

	const uint32_t lockReg = otg_reg(cfg.pipe, kSeg2OtgMasterUpdateLock);
	if (!hw.wrSeg2(hw.cookie, lockReg, 1u)) return false;

	bool ok = true;
	const uint32_t mallReg = hubp_reg(cfg.pipe, kSeg2HubpMallConfig);
	const uint32_t mallVal = hw.rdSeg2(hw.cookie, mallReg);
	ok = ok && hw.wrSeg2(hw.cookie, mallReg, mallVal & ~kMallUseForCursorBit);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorSettings), kCursorSettingsDefault);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorAddr),
	                     static_cast<uint32_t>(cfg.mcAddr & 0xffffffffu));
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorAddrHigh),
	                     static_cast<uint32_t>(cfg.mcAddr >> 32));
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorSize),
	                     ((cfg.height - 1u) & 0xffffu) | (((cfg.width - 1u) & 0xffffu) << 16));
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorPosition), pos.posReg);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorHotSpot), pos.hotReg);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorDstOffset), pos.dstOffset);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorControl),
	                     cursor_control_word(cfg.enabled));
	ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(cfg.pipe, kSeg2DppCursorCmScaleGY), kFp16One);
	ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(cfg.pipe, kSeg2DppCursorCmScaleRB), kFp16One);
	ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(cfg.pipe, kSeg2DppCursorCmMatrix), 0u);
	ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(cfg.pipe, kSeg2DppCursorCmControl),
	                     cursor_cm_control_word(cfg.enabled));
	ok = hw.wrSeg2(hw.cookie, lockReg, 0u) && ok;
	return ok;
}

inline bool move_cursor_plane(const CursorHw &hw, const CursorPlaneConfig &cfg, bool visibilityChanged) {
	if (!hw.wrSeg2) return false;
	if (cfg.pipe >= kCursorMaxPipes || cfg.width == 0u || cfg.height == 0u ||
	    cfg.width > kCursorMaxDim || cfg.height > kCursorMaxDim) {
		return false;
	}
	EncodedPos pos {};
	encode_cursor_position(cfg.x, cfg.y, cfg.width, cfg.height,
	                       cfg.hotX, cfg.hotY, cfg.pixClkKhz, &pos);

	const uint32_t lockReg = otg_reg(cfg.pipe, kSeg2OtgMasterUpdateLock);
	if (!hw.wrSeg2(hw.cookie, lockReg, 1u)) return false;

	bool ok = true;
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorPosition), pos.posReg);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorHotSpot), pos.hotReg);
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorDstOffset), pos.dstOffset);
	if (visibilityChanged) {
		ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(cfg.pipe, kSeg2HubpCursorControl),
		                     cursor_control_word(cfg.enabled));
		ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(cfg.pipe, kSeg2DppCursorCmControl),
		                     cursor_cm_control_word(cfg.enabled));
	}
	ok = hw.wrSeg2(hw.cookie, lockReg, 0u) && ok;
	return ok;
}

inline bool disable_cursor_plane(const CursorHw &hw, uint32_t pipe) {
	if (!hw.wrSeg2 || pipe >= kCursorMaxPipes) return false;
	const uint32_t lockReg = otg_reg(pipe, kSeg2OtgMasterUpdateLock);
	if (!hw.wrSeg2(hw.cookie, lockReg, 1u)) return false;
	bool ok = true;
	ok = ok && hw.wrSeg2(hw.cookie, hubp_reg(pipe, kSeg2HubpCursorControl),
	                     cursor_control_word(false));
	ok = ok && hw.wrSeg2(hw.cookie, dpp_reg(pipe, kSeg2DppCursorCmControl),
	                     cursor_cm_control_word(false));
	ok = hw.wrSeg2(hw.cookie, lockReg, 0u) && ok;
	return ok;
}

} // namespace rdna4cursor
