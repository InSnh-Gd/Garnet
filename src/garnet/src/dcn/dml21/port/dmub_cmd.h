/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef GARNET_DML21_DMUB_CMD_H
#define GARNET_DML21_DMUB_CMD_H

#include <stdint.h>


/* Define to ensure that the "common" members always appear in the same
 * order in different structs for back compat purposes
 */
#define COMMON_STREAM_STATIC_SUB_STATE \
    struct dmub_fams2_cmd_legacy_stream_static_state legacy; \
    struct dmub_fams2_cmd_subvp_stream_static_state subvp; \
    struct dmub_fams2_cmd_drr_stream_static_state drr;

/* Maximum number of streams on any ASIC. */
#define DMUB_MAX_STREAMS 6

/* Maximum number of planes on any ASIC. */
#define DMUB_MAX_PLANES 6

/* Maximum number of phantom planes on any ASIC */
#define DMUB_MAX_PHANTOM_PLANES ((DMUB_MAX_PLANES) / 2)

#pragma pack(push, 1)
union fw_assisted_mclk_switch_version {
	struct {
		uint8_t minor : 5;
		uint8_t major : 3;
	};
	uint8_t ver;
};
enum fams2_stream_type {
	FAMS2_STREAM_TYPE_NONE = 0,
	FAMS2_STREAM_TYPE_VBLANK = 1,
	FAMS2_STREAM_TYPE_VACTIVE = 2,
	FAMS2_STREAM_TYPE_DRR = 3,
	FAMS2_STREAM_TYPE_SUBVP = 4,
	FAMS2_STREAM_TYPE_ALTERNATE = 5,
};

struct dmub_rect16 {
	/**
	 * Dirty rect x offset.
	 */
	uint16_t x;

	/**
	 * Dirty rect y offset.
	 */
	uint16_t y;

	/**
	 * Dirty rect width.
	 */
	uint16_t width;

	/**
	 * Dirty rect height.
	 */
	uint16_t height;
};

struct plane_pipe_rect {
	struct dmub_rect16 luma;
	struct dmub_rect16 chroma;
};

struct dmub_fams2_cmd_alternate_stream_static_state {
	uint32_t total_bytes_to_copy;
	uint16_t svp0_dst_lines; // per stream
	uint16_t svp1_dst_lines; // per stream
	uint16_t min_lead_dst_lines; // per stream, should be max(nominal_req_limit, vstartup_to_vactive). Does not have to be maxed over all planes
	uint16_t svp_req_limit; // per stream, should be the same value in time between all streams max(2 swaths, dst_y_pre) over all planes
	uint16_t fw_delays;
	uint16_t vstartup_start;
	uint16_t rec_height[3];
	uint16_t viewport_start[3];
	uint16_t viewport_size[3]; // for now size will be the number of lines perpendicular to scan direction (height for 0 / 180, width for 90 and 270)
	uint16_t viewport_start_c[3];
	uint16_t viewport_size_c[3];
	uint16_t surface_pitch[3];
	uint16_t surface_pitch_c[3];
	uint16_t surface_height[3];
	uint16_t surface_height_c[3];
	uint8_t element_size[3];
	uint8_t element_size_c[3];
	uint8_t swizzle_mode[3]; // TODO: Add mapping, should be value used in LSDMA command
	uint8_t vready_offset_lines; // vready offset from vstartup in lines (rounded up, as the actual offset may be a fraction of a line)
	uint16_t dst_y_prefetch_x1000[3];
	uint16_t total_swaths[3];
	uint16_t total_swaths_c[3];
	uint8_t prefetch_swaths[3];
	uint8_t prefetch_swaths_c[3];
	uint8_t swath_height[3];
	uint8_t swath_height_c[3];
	uint16_t block_256b_width[3];
	uint16_t block_256b_height[3];
	uint16_t block_256b_width_c[3];
	uint16_t block_256b_height_c[3];
	uint16_t macro_tile_width[3];
	uint16_t macro_tile_width_c[3];
	union {
		struct {
			uint8_t is_multi_planar : 1;
			uint8_t is_yuv420 : 1;
			uint8_t prefetch_relative_vblank : 1;
			uint8_t vertical_access : 1; // vertical_access = 1 means 90 or 270 rotation
			uint8_t access_direction : 1; // access_direction = 1 means bigger to smaller coordinations (e.g., scan from 2160 to 0 as opposed to regular 0 to 2160)
			uint8_t dcc : 1;
			uint8_t tmz : 1; // TODO: Need to assign outside of DML (DML not aware of TMZ)
		} bits;
		uint8_t all;
	} config[3];
	uint8_t max_cursor_size;
	uint16_t pre_hdl_delta_x1000[3];
	uint16_t pre_hdl_delta_c_x1000[3];
	uint16_t rec_hdl_delta_x1000[3];
	uint16_t rec_hdl_delta_c_x1000[3];
	uint16_t dst_y_per_vm_vblank_x1000[3];
	uint16_t dst_y_per_row_vblank_x1000[3];
	uint16_t dst_y_after_scaler[3];
	uint16_t vinit_prefill[3];
	uint16_t vinit_prefill_c[3];
	uint16_t vratio_x1000[3];
	uint16_t vratio_c_x1000[3];
	struct plane_pipe_rect pipe_viewports[4];
	/* TODO - remove these deprecated vars */
	uint32_t pipe_copy_offset[2][4]; // [2] - SVP0/1, [4] - 4 pipes
	uint32_t pipe_copy_offset_c[2][4];
	/* bits 47:16 of the surface address */
	uint32_t pipe_copy_addr_47_16[2][4]; // [2] - SVP0/1, [4] - 4 pipes
	uint32_t pipe_copy_addr_47_16_c[2][4];
	uint32_t pipe_copy_max_size[2][4];
	uint32_t pipe_copy_max_size_c[2][4];
};

