// garnet_commands.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_commands_internal.h"


// ---------------------------------------------------------------------------------------------------------------
// Common helpers for the 10b-10d classes.
// ---------------------------------------------------------------------------------------------------------------

GARNET_INTERNAL uint64_t rdna4_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// flags: RDNA4F_A8 = Metal A8Unorm stored as VK R8_UNORM; sampling goes through a view with components (0,0,0,R) (Metal returns (0,0,0,a)); a render target of this
// format is refused (a Vulkan attachment view must be identity, so a fragment's alpha cannot be steered into the R8 image).
static const RDNA4Fmt rdna4_fmts[] = {
    { MTLPixelFormatBGRA8Unorm,      VK_FORMAT_B8G8R8A8_UNORM, 4, 0 },
    { MTLPixelFormatBGRA8Unorm_sRGB, VK_FORMAT_B8G8R8A8_SRGB,  4, 0 },
    { MTLPixelFormatRGBA8Unorm,      VK_FORMAT_R8G8B8A8_UNORM, 4, 0 },
    { MTLPixelFormatRGBA8Unorm_sRGB, VK_FORMAT_R8G8B8A8_SRGB,  4, 0 },
    { MTLPixelFormatR8Unorm,         VK_FORMAT_R8_UNORM,       1, 0 },
    { MTLPixelFormatRG8Unorm,        VK_FORMAT_R8G8_UNORM,     2, 0 },
    { MTLPixelFormatRGBA16Float,     VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0 },
    { MTLPixelFormatRGBA32Float,     VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0 },
    // m11h3 format coverage (CoreDisplay's display pipe and the usual WindowServer set). Every entry is checked against RADV's format features
    // at texture / pipeline creation (rdna4_fmt_feats); an entry RADV cannot sample or copy is refused with a nil log, never faked.
    { MTLPixelFormatR32Float,        VK_FORMAT_R32_SFLOAT,          4, 0 },   // 55: the 16384x1 gamma/LUT array of RunFullDisplayPipe
    { MTLPixelFormatRG32Float,       VK_FORMAT_R32G32_SFLOAT,       8, 0 },
    { MTLPixelFormatR16Float,        VK_FORMAT_R16_SFLOAT,          2, 0 },
    { MTLPixelFormatRG16Float,       VK_FORMAT_R16G16_SFLOAT,       4, 0 },
    { MTLPixelFormatR16Unorm,        VK_FORMAT_R16_UNORM,           2, 0 },
    { MTLPixelFormatRG16Unorm,       VK_FORMAT_R16G16_UNORM,        4, 0 },
    { MTLPixelFormatRGBA16Unorm,     VK_FORMAT_R16G16B16A16_UNORM,  8, 0 },
    // Metal packs RGB10A2 as A2 B10 G10 R10 (red in the LOW bits) and BGR10A2 as A2 R10 G10 B10 (blue low); the Vulkan PACK32 names list the top bits first.
    { MTLPixelFormatRGB10A2Unorm,    VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0 },
    { MTLPixelFormatBGR10A2Unorm,    VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, 0 },
    { MTLPixelFormatRG11B10Float,    VK_FORMAT_B10G11R11_UFLOAT_PACK32,  4, 0 },
    // m11h9: A8Unorm (178 NIL lines in the first Metal-compositor run: glyph / mask textures, also buffer-backed)
    { MTLPixelFormatA8Unorm,         VK_FORMAT_R8_UNORM,                 1, RDNA4F_A8 },
    // bundle 19 (integer format support): pixel format 63. Core Animation's large-shadow distance-field pass (brim_init / brim_jump / brim_outline) ping-pongs two of these; the refusal made
    // emit_large_brim return without drawing, so the whole menu / Spotlight layer vanished. The RADV feature check at texture / pipeline creation stays the fail-closed gate.
    { MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UINT,              4, RDNA4F_UINT },
    { MTLPixelFormatR32Uint,         VK_FORMAT_R32_UINT,                 4, RDNA4F_UINT },
    { MTLPixelFormatR32Sint,         VK_FORMAT_R32_SINT,                 4, RDNA4F_SINT },
    // Metal 2 Mac2 uncompressed SNorm / UInt / SInt / RGB9E5Float formats
    { MTLPixelFormatR8Snorm,         VK_FORMAT_R8_SNORM,                 1, 0 },
    { MTLPixelFormatR8Uint,          VK_FORMAT_R8_UINT,                  1, RDNA4F_UINT },
    { MTLPixelFormatR8Sint,          VK_FORMAT_R8_SINT,                  1, RDNA4F_SINT },
    { MTLPixelFormatR16Snorm,        VK_FORMAT_R16_SNORM,                2, 0 },
    { MTLPixelFormatR16Uint,         VK_FORMAT_R16_UINT,                 2, RDNA4F_UINT },
    { MTLPixelFormatR16Sint,         VK_FORMAT_R16_SINT,                 2, RDNA4F_SINT },
    { MTLPixelFormatRG8Snorm,        VK_FORMAT_R8G8_SNORM,               2, 0 },
    { MTLPixelFormatRG8Uint,         VK_FORMAT_R8G8_UINT,                2, RDNA4F_UINT },
    { MTLPixelFormatRG8Sint,         VK_FORMAT_R8G8_SINT,                2, RDNA4F_SINT },
    { MTLPixelFormatRG16Snorm,       VK_FORMAT_R16G16_SNORM,             4, 0 },
    { MTLPixelFormatRG16Sint,        VK_FORMAT_R16G16_SINT,              4, RDNA4F_SINT },
    { MTLPixelFormatRGBA8Snorm,      VK_FORMAT_R8G8B8A8_SNORM,           4, 0 },
    { MTLPixelFormatRGBA8Uint,       VK_FORMAT_R8G8B8A8_UINT,            4, RDNA4F_UINT },
    { MTLPixelFormatRGBA8Sint,       VK_FORMAT_R8G8B8A8_SINT,            4, RDNA4F_SINT },
    { MTLPixelFormatRGB10A2Uint,     VK_FORMAT_A2B10G10R10_UINT_PACK32,  4, RDNA4F_UINT },
    { MTLPixelFormatRGB9E5Float,     VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,   4, 0 },
    { MTLPixelFormatRG32Uint,        VK_FORMAT_R32G32_UINT,              8, RDNA4F_UINT },
    { MTLPixelFormatRG32Sint,        VK_FORMAT_R32G32_SINT,              8, RDNA4F_SINT },
    { MTLPixelFormatRGBA16Snorm,     VK_FORMAT_R16G16B16A16_SNORM,       8, 0 },
    { MTLPixelFormatRGBA16Uint,      VK_FORMAT_R16G16B16A16_UINT,        8, RDNA4F_UINT },
    { MTLPixelFormatRGBA16Sint,      VK_FORMAT_R16G16B16A16_SINT,        8, RDNA4F_SINT },
    { MTLPixelFormatRGBA32Uint,      VK_FORMAT_R32G32B32A32_UINT,       16, RDNA4F_UINT },
    { MTLPixelFormatRGBA32Sint,      VK_FORMAT_R32G32B32A32_SINT,       16, RDNA4F_SINT },
    // Metal 2 Mac2 BC1..BC7 block-compressed formats (bpp = bytes per 4x4 block)
    { MTLPixelFormatBC1_RGBA,        VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     8, RDNA4F_BC },
    { MTLPixelFormatBC1_RGBA_sRGB,   VK_FORMAT_BC1_RGBA_SRGB_BLOCK,      8, RDNA4F_BC },
    { MTLPixelFormatBC2_RGBA,        VK_FORMAT_BC2_UNORM_BLOCK,         16, RDNA4F_BC },
    { MTLPixelFormatBC2_RGBA_sRGB,   VK_FORMAT_BC2_SRGB_BLOCK,          16, RDNA4F_BC },
    { MTLPixelFormatBC3_RGBA,        VK_FORMAT_BC3_UNORM_BLOCK,         16, RDNA4F_BC },
    { MTLPixelFormatBC3_RGBA_sRGB,   VK_FORMAT_BC3_SRGB_BLOCK,          16, RDNA4F_BC },
    { MTLPixelFormatBC4_RUnorm,      VK_FORMAT_BC4_UNORM_BLOCK,          8, RDNA4F_BC },
    { MTLPixelFormatBC4_RSnorm,      VK_FORMAT_BC4_SNORM_BLOCK,          8, RDNA4F_BC },
    { MTLPixelFormatBC5_RGUnorm,     VK_FORMAT_BC5_UNORM_BLOCK,         16, RDNA4F_BC },
    { MTLPixelFormatBC5_RGSnorm,     VK_FORMAT_BC5_SNORM_BLOCK,         16, RDNA4F_BC },
    { MTLPixelFormatBC6H_RGBFloat,   VK_FORMAT_BC6H_SFLOAT_BLOCK,       16, RDNA4F_BC },
    { MTLPixelFormatBC6H_RGBUfloat,  VK_FORMAT_BC6H_UFLOAT_BLOCK,       16, RDNA4F_BC },
    { MTLPixelFormatBC7_RGBAUnorm,   VK_FORMAT_BC7_UNORM_BLOCK,         16, RDNA4F_BC },
    { MTLPixelFormatBC7_RGBAUnorm_sRGB, VK_FORMAT_BC7_SRGB_BLOCK,       16, RDNA4F_BC },
};


