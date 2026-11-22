// Real 25G332 SPDisplaysReporter, with the native accelerator's registry graph mocked.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdatomic.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
static unsigned pciDevice = 0x7590, retained, resources;
static uint64_t deviceRegistry = 0x123456;
static NSString *owner = @"org.garnet.accel", *plugin = @"GarnetMetal";
static BOOL mac2 = YES;
static const char *process = "system_profiler";
static boolean_t reportConforms(io_object_t entry, const io_name_t name) {
    return (entry == 42 && !strcmp(name, "GarnetAccelerator")) || (entry == 30 && !strcmp(name, "IOPCIDevice"));
}
static kern_return_t reportRetain(io_object_t entry) { (void)entry; retained++; return KERN_SUCCESS; }
static kern_return_t reportRelease(io_object_t entry) { (void)entry; retained--; return KERN_SUCCESS; }
static kern_return_t reportParent(io_registry_entry_t entry, const io_name_t plane, io_registry_entry_t *parent) {
    (void)plane; if (entry == 42) *parent = 41; else if (entry == 41) *parent = 40; else if (entry == 40) *parent = 30; else return kIOReturnNotFound;
    retained++; return KERN_SUCCESS;
}
static kern_return_t reportPath(io_registry_entry_t entry, const io_name_t plane, io_string_t path) {
    (void)plane;
    if (entry == 30) strlcpy(path, "IOService:/mock/display@0", sizeof(io_string_t));
    else if (entry == 42) strlcpy(path, "IOService:/mock/display@0/GarnetDriver/GarnetMetalNub/GarnetAccelerator", sizeof(io_string_t));
    else return kIOReturnNotFound;
    return KERN_SUCCESS;
}
static kern_return_t reportRegistry(io_registry_entry_t entry, uint64_t *value) {
    if (entry != 42) return kIOReturnNotFound;
    *value = deviceRegistry; return KERN_SUCCESS;
}
static CFTypeRef reportProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, IOOptionBits opts) {
    (void)allocator; (void)opts;
    if (entry == 42) {
        if (CFEqual(key, CFSTR("CFBundleIdentifier"))) return CFRetain((__bridge CFTypeRef)owner);
        if (CFEqual(key, CFSTR("MetalPluginName"))) return CFRetain((__bridge CFTypeRef)plugin);
        if (CFEqual(key, CFSTR("MetalPluginClassName"))) return CFRetain(CFSTR("GarnetDevice"));
    }
    uint32_t value;
    if (entry == 30 && CFEqual(key, CFSTR("vendor-id"))) value = 0x1002;
    else if (entry == 30 && CFEqual(key, CFSTR("device-id"))) value = pciDevice;
    else return NULL;
    return CFDataCreate(kCFAllocatorDefault, (const UInt8 *)&value, sizeof value);
}
static const char *reportProcess(void) { return process; }
#define getprogname reportProcess
#define IOObjectConformsTo reportConforms
#define IOObjectRetain reportRetain
#define IOObjectRelease reportRelease
#define IORegistryEntryGetParentEntry reportParent
#define IORegistryEntryGetPath reportPath
#define IORegistryEntryGetRegistryEntryID reportRegistry
#define IORegistryEntryCreateCFProperty reportProperty
#include "rdna4_system_report_impl.inc"
@interface ReportDevice : NSObject
@end
@implementation ReportDevice
- (uint64_t)registryID { return 0x123456; }
- (NSString *)name { return @"AMD Radeon RX 9060 XT"; }
- (BOOL)supportsFamily:(MTLGPUFamily)family { return mac2 && family == MTLGPUFamilyMac2; }
- (id)newCommandQueue { resources++; return nil; }
@end
static NSDictionary *dictionary(NSString *path, NSString *framebufferOwner, BOOL hasModel) {
    uint32_t cls = 0x030000, device = 0x7590, vendor = 0x1002;
    id reporter = [[NSClassFromString(@"SPDisplaysDevice") alloc] init];
    NSMutableDictionary *properties = [@{
        @"class-code":[NSData dataWithBytes:&cls length:4], @"device-id":[NSData dataWithBytes:&device length:4],
        @"vendor-id":[NSData dataWithBytes:&vendor length:4], @"CFBundleIdentifier":framebufferOwner,
        @"io_registry_entry_name_key":@"display", @"io_registry_entry_path_key":path } mutableCopy];
    if (hasModel) properties[@"model"] = [@"AMD Radeon RX 9060 XT" dataUsingEncoding:NSUTF8StringEncoding];
    [reporter setValue:properties forKey:@"properties"];
    [reporter setValue:[NSMutableArray array] forKey:@"children"];
    NSDictionary *result = ((id(*)(id,SEL))objc_msgSend)(reporter,sel_getUid("dictionaryRepresentation"));
    NSCAssert([properties[@"CFBundleIdentifier"] isEqual:framebufferOwner], @"changed actual framebuffer identity");
    return result;
}
#define CHECK(name, cond) do { BOOL ok = (cond); printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) failures++; } while (0)
int main(int argc, const char *argv[]) { @autoreleasepool {
    BOOL baseline = argc == 2 && !strcmp(argv[1], "--baseline");
    BOOL lateLoad = argc == 2 && !strcmp(argv[1], "--late-load");
    if (argc > 2 || (argc == 2 && !baseline && !lateLoad)) return 2;
    unsigned failures = 0;
    id device = [ReportDevice new];
    if (lateLoad) rdna4sp_register(device, 42);
    NSBundle *reporter = [NSBundle bundleWithPath:@"/System/Library/SystemProfiler/SPDisplaysReporter.spreporter"];
    NSError *loadError = nil;
    if (![reporter loadAndReturnError:&loadError]) { puts(loadError.description.UTF8String); return 2; }
    if (!baseline && !lateLoad) rdna4sp_register(device, 42);
    NSDictionary *result = dictionary(@"IOService:/mock/display@0", @"com.apple.iokit.IONDRVSupport", YES);
    CHECK("real reporter identifies the actual loaded native accelerator owner", [result[@"sppci_kextinfo"] isEqual:@"org.garnet.accel"]);
    CHECK("reported Metal family stays at the implemented Metal 2 level", [result[@"spdisplays_mtlgpufamilysupport"] isEqual:@"spdisplays_metal2"]);
    CHECK("report is associated with the exact Metal accelerator registry identity", [result[@"_spdisplays_regid"] isEqual:@"123456"] && [result[@"_metal_path"] hasSuffix:@"/GarnetAccelerator"]);
    result = dictionary(@"IOService:/mock/display@0", @"org.garnet.driver", NO);
    CHECK("Garnet framebuffer report uses the verified GPU model", [result[@"sppci_model"] isEqual:[device name]] && [result[@"_name"] isEqual:[device name]]);
    CHECK("Garnet framebuffer report uses the verified Metal family", [result[@"spdisplays_mtlgpufamilysupport"] isEqual:@"spdisplays_metal2"]);
    CHECK("Garnet framebuffer report uses the verified accelerator owner", [result[@"sppci_kextinfo"] isEqual:@"org.garnet.accel"]);
    CHECK("another GPU's report is unchanged", [dictionary(@"IOService:/mock/other@0", @"com.apple.iokit.IONDRVSupport", YES)[@"sppci_kextinfo"] isEqual:@"sppci_kextnotloaded"]);
    CHECK("another framebuffer owner is unchanged", ![dictionary(@"IOService:/mock/display@0", @"org.other.framebuffer", NO)[@"sppci_kextinfo"] isEqual:@"org.garnet.accel"]);
    mac2 = NO;
    if (!baseline) rdna4sp_register(device, 42);
    CHECK("an admitted device without a supported family does not advertise Metal 2", !dictionary(@"IOService:/mock/display@0", @"com.apple.iokit.IONDRVSupport", NO)[@"spdisplays_mtlgpufamilysupport"]);
    mac2 = YES;
    for (unsigned which = 0; which < 5; which++) {
        pthread_mutex_lock(&rdna4sp_mu); [rdna4sp_devices removeAllObjects]; pthread_mutex_unlock(&rdna4sp_mu);
        pciDevice = which == 0 ? 0x164e : 0x7590;
        owner = which == 1 ? @"com.other.driver" : @"org.garnet.accel";
        plugin = which == 2 ? @"OtherMetal" : @"GarnetMetal";
        deviceRegistry = which == 3 ? 0x9999 : 0x123456;
        process = which == 4 ? "SomeOtherApp" : "system_profiler";
        rdna4sp_register(device, 42);
        CHECK("unverified PCI/owner/plugin/registry/process does not change reporting", [dictionary(@"IOService:/mock/display@0", @"com.apple.iokit.IONDRVSupport", YES)[@"sppci_kextinfo"] isEqual:@"sppci_kextnotloaded"]);
    }
    CHECK("registry references balanced and no GPU resources opened", retained == 0 && resources == 0);
    printf("SYSTEM_REPORT_%s failures=%u registry=mocked GPU=unused\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
} }
