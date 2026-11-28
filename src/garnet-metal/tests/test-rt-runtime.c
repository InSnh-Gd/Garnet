// Pure-C host unit tests for garnet_rt_runtime.h and Task 7.3 runtime source pins.
// Build (GCC):
//   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-runtime.c -o test-rt-runtime
// Build (Clang ASan+UBSan):
//   clang -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-runtime.c -o test-rt-runtime
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_rt_runtime.h"

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

static rdna4rtr_hw_caps_t make_full_gfx12_caps(void) {
    return (rdna4rtr_hw_caps_t){
        .accel_struct_supported    = 1u,
        .ray_query_supported       = 1u,
        .buffer_device_address     = 1u,
        .deferred_host_ops         = 1u,
        .translator_stage3_ready   = 1u,
        .motion_blur_ext_supported = 0u,
        .curve_geometry_supported  = 0u,
    };
}

static void test_capabilities_and_footnote10(void) {
    rdna4rtr_hw_caps_t zero;
    memset(&zero, 0, sizeof zero);
    CHECK(!rdna4rtr_supports_raytracing(NULL), "NULL caps -> no raytracing");
    CHECK(!rdna4rtr_supports_raytracing(&zero), "zero caps -> no raytracing");
    CHECK(!rdna4rtr_supports_raytracing_from_render(&zero), "zero caps -> no render raytracing");
    CHECK(!rdna4rtr_supports_primitive_motion_blur(&zero), "zero caps -> no motion blur");
    CHECK(!rdna4rtr_supports_curve_intersections(&zero), "zero caps -> no curve intersections");

    rdna4rtr_hw_caps_t full = make_full_gfx12_caps();
    CHECK(rdna4rtr_supports_raytracing(&full) == 1, "GFX12 full caps -> supportsRaytracing");
    CHECK(rdna4rtr_supports_raytracing_from_render(&full) == 1, "GFX12 full caps -> supportsRaytracingFromRender");
    CHECK(rdna4rtr_supports_primitive_motion_blur(&full) == 0, "GFX12 full caps -> honestly NO primitive motion blur");
    CHECK(rdna4rtr_supports_curve_intersections(&full) == 0, "GFX12 full caps -> honestly NO curve intersections");

    // Removing any single prerequisite must gate both compute and render RT off.
    for (int bit = 0; bit < 5; bit++) {
        rdna4rtr_hw_caps_t c = full;
        if (bit == 0) c.accel_struct_supported = 0u;
        if (bit == 1) c.ray_query_supported = 0u;
        if (bit == 2) c.buffer_device_address = 0u;
        if (bit == 3) c.deferred_host_ops = 0u;
        if (bit == 4) c.translator_stage3_ready = 0u;
        CHECK(!rdna4rtr_supports_raytracing(&c), "missing RT prerequisite disables supportsRaytracing");
        CHECK(!rdna4rtr_supports_raytracing_from_render(&c), "missing RT prerequisite disables supportsRaytracingFromRender");
    }

    // Even if motion_blur_ext_supported were 1, missing core RT gates it off.
    rdna4rtr_hw_caps_t mb_no_rt = zero;
    mb_no_rt.motion_blur_ext_supported = 1u;
    CHECK(!rdna4rtr_supports_primitive_motion_blur(&mb_no_rt), "motion blur requires core RT");

    // Footnote 10 (AMD 6000-series parity): mesh/object cannot combine with render RT.
    CHECK(rdna4rtr_mesh_raytracing_conflict(0, 0) == RDNA4RTR_OK, "no mesh + no RT ok");
    CHECK(rdna4rtr_mesh_raytracing_conflict(1, 0) == RDNA4RTR_OK, "mesh + no RT ok");
    CHECK(rdna4rtr_mesh_raytracing_conflict(0, 1) == RDNA4RTR_OK, "vertex/frag + RT ok");
    CHECK(rdna4rtr_mesh_raytracing_conflict(1, 1) == RDNA4RTR_ERR_FOOTNOTE10, "mesh + render RT refused (Footnote 10)");

    // Stage descriptor set mapping & binding-band conversion.
    CHECK(rdna4rtr_stage_descriptor_set(RDNA4RTR_STAGE_COMPUTE) == 0u, "compute -> set 0");
    CHECK(rdna4rtr_stage_descriptor_set(RDNA4RTR_STAGE_VERTEX) == 0u, "vertex -> set 0");
    CHECK(rdna4rtr_stage_descriptor_set(RDNA4RTR_STAGE_FRAGMENT) == 1u, "fragment -> set 1");

    uint32_t midx = 99u;
    CHECK(rdna4rtr_binding_to_metal_index(0u, 1u, &midx) && midx == 0u, "binding 0 -> metal 0");
    CHECK(rdna4rtr_binding_to_metal_index(30u, 1u, &midx) && midx == 30u, "binding 30 -> metal 30");
    CHECK(!rdna4rtr_binding_to_metal_index(31u, 1u, &midx), "binding 31 OOB");
    CHECK(!rdna4rtr_binding_to_metal_index(30u, 2u, &midx), "binding 30 count 2 OOB");
    CHECK(!rdna4rtr_binding_to_metal_index(0u, 0u, &midx), "count 0 rejected");
    CHECK(GARNETX_CAP_RT_POSITION_FETCH == (1u << 9), "GARNETX_CAP_RT_POSITION_FETCH == 1<<9");
    CHECK(GARNETX_CAP_RT_IFT_ABI_V2 == (1u << 10), "GARNETX_CAP_RT_IFT_ABI_V2 == 1<<10");
}

