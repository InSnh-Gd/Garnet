#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <unistd.h>

static CFTypeRef identityModel;
static unsigned identityReads;
static CFTypeRef identityMemory;
static unsigned memoryReads;
static unsigned profileReads;
static unsigned registryIDReads;
static uint64_t identityPcieCapabilities = 0x00737105;
static io_registry_entry_t identityRegistryEntryID(io_registry_entry_t entry, uint64_t *registryID) {
    if (entry != 0x42 || !registryID) return kIOReturnBadArgument;
    registryIDReads++;
    *registryID = 0x123456789abcdef0ull;
    return KERN_SUCCESS;
}
static uid_t identityUid(void) { return 0; }
static CFTypeRef __attribute__((unused)) identityProperty(io_registry_entry_t entry, const io_name_t plane,
                                 CFStringRef key, CFAllocatorRef allocator, IOOptionBits options) {
    (void)entry; (void)plane; (void)allocator; (void)options;
    if (CFEqual(key, CFSTR("Garnet,MemoryCaps"))) {
        memoryReads++;
        return identityMemory ? CFRetain(identityMemory) : NULL;
    }
    if (CFEqual(key, CFSTR("IOPCIExpressLinkCapabilities"))) {
        profileReads++;
        int64_t value = (int64_t)identityPcieCapabilities;
        return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    }
    if (CFEqual(key, CFSTR("pcidebug"))) {
        profileReads++;
        return CFRetain(CFSTR("0:1:1(1:3)"));
    }
    if (!CFEqual(key, CFSTR("model"))) abort();
    identityReads++;
    return identityModel ? CFRetain(identityModel) : NULL;
}

#define geteuid identityUid
#define IORegistryEntrySearchCFProperty identityProperty
#define IORegistryEntryGetRegistryEntryID identityRegistryEntryID
#include "GarnetDevice.m"
#undef IORegistryEntrySearchCFProperty
#undef IORegistryEntryGetRegistryEntryID
#undef geteuid

static void require(BOOL ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "DEVICE_IDENTITY_FAIL %s\n", message);
        exit(1);
    }
}