#define RDNA4_NOUINT_FILE "/private/tmp/garnet-nouint"   // bundle 19: exists -> the integer formats are refused as before (nil), read ONCE per process
static BOOL rdna4_nouint(void) {
    static BOOL v; static dispatch_once_t once;
    dispatch_once(&once, ^{ struct stat st; v = rdna4_sb_stat(RDNA4_NOUINT_FILE, &st) == 0; if (v) RDNA4LOG("integer colour formats (RG16Uint) OFF: kill file " RDNA4_NOUINT_FILE " exists (read once per process)"); });
    return v;
}
// The component mapping of a view of format f whose client swizzle is swz: client channel -> (format's own mapping of that channel). Identity for every format but A8.
_Static_assert(RDNA4SWZ_FMT_A8 == RDNA4F_A8, "RDNA4SWZ_FMT_A8 matches RDNA4F_A8");
_Static_assert((int)MTLTextureSwizzleZero == RDNA4SWZ_ZERO && (int)MTLTextureSwizzleOne == RDNA4SWZ_ONE &&
               (int)MTLTextureSwizzleRed == RDNA4SWZ_RED && (int)MTLTextureSwizzleGreen == RDNA4SWZ_GREEN &&
               (int)MTLTextureSwizzleBlue == RDNA4SWZ_BLUE && (int)MTLTextureSwizzleAlpha == RDNA4SWZ_ALPHA, "MTLTextureSwizzle values");
_Static_assert((int)VK_COMPONENT_SWIZZLE_IDENTITY == RDNA4VKSWZ_IDENTITY && (int)VK_COMPONENT_SWIZZLE_ZERO == RDNA4VKSWZ_ZERO &&
               (int)VK_COMPONENT_SWIZZLE_ONE == RDNA4VKSWZ_ONE && (int)VK_COMPONENT_SWIZZLE_R == RDNA4VKSWZ_R &&
               (int)VK_COMPONENT_SWIZZLE_G == RDNA4VKSWZ_G && (int)VK_COMPONENT_SWIZZLE_B == RDNA4VKSWZ_B &&
               (int)VK_COMPONENT_SWIZZLE_A == RDNA4VKSWZ_A, "VkComponentSwizzle values");
_Static_assert((unsigned)MTLTextureUsageShaderRead == RDNA4SWZ_USE_READ && (unsigned)MTLTextureUsageShaderWrite == RDNA4SWZ_USE_WRITE &&
               (unsigned)MTLTextureUsageRenderTarget == RDNA4SWZ_USE_RT && (unsigned)MTLTextureUsagePixelFormatView == RDNA4SWZ_USE_PFV, "MTLTextureUsage swizzle values");


