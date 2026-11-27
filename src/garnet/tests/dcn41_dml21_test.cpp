#include "dcn/dcn41_dml21_bridge.h"
#include "dml21-oracle/expected.h"
#include "dml21-oracle/fixture.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rdna4dml;

static void rejects(const Input &input) {
	Result result{};
	result.uclkPstateSupported = true;
	result.fams2Required = true;
	result.compbufSegments = 21;
	result.fams2Global[8] = 1;
	assert(!solve(input, result));
	assert(!result.uclkPstateSupported && !result.fams2Required);
	assert(!result.compbufSegments && !result.fams2Global[8]);
}

template <size_t Count> static ClockTable table(const uint32_t (&values)[Count]) {
	static_assert(Count <= kMaxClocks);
	ClockTable table{Count, {}};
	memcpy(table.kHz, values, sizeof(values));
	return table;
}

static Input makeInput(const fixture_soc &soc, const fixture_mode &mode) {
	Input input{};
	input.count = mode.count;
	for (uint32_t index = 0; index < input.count; ++index) {
		const auto &source = mode.streams[index];
		input.streams[index] = {source.h_active, source.h_total, source.h_front, source.h_sync,
		                        source.v_active, source.v_total, source.v_front, source.v_sync,
		                        source.pixel_khz, source.pitch_pixels,
		                        source.link == FIXTURE_DP_HBR3 ? Link::DpHbr3 : Link::HdmiTmds,
		                        static_cast<uint8_t>(source.bpc), static_cast<uint8_t>(source.lanes),
		                        static_cast<uint8_t>(source.dsc_slices), static_cast<uint16_t>(source.dsc_bpp_x16),
		                        static_cast<uint16_t>(source.cursor_width)};
	}
	input.enableFams2 = input.enableSubVp = true;
	input.soc.mallMiB = soc.mall_mib;
	input.soc.dramChannels = soc.dram_channels;
	input.soc.dramChannelWidthBytes = FIXTURE_DRAM_CHANNEL_WIDTH_BYTES;
	input.soc.dramTransactionsPerClock = FIXTURE_DRAM_TRANSACTIONS_PER_CLOCK;
	input.soc.uclk = table(fixture_uclk_khz);
	input.soc.fclk = table(fixture_fclk_khz);
	input.soc.dcfclk = table(fixture_dcfclk_khz);
	input.soc.dispclk = table(fixture_dispclk_khz);
	input.soc.dppclk = table(fixture_dppclk_khz);
	input.soc.dtbclk = table(fixture_dtbclk_khz);
	input.soc.socclk = table(fixture_socclk_khz);
	input.soc.uclkBlackoutNs = FIXTURE_UCLK_BLACKOUT_NS;
	input.soc.fclkBlackoutNs = FIXTURE_FCLK_BLACKOUT_NS;
	input.soc.stutterEnterExitNs = FIXTURE_STUTTER_ENTER_EXIT_NS;
	input.soc.stutterExitNs = FIXTURE_STUTTER_EXIT_NS;
	return input;
}

static void invalidInput() {
	const auto baseline = makeInput(fixture_socs[0], fixture_modes[0]);
	rejects({});
	auto input = baseline;
	input.count = kMaxStreams + 1;
	rejects(input);
	input = baseline;
	input.streams[0].hTotal = 3839;
	rejects(input);
	input = baseline;
	input.streams[0].vFront = UINT32_MAX;
	rejects(input);
	input = baseline;
	input.streams[0].pitchPixels = 3841;
	rejects(input);
	input = baseline;
	input.streams[0].dscSlices = 8;
	rejects(input);
	input = baseline;
	input.soc.fclk.count = kMaxClocks + 1;
	rejects(input);
	input = baseline;
	input.soc.uclk.kHz[1] = 96000;
	rejects(input);
	input = baseline;
	input.enableFams2 = false;
	rejects(input);
	input = baseline;
	input.soc.mallMiB = 64;
	rejects(input);
	input = makeInput(fixture_socs[0], fixture_modes[1]);
	input.streams[0].hActive = input.streams[0].dscSlices;
	rejects(input);
	input = baseline;
	input.streams[0].hTotal = 16701;
	input.streams[0].pixelKHz = 25000;
	rejects(input);
	input = baseline;
	input.streams[0].lanes = 1;
	rejects(input);
	input = baseline;
	input.streams[0].link = Link::HdmiTmds;
	input.streams[0].lanes = 0;
	input.streams[0].bpc = 12;
	rejects(input);
	puts("DML2.1 bridge rejected malformed mode and SoC inputs");
}

