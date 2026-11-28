#pragma once

#include <stdbool.h>
#include <stdint.h>

struct fixture_expected_watermarks {
    uint32_t urgent, sr_enter, sr_exit, uclk_pstate, fclk_pstate;
};

struct fixture_expected_stream {
    uint32_t dpp_count, odm_count, pstate_method;
    uint32_t det_segments[4], phantom_det_segments[4];
    uint32_t surface_mall_bytes, svp_mall_bytes;
    bool phantom;
    uint8_t fams2_base[48], fams2_sub[20];
    uint32_t phantom_vertical[3];
    uint32_t phantom_luma_ttu[4][6];
};

struct fixture_expected {
    bool supported;
    uint32_t compbuf_segments, watermark_count;
    struct fixture_expected_watermarks watermarks[4];
    bool uclk_pstate_supported, fclk_pstate_supported, fams2_required;
    uint8_t fams2_global[28];
    struct fixture_expected_stream streams[2];
};

#include "expected.inc"
