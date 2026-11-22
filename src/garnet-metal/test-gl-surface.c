// Host test of rdna4_gl_surface_pure.h plus source pins on assembled GarnetDevice.m.
// Build/run from repo root (dev host):
//   python3 src/garnet-metal/assemble-device.py src/garnet-metal/GarnetDevice.m \
//     --output temp/scratch/garnet-assembled-device.m
//   clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
//     -Isrc/garnet-metal -o src/garnet/build-host-rdna4/test-gl-surface \
//     src/garnet-metal/test-gl-surface.c
//   ./src/garnet/build-host-rdna4/test-gl-surface \
//     temp/scratch/garnet-assembled-device.m src/garnet-metal
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna4_gl_surface_pure.h"

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
    } \
} while (0)

static char *slurp_path(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1u, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

static void fill_region(uint8_t out[12], uint32_t num_rects,
                        int16_t x, int16_t y, int16_t w, int16_t h) {
    memcpy(out, &num_rects, 4u);
    int16_t b[4] = { x, y, w, h };
    memcpy(out + 4u, b, sizeof b);
}

static void test_pure_surface_table(void) {
    rdna4gl_table t;
    rdna4gl_init(&t);

    CHECK(!rdna4gl_is_connect(0ULL), "connect: 0 rejected");
    CHECK(!rdna4gl_is_connect(0x1203ULL), "connect: 32-bit mach port rejected");
    CHECK(!rdna4gl_is_connect(0xffffffffULL), "connect: 32-bit max rejected");
    CHECK(!rdna4gl_is_connect(RDNA4GL_CONNECT_TAG), "connect: zero seq rejected");
    CHECK(!rdna4gl_is_connect((0x47524e53ULL << 32) | 1ULL), "connect: wrong tag rejected");
    CHECK(rdna4gl_is_connect(RDNA4GL_CONNECT_TAG | 1ULL), "connect: GRNT tag + seq accepted");

    uint64_t dummy = 0ULL;
    CHECK(!rdna4gl_create(&t, 0u, &dummy), "create: sid 0 rejected");
    CHECK(!rdna4gl_create(NULL, 1u, &dummy), "create: NULL table rejected");
    CHECK(!rdna4gl_create(&t, 1u, NULL), "create: NULL out_connect rejected");

    uint64_t conns[RDNA4GL_MAX_SURFACES];
    for (unsigned i = 0; i < RDNA4GL_MAX_SURFACES; i++) {
        conns[i] = 0ULL;
        CHECK(rdna4gl_create(&t, 100u + i, &conns[i]), "create: slot within 64 succeeds");
        CHECK(rdna4gl_is_connect(conns[i]), "create: output token has GRNT tag");
        for (unsigned j = 0; j < i; j++) {
            if (conns[i] == conns[j]) {
                CHECK(0, "create: tokens must be distinct");
                break;
            }
        }
    }
    CHECK(!rdna4gl_create(&t, 999u, &dummy), "create: 65th surface rejected when full");
    CHECK(rdna4gl_find_by_sid(&t, 0u) == NULL, "find_by_sid: 0 returns NULL");
    CHECK(rdna4gl_find_by_sid(&t, 132u) != NULL, "find_by_sid: active sid found");
    CHECK(rdna4gl_find_by_connect(&t, conns[32]) != NULL, "find_by_connect: active token found");

    void *retired[RDNA4GL_NUM_BUFFERS] = { NULL, NULL };
    unsigned nret = 99u;
    CHECK(rdna4gl_destroy(&t, conns[32], retired, &nret) && nret == 0u,
          "destroy: empty slot destroys with 0 retired buffers");
    CHECK(rdna4gl_find_by_connect(&t, conns[32]) == NULL, "destroy: token removed");
    CHECK(rdna4gl_find_by_sid(&t, 132u) == NULL, "destroy: sid removed");
    CHECK(!rdna4gl_destroy(&t, conns[32], retired, &nret), "destroy: double destroy fails");

    uint64_t reused = 0ULL;
    CHECK(rdna4gl_create(&t, 500u, &reused) && rdna4gl_is_connect(reused) &&
          reused != conns[32], "create: freed slot reusable with fresh token");

    // Sequence wraparound collision check while slot 0 holds seq == 1.
    rdna4gl_init(&t);
    uint64_t c0 = 0ULL, c1 = 0ULL, c2 = 0ULL;
    CHECK(rdna4gl_create(&t, 1u, &c0) && (c0 & 0xffffffULL) == 1ULL, "wrap: first seq is 1");
    t.next_seq = 0xffffffu - 1u;
    CHECK(rdna4gl_create(&t, 2u, &c1) && (c1 & 0xffffffULL) == 0xffffffu, "wrap: reaches 0xffffff");
    CHECK(rdna4gl_create(&t, 3u, &c2) && rdna4gl_is_connect(c2) &&
          c2 != c0 && c2 != c1, "wrap: skips occupied seq 1 cleanly");

    // Geometry parser + shape + buffer lifecycle.
    uint8_t reg[12];
    uint32_t w = 0u, h = 0u;
    fill_region(reg, 1u, 0, 0, 1280, 720);
    CHECK(!rdna4gl_parse_shape(NULL, 12u, &w, &h), "parse_shape: NULL region rejected");
    CHECK(!rdna4gl_parse_shape(reg, 11u, &w, &h), "parse_shape: truncated region rejected");
    CHECK(rdna4gl_parse_shape(reg, sizeof reg, &w, &h) && w == 1280u && h == 720u,
          "parse_shape: 1280x720 parsed");
    fill_region(reg, 1u, 10, 20, 0, 720);
    CHECK(!rdna4gl_parse_shape(reg, sizeof reg, &w, &h), "parse_shape: zero width rejected");
    fill_region(reg, 1u, 0, 0, 1280, -4);
    CHECK(!rdna4gl_parse_shape(reg, sizeof reg, &w, &h), "parse_shape: negative height rejected");
    fill_region(reg, 1u, 0, 0, 16385, 64);
    CHECK(!rdna4gl_parse_shape(reg, sizeof reg, &w, &h), "parse_shape: oversized width rejected");
    fill_region(reg, 1u, 0, 0, 16384, 16384);
    CHECK(rdna4gl_parse_shape(reg, sizeof reg, &w, &h) && w == 16384u && h == 16384u,
          "parse_shape: max 16384x16384 accepted");

    rdna4gl_init(&t);
    uint64_t sc = 0ULL;
    CHECK(rdna4gl_create(&t, 77u, &sc), "lifecycle: surface created");
    rdna4gl_slot *s = rdna4gl_find_by_connect(&t, sc);
    CHECK(s && !rdna4gl_needs_buffer(s, 0u) && !rdna4gl_needs_buffer(s, 1u),
          "lifecycle: zero-sized surface needs no buffers yet");

    fill_region(reg, 1u, 0, 0, 800, 600);
    CHECK(rdna4gl_set_shape(&t, sc, reg, sizeof reg, retired, &nret) && nret == 0u,
          "lifecycle: initial 800x600 shape succeeds");
    CHECK(s->width == 800u && s->height == 600u, "lifecycle: dimensions stored");
    CHECK(rdna4gl_needs_buffer(s, 0u) && rdna4gl_needs_buffer(s, 1u) &&
          !rdna4gl_needs_buffer(s, 2u), "lifecycle: buffers 0 and 1 needed after shape");

    void *b0 = (void *)(uintptr_t)0x1001;
    void *b1 = (void *)(uintptr_t)0x1002;
    rdna4gl_install_buffers(s, b0, b1, retired, &nret);
    CHECK(nret == 0u && !rdna4gl_needs_buffer(s, 0u) && !rdna4gl_needs_buffer(s, 1u),
          "lifecycle: buffers installed");
    CHECK(rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 0u) == b0,
          "control: cmd 0x14 arg 0 returns buffer 0");
    CHECK(rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 1u) == b1,
          "control: cmd 0x14 arg 1 returns buffer 1");
    CHECK(rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 2u) == NULL,
          "control: arg 2 rejected");
    CHECK(rdna4gl_control_buffer(&t, sc, 0x15u, 0u) == NULL,
          "control: non-0x14 command rejected");
    CHECK(rdna4gl_control_buffer(&t, 0x1203ULL, RDNA4GL_CTRL_GET_BUFFER, 0u) == NULL,
          "control: untagged connect rejected");

    // Same-size shape update retains existing buffers.
    CHECK(rdna4gl_set_shape(&t, sc, reg, sizeof reg, retired, &nret) && nret == 0u &&
          !rdna4gl_needs_buffer(s, 0u) &&
          rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 0u) == b0,
          "lifecycle: same-size shape retains buffers");

    // Invalid shape leaves valid dimensions and buffers intact.
    fill_region(reg, 1u, 0, 0, -10, 600);
    CHECK(!rdna4gl_set_shape(&t, sc, reg, sizeof reg, retired, &nret) && nret == 0u &&
          s->width == 800u && s->height == 600u &&
          rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 0u) == b0,
          "lifecycle: invalid shape preserves existing geometry and buffers");

    // Resize invalidates and exports old buffers cleanly.
    fill_region(reg, 1u, 0, 0, 1920, 1080);
    CHECK(rdna4gl_set_shape(&t, sc, reg, sizeof reg, retired, &nret) &&
          nret == 2u && retired[0] == b0 && retired[1] == b1,
          "lifecycle: resize retires old buffers in order");
    CHECK(s->width == 1920u && s->height == 1080u &&
          rdna4gl_needs_buffer(s, 0u) && rdna4gl_needs_buffer(s, 1u) &&
          rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 0u) == NULL,
          "lifecycle: stale buffers cleared on resize");

    void *b2 = (void *)(uintptr_t)0x2001;
    void *b3 = (void *)(uintptr_t)0x2002;
    rdna4gl_install_buffers(s, b2, b3, retired, &nret);
    CHECK(nret == 0u, "lifecycle: resized buffers installed cleanly");
    CHECK(rdna4gl_destroy(&t, sc, retired, &nret) &&
          nret == 2u && retired[0] == b2 && retired[1] == b3,
          "destroy: retires installed buffers on teardown");
    CHECK(rdna4gl_control_buffer(&t, sc, RDNA4GL_CTRL_GET_BUFFER, 0u) == NULL,
          "destroy: buffer unreachable after destroy");
}

