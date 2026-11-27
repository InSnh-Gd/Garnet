// test-mesh-runtime.c: pure-C host test suite for garnet_mesh.h and source pins for
// garnet_mesh_rt.m / test-metal3-mesh-shaders.m (Wave 3D Task 6.2).
//
// Compile & run (GCC -Werror):
//   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-mesh-runtime.c -o "$TMPDIR/test-mesh-runtime-gcc"
// Compile & run (Clang ASan+UBSan -Werror):
//   clang -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal src/garnet-metal/test-mesh-runtime.c -o "$TMPDIR/test-mesh-runtime-clang"
#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define RDNA4_TEST_HAS_VULKAN 1
#include <vulkan/vulkan.h>
#endif
#endif

#include "garnet_mesh.h"

#ifdef RDNA4_TEST_HAS_VULKAN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "garnet_spvrefl.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;

static void fail_at(int line, const char *expr) {
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
    exit(1);
}

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) fail_at(__LINE__, #cond); \
} while (0)

static rdna4dc_caps make_rx9060_gfx_caps(int native_task) {
    rdna4dc_caps c;
    memset(&c, 0, sizeof c);
    c.invocations = 1024u;
    c.workgroup_size[0] = 1024u;
    c.workgroup_size[1] = 1024u;
    c.workgroup_size[2] = 1024u;
    c.shared_memory = 32768u;
    c.buffer_length = UINT64_C(4) << 30;
    c.subgroup_size = 32u;

    c.metal3.mesh_shader = 1;
    c.metal3.object_shader = native_task ? 1 : 0;
    c.metal3.mesh_max_payload_bytes = native_task ? 16384u : 0u;
    c.metal3.mesh_max_output_vertices = 256u;
    c.metal3.mesh_max_output_primitives = 256u;
    c.metal3.mesh_max_workgroup_count[0] = 65535u;
    c.metal3.mesh_max_workgroup_count[1] = 65535u;
    c.metal3.mesh_max_workgroup_count[2] = 65535u;
    c.metal3.mesh_max_workgroup_total_count = 4194304u;
    c.metal3.mesh_max_workgroup_invocations = 1024u;
    c.metal3.object_max_workgroup_count[0] = native_task ? 65535u : 0u;
    c.metal3.object_max_workgroup_count[1] = native_task ? 65535u : 0u;
    c.metal3.object_max_workgroup_count[2] = native_task ? 65535u : 0u;
    c.metal3.object_max_workgroup_total_count = native_task ? 4194304u : 0u;
    c.metal3.object_max_workgroup_invocations = native_task ? 1024u : 0u;
    return c;
}

static size_t build_min_spv(uint32_t *buf, size_t cap,
                            uint32_t exec_model,
                            const uint32_t *caps, size_t ncaps,
                            int has_local, uint32_t lx, uint32_t ly, uint32_t lz,
                            uint32_t out_verts, uint32_t out_prims, uint32_t topo_mode) {
    size_t n = 0;
    CHECK(cap >= 64u);
    buf[n++] = RDNA4MESH_SPV_MAGIC;
    buf[n++] = 0x00010400u; // SPIR-V 1.4
    buf[n++] = 0u;
    buf[n++] = 16u;
    buf[n++] = 0u;

    for (size_t i = 0; i < ncaps; i++) {
        buf[n++] = (2u << 16) | 17u; // OpCapability
        buf[n++] = caps[i];
    }
    // OpEntryPoint <model> %1 "main"
    buf[n++] = (5u << 16) | 15u;
    buf[n++] = exec_model;
    buf[n++] = 1u;
    buf[n++] = 0x6E69616Du; // "main"
    buf[n++] = 0u;

    if (has_local) {
        buf[n++] = (6u << 16) | 16u; // OpExecutionMode %1 LocalSize lx ly lz
        buf[n++] = 1u;
        buf[n++] = RDNA4MESH_SPV_MODE_LOCAL_SIZE;
        buf[n++] = lx;
        buf[n++] = ly;
        buf[n++] = lz;
    }
    if (out_verts) {
        buf[n++] = (4u << 16) | 16u;
        buf[n++] = 1u;
        buf[n++] = RDNA4MESH_SPV_MODE_OUT_VERTICES;
        buf[n++] = out_verts;
    }
    if (out_prims) {
        buf[n++] = (4u << 16) | 16u;
        buf[n++] = 1u;
        buf[n++] = RDNA4MESH_SPV_MODE_OUT_PRIMS_EXT;
        buf[n++] = out_prims;
    }
    if (topo_mode) {
        buf[n++] = (3u << 16) | 16u;
        buf[n++] = 1u;
        buf[n++] = topo_mode;
    }
    CHECK(n <= cap);
    return n;
}

static void test_locked_abi_and_payload_strides(void) {
    CHECK(GARNETX_CAP_MESH_EMULATED_PAYLOAD == (1u << 8));
    CHECK(GARNETX_MESH_GRID_PARAMS_BINDING == 673u);
    CHECK(GARNETX_MESH_PAYLOAD_BINDING == 674u);
    CHECK(GARNETX_MESH_INDIRECT_BINDING == 675u);
    CHECK(sizeof(rdna4mesh_grid_params_t) == 16u);
    CHECK(sizeof(rdna4mesh_indirect_cmd_t) == 12u);
    CHECK(RDNA4MESH_MAX_PAYLOAD_BYTES == 16384u);
    CHECK(RDNA4MESH_MIN_GRID == 1024u);

    uint32_t stride = 0u;
    CHECK(rdna4mesh_payload_stride(0u, &stride) == RDNA4MESH_OK && stride == 16u);
    CHECK(rdna4mesh_payload_stride(1u, &stride) == RDNA4MESH_OK && stride == 16u);
    CHECK(rdna4mesh_payload_stride(12u, &stride) == RDNA4MESH_OK && stride == 16u);
    CHECK(rdna4mesh_payload_stride(16u, &stride) == RDNA4MESH_OK && stride == 16u);
    CHECK(rdna4mesh_payload_stride(17u, &stride) == RDNA4MESH_OK && stride == 32u);
    CHECK(rdna4mesh_payload_stride(16369u, &stride) == RDNA4MESH_OK && stride == 16384u);
    CHECK(rdna4mesh_payload_stride(16384u, &stride) == RDNA4MESH_OK && stride == 16384u);
    CHECK(rdna4mesh_payload_stride(16385u, &stride) == RDNA4MESH_ERR_PAYLOAD_LIMIT && stride == 0u);
    CHECK(rdna4mesh_payload_stride(UINT32_MAX, &stride) == RDNA4MESH_ERR_PAYLOAD_LIMIT);
    CHECK(rdna4mesh_payload_stride(16u, NULL) == RDNA4MESH_ERR_INVALID_ARG);
}

