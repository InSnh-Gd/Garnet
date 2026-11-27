#include "fixture.h"
#include "dml_top.h"
#include "bounding_boxes/dcn4_soc_bb.h"

#ifdef NDEBUG
#error "The reference harness requires assertions"
#endif

#define COUNT(a) (sizeof(a) / sizeof(*(a)))

_Static_assert(sizeof(struct dmub_cmd_fams2_global_config) == 28, "FAMS2 global size");
_Static_assert(sizeof(struct dmub_fams2_cmd_stream_static_base_state) == 48, "FAMS2 v2.1 base size");
_Static_assert(sizeof(union dmub_fams2_cmd_stream_static_sub_state) == 20, "FAMS2 v2.1 sub-state size");

static void set_clocks(struct dml2_clk_table *table, const uint32_t *values, size_t count)
{
    memset(table, 0, sizeof(*table));
    table->num_clk_values = count;
    for (size_t index = 0; index < count; ++index)
        table->clk_values_khz[index] = values[index];
}

static struct dml2_instance *initialize(const struct fixture_soc *soc)
{
    struct dml2_initialize_instance_in_out *init = calloc(1, sizeof(*init));
    assert(init);
    init->dml2_instance = calloc(1, dml2_get_instance_size_bytes());
    assert(init->dml2_instance);
    init->options.project_id = dml2_project_dcn4x_stage2_auto_drr_svp;
    init->options.pmo_options.disable_dyn_odm = true;
    init->options.pmo_options.disable_dyn_odm_for_multi_stream = true;
    init->options.pmo_options.disable_dyn_odm_for_stream_with_svp = true;
    init->options.pmo_options.disable_drr_clamped = true;
    init->options.pmo_options.disable_drr_var = true;
    init->soc_bb = dml2_socbb_dcn401;
    init->soc_bb.qos_parameters = dml_dcn4_variant_a_soc_qos_params;
    init->ip_caps = dml2_dcn401_max_ip_caps;
    struct dml2_soc_state_table *clocks = &init->soc_bb.clk_table;
    set_clocks(&clocks->uclk, fixture_uclk_khz, COUNT(fixture_uclk_khz));
    set_clocks(&clocks->fclk, fixture_fclk_khz, COUNT(fixture_fclk_khz));
    set_clocks(&clocks->dcfclk, fixture_dcfclk_khz, COUNT(fixture_dcfclk_khz));
    set_clocks(&clocks->socclk, fixture_socclk_khz, COUNT(fixture_socclk_khz));
    set_clocks(&clocks->dispclk, fixture_dispclk_khz, COUNT(fixture_dispclk_khz));
    set_clocks(&clocks->dppclk, fixture_dppclk_khz, COUNT(fixture_dppclk_khz));
    set_clocks(&clocks->dtbclk, fixture_dtbclk_khz, COUNT(fixture_dtbclk_khz));
    clocks->dram_config.channel_count = soc->dram_channels;
    clocks->dram_config.channel_width_bytes = FIXTURE_DRAM_CHANNEL_WIDTH_BYTES;
    clocks->dram_config.transactions_per_clock = FIXTURE_DRAM_TRANSACTIONS_PER_CLOCK;
    init->soc_bb.mall_allocated_for_dcn_mbytes = soc->mall_mib;
    struct dml2_soc_power_management_parameters *power = &init->soc_bb.power_management_parameters;
    power->dram_clk_change_blackout_us = FIXTURE_UCLK_BLACKOUT_NS / 1000.0;
    power->fclk_change_blackout_us = FIXTURE_FCLK_BLACKOUT_NS / 1000.0;
    power->stutter_enter_plus_exit_latency_us = FIXTURE_STUTTER_ENTER_EXIT_NS / 1000.0;
    power->stutter_exit_latency_us = FIXTURE_STUTTER_EXIT_NS / 1000.0;
    assert(dml2_initialize_instance(init));
    struct dml2_instance *instance = init->dml2_instance;
    free(init);
    return instance;
}

