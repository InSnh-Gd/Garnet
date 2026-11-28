// test-sparse-metal.c: pure-C host unit tests + Vulkan 1.4 struct checks + source pins
// for Task 5.3 (Metal 64 KiB sparse tile geometry, pixel<->tile conversions, mip-tail
// calculation, tile pool, atomic bind/unbind batch planner, and negative/overflow checks).
// Build/run:
//   cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal -o "$TMPDIR/test-sparse-metal" src/garnet-metal/test-sparse-metal.c
//   "$TMPDIR/test-sparse-metal" [src-dir]
// Optional Vulkan 1.4 header verification:
//   cc -std=c11 -D_GNU_SOURCE -DRDNA4_SPARSE_TEST_VK -DVK_NO_PROTOTYPES -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Werror -Isrc/garnet-metal -I"$VK_INC" -o "$TMPDIR/test-sparse-metal-vk" src/garnet-metal/test-sparse-metal.c
#ifdef RDNA4_SPARSE_TEST_VK
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_gate.h"
#include "garnet_sparse.h"

static int fails, checks;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    checks++; \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

static char *slurp(const char *dir, const char *name) {
    char p[1024];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof p) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)calloc(1, (size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

static rdna4dc_metal3_caps make_sparse_caps(int has2d, int has3d, int ms2, int ms4, int ms8) {
    rdna4dc_metal3_probe p = {
        .sparse_queue = 1,
        .sparse_bind_func = 1,
        .sparse_binding = 1,
        .sparse_residency_buffer = 1,
        .sparse_residency_image_2d = has2d,
        .sparse_residency_image_3d = has3d,
        .sparse_residency_2_samples = ms2,
        .sparse_residency_4_samples = ms4,
        .sparse_residency_8_samples = ms8,
        .sparse_residency_aliased = 1,
    };
    return rdna4dc_metal3_from_probe(&p);
}

static void test_tile_geometry_and_gating(void) {
    // Verify 64 KiB byte invariant across 8/16/32/64/128-bpp, BC1..BC7, 2D/3D, and 2x/4x/8x MSAA.
    const uint32_t uncomp_fmts[] = {
        RDNA4SP_PF_A8Unorm, RDNA4SP_PF_R8Unorm, RDNA4SP_PF_R8Sint, RDNA4SP_PF_Stencil8,
        RDNA4SP_PF_R16Unorm, RDNA4SP_PF_R16Float, RDNA4SP_PF_RG8Unorm, RDNA4SP_PF_Depth16Unorm,
        RDNA4SP_PF_R32Float, RDNA4SP_PF_RG16Uint, RDNA4SP_PF_RGBA8Unorm, RDNA4SP_PF_BGRA8Unorm_sRGB,
        RDNA4SP_PF_RGB10A2Unorm, RDNA4SP_PF_RG11B10Float, RDNA4SP_PF_RGB9E5Float, RDNA4SP_PF_Depth32Float,
        RDNA4SP_PF_RG32Float, RDNA4SP_PF_RGBA16Unorm, RDNA4SP_PF_RGBA16Float,
        RDNA4SP_PF_RGBA32Uint, RDNA4SP_PF_RGBA32Sint, RDNA4SP_PF_RGBA32Float,
    };
    const uint32_t bc_fmts[] = {
        RDNA4SP_PF_BC1_RGBA, RDNA4SP_PF_BC1_RGBA_sRGB, RDNA4SP_PF_BC4_RUnorm, RDNA4SP_PF_BC4_RSnorm,
        RDNA4SP_PF_BC2_RGBA, RDNA4SP_PF_BC2_RGBA_sRGB, RDNA4SP_PF_BC3_RGBA, RDNA4SP_PF_BC3_RGBA_sRGB,
        RDNA4SP_PF_BC5_RGUnorm, RDNA4SP_PF_BC5_RGSnorm, RDNA4SP_PF_BC6H_RGBFloat, RDNA4SP_PF_BC6H_RGBUfloat,
        RDNA4SP_PF_BC7_RGBAUnorm, RDNA4SP_PF_BC7_RGBAUnorm_sRGB,
    };
    const uint64_t types2d[] = {
        RDNA4TD_MTL_2D, RDNA4TD_MTL_2DARRAY, RDNA4TD_MTL_CUBE, RDNA4TD_MTL_CUBEARRAY
    };

    int all_uncomp_64k = 1;
    for (size_t i = 0; i < sizeof(uncomp_fmts) / sizeof(uncomp_fmts[0]); i++) {
        const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup(uncomp_fmts[i]);
        if (!fi) { all_uncomp_64k = 0; break; }
        for (size_t t = 0; t < 4u; t++) {
            rdna4sp_size sz;
            if (!rdna4sp_tile_size_for_format(types2d[t], uncomp_fmts[i], 1u, &sz) ||
                sz.width * sz.height * sz.depth * fi->block_bytes != RDNA4SP_TILE_BYTES) {
                all_uncomp_64k = 0;
            }
        }
        // 3D (color only; DS cannot be 3D)
        rdna4sp_size sz3;
        int ok3 = rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, uncomp_fmts[i], 1u, &sz3);
        if (fi->is_ds) {
            if (ok3 || sz3.width || sz3.height || sz3.depth) all_uncomp_64k = 0;
        } else {
            if (!ok3 || sz3.width * sz3.height * sz3.depth * fi->block_bytes != RDNA4SP_TILE_BYTES)
                all_uncomp_64k = 0;
        }
        // MSAA 2x/4x/8x (both 2DMS and 2DMSArray)
        const uint64_t samples[] = { 2u, 4u, 8u };
        const uint64_t mstypes[] = { RDNA4TD_MTL_2DMS, RDNA4TD_MTL_2DMSARRAY };
        for (size_t si = 0; si < 3u; si++) {
            for (size_t mt = 0; mt < 2u; mt++) {
                rdna4sp_size msz;
                if (!rdna4sp_tile_size_for_format(mstypes[mt], uncomp_fmts[i], samples[si], &msz) ||
                    msz.width * msz.height * msz.depth * fi->block_bytes * samples[si] != RDNA4SP_TILE_BYTES) {
                    all_uncomp_64k = 0;
                }
            }
        }
    }
    CHECK("64 KiB invariant across all uncompressed 8..128-bpp x 2D/Array/Cube/3D/2x/4x/8x MSAA",
          all_uncomp_64k, "every accepted tile equals 65536 B");

    int all_bc_64k = 1;
    for (size_t i = 0; i < sizeof(bc_fmts) / sizeof(bc_fmts[0]); i++) {
        const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup(bc_fmts[i]);
        if (!fi || !fi->is_bc || fi->block_w != 4u || fi->block_h != 4u) { all_bc_64k = 0; break; }
        for (size_t t = 0; t < 4u; t++) {
            rdna4sp_size sz;
            if (!rdna4sp_tile_size_for_format(types2d[t], bc_fmts[i], 1u, &sz) ||
                (sz.width % 4u) != 0u || (sz.height % 4u) != 0u ||
                (sz.width / 4u) * (sz.height / 4u) * sz.depth * fi->block_bytes != RDNA4SP_TILE_BYTES) {
                all_bc_64k = 0;
            }
        }
        rdna4sp_size sz3;
        if (!rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, bc_fmts[i], 1u, &sz3) ||
            (sz3.width / 4u) * (sz3.height / 4u) * sz3.depth * fi->block_bytes != RDNA4SP_TILE_BYTES) {
            all_bc_64k = 0;
        }
        // BC must reject MSAA
        rdna4sp_size msz;
        if (rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, bc_fmts[i], 4u, &msz) || msz.width != 0u)
            all_bc_64k = 0;
    }
    CHECK("64 KiB invariant across all BC1..BC7 formats (2D/3D ok, MSAA rejected)",
          all_bc_64k, "BC1/BC4=512x256, BC2/3/5/6H/7=256x256");

    // Exact spot-checks against Vulkan Standard Sparse Image Block Shapes.
    rdna4sp_size s = {0};
    CHECK("2D 8/16/32/64/128-bpp exact tile shapes",
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_R8Unorm, 1u, &s) && s.width == 256 && s.height == 256 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_R16Float, 1u, &s) && s.width == 256 && s.height == 128 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) && s.width == 128 && s.height == 128 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA16Float, 1u, &s) && s.width == 128 && s.height == 64 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA32Float, 1u, &s) && s.width == 64 && s.height == 64 && s.depth == 1,
          "256x256, 256x128, 128x128, 128x64, 64x64");

    CHECK("BC1/BC4 & BC3/BC7 exact texel tile shapes",
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_BC1_RGBA, 1u, &s) && s.width == 512 && s.height == 256 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_BC4_RUnorm, 1u, &s) && s.width == 512 && s.height == 256 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_BC3_RGBA, 1u, &s) && s.width == 256 && s.height == 256 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_BC7_RGBAUnorm, 1u, &s) && s.width == 256 && s.height == 256 && s.depth == 1,
          "512x256 and 256x256");

    CHECK("3D 8/16/32/64/128-bpp exact tile shapes",
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_R8Unorm, 1u, &s) && s.width == 64 && s.height == 32 && s.depth == 32 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_R16Float, 1u, &s) && s.width == 32 && s.height == 32 && s.depth == 32 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) && s.width == 32 && s.height == 32 && s.depth == 16 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA16Float, 1u, &s) && s.width == 32 && s.height == 16 && s.depth == 16 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA32Float, 1u, &s) && s.width == 16 && s.height == 16 && s.depth == 16,
          "64x32x32 .. 16x16x16");

    CHECK("MSAA 2x/4x/8x RGBA8 & RGBA32F exact tile shapes",
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 2u, &s) && s.width == 64 && s.height == 128 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 4u, &s) && s.width == 64 && s.height == 64 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 8u, &s) && s.width == 32 && s.height == 64 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA32Float, 2u, &s) && s.width == 32 && s.height == 64 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA32Float, 4u, &s) && s.width == 32 && s.height == 32 && s.depth == 1 &&
          rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA32Float, 8u, &s) && s.width == 16 && s.height == 32 && s.depth == 1,
          "all MSAA cells match Vulkan spec");

    // Unsupported types, formats, and sample counts return 0 / {0,0,0}.
    CHECK("unsupported texture types, formats, and sample counts rejected",
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_1D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_1DARRAY, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_BUFFER, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_for_format(99u, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_INVALID, 1u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, 255u, 1u, &s) &&  // D24S8
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, 260u, 1u, &s) &&  // D32S8 combined
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, 186u, 1u, &s) &&  // ASTC
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 4u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 16u, &s) &&
          !rdna4sp_tile_size_for_format(RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA8Unorm, 2u, &s) &&
          s.width == 0u && s.height == 0u && s.depth == 0u,
          "1D/Buffer/D24S8/D32S8/ASTC/bad-samples rejected");

    // Capability gating + Apple6 EX01 independence.
    rdna4dc_metal3_caps off = rdna4dc_metal3_fallback();
    rdna4dc_metal3_caps only2d = make_sparse_caps(1, 0, 0, 1, 0);
    rdna4dc_metal3_caps full = make_sparse_caps(1, 1, 1, 1, 1);
    CHECK("pre-open fallback keeps sparse off; probed caps gate 2D/3D/MSAA accurately; Apple6 stays NO",
          rdna4sp_tile_size_in_bytes(NULL) == 0u &&
          rdna4sp_tile_size_in_bytes(&off) == 0u &&
          !rdna4sp_tile_size_gated(&off, RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          rdna4sp_tile_size_in_bytes(&only2d) == 65536u &&
          rdna4sp_tile_size_in_bytes_for_page_size(&only2d, RDNA4SP_PAGE_SIZE_64) == 65536u &&
          rdna4sp_tile_size_in_bytes_for_page_size(&only2d, RDNA4SP_PAGE_SIZE_16) == 0u &&
          rdna4sp_tile_size_in_bytes_for_page_size(&only2d, RDNA4SP_PAGE_SIZE_256) == 0u &&
          rdna4sp_tile_size_gated(&only2d, RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_gated(&only2d, RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          !rdna4sp_tile_size_gated(&only2d, RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 2u, &s) &&
          rdna4sp_tile_size_gated(&only2d, RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 4u, &s) &&
          rdna4sp_tile_size_gated(&full, RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA8Unorm, 1u, &s) &&
          rdna4sp_tile_size_gated(&full, RDNA4TD_MTL_2DMS, RDNA4SP_PF_RGBA8Unorm, 8u, &s) &&
          rdna4g_supports_family(1006L, 0) == 0 && rdna4g_supports_family(1006L, 1) == 0,
          "gated on RADV caps, Apple6 (1006) == NO");
}

static void test_region_conversions(void) {
    const rdna4sp_size t2d = { 128u, 128u, 1u };
    const rdna4sp_region px[] = {
        { { 0u, 0u, 0u },     { 128u, 128u, 1u } },   // exact single tile
        { { 64u, 64u, 0u },   { 128u, 128u, 1u } },   // straddles 2x2 tiles
        { { 1u, 127u, 0u },   { 1u, 2u, 1u } },       // 1px inside tile 0, 2px straddling y=128
        { { 64u, 0u, 0u },    { 256u, 300u, 1u } },   // inward keeps [1,2) x [0,2)
        { { 128u, 256u, 0u }, { 0u, 128u, 1u } },     // zero-width empty region
    };
    rdna4sp_region out_out[5], out_in[5];
    int ok_out = rdna4sp_convert_pixel_regions_to_tiles(px, out_out, t2d, RDNA4SP_ALIGN_OUTWARD, 5u);
    int ok_in  = rdna4sp_convert_pixel_regions_to_tiles(px, out_in,  t2d, RDNA4SP_ALIGN_INWARD,  5u);

    CHECK("pixel->tile OUTWARD alignment",
          ok_out &&
          out_out[0].origin.x == 0 && out_out[0].origin.y == 0 && out_out[0].size.width == 1 && out_out[0].size.height == 1 && out_out[0].size.depth == 1 &&
          out_out[1].origin.x == 0 && out_out[1].origin.y == 0 && out_out[1].size.width == 2 && out_out[1].size.height == 2 &&
          out_out[2].origin.x == 0 && out_out[2].origin.y == 0 && out_out[2].size.width == 1 && out_out[2].size.height == 2 &&
          out_out[3].origin.x == 0 && out_out[3].origin.y == 0 && out_out[3].size.width == 3 && out_out[3].size.height == 3 &&
          out_out[4].origin.x == 1 && out_out[4].size.width == 0 && out_out[4].origin.y == 2 && out_out[4].size.height == 1,
          "partial tiles included");

    CHECK("pixel->tile INWARD alignment",
          ok_in &&
          out_in[0].origin.x == 0 && out_in[0].origin.y == 0 && out_in[0].size.width == 1 && out_in[0].size.height == 1 &&
          out_in[1].origin.x == 1 && out_in[1].origin.y == 1 && out_in[1].size.width == 0 && out_in[1].size.height == 0 &&
          out_in[2].origin.x == 1 && out_in[2].size.width == 0 && out_in[2].origin.y == 1 && out_in[2].size.height == 0 &&
          out_in[3].origin.x == 1 && out_in[3].origin.y == 0 && out_in[3].size.width == 1 && out_in[3].size.height == 2 &&
          out_in[4].origin.x == 1 && out_in[4].size.width == 0 && out_in[4].origin.y == 2 && out_in[4].size.height == 1,
          "only fully enclosed tiles kept");

    // 3D tile<->pixel round-trip.
    const rdna4sp_size t3d = { 32u, 32u, 16u };
    const rdna4sp_region tile3 = { { 2u, 3u, 1u }, { 4u, 5u, 2u } };
    rdna4sp_region px3 = {0}, rt_out = {0}, rt_in = {0};
    int ok3 = rdna4sp_convert_tile_regions_to_pixels(&tile3, &px3, t3d, 1u) &&
              rdna4sp_convert_pixel_regions_to_tiles(&px3, &rt_out, t3d, RDNA4SP_ALIGN_OUTWARD, 1u) &&
              rdna4sp_convert_pixel_regions_to_tiles(&px3, &rt_in,  t3d, RDNA4SP_ALIGN_INWARD,  1u);
    CHECK("3D tile->pixel->tile exact round-trip",
          ok3 &&
          px3.origin.x == 64 && px3.origin.y == 96 && px3.origin.z == 16 &&
          px3.size.width == 128 && px3.size.height == 160 && px3.size.depth == 32 &&
          memcmp(&rt_out, &tile3, sizeof(tile3)) == 0 &&
          memcmp(&rt_in, &tile3, sizeof(tile3)) == 0,
          "3D exact round-trip");

    // Overflow & invalid parameter rejections.
    const rdna4sp_region ovf_px = { { UINT64_MAX - 10u, 0u, 0u }, { 20u, 1u, 1u } };
    const rdna4sp_region ovf_t  = { { UINT64_MAX / 64u, 0u, 0u }, { 2u, 1u, 1u } };
    rdna4sp_region tmp = {0};
    CHECK("conversion rejects overflow, zero tile, bad mode, NULL ptrs (0 regions allowed)",
          !rdna4sp_convert_pixel_regions_to_tiles(&ovf_px, &tmp, t2d, RDNA4SP_ALIGN_OUTWARD, 1u) &&
          !rdna4sp_convert_tile_regions_to_pixels(&ovf_t, &tmp, t2d, 1u) &&
          !rdna4sp_convert_pixel_regions_to_tiles(px, &tmp, (rdna4sp_size){ 0u, 128u, 1u }, RDNA4SP_ALIGN_OUTWARD, 1u) &&
          !rdna4sp_convert_tile_regions_to_pixels(&tile3, &tmp, (rdna4sp_size){ 32u, 0u, 16u }, 1u) &&
          !rdna4sp_convert_pixel_regions_to_tiles(px, &tmp, t2d, 2u, 1u) &&
          !rdna4sp_convert_pixel_regions_to_tiles(NULL, &tmp, t2d, RDNA4SP_ALIGN_OUTWARD, 1u) &&
          rdna4sp_convert_pixel_regions_to_tiles(NULL, NULL, (rdna4sp_size){ 0u, 0u, 0u }, 99u, 0u) &&
          rdna4sp_convert_tile_regions_to_pixels(NULL, NULL, (rdna4sp_size){ 0u, 0u, 0u }, 0u),
          "all invalid conversions rejected safely");
}

static void test_mip_tail_layouts(void) {
    rdna4sp_image_layout lay;

    // 512x512 RGBA8, 10 mips (512..1): L0=512, L1=256, L2=128 -> tail starts at L3 (64x64), 64 KiB.
    CHECK("512x512 RGBA8 10-mip chain mip-tail",
          rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 512u, 512u, 1u, 10u, 1u, 1u, 0, 0, &lay) &&
          lay.granularity.width == 128u && lay.granularity.height == 128u && lay.granularity.depth == 1u &&
          lay.mip_tail_first_lod == 3u && lay.mip_tail_size == 65536u,
          "firstMipmapInTail=3 tailSize=65536");

    // 512x512 RGBA8, 2 mips (512, 256): no mip tail!
    CHECK("512x512 RGBA8 2-mip chain has no mip tail",
          rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 512u, 512u, 1u, 2u, 1u, 1u, 0, 0, &lay) &&
          lay.mip_tail_first_lod == 2u && lay.mip_tail_size == 0u,
          "firstMipmapInTail=2 tailSize=0");

    // 64x64 RGBA8: smaller than 128x128 tile at level 0 -> entire texture is mip tail.
    CHECK("64x64 RGBA8 enters tail at level 0",
          rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 64u, 64u, 1u, 7u, 1u, 1u, 0, 0, &lay) &&
          lay.mip_tail_first_lod == 0u && lay.mip_tail_size == 65536u,
          "firstMipmapInTail=0 tailSize=65536");

    // Non-PoT 300x300 RGBA8: aligned_mip_size=0 puts L0(300)/L1(150) outside tail (L2=75 in tail);
    // aligned_mip_size=1 forces L0 into tail since 300 % 128 != 0.
    rdna4sp_image_layout lay_al;
    uint32_t tx0 = 0, ty0 = 0, tz0 = 0;
    CHECK("300x300 non-PoT standard vs ALIGNED_MIP_SIZE_BIT",
          rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 300u, 300u, 1u, 5u, 1u, 1u, 0, 0, &lay) &&
          lay.mip_tail_first_lod == 2u && rdna4sp_mip_tile_dims(&lay, 0u, &tx0, &ty0, &tz0) &&
          tx0 == 3u && ty0 == 3u && tz0 == 1u &&
          rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 300u, 300u, 1u, 5u, 1u, 1u, 1, 0, &lay_al) &&
          lay_al.mip_tail_first_lod == 0u && lay_al.mip_tail_size >= 65536u &&
          (lay_al.mip_tail_size % 65536u) == 0u,
          "standard firstTail=2 (3x3 tiles at L0), aligned firstTail=0");

    // 3D 128x128x64 RGBA8 (tile 32x32x16): L0=128x128x64, L1=64x64x32, L2=32x32x16, L3=16x16x8 (tail!).
    CHECK("3D 128x128x64 RGBA8 mip-tail starts at L3",
          rdna4sp_compute_layout(RDNA4TD_MTL_3D, RDNA4SP_PF_RGBA8Unorm, 128u, 128u, 64u, 6u, 1u, 1u, 0, 0, &lay) &&
          lay.mip_tail_first_lod == 3u && lay.mip_tail_size == 65536u,
          "3D firstMipmapInTail=3");

    // Adopting Vulkan requirements validates alignment and rejects corrupted/overflowing values.
    const uint64_t near_max = UINT64_MAX & ~(RDNA4SP_TILE_BYTES - 1u);
    CHECK("adopt_vk_reqs accepts aligned reqs and rejects misaligned or overflowing tail offset/size/stride",
          rdna4sp_compute_layout(RDNA4TD_MTL_2DARRAY, RDNA4SP_PF_RGBA8Unorm, 512u, 512u, 1u, 5u, 4u, 1u, 0, 0, &lay) &&
          rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 65536u, 131072u, 65536u, 0) &&
          lay.mip_tail_offset == 131072u && lay.mip_tail_stride == 65536u &&
          !rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 4096u, 65536u, 65536u, 0) &&
          !rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 65536u, 1234u, 65536u, 0) &&
          !rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 65536u, 65536u, 0u, 0) &&
          !rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 65536u, near_max, 65536u, 0) &&
          !rdna4sp_layout_adopt_vk_reqs(&lay, 128u, 128u, 1u, RDNA4SP_ASP_COLOR, 3u, 65536u, 65536u, near_max / 2u, 0),
          "Vulkan sparse reqs validated");
}