/* static stream state */
struct dmub_fams2_legacy_stream_static_state {
	uint8_t vactive_det_fill_delay_otg_vlines;
	uint8_t programming_delay_otg_vlines;
}; //v0

struct dmub_fams2_subvp_stream_static_state {
	uint16_t vratio_numerator;
	uint16_t vratio_denominator;
	uint16_t phantom_vtotal;
	uint16_t phantom_vactive;
	union {
		struct {
			uint8_t is_multi_planar : 1;
			uint8_t is_yuv420 : 1;
		} bits;
		uint8_t all;
	} config;
	uint8_t programming_delay_otg_vlines;
	uint8_t prefetch_to_mall_otg_vlines;
	uint8_t phantom_otg_inst;
	uint8_t phantom_pipe_mask;
	uint8_t phantom_plane_pipe_masks[DMUB_MAX_PHANTOM_PLANES]; // phantom pipe mask per plane (for flip passthrough)
}; //v0

struct dmub_fams2_drr_stream_static_state {
	uint16_t nom_stretched_vtotal;
	uint8_t programming_delay_otg_vlines;
	uint8_t only_stretch_if_required;
	uint8_t pad[2];
}; //v0

struct dmub_fams2_cmd_legacy_stream_static_state {
	uint16_t vactive_det_fill_delay_otg_vlines;
	uint16_t programming_delay_otg_vlines;
	uint32_t disallow_time_us;
}; //v1

struct dmub_fams2_cmd_subvp_stream_static_state {
	uint16_t vratio_numerator;
	uint16_t vratio_denominator;
	uint16_t phantom_vtotal;
	uint16_t phantom_vactive;
	uint16_t programming_delay_otg_vlines;
	uint16_t prefetch_to_mall_otg_vlines;
	union {
		struct {
			uint8_t is_multi_planar : 1;
			uint8_t is_yuv420 : 1;
		} bits;
		uint8_t all;
	} config;
	uint8_t phantom_otg_inst;
	uint8_t phantom_pipe_mask;
	uint8_t pad0;
	uint8_t phantom_plane_pipe_masks[DMUB_MAX_PHANTOM_PLANES]; // phantom pipe mask per plane (for flip passthrough)
	uint8_t pad1[4 - (DMUB_MAX_PHANTOM_PLANES % 4)];
}; //v1

struct dmub_fams2_cmd_drr_stream_static_state {
	uint16_t nom_stretched_vtotal;
	uint16_t programming_delay_otg_vlines;
	uint8_t only_stretch_if_required;
	uint8_t pad[3];
}; //v1

union dmub_fams2_stream_static_sub_state {
	struct dmub_fams2_legacy_stream_static_state legacy;
	struct dmub_fams2_subvp_stream_static_state subvp;
	struct dmub_fams2_drr_stream_static_state drr;
}; //v0

union dmub_fams2_cmd_stream_static_sub_state {
	COMMON_STREAM_STATIC_SUB_STATE
}; //v1

union dmub_fams2_stream_static_sub_state_v2 {
	COMMON_STREAM_STATIC_SUB_STATE
	struct dmub_fams2_cmd_alternate_stream_static_state alternate;
}; //v2