static void describe_mode(const struct fixture_mode *mode, struct dml2_display_cfg *cfg)
{
    cfg->num_planes = cfg->num_streams = mode->count;
    cfg->gpuvm_max_page_table_levels = 4;
    cfg->minimize_det_reallocation = true;
    cfg->overrides.enable_subvp_implicit_pmo = true;
    for (uint32_t index = 0; index < mode->count; ++index) {
        const struct fixture_stream *fixture = &mode->streams[index];
        struct dml2_stream_parameters *stream = &cfg->stream_descriptors[index];
        struct dml2_timing_cfg *timing = &stream->timing;
        timing->h_active = fixture->h_active;
        timing->v_active = fixture->v_active;
        timing->h_total = fixture->h_total;
        timing->v_total = fixture->v_total;
        timing->h_blank_end = fixture->h_total - fixture->h_active - fixture->h_front;
        timing->v_blank_end = fixture->v_total - fixture->v_active - fixture->v_front;
        timing->h_front_porch = fixture->h_front;
        timing->v_front_porch = fixture->v_front;
        timing->h_sync_width = fixture->h_sync;
        timing->pixel_clock_khz = fixture->pixel_khz;
        timing->bpc = fixture->bpc;
        timing->drr_config.disallowed = true;
        timing->dsc.enable = fixture->dsc_slices ? dml2_dsc_enable : dml2_dsc_disable;
        timing->dsc.overrides.num_slices = fixture->dsc_slices;
        timing->dsc.dsc_compressed_bpp_x16 = fixture->dsc_bpp_x16;

        stream->output.output_format = dml2_444;
        stream->output.output_encoder = fixture->link == FIXTURE_DP_HBR3 ? dml2_dp : dml2_hdmi;
        stream->output.output_dp_link_rate = fixture->link == FIXTURE_DP_HBR3 ? dml2_dp_rate_hbr3 : dml2_dp_rate_na;
        stream->output.output_dp_lane_count = fixture->lanes;
        stream->output.validate_output = true;
        stream->overrides.disable_dynamic_odm = true;
        stream->overrides.hw.twait_budgeting.fclk_pstate = dml2_twait_budgeting_setting_if_needed;
        stream->overrides.hw.twait_budgeting.uclk_pstate = dml2_twait_budgeting_setting_if_needed;
        stream->overrides.hw.twait_budgeting.stutter_enter_exit = dml2_twait_budgeting_setting_if_needed;

        struct dml2_plane_parameters *plane = &cfg->plane_descriptors[index];
        plane->stream_index = index;
        plane->pixel_format = dml2_444_32;
        plane->surface.tiling = dml2_sw_linear;
        plane->surface.plane0.width = fixture->h_active;
        plane->surface.plane0.height = fixture->v_active;
        plane->surface.plane0.pitch = fixture->pitch_pixels;
        plane->composition.viewport.plane0.width = fixture->h_active;
        plane->composition.viewport.plane0.height = fixture->v_active;
        plane->composition.scaler_info.plane0.h_ratio = plane->composition.scaler_info.plane0.v_ratio = 1;
        plane->composition.scaler_info.plane1.h_ratio = plane->composition.scaler_info.plane1.v_ratio = 1;
        plane->composition.scaler_info.plane0.h_taps = plane->composition.scaler_info.plane0.v_taps = 1;
        plane->composition.scaler_info.plane1.h_taps = plane->composition.scaler_info.plane1.v_taps = 1;
        plane->composition.scaler_info.rect_out_width = fixture->h_active;
        plane->composition.rect_out_height_spans_vactive = true;
        plane->overrides.gpuvm_min_page_size_kbytes = dml2_socbb_dcn401.gpuvm_min_page_size_kbytes;
        plane->overrides.hostvm_min_page_size_kbytes = dml2_socbb_dcn401.hostvm_min_page_size_kbytes;
        plane->cursor.cursor_width = fixture->cursor_width;
        plane->cursor.cursor_bpp = 32;
        plane->cursor.num_cursors = 1;
        plane->immediate_flip = true;
    }
}

