#include "dcn/dcn41_dml21_bridge.h"
#include "dcn/dcn41_fams2_dml_pure.h"
#include "dcn/dml21/port/dmub_cmd.h"
#include "../../dcn41/dcn41_allow_ranges.h"
#include "../../dcn41/dcn41_regs.h"
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

struct PhantomTtuReplay {
	static constexpr uint32_t kSeg2 = 0x34c0u;
	static constexpr uint32_t kInitial = 0x80000000u;
	uint32_t registers[3];
	uint32_t values[3] = {kInitial, kInitial, kInitial};
	unsigned writes = 0u;

	explicit PhantomTtuReplay(unsigned hubp) : registers{DCN41_HUBPREQ_DCN_TTU_QOS_WM(hubp),
		DCN41_HUBPREQ_DCN_SURF0_TTU_CNTL0(hubp), DCN41_HUBPREQ_DCN_SURF0_TTU_CNTL1(hubp)} {}

	rdna4det::Hw callbacks() {
		return {this,
			[](void *cookie, uint32_t offset) {
				auto &replay = *static_cast<PhantomTtuReplay *>(cookie);
				for (unsigned index = 0; index < 3; ++index)
					if (replay.registers[index] == offset) return replay.values[index];
				return UINT32_MAX;
			},
			[](void *cookie, uint32_t offset, uint32_t value) {
				auto &replay = *static_cast<PhantomTtuReplay *>(cookie);
				const unsigned index = replay.writes;
				if (index >= 3 || replay.registers[index] != offset) return false;
				const uint32_t address = kSeg2 + offset;
				for (const auto &range : dcn41_allow_ranges) {
					if (address < range.abs_lo || address > range.abs_hi) continue;
					replay.values[index] = value;
					++replay.writes;
					return true;
				}
				return false;
			}, nullptr};
	}
};

template <size_t Count>
static void replayPhantomTtu(const StreamResult &actual, const fixture_expected_stream &expected,
                             const rdna4fams2::PlaneRoute &route, const unsigned (&hubpOrder)[Count]) {
	assert(actual.phantom && actual.dppCount == Count);
	unsigned hubps = route.hubps;
	for (size_t part = 0; part < Count; ++part) {
		const unsigned hubp = hubpOrder[part];
		assert(hubp < rdna4fams2::kPipeCount && (hubps & (1u << hubp)));
		hubps &= ~(1u << hubp);
		PhantomTtuReplay replay(hubp);
		assert(rdna4det::program_hubp_ttu(replay.callbacks(), hubp, actual.phantomLumaTtu[part]));
		equal("phantom luma TTU write count", replay.writes, 3u);
		const auto &ttu = expected.phantom_luma_ttu[part];
		const uint32_t words[] = {
			PhantomTtuReplay::kInitial | ttu[0] | (ttu[1] << 16),
			PhantomTtuReplay::kInitial | ttu[2] | (ttu[3] << 24) | (ttu[4] << 28),
			PhantomTtuReplay::kInitial | ttu[5],
		};
		equalArray("phantom luma TTU MMIO", replay.values, words);
	}
	assert(!hubps);
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
		const auto &vertical = actual.phantomVertical;
		const uint32_t geometry[] {vertical.active, vertical.total, vertical.frontPorch};
		equalArray("phantom vertical geometry", geometry, expected.phantom_vertical);
	}
}

static void compareFams21Chain(const Result &solved, const rdna4fams2::Config &config,
                              uint32_t count, const uint8_t *mainOtgs, const uint8_t *mainMasks,
                              const uint8_t *phantomOtgs, const uint8_t *phantomMasks) {
	using namespace rdna4fams2;
	ConfigCommands chain{};
	assert(build_config(config, chain) == 1 + 2 * count);
	dmub_cmd_fams2_global_config global{};
	memcpy(&global, solved.fams2Global, sizeof(global));
	global.num_streams = count;
	assert(!memcmp(chain[0].payload, &global, sizeof(global)));
	for (uint32_t stream = 0; stream < count; ++stream) {
		dmub_fams2_cmd_stream_static_base_state base{};
		memcpy(&base, solved.streams[stream].fams2Base, sizeof(base));
		assert(base.type == FAMS2_STREAM_TYPE_SUBVP);
		base.otg_inst = mainOtgs[stream];
		base.pipe_mask = base.plane_pipe_masks[0] = mainMasks[stream];
		base.num_planes = 1;
		assert(!memcmp(chain[1 + stream].payload, &base, sizeof(base)));
		dmub_fams2_cmd_stream_static_sub_state sub{};
		memcpy(&sub, solved.streams[stream].fams2Sub, sizeof(sub));
		sub.subvp.phantom_otg_inst = phantomOtgs[stream];
		sub.subvp.phantom_pipe_mask = sub.subvp.phantom_plane_pipe_masks[0] = phantomMasks[stream];
		assert(!memcmp(chain[1 + count + stream].payload, &sub, sizeof(sub)));
	}
}

