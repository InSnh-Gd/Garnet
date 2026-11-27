// Production automatic-heap execution tests with a Vulkan test double.
// The real GarnetDevice/RDNA4Heap/buffer/texture constructors and allocator run;
// only Vulkan entry points, mmap/munmap and the test device identity are doubled.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t hostMaps, hostUnmaps, hostMappedBytes, hostUnmappedBytes, hostLiveBytes;

static void *heapTestMmap(void *address, size_t size, int prot, int flags, int fd,
                          off_t offset) {
    void *mapping = mmap(address, size, prot, flags, fd, offset);
    if (mapping != MAP_FAILED) {
        hostMaps++;
        hostMappedBytes += size;
        hostLiveBytes += size;
    }
    return mapping;
}

static int heapTestMunmap(void *address, size_t size) {
    int result = munmap(address, size);
    if (result == 0) {
        hostUnmaps++;
        hostUnmappedBytes += size;
        hostLiveBytes -= size;
    }
    return result;
}

static int refuseProductionRealloc;
static void *heapTestRealloc(void *pointer, size_t size) {
    return refuseProductionRealloc ? NULL : realloc(pointer, size);
}

#define mmap heapTestMmap
#define munmap heapTestMunmap
#define realloc heapTestRealloc
#include "GarnetDevice.m"
#undef realloc
#undef mmap
#undef munmap

enum {
    kBufferMagic = 0x48425442,
    kImageMagic = 0x48494d47,
    kViewMagic = 0x48495657,
    kMemoryMagic = 0x484d454d,
    kCommandMagic = 0x48434d44,
    kFenceMagic = 0x48464e43,
    kMiB = 1 << 20,
    kHeapSize = 16 * kMiB,
};

typedef struct HeapTestMemory HeapTestMemory;

typedef struct {
    uint32_t magic;
    VkBufferCreateInfo create;
    VkMemoryRequirements requirements;
    BOOL hasRequirements;
    BOOL external;
    HeapTestMemory *memory;
} HeapTestBuffer;

typedef struct {
    uint32_t magic;
    VkImageCreateInfo create;
    VkMemoryRequirements requirements;
    BOOL hasRequirements;
    HeapTestMemory *memory;
} HeapTestImage;

typedef struct {
    uint32_t magic;
    HeapTestImage *image;
} HeapTestView;

typedef struct {
    uint32_t magic;
} HeapTestCommand;

typedef struct {
    uint32_t magic;
    BOOL signaled;
} HeapTestFence;

struct HeapTestMemory {
    uint32_t magic;
    VkDeviceSize size;
    uint32_t memoryType;
    void *map;
    const void *hostPointer;
    BOOL imported;
    unsigned boundBuffers;
    unsigned boundImages;
    VkBuffer dedicatedBuffer;
    VkImage dedicatedImage;
};

enum {
    FailNone,
    FailCreateBuffer,
    FailCreateImage,
    FailAllocate,
    FailMap,
    FailBindBuffer,
    FailBindImage,
    FailCreateView,
    FailHostProperties,
    FailCreateFence,
    FailQueueSubmit,
    FailFenceWait,
};

static unsigned failurePoint;
static unsigned liveBuffers, liveImages, liveViews, liveMemories, liveMappings;
static unsigned liveCommands, liveFences;
static unsigned bufferCreates, bufferDestroys, imageCreates, imageDestroys;
static unsigned viewCreates, viewDestroys, memoryAllocates, memoryFrees;
static unsigned mapCalls, unmapCalls, bindBufferCalls, bindImageCalls;
static unsigned commandAllocates, commandFrees, fenceCreates, fenceDestroys;
static unsigned queueSubmits, fenceWaits, fillCommands, copyCommands, barrierCommands;
static unsigned legacyBufferQueries, legacyImageQueries;
static unsigned dedicatedAllocations, importedAllocations, hostPropertyCalls;
static _Atomic uint64_t allocatedBytes;
static uint64_t lastBufferBindOffset, lastImageBindOffset;
static HeapTestBuffer *lastBuffer;
static HeapTestImage *lastImage;
static HeapTestMemory *lastMemory;
static BOOL lastBufferExternal;
static VkImageCreateFlags lastImageFlags;
static VkBufferCreateInfo lastBufferCreate;
static VkImageCreateInfo lastImageCreate;
static uint64_t lastFillSize;
static VkDeviceSize lastFillOffset;
static VkBuffer lastFillBuffer;
static uint64_t lastCopySize, lastCopySourceOffset, lastCopyDestinationOffset;
static VkBuffer lastCopySource, lastCopyDestination;
static VkDeviceSize lastAllocationSize;
static uint32_t lastAllocationMemoryType;
static BOOL lastAllocationDedicated;
static BOOL forceEdgeRequirements;

static BOOL forceRequirements;
static VkMemoryRequirements forcedRequirements;
static BOOL requirementsPreferDedicated;
static BOOL requirementsRequireDedicated;
static void *lastImportedHost;

