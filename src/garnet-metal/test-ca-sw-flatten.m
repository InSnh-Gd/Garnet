// Diagnostic for QuartzCore 25G332. No GPU work or RDNA4 bundle loading.
// --raw reproduces the physical WindowServer assertion; --compat checks the fix.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#include <assert.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <unistd.h>
#define RDNA4LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "rdna4_ca_sw_compat.h"

@interface CARenderer (DiagnosticSoftware)
- (instancetype)_initWithOptions:(NSDictionary *)options;
@end

static uint32_t pixels[16 * 16];
static uint32_t reference[16 * 16];

static BOOL matching_image(const void *base) {
    const uint8_t expected[16] = {
        0x51,0x6d,0xa0,0xce,0xe1,0x22,0x3d,0xb8,
        0x8a,0x17,0x05,0xaf,0x12,0xeb,0x86,0xeb
    };
    const struct mach_header_64 *header = base;
    if (header->magic != MH_MAGIC_64) return NO;
    const struct load_command *command = (const void *)(header + 1);
    for (unsigned i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID)
            return memcmp(((const struct uuid_command *)command)->uuid,
                          expected, sizeof expected) == 0;
        command = (const void *)((const char *)command + command->cmdsize);
    }
    return NO;
}

static CARenderer *software_renderer(void) {
    void *(*create)(void *, void *, unsigned) =
        dlsym(RTLD_DEFAULT, "CARenderOGLNew");
    void *callbacks = dlsym(RTLD_DEFAULT, "kCARenderSoftwareCallbacks");
    Dl_info image;
    if (!create || !callbacks || !dladdr(create, &image) ||
        !matching_image(image.dli_fbase)) return nil;
    void *renderer = create(callbacks, NULL, 0);
    assert(renderer);
    void *context = *(void **)renderer;
    CARenderer *object = [[CARenderer alloc] _initWithOptions:nil];
    assert(object);
    // Same context/renderer fields filled by CARenderer's platform initializers.
    void *data = *(void **)((char *)(__bridge void *)object + 8);
    *(void **)((char *)data + 0x28) = context;
    *(void **)((char *)data + 0x30) = renderer;
    BOOL (*set_destination)(void *, void *, long, unsigned long, void *, long,
                            int, int, int, int) =
        (void *)((char *)image.dli_fbase + 0x1ebc7e);
    assert(set_destination(context, pixels, 64, 32, NULL, 0, 0, 0, 16, 16));
    object.bounds = CGRectMake(0, 0, 16, 16);
    return object;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 || (strcmp(argv[1], "--raw") &&
                          strcmp(argv[1], "--plain") && strcmp(argv[1], "--compat"))) return 64;
        alarm(30);
        setvbuf(stdout, NULL, _IONBF, 0);
        BOOL compat = !strcmp(argv[1], "--compat");
        if (compat && rdna4_ca_sw_compat() != 1) return 1;
        for (unsigned frame = 0; frame < 17; ++frame) { @autoreleasepool {
        memset(pixels, 0, sizeof pixels);
        CARenderer *renderer = software_renderer();
        if (!renderer) { puts("UNSUPPORTED_QUARTZCORE_IMAGE"); return 77; }
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        CALayer *root = [CALayer layer];
        root.frame = renderer.bounds;
        CALayer *tile = [CALayer layer];
        tile.frame = CGRectMake(4, 4, 8, 8);
        tile.opacity = 0.8;
        tile.allowsGroupOpacity = YES;
        CGColorRef color = CGColorCreateGenericRGB(1, 0, 0, 1);
        for (unsigned i = 0; i < 2; ++i) {
            CALayer *part = [CALayer layer];
            part.frame = CGRectMake(i * 2, i * 2, 6, 6);
            part.backgroundColor = color;
            [tile addSublayer:part];
        }
        CGColorRelease(color);
        ((void (*)(id, SEL, BOOL))objc_msgSend)(
            tile, sel_getUid("setShouldFlatten:"), frame && strcmp(argv[1], "--plain") != 0);
        [root addSublayer:tile];
        renderer.layer = root;
        [CATransaction commit];
        [CATransaction flush];
        if (frame == 1) printf("SW_FLATTEN_BEGIN %s\n", argv[1]);
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [renderer addUpdateRect:renderer.bounds];
        [renderer render];
        [renderer endFrame];
        assert(pixels[8 * 16 + 8] != 0);
        if (!frame) memcpy(reference, pixels, sizeof pixels);
        else {
            unsigned bad = 0, max_delta = 0;
            for (unsigned i = 0; i < 16 * 16; ++i) {
                if (reference[i] != pixels[i]) ++bad;
                for (unsigned c = 0; c < 4; ++c) {
                    int delta = (int)((reference[i] >> (c * 8)) & 255) -
                                (int)((pixels[i] >> (c * 8)) & 255);
                    unsigned magnitude = (unsigned)abs(delta);
                    if (magnitude > max_delta) max_delta = magnitude;
                }
            }
            if (max_delta > 1) printf("SW_FLATTEN_DIFF frame=%u pixels=%u max_delta=%u\n",
                                     frame, bad, max_delta);
            // The extra offscreen blend rounds four edge pixels by one LSB.
            assert(max_delta <= 1);
        }
        if (frame == 1) printf("SW_FLATTEN_PIXEL %08x\n", pixels[8 * 16 + 8]);
        renderer.layer = nil;
        }}
        printf("SW_FLATTEN_COMPLETED %s contexts=16 pixel_tolerance=1\n", argv[1]);
        _exit(0);
    }
}
