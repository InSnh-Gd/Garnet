// Real Core Animation icon composition; VM backend pixels only, no native scanout coverage.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>

static void fail(NSString *reason) __attribute__((noreturn));
static void fail(NSString *reason) {
    fprintf(stderr, "CA_ICONS_FAIL %s\n", reason.UTF8String);
    _exit(1);
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3 || geteuid() != 0) {
        fputs("Usage: sudo test-ca-icons --reference|VM_TEST_BUNDLE OUTPUT_DIRECTORY\n", stderr);
        return 2;
    }
    alarm(90);
    setvbuf(stdout, NULL, _IONBF, 0);
    BOOL reference = !strcmp(argv[1], "--reference");
    NSString *directory = @(argv[2]);
    [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    setenv("RDNA4M_ALLOW", "1", 1);
    setenv("RDNA4M_TEST_INPROC", "1", 1);
    setenv("RDNA4M_TEST_USERCACHE", argv[2], 1);
    setenv("RDNA4M_TEST_SIDE_DIR", argv[2], 1);
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 1);
    setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
    id<MTLDevice> device;
    if (reference) device = MTLCreateSystemDefaultDevice();
    else {
        NSError *error = nil;
        if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
        device = [[objc_getClass("GarnetDevice") alloc] init];
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    if (!queue || !texture) fail(@"device, queue or destination unavailable");
    CALayer *root = [CALayer layer];
    root.frame = CGRectMake(0, 0, 256, 256);
    CGColorRef background = CGColorCreateGenericRGB(0.2, 0.3, 0.4, 1);
    root.backgroundColor = background;
    CGColorRelease(background);
    NSArray *names = @[@"gearshape.fill", @"wifi", @"person.crop.circle.fill", @"bell.fill"];
    for (NSUInteger i = 0; i < names.count; i++) {
        NSImage *symbol = [NSImage imageWithSystemSymbolName:names[i] accessibilityDescription:nil];
        CGImageRef image = [symbol CGImageForProposedRect:NULL context:nil hints:nil];
        if (!image) fail([NSString stringWithFormat:@"symbol unavailable: %@", names[i]]);
        CALayer *icon = [CALayer layer];
        icon.frame = CGRectMake(16 + (i % 2) * 128, 16 + (i / 2) * 128, 96, 96);
        icon.contents = (__bridge id)image;
        [root addSublayer:icon];
    }
    CARenderer *renderer = [CARenderer rendererWithMTLTexture:texture
        options:@{kCARendererMetalCommandQueue: queue}];
    if (!renderer) fail(@"Core Animation rejected the destination");
    renderer.layer = root;
    renderer.bounds = root.bounds;
    [CATransaction flush];
    NSString *referencePath = [directory stringByAppendingPathComponent:@"reference.bgra"];
    NSData *expected = reference ? nil : [NSData dataWithContentsOfFile:referencePath];
    if (!reference && expected.length != 256 * 256 * 4) fail(@"missing reference pixels");
    for (unsigned frame = 0; frame < 3; frame++) {
        double start = CACurrentMediaTime();
        [renderer beginFrameAtTime:start timeStamp:NULL];
        [renderer addUpdateRect:renderer.bounds];
        [renderer render];
        [renderer endFrame];
        id<MTLCommandBuffer> fence = [queue commandBuffer];
        [fence commit];
        [fence waitUntilCompleted];
        if (fence.status != MTLCommandBufferStatusCompleted || fence.error) fail(fence.error.description ?: @"GPU fence failed");
        double ms = (CACurrentMediaTime() - start) * 1000;
        NSMutableData *pixels = [NSMutableData dataWithLength:256 * 256 * 4];
        [texture getBytes:pixels.mutableBytes bytesPerRow:1024 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
        if (reference && frame == 0) {
            expected = [pixels copy];
            if (![expected writeToFile:referencePath atomically:YES]) fail(@"reference write failed");
        }
        const uint8_t *a = pixels.bytes, *b = expected.bytes;
        unsigned bad = 0, maxDelta = 0, changed = 0;
        for (NSUInteger p = 0; p < 256 * 256; p++) {
            unsigned delta = 0;
            for (unsigned c = 0; c < 4; c++) {
                unsigned d = (unsigned)abs((int)a[4 * p + c] - b[4 * p + c]);
                if (d > delta) delta = d;
            }
            bad += delta > 2;
            if (delta > maxDelta) maxDelta = delta;
            changed += memcmp(a + 4 * p, a, 4) != 0;
        }
        printf("CA_ICONS_PIXELS reference=%d frame=%u ms=%.3f bad=%u max_delta=%u non_background=%u\n",
            reference, frame, ms, bad, maxDelta, changed);
        NSString *name = [NSString stringWithFormat:@"%@-%u.bgra", reference ? @"reference" : @"candidate", frame];
        [pixels writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES];
        if (bad || changed < 500) fail(@"first-use or warm icon pixels differ or icons are absent");
    }
    puts("CA_ICONS_PASS icons=4 frames=3 first_frame_verified=1 native_transport=untested");
    _exit(0);
} }
