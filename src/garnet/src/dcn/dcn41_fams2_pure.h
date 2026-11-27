#pragma once

#include "../../../dcn41/dcn41_dmub.h"

namespace rdna4fams2 {

// Linux dmub_cmd.h and dc_dmub_srv.c: FAMS 2.1 uses ring commands, not IB_CONFIG.
constexpr uint32_t kCommandType = 76u;
constexpr uint32_t kConfigSubtype = 4u;
constexpr uint32_t kDrrSubtype = 5u;
constexpr uint32_t kFlipSubtype = 6u;
constexpr uint32_t kIbConfigSubtype = 7u;
constexpr uint32_t kPipeCount = 4u;
constexpr uint32_t kPipeMask = (1u << kPipeCount) - 1u;
constexpr uint32_t kConfigPayloadBytes = 60u;

constexpr bool supports_subtype(uint32_t subtype) {
	return subtype == kConfigSubtype || subtype == kDrrSubtype || subtype == kFlipSubtype;
}

struct Config {
	uint8_t global[28];
	struct Stream {
		uint8_t base[48];
		uint8_t sub[20];
	} stream[kPipeCount];
};

using ConfigCommands = dcn41_dmub_cmd[1 + 2 * kPipeCount];

template <uint32_t Subtype>
inline dcn41_dmub_cmd command(uint32_t payloadBytes) {
	static_assert(supports_subtype(Subtype), "unsupported FAMS 2.1 subtype");
	return {kCommandType | (Subtype << DCN41_DMUB_HDR_SUB_TYPE_SHIFT) |
	        (payloadBytes << DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT), {}};
}

template <unsigned Size>
inline void copy_payload(dcn41_dmub_cmd &cmd, const uint8_t (&payload)[Size]) {
	static_assert(Size <= sizeof(cmd.payload), "DMUB payload exceeds one ring slot");
	for (unsigned index = 0; index < Size; ++index) cmd.payload[index] = payload[index];
}

inline uint32_t get_u32(const uint8_t *bytes) {
	uint32_t val = 0;
	for (unsigned index = 0; index < 4; ++index) val |= uint32_t(bytes[index]) << (8u * index);
	return val;
}

inline void put_le(uint8_t *bytes, uint64_t val, unsigned size) {
	for (unsigned index = 0; index < size; ++index) bytes[index] = uint8_t(val >> (8u * index));
}

inline dcn41_dmub_cmd build_disable_config() {
	return command<kConfigSubtype>(kConfigPayloadBytes);
}

inline bool claim_topology(uint32_t otg, uint32_t pipeMask, uint32_t &otgs, uint32_t &pipes) {
	if (otg >= kPipeCount || (otgs & (1u << otg)) || !pipeMask ||
	    (pipeMask & ~kPipeMask) || (pipeMask & pipes)) return false;
	otgs |= 1u << otg;
	pipes |= pipeMask;
	return true;
}

inline bool plane_masks_match(const uint8_t *masks, uint32_t count, uint32_t pipeMask, bool allowEmpty) {
	uint32_t combined = 0;
	for (unsigned index = 0; index < count; ++index) {
		if ((!allowEmpty && !masks[index]) || (combined & masks[index])) return false;
		combined |= masks[index];
	}
	return combined == pipeMask;
}

inline unsigned build_config(const Config &config, ConfigCommands &out) {
	const uint32_t streamCount = get_u32(config.global + 8);
	const uint32_t features = get_u32(config.global + 12);
	constexpr uint32_t kEnable = 1u;
	constexpr uint32_t kLegacyWithoutFams2 = 1u << 8;
	if (streamCount == 0 || streamCount > kPipeCount || !(features & (kEnable | kLegacyWithoutFams2))) return 0;
	uint32_t otgs = 0, pipes = 0;
	for (uint32_t index = 0; index < streamCount; ++index) {
		const auto &stream = config.stream[index];
		const uint32_t type = get_u32(stream.base);
		const uint32_t planeCount = stream.base[39];
		if (type < 1u || type > 4u || !planeCount || planeCount > 6u ||
		    !claim_topology(stream.base[37], stream.base[38], otgs, pipes) ||
		    !plane_masks_match(stream.base + 40, planeCount, stream.base[38], false)) return 0;
		if (type == 4u && (!claim_topology(stream.sub[13], stream.sub[14], otgs, pipes) ||
		                   !plane_masks_match(stream.sub + 16, 3u, stream.sub[14], true))) return 0;
	}
	const unsigned numCommands = 1u + 2u * streamCount;
	for (unsigned index = 0; index < numCommands; ++index) {
		out[index] = build_disable_config();
		if (index + 1 < numCommands) out[index].header |= DCN41_DMUB_HDR_MULTI_CMD_PENDING_MASK;
	}
	copy_payload(out[0], config.global);
	for (unsigned index = 0; index < streamCount; ++index) {
		copy_payload(out[1u + index], config.stream[index].base);
		copy_payload(out[1u + streamCount + index], config.stream[index].sub);
	}
	return numCommands;
}

inline bool build_drr(dcn41_dmub_cmd &cmd, uint8_t otg, uint32_t vtotalMin, uint32_t vtotalMax,
                      uint32_t vtotalMid, uint32_t midFrames, bool manualTrigger) {
	if (otg >= kPipeCount || vtotalMin > vtotalMax ||
	    (vtotalMid && (vtotalMid < vtotalMin || vtotalMid > vtotalMax)) || (!vtotalMid && midFrames)) return false;
	cmd = command<kDrrSubtype>(20u);
	put_le(cmd.payload, vtotalMin, 4u);
	put_le(cmd.payload + 4, vtotalMax, 4u);
	put_le(cmd.payload + 8, vtotalMid, 4u);
	put_le(cmd.payload + 12, midFrames, 4u);
	cmd.payload[16] = manualTrigger ? 1u : 0u;
	cmd.payload[17] = otg;
	return true;
}

inline bool build_graphics_flip(dcn41_dmub_cmd &cmd, uint8_t otg, uint32_t pipeMask,
                                uint64_t surface, uint64_t metadata, bool immediate) {
	if (otg >= kPipeCount || !pipeMask || (pipeMask & ~kPipeMask) || !surface ||
	    (surface >> 48) || (metadata >> 48)) return false;
	cmd = command<kFlipSubtype>(28u);
	cmd.payload[0] = immediate ? 1u : 0u;
	cmd.payload[1] = otg;
	cmd.payload[2] = uint8_t(pipeMask);
	put_le(cmd.payload + 4, surface, 4u);
	put_le(cmd.payload + 12, metadata, 4u);
	put_le(cmd.payload + 20, surface >> 32, 2u);
	put_le(cmd.payload + 24, metadata >> 32, 2u);
	return true;
}

} // namespace rdna4fams2
