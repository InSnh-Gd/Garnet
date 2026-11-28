#include "dcn/dcn41_dml21_soc.h"
#include "dml21-oracle/fixture.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace rdna4dml;

static void require(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		std::exit(1);
	}
}

template <size_t Count>
static rdna4dal::ClockSnapshotRow row(const uint32_t (&kHz)[Count], uint32_t dcMaxMHz = 0) {
	rdna4dal::ClockSnapshotRow result {Count, Count, {}, dcMaxMHz, dcMaxMHz * 1000u, false};
	for (size_t index = 0; index < Count; ++index) result.kHz[index] = kHz[index];
	return result;
}

static Input display(const fixture_mode &mode) {
	Input input {};
	input.count = mode.count;
	for (uint32_t index = 0; index < mode.count; ++index) {
		const auto &stream = mode.streams[index];
		input.streams[index] = {stream.h_active, stream.h_total, stream.h_front, stream.h_sync,
		    stream.v_active, stream.v_total, stream.v_front, stream.v_sync, stream.pixel_khz, stream.pitch_pixels,
		    stream.link == FIXTURE_DP_HBR3 ? Link::DpHbr3 : Link::HdmiTmds,
		    static_cast<uint8_t>(stream.bpc), static_cast<uint8_t>(stream.lanes), static_cast<uint8_t>(stream.dsc_slices),
		    static_cast<uint16_t>(stream.dsc_bpp_x16), static_cast<uint16_t>(stream.cursor_width)};
	}
	return input;
}

static void tests() {
	const rdna4atom::Board board {8, 2, 100000, 4000, 410, 540};
	const rdna4dal::ClockSnapshot clocks {
	    row(fixture_dcfclk_khz, 1300), row(fixture_socclk_khz), row(fixture_dtbclk_khz),
	    row(fixture_dispclk_khz), row(fixture_dppclk_khz), row(fixture_uclk_khz, 1000), row(fixture_fclk_khz),
	    8, {0x1234002du, 0x19u, 0x1002u}};
	Input input = display(fixture_modes[0]);
	auto missingDfs = clocks;
	missingDfs.references.dpRefDfs = 0;
	require(!prepareSoc(amdgpu::AsicId::Navi44, board, missingDfs, PowerSource::AC, input.soc) &&
	        !input.soc.dramChannels, "a missing raw DPREF divider refuses the whole snapshot");
	require(prepareSoc(amdgpu::AsicId::Navi44, board, clocks, PowerSource::AC, input.soc),
	        "complete board and PMFW table produce an AC SoC");
	require(input.soc.mallMiB == 32 && input.soc.dramChannels == 8 && input.soc.dramChannelWidthBytes == 2 &&
	        input.soc.dramTransactionsPerClock == 16 && input.soc.dcfclk.count == 2 &&
	        input.soc.dcfclk.kHz[1] == 1564000 && input.soc.uclk.count == 3 &&
	        input.soc.uclkBlackoutNs == 400000 && input.soc.stutterExitNs == 41000 &&
	        input.soc.stutterEnterExitNs == 54000 && input.soc.references.vcoKHz == 4507110 &&
	        input.soc.references.dpRefKHz == 721137 && input.soc.references.dchubRefKHz == 50000,
	        "AC preserves clock levels, decodes fractional PLL/DFS and divides the board crystal for DCHUB");
	Result result {};
	require(solve(input, result) && result.streams[0].dppCount == 1 && result.compbufSegments == 1 &&
	        result.streams[0].detSegments[0] == 20 && result.streams[0].surfaceMallBytes == 33177600,
	        "the solver programs one 4K60 linear plane (independent reference topology and size)");
	const auto watermark50MHz = result.watermarks[0].urgent;
	auto crystal50MHz = board;
	crystal50MHz.crystalKhz = 50000u;
	Input slowerReference = input;
	require(prepareSoc(amdgpu::AsicId::Navi44, crystal50MHz, clocks, PowerSource::AC, slowerReference.soc) &&
	        solve(slowerReference, result) && result.watermarks[0].urgent > 0 &&
	        watermark50MHz >= 2u * result.watermarks[0].urgent && watermark50MHz <= 2u * result.watermarks[0].urgent + 1u,
	        "halving the observed DCHUB reference halves urgent watermark cycles, within one cycle of truncation");

	require(prepareSoc(amdgpu::AsicId::Navi44, board, clocks, PowerSource::DC, input.soc) &&
	        input.soc.uclk.count == 2 && input.soc.uclk.kHz[1] == 1000000 &&
	        input.soc.dcfclk.count == 2 && input.soc.dcfclk.kHz[1] == 1300000 &&
	        input.soc.dppclk.kHz[1] == 2000000,
	        "DC drops rows over an exact cap or ends with the non-row cap; no DPPCLK DC query");
	auto broken = clocks;
	broken.dispclk = {};
	require(!prepareSoc(amdgpu::AsicId::Navi44, board, broken, PowerSource::AC, input.soc) &&
	        !input.soc.dramChannels, "missing a single PMFW domain clears the candidate");
	broken = clocks;
	broken.references.dchubTimer = 0x1000u;
	require(!prepareSoc(amdgpu::AsicId::Navi44, board, broken, PowerSource::AC, input.soc) &&
	        prepareSoc(amdgpu::AsicId::Navi44, crystal50MHz, broken, PowerSource::AC, input.soc) &&
	        input.soc.references.dchubRefKHz == 50000u, "undivided XTAL is valid only within the DCHUB range");
	broken = clocks;
	broken.dcfclk.rawCount = 0x80000000u;
	broken.dcfclk.fineGrained = true;
	broken.dcfclk.kHz[0] = broken.dcfclk.kHz[1];
	require(prepareSoc(amdgpu::AsicId::Navi48, board, broken, PowerSource::DC, input.soc) &&
	        input.soc.dcfclk.count == 1 && input.soc.dcfclk.kHz[0] == 1300000,
	        "pinned limits may produce a sole legal endpoint below the original first row");

	auto vbiosDefaults = board;
	vbiosDefaults.dramClockChange100Ns = vbiosDefaults.stutterExit100Ns = vbiosDefaults.stutterEnterExit100Ns = 0;
	broken = clocks;
	broken.umcChannels = 0;
	require(prepareSoc(amdgpu::AsicId::Navi44, vbiosDefaults, broken, PowerSource::AC, input.soc) &&
	        input.soc.dramChannels == 8 && input.soc.uclkBlackoutNs == 400000 &&
	        input.soc.fclkBlackoutNs == 0 && input.soc.stutterExitNs == 41000 &&
	        input.soc.stutterEnterExitNs == 54000 && solve(input, result),
	        "zero firmware channels and unspecified VBIOS latencies use the pinned model");
	std::puts("DCN 4.01 board/PMFW SoC and host solve: PASS");
}

