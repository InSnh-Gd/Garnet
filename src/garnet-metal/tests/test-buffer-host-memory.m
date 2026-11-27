#import <Foundation/Foundation.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>

static uint64_t mappedBytes, unmappedBytes;
static BOOL refuseMap;
static void *hostMap(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    if (refuseMap) { errno = ENOMEM; return MAP_FAILED; }
    void *p = mmap(address, size, prot, flags, fd, offset);
    if (p != MAP_FAILED) mappedBytes += size;
    return p;
}
static int hostUnmap(void *address, size_t size) {
    int result = munmap(address, size);
    if (!result) unmappedBytes += size;
    return result;
}
#define mmap hostMap
#define munmap hostUnmap
#include "GarnetDevice.m"
#undef mmap
#undef munmap

enum { failNone, failCreate, failProperties, failAllocate, failBind, failAddress };
static unsigned failAt, creates, frees, destroys, liveMemory;
static uint64_t requestedSize;
static void *importedPointer;
static void require(BOOL value, const char *message) {
    if (!value) { fprintf(stderr, "BUFFER_HOST_MEMORY_FAIL %s\n", message); exit(1); }
}
static VkResult VKAPI_CALL createBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)device; (void)allocator;
    creates++; requestedSize = info->size;
    require(info->pNext != NULL, "large Shared buffer did not request external memory");
    if (failAt == failCreate) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *buffer = (VkBuffer)(uintptr_t)1; return VK_SUCCESS;
}
static void VKAPI_CALL destroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    (void)device; (void)buffer; (void)allocator; destroys++;
}
static void VKAPI_CALL requirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *out) {
    (void)device; (void)buffer;
    *out = (VkMemoryRequirements){requestedSize, 4096, 1};
}
static VkResult VKAPI_CALL hostProperties(VkDevice device, VkExternalMemoryHandleTypeFlagBits type,
                                         const void *pointer, VkMemoryHostPointerPropertiesEXT *out) {
    (void)device;
    require(type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "wrong external handle");
    importedPointer = (void *)pointer;
    require(!((uintptr_t)pointer % RDNA4R.hostAlign), "host pointer is not aligned");
    out->memoryTypeBits = 1;
    return failAt == failProperties ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS;
}
static VkResult VKAPI_CALL allocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    (void)device; (void)allocator;
    const VkMemoryAllocateFlagsInfo *flags = info->pNext;
    const VkImportMemoryHostPointerInfoEXT *host = flags->pNext;
    require(flags->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO &&
            host->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT &&
            host->pHostPointer == importedPointer && info->allocationSize == requestedSize,
            "import allocation lost host range or device address flags");
    if (failAt == failAllocate) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *memory = (VkDeviceMemory)(uintptr_t)2; liveMemory++; return VK_SUCCESS;
}
static VkResult VKAPI_CALL bindMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device; (void)buffer; (void)memory;
    require(!offset, "unexpected import binding offset");
    return failAt == failBind ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VkDeviceAddress VKAPI_CALL address(VkDevice device, const VkBufferDeviceAddressInfo *info) {
    (void)device; (void)info;
    return failAt == failAddress ? 0 : UINT64_C(0x1800000000);
}
static void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    (void)device; (void)memory; (void)allocator;
    require(liveMemory == 1, "memory freed twice");
    // The host VA must remain mapped through Vulkan release, including failures after allocation.
    volatile uint8_t first = *(uint8_t *)importedPointer; (void)first;
    liveMemory--; frees++;
}
static VkResult VKAPI_CALL createLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    require(info->bindingCount == 1 && info->pBindings[0].binding == 640, "table reflection changed");
    *layout = (VkDescriptorSetLayout)(uintptr_t)3; return VK_SUCCESS;
}
static void tableMetadata(void) {
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL, "cannot allocate reflection");
    reflection->bind[0][640] = (RDNA4sBind){ .used = 1, .count = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    NSArray *variants = @[@{}, @{ @"buffer_address_table_version": @2, @"buffer_address_table_bytes": @768 },
        @{ @"buffer_address_table_version": @2 }, @{ @"buffer_address_table_bytes": @768 },
        @{ @"buffer_address_table_version": @2.5, @"buffer_address_table_bytes": @768 },
        @{ @"buffer_address_table_version": @2, @"buffer_address_table_bytes": @768.5 },
        @{ @"buffer_address_table_version": @YES, @"buffer_address_table_bytes": @768 },
        @{ @"buffer_address_table_version": @3, @"buffer_address_table_bytes": @768 },
        @{ @"buffer_address_table_version": @2, @"buffer_address_table_bytes": @256 },
        @{ @"buffer_address_table_version": @"2", @"buffer_address_table_bytes": @768 }];
    vkCreateDescriptorSetLayout = createLayout;
    for (NSUInteger i = 0; i < variants.count; i++) {
        NSMutableDictionary *meta = [variants[i] mutableCopy];
        meta[@"bindings"] = @[@{ @"kind": @"BufferAddressTable", @"descriptor": @{ @"binding": @640, @"count": @1 } }];
        RDNA4PB *bindings = NULL; uint32_t count = 0; VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        NSError *error = nil;
        BOOL ok = rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT, meta, &layout, &bindings, &count, [NSMutableArray array], &error);
        require(ok == (i < 2), "unsupported table metadata was accepted or old metadata rejected");
        if (ok) require(!error && count == 1 && bindings[0].addressTableVersion == (i ? 2u : 1u), "table generation was not retained");
        else require(error && !bindings && !count, "invalid metadata published partial bindings");
        free(bindings);
    }
    free(reflection);
    puts("BUFFER_HOST_MEMORY_PASS table_metadata=legacy+v2 malformed_rejected=8");
}
static void reset(unsigned failure) {
    failAt = failure; creates = frees = destroys = liveMemory = 0;
    requestedSize = mappedBytes = unmappedBytes = 0; importedPointer = NULL;
    require(RDNA4R.impTotal == 0 && atomic_load(&rdna4_alloc_total) == 0, "accounting leaked between cases");
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-buffer-host-memory\nRuns actual Metal buffer initializers with simulated Vulkan import. Large cases reserve CPU VA and touch selected pages; they do not pin large backing or execute GPU work."); return 0;
        }
        if (argc != 1) return 2;
        RDNA4R.tried = RDNA4R.ok = RDNA4R.hostExt = RDNA4R.bufferAddress = YES;
        RDNA4R.hostAlign = 65536;
        RDNA4R.mp.memoryTypeCount = 1;
        RDNA4R.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        RDNA4Caps = rdna4dc_fallback(); RDNA4Caps.buffer_length = UINT64_C(8) << 30;
        atomic_store(&rdna4_caps_ready, 1);
        vkCreateBuffer = createBuffer; vkDestroyBuffer = destroyBuffer; vkGetBufferMemoryRequirements = requirements;
        vkAllocateMemory = allocateMemory; vkFreeMemory = freeMemory; vkBindBufferMemory = bindMemory;
        rdna4_vkGetMHPP = hostProperties; rdna4_vkGetBufferAddress = address;
        tableMetadata();
        for (unsigned mode = 0; mode < 2; mode++) {
            reset(failNone);
            uint64_t length = mode ? (UINT64_C(8) << 30) : (UINT64_C(64) << 20) + 17;
            @autoreleasepool {
                NSError *error = nil;
                RDNA4Buffer *buffer = [[RDNA4Buffer alloc] initWithDevice:nil length:length options:mode << 4 error:&error];
                require(buffer && !error && buffer.length == length && buffer.contents == importedPointer, "owned import failed or lost logical size");
                require(buffer.gpuAddress == UINT64_C(0x1800000000) && !buffer.rdna4VkOffset && buffer.rdna4Imported, "owned import lost address");
                uint8_t *p = buffer.contents;
                require(p[0] == 0 && p[length - 1] == 0, "fresh shared memory is not zero");
                const uint64_t offsets[] = {0, length / 2, length - 1};
                for (unsigned i = 0; i < 3; i++) p[offsets[i]] = (uint8_t)(41 + i);
                for (unsigned i = 0; i < 3; i++) require(((uint8_t *)importedPointer)[offsets[i]] == 41 + i, "CPU VA does not alias imported pointer");
                require(RDNA4R.impTotal == requestedSize && liveMemory == 1 &&
                    atomic_load(&rdna4_alloc_total) == requestedSize, "owned import was not counted once");
                if (!mode) {
                    [buffer rdna4AdoptHeap:nil offset:0 size:(NSUInteger)length bid:0];
                    require(!atomic_load(&rdna4_alloc_total) && RDNA4R.impTotal == requestedSize,
                        "heap adoption double-counted its owned import");
                }
            }
            require(frees == 1 && destroys == 1 && !liveMemory && mappedBytes == unmappedBytes, "owned import did not retire and unmap once");
            printf("BUFFER_HOST_MEMORY_PASS mode=%u logical=%llu synthetic_import=1 selected_cpu_pages=3\n", mode, (unsigned long long)length);
        }
        for (unsigned failure = failCreate; failure <= failAddress; failure++) {
            reset(failure);
            @autoreleasepool {
                NSError *error = nil;
                RDNA4Buffer *buffer = [[RDNA4Buffer alloc] initWithDevice:nil length:(64ull << 20) + 1 options:0 error:&error];
                require(!buffer && error, "injected failure was accepted");
            }
            require(mappedBytes == unmappedBytes && !liveMemory && RDNA4R.impTotal == 0, "failed owned import leaked VA, backing or accounting");
            require(frees == (failure >= failBind) && destroys == (failure != failCreate), "failure released the wrong resources");
        }
        reset(failNone); refuseMap = YES;
        require(![[RDNA4Buffer alloc] initWithDevice:nil length:128ull << 20 options:0 error:NULL], "mmap failure was accepted");
        refuseMap = NO; require(!creates, "mmap failure reached Vulkan");
        void *caller = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        require(caller != MAP_FAILED, "caller allocation failed");
        // Use a host-aligned subrange without relying on mmap alignment beyond the OS page.
        RDNA4R.hostAlign = (uint64_t)sysconf(_SC_PAGESIZE);
        __block unsigned callbacks = 0;
        for (unsigned failure = failNone; failure <= failAddress; failure++) {
            reset(failure);
            unsigned oldCallbacks = callbacks;
            @autoreleasepool {
                RDNA4Buffer *buffer = [[RDNA4Buffer alloc] initWithDevice:nil bytesNoCopy:(uint8_t *)caller + 7 length:32761 options:0 deallocator:^(void *p, NSUInteger n) {
                    require(p == (uint8_t *)caller + 7 && n == 32761 && !liveMemory, "NoCopy callback lost original arguments or ran before retirement"); callbacks++;
                } error:NULL];
                require((buffer != nil) == (failure == failNone), "NoCopy ignored injected failure");
                if (buffer) require(buffer.contents == (uint8_t *)caller + 7 && buffer.rdna4VkOffset == 7 &&
                    !atomic_load(&rdna4_alloc_total), "NoCopy copied, lost delta or charged borrowed backing");
            }
            require(callbacks == oldCallbacks + (failure == failNone) && !liveMemory && !mappedBytes && !unmappedBytes && RDNA4R.impTotal == 0, "NoCopy changed ownership or failure accounting");
        }
        reset(failNone);
        require(![[RDNA4Buffer alloc] initWithDevice:nil bytesNoCopy:caller length:(64ull << 20) + 1 options:0 deallocator:nil error:NULL] && !creates,
                "NoCopy single-import limit was enlarged");
        munmap(caller, 65536);
        puts("BUFFER_HOST_MEMORY_PASS failures=create/properties/allocate/bind/address/mmap callbacks=exactly-once simulated_vulkan=1 gpu_execution=0");
    }
    return 0;
}
