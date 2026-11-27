// Pure-C host unit tests for garnet_rt_curves.h and curve SPIR-V compute ABI.
// Build (GCC):
//   gcc -std=c11 -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-rt-curves.c -lm -o test-rt-curves
// Build (Clang ASan+UBSan):
//   clang -std=c11 -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-rt-curves.c -lm -o test-rt-curves
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_rt_accel.h"
#include "garnet_rt_spv_curves.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        g_pass++; \
    } else { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        g_fail++; \
    } \
} while (0)

static uint32_t test_lcg(uint32_t *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static float test_randf(uint32_t *state, float lo, float hi) {
    uint32_t v = test_lcg(state) >> 8;
    float u = (float)v / 16777216.0f;
    return lo + (hi - lo) * u;
}

static rdna4rt_vec4_t test_direct_basis_eval(rdna4rt_curve_basis_t basis,
                                             uint32_t cp_count,
                                             const rdna4rt_vec4_t *p,
                                             float u) {
    float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float om = 1.0f - u;
    if (basis == RDNA4RT_CURVE_BASIS_LINEAR && cp_count == 2u) {
        w[0] = om; w[1] = u;
    } else if (basis == RDNA4RT_CURVE_BASIS_BEZIER && cp_count == 3u) {
        w[0] = om * om; w[1] = 2.0f * om * u; w[2] = u * u;
    } else if (basis == RDNA4RT_CURVE_BASIS_BEZIER && cp_count == 4u) {
        w[0] = om * om * om; w[1] = 3.0f * om * om * u;
        w[2] = 3.0f * om * u * u; w[3] = u * u * u;
    } else if (basis == RDNA4RT_CURVE_BASIS_BSPLINE && cp_count == 3u) {
        w[0] = 0.5f * om * om;
        w[1] = 0.5f * (-2.0f * u * u + 2.0f * u + 1.0f);
        w[2] = 0.5f * u * u;
    } else if (basis == RDNA4RT_CURVE_BASIS_BSPLINE && cp_count == 4u) {
        float u2 = u * u, u3 = u2 * u;
        w[0] = (1.0f - 3.0f * u + 3.0f * u2 - u3) / 6.0f;
        w[1] = (4.0f - 6.0f * u2 + 3.0f * u3) / 6.0f;
        w[2] = (1.0f + 3.0f * u + 3.0f * u2 - 3.0f * u3) / 6.0f;
        w[3] = u3 / 6.0f;
    } else if (basis == RDNA4RT_CURVE_BASIS_CATMULL_ROM && cp_count == 4u) {
        float u2 = u * u, u3 = u2 * u;
        w[0] = (-u + 2.0f * u2 - u3) * 0.5f;
        w[1] = (2.0f - 5.0f * u2 + 3.0f * u3) * 0.5f;
        w[2] = (u + 4.0f * u2 - 3.0f * u3) * 0.5f;
        w[3] = (-u2 + u3) * 0.5f;
    }
    rdna4rt_vec4_t out = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0u; i < cp_count; i++) {
        out.x += w[i] * p[i].x;
        out.y += w[i] * p[i].y;
        out.z += w[i] * p[i].z;
        out.w += w[i] * p[i].w;
    }
    return out;
}

