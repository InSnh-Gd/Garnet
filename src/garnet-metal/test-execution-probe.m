// Reuse the existing pixel oracles without interposing Metal symbols in MoltenVK.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unistd.h>

static id<MTLDevice> executionDevice;
static id<MTLDevice> executionProbeDevice(void) { return executionDevice; }
static NSArray<id<MTLDevice>> *executionProbeDevices(void) { return @[ executionDevice ]; }

#define MTLCreateSystemDefaultDevice executionProbeDevice
#define MTLCopyAllDevices executionProbeDevices
#define main executionProbeMain
#include "../../tools/native/mtlprobe/mtlprobe.m"
#undef main
#undef MTLCopyAllDevices
#undef MTLCreateSystemDefaultDevice

static int executionProbeFetchBuiltinGuards(BOOL reference) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 plain_vs(uint v [[vertex_id]]) { return float4(float(v), 0, 0, 1); }\n"
        "vertex float4 base_vertex_vs(uint v [[vertex_id]], uint b [[base_vertex]]) { return float4(float(v-b), 0, 0, 1); }\n"
        "vertex float4 base_instance_vs(uint i [[instance_id]], uint b [[base_instance]]) { return float4(float(i-b), 0, 0, 1); }\n"
        "fragment float4 fetch_fs(float4 color [[color(0)]]) { return 1-color; }\n"
        "fragment float4 primitive_fs(float4 color [[color(0)]], uint p [[primitive_id]]) { return color+float(p); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [executionDevice newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "FETCH_BUILTIN_FAIL library: %s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *control = [MTLRenderPipelineDescriptor new];
    control.vertexFunction = [library newFunctionWithName:@"plain_vs"];
    control.fragmentFunction = [library newFunctionWithName:@"fetch_fs"];
    control.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (![executionDevice newRenderPipelineStateWithDescriptor:control error:&error] || error) {
        fprintf(stderr, "FETCH_BUILTIN_FAIL control: %s\n", error.description.UTF8String); return 1;
    }
    NSArray<NSString *> *names = @[ @"base_vertex", @"base_instance", @"primitive_id" ];
    for (NSUInteger i = 0; i < names.count; i++) {
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:i == 0 ? @"base_vertex_vs" : i == 1 ? @"base_instance_vs" : @"plain_vs"];
        descriptor.fragmentFunction = [library newFunctionWithName:i == 2 ? @"primitive_fs" : @"fetch_fs"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (!descriptor.vertexFunction || !descriptor.fragmentFunction) return 1;
        error = nil;
        id<MTLRenderPipelineState> pipeline = [executionDevice newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if ((reference && (!pipeline || error)) || (!reference && (pipeline || ![error.domain isEqual:@"GarnetMetal"] || error.code != (i == 2 ? 59 : 41)))) {
            fprintf(stderr, "FETCH_BUILTIN_FAIL name=%s reference=%d pipeline=%p error=%s\n",
                    names[i].UTF8String, reference, (__bridge void *)pipeline, error.description.UTF8String);
            return 1;
        }
        printf("FETCH_BUILTIN_PASS name=%s reference=%d accepted=%d error=%s\n",
               names[i].UTF8String, reference, pipeline != nil, error ? error.description.UTF8String : "none");
    }
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-execution-probe VM_TEST_BUNDLE|--reference COMMAND [ARGS]\nRuns existing mtlprobe oracles on a probe-local device. Native mode requires root and a test-only executable Vulkan ICD; native hardware transport is not tested.");
            return 0;
        }
        if (argc < 3) return 2;
        BOOL reference = !strcmp(argv[1], "--reference");
        if (!reference && (argv[1][0] == '-' || geteuid() != 0)) return 2;
        alarm(120);
        if (reference) executionDevice = MTLCreateSystemDefaultDevice();
        else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
            // MoltenVK's placed-heap index buffers fail in this VM. Keep our
            // Vulkan suballocation pool enabled and use MTLBuffer backing here.
            setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
            NSError *error = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) {
                fprintf(stderr, "PROBE_EXECUTION_FAIL bundle: %s\n", error.description.UTF8String);
                _exit(1);
            }
            executionDevice = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(executionDevice, sel_registerName("initLimits"));
        }
        if (!executionDevice) {
            fputs("PROBE_EXECUTION_FAIL no Metal device\n", stderr);
            _exit(1);
        }
        printf("PROBE_EXECUTION_DEVICE class=%s reference=%d command=%s\n",
               class_getName([executionDevice class]), reference, argv[2]);
        if (!strcmp(argv[2], "fetch-builtin-guards")) _exit(executionProbeFetchBuiltinGuards(reference));
        argv[1] = argv[0];
        int result = executionProbeMain(argc - 1, argv + 1);
        printf("PROBE_EXECUTION_EXIT=%d command=%s\n", result, argv[2]);
        // The generic native device has no accelerator teardown state.
        _exit(result);
    }
}
