// edid_modes_test.cpp — Host tests for EDID 1.4 / CTA-861 / DisplayID mode extraction.
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "dcn/edid_modes_pure.h"

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

void write_dtd(uint8_t *dtd, uint32_t pixClockKhz, uint32_t hActive, uint32_t hBlank,
               uint32_t hSyncOff, uint32_t hSyncW, uint32_t vActive, uint32_t vBlank,
               uint32_t vSyncOff, uint32_t vSyncW, bool hPos, bool vPos) {
	const uint16_t clk10 = static_cast<uint16_t>(pixClockKhz / 10u);
	dtd[0] = static_cast<uint8_t>(clk10 & 0xffu);
	dtd[1] = static_cast<uint8_t>((clk10 >> 8) & 0xffu);
	dtd[2] = static_cast<uint8_t>(hActive & 0xffu);
	dtd[3] = static_cast<uint8_t>(hBlank & 0xffu);
	dtd[4] = static_cast<uint8_t>(((hActive >> 8) << 4) | ((hBlank >> 8) & 0xfu));
	dtd[5] = static_cast<uint8_t>(vActive & 0xffu);
	dtd[6] = static_cast<uint8_t>(vBlank & 0xffu);
	dtd[7] = static_cast<uint8_t>(((vActive >> 8) << 4) | ((vBlank >> 8) & 0xfu));
	dtd[8] = static_cast<uint8_t>(hSyncOff & 0xffu);
	dtd[9] = static_cast<uint8_t>(hSyncW & 0xffu);
	dtd[10] = static_cast<uint8_t>(((vSyncOff & 0xfu) << 4) | (vSyncW & 0xfu));
	dtd[11] = static_cast<uint8_t>((((hSyncOff >> 8) & 0x3u) << 6) |
	                               (((hSyncW >> 8) & 0x3u) << 4) |
	                               (((vSyncOff >> 4) & 0x3u) << 2) |
	                               ((vSyncW >> 4) & 0x3u));
	dtd[17] = static_cast<uint8_t>(0x18u | (vPos ? 0x04u : 0u) | (hPos ? 0x02u : 0u));
}

void make_base_edid(uint8_t edid[256], uint8_t extCount) {
	std::memset(edid, 0, 256);
	static constexpr uint8_t kHdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	std::memcpy(edid, kHdr, 8);
	edid[18] = 1;
	edid[19] = 4;
	// DTD #0: 2560x1440@60 (241.5 MHz, 2720x1481)
	write_dtd(edid + 54, 241500u, 2560u, 160u, 48u, 32u, 1440u, 41u, 3u, 5u, true, false);
	// DTD #1: 1920x1080@60 (148.5 MHz, 2200x1125)
	write_dtd(edid + 72, 148500u, 1920u, 280u, 88u, 44u, 1080u, 45u, 4u, 5u, true, true);
	edid[126] = extCount;
	fix_checksum(edid);
}

bool contains_mode(const rdna4edidmodes::ModeList &list, uint32_t w, uint32_t h) {
	for (size_t i = 0u; i < list.count; ++i) {
		if (list.modes[i].timing.width == w && list.modes[i].timing.height == h) return true;
	}
	return false;
}

}  // namespace

