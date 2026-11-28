#include "dcn41_dml21_soc.h"
#include "dcn41_mall_pure.h"
#include "dml21_adapter/dcn41_dml21_adapter.h"

#if defined(KERNEL)
#error "DML2.1 SoC preparation uses the host reference model"
#endif

namespace rdna4dml {
namespace {

bool copyClock(const rdna4dal::ClockSnapshotRow &row, bool hasDcQuery, bool useDcCap,
               ClockTable &output) {
	if (row.rawCount == UINT32_MAX || !row.count || row.count > rdna4dal::kDpmMaxLevels ||
	    row.count != rdna4dal::level_count(row.rawCount) || row.fineGrained != rdna4dal::level_fine_grained(row.rawCount) ||
	    row.rawDcMax > 10000u || row.dcMaxKHz != row.rawDcMax * 1000u ||
	    (!hasDcQuery && row.dcMaxKHz) || row.dcMaxKHz > row.kHz[row.count - 1]) return false;
	for (uint32_t index = 0; index < row.count; ++index) {
		const uint32_t value = row.kHz[index];
		if (value < 1000u || value > 10000000u || value % 1000u ||
		    (index && value < row.kHz[index - 1])) return false;
	}
	const uint32_t limit = useDcCap ? row.dcMaxKHz : 0;
	for (uint32_t index = 0; index < row.count; ++index) {
		const uint32_t value = row.kHz[index];
		if (limit && value > limit) {
			if (!index || row.kHz[index - 1] < limit) output.kHz[output.count++] = limit;
			break;
		}
		output.kHz[output.count++] = value;
	}
	return true;
}

} // namespace

bool prepareSoc(amdgpu::AsicId asic, const rdna4atom::Board &board,
                const rdna4dal::ClockSnapshot &clocks, PowerSource source, Soc &output) {
	output = {};
	const auto &raw = clocks.references;
	if (raw.pllRequest == UINT32_MAX || !(raw.pllRequest & 0x1ffu) || !rdna4dal::dfs_did_ok(raw.dpRefDfs) ||
	    raw.dchubTimer == UINT32_MAX || !(raw.dchubTimer & rdna4dal::kDchubTimerEnable)) return false;
	const uint32_t vco = rdna4dal::vco_khz(raw.pllRequest);
	const uint32_t dchub = (raw.dchubTimer & 0xfu) == 2u ? board.crystalKhz / 2u : board.crystalKhz;
	const uint32_t channels = clocks.umcChannels ? clocks.umcChannels : board.channels;
	if ((source != PowerSource::AC && source != PowerSource::DC) ||
	    board.channels < 2u || board.channels > 32u || channels < 2u || channels > 32u ||
	    board.channelWidthBytes != 2u || dchub < 20000u || dchub > 50000u || vco > 10000000u ||
	    board.dramClockChange100Ns > UINT32_MAX / 100 || board.stutterEnterExit100Ns > UINT32_MAX / 100 ||
	    board.stutterExit100Ns > UINT32_MAX / 100) return false;

	Soc soc {};
	soc.references = {rdna4dal::did_khz(vco, raw.dpRefDfs), dchub, vco};
	soc.crystalKHz = board.crystalKhz;
	soc.dramChannels = channels;
	soc.dramChannelWidthBytes = board.channelWidthBytes;
	soc.mallMiB = static_cast<uint32_t>(rdna4mall::display_budget(asic, channels) >> 20);
	if (!soc.mallMiB) return false;
	dcn401SocDefaults(soc);
	if (board.dramClockChange100Ns) soc.uclkBlackoutNs = board.dramClockChange100Ns * 100u;
	if (board.stutterExit100Ns) soc.stutterExitNs = board.stutterExit100Ns * 100u;
	if (board.stutterEnterExit100Ns) soc.stutterEnterExitNs = board.stutterEnterExit100Ns * 100u;
	if (soc.stutterEnterExitNs < soc.stutterExitNs) return false;

	const bool dc = source == PowerSource::DC;
	if (!copyClock(clocks.dcfclk, true, dc, soc.dcfclk) || !copyClock(clocks.socclk, true, dc, soc.socclk) ||
	    !copyClock(clocks.dtbclk, true, dc, soc.dtbclk) || !copyClock(clocks.dispclk, true, dc, soc.dispclk) ||
	    !copyClock(clocks.dppclk, false, dc, soc.dppclk) || !copyClock(clocks.uclk, true, dc, soc.uclk) ||
	    !copyClock(clocks.fclk, true, dc, soc.fclk)) return false;
	output = soc;
	return true;
}

} // namespace rdna4dml
