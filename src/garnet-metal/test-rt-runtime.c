// Pure-C host unit tests for rdna4_rt_runtime.h and Task 7.3 runtime source pins.
// Build (GCC):
//   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-runtime.c -o test-rt-runtime
// Build (Clang ASan+UBSan):
//   clang -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror src/garnet-metal/test-rt-runtime.c -o test-rt-runtime
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdna4_rt_runtime.h"

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
        "\"handles\":[{\"handle_id\":1,\"symbol\":\"a\",\"kind\":\"Curve\"}]}}";
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
        "!43 = !{ptr @meta_only_tri, !\"triangle\"}\n";

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
          "argbuf nil AS encodes 0");
    CHECK(rdna4rtr_plan_as_bda_encode(RDNA4RTR_ARG_KIND_INSTANCE_AS, 0u, 8u, 64u,
                                      0, bda, RDNA4RTR_AS_KIND_PRIMITIVE, &bp) == RDNA4RTR_ERR_KIND_MISMATCH,
          "argbuf BLAS into InstanceAS field rejected");
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

static void test_source_pins(void) {
    char *impl = read_source_file("src/garnet-metal/rdna4_rt_runtime_impl.inc", "rdna4_rt_runtime_impl.inc");
    CHECK(impl != NULL, "read rdna4_rt_runtime_impl.inc for source pins");
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
        free(impl);
    }

    char *live = read_source_file("src/garnet-metal/test-metal3-raytracing.m", "test-metal3-raytracing.m");
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

int main(void) {
    test_capabilities_and_footnote10();
    test_as_reflection_json();
    test_ift_reflection_json();
    test_ir_kind_inference();
    test_ift_encoder_and_dispatch_sim();
    test_as_descriptor_and_bda_planners();
    test_source_pins();
    printf("test-rt-runtime: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