int main() {
	// Null / short / corrupt EDID falls back cleanly to console geometry.
	{
		rdna4edidmodes::ModeList list {};
		const size_t n = rdna4edidmodes::build(nullptr, 0u, 2560u, 1440u, 10240u, list);
		expect_u("null EDID yields single console fallback mode", n, 1u);
		expect_u("fallback mode width", list.modes[0].timing.width, 2560u);
		expect_u("fallback mode height", list.modes[0].timing.height, 1440u);
		expect_u("fallback refresh1616 == 60Hz", list.modes[0].timing.refresh1616(), 60u << 16);
	}

	// Corrupt checksum fails baseBlockValid and triggers console fallback.
	{
		uint8_t bad[256] {};
		make_base_edid(bad, 0u);
		bad[127] ^= 0x01u;
		expect(!rdna4edidmodes::baseBlockValid(bad), "bad checksum rejected");
		rdna4edidmodes::ModeList list {};
		const size_t n = rdna4edidmodes::build(bad, 128u, 1920u, 1080u, 7680u, list);
		expect_u("bad checksum falls back to console mode", n, 1u);
		expect_u("bad checksum fallback width", list.modes[0].timing.width, 1920u);
	}

	// Base EDID 1.4 DTDs + CTA-861 DTD extraction + console bounds enforcement.
	{
		uint8_t edid[256] {};
		make_base_edid(edid, 1u);
		edid[128] = 0x02u;
		edid[129] = 0x03u;
		edid[130] = 4u;  // DTD start
		// CTA DTD #0: 1280x720@60 (fits inside 2560x1440 console)
		write_dtd(edid + 132, 74250u, 1280u, 370u, 110u, 40u, 720u, 30u, 5u, 5u, true, true);
		// CTA DTD #1: 3840x2160@60 (exceeds 2560x1440 console -> filtered out)
		write_dtd(edid + 150, 533250u, 3840u, 160u, 48u, 32u, 2160u, 62u, 3u, 5u, true, false);
		fix_checksum(edid + 128);

		rdna4edidmodes::ModeList list {};
		const size_t n = rdna4edidmodes::build(edid, 256u, 2560u, 1440u, 10240u, list);
		expect_u("EDID + CTA yields 3 bounded DTD modes", n, 3u);
		expect(contains_mode(list, 2560u, 1440u), "contains 2560x1440 preferred DTD");
		expect(contains_mode(list, 1920u, 1080u), "contains 1920x1080 base DTD #1");
		expect(contains_mode(list, 1280u, 720u), "contains 1280x720 CTA DTD");
		expect(!contains_mode(list, 3840u, 2160u), "3840x2160 exceeding console bounds filtered out");
		expect(list.modes[0].timing.preferred, "first DTD marked preferred");
		expect_u("2560x1440 hActive", list.modes[0].timing.width, 2560u);
		expect_u("2560x1440 hBlank", list.modes[0].timing.hBlank, 160u);
		expect(list.modes[0].timing.hPositive && !list.modes[0].timing.vPositive,
		       "2560x1440 sync polarities preserved");
	}

	// DisplayID 1.2/1.3 extension block Type I detailed timing (1440x900@60).
	{
		uint8_t edid[256] {};
		make_base_edid(edid, 1u);
		uint8_t *did = edid + 128;
		did[0] = 0x70u;  // DisplayID extension tag
		did[1] = 0x12u;  // version 1.2
		did[2] = 23u;    // payload bytes (end = 5 + 23 = 28)
		did[3] = 0x00u;  // display product type
		did[4] = 0x00u;  // extension count
		// Data block: Type I timing (tag=0x03, rev=0, len=20)
		did[5] = 0x03u;
		did[6] = 0x00u;
		did[7] = 20u;
		uint8_t *t = did + 8;
		const uint32_t clkMinus1 = 10649u;  // 106.50 MHz
		t[0] = static_cast<uint8_t>(clkMinus1 & 0xffu);
		t[1] = static_cast<uint8_t>((clkMinus1 >> 8) & 0xffu);
		t[2] = static_cast<uint8_t>((clkMinus1 >> 16) & 0xffu);
		t[3] = 0x80u;  // preferred
		auto put16m1 = [](uint8_t *p, uint16_t v) {
			const uint16_t m = static_cast<uint16_t>(v - 1u);
			p[0] = static_cast<uint8_t>(m & 0xffu);
			p[1] = static_cast<uint8_t>(m >> 8);
		};
		put16m1(t + 4, 1440u);   // hActive
		put16m1(t + 6, 464u);    // hBlank
		put16m1(t + 8, 80u);     // hSyncOffset (+hPol in bit 15)
		t[9] |= 0x80u;
		put16m1(t + 10, 152u);   // hSyncWidth
		put16m1(t + 12, 900u);   // vActive
		put16m1(t + 14, 34u);    // vBlank
		put16m1(t + 16, 3u);     // vSyncOffset
		put16m1(t + 18, 6u);     // vSyncWidth
		// Inner DisplayID checksum at did[28] covering did[1..27] (so sum(did[1..28]) == 0 mod 256)
		uint32_t inner = 0u;
		for (size_t i = 1u; i < 28u; ++i) inner += did[i];
		did[28] = static_cast<uint8_t>(0u - inner);
		fix_checksum(did);

		rdna4edidmodes::ModeList list {};
		(void)rdna4edidmodes::build(edid, 256u, 2560u, 1440u, 10240u, list);
		expect(contains_mode(list, 1440u, 900u), "DisplayID Type I detailed timing extracted");
	}

	// DisplayID 2.0 extension block with Type VII rev 2 (Mi Monitor 3840x2160@160Hz 1408MHz) + zero padding.
	{
		uint8_t did[128] {};
		did[0] = 0x70u; // DisplayID extension tag
		did[1] = 0x20u; // version 2.0
		did[2] = 0x79u; // 121 payload bytes (end = 126, zero-padded to end!)
		did[3] = 0x05u;
		did[4] = 0x00u;
		// Type VII timing (tag=0x22, rev=0x02, len=20) — exact live Mi Monitor bytes
		static constexpr uint8_t kType7[23] = {
			0x22u, 0x02u, 0x14u,
			0xffu, 0x7bu, 0x15u, 0x04u,
			0xffu, 0x0eu, 0x9fu, 0x00u,
			0x2fu, 0x80u, 0x1fu, 0x00u,
			0x6fu, 0x08u, 0x27u, 0x00u,
			0x02u, 0x00u, 0x04u, 0x00u,
		};
		memcpy(did + 5, kType7, sizeof(kType7));
		uint32_t inner = 0u;
		for (size_t i = 1u; i < 126u; ++i) inner += did[i];
		did[126] = static_cast<uint8_t>(0u - inner);
		fix_checksum(did);

		rdna4edidmodes::DetailedTiming out[4] {};
		const size_t n = rdna4edidmodes::parseDisplayIdBlock(did, out, 4u);
		expect_u("DisplayID 2.0 zero-padded block extracts 1 timing", n, 1u);
		expect_u("DisplayID 2.0 4K width", out[0].width, 3840u);
		expect_u("DisplayID 2.0 4K height", out[0].height, 2160u);
		expect_u("DisplayID 2.0 4K@160Hz exact mHz", out[0].refreshMilliHz(), 160000u);
		expect(out[0].pixelClockHz == 1408000000ULL, "DisplayID 2.0 1408 MHz pixel clock");
	}

	std::printf("edid_modes_test: %d checks, %d failed\n", gRun, gFail);
	return gFail ? 1 : 0;
}