static void test_mode_selection_and_caps(void) {
    rdna4dc_caps gfx_emul = make_rx9060_gfx_caps(0);
    rdna4dc_caps native = make_rx9060_gfx_caps(1);
    rdna4dc_caps none;
    memset(&none, 0, sizeof none);

    CHECK(rdna4mesh_select_mode(&none.metal3, 0) == RDNA4MESH_MODE_UNSUPPORTED);
    CHECK(rdna4mesh_select_mode(&none.metal3, 1) == RDNA4MESH_MODE_UNSUPPORTED);
    CHECK(!rdna4mesh_device_supports_mesh(&none.metal3, 1));

    // Live RX 9060 XT GFX ring 0 (meshShader=1, taskShader=0):
    CHECK(rdna4mesh_select_mode(&gfx_emul.metal3, 0) == RDNA4MESH_MODE_DIRECT_MESH_ONLY);
    CHECK(rdna4mesh_select_mode(&gfx_emul.metal3, 1) == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    CHECK(rdna4mesh_device_supports_mesh(&gfx_emul.metal3, 1));
    CHECK(!rdna4mesh_device_supports_mesh(&gfx_emul.metal3, 0));

    // Native TaskEXT + MeshEXT:
    CHECK(rdna4mesh_select_mode(&native.metal3, 0) == RDNA4MESH_MODE_DIRECT_MESH_ONLY);
    CHECK(rdna4mesh_select_mode(&native.metal3, 1) == RDNA4MESH_MODE_NATIVE_TASK_MESH);
    CHECK(rdna4mesh_device_supports_mesh(&native.metal3, 0));

    // Mesh grid < 1024 must be rejected.
    rdna4dc_caps small_grid = gfx_emul;
    small_grid.metal3.mesh_max_workgroup_total_count = 1023u;
    CHECK(rdna4mesh_select_mode(&small_grid.metal3, 0) == RDNA4MESH_MODE_UNSUPPORTED);
    small_grid = gfx_emul;
    small_grid.metal3.mesh_max_workgroup_count[0] = 512u;
    CHECK(rdna4mesh_select_mode(&small_grid.metal3, 0) == RDNA4MESH_MODE_UNSUPPORTED);

    // Native task shader with payload < 16 KiB falls back to emulated Object+Mesh.
    rdna4dc_caps small_payload = native;
    small_payload.metal3.mesh_max_payload_bytes = 8192u;
    CHECK(rdna4mesh_select_mode(&small_payload.metal3, 1) == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
}

static void test_footnote10_and_descriptor_validation(void) {
    const char *why = NULL;
    CHECK(rdna4mesh_check_footnote10(0u, &why) == RDNA4MESH_OK && why == NULL);
    // privateFunctions ALONE is explicitly permitted by Footnote 10!
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_PRIVATE_FUNCTIONS, &why) == RDNA4MESH_OK);

    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_PUBLIC_FUNCTIONS, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(why != NULL);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_BINARY_FUNCTIONS, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_GROUPS, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_SUPPORT_ADD_BIN_FNS, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_VFT_METADATA, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_PRIVATE_FUNCTIONS | RDNA4MESH_FN10_PUBLIC_FUNCTIONS, &why) ==
          RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_RAYTRACING, &why) == RDNA4MESH_ERR_FOOTNOTE10_RT);
    CHECK(rdna4mesh_check_footnote10(RDNA4MESH_FN10_PRIVATE_FUNCTIONS | RDNA4MESH_FN10_RAYTRACING, &why) ==
          RDNA4MESH_ERR_FOOTNOTE10_RT);

    rdna4dc_caps caps = make_rx9060_gfx_caps(0);
    rdna4mesh_exec_mode_t mode = RDNA4MESH_MODE_UNSUPPORTED;
    rdna4mesh_desc_t d;
    memset(&d, 0, sizeof d);

    // Missing meshFunction rejected.
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_INVALID_ARG);

    // Valid mesh-only descriptor with privateFunctions.
    d.has_mesh_function = 1;
    d.has_fragment_function = 1;
    d.max_total_threadgroups_per_mesh_grid = 1024u;
    d.max_total_threads_per_mesh_threadgroup = 64u;
    d.max_vertex_amplification_count = 1u;
    d.footnote10_flags = RDNA4MESH_FN10_PRIVATE_FUNCTIONS;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_OK);
    CHECK(mode == RDNA4MESH_MODE_DIRECT_MESH_ONLY);

    // Non-zero payloadMemoryLength without objectFunction rejected.
    d.payload_memory_length = 16u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_PAYLOAD_LIMIT);

    // Valid object+mesh with full 16,384-byte payload selects EMULATED_OBJECT_MESH on GFX ring 0.
    d.has_object_function = 1;
    d.payload_memory_length = 16384u;
    d.max_total_threads_per_object_threadgroup = 64u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_OK);
    CHECK(mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);

    // Payload 16385 B rejected.
    d.payload_memory_length = 16385u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_PAYLOAD_LIMIT);
    d.payload_memory_length = 16384u;

    // Footnote 10 public functions / RT rejected on descriptor.
    d.footnote10_flags = RDNA4MESH_FN10_PUBLIC_FUNCTIONS;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    d.footnote10_flags = RDNA4MESH_FN10_RAYTRACING;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_FOOTNOTE10_RT);
    d.footnote10_flags = 0u;

    // Vertex amplification > 1 rejected.
    d.max_vertex_amplification_count = 2u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_AMP_UNSUPPORTED);
    d.max_vertex_amplification_count = 1u;

    // Excessive mesh grid / workgroup sizes rejected.
    d.max_total_threadgroups_per_mesh_grid = (uint64_t)caps.metal3.mesh_max_workgroup_total_count + 1u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_GRID_LIMIT);
    d.max_total_threadgroups_per_mesh_grid = 1024u;

    d.max_total_threads_per_mesh_threadgroup = 1025u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    d.max_total_threads_per_mesh_threadgroup = 64u;

    d.max_total_threads_per_object_threadgroup = 1025u;
    CHECK(rdna4mesh_validate_desc(&d, &caps, 1u, &mode, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    d.max_total_threads_per_object_threadgroup = 64u;

    // Initial LocalSize selection helper.
    uint32_t loc[3] = {0};
    rdna4mesh_initial_local_size(0u, 1024u, 1024u, 32u, 0, loc);
    CHECK(loc[0] == 32u && loc[1] == 1u && loc[2] == 1u);
    rdna4mesh_initial_local_size(16u, 1024u, 1024u, 32u, 0, loc);
    CHECK(loc[0] == 16u && loc[1] == 1u && loc[2] == 1u);
    rdna4mesh_initial_local_size(96u, 1024u, 1024u, 32u, 1, loc);
    CHECK(loc[0] == 32u && loc[1] == 1u && loc[2] == 1u);
}

static void test_reflection_json_parser(void) {
    // 1. Valid 0078 emulated Object JSON (with 16 KiB payload, emulated_payload, payload_stride).
    const char *obj_json =
        "{"
        "\"stage\":\"Object\","
        "\"entry_point\":\"obj_main\","
        "\"local_size\":[32,1,1],"
        "\"max_work_group_size\":256,"
        "\"object\":{"
        "  \"has_payload\":true,"
        "  \"payload_size\":16384,"
        "  \"payload_stride\":16384,"
        "  \"emulated_payload\":true,"
        "  \"object_grid_dims\":[4,2,1],"
        "  \"max_total_threadgroups_per_mesh_grid\":2048"
        "},"
        "\"bindings\":[{\"kind\":\"Buffer\",\"descriptor\":{\"set\":0,\"binding\":0,\"count\":1}}]"
        "}";
    rdna4mesh_refl_t r;
    CHECK(rdna4mesh_parse_reflection_json(obj_json, strlen(obj_json), &r) == RDNA4MESH_OK);
    CHECK(r.stage == RDNA4MESH_STAGE_OBJECT);
    CHECK(strcmp(r.entry_point, "obj_main") == 0);
    CHECK(r.has_local_size && r.local_size[0] == 32u && r.local_size[1] == 1u && r.local_size[2] == 1u);
    CHECK(r.has_max_work_group_size && r.max_work_group_size == 256u);
    CHECK(r.has_object && r.obj.has_payload && r.obj.has_payload_size && r.obj.payload_size == 16384u);
    CHECK(r.obj.has_payload_stride && r.obj.payload_stride == 16384u && r.obj.emulated_payload);
    CHECK(r.obj.has_object_grid_dims && r.obj.object_grid_dims[0] == 4u && r.obj.object_grid_dims[1] == 2u);
    CHECK(r.obj.has_max_mesh_grid && r.obj.max_total_threadgroups_per_mesh_grid == 2048u);
    CHECK(!r.has_vft && !r.has_raytracing && !r.has_sampler_coord_flags && !r.has_argument_tables);

    // 2. Valid 0076/0078 Mesh JSON (Triangle + PerPrimitiveEXT + CullPrimitive + Layer + Viewport).
    const char *mesh_json =
        "{"
        "\"stage\":\"Mesh\","
        "\"entry_point\":\"mesh_main\","
        "\"local_size\":[3,1,1],"
        "\"mesh\":{"
        "  \"topology\":\"Triangle\","
        "  \"max_vertices\":6,"
        "  \"max_primitives\":2,"
        "  \"has_payload\":true,"
        "  \"payload_size\":16384,"
        "  \"payload_stride\":16384,"
        "  \"emulated_payload\":true,"
        "  \"writes_position\":true,"
        "  \"writes_cull_primitive\":true,"
        "  \"writes_layer\":true,"
        "  \"writes_viewport_index\":true,"
        "  \"vertex_outputs\":[{\"location\":0,\"name\":\"vcolor\",\"type\":\"float4\"}],"
        "  \"primitive_outputs\":[{\"location\":1,\"name\":\"pcolor\",\"type\":\"float4\"}]"
        "}"
        "}";
    CHECK(rdna4mesh_parse_reflection_json(mesh_json, strlen(mesh_json), &r) == RDNA4MESH_OK);
    CHECK(r.stage == RDNA4MESH_STAGE_MESH && r.has_mesh);
    CHECK(r.mesh.topology == RDNA4MESH_TOPO_TRIANGLE);
    CHECK(r.mesh.max_vertices == 6u && r.mesh.max_primitives == 2u);
    CHECK(r.mesh.has_payload && r.mesh.payload_size == 16384u && r.mesh.emulated_payload);
    CHECK(r.mesh.writes_position && r.mesh.writes_cull_primitive && r.mesh.writes_layer && r.mesh.writes_viewport_index);
    CHECK(r.mesh.vertex_output_count == 1u && r.mesh.vertex_out_mask == (UINT64_C(1) << 0));
    CHECK(r.mesh.primitive_output_count == 1u && r.mesh.primitive_out_mask == (UINT64_C(1) << 1));

    // Point and Line topologies.
    const char *pt_json = "{\"stage\":\"Mesh\",\"mesh\":{\"topology\":\"Point\",\"max_vertices\":4,\"max_primitives\":4,\"has_payload\":false,\"payload_size\":null}}";
    CHECK(rdna4mesh_parse_reflection_json(pt_json, strlen(pt_json), &r) == RDNA4MESH_OK);
    CHECK(r.mesh.topology == RDNA4MESH_TOPO_POINT && !r.mesh.has_payload && !r.mesh.has_payload_size);

    const char *ln_json = "{\"stage\":\"Mesh\",\"mesh\":{\"topology\":\"Line\",\"max_vertices\":4,\"max_primitives\":2,\"has_payload\":false}}";
    CHECK(rdna4mesh_parse_reflection_json(ln_json, strlen(ln_json), &r) == RDNA4MESH_OK);
    CHECK(r.mesh.topology == RDNA4MESH_TOPO_LINE);

    // 3. Fragment JSON with PerPrimitiveEXT varyings + Footnote 10 metadata detection + SamplerCoordinateFlags.
    const char *frag_json =
        "{"
        "\"stage\":\"Fragment\","
        "\"entry_point\":\"frag_main\","
        "\"varyings\":[{\"location\":0,\"per_primitive\":false},{\"location\":1,\"per_primitive\":true}],"
        "\"bindings\":[{\"kind\":\"SamplerCoordinateFlags\",\"descriptor\":{\"set\":0,\"binding\":674}}],"
        "\"argument_resource_tables\":{\"abi_version\":10}"
        "}";
    CHECK(rdna4mesh_parse_reflection_json(frag_json, strlen(frag_json), &r) == RDNA4MESH_OK);
    CHECK(r.stage == RDNA4MESH_STAGE_FRAGMENT && r.varying_count == 2u);
    CHECK(r.varyings[0].location == 0u && !r.varyings[0].per_primitive);
    CHECK(r.varyings[1].location == 1u && r.varyings[1].per_primitive);
    CHECK(r.has_sampler_coord_flags && r.has_argument_tables);

    const char *vft_json = "{\"stage\":\"Fragment\",\"visible_function_tables\":{\"abi_version\":1}}";
    CHECK(rdna4mesh_parse_reflection_json(vft_json, strlen(vft_json), &r) == RDNA4MESH_OK && r.has_vft);

    const char *rt_json = "{\"stage\":\"Fragment\",\"acceleration_structures\":[{\"kind\":\"Instance\"}]}";
    CHECK(rdna4mesh_parse_reflection_json(rt_json, strlen(rt_json), &r) == RDNA4MESH_OK && r.has_raytracing);

    const char *empty_rt_json = "{\"stage\":\"Fragment\",\"acceleration_structures\":[]}";
    CHECK(rdna4mesh_parse_reflection_json(empty_rt_json, strlen(empty_rt_json), &r) == RDNA4MESH_OK && !r.has_raytracing);

    const char *ift_json = "{\"stage\":\"Fragment\",\"intersection_function_tables\":{\"abi_version\":1}}";
    CHECK(rdna4mesh_parse_reflection_json(ift_json, strlen(ift_json), &r) == RDNA4MESH_OK && r.has_raytracing);

    // 4. Negative / malformed JSON tests.
    CHECK(rdna4mesh_parse_reflection_json(NULL, 1u, &r) == RDNA4MESH_ERR_INVALID_ARG);
    CHECK(rdna4mesh_parse_reflection_json("", 0u, &r) == RDNA4MESH_ERR_INVALID_ARG);
    const char *bad_cases[] = {
        "{",
        "[]",
        "{\"stage\":\"Mesh\"} trailing",
        "{\"mesh\":{\"topology\":\"Triangle\",\"max_vertices\":3,\"max_primitives\":1}}", // missing has_payload
        "{\"object\":{}}", // missing has_payload
        "{\"mesh\":{\"topology\":\"Quad\",\"max_vertices\":4,\"max_primitives\":1,\"has_payload\":false}}",
        "{\"object\":{\"has_payload\":false},\"object\":{\"has_payload\":false}}", // duplicate object
        "{\"mesh\":{\"topology\":\"Point\",\"max_vertices\":1,\"max_primitives\":1,\"has_payload\":false},\"mesh\":{\"topology\":\"Point\",\"max_vertices\":1,\"max_primitives\":1,\"has_payload\":false}}",
        "{\"local_size\":[1,2]}", // short array
        "{\"object\":{\"has_payload\":true,\"payload_size\":-1}}",
        "{\"object\":{\"has_payload\":true,\"payload_size\":01}}",
        "{\"object\":{\"has_payload\":true,\"payload_size\":4294967296}}",
    };
    for (size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++) {
        rdna4mesh_status_t st = rdna4mesh_parse_reflection_json(bad_cases[i], strlen(bad_cases[i]), &r);
        CHECK(st == RDNA4MESH_ERR_JSON || st == RDNA4MESH_ERR_TOPOLOGY);
    }

    // Excessive nesting depth rejected cleanly (no stack overflow).
    char deep[256];
    size_t pos = 0;
    memcpy(deep + pos, "{\"x\":", 5); pos += 5;
    for (int i = 0; i < 70; i++) deep[pos++] = '[';
    for (int i = 0; i < 70; i++) deep[pos++] = ']';
    deep[pos++] = '}';
    CHECK(rdna4mesh_parse_reflection_json(deep, pos, &r) == RDNA4MESH_ERR_JSON);
}

static void test_spv_inspector_and_stage_linkage(void) {
    uint32_t spv[64];
    rdna4mesh_spv_info_t info;
    const uint32_t mesh_caps[] = { RDNA4MESH_SPV_CAP_MESH_SHADING, RDNA4MESH_SPV_CAP_DRAW_PARAMS };
    const uint32_t rt_caps[] = { RDNA4MESH_SPV_CAP_MESH_SHADING, RDNA4MESH_SPV_CAP_RAY_QUERY_KHR };

    // Valid TaskEXT, GLCompute (emulated), MeshEXT (Triangle/Point/Line), Fragment.
    size_t n_task = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_TASK_EXT, mesh_caps, 1, 1, 32, 1, 1, 0, 0, 0);
    rdna4mesh_spv_info_t task_info;
    CHECK(rdna4mesh_inspect_spv(spv, n_task, &task_info) == RDNA4MESH_OK);
    CHECK(task_info.ep_model == RDNA4MESH_SPV_EM_TASK_EXT && task_info.has_mesh_shading_cap && !task_info.has_raytracing_cap);
    CHECK(task_info.has_local_size && task_info.local_size[0] == 32u);

    size_t n_comp = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_GLCOMPUTE, NULL, 0, 1, 32, 1, 1, 0, 0, 0);
    rdna4mesh_spv_info_t comp_info;
    CHECK(rdna4mesh_inspect_spv(spv, n_comp, &comp_info) == RDNA4MESH_OK);
    CHECK(comp_info.ep_model == RDNA4MESH_SPV_EM_GLCOMPUTE);

    size_t n_mesh = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_MESH_EXT, mesh_caps, 2, 1, 3, 1, 1, 6, 2, RDNA4MESH_SPV_MODE_OUT_TRIS_EXT);
    rdna4mesh_spv_info_t mesh_info;
    CHECK(rdna4mesh_inspect_spv(spv, n_mesh, &mesh_info) == RDNA4MESH_OK);
    CHECK(mesh_info.ep_model == RDNA4MESH_SPV_EM_MESH_EXT && mesh_info.topology == RDNA4MESH_TOPO_TRIANGLE);
    CHECK(mesh_info.output_vertices == 6u && mesh_info.output_primitives == 2u && mesh_info.has_draw_params_cap);

    size_t n_pt = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_MESH_EXT, mesh_caps, 1, 1, 1, 1, 1, 4, 4, RDNA4MESH_SPV_MODE_OUT_POINTS);
    CHECK(rdna4mesh_inspect_spv(spv, n_pt, &info) == RDNA4MESH_OK && info.topology == RDNA4MESH_TOPO_POINT);

    size_t n_ln = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_MESH_EXT, mesh_caps, 1, 1, 2, 1, 1, 4, 2, RDNA4MESH_SPV_MODE_OUT_LINES_EXT);
    CHECK(rdna4mesh_inspect_spv(spv, n_ln, &info) == RDNA4MESH_OK && info.topology == RDNA4MESH_TOPO_LINE);

    size_t n_frag = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_FRAGMENT, mesh_caps, 1, 0, 0, 0, 0, 0, 0, 0);
    rdna4mesh_spv_info_t frag_info;
    CHECK(rdna4mesh_inspect_spv(spv, n_frag, &frag_info) == RDNA4MESH_OK);
    CHECK(frag_info.ep_model == RDNA4MESH_SPV_EM_FRAGMENT);

    // RayQueryKHR capability detected.
    size_t n_rt = build_min_spv(spv, 64, RDNA4MESH_SPV_EM_MESH_EXT, rt_caps, 2, 1, 3, 1, 1, 3, 1, RDNA4MESH_SPV_MODE_OUT_TRIS_EXT);
    rdna4mesh_spv_info_t rt_mesh_info;
    CHECK(rdna4mesh_inspect_spv(spv, n_rt, &rt_mesh_info) == RDNA4MESH_OK && rt_mesh_info.has_raytracing_cap);

    // Bad SPIR-V inputs rejected cleanly.
    CHECK(rdna4mesh_inspect_spv(NULL, 10, &info) == RDNA4MESH_ERR_SPIRV);
    CHECK(rdna4mesh_inspect_spv(spv, 4, &info) == RDNA4MESH_ERR_SPIRV);
    spv[0] = 0xDEADBEEFu;
    CHECK(rdna4mesh_inspect_spv(spv, n_mesh, &info) == RDNA4MESH_ERR_SPIRV);
    spv[0] = RDNA4MESH_SPV_MAGIC;
    spv[5] = 0u; // wc == 0
    CHECK(rdna4mesh_inspect_spv(spv, n_mesh, &info) == RDNA4MESH_ERR_SPIRV);
    spv[5] = (999u << 16) | 17u; // truncated
    CHECK(rdna4mesh_inspect_spv(spv, n_mesh, &info) == RDNA4MESH_ERR_SPIRV);

    // Cross-stage linkage checks.
    rdna4dc_caps caps_emul = make_rx9060_gfx_caps(0);
    rdna4dc_caps caps_nat = make_rx9060_gfx_caps(1);
    rdna4mesh_desc_t desc;
    memset(&desc, 0, sizeof desc);
    desc.has_object_function = 1;
    desc.has_mesh_function = 1;
    desc.has_fragment_function = 1;
    desc.payload_memory_length = 16384u;

    rdna4mesh_refl_t orefl, mrefl, frefl;
    memset(&orefl, 0, sizeof orefl);
    memset(&mrefl, 0, sizeof mrefl);
    memset(&frefl, 0, sizeof frefl);
    orefl.has_object = 1; orefl.obj.has_payload = 1; orefl.obj.has_payload_size = 1; orefl.obj.payload_size = 16384u;
    orefl.obj.has_max_mesh_grid = 1; orefl.obj.max_total_threadgroups_per_mesh_grid = 2048u;
    mrefl.has_mesh = 1; mrefl.mesh.topology = RDNA4MESH_TOPO_TRIANGLE; mrefl.mesh.max_vertices = 6u; mrefl.mesh.max_primitives = 2u;
    mrefl.mesh.has_payload = 1; mrefl.mesh.has_payload_size = 1; mrefl.mesh.payload_size = 16384u;
    mrefl.mesh.vertex_out_mask = UINT64_C(1) << 0;
    mrefl.mesh.primitive_out_mask = UINT64_C(1) << 1;
    frefl.varying_count = 2u;
    frefl.varyings[0] = (rdna4mesh_varying_t){ 0u, 0 };
    frefl.varyings[1] = (rdna4mesh_varying_t){ 1u, 1 };

    rdna4mesh_linked_info_t linked;
    const char *why = NULL;

    // Valid emulated Object(GLCompute) + Mesh + Frag.
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_OK);
    CHECK(linked.effective_payload_bytes == 16384u && linked.effective_payload_stride == 16384u);
    CHECK(linked.effective_max_mesh_grid == 2048u && linked.topology == RDNA4MESH_TOPO_TRIANGLE);
    CHECK(linked.max_vertices == 6u && linked.max_primitives == 2u);

    // Valid native Object(TaskEXT) + Mesh + Frag.
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_NATIVE_TASK_MESH, &caps_nat, &desc,
                                           &orefl, &task_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_OK);

    // Wrong Object execution model (TaskEXT when emulated expected, GLCompute when native expected).
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &task_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_SPIRV);
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_NATIVE_TASK_MESH, &caps_nat, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_SPIRV);

    // Footnote 10 via SPIR-V RayQueryKHR or fragment VFT reflection rejected.
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &rt_mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_FOOTNOTE10_RT);
    frefl.has_vft = 1;
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_FOOTNOTE10_FN_PTR);
    frefl.has_vft = 0;

    // Fragment PerPrimitiveEXT mismatch rejected.
    frefl.varyings[1] = (rdna4mesh_varying_t){ 2u, 1 };
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_STAGE_LINKAGE);
    // Standalone Apple AIR fragment input (per_primitive == 0) at mesh primitive-only location is
    // allowed so pipeline creation reaches rdna4_mesh_patch_frag_per_primitive().
    frefl.varyings[1] = (rdna4mesh_varying_t){ 1u, 0 };
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_OK);
    // Overlapping mesh per-vertex and PerPrimitiveEXT output locations rejected.
    mrefl.mesh.vertex_out_mask |= (UINT64_C(1) << 1);
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_STAGE_LINKAGE);
    mrefl.mesh.vertex_out_mask = UINT64_C(1) << 0;
    // Also reject overlap between mesh SPIR-V per_prim_out_mask and reflection vertex_out_mask.
    mesh_info.per_prim_out_mask = UINT64_C(1) << 0;
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_STAGE_LINKAGE);
    mesh_info.per_prim_out_mask = 0u;
    frefl.varyings[1] = (rdna4mesh_varying_t){ 1u, 1 };

    // Descriptor payloadMemoryLength smaller than shader payload_size rejected.
    desc.payload_memory_length = 4096u;
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_EMULATED_OBJECT_MESH, &caps_emul, &desc,
                                           &orefl, &comp_info, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_PAYLOAD_LIMIT);
    desc.payload_memory_length = 16384u;

    // Mesh reading [[payload]] without objectFunction rejected.
    desc.has_object_function = 0;
    desc.payload_memory_length = 0u;
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_DIRECT_MESH_ONLY, &caps_emul, &desc,
                                           NULL, NULL, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_STAGE_LINKAGE);

    // Output vertices exceeding hardware limit (257 > 256) rejected.
    mrefl.mesh.has_payload = 0; mrefl.mesh.has_payload_size = 0; mrefl.mesh.payload_size = 0;
    mesh_info.output_vertices = 257u;
    CHECK(rdna4mesh_validate_stage_linkage(RDNA4MESH_MODE_DIRECT_MESH_ONLY, &caps_emul, &desc,
                                           NULL, NULL, &mrefl, &mesh_info, &frefl, &frag_info,
                                           &linked, &why) == RDNA4MESH_ERR_OUTPUT_LIMIT);
}

