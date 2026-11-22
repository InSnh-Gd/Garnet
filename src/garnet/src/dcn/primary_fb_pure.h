// primary_fb_pure.h — pure mode, pixel-format, DDC, and aperture helpers for GarnetPrimaryFramebuffer.
// Header-only, C++17, no kernel/IOKit headers, no floating point or 128-bit division.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display_state_pure.h"
#include "edid_modes_pure.h"

namespace rdna4fbprimary {

struct ModeTable {
	rdna4edidmodes::ModeList list;
	uint32_t activeModeId;
	uint32_t consoleWidth;
	uint32_t consoleHeight;
	uint32_t consoleRowBytes;
	bool fromEdid;
};

struct ModeInfoSummary {
	uint32_t nominalWidth;
	uint32_t nominalHeight;
	uint32_t refreshFixed16_16;
	uint32_t maxDepthIndex;
	uint32_t flags;
};

struct PixelSpec {
	uint32_t bytesPerRow;
	uint32_t bytesPerPlane;
	uint32_t bitsPerPixel;
	uint32_t pixelType;
	uint32_t componentCount;
	uint32_t bitsPerComponent;
	uint32_t componentMasks[3];
	char pixelFormat[64];
	uint32_t activeWidth;
	uint32_t activeHeight;
};

struct TimingSpec {
	uint64_t pixelClockHz;
	uint32_t hActive;
	uint32_t hBlank;
	uint32_t hSyncOffset;
	uint32_t hSyncWidth;
	uint32_t vActive;
	uint32_t vBlank;
	uint32_t vSyncOffset;
	uint32_t vSyncWidth;
	bool hPositive;
	bool vPositive;
};

inline uint64_t consolePhysicalAddress(uint64_t baseAddress) {
	return baseAddress & ~3ull;
}

inline bool validateApertureRange(uint64_t bar0Phys, uint64_t bar0Size,
                                  uint64_t consolePhys, uint64_t consoleLen) {
	if (!bar0Phys || !bar0Size || !consolePhys || consoleLen < 4096ull) return false;
	if (bar0Size > ~0ull - bar0Phys) return false;
	if (consoleLen > bar0Size) return false;
	if (consolePhys < bar0Phys) return false;
	return (consolePhys - bar0Phys) <= (bar0Size - consoleLen);
}

inline bool primaryMatchesSnapshot(uint64_t bar0Phys, uint64_t bar0Size,
                                   uint64_t consolePhys, uint64_t consoleLen,
                                   const rdna4peer::Snapshot &snap) {
	if (!validateApertureRange(bar0Phys, bar0Size, consolePhys, consoleLen)) return false;
	if (snap.state != rdna4peer::kStateReady) return false;
	if (bar0Phys != snap.bar0Phys || bar0Size != snap.bar0Size) return false;
	if (consolePhys != snap.consolePhysBase || consoleLen != snap.console.length) return false;
	if (snap.vramLimit == 0ull || snap.vramLimit > snap.bar0Size || snap.vramLimit > snap.vramBytes) return false;
	const uint64_t off = consolePhys - bar0Phys;
	return off == snap.console.offset && off <= snap.vramLimit && consoleLen <= snap.vramLimit - off;
}

inline void buildModeTable(const uint8_t *edidBytes, size_t edidLen,
                           uint32_t consoleW, uint32_t consoleH,
                           uint32_t consoleRowBytes, ModeTable *out,
                           uint64_t maxClockHz = rdna4edidmodes::MaxPixelClockHz,
                           bool sameSizeOnly = false,
                           bool synthesizeRefreshSteps = false) {
	if (!out) return;
	*out = ModeTable {};
	out->consoleWidth = consoleW ? consoleW : 1920u;
	out->consoleHeight = consoleH ? consoleH : 1080u;
	out->consoleRowBytes = (consoleRowBytes >= out->consoleWidth * 4u)
	                           ? consoleRowBytes
	                           : (out->consoleWidth * 4u);

	const size_t n = rdna4edidmodes::build(edidBytes, edidLen,
	                                       out->consoleWidth, out->consoleHeight,
	                                       out->consoleRowBytes, out->list, maxClockHz,
	                                       sameSizeOnly, synthesizeRefreshSteps);
	if (n == 0u) {
		out->list.modes[0].modeId = 1u;
		out->list.modes[0].timing =
		    rdna4edidmodes::makeConsoleTiming(out->consoleWidth, out->consoleHeight, 60u);
		out->list.count = 1u;
	}

	out->fromEdid = (edidBytes != nullptr && edidLen >= rdna4edidmodes::BlockSize &&
	                 rdna4edidmodes::baseBlockValid(edidBytes));
	out->activeModeId = out->list.modes[0].modeId;

	// Prefer the boot ~60 Hz mode matching the console geometry so initial fCurrentMode matches GOP raster.
	uint32_t bestDist = 0xffffffffu;
	for (size_t i = 0u; i < out->list.count; ++i) {
		const rdna4edidmodes::Mode &m = out->list.modes[i];
		if (m.timing.width == out->consoleWidth && m.timing.height == out->consoleHeight) {
			const uint32_t r = m.timing.refreshMilliHz();
			const uint32_t dist = (r >= 60000u) ? (r - 60000u) : (60000u - r);
			if (dist < bestDist) {
				bestDist = dist;
				out->activeModeId = m.modeId;
			}
		}
	}
}

inline bool modeForId(const ModeTable &t, int32_t modeId,
                      rdna4edidmodes::DetailedTiming *out,
                      uint32_t *outIdx = nullptr) {
	if (modeId < 1) return false;
	for (size_t i = 0u; i < t.list.count; ++i) {
		if (t.list.modes[i].modeId == static_cast<uint32_t>(modeId)) {
			if (out) *out = t.list.modes[i].timing;
			if (outIdx) *outIdx = static_cast<uint32_t>(i);
			return true;
		}
	}
	return false;
}

inline bool modeInfoForId(const ModeTable &t, int32_t modeId, ModeInfoSummary *out) {
	if (!out) return false;
	rdna4edidmodes::DetailedTiming m {};
	uint32_t idx = 0u;
	if (!modeForId(t, modeId, &m, &idx)) return false;

	*out = ModeInfoSummary {};
	out->nominalWidth = m.width;
	out->nominalHeight = m.height;
	out->refreshFixed16_16 = m.refresh1616();
	out->maxDepthIndex = 0u;
	// Valid | Safe (0x3); Default (0x4) only on activeModeId; Native (0x02000000) on preferred / primary console match.
	out->flags = 0x3u;
	if (static_cast<uint32_t>(modeId) == t.activeModeId) {
		out->flags |= 0x4u;
	}
	if (m.preferred || (t.fromEdid && idx == 0u) ||
	    (m.width == t.consoleWidth && m.height == t.consoleHeight)) {
		out->flags |= 0x02000000u;
	}
	return true;
}

inline bool pixelSpecForMode(const ModeTable &t, int32_t modeId, int32_t depth,
                             bool isSystemAperture, PixelSpec *out) {
	if (!out || depth != 0 || !isSystemAperture) return false;
	rdna4edidmodes::DetailedTiming m {};
	if (!modeForId(t, modeId, &m)) return false;

	*out = PixelSpec {};
	const uint32_t minBytes = m.width * 4u;
	if (m.width == t.consoleWidth && m.height == t.consoleHeight &&
	    t.consoleRowBytes >= minBytes) {
		out->bytesPerRow = t.consoleRowBytes;
	} else {
		out->bytesPerRow = (minBytes + 255u) & ~255u;
	}
	out->bytesPerPlane = out->bytesPerRow * m.height;
	out->bitsPerPixel = 32u;
	out->pixelType = 2u;  // kIORGBDirectPixels
	out->componentCount = 3u;
	out->bitsPerComponent = 8u;
	out->componentMasks[0] = 0x00FF0000u;
	out->componentMasks[1] = 0x0000FF00u;
	out->componentMasks[2] = 0x000000FFu;
	static constexpr char kFmt[] = "--------RRRRRRRRGGGGGGGGBBBBBBBB";
	memcpy(out->pixelFormat, kFmt, sizeof(kFmt));
	out->activeWidth = m.width;
	out->activeHeight = m.height;
	return true;
}

inline bool timingSpecForMode(const ModeTable &t, int32_t modeId, TimingSpec *out) {
	if (!out) return false;
	rdna4edidmodes::DetailedTiming m {};
	if (!modeForId(t, modeId, &m)) return false;

	*out = TimingSpec {};
	out->pixelClockHz = m.pixelClockHz;
	out->hActive = m.width;
	out->hBlank = m.hBlank;
	out->hSyncOffset = m.hSyncOffset;
	out->hSyncWidth = m.hSyncWidth;
	out->vActive = m.height;
	out->vBlank = m.vBlank;
	out->vSyncOffset = m.vSyncOffset;
	out->vSyncWidth = m.vSyncWidth;
	out->hPositive = m.hPositive;
	out->vPositive = m.vPositive;
	return true;
}

inline bool sliceDdcBlock(const uint8_t *edid, size_t edidLen, uint32_t blockNumber,
                          uint8_t out[rdna4edidmodes::BlockSize]) {
	if (!edid || !out || blockNumber == 0u) return false;
	if (!rdna4edidmodes::baseBlockValid(edid)) return false;
	const size_t offset = static_cast<size_t>(blockNumber - 1u) * rdna4edidmodes::BlockSize;
	if (offset > edidLen || (edidLen - offset) < rdna4edidmodes::BlockSize) return false;
	const uint8_t *blk = edid + offset;
	if (!rdna4edidmodes::blockChecksumOk(blk)) return false;
	memcpy(out, blk, rdna4edidmodes::BlockSize);
	return true;
}

inline rdna4peer::PrimaryView makePrimaryView(const rdna4peer::Snapshot &snap,
                                              uint64_t fbEntryId) {
	rdna4peer::PrimaryView p {};
	p.version = rdna4peer::kVersion;
	p.asic = snap.asic;
	p.vendor = snap.vendor;
	p.device = snap.device;
	p.generation = snap.generation;
	p.providerEntryID = snap.providerEntryID;
	p.fbEntryID = fbEntryId;
	p.bar0Phys = snap.bar0Phys;
	p.bar0Size = snap.bar0Size;
	p.consolePhysBase = snap.consolePhysBase;
	p.consoleVramOffset = snap.console.offset;
	p.consoleLength = snap.console.length;
	p.consoleWidth = snap.consoleWidth;
	p.consoleHeight = snap.consoleHeight;
	p.consoleRowBytes = snap.consoleRowBytes;
	p.consoleDepth = 32u;
	p.modeWidth = snap.consoleWidth;
	p.modeHeight = snap.consoleHeight;
	p.modeRowBytes = snap.consoleRowBytes;
	p.modeDepth = 32u;
	p.displayIndex = 0u;
	p.head = 0u;
	p.otg = 0u;
	return p;
}

}  // namespace rdna4fbprimary