static void test_pool_and_batch_planner(void) {
    // 1. Tile pool multi-chunk alloc/resolve/free/exhaustion/double-free.
    rdna4sp_tile_pool pool;
    CHECK("pool init rejects unaligned/zero and accepts 8-tile (512 KiB) capacity",
          !rdna4sp_pool_init(&pool, 0u) &&
          !rdna4sp_pool_init(&pool, 4096u) &&
          rdna4sp_pool_init(&pool, 8u * RDNA4SP_TILE_BYTES),
          "512 KiB pool");
    CHECK("pool multi-chunk add and capacity enforcement",
          rdna4sp_pool_add_chunk(&pool, 0x1000u, 4u * RDNA4SP_TILE_BYTES) &&
          rdna4sp_pool_add_chunk(&pool, 0x2000u, 4u * RDNA4SP_TILE_BYTES) &&
          !rdna4sp_pool_add_chunk(&pool, 0x3000u, RDNA4SP_TILE_BYTES) &&
          rdna4sp_pool_free_backed(&pool) == 8u && rdna4sp_pool_unbacked(&pool) == 0u,
          "2x4-tile chunks");

    uint32_t s_extra = 0;
    uint64_t mh0 = 0, mo0 = 0, mh3 = 0, mo3 = 0, mh4 = 0, mo4 = 0, mh7 = 0, mo7 = 0;
    uint32_t tmp_slots[8];
    int alloc_all_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (!rdna4sp_pool_alloc_slot(&pool, &tmp_slots[i])) alloc_all_ok = 0;
    }
    int ninth_refused = !rdna4sp_pool_alloc_slot(&pool, &s_extra);
    int res_ok = rdna4sp_pool_resolve_slot(&pool, 0u, &mh0, &mo0) && mh0 == 0x1000u && mo0 == 0u &&
                 rdna4sp_pool_resolve_slot(&pool, 3u, &mh3, &mo3) && mh3 == 0x1000u && mo3 == 3u * RDNA4SP_TILE_BYTES &&
                 rdna4sp_pool_resolve_slot(&pool, 4u, &mh4, &mo4) && mh4 == 0x2000u && mo4 == 0u &&
                 rdna4sp_pool_resolve_slot(&pool, 7u, &mh7, &mo7) && mh7 == 0x2000u && mo7 == 3u * RDNA4SP_TILE_BYTES;
    int first_free_ok = rdna4sp_pool_free_slot(&pool, tmp_slots[0]);
    int dbl_free_refused = !rdna4sp_pool_free_slot(&pool, tmp_slots[0]);
    int oob_free_refused = !rdna4sp_pool_free_slot(&pool, 99u);
    CHECK("pool allocates 8 slots across 2 chunks, rejects 9th, resolves exact offsets, blocks double-free",
          alloc_all_ok && ninth_refused && res_ok && first_free_ok && dbl_free_refused && oob_free_refused,
          "pool invariants hold");
    for (int i = 1; i < 8; i++) (void)rdna4sp_pool_free_slot(&pool, tmp_slots[i]);
    CHECK("pool fully restored to 8 free tiles", pool.used_tiles == 0u && rdna4sp_pool_free_backed(&pool) == 8u, "used=0");

    // 2. Sparse texture 300x300 RGBA8, 5 mips, 2 array layers (L0=3x3 tiles, L1=2x2 tiles, L2..4=tail 64 KiB/slice).
    rdna4sp_image_layout lay;
    CHECK("init 300x300 2-slice sparse texture state",
          rdna4sp_compute_layout(RDNA4TD_MTL_2DARRAY, RDNA4SP_PF_RGBA8Unorm, 300u, 300u, 1u, 5u, 2u, 1u, 0, 0, &lay),
          "layout ready");
    rdna4sp_texture_state tex;
    CHECK("rdna4sp_tex_init", rdna4sp_tex_init(&tex, &lay) && tex.is_sparse && tex.mapped_tile_count == 0u, "unmapped initially");

    // 3. Plan MAP of edge tile (2,2) on slice 0 L0 + overlapping duplicate + mip tail on slice 1.
    rdna4sp_bind_plan plan;
    rdna4sp_plan_init(&plan);
    const rdna4sp_region map_regs[3] = {
        { { 2u, 2u, 0u }, { 1u, 1u, 1u } }, // clamped edge tile: offset (256,256,0), extent (44,44,1)!
        { { 2u, 2u, 0u }, { 1u, 1u, 1u } }, // duplicate in same batch -> idempotent no-op!
        { { 0u, 0u, 0u }, { 1u, 1u, 1u } }, // mip tail on slice 1 (L2)
    };
    const uint64_t map_mips[3] = { 0u, 0u, 2u };
    const uint64_t map_slices[3] = { 0u, 0u, 1u };
    const char *why = NULL;
    uint64_t need_dup = 99u;
    int rc_cnt_dup = rdna4sp_count_needed_tiles(&tex, RDNA4SP_MODE_MAP, map_regs, map_mips, map_slices, 3u, &need_dup, &why);
    // Overlapping regions: (0,0)+(2,2) and (1,0)+(2,2) -> union is 3x2 = 6 unique tiles (not 8).
    const rdna4sp_region ovl_regs[2] = {
        { { 0u, 0u, 0u }, { 2u, 2u, 1u } },
        { { 1u, 0u, 0u }, { 2u, 2u, 1u } },
    };
    const uint64_t ovl_mips[2] = { 0u, 0u };
    const uint64_t ovl_slices[2] = { 0u, 0u };
    uint64_t need_ovl = 99u;
    int rc_cnt_ovl = rdna4sp_count_needed_tiles(&tex, RDNA4SP_MODE_MAP, ovl_regs, ovl_mips, ovl_slices, 2u, &need_ovl, &why);
    CHECK("count_needed_tiles deduplicates repeated and overlapping tile regions",
          rc_cnt_dup == RDNA4SP_OK && need_dup == 2u &&
          rc_cnt_ovl == RDNA4SP_OK && need_ovl == 6u,
          "dup need=%llu (expect 2), overlap need=%llu (expect 6)",
          (unsigned long long)need_dup, (unsigned long long)need_ovl);

    int rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, map_regs, map_mips, map_slices, 3u, &why);
    CHECK("MAP batch clamps edge extent (44x44), deduplicates, plans opaque mip-tail bind",
          rc == RDNA4SP_OK && plan.n_image_ops == 1u && plan.n_opaque_ops == 1u &&
          plan.image_ops[0].offset_x == 256 && plan.image_ops[0].offset_y == 256 && plan.image_ops[0].offset_z == 0 &&
          plan.image_ops[0].extent_w == 44u && plan.image_ops[0].extent_h == 44u && plan.image_ops[0].extent_d == 1u &&
          plan.image_ops[0].array_layer == 0u && plan.image_ops[0].mip_level == 0u &&
          plan.image_ops[0].is_unmap == 0u && plan.image_ops[0].mem_handle != 0u &&
          plan.opaque_ops[0].resource_offset == lay.mip_tail_offset + lay.mip_tail_stride &&
          plan.opaque_ops[0].size == RDNA4SP_TILE_BYTES && plan.opaque_ops[0].is_unmap == 0u &&
          tex.mapped_tile_count == 2u && pool.used_tiles == 2u,
          "edge extent=44x44, opaque off=%llu", (unsigned long long)(plan.n_opaque_ops ? plan.opaque_ops[0].resource_offset : 0u));

