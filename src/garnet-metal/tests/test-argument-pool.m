// Exercise GPU-argument descriptor-pool global accounting with mocked Vulkan.
#include "GarnetDevice.m"

static unsigned checks, creates, destroys, failCreateAt;

static void require(BOOL ok, const char *message) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "ARGUMENT_POOL_FAIL %s\n", message);
        exit(1);
    }
}

static VkResult VKAPI_CALL createPool(VkDevice device,
    const VkDescriptorPoolCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkDescriptorPool *pool) {
    (void)device;
    (void)allocator;
    require(info != NULL && pool != NULL, "pool create arguments");
    require(info->flags == VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        "argument pools are update-after-bind");
    ++creates;
    *pool = VK_NULL_HANDLE;
    if (failCreateAt && creates == failCreateAt) {
        failCreateAt = 0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *pool = (VkDescriptorPool)(uintptr_t)creates;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroyPool(VkDevice device, VkDescriptorPool pool,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(pool != VK_NULL_HANDLE, "destroy real pool handle");
    ++destroys;
}

static RDNA4ArgumentPool *newPool(uint32_t first, uint32_t second,
    uint32_t maxSets, NSError **error) {
    VkDescriptorPoolSize sizes[2];
    uint32_t sizeCount = 0;
    if (first)
        sizes[sizeCount++] = (VkDescriptorPoolSize){
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, first };
    if (second)
        sizes[sizeCount++] = (VkDescriptorPoolSize){
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, second };
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = maxSets,
        .poolSizeCount = sizeCount,
        .pPoolSizes = sizes,
    };
    return [[RDNA4ArgumentPool alloc] initWithInfo:&info error:error];
}

static RDNA4ArgumentPool *newQueryPool(NSError **error) {
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
    };
    return [[RDNA4ArgumentPool alloc] initWithInfo:&info error:error];
}

static uint64_t accounted(void) {
    return atomic_load(&rdna4_argument_pool_descriptors);
}

int main(void) {
    @autoreleasepool {
        RDNA4R.dev = (VkDevice)(uintptr_t)1;
        RDNA4R.ok = YES;
        RDNA4R.descriptorLimits.maxUpdateAfterBindDescriptorsInAllPools = 12;
        vkCreateDescriptorPool = createPool;
        vkDestroyDescriptorPool = destroyPool;

        NSError *error = nil;
        RDNA4ArgumentPool *a = newPool(3, 2, 3, &error);
        RDNA4ArgumentPool *b = newPool(4, 3, 3, &error);
        require(a != nil && b != nil && !error && creates == 2 && accounted() == 12,
            "last legal aggregate pool descriptor count");

        unsigned createsBefore = creates;
        error = nil;
        RDNA4ArgumentPool *rejected = newPool(1, 0, 3, &error);
        require(rejected == nil && error != nil && creates == createsBefore &&
                accounted() == 12,
            "over-limit pool is rejected before vkCreateDescriptorPool");

        b = nil;
        require(accounted() == 5 && destroys == 1,
            "releasing a pool returns its descriptor slots");
        error = nil;
        RDNA4ArgumentPool *c = newPool(4, 3, 3, &error);
        require(c != nil && !error && accounted() == 12,
            "released slots are reusable at the last legal total");
        a = nil;
        c = nil;
        require(accounted() == 0 && destroys == 3,
            "all pool slots return to the global budget");

        error = nil;
        failCreateAt = creates + 1;
        RDNA4ArgumentPool *failed = newPool(7, 5, 3, &error);
        require(failed == nil && error != nil && accounted() == 0,
            "vkCreateDescriptorPool failure rolls back its reservation");

        error = nil;
        RDNA4ArgumentPool *afterFailure = newPool(7, 5, 3, &error);
        require(afterFailure != nil && !error && accounted() == 12,
            "a failed reservation does not poison the remaining budget");
        afterFailure = nil;
        require(accounted() == 0,
            "failed-path reservation accounting is released");

        error = nil;
        createsBefore = creates;
        RDNA4ArgumentPool *queryOnly = newQueryPool(&error);
        require(queryOnly != nil && !error && creates == createsBefore + 1 &&
                accounted() == 0,
            "zero-size query-only pool is legal without consuming descriptors");
        queryOnly = nil;
        require(accounted() == 0,
            "zero-size pool release does not underflow the global budget");
        require(creates == 6 && destroys == 5,
            "every successfully created pool has exactly one destroy");

        printf("ARGUMENT_POOL_PASS checks=%u creates=%u destroys=%u gpu_execution=0\n",
            checks, creates, destroys);
    }
    return 0;
}
