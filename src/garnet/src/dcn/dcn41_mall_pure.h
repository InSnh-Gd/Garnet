#pragma once

#include "amd/amdgpu_asic.h"
#include "dcn41_dmub.h"

namespace rdna4mall {

constexpr uint8_t kCabWays = 16u;
constexpr uint8_t kDoesNotFit = UINT8_MAX;

inline uint64_t physical_bytes(amdgpu::AsicId id) {
	const auto *asic = amdgpu::asic_by_id(id);
	return asic ? static_cast<uint64_t>(asic->mallSizeMiB) << 20 : 0u;
}

// Linux dcn401_calc_num_avail_chans_for_mall uses ATOM memory channels, not GC GL2C count.
inline uint64_t display_budget(amdgpu::AsicId id, uint32_t vramBiosChannels) {
	const auto *asic = amdgpu::asic_by_id(id);
	if (!asic || vramBiosChannels < 2u) return 0u;
	uint32_t channels = 2u;
	while (channels <= vramBiosChannels / 2u && channels <= asic->displayMallChannelLimit / 2u)
		channels *= 2u;
	const uint64_t budget = static_cast<uint64_t>(channels) << 22;
	const uint64_t physical = physical_bytes(id);
	return budget < physical ? budget : physical;
}

constexpr uint8_t cab_ways(uint64_t budget, uint64_t surfaceBytes, uint64_t cursorBytes) {
	constexpr uint64_t kLineBytes = 64u;
	if (surfaceBytes > UINT64_MAX - cursorBytes) return kDoesNotFit;
	const uint64_t bytes = surfaceBytes + cursorBytes;
	if (bytes == 0u) return 0u;
	const uint64_t linesPerWay = budget / (kCabWays * kLineBytes);
	if (!linesPerWay) return kDoesNotFit;
	const uint64_t linesUsed = bytes / kLineBytes + 2u;
	const uint64_t ways = linesUsed / linesPerWay + (linesUsed % linesPerWay != 0u);
	return ways <= kCabWays ? static_cast<uint8_t>(ways) : kDoesNotFit;
}

enum CabAction : uint8_t { kNoIdle = 0, kNoDcnReq = 1, kFit = 2, kNotFit = 3 };

// dmub_cmd.h: DMUB_CMD__CAB_FOR_SS and packed dmub_rb_cmd_cab_for_ss.
inline void build_cab_command(dcn41_dmub_cmd &cmd, CabAction action, uint8_t ways) {
	if (action == kFit && (ways == 0u || ways > kCabWays)) action = kNotFit;
	cmd = {};
	cmd.header = 75u | (static_cast<uint32_t>(action) << DCN41_DMUB_HDR_SUB_TYPE_SHIFT) |
	             (2u << DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT);
	cmd.payload[0] = action == kFit ? ways : 0u;
}

} // namespace rdna4mall