static void equal(const char *field, uint32_t got, uint32_t expected) {
	if (got == expected) return;
	fprintf(stderr, "%s: got %u, expected %u\n", field, got, expected);
	abort();
}

template <typename Value, size_t Count>
static void equalArray(const char *field, const Value (&got)[Count], const Value (&want)[Count]) {
	for (size_t index = 0; index < Count; ++index) equal(field, got[index], want[index]);
}

static void compare(const Input &input, const fixture_expected &want) {
	Result result{};
	equal("mode supported", solve(input, result), want.supported);
	equal("COMPBUF", result.compbufSegments, want.compbuf_segments);
	equal("watermark count", result.watermarkCount, want.watermark_count);
	equal("UCLK P-state support", result.uclkPstateSupported, want.uclk_pstate_supported);
	equal("FCLK P-state support", result.fclkPstateSupported, want.fclk_pstate_supported);
	equal("FAMS2 required", result.fams2Required, want.fams2_required);
	equalArray("FAMS2 global", result.fams2Global, want.fams2_global);
	for (uint32_t index = 0; index < result.watermarkCount; ++index) {
		const auto &actual = result.watermarks[index];
		const auto &expected = want.watermarks[index];
		equal("urgent watermark", actual.urgent, expected.urgent);
		equal("stutter entry watermark", actual.srEnter, expected.sr_enter);
		equal("stutter exit watermark", actual.srExit, expected.sr_exit);
		equal("UCLK P-state watermark", actual.uclkPstate, expected.uclk_pstate);
		equal("FCLK P-state watermark", actual.fclkPstate, expected.fclk_pstate);
	}
	for (uint32_t index = 0; index < input.count; ++index) {
		const auto &actual = result.streams[index];
		const auto &expected = want.streams[index];
		equal("DPP count", actual.dppCount, expected.dpp_count);
		equal("ODM count", actual.odmCount, expected.odm_count);
		equal("UCLK P-state method", actual.pstateMethod, expected.pstate_method);
		equalArray("DET", actual.detSegments, expected.det_segments);
		equalArray("phantom DET", actual.phantomDetSegments, expected.phantom_det_segments);
		equal("static MALL bytes", actual.surfaceMallBytes, expected.surface_mall_bytes);
		equal("SubVP MALL bytes", actual.svpMallBytes, expected.svp_mall_bytes);
		equal("phantom selected", actual.phantom, expected.phantom);
		equalArray("FAMS2 base", actual.fams2Base, expected.fams2_base);
		equalArray("FAMS2 sub", actual.fams2Sub, expected.fams2_sub);
	}
}

int main() {
	invalidInput();
	size_t index = 0;
	for (const auto &soc : fixture_socs) {
		for (const auto &mode : fixture_modes) {
			fprintf(stderr, "DCN 4.01 reference %s %s\n", soc.name, mode.name);
			compare(makeInput(soc, mode), fixture_results[index++]);
		}
	}
	printf("DML2.1 matched %zu pinned Linux reference vectors\n", index);
	auto input = makeInput(fixture_socs[0], fixture_modes[1]);
	input.count = kMaxStreams;
	for (uint32_t index = 1; index < input.count; ++index) input.streams[index] = input.streams[0];
	rejects(input);
	puts("DML2.1 rejected well-formed layout exceeding DCN pipe capacity");
	input = makeInput(fixture_socs[1], fixture_modes[0]);
	input.soc.fclk = input.soc.dcfclk = {2, {1000, 2000}};
	rejects(input);
	puts("DML2.1 rejected a bounding box with no valid fabric clock state");
}