GARNET_INTERNAL VkComponentMapping rdna4_view_map(const RDNA4Fmt *f, MTLTextureSwizzleChannels swz) {
    rdna4swz_vk_t v;
    if (!rdna4swz_to_vk(f ? f->flags : 0u, rdna4_swz_from_mtl(swz), &v))
        return (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    return (VkComponentMapping){ (VkComponentSwizzle)v.r, (VkComponentSwizzle)v.g, (VkComponentSwizzle)v.b, (VkComponentSwizzle)v.a };
}
GARNET_INTERNAL BOOL rdna4_is_identity_map(const VkComponentMapping *m) {
    if (!m) return YES;
    return rdna4swz_vk_is_identity((rdna4swz_vk_t){ (uint32_t)m->r, (uint32_t)m->g, (uint32_t)m->b, (uint32_t)m->a }) ? YES : NO;
}
GARNET_INTERNAL const RDNA4Fmt *rdna4_fmt(MTLPixelFormat f) {
    for (size_t i = 0; i < sizeof rdna4_fmts / sizeof *rdna4_fmts; i++) if (rdna4_fmts[i].mtl == f) return ((rdna4_fmts[i].flags & (RDNA4F_UINT | RDNA4F_SINT)) && rdna4_nouint()) ? NULL : &rdna4_fmts[i];
    return NULL;
}
// bundle 10: the depth / stencil formats live in their OWN table, so rdna4_fmt (colour targets, IOSurface- and buffer-backed textures, views, heaps' colour sizing) answers for them exactly as before: NULL.
// Depth24Unorm_Stencil8 is not in it (AMD has no D24S8 on this path).  The numbers are tied to garnet_depth.h by the asserts below.
#define RDNA4F_DS 2u
static const RDNA4Fmt rdna4_dsfmts[] = {
    { MTLPixelFormatDepth16Unorm,          VK_FORMAT_D16_UNORM,          2, RDNA4F_DS },
    { MTLPixelFormatDepth32Float,          VK_FORMAT_D32_SFLOAT,         4, RDNA4F_DS },
    { MTLPixelFormatStencil8,              VK_FORMAT_S8_UINT,            1, RDNA4F_DS },
    { MTLPixelFormatDepth32Float_Stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, RDNA4F_DS },
};
GARNET_INTERNAL const RDNA4Fmt *rdna4_fmt_ds(MTLPixelFormat f) {
    for (size_t i = 0; i < sizeof rdna4_dsfmts / sizeof *rdna4_dsfmts; i++) if (rdna4_dsfmts[i].mtl == f) return &rdna4_dsfmts[i];
    return NULL;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // the macOS 27 SDK deprecates Depth24Unorm_Stencil8, X24_Stencil8 and Managed: the asserts still name them (this bundle refuses the first two and never creates Managed depth textures)
_Static_assert((int)MTLPixelFormatDepth16Unorm == RDNA4DP_MTL_DEPTH16 && (int)MTLPixelFormatDepth32Float == RDNA4DP_MTL_DEPTH32 && (int)MTLPixelFormatStencil8 == RDNA4DP_MTL_STENCIL8 &&
               (int)MTLPixelFormatDepth24Unorm_Stencil8 == RDNA4DP_MTL_D24S8 && (int)MTLPixelFormatDepth32Float_Stencil8 == RDNA4DP_MTL_D32S8 && (int)MTLPixelFormatX32_Stencil8 == RDNA4DP_MTL_X32S8 &&
               (int)MTLPixelFormatX24_Stencil8 == RDNA4DP_MTL_X24S8, "MTLPixelFormat depth/stencil values");
_Static_assert((int)VK_FORMAT_D16_UNORM == RDNA4DP_VK_D16 && (int)VK_FORMAT_D32_SFLOAT == RDNA4DP_VK_D32 && (int)VK_FORMAT_S8_UINT == RDNA4DP_VK_S8 && (int)VK_FORMAT_D32_SFLOAT_S8_UINT == RDNA4DP_VK_D32S8, "VkFormat depth/stencil values");
_Static_assert((int)VK_IMAGE_ASPECT_COLOR_BIT == RDNA4DP_ASP_COLOR && (int)VK_IMAGE_ASPECT_DEPTH_BIT == RDNA4DP_ASP_DEPTH && (int)VK_IMAGE_ASPECT_STENCIL_BIT == RDNA4DP_ASP_STENCIL, "VkImageAspectFlagBits");
_Static_assert((int)VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT == RDNA4DP_FEAT_SAMPLED && (int)VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT == RDNA4DP_FEAT_DS_ATT, "VkFormatFeatureFlagBits");
_Static_assert((int)MTLTextureUsageShaderRead == RDNA4DP_USE_READ && (int)MTLTextureUsageShaderWrite == RDNA4DP_USE_WRITE && (int)MTLTextureUsageRenderTarget == RDNA4DP_USE_RT && (int)MTLTextureUsagePixelFormatView == RDNA4DP_USE_PFV, "MTLTextureUsage values (depth)");
_Static_assert((int)MTLStorageModeShared == RDNA4DP_ST_SHARED && (int)MTLStorageModeManaged == RDNA4DP_ST_MANAGED && (int)MTLStorageModePrivate == RDNA4DP_ST_PRIVATE && (int)MTLStorageModeMemoryless == RDNA4DP_ST_MEMORYLESS, "MTLStorageMode values");
#pragma clang diagnostic pop
_Static_assert((int)MTLTextureType2D == RDNA4DP_T_2D && (int)MTLTextureType2DMultisample == RDNA4DP_T_2DMS, "MTLTextureType values (depth)");
_Static_assert((int)MTLStoreActionDontCare == RDNA4SA_DONTCARE && (int)MTLStoreActionStore == RDNA4SA_STORE && (int)MTLStoreActionMultisampleResolve == RDNA4SA_RESOLVE &&
               (int)MTLStoreActionStoreAndMultisampleResolve == RDNA4SA_STORE_RESOLVE && (int)MTLStoreActionUnknown == RDNA4SA_UNKNOWN && (int)MTLStoreActionCustomSampleDepthStore == RDNA4SA_CUSTOM, "MTLStoreAction values");
_Static_assert((int)MTLLoadActionDontCare == RDNA4LA_DONTCARE && (int)MTLLoadActionLoad == RDNA4LA_LOAD && (int)MTLLoadActionClear == RDNA4LA_CLEAR, "MTLLoadAction values");
_Static_assert((int)VK_ATTACHMENT_LOAD_OP_LOAD == RDNA4VK_LOAD && (int)VK_ATTACHMENT_LOAD_OP_CLEAR == RDNA4VK_CLEAR && (int)VK_ATTACHMENT_LOAD_OP_DONT_CARE == RDNA4VK_LOAD_DONT_CARE &&
               (int)VK_ATTACHMENT_STORE_OP_STORE == RDNA4VK_STORE && (int)VK_ATTACHMENT_STORE_OP_DONT_CARE == RDNA4VK_STORE_DONT_CARE, "VkAttachmentLoadOp / StoreOp values");
_Static_assert((int)MTLCompareFunctionNever == RDNA4DS_CMP_NEVER && (int)MTLCompareFunctionLess == RDNA4DS_CMP_LESS && (int)MTLCompareFunctionEqual == RDNA4DS_CMP_EQUAL && (int)MTLCompareFunctionLessEqual == RDNA4DS_CMP_LE &&
               (int)MTLCompareFunctionGreater == RDNA4DS_CMP_GREATER && (int)MTLCompareFunctionNotEqual == RDNA4DS_CMP_NE && (int)MTLCompareFunctionGreaterEqual == RDNA4DS_CMP_GE && (int)MTLCompareFunctionAlways == RDNA4DS_CMP_ALWAYS, "MTLCompareFunction values");
_Static_assert((int)VK_COMPARE_OP_NEVER == RDNA4DS_CMP_NEVER && (int)VK_COMPARE_OP_LESS == RDNA4DS_CMP_LESS && (int)VK_COMPARE_OP_EQUAL == RDNA4DS_CMP_EQUAL && (int)VK_COMPARE_OP_LESS_OR_EQUAL == RDNA4DS_CMP_LE &&
               (int)VK_COMPARE_OP_GREATER == RDNA4DS_CMP_GREATER && (int)VK_COMPARE_OP_NOT_EQUAL == RDNA4DS_CMP_NE && (int)VK_COMPARE_OP_GREATER_OR_EQUAL == RDNA4DS_CMP_GE && (int)VK_COMPARE_OP_ALWAYS == RDNA4DS_CMP_ALWAYS, "VkCompareOp values");
_Static_assert((int)MTLStencilOperationKeep == RDNA4DS_OP_KEEP && (int)MTLStencilOperationZero == RDNA4DS_OP_ZERO && (int)MTLStencilOperationReplace == RDNA4DS_OP_REPLACE && (int)MTLStencilOperationIncrementClamp == RDNA4DS_OP_INC_CLAMP &&
               (int)MTLStencilOperationDecrementClamp == RDNA4DS_OP_DEC_CLAMP && (int)MTLStencilOperationInvert == RDNA4DS_OP_INVERT && (int)MTLStencilOperationIncrementWrap == RDNA4DS_OP_INC_WRAP && (int)MTLStencilOperationDecrementWrap == RDNA4DS_OP_DEC_WRAP, "MTLStencilOperation values");
_Static_assert((int)VK_STENCIL_OP_KEEP == RDNA4DS_OP_KEEP && (int)VK_STENCIL_OP_ZERO == RDNA4DS_OP_ZERO && (int)VK_STENCIL_OP_REPLACE == RDNA4DS_OP_REPLACE && (int)VK_STENCIL_OP_INCREMENT_AND_CLAMP == RDNA4DS_OP_INC_CLAMP &&
               (int)VK_STENCIL_OP_DECREMENT_AND_CLAMP == RDNA4DS_OP_DEC_CLAMP && (int)VK_STENCIL_OP_INVERT == RDNA4DS_OP_INVERT && (int)VK_STENCIL_OP_INCREMENT_AND_WRAP == RDNA4DS_OP_INC_WRAP && (int)VK_STENCIL_OP_DECREMENT_AND_WRAP == RDNA4DS_OP_DEC_WRAP, "VkStencilOp values");
_Static_assert((int)VK_DYNAMIC_STATE_VIEWPORT == RDNA4VK_DYN_VIEWPORT && (int)VK_DYNAMIC_STATE_SCISSOR == RDNA4VK_DYN_SCISSOR && (int)VK_DYNAMIC_STATE_DEPTH_BIAS == RDNA4VK_DYN_DEPTH_BIAS && (int)VK_DYNAMIC_STATE_BLEND_CONSTANTS == RDNA4VK_DYN_BLEND_CONSTANTS &&
               (int)VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK == RDNA4VK_DYN_STENCIL_COMPARE_MASK && (int)VK_DYNAMIC_STATE_STENCIL_WRITE_MASK == RDNA4VK_DYN_STENCIL_WRITE_MASK && (int)VK_DYNAMIC_STATE_STENCIL_REFERENCE == RDNA4VK_DYN_STENCIL_REFERENCE, "VkDynamicState values");
_Static_assert((int)VK_SAMPLE_COUNT_2_BIT == 2 && (int)VK_SAMPLE_COUNT_4_BIT == 4 && (int)VK_SAMPLE_COUNT_8_BIT == 8, "VkSampleCountFlagBits are the counts");
// The device's sample counts that are usable for BOTH colour and depth/stencil framebuffer attachments.  0 until RADV is open.
GARNET_INTERNAL unsigned rdna4_ms_mask(void) {
    if (!RDNA4R.ok) return 0;
    return RDNA4R.lim.framebufferColorSampleCounts & RDNA4R.lim.framebufferDepthSampleCounts & RDNA4R.lim.framebufferStencilSampleCounts;
}
// RADV's optimal-tiling features of a format (RADV must be open). 0 when the entry point is missing.
GARNET_INTERNAL VkFormatFeatureFlags rdna4_fmt_feats(const RDNA4Fmt *f) {
    VkFormatProperties fp = {0};
    if (vkGetPhysicalDeviceFormatProperties && f) vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, f->vk, &fp);
    return fp.optimalTilingFeatures;
}

GARNET_INTERNAL void rdna4_add_protocols(Class c, const char **names, unsigned n, const char *tag) {
    for (unsigned i = 0; i < n; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        BOOL added = p ? class_addProtocol(c, p) : NO;
        if (!p || (!added && !class_conformsToProtocol(c, p)))
            os_log_error(rdna4_oslog(), "GarnetMetal: +load %{public}s protocol %{public}s found %d added %d", tag, names[i], p != nil, added);
        else if (rdna4_verbose_log())
            os_log(rdna4_oslog(), "GarnetMetal: +load %{public}s protocol %{public}s found %d added %d", tag, names[i], p != nil, added);
    }
}

@class RDNA4CommandBuffer;
@class RDNA4Texture;
// ---------------------------------------------------------------------------------------------------------------
// RDNA4CommandQueue / RDNA4CommandBuffer (10b). A shared scheduler advances only each logical queue's
// head. Event positions split the Vulkan recording into ordered segments; each segment has its own fence.
// Every segment boundary flushes IOSurface writes; a signal also makes them host-visible before its fence. A concurrent waiter queue
// waits each fence in 2 s slices without blocking the serial completion queue, which then signals that segment's events on success and
// calls commandBufferDidComplete:startTime:completionTime:error: only after the terminal segment.
// 11e: the command buffer owns the bytes upload ring and non-resource command
// state. Each Vulkan command segment owns its descriptor pools and the strong
// resource snapshots encoded into that segment; both are released at that
// segment's fence rather than at terminal command-buffer completion. A sticky
// NSError is delivered as the completion error instead of submitting.
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4CommandSegment
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    waits = [NSMutableArray array];
    signals = [NSMutableArray array];
    fenceWaits = [NSMutableArray array];
    fenceUpdates = [NSMutableArray array];
    touches = [NSMutableArray array];
    argumentIntents = [NSMutableArray array];
    argumentRefs = [NSMutableSet set];
    commandRefs = [NSMutableSet set];
    argumentPools = [NSMutableArray array];
    descriptorPools = [NSMutableArray array];
    return self;
}
@end

