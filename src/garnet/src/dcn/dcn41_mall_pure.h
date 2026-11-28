#pragma once

#include "amd/amdgpu_asic.h"
#include "dcn41_cursor_pure.h"
#include "dcn41_dmub.h"
#include "dcn41_io.h"

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

struct CabFront {
	amdgpu::AsicId chip;
	uint32_t vramBiosChannels, hubp, otg;
	uint64_t generation, mcAddress, surfaceBytes;
	uint32_t cursorWidth, cursorHeight;
	bool enabled, sealed, cursorEnabled, psr, stereo, subvp;
};

struct CabState {
	const dcn41_dmub_ring *owner { nullptr };
	uint32_t hubp { 0 }, savedFields { 0 };
	bool stopRequired { false };
};

constexpr uint32_t kOwnedMallFields = DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, USE_MALL_SEL) |
	DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, USE_MALL_FOR_CURSOR) |
	DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, MALL_PREF_CMD_TYPE) |
	DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, MALL_PREF_MODE);

constexpr uint32_t kArbiterBusy = DCN41_M(DCN41_DCHUBBUB_ARB_MALL_CNTL, GLOBAL_USE_MALL_FOR_SS) |
	DCN41_M(DCN41_DCHUBBUB_ARB_MALL_CNTL, MALL_IN_USE);

inline int mall_read(dcn41_dev *dev, uint32_t address, uint32_t &raw) {
	const int result = dcn41_read(dev, address, &raw);
	return result ? result : raw == UINT32_MAX ? DCN41_E_HW : DCN41_OK;
}

inline int write_mall_fields(dcn41_dev *dev, uint32_t address, uint32_t fields) {
	uint32_t raw = 0;
	int result = mall_read(dev, address, raw);
	if (result) return result;
	if ((raw & kOwnedMallFields) == fields) return DCN41_OK;
	result = dcn41_write(dev, address, (raw & ~kOwnedMallFields) | fields);
	if (result) return result;
	result = mall_read(dev, address, raw);
	return result ? result : (raw & kOwnedMallFields) == fields ? DCN41_OK : DCN41_E_READBACK;
}

inline int wait_cab(dcn41_dev *dev, bool entering) {
	const uint32_t address = DCN41_ADDR(dev, DCN41_DCHUBBUB_ARB_MALL_CNTL);
	constexpr uint32_t kCabSelected = DCN41_M(DCN41_DCHUBBUB_ARB_MALL_CNTL, GLOBAL_USE_MALL_FOR_SS);
	for (unsigned poll = 0; poll < 1000u; ++poll) {
		uint32_t raw = 0;
		const int result = mall_read(dev, address, raw);
		if (result) return result;
		if (entering ? (raw & kCabSelected) != 0u : (raw & kArbiterBusy) == 0u) return DCN41_OK;
		dev->udelay(dev->cookie, 1u);
	}
	return DCN41_E_TIMEOUT;
}

// The caller holds the display lease, exclusive DAL ring ownership and the pin/write seal for
// both the validated one-plane HUBP/OTG route and its cursor until cab_leave returns OK.
inline int cab_leave(dcn41_dmub_ring &ring, CabState &state) {
	if (!state.owner) return DCN41_OK;
	if (state.owner != &ring || !dcn41_ready(ring.dev)) return DCN41_E_STATE;
	auto *dev = ring.dev;
	if (state.stopRequired) {
		dcn41_dmub_cmd cmd;
		build_cab_command(cmd, kNoIdle, 0u);
		int result = dcn41_dmub_ring_submit(&ring, &cmd);
		if (result) return result;
		result = dcn41_dmub_ring_wait_idle(&ring, 1000u);
		if (result) return result;
		result = wait_cab(dev, false);
		if (result) return result;
		state.stopRequired = false;
	}
	const int result = write_mall_fields(dev, DCN41_ADDR_I(dev, DCN41_HUBP_DCHUBP_MALL_CONFIG, state.hubp),
	                                     state.savedFields);
	if (!result) state = {};
	return result;
}

