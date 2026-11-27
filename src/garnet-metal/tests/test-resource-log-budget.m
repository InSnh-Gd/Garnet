// Reuse production Vulkan doubles; measure CPU/log work without opening a GPU.
#define main rdna4_heap_fixture_main
#include "test-heap-execution.m"
#undef main

int main(void) { @autoreleasepool {
    alarm(30);
    configureVulkan();
    forcedRequirements = (VkMemoryRequirements){256, 256, 2};
    forceRequirements = YES;
    GarnetDevice *device = sharedTestDevice();
    const unsigned rounds = 4096;
    uint64_t begin = rdna4_now();
    for (unsigned i = 0; i < rounds; ++i) { @autoreleasepool {
        RDNA4Buffer *buffer = (RDNA4Buffer *)[device newBufferWithLength:256
            options:MTLResourceStorageModeShared];
        require(buffer && buffer.length == 256 && buffer.contents,
            "resource log benchmark allocation failed");
        require(((uint8_t *)buffer.contents)[255] == 0, "buffer is not initialized");
    } }
    uint64_t elapsed = rdna4_now() - begin;
    require(bufferCreates == rounds && bufferDestroys == rounds &&
        memoryAllocates == memoryFrees && !liveBuffers && !liveMemories &&
        !atomic_load(&rdna4_alloc_total), "resource benchmark leaked backing");
    for (unsigned i = 0; i < 32; ++i)
        require([device newBufferWithLength:0 options:MTLResourceStorageModeShared] == nil,
            "invalid allocation was accepted");
    printf("RESOURCE_LOG_WORK_PASS rounds=%u elapsed_ns=%llu errors=32 gpu_execution=0\n",
        rounds, (unsigned long long)elapsed);
} return 0; }
