// test-texdesc.c: host test of rdna4_texdesc.h (bundle 9, app crash study item 2: cube and 2D-array textures), plus source pins that tie the REAL GarnetDevice.m to it.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-texdesc test-texdesc.c && /tmp/test-texdesc .
//   argv[1] = the directory holding GarnetDevice.m and rdna4_texdesc.h (default ".").
// The rule this file enforces: every descriptor the bundle accepted BEFORE build 9 (2D, 1D, 1DArray, 3D) maps EXACTLY as before.  The old mapping is reproduced below
// VERBATIM from build 8 (GarnetDevice.m RDNA4Texture initWithDevice:descriptor:error:, the check, VkImageType, arrayLayers, view type, _layers, textureType, arrayLength, heap layers)
// and a grid over every field is compared with the new function.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna4_texdesc.h"
static int fails, checks;
#define CHECK(c, ...) do { int ok_ = (c) ? 1 : 0; checks++; printf("%s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define QUIET(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- build 8, verbatim (types spelled with the numeric values: 1D 0, 1DArray 1, 2D 2, 3D 7) ----
typedef struct { int accepted, imageType, arrayLayers, viewType, layersIvar, mtlType; unsigned long arrayLengthReported, heapLayers; } old_t;
static old_t old_map(unsigned long tt, unsigned long w, unsigned long h, unsigned long dd, unsigned long al, unsigned long sc, unsigned long mip) {
    (void)w; (void)h; old_t o; memset(&o, 0, sizeof o);
    int is1D = tt == 0 || tt == 1, is3D = tt == 7; int arr1D = tt == 1;
    unsigned long levels = mip ? mip : 1, depth = dd ? dd : 1;
    if (!(tt == 2 || is1D || is3D) || (is3D ? al != 1 : depth != 1) || sc != 1 ||
        (tt != 1 && al != 1) || al < 1 || (is1D && levels != 1)) { o.accepted = 0; return o; }
    o.accepted = 1;
    o.imageType = is1D ? 0 : is3D ? 2 : 1;                                     // VK_IMAGE_TYPE_1D / 3D / 2D
    o.arrayLayers = is1D ? (int)al : 1;
    o.viewType = is1D ? (arr1D ? 4 : 0) : is3D ? 2 : 1;                        // 1D_ARRAY / 1D / 3D / 2D
    o.layersIvar = is1D ? (int)al : 0;                                         // _layers
    o.mtlType = is1D ? (arr1D ? 1 : 0) : is3D ? 7 : 2;                         // textureType
    o.arrayLengthReported = is1D ? (unsigned long)o.layersIvar : 1;             // arrayLength
    o.heapLayers = tt == 1 ? al : 1;                                           // rdna4_heap_tex_sa
    return o;
}
static int cube_ok(unsigned long w, unsigned long h, unsigned long dd, unsigned long al, unsigned long sc, unsigned long use, rdna4td_t *o) { return rdna4td_map(RDNA4TD_MTL_CUBE, w, h, dd, al, sc, 1, use, o); }

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); return b; }
static int count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }


