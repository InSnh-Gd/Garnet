// Pure-C host unit tests for garnet_rt_motion.h and motion SPIR-V compute ABI.
// Build (GCC):
//   gcc -std=c11 -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-rt-motion.c -lm -o test-rt-motion
// Build (Clang ASan+UBSan):
//   clang -std=c11 -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-rt-motion.c -lm -o test-rt-motion
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_rt_accel.h"
#include "garnet_rt_spv_motion.h"

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

static int feq(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

static rdna4dc_metal3_caps make_rt_caps(uint32_t scratch_align, uint64_t max_geom,
                                        uint64_t max_inst, uint64_t max_prim) {
    rdna4dc_metal3_probe p;
    memset(&p, 0, sizeof(p));
    p.api_1_2 = 1;
    p.buffer_device_address = 1;
    p.ext_deferred_host_ops = 1;
    p.ext_accel_struct = 1;
    p.feat_accel_struct = 1;
    p.feat_accel_indirect_build = 1;
    p.feat_accel_uab = 1;
    p.rt_min_scratch_alignment = scratch_align;
    p.rt_max_geometries = max_geom;
    p.rt_max_instances = max_inst;
    p.rt_max_primitives = max_prim;
    p.ext_ray_query = 1;
    p.feat_ray_query = 1;
    p.ext_ray_tracing_position_fetch = 1;
    p.feat_ray_tracing_position_fetch = 1;
    return rdna4dc_metal3_from_probe(&p);
}

static void test_wave3_motion_blur_math_layout_and_oracles(void) {
    /* 1. Exact ABI struct sizes & magic constants. */
    CHECK(sizeof(rdna4rt_component_transform_t) == 64u &&
          sizeof(rdna4rt_mtl_instance_indirect_motion_t) == 48u &&
          sizeof(rdna4rt_motion_geom_hdr_t) == 48u &&
          sizeof(rdna4rt_motion_tlas_hdr_t) == 32u &&
          sizeof(rdna4rt_motion_tlas_inst_t) == 64u &&
          RDNA4RT_MOTION_GEOM_MAGIC == UINT32_C(0x52544D47) &&
          RDNA4RT_MOTION_TLAS_MAGIC == UINT32_C(0x52544D54),
          "Wave 3.1 motion blur ABI sizes and magic constants pinned");

    /* 2. rdna4rt_motion_time_eval across t in {-0.25, 0.0, 0.25, 0.5, 0.75, 1.0, 1.25}. */
    {
        uint32_t k0 = 99u, k1 = 99u;
        float al = -1.0f;
        /* Clamp left, Vanish right, K=3 over [0, 1]. */
        CHECK(rdna4rt_motion_time_eval(-0.25f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_VANISH, 3u, &k0, &k1, &al) == 1 &&
              k0 == 0u && k1 == 1u && feq(al, 0.0f, 1e-6f),
              "motion_time_eval t=-0.25 Clamp clamps to key 0");
        CHECK(rdna4rt_motion_time_eval(1.25f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_VANISH, 3u, &k0, &k1, &al) == 0,
              "motion_time_eval t=1.25 Vanish vanishes");
        CHECK(rdna4rt_motion_time_eval(-0.25f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_VANISH,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 2u, &k0, &k1, &al) == 0,
              "motion_time_eval t=-0.25 Vanish vanishes");
        CHECK(rdna4rt_motion_time_eval(1.25f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_VANISH,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 2u, &k0, &k1, &al) == 1 &&
              k0 == 1u && k1 == 1u && feq(al, 0.0f, 1e-6f),
              "motion_time_eval t=1.25 Clamp clamps to last key");
        CHECK(rdna4rt_motion_time_eval(0.25f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 5u, &k0, &k1, &al) == 1 &&
              k0 == 1u && k1 == 2u && feq(al, 0.0f, 1e-6f),
              "motion_time_eval t=0.25 K=5 exact key 1");
        CHECK(rdna4rt_motion_time_eval(0.5f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 2u, &k0, &k1, &al) == 1 &&
              k0 == 0u && k1 == 1u && feq(al, 0.5f, 1e-6f),
              "motion_time_eval t=0.5 K=2 midpoint alpha=0.5");
        CHECK(rdna4rt_motion_time_eval(0.75f, 0.0f, 1.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 3u, &k0, &k1, &al) == 1 &&
              k0 == 1u && k1 == 2u && feq(al, 0.5f, 1e-6f),
              "motion_time_eval t=0.75 K=3 brackets (1,2) alpha=0.5");
        CHECK(rdna4rt_motion_time_eval(2.0f, 2.0f, 2.0f, RDNA4RT_MOTION_BORDER_CLAMP,
                                       RDNA4RT_MOTION_BORDER_CLAMP, 3u, &k0, &k1, &al) == 1 &&
              k0 == 0u && k1 == 0u && feq(al, 0.0f, 1e-6f),
              "motion_time_eval zero duration handles cleanly");
        CHECK(rdna4rt_motion_time_eval(NAN, 0.0f, 1.0f, 0u, 0u, 2u, &k0, &k1, &al) == -1 &&
              rdna4rt_motion_time_eval(0.5f, 2.0f, 1.0f, 0u, 0u, 2u, &k0, &k1, &al) == -1,
              "motion_time_eval rejects NaN and start > end");
    }

    /* 3. Quaternion SLERP: 90 deg midpoint = 45 deg, antipodal hemisphere flip, near-identical, zero reject. */
    {
        float s45 = sinf(0.78539816339f);
        float qid[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float q90[4] = { 0.0f, 0.0f, s45, s45 };
        float qmid[4] = { 0 };
        float s225 = sinf(0.39269908169f), c225 = cosf(0.39269908169f);
        CHECK(rdna4rt_quat_slerp(qid, q90, 0.5f, qmid) == 1 &&
              feq(qmid[0], 0.0f, 1e-5f) && feq(qmid[1], 0.0f, 1e-5f) &&
              feq(qmid[2], s225, 1e-5f) && feq(qmid[3], c225, 1e-5f),
              "quat_slerp 90 deg midpoint equals 45 deg rotation");
        float q90_neg[4] = { 0.0f, 0.0f, -s45, -s45 };
        float qmid_anti[4] = { 0 };
        CHECK(rdna4rt_quat_slerp(qid, q90_neg, 0.5f, qmid_anti) == 1 &&
              feq(fabsf(qmid_anti[2]), s225, 1e-5f) && feq(fabsf(qmid_anti[3]), c225, 1e-5f),
              "quat_slerp flips antipodal hemisphere to take short arc");
        float qnear[4] = { 0.0f, 0.0f, 1e-4f, 1.0f };
        CHECK(rdna4rt_quat_slerp(qid, qnear, 0.5f, qmid) == 1 &&
              feq(qmid[3], 1.0f, 1e-4f),
              "quat_slerp linear fallback for near-identical quaternions");
        float qzero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(rdna4rt_quat_slerp(qzero, qid, 0.5f, qmid) == 0,
              "quat_slerp rejects zero quaternion");
    }

    /* 4. MTLComponentTransform exact SRT composition & 3x4 inversion. */
    {
        float s45 = sinf(0.78539816339f);
        rdna4rt_component_transform_t srt = {
            .scale = { 2.0f, 3.0f, 1.0f },
            .shear = { 0.5f, 0.0f, 0.0f },
            .pivot = { 1.0f, 0.0f, 0.0f },
            .rotation = { 0.0f, 0.0f, s45, s45 }, /* +90 deg Z */
            .translation = { 10.0f, 20.0f, 30.0f },
        };
        rdna4rt_vk_transform_t xf;
        CHECK(rdna4rt_component_transform_to_3x4(&srt, &xf) == RDNA4RT_OK,
              "component_transform_to_3x4 succeeds");
        /* Apply to point (0, 2, 1): S*p = (1.0, 6.0, 1.0), + pivot(1,0,0) = (2.0, 6.0, 1.0),
         * R(+90z) = (-6.0, 2.0, 1.0), + translation(10, 20, 30) = (4.0, 22.0, 31.0). */
        float pin[3] = { 0.0f, 2.0f, 1.0f }, pout[3] = { 0 };
        rdna4rt_transform_3x4_point_vec(&xf, pin, NULL, pout, NULL);
        CHECK(feq(pout[0], 4.0f, 1e-4f) && feq(pout[1], 22.0f, 1e-4f) && feq(pout[2], 31.0f, 1e-4f),
              "component_transform exact T * R * (pivot + S * p)");

        rdna4rt_vk_transform_t inv;
        CHECK(rdna4rt_transform_3x4_invert(&xf, &inv) == 1, "transform_3x4_invert succeeds");
        float roundtrip[3] = { 0 };
        rdna4rt_transform_3x4_point_vec(&inv, pout, NULL, roundtrip, NULL);
        CHECK(feq(roundtrip[0], pin[0], 1e-4f) &&
              feq(roundtrip[1], pin[1], 1e-4f) &&
              feq(roundtrip[2], pin[2], 1e-4f),
              "transform_3x4_invert round-trips point to 1e-4");

        rdna4rt_vk_transform_t sing = {{ {1,0,0,0}, {0,0,0,0}, {0,0,1,0} }};
        CHECK(rdna4rt_transform_3x4_invert(&sing, &inv) == 0,
              "transform_3x4_invert rejects singular matrix");
    }

    /* 5. Motion BLAS & TLAS descriptor validators (including INDIRECT_MOTION & Component 64B). */
    {
        rdna4dc_metal3_caps caps = make_rt_caps(256u, 16u, 16u, 1000u);
        int has_vbs[2] = { 1, 1 };
        uint64_t vaddrs[2] = { UINT64_C(0x100000), UINT64_C(0x200000) };
        uint64_t vlens[2] = { 144u, 144u };
        uint64_t voffs[2] = { 0u, 0u };
        rdna4rt_motion_tri_geom_desc_t mtd;
        memset(&mtd, 0, sizeof(mtd));
        mtd.vertex_format = RDNA4RT_VFMT_FLOAT3;
        mtd.keyframe_count = 2u;
        mtd.has_vertex_bufs = has_vbs;
        mtd.vertex_buf_gpu_addrs = vaddrs;
        mtd.vertex_buf_lens = vlens;
        mtd.vertex_buf_offsets = voffs;
        mtd.triangle_count = 4u;
        rdna4rt_motion_tri_plan_t mtp;
        CHECK(rdna4rt_validate_motion_tri_geom(&mtd, 1, 0, &caps, &mtp) == RDNA4RT_ERR_MOTION_UNSUPPORTED,
              "motion tri geom gated off when unsupported");
        CHECK(rdna4rt_validate_motion_tri_geom(&mtd, 1, 1, &caps, &mtp) == RDNA4RT_OK &&
              mtp.payload_bytes == 4u * 2u * RDNA4RT_MOTION_TRI_STRIDE,
              "motion tri geom valid and computes exact payload bytes");
        vlens[1] = 100u;
        CHECK(rdna4rt_validate_motion_tri_geom(&mtd, 1, 1, &caps, &mtp) == RDNA4RT_ERR_BOUNDS,
              "motion tri geom rejects undersized keyframe buffer");

        rdna4rt_motion_aabb_geom_desc_t mab;
        memset(&mab, 0, sizeof(mab));
        uint64_t alens[2] = { 48u, 48u };
        mab.keyframe_count = 2u;
        mab.has_bufs = has_vbs;
        mab.buf_gpu_addrs = vaddrs;
        mab.buf_lens = alens;
        mab.buf_offsets = voffs;
        mab.box_count = 2u;
        rdna4rt_motion_aabb_plan_t map;
        CHECK(rdna4rt_validate_motion_aabb_geom(&mab, 1, 1, &caps, &map) == RDNA4RT_OK &&
              map.payload_bytes == 2u * 2u * RDNA4RT_MOTION_AABB_STRIDE,
              "motion AABB geom valid and computes exact payload bytes");

        rdna4rt_tlas_desc_t ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.descriptor_type = RDNA4RT_INST_DESC_MOTION;
        ctl.instance_count = 1u;
        ctl.has_inst_buf = 1;
        ctl.inst_buf_len = 44u;
        ctl.has_motion_transform_buf = 1;
        ctl.motion_transform_type = RDNA4RT_XFORM_COMPONENT;
        ctl.motion_transform_count = 2u;
        ctl.motion_transform_buf_len = 2u * 64u;
        rdna4rt_tlas_plan_t tp;
        CHECK(rdna4rt_validate_tlas_desc(&ctl, 1, 1, &caps, &tp) == RDNA4RT_OK,
              "Component motion TLAS desc validates 2 * 64B buffer");
        ctl.motion_transform_buf_len = 2u * 64u - 1u;
        CHECK(rdna4rt_validate_tlas_desc(&ctl, 1, 1, &caps, &tp) == RDNA4RT_ERR_BOUNDS,
              "Component motion TLAS desc rejects undersized 64B buffer");

        rdna4rt_indirect_tlas_desc_t imtl;
        memset(&imtl, 0, sizeof(imtl));
        imtl.descriptor_type = RDNA4RT_INST_DESC_INDIRECT_MOTION;
        imtl.max_instance_count = 2u;
        imtl.has_inst_buf = 1;
        imtl.inst_buf_len = 2u * 48u;
        imtl.has_count_buf = 1;
        imtl.count_buf_len = 4u;
        imtl.has_motion_transform_buf = 1;
        imtl.motion_transform_count = 4u;
        imtl.motion_transform_buf_len = 4u * 48u;
        CHECK(rdna4rt_validate_indirect_tlas_desc(&imtl, 1, 1, &caps, &tp) == RDNA4RT_OK &&
              tp.stride == 48u,
              "IndirectMotion TLAS desc validates 48B stride when enabled");
    }

    /* 6. BLAS combined primData + curve + motion sidecar layout & TLAS motion layout. */
    {
        rdna4rt_prim_data_geom_plan_t gp[3];
        memset(gp, 0, sizeof(gp));
        gp[0].present = 1; gp[0].prim_count = 2u; gp[0].copy_bytes = 32u;
        uint32_t ca[3] = { 0u, 2u, 0u };
        uint64_t mp[3] = { 0u, 0u, 288u };
        uint64_t po[3] = { 0 }, so[3] = { 0 }, mo[3] = { 0 };
        uint64_t hb = 0u, ac = 0u, am = 0u, tb = 0u;
        int ap = 0, acv = 0, amot = 0;
        CHECK(rdna4rt_blas_sidecar_layout_compute(gp, ca, mp, 3u, po, &hb, &ac, so, &am, mo, &tb,
                                                  &ap, &acv, &amot) == RDNA4RT_OK &&
              ap == 1 && acv == 1 && amot == 1 && hb == 48u &&
              po[0] == 48u && ac == 80u && so[1] == 224u && am == 384u && mo[2] == 528u &&
              tb == 528u + 288u,
              "BLAS sidecar layout packs primData + curve + motion with exact 16B alignments");

        uint64_t hoff = 0u, ioff = 0u, xoff = 0u, tot = 0u;
        CHECK(rdna4rt_motion_tlas_layout_compute(2u, 3u, RDNA4RT_XFORM_COMPONENT, &hoff, &ioff, &xoff, &tot) == RDNA4RT_OK &&
              hoff == 128u && ioff == 160u && xoff == 288u && tot == 288u + 3u * 64u,
              "TLAS motion sidecar layout computes exact 16B-aligned offsets");
    }

    /* 7. CPU exact-oracle ray-motion-tri & ray-motion-instance intersection across t in [-0.25, 1.25]. */
    {
        /* Triangle translating along +X from [0..1] (t=0) to [2..3] (t=1). */
        rdna4rt_tri_verts_t kf[2] = {
            {{ { 0.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.5f, 1.0f, 0.0f } }},
            {{ { 2.0f, -1.0f, 0.0f }, { 3.0f, -1.0f, 0.0f }, { 2.5f, 1.0f, 0.0f } }},
        };
        const float ro[3] = { 1.5f, 0.0f, 5.0f };
        const float rd[3] = { 0.0f, 0.0f, -1.0f };
        float th = 0.0f, uh = 0.0f, vh = 0.0f;
        int ff = 0;
        /* Normal is (0,0,+2), dot(n, rd) = -2 -> CCW front-facing = 1. */
        CHECK(rdna4rt_cpu_intersect_motion_tri(ro, rd, 0.01f, 100.0f, 0.5f, kf, 2u, 0.0f, 1.0f,
                                               RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_VANISH,
                                               0, 0u, &th, &uh, &vh, &ff) == 1 &&
              feq(th, 5.0f, 1e-5f) && ff == 1 && feq(vh, 0.5f, 1e-5f),
              "motion tri oracle hits x=1.5 at ray_time=0.5 (CCW front)");
        CHECK(rdna4rt_cpu_intersect_motion_tri(ro, rd, 0.01f, 100.0f, 0.5f, kf, 2u, 0.0f, 1.0f,
                                               RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_VANISH,
                                               0, 1u /* cull front */, &th, &uh, &vh, &ff) == 0,
              "motion tri oracle respects front-face culling");
        CHECK(rdna4rt_cpu_intersect_motion_tri(ro, rd, 0.01f, 100.0f, 1.25f, kf, 2u, 0.0f, 1.0f,
                                               RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_VANISH,
                                               0, 0u, &th, &uh, &vh, &ff) == 0,
              "motion tri oracle respects Vanish at t=1.25");

        /* Spinning instance (SRT): centered around origin, translates in Y by [0 -> 4] and rotates 0 -> 90 deg. */
        float s45 = sinf(0.78539816339f);
        rdna4rt_component_transform_t srt_keys[2] = {
            { {1,1,1}, {0,0,0}, {0,0,0}, {0, 0, 0, 1}, {0, 0, 0} },
            { {1,1,1}, {0,0,0}, {0,0,0}, {0, 0, s45, s45}, {0, 4, 0} },
        };
        rdna4rt_tri_verts_t local_tri = {{ { -1.0f, -1.0f, 0.0f }, { 2.0f, -1.0f, 0.0f }, { -1.0f, 2.0f, 0.0f } }};
        const float ro_inst_c[3] = { 0.0f, 0.0f, 5.0f };
        CHECK(rdna4rt_cpu_intersect_motion_inst_tri(
                  ro_inst_c, rd, 0.01f, 100.0f, -0.25f,
                  RDNA4RT_XFORM_COMPONENT, srt_keys, 2u, 0.0f, 1.0f,
                  RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_CLAMP,
                  &local_tri, 1u, 0.0f, 1.0f, 0u, 0u, 0, 0u,
                  &th, &uh, &vh, &ff) == 1 && feq(th, 5.0f, 1e-5f),
              "motion instance oracle Clamp at t=-0.25 hits keyframe 0");
        const float ro_inst_1[3] = { 0.0f, 4.0f, 5.0f };
        CHECK(rdna4rt_cpu_intersect_motion_inst_tri(
                  ro_inst_1, rd, 0.01f, 100.0f, 1.25f,
                  RDNA4RT_XFORM_COMPONENT, srt_keys, 2u, 0.0f, 1.0f,
                  RDNA4RT_MOTION_BORDER_CLAMP, RDNA4RT_MOTION_BORDER_CLAMP,
                  &local_tri, 1u, 0.0f, 1.0f, 0u, 0u, 0, 0u,
                  &th, &uh, &vh, &ff) == 1 && feq(th, 5.0f, 1e-5f),
              "motion instance oracle Clamp at t=1.25 hits translated/rotated keyframe 1");
    }

    CHECK(sizeof(rdna4rt_motion_blas_build_pc_t) == 64u &&
          sizeof(rdna4rt_motion_blas_entry_t) == 24u &&
          sizeof(rdna4rt_motion_tlas_build_pc_t) == 80u,
          "motion compute push-constant & BLAS table structs match shader ABI");
    CHECK(rdna4rt_motion_blas_pack_flags(3u, 2u, 1u, 1u) ==
          (3u | (2u << 4u) | (1u << 8u) | (1u << 12u)),
          "motion_blas_pack_flags encodes vfmt, idx_type, geom_kind, xform_row_major");
    CHECK(sizeof(rdna4rt_spv_motion_blas_build) / sizeof(uint32_t) == 7575u &&
          rdna4rt_spv_motion_blas_build[0] == 0x07230203u &&
          rdna4rt_spv_motion_blas_build[1] == 0x00010600u,
          "embedded motion_blas_build SPIR-V 1.6 magic & size match");
    CHECK(sizeof(rdna4rt_spv_motion_tlas_build) / sizeof(uint32_t) == 9552u &&
          rdna4rt_spv_motion_tlas_build[0] == 0x07230203u &&
          rdna4rt_spv_motion_tlas_build[1] == 0x00010600u,
          "embedded motion_tlas_build SPIR-V 1.6 magic & size match");
}


int main(void) {
    test_wave3_motion_blur_math_layout_and_oracles();
    printf("test-rt-motion: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