static void test_source_pins(const char *assembled_path, const char *dir) {
    char *asm_src = slurp_path(assembled_path);
    CHECK(asm_src != NULL, "source: assembled GarnetDevice.m readable");
    if (asm_src) {
        CHECK(strstr(asm_src, "- (BOOL)supportsUnalignedVertexFetch { return YES; }") != NULL,
              "pin: supportsUnalignedVertexFetch implemented");
        CHECK(strstr(asm_src, "- (id)newGLDrawable") != NULL,
              "pin: newGLDrawable implemented");
        CHECK(strstr(asm_src, "- (int)getSurfaceMode:(unsigned long long *)mode width:(unsigned *)width") != NULL,
              "pin: getSurfaceMode:width:height:forId: implemented");
        CHECK(strstr(asm_src, "- (void)commitAndHold { [self commit]; }") != NULL,
              "pin: commitAndHold implemented");
        CHECK(strstr(asm_src, "- (void)setLineWidth:(float)w { (void)w; }") != NULL,
              "pin: setLineWidth: implemented");
        CHECK(strstr(asm_src, "if (self && !rdna4_is_ws()) rdna4_gl_surfaces_install();") != NULL,
              "pin: rdna4_gl_surfaces_install wired outside WindowServer");

        const char *install_fn = strstr(asm_src, "static void rdna4_gl_surfaces_install(void)");
        CHECK(install_fn != NULL && strstr(install_fn, "if (rdna4_is_ws()) return;") != NULL,
              "pin: rdna4_gl_surfaces_install guards against WindowServer");

        const char *lookup = strstr(asm_src, "- (IOSurfaceRef)lookupIOSurfaceAtIndex:");
        const char *width_m = lookup ? strstr(lookup, "- (unsigned)width") : NULL;
        const char *retain = lookup ? strstr(lookup, "CFRetain") : NULL;
        CHECK(lookup && width_m && retain && retain < width_m,
              "pin: lookupIOSurfaceAtIndex: returns +1 CFRetain IOSurfaceRef");

        static const char *const k_imports[6] = {
            "IOAccelCreateSurface",
            "IOAccelDestroySurface",
            "IOAccelSurfaceControl",
            "IOAccelSetSurfaceFramebufferShape",
            "IOAccelSetSurfaceScale",
            "IOAccelWaitForSurface"
        };
        for (unsigned i = 0; i < 6u; i++) {
            CHECK(strstr(asm_src, k_imports[i]) != NULL,
                  "pin: SkyLight IOAccel surface symbol present");
        }
        free(asm_src);
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/rdna4_gl_surface_pure.h", dir);
    char *pure_src = slurp_path(path);
    CHECK(pure_src != NULL, "source: rdna4_gl_surface_pure.h readable");
    if (pure_src) {
        CHECK(strstr(pure_src, "AppleParavirt") == NULL,
              "boundary: rdna4_gl_surface_pure.h has zero AppleParavirt references");
        CHECK(strstr(pure_src, "glProgramAir") == NULL,
              "boundary: rdna4_gl_surface_pure.h has zero glProgramAir references");
        free(pure_src);
    }

    snprintf(path, sizeof path, "%s/rdna4_gl_drawable_impl.inc", dir);
    char *impl_src = slurp_path(path);
    CHECK(impl_src != NULL, "source: rdna4_gl_drawable_impl.inc readable");
    if (impl_src) {
        CHECK(strstr(impl_src, "AppleParavirt") == NULL,
              "boundary: rdna4_gl_drawable_impl.inc has zero AppleParavirt references");
        CHECK(strstr(impl_src, "glProgramAir") == NULL,
              "boundary: rdna4_gl_drawable_impl.inc has zero glProgramAir references");
        CHECK(strstr(impl_src, "GarnetAccelerator") != NULL,
              "pin: rdna4_gl_create_surface filters for GarnetAccelerator");
        free(impl_src);
    }
}

int main(int argc, char **argv) {
    const char *assembled_path = argc > 1 ? argv[1] : "GarnetDevice.m";
    const char *dir = argc > 2 ? argv[2] : "src/garnet-metal";
    test_pure_surface_table();
    test_source_pins(assembled_path, dir);
    printf("test-gl-surface: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