#ifdef RDNA4_SPARSE_TEST_VK
    // Verify real Vulkan 1.4.363 struct builders compile and populate exact fields.
    VkSparseImageMemoryBind vk_img = rdna4sp_to_vk_image_bind(&plan.image_ops[0]);
    VkSparseMemoryBind vk_opq = rdna4sp_to_vk_opaque_bind(&plan.opaque_ops[0]);
    VkSparseImageMemoryBindInfo img_info = { .image = VK_NULL_HANDLE, .bindCount = 1u, .pBinds = &vk_img };
    VkSparseImageOpaqueMemoryBindInfo opq_info = { .image = VK_NULL_HANDLE, .bindCount = 1u, .pBinds = &vk_opq };
    VkSemaphore dummy_sem = (VkSemaphore)(uintptr_t)0x5555u;
    VkBindSparseInfo bi = rdna4sp_make_bind_sparse_info(&img_info, &opq_info, 0u, NULL, 1u, &dummy_sem);
    CHECK("Vulkan 1.4 VkBindSparseInfo struct conversion exact",
          bi.sType == VK_STRUCTURE_TYPE_BIND_SPARSE_INFO &&
          bi.imageBindCount == 1u && bi.imageOpaqueBindCount == 1u &&
          bi.signalSemaphoreCount == 1u && bi.pSignalSemaphores == &dummy_sem &&
          vk_img.offset.x == 256 && vk_img.extent.width == 44u &&
          vk_img.subresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
          vk_img.memory != VK_NULL_HANDLE &&
          vk_opq.size == RDNA4SP_TILE_BYTES && vk_opq.memory != VK_NULL_HANDLE,
          "Vulkan 1.4.363 structs verified");
