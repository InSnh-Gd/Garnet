// Pure-C host unit tests for garnet_rt_accel.h (Task 7.1).
// Build (GCC):
//   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-accel.c -lm -o test-rt-accel
// Build (Clang ASan+UBSan):
//   clang -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-accel.c -lm -o test-rt-accel
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_rt_accel.h"

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

static void test_caps_and_alignment(void) {
    rdna4dc_metal3_caps fb = rdna4dc_metal3_fallback();
    CHECK(!rdna4rt_accel_supported(&fb), "fallback must not claim accel support");
    CHECK(!rdna4rt_raytracing_supported(&fb), "fallback must not claim RT support");
    CHECK(!rdna4rt_motion_blur_supported(&fb), "fallback must not claim motion blur");
    CHECK(rdna4rt_scratch_align(&fb) == 0u, "fallback scratch align is 0");

    rdna4dc_metal3_probe p;
    memset(&p, 0, sizeof(p));
    p.api_1_2 = 1;
    p.buffer_device_address = 1;
    p.ext_deferred_host_ops = 1;
    p.ext_accel_struct = 1;
    p.feat_accel_struct = 1;
    p.rt_min_scratch_alignment = 128u;
    rdna4dc_metal3_caps no_rq = rdna4dc_metal3_from_probe(&p);
    CHECK(rdna4rt_accel_supported(&no_rq), "accel supported without ray_query");
    CHECK(!rdna4rt_raytracing_supported(&no_rq), "raytracing gated on ray_query");
    CHECK(rdna4rt_scratch_align(&no_rq) == 128u, "probed 128B scratch align");

    rdna4dc_metal3_caps full = make_rt_caps(256u, 1024u, 4096u, 100000u);
    CHECK(rdna4rt_accel_supported(&full), "full accel supported");
    CHECK(rdna4rt_raytracing_supported(&full), "full raytracing supported");
    CHECK(!rdna4rt_motion_blur_supported(&full), "motion blur stays honestly false");
    CHECK(rdna4rt_scratch_align(&full) == 256u, "probed 256B scratch align");

    rdna4dc_metal3_caps bad_al = make_rt_caps(192u, 1024u, 4096u, 100000u);
    CHECK(rdna4rt_scratch_align(&bad_al) == 256u, "non-pow2 scratch align falls back to 256");

    uint64_t v = 0;
    CHECK(rdna4rt_align_up_u64(0u, 256u, &v) && v == 0u, "align_up 0");
    CHECK(rdna4rt_align_up_u64(1u, 256u, &v) && v == 256u, "align_up 1 -> 256");
    CHECK(rdna4rt_align_up_u64(256u, 256u, &v) && v == 256u, "align_up 256 -> 256");
    CHECK(!rdna4rt_align_up_u64(10u, 3u, &v), "align_up rejects non-pow2");
    CHECK(!rdna4rt_align_up_u64(UINT64_MAX - 10u, 256u, &v), "align_up catches overflow");
    CHECK(!rdna4rt_add_u64(UINT64_MAX, 1u, &v), "add_u64 overflow");
    CHECK(!rdna4rt_mul_u64(UINT64_MAX / 2u + 1u, 2u, &v), "mul_u64 overflow");
}

