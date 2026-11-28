// Host ASan+UBSan unit + contract test for garnet_gpl.h and GPL + EDS1/2/3 wiring.
#include "garnet_gpl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;

static void check(int cond, const char *msg) {
    g_checks++;
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_fails++;
    }
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    buf[r] = '\0';
    fclose(f);
    return buf;
}

int main(void) {
    // 1. Topology class folding
    check(garnet_gpl_topo_class(GARNET_VK_TOPO_POINT_LIST) == GARNET_VK_TOPO_POINT_LIST, "POINT_LIST stays POINT");
    check(garnet_gpl_topo_class(GARNET_VK_TOPO_LINE_LIST) == GARNET_VK_TOPO_LINE_LIST, "LINE_LIST -> LINE class");
    check(garnet_gpl_topo_class(GARNET_VK_TOPO_LINE_STRIP) == GARNET_VK_TOPO_LINE_LIST, "LINE_STRIP folds to LINE class");
    check(garnet_gpl_topo_class(GARNET_VK_TOPO_TRIANGLE_LIST) == GARNET_VK_TOPO_TRIANGLE_LIST, "TRIANGLE_LIST -> TRIANGLE class");
    check(garnet_gpl_topo_class(GARNET_VK_TOPO_TRIANGLE_STRIP) == GARNET_VK_TOPO_TRIANGLE_LIST, "TRIANGLE_STRIP folds to TRIANGLE class");
    check(garnet_gpl_topo_class(10u) == 10u, "PATCH_LIST passes through");
    check(garnet_gpl_is_strip_topo(GARNET_VK_TOPO_LINE_STRIP) && garnet_gpl_is_strip_topo(GARNET_VK_TOPO_TRIANGLE_STRIP), "strip detection true for strips");
    check(!garnet_gpl_is_strip_topo(GARNET_VK_TOPO_LINE_LIST) && !garnet_gpl_is_strip_topo(GARNET_VK_TOPO_TRIANGLE_LIST), "strip detection false for lists");

    // 2. Eligibility matrix
    check(garnet_gpl_eligible(1, 1, 0, 0, 0, 0, 0, 0, 0) == 1, "standard render PSO eligible for GPL");
    check(garnet_gpl_eligible(0, 1, 0, 0, 0, 0, 0, 0, 0) == 0, "gpl disabled -> ineligible");
    check(garnet_gpl_eligible(1, 0, 0, 0, 0, 0, 0, 0, 0) == 0, "vertex-only (no frag) -> ineligible");
    check(garnet_gpl_eligible(1, 1, 1, 0, 0, 0, 0, 0, 0) == 0, "tessellation -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 1, 0, 0, 0, 0, 0) == 0, "ICB -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 0, 1, 0, 0, 0, 0) == 0, "fallback placeholder -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 0, 0, 1, 0, 0, 0) == 0, "rasterizer discard -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 0, 0, 0, 1, 0, 0) == 0, "GPU argument tables -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 0, 0, 0, 0, 1, 0) == 0, "binary archive cache scope -> ineligible");
    check(garnet_gpl_eligible(1, 1, 0, 0, 0, 0, 0, 0, 1) == 0, "kill switch -> ineligible");

    // 3. Variant key folding
    uint64_t k_tri0 = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 0u, 1u, 0u, 0u, 1, 1, 1);
    uint64_t k_tri1 = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_STRIP, 2u, 0u, 0x12345u, 3u, 1, 1, 1);
    check(k_tri0 == k_tri1, "GPL folds cull/front/ds/fillMode/depthClamp and TRIANGLE_LIST<->STRIP into one key");

    uint64_t k_line0 = garnet_gpl_variant_key(GARNET_VK_TOPO_LINE_LIST, 0u, 1u, 0u, 0u, 1, 1, 1);
    uint64_t k_line1 = garnet_gpl_variant_key(GARNET_VK_TOPO_LINE_STRIP, 1u, 0u, 0x99u, 2u, 1, 1, 1);
    check(k_line0 == k_line1 && k_line0 != k_tri0, "GPL separates LINE class from TRIANGLE class and folds LINE_LIST<->STRIP");

    uint64_t k_vp2 = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 0u, 1u, 0u, (1u << 3), 1, 1, 1);
    check(k_vp2 != k_tri0, "GPL preserves viewportCount > 1 in variant key");

    uint64_t k_sl = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 2u, 1u, 7u, 4u, 1, 1, 1);
    uint64_t k_sl_mono = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 2u, 1u, 7u, 4u, 0, 1, 1);
    check(k_sl == k_sl_mono, "programmable sample locations falls back to full monolithic key");

    uint64_t k_nopoly = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 0u, 0u, 0u, 1u, 1, 0, 1);
    check(k_nopoly != k_tri0, "without EDS3 polygonMode, fillLine stays in variant key");

    uint64_t k_noclamp = garnet_gpl_variant_key(GARNET_VK_TOPO_TRIANGLE_LIST, 0u, 0u, 0u, 2u, 1, 1, 0);
    check(k_noclamp != k_tri0, "without EDS3 depthClamp, depthClamp stays in variant key");

    // Monolithic key exact bit layout
    uint64_t km = garnet_gpl_variant_key(3u, 2u, 1u, 0xABCDu, 0x15u, 0, 1, 1);
    uint64_t km_exp = 3ull | (2ull << 8) | (1ull << 16) | (0xABCDull << 24) | (0x15ull << 56);
    check(km == km_exp, "monolithic key matches exact legacy layout");

    // 4. Source contract checks
    char *be = slurp("garnet_backend.m");
    char *pi = slurp("garnet_pipelines.m");
    char *cm = slurp("garnet_render_encoder.m");
    check(be && pi && cm, "source files readable");
    if (be && pi && cm) {
        check(strstr(be, "VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME") != NULL &&
              strstr(be, "VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME") != NULL &&
              strstr(be, "VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME") != NULL,
              "garnet_backend.m queries and enables GPL + EDS3 extensions");
        check(strstr(be, "GARNET_NOGPL_FILE") != NULL &&
              strstr(be, "rdna4_gpl_prewarm_start();") != NULL &&
              strstr(be, "seeded read-only from") != NULL,
              "garnet_backend.m honors /private/tmp/garnet-nogpl, triggers pre-warm, and seeds cold app caches read-only");
        check(strstr(pi, "VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT") != NULL &&
              strstr(pi, "VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT") != NULL &&
              strstr(pi, "VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT") != NULL &&
              strstr(pi, "VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT") != NULL &&
              strstr(pi, "VK_PIPELINE_CREATE_LIBRARY_BIT_KHR") != NULL &&
              strstr(pi, "VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT") == NULL,
              "garnet_pipelines.m uses 3-library GPL fast-link without LTO");
        check(strstr(pi, "6cc985fd81b32b264203827cb3f7a8c6d9216d7a7f2b23707b18adeee36898ec") != NULL &&
              strstr(pi, "0fca3c0b863929837cced752a6d4f37e7271ee055415c1937586c33184306f5c") != NULL &&
              strstr(pi, "b4d554ed376a3f6f4bb279d1da1672958aaed902c73312d788ddb99283c29e70") != NULL &&
              strstr(pi, "0fddae4cfb947d265b833f525390d8eb9efb806d99d54a83f5af8248dae6cc3c") != NULL &&
              strstr(pi, "rdna4_pc_save(\"prewarm\")") != NULL,
              "garnet_pipelines.m pre-warms all 4 giant spvcache shaders and persists cache on miss");
        check(strstr(cm, "RDNA4R.cmdSetCullMode(cmd, cull)") != NULL &&
              strstr(cm, "RDNA4R.cmdSetFrontFace(cmd, front)") != NULL &&
              strstr(cm, "RDNA4R.cmdSetPrimitiveTopology(cmd, topo)") != NULL &&
              strstr(cm, "RDNA4R.cmdSetPrimitiveRestartEnable(cmd,") != NULL &&
              strstr(cm, "RDNA4R.cmdSetPolygonMode(cmd,") != NULL &&
              strstr(cm, "RDNA4R.cmdSetDepthClampEnable(cmd,") != NULL &&
              strstr(cm, "RDNA4R.cmdSetDepthTestEnable(cmd,") != NULL &&
              strstr(cm, "RDNA4R.cmdSetStencilOp(cmd,") != NULL,
              "garnet_commands.m emits all Vulkan 1.3 + EDS3 dynamic states for GPL pipelines");
        check(strstr(cm, "_dynDirty |= GARNET_DYN_RAST") != NULL &&
              strstr(cm, "GARNET_DYN_STENCIL_M | GARNET_DYN_DS_OP") != NULL,
              "garnet_commands.m dirties raster and depth/stencil dynamic state in setters");
    }
    free(be); free(pi); free(cm);

    printf("test-gpl: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
