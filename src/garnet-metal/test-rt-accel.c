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
#include "garnet_rt_spv.h"

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
    memset(&sz, 0, sizeof(sz));
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
    tl.inst_buf_len = 4u * 72u;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 1, 0, &caps, &tp) == RDNA4RT_OK && tp.stride == 72u,
          "Indirect descriptor type in TLAS desc supported (72B)");
    tl.descriptor_type = RDNA4RT_INST_DESC_INDIRECT_MOTION;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 0, 0, &caps, &tp) == RDNA4RT_ERR_MOTION_UNSUPPORTED,
          "Indirect motion TLAS rejected when motion unsupported");
    tl.descriptor_type = RDNA4RT_INST_DESC_MOTION;
    CHECK(rdna4rt_validate_tlas_desc(&tl, 0, 0, &caps, &tp) == RDNA4RT_ERR_MOTION_UNSUPPORTED, "Motion TLAS gated off when unsupported");

    /* Wave 3: Indirect instance descriptor conversion (72B) + Indirect TLAS descriptor + staging buckets + targeted barriers. */
    rdna4rt_mtl_instance_indirect_t ind0 = {
        .transform = {{ {1,0,0}, {0,1,0}, {0,0,1}, {7,8,9} }},
        .options = RDNA4RT_INST_OPT_OPAQUE,
        .mask = 0xAAu,
        .intersection_function_offset = 5u,
        .user_id = 0x123456u,
        .accel_struct_id = UINT64_C(0xFFFF8001001C0000)
    };
    rdna4rt_vk_instance_t vk_ind;
    CHECK(rdna4rt_convert_instances(RDNA4RT_INST_DESC_INDIRECT, (const uint8_t *)&ind0, sizeof(ind0),
                                    0u, 0u, 1u, NULL, 0u, &vk_ind) == RDNA4RT_OK,
          "indirect 72B instance conversion");
    CHECK(feq(vk_ind.transform.matrix[0][3], 7.0f, 1e-6f) &&
          feq(vk_ind.transform.matrix[1][3], 8.0f, 1e-6f) &&
          feq(vk_ind.transform.matrix[2][3], 9.0f, 1e-6f) &&
          (vk_ind.custom_index_and_mask & 0x00FFFFFFu) == 0x123456u &&
          (vk_ind.custom_index_and_mask >> 24) == 0xAAu &&
          (vk_ind.sbt_offset_and_flags & 0x00FFFFFFu) == 5u &&
          (vk_ind.sbt_offset_and_flags >> 24) == RDNA4RT_VK_INST_FORCE_OPAQUE &&
          vk_ind.accel_struct_reference == UINT64_C(0xFFFF8001001C0000),
          "indirect 72B instance exact BDA/userID/transform packing");

    rdna4rt_indirect_tlas_desc_t itl;
    memset(&itl, 0, sizeof(itl));
    itl.descriptor_type = RDNA4RT_INST_DESC_INDIRECT;
    itl.usage = RDNA4RT_USAGE_REFIT;
    itl.max_instance_count = 4u;
    itl.has_inst_buf = 1;
    itl.inst_buf_len = 4u * 72u;
    itl.has_count_buf = 1;
    itl.count_buf_len = 4u;
    CHECK(rdna4rt_validate_indirect_tlas_desc(&itl, 1, 0, &caps, &tp) == RDNA4RT_OK &&
          tp.stride == 72u && tp.instance_count == 4u,
          "valid MTLIndirectInstanceAccelerationStructureDescriptor");
    itl.count_buf_len = 2u;
    CHECK(rdna4rt_validate_indirect_tlas_desc(&itl, 1, 0, &caps, &tp) == RDNA4RT_ERR_BOUNDS,
          "indirect TLAS rejects undersized count buffer");
    itl.count_buf_len = 4u;
    itl.count_buf_offset = 2u;
    CHECK(rdna4rt_validate_indirect_tlas_desc(&itl, 1, 0, &caps, &tp) == RDNA4RT_ERR_ALIGNMENT,
          "indirect TLAS rejects misaligned count buffer offset");
    itl.count_buf_offset = 0u;
    itl.descriptor_type = RDNA4RT_INST_DESC_DEFAULT;
    CHECK(rdna4rt_validate_indirect_tlas_desc(&itl, 1, 0, &caps, &tp) == RDNA4RT_ERR_INVALID_ARG,
          "indirect TLAS rejects Default descriptor type");
    itl.descriptor_type = RDNA4RT_INST_DESC_INDIRECT_MOTION;
    CHECK(rdna4rt_validate_indirect_tlas_desc(&itl, 1, 0, &caps, &tp) == RDNA4RT_ERR_MOTION_UNSUPPORTED,
          "indirect TLAS rejects IndirectMotion when motion unsupported");

    uint64_t bkt = 0;
    CHECK(rdna4rt_staging_bucket_size(64u, &bkt) == 1 && bkt == 4096u, "staging bucket min 4 KiB");
    CHECK(rdna4rt_staging_bucket_size(4097u, &bkt) == 1 && bkt == 8192u, "staging bucket next pow2 8 KiB");
    CHECK(rdna4rt_staging_bucket_size(RDNA4RT_STAGING_POOL_MAX_BUCKET, &bkt) == 1 &&
          bkt == RDNA4RT_STAGING_POOL_MAX_BUCKET, "staging bucket max 64 MiB");
    CHECK(rdna4rt_staging_bucket_size(RDNA4RT_STAGING_POOL_MAX_BUCKET + 1u, &bkt) == 0,
          "staging > 64 MiB bypasses pool");

    /* Targeted barrier masks: computeQ must never contain graphics stage bits. */
    const uint32_t gfx_stages = RDNA4RT_VK_STAGE_VERTEX_SHADER | RDNA4RT_VK_STAGE_FRAGMENT_SHADER | RDNA4RT_VK_STAGE_ALL_GRAPHICS;
    for (int k = 0; k <= (int)RDNA4RT_BARRIER_TRANSFER_TO_HOST; k++) {
        rdna4rt_barrier_spec_t cb_spec = rdna4rt_barrier_spec((rdna4rt_barrier_kind_t)k, 1);
        CHECK((cb_spec.src_stage & gfx_stages) == 0u && (cb_spec.dst_stage & gfx_stages) == 0u,
              "computeQ RT barrier contains zero graphics stage bits");
    }
    rdna4rt_barrier_spec_t post_gfx = rdna4rt_barrier_spec(RDNA4RT_BARRIER_POST_BUILD, 0);
    CHECK((post_gfx.dst_stage & RDNA4RT_VK_STAGE_VERTEX_SHADER) != 0u &&
          (post_gfx.dst_stage & RDNA4RT_VK_STAGE_FRAGMENT_SHADER) != 0u &&
          (post_gfx.dst_stage & RDNA4RT_VK_STAGE_COMPUTE_SHADER) != 0u,
          "gfxQ post-build RT barrier covers vertex, fragment, and compute stages");
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

