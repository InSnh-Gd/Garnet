// rdna4_sparse.h: pure-C 64 KiB sparse tile geometry table, pixel<->tile region
// conversion, mip-tail calculation, physical 64 KiB tile pool, and atomic
// VkSparseImageMemoryBind / VkSparseMemoryBind batch planner & validator.
//
// Host-testable with zero Metal/ObjC dependencies. When <vulkan/vulkan.h> is
// included beforehand (VK_VERSION_1_0), inline Vulkan sparse bind struct builders
// are also exposed. Never advertises MTLGPUFamilyApple6 (sparse is gated on
// rdna4dc_metal3_sparse_texture_ok() over GFX12 64 KiB PRT).
#ifndef RDNA4_SPARSE_H
#define RDNA4_SPARSE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rdna4_devcaps.h"
#include "rdna4_texdesc.h"

#define RDNA4SP_TILE_BYTES        UINT64_C(65536)  // GFX12 PRT / Vulkan standard 64 KiB tile (ML08)
#define RDNA4SP_TILE_SHIFT        16u
#define RDNA4SP_SLOT_UNMAPPED     UINT32_MAX
#define RDNA4SP_MAX_BATCH_REGIONS 65536u
#define RDNA4SP_MAX_TRACKED_TILES (UINT64_C(4) << 20)  // 4M tiles = 256 GiB virtual cap
#define RDNA4SP_MAX_HEAP_TILES    (UINT64_C(1) << 20)  // 64 GiB physical sparse heap cap
#define RDNA4SP_MAX_CHUNK_BYTES   (UINT64_C(64) << 20) // 64 MiB max single non-composite BO chunk

// MTLSparseTextureMappingMode (asserted against Metal headers in rdna4_sparse_impl.inc)
enum {
    RDNA4SP_MODE_MAP   = 0,
    RDNA4SP_MODE_UNMAP = 1,
};

// MTLSparseTextureRegionAlignmentMode (asserted against Metal headers)
enum {
    RDNA4SP_ALIGN_OUTWARD = 0,
    RDNA4SP_ALIGN_INWARD  = 1,
};

// MTLSparsePageSize (macOS 13+; asserted against Metal headers when __MAC_13_0)
enum {
    RDNA4SP_PAGE_SIZE_DEFAULT = 0,
    RDNA4SP_PAGE_SIZE_16      = 101,
    RDNA4SP_PAGE_SIZE_64      = 102,
    RDNA4SP_PAGE_SIZE_256     = 103,
};

// MTLHeapType values (asserted against Metal headers)
enum {
    RDNA4SP_HEAP_AUTOMATIC = 0,
    RDNA4SP_HEAP_PLACEMENT = 1,
    RDNA4SP_HEAP_SPARSE    = 2,
};

// VkImageAspectFlagBits (color / depth / stencil)
enum {
    RDNA4SP_ASP_COLOR   = 1u,
    RDNA4SP_ASP_DEPTH   = 2u,
    RDNA4SP_ASP_STENCIL = 4u,
};

// MTLPixelFormat numeric values for all Garnet-supported sparse formats (asserted in rdna4_sparse_impl.inc)
enum {
    RDNA4SP_PF_INVALID           = 0u,
    RDNA4SP_PF_A8Unorm           = 1u,
    RDNA4SP_PF_R8Unorm           = 10u,
    RDNA4SP_PF_R8Snorm           = 12u,
    RDNA4SP_PF_R8Uint            = 13u,
    RDNA4SP_PF_R8Sint            = 14u,
    RDNA4SP_PF_R16Unorm          = 20u,
    RDNA4SP_PF_R16Snorm          = 22u,
    RDNA4SP_PF_R16Uint           = 23u,
    RDNA4SP_PF_R16Sint           = 24u,
    RDNA4SP_PF_R16Float          = 25u,
    RDNA4SP_PF_RG8Unorm          = 30u,
    RDNA4SP_PF_RG8Snorm          = 32u,
    RDNA4SP_PF_RG8Uint           = 33u,
    RDNA4SP_PF_RG8Sint           = 34u,
    RDNA4SP_PF_R32Uint           = 53u,
    RDNA4SP_PF_R32Sint           = 54u,
    RDNA4SP_PF_R32Float          = 55u,
    RDNA4SP_PF_RG16Unorm         = 60u,
    RDNA4SP_PF_RG16Snorm         = 62u,
    RDNA4SP_PF_RG16Uint          = 63u,
    RDNA4SP_PF_RG16Sint          = 64u,
    RDNA4SP_PF_RG16Float         = 65u,
    RDNA4SP_PF_RGBA8Unorm        = 70u,
    RDNA4SP_PF_RGBA8Unorm_sRGB   = 71u,
    RDNA4SP_PF_RGBA8Snorm        = 72u,
    RDNA4SP_PF_RGBA8Uint         = 73u,
    RDNA4SP_PF_RGBA8Sint         = 74u,
    RDNA4SP_PF_BGRA8Unorm        = 80u,
    RDNA4SP_PF_BGRA8Unorm_sRGB   = 81u,
    RDNA4SP_PF_RGB10A2Unorm      = 90u,
    RDNA4SP_PF_RGB10A2Uint       = 91u,
    RDNA4SP_PF_RG11B10Float      = 92u,
    RDNA4SP_PF_RGB9E5Float       = 93u,
    RDNA4SP_PF_BGR10A2Unorm      = 94u,
    RDNA4SP_PF_RG32Uint          = 103u,
    RDNA4SP_PF_RG32Sint          = 104u,
    RDNA4SP_PF_RG32Float         = 105u,
    RDNA4SP_PF_RGBA16Unorm       = 110u,
    RDNA4SP_PF_RGBA16Snorm       = 112u,
    RDNA4SP_PF_RGBA16Uint        = 113u,
    RDNA4SP_PF_RGBA16Sint        = 114u,
    RDNA4SP_PF_RGBA16Float       = 115u,
    RDNA4SP_PF_RGBA32Uint        = 123u,
    RDNA4SP_PF_RGBA32Sint        = 124u,
    RDNA4SP_PF_RGBA32Float       = 125u,
    RDNA4SP_PF_BC1_RGBA          = 130u,
    RDNA4SP_PF_BC1_RGBA_sRGB     = 131u,
    RDNA4SP_PF_BC2_RGBA          = 132u,
    RDNA4SP_PF_BC2_RGBA_sRGB     = 133u,
    RDNA4SP_PF_BC3_RGBA          = 134u,
    RDNA4SP_PF_BC3_RGBA_sRGB     = 135u,
    RDNA4SP_PF_BC4_RUnorm        = 140u,
    RDNA4SP_PF_BC4_RSnorm        = 141u,
    RDNA4SP_PF_BC5_RGUnorm       = 142u,
    RDNA4SP_PF_BC5_RGSnorm       = 143u,
    RDNA4SP_PF_BC6H_RGBFloat     = 150u,
    RDNA4SP_PF_BC6H_RGBUfloat    = 151u,
    RDNA4SP_PF_BC7_RGBAUnorm     = 152u,
    RDNA4SP_PF_BC7_RGBAUnorm_sRGB = 153u,
    RDNA4SP_PF_Depth16Unorm      = 250u,
    RDNA4SP_PF_Depth32Float      = 252u,
    RDNA4SP_PF_Stencil8          = 253u,
};

// Validation / planner status codes
enum {
    RDNA4SP_OK                = 0,
    RDNA4SP_ERR_INVALID_ARG   = 1,
    RDNA4SP_ERR_NON_SPARSE    = 2,
    RDNA4SP_ERR_BAD_MODE      = 3,
    RDNA4SP_ERR_OUT_OF_BOUNDS = 4,
    RDNA4SP_ERR_OVERFLOW      = 5,
    RDNA4SP_ERR_MIP_IN_TAIL   = 6,
    RDNA4SP_ERR_OUT_OF_MEMORY = 7,
    RDNA4SP_ERR_UNSUPPORTED   = 8,
};

typedef struct {
    uint64_t x, y, z;
} rdna4sp_origin;

typedef struct {
    uint64_t width, height, depth;
} rdna4sp_size;

typedef struct {
    rdna4sp_origin origin;
    rdna4sp_size   size;
} rdna4sp_region;

// Exact wire layout of Apple Metal MTLMapIndirectArguments (32 bytes)
typedef struct {
    uint32_t region_origin_x;
    uint32_t region_origin_y;
    uint32_t region_origin_z;
    uint32_t region_size_width;
    uint32_t region_size_height;
    uint32_t region_size_depth;
    uint32_t mip_map_level;
    uint32_t slice_id;
} rdna4sp_map_indirect_args;

typedef struct {
    uint32_t mtl_format;
    uint16_t block_bytes; // 1/2/4/8/16 (uncompressed) or 8/16 (4x4 BC block)
    uint8_t  block_w;     // 1 (uncompressed) or 4 (BC)
    uint8_t  block_h;     // 1 (uncompressed) or 4 (BC)
    uint8_t  is_bc;
    uint8_t  is_ds;
    uint8_t  aspect_mask; // RDNA4SP_ASP_*
} rdna4sp_fmt_info;