static pthread_mutex_t rdna4_event_state_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMapTable *rdna4_plain_event_values;
static NSMapTable *rdna4_event_failures;

static NSMapTable *rdna4_plain_event_map(void) {
    if (!rdna4_plain_event_values)
        rdna4_plain_event_values = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                                        valueOptions:NSPointerFunctionsStrongMemory];
    return rdna4_plain_event_values;
}

static NSMapTable *rdna4_event_failure_map(void) {
    if (!rdna4_event_failures)
        rdna4_event_failures = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                                    valueOptions:NSPointerFunctionsStrongMemory];
    return rdna4_event_failures;
}

GARNET_INTERNAL BOOL rdna4_event_is_shared(id event) {
    return [event respondsToSelector:@selector(signaledValue)] &&
        [event respondsToSelector:@selector(setSignaledValue:)];
}

GARNET_INTERNAL uint64_t rdna4_event_value(id event) {
    if (rdna4_event_is_shared(event)) return [(id<MTLSharedEvent>)event signaledValue];
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSNumber *value = [rdna4_plain_event_map() objectForKey:event];
    pthread_mutex_unlock(&rdna4_event_state_mu);
    return value ? value.unsignedLongLongValue : 0;
}

GARNET_INTERNAL void rdna4_event_set_value(id event, uint64_t value) {
    if (rdna4_event_is_shared(event)) {
        [(id<MTLSharedEvent>)event setSignaledValue:value];
        return;
    }
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSMapTable *values = rdna4_plain_event_map();
    NSNumber *old = [values objectForKey:event];
    if (!old || old.unsignedLongLongValue < value) [values setObject:@(value) forKey:event];
    pthread_mutex_unlock(&rdna4_event_state_mu);
}

GARNET_INTERNAL void rdna4_event_fail_signals(NSArray *signals) {
    if (!signals.count) return;
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSMapTable *failures = rdna4_event_failure_map();
    for (NSArray *s in signals) {
        NSNumber *old = [failures objectForKey:s[0]];
        uint64_t value = [s[1] unsignedLongLongValue];
        if (!old || old.unsignedLongLongValue < value) [failures setObject:@(value) forKey:s[0]];
    }
    pthread_mutex_unlock(&rdna4_event_state_mu);
}