static void test_usage_sizes_and_scratch(void) {
    uint32_t f = 0;
    CHECK(rdna4rt_usage_to_vk_build_flags(RDNA4RT_USAGE_NONE, &f) == RDNA4RT_OK &&
          f == (RDNA4RT_VK_BUILD_ALLOW_COMPACTION | RDNA4RT_VK_BUILD_PREFER_FAST_TRACE),
          "default usage -> COMPACTION | FAST_TRACE");
    CHECK(rdna4rt_usage_to_vk_build_flags(RDNA4RT_USAGE_REFIT | RDNA4RT_USAGE_PREFER_FAST_BUILD, &f) == RDNA4RT_OK &&
          f == (RDNA4RT_VK_BUILD_ALLOW_COMPACTION | RDNA4RT_VK_BUILD_ALLOW_UPDATE | RDNA4RT_VK_BUILD_PREFER_FAST_BUILD),
          "refit + fast build flags");
    CHECK(rdna4rt_usage_to_vk_build_flags(RDNA4RT_USAGE_MINIMIZE_MEMORY | RDNA4RT_USAGE_PREFER_FAST_INTERSECTION | RDNA4RT_USAGE_EXTENDED_LIMITS, &f) == RDNA4RT_OK &&
          f == (RDNA4RT_VK_BUILD_ALLOW_COMPACTION | RDNA4RT_VK_BUILD_PREFER_FAST_TRACE | RDNA4RT_VK_BUILD_LOW_MEMORY),
          "minimize memory + fast intersection + extended limits");
    CHECK(rdna4rt_usage_to_vk_build_flags(RDNA4RT_USAGE_PREFER_FAST_BUILD | RDNA4RT_USAGE_PREFER_FAST_INTERSECTION, &f) == RDNA4RT_ERR_CONFLICT,
          "conflicting fast build + fast intersection rejected");
    CHECK(rdna4rt_usage_to_vk_build_flags(1u << 3, &f) == RDNA4RT_ERR_INVALID_ARG,
          "reserved usage bit rejected");

    /* VK_KHR_ray_tracing_position_fetch ALLOW_DATA_ACCESS flag gating (triangle BLAS only). */
    CHECK(RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS == 0x00000800u,
          "RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS matches Vulkan bit 11");
    rdna4dc_metal3_caps full_pf = make_rt_caps(256u, 1024u, 4096u, 100000u);
    rdna4dc_metal3_caps no_pf = full_pf;
    no_pf.ray_tracing_position_fetch = 0;
    uint32_t base_f = RDNA4RT_VK_BUILD_ALLOW_COMPACTION | RDNA4RT_VK_BUILD_PREFER_FAST_TRACE;
    CHECK(rdna4rt_blas_build_flags_with_caps(base_f, 1, &full_pf) ==
              (base_f | RDNA4RT_VK_BUILD_ALLOW_DATA_ACCESS),
          "triangle BLAS with position_fetch sets ALLOW_DATA_ACCESS");
    CHECK(rdna4rt_blas_build_flags_with_caps(base_f, 0, &full_pf) == base_f,
          "AABB-only BLAS omits ALLOW_DATA_ACCESS");
    CHECK(rdna4rt_blas_build_flags_with_caps(base_f, 1, &no_pf) == base_f,
          "triangle BLAS without position_fetch omits ALLOW_DATA_ACCESS");
    CHECK(rdna4rt_blas_build_flags_with_caps(base_f, 1, NULL) == base_f,
          "NULL caps omits ALLOW_DATA_ACCESS");

    rdna4rt_sizes_t sz;
    CHECK(rdna4rt_compute_sizes(100u, 1024u, 512u, RDNA4RT_USAGE_NONE, 256u, &sz) == RDNA4RT_OK,
          "compute sizes without refit");
    CHECK(sz.acceleration_structure_size == 256u, "AS size rounded to 256");
    CHECK(sz.build_scratch_size == 1280u, "build scratch padded + aligned for arbitrary offset");
    CHECK(sz.refit_scratch_size == 0u, "refit scratch 0 when UsageRefit unset");

    CHECK(rdna4rt_compute_sizes(0u, 1000u, 300u, RDNA4RT_USAGE_REFIT, 128u, &sz) == RDNA4RT_OK,
          "compute sizes with refit + 0 vk AS size");
    CHECK(sz.acceleration_structure_size == 256u, "0 AS size clamped to 256");
    CHECK(sz.build_scratch_size == 1152u, "build scratch 1000+127 aligned to 128 = 1152");
    CHECK(sz.refit_scratch_size == 512u, "refit scratch 300+127 aligned to 128 = 512");

    CHECK(rdna4rt_compute_sizes(256u, 1024u, 0u, RDNA4RT_USAGE_NONE, 0u, &sz) == RDNA4RT_ERR_ALIGNMENT,
          "0 scratch align rejected");
    CHECK(rdna4rt_compute_sizes(UINT64_MAX, 1024u, 0u, RDNA4RT_USAGE_NONE, 256u, &sz) == RDNA4RT_ERR_OVERFLOW,
          "AS size overflow rejected");
    CHECK(rdna4rt_compute_sizes(256u, UINT64_MAX - 10u, 0u, RDNA4RT_USAGE_NONE, 256u, &sz) == RDNA4RT_ERR_OVERFLOW,
          "scratch size overflow rejected");

    /* Scratch address resolution for arbitrary unaligned client offset. */
    uint64_t addr = 0;
    CHECK(rdna4rt_resolve_scratch_addr(0x10000u, 1280u, 16u, 1024u, 256u, &addr) == RDNA4RT_OK &&
          addr == 0x10100u, "unaligned offset 16 aligns up to 0x10100 within padded 1280B scratch");
    CHECK(rdna4rt_resolve_scratch_addr(0x10000u, 1279u, 16u, 1024u, 256u, &addr) == RDNA4RT_ERR_BOUNDS,
          "undersized scratch by 1 byte rejected");
    CHECK(rdna4rt_resolve_scratch_addr(0u, 1024u, 0u, 1024u, 256u, &addr) == RDNA4RT_ERR_INVALID_ARG,
          "null scratch BDA rejected when required > 0");
    CHECK(rdna4rt_resolve_scratch_addr(0u, 0u, 0u, 0u, 256u, &addr) == RDNA4RT_OK && addr == 0u,
          "0 required scratch allows null scratch buffer");
    CHECK(rdna4rt_resolve_scratch_addr(UINT64_MAX - 8u, 512u, 16u, 64u, 256u, &addr) == RDNA4RT_ERR_OVERFLOW,
          "scratch BDA + offset overflow rejected");

    /* Compacted size write validation (UInt32 / UInt64). */
    CHECK(rdna4rt_validate_compacted_size_write(8u, 4u, RDNA4RT_SIZE_TYPE_UINT32) == RDNA4RT_OK, "uint32 offset 4 ok");
    CHECK(rdna4rt_validate_compacted_size_write(8u, 2u, RDNA4RT_SIZE_TYPE_UINT32) == RDNA4RT_ERR_ALIGNMENT, "uint32 offset 2 misaligned");
    CHECK(rdna4rt_validate_compacted_size_write(16u, 8u, RDNA4RT_SIZE_TYPE_UINT64) == RDNA4RT_OK, "uint64 offset 8 ok");
    CHECK(rdna4rt_validate_compacted_size_write(16u, 4u, RDNA4RT_SIZE_TYPE_UINT64) == RDNA4RT_ERR_ALIGNMENT, "uint64 offset 4 misaligned");
    CHECK(rdna4rt_validate_compacted_size_write(8u, 4u, RDNA4RT_SIZE_TYPE_UINT64) == RDNA4RT_ERR_ALIGNMENT, "uint64 offset 4 misaligned");
    CHECK(rdna4rt_validate_compacted_size_write(8u, 8u, RDNA4RT_SIZE_TYPE_UINT64) == RDNA4RT_ERR_BOUNDS, "uint64 oob");
    CHECK(rdna4rt_validate_compacted_size_write(16u, 0u, 2u) == RDNA4RT_ERR_INVALID_ARG, "invalid compacted size type");
}