static void test_draw_validators_and_tg_memory(void) {
    const uint32_t hw_tg[3] = { 1024u, 1024u, 1024u };
    const char *why = NULL;

    // Threadgroup size validator.
    CHECK(rdna4mesh_validate_tg_size(32, 1, 1, hw_tg, 1024, 0, 32, 1, &why) == RDNA4MESH_OK);
    CHECK(rdna4mesh_validate_tg_size(33, 1, 1, hw_tg, 1024, 0, 32, 1, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    CHECK(rdna4mesh_validate_tg_size(33, 1, 1, hw_tg, 1024, 0, 32, 0, &why) == RDNA4MESH_OK);
    CHECK(rdna4mesh_validate_tg_size(0, 1, 1, hw_tg, 1024, 0, 32, 0, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    CHECK(rdna4mesh_validate_tg_size(32, 32, 2, hw_tg, 1024, 0, 32, 0, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    CHECK(rdna4mesh_validate_tg_size(64, 1, 1, hw_tg, 1024, 32, 32, 0, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);
    CHECK(rdna4mesh_validate_tg_size(UINT64_MAX, 2, 1, hw_tg, 1024, 0, 32, 0, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);

    // Grid validator.
    const uint32_t gmax[3] = { 65535u, 65535u, 65535u };
    int noop = 0; uint64_t total = 0;
    CHECK(rdna4mesh_validate_grid(0, 1, 1, gmax, 4096u, &noop, &total, &why) == RDNA4MESH_OK && noop == 1);
    CHECK(rdna4mesh_validate_grid(4, 2, 2, gmax, 4096u, &noop, &total, &why) == RDNA4MESH_OK && noop == 0 && total == 16u);
    CHECK(rdna4mesh_validate_grid(65536u, 1, 1, gmax, 1000000u, &noop, &total, &why) == RDNA4MESH_ERR_GRID_LIMIT);
    CHECK(rdna4mesh_validate_grid(100, 100, 1, gmax, 4096u, &noop, &total, &why) == RDNA4MESH_ERR_GRID_LIMIT);

    // drawMeshThreads -> threadgroups ceil_div conversion.
    uint64_t groups[3] = {0};
    CHECK(rdna4mesh_threads_to_threadgroups(65, 9, 1, 32, 4, 1, groups, &noop, &why) == RDNA4MESH_OK);
    CHECK(!noop && groups[0] == 3u && groups[1] == 3u && groups[2] == 1u);
    CHECK(rdna4mesh_threads_to_threadgroups(64, 8, 2, 32, 4, 2, groups, &noop, &why) == RDNA4MESH_OK);
    CHECK(!noop && groups[0] == 2u && groups[1] == 2u && groups[2] == 1u);
    CHECK(rdna4mesh_threads_to_threadgroups(0, 8, 1, 32, 4, 1, groups, &noop, &why) == RDNA4MESH_OK && noop == 1);
    CHECK(rdna4mesh_threads_to_threadgroups(64, 8, 1, 0, 4, 1, groups, &noop, &why) == RDNA4MESH_ERR_WORKGROUP_LIMIT);

    // Indirect draw buffer validation (4-byte alignment, >= 12 bytes, uint64 overflow).
    uint64_t vk_off = 0u;
    CHECK(rdna4mesh_validate_indirect_buffer(12u, 0u, 0u, &vk_off, &why) == RDNA4MESH_OK && vk_off == 0u);
    CHECK(rdna4mesh_validate_indirect_buffer(28u, 16u, 256u, &vk_off, &why) == RDNA4MESH_OK && vk_off == 272u);
    CHECK(rdna4mesh_validate_indirect_buffer(28u, 2u, 0u, &vk_off, &why) == RDNA4MESH_ERR_ALIGNMENT);
    CHECK(rdna4mesh_validate_indirect_buffer(28u, 4u, 2u, &vk_off, &why) == RDNA4MESH_ERR_ALIGNMENT);
    CHECK(rdna4mesh_validate_indirect_buffer(11u, 0u, 0u, &vk_off, &why) == RDNA4MESH_ERR_BOUNDS);
    CHECK(rdna4mesh_validate_indirect_buffer(24u, 16u, 0u, &vk_off, &why) == RDNA4MESH_ERR_BOUNDS);
    CHECK(rdna4mesh_validate_indirect_buffer(64u, 16u, UINT64_MAX - 8u, &vk_off, &why) == RDNA4MESH_ERR_OVERFLOW);

    // Object threadgroup memory validation (0..30, 16-byte align, <= 32 KiB total).
    uint64_t tg_mem[RDNA4MESH_MAX_TG_MEM_INDICES] = {0};
    uint64_t tg_tot = 0u;
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 0u, 16384u, 32768u, &tg_tot, &why) == RDNA4MESH_OK && tg_tot == 16384u);
    tg_mem[0] = 16384u;
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 1u, 16384u, 32768u, &tg_tot, &why) == RDNA4MESH_OK && tg_tot == 32768u);
    tg_mem[1] = 16384u;
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 2u, 16u, 32768u, &tg_tot, &why) == RDNA4MESH_ERR_TG_MEM);
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 0u, 0u, 32768u, &tg_tot, &why) == RDNA4MESH_OK && tg_tot == 16384u);
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 3u, 8u, 32768u, &tg_tot, &why) == RDNA4MESH_ERR_ALIGNMENT);
    CHECK(rdna4mesh_validate_tg_memory(tg_mem, 31u, 16u, 32768u, &tg_tot, &why) == RDNA4MESH_ERR_TG_MEM);
}