static void test_wave2_curve_geom_validation_and_intersectors(void) {
    /* 1. Geometry descriptor validation across bases, CP counts, formats, bounds & alignment. */
    CHECK(RDNA4RT_CURVE_TYPE_ROUND == 0u && RDNA4RT_CURVE_TYPE_FLAT == 1u &&
          RDNA4RT_CURVE_BASIS_BSPLINE == 0u && RDNA4RT_CURVE_BASIS_CATMULL_ROM == 1u &&
          RDNA4RT_CURVE_BASIS_LINEAR == 2u && RDNA4RT_CURVE_BASIS_BEZIER == 3u &&
          RDNA4RT_CURVE_ENDCAPS_NONE == 0u && RDNA4RT_CURVE_ENDCAPS_DISK == 1u &&
          RDNA4RT_CURVE_ENDCAPS_SPHERE == 2u && RDNA4RT_GEOM_CURVES == 3u,
          "SDK-verified curve enum constants exact");

    rdna4rt_curve_geom_t cg;
    memset(&cg, 0, sizeof(cg));
    cg.curve_type = RDNA4RT_CURVE_TYPE_ROUND;
    cg.curve_basis = RDNA4RT_CURVE_BASIS_CATMULL_ROM;
    cg.curve_end_caps = RDNA4RT_CURVE_ENDCAPS_SPHERE;
    cg.segment_control_point_count = 4u;
    cg.segment_count = 4u;
    cg.control_point_count = 7u;
    cg.control_point_format = RDNA4RT_CURVE_CP_FLOAT3;
    cg.has_control_point_buf = 1;
    cg.control_point_buf_gpu_addr = UINT64_C(0x100000);
    cg.control_point_buf_len = 256u;
    cg.control_point_stride = 12u;
    cg.radius_format = RDNA4RT_CURVE_RAD_FLOAT;
    cg.has_radius_buf = 1;
    cg.radius_buf_gpu_addr = UINT64_C(0x200000);
    cg.radius_buf_len = 128u;
    cg.radius_stride = 4u;
    cg.has_index_buf = 1;
    cg.index_type = RDNA4RT_INDEX_UINT32;
    cg.index_buf_gpu_addr = UINT64_C(0x300000);
    cg.index_buf_len = 16u;
    CHECK(rdna4rt_validate_curve_geom(&cg, 1024u) == RDNA4RT_OK,
          "indexed Catmull-Rom 4-CP curve descriptor accepted");

    /* Valid basis/CP combinations: Linear=2, BSpline=3|4, Bezier=3|4, CatmullRom=4, Float2..Half4, Half/Float rad. */
    const rdna4rt_curve_basis_t good_bases[6] = {
        RDNA4RT_CURVE_BASIS_LINEAR, RDNA4RT_CURVE_BASIS_BSPLINE, RDNA4RT_CURVE_BASIS_BSPLINE,
        RDNA4RT_CURVE_BASIS_CATMULL_ROM, RDNA4RT_CURVE_BASIS_BEZIER, RDNA4RT_CURVE_BASIS_BEZIER,
    };
    const uint32_t good_cps[6] = { 2u, 3u, 4u, 4u, 3u, 4u };
    int all_good_combos = 1;
    for (uint32_t i = 0u; i < 6u; i++) {
        rdna4rt_curve_geom_t g2 = cg;
        g2.curve_basis = good_bases[i];
        g2.segment_control_point_count = good_cps[i];
        g2.curve_type = (i & 1u) ? RDNA4RT_CURVE_TYPE_FLAT : RDNA4RT_CURVE_TYPE_ROUND;
        g2.curve_end_caps = (rdna4rt_curve_endcaps_t)(i % 3u);
        g2.control_point_format = (rdna4rt_curve_cp_fmt_t)i;
        g2.control_point_stride = 0u;
        g2.radius_format = (i & 1u) ? RDNA4RT_CURVE_RAD_HALF : RDNA4RT_CURVE_RAD_FLOAT;
        g2.radius_stride = 0u;
        g2.has_index_buf = 0;
        g2.control_point_count = g2.segment_count * g2.segment_control_point_count;
        g2.control_point_buf_len = 512u;
        g2.radius_buf_len = 256u;
        if (rdna4rt_validate_curve_geom(&g2, 0u) != RDNA4RT_OK) all_good_combos = 0;
    }
    CHECK(all_good_combos, "all 6 valid basis/CP-count and CP/radius formats pass");

    /* Invalid basis/CP counts & descriptor errors. */
    {
        rdna4rt_curve_geom_t bad = cg;
        bad.curve_basis = RDNA4RT_CURVE_BASIS_LINEAR; bad.segment_control_point_count = 3u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_INVALID_ARG, "linear 3-CP rejected");

        bad = cg; bad.curve_basis = RDNA4RT_CURVE_BASIS_CATMULL_ROM; bad.segment_control_point_count = 3u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_INVALID_ARG, "Catmull-Rom 3-CP rejected");

        bad = cg; bad.curve_basis = RDNA4RT_CURVE_BASIS_BEZIER; bad.segment_control_point_count = 2u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_INVALID_ARG, "Bezier 2-CP rejected");

        bad = cg; bad.control_point_buf_offset = 2u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_ALIGNMENT, "misaligned Float3 CP offset rejected");

        bad = cg; bad.radius_stride = 6u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_ALIGNMENT, "misaligned Float radius stride rejected");

        bad = cg; bad.index_buf_len = 12u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_BOUNDS, "undersized curve index buffer rejected");

        bad = cg; bad.control_point_buf_gpu_addr = UINT64_MAX - 4u; bad.control_point_buf_offset = 16u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 0u) == RDNA4RT_ERR_OVERFLOW, "CP GPU addr overflow rejected");

        bad = cg; bad.segment_count = 5u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 4u) == RDNA4RT_ERR_LIMIT, "segment_count > max_primitives rejected");

        bad = cg; bad.segment_count = 0u;
        CHECK(rdna4rt_validate_curve_geom(&bad, 4u) == RDNA4RT_OK, "0-segment empty curve geometry accepted");
    }

    /* 2. Basis conversion to cubic Bezier accuracy across all 4 bases at u = {0, 0.25, 0.5, 0.75, 1}. */
    {
        const rdna4rt_vec4_t sample_cps[4] = {
            { -1.2f,  0.4f,  0.8f, 0.05f },
            { -0.3f,  1.5f, -0.4f, 0.12f },
            {  0.7f, -0.9f,  0.6f, 0.09f },
            {  1.6f,  0.2f, -0.2f, 0.03f },
        };
        const float us[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        int basis_ok = 1;
        for (uint32_t bi = 0u; bi < 6u; bi++) {
            rdna4rt_vec4_t bz[4];
            if (rdna4rt_curve_to_bezier_4(good_bases[bi], good_cps[bi], sample_cps, bz) != RDNA4RT_OK) {
                basis_ok = 0;
                break;
            }
            for (uint32_t ui = 0u; ui < 5u; ui++) {
                rdna4rt_vec4_t got, exp_v;
                rdna4rt_curve_eval_bezier(bz, us[ui], &got, NULL);
                exp_v = test_direct_basis_eval(good_bases[bi], good_cps[bi], sample_cps, us[ui]);
                if (fabsf(got.x - exp_v.x) > 1e-5f || fabsf(got.y - exp_v.y) > 1e-5f ||
                    fabsf(got.z - exp_v.z) > 1e-5f || fabsf(got.w - exp_v.w) > 1e-5f) {
                    basis_ok = 0;
                }
            }
        }
        CHECK(basis_ok, "basis-to-Bezier exact across Linear/Bezier(3,4)/BSpline(3,4)/Catmull-Rom");

        rdna4rt_vec4_t nan_cps[2] = { { 0.0f, NAN, 0.0f, 0.1f }, { 1.0f, 0.0f, 0.0f, 0.1f } };
        rdna4rt_vec4_t dummy_b[4];
        CHECK(rdna4rt_curve_to_bezier_4(RDNA4RT_CURVE_BASIS_LINEAR, 2u, nan_cps, dummy_b) == RDNA4RT_ERR_INVALID_ARG,
              "NaN control point rejected by basis converter");
    }

    /* 3. de Casteljau sub-AABB enclosure across 1,000 pseudo-random curves and 4 sub-ranges. */
    {
        uint32_t rng = 0x51A7F00Du;
        const float t0s[4] = { 0.0f, 0.0f, 0.25f, 0.5f };
        const float t1s[4] = { 1.0f, 0.5f, 0.75f, 1.0f };
        int all_enclosed = 1;
        for (int ci = 0; ci < 1000 && all_enclosed; ci++) {
            rdna4rt_vec4_t b[4];
            for (int k = 0; k < 4; k++) {
                b[k] = (rdna4rt_vec4_t){
                    test_randf(&rng, -20.0f, 20.0f),
                    test_randf(&rng, -20.0f, 20.0f),
                    test_randf(&rng, -20.0f, 20.0f),
                    test_randf(&rng, 0.0f, 1.5f),
                };
            }
            for (int ri = 0; ri < 4 && all_enclosed; ri++) {
                rdna4rt_vk_aabb_t box;
                if (rdna4rt_curve_bezier_sub_aabb(b, t0s[ri], t1s[ri], &box) != RDNA4RT_OK) {
                    all_enclosed = 0;
                    break;
                }
                for (int si = 0; si <= 32; si++) {
                    float u = t0s[ri] + (t1s[ri] - t0s[ri]) * ((float)si / 32.0f);
                    rdna4rt_vec4_t p;
                    rdna4rt_curve_eval_bezier(b, u, &p, NULL);
                    float r = p.w > 0.0f ? p.w : 0.0f;
                    if (p.x - r < box.min_x || p.x + r > box.max_x ||
                        p.y - r < box.min_y || p.y + r > box.max_y ||
                        p.z - r < box.min_z || p.z + r > box.max_z) {
                        all_enclosed = 0;
                        break;
                    }
                }
            }
        }
        CHECK(all_enclosed, "1,000 random cubic curves enclosed by de Casteljau sub-AABBs");
    }

    /* 4. CPU analytic linear & cubic intersectors + caps + differential check. */
    {
        rdna4rt_vec4_t lp0 = { 0.0f, -1.0f, 0.0f, 0.25f };
        rdna4rt_vec4_t lp1 = { 0.0f,  1.0f, 0.0f, 0.25f };
        float ro_center[3] = { 0.0f, 0.5f, -5.0f };
        float rd_z[3]      = { 0.0f, 0.0f,  1.0f };
        float th = 0.0f, uh = 0.0f;
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_center, rd_z, 0.001f, 100.0f, &lp0, &lp1,
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_NONE,
                                                 &th, &uh) == 1 &&
              fabsf(th - 4.75f) < 1e-4f && fabsf(uh - 0.75f) < 1e-4f,
              "linear round cylinder hit matches analytic t=4.75, u=0.75");

        /* Endcap comparison along axis ray shooting from below lp0 toward +Y. */
        float ro_axis[3] = { 0.0f, -3.0f, 0.0f };
        float rd_y[3]    = { 0.0f,  1.0f, 0.0f };
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_axis, rd_y, 0.001f, 100.0f, &lp0, &lp1,
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_NONE,
                                                 &th, &uh) == 0,
              "linear axis ray misses open NONE endcaps");
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_axis, rd_y, 0.001f, 100.0f, &lp0, &lp1,
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_DISK,
                                                 &th, &uh) == 1 &&
              fabsf(th - 2.0f) < 1e-4f && fabsf(uh - 0.0f) < 1e-4f,
              "linear axis ray hits DISK endcap at t=2.0, u=0.0");
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_axis, rd_y, 0.001f, 100.0f, &lp0, &lp1,
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                 &th, &uh) == 1 &&
              fabsf(th - 1.75f) < 1e-4f && fabsf(uh - 0.0f) < 1e-4f,
              "linear axis ray hits SPHERE endcap at t=1.75, u=0.0");

        /* Tapered linear segment degree-elevated to cubic: cubic vs linear must agree. */
        rdna4rt_vec4_t tp[2] = { { -1.0f, 0.0f, 0.0f, 0.3f }, { 1.0f, 0.0f, 0.0f, 0.1f } };
        rdna4rt_vec4_t tb[4];
        CHECK(rdna4rt_curve_to_bezier_4(RDNA4RT_CURVE_BASIS_LINEAR, 2u, tp, tb) == RDNA4RT_OK,
              "elevate tapered linear to cubic Bezier");
        float ro_tap[3] = { -0.2f, 0.0f, -4.0f };
        float t_lin = 0.0f, u_lin = 0.0f, t_cub = 0.0f, u_cub = 0.0f;
        int h_lin = rdna4rt_cpu_intersect_curve_linear(ro_tap, rd_z, 0.001f, 100.0f, &tp[0], &tp[1],
                                                       RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                       &t_lin, &u_lin);
        int h_cub = rdna4rt_cpu_intersect_curve_cubic(ro_tap, rd_z, 0.001f, 100.0f, tb,
                                                      RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                      &t_cub, &u_cub);
        CHECK(h_lin == 1 && h_cub == 1 && fabsf(t_lin - t_cub) < 1e-4f && fabsf(u_lin - u_cub) < 1e-4f,
              "tapered linear vs cubic Bezier differential hit agreement");

        /* Slanted ray on tapered linear vs cubic Bezier, plus grazing u=0 overhang check near sphere cap. */
        float ro_sl[3] = { -0.35f, 0.12f, -3.5f };
        float rd_sl[3] = {  0.22f, -0.05f, 0.9742f };
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_sl, rd_sl, 0.001f, 100.0f, &tp[0], &tp[1],
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                 &t_lin, &u_lin) == 1 &&
              rdna4rt_cpu_intersect_curve_cubic(ro_sl, rd_sl, 0.001f, 100.0f, tb,
                                                RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                &t_cub, &u_cub) == 1 &&
              fabsf(t_lin - t_cub) < 1e-4f && fabsf(u_lin - u_cub) < 1e-4f,
              "slanted tapered linear vs cubic Bezier exact differential agreement");
        float ro_cap[3] = { -1.25f, 0.05f, -4.0f };
        float rd_cap[3] = {  0.10f, 0.0f,   0.995f };
        CHECK(rdna4rt_cpu_intersect_curve_linear(ro_cap, rd_cap, 0.001f, 100.0f, &tp[0], &tp[1],
                                                 RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                 &t_lin, &u_lin) == 1 &&
              rdna4rt_cpu_intersect_curve_cubic(ro_cap, rd_cap, 0.001f, 100.0f, tb,
                                                RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_SPHERE,
                                                &t_cub, &u_cub) == 1 &&
              fabsf(t_lin - t_cub) < 1e-4f && fabsf(u_lin - u_cub) < 1e-4f,
              "grazing u=0 sphere cap overhang rejection agrees between linear & cubic");

        /* Flat ribbon curve hit & miss. */
        CHECK(rdna4rt_cpu_intersect_curve_cubic(ro_center, rd_z, 0.001f, 100.0f, tb,
                                                RDNA4RT_CURVE_TYPE_FLAT, RDNA4RT_CURVE_ENDCAPS_NONE,
                                                &th, &uh) == 0,
              "cubic flat ribbon off-center y=0.5 misses");
        float ro_flat[3] = { 0.0f, 0.1f, -4.0f };
        CHECK(rdna4rt_cpu_intersect_curve_cubic(ro_flat, rd_z, 0.001f, 100.0f, tb,
                                                RDNA4RT_CURVE_TYPE_FLAT, RDNA4RT_CURVE_ENDCAPS_NONE,
                                                &th, &uh) == 1 &&
              fabsf(th - 4.0f) < 1e-3f && fabsf(uh - 0.5f) < 1e-2f,
              "cubic flat ribbon hits centerline plane at t=4.0, u=0.5");

        /* Curved cubic arch hit at apex. */
        rdna4rt_vec4_t arch[4] = {
            { -1.0f, 0.0f, 0.0f, 0.1f },
            { -0.5f, 1.0f, 0.0f, 0.1f },
            {  0.5f, 1.0f, 0.0f, 0.1f },
            {  1.0f, 0.0f, 0.0f, 0.1f },
        };
        float ro_arch[3] = { 0.0f, 0.75f, -5.0f };
        CHECK(rdna4rt_cpu_intersect_curve_cubic(ro_arch, rd_z, 0.001f, 100.0f, arch,
                                                RDNA4RT_CURVE_TYPE_ROUND, RDNA4RT_CURVE_ENDCAPS_NONE,
                                                &th, &uh) == 1 &&
              fabsf(th - 4.9f) < 1e-3f && fabsf(uh - 0.5f) < 1e-2f,
              "curved cubic arch hits apex (u=0.5, y=0.75, t=4.9)");
    }

    /* 5. GPU curve sidecar metadata ABI & SPIR-V header pins. */
    CHECK(sizeof(rdna4rt_curve_meta_hdr_t) == 48u &&
          sizeof(rdna4rt_curve_seg_gpu_t) == 80u &&
          sizeof(rdna4rt_curve_build_pc_t) == 64u &&
          RDNA4RT_CURVE_META_MAGIC == UINT32_C(0x52544356),
          "GPU curve sidecar metadata & push-constant sizes pinned (48B hdr, 80B seg, 64B pc)");

    CHECK(rdna4rt_curve_sub_splits(RDNA4RT_CURVE_BASIS_LINEAR) == 1u &&
          rdna4rt_curve_sub_splits(RDNA4RT_CURVE_BASIS_BEZIER) == 2u &&
          rdna4rt_curve_sub_splits(RDNA4RT_CURVE_BASIS_BSPLINE) == 2u &&
          rdna4rt_curve_sub_splits(RDNA4RT_CURVE_BASIS_CATMULL_ROM) == 2u,
          "curve sub-split counts: Linear=1, Bezier/BSpline/CatmullRom=2");

    uint32_t aabb_cnt = 0u;
    CHECK(rdna4rt_curve_aabb_count(10u, 2u, &aabb_cnt) == RDNA4RT_OK && aabb_cnt == 20u &&
          rdna4rt_curve_aabb_count(UINT32_MAX, 2u, &aabb_cnt) == RDNA4RT_ERR_OVERFLOW &&
          rdna4rt_curve_aabb_count(10u, 0u, &aabb_cnt) == RDNA4RT_ERR_INVALID_ARG,
          "curve AABB count multiplication overflow-safe");

    {
        rdna4rt_prim_data_geom_plan_t gp[2];
        memset(gp, 0, sizeof(gp));
        gp[0].present = 1; gp[0].prim_count = 3u; gp[0].copy_bytes = 36u;
        uint32_t c_aabbs[2] = { 0u, 6u };
        uint64_t prim_offs[2] = { 0u, 0u }, seg_offs[2] = { 0u, 0u };
        uint64_t hdr_b = 0u, aux_off = 0u, tot_b = 0u;
        int any_p = 0, any_c = 0;
        CHECK(rdna4rt_blas_curve_layout_compute(gp, c_aabbs, 2u, prim_offs, &hdr_b,
                                                &aux_off, seg_offs, &tot_b, &any_p, &any_c) == RDNA4RT_OK &&
              any_p == 1 && any_c == 1 && hdr_b == 32u &&
              prim_offs[0] == 32u && aux_off == 80u && seg_offs[0] == 0u && seg_offs[1] == 176u &&
              tot_b == 176u + 6u * 80u,
              "BLAS combined primData + curve sidecar layout computes exact 16B-aligned offsets");
        rdna4rt_prim_data_geom_plan_t gp_ov[2];
        memset(gp_ov, 0, sizeof(gp_ov));
        gp_ov[0].present = 1; gp_ov[0].prim_count = 1u; gp_ov[0].copy_bytes = UINT64_MAX - 64u;
        CHECK(rdna4rt_blas_curve_layout_compute(gp_ov, c_aabbs, 2u, prim_offs, &hdr_b,
                                                &aux_off, seg_offs, &tot_b, &any_p, &any_c) == RDNA4RT_ERR_OVERFLOW,
              "BLAS curve sidecar layout rejects u64 size overflow");
    }

    CHECK(rdna4rt_spv_curve_build[0] == 0x07230203u &&
          rdna4rt_spv_curve_build[1] == 0x00010600u,
          "embedded curve_build SPIR-V 1.6 header valid");
}

int main(void) {
    test_wave2_curve_geom_validation_and_intersectors();
    printf("test-rt-curves: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