static void test_wave4_prim_data_validation_and_layout(void) {
    /* 1. Absent buffer -> clean no-op plan. */
    rdna4rt_prim_data_geom_desc_t d0 = { .has_buf = 0, .prim_count = 10u };
    rdna4rt_prim_data_geom_plan_t p0;
    CHECK(rdna4rt_validate_prim_data_geom(&d0, 1, &p0) == RDNA4RT_OK &&
          p0.present == 0 && p0.copy_bytes == 0u,
          "nil primitiveDataBuffer produces empty plan");

    /* 2. Default stride (0 -> element_size), tight bounds. */
    rdna4rt_prim_data_geom_desc_t d1 = {
        .has_buf = 1, .buf_len = 64u, .buf_offset = 0u,
        .element_size = 16u, .stride = 0u, .prim_count = 4u
    };
    rdna4rt_prim_data_geom_plan_t p1;
    CHECK(rdna4rt_validate_prim_data_geom(&d1, 1, &p1) == RDNA4RT_OK &&
          p1.present == 1 && p1.stride == 16u &&
          p1.copy_bytes == 64u && p1.prim_count == 4u,
          "default stride==0 uses element_size");

    /* 3. Padded stride > element_size with non-zero 4B offset: exact boundary vs 1B short. */
    rdna4rt_prim_data_geom_desc_t d2 = {
        .has_buf = 1, .buf_len = 68u, .buf_offset = 8u,
        .element_size = 12u, .stride = 24u, .prim_count = 3u
    };
    rdna4rt_prim_data_geom_plan_t p2;
    CHECK(rdna4rt_validate_prim_data_geom(&d2, 1, &p2) == RDNA4RT_OK &&
          p2.stride == 24u && p2.copy_bytes == 60u && p2.src_offset == 8u,
          "padded stride exact boundary accepted");
    d2.buf_len = 67u;
    CHECK(rdna4rt_validate_prim_data_geom(&d2, 1, &p2) == RDNA4RT_ERR_BOUNDS,
          "padded stride 1 byte short rejected");

    /* 4. Zero primitives with valid buffer metadata -> copy_bytes == 0. */
    rdna4rt_prim_data_geom_desc_t dzero = {
        .has_buf = 1, .buf_len = 4u, .buf_offset = 4u,
        .element_size = 16u, .stride = 16u, .prim_count = 0u
    };
    rdna4rt_prim_data_geom_plan_t pzero;
    CHECK(rdna4rt_validate_prim_data_geom(&dzero, 1, &pzero) == RDNA4RT_OK &&
          pzero.present == 1 && pzero.copy_bytes == 0u,
          "zero primitives with valid primData metadata ok");

    /* 5. Negative alignment / size / stride / limit / overflow checks. */
    rdna4rt_prim_data_geom_desc_t bad = d1;
    bad.element_size = 0u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_INVALID_ARG, "primData elementSize==0 rejected");
    bad = d1; bad.element_size = 6u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_ALIGNMENT, "primData unaligned elementSize rejected");
    bad = d1; bad.buf_offset = 2u; bad.buf_len = 128u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_ALIGNMENT, "primData unaligned offset rejected");
    bad = d1; bad.stride = 18u; bad.buf_len = 128u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_ALIGNMENT, "primData unaligned stride rejected");
    bad = d1; bad.stride = 12u; bad.buf_len = 128u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_INVALID_ARG, "primData stride < elementSize rejected");
    bad = d1; bad.buf_offset = 68u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_BOUNDS, "primData offset > buf_len rejected");
    bad = d1; bad.prim_count = (uint64_t)UINT32_MAX + 1u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_LIMIT, "primData prim_count > UINT32_MAX rejected");
    bad = d1; bad.prim_count = UINT32_MAX; bad.stride = (uint64_t)UINT32_MAX - 3u;
    CHECK(rdna4rt_validate_prim_data_geom(&bad, 1, &p1) == RDNA4RT_ERR_BOUNDS, "primData huge span rejected");

    /* 6. Multi-geometry BLAS layout, header build, and rebase for copy/compact. */
    rdna4rt_prim_data_geom_plan_t plans[3];
    memset(plans, 0, sizeof(plans));
    plans[0] = (rdna4rt_prim_data_geom_plan_t){ .present = 1, .stride = 8u, .element_size = 8u, .prim_count = 3u, .src_offset = 0u, .copy_bytes = 20u };
    plans[1] = (rdna4rt_prim_data_geom_plan_t){ .present = 0 };
    plans[2] = (rdna4rt_prim_data_geom_plan_t){ .present = 1, .stride = 12u, .element_size = 12u, .prim_count = 1u, .src_offset = 16u, .copy_bytes = 12u };

    uint64_t offs[3] = { 0, 0, 0 };
    uint64_t hdrBytes = 0, totBytes = 0;
    int anyPrim = 0;
    CHECK(rdna4rt_blas_prim_layout_compute(plans, 3u, offs, &hdrBytes, &totBytes, &anyPrim) == RDNA4RT_OK &&
          anyPrim == 1 && hdrBytes == 48u && offs[0] == 48u && offs[1] == 0u && offs[2] == 80u && totBytes == 96u,
          "multi-geom BLAS prim layout computes 16B-aligned slices");

    rdna4rt_geom_prim_entry_t srcHdr[3], dstHdr[3];
    const uint64_t srcBase = UINT64_C(0x10000000);
    const uint64_t dstBase = UINT64_C(0x20000000);
    CHECK(rdna4rt_blas_prim_build_headers(srcBase, plans, offs, 3u, srcHdr) == RDNA4RT_OK &&
          srcHdr[0].data_bda == srcBase + 48u && srcHdr[0].stride == 8u && srcHdr[0].prim_count == 3u &&
          srcHdr[1].data_bda == 0u && srcHdr[1].stride == 0u && srcHdr[1].prim_count == 0u &&
          srcHdr[2].data_bda == srcBase + 80u && srcHdr[2].stride == 12u && srcHdr[2].prim_count == 1u,
          "BLAS prim headers store absolute BDAs and zero absent geometries");

    CHECK(rdna4rt_blas_prim_rebase_headers(srcHdr, 3u, srcBase, dstBase, totBytes, dstHdr) == RDNA4RT_OK &&
          dstHdr[0].data_bda == dstBase + 48u && dstHdr[0].stride == 8u && dstHdr[0].prim_count == 3u &&
          dstHdr[1].data_bda == 0u && dstHdr[1].stride == 0u && dstHdr[1].prim_count == 0u &&
          dstHdr[2].data_bda == dstBase + 80u && dstHdr[2].stride == 12u && dstHdr[2].prim_count == 1u,
          "BLAS prim header rebase shifts BDAs cleanly to destination base");

    /* Corrupted source headers rejected by rebase. */
    rdna4rt_geom_prim_entry_t badHdr[3];
    memcpy(badHdr, srcHdr, sizeof(badHdr));
    badHdr[0].data_bda = srcBase - 16u;
    CHECK(rdna4rt_blas_prim_rebase_headers(badHdr, 3u, srcBase, dstBase, totBytes, dstHdr) == RDNA4RT_ERR_BOUNDS,
          "rebase rejects BDA below srcBase");
    memcpy(badHdr, srcHdr, sizeof(badHdr));
    badHdr[0].data_bda = srcBase + 16u; /* inside header table */
    CHECK(rdna4rt_blas_prim_rebase_headers(badHdr, 3u, srcBase, dstBase, totBytes, dstHdr) == RDNA4RT_ERR_BOUNDS,
          "rebase rejects BDA pointing inside header");
    memcpy(badHdr, srcHdr, sizeof(badHdr));
    badHdr[2].data_bda = srcBase + totBytes;
    CHECK(rdna4rt_blas_prim_rebase_headers(badHdr, 3u, srcBase, dstBase, totBytes, dstHdr) == RDNA4RT_ERR_BOUNDS,
          "rebase rejects BDA at/past totalBytes");
    CHECK(rdna4rt_blas_prim_rebase_headers(srcHdr, 3u, srcBase, 0u, totBytes, dstHdr) == RDNA4RT_ERR_INVALID_ARG,
          "rebase rejects zero dstBase");

    /* All-absent BLAS -> anyPrim == 0. */
    rdna4rt_prim_data_geom_plan_t emptyPlans[2];
    memset(emptyPlans, 0, sizeof(emptyPlans));
    CHECK(rdna4rt_blas_prim_layout_compute(emptyPlans, 2u, offs, &hdrBytes, &totBytes, &anyPrim) == RDNA4RT_OK &&
          anyPrim == 0 && totBytes == 0u,
          "all-absent BLAS produces anyPrim == 0");
}

