#include "dcn41_dml21_adapter.h"
#include "../dcn41_dml21_bridge.h"

extern "C" {
#include "dml_top.h"
}
#include "bounding_boxes/dcn4_soc_bb.h"

#include <string.h>

namespace rdna4dml {
namespace {

struct Workspace {
	dml2_initialize_instance_in_out init;
	dml2_display_cfg display;
	dml2_display_cfg_programming programming;
};

void setClockTable(dml2_clk_table &output, const ClockTable &input) {
	static_assert(kMaxClocks == DML_MAX_CLK_TABLE_SIZE);
	output = {};
	output.num_clk_values = static_cast<unsigned char>(input.count);
	for (uint32_t index = 0; index < input.count; ++index) output.clk_values_khz[index] = input.kHz[index];
}

void initSoc(dml2_initialize_instance_in_out &init, const Input &input) {
	init.soc_bb = dml2_socbb_dcn401;
	init.soc_bb.qos_parameters = dml_dcn4_variant_a_soc_qos_params;
	init.ip_caps = dml2_dcn401_max_ip_caps;
	const auto &soc = input.soc;
	auto &clocks = init.soc_bb.clk_table;
	setClockTable(clocks.uclk, soc.uclk);
	setClockTable(clocks.fclk, soc.fclk);
	setClockTable(clocks.dcfclk, soc.dcfclk);
	setClockTable(clocks.socclk, soc.socclk);
	setClockTable(clocks.dispclk, soc.dispclk);
	setClockTable(clocks.dppclk, soc.dppclk);
	setClockTable(clocks.dtbclk, soc.dtbclk);
	clocks.dram_config.channel_count = soc.dramChannels;
	clocks.dram_config.channel_width_bytes = soc.dramChannelWidthBytes;
	clocks.dram_config.transactions_per_clock = soc.dramTransactionsPerClock;
	init.soc_bb.mall_allocated_for_dcn_mbytes = soc.mallMiB;
	auto &power = init.soc_bb.power_management_parameters;
	power.dram_clk_change_blackout_us = soc.uclkBlackoutNs / 1000.0;
	power.fclk_change_blackout_us = soc.fclkBlackoutNs / 1000.0;
	power.stutter_enter_plus_exit_latency_us = soc.stutterEnterExitNs / 1000.0;
	power.stutter_exit_latency_us = soc.stutterExitNs / 1000.0;
	init.options.project_id = dml2_project_dcn4x_stage2_auto_drr_svp;
	auto &pmo = init.options.pmo_options;
	pmo.disable_fams2 = !input.enableFams2;
	pmo.disable_svp = !input.enableSubVp;
	pmo.disable_drr_clamped = pmo.disable_drr_var = true;
	pmo.disable_dyn_odm = pmo.disable_dyn_odm_for_multi_stream = pmo.disable_dyn_odm_for_stream_with_svp = true;
}

void setStream(dml2_stream_parameters &destination, const Stream &source) {
	auto &timing = destination.timing;
	timing.h_active = source.hActive;
	timing.h_total = source.hTotal;
	timing.h_front_porch = source.hFront;
	timing.h_sync_width = source.hSync;
	timing.h_blank_end = source.hTotal - source.hActive - source.hFront;
	timing.v_active = source.vActive;
	timing.v_total = source.vTotal;
	timing.v_front_porch = source.vFront;
	timing.v_blank_end = source.vTotal - source.vActive - source.vFront;
	timing.pixel_clock_khz = source.pixelKHz;
	timing.bpc = source.bpc;
	timing.drr_config.disallowed = true;
	timing.dsc.enable = source.dscBppX16 ? dml2_dsc_enable : dml2_dsc_disable;
	timing.dsc.dsc_compressed_bpp_x16 = source.dscBppX16;
	timing.dsc.overrides.num_slices = source.dscSlices;
	auto &output = destination.output;
	destination.overrides.disable_dynamic_odm = true;
	auto &budget = destination.overrides.hw.twait_budgeting;
	budget.fclk_pstate = budget.uclk_pstate = budget.stutter_enter_exit = dml2_twait_budgeting_setting_if_needed;
	output.output_format = dml2_444;
	output.output_dp_lane_count = source.lanes;
	output.validate_output = true;
	switch (source.link) {
	case Link::HdmiTmds: output.output_encoder = dml2_hdmi; output.output_dp_link_rate = dml2_dp_rate_na; return;
	case Link::DpHbr: output.output_dp_link_rate = dml2_dp_rate_hbr; break;
	case Link::DpHbr2: output.output_dp_link_rate = dml2_dp_rate_hbr2; break;
	case Link::DpHbr3: output.output_dp_link_rate = dml2_dp_rate_hbr3; break;
	case Link::DpUhbr10: output.output_dp_link_rate = dml2_dp_rate_uhbr10; break;
	case Link::DpUhbr13_5: output.output_dp_link_rate = dml2_dp_rate_uhbr13p5; break;
	case Link::DpUhbr20: output.output_dp_link_rate = dml2_dp_rate_uhbr20; break;
	}
	output.output_encoder = source.link >= Link::DpUhbr10 ? dml2_dp2p0 : dml2_dp;
}

void setPlane(dml2_plane_parameters &destination, uint32_t index, const Stream &source,
              const dml2_soc_bb &soc) {
	destination.stream_index = index;
	destination.pixel_format = dml2_444_32;
	destination.surface.tiling = dml2_sw_linear;
	destination.surface.plane0 = {source.pitchPixels, source.hActive, source.vActive};
	auto &composition = destination.composition;
	composition.rect_out_height_spans_vactive = true;
	composition.viewport.plane0.width = source.hActive;
	composition.viewport.plane0.height = source.vActive;
	composition.scaler_info.plane0 = {1.0, 1.0, 1, 1};
	composition.scaler_info.plane1 = {1.0, 1.0, 1, 1};
	composition.scaler_info.rect_out_width = source.hActive;
	destination.immediate_flip = true;
	if (source.cursorWidth) {
		destination.cursor.num_cursors = 1;
		destination.cursor.cursor_width = source.cursorWidth;
		destination.cursor.cursor_bpp = 32;
	}
	destination.overrides.gpuvm_min_page_size_kbytes = soc.gpuvm_min_page_size_kbytes;
	destination.overrides.hostvm_min_page_size_kbytes = soc.hostvm_min_page_size_kbytes;
}

bool mapResult(const dml2_display_cfg_programming &programming, uint32_t count, Result &result) {
	const auto &global = programming.global_regs;
	if (!global.num_watermark_sets || global.num_watermark_sets > kMaxWatermarks) return false;
	result.watermarkCount = global.num_watermark_sets;
	result.compbufSegments = global.arb_regs.compbuf_size;
	result.uclkPstateSupported = programming.uclk_pstate_supported;
	result.fclkPstateSupported = programming.fclk_pstate_supported;
	result.fams2Required = programming.fams2_required;
	static_assert(sizeof(result.fams2Global) == sizeof(programming.fams2_global_config));
	memcpy(result.fams2Global, &programming.fams2_global_config, sizeof(result.fams2Global));
	for (uint32_t index = 0; index < result.watermarkCount; ++index) {
		const auto &registers = global.wm_regs[index];
		result.watermarks[index] = {registers.urgent, registers.sr_enter, registers.sr_exit,
		                            registers.uclk_pstate, registers.fclk_pstate};
	}
	for (uint32_t index = 0; index < count; ++index) {
		const auto &plane = programming.plane_programming[index];
		const auto &stream = programming.stream_programming[index];
		auto &output = result.streams[index];
		if (!plane.num_dpps_required || plane.num_dpps_required > kMaxStreams) return false;
		output.dppCount = plane.num_dpps_required;
		output.odmCount = stream.num_odms_required;
		output.pstateMethod = stream.uclk_pstate_method;
		output.surfaceMallBytes = plane.surface_size_mall_bytes;
		output.svpMallBytes = plane.svp_size_mall_bytes;
		output.phantom = plane.phantom_plane.valid;
		for (uint32_t pipe = 0; pipe < output.dppCount; ++pipe) {
			if (!plane.pipe_regs[pipe]) return false;
			output.detSegments[pipe] = plane.pipe_regs[pipe]->det_size;
			if (output.phantom) {
				if (!plane.phantom_plane.pipe_regs[pipe]) return false;
				output.phantomDetSegments[pipe] = plane.phantom_plane.pipe_regs[pipe]->det_size;
			}
		}
		const auto &base = stream.fams2_base_params.stream_v1.base;
		const auto &sub = stream.fams2_sub_params.stream_v1.sub_state;
		static_assert(sizeof(output.fams2Base) == sizeof(base));
		static_assert(sizeof(output.fams2Sub) == sizeof(sub));
		memcpy(output.fams2Base, &base, sizeof(output.fams2Base));
		memcpy(output.fams2Sub, &sub, sizeof(output.fams2Sub));
	}
	return true;
}

} // namespace

size_t dml21WorkspaceBytes() { return sizeof(Workspace); }
size_t dml21InstanceBytes() { return dml2_get_instance_size_bytes(); }

bool solveWithDml21(const Input &input, Result &result, void *workspaceMemory, void *instanceMemory) {
	auto *workspace = static_cast<Workspace *>(workspaceMemory);
	workspace->init.dml2_instance = static_cast<dml2_instance *>(instanceMemory);
	initSoc(workspace->init, input);
	if (!dml2_initialize_instance(&workspace->init)) return false;
	auto &display = workspace->display;
	display.num_planes = display.num_streams = input.count;
	display.gpuvm_max_page_table_levels = 4;
	display.minimize_det_reallocation = true;
	display.overrides.enable_subvp_implicit_pmo = input.enableSubVp;
	for (uint32_t index = 0; index < input.count; ++index) {
		setStream(display.stream_descriptors[index], input.streams[index]);
		setPlane(display.plane_descriptors[index], index, input.streams[index], workspace->init.soc_bb);
	}
	dml2_build_mode_programming_in_out request{workspace->init.dml2_instance, &display, &workspace->programming};
	dml2_check_mode_supported_in_out support{workspace->init.dml2_instance, &display, false};
	return dml2_check_mode_supported(&support) && support.is_supported &&
	       dml2_build_mode_programming(&request) && mapResult(workspace->programming, input.count, result);
}

} // namespace rdna4dml
