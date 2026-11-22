#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo fixture-control publish|withdraw|status|bad-shape\nControls only the VM-only software nub, not GPU engines.");
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "publish") && strcmp(argv[1], "withdraw") && strcmp(argv[1], "status") && strcmp(argv[1], "bad-shape"))) return 2;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("GarnetVMFixture"));
    if (!service) { fputs("FIXTURE_FAIL service absent\n", stderr); return 1; }
    io_connect_t connection = 0;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) { fprintf(stderr, "FIXTURE_OPEN_FAIL uid=%u rc=%#x\n", getuid(), result); return 1; }
    uint64_t output[6] = {0};
    uint32_t count = !strcmp(argv[1], "status") ? 6 : !strcmp(argv[1], "publish") ? 1 : 0;
    uint32_t selector = !strcmp(argv[1], "status") ? 2 : !strcmp(argv[1], "withdraw") ? 1 : 0;
    result = IOConnectCallScalarMethod(connection, selector, NULL, 0, output, &count);
    IOServiceClose(connection);
    if (!strcmp(argv[1], "bad-shape")) {
        if (result != kIOReturnBadArgument) { fprintf(stderr, "FIXTURE_FAIL bad shape rc=%#x\n", result); return 1; }
        puts("FIXTURE_BAD_SHAPE_PASS");
        return 0;
    }
    if (result != KERN_SUCCESS) { fprintf(stderr, "FIXTURE_FAIL %s rc=%#x\n", argv[1], result); return 1; }
    if (!strcmp(argv[1], "status")) {
        printf("FIXTURE_STATUS abi=%llu size=%llu build=%llu caps=%#llx stamp_present=%llu stamp_count=%llu\n",
               output[0], output[1], output[2], output[3], output[4], output[5]);
    } else printf("FIXTURE_PASS action=%s nub=%llu\n", argv[1], output[0]);
    return 0;
}
