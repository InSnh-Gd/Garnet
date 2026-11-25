// garnet_commands.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_luma_workgroup.h"

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
    // bundle 19 (NATIVE-S8-MENUS.md): pixel format 63. Core Animation's large-shadow distance-field pass (brim_init / brim_jump / brim_outline) ping-pongs two of these; the refusal made
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
    dispatch_once(&once, ^{ struct stat st; v = stat(RDNA4_NOUINT_FILE, &st) == 0; if (v) RDNA4LOG("integer colour formats (RG16Uint) OFF: kill file " RDNA4_NOUINT_FILE " exists (read once per process)"); });
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
        os_log(OS_LOG_DEFAULT, "GarnetMetal: +load %{public}s protocol %{public}s found %d added %d", tag, names[i], p != nil, added);
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



@class RDNA4CommandBuffer;

// CRC diagnostic, completion time (fence done, the copy ran): A1, B, A2. Returns the record the present step uses for C (nil when the check cannot run).
static rdna4crc_rec_t *rdna4s_crc_check(RDNA4Texture *dt, int slot, uint64_t seq) {
    const uint8_t *src = [dt rdna4IBase]; IOSurfaceRef ios = [dt rdna4IOSRef];
    uint8_t scratch[16384]; if (!src || !ios || RDNA4S.pitch > sizeof scratch || slot < 0 || slot >= RDNA4DF_MAX_SLOTS) return NULL;
    const int doslot = !atomic_load(&RDNA4C.noslot);
    const uint8_t *slotBase = NULL;
    if (doslot) {
        pthread_mutex_lock(&RDNA4S.mu);
        if (!RDNA4S.map[slot]) {
            void *m = NULL; VkResult mr = vkMapMemory(RDNA4R.dev, RDNA4S.mem[slot], 0, VK_WHOLE_SIZE, 0, &m);
            if (mr != VK_SUCCESS || !m) { if (!RDNA4C.mapfail++) RDNA4LOG("scanout: CRC diagnostic: vkMapMemory(slot %d) = %d: the slot CRC is skipped", slot, mr); pthread_mutex_unlock(&RDNA4S.mu); return NULL; }
            RDNA4S.map[slot] = m;
        }
        slotBase = RDNA4S.map[slot];
        pthread_mutex_unlock(&RDNA4S.mu);
    }
    rdna4crc_rec_t *r = calloc(1, sizeof *r); if (!r) return NULL;
    r->ios = (IOSurfaceRef)CFRetain(ios); r->base = src; r->pitch = RDNA4S.pitch; r->rowbytes = RDNA4S.pitch; r->h = RDNA4S.ph; r->step = atomic_load(&RDNA4C.step); r->sid = IOSurfaceGetID(ios); r->slot = slot; r->seq = seq;
    rdna4crc_samp_t a2, b; uint64_t t0 = rdna4_now(), ts = 0;
    rdna4crc_sample(&r->a1, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    if (doslot) { uint64_t t1 = rdna4_now(); rdna4crc_sample(&b, slotBase, r->pitch, r->rowbytes, r->h, r->step, scratch); ts = rdna4_now() - t1; }
    rdna4crc_sample(&a2, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    uint64_t dtn = rdna4_now() - t0;
    pthread_mutex_lock(&RDNA4C.mu);
    int later = 0; for (int i = 0; i < 8; i++) if (RDNA4C.surf[i].sid == r->sid) { later = RDNA4C.surf[i].seq > seq; break; }
    int f = rdna4crc_account(&RDNA4C.st, &r->a1, &a2, doslot ? &b : NULL, dtn, ts, later); r->unst = (f & RDNA4CRC_F_UNSTABLE) != 0;
    int lg = f && RDNA4C.st.logged < 20 ? (int)++RDNA4C.st.logged : 0;
    pthread_mutex_unlock(&RDNA4C.mu);
    if (lg) {
        uint32_t d1[8] = { 0 }, d2[8] = { 0 }; int n1 = doslot ? rdna4crc_diff(&r->a1, &b, d1, 8) : 0, n2 = rdna4crc_diff(&r->a1, &a2, d2, 8);
        RDNA4LOG("scanout: CRC MISMATCH #%d: seq %llu surface %u slot %d step %u:%s%s slot!=src rows %d (y: %u %u %u %u), src-unstable rows %d (y: %u %u %u %u), %s, check cost %.0f us (slot read %.0f us)", lg, (unsigned long long)seq, r->sid, slot, r->step,
               (f & RDNA4CRC_F_SLOT_NE) ? " SLOT_NE_SRC" : "", (f & RDNA4CRC_F_UNSTABLE) ? " SRC_UNSTABLE" : "", n1, d1[0], n1 > 1 ? d1[1] : 0, n1 > 2 ? d1[2] : 0, n1 > 3 ? d1[3] : 0,
               n2, d2[0], n2 > 1 ? d2[1] : 0, n2 > 2 ? d2[2] : 0, n2 > 3 ? d2[3] : 0,
               later ? "a LATER command buffer for this surface was submitted" : "NO later command buffer: written outside our command buffers", dtn / 1000.0, ts / 1000.0);
    }
    return r;
}



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


static void rdna4_schedule_register(RDNA4CommandQueue *queue) {
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_queues) rdna4_schedule_queues = [NSHashTable weakObjectsHashTable];
    if (!rdna4_schedule_q) rdna4_schedule_q = dispatch_queue_create("rdna4.event-scheduler", DISPATCH_QUEUE_SERIAL);
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
    if (!rdna4_schedule_q) rdna4_schedule_q = dispatch_queue_create("rdna4.event-scheduler", DISPATCH_QUEUE_SERIAL);
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
    self = [super initWithDevice:dev descriptor:qd];
    if (!self) { if (err) *err = rdna4_err(50, @"_MTLCommandQueue initWithDevice:descriptor: returned nil"); return nil; }
    pthread_mutex_init(&_qmu, NULL);
    garnet_vkpool_init(&_cmdPool, GARNET_VKPOOL_CMD_CAP);
    _pending = [NSMutableArray array];
    _residencySets = [NSMutableArray array];
    // The base deallocator needs its dispatch objects even when RADV is unavailable.
    if (!rdna4_radv_open(err)) return nil;
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = RDNA4R.qfi,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkResult r = vkCreateCommandPool(RDNA4R.dev, &cpc, NULL, &_pool);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"vkCreateCommandPool = %d", r]); return nil; }
    _ndev = dev; _cq = dispatch_queue_create("rdna4.completion", DISPATCH_QUEUE_SERIAL);
    rdna4_schedule_register(self);
    RDNA4LOG("RDNA4CommandQueue %p: pool %p", (__bridge void *)self, (void *)_pool);
    return self;
}
- (void)dealloc {
    rdna4_schedule_unregister(self);
    garnet_vkpool_drain(&_cmdPool, NULL, GARNET_VKPOOL_CMD_CAP);
    if (RDNA4R.ok && _pool) vkDestroyCommandPool(RDNA4R.dev, _pool, NULL);
    pthread_mutex_destroy(&_qmu);
}
RDNA4_DNR(RDNA4CommandQueue)
- (VkCommandPool)pool { return _pool; }
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
- (BOOL)rdna4HasResidencySets {
    return atomic_load_explicit(&_residencySetCount, memory_order_relaxed) != 0u;
}
- (id)device { return _ndev; }
- (dispatch_queue_t)rdna4CompletionQueue { return _cq; }
- (void)rdna4Kick { rdna4_schedule_kick(); }
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb {
    BOOL removed = NO;
    pthread_mutex_lock(&_qmu);
    if (_pending.firstObject == cb) { [_pending removeObjectAtIndex:0]; removed = YES; }
    pthread_mutex_unlock(&_qmu);
    if (!removed) RDNA4LOG("completion for a command buffer that is no longer this queue's head");
    rdna4_schedule_kick();
}
- (BOOL)rdna4AdvanceHead {
    RDNA4CommandBuffer *head = nil;
    pthread_mutex_lock(&_qmu);
    if (_pending.count) head = _pending.firstObject;
    pthread_mutex_unlock(&_qmu);
    return head ? [head rdna4AdvanceWithQueue:self] : NO;
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
    if (n) rdna4_schedule_kick();
}
// Base version runs right after submitCommandBuffers:count: (F4) and would complete before the GPU is done.
- (void)completeCommandBuffers:(id __unsafe_unretained *)cbs count:(NSUInteger)n {
    (void)cbs; RDNA4LOG("queue completeCommandBuffers:count:%lu (deferred to the completion thread)", (unsigned long)n);
}
@end


// 11h.6 IOSurface coherency. Path (a) stays in GENERAL after its first submitted transition. Path (b) uploads at first use and copies
// back after writes. Uploads run outside render passes, and both paths make writes visible to the host before completing.
// Both run at GPU execution time, so CPU writes made before commit are seen and the CPU sees the result after the completion handler / waitUntilCompleted.
static void rdna4_ios_linear_init(VkCommandBuffer cmd, RDNA4Texture *texture, RDNA4Buffer *saved, NSUInteger bytes) {
    rdna4_full_barrier(cmd);
    VkBufferCopy copy = { .srcOffset = [texture rdna4IOSOffset], .dstOffset = [saved rdna4VkOffset], .size = bytes };
    vkCmdCopyBuffer(cmd, [texture rdna4IOSBuffer], [saved vkBuffer], 1, &copy);
    rdna4_full_barrier(cmd);
    rdna4_image_transition(cmd, texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .bufferOffset = [saved rdna4VkOffset],
        .bufferRowLength = (uint32_t)([texture rdna4IOSBytesPerRow] / [texture bytesPerPixel]),
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)[texture width], (uint32_t)[texture height], 1 } };
    vkCmdCopyBufferToImage(cmd, [saved vkBuffer], [texture vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    rdna4_full_barrier(cmd);
    VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
}
static void rdna4_ios_upload(RDNA4CommandBuffer *cb, RDNA4Texture *t) {
    VkCommandBuffer cmd = [cb vk];
    rdna4_full_barrier(cmd);
    if ([t rdna4IOSLinear]) return;
    rdna4_image_transition(cmd, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = { .bufferOffset = [t rdna4IOSOffset], .bufferRowLength = (uint32_t)([t rdna4IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
    vkCmdCopyBufferToImage(cmd, [t rdna4IOSBuffer], [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    rdna4_full_barrier(cmd);
}
static BOOL rdna4_ios_writeback(VkCommandBuffer cmd, RDNA4Texture *t) {
    if (rdna4nc_drop_write(t, RDNA4NC_WRITEBACK, "IOSurface write-back", "BlitCommandEncoder")) return NO;
    if (![t rdna4IsIOS]) return YES;   // P6: a base-less display texture is in _iow but has no host copy
    if (![t rdna4IOSLinear]) {
        rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [t rdna4IOSOffset], .bufferRowLength = (uint32_t)([t rdna4IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
        vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [t rdna4IOSBuffer], 1, &bic);
    }
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return YES;
}
static void rdna4_host_visibility_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// P6: the D-copy of a display texture that has only a VkImage (protected IOSurface, no CPU mapping): image -> scanout slot buffer. Layout: the image is moved to TRANSFER_SRC_OPTIMAL with rdna4_tex_to
// (a full-memory barrier from its current tracked layout, which is what the last render pass / blit left it in; [t layout] is updated, so a later pass starts from TRANSFER_SRC like after rdna4_ios_writeback);
// the caller brackets this with rdna4_full_barrier (writes before, slot writes visible after). Same plan as rdna4s_record_copy: full = one region of the whole image; D2 partial = the base slot first
// (buffer -> buffer), a WAW barrier, then the rect from the image at the same byte offsets (rdna4df_img_region). The image is the plane's size and 4 bytes per texel (checked before the slot is taken).
static void rdna4s_record_copy_img(VkCommandBuffer cmd, RDNA4Texture *t, const rdna4df_plan_t *pl) {
    int slot = pl->slot; rdna4df_imgreg_t g;
    const rdna4x_desc_t *const xd = rdna4x_desc((uint32_t)pl->inst);   // bundle 15: the instance's geometry for an HDMI plan
    const uint32_t gw = xd ? xd->w : RDNA4S.pw, gh = xd ? xd->h : RDNA4S.ph, gp = xd ? xd->pitch_bytes : RDNA4S.pitch;
    VkDeviceSize all = (VkDeviceSize)gp * gh;
    rdna4df_rect_t full = { 0, 0, (int32_t)gw, (int32_t)gh };
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (pl->base >= 0) {
        VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, RDNA4S.buf[pl->base], RDNA4S.buf[slot], 1, &bc);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // WAW: the region copy must land after the base copy
    }
    if (!rdna4df_img_region(pl->base >= 0 ? pl->rect : full, gw, gh, gp, 4, &g)) return;   // empty partial rectangle: only the base copy (same as the buffer path); full frames were validated by rdna4df_img_ok
    VkBufferImageCopy bic = { .bufferOffset = g.buf_off, .bufferRowLength = g.row_len, .bufferImageHeight = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)g.x, (int32_t)g.y, 0 }, .imageExtent = { g.w, g.h, 1 } };
    vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, xd ? RDNA4XI(pl->inst).buf[slot] : RDNA4S.buf[slot], 1, &bic);   // the planned instance's slot buffer
    atomic_fetch_add(&rdna4s_imgcopies, 1);
}
// Build 16 (P1): the touch of an IOSurface texture `t` by command buffer `cb`: retains it, takes the F3 use count, and does what the aliasing rules (garnet_ioalias.h) demand before the caller's own commands:
//   * a PATH (b) writer that another wrapper of the same surface range touches is copied back NOW (and leaves the end-of-command-buffer write-back list), so the pages are current;
//   * a path (b) toucher whose image is stale (another wrapper wrote since) uploads again, even if it was touched before in this command buffer.
// endPass (may be NULL) is called, once, before the first recorded command: a render encoder ends its Vulkan render pass there (a copy cannot be recorded inside one).
// WindowServer and the kill file /private/tmp/rdna4m-noioalias keep build 14's rule exactly (upload at the first touch that needs the contents) and only COUNT the cases the rules would have acted on.
// Returns YES when commands were recorded. The caller guarantees [t rdna4IsIOS].
GARNET_INTERNAL BOOL rdna4_ios_touch(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL need, BOOL write, void (^endPass)(void)) {
    const BOOL first = [cb rdna4IOFirstTouch:t];
    t = [t rootResource];   // Views share one image: a second first-use upload would overwrite earlier GPU writes.
    const rdna4ia_act d = [cb rdna4IOAlias:t need:need write:write];
    BOOL did = NO;
    if (!rdna4_ioalias_acts()) {
        if (d.flush) atomic_fetch_add(&RDNA4LED.iaWsFlush, 1);
        if (d.reupload) atomic_fetch_add(&RDNA4LED.iaWsReup, 1);
        if (first && need) { if (endPass) endPass(); rdna4_ios_upload(cb, t); did = YES; }
        return did;
    }
    if (d.flush) {
        RDNA4Texture *x = [cb rdna4IOTex:d.flush];
        if (x && [x rdna4IsIOS]) {
            if (endPass) endPass();
            if (!rdna4_ios_writeback([cb vk], x)) {
                [cb rdna4CBTraceSurface:x use:"writeback-refused"];
                [cb rdna4Fail:@"IOSurface alias write-back destination is read-only"];
                return NO;
            }
            [cb rdna4NoteHostWriteback:x];
            rdna4_full_barrier([cb vk]);       // ... and the transfer write must be visible to whatever the GPU does next with those pages
            [cb rdna4IOUnwrite:x];             // the end-of-command-buffer write-back must not copy the (now older) image over a later writer
            atomic_fetch_add(&RDNA4LED.iaFlush, 1); did = YES;
        }
    }
    if (d.upload) {
        if (endPass) endPass();
        rdna4_ios_upload(cb, t);
        if (d.reupload) atomic_fetch_add(&RDNA4LED.iaReup, 1);
        did = YES;
    }
    return did;
}
// Call OUTSIDE a render pass. needContents NO = the caller clears the whole texture, so the upload is skipped.
GARNET_INTERNAL void rdna4_ios_usek(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write, uint32_t kind) {
    if (![t rdna4IsIOS]) {
        if ([t rdna4DispImgOnly]) {   // P6: a protected display surface has no host copy to upload or write back; it only needs to be retained and recorded as written so the D-copy reads its image
            (void)[cb rdna4IOFirstTouch:t];
            if (needContents) [cb rdna4CBRead:t];
            if (write) { [cb rdna4IOWritten:t]; if (kind) [cb rdna4DispNote:t bits:kind pso:nil]; }
        }
        return;
    }
    (void)rdna4_ios_touch(cb, t, needContents, write, NULL);   // build 16 (P1): the upload / flush decision; was: first touch && needContents -> upload
    if (needContents) [cb rdna4CBRead:t];
    if (write) { [cb rdna4IOWritten:t]; if (kind) [cb rdna4DispNote:t bits:kind pso:nil]; }
}
static void rdna4_ios_use(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write) { rdna4_ios_usek(cb, t, needContents, write, 0); }

_Static_assert((int)VK_TIMEOUT == RDNA4FW_TIMEOUT, "VK_TIMEOUT == RDNA4FW_TIMEOUT");


@class GarnetASHandle;


static dispatch_queue_t rdna4_fence_wait_queue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INTERACTIVE, 0);
        queue = dispatch_queue_create("rdna4.fence-wait", attr);
    });
    return queue;
}

GARNET_INTERNAL int rdna4_fence_wait_slice(void *ctx, uint64_t timeoutNs) {
    rdna4_fence_wait_ctx *w = (rdna4_fence_wait_ctx *)ctx;
    return (int)vkWaitForFences(RDNA4R.dev, 1, &w->fence, VK_TRUE, timeoutNs);
}

GARNET_INTERNAL void rdna4_fence_wait_pending(void *ctx, int slices) {
    const rdna4_fence_wait_ctx *w = (const rdna4_fence_wait_ctx *)ctx;
    const uint64_t now = rdna4_now();
    const uint64_t elapsedMs = now > w->t1 ? (now - w->t1) / 1000000ULL : 0ULL;
    RDNA4LOG("FENCE PENDING: command buffer %llu has not completed after %llu ms (%d slice(s))",
             (unsigned long long)w->cbid, (unsigned long long)elapsedMs, slices);
}

typedef struct {
    BOOL earlySchedule;
    BOOL noWsCompletion;
    BOOL wsCompletionAll;
} RDNA4CBKillFlags;

static RDNA4CBKillFlags rdna4_cb_kill_flags(void) {
    static RDNA4CBKillFlags flags;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        flags.earlySchedule = access("/private/tmp/garnet-early-schedule", F_OK) == 0;
        flags.noWsCompletion = access("/private/tmp/garnet-no-ws-completion", F_OK) == 0;
        flags.wsCompletionAll = access("/private/tmp/garnet-ws-completion-all", F_OK) == 0;
    });
    return flags;
}

