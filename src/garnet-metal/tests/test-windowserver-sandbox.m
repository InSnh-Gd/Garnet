// Exercise shipping cold pipeline creation under the OS's WindowServer profile
// and uid. RADV uses mock BOs; this does not exercise scanout or GPU submission.
// Build in macOS with clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror
// -framework Foundation -framework Metal test-windowserver-sandbox.m -o test-windowserver-sandbox
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

extern int sandbox_check(pid_t, const char *, int, ...);

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "WS_SANDBOX_FAIL %s\n", message.UTF8String);
    _exit(1);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: sudo test-windowserver-sandbox INSTALLED_BUNDLE compute|render\n"
                 "VM only. Applies the unmodified OS WindowServer sandbox at uid 88; "
                 "compiles a fresh native pipeline through the installed daemon. No GPU submission.");
            return 0;
        }
        if (argc != 3 || geteuid() != 0 ||
            (strcmp(argv[2], "compute") && strcmp(argv[2], "render"))) return 2;
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        if (!(c & (1u << 31))) fail(@"requires a VM");
        setvbuf(stdout, NULL, _IONBF, 0);
        alarm(90);
        setprogname("WindowServer");
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        const char *const testKeys[] = {
                "GARNET_ALLOW", "GARNET_TEST_INPROC",
                "GARNET_TEST_SIDE_DIR", "GARNET_TEST_USERCACHE", "GARNET_TEST_HIDE_BUNDLE_SPV",
                "GARNET_FORCE_FALLBACK", "GARNET_TEST_FALLBACK_AS_WS",
                "RDNA4M_ALLOW", "RDNA4M_TEST_INPROC",
                "RDNA4M_TEST_SIDE_DIR", "RDNA4M_TEST_USERCACHE", "RDNA4M_TEST_HIDE_BUNDLE_SPV",
                "RDNA4M_FORCE_FALLBACK", "RDNA4M_TEST_FALLBACK_AS_WS" };
        for (size_t i = 0; i < sizeof testKeys / sizeof testKeys[0]; ++i) unsetenv(testKeys[i]);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) fail(error.description);
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        if (!device) fail(@"device unavailable");
        NSString *suffix = [NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
        NSString *kernel = [@"sandbox_write_" stringByAppendingString:suffix];
        NSString *vertex = [@"sandbox_vertex_" stringByAppendingString:suffix];
        NSString *fragment = [@"sandbox_fragment_" stringByAppendingString:suffix];
        NSString *source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void %@(device uint *p [[buffer(0)]]) { p[0] = 7u; }\n"
             "vertex float4 %@(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
             "fragment float4 %@() { return float4(0,1,0,1); }\n", kernel, vertex, fragment];
        // WS consumes precompiled AIR. Prepare AIR before entering its sandbox;
        // the native AIR-to-SPIR-V/RADV pipeline work below must remain cold.
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) fail(error.description);
        NSString *profile = [NSString stringWithContentsOfFile:@"/System/Library/Sandbox/Profiles/com.apple.WindowServer.sb"
                                                    encoding:NSUTF8StringEncoding error:&error];
        if (!profile) fail(error.description);
        int (*apply)(const char *, uint64_t, const char *const *, char **) =
            dlsym(RTLD_DEFAULT, "sandbox_init_with_parameters");
        if (!apply) fail(@"sandbox_init_with_parameters unavailable");
        struct passwd *account = getpwnam("_windowserver");
        if (!account || account->pw_uid != 88) fail(@"WindowServer uid is not 88");
        uid_t uid = account->pw_uid;
        gid_t gid = account->pw_gid;
        if (setgroups(1, &gid) || setgid(gid) || setuid(uid)) fail(@"cannot select WindowServer credentials");
        char cache[1024];
        size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, cache, sizeof cache);
        if (!n || n > sizeof cache) fail(@"WindowServer cache directory unavailable");
        const char *parameters[] = { "HOME", "/var/empty", "TMPDIR", "/private/var/tmp", "DARWIN_CACHE_DIR", cache, NULL };
        char *sandboxError = NULL;
        if (apply(profile.UTF8String, 0, parameters, &sandboxError))
            fail([NSString stringWithFormat:@"cannot apply OS profile: %s", sandboxError ?: "unknown"]);
        int active = sandbox_check(getpid(), NULL, 0);
        int sideRead = sandbox_check(getpid(), "file-read-data", 1, "/private/var/tmp/garnet-spv");
        int mapBundle = sandbox_check(getpid(), "file-map-executable", 1, bundle.executablePath.fileSystemRepresentation);
        int outsideWrite = sandbox_check(getpid(), "file-write-data", 1, "/Library/Logs/GarnetTranslate");
        printf("WS_SANDBOX_CONTROLS active=%d side_read=%d bundle_map=%d outside_write=%d\n",
               active, sideRead, mapBundle, outsideWrite);
        if (!active || sideRead || mapBundle || !outsideWrite)
            fail(@"sandbox positive/negative controls failed");
        printf("WS_SANDBOX_ACTIVE uid=%u gid=%u profile=unmodified_OS_WindowServer native_submission=untested\n",
               (unsigned)geteuid(), (unsigned)getegid());
        id pipeline = nil;
        error = nil;
        if (!strcmp(argv[2], "compute")) {
            pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:kernel] error:&error];
        } else {
            MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:vertex];
            descriptor.fragmentFunction = [library newFunctionWithName:fragment];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        }
        if (!pipeline || error) fail([NSString stringWithFormat:@"cold %@ pipeline=%@ error=%@", @(argv[2]), pipeline, error]);
        printf("WS_SANDBOX_PIPELINE_PASS mode=%s mock_BO=1 native_submission=untested\n", argv[2]);
        _exit(0);
    }
}
