// garnet_texdesc.h (bundle 9, app crash study item 2): the pure mapping from a Metal texture descriptor to the Vulkan image, so cube and 2D-array textures can exist.
// Pure C (no Metal / Vulkan headers: the numeric values are asserted against the real enums in GarnetDevice.m with _Static_assert), host-tested by test-texdesc.c and test-vkimage.c.
// an internal design note, "App crash study", item (2): VectorKit (Photos, Maps) creates MTLTextureTypeCube textures and aborts on nil.
//
// Rule that binds this file: every descriptor the bundle accepted before build 9 (2D, 1D, 1DArray, 3D) maps EXACTLY as before; test-texdesc.c sweeps a grid against a verbatim copy of the old check.
#ifndef RDNA4_TEXDESC_H
#define RDNA4_TEXDESC_H
#include <stdint.h>
#include <stddef.h>

// MTLTextureType values (asserted in GarnetDevice.m)
enum { RDNA4TD_MTL_1D = 0, RDNA4TD_MTL_1DARRAY = 1, RDNA4TD_MTL_2D = 2, RDNA4TD_MTL_2DARRAY = 3, RDNA4TD_MTL_2DMS = 4, RDNA4TD_MTL_CUBE = 5, RDNA4TD_MTL_CUBEARRAY = 6, RDNA4TD_MTL_3D = 7, RDNA4TD_MTL_2DMSARRAY = 8, RDNA4TD_MTL_BUFFER = 9 };
// MTLTextureUsage bits
enum { RDNA4TD_USE_SHADERWRITE = 2, RDNA4TD_USE_RENDERTARGET = 4 };
// VkImageType / VkImageViewType values and the create flag (asserted in GarnetDevice.m)
enum { RDNA4TD_VK_IMAGE_1D = 0, RDNA4TD_VK_IMAGE_2D = 1, RDNA4TD_VK_IMAGE_3D = 2 };
enum { RDNA4TD_VK_VIEW_1D = 0, RDNA4TD_VK_VIEW_2D = 1, RDNA4TD_VK_VIEW_3D = 2, RDNA4TD_VK_VIEW_CUBE = 3, RDNA4TD_VK_VIEW_1D_ARRAY = 4, RDNA4TD_VK_VIEW_2D_ARRAY = 5, RDNA4TD_VK_VIEW_CUBE_ARRAY = 6 };
#define RDNA4TD_VK_CREATE_CUBE_COMPATIBLE 0x10u
// texture kind stored by the texture object; 0 is the plain 2D texture so a zero-initialised object (IOSurface-, buffer-backed and view textures) stays 2D
enum { RDNA4TD_K_2D = 0, RDNA4TD_K_1D = 1, RDNA4TD_K_1DARRAY = 2, RDNA4TD_K_3D = 3, RDNA4TD_K_CUBE = 4, RDNA4TD_K_2DARRAY = 5, RDNA4TD_K_2DMS = 6, RDNA4TD_K_CUBEARRAY = 7, RDNA4TD_K_2DMSARRAY = 8 };
#define RDNA4TD_CUBE_FACES 6u

typedef struct {
    int ok;                 // 1: the descriptor is accepted
    const char *why;        // refusal text (ok == 0)
    unsigned kind;          // RDNA4TD_K_*
    unsigned imageType;     // RDNA4TD_VK_IMAGE_*
    unsigned viewType;      // RDNA4TD_VK_VIEW_*
    unsigned layers;        // VkImageCreateInfo.arrayLayers and the sampling view's layerCount
    unsigned flags;         // extra VkImageCreateFlags (cube-compatible)
    unsigned layersIvar;    // the value the texture stores in _layers (0 = not a layered type: rdna4Layers() is then 1)
    unsigned samples;       // bundle 10: VkImageCreateInfo.samples (1 for everything but a 2D multisample texture)
    unsigned levels;        // mip levels (0 -> 1)
    unsigned depth;         // depth (0 -> 1)
    int is1D, is3D;
} rdna4td_t;