#endif

    // Commit the MAP plan.
    rdna4sp_plan_commit(&plan, &pool);
    CHECK("post-commit residency tracking",
          rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 2u, 2u, 0u) &&
          !rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 0u, 0u, 0u) &&
          rdna4sp_tex_is_tail_mapped(&tex, 1u) &&
          !rdna4sp_tex_is_tail_mapped(&tex, 0u),
          "tile (2,2) & slice-1 tail mapped");

    // 4. Test atomic rollback when vkQueueBindSparse fails on a MAP of 2x2 tiles (4 tiles).
    const rdna4sp_region r_2x2 = { { 0u, 0u, 0u }, { 2u, 2u, 1u } };
    const uint64_t m0 = 0u, sl0 = 0u;
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &m0, &sl0, 1u, &why);
    CHECK("prepare 2x2 MAP succeeds before simulated Vulkan failure",
          rc == RDNA4SP_OK && plan.n_image_ops == 4u && pool.used_tiles == 6u && tex.mapped_tile_count == 6u,
          "tentatively reserved 4 tiles");
    rdna4sp_plan_rollback(&plan, &tex, &pool);
    CHECK("rollback restores exact tile state & pool slots on Vulkan error",
          pool.used_tiles == 2u && tex.mapped_tile_count == 2u &&
          !rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 0u, 0u, 0u) &&
          rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 2u, 2u, 0u),
          "zero leaked tiles after rollback");

    // 5. Test atomic rollback when pool runs out of tiles mid-batch (only 6 tiles free, asking for 8).
    const rdna4sp_region r_3x3 = { { 0u, 0u, 0u }, { 3u, 3u, 1u } }; // 8 unmapped tiles (since (2,2) already mapped)
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_3x3, &m0, &sl0, 1u, &why);
    CHECK("pool exhaustion mid-batch fails cleanly with atomic rollback",
          rc == RDNA4SP_ERR_OUT_OF_MEMORY && why != NULL &&
          pool.used_tiles == 2u && tex.mapped_tile_count == 2u &&
          !rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 0u, 0u, 0u) &&
          rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 2u, 2u, 0u),
          "OOM rolled back atomically");

    // 6. Test UNMAP rollback vs commit: slots MUST NOT return to pool until commit!
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_UNMAP, map_regs, map_mips, map_slices, 3u, &why);
    CHECK("UNMAP emits VK_NULL_HANDLE ops and defers slot free until commit",
          rc == RDNA4SP_OK && plan.n_image_ops == 1u && plan.n_opaque_ops == 1u &&
          plan.image_ops[0].is_unmap == 1u && plan.image_ops[0].mem_handle == 0u && plan.image_ops[0].mem_offset == 0u &&
          plan.opaque_ops[0].is_unmap == 1u && plan.opaque_ops[0].mem_handle == 0u &&
          pool.used_tiles == 2u && tex.mapped_tile_count == 0u,
          "slots held until commit");
    // Rollback UNMAP -> tiles stay mapped!
    rdna4sp_plan_rollback(&plan, &tex, &pool);
    CHECK("UNMAP rollback restores mapped state",
          pool.used_tiles == 2u && tex.mapped_tile_count == 2u &&
          rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 2u, 2u, 0u) && rdna4sp_tex_is_tail_mapped(&tex, 1u),
          "restored");
    // Now UNMAP and commit -> slots return to pool!
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_UNMAP, map_regs, map_mips, map_slices, 3u, &why);
    rdna4sp_plan_commit(&plan, &pool);
    CHECK("UNMAP commit releases slots back to pool for reuse",
          rc == RDNA4SP_OK && pool.used_tiles == 0u && rdna4sp_pool_free_backed(&pool) == 8u &&
          tex.mapped_tile_count == 0u &&
          !rdna4sp_tex_is_tile_mapped(&tex, 0u, 0u, 2u, 2u, 0u) && !rdna4sp_tex_is_tail_mapped(&tex, 1u),
          "all 8 tiles free again");

    // 7. Re-UNMAP of already-unmapped tiles is a clean no-op (0 ops).
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_UNMAP, map_regs, map_mips, map_slices, 3u, &why);
    rdna4sp_plan_commit(&plan, &pool);
    CHECK("re-UNMAP of unmapped tiles is a safe 0-op no-op",
          rc == RDNA4SP_OK && plan.n_image_ops == 0u && plan.n_opaque_ops == 0u && pool.used_tiles == 0u,
          "no-op");

    // 8. Negative / bounds / overflow / non-sparse / indirect rejections.
    rdna4sp_texture_state non_sparse;
    memset(&non_sparse, 0, sizeof non_sparse);
    const rdna4sp_region bad_oob = { { 3u, 0u, 0u }, { 1u, 1u, 1u } };
    const rdna4sp_region bad_ovf = { { UINT64_MAX - 1u, 0u, 0u }, { 4u, 1u, 1u } };
    const rdna4sp_region bad_tail = { { 1u, 0u, 0u }, { 1u, 1u, 1u } };
    const rdna4sp_region ok_empty = { { 0u, 0u, 0u }, { 0u, 1u, 1u } };
    const uint64_t bad_mip_oob = 5u, bad_mip_tail = 3u, bad_slice = 2u, tail_mip = 2u;
    CHECK("non-sparse, bad mode, OOB, overflow, mip > firstTail, bad tail region rejected",
          rdna4sp_plan_prepare(&plan, &non_sparse, &pool, RDNA4SP_MODE_MAP, &r_2x2, &m0, &sl0, 1u, &why) == RDNA4SP_ERR_NON_SPARSE &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, 99u, &r_2x2, &m0, &sl0, 1u, &why) == RDNA4SP_ERR_BAD_MODE &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &bad_oob, &m0, &sl0, 1u, &why) == RDNA4SP_ERR_OUT_OF_BOUNDS &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &bad_ovf, &m0, &sl0, 1u, &why) == RDNA4SP_ERR_OVERFLOW &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &bad_mip_oob, &sl0, 1u, &why) == RDNA4SP_ERR_OUT_OF_BOUNDS &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &bad_mip_tail, &sl0, 1u, &why) == RDNA4SP_ERR_MIP_IN_TAIL &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &m0, &bad_slice, 1u, &why) == RDNA4SP_ERR_OUT_OF_BOUNDS &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &bad_tail, &tail_mip, &sl0, 1u, &why) == RDNA4SP_ERR_OUT_OF_BOUNDS &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, NULL, &m0, &sl0, 1u, &why) == RDNA4SP_ERR_INVALID_ARG &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &m0, &sl0, RDNA4SP_MAX_BATCH_REGIONS + 1u, &why) == RDNA4SP_ERR_OVERFLOW &&
          rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &ok_empty, &m0, &sl0, 1u, &why) == RDNA4SP_OK &&
          plan.n_image_ops == 0u,
          "all negative checks passed");
    rdna4sp_plan_commit(&plan, &pool);

    // 9. Indirect arguments wire layout & validation.
    rdna4sp_map_indirect_args ia_ok = { 1u, 1u, 0u, 2u, 2u, 1u, 0u, 1u };
    rdna4sp_map_indirect_args ia_oob = { 2u, 0u, 0u, 2u, 1u, 1u, 0u, 0u };
    rdna4sp_region ir = {0}; uint64_t im = 0, is = 0;
    CHECK("MTLMapIndirectArguments size=32 & validation",
          sizeof(rdna4sp_map_indirect_args) == 32u &&
          rdna4sp_validate_indirect_args(&tex, RDNA4SP_MODE_MAP, &ia_ok, &ir, &im, &is, &why) == RDNA4SP_OK &&
          ir.origin.x == 1u && ir.size.width == 2u && im == 0u && is == 1u &&
          rdna4sp_validate_indirect_args(&tex, RDNA4SP_MODE_MAP, &ia_oob, NULL, NULL, NULL, &why) == RDNA4SP_ERR_OUT_OF_BOUNDS &&
          rdna4sp_validate_indirect_args(&non_sparse, RDNA4SP_MODE_MAP, &ia_ok, NULL, NULL, NULL, &why) == RDNA4SP_ERR_NON_SPARSE,
          "indirect arg validation ok");

    // 10. Teardown releases all remaining mapped slots cleanly (ASan leak check).
    rc = rdna4sp_plan_prepare(&plan, &tex, &pool, RDNA4SP_MODE_MAP, &r_2x2, &m0, &sl0, 1u, &why);
    rdna4sp_plan_commit(&plan, &pool);
    rdna4sp_tex_release_all_slots(&tex, &pool);
    CHECK("texture teardown releases all remaining mapped slots back to pool",
          rc == RDNA4SP_OK && tex.mapped_tile_count == 0u && pool.used_tiles == 0u,
          "0 leaked slots");

    // 11. Deduplication for single_miptail across slices and large (>2048 tile) heap-bitset path.
    rdna4sp_image_layout smt_lay;
    rdna4sp_texture_state smt_tex;
    rdna4sp_tile_pool smt_pool;
    rdna4sp_bind_plan smt_plan;
    rdna4sp_plan_init(&smt_plan);
    int smt_init_ok = rdna4sp_compute_layout(RDNA4TD_MTL_2DARRAY, RDNA4SP_PF_RGBA8Unorm, 512u, 512u, 1u, 5u, 2u, 1u, 0, 1, &smt_lay) &&
                      rdna4sp_tex_init(&smt_tex, &smt_lay) &&
                      rdna4sp_pool_init(&smt_pool, RDNA4SP_TILE_BYTES) &&
                      rdna4sp_pool_add_chunk(&smt_pool, 0x9000u, RDNA4SP_TILE_BYTES);
    const rdna4sp_region smt_regs[2] = {
        { { 0u, 0u, 0u }, { 1u, 1u, 1u } },
        { { 0u, 0u, 0u }, { 1u, 1u, 1u } },
    };
    const uint64_t smt_mips[2] = { 3u, 3u };
    const uint64_t smt_slices[2] = { 0u, 1u };
    uint64_t need_smt = 99u;
    int rc_smt_cnt = smt_init_ok ? rdna4sp_count_needed_tiles(&smt_tex, RDNA4SP_MODE_MAP, smt_regs, smt_mips, smt_slices, 2u, &need_smt, &why) : -1;
    int rc_smt_prep = (rc_smt_cnt == RDNA4SP_OK) ? rdna4sp_plan_prepare(&smt_plan, &smt_tex, &smt_pool, RDNA4SP_MODE_MAP, smt_regs, smt_mips, smt_slices, 2u, &why) : -1;
    if (rc_smt_prep == RDNA4SP_OK) rdna4sp_plan_commit(&smt_plan, &smt_pool);
    CHECK("single_miptail across 2 slices counts 1 unique tile and binds cleanly from 1-tile pool",
          smt_init_ok && rc_smt_cnt == RDNA4SP_OK && need_smt == 1u &&
          rc_smt_prep == RDNA4SP_OK && smt_plan.n_opaque_ops == 1u &&
          smt_pool.used_tiles == 1u &&
          rdna4sp_tex_is_tail_mapped(&smt_tex, 0u) && rdna4sp_tex_is_tail_mapped(&smt_tex, 1u),
          "need_smt=%llu opaque_ops=%zu", (unsigned long long)need_smt, smt_plan.n_opaque_ops);
    rdna4sp_tex_release_all_slots(&smt_tex, &smt_pool);
    rdna4sp_plan_destroy(&smt_plan);
    rdna4sp_tex_destroy(&smt_tex);
    rdna4sp_pool_destroy(&smt_pool);

    // Large texture (64x64 tiles = 4096 > 2048 stack bitset threshold): exercises heap bitset & early-error free.
    rdna4sp_image_layout big_lay;
    rdna4sp_texture_state big_tex;
    int big_init_ok = rdna4sp_compute_layout(RDNA4TD_MTL_2D, RDNA4SP_PF_RGBA8Unorm, 8192u, 8192u, 1u, 1u, 1u, 1u, 0, 0, &big_lay) &&
                      rdna4sp_tex_init(&big_tex, &big_lay);
    const rdna4sp_region big_ovl[2] = {
        { { 60u, 60u, 0u }, { 4u, 4u, 1u } },
        { { 62u, 62u, 0u }, { 2u, 2u, 1u } },
    };
    const rdna4sp_region big_bad[2] = {
        { { 0u, 0u, 0u }, { 2u, 2u, 1u } },
        { { 64u, 0u, 0u }, { 1u, 1u, 1u } },
    };
    const uint64_t big_mips[2] = { 0u, 0u };
    const uint64_t big_slices[2] = { 0u, 0u };
    uint64_t need_big = 0u, need_big_err = 99u;
    int rc_big_ok = big_init_ok ? rdna4sp_count_needed_tiles(&big_tex, RDNA4SP_MODE_MAP, big_ovl, big_mips, big_slices, 2u, &need_big, &why) : -1;
    int rc_big_err = big_init_ok ? rdna4sp_count_needed_tiles(&big_tex, RDNA4SP_MODE_MAP, big_bad, big_mips, big_slices, 2u, &need_big_err, &why) : 0;
    CHECK("large texture (>2048 tiles) heap-bitset deduplication and error cleanup",
          big_init_ok && big_tex.total_non_tail_tiles == 4096u &&
          rc_big_ok == RDNA4SP_OK && need_big == 16u &&
          rc_big_err == RDNA4SP_ERR_OUT_OF_BOUNDS && need_big_err == 0u,
          "4096-tile heap bitset need=%llu (expect 16, no leaks)", (unsigned long long)need_big);
    rdna4sp_tex_destroy(&big_tex);

    rdna4sp_plan_destroy(&plan);
    rdna4sp_tex_destroy(&tex);
    rdna4sp_pool_destroy(&pool);
}