static void test_as_reflection_json(void) {
    rdna4rtr_as_reflection_t r;

    // Non-RT / null / empty documents.
    const char *empty_doc = "{\"entry\":\"main\",\"buffers\":[]}";
    CHECK(rdna4rtr_parse_as_json(empty_doc, strlen(empty_doc), &r) == RDNA4RTR_OK &&
          r.present == 0u && r.count == 0u, "non-RT JSON has present=0");

    const char *null_as = "{\"acceleration_structures\":null}";
    CHECK(rdna4rtr_parse_as_json(null_as, strlen(null_as), &r) == RDNA4RTR_OK &&
          r.present == 0u && r.count == 0u, "null acceleration_structures -> present=0");

    const char *arr_empty = "{\"acceleration_structures\":[]}";
    CHECK(rdna4rtr_parse_as_json(arr_empty, strlen(arr_empty), &r) == RDNA4RTR_OK &&
          r.present == 0u && r.count == 0u, "empty acceleration_structures -> present=0");

    // Full 0077-schema document with Descriptor (Instance + Primitive) and BDA (`u64_device_address`).
    const char *valid_doc =
        "{"
        "  \"entry_point\": \"rt_kernel\","
        "  \"buffers\": [{\"metal_index\": 0, \"binding\": 0}],"
        "  \"acceleration_structures\": ["
        "    {"
        "      \"param_index\": 1,"
        "      \"metal_index\": 2,"
        "      \"kind\": \"Instance\","
        "      \"binding_mode\": \"Descriptor\","
        "      \"descriptor\": {\"set\": 0, \"binding\": 2, \"count\": 1},"
        "      \"argument_buffer_index\": null,"
        "      \"byte_offset\": null,"
        "      \"bda_abi\": null"
        "    },"
        "    {"
        "      \"param_index\": 2,"
        "      \"metal_index\": 5,"
        "      \"kind\": \"Primitive\","
        "      \"binding_mode\": \"Descriptor\","
        "      \"descriptor\": {\"set\": 1, \"binding\": 5, \"count\": 1}"
        "    },"
        "    {"
        "      \"param_index\": null,"
        "      \"metal_index\": 0,"
        "      \"kind\": \"Instance\","
        "      \"binding_mode\": \"BufferDeviceAddress\","
        "      \"descriptor\": null,"
        "      \"argument_buffer_index\": 1,"
        "      \"byte_offset\": 16,"
        "      \"bda_abi\": \"u64_device_address\","
        "      \"future_field\": [1, true, {\"nested\": \"ok\"}]"
        "    }"
        "  ]"
        "}";
    CHECK(rdna4rtr_parse_as_json(valid_doc, strlen(valid_doc), &r) == RDNA4RTR_OK, "valid 0077 AS JSON parses");
    CHECK(r.present == 1u && r.count == 3u && r.descriptor_count == 2u && r.bda_count == 1u,
          "AS counts: 3 total, 2 descriptor, 1 BDA");
    CHECK(r.entries[0].kind == RDNA4RTR_AS_KIND_INSTANCE && r.entries[0].mode == RDNA4RTR_AS_BIND_DESCRIPTOR &&
          r.entries[0].has_param_index == 1u && r.entries[0].param_index == 1u &&
          r.entries[0].metal_index == 2u && r.entries[0].set == 0u && r.entries[0].binding == 2u,
          "entry 0 Instance Descriptor fields match");
    CHECK(r.entries[1].kind == RDNA4RTR_AS_KIND_PRIMITIVE && r.entries[1].set == 1u && r.entries[1].binding == 5u,
          "entry 1 Primitive Descriptor fields match");
    CHECK(r.entries[2].mode == RDNA4RTR_AS_BIND_BDA && r.entries[2].has_param_index == 0u &&
          r.entries[2].has_argbuf_index == 1u && r.entries[2].argbuf_index == 1u &&
          r.entries[2].has_byte_offset == 1u && r.entries[2].byte_offset == 16u,
          "entry 2 BDA fields match");

    const rdna4rtr_as_binding_t *found = rdna4rtr_as_find_descriptor(&r, 2u);
    CHECK(found != NULL && found->kind == RDNA4RTR_AS_KIND_INSTANCE, "find_descriptor(2) finds Instance AS");
    CHECK(rdna4rtr_as_find_descriptor(&r, 0u) == NULL, "find_descriptor ignores BDA entry");
    const rdna4rtr_as_binding_t *by_b = rdna4rtr_as_find_by_binding(&r, 1u, 5u);
    CHECK(by_b != NULL && by_b->kind == RDNA4RTR_AS_KIND_PRIMITIVE, "find_by_binding(1,5) finds Primitive AS");

    // Negative cases: ABI mismatch, bounds, duplicates, alignment, malformed JSON.
    const char *bad_bind =
        "{\"acceleration_structures\":[{\"metal_index\":2,\"kind\":\"Instance\","
        "\"binding_mode\":\"Descriptor\",\"descriptor\":{\"set\":0,\"binding\":9,\"count\":1}}]}";
    CHECK(rdna4rtr_parse_as_json(bad_bind, strlen(bad_bind), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "Descriptor binding != BUFFER_BINDING_BASE+metal_index rejected");

    const char *bad_desc_bda =
        "{\"acceleration_structures\":[{\"metal_index\":2,\"kind\":\"Instance\","
        "\"binding_mode\":\"Descriptor\",\"descriptor\":{\"set\":0,\"binding\":2,\"count\":1},"
        "\"bda_abi\":\"u64_device_address\"}]}";
    CHECK(rdna4rtr_parse_as_json(bad_desc_bda, strlen(bad_desc_bda), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "Descriptor mode carrying bda_abi rejected");

    const char *bad_slot =
        "{\"acceleration_structures\":[{\"metal_index\":31,\"kind\":\"Instance\","
        "\"binding_mode\":\"Descriptor\",\"descriptor\":{\"set\":0,\"binding\":31,\"count\":1}}]}";
    CHECK(rdna4rtr_parse_as_json(bad_slot, strlen(bad_slot), &r) == RDNA4RTR_ERR_BOUNDS,
          "Descriptor metal_index 31 rejected");

    const char *dup_desc =
        "{\"acceleration_structures\":["
        "{\"metal_index\":3,\"kind\":\"Instance\",\"binding_mode\":\"Descriptor\",\"descriptor\":{\"set\":0,\"binding\":3,\"count\":1}},"
        "{\"metal_index\":3,\"kind\":\"Primitive\",\"binding_mode\":\"Descriptor\",\"descriptor\":{\"set\":0,\"binding\":3,\"count\":1}}]}";
    CHECK(rdna4rtr_parse_as_json(dup_desc, strlen(dup_desc), &r) == RDNA4RTR_ERR_DUPLICATE,
          "duplicate Descriptor slot rejected");

    const char *bad_bda_abi =
        "{\"acceleration_structures\":[{\"metal_index\":0,\"kind\":\"Instance\","
        "\"binding_mode\":\"BufferDeviceAddress\",\"argument_buffer_index\":0,\"byte_offset\":8,\"bda_abi\":\"u32_idx\"}]}";
    CHECK(rdna4rtr_parse_as_json(bad_bda_abi, strlen(bad_bda_abi), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "wrong bda_abi string rejected");

    const char *bad_bda_align =
        "{\"acceleration_structures\":[{\"metal_index\":0,\"kind\":\"Instance\","
        "\"binding_mode\":\"BufferDeviceAddress\",\"argument_buffer_index\":0,\"byte_offset\":12,\"bda_abi\":\"u64_device_address\"}]}";
    CHECK(rdna4rtr_parse_as_json(bad_bda_align, strlen(bad_bda_align), &r) == RDNA4RTR_ERR_ALIGNMENT,
          "unaligned BDA byte_offset rejected");

    const char *bad_bda_argbuf =
        "{\"acceleration_structures\":[{\"metal_index\":0,\"kind\":\"Instance\","
        "\"binding_mode\":\"BufferDeviceAddress\",\"argument_buffer_index\":31,\"byte_offset\":0,\"bda_abi\":\"u64_device_address\"}]}";
    CHECK(rdna4rtr_parse_as_json(bad_bda_argbuf, strlen(bad_bda_argbuf), &r) == RDNA4RTR_ERR_BOUNDS,
          "BDA argument_buffer_index 31 rejected");

    const char *dup_bda =
        "{\"acceleration_structures\":["
        "{\"metal_index\":0,\"kind\":\"Instance\",\"binding_mode\":\"BufferDeviceAddress\",\"argument_buffer_index\":1,\"byte_offset\":8,\"bda_abi\":\"u64_device_address\"},"
        "{\"metal_index\":1,\"kind\":\"Primitive\",\"binding_mode\":\"BufferDeviceAddress\",\"argument_buffer_index\":1,\"byte_offset\":8,\"bda_abi\":\"u64_device_address\"}]}";
    CHECK(rdna4rtr_parse_as_json(dup_bda, strlen(dup_bda), &r) == RDNA4RTR_ERR_DUPLICATE,
          "duplicate BDA (argbuf, byte_offset) rejected");

    const char *bad_kind =
        "{\"acceleration_structures\":[{\"metal_index\":0,\"kind\":\"Curve\",\"binding_mode\":\"Descriptor\","
        "\"descriptor\":{\"set\":0,\"binding\":0,\"count\":1}}]}";
    CHECK(rdna4rtr_parse_as_json(bad_kind, strlen(bad_kind), &r) == RDNA4RTR_ERR_JSON,
          "unknown AS kind rejected");

    const char *trailing = "{\"acceleration_structures\":[]} garbage";
    CHECK(rdna4rtr_parse_as_json(trailing, strlen(trailing), &r) == RDNA4RTR_ERR_JSON,
          "trailing garbage after root object rejected");

    // Nesting depth DoS guard (>64 nested arrays in skipped field).
    char deep[256];
    size_t pos = 0;
    memcpy(deep + pos, "{\"x\":", 5); pos += 5;
    for (int i = 0; i < 80; i++) deep[pos++] = '[';
    for (int i = 0; i < 80; i++) deep[pos++] = ']';
    deep[pos++] = '}';
    deep[pos] = '\0';
    CHECK(rdna4rtr_parse_as_json(deep, pos, &r) == RDNA4RTR_ERR_JSON,
          "excessive JSON nesting depth rejected cleanly");
}

static void test_ift_reflection_json(void) {
    rdna4rtr_ift_reflection_t r;

    const char *no_ift = "{\"entry\":\"main\"}";
    CHECK(rdna4rtr_parse_ift_json(no_ift, strlen(no_ift), &r) == RDNA4RTR_OK && r.present == 0u,
          "absent IFT -> present=0");
    rdna4rtr_ift_refl_destroy(&r);

    const char *null_ift = "{\"intersection_function_tables\":null}";
    CHECK(rdna4rtr_parse_ift_json(null_ift, strlen(null_ift), &r) == RDNA4RTR_OK && r.present == 0u,
          "null IFT -> present=0");
    rdna4rtr_ift_refl_destroy(&r);

    const char *valid_ift =
        "{"
        "  \"acceleration_structures\": ["
        "    {\"param_index\": 0, \"metal_index\": 0, \"kind\": \"Instance\", \"binding_mode\": \"Descriptor\","
        "     \"descriptor\": {\"set\": 0, \"binding\": 0, \"count\": 1}}"
        "  ],"
        "  \"intersection_function_tables\": {"
        "    \"abi_version\": 1,"
        "    \"header_words\": 1,"
        "    \"tables\": ["
        "      {\"param_index\": 1, \"metal_index\": 4, \"descriptor\": {\"set\": 0, \"binding\": 4, \"count\": 1}},"
        "      {\"param_index\": 2, \"metal_index\": 6, \"descriptor\": null}"
        "    ],"
        "    \"handles\": ["
        "      {\"handle_id\": 1, \"symbol\": \"aabb_sphere\", \"kind\": \"BoundingBox\"},"
        "      {\"handle_id\": 2, \"symbol\": \"tri_alpha_\\u0066ilter\", \"kind\": \"TriangleAnyHit\"},"
        "      {\"handle_id\": 3, \"symbol\": \"aabb_cone\", \"kind\": \"BoundingBox\"}"
        "    ]"
        "  }"
        "}";
    rdna4rtr_reflection_t full;
    CHECK(rdna4rtr_parse_reflection_json(valid_ift, strlen(valid_ift), &full) == RDNA4RTR_OK,
          "combined AS + IFT reflection JSON parses");
    CHECK(rdna4rtr_refl_uses_raytracing(&full) == 1, "refl_uses_raytracing == 1");
    CHECK(full.as.present == 1u && full.as.count == 1u, "combined AS parsed");
    CHECK(full.ift.present == 1u && full.ift.abi_version == 1u && full.ift.header_words == 1u &&
          full.ift.table_count == 2u && full.ift.handle_count == 3u && full.ift.max_handle_id == 3u,
          "IFT metadata header/tables/handles parsed");
    CHECK(rdna4rtr_ift_refl_has_table_at_buffer(&full.ift, 4u) == 1 &&
          rdna4rtr_ift_refl_has_table_at_buffer(&full.ift, 6u) == 1 &&
          rdna4rtr_ift_refl_has_table_at_buffer(&full.ift, 5u) == 0,
          "IFT table buffer index lookups accurate");

    uint32_t kind = 99u;
    CHECK(rdna4rtr_ift_refl_find_handle(&full.ift, "aabb_sphere", &kind) == 1u &&
          kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX, "lookup BoundingBox handle 1");
    CHECK(rdna4rtr_ift_refl_find_handle(&full.ift, "tri_alpha_filter", &kind) == 2u &&
          kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT, "lookup unescaped TriangleAnyHit handle 2");
    CHECK(rdna4rtr_ift_refl_find_handle(&full.ift, "nonexistent", &kind) == 0u, "missing symbol -> 0");
    const rdna4rtr_ift_handle_entry_t *e3 = rdna4rtr_ift_refl_find_by_id(&full.ift, 3u);
    CHECK(e3 != NULL && strcmp(e3->symbol, "aabb_cone") == 0 && e3->kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX,
          "find_by_id(3) returns aabb_cone");
    rdna4rtr_refl_destroy(&full);

    // Negative IFT cases (also exercises leak-free cleanup on mid-parse failures under ASan).
    const char *bad_abi =
        "{\"intersection_function_tables\":{\"abi_version\":2,\"header_words\":1,\"tables\":[],\"handles\":[]}}";
    CHECK(rdna4rtr_parse_ift_json(bad_abi, strlen(bad_abi), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "IFT abi_version != 1 rejected");

    const char *bad_hdr =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":2,\"tables\":[],\"handles\":[]}}";
    CHECK(rdna4rtr_parse_ift_json(bad_hdr, strlen(bad_hdr), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "IFT header_words != 1 rejected");

    const char *zero_hid =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":0,\"symbol\":\"bad\",\"kind\":\"BoundingBox\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(zero_hid, strlen(zero_hid), &r) == RDNA4RTR_ERR_BOUNDS,
          "IFT handle_id 0 rejected");

    const char *dup_hid =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"a\",\"kind\":\"BoundingBox\"},"
        "{\"handle_id\":1,\"symbol\":\"b\",\"kind\":\"TriangleAnyHit\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(dup_hid, strlen(dup_hid), &r) == RDNA4RTR_ERR_DUPLICATE,
          "duplicate IFT handle_id rejected (leak-free)");

    const char *dup_sym =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"same\",\"kind\":\"BoundingBox\"},"
        "{\"handle_id\":2,\"symbol\":\"same\",\"kind\":\"TriangleAnyHit\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(dup_sym, strlen(dup_sym), &r) == RDNA4RTR_ERR_DUPLICATE,
          "duplicate IFT symbol rejected (leak-free)");

    const char *bad_kind =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,\"tables\":[],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"a\",\"kind\":\"UnknownKind\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(bad_kind, strlen(bad_kind), &r) == RDNA4RTR_ERR_JSON,
          "unknown IFT handle kind rejected");

    const char *bad_tbind =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,"
        "\"tables\":[{\"param_index\":0,\"metal_index\":3,\"descriptor\":{\"set\":0,\"binding\":7,\"count\":1}}],"
        "\"handles\":[]}}";
    CHECK(rdna4rtr_parse_ift_json(bad_tbind, strlen(bad_tbind), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "IFT table descriptor binding mismatch rejected");

    const char *dup_tbl =
        "{\"intersection_function_tables\":{\"abi_version\":1,\"header_words\":1,"
        "\"tables\":[{\"param_index\":0,\"metal_index\":3},{\"param_index\":1,\"metal_index\":3}],"
        "\"handles\":[]}}";
    CHECK(rdna4rtr_parse_ift_json(dup_tbl, strlen(dup_tbl), &r) == RDNA4RTR_ERR_DUPLICATE,
          "duplicate IFT table metal_index rejected");

    // Wave 2 IFT ABI v2 reflection (abi_version=2, header_words=66) accepted; mismatched header rejected.
    const char *v2_doc =
        "{\"intersection_function_tables\":{\"abi_version\":2,\"header_words\":66,"
        "\"tables\":[{\"param_index\":1,\"metal_index\":1,\"descriptor\":{\"set\":0,\"binding\":1,\"count\":1}}],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"ift_full_cb\",\"kind\":\"BoundingBox\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(v2_doc, strlen(v2_doc), &r) == RDNA4RTR_OK &&
          r.present == 1u && r.abi_version == 2u && r.header_words == 66u &&
          r.table_count == 1u && r.handle_count == 1u,
          "IFT ABI v2 reflection JSON accepted");
    rdna4rtr_ift_refl_destroy(&r);

    const char *v2_bad_hdr =
        "{\"intersection_function_tables\":{\"abi_version\":2,\"header_words\":1,"
        "\"tables\":[],\"handles\":[]}}";
    CHECK(rdna4rtr_parse_ift_json(v2_bad_hdr, strlen(v2_bad_hdr), &r) == RDNA4RTR_ERR_ABI_MISMATCH,
          "IFT ABI v2 with header_words=1 rejected");
}

static void test_ir_kind_inference(void) {
    const char *src =
        "define zeroext i1 @tri_anyhit_fn(<3 x float> %o, <3 x float> %d) {\n"
        "  ret i1 true\n"
        "}\n"
        "define { i1, float } @sphere_bbox_fn(<3 x float> %o, <3 x float> %d) {\n"
        "  ret { i1, float } zeroinitializer\n"
        "}\n"
        "define %struct.bounding_box_res @\"quoted_bbox\"(float %tmin) {\n"
        "  ret %struct.bounding_box_res zeroinitializer\n"
        "}\n"
        "define void @sret_bbox(ptr sret(%struct.res) %out) {\n"
        "  ret void\n"
        "}\n"
        "!42 = !{ptr @meta_only_box, !\"bounding_box\"}\n"
        "!43 = !{ptr @meta_only_tri, !\"triangle\"}\n"
        // Real Cycles Apple Clang 21 opaque-pointer AIR callbacks & !air.intersection metadata:
        "define <{ i1, i1 }> @__intersection__tri(ptr addrspace(2) %lp, i32 %obj) local_unnamed_addr #0 {\n"
        "  ret <{ i1, i1 }> <{ i1 true, i1 false }>\n"
        "}\n"
        "define <{ i1, i1 }> @__intersection__tri_shadow_all(ptr addrspace(2) %lp, i32 %obj) local_unnamed_addr #0 {\n"
        "  ret <{ i1, i1 }> <{ i1 true, i1 true }>\n"
        "}\n"
        "define <{ i1, i1, float }> @__intersection__point(ptr addrspace(2) %lp, i32 %obj) local_unnamed_addr #0 {\n"
        "  ret <{ i1, i1, float }> zeroinitializer\n"
        "}\n"
        "define <{ i1, i1 }> @__intersection__tri_nometa(i32 %prim) {\n"
        "  ret <{ i1, i1 }> <{ i1 true, i1 false }>\n"
        "}\n"
        "define <{ i1, i1 }> @__intersection__curve(ptr addrspace(2) %lp, float %u) {\n"
        "  ret <{ i1, i1 }> <{ i1 true, i1 false }>\n"
        "}\n"
        // Metadata precedence: %struct return overridden by explicit !"air.triangle" metadata.
        "define %struct.tri_custom @meta_precedence_tri(i32 %p) {\n"
        "  ret %struct.tri_custom zeroinitializer\n"
        "}\n"
        // Prefix disambiguation: @__intersection__tri_box_suffix must not hijack @__intersection__tri.
        "!air.intersection = !{!14, !15, !16, !17, !18, !19}\n"
        "!14 = !{ptr @__intersection__tri, !1, !2, !\"air.triangle\", !\"air.instancing\", !\"air.triangle_data\", !\"air.curve_data\", !\"air.extended_limits\"}\n"
        "!15 = !{ptr @__intersection__tri_shadow_all, !1, !2, !\"air.triangle\", !\"air.instancing\", !\"air.triangle_data\", !\"air.curve_data\"}\n"
        "!16 = !{ptr @__intersection__point, !1, !3, !\"air.bounding_box\", !\"air.instancing\", !\"air.triangle_data\", !\"air.curve_data\", !\"air.extended_limits\"}\n"
        "!17 = !{ptr @__intersection__curve, !1, !2, !\"air.curve\", !\"air.instancing\", !\"air.triangle_data\", !\"air.curve_data\"}\n"
        "!18 = !{ptr @meta_precedence_tri, !1, !2, !\"air.triangle\"}\n"
        "!19 = !{ptr @__intersection__tri_box_suffix, !1, !3, !\"air.bounding_box\"}\n";

    // Allocate an exact non-NUL-terminated copy to verify strict length-bounded scanning under ASan.
    size_t slen = strlen(src);
    char *raw = (char *)malloc(slen);
    CHECK(raw != NULL, "alloc non-NUL IR buffer");
    if (raw) {
        memcpy(raw, src, slen);
        uint32_t kind = 99u;
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "tri_anyhit_fn", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT, "i1 return -> TriangleAnyHit");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "sphere_bbox_fn", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX, "{ i1, float } return -> BoundingBox");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "quoted_bbox", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX, "quoted %struct return -> BoundingBox");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "sret_bbox", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX, "sret param -> BoundingBox");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "meta_only_box", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX, "metadata bounding_box fallback");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "meta_only_tri", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT, "metadata triangle fallback");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__tri", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT,
              "Cycles __intersection__tri (<{ i1, i1 }> + !\"air.triangle\") -> TriangleAnyHit");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__tri_shadow_all", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT,
              "Cycles __intersection__tri_shadow_all -> TriangleAnyHit");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__point", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX,
              "Cycles __intersection__point (<{ i1, i1, float }> + !\"air.bounding_box\" + !\"air.triangle_data\") -> BoundingBox");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__tri_nometa", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT,
              "<{ i1, i1 }> return without metadata -> TriangleAnyHit");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__curve", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_CURVE,
              "Cycles __intersection__curve (!\"air.curve\" + !\"air.triangle_data\") -> Curve");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "meta_precedence_tri", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT,
              "!air.intersection !\"air.triangle\" metadata takes precedence over %struct return");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "__intersection__tri_box_suffix", &kind) == 1 &&
              kind == RDNA4RTR_IFT_KIND_BOUNDING_BOX,
              "symbol boundary matching distinguishes __intersection__tri_box_suffix from __intersection__tri");
        CHECK(rdna4rtr_infer_ift_kind_from_ir(raw, slen, "unknown_sym", &kind) == 0,
              "unknown symbol returns 0");
        free(raw);
    }
}

