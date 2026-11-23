// test-intfmt.c: host test of rdna4_intfmt.h (bundle 19, missing menus: RG16Uint) plus source pins on GarnetDevice.m that prove REACHABILITY and ORDER of the glue, not just the text.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-intfmt test-intfmt.c && /tmp/test-intfmt .      (argv[1] = the directory holding GarnetDevice.m, default ".")
// The clear table below is what Apple's Metal did on the M4 (test-intclear-semantics.m repeats it live against the real Metal and against this header).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "rdna4_intfmt.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); return b; }
static int count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }
static const char *at(const char *hay, const char *needle) { return strstr(hay, needle); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    // ---- the measured Metal clear conversion (RG16Uint, clearColor doubles) ----
    static const struct { double a, b; unsigned ea, eb; } m[] = {
        { 12345, 65535, 12345, 65535 }, { 0, 1, 0, 1 }, { 1.4, 1.5, 1, 1 }, { 2.5, 3.5, 2, 3 }, { 0.5, -1, 0, 0 }, { -5, 70000, 0, 65535 }, { 65536, 65535.5, 65535, 65535 },
        { 1e10, -1e10, 65535, 0 }, { 255.9999, 0.4999, 255, 0 }, { 4294967295.0, 1e300, 65535, 65535 }, { NAN, 1, 0, 1 },
    };
    int bad = 0; for (size_t i = 0; i < sizeof m / sizeof *m; i++) { unsigned a = rdna4if_clear_u(m[i].a, 16), b = rdna4if_clear_u(m[i].b, 16); if (a != m[i].ea || b != m[i].eb) { bad++; printf("   (%g, %g) -> (%u, %u) expected (%u, %u)\n", m[i].a, m[i].b, a, b, m[i].ea, m[i].eb); } }
    CHECK("clear conversion equals the 11 measured Metal rows", bad == 0, "%d rows differ", bad);
    CHECK("truncates, not rounds (1.5 -> 1, 2.5 -> 2, 3.5 -> 3, 255.9999 -> 255)", rdna4if_clear_u(1.5, 16) == 1 && rdna4if_clear_u(2.5, 16) == 2 && rdna4if_clear_u(3.5, 16) == 3 && rdna4if_clear_u(255.9999, 16) == 255, "ok");
    CHECK("saturates at 2^bits-1 (8, 16, 32 bits)", rdna4if_clear_u(1e9, 8) == 255 && rdna4if_clear_u(300, 8) == 255 && rdna4if_clear_u(65535.9, 16) == 65535 && rdna4if_clear_u(1e30, 32) == 0xFFFFFFFFu && rdna4if_clear_u(4294967295.0, 32) == 0xFFFFFFFFu, "ok");
    CHECK("negatives, -0 and NaN give 0", rdna4if_clear_u(-0.0, 16) == 0 && rdna4if_clear_u(-1e-9, 16) == 0 && rdna4if_clear_u(NAN, 16) == 0 && rdna4if_clear_u(-INFINITY, 16) == 0 && rdna4if_clear_u(INFINITY, 16) == 65535, "ok");
    CHECK("a float-bits clear would be wrong: (float)12345.0 reinterpreted as uint is not 12345", ({ float f = 12345.0f; uint32_t u; memcpy(&u, &f, 4); u != 12345u; }), "the bug the .uint32 rule prevents");
    CHECK("signed clears preserve negative values, truncate and saturate", rdna4if_clear_s32(-19.75) == -19 &&
        rdna4if_clear_s32(1.5) == 1 && rdna4if_clear_s32(-INFINITY) == INT32_MIN &&
        rdna4if_clear_s32(INFINITY) == INT32_MAX && rdna4if_clear_s32(NAN) == 0, "signed 32-bit boundary");
    CHECK("signed 8/16/32 clears clamp to bit width",
        rdna4if_clear_s(-200.0, 8) == -128 && rdna4if_clear_s(200.0, 8) == 127 && rdna4if_clear_s(-19.9, 8) == -19 &&
        rdna4if_clear_s(-40000.0, 16) == -32768 && rdna4if_clear_s(40000.0, 16) == 32767 && rdna4if_clear_s(NAN, 16) == 0 &&
        rdna4if_clear_s(INFINITY, 32) == INT32_MAX, "signed width clamps");
    // ---- the fallback write mask ----
    int ok = 1; for (unsigned wm = 0; wm < 16; wm++) { ok &= rdna4if_write_mask(1, 1, wm) == 0; ok &= rdna4if_write_mask(1, 0, wm) == wm; ok &= rdna4if_write_mask(0, 1, wm) == wm; ok &= rdna4if_write_mask(0, 0, wm) == wm; }
    CHECK("write mask: 0 only for (fallback AND integer attachment); every other case keeps the descriptor's mask", ok, "16 masks x 4 cases");

    // ---- source pins on the REAL GarnetDevice.m ----
    char *m_ = slurp(dir, "GarnetDevice.m"); if (!m_) { printf("FAIL: cannot read GarnetDevice.m in %s\n", dir); return 2; }
    CHECK("pin: flag bit 4u, distinct from RDNA4F_A8 1u, RDNA4F_DS 2u and RDNA4F_SINT 8u", count(m_, "#define RDNA4F_UINT 4u") == 1 && count(m_, "#define RDNA4F_A8   1u") == 1 && count(m_, "#define RDNA4F_DS 2u") == 1 && count(m_, "#define RDNA4F_SINT 8u") == 1, "one definition each");
    CHECK("pin: table entry RG16Uint -> VK_FORMAT_R16G16_UINT, 4 bytes, RDNA4F_UINT (not 2u, not UNORM)", count(m_, "{ MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UINT,              4, RDNA4F_UINT }") == 1 && count(m_, "VK_FORMAT_R16G16_UINT,              4, RDNA4F_UINT") == 1, "one entry (the vertex-format use of VK_FORMAT_R16G16_UINT is separate)");
    CHECK("pin: R32Uint uses a uint format and per-channel bit-width helper", count(m_, "{ MTLPixelFormatR32Uint,         VK_FORMAT_R32_UINT,                 4, RDNA4F_UINT }") == 1 && count(m_, "- (void)rdna4IntBitsRGB:(unsigned *)rgb alpha:(unsigned *)alpha { unsigned r = 16, a = 16; rdna4_int_bits(_pf, &r, &a); if (rgb) *rgb = r; if (alpha) *alpha = a; }") == 1 && count(m_, "[t rdna4IntBitsRGB:&rgbB alpha:&aB];") == 2, "RGB/alpha bit limits support 8/10-2/16/32 integer formats");
    const char *tab = at(m_, "static const RDNA4Fmt rdna4_fmts[] = {"), *tabend = tab ? at(tab, "\n};") : NULL, *ent = at(m_, "{ MTLPixelFormatRG16Uint,");
    CHECK("pin: the entry lies inside rdna4_fmts[] (the colour table), not the depth table", tab && tabend && ent && ent > tab && ent < tabend, "inside");
    CHECK("pin: kill file /private/tmp/rdna4m-nouint, one definition, one stat(), read through dispatch_once", count(m_, "#define RDNA4_NOUINT_FILE \"/private/tmp/rdna4m-nouint\"") == 1 && count(m_, "stat(RDNA4_NOUINT_FILE, &st)") == 1 && count(m_, "rdna4m-nouint\"") >= 1, "latched once per process");
    const char *nu = at(m_, "static BOOL rdna4_nouint(void) {"), *nuo = nu ? at(nu, "dispatch_once(&once") : NULL, *nus = nu ? at(nu, "stat(RDNA4_NOUINT_FILE") : NULL;
    CHECK("pin: the stat() lives inside the dispatch_once block of rdna4_nouint()", nu && nuo && nus && nuo < nus && nus - nu < 400, "once-only");
    const char *fm = at(m_, "static const RDNA4Fmt *rdna4_fmt(MTLPixelFormat f) {"), *fme = fm ? at(fm, "\n}") : NULL, *fmk = fm ? at(fm, "(RDNA4F_UINT | RDNA4F_SINT)) && rdna4_nouint()") : NULL;
    CHECK("pin: rdna4_fmt() (the one colour lookup) answers NULL for an RDNA4F_UINT entry when the kill file exists", fm && fme && fmk && fmk < fme, "the kill file restores today's nil everywhere rdna4_fmt is used");
    // clear site
    const char *cl = at(m_, "MTLClearColor c = ca.clearColor;"), *cu = cl ? at(cl, "if ([t rdna4IsUInt]) {") : NULL, *cu32 = cl ? at(cl, ".uint32 = { rdna4if_clear_u(c.red, rgbB)") : NULL, *cs = cl ? at(cl, "} else if ([t rdna4IsSInt]) {") : NULL, *cs32 = cs ? at(cs, ".int32 = { rdna4if_clear_s(c.red, rgbB)") : NULL, *cf = cl ? at(cl, ".float32 = { (float)c.red") : NULL;
    CHECK("pin: the colour clear takes the .uint32 / .int32 branch for integer attachments BEFORE the .float32 fill (same loop, no early exit)", cl && cu && cu32 && cs && cs32 && cf && cu > cl && cu32 > cu && cs > cu32 && cs32 > cs && cf > cs32 && cf - cl < 1800, "order clear < isUInt < .uint32 < isSInt < .int32 < .float32");
    CHECK("pin: integer branches chain into the float32 else arm", cu && cs && cf && at(cs, "} else\n        _cvs[na] = (VkClearValue){ .color = { .float32") != NULL, "else arm");
    CHECK("pin: exactly one colour clear value site (.float32), one .uint32 site, and one .int32 site", count(m_, ".color = { .float32") == 1 && count(m_, ".color = { .uint32") == 1 && count(m_, ".color = { .int32") == 1, "no other clear site");
    CHECK("pin: rdna4IsUInt is declared and defined once", count(m_, "- (BOOL)rdna4IsUInt;") == 1 && count(m_, "- (BOOL)rdna4IsUInt { return _fmt != NULL && (_fmt->flags & RDNA4F_UINT) != 0; }") == 1, "reads the table flag of the texture (views inherit _fmt)");
    // fallback mask
    const char *wm0 = at(m_, "VkColorComponentFlags wm = 0; MTLColorWriteMask m = ca.writeMask;"), *wmr = wm0 ? at(wm0, "wm = rdna4if_write_mask(fb, (f->flags & (RDNA4F_UINT | RDNA4F_SINT)) != 0, wm);") : NULL, *wmo = wm0 ? at(wm0, "if (rf && !(rf->outmask") : NULL, *wcb = wm0 ? at(wm0, "_cba[na] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = wm };") : NULL;
    CHECK("pin: the fallback mask rule sits between the descriptor's mask and the blend-attachment state that consumes it", wm0 && wmr && wmo && wcb && wmr > wm0 && wmr < wmo && wmo < wcb, "wm built < rule < outmask rule < _cba");
    CHECK("pin: the rule uses the pipeline's own `fb` (fallback) flag and the attachment's table flag", count(m_, "rdna4if_write_mask(fb, (f->flags & (RDNA4F_UINT | RDNA4F_SINT)) != 0, wm)") == 1 && at(m_, "BOOL fb = NO;") != NULL, "same fb that selects rdna4_fb_fs_spv");
    const char *fbs = at(m_, "_fs = fb ? rdna4_spv_module(rdna4_fb_fs_spv"), *fbd = at(m_, "BOOL fb = NO;");
    CHECK("pin: `fb` is set (declared) before both the fallback shader choice and the mask rule", fbd && fbs && wmr && fbd < fbs && fbs < wmr, "declared < module < rule");
    CHECK("pin: all four clear channels go through rdna4if_clear_u / rdna4if_clear_s (rgbB and aB handle mixed 10-2 formats)",
          count(m_, "rdna4if_clear_u(c.red, rgbB)") == 1 && count(m_, "rdna4if_clear_u(c.green, rgbB)") == 1 &&
          count(m_, "rdna4if_clear_u(c.blue, rgbB)") == 1 && count(m_, "rdna4if_clear_u(c.alpha, aB)") == 1 &&
          count(m_, "rdna4if_clear_s(c.red, rgbB)") == 1 && count(m_, "rdna4if_clear_s(c.green, rgbB)") == 1 &&
          count(m_, "rdna4if_clear_s(c.blue, rgbB)") == 1 && count(m_, "rdna4if_clear_s(c.alpha, aB)") == 1, "4+4 of 4+4");
    { char *h_ = slurp(dir, "rdna4_intfmt.h"); CHECK("pin: the converter's NaN guard is `!(v > 0.0)` (a UB cast of NaN gives 0 on arm64 but not on x86, so the value test cannot see it)", h_ && count(h_, "if (!(v > 0.0)) return 0;") == 1, "present"); free(h_); }
    CHECK("pin: the include of rdna4_intfmt.h", count(m_, "#include \"rdna4_intfmt.h\"") == 1, "present");
    CHECK("pin: texture and pipeline creation check the backend format features", count(m_, "rdna4_fmt_feats(p->fmt)") == 1 && count(m_, "rdna4_fmt_feats(f)") >= 1 && count(m_, "RADV features 0x%x lack") >= 1 && count(m_, "p->canRT = p->isDS ?") == 1, "texture allocation uses the shared image plan");
    free(m_);
    { char *pl = slurp(dir, "Info.plist"); CHECK("pin: Info.plist CFBundleVersion is 43", pl && strstr(pl, "<key>CFBundleVersion</key>\n\t<string>43</string>"), "expected build 43"); free(pl); }
    printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails); return fails ? 1 : 0;
}