int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    rdna4td_t t;
    // ---- the new types ----
    CHECK(cube_ok(64, 64, 1, 1, 1, 1, &t) && t.kind == RDNA4TD_K_CUBE && t.imageType == RDNA4TD_VK_IMAGE_2D && t.viewType == RDNA4TD_VK_VIEW_CUBE && t.layers == 6 && t.layersIvar == 6 && t.flags == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE,
          "cube 64x64: VkImageType 2D, view CUBE, 6 layers, flag CUBE_COMPATIBLE");
    CHECK(!cube_ok(64, 32, 1, 1, 1, 1, &t) && t.why, "cube with w != h -> refused (\"%s\")", t.why ? t.why : "");
    CHECK(!cube_ok(32, 64, 1, 1, 1, 1, &t), "cube with w < h -> refused");
    CHECK(rdna4td_map(RDNA4TD_MTL_CUBE, 64, 64, 1, 1, 1, 7, 1, &t) && t.levels == 7 && t.layers == 6, "cube with mips (7 levels of 64) -> accepted, still 6 layers");
    CHECK(!rdna4td_map(RDNA4TD_MTL_CUBE, 64, 64, 1, 1, 4, 1, 1, &t), "cube with MSAA (4 samples) -> refused");
    CHECK(!cube_ok(64, 64, 1, 6, 1, 1, &t), "cube with arrayLength 6 -> refused (an array of cubes is CubeArray)");
    CHECK(!cube_ok(64, 64, 1, 0, 1, 1, &t), "cube with arrayLength 0 -> refused");
    CHECK(!cube_ok(64, 64, 4, 1, 1, 1, &t), "cube with depth 4 -> refused");
    CHECK(cube_ok(64, 64, 0, 1, 1, 1, &t), "cube with depth 0 (the unset default) -> accepted");
    CHECK(cube_ok(64, 64, 1, 1, 1, RDNA4TD_USE_SHADERWRITE | 1, &t) && t.viewType == RDNA4TD_VK_VIEW_CUBE && t.layers == 6, "ShaderWrite on a cube retains the six-face cube view");
    CHECK(cube_ok(64, 64, 1, 1, 1, RDNA4TD_USE_RENDERTARGET | 1, &t), "RenderTarget on a cube -> accepted (per-layer 2D views)");
    CHECK(rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 2, 1, 7, 1, &t) && t.kind == RDNA4TD_K_CUBEARRAY &&
          t.imageType == RDNA4TD_VK_IMAGE_2D && t.viewType == RDNA4TD_VK_VIEW_CUBE_ARRAY && t.layers == 12 && t.layersIvar == 12 &&
          t.flags == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE && t.levels == 7 && rdna4td_array_length(t.kind, t.layersIvar) == 2 &&
          rdna4td_mtl_type(t.kind) == RDNA4TD_MTL_CUBEARRAY, "CubeArray x2: 12 layers, CUBE_ARRAY view, two reported cubes and seven mip levels");
    CHECK(rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 1, 1, 1, 1, &t) && t.viewType == RDNA4TD_VK_VIEW_CUBE_ARRAY && t.layers == 6,
          "CubeArray with one cube retains its array view type");
    CHECK(!rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 32, 1, 2, 1, 1, 1, &t) &&
          !rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 2, 2, 1, 1, 1, &t) &&
          !rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 2, 4, 1, 1, &t), "CubeArray rejects nonsquare, 3D and multisample descriptors");
    CHECK(!rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 0, 1, 1, 1, &t) &&
          !rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, (unsigned long)UINT32_MAX / 6 + 1, 1, 1, 1, &t), "CubeArray rejects zero and overflowing layer counts");
    CHECK(rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 2, 1, 1, RDNA4TD_USE_RENDERTARGET, &t) &&
          rdna4td_map(RDNA4TD_MTL_CUBEARRAY, 64, 64, 1, 2, 1, 1, RDNA4TD_USE_SHADERWRITE, &t) && t.viewType == RDNA4TD_VK_VIEW_CUBE_ARRAY && t.layers == 12, "CubeArray supports render targets and storage-image writes with twelve faces");
    { rdna4td_att_t a;
      CHECK(rdna4td_att_view(RDNA4TD_K_CUBEARRAY, 12, 7, &a) && a.viewType == RDNA4TD_VK_VIEW_2D && a.baseLayer == 7 && a.layerCount == 1 &&
            rdna4td_att_view(RDNA4TD_K_CUBEARRAY, 12, 11, &a) && !rdna4td_att_view(RDNA4TD_K_CUBEARRAY, 12, 12, &a), "CubeArray attachments address individual faces across cube boundaries"); }
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 5, 1, 1, 1, &t) && t.kind == RDNA4TD_K_2DARRAY && t.imageType == RDNA4TD_VK_IMAGE_2D && t.viewType == RDNA4TD_VK_VIEW_2D_ARRAY && t.layers == 5 && t.layersIvar == 5 && t.flags == 0,
          "2DArray 32x16 x5: VkImageType 2D, view 2D_ARRAY, 5 layers, no flags");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 32, 1, 6, 1, 1, 1, &t) && t.flags == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE,
          "square 2DArray opts into cube-compatible views");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 1, 1, 1, 1, &t) && t.viewType == RDNA4TD_VK_VIEW_2D_ARRAY && t.layers == 1, "2DArray with arrayLength 1 -> view type is still 2D_ARRAY");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 3, 1, 1, RDNA4TD_USE_SHADERWRITE, &t) &&
          t.kind == RDNA4TD_K_2DARRAY && t.imageType == RDNA4TD_VK_IMAGE_2D &&
          t.viewType == RDNA4TD_VK_VIEW_2D_ARRAY && t.layers == 3 && t.layersIvar == 3 && t.samples == 1,
          "ShaderWrite on a 2DArray -> accepted as a 2D image with a 3-layer 2D_ARRAY view");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 0, 1, 1, RDNA4TD_USE_SHADERWRITE, &t) &&
          !rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, (unsigned long)UINT32_MAX + 1, 1, 1, RDNA4TD_USE_SHADERWRITE, &t),
          "ShaderWrite 2DArray still refuses zero and >UINT32_MAX layer counts");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 3, 0, 1, RDNA4TD_USE_SHADERWRITE, &t) &&
          !rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 3, 2, 1, RDNA4TD_USE_SHADERWRITE, &t) &&
          !rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 3, 4, 1, RDNA4TD_USE_SHADERWRITE, &t),
          "ShaderWrite 2DArray still refuses sampleCount 0/2/4 (storage images are single-sample)");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 2, 3, 1, 1, RDNA4TD_USE_SHADERWRITE, &t),
          "ShaderWrite 2DArray still refuses depth != 1");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 0, 3, 1, 1, RDNA4TD_USE_SHADERWRITE, &t) && t.depth == 1,
          "ShaderWrite 2DArray accepts the unset depth default (0 -> 1)");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DARRAY, 32, 16, 1, 3, 1, 5, 1, &t) && t.levels == 5, "2DArray with mips -> accepted");
    // bundle 10: 2D multisample textures exist; the null factory also needs the array form
    CHECK(rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 1, 4, 1, 4, &t) && t.kind == RDNA4TD_K_2DMS && t.imageType == RDNA4TD_VK_IMAGE_2D && t.viewType == RDNA4TD_VK_VIEW_2D && t.layers == 1 && t.layersIvar == 0 && t.flags == 0 && t.samples == 4 && t.levels == 1,
          "2DMultisample 32x16, 4 samples, RenderTarget: VkImageType 2D, view 2D, 1 layer, no flags, samples 4");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 1, 8, 1, 5, &t) && t.samples == 8, "2DMultisample with 8 samples and ShaderRead|RenderTarget -> accepted by the descriptor map (the device decides 8)");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 1, 1, 1, 4, &t) && t.why && strstr(t.why, "sampleCount >= 2"), "2DMultisample with sampleCount 1 -> refused (\"%s\")", t.why ? t.why : "");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 2, 4, 1, 4, &t) && !rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 3, 1, 4, 1, 4, &t) && !rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 1, 4, 2, 4, &t), "2DMultisample with arrayLength 2, depth 3 or 2 mip levels -> refused");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DMS, 32, 16, 1, 1, 4, 1, RDNA4TD_USE_SHADERWRITE, &t) && t.why && strstr(t.why, "ShaderWrite"), "ShaderWrite on a multisample texture -> refused");
    CHECK(rdna4td_map(RDNA4TD_MTL_2DMSARRAY, 32, 16, 1, 2, 4, 1, 4, &t) && t.kind == RDNA4TD_K_2DMSARRAY && t.imageType == RDNA4TD_VK_IMAGE_2D && t.viewType == RDNA4TD_VK_VIEW_2D_ARRAY && t.layers == 2 && t.layersIvar == 2 && t.samples == 4, "2DMultisampleArray 2 layers, 4 samples: VkImageType 2D, 2D-array view");
    CHECK(!rdna4td_map(RDNA4TD_MTL_2DMSARRAY, 32, 16, 1, 1, 1, 1, 1, &t) && !rdna4td_map(RDNA4TD_MTL_2DMSARRAY, 32, 16, 1, 1, 4, 1, RDNA4TD_USE_SHADERWRITE, &t) && !rdna4td_map(RDNA4TD_MTL_2DMSARRAY, 32, 16, 1, 0, 4, 1, 1, &t), "2DMultisampleArray sample 1, ShaderWrite or zero layers -> refused");
    { rdna4td_att_t a; CHECK(rdna4td_att_view(RDNA4TD_K_2DMSARRAY, 2, 1, &a) && a.viewType == RDNA4TD_VK_VIEW_2D && a.baseLayer == 1 && a.layerCount == 1 && !rdna4td_att_view(RDNA4TD_K_2DMSARRAY, 2, 2, &a), "2DMSArray attachment view: layer 1 as a plain 2D view"); }
    CHECK(!rdna4td_map(RDNA4TD_MTL_BUFFER, 32, 16, 1, 1, 1, 1, 1, &t), "TextureBuffer -> still refused");
    CHECK(rdna4td_mtl_type(RDNA4TD_K_2DMS) == RDNA4TD_MTL_2DMS && rdna4td_array_length(RDNA4TD_K_2DMS, 0) == 1 && rdna4td_nlayers(0) == 1 && rdna4td_heap_layers(RDNA4TD_MTL_2DMS, 1) == 1, "2DMS kind reports textureType 2DMultisample, arrayLength 1, one layer");
    CHECK(rdna4td_mtl_type(RDNA4TD_K_2DMSARRAY) == RDNA4TD_MTL_2DMSARRAY && rdna4td_array_length(RDNA4TD_K_2DMSARRAY, 3) == 3 && rdna4td_heap_layers(RDNA4TD_MTL_2DMSARRAY, 3) == 3, "2DMSArray kind reports its array length");
    { rdna4td_att_t a; CHECK(rdna4td_att_view(RDNA4TD_K_2DMS, 0, 0, &a) && a.viewType == RDNA4TD_VK_VIEW_2D && a.baseLayer == 0 && a.layerCount == 1 && !rdna4td_att_view(RDNA4TD_K_2DMS, 0, 1, &a), "2DMS attachment view: a plain 2D view of layer 0"); }
    for (unsigned long tt = 0; tt < 12; tt++) { rdna4td_t u; if (tt == 4) continue; if (rdna4td_map(tt, 16, 16, 1, 1, 1, 1, 1, &u)) CHECK(u.samples == 1, "type %lu (not 2DMS): samples stay 1", tt); }
    CHECK(!rdna4td_map(99, 32, 16, 1, 1, 1, 1, 1, &t), "an unknown type -> refused");
    // helper functions
    CHECK(rdna4td_nlayers(0) == 1 && rdna4td_nlayers(1) == 1 && rdna4td_nlayers(6) == 6, "rdna4Layers: 0 -> 1, 6 -> 6");
    CHECK(rdna4td_mtl_type(RDNA4TD_K_CUBE) == RDNA4TD_MTL_CUBE && rdna4td_mtl_type(RDNA4TD_K_2DARRAY) == RDNA4TD_MTL_2DARRAY && rdna4td_mtl_type(RDNA4TD_K_2D) == RDNA4TD_MTL_2D && rdna4td_mtl_type(RDNA4TD_K_3D) == RDNA4TD_MTL_3D &&
          rdna4td_mtl_type(RDNA4TD_K_1D) == RDNA4TD_MTL_1D && rdna4td_mtl_type(RDNA4TD_K_1DARRAY) == RDNA4TD_MTL_1DARRAY, "textureType per kind (cube -> Cube, 2DArray -> 2DArray)");
    { rdna4td_view_t v;
      CHECK(rdna4td_view_map(RDNA4TD_K_2DARRAY, 6, RDNA4TD_MTL_2D, 2, 1, &v) && v.kind == RDNA4TD_K_2D && v.layersIvar == 0 && v.viewType == RDNA4TD_VK_VIEW_2D,
            "array slice 2 maps to a single-slice 2D view");
      CHECK(rdna4td_view_map(RDNA4TD_K_2D, 1, RDNA4TD_MTL_2DARRAY, 0, 1, &v) && v.kind == RDNA4TD_K_2DARRAY && v.layersIvar == 1,
            "single 2D image maps to a one-layer array view");
      CHECK(rdna4td_view_map(RDNA4TD_K_2DARRAY, 6, RDNA4TD_MTL_2DARRAY, 1, 4, &v) && v.kind == RDNA4TD_K_2DARRAY && v.layersIvar == 4 && v.viewType == RDNA4TD_VK_VIEW_2D_ARRAY,
            "array subrange maps to a view-local array layer count");
      CHECK(rdna4td_view_map(RDNA4TD_K_CUBEARRAY, 12, RDNA4TD_MTL_CUBE, 6, 6, &v) && v.kind == RDNA4TD_K_CUBE && v.layersIvar == 6 && v.viewType == RDNA4TD_VK_VIEW_CUBE,
            "aligned cube-array group maps to a cube view");
      CHECK(rdna4td_view_map(RDNA4TD_K_CUBE, 6, RDNA4TD_MTL_CUBEARRAY, 0, 6, &v) && v.kind == RDNA4TD_K_CUBEARRAY && v.layersIvar == 6 && v.viewType == RDNA4TD_VK_VIEW_CUBE_ARRAY,
            "cube maps to a one-cube cube-array view");
      CHECK(!rdna4td_view_map(RDNA4TD_K_2D, 1, RDNA4TD_MTL_CUBE, 0, 6, &v) &&
            !rdna4td_view_map(RDNA4TD_K_CUBEARRAY, 12, RDNA4TD_MTL_CUBE, 1, 6, &v) &&
            !rdna4td_view_map(RDNA4TD_K_CUBEARRAY, 12, RDNA4TD_MTL_CUBEARRAY, 0, 7, &v) &&
            !rdna4td_view_map(RDNA4TD_K_2DARRAY, 6, RDNA4TD_MTL_2DARRAY, 6, 1, &v),
            "view map refuses incompatible parents, misaligned cubes and out-of-range slices"); }
    CHECK(rdna4td_array_length(RDNA4TD_K_CUBE, 6) == 1 && rdna4td_array_length(RDNA4TD_K_2DARRAY, 5) == 5 && rdna4td_array_length(RDNA4TD_K_2D, 0) == 1, "arrayLength: cube 1, 2DArray 5, 2D 1");
    CHECK(rdna4td_base_layer(0, 5) == 5 && rdna4td_base_layer(1, 5) == 0 && rdna4td_base_layer(0, 0) == 0, "slice -> baseArrayLayer: 2D/cube/array slice 5 -> 5, 3D -> 0");
    CHECK(rdna4td_heap_layers(RDNA4TD_MTL_CUBE, 1) == 6 && rdna4td_heap_layers(RDNA4TD_MTL_2DARRAY, 4) == 4 && rdna4td_heap_layers(RDNA4TD_MTL_1DARRAY, 3) == 3 && rdna4td_heap_layers(RDNA4TD_MTL_2D, 1) == 1, "heap sizing counts the layers (cube 6, 2DArray 4, 1DArray 3, 2D 1)");
    { rdna4td_att_t a; rdna4td_t c; cube_ok(64, 64, 1, 1, 1, 1, &c);
      CHECK(rdna4td_att_view(RDNA4TD_K_CUBE, c.layersIvar, 3, &a) && a.viewType == RDNA4TD_VK_VIEW_2D && a.baseLayer == 3 && a.layerCount == 1, "cube attachment, layer 3: a 2D view, baseArrayLayer 3, layerCount 1");
      CHECK(rdna4td_att_view(RDNA4TD_K_CUBE, c.layersIvar, 5, &a) && a.baseLayer == 5 && !rdna4td_att_view(RDNA4TD_K_CUBE, c.layersIvar, 6, &a), "cube attachment: layer 5 exists, layer 6 does not (slice < rdna4Layers)");
      CHECK(rdna4td_att_view(RDNA4TD_K_2DARRAY, 4, 3, &a) && a.baseLayer == 3 && !rdna4td_att_view(RDNA4TD_K_2DARRAY, 4, 4, &a), "2DArray(4) attachment: layer 3 exists, layer 4 does not");
      CHECK(rdna4td_att_view(RDNA4TD_K_2D, 0, 0, &a) && a.viewType == RDNA4TD_VK_VIEW_2D && a.baseLayer == 0 && a.layerCount == 1 && !rdna4td_att_view(RDNA4TD_K_2D, 0, 1, &a), "plain 2D attachment: layer 0 only (the old slice != 0 refusal)");
      CHECK(rdna4td_att_view(RDNA4TD_K_1DARRAY, 3, 0, &a) && a.viewType == RDNA4TD_VK_VIEW_1D_ARRAY && a.layerCount == 3 && a.baseLayer == 0, "1DArray attachment view: as before (1D_ARRAY, all 3 layers, base 0)");
      CHECK(rdna4td_att_view(RDNA4TD_K_3D, 0, 0, &a) && a.viewType == RDNA4TD_VK_VIEW_3D && a.layerCount == 1 && rdna4td_att_view(RDNA4TD_K_1D, 1, 0, &a) && a.viewType == RDNA4TD_VK_VIEW_1D && a.layerCount == 1, "3D and 1D attachment views: as before"); }

    // ---- the identity proof: every previously accepted descriptor maps exactly as before ----
    { static const unsigned long tts[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 99 }, dims[] = { 1, 4, 16, 64, 100, 16384 }, deps[] = { 0, 1, 2, 5 }, als[] = { 0, 1, 2, 3, 6, 255 }, scs[] = { 0, 1, 2, 4 }, mips[] = { 0, 1, 2, 3, 7 }, uses[] = { 0, 1, 3, 4, 5, 0x10 };
      long total = 0, oldAcc = 0, same = 0, newAcc = 0, oldRefusedNewAccepted = 0, oldRefusedNewAcceptedNonNewTypes = 0;
      for (size_t a = 0; a < sizeof tts / sizeof *tts; a++) for (size_t b = 0; b < sizeof dims / sizeof *dims; b++) for (size_t c = 0; c < sizeof dims / sizeof *dims; c++) for (size_t d = 0; d < sizeof deps / sizeof *deps; d++)
        for (size_t e = 0; e < sizeof als / sizeof *als; e++) for (size_t f = 0; f < sizeof scs / sizeof *scs; f++) for (size_t g = 0; g < sizeof mips / sizeof *mips; g++) for (size_t u = 0; u < sizeof uses / sizeof *uses; u++) {
            unsigned long tt = tts[a], w = dims[b], h = dims[c], dd = deps[d], al = als[e], sc = scs[f], mp = mips[g], us = uses[u];
            old_t o = old_map(tt, w, h, dd, al, sc, mp); rdna4td_t n; int acc = rdna4td_map(tt, w, h, dd, al, sc, mp, us, &n); total++;
            if (acc) newAcc++;
            if (o.accepted) {
                oldAcc++;
                int ok = acc && n.samples == 1 && (int)n.imageType == o.imageType && (int)n.layers == o.arrayLayers && (int)n.viewType == o.viewType && (int)n.layersIvar == o.layersIvar && (int)n.flags == 0 &&
                         (int)rdna4td_mtl_type(n.kind) == o.mtlType && rdna4td_array_length(n.kind, n.layersIvar) == o.arrayLengthReported && rdna4td_heap_layers(tt, al) == o.heapLayers &&
                         rdna4td_nlayers(n.layersIvar) == (o.layersIvar ? (unsigned)o.layersIvar : 1u) && n.levels == (mp ? mp : 1) && n.depth == (dd ? dd : 1) && n.is1D == (tt == 0 || tt == 1) && n.is3D == (tt == 7);
                { rdna4td_att_t av; ok = ok && rdna4td_att_view(n.kind, n.layersIvar, 0, &av) && (int)av.viewType == o.viewType && av.baseLayer == 0 && (int)av.layerCount == (o.imageType == 0 ? o.arrayLayers : 1); }
                QUIET(ok, "old descriptor type %lu %lux%lu depth %lu array %lu samples %lu mips %lu usage %lx no longer maps identically", tt, w, h, dd, al, sc, mp, us);
                same += ok;
            } else if (acc) { oldRefusedNewAccepted++; if (!(tt == 3 || tt == 4 || tt == 5 || tt == 6 || tt == 8)) oldRefusedNewAcceptedNonNewTypes++; }
        }
      CHECK(oldAcc > 1000 && same == oldAcc, "identity sweep over %ld descriptors: the old build accepted %ld, %ld map identically (type, VkImageType, arrayLayers, view type, _layers, textureType, arrayLength, heap layers, attachment view)", total, oldAcc, same);
      CHECK(oldRefusedNewAcceptedNonNewTypes == 0, "newly accepted types are cube (5), CubeArray (6), 2DArray (3), 2DMultisample (4) and 2DMultisampleArray (8): %ld newly accepted, %ld outside those types", oldRefusedNewAccepted, oldRefusedNewAcceptedNonNewTypes);
      CHECK(newAcc > oldAcc, "the new function accepts more than the old (%ld vs %ld)", newAcc, oldAcc); }
    // spot values of the old set, written out (so a wrong transcription of the reference cannot hide a wrong header)
    CHECK(rdna4td_map(2, 100, 50, 1, 1, 1, 3, 7, &t) && t.viewType == 1 && t.imageType == 1 && t.layers == 1 && t.layersIvar == 0 && t.flags == 0, "2D 100x50 mips 3: VkImageType 2D, view 2D, 1 layer, _layers 0, no flags");
    CHECK(rdna4td_map(1, 16384, 1, 1, 3, 1, 1, 1, &t) && t.viewType == 4 && t.imageType == 0 && t.layers == 3 && t.layersIvar == 3, "1DArray 16384 x3 (the WindowServer LUT): VkImageType 1D, view 1D_ARRAY, 3 layers");
    CHECK(rdna4td_map(0, 100, 1, 1, 1, 1, 1, 1, &t) && t.viewType == 0 && t.imageType == 0 && t.layers == 1 && t.layersIvar == 1, "1D 100: VkImageType 1D, view 1D, 1 layer, _layers 1");
    CHECK(rdna4td_map(7, 8, 8, 4, 1, 1, 2, 1, &t) && t.viewType == 2 && t.imageType == 2 && t.layers == 1 && t.layersIvar == 0 && t.depth == 4, "3D 8x8x4 mips 2: VkImageType 3D, view 3D, 1 layer");
    CHECK(!rdna4td_map(0, 100, 1, 1, 1, 1, 2, 1, &t) && !rdna4td_map(2, 8, 8, 1, 2, 1, 1, 1, &t) && !rdna4td_map(2, 8, 8, 2, 1, 1, 1, 1, &t) && !rdna4td_map(2, 8, 8, 1, 1, 4, 1, 1, &t), "still refused: 1D with mips, 2D with arrayLength 2, 2D with depth 2, 2D with MSAA");

    // ---- source pins on the REAL GarnetDevice.m ----
    char *m = slurp(dir, "GarnetDevice.m");
    CHECK(m != NULL, "GarnetDevice.m is readable");
    if (m) {
        CHECK(strstr(m, "#include \"rdna4_texdesc.h\"") != NULL, "GarnetDevice.m includes rdna4_texdesc.h");
        const char *plan = strstr(m, "static BOOL rdna4_heap_image_plan_make(MTLTextureDescriptor *d, BOOL aliasable, rdna4_heap_image_plan *p, NSError **err)");
        const char *plan_end = plan ? strstr(plan, "\n@implementation RDNA4Texture") : NULL;
        CHECK(plan && plan_end, "the shared heap image plan is found");
        if (plan && plan_end) {
            CHECK(strstr(plan, "rdna4td_map((unsigned long)d.textureType, d.width, d.height, d.depth, d.arrayLength, d.sampleCount, d.mipmapLevelCount, (unsigned long)d.usage, &p->td)") != NULL,
                  "the image plan maps every descriptor field through rdna4td_map");
            CHECK(strstr(plan, "p->wantSt ? VK_IMAGE_USAGE_STORAGE_BIT : 0") != NULL,
                  "the image plan adds VK_IMAGE_USAGE_STORAGE_BIT for ShaderWrite");
            CHECK(strstr(plan, "VkImageCreateFlags icf = ((d.usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) | p->td.flags |") != NULL,
                  "the image plan keeps the mapped image-view flags");
            CHECK(strstr(plan, "vkGetPhysicalDeviceImageFormatProperties ? vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, p->fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp)") != NULL &&
                  strstr(plan, "p->td.layers > ifp.maxArrayLayers") != NULL && strstr(plan, "p->levels > ifp.maxMipLevels") != NULL,
                  "the image plan checks the mapped layer and mip counts against Vulkan limits");
            CHECK(strstr(plan, "p->create = (VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = vit, .format = p->fmt->vk,") != NULL &&
                  strstr(plan, ".mipLevels = (uint32_t)p->levels, .arrayLayers = p->td.layers, .samples = (VkSampleCountFlagBits)p->td.samples,") != NULL &&
                  strstr(plan, ".flags = icf, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .usage = iu };") != NULL,
                  "the image plan publishes the mapped VkImageCreateInfo (arrayLayers from p->td.layers)");
        }
        const char *init = strstr(m, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap error:(NSError **)err {");
        const char *init_end = init ? strstr(init, "\n// A single-level, identity-swizzle view") : NULL;
        CHECK(init && init_end, "the RDNA4Texture descriptor initialiser is found");
        if (init && init_end) {
            CHECK(strstr(m, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err {\n    return [self initWithDevice:dev descriptor:d heap:nil error:err];") != NULL,
                  "the public initialiser delegates to the heap-aware initialiser");
            CHECK(strstr(init, "rdna4_heap_image_plan plan;") != NULL &&
                  strstr(init, "rdna4_heap_image_plan_make(d, heap != nil, &plan, err)") != NULL &&
                  strstr(init, "rdna4td_t ti = plan.td;") != NULL,
                  "the initialiser uses the shared image plan without remapping the descriptor");
            CHECK(strstr(init, "VkImageCreateInfo ic = plan.create;") != NULL &&
                  strstr(init, "_imageFlags = ic.flags;") != NULL &&
                  strstr(init, "vkCreateImage(RDNA4R.dev, &ic, NULL, &_img)") != NULL,
                  "the initialiser creates the image from the plan's mapped create info");
            CHECK(strstr(init, ".viewType = (VkImageViewType)ti.viewType,") != NULL &&
                  strstr(init, "(uint32_t)_levels, 0, ti.layers }") != NULL,
                  "the sampling view type and layerCount come from the mapped plan");
            CHECK(strstr(init, "_layers = ti.layersIvar; _tdk = ti.kind;") != NULL, "_layers and the kind are stored from the header");
            CHECK(!strstr(init, "is1D ? (uint32_t)_layers : 1"), "no hand-written layer count is left in the initialiser");
            CHECK(strstr(init, "rdna4_requirements2(VK_NULL_HANDLE, _img, &mr, &prefersDedicated, &requiresDedicated, heap != nil)") != NULL,
                  "the initialiser queries image requirements2 and dedicated requirements");
            CHECK(strstr(init, "[heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]") != NULL &&
                  strstr(init, "_hpSize = (NSUInteger)mr.size;") != NULL,
                  "heap reservation uses the Vulkan image requirement size");
        }
        const char *size_align = strstr(m, "static BOOL rdna4_heap_image_size_align(MTLTextureDescriptor *d,");
        const char *size_align_end = size_align ? strstr(size_align, "\n// ---- m11 H1: heaps") : NULL;
        CHECK(size_align && size_align_end, "the heap TextureSizeAndAlign helper is found");
        if (size_align && size_align_end) {
            CHECK(strstr(size_align, "rdna4_heap_image_plan plan;") != NULL &&
                  strstr(size_align, "rdna4_heap_image_plan_make(d, YES, &plan, &error)") != NULL,
                  "TextureSizeAndAlign uses the same image plan as texture creation");
            CHECK(strstr(size_align, "vkCreateImage(RDNA4R.dev, &plan.create, NULL, &image)") != NULL &&
                  strstr(size_align, "rdna4_requirements2(VK_NULL_HANDLE, image, &mr, NULL, NULL, YES)") != NULL &&
                  strstr(size_align, "vkDestroyImage(RDNA4R.dev, image, NULL)") != NULL &&
                  strstr(size_align, "rdna4_heap_query_extent(mr, NO, size, align)") != NULL,
                  "TextureSizeAndAlign creates a temporary image, queries requirements2, destroys it and returns the Vulkan requirements");
            CHECK(!strstr(size_align, "rdna4td_heap_layers"), "TextureSizeAndAlign does not use the old tight pixel-byte estimate");
        }
        const char *requirements = strstr(m, "static BOOL rdna4_requirements2(VkBuffer buffer, VkImage image, VkMemoryRequirements *mr,");
        const char *requirements_end = requirements ? strstr(requirements, "\n// One source of truth for the heap buffer's create info.") : NULL;
        CHECK(requirements && requirements_end, "the shared requirements2 helper is found");
        if (requirements && requirements_end) {
            CHECK(strstr(requirements, "VkMemoryDedicatedRequirements dedicated") != NULL &&
                  strstr(requirements, "VkImageMemoryRequirementsInfo2 info = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image };") != NULL &&
                  strstr(requirements, "RDNA4P.imr2(RDNA4R.dev, &info, &req);") != NULL &&
                  strstr(requirements, "if (mr) *mr = req.memoryRequirements;") != NULL,
                  "requirements2 queries the real VkImageMemoryRequirements2");
            CHECK(strstr(requirements, "prefersDedicatedAllocation != VK_FALSE") != NULL &&
                  strstr(requirements, "requiresDedicatedAllocation != VK_FALSE") != NULL,
                  "requirements2 preserves both dedicated-allocation facts");
        }
        CHECK(strstr(m, "- (uint32_t)rdna4Layers { return rdna4td_nlayers((unsigned)_layers); }") != NULL &&
              strstr(m, "- (uint32_t)rdna4ImageLayers { return _root ? [_root rdna4ImageLayers] : rdna4td_nlayers((unsigned)_layers); }") != NULL,
              "visible view layers stay separate from whole-image transition layers");
        CHECK(strstr(m, "- (MTLTextureType)textureType { return (MTLTextureType)rdna4td_mtl_type(_tdk); }") != NULL, "textureType comes from the kind");
        CHECK(strstr(m, "- (NSUInteger)arrayLength { return rdna4td_array_length(_tdk, (unsigned)_layers); }") != NULL, "arrayLength comes from the kind (cube 1)");
        CHECK(strstr(m, "- (uint32_t)rdna4Slice:(NSUInteger)slice { return rdna4td_base_layer(_is3D, (NSUInteger)_baseSlice + slice); }") != NULL &&
              count(m, "[self rdna4Slice:slice]") == 2 && strstr(m, "[t rdna4Slice:slice]") != NULL,
              "CPU and blit subresources resolve view-relative slices to absolute image layers");
        CHECK(!strstr(m, "self->_is3D ? 0 : (uint32_t)slice") && !strstr(m, "is3 ? 0 : (uint32_t)slice"), "no hand-written slice -> layer expression is left");
        CHECK(strstr(m, "ca.slice >= [t rdna4Layers]") != NULL && !strstr(m, "ca.slice != 0"), "the render pass refuses slice >= rdna4Layers (not slice != 0)");
        CHECK(strstr(m, "[t rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice]") != NULL, "the render pass attaches the per-layer view of ca.slice");
        CHECK(strstr(m, "BOOL wholeImg = [t rdna4Levels] > 1 || [t rdna4ImageLayers] > 1;") != NULL, "a layered texture is moved to the attachment layout as a whole");
        CHECK(strstr(m, "rdna4td_view_map(root->_tdk, [root rdna4Layers]") != NULL &&
              strstr(m, "_baseLevel, (uint32_t)_levels, _baseSlice, rdna4td_nlayers(view.layersIvar)") != NULL &&
              !strstr(m, "views of cube / 2D-array textures are not implemented"),
              "texture views map compatible layer ranges to typed Vulkan views");
        CHECK(strstr(m, "if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72") != NULL &&
              strstr(m, "if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) BFAIL(121") != NULL, "IOSurface- and buffer-backed textures are still 2D only");
        CHECK(strstr(m, "_Static_assert((unsigned)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE") != NULL && strstr(m, "(int)MTLTextureTypeCube == RDNA4TD_MTL_CUBE") != NULL, "the header's numeric values are asserted against the real enums");
    }
    free(m);
    printf("test-texdesc: %d checks, %d failed\n", checks, fails); return fails ? 1 : 0;
}