// d_* are the raw descriptor fields.  usage is MTLTextureUsage.  Returns 1 when accepted (and fills *o), else 0 with o->why set.
static inline int rdna4td_map(unsigned long tt, unsigned long w, unsigned long h, unsigned long depthRaw, unsigned long arrayLength, unsigned long sampleCount,
                            unsigned long mipLevelsRaw, unsigned long usage, rdna4td_t *o) {
    unsigned long levels = mipLevelsRaw ? mipLevelsRaw : 1, depth = depthRaw ? depthRaw : 1;
    int is1D = tt == RDNA4TD_MTL_1D || tt == RDNA4TD_MTL_1DARRAY, is3D = tt == RDNA4TD_MTL_3D;
    o->ok = 0; o->why = NULL; o->kind = RDNA4TD_K_2D; o->imageType = RDNA4TD_VK_IMAGE_2D; o->viewType = RDNA4TD_VK_VIEW_2D; o->layers = 1; o->flags = 0; o->layersIvar = 0;
    o->levels = (unsigned)levels; o->depth = (unsigned)depth; o->is1D = is1D; o->is3D = is3D; o->samples = 1;
    if (tt == RDNA4TD_MTL_2DMSARRAY) {
        if (sampleCount < 2) { o->why = "a 2D multisample-array texture needs sampleCount >= 2"; return 0; }
        if (arrayLength < 1 || arrayLength > UINT32_MAX) { o->why = "a 2D multisample-array texture needs 1 <= arrayLength <= UINT32_MAX"; return 0; }
        if (depth != 1) { o->why = "a 2D multisample-array texture has depth 1"; return 0; }
        if (levels != 1) { o->why = "a 2D multisample-array texture has one mip level"; return 0; }
        if (usage & RDNA4TD_USE_SHADERWRITE) { o->why = "ShaderWrite on a multisample texture is not supported (a storage image cannot be multisampled here)"; return 0; }
        o->kind = RDNA4TD_K_2DMSARRAY; o->imageType = RDNA4TD_VK_IMAGE_2D; o->viewType = RDNA4TD_VK_VIEW_2D_ARRAY;
        o->layers = (unsigned)arrayLength; o->flags = 0; o->layersIvar = (unsigned)arrayLength; o->samples = (unsigned)sampleCount;
        o->ok = 1; return 1;
    }
    if (tt == RDNA4TD_MTL_2DMS) {   // bundle 10: MSAA colour / depth targets.  The sample count itself (2, 4, 8) is checked against the device in GarnetDevice.m (garnet_depth.h rdna4ms_texture_ok).
        if (sampleCount < 2) { o->why = "a 2D multisample texture needs sampleCount >= 2"; return 0; }
        if (arrayLength != 1) { o->why = "a 2D multisample texture has arrayLength 1 (use 2DMultisampleArray)"; return 0; }
        if (depth != 1) { o->why = "a 2D multisample texture has depth 1"; return 0; }
        if (levels != 1) { o->why = "a 2D multisample texture has one mip level"; return 0; }
        if (usage & RDNA4TD_USE_SHADERWRITE) { o->why = "ShaderWrite on a multisample texture is not supported (a storage image cannot be multisampled here)"; return 0; }
        o->kind = RDNA4TD_K_2DMS; o->imageType = RDNA4TD_VK_IMAGE_2D; o->viewType = RDNA4TD_VK_VIEW_2D; o->layers = 1; o->flags = 0; o->layersIvar = 0; o->samples = (unsigned)sampleCount;
        o->ok = 1; return 1;
    }
    if (tt == RDNA4TD_MTL_CUBE || tt == RDNA4TD_MTL_CUBEARRAY) {
        if (w != h) { o->why = "a cube texture must be square (width must equal height)"; return 0; }
        if (sampleCount != 1) { o->why = "multisampled cube textures are not supported"; return 0; }
        if (tt == RDNA4TD_MTL_CUBE && arrayLength != 1) { o->why = "a cube texture has arrayLength 1 (use CubeArray for multiple cubes)"; return 0; }
        if (arrayLength < 1 || arrayLength > UINT32_MAX / RDNA4TD_CUBE_FACES) { o->why = "cube-array layer count is zero or overflows Vulkan's layer count"; return 0; }
        if (depth != 1) { o->why = "a cube texture has depth 1"; return 0; }
        o->kind = tt == RDNA4TD_MTL_CUBE ? RDNA4TD_K_CUBE : RDNA4TD_K_CUBEARRAY;
        o->imageType = RDNA4TD_VK_IMAGE_2D; o->viewType = tt == RDNA4TD_MTL_CUBE ? RDNA4TD_VK_VIEW_CUBE : RDNA4TD_VK_VIEW_CUBE_ARRAY;
        o->layers = RDNA4TD_CUBE_FACES * (unsigned)arrayLength; o->flags = RDNA4TD_VK_CREATE_CUBE_COMPATIBLE; o->layersIvar = o->layers;
        o->ok = 1; return 1;
    }
    if (tt == RDNA4TD_MTL_2DARRAY) {
        if (sampleCount != 1) { o->why = "multisampled 2D-array textures are not supported"; return 0; }
        if (arrayLength < 1 || arrayLength > UINT32_MAX) { o->why = "a 2D-array texture needs 1 <= arrayLength <= UINT32_MAX"; return 0; }
        if (depth != 1) { o->why = "a 2D-array texture has depth 1"; return 0; }
        // A 2D-array is a legal storage image: VkImageType 2D, arrayLayers=N and a 2D_ARRAY view.
        // ShaderWrite still requires the format's STORAGE_IMAGE feature in GarnetDevice.m.
        o->kind = RDNA4TD_K_2DARRAY; o->imageType = RDNA4TD_VK_IMAGE_2D; o->viewType = RDNA4TD_VK_VIEW_2D_ARRAY; o->layers = (unsigned)arrayLength;
        o->flags = w == h ? RDNA4TD_VK_CREATE_CUBE_COMPATIBLE : 0; o->layersIvar = (unsigned)arrayLength;
        o->ok = 1; return 1;
    }
    // ---- the types accepted before build 9: this block is the old check, unchanged ----
    if (!(tt == RDNA4TD_MTL_2D || is1D || is3D) || (is3D ? arrayLength != 1 : depth != 1) || sampleCount != 1 ||
        (tt != RDNA4TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (is1D && levels != 1)) {
        o->why = "only single-sample 2D (mips), 3D (mips), cube (mips), 2D-array and single-level 1D / 1DArray textures"; return 0; }
    o->kind = tt == RDNA4TD_MTL_1DARRAY ? RDNA4TD_K_1DARRAY : tt == RDNA4TD_MTL_1D ? RDNA4TD_K_1D : is3D ? RDNA4TD_K_3D : RDNA4TD_K_2D;
    o->imageType = is1D ? RDNA4TD_VK_IMAGE_1D : is3D ? RDNA4TD_VK_IMAGE_3D : RDNA4TD_VK_IMAGE_2D;
    o->viewType = is1D ? (tt == RDNA4TD_MTL_1DARRAY ? RDNA4TD_VK_VIEW_1D_ARRAY : RDNA4TD_VK_VIEW_1D) : is3D ? RDNA4TD_VK_VIEW_3D : RDNA4TD_VK_VIEW_2D;
    o->layers = is1D ? (unsigned)arrayLength : 1; o->layersIvar = is1D ? (unsigned)arrayLength : 0; o->flags = 0;
    o->ok = 1; return 1;
}

// The texture's own layer count (what rdna4Layers returns for a root texture): barriers, region checks, mip blits, readbacks.
static inline unsigned rdna4td_nlayers(unsigned layersIvar) { return layersIvar ? layersIvar : 1; }
// What textureType / arrayLength report.
static inline unsigned rdna4td_mtl_type(unsigned kind) {
    switch (kind) { case RDNA4TD_K_1D: return RDNA4TD_MTL_1D; case RDNA4TD_K_1DARRAY: return RDNA4TD_MTL_1DARRAY; case RDNA4TD_K_3D: return RDNA4TD_MTL_3D; case RDNA4TD_K_CUBE: return RDNA4TD_MTL_CUBE; case RDNA4TD_K_CUBEARRAY: return RDNA4TD_MTL_CUBEARRAY; case RDNA4TD_K_2DARRAY: return RDNA4TD_MTL_2DARRAY; case RDNA4TD_K_2DMS: return RDNA4TD_MTL_2DMS; case RDNA4TD_K_2DMSARRAY: return RDNA4TD_MTL_2DMSARRAY; default: return RDNA4TD_MTL_2D; }
}
static inline unsigned long rdna4td_array_length(unsigned kind, unsigned layersIvar) {
    switch (kind) { case RDNA4TD_K_1D: case RDNA4TD_K_1DARRAY: case RDNA4TD_K_2DARRAY: case RDNA4TD_K_2DMSARRAY: return layersIvar; case RDNA4TD_K_CUBEARRAY: return layersIvar / RDNA4TD_CUBE_FACES; default: return 1; }
}
// Texture types that are an array of 2D images in Vulkan (cube and 2D arrays): their attachment views are per-layer 2D views.
static inline int rdna4td_is_layered2d(unsigned kind) { return kind == RDNA4TD_K_CUBE || kind == RDNA4TD_K_CUBEARRAY || kind == RDNA4TD_K_2DARRAY || kind == RDNA4TD_K_2DMSARRAY; }
typedef struct { unsigned kind, layersIvar, viewType; } rdna4td_view_t;
static inline int rdna4td_view_map(unsigned parentKind, unsigned parentLayers, unsigned long type,
                                  unsigned long baseSlice, unsigned long sliceCount, rdna4td_view_t *v) {
    if ((parentKind != RDNA4TD_K_2D && !rdna4td_is_layered2d(parentKind)) || !sliceCount ||
        baseSlice > parentLayers || sliceCount > parentLayers - baseSlice || sliceCount > UINT32_MAX) return 0;
    v->layersIvar = 0;
    if (type == RDNA4TD_MTL_2D && sliceCount == 1) {
        v->kind = RDNA4TD_K_2D; v->viewType = RDNA4TD_VK_VIEW_2D; return 1;
    }
    if (type == RDNA4TD_MTL_2DARRAY) {
        v->kind = RDNA4TD_K_2DARRAY; v->layersIvar = (unsigned)sliceCount; v->viewType = RDNA4TD_VK_VIEW_2D_ARRAY; return 1;
    }
    if (type == RDNA4TD_MTL_CUBE && sliceCount == RDNA4TD_CUBE_FACES && baseSlice % RDNA4TD_CUBE_FACES == 0) {
        v->kind = RDNA4TD_K_CUBE; v->layersIvar = RDNA4TD_CUBE_FACES; v->viewType = RDNA4TD_VK_VIEW_CUBE; return 1;
    }
    if (type == RDNA4TD_MTL_CUBEARRAY && sliceCount % RDNA4TD_CUBE_FACES == 0 && baseSlice % RDNA4TD_CUBE_FACES == 0) {
        v->kind = RDNA4TD_K_CUBEARRAY; v->layersIvar = (unsigned)sliceCount; v->viewType = RDNA4TD_VK_VIEW_CUBE_ARRAY; return 1;
    }
    return 0;
}
// VkImageSubresourceLayers.baseArrayLayer for a Metal slice (3D textures put the depth in the extent: layer 0).
static inline uint32_t rdna4td_base_layer(int is3D, unsigned long slice) { return is3D ? 0u : (uint32_t)slice; }

// The single-level view for an attachment / storage image of (level, layer).  Old kinds: the sampling view type with layerCount = layers, base 0, and only layer 0 exists.
// Cube and 2D array: a plain 2D view of one layer.  Returns 0 when the layer does not exist.
typedef struct { unsigned viewType, baseLayer, layerCount; } rdna4td_att_t;
static inline int rdna4td_att_view(unsigned kind, unsigned layersIvar, unsigned long layer, rdna4td_att_t *a) {
    unsigned n = rdna4td_nlayers(layersIvar);
    if (layer >= n) return 0;
    if (rdna4td_is_layered2d(kind)) { a->viewType = RDNA4TD_VK_VIEW_2D; a->baseLayer = (unsigned)layer; a->layerCount = 1; return 1; }
    a->viewType = kind == RDNA4TD_K_1DARRAY ? RDNA4TD_VK_VIEW_1D_ARRAY : kind == RDNA4TD_K_1D ? RDNA4TD_VK_VIEW_1D : kind == RDNA4TD_K_3D ? RDNA4TD_VK_VIEW_3D : RDNA4TD_VK_VIEW_2D;
    a->baseLayer = 0; a->layerCount = (kind == RDNA4TD_K_1D || kind == RDNA4TD_K_1DARRAY) ? layersIvar : 1; return 1;
}
// Heap sizing: the layer count the pixel bytes are multiplied by.  (The old code: 1DArray = arrayLength, everything else 1.)
static inline unsigned long rdna4td_heap_layers(unsigned long tt, unsigned long arrayLength) {
    return tt == RDNA4TD_MTL_1DARRAY ? arrayLength : tt == RDNA4TD_MTL_2DARRAY ? arrayLength : tt == RDNA4TD_MTL_2DMSARRAY ? arrayLength : tt == RDNA4TD_MTL_CUBE ? RDNA4TD_CUBE_FACES : tt == RDNA4TD_MTL_CUBEARRAY ? RDNA4TD_CUBE_FACES * arrayLength : 1;
}
#endif