static void test_wave4_prim_refit_and_barriers(void) {
    CHECK(!rdna4rt_refit_updates_vertices(0u) && !rdna4rt_refit_updates_prim_data(0u) && !rdna4rt_refit_is_prim_data_only(0u),
          "refit option 0 updates nothing");
    CHECK(rdna4rt_refit_updates_vertices(RDNA4RT_REFIT_OPT_VERTEX_DATA) &&
          !rdna4rt_refit_updates_prim_data(RDNA4RT_REFIT_OPT_VERTEX_DATA) &&
          !rdna4rt_refit_is_prim_data_only(RDNA4RT_REFIT_OPT_VERTEX_DATA),
          "VertexData-only option predicates");
    CHECK(!rdna4rt_refit_updates_vertices(RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) &&
          rdna4rt_refit_updates_prim_data(RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) &&
          rdna4rt_refit_is_prim_data_only(RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA),
          "PerPrimitiveData-only option predicates");
    const uint64_t both = RDNA4RT_REFIT_OPT_VERTEX_DATA | RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA;
    CHECK(rdna4rt_refit_updates_vertices(both) && rdna4rt_refit_updates_prim_data(both) &&
          !rdna4rt_refit_is_prim_data_only(both),
          "VertexData|PerPrimitiveData predicates");

    rdna4rt_accel_meta_t blas, dst, smallDst;
    rdna4rt_meta_init(&blas, 4096u, RDNA4RT_KIND_BLAS);
    rdna4rt_meta_init(&dst, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&smallDst, 1024u, RDNA4RT_KIND_UNKNOWN);
    uint32_t sig[1] = { rdna4rt_geom_sig(RDNA4RT_GEOM_TRIANGLES, RDNA4RT_INDEX_UINT32, RDNA4RT_VFMT_FLOAT3, RDNA4RT_VK_GEOM_OPAQUE) };
    uint32_t pc[1] = { 8u };
    uint64_t topo = rdna4rt_topology_hash(sig, pc, 1u);
    CHECK(rdna4rt_state_apply_build(&blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_REFIT, 1u, topo) == RDNA4RT_OK,
          "build refittable BLAS for Wave 4 prim-refit test");

    /* options == 0 rejected. */
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 2048u, 1u, topo, 0u) == RDNA4RT_ERR_INVALID_ARG,
          "refit options == 0 rejected");

    /* PerPrimitiveData-only in-place (required_as_size=0, topology_hash=0 allowed) preserves built_size & topo. */
    CHECK(rdna4rt_state_apply_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 0u, 1u, 0u, RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) == RDNA4RT_OK &&
          blas.built_size_bytes == 2048u && blas.topology_hash == topo && blas.allow_refit == 1u,
          "PerPrimitiveData-only in-place refit preserves built_size and topology_hash");

    /* PerPrimitiveData-only out-of-place succeeds into sized dst and fails into undersized dst. */
    CHECK(rdna4rt_state_apply_refit(&blas, &dst, RDNA4RT_KIND_BLAS, 0u, 1u, 0u, RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) == RDNA4RT_OK &&
          dst.built_size_bytes == 2048u && dst.topology_hash == topo && dst.kind == RDNA4RT_KIND_BLAS,
          "PerPrimitiveData-only out-of-place refit populates dst state");
    CHECK(rdna4rt_state_can_refit(&blas, &smallDst, RDNA4RT_KIND_BLAS, 0u, 1u, 0u, RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) == RDNA4RT_ERR_BOUNDS,
          "PerPrimitiveData-only out-of-place refit rejects undersized dst");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_BLAS, 0u, 2u, 0u, RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) == RDNA4RT_ERR_STATE,
          "PerPrimitiveData-only refit rejects geom_count mismatch");
    CHECK(rdna4rt_state_can_refit(&blas, &blas, RDNA4RT_KIND_TLAS, 0u, 1u, 0u, RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA) == RDNA4RT_ERR_STATE,
          "PerPrimitiveData-only refit rejects kind mismatch");

    /* Both (0x3) requires matching topology hash and sized dst. */
    CHECK(rdna4rt_state_apply_refit(&blas, &dst, RDNA4RT_KIND_BLAS, 2048u, 1u, topo, both) == RDNA4RT_OK,
          "VertexData|PerPrimitiveData refit succeeds");
    CHECK(rdna4rt_state_can_refit(&blas, &dst, RDNA4RT_KIND_BLAS, 2048u, 1u, topo ^ 1u, both) == RDNA4RT_ERR_STATE,
          "VertexData|PerPrimitiveData refit rejects topology mismatch");

    /* Barrier specs cover TRANSFER for prim-data copies/updates and keep computeQ free of graphics stages. */
    rdna4rt_barrier_spec_t preG = rdna4rt_barrier_spec(RDNA4RT_BARRIER_PRE_BUILD, 0);
    rdna4rt_barrier_spec_t preC = rdna4rt_barrier_spec(RDNA4RT_BARRIER_PRE_BUILD, 1);
    rdna4rt_barrier_spec_t c2bC = rdna4rt_barrier_spec(RDNA4RT_BARRIER_COMP_TO_BUILD, 1);
    rdna4rt_barrier_spec_t postC = rdna4rt_barrier_spec(RDNA4RT_BARRIER_POST_BUILD, 1);
    CHECK((preG.dst_stage & RDNA4RT_VK_STAGE_TRANSFER) != 0u &&
          (preC.dst_stage & RDNA4RT_VK_STAGE_TRANSFER) != 0u &&
          (preC.dst_access & RDNA4RT_VK_ACCESS_TRANSFER_READ) != 0u &&
          (preC.dst_access & RDNA4RT_VK_ACCESS_TRANSFER_WRITE) != 0u,
          "PRE_BUILD barrier covers TRANSFER stage and accesses");
    CHECK((c2bC.dst_stage & RDNA4RT_VK_STAGE_TRANSFER) != 0u &&
          (c2bC.dst_access & RDNA4RT_VK_ACCESS_TRANSFER_READ) != 0u,
          "COMP_TO_BUILD barrier covers TLAS snapshot vkCmdCopyBuffer read");
    CHECK((postC.src_stage & RDNA4RT_VK_STAGE_TRANSFER) != 0u &&
          (postC.src_stage & RDNA4RT_VK_STAGE_COMPUTE_SHADER) != 0u &&
          (postC.src_access & RDNA4RT_VK_ACCESS_TRANSFER_WRITE) != 0u &&
          (postC.src_access & RDNA4RT_VK_ACCESS_SHADER_WRITE) != 0u,
          "POST_BUILD barrier flushes prim-data TRANSFER writes and curve compute writes to shaders");
    CHECK((preC.src_stage & RDNA4RT_VK_STAGE_ALL_GRAPHICS) == 0u &&
          (postC.dst_stage & RDNA4RT_VK_STAGE_ALL_GRAPHICS) == 0u,
          "computeQ barriers emit zero ALL_GRAPHICS stages");
}