static void replay(const char *vfctPath, const char *observationPath) {
	std::ifstream observation(observationPath);
	uint32_t bus, slot, function, device, subVendor, subDevice;
	std::string policy;
	rdna4dal::ClockSnapshot clocks {};
	require(static_cast<bool>(observation >> std::hex >> bus >> slot >> function >> device >> subVendor >> subDevice
	    >> policy >> clocks.references.pllRequest >> clocks.references.dpRefDfs >> clocks.references.dchubTimer >> std::dec
	    >> clocks.umcChannels), "PCI identity, AC/DC, three raw reference registers (hex) and PMFW UMC channels");
	require(bus <= 255 && slot <= 31 && function <= 7 && device <= 65535 && subVendor > 0 && subVendor < 65535 &&
	        subDevice <= 65535 && (policy == "AC" || policy == "DC"), "PCI identity and power policy");
	const auto *asic = amdgpu::asic_from_pci(0x1002, static_cast<uint16_t>(device));
	require(asic != nullptr, "supported RDNA4 device");
	for (const auto &clock : rdna4dal::kSnapshotClocks) {
		std::string name;
		auto &observed = clocks.*clock.row;
		require(static_cast<bool>(observation >> name >> std::hex >> observed.rawCount >> std::dec >> observed.rawDcMax) &&
		    name == clock.name && observed.rawDcMax <= 10000, "ordered clock name, raw count (hex), DC max (MHz)");
		observed.fineGrained = rdna4dal::level_fine_grained(observed.rawCount);
		observed.count = rdna4dal::level_count(observed.rawCount);
		observed.dcMaxKHz = observed.rawDcMax * 1000u;
		require(observed.count > 0 && observed.count <= rdna4dal::kDpmMaxLevels, "observed PMFW clock count");
		for (uint32_t index = 0; index < observed.count; ++index)
			require(static_cast<bool>(observation >> observed.kHz[index]), "all observed PMFW levels (kHz)");
	}

	Input input {};
	require(static_cast<bool>(observation >> input.count) && input.count > 0 && input.count <= kMaxStreams, "stream count");
	for (uint32_t index = 0; index < input.count; ++index) {
		auto &stream = input.streams[index];
		uint32_t link, bpc, lanes, slices, compressedBpp, cursor;
		require(static_cast<bool>(observation >> stream.hActive >> stream.hTotal >> stream.hFront >> stream.hSync
		    >> stream.vActive >> stream.vTotal >> stream.vFront >> stream.vSync >> stream.pixelKHz >> stream.pitchPixels
		    >> link >> bpc >> lanes >> slices >> compressedBpp >> cursor) && link <= static_cast<unsigned>(Link::DpUhbr20) &&
		    bpc <= UINT8_MAX && lanes <= UINT8_MAX && slices <= UINT8_MAX && compressedBpp <= UINT16_MAX &&
		    cursor <= UINT16_MAX, "stream geometry, link and pixel format");
		stream.link = static_cast<Link>(link);
		stream.bpc = static_cast<uint8_t>(bpc);
		stream.lanes = static_cast<uint8_t>(lanes);
		stream.dscSlices = static_cast<uint8_t>(slices);
		stream.dscBppX16 = static_cast<uint16_t>(compressedBpp);
		stream.cursorWidth = static_cast<uint16_t>(cursor);
	}
	std::string trailing;
	require(!(observation >> trailing) && observation.eof(), "no trailing observation fields");

	std::ifstream vfct(vfctPath, std::ios::binary);
	require(static_cast<bool>(vfct), "open VFCT");
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(vfct)), std::istreambuf_iterator<char>());
	const rdna4atom::PciIdentity identity {static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(function),
	    0x1002, static_cast<uint16_t>(device), static_cast<uint16_t>(subVendor), static_cast<uint16_t>(subDevice)};
	rdna4atom::Board board {};
	require(rdna4atom::parse_vfct(bytes.data(), bytes.size(), identity, board), "validated VFCT board");
	require(prepareSoc(asic->id, board, clocks, policy == "DC" ? PowerSource::DC : PowerSource::AC,
	    input.soc), "complete measured SoC input");
	Result result {};
	require(solve(input, result), "DML2.1 mode support and programming");
	std::printf("offline solve: %s UMC=%u width_bytes=%u MALL_MiB=%u compbuf=%u wm_sets=%u\n",
	    policy.c_str(), input.soc.dramChannels, input.soc.dramChannelWidthBytes, input.soc.mallMiB,
	    result.compbufSegments, result.watermarkCount);
	std::printf("reference kHz: VCO=%u DPREF=%u DCHUB=%u\n", input.soc.references.vcoKHz,
	    input.soc.references.dpRefKHz, input.soc.references.dchubRefKHz);
	for (uint32_t index = 0; index < input.count; ++index)
		std::printf("stream %u: dpp=%u odm=%u det0=%u surface_MALL_bytes=%u\n", index, result.streams[index].dppCount,
		    result.streams[index].odmCount, result.streams[index].detSegments[0], result.streams[index].surfaceMallBytes);
}