@implementation RDNA4CommandBuffer
- (BOOL)rdna4IOFirstTouch:(RDNA4Texture *)t {
    RDNA4Texture *backing = [t rootResource];
    if (backing != t) [self rdna4Retain:t];
    t = backing;
    BOOL first = YES;
    if (_curSegment) {
        if (!_curSegment->touches) _curSegment->touches = [NSMutableArray array];
        first = [_curSegment->touches indexOfObjectIdenticalTo:t] == NSNotFound;
        if (first) [_curSegment->touches addObject:t];
    }
    if (!_iot) { _iot = [NSMutableArray array]; _iow = [NSMutableArray array]; }
    if ([_iot indexOfObjectIdenticalTo:t] == NSNotFound) {
        [_iot addObject:t]; [self rdna4Retain:t];
        rdna4_led_latch();
        if ([t rdna4UCSurface]) {   // F3: from the first touch until the command buffer is finished with the GPU the surface is use-counted (rdna4UCRelease); a buffer-backed texture has no surface and no ledger entry
            (void)rdna4uc_take(&_uc, RDNA4LED.ucMode, (void *)[t rdna4UCSurface], rdna4_uc_inc, NULL);
            rdna4_led_touch(t, 1); _ucLive = YES;
        }
    }
    return first;
}
- (rdna4ia_act)rdna4IOAlias:(RDNA4Texture *)t need:(BOOL)need write:(BOOL)w {
    t = [t rootResource];
    uint32_t sid = 0; uint64_t off = 0; [t rdna4AliasKey:&sid off:&off];
    return rdna4ia_touch(&_ia, sid, off, (uintptr_t)(__bridge void *)t, [t rdna4IOSLinear] ? 0 : 1, need ? 1 : 0, w ? 1 : 0);
}
- (RDNA4Texture *)rdna4IOTex:(uintptr_t)key { for (RDNA4Texture *x in _iot) if ((uintptr_t)(__bridge void *)x == key) return x; return nil; }
- (void)rdna4IOUnwrite:(RDNA4Texture *)t {
    [_iow removeObjectIdenticalTo:t];
    [_eventDirty removeObjectIdenticalTo:t];
}   // P1: flushed (copied back) mid-command-buffer: a later write-back must not copy the older image over a newer writer
// F3: release each use count once after completion, device loss, or no submission.
// Unresolved submissions remain retained and cannot reach dealloc.
- (void)rdna4UCRelease {
    if (!_ucLive) return;
    _ucLive = NO;
    rdna4uc_release(&_uc, rdna4_uc_dec, NULL);
    for (RDNA4Texture *t in _iot) if ([t rdna4UCSurface]) rdna4_led_touch(t, -1);
}
- (uint64_t)rdna4TBegin { return _tBeg; }
- (BOOL)rdna4IsFrame { return _frame; }
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use {
    if (!rdna4cbl_on()) return;
    texture = [texture rootResource];
    const BOOL readOnly = [texture rdna4HostReadOnly], display = [texture rdna4IsDisp];
    if ((!readOnly && !display) || [texture pixelFormat] != MTLPixelFormatBGRA8Unorm) return;
    IOSurfaceRef surface = [texture rdna4UCSurface];
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cbl_line_locked("T t=%llu cb=%llu tex=%p base=%p sid=%u disp=%d ro=%d size=%lux%lu pitch=%zu use=%s",
        (unsigned long long)rdna4_now(), (unsigned long long)_cbid, (__bridge void *)texture, (const void *)[texture rdna4IBase],
        surface ? IOSurfaceGetID(surface) : 0, display, readOnly, (unsigned long)texture.width, (unsigned long)texture.height,
        [texture rdna4IOSBytesPerRow], use);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4IOWritten:(RDNA4Texture *)t {
    t = [t rootResource];
    [self rdna4CBTraceSurface:t use:"write"];
    // IOSurfaces (both path (a) linear and path (b) copy-back) are imported
    // outside IOAccelerator fence tracking. Do not publish the command buffer as
    // scheduled until its GPU fence and host-visibility barrier complete; a
    // WindowServer, CA Whippet, or CPU capture consumer may read the surface as
    // soon as scheduled is visible. The /private/tmp/garnet-early-schedule kill
    // file restores early scheduling.
    if ([t rdna4IsIOS] &&
        !rdna4_cb_kill_flags().earlySchedule && !_scheduleAfterPublish) {
        _scheduleAfterPublish = YES;
        RDNA4_ONCE("scheduled publication deferred until IOSurface write completes");
    }
    // rdna4Finish selects the last display write, including a revisit through a view.
    [_iow removeObjectIdenticalTo:t];
    [_iow addObject:t];
    if (!_eventDirty) _eventDirty = [NSMutableArray array];
    if ([_eventDirty indexOfObjectIdenticalTo:t] == NSNotFound) [_eventDirty addObject:t];
    if ([t rdna4IsIOS] && [t rdna4IOSRef]) _ncbw = rdna4cb_addsid(_cbw, _ncbw, RDNA4CB_MAXW, IOSurfaceGetID([t rdna4IOSRef]));
    else if ([t rdna4DispImgOnly]) _ncbw = rdna4cb_addsid(_cbw, _ncbw, RDNA4CB_MAXW, [t rdna4DispSid]);   // P6: the transaction correlation also sees protected display surfaces
}
- (void)rdna4CBRead:(RDNA4Texture *)t {
    [self rdna4CBTraceSurface:t use:"read"];
    IOSurfaceRef surface = [t rdna4IOSRef];
    if (!surface) {
        if (![t rdna4DispImgOnly]) return;
        _ncbr = rdna4cb_addsid(_cbr, _ncbr, RDNA4CB_MAXR, [t rdna4DispSid]);
        RDNA4Texture *root = [t rootResource];
        if (!_curSegment || !(rdna4_is_ws() || rdna4s_testmode()) || [_curSegment->displayImageOverwrites containsObject:root]) return;
        if (!_curSegment->displayImageReads)
            _curSegment->displayImageReads = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageReads addObject:root];
        return;
    }
    const uint32_t sid = IOSurfaceGetID(surface);
    _ncbr = rdna4cb_addsid(_cbr, _ncbr, RDNA4CB_MAXR, sid);
    if (!_curSegment || !(rdna4_is_ws() || rdna4s_testmode()) ||
        _curSegment->displaySourceOverwrites[@(sid)] == [t rootResource]) return;
    if (!_curSegment->displaySourceReads) _curSegment->displaySourceReads = [NSMutableIndexSet indexSet];
    [_curSegment->displaySourceReads addIndex:sid];
}
// Stage 0b. -commit is stamped, logged, then handed to Metal's own implementation unchanged.
- (void)commit {
    [self rdna4AttachQueueResidencySnapshots];
    _tcommit = rdna4_now();
    [self rdna4DisplayCommit];
    if (rdna4cbl_on()) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        char w[160], r[256]; rdna4cbl_sids(w, sizeof w, _cbw, _ncbw); rdna4cbl_sids(r, sizeof r, _cbr, _ncbr);
        pthread_mutex_lock(&RDNA4CB.mu);
        rdna4cb_commit(&RDNA4CB.trk, _cbid, (uint64_t)(uintptr_t)(__bridge void *)_nq, _tcommit, _cbw, _ncbw, _cbr, _ncbr);
        rdna4cbl_line_locked("C t=%llu cb=%llu q=%#llx tid=%llu w=%s r=%s", (unsigned long long)_tcommit, (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, (unsigned long long)tid, w, r);
        pthread_mutex_unlock(&RDNA4CB.mu);
    }
    [super commit];
}
- (void)commitAndHold { [self commit]; }
- (void)rdna4DisplayCommit {
    pthread_mutex_lock(&RDNA4S.mu);
    for (int i = 0; i < _ndw; i++) {
        if (!_shaderFallback && !(_dw[i].mask & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP))) continue;
        const uint32_t sid = [_dw[i].t rdna4DispSid];
        if (!sid || _displayCommits[i].sid) continue;
        rdna4dc_commit_add(&RDNA4S.commits, &_displayCommits[i], sid, _tcommit);
        rdna4dc_commit_invalidate(&RDNA4S.sm, sid, _tcommit);
        for (int inst = 1; inst <= 2; inst++) rdna4dc_commit_invalidate(&RDNA4XI(inst).sm, sid, _tcommit);
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
- (void)rdna4DisplayUncommit {
    pthread_mutex_lock(&RDNA4S.mu);
    for (int i = 0; i < 4; i++) rdna4dc_commit_remove(&RDNA4S.commits, &_displayCommits[i]);
    pthread_mutex_unlock(&RDNA4S.mu);
}
// WindowServer sends this private selector instead of -commit: its display swap
// follows immediately, so "scheduled" is not enough to make the composited
// surface safe for scanout. Wait for the terminal fence in WindowServer; apps
// keep Metal's normal submit-only contract. The kill file preserves the old
// behaviour for A/B diagnosis without another source change.
- (BOOL)commitAndWaitUntilSubmitted {
    const BOOL ws = rdna4_is_ws();
    const RDNA4CBKillFlags kf = ws ? rdna4_cb_kill_flags() : (RDNA4CBKillFlags){0};
    [self commit];
    if (ws && !kf.noWsCompletion &&
        ([self rdna4IsFrame] || kf.wsCompletionAll)) {
        RDNA4_ONCE("WindowServer commitAndWaitUntilSubmitted: waiting for GPU completion on display frames");
        // A display frame's completion wait is on WindowServer's frame path: when it
        // takes long, the picture on the monitor stops changing for exactly that long.
        // Log it (and how long) so a frozen screen is attributed to this wait instead
        // of being guessed at from missing presents.
        const uint64_t tw = rdna4_now();
        [self waitUntilCompleted];
        const uint64_t waited = rdna4_now() - tw;
        if (waited > 500000000ull) RDNA4LOG("FRAME WAIT STALL: WindowServer waited %.1f ms for command buffer %llu (%lu segment(s), status %lu): the display shows the previous frame until it returns",
                                            (double)waited / 1e6, (unsigned long long)_cbid, (unsigned long)_segments.count,
                                            (unsigned long)[self status]);
    } else {
        if (ws && kf.noWsCompletion)
            RDNA4_ONCE("WindowServer commitAndWaitUntilSubmitted: completion wait disabled");
        [self waitUntilScheduled];
    }
    return [self status] != MTLCommandBufferStatusError;
}
- (void)rdna4CBSubmit:(uint64_t)tq {
    if (!rdna4cbl_on()) return;
    rdna4cb_inv_t inv[RDNA4CB_MAXINV]; uint64_t sq = 0, tid = 0; pthread_threadid_np(NULL, &tid);
    char w[160], r[256]; rdna4cbl_sids(w, sizeof w, _cbw, _ncbw); rdna4cbl_sids(r, sizeof r, _cbr, _ncbr);
    uint64_t t = rdna4_now(), q = (uint64_t)(uintptr_t)(__bridge void *)_nq;
    pthread_mutex_lock(&RDNA4CB.mu);
    uint32_t n = rdna4cb_submit(&RDNA4CB.trk, _cbid, q, t, _cbw, _ncbw, _cbr, _ncbr, inv, RDNA4CB_MAXINV, &sq);
    rdna4cba_submit(&RDNA4CB.activity);
    rdna4cbl_line_locked("S t=%llu tq=%llu cb=%llu q=%#llx tid=%llu sub=%llu dseq=%llu cmt=%llu cls=%s disp=%u slot=%d w=%s r=%s inv=%u", (unsigned long long)t, (unsigned long long)tq, (unsigned long long)_cbid, (unsigned long long)q,
        (unsigned long long)tid, (unsigned long long)sq, (unsigned long long)_dseq, (unsigned long long)_tcommit, _cbcls ? rdna4df_cls_name[_cbcls - 1] : "-", _cbdsid, _dslot, w, r, n);
    for (uint32_t i = 0; i < n && i < RDNA4CB_MAXINV && RDNA4CB.inv_logged < RDNA4_CBLOG_INV_LOGGED; i++) {
        RDNA4CB.inv_logged++;
        rdna4cbl_line_locked("I t=%llu rd_cb=%llu rd_q=%#llx rd_sub=%llu sid=%u wr_cb=%llu wr_q=%#llx wr_commit=%llu", (unsigned long long)t, (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue,
            (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue, (unsigned long long)inv[i].writer_commit);
        RDNA4LOG("cblog: INVERSION #%llu: cb %llu (queue %#llx, submit #%llu) reads IOSurface %u whose writer cb %llu (queue %#llx, committed earlier) is not yet submitted", (unsigned long long)RDNA4CB.inv_logged,
            (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue, (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue);
    }
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4CBDropWithError:(NSError *)error fault:(VkResult)fault {
    if (!rdna4cbl_on()) return;
    const char *phase = error && error == _encErr ? "encode" : "pre-submit";
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cb_drop(&RDNA4CB.trk, _cbid);
    rdna4cba_result(&RDNA4CB.activity, 0);
    rdna4cbl_line_locked("D t=%llu cb=%llu phase=%s code=%ld vk=%d", (unsigned long long)rdna4_now(),
        (unsigned long long)_cbid, phase, (long)error.code, (int)fault);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4CBFence:(int)vkr {
    if (!rdna4cbl_on()) return;
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cba_result(&RDNA4CB.activity, vkr == VK_SUCCESS);
    rdna4cbl_line_locked("F t=%llu cb=%llu q=%#llx vk=%d", (unsigned long long)rdna4_now(), (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, vkr);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
// native #12 P1: records HOW a display surface is written by this command buffer (OR-ed bits per surface; the pipeline names of every draw, deduplicated).
- (void)rdna4NoteShaderFallback { _shaderFallback = YES; }
- (void)rdna4DispNote:(RDNA4Texture *)t bits:(uint32_t)bits pso:(RDNA4RenderPipelineState *)p {
    t = [t rootResource];
    if (![t rdna4IsDisp]) return;
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) { if (_ndw >= 4) return; k = _ndw++; _dw[k].t = t; _dw[k].mask = 0; _dw[k].lastpso = NULL; _dw[k].fns[0] = 0; memset(&_dw[k].wr, 0, sizeof _dw[k].wr); }
    _dw[k].mask |= bits;
    if (bits & RDNA4DF_W_CLEAR) rdna4df_dmg_set_full(&_dw[k].wr.all);   // a clear load wrote the whole surface (for D2 it counts only when a presentable draw follows in the same pass: see rdna4DispDraw)
    if (bits & (RDNA4DF_W_BLIT | RDNA4DF_W_COMPUTE)) { rdna4df_dmg_set_full(&_dw[k].wr.all); rdna4df_dmg_set_full(&_dw[k].wr.pres); }   // unknown extent: the whole surface
    if (p && _dw[k].lastpso != (__bridge const void *)p) {
        _dw[k].lastpso = (__bridge const void *)p;
        if (RDNA4DO.on && atomic_load(&rdna4_dw_logged) >= 20) return;   // P5b: fns is only ever printed by the first 20 "scanout: display write" lines; nothing else reads it
        char one[140]; snprintf(one, sizeof one, "%s/%s ", [p rdna4VName], [p rdna4FName]);
        if (!strstr(_dw[k].fns, one) && strlen(_dw[k].fns) + strlen(one) < sizeof _dw[k].fns) strlcat(_dw[k].fns, one, sizeof _dw[k].fns);
    }
}
// native #12 D1: one draw on display surface t. `all` collects every draw (statistics); `pres` only the presentable ones (GPUPass, SkyLight composite; never ColorFill) plus, for the first such draw
// of a pass whose load action was Clear, the whole surface. The first presentable draw's first sampled IOSurface texture is the "source" (what GPUPass reads).
- (void)rdna4DispDraw:(RDNA4Texture *)t bits:(uint32_t)bits rect:(rdna4df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(RDNA4Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc {
    t = [t rootResource]; src = [src rootResource];
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) return;
    rdna4df_wr_t *wr = &_dw[k].wr;
    rdna4df_dmg_add_draw(&wr->all, r, w, h);
    if (!wr->vp_set) { wr->vp_set = 1; wr->vp[0] = vp.originX; wr->vp[1] = vp.originY; wr->vp[2] = vp.width; wr->vp[3] = vp.height; wr->sc[0] = (int64_t)sc.x; wr->sc[1] = (int64_t)sc.y; wr->sc[2] = (int64_t)sc.width; wr->sc[3] = (int64_t)sc.height; }
    if (bits & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP)) {
        rdna4df_dmg_add_draw(&wr->pres, r, w, h);
        if (clr) rdna4df_dmg_set_full(&wr->pres);
        if (!wr->src_kind && src && [src rdna4IOSRef]) {
            uint32_t a = IOSurfaceGetID([src rdna4IOSRef]), b = [t rdna4IOSRef] ? IOSurfaceGetID([t rdna4IOSRef]) : 0;
            wr->src_sid = a; wr->src_w = (uint32_t)src.width; wr->src_h = (uint32_t)src.height;
            wr->src_kind = a == b ? 3 : [src rdna4IsDisp] ? 2 : 1;
        }
    }
}
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r {
    self = [super initWithQueue:q retainedReferences:r];
    if (!self) return nil;
    pthread_mutex_init(&_smu, NULL);
    _cbid = atomic_fetch_add(&rdna4cb_ids, 1) + 1; rdna4cbl_start();
    _dslot = -1; _nq = q; _refs = [NSMutableArray array];
    _residencySnapshots = [NSMutableArray array];
    _residencyHeapResources = [NSMutableSet set];
    _segments = [NSMutableArray array];
    NSError *segmentError = nil;
    if (![self rdna4OpenSegment:&segmentError]) {
        RDNA4LOG("command buffer segment allocation failed: %s", segmentError.localizedDescription.UTF8String);
        return nil;
    }
    _tBeg = rdna4_now();
    _pser = rdna4_fence_open();   // P1: fence-gated pool/backing reuse; unconditional for heap retirement when the pool is OFF
    if (!_pser) {
        RDNA4LOG("command buffer refused: GPU retirement timeline is unavailable (allocation failure)");
        return nil;
    }
    RDNA4LOGR("RDNA4CommandBuffer %p: vk %p begun", (__bridge void *)self, (void *)_vk);
    return self;
}
- (void)dealloc {
    [self rdna4DisplayUncommit];
    [self rdna4ClearHostActions];
    if (!_submitAttempted) [self rdna4CancelUnsubmittedArgumentIntents];
    [self rdna4FailPendingFenceUpdates:VK_ERROR_UNKNOWN];
    [self rdna4UCRelease]; rdna4uc_free(&_uc); rdna4ia_destroy(&_ia);   // build 16
    free(_native.ops);
    for (uint32_t j = 0; j < _noj; j++) free(_oj[j].o); free(_oj);   // build 18 (P2): bookkeeping of passes that never resolved
    if (RDNA4R.ok) {
        if (_pser && !_fence) rdna4_fence_close(_pser);   // P1: never submitted (a submitted one is closed by its completion block; a hung one stays open)
        for (RDNA4CommandSegment *segment in _segments)
            for (NSValue *v in segment->descriptorPools) {
                VkDescriptorPool dp_ = (VkDescriptorPool)v.pointerValue;
                if (!(RDNA4P.on && rdna4_dp_give(dp_)))
                    vkDestroyDescriptorPool(RDNA4R.dev, dp_, NULL);
            }
        for (NSValue *v in _qpools) vkDestroyQueryPool(RDNA4R.dev, (VkQueryPool)v.pointerValue, NULL);   // build 18 (P2)
        if (_dslot >= 0) { int s = _dslot; _dslot = -1; rdna4s_complete(_dinst, s, VK_ERROR_UNKNOWN, 0, 0, NULL, 0, 0); }   // never presented
        for (RDNA4CommandSegment *segment in _segments) {
            if (!segment->fence) continue;
            if ((!segment->submitted || segment->done) && !segment->retirementUnknown)
                rdna4_fence_give(segment->fence);
            else
                vkDestroyFence(RDNA4R.dev, segment->fence, NULL);
            segment->fence = VK_NULL_HANDLE;
        }
        if (_segments.count && _nq) {
            VkCommandBuffer *commands = calloc(_segments.count, 2 * sizeof *commands);
            if (commands) {
                uint32_t count = 0;
                for (RDNA4CommandSegment *segment in _segments) {
                    const BOOL reusable = (!segment->submitted || segment->done) && !segment->retirementUnknown;
                    if (segment->vk) {
                        if (reusable && rdna4_cmd_pool_active()) [_nq rdna4GiveCmdBuf:segment->vk];
                        else commands[count++] = segment->vk;
                    }
                    if (segment->linearInit) {
                        if (reusable && rdna4_cmd_pool_active()) [_nq rdna4GiveCmdBuf:segment->linearInit];
                        else commands[count++] = segment->linearInit;
                    }
                }
                if (count) {
                    rdna4_qlock();
                    vkFreeCommandBuffers(RDNA4R.dev, [_nq pool], count, commands);
                    pthread_mutex_unlock(&RDNA4R.qlock);
                }
                free(commands);
            }
        }
    }
    pthread_mutex_destroy(&_smu);
}
RDNA4_DNR(RDNA4CommandBuffer)
- (BOOL)rdna4OpenSegment:(NSError **)err {
    VkCommandBuffer vk = VK_NULL_HANDLE;
    if (_nq) {
        vk = [_nq rdna4TakeCmdBuf:err];
    } else {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = VK_NULL_HANDLE, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        rdna4_qlock();
        VkResult ar = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &vk);
        pthread_mutex_unlock(&RDNA4R.qlock);
        if (ar != VK_SUCCESS) {
            if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkAllocateCommandBuffers(segment) = %d", ar]);
            return NO;
        }
    }
    if (!vk) return NO;
    RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
    segment->vk = vk;
    [_segments addObject:segment];
    _curSegment = segment; _vk = vk;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult r = vkBeginCommandBuffer(vk, &bi);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkBeginCommandBuffer(segment) = %d", r]);
        return NO;
    }
    /*
     * A descriptor pool cannot be shared across command buffers while either
     * one can be in flight. The previous segment keeps its pools until its
     * fence; this segment starts with a fresh allocation pool.
     */
    _pool = VK_NULL_HANDLE;
    ++_segmentSerial;
    if (!_segmentSerial) ++_segmentSerial;
    return YES;
}

- (BOOL)rdna4EndCurrentSegment:(NSError **)err {
    if (!_curSegment || _curSegment->ended) return YES;
    BOOL signals = _curSegment->signals.count != 0;
    BOOL writes = _eventDirty.count != 0;
    [self rdna4FlushEventWrites:_curSegment->vk];
    if (_encErr) { if (err) *err = _encErr; return NO; }
    rdna4_full_barrier(_curSegment->vk);
    if (!rdna4_argument_summary_owners_encode(_curSegment->argumentIntents,
            _curSegment->vk, &_curSegment->argumentSummaryAttempts, err)) {
        if (!_encErr) _encErr = err && *err ? *err :
            rdna4_err(68, @"Private argument summary encoding failed");
        return NO;
    }
    if (_curSegment->argumentSummaryAttempts.count) _native.blocked = true;
    if (signals || writes) rdna4_host_visibility_barrier(_curSegment->vk);
    VkResult r = vkEndCommandBuffer(_curSegment->vk);
    _curSegment->ended = YES;
    if (r == VK_SUCCESS) {
        // A segment is a host-coherence boundary. Its alias state cannot outlive the
        // write-back above: the next segment starts from the current surface pages.
        rdna4ia_destroy(&_ia);
        rdna4ia_init(&_ia);
        return YES;
    }
    NSError *endError = rdna4_err(55, [NSString stringWithFormat:@"vkEndCommandBuffer(segment) = %d", r]);
    if (!_encErr) _encErr = endError;
    if (err) *err = endError;
    return NO;
}

- (BOOL)rdna4HasArgumentWrites {
    return _curSegment && !_curSegment->ended &&
        rdna4_argument_summary_owners_needed(_curSegment->argumentIntents);
}
- (void)rdna4ArgumentCheckpoint {
    if (_encErr || ![self rdna4HasArgumentWrites]) return;
    NSError *error = nil;
    // A segment boundary allows retirement before a later event-blocked writer.
    // Recording never submits or waits for a producer here.
    _hasEvents = YES;
    _native.blocked = true;
    if (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])
        [self rdna4Fail:error.localizedDescription ?: @"argument checkpoint failed"];
}

- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error {
    if (_ended || _prepared || !_curSegment || _curSegment->ended) {
        NSError *failure = rdna4_err(66, @"ordered host action encoded after command-buffer encoding ended");
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    if (!action) {
        NSError *failure = rdna4_err(66, @"ordered host action block is nil");
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    if (_encErr) {
        if (error) *error = _encErr;
        return NO;
    }

    // This is a scheduling barrier even when the preceding Vulkan segment is empty:
    // the action must not run through the whole-command-buffer native fast path.
    _hasEvents = YES;
    _native.blocked = true;
    NSError *segmentError = nil;
    if (![self rdna4EndCurrentSegment:&segmentError]) {
        if (error) *error = segmentError;
        [self rdna4Fail:(segmentError.localizedDescription ?: @"cannot end segment before ordered host action")];
        return NO;
    }

    RDNA4CommandSegment *host = nil;
    @try {
        host = [RDNA4CommandSegment new];
        host->hostOnly = YES;
        host->ended = YES;
        host->hasWork = YES;
        host->hostAction = [action copy];
    } @catch (NSException *exception) {
        NSError *failure = rdna4_err(66, [NSString stringWithFormat:@"ordered host action allocation failed: %@",
                                         exception.reason ?: @"out of memory"]);
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    @try {
        [_segments addObject:host];
    } @catch (NSException *exception) {
        NSError *failure = rdna4_err(66, [NSString stringWithFormat:@"ordered host action enqueue failed: %@",
                                         exception.reason ?: @"out of memory"]);
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }

    if (![self rdna4OpenSegment:&segmentError]) {
        if (error) *error = segmentError;
        [self rdna4Fail:(segmentError.localizedDescription ?: @"cannot open segment after ordered host action")];
        return NO;
    }
    return YES;
}

- (void)rdna4ClearHostActions {
    for (RDNA4CommandSegment *segment in _segments)
        if (segment->hostOnly) segment->hostAction = nil;
}

- (void)rdna4NoteHostWriteback:(RDNA4Texture *)texture {
    if (!_curSegment) return;
    if (![texture rdna4IsIOS]) {
        if (![texture rdna4DispImgOnly] || !(rdna4_is_ws() || rdna4s_testmode())) return;
        if (!_curSegment->displayImageWrites)
            _curSegment->displayImageWrites = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageWrites addObject:[texture rootResource]];
        return;
    }
    if ([texture rdna4IOSLinear]) _curSegment->hostLinearWrites++;
    else _curSegment->hostWritebackCopies++;
    IOSurfaceRef surface = [texture rdna4IOSRef];
    if (!surface || !([texture rdna4IsDisp] || rdna4_is_ws() || rdna4s_testmode())) return;
    if (!_curSegment->displaySourceWrites) _curSegment->displaySourceWrites = [NSMutableIndexSet indexSet];
    [_curSegment->displaySourceWrites addIndex:IOSurfaceGetID(surface)];
}

- (void)rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level {
    if (!_curSegment || level || !(rdna4_is_ws() || rdna4s_testmode()) || [texture rootResource] != texture) return;
    if ([texture rdna4DispImgOnly]) {
        if (width != texture.width || height != texture.height) return;
        if (!_curSegment->displayImageOverwrites)
            _curSegment->displayImageOverwrites = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageOverwrites addObject:texture];
        return;
    }
    IOSurfaceRef surface = [texture rdna4IOSRef];
    if (!surface || IOSurfaceGetPlaneCount(surface) > 1 || width != IOSurfaceGetWidth(surface) ||
        height != IOSurfaceGetHeight(surface) || width != texture.width || height != texture.height) return;
    if (!_curSegment->displaySourceOverwrites) _curSegment->displaySourceOverwrites = [NSMutableDictionary dictionary];
    _curSegment->displaySourceOverwrites[@(IOSurfaceGetID(surface))] = texture;
}

- (void)rdna4FlushEventWrites:(VkCommandBuffer)vk {
    if (!_eventDirty.count) return;
    // A CPU listener may touch the pages between segments, and a copy-path consumer
    // cannot wait for terminal completion. Make the pages current before the fence.
    RDNA4LOGR("segment boundary: host write-back for %lu IOSurface texture(s)", (unsigned long)_eventDirty.count);
    for (RDNA4Texture *t in _eventDirty) {
        if (!rdna4_ios_writeback(vk, t)) {
            [self rdna4CBTraceSurface:t use:"writeback-refused"];
            [self rdna4Fail:@"IOSurface write-back destination is read-only"];
            return;
        }
        [self rdna4NoteHostWriteback:t];
    }
    [_eventDirty removeAllObjects];
}

- (void)rdna4UpdateFence:(id)f {
    if (_ended || !_curSegment) { [self rdna4Fail:@"updateFence after the command buffer was finished"]; return; }
    if ([self rdna4Error]) return;
    if (![f isKindOfClass:[RDNA4Fence class]] || [(RDNA4Fence *)f device] != [self device]) {
        [self rdna4Fail:@"updateFence requires an RDNA4Fence from this device"];
        return;
    }
    RDNA4FenceCheckpoint *checkpoint = [(RDNA4Fence *)f rdna4UpdateCheckpointForCB:_cbid];
    if (!checkpoint) { [self rdna4Fail:@"updateFence generation overflow"]; return; }
    [self rdna4Retain:f];
    [_curSegment->fenceUpdates addObject:checkpoint];
    _curSegment->hasWork = YES;
    _hasEvents = YES;
    _native.blocked = true;
    rdna4_full_barrier(_vk);
}

- (void)rdna4WaitForFence:(id)f {
    if (_ended || !_curSegment) { [self rdna4Fail:@"waitForFence after the command buffer was finished"]; return; }
    if ([self rdna4Error]) return;
    if (![f isKindOfClass:[RDNA4Fence class]] || [(RDNA4Fence *)f device] != [self device]) {
        [self rdna4Fail:@"waitForFence requires an RDNA4Fence from this device"];
        return;
    }
    RDNA4FenceCheckpoint *checkpoint = [(RDNA4Fence *)f rdna4WaitCheckpoint];
    int state = atomic_load_explicit(&checkpoint->state, memory_order_acquire);
    if ((checkpoint->producerCBId && checkpoint->producerCBId == _cbid) || state == RDNA4FenceCheckpointReady) {
        // Intra-CB or already-satisfied fence dependency is ordered by an in-CB barrier; avoid splitting VkCommandBuffer segments.
        [self rdna4Retain:f];
        _curSegment->hasWork = YES;
        _hasEvents = YES;
        _native.blocked = true;
        rdna4_full_barrier(_vk);
        return;
    }
    BOOL empty = !_curSegment->hasWork && !_curSegment->waits.count && !_curSegment->signals.count &&
                 !_curSegment->fenceWaits.count && !_curSegment->fenceUpdates.count &&
                 !_curSegment->argumentIntents.count;
    NSError *error = nil;
    if (!empty && (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])) {
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at waitForFence"];
        return;
    }
    [_curSegment->fenceWaits addObject:checkpoint];
    [self rdna4Retain:f];
    _curSegment->hasWork = YES;
    _hasEvents = YES;
    _native.blocked = true;
    rdna4_full_barrier(_vk);
}

// MTLSharedEvent values live on the event. A plain MTLEvent has no public value API, so a weak token
// map holds the maximum value signaled by this backend. Each event call is a command-buffer segment
// boundary: the preceding segment carries signals, the following segment carries waits. A queue-local
// scheduler advances only that queue's head, while all logical queues share one ready-scan thread.
- (void)encodeSignalEvent:(id)e value:(uint64_t)v {
    if (_ended || !_curSegment) { [self rdna4Fail:@"encodeSignalEvent after the command buffer was finished"]; return; }
    if (!rdna4_event_is_shared(e) && ![e conformsToProtocol:@protocol(MTLEvent)]) {
        [self rdna4Fail:@"encodeSignalEvent: the object is not a Metal event"]; return;
    }
    _hasEvents = YES; _native.blocked = true;
    [_curSegment->signals addObject:@[e, @(v)]];
    [self rdna4Retain:e];
    NSError *error = nil;
    if (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at encodeSignalEvent"];
}

- (void)encodeWaitForEvent:(id)e value:(uint64_t)v { [self encodeWaitForEvent:e value:v timeout:60000]; }
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v timeout:(uint32_t)ms {
    if (_ended || !_curSegment) { [self rdna4Fail:@"encodeWaitForEvent after the command buffer was finished"]; return; }
    if (!rdna4_event_is_shared(e) && ![e conformsToProtocol:@protocol(MTLEvent)]) {
        [self rdna4Fail:@"encodeWaitForEvent: the object is not a Metal event"]; return;
    }
    _hasEvents = YES; _native.blocked = true;
    BOOL empty = !_curSegment->hasWork && !_curSegment->waits.count && !_curSegment->signals.count &&
                 !_curSegment->fenceWaits.count && !_curSegment->fenceUpdates.count &&
                 !_curSegment->argumentIntents.count;
    NSError *error = nil;
    if (!empty && (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])) {
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at encodeWaitForEvent"];
        return;
    }
    NSMutableArray *wait = [@[e, @(v), @(ms), @0] mutableCopy];
    [_curSegment->waits addObject:wait];
    [self rdna4Retain:e];
}

- (int)rdna4WaitsReady:(RDNA4CommandSegment *)segment error:(NSError **)err {
    BOOL fencePending = NO;
    for (RDNA4FenceCheckpoint *checkpoint in segment->fenceWaits) {
        int state = atomic_load_explicit(&checkpoint->state, memory_order_acquire);
        if (state == RDNA4FenceCheckpointReady) continue;
        if (state == RDNA4FenceCheckpointFailed) {
            int fault = atomic_load_explicit(&checkpoint->fault, memory_order_relaxed);
            if (err) *err = rdna4_err(59, [NSString stringWithFormat:
                @"waitForFence: producer update generation %llu failed (VkResult %d)",
                (unsigned long long)checkpoint->generation, fault]);
            return -1;
        }
        fencePending = YES;
    }
    for (NSMutableArray *w in segment->waits) {
        id event = w[0];
        uint64_t value = [w[1] unsignedLongLongValue];
        uint32_t timeoutMS = [w[2] unsignedIntValue];
        if (rdna4_event_value(event) >= value) continue;
        if (rdna4_event_failed(event, value)) {
            if (err) *err = rdna4_err(58, [NSString stringWithFormat:@"encodeWaitForEvent: producer for value %llu failed", (unsigned long long)value]);
            return -1;
        }
        uint64_t now = rdna4_now(), started = [w[3] unsignedLongLongValue];
        if (!started) { started = now; [w replaceObjectAtIndex:3 withObject:@(started)]; }
        if (now - started >= (uint64_t)timeoutMS * 1000000ULL) {
            if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"encodeWaitForEvent: value %llu not signaled within %u ms", (unsigned long long)value, timeoutMS]);
            return -1;
        }
        return 0;
    }
    if (fencePending) return 0;
    return 1;
}

- (void)rdna4SignalSegment:(RDNA4CommandSegment *)segment {
    [self rdna4SignalSegment:segment releaseRefs:YES];
}
- (void)rdna4SignalSegment:(RDNA4CommandSegment *)segment releaseRefs:(BOOL)releaseRefs {
    if ((segment->hostWritebackCopies || segment->hostLinearWrites) && rdna4cbl_on()) {
        pthread_mutex_lock(&RDNA4CB.mu);
        rdna4cbl_line_locked("W t=%llu cb=%llu dseq=%llu copies=%lu linear=%lu", (unsigned long long)rdna4_now(),
            (unsigned long long)_cbid, (unsigned long long)_dseq, (unsigned long)segment->hostWritebackCopies,
            (unsigned long)segment->hostLinearWrites);
        rdna4cba_writeback(&RDNA4CB.activity);
        pthread_mutex_unlock(&RDNA4CB.mu);
    }
    // Completion is published only after both acceptance and a successful completion path.
    for (id intent in segment->argumentIntents) rdna4_argument_sampler_complete(intent);
    [segment->argumentIntents removeAllObjects];
    for (id attempt in segment->argumentSummaryAttempts) {
        NSError *error = nil;
        if (!rdna4_argument_summary_owners_finish(attempt, YES,
                segment->argumentSubmissionEpoch, &error)) {
            if (!_encErr) _encErr = error ?: rdna4_err(68,
                @"Private argument summary publication failed");
            RDNA4LOG("argument summary retained prior owners: %s",
                error.localizedDescription.UTF8String ?: "publication failed");
        }
    }
    segment->argumentSummaryAttempts = nil;
    // These immutable descriptor snapshots are used only by this segment.
    [segment->argumentPools removeAllObjects];
    if (releaseRefs) {
        [segment->argumentRefs removeAllObjects];
        [segment->commandRefs removeAllObjects];
    }
    if (segment->vk || segment->linearInit) {
        if (rdna4_cmd_pool_active() && _nq) {
            if (segment->vk) [_nq rdna4GiveCmdBuf:segment->vk];
            if (segment->linearInit) [_nq rdna4GiveCmdBuf:segment->linearInit];
        } else {
            VkCommandBuffer commands[2];
            uint32_t count = 0;
            if (segment->vk) commands[count++] = segment->vk;
            if (segment->linearInit) commands[count++] = segment->linearInit;
            rdna4_qlock();
            vkFreeCommandBuffers(RDNA4R.dev, [_nq pool], count, commands);
            pthread_mutex_unlock(&RDNA4R.qlock);
        }
        if (_vk == segment->vk) _vk = VK_NULL_HANDLE;
        segment->vk = VK_NULL_HANDLE;
        segment->linearInit = VK_NULL_HANDLE;
    }
    [self rdna4ReleaseSegmentDescriptorPools:segment];
    for (RDNA4FenceCheckpoint *checkpoint in segment->fenceUpdates) [checkpoint rdna4Ready];
    for (NSArray *s in segment->signals) {
        id event = s[0];
        uint64_t value = [s[1] unsignedLongLongValue];
        rdna4_event_set_value(event, value);
        RDNA4LOG("event signaled to %llu at its encoded segment", (unsigned long long)value);
    }
}

- (void)rdna4ReleaseSegmentDescriptorPools:(RDNA4CommandSegment *)segment {
    if (!segment || !segment->descriptorPools.count) return;
    NSArray<NSValue *> *pools = [segment->descriptorPools copy];
    [segment->descriptorPools removeAllObjects];
    if (!RDNA4R.ok) return;
    for (NSValue *value in pools) {
        VkDescriptorPool pool = (VkDescriptorPool)value.pointerValue;
        if (_pool == pool) _pool = VK_NULL_HANDLE;
        if (!(RDNA4P.on && rdna4_dp_give(pool)))
            vkDestroyDescriptorPool(RDNA4R.dev, pool, NULL);
    }
}

- (void)rdna4FailPendingSignals {
    for (RDNA4CommandSegment *segment in _segments) {
        if (!segment->done && !segment->pruned) rdna4_event_fail_signals(segment->signals);
    }
}

- (void)rdna4FailPendingFenceUpdates:(VkResult)fault {
    if (fault == VK_SUCCESS) fault = VK_ERROR_UNKNOWN;
    for (RDNA4CommandSegment *segment in _segments)
        for (RDNA4FenceCheckpoint *checkpoint in segment->fenceUpdates)
            [checkpoint rdna4Fail:fault];
}

- (NSUInteger)protectionOptions { return _prot; }
- (void)setProtectionOptions:(NSUInteger)o { _prot = o; RDNA4_ONCE("cb setProtectionOptions: recorded only (no protected content)"); }
- (VkCommandBuffer)vk { _native.blocked = true; if (_curSegment) _curSegment->hasWork = YES; return _vk; }
- (VkCommandBuffer)rdna4BlitVK { if (_curSegment) _curSegment->hasWork = YES; return _vk; }
- (void)rdna4NativeRecord:(RDNA4NativeOp)op { rdna4_native_record(&_native, op); }
- (BOOL)rdna4NativeEligible {
    return !_native.blocked && _native.count && !_iot.count &&
        rdna4_vkQueueWaitIdle && rdna4_native_bridge.copy && rdna4_native_bridge.fill && rdna4_native_bridge.wait;
}
- (int)rdna4NativeExecute:(bool *)fallback {
    return rdna4_native_execute(&_native, rdna4_native_bridge, RDNA4R.dev, fallback);
}
- (id)device { return [_nq device]; }
- (VkFence)fence { return _fence; }
- (void)setFence:(VkFence)f { _fence = f; }
- (void)rdna4Retain:(id)o { if (o) [_refs addObject:o]; }
- (void)rdna4RetainArgument:(id)object {
    if (!object) return;
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument snapshot has no open command segment"];
        return;
    }
    [_curSegment->argumentRefs addObject:object];
}
- (void)rdna4RetainEncoded:(id)object {
    if (!object) return;
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"encoded resource snapshot has no open command segment"];
        return;
    }
    [_curSegment->commandRefs addObject:object];
}
- (uint64_t)rdna4SegmentSerial { return _segmentSerial; }
- (uint64_t)rdna4CBId { return _cbid; }
- (BOOL)rdna4HasResidency {
    return _residencySnapshots.count != 0 || _residencyHeapResources.count != 0 || [_nq rdna4HasResidencySets];
}
- (void)rdna4UseResource:(id)resource usage:(NSUInteger)usage {
    if (_encErr) return;
    Class icbCls = objc_getClass("RDNA4IndirectCommandBuffer");
    BOOL isICB = icbCls && [resource isKindOfClass:icbCls];
    Class asCls = objc_getClass("GarnetAccelerationStructure");
    BOOL isAS = asCls && [resource isKindOfClass:asCls];
    if ((!(isICB || isAS || [resource isKindOfClass:[RDNA4Buffer class]] || [resource isKindOfClass:[RDNA4Texture class]])) ||
        [(id<MTLResource>)resource device] != [self device] || !usage || (usage & ~(NSUInteger)7)) {
        [self rdna4Fail:@"useResource has an invalid resource, device or usage"];
        return;
    }
    [self rdna4RetainEncoded:resource];
    if (isICB) {
        id backing = [(id<RDNA4ICBInternal>)resource rdna4GPUBacking];
        if (backing) [self rdna4RetainEncoded:backing];
    } else if (isAS) {
        id handle = [(id<GarnetASInternal>)resource rdna4CurrentHandle];
        if (handle) [self rdna4RetainEncoded:handle];
    }
}
- (void)rdna4UseHeap:(id)heap {
    if (_encErr) return;
    Class spHeapCls = objc_getClass("RDNA4SparseHeap");
    BOOL isSparseHeap = spHeapCls && [heap isKindOfClass:spHeapCls];
    if ((!(isSparseHeap || [heap isKindOfClass:[RDNA4Heap class]])) || [heap device] != [self device]) {
        [self rdna4Fail:@"useHeap has an invalid heap or device"]; return;
    }
    [self rdna4RetainEncoded:heap];
    if (!isSparseHeap) {
        for (id resource in [heap rdna4LiveResources]) if (![resource isAliasable]) [self rdna4RetainEncoded:resource];
    }
}
- (void)rdna4AttachResidencySnapshot:(id)snapshot {
    if (!snapshot || ![snapshot respondsToSelector:@selector(rdna4Allocations)]) return;
    if (!_residencySnapshots) _residencySnapshots = [NSMutableArray array];
    if (!_residencyHeapResources) _residencyHeapResources = [NSMutableSet set];
    if ([_residencySnapshots indexOfObjectIdenticalTo:snapshot] == NSNotFound)
        [_residencySnapshots addObject:snapshot];
    for (id alloc in [(id<RDNA4ResidencySnapshotInternal>)snapshot rdna4Allocations]) {
        if ([alloc isKindOfClass:[RDNA4Heap class]]) {
            for (id resource in [(RDNA4Heap *)alloc rdna4LiveResources]) {
                if (![resource isAliasable]) [_residencyHeapResources addObject:resource];
            }
        }
    }
}
- (void)rdna4AttachQueueResidencySnapshots {
    if (!_nq) return;
    for (id snap in [_nq rdna4ResidencySetSnapshots])
        [self rdna4AttachResidencySnapshot:snap];
}
- (void)useResidencySet:(id)residencySet {
    if (_encErr || !residencySet) return;
    if (![residencySet respondsToSelector:@selector(rdna4CommittedSnapshot)] ||
        ![residencySet respondsToSelector:@selector(device)] ||
        [residencySet device] != [self device]) {
        [self rdna4Fail:@"useResidencySet has an invalid residency set or device"];
        return;
    }
    [self rdna4AttachResidencySnapshot:[(id<RDNA4ResidencySetInternal>)residencySet rdna4CommittedSnapshot]];
}
- (void)useResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (_encErr || !count) return;
    if (!residencySets) {
        [self rdna4Fail:@"useResidencySets:count: has a NULL array"];
        return;
    }
    for (NSUInteger i = 0; i < count; i++) [self useResidencySet:residencySets[i]];
}
- (NSSet *)rdna4ResidencyDeclaredResources {
    [self rdna4AttachQueueResidencySnapshots];
    if (!_residencySnapshots.count && !_residencyHeapResources.count) return [NSSet set];
    NSMutableSet *out = [NSMutableSet set];
    for (id<RDNA4ResidencySnapshotInternal> snap in _residencySnapshots) {
        for (id obj in [snap rdna4Allocations]) [out addObject:obj];
    }
    for (id obj in _residencyHeapResources) [out addObject:obj];
    return out;
}
- (void)rdna4Fail:(NSString *)why {
    RDNA4LOG("ENCODE ERROR (command buffer will complete with an error): %s", why.UTF8String);
    if (!_encErr) _encErr = rdna4_err(66, why);
}
- (NSError *)rdna4Error { return _encErr; }
- (void)rdna4DispDone:(VkResult)r {
    if (_dlate) {
        _dlate = NO;
        if (r == VK_SUCCESS) rdna4s_late_complete(_dtex, _dinst, _dseq, _dsid, _dcls, _tcommit);
        _dtex = nil;
    }
    [self rdna4DisplayUncommit];
    int s = _dslot; if (s < 0) return; _dslot = -1;
    rdna4crc_rec_t *rec = (_dcrc && r == VK_SUCCESS && _dtex && _dinst == 0) ? rdna4s_crc_check(_dtex, s, _dseq) : NULL;   // bundle 13: the CRC diagnostic is the DP's alone
    _dtex = nil;
    rdna4s_complete(_dinst, s, r, _dseq, _dsid, rec, _dcls, _tcommit);
}
- (void)rdna4DispSourceWrites:(RDNA4CommandSegment *)segment {
    NSIndexSet *sources = segment->displaySourceWrites;
    NSIndexSet *reads = segment->displaySourceReads;
    if (!sources.count && !reads.count && !segment->displayImageWrites.count && !segment->displayImageReads.count) return;
    const uint64_t seq = sources.count ? atomic_fetch_add(&RDNA4S.seq, 1) + 1 : 0;
    pthread_mutex_lock(&RDNA4S.mu);
    const uint64_t now = rdna4_now();
    // SkyLight ping-pongs composite surfaces (e.g. 1 <-> 3) without MTLLoadActionClear,
    // so unbounded taint permanently suppresses scanout after any single transient fallback.
    // Expire taint after a fallback-quiet window and cap continuous suppression.
    if (RDNA4S.incompleteLastFallbackNs && now >= RDNA4S.incompleteLastFallbackNs &&
        now - RDNA4S.incompleteLastFallbackNs >= RDNA4DF_INCOMPLETE_QUIET_NS) {
        if (RDNA4S.incompleteSources.count || RDNA4S.incompleteImages.count) {
            RDNA4LOG("scanout: clearing incomplete display taint (%lu surface(s), %lu image(s)) after %.0f ms without shader fallback",
                     (unsigned long)RDNA4S.incompleteSources.count,
                     (unsigned long)RDNA4S.incompleteImages.count,
                     (double)(now - RDNA4S.incompleteLastFallbackNs) / 1e6);
            [RDNA4S.incompleteSources removeAllIndexes];
            [RDNA4S.incompleteImages removeAllObjects];
        }
        RDNA4S.incompleteSinceNs = 0;
        RDNA4S.incompleteLastFallbackNs = 0;
        RDNA4S.incompleteCapLogged = NO;
    }
    BOOL suppressFallback = _shaderFallback;
    if (_shaderFallback) {
        if (!RDNA4S.incompleteSinceNs) RDNA4S.incompleteSinceNs = now;
        RDNA4S.incompleteLastFallbackNs = now;
    }
    if (RDNA4S.incompleteSinceNs && now >= RDNA4S.incompleteSinceNs &&
        now - RDNA4S.incompleteSinceNs >= RDNA4DF_INCOMPLETE_MAX_NS) {
        suppressFallback = NO;
        if (RDNA4S.incompleteSources.count || RDNA4S.incompleteImages.count || !RDNA4S.incompleteCapLogged) {
            if (!RDNA4S.incompleteCapLogged) {
                RDNA4S.incompleteCapLogged = YES;
                RDNA4LOG("scanout: shader-fallback suppression cap (%.0f ms) reached; resuming presentation",
                         (double)(now - RDNA4S.incompleteSinceNs) / 1e6);
            }
            [RDNA4S.incompleteSources removeAllIndexes];
            [RDNA4S.incompleteImages removeAllObjects];
        }
    }
    if (!_displayIncomplete && RDNA4S.incompleteSources.count) {
        for (NSUInteger sid = reads.firstIndex; reads && sid != NSNotFound; sid = [reads indexGreaterThanIndex:sid]) {
            if (![RDNA4S.incompleteSources containsIndex:sid]) continue;
            _displayIncomplete = YES;
            RDNA4LOGR("scanout: command buffer %llu reads incomplete IOSurface %lu", (unsigned long long)_cbid, (unsigned long)sid);
            break;
        }
    }
    if (!_displayIncomplete && segment->displayImageReads.count &&
        [RDNA4S.incompleteImages intersectsHashTable:segment->displayImageReads]) {
        _displayIncomplete = YES;
        RDNA4LOGR("scanout: command buffer %llu reads an incomplete GPU-only IOSurface image", (unsigned long long)_cbid);
    }
    const BOOL incomplete = suppressFallback || _displayIncomplete;
    for (RDNA4Texture *image in segment->displayImageWrites) {
        if (incomplete) {
            if (!RDNA4S.incompleteImages) RDNA4S.incompleteImages = [NSHashTable hashTableWithOptions:
                NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality];
            [RDNA4S.incompleteImages addObject:image];
        } else if ([segment->displayImageOverwrites containsObject:image]) {
            [RDNA4S.incompleteImages removeObject:image];
        }
    }
    if (incomplete && sources.count && !RDNA4S.incompleteSources) RDNA4S.incompleteSources = [NSMutableIndexSet indexSet];
    for (NSUInteger sid = sources.firstIndex; sources && sid != NSNotFound; sid = [sources indexGreaterThanIndex:sid]) {
        if (incomplete) [RDNA4S.incompleteSources addIndex:sid];
        else if (segment->displaySourceOverwrites[@(sid)]) [RDNA4S.incompleteSources removeIndex:sid];
        const BOOL rejected = [RDNA4S.incompleteSources containsIndex:sid];
        for (int inst = 0; inst <= 2; ++inst) {
            rdna4df_t *sm = inst ? &RDNA4XI(inst).sm : &RDNA4S.sm;
            if (rejected) rdna4df_submit_rejected_write(sm, (uint32_t)sid, seq);
            else rdna4df_invalidate_deferred_source(sm, (uint32_t)sid, seq);
        }
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
- (void)rdna4DispSeq {
    if (_shaderFallback && _ndw) {
        _dseq = atomic_fetch_add(&RDNA4S.seq, 1) + 1;
        pthread_mutex_lock(&RDNA4S.mu);
        for (int writeIndex = 0; writeIndex < _ndw; ++writeIndex) {
            const uint32_t sid = [_dw[writeIndex].t rdna4DispSid];
            rdna4df_submit_rejected_write(&RDNA4S.sm, sid, _dseq);
            for (int inst = 1; inst <= 2; ++inst)
                rdna4df_submit_rejected_write(&RDNA4XI(inst).sm, sid, _dseq);
        }
        pthread_mutex_unlock(&RDNA4S.mu);
        return;
    }
    if (_dslot >= 0 || _dlate || (_dcrc && _dtex)) _dseq = atomic_fetch_add(&RDNA4S.seq, 1) + 1;
    if ((_dslot >= 0 || _dlate) && (_dsid || _dcid)) {
        pthread_mutex_lock(&RDNA4S.mu);
        rdna4df_t *const dsm = rdna4x_desc((uint32_t)_dinst) ? &RDNA4XI(_dinst).sm : &RDNA4S.sm;
        const BOOL targetIncomplete = [_dtex rdna4DispImgOnly] ? [RDNA4S.incompleteImages containsObject:_dtex]
            : [RDNA4S.incompleteSources containsIndex:_dsid];
        if (_displayIncomplete || targetIncomplete) {
            _dlate = NO;
            _dcrc = NO;
            _cbcls = RDNA4DF_C_SHADER_FALLBACK + 1;
            if (_dslot >= 0) {
                dsm->poison[_dslot] = 1;
                dsm->head = -1;
                if (_dcid) {
                    dsm->last_sub_id = _dcid;
                    dsm->last_sub_valid = 0;
                }
            }
            pthread_mutex_unlock(&RDNA4S.mu);
            rdna4cbl_skip(_dsid, _dseq, _dslot, "incomplete-source", 0);
            return;
        }
        if (_dsid) rdna4df_submit(dsm, _dslot, _dsid, _dseq);
        if (_dslot >= 0) {
            dsm->sub_commit[_dslot] = _tcommit;
            if (rdna4dc_commit_newer(RDNA4S.commits, _dsid, _tcommit)) dsm->commit_sup[_dslot] = 1;
        }
        rdna4df_chain_submit(dsm, _dslot, _dcid, _dbid);
        pthread_mutex_unlock(&RDNA4S.mu);
    }
    if (_dcrc && _dtex && [_dtex rdna4IOSRef]) rdna4s_crc_note(IOSurfaceGetID([_dtex rdna4IOSRef]), _dseq);   // CRC diagnostic: which surface this submission wrote
}   // call with the submit lock held, right before vkQueueSubmit
- (void)rdna4PrepareForSubmission {
    if (_prepared) return;
    _startTime = rdna4_now();
    NSError *error = nil;
    if (![self rdna4Finish:&error]) _prepareErr = error ?: _encErr ?: rdna4_err(55, @"command buffer preparation failed");
    _prepared = YES;
}
- (BOOL)rdna4Finish:(NSError **)err {
    if (_ended) return _encErr == nil;
    _ended = YES;
    if (_encErr) { if (err) *err = _encErr; return NO; }   // never submit a command buffer whose encoding failed
    // Only writes since the previous segment boundary. A signal segment already flushed and
    // cleared _eventDirty, so a CPU listener's later page changes cannot be overwritten here.
    [self rdna4FlushEventWrites:_vk];
    if (_encErr) { if (err) *err = _encErr; return NO; }
    {   // S5.2a: append barrier + copy (display texture's import -> a free scanout slot) + barrier; the completion block presents it
        RDNA4Texture *dt = nil; int nd = 0; uint32_t dmask = 0; const char *dfns = ""; BOOL dfinal = NO; const rdna4df_wr_t *dwr = NULL;
        int dmgon = rdna4s_damage_on();
        for (RDNA4Texture *t in _iow) if ([t rdna4IsDisp]) {   // the presented surface: the last presentable one (FINAL pass; with damage on also a SkyLight composite pass) in write order, else the last display surface written
            nd++; uint32_t m = 0; const char *f = ""; const rdna4df_wr_t *w = NULL;
            for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { m = _dw[i].mask; f = _dw[i].fns; w = &_dw[i].wr; break; }
            BOOL fin = (m & RDNA4DF_W_DRAW_FINAL) != 0 || (dmgon && (m & RDNA4DF_W_DRAW_COMP) != 0);
            if (!dt || fin || !dfinal) { dt = t; dmask = m; dfns = f; dfinal = fin; dwr = w; }
        }
        if (_shaderFallback) dmask |= RDNA4DF_W_SHADER_FALLBACK;
        if (nd > 1) { uint64_t m = atomic_fetch_add(&RDNA4S.multi, 1) + 1; if (m <= 8) RDNA4LOG("scanout: command buffer wrote %d display surfaces; the LAST one in write order is presented (study: frame-order ambiguity, #%llu)", nd, (unsigned long long)m); }
        if (dt) {
            _frame = YES; _cbcls = rdna4df_class(dmask) + 1; _cbdsid = [dt rdna4DispSid];
            BOOL imgsrc = [dt rdna4DispImgOnly];   // P6: a protected display surface: copy from its VkImage
            int pinst = 0, ptent = 0;   // bundle 13/15: the instance this frame is PLANNED for (0 = the DP, 1 = the monitor A, 2 = the monitor B) and whether the plan is only tentative (unknown surface: instance 0 only)
            BOOL take = rdna4s_disp_account(dmask, [dt rdna4DispSid], dfns, atomic_fetch_add(&RDNA4S.cbno, 1) + 1, dwr, &pinst, &ptent);   // native #12 P1/P2/D1: only a presentable frame takes a slot
            rdna4df_plan_t pl; pl.slot = -1; pl.inst = 0; pl.tent = 0;
            const rdna4x_desc_t *const pxd = rdna4x_desc((uint32_t)pinst);   // bundle 15: the planned instance's geometry (the DP's plane for instance 0)
            const uint32_t gw_ = pxd ? pxd->w : RDNA4S.pw, gh_ = pxd ? pxd->h : RDNA4S.ph, gp_ = pxd ? pxd->pitch_bytes : RDNA4S.pitch;
            // bundle 17 (R-B1): the width / height check applies to EVERY frame, not only the image-sourced ones; a buffer-sourced frame (the surface's pages) also needs bytes-per-row == the plan's pitch (the copy reads pitch*height bytes of the surface)
            if (take && !((imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES) && [dt bytesPerPixel] == 4 && [dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_))) {   // refuse before a slot is taken
                static _Atomic int rl; if (atomic_fetch_add(&rl, 1) < 8) RDNA4LOG("scanout: display surface %u cannot be copied to the slot (%lux%lu bpp %lu bpr %lu, %s; plan %ux%u pitch %u): frame not copied", [dt rdna4DispSid], (unsigned long)[dt width], (unsigned long)[dt height], (unsigned long)[dt bytesPerPixel], (unsigned long)(imgsrc ? 0 : [dt rdna4IOSBytesPerRow]), imgsrc ? "no CPU mapping" : "buffer source", gw_, gh_, gp_);
                take = NO; }
            int sl = take ? rdna4s_plan(dwr, pinst, ptent, &pl) : -1;
            // A mapped display copy reads the IOSurface pages (whether path (a) or (b)), so a CPU update
            // after the signal is visible without touching the texture. Only a protected, base-less
            // display surface copies from its image, and it has no CPU mapping to refresh.
            if (sl >= 0) { rdna4_full_barrier(_vk); if (imgsrc) rdna4s_record_copy_img(_vk, dt, &pl); else rdna4s_record_copy(_vk, [dt rdna4DispBuf], [dt rdna4IOSOffset], &pl); rdna4_full_barrier(_vk); _dtex = dt; _dslot = sl; _dinst = pl.inst; _dcls = rdna4df_class(dmask); _dcid = pl.id; _dbid = pl.baseid; _dsid = [dt rdna4DispSid]; }
            else if (take && !imgsrc) { _dlate = YES; _dtex = dt; _dinst = pinst; _dcls = rdna4df_class(dmask); _dsid = [dt rdna4DispSid]; }
            if (rdna4s_crc_on() && pinst == RDNA4X_PLAN_DP) { _dcrc = YES; _dtex = dt; }   // bundle 13: the CRC diagnostic reads the DP's slot: never a monitor B frame
        }
    }
    NSError *endError = nil;
    if (![self rdna4EndCurrentSegment:&endError]) { [self rdna4DispDone:VK_ERROR_UNKNOWN]; if (err) *err = endError; return NO; }
    if (_segments.count > 1 && _curSegment == _segments.lastObject && !_curSegment->hasWork &&
        !_curSegment->waits.count && !_curSegment->signals.count && !_curSegment->fenceWaits.count &&
        !_curSegment->fenceUpdates.count && !_curSegment->argumentIntents.count &&
        !_iow.count && !_iot.count)
        _curSegment->pruned = YES;
    for (RDNA4CommandSegment *segment in _segments.reverseObjectEnumerator) {
        if (segment->pruned) continue;
        segment->terminal = YES;
        break;
    }
    return YES;
}

- (void)rdna4EnsureScheduledWithError:(NSError *)err {
    if (_scheduleAfterPublish && err == nil) return;
    if (atomic_exchange(&_scheduledDelivered, 1)) return;
    [self didScheduleWithStartTime:_startTime endTime:rdna4_now() error:err];
    RDNA4LOGR("didSchedule sent (ok=%d)", err == nil);
}

- (BOOL)rdna4ClaimTerminal {
    return atomic_exchange(&_terminalClaimed, 1) == 0;
}
- (void)rdna4ReleaseCompletionRefs {
    // Release retained resources AFTER commandBufferDidComplete: so Metal
    // completion handlers still observe live resources during callback execution.
    [_refs removeAllObjects];
    [_residencySnapshots removeAllObjects];
    [_residencyHeapResources removeAllObjects];
    for (RDNA4CommandSegment *segment in _segments) {
        if (!segment->retirementUnknown) {
            [segment->argumentRefs removeAllObjects];
            [segment->commandRefs removeAllObjects];
        }
    }
}
- (void)rdna4DeliverTerminalSuccessWithQueue:(RDNA4CommandQueue *)queue {
    rdna4_capture_surfaces(_iot, _cbid);
    [self rdna4ClearHostActions];
    dispatch_async([queue rdna4CompletionQueue], ^{
        self->_scheduleAfterPublish = NO;
        [self rdna4EnsureScheduledWithError:nil];
        [self rdna4CBFence:VK_SUCCESS];
        [self rdna4DispDone:VK_SUCCESS];
        [self rdna4PoolDone];
        [queue commandBufferDidComplete:self startTime:self->_startTime completionTime:rdna4_now() error:self->_encErr];
        [self rdna4ReleaseCompletionRefs];
        [queue rdna4DidComplete:self];
        rdna4_mark_clean();
    });
}

- (void)rdna4DeliverTerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault {
    [self rdna4ClearHostActions];
    [self rdna4CancelUnsubmittedArgumentIntents];
    [self rdna4FailPendingFenceUpdates:fault];
    [self rdna4FailPendingSignals];
    if (unresolved) rdna4_quarantine(self, fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault);
    dispatch_async([queue rdna4CompletionQueue], ^{
        [self rdna4EnsureScheduledWithError:err];
        if (!unresolved) {
            [self rdna4PoolDone];
            if (self->_cbSubmitLogged) [self rdna4CBFence:(int)(fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault)];
            else [self rdna4CBDropWithError:err fault:(fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault)];
            [self rdna4DispDone:VK_ERROR_UNKNOWN];
        } else {
            [self rdna4CBFence:(int)fault];
        }
        [queue commandBufferDidComplete:self startTime:self->_startTime completionTime:rdna4_now() error:err];
        if (!unresolved) [self rdna4ReleaseCompletionRefs];
        [queue rdna4DidComplete:self];
    });
}
- (void)rdna4TerminalSuccessWithQueue:(RDNA4CommandQueue *)queue {
    if (![self rdna4ClaimTerminal]) return;
    [self rdna4DeliverTerminalSuccessWithQueue:queue];
}
- (void)rdna4TerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault {
    if (![self rdna4ClaimTerminal]) return;
    [self rdna4DeliverTerminalFailure:err queue:queue unresolved:unresolved fault:fault];
}

// The completion of a segment: the fence result `r`, the delivered flag of the test
// hook and the (optional) error the wait already built. Runs ON the serial completion
// queue, so every delivery stays ordered.
- (void)rdna4FenceDone:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue r:(VkResult)r t1:(uint64_t)t1 slices:(int)slices
             delivered:(BOOL)delivered werr:(NSError *)werr {
    if (r != VK_SUCCESS && !werr)
        werr = rdna4_err(54, [NSString stringWithFormat:@"the GPU fence = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : r == VK_TIMEOUT ? " (TIMEOUT: HUNG?)" : ""]);
    { uint64_t tg_ = rdna4_now() - t1; rdna4t_add(&T1.gpu, tg_); if (self->_frame) rdna4t_add(&T1.fgpu, tg_); }
    RDNA4LOGR("segment fence wait done = %d after %d slice(s)", r, slices);
    BOOL terminal = NO;
    BOOL terminalClaimed = NO;
    pthread_mutex_lock(&self->_smu);
    if (r == VK_SUCCESS) segment->done = YES;
    terminal = segment->terminal;
    if (!delivered && (r != VK_SUCCESS || terminal)) terminalClaimed = [self rdna4ClaimTerminal];
    pthread_mutex_unlock(&self->_smu);
    if (r == VK_SUCCESS) {
        [self rdna4SignalSegment:segment releaseRefs:(delivered || !terminal)];
        if (delivered) {
            [self rdna4CBFence:VK_SUCCESS];
            [self rdna4DispDone:VK_SUCCESS];
            [self rdna4PoolDone];
            [self rdna4ReleaseCompletionRefs];
            [queue rdna4DidComplete:self];
            rdna4_mark_clean();
        } else if (terminalClaimed) {
            [self rdna4DeliverTerminalSuccessWithQueue:queue];
        } else if (!terminal) {
            pthread_mutex_lock(&self->_smu);
            self->_inFlight = NO;
            pthread_mutex_unlock(&self->_smu);
            [queue rdna4Kick];
        }
        return;
    }
    if (delivered) {
        [self rdna4FailPendingFenceUpdates:r];
        rdna4_quarantine(self, r);
        [queue rdna4DidComplete:self];
        return;
    }
    if (terminalClaimed) [self rdna4DeliverTerminalFailure:werr queue:queue unresolved:YES fault:r];
}
// Production wait: run bounded vkWaitForFences slices on a concurrent waiter queue
// so a stuck fence never blocks the serial completion queue or other queues, and
// normal completions wake immediately from kernel n1c_wait (no 2 ms timer floor).
- (void)rdna4FenceWait:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue {
    static _Atomic int testUsed;
    static long testMs = -1;
    if (testMs < 0) { const char *e = garnet_getenv("GARNET_TEST_TIMEOUT_MS", "RDNA4M_TEST_TIMEOUT_MS"); testMs = e ? atol(e) : 0; }
    BOOL testWait = testMs > 0 && atomic_exchange(&testUsed, 1) == 0;
    long tms = testMs;
    uint64_t t1 = _readyTime;
    uint64_t tenq = rdna4_now();
    if (!testWait) {
        dispatch_queue_t completionQueue = [queue rdna4CompletionQueue];
        const rdna4_fence_wait_ctx waitCtx = { .fence = segment->fence, .cbid = _cbid, .t1 = t1 };
        // Transfer +1 refs to completionQueue so the waiter thread never retains or
        // deallocates self/segment/queue after handoff.
        CFTypeRef cbRef = CFBridgingRetain(self);
        CFTypeRef segRef = CFBridgingRetain(segment);
        CFTypeRef qRef = CFBridgingRetain(queue);
        dispatch_async(rdna4_fence_wait_queue(), ^{
            rdna4t_add(&T1.wake, rdna4_now() - tenq);
            rdna4_fence_wait_ctx ctx = waitCtx;
            int slices = 0;
            const VkResult r = (VkResult)rdna4fw_wait_bounded(
                rdna4_fence_wait_slice, &ctx, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                rdna4_fence_wait_pending, &slices);
            if (r == VK_TIMEOUT) {
                const uint64_t now = rdna4_now();
                const uint64_t elapsedMs = now > t1 ? (now - t1) / 1000000ULL : 0ULL;
                RDNA4LOG("FENCE STALL: command buffer %llu: the GPU fence did not signal within %llu ms (%d slice(s)); failing this submission",
                         (unsigned long long)ctx.cbid, (unsigned long long)elapsedMs, slices);
            }
            dispatch_async(completionQueue, ^{
                RDNA4CommandBuffer *cb = (__bridge_transfer RDNA4CommandBuffer *)cbRef;
                RDNA4CommandSegment *seg = (__bridge_transfer RDNA4CommandSegment *)segRef;
                RDNA4CommandQueue *q = (__bridge_transfer RDNA4CommandQueue *)qRef;
                [cb rdna4FenceDone:seg queue:q r:r t1:t1 slices:slices delivered:NO werr:nil];
            });
        });
        return;
    }
    dispatch_async([queue rdna4CompletionQueue], ^{
        rdna4t_add(&T1.wake, rdna4_now() - tenq);
        NSError *werr = nil;
        VkResult r = vkWaitForFences(RDNA4R.dev, 1, &segment->fence, VK_TRUE, (uint64_t)tms * 1000000ULL);
        int slices = 1;
        BOOL delivered = NO;
        RDNA4LOG("TEST_TIMEOUT: first fence wait of %ld ms = %d", tms, r);
        if (r == VK_TIMEOUT && !self->_hasEvents && atomic_exchange(&self->_terminalClaimed, 1) == 0) {
            werr = rdna4_err(54, [NSString stringWithFormat:@"vkWaitForFences = %d (TIMEOUT after %ld ms; test hook, GPU still busy)", r, tms]);
            [queue commandBufferDidComplete:self startTime:t1 completionTime:rdna4_now() error:werr];
            RDNA4LOG("TEST_TIMEOUT: commandBufferDidComplete(error) sent; still waiting the fence before release");
            delivered = YES;
        }
        if (r == VK_TIMEOUT) {
            rdna4_fence_wait_ctx ctx = { .fence = segment->fence, .cbid = self->_cbid, .t1 = t1 };
            r = (VkResult)rdna4fw_wait_bounded(
                rdna4_fence_wait_slice, &ctx, slices, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                NULL, &slices);
        }
        [self rdna4FenceDone:segment queue:queue r:r t1:t1 slices:slices delivered:delivered werr:werr];
    });
}

static NSUInteger rdna4_linear_image_span(RDNA4Texture *texture) {
    return ([texture height] - 1) * [texture rdna4IOSBytesPerRow] + [texture width] * [texture bytesPerPixel];
}

- (BOOL)rdna4PrepareLinearImages:(RDNA4CommandSegment *)segment error:(NSError **)error {
    NSUInteger scratchLength = 0;
    for (RDNA4Texture *texture in segment->touches) {
        if (![texture rdna4IOSLinearNeedsInit]) continue;
        const NSUInteger bytes = rdna4_linear_image_span(texture);
        scratchLength = MAX(scratchLength, bytes);
    }
    if (!scratchLength) return YES;
    RDNA4Buffer *saved = [[RDNA4Buffer alloc] initWithDevice:[self device] length:scratchLength
        options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:error];
    if (!saved) return NO;
    [segment->commandRefs addObject:saved];

    VkCommandBuffer init = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    if (_nq) {
        init = [_nq rdna4TakeCmdBuf:error];
        if (!init) result = VK_ERROR_OUT_OF_HOST_MEMORY;
    } else {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = VK_NULL_HANDLE, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        rdna4_qlock();
        result = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &init);
        pthread_mutex_unlock(&RDNA4R.qlock);
    }
    if (result == VK_SUCCESS) {
        segment->linearInit = init;
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        result = vkBeginCommandBuffer(init, &begin);
    }
    if (result != VK_SUCCESS) {
        *error = rdna4_err(55, [NSString stringWithFormat:@"IOSurface initialization command begin = %d", result]);
        return NO;
    }
    for (RDNA4Texture *texture in segment->touches)
        if ([texture rdna4IOSLinearNeedsInit])
            rdna4_ios_linear_init(init, texture, saved, rdna4_linear_image_span(texture));
    result = vkEndCommandBuffer(init);
    if (result == VK_SUCCESS) return YES;
    *error = rdna4_err(55, [NSString stringWithFormat:@"IOSurface initialization command end = %d", result]);
    return NO;
}

- (BOOL)rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue {
    pthread_mutex_lock(&_smu);
    if (atomic_load(&_terminalClaimed) || _inFlight) {
        pthread_mutex_unlock(&_smu);
        return NO;
    }
    if (_prepareErr) {
        NSError *error = _prepareErr;
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    if (!_prepared) {
        NSError *error = rdna4_err(55, @"command buffer reached the scheduler without preparation");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    RDNA4CommandSegment *segment = nil;
    while (_nextSegment < _segments.count) {
        segment = _segments[_nextSegment];
        if (!segment->pruned) break;
        _nextSegment++;
    }
    if (!segment || _nextSegment >= _segments.count) {
        NSError *error = rdna4_err(55, @"command buffer has no submit-ready segment");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    VkResult fault = (VkResult)atomic_load(&rdna4_submit_fault);
    if (fault != VK_SUCCESS) {
        NSError *error = rdna4_err(53, @"An earlier GPU submission is unresolved; restart the process before submitting again");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:fault];
        return NO;
    }
    NSError *error = nil;
    int ready = [self rdna4WaitsReady:segment error:&error];
    if (ready == 0) {
        pthread_mutex_unlock(&_smu);
        return YES;
    }
    if (ready < 0) {
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }

    if (segment->hostOnly) {
        if (segment->hostActionAccepted) {
            BOOL terminal = segment->terminal;
            BOOL terminalClaimed = terminal ? [self rdna4ClaimTerminal] : NO;
            segment->done = YES;
            _nextSegment++;
            pthread_mutex_unlock(&_smu);
            if (terminal) {
                if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
            } else {
                [queue rdna4Kick];
            }
            return NO;
        }
        if (segment->hostActionRunning) {
            pthread_mutex_unlock(&_smu);
            return YES;
        }

        BOOL (^action)(NSError **) = segment->hostAction;
        if (!action) {
            NSError *failure = rdna4_err(67, @"ordered host action has no runnable block");
            segment->hostActionAccepted = YES;
            segment->done = YES;
            _nextSegment++;
            BOOL claimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&_smu);
            if (claimed) [self rdna4DeliverTerminalFailure:failure queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
            return NO;
        }

        segment->hostActionRunning = YES;
        segment->hostAction = nil;
        _inFlight = YES;
        pthread_mutex_unlock(&_smu);

        NSError *actionError = nil;
        BOOL accepted = NO;
        @try {
            accepted = action(&actionError);
        } @catch (NSException *exception) {
            actionError = rdna4_err(67, [NSString stringWithFormat:@"ordered host action raised: %@",
                                         exception.reason ?: @"unknown exception"]);
        }

        pthread_mutex_lock(&_smu);
        segment->hostActionRunning = NO;
        segment->hostActionAccepted = YES;
        segment->done = YES;
        _nextSegment++;
        _inFlight = NO;
        if (!accepted) {
            NSError *failure = actionError ?: rdna4_err(67, @"ordered host action was rejected without an error");
            BOOL claimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&_smu);
            if (claimed) [self rdna4DeliverTerminalFailure:failure queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
            return NO;
        }

        BOOL terminal = segment->terminal;
        BOOL terminalClaimed = terminal ? [self rdna4ClaimTerminal] : NO;
        pthread_mutex_unlock(&_smu);
        if (terminal) {
            if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
        } else {
            [queue rdna4Kick];
        }
        return NO;
    }

    if (![self rdna4PrepareLinearImages:segment error:&error]) {
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error ?: rdna4_err(55, @"IOSurface initialization failed")
            queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkResult r = rdna4_fence_take(&fence);
    if (r != VK_SUCCESS) {
        error = rdna4_err(52, [NSString stringWithFormat:@"vkCreateFence = %d", r]);
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:r];
        return NO;
    }
    segment->fence = fence;
    _fence = fence;
    VkCommandBuffer commands[] = { segment->linearInit, segment->vk };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = segment->linearInit ? 2 : 1, .pCommandBuffers = segment->linearInit ? commands : commands + 1 };
    BOOL submitAttempted = NO, nativeDone = NO;
    uint64_t argumentSubmissionEpoch = 0;
    uint64_t tLock = rdna4_now(); rdna4_qlock(); tLock = rdna4_now() - tLock;
    if (tLock > 250000000ull) RDNA4LOG("SUBMIT STALL: the queue lock was held for %.1f ms before command buffer %llu (a frame thread waits here: the display freezes)", (double)tLock / 1e6, (unsigned long long)_cbid);
    r = (VkResult)atomic_load(&rdna4_submit_fault);
    if (r == VK_SUCCESS) {
        [self rdna4DispSourceWrites:segment];
        if (_shaderFallback || (segment->terminal && !_dispSeqLogged)) {
            [self rdna4DispSeq];
            _dispSeqLogged = YES;
        }
        if (!_cbSubmitLogged) {
            [self rdna4CBSubmit:_startTime];
            _cbSubmitLogged = YES;
        }
        if (!_hasEvents && [self rdna4NativeEligible]) {
            uint64_t tIdle = rdna4_now(); r = rdna4_vkQueueWaitIdle(RDNA4R.q); tIdle = rdna4_now() - tIdle;
            if (tIdle > 250000000ull) RDNA4LOG("SUBMIT STALL: vkQueueWaitIdle took %.1f ms on the native-buffer route (command buffer %llu)", (double)tIdle / 1e6, (unsigned long long)_cbid);
            if (r == VK_SUCCESS) {
                bool fallback = false;
                int nr = [self rdna4NativeExecute:&fallback];
                nativeDone = !fallback;
                if (nr) {
                    r = nr == -ETIMEDOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
                    error = rdna4_err(53, [NSString stringWithFormat:@"native buffer operation = %d", nr]);
                }
            }
            if (r == VK_SUCCESS && nativeDone)
                argumentSubmissionEpoch = rdna4_argument_next_submission_epoch();
            if (r != VK_SUCCESS) {
                submitAttempted = YES;
                atomic_store(&rdna4_submit_fault, (int)r);
            }
        }
        if (r == VK_SUCCESS && !nativeDone) {
            submitAttempted = YES;
            r = vkQueueSubmit(RDNA4R.q, 1, &si, fence);
            if (r != VK_SUCCESS) atomic_store(&rdna4_submit_fault, (int)r);
            else {
                if (segment->linearInit)
                    for (RDNA4Texture *texture in segment->touches)
                        if ([texture rdna4IOSLinearNeedsInit]) [texture rdna4IOSLinearDidSubmit];
                argumentSubmissionEpoch = rdna4_argument_next_submission_epoch();
            }
        }
    }
    pthread_mutex_unlock(&RDNA4R.qlock);
    RDNA4LOGR("GPU segment submission result = %d (route=%s)", r, nativeDone ? "native-buffer" : "vulkan");
    if (submitAttempted || nativeDone) _submitAttempted = YES;
    if (r != VK_SUCCESS && !error) error = rdna4_err(53, [NSString stringWithFormat:@"GPU submission = %d", r]);
    if (r != VK_SUCCESS) {
        // A failed submit may have reached the GPU; only later, unattempted
        // segments can roll back their ownership while this one is quarantined.
        segment->retirementUnknown = submitAttempted;
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (!claimed) return NO;
        [self rdna4EnsureScheduledWithError:error];
        [self rdna4DeliverTerminalFailure:error queue:queue unresolved:submitAttempted fault:r];
        return NO;
    }

    segment->submitted = YES;
    segment->argumentSubmissionEpoch = argumentSubmissionEpoch;
    // Encoding order is not submission order: attach the epoch only after the
    // native path or vkQueueSubmit has accepted this segment.
    _nextSegment++;
    _inFlight = YES;
    _readyTime = rdna4_now();
    pthread_mutex_unlock(&_smu);
    // Buffer owner publication may wait for a setter or refinement. Never do
    // that while holding the command-buffer submit lock.
    for (id intent in segment->argumentIntents)
        rdna4_argument_sampler_submit(intent, argumentSubmissionEpoch);
    [self rdna4EnsureScheduledWithError:nil];
    if (nativeDone) {
        dispatch_async([queue rdna4CompletionQueue], ^{
            BOOL terminalClaimed = NO;
            pthread_mutex_lock(&self->_smu);
            segment->done = YES;
            BOOL terminal = segment->terminal;
            if (terminal) terminalClaimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&self->_smu);
            [self rdna4SignalSegment:segment];
            if (terminal) {
                if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
            } else {
                pthread_mutex_lock(&self->_smu);
                self->_inFlight = NO;
                pthread_mutex_unlock(&self->_smu);
                [queue rdna4Kick];
            }
        });
    } else {
        [self rdna4FenceWait:segment queue:queue];
    }
    return NO;
}

// build 18 (P2): hand a render pass its occlusion bookkeeping and RDNA4OCC_MAXQ reset-able query indices from this command buffer's pool (a pool holds 2 chunks; a new pool is made when it is used up).
// Returns the rdna4occ (owned by this command buffer; never nil once the buffer is usable). *qp == VK_NULL_HANDLE: no pool could be made, so the pass answers "visible" for every offset it selects.
- (rdna4occ *)rdna4OccNew:(RDNA4Buffer *)vb accumulate:(int)acc pool:(VkQueryPool *)qp base:(uint32_t *)base {
    rdna4occ *o = calloc(1, sizeof *o);
    if (!o) { *qp = VK_NULL_HANDLE; return NULL; }
    rdna4occ_init(o, (uint64_t)[vb length], acc);
    *qp = VK_NULL_HANDLE; *base = 0;
    if (RDNA4R.occOK) {
        if (!_qpCur || _qpUsed + RDNA4OCC_MAXQ > 2 * RDNA4OCC_MAXQ) {
            VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 2 * RDNA4OCC_MAXQ };
            VkQueryPool np = VK_NULL_HANDLE; VkResult r = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &np);
            if (r == VK_SUCCESS) { if (!_qpools) _qpools = [NSMutableArray array]; [_qpools addObject:[NSValue valueWithPointer:(void *)np]]; _qpCur = np; _qpUsed = 0; }
            else { RDNA4LOG("occlusion: vkCreateQueryPool = %d: this pass answers visible", r); _qpCur = VK_NULL_HANDLE; }
        }
        if (_qpCur) { *qp = _qpCur; *base = _qpUsed; _qpUsed += RDNA4OCC_MAXQ; }
    }
    if (*qp == VK_NULL_HANDLE) rdna4occ_dead(o);
    if (_noj == _coj) { uint32_t nc = _coj ? _coj * 2 : 4; void *nn = realloc(_oj, nc * sizeof *_oj); if (!nn) { free(o); *qp = VK_NULL_HANDLE; return NULL; } _oj = nn; _coj = nc; }
    _oj[_noj].o = o; _oj[_noj].qp = *qp; _oj[_noj].base = *base; _noj++;
    if (!_ojb) _ojb = [NSMutableArray array];
    [_ojb addObject:vb]; [self rdna4Retain:vb];
    return o;
}
// The GPU is finished with this command buffer (or it never ran): write every selected offset of every visibility buffer, once. Results are read WITHOUT waiting (a lost device must not hang the completion thread):
// only when the fence reads VK_SUCCESS are the queries read, with availability; anything else answers "visible" (the fail-safe in garnet_occ.h).
- (void)rdna4OccResolve {
    if (!_noj) return;
    BOOL ran = NO;
    if (RDNA4R.ok && RDNA4R.occOK && _fence != VK_NULL_HANDLE) ran = vkGetFenceStatus(RDNA4R.dev, _fence) == VK_SUCCESS;
    uint32_t nj = _noj; _noj = 0;
    for (uint32_t j = 0; j < nj; j++) {
        rdna4occ *o = _oj[j].o; RDNA4Buffer *vb = _ojb[j]; uint8_t *base = (uint8_t *)[vb contents];
        uint64_t res[RDNA4OCC_MAXQ]; BOOL ok = ran && _oj[j].qp != VK_NULL_HANDLE;
        if (ok && o->nq) {
            uint64_t raw[2 * RDNA4OCC_MAXQ];   // (result, availability) pairs
            VkResult r = vkGetQueryPoolResults(RDNA4R.dev, _oj[j].qp, _oj[j].base, o->nq, sizeof raw, raw, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (r != VK_SUCCESS) ok = NO;
            if (ok && !rdna4occ_read(o, raw, res)) ok = NO;   // an interval that is not available answers visible
        }
        if (!ok) RDNA4LOGR("occlusion: pass with %u offset(s) answered VISIBLE (fail-safe: fence %s, %u interval(s), overflow %d)", o->nu, ran ? "done" : "not done", o->nq, o->overflow);
        for (uint32_t u = 0; u < o->nu && base; u++) {
            uint64_t *slot = (uint64_t *)(void *)(base + o->u[u].off);
            *slot = rdna4occ_final(o, u, ok ? res : NULL, ok, *slot);
        }
        RDNA4LOGR("occlusion: resolved %u interval(s) into %u offset(s) (%s)", o->nq, o->nu, o->accumulate ? "Accumulate" : "Reset");
        free(o); _oj[j].o = NULL;
    }
    [_ojb removeAllObjects];
}
- (void)rdna4ArgumentIntent:(id)intent {
    if (!intent) { [self rdna4Fail:@"argument write intent allocation failed"]; return; }
    if (!_curSegment || _curSegment->ended) {
        rdna4_argument_sampler_abandon(intent);
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    [_curSegment->argumentIntents addObject:intent];
    _curSegment->hasWork = YES;
}
- (void)rdna4ArgumentWrite:(id)buffer range:(NSRange)range candidates:(NSSet *)candidates fillOnly:(BOOL)fillOnly {
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    if (!buffer || !range.length) {
        [self rdna4Fail:@"argument write intent has an empty range"];
        return;
    }
    for (id intent in _curSegment->argumentIntents) {
        if (rdna4_argument_sampler_intent_matches(intent, buffer, range, fillOnly)) {
            rdna4_argument_sampler_add_candidates(intent, candidates);
            return;
        }
    }
    RDNA4ArgumentWriteIntent *created = rdna4_argument_sampler_intent(buffer, range, candidates,
                                                                     fillOnly);
    if (!created) {
        [self rdna4Fail:@"argument write intent range is invalid"];
        return;
    }
    [self rdna4RetainArgument:buffer];
    [self rdna4ArgumentIntent:created];
}
- (void)rdna4ArgumentWriteRanges:(id)buffer ranges:(NSArray<NSValue *> *)ranges candidates:(NSSet *)candidates {
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    if (!buffer || ![ranges isKindOfClass:[NSArray class]] || !ranges.count) {
        [self rdna4Fail:@"argument write intent has no ranges"];
        return;
    }
    NSMutableArray<NSValue *> *normalized = [NSMutableArray arrayWithCapacity:ranges.count];
    for (id value in ranges) {
        if (![value isKindOfClass:[NSValue class]]) {
            [self rdna4Fail:@"argument write intent has a non-range value"];
            return;
        }
        NSRange range = [(NSValue *)value rangeValue];
        if (!range.length || range.location > [buffer length] ||
            range.length > [buffer length] - range.location) {
            [self rdna4Fail:@"argument write intent range is invalid"];
            return;
        }
        [normalized addObject:[NSValue valueWithRange:range]];
    }
    [normalized sortUsingComparator:^NSComparisonResult(NSValue *left, NSValue *right) {
        NSRange a = left.rangeValue, b = right.rangeValue;
        if (a.location != b.location)
            return a.location < b.location ? NSOrderedAscending : NSOrderedDescending;
        return a.length < b.length ? NSOrderedAscending :
            a.length > b.length ? NSOrderedDescending : NSOrderedSame;
    }];
    NSMutableArray<NSValue *> *merged = [NSMutableArray arrayWithCapacity:normalized.count];
    for (NSValue *value in normalized) {
        NSRange next = value.rangeValue;
        if (merged.count && next.location <= NSMaxRange(merged.lastObject.rangeValue)) {
            NSRange current = merged.lastObject.rangeValue;
            NSUInteger end = MAX(NSMaxRange(current), NSMaxRange(next));
            merged[merged.count - 1] =
                [NSValue valueWithRange:NSMakeRange(current.location,
                                                    end - current.location)];
        } else {
            [merged addObject:value];
        }
    }
    NSRange first = merged.firstObject.rangeValue;
    NSRange last = merged.lastObject.rangeValue;
    NSRange envelope = NSMakeRange(first.location, NSMaxRange(last) - first.location);
    for (id intent in _curSegment->argumentIntents) {
        if (rdna4_argument_sampler_intent_matches_ranges(
                intent, buffer, envelope, merged, NO)) {
            rdna4_argument_sampler_add_candidates(intent, candidates);
            return;
        }
    }
    RDNA4ArgumentWriteIntent *created = rdna4_argument_sampler_intent_ranges(
        buffer, envelope, merged, candidates, NO);
    if (!created) {
        [self rdna4Fail:@"argument write intent ranges are invalid"];
        return;
    }
    [self rdna4RetainArgument:buffer];
    [self rdna4ArgumentIntent:created];
}
- (void)rdna4CancelUnsubmittedArgumentIntents {
    for (RDNA4CommandSegment *segment in _segments) {
        if (segment->submitted || segment->retirementUnknown) continue;
        for (id intent in segment->argumentIntents) rdna4_argument_sampler_abandon(intent);
        [segment->argumentIntents removeAllObjects];
        segment->argumentSummaryAttempts = nil;
        [segment->argumentPools removeAllObjects];
        [segment->argumentRefs removeAllObjects];
        [segment->commandRefs removeAllObjects];
        [self rdna4ReleaseSegmentDescriptorPools:segment];
    }
}
- (void)rdna4PoolDone {
    [self rdna4UCRelease]; [self rdna4OccResolve]; if (_pser) rdna4_fence_close(_pser);
    for (RDNA4CommandSegment *segment in _segments) {
        if (segment->fence && segment->done && !segment->retirementUnknown) {
            VkFence f = segment->fence;
            segment->fence = VK_NULL_HANDLE;
            if (_fence == f) _fence = VK_NULL_HANDLE;
            rdna4_fence_give(f);
        }
    }
    _ring = nil;
    _argumentTable = nil;
    _ringUsed = 0;
    _argumentTableBytes = 0;
    _pool = VK_NULL_HANDLE;
    for (RDNA4CommandSegment *segment in _segments)
        [self rdna4ReleaseSegmentDescriptorPools:segment];
}
// Copies bytes into the command buffer's upload ring (Shared memory, 256-byte aligned: covers every storage/uniform offset alignment).
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo {
    if ((n && !p) || n > NSUIntegerMax - 255) {
        [self rdna4Fail:@"upload byte count or source is invalid"]; return NO;
    }
    NSUInteger need = ((n ? n : 1) + 255) & ~(NSUInteger)255;
    if (!_ring || _ringUsed > [_ring length] || need > [_ring length] - _ringUsed) {
        NSError *e = nil; NSUInteger sz = need > 262144 ? need : 262144;
        RDNA4Buffer *nb = [[RDNA4Buffer alloc] initWithDevice:[_nq device] length:sz options:MTLResourceStorageModeShared | (RDNA4P.on ? RDNA4_OPT_NOZERO : 0) error:&e];   // P1: slab range / recycled, not a kernel BO per command buffer; every byte is written before use
        if (!nb) { [self rdna4Fail:[NSString stringWithFormat:@"upload ring: %@", e.localizedDescription]]; return NO; }
        _ring = nb; _ringUsed = 0; [self rdna4RetainArgument:nb];
    }
    memcpy((uint8_t *)[_ring contents] + _ringUsed, p, n);
    *ob = _ring; *oo = _ringUsed; _ringUsed += need;
    return YES;
}
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!_pool) {
            VkDescriptorPool reuse_ = rdna4_dp_take();   // P1: a fence-cleared, reset pool of an earlier command buffer (VK_NULL_HANDLE when OFF)
            if (reuse_) _pool = reuse_;
            else {
                // A 256-set pool with room for 1024 storage buffers is RADV memory proportional to
                // maxSets x descriptors. One such pool per command buffer can fail under memory
                // pressure (login: many pipelines + imports at once) and a failed pool fails the
                // whole command buffer, i.e. a frame is never drawn. Degrade the bound instead:
                // the numbers are upper bounds, rdna4AllocSet already replaces an exhausted pool.
                static const uint32_t sets[3] = { 256, 64, 16 }, div[3] = { 1, 4, 16 };
                VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                for (int tier = 0; tier < 3; tier++) {
                    VkDescriptorPoolSize sz[10] = {
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_SAMPLER, 512 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 / div[tier] }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 256 / div[tier] } };
                    uint32_t nsz = rdna4_rt_descriptor_pool_append(sz, 9u, 10u, div[tier]);
                    VkDescriptorPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = sets[tier],
                                                      .poolSizeCount = nsz, .pPoolSizes = sz };
                    r = vkCreateDescriptorPool(RDNA4R.dev, &pc, NULL, &_pool);
                    if (r == VK_SUCCESS) break;
                    _pool = VK_NULL_HANDLE;
                    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) break;
                }
                if (r != VK_SUCCESS) { [self rdna4Fail:[NSString stringWithFormat:@"vkCreateDescriptorPool = %d (all three pool sizes refused)", r]]; return VK_NULL_HANDLE; }
                rdna4_dp_created();
            }
            if (!_curSegment || _curSegment->ended) {
                if (!(RDNA4P.on && rdna4_dp_give(_pool)))
                    vkDestroyDescriptorPool(RDNA4R.dev, _pool, NULL);
                _pool = VK_NULL_HANDLE;
                [self rdna4Fail:@"descriptor pool has no open command segment"];
                return VK_NULL_HANDLE;
            }
            [_curSegment->descriptorPools
                addObject:[NSValue valueWithPointer:(void *)_pool]];
        }
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = _pool, .descriptorSetCount = 1, .pSetLayouts = &l };
        VkDescriptorSet s = VK_NULL_HANDLE; VkResult r = vkAllocateDescriptorSets(RDNA4R.dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        _pool = VK_NULL_HANDLE;   // exhausted or fragmented: the next attempt makes a new pool
    }
    [self rdna4Fail:@"vkAllocateDescriptorSets failed twice (descriptor pool)"];
    return VK_NULL_HANDLE;
}
// Writes the descriptors of one stage's set from the encoder's bound state (see the binding-model comment).
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm {
    enum { MAXW = 256, MAXI = 1024 };
    if (n > MAXW) { [self rdna4Fail:[NSString stringWithFormat:@"%s stage has %u descriptors (> %d)", nm, n, MAXW]]; return NO; }
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXI]; VkDescriptorImageInfo ii[MAXI];
    VkBufferView texelViews[MAXI]; uint32_t nbi = 0, nii = 0, ntv = 0; id dev = [self device];
    VkWriteDescriptorSetAccelerationStructureKHR asw[MAXW]; VkAccelerationStructureKHR ash[MAXI]; uint32_t nash = 0;
    const rdna4rtr_stage_t rtStage = (nm && strstr(nm, "fragment") != NULL) ? RDNA4RTR_STAGE_FRAGMENT
        : (nm && strcmp(nm, "vertex") == 0) ? RDNA4RTR_STAGE_VERTEX : RDNA4RTR_STAGE_COMPUTE;
    for (uint32_t i = 0; i < n; i++) {
        const RDNA4PB *p = &pb[i];
        if (p->count > MAXI - nbi || p->count > MAXI - nii || p->count > MAXI - ntv) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: too many array descriptors", nm, p->binding]]; return NO; }
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = p->binding, .descriptorCount = p->count, .descriptorType = p->type };
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            if (p->samplerCoordinateFlags) {
                uint32_t flags[96] = {0};
                BOOL assigned[32] = {NO};
                for (uint32_t q = 0; q < n; ++q) {
                    const RDNA4PB *source = &pb[q];
                    if (source->type != VK_DESCRIPTOR_TYPE_SAMPLER) continue;
                    for (uint32_t k = 0; k < source->count; ++k) {
                        uint64_t logical = 0;
                        if (!rdna4_coordinate_band_index(source, k, &logical)) {
                            [self rdna4Fail:[NSString stringWithFormat:
                                @"%s sampler binding %u count %u crosses the 32-slot coordinate band",
                                nm, source->binding, source->count]];
                            return NO;
                        }
                        BOOL pixel = NO;
                        if (source->stat) {
                            pixel = source->pixelCoordinates;
                        } else {
                            id sampler = source->embedded
                                ? rdna4_embedded_resource(source, k, st, NO, dev)
                                : (logical < 16 ? st->smp[logical] : nil);
                            if (!sampler) {
                                if (source->embedded) {
                                    [self rdna4Fail:[NSString stringWithFormat:@"%s sampler(%llu): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, (unsigned long long)logical]];
                                    return NO;
                                }
                                continue;
                            }
                            pixel = rdna4_sampler_object_pixel(sampler);
                            [self rdna4RetainArgument:sampler];
                        }
                        RDNA4CoordinateFlagResult merged =
                            rdna4_coordinate_flag_merge(flags, assigned, logical,
                                                        pixel);
                        if (merged != RDNA4CoordinateFlagOK) {
                            [self rdna4Fail:[NSString stringWithFormat:
                                @"%s sampler coordinate %s at logical index %llu",
                                nm,
                                merged == RDNA4CoordinateFlagConflict
                                    ? "sources disagree" : "index exceeds 32 slots",
                                (unsigned long long)logical]];
                            return NO;
                        }
                    }
                }
                RDNA4Buffer *table = nil; NSUInteger offset = 0;
                if (p->stageResourceMetadata) {
                    for (unsigned index = 32; index < 96; ++index) flags[index] = UINT32_MAX;
                    for (unsigned index = 0; index < p->nullShapeCount; ++index) {
                        unsigned access = p->nullShapeAccess[index];
                        uint32_t ordinal = p->nullShapeRemap[index];
                        if (!p->stageTokenTable && rdna4ns_access_is_image(access)) {
                            uint32_t base = _argumentTokenCounts[access == RDNA4NS_ACCESS_STORAGE];
                            if (ordinal > UINT32_MAX - base) {
                                [self rdna4Fail:@"null-shape descriptor index overflow"];
                                return NO;
                            }
                            ordinal += base;
                        } else if (p->stageTokenTable &&
                                   rdna4ns_access_is_texel(access)) {
                            unsigned category = access == RDNA4NS_ACCESS_STORAGE_TEXEL
                                ? 4u : 3u;
                            uint32_t base = _argumentTokenCounts[category];
                            if (base && rdna4ns_texel_null_index(
                                    base, ordinal, &ordinal) != RDNA4NS_OK) {
                                [self rdna4Fail:@"null texel descriptor index overflow"];
                                return NO;
                            }
                        }
                        flags[32 + index] = ordinal;
                    }
                }
                NSUInteger bytes = p->stageResourceMetadata ? sizeof flags : 32 * sizeof *flags;
                if (![self rdna4Bytes:flags length:bytes buffer:&table offset:&offset]) return NO;
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){[table vkBuffer],
                    [table rdna4VkOffset] + offset, bytes};
                break;
            }
            if (p->argumentTable) {
                if (!_argumentTable) { [self rdna4Fail:@"GPU argument table was not prepared"]; return NO; }
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){[_argumentTable vkBuffer], [_argumentTable rdna4VkOffset] + _argumentTableOffset, _argumentTableBytes};
                break;
            }
            if (p->addressTable) {
                rdna4_buffer_table addresses = {0};
                for (uint32_t j = 0; j < 31; ++j) {
                    RDNA4BufSlot slot = st->buf[j];
                    if (!slot.b) continue;
                    uint64_t base = [slot.b gpuAddress];
                    if ([slot.b device] != dev || !rdna4_buffer_table_set(&addresses, j, base, slot.b.length, slot.off, [slot.b rdna4HostReadOnly])) {
                        [self rdna4Fail:@"buffer-address table contains an invalid bound resource"];
                        return NO;
                    }
                    [self rdna4RetainEncoded:slot.b];
                }
                RDNA4Buffer *table = nil; NSUInteger offset = 0;
                size_t tableBytes = p->addressTableVersion == RDNA4_BUFFER_TABLE_VERSION ? sizeof addresses : sizeof addresses.address;
                NSData *registry = nil;
                if (p->addressTableVersion == RDNA4_BUFFER_REGISTRY_VERSION) {
                    [self rdna4AttachQueueResidencySnapshots];
                    NSMutableSet *resources = [_bufferAddressResources mutableCopy] ?: [NSMutableSet set];
                    for (uint32_t j = 0; j < 31; ++j)
                        if (st->buf[j].b) [resources addObject:st->buf[j].b];
                    for (id snap in _residencySnapshots) {
                        if ([snap respondsToSelector:@selector(rdna4AddBuffersToSet:device:)])
                            [(id<RDNA4ResidencySnapshotInternal>)snap rdna4AddBuffersToSet:resources device:dev];
                    }
                    for (id obj in _residencyHeapResources) {
                        if ([obj isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)obj device] == dev &&
                            ![obj isAliasable])
                            [resources addObject:obj];
                    }
                    if (!rdna4_buffer_registry_size(resources.count,
                            RDNA4R.lim.maxStorageBufferRange, &tableBytes)) {
                        [self rdna4Fail:@"buffer-address registry exceeds the backend buffer range or host allocation extent"];
                        return NO;
                    }
                    void *storage = malloc(tableBytes);
                    if (!storage) { [self rdna4Fail:@"cannot allocate buffer-address registry"]; return NO; }
                    BOOL valid = rdna4_buffer_registry_init(storage, tableBytes, &addresses,
                        resources.count, RDNA4R.lim.maxStorageBufferRange);
                    uint64_t row = 0;
                    for (RDNA4Buffer *buffer in resources) {
                        if (!valid) break;
                        valid = [buffer device] == dev && ![buffer isAliasable] &&
                            rdna4_buffer_registry_set(storage, tableBytes, row++,
                                [buffer gpuAddress], buffer.length, [buffer rdna4HostReadOnly]);
                        [self rdna4RetainArgument:buffer];
                    }
                    if (!valid) {
                        free(storage);
                        [self rdna4Fail:@"buffer-address registry contains an invalid resource or overflowing range"];
                        return NO;
                    }
                    registry = [NSData dataWithBytesNoCopy:storage length:tableBytes freeWhenDone:YES];
                }
                if (![self rdna4Bytes:registry ? registry.bytes : &addresses length:tableBytes buffer:&table offset:&offset]) return NO;
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){ [table vkBuffer], [table rdna4VkOffset] + offset, tableBytes };
                break;
            }
            if (p->binding >= 32 && !p->argumentRoot) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: buffer descriptor outside the [[buffer(n)]] band 0..31 (synthetic address table? not supported)", nm, p->binding]]; return NO; }
            w[i].pBufferInfo = &bi[nbi];
            NSUInteger align = p->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? RDNA4R.lim.minUniformBufferOffsetAlignment : RDNA4R.lim.minStorageBufferOffsetAlignment;
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = (p->argumentRoot ? p->embeddedBuffer : p->binding) + k; RDNA4BufSlot s = ix < 31 ? st->buf[ix] : (RDNA4BufSlot){ nil, 0 }; RDNA4Buffer *b = s.b; NSUInteger off = s.off;
                if (!b) { RDNA4LOGR("WARN %s buffer(%u) is used by the shader but was never bound: zero-filled dummy", nm, ix); b = rdna4_dummy_buffer(dev); off = 0; }
                if (!b) { [self rdna4Fail:@"no dummy buffer"]; return NO; }
                uint64_t vkoff, viewSize;
                uint64_t viewLimit = p->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? RDNA4R.lim.maxUniformBufferRange : RDNA4R.lim.maxStorageBufferRange;
                if ([b device] != dev || !rdna4_buffer_view(b.length, off, [b rdna4VkOffset], align, viewLimit, &vkoff, &viewSize)) {
                    [self rdna4Fail:[NSString stringWithFormat:@"%s buffer(%u) descriptor range or alignment is invalid", nm, ix]]; return NO;
                }
                [self rdna4RetainEncoded:b];
                bi[nbi++] = (VkDescriptorBufferInfo){ [b vkBuffer], vkoff, viewSize };
            }
            break; }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            BOOL writable = p->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            uint32_t base = writable ? 480 : 32;
            if (p->binding < base || p->binding >= base + 128) {
                [self rdna4Fail:@"texel descriptor outside the texture binding band"]; return NO;
            }
            w[i].pTexelBufferView = &texelViews[ntv];
            for (uint32_t k = 0; k < p->count; ++k) {
                uint32_t index = p->binding - base + k;
                RDNA4Texture *t = p->embedded ? rdna4_embedded_resource(p, k, st, YES, dev) :
                    (index < 128 ? st->tex[index] : nil);
                if (![t isKindOfClass:[RDNA4Texture class]] || [t device] != dev ||
                    ![t rdna4IsTexel] || ![t rdna4TexelView] || [t isAliasable] ||
                    (writable && ![t rdna4TexelWritable])) {
                    [self rdna4Fail:@"texel descriptor needs a live compatible texture buffer"]; return NO;
                }
                unsigned component = [t rdna4IsUInt] ? RDNA4NS_COMP_UINT :
                    [t rdna4IsSInt] ? RDNA4NS_COMP_SINT : RDNA4NS_COMP_FLOAT;
                if (p->texelComponentKinds != (1u << component)) {
                    [self rdna4Fail:@"texture-buffer numeric class disagrees with the shader"]; return NO;
                }
                [self rdna4RetainEncoded:t];
                texelViews[ntv++] = [t rdna4TexelView];
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            if (p->binding < 32 || p->binding >= 160) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: sampled image outside the texture band 32..159", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 32 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && p->embedded) { [self rdna4Fail:[NSString stringWithFormat:@"%s texture(%u): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, ix]]; return NO; }
                if (!t) { RDNA4LOGR("WARN %s texture(%u) is used by the shader but was never bound: 1x1 dummy", nm, ix); t = rdna4_dummy_texture(dev); }
                if (!t) { [self rdna4Fail:@"no dummy texture"]; return NO; }
                if ([t rdna4IsTexel]) { [self rdna4Fail:@"sampled image binding cannot use a texture buffer"]; return NO; }
                unsigned sampleKind = [t rdna4Samples] > 1 ? 2u : 1u;
                unsigned expectedKinds = p->imageSampleKinds ? p->imageSampleKinds : 1u;
                if (expectedKinds != sampleKind) {
                    [self rdna4Fail:[NSString stringWithFormat:
                        @"%s texture(%u): %lu samples do not match the shader image's multisample type", nm, ix, (unsigned long)[t rdna4Samples]]];
                    return NO;
                }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t vkView],
                    rdna4_use_layout(t, p->gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) };
            }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (!p->comparisonSampler && (p->binding < 160 || p->binding >= 192)) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: sampler outside the normal/comparison bands", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                VkSampler vs = p->stat;
                if (!vs) {
                    uint32_t ix = p->binding - (p->comparisonSampler ? 736 : 160) + k;
                    id s = p->embedded ? rdna4_embedded_resource(p, k, st, NO, dev) : (ix < 16 ? st->smp[ix] : nil);
                    if (!s && p->embedded) { [self rdna4Fail:[NSString stringWithFormat:@"%s sampler(%u): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, ix]]; return NO; }
                    if (!s) { RDNA4LOGR("WARN %s sampler(%u) is used by the shader but was never bound: default nearest/clamp sampler", nm, ix); s = rdna4_dummy_sampler(dev); }
                    if (!s) { [self rdna4Fail:@"no dummy sampler"]; return NO; }
                    NSError *error = nil;
                    vs = rdna4_sampler_object_handle(s, p->comparisonSampler,
                        p->normalizedCoordinates, &error);
                    if (!vs) { [self rdna4Fail:error.localizedDescription ?: @"sampler representation unavailable"]; return NO; }
                    [self rdna4RetainArgument:s];
                }
                ii[nii++] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (p->binding < 480 || p->binding >= 608) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: storage image outside the band 480..607", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 480 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t) { [self rdna4Fail:[NSString stringWithFormat:@"%s: writable texture(%u) is used by the shader but was never bound", nm, ix]]; return NO; }
                if ([t rdna4IsTexel]) { [self rdna4Fail:@"storage image binding cannot use a texture buffer"]; return NO; }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t rdna4StorageView], VK_IMAGE_LAYOUT_GENERAL };   // storage images: identity-swizzle full-layer view
            }
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:   // 11e-2: [[color(n)]] = binding 192+n = the encoder's colour attachment n, read in place (GENERAL)
            if (p->binding < 192 || p->binding >= 200) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: input attachment outside the colour band 192..199", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 192 + k; RDNA4Texture *t = ix < 8 ? st->att[ix] : nil;
                if (!t) { [self rdna4Fail:[NSString stringWithFormat:@"%s: [[color(%u)]] is read by the shader but the pass has no colour attachment %u", nm, ix, ix]]; return NO; }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t rdna4AttViewLevel:0], VK_IMAGE_LAYOUT_GENERAL };
            }
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            if (!rdna4_rt_fill_accel_descriptor(self, dev, nm, rtStage, p->binding, p->count,
                                                &st->rt, st->rtAsRefl, &w[i], &asw[i], ash, &nash, MAXI))
                return NO;
            break;
        default: [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: descriptor type %d not supported (texel buffer)", nm, p->binding, p->type]]; return NO;
        }
    }
    vkUpdateDescriptorSets(RDNA4R.dev, n, w, 0, NULL);
    return YES;
}
@end