static void test_wave4_heap_as_sizing_and_validation(void) {
    rdna4rt_heap_size_align_t sa;
    /* Align bumped to >= 256, size aligned up to max(256, gran). */
    CHECK(rdna4rt_heap_as_size_align(100u, 128u, 16u, 1u, &sa) == RDNA4RT_OK &&
          sa.align == 256u && sa.size == 256u,
          "heap AS size/align floors alignment and size to 256B");
    CHECK(rdna4rt_heap_as_size_align(500u, 1024u, 4096u, 65536u, &sa) == RDNA4RT_OK &&
          sa.align == 65536u && sa.size == 65536u,
          "heap AS size/align honors larger bufferImageGranularity");
    CHECK(rdna4rt_heap_as_size_align(4096u, 4096u, 8192u, 256u, &sa) == RDNA4RT_OK &&
          sa.align == 8192u && sa.size == 4096u,
          "heap AS size/align honors larger vk_req_align");
    CHECK(rdna4rt_heap_as_size_align(0u, 256u, 256u, 256u, &sa) == RDNA4RT_ERR_INVALID_ARG,
          "heap AS size/align rejects requested_size == 0");
    CHECK(rdna4rt_heap_as_size_align(256u, 256u, 300u, 256u, &sa) == RDNA4RT_ERR_ALIGNMENT,
          "heap AS size/align rejects non-power-of-two alignment");
    CHECK(rdna4rt_heap_as_size_align(UINT64_MAX, 256u, 256u, 256u, &sa) == RDNA4RT_ERR_OVERFLOW,
          "heap AS size/align catches overflow");

    /* Allocation validation: Private Automatic & Private Placement. */
    uint64_t aligned = 0u;
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 0u, 65536u, 500u, 0, 0u, 256u, &aligned) == RDNA4RT_OK &&
          aligned == 512u,
          "Private Automatic heap AS alloc accepted");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 300u, 1, 256u, 256u, &aligned) == RDNA4RT_OK &&
          aligned == 512u,
          "Private Placement heap AS alloc accepted at 256B offset");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 512u, 1, 65024u, 256u, &aligned) == RDNA4RT_OK &&
          aligned == 512u,
          "Private Placement heap AS exact end boundary accepted");

    /* Rejections: Shared/Managed, Placement-vs-Automatic mismatch, alignment, bounds, zero size. */
    CHECK(rdna4rt_heap_as_validate_alloc(0u, 0u, 65536u, 512u, 0, 0u, 256u, &aligned) == RDNA4RT_ERR_UNSUPPORTED,
          "Shared heap rejected for AS");
    CHECK(rdna4rt_heap_as_validate_alloc(1u, 1u, 65536u, 512u, 1, 0u, 256u, &aligned) == RDNA4RT_ERR_UNSUPPORTED,
          "Managed heap rejected for AS");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 0u, 65536u, 512u, 1, 0u, 256u, &aligned) == RDNA4RT_ERR_CONFLICT,
          "Automatic heap rejects placement offset:");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 512u, 0, 0u, 256u, &aligned) == RDNA4RT_ERR_CONFLICT,
          "Placement heap rejects non-offset alloc");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 512u, 1, 128u, 256u, &aligned) == RDNA4RT_ERR_ALIGNMENT,
          "Placement heap rejects misaligned 128B offset");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 512u, 1, 65280u, 256u, &aligned) == RDNA4RT_ERR_BOUNDS,
          "Placement heap rejects OOB offset+alignedSize");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 1u, 65536u, 512u, 1, 65536u, 256u, &aligned) == RDNA4RT_ERR_BOUNDS,
          "Placement heap rejects offset == heapSize");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 0u, 65536u, 0u, 0, 0u, 256u, &aligned) == RDNA4RT_ERR_INVALID_ARG,
          "heap AS rejects size == 0");
    CHECK(rdna4rt_heap_as_validate_alloc(2u, 0u, 1024u, 2048u, 0, 0u, 256u, &aligned) == RDNA4RT_ERR_BOUNDS,
          "Automatic heap rejects size > heapSize");
}

