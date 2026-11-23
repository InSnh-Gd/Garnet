// ---------------------------------------------------------------------------------------------------------------
// Common helpers for the 10b-10d classes.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_DNR(cls) \
    - (void)doesNotRecognizeSelector:(SEL)sel { \
        RDNA4LOG("UNRECOGNIZED selector %s on " #cls, sel_getName(sel)); \
        [super doesNotRecognizeSelector:sel]; }

static uint64_t rdna4_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// flags: RDNA4F_A8 = Metal A8Unorm stored as VK R8_UNORM; sampling goes through a view with components (0,0,0,R) (Metal returns (0,0,0,a)); a render target of this
// format is refused (a Vulkan attachment view must be identity, so a fragment's alpha cannot be steered into the R8 image).
#define RDNA4F_A8   1u
#define RDNA4F_SINT 8u
#define RDNA4F_UINT 4u   // bundle 19: an unsigned-integer colour format (RG16Uint): clears fill .uint32, the magenta fallback is masked off, /private/tmp/rdna4m-nouint restores the old refusal. (2u is RDNA4F_DS below.)
#define RDNA4F_BC   16u  // 4x4 block-compressed format; bpp holds bytes per 4x4 block (8 or 16)
typedef struct { MTLPixelFormat mtl; VkFormat vk; uint32_t bpp; uint32_t flags; } RDNA4Fmt;
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
static inline BOOL rdna4_fmt_is_bc(const RDNA4Fmt *f) { return f != NULL && (f->flags & RDNA4F_BC) != 0; }
static inline void rdna4_int_bits(MTLPixelFormat pf, unsigned *rgb, unsigned *a) {
    switch (pf) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
        *rgb = 8; *a = 8; return;
    case MTLPixelFormatRGB10A2Uint:
        *rgb = 10; *a = 2; return;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
        *rgb = 32; *a = 32; return;
    default:
        *rgb = 16; *a = 16; return;
    }
}
#define RDNA4_NOUINT_FILE "/private/tmp/rdna4m-nouint"   // bundle 19: exists -> the integer formats are refused as before (nil), read ONCE per process
static BOOL rdna4_nouint(void) {
    static BOOL v; static dispatch_once_t once;
    dispatch_once(&once, ^{ struct stat st; v = stat(RDNA4_NOUINT_FILE, &st) == 0; if (v) RDNA4LOG("integer colour formats (RG16Uint) OFF: kill file " RDNA4_NOUINT_FILE " exists (read once per process)"); });
    return v;
}
// The component mapping of a view of format f whose client swizzle is swz: client channel -> (format's own mapping of that channel). Identity for every format but A8.
static VkComponentMapping rdna4_view_map(const RDNA4Fmt *f, MTLTextureSwizzleChannels swz) {
    static const VkComponentSwizzle base[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
    static const VkComponentSwizzle a8[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
    const VkComponentSwizzle *m = (f && (f->flags & RDNA4F_A8)) ? a8 : base;
    unsigned c[4] = { swz.red, swz.green, swz.blue, swz.alpha };
    VkComponentSwizzle o[4]; for (int i = 0; i < 4; i++) o[i] = c[i] < 6 ? m[c[i]] : VK_COMPONENT_SWIZZLE_IDENTITY;
    return (VkComponentMapping){ o[0], o[1], o[2], o[3] };
}
static BOOL rdna4_is_identity_map(const VkComponentMapping *m) {
    return (m->r == VK_COMPONENT_SWIZZLE_R || m->r == VK_COMPONENT_SWIZZLE_IDENTITY) && (m->g == VK_COMPONENT_SWIZZLE_G || m->g == VK_COMPONENT_SWIZZLE_IDENTITY) &&
           (m->b == VK_COMPONENT_SWIZZLE_B || m->b == VK_COMPONENT_SWIZZLE_IDENTITY) && (m->a == VK_COMPONENT_SWIZZLE_A || m->a == VK_COMPONENT_SWIZZLE_IDENTITY);
}
static const RDNA4Fmt *rdna4_fmt(MTLPixelFormat f) {
    for (size_t i = 0; i < sizeof rdna4_fmts / sizeof *rdna4_fmts; i++) if (rdna4_fmts[i].mtl == f) return ((rdna4_fmts[i].flags & (RDNA4F_UINT | RDNA4F_SINT)) && rdna4_nouint()) ? NULL : &rdna4_fmts[i];
    return NULL;
}
// bundle 10: the depth / stencil formats live in their OWN table, so rdna4_fmt (colour targets, IOSurface- and buffer-backed textures, views, heaps' colour sizing) answers for them exactly as before: NULL.
// Depth24Unorm_Stencil8 is not in it (AMD has no D24S8 on this path).  The numbers are tied to rdna4_depth.h by the asserts below.
#define RDNA4F_DS 2u
static const RDNA4Fmt rdna4_dsfmts[] = {
    { MTLPixelFormatDepth16Unorm,          VK_FORMAT_D16_UNORM,          2, RDNA4F_DS },
    { MTLPixelFormatDepth32Float,          VK_FORMAT_D32_SFLOAT,         4, RDNA4F_DS },
    { MTLPixelFormatStencil8,              VK_FORMAT_S8_UINT,            1, RDNA4F_DS },
    { MTLPixelFormatDepth32Float_Stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, RDNA4F_DS },
};
static const RDNA4Fmt *rdna4_fmt_ds(MTLPixelFormat f) {
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
static unsigned rdna4_ms_mask(void) {
    if (!RDNA4R.ok) return 0;
    return RDNA4R.lim.framebufferColorSampleCounts & RDNA4R.lim.framebufferDepthSampleCounts & RDNA4R.lim.framebufferStencilSampleCounts;
}
// RADV's optimal-tiling features of a format (RADV must be open). 0 when the entry point is missing.
static VkFormatFeatureFlags rdna4_fmt_feats(const RDNA4Fmt *f) {
    VkFormatProperties fp = {0};
    if (vkGetPhysicalDeviceFormatProperties && f) vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, f->vk, &fp);
    return fp.optimalTilingFeatures;
}

static void rdna4_add_protocols(Class c, const char **names, unsigned n, const char *tag) {
    for (unsigned i = 0; i < n; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        BOOL added = p ? class_addProtocol(c, p) : NO;
        os_log(OS_LOG_DEFAULT, "GarnetMetal: +load %{public}s protocol %{public}s found %d added %d", tag, names[i], p != nil, added);
    }
}

@class RDNA4CommandBuffer;
@class RDNA4Texture;

