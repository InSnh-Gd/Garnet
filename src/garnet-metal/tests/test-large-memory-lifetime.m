// Large logical-resource lifetime/process-fault checks. This includes the
// production Objective-C driver but replaces Vulkan with bounded test doubles.
// It does not allocate large pinned backing or execute GPU work.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic uint64_t hostMapCalls;
static _Atomic uint64_t hostUnmapCalls;
static _Atomic uint64_t hostMappedBytes;
static _Atomic uint64_t hostUnmappedBytes;
static _Atomic uint64_t hostLiveMapBytes;
static _Atomic int refuseHostMap;

static void *lifetimeMmap(void *address, size_t size, int prot, int flags, int fd,
                          off_t offset) {
    if (atomic_load(&refuseHostMap)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    void *mapping = mmap(address, size, prot, flags, fd, offset);
    if (mapping != MAP_FAILED) {
        atomic_fetch_add(&hostMapCalls, 1);
        atomic_fetch_add(&hostMappedBytes, size);
        atomic_fetch_add(&hostLiveMapBytes, size);
    }
    return mapping;
}

static int lifetimeMunmap(void *address, size_t size) {
    int result = munmap(address, size);
    if (result == 0) {
        atomic_fetch_add(&hostUnmapCalls, 1);
        atomic_fetch_add(&hostUnmappedBytes, size);
        atomic_fetch_sub(&hostLiveMapBytes, size);
    }
    return result;
}

#define mmap lifetimeMmap
#define munmap lifetimeMunmap
#include "GarnetDevice.m"
#undef mmap
#undef munmap

enum {
    kCycleCount = 3,
    kFailureRounds = 24,
    kSuccessRounds = 16,
    kFaultExitCode = 75,
    kInvalidExitCode = 2,
};

static const uint64_t kBoundaryBytes = UINT64_C(64) << 20;
static const uint64_t kLargeLength = (UINT64_C(64) << 20) + (UINT64_C(64) << 10);
static const uint64_t kFillOffset = (UINT64_C(64) << 20) - 128;
static const uint64_t kFillBytes = 256;
static const uint64_t kHostAlignment = UINT64_C(64) << 10;

typedef enum {
    RunLifecycle,
    RunAllocationStress,
    RunHang,
    RunPoison,
} RunMode;

typedef enum {
    FailNone,
    FailCreate,
    FailProperties,
    FailAllocate,
    FailBind,
    FailAddress,
} FailurePoint;

enum {
    TestBufferMagic = 0x4c425546,
    TestMemoryMagic = 0x4c4d454d,
    TestCommandMagic = 0x4c434d44,
    TestFenceMagic = 0x4c464e43,
    TestPoolMagic = 0x4c504f4f,
};

typedef struct {
    uint32_t magic;
    uint64_t size;
    VkDeviceMemory memory;
} LifetimeTestBuffer;

typedef struct {
    uint32_t magic;
    uint64_t size;
    void *cpu;
    BOOL imported;
    const void *hostPointer;
} LifetimeTestMemory;

typedef struct {
    uint32_t magic;
} LifetimeTestCommand;

typedef struct {
    uint32_t magic;
} LifetimeTestFence;

typedef struct {
    uint32_t magic;
} LifetimeTestPool;

static _Atomic unsigned bufferCreates;
static _Atomic unsigned bufferDestroys;
static _Atomic unsigned memoryAllocates;
static _Atomic unsigned memoryFrees;
static _Atomic unsigned commandPoolCreates;
static _Atomic unsigned commandPoolDestroys;
static _Atomic unsigned commandAllocates;
static _Atomic unsigned commandFrees;
static _Atomic unsigned fenceCreates;
static _Atomic unsigned fenceDestroys;
static _Atomic unsigned submits;
static _Atomic unsigned fenceWaits;
static _Atomic unsigned fillCommands;
static _Atomic uint64_t lastFillOffset;
static _Atomic uint64_t lastFillSize;
static _Atomic int injectedFailure;
static _Atomic int runtimeMode;

static dispatch_semaphore_t fenceEntered;
static dispatch_semaphore_t fenceRelease;
static dispatch_semaphore_t completionSeen;
static dispatch_semaphore_t rejectedSeen;
static _Atomic unsigned completionCallbacks;
static _Atomic unsigned rejectedCallbacks;
static _Atomic int completionStatus;
static _Atomic int completionHadError;
static _Atomic int completionSawLiveBuffer;
static _Atomic int rejectedHadError;
static __weak RDNA4Buffer *weakLargeBuffer;
static __weak RDNA4CommandBuffer *weakQuarantinedCommand;

static void failf(const char *format, ...) __attribute__((noreturn));
static void failf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("LARGE_MEMORY_LIFETIME_FAIL ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    exit(1);
}

static void require(BOOL condition, const char *message) {
    if (!condition) failf("%s", message);
}

static VkResult VKAPI_CALL testCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkBuffer *buffer) {
    (void)device;
    (void)allocator;
    require(info != NULL && buffer != NULL && info->size > 0, "invalid vkCreateBuffer request");
    if (atomic_load(&injectedFailure) == FailCreate) return VK_ERROR_OUT_OF_HOST_MEMORY;
    LifetimeTestBuffer *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkBuffer");
    created->magic = TestBufferMagic;
    created->size = info->size;
    atomic_fetch_add(&bufferCreates, 1);
    *buffer = (VkBuffer)created;
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyBuffer(VkDevice device, VkBuffer buffer,
                                          const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    LifetimeTestBuffer *destroyed = (LifetimeTestBuffer *)buffer;
    require(destroyed != NULL && destroyed->magic == TestBufferMagic,
            "double or foreign vkDestroyBuffer");
    destroyed->magic = 0;
    free(destroyed);
    atomic_fetch_add(&bufferDestroys, 1);
}

static void VKAPI_CALL testBufferRequirements(VkDevice device, VkBuffer buffer,
                                               VkMemoryRequirements *requirements) {
    (void)device;
    LifetimeTestBuffer *tested = (LifetimeTestBuffer *)buffer;
    require(tested != NULL && tested->magic == TestBufferMagic && requirements != NULL,
            "invalid buffer requirements request");
    uint64_t rounded = (tested->size + UINT64_C(4095)) & ~UINT64_C(4095);
    *requirements = (VkMemoryRequirements){
        .size = rounded,
        .alignment = 4096,
        .memoryTypeBits = 1,
    };
}

static VkResult VKAPI_CALL testHostPointerProperties(
    VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
    const void *pointer, VkMemoryHostPointerPropertiesEXT *properties) {
    (void)device;
    require(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT &&
            pointer != NULL && properties != NULL,
            "invalid host-import properties request");
    require(((uintptr_t)pointer & (kHostAlignment - 1)) == 0,
            "owned host import is not aligned");
    if (atomic_load(&injectedFailure) == FailProperties)
        return VK_ERROR_MEMORY_MAP_FAILED;
    properties->memoryTypeBits = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testAllocateMemory(VkDevice device,
                                               const VkMemoryAllocateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkDeviceMemory *memory) {
    (void)device;
    (void)allocator;
    require(info != NULL && memory != NULL && info->allocationSize > 0,
            "invalid vkAllocateMemory request");
    const VkMemoryAllocateFlagsInfo *flags =
        (const VkMemoryAllocateFlagsInfo *)info->pNext;
    const VkImportMemoryHostPointerInfoEXT *imported = NULL;
    if (flags && flags->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)
        imported = (const VkImportMemoryHostPointerInfoEXT *)flags->pNext;
    if (imported) {
        require(imported->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT &&
                imported->pHostPointer != NULL,
                "invalid host-pointer import allocation");
    }
    if (atomic_load(&injectedFailure) == FailAllocate)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    LifetimeTestMemory *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test VkDeviceMemory");
    created->magic = TestMemoryMagic;
    created->size = info->allocationSize;
    if (imported) {
        created->imported = YES;
        created->hostPointer = imported->pHostPointer;
    } else {
        if (posix_memalign(&created->cpu, 4096, (size_t)info->allocationSize) != 0)
            failf("cannot allocate test host mapping");
        memset(created->cpu, 0, (size_t)info->allocationSize);
    }
    atomic_fetch_add(&memoryAllocates, 1);
    *memory = (VkDeviceMemory)created;
    return VK_SUCCESS;
}

static void VKAPI_CALL testFreeMemory(VkDevice device, VkDeviceMemory memory,
                                       const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    LifetimeTestMemory *freed = (LifetimeTestMemory *)memory;
    require(freed != NULL && freed->magic == TestMemoryMagic,
            "double or foreign vkFreeMemory");
    if (freed->imported) {
        require(atomic_load(&hostLiveMapBytes) > 0,
                "owned host mapping was removed before Vulkan memory release");
        volatile uint8_t first = *(const uint8_t *)freed->hostPointer;
        (void)first;
    }
    freed->magic = 0;
    free(freed->cpu);
    free(freed);
    atomic_fetch_add(&memoryFrees, 1);
}

static VkResult VKAPI_CALL testBindMemory(VkDevice device, VkBuffer buffer,
                                           VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device;
    LifetimeTestBuffer *tested = (LifetimeTestBuffer *)buffer;
    LifetimeTestMemory *bound = (LifetimeTestMemory *)memory;
    require(tested != NULL && tested->magic == TestBufferMagic &&
            bound != NULL && bound->magic == TestMemoryMagic && offset == 0,
            "invalid vkBindBufferMemory request");
    if (atomic_load(&injectedFailure) == FailBind)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    tested->memory = memory;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testMapMemory(VkDevice device, VkDeviceMemory memory,
                                          VkDeviceSize offset, VkDeviceSize size,
                                          VkMemoryMapFlags flags, void **pointer) {
    (void)device;
    (void)offset;
    (void)size;
    (void)flags;
    LifetimeTestMemory *mapped = (LifetimeTestMemory *)memory;
    require(mapped != NULL && mapped->magic == TestMemoryMagic &&
            mapped->cpu != NULL && pointer != NULL,
            "invalid vkMapMemory request");
    *pointer = mapped->cpu;
    return VK_SUCCESS;
}

static VkDeviceAddress VKAPI_CALL testBufferAddress(
    VkDevice device, const VkBufferDeviceAddressInfo *info) {
    (void)device;
    (void)info;
    return atomic_load(&injectedFailure) == FailAddress
        ? 0 : UINT64_C(0x1800000000);
}

static VkResult VKAPI_CALL testCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *pool) {
    (void)device;
    (void)info;
    (void)allocator;
    LifetimeTestPool *created = calloc(1, sizeof *created);
    require(created != NULL && pool != NULL, "cannot allocate test command pool");
    created->magic = TestPoolMagic;
    *pool = (VkCommandPool)created;
    atomic_fetch_add(&commandPoolCreates, 1);
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyCommandPool(
    VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    LifetimeTestPool *destroyed = (LifetimeTestPool *)pool;
    require(destroyed != NULL && destroyed->magic == TestPoolMagic,
            "double or foreign vkDestroyCommandPool");
    destroyed->magic = 0;
    free(destroyed);
    atomic_fetch_add(&commandPoolDestroys, 1);
}

static VkResult VKAPI_CALL testAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *commands) {
    (void)device;
    require(info != NULL && info->commandBufferCount == 1 && commands != NULL,
            "invalid command-buffer allocation request");
    LifetimeTestCommand *created = calloc(1, sizeof *created);
    require(created != NULL, "cannot allocate test command buffer");
    created->magic = TestCommandMagic;
    commands[0] = (VkCommandBuffer)created;
    atomic_fetch_add(&commandAllocates, 1);
    return VK_SUCCESS;
}

static void VKAPI_CALL testFreeCommandBuffers(
    VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *commands) {
    (void)device;
    (void)pool;
    require(commands != NULL, "invalid command-buffer free request");
    for (uint32_t i = 0; i < count; ++i) {
        LifetimeTestCommand *freed = (LifetimeTestCommand *)commands[i];
        require(freed != NULL && freed->magic == TestCommandMagic,
                "double or foreign vkFreeCommandBuffers");
        freed->magic = 0;
        free(freed);
        atomic_fetch_add(&commandFrees, 1);
    }
}

static VkResult VKAPI_CALL testBeginCommandBuffer(
    VkCommandBuffer command, const VkCommandBufferBeginInfo *info) {
    (void)command;
    (void)info;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testEndCommandBuffer(VkCommandBuffer command) {
    (void)command;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testCreateFence(
    VkDevice device, const VkFenceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device;
    (void)info;
    (void)allocator;
    LifetimeTestFence *created = calloc(1, sizeof *created);
    require(created != NULL && fence != NULL, "cannot allocate test fence");
    created->magic = TestFenceMagic;
    *fence = (VkFence)created;
    atomic_fetch_add(&fenceCreates, 1);
    return VK_SUCCESS;
}

static void VKAPI_CALL testDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    LifetimeTestFence *destroyed = (LifetimeTestFence *)fence;
    require(destroyed != NULL && destroyed->magic == TestFenceMagic,
            "double or foreign vkDestroyFence");
    destroyed->magic = 0;
    free(destroyed);
    atomic_fetch_add(&fenceDestroys, 1);
}

static VkResult VKAPI_CALL testQueueSubmit(
    VkQueue queue, uint32_t count, const VkSubmitInfo *submitInfos, VkFence fence) {
    (void)queue;
    (void)fence;
    require(submitInfos != NULL && count == 1 &&
            submitInfos->commandBufferCount == 1,
            "invalid vkQueueSubmit request");
    atomic_fetch_add(&submits, 1);
    if (atomic_load(&runtimeMode) == RunPoison)
        return VK_ERROR_DEVICE_LOST;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL testWaitForFences(
    VkDevice device, uint32_t count, const VkFence *fences,
    VkBool32 waitAll, uint64_t timeout) {
    (void)device;
    require(count == 1 && fences != NULL && waitAll == VK_TRUE && timeout > 0,
            "invalid vkWaitForFences request");
    unsigned waitIndex = atomic_fetch_add(&fenceWaits, 1);
    RunMode mode = (RunMode)atomic_load(&runtimeMode);
    if (mode == RunHang) {
        require(waitIndex == 0 ? timeout == UINT64_C(1000000)
                              : timeout == UINT64_C(2000000000),
                "hang fence wait is not the production bounded slice");
        return VK_TIMEOUT;
    }
    if (mode == RunLifecycle) {
        require(timeout == UINT64_C(2000000000),
                "normal fence wait is not bounded");
        dispatch_semaphore_signal(fenceEntered);
        if (dispatch_semaphore_wait(fenceRelease,
                                    dispatch_time(DISPATCH_TIME_NOW,
                                                  INT64_C(5) * NSEC_PER_SEC)) != 0) {
            return VK_ERROR_DEVICE_LOST;
        }
        return VK_SUCCESS;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL testCmdFillBuffer(
    VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset,
    VkDeviceSize size, uint32_t value) {
    (void)command;
    (void)value;
    LifetimeTestBuffer *tested = (LifetimeTestBuffer *)buffer;
    require(tested != NULL && tested->magic == TestBufferMagic,
            "fill reached a foreign VkBuffer");
    require((offset & 3) == 0 && (size & 3) == 0 &&
            offset <= tested->size && size <= tested->size - offset,
            "fill is not word-aligned or is outside the VkBuffer");
    atomic_store(&lastFillOffset, offset);
    atomic_store(&lastFillSize, size);
    atomic_fetch_add(&fillCommands, 1);
}

static void VKAPI_CALL testCmdPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage, VkDependencyFlags dependencies,
    uint32_t memoryCount, const VkMemoryBarrier *memoryBarriers,
    uint32_t bufferCount, const VkBufferMemoryBarrier *bufferBarriers,
    uint32_t imageCount, const VkImageMemoryBarrier *imageBarriers) {
    (void)command;
    (void)sourceStage;
    (void)destinationStage;
    (void)dependencies;
    (void)memoryCount;
    (void)memoryBarriers;
    (void)bufferCount;
    (void)bufferBarriers;
    (void)imageCount;
    (void)imageBarriers;
}

static void setupRuntime(RunMode mode) {
    atomic_store(&runtimeMode, mode);
    unsetenv("RDNA4M_TEST_TIMEOUT_MS");
    RDNA4R.tried = YES;
    RDNA4R.ok = YES;
    RDNA4R.dev = (VkDevice)(uintptr_t)0x1000;
    RDNA4R.q = (VkQueue)(uintptr_t)0x2000;
    RDNA4R.qfi = 0;
    RDNA4R.hostExt = YES;
    RDNA4R.bufferAddress = YES;
    RDNA4R.hostAlign = kHostAlignment;
    RDNA4R.lim.bufferImageGranularity = 1;
    RDNA4R.lim.minStorageBufferOffsetAlignment = 4;
    RDNA4R.lim.minUniformBufferOffsetAlignment = 4;
    RDNA4R.mp.memoryTypeCount = 1;
    RDNA4R.mp.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pthread_mutex_init(&RDNA4R.qlock, NULL);

    RDNA4Caps = rdna4dc_fallback();
    RDNA4Caps.buffer_length = UINT64_C(8) << 30;
    atomic_store(&rdna4_caps_ready, 1);

    vkCreateBuffer = testCreateBuffer;
    vkDestroyBuffer = testDestroyBuffer;
    vkGetBufferMemoryRequirements = testBufferRequirements;
    rdna4_vkGetMHPP = testHostPointerProperties;
    vkAllocateMemory = testAllocateMemory;
    vkFreeMemory = testFreeMemory;
    vkBindBufferMemory = testBindMemory;
    vkMapMemory = testMapMemory;
    rdna4_vkGetBufferAddress = testBufferAddress;
    vkCreateCommandPool = testCreateCommandPool;
    vkDestroyCommandPool = testDestroyCommandPool;
    vkAllocateCommandBuffers = testAllocateCommandBuffers;
    vkFreeCommandBuffers = testFreeCommandBuffers;
    vkBeginCommandBuffer = testBeginCommandBuffer;
    vkEndCommandBuffer = testEndCommandBuffer;
    vkCreateFence = testCreateFence;
    vkDestroyFence = testDestroyFence;
    vkQueueSubmit = testQueueSubmit;
    vkWaitForFences = testWaitForFences;
    vkCmdFillBuffer = testCmdFillBuffer;
    vkCmdPipelineBarrier = testCmdPipelineBarrier;

    if (mode == RunLifecycle) {
        fenceEntered = dispatch_semaphore_create(0);
        fenceRelease = dispatch_semaphore_create(0);
    }
    completionSeen = dispatch_semaphore_create(0);
    rejectedSeen = dispatch_semaphore_create(0);
    require(completionSeen != NULL && rejectedSeen != NULL,
            "cannot create completion semaphores");
}

static id<MTLDevice> makeDevice(void) {
    GarnetDevice *device = [[GarnetDevice alloc] init];
    require(device != nil, "GarnetDevice init returned nil");
    return (id<MTLDevice>)device;
}

static BOOL waitForWeakBufferRelease(unsigned timeoutMs) {
    for (unsigned elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        if (weakLargeBuffer == nil) return YES;
        usleep(10000);
    }
    return weakLargeBuffer == nil;
}

static BOOL waitForWeakCommandRelease(unsigned timeoutMs) {
    for (unsigned elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        if (weakQuarantinedCommand == nil) return YES;
        usleep(10000);
    }
    return weakQuarantinedCommand == nil;
}

static void requireNoRuntimeGrowth(const char *scenario, unsigned expectedLivePools) {
    require(atomic_load(&hostMappedBytes) == atomic_load(&hostUnmappedBytes),
            "owned host mappings did not balance");
    require(atomic_load(&hostLiveMapBytes) == 0,
            "owned host mapping remained live");
    require(atomic_load(&bufferCreates) == atomic_load(&bufferDestroys),
            "VkBuffer handles did not balance");
    require(atomic_load(&memoryAllocates) == atomic_load(&memoryFrees),
            "VkDeviceMemory handles did not balance");
    require(atomic_load(&commandAllocates) == atomic_load(&commandFrees),
            "command-buffer handles did not balance");
    require(atomic_load(&commandPoolCreates) - atomic_load(&commandPoolDestroys) ==
            expectedLivePools,
            "command-pool handle count is not the expected live count");
    require(atomic_load(&fenceCreates) == atomic_load(&fenceDestroys),
            "fence handles did not balance");
    require(RDNA4R.impTotal == 0, "import ledger did not return to zero");
    require(atomic_load(&rdna4_alloc_total) == 0,
            "currentAllocatedSize did not return to zero");
    require(rdna4_unresolved_commands.count == 0,
            "normal scenario retained unresolved resources");
    (void)scenario;
}

static void runLifecycle(void) {
    setupRuntime(RunLifecycle);
    id<MTLDevice> device = makeDevice();
    RDNA4CommandQueue *queue = (RDNA4CommandQueue *)[device newCommandQueue];
    require(queue != nil, "RDNA4CommandQueue init returned nil");
    uint64_t expectedSpan = kLargeLength;
    require(expectedSpan > kBoundaryBytes, "large resource does not cross 64 MiB");

    for (unsigned cycle = 0; cycle < kCycleCount; ++cycle) {
        @autoreleasepool {
            RDNA4Buffer *buffer =
                [device newBufferWithLength:(NSUInteger)kLargeLength
                                   options:MTLResourceStorageModeShared];
            require(buffer != nil, "large Shared buffer allocation failed");
            require(buffer.length == kLargeLength && buffer.contents != NULL,
                    "large Shared buffer lost logical length or CPU visibility");
            require(buffer.rdna4Imported, "large Shared buffer did not use owned import");
            require(RDNA4R.impTotal == expectedSpan,
                    "owned import ledger does not equal the aligned span");
            require(atomic_load(&rdna4_alloc_total) == expectedSpan,
                    "allocated-size accounting does not include one owned span");
            uint8_t *bytes = buffer.contents;
            bytes[0] = (uint8_t)(0x20 + cycle);
            bytes[buffer.length - 1] = (uint8_t)(0x80 + cycle);
            require(bytes[0] == (uint8_t)(0x20 + cycle) &&
                    bytes[buffer.length - 1] == (uint8_t)(0x80 + cycle),
                    "owned CPU range is not directly visible");
            weakLargeBuffer = buffer;

            id<MTLCommandBuffer> command =
                [(id<MTLCommandQueue>)queue commandBuffer];
            require(command != nil, "command buffer allocation failed");
            id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
            require(encoder != nil, "blit encoder allocation failed");
            [encoder fillBuffer:(id<MTLBuffer>)buffer
                          range:NSMakeRange((NSUInteger)kFillOffset,
                                            (NSUInteger)kFillBytes)
                          value:(uint8_t)(0x40 + cycle)];
            [encoder endEncoding];
            require(![(RDNA4CommandBuffer *)command rdna4Error],
                    "cross-boundary fill failed to encode");

            unsigned destroysBefore = atomic_load(&bufferDestroys);
            unsigned freesBefore = atomic_load(&memoryFrees);
            uint64_t liveBefore = atomic_load(&hostLiveMapBytes);
            [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
                atomic_store(&completionStatus, (int)finished.status);
                atomic_store(&completionHadError, finished.error != nil);
                atomic_store(&completionSawLiveBuffer, weakLargeBuffer != nil);
                atomic_fetch_add(&completionCallbacks, 1);
                dispatch_semaphore_signal(completionSeen);
            }];
            [command commit];

            // Drop the application's only strong reference before GPU completion.
            buffer = nil;
            require(weakLargeBuffer != nil,
                    "command buffer did not keep an early-released resource alive");
            require(dispatch_semaphore_wait(
                        fenceEntered,
                        dispatch_time(DISPATCH_TIME_NOW,
                                      INT64_C(5) * NSEC_PER_SEC)) == 0,
                    "fence wait did not begin");
            require(weakLargeBuffer != nil,
                    "resource disappeared after commit but before retirement");
            require(atomic_load(&bufferDestroys) == destroysBefore &&
                    atomic_load(&memoryFrees) == freesBefore &&
                    atomic_load(&hostLiveMapBytes) == liveBefore,
                    "in-flight buffer backing was released before completion");

            dispatch_semaphore_signal(fenceRelease);
            require(dispatch_semaphore_wait(
                        completionSeen,
                        dispatch_time(DISPATCH_TIME_NOW,
                                      INT64_C(5) * NSEC_PER_SEC)) == 0,
                    "normal completion callback was not delivered");
            [command waitUntilCompleted];
            require(atomic_load(&completionCallbacks) == cycle + 1,
                    "completion handler did not run exactly once");
            require(atomic_load(&completionStatus) == MTLCommandBufferStatusCompleted &&
                    !atomic_load(&completionHadError),
                    "normal command buffer did not complete successfully");
            require(atomic_load(&completionSawLiveBuffer),
                    "completion handler observed a released large resource");
            require(atomic_load(&lastFillOffset) == kFillOffset &&
                    atomic_load(&lastFillSize) == kFillBytes,
                    "encoded fill did not cross the 64 MiB logical boundary");

            weakQuarantinedCommand = (RDNA4CommandBuffer *)command;
            command = nil;
            dispatch_sync([queue rdna4CompletionQueue], ^{});
        }
        require(waitForWeakBufferRelease(5000),
                "large resource was not released after completion");
        require(waitForWeakCommandRelease(5000),
                "completed command buffer remained retained");
        requireNoRuntimeGrowth("lifecycle cycle", 1);
        printf("LARGE_MEMORY_LIFETIME_CYCLE_PASS cycle=%u logical_bytes=%llu "
               "cross_boundary=1 early_app_release=1 retained_until_completion=1 "
               "release_balanced=1\n",
               cycle + 1, (unsigned long long)kLargeLength);
    }
    queue = nil;
    device = nil;
    requireNoRuntimeGrowth("lifecycle", 0);
    printf("LARGE_MEMORY_LIFETIME_PASS cycles=%u logical_bytes=%llu "
           "owned_import_balance=1 early_release_retained=1 "
           "selected_cpu_pages=2 simulated_vulkan=1 gpu_execution=0 "
           "real_large_backing=0\n",
           kCycleCount, (unsigned long long)kLargeLength);
}

static void runAllocationStress(void) {
    setupRuntime(RunAllocationStress);
    id<MTLDevice> device = makeDevice();
    unsigned failures = 0;
    for (unsigned round = 0; round < kFailureRounds; ++round) {
        for (int failure = FailCreate; failure <= FailAddress; ++failure) {
            atomic_store(&injectedFailure, failure);
            @autoreleasepool {
                NSError *error = nil;
                RDNA4Buffer *buffer =
                    [[RDNA4Buffer alloc] initWithDevice:device
                                                length:(NSUInteger)kLargeLength
                                               options:MTLResourceStorageModeShared
                                                 error:&error];
                require(buffer == nil && error != nil,
                        "injected allocation failure was accepted");
            }
            atomic_store(&injectedFailure, FailNone);
            requireNoRuntimeGrowth("Vulkan allocation failure", 0);
            failures++;
        }
        atomic_store(&refuseHostMap, 1);
        @autoreleasepool {
            NSError *error = nil;
            RDNA4Buffer *buffer =
                [[RDNA4Buffer alloc] initWithDevice:device
                                            length:(NSUInteger)kLargeLength
                                           options:MTLResourceStorageModeShared
                                             error:&error];
            require(buffer == nil && error != nil, "injected mmap failure was accepted");
        }
        atomic_store(&refuseHostMap, 0);
        requireNoRuntimeGrowth("host mapping failure", 0);
        failures++;
    }
    for (unsigned round = 0; round < kSuccessRounds; ++round) {
        @autoreleasepool {
            NSError *error = nil;
            RDNA4Buffer *buffer =
                [[RDNA4Buffer alloc] initWithDevice:device
                                            length:(NSUInteger)kLargeLength
                                           options:MTLResourceStorageModeShared
                                             error:&error];
            require(buffer != nil && error == nil,
                    "successful stress allocation failed");
            require(buffer.rdna4Imported &&
                    RDNA4R.impTotal == kLargeLength &&
                    atomic_load(&rdna4_alloc_total) == kLargeLength,
                    "successful stress allocation lost owned-import accounting");
        }
        requireNoRuntimeGrowth("successful allocation release", 0);
    }
    device = nil;
    printf("LARGE_MEMORY_LIFETIME_ALLOC_STRESS_PASS failures=%u "
           "successful_release_cycles=%u logical_bytes=%llu "
           "mapping_growth=0 handle_growth=0 ledger_growth=0 "
           "simulated_vulkan=1 gpu_execution=0\n",
           failures, kSuccessRounds, (unsigned long long)kLargeLength);
}

static void waitForFaultCompletion(RunMode mode) {
    if (mode == RunHang) {
        for (unsigned elapsed = 0; elapsed < 5000; elapsed += 10) {
            if (atomic_load(&fenceWaits) >= 30) break;
            usleep(10000);
        }
        require(atomic_load(&fenceWaits) == 30,
                "hung fence simulation did not exhaust the bounded slices");
    }
    require(dispatch_semaphore_wait(
                completionSeen,
                dispatch_time(DISPATCH_TIME_NOW, INT64_C(5) * NSEC_PER_SEC)) == 0,
            "fault completion callback was not delivered");
}

static void runFault(RunMode mode) {
    require(mode == RunHang || mode == RunPoison, "invalid fault mode");
    setupRuntime(mode);
    if (mode == RunHang) setenv("RDNA4M_TEST_TIMEOUT_MS", "1", 1);
    id<MTLDevice> device = makeDevice();
    RDNA4CommandQueue *queue = (RDNA4CommandQueue *)[device newCommandQueue];
    require(queue != nil, "fault command queue allocation failed");

    RDNA4Buffer *buffer = nil;
    id<MTLCommandBuffer> command = nil;
    @autoreleasepool {
        buffer = [device newBufferWithLength:(NSUInteger)kLargeLength
                                     options:MTLResourceStorageModeShared];
        require(buffer != nil, "fault scenario large buffer allocation failed");
        weakLargeBuffer = buffer;
        command = [(id<MTLCommandQueue>)queue commandBuffer];
        require(command != nil, "fault scenario command buffer allocation failed");
        weakQuarantinedCommand = (RDNA4CommandBuffer *)command;
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        require(encoder != nil, "fault scenario blit encoder allocation failed");
        [encoder fillBuffer:(id<MTLBuffer>)buffer
                      range:NSMakeRange((NSUInteger)kFillOffset,
                                        (NSUInteger)kFillBytes)
                      value:0x7a];
        [encoder endEncoding];
        require(![(RDNA4CommandBuffer *)command rdna4Error],
                "fault scenario fill did not encode");
        [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            atomic_store(&completionStatus, (int)finished.status);
            atomic_store(&completionHadError, finished.error != nil);
            atomic_store(&completionSawLiveBuffer, weakLargeBuffer != nil);
            atomic_fetch_add(&completionCallbacks, 1);
            dispatch_semaphore_signal(completionSeen);
        }];
        uint64_t mappedBefore = atomic_load(&hostLiveMapBytes);
        unsigned destroysBefore = atomic_load(&bufferDestroys);
        unsigned freesBefore = atomic_load(&memoryFrees);
        [command commit];
        buffer = nil;
        waitForFaultCompletion(mode);
        dispatch_sync([queue rdna4CompletionQueue], ^{});
        require(atomic_load(&completionCallbacks) == 1,
                "fault completion callback did not run exactly once");
        require(atomic_load(&completionStatus) == MTLCommandBufferStatusError &&
                atomic_load(&completionHadError),
                "fault completion did not report an error");
        require(atomic_load(&completionSawLiveBuffer),
                "fault completion released the referenced large resource early");
        require(rdna4_unresolved_commands.count == 1,
                "unresolved command was not quarantined exactly once");
        require(atomic_load(&submits) == 1,
                "fault path did not submit exactly once");
        if (mode == RunHang) {
            require(atomic_load(&fenceWaits) == 30,
                    "hang path did not stop after its bounded fence slices");
            require(atomic_load(&rdna4_submit_fault) == VK_TIMEOUT,
                    "hang did not poison subsequent submissions with VK_TIMEOUT");
        } else {
            require(atomic_load(&fenceWaits) == 0,
                    "submit poison reached a fence wait");
            require(atomic_load(&rdna4_submit_fault) == VK_ERROR_DEVICE_LOST,
                    "submit poison did not set VK_ERROR_DEVICE_LOST");
        }
        require(weakLargeBuffer != nil && weakQuarantinedCommand != nil,
                "quarantine did not retain the resource and command buffer");
        require(atomic_load(&hostLiveMapBytes) == mappedBefore &&
                atomic_load(&bufferDestroys) == destroysBefore &&
                atomic_load(&memoryFrees) == freesBefore,
                "fault quarantine released in-flight owned backing");
        require(RDNA4R.impTotal == kLargeLength &&
                atomic_load(&rdna4_alloc_total) == kLargeLength,
                "fault quarantine lost import or allocated-size accounting");
        command = nil;
    }

    unsigned submitsBeforeReject = atomic_load(&submits);
    unsigned waitsBeforeReject = atomic_load(&fenceWaits);
    @autoreleasepool {
        RDNA4Buffer *later = [device newBufferWithLength:4096
                                                 options:MTLResourceStorageModeShared];
        require(later != nil, "post-fault small allocation failed");
        id<MTLCommandBuffer> rejected =
            [(id<MTLCommandQueue>)queue commandBuffer];
        require(rejected != nil, "post-fault command allocation failed");
        id<MTLBlitCommandEncoder> encoder = [rejected blitCommandEncoder];
        [encoder fillBuffer:(id<MTLBuffer>)later
                      range:NSMakeRange(0, 4)
                      value:0x33];
        [encoder endEncoding];
        [rejected addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            atomic_store(&rejectedHadError, finished.error != nil);
            atomic_fetch_add(&rejectedCallbacks, 1);
            dispatch_semaphore_signal(rejectedSeen);
        }];
        [rejected commit];
        require(dispatch_semaphore_wait(
                    rejectedSeen,
                    dispatch_time(DISPATCH_TIME_NOW,
                                  INT64_C(5) * NSEC_PER_SEC)) == 0,
                "post-fault submission was not rejected");
        [rejected waitUntilCompleted];
        require(atomic_load(&rejectedCallbacks) == 1 &&
                atomic_load(&rejectedHadError),
                "post-fault command did not complete with an error");
        require(atomic_load(&submits) == submitsBeforeReject &&
                atomic_load(&fenceWaits) == waitsBeforeReject,
                "post-fault command replayed Vulkan submission or fence work");
        require(rdna4_unresolved_commands.count == 1,
                "post-fault rejection changed the quarantine set");
    }
    dispatch_sync([queue rdna4CompletionQueue], ^{});
    require(weakLargeBuffer != nil && weakQuarantinedCommand != nil,
            "post-fault rejection released the original quarantine");
    require(atomic_load(&commandAllocates) - atomic_load(&commandFrees) == 1,
            "quarantine did not retain exactly one VkCommandBuffer");
    require(atomic_load(&fenceCreates) - atomic_load(&fenceDestroys) == 1,
            "quarantine did not retain exactly one VkFence");
    require(atomic_load(&bufferCreates) - atomic_load(&bufferDestroys) == 1 &&
            atomic_load(&memoryAllocates) - atomic_load(&memoryFrees) == 1,
            "quarantine handle accounting is not exact");

    printf("LARGE_MEMORY_LIFETIME_FAULT_PASS mode=%s logical_bytes=%llu "
           "submits=%u fence_waits=%u quarantined=%lu early_release_retained=1 "
           "replayed=0 subsequent_rejected=1 bounded_wait=1 "
           "simulated_vulkan=1 gpu_execution=0 real_large_backing=0 "
           "process_exit_required=1\n",
           mode == RunHang ? "hang" : "poison",
           (unsigned long long)kLargeLength, atomic_load(&submits),
           atomic_load(&fenceWaits),
           (unsigned long)rdna4_unresolved_commands.count);
}

static BOOL waitForChild(pid_t child, unsigned timeoutMs, int *status) {
    for (unsigned elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) return YES;
        if (waited < 0 && errno != EINTR) return NO;
        usleep(10000);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, status, 0);
    return NO;
}

static BOOL runChild(const char *executable, const char *mode, int expected,
                     unsigned timeoutMs) {
    posix_spawn_file_actions_t actions;
    int actionStatus = posix_spawn_file_actions_init(&actions);
    require(actionStatus == 0, "cannot initialize child process actions");
    char *const arguments[] = { (char *)executable, (char *)mode, NULL };
    pid_t child = 0;
    uint64_t started = rdna4_now();
    int spawnStatus = posix_spawn(&child, executable, &actions, NULL,
                                  arguments, *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    require(spawnStatus == 0, "cannot spawn lifetime child");
    int status = 0;
    BOOL exited = waitForChild(child, timeoutMs, &status);
    uint64_t elapsedMs = (rdna4_now() - started) / UINT64_C(1000000);
    if (!exited) {
        fprintf(stderr, "LARGE_MEMORY_LIFETIME_CHILD_TIMEOUT mode=%s pid=%d\n",
                mode, (int)child);
        return NO;
    }
    int actual = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    BOOL passed = WIFEXITED(status) && actual == expected;
    printf("%s child=%s expected_exit=%d actual_exit=%d elapsed_ms=%llu "
           "bounded_wait=1 kernel_release_unverified=1\n",
           passed ? "LARGE_MEMORY_LIFETIME_PROCESS_EXIT_PASS"
                  : "LARGE_MEMORY_LIFETIME_PROCESS_EXIT_FAIL",
           mode, expected, actual, (unsigned long long)elapsedMs);
    return passed;
}

static void runAllChildren(const char *executable) {
    setvbuf(stdout, NULL, _IONBF, 0);
    BOOL ok = YES;
    ok &= runChild(executable, "lifecycle", 0, 15000);
    ok &= runChild(executable, "alloc-stress", 0, 15000);
    ok &= runChild(executable, "hang", kFaultExitCode, 15000);
    ok &= runChild(executable, "poison", kFaultExitCode, 15000);
    require(ok, "one or more child process contracts failed");
    puts("LARGE_MEMORY_LIFETIME_ALL_PASS children=4 lifecycle_exit=0 "
         "fault_exit=75 kernel_release_unverified=1");
}

static BOOL parseMode(const char *text, RunMode *mode) {
    if (!strcmp(text, "lifecycle")) *mode = RunLifecycle;
    else if (!strcmp(text, "alloc-stress")) *mode = RunAllocationStress;
    else if (!strcmp(text, "hang")) *mode = RunHang;
    else if (!strcmp(text, "poison")) *mode = RunPoison;
    else return NO;
    return YES;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-large-memory-lifetime --help\n"
                 "       test-large-memory-lifetime all\n"
                 "       test-large-memory-lifetime lifecycle|alloc-stress|hang|poison\n"
                 "Runs production Metal allocation/command lifetime code with simulated Vulkan.\n"
                 "hang and poison intentionally exit 75 after verifying bounded quarantine.\n"
                 "No real large backing, GPU execution, or kernel page/handle release is tested.");
            return 0;
        }
        if (argc != 2) return kInvalidExitCode;
        if (!strcmp(argv[1], "all")) {
            runAllChildren(argv[0]);
            return 0;
        }
        RunMode mode;
        if (!parseMode(argv[1], &mode)) return kInvalidExitCode;
        if (mode == RunLifecycle) {
            runLifecycle();
            return 0;
        }
        if (mode == RunAllocationStress) {
            runAllocationStress();
            return 0;
        }
        runFault(mode);
        printf("LARGE_MEMORY_LIFETIME_FAULT_EXIT mode=%s exit_status=75 "
               "kernel_release_unverified=1\n",
               mode == RunHang ? "hang" : "poison");
        fflush(stdout);
        _exit(kFaultExitCode);
    }
}