// ---------------------------------------------------------------------------------------------------------------
// GPU Timestamp Counter Sampling (MTLCounter / MTLCounterSet / MTLCounterSampleBuffer) over VK_QUERY_TYPE_TIMESTAMP.
//
// Wave 2B GarnetDevice.m wiring notes (do NOT edit GarnetDevice.m in Wave 2A):
//   1. Include after #include "rdna4_command_buffer_impl.m" (order vs encoders is flexible):
//        #include "rdna4_counters_impl.m"
//   2. Register protocols in +[GarnetDevice load]:
//        { const char *n[] = { "MTLCounter" }; rdna4_add_protocols([RDNA4Counter class], n, 1, "RDNA4Counter"); }
//        { const char *n[] = { "MTLCounterSet" }; rdna4_add_protocols([RDNA4CounterSet class], n, 1, "RDNA4CounterSet"); }
//        { const char *n[] = { "MTLCounterSampleBuffer" }; rdna4_add_protocols([RDNA4CounterSampleBuffer class], n, 1, "RDNA4CounterSampleBuffer"); }
//   3. Wire pass descriptors in @implementation RDNA4CommandBuffer (Encoders):
//        - (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d {
//            RDNA4LOGR("cb blitCommandEncoderWithDescriptor:");
//            return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//        - (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d {
//            (void)[self vk];
//            RDNA4LOGR("cb computeCommandEncoderWithDescriptor:");
//            return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//   4. Wire MTLDevice counter selectors in @implementation GarnetDevice:
//        - (NSArray<id<MTLCounterSet>> *)counterSets { return rdna4_device_counter_sets(); }
//        - (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)samplingPoint {
//            return rdna4_device_supports_counter_sampling(samplingPoint);
//        }
//        - (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)descriptor
//                                                                     error:(NSError **)error {
//            return rdna4_device_new_counter_sample_buffer(self, descriptor, error);
//        }
//        - (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp {
//            rdna4_device_sample_timestamps(cpuTimestamp, gpuTimestamp);
//        }
//      Existing `- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }`
//      already matches RDNA4AMP_MAX_SUPPORTED_COUNT (1) and test-gate.c:188.
//   5. Note for Wave 2B / Task 8.1 test-occ.c source-pin updates:
//      `rdna4_render_encoder_impl.inc` adds 3 pass-split `[self rdna4EndPass]` call sites
//      (counter sampling + layer-shifted amplification) and `rdna4_counters_impl.inc` uses
//      `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` (without `VK_QUERY_RESULT_WAIT_BIT`).
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_COUNTERS_IMPL_INC
#define RDNA4_COUNTERS_IMPL_INC