static uint32_t rt_u32(uint32_t v) {
    volatile uint32_t x = v;
    return x;
}

static void test_ift_encoder_and_dispatch_sim(void) {
    // 1. Empty table (0 slots): valid 16B-aligned buffer, word[0]=0, safe OOB behavior.
    rdna4rtr_ift_table_t empty_t;
    CHECK(rdna4rtr_ift_init(&empty_t, 0u, 0u) == RDNA4RTR_OK, "empty IFT init");
    CHECK(rdna4rtr_ift_buffer_words(0u) == 1u && rdna4rtr_ift_aligned_buffer_bytes(0u) == 16u,
          "empty IFT buffer sizing");
    uint32_t words0[4] = { 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
    size_t w0 = 0;
    CHECK(rdna4rtr_ift_encode(&empty_t, words0, 4u, &w0) == RDNA4RTR_OK && w0 == 1u && words0[0] == 0u,
          "empty IFT encodes word[0]=0");
    rdna4rtr_ift_dispatch_result_t dr;
    rdna4rtr_ift_simulate_dispatch(words0, w0, 1, 0u, 0u, 0, 0, &dr);
    CHECK(dr.out_of_bounds == 1u && dr.handle_id == 0u && dr.action == RDNA4RTR_DISPATCH_CONFIRM_TRIANGLE,
          "empty IFT triangle candidate -> OOB confirm");
    rdna4rtr_ift_simulate_dispatch(words0, w0, 1, 0u, 0u, 0, 1, &dr);
    CHECK(dr.out_of_bounds == 1u && dr.handle_id == 0u && dr.action == RDNA4RTR_DISPATCH_SKIP_AABB,
          "empty IFT AABB candidate -> OOB safe skip");
    rdna4rtr_ift_destroy(&empty_t);

    // 2. Populated table (6 slots, max_handle_id=3): functions, atomic range, opaque tri, opaque curve, OOB/null.
    rdna4rtr_ift_table_t t;
    CHECK(rdna4rtr_ift_init(&t, 6u, 3u) == RDNA4RTR_OK, "6-slot IFT init");
    CHECK(rdna4rtr_ift_set_function(&t, 0u, 1u, RDNA4RTR_IFT_KIND_BOUNDING_BOX) == RDNA4RTR_OK, "slot 0 -> handle 1");
    CHECK(rdna4rtr_ift_set_function(&t, 1u, 2u, RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT) == RDNA4RTR_OK, "slot 1 -> handle 2");
    CHECK(rdna4rtr_ift_set_function(&t, rt_u32(6u), 1u, RDNA4RTR_IFT_KIND_BOUNDING_BOX) == RDNA4RTR_ERR_BOUNDS, "slot 6 OOB");
    CHECK(rdna4rtr_ift_set_function(&t, 2u, 4u, RDNA4RTR_IFT_KIND_BOUNDING_BOX) == RDNA4RTR_ERR_BOUNDS, "handle 4 > max 3");
    CHECK(rdna4rtr_ift_set_function(&t, 2u, 1u, 9u) == RDNA4RTR_ERR_INVALID, "invalid fn_kind rejected");

    // Atomic range failure must leave slots untouched.
    uint32_t bad_ids[2] = { 3u, 99u };
    uint32_t kinds2[2] = { RDNA4RTR_IFT_KIND_BOUNDING_BOX, RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT };
    CHECK(rdna4rtr_ift_set_functions_range(&t, 2u, 2u, bad_ids, kinds2) == RDNA4RTR_ERR_BOUNDS,
          "range with bad handle fails");
    CHECK(rdna4rtr_ift_get_handle(&t, 2u) == 0u && rdna4rtr_ift_get_mode(&t, 2u) == RDNA4RTR_IFT_SLOT_EMPTY,
          "atomic range failure leaves slot 2 unmodified");

    uint32_t good_ids[2] = { 3u, 0u };
    CHECK(rdna4rtr_ift_set_functions_range(&t, 2u, 2u, good_ids, kinds2) == RDNA4RTR_OK,
          "valid range succeeds");
    CHECK(rdna4rtr_ift_get_handle(&t, 2u) == 3u && rdna4rtr_ift_get_handle(&t, 3u) == 0u,
          "range wrote slot 2=3, slot 3=0");

    // Opaque triangle signature handling (handle_id=0 + mode=OPAQUE_TRIANGLE).
    uint64_t tri_sig = RDNA4RTR_SIG_INSTANCING | RDNA4RTR_SIG_TRIANGLE_DATA | RDNA4RTR_SIG_WORLD_SPACE_DATA;
    CHECK(rdna4rtr_ift_set_opaque_triangle(&t, 4u, tri_sig) == RDNA4RTR_OK, "opaque triangle slot 4 ok");
    CHECK(rdna4rtr_ift_get_handle(&t, 4u) == 0u && rdna4rtr_ift_get_mode(&t, 4u) == RDNA4RTR_IFT_SLOT_OPAQUE_TRIANGLE,
          "opaque triangle encodes handle 0 + OPAQUE_TRIANGLE mode");
    CHECK(rdna4rtr_ift_set_opaque_triangle(&t, 4u, RDNA4RTR_SIG_INSTANCE_MOTION) == RDNA4RTR_ERR_UNSUPPORTED,
          "opaque triangle rejects instance motion signature");
    CHECK(rdna4rtr_ift_set_opaque_triangle(&t, 4u, RDNA4RTR_SIG_PRIMITIVE_MOTION) == RDNA4RTR_ERR_UNSUPPORTED,
          "opaque triangle rejects primitive motion signature");
    CHECK(rdna4rtr_ift_set_opaque_triangle(&t, 4u, RDNA4RTR_SIG_CURVE_DATA) == RDNA4RTR_ERR_INVALID,
          "opaque triangle rejects curve data signature");
    CHECK(rdna4rtr_ift_set_opaque_triangle(&t, 4u, UINT64_C(1) << 20) == RDNA4RTR_ERR_INVALID,
          "opaque triangle rejects unknown signature bit");
    CHECK(rdna4rtr_ift_set_opaque_triangle_range(&t, rt_u32(5u), rt_u32(2u), RDNA4RTR_SIG_NONE) == RDNA4RTR_ERR_BOUNDS,
          "opaque triangle range OOB rejected");

    // Opaque curve: honestly returns UNSUPPORTED while encoding safe handle_id=0.
    CHECK(rdna4rtr_ift_set_function(&t, 5u, 2u, RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT) == RDNA4RTR_OK,
          "pre-populate slot 5 with handle 2");
    CHECK(rdna4rtr_ift_set_opaque_curve(&t, 5u, RDNA4RTR_SIG_CURVE_DATA) == RDNA4RTR_ERR_UNSUPPORTED,
          "opaque curve honestly reports UNSUPPORTED");
    CHECK(rdna4rtr_ift_get_handle(&t, 5u) == 0u && rdna4rtr_ift_get_mode(&t, 5u) == RDNA4RTR_IFT_SLOT_OPAQUE_CURVE,
          "opaque curve clears slot 5 to safe handle 0");
    CHECK(rdna4rtr_ift_set_opaque_curve(&t, rt_u32(99u), RDNA4RTR_SIG_NONE) == RDNA4RTR_ERR_BOUNDS,
          "opaque curve OOB returns BOUNDS");

    // Encode to buffer matching 0077 ABI: [6, 1, 2, 3, 0, 0, 0].
    uint32_t words[8];
    size_t written = 0;
    CHECK(rdna4rtr_ift_encode(&t, words, 6u, &written) == RDNA4RTR_ERR_BOUNDS, "undersized encode buffer rejected");
    CHECK(rdna4rtr_ift_encode(&t, words, 8u, &written) == RDNA4RTR_OK && written == 7u, "IFT encode writes 7 words");
    CHECK(words[0] == 6u && words[1] == 1u && words[2] == 2u && words[3] == 3u &&
          words[4] == 0u && words[5] == 0u && words[6] == 0u,
          "encoded IFT buffer matches [slot_count, handle_ids...]");

    // 0077 dispatch simulation: instancing SBT + geom + fto, non-instancing, negative fto clamp, OOB.
    rdna4rtr_ift_simulate_dispatch(words, written, 1, 1u, 1u, 0, 1, &dr);
    CHECK(dr.computed_slot == 2u && dr.handle_id == 3u && !dr.out_of_bounds && dr.action == RDNA4RTR_DISPATCH_CALL_HANDLE,
          "instancing SBT(1)+geom(1)+fto(0) -> slot 2 handle 3");

    rdna4rtr_ift_simulate_dispatch(words, written, 0, 10u, 1u, 0, 0, &dr);
    CHECK(dr.computed_slot == 1u && dr.handle_id == 2u && dr.action == RDNA4RTR_DISPATCH_CALL_HANDLE,
          "non-instancing ignores SBT offset -> slot 1 handle 2");

    rdna4rtr_ift_simulate_dispatch(words, written, 1, 0u, 0u, -5, 1, &dr);
    CHECK(dr.computed_slot == 0u && dr.handle_id == 1u && dr.action == RDNA4RTR_DISPATCH_CALL_HANDLE,
          "negative function_table_offset clamped to 0");

    rdna4rtr_ift_simulate_dispatch(words, written, 1, 2u, 2u, 0, 0, &dr);
    CHECK(dr.computed_slot == 4u && dr.handle_id == 0u && !dr.out_of_bounds &&
          dr.action == RDNA4RTR_DISPATCH_CONFIRM_TRIANGLE,
          "opaque triangle slot 4 -> CONFIRM_TRIANGLE");

    rdna4rtr_ift_simulate_dispatch(words, written, 1, 3u, 0u, 0, 1, &dr);
    CHECK(dr.computed_slot == 3u && dr.handle_id == 0u && !dr.out_of_bounds &&
          dr.action == RDNA4RTR_DISPATCH_SKIP_AABB,
          "null AABB slot 3 -> SKIP_AABB");

    rdna4rtr_ift_simulate_dispatch(words, written, 1, 5u, 4u, 0, 1, &dr);
    CHECK(dr.computed_slot == 9u && dr.out_of_bounds == 1u && dr.handle_id == 0u &&
          dr.action == RDNA4RTR_DISPATCH_SKIP_AABB,
          "OOB slot 9 -> safe skip on AABB");
    rdna4rtr_ift_destroy(&t);

    // 3. Wave 2 IFT ABI v2: 66-word header with attached_bda[31] + handle_id[] starting at word 66.
    rdna4rtr_ift_table_t t2;
    CHECK(rdna4rtr_ift_init_with_abi(&t2, 4u, 2u, 99u) == RDNA4RTR_ERR_ABI_MISMATCH,
          "invalid ABI version rejected");
    CHECK(rdna4rtr_ift_init_with_abi(&t2, 4u, 2u, RDNA4RTR_IFT_ABI_VERSION_V2) == RDNA4RTR_OK,
          "IFT ABI v2 init");
    CHECK(rdna4rtr_ift_buffer_words_for_abi(4u, RDNA4RTR_IFT_ABI_VERSION_V2) == 70u &&
          rdna4rtr_ift_aligned_buffer_bytes_for_abi(4u, RDNA4RTR_IFT_ABI_VERSION_V2) == 288u,
          "IFT ABI v2 buffer sizing (66+4=70 words -> 288B aligned)");
    const uint64_t bda0 = UINT64_C(0x0000123456789AB0);
    const uint64_t bda2 = UINT64_C(0x0000FEDCBA987650);
    CHECK(rdna4rtr_ift_set_attached_bda(&t2, 0u, bda0) == RDNA4RTR_OK, "attach BDA slot 0");
    CHECK(rdna4rtr_ift_set_attached_bda(&t2, 2u, bda2) == RDNA4RTR_OK, "attach BDA slot 2");
    CHECK(rdna4rtr_ift_set_attached_bda(&t2, 31u, bda0) == RDNA4RTR_ERR_BOUNDS, "attach BDA slot 31 OOB");
    CHECK(rdna4rtr_ift_set_function(&t2, 1u, 2u, RDNA4RTR_IFT_KIND_BOUNDING_BOX) == RDNA4RTR_OK, "v2 slot 1 -> handle 2");
    uint32_t w2[72];
    size_t written2 = 0;
    CHECK(rdna4rtr_ift_encode(&t2, w2, 65u, &written2) == RDNA4RTR_ERR_BOUNDS, "undersized v2 buffer rejected");
    CHECK(rdna4rtr_ift_encode(&t2, w2, 72u, &written2) == RDNA4RTR_OK && written2 == 70u, "v2 encode writes 70 words");
    uint32_t partial[72];
    memcpy(partial, w2, sizeof(uint32_t) * written2);
    partial[67] = 99u;
    CHECK(rdna4rtr_ift_encode_slots(&t2, partial, 70u, 1u, 1u) == RDNA4RTR_OK &&
          !memcmp(partial, w2, sizeof(uint32_t) * written2), "single-slot update matches full v2 encoding");
    CHECK(rdna4rtr_ift_encode_slots(&t2, partial, 67u, 1u, 1u) == RDNA4RTR_ERR_BOUNDS &&
          rdna4rtr_ift_encode_slots(&t2, partial, 70u, 3u, 2u) == RDNA4RTR_ERR_BOUNDS,
          "partial update rejects short backing or invalid slot range");
    CHECK(w2[0] == 4u && w2[1] == 2u && w2[2] == ((1u << 0) | (1u << 2)) && w2[3] == 0u, "v2 header words 0..3 exact");
    uint64_t read_bda = 0u;
    CHECK(rdna4rtr_ift_read_attached_bda(w2, written2, 0u, &read_bda) == RDNA4RTR_OK && read_bda == bda0, "read attached BDA 0");
    CHECK(rdna4rtr_ift_read_attached_bda(w2, written2, 1u, &read_bda) == RDNA4RTR_OK && read_bda == 0u, "read attached BDA 1 == 0");
    CHECK(rdna4rtr_ift_read_attached_bda(w2, written2, 2u, &read_bda) == RDNA4RTR_OK && read_bda == bda2, "read attached BDA 2");
    CHECK(w2[66] == 0u && w2[67] == 2u && w2[68] == 0u && w2[69] == 0u, "v2 handles stored at word[66 + slot]");
    rdna4rtr_ift_simulate_dispatch_abi(w2, written2, RDNA4RTR_IFT_ABI_VERSION_V2, 1, 1u, 0u, 0, 1, &dr);
    CHECK(dr.computed_slot == 1u && dr.handle_id == 2u && !dr.out_of_bounds && dr.action == RDNA4RTR_DISPATCH_CALL_HANDLE,
          "v2 dispatch sim reads handle 2 at word[67]");

    // Apple Sample 1 sphere dispatch: inst.iftOffset(0) + geom.iftOffset(2) -> vkInstanceSBT=2, geomIdx=0 -> IFT slot 2.
    CHECK(rdna4rtr_ift_set_function(&t2, 2u, 1u, RDNA4RTR_IFT_KIND_BOUNDING_BOX) == RDNA4RTR_OK,
          "v2 slot 2 -> sphereIntersectionFunction handle 1");
    CHECK(rdna4rtr_ift_encode(&t2, w2, 72u, &written2) == RDNA4RTR_OK, "re-encode v2 IFT with slot 2");
    rdna4rtr_ift_simulate_dispatch_abi(w2, written2, RDNA4RTR_IFT_ABI_VERSION_V2, 1, 0u + 2u, 0u, 0, 1, &dr);
    CHECK(dr.computed_slot == 2u && dr.handle_id == 1u && !dr.out_of_bounds &&
          dr.action == RDNA4RTR_DISPATCH_CALL_HANDLE,
          "Apple Sample 1 sphere: combined SBT(0+2) + geom(0) -> slot 2 handle 1");
    // Multi-geometry consecutive BLAS: inst(1) + geom_base(1) -> vkInstanceSBT=2, geomIdx=1 -> slot 3 (null AABB -> skip).
    rdna4rtr_ift_simulate_dispatch_abi(w2, written2, RDNA4RTR_IFT_ABI_VERSION_V2, 1, 1u + 1u, 1u, 0, 1, &dr);
    CHECK(dr.computed_slot == 3u && dr.handle_id == 0u && !dr.out_of_bounds &&
          dr.action == RDNA4RTR_DISPATCH_SKIP_AABB,
          "multi-geom consecutive: combined SBT(1+1) + geom(1) -> slot 3");
    rdna4rtr_ift_destroy(&t2);
    // Re-init empty dummy for trailing destroy below.
    (void)rdna4rtr_ift_init(&t, 0u, 0u);

    rdna4rtr_ift_destroy(&t);
}

static void test_as_descriptor_and_bda_planners(void) {
    // Direct descriptor binding plans.
    rdna4rtr_as_desc_plan_t dp;
    const uint64_t fake_vk_as = UINT64_C(0xCAFEBABE12345678);
    CHECK(rdna4rtr_plan_as_descriptor_bind(3u, RDNA4RTR_STAGE_COMPUTE, fake_vk_as,
                                           RDNA4RTR_AS_KIND_INSTANCE, 1, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_OK &&
          dp.dst_set == 0u && dp.dst_binding == 3u && dp.descriptor_type == RDNA4RTR_VK_DESC_TYPE_AS &&
          dp.vk_accel_handle == fake_vk_as,
          "compute direct AS bind -> set 0, binding BUFFER_BINDING_BASE+3");

    CHECK(rdna4rtr_plan_as_descriptor_bind(2u, RDNA4RTR_STAGE_FRAGMENT, fake_vk_as,
                                           RDNA4RTR_AS_KIND_INSTANCE, 1, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_OK &&
          dp.dst_set == 1u && dp.dst_binding == 2u,
          "fragment direct AS bind -> set 1, binding 2");

    CHECK(rdna4rtr_plan_as_descriptor_bind(31u, RDNA4RTR_STAGE_COMPUTE, fake_vk_as,
                                           RDNA4RTR_AS_KIND_INSTANCE, 1, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_ERR_BOUNDS,
          "direct AS bind slot 31 rejected");
    CHECK(rdna4rtr_plan_as_descriptor_bind(0u, RDNA4RTR_STAGE_COMPUTE, 0u,
                                           RDNA4RTR_AS_KIND_INSTANCE, 1, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_ERR_NULL_HANDLE,
          "direct AS bind null handle rejected");
    CHECK(rdna4rtr_plan_as_descriptor_bind(0u, RDNA4RTR_STAGE_COMPUTE, fake_vk_as,
                                           RDNA4RTR_AS_KIND_INSTANCE, 0, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_ERR_UNBUILT,
          "direct AS bind unbuilt rejected");
    CHECK(rdna4rtr_plan_as_descriptor_bind(0u, RDNA4RTR_STAGE_COMPUTE, fake_vk_as,
                                           RDNA4RTR_AS_KIND_PRIMITIVE, 1, RDNA4RTR_AS_KIND_INSTANCE, &dp) == RDNA4RTR_ERR_KIND_MISMATCH,
          "direct AS bind BLAS into TLAS slot rejected");

    // Argument-buffer 64-bit BDA (`u64_device_address`) plans.
    rdna4rtr_arg_bda_plan_t bp;
    const uint64_t bda = UINT64_C(0x0000123456789AB0);
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 16u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_OK &&
          bp.byte_offset == 16u && bp.encoded_u64 == bda && bp.is_null == 0u,
          "argbuf TLAS BDA encode ok");

    uint8_t wire[8];
    rdna4rtr_pack_u64_le(wire, bp.encoded_u64);
    CHECK(wire[0] == 0xB0u && wire[1] == 0x9Au && wire[7] == 0x00u &&
          rdna4rtr_unpack_u64_le(wire) == bda,
          "64-bit BDA little-endian pack/unpack roundtrip");

    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_PRIMITIVE_AS, 0u, 8u, 64u,
                                      1, 0u, RDNA4RTR_AS_KIND_UNKNOWN, &bp) == RDNA4RTR_OK &&
          bp.encoded_u64 == 0u && bp.is_null == 1u,
          "argbuf nil AS into PrimitiveAS slot encodes 0");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 8u, 8u, 64u,
                                      1, 0u, RDNA4RTR_AS_KIND_UNKNOWN, &bp) == RDNA4RTR_OK &&
          bp.byte_offset == 8u && bp.encoded_u64 == 0u && bp.is_null == 1u,
          "argbuf nil AS into InstanceAS slot encodes 0");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 0u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_PRIMITIVE, &bp) == RDNA4RTR_OK &&
          bp.byte_offset == 0u && bp.encoded_u64 == bda && bp.is_null == 0u,
          "argbuf BLAS into InstanceAS (118) slot accepted (Cycles update_bvh blas_array)");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_PRIMITIVE_AS, 8u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_OK &&
          bp.byte_offset == 8u && bp.encoded_u64 == bda && bp.is_null == 0u,
          "argbuf TLAS into PrimitiveAS (117) slot accepted");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_PRIMITIVE_AS, 16u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_PRIMITIVE, &bp) == RDNA4RTR_OK &&
          bp.byte_offset == 16u && bp.encoded_u64 == bda && bp.is_null == 0u,
          "argbuf BLAS into PrimitiveAS (117) slot accepted");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_NONE, 0u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_PRIMITIVE, &bp) == RDNA4RTR_ERR_INVALID,
          "argbuf non-AS (NONE) field kind rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_IFT, 0u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_ERR_INVALID,
          "argbuf non-AS (IFT) field kind rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 0u, 8u, 64u,
                                      0, bda, (rdna4rtr_as_kind_t)99, &bp) == RDNA4RTR_ERR_KIND_MISMATCH,
          "argbuf invalid AS kind enum rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 4u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_ERR_ALIGNMENT,
          "argbuf misaligned offset 4 rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 0u, 4u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_ERR_ABI_MISMATCH,
          "argbuf 4-byte field size rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 64u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_ERR_BOUNDS,
          "argbuf OOB offset rejected");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 0u, 8u, 64u,
                                      0, 0u, RDNA4RTR_AS_KIND_INSTANCE, &bp) == RDNA4RTR_ERR_NULL_HANDLE,
          "argbuf non-nil AS with zero BDA rejected");

    // Argument-buffer IFT BDA encode.
    CHECK(rdna4rtr_plan_ift_bda_encode(RDNA4RTR_ARG_KIND_IFT, 8u, 8u, 64u, 0, bda, &bp) == RDNA4RTR_OK &&
          bp.encoded_u64 == bda, "argbuf IFT BDA encode ok");
    CHECK(rdna4rtr_plan_ift_bda_encode(RDNA4RTR_ARG_KIND_IFT, 8u, 8u, 64u, 1, 0u, &bp) == RDNA4RTR_OK &&
          bp.encoded_u64 == 0u && bp.is_null == 1u, "argbuf nil IFT encodes 0");
    CHECK(rdna4rtr_plan_ift_bda_encode(RDNA4RTR_ARG_KIND_IFT, 8u, 8u, 64u, 0, 0u, &bp) == RDNA4RTR_ERR_NULL_HANDLE,
          "argbuf IFT with zero BDA rejected");
    CHECK(rdna4rtr_plan_ift_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 8u, 8u, 64u, 0, bda, &bp) == RDNA4RTR_ERR_INVALID,
          "argbuf IFT planner rejects non-IFT field kind");
}

