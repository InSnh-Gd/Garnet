// primary_fb_test.cpp — Host tests for AMDGarnetPrimaryFramebuffer pure helpers.
#include <cstdint>
#include <cstring>
#include <cstdio>

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

void fix_checksum(uint8_t blk[128]) {
	uint32_t sum = 0u;
	for (size_t i = 0u; i < 127u; ++i) sum += blk[i];
	blk[127] = static_cast<uint8_t>(0u - sum);
}

void make_sample_edid(uint8_t edid[256]) {
	std::memset(edid, 0, 256);
	static constexpr uint8_t kHdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	std::memcpy(edid, kHdr, 8);
	edid[18] = 1;
	edid[19] = 4;
	// DTD #0: 1920x1080@60 preferred (148.5 MHz, 2200x1125)
	uint8_t *d = edid + 54;
	const uint16_t clk10 = 14850u;
	d[0] = static_cast<uint8_t>(clk10 & 0xffu);
	d[1] = static_cast<uint8_t>(clk10 >> 8);
	d[2] = 1920u & 0xffu;
	d[3] = 280u & 0xffu;
	d[4] = ((1920u >> 8) << 4) | (280u >> 8);
	d[5] = 1080u & 0xffu;
	d[6] = 45u & 0xffu;
	d[7] = ((1080u >> 8) << 4) | (45u >> 8);
	d[8] = 88u;
	d[9] = 44u;
	d[10] = (4u << 4) | 5u;
	d[17] = 0x1eu;
	// DTD #1: 1280x720@60 (74.25 MHz, 1650x750)
	uint8_t *d2 = edid + 72;
	const uint16_t clk720 = 7425u;
	d2[0] = static_cast<uint8_t>(clk720 & 0xffu);
	d2[1] = static_cast<uint8_t>(clk720 >> 8);
	d2[2] = 1280u & 0xffu;
	d2[3] = 370u & 0xffu;
	d2[4] = ((1280u >> 8) << 4) | (370u >> 8);
	d2[5] = 720u & 0xffu;
	d2[6] = 30u & 0xffu;
	d2[7] = ((720u >> 8) << 4) | (30u >> 8);
	d2[8] = 110u;
	d2[9] = 40u;
	d2[10] = (5u << 4) | 5u;
	d2[17] = 0x1eu;
	edid[126] = 1u;
	fix_checksum(edid);

	edid[128] = 0x02u;
	edid[129] = 0x03u;
	edid[130] = 4u;
	fix_checksum(edid + 128);
}

}  // namespace