_Static_assert(sizeof(MTLCounterResultTimestamp) == sizeof(uint64_t), "MTLCounterResultTimestamp must be 8 bytes");
_Static_assert((int)MTLCounterSamplingPointAtStageBoundary == RDNA4CNT_SP_STAGE, "stage sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtDrawBoundary == RDNA4CNT_SP_DRAW, "draw sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtDispatchBoundary == RDNA4CNT_SP_DISPATCH, "dispatch sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtTileDispatchBoundary == RDNA4CNT_SP_TILE_DISPATCH, "tile sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtBlitBoundary == RDNA4CNT_SP_BLIT, "blit sampling point enum");
_Static_assert((int)MTLStorageModeShared == RDNA4CNT_STORAGE_SHARED, "Shared storage enum");
_Static_assert((int)MTLStorageModePrivate == RDNA4CNT_STORAGE_PRIVATE, "Private storage enum");
_Static_assert(sizeof(MTLVertexAmplificationViewMapping) == sizeof(rdna4amp_mapping), "view mapping struct size");

#ifndef RDNA4_COUNTER_INTERNAL_PROTO
#define RDNA4_COUNTER_INTERNAL_PROTO
#endif

static NSError *rdna4_counter_err(MTLCounterSampleBufferError code, NSString *msg) {
    return [NSError errorWithDomain:MTLCounterErrorDomain
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: msg ?: @"counter sample buffer error" }];
}

// Always resolve Vulkan 1.0 core vkCmdWriteTimestamp (synchronization2 is not enabled on RDNA4R.dev).
static PFN_vkCmdWriteTimestamp rdna4_vk_cmd_write_timestamp(void) {
    if (!RDNA4R.ok || !RDNA4R.dev) return NULL;
    static PFN_vkCmdWriteTimestamp fn;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr)
            fn = (PFN_vkCmdWriteTimestamp)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdWriteTimestamp");
        if (!fn && rdna4_gipa && RDNA4R.inst)
            fn = (PFN_vkCmdWriteTimestamp)rdna4_gipa(RDNA4R.inst, "vkCmdWriteTimestamp");
    });
    return fn;
}

static void rdna4_counter_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}


@implementation RDNA4Counter
- (instancetype)initWithName:(NSString *)name {
    self = [super init];
    if (!self) return nil;
    _name = [name copy];
    return self;
}
RDNA4_DNR(RDNA4Counter)
- (NSString *)name { return _name; }
@end


@implementation RDNA4CounterSet
- (instancetype)initTimestampSet {
    self = [super init];
    if (!self) return nil;
    _name = [MTLCommonCounterSetTimestamp copy];
    RDNA4Counter *ts = [[RDNA4Counter alloc] initWithName:MTLCommonCounterTimestamp];
    _counters = ts ? @[ (id<MTLCounter>)ts ] : @[];
    return self;
}
RDNA4_DNR(RDNA4CounterSet)
- (NSString *)name { return _name; }
- (NSArray<id<MTLCounter>> *)counters { return _counters; }
@end


@implementation RDNA4CounterSampleBuffer
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLCounterSampleBufferDescriptor *)d
                         error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!d) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"counter sample buffer descriptor is nil");
        return nil;
    }
    if (!d.counterSet || ![d.counterSet isKindOfClass:[RDNA4CounterSet class]]) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"counterSet must be a supported timestamp counter set from this device");
        return nil;
    }
    const char *why = NULL;
    int vrc = rdna4cnt_validate_desc((uint64_t)d.sampleCount, (uint64_t)d.storageMode, &why);
    if (vrc != RDNA4CNT_DESC_OK) {
        MTLCounterSampleBufferError code = (vrc == RDNA4CNT_DESC_OOM)
            ? MTLCounterSampleBufferErrorOutOfMemory
            : MTLCounterSampleBufferErrorInvalid;
        if (err) *err = rdna4_counter_err(code, [NSString stringWithUTF8String:why ?: "invalid counter sample buffer descriptor"]);
        return nil;
    }
    NSError *openErr = nil;
    if (!rdna4_radv_open(&openErr)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal,
                                          openErr.localizedDescription ?: @"RADV open failed for counter sample buffer");
        return nil;
    }
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3) || !vkCreateQueryPool || !vkDestroyQueryPool ||
        !vkCmdResetQueryPool || !vkGetQueryPoolResults || !rdna4_vk_cmd_write_timestamp()) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal, @"GPU timestamp counter sampling is unsupported by the backend");
        return nil;
    }
    if (!rdna4cnt_pool_init(&_state, (uint32_t)d.sampleCount, (uint32_t)d.storageMode,
                            m3.timestamp_valid_bits, (double)m3.timestamp_period_ns)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorOutOfMemory, @"cannot allocate counter sample tracking state");
        return nil;
    }
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = _state.sample_count
    };
    VkResult vr = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &_qp);
    if (vr != VK_SUCCESS || _qp == VK_NULL_HANDLE) {
        MTLCounterSampleBufferError code = (vr == VK_ERROR_OUT_OF_HOST_MEMORY || vr == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            ? MTLCounterSampleBufferErrorOutOfMemory
            : MTLCounterSampleBufferErrorInternal;
        if (err) *err = rdna4_counter_err(code, [NSString stringWithFormat:@"vkCreateQueryPool(TIMESTAMP, %u) = %d", _state.sample_count, vr]);
        rdna4cnt_pool_destroy(&_state);
        return nil;
    }
    // Reset every slot once on creation so unsampled or aborted slots read as unavailable (~0ULL).
    VkQueryPool poolToReset = _qp;
    uint32_t resetCount = _state.sample_count;
    NSError *resetErr = nil;
    BOOL unresolved = NO;
    BOOL resetOk = rdna4_oneshot(&resetErr, ^(VkCommandBuffer cmd) {
        vkCmdResetQueryPool(cmd, poolToReset, 0u, resetCount);
    }, &unresolved);
    if (!resetOk) {
        if (!unresolved) vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
        _qp = VK_NULL_HANDLE;
        rdna4cnt_pool_destroy(&_state);
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal,
                                          resetErr.localizedDescription ?: @"initial timestamp query pool reset failed");
        return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _dev = dev;
    _lbl = [d.label copy];
    RDNA4LOGR("RDNA4CounterSampleBuffer %p: %u timestamp slot(s), storage %u, %.3f ns/tick (%u valid bits)",
              (__bridge void *)self, _state.sample_count, _state.storage_mode, _state.period_ns, _state.valid_bits);
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok && _qp != VK_NULL_HANDLE && vkDestroyQueryPool)
        vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
    _qp = VK_NULL_HANDLE;
    rdna4cnt_pool_destroy(&_state);
    if (_muInit) pthread_mutex_destroy(&_mu);
}

RDNA4_DNR(RDNA4CounterSampleBuffer)
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (NSUInteger)sampleCount { return (NSUInteger)_state.sample_count; }
- (MTLStorageMode)rdna4StorageMode { return (MTLStorageMode)_state.storage_mode; }

- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb
                   vkCommandBuffer:(VkCommandBuffer)cmd
                       sampleIndex:(NSUInteger)sampleIndex
                           barrier:(BOOL)barrier
                        stageFlags:(VkPipelineStageFlagBits)stage
                               who:(const char *)who {
    if (!cb || cmd == VK_NULL_HANDLE || _qp == VK_NULL_HANDLE) return NO;
    if (!rdna4cnt_index_ok((uint64_t)_state.sample_count, (uint64_t)sampleIndex)) {
        [cb rdna4Fail:[NSString stringWithFormat:@"%s sampleCountersInBuffer: sampleIndex %lu out of range (sampleCount %u)",
                       who ?: "encoder", (unsigned long)sampleIndex, _state.sample_count]];
        return NO;
    }
    PFN_vkCmdWriteTimestamp writeTs = rdna4_vk_cmd_write_timestamp();
    if (!writeTs || !vkCmdResetQueryPool) {
        [cb rdna4Fail:[NSString stringWithFormat:@"%s sampleCountersInBuffer: vkCmdWriteTimestamp unavailable", who ?: "encoder"]];
        return NO;
    }
    [cb rdna4RetainEncoded:self];
    if (barrier) rdna4_counter_barrier(cmd);
    // Caller must be outside any active Vulkan render pass before resetting/sampling.
    vkCmdResetQueryPool(cmd, _qp, (uint32_t)sampleIndex, 1u);
    writeTs(cmd, stage, _qp, (uint32_t)sampleIndex);
    pthread_mutex_lock(&_mu);
    rdna4cnt_slot_mark_sampled(&_state, (uint32_t)sampleIndex);
    pthread_mutex_unlock(&_mu);
    return YES;
}

- (BOOL)rdna4ResolveRange:(NSRange)range
                intoBytes:(uint64_t *)dst
                    error:(NSError **)err {
    if (!dst || !rdna4cnt_range_ok((uint64_t)_state.sample_count, (uint64_t)range.location, (uint64_t)range.length)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"resolveCounterRange: range out of bounds");
        return NO;
    }
    for (NSUInteger i = 0; i < range.length; i++) dst[i] = RDNA4CNT_ERROR_VALUE;
    if (!RDNA4R.ok || _qp == VK_NULL_HANDLE || !vkGetQueryPoolResults || atomic_load(&rdna4_submit_fault) != VK_SUCCESS)
        return YES;

    uint8_t *sampledSnap = (uint8_t *)calloc(range.length, 1u);
    if (!sampledSnap) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorOutOfMemory, @"out of memory resolving counter range");
        return NO;
    }
    pthread_mutex_lock(&_mu);
    for (NSUInteger i = 0; i < range.length; i++)
        sampledSnap[i] = (uint8_t)rdna4cnt_slot_is_sampled(&_state, (uint32_t)(range.location + i));
    pthread_mutex_unlock(&_mu);

    NSUInteger pos = 0;
    while (pos < range.length) {
        if (!sampledSnap[pos]) { pos++; continue; }
        NSUInteger runStart = pos;
        while (pos < range.length && sampledSnap[pos] && (pos - runStart) < 256u) pos++;
        uint32_t chunk = (uint32_t)(pos - runStart);
        uint64_t raw[2u * 256u];
        memset(raw, 0, sizeof(uint64_t) * 2u * chunk);
        VkResult vr = vkGetQueryPoolResults(
            RDNA4R.dev, _qp, (uint32_t)(range.location + runStart), chunk,
            sizeof(uint64_t) * 2u * chunk, raw, 2u * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (vr == VK_SUCCESS || vr == VK_NOT_READY) {
            rdna4cnt_resolve_pairs(raw, chunk, _state.valid_bits, _state.period_ns, dst + runStart);
        }
    }
    free(sampledSnap);
    return YES;
}

- (NSData *)resolveCounterRange:(NSRange)range {
    if (!rdna4cnt_resolve_cpu_allowed(_state.storage_mode)) {
        RDNA4LOG("resolveCounterRange: refused on non-Shared counter sample buffer (storageMode %u)", _state.storage_mode);
        return nil;
    }
    if (!rdna4cnt_range_ok((uint64_t)_state.sample_count, (uint64_t)range.location, (uint64_t)range.length)) {
        RDNA4LOG("resolveCounterRange: invalid range {%lu, %lu} for sampleCount %u",
                 (unsigned long)range.location, (unsigned long)range.length, _state.sample_count);
        return nil;
    }
    NSMutableData *out = [NSMutableData dataWithLength:range.length * sizeof(MTLCounterResultTimestamp)];
    if (!out) return nil;
    if (![self rdna4ResolveRange:range intoBytes:(uint64_t *)out.mutableBytes error:NULL]) return nil;
    return out;
}
@end

GARNET_INTERNAL NSArray<id<MTLCounterSet>> *rdna4_device_counter_sets(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3)) return @[];
    static NSArray<id<MTLCounterSet>> *sets;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        RDNA4CounterSet *ts = [[RDNA4CounterSet alloc] initTimestampSet];
        sets = ts ? @[ (id<MTLCounterSet>)ts ] : @[];
    });
    return sets;
}

GARNET_INTERNAL BOOL rdna4_device_supports_counter_sampling(MTLCounterSamplingPoint point) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4cnt_supports_sampling_point(&m3, (uint64_t)point) ? YES : NO;
}

GARNET_INTERNAL id<MTLCounterSampleBuffer> rdna4_device_new_counter_sample_buffer(
    id dev, MTLCounterSampleBufferDescriptor *descriptor, NSError **error) {
    return (id<MTLCounterSampleBuffer>)[[RDNA4CounterSampleBuffer alloc]
        initWithDevice:dev descriptor:descriptor error:error];
}

// Device-level calibrated CPU/GPU timestamp pair: never opens RADV unexpectedly and never fakes GPU time.
GARNET_INTERNAL void rdna4_device_sample_timestamps(MTLTimestamp *cpuTimestamp, MTLTimestamp *gpuTimestamp) {
    if (gpuTimestamp) *gpuTimestamp = 0;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    PFN_vkCmdWriteTimestamp writeTs = rdna4_vk_cmd_write_timestamp();
    if (!gpuTimestamp || !RDNA4R.ok || !rdna4dc_metal3_timestamps_supported(&m3) ||
        !writeTs || !vkCreateQueryPool || !vkCmdResetQueryPool || !vkGetQueryPoolResults ||
        atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        if (cpuTimestamp) *cpuTimestamp = (MTLTimestamp)rdna4_now();
        return;
    }
    static pthread_mutex_t tsMu = PTHREAD_MUTEX_INITIALIZER;
    static VkQueryPool tsPool = VK_NULL_HANDLE;
    pthread_mutex_lock(&tsMu);
    if (tsPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 1u
        };
        if (vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &tsPool) != VK_SUCCESS) tsPool = VK_NULL_HANDLE;
    }
    VkQueryPool qp = tsPool;
    uint64_t t0 = rdna4_now();
    uint64_t gpuNs = 0;
    if (qp != VK_NULL_HANDLE) {
        BOOL unresolved = NO;
        BOOL ok = rdna4_oneshot(NULL, ^(VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, qp, 0u, 1u);
            writeTs(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0u);
        }, &unresolved);
        if (ok) {
            uint64_t raw[2] = { 0u, 0u };
            VkResult vr = vkGetQueryPoolResults(
                RDNA4R.dev, qp, 0u, 1u, sizeof raw, raw, sizeof raw,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if ((vr == VK_SUCCESS || vr == VK_NOT_READY) && raw[1] != 0u) {
                uint64_t ns = rdna4cnt_ticks_to_ns(raw[0], m3.timestamp_valid_bits, (double)m3.timestamp_period_ns);
                if (ns != RDNA4CNT_ERROR_VALUE) gpuNs = ns;
            }
        }
    }
    uint64_t t1 = rdna4_now();
    pthread_mutex_unlock(&tsMu);
    if (cpuTimestamp) *cpuTimestamp = (MTLTimestamp)(t0 + (t1 >= t0 ? (t1 - t0) / 2u : 0u));
    *gpuTimestamp = (MTLTimestamp)gpuNs;
}
#endif
// ---------------------------------------------------------------------------------------------------------------
// RDNA4RenderEncoder / RDNA4ComputeEncoder / RDNA4BlitEncoder (10c, 10d, 11e)
// ---------------------------------------------------------------------------------------------------------------

#ifndef RDNA4_COUNTER_INTERNAL_PROTO
#define RDNA4_COUNTER_INTERNAL_PROTO
#endif

static id<RDNA4CounterSampleBufferInternal> rdna4_counter_buffer_check(
    id sampleBuffer, id expectedDevice, NSUInteger sampleIndex,
    BOOL checkIndex, NSString **whyOut) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3)) {
        if (whyOut) *whyOut = @"GPU timestamp counters are unsupported on this device";
        return nil;
    }
    Class cls = objc_getClass("RDNA4CounterSampleBuffer");
    if (!cls || !sampleBuffer || ![sampleBuffer isKindOfClass:cls]) {
        if (whyOut) *whyOut = @"invalid counter sample buffer";
        return nil;
    }
    id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)sampleBuffer;
    if ([csb device] != expectedDevice) {
        if (whyOut) *whyOut = @"counter sample buffer belongs to another device";
        return nil;
    }
    if (checkIndex && !rdna4cnt_index_ok((uint64_t)[csb sampleCount], (uint64_t)sampleIndex)) {
        if (whyOut) *whyOut = [NSString stringWithFormat:@"sampleIndex %lu out of range (sampleCount %lu)",
                               (unsigned long)sampleIndex, (unsigned long)[csb sampleCount]];
        return nil;
    }
    return csb;
}

static BOOL rdna4_counter_pass_index_valid(id<RDNA4CounterSampleBufferInternal> csb, NSUInteger index) {
    return index == MTLCounterDontSample ||
           rdna4cnt_index_ok((uint64_t)[csb sampleCount], (uint64_t)index);
}