static void test_wave4_prim_data_table_and_abi(void) {
    CHECK(GARNETX_CAP_RT_PRIMITIVE_DATA == (1u << 11), "GARNETX_CAP_RT_PRIMITIVE_DATA == (1u << 11)");
    CHECK(GARNETX_CAP_NATIVE_INT64 == (1u << 12) &&
          (GARNETX_CAP_NATIVE_INT64 & (0x3u | GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2 | GARNETX_CAP_RT_PRIMITIVE_DATA)) == 0u,
          "GARNETX_CAP_NATIVE_INT64 == (1u << 12) and disjoint from query/RT cap bits");
    CHECK(RDNA4RT_PRIM_DATA_BINDING == 676u, "prim data binding == 676");
    CHECK(RDNA4RT_PRIM_DATA_STAGE_TABLE_BYTES == 264u && sizeof(rdna4rt_stage_prim_table_t) == 264u,
          "rdna4rt_stage_prim_table_t == 264 bytes");
    CHECK(sizeof(rdna4rt_prim_slot_t) == 32u, "rdna4rt_prim_slot_t == 32 bytes");
    CHECK(sizeof(rdna4rt_geom_prim_entry_t) == 16u, "rdna4rt_geom_prim_entry_t == 16 bytes");
    CHECK(RDNA4RT_PRIM_TABLE_CAP == 4096u && RDNA4RT_PRIM_TABLE_MASK == 4095u &&
          RDNA4RT_PRIM_TABLE_MAX_PROBE == 32u && RDNA4RT_PRIM_TABLE_BYTES == 131072u,
          "prim data table capacity/mask/probe/bytes constants");
    CHECK(RDNA4RT_PRIM_TLAS_ENTRY_STRIDE == 64u && RDNA4RT_PRIM_TLAS_REF_OFFSET == 56u,
          "TLAS instance snapshot stride 64 and ref_offset 56");

    /* Exact hash function parity check. */
    const uint64_t sampleKey = UINT64_C(0x0000123456789A00);
    uint64_t refH = (sampleKey >> 8) * UINT64_C(0x61C8864680B583EB);
    refH ^= (refH >> 32);
    CHECK(rdna4rt_prim_hash_slot(sampleKey, RDNA4RT_PRIM_TABLE_MASK) == ((uint32_t)refH & RDNA4RT_PRIM_TABLE_MASK),
          "rdna4rt_prim_hash_slot matches exact shared hash formula");

    /* Allocate 4096-slot table on heap (ASan/UBSan verified). */
    rdna4rt_prim_slot_t *tbl = (rdna4rt_prim_slot_t *)calloc(RDNA4RT_PRIM_TABLE_CAP, sizeof(*tbl));
    CHECK(tbl != NULL, "allocate 4096-slot RDNA4RTPrimDataTable");
    if (!tbl) return;

    /* Invalid sentinel keys rejected. */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    RDNA4RT_PRIM_SLOT_EMPTY, 0x1000u, 1u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS) == RDNA4RTR_ERR_INVALID,
          "upsert rejects EMPTY sentinel key 0");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    RDNA4RT_PRIM_SLOT_TOMBSTONE, 0x1000u, 1u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS) == RDNA4RTR_ERR_INVALID,
          "upsert rejects TOMBSTONE sentinel key 1");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, 0u, NULL) == 0,
          "lookup rejects key 0");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, 1u, NULL) == 0,
          "lookup rejects key 1");

    /* Basic insert, lookup, update in-place for BLAS & TLAS. */
    const uint64_t blasBda = UINT64_C(0x10000100);
    const uint64_t tlasBda = UINT64_C(0x20000200);
    const uint32_t blasBaseSlot = rdna4rt_prim_hash_slot(blasBda, RDNA4RT_PRIM_TABLE_MASK);
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    blasBda, UINT64_C(0x30000000), 3u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS) == RDNA4RTR_OK,
          "insert BLAS into prim table");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    tlasBda, UINT64_C(0x40000000), 5u, 64u, 56u, RDNA4RT_PRIM_KIND_TLAS) == RDNA4RTR_OK,
          "insert TLAS into prim table");

    rdna4rt_prim_slot_t got;
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, blasBda, &got) == 1 &&
          got.as_bda == blasBda && got.payload_bda == UINT64_C(0x30000000) && got.count == 3u && got.kind == RDNA4RT_PRIM_KIND_BLAS,
          "lookup BLAS matches inserted fields");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, tlasBda, &got) == 1 &&
          got.as_bda == tlasBda && got.payload_bda == UINT64_C(0x40000000) && got.count == 5u &&
          got.entry_stride == 64u && got.ref_offset == 56u && got.kind == RDNA4RT_PRIM_KIND_TLAS,
          "lookup TLAS matches inserted fields");

    /* Update BLAS in-place keeps same slot. */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    blasBda, UINT64_C(0x30001000), 4u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS) == RDNA4RTR_OK &&
          tbl[blasBaseSlot].as_bda == blasBda && tbl[blasBaseSlot].payload_bda == UINT64_C(0x30001000),
          "update existing BLAS key reuses exact slot");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, blasBda, &got) == 1 &&
          got.payload_bda == UINT64_C(0x30001000) && got.count == 4u,
          "updated BLAS payload/count visible");

    /* Find 34 distinct 256B-aligned keys that collide on the same base slot. */
    memset(tbl, 0, RDNA4RT_PRIM_TABLE_CAP * sizeof(*tbl));
    uint64_t coll[34];
    uint32_t foundColl = 0u;
    const uint32_t targetSlot = 42u;
    for (uint64_t k = UINT64_C(0x100000); foundColl < 34u && k < UINT64_C(0x100000000); k += 256u) {
        if (rdna4rt_prim_hash_slot(k, RDNA4RT_PRIM_TABLE_MASK) == targetSlot)
            coll[foundColl++] = k;
    }
    CHECK(foundColl == 34u, "found 34 colliding 256B-aligned AS BDAs on same base slot");

    /* Collision chain + tombstone + probe-past-tombstone + in-place update + tombstone reuse. */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[0], 0xA000u, 1u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[targetSlot].as_bda == coll[0],
          "coll[0] lands at base slot");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[1], 0xB000u, 2u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[(targetSlot + 1u) & RDNA4RT_PRIM_TABLE_MASK].as_bda == coll[1],
          "coll[1] probes to base+1");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[2], 0xC000u, 3u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[(targetSlot + 2u) & RDNA4RT_PRIM_TABLE_MASK].as_bda == coll[2],
          "coll[2] probes to base+2");

    /* Delete coll[0] -> tombstone at base slot; coll[1] & coll[2] still found. */
    CHECK(rdna4rt_prim_table_remove(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[0]) == 1 &&
          tbl[targetSlot].as_bda == RDNA4RT_PRIM_SLOT_TOMBSTONE,
          "deleting coll[0] writes tombstone sentinel 1");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[0], NULL) == 0,
          "deleted coll[0] is not found");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[1], &got) == 1 &&
          got.payload_bda == 0xB000u,
          "coll[1] found past tombstone");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[2], &got) == 1 &&
          got.payload_bda == 0xC000u,
          "coll[2] found past tombstone");

    /* Re-inserting coll[2] MUST update base+2 in place (no duplicate at tombstone base slot!). */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[2], 0xC999u, 9u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[(targetSlot + 2u) & RDNA4RT_PRIM_TABLE_MASK].as_bda == coll[2] &&
          tbl[(targetSlot + 2u) & RDNA4RT_PRIM_TABLE_MASK].payload_bda == 0xC999u &&
          tbl[targetSlot].as_bda == RDNA4RT_PRIM_SLOT_TOMBSTONE,
          "re-upsert coll[2] updates existing slot past tombstone without duplicating");

    /* Inserting brand-new coll[3] reuses the tombstone at base slot. */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[3], 0xD000u, 4u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[targetSlot].as_bda == coll[3],
          "new key coll[3] reuses first tombstone slot");

    /* Placement-aliasing conditional remove_if_match prevents stale AS1 dealloc from clobbering AS2. */
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[1], 0xB222u, 2u, 0u, 0u, 0u) == RDNA4RTR_OK,
          "aliasing AS2 overwrites coll[1] payload");
    CHECK(rdna4rt_prim_table_remove_if_match(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[1], 0xB000u) == 0,
          "stale AS1 dealloc with old payload_bda does not tombstone live AS2");
    CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[1], &got) == 1 &&
          got.payload_bda == 0xB222u,
          "AS2 entry survives stale AS1 remove_if_match");
    CHECK(rdna4rt_prim_table_remove_if_match(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[1], 0xB222u) == 1,
          "matching AS2 remove_if_match tombstones slot");

    /* Full 32-probe collision saturation & recovery via tombstone reuse. */
    memset(tbl, 0, RDNA4RT_PRIM_TABLE_CAP * sizeof(*tbl));
    int all32 = 1;
    for (uint32_t i = 0; i < RDNA4RT_PRIM_TABLE_MAX_PROBE; i++) {
        if (rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                      coll[i], 0x50000u + (uint64_t)i * 16u, i + 1u, 0u, 0u, 0u) != RDNA4RTR_OK)
            all32 = 0;
    }
    CHECK(all32, "32 colliding keys fill entire probe window");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[32], 0x99999u, 1u, 0u, 0u, 0u) == RDNA4RTR_ERR_BOUNDS,
          "33rd colliding key exceeds MAX_PROBE=32 and returns BOUNDS");
    CHECK(rdna4rt_prim_table_remove(tbl, RDNA4RT_PRIM_TABLE_CAP, coll[15]) == 1,
          "delete middle colliding key coll[15]");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    coll[32], 0x99999u, 1u, 0u, 0u, 0u) == RDNA4RTR_OK &&
          tbl[(targetSlot + 15u) & RDNA4RT_PRIM_TABLE_MASK].as_bda == coll[32],
          "coll[32] succeeds after tombstone frees slot in probe window");

    /* End-to-end 2-level shader lookup simulation: stage_table -> TLAS -> BLAS -> geom -> prim. */
    memset(tbl, 0, RDNA4RT_PRIM_TABLE_CAP * sizeof(*tbl));
    struct { uint32_t id; float weight; } primPayload[2] = { { 101u, 1.5f }, { 202u, 2.75f } };
    rdna4rt_geom_prim_entry_t blasGeomTable[2] = {
        { 0u, 0u, 0u },
        { (uint64_t)(uintptr_t)primPayload, (uint32_t)sizeof(primPayload[0]), 2u }
    };
    uint8_t tlasInstBuf[2 * 64];
    memset(tlasInstBuf, 0, sizeof(tlasInstBuf));
    const uint64_t simBlasBda = UINT64_C(0x55550000);
    const uint64_t simTlasBda = UINT64_C(0x66660000);
    memcpy(tlasInstBuf + 1u * RDNA4RT_PRIM_TLAS_ENTRY_STRIDE + RDNA4RT_PRIM_TLAS_REF_OFFSET,
           &simBlasBda, sizeof(simBlasBda));

    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    simBlasBda, (uint64_t)(uintptr_t)blasGeomTable, 2u, 0u, 0u,
                                    RDNA4RT_PRIM_KIND_BLAS) == RDNA4RTR_OK,
          "2-level sim: register BLAS");
    CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                    simTlasBda, (uint64_t)(uintptr_t)tlasInstBuf, 2u,
                                    RDNA4RT_PRIM_TLAS_ENTRY_STRIDE, RDNA4RT_PRIM_TLAS_REF_OFFSET,
                                    RDNA4RT_PRIM_KIND_TLAS) == RDNA4RTR_OK,
          "2-level sim: register TLAS");

    rdna4rt_stage_prim_table_t stageTbl;
    rdna4rt_stage_prim_table_init(&stageTbl, (uint64_t)(uintptr_t)tbl, RDNA4RT_PRIM_TABLE_MASK);
    stageTbl.slot_as_bda[0] = simTlasBda;

    /* Simulate shader: slot 0 -> TLAS -> instance 1 -> BLAS -> geom 1 -> prim 1. */
    rdna4rt_prim_slot_t tlasSlot, blasSlot;
    CHECK(rdna4rt_prim_table_lookup((const rdna4rt_prim_slot_t *)(uintptr_t)stageTbl.global_table_bda,
                                    (uint32_t)stageTbl.table_mask + 1u,
                                    stageTbl.slot_as_bda[0], &tlasSlot) == 1 &&
          tlasSlot.kind == RDNA4RT_PRIM_KIND_TLAS && 1u < tlasSlot.count,
          "2-level sim: stage slot 0 resolves TLAS");
    uint64_t resolvedBlasBda = 0u;
    memcpy(&resolvedBlasBda,
           (const uint8_t *)(uintptr_t)(tlasSlot.payload_bda + 1u * (uint64_t)tlasSlot.entry_stride + tlasSlot.ref_offset),
           sizeof(resolvedBlasBda));
    CHECK(resolvedBlasBda == simBlasBda, "2-level sim: TLAS instance 1 resolves BLAS BDA");
    CHECK(rdna4rt_prim_table_lookup((const rdna4rt_prim_slot_t *)(uintptr_t)stageTbl.global_table_bda,
                                    (uint32_t)stageTbl.table_mask + 1u,
                                    resolvedBlasBda, &blasSlot) == 1 &&
          blasSlot.kind == RDNA4RT_PRIM_KIND_BLAS && 1u < blasSlot.count,
          "2-level sim: BLAS BDA resolves geom header table");
    const rdna4rt_geom_prim_entry_t *ge =
        ((const rdna4rt_geom_prim_entry_t *)(uintptr_t)blasSlot.payload_bda) + 1u;
    CHECK(ge->data_bda != 0u && 1u < ge->prim_count, "2-level sim: geom 1 entry valid");
    const uint8_t *primPtr = (const uint8_t *)(uintptr_t)(ge->data_bda + 1u * (uint64_t)ge->stride);
    uint32_t readId = 0u;
    float readWeight = 0.0f;
    memcpy(&readId, primPtr, sizeof(readId));
    memcpy(&readWeight, primPtr + 4u, sizeof(readWeight));
    CHECK(readId == 202u && readWeight == 2.75f, "2-level sim: reads exact per-primitive payload");

    /* Shadow + GPU-timeline synchronization simulation (pipelined refit, abandoned CB, encode-time clear). */
    rdna4rt_prim_slot_t *shadow = (rdna4rt_prim_slot_t *)calloc(RDNA4RT_PRIM_TABLE_CAP, sizeof(*shadow));
    rdna4rt_prim_slot_t *gpuTbl = (rdna4rt_prim_slot_t *)calloc(RDNA4RT_PRIM_TABLE_CAP, sizeof(*gpuTbl));
    CHECK(shadow != NULL && gpuTbl != NULL, "allocate shadow and GPU timeline tables");
    if (shadow && gpuTbl) {
        /* 1. Encode build(V1) -> shadow[S] transitions EMPTY->occupied (wasEmpty==1 -> CPU marks gpu[S]=TOMBSTONE). */
        uint32_t idxV1 = 0u, idxV2 = 0u, idxK1 = 0u, idxClr = 0u;
        int wasEmptyV1 = 0, wasEmptyV2 = 0, wasEmptyK1 = 0;
        CHECK(rdna4rt_prim_table_upsert_slot(shadow, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[0], 0x11110000u, 1u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS,
                                             &idxV1, &wasEmptyV1) == RDNA4RTR_OK &&
              idxV1 == targetSlot && wasEmptyV1 == 1,
              "shadow build(V1) reports slot_idx and was_empty==1");
        if (wasEmptyV1) gpuTbl[idxV1].as_bda = RDNA4RT_PRIM_SLOT_TOMBSTONE;

        /* 2. Simulate GPU executing build(V1) 32B vkCmdUpdateBuffer. */
        gpuTbl[idxV1] = shadow[idxV1];

        /* 3. Encode refit_inplace(V2) while trace1 is STILL in flight on GPU. */
        CHECK(rdna4rt_prim_table_upsert_slot(shadow, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[0], 0x22220000u, 1u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS,
                                             &idxV2, &wasEmptyV2) == RDNA4RTR_OK &&
              idxV2 == idxV1 && wasEmptyV2 == 0,
              "shadow refit_inplace(V2) reuses exact slot with was_empty==0 (does not touch live GPU slot)");
        /* trace1 executes on GPU BEFORE refit_inplace(V2) GPU update: MUST still see V1! */
        CHECK(rdna4rt_prim_table_lookup(gpuTbl, RDNA4RT_PRIM_TABLE_CAP, coll[0], &got) == 1 &&
              got.payload_bda == 0x11110000u,
              "in-flight trace1 still sees V1 on GPU while refit(V2) is only encoded in shadow");
        /* GPU executes refit_inplace(V2) 32B update -> trace2 sees V2! */
        gpuTbl[idxV2] = shadow[idxV2];
        CHECK(rdna4rt_prim_table_lookup(gpuTbl, RDNA4RT_PRIM_TABLE_CAP, coll[0], &got) == 1 &&
              got.payload_bda == 0x22220000u,
              "trace2 sees V2 after GPU executes refit_inplace(V2) update");

        /* 4. Encode clear (rebuild without primData) while trace2 is still in flight on GPU. */
        CHECK(rdna4rt_prim_table_remove_if_match_slot(shadow, RDNA4RT_PRIM_TABLE_CAP,
                                                      coll[0], 0x22220000u, &idxClr) == 1 &&
              idxClr == idxV2,
              "shadow encode-time clear returns tombstoned slot index");
        /* Prior trace2 on GPU STILL sees V2 before the 8B GPU tombstone runs! */
        CHECK(rdna4rt_prim_table_lookup(gpuTbl, RDNA4RT_PRIM_TABLE_CAP, coll[0], &got) == 1 &&
              got.payload_bda == 0x22220000u,
              "in-flight trace2 survives encode-time shadow clear until GPU 8B tombstone runs");
        /* GPU executes 8B TOMBSTONE update -> subsequent trace sees NOT FOUND. */
        gpuTbl[idxClr].as_bda = RDNA4RT_PRIM_SLOT_TOMBSTONE;
        CHECK(rdna4rt_prim_table_lookup(gpuTbl, RDNA4RT_PRIM_TABLE_CAP, coll[0], NULL) == 0,
              "post-clear trace sees NOT FOUND after GPU tombstone executes");

        /* 5. Abandoned CB collision-chain preservation: CB_A encodes coll[4] into EMPTY slot,
         * marks gpu[slot]=TOMBSTONE on CPU, and is abandoned; CB_B encodes & runs colliding coll[5]. */
        memset(shadow, 0, RDNA4RT_PRIM_TABLE_CAP * sizeof(*shadow));
        memset(gpuTbl, 0, RDNA4RT_PRIM_TABLE_CAP * sizeof(*gpuTbl));
        CHECK(rdna4rt_prim_table_upsert_slot(shadow, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[4], 0x44440000u, 1u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS,
                                             &idxV1, &wasEmptyV1) == RDNA4RTR_OK && wasEmptyV1 == 1,
              "abandoned CB_A allocates base slot (was_empty==1)");
        gpuTbl[idxV1].as_bda = RDNA4RT_PRIM_SLOT_TOMBSTONE; /* CB_A abandoned: 32B GPU write never runs */
        CHECK(rdna4rt_prim_table_upsert_slot(shadow, RDNA4RT_PRIM_TABLE_CAP,
                                             coll[5], 0x55550000u, 2u, 0u, 0u, RDNA4RT_PRIM_KIND_BLAS,
                                             &idxK1, &wasEmptyK1) == RDNA4RTR_OK &&
              idxK1 == ((targetSlot + 1u) & RDNA4RT_PRIM_TABLE_MASK) && wasEmptyK1 == 1,
              "CB_B places colliding coll[5] at base+1");
        gpuTbl[idxK1] = shadow[idxK1]; /* CB_B runs on GPU */
        CHECK(rdna4rt_prim_table_lookup(gpuTbl, RDNA4RT_PRIM_TABLE_CAP, coll[5], &got) == 1 &&
              got.payload_bda == 0x55550000u,
              "GPU probes past abandoned CB_A tombstone and resolves CB_B colliding key coll[5]");
    }
    free(shadow);
    free(gpuTbl);

    free(tbl);
}