int main(int argc, char **argv) {
	if (argc == 1) tests();
	else if (argc == 3) replay(argv[1], argv[2]);
	else {
		std::fputs("Usage: dcn41_dml21_soc_test [VFCT observations.txt]\n"
		    "Hex PCI: BUS SLOT FUNCTION DEVICE SUBVENDOR SUBDEVICE\n"
		    "AC|DC CLK0_PLL_REQ_HEX CLK0_DPREF_DFS_HEX DCHUB_GLOBAL_TIMER_CNTL_HEX UMC_CHANNELS\n"
		    "Seven rows: NAME RAW_COUNT_HEX DC_MAX_MHZ LEVEL_KHZ... (DPPCLK DC max is 0)\n"
		    "Order: DCFCLK SOCCLK DTBCLK DISPCLK DPPCLK UCLK FCLK\n"
		    "Stream count then each stream: H_ACTIVE H_TOTAL H_FRONT H_SYNC V_ACTIVE V_TOTAL V_FRONT V_SYNC\n"
		    " PIXEL_KHZ PITCH_PIXELS LINK BPC LANES DSC_SLICES DSC_BPP_X16 CURSOR_WIDTH\n"
		    "Link: 0=HDMI TMDS 1=HBR 2=HBR2 3=HBR3 4=UHBR10 5=UHBR13.5 6=UHBR20\n", stderr);
		return 2;
	}
}