static void failf(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void failf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("HEAP_EXECUTION_FAIL ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

static void require(BOOL condition, const char *message) {
    if (!condition) failf("%s", message);
}

static void checkMagic(const void *handle, uint32_t magic, const char *what) {
    const uint32_t *value = (const uint32_t *)handle;
    require(handle && *value == magic, what);
}

static VkResult VKAPI_CALL testCreateBuffer(VkDevice device,
                                             const VkBufferCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkBuffer *buffer) {
    (void)device;
    (void)allocator;
    require(info && buffer && info->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO &&
            info->size > 0, "invalid vkCreateBuffer request");
    bufferCreates++;
    if (failurePoint == FailCreateBuffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    HeapTestBuffer *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkBuffer");
    created->magic = kBufferMagic;
    created->create = *info;
    lastBufferCreate = *info;
    lastBufferCreate.pNext = NULL;
    created->create.pNext = NULL;
    created->external = info->pNext != NULL;
    if (created->external) {
        const VkExternalMemoryBufferCreateInfo *external =
            (const VkExternalMemoryBufferCreateInfo *)info->pNext;
        require(external->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO &&
                external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                "heap buffer lost its external host-allocation createInfo");
    }
    *buffer = (VkBuffer)created;
    liveBuffers++;
    lastBuffer = created;
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyBuffer(VkDevice device, VkBuffer buffer,
                                          const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    HeapTestBuffer *destroyed = (HeapTestBuffer *)buffer;
    checkMagic(destroyed, kBufferMagic, "double or foreign vkDestroyBuffer");
    if (destroyed->memory) {
        require(destroyed->memory->boundBuffers > 0,
                "buffer destroy underflowed its memory binding");
        destroyed->memory->boundBuffers--;
        destroyed->memory = NULL;
    }
    lastBufferExternal = destroyed->external;
    destroyed->magic = 0;
    free(destroyed);
    liveBuffers--;
    bufferDestroys++;
}

static void fillRequirements(VkMemoryRequirements2 *out, VkDeviceSize fallbackSize,
                             VkDeviceSize fallbackAlignment, uint32_t fallbackBits) {
    require(out && out->sType == VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            "requirements2 lost its structure type");
    out->memoryRequirements = forceRequirements
        ? forcedRequirements
        : (VkMemoryRequirements){fallbackSize, fallbackAlignment, fallbackBits};
    VkMemoryDedicatedRequirements *dedicated =
        (VkMemoryDedicatedRequirements *)out->pNext;
    require(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
            "requirements2 lost the dedicated requirements chain");
    dedicated->prefersDedicatedAllocation =
        requirementsPreferDedicated ? VK_TRUE : VK_FALSE;
    dedicated->requiresDedicatedAllocation =
        requirementsRequireDedicated ? VK_TRUE : VK_FALSE;
}

static void VKAPI_CALL testBufferRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *out) {
    (void)device;
    require(info && info->sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
            "invalid buffer requirements2 request");
    HeapTestBuffer *buffer = (HeapTestBuffer *)info->buffer;
    checkMagic(buffer, kBufferMagic, "buffer requirements2 used an invalid handle");
    if (buffer->create.size == 4) {
        // The production tail initializer creates a four-byte Shared edge
        // outside the heap. It must stay small even while a test forces the
        // heap resource's otherwise-large requirements.
        if (!forceEdgeRequirements) {
            fillRequirements(out, 4, 256, 2);
            buffer->requirements = out->memoryRequirements;
            buffer->hasRequirements = YES;
            return;
        }
        VkMemoryDedicatedRequirements *dedicated =
            (VkMemoryDedicatedRequirements *)out->pNext;
        require(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
                "edge requirements2 lost the dedicated requirements chain");
        out->memoryRequirements = (VkMemoryRequirements){4, 256, 2};
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
        buffer->requirements = out->memoryRequirements;
        buffer->hasRequirements = YES;
        return;
    }
    fillRequirements(out, buffer->create.size, 256, 1);
    buffer->requirements = out->memoryRequirements;
    buffer->hasRequirements = YES;
}

static void VKAPI_CALL testLegacyBufferRequirements(VkDevice device, VkBuffer buffer,
                                                     VkMemoryRequirements *out) {
    (void)device;
    HeapTestBuffer *queried = (HeapTestBuffer *)buffer;
    checkMagic(queried, kBufferMagic, "legacy buffer requirements used an invalid handle");
    legacyBufferQueries++;
    *out = (VkMemoryRequirements){queried->create.size, 256, 1};
    queried->requirements = *out;
    queried->hasRequirements = YES;
}

static VkResult VKAPI_CALL testCreateImage(VkDevice device,
                                            const VkImageCreateInfo *info,
                                            const VkAllocationCallbacks *allocator,
                                            VkImage *image) {
    (void)device;
    (void)allocator;
    require(info && image && info->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO &&
            (info->flags & VK_IMAGE_CREATE_ALIAS_BIT),
            "heap image createInfo lost ALIAS_BIT");
    imageCreates++;
    if (failurePoint == FailCreateImage) return VK_ERROR_OUT_OF_HOST_MEMORY;
    HeapTestImage *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkImage");
    created->magic = kImageMagic;
    created->create = *info;
    lastImageCreate = *info;
    lastImageCreate.pNext = NULL;
    created->create.pNext = NULL;
    *image = (VkImage)created;
    liveImages++;
    lastImage = created;
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyImage(VkDevice device, VkImage image,
                                         const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    HeapTestImage *destroyed = (HeapTestImage *)image;
    checkMagic(destroyed, kImageMagic, "double or foreign vkDestroyImage");
    if (destroyed->memory) {
        require(destroyed->memory->boundImages > 0,
                "image destroy underflowed its memory binding");
        destroyed->memory->boundImages--;
        destroyed->memory = NULL;
    }
    lastImageFlags = destroyed->create.flags;
    destroyed->magic = 0;
    free(destroyed);
    liveImages--;
    imageDestroys++;
}

static void VKAPI_CALL testImageRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *out) {
    (void)device;
    require(info && info->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            "invalid image requirements2 request");
    HeapTestImage *image = (HeapTestImage *)info->image;
    checkMagic(image, kImageMagic, "image requirements2 used an invalid handle");
    uint64_t fallback = (uint64_t)image->create.extent.width *
                        image->create.extent.height *
                        image->create.extent.depth * 4;
    fillRequirements(out, fallback, 16384, 1);
    image->requirements = out->memoryRequirements;
    image->hasRequirements = YES;
}

static void VKAPI_CALL testLegacyImageRequirements(VkDevice device, VkImage image,
                                                    VkMemoryRequirements *out) {
    (void)device;
    HeapTestImage *queried = (HeapTestImage *)image;
    checkMagic(queried, kImageMagic, "legacy image requirements used an invalid handle");
    legacyImageQueries++;
    *out = (VkMemoryRequirements){4096, 256, 1};
    queried->requirements = *out;
    queried->hasRequirements = YES;
}

static VkResult VKAPI_CALL testCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view) {
    (void)device;
    (void)allocator;
    require(info && view && info->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            "invalid vkCreateImageView request");
    checkMagic((void *)info->image, kImageMagic, "image view used an invalid image");
    viewCreates++;
    if (failurePoint == FailCreateView) return VK_ERROR_OUT_OF_HOST_MEMORY;
    HeapTestView *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkImageView");
    created->magic = kViewMagic;
    created->image = (HeapTestImage *)info->image;
    *view = (VkImageView)created;
    liveViews++;
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyImageView(VkDevice device, VkImageView view,
                                             const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    HeapTestView *destroyed = (HeapTestView *)view;
    checkMagic(destroyed, kViewMagic, "double or foreign vkDestroyImageView");
    checkMagic(destroyed->image, kImageMagic,
               "image view outlived its image");
    destroyed->magic = 0;
    free(destroyed);
    liveViews--;
    viewDestroys++;
}

static void VKAPI_CALL testFormatProperties(VkPhysicalDevice physicalDevice,
                                             VkFormat format,
                                             VkFormatProperties *properties) {
    (void)physicalDevice;
    (void)format;
    require(properties != NULL, "invalid format properties request");
    properties->optimalTilingFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

static VkResult VKAPI_CALL testImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *properties) {
    (void)physicalDevice;
    (void)format;
    (void)type;
    (void)tiling;
    (void)usage;
    (void)flags;
    require(properties != NULL, "invalid image format properties request");
    memset(properties, 0, sizeof *properties);
    properties->maxArrayLayers = 2048;
    properties->maxMipLevels = 15;
    properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                               VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
    properties->maxExtent = (VkExtent3D){65536, 65536, 8192};
    properties->maxResourceSize = UINT64_C(1) << 34;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testHostPointerProperties(
    VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
    const void *pointer, VkMemoryHostPointerPropertiesEXT *properties) {
    (void)device;
    require(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT &&
            pointer && properties, "invalid host import properties request");
    hostPropertyCalls++;
    require(((uintptr_t)pointer & (RDNA4R.hostAlign - 1)) == 0,
            "heap host import pointer is not aligned");
    lastImportedHost = (void *)pointer;
    if (failurePoint == FailHostProperties) return VK_ERROR_MEMORY_MAP_FAILED;
    properties->memoryTypeBits = 1u << 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    (void)device;
    (void)allocator;
    require(info && memory && info->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO &&
            info->allocationSize > 0, "invalid vkAllocateMemory request");
    memoryAllocates++;
    if (failurePoint == FailAllocate) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    const VkMemoryAllocateFlagsInfo *flags = NULL;
    const VkMemoryDedicatedAllocateInfo *dedicated = NULL;
    const VkImportMemoryHostPointerInfoEXT *imported = NULL;
    const void *next = info->pNext;
    unsigned links = 0;
    while (next) {
        require(++links <= 3, "unexpected heap allocation pNext chain");
        VkStructureType type = *(const VkStructureType *)next;
        if (type == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            require(flags == NULL, "duplicate allocation flags");
            flags = (const VkMemoryAllocateFlagsInfo *)next;
            next = flags->pNext;
        } else if (type == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            require(dedicated == NULL, "duplicate dedicated allocation info");
            dedicated = (const VkMemoryDedicatedAllocateInfo *)next;
            next = dedicated->pNext;
        } else if (type == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) {
            require(imported == NULL, "duplicate host import info");
            imported = (const VkImportMemoryHostPointerInfoEXT *)next;
            next = imported->pNext;
        } else {
            failf("unexpected allocation pNext sType=%d", type);
        }
    }

    if (dedicated) {
        require(dedicated->buffer || dedicated->image,
                "dedicated allocation names neither a buffer nor an image");
        if (dedicated->buffer)
            checkMagic((void *)dedicated->buffer, kBufferMagic,
                       "dedicated allocation has an invalid buffer");
        if (dedicated->image)
            checkMagic((void *)dedicated->image, kImageMagic,
                       "dedicated allocation has an invalid image");
        dedicatedAllocations++;
    }
    if (imported) {
        require(imported->handleType ==
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT &&
                imported->pHostPointer != NULL,
                "host import allocation lost its pointer or handle type");
        require(((uintptr_t)imported->pHostPointer & (RDNA4R.hostAlign - 1)) == 0,
                "host import allocation is not aligned");
        importedAllocations++;
        lastImportedHost = imported->pHostPointer;
    }

    HeapTestMemory *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkDeviceMemory");
    created->magic = kMemoryMagic;
    created->size = info->allocationSize;
    created->memoryType = info->memoryTypeIndex;
    created->imported = imported != NULL;
    created->hostPointer = imported ? imported->pHostPointer : NULL;
    if (dedicated) {
        created->dedicatedBuffer = dedicated->buffer;
        created->dedicatedImage = dedicated->image;
    }
    atomic_fetch_add_explicit(&allocatedBytes, info->allocationSize,
                              memory_order_relaxed);
    *memory = (VkDeviceMemory)created;
    lastMemory = created;
    lastAllocationSize = info->allocationSize;
    lastAllocationMemoryType = info->memoryTypeIndex;
    lastAllocationDedicated = dedicated != NULL;
    liveMemories++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testMapMemory(VkDevice device, VkDeviceMemory memory,
                                          VkDeviceSize offset, VkDeviceSize size,
                                          VkMemoryMapFlags flags, void **pointer) {
    (void)device;
    (void)flags;
    HeapTestMemory *mapped = (HeapTestMemory *)memory;
    checkMagic(mapped, kMemoryMagic, "vkMapMemory used an invalid handle");
    require(pointer && !mapped->map && offset == 0 &&
            (size == VK_WHOLE_SIZE || size == mapped->size),
            "invalid heap mapping request");
    if (failurePoint == FailMap) return VK_ERROR_MEMORY_MAP_FAILED;
    void *cpu = NULL;
    require(posix_memalign(&cpu, 4096, (size_t)mapped->size) == 0 && cpu,
            "cannot allocate simulated Vulkan mapping");
    memset(cpu, 0, (size_t)mapped->size);
    mapped->map = cpu;
    *pointer = cpu;
    mapCalls++;
    liveMappings++;
    return VK_SUCCESS;
}

static void VKAPI_CALL testUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device;
    HeapTestMemory *mapped = (HeapTestMemory *)memory;
    checkMagic(mapped, kMemoryMagic, "vkUnmapMemory used an invalid handle");
    require(mapped->map != NULL, "vkUnmapMemory without a mapping");
    free(mapped->map);
    mapped->map = NULL;
    unmapCalls++;
    liveMappings--;
}

static void VKAPI_CALL testFreeMemory(VkDevice device, VkDeviceMemory memory,
                                       const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    HeapTestMemory *freed = (HeapTestMemory *)memory;
    checkMagic(freed, kMemoryMagic, "double or foreign vkFreeMemory");
    require(freed->boundBuffers == 0 && freed->boundImages == 0,
            "VkDeviceMemory freed while a buffer/image handle still uses it");
    if (freed->map) {
        // Vulkan implicitly ends a mapping when the memory is freed.
        free(freed->map);
        freed->map = NULL;
        liveMappings--;
    }
    atomic_fetch_sub_explicit(&allocatedBytes, freed->size,
                              memory_order_relaxed);
    freed->magic = 0;
    free(freed);
    liveMemories--;
    memoryFrees++;
}

static VkResult VKAPI_CALL testBindBufferMemory(VkDevice device, VkBuffer buffer,
                                                 VkDeviceMemory memory,
                                                 VkDeviceSize offset) {
    (void)device;
    HeapTestBuffer *bound = (HeapTestBuffer *)buffer;
    HeapTestMemory *backing = (HeapTestMemory *)memory;
    checkMagic(bound, kBufferMagic, "vkBindBufferMemory used an invalid buffer");
    checkMagic(backing, kMemoryMagic, "vkBindBufferMemory used an invalid memory");
    require(!bound->memory, "buffer bound twice");
    require(bound->hasRequirements,
            "buffer bound before its requirements were queried");
    require(offset < backing->size, "buffer bind offset is outside its backing");
    require(offset % bound->requirements.alignment == 0,
            "buffer bind offset is not Vulkan-aligned");
    require(bound->requirements.size <= backing->size - offset,
            "buffer requirement does not fit at its bind offset");
    bindBufferCalls++;
    lastBufferBindOffset = offset;
    if (failurePoint == FailBindBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bound->memory = backing;
    backing->boundBuffers++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testBindImageMemory(VkDevice device, VkImage image,
                                                VkDeviceMemory memory,
                                                VkDeviceSize offset) {
    (void)device;
    HeapTestImage *bound = (HeapTestImage *)image;
    HeapTestMemory *backing = (HeapTestMemory *)memory;
    checkMagic(bound, kImageMagic, "vkBindImageMemory used an invalid image");
    checkMagic(backing, kMemoryMagic, "vkBindImageMemory used an invalid memory");
    require(!bound->memory, "image bound twice");
    require(bound->hasRequirements,
            "image bound before its requirements were queried");
    require(offset < backing->size, "image bind offset is outside its backing");
    require(offset % bound->requirements.alignment == 0,
            "image bind offset is not Vulkan-aligned");
    require(bound->requirements.size <= backing->size - offset,
            "image requirement does not fit at its bind offset");
    bindImageCalls++;
    lastImageBindOffset = offset;
    if (failurePoint == FailBindImage) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bound->memory = backing;
    backing->boundImages++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *commands) {
    (void)device;
    require(info && commands &&
            info->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO &&
            info->commandBufferCount == 1,
            "invalid command-buffer allocation request");
    HeapTestCommand *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkCommandBuffer");
    created->magic = kCommandMagic;
    commands[0] = (VkCommandBuffer)created;
    liveCommands++;
    commandAllocates++;
    return VK_SUCCESS;
}

static void VKAPI_CALL testFreeCommandBuffers(VkDevice device,
                                               VkCommandPool commandPool,
                                               uint32_t count,
                                               const VkCommandBuffer *commands) {
    (void)device;
    (void)commandPool;
    require(commands != NULL || count == 0, "NULL command-buffer free list");
    for (uint32_t i = 0; i < count; i++) {
        HeapTestCommand *freed = (HeapTestCommand *)commands[i];
        checkMagic(freed, kCommandMagic, "double or foreign vkFreeCommandBuffers");
        freed->magic = 0;
        free(freed);
        liveCommands--;
        commandFrees++;
    }
}

static VkResult VKAPI_CALL testBeginCommandBuffer(
    VkCommandBuffer command, const VkCommandBufferBeginInfo *info) {
    checkMagic((void *)command, kCommandMagic, "vkBeginCommandBuffer used an invalid handle");
    require(info && info->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            "invalid command-buffer beginInfo");
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testEndCommandBuffer(VkCommandBuffer command) {
    checkMagic((void *)command, kCommandMagic, "vkEndCommandBuffer used an invalid handle");
    return VK_SUCCESS;
}

static void VKAPI_CALL testCmdFillBuffer(VkCommandBuffer command, VkBuffer buffer,
                                          VkDeviceSize offset, VkDeviceSize size,
                                          uint32_t value) {
    checkMagic((void *)command, kCommandMagic, "vkCmdFillBuffer used an invalid command buffer");
    checkMagic((void *)buffer, kBufferMagic, "vkCmdFillBuffer used an invalid buffer");
    HeapTestBuffer *filled = (HeapTestBuffer *)buffer;
    require(filled->memory && filled->memory->map == NULL,
            "large private heap initializer filled mapped or unbound memory");
    require(size > 0 && offset == 0 && value == 0 && !(size & 3) &&
                size <= filled->create.size,
            "large private heap initializer issued a wrong fill");
    fillCommands++;
    lastFillOffset = offset;
    lastFillSize = size;
    lastFillBuffer = buffer;
}

static void VKAPI_CALL testCmdCopyBuffer(VkCommandBuffer command, VkBuffer source,
                                          VkBuffer destination, uint32_t count,
                                          const VkBufferCopy *regions) {
    checkMagic((void *)command, kCommandMagic, "vkCmdCopyBuffer used an invalid command buffer");
    checkMagic((void *)source, kBufferMagic, "vkCmdCopyBuffer used an invalid source");
    checkMagic((void *)destination, kBufferMagic, "vkCmdCopyBuffer used an invalid destination");
    require(count == 1 && regions && regions[0].size > 0,
            "large private heap initializer issued a wrong tail copy");
    HeapTestBuffer *sourceBuffer = (HeapTestBuffer *)source;
    HeapTestBuffer *destinationBuffer = (HeapTestBuffer *)destination;
    const VkBufferCopy *copy = &regions[0];
    require(copy->size < 4 && copy->srcOffset <= 4 &&
                copy->size <= 4 - copy->srcOffset &&
                copy->dstOffset <= destinationBuffer->create.size &&
                copy->size <= destinationBuffer->create.size - copy->dstOffset,
            "large private heap initializer tail is outside a buffer");
    require(sourceBuffer->memory && sourceBuffer->memory->map,
            "large private heap initializer source is not host mapped");
    for (VkDeviceSize i = 0; i < copy->size; i++)
        require(((const uint8_t *)sourceBuffer->memory->map)[copy->srcOffset + i] == 0,
                "large private heap initializer tail staging is not zero");
    copyCommands++;
    lastCopySource = source;
    lastCopyDestination = destination;
    lastCopySize = copy->size;
    lastCopySourceOffset = copy->srcOffset;
    lastCopyDestinationOffset = copy->dstOffset;
}

static void VKAPI_CALL testCmdPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *memoryBarriers,
    uint32_t bufferBarrierCount, const VkBufferMemoryBarrier *bufferBarriers,
    uint32_t imageBarrierCount, const VkImageMemoryBarrier *imageBarriers) {
    (void)sourceStage;
    (void)destinationStage;
    (void)dependencyFlags;
    (void)memoryBarrierCount;
    (void)memoryBarriers;
    (void)bufferBarrierCount;
    (void)bufferBarriers;
    (void)imageBarrierCount;
    (void)imageBarriers;
    checkMagic((void *)command, kCommandMagic,
               "vkCmdPipelineBarrier used an invalid command buffer");
    barrierCommands++;
}

static VkResult VKAPI_CALL testCreateFence(
    VkDevice device, const VkFenceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device;
    (void)allocator;
    require(info && fence && info->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            "invalid vkCreateFence request");
    fenceCreates++;
    if (failurePoint == FailCreateFence) return VK_ERROR_OUT_OF_HOST_MEMORY;
    HeapTestFence *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkFence");
    created->magic = kFenceMagic;
    *fence = (VkFence)created;
    liveFences++;
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyFence(VkDevice device, VkFence fence,
                                         const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    HeapTestFence *destroyed = (HeapTestFence *)fence;
    checkMagic(destroyed, kFenceMagic, "double or foreign vkDestroyFence");
    destroyed->magic = 0;
    free(destroyed);
    liveFences--;
    fenceDestroys++;
}

static VkResult VKAPI_CALL testQueueSubmit(VkQueue queue, uint32_t submitCount,
                                            const VkSubmitInfo *submits,
                                            VkFence fence) {
    (void)queue;
    require(submitCount == 1 && submits && submits[0].commandBufferCount == 1 &&
            submits[0].pCommandBuffers,
            "invalid vkQueueSubmit request");
    checkMagic((void *)submits[0].pCommandBuffers[0], kCommandMagic,
               "vkQueueSubmit used an invalid command buffer");
    checkMagic((void *)fence, kFenceMagic, "vkQueueSubmit used an invalid fence");
    queueSubmits++;
    if (failurePoint == FailQueueSubmit) return VK_ERROR_DEVICE_LOST;
    ((HeapTestFence *)fence)->signaled = YES;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testWaitForFences(VkDevice device, uint32_t fenceCount,
                                              const VkFence *fences,
                                              VkBool32 waitAll,
                                              uint64_t timeout) {
    (void)device;
    (void)waitAll;
    require(fenceCount == 1 && fences != NULL, "invalid vkWaitForFences request");
    require(timeout == UINT64_C(2000000000),
            "large private heap initializer used an unbounded fence wait");
    checkMagic((void *)fences[0], kFenceMagic, "vkWaitForFences used an invalid fence");
    fenceWaits++;
    if (failurePoint == FailFenceWait) return VK_TIMEOUT;
    return ((HeapTestFence *)fences[0])->signaled ? VK_SUCCESS : VK_NOT_READY;
}

static void resetCounters(void) {
    require(liveBuffers == 0 && liveImages == 0 && liveViews == 0 &&
            liveMemories == 0 && liveMappings == 0 &&
            liveCommands == 0 && liveFences == 0,
            "test handles leaked between cases");
    require(hostLiveBytes == 0 && hostMappedBytes == hostUnmappedBytes,
            "test host mappings leaked between cases");
    require(atomic_load(&rdna4_alloc_total) == 0 &&
            atomic_load(&rdna4_heap_reserved) == 0 && RDNA4R.impTotal == 0,
            "production accounting leaked between cases");
    require(rdna4_unresolved_commands.count == 0 &&
            atomic_load(&rdna4_submit_fault) == VK_SUCCESS,
            "production quarantine leaked between cases");
    failurePoint = FailNone;
    bufferCreates = bufferDestroys = imageCreates = imageDestroys = 0;
    viewCreates = viewDestroys = memoryAllocates = memoryFrees = 0;
    mapCalls = unmapCalls = bindBufferCalls = bindImageCalls = 0;
    commandAllocates = commandFrees = fenceCreates = fenceDestroys = 0;
    queueSubmits = fenceWaits = fillCommands = copyCommands = barrierCommands = 0;
    legacyBufferQueries = legacyImageQueries = 0;
    dedicatedAllocations = importedAllocations = hostPropertyCalls = 0;
    allocatedBytes = 0;
    lastBufferBindOffset = lastImageBindOffset = 0;
    lastBuffer = NULL;
    lastImage = NULL;
    lastMemory = NULL;
    lastBufferExternal = NO;
    lastImageFlags = 0;
    memset(&lastBufferCreate, 0, sizeof lastBufferCreate);
    memset(&lastImageCreate, 0, sizeof lastImageCreate);
    lastFillOffset = lastFillSize = 0;
    lastFillBuffer = VK_NULL_HANDLE;
    lastCopySource = lastCopyDestination = VK_NULL_HANDLE;
    lastCopySize = lastCopySourceOffset = lastCopyDestinationOffset = 0;
    lastAllocationSize = lastAllocationMemoryType = 0;
    lastAllocationDedicated = NO;
    forceEdgeRequirements = NO;
    forceRequirements = NO;
    memset(&forcedRequirements, 0, sizeof forcedRequirements);
    requirementsPreferDedicated = NO;
    requirementsRequireDedicated = NO;
    lastImportedHost = NULL;
    hostMaps = hostUnmaps = hostMappedBytes = hostUnmappedBytes = 0;
    refuseProductionRealloc = 0;
}

static void configureVulkan(void) {
    RDNA4R.tried = RDNA4R.ok = YES;
    RDNA4R.hostExt = YES;
    RDNA4R.bufferAddress = NO;
    RDNA4R.dev = (VkDevice)(uintptr_t)0x1000;
    RDNA4R.pd = (VkPhysicalDevice)(uintptr_t)0x2000;
    RDNA4R.q = (VkQueue)(uintptr_t)0x3000;
    RDNA4R.opool = (VkCommandPool)(uintptr_t)0x4000;
    pthread_mutex_init(&RDNA4R.qlock, NULL);
    RDNA4R.hostAlign = 256 * 1024;
    RDNA4R.lim.bufferImageGranularity = 64 * 1024;
    RDNA4R.lim.maxImageDimension1D = 65536;
    RDNA4R.lim.maxImageDimension2D = 65536;
    RDNA4R.lim.maxImageDimension3D = 8192;
    RDNA4R.lim.maxImageArrayLayers = 2048;
    RDNA4R.lim.framebufferColorSampleCounts = 0xff;
    RDNA4R.lim.framebufferDepthSampleCounts = 0xff;
    RDNA4R.lim.framebufferStencilSampleCounts = 0xff;
    RDNA4R.mp.memoryTypeCount = 2;
    RDNA4R.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    RDNA4R.mp.memoryTypes[0].heapIndex = 0;
    RDNA4R.mp.memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    RDNA4R.mp.memoryTypes[1].heapIndex = 0;
    RDNA4R.mp.memoryHeapCount = 1;
    RDNA4R.mp.memoryHeaps[0].size = UINT64_C(1) << 30;
    RDNA4R.mp.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    RDNA4Caps = rdna4dc_fallback();
    RDNA4Caps.allocation_budget = UINT64_C(512) * kMiB;
    RDNA4Caps.working_set = UINT64_C(384) * kMiB;
    RDNA4Caps.buffer_length = UINT64_C(8) << 30;
    atomic_store(&rdna4_caps_ready, 1);

    RDNA4P.on = NO;
    RDNA4P.reqOK = YES;
    RDNA4P.bmr2 = testBufferRequirements2;
    RDNA4P.imr2 = testImageRequirements2;
    rdna4p_init(&RDNA4P.p, 0);
    rdna4p_init(&RDNA4HT.p, 1);
    RDNA4HT.ready = NO;

    vkCreateBuffer = testCreateBuffer;
    vkDestroyBuffer = testDestroyBuffer;
    vkGetBufferMemoryRequirements = testLegacyBufferRequirements;
    vkCreateImage = testCreateImage;
    vkDestroyImage = testDestroyImage;
    vkGetImageMemoryRequirements = testLegacyImageRequirements;
    vkCreateImageView = testCreateImageView;
    vkDestroyImageView = testDestroyImageView;
    vkGetPhysicalDeviceFormatProperties = testFormatProperties;
    vkGetPhysicalDeviceImageFormatProperties = testImageFormatProperties;
    vkAllocateMemory = testAllocateMemory;
    vkFreeMemory = testFreeMemory;
    vkMapMemory = testMapMemory;
    vkUnmapMemory = testUnmapMemory;
    vkBindBufferMemory = testBindBufferMemory;
    vkBindImageMemory = testBindImageMemory;
    vkAllocateCommandBuffers = testAllocateCommandBuffers;
    vkFreeCommandBuffers = testFreeCommandBuffers;
    vkBeginCommandBuffer = testBeginCommandBuffer;
    vkEndCommandBuffer = testEndCommandBuffer;
    vkCmdFillBuffer = testCmdFillBuffer;
    vkCmdCopyBuffer = testCmdCopyBuffer;
    vkCmdPipelineBarrier = testCmdPipelineBarrier;
    vkCreateFence = testCreateFence;
    vkDestroyFence = testDestroyFence;
    vkQueueSubmit = testQueueSubmit;
    vkWaitForFences = testWaitForFences;
    rdna4_vkGetMHPP = testHostPointerProperties;
    rdna4_vkGetBufferAddress = NULL;
}

static RDNA4Heap *makeHeap(GarnetDevice *device, NSUInteger size,
                           MTLStorageMode storage) {
    MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
    descriptor.type = MTLHeapTypeAutomatic;
    descriptor.size = size;
    descriptor.storageMode = storage;
    descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeDefault;
    RDNA4Heap *heap = (RDNA4Heap *)[device newHeapWithDescriptor:descriptor];
    require(heap != nil, "production heap creation failed");
    return heap;
}

static MTLTextureDescriptor *textureDescriptor(MTLStorageMode storage) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.width = 1024;
    descriptor.height = 1024;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.arrayLength = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = storage;
    descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeDefault;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    return descriptor;
}

static uint64_t alignUp(uint64_t value, uint64_t alignment) {
    require(alignment && !(alignment & (alignment - 1)), "alignment is not a power of two");
    require(value <= UINT64_MAX - (alignment - 1), "alignment overflow");
    return (value + alignment - 1) & ~(alignment - 1);
}

static void checkCleanBacking(RDNA4Heap *heap) {
    require([heap usedSize] == 0, "heap usedSize did not roll back");
    require([heap rdna4BackingBytes] == 0, "heap retained physical backing");
    require([heap rdna4BackingInUse] == 0, "heap retained physical ranges");
}

// The capacity and newHeap path must stay RADV-free, while SizeAndAlign is an
// exact resource query and may open RADV.
static void testCapacityQueriesStayCold(GarnetDevice *device) {
    resetCounters();
    RDNA4R.ok = NO;
    RDNA4R.tried = NO;
    require([device maxBufferLength] == RDNA4Caps.buffer_length,
            "cold maxBufferLength changed");
    require([device recommendedMaxWorkingSetSize] == RDNA4Caps.working_set,
            "cold recommended working set changed");
    NSString *fallbackName = [NSString stringWithUTF8String:rdna4dc_fallback().name];
    require([[device name] isEqualToString:fallbackName] &&
                [[device productName] isEqualToString:fallbackName],
            "cold Metal identity changed");
    require(!RDNA4R.tried, "identity query opened RADV");
    RDNA4Heap *heap = makeHeap(device, 8 * kMiB, MTLStorageModePrivate);
    require(!RDNA4R.tried, "newHeapWithDescriptor opened RADV");
    require([heap size] == 8 * kMiB, "newHeap capacity changed");
    heap = nil;
    require(atomic_load(&rdna4_heap_reserved) == 0,
            "capacity query left a heap reservation");
    RDNA4R.ok = YES;
    RDNA4R.tried = YES;
    puts("HEAP_EXECUTION_CASE_PASS capacity_queries radv_open=0");
}

static void testExactSizeAndAlign(GarnetDevice *device) {
    resetCounters();
    forcedRequirements = (VkMemoryRequirements){
        .size = 131073,
        .alignment = 16384,
        .memoryTypeBits = 1,
    };
    forceRequirements = YES;
    MTLSizeAndAlign buffer = [device heapBufferSizeAndAlignWithLength:131073
                                                             options:MTLResourceStorageModePrivate];
    uint64_t expected = alignUp(131073, 64 * 1024);
    require(buffer.size == expected && buffer.align == 64 * 1024,
            "buffer SizeAndAlign did not use requirements2 plus granularity");
    require(bufferCreates == 1 && bufferDestroys == 1 &&
            memoryAllocates == 0 && bindBufferCalls == 0,
            "buffer SizeAndAlign allocated or bound backing");
    require(lastBufferCreate.sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO &&
                lastBufferCreate.size == 131073 && lastBufferCreate.pNext == NULL,
            "buffer SizeAndAlign did not create the exact logical buffer");
    require(!lastBufferExternal,
            "small heap buffer query unexpectedly requested external memory");
    resetCounters();

    forcedRequirements = (VkMemoryRequirements){
        .size = (64 * kMiB) + 1,
        .alignment = 4096,
        .memoryTypeBits = 2,
    };
    forceRequirements = YES;
    MTLSizeAndAlign imported = [device
        heapBufferSizeAndAlignWithLength:(64 * kMiB) + 1
                                options:MTLResourceStorageModeShared];
    expected = alignUp(alignUp((64 * kMiB) + 1, 64 * 1024), 256 * 1024);
    require(imported.size == expected && imported.align == 256 * 1024,
            "large Shared SizeAndAlign lost host alignment/extent");
    require(bufferCreates == 1 && bufferDestroys == 1 &&
            memoryAllocates == 0 && hostPropertyCalls == 0,
            "large Shared SizeAndAlign allocated/imported backing");
    require(lastBufferCreate.size == (64 * kMiB) + 1 &&
                lastBufferExternal,
            "large Shared SizeAndAlign did not create the exact external buffer");
    resetCounters();

    forcedRequirements = (VkMemoryRequirements){
        .size = (5 * kMiB) + 17,
        .alignment = 16384,
        .memoryTypeBits = 1,
    };
    forceRequirements = YES;
    MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModePrivate);
    MTLSizeAndAlign texture = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
    expected = alignUp((5 * kMiB) + 17, 64 * 1024);
    require(texture.size == expected && texture.align == 64 * 1024,
            "texture SizeAndAlign did not use image requirements2");
    require(imageCreates == 1 && imageDestroys == 1 &&
            memoryAllocates == 0 && bindImageCalls == 0 && viewCreates == 0,
            "texture SizeAndAlign allocated, bound or viewed an image");
    require(lastImageCreate.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO &&
                lastImageCreate.imageType == VK_IMAGE_TYPE_2D &&
                lastImageCreate.extent.width == 1024 &&
                lastImageCreate.extent.height == 1024 &&
                lastImageCreate.extent.depth == 1,
            "texture SizeAndAlign did not create the exact temporary image");
    require((lastImageFlags & VK_IMAGE_CREATE_ALIAS_BIT) != 0,
            "texture SizeAndAlign temp image lost ALIAS_BIT");
    resetCounters();

    RDNA4P.bmr2 = NULL;
    buffer = [device heapBufferSizeAndAlignWithLength:4096
                                             options:MTLResourceStorageModePrivate];
    require(buffer.size == 0 && buffer.align == 0 && bufferCreates == 0,
            "missing buffer requirements2 used a synthetic fallback");
    RDNA4P.bmr2 = testBufferRequirements2;
    RDNA4P.imr2 = NULL;
    texture = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
    require(texture.size == 0 && texture.align == 0 && imageCreates == 0,
            "missing image requirements2 used a synthetic fallback");
    RDNA4P.imr2 = testImageRequirements2;
    require(legacyBufferQueries == 0 && legacyImageQueries == 0,
            "heap query fell back to the Vulkan 1.0 requirements");
    resetCounters();

    failurePoint = FailCreateBuffer;
    buffer = [device heapBufferSizeAndAlignWithLength:4096
                                             options:MTLResourceStorageModePrivate];
    require(buffer.size == 0 && buffer.align == 0 &&
            memoryAllocates == 0 && bufferDestroys == 0,
            "buffer query failure did not return zero without a synthetic size");
    resetCounters();
    failurePoint = FailCreateImage;
    texture = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
    require(texture.size == 0 && texture.align == 0 &&
            memoryAllocates == 0 && imageDestroys == 0,
            "image query failure did not return zero without a synthetic size");
    resetCounters();

    RDNA4R.ok = NO;
    RDNA4R.tried = YES;
    RDNA4R.err = rdna4_err(7, @"simulated open failure");
    buffer = [device heapBufferSizeAndAlignWithLength:4096
                                             options:MTLResourceStorageModePrivate];
    require(buffer.size == 0 && buffer.align == 0 && bufferCreates == 0,
            "SizeAndAlign ignored a RADV open failure");
    resetCounters();
    RDNA4R.ok = YES;
    RDNA4R.err = nil;
    puts("HEAP_EXECUTION_CASE_PASS size_align exact=1 backing_allocs=0 fallback=0");
}

static void testRequiredSizingRejectsHeapResources(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);
    RDNA4P.bmr2 = NULL;
    id buffer = [heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    require(buffer == nil, "heap buffer without requirements2 was accepted");
    require(bufferCreates == 1 && bufferDestroys == 1 &&
            memoryAllocates == 0 && liveBuffers == 0,
            "missing buffer requirements2 leaked a handle or allocated memory");
    checkCleanBacking(heap);
    RDNA4P.bmr2 = testBufferRequirements2;
    heap = nil;
    resetCounters();

    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);
    RDNA4P.imr2 = NULL;
    MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModePrivate);
    id texture = [heap newTextureWithDescriptor:descriptor];
    require(texture == nil, "heap texture without requirements2 was accepted");
    require(imageCreates == 1 && imageDestroys == 1 &&
            memoryAllocates == 0 && liveImages == 0,
            "missing image requirements2 leaked a handle or allocated memory");
    checkCleanBacking(heap);
    RDNA4P.imr2 = testImageRequirements2;
    require(legacyBufferQueries == 0 && legacyImageQueries == 0,
            "heap resource used the legacy requirements fallback");
    heap = nil;
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS requirements2_required buffers=1 textures=1");
}

static void testMemoryTypesAndOffsets(GarnetDevice *device) {
    resetCounters();
    @autoreleasepool {
        RDNA4Heap *heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

        forcedRequirements = (VkMemoryRequirements){1000, 4096, 2};
        forceRequirements = YES;
        RDNA4Buffer *type1 = (RDNA4Buffer *)[heap newBufferWithLength:1000
                                                            options:MTLResourceStorageModePrivate];
        require(type1 != nil, "type-1 heap buffer failed");
        uint64_t type1Block = [type1 rdna4BackingBlock];
        require(type1Block != 0 && [type1 rdna4BackingOffset] == 0 &&
                    [type1 allocatedSize] == 1000 &&
                    [type1 rdna4BackingSize] == 64 * 1024,
                "type-1 heap buffer has no backing range");
        HeapTestBuffer *type1Handle = (HeapTestBuffer *)[type1 vkBuffer];
        require(type1Handle->memory && type1Handle->memory->memoryType == 1,
                "type-1 requirement selected the wrong Vulkan memory type");

        forcedRequirements = (VkMemoryRequirements){4097, 4096, 1};
        RDNA4Buffer *type0a = (RDNA4Buffer *)[heap newBufferWithLength:4097
                                                             options:MTLResourceStorageModePrivate];
        RDNA4Buffer *type0b = (RDNA4Buffer *)[heap newBufferWithLength:4097
                                                             options:MTLResourceStorageModePrivate];
        require(type0a && type0b, "type-0 heap buffers failed");
        uint64_t type0Block = [type0a rdna4BackingBlock];
        require(type0Block != type1Block,
                "different memoryTypeBits shared one physical block");
        require([type0b rdna4BackingBlock] == type0Block,
                "compatible heap buffers did not share a block");
        require([type0a rdna4BackingOffset] == 0 &&
                [type0b rdna4BackingOffset] != 0,
                "second compatible buffer did not bind at a nonzero physical offset");
        require([type0b rdna4BackingOffset] % (64 * 1024) == 0,
                "physical backing offset ignored bufferImageGranularity");
        require(bindBufferCalls == 3 && lastBufferBindOffset != 0,
                "production bind calls were not made at real offsets");
        require([type0a allocatedSize] == 4097 && [type0b allocatedSize] == 4097 &&
                    [type0a rdna4BackingSize] == 64 * 1024 &&
                    [type0b rdna4BackingSize] == 64 * 1024,
                "heap resources did not preserve Vulkan requirements versus tight Metal bytes");
        require([heap usedSize] == 1000 + 4097 + 4097,
                "heap usedSize did not sum the tight Vulkan requirement sizes");
        HeapTestBuffer *type0Handle = (HeapTestBuffer *)[type0a vkBuffer];
        require(type0Handle->memory && type0Handle->memory->memoryType == 0,
                "type-0 requirement selected the wrong Vulkan memory type");
        require(dedicatedAllocations == 0,
                "non-dedicated requirements unexpectedly made dedicated allocations");
        type1 = nil;
        type0a = nil;
        type0b = nil;
        heap = nil;
    }
    require(bufferCreates == 3 && bufferDestroys == 3 &&
            memoryAllocates == 2 && memoryFrees == 2,
            "memory-type case leaked handles or backing");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS memory_types=2 blocks=2 offsets=aligned+nonzero");
}

static void testDedicatedRequirements(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){(2 * kMiB) + 1, 64 * 1024, 1};
    forceRequirements = YES;
    requirementsRequireDedicated = YES;
    RDNA4Buffer *buffer = (RDNA4Buffer *)[heap newBufferWithLength:(2 * kMiB) + 1
                                                          options:MTLResourceStorageModePrivate];
    require(buffer != nil && dedicatedAllocations == 1,
            "required dedicated allocation was not requested");
    require(lastAllocationDedicated && lastMemory &&
                lastMemory->dedicatedBuffer == [buffer vkBuffer],
            "required dedicated allocation did not name the exact buffer");
    require([buffer rdna4BackingSize] == alignUp((2 * kMiB) + 1, 64 * 1024),
            "dedicated block was not sized to the sorted requirement");
    require([buffer rdna4BackingOffset] == 0,
            "dedicated block did not bind at zero");
    buffer = nil;
    require(liveBuffers == 0 && liveMemories == 0 && memoryFrees == 1,
            "required dedicated lease was destroyed in the wrong order");
    heap = nil;
    resetCounters();

    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){3 * kMiB, 64 * 1024, 1};
    forceRequirements = YES;
    requirementsPreferDedicated = YES;
    buffer = (RDNA4Buffer *)[heap newBufferWithLength:3 * kMiB
                                             options:MTLResourceStorageModePrivate];
    require(buffer != nil && dedicatedAllocations == 1,
            "preferred dedicated allocation was not selected");
    require(lastAllocationDedicated && lastMemory &&
                lastMemory->dedicatedBuffer == [buffer vkBuffer],
            "preferred dedicated allocation did not name the exact buffer");
    require([buffer rdna4BackingSize] == 3 * kMiB,
            "preferred dedicated block was not exact");
    buffer = nil;
    heap = nil;
    require(memoryFrees == 1 && liveMemories == 0,
            "preferred dedicated allocation leaked");
    resetCounters();

    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){2 * kMiB, 64 * 1024, 1};
    forceRequirements = YES;
    requirementsRequireDedicated = YES;
    MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModePrivate);
    RDNA4Texture *texture =
        (RDNA4Texture *)[heap newTextureWithDescriptor:descriptor];
    require(texture != nil && dedicatedAllocations == 1 &&
                lastAllocationDedicated && lastMemory &&
                lastMemory->dedicatedImage == [texture vkImage],
            "required dedicated image allocation lost its exact pNext");
    texture = nil;
    heap = nil;
    require(memoryFrees == 1 && liveImages == 0 && liveMemories == 0,
            "required dedicated image leaked its backing");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS dedicated=required+preferred "
         "pNext=buffer+image validated");
}

