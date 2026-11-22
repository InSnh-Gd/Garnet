// VM-only probe of the shipping accelerator's standard user-client entry.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static io_service_t fixture_accelerator(void) {
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("GarnetAccelerator"), &iterator)) return 0;
    io_service_t service, found = 0;
    while ((service = IOIteratorNext(iterator))) {
        io_registry_entry_t parent = 0;
        if (!IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent)) {
            CFTypeRef test = IORegistryEntryCreateCFProperty(parent, CFSTR("VMTestOnly"), kCFAllocatorDefault, 0);
            int match = test && CFGetTypeID(test) == CFBooleanGetTypeID() && CFBooleanGetValue(test);
            if (test) CFRelease(test);
            IOObjectRelease(parent);
            if (match) { found = service; break; }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return found;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-user-client TYPE\nOpens and closes one standard IOAccel user client on the VM-only fixture. Type 5 verifies read-only device name/config methods. No GPU submissions.");
        return 0;
    }
    if (argc != 2 || geteuid() != 0 || !argv[1][0] || argv[1][0] == '-') return 2;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(argv[1], &end, 0);
    if (errno || *end || parsed > UINT32_MAX) return 2;
    uint32_t type = (uint32_t)parsed;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(30);
    io_service_t service = fixture_accelerator();
    if (!service) { fputs("USER_CLIENT_FAIL no VM fixture accelerator\n", stderr); return 1; }
    printf("USER_CLIENT_BEGIN type=%#x\n", type);
    io_connect_t connection = 0;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), type, &connection);
    printf("USER_CLIENT_OPEN type=%#x rc=%#x connection=%#x\n", type, result, connection);
    if (result == KERN_SUCCESS && connection) {
        int valid = 1;
        if (type == 5) {
            char name[64] = {0};
            size_t size = sizeof name;
            kern_return_t read = IOConnectCallStructMethod(connection, 1, NULL, 0, name, &size);
            name[sizeof name - 1] = 0;
            printf("USER_CLIENT_NAME rc=%#x bytes=%zu name=%s\n", read, size, name);
            valid = read == KERN_SUCCESS && size == sizeof name && !strcmp(name, "Garnet Accelerator");
            uint32_t config[16] = {0};
            size = sizeof config;
            read = IOConnectCallStructMethod(connection, 0, NULL, 0, config, &size);
            printf("USER_CLIENT_CONFIG rc=%#x bytes=%zu\n", read, size);
            valid = valid && read == KERN_SUCCESS && size == sizeof config;
            size = sizeof config - 1;
            read = IOConnectCallStructMethod(connection, 0, NULL, 0, config, &size);
            printf("USER_CLIENT_BAD_SIZE rc=%#x\n", read);
            valid = valid && read == kIOReturnBadArgument;
        }
        io_connect_t native = 0;
        kern_return_t denied = IOServiceOpen(service, mach_task_self(), 0x47524E54u /* 'GRNT' */, &native);
        printf("USER_CLIENT_NATIVE type=%#x rc=%#x connection=%#x\n", type, denied, native);
        if (native) IOServiceClose(native);
        kern_return_t closed = IOServiceClose(connection);
        printf("USER_CLIENT_CLOSE type=%#x rc=%#x\n", type, closed);
        IOObjectRelease(service);
        if (!valid || denied != kIOReturnNotReady || native || closed != KERN_SUCCESS) return 1;
        return 0;
    }
    if (connection) IOServiceClose(connection);
    IOObjectRelease(service);
    return result == KERN_SUCCESS ? 1 : 3;
}
