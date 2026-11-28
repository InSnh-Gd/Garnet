#pragma once

#include "dcn41_dml21_bridge.h"
#include "dcn41_fams2_pure.h"

namespace rdna4fams2 {

struct TopologyStamp {
	uint64_t owner, generation;
};

struct PlaneRoute {
	uint8_t otg, planeCount, pipeSlots, odmHeads, hubps;
};

// One plane per stream, in solver order. Both stamps must come from the same
// display owner and its modeset generation, not from the display timings.
struct Topology {
	TopologyStamp stamp;
	uint8_t streamCount, enabledPipes;
	struct Stream {
		PlaneRoute main, phantom;
	} streams[kPipeCount];
};

namespace mapping {

constexpr unsigned kStreamCountOffset = 8;
constexpr unsigned kFeatureOffset = 12;
constexpr unsigned kBaseOtg = 37, kBasePipeMask = 38, kBasePlaneCount = 39, kBasePlaneMasks = 40;
constexpr unsigned kPhantomOtg = 13, kPhantomPipeMask = 14, kPhantomPlaneMasks = 16;

inline bool same_stamp(TopologyStamp left, TopologyStamp right) {
	return left.owner == right.owner && left.generation == right.generation;
}

inline unsigned count_pipes(unsigned mask) {
	unsigned count = 0;
	for (; mask; mask &= mask - 1) ++count;
	return count;
}

inline bool empty_route(const PlaneRoute &route) {
	return !route.otg && !route.planeCount && !route.pipeSlots && !route.odmHeads && !route.hubps;
}

// DCN401 compacts unfused hardware instances into pipe_ctx slots in instance
// order. FAMS2.1 CONFIG uses those slots for masks, unlike FLIP's HUBP bits.
inline bool valid_route(const PlaneRoute &route, unsigned enabledPipes, unsigned dpps, unsigned odms,
                        unsigned &claimedSlots) {
	if (route.planeCount != 1 || route.otg >= kPipeCount || !dpps || !odms ||
	    (odms > 1 && odms != dpps) || count_pipes(route.pipeSlots) != dpps ||
	    count_pipes(route.odmHeads) != odms || (route.odmHeads & ~route.pipeSlots) ||
	    (route.pipeSlots & claimedSlots)) return false;

	unsigned slot = 0, physical = 0, primary = 0, availableSlots = 0;
	for (unsigned inst = 0; inst < kPipeCount; ++inst) {
		if (!(enabledPipes & (1u << inst))) continue;
		const unsigned bit = 1u << slot++;
		availableSlots |= bit;
		if (route.pipeSlots & bit) physical |= 1u << inst;
		if (route.otg == inst) primary = bit;
	}
	if ((route.pipeSlots & ~availableSlots) || physical != route.hubps || !(primary & route.odmHeads)) return false;
	claimedSlots |= route.pipeSlots;
	return true;
}

// dml2_core_calcs_get_stream_fams2_programming maps methods to wire types.
inline unsigned wire_type(unsigned method) {
	switch (method) {
	case 1: case 11: return 2; // VACTIVE
	case 2: case 12: return 1; // VBLANK
	case 21: return 3; // DRR
	case 6: case 13: return 4; // SubVP
	default: return 0;
	}
}

template <unsigned Size>
inline void copy_bytes(uint8_t (&out)[Size], const uint8_t (&in)[Size]) {
	for (unsigned index = 0; index < Size; ++index) out[index] = in[index];
}

} // namespace mapping

// Offline CONFIG data only. This is not a firmware acknowledgement or a
// claim that the DML layout has been programmed. Legacy-only is not in the bridge.
inline bool lower_config(const rdna4dml::Result &solved, TopologyStamp solvedFor, const Topology &topology,
                         TopologyStamp current, Config &out) {
	using namespace mapping;
	static_assert(kPipeCount == rdna4dml::kMaxStreams);
	out = {};
	const unsigned count = topology.streamCount;
	if (!current.owner || !current.generation || !same_stamp(solvedFor, current) ||
	    !same_stamp(topology.stamp, current) || !count || count > kPipeCount ||
	    !topology.enabledPipes || (topology.enabledPipes & ~kPipeMask) ||
	    !solved.fams2Required || !solved.uclkPstateSupported ||
	    get_u32(solved.fams2Global + kStreamCountOffset) != count ||
	    !(get_u32(solved.fams2Global + kFeatureOffset) & 1u)) return false;

	unsigned claimedSlots = 0;
	for (unsigned index = 0; index < kPipeCount; ++index) {
		const auto &route = topology.streams[index];
		if (index >= count) {
			if (!empty_route(route.main) || !empty_route(route.phantom)) return false;
			continue;
		}
		const auto &stream = solved.streams[index];
		const unsigned type = wire_type(stream.pstateMethod);
		if (!type || get_u32(stream.fams2Base) != type || stream.phantom != (type == 4u) ||
		    !valid_route(route.main, topology.enabledPipes, stream.dppCount, stream.odmCount, claimedSlots)) return false;
		if (stream.phantom) {
			if (!valid_route(route.phantom, topology.enabledPipes, stream.dppCount, stream.odmCount, claimedSlots)) return false;
		} else if (!empty_route(route.phantom)) return false;
	}

	copy_bytes(out.global, solved.fams2Global);
	put_le(out.global + kStreamCountOffset, count, 4);
	for (unsigned index = 0; index < count; ++index) {
		const auto &stream = solved.streams[index];
		const auto &route = topology.streams[index];
		auto &wire = out.stream[index];
		copy_bytes(wire.base, stream.fams2Base);
		copy_bytes(wire.sub, stream.fams2Sub);
		wire.base[kBaseOtg] = route.main.otg;
		wire.base[kBasePipeMask] = route.main.pipeSlots;
		wire.base[kBasePlaneCount] = route.main.planeCount;
		wire.base[kBasePlaneMasks] = route.main.pipeSlots;
		for (unsigned plane = 1; plane < 6; ++plane) wire.base[kBasePlaneMasks + plane] = 0;
		if (stream.phantom) {
			wire.sub[kPhantomOtg] = route.phantom.otg;
			wire.sub[kPhantomPipeMask] = route.phantom.pipeSlots;
			wire.sub[kPhantomPlaneMasks] = route.phantom.pipeSlots;
			for (unsigned plane = 1; plane < 3; ++plane) wire.sub[kPhantomPlaneMasks + plane] = 0;
		}
	}
	return true;
}

} // namespace rdna4fams2