static void testEmptyIncompatibleBlockEviction(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, 8 * kMiB, MTLStorageModeShared);

    forcedRequirements = (VkMemoryRequirements){4 * kMiB, 4096, 2};
    forceRequirements = YES;
    RDNA4Buffer *hostBlock =
        (RDNA4Buffer *)[heap newBufferWithLength:4 * kMiB
                                        options:MTLResourceStorageModeShared];
    require(hostBlock != nil && lastAllocationMemoryType == 1,
            "Shared heap buffer did not use host-visible memory");
    require(memoryAllocates == 1 && mapCalls == 1 &&
                [heap usedSize] == 4 * kMiB,
            "Shared heap buffer backing is not exact");
    hostBlock = nil;
    require(memoryFrees == 1 && [heap rdna4BackingBytes] == 0,
            "empty host block was not evicted");

    forcedRequirements = (VkMemoryRequirements){64 * 1024, 4096, 3};
    forceRequirements = YES;
    MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModeShared);
    RDNA4Texture *image = (RDNA4Texture *)[heap newTextureWithDescriptor:descriptor];
    require(image != nil, "Shared heap image failed after host block eviction");
    require(lastAllocationMemoryType == 0 && lastAllocationSize == 4 * kMiB,
            "Shared heap image did not allocate device-local memory");
    require(mapCalls == 1 && importedAllocations == 0 &&
                [image rdna4BackingOffset] == 0,
            "Shared heap image was host-mapped/imported or mis-bound");
    require(memoryAllocates == 2 && memoryFrees == 1 &&
                [heap usedSize] == 64 * 1024,
            "incompatible block did not yield exact image backing");

    image = nil;
    checkCleanBacking(heap);
    heap = nil;
    require(memoryAllocates == 2 && memoryFrees == 2 &&
                liveImages == 0 && liveMemories == 0,
            "incompatible-block case leaked Vulkan backing");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS incompatible_block=evicted image_memtype=DEVICE_LOCAL");
}