static void test_emulated_scratch_planner(void) {
    rdna4mesh_emul_plan_t plan;
    const char *why = NULL;

    // 0-grid noop.
    CHECK(rdna4mesh_plan_emulated_draw(0, 1, 1, 16384u, UINT64_C(1) << 30, UINT64_C(1) << 30, 65536u, 64u, &plan, &why) == RDNA4MESH_OK);
    CHECK(plan.is_noop == 1);

    // 4x2x2 = 16 object groups with full 16 KiB payload and 256-byte storage alignment.
    CHECK(rdna4mesh_plan_emulated_draw(4, 2, 2, 16384u, UINT64_C(1) << 30, UINT64_C(1) << 30, 65536u, 256u, &plan, &why) == RDNA4MESH_OK);
    CHECK(!plan.is_noop && plan.total_object_groups == 16u);
    CHECK(plan.grid_params.obj_grid_x == 4u && plan.grid_params.obj_grid_y == 2u &&
          plan.grid_params.obj_grid_z == 2u && plan.grid_params.payload_stride_bytes == 16384u);
    CHECK(plan.grid_params_offset == 0u);
    CHECK(plan.indirect_offset == 256u && plan.indirect_bytes == 192u && plan.indirect_fill_bytes == 192u);
    CHECK(plan.payload_offset == 512u && plan.payload_bytes == 16u * 16384u);
    CHECK(plan.total_scratch_bytes >= plan.payload_offset + plan.payload_bytes);
    CHECK((plan.indirect_offset % 256u) == 0u && (plan.payload_offset % 256u) == 0u);
    // Verify zero region overlap.
    CHECK(plan.grid_params_offset + RDNA4MESH_GRID_PARAMS_BYTES <= plan.indirect_offset);
    CHECK(plan.indirect_offset + plan.indirect_fill_bytes <= plan.payload_offset);

    // Overflow & limit rejections.
    CHECK(rdna4mesh_plan_emulated_draw(0x10000u, 0x10000u, 1u, 16u, UINT64_C(1) << 30, UINT64_C(1) << 30, UINT32_MAX, 64u, &plan, &why) ==
          RDNA4MESH_ERR_OVERFLOW);
    CHECK(rdna4mesh_plan_emulated_draw(UINT64_C(1) << 33, 1, 1, 16u, UINT64_C(1) << 30, UINT64_C(1) << 30, UINT32_MAX, 64u, &plan, &why) ==
          RDNA4MESH_ERR_OVERFLOW);
    CHECK(rdna4mesh_plan_emulated_draw(100, 1, 1, 16u, UINT64_C(1) << 30, UINT64_C(1) << 30, 50u, 64u, &plan, &why) ==
          RDNA4MESH_ERR_GRID_LIMIT);
    CHECK(rdna4mesh_plan_emulated_draw(4, 1, 1, 16384u, 32768u, UINT64_C(1) << 30, 65536u, 64u, &plan, &why) ==
          RDNA4MESH_ERR_BOUNDS);
    CHECK(rdna4mesh_plan_emulated_draw(4, 1, 1, 16384u, UINT64_C(1) << 30, 32768u, 65536u, 64u, &plan, &why) ==
          RDNA4MESH_ERR_BOUNDS);
    CHECK(rdna4mesh_plan_emulated_draw(1, 1, 1, 16385u, UINT64_C(1) << 30, UINT64_C(1) << 30, 65536u, 64u, &plan, &why) ==
          RDNA4MESH_ERR_PAYLOAD_LIMIT);
    CHECK(rdna4mesh_plan_emulated_draw(1, 1, 1, 16u, UINT64_C(1) << 30, UINT64_C(1) << 30, 65536u, 48u, &plan, &why) ==
          RDNA4MESH_ERR_ALIGNMENT);

    // GPU-only indirect emulated planner (full 16 KiB payload stays bounded within 16 MiB budget).
    CHECK(rdna4mesh_plan_emulated_indirect_draw(16384u, UINT64_C(1) << 30, UINT64_C(1) << 30, 65536u, 64u,
                                                RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS, &plan, &why) == RDNA4MESH_OK);
    CHECK(plan.total_object_groups > 0u && plan.total_object_groups <= RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS);
    CHECK(plan.grid_params.payload_stride_bytes == 16384u);
    CHECK(plan.total_scratch_bytes <= RDNA4MESH_EMUL_INDIRECT_BUDGET_BYTES + 256u);

    // Budget too small rejected cleanly.
    CHECK(rdna4mesh_plan_emulated_indirect_draw(16384u, 4096u, 4096u, 65536u, 64u, 1024u, &plan, &why) ==
          RDNA4MESH_ERR_BOUNDS);
}

