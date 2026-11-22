#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>

@interface NSObject (GarnetMetalInit)
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

static id<MTLDevice> rdna4_direct_device(void) {
    static id<MTLDevice> device;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *path = getenv("RDNA4_METAL_BUNDLE");
        if (!path || path[0] != '/') {
            fprintf(stderr, "RDNA4_METAL_BUNDLE must name the verified bundle\n");
            return;
        }
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(path)];
        if (!bundle || ![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "Bundle load failed: %s\n", error.description.UTF8String);
            return;
        }
        Class cls = [bundle classNamed:@"GarnetDevice"];
        if (!cls) {
            fprintf(stderr, "GarnetDevice class absent from bundle\n");
            return;
        }
        io_service_t accelerator = IOServiceGetMatchingService(kIOMainPortDefault,
                                      IOServiceMatching("GarnetAccelerator"));
        if (!accelerator) {
            fprintf(stderr, "GarnetAccelerator service absent\n");
            return;
        }
        device = [[cls alloc] initWithAcceleratorPort:accelerator];
        // Keep the service port alive for the device's entire process lifetime.
        if (!device) IOObjectRelease(accelerator);
        else fprintf(stderr, "Direct bundle device: %s (class %s)\n",
                     device.name.UTF8String, NSStringFromClass([device class]).UTF8String);
    });
    return device;
}

static NSArray<id<MTLDevice>> *rdna4_direct_devices(void) {
    id<MTLDevice> device = rdna4_direct_device();
    return device ? @[device] : @[];
}

#define MTLCopyAllDevices rdna4_direct_devices
#define MTLCreateSystemDefaultDevice rdna4_direct_device
#include "mtlprobe.m"
