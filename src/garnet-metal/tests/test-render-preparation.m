// Reuse the pixel tests' exact system shaders, descriptors and draw encoding.
// Stop at commit after finishing the real RADV command buffer: no pixel claim.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>

extern BOOL rdna4_audit_has_device(void);
extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_graphics_pipelines(void);
extern unsigned rdna4_audit_draws(void);
extern unsigned rdna4_audit_invalid_attachments(void);

static id<MTLDevice> preparationDevice;
static NSString *preparationCommand;
static unsigned minimumPipelines;
static id<MTLDevice> preparationProbeDevice(void) { return preparationDevice; }
static NSArray<id<MTLDevice>> *preparationProbeDevices(void) { return @[ preparationDevice ]; }

#define MTLCreateSystemDefaultDevice preparationProbeDevice
#define MTLCopyAllDevices preparationProbeDevices
#define main preparationProbeMain
#include "../mtlprobe/mtlprobe.m"
#undef main
#undef MTLCopyAllDevices
#undef MTLCreateSystemDefaultDevice

@interface NSObject (RDNA4RenderPreparation)
- (BOOL)rdna4Finish:(NSError **)error;
@end

static void preparationCommit(id commands, SEL selector) {
    (void)selector;
    NSError *error = nil;
    unsigned before = rdna4_audit_submission_count();
    BOOL finished = [commands rdna4Finish:&error];
    unsigned pipelines = rdna4_audit_graphics_pipelines(), draws = rdna4_audit_draws();
    if (!finished || error || !rdna4_audit_has_device() || pipelines < minimumPipelines || !draws || rdna4_audit_invalid_attachments() ||
        rdna4_audit_submission_count() != before) {
        fprintf(stderr, "RENDER_PREPARATION_FAIL command=%s finished=%d pipelines=%u draws=%u error=%s\n",
                preparationCommand.UTF8String, finished, pipelines, draws, error.description.UTF8String);
        _exit(1);
    }
    printf("RENDER_PREPARATION_PASS command=%s pipelines=%u draws=%u simulated_uploads=%u; "
           "real RADV/ACO, mock BOs, render not submitted, pixels untested\n",
           preparationCommand.UTF8String, pipelines, draws, before);
    _exit(0);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-render-preparation RADV_BUNDLE sysdraw|skydraw|fbsover|fbcopy|fbfetch|depth|msaa\n"
                 "Requires root in macOS. Compiles and encodes the existing render tests using RADV/ACO. "
                 "Mock BOs; no GPU work or pixel validation.");
            return 0;
        }
        NSDictionary<NSString *, NSNumber *> *commands = @{
            @"sysdraw": @1, @"skydraw": @1, @"fbsover": @2, @"fbcopy": @3,
            @"fbfetch": @2, @"depth": @1, @"msaa": @1
        };
        if (argc != 3 || !commands[@(argv[2])] || geteuid() != 0) return 2;
        preparationCommand = @(argv[2]);
        minimumPipelines = commands[preparationCommand].unsignedIntValue;
        alarm(120);
        setenv("RDNA4_AUDIT_SUBMIT", "complete", 1);
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        NSError *error = nil;
        if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) {
            fprintf(stderr, "RENDER_PREPARATION_FAIL bundle: %s\n", error.description.UTF8String);
            return 1;
        }
        Class bufferClass = objc_getClass("RDNA4CommandBuffer");
        Method commit = class_getInstanceMethod(bufferClass, @selector(commit));
        if (!commit) return 1;
        method_setImplementation(commit, (IMP)preparationCommit);
        preparationDevice = [[objc_getClass("GarnetDevice") alloc] init];
        if (!preparationDevice) return 1;
        ((void (*)(id, SEL))objc_msgSend)(preparationDevice, sel_registerName("initLimits"));
        argv[1] = argv[0];
        int result = preparationProbeMain(argc - 1, argv + 1);
        fprintf(stderr, "RENDER_PREPARATION_FAIL command=%s returned=%d without reaching commit\n",
                preparationCommand.UTF8String, result);
        _exit(1);
    }
}