static void test_blas_triangle_and_aabb(void) {
    rdna4dc_metal3_caps caps = make_rt_caps(256u, 4u, 16u, 100u);

    /* Non-indexed Float3 triangles (with Metal default indexType=UINT32 ignored when has_index_buf=0). */
    rdna4rt_tri_desc_t td;
    memset(&td, 0, sizeof(td));
    td.vertex_format = RDNA4RT_VFMT_FLOAT3;
    td.has_vertex_buf = 1;
    td.vertex_buf_gpu_addr = 0x20000u;
    td.vertex_buf_len = 72u; /* 2 triangles = 6 verts * 12B */
    td.vertex_buf_offset = 0u;
    td.vertex_stride = 0u; /* default 12 */
    td.has_index_buf = 0;
    td.index_type = RDNA4RT_INDEX_UINT32;
    td.triangle_count = 2u;
    td.opaque = 1;
    td.allow_duplicate_any_hit = 0;

    rdna4rt_tri_plan_t tp;
    CHECK(rdna4rt_validate_triangle_geom(&td, 1, &caps, &tp) == RDNA4RT_OK, "non-indexed float3 valid");
    CHECK(tp.vertex_stride == 12u && tp.max_vertex == 5u && tp.effective_index_type == RDNA4RT_INDEX_NONE,
          "non-indexed stride/max_vertex/index_type");
    CHECK(tp.vk_geom_flags == (RDNA4RT_VK_GEOM_OPAQUE | RDNA4RT_VK_GEOM_NO_DUPLICATE_ANY_HIT),
          "geom flags opaque + no-dup-any-hit");

    /* Indexed UINT16 with strided 16B vertices + column-major transform buffer. */
    td.vertex_stride = 16u;
    td.vertex_buf_len = 60u; /* offsets 0, 16, 32, 48 -> 4 verts (last needs 48+12=60) */
    td.has_index_buf = 1;
    td.index_type = RDNA4RT_INDEX_UINT16;
    td.index_buf_gpu_addr = 0x30000u;
    td.index_buf_len = 14u;
    td.index_buf_offset = 2u; /* 2 + 2*3*2 = 14 */
    td.has_transform_buf = 1;
    td.transform_buf_gpu_addr = 0x40000u;
    td.transform_buf_len = 64u;
    td.transform_buf_offset = 16u;
    td.transform_layout = RDNA4RT_MATRIX_COLUMN_MAJOR;
    CHECK(rdna4rt_validate_triangle_geom(&td, 1, &caps, &tp) == RDNA4RT_OK, "indexed uint16 + col-major transform");
    CHECK(tp.max_vertex == 3u && tp.index_addr == 0x30002u && tp.needs_transform_staging == 1,
          "indexed max_vertex=3, col-major needs staging");

    /* Row-major aligned transform requires no staging; unaligned row-major does. */
    td.transform_layout = RDNA4RT_MATRIX_ROW_MAJOR;
    CHECK(rdna4rt_validate_triangle_geom(&td, 1, &caps, &tp) == RDNA4RT_OK && tp.needs_transform_staging == 0,
          "row-major 16B-aligned needs no staging");
    td.transform_buf_offset = 4u;
    CHECK(rdna4rt_validate_triangle_geom(&td, 1, &caps, &tp) == RDNA4RT_OK && tp.needs_transform_staging == 1,
          "row-major 4B-aligned needs staging");

    /* Size-query mode allows nil buffers. */
    rdna4rt_tri_desc_t qd;
    memset(&qd, 0, sizeof(qd));
    qd.vertex_format = RDNA4RT_VFMT_HALF4;
    qd.triangle_count = 10u;
    CHECK(rdna4rt_validate_triangle_geom(&qd, 0, &caps, &tp) == RDNA4RT_OK && tp.vertex_stride == 8u && tp.max_vertex == 29u,
          "size-query mode succeeds without buffers");
    CHECK(rdna4rt_validate_triangle_geom(&qd, 1, &caps, &tp) == RDNA4RT_ERR_INVALID_ARG,
          "build mode rejects missing vertex buffer");

    /* Negative triangle checks. */
    rdna4rt_tri_desc_t bad = td;
    bad.vertex_buf_len = 59u;
    bad.has_index_buf = 0;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_BOUNDS, "undersized vertex buffer rejected");
    bad = td; bad.vertex_stride = 8u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_INVALID_ARG, "stride < elem_bytes rejected");
    bad = td; bad.vertex_stride = 14u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_ALIGNMENT, "misaligned vertex stride rejected");
    bad = td; bad.index_buf_offset = 1u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_ALIGNMENT, "misaligned uint16 index offset rejected");
    bad = td; bad.index_buf_len = 13u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_BOUNDS, "undersized index buffer rejected");
    bad = td; bad.triangle_count = 101u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 0, &caps, &tp) == RDNA4RT_ERR_LIMIT, "exceeds rt_max_primitives rejected");
    bad = td; bad.transform_buf_len = 47u; bad.transform_buf_offset = 0u;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 1, &caps, &tp) == RDNA4RT_ERR_BOUNDS, "undersized transform buffer rejected");
    bad = td; bad.vertex_format = RDNA4RT_VFMT_COUNT;
    CHECK(rdna4rt_validate_triangle_geom(&bad, 0, &caps, &tp) == RDNA4RT_ERR_UNSUPPORTED, "invalid vertex format rejected");

    /* AABB geometry validation. */
    rdna4rt_aabb_desc_t ad;
    memset(&ad, 0, sizeof(ad));
    ad.has_buf = 1;
    ad.buf_gpu_addr = 0x50000u;
    ad.buf_len = 88u; /* offset 8 + (3-1)*24 + 24 = 80 <= 88 */
    ad.buf_offset = 8u;
    ad.stride = 0u; /* default 24 */
    ad.box_count = 3u;
    ad.opaque = 0;
    ad.allow_duplicate_any_hit = 1;
    rdna4rt_aabb_plan_t ap;
    CHECK(rdna4rt_validate_aabb_geom(&ad, 1, &caps, &ap) == RDNA4RT_OK &&
          ap.stride == 24u && ap.data_addr == 0x50008u && ap.vk_geom_flags == 0u,
          "valid AABB geometry");

    rdna4rt_aabb_desc_t bad_a = ad; bad_a.stride = 28u;
    CHECK(rdna4rt_validate_aabb_geom(&bad_a, 1, &caps, &ap) == RDNA4RT_ERR_ALIGNMENT, "AABB stride not 8B-aligned rejected");
    bad_a = ad; bad_a.stride = 16u;
    CHECK(rdna4rt_validate_aabb_geom(&bad_a, 1, &caps, &ap) == RDNA4RT_ERR_INVALID_ARG, "AABB stride < 24 rejected");
    bad_a = ad; bad_a.buf_offset = 4u;
    CHECK(rdna4rt_validate_aabb_geom(&bad_a, 1, &caps, &ap) == RDNA4RT_ERR_ALIGNMENT, "AABB offset not 8B-aligned rejected");
    bad_a = ad; bad_a.buf_len = 79u;
    CHECK(rdna4rt_validate_aabb_geom(&bad_a, 1, &caps, &ap) == RDNA4RT_ERR_BOUNDS, "AABB undersized buffer rejected");
    bad_a = ad; bad_a.box_count = 101u;
    CHECK(rdna4rt_validate_aabb_geom(&bad_a, 0, &caps, &ap) == RDNA4RT_ERR_LIMIT, "AABB exceeds rt_max_primitives rejected");

    CHECK(rdna4rt_validate_blas_totals(4u, 100u, RDNA4RT_USAGE_NONE, &caps) == RDNA4RT_OK, "blas totals at limit ok");
    CHECK(rdna4rt_validate_blas_totals(5u, 100u, RDNA4RT_USAGE_NONE, &caps) == RDNA4RT_ERR_LIMIT, "blas geom count > limit rejected");
    CHECK(rdna4rt_validate_blas_totals(1u, 101u, RDNA4RT_USAGE_NONE, &caps) == RDNA4RT_ERR_LIMIT, "blas prim count > limit rejected");
}