static void testFullCapacityAliasReuse(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, 64 * 1024, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 1};
    forceRequirements = YES;

    @autoreleasepool {
        NSMutableArray<RDNA4Buffer *> *aliases = [NSMutableArray new];
        uint64_t block = 0;
        for (unsigned i = 0; i < 8; i++) {
            RDNA4Buffer *buffer =
                (RDNA4Buffer *)[heap newBufferWithLength:64 * 1024
                                                 options:MTLResourceStorageModePrivate];
            require(buffer != nil, "full-capacity alias allocation failed");
            if (!i) block = [buffer rdna4BackingBlock];
            require(block != 0 && [buffer rdna4BackingBlock] == block &&
                        [buffer rdna4BackingOffset] == 0,
                    "full-capacity range was not reused at offset zero");
            require([heap usedSize] == 64 * 1024,
                    "full-capacity allocation accounting is not tight");
            [buffer makeAliasable];
            require([buffer isAliasable] && [heap usedSize] == 0 &&
                        [heap rdna4BackingInUse] == 0,
                    "full-capacity alias did not release its logical range");
            [aliases addObject:buffer];
        }
        require(bufferCreates == 8 && bindBufferCalls == 8 &&
                    memoryAllocates == 1 && memoryFrees == 0 &&
                    [heap rdna4BackingBytes] == 64 * 1024,
                "repeated aliases created extra physical blocks");

        aliases = nil;
    }
    checkCleanBacking(heap);
    heap = nil;
    require(memoryFrees == 1 && liveBuffers == 0 && liveMemories == 0,
            "full-capacity alias handles did not retire exactly once");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS full_capacity=8-alias-cycles physical_alloc=1");
}