static char *read_source_file(const char *rel_a, const char *rel_b) {
    FILE *f = fopen(rel_a, "rb");
    if (!f && rel_b) f = fopen(rel_b, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static char *read_rt_sources(void) {
    static const char *const kRelA[] = {
        "src/garnet-metal/garnet_rt_runtime.m",
        "src/garnet-metal/garnet_rt_accel.m",
        "src/garnet-metal/garnet_rt_plan.m",
        "src/garnet-metal/garnet_rt_encoder.m",
    };
    static const char *const kRelB[] = {
        "garnet_rt_runtime.m",
        "garnet_rt_accel.m",
        "garnet_rt_plan.m",
        "garnet_rt_encoder.m",
    };
    char *parts[4] = { NULL, NULL, NULL, NULL };
    size_t lens[4] = { 0, 0, 0, 0 };
    size_t total = 1u;
    for (size_t i = 0; i < 4u; i++) {
        parts[i] = read_source_file(kRelA[i], kRelB[i]);
        if (!parts[i]) {
            for (size_t j = 0; j < i; j++) free(parts[j]);
            return NULL;
        }
        lens[i] = strlen(parts[i]);
        total += lens[i] + 1u;
    }
    char *joined = (char *)malloc(total);
    if (!joined) {
        for (size_t j = 0; j < 4u; j++) free(parts[j]);
        return NULL;
    }
    size_t pos = 0u;
    for (size_t i = 0; i < 4u; i++) {
        memcpy(joined + pos, parts[i], lens[i]);
        pos += lens[i];
        joined[pos++] = '\n';
        free(parts[i]);
    }
    joined[pos] = '\0';
    return joined;
}

static void test_source_pins(void) {
    char *impl = read_rt_sources();
    CHECK(impl != NULL, "read split garnet_rt_{runtime,accel,plan,encoder}.m for source pins");
    if (impl) {
        CHECK(strstr(impl, "RDNA4IntersectionFunctionTable") != NULL, "impl defines RDNA4IntersectionFunctionTable");
        CHECK(strstr(impl, "rdna4VkAccelerationStructure") != NULL, "impl uses real rdna4VkAccelerationStructure selector");
        CHECK(strstr(impl, "rdna4VkHandle") == NULL, "impl never references nonexistent rdna4VkHandle selector");
        CHECK(strstr(impl, "RDNA4_RESOURCE_SPI") != NULL, "impl uses RDNA4_RESOURCE_SPI for gpuResourceID");
        CHECK(strstr(impl, "- (MTLResourceID)gpuResourceID") == NULL, "impl has no duplicate gpuResourceID definition");
        CHECK(strstr(impl, "VkWriteDescriptorSetAccelerationStructureKHR") != NULL, "impl fills VkWriteDescriptorSetAccelerationStructureKHR");
        CHECK(strstr(impl, "setOpaqueTriangleIntersectionFunctionWithSignature:") != NULL, "impl handles opaque triangle IFT");
        CHECK(strstr(impl, "setOpaqueCurveIntersectionFunctionWithSignature:") != NULL, "impl handles opaque curve IFT");
        CHECK(strstr(impl, "rdna4_rt_arg_resolve_as_address") != NULL, "impl provides BDA AS argument-buffer helper");
        CHECK(strstr(impl, "rdna4_rt_runtime_supports_primitive_motion_blur") != NULL, "impl provides honest motion blur query");
        CHECK(strstr(impl, "garnetx_translate_stage3") != NULL, "impl resolves garnetx_translate_stage3");
        CHECK(strstr(impl, "MTLFunctionTypeIntersection") != NULL, "impl validates MTLFunctionTypeIntersection handles");
        CHECK(strstr(impl, "rdna4_rt_parse_meta(\n    id meta,") != NULL, "rdna4_rt_parse_meta accepts id (NSDictionary or NSData)");
        CHECK(strstr(impl, "isKindOfClass:[NSDictionary class]") != NULL &&
              strstr(impl, "NSJSONSerialization") != NULL,
              "rdna4_rt_parse_meta handles NSDictionary pipeline metadata");
        CHECK(strstr(impl, "rdna4rt_blas_build_flags_with_caps") != NULL,
              "BLAS build plan applies position_fetch ALLOW_DATA_ACCESS via helper");
        CHECK(strstr(impl, "RDNA4RTPrimDataTable") != NULL &&
              strstr(impl, "GarnetASPrimData") != NULL &&
              strstr(impl, "rdna4_rt_fill_stage_prim_table") != NULL &&
              strstr(impl, "rdna4rt_blas_prim_rebase_headers") != NULL &&
              strstr(impl, "rdna4_rt_prim_table_register_cmd") != NULL &&
              strstr(impl, "rdna4_rt_prim_table_clear_cmd") != NULL &&
              strstr(impl, "rdna4rt_prim_table_upsert_slot") != NULL &&
              strstr(impl, "rdna4rt_prim_table_remove_if_match_slot") != NULL &&
              strstr(impl, "VK_BUFFER_USAGE_TRANSFER_DST_BIT") != NULL &&
              strstr(impl, "rdna4_rt_prim_pool_pop") != NULL &&
              strstr(impl, "rdna4_rt_prim_pool_push") != NULL &&
              strstr(impl, "rdna4_rt_qp_pool_pop") != NULL &&
              strstr(impl, "rdna4_rt_qp_pool_push") != NULL,
              "Wave 4.1B GPU-timeline prim-data table, pooled snapshot/query-pool, and copy/compact rebase wired");
        CHECK(strstr(impl, "per-primitive data buffers are not supported") == NULL &&
              strstr(impl, "per-primitive-only refit without vertex data is unsupported") == NULL,
              "Wave 4.1B removes legacy primitiveDataBuffer and PerPrimitiveData-only refit rejections");
        CHECK(strstr(impl, "rdna4_rt_device_heap_as_size_align_with_size") != NULL &&
              strstr(impl, "rdna4_rt_heap_new_as_with_size") != NULL &&
              strstr(impl, "rdna4_rt_heap_new_as_with_descriptor") != NULL &&
              strstr(impl, "RDNA4_HEAP_RETIRE") != NULL,
              "Wave 4.2 MTLHeap placement/automatic AS helpers and lifecycle retirement wired");
        CHECK(strstr(impl, "RDNA4R.ok ? (rdna4dc_metal3_raytracing_supported(&m3) ? 1u : 0u) : 1u") != NULL,
              "pre-open supportsRaytracing reports RDNA4-guaranteed RT hw caps with radv_open=0");
        CHECK(strstr(impl, "intersectionFunctionTableOffset") != NULL &&
              strstr(impl, "rdna4rt_geom_ift_acc_add") != NULL &&
              strstr(impl, "rdna4rt_convert_instances_ex") != NULL &&
              strstr(impl, "rdna4rt_state_apply_build_ex") != NULL &&
              strstr(impl, "rdna4rt_state_apply_refit_ex") != NULL &&
              strstr(impl, "rdna4rt_blas_table_entry_t") != NULL &&
              strstr(impl, "rdna4GeomIftBase") != NULL,
              "Wave 5 BLAS geometry intersectionFunctionTableOffset capture, propagation, and CPU+GPU TLAS combine wired");
        CHECK(strstr(impl, "GARNET_RT_MOTION") != NULL &&
              strstr(impl, "rdna4_rt_motion_enabled") != NULL &&
              strstr(impl, "rdna4_rt_ensure_motion_pipes") != NULL &&
              strstr(impl, "rdna4_rt_ensure_unit_box_blas") != NULL &&
              strstr(impl, "rdna4_rt_prepare_motion_tlas_sidecar") != NULL &&
              strstr(impl, "RDNA4RT_PRIM_KIND_TLAS_MOTION") != NULL &&
              strstr(impl, "MTLAccelerationStructureMotionTriangleGeometryDescriptor") != NULL &&
              strstr(impl, "MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor") != NULL,
              "Wave 3.2 runtime motion gate, pipelines, unit-box BLAS, motion BLAS/TLAS builders, and sidecars wired");
        free(impl);
    }

    char *meshImpl = read_source_file("src/garnet-metal/garnet_mesh.m", "garnet_mesh.m");
    CHECK(meshImpl != NULL, "read garnet_mesh.m for R6 source pin");
    if (meshImpl) {
        CHECK(strstr(meshImpl, "if (RDNA4R.shaderInt64) work.caps.flags |= GARNETX_CAP_NATIVE_INT64;") != NULL,
              "R6: mesh/object Stage 3 worker sets GARNETX_CAP_NATIVE_INT64 when RDNA4R.shaderInt64 is enabled");
        free(meshImpl);
    }

    char *shd = read_source_file("src/garnet-metal/garnet_shaders.m", "garnet_shaders.m");
    char *spv = read_source_file("src/garnet-metal/garnet_spvcache.m", "garnet_spvcache.m");
    CHECK(shd != NULL && spv != NULL, "read garnet_shaders.m and garnet_spvcache.m for Wave 4.1B pins");
    if (shd && spv) {
        CHECK(strstr(shd, "GARNETX_CAP_RT_PRIMITIVE_DATA") != NULL &&
              strstr(spv, "rtPrimDataTable") != NULL &&
              strstr(spv, "RTPrimDataTable") != NULL,
              "garnet_shaders/garnet_spvcache wire GARNETX_CAP_RT_PRIMITIVE_DATA and binding 676 RTPrimDataTable");
        CHECK(strstr(shd, "const BOOL unlimited = (deadlineNs == UINT64_MAX && !rdna4_is_ws());") != NULL &&
              strstr(shd, "waitMs = -1;") != NULL,
              "R4: rdna4x_translate_fn_with_error honors UINT64_MAX sentinel -> waitMs=-1 for non-WS");
        CHECK(strstr(shd, "if (RDNA4R.shaderInt64)\n        caps.flags |= GARNETX_CAP_NATIVE_INT64;") != NULL,
              "R6: rdna4_rt_xlate_caps sets GARNETX_CAP_NATIVE_INT64 when RDNA4R.shaderInt64 is enabled");
        CHECK(strstr(shd, "GARNETX_CAP_RT_MOTION_BLUR") != NULL,
              "Wave 3.2: garnet_shaders wires GARNETX_CAP_RT_MOTION_BLUR");
    }
    free(shd);
    free(spv);

    char *devm = read_source_file("src/garnet-metal/garnet_device.m", "garnet_device.m");
    CHECK(devm != NULL, "read garnet_device.m for Wave 4.2 pins");
    if (devm) {
        CHECK(strstr(devm, "heapAccelerationStructureSizeAndAlignWithSize:") != NULL &&
              strstr(devm, "heapAccelerationStructureSizeAndAlignWithDescriptor:") != NULL,
              "GarnetDevice wires heapAccelerationStructureSizeAndAlignWithSize/Descriptor");
        CHECK(strstr(devm, "const uint64_t computeDeadline = rdna4_is_ws()") != NULL &&
              strstr(devm, "rdna4x_translate_fn_with_error(fn, \"kernel\", computeDeadline, &inprocFailure)") != NULL,
              "R4: compute pipeline translation passes UINT64_MAX sentinel for non-WS");
        free(devm);
    }

    char *cmds = read_source_file("src/garnet-metal/garnet_command_buffer.m", "garnet_command_buffer.m");
    CHECK(cmds != NULL, "read garnet_command_buffer.m for useResource RT pins");
    if (cmds) {
        const char *ur = strstr(cmds, "- (void)rdna4UseResource:(id)resource usage:(NSUInteger)usage");
        CHECK(ur != NULL, "found rdna4UseResource:usage:");
        if (ur) {
            char seg[1600];
            size_t slen = strlen(ur);
            if (slen >= sizeof(seg)) slen = sizeof(seg) - 1u;
            memcpy(seg, ur, slen);
            seg[slen] = '\0';
            CHECK(strstr(seg, "RDNA4IntersectionFunctionTable") != NULL,
                  "rdna4UseResource accepts RDNA4IntersectionFunctionTable");
            CHECK(strstr(seg, "RDNA4VisibleFunctionTable") != NULL,
                  "rdna4UseResource accepts RDNA4VisibleFunctionTable");
            CHECK(strstr(seg, "rdna4CurrentPrimData") != NULL,
                  "rdna4UseResource retains GarnetASPrimData");
        }
        CHECK(strstr(cmds, "rdna4_rt_fill_stage_prim_table") != NULL &&
              strstr(cmds, "p->rtPrimDataTable") != NULL,
              "garnet_commands fills binding 676 stage primitive data table");
        free(cmds);
    }

    char *be = read_source_file("src/garnet-metal/garnet_backend.m", "garnet_backend.m");
    CHECK(be != NULL, "read garnet_backend.m for position_fetch pin");
    if (be) {
        CHECK(strstr(be, "VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME") != NULL,
              "garnet_backend probes/enables VK_KHR_ray_tracing_position_fetch");
        CHECK(strstr(be, "RDNA4R.shaderInt64 = (ef.shaderInt64 != VK_FALSE) ? YES : NO;") != NULL &&
              strstr(be, "GARNETX_CAP_NATIVE_INT64") == NULL,
              "R6: garnet_backend publishes RDNA4R.shaderInt64 without polluting RDNA4QueryCaps");
        free(be);
    }

    char *args = read_source_file("src/garnet-metal/garnet_arguments.m", "garnet_arguments.m");
    char *argenc = read_source_file("src/garnet-metal/garnet_argument_encoder.m", "garnet_argument_encoder.m");
    CHECK(args != NULL && argenc != NULL, "read garnet_arguments.m and garnet_argument_encoder.m for IFT/VFT backing BDA registry pin");
    if (args && argenc) {
        CHECK(strstr(args, "[object isKindOfClass:[RDNA4IntersectionFunctionTable class]]") != NULL &&
              strstr(args, "rdna4BackingBuffer") != NULL &&
              strstr(args, "rdna4AttachedBufferAtIndex:") != NULL &&
              strstr(argenc, "bindlessQueried") != NULL,
              "rdna4PrepareArguments registers IFT attached buffers and honors bindlessQueried samplers");
        CHECK(strstr(argenc, "rdna4_argument_descriptor_effective_count(argument.arrayLength)") != NULL &&
              strstr(argenc, "|| !argument.arrayLength") == NULL,
              "RDNA4ArgumentLayout initWithArguments normalizes arrayLength == 0 to 1");
        CHECK(strstr(argenc, "field.type != MTLDataTypeInstanceAccelerationStructure && field.type != MTLDataTypePrimitiveAccelerationStructure") != NULL,
              "RDNA4ArgumentEncoder setAccelerationStructure accepts both InstanceAS (118) and PrimitiveAS (117) slots");
        CHECK(strstr(args, "const BOOL unlimited = (deadline == UINT64_MAX && !rdna4_is_ws());") != NULL &&
              strstr(args, "waitMs = -1;") != NULL,
              "R4: rdna4_gpu_argument_spv honors UINT64_MAX sentinel -> waitMs=-1 for non-WS");
    }
    free(args);
    free(argenc);

    char *xl = read_source_file("src/garnet-metal/garnet_xlate.h", "garnet_xlate.h");
    CHECK(xl != NULL, "read garnet_xlate.h for 64 MiB SPIR-V cap pin");
    if (xl) {
        CHECK(strstr(xl, "#define RDNA4X_MAX_SPV       (64u * 1024u * 1024u)") != NULL,
              "R3: RDNA4X_MAX_SPV bumped to 64 MiB for Cycles uber-shaders");
        CHECK(strstr(xl, "#define RDNA4X_CACHE_CAP     (512ull * 1024ull * 1024ull)") != NULL,
              "R4: RDNA4X_CACHE_CAP bumped to 512 MiB for 66 Cycles shaders + lowered copies");
        CHECK(strstr(xl, "if (waitMs < 0) {\n        while (!j->done) pthread_cond_wait(&e->cvDone, &e->mu);") != NULL,
              "R4: rdna4x_engine_submit waits synchronously on pthread_cond_wait when waitMs < 0");
        free(xl);
    }

    char *pipes = read_source_file("src/garnet-metal/garnet_compute_pipelines.m", "garnet_compute_pipelines.m");
    CHECK(pipes != NULL, "read garnet_compute_pipelines.m for R4 compute arg-lowering deadline pin");
    if (pipes) {
        CHECK(strstr(pipes, "const uint64_t deadline = rdna4_is_ws()\n        ? (rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_WS_MS * 1000000ull)\n        : UINT64_MAX;") != NULL,
              "R4: compute pipeline passes UINT64_MAX deadline to rdna4_gpu_argument_spv for non-WS");
        CHECK(strstr(pipes, "#define RDNA4_HEAVY_COMPUTE_SPV_BYTES (1u << 20)") != NULL &&
              strstr(pipes, "static pthread_mutex_t rdna4_heavy_compute_pipe_mu = PTHREAD_MUTEX_INITIALIZER;") != NULL &&
              strstr(pipes, "rdna4_create_compute_pipeline_guarded(RDNA4R.dev, pc, &ci, spvBytes, &p)") != NULL &&
              strstr(pipes, "rdna4_create_compute_pipeline_guarded(RDNA4R.dev, pc, &ci, count * 4u, &p)") != NULL,
              "R7: garnet_pipelines serializes >=1 MiB compute vkCreateComputePipelines via leaf mutex");
        free(pipes);
    }

    char *ba = read_source_file("src/garnet-metal/garnet_binary_archive.h", "garnet_binary_archive.h");
    CHECK(ba != NULL, "read garnet_binary_archive.h for 80 MiB entry payload cap pin");
    if (ba) {
        CHECK(strstr(ba, "#define RDNA4BA_MAX_ENTRY_PAYLOAD (UINT64_C(80) << 20)") != NULL,
              "R3: RDNA4BA_MAX_ENTRY_PAYLOAD bumped to 80 MiB for 64 MiB SPIR-V + meta + vkCache");
        free(ba);
    }

    char *refl = read_source_file("src/garnet-metal/garnet_spvrefl.h", "garnet_spvrefl.h");
    CHECK(refl != NULL, "read garnet_spvrefl.h for 8M ID bound cap pin");
    if (refl) {
        CHECK(strstr(refl, "#define RDNA4S_MAX_ID_BOUND (8u * 1024u * 1024u)") != NULL &&
              strstr(refl, "if (m->bound > RDNA4S_MAX_ID_BOUND)") != NULL,
              "R3: garnet_spvrefl raises ID bound cap to 8M for up to 64 MiB SPIR-V");
        free(refl);
    }

    char *indc = read_source_file("src/garnet-metal/garnet_indirect_compute.h", "garnet_indirect_compute.h");
    CHECK(indc != NULL, "read garnet_indirect_compute.h for 64 MiB word cap pin");
    if (indc) {
        CHECK(strstr(indc, "#define RDNA4I_MAX_WORDS (16u * 1024u * 1024u)") != NULL &&
              strstr(indc, "count > RDNA4I_MAX_WORDS") != NULL,
              "R3: garnet_indirect_compute raises SPIR-V word cap to 16M words (64 MiB)");
        free(indc);
    }

    char *icb = read_source_file("src/garnet-metal/garnet_icb_abi.h", "garnet_icb_abi.h");
    CHECK(icb != NULL, "read garnet_icb_abi.h for 64 MiB word cap pin");
    if (icb) {
        CHECK(strstr(icb, "#define RDNA4IC_MAX_WORDS (16u * 1024u * 1024u)") != NULL &&
              strstr(icb, "count > RDNA4IC_MAX_WORDS") != NULL,
              "R3: garnet_icb_abi raises compute ICB SPIR-V word cap to 16M words (64 MiB)");
        free(icb);
    }

    char *tex = read_source_file("src/garnet-metal/garnet_textures.m", "garnet_textures.m");
    char *sr = read_source_file("src/garnet-metal/garnet_states_residency.m", "garnet_states_residency.m");
    char *heaps = read_source_file("src/garnet-metal/garnet_heaps.m", "garnet_heaps.m");
    CHECK(tex != NULL && sr != NULL && heaps != NULL, "read garnet_textures.m, garnet_states_residency.m, and garnet_heaps.m for Wave 2 Bindless Bridges 1 & 2 pins");
    if (tex && sr && heaps) {
        CHECK(strstr(tex, "rdna4_argument_resource_token_bindless(self, _dev, YES, YES)") != NULL,
              "Bridge 1: RDNA4Texture unifies uniqueIdentifier/gpuResourceID to 0x5244 token");
        CHECK(strstr(sr, "rdna4_argument_resource_token_bindless(self, _dev, NO, YES)") != NULL,
              "Bridge 2: RDNA4SamplerState.gpuResourceID marks bindlessQueried");
        CHECK(strstr(heaps, "newAccelerationStructureWithSize:(NSUInteger)size offset:") != NULL &&
              strstr(heaps, "newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d offset:") != NULL,
              "Wave 4.2: RDNA4Heap wires placement & automatic acceleration structure constructors");
    }
    free(tex);
    free(sr);
    free(heaps);

    char *live = read_source_file("src/garnet-metal/tests/test-metal3-raytracing.m", "test-metal3-raytracing.m");
    CHECK(live != NULL, "read test-metal3-raytracing.m for source pins");
    if (live) {
        CHECK(strstr(live, "createGarnetDevice") != NULL, "live test uses self-contained createGarnetDevice");
        CHECK(strstr(live, "supportsRaytracing") != NULL, "live test checks supportsRaytracing");
        CHECK(strstr(live, "supportsRaytracingFromRender") != NULL, "live test checks supportsRaytracingFromRender");
        CHECK(strstr(live, "supportsPrimitiveMotionBlur") != NULL, "live test checks supportsPrimitiveMotionBlur == NO");
        CHECK(strstr(live, "setAccelerationStructure:") != NULL, "live test binds AS directly and in argbuf");
        CHECK(strstr(live, "setFragmentAccelerationStructure:") != NULL, "live test exercises fragment-stage ray query");
        CHECK(strstr(live, "copyAndCompactAccelerationStructure:") != NULL, "live test traverses compacted AS");
        CHECK(strstr(live, "refitAccelerationStructure:") != NULL, "live test traverses refitted AS");
        CHECK(strstr(live, "newIntersectionFunctionTableWithDescriptor:") != NULL, "live test creates MTLIntersectionFunctionTable");
        CHECK(strstr(live, "setOpaqueTriangleIntersectionFunctionWithSignature:") != NULL, "live test exercises opaque triangle IFT swap");
        CHECK(strstr(live, "MTLStorageModePrivate") != NULL &&
              strstr(live, "copyFromTexture:rtTex") != NULL &&
              strstr(live, "getBytes:px") == NULL,
              "fragment render target uses Private storage + blit readback (macOS discrete GPU safe)");
        free(live);
    }
}

static void test_wave2_curve_caps_and_ift(void) {
    CHECK(GARNETX_CAP_RT_CURVES == (1u << 13), "GARNETX_CAP_RT_CURVES == 1<<13");
    CHECK(GARNETX_CAP_RT_MOTION_BLUR == (1u << 14), "GARNETX_CAP_RT_MOTION_BLUR == 1<<14");
    CHECK(RDNA4RTR_IFT_KIND_CURVE == UINT32_C(2), "RDNA4RTR_IFT_KIND_CURVE == 2");

    rdna4rtr_hw_caps_t c = make_full_gfx12_caps();
    c.curve_geometry_supported = 1u;
    CHECK(rdna4rtr_supports_curve_intersections(&c) == 1,
          "curve_geometry_supported=1 + core RT enables curve intersections");

    /* Exact Apple Clang 21 curve_rt.ll !air.intersection IR fixture check. */
    const char *curve_air_ir =
        "define i1 @curve_anyhit(float noundef %dist, float noundef %param, i32 noundef %prim) local_unnamed_addr #0 {\n"
        "  ret i1 true\n"
        "}\n"
        "!air.intersection = !{!14}\n"
        "!14 = !{ptr @curve_anyhit, !15, !17, !\"air.curve\", !\"air.instancing\", !\"air.curve_data\"}\n";
    uint32_t kind = 99u;
    CHECK(rdna4rtr_infer_ift_kind_from_ir(curve_air_ir, strlen(curve_air_ir), "curve_anyhit", &kind) == 1 &&
          kind == RDNA4RTR_IFT_KIND_CURVE,
          "Wave 0.1 curve_anyhit (!\"air.curve\", !\"air.curve_data\") -> Curve");

    /* Reflection JSON "Curve" / "CurveAnyHit" kind parsing. */
    rdna4rtr_ift_reflection_t r;
    const char *curve_json =
        "{\"intersection_function_tables\":{\"abi_version\":2,\"header_words\":66,"
        "\"tables\":[{\"param_index\":1,\"metal_index\":1,\"descriptor\":{\"set\":0,\"binding\":1,\"count\":1}}],"
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"curve_anyhit\",\"kind\":\"Curve\"},"
        "{\"handle_id\":2,\"symbol\":\"curve_ah2\",\"kind\":\"CurveAnyHit\"}]}}";
    CHECK(rdna4rtr_parse_ift_json(curve_json, strlen(curve_json), &r) == RDNA4RTR_OK &&
          r.handle_count == 2u && r.handles[0].kind == RDNA4RTR_IFT_KIND_CURVE &&
          r.handles[1].kind == RDNA4RTR_IFT_KIND_CURVE,
          "IFT reflection JSON parses Curve and CurveAnyHit kinds");
    rdna4rtr_ift_refl_destroy(&r);

    /* IFT function slot + opaque curve gated helper. */
    rdna4rtr_ift_table_t t;
    CHECK(rdna4rtr_ift_init(&t, 4u, 2u) == RDNA4RTR_OK, "curve IFT init");
    CHECK(rdna4rtr_ift_set_function(&t, 0u, 1u, RDNA4RTR_IFT_KIND_CURVE) == RDNA4RTR_OK &&
          rdna4rtr_ift_get_handle(&t, 0u) == 1u,
          "IFT slot accepts RDNA4RTR_IFT_KIND_CURVE handle");
    uint32_t ids[2] = { 1u, 2u };
    uint32_t kinds[2] = { RDNA4RTR_IFT_KIND_CURVE, RDNA4RTR_IFT_KIND_CURVE };
    CHECK(rdna4rtr_ift_set_functions_range(&t, 1u, 2u, ids, kinds) == RDNA4RTR_OK,
          "IFT range accepts RDNA4RTR_IFT_KIND_CURVE");
    CHECK(rdna4rtr_ift_set_opaque_curve_range_ex(&t, 3u, 1u, RDNA4RTR_SIG_CURVE_DATA, 1) == RDNA4RTR_OK &&
          rdna4rtr_ift_get_mode(&t, 3u) == RDNA4RTR_IFT_SLOT_OPAQUE_CURVE &&
          rdna4rtr_ift_get_handle(&t, 3u) == 0u,
          "opaque curve gated helper returns OK when curve_supported=1");
    rdna4rtr_ift_destroy(&t);
}

static void test_wave3_motion_runtime_table_and_ift(void) {
    CHECK(RDNA4RT_PRIM_KIND_TLAS_MOTION == UINT32_C(2), "RDNA4RT_PRIM_KIND_TLAS_MOTION == 2");

    rdna4rt_prim_slot_t *tbl = (rdna4rt_prim_slot_t *)calloc(RDNA4RT_PRIM_TABLE_CAP, sizeof(*tbl));
    CHECK(tbl != NULL, "calloc prim table for motion TLAS");
    if (tbl) {
        const uint64_t mtlasBda = UINT64_C(0x5555000000);
        const uint64_t mtlasPay = UINT64_C(0x6666000000);
        CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                        mtlasBda, mtlasPay, 4u, 64u, 56u,
                                        RDNA4RT_PRIM_KIND_TLAS_MOTION) == RDNA4RTR_OK,
              "prim table accepts RDNA4RT_PRIM_KIND_TLAS_MOTION");
        rdna4rt_prim_slot_t got;
        CHECK(rdna4rt_prim_table_lookup(tbl, RDNA4RT_PRIM_TABLE_CAP, mtlasBda, &got) == 1 &&
              got.kind == RDNA4RT_PRIM_KIND_TLAS_MOTION && got.payload_bda == mtlasPay && got.count == 4u,
              "prim table round-trips RDNA4RT_PRIM_KIND_TLAS_MOTION slot");
        CHECK(rdna4rt_prim_table_upsert(tbl, RDNA4RT_PRIM_TABLE_CAP,
                                        mtlasBda, mtlasPay, 4u, 64u, 56u, 3u) == RDNA4RTR_ERR_INVALID,
              "prim table rejects invalid kind > TLAS_MOTION");
        free(tbl);
    }

    rdna4rtr_ift_table_t t;
    CHECK(rdna4rtr_ift_init(&t, 4u, 2u) == RDNA4RTR_OK, "motion IFT init");
    uint64_t msig = RDNA4RTR_SIG_INSTANCING | RDNA4RTR_SIG_INSTANCE_MOTION | RDNA4RTR_SIG_PRIMITIVE_MOTION;
    CHECK(rdna4rtr_ift_set_opaque_triangle_range_ex(&t, 0u, 2u, msig, 0) == RDNA4RTR_ERR_UNSUPPORTED,
          "opaque triangle rejects motion sig when motion_supported=0");
    CHECK(rdna4rtr_ift_set_opaque_triangle_range_ex(&t, 0u, 2u, msig, 1) == RDNA4RTR_OK &&
          rdna4rtr_ift_get_mode(&t, 0u) == RDNA4RTR_IFT_SLOT_OPAQUE_TRIANGLE &&
          rdna4rtr_ift_get_handle(&t, 0u) == 0u,
          "opaque triangle accepts motion sig when motion_supported=1");
    rdna4rtr_ift_destroy(&t);
}

int main(void) {
    test_capabilities_and_footnote10();
    test_as_reflection_json();
    test_ift_reflection_json();
    test_ir_kind_inference();
    test_ift_encoder_and_dispatch_sim();
    test_as_descriptor_and_bda_planners();
    test_wave4_prim_data_table_and_abi();
    test_wave2_curve_caps_and_ift();
    test_wave3_motion_runtime_table_and_ift();
    test_source_pins();
    printf("test-rt-runtime: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