static void test_tlas_and_instance_conversion(void) {
    rdna4dc_metal3_caps caps = make_rt_caps(256u, 16u, 8u, 1000u);

    /* Exact 4x3 column-major -> 3x4 row-major transform transpose test. */
    rdna4rt_packed_float4x3_t col = {{
        { 1.0f, 2.0f, 3.0f },   /* col 0 */
        { 4.0f, 5.0f, 6.0f },   /* col 1 */
        { 7.0f, 8.0f, 9.0f },   /* col 2 */
        { 10.0f, 20.0f, 30.0f } /* col 3 (translation) */
    }};
    rdna4rt_vk_transform_t row;
    CHECK(rdna4rt_transpose_4x3_to_3x4(&col, &row) == RDNA4RT_OK, "transpose valid matrix");
    /* Verify M * p for p = (2, -3, 5, 1) matches column-major linear combination. */
    const float px = 2.0f, py = -3.0f, pz = 5.0f;
    float exp_x = col.columns[0][0] * px + col.columns[1][0] * py + col.columns[2][0] * pz + col.columns[3][0];
    float exp_y = col.columns[0][1] * px + col.columns[1][1] * py + col.columns[2][1] * pz + col.columns[3][1];
    float exp_z = col.columns[0][2] * px + col.columns[1][2] * py + col.columns[2][2] * pz + col.columns[3][2];
    float got_x = row.matrix[0][0] * px + row.matrix[0][1] * py + row.matrix[0][2] * pz + row.matrix[0][3];
    float got_y = row.matrix[1][0] * px + row.matrix[1][1] * py + row.matrix[1][2] * pz + row.matrix[1][3];
    float got_z = row.matrix[2][0] * px + row.matrix[2][1] * py + row.matrix[2][2] * pz + row.matrix[2][3];
    CHECK(feq(got_x, exp_x, 1e-6f) && feq(got_y, exp_y, 1e-6f) && feq(got_z, exp_z, 1e-6f),
          "4x3->3x4 transpose preserves affine point transformation");

    rdna4rt_packed_float4x3_t bad_nan = col;
    bad_nan.columns[2][1] = NAN;
    CHECK(rdna4rt_transpose_4x3_to_3x4(&bad_nan, &row) == RDNA4RT_ERR_NAN_TRANSFORM, "NaN transform rejected");
    bad_nan.columns[2][1] = INFINITY;
    CHECK(rdna4rt_transpose_4x3_to_3x4(&bad_nan, &row) == RDNA4RT_ERR_NAN_TRANSFORM, "Inf transform rejected");

    /* Default (64B) instance conversion with custom 80B stride and 16B offset. */
    const uint64_t blas_addrs[2] = { UINT64_C(0x100000000), UINT64_C(0x200000000) };
    uint8_t raw_buf[16 + 2 * 80];
    memset(raw_buf, 0xCC, sizeof(raw_buf));
    rdna4rt_mtl_instance_default_t d0 = {
        .transform = col,
        .options = RDNA4RT_INST_OPT_DISABLE_CULLING | RDNA4RT_INST_OPT_OPAQUE,
        .mask = 0x123456A5u, /* low 8 bits -> 0xA5 */
        .intersection_function_offset = 0x12345u,
        .accel_struct_index = 1u
    };
    rdna4rt_mtl_instance_default_t d1 = {
        .transform = col,
        .options = RDNA4RT_INST_OPT_CCW | RDNA4RT_INST_OPT_NON_OPAQUE,
        .mask = 0x0Fu,
        .intersection_function_offset = 7u,
        .accel_struct_index = 0u
    };
    memcpy(raw_buf + 16, &d0, sizeof(d0));
    memcpy(raw_buf + 16 + 80, &d1, sizeof(d1));

    rdna4rt_vk_instance_t vk_out[2];
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_DEFAULT, raw_buf, sizeof(raw_buf),
                                    16u, 80u, 2u, blas_addrs, 2u, vk_out) == RDNA4RT_OK,
          "default instance conversion with padded stride");
    CHECK((vk_out[0].custom_index_and_mask & 0x00FFFFFFu) == 0u, "default customIndex == 0");
    CHECK((vk_out[0].custom_index_and_mask >> 24) == 0xA5u, "default mask low 8 bits == 0xA5");
    CHECK((vk_out[0].sbt_offset_and_flags & 0x00FFFFFFu) == 0x12345u, "default sbtOffset");
    CHECK((vk_out[0].sbt_offset_and_flags >> 24) == (RDNA4RT_VK_INST_DISABLE_CULLING | RDNA4RT_VK_INST_FORCE_OPAQUE),
          "default vk flags");
    CHECK(vk_out[0].accel_struct_reference == blas_addrs[1], "default blas_ref[1]");
    CHECK((vk_out[1].sbt_offset_and_flags >> 24) == (RDNA4RT_VK_INST_FLIP_FACING | RDNA4RT_VK_INST_FORCE_NO_OPAQUE),
          "second instance vk flags");
    CHECK(vk_out[1].accel_struct_reference == blas_addrs[0], "second instance blas_ref[0]");

    /* UserID (68B) instance conversion. */
    rdna4rt_mtl_instance_userid_t u0 = {
        .transform = col,
        .options = RDNA4RT_INST_OPT_NONE,
        .mask = 0xFFu,
        .intersection_function_offset = 0x00FFFFFFu,
        .accel_struct_index = 0u,
        .user_id = 0x00ABCDEFu
    };
    rdna4rt_vk_instance_t vk_u;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&u0, sizeof(u0),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_OK,
          "userID instance conversion");
    CHECK((vk_u.custom_index_and_mask & 0x00FFFFFFu) == 0x00ABCDEFu &&
          (vk_u.custom_index_and_mask >> 24) == 0xFFu &&
          (vk_u.sbt_offset_and_flags & 0x00FFFFFFu) == 0x00FFFFFFu,
          "userID 24-bit customIndex + 8-bit mask + 24-bit sbtOffset exact packing");

    /* Negative instance conversions. */
    rdna4rt_mtl_instance_userid_t bad_u = u0;
    bad_u.user_id = 0x01000000u; /* > 24-bit */
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "userID > 24 bits rejected");
    bad_u = u0; bad_u.intersection_function_offset = 0x01000000u;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "sbt offset > 24 bits rejected");
    bad_u = u0; bad_u.options = RDNA4RT_INST_OPT_OPAQUE | RDNA4RT_INST_OPT_NON_OPAQUE;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_ERR_CONFLICT,
          "opaque + non-opaque conflict rejected");
    bad_u = u0; bad_u.options = 1u << 4;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_ERR_INVALID_ARG,
          "unknown instance option bit rejected");
    bad_u = u0; bad_u.accel_struct_index = 2u;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, blas_addrs, 2u, &vk_u) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "out-of-range accel_struct_index rejected");
    const uint64_t zero_blas[1] = { 0u };
    bad_u = u0; bad_u.accel_struct_index = 0u;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, zero_blas, 1u, &vk_u) == RDNA4RT_ERR_STATE,
          "active instance referencing unbuilt (0) BLAS address rejected");
    bad_u.mask = 0u;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&bad_u, sizeof(bad_u),
                                    0u, 0u, 1u, zero_blas, 1u, &vk_u) == RDNA4RT_OK &&
          vk_u.accel_struct_reference == 0u,
          "inactive mask==0 instance allows 0 BLAS ref");

    /* TLAS descriptor validator. */
    rdna4rt_tlas_desc_t tl;
    memset(&tl, 0, sizeof(tl));
    tl.descriptor_type = RDNA4RT_INST_DESC_DEFAULT;
    tl.usage = RDNA4RT_USAGE_REFIT;
    tl.instance_count = 4u;
    tl.has_inst_buf = 1;
    tl.inst_buf_len = 256u;
    rdna4rt_tlas_plan_t tp;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 1, 0, &caps, &tp) == RDNA4RT_OK && tp.stride == 64u, "valid TLAS desc");
    tl.instance_count = 9u;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 0, 0, &caps, &tp) == RDNA4RT_ERR_LIMIT, "TLAS > rt_max_instances rejected");
    tl.instance_count = 4u;
    tl.descriptor_type = RDNA4RT_INST_DESC_INDIRECT;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 0, 0, &caps, &tp) == RDNA4RT_ERR_UNSUPPORTED, "Indirect TLAS rejected");
    tl.descriptor_type = RDNA4RT_INST_DESC_MOTION;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 0, 0, &caps, &tp) == RDNA4RT_ERR_MOTION_UNSUPPORTED, "Motion TLAS gated off when unsupported");
}