static void print_bytes(const void *obj, size_t size)
{
    const unsigned char *bytes = obj;
    printf("{");
    for (size_t index = 0; index < size; ++index)
        printf("%s%u", index ? "," : "", bytes[index]);
    printf("}");
}

static void print_det(struct dml2_dchub_per_pipe_register_set *const *regs, unsigned int count)
{
    printf("{");
    for (unsigned int index = 0; index < 4; ++index) {
        assert(index >= count || regs[index]);
        printf("%s%u", index ? "," : "", index < count ? regs[index]->det_size : 0);
    }
    printf("}");
}

static void print_result(const struct dml2_display_cfg_programming *programming, uint32_t streams)
{
    const struct dml2_dchub_global_register_set *global = &programming->global_regs;
    assert(global->num_watermark_sets && global->num_watermark_sets <= DML2_DCHUB_WATERMARK_SET_NUM);
    printf("    {true, %u, %u, {", global->arb_regs.compbuf_size, global->num_watermark_sets);
    for (unsigned int index = 0; index < global->num_watermark_sets; ++index) {
        const struct dml2_dchub_watermark_regs *wm = &global->wm_regs[index];
        printf("%s{%u,%u,%u,%u,%u}", index ? "," : "", wm->urgent, wm->sr_enter,
               wm->sr_exit, wm->uclk_pstate, wm->fclk_pstate);
    }
    printf("}, %d, %d, %d, ", programming->uclk_pstate_supported,
           programming->fclk_pstate_supported, programming->fams2_required);
    print_bytes(&programming->fams2_global_config, sizeof(programming->fams2_global_config));
    puts(", {");
    for (uint32_t index = 0; index < streams; ++index) {
        const struct dml2_per_plane_programming *plane = &programming->plane_programming[index];
        const struct dml2_per_stream_programming *stream = &programming->stream_programming[index];
        assert(plane->num_dpps_required && plane->num_dpps_required <= 4);
        printf("        {%u,%u,%u,", plane->num_dpps_required, stream->num_odms_required,
               (unsigned int)plane->uclk_pstate_support_method);
        print_det(plane->pipe_regs, plane->num_dpps_required);
        printf(",");
        print_det(plane->phantom_plane.pipe_regs, plane->phantom_plane.valid ? plane->num_dpps_required : 0);
        printf(",%u,%u,%d,", plane->surface_size_mall_bytes, plane->svp_size_mall_bytes, plane->phantom_plane.valid);
        print_bytes(&stream->fams2_base_params.stream_v1.base, sizeof(stream->fams2_base_params.stream_v1.base));
        printf(",");
        print_bytes(&stream->fams2_sub_params.stream_v1.sub_state, sizeof(stream->fams2_sub_params.stream_v1.sub_state));
        puts("},");
    }
    puts("    }},");
}

int main(void)
{
    puts("static const struct fixture_expected fixture_results[] = {");
    for (size_t chip = 0; chip < COUNT(fixture_socs); ++chip) {
        for (size_t mode = 0; mode < COUNT(fixture_modes); ++mode) {
            struct dml2_instance *instance = initialize(&fixture_socs[chip]);
            struct dml2_display_cfg *display = calloc(1, sizeof(*display));
            struct dml2_display_cfg_programming *programming = calloc(1, sizeof(*programming));
            assert(display && programming);
            describe_mode(&fixture_modes[mode], display);

            struct dml2_check_mode_supported_in_out support = {instance, display, false};
            struct dml2_build_mode_programming_in_out program = {instance, display, programming};
            const bool supported = dml2_check_mode_supported(&support);
            fprintf(stderr, "%s %s: mode support=%d\n", fixture_socs[chip].name, fixture_modes[mode].name, supported);
            assert(supported && support.is_supported);
            assert(dml2_build_mode_programming(&program));
            printf("    /* %s %s */\n", fixture_socs[chip].name, fixture_modes[mode].name);
            print_result(programming, display->num_streams);

            free(programming);
            free(display);
            free(instance);
        }
    }
    puts("};");
}