GARNET_INTERNAL BOOL rdna4_event_failed(id event, uint64_t value) {
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSNumber *failed = rdna4_event_failures ? [rdna4_event_failures objectForKey:event] : nil;
    pthread_mutex_unlock(&rdna4_event_state_mu);
    return failed && failed.unsignedLongLongValue >= value;
}

static pthread_mutex_t rdna4_schedule_mu = PTHREAD_MUTEX_INITIALIZER;
static NSHashTable *rdna4_schedule_queues;
static dispatch_queue_t rdna4_schedule_q;
static BOOL rdna4_schedule_active, rdna4_schedule_dirty, rdna4_schedule_retry;
static char rdna4_cq_key;

static dispatch_queue_t rdna4_serial_interactive_q(const char *label) {
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    return dispatch_queue_create(label, attr);
}

static void rdna4_schedule_register(RDNA4CommandQueue *queue) {
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_queues) rdna4_schedule_queues = [NSHashTable weakObjectsHashTable];
    if (!rdna4_schedule_q) rdna4_schedule_q = rdna4_serial_interactive_q("rdna4.event-scheduler");
    [rdna4_schedule_queues addObject:queue];
    pthread_mutex_unlock(&rdna4_schedule_mu);
}

static void rdna4_schedule_unregister(RDNA4CommandQueue *queue) {
    pthread_mutex_lock(&rdna4_schedule_mu);
    [rdna4_schedule_queues removeObject:queue];
    pthread_mutex_unlock(&rdna4_schedule_mu);
}

GARNET_INTERNAL void rdna4_schedule_kick(void) {
    dispatch_queue_t queue = nil;
    BOOL launch = NO;
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_q) rdna4_schedule_q = rdna4_serial_interactive_q("rdna4.event-scheduler");
    queue = rdna4_schedule_q;
    if (rdna4_schedule_active) rdna4_schedule_dirty = YES;
    else { rdna4_schedule_active = YES; rdna4_schedule_dirty = NO; launch = YES; }
    pthread_mutex_unlock(&rdna4_schedule_mu);
    if (!launch) return;
    dispatch_async(queue, ^{
        for (;;) {
            NSArray *queues = nil;
            BOOL waiting = NO;
            pthread_mutex_lock(&rdna4_schedule_mu);
            queues = rdna4_schedule_queues ? rdna4_schedule_queues.allObjects : @[];
            pthread_mutex_unlock(&rdna4_schedule_mu);
            for (RDNA4CommandQueue *q in queues) waiting |= [q rdna4AdvanceHead];
            pthread_mutex_lock(&rdna4_schedule_mu);
            if (rdna4_schedule_dirty) {
                rdna4_schedule_dirty = NO;
                pthread_mutex_unlock(&rdna4_schedule_mu);
                continue;
            }
            rdna4_schedule_active = NO;
            BOOL retry = waiting && !rdna4_schedule_retry;
            if (retry) rdna4_schedule_retry = YES;
            dispatch_queue_t q = rdna4_schedule_q;
            pthread_mutex_unlock(&rdna4_schedule_mu);
            if (retry) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC), q, ^{
                    pthread_mutex_lock(&rdna4_schedule_mu);
                    rdna4_schedule_retry = NO;
                    pthread_mutex_unlock(&rdna4_schedule_mu);
                    rdna4_schedule_kick();
                });
            }
            return;
        }
    });
}