static void test_source_pins(const char *dir) {
    char *impl = slurp(dir, "garnet_sparse.m");
    char *hdr = slurp(dir, "garnet_sparse_internal.h");
    char *live = slurp(dir, "tests/test-metal3-sparse-prt.m");
    CHECK("source pins: read garnet_sparse.m, garnet_sparse_internal.h, and test-metal3-sparse-prt.m",
          impl != NULL && hdr != NULL && live != NULL, "dir=%s", dir);
    if (impl && hdr) {
        CHECK("pin: RDNA4ResourceStateEncoder implements all 5 sparse/fence selectors & vkQueueBindSparse fence wait",
              strstr(impl, "updateTextureMappings:") != NULL &&
              strstr(impl, "updateTextureMapping:") != NULL &&
              strstr(impl, "indirectBufferOffset:") != NULL &&
              strstr(impl, "updateFence:") != NULL &&
              strstr(impl, "waitForFence:") != NULL &&
              strstr(impl, "queueBindSparse") != NULL &&
              strstr(impl, "VK_ERROR_DEVICE_LOST") != NULL &&
              strstr(impl, "isAliasable") != NULL &&
              strstr(impl, "__builtin_add_overflow") != NULL &&
              strstr(impl, "propCount == 0u") != NULL &&
              strstr(impl, "aspectMask & fi->aspect_mask") != NULL &&
              strstr(impl, "rdna4fw_wait_bounded") != NULL &&
              strstr(hdr, "RDNA4SparseTexture : RDNA4Texture") != NULL &&
              strstr(impl, "RDNA4SparseHeap") != NULL &&
              strstr(impl, "RDNA4Heap (RDNA4SparseBacking)") != NULL &&
              strstr(impl, "Wave 3 integration notes") != NULL,
              "encoder + sparse texture/heap + fence wait + safety guards present");
        CHECK("pin: garnet_sparse.m never enables MTLGPUFamilyApple6",
              strstr(impl, "RDNA4G_FAMILY_APPLE6") == NULL &&
              strstr(impl, "- (BOOL)supportsFamily:") == NULL &&
              strstr(impl, "-(BOOL)supportsFamily:") == NULL,
              "no Apple6 override");
    }
    if (live) {
        CHECK("pin: test-metal3-sparse-prt.m verifies 64 KiB tiles, sparse_color::resident(), and Apple6 == NO",
              strstr(live, "MTLGPUFamilyApple6") != NULL &&
              strstr(live, "sparseTileSizeInBytes") != NULL &&
              strstr(live, "sparse_color") != NULL &&
              strstr(live, ".resident()") != NULL &&
              strstr(live, "resourceStateCommandEncoder") != NULL &&
              strstr(live, "updateTextureMapping:") != NULL &&
              strstr(live, "indirectBufferOffset:") != NULL &&
              strstr(live, "METAL3_SPARSE_PRT_PASS") != NULL,
              "live test covers all S03 requirements");
    }
    free(impl);
    free(hdr);
    free(live);
}

int main(int argc, char **argv) {
    const char *src_dir = argc > 1 ? argv[1] : "src/garnet-metal";
    test_tile_geometry_and_gating();
    test_region_conversions();
    test_mip_tail_layouts();
    test_pool_and_batch_planner();
    test_source_pins(src_dir);
    printf("test-sparse-metal: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
