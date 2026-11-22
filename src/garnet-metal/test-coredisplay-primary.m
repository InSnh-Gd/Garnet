#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <assert.h>

static CFStringRef framebufferClass = CFSTR("AMDGarnetPrimaryFramebuffer");
static CFStringRef framebufferBundle = CFSTR("org.garnet.driver");
static uint32_t vendor = 0x1002, device = 0x7590;
static BOOL bindingMatches = YES, acceleratorMatches = YES;
static unsigned nextFramebuffer;

static boolean_t testConforms(io_object_t object, const io_name_t name) {
    return (object == 10 && acceleratorMatches && !strcmp(name, "GarnetAccelerator")) ||
           (object == 30 && !strcmp(name, "IOPCIDevice"));
}
static kern_return_t testMatching(mach_port_t port, CFDictionaryRef dictionary, io_iterator_t *iterator) {
    (void)port;
    CFRelease(dictionary);
    nextFramebuffer = 0;
    *iterator = 40;
    return KERN_SUCCESS;
}
static io_object_t testNext(io_iterator_t iterator) {
    assert(iterator == 40);
    return nextFramebuffer++ == 0 ? 20 : 0;
}
static CFStringRef testClass(io_object_t object) {
    assert(object == 20);
    return CFRetain(framebufferClass);
}
static CFTypeRef testProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, IOOptionBits options) {
    (void)allocator;
    (void)options;
    if (entry == 20 && CFEqual(key, CFSTR("CFBundleIdentifier"))) return CFRetain(framebufferBundle);
    if (entry == 20 && CFEqual(key, CFSTR("IOAccelTypes"))) return CFRetain(CFSTR("IOService:/fixture/accelerator"));
    if (entry == 30 && CFEqual(key, CFSTR("vendor-id"))) return CFDataCreate(NULL, (const UInt8 *)&vendor, 4);
    if (entry == 30 && CFEqual(key, CFSTR("device-id"))) return CFDataCreate(NULL, (const UInt8 *)&device, 4);
    return NULL;
}
static io_registry_entry_t testFromPath(mach_port_t port, const io_string_t path) {
    (void)port;
    assert(!strcmp(path, "IOService:/fixture/accelerator"));
    return bindingMatches ? 10 : 11;
}
static boolean_t testEqual(io_object_t first, io_object_t second) { return first == second; }
static kern_return_t testParent(io_registry_entry_t entry, const io_name_t plane, io_registry_entry_t *parent) {
    assert(entry == 20 && !strcmp(plane, kIOServicePlane));
    *parent = 30;
    return KERN_SUCCESS;
}
static kern_return_t testRelease(io_object_t object) { (void)object; return KERN_SUCCESS; }

#define IOObjectConformsTo testConforms
#define IOServiceGetMatchingServices testMatching
#define IOIteratorNext testNext
#define IOObjectCopyClass testClass
#define IORegistryEntryCreateCFProperty testProperty
#define IORegistryEntryFromPath testFromPath
#define IOObjectIsEqualTo testEqual
#define IORegistryEntryGetParentEntry testParent
#define IOObjectRelease testRelease
#define RDNA4LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "rdna4_coredisplay_compat.h"

int main(void) {
    @autoreleasepool {
        (void)rdna4_coredisplay_compat;
        assert(rdna4_cd_bound_primary(10));
        device = 0x7550;
        assert(rdna4_cd_bound_primary(10));
        device = 0x7551;
        assert(rdna4_cd_bound_primary(10));
        device = 0x1234;
        assert(!rdna4_cd_bound_primary(10));
        device = 0x7590;
        framebufferClass = CFSTR("IONDRVFramebuffer");
        assert(!rdna4_cd_bound_primary(10));
        framebufferClass = CFSTR("GarnetPrimaryFramebuffer");
        assert(!rdna4_cd_bound_primary(10));
        framebufferClass = CFSTR("AMDGarnetPrimaryFramebuffer");
        framebufferBundle = CFSTR("other.driver");
        assert(!rdna4_cd_bound_primary(10));
        framebufferBundle = CFSTR("org.garnet.driver");
        bindingMatches = NO;
        assert(!rdna4_cd_bound_primary(10));
        bindingMatches = YES;
        vendor = 0x8086;
        assert(!rdna4_cd_bound_primary(10));
        vendor = 0x1002; acceleratorMatches = NO;
        assert(!rdna4_cd_bound_primary(10));
        acceleratorMatches = YES;
        assert(!rdna4_cd_bound_primary(0));
        assert(rdna4_cd_bound_primary(10));
        puts("COREDISPLAY_PRIMARY_PASS cases=12 registry_fixture=1 gpu_execution=0");
        return 0;
    }
}