@implementation RDNA4CommandQueue
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    id qd = desc ? [desc copy] : [[NSClassFromString(@"MTLCommandQueueDescriptor") alloc] init];
    if ([[qd valueForKey:@"maxCommandBufferCount"] unsignedIntegerValue] == 0) [qd setValue:@64 forKey:@"maxCommandBufferCount"];   // base asserts on 0
    if (!desc && [qd respondsToSelector:NSSelectorFromString(@"setCommitSynchronously:")] &&
        !rdna4_sb_access_exists("/private/tmp/garnet-async-commit"))
        [qd setValue:@YES forKey:@"commitSynchronously"];
    self = [super initWithDevice:dev descriptor:qd];
    if (!self) { if (err) *err = rdna4_err(50, @"_MTLCommandQueue initWithDevice:descriptor: returned nil"); return nil; }
    pthread_mutex_init(&_qmu, NULL);
    garnet_vkpool_init(&_cmdPool, GARNET_VKPOOL_CMD_CAP);
    garnet_vkpool_init(&_computeCmdPool, GARNET_VKPOOL_CMD_CAP);
    _computePool = VK_NULL_HANDLE;
    _orderSem = VK_NULL_HANDLE;
    _order = (garnet_async_qorder_t){ 0, 0 };
    atomic_init(&_orderLastWasCompute, 0);
    const unsigned maxInFlight = rdna4_sb_access_exists(GARNET_SERIAL_QUEUE_FILE)
        ? GARNET_QP_SERIAL_MAX_INFLIGHT : GARNET_QP_DEFAULT_MAX_INFLIGHT;
    garnet_qp_init(&_qp, maxInFlight);
    _launchedCount = 0;
    _pending = [NSMutableArray array];
    _residencySets = [NSMutableArray array];
    // The base deallocator needs its dispatch objects even when RADV is unavailable.
    if (!rdna4_radv_open(err)) return nil;
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = RDNA4R.qfi,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkResult r = vkCreateCommandPool(RDNA4R.dev, &cpc, NULL, &_pool);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"vkCreateCommandPool = %d", r]); return nil; }
    if (RDNA4R.asyncComputeOn && vkCreateSemaphore && vkDestroySemaphore) {
        VkSemaphoreTypeCreateInfo stci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci };
        VkResult sr = vkCreateSemaphore(RDNA4R.dev, &sci, NULL, &_orderSem);
        if (sr != VK_SUCCESS) {
            _orderSem = VK_NULL_HANDLE;
            RDNA4LOG_WARN("RDNA4CommandQueue %p: timeline semaphore create failed (%d); staying on GFX queue", (__bridge void *)self, sr);
        }
    }
    _ndev = dev;
    _cq = rdna4_serial_interactive_q("rdna4.completion");
    dispatch_queue_set_specific(_cq, &rdna4_cq_key, (__bridge void *)self, NULL);
    rdna4_schedule_register(self);
    RDNA4LOG("RDNA4CommandQueue %p: pool %p", (__bridge void *)self, (void *)_pool);
    return self;
}
- (void)dealloc {
    rdna4_schedule_unregister(self);
    garnet_vkpool_drain(&_cmdPool, NULL, GARNET_VKPOOL_CMD_CAP);
    garnet_vkpool_drain(&_computeCmdPool, NULL, GARNET_VKPOOL_CMD_CAP);
    if (RDNA4R.ok && _computePool) vkDestroyCommandPool(RDNA4R.dev, _computePool, NULL);
    if (RDNA4R.ok && _orderSem && vkDestroySemaphore) vkDestroySemaphore(RDNA4R.dev, _orderSem, NULL);
    if (RDNA4R.ok && _pool) vkDestroyCommandPool(RDNA4R.dev, _pool, NULL);
    pthread_mutex_destroy(&_qmu);
}
RDNA4_DNR(RDNA4CommandQueue)
- (VkCommandPool)pool { return _pool; }
- (VkCommandPool)computePool { return _computePool; }
- (VkSemaphore)rdna4OrderSemaphore { return _orderSem; }
- (BOOL)rdna4OrderLastWasCompute {
    // Lock-free: called from rdna4NativeEligible -> rdna4PipelineEligible while caller already holds _qmu.
    return _orderSem != VK_NULL_HANDLE &&
           atomic_load_explicit(&_orderLastWasCompute, memory_order_acquire) != 0;
}
- (uint64_t)rdna4PendingCrossQueueWaitForCompute:(BOOL)isCompute {
    pthread_mutex_lock(&_qmu);
    const uint64_t waitVal = garnet_async_qorder_cross_wait(&_order, isCompute ? 1 : 0);
    pthread_mutex_unlock(&_qmu);
    return waitVal;
}
- (garnet_async_submit_order_t)rdna4NextOrderForCompute:(BOOL)isCompute {
    pthread_mutex_lock(&_qmu);
    garnet_async_submit_order_t ord = garnet_async_qorder_next(&_order, isCompute ? 1 : 0);
    atomic_store_explicit(&_orderLastWasCompute, isCompute ? 1 : 0, memory_order_release);
    pthread_mutex_unlock(&_qmu);
    return ord;
}
- (VkCommandBuffer)rdna4TakeCmdBuf:(NSError **)err {
    if (rdna4_cmd_pool_active()) {
        pthread_mutex_lock(&_qmu);
        VkCommandBuffer cached = (VkCommandBuffer)(uintptr_t)garnet_vkpool_pop(&_cmdPool);
        pthread_mutex_unlock(&_qmu);
        if (cached != VK_NULL_HANDLE) return cached;
    }
    VkCommandBuffer vk = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = _pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    rdna4_qlock();
    VkResult r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &vk);
    pthread_mutex_unlock(&RDNA4R.qlock);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkAllocateCommandBuffers(segment) = %d", r]);
        return VK_NULL_HANDLE;
    }
    return vk;
}
// Caller already holds RDNA4R.qlock. Lock order: RDNA4R.qlock -> _qmu (never reversed).
- (VkCommandBuffer)rdna4TakeCmdBufLocked:(NSError **)err {
    if (rdna4_cmd_pool_active()) {
        pthread_mutex_lock(&_qmu);
        VkCommandBuffer cached = (VkCommandBuffer)(uintptr_t)garnet_vkpool_pop(&_cmdPool);
        pthread_mutex_unlock(&_qmu);
        if (cached != VK_NULL_HANDLE) return cached;
    }
    VkCommandBuffer vk = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = _pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkResult r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &vk);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkAllocateCommandBuffers(dispCopy) = %d", r]);
        return VK_NULL_HANDLE;
    }
    return vk;
}
- (void)rdna4GiveCmdBuf:(VkCommandBuffer)cmd {
    if (!cmd || !RDNA4R.ok) return;
    if (rdna4_cmd_pool_active()) {
        rdna4_qlock();
        VkResult rr = vkResetCommandBuffer(cmd, 0);
        pthread_mutex_unlock(&RDNA4R.qlock);
        if (rr == VK_SUCCESS) {
            pthread_mutex_lock(&_qmu);
            int kept = garnet_vkpool_push(&_cmdPool, (uint64_t)(uintptr_t)cmd);
            pthread_mutex_unlock(&_qmu);
            if (kept) return;
        }
    }
    rdna4_qlock();
    vkFreeCommandBuffers(RDNA4R.dev, _pool, 1, &cmd);
    pthread_mutex_unlock(&RDNA4R.qlock);
}
- (VkCommandBuffer)rdna4TakeComputeCmdBuf:(NSError **)err {
    if (!RDNA4R.asyncComputeOn) return VK_NULL_HANDLE;
    if (rdna4_cmd_pool_active()) {
        pthread_mutex_lock(&_qmu);
        VkCommandBuffer cached = (VkCommandBuffer)(uintptr_t)garnet_vkpool_pop(&_computeCmdPool);
        pthread_mutex_unlock(&_qmu);
        if (cached != VK_NULL_HANDLE) return cached;
    }
    VkCommandBuffer vk = VK_NULL_HANDLE;
    VkResult r = VK_SUCCESS;
    rdna4_compute_qlock();
    if (!_computePool) {
        VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = RDNA4R.computeQfi,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        r = vkCreateCommandPool(RDNA4R.dev, &cpc, NULL, &_computePool);
    }
    if (r == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = _computePool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &vk);
    }
    pthread_mutex_unlock(&RDNA4R.computeQlock);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkAllocateCommandBuffers(compute) = %d", r]);
        return VK_NULL_HANDLE;
    }
    return vk;
}
- (void)rdna4GiveComputeCmdBuf:(VkCommandBuffer)cmd {
    if (!cmd || !RDNA4R.ok || !_computePool) return;
    if (rdna4_cmd_pool_active()) {
        rdna4_compute_qlock();
        VkResult rr = vkResetCommandBuffer(cmd, 0);
        pthread_mutex_unlock(&RDNA4R.computeQlock);
        if (rr == VK_SUCCESS) {
            pthread_mutex_lock(&_qmu);
            int kept = garnet_vkpool_push(&_computeCmdPool, (uint64_t)(uintptr_t)cmd);
            pthread_mutex_unlock(&_qmu);
            if (kept) return;
        }
    }
    rdna4_compute_qlock();
    vkFreeCommandBuffers(RDNA4R.dev, _computePool, 1, &cmd);
    pthread_mutex_unlock(&RDNA4R.computeQlock);
}
- (BOOL)rdna4HasResidencySets {
    return atomic_load_explicit(&_residencySetCount, memory_order_acquire) != 0u;
}
- (id)device { return _ndev; }
- (dispatch_queue_t)rdna4CompletionQueue { return _cq; }
- (void)rdna4Kick { rdna4_schedule_kick(); }
- (void)rdna4DrainOrderedCompletionsOnCQ {
    BOOL needKick = NO;
    for (;;) {
        RDNA4CommandBuffer *head = nil;
        void (^run)(void) = nil;
        BOOL headFailed = NO;
        pthread_mutex_lock(&self->_qmu);
        if (self->_pending.count) {
            RDNA4CommandBuffer *first = self->_pending.firstObject;
            if (first->_fifoDoneBlock) {
                head = first;
                run = first->_fifoDoneBlock;
                headFailed = first->_fifoDoneFailed;
                first->_fifoDoneBlock = nil;
                [self->_pending removeObjectAtIndex:0];
                if (self->_launchedCount > 0) self->_launchedCount--;
                garnet_qp_on_complete(&self->_qp, head->_launchedAsBarrier ? 1 : 0, headFailed ? 1 : 0);
                head->_launchedAsBarrier = NO;
                if (self->_pending.count > self->_launchedCount || [head rdna4HasEvents]) needKick = YES;
            }
        }
        pthread_mutex_unlock(&self->_qmu);
        if (!run) break;
        run();
    }
    if (needKick) rdna4_schedule_kick();
}
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb {
    [self rdna4DidComplete:cb failed:NO];
}
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb failed:(BOOL)failed {
    BOOL removed = NO, drainNext = NO, needKick = NO;
    pthread_mutex_lock(&_qmu);
    if (_pending.firstObject == cb) {
        [_pending removeObjectAtIndex:0];
        if (_launchedCount > 0) _launchedCount--;
        garnet_qp_on_complete(&_qp, cb->_launchedAsBarrier ? 1 : 0, failed ? 1 : 0);
        cb->_launchedAsBarrier = NO;
        removed = YES;
        if (_pending.count && _pending.firstObject->_fifoDoneBlock) drainNext = YES;
        if (_pending.count > _launchedCount || [cb rdna4HasEvents]) needKick = YES;
    }
    pthread_mutex_unlock(&_qmu);
    if (!removed) RDNA4LOG("completion for a command buffer that is no longer this queue's head");
    if (drainNext) {
        if (dispatch_get_specific(&rdna4_cq_key) == (__bridge void *)self) [self rdna4DrainOrderedCompletionsOnCQ];
        else dispatch_async(_cq, ^{ [self rdna4DrainOrderedCompletionsOnCQ]; });
    }
    if (needKick) rdna4_schedule_kick();
}
- (void)rdna4EnqueueOrderedCompletion:(RDNA4CommandBuffer *)cb failed:(BOOL)failed block:(void (^)(void))block {
    void (^copied)(void) = [block copy];
    if (dispatch_get_specific(&rdna4_cq_key) == (__bridge void *)self) {
        cb->_fifoDoneBlock = copied;
        cb->_fifoDoneFailed = failed;
        [self rdna4DrainOrderedCompletionsOnCQ];
        return;
    }
    dispatch_async(_cq, ^{
        cb->_fifoDoneBlock = copied;
        cb->_fifoDoneFailed = failed;
        [self rdna4DrainOrderedCompletionsOnCQ];
    });
}
- (void)rdna4NoteElidedDCopy {
    pthread_mutex_lock(&_qmu);
    _qp.elided_dcopies++;
    pthread_mutex_unlock(&_qmu);
}
- (BOOL)rdna4PendingHasNewerSidLocked:(uint32_t)sid fromIndex:(NSUInteger)idx {
    if (!sid) return NO;
    for (NSUInteger i = idx + 1; i < _pending.count; i++) {
        if ([_pending[i] rdna4DisplayTargetSid] == sid) return YES;
    }
    return NO;
}
- (BOOL)rdna4PendingHasNewerSid:(uint32_t)sid after:(RDNA4CommandBuffer *)cb {
    if (!sid || !cb) return NO;
    BOOL found = NO;
    pthread_mutex_lock(&_qmu);
    NSUInteger idx = [_pending indexOfObjectIdenticalTo:cb];
    if (idx != NSNotFound) found = [self rdna4PendingHasNewerSidLocked:sid fromIndex:idx];
    pthread_mutex_unlock(&_qmu);
    return found;
}
- (void)rdna4ArmDeferTimer:(uint64_t)deferNs {
    pthread_mutex_lock(&_qmu);
    if (_deferTimerArmed) { pthread_mutex_unlock(&_qmu); return; }
    _deferTimerArmed = YES;
    pthread_mutex_unlock(&_qmu);
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_q) rdna4_schedule_q = rdna4_serial_interactive_q("rdna4.event-scheduler");
    dispatch_queue_t sq = rdna4_schedule_q;
    pthread_mutex_unlock(&rdna4_schedule_mu);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)deferNs), sq, ^{
        pthread_mutex_lock(&self->_qmu);
        self->_deferTimerArmed = NO;
        pthread_mutex_unlock(&self->_qmu);
        rdna4_schedule_kick();
    });
}
- (BOOL)rdna4AdvanceHead {
    for (;;) {
        RDNA4CommandBuffer *cb = nil;
        BOOL reAdvanceBarrier = NO;
        BOOL eligible = NO;
        BOOL checkDefer = NO;
        RDNA4CommandBuffer *deferCand = nil;
        pthread_mutex_lock(&_qmu);
        if (_submitting) {
            _scheduleKickNeeded = YES;
            pthread_mutex_unlock(&_qmu);
            return NO;
        }
        if (_qp.barrier_active && _launchedCount > 0 && _pending.count > 0) {
            cb = _pending.firstObject;
            reAdvanceBarrier = YES;
        } else if (_launchedCount < _pending.count) {
            RDNA4CommandBuffer *cand = _pending[_launchedCount];
            eligible = [cand rdna4PipelineEligible];
            if (garnet_qp_can_launch(&_qp, eligible ? 1 : 0)) {
                if (eligible && [cand rdna4MayDeferFinal] &&
                    ![self rdna4PendingHasNewerSidLocked:[cand rdna4DisplayTargetSid] fromIndex:_launchedCount]) {
                    checkDefer = YES;
                    deferCand = cand;
                } else {
                    cb = cand;
                    garnet_qp_on_launch(&_qp, eligible ? 1 : 0);
                    cand->_launchedAsBarrier = !eligible;
                    _launchedCount++;
                    if (eligible) _submitting = YES;
                }
            }
        }
        pthread_mutex_unlock(&_qmu);
        if (checkDefer) {
            const uint64_t deferNs = [deferCand rdna4FinalDeferNs];
            if (deferNs > 0) {
                [self rdna4ArmDeferTimer:deferNs];
                return NO;
            }
            pthread_mutex_lock(&_qmu);
            if (_submitting) {
                _scheduleKickNeeded = YES;
                pthread_mutex_unlock(&_qmu);
                return NO;
            }
            if (!_qp.barrier_active && _launchedCount < _pending.count &&
                _pending[_launchedCount] == deferCand && garnet_qp_can_launch(&_qp, 1)) {
                cb = deferCand;
                garnet_qp_on_launch(&_qp, 1);
                cb->_launchedAsBarrier = NO;
                _launchedCount++;
                _submitting = YES;
            }
            pthread_mutex_unlock(&_qmu);
        }
        if (!cb) return NO;
        BOOL waiting = [cb rdna4AdvanceWithQueue:self];
        BOOL needKick = NO;
        if (waiting || eligible) {
            pthread_mutex_lock(&_qmu);
            if (waiting && !reAdvanceBarrier && _launchedCount > 0 && _pending[_launchedCount - 1] == cb) {
                _launchedCount--;
                garnet_qp_on_complete(&_qp, !eligible, 0);
                cb->_launchedAsBarrier = NO;
            }
            if (eligible) {
                _submitting = NO;
                if (_scheduleKickNeeded) { _scheduleKickNeeded = NO; needKick = YES; }
            }
            pthread_mutex_unlock(&_qmu);
        }
        if (needKick) rdna4_schedule_kick();
        if (waiting) return YES;
        if (reAdvanceBarrier || !eligible) return NO;
        if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) return NO;
    }
}
- (void)rdna4TryInlineAdvance {
    BOOL needKick = NO;
    for (;;) {
        RDNA4CommandBuffer *cb = nil;
        RDNA4CommandBuffer *deferCand = nil;
        pthread_mutex_lock(&_qmu);
        if (_submitting) {
            _scheduleKickNeeded = YES;
            pthread_mutex_unlock(&_qmu);
            return;
        }
        if (_qp.barrier_active) {
            needKick = YES;
        } else if (_launchedCount < _pending.count) {
            RDNA4CommandBuffer *cand = _pending[_launchedCount];
            if (![cand rdna4PipelineEligible]) {
                if (garnet_qp_can_launch(&_qp, 0)) needKick = YES;
            } else if (garnet_qp_can_launch(&_qp, 1)) {
                if ([cand rdna4MayDeferFinal] &&
                    ![self rdna4PendingHasNewerSidLocked:[cand rdna4DisplayTargetSid] fromIndex:_launchedCount]) {
                    deferCand = cand;
                } else {
                    cb = cand;
                    garnet_qp_on_launch(&_qp, 1);
                    cb->_launchedAsBarrier = NO;
                    _launchedCount++;
                    _submitting = YES;
                }
            }
        }
        pthread_mutex_unlock(&_qmu);
        if (deferCand) {
            const uint64_t deferNs = [deferCand rdna4FinalDeferNs];
            if (deferNs > 0) {
                [self rdna4ArmDeferTimer:deferNs];
                return;
            }
            pthread_mutex_lock(&_qmu);
            if (_submitting) {
                _scheduleKickNeeded = YES;
                pthread_mutex_unlock(&_qmu);
                return;
            }
            if (!_qp.barrier_active && _launchedCount < _pending.count &&
                _pending[_launchedCount] == deferCand && garnet_qp_can_launch(&_qp, 1)) {
                cb = deferCand;
                garnet_qp_on_launch(&_qp, 1);
                cb->_launchedAsBarrier = NO;
                _launchedCount++;
                _submitting = YES;
            }
            pthread_mutex_unlock(&_qmu);
        }
        if (!cb) break;
        BOOL waiting = [cb rdna4AdvanceWithQueue:self];
        pthread_mutex_lock(&_qmu);
        if (waiting && _launchedCount > 0 && _pending[_launchedCount - 1] == cb) {
            _launchedCount--;
            garnet_qp_on_complete(&_qp, 0, 0);
            needKick = YES;
        }
        _submitting = NO;
        if (_scheduleKickNeeded) { _scheduleKickNeeded = NO; needKick = YES; }
        pthread_mutex_unlock(&_qmu);
        if (waiting || atomic_load(&rdna4_submit_fault) != VK_SUCCESS) break;
    }
    if (needKick) rdna4_schedule_kick();
}
// ---- gap census: MTLCommandQueueSPI, sent by SkyLight right after -newCommandQueue (census rdt1). Priorities are recorded; there is one hardware queue. ----
// Return values as measured on an Apple-silicon Mac (mtlprobe apigaps): setGPUPriority: YES, the background setters NO.
- (BOOL)setGPUPriority:(NSUInteger)p { _gprio = p; RDNA4_ONCE("queue setGPUPriority: recorded only"); return YES; }
- (BOOL)setGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _gprio = p; return YES; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p { _bprio = p; return NO; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _bprio = p; return NO; }
- (BOOL)_setGPUPriority:(NSUInteger)p backgroundPriority:(NSUInteger)b { _gprio = p; _bprio = b; RDNA4_ONCE("queue _setGPUPriority:backgroundPriority: recorded only"); return NO; }
- (NSUInteger)getGPUPriority { return _gprio; }
- (NSUInteger)getBackgroundGPUPriority { return _bprio; }
- (void)setCompletionQueue:(id)q { _compQ = q; RDNA4_ONCE("queue setCompletionQueue: recorded only (completion handlers run on the bundle's completion thread)"); }
- (void)addResidencySet:(id)residencySet {
    if (!residencySet) return;
    if (![residencySet respondsToSelector:@selector(rdna4CommittedSnapshot)] ||
        ![residencySet respondsToSelector:@selector(device)] ||
        [residencySet device] != _ndev) {
        RDNA4_ONCE("queue addResidencySet: invalid residency set or device mismatch; ignored");
        return;
    }
    pthread_mutex_lock(&_qmu);
    if ([_residencySets indexOfObjectIdenticalTo:residencySet] == NSNotFound) {
        [_residencySets addObject:residencySet];
        atomic_store_explicit(&_residencySetCount, (uint32_t)_residencySets.count, memory_order_release);
    }
    pthread_mutex_unlock(&_qmu);
}
- (void)addResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (!residencySets || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self addResidencySet:residencySets[i]];
}
- (void)removeResidencySet:(id)residencySet {
    if (!residencySet) return;
    pthread_mutex_lock(&_qmu);
    [_residencySets removeObjectIdenticalTo:residencySet];
    atomic_store_explicit(&_residencySetCount, (uint32_t)_residencySets.count, memory_order_release);
    pthread_mutex_unlock(&_qmu);
}
- (void)removeResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (!residencySets || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self removeResidencySet:residencySets[i]];
}
- (NSArray *)rdna4ResidencySetSnapshots {
    if (atomic_load_explicit(&_residencySetCount, memory_order_acquire) == 0u) return @[];
    NSArray *sets = nil;
    pthread_mutex_lock(&_qmu);
    if (_residencySets.count) sets = [_residencySets copy];
    pthread_mutex_unlock(&_qmu);
    if (!sets.count) return @[];
    NSMutableArray *snaps = [NSMutableArray arrayWithCapacity:sets.count];
    for (id<RDNA4ResidencySetInternal> rs in sets) {
        id<RDNA4ResidencySnapshotInternal> snap = [rs rdna4CommittedSnapshot];
        if (snap) [snaps addObject:snap];
    }
    return snaps;
}
- (id)commandBuffer {
    RDNA4LOGR("queue commandBuffer");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (id)commandBufferWithUnretainedReferences {
    RDNA4LOGR("queue commandBufferWithUnretainedReferences");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:NO];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (id)commandBufferWithDescriptor:(id)d {
    (void)d; RDNA4LOGR("queue commandBufferWithDescriptor:");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (void)submitCommandBuffers:(id __unsafe_unretained const *)cbs count:(NSUInteger)n {
    RDNA4LOGR("queue submitCommandBuffers:count:%lu", (unsigned long)n);
    for (NSUInteger i = 0; i < n; i++) {
        RDNA4CommandBuffer *cb = (RDNA4CommandBuffer *)cbs[i];
        if (!cb) continue;
        uint64_t t0 = rdna4_now();
        rdna4t_add(&T1.enc, t0 - [cb rdna4TBegin]);
        [cb rdna4PrepareForSubmission];
        BOOL isFrame = [cb rdna4IsFrame];
        if (isFrame) { atomic_fetch_add(&rdna4_t1_frames, 1); uint64_t lf = atomic_exchange(&rdna4_t1_lastframe, t0); if (lf) rdna4t_add(&T1.fgap, t0 - lf); }
        pthread_mutex_lock(&_qmu);
        [_pending addObject:cb];
        pthread_mutex_unlock(&_qmu);
    }
    if (n) [self rdna4TryInlineAdvance];
}
// Base version runs right after submitCommandBuffers:count: (F4) and would complete before the GPU is done.
- (void)completeCommandBuffers:(id __unsafe_unretained *)cbs count:(NSUInteger)n {
    (void)cbs; RDNA4LOG("queue completeCommandBuffers:count:%lu (deferred to the completion thread)", (unsigned long)n);
}
@end