static void mapFams21() {
	using namespace rdna4fams2;
	const TopologyStamp current{16, 22};
	const Topology topology{current, 2, 0xf, {
		{{3, 1, 0x8, 0x8, 0x8}, {0, 1, 0x1, 0x1, 0x1}},
		{{1, 1, 0x2, 0x2, 0x2}, {2, 1, 0x4, 0x4, 0x4}},
	}};
	Result solved{};
	assert(solve(makeInput(fixture_socs[0], fixture_modes[2]), solved));
	Config config{};
	assert(lower_config(solved, current, topology, current, config));
	const uint8_t mainOtgs[]{3, 1}, mainMasks[]{0x8, 0x2}, phantomOtgs[]{0, 2}, phantomMasks[]{0x1, 0x4};
	compareFams21Chain(solved, config, 2, mainOtgs, mainMasks, phantomOtgs, phantomMasks);
	replayPhantomTtu(solved.streams[0], fixture_results[2].streams[0], topology.streams[0].phantom, {0});
	replayPhantomTtu(solved.streams[1], fixture_results[2].streams[1], topology.streams[1].phantom, {2});

	const auto populatedConfig = config;
	auto reject = [&](const Result &candidate, const Topology &mapping, TopologyStamp solvedFor) {
		config = populatedConfig;
		assert(!lower_config(candidate, solvedFor, mapping, current, config));
		ConfigCommands chain{};
		assert(build_config(config, chain) == 0);
	};
	reject(solved, topology, {current.owner, current.generation - 1});
	auto bad = topology;
	bad.stamp.owner++;
	reject(solved, bad, current);
	bad = topology;
	bad.streams[0].phantom = {};
	reject(solved, bad, current);
	bad = topology;
	bad.streams[1].phantom = bad.streams[0].phantom;
	reject(solved, bad, current);
	bad = topology;
	bad.streams[0].main.otg = 1;
	reject(solved, bad, current);
	bad = topology;
	bad.streams[1].main.planeCount = 2;
	reject(solved, bad, current);
	auto badResult = solved;
	badResult.streams[0].pstateMethod = UINT32_MAX;
	reject(badResult, topology, current);

	assert(solve(makeInput(fixture_socs[0], fixture_modes[1]), solved));
	const Topology split{current, 1, 0xf, {
		{{3, 1, 0xa, 0xa, 0xa}, {2, 1, 0x5, 0x5, 0x5}},
	}};
	assert(lower_config(solved, current, split, current, config));
	const uint8_t splitMainOtg[]{3}, splitMainMask[]{0xa}, splitPhantomOtg[]{2}, splitPhantomMask[]{0x5};
	compareFams21Chain(solved, config, 1, splitMainOtg, splitMainMask, splitPhantomOtg, splitPhantomMask);
	replayPhantomTtu(solved.streams[0], fixture_results[1].streams[0], split.streams[0].phantom, {2, 0});
	bad = split;
	bad.streams[0].phantom.odmHeads = 0x4;
	reject(solved, bad, current);
	bad = split;
	bad.streams[1].phantom = split.streams[0].phantom;
	reject(solved, bad, current);

	auto input = makeInput(fixture_socs[0], fixture_modes[0]);
	input.soc.uclkBlackoutNs = 550000;
	assert(solve(input, solved));
	const Topology fused{current, 1, 0xd, {
		{{3, 1, 0x4, 0x4, 0x8}, {2, 1, 0x2, 0x2, 0x4}},
	}};
	assert(lower_config(solved, current, fused, current, config));
	const uint8_t fusedMainOtg[]{3}, fusedMainMask[]{0x4}, fusedPhantomOtg[]{2}, fusedPhantomMask[]{0x2};
	compareFams21Chain(solved, config, 1, fusedMainOtg, fusedMainMask, fusedPhantomOtg, fusedPhantomMask);
	bad = fused;
	bad.streams[0].phantom.hubps = bad.streams[0].phantom.pipeSlots;
	reject(solved, bad, current);
	puts("DML2.1 FAMS2.1 matched Linux CONFIG fields for permuted, split and fused SubVP routes");
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
	mapFams21();
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
