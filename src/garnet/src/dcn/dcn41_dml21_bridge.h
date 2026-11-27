#pragma once

#include <stdint.h>

namespace rdna4dml {

constexpr uint32_t kMaxStreams = 4;
constexpr uint32_t kMaxClocks = 20;
constexpr uint32_t kMaxWatermarks = 4;

enum class Link : uint8_t { HdmiTmds, DpHbr, DpHbr2, DpHbr3, DpUhbr10, DpUhbr13_5, DpUhbr20 };

struct ClockTable {
	uint32_t count;
	uint32_t kHz[kMaxClocks];
};

struct Soc {
	uint32_t mallMiB;
	uint32_t dramChannels;
	uint32_t dramChannelWidthBytes;
	uint32_t dramTransactionsPerClock;
	ClockTable uclk, fclk, dcfclk, socclk, dispclk, dppclk, dtbclk;
	uint32_t uclkBlackoutNs, fclkBlackoutNs, stutterEnterExitNs, stutterExitNs;
};

struct Stream {
	uint32_t hActive, hTotal, hFront, hSync;
	uint32_t vActive, vTotal, vFront, vSync;
	uint32_t pixelKHz, pitchPixels;
	Link link;
	uint8_t bpc, lanes, dscSlices;
	uint16_t dscBppX16, cursorWidth;
};

struct Input {
	Soc soc;
	uint32_t count;
	Stream streams[kMaxStreams];
	bool enableFams2, enableSubVp;
};

// DCHUB register encodings, not time in nanoseconds.
struct Watermarks {
	uint32_t urgent, srEnter, srExit, uclkPstate, fclkPstate;
};

struct StreamResult {
	uint32_t dppCount, odmCount, pstateMethod;
	uint32_t detSegments[kMaxStreams], phantomDetSegments[kMaxStreams];
	uint32_t surfaceMallBytes, svpMallBytes;
	bool phantom;
	uint8_t fams2Base[48], fams2Sub[20];
};

struct Result {
	uint32_t compbufSegments, watermarkCount;
	Watermarks watermarks[kMaxWatermarks];
	StreamResult streams[kMaxStreams];
	bool uclkPstateSupported, fclkPstateSupported, fams2Required;
	uint8_t fams2Global[28];
};

// One unscaled, linear ARGB8888 plane per stream. FAMS2 outputs lack the final
// OTG/HUBP mapping and must not be submitted as firmware commands.
bool solve(const Input &input, Result &result);

} // namespace rdna4dml