static void testPendingCapacityUsage(GarnetDevice *device) {
    resetCounters();
    uint64_t block, serial;
    @autoreleasepool {
        RDNA4Heap *bufferHeap = makeHeap(device, 64 * 1024, MTLStorageModeShared);
        forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 2};
        forceRequirements = YES;
        RDNA4Buffer *buffer =
            (RDNA4Buffer *)[bufferHeap newBufferWithLength:64 * 1024
                                                   options:MTLResourceStorageModeShared];
        require(buffer != nil && lastAllocationMemoryType == 1,
                "full-capacity Shared buffer failed");
        block = [buffer rdna4BackingBlock];
        serial = rdna4_fence_open();
        require(serial != 0, "heap retirement timeline did not open");
        [buffer makeAliasable];
        require([bufferHeap usedSize] == 0 &&
                    [bufferHeap rdna4BackingInUse] == 64 * 1024,
                "pending buffer range was released before completion");
        require([bufferHeap maxAvailableSizeWithAlignment:256] == 0,
                "maxAvailable counted an in-flight buffer range");

        RDNA4Buffer *blocked =
            (RDNA4Buffer *)[bufferHeap newBufferWithLength:64 * 1024
                                                   options:MTLResourceStorageModeShared];
        require(blocked == nil, "in-flight full-capacity buffer was reused");
        if (bufferCreates != 2 || bufferDestroys != 1)
            failf("in-flight full-capacity buffer rollback creates=%u destroys=%u",
                  bufferCreates, bufferDestroys);
        require(memoryAllocates == 1 && memoryFrees == 0,
                "in-flight full-capacity rejection changed backing allocations");
        rdna4_fence_close(serial);
        NSUInteger available = [bufferHeap maxAvailableSizeWithAlignment:256];
        require([bufferHeap rdna4BackingInUse] == 0 &&
                    available == 64 * 1024,
                "completed buffer range did not become available");
        RDNA4Buffer *reused =
            (RDNA4Buffer *)[bufferHeap newBufferWithLength:64 * 1024
                                                   options:MTLResourceStorageModeShared];
        require(reused != nil && [reused rdna4BackingBlock] == block &&
                    [reused rdna4BackingOffset] == 0,
                "completed buffer range was not reused");
        buffer = nil;
        reused = nil;
        bufferHeap = nil;
    }
    require(memoryAllocates == 1 && memoryFrees == 1 && liveBuffers == 0,
            "buffer pending-capacity case leaked backing");
    resetCounters();

    @autoreleasepool {
        RDNA4Heap *imageHeap = makeHeap(device, 64 * 1024, MTLStorageModeShared);
        forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 3};
        forceRequirements = YES;
        MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModeShared);
        RDNA4Texture *image = (RDNA4Texture *)[imageHeap newTextureWithDescriptor:descriptor];
        require(image != nil && lastAllocationMemoryType == 0 && mapCalls == 0,
                "full-capacity Shared image did not use device-local memory");
        block = [image rdna4BackingBlock];
        serial = rdna4_fence_open();
        require(serial != 0, "image retirement timeline did not open");
        [image makeAliasable];
        require([imageHeap usedSize] == 0 &&
                    [imageHeap rdna4BackingInUse] == 64 * 1024 &&
                    [imageHeap maxAvailableSizeWithAlignment:256] == 0,
                "in-flight image range was counted as available");
        rdna4_fence_close(serial);
        require([imageHeap maxAvailableSizeWithAlignment:256] == 64 * 1024,
                "completed image range did not become available");
        RDNA4Texture *reusedImage =
            (RDNA4Texture *)[imageHeap newTextureWithDescriptor:descriptor];
        require(reusedImage != nil &&
                    [reusedImage rdna4BackingBlock] == block &&
                    [reusedImage rdna4BackingOffset] == 0 &&
                    lastAllocationMemoryType == 0,
                "completed image range was not reused");
        image = nil;
        reusedImage = nil;
        imageHeap = nil;
    }
    require(memoryAllocates == 1 && memoryFrees == 1 && liveImages == 0 &&
                liveMemories == 0,
            "image pending-capacity case leaked backing");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS pending=no-maxavail classes=buffer+image");
}

