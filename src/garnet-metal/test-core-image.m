// VM-only Core Image preparation through the shipped native bundle and mock BOs.
// Link with CoreImage/Metal/Foundation and the test-only librdna4featureaudit.
// The generic device isolates CPU APIs from accelerator admission; no pixel test.
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>

extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_compute_pipelines(void);
extern unsigned rdna4_audit_graphics_pipelines(void);
extern unsigned rdna4_audit_dispatches(void);

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "CORE_IMAGE_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static CIColorKernel *colorKernel(void) {
    // Runtime dispatch covers the legacy CIKL path used by existing applications.
    NSString *source = @"kernel vec4 rdna4_ci_invert(__sample c) { return vec4(vec3(1.0)-c.rgb,c.a); }";
    return ((id (*)(id, SEL, id))objc_msgSend)(CIColorKernel.class,
        sel_registerName("kernelWithString:"), source);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-core-image BUNDLE builtin|cikl|blur|transform|composite early|late\nTests Core Image preparation and encoding only; no GPU output verification.");
            return 0;
        }
        if (argc != 4 || ![@[ @"builtin", @"cikl", @"blur", @"transform", @"composite" ] containsObject:@(argv[2])] ||
            (strcmp(argv[3], "early") && strcmp(argv[3], "late")) || geteuid() != 0) return 2;
        alarm(45);
        setenv("RDNA4_AUDIT_SUBMIT", "reject", 1);
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        if (!strcmp(argv[3], "late")) {
            CIContext *cpu = [CIContext contextWithOptions:@{ kCIContextUseSoftwareRenderer: @YES }];
            if (!cpu || !colorKernel()) fail(@"CPU Core Image prewarm failed");
            puts("CORE_IMAGE_PREWARM_PASS");
        }
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) fail(error.description);
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        if (!device) fail(@"generic native device unavailable");
        // Generic init bypasses MTLIOAccelDevice's accelerator initialization.
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        printf("CORE_IMAGE_ENV %s\n", getenv("CI_USE_MTL_DAG_FOR_CIKL_SRC") ?: "unset");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) fail(@"native command queue unavailable");
        CIContext *context = [CIContext contextWithMTLCommandQueue:queue options:@{
            kCIContextUseSoftwareRenderer: @NO, kCIContextCacheIntermediates: @NO,
            // The generic test device has no IOAccel-reported memory budget.
            kCIContextMemoryLimit: @64,
            kCIContextWorkingColorSpace: NSNull.null, kCIContextOutputColorSpace: NSNull.null
        }];
        if (!context) fail(@"Core Image rejected native command queue");
        puts("CORE_IMAGE_CONTEXT_PASS");
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        id<MTLTexture> input = [device newTextureWithDescriptor:descriptor];
        id<MTLTexture> output = [device newTextureWithDescriptor:descriptor];
        if (!input || !output) fail(@"native textures unavailable");
        CIImage *image = [[CIImage alloc] initWithMTLTexture:input options:@{ kCIImageColorSpace: NSNull.null }];
        if (!image) fail(@"Core Image rejected native input texture");
        CIImage *filtered = nil;
        if (!strcmp(argv[2], "cikl")) {
            CIColorKernel *kernel = colorKernel();
            if (!kernel) fail(@"legacy CIKL kernel unavailable");
            filtered = [kernel applyWithExtent:image.extent arguments:@[ image ]];
            puts("CORE_IMAGE_CIKL_PASS");
        } else if (!strcmp(argv[2], "blur")) {
            filtered = [[image imageByApplyingFilter:@"CIGaussianBlur" withInputParameters:@{ kCIInputRadiusKey: @2.0 }]
                imageByCroppingToRect:image.extent];
        } else if (!strcmp(argv[2], "transform")) {
            filtered = [[image imageByApplyingTransform:CGAffineTransformMakeTranslation(0.5, 0.25)]
                imageByCroppingToRect:image.extent];
        } else if (!strcmp(argv[2], "composite")) {
            filtered = [[image imageByApplyingFilter:@"CIColorInvert"] imageByCompositingOverImage:image];
        } else filtered = [image imageByApplyingFilter:@"CIColorInvert"];
        if (!filtered) fail(@"filter graph unavailable");
        CIRenderDestination *destination = [[CIRenderDestination alloc] initWithMTLTexture:output commandBuffer:nil];
        if (!destination) fail(@"Core Image rejected native destination");
        printf("CORE_IMAGE_BOUNDS input=%s filtered=%s destination=%lux%lu\n",
            NSStringFromRect(image.extent).UTF8String, NSStringFromRect(filtered.extent).UTF8String,
            (unsigned long)destination.width, (unsigned long)destination.height);
        BOOL prepared = [context prepareRender:filtered fromRect:filtered.extent
            toDestination:destination atPoint:CGPointZero error:&error];
        unsigned submissions = rdna4_audit_submission_count();
        unsigned pipelines = rdna4_audit_compute_pipelines() + rdna4_audit_graphics_pipelines();
        if (!prepared || error || submissions || !pipelines) fail([NSString stringWithFormat:
            @"prepare=%d pipelines=%u submissions=%u error=%@", prepared, pipelines, submissions, error]);
        printf("CORE_IMAGE_PREPARE_PASS filter=%s load=%s pipelines=%u\n", argv[2], argv[3], pipelines);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (!commands) fail(@"native command buffer unavailable");
        CGColorSpaceRef outputSpace = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
        [context render:filtered toMTLTexture:output commandBuffer:commands bounds:filtered.extent colorSpace:outputSpace];
        CGColorSpaceRelease(outputSpace);
        if (commands.error || rdna4_audit_submission_count() || !rdna4_audit_dispatches()) fail([NSString stringWithFormat:
            @"encoding error=%@ submissions=%u dispatches=%u", commands.error, rdna4_audit_submission_count(), rdna4_audit_dispatches()]);
        printf("CORE_IMAGE_ENCODING_PASS dispatches=%u; mock BOs, no submissions, pixels untested\n", rdna4_audit_dispatches());
        // The generic device has no accelerator teardown state.
        _exit(0);
    }
}
