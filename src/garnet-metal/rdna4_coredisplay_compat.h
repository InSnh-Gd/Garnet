// Version-independent CoreDisplay verification for the bound AMD primary framebuffer.
// Naming the class AMDGarnetPrimaryFramebuffer makes CoreDisplay's native vendor
// classifier recognize the framebuffer without dyld shared cache offset patching
// (technique credited to RX4Darwin/AMDRadeonRDNA4.kext commit b4751220, BSD 3-Clause).
#ifndef RDNA4_COREDISPLAY_COMPAT_H
#define RDNA4_COREDISPLAY_COMPAT_H

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rdna4_cd_pci_word(io_service_t service, CFStringRef key, uint32_t *out) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    int ok = 0;
    if (value && CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength(value) == 4) {
        memcpy(out, CFDataGetBytePtr(value), sizeof *out);
        ok = 1;
    }
    if (value) CFRelease(value);
    return ok;
}

static int rdna4_cd_is_rdna4_device(uint32_t device_id) {
    return device_id == 0x7590 || device_id == 0x7550 || device_id == 0x7551;
}

static int rdna4_cd_bound_primary(io_service_t accelerator) {
    io_iterator_t iterator = 0;
    if (!accelerator || !IOObjectConformsTo(accelerator, "GarnetAccelerator") ||
        IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOFramebuffer"), &iterator) != KERN_SUCCESS) return 0;
    unsigned matches = 0;
    io_service_t fb;
    while ((fb = IOIteratorNext(iterator))) {
        CFStringRef cls = IOObjectCopyClass(fb);
        CFTypeRef bundle = IORegistryEntryCreateCFProperty(fb, CFSTR("CFBundleIdentifier"),
                                                          kCFAllocatorDefault, 0);
        int valid = cls && CFEqual(cls, CFSTR("AMDGarnetPrimaryFramebuffer")) &&
            bundle && CFEqual(bundle, CFSTR("org.garnet.driver"));
        if (cls) CFRelease(cls);
        if (bundle) CFRelease(bundle);
        CFTypeRef path = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelTypes"),
                                                        kCFAllocatorDefault, 0);
        char name[1024];
        if (valid && path && CFGetTypeID(path) == CFStringGetTypeID() &&
            CFStringGetCString(path, name, sizeof name, kCFStringEncodingUTF8)) {
            io_service_t bound = IORegistryEntryFromPath(kIOMainPortDefault, name);
            valid = bound && IOObjectIsEqualTo(bound, accelerator);
            if (bound) IOObjectRelease(bound);
        } else valid = 0;
        if (path) CFRelease(path);
        io_registry_entry_t pci = 0;
        if (valid && IORegistryEntryGetParentEntry(fb, kIOServicePlane, &pci) == KERN_SUCCESS) {
            uint32_t vendor = 0, device = 0;
            valid = IOObjectConformsTo(pci, "IOPCIDevice") &&
                rdna4_cd_pci_word(pci, CFSTR("vendor-id"), &vendor) && vendor == 0x1002 &&
                rdna4_cd_pci_word(pci, CFSTR("device-id"), &device) &&
                rdna4_cd_is_rdna4_device(device);
            IOObjectRelease(pci);
        } else valid = 0;
        matches += valid != 0;
        IOObjectRelease(fb);
    }
    IOObjectRelease(iterator);
    return matches == 1;
}

static void rdna4_coredisplay_compat(io_service_t accelerator) {
#if defined(__x86_64__)
    if (!accelerator || geteuid() != 88 || strcmp(getprogname(), "WindowServer")) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        int ok = rdna4_cd_bound_primary(accelerator);
        RDNA4LOG("CoreDisplay compatibility: bound AMDGarnetPrimaryFramebuffer verified=%d", ok);
    });
#else
    (void)accelerator;
    (void)rdna4_cd_bound_primary;
#endif
}

#endif