static size_t spv_push_str(uint32_t *buf, size_t n, const char *s) {
    size_t len = strlen(s) + 1u;
    size_t words = (len + 3u) / 4u;
    for (size_t i = 0u; i < words; i++) {
        uint32_t w = 0u;
        for (size_t b = 0u; b < 4u; b++) {
            size_t idx = i * 4u + b;
            uint32_t ch = idx < len ? (uint32_t)(unsigned char)s[idx] : 0u;
            w |= ch << (b * 8u);
        }
        buf[n++] = w;
    }
    return n;
}

static size_t build_frag_spv(uint32_t *buf, size_t cap,
                             uint32_t spv_version,
                             int with_mesh_cap,
                             int with_other_ext,
                             int with_mesh_ext,
                             int loc1_has_per_prim) {
    size_t n = 0u;
    CHECK(cap >= 192u);
    buf[n++] = RDNA4MESH_SPV_MAGIC;
    buf[n++] = spv_version;
    buf[n++] = 0u;
    buf[n++] = 17u; // id_bound (IDs 1..16)
    buf[n++] = 0u;

    // Section 1: Capabilities
    buf[n++] = (2u << 16) | 17u; // OpCapability Shader
    buf[n++] = 1u;
    if (with_mesh_cap) {
        buf[n++] = (2u << 16) | 17u;
        buf[n++] = RDNA4MESH_SPV_CAP_MESH_SHADING;
    }

    // Section 2: Extensions
    if (with_other_ext) {
        size_t start = n++;
        n = spv_push_str(buf, n, "SPV_KHR_storage_buffer_storage_class");
        buf[start] = ((uint32_t)(n - start) << 16) | 10u;
    }
    if (with_mesh_ext) {
        size_t start = n++;
        n = spv_push_str(buf, n, "SPV_EXT_mesh_shader");
        buf[start] = ((uint32_t)(n - start) << 16) | 10u;
    }

    // Section 4: MemoryModel Logical GLSL450
    buf[n++] = (3u << 16) | 14u;
    buf[n++] = 0u;
    buf[n++] = 1u;

    // Section 5: OpEntryPoint Fragment %2 "frag_per_prim" %10 %11 %12 %13 %14 %15
    {
        size_t start = n++;
        buf[n++] = RDNA4MESH_SPV_EM_FRAGMENT;
        buf[n++] = 2u;
        n = spv_push_str(buf, n, "frag_per_prim");
        buf[n++] = 10u;
        buf[n++] = 11u;
        buf[n++] = 12u;
        buf[n++] = 13u;
        buf[n++] = 14u;
        buf[n++] = 15u;
        buf[start] = ((uint32_t)(n - start) << 16) | 15u;
    }

    // Section 6: OpExecutionMode %2 OriginUpperLeft (7)
    buf[n++] = (3u << 16) | 16u;
    buf[n++] = 2u;
    buf[n++] = 7u;

    // Section 8: Annotations
    // %10: Input Location 0 (per-vertex)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 10u; buf[n++] = RDNA4MESH_SPV_DEC_LOCATION; buf[n++] = 0u;
    // %11: Input Location 1 + Flat (paired with mesh primitive output)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 11u; buf[n++] = RDNA4MESH_SPV_DEC_LOCATION; buf[n++] = 1u;
    buf[n++] = (3u << 16) | 71u; buf[n++] = 11u; buf[n++] = RDNA4MESH_SPV_DEC_FLAT;
    if (loc1_has_per_prim) {
        buf[n++] = (3u << 16) | 71u; buf[n++] = 11u; buf[n++] = RDNA4MESH_SPV_DEC_PER_PRIMITIVE;
    }
    // %12: Input Location 3 + Flat (second primitive output)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 12u; buf[n++] = RDNA4MESH_SPV_DEC_LOCATION; buf[n++] = 3u;
    buf[n++] = (3u << 16) | 71u; buf[n++] = 12u; buf[n++] = RDNA4MESH_SPV_DEC_FLAT;
    // %13: Input BuiltIn FragCoord (15)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 13u; buf[n++] = RDNA4MESH_SPV_DEC_BUILTIN; buf[n++] = 15u;
    // %14: Output Location 0 (color attachment 0)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 14u; buf[n++] = RDNA4MESH_SPV_DEC_LOCATION; buf[n++] = 0u;
    // %15: Output Location 1 (color attachment 1 - same Location 1 as %11, must NEVER get PerPrimitiveEXT)
    buf[n++] = (4u << 16) | 71u; buf[n++] = 15u; buf[n++] = RDNA4MESH_SPV_DEC_LOCATION; buf[n++] = 1u;

    // Section 9: Types & Global Variables
    buf[n++] = (2u << 16) | 19u; buf[n++] = 3u;                              // OpTypeVoid %3
    buf[n++] = (3u << 16) | 33u; buf[n++] = 4u; buf[n++] = 3u;               // OpTypeFunction %4 %3
    buf[n++] = (3u << 16) | 22u; buf[n++] = 5u; buf[n++] = 32u;              // OpTypeFloat %5 32
    buf[n++] = (4u << 16) | 23u; buf[n++] = 6u; buf[n++] = 5u; buf[n++] = 4u;// OpTypeVector %6 %5 4
    buf[n++] = (4u << 16) | 32u; buf[n++] = 7u; buf[n++] = RDNA4MESH_SPV_SC_INPUT;  buf[n++] = 6u;
    buf[n++] = (4u << 16) | 32u; buf[n++] = 8u; buf[n++] = RDNA4MESH_SPV_SC_OUTPUT; buf[n++] = 6u;

    buf[n++] = (4u << 16) | 59u; buf[n++] = 7u; buf[n++] = 10u; buf[n++] = RDNA4MESH_SPV_SC_INPUT;
    buf[n++] = (4u << 16) | 59u; buf[n++] = 7u; buf[n++] = 11u; buf[n++] = RDNA4MESH_SPV_SC_INPUT;
    buf[n++] = (4u << 16) | 59u; buf[n++] = 7u; buf[n++] = 12u; buf[n++] = RDNA4MESH_SPV_SC_INPUT;
    buf[n++] = (4u << 16) | 59u; buf[n++] = 7u; buf[n++] = 13u; buf[n++] = RDNA4MESH_SPV_SC_INPUT;
    buf[n++] = (4u << 16) | 59u; buf[n++] = 8u; buf[n++] = 14u; buf[n++] = RDNA4MESH_SPV_SC_OUTPUT;
    buf[n++] = (4u << 16) | 59u; buf[n++] = 8u; buf[n++] = 15u; buf[n++] = RDNA4MESH_SPV_SC_OUTPUT;

    // Section 10: Function definition
    buf[n++] = (5u << 16) | 54u; buf[n++] = 3u; buf[n++] = 2u; buf[n++] = 0u; buf[n++] = 4u; // OpFunction
    buf[n++] = (2u << 16) | 248u; buf[n++] = 16u;                                            // OpLabel %16
    buf[n++] = (1u << 16) | 253u;                                                            // OpReturn
    buf[n++] = (1u << 16) | 56u;                                                             // OpFunctionEnd

    CHECK(n <= cap);
    return n;
}