GARNET_INTERNAL void rdna4_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
GARNET_INTERNAL VkPrimitiveTopology rdna4_topo(MTLPrimitiveType t) {
    switch (t) { case MTLPrimitiveTypePoint: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case MTLPrimitiveTypeLine: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case MTLPrimitiveTypeLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case MTLPrimitiveTypeTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}
// Without ordered attachment access, the encoder emits one primitive per draw
// and uses the self-dependency to make each primitive's writes visible.
GARNET_INTERNAL VkRenderPass rdna4_mk_rp(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch) {
    VkAttachmentReference refs[8], ins[8], dref = { na, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };   // bundle 10: nd = 1: ads[na] is the depth/stencil attachment
    for (uint32_t i = 0; i < na; i++) { refs[i] = (VkAttachmentReference){ i, fetch ? VK_IMAGE_LAYOUT_GENERAL : ads[i].finalLayout }; ins[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL }; }
    VkSubpassDescription sp = { .flags = fetch && RDNA4R.orderedColorAccess ? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT : 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = na, .pColorAttachments = refs,
        .inputAttachmentCount = fetch ? na : 0, .pInputAttachments = fetch ? ins : NULL, .pDepthStencilAttachment = nd ? &dref : NULL };
    VkSubpassDependency deps[3] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT } };
    // deps[0] = external in, deps[1] = external out, deps[2] = the fetch self-dependency (only declared for a fetch pass)
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na + nd, .pAttachments = ads, .subpassCount = 1,
        .pSubpasses = &sp, .dependencyCount = fetch ? 3 : 2, .pDependencies = deps };
    VkRenderPass rp = VK_NULL_HANDLE; VkResult r = vkCreateRenderPass(RDNA4R.dev, &rpc, NULL, &rp);
    if (r != VK_SUCCESS) { RDNA4LOG("render pass: vkCreateRenderPass = %d", r); return VK_NULL_HANDLE; }
    return rp;
}
// P5b: cached passes belong to the device; uncached passes belong to the caller.
static VkRenderPass rdna4_mk_rp_c(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch, BOOL *cached) {
    *cached = NO;
    if (!RDNA4DO.on || na + nd > 8) return rdna4_mk_rp(ads, na, nd, fetch);   // bundle 10: the key counts every attachment; 8 colour + depth is not cached
    rdna4dc_rpkey k; memset(&k, 0, sizeof k); k.na = na + nd; k.fetch = (fetch ? 1u : 0u) | (nd << 1);   // nd == 0: exactly the old key
    for (uint32_t i = 0; i < na + nd; i++)
        k.a[i] = (rdna4dc_att){ (uint32_t)ads[i].format, (uint32_t)ads[i].samples, (uint32_t)ads[i].loadOp, (uint32_t)ads[i].storeOp, (uint32_t)ads[i].stencilLoadOp,
                              (uint32_t)ads[i].stencilStoreOp, (uint32_t)ads[i].initialLayout, (uint32_t)ads[i].finalLayout };
    uint64_t h;
    pthread_mutex_lock(&RDNA4DO.mu); h = rdna4dc_rp_find(RDNA4DO.rp, &k); pthread_mutex_unlock(&RDNA4DO.mu);
    if (h) { atomic_fetch_add(&RDNA4DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    VkRenderPass rp = rdna4_mk_rp(ads, na, nd, fetch); if (!rp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&RDNA4DO.mu);
    h = rdna4dc_rp_find(RDNA4DO.rp, &k);   // another thread may have entered the same key meanwhile
    if (!h && rdna4dc_rp_add(RDNA4DO.rp, &k, (uint64_t)(uintptr_t)rp)) { pthread_mutex_unlock(&RDNA4DO.mu); atomic_fetch_add(&RDNA4DO.rpCreated, 1); *cached = YES; return rp; }
    pthread_mutex_unlock(&RDNA4DO.mu);
    if (h) { vkDestroyRenderPass(RDNA4R.dev, rp, NULL); atomic_fetch_add(&RDNA4DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    atomic_fetch_add(&RDNA4DO.rpUncached, 1);
    return rp;
}

// The encoder owns reusable handles while recording; each consuming segment
// shares ownership until its fence retires. Cached render passes stay device-owned.

@implementation RDNA4RenderHandles
- (instancetype)initWithRenderPass:(VkRenderPass)renderPass
                       framebuffer:(VkFramebuffer)framebuffer {
    self = [super init];
    if (!self) return nil;
    _renderPass = renderPass;
    _framebuffer = framebuffer;
    return self;
}
- (void)dealloc {
    if (!RDNA4R.ok) return;
    if (_framebuffer) vkDestroyFramebuffer(RDNA4R.dev, _framebuffer, NULL);
    if (_renderPass) vkDestroyRenderPass(RDNA4R.dev, _renderPass, NULL);
}
@end


@implementation RDNA4RenderEncoder
- (BOOL)rdna4CheckDSResolve:(BOOL)isDepth {
    if (!_nd) return NO;
    RDNA4Texture *src = _tex[_na]; id r = isDepth ? _drtex : _srtex;
    uint32_t rlvl = isDepth ? _drlvl : _srlvl, rslice = isDepth ? _drslice : _srslice;
    NSUInteger filter = isDepth ? _dresFilter : _sresFilter;
    if (!RDNA4R.ok || !vkCreateRenderPass2 || !vkCmdBeginRenderPass2 || !vkCmdEndRenderPass2) {
        RDNA4LOG("render pass: depth/stencil resolve requires Vulkan 1.2 renderPass2; refused"); return NO;
    }
    if (![r isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: %s resolve texture is not an RDNA4Texture; refused", isDepth ? "depth" : "stencil"); return NO; }
    RDNA4Texture *dst = r;
    if (dst == src || [dst rdna4IsView] || [dst textureType] == MTLTextureType3D) {
        RDNA4LOG("render pass: %s resolve texture is self, a view or a 3D texture; refused", isDepth ? "depth" : "stencil"); return NO;
    }
    if (rlvl >= [dst mipmapLevelCount] || rslice + _layers > [dst rdna4Layers]) {
        RDNA4LOG("render pass: %s resolve level %u slice %u+%u outside resolve texture; refused", isDepth ? "depth" : "stencil", rlvl, rslice, _layers); return NO;
    }
    unsigned sw = (unsigned)MAX((NSUInteger)1, src.width >> _dlvl), sh = (unsigned)MAX((NSUInteger)1, src.height >> _dlvl);
    unsigned dw = (unsigned)MAX((NSUInteger)1, dst.width >> rlvl), dh = (unsigned)MAX((NSUInteger)1, dst.height >> rlvl);
    VkFormat svkf = [src vkFormat], dvkf = [dst vkFormat];
    BOOL compatFmt = (svkf == dvkf) || (svkf == VK_FORMAT_D32_SFLOAT_S8_UINT && ((isDepth && dvkf == VK_FORMAT_D32_SFLOAT) || (!isDepth && dvkf == VK_FORMAT_S8_UINT)));
    const char *why = NULL;
    if (!rdna4sa_ds_target_ok(sw, sh, (unsigned)svkf, dw, dh, compatFmt ? (unsigned)svkf : (unsigned)dvkf, (unsigned)[dst rdna4Samples], &why)) {
        RDNA4LOG("render pass: %s resolve: %s; refused", isDepth ? "depth" : "stencil", why); return NO;
    }
    if (isDepth) {
        VkResolveModeFlagBits rm = filter == 1 ? VK_RESOLVE_MODE_MIN_BIT : filter == 2 ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (filter > 2 || !(RDNA4R.depthResolveModes & rm)) {
            RDNA4LOG("render pass: depthResolveFilter %lu is not supported on RADV (modes 0x%x); refused", (unsigned long)filter, RDNA4R.depthResolveModes); return NO;
        }
    } else {
        if (filter != 0) {
            RDNA4LOG("render pass: stencilResolveFilter %lu (DepthResolvedSample) is not supported on Vulkan; refused", (unsigned long)filter); return NO;
        }
        if (!(RDNA4R.stencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) {
            RDNA4LOG("render pass: stencil Sample0 resolve is not supported on RADV (modes 0x%x); refused", RDNA4R.stencilResolveModes); return NO;
        }
    }
    return YES;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d {
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb; _wind = MTLWindingClockwise; _tessFactorScale = 1.0f;
    _fillMode = MTLTriangleFillModeFill; _depthClipMode = MTLDepthClipModeClip;
    _ampCount = 1u; _ampHasMappings = NO; _ampLayerOff = 0u; _activeLayerOff = 0u;
    _resourceBindings = [RDNA4ResourceBindings new];
    _resourceSegmentSerial = [_cb rdna4SegmentSerial];
    NSUInteger reqLayers = [d respondsToSelector:@selector(renderTargetArrayLength)] ? d.renderTargetArrayLength : 0;
    if (reqLayers && (!RDNA4R.ok || reqLayers > RDNA4R.lim.maxFramebufferLayers)) {
        RDNA4LOG("render pass: renderTargetArrayLength %lu exceeds RADV maxFramebufferLayers %u; refused", (unsigned long)reqLayers, RDNA4R.lim.maxFramebufferLayers);
        return nil;
    }
    _layers = reqLayers ? (uint32_t)reqLayers : 1u;
    // Keep attachment indices and render-pass compatibility intact: an unsafe pass records nothing.
    for (NSUInteger i = 0; i < 8; i++) {
        id texture = d.colorAttachments[i].texture;
        if ([texture isKindOfClass:[RDNA4Texture class]] &&
            rdna4nc_drop_write(texture, RDNA4NC_ATTACHMENT, "render attachment store", "RenderCommandEncoder")) {
            _hostWriteDropped = YES;
            [_cb rdna4CBTraceSurface:texture use:"attachment-refused"];
        }
    }
    id depth = d.depthAttachment.texture, stencil = d.stencilAttachment.texture;
    if ([depth isKindOfClass:[RDNA4Texture class]])
        _hostWriteDropped |= rdna4nc_drop_write(depth, RDNA4NC_ATTACHMENT, "depth attachment store", "RenderCommandEncoder");
    if (stencil != depth && [stencil isKindOfClass:[RDNA4Texture class]])
        _hostWriteDropped |= rdna4nc_drop_write(stencil, RDNA4NC_ATTACHMENT, "stencil attachment store", "RenderCommandEncoder");
    if (_hostWriteDropped) { [_cb rdna4Fail:@"render attachment writes a read-only no-copy texture"]; return self; }
    _dcOn = RDNA4DO.on; if (_dcOn) rdna4dc_enc_init(&_dc);
    VkImageView views[9]; uint32_t na = 0; _dsi = rdna4_dsinfo_default();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        RDNA4Texture *t = (RDNA4Texture *)ca.texture;
        if (!t) continue;
        if (![t isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: attachment %lu is not an RDNA4Texture", (unsigned long)i); return nil; }
        NSUInteger tsm = [t rdna4Samples];   // bundle 10: every attachment of a pass has the same sample count
        if (_encSamples && tsm != _encSamples) { RDNA4LOG("render pass: attachment %lu has %lu samples, the others %u; refused", (unsigned long)i, (unsigned long)tsm, _encSamples); return nil; }
        _encSamples = (unsigned)tsm;
        // m11h9: a mip level of a 2D texture can be the attachment (the compositor renders into the base level of mipmapped textures and generates the rest)
        NSUInteger lvl = ca.level;
        if (lvl >= [t mipmapLevelCount] || ca.slice >= [t rdna4Layers] || _layers > [t rdna4Layers] - ca.slice || ca.depthPlane != 0) { RDNA4LOG("render pass: attachment %lu level %lu slice %lu+%u depthPlane %lu outside the texture (%lu level(s), %u slice(s)); refused", (unsigned long)i, (unsigned long)lvl, (unsigned long)ca.slice, _layers, (unsigned long)ca.depthPlane, (unsigned long)[t mipmapLevelCount], [t rdna4Layers]); return nil; }
        const NSUInteger attachmentWidth = MAX((NSUInteger)1, t.width >> lvl);
        const NSUInteger attachmentHeight = MAX((NSUInteger)1, t.height >> lvl);
        if (na == 0) { _w = (uint32_t)attachmentWidth; _h = (uint32_t)attachmentHeight; }
        const BOOL preserveOutsideClear = ca.loadAction == MTLLoadActionClear && [t rdna4IsIOS] &&
            (_w < attachmentWidth || _h < attachmentHeight);
        rdna4_ios_use(_cb, t, ca.loadAction != MTLLoadActionClear || preserveOutsideClear, YES);
        [_cb rdna4DispNote:t bits:(RDNA4DF_W_PASS | (ca.loadAction == MTLLoadActionClear ? RDNA4DF_W_CLEAR : 0)) pso:nil];   // native #12 P1
        BOOL load = (ca.loadAction == MTLLoadActionLoad || ([t rdna4IsIOS] && ca.loadAction != MTLLoadActionClear)) && [t layout] != VK_IMAGE_LAYOUT_UNDEFINED;
        // The layout of a texture is tracked for the WHOLE image. A render pass only transitions its attachment's level, so a multi-level texture is moved to the attachment layout
        // explicitly (outside any pass: the previous encoder has ended) and the pass then starts from that layout.
        BOOL wholeImg = [t rdna4Levels] > 1 || [t rdna4ImageLayers] > 1;   // bundle 9: a layered texture is moved as a whole too (the pass only transitions its one layer)
        VkImageLayout attachmentLayout = rdna4_use_layout(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (wholeImg) rdna4_tex_to([_cb vk], t, attachmentLayout);
        // storeOp is always STORE: a pass may be split (a texture bound mid-encoder needs a layout change outside the pass) and the
        // continuation LOADs what the first part stored.
        _ads[na] = (VkAttachmentDescription){ .format = [t vkFormat], .samples = (VkSampleCountFlagBits)tsm,
            .loadOp = ca.loadAction == MTLLoadActionClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = wholeImg || attachmentLayout == VK_IMAGE_LAYOUT_GENERAL ? attachmentLayout :
                (load || preserveOutsideClear ? [t layout] : VK_IMAGE_LAYOUT_UNDEFINED),
            .finalLayout = attachmentLayout };
        if (ca.loadAction == MTLLoadActionClear) _clrm |= 1u << na;
        views[na] = _layers > 1
            ? [t rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice layerCount:_layers]
            : [t rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice];
        _views[na] = views[na]; _tex[na] = t; _sf.att[na] = t;
        [self rdna4OwnResource:t kind:RDNA4_RESOURCE_ATTACHMENT
                      fragment:NO index:na];
        MTLClearColor c = ca.clearColor;
        if ([t rdna4IsUInt]) {   // bundle 19: an integer attachment is cleared through .uint32 with Metal's conversion (truncate, saturate; measured on an Apple-silicon Mac, garnet_intfmt.h); .float32 would put float bits in the texture
            unsigned rgbB = 16, aB = 16; [t rdna4IntBitsRGB:&rgbB alpha:&aB];
            _cvs[na] = (VkClearValue){ .color = { .uint32 = { rdna4if_clear_u(c.red, rgbB), rdna4if_clear_u(c.green, rgbB), rdna4if_clear_u(c.blue, rgbB), rdna4if_clear_u(c.alpha, aB) } } };
        } else if ([t rdna4IsSInt]) {
            unsigned rgbB = 32, aB = 32; [t rdna4IntBitsRGB:&rgbB alpha:&aB];
            _cvs[na] = (VkClearValue){ .color = { .int32 = { rdna4if_clear_s(c.red, rgbB), rdna4if_clear_s(c.green, rgbB), rdna4if_clear_s(c.blue, rgbB), rdna4if_clear_s(c.alpha, aB) } } };
        } else
        _cvs[na] = (VkClearValue){ .color = { .float32 = { (float)c.red, (float)c.green, (float)c.blue, (float)c.alpha } } };
        [t setLayout:attachmentLayout];
        // bundle 10: store action / multisample resolve.  The attachment itself is always stored; a resolve texture receives vkCmdResolveImage at endEncoding (a pass may be split, so never inside it).
        _slvl[na] = (uint32_t)lvl; _sslice[na] = (uint32_t)ca.slice; _sact[na] = (NSUInteger)ca.storeAction; _rtex[na] = ca.resolveTexture; _rlvl[na] = (uint32_t)ca.resolveLevel; _rslice[na] = (uint32_t)ca.resolveSlice;
        if (ca.resolveTexture)
            [self rdna4OwnResource:ca.resolveTexture
                              kind:RDNA4_RESOURCE_RESOLVE
                          fragment:NO index:na];
        { rdna4sa_t sat; if (!rdna4sa_map((unsigned long)ca.storeAction, ca.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: attachment %lu: %s; refused", (unsigned long)i, sat.why); return nil; }
          if (sat.resolve && ![self rdna4CheckResolve:na]) return nil; }
        na++;
    }
    if (d.depthAttachment.texture || d.stencilAttachment.texture) {   // bundle 10: the depth / stencil attachment (one Vulkan attachment: one texture, depth and/or stencil aspect)
        id dtI = d.depthAttachment.texture, stI = d.stencilAttachment.texture;
        MTLRenderPassDepthAttachmentDescriptor *da = d.depthAttachment; MTLRenderPassStencilAttachmentDescriptor *sa = d.stencilAttachment;
        if (dtI && stI && dtI != stI) { RDNA4LOG("render pass: separate depth and stencil textures are not supported (use one Depth32Float_Stencil8 texture); refused"); return nil; }
        id ti = dtI ? dtI : stI;
        if (![ti isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: the depth/stencil attachment is not an RDNA4Texture"); return nil; }
        RDNA4Texture *dt = ti; unsigned asp = [dt rdna4Aspects];
        if (![dt rdna4IsDS]) { RDNA4LOG("render pass: the depth/stencil attachment texture has a colour pixel format; refused"); return nil; }
        if ((dtI && !(asp & RDNA4DP_ASP_DEPTH)) || (stI && !(asp & RDNA4DP_ASP_STENCIL))) { RDNA4LOG("render pass: the %s attachment texture (pixel format %lu) has no %s aspect; refused", dtI ? "depth" : "stencil", (unsigned long)dt.pixelFormat, dtI ? "depth" : "stencil"); return nil; }
        if ([dt rdna4IsView] && ([dt pixelFormat] == MTLPixelFormatX32_Stencil8 || asp != rdna4dp_view_aspect(asp))) { RDNA4LOG("render pass: a view of a combined depth/stencil texture cannot be an attachment; refused"); return nil; }
        if (dtI && stI && (da.level != sa.level || da.slice != sa.slice)) { RDNA4LOG("render pass: the depth and stencil attachments name different levels / slices; refused"); return nil; }
        NSUInteger lvl = dtI ? da.level : sa.level, slc = dtI ? da.slice : sa.slice, dpl = dtI ? da.depthPlane : sa.depthPlane;
        if (lvl >= [dt mipmapLevelCount] || slc >= [dt rdna4Layers] || _layers > [dt rdna4Layers] - slc || dpl != 0) { RDNA4LOG("render pass: the depth/stencil attachment level %lu slice %lu+%u depthPlane %lu is outside the texture; refused", (unsigned long)lvl, (unsigned long)slc, _layers, (unsigned long)dpl); return nil; }
        NSUInteger tsm = [dt rdna4Samples];
        if (_encSamples && tsm != _encSamples) { RDNA4LOG("render pass: the depth/stencil attachment has %lu samples, the colour attachments %u; refused", (unsigned long)tsm, _encSamples); return nil; }
        _encSamples = (unsigned)tsm;
        uint32_t dw = (uint32_t)MAX((NSUInteger)1, dt.width >> lvl), dh = (uint32_t)MAX((NSUInteger)1, dt.height >> lvl);
        if (na == 0) { _w = dw; _h = dh; }
        else if (dw < _w || dh < _h) { RDNA4LOG("render pass: the depth/stencil attachment (%ux%u) is smaller than the colour attachments (%ux%u); refused", dw, dh, _w, _h); return nil; }
        rdna4rp_ds_t ops; rdna4rp_ds(asp, dtI != nil, (unsigned)da.loadAction, stI != nil, (unsigned)sa.loadAction, [dt layout] == VK_IMAGE_LAYOUT_UNDEFINED, &ops);
        BOOL wholeImg = [dt rdna4Levels] > 1 || [dt rdna4ImageLayers] > 1;
        if (wholeImg) rdna4_tex_to([_cb vk], dt, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        _ads[na] = (VkAttachmentDescription){ .format = [dt vkFormat], .samples = (VkSampleCountFlagBits)tsm,
            .loadOp = (VkAttachmentLoadOp)ops.depthLoad, .storeOp = (VkAttachmentStoreOp)ops.depthStore, .stencilLoadOp = (VkAttachmentLoadOp)ops.stencilLoad, .stencilStoreOp = (VkAttachmentStoreOp)ops.stencilStore,
            .initialLayout = wholeImg ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : ops.undefinedInitial ? VK_IMAGE_LAYOUT_UNDEFINED : [dt layout], .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        _cvs[na] = (VkClearValue){ .depthStencil = { (float)da.clearDepth, (uint32_t)sa.clearStencil } };
        views[na] = _layers > 1
            ? [dt rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)slc layerCount:_layers]
            : [dt rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)slc];
        _views[na] = views[na]; _tex[na] = dt; _dsVkFmt = [dt vkFormat]; _nd = 1;
        _dlvl = (uint32_t)lvl; _dslice = (uint32_t)slc;
        if (dtI) {
            _dsact = (NSUInteger)da.storeAction; _drtex = da.resolveTexture; _drlvl = (uint32_t)da.resolveLevel; _drslice = (uint32_t)da.resolveSlice;
            _dresFilter = [da respondsToSelector:@selector(depthResolveFilter)] ? (NSUInteger)da.depthResolveFilter : 0;
            if (da.resolveTexture) [self rdna4OwnResource:da.resolveTexture kind:RDNA4_RESOURCE_RESOLVE fragment:NO index:8];
        }
        if (stI) {
            _ssact = (NSUInteger)sa.storeAction; _srtex = sa.resolveTexture; _srlvl = (uint32_t)sa.resolveLevel; _srslice = (uint32_t)sa.resolveSlice;
            _sresFilter = [sa respondsToSelector:@selector(stencilResolveFilter)] ? (NSUInteger)sa.stencilResolveFilter : 0;
            if (sa.resolveTexture) [self rdna4OwnResource:sa.resolveTexture kind:RDNA4_RESOURCE_RESOLVE fragment:NO index:9];
        }
        { rdna4sa_t sat;
          if (dtI && !rdna4sa_map((unsigned long)da.storeAction, da.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: depth attachment: %s; refused", sat.why); return nil; }
          if (dtI && sat.resolve && ![self rdna4CheckDSResolve:YES]) return nil;
          if (stI && !rdna4sa_map((unsigned long)sa.storeAction, sa.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: stencil attachment: %s; refused", sat.why); return nil; }
          if (stI && sat.resolve && ![self rdna4CheckDSResolve:NO]) return nil; }
        [self rdna4OwnResource:dt kind:RDNA4_RESOURCE_ATTACHMENT
                      fragment:NO index:na];
        [dt setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
        RDNA4LOGR("render pass: depth/stencil attachment %p pixel format %lu (vk %d) aspects 0x%x level %lu slice %lu, %lu sample(s), depth load %u store %u, stencil load %u store %u", (__bridge void *)dt, (unsigned long)dt.pixelFormat, [dt vkFormat], asp,
                (unsigned long)lvl, (unsigned long)slc, (unsigned long)tsm, ops.depthLoad, ops.depthStore, ops.stencilLoad, ops.stencilStore);
    }
    if (!_encSamples) _encSamples = 1;
    if (!na && !_nd) { RDNA4LOG("render pass: no colour or depth/stencil attachments"); return nil; }
    if ([d respondsToSelector:@selector(getSamplePositions:count:)]) {
        NSUInteger spc = [d getSamplePositions:NULL count:0];
        if (spc > 0) {
            if (!RDNA4R.sampleLocationsExt || spc != _encSamples || spc > 8 || !(RDNA4R.sampleLocationCounts & _encSamples)) {
                RDNA4LOG("render pass: programmable sample positions count %lu for %u samples unsupported (ext %d counts 0x%x); refused",
                         (unsigned long)spc, _encSamples, RDNA4R.sampleLocationsExt, RDNA4R.sampleLocationCounts);
                return nil;
            }
            [d getSamplePositions:_samplePos count:spc];
            _samplePosCount = (uint32_t)spc;
            _hasSamplePos = YES;
        }
    }
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLRenderPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLRenderPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfVertexSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfVertexSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.startOfFragmentSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfFragmentSampleIndex)) {
                RDNA4LOG("render pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                         (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
    }
    _na = na;
    for (uint32_t i = 0; i < na + _nd; i++) _curLay[i] = _ads[i].initialLayout;
    _rp1 = rdna4_mk_rp_c(_ads, na, _nd, NO, &_rp1c); if (!_rp1) return nil;
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = _rp1, .attachmentCount = na + _nd, .pAttachments = views, .width = _w, .height = _h, .layers = _layers };
    VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &_fb);
    if (r != VK_SUCCESS) { RDNA4LOG("render pass: vkCreateFramebuffer = %d", r); if (!_rp1c) vkDestroyRenderPass(RDNA4R.dev, _rp1, NULL); return nil; }
    _baseHandles = [[RDNA4RenderHandles alloc]
        initWithRenderPass:_rp1c ? VK_NULL_HANDLE : _rp1 framebuffer:_fb];
    // Metal's default viewport = the attachment, y down; translated shaders do not flip clip Y (F6): the viewport is emitted with a negative height.
    _vp = (MTLViewport){ 0, 0, _w, _h, 0, 1 }; _sr = (MTLScissorRect){ 0, 0, _w, _h };
    _vps[0] = _vp; _nvp = 1; _srs[0] = _sr; _nsr = 1;
    RDNA4LOGR("RDNA4RenderEncoder %p: %u x %u x %u, %u attachment(s); the Vulkan render pass begins at the first draw (or at endEncoding)", (__bridge void *)self, _w, _h, _layers, na);
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLRenderPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLRenderPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfVertexSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfVertexSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"render-pass-start-vertex"];
            if (att.startOfFragmentSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfFragmentSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_VERTEX_SHADER_BIT who:"render-pass-start-fragment"];
            if ((att.endOfVertexSampleIndex != MTLCounterDontSample ||
                 att.endOfFragmentSampleIndex != MTLCounterDontSample) &&
                _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndV[_nCsbEnd] = att.endOfVertexSampleIndex;
                _csbEndF[_nCsbEnd] = att.endOfFragmentSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    [self rdna4OccInit:d];   // build 18 (P2): outside any render pass (the previous encoder has ended), so the query reset is legal
    return self;
}
RDNA4_DNR(RDNA4RenderEncoder)
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index {
    [_resourceBindings setObject:object
                             key:rdna4_resource_slot_key(kind, fragment, index)];
}
- (void)rdna4SyncResourceSegment {
    uint64_t serial = [_cb rdna4SegmentSerial];
    if (!rdna4_encoder_take_segment_change(&_resourceSegmentSerial, serial))
        return;
    if (_dcOn) rdna4dc_enc_reset_bound(&_dc);
}
- (void)rdna4SnapshotResources {
    [_resourceBindings snapshotToCommandBuffer:_cb];
    const uint64_t cbid = [_cb rdna4CBId], segSer = [_cb rdna4SegmentSerial];
    if (cbid && segSer && cbid == _handlesSnapCBId && segSer == _handlesSnapSegSer &&
        _snapBaseHandles == _baseHandles && _snapFetchHandles == _fetchHandles)
        return;
    [_cb rdna4RetainEncoded:_baseHandles];
    [_cb rdna4RetainEncoded:_fetchHandles];
    _handlesSnapCBId = cbid;
    _handlesSnapSegSer = segSer;
    _snapBaseHandles = _baseHandles;
    _snapFetchHandles = _fetchHandles;
}
- (void)rdna4ReleaseRenderHandles {
    _baseHandles = nil;
    _fetchHandles = nil;
    _rp1 = VK_NULL_HANDLE;
    _fb = _fbf = VK_NULL_HANDLE;
}
// ---- build 18 (P2): occlusion queries (garnet_occ.h has the model and the measured Metal semantics) ----
// Called at the end of the encoder's init. A pass with a visibility result buffer gets RDNA4OCC_MAXQ query indices from the command buffer's pool, reset HERE (a reset is illegal inside a render pass).
- (void)rdna4OccInit:(MTLRenderPassDescriptor *)d {
    id vb = d.visibilityResultBuffer;
    if (!vb) return;
    if (![vb isKindOfClass:[RDNA4Buffer class]] || ![(RDNA4Buffer *)vb contents]) { RDNA4_ONCE("occlusion: the visibility result buffer is not a CPU-mapped RDNA4Buffer; its offsets are not written"); return; }
    if (rdna4nc_drop_write(vb, RDNA4NC_VISIBILITY, "visibility result store", "RenderCommandEncoder")) { [_cb rdna4Fail:@"visibility result buffer is read-only"]; return; }
    int acc = 0;
    if ([d respondsToSelector:@selector(visibilityResultType)]) acc = (long)[(id)d visibilityResultType] == 1;   // MTLVisibilityResultTypeAccumulate
    rdna4occ *o = [_cb rdna4OccNew:(RDNA4Buffer *)vb accumulate:acc pool:&_qp base:&_qbase];
    if (!o) return;
    _occ = o;
    if (_qp != VK_NULL_HANDLE) vkCmdResetQueryPool([_cb vk], _qp, _qbase, RDNA4OCC_MAXQ);
    RDNA4LOGR("occlusion: pass with a visibility buffer (%lu bytes, %s)", (unsigned long)[vb length], acc ? "Accumulate" : "Reset");
}
- (void)rdna4OccEndQuery {   // the open interval ends here: always BEFORE the render pass it began in ends
    uint32_t q;
    if (_occ && rdna4occ_end(_occ, &q)) vkCmdEndQuery([_cb vk], _qp, _qbase + q);
}
- (void)rdna4OccDraw {   // inside the render pass, right before a draw is recorded
    uint32_t q; int precise = 0;
    if (_occ && rdna4occ_draw(_occ, &q, &precise)) vkCmdBeginQuery([_cb vk], _qp, _qbase + q, (precise && RDNA4R.occPrecise) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
- (void)rdna4EndPass {   // THE one place a Vulkan render pass of this encoder ends (the pass is split by layout changes, uploads, barriers, framebuffer fetch; each part is a new query interval)
    if (!_inPass) return;
    [self rdna4OccEndQuery];
    vkCmdEndRenderPass([_cb vk]); _inPass = NO;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset {
    if (!_occ) { RDNA4_ONCE("setVisibilityResultMode:offset: on a pass without a (usable) visibility result buffer: ignored"); return; }
    int endq = 0; int rc = rdna4occ_set(_occ, mode, offset, &endq);
    if (endq) vkCmdEndQuery([_cb vk], _qp, _qbase + _occ->activeQ);   // rdna4occ_set closed the interval; activeQ names its query
    if (rc) RDNA4LOG("setVisibilityResultMode:%lu offset:%lu REJECTED (mode > 2, offset not a multiple of 8 or past the %llu-byte buffer): treated as Disabled", (unsigned long)mode, (unsigned long)offset, (unsigned long long)_occ->buflen);
}
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 1; }

// ---- bundle 10 + Metal 2: multisample resolve ----
// 1 when colour attachment i's resolve texture is usable (a 2D single-sample texture of the attachment's size and format); logs and returns 0 otherwise.
- (BOOL)rdna4CheckResolve:(uint32_t)i {
    RDNA4Texture *src = _tex[i]; id r = _rtex[i];
    if (![r isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: attachment %u: the resolve texture is not an RDNA4Texture; refused", i); return NO; }
    RDNA4Texture *dst = r;
    if (dst == src || [dst rdna4IsView] || [dst textureType] == MTLTextureType3D) { RDNA4LOG("render pass: attachment %u: the resolve texture is the attachment itself, a view or a 3D texture; refused", i); return NO; }
    if (_rlvl[i] >= [dst mipmapLevelCount] || _rslice[i] + _layers > [dst rdna4Layers]) { RDNA4LOG("render pass: attachment %u: resolve level %u slice %u+%u is outside the resolve texture; refused", i, _rlvl[i], _rslice[i], _layers); return NO; }
    unsigned sw = (unsigned)MAX((NSUInteger)1, src.width >> _slvl[i]), sh = (unsigned)MAX((NSUInteger)1, src.height >> _slvl[i]), dw = (unsigned)MAX((NSUInteger)1, dst.width >> _rlvl[i]), dh = (unsigned)MAX((NSUInteger)1, dst.height >> _rlvl[i]);
    const char *why = NULL;
    if (!rdna4sa_target_ok(sw, sh, (unsigned)[src vkFormat], [src rdna4Aspects], dw, dh, (unsigned)[dst vkFormat], (unsigned)[dst rdna4Samples], &why)) { RDNA4LOG("render pass: attachment %u: %s; refused", i, why); return NO; }
    return YES;
}
- (void)rdna4RunDSResolveInto:(RDNA4Texture *)dst level:(uint32_t)dlvl slice:(uint32_t)dslice
                    depthMode:(VkResolveModeFlagBits)dMode stencilMode:(VkResolveModeFlagBits)sMode {
    RDNA4Texture *src = _tex[_na];
    VkCommandBuffer cmd = [_cb vk];
    if (rdna4nc_drop_write(dst, RDNA4NC_RESOLVE, "depth/stencil resolve", "RenderCommandEncoder")) { [_cb rdna4Fail:@"depth/stencil resolve destination is read-only"]; return; }
    [_cb rdna4RetainEncoded:src];
    [_cb rdna4RetainEncoded:dst];
    rdna4_ios_usek(_cb, dst, NO, YES, RDNA4DF_W_BLIT);
    rdna4_tex_to(cmd, src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    rdna4_tex_to(cmd, dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    uint32_t sw = (uint32_t)MAX((NSUInteger)1, src.width >> _dlvl), sh = (uint32_t)MAX((NSUInteger)1, src.height >> _dlvl);
    VkImageView srcView = _layers > 1 ? [src rdna4AttViewLevel:_dlvl layer:_dslice layerCount:_layers] : [src rdna4AttViewLevel:_dlvl layer:_dslice];
    VkImageView dstView = _layers > 1 ? [dst rdna4AttViewLevel:dlvl layer:dslice layerCount:_layers] : [dst rdna4AttViewLevel:dlvl layer:dslice];
    unsigned srcAsp = [src rdna4Aspects], dstAsp = [dst rdna4Aspects];
    BOOL dstHasD = (dstAsp & RDNA4DP_ASP_DEPTH) != 0, dstHasS = (dstAsp & RDNA4DP_ASP_STENCIL) != 0;
    VkAttachmentDescription2 ad[2] = {
        { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = [src vkFormat], .samples = (VkSampleCountFlagBits)[src rdna4Samples],
          .loadOp = (srcAsp & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .storeOp = (srcAsp & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = (srcAsp & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = (srcAsp & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = [dst vkFormat], .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = !dstHasD ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : dMode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
          .storeOp = dstHasD ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = !dstHasS ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : sMode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
          .stencilStoreOp = dstHasS ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL }
    };
    VkAttachmentReference2 refSrc = { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = 0, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .aspectMask = (VkImageAspectFlags)srcAsp };
    VkAttachmentReference2 refDst = { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .aspectMask = (VkImageAspectFlags)dstAsp };
    VkSubpassDescriptionDepthStencilResolve dsRes = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = dMode, .stencilResolveMode = sMode, .pDepthStencilResolveAttachment = &refDst };
    VkSubpassDescription2 sp = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, .pNext = &dsRes,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &refSrc };
    VkSubpassDependency2 deps[2] = {
        { .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT }
    };
    VkRenderPassCreateInfo2 rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, .attachmentCount = 2, .pAttachments = ad,
        .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = deps };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult vr = vkCreateRenderPass2(RDNA4R.dev, &rpci, NULL, &rp);
    if (vr != VK_SUCCESS) { [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateRenderPass2 (depth/stencil resolve) = %d", vr]]; return; }
    VkImageView views[2] = { srcView, dstView };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = views, .width = sw, .height = sh, .layers = _layers };
    VkFramebuffer fb = VK_NULL_HANDLE;
    vr = vkCreateFramebuffer(RDNA4R.dev, &fci, NULL, &fb);
    if (vr != VK_SUCCESS) { vkDestroyRenderPass(RDNA4R.dev, rp, NULL); [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (depth/stencil resolve) = %d", vr]]; return; }
    [_cb rdna4RetainEncoded:[[RDNA4RenderHandles alloc] initWithRenderPass:rp framebuffer:fb]];
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { sw, sh } } };
    VkSubpassBeginInfo sbi = { .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, .contents = VK_SUBPASS_CONTENTS_INLINE };
    VkSubpassEndInfo sei = { .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO };
    vkCmdBeginRenderPass2(cmd, &rbi, &sbi);
    vkCmdEndRenderPass2(cmd, &sei);
    [dst setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
    [src setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
}
// At endEncoding (after the last vkCmdEndRenderPass): every colour and depth/stencil attachment whose FINAL store action resolves is resolved into its resolve texture.
- (void)rdna4ResolveAll {
    VkCommandBuffer cmd = [_cb vk];
    for (uint32_t i = 0; i < _na; i++) {
        rdna4sa_t sat; RDNA4Texture *src = _tex[i];
        if (!rdna4sa_map((unsigned long)_sact[i], _rtex[i] != nil, [src rdna4Samples] > 1, &sat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"attachment %u: %s", i, sat.why]]; continue; }
        if (!sat.resolve) continue;
        if (![self rdna4CheckResolve:i]) { [_cb rdna4Fail:[NSString stringWithFormat:@"attachment %u: the resolve texture is unusable (see the log)", i]]; continue; }
        RDNA4Texture *dst = _rtex[i];
        if (rdna4nc_drop_write(dst, RDNA4NC_RESOLVE, "multisample resolve", "RenderCommandEncoder")) { [_cb rdna4Fail:@"multisample resolve destination is read-only"]; continue; }
        [_cb rdna4RetainEncoded:dst];
        rdna4_ios_usek(_cb, dst, NO, YES, RDNA4DF_W_BLIT);
        rdna4_tex_to(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); rdna4_tex_to(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve rg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _slvl[i], _sslice[i], _layers }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _rlvl[i], _rslice[i], _layers },
            .extent = { (uint32_t)MAX((NSUInteger)1, src.width >> _slvl[i]), (uint32_t)MAX((NSUInteger)1, src.height >> _slvl[i]), 1 } };
        vkCmdResolveImage(cmd, [src vkImage], rdna4_use_layout(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            [dst vkImage], rdna4_use_layout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &rg);
        RDNA4LOGR("render pass: attachment %u (%lu samples) resolved into texture %p level %u slice %u+%u", i, (unsigned long)[src rdna4Samples], (__bridge void *)dst, _rlvl[i], _rslice[i], _layers);
    }
    if (_nd) {
        RDNA4Texture *dsSrc = _tex[_na];
        unsigned srcSamples = (unsigned)[dsSrc rdna4Samples];
        rdna4sa_t dsat = {0}, ssat = {0};
        BOOL wantD = _drtex != nil || _dsact == MTLStoreActionMultisampleResolve || _dsact == MTLStoreActionStoreAndMultisampleResolve;
        BOOL wantS = _srtex != nil || _ssact == MTLStoreActionMultisampleResolve || _ssact == MTLStoreActionStoreAndMultisampleResolve;
        BOOL resD = NO, resS = NO;
        if (wantD && ([dsSrc rdna4Aspects] & RDNA4DP_ASP_DEPTH)) {
            if (!rdna4sa_map((unsigned long)_dsact, _drtex != nil, srcSamples > 1, &dsat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"depth resolve: %s", dsat.why]]; return; }
            if (dsat.resolve) {
                if (![self rdna4CheckDSResolve:YES]) { [_cb rdna4Fail:@"depth resolve target or filter invalid"]; return; }
                resD = YES;
            }
        }
        if (wantS && ([dsSrc rdna4Aspects] & RDNA4DP_ASP_STENCIL)) {
            if (!rdna4sa_map((unsigned long)_ssact, _srtex != nil, srcSamples > 1, &ssat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"stencil resolve: %s", ssat.why]]; return; }
            if (ssat.resolve) {
                if (![self rdna4CheckDSResolve:NO]) { [_cb rdna4Fail:@"stencil resolve target or filter invalid"]; return; }
                resS = YES;
            }
        }
        VkResolveModeFlagBits dMode = _dresFilter == 1 ? VK_RESOLVE_MODE_MIN_BIT : _dresFilter == 2 ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (resD && resS && _drtex == _srtex && _drlvl == _srlvl && _drslice == _srslice) {
            [self rdna4RunDSResolveInto:_drtex level:_drlvl slice:_drslice depthMode:dMode stencilMode:VK_RESOLVE_MODE_SAMPLE_ZERO_BIT];
        } else {
            if (resD) [self rdna4RunDSResolveInto:_drtex level:_drlvl slice:_drslice depthMode:dMode stencilMode:VK_RESOLVE_MODE_NONE];
            if (resS) [self rdna4RunDSResolveInto:_srtex level:_srlvl slice:_srslice depthMode:VK_RESOLVE_MODE_NONE stencilMode:VK_RESOLVE_MODE_SAMPLE_ZERO_BIT];
        }
    }
}
// ---- pass management ----
- (void)rdna4BeginPass {
    if (_hostWriteDropped) return;
    if (_inPass) return;
    if (_dcOn) rdna4dc_enc_reset_bound(&_dc);   // P5b: a new Vulkan render pass starts: forget the bound pipeline and the cached descriptor sets
    _dynDirty = GARNET_DYN_ALL;
    VkRenderPass rp = _rp1; VkFramebuffer fb = _fb; uint32_t ncv = _begun ? 0 : _na + _nd;
    BOOL rpc = _rp1c;
    RDNA4RenderHandles *continuationHandles = nil;
    if (_begun || _fetch || _ampLayerOff) {   // continuation/fetch/shifted-layer passes use the attachments' current layouts
        VkAttachmentDescription a2[9];
        for (uint32_t i = 0; i < _na; i++) {
            a2[i] = _ads[i]; a2[i].initialLayout = _curLay[i];
            if (_begun) a2[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a2[i].finalLayout = _fetch ? VK_IMAGE_LAYOUT_GENERAL : _ads[i].finalLayout;
        }
        if (_nd) {   // bundle 10: the depth/stencil attachment continues with what the first part stored
            a2[_na] = _ads[_na]; a2[_na].initialLayout = _curLay[_na]; a2[_na].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            if (_begun) { a2[_na].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a2[_na].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; }
        }
        rp = rdna4_mk_rp_c(a2, _na, _nd, _fetch, &rpc);
        if (!rp) { [_cb rdna4Fail:@"cannot create the continuation / framebuffer-fetch render pass"]; return; }
        if (_ampLayerOff && _ampLayerOff < _layers) {
            VkImageView ampViews[9];
            uint32_t ampLayers = _layers - _ampLayerOff;
            for (uint32_t i = 0; i < _na; i++) {
                ampViews[i] = ampLayers > 1
                    ? [_tex[i] rdna4AttViewLevel:_slvl[i] layer:_sslice[i] + _ampLayerOff layerCount:ampLayers]
                    : [_tex[i] rdna4AttViewLevel:_slvl[i] layer:_sslice[i] + _ampLayerOff];
            }
            if (_nd) {
                ampViews[_na] = ampLayers > 1
                    ? [_tex[_na] rdna4AttViewLevel:_dlvl layer:_dslice + _ampLayerOff layerCount:ampLayers]
                    : [_tex[_na] rdna4AttViewLevel:_dlvl layer:_dslice + _ampLayerOff];
            }
            BOOL viewsOk = YES;
            for (uint32_t i = 0; i < _na + _nd; i++) {
                if (ampViews[i] == VK_NULL_HANDLE) { viewsOk = NO; break; }
            }
            if (!viewsOk) {
                if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL);
                [_cb rdna4Fail:@"amplification layer offset: attachment image view unavailable"];
                return;
            }
            VkFramebuffer ampFb = VK_NULL_HANDLE;
            VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                .attachmentCount = _na + _nd, .pAttachments = ampViews, .width = _w, .height = _h, .layers = ampLayers };
            VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &ampFb);
            if (r != VK_SUCCESS) {
                if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL);
                [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (amplification layer offset) = %d", r]];
                return;
            }
            continuationHandles = [[RDNA4RenderHandles alloc]
                initWithRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:ampFb];
            fb = ampFb;
        } else if (_fetch) {
            if (!_fbf) {   // the fetch framebuffer: same views, a fetch-shaped (input-attachment) render pass for compatibility
                VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = _na + _nd, .pAttachments = _views, .width = _w, .height = _h, .layers = _layers };
                VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &_fbf);
                if (r != VK_SUCCESS) { _fbf = VK_NULL_HANDLE; if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL); [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (fetch) = %d", r]]; return; }
                _fetchHandles = [[RDNA4RenderHandles alloc]
                    initWithRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:_fbf];
            } else if (!rpc) continuationHandles = [[RDNA4RenderHandles alloc]
                initWithRenderPass:rp framebuffer:VK_NULL_HANDLE];
            fb = _fbf;
        } else if (!rpc) continuationHandles = [[RDNA4RenderHandles alloc]
            initWithRenderPass:rp framebuffer:VK_NULL_HANDLE];
    }
    for (uint32_t i = 0; i < _na; i++) { _curLay[i] = _fetch ? VK_IMAGE_LAYOUT_GENERAL : _ads[i].finalLayout; [_tex[i] setLayout:_curLay[i]]; }
    if (_nd) { _curLay[_na] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; [_tex[_na] setLayout:_curLay[_na]]; }
    VkSampleLocationEXT vkSL[8];
    if (_hasSamplePos) for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
    VkSampleLocationsInfoEXT slInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
        .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
        .sampleLocationGridSize = { 1, 1 },
        .sampleLocationsCount = _samplePosCount, .pSampleLocations = vkSL };
    VkAttachmentSampleLocationsEXT attLoc = { .attachmentIndex = _na, .sampleLocationsInfo = slInfo };
    VkSubpassSampleLocationsEXT postLoc = { .subpassIndex = 0, .sampleLocationsInfo = slInfo };
    VkRenderPassSampleLocationsBeginInfoEXT rpSL = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,
        .attachmentInitialSampleLocationsCount = _nd ? 1u : 0u, .pAttachmentInitialSampleLocations = _nd ? &attLoc : NULL,
        .postSubpassSampleLocationsCount = 1u, .pPostSubpassSampleLocations = &postLoc };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .pNext = _hasSamplePos ? &rpSL : NULL,
        .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { _w, _h } }, .clearValueCount = ncv, .pClearValues = _cvs };
    // A clear-only pass can begin before its encoder's first draw. Snapshot
    // attachments into the segment that actually records the pass.
    [self rdna4SnapshotResources];
    [_cb rdna4RetainEncoded:continuationHandles];
    vkCmdBeginRenderPass([_cb vk], &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (!_begun) for (uint32_t index = 0; index < _na; index++)
        if (_ads[index].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            [_cb rdna4NoteSurfaceOverwrite:_tex[index] width:_w height:_h level:_slvl[index]];
    _inPass = YES; _begun = YES; _activeLayerOff = _ampLayerOff;
}
- (void)rdna4Need:(RDNA4Texture *)t layout:(VkImageLayout)l {
    l = rdna4_use_layout(t, l);
    if ([t layout] == l) return;
    if (_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: texture %p needs layout %d (was %d)", (__bridge void *)t, l, [t layout]); }
    rdna4_tex_to([_cb vk], t, l);
}

// ---- state ----
- (void)setRenderPipelineState:(id)pso {
    if (![pso isKindOfClass:[RDNA4RenderPipelineState class]]) { RDNA4LOG("setRenderPipelineState: not an RDNA4RenderPipelineState"); return; }
    _pso = pso; _roReflection = [_pso reflection];
    _sv.scanPlan = [_pso scanPlanV];
    _sf.scanPlan = [_pso scanPlanF];
    _sv.rtAsRefl = [_pso rdna4RtAsReflV];
    _sf.rtAsRefl = [_pso rdna4RtAsReflF];
    [self rdna4OwnResource:pso kind:RDNA4_RESOURCE_PIPELINE
                  fragment:NO index:0];
}
- (void)setCullMode:(MTLCullMode)m { _cull = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { _wind = w; }
- (void)setViewport:(MTLViewport)v { _vp = v; _vps[0] = v; _nvp = 1; _dynDirty |= GARNET_DYN_VIEWPORT; }
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n {
    if (!n || !v) return;
    if (n > (RDNA4R.maxViewports ?: 1u)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setViewports: count %lu > maxViewports %u", (unsigned long)n, RDNA4R.maxViewports ?: 1u]];
        return;
    }
    for (NSUInteger i = 0; i < n; i++) _vps[i] = v[i];
    _nvp = (uint32_t)n; _vp = v[0]; _dynDirty |= GARNET_DYN_VIEWPORT;
}
- (void)setScissorRect:(MTLScissorRect)r { _sr = r; _srs[0] = r; _nsr = 1; _dynDirty |= GARNET_DYN_SCISSOR; }
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n {
    if (!n || !r) return;
    if (n > (RDNA4R.maxViewports ?: 1u)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setScissorRects: count %lu > maxViewports %u", (unsigned long)n, RDNA4R.maxViewports ?: 1u]];
        return;
    }
    for (NSUInteger i = 0; i < n; i++) _srs[i] = r[i];
    _nsr = (uint32_t)n; _sr = r[0]; _dynDirty |= GARNET_DYN_SCISSOR;
}
- (void)setVertexAmplificationCount:(NSUInteger)count
                       viewMappings:(const MTLVertexAmplificationViewMapping *)viewMappings {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"render setVertexAmplificationCount after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    rdna4amp_mapping tmp[RDNA4AMP_MAX_SUPPORTED_COUNT];
    const rdna4amp_mapping *mp = NULL;
    if (viewMappings && count >= 1u && count <= RDNA4AMP_MAX_SUPPORTED_COUNT) {
        for (NSUInteger i = 0; i < count; i++) {
            tmp[i].viewport_offset = viewMappings[i].viewportArrayIndexOffset;
            tmp[i].layer_offset = viewMappings[i].renderTargetArrayIndexOffset;
        }
        mp = tmp;
    }
    const char *why = NULL;
    if (!rdna4amp_validate_encoder_set((uint64_t)count, mp, RDNA4AMP_MAX_SUPPORTED_COUNT,
                                       RDNA4R.maxViewports ?: 1u, _layers, &why)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setVertexAmplificationCount:%lu: %s",
                        (unsigned long)count, why ?: "invalid"]];
        return;
    }
    _ampCount = (uint32_t)count;
    _ampHasMappings = (viewMappings != NULL);
    if (_ampHasMappings) memcpy(_ampMappings, tmp, count * sizeof(rdna4amp_mapping));
    else memset(_ampMappings, 0, sizeof _ampMappings);
    _ampLayerOff = _ampHasMappings ? _ampMappings[0].layer_offset : 0u;
    _dynDirty |= GARNET_DYN_VIEWPORT | GARNET_DYN_SCISSOR;
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"render sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"render sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    // vkCmdResetQueryPool is forbidden inside an active Vulkan render pass; split cleanly first.
    if (_inPass) [self rdna4EndPass];
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"render"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { _blend[0] = r; _blend[1] = g; _blend[2] = b; _blend[3] = a; _dynDirty |= GARNET_DYN_BLEND; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { _fillMode = m; }
- (void)setDepthClipMode:(NSUInteger)m { _depthClipMode = (MTLDepthClipMode)m; }
- (void)setDepthBias:(float)a slopeScale:(float)b clamp:(float)c { _bias[0] = a; _bias[1] = RDNA4R.dbClamp ? c : 0.0f; _bias[2] = b; _dynDirty |= GARNET_DYN_DEPTHBIAS; if (c != 0.0f && !RDNA4R.dbClamp) RDNA4_ONCE("setDepthBias: a non-zero clamp is ignored (the device lacks depthBiasClamp)"); }   // bundle 10
- (void)setStencilReferenceValue:(uint32_t)v { _sref[0] = _sref[1] = v; _dynDirty |= GARNET_DYN_STENCIL_R; }
- (void)setDepthStencilState:(id)s {   // bundle 10: recorded; read at every draw of a pipeline that has a depth/stencil attachment (a colour-only pipeline never looks at it)
    if (!s) {
        _dsi = rdna4_dsinfo_default();
        _dynDirty |= GARNET_DYN_STENCIL_M;
        [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_DEPTH_STENCIL
                      fragment:NO index:0];
        return;
    }
    if (![s isKindOfClass:[RDNA4DepthStencilState class]]) { RDNA4LOG("setDepthStencilState: not an RDNA4DepthStencilState"); return; }
    _dsi = [(RDNA4DepthStencilState *)s rdna4Info];
    _dynDirty |= GARNET_DYN_STENCIL_M;
    [self rdna4OwnResource:s kind:RDNA4_RESOURCE_DEPTH_STENCIL
                  fragment:NO index:0];
}
// ---- gap census: barriers, fences, residency, store actions (QuartzCore sends -memoryBarrierWithScope:afterStages:beforeStages: 6369 times in 45 s) ----
// A barrier between draws of one encoder = end the Vulkan render pass (the next draw starts a LOADing continuation pass; the pass-split machinery already
// exists for layout changes) plus a full memory barrier, which orders everything before it against everything after it.
- (void)rdna4Barrier:(const char *)what {
    if (_hostWriteDropped) return;
    if (_ended) return;
    if (_inPass) { [self rdna4EndPass]; }
    rdna4_full_barrier([_cb vk]);
    RDNA4_ONCE("render encoder %s: render pass split + full barrier", what);
}
- (void)memoryBarrierWithScope:(NSUInteger)sc afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)sc; (void)a; (void)b; [self rdna4Barrier:"memoryBarrierWithScope:"]; }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)r; (void)n; (void)a; (void)b; [self rdna4Barrier:"memoryBarrierWithResources:"]; }
- (void)textureBarrier { [self rdna4Barrier:"textureBarrier"]; }
- (void)updateFence:(id)f afterStages:(NSUInteger)st {
    (void)st;   // a full-pass split is a conservative superset of every selected render stage
    if (_ended) { [_cb rdna4Fail:@"render updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_begun) [self rdna4BeginPass];   // a clear-only pass records its load before the checkpoint
    if ([_cb rdna4Error]) return;
    if (_inPass) [self rdna4EndPass];
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f beforeStages:(NSUInteger)st {
    (void)st;
    if (_ended) { [_cb rdna4Fail:@"render waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (_inPass) [self rdna4EndPass];
    [_cb rdna4WaitForFence:f];
}
- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i { if (i < _na) _sact[i] = a; else RDNA4_ONCE("setColorStoreAction: index past the colour attachments; ignored"); }   // bundle 10: only a resolve changes anything (colour attachments are always stored)
- (void)setColorStoreActionOptions:(NSUInteger)a atIndex:(NSUInteger)i { (void)a; (void)i; }
- (void)setDepthStoreAction:(NSUInteger)a { if (_nd) _dsact = a; }
- (void)setDepthStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilStoreAction:(NSUInteger)a { if (_nd) _ssact = a; }
- (void)setStencilStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { _sref[0] = f; _sref[1] = b; _dynDirty |= GARNET_DYN_STENCIL_R; }
- (void)setLineWidth:(float)w { (void)w; }
RDNA4_ENCODER_SHARED
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

- (RDNA4StageState *)rdna4Stage:(BOOL)frag { return frag ? &_sf : &_sv; }
- (void)rdna4SetBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31) { [_cb rdna4Fail:[NSString stringWithFormat:@"buffer index %lu >= 31", (unsigned long)i]]; return; }
    if (buf && ![buf isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"setBuffer: not an RDNA4Buffer"]; return; }
    RDNA4StageState *s = [self rdna4Stage:f]; s->buf[i].b = buf; s->buf[i].off = off;
    s->rt.as[i] = nil;
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_ACCEL fragment:f index:i];
    [self rdna4OwnResource:buf kind:RDNA4_RESOURCE_BUFFER fragment:f index:i];
}
- (void)rdna4SetBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31 || !p) { [_cb rdna4Fail:[NSString stringWithFormat:@"setBytes: index %lu / bytes %p", (unsigned long)i, p]]; return; }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off]) [self rdna4SetBuffer:b offset:off index:i frag:f];
}
- (void)rdna4SetTexture:(id)t index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 128) { [_cb rdna4Fail:@"texture index >= 128"]; return; }
    if (t && ![t isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"setTexture: not an RDNA4Texture"]; return; }
    [self rdna4Stage:f]->tex[i] = t;
    [self rdna4OwnResource:t kind:RDNA4_RESOURCE_TEXTURE fragment:f index:i];
}
- (void)rdna4SetSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                 override:(BOOL)override index:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    if (i >= 16) { [_cb rdna4Fail:@"sampler index >= 16"]; return; }
    if (s && ![s isKindOfClass:[RDNA4SamplerState class]]) { [_cb rdna4Fail:@"setSamplerState: not an RDNA4SamplerState"]; return; }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4StageState *state = [self rdna4Stage:f];
    RDNA4SamplerBinding *old = state->smp[i];
    state->smp[i] = binding;
    if (binding) {
        if (!_samplerBindings) _samplerBindings = [NSMutableSet set];
        [_samplerBindings addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < 16 && !used; ++k)
            used = _sv.smp[k] == old || _sf.smp[k] == old;
        if (!used) [_samplerBindings removeObject:old];
    }
}
- (void)rdna4SetSampler:(id)s index:(NSUInteger)i frag:(BOOL)f {
    [self rdna4SetSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i frag:f];
}
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetBuffer:b offset:o index:i frag:NO]; }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetBuffer:b offset:o index:i frag:YES]; }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sv.buf[i].off = o; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sf.buf[i].off = o; }
- (void)setVertexBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetBuffer:b[k] offset:o[k] index:r.location + k frag:NO]; }
- (void)setFragmentBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetBuffer:b[k] offset:o[k] index:r.location + k frag:YES]; }
- (void)rdna4SetVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    if (i >= 31) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setVisibleFunctionTable: buffer index %lu >= 31",
                        f ? "fragment" : "vertex", (unsigned long)i]];
        return;
    }
    if (!t) { [self rdna4SetBuffer:nil offset:0 index:i frag:f]; return; }
    if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]] || [t device] != [_cb device]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setVisibleFunctionTable: not an RDNA4VisibleFunctionTable for this device",
                        f ? "fragment" : "vertex"]];
        return;
    }
    RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
    [vft rdna4RetainIntoCommandBuffer:_cb];
    [self rdna4SetBuffer:[vft rdna4BackingBuffer] offset:0 index:i frag:f];
}
- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetVisibleFunctionTable:t atBufferIndex:i frag:NO];
}
- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"vertex setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetVisibleFunctionTable:t[k] atBufferIndex:r.location + k frag:NO];
}
- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetVisibleFunctionTable:t atBufferIndex:i frag:YES];
}
- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"fragment setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetVisibleFunctionTable:t[k] atBufferIndex:r.location + k frag:YES];
}
- (void)rdna4SetAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    RDNA4StageState *s = [self rdna4Stage:f];
    if (!rdna4_rt_encoder_bind_accel(_cb, [_cb device], f ? "fragment" : "vertex", &s->rt, accel, i)) return;
    s->buf[i] = (RDNA4BufSlot){ nil, 0 };
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_BUFFER fragment:f index:i];
    [self rdna4OwnResource:accel kind:RDNA4_RESOURCE_ACCEL fragment:f index:i];
}
- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    [self rdna4SetAccelerationStructure:accel atBufferIndex:i frag:NO];
}
- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    [self rdna4SetAccelerationStructure:accel atBufferIndex:i frag:YES];
}
- (void)rdna4SetIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    RDNA4StageState *s = [self rdna4Stage:f];
    RDNA4Buffer *backing = nil;
    if (!rdna4_rt_encoder_bind_ift(_cb, [_cb device], f ? "fragment" : "vertex", &s->rt, t, i, &backing)) return;
    [self rdna4SetBuffer:backing offset:0 index:i frag:f];
}
- (void)setVertexIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetIntersectionFunctionTable:t atBufferIndex:i frag:NO];
}
- (void)setVertexIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "vertex", &_sv.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self rdna4SetBuffer:buf offset:0 index:idx frag:NO];
                                     });
}
- (void)setFragmentIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetIntersectionFunctionTable:t atBufferIndex:i frag:YES];
}
- (void)setFragmentIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "fragment", &_sf.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self rdna4SetBuffer:buf offset:0 index:idx frag:YES];
                                     });
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetBytes:p length:n index:i frag:NO]; }
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetBytes:p length:n index:i frag:YES]; }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { [self rdna4SetTexture:t index:i frag:NO]; }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { [self rdna4SetTexture:t index:i frag:YES]; }
- (void)setVertexTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetTexture:t[k] index:r.location + k frag:NO]; }
- (void)setFragmentTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetTexture:t[k] index:r.location + k frag:YES]; }
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { [self rdna4SetSampler:s index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { [self rdna4SetSampler:s index:i frag:YES]; }
- (void)setVertexSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self rdna4SetSampler:s lodMinClamp:a lodMaxClamp:b override:YES index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self rdna4SetSampler:s lodMinClamp:a lodMaxClamp:b override:YES index:i frag:YES]; }
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetSampler:s[k] index:r.location + k frag:NO]; }
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetSampler:s[k] index:r.location + k frag:YES]; }
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s
                   lodMinClamps:(const float *)lo
                   lodMaxClamps:(const float *)hi
                      withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"setVertexSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self rdna4SetSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES index:r.location + k frag:NO];
}
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s
                     lodMinClamps:(const float *)lo
                     lodMaxClamps:(const float *)hi
                        withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"setFragmentSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self rdna4SetSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES index:r.location + k frag:YES];
}

