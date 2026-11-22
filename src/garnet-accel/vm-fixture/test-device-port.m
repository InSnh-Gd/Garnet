// Uses the shipping IOAccelerator, not the generic NSObject device initializer.
// Resource encoding uses RADV mock BOs; no command buffer is submitted.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#include <unistd.h>

@interface NSObject (GarnetDevicePortTest)
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
- (void)lazyInitialize;
- (BOOL)rdna4Finish:(NSError **)error;
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "DEVICE_PORT_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void nativeDenied(io_service_t service, const char *stage) {
    io_connect_t native = 0;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0x47524E54u /* 'GRNT' */, &native);
    if (native) IOServiceClose(native);
    // The fixture configures a device but never publishes a hardware transport.
    if (result != kIOReturnNotReady || native)
        fail([NSString stringWithFormat:@"hardware-free native client returned %#x", result]);
    printf("DEVICE_PORT_NATIVE_DENIAL_PASS stage=%s rc=%#x hardware_transport=absent\n", stage, result);
}

static io_service_t fixtureAccelerator(void) {
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("GarnetAccelerator"), &iterator)) return 0;
    io_service_t service, found = 0;
    while ((service = IOIteratorNext(iterator))) {
        io_registry_entry_t parent = 0;
        if (!IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent)) {
            CFTypeRef test = IORegistryEntryCreateCFProperty(parent, CFSTR("VMTestOnly"), kCFAllocatorDefault, 0);
            BOOL match = test && CFGetTypeID(test) == CFBooleanGetTypeID() && CFBooleanGetValue(test);
            if (test) CFRelease(test);
            IOObjectRelease(parent);
            if (match) { found = service; break; }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return found;
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-device-port RADV_BUNDLE deny|allow|enumerate-deny|enumerate-allow\nExercises the shipping accelerator port and Metal initializer in the VM-only fixture. "
             "Ready=0 must decline; allow uses the existing root test override. Resource encoding uses mock BOs, without GPU submissions.");
        return 0;
    }
    if (argc != 3 || (strcmp(argv[2], "deny") && strcmp(argv[2], "allow") &&
                     strcmp(argv[2], "enumerate-deny") && strcmp(argv[2], "enumerate-allow")) || geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    io_service_t service = fixtureAccelerator();
    if (!service) fail(@"no fixture accelerator matched");
    uint64_t registryID = 0;
    if (IORegistryEntryGetRegistryEntryID(service, &registryID)) fail(@"accelerator ID unavailable");
    nativeDenied(service, "before-init");
    @autoreleasepool {
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        unsetenv("RDNA4M_TEST_READY");
        BOOL enumerate = !strncmp(argv[2], "enumerate-", 10);
        BOOL allow = !strcmp(argv[2], "allow") || !strcmp(argv[2], "enumerate-allow");
        if (allow) setenv("RDNA4M_TEST_READY", "1", 1);
        NSError *error = nil;
        id<MTLDevice> device = nil;
        if (enumerate) {
            for (id<MTLDevice> candidate in MTLCopyAllDevices())
                if (candidate.registryID == registryID) { device = candidate; break; }
            printf("DEVICE_PORT_ENUMERATION registryID=%llu found=%u expected=%u\n", registryID, device != nil, allow);
        } else {
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
            device = [[objc_getClass("GarnetDevice") alloc] initWithAcceleratorPort:service];
        }
        if (!allow) {
            if (device) fail(@"Ready=0 admitted a device");
            puts("DEVICE_PORT_DENIAL_PASS Ready=0 nil_device=1 no_crash=1");
        } else {
            if (!device) fail(@"real accelerator-port initialization returned nil");
            if (device.registryID != registryID) fail([NSString stringWithFormat:@"registryID=%llu expected=%llu", device.registryID, registryID]);
            printf("DEVICE_PORT_INIT_PASS class=%s registryID=%llu name=%s\n",
                   class_getName([device class]), registryID, device.name.UTF8String);
            puts("DEVICE_PORT_LAZY_BEGIN");
            [(id)device lazyInitialize];
            puts("DEVICE_PORT_LAZY_PASS");
            nativeDenied(service, "after-lazy");
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
            if (!queue || !buffer || !commands || !encoder) fail(@"resource creation through initialized device failed");
            [encoder fillBuffer:buffer range:NSMakeRange(0, buffer.length) value:0x5a];
            [encoder endEncoding];
            if (![(id)commands rdna4Finish:&error] || error) fail(error.description);
            puts("DEVICE_PORT_ENCODING_PASS real_IOAccelerator_port=1 mock_BOs=1 GPU_submissions=0");
        }
    }
    IOObjectRelease(service);
    puts("DEVICE_PORT_TEARDOWN_PASS");
    return 0;
}