typedef struct {
    unsigned mesh_caps;
    unsigned mesh_exts;
    unsigned other_exts;
    unsigned flat_on[17];
    unsigned per_prim_on[17];
} frag_patch_audit_t;

static void verify_patched_frag_spv(const uint32_t *w, size_t nw,
                                    uint32_t expected_bound,
                                    frag_patch_audit_t *audit) {
    memset(audit, 0, sizeof *audit);
    CHECK(w != NULL && nw >= 5u);
    CHECK(w[0] == RDNA4MESH_SPV_MAGIC);
    CHECK(w[3] == expected_bound);

    unsigned section = 0u;
    for (size_t p = 5u; p < nw;) {
        uint32_t op = w[p] & 0xFFFFu;
        uint32_t wc = w[p] >> 16;
        CHECK(wc > 0u && (size_t)wc <= nw - p);
        const uint32_t *a = w + p + 1u;

        unsigned cur_sec = section;
        if (op == 17u) cur_sec = 1u;
        else if (op == 10u) cur_sec = 2u;
        else if (op == 11u || op == 14u || op == 15u || op == 16u) cur_sec = 3u;
        else if (rdna4mesh_spv_is_annotation_op(op)) cur_sec = 4u;
        else if (op == 54u || op == 56u || op == 248u || op == 253u) cur_sec = 6u;
        else cur_sec = 5u;
        CHECK(cur_sec >= section);
        section = cur_sec;

        if (op == 17u && wc == 2u && a[0] == RDNA4MESH_SPV_CAP_MESH_SHADING) {
            audit->mesh_caps++;
        } else if (op == 10u && wc >= 2u) {
            char name[64];
            size_t len = 0u;
            for (uint32_t wi = 0u; wi < wc - 1u && len + 1u < sizeof name; wi++) {
                uint32_t word = a[wi];
                for (unsigned b = 0u; b < 4u && len + 1u < sizeof name; b++) {
                    char ch = (char)((word >> (b * 8u)) & 0xFFu);
                    if (!ch) { wi = wc; break; }
                    name[len++] = ch;
                }
            }
            name[len] = '\0';
            if (strcmp(name, "SPV_EXT_mesh_shader") == 0) audit->mesh_exts++;
            else if (strcmp(name, "SPV_KHR_storage_buffer_storage_class") == 0) audit->other_exts++;
        } else if (op == 71u && wc >= 3u && a[0] < 17u) {
            if (a[1] == RDNA4MESH_SPV_DEC_PER_PRIMITIVE) audit->per_prim_on[a[0]]++;
            else if (a[1] == RDNA4MESH_SPV_DEC_FLAT) audit->flat_on[a[0]]++;
        }
        p += (size_t)wc;
    }

#ifdef RDNA4_TEST_HAS_VULKAN
    RDNA4sMod mod;
    RDNA4sRefl refl;
    char why[256] = "";
    CHECK(rdna4s_parse(w, nw, &mod, why, sizeof why) == 0);
    CHECK(mod.bound == expected_bound);
    CHECK(mod.has_ep && mod.ep_model == RDNA4MESH_SPV_EM_FRAGMENT);
    CHECK(strcmp(mod.ep_name, "frag_per_prim") == 0);
    CHECK(mod.id[10].loc == 0 && mod.id[11].loc == 1 && mod.id[12].loc == 3);
    CHECK(mod.id[13].builtin == 15);
    CHECK(rdna4s_reflect(&mod, 1, &refl) == 0);
    CHECK(refl.nout == 2 && refl.outmask == 0x3u);
    rdna4s_free(&mod);
#endif
}

