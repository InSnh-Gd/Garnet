// test-intclear-semantics.m (bundle 19): runs Apple's REAL Metal (the host Mac's M4) and compares an RG16Uint render-pass Clear against rdna4_intfmt.h's rdna4if_clear_u, over edge values and a pseudo-random grid.
// Build/run (from this directory):  clang -fobjc-arc -framework Metal -framework Foundation -I. -o /tmp/test-intclear test-intclear-semantics.m && /tmp/test-intclear
// Exit 3 = SKIP (no Metal device). This is what proves the conversion is Metal's, not a guess.
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "rdna4_intfmt.h"
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc > 3) return 2;
    BOOL signed32 = argc == 3 && !strcmp(argv[2], "--r32s");
    BOOL r32 = signed32 || (argc == 3 && !strcmp(argv[2], "--r32"));
    if (argc == 3 && !r32) return 2;
    id<MTLDevice> dev;
    if (argc < 2 || !strcmp(argv[1], "--reference")) dev = MTLCreateSystemDefaultDevice();
    else {
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) { fprintf(stderr, "FAIL: %s\n", error.description.UTF8String); return 1; }
        dev = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
    }
    if (!dev) { printf("SKIP: no Metal device\n"); return 3; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const char *format = signed32 ? "R32Sint" : r32 ? "R32Uint" : "RG16Uint";
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:signed32 ? MTLPixelFormatR32Sint : r32 ? MTLPixelFormatR32Uint : MTLPixelFormatRG16Uint width:2 height:2 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    double edge[] = { 0, 1, 0.5, 0.9999999, 1.5, 2.5, 3.5, 254.5, 255, 255.9999, 256, 12345, 12345.99, 32767.5, 32768, 65534.5, 65535, 65535.5, 65535.9999, 65536, 70000, 1e10, 4294967295.0, 4294967296.0, 1e300,
                      -0.0, -1e-300, -0.5, -1, -5, -65535, -1e10, INFINITY, -INFINITY, NAN,
                      -1.5, -2.5, 2147483646.5, 2147483647.0, 2147483648.0, -2147483647.5, -2147483648.0, -2147483649.0 };
    int n = (int)(sizeof edge / sizeof *edge), tests = 0, bad = 0;
    unsigned long long s = 88172645463325252ull;
    int grid = r32 ? n : n * n;
    for (int i = 0; i < grid + 200; i++) {
        double a, b;
        if (i < grid) { a = edge[r32 ? i : i / n]; b = edge[i % n]; }
        else { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a = (double)(s % 140000) / 8.0 - 2000.0; s ^= s << 13; s ^= s >> 7; s ^= s << 17; b = (double)(s % 140000) / 8.0 - 2000.0; }
        id<MTLTexture> t = [dev newTextureWithDescriptor:td]; if (!t) { printf("FAIL: no %s texture\n", format); return 1; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(a, b, 0, 0);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (!e || cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "FAIL: integer clear execution\n"); return 1; }
        uint32_t raw = 0; [t getBytes:&raw bytesPerRow:4 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
        uint16_t pair[2]; memcpy(pair, &raw, sizeof pair);
        unsigned pa = r32 ? raw : pair[0], pb = r32 ? 0 : pair[1];
        unsigned ea = signed32 ? (uint32_t)rdna4if_clear_s32(a) : rdna4if_clear_u(a, r32 ? 32 : 16);
        unsigned eb = r32 ? 0 : rdna4if_clear_u(b, 16); tests++;
        if (pa != ea || pb != eb) { bad++; if (bad <= 10) printf("FAIL: clear (%.17g, %.17g): Metal (%u, %u), rdna4if_clear_u (%u, %u)\n", a, b, pa, pb, ea, eb); }
    }
    printf("%s: %d clears of an %s target on %s, %d differ from rdna4if conversion\n", bad ? "FAIL" : "ok  ", tests, format, dev.name.UTF8String, bad);
    return bad ? 1 : 0;
} }
