#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "GarnetNativeABI.h"

static int failed(const char *operation, kern_return_t result) {
    fprintf(stderr, "%s: 0x%x (%s)\n", operation, result, mach_error_string(result));
    return 1;
}

static void matched(void *context, io_iterator_t iterator) {
    uint64_t *registry_id = context;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        IORegistryEntryGetRegistryEntryID(service, registry_id);
        IOObjectRelease(service);
    }
    if (*registry_id) CFRunLoopStop(CFRunLoopGetCurrent());
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: publish-metal\nVerify Garnet native self-test, publish Metal nub, print accelerator registry ID.");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: publish-metal\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Root required\n");
        return 1;
    }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                      IOServiceMatching("GarnetDriver"));
    if (!service) {
        fprintf(stderr, "GarnetDriver service absent\n");
        return 1;
    }
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), RDNA4N_UC_TYPE, &connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) return failed("Native open", result);
    uint64_t hello[2] = {RDNA4N_ABI_VERSION, 0}, reply[4] = {0};
    uint32_t count = 4;
    result = IOConnectCallScalarMethod(connection, RDNA4N_SEL_HELLO, hello, 2, reply, &count);
    if (result != KERN_SUCCESS || count != 4 || reply[0] != RDNA4N_ABI_VERSION) {
        IOServiceClose(connection);
        return failed("Native Hello / ABI", result != KERN_SUCCESS ? result : kIOReturnBadArgument);
    }
    struct rdna4n_info info = {0};
    size_t size = sizeof(info);
    result = IOConnectCallStructMethod(connection, RDNA4N_SEL_QUERYINFO, NULL, 0, &info, &size);
    if (result != KERN_SUCCESS) {
        IOServiceClose(connection);
        return failed("Native QueryInfo", result);
    }
    fprintf(stderr, "Native GC=%u.%u.%u flags=0x%x VRAM=%llu MiB\n",
            info.gc_version >> 16, (info.gc_version >> 8) & 255, info.gc_version & 255,
            info.flags, (unsigned long long)((info.vram_vis_total + info.vram_hi_total) >> 20));
    const uint32_t required = RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT;
    if (size != sizeof(info) || info.abi_version != RDNA4N_ABI_VERSION ||
        info.gc_version != 0x0c0000 || (info.flags & required) != required ||
        (info.flags & RDNA4N_INFO_HUNG)) {
        fprintf(stderr, "REFUSED: Garnet native self-test must pass before Metal publication\n");
        IOServiceClose(connection);
        return 1;
    }
    uint64_t flags = 0;
    count = 4;
    result = IOConnectCallScalarMethod(connection, RDNA4N_SEL_METAL_NUB_PUBLISH,
                                       &flags, 1, reply, &count);
    IOServiceClose(connection);
    if (result != KERN_SUCCESS) return failed("Metal nub publish", result);
    uint64_t registry_id = 0;
    IONotificationPortRef notifications = IONotificationPortCreate(kIOMainPortDefault);
    if (!notifications) return failed("Notification port", kIOReturnNoMemory);
    CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(notifications);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    io_iterator_t iterator = IO_OBJECT_NULL;
    result = IOServiceAddMatchingNotification(notifications, kIOFirstMatchNotification,
               IOServiceMatching("GarnetAccelerator"), matched, &registry_id, &iterator);
    if (result == KERN_SUCCESS) {
        matched(&registry_id, iterator);
        if (!registry_id) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 10.0, false);
        IOObjectRelease(iterator);
    }
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    IONotificationPortDestroy(notifications);
    if (result != KERN_SUCCESS) return failed("Accelerator notification", result);
    if (!registry_id) {
        fprintf(stderr, "GarnetAccelerator absent after publication; inspect runtime layout gate\n");
        return 1;
    }
    printf("0x%llx\n", (unsigned long long)registry_id);
    return 0;
}