// ---- P5b helpers (only reached when the rdna4m-drawopt switch is ON) ----
- (void)rdna4NeedTex:(RDNA4Texture *)t layout:(VkImageLayout)l writable:(BOOL)writable {   // the body of needTex in rdna4PrepareDraw, verbatim
    if ([t rdna4IsIOS]) (void)rdna4_ios_touch(_cb, t, YES, writable, ^{   // 11h.6 / build 16 (P1): upload / flush outside the render pass (splits it if one is open)
        if (self->_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); } });
    [_cb rdna4CBRead:t];
    if ([t rdna4DispImgOnly]) (void)[_cb rdna4IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
    if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) { [_cb rdna4IOWritten:t]; [_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil]; }
    [self rdna4Need:t layout:l];
}
- (void)rdna4EachTex:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dev:(id)dev {   // rdna4_each_tex, with the callback inlined
    for (uint32_t i = 0; i < n; i++) {
        if ((pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && !pb[i].embedded) t = rdna4_dummy_texture(dev); if (t) [self rdna4NeedTex:t layout:pb[i].gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL writable:NO]; }
        } else if ((pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) {
                uint32_t ix = pb[i].binding - 480 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (t) [self rdna4NeedTex:t layout:VK_IMAGE_LAYOUT_GENERAL writable:YES];
            }
        }
    }
}
// One stage (idx 0 = vertex set, 1 = fragment set): signature of exactly the slots rdna4FillSet reads for these bindings; equal to the last bound one = no new set, no vkCmdBindDescriptorSets.
// Decision (RADV, radv_cmd_buffer.c): radv_mark_descriptors_dirty() only does `descriptors_state->dirty |= descriptors_state->valid`: bound sets stay valid across vkCmdBindPipeline and are
// re-flushed by the next draw, so a skipped vkCmdBindDescriptorSets after a pipeline bind leaves RADV with valid state (and radv_bind_descriptor_sets itself skips an already-bound valid set).
- (BOOL)rdna4DcStage:(int)idx pb:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dsl:(VkDescriptorSetLayout)dsl name:(const char *)nm cmd:(VkCommandBuffer)cmd {
    rdna4dc_sig sig; rdna4dc_sig_begin(&sig, _dc.layout, (uint64_t)(uintptr_t)dsl);
    for (uint32_t i = 0; i < n; i++) {
        const RDNA4PB *p = &pb[i];
        if (p->addressTable || p->samplerCoordinateFlags) { sig.over = 1; break; }
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding + k; RDNA4BufSlot sl = ix < 31 ? st->buf[ix] : (RDNA4BufSlot){ nil, 0 };
                rdna4dc_sig_push(&sig, sl.b ? (uint64_t)(uintptr_t)[sl.b vkBuffer] : 0); rdna4dc_sig_push(&sig, sl.b ? [sl.b rdna4VkOffset] + (uint64_t)sl.off : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - 32 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, [_cb device]) : ((p->binding >= 32 && ix < 128) ? st->tex[ix] : nil);
                if (t) [_cb rdna4RetainEncoded:t];
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); rdna4dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)[t vkView] : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)p->stat);
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - (p->comparisonSampler ? 736 : 160) + k;
                id sm = p->embedded ? rdna4_embedded_resource(p, k, st, NO, [_cb device]) : (ix < 16 ? st->smp[ix] : nil);
                if (sm) [_cb rdna4RetainArgument:sm];
                VkSampler handle = VK_NULL_HANDLE;
                if ([sm isKindOfClass:[RDNA4SamplerBinding class]])
                    handle = [(RDNA4SamplerBinding *)sm vkSampler];
                else if ([sm isKindOfClass:[RDNA4SamplerState class]])
                    handle = [(RDNA4SamplerState *)sm vkSampler];
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)handle); }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            BOOL inp = p->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - (inp ? 192 : 480) + k;
                RDNA4Texture *t = (!inp && p->embedded)
                    ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, [_cb device])
                    : ((p->binding >= (inp ? 192u : 480u) && ix < (inp ? 8u : 128u)) ? (inp ? st->att[ix] : st->tex[ix]) : nil);
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); rdna4dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)(inp ? [t rdna4AttViewLevel:0] : [t rdna4StorageView]) : 0); }
            break; }
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            rdna4_rt_dc_push_accel_sig(&sig, p->binding, p->count, &st->rt);
            break;
        default: sig.over = 1; break;   // unsupported type: let rdna4FillSet refuse it as before
        }
        if (sig.over) break;
    }
    uint64_t set;
    if (rdna4dc_stage_match(&_dc.st[idx], &sig, &set)) { _dc.setsReused++; return YES; }   // same slots, same layout: the set bound at this index is already right
    VkDescriptorSet s = [_cb rdna4AllocSet:dsl]; if (!s || ![_cb rdna4FillSet:s bindings:pb count:n state:st stage:nm]) { rdna4dc_stage_reset(&_dc.st[idx]); return NO; }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], (uint32_t)idx, 1, &s, 0, NULL);
    rdna4dc_stage_store(&_dc.st[idx], &sig, (uint64_t)(uintptr_t)s); _dc.setsAllocated++;
    return YES;
}
// ---- draws ----
- (BOOL)rdna4PrepareDraw:(VkPrimitiveTopology)topo {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"draw after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if ([_cb rdna4HasArgumentWrites]) {
        if (_inPass) [self rdna4EndPass];
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return NO;
    }
    if (!_pso) { [_cb rdna4Fail:@"draw without a render pipeline state"]; return NO; }
    { RDNA4RenderPipelineState *r = [_pso rdna4Real];   // hot-swap: switch to the real pipeline BETWEEN draws; every accessor below then reads one coherent object
      if (r) {
          _pso = r;
          _sv.scanPlan = [_pso scanPlanV];
          _sf.scanPlan = [_pso scanPlanF];
          _sv.rtAsRefl = [_pso rdna4RtAsReflV];
          _sf.rtAsRefl = [_pso rdna4RtAsReflF];
          [self rdna4OwnResource:r kind:RDNA4_RESOURCE_PIPELINE
                        fragment:NO index:0];
          RDNA4LOGR("HOT-SWAP: render encoder now uses the real pipeline %p", (__bridge void *)r);
      } }
    if ([_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawPrimitives/drawPatches called with a mesh render pipeline"];
        return NO;
    }
    if ([_pso rdna4IsTessellation] != (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)) {
        [_cb rdna4Fail:[_pso rdna4IsTessellation]
            ? @"drawPrimitives called with a tessellation render pipeline"
            : @"drawPatches called with a non-tessellation render pipeline"];
        return NO;
    }
    MTLRenderPipelineReflection *reflection = nil;
    const BOOL hasFrag = [_pso rdna4HasFragment];
    if (rdna4nc_any_readonly_buffer()) {
        reflection = [_pso reflection] ?: _roReflection;
        BOOL roV = rdna4nc_stage_writes(&_sv, rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX), RDNA4NC_VERTEX, "vertex draw", &_roWarnV);
        BOOL roF = hasFrag ? rdna4nc_stage_writes(&_sf, rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT), RDNA4NC_FRAGMENT, "fragment draw", &_roWarnF) : NO;
        if (roV || roF) { [_cb rdna4Fail:@"render draw writes a read-only no-copy buffer"]; return NO; }
    }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];
    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // both are window-space (y down) senses
    if ([_pso rdna4DSVk] != _dsVkFmt || [_pso rdna4SampleCount] != _encSamples) {   // bundle 10: the pipeline's attachments must be the pass's (Metal validation; a Vulkan render-pass compatibility rule)
        [_cb rdna4Fail:[NSString stringWithFormat:@"the render pipeline's depth/stencil format (vk %d) or sample count (%u) differs from the pass's (vk %d, %u)", (int)[_pso rdna4DSVk], [_pso rdna4SampleCount], (int)_dsVkFmt, _encSamples]]; return NO; }
    const char *ampWhy = NULL;
    if (!rdna4amp_draw_ok(_ampCount, [_pso rdna4MaxVertexAmplificationCount],
                          _ampHasMappings ? _ampMappings : NULL, _nvp, _layers, &ampWhy)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"render draw vertex amplification invalid: %s", ampWhy ?: "unsupported"]];
        return NO;
    }
    uint32_t dk = [_pso rdna4DSKey:&_dsi];   // bundle 10: 0 for a colour-only pipeline
    uint32_t vpOff = _ampHasMappings ? _ampMappings[0].viewport_offset : 0u;
    uint32_t vpCount = rdna4amp_viewport_count(_nvp, _nsr, vpOff, RDNA4R.maxViewports ?: 1u);
    uint32_t rflags = ((_fillMode == MTLTriangleFillModeLines) ? 1u : 0u) |
                      ((_depthClipMode == MTLDepthClipModeClamp) ? 2u : 0u) |
                      (_hasSamplePos ? 4u : 0u) | ((vpCount - 1u) << 3);
    uint32_t ckey = (uint32_t)cull | (rflags << 4);
    uint32_t fkey = (uint32_t)front | (dk << 1);   // the last-answer cache compares (topology, cull+rflags, front+ds, pso)
    VkPipeline vkp;
    if (_dcOn) {   // P5b item 3: the last (topology, cull, front, pso) -> VkPipeline answer skips pipelineForTopology's NSNumber / NSLock / NSDictionary
        uint64_t pp;
        if (rdna4dc_enc_pc_get(&_dc, (uint32_t)topo, ckey, fkey, (__bridge const void *)_pso, &pp)) vkp = (VkPipeline)(uintptr_t)pp;
        else { vkp = [_pso pipelineForTopology:topo cull:cull front:front ds:dk rflags:rflags]; rdna4dc_enc_pc_put(&_dc, (uint32_t)topo, ckey, fkey, (__bridge const void *)_pso, (uint64_t)(uintptr_t)vkp); }
    } else vkp = [_pso pipelineForTopology:topo cull:cull front:front ds:dk rflags:rflags];
    if (!vkp) { [_cb rdna4Fail:@"cannot build a pipeline variant for this draw"]; return NO; }
    const uint32_t *need = [_pso needBindings]; uint32_t nn = [_pso nNeed];
    for (uint32_t i = 0; i < nn; i++) if (need[i] != 31 && !_sv.buf[need[i]].b) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"vertex buffer at index %u is read by the vertex descriptor but never bound", need[i]]]; return NO; }
    id dev = [_cb device];
    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        if ([t rdna4IsIOS]) (void)rdna4_ios_touch(self->_cb, t, YES, writable, ^{   // 11h.6 / build 16 (P1): upload / flush outside the render pass (splits it if one is open)
            if (self->_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); } });
        [self->_cb rdna4CBRead:t];   // Stage 0b: sampled (or storage-accessed) by a render draw
        if ([t rdna4DispImgOnly]) (void)[self->_cb rdna4IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
        if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) { [self->_cb rdna4IOWritten:t]; [self->_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil]; }   // a storage-image write from a render pass
        [self rdna4Need:t layout:l];
    };
    if (_dcOn) {   // P5b item 5: the same visit as rdna4_each_tex, as a plain loop (no heap-copied block)
        [self rdna4EachTex:[_pso pbV] n:[_pso npbV] state:&_sv dev:dev];
        if (hasFrag) [self rdna4EachTex:[_pso pbF] n:[_pso npbF] state:&_sf dev:dev];
    } else {
    rdna4_each_tex([_pso pbV], [_pso npbV], &_sv, dev, needTex);
    if (hasFrag) rdna4_each_tex([_pso pbF], [_pso npbF], &_sf, dev, needTex);
    }
    const RDNA4ArgumentTables *argTables = [_pso argumentTables];
    const BOOL needArgPrep =
        (argTables && argTables->active) ||
        [_pso rdna4HasAddressRegistry] ||
        (_sv.scanPlan && _sv.scanPlan->plan.count) ||
        (hasFrag && _sf.scanPlan && _sf.scanPlan->plan.count) ||
        _argumentResources.count != 0 ||
        _argumentWritable.count != 0 ||
        [_cb rdna4HasResidency] ||
        rdna4_rt_any_arg_owners();
    if (needArgPrep) {
        if (!reflection) reflection = [_pso reflection] ?: _roReflection;
        NSMutableSet *argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pbV], [_pso npbV], &_sv, rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX));
        if (hasFrag) rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pbF], [_pso npbF], &_sf, rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT));
        if (rdna4_rt_any_arg_owners()) {
            for (uint32_t i = 0; i < 31; i++) {
                if (_sv.buf[i].b) rdna4_rt_retain_argbuf_resources(_sv.buf[i].b, _cb);
                if (hasFrag && _sf.buf[i].b) rdna4_rt_retain_argbuf_resources(_sf.buf[i].b, _cb);
            }
            for (id res in argumentResources) {
                if ([res isKindOfClass:[RDNA4Buffer class]])
                    rdna4_rt_retain_argbuf_resources((RDNA4Buffer *)res, _cb);
            }
        }
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[] = {&_sv, &_sf};
        NSString *scanError = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, hasFrag ? 2 : 1, argumentWritable, dev,
                                         &scanError)) {
            [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
            return NO;
        }
        BOOL argumentsReady = [_cb rdna4PrepareArguments:argTables
            resources:argumentResources writable:argumentWritable scan:&scan
            textures:needTex];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!argumentsReady) return NO;
    }
    if ([_pso fetch] != _fetch) {   // 11e-2: framebuffer-fetch draws need the input-attachment render-pass shape; a change of shape is a pass split (LOAD continuation)
        if ([_pso fetch] && [_pso fetchMax] >= _na) { [_cb rdna4Fail:[NSString stringWithFormat:@"pipeline reads [[color(%u)]] but the encoder has %u colour attachment(s)", [_pso fetchMax], _na]]; return NO; }
        if (_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: framebuffer fetch %s", [_pso fetch] ? "on" : "off"); }
        _fetch = [_pso fetch];
    }
    if (_ampLayerOff && !_begun) {
        // Ensure initial Clear/Load runs across all pass layers before starting a layer-shifted continuation pass.
        uint32_t savedLayerOff = _ampLayerOff;
        _ampLayerOff = 0u;
        [self rdna4BeginPass];
        if ([_cb rdna4Error]) { _ampLayerOff = savedLayerOff; return NO; }
        [self rdna4EndPass];
        _ampLayerOff = savedLayerOff;
    }
    if (_inPass && _activeLayerOff != _ampLayerOff) [self rdna4EndPass];
    [self rdna4BeginPass];
    if ([_cb rdna4Error]) return NO;
    [self rdna4OccDraw];   // build 18 (P2): the first draw after a visibility mode / offset change (or pass split) begins its query interval
    const BOOL fallback = [_pso rdna4IsRenderFallback];
    if (fallback) [_cb rdna4NoteShaderFallback];
    {   // native #12 P1/D1: this draw writes a display surface; GPUPass = CoreDisplay's final display pass, ColorFill = a fill, anything else a SkyLight composite
        BOOL anyd = NO; for (uint32_t i = 0; i < _na; i++) if ([_tex[i] rdna4IsDisp]) anyd = YES;
        if (anyd) {
            RDNA4Texture *src = nil;   // the first IOSurface texture this draw samples in the fragment stage
            const RDNA4PB *pbf = [_pso pbF]; uint32_t npf = [_pso npbF];
            for (uint32_t i = 0; i < npf && !src; i++) if (pbf[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && pbf[i].binding >= 32 && pbf[i].binding < 160)
                for (uint32_t k = 0; k < pbf[i].count && !src; k++) { uint32_t ix = pbf[i].binding - 32 + k; RDNA4Texture *tt = ix < 128 ? _sf.tex[ix] : nil; if (tt && [tt rdna4IsIOS]) src = tt; }
            uint32_t bits = fallback ? RDNA4DF_W_SHADER_FALLBACK : rdna4df_draw_bits([_pso rdna4FName]);
            rdna4df_rect_t rc = rdna4df_draw_rect(_vp.originX, _vp.originY, _vp.width, _vp.height, (int64_t)_sr.x, (int64_t)_sr.y, (int64_t)_sr.width, (int64_t)_sr.height, _w, _h);
            for (uint32_t i = 0; i < _na; i++) if ([_tex[i] rdna4IsDisp]) {
                BOOL clr = (_clrm >> i) & 1u; if (clr && (bits & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP))) _clrm &= ~(1u << i); else clr = NO;
                [_cb rdna4DispNote:_tex[i] bits:bits pso:_pso];
                [_cb rdna4DispDraw:_tex[i] bits:bits rect:rc w:_w h:_h clear:clr src:src vp:_vp sc:_sr];
            }
            static _Atomic int vlog;   // D1 sample of the first 30 display draws: viewport, scissor, vertex count, and the first 32 bytes of the first bound vertex-stage buffer (the extents may live in vertex data)
            if (atomic_load(&vlog) < 30 && atomic_fetch_add(&vlog, 1) < 30) {
                const uint8_t *vb = NULL; uint32_t vbi = 0;
                for (uint32_t i = 0; i < 31 && !vb; i++) if (_sv.buf[i].b) { const uint8_t *c = (const uint8_t *)[_sv.buf[i].b contents]; if (c) { vb = c + _sv.buf[i].off; vbi = i; } }
                char hx[200]; hx[0] = 0; if (vb) for (int k = 0; k < 8; k++) { float f; uint32_t u; memcpy(&u, vb + 4 * k, 4); memcpy(&f, &u, 4); size_t l = strlen(hx); snprintf(hx + l, sizeof hx - l, "%s%08x(%g)", k ? " " : "", u, (double)f); }
                RDNA4LOG("scanout: DRAW sample #%d: %s/%s count %lu viewport %.1f,%.1f %.1fx%.1f scissor %lu,%lu %lux%lu -> rect [%d,%d)-[%d,%d); source texture: %s id %u %lux%lu; vertex buffer %u words: %s",
                    atomic_load(&vlog), [_pso rdna4VName], [_pso rdna4FName], (unsigned long)_dvc, _vp.originX, _vp.originY, _vp.width, _vp.height, (unsigned long)_sr.x, (unsigned long)_sr.y, (unsigned long)_sr.width, (unsigned long)_sr.height,
                    rc.x0, rc.y0, rc.x1, rc.y1, src ? ([src rdna4IsDisp] ? "display surface" : "IOSurface") : "none", src && [src rdna4IOSRef] ? IOSurfaceGetID([src rdna4IOSRef]) : 0, src ? (unsigned long)src.width : 0UL, src ? (unsigned long)src.height : 0UL, vbi, vb ? hx : "(none or private)");
            }
        }
    }
    VkCommandBuffer cmd = [_cb vk];
    if (_fetch) {   // earlier draws' colour writes must be visible to this draw's input-attachment reads (self-dependency of the pass)
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    }
    if (_dcOn) {   // P5b item 2: RADV's radv_CmdBindPipeline re-marks descriptors dirty and re-binds dynamic state even for the same pipeline; viewport / scissor / blend are set below every draw anyway
        _dc.draws++;
        if (!rdna4dc_enc_pipe_same(&_dc, (uint64_t)(uintptr_t)vkp)) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    } else
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    const uint32_t dynEmit = _dcOn ? _dynDirty : GARNET_DYN_ALL;
    if (dynEmit & (GARNET_DYN_VIEWPORT | GARNET_DYN_SCISSOR)) {
        VkViewport vps[16]; VkRect2D scs[16];
        for (uint32_t i = 0; i < vpCount; i++) {
            uint32_t vi = i + vpOff, si = i + vpOff;
            if (dynEmit & GARNET_DYN_VIEWPORT) {
                MTLViewport mv = vi < _nvp ? _vps[vi] : _vps[_nvp - 1];
                vps[i] = (VkViewport){ (float)mv.originX, (float)(mv.originY + mv.height), (float)mv.width, -(float)mv.height, (float)mv.znear, (float)mv.zfar };
            }
            if (dynEmit & GARNET_DYN_SCISSOR) {
                MTLScissorRect ms = si < _nsr ? _srs[si] : (MTLScissorRect){ 0, 0, _w, _h };
                scs[i] = (VkRect2D){ { (int32_t)ms.x, (int32_t)ms.y }, { (uint32_t)ms.width, (uint32_t)ms.height } };
            }
        }
        if (dynEmit & GARNET_DYN_VIEWPORT) { vkCmdSetViewport(cmd, 0, vpCount, vps); _dynDirty &= ~GARNET_DYN_VIEWPORT; }
        if (dynEmit & GARNET_DYN_SCISSOR)  { vkCmdSetScissor(cmd, 0, vpCount, scs);  _dynDirty &= ~GARNET_DYN_SCISSOR; }
    }
    if (dynEmit & GARNET_DYN_BLEND) { vkCmdSetBlendConstants(cmd, _blend); _dynDirty &= ~GARNET_DYN_BLEND; }
    if (_hasSamplePos && rdna4_vkCmdSetSampleLocationsEXT) {
        VkSampleLocationEXT vkSL[8];
        for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
        VkSampleLocationsInfoEXT slInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _samplePosCount, .pSampleLocations = vkSL };
        rdna4_vkCmdSetSampleLocationsEXT(cmd, &slInfo);
    }
    if ([_pso rdna4HasDS]) {   // bundle 10: the dynamic depth/stencil state (masks and reference per face; the bias when the pipeline has depth)
        if (dynEmit & GARNET_DYN_STENCIL_M) {
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead); vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bRead);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fWrite); vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite);
            _dynDirty &= ~GARNET_DYN_STENCIL_M;
        }
        if (dynEmit & GARNET_DYN_STENCIL_R) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]); vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);
            _dynDirty &= ~GARNET_DYN_STENCIL_R;
        }
        if ([_pso rdna4HasDepth] && (dynEmit & GARNET_DYN_DEPTHBIAS)) {
            vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2]);
            _dynDirty &= ~GARNET_DYN_DEPTHBIAS;
        }
    }
    for (uint32_t i = 0; i < nn; i++) {
        RDNA4Buffer *b; NSUInteger off;
        if (need[i] == 31) { b = rdna4_dummy_buffer(dev); off = 0; if (!b) { [_cb rdna4Fail:@"no dummy buffer"]; return NO; } [_cb rdna4RetainEncoded:b]; }
        else { b = _sv.buf[need[i]].b; off = _sv.buf[need[i]].off; }
        uint64_t vo, viewSize;
        if ([b device] != dev || !rdna4_buffer_view(b.length, off, [b rdna4VkOffset], 1, UINT32_MAX, &vo, &viewSize)) {
            [_cb rdna4Fail:@"vertex buffer window is outside the logical resource"]; return NO;
        }
        VkBuffer vb = [b vkBuffer];
        vkCmdBindVertexBuffers(cmd, need[i], 1, &vb, &vo);
    }
    [_cb rdna4BindArguments:[_pso argumentTables] point:VK_PIPELINE_BIND_POINT_GRAPHICS layout:[_pso layout]];
    if (_dcOn && ![_pso argumentTables]->active) {   // GPU tables get a fresh stage set for each immutable snapshot.
        rdna4dc_enc_layout(&_dc, (uint64_t)(uintptr_t)[_pso layout]);
        if ([_pso npbV] && ![self rdna4DcStage:0 pb:[_pso pbV] n:[_pso npbV] state:&_sv dsl:[_pso dsl:0] name:"vertex" cmd:cmd]) return NO;
        if ([_pso npbF] && ![self rdna4DcStage:1 pb:[_pso pbF] n:[_pso npbF] state:&_sf dsl:[_pso dsl:1] name:"fragment" cmd:cmd]) return NO;
        return YES;
    }
    if ([_pso npbV]) {
        VkDescriptorSet s = [_cb rdna4AllocSet:[_pso dsl:0]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pbV] count:[_pso npbV] state:&_sv stage:"vertex"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 0, 1, &s, 0, NULL);
    }
    if ([_pso npbF]) {
        VkDescriptorSet s = [_cb rdna4AllocSet:[_pso dsl:1]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pbF] count:[_pso npbF] state:&_sf stage:"fragment"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 1, 1, &s, 0, NULL);
    }
    return YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi {
    if (_hostWriteDropped) return;
    _dvc = c * (ic ? ic : 1);
    if (!c || !ic) return;
    if (c > UINT32_MAX || ic > UINT32_MAX || s > UINT32_MAX || bi > UINT32_MAX || c - 1 > UINT32_MAX - s || ic - 1 > UINT32_MAX - bi) {
        [_cb rdna4Fail:@"draw vertex or instance range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([[_pso rdna4Real] fetch] || [_pso fetch]) {
        if (!RDNA4R.orderedColorAccess) {
            NSUInteger size = t == MTLPrimitiveTypePoint ? 1 : (t == MTLPrimitiveTypeLine || t == MTLPrimitiveTypeLineStrip) ? 2 : 3;
            NSUInteger step = t == MTLPrimitiveTypeLineStrip || t == MTLPrimitiveTypeTriangleStrip ? 1 : size;
            if (c < size) return;
            MTLWinding winding = _wind;
            for (NSUInteger instance = 0; instance < ic; instance++) {
                for (NSUInteger first = 0; first <= c - size; first += step) {
                    // A separate strip draw restarts parity; preserve front-facing and stencil semantics.
                    _wind = t == MTLPrimitiveTypeTriangleStrip && (first & 1) ? (winding == MTLWindingClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise) : winding;
                    BOOL ready = [self rdna4PrepareDraw:rdna4_topo(t)];
                    _wind = winding;
                    if (!ready) return;
                    vkCmdDraw([_cb vk], (uint32_t)size, 1, (uint32_t)(s + first), (uint32_t)(bi + instance));
                }
            }
            return;
        }
    }
    if ([self rdna4PrepareDraw:rdna4_topo(t)]) vkCmdDraw([_cb vk], (uint32_t)c, (uint32_t)ic, (uint32_t)s, (uint32_t)bi);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:1 baseInstance:0]; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:ic baseInstance:0]; }