static id identityBaseInit(id object, SEL command, uint32_t port) __attribute__((ns_returns_retained));
static id identityBaseInit(id object, SEL command, uint32_t port) {
    (void)command; (void)port;
    return [object init];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-device-identity\nChecks the production Metal initializer and stable identity without opening RADV. Registry model reads, admission UID and the base accelerator initializer are simulated.");
            return 0;
        }
        if (argc != 1) return 2;
        require(rdna4sp_pcie_rate(0x11) == 250000000ull &&
            rdna4sp_pcie_rate(0x102) == 500000000ull * 16ull &&
            rdna4sp_pcie_rate(0x13) == 984615384ull,
            "PCIe link generation encoding or bandwidth efficiency is incorrect");
        require(!rdna4sp_pcie_rate(0) && !rdna4sp_pcie_rate(0x76) &&
            !rdna4sp_pcie_rate(0x705), "invalid PCIe generation/width was accepted");
        require(rdna4sp_pci_location_number("0:1:1(1:3)") == 1 &&
            !rdna4sp_pci_location_number("0:1:32(1:3)") &&
            !rdna4sp_pci_location_number("not-a-pci-path"),
            "PCI location parsing accepted malformed registry data");
        require(rdna4sp_pcie_rate(identityPcieCapabilities) == 63015384615ull,
            "PCIe transfer-rate helper differs from the Gen5 x16 oracle");
        CFTypeRef pcieProperty = identityProperty(0x42, kIOServicePlane,
            CFSTR("IOPCIExpressLinkCapabilities"), kCFAllocatorDefault, kIORegistryIterateParents);
        uint64_t pcieValue = 0;
        require(rdna4_number_u64(pcieProperty, &pcieValue) && pcieValue == identityPcieCapabilities,
            "PCIe mock property is malformed");
        CFRelease(pcieProperty);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_READY", "1", 1);
        setenv("RDNA4M_TEST_AUTODISARMED", "0", 1);
        setenv("RDNA4M_TEST_IGNORE_KILL", "1", 1);
        Method base = class_getInstanceMethod([MTLIOAccelDevice class], @selector(initWithAcceleratorPort:));
        require(base != NULL, "base accelerator initializer unavailable");
        IMP original = method_setImplementation(base, (IMP)identityBaseInit);
        const char bytes[] = "AMD Radeon RX 9060 XT\0ignored";
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)bytes, sizeof bytes);
        const CFTypeRef models[] = { CFSTR("AMD Radeon RX 9060 XT"), data,
            CFSTR("  AMD Radeon RX 9060 XT \n"), NULL, kCFBooleanTrue, CFSTR("") };
        for (unsigned i = 0; i < sizeof models / sizeof models[0]; i++) {
            identityModel = models[i];
            atomic_store(&rdna4_caps_ready, 0);
            id device = [[GarnetDevice alloc] initWithAcceleratorPort:0x42];
            require(device != nil, "production initializer refused the test accelerator");
            NSString *expected = i < 3 ? @"AMD Radeon RX 9060 XT" : @"AMD Radeon (native Vulkan)";
            NSString *cold = [device name];
            require([cold isEqualToString:expected], "cold Metal name does not match the hardware model");
            require([[device productName] isEqualToString:expected], "product and full names disagree");
            require([[device vendorName] isEqualToString:@"AMD"], "Metal vendor name is missing");
            require([device registryID] == 0x123456789abcdef0ull, "Metal identity does not match the accelerator registry entry");
            require([device location] == MTLDeviceLocationSlot && [device locationNumber] == 1,
                "Metal location does not match the PCIe slot topology");
            require([device maxTransferRate] == 63015384615ull,
                "PCIe link capabilities were not converted to the transfer-rate API units");
            require(!RDNA4R.tried, "enumerating identity opened RADV");
            RDNA4Caps = rdna4dc_fallback();
            strlcpy(RDNA4Caps.name, "AMD Radeon RX 9060 XT (RADV GFX1200)", sizeof RDNA4Caps.name);
            atomic_store(&rdna4_caps_ready, 1);
            require([[device name] isEqualToString:cold], "Metal name changed after publishing backend capabilities");
            require([[device productName] isEqualToString:cold], "product name changed after backend initialization");
            printf("DEVICE_IDENTITY_CASE_PASS case=%u name=%s radv_open=0\n", i, cold.UTF8String);
        }
        require(identityReads == 6, "identity was not read once per initialized device");
        require(memoryReads == 6, "memory snapshot was not read once per initialized device");
        RDNA4Texture *texture = [RDNA4Texture new];
        require([texture respondsToSelector:@selector(gpuResourceID)], "Metal texture resource ID is unavailable");
        uint64_t textureIDs[256] = {0};
        uint64_t *textureIDsPtr = textureIDs;
        dispatch_apply(256, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
            textureIDsPtr[i] = texture.gpuResourceID._impl;
        });
        uint64_t resourceID = textureIDs[0];
        require(resourceID != 0 && resourceID == texture.uniqueIdentifier, "texture resource ID is not its stable resource identity");
        for (unsigned i = 1; i < 256; i++) require(textureIDs[i] == resourceID, "concurrent texture resource IDs were inconsistent");
        RDNA4Texture *otherTexture = [RDNA4Texture new];
        require(otherTexture.gpuResourceID._impl != resourceID, "distinct textures shared one resource ID");
        puts("DEVICE_RESOURCE_ID_PASS concurrent stable Metal texture identity");
        NSDictionary *memory = @{ @"version": @1, @"physical": @(16ull << 30), @"pool": @(15ull << 30),
            @"visible": @(217ull << 20), @"applicationBudget": @((14ull << 30) - (64ull << 20)),
            @"maxBuffer": @0xfffffffcull };
        NSMutableDictionary *future = [memory mutableCopy]; future[@"version"] = @2;
        NSMutableDictionary *wrongType = [memory mutableCopy]; wrongType[@"pool"] = @"large";
        NSMutableDictionary *negative = [memory mutableCopy]; negative[@"visible"] = @-1;
        NSMutableDictionary *boolean = [memory mutableCopy]; boolean[@"version"] = @YES;
        NSMutableDictionary *floating = [memory mutableCopy]; floating[@"version"] = @1.0;
        NSMutableDictionary *oversize = [memory mutableCopy]; oversize[@"applicationBudget"] = @(32ull << 30);
        NSMutableDictionary *lowBudget = [memory mutableCopy]; lowBudget[@"applicationBudget"] = @(64ull << 20);
        NSMutableDictionary *exhausted = [memory mutableCopy]; exhausted[@"applicationBudget"] = @0;
        NSMutableDictionary *composite = [memory mutableCopy]; composite[@"compositeMaxBuffer"] = @(8ull << 30);
        NSMutableDictionary *compositeExhausted = [composite mutableCopy]; compositeExhausted[@"applicationBudget"] = @0;
        NSMutableDictionary *compositeFloat = [memory mutableCopy]; compositeFloat[@"compositeMaxBuffer"] = @8589934592.0;
        NSMutableDictionary *compositeNegative = [memory mutableCopy]; compositeNegative[@"compositeMaxBuffer"] = @-1;
        NSMutableDictionary *compositeOversize = [memory mutableCopy]; compositeOversize[@"compositeMaxBuffer"] = @(16ull << 30);
        NSMutableDictionary *compositeBoolean = [memory mutableCopy]; compositeBoolean[@"compositeMaxBuffer"] = @YES;
        NSArray *snapshots = @[memory, @{}, @1, future, wrongType, negative, boolean, floating, oversize,
            lowBudget, exhausted, composite, compositeExhausted, compositeFloat, compositeNegative,
            compositeOversize, compositeBoolean];
        for (NSUInteger i = 0; i < snapshots.count; i++) {
            identityMemory = (__bridge CFTypeRef)snapshots[i];
            atomic_store(&rdna4_caps_ready, 0);
            id device = [[GarnetDevice alloc] initWithAcceleratorPort:0x42];
            require(device != nil, "memory snapshot affected accelerator admission");
            BOOL valid = i == 0 || i >= 9;
            uint64_t recommendation = !valid ? 256ull << 20 : i == 9 ? 64ull << 20 :
                (i == 10 || i == 12) ? 0 : 12ull << 30;
            uint64_t maxBuffer = !valid ? 128ull << 20 : (i == 11 || i == 12) ? 8ull << 30 : 0xfffffffcull;
            require([device maxBufferLength] == maxBuffer, "pre-open buffer limit did not validate the native snapshot");
            require([device recommendedMaxWorkingSetSize] == recommendation, "pre-open recommendation did not validate the native snapshot");
            require(!RDNA4R.tried, "querying native memory opened RADV");
            if (i == 0) {
                MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.size = 13ull << 30; hd.storageMode = MTLStorageModePrivate;
                id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
                require(heap && heap.size == hd.size, "recommendation was still enforced as the allocation budget");
                require(!RDNA4R.tried, "creating a budget heap opened RADV");
            }
            RDNA4Caps = rdna4dc_fallback(); RDNA4Caps.buffer_length = 64ull << 20;
            atomic_store(&rdna4_caps_ready, 1);
            require([device maxBufferLength] == 64ull << 20, "opened backend limits did not supersede the static snapshot");
            printf("DEVICE_MEMORY_CASE_PASS case=%lu native=%u radv_open=0\n", (unsigned long)i, valid);
        }
        require(memoryReads == 6 + snapshots.count && identityReads == 6 + snapshots.count &&
            registryIDReads == 6 + snapshots.count && profileReads == 13,
            "device profile properties were reread or did not use the accelerator registry identity");
        /* RX 9060 XT 8GB memory snapshot & argument-limit host-fact pool regression (simulated). */
        const uint64_t phys8g = UINT64_C(8112) << 20;
        const uint64_t pool8g = UINT64_C(7288) << 20;
        const uint64_t budget8g = UINT64_C(6100) << 20;
        NSDictionary *mem8gFull = @{ @"version": @1, @"physical": @(phys8g), @"pool": @(pool8g),
            @"visible": @(UINT64_C(7224) << 20), @"applicationBudget": @(budget8g),
            @"maxBuffer": @0xfffffffcull };
        NSMutableDictionary *mem8gComposite = [mem8gFull mutableCopy];
        mem8gComposite[@"compositeMaxBuffer"] = @(UINT64_C(7224) << 20);
        NSMutableDictionary *mem8gSmallBar = [mem8gFull mutableCopy];
        mem8gSmallBar[@"visible"] = @(UINT64_C(217) << 20);
        NSArray *snapshots8g = @[mem8gFull, mem8gComposite, mem8gSmallBar];
        for (NSUInteger i = 0; i < snapshots8g.count; i++) {
            identityMemory = (__bridge CFTypeRef)snapshots8g[i];
            atomic_store(&rdna4_caps_ready, 0);
            id device8g = [[GarnetDevice alloc] initWithAcceleratorPort:0x42];
            require(device8g != nil, "8GB memory snapshot affected accelerator admission");
            uint64_t wantMaxBuf = (i == 1) ? (UINT64_C(7224) << 20) : 0xfffffffcull;
            require([device8g maxBufferLength] == wantMaxBuf,
                "8GB pre-open buffer limit mismatch");
            require([device8g recommendedMaxWorkingSetSize] == budget8g,
                "8GB pre-open working set recommendation mismatch");
            require(!RDNA4R.tried, "querying 8GB native memory opened RADV");
        }
        rdna4dc_native_memory native8g = {
            .version = RDNA4DC_MEMORY_CAPS_VERSION,
            .physical = phys8g,
            .pool = pool8g,
            .visible = UINT64_C(7224) << 20,
            .application_budget = budget8g,
            .max_buffer = 0xfffffffcull,
        };
        rdna4_radv_darwin_host_facts_v1 facts8g;
        const rdna4_radv_darwin_host_facts_v1 *factsPtr8g =
            rdna4_argument_limit_host_facts(&native8g, &facts8g);
        require(factsPtr8g == &facts8g &&
                facts8g.valid_flags == RDNA4_RADV_DARWIN_FACT_VRAM_BYTES &&
                facts8g.vram_bytes == native8g.pool &&
                facts8g.vram_bytes != native8g.physical,
                "8GB argument-limit host facts must report usable pool rather than raw physical");
        rdna4dc_native_memory nativeBad = native8g;
        nativeBad.pool = 0;
        require(rdna4_argument_limit_host_facts(&nativeBad, &facts8g) == NULL &&
                rdna4_argument_limit_host_facts(NULL, &facts8g) == NULL,
                "invalid native memory produces NULL argument-limit host facts");
        require(memoryReads == 6 + snapshots.count + snapshots8g.count &&
            identityReads == 6 + snapshots.count + snapshots8g.count &&
            registryIDReads == 6 + snapshots.count + snapshots8g.count,
            "8GB snapshot checks read unexpected property count");
        method_setImplementation(base, original);
        CFRelease(data);
        puts("DEVICE_PROFILE_PASS vendor=AMD registryID=accelerator PCIe=Slot-1 Gen5x16=63015384615");
        puts("DEVICE_IDENTITY_PASS production initializer and getters; simulated registry models, no physical GPU claim");
    }
    return 0;
}
