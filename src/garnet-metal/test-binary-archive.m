#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dispatch/dispatch.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint16_t major; uint8_t minor; uint8_t update; } TargetArchVersion;
_Static_assert(sizeof(TargetArchVersion) == 4, "Metal target version ABI");

// Every Metal loader of native-code containers calls +[MTLLoader sliceIDForDevice:...], which asserts when the
// device's -targetDeviceArchitecture is nil (the iconservicesagent / Spotlight / Dock-extra crash loop of b42).
// Modes:
//   empty|renderbox|missing  -[device newBinaryArchiveWithDescriptor:error:] declines with an NSError
//   base-renderbox           Metal's own MTLIOAccelDevice implementation (bypassing our override) on RenderBox's archive
//   archive-type             +[_MTLBinaryArchive archiveTypeAtURL:device:error:] on RenderBox's archive
//   dylib                    -[device newDynamicLibraryWithURL:error:] declines
//   mtl4                     -[device newArchiveWithURL:error:] declines (if Metal 4 exists)
//   arch                     -targetDeviceArchitecture is non-nil and not an Apple GPU slice
//   fallback                 decline, compile source, then verify two real dispatches
// Without the fix base-renderbox and archive-type abort inside sliceIDForDevice (exit by SIGABRT); with it they return.
static NSString *const kRenderBox = @"/System/Library/PrivateFrameworks/RenderBox.framework/Resources/archive.metallib";

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void compileAndExecute(id<MTLDevice> device) {
    ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
    NSError *error = nil;
    double start = seconds();
    id<MTLLibrary> library = [device newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void archive_fallback(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] = i * 7u + 3u; }\n"
        options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"archive_fallback"];
    id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline || error) {
        fprintf(stderr, "ARCHIVE_FAIL online compilation: %s\n", error.description.UTF8String ?: "no pipeline");
        _exit(1);
    }
    printf("ARCHIVE_ONLINE_COMPILE_PASS seconds=%.6f\n", seconds() - start);
    id<MTLBuffer> output = [device newBufferWithLength:16 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!output.contents || !queue) { fputs("ARCHIVE_FAIL execution resources\n", stderr); _exit(1); }
    for (unsigned run = 0; run < 2; ++run) {
        memset(output.contents, 0xa7, output.length);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!encoder) { fputs("ARCHIVE_FAIL compute encoder\n", stderr); _exit(1); }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        start = seconds();
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted || command.error) {
            fprintf(stderr, "ARCHIVE_FAIL dispatch: %s\n", command.error.description.UTF8String ?: "not completed");
            _exit(1);
        }
        const uint32_t *words = output.contents;
        for (unsigned i = 0; i < 16; ++i) {
            if (words[i] != i * 7u + 3u) {
                fprintf(stderr, "ARCHIVE_FAIL output[%u]=%u expected=%u\n", i, words[i], i * 7u + 3u);
                _exit(1);
            }
        }
        printf("ARCHIVE_EXECUTION_PASS run=%s words=16 seconds=%.6f\n", run ? "warm" : "cold", seconds() - start);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSArray *modes = @[ @"empty", @"renderbox", @"missing", @"base-renderbox", @"archive-type", @"dylib", @"mtl4", @"arch", @"fallback" ];
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            printf("Usage: test-binary-archive BUNDLE %s [DEVICE_CLASS]\n", [modes componentsJoinedByString:@"|"].UTF8String);
            return 0;
        }
        if ((argc != 3 && argc != 4) || ![modes containsObject:@(argv[2])]) return 2;
        const char *mode = argv[2];
        const char *className = argc == 4 ? argv[3] : "GarnetDevice";
        alarm(!strcmp(mode, "fallback") ? 90 : 30);
        setvbuf(stdout, NULL, _IONBF, 0);
        setenv("RDNA4M_ALLOW", "1", 1);
        if (!strcmp(mode, "fallback")) setenv("RDNA4M_TEST_INPROC", "1", 1);
        // System processes run with fatal Metal failures (the b42 crash); make MTLReportFailure abort here too.
        if (!getenv("MTL_ERROR_MODE")) setenv("MTL_ERROR_MODE", "assert", 1);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "ARCHIVE_FAIL load: %s\n", error.description.UTF8String);
            return 1;
        }
        Class devClass = objc_getClass(className);
        id<MTLDevice> device = [[devClass alloc] init];
        if (!device) { fprintf(stderr, "ARCHIVE_FAIL no %s\n", className); return 1; }
        NSURL *rb = [NSURL fileURLWithPath:kRenderBox];
        BOOL needRB = !strcmp(mode, "renderbox") || !strcmp(mode, "base-renderbox") || !strcmp(mode, "archive-type");
        if (needRB && ![[NSFileManager defaultManager] fileExistsAtPath:kRenderBox]) return 2;
        printf("ARCHIVE_BEGIN mode=%s class=%s\n", mode, className);
        if (!strcmp(mode, "arch")) {
            for (NSString *query in @[ @"supportsDynamicLibraries", @"supportsRenderDynamicLibraries", @"supportsBinaryArchives" ]) {
                SEL sel = NSSelectorFromString(query);
                if (![(id)device respondsToSelector:sel] || ((BOOL (*)(id, SEL))objc_msgSend)(device, sel)) {
                    fprintf(stderr, "ARCHIVE_FAIL %s does not decline native code support\n", query.UTF8String); _exit(1);
                }
            }
            id arch = [(id)device respondsToSelector:@selector(targetDeviceArchitecture)] ? [(id)device performSelector:@selector(targetDeviceArchitecture)] : nil;
            if (!arch) { fprintf(stderr, "ARCHIVE_FAIL targetDeviceArchitecture is nil\n"); _exit(1); }
            uint32_t cpu = ((uint32_t (*)(id, SEL))objc_msgSend)(arch, @selector(cpuType));
            uint32_t sub = ((uint32_t (*)(id, SEL))objc_msgSend)(arch, @selector(subType));
            if (cpu == 0x1000013u || cpu == 0 || cpu == 0xffffffffu) { fprintf(stderr, "ARCHIVE_FAIL cpuType 0x%x matches shipped slices\n", cpu); _exit(1); }
            TargetArchVersion version = ((TargetArchVersion (*)(id, SEL))objc_msgSend)(arch, @selector(version));
            if (cpu != 0x01000044u || sub != 4 || version.major != 1 || version.minor || version.update) {
                fprintf(stderr, "ARCHIVE_FAIL architecture fields or version ABI\n"); _exit(1);
            }
            dispatch_apply(64, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(size_t i) {
                (void)i;
                id again = ((id (*)(id, SEL))objc_msgSend)(device, @selector(targetDeviceArchitecture));
                if (again != arch) { fprintf(stderr, "ARCHIVE_FAIL unstable architecture identity\n"); _exit(1); }
            });
            printf("ARCHIVE_PASS mode=arch cpuType 0x%x subType 0x%x\n", cpu, sub);
            _exit(0);
        }
        if (!strcmp(mode, "base-renderbox")) {
            // Metal's own implementation, as reached by any caller of a superclass IMP: must not abort.
            Class base = objc_getClass("MTLIOAccelDevice");
            SEL sel = @selector(newBinaryArchiveWithDescriptor:error:);
            IMP imp = class_getMethodImplementation(base, sel);
            MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
            d.url = rb;
            id a = ((id (*)(id, SEL, id, NSError **))imp)(device, sel, d, &error);
            printf("ARCHIVE_PASS mode=base-renderbox archive=%s error=%s (no assertion)\n", a ? "created" : "nil", error ? error.localizedDescription.UTF8String : "none");
            _exit(0);
        }
        if (!strcmp(mode, "archive-type")) {
            Class c = objc_getClass("_MTLBinaryArchive");
            SEL sel = NSSelectorFromString(@"archiveTypeAtURL:device:error:");
            if (!c || ![c respondsToSelector:sel]) { printf("ARCHIVE_SKIP archiveTypeAtURL:device:error: not present\n"); _exit(0); }
            long t = ((long (*)(id, SEL, id, id, NSError **))objc_msgSend)(c, sel, rb, device, &error);
            printf("ARCHIVE_PASS mode=archive-type type=%ld error=%s (no assertion)\n", t, error ? error.localizedDescription.UTF8String : "none");
            _exit(0);
        }
        if (!strcmp(mode, "dylib")) {
            id l = [device newDynamicLibraryWithURL:rb error:&error];
            if (l || ![error.domain isEqualToString:MTLDynamicLibraryDomain] || error.code != MTLDynamicLibraryErrorUnsupported) {
                fprintf(stderr, "ARCHIVE_FAIL dylib URL result/error\n"); _exit(1);
            }
            error = nil;
            // The override rejects before inspecting the library, including absent input.
            l = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(device, @selector(newDynamicLibrary:error:), nil, &error);
            if (l || ![error.domain isEqualToString:MTLDynamicLibraryDomain] || error.code != MTLDynamicLibraryErrorUnsupported ||
                ((id (*)(id, SEL, id, NSError **))objc_msgSend)(device, @selector(newDynamicLibrary:error:), nil, NULL) ||
                [device newDynamicLibraryWithURL:rb error:NULL]) {
                fprintf(stderr, "ARCHIVE_FAIL dylib library result/error\n"); _exit(1);
            }
            printf("ARCHIVE_PASS mode=dylib declined: %s\n", error.localizedDescription.UTF8String);
            _exit(0);
        }
        if (!strcmp(mode, "mtl4")) {
            SEL sel = NSSelectorFromString(@"newArchiveWithURL:error:");
            if (![(id)device respondsToSelector:sel]) { printf("ARCHIVE_SKIP no Metal 4 archives\n"); _exit(0); }
            id a = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(device, sel, rb, &error);
            if (a || !error) { fprintf(stderr, "ARCHIVE_FAIL mtl4 archive=%s\n", a ? "created" : "nil without error"); _exit(1); }
            printf("ARCHIVE_PASS mode=mtl4 declined: %s\n", error.localizedDescription.UTF8String);
            _exit(0);
        }

        MTLBinaryArchiveDescriptor *descriptor = [MTLBinaryArchiveDescriptor new];
        if (!strcmp(mode, "renderbox")) descriptor.url = rb;
        else if (!strcmp(mode, "missing")) descriptor.url = [NSURL fileURLWithPath:@"/nonexistent/rdna4-binary-archive.metallib"];
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (archive || ![error.domain isEqualToString:MTLBinaryArchiveDomain] ||
            error.code == MTLBinaryArchiveErrorNone) {
            fprintf(stderr, "ARCHIVE_FAIL archive=%s error=%s\n", archive ? "created" : "nil", error.description.UTF8String);
            _exit(1);
        }
        // Frameworks can compile their AIR when this optional native-code cache is unavailable.
        error = nil;
        if ([device newBinaryArchiveWithDescriptor:descriptor error:NULL]) _exit(1);
        if (!strcmp(mode, "fallback")) {
            puts("ARCHIVE_DECLINE_PASS mode=fallback");
            compileAndExecute(device);
        }
        printf("ARCHIVE_PASS mode=%s optional archive declined without a Metal assertion\n", mode);
        _exit(0);
    }
}
