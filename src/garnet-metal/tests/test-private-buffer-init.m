// Actual Metal allocation/one-shot code, with Vulkan recording and retirement boundaries simulated.
#include "GarnetDevice.m"

struct TestMemory { uint64_t size; void *cpu; };
struct TestBuffer { uint64_t size; struct TestMemory *memory; };
static unsigned liveBuffers, liveMemory, fills, copies, submits, waits;
static BOOL allocationFailure, waitTimeout;
static uint64_t fillBytes, copyBytes, tailOffset;
static void require(BOOL value, const char *message) {
    if (!value) { fprintf(stderr, "PRIVATE_INIT_FAIL %s\n", message); exit(1); }
}
static VkResult VKAPI_CALL createBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)device; (void)allocator;
    struct TestBuffer *b = calloc(1, sizeof *b); require(b != NULL, "buffer stub allocation");
    b->size = info->size; *buffer = (VkBuffer)b; liveBuffers++; return VK_SUCCESS;
}
static void VKAPI_CALL destroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator; free((void *)buffer); liveBuffers--;
}
static void VKAPI_CALL requirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *out) {
    (void)device;
    *out = (VkMemoryRequirements){ (((struct TestBuffer *)buffer)->size + 4095) & ~UINT64_C(4095), 4096, 3 };
}
static VkResult VKAPI_CALL allocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    (void)device; (void)allocator;
    struct TestMemory *m = calloc(1, sizeof *m); require(m != NULL, "memory stub allocation");
    m->size = info->allocationSize;
    if (info->memoryTypeIndex == 1) {
        require(m->size <= 4096 && !posix_memalign(&m->cpu, 4096, (size_t)m->size), "unexpected staging size");
        memset(m->cpu, 0xa5, (size_t)m->size);
    }
    *memory = (VkDeviceMemory)m; liveMemory++; return VK_SUCCESS;
}
static void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    struct TestMemory *m = (void *)memory; free(m->cpu); free(m); liveMemory--;
}
static VkResult VKAPI_CALL bindMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device; require(!offset, "unexpected buffer binding offset");
    ((struct TestBuffer *)buffer)->memory = (void *)memory; return VK_SUCCESS;
}
static VkResult VKAPI_CALL mapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                    VkMemoryMapFlags flags, void **pointer) {
    (void)device; (void)offset; (void)size; (void)flags;
    *pointer = ((struct TestMemory *)memory)->cpu; return *pointer ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
}
static VkDeviceAddress VKAPI_CALL address(VkDevice device, const VkBufferDeviceAddressInfo *info) {
    (void)device; (void)info; return UINT64_C(0x1800000000);
}
static VkResult VKAPI_CALL allocateCommands(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *command) {
    (void)device; (void)info;
    *command = (VkCommandBuffer)(uintptr_t)3; return allocationFailure ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
static void VKAPI_CALL freeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *commands) {
    (void)device; (void)pool; (void)count; (void)commands;
}
static VkResult VKAPI_CALL beginCommand(VkCommandBuffer command, const VkCommandBufferBeginInfo *info) {
    (void)command; (void)info; return VK_SUCCESS;
}
static VkResult VKAPI_CALL endCommand(VkCommandBuffer command) { (void)command; return VK_SUCCESS; }
static VkResult VKAPI_CALL createFence(VkDevice device, const VkFenceCreateInfo *info,
                                      const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device; (void)info; (void)allocator; *fence = (VkFence)(uintptr_t)4; return VK_SUCCESS;
}
static void VKAPI_CALL destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device; (void)fence; (void)allocator;
}
static VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    (void)queue; (void)info; (void)fence; require(count == 1, "initialization did not submit one command buffer");
    submits++; return VK_SUCCESS;
}
static VkResult VKAPI_CALL waitFence(VkDevice device, uint32_t count, const VkFence *fence, VkBool32 all, uint64_t timeout) {
    (void)device; (void)count; (void)fence; (void)all;
    require(timeout == UINT64_C(2000000000), "unbounded fence wait"); waits++;
    return waitTimeout ? VK_TIMEOUT : VK_SUCCESS;
}
static void VKAPI_CALL fill(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, uint32_t pattern) {
    (void)command; struct TestBuffer *b = (void *)buffer;
    require(!b->memory->cpu && pattern == 0 && !((offset | size) & 3) && offset <= b->size && size <= b->size - offset,
            "invalid private initialization fill");
    fills++; fillBytes += size;
}
static void VKAPI_CALL copy(VkCommandBuffer command, VkBuffer source, VkBuffer destination, uint32_t count, const VkBufferCopy *regions) {
    (void)command; struct TestBuffer *s = (void *)source, *d = (void *)destination;
    require(count == 1 && s->memory->cpu && !d->memory->cpu, "invalid initialization tail copy");
    const VkBufferCopy *r = regions;
    require(r->size < 4 && r->dstOffset <= d->size && r->size <= d->size - r->dstOffset, "tail outside logical length");
    for (uint64_t i = 0; i < r->size; i++) require(!((uint8_t *)s->memory->cpu)[r->srcOffset + i], "tail staging was not zeroed");
    copies++; copyBytes += r->size; tailOffset = r->dstOffset;
}
static void VKAPI_CALL barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkPipelineStageFlags destination,
    VkDependencyFlags dependencies, uint32_t memoryCount, const VkMemoryBarrier *memory, uint32_t bufferCount,
    const VkBufferMemoryBarrier *buffers, uint32_t imageCount, const VkImageMemoryBarrier *images) {
    (void)command; (void)source; (void)destination; (void)dependencies; (void)memoryCount; (void)memory;
    (void)bufferCount; (void)buffers; (void)imageCount; (void)images;
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-private-buffer-init\nRuns production large Private initialization with simulated Vulkan, no real large backing or GPU execution."); return 0;
        }
        if (argc != 1) return 2;
        RDNA4R.tried = RDNA4R.ok = RDNA4R.bufferAddress = YES;
        pthread_mutex_init(&RDNA4R.qlock, NULL);
        RDNA4R.mp.memoryTypeCount = 2;
        RDNA4R.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        RDNA4R.mp.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        RDNA4Caps = rdna4dc_fallback(); RDNA4Caps.buffer_length = UINT64_C(8) << 30;
        atomic_store(&rdna4_caps_ready, 1);
        vkCreateBuffer = createBuffer; vkDestroyBuffer = destroyBuffer; vkGetBufferMemoryRequirements = requirements;
        vkAllocateMemory = allocateMemory; vkFreeMemory = freeMemory; vkBindBufferMemory = bindMemory; vkMapMemory = mapMemory;
        rdna4_vkGetBufferAddress = address; vkAllocateCommandBuffers = allocateCommands; vkFreeCommandBuffers = freeCommands;
        vkBeginCommandBuffer = beginCommand; vkEndCommandBuffer = endCommand; vkCreateFence = createFence; vkDestroyFence = destroyFence;
        vkQueueSubmit = submit; vkWaitForFences = waitFence; vkCmdFillBuffer = fill; vkCmdCopyBuffer = copy; vkCmdPipelineBarrier = barrier;
        for (unsigned tail = 0; tail < 4; tail++) {
            fills = copies = submits = waits = 0; fillBytes = copyBytes = tailOffset = 0;
            uint64_t length = (UINT64_C(8) << 30) - 4 + tail;
            @autoreleasepool {
                NSError *error = nil;
                RDNA4Buffer *buffer = [[RDNA4Buffer alloc] initWithDevice:nil length:length options:MTLResourceStorageModePrivate error:&error];
                require(buffer && !error && buffer.length == length && !buffer.contents, "private buffer initialization failed");
                require(fills == 1 && fillBytes == (length & ~UINT64_C(3)) && copies == (tail != 0) && copyBytes == tail &&
                    (!tail || tailOffset == length - tail) && submits == 1 && waits == 1, "initialization missed bytes, tail or completion");
            }
            require(!liveBuffers && !liveMemory && !atomic_load(&rdna4_alloc_total), "successful initialization leaked resources");
        }
        allocationFailure = YES;
        @autoreleasepool {
            require(![[RDNA4Buffer alloc] initWithDevice:nil length:UINT64_C(8) << 30 options:MTLResourceStorageModePrivate error:NULL],
                    "pre-submit failure accepted");
        }
        require(!liveBuffers && !liveMemory && !atomic_load(&rdna4_alloc_total), "pre-submit failure did not reclaim backing");
        allocationFailure = NO; waitTimeout = YES; submits = waits = 0;
        @autoreleasepool {
            require(![[RDNA4Buffer alloc] initWithDevice:nil length:UINT64_C(8) << 30 options:MTLResourceStorageModePrivate error:NULL],
                    "unresolved initialization published a buffer");
        }
        require(submits == 1 && waits == 30 && liveBuffers == 1 && liveMemory == 1 &&
                rdna4_unresolved_commands.count == 1 && atomic_load(&rdna4_submit_fault) == VK_TIMEOUT,
                "unresolved work was replayed, waited without a bound, or released its backing");
        puts("PRIVATE_INIT_PASS logical_8g=simulated fill_and_byte_tails=4 rollback=pass timeout=bounded quarantine=retained gpu_execution=0");
    }
    return 0;
}