inline int cab_enter(dcn41_dmub_ring &ring, CabState &state, const CabFront &front, uint64_t liveGeneration) {
	if (!front.enabled || !front.sealed || front.psr || front.stereo || front.subvp || state.owner ||
	    !front.generation || front.generation != liveGeneration || !front.mcAddress ||
	    !front.surfaceBytes || front.hubp >= DCN41_NUM_PIPES || front.otg >= DCN41_NUM_PIPES ||
	    !dcn41_ready(ring.dev) ||
	    (front.cursorEnabled && (!front.cursorWidth || front.cursorWidth > rdna4cursor::kCursorMaxDim ||
	                            !front.cursorHeight || front.cursorHeight > rdna4cursor::kCursorMaxDim)))
		return DCN41_E_STATE;
	const uint32_t cursorBytes = rdna4cursor::mall_cursor_bytes(front.cursorEnabled, front.cursorHeight);
	const uint8_t ways = cab_ways(display_budget(front.chip, front.vramBiosChannels), front.surfaceBytes, cursorBytes);
	if (ways == kDoesNotFit) return DCN41_E_STATE;
	auto *dev = ring.dev;
	uint32_t raw = 0;
	int result = mall_read(dev, DCN41_ADDR(dev, DCN41_DMCUB_SCRATCH0), raw);
	constexpr uint32_t kDalReady = DCN41_DMUB_BOOT_DAL_FW | DCN41_DMUB_BOOT_MAILBOX_RDY |
		DCN41_DMUB_BOOT_HW_POWER_INIT_DONE;
	if (result || (raw & kDalReady) != kDalReady || dev->hubp_power_gated[front.hubp]) return DCN41_E_STATE;
	for (uint32_t otg = 0; otg < DCN41_NUM_PIPES; ++otg) {
		result = mall_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_CONTROL, otg), raw);
		if (result || !!(raw & DCN41_M(DCN41_OTG_OTG_CONTROL, OTG_MASTER_EN)) != (otg == front.otg))
			return DCN41_E_STATE;
	}
	for (uint32_t hubp = 0; hubp < DCN41_NUM_PIPES; ++hubp) {
		if (hubp != front.hubp && dev->hubp_power_gated[hubp]) continue;
		result = mall_read(dev, DCN41_ADDR_I(dev, DCN41_HUBP_DCHUBP_CNTL, hubp), raw);
		if (result || !!DCN41_FG(DCN41_HUBP_DCHUBP_CNTL, HUBP_BLANK_EN, raw) != (hubp != front.hubp) ||
		    (hubp == front.hubp && DCN41_FG(DCN41_HUBP_DCHUBP_CNTL, HUBP_VTG_SEL, raw) != front.otg))
			return DCN41_E_STATE;
	}
	bool pending = false;
	uint64_t inUse = 0;
	result = dcn41_hubp_is_flip_pending(dev, front.hubp, &pending, &inUse);
	if (result || pending || inUse != front.mcAddress) return DCN41_E_STATE;
	result = mall_read(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_CONTROL, front.hubp), raw);
	if (result || (raw & DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_CONTROL, PRIMARY_SURFACE_TMZ))) return DCN41_E_STATE;
	result = mall_read(dev, DCN41_ADDR(dev, DCN41_DCHUBBUB_ARB_MALL_CNTL), raw);
	if (result || (raw & kArbiterBusy)) return DCN41_E_STATE;
	const uint32_t address = DCN41_ADDR_I(dev, DCN41_HUBP_DCHUBP_MALL_CONFIG, front.hubp);
	result = mall_read(dev, address, raw);
	if (result || (raw & (DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, USE_MALL_SEL) |
	                      DCN41_M(DCN41_HUBP_DCHUBP_MALL_CONFIG, USE_MALL_FOR_CURSOR)))) return DCN41_E_STATE;
	result = dcn41_dmub_ring_wait_idle(&ring, 1000u);
	if (result) return result;
	state = {&ring, front.hubp, raw & kOwnedMallFields, false};
	const uint32_t fields = rdna4cursor::mall_cursor_select(
		DCN41_FV(DCN41_HUBP_DCHUBP_MALL_CONFIG, USE_MALL_SEL, 2u) |
		DCN41_FV(DCN41_HUBP_DCHUBP_MALL_CONFIG, MALL_PREF_CMD_TYPE, 1u),
		cursorBytes, front.cursorEnabled, front.cursorWidth, front.cursorHeight, false);
	result = write_mall_fields(dev, address, fields);
	if (!result) {
		dcn41_dmub_cmd cmd;
		build_cab_command(cmd, kFit, ways);
		result = dcn41_dmub_ring_submit(&ring, &cmd);
		if (!result) {
			state.stopRequired = true;
			result = dcn41_dmub_ring_wait_idle(&ring, 1000u);
			if (!result) result = wait_cab(dev, true);
		}
	}
	if (result) {
		const int restored = cab_leave(ring, state);
		return restored ? restored : result;
	}
	return DCN41_OK;
}

} // namespace rdna4mall