int main() {
	using namespace rdna4fbprimary;

	expect_u("physical console flag is excluded from scanout address",
	         consolePhysicalAddress(0xa0000001ull), 0xa0000000ull);
	expect_u("both console address flag bits are excluded",
	         consolePhysicalAddress(0x840000003ull), 0x840000000ull);
	expect_u("unflagged console preserves its subpage address",
	         consolePhysicalAddress(0x840000100ull), 0x840000100ull);
	expect_u("flagged empty console has no physical address",
	         consolePhysicalAddress(1ull), 0ull);

	// 1. validateApertureRange checks containment and overflow safety.
	expect(validateApertureRange(0x100000000ull, 0x10000000ull, 0x100000000ull, 2560ull * 1440ull * 4ull),
	       "console scanout inside BAR0 is valid");
	expect(!validateApertureRange(0x100000000ull, 0x10000000ull, 0x0f0000000ull, 0x100000ull),
	       "console below BAR0 rejected");
	expect(!validateApertureRange(0x100000000ull, 0x01000000ull, 0x100800000ull, 0x01000000ull),
	       "console extending past BAR0 rejected");
	expect(!validateApertureRange(0x100000000ull, 0x10000000ull, 0x100000000ull, 0xfffffffffffffff0ull),
	       "overflowing console length rejected");
	expect(!validateApertureRange(0ull, 0x10000000ull, 0x100000000ull, 0x100000ull),
	       "zero BAR0 rejected");

	// 2. buildModeTable + pixel/timing helpers on console + EDID.
	uint8_t edid[256] {};
	make_sample_edid(edid);

	ModeTable tbl {};
	buildModeTable(edid, 256u, 1920u, 1080u, 8192u, &tbl);
	expect(tbl.fromEdid, "EDID mode table marked fromEdid");
	expect(tbl.list.count >= 2u, "EDID builds multiple modes");
	expect_u("active modeId matches console geometry", tbl.activeModeId, 1u);

	ModeInfoSummary mi {};
	expect(modeInfoForId(tbl, static_cast<int32_t>(tbl.activeModeId), &mi), "modeInfoForId succeeds");
	expect_u("modeInfo width", mi.nominalWidth, 1920u);
	expect_u("modeInfo height", mi.nominalHeight, 1080u);
	expect((mi.flags & 0x02000000u) != 0u, "preferred/console mode has native flag");
	expect(!modeInfoForId(tbl, 0, &mi) && !modeInfoForId(tbl, 999, &mi), "invalid mode IDs rejected");

	PixelSpec px {};
	expect(pixelSpecForMode(tbl, static_cast<int32_t>(tbl.activeModeId), 0, true, &px),
	       "pixelSpecForMode succeeds for console mode");
	expect_u("console mode preserves consoleRowBytes", px.bytesPerRow, 8192u);
	expect_u("console mode bitsPerPixel", px.bitsPerPixel, 32u);
	expect_u("console mode pixelType is kIORGBDirectPixels", px.pixelType, 2u);
	expect_u("red component mask", px.componentMasks[0], 0x00ff0000u);
	expect_u("green component mask", px.componentMasks[1], 0x0000ff00u);
	expect_u("blue component mask", px.componentMasks[2], 0x000000ffu);
	expect(std::strcmp(px.pixelFormat, "--------RRRRRRRRGGGGGGGGBBBBBBBB") == 0,
	       "pixel format string is IO32BitDirectPixels");
	expect(!pixelSpecForMode(tbl, static_cast<int32_t>(tbl.activeModeId), 1, true, &px),
	       "depth != 0 rejected");
	expect(!pixelSpecForMode(tbl, static_cast<int32_t>(tbl.activeModeId), 0, false, &px),
	       "non-system aperture rejected");

	// Non-console mode rounds pitch up to 256 bytes.
	uint32_t secondId = 0u;
	for (size_t i = 0u; i < tbl.list.count; ++i) {
		if (tbl.list.modes[i].timing.width == 1280u && tbl.list.modes[i].timing.height == 720u) {
			secondId = tbl.list.modes[i].modeId;
			break;
		}
	}
	expect(secondId != 0u, "found 1280x720 mode");
	expect(pixelSpecForMode(tbl, static_cast<int32_t>(secondId), 0, true, &px),
	       "pixelSpecForMode succeeds for 1280x720");
	expect_u("1280x720 rowBytes aligned to 256B", px.bytesPerRow, 5120u);

	TimingSpec ts {};
	expect(timingSpecForMode(tbl, static_cast<int32_t>(tbl.activeModeId), &ts),
	       "timingSpecForMode succeeds");
	expect_u("timing pixelClockHz", ts.pixelClockHz, 148500000ull);
	expect_u("timing hActive", ts.hActive, 1920u);
	expect_u("timing hBlank", ts.hBlank, 280u);
	expect(ts.hPositive && ts.vPositive, "timing polarity preserved");

	// 3. sliceDdcBlock validates 1-based block numbers, bounds, and checksums.
	uint8_t blk[128] {};
	expect(!sliceDdcBlock(edid, 256u, 0u, blk), "block 0 invalid");
	expect(sliceDdcBlock(edid, 256u, 1u, blk) && blk[0] == 0x00u && blk[1] == 0xffu,
	       "block 1 returns base EDID");
	expect(sliceDdcBlock(edid, 256u, 2u, blk) && blk[0] == 0x02u,
	       "block 2 returns CTA-861 block");
	expect(!sliceDdcBlock(edid, 256u, 3u, blk), "out-of-range block 3 rejected");
	edid[255] ^= 0xffu;
	expect(!sliceDdcBlock(edid, 256u, 2u, blk), "corrupt extension block checksum rejected");

	// 4. makePrimaryView populates a PrimaryView accepted by rdna4peer::validate_primary.
	rdna4peer::Snapshot snap {};
	snap.version = rdna4peer::kVersion;
	snap.state = rdna4peer::kStateReady;
	snap.asic = 2u;
	snap.vendor = 0x1002u;
	snap.device = 0x7590u;
	snap.generation = 3u;
	snap.providerEntryID = 0x100000300ull;
	snap.bar0Phys = 0x100000000ull;
	snap.bar0Size = 0x10000000ull;
	snap.consolePhysBase = 0x100000000ull;
	snap.console.offset = 0ull;
	snap.console.length = 8192ull * 1080ull;
	snap.consoleWidth = 1920u;
	snap.consoleHeight = 1080u;
	snap.consoleRowBytes = 8192u;
	snap.consoleDepth = 32u;
	const rdna4peer::PrimaryView pv = makePrimaryView(snap, 0x100000400ull);
	expect_u("makePrimaryView passes validate_primary",
	         rdna4peer::validate_primary(snap, pv), rdna4peer::kOk);

	// 5. High 64-bit 16 GiB BAR0 aperture & snapshot agreement checks (Task 3.3).
	{
		const uint64_t hiBar = 0x7C00000000ull;
		const uint64_t bar16G = 16ull << 30;
		const uint64_t vram16304M = 16304ull << 20;
		const uint64_t lim15472M = 15472ull << 20;
		const uint64_t consLen = 8192ull * 1080ull;

		expect(validateApertureRange(hiBar, bar16G, hiBar, consLen),
		       "16 GiB high BAR0 accepts console at offset 0");
		expect(!validateApertureRange(hiBar, bar16G, 0xD0000000ull, consLen),
		       "stale 32-bit console phys outside 64-bit BAR0 rejected");
		expect(!validateApertureRange(0xFFFFFFFFFFFF0000ull, 0x20000ull, 0xFFFFFFFFFFFF0000ull, 4096ull),
		       "wrapping bar0Phys + bar0Size rejected");

		rdna4peer::Snapshot hiSnap = snap;
		hiSnap.bar0Phys = hiBar;
		hiSnap.bar0Size = bar16G;
		hiSnap.vramBytes = vram16304M;
		hiSnap.vramLimit = lim15472M;
		hiSnap.consolePhysBase = hiBar;
		hiSnap.console.offset = 0ull;
		hiSnap.console.length = consLen;
		expect(primaryMatchesSnapshot(hiBar, bar16G, hiBar, consLen, hiSnap),
		       "primaryMatchesSnapshot accepts matching 64-bit 16 GiB snapshot");
		expect(!primaryMatchesSnapshot(0xD0000000ull, bar16G, hiBar, consLen, hiSnap),
		       "primaryMatchesSnapshot rejects BAR0 physical mismatch");
		expect(!primaryMatchesSnapshot(hiBar, 1ull << 30, hiBar, consLen, hiSnap),
		       "primaryMatchesSnapshot rejects truncated BAR0 size mismatch");
		expect(!primaryMatchesSnapshot(hiBar, bar16G, hiBar + 4096ull, consLen, hiSnap),
		       "primaryMatchesSnapshot rejects stale console physical address");
		rdna4peer::Snapshot badLim = hiSnap;
		badLim.console.offset = lim15472M;
		badLim.consolePhysBase = hiBar + lim15472M;
		expect(!primaryMatchesSnapshot(hiBar, bar16G, badLim.consolePhysBase, consLen, badLim),
		       "primaryMatchesSnapshot rejects console above vramLimit");
		rdna4peer::Snapshot notReady = hiSnap;
		notReady.state = rdna4peer::kStateStarting;
		expect(!primaryMatchesSnapshot(hiBar, bar16G, hiBar, consLen, notReady),
		       "primaryMatchesSnapshot rejects non-READY snapshot");
	}

	std::printf("primary_fb_test: %d checks, %d failed\n", gRun, gFail);
	return gFail ? 1 : 0;
}