static void test_motion_blur_validation_and_interpolation(void) {
    rdna4dc_metal3_caps caps = make_rt_caps(256u, 16u, 16u, 1000u);

    rdna4rt_motion_params_t mp = {
        .is_motion = 1,
        .keyframe_count = 2u,
        .keyframe_buffer_count = 2u,
        .start_border_mode = RDNA4RT_MOTION_BORDER_CLAMP,
        .end_border_mode = RDNA4RT_MOTION_BORDER_VANISH,
        .start_time = 0.0f,
        .end_time = 1.0f
    };
    CHECK(rdna4rt_validate_motion_params(&mp, 0) == RDNA4RT_ERR_MOTION_UNSUPPORTED, "motion gated when unsupported");
    CHECK(rdna4rt_validate_motion_params(&mp, 1) == RDNA4RT_OK, "valid motion params accepted when allowed");

    rdna4rt_motion_params_t bad_mp = mp;
    bad_mp.keyframe_count = 1u;
    CHECK(rdna4rt_validate_motion_params(&bad_mp, 1) == RDNA4RT_ERR_INVALID_ARG, "keyframe_count < 2 rejected");
    bad_mp = mp; bad_mp.start_time = 2.0f; bad_mp.end_time = 1.0f;
    CHECK(rdna4rt_validate_motion_params(&bad_mp, 1) == RDNA4RT_ERR_INVALID_ARG, "start_time > end_time rejected");
    bad_mp = mp; bad_mp.end_border_mode = 2u;
    CHECK(rdna4rt_validate_motion_params(&bad_mp, 1) == RDNA4RT_ERR_INVALID_ARG, "invalid border mode rejected");
    bad_mp = mp; bad_mp.keyframe_buffer_count = 3u;
    CHECK(rdna4rt_validate_motion_params(&bad_mp, 1) == RDNA4RT_ERR_INVALID_ARG, "mismatched keyframe buffer count rejected");

    /* TLAS motion descriptor validation when enabled. */
    rdna4rt_tlas_desc_t mtl;
    memset(&mtl, 0, sizeof(mtl));
    mtl.descriptor_type = RDNA4RT_INST_DESC_MOTION;
    mtl.instance_count = 2u;
    mtl.has_inst_buf = 1;
    mtl.inst_buf_len = 2u * 44u;
    mtl.has_motion_transform_buf = 1;
    mtl.motion_transform_buf_len = 4u * 48u;
    mtl.motion_transform_count = 4u;
    rdna4rt_tlas_plan_t mtp;
    CHECK(rdna4rt_validate_tlas_desc(&mtl, 1, 1, &caps, &mtp) == RDNA4RT_OK && mtp.stride == 44u,
          "motion TLAS desc valid when enabled");
    mtl.motion_transform_buf_len = 4u * 48u - 1u;
    CHECK(rdna4rt_validate_tlas_desc(&mtl, 1, 1, &caps, &mtp) == RDNA4RT_ERR_BOUNDS,
          "undersized motion transform buffer rejected");

    /* Motion instance keyframe interpolation + Clamp/Vanish border modes. */
    rdna4rt_packed_float4x3_t keys[3] = {
        {{ {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0} }},
        {{ {1,0,0}, {0,1,0}, {0,0,1}, {10,0,0} }},
        {{ {1,0,0}, {0,1,0}, {0,0,1}, {30,20,0} }}
    };
    const uint64_t blas_addrs[1] = { UINT64_C(0x90000000) };
    rdna4rt_mtl_instance_motion_t mi = {
        .options = RDNA4RT_INST_OPT_OPAQUE,
        .mask = 0x7Fu,
        .intersection_function_offset = 3u,
        .accel_struct_index = 0u,
        .user_id = 42u,
        .motion_transforms_start_index = 0u,
        .motion_transforms_count = 3u,
        .motion_start_border_mode = RDNA4RT_MOTION_BORDER_CLAMP,
        .motion_end_border_mode = RDNA4RT_MOTION_BORDER_VANISH,
        .motion_start_time = 0.0f,
        .motion_end_time = 1.0f
    };
    rdna4rt_vk_instance_t out;
    /* t = 0.25 -> halfway between key[0] (tx=0) and key[1] (tx=10) => tx = 5 */
    CHECK(rdna4rt_convert_instance_motion(&mi, keys, 3u, 0.25f, blas_addrs, 1u, &out) == RDNA4RT_OK, "motion t=0.25");
    CHECK(feq(out.transform.matrix[0][3], 5.0f, 1e-5f) && feq(out.transform.matrix[1][3], 0.0f, 1e-5f),
          "motion t=0.25 exact lerp");
    CHECK((out.custom_index_and_mask & 0x00FFFFFFu) == 42u && (out.custom_index_and_mask >> 24) == 0x7Fu &&
          out.accel_struct_reference == blas_addrs[0], "motion t=0.25 userID/mask/ref");

    /* t = 0.75 -> halfway between key[1] (10,0,0) and key[2] (30,20,0) => (20,10,0) */
    CHECK(rdna4rt_convert_instance_motion(&mi, keys, 3u, 0.75f, blas_addrs, 1u, &out) == RDNA4RT_OK, "motion t=0.75");
    CHECK(feq(out.transform.matrix[0][3], 20.0f, 1e-5f) && feq(out.transform.matrix[1][3], 10.0f, 1e-5f),
          "motion t=0.75 exact lerp");

    /* t = -0.5 -> start_border_mode = CLAMP -> key[0] (0,0,0), still active! */
    CHECK(rdna4rt_convert_instance_motion(&mi, keys, 3u, -0.5f, blas_addrs, 1u, &out) == RDNA4RT_OK, "motion t=-0.5 clamp");
    CHECK(feq(out.transform.matrix[0][3], 0.0f, 1e-5f) && (out.custom_index_and_mask >> 24) == 0x7Fu &&
          out.accel_struct_reference == blas_addrs[0], "clamp keeps active instance");

    /* t = 1.5 -> end_border_mode = VANISH -> inactive (mask == 0, ref == 0)! */
    CHECK(rdna4rt_convert_instance_motion(&mi, keys, 3u, 1.5f, blas_addrs, 1u, &out) == RDNA4RT_OK, "motion t=1.5 vanish");
    CHECK((out.custom_index_and_mask >> 24) == 0u && out.accel_struct_reference == 0u,
          "vanish zeroes mask and AS reference");

    /* Motion start+count overflow / out of bounds. */
    rdna4rt_mtl_instance_motion_t bad_mi = mi;
    bad_mi.motion_transforms_start_index = UINT32_MAX;
    bad_mi.motion_transforms_count = 2u;
    CHECK(rdna4rt_convert_instance_motion(&bad_mi, keys, 3u, 0.5f, blas_addrs, 1u, &out) == RDNA4RT_ERR_BOUNDS,
          "motion start+count wraparound caught");
}