static void test_wave4_prim_and_query_pools(void) {
    /* 1. Device-local GarnetASPrimData buffer pool: bucket fit, 2x guard, slab/byte caps, overflow. */
    rdna4rt_buf_pool_t bp;
    memset(&bp, 0, sizeof(bp));
    rdna4rt_buf_pool_entry_t out;
    CHECK(rdna4rt_buf_pool_pop(&bp, 4096u, &out) == 0 && bp.misses == 1u, "empty prim pool pop misses");

    rdna4rt_buf_pool_entry_t e4k = { .buf_handle = 1u, .mem_handle = 101u, .bda = 0x10000u, .cap = 4096u, .alloc_size = 4096u };
    rdna4rt_buf_pool_entry_t e8k = { .buf_handle = 2u, .mem_handle = 102u, .bda = 0x20000u, .cap = 8192u, .alloc_size = 8192u };
    rdna4rt_buf_pool_entry_t e16m = { .buf_handle = 3u, .mem_handle = 103u, .bda = 0x30000u, .cap = 16u << 20, .alloc_size = 16u << 20 };
    CHECK(rdna4rt_buf_pool_push(&bp, &e16m) == 1 && rdna4rt_buf_pool_push(&bp, &e8k) == 1 &&
          rdna4rt_buf_pool_push(&bp, &e4k) == 1 && bp.count == 3u,
          "push 16MiB, 8KiB, 4KiB slabs to prim pool");

    /* Best-fit picks 4KiB first, then 8KiB (<= 2x), then refuses to steal 16MiB for a 4KiB request. */
    CHECK(rdna4rt_buf_pool_pop(&bp, 4096u, &out) == 1 && out.buf_handle == 1u && bp.hits == 1u,
          "4KiB pop picks exact 4KiB slab");
    CHECK(rdna4rt_buf_pool_pop(&bp, 4096u, &out) == 1 && out.buf_handle == 2u && bp.hits == 2u,
          "second 4KiB pop picks 8KiB slab (<=2x)");
    CHECK(rdna4rt_buf_pool_pop(&bp, 4096u, &out) == 0 && bp.count == 1u,
          "third 4KiB pop refuses to pin oversized 16MiB slab");
    CHECK(rdna4rt_buf_pool_pop(&bp, 16u << 20, &out) == 1 && out.buf_handle == 3u &&
          bp.count == 0u && bp.total_bytes == 0u,
          "16MiB pop reclaims 16MiB slab cleanly");

    /* Slab cap (32) enforcement. */
    int all32 = 1;
    for (uint32_t i = 0u; i < RDNA4RT_PRIM_POOL_MAX_SLABS; i++) {
        rdna4rt_buf_pool_entry_t e = { .buf_handle = i + 10u, .mem_handle = i + 100u, .bda = 0x40000u + (uint64_t)i * 4096u, .cap = 4096u, .alloc_size = 4096u };
        if (!rdna4rt_buf_pool_push(&bp, &e)) all32 = 0;
    }
    CHECK(all32 && bp.count == RDNA4RT_PRIM_POOL_MAX_SLABS, "prim pool accepts 32 slabs");
    CHECK(rdna4rt_buf_pool_push(&bp, &e4k) == 0, "prim pool rejects 33rd slab");

    /* Byte cap (64 MiB) + overflow guard. */
    rdna4rt_buf_pool_reset(&bp);
    rdna4rt_buf_pool_entry_t e32m = { .buf_handle = 50u, .mem_handle = 150u, .bda = 0x80000u, .cap = 32u << 20, .alloc_size = 32u << 20 };
    CHECK(rdna4rt_buf_pool_push(&bp, &e32m) == 1 && rdna4rt_buf_pool_push(&bp, &e32m) == 1 &&
          bp.total_bytes == RDNA4RT_PRIM_POOL_MAX_BUCKET,
          "prim pool fills to exact 64 MiB cap");
    CHECK(rdna4rt_buf_pool_push(&bp, &e4k) == 0, "prim pool rejects push exceeding 64 MiB cap");
    rdna4rt_buf_pool_entry_t eHuge = { .buf_handle = 99u, .mem_handle = 199u, .bda = 0x90000u, .cap = 4096u, .alloc_size = UINT64_MAX };
    CHECK(rdna4rt_buf_pool_push(&bp, &eHuge) == 0, "prim pool rejects overflowing alloc_size");
    rdna4rt_buf_pool_entry_t eZeroBda = e4k; eZeroBda.bda = 0u;
    CHECK(rdna4rt_buf_pool_push(&bp, &eZeroBda) == 0, "prim pool rejects zero BDA entry");
    CHECK(rdna4rt_buf_pool_pop(&bp, 32u << 20, &out) == 1 && rdna4rt_buf_pool_push(&bp, &e4k) == 1,
          "prim pool accepts push after popping frees budget");

    /* 2. Compacted-size VkQueryPool free list (cap 8). */
    rdna4rt_qp_pool_t qp;
    memset(&qp, 0, sizeof(qp));
    uint64_t qh = 0u;
    CHECK(rdna4rt_qp_pool_pop(&qp, &qh) == 0 && qp.misses == 1u, "empty query pool misses");
    CHECK(rdna4rt_qp_pool_push(&qp, 0u) == 0, "query pool rejects null handle 0");
    int all8 = 1;
    for (uint32_t i = 0u; i < RDNA4RT_QUERY_POOL_MAX_FREE; i++) {
        if (!rdna4rt_qp_pool_push(&qp, UINT64_C(0x7000) + i)) all8 = 0;
    }
    CHECK(all8 && qp.count == RDNA4RT_QUERY_POOL_MAX_FREE, "query pool accepts 8 free pools");
    CHECK(rdna4rt_qp_pool_push(&qp, UINT64_C(0x7099)) == 0, "query pool rejects 9th pool");
    int popped8 = 1;
    for (uint32_t i = 0u; i < RDNA4RT_QUERY_POOL_MAX_FREE; i++) {
        if (!rdna4rt_qp_pool_pop(&qp, &qh) || qh != UINT64_C(0x7000) + (RDNA4RT_QUERY_POOL_MAX_FREE - 1u - i))
            popped8 = 0;
    }
    CHECK(popped8 && qp.count == 0u && qp.hits == RDNA4RT_QUERY_POOL_MAX_FREE &&
          rdna4rt_qp_pool_pop(&qp, &qh) == 0,
          "query pool LIFO pop drains all 8 handles cleanly");
}