- (BOOL)rdna4IndirectBuffer:(id)buffer offset:(NSUInteger)off bytes:(NSUInteger)bytes {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"indirect draw after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || (off & 3) || off > [buffer length] || bytes > [buffer length] - off) {
        [_cb rdna4Fail:@"indirect argument buffer, alignment or range is invalid"]; return NO;
    }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)buffer rdna4VkOffset] + off, 4, RDNA4NC_ALIGN_INDIRECT)) {
        [_cb rdna4Fail:@"indirect draw Vulkan offset must be a multiple of 4"]; return NO;
    }
    if (!vkCmdDrawIndirect || !vkCmdDrawIndexedIndirect || !RDNA4R.indirectFirstInstance) {
        [_cb rdna4Fail:@"backend lacks indirect drawing with nonzero base instances"]; return NO;
    }
    if (!RDNA4R.orderedColorAccess && ([[_pso rdna4Real] fetch] || [_pso fetch])) {
        [_cb rdna4Fail:@"indirect framebuffer fetch requires rasterization-order attachment access"]; return NO;
    }
    // A shader in an earlier draw may have generated these arguments.
    if (_inPass) [self rdna4EndPass];
    rdna4_full_barrier([_cb vk]);
    _dvc = 0;
    return YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off {
    if (![self rdna4IndirectBuffer:buffer offset:off bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PrepareDraw:rdna4_topo(t)]) {
        [_cb rdna4RetainEncoded:buffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)buffer vkBuffer], [(RDNA4Buffer *)buffer rdna4VkOffset] + off, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)ioff
              indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off {
    if (_hostWriteDropped) return;
    if (![ib isKindOfClass:[RDNA4Buffer class]] || (it != MTLIndexTypeUInt16 && it != MTLIndexTypeUInt32)) {
        [_cb rdna4Fail:@"indirect index buffer or index type is invalid"]; return;
    }
    NSUInteger size = it == MTLIndexTypeUInt32 ? 4 : 2;
    if (ioff % size || ioff >= [ib length]) { [_cb rdna4Fail:@"indirect index buffer offset is invalid"]; return; }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)ib rdna4VkOffset] + ioff, size, RDNA4NC_ALIGN_INDEX)) {
        [_cb rdna4Fail:@"index buffer Vulkan offset must be a multiple of the index size"]; return;
    }
    if (![self rdna4IndirectBuffer:buffer offset:off bytes:sizeof(VkDrawIndexedIndirectCommand)]) return;
    if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
    [_cb rdna4RetainEncoded:ib];
    [_cb rdna4RetainEncoded:buffer];
    vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + ioff, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect([_cb vk], [(RDNA4Buffer *)buffer vkBuffer], [(RDNA4Buffer *)buffer rdna4VkOffset] + off, 1, sizeof(VkDrawIndexedIndirectCommand));
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    if (_hostWriteDropped) return;
    if (![ib isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"drawIndexedPrimitives: index buffer is not an RDNA4Buffer"]; return; }
    NSUInteger isz = it == MTLIndexTypeUInt32 ? 4 : 2;
    if (off % isz) { [_cb rdna4Fail:@"index buffer offset is not a multiple of the index size"]; return; }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)ib rdna4VkOffset] + off, isz, RDNA4NC_ALIGN_INDEX)) {
        [_cb rdna4Fail:@"index buffer Vulkan offset must be a multiple of the index size"]; return;
    }
    if (off > [ib length] || c > ([ib length] - off) / isz) { [_cb rdna4Fail:@"indexed draw exceeds the index buffer"]; return; }
    _dvc = c * (ic ? ic : 1);
    if (!c || !ic) return;
    if (c > UINT32_MAX || ic > UINT32_MAX || bi > UINT32_MAX || ic - 1 > UINT32_MAX - bi || bv < INT32_MIN || bv > INT32_MAX) {
        [_cb rdna4Fail:@"indexed draw range exceeds Vulkan's 32-bit limits"]; return;
    }
    if (!RDNA4R.orderedColorAccess && ([[_pso rdna4Real] fetch] || [_pso fetch])) {
        if (t == MTLPrimitiveTypeLineStrip || t == MTLPrimitiveTypeTriangleStrip) {
            [_cb rdna4Fail:@"framebuffer fetch on indexed strips requires rasterization-order attachment access (restart parity cannot be inferred on the CPU)"]; return;
        }
        NSUInteger size = t == MTLPrimitiveTypePoint ? 1 : t == MTLPrimitiveTypeLine ? 2 : 3;
        if (c < size) return;
        for (NSUInteger instance = 0; instance < ic; instance++) {
            for (NSUInteger first = 0; first <= c - size; first += size) {
                if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
                [_cb rdna4RetainEncoded:ib];
                vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + off, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed([_cb vk], (uint32_t)size, 1, (uint32_t)first, (int32_t)bv, (uint32_t)(bi + instance));
            }
        }
        return;
    }
    if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
    [_cb rdna4RetainEncoded:ib];
    vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + off, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed([_cb vk], (uint32_t)c, (uint32_t)ic, 0, (int32_t)bv, (uint32_t)bi);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:ic baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)setTessellationFactorBuffer:(id)buffer offset:(NSUInteger)offset instanceStride:(NSUInteger)instanceStride {
    _tessFactorBuf = buffer; _tessFactorOff = offset; _tessFactorStride = instanceStride;
}
- (void)setTessellationFactorScale:(float)scale { _tessFactorScale = scale; }
- (BOOL)rdna4PreparePatchDraw:(NSUInteger)numCP indexed:(BOOL)indexed
                   patchIndex:(id)patchBuf patchIndexOffset:(NSUInteger)patchOff
                 controlIndex:(id)cpBuf controlIndexOffset:(NSUInteger)cpOff
                     indirect:(BOOL)indirect {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"drawPatches after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if (!_pso) { [_cb rdna4Fail:@"drawPatches without a render pipeline state"]; return NO; }
    RDNA4RenderPipelineState *real = [_pso rdna4Real] ?: _pso;
    if (![real rdna4IsTessellation]) {
        [_cb rdna4Fail:@"drawPatches: called with a non-tessellation render pipeline"]; return NO;
    }
    if (numCP > RDNA4R.maxTessPatchSize) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"numberOfPatchControlPoints %lu exceeds %u", (unsigned long)numCP, RDNA4R.maxTessPatchSize]];
        return NO;
    }
    id dev = [_cb device];
    const rdna4_tess_cfg_t *cfg = [real tessCfg];
    if (![_tessFactorBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)_tessFactorBuf device] != dev ||
        (_tessFactorOff & 3) || (_tessFactorStride & 3) || _tessFactorOff > [_tessFactorBuf length] ||
        ![(RDNA4Buffer *)_tessFactorBuf gpuAddress] || _tessFactorOff > UINT64_MAX - [(RDNA4Buffer *)_tessFactorBuf gpuAddress]) {
        [_cb rdna4Fail:@"tessellation factor buffer, offset or instance stride is invalid"]; return NO;
    }
    uint32_t fbytes = rdna4_tess_factor_stride(cfg);
    if ((!indirect || cfg->factor_step == RDNA4_TESS_FSTEP_CONST) && fbytes > [_tessFactorBuf length] - _tessFactorOff) {
        [_cb rdna4Fail:@"tessellation factor buffer range exceeds buffer length"]; return NO;
    }
    if (patchBuf && (![patchBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)patchBuf device] != dev ||
                     (patchOff & 3) || patchOff > [patchBuf length] ||
                     ![(RDNA4Buffer *)patchBuf gpuAddress] || patchOff > UINT64_MAX - [(RDNA4Buffer *)patchBuf gpuAddress])) {
        [_cb rdna4Fail:@"tessellation patch index buffer or offset is invalid"]; return NO;
    }
    if (indexed) {
        if (cfg->cp_idx_type == RDNA4_TESS_CPIDX_NONE) {
            [_cb rdna4Fail:@"drawIndexedPatches: requires tessellationControlPointIndexType != None"]; return NO;
        }
        NSUInteger align = cfg->cp_idx_type == RDNA4_TESS_CPIDX_U32 ? 4u : 2u;
        if (![cpBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)cpBuf device] != dev ||
            (cpOff & (align - 1u)) || cpOff > [cpBuf length] ||
            ![(RDNA4Buffer *)cpBuf gpuAddress] || cpOff > UINT64_MAX - [(RDNA4Buffer *)cpBuf gpuAddress]) {
            [_cb rdna4Fail:@"control point index buffer, alignment or offset is invalid"]; return NO;
        }
    }
    rdna4_tess_draw_params_t params = {0};
    params.factor_bda = [(RDNA4Buffer *)_tessFactorBuf gpuAddress] + _tessFactorOff;
    params.factor_size = [_tessFactorBuf length] - _tessFactorOff;
    params.factor_instance_stride = _tessFactorStride;
    if (patchBuf) {
        params.patch_idx_bda = [(RDNA4Buffer *)patchBuf gpuAddress] + patchOff;
        params.patch_idx_size = [patchBuf length] - patchOff;
    }
    if (indexed) {
        params.cp_idx_bda = [(RDNA4Buffer *)cpBuf gpuAddress] + cpOff;
        params.cp_idx_size = [cpBuf length] - cpOff;
    }
    params.factor_scale = _tessFactorScale;
    params.num_patch_cp = (uint32_t)numCP;
    RDNA4Buffer *retainVbs[RDNA4_TESS_MAX_VBUFS];
    uint32_t nRetainVbs = 0;
    for (uint32_t i = 0; i < cfg->num_inputs; i++) {
        const rdna4_tess_input_t *in = &cfg->inputs[i];
        if (!in->has_attr) continue;
        RDNA4Buffer *vb = _sv.buf[in->buf_idx].b;
        NSUInteger vo = _sv.buf[in->buf_idx].off;
        if (!vb || ![vb isKindOfClass:[RDNA4Buffer class]] || [vb device] != dev || vo > [vb length]) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"tessellation vertex buffer at index %u is missing or out of range", in->buf_idx]];
            return NO;
        }
        uint64_t va = [vb gpuAddress];
        if (!va || vo > UINT64_MAX - va) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"tessellation vertex buffer at index %u has invalid GPU address", in->buf_idx]];
            return NO;
        }
        if (!params.vb_bda[in->buf_idx]) {
            retainVbs[nRetainVbs++] = vb;
            params.vb_bda[in->buf_idx] = va + vo;
            params.vb_size[in->buf_idx] = [vb length] - vo;
        }
    }
    if (![self rdna4PrepareDraw:VK_PRIMITIVE_TOPOLOGY_PATCH_LIST]) return NO;
    for (uint32_t i = 0; i < nRetainVbs; i++) [_cb rdna4RetainEncoded:retainVbs[i]];
    [_cb rdna4RetainEncoded:_tessFactorBuf];
    if (patchBuf) [_cb rdna4RetainEncoded:patchBuf];
    if (indexed) [_cb rdna4RetainEncoded:cpBuf];
    RDNA4Buffer *pbuf = nil; NSUInteger poff = 0;
    if (![_cb rdna4Bytes:&params length:sizeof params buffer:&pbuf offset:&poff]) return NO;
    VkDescriptorSet ds = [_cb rdna4AllocSet:[_pso tessDsl]];
    if (!ds) return NO;
    VkDescriptorBufferInfo dbi = { [pbuf vkBuffer], [pbuf rdna4VkOffset] + poff, sizeof params };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(RDNA4R.dev, 1, &wds, 0, NULL);
    vkCmdBindDescriptorSets([_cb vk], VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], [_pso tessSet], 1, &ds, 0, NULL);
    return YES;
}
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance {
    if (_hostWriteDropped) return;
    _dvc = patchCount * (instanceCount ? instanceCount : 1);
    if (!patchCount || !instanceCount) return;
    if (patchCount > UINT32_MAX || instanceCount > UINT32_MAX || patchStart > UINT32_MAX || baseInstance > UINT32_MAX ||
        patchCount - 1 > UINT32_MAX - patchStart || instanceCount - 1 > UINT32_MAX - baseInstance) {
        [_cb rdna4Fail:@"drawPatches range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:NO patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:nil controlIndexOffset:0 indirect:NO]) {
        vkCmdDraw([_cb vk], (uint32_t)patchCount, (uint32_t)instanceCount, (uint32_t)patchStart, (uint32_t)baseInstance);
    }
}
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (![self rdna4IndirectBuffer:indirectBuffer offset:indirectBufferOffset bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:NO patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:nil controlIndexOffset:0 indirect:YES]) {
        [_cb rdna4RetainEncoded:indirectBuffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)indirectBuffer vkBuffer], [(RDNA4Buffer *)indirectBuffer rdna4VkOffset] + indirectBufferOffset, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance {
    if (_hostWriteDropped) return;
    _dvc = patchCount * (instanceCount ? instanceCount : 1);
    if (!patchCount || !instanceCount) return;
    if (patchCount > UINT32_MAX || instanceCount > UINT32_MAX || patchStart > UINT32_MAX || baseInstance > UINT32_MAX ||
        patchCount - 1 > UINT32_MAX - patchStart || instanceCount - 1 > UINT32_MAX - baseInstance) {
        [_cb rdna4Fail:@"drawIndexedPatches range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:YES patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:controlPointIndexBuffer controlIndexOffset:controlPointIndexBufferOffset indirect:NO]) {
        vkCmdDraw([_cb vk], (uint32_t)patchCount, (uint32_t)instanceCount, (uint32_t)patchStart, (uint32_t)baseInstance);
    }
}
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (![self rdna4IndirectBuffer:indirectBuffer offset:indirectBufferOffset bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:YES patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:controlPointIndexBuffer controlIndexOffset:controlPointIndexBufferOffset indirect:YES]) {
        [_cb rdna4RetainEncoded:indirectBuffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)indirectBuffer vkBuffer], [(RDNA4Buffer *)indirectBuffer rdna4VkOffset] + indirectBufferOffset, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)endEncoding {
    if (_ended) return;
    if (_hostWriteDropped) { _ended = YES; [super endEncoding]; return; }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];
    if (!_begun && ![_cb rdna4Error]) { _ampLayerOff = 0u; [self rdna4BeginPass]; }   // a clear-only encoder still has to run its load action across all layers
    _ended = YES;
    if (_dcOn) {   // P5b: fold this encoder's counters into the device totals
        atomic_fetch_add(&RDNA4DO.draws, _dc.draws); atomic_fetch_add(&RDNA4DO.setsReused, _dc.setsReused); atomic_fetch_add(&RDNA4DO.setsAlloc, _dc.setsAllocated);
        atomic_fetch_add(&RDNA4DO.pipesSkipped, _dc.pipesSkipped); atomic_fetch_add(&RDNA4DO.pipesBound, _dc.pipesBound); atomic_fetch_add(&RDNA4DO.pcHits, _dc.pcHits);
    }
    if (_inPass) { [self rdna4EndPass]; }
    [self rdna4ResolveAll];   // bundle 10: multisample resolve, outside the pass
    rdna4_full_barrier([_cb vk]);
    if (_nCsbEnd) {
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            id<RDNA4CounterSampleBufferInternal> csb = _csbEnd[k];
            if (cbOk && csb && _csbEndV[k] != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:_csbEndV[k]
                                        barrier:NO stageFlags:VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                                            who:"render-pass-end-vertex"];
            if (cbOk && csb && _csbEndF[k] != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:_csbEndF[k]
                                        barrier:NO stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                            who:"render-pass-end-fragment"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    }
    [_cb rdna4ArgumentCheckpoint];
    [self rdna4ReleaseRenderHandles];
    RDNA4LOGR("RDNA4RenderEncoder endEncoding (render pass ended)");
    [super endEncoding];
}
@end

// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputeEncoder (11e R2)
// ---------------------------------------------------------------------------------------------------------------




@implementation RDNA4ComputeEncoder
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb]; if (!self) return nil;
    _cb = cb;
    _resourceBindings = [RDNA4ResourceBindings new];
    RDNA4LOGR("RDNA4ComputeEncoder %p", (__bridge void *)self);
    return self;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLComputePassDescriptor *)d {
    self = [self initWithCommandBuffer:cb compute:0];
    if (!self || !d) return self;
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLComputePassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLComputePassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfEncoderSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfEncoderSampleIndex)) {
                RDNA4LOG("compute pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                         (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLComputePassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfEncoderSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfEncoderSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"compute-pass-start"];
            if (att.endOfEncoderSampleIndex != MTLCounterDontSample && _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndIdx[_nCsbEnd] = att.endOfEncoderSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    return self;
}
RDNA4_DNR(RDNA4ComputeEncoder)
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index {
    [_resourceBindings setObject:object
                             key:rdna4_resource_slot_key(kind, fragment, index)];
}
- (void)rdna4SnapshotResources {
    [_resourceBindings snapshotToCommandBuffer:_cb];
}
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 2; }
- (NSUInteger)dispatchType { return 0; }
- (void)setComputePipelineState:(id)p {
    if (![p isKindOfClass:[RDNA4ComputePipelineState class]]) { [_cb rdna4Fail:@"setComputePipelineState: not an RDNA4ComputePipelineState"]; return; }
    _pso = p; _roReflection = [_pso reflection];
    _st.scanPlan = [_pso scanPlan];
    _st.rtAsRefl = [_pso rdna4RtAsRefl];
    [self rdna4OwnResource:p kind:RDNA4_RESOURCE_PIPELINE
                  fragment:NO index:0];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i >= 31 || (b && ![b isKindOfClass:[RDNA4Buffer class]])) { [_cb rdna4Fail:@"compute setBuffer: bad index or class"]; return; }
    _st.buf[i].b = b; _st.buf[i].off = o;
    _st.rt.as[i] = nil;
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
    [self rdna4OwnResource:b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o attributeStride:(NSUInteger)stride atIndex:(NSUInteger)i {
    if (stride != 0 && stride != (NSUInteger)-1)
        RDNA4LOG("compute setBuffer:attributeStride:%lu atIndex:%lu ignoring dynamic stride", (unsigned long)stride, (unsigned long)i);
    [self setBuffer:b offset:o atIndex:i];
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _st.buf[i].off = o; }
- (void)setBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:b[k] offset:o[k] atIndex:r.location + k]; }
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {
    if (i >= 31 || !p) { [_cb rdna4Fail:@"compute setBytes: bad index/bytes"]; return; }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off]) [self setBuffer:b offset:off atIndex:i];
}
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (i >= 31) { [_cb rdna4Fail:@"compute setVisibleFunctionTable: buffer index >= 31"]; return; }
    if (!t) { [self setBuffer:nil offset:0 atIndex:i]; return; }
    if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]] || [t device] != [_cb device]) {
        [_cb rdna4Fail:@"compute setVisibleFunctionTable: not an RDNA4VisibleFunctionTable for this device"];
        return;
    }
    RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
    [vft rdna4RetainIntoCommandBuffer:_cb];
    [self setBuffer:[vft rdna4BackingBuffer] offset:0 atIndex:i];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"compute setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:t[k] atBufferIndex:r.location + k];
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (!rdna4_rt_encoder_bind_accel(_cb, [_cb device], "compute", &_st.rt, accel, i)) return;
    _st.buf[i] = (RDNA4BufSlot){ nil, 0 };
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
    [self rdna4OwnResource:accel kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
}
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    RDNA4Buffer *backing = nil;
    if (!rdna4_rt_encoder_bind_ift(_cb, [_cb device], "compute", &_st.rt, t, i, &backing)) return;
    [self setBuffer:backing offset:0 atIndex:i];
}
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "compute", &_st.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self setBuffer:buf offset:0 atIndex:idx];
                                     });
}
- (void)setTexture:(id)t atIndex:(NSUInteger)i {
    if (i >= 128 || (t && ![t isKindOfClass:[RDNA4Texture class]])) { [_cb rdna4Fail:@"compute setTexture: bad index or class"]; return; }
    _st.tex[i] = t;
    [self rdna4OwnResource:t kind:RDNA4_RESOURCE_TEXTURE fragment:NO index:i];
}
- (void)setTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setTexture:t[k] atIndex:r.location + k]; }
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                 override:(BOOL)override atIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (i >= 16 || (s && ![s isKindOfClass:[RDNA4SamplerState class]])) { [_cb rdna4Fail:@"compute setSamplerState: bad index or class"]; return; }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"compute sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4SamplerBinding *old = _st.smp[i];
    _st.smp[i] = binding;
    if (binding) {
        if (!_samplerBindings) _samplerBindings = [NSMutableSet set];
        [_samplerBindings addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < 16 && !used; ++k) used = _st.smp[k] == old;
        if (!used) [_samplerBindings removeObject:old];
    }
}
- (void)setSamplerState:(id)s atIndex:(NSUInteger)i {
    [self setSamplerState:s lodMinClamp:0 lodMaxClamp:0 override:NO atIndex:i];
}
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self setSamplerState:s lodMinClamp:a lodMaxClamp:b override:YES atIndex:i]; }
- (void)setSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:s[k] atIndex:r.location + k]; }
- (void)setSamplerStates:(const id __unsafe_unretained *)s
             lodMinClamps:(const float *)lo
             lodMaxClamps:(const float *)hi
                withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"compute setSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES atIndex:r.location + k];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= RDNA4_ICB_MAX_TG_MEM_INDICES) {
        RDNA4LOG("compute setThreadgroupMemoryLength at index %lu exceeds limit %u", (unsigned long)index, RDNA4_ICB_MAX_TG_MEM_INDICES);
        return;
    }
    _tgMemLengths[index] = length;
    if (index == 0) _threadgroupBytes = length;
}
- (void)setStageInRegion:(MTLRegion)region {
    _stageInRegion = region;
    _hasStageInRegion = YES;
}
- (void)rdna4ICBComputeBarrier {
    VkCommandBuffer cmd = [_cb vk];
    if (!cmd) return;
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)memoryBarrierWithScope:(NSUInteger)s { (void)s; rdna4_full_barrier([_cb vk]); }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n { (void)r; (void)n; rdna4_full_barrier([_cb vk]); }
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_ended) { [_cb rdna4Fail:@"compute sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"compute sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"compute"];
}
RDNA4_ENCODER_SHARED
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

