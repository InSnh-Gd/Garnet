// VM-only integration test of Apple's actual AGDC user client and the shared vendor callback.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static unsigned checks;

static void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static uint64_t property_number(io_service_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    int64_t number = 0;
    require(value && CFGetTypeID(value) == CFNumberGetTypeID(), "numeric registry property exists");
    require(CFNumberGetValue(value, kCFNumberSInt64Type, &number), "numeric registry property decodes");
    CFRelease(value);
    return (uint64_t)number;
}

static bool property_true(io_service_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    const bool result = value && CFEqual(value, kCFBooleanTrue);
    if (value) CFRelease(value);
    return result;
}

static uint32_t read32(const void *bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, (const uint8_t *)bytes + offset, sizeof value);
    return value;
}

static uint64_t read64(const void *bytes, size_t offset) {
    uint64_t value;
    memcpy(&value, (const uint8_t *)bytes + offset, sizeof value);
    return value;
}

static void write32(void *bytes, size_t offset, uint32_t value) {
    memcpy((uint8_t *)bytes + offset, &value, sizeof value);
}

static void resolve_id(uint64_t id, const char *expected_class) {
    io_service_t found = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id));
    require(found != IO_OBJECT_NULL, "returned registry ID resolves to a registered service");
    require(IOObjectConformsTo(found, expected_class), "returned registry ID names the expected service class");
    IOObjectRelease(found);
}