static void test_frag_per_primitive_patcher(void) {
    uint32_t spv[192];
    size_t nw = build_frag_spv(spv, 192u, 0x00010300u, 0, 0, 0, 0);
    uint32_t *out = NULL;
    size_t out_nw = 0u;
    frag_patch_audit_t audit;
    rdna4mesh_spv_info_t info;

    // 1. No-op when per_prim_loc_mask == 0.
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, 0u, &out, &out_nw) == RDNA4MESH_OK);
    CHECK(out == NULL && out_nw == 0u);

    // 2. No-op when mask targets locations not read as fragment Inputs.
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, (UINT64_C(1) << 2) | (UINT64_C(1) << 7),
                                              &out, &out_nw) == RDNA4MESH_OK);
    CHECK(out == NULL && out_nw == 0u);

    // 3. Single PerPrimitiveEXT injection at Location 1 (SPIR-V 1.3 -> 1.4, Flat preserved, Output %15 untouched).
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_OK);
    CHECK(out != NULL && out_nw == nw + 11u); // +2 cap + 6 ext + 3 dec
    CHECK(out[1] == RDNA4MESH_SPV_VERSION_1_4);
    verify_patched_frag_spv(out, out_nw, 17u, &audit);
    CHECK(audit.mesh_caps == 1u && audit.mesh_exts == 1u && audit.other_exts == 0u);
    CHECK(audit.per_prim_on[11] == 1u && audit.flat_on[11] == 1u);
    CHECK(audit.per_prim_on[10] == 0u && audit.per_prim_on[12] == 0u &&
          audit.per_prim_on[13] == 0u && audit.per_prim_on[14] == 0u && audit.per_prim_on[15] == 0u);
    CHECK(rdna4mesh_inspect_spv(out, out_nw, &info) == RDNA4MESH_OK);
    CHECK(info.has_mesh_shading_cap && info.has_mesh_shading_ext);
    CHECK(info.per_prim_in_mask == (UINT64_C(1) << 1) && info.per_prim_out_mask == 0u);

    // 4. Idempotence: re-patching already-patched SPIR-V with same mask is a no-op.
    uint32_t *out2 = NULL;
    size_t out2_nw = 0u;
    CHECK(rdna4_mesh_patch_frag_per_primitive(out, out_nw, UINT64_C(1) << 1, &out2, &out2_nw) == RDNA4MESH_OK);
    CHECK(out2 == NULL && out2_nw == 0u);

    // 5. Incremental injection: cap+ext+loc1 already present -> adds ONLY +3 words for loc3.
    CHECK(rdna4_mesh_patch_frag_per_primitive(out, out_nw, (UINT64_C(1) << 1) | (UINT64_C(1) << 3),
                                              &out2, &out2_nw) == RDNA4MESH_OK);
    CHECK(out2 != NULL && out2_nw == out_nw + 3u);
    verify_patched_frag_spv(out2, out2_nw, 17u, &audit);
    CHECK(audit.mesh_caps == 1u && audit.mesh_exts == 1u);
    CHECK(audit.per_prim_on[11] == 1u && audit.per_prim_on[12] == 1u);
    CHECK(audit.per_prim_on[10] == 0u && audit.per_prim_on[15] == 0u);
    CHECK(rdna4mesh_inspect_spv(out2, out2_nw, &info) == RDNA4MESH_OK);
    CHECK(info.per_prim_in_mask == ((UINT64_C(1) << 1) | (UINT64_C(1) << 3)));
    free(out2);
    free(out);
    out = out2 = NULL;

    // 6. Multi-location injection on SPIR-V 1.5 preserving version and pre-existing OpExtension.
    nw = build_frag_spv(spv, 192u, 0x00010500u, 0, 1, 0, 0);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, (UINT64_C(1) << 1) | (UINT64_C(1) << 3),
                                              &out, &out_nw) == RDNA4MESH_OK);
    CHECK(out != NULL && out_nw == nw + 14u); // +2 cap + 6 ext + 2*3 dec
    CHECK(out[1] == 0x00010500u);
    verify_patched_frag_spv(out, out_nw, 17u, &audit);
    CHECK(audit.mesh_caps == 1u && audit.mesh_exts == 1u && audit.other_exts == 1u);
    CHECK(audit.per_prim_on[11] == 1u && audit.per_prim_on[12] == 1u &&
          audit.flat_on[11] == 1u && audit.flat_on[12] == 1u);
    CHECK(audit.per_prim_on[10] == 0u && audit.per_prim_on[15] == 0u);
    free(out);
    out = NULL;

    // 7. Already capable, extended, and decorated fragment shader -> no-op.
    nw = build_frag_spv(spv, 192u, 0x00010400u, 1, 1, 1, 1);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_OK);
    CHECK(out == NULL && out_nw == 0u);

    // 8. MeshEXT SPIR-V per_prim_out_mask detection in rdna4mesh_inspect_spv.
    {
        uint32_t mspv[96];
        size_t mn = 0u;
        mspv[mn++] = RDNA4MESH_SPV_MAGIC;
        mspv[mn++] = RDNA4MESH_SPV_VERSION_1_4;
        mspv[mn++] = 0u;
        mspv[mn++] = 12u;
        mspv[mn++] = 0u;
        mspv[mn++] = (2u << 16) | 17u; mspv[mn++] = RDNA4MESH_SPV_CAP_MESH_SHADING;
        {
            size_t s = mn++;
            mn = spv_push_str(mspv, mn, "SPV_EXT_mesh_shader");
            mspv[s] = ((uint32_t)(mn - s) << 16) | 10u;
        }
        mspv[mn++] = (5u << 16) | 15u; mspv[mn++] = RDNA4MESH_SPV_EM_MESH_EXT; mspv[mn++] = 1u;
        mspv[mn++] = 0x6E69616Du; mspv[mn++] = 0u;
        mspv[mn++] = (4u << 16) | 71u; mspv[mn++] = 10u; mspv[mn++] = RDNA4MESH_SPV_DEC_LOCATION; mspv[mn++] = 0u;
        mspv[mn++] = (4u << 16) | 71u; mspv[mn++] = 11u; mspv[mn++] = RDNA4MESH_SPV_DEC_LOCATION; mspv[mn++] = 1u;
        mspv[mn++] = (3u << 16) | 71u; mspv[mn++] = 11u; mspv[mn++] = RDNA4MESH_SPV_DEC_PER_PRIMITIVE;
        mspv[mn++] = (4u << 16) | 59u; mspv[mn++] = 7u; mspv[mn++] = 10u; mspv[mn++] = RDNA4MESH_SPV_SC_OUTPUT;
        mspv[mn++] = (4u << 16) | 59u; mspv[mn++] = 7u; mspv[mn++] = 11u; mspv[mn++] = RDNA4MESH_SPV_SC_OUTPUT;
        CHECK(rdna4mesh_inspect_spv(mspv, mn, &info) == RDNA4MESH_OK);
        CHECK(info.ep_model == RDNA4MESH_SPV_EM_MESH_EXT && info.has_mesh_shading_cap && info.has_mesh_shading_ext);
        CHECK(info.per_prim_out_mask == (UINT64_C(1) << 1) && info.per_prim_in_mask == 0u);
        // Reject non-Fragment entry point in fragment patcher.
        CHECK(rdna4_mesh_patch_frag_per_primitive(mspv, mn, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);
        CHECK(out == NULL && out_nw == 0u);
    }

    // 9. Malformed / truncated / out-of-order SPIR-V rejection.
    nw = build_frag_spv(spv, 192u, 0x00010400u, 0, 0, 0, 0);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, 1u, NULL, &out_nw) == RDNA4MESH_ERR_INVALID_ARG);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw, 1u, &out, NULL) == RDNA4MESH_ERR_INVALID_ARG);
    CHECK(rdna4_mesh_patch_frag_per_primitive(NULL, nw, 1u, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, 4u, 1u, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);
    CHECK(rdna4_mesh_patch_frag_per_primitive(spv, nw - 1u, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);
    {
        uint32_t bad[192];
        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[0] = 0xDEADBEEFu;
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[3] = 0u; // zero id_bound
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[5] = 0u; // wc == 0
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[5] = (999u << 16) | 17u; // truncated
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        // Out-of-order OpCapability appended at end.
        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[nw] = (2u << 16) | 17u;
        bad[nw + 1u] = 1u;
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw + 2u, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        // Out-of-order OpDecorate appended after OpVariable/OpFunction.
        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[nw] = (3u << 16) | 71u;
        bad[nw + 1u] = 11u;
        bad[nw + 2u] = RDNA4MESH_SPV_DEC_FLAT;
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw + 3u, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);

        // Out-of-range id_bound (Input OpVariable ID 15 >= bound 12).
        memcpy(bad, spv, nw * sizeof(uint32_t));
        bad[3] = 12u;
        CHECK(rdna4_mesh_patch_frag_per_primitive(bad, nw, UINT64_C(1) << 1, &out, &out_nw) == RDNA4MESH_ERR_SPIRV);
        CHECK(out == NULL && out_nw == 0u);
    }
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    return buf;
}