static void testImageAndViewAliasing(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, 8 * kMiB, MTLStorageModeShared);
    __weak RDNA4Texture *weakRoot;
    @autoreleasepool {
        forcedRequirements = (VkMemoryRequirements){64 * 1024, 4096, 3};
        forceRequirements = YES;
        MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModeShared);
        RDNA4Texture *root = (RDNA4Texture *)[heap newTextureWithDescriptor:descriptor];
        RDNA4Texture *next = (RDNA4Texture *)[heap newTextureWithDescriptor:descriptor];
        require(root != nil && next != nil, "Shared heap images failed");
        require(lastAllocationMemoryType == 0 && mapCalls == 0,
                "Shared heap images did not use device-local memory");
        require([root rdna4BackingBlock] == [next rdna4BackingBlock] &&
                    [next rdna4BackingOffset] == 64 * 1024 &&
                    lastImageBindOffset == 64 * 1024,
                "compatible heap images did not bind at aligned nonzero offsets");
        require(![root doesAliasResource:next] && ![next doesAliasResource:root],
                "disjoint ranges in one heap alias each other");

        RDNA4Texture *view =
            (RDNA4Texture *)[root newTextureViewWithPixelFormat:[(id)root pixelFormat]];
        require(view != nil, "heap texture view creation failed");
        require((RDNA4Heap *)[(id)view heap] == heap &&
                    [view heapOffset] == 0,
                "heap texture view does not expose its root heap");
        require([view rdna4BackingBlock] == [root rdna4BackingBlock] &&
                    [view rdna4BackingOffset] == [root rdna4BackingOffset] &&
                    [view rdna4BackingSize] == [root rdna4BackingSize] &&
                    [view allocatedSize] == [root allocatedSize],
                "heap texture view lost root backing identity");
        require([root doesAliasResource:view] && [view doesAliasResource:root] &&
                    [view rootResource] == root,
                "heap texture view/root alias metadata is inconsistent");

        NSUInteger used = [heap usedSize];
        [view makeAliasable];
        require([heap usedSize] == used && ![view isAliasable] &&
                    ![root isAliasable] && ![[view rootResource] isAliasable],
                "makeAliasable on a heap texture view released its root");

        weakRoot = root;
        root = nil;
        require(weakRoot != nil && [view doesAliasResource:weakRoot],
                "heap texture view did not retain its root");
        [weakRoot makeAliasable];
        require([weakRoot isAliasable] && [view isAliasable] &&
                    [heap usedSize] == 64 * 1024,
                "view isAliasable did not delegate to the root");
        view = nil;
        next = nil;
    }
    require(weakRoot == nil, "heap view outlived or retained the root twice");
    checkCleanBacking(heap);
    heap = nil;
    require(memoryAllocates == 1 && memoryFrees == 1 &&
                liveImages == 0 && liveViews == 0 && liveMemories == 0,
            "heap image/view lifecycle leaked handles");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS image_alias=metadata view=root-retained-no-release");
}

static void testCrossHeapAliasing(GarnetDevice *device) {
    resetCounters();
    @autoreleasepool {
        RDNA4Heap *leftHeap = makeHeap(device, 64 * 1024, MTLStorageModePrivate);
        RDNA4Heap *rightHeap = makeHeap(device, 64 * 1024, MTLStorageModePrivate);
        forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 1};
        forceRequirements = YES;
        RDNA4Buffer *left =
            (RDNA4Buffer *)[leftHeap newBufferWithLength:64 * 1024
                                                options:MTLResourceStorageModePrivate];
        RDNA4Buffer *right =
            (RDNA4Buffer *)[rightHeap newBufferWithLength:64 * 1024
                                                 options:MTLResourceStorageModePrivate];
        require(left && right && [left rdna4BackingBlock] == [right rdna4BackingBlock] &&
                    [left rdna4BackingOffset] == [right rdna4BackingOffset],
                "cross-heap metadata did not coincide for the alias check");
        require(![left doesAliasResource:right] && ![right doesAliasResource:left],
                "cross-heap resources with equal block ids alias each other");
        left = nil;
        right = nil;
        leftHeap = nil;
        rightHeap = nil;
        require(memoryAllocates == 2 && memoryFrees == 2 &&
                    liveBuffers == 0 && liveMemories == 0,
                "cross-heap alias case leaked backing");
    }
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS cross_heap_alias=no");
}