static void query(io_connect_t client, uint32_t command, void *bytes, size_t size, bool inout) {
    size_t returned = size;
    const kern_return_t kr = IOConnectCallStructMethod(client, command, inout ? bytes : NULL,
                                                      inout ? size : 0, bytes, &returned);
    printf("QUERY cmd=%#x kr=%#x size=%#zx\n", command, kr, returned);
    require(kr == KERN_SUCCESS && returned == size, "query succeeds with its exact reply size");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: agdc-live-client --denied|--query\nVM-only; --denied tests normal authorization, --query needs entitlement or the VM-only agdc_entitle boot argument.");
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "--denied") && strcmp(argv[1], "--query"))) return 2;
    const bool denied = !strcmp(argv[1], "--denied");
    char features[2048] = {0}, build[64] = {0};
    size_t features_size = sizeof features, build_size = sizeof build;
    require(sysctlbyname("machdep.cpu.features", features, &features_size, NULL, 0) == 0 &&
            strstr(features, "VMM"), "running in an x86 VM");
    require(sysctlbyname("kern.osversion", build, &build_size, NULL, 0) == 0 &&
            !strcmp(build, "25G332"), "running the pinned macOS build");

    // Given the VM probe has run the actual production builder on its VM framebuffer.
    io_service_t probe = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA4AGDCLive"));
    require(probe && property_true(probe, CFSTR("VMTestOnly")), "VM-only probe exists");
    require(property_true(probe, CFSTR("ServiceChecksPassed")) &&
            property_true(probe, CFSTR("ServiceRegistered")), "VM probe observed completed AGDC registration");
    require(!property_true(probe, CFSTR("NativeFlowExecuted")), "native GPU flow was not executed");
    require(property_number(probe, CFSTR("ServiceStatus")) == 0 &&
            property_true(probe, CFSTR("ServiceStartOK")), "actual AGDC start succeeded");
    require(property_number(probe, CFSTR("ServiceVendorInfoCalls")) > 0 &&
            property_number(probe, CFSTR("ServiceGPUCapabilityCalls")) > 0, "start called the production vendor callback");
    const uint64_t service_id = property_number(probe, CFSTR("ServiceID"));
    const uint64_t framebuffer_id = property_number(probe, CFSTR("VMFramebufferID"));
    const uint64_t pci_id = property_number(probe, CFSTR("VMPCIID"));
    IOObjectRelease(probe);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(service_id));
    require(service && property_true(service, CFSTR("RDNA4,VMTestOnly")) &&
            IOObjectConformsTo(service, "AppleGraphicsDeviceControl"), "AGDC registration is observable from userspace");

    // When Apple's real type-2 AGDC user client is opened by this task.
    io_connect_t client = IO_OBJECT_NULL;
    const kern_return_t open_status = IOServiceOpen(service, mach_task_self(), 2, &client);
    IOObjectRelease(service);
    printf("OPEN mode=%s kr=%#x\n", denied ? "denied" : "query", open_status);
    if (denied) {
        require(open_status == kIOReturnNotPermitted && !client, "an unentitled task is denied");
        printf("PASS %u checks; actual VM AGDC authorization refusal\n", checks);
        return 0;
    }
    require(open_status == KERN_SUCCESS && client, "test task opens the real AGDC user client");

    // Then replies cross the kernel/userspace boundary with IDs and usable capabilities.
    uint8_t vendor[0x2c] = {0}, gpu[0xdc] = {0}, link[0xb0] = {0}, pipeline[0x196c] = {0};
    query(client, 1, vendor, sizeof vendor, false);
    require(read32(vendor, 0) == 0x30000 && read32(vendor, 0x24) == 0x1002 &&
            read32(vendor, 0x28) == 2, "vendor reply carries AMD and the expected API version/type");
    query(client, 0x980, gpu, sizeof gpu, false);
    require(read64(gpu, 0) == 2 && read32(gpu, 0x20) == 1 && read32(gpu, 0x30) == 1,
            "GPU capability declares one framebuffer");
    printf("IDS pci=%#llx fb=%#llx expected-pci=%#llx expected-fb=%#llx\n",
           (unsigned long long)read64(gpu, 0x34), (unsigned long long)read64(gpu, 0x3c),
           (unsigned long long)pci_id, (unsigned long long)framebuffer_id);
    require(read64(gpu, 0x34) == pci_id && read64(gpu, 0x3c) == framebuffer_id,
            "Apple converts both production pointers into the correct registry IDs");
    resolve_id(read64(gpu, 0x34), "IOPCIDevice");
    resolve_id(read64(gpu, 0x3c), "IONDRVFramebuffer");
    query(client, 0x921, link, sizeof link, true);
    require(read32(link, 0x44) == 1920 && read32(link, 0x54) == 1080 &&
            read64(link, 0x2c) == 148500000, "VM-boundary link reply uses the documented fallback timing");
    const uint32_t types[] = {0x10, 0x40};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i) {
        memset(pipeline, 0, sizeof pipeline);
        write32(pipeline, 4, types[i]);
        write32(pipeline, 8, 8);
        query(client, 0x711, pipeline, sizeof pipeline, true);
        require(read32(pipeline, 8) == 1, "scaler and inline-buffer replies contain an entry");
        require(types[i] == 0x10 ? read32(pipeline, 0x30) == 1920 && read32(pipeline, 0x34) == 1080
                                : read32(pipeline, 0x1c) == 1920 && read32(pipeline, 0x18) == 1080,
                "pipeline reply retains the expected endpoint dimensions");
    }
    const uint32_t commands[] = {1, 0x980, 0x921, 0x711};
    const size_t sizes[] = {sizeof vendor, sizeof gpu, sizeof link, sizeof pipeline};
    for (size_t i = 0; i < sizeof commands / sizeof *commands; ++i) {
        size_t returned = sizes[i] - 1;
        const kern_return_t kr = IOConnectCallStructMethod(client, commands[i],
            i >= 2 ? pipeline : NULL, i >= 2 ? sizes[i] : 0, pipeline, &returned);
        printf("BAD_LENGTH cmd=%#x kr=%#x\n", commands[i], kr);
        require(kr == kIOReturnBadArgument, "undersized replies are rejected");
    }
    require(IOServiceClose(client) == KERN_SUCCESS, "client closes cleanly");
    printf("PASS %u checks; actual VM AGDC user client, no native GPU submissions\n", checks);
    return 0;
}
