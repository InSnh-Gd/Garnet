#include "dcn/dcn41_fams2_pure.h"

#include <cstdio>
#include <cstring>

namespace {
int checks, failures;

void expect(bool ok, const char *desc) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("FAIL: %s\n", desc);
	}
}
}

int main() {
	using namespace rdna4fams2;
	Config config {};
	config.global[0] = 100u;
	config.global[8] = 2u;
	config.global[12] = 1u;
	config.stream[0].base[0] = 4u;
	config.stream[0].base[38] = 1u;
	config.stream[0].base[39] = 1u;
	config.stream[0].base[40] = 1u;
	config.stream[0].sub[13] = 2u;
	config.stream[0].sub[14] = 4u;
	config.stream[0].sub[16] = 4u;
	config.stream[1].base[0] = 1u;
	config.stream[1].base[37] = 1u;
	config.stream[1].base[38] = 2u;
	config.stream[1].base[39] = 1u;
	config.stream[1].base[40] = 2u;
	config.stream[1].sub[0] = 12u;
	ConfigCommands batch {};
	expect(build_config(config, batch) == 5u, "global, two base packets, then two sub packets");
	const dcn41_dmub_cmd expectedConfig[5] = {
		{0x3c02044cu, {100u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 2u, 0u, 0u, 0u, 1u}},
		{0x3c02044cu, {}}, {0x3c02044cu, {}}, {0x3c02044cu, {}}, {0x3c00044cu, {}},
	};
	expect(std::memcmp(&batch[0], &expectedConfig[0], sizeof(batch[0])) == 0, "global wire packet matches Linux ABI");
	expect(batch[1].header == expectedConfig[1].header && batch[1].payload[0] == 4u && batch[1].payload[38] == 1u,
	       "SubVP base packet precedes all subtype payloads");
	expect(batch[2].header == expectedConfig[2].header && batch[2].payload[0] == 1u && batch[2].payload[37] == 1u,
	       "second stream base packet");
	expect(batch[3].header == expectedConfig[3].header && batch[3].payload[13] == 2u && batch[3].payload[14] == 4u,
	       "SubVP subtype packet keeps phantom OTG and pipe mask distinct");
	expect(batch[4].header == expectedConfig[4].header && batch[4].payload[0] == 12u && batch[4].payload[59] == 0u,
	       "last packet terminates the chain and clears padding");
	config.global[9] = 1u;
	expect(build_config(config, batch) == 0u, "reject oversized 32-bit stream count before indexing");
	config.global[9] = 0u;
	config.global[8] = 5u;
	expect(build_config(config, batch) == 0u, "DCN 4.01 cannot program five streams");
	config.global[8] = 2u;
	config.stream[0].sub[14] = 0u;
	expect(build_config(config, batch) == 0u, "a SubVP stream without a mapped phantom must not reach firmware");
	config.stream[0].sub[14] = 4u;
	config.stream[1].base[37] = 0u;
	expect(build_config(config, batch) == 0u, "duplicate OTG assignment must not reach firmware");

	dcn41_dmub_cmd cmd = build_disable_config();
	const dcn41_dmub_cmd expectedDisable {0x3c00044cu, {}};
	expect(std::memcmp(&cmd, &expectedDisable, sizeof(cmd)) == 0, "disable uses empty unchained CONFIG");
	const dcn41_dmub_cmd expectedDrr {
		0x1400054cu, {0x8b, 0x08, 0, 0, 0xc3, 0x09, 0, 0, 0xfb, 0x08, 0, 0, 3, 0, 0, 0, 1, 2},
	};
	expect(build_drr(cmd, 2u, 2187u, 2499u, 2299u, 3u, true) &&
	       std::memcmp(&cmd, &expectedDrr, sizeof(cmd)) == 0, "DRR bytes match Linux OPTC state wire layout");
	expect(!build_drr(cmd, 4u, 2187u, 2499u, 2299u, 3u, true), "out-of-range DRR OTG refused");
	expect(!build_drr(cmd, 2u, 2500u, 2499u, 0u, 0u, false), "inverted DRR range refused");
	expect(build_drr(cmd, 2u, 0u, 0u, 0u, 0u, false) && cmd.payload[17] == 2u, "DRR disable accepted");
	const dcn41_dmub_cmd expectedFlip {
		0x1c00064cu, {1, 2, 4, 0, 0x00, 0x56, 0x34, 0x12, 0, 0, 0, 0, 0x00, 0xef, 0xcd, 0xab,
		              0, 0, 0, 0, 0x83, 0, 0, 0, 0x81, 0},
	};
	expect(build_graphics_flip(cmd, 2u, 4u, 0x8312345600ULL, 0x81abcdef00ULL, true) &&
	       std::memcmp(&cmd, &expectedFlip, sizeof(cmd)) == 0, "graphics flip preserves the 48-bit surface and DCC metadata addresses");
	expect(!build_graphics_flip(cmd, 2u, 4u, 1ULL << 48, 0u, false), "refuse truncated surface address");
	expect(!build_graphics_flip(cmd, 2u, 16u, 256u, 0u, false), "refuse nonexistent pipe");

	expect(supports_subtype(4u) && supports_subtype(5u) && supports_subtype(6u), "2.1 supports CONFIG, DRR_UPDATE and FLIP");
	expect(kIbConfigSubtype == 7u && !supports_subtype(7u), "IB_CONFIG is a major-3 command, not a v2.1 command");

	std::printf("dcn41_fams2_test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