static void testTimelineOOMRollbackAndRetry(GarnetDevice *device) {
    (void)device;
    resetCounters();
    RDNA4P.on = YES;
    rdna4p_init(&RDNA4P.p, 1);
    rdna4p_init(&RDNA4HT.p, 1);
    RDNA4HT.ready = YES;

    // Give only the heap timeline spare pending capacity. That lets it acquire
    // the speculative serial while the pool's first allocation fails.
    uint64_t capacityToken = rdna4p_submit(&RDNA4HT.p);
    require(capacityToken == 1 &&
                rdna4p_cancel_last(&RDNA4HT.p, capacityToken) &&
                RDNA4HT.p.submitted == 0 &&
                RDNA4HT.p.completed == 0 &&
                RDNA4HT.p.npend == 0 && RDNA4HT.p.cpend > 0 &&
                RDNA4P.p.cpend == 0,
            "could not prepare a clean pool-side OOM");

    refuseProductionRealloc = 1;
    uint64_t serial = rdna4_fence_open();
    refuseProductionRealloc = 0;
    require(serial == 0, "timeline OOM published a serial");
    require(RDNA4HT.p.submitted == 0 &&
                RDNA4HT.p.completed == 0 &&
                RDNA4HT.p.npend == 0 &&
                RDNA4P.p.submitted == 0 &&
                RDNA4P.p.completed == 0 &&
                RDNA4P.p.npend == 0 && RDNA4P.p.cpend == 0,
            "pool-side OOM did not roll back the speculative heap token");

    serial = rdna4_fence_open();
    require(serial == 1 && RDNA4HT.p.submitted == 1 &&
                RDNA4P.p.submitted == 1 && RDNA4HT.p.npend == 1 &&
                RDNA4P.p.npend == 1 && RDNA4HT.p.completed == 0 &&
                RDNA4P.p.completed == 0,
            "retry after pool-side OOM did not publish matching serial 1");
    rdna4_fence_close(serial);
    require(RDNA4HT.p.submitted == 1 && RDNA4HT.p.completed == 1 &&
                RDNA4HT.p.npend == 0 && RDNA4P.p.submitted == 1 &&
                RDNA4P.p.completed == 1 && RDNA4P.p.npend == 0,
            "retry token did not retire from both timelines exactly once");

    rdna4p_destroy(&RDNA4P.p);
    rdna4p_init(&RDNA4P.p, 0);
    RDNA4P.on = NO;
    rdna4p_destroy(&RDNA4HT.p);
    rdna4p_init(&RDNA4HT.p, 1);
    RDNA4HT.ready = NO;
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS timeline_oom=pool-side-rollback "
         "retry=serial-1 both=retired");
}

static void testRetirementReuseWithPoolOff(GarnetDevice *device) {
    resetCounters();
    require(RDNA4P.on == NO, "test setup enabled the allocation pool");
    @autoreleasepool {
        RDNA4Heap *heap = makeHeap(device, 8 * kMiB, MTLStorageModePrivate);
        forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 1};
        forceRequirements = YES;

        RDNA4Buffer *first = (RDNA4Buffer *)[heap newBufferWithLength:64 * 1024
                                                            options:MTLResourceStorageModePrivate];
        require(first && [first rdna4BackingOffset] == 0,
                "first retirement buffer did not bind at zero");
        uint64_t serial = rdna4_fence_open();
        require(serial != 0 && RDNA4HT.p.submitted == serial &&
                RDNA4HT.p.completed != serial && RDNA4P.p.submitted == 0,
                "RDNA4HT did not take the retirement serial with the pool OFF");
        [first makeAliasable];

        RDNA4Buffer *second = (RDNA4Buffer *)[heap newBufferWithLength:64 * 1024
                                                             options:MTLResourceStorageModePrivate];
        require(second != nil, "second retirement buffer failed");
        require([second rdna4BackingBlock] == [first rdna4BackingBlock] &&
                [second rdna4BackingOffset] == 64 * 1024,
                "physical range was reused before its retirement serial closed");
        rdna4_fence_close(serial);
        require(RDNA4HT.p.completed == serial,
                "RDNA4HT completion did not advance with the pool OFF");

        RDNA4Buffer *third = (RDNA4Buffer *)[heap newBufferWithLength:64 * 1024
                                                            options:MTLResourceStorageModePrivate];
        require(third && [third rdna4BackingBlock] == [first rdna4BackingBlock] &&
                [third rdna4BackingOffset] == 0,
                "completed retirement did not release the lowest physical range");
        require([heap maxAvailableSizeWithAlignment:256] > 0,
                "logical heap allocator became unusable");

        first = nil;
        second = nil;
        third = nil;
        heap = nil;
    }
    require(memoryFrees == 1 && liveMemories == 0 && liveBuffers == 0,
            "retirement case leaked backing or handles");
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS pool=off reuse=after-RDNA4HT-serial");
}