struct dmub_fams2_stream_static_state {
	enum fams2_stream_type type;
	uint32_t otg_vline_time_ns;
	uint32_t otg_vline_time_ticks;
	uint16_t htotal;
	uint16_t vtotal; // nominal vtotal
	uint16_t vblank_start;
	uint16_t vblank_end;
	uint16_t max_vtotal;
	uint16_t allow_start_otg_vline;
	uint16_t allow_end_otg_vline;
	uint16_t drr_keepout_otg_vline; // after this vline, vtotal cannot be changed
	uint8_t scheduling_delay_otg_vlines; // min time to budget for ready to microschedule start
	uint8_t contention_delay_otg_vlines; // time to budget for contention on execution
	uint8_t vline_int_ack_delay_otg_vlines; // min time to budget for vertical interrupt firing
	uint8_t allow_to_target_delay_otg_vlines; // time from allow vline to target vline
	union {
		struct {
			uint8_t is_drr : 1; // stream is DRR enabled
			uint8_t clamp_vtotal_min : 1; // clamp vtotal to min instead of nominal
			uint8_t min_ttu_vblank_usable : 1; // if min ttu vblank is above wm, no force pstate is needed in blank
			uint8_t imm_restore_drr : 1; // does not wait to latch DRR vtotal on restore
		} bits;
		uint8_t all;
	} config;
	uint8_t otg_inst;
	uint8_t pipe_mask; // pipe mask for the whole config
	uint8_t num_planes;
	uint8_t plane_pipe_masks[DMUB_MAX_PLANES]; // pipe mask per plane (for flip passthrough)
	uint8_t pad[4 - (DMUB_MAX_PLANES % 4)];
	union dmub_fams2_stream_static_sub_state sub_state;
}; //v0

struct dmub_fams2_cmd_stream_static_base_state {
	enum fams2_stream_type type;
	uint32_t otg_vline_time_ns;
	uint32_t otg_vline_time_ticks;
	uint16_t htotal;
	uint16_t vtotal; // nominal vtotal
	uint16_t vblank_start;
	uint16_t vblank_end;
	uint16_t max_vtotal;
	uint16_t allow_start_otg_vline;
	uint16_t allow_end_otg_vline;
	uint16_t drr_keepout_otg_vline; // after this vline, vtotal cannot be changed
	uint16_t scheduling_delay_otg_vlines; // min time to budget for ready to microschedule start
	uint16_t contention_delay_otg_vlines; // time to budget for contention on execution
	uint16_t vline_int_ack_delay_otg_vlines; // min time to budget for vertical interrupt firing
	uint16_t allow_to_target_delay_otg_vlines; // time from allow vline to target vline
	union {
		struct {
			uint8_t is_drr : 1; // stream is DRR enabled
			uint8_t clamp_vtotal_min : 1; // clamp vtotal to min instead of nominal
			uint8_t min_ttu_vblank_usable : 1; // if min ttu vblank is above wm, no force pstate is needed in blank
			uint8_t imm_restore_drr : 1; // does not wait to latch DRR vtotal on restore
		} bits;
		uint8_t all;
	} config;
	uint8_t otg_inst;
	uint8_t pipe_mask; // pipe mask for the whole config
	uint8_t num_planes;
	uint8_t plane_pipe_masks[DMUB_MAX_PLANES]; // pipe mask per plane (for flip passthrough)
	uint8_t pad[4 - (DMUB_MAX_PLANES % 4)];
}; //v1

struct dmub_fams2_stream_static_state_v1 {
	struct dmub_fams2_cmd_stream_static_base_state base;
	union dmub_fams2_stream_static_sub_state_v2 sub_state;
}; //v1

/**
 * enum dmub_fams2_allow_delay_check_mode - macroscheduler mode for breaking on excessive
 * p-state request to allow latency
 */
enum dmub_fams2_allow_delay_check_mode {
	/* No check for request to allow delay */
	FAMS2_ALLOW_DELAY_CHECK_NONE = 0,
	/* Check for request to allow delay */
	FAMS2_ALLOW_DELAY_CHECK_FROM_START = 1,
	/* Check for prepare to allow delay */
	FAMS2_ALLOW_DELAY_CHECK_FROM_PREPARE = 2,
};

union dmub_fams2_global_feature_config {
	struct {
		uint32_t enable: 1;
		uint32_t enable_ppt_check: 1;
		uint32_t enable_stall_recovery: 1;
		uint32_t enable_debug: 1;
		uint32_t enable_offload_flip: 1;
		uint32_t enable_visual_confirm: 1;
		uint32_t allow_delay_check_mode: 2;
		uint32_t legacy_method_no_fams2 : 1;
		uint32_t alternate_channel_workaround : 1;
		uint32_t reserved: 22;
	} bits;
	uint32_t all;
};

struct dmub_cmd_fams2_global_config {
	uint32_t max_allow_delay_us; // max delay to assert allow from uclk change begin
	uint32_t lock_wait_time_us; // time to forecast acquisition of lock
	uint32_t num_streams;
	union dmub_fams2_global_feature_config features;
	uint32_t recovery_timeout_us;
	uint32_t hwfq_flip_programming_delay_us;
	uint32_t max_allow_to_target_delta_us; // how early DCN could assert P-State allow compared to the P-State target
};

union dmub_cmd_fams2_config {
	struct dmub_cmd_fams2_global_config global;
	struct dmub_fams2_stream_static_state stream; //v0
	union {
		struct dmub_fams2_cmd_stream_static_base_state base;
		union dmub_fams2_cmd_stream_static_sub_state sub_state;
	} stream_v1; //v1
};
#pragma pack(pop)

#endif
