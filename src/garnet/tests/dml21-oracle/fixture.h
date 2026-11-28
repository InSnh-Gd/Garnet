#pragma once

#include <stdint.h>

enum fixture_link { FIXTURE_DP_HBR3, FIXTURE_HDMI_TMDS };

struct fixture_stream {
    uint32_t h_active, h_total, h_front, h_sync;
    uint32_t v_active, v_total, v_front, v_sync;
    uint32_t pixel_khz, pitch_pixels, bpc, lanes, dsc_bpp_x16, dsc_slices, cursor_width;
    enum fixture_link link;
};

static const struct fixture_mode {
    const char *name;
    uint32_t count;
    struct fixture_stream streams[2];
} fixture_modes[] = {
    {"4k60", 1, {{3840, 4000, 48, 32, 2160, 2222, 3, 5, 533250, 3840, 8, 4, 0, 0, 256, FIXTURE_DP_HBR3}}},
    {"4k160-dsc", 1, {{3840, 4000, 48, 32, 2160, 2222, 3, 5, 1422080, 3840, 8, 4, 128, 8, 256, FIXTURE_DP_HBR3}}},
    {"dp-hdmi", 2, {{3840, 4000, 48, 32, 2160, 2222, 3, 5, 533250, 3840, 8, 4, 0, 0, 256, FIXTURE_DP_HBR3},
                    {1920, 2200, 88, 44, 1080, 1125, 4, 5, 148500, 1920, 8, 0, 0, 0, 256, FIXTURE_HDMI_TMDS}}},
};

static const struct fixture_soc {
    const char *name;
    uint32_t mall_mib, dram_channels;
} fixture_socs[] = {
    {"navi44", 32, 8},
    {"navi48", 64, 16},
};

static const uint32_t fixture_uclk_khz[] = {97000, 1000000, 1250000};
static const uint32_t fixture_fclk_khz[] = {300000, 2500000};
static const uint32_t fixture_dcfclk_khz[] = {200000, 1564000};
static const uint32_t fixture_dispclk_khz[] = {100000, 2000000};
static const uint32_t fixture_dppclk_khz[] = {100000, 2000000};
static const uint32_t fixture_dtbclk_khz[] = {100000, 1564000};
static const uint32_t fixture_socclk_khz[] = {300000, 1200000};

enum {
    FIXTURE_DRAM_CHANNEL_WIDTH_BYTES = 2,
    FIXTURE_DRAM_TRANSACTIONS_PER_CLOCK = 16,
    FIXTURE_UCLK_BLACKOUT_NS = 400000,
    FIXTURE_FCLK_BLACKOUT_NS = 20000,
    FIXTURE_STUTTER_ENTER_EXIT_NS = 54000,
    FIXTURE_STUTTER_EXIT_NS = 41000,
};