static void test_lifecycle_state_machine(void) {
    rdna4rt_accel_meta_t blas, compact_blas, copy_blas, small_blas, norefit_blas;
    rdna4rt_meta_init(&blas, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&compact_blas, 1024u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&copy_blas, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&small_blas, 512u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&norefit_blas, 4096u, RDNA4RT_KIND_BLAS);

    uint32_t sig0[1] = { rdna4rt_geom_sig(RDNA4RT_GEOM_TRIANGLES, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, RDNA4RT_VK_GEOM_OPAQUE) };
    uint32_t prim0[1] = { 12u };
    uint64_t topo0 = rdna4rt_topology_hash(sig0, prim0, 1u);
    uint32_t prim1[1] = { 13u };
    uint64_t topo1 = rdna4rt_topology_hash(sig0, prim1, 1u);

    /* Unbuilt rejections. */
    CHECK(rdna4rt_state_can_write_compacted_size(&blas) == RDNA4RT_ERR_STATE, "cannot query compacted size of unbuilt AS");
    CHECK(rdna4rt_state_can_copy(&blas, &copy_blas, 0) == RDNA4RT_ERR_STATE, "cannot copy unbuilt AS");
    CHECK(rdna4rt_state_can_compact(&blas, &compact_blas, 0, 1024u) == RDNA4RT_ERR_STATE, "cannot compact unbuilt AS");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_STATE,
          "cannot refit unbuilt AS");

    /* Build into undersized AS fails; build into sized AS succeeds. */
    CHECK(rdna4rt_state_apply_build(&small_blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_REFIT, 1u, topo0) == RDNA4RT_ERR_BOUNDS,
          "undersized AS build rejected");
    CHECK(rdna4rt_state_apply_build(&blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_REFIT, 1u, topo0) == RDNA4RT_OK,
          "valid refittable BLAS build");
    CHECK(blas.state == RDNA4RT_STATE_BUILT && blas.allow_refit == 1u && blas.allow_compaction == 1u,
          "built BLAS state flags");

    /* Compacted size query + copyAndCompact. */
    CHECK(rdna4rt_state_apply_write_compacted_size(&blas) == RDNA4RT_OK && blas.compacted_queried == 1u,
          "writeCompactedSize updates state");
    CHECK(rdna4rt_state_can_compact(&blas, &blas, 1, 1024u) == RDNA4RT_ERR_CONFLICT, "self-compact rejected");
    CHECK(rdna4rt_state_can_compact(&blas, &small_blas, 0, 1024u) == RDNA4RT_ERR_BOUNDS, "undersized compact dst rejected");
    CHECK(rdna4rt_state_apply_compact(&blas, &compact_blas, 0, 1024u) == RDNA4RT_OK, "valid copyAndCompact");
    CHECK(compact_blas.state == RDNA4RT_STATE_COMPACTED && compact_blas.allow_refit == 0u,
          "compacted AS disables refit");
    CHECK(rdna4rt_state_can_refit(&compact_blas, &compact_blas, RDNA4RT_KIND_BLAS, 1024u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_STATE,
          "cannot refit compacted AS");

    /* Copy (clone) built and compacted AS. */
    CHECK(rdna4rt_state_can_copy(&blas, &blas, 1) == RDNA4RT_ERR_CONFLICT, "self-copy rejected");
    CHECK(rdna4rt_state_can_copy(&blas, &small_blas, 0) == RDNA4RT_ERR_BOUNDS, "undersized copy dst rejected");
    CHECK(rdna4rt_state_apply_copy(&blas, &copy_blas, 0) == RDNA4RT_OK &&
          copy_blas.state == RDNA4RT_STATE_BUILT && copy_blas.allow_refit == 1u,
          "copy preserves refit capability");

    /* Refit in-place and out-of-place. */
    CHECK(rdna4rt_state_apply_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_OK,
          "in-place refit ok");
    CHECK(rdna4rt_state_apply_refit(&blas, &copy_blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_OK,
          "out-of-place refit ok");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo1, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_STATE,
          "refit rejects topology change");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_TLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_STATE,
          "refit rejects BLAS->TLAS kind mismatch");
    CHECK(rdna4rt_state_can_refit(&blas, &small_blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_BOUNDS,
          "refit rejects undersized destination");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, 1u << 5) == RDNA4RT_ERR_INVALID_ARG,
          "refit rejects unknown options");

    /* Non-refittable build rejects refit. */
    CHECK(rdna4rt_state_apply_build(&norefit_blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_NONE, 1u, topo0) == RDNA4RT_OK,
          "non-refit build ok");
    CHECK(rdna4rt_state_can_refit(&norefit_blas, &norefit_blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo0, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_ERR_STATE,
          "refit rejected without UsageRefit");
}

