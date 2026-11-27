#include "dcn/dcn41_mall_pure.h"

#include <cstdio>

namespace {

unsigned checks = 0, failures = 0;

void expect(bool condition, const char *message) {
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL: %s\n", message);
	}
}

} // namespace

int main() {
	using namespace rdna4mall;
	using amdgpu::AsicId;

	constexpr uint64_t mib = 1u << 20;
	expect(physical_bytes(AsicId::Navi44) == 32u * mib &&
	       physical_bytes(AsicId::Navi48) == 64u * mib, "physical Infinity Cache capacities");
	expect(display_budget(AsicId::Navi44, 8u) == 32u * mib &&
	       display_budget(AsicId::Navi44, 6u) == 16u * mib, "BIOS memory channels round down to a power of two");
	expect(display_budget(AsicId::Navi48, 16u) == 64u * mib &&
	       display_budget(AsicId::Navi48, 8u) == 32u * mib &&
	       display_budget(AsicId::Navi44, 16u) == 32u * mib,
	       "display CAB reflects actual memory channels up to each chip's physical capacity");
	expect(display_budget(AsicId::Navi44, 0u) == 0u && display_budget(AsicId::Navi44, 1u) == 0u &&
	       display_budget(AsicId::Unknown, 16u) == 0u, "unsupported topology cannot allocate CAB");

	const uint64_t cab = display_budget(AsicId::Navi44, 8u);
	expect(cab_ways(cab, 0u, 0u) == 0u, "no memory demand needs zero ways");
	expect(cab_ways(cab, cab / 16u - 128u, 0u) == 1u &&
	       cab_ways(cab, cab / 16u - 64u, 0u) == 2u, "CAB ways account for upstream two-line alignment margin");
	expect(cab_ways(cab, cab - 128u, 0u) == 16u &&
	       cab_ways(cab, cab - 128u, 65536u) == kDoesNotFit, "cursor bytes are included before way fit");
	expect(cab_ways(cab, UINT64_MAX, 1u) == kDoesNotFit && cab_ways(0u, 1u, 0u) == kDoesNotFit,
	       "overflow and unknown budget never admit CAB");

	dcn41_dmub_cmd cmd {};
	build_cab_command(cmd, kFit, 16u);
	expect(cmd.header == 0x0200024bu && cmd.payload[0] == 16u && cmd.payload[1] == 0u &&
	       cmd.payload[59] == 0u, "FIT command exactly matches Linux DMUB type, subtype and payload");
	build_cab_command(cmd, kNoIdle, 0u);
	expect(cmd.header == 0x0200004bu && cmd.payload[0] == 0u, "CAB stop clears allocation and uses NO_IDLE");
	build_cab_command(cmd, kNotFit, 0u);
	expect(cmd.header == 0x0200034bu && cmd.payload[0] == 0u, "NOT_FIT matches the firmware ABI");
	build_cab_command(cmd, kFit, kDoesNotFit);
	expect(cmd.header == 0x0200034bu && cmd.payload[0] == 0u, "invalid FIT count never reaches firmware");

	std::printf("dcn41_fams2_mall_test: %u/%u passed\n", checks - failures, checks);
	return failures != 0;
}