static inline const rdna4sp_fmt_info *rdna4sp_fmt_lookup(uint64_t pixel_format) {
    static const rdna4sp_fmt_info kTable[] = {
        // 8-bit uncompressed (1 B/texel)
        { RDNA4SP_PF_A8Unorm,            1, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R8Unorm,            1, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R8Snorm,            1, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R8Uint,             1, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R8Sint,             1, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_Stencil8,           1, 1, 1, 0, 1, RDNA4SP_ASP_STENCIL },
        // 16-bit uncompressed (2 B/texel)
        { RDNA4SP_PF_R16Unorm,           2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R16Snorm,           2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R16Uint,            2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R16Sint,            2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R16Float,           2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG8Unorm,           2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG8Snorm,           2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG8Uint,            2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG8Sint,            2, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_Depth16Unorm,       2, 1, 1, 0, 1, RDNA4SP_ASP_DEPTH },
        // 32-bit uncompressed (4 B/texel)
        { RDNA4SP_PF_R32Uint,            4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R32Sint,            4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_R32Float,           4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG16Unorm,          4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG16Snorm,          4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG16Uint,           4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG16Sint,           4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG16Float,          4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA8Unorm,         4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA8Unorm_sRGB,    4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA8Snorm,         4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA8Uint,          4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA8Sint,          4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BGRA8Unorm,         4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BGRA8Unorm_sRGB,    4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGB10A2Unorm,       4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGB10A2Uint,        4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG11B10Float,       4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGB9E5Float,        4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BGR10A2Unorm,       4, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_Depth32Float,       4, 1, 1, 0, 1, RDNA4SP_ASP_DEPTH },
        // 64-bit uncompressed (8 B/texel)
        { RDNA4SP_PF_RG32Uint,           8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG32Sint,           8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RG32Float,          8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA16Unorm,        8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA16Snorm,        8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA16Uint,         8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA16Sint,         8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA16Float,        8, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        // 128-bit uncompressed (16 B/texel)
        { RDNA4SP_PF_RGBA32Uint,        16, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA32Sint,        16, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_RGBA32Float,       16, 1, 1, 0, 0, RDNA4SP_ASP_COLOR },
        // BC 64-bit / 4x4 block (8 B per 4x4 block)
        { RDNA4SP_PF_BC1_RGBA,           8, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC1_RGBA_sRGB,      8, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC4_RUnorm,         8, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC4_RSnorm,         8, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        // BC 128-bit / 4x4 block (16 B per 4x4 block)
        { RDNA4SP_PF_BC2_RGBA,          16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC2_RGBA_sRGB,     16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC3_RGBA,          16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC3_RGBA_sRGB,     16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC5_RGUnorm,       16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC5_RGSnorm,       16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC6H_RGBFloat,     16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC6H_RGBUfloat,    16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC7_RGBAUnorm,     16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
        { RDNA4SP_PF_BC7_RGBAUnorm_sRGB,16, 4, 4, 1, 0, RDNA4SP_ASP_COLOR },
    };
    if (pixel_format > UINT32_MAX) return NULL;
    for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        if (kTable[i].mtl_format == (uint32_t)pixel_format) return &kTable[i];
    }
    return NULL;
}

static inline int rdna4sp_bpp_index(uint16_t block_bytes) {
    switch (block_bytes) {
    case 1u:  return 0;
    case 2u:  return 1;
    case 4u:  return 2;
    case 8u:  return 3;
    case 16u: return 4;
    default:  return -1;
    }
}

// Pure-C capability gate for sparse textures (independent of MTLGPUFamilyApple6).
static inline int rdna4sp_caps_support_texture(const rdna4dc_metal3_caps *caps,
                                               uint64_t tex_type, uint64_t sample_count) {
    if (!rdna4dc_metal3_sparse_texture_ok(caps)) return 0;
    switch (tex_type) {
    case RDNA4TD_MTL_2D:
    case RDNA4TD_MTL_2DARRAY:
    case RDNA4TD_MTL_CUBE:
    case RDNA4TD_MTL_CUBEARRAY:
        return sample_count == 1u;
    case RDNA4TD_MTL_3D:
        return sample_count == 1u && caps->sparse_residency_image_3d != 0;
    case RDNA4TD_MTL_2DMS:
    case RDNA4TD_MTL_2DMSARRAY:
        if (sample_count == 2u) return caps->sparse_residency_2_samples != 0;
        if (sample_count == 4u) return caps->sparse_residency_4_samples != 0;
        if (sample_count == 8u) return caps->sparse_residency_8_samples != 0;
        return 0;
    default:
        return 0;
    }
}

static inline uint64_t rdna4sp_tile_size_in_bytes(const rdna4dc_metal3_caps *caps) {
    return rdna4dc_metal3_sparse_texture_ok(caps) ? RDNA4SP_TILE_BYTES : 0u;
}

static inline uint64_t rdna4sp_tile_size_in_bytes_for_page_size(const rdna4dc_metal3_caps *caps,
                                                                int64_t sparse_page_size) {
    if (!rdna4dc_metal3_sparse_texture_ok(caps)) return 0u;
    // GFX12 PRT native sparse tile granularity is strictly 64 KiB (ML08).
    return sparse_page_size == RDNA4SP_PAGE_SIZE_64 ? RDNA4SP_TILE_BYTES : 0u;
}

// Vulkan Standard Sparse Image Block Shapes (all 64 KiB = 65,536 bytes).
// Returns 1 and writes texel dimensions to *out, or 0 ({0,0,0}) for unsupported inputs.
static inline int rdna4sp_tile_size_for_format(uint64_t tex_type, uint64_t pixel_format,
                                               uint64_t sample_count, rdna4sp_size *out) {
    if (out) *out = (rdna4sp_size){ 0u, 0u, 0u };
    if (!out) return 0;
    const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup(pixel_format);
    if (!fi) return 0;
    const int bi = rdna4sp_bpp_index(fi->block_bytes);
    if (bi < 0) return 0;

    // Indexed by block_bytes: [0]=1B, [1]=2B, [2]=4B, [3]=8B, [4]=16B.
    static const uint16_t k2D[5][2] = {
        { 256, 256 }, { 256, 128 }, { 128, 128 }, { 128, 64 }, { 64, 64 }
    };
    static const uint16_t k3D[5][3] = {
        { 64, 32, 32 }, { 32, 32, 32 }, { 32, 32, 16 }, { 32, 16, 16 }, { 16, 16, 16 }
    };
    static const uint16_t kMS2[5][2] = {
        { 128, 256 }, { 128, 128 }, { 64, 128 }, { 64, 64 }, { 32, 64 }
    };
    static const uint16_t kMS4[5][2] = {
        { 128, 128 }, { 128, 64 }, { 64, 64 }, { 64, 32 }, { 32, 32 }
    };
    static const uint16_t kMS8[5][2] = {
        { 64, 128 }, { 64, 64 }, { 32, 64 }, { 32, 32 }, { 16, 32 }
    };

    switch (tex_type) {
    case RDNA4TD_MTL_2D:
    case RDNA4TD_MTL_2DARRAY:
    case RDNA4TD_MTL_CUBE:
    case RDNA4TD_MTL_CUBEARRAY:
        if (sample_count != 1u) return 0;
        out->width  = (uint64_t)k2D[bi][0] * (uint64_t)fi->block_w;
        out->height = (uint64_t)k2D[bi][1] * (uint64_t)fi->block_h;
        out->depth  = 1u;
        return 1;
    case RDNA4TD_MTL_3D:
        if (sample_count != 1u || fi->is_ds) return 0;
        out->width  = (uint64_t)k3D[bi][0] * (uint64_t)fi->block_w;
        out->height = (uint64_t)k3D[bi][1] * (uint64_t)fi->block_h;
        out->depth  = (uint64_t)k3D[bi][2];
        return 1;
    case RDNA4TD_MTL_2DMS:
    case RDNA4TD_MTL_2DMSARRAY:
        if (fi->is_bc) return 0;
        if (sample_count == 2u) {
            out->width = kMS2[bi][0]; out->height = kMS2[bi][1]; out->depth = 1u;
            return 1;
        }
        if (sample_count == 4u) {
            out->width = kMS4[bi][0]; out->height = kMS4[bi][1]; out->depth = 1u;
            return 1;
        }
        if (sample_count == 8u) {
            out->width = kMS8[bi][0]; out->height = kMS8[bi][1]; out->depth = 1u;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static inline int rdna4sp_tile_size_gated(const rdna4dc_metal3_caps *caps,
                                          uint64_t tex_type, uint64_t pixel_format,
                                          uint64_t sample_count, rdna4sp_size *out) {
    if (out) *out = (rdna4sp_size){ 0u, 0u, 0u };
    if (!rdna4sp_caps_support_texture(caps, tex_type, sample_count)) return 0;
    return rdna4sp_tile_size_for_format(tex_type, pixel_format, sample_count, out);
}

// ---- Pixel <-> Tile region conversions ----

static inline int rdna4sp_axis_px_to_tile(uint64_t orig, uint64_t size, uint64_t tile_dim,
                                          uint32_t align_mode, uint64_t *t_orig, uint64_t *t_size) {
    if (!tile_dim || !t_orig || !t_size) return 0;
    if (orig > UINT64_MAX - size) return 0;
    if (size == 0u) {
        *t_orig = orig / tile_dim;
        *t_size = 0u;
        return 1;
    }
    const uint64_t end_px = orig + size;
    if (align_mode == RDNA4SP_ALIGN_OUTWARD) {
        const uint64_t start_t = orig / tile_dim;
        const uint64_t end_t = ((end_px - 1u) / tile_dim) + 1u;
        *t_orig = start_t;
        *t_size = end_t - start_t;
        return 1;
    }
    if (align_mode == RDNA4SP_ALIGN_INWARD) {
        const uint64_t start_t = (orig == 0u) ? 0u : (((orig - 1u) / tile_dim) + 1u);
        const uint64_t end_t = end_px / tile_dim;
        *t_orig = start_t;
        *t_size = end_t > start_t ? (end_t - start_t) : 0u;
        return 1;
    }
    return 0;
}

static inline int rdna4sp_convert_pixel_regions_to_tiles(const rdna4sp_region *pixel_regions,
                                                         rdna4sp_region *tile_regions,
                                                         rdna4sp_size tile_size,
                                                         uint32_t alignment_mode,
                                                         size_t num_regions) {
    if (num_regions == 0u) return 1;
    if (!pixel_regions || !tile_regions) return 0;
    if (!tile_size.width || !tile_size.height || !tile_size.depth) return 0;
    if (alignment_mode != RDNA4SP_ALIGN_OUTWARD && alignment_mode != RDNA4SP_ALIGN_INWARD) return 0;
    for (size_t i = 0; i < num_regions; i++) {
        rdna4sp_region out;
        if (!rdna4sp_axis_px_to_tile(pixel_regions[i].origin.x, pixel_regions[i].size.width,
                                     tile_size.width, alignment_mode, &out.origin.x, &out.size.width) ||
            !rdna4sp_axis_px_to_tile(pixel_regions[i].origin.y, pixel_regions[i].size.height,
                                     tile_size.height, alignment_mode, &out.origin.y, &out.size.height) ||
            !rdna4sp_axis_px_to_tile(pixel_regions[i].origin.z, pixel_regions[i].size.depth,
                                     tile_size.depth, alignment_mode, &out.origin.z, &out.size.depth)) {
            return 0;
        }
        tile_regions[i] = out;
    }
    return 1;
}

static inline int rdna4sp_axis_tile_to_px(uint64_t orig, uint64_t size, uint64_t tile_dim,
                                          uint64_t *p_orig, uint64_t *p_size) {
    if (!tile_dim || !p_orig || !p_size) return 0;
    if (orig > UINT64_MAX / tile_dim || size > UINT64_MAX / tile_dim) return 0;
    const uint64_t po = orig * tile_dim;
    const uint64_t ps = size * tile_dim;
    if (po > UINT64_MAX - ps) return 0;
    *p_orig = po;
    *p_size = ps;
    return 1;
}

static inline int rdna4sp_convert_tile_regions_to_pixels(const rdna4sp_region *tile_regions,
                                                         rdna4sp_region *pixel_regions,
                                                         rdna4sp_size tile_size,
                                                         size_t num_regions) {
    if (num_regions == 0u) return 1;
    if (!tile_regions || !pixel_regions) return 0;
    if (!tile_size.width || !tile_size.height || !tile_size.depth) return 0;
    for (size_t i = 0; i < num_regions; i++) {
        rdna4sp_region out;
        if (!rdna4sp_axis_tile_to_px(tile_regions[i].origin.x, tile_regions[i].size.width,
                                     tile_size.width, &out.origin.x, &out.size.width) ||
            !rdna4sp_axis_tile_to_px(tile_regions[i].origin.y, tile_regions[i].size.height,
                                     tile_size.height, &out.origin.y, &out.size.height) ||
            !rdna4sp_axis_tile_to_px(tile_regions[i].origin.z, tile_regions[i].size.depth,
                                     tile_size.depth, &out.origin.z, &out.size.depth)) {
            return 0;
        }
        pixel_regions[i] = out;
    }
    return 1;
}

// ---- Sparse image layout & mip-tail threshold calculation ----

typedef struct {
    uint32_t width, height, depth;
    uint32_t mip_levels, array_layers, sample_count;
    uint32_t tex_type, pixel_format, aspect_mask;
    uint8_t  is_bc, is_3d, single_miptail;
    rdna4sp_size granularity;     // 64 KiB tile extent in texels
    uint32_t mip_tail_first_lod;  // == mip_levels when no mip tail exists
    uint64_t mip_tail_size;       // bytes per tail instance (multiple of 65536, or 0)
    uint64_t mip_tail_offset;     // Vulkan opaque bind base offset
    uint64_t mip_tail_stride;     // Vulkan per-slice opaque bind stride (0 when single_miptail)
} rdna4sp_image_layout;

static inline uint32_t rdna4sp_mip_dim(uint32_t base, uint32_t level) {
    if (level >= 32u) return 1u;
    uint32_t v = base >> level;
    return v ? v : 1u;
}

static inline uint32_t rdna4sp_ceil_div_u32(uint32_t a, uint32_t b) {
    return b ? ((a - 1u) / b) + 1u : 0u;
}

static inline uint32_t rdna4sp_max_mips(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h;
    if (d > m) m = d;
    uint32_t levels = 1u;
    while (m > 1u) { m >>= 1u; levels++; }
    return levels;
}

static inline int rdna4sp_mip_tile_dims(const rdna4sp_image_layout *lay, uint32_t mip,
                                        uint32_t *tx, uint32_t *ty, uint32_t *tz) {
    if (!lay || mip >= lay->mip_levels || !tx || !ty || !tz) return 0;
    if (!lay->granularity.width || !lay->granularity.height || !lay->granularity.depth) return 0;
    if (mip >= lay->mip_tail_first_lod) {
        *tx = 1u; *ty = 1u; *tz = 1u;
        return 1;
    }
    const uint32_t mw = rdna4sp_mip_dim(lay->width, mip);
    const uint32_t mh = rdna4sp_mip_dim(lay->height, mip);
    const uint32_t md = lay->is_3d ? rdna4sp_mip_dim(lay->depth, mip) : 1u;
    *tx = rdna4sp_ceil_div_u32(mw, (uint32_t)lay->granularity.width);
    *ty = rdna4sp_ceil_div_u32(mh, (uint32_t)lay->granularity.height);
    *tz = rdna4sp_ceil_div_u32(md, (uint32_t)lay->granularity.depth);
    return 1;
}

// Pure-C layout computation for a sparse texture (host tests & pre-Vulkan fallback).
// aligned_mip_size: 1 mirrors VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT.
static inline int rdna4sp_compute_layout(uint64_t tex_type, uint64_t pixel_format,
                                         uint32_t width, uint32_t height, uint32_t depth,
                                         uint32_t mip_levels, uint32_t array_layers,
                                         uint32_t sample_count, int aligned_mip_size,
                                         int single_miptail, rdna4sp_image_layout *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!width || !height || !mip_levels || !array_layers) return 0;
    const int is_3d = (tex_type == RDNA4TD_MTL_3D);
    if (!is_3d) depth = 1u;
    if (!depth) return 0;
    if ((tex_type == RDNA4TD_MTL_CUBE || tex_type == RDNA4TD_MTL_CUBEARRAY) && width != height) return 0;
    if (tex_type == RDNA4TD_MTL_CUBE && array_layers != RDNA4TD_CUBE_FACES) return 0;
    if (tex_type == RDNA4TD_MTL_CUBEARRAY && (array_layers % RDNA4TD_CUBE_FACES) != 0u) return 0;
    if ((tex_type == RDNA4TD_MTL_2DMS || tex_type == RDNA4TD_MTL_2DMSARRAY) && mip_levels != 1u) return 0;
    if (mip_levels > rdna4sp_max_mips(width, height, is_3d ? depth : 1u)) return 0;

    const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup(pixel_format);
    rdna4sp_size gran = { 0u, 0u, 0u };
    if (!fi || !rdna4sp_tile_size_for_format(tex_type, pixel_format, sample_count, &gran)) return 0;

    uint32_t first_tail = mip_levels;
    for (uint32_t lv = 0; lv < mip_levels; lv++) {
        const uint32_t mw = rdna4sp_mip_dim(width, lv);
        const uint32_t mh = rdna4sp_mip_dim(height, lv);
        const uint32_t md = is_3d ? rdna4sp_mip_dim(depth, lv) : 1u;
        const int smaller = (mw < gran.width) || (mh < gran.height) || (is_3d && md < gran.depth);
        const int misaligned = aligned_mip_size &&
            ((mw % gran.width) != 0u || (mh % gran.height) != 0u || (is_3d && (md % gran.depth) != 0u));
        if (smaller || misaligned) {
            first_tail = lv;
            break;
        }
    }

    uint64_t tail_bytes = 0u;
    if (first_tail < mip_levels) {
        uint64_t raw = 0u;
        for (uint32_t lv = first_tail; lv < mip_levels; lv++) {
            const uint64_t mw = rdna4sp_mip_dim(width, lv);
            const uint64_t mh = rdna4sp_mip_dim(height, lv);
            const uint64_t md = is_3d ? rdna4sp_mip_dim(depth, lv) : 1u;
            const uint64_t bw = fi->is_bc ? ((mw + 3u) / 4u) : mw;
            const uint64_t bh = fi->is_bc ? ((mh + 3u) / 4u) : mh;
            if (bw > UINT64_MAX / bh) return 0;
            uint64_t blocks = bw * bh;
            if (blocks > UINT64_MAX / md) return 0;
            blocks *= md;
            const uint64_t per_blk = (uint64_t)fi->block_bytes * (uint64_t)sample_count;
            if (blocks > UINT64_MAX / per_blk) return 0;
            const uint64_t lv_bytes = blocks * per_blk;
            if (raw > UINT64_MAX - lv_bytes) return 0;
            raw += lv_bytes;
        }
        if (single_miptail && array_layers > 1u) {
            if (raw > UINT64_MAX / array_layers) return 0;
            raw *= array_layers;
        }
        if (raw == 0u) raw = 1u;
        if (raw > UINT64_MAX - (RDNA4SP_TILE_BYTES - 1u)) return 0;
        tail_bytes = (raw + RDNA4SP_TILE_BYTES - 1u) & ~(RDNA4SP_TILE_BYTES - 1u);
    }

    out->width = width;
    out->height = height;
    out->depth = depth;
    out->mip_levels = mip_levels;
    out->array_layers = array_layers;
    out->sample_count = sample_count;
    out->tex_type = (uint32_t)tex_type;
    out->pixel_format = (uint32_t)pixel_format;
    out->aspect_mask = fi->aspect_mask;
    out->is_bc = fi->is_bc;
    out->is_3d = is_3d ? 1u : 0u;
    out->single_miptail = single_miptail ? 1u : 0u;
    out->granularity = gran;
    out->mip_tail_first_lod = first_tail;
    out->mip_tail_size = tail_bytes;
    // Deterministic synthetic opaque offsets for host planner tests (overwritten by Vulkan reqs live).
    out->mip_tail_offset = tail_bytes ? RDNA4SP_TILE_BYTES : 0u;
    out->mip_tail_stride = (tail_bytes && !single_miptail) ? tail_bytes : 0u;
    return 1;
}

// Adopt authoritative Vulkan VkSparseImageMemoryRequirements fields into rdna4sp_image_layout.
static inline int rdna4sp_layout_adopt_vk_reqs(rdna4sp_image_layout *lay,
                                               uint32_t gran_w, uint32_t gran_h, uint32_t gran_d,
                                               uint32_t aspect_mask, uint32_t tail_first_lod,
                                               uint64_t tail_size, uint64_t tail_offset,
                                               uint64_t tail_stride, int single_miptail) {
    if (!lay || !gran_w || !gran_h || !gran_d || !aspect_mask) return 0;
    if (tail_first_lod > lay->mip_levels) tail_first_lod = lay->mip_levels;
    if (tail_first_lod < lay->mip_levels) {
        if (!tail_size || (tail_size % RDNA4SP_TILE_BYTES) != 0u) return 0;
        if ((tail_offset % RDNA4SP_TILE_BYTES) != 0u) return 0;
        if (tail_offset > UINT64_MAX - tail_size) return 0;
        if (!single_miptail && lay->array_layers > 1u) {
            if (!tail_stride || (tail_stride % RDNA4SP_TILE_BYTES) != 0u) return 0;
            const uint64_t span_layers = (uint64_t)(lay->array_layers - 1u);
            if (span_layers > (UINT64_MAX - tail_offset - tail_size) / tail_stride) return 0;
        }
    } else {
        tail_size = 0u;
        tail_offset = 0u;
        tail_stride = 0u;
    }
    lay->granularity = (rdna4sp_size){ gran_w, gran_h, gran_d };
    lay->aspect_mask = aspect_mask;
    lay->mip_tail_first_lod = tail_first_lod;
    lay->mip_tail_size = tail_size;
    lay->mip_tail_offset = tail_offset;
    lay->mip_tail_stride = single_miptail ? 0u : tail_stride;
    lay->single_miptail = single_miptail ? 1u : 0u;
    return 1;
}

// ---- Physical 64 KiB sparse tile pool (backing heap memory manager) ----

typedef struct {
    uint64_t mem_handle;  // (uint64_t)(uintptr_t)VkDeviceMemory
    uint64_t byte_size;
    uint32_t first_slot;
    uint32_t slot_count;
} rdna4sp_pool_chunk;

typedef struct {
    uint32_t capacity_tiles;
    uint32_t used_tiles;
    uint32_t free_top;
    uint32_t *free_stack;
    uint8_t  *in_use;
    rdna4sp_pool_chunk *chunks;
    size_t n_chunks, cap_chunks;
    uint32_t backed_tiles;
} rdna4sp_tile_pool;

static inline int rdna4sp_pool_init(rdna4sp_tile_pool *p, uint64_t capacity_bytes) {
    if (!p) return 0;
    memset(p, 0, sizeof *p);
    if (!capacity_bytes || (capacity_bytes % RDNA4SP_TILE_BYTES) != 0u) return 0;
    const uint64_t n = capacity_bytes / RDNA4SP_TILE_BYTES;
    if (n == 0u || n > RDNA4SP_MAX_HEAP_TILES) return 0;
    p->free_stack = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    p->in_use = (uint8_t *)calloc((size_t)n, 1u);
    if (!p->free_stack || !p->in_use) {
        free(p->free_stack);
        free(p->in_use);
        memset(p, 0, sizeof *p);
        return 0;
    }
    p->capacity_tiles = (uint32_t)n;
    return 1;
}

static inline void rdna4sp_pool_destroy(rdna4sp_tile_pool *p) {
    if (!p) return;
    free(p->free_stack);
    free(p->in_use);
    free(p->chunks);
    memset(p, 0, sizeof *p);
}

// Register a physical 64 KiB-aligned VkDeviceMemory chunk and push its tile slots onto free_stack.
static inline int rdna4sp_pool_add_chunk(rdna4sp_tile_pool *p, uint64_t mem_handle, uint64_t byte_size) {
    if (!p || !mem_handle || !byte_size || (byte_size % RDNA4SP_TILE_BYTES) != 0u) return 0;
    const uint64_t tiles = byte_size / RDNA4SP_TILE_BYTES;
    if (!tiles || tiles > (uint64_t)(p->capacity_tiles - p->backed_tiles)) return 0;
    if (p->n_chunks == p->cap_chunks) {
        size_t ncap = p->cap_chunks ? p->cap_chunks * 2u : 4u;
        if (ncap < p->cap_chunks || ncap > SIZE_MAX / sizeof(*p->chunks)) return 0;
        rdna4sp_pool_chunk *nc = (rdna4sp_pool_chunk *)realloc(p->chunks, ncap * sizeof(*p->chunks));
        if (!nc) return 0;
        p->chunks = nc;
        p->cap_chunks = ncap;
    }
    const uint32_t first = p->backed_tiles;
    const uint32_t count = (uint32_t)tiles;
    p->chunks[p->n_chunks++] = (rdna4sp_pool_chunk){
        .mem_handle = mem_handle, .byte_size = byte_size,
        .first_slot = first, .slot_count = count,
    };
    p->backed_tiles += count;
    // Push in reverse so lowest slot indices are handed out first.
    for (uint32_t i = count; i > 0u; i--) {
        p->free_stack[p->free_top++] = first + (i - 1u);
    }
    return 1;
}

static inline uint32_t rdna4sp_pool_free_backed(const rdna4sp_tile_pool *p) {
    return p ? p->free_top : 0u;
}

static inline uint32_t rdna4sp_pool_unbacked(const rdna4sp_tile_pool *p) {
    return (p && p->capacity_tiles >= p->backed_tiles) ? (p->capacity_tiles - p->backed_tiles) : 0u;
}

static inline uint32_t rdna4sp_pool_total_available(const rdna4sp_tile_pool *p) {
    return p ? (p->capacity_tiles - p->used_tiles) : 0u;
}

static inline int rdna4sp_pool_alloc_slot(rdna4sp_tile_pool *p, uint32_t *slot_out) {
    if (!p || !slot_out || p->free_top == 0u) return 0;
    const uint32_t slot = p->free_stack[--p->free_top];
    if (slot >= p->backed_tiles || p->in_use[slot]) return 0;
    p->in_use[slot] = 1u;
    p->used_tiles++;
    *slot_out = slot;
    return 1;
}

static inline int rdna4sp_pool_free_slot(rdna4sp_tile_pool *p, uint32_t slot) {
    if (!p || slot >= p->backed_tiles || !p->in_use[slot] || p->used_tiles == 0u) return 0;
    p->in_use[slot] = 0u;
    p->used_tiles--;
    p->free_stack[p->free_top++] = slot;
    return 1;
}

static inline int rdna4sp_pool_resolve_slot(const rdna4sp_tile_pool *p, uint32_t slot,
                                            uint64_t *mem_handle_out, uint64_t *mem_offset_out) {
    if (!p || !mem_handle_out || !mem_offset_out || slot >= p->backed_tiles) return 0;
    for (size_t i = 0; i < p->n_chunks; i++) {
        const rdna4sp_pool_chunk *c = &p->chunks[i];
        if (slot >= c->first_slot && slot < c->first_slot + c->slot_count) {
            *mem_handle_out = c->mem_handle;
            *mem_offset_out = (uint64_t)(slot - c->first_slot) * RDNA4SP_TILE_BYTES;
            return 1;
        }
    }
    return 0;
}

// ---- Per-texture sparse residency state ----

typedef struct {
    int is_sparse;
    rdna4sp_image_layout layout;
    uint32_t *mip_base;         // per-mip offset within one slice's non-tail table
    uint32_t *mip_tx, *mip_ty, *mip_tz;
    uint64_t tiles_per_slice;
    uint64_t total_non_tail_tiles;
    uint32_t *tile_slots;       // length = total_non_tail_tiles (RDNA4SP_SLOT_UNMAPPED or pool slot)
    uint32_t tail_instances;    // 0, 1 (single_miptail), or array_layers
    uint32_t tail_pages_each;   // mip_tail_size / 65536
    uint32_t *tail_slots;       // length = tail_instances * tail_pages_each
    uint64_t mapped_tile_count; // live mapped 64 KiB tiles (non-tail + tail)
} rdna4sp_texture_state;

static inline void rdna4sp_tex_destroy(rdna4sp_texture_state *t) {
    if (!t) return;
    free(t->mip_base);
    free(t->mip_tx);
    free(t->mip_ty);
    free(t->mip_tz);
    free(t->tile_slots);
    free(t->tail_slots);
    memset(t, 0, sizeof *t);
}

static inline int rdna4sp_tex_init(rdna4sp_texture_state *t, const rdna4sp_image_layout *lay) {
    if (!t || !lay || !lay->mip_levels || !lay->array_layers) return 0;
    memset(t, 0, sizeof *t);
    t->layout = *lay;
    const uint32_t non_tail_mips = lay->mip_tail_first_lod < lay->mip_levels
        ? lay->mip_tail_first_lod : lay->mip_levels;

    if (non_tail_mips > 0u) {
        t->mip_base = (uint32_t *)calloc(non_tail_mips, sizeof(uint32_t));
        t->mip_tx   = (uint32_t *)calloc(non_tail_mips, sizeof(uint32_t));
        t->mip_ty   = (uint32_t *)calloc(non_tail_mips, sizeof(uint32_t));
        t->mip_tz   = (uint32_t *)calloc(non_tail_mips, sizeof(uint32_t));
        if (!t->mip_base || !t->mip_tx || !t->mip_ty || !t->mip_tz) {
            rdna4sp_tex_destroy(t);
            return 0;
        }
        uint64_t per_slice = 0u;
        for (uint32_t m = 0; m < non_tail_mips; m++) {
            uint32_t tx = 0u, ty = 0u, tz = 0u;
            if (!rdna4sp_mip_tile_dims(lay, m, &tx, &ty, &tz) || !tx || !ty || !tz) {
                rdna4sp_tex_destroy(t);
                return 0;
            }
            if (per_slice > UINT32_MAX) { rdna4sp_tex_destroy(t); return 0; }
            t->mip_base[m] = (uint32_t)per_slice;
            t->mip_tx[m] = tx;
            t->mip_ty[m] = ty;
            t->mip_tz[m] = tz;
            const uint64_t xy = (uint64_t)tx * (uint64_t)ty;
            if (xy > RDNA4SP_MAX_TRACKED_TILES / tz) { rdna4sp_tex_destroy(t); return 0; }
            const uint64_t mtiles = xy * (uint64_t)tz;
            if (per_slice > RDNA4SP_MAX_TRACKED_TILES - mtiles) { rdna4sp_tex_destroy(t); return 0; }
            per_slice += mtiles;
        }
        if (per_slice > RDNA4SP_MAX_TRACKED_TILES / lay->array_layers) {
            rdna4sp_tex_destroy(t);
            return 0;
        }
        t->tiles_per_slice = per_slice;
        t->total_non_tail_tiles = per_slice * (uint64_t)lay->array_layers;
        if (t->total_non_tail_tiles > 0u) {
            t->tile_slots = (uint32_t *)malloc((size_t)t->total_non_tail_tiles * sizeof(uint32_t));
            if (!t->tile_slots) { rdna4sp_tex_destroy(t); return 0; }
            for (uint64_t i = 0; i < t->total_non_tail_tiles; i++)
                t->tile_slots[i] = RDNA4SP_SLOT_UNMAPPED;
        }
    }

    if (lay->mip_tail_first_lod < lay->mip_levels && lay->mip_tail_size > 0u) {
        if ((lay->mip_tail_size % RDNA4SP_TILE_BYTES) != 0u) { rdna4sp_tex_destroy(t); return 0; }
        const uint64_t pages = lay->mip_tail_size / RDNA4SP_TILE_BYTES;
        const uint32_t inst = lay->single_miptail ? 1u : lay->array_layers;
        if (!pages || pages > RDNA4SP_MAX_TRACKED_TILES / inst) { rdna4sp_tex_destroy(t); return 0; }
        const uint64_t total_tail = pages * (uint64_t)inst;
        if (total_tail > RDNA4SP_MAX_TRACKED_TILES - t->total_non_tail_tiles) {
            rdna4sp_tex_destroy(t);
            return 0;
        }
        t->tail_instances = inst;
        t->tail_pages_each = (uint32_t)pages;
        t->tail_slots = (uint32_t *)malloc((size_t)total_tail * sizeof(uint32_t));
        if (!t->tail_slots) { rdna4sp_tex_destroy(t); return 0; }
        for (uint64_t i = 0; i < total_tail; i++) t->tail_slots[i] = RDNA4SP_SLOT_UNMAPPED;
    }

    t->is_sparse = 1;
    return 1;
}

static inline int rdna4sp_tex_tile_index(const rdna4sp_texture_state *t, uint32_t slice,
                                         uint32_t mip, uint32_t tx, uint32_t ty, uint32_t tz,
                                         uint64_t *idx_out) {
    if (!t || !t->is_sparse || !idx_out) return 0;
    if (slice >= t->layout.array_layers || mip >= t->layout.mip_tail_first_lod ||
        mip >= t->layout.mip_levels) return 0;
    const uint32_t nx = t->mip_tx[mip], ny = t->mip_ty[mip], nz = t->mip_tz[mip];
    if (tx >= nx || ty >= ny || tz >= nz) return 0;
    const uint64_t local = (uint64_t)t->mip_base[mip] +
        ((uint64_t)tz * (uint64_t)ny + (uint64_t)ty) * (uint64_t)nx + (uint64_t)tx;
    *idx_out = (uint64_t)slice * t->tiles_per_slice + local;
    return 1;
}

static inline int rdna4sp_tex_is_tile_mapped(const rdna4sp_texture_state *t, uint32_t slice,
                                             uint32_t mip, uint32_t tx, uint32_t ty, uint32_t tz) {
    uint64_t idx = 0u;
    if (!rdna4sp_tex_tile_index(t, slice, mip, tx, ty, tz, &idx)) return 0;
    return t->tile_slots[idx] != RDNA4SP_SLOT_UNMAPPED;
}

static inline int rdna4sp_tex_is_tail_mapped(const rdna4sp_texture_state *t, uint32_t slice) {
    if (!t || !t->is_sparse || !t->tail_instances || !t->tail_pages_each) return 0;
    if (slice >= t->layout.array_layers) return 0;
    const uint32_t inst = t->layout.single_miptail ? 0u : slice;
    return t->tail_slots[(uint64_t)inst * (uint64_t)t->tail_pages_each] != RDNA4SP_SLOT_UNMAPPED;
}

// Release all still-mapped slots back to pool (called during sparse texture teardown).
static inline void rdna4sp_tex_release_all_slots(rdna4sp_texture_state *t, rdna4sp_tile_pool *pool) {
    if (!t || !t->is_sparse) return;
    for (uint64_t i = 0; i < t->total_non_tail_tiles; i++) {
        if (t->tile_slots[i] != RDNA4SP_SLOT_UNMAPPED) {
            if (pool) (void)rdna4sp_pool_free_slot(pool, t->tile_slots[i]);
            t->tile_slots[i] = RDNA4SP_SLOT_UNMAPPED;
            if (t->mapped_tile_count > 0u) t->mapped_tile_count--;
        }
    }
    const uint64_t total_tail = (uint64_t)t->tail_instances * (uint64_t)t->tail_pages_each;
    for (uint64_t i = 0; i < total_tail; i++) {
        if (t->tail_slots[i] != RDNA4SP_SLOT_UNMAPPED) {
            if (pool) (void)rdna4sp_pool_free_slot(pool, t->tail_slots[i]);
            t->tail_slots[i] = RDNA4SP_SLOT_UNMAPPED;
            if (t->mapped_tile_count > 0u) t->mapped_tile_count--;
        }
    }
}

// ---- Batch planner & atomic two-phase transaction (prepare -> commit / rollback) ----

typedef struct {
    uint32_t aspect_mask;
    uint32_t mip_level;
    uint32_t array_layer;
    int32_t  offset_x, offset_y, offset_z;
    uint32_t extent_w, extent_h, extent_d;
    uint64_t mem_handle;  // 0 on unmap
    uint64_t mem_offset;  // 0 on unmap
    uint32_t tile_slot;   // RDNA4SP_SLOT_UNMAPPED on unmap
    uint8_t  is_unmap;
} rdna4sp_image_bind_op;

typedef struct {
    uint64_t resource_offset;
    uint64_t size;
    uint64_t mem_handle;  // 0 on unmap
    uint64_t mem_offset;  // 0 on unmap
    uint32_t tile_slot;   // RDNA4SP_SLOT_UNMAPPED on unmap
    uint8_t  is_unmap;
} rdna4sp_opaque_bind_op;

typedef struct {
    uint64_t index;
    uint32_t prev_slot;
    uint8_t  is_tail;
} rdna4sp_slot_delta;

typedef struct {
    rdna4sp_image_bind_op  *image_ops;
    size_t                  n_image_ops, cap_image_ops;
    rdna4sp_opaque_bind_op *opaque_ops;
    size_t                  n_opaque_ops, cap_opaque_ops;
    uint32_t               *alloc_slots;   // newly taken from pool; freed on rollback
    size_t                  n_alloc_slots, cap_alloc_slots;
    uint32_t               *release_slots; // unmapped slots; freed on commit
    size_t                  n_release_slots, cap_release_slots;
    rdna4sp_slot_delta     *deltas;        // exact table mutations for atomic rollback
    size_t                  n_deltas, cap_deltas;
    uint64_t                prev_mapped_count;
    int                     active;
} rdna4sp_bind_plan;

static inline void rdna4sp_plan_init(rdna4sp_bind_plan *pl) {
    if (pl) memset(pl, 0, sizeof *pl);
}

static inline void rdna4sp_plan_destroy(rdna4sp_bind_plan *pl) {
    if (!pl) return;
    free(pl->image_ops);
    free(pl->opaque_ops);
    free(pl->alloc_slots);
    free(pl->release_slots);
    free(pl->deltas);
    memset(pl, 0, sizeof *pl);
}

static inline int rdna4sp_grow_array(void **ptr, size_t *cap, size_t elem_size, size_t need) {
    if (need <= *cap) return 1;
    size_t ncap = *cap ? *cap : 16u;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2u) return 0;
        ncap *= 2u;
    }
    if (ncap > SIZE_MAX / elem_size) return 0;
    void *np = realloc(*ptr, ncap * elem_size);
    if (!np) return 0;
    *ptr = np;
    *cap = ncap;
    return 1;
}

static inline int rdna4sp_validate_update_region(const rdna4sp_texture_state *t,
                                                 uint32_t mode,
                                                 const rdna4sp_region *r,
                                                 uint64_t mip, uint64_t slice,
                                                 int *is_tail_out, int *is_empty_out,
                                                 const char **why) {
    if (why) *why = NULL;
    if (is_tail_out) *is_tail_out = 0;
    if (is_empty_out) *is_empty_out = 0;
    if (!t || !t->is_sparse) {
        if (why) *why = "texture is not sparse";
        return RDNA4SP_ERR_NON_SPARSE;
    }
    if (mode != RDNA4SP_MODE_MAP && mode != RDNA4SP_MODE_UNMAP) {
        if (why) *why = "invalid MTLSparseTextureMappingMode";
        return RDNA4SP_ERR_BAD_MODE;
    }
    if (!r) {
        if (why) *why = "NULL sparse region";
        return RDNA4SP_ERR_INVALID_ARG;
    }
    if (mip >= t->layout.mip_levels) {
        if (why) *why = "mipLevel out of texture bounds";
        return RDNA4SP_ERR_OUT_OF_BOUNDS;
    }
    if (slice >= t->layout.array_layers) {
        if (why) *why = "slice out of texture bounds";
        return RDNA4SP_ERR_OUT_OF_BOUNDS;
    }
    if (r->origin.x > UINT64_MAX - r->size.width ||
        r->origin.y > UINT64_MAX - r->size.height ||
        r->origin.z > UINT64_MAX - r->size.depth) {
        if (why) *why = "sparse tile region origin+size overflow";
        return RDNA4SP_ERR_OVERFLOW;
    }

    const int has_tail = (t->layout.mip_tail_first_lod < t->layout.mip_levels);
    if (has_tail && mip > t->layout.mip_tail_first_lod) {
        if (why) *why = "mipLevel > firstMipmapInTail; address the mip tail via firstMipmapInTail";
        return RDNA4SP_ERR_MIP_IN_TAIL;
    }
    const int is_tail = has_tail && (mip == t->layout.mip_tail_first_lod);
    uint32_t lim_x = 1u, lim_y = 1u, lim_z = 1u;
    if (!is_tail) {
        lim_x = t->mip_tx[mip];
        lim_y = t->mip_ty[mip];
        lim_z = t->mip_tz[mip];
    }
    const int empty = (r->size.width == 0u || r->size.height == 0u || r->size.depth == 0u);
    if (r->origin.x > lim_x || r->size.width > (uint64_t)lim_x - r->origin.x ||
        r->origin.y > lim_y || r->size.height > (uint64_t)lim_y - r->origin.y ||
        r->origin.z > lim_z || r->size.depth > (uint64_t)lim_z - r->origin.z) {
        if (why) *why = is_tail ? "mip-tail region must be within (0,0,0)+(1,1,1)"
                                : "sparse tile region exceeds mip tile grid";
        return RDNA4SP_ERR_OUT_OF_BOUNDS;
    }
    if (!is_tail && !empty) {
        // Verify pixel offsets fit in Vulkan signed int32_t VkOffset3D.
        const uint64_t max_ox = (r->origin.x + r->size.width - 1u);
        const uint64_t max_oy = (r->origin.y + r->size.height - 1u);
        const uint64_t max_oz = (r->origin.z + r->size.depth - 1u);
        if (max_ox > (uint64_t)INT32_MAX / t->layout.granularity.width ||
            max_oy > (uint64_t)INT32_MAX / t->layout.granularity.height ||
            max_oz > (uint64_t)INT32_MAX / t->layout.granularity.depth) {
            if (why) *why = "sparse pixel offset exceeds VkOffset3D range";
            return RDNA4SP_ERR_OVERFLOW;
        }
    }
    if (is_tail_out) *is_tail_out = is_tail;
    if (is_empty_out) *is_empty_out = empty;
    return RDNA4SP_OK;
}

static inline int rdna4sp_validate_indirect_args(const rdna4sp_texture_state *t,
                                                 uint32_t mode,
                                                 const rdna4sp_map_indirect_args *args,
                                                 rdna4sp_region *region_out,
                                                 uint64_t *mip_out, uint64_t *slice_out,
                                                 const char **why) {
    if (!args) {
        if (why) *why = "NULL indirect arguments";
        return RDNA4SP_ERR_INVALID_ARG;
    }
    rdna4sp_region r = {
        .origin = { args->region_origin_x, args->region_origin_y, args->region_origin_z },
        .size   = { args->region_size_width, args->region_size_height, args->region_size_depth },
    };
    int rc = rdna4sp_validate_update_region(t, mode, &r, args->mip_map_level, args->slice_id,
                                            NULL, NULL, why);
    if (rc != RDNA4SP_OK) return rc;
    if (region_out) *region_out = r;
    if (mip_out) *mip_out = args->mip_map_level;
    if (slice_out) *slice_out = args->slice_id;
    return RDNA4SP_OK;
}

// Compute how many unique new 64 KiB physical tiles a MAP batch will require (dry run, zero mutation).
// Deduplicates repeated/overlapping tile regions and shared single_miptail slices within the batch.
static inline int rdna4sp_count_needed_tiles(const rdna4sp_texture_state *t, uint32_t mode,
                                             const rdna4sp_region *regions,
                                             const uint64_t *mip_levels, const uint64_t *slices,
                                             size_t num_regions, uint64_t *needed_out,
                                             const char **why) {
    if (needed_out) *needed_out = 0u;
    if (why) *why = NULL;
    if (!t || !t->is_sparse) {
        if (why) *why = "texture is not sparse";
        return RDNA4SP_ERR_NON_SPARSE;
    }
    if (mode != RDNA4SP_MODE_MAP && mode != RDNA4SP_MODE_UNMAP) {
        if (why) *why = "invalid MTLSparseTextureMappingMode";
        return RDNA4SP_ERR_BAD_MODE;
    }
    if (num_regions > RDNA4SP_MAX_BATCH_REGIONS) {
        if (why) *why = "numRegions exceeds RDNA4SP_MAX_BATCH_REGIONS";
        return RDNA4SP_ERR_OVERFLOW;
    }
    if (num_regions > 0u && (!regions || !mip_levels || !slices)) {
        if (why) *why = "NULL regions/mipLevels/slices pointer";
        return RDNA4SP_ERR_INVALID_ARG;
    }

    uint8_t stack_seen[256];
    uint8_t *heap_seen = NULL;
    uint8_t *seen = NULL;
    const uint64_t tail_total = (uint64_t)t->tail_instances * (uint64_t)t->tail_pages_each;
    if (mode == RDNA4SP_MODE_MAP && num_regions > 1u) {
        if (tail_total > RDNA4SP_MAX_TRACKED_TILES ||
            t->total_non_tail_tiles > RDNA4SP_MAX_TRACKED_TILES - tail_total) {
            if (why) *why = "sparse tile tracking table exceeds maximum bound";
            return RDNA4SP_ERR_OVERFLOW;
        }
        const uint64_t total_slots = t->total_non_tail_tiles + tail_total;
        if (total_slots > 0u) {
            const size_t nbytes = (size_t)((total_slots + 7u) / 8u);
            if (nbytes <= sizeof(stack_seen)) {
                memset(stack_seen, 0, nbytes);
                seen = stack_seen;
            } else {
                heap_seen = (uint8_t *)calloc(nbytes, 1u);
                if (!heap_seen) {
                    if (why) *why = "out of host memory counting sparse tiles";
                    return RDNA4SP_ERR_OUT_OF_MEMORY;
                }
                seen = heap_seen;
            }
        }
    }

    uint64_t need = 0u;
    int rc = RDNA4SP_OK;
    for (size_t i = 0; i < num_regions; i++) {
        int is_tail = 0, is_empty = 0;
        rc = rdna4sp_validate_update_region(t, mode, &regions[i], mip_levels[i], slices[i],
                                            &is_tail, &is_empty, why);
        if (rc != RDNA4SP_OK) goto out;
        if (mode != RDNA4SP_MODE_MAP || is_empty) continue;
        if (is_tail) {
            const uint32_t inst = t->layout.single_miptail ? 0u : (uint32_t)slices[i];
            const uint64_t base = (uint64_t)inst * (uint64_t)t->tail_pages_each;
            for (uint32_t k = 0; k < t->tail_pages_each; k++) {
                if (t->tail_slots[base + k] != RDNA4SP_SLOT_UNMAPPED) continue;
                if (seen) {
                    const uint64_t bit = t->total_non_tail_tiles + base + k;
                    const uint8_t mask = (uint8_t)(1u << (bit & 7u));
                    if (seen[bit >> 3u] & mask) continue;
                    seen[bit >> 3u] |= mask;
                }
                need++;
            }
        } else {
            const uint32_t sl = (uint32_t)slices[i];
            const uint32_t ml = (uint32_t)mip_levels[i];
            const rdna4sp_region *r = &regions[i];
            for (uint64_t z = 0; z < r->size.depth; z++) {
                for (uint64_t y = 0; y < r->size.height; y++) {
                    for (uint64_t x = 0; x < r->size.width; x++) {
                        uint64_t idx = 0u;
                        if (rdna4sp_tex_tile_index(t, sl, ml,
                                (uint32_t)(r->origin.x + x), (uint32_t)(r->origin.y + y),
                                (uint32_t)(r->origin.z + z), &idx) &&
                            t->tile_slots[idx] == RDNA4SP_SLOT_UNMAPPED) {
                            if (seen) {
                                const uint8_t mask = (uint8_t)(1u << (idx & 7u));
                                if (seen[idx >> 3u] & mask) continue;
                                seen[idx >> 3u] |= mask;
                            }
                            need++;
                        }
                    }
                }
            }
        }
    }
    if (needed_out) *needed_out = need;
out:
    free(heap_seen);
    return rc;
}

static inline void rdna4sp_plan_rollback(rdna4sp_bind_plan *pl, rdna4sp_texture_state *t,
                                         rdna4sp_tile_pool *pool) {
    if (!pl || !pl->active) return;
    if (t && t->is_sparse) {
        for (size_t i = pl->n_deltas; i > 0u; i--) {
            const rdna4sp_slot_delta d = pl->deltas[i - 1u];
            if (d.is_tail) t->tail_slots[d.index] = d.prev_slot;
            else t->tile_slots[d.index] = d.prev_slot;
        }
        t->mapped_tile_count = pl->prev_mapped_count;
    }
    if (pool) {
        for (size_t i = 0; i < pl->n_alloc_slots; i++)
            (void)rdna4sp_pool_free_slot(pool, pl->alloc_slots[i]);
    }
    pl->active = 0;
}

static inline void rdna4sp_plan_commit(rdna4sp_bind_plan *pl, rdna4sp_tile_pool *pool) {
    if (!pl || !pl->active) return;
    if (pool) {
        for (size_t i = 0; i < pl->n_release_slots; i++)
            (void)rdna4sp_pool_free_slot(pool, pl->release_slots[i]);
    }
    pl->active = 0;
}

static inline int rdna4sp_plan_push_delta(rdna4sp_bind_plan *pl, uint64_t idx,
                                          uint32_t prev_slot, uint8_t is_tail) {
    if (!rdna4sp_grow_array((void **)&pl->deltas, &pl->cap_deltas,
                            sizeof(*pl->deltas), pl->n_deltas + 1u)) return 0;
    pl->deltas[pl->n_deltas++] = (rdna4sp_slot_delta){ idx, prev_slot, is_tail };
    return 1;
}

static inline int rdna4sp_plan_push_alloc(rdna4sp_bind_plan *pl, uint32_t slot) {
    if (!rdna4sp_grow_array((void **)&pl->alloc_slots, &pl->cap_alloc_slots,
                            sizeof(*pl->alloc_slots), pl->n_alloc_slots + 1u)) return 0;
    pl->alloc_slots[pl->n_alloc_slots++] = slot;
    return 1;
}

static inline int rdna4sp_plan_push_release(rdna4sp_bind_plan *pl, uint32_t slot) {
    if (!rdna4sp_grow_array((void **)&pl->release_slots, &pl->cap_release_slots,
                            sizeof(*pl->release_slots), pl->n_release_slots + 1u)) return 0;
    pl->release_slots[pl->n_release_slots++] = slot;
    return 1;
}

static inline int rdna4sp_plan_push_image_op(rdna4sp_bind_plan *pl, const rdna4sp_image_bind_op *op) {
    if (!rdna4sp_grow_array((void **)&pl->image_ops, &pl->cap_image_ops,
                            sizeof(*pl->image_ops), pl->n_image_ops + 1u)) return 0;
    pl->image_ops[pl->n_image_ops++] = *op;
    return 1;
}

static inline int rdna4sp_plan_push_opaque_op(rdna4sp_bind_plan *pl, const rdna4sp_opaque_bind_op *op) {
    if (!rdna4sp_grow_array((void **)&pl->opaque_ops, &pl->cap_opaque_ops,
                            sizeof(*pl->opaque_ops), pl->n_opaque_ops + 1u)) return 0;
    pl->opaque_ops[pl->n_opaque_ops++] = *op;
    return 1;
}

// Validate and plan a batch of Metal sparse map/unmap regions into Vulkan bind ops.
// On success (RDNA4SP_OK), caller executes vkQueueBindSparse and calls rdna4sp_plan_commit()
// on VK_SUCCESS or rdna4sp_plan_rollback() on failure. Any error here rolls back atomically.
static inline int rdna4sp_plan_prepare(rdna4sp_bind_plan *pl, rdna4sp_texture_state *t,
                                       rdna4sp_tile_pool *pool, uint32_t mode,
                                       const rdna4sp_region *regions,
                                       const uint64_t *mip_levels, const uint64_t *slices,
                                       size_t num_regions, const char **why) {
    if (why) *why = NULL;
    if (!pl) { if (why) *why = "NULL bind plan"; return RDNA4SP_ERR_INVALID_ARG; }
    pl->n_image_ops = 0u;
    pl->n_opaque_ops = 0u;
    pl->n_alloc_slots = 0u;
    pl->n_release_slots = 0u;
    pl->n_deltas = 0u;
    pl->active = 0;

    if (!t || !t->is_sparse) {
        if (why) *why = "texture is not sparse";
        return RDNA4SP_ERR_NON_SPARSE;
    }
    if (mode != RDNA4SP_MODE_MAP && mode != RDNA4SP_MODE_UNMAP) {
        if (why) *why = "invalid MTLSparseTextureMappingMode";
        return RDNA4SP_ERR_BAD_MODE;
    }
    if (mode == RDNA4SP_MODE_MAP && !pool) {
        if (why) *why = "MAP requires a sparse backing tile pool";
        return RDNA4SP_ERR_INVALID_ARG;
    }
    if (num_regions > RDNA4SP_MAX_BATCH_REGIONS) {
        if (why) *why = "numRegions exceeds RDNA4SP_MAX_BATCH_REGIONS";
        return RDNA4SP_ERR_OVERFLOW;
    }
    if (num_regions > 0u && (!regions || !mip_levels || !slices)) {
        if (why) *why = "NULL regions/mipLevels/slices pointer";
        return RDNA4SP_ERR_INVALID_ARG;
    }

    // Phase 1: validate every region before mutating anything.
    for (size_t i = 0; i < num_regions; i++) {
        int rc = rdna4sp_validate_update_region(t, mode, &regions[i], mip_levels[i], slices[i],
                                                NULL, NULL, why);
        if (rc != RDNA4SP_OK) return rc;
    }

    // Phase 2: plan binds & record rollback journal.
    pl->prev_mapped_count = t->mapped_tile_count;
    pl->active = 1;

    for (size_t i = 0; i < num_regions; i++) {
        int is_tail = 0, is_empty = 0;
        (void)rdna4sp_validate_update_region(t, mode, &regions[i], mip_levels[i], slices[i],
                                             &is_tail, &is_empty, NULL);
        if (is_empty) continue;

        const uint32_t sl = (uint32_t)slices[i];
        const uint32_t ml = (uint32_t)mip_levels[i];

        if (is_tail) {
            if (!t->tail_instances || !t->tail_pages_each) continue;
            const uint32_t inst = t->layout.single_miptail ? 0u : sl;
            const uint64_t base_idx = (uint64_t)inst * (uint64_t)t->tail_pages_each;
            const uint64_t base_off = t->layout.mip_tail_offset +
                (t->layout.single_miptail ? 0u : (uint64_t)sl * t->layout.mip_tail_stride);

            if (mode == RDNA4SP_MODE_MAP) {
                for (uint32_t k = 0; k < t->tail_pages_each; k++) {
                    const uint64_t slot_idx = base_idx + k;
                    if (t->tail_slots[slot_idx] != RDNA4SP_SLOT_UNMAPPED) continue; // idempotent
                    uint32_t slot = RDNA4SP_SLOT_UNMAPPED;
                    uint64_t mh = 0u, mo = 0u;
                    if (!rdna4sp_pool_alloc_slot(pool, &slot)) {
                        rdna4sp_plan_rollback(pl, t, pool);
                        if (why) *why = "sparse heap exhausted while mapping mip tail";
                        return RDNA4SP_ERR_OUT_OF_MEMORY;
                    }
                    if (!rdna4sp_pool_resolve_slot(pool, slot, &mh, &mo) ||
                        !rdna4sp_plan_push_alloc(pl, slot) ||
                        !rdna4sp_plan_push_delta(pl, slot_idx, RDNA4SP_SLOT_UNMAPPED, 1u)) {
                        if (pl->n_alloc_slots == 0u || pl->alloc_slots[pl->n_alloc_slots - 1u] != slot)
                            (void)rdna4sp_pool_free_slot(pool, slot);
                        rdna4sp_plan_rollback(pl, t, pool);
                        if (why) *why = "host out of memory planning mip-tail map";
                        return RDNA4SP_ERR_OUT_OF_MEMORY;
                    }
                    t->tail_slots[slot_idx] = slot;
                    t->mapped_tile_count++;
                    rdna4sp_opaque_bind_op op = {
                        .resource_offset = base_off + (uint64_t)k * RDNA4SP_TILE_BYTES,
                        .size = RDNA4SP_TILE_BYTES,
                        .mem_handle = mh, .mem_offset = mo,
                        .tile_slot = slot, .is_unmap = 0u,
                    };
                    if (!rdna4sp_plan_push_opaque_op(pl, &op)) {
                        rdna4sp_plan_rollback(pl, t, pool);
                        if (why) *why = "host out of memory appending mip-tail bind";
                        return RDNA4SP_ERR_OUT_OF_MEMORY;
                    }
                }
            } else {
                int any_mapped = 0;
                for (uint32_t k = 0; k < t->tail_pages_each; k++) {
                    const uint64_t slot_idx = base_idx + k;
                    const uint32_t prev = t->tail_slots[slot_idx];
                    if (prev == RDNA4SP_SLOT_UNMAPPED) continue;
                    any_mapped = 1;
                    if (!rdna4sp_plan_push_delta(pl, slot_idx, prev, 1u) ||
                        !rdna4sp_plan_push_release(pl, prev)) {
                        rdna4sp_plan_rollback(pl, t, pool);
                        if (why) *why = "host out of memory planning mip-tail unmap";
                        return RDNA4SP_ERR_OUT_OF_MEMORY;
                    }
                    t->tail_slots[slot_idx] = RDNA4SP_SLOT_UNMAPPED;
                    if (t->mapped_tile_count > 0u) t->mapped_tile_count--;
                }
                if (any_mapped) {
                    rdna4sp_opaque_bind_op op = {
                        .resource_offset = base_off,
                        .size = t->layout.mip_tail_size,
                        .mem_handle = 0u, .mem_offset = 0u,
                        .tile_slot = RDNA4SP_SLOT_UNMAPPED, .is_unmap = 1u,
                    };
                    if (!rdna4sp_plan_push_opaque_op(pl, &op)) {
                        rdna4sp_plan_rollback(pl, t, pool);
                        if (why) *why = "host out of memory appending mip-tail unmap";
                        return RDNA4SP_ERR_OUT_OF_MEMORY;
                    }
                }
            }
            continue;
        }

        // Regular (non-tail) tiled mip level.
        const uint32_t mw = rdna4sp_mip_dim(t->layout.width, ml);
        const uint32_t mh = rdna4sp_mip_dim(t->layout.height, ml);
        const uint32_t md = t->layout.is_3d ? rdna4sp_mip_dim(t->layout.depth, ml) : 1u;
        const uint32_t gw = (uint32_t)t->layout.granularity.width;
        const uint32_t gh = (uint32_t)t->layout.granularity.height;
        const uint32_t gd = (uint32_t)t->layout.granularity.depth;
        const rdna4sp_region *r = &regions[i];

        if (mode == RDNA4SP_MODE_MAP) {
            for (uint64_t dz = 0; dz < r->size.depth; dz++) {
                for (uint64_t dy = 0; dy < r->size.height; dy++) {
                    for (uint64_t dx = 0; dx < r->size.width; dx++) {
                        const uint32_t tx = (uint32_t)(r->origin.x + dx);
                        const uint32_t ty = (uint32_t)(r->origin.y + dy);
                        const uint32_t tz = (uint32_t)(r->origin.z + dz);
                        uint64_t idx = 0u;
                        (void)rdna4sp_tex_tile_index(t, sl, ml, tx, ty, tz, &idx);
                        if (t->tile_slots[idx] != RDNA4SP_SLOT_UNMAPPED) continue; // idempotent

                        uint32_t slot = RDNA4SP_SLOT_UNMAPPED;
                        uint64_t mem_h = 0u, mem_o = 0u;
                        if (!rdna4sp_pool_alloc_slot(pool, &slot)) {
                            rdna4sp_plan_rollback(pl, t, pool);
                            if (why) *why = "sparse heap exhausted while mapping tile region";
                            return RDNA4SP_ERR_OUT_OF_MEMORY;
                        }
                        if (!rdna4sp_pool_resolve_slot(pool, slot, &mem_h, &mem_o) ||
                            !rdna4sp_plan_push_alloc(pl, slot) ||
                            !rdna4sp_plan_push_delta(pl, idx, RDNA4SP_SLOT_UNMAPPED, 0u)) {
                            if (pl->n_alloc_slots == 0u || pl->alloc_slots[pl->n_alloc_slots - 1u] != slot)
                                (void)rdna4sp_pool_free_slot(pool, slot);
                            rdna4sp_plan_rollback(pl, t, pool);
                            if (why) *why = "host out of memory planning tile map";
                            return RDNA4SP_ERR_OUT_OF_MEMORY;
                        }
                        t->tile_slots[idx] = slot;
                        t->mapped_tile_count++;

                        const uint32_t ox = tx * gw;
                        const uint32_t oy = ty * gh;
                        const uint32_t oz = tz * gd;
                        const uint32_t ew = (mw - ox) < gw ? (mw - ox) : gw;
                        const uint32_t eh = (mh - oy) < gh ? (mh - oy) : gh;
                        const uint32_t ed = (md - oz) < gd ? (md - oz) : gd;
                        rdna4sp_image_bind_op op = {
                            .aspect_mask = t->layout.aspect_mask,
                            .mip_level = ml, .array_layer = sl,
                            .offset_x = (int32_t)ox, .offset_y = (int32_t)oy, .offset_z = (int32_t)oz,
                            .extent_w = ew, .extent_h = eh, .extent_d = ed,
                            .mem_handle = mem_h, .mem_offset = mem_o,
                            .tile_slot = slot, .is_unmap = 0u,
                        };
                        if (!rdna4sp_plan_push_image_op(pl, &op)) {
                            rdna4sp_plan_rollback(pl, t, pool);
                            if (why) *why = "host out of memory appending tile map";
                            return RDNA4SP_ERR_OUT_OF_MEMORY;
                        }
                    }
                }
            }
        } else {
            int any_mapped = 0;
            for (uint64_t dz = 0; dz < r->size.depth; dz++) {
                for (uint64_t dy = 0; dy < r->size.height; dy++) {
                    for (uint64_t dx = 0; dx < r->size.width; dx++) {
                        const uint32_t tx = (uint32_t)(r->origin.x + dx);
                        const uint32_t ty = (uint32_t)(r->origin.y + dy);
                        const uint32_t tz = (uint32_t)(r->origin.z + dz);
                        uint64_t idx = 0u;
                        (void)rdna4sp_tex_tile_index(t, sl, ml, tx, ty, tz, &idx);
                        const uint32_t prev = t->tile_slots[idx];
                        if (prev == RDNA4SP_SLOT_UNMAPPED) continue;
                        any_mapped = 1;
                        if (!rdna4sp_plan_push_delta(pl, idx, prev, 0u) ||
                            !rdna4sp_plan_push_release(pl, prev)) {
                            rdna4sp_plan_rollback(pl, t, pool);
                            if (why) *why = "host out of memory planning tile unmap";
                            return RDNA4SP_ERR_OUT_OF_MEMORY;
                        }
                        t->tile_slots[idx] = RDNA4SP_SLOT_UNMAPPED;
                        if (t->mapped_tile_count > 0u) t->mapped_tile_count--;
                    }
                }
            }
            if (any_mapped) {
                const uint32_t ox = (uint32_t)r->origin.x * gw;
                const uint32_t oy = (uint32_t)r->origin.y * gh;
                const uint32_t oz = (uint32_t)r->origin.z * gd;
                const uint64_t raw_w = r->size.width * (uint64_t)gw;
                const uint64_t raw_h = r->size.height * (uint64_t)gh;
                const uint64_t raw_d = r->size.depth * (uint64_t)gd;
                const uint32_t ew = (raw_w > (uint64_t)(mw - ox)) ? (mw - ox) : (uint32_t)raw_w;
                const uint32_t eh = (raw_h > (uint64_t)(mh - oy)) ? (mh - oy) : (uint32_t)raw_h;
                const uint32_t ed = (raw_d > (uint64_t)(md - oz)) ? (md - oz) : (uint32_t)raw_d;
                rdna4sp_image_bind_op op = {
                    .aspect_mask = t->layout.aspect_mask,
                    .mip_level = ml, .array_layer = sl,
                    .offset_x = (int32_t)ox, .offset_y = (int32_t)oy, .offset_z = (int32_t)oz,
                    .extent_w = ew, .extent_h = eh, .extent_d = ed,
                    .mem_handle = 0u, .mem_offset = 0u,
                    .tile_slot = RDNA4SP_SLOT_UNMAPPED, .is_unmap = 1u,
                };
                if (!rdna4sp_plan_push_image_op(pl, &op)) {
                    rdna4sp_plan_rollback(pl, t, pool);
                    if (why) *why = "host out of memory appending tile unmap";
                    return RDNA4SP_ERR_OUT_OF_MEMORY;
                }
            }
        }
    }

    return RDNA4SP_OK;
}

// ---- Optional Vulkan 1.0+ struct builders (active when <vulkan/vulkan.h> is included) ----
#ifdef VK_VERSION_1_0
static inline VkSparseImageMemoryBind rdna4sp_to_vk_image_bind(const rdna4sp_image_bind_op *op) {
    VkSparseImageMemoryBind b;
    memset(&b, 0, sizeof b);
    if (!op) return b;
    b.subresource.aspectMask = (VkImageAspectFlags)op->aspect_mask;
    b.subresource.mipLevel = op->mip_level;
    b.subresource.arrayLayer = op->array_layer;
    b.offset.x = op->offset_x;
    b.offset.y = op->offset_y;
    b.offset.z = op->offset_z;
    b.extent.width = op->extent_w;
    b.extent.height = op->extent_h;
    b.extent.depth = op->extent_d;
    b.memory = (op->is_unmap || !op->mem_handle)
        ? VK_NULL_HANDLE : (VkDeviceMemory)(uintptr_t)op->mem_handle;
    b.memoryOffset = (op->is_unmap || !op->mem_handle) ? 0u : (VkDeviceSize)op->mem_offset;
    b.flags = 0u;
    return b;
}

static inline VkSparseMemoryBind rdna4sp_to_vk_opaque_bind(const rdna4sp_opaque_bind_op *op) {
    VkSparseMemoryBind b;
    memset(&b, 0, sizeof b);
    if (!op) return b;
    b.resourceOffset = (VkDeviceSize)op->resource_offset;
    b.size = (VkDeviceSize)op->size;
    b.memory = (op->is_unmap || !op->mem_handle)
        ? VK_NULL_HANDLE : (VkDeviceMemory)(uintptr_t)op->mem_handle;
    b.memoryOffset = (op->is_unmap || !op->mem_handle) ? 0u : (VkDeviceSize)op->mem_offset;
    b.flags = 0u;
    return b;
}

static inline VkBindSparseInfo rdna4sp_make_bind_sparse_info(
    const VkSparseImageMemoryBindInfo *image_bind_info,
    const VkSparseImageOpaqueMemoryBindInfo *opaque_bind_info,
    uint32_t wait_sem_count, const VkSemaphore *wait_sems,
    uint32_t signal_sem_count, const VkSemaphore *signal_sems) {
    VkBindSparseInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bi.waitSemaphoreCount = wait_sem_count;
    bi.pWaitSemaphores = wait_sem_count ? wait_sems : NULL;
    if (opaque_bind_info && opaque_bind_info->bindCount > 0u) {
        bi.imageOpaqueBindCount = 1u;
        bi.pImageOpaqueBinds = opaque_bind_info;
    }
    if (image_bind_info && image_bind_info->bindCount > 0u) {
        bi.imageBindCount = 1u;
        bi.pImageBinds = image_bind_info;
    }
    bi.signalSemaphoreCount = signal_sem_count;
    bi.pSignalSemaphores = signal_sem_count ? signal_sems : NULL;
    return bi;
}
#endif /* VK_VERSION_1_0 */

#endif /* RDNA4_SPARSE_H */