static void test_source_pins(void) {
    char *impl = slurp_file("src/garnet-metal/garnet_mesh_rt.m");
    if (!impl) impl = slurp_file("garnet_mesh_rt.m");
    CHECK(impl != NULL);

    const char *impl_pins[] = {
        "RDNA4MeshRenderPipelineState",
        "vkCmdDrawMeshTasksEXT",
        "vkCmdDrawMeshTasksIndirectEXT",
        "GARNETX_CAP_MESH_EMULATED_PAYLOAD",
        "GARNETX_MESH_GRID_PARAMS_BINDING",
        "GARNETX_MESH_PAYLOAD_BINDING",
        "GARNETX_MESH_INDIRECT_BINDING",
        "garnetx_translate_stage3",
        "setObjectBuffer:",
        "setObjectBytes:",
        "setObjectTexture:",
        "setObjectSamplerState:",
        "setObjectThreadgroupMemoryLength:",
        "setMeshBuffer:",
        "setMeshBytes:",
        "setMeshTexture:",
        "setMeshSamplerState:",
        "drawMeshThreadgroups:",
        "drawMeshThreads:",
        "drawMeshThreadgroupsWithIndirectBuffer:",
        "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT",
        "VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT",
        "VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT",
        "RDNA4MESH_MODE_EMULATED_OBJECT_MESH",
        "RDNA4MESH_MODE_NATIVE_TASK_MESH",
        "RDNA4_HAS_MULTI_DRAW_INDIRECT",
        "rdna4_mesh_max_draw_indirect_count",
        "rdna4_mesh_cift_fn",
        "rdna4_mesh_translate_stage3_fn",
        "rdna4_mesh_patch_frag_per_primitive",
        "Wave 3E integration checklist",
    };
    for (size_t i = 0; i < sizeof(impl_pins) / sizeof(impl_pins[0]); i++) {
        CHECK(strstr(impl, impl_pins[i]) != NULL);
    }
    // Regression pins: no 656 KiB stack RDNA4sRefl, no unprefixed Stage 3 typedef collisions in mesh section.
    const char *mesh_sec = strstr(impl, "RDNA4_MESH_IMPL_INC");
    CHECK(mesh_sec != NULL);
    CHECK(strstr(mesh_sec, "RDNA4sRefl userR;") == NULL);
    CHECK(strstr(mesh_sec, "rdna4x_cift_fn") == NULL);
    CHECK(strstr(mesh_sec, "rdna4x_translate_stage3_fn") == NULL);
    free(impl);

    char *sr = slurp_file("src/garnet-metal/garnet_shaders.m");
    if (!sr) sr = slurp_file("garnet_shaders.m");
    CHECK(sr != NULL);
    CHECK(strstr(sr, "MTLFunctionTypeIntersection") != NULL);
    CHECK(strstr(sr, "RDNA4RTR_MTL_FN_TYPE_INTERSECTION") == NULL);
    free(sr);

    char *live = slurp_file("src/garnet-metal/tests/test-metal3-mesh-shaders.m");
    if (!live) live = slurp_file("test-metal3-mesh-shaders.m");
    CHECK(live != NULL);
    const char *live_pins[] = {
        "createGarnetDevice",
        "GarnetAccelerator",
        "MTLMeshRenderPipelineDescriptor",
        "drawMeshThreadgroups:",
        "drawMeshThreads:",
        "drawMeshThreadgroupsWithIndirectBuffer:",
        "setObjectThreadgroupMemoryLength:",
        "16384",
        "privateFunctions",
        "@available(macOS 14.0, *)",
        "MTLGPUFamilyApple6",
        "respondsToSelector:@selector(newRenderPipelineStateWithMeshDescriptor:options:reflection:error:)",
        "METAL3_MESH_PASS",
    };
    for (size_t i = 0; i < sizeof(live_pins) / sizeof(live_pins[0]); i++) {
        CHECK(strstr(live, live_pins[i]) != NULL);
    }
    CHECK(strstr(live, "sudo RDNA4M_ALLOW") == NULL);
    free(live);
}

int main(void) {
    test_locked_abi_and_payload_strides();
    test_mode_selection_and_caps();
    test_footnote10_and_descriptor_validation();
    test_reflection_json_parser();
    test_spv_inspector_and_stage_linkage();
    test_draw_validators_and_tg_memory();
    test_emulated_scratch_planner();
    test_frag_per_primitive_patcher();
    test_source_pins();
    printf("test-mesh-runtime: OK (%d checks)\n", g_checks);
    return 0;
}