static void test_wave5_geom_ift_offset_and_spv(void) {
    /* 1. BLAS geometry intersectionFunctionTableOffset extraction & consecutive validation. */
    uint32_t base = 99u;
    int non_consec = 1;
    CHECK(rdna4rt_compute_geom_ift_base(NULL, 0u, &base, &non_consec) == RDNA4RT_OK &&
          base == 0u && non_consec == 0,
          "empty BLAS yields geom_ift_base=0, consecutive");

    const uint64_t apple_sphere[1] = { 2u };
    CHECK(rdna4rt_compute_geom_ift_base(apple_sphere, 1u, &base, &non_consec) == RDNA4RT_OK &&
          base == 2u && non_consec == 0,
          "Apple Sample 1 sphere BLAS (1 geom, iftOffset=2) -> base=2, consecutive");

    const uint64_t consec[3] = { 4u, 5u, 6u };
    CHECK(rdna4rt_compute_geom_ift_base(consec, 3u, &base, &non_consec) == RDNA4RT_OK &&
          base == 4u && non_consec == 0,
          "consecutive multi-geom offsets [4,5,6] -> base=4, non_consecutive=0");

    const uint64_t nonconsec_gap[2] = { 2u, 5u };
    CHECK(rdna4rt_compute_geom_ift_base(nonconsec_gap, 2u, &base, &non_consec) == RDNA4RT_OK &&
          base == 2u && non_consec == 1,
          "non-consecutive multi-geom offsets [2,5] -> base=2, non_consecutive=1");

    const uint64_t nonconsec_dup[2] = { 0u, 0u };
    CHECK(rdna4rt_compute_geom_ift_base(nonconsec_dup, 2u, &base, &non_consec) == RDNA4RT_OK &&
          base == 0u && non_consec == 1,
          "duplicate multi-geom offsets [0,0] flagged non_consecutive=1");

    const uint64_t oob_geom[1] = { (uint64_t)RDNA4RT_INSTANCE_SBT_OFFSET_MAX + 1u };
    CHECK(rdna4rt_compute_geom_ift_base(oob_geom, 1u, &base, &non_consec) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "geometry iftOffset > 24-bit rejected");

    const uint64_t oob_geom2[2] = { 0u, (uint64_t)RDNA4RT_INSTANCE_SBT_OFFSET_MAX + 1u };
    CHECK(rdna4rt_compute_geom_ift_base(oob_geom2, 2u, &base, &non_consec) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "second geometry iftOffset > 24-bit rejected");

    /* 2. Overflow-safe SBT offset combination. */
    uint32_t comb = 0u;
    CHECK(rdna4rt_combine_sbt_offset(0u, 2u, &comb) == RDNA4RT_OK && comb == 2u,
          "combine 0 + 2 == 2 (Apple Sample 1 sphere)");
    CHECK(rdna4rt_combine_sbt_offset(5u, 2u, &comb) == RDNA4RT_OK && comb == 7u,
          "combine 5 + 2 == 7");
    CHECK(rdna4rt_combine_sbt_offset(RDNA4RT_INSTANCE_SBT_OFFSET_MAX, 0u, &comb) == RDNA4RT_OK &&
          comb == RDNA4RT_INSTANCE_SBT_OFFSET_MAX,
          "combine max + 0 == max");
    CHECK(rdna4rt_combine_sbt_offset(RDNA4RT_INSTANCE_SBT_OFFSET_MAX, 1u, &comb) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "combine 0x00FFFFFF + 1 rejected as 24-bit overflow");
    CHECK(rdna4rt_combine_sbt_offset(UINT32_MAX, 2u, &comb) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "combine UINT32_MAX + 2 rejected without u32 wraparound");

    /* 3. Lifecycle propagation of geom_ift_base across build / refit / copy / compact / rebuild. */
    rdna4rt_accel_meta_t blas, copied, compacted, refitted, tlas;
    rdna4rt_meta_init(&blas, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&copied, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&compacted, 2048u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&refitted, 4096u, RDNA4RT_KIND_UNKNOWN);
    rdna4rt_meta_init(&tlas, 4096u, RDNA4RT_KIND_UNKNOWN);
    uint32_t sig[1] = { rdna4rt_geom_sig(RDNA4RT_GEOM_AABBS, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, 0u) };
    uint32_t pc[1] = { 1u };
    uint64_t topo = rdna4rt_topology_hash(sig, pc, 1u);

    CHECK(rdna4rt_state_apply_build_ex(&blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_REFIT, 1u, topo, 2u) == RDNA4RT_OK &&
          blas.geom_ift_base == 2u,
          "build_ex stores BLAS geom_ift_base=2");
    CHECK(rdna4rt_state_apply_copy(&blas, &copied, 0) == RDNA4RT_OK && copied.geom_ift_base == 2u,
          "copy propagates geom_ift_base=2");
    CHECK(rdna4rt_state_apply_compact(&blas, &compacted, 0, 1024u) == RDNA4RT_OK && compacted.geom_ift_base == 2u,
          "copyAndCompact propagates geom_ift_base=2 (Apple Sample 1 sphere path)");
    CHECK(rdna4rt_state_apply_refit(&blas, &refitted, RDNA4RT_KIND_BLAS, 2048u, 1u, topo, RDNA4RT_REFIT_OPT_VERTEX_DATA) == RDNA4RT_OK &&
          refitted.geom_ift_base == 2u,
          "refit preserves geom_ift_base=2");
    CHECK(rdna4rt_state_apply_refit_ex(&blas, &refitted, RDNA4RT_KIND_BLAS, 2048u, 1u, topo, RDNA4RT_REFIT_OPT_VERTEX_DATA, 5u) == RDNA4RT_OK &&
          refitted.geom_ift_base == 5u,
          "refit_ex updates BLAS geom_ift_base=5");
    CHECK(rdna4rt_state_apply_build(&blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_NONE, 1u, topo) == RDNA4RT_OK &&
          blas.geom_ift_base == 0u,
          "rebuild without IFT base resets geom_ift_base to 0");
    CHECK(rdna4rt_state_apply_build_ex(&tlas, RDNA4RT_KIND_TLAS, 2048u, RDNA4RT_USAGE_NONE, 1u, topo, 7u) == RDNA4RT_OK &&
          tlas.geom_ift_base == 0u,
          "TLAS build forces geom_ift_base to 0");
    CHECK(rdna4rt_state_apply_build_ex(&blas, RDNA4RT_KIND_BLAS, 2048u, RDNA4RT_USAGE_NONE, 1u, topo,
                                       RDNA4RT_INSTANCE_SBT_OFFSET_MAX + 1u) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "build_ex rejects out-of-range geom_ift_base");

    /* 4. CPU TLAS conversion adds blas_ift_offsets[accel_struct_index] (Default + UserID + overflow + Indirect). */
    const uint64_t blas_addrs[3] = { UINT64_C(0x10000000), UINT64_C(0x20000000), UINT64_C(0x30000000) };
    const uint32_t blas_ifts[3]  = { 0u, 1u, 2u }; /* Apple Sample 1: light=0, box=1, sphere=2 */
    rdna4rt_packed_float4x3_t ident = {{ {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0} }};
    rdna4rt_mtl_instance_default_t defs[3] = {
        { .transform = ident, .options = RDNA4RT_INST_OPT_OPAQUE, .mask = 0xFFu, .intersection_function_offset = 0u, .accel_struct_index = 0u },
        { .transform = ident, .options = RDNA4RT_INST_OPT_OPAQUE, .mask = 0xFFu, .intersection_function_offset = 0u, .accel_struct_index = 1u },
        { .transform = ident, .options = RDNA4RT_INST_OPT_NON_OPAQUE, .mask = 0xFFu, .intersection_function_offset = 0u, .accel_struct_index = 2u },
    };
    rdna4rt_vk_instance_t vk_defs[3];
    CHECK(rdna4rt_convert_instances_ex(RDNA4RT_INST_DESC_DEFAULT, (const uint8_t *)defs, sizeof(defs),
                                       0u, 0u, 3u, blas_addrs, blas_ifts, 3u, vk_defs) == RDNA4RT_OK,
          "Apple Sample 1 Default TLAS conversion with per-BLAS geom IFT offsets");
    CHECK((vk_defs[0].sbt_offset_and_flags & 0x00FFFFFFu) == 0u &&
          (vk_defs[1].sbt_offset_and_flags & 0x00FFFFFFu) == 1u &&
          (vk_defs[2].sbt_offset_and_flags & 0x00FFFFFFu) == 2u &&
          (vk_defs[2].sbt_offset_and_flags >> 24) == RDNA4RT_VK_INST_FORCE_NO_OPAQUE &&
          vk_defs[2].accel_struct_reference == blas_addrs[2],
          "Apple Sample 1 sphere instance receives instanceSBT=2 without clobbering flags/BDA");

    rdna4rt_mtl_instance_userid_t uid = {
        .transform = ident,
        .options = RDNA4RT_INST_OPT_DISABLE_CULLING,
        .mask = 0x5Au,
        .intersection_function_offset = 5u,
        .accel_struct_index = 2u,
        .user_id = 0x000123u
    };
    rdna4rt_vk_instance_t vk_uid;
    CHECK(rdna4rt_convert_instances_ex(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&uid, sizeof(uid),
                                       0u, 0u, 1u, blas_addrs, blas_ifts, 3u, &vk_uid) == RDNA4RT_OK &&
          (vk_uid.sbt_offset_and_flags & 0x00FFFFFFu) == 7u &&
          (vk_uid.sbt_offset_and_flags >> 24) == RDNA4RT_VK_INST_DISABLE_CULLING &&
          (vk_uid.custom_index_and_mask & 0x00FFFFFFu) == 0x000123u &&
          (vk_uid.custom_index_and_mask >> 24) == 0x5Au,
          "UserID instance combines inst.iftOffset(5) + blas.geomIftBase(2) == 7");

    uid.intersection_function_offset = RDNA4RT_INSTANCE_SBT_OFFSET_MAX;
    CHECK(rdna4rt_convert_instances_ex(RDNA4RT_INST_DESC_USER_ID, (const uint8_t *)&uid, sizeof(uid),
                                       0u, 0u, 1u, blas_addrs, blas_ifts, 3u, &vk_uid) == RDNA4RT_ERR_OUT_OF_RANGE_INDEX,
          "UserID instance combined SBT > 24-bit rejected");

    /* Indirect instance keeps its own intersection_function_offset intact. */
    rdna4rt_mtl_instance_indirect_t ind = {
        .transform = ident,
        .options = RDNA4RT_INST_OPT_NONE,
        .mask = 0xFFu,
        .intersection_function_offset = 9u,
        .user_id = 77u,
        .accel_struct_id = blas_addrs[2]
    };
    rdna4rt_vk_instance_t vk_ind;
    CHECK(rdna4rt_convert_instances_ex(RDNA4RT_INST_DESC_INDIRECT, (const uint8_t *)&ind, sizeof(ind),
                                       0u, 0u, 1u, NULL, NULL, 0u, &vk_ind) == RDNA4RT_OK &&
          (vk_ind.sbt_offset_and_flags & 0x00FFFFFFu) == 9u &&
          vk_ind.accel_struct_reference == blas_addrs[2],
          "Indirect instance conversion remains intact");

    /* 5. GPU inst_convert SPIR-V & BlasEntry ABI pins. */
    CHECK(sizeof(rdna4rt_inst_convert_pc_t) == 48u &&
          sizeof(rdna4rt_blas_table_entry_t) == 16u &&
          offsetof(rdna4rt_blas_table_entry_t, bda) == 0u &&
          offsetof(rdna4rt_blas_table_entry_t, geom_ift_base) == 8u,
          "GPU inst_convert PC (48B) and BlasTable entry (16B) layouts pinned");
    CHECK(rdna4rt_spv_inst_convert[0] == 0x07230203u &&
          rdna4rt_spv_inst_convert[1] == 0x00010600u &&
          rdna4rt_spv_xform_transpose[0] == 0x07230203u,
          "embedded SPIR-V 1.6 headers valid");
}

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
        CHECK(h_lin == 1 && h_cub == 1 && fabsf(t_lin - t_cub) < 5e-3f && fabsf(u_lin - u_cub) < 2e-2f,
              "tapered linear vs cubic Bezier differential hit agreement");

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
    test_caps_and_alignment();
    test_usage_sizes_and_scratch();
    test_blas_triangle_and_aabb();
    test_tlas_and_instance_conversion();
    test_motion_blur_validation_and_interpolation();
    test_lifecycle_state_machine();
    test_heap_batch_asan();
    test_wave4_prim_data_validation_and_layout();
    test_wave4_prim_refit_and_barriers();
    test_wave4_heap_as_sizing_and_validation();
    test_wave4_prim_and_query_pools();
    test_wave5_geom_ift_offset_and_spv();
    test_wave2_curve_geom_validation_and_intersectors();
    printf("test-rt-accel: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