static void test_heap_batch_asan(void) {
    const uint32_t N = 512u;
    rdna4rt_mtl_instance_userid_t *src = (rdna4rt_mtl_instance_userid_t *)calloc(N, sizeof(*src));
    rdna4rt_vk_instance_t *dst = (rdna4rt_vk_instance_t *)calloc(N, sizeof(*dst));
    uint64_t *addrs = (uint64_t *)calloc(16u, sizeof(*addrs));
    CHECK(src && dst && addrs, "batch allocations succeeded");
    if (src && dst && addrs) {
        for (uint32_t i = 0; i < 16u; i++) addrs[i] = UINT64_C(0x10000000) + (uint64_t)i * 4096u;
        for (uint32_t i = 0; i < N; i++) {
            src[i].transform.columns[0][0] = 1.0f;
            src[i].transform.columns[1][1] = 1.0f;
            src[i].transform.columns[2][2] = 1.0f;
            src[i].transform.columns[3][0] = (float)i;
            src[i].options = (i & 1u) ? RDNA4RT_INST_OPT_OPAQUE : RDNA4RT_INST_OPT_DISABLE_CULLING;
            src[i].mask = 0xFFu;
            src[i].intersection_function_offset = i & 0x3Fu;
            src[i].accel_struct_index = i % 16u;
            src[i].user_id = i * 3u;
        }
        CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)src,
                                        (size_t)N * sizeof(*src), 0u, sizeof(*src), N,
                                        addrs, 16u, dst) == RDNA4RT_OK,
              "512-instance batch conversion");
        CHECK(feq(dst[N - 1u].transform.matrix[0][3], (float)(N - 1u), 1e-6f) &&
              (dst[N - 1u].custom_index_and_mask & 0x00FFFFFFu) == (N - 1u) * 3u &&
              dst[N - 1u].accel_struct_reference == addrs[(N - 1u) % 16u],
              "last batch instance verified");
    }
    free(src);
    free(dst);
    free(addrs);
}

int main(void) {
    test_caps_and_alignment();
    test_usage_sizes_and_scratch();
    test_blas_triangle_and_aabb();
    test_tlas_and_instance_conversion();
    test_motion_blur_validation_and_interpolation();
    test_lifecycle_state_machine();
    test_heap_batch_asan();
    printf("test-rt-accel: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