// grid: threads (dispatchThreads) or groups*tpt (dispatchThreadgroups). The Metal call's threadsPerThreadgroup must equal the local size the
// module was translated with (meta local_size); otherwise the command buffer completes with an NSError instead of dispatching.
- (void)rdna4Dispatch:(MTLSize)grid tpt:(MTLSize)tpt exactThreads:(BOOL)exact indirect:(RDNA4Buffer *)indirect offset:(NSUInteger)offset {
    if (_ended) { [_cb rdna4Fail:@"dispatch after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_pso) { [_cb rdna4Fail:@"dispatch without a compute pipeline state"]; return; }
    { RDNA4ComputePipelineState *r = [_pso rdna4Real];   // hot-swap (see the render encoder)
      if (r) {
          _pso = r;
          _st.scanPlan = [_pso scanPlan];
          _st.rtAsRefl = [_pso rdna4RtAsRefl];
          [self rdna4OwnResource:r kind:RDNA4_RESOURCE_PIPELINE
                        fragment:NO index:0];
          RDNA4LOGR("HOT-SWAP: compute encoder now uses the real pipeline %p", (__bridge void *)r);
      } }
    MTLComputePipelineReflection *reflection = [_pso reflection] ?: _roReflection;
    [self rdna4SnapshotResources];
    if (rdna4nc_stage_writes(&_st, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE), RDNA4NC_COMPUTE, "compute dispatch", &_roWarn)) {
        [_cb rdna4Fail:@"compute dispatch writes a read-only no-copy buffer"];
        return;
    }
    if ([_pso noop]) { RDNA4LOGR("COMPUTE FALLBACK: dispatch %lux%lux%lu (tpt %lux%lux%lu) skipped, placeholder pipeline encodes nothing", (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth, (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth); return; }
    const NSUInteger threadgroupBytes = [_pso rdna4LumaWorkgroup] ? _threadgroupBytes : 0;
    if (_threadgroupBytes && !threadgroupBytes) RDNA4LOGR("compute dispatch uses fixed threadgroup storage (requested %lu bytes at index 0)", (unsigned long)_threadgroupBytes);
    if (threadgroupBytes && !rdna4_luma_workgroup_bytes_ok(threadgroupBytes, rdna4_device_caps().shared_memory)) {
        [_cb rdna4Fail:@"luma threadgroup memory length is not a whole number of float4 elements or exceeds the device limit"];
        return;
    }
    uint32_t nom[3] = { [_pso nominalLocal][0], [_pso nominalLocal][1], [_pso nominalLocal][2] };
    if ([_pso mode] == 1) {
        // #12 R2: a ThreadsDynamic module declares its local size through spec ids 0..2 (checked: every kernel in spvcache), so the
        // dispatch's own threadsPerThreadgroup is the nominal local size; a pipeline variant per (x,y,z) is built lazily and cached.
        rdna4dc_caps caps = rdna4_device_caps();
        if (!rdna4dc_workgroup_ok(&caps, tpt.width, tpt.height, tpt.depth)) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu is zero or outside the RADV limits (per axis %u,%u,%u; invocations %u)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth,
                          caps.workgroup_size[0], caps.workgroup_size[1], caps.workgroup_size[2], caps.invocations]]; return; }
        if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2])
            RDNA4LOGR("compute dispatch: threadsPerThreadgroup %lux%lux%lu overrides the translated local size %ux%ux%u (spec ids 0..2)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]);
        nom[0] = (uint32_t)tpt.width; nom[1] = (uint32_t)tpt.height; nom[2] = (uint32_t)tpt.depth;
    } else if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu != the SPIR-V local size %ux%ux%u the kernel was translated with (metal2vulkan --local; this module is Workgroups, local size is not specialisable)",
                      (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]]]; return; }
    if (threadgroupBytes && !rdna4_luma_workgroup_fits(threadgroupBytes, nom)) {
        [_cb rdna4Fail:@"luma threadgroup memory requires one float4 per invocation"];
        return;
    }
    uint32_t g[3] = { (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth };
    if (!indirect && (!g[0] || !g[1] || !g[2])) return;
    id dev = [_cb device];
    const RDNA4ArgumentTables *argTables = [_pso argumentTables];
    const BOOL needArgPrep =
        (argTables && argTables->active) ||
        [_pso rdna4HasAddressRegistry] ||
        (_st.scanPlan && _st.scanPlan->plan.count) ||
        _argumentResources.count != 0 ||
        _argumentWritable.count != 0 ||
        [_cb rdna4HasResidency] ||
        rdna4_rt_any_arg_owners();
    NSMutableSet *argumentResources = nil;
    if (needArgPrep) {
        argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pb], [_pso npb], &_st, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE));
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[] = {&_st};
        NSString *scanError = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, 1, argumentWritable, dev,
                                         &scanError)) {
            [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
            return;
        }
        BOOL argumentsReady = [_cb rdna4PrepareArguments:argTables
            resources:argumentResources writable:argumentWritable scan:&scan
            textures:^(RDNA4Texture *t, VkImageLayout layout, BOOL writable) {
            rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE);
            rdna4_tex_to([self->_cb vk], t, layout);
        }];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!argumentsReady) return;
    }
    rdna4_each_tex([_pso pb], [_pso npb], &_st, dev, ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) { rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE); rdna4_tex_to([self->_cb vk], t, l); });
    if (rdna4_icb_any_arg_owners() || rdna4_rt_any_arg_owners()) {
        NSMutableSet *icbRefs = [NSMutableSet set];
        NSMutableSet *icbPsoRefs = [NSMutableSet set];
        for (uint32_t i = 0; i < 31; i++) {
            if (_st.buf[i].b) {
                rdna4_argbuf_icb_snapshot(_st.buf[i].b, icbRefs, icbPsoRefs);
                rdna4_rt_retain_argbuf_resources(_st.buf[i].b, _cb);
            }
        }
        for (id res in _argumentResources) {
            if ([res isKindOfClass:[RDNA4Buffer class]])
                rdna4_argbuf_icb_snapshot((RDNA4Buffer *)res, icbRefs, icbPsoRefs);
        }
        for (id res in argumentResources) {
            if ([res isKindOfClass:[RDNA4Buffer class]])
                rdna4_rt_retain_argbuf_resources((RDNA4Buffer *)res, _cb);
        }
        for (id pso in icbPsoRefs) {
            [_cb rdna4RetainEncoded:pso];
        }
        for (id<RDNA4ICBInternal> icb in icbRefs) {
            if ([icb device] != dev) {
                [_cb rdna4Fail:@"compute dispatch references indirect command buffer from another device"];
                return;
            }
            if (![icb rdna4PrepareForGPUProducerOnCommandBuffer:_cb]) return;
            [_cb rdna4RetainEncoded:icb];
            id backing = [icb rdna4GPUBacking];
            if (backing) [_cb rdna4RetainEncoded:backing];
            for (id pso in icbPsoRefs) {
                if ([icb respondsToSelector:@selector(rdna4IsComputeICB)]) {
                    BOOL wantCompute = [(id<RDNA4ComputeICBInternal>)icb rdna4IsComputeICB];
                    BOOL isComputePso = [pso isKindOfClass:[RDNA4ComputePipelineState class]];
                    if (wantCompute != isComputePso && icbRefs.count > 1) continue;
                }
                if (![icb rdna4RegisterCandidatePipeline:pso]) {
                    [_cb rdna4Fail:@"indirect command buffer exceeds 32 candidate pipelines or pipeline type mismatch"];
                    return;
                }
            }
        }
    }
    VkCommandBuffer cmd = [_cb vk];
    VkDescriptorSet s = VK_NULL_HANDLE;
    if ([_pso npb]) { s = [_cb rdna4AllocSet:[_pso dsl]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pb] count:[_pso npb] state:&_st stage:"compute"]) return; }
    [_cb rdna4BindArguments:[_pso argumentTables] point:VK_PIPELINE_BIND_POINT_COMPUTE layout:[_pso layout]];
    if (indirect || [_pso mode] == 0) {   // Whole workgroups, with CPU or GPU dispatch counts.
        if (exact && (g[0] % nom[0] || g[1] % nom[1] || g[2] % nom[2])) { [_cb rdna4Fail:@"dispatchThreads with a partial boundary threadgroup needs a ThreadsDynamic translation (this module is Workgroups)"]; return; }
        VkPipeline pipeline = [_pso pipelineForLocal:nom indirect:indirect != nil threadgroupBytes:threadgroupBytes];
        if (!pipeline) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        if (indirect) {
            rdna4_full_barrier(cmd);
            vkCmdDispatchIndirect(cmd, [indirect vkBuffer], [indirect rdna4VkOffset] + offset);
        } else vkCmdDispatch(cmd, exact ? g[0] / nom[0] : g[0], exact ? g[1] / nom[1] : g[1], exact ? g[2] / nom[2] : g[2]);
    } else {                  // ThreadsDynamic: the metal2vulkan region plan, one dispatch per region
        RDNA4Region reg[8]; uint32_t threads[3], tgpg[3];
        for (int d = 0; d < 3; d++) { threads[d] = exact ? g[d] : g[d] * nom[d]; tgpg[d] = (threads[d] + nom[d] - 1) / nom[d]; }
        int nr = rdna4_plan(threads, nom, reg);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        for (int i = 0; i < nr; i++) {
            VkPipeline p = [_pso pipelineForLocal:reg[i].local indirect:NO threadgroupBytes:threadgroupBytes]; if (!p) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
            uint32_t words[12] = { threads[0], threads[1], threads[2], reg[i].tbase[0], reg[i].tbase[1], reg[i].tbase[2], reg[i].gbase[0], reg[i].gbase[1], reg[i].gbase[2], tgpg[0], tgpg[1], tgpg[2] };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdPushConstants(cmd, [_pso layout], VK_SHADER_STAGE_COMPUTE_BIT, [_pso pcOffset], sizeof words, words);
            vkCmdDispatch(cmd, reg[i].groups[0], reg[i].groups[1], reg[i].groups[2]);
        }
        RDNA4LOGR("compute dispatch: threads %u,%u,%u local %u,%u,%u -> %d region(s)", threads[0], threads[1], threads[2], nom[0], nom[1], nom[2], nr);
    }
    rdna4_full_barrier(cmd);
    [_cb rdna4ArgumentCheckpoint];
}
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self rdna4Dispatch:g tpt:t exactThreads:NO indirect:nil offset:0]; }
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self rdna4Dispatch:g tpt:t exactThreads:YES indirect:nil offset:0]; }
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)t {
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || offset % 4 || offset > [buffer length] || sizeof(VkDispatchIndirectCommand) > [buffer length] - offset) {
        [_cb rdna4Fail:@"indirect compute argument buffer range or alignment is invalid"]; return;
    }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)buffer rdna4VkOffset] + offset, 4, RDNA4NC_ALIGN_INDIRECT)) {
        [_cb rdna4Fail:@"indirect compute Vulkan offset must be a multiple of 4"]; return;
    }
    [_cb rdna4RetainEncoded:buffer];
    [self rdna4Dispatch:MTLSizeMake(0,0,0) tpt:t exactThreads:NO indirect:buffer offset:offset];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange {
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer after endEncoding or after an earlier encode error"];
        return;
    }
    if (!indirectCommandBuffer || ![indirectCommandBuffer respondsToSelector:@selector(rdna4IsComputeICB)]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    id<RDNA4ComputeICBInternal> icb = (id<RDNA4ComputeICBInternal>)indirectCommandBuffer;
    if (![icb rdna4IsComputeICB]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer cannot execute a render indirect command buffer"];
        return;
    }
    if ([icb rdna4UseGPUExecution]) {
        [icb rdna4ExecuteComputeGPUOnEncoder:self
                                 directRange:executionRange
                              indirectBuffer:nil
                              indirectOffset:0
                                 useIndirect:NO];
        return;
    }
    NSString *problem = nil;
    RDNA4ComputePipelineState *parentPipeline = _pso;
    MTLComputePipelineReflection *parentReflection = _roReflection;
    RDNA4StageState parentState = _st;
    NSUInteger parentTgBytes = _threadgroupBytes;
    NSUInteger parentTgLengths[RDNA4_ICB_MAX_TG_MEM_INDICES];
    memcpy(parentTgLengths, _tgMemLengths, sizeof parentTgLengths);
    MTLRegion parentStageIn = _stageInRegion;
    BOOL parentHasStageIn = _hasStageInRegion;
    [_cb rdna4RetainEncoded:icb];
    NSUInteger replayed = [icb rdna4ExecuteComputeOnEncoder:self range:executionRange reason:&problem];
    _pso = parentPipeline;
    _roReflection = parentReflection;
    _st = parentState;
    [self rdna4OwnResource:_pso kind:RDNA4_RESOURCE_PIPELINE fragment:NO index:0];
    for (NSUInteger i = 0; i < 31; ++i) {
        [self rdna4OwnResource:_st.buf[i].b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
        [self rdna4OwnResource:_st.rt.as[i] kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
    }
    _threadgroupBytes = parentTgBytes;
    memcpy(_tgMemLengths, parentTgLengths, sizeof _tgMemLengths);
    _stageInRegion = parentStageIn;
    _hasStageInRegion = parentHasStageIn;
    if (problem) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"compute executeCommandsInBuffer [%lu,+%lu): %@",
                        (unsigned long)executionRange.location, (unsigned long)executionRange.length, problem]];
        return;
    }
    RDNA4LOGR("compute executeCommandsInBuffer: replayed %lu command(s)", (unsigned long)replayed);
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer
                 indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer
           indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: after endEncoding or after an earlier encode error"];
        return;
    }
    if (!indirectCommandBuffer || ![indirectCommandBuffer respondsToSelector:@selector(rdna4IsComputeICB)]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    id<RDNA4ComputeICBInternal> icb = (id<RDNA4ComputeICBInternal>)indirectCommandBuffer;
    if (![icb rdna4IsComputeICB]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: cannot execute a render indirect command buffer"];
        return;
    }
    [icb rdna4ExecuteComputeGPUOnEncoder:self
                             directRange:NSMakeRange(0, 0)
                          indirectBuffer:indirectRangeBuffer
                          indirectOffset:indirectBufferOffset
                             useIndirect:YES];
}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    rdna4_full_barrier([_cb vk]);
    if (_nCsbEnd) {
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            if (cbOk && _csbEnd[k] && _csbEndIdx[k] != MTLCounterDontSample)
                [_csbEnd[k] rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                           sampleIndex:_csbEndIdx[k] barrier:NO
                                            stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                   who:"compute-pass-end"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    }
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOGR("RDNA4ComputeEncoder endEncoding");
    [super endEncoding];
}
@end


// m11h9: one buffer<->image copy region for a level / slice (2D, 1D arrays) or a box (3D: baseArrayLayer 0, depth in the extent, bufferImageHeight from bytesPerImage).
// Vulkan specifies bufferRowLength and bufferImageHeight in TEXELS (for compressed formats, multiply block counts by the 4x4 block dimension).
static VkBufferImageCopy rdna4_bic(RDNA4Texture *t, NSUInteger off, NSUInteger bpr, NSUInteger bpi, NSUInteger level, NSUInteger slice, MTLOrigin o, MTLSize sz) {
    BOOL is3 = [t textureType] == MTLTextureType3D; uint32_t bpp = [t bytesPerPixel];
    uint32_t rowTexels = bpr && bpp ? (uint32_t)((bpr / bpp) * ([t isCompressed] ? 4u : 1u)) : 0u;
    uint32_t imgHeight = (is3 && sz.depth > 1 && bpr) ? (uint32_t)((bpi / bpr) * ([t isCompressed] ? 4u : 1u)) : 0u;
    return (VkBufferImageCopy){ .bufferOffset = off, .bufferRowLength = rowTexels, .bufferImageHeight = imgHeight,
        .imageSubresource = { [t rdna4CopyAspect], [t rdna4MipLevel:level], [t rdna4Slice:slice], 1 },   // bundle 10: colour, or the one depth / stencil aspect
        .imageOffset = { (int32_t)o.x, (int32_t)o.y, is3 ? (int32_t)o.z : 0 }, .imageExtent = { (uint32_t)sz.width, (uint32_t)sz.height, is3 ? (uint32_t)sz.depth : 1 } };
}
static BOOL rdna4_blit_buffer_layout(RDNA4Texture *texture, RDNA4Buffer *buffer,
    MTLSize size, NSUInteger offset, NSUInteger *rowStride, NSUInteger *imageStride,
    VkDeviceSize *vkOffset) {
    NSUInteger rowBytes, minimumImage;
    NSUInteger bpp = [texture bytesPerPixel];
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    if (!bpp || __builtin_mul_overflow(bw, bpp, &rowBytes)) return NO;
    NSUInteger row = *rowStride ?: rowBytes;
    NSUInteger depth = [texture textureType] == MTLTextureType3D ? size.depth : 1;
    NSUInteger image = 0;
    if (depth > 1) {
        if (!row || __builtin_mul_overflow(row, bh, &minimumImage)) return NO;
        image = *imageStride ?: minimumImage;
        if (image % row || (image / row) * (bc ? 4 : 1) > UINT32_MAX) return NO;
    }
    rdna4_texture_transfer transfer;
    if (row % bpp || (row / bpp) * (bc ? 4 : 1) > UINT32_MAX ||
        !rdna4_texture_transfer_layout(bw, bh, depth, bpp, row, image, &transfer) ||
        !rdna4_buffer_range(buffer.length, offset, transfer.span)) return NO;
    VkDeviceSize relocated;
    if (__builtin_add_overflow([buffer rdna4VkOffset], (VkDeviceSize)offset, &relocated) ||
        !rdna4nc_vk_offset_ok(relocated, [texture rdna4CopyAspect] == VK_IMAGE_ASPECT_COLOR_BIT ? bpp : 4,
            RDNA4NC_ALIGN_IMAGE)) return NO;
    *rowStride = row; *imageStride = image; *vkOffset = relocated;
    return YES;
}
// Exact buffer rows touched by VkCmdCopyImageToBuffer. Keeping rows separate avoids
// retiring sampler owners that live in destinationBytesPerRow gaps.
static NSArray<NSValue *> *rdna4_blit_texture_buffer_ranges(RDNA4Texture *texture, MTLSize size,
        NSUInteger offset, NSUInteger bytesPerRow, NSUInteger bytesPerImage,
        NSUInteger bufferLength, NSRange *envelope) {
    if (!envelope || !texture || !size.width || !size.height) return @[];
    BOOL is3 = [texture textureType] == MTLTextureType3D;
    NSUInteger depth = is3 ? size.depth : 1;
    if (!depth) return @[];
    if (offset > bufferLength) return nil;
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    NSUInteger pixelBytes = [texture bytesPerPixel], rowBytes = 0, minimumImageBytes = 0;
    if (!pixelBytes || __builtin_mul_overflow(bw, pixelBytes, &rowBytes)) return nil;
    NSUInteger rowStride = bytesPerRow ? bytesPerRow : rowBytes;
    if (rowStride < rowBytes || __builtin_mul_overflow(rowStride, bh,
                                                       &minimumImageBytes)) return nil;
    NSUInteger imageStride = depth > 1 && bytesPerImage ? bytesPerImage : minimumImageBytes;
    if (imageStride < minimumImageBytes) return nil;

    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSUInteger first = NSUIntegerMax, last = 0;
    for (NSUInteger z = 0; z < depth; ++z) {
        NSUInteger imageOffset = 0;
        if (__builtin_mul_overflow(z, imageStride, &imageOffset) ||
            imageOffset > NSUIntegerMax - offset) return nil;
        for (NSUInteger y = 0; y < bh; ++y) {
            NSUInteger rowOffset = 0, start = 0, end = 0;
            if (__builtin_mul_overflow(y, rowStride, &rowOffset) ||
                rowOffset > NSUIntegerMax - imageOffset - offset) return nil;
            start = offset + imageOffset + rowOffset;
            if (start > bufferLength || rowBytes > bufferLength - start ||
                __builtin_add_overflow(start, rowBytes, &end)) return nil;
            if (first == NSUIntegerMax) first = start;
            if (ranges.count && NSMaxRange(ranges.lastObject.rangeValue) == start) {
                NSRange merged = NSMakeRange(ranges.lastObject.rangeValue.location,
                    start + rowBytes - ranges.lastObject.rangeValue.location);
                ranges[ranges.count - 1] = [NSValue valueWithRange:merged];
            } else {
                [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, rowBytes)]];
            }
            last = end;
        }
    }
    if (!ranges.count) return ranges;
    *envelope = NSMakeRange(first, last - first);
    return ranges;
}
static BOOL rdna4_blit_region_ok(RDNA4Texture *t, MTLOrigin o, MTLSize sz, NSUInteger level, NSUInteger slice, const char *what) {
    return rdna4_region_ok(t, (MTLRegion){ o, sz }, level, slice, what);
}
static void rdna4_blit_note_overwrite(RDNA4CommandBuffer *cb, RDNA4Texture *texture,
        MTLOrigin origin, MTLSize size, NSUInteger level, NSUInteger slice) {
    if (level || slice || origin.x || origin.y || origin.z || size.depth != 1 ||
        [texture textureType] != MTLTextureType2D || [texture mipmapLevelCount] != 1 ||
        [texture rdna4Aspects] != VK_IMAGE_ASPECT_COLOR_BIT) return;
    [cb rdna4NoteSurfaceOverwrite:texture width:(uint32_t)size.width height:(uint32_t)size.height level:0];
}
@implementation RDNA4BlitEncoder
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    RDNA4LOGR("RDNA4BlitEncoder %p", (__bridge void *)self);
    return self;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLBlitPassDescriptor *)d {
    self = [self initWithCommandBuffer:cb blit:0];
    if (!self || !d) return self;
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLBlitPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLBlitPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfEncoderSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfEncoderSampleIndex)) {
                RDNA4LOG("blit pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                         (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLBlitPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfEncoderSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfEncoderSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"blit-pass-start"];
            if (att.endOfEncoderSampleIndex != MTLCounterDontSample && _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndIdx[_nCsbEnd] = att.endOfEncoderSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    return self;
}
RDNA4_DNR(RDNA4BlitEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 3; }
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    if (_ended) { [_cb rdna4Fail:@"texture-to-buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    RDNA4LOGR("blit copyFromTexture toBuffer: slice %lu level %lu origin %lu,%lu,%lu size %lux%lux%lu offset %lu bpr %lu bpi %lu", (unsigned long)ss, (unsigned long)sl,
           (unsigned long)so.x, (unsigned long)so.y, (unsigned long)so.z, (unsigned long)sz.width, (unsigned long)sz.height, (unsigned long)sz.depth, (unsigned long)doff, (unsigned long)bpr, (unsigned long)bpi);
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_TEXTURE_BUFFER, "copyFromTexture:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toBuffer: destination is read-only"]; return; }
    RDNA4Texture *t = src; RDNA4Buffer *b = dst;
    [_cb rdna4RetainArgument:t]; [_cb rdna4RetainArgument:b];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: a combined depth/stencil texture or a multisample texture cannot be copied to a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, so, sz, sl, ss, "blit copyFromTexture:toBuffer:")) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, b, sz, doff, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:[t rdna4TexelBuffer] sourceOffset:[t bufferOffset] + so.x * [t bytesPerPixel]
            toBuffer:b destinationOffset:doff size:sz.width * [t bytesPerPixel]];
        return;
    }
    NSRange argumentRange = NSMakeRange(0, 0);
    NSArray<NSValue *> *argumentRanges = rdna4_blit_texture_buffer_ranges(t, sz, doff, bpr, bpi,
                                                                           [b length], &argumentRange);
    if (!argumentRanges) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: buffer layout overflows or exceeds the destination"];
        return;
    }
    if (argumentRanges.count) {
        // Texture contents are opaque to CPU argument scanning. A mapped
        // destination is refined from its final bytes; an unmapped destination
        // must conservatively retain the owners it had before this copy.
        RDNA4ArgumentWriteIntent *intent =
            rdna4_argument_sampler_intent_ranges(b, argumentRange, argumentRanges, nil, NO);
        if (!intent) {
            [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: argument-write intent is invalid"];
            return;
        }
        [_cb rdna4ArgumentIntent:intent];
    }
    rdna4_ios_use(_cb, t, YES, NO);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, sl, ss, so, sz);
    vkCmdCopyImageToBuffer(cmd, [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), [b vkBuffer], 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"buffer-to-texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_BUFFER_TEXTURE, "copyFromBuffer:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toTexture: destination is read-only"]; return; }
    RDNA4Texture *t = dst; [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:t];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: a combined depth/stencil texture or a multisample texture cannot be filled from a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, dorg, sz, dl, ds, "blit copyFromBuffer:toTexture:")) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, src, sz, so, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:src sourceOffset:so toBuffer:[t rdna4TexelBuffer]
            destinationOffset:[t bufferOffset] + dorg.x * [t bytesPerPixel] size:sz.width * [t bytesPerPixel]];
        return;
    }
    if (![(RDNA4Buffer *)src rdna4Imported]) rdna4_blit_note_overwrite(_cb, t, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, t, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, dl, ds, dorg, sz);
    vkCmdCopyBufferToImage(cmd, [(RDNA4Buffer *)src vkBuffer], [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &bic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_TEXTURE, "copyFromTexture:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toTexture: destination is read-only"]; return; }
    RDNA4Texture *a = src, *b = dst; [_cb rdna4RetainArgument:a]; [_cb rdna4RetainArgument:b];
    if ([a rdna4Aspects] != [b rdna4Aspects] || [a rdna4Samples] != [b rdna4Samples]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: the textures differ in aspects (colour / depth / stencil) or sample count"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(a, so, sz, sl, ss, "blit copyFromTexture:toTexture: (source)") || !rdna4_blit_region_ok(b, dorg, sz, dl, ds, "blit copyFromTexture:toTexture: (destination)")) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: region outside a texture level"]; return; }
    if ([a rdna4IsTexel] || [b rdna4IsTexel]) {
        if (a.pixelFormat != b.pixelFormat) { [_cb rdna4Fail:@"texture-buffer copy requires identical pixel formats"]; return; }
        NSUInteger row = sz.width * [a bytesPerPixel];
        if ([a rdna4IsTexel]) {
            [self copyFromBuffer:[a rdna4TexelBuffer] sourceOffset:[a bufferOffset] + so.x * [a bytesPerPixel]
                sourceBytesPerRow:row sourceBytesPerImage:0 sourceSize:sz toTexture:b
                destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
        } else {
            [self copyFromTexture:a sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz
                toBuffer:[b rdna4TexelBuffer] destinationOffset:[b bufferOffset] + dorg.x * [b bytesPerPixel]
                destinationBytesPerRow:row destinationBytesPerImage:0];
        }
        return;
    }
    BOOL sameImage = [a vkImage] == [b vkImage];
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if (sameImage && sourceMip == destinationMip && sourceSlice == destinationSlice && so.x < dorg.x + sz.width && dorg.x < so.x + sz.width &&
        so.y < dorg.y + sz.height && dorg.y < so.y + sz.height && so.z < dorg.z + sz.depth && dorg.z < so.z + sz.depth) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: source and destination overlap"]; return;
    }
    rdna4_ios_use(_cb, a, YES, NO);
    IOSurfaceRef sourceSurface = [a rdna4UCSurface], destinationSurface = [b rdna4UCSurface];
    if (!sameImage && destinationSurface && (sourceSurface ? IOSurfaceGetID(sourceSurface) != IOSurfaceGetID(destinationSurface)
                                                          : ![a rdna4IsIOS]))
        rdna4_blit_note_overwrite(_cb, b, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, b, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    BOOL a3 = [a textureType] == MTLTextureType3D, b3 = [b textureType] == MTLTextureType3D;
    VkImageLayout sourceLayout = rdna4_use_layout(a, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageLayout destinationLayout = rdna4_use_layout(b, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rdna4_tex_to(cmd, a, sourceLayout);
    rdna4_tex_to(cmd, b, destinationLayout);
    VkImageCopy ic = { .srcSubresource = { [a rdna4Aspects], sourceMip, sourceSlice, 1 }, .srcOffset = { (int32_t)so.x, (int32_t)so.y, a3 ? (int32_t)so.z : 0 },
        .dstSubresource = { [b rdna4Aspects], destinationMip, destinationSlice, 1 }, .dstOffset = { (int32_t)dorg.x, (int32_t)dorg.y, b3 ? (int32_t)dorg.z : 0 },
        .extent = { (uint32_t)sz.width, (uint32_t)sz.height, (a3 || b3) ? (uint32_t)sz.depth : 1 } };
    vkCmdCopyImage(cmd, [a vkImage], sourceLayout, [b vkImage], destinationLayout, 1, &ic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    if (_ended) { [_cb rdna4Fail:@"buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_BUFFER, "copyFromBuffer:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toBuffer: destination is read-only"]; return; }
    RDNA4Buffer *a = src, *b = dst;
    if (so > a.length || size > a.length - so || doff > b.length || size > b.length - doff || ((so | doff | size) & 3)) {
        [_cb rdna4Fail:@"blit buffer copy range or macOS alignment is invalid"]; return;
    }
    if (!size) return;
    VkDeviceSize srcOffset = [a rdna4VkOffset] + so, dstOffset = [b rdna4VkOffset] + doff;
    if ([a vkBuffer] == [b vkBuffer] && srcOffset < dstOffset + size && dstOffset < srcOffset + size) {
        [_cb rdna4Fail:@"blit buffer copy source and destination overlap"]; return;
    }
    if ([a rdna4Imported] && [b rdna4Imported]) {
        uintptr_t ap = (uintptr_t)a.contents + so, bp = (uintptr_t)b.contents + doff;
        if (ap < bp ? bp - ap < size : ap - bp < size) {
            [_cb rdna4Fail:@"blit buffer copy imported source and destination overlap"]; return;
        }
    }
    [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:dst];
    RDNA4ArgumentWriteIntent *copyIntent = rdna4_argument_sampler_copy(a, so, b, doff, size);
    if (!copyIntent) {
        [_cb rdna4Fail:@"blit buffer copy argument-write intent is invalid"];
        return;
    }
    [_cb rdna4ArgumentIntent:copyIntent];
    uint64_t ao = 0, bo = 0;
    VkDeviceMemory am = [a rdna4NativeMemory:&ao], bm = [b rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .src = am, .dst = bm,
        .srcOffset = ao <= UINT64_MAX - so ? ao + so : UINT64_MAX,
        .dstOffset = bo <= UINT64_MAX - doff ? bo + doff : UINT64_MAX, .size = size }];
    // Vulkan buffer copies allow the byte offsets introduced by an unaligned import.
    VkBufferCopy bc = { srcOffset, dstOffset, size };
    vkCmdCopyBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)src vkBuffer], [(RDNA4Buffer *)dst vkBuffer], 1, &bc);
    rdna4_full_barrier([_cb rdna4BlitVK]);
}
- (void)fillBuffer:(id)buf range:(NSRange)r value:(uint8_t)v {
    if (_ended) { [_cb rdna4Fail:@"buffer fill after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![buf isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit fillBuffer: bad class"]; return; }
    if (rdna4nc_drop_write(buf, RDNA4NC_FILL, "fillBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"fillBuffer: destination is read-only"]; return; }
    if (r.location > [buf length] || r.length > [buf length] - r.location) {
        [_cb rdna4Fail:@"blit fillBuffer: range outside the buffer"]; return;
    }
    if (!r.length) return;
    [_cb rdna4RetainArgument:buf];
    [_cb rdna4ArgumentWrite:buf range:r candidates:nil fillOnly:YES];
    uint32_t w = v * 0x01010101u;
    uint64_t bo = 0; VkDeviceMemory bm = [(RDNA4Buffer *)buf rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .dst = bm,
        .dstOffset = bo <= UINT64_MAX - r.location ? bo + r.location : UINT64_MAX,
        .size = r.length, .pattern = w, .fill = true }];
    VkDeviceSize dstOffset = [(RDNA4Buffer *)buf rdna4VkOffset] + r.location;
    NSUInteger head = MIN(r.length, (4 - (dstOffset & 3)) & 3);
    NSUInteger tail = (r.length - head) & 3, middle = r.length - head - tail;
    RDNA4Buffer *edge = nil; NSUInteger offset = 0;
    // Metal accepts byte ranges; Vulkan fill only accepts whole aligned words.
    if ((head || tail) && ![_cb rdna4Bytes:&w length:sizeof w buffer:&edge offset:&offset]) return;
    if (middle) vkCmdFillBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)buf vkBuffer], dstOffset + head, middle, w);
    VkBufferCopy edges[2]; uint32_t count = 0;
    if (head) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset, head };
    if (tail) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset + head + middle, tail };
    if (count) vkCmdCopyBuffer([_cb vk], [edge vkBuffer], [(RDNA4Buffer *)buf vkBuffer], count, edges);
    rdna4_full_barrier([_cb rdna4BlitVK]);
}
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_ended) { [_cb rdna4Fail:@"blit sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"blit sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    // Use [_cb vk] (not rdna4BlitVK) so the native blit fast path cannot bypass timestamp writes.
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"blit"];
}
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
                inRange:(NSRange)range
      destinationBuffer:(id<MTLBuffer>)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset {
    if (_ended) { [_cb rdna4Fail:@"blit resolveCounters after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], 0, NO, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"blit resolveCounters: %@", why ?: @"invalid"]];
        return;
    }
    if (!rdna4cnt_range_ok((uint64_t)[csb sampleCount], (uint64_t)range.location, (uint64_t)range.length)) {
        [_cb rdna4Fail:@"blit resolveCounters: range out of bounds"];
        return;
    }
    if (![destinationBuffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)destinationBuffer device] != [_cb device]) {
        [_cb rdna4Fail:@"blit resolveCounters: destinationBuffer is invalid"];
        return;
    }
    if (rdna4nc_drop_write(destinationBuffer, RDNA4NC_COPY_BUFFER, "resolveCounters:", "BlitCommandEncoder")) {
        [_cb rdna4Fail:@"blit resolveCounters: destination is read-only"];
        return;
    }
    RDNA4Buffer *dst = (RDNA4Buffer *)destinationBuffer;
    const uint64_t bytes = (uint64_t)range.length * sizeof(uint64_t);
    if ((destinationOffset & 3u) || !rdna4_buffer_range(dst.length, destinationOffset, bytes)) {
        [_cb rdna4Fail:@"blit resolveCounters: destinationBuffer range or alignment is invalid"];
        return;
    }
    [_cb rdna4RetainEncoded:csb];
    [_cb rdna4RetainEncoded:dst];
    [_cb rdna4ArgumentWrite:dst range:NSMakeRange(destinationOffset, (NSUInteger)bytes) candidates:nil fillOnly:YES];
    NSError *queueErr = nil;
    if (![_cb rdna4EncodeOrderedHostAction:^BOOL(NSError **actionErr) {
        uint64_t *tmp = (uint64_t *)malloc((size_t)bytes);
        if (!tmp) {
            if (actionErr) *actionErr = rdna4_err(66, @"blit resolveCounters: out of memory");
            return NO;
        }
        BOOL ok = [csb rdna4ResolveRange:range intoBytes:tmp error:actionErr];
        if (ok) {
            if ([dst contents]) {
                memcpy((uint8_t *)[dst contents] + destinationOffset, tmp, (size_t)bytes);
            } else {
                RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:[dst device]
                                                                length:(NSUInteger)bytes
                                                               options:MTLResourceStorageModeShared
                                                                 error:actionErr];
                if (!stg || ![stg contents]) {
                    if (actionErr && !*actionErr) *actionErr = rdna4_err(66, @"blit resolveCounters: staging buffer allocation failed");
                    ok = NO;
                } else {
                    memcpy([stg contents], tmp, (size_t)bytes);
                    BOOL unresolved = NO;
                    ok = rdna4_oneshot(actionErr, ^(VkCommandBuffer cmd) {
                        VkBufferCopy bc = { [stg rdna4VkOffset], [dst rdna4VkOffset] + destinationOffset, bytes };
                        vkCmdCopyBuffer(cmd, [stg vkBuffer], [dst vkBuffer], 1u, &bc);
                        rdna4_full_barrier(cmd);
                    }, &unresolved);
                    if (unresolved) {
                        (void)CFBridgingRetain(stg);
                        (void)CFBridgingRetain(dst);
                    }
                }
            }
        }
        free(tmp);
        return ok;
    } error:&queueErr]) {
        if (![_cb rdna4Error]) [_cb rdna4Fail:queueErr.localizedDescription ?: @"blit resolveCounters failed"];
    }
}
RDNA4_ENCODER_SHARED
// m11h9: a box-filtered chain, level L-1 -> L with vkCmdBlitImage (linear filter when RADV can filter the format, else nearest) inside one GENERAL-layout image (source and
// destination are different levels, so the regions never overlap); each blit is ordered after the previous by a full memory barrier.
- (void)generateMipmapsForTexture:(id)tex {
    if (![tex isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: bad class"]; return; }
    if (rdna4nc_drop_write(tex, RDNA4NC_MIPMAPS, "generateMipmapsForTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"generateMipmapsForTexture: destination is read-only"]; return; }
    RDNA4Texture *t = tex; uint32_t nl = (uint32_t)[t mipmapLevelCount];
    if ([t rdna4IsDS] || [t rdna4Samples] > 1 || [t isCompressed]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: depth/stencil, multisample and compressed textures cannot be mip-mapped here"]; return; }   // bundle 10
    if (nl <= 1) { RDNA4_ONCE("generateMipmapsForTexture: the texture has a single level; nothing to generate"); return; }
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, [t vkFormat], &fp);
    if ((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) != (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"generateMipmapsForTexture: vk format %d cannot be blitted on RADV (features 0x%x)", [t vkFormat], fp.optimalTilingFeatures]]; return; }
    VkFilter fl = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    [_cb rdna4RetainArgument:t];
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t L = 1; L < nl; L++) {
        rdna4_full_barrier(cmd);
        int32_t sw = (int32_t)MAX((NSUInteger)1, [t width] >> (L - 1)), sh = (int32_t)MAX((NSUInteger)1, [t height] >> (L - 1)), sd = (int32_t)MAX((NSUInteger)1, [t depth] >> (L - 1));
        int32_t dw = (int32_t)MAX((NSUInteger)1, [t width] >> L), dh = (int32_t)MAX((NSUInteger)1, [t height] >> L), dd = (int32_t)MAX((NSUInteger)1, [t depth] >> L);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L - 1], [t rdna4Slice:0], [t rdna4Layers] }, .srcOffsets = { { 0, 0, 0 }, { sw, sh, sd } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L], [t rdna4Slice:0], [t rdna4Layers] }, .dstOffsets = { { 0, 0, 0 }, { dw, dh, dd } } };
        vkCmdBlitImage(cmd, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, fl);
    }
    rdna4_full_barrier(cmd);
    RDNA4LOG("blit generateMipmapsForTexture: %u levels of %lux%lux%lu (vk format %d, %s filter)", nl, (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t depth], [t vkFormat], fl == VK_FILTER_LINEAR ? "linear" : "nearest");
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id)dst
      destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit texture range copy: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger al = [a mipmapLevelCount], bl = [b mipmapLevelCount], as = [a rdna4Layers], bs = [b rdna4Layers];
    if (a.pixelFormat != b.pixelFormat || [a rdna4Samples] != [b rdna4Samples]) {
        [_cb rdna4Fail:@"blit texture range copy: pixel format or sample count differs"]; return;
    }
    if (sl > al || lc > al - sl || dl > bl || lc > bl - dl || ss > as || sc > as - ss || ds > bs || sc > bs - ds) {
        [_cb rdna4Fail:@"blit texture range copy: mip or slice range outside a texture"]; return;
    }
    if (!sc || !lc) return;
    if (MAX((NSUInteger)1, a.width >> sl) != MAX((NSUInteger)1, b.width >> dl) ||
        MAX((NSUInteger)1, a.height >> sl) != MAX((NSUInteger)1, b.height >> dl) ||
        MAX((NSUInteger)1, a.depth >> sl) != MAX((NSUInteger)1, b.depth >> dl)) {
        [_cb rdna4Fail:@"blit texture range copy: starting mip dimensions differ"]; return;
    }
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if ([a vkImage] == [b vkImage] && sourceSlice < destinationSlice + sc && destinationSlice < sourceSlice + sc &&
        sourceMip < destinationMip + lc && destinationMip < sourceMip + lc) {
        [_cb rdna4Fail:@"blit texture range copy: source and destination overlap"]; return;
    }
    for (NSUInteger level = 0; level < lc; ++level) {
        MTLSize size = MTLSizeMake(MAX((NSUInteger)1, a.width >> (sl + level)),
            MAX((NSUInteger)1, a.height >> (sl + level)), MAX((NSUInteger)1, a.depth >> (sl + level)));
        for (NSUInteger slice = 0; slice < sc; ++slice)
            [self copyFromTexture:a sourceSlice:ss + slice sourceLevel:sl + level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
                toTexture:b destinationSlice:ds + slice destinationLevel:dl + level destinationOrigin:MTLOriginMake(0,0,0)];
    }
}
- (void)copyFromTexture:(id)src toTexture:(id)dst {
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger sl = 0, dl = 0;
    BOOL matched = NO;
    for (NSUInteger level = 0; level < a.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, a.width >> level) == b.width && MAX((NSUInteger)1, a.height >> level) == b.height &&
            MAX((NSUInteger)1, a.depth >> level) == b.depth) { sl = level; matched = YES; break; }
    }
    if (!matched) for (NSUInteger level = 1; level < b.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, b.width >> level) == a.width && MAX((NSUInteger)1, b.height >> level) == a.height &&
            MAX((NSUInteger)1, b.depth >> level) == a.depth) { dl = level; matched = YES; break; }
    }
    if (!matched) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: no mip matches the smaller texture"]; return; }
    [self copyFromTexture:a sourceSlice:0 sourceLevel:sl toTexture:b destinationSlice:0 destinationLevel:dl
        sliceCount:MIN([a rdna4Layers], [b rdna4Layers]) levelCount:MIN(a.mipmapLevelCount - sl, b.mipmapLevelCount - dl)];
}
// the MTLBlitOption variants: option 0 (none) = the plain call; PVRTC row-linear options are not supported
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toBuffer:dst destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture:options: blit options are not supported"]; return; }
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)synchronizeResource:(id)r { (void)r; }   // Managed resources: host-visible coherent memory, nothing to flush
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    if (_nCsbEnd) {
        rdna4_full_barrier([_cb vk]);
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            if (cbOk && _csbEnd[k] && _csbEndIdx[k] != MTLCounterDontSample)
                [_csbEnd[k] rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                           sampleIndex:_csbEndIdx[k] barrier:NO
                                            stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                   who:"blit-pass-end"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    } else {
        rdna4_full_barrier([_cb rdna4BlitVK]);
    }
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOG("RDNA4BlitEncoder endEncoding");
    [super endEncoding];
}
@end