static void testRollback(GarnetDevice *device) {
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    failurePoint = FailCreateBuffer;
    id buffer = [heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    require(buffer == nil && bufferCreates == 1 && bufferDestroys == 0 &&
                memoryAllocates == 0 && liveBuffers == 0,
            "buffer-create failure was not returned cleanly");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    MTLTextureDescriptor *descriptor = textureDescriptor(MTLStorageModePrivate);
    failurePoint = FailCreateImage;
    id texture = [heap newTextureWithDescriptor:descriptor];
    require(texture == nil && imageCreates == 1 && imageDestroys == 0 &&
                memoryAllocates == 0 && liveImages == 0,
            "image-create failure was not returned cleanly");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    forcedRequirements = (VkMemoryRequirements){kHeapSize + 1, 256, 1};
    forceRequirements = YES;
    buffer = [heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    require(buffer == nil, "logical reservation failure was accepted");
    require(bufferCreates == 1 && bufferDestroys == 1 &&
            memoryAllocates == 0 && liveBuffers == 0,
            "logical reservation rollback leaked a buffer or allocated memory");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    forcedRequirements = (VkMemoryRequirements){4096, 256, 1};
    forceRequirements = YES;
    failurePoint = FailAllocate;
    buffer = [heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    require(buffer == nil, "allocation failure was accepted");
    require(memoryAllocates == 1 && memoryFrees == 0 &&
            bufferCreates == 1 && bufferDestroys == 1 &&
            liveMemories == 0 && liveBuffers == 0,
            "allocation rollback freed the wrong objects");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    failurePoint = FailBindBuffer;
    buffer = [heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    require(buffer == nil, "buffer bind failure was accepted");
    require(bindBufferCalls == 1 && memoryAllocates == 1 && memoryFrees == 1 &&
            bufferCreates == 1 && bufferDestroys == 1 &&
            liveMemories == 0 && liveBuffers == 0,
            "buffer bind rollback leaked or double-freed backing");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    forcedRequirements = (VkMemoryRequirements){64 * 1024, 256, 1};
    forceRequirements = YES;
    failurePoint = FailBindImage;
    texture = [heap newTextureWithDescriptor:descriptor];
    require(texture == nil, "image bind failure was accepted");
    require(bindImageCalls == 1 && memoryAllocates == 1 && memoryFrees == 1 &&
                imageCreates == 1 && imageDestroys == 1 &&
                viewCreates == 0 && liveImages == 0 && liveMemories == 0,
            "image bind rollback leaked or double-freed backing");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, kHeapSize, MTLStorageModePrivate);

    failurePoint = FailCreateView;
    texture = [heap newTextureWithDescriptor:descriptor];
    require(texture == nil, "image-view failure was accepted");
    require(bindImageCalls == 1 && viewCreates == 1 && viewDestroys == 0 &&
            memoryAllocates == 1 && memoryFrees == 1 &&
            imageCreates == 1 && imageDestroys == 1 &&
            liveViews == 0 && liveImages == 0 && liveMemories == 0,
            "image-view rollback leaked an image, view or memory");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS rollback=create/reserve/alloc/bind-buffer/"
         "bind-image/view exact");
}

static void checkLargeHostImport(GarnetDevice *device, MTLStorageMode storage,
                                 const char *name) {
    resetCounters();
    const NSUInteger length = (64 * kMiB) + 4097;
    MTLResourceOptions options =
        (MTLResourceOptions)((NSUInteger)storage << MTLResourceStorageModeShift);
    VkMemoryRequirements hostRequirements = {length, 4096, 2};
    forcedRequirements = hostRequirements;
    forceRequirements = YES;
    MTLSizeAndAlign sizing = [device heapBufferSizeAndAlignWithLength:length
                                                              options:options];
    require(sizing.size > length && sizing.align == RDNA4R.hostAlign,
            "large host-backed query did not return the aligned import extent");
    require(bufferCreates == 1 && bufferDestroys == 1 &&
            memoryAllocates == 0 && importedAllocations == 0 &&
            hostMaps == 0 && hostPropertyCalls == 0,
            "large host-backed query allocated backing");
    uint64_t expectedSize = alignUp(alignUp(length, RDNA4R.lim.bufferImageGranularity),
                                    RDNA4R.hostAlign);
    require(sizing.size == expectedSize,
            "large host-backed query extent disagrees with constructor placement");
    resetCounters();

    forcedRequirements = hostRequirements;
    forceRequirements = YES;
    RDNA4Heap *heap = makeHeap(device, (NSUInteger)expectedSize, storage);
    RDNA4Buffer *buffer = (RDNA4Buffer *)[heap newBufferWithLength:length
                                                          options:options];
    require(buffer != nil, "large host-backed heap buffer failed");
    require(buffer.contents != NULL &&
            ((uintptr_t)buffer.contents & (RDNA4R.hostAlign - 1)) == 0,
            "large host-backed heap buffer lost its aligned owned mapping");
    require(!buffer.rdna4Imported && importedAllocations == 1 &&
                hostPropertyCalls == 1 && hostMaps > 0,
            "heap-owned import was mislabeled as client NoCopy or skipped import");
    require(lastImportedHost != NULL &&
            ((uintptr_t)lastImportedHost & (RDNA4R.hostAlign - 1)) == 0 &&
                RDNA4R.impTotal == expectedSize &&
                lastAllocationSize == expectedSize &&
                lastAllocationMemoryType == 1 && lastMemory &&
                lastMemory->imported,
            "heap-owned import pointer, type or accounting is invalid");
    require(mapCalls == 0 && bindBufferCalls == 1 && lastBufferBindOffset == 0,
            "owned host import was mapped again or not bound");

    buffer = nil;
    heap = nil;
    require(liveBuffers == 0 && liveMemories == 0 && liveMappings == 0 &&
            memoryFrees == 1 && hostUnmaps > 0 &&
            hostMappedBytes == hostUnmappedBytes && hostLiveBytes == 0 &&
            RDNA4R.impTotal == 0 && atomic_load(&rdna4_alloc_total) == 0,
            "large host import did not unmap/free/account exactly once");
    resetCounters();
    printf("HEAP_EXECUTION_CASE_PASS large_%s=64MiB+ external_host=1 "
           "mapping=balanced imported_bit=NO\n",
           name);
}

static void testLargeHostImportFailures(GarnetDevice *device) {
    const NSUInteger length = (64 * kMiB) + 4097;
    uint64_t expectedSize = alignUp(alignUp(length, RDNA4R.lim.bufferImageGranularity),
                                    RDNA4R.hostAlign);
    VkMemoryRequirements hostRequirements = {length, 4096, 2};
    resetCounters();
    RDNA4Heap *heap = makeHeap(device, (NSUInteger)expectedSize,
                               MTLStorageModeShared);
    forcedRequirements = hostRequirements;
    forceRequirements = YES;

    failurePoint = FailHostProperties;
    RDNA4Buffer *buffer =
        (RDNA4Buffer *)[heap newBufferWithLength:length
                                         options:MTLResourceStorageModeShared];
    require(buffer == nil && hostPropertyCalls == 1 && memoryAllocates == 0 &&
                bufferCreates == 1 && bufferDestroys == 1 &&
                hostMaps > 0 && hostMappedBytes == hostUnmappedBytes &&
                hostLiveBytes == 0 && RDNA4R.impTotal == 0,
            "host-property failure leaked the owned mapping or handles");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    heap = makeHeap(device, (NSUInteger)expectedSize, MTLStorageModeShared);
    forcedRequirements = hostRequirements;
    forceRequirements = YES;

    failurePoint = FailAllocate;
    buffer = (RDNA4Buffer *)[heap newBufferWithLength:length
                                             options:MTLResourceStorageModeShared];
    require(buffer == nil && hostPropertyCalls == 1 && memoryAllocates == 1 &&
                memoryFrees == 0 && bufferCreates == 1 && bufferDestroys == 1 &&
                hostMaps > 0 && hostMappedBytes == hostUnmappedBytes &&
                hostLiveBytes == 0 && RDNA4R.impTotal == 0,
            "host-import allocation failure leaked mapping or handles");
    checkCleanBacking(heap);
    heap = nil;
    resetCounters();
    puts("HEAP_EXECUTION_CASE_PASS host_import_failures=properties+allocate clean");
}

static RDNA4Heap *quarantineHeap;

static void testLargePrivateHeapInitialization(GarnetDevice *device) {
    resetCounters();
    const NSUInteger length = (NSUInteger)RDNA4_BUFFER_LEGACY_MAX + 5;
    const uint64_t range = alignUp(length, 64 * 1024);
    const uint64_t savedBudget = RDNA4Caps.allocation_budget;
    const uint64_t savedWorkingSet = RDNA4Caps.working_set;
    RDNA4Caps.allocation_budget = UINT64_C(8) << 30;
    RDNA4Caps.working_set = UINT64_C(6) << 30;
    forcedRequirements = (VkMemoryRequirements){length, 4096, 1};
    forceRequirements = YES;
    forceEdgeRequirements = YES;

    RDNA4Heap *heap = makeHeap(device, (NSUInteger)range, MTLStorageModePrivate);
    RDNA4Buffer *buffer =
        (RDNA4Buffer *)[heap newBufferWithLength:length
                                        options:MTLResourceStorageModePrivate];
    require(buffer != nil && buffer.contents == NULL &&
                [buffer allocatedSize] == length &&
                [buffer rdna4BackingSize] == range,
            "large Private heap buffer did not expose tight logical/range bytes");
    HeapTestBuffer *handle = (HeapTestBuffer *)[buffer vkBuffer];
    require(handle->memory && handle->memory->memoryType == 0 &&
                handle->memory->size == range,
            "large Private heap buffer did not use its exact device-local block");
    require(fillCommands == 1 && lastFillSize == (length & ~(NSUInteger)3) &&
                lastFillOffset == 0 &&
                copyCommands == 1 && lastCopySize == 1 &&
                lastCopySourceOffset == 0 &&
                lastCopyDestinationOffset == (length & ~(NSUInteger)3) &&
                barrierCommands == 1 && queueSubmits == 1 && fenceWaits == 1 &&
                commandAllocates == 1 && commandFrees == 1 &&
                fenceCreates == 1 && fenceDestroys == 1,
            "large Private heap zero-fill/tail/completion is incomplete");
    buffer = nil;
    heap = nil;
    require(bufferCreates == 2 && bufferDestroys == 2 &&
                memoryAllocates == 2 && memoryFrees == 2 &&
                liveBuffers == 0 && liveMemories == 0 &&
                atomic_load(&rdna4_alloc_total) == 0,
            "successful large Private heap initialization leaked backing");
    resetCounters();

    heap = makeHeap(device, (NSUInteger)range, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){length, 4096, 1};
    forceRequirements = YES;
    forceEdgeRequirements = YES;
    failurePoint = FailCreateFence;
    buffer = (RDNA4Buffer *)[heap newBufferWithLength:length
                                             options:MTLResourceStorageModePrivate];
    require(buffer == nil && fillCommands == 1 && copyCommands == 1 &&
                barrierCommands == 1 && queueSubmits == 0 && fenceWaits == 0 &&
                commandAllocates == 1 && commandFrees == 1 &&
                fenceCreates == 1 && fenceDestroys == 0 &&
                rdna4_unresolved_commands.count == 0 &&
                atomic_load(&rdna4_submit_fault) == VK_SUCCESS,
            "pre-submit large Private failure did not roll back");
    checkCleanBacking(heap);
    heap = nil;
    require(bufferCreates == 2 && bufferDestroys == 2 &&
                memoryAllocates == 2 && memoryFrees == 2 &&
                liveBuffers == 0 && liveMemories == 0 &&
                atomic_load(&rdna4_alloc_total) == 0,
            "pre-submit large Private failure retained handles or backing");
    resetCounters();

    heap = makeHeap(device, (NSUInteger)range, MTLStorageModePrivate);
    forcedRequirements = (VkMemoryRequirements){length, 4096, 1};
    forceRequirements = YES;
    forceEdgeRequirements = YES;
    failurePoint = FailQueueSubmit;
    buffer = (RDNA4Buffer *)[heap newBufferWithLength:length
                                             options:MTLResourceStorageModePrivate];
    require(buffer == nil && queueSubmits == 1 && fenceWaits == 0 &&
                commandAllocates == 1 && commandFrees == 0 &&
                fenceCreates == 1 && fenceDestroys == 0 &&
                liveBuffers == 2 && liveCommands == 1 && liveFences == 1 &&
                liveMemories == 2 && liveMappings == 1 &&
                rdna4_unresolved_commands.count == 1 &&
                atomic_load(&rdna4_submit_fault) == VK_ERROR_DEVICE_LOST &&
                [heap usedSize] == length &&
                [heap rdna4BackingBytes] == range &&
                [heap rdna4BackingInUse] == range,
            "unresolved large Private submit did not quarantine exact backing");
    quarantineHeap = heap;
    RDNA4Caps.allocation_budget = savedBudget;
    RDNA4Caps.working_set = savedWorkingSet;
    puts("HEAP_EXECUTION_CASE_PASS large_private=4GiB+ fill=1 tail=1 "
         "presubmit=rollback unresolved=quarantined");
}

static GarnetDevice *sharedTestDevice(void) {
    // GarnetDevice's initWithAcceleratorPort requires a live accelerator nub. The
    // production selectors used here never touch device ivars while capabilities
    // are published, so keep one allocated proxy alive for the process lifetime.
    static GarnetDevice *device;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ device = [GarnetDevice alloc]; });
    return device;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            puts("Usage: test-heap-execution\n"
                 "Runs production RDNA4Heap buffer/texture construction, requirements2 sizing, "
                 "dedicated/import backing, retirement reuse and rollback against simulated Vulkan.");
            return 0;
        }
        if (argc != 1) return 2;
        configureVulkan();
        GarnetDevice *device = sharedTestDevice();
        testCapacityQueriesStayCold(device);
        testExactSizeAndAlign(device);
        testRequiredSizingRejectsHeapResources(device);
        testMemoryTypesAndOffsets(device);
        testDedicatedRequirements(device);
        testTimelineOOMRollbackAndRetry(device);
        testEmptyIncompatibleBlockEviction(device);
        testFullCapacityAliasReuse(device);
        testPendingCapacityUsage(device);
        testImageAndViewAliasing(device);
        testCrossHeapAliasing(device);
        testRetirementReuseWithPoolOff(device);
        testRollback(device);
        checkLargeHostImport(device, MTLStorageModeShared, "shared");
        checkLargeHostImport(device, MTLStorageModeManaged, "managed");
        testLargeHostImportFailures(device);
        testLargePrivateHeapInitialization(device);
        require(RDNA4P.on == NO && legacyBufferQueries == 0 &&
                legacyImageQueries == 0,
                "final heap state used the pool or legacy requirements fallback");
        puts("HEAP_EXECUTION_PASS production=heap/buffer/texture "
             "requirements2=exact dedicated=required+preferred "
             "external_host=large-shared+managed aliases=full+pending+views "
             "retirement=RDNA4HT failures=exact quarantine=retained "
             "simulated_vulkan=1 gpu_execution=0");
    }
    return 0;
}
