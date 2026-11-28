#include "dcn41_dml21_bridge.h"
#include "dml21_adapter/dcn41_dml21_adapter.h"

#if defined(KERNEL)
#error "DML2.1 solve is host-only until XNU FPU ownership is verified"
#endif

#include <cstdlib>
#include <memory>

namespace rdna4dml {
namespace {

constexpr uint32_t kMaxTimingTotal = (1u << 15) - 1;
constexpr uint32_t kDcn401NominalVblankUs = 668;

bool validTiming(uint32_t active, uint32_t total, uint32_t front, uint32_t sync) {
	return active != 0 && active < total && total <= kMaxTimingTotal &&
	       front != 0 && front < total - active && sync != 0 && sync <= total - active - front;
}

bool validStream(const Stream &stream) {
	if (!validTiming(stream.hActive, stream.hTotal, stream.hFront, stream.hSync) ||
	    !validTiming(stream.vActive, stream.vTotal, stream.vFront, stream.vSync) ||
	    stream.pixelKHz < 25000 || stream.pixelKHz > 2500000 ||
	    stream.hTotal * 1000u >= stream.pixelKHz * uint64_t{kDcn401NominalVblankUs} ||
	    stream.pitchPixels < stream.hActive || stream.pitchPixels > kMaxTimingTotal + 1 ||
	    (stream.pitchPixels % 32) != 0 || stream.cursorWidth > 256 ||
	    (stream.bpc != 8 && stream.bpc != 10 && stream.bpc != 12))
		return false;
	if (stream.link == Link::HdmiTmds)
		return !stream.lanes && !stream.dscBppX16 && !stream.dscSlices &&
		       stream.pixelKHz * stream.bpc <= 600000u * 8u;
	constexpr uint32_t laneKbps[] = {0, 2700000u, 5400000u, 8100000u, 10000000u, 13500000u, 20000000u};
	static_assert(sizeof(laneKbps) / sizeof(laneKbps[0]) == static_cast<unsigned>(Link::DpUhbr20) + 1u);
	const unsigned link = static_cast<unsigned>(stream.link);
	if (link > static_cast<unsigned>(Link::DpUhbr20) ||
	    (stream.lanes != 1 && stream.lanes != 2 && stream.lanes != 4))
		return false;
	const uint64_t rawKbps = uint64_t{laneKbps[link]} * stream.lanes;
	const uint64_t payloadKbps = stream.link >= Link::DpUhbr10 ? rawKbps * 128u / 132u : rawKbps * 8u / 10u;
	const uint32_t bitsPerPixelX16 = stream.dscBppX16 ? stream.dscBppX16 : stream.bpc * 48u;
	if (uint64_t{stream.pixelKHz} * bitsPerPixelX16 > payloadKbps * 16u) return false;
	if (!stream.dscBppX16) return stream.dscSlices == 0;
	return stream.dscBppX16 >= 128 && stream.dscBppX16 <= 256 &&
	       (stream.dscSlices == 1 || stream.dscSlices == 2 || stream.dscSlices == 4 || stream.dscSlices == 8) &&
	       stream.hActive >= stream.dscSlices * 2u && stream.hActive % stream.dscSlices == 0;
}

bool validClockTable(const ClockTable &table) {
	if (!table.count || table.count > kMaxClocks) return false;
	for (uint32_t index = 0; index < table.count; ++index)
		if (table.kHz[index] < 1000 || table.kHz[index] > 10000000 ||
		    (index != 0 && table.kHz[index] < table.kHz[index - 1])) return false;
	return true;
}

bool validSoc(const Soc &soc) {
	return soc.dramChannels >= 2 && soc.dramChannels <= 32 &&
	       soc.dramChannelWidthBytes == 2 && soc.dramTransactionsPerClock == 16 &&
	       soc.mallMiB <= soc.dramChannels * 4 && (soc.mallMiB & (soc.mallMiB - 1u)) == 0 &&
	       soc.uclkBlackoutNs && soc.stutterExitNs &&
	       soc.stutterEnterExitNs >= soc.stutterExitNs &&
	       validClockTable(soc.uclk) && validClockTable(soc.fclk) && validClockTable(soc.dcfclk) &&
	       validClockTable(soc.socclk) && validClockTable(soc.dispclk) &&
	       validClockTable(soc.dppclk) && validClockTable(soc.dtbclk);
}

} // namespace

bool solve(const Input &input, Result &result) {
	result = {};
	if (!input.count || input.count > kMaxStreams || !validSoc(input.soc) ||
	    (input.enableSubVp && (!input.enableFams2 || !input.soc.mallMiB))) return false;
	for (uint32_t index = 0; index < input.count; ++index)
		if (!validStream(input.streams[index])) return false;
	using Allocation = std::unique_ptr<void, decltype(&std::free)>;
	const Allocation workspace(std::calloc(1, dml21WorkspaceBytes()), &std::free);
	const Allocation instance(std::calloc(1, dml21InstanceBytes()), &std::free);
	if (workspace && instance && solveWithDml21(input, result, workspace.get(), instance.get())) return true;
	result = {};
	return false;
}

} // namespace rdna4dml
