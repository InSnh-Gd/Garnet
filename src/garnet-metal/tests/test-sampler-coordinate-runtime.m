// Exercise lazy normalized sampler representations with a mocked Vulkan backend.
#include "GarnetDevice.m"

@interface RDNA4SamplerState (CoordinateRuntimeTest)
- (RDNA4SamplerStorage *)rdna4TestStorage;
@end

@implementation RDNA4SamplerState (CoordinateRuntimeTest)
- (RDNA4SamplerStorage *)rdna4TestStorage { return _storage; }
@end

static unsigned creates, destroys;
static BOOL failNext;
static VkSamplerCreateInfo infos[16];
static VkSampler destroyed[16];

static void require(BOOL ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "SAMPLER_COORDINATE_RUNTIME_FAIL %s\n", message);
        exit(1);
    }
}

static NSUInteger allocationCount(void) {
    return (NSUInteger)atomic_load(&rdna4_sampler_allocations);
}

static RDNA4SamplerStorage *storageOf(RDNA4SamplerState *state) {
    require(state != nil, "sampler wrapper is nil");
    return [state rdna4TestStorage];
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSampler *sampler) {
    (void)device;
    (void)allocator;
    require(info && sampler && creates < sizeof infos / sizeof infos[0],
        "mock create arguments/history");
    infos[creates] = *info;
    if (failNext) {
        failNext = NO;
        *sampler = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *sampler = (VkSampler)(uintptr_t)(0x7000u + creates);
    ++creates;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
                                      const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(sampler != VK_NULL_HANDLE &&
        destroys < sizeof destroyed / sizeof destroyed[0],
        "mock destroy arguments/history");
    for (unsigned i = 0; i < destroys; ++i)
        require(destroyed[i] != sampler, "sampler destroyed twice");
    destroyed[destroys++] = sampler;
}

static MTLSamplerDescriptor *descriptor(BOOL normalizedCoordinates,
                                        NSString *label) {
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.label = label;
    d.normalizedCoordinates = normalizedCoordinates;
    d.minFilter = MTLSamplerMinMagFilterNearest;
    d.magFilter = MTLSamplerMinMagFilterNearest;
    d.mipFilter = normalizedCoordinates ? MTLSamplerMipFilterLinear :
                                          MTLSamplerMipFilterNotMipmapped;
    d.sAddressMode = normalizedCoordinates ? MTLSamplerAddressModeRepeat :
                                             MTLSamplerAddressModeClampToEdge;
    d.tAddressMode = MTLSamplerAddressModeClampToEdge;
    d.rAddressMode = MTLSamplerAddressModeClampToEdge;
    d.maxAnisotropy = 1;
    d.compareFunction = MTLCompareFunctionLess;
    d.lodMinClamp = normalizedCoordinates ? 0.25 : 0.0;
    d.lodMaxClamp = normalizedCoordinates ? 12.5 : 0.0;
    return d;
}

static void reset(const char *test) {
    @synchronized ([RDNA4SamplerStorage class]) {
        require(!rdna4_sampler_configs || ![rdna4_sampler_configs count],
            "sampler registry was not empty");
        require(allocationCount() == 0, "sampler allocation count was not empty");
        rdna4_sampler_configs = [NSMapTable strongToWeakObjectsMapTable];
        rdna4_argument_sampler_configs = 0;
        atomic_store(&rdna4_sampler_allocations, 0);
    }
    creates = destroys = 0;
    failNext = NO;
    memset(infos, 0, sizeof infos);
    memset(destroyed, 0, sizeof destroyed);
    RDNA4R.lim.maxSamplerAllocationCount = 64;
    RDNA4LOG("sampler-coordinate-runtime: %s", test);
}

static void testPixelThreeHandles(void) {
    reset("pixel-three-handles");
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:descriptor(NO, @"pixel") error:&error];
        require(state && !error, "pixel sampler creation");
        RDNA4SamplerStorage *storage = storageOf(state);
        VkSampler legacyNormal = storage->normal;
        require([state rdna4PixelCoordinates] &&
            storage->comparison == VK_NULL_HANDLE &&
            storage->normalizedNormal == VK_NULL_HANDLE &&
            allocationCount() == 1,
            "pixel storage did not start with only the legacy normal handle");

        error = nil;
        require([state rdna4SamplerComparison:YES error:&error] ==
            VK_NULL_HANDLE && error && creates == 1 &&
            storage->comparison == VK_NULL_HANDLE,
            "legacy pixel comparison was not rejected before allocation");

        error = nil;
        VkSampler normalizedNormal =
            [state rdna4SamplerComparison:NO normalizedCoordinates:YES
                                    error:&error];
        require(normalizedNormal != VK_NULL_HANDLE && !error && creates == 2 &&
            allocationCount() == 2 && storage->normal == legacyNormal &&
            storage->normalizedNormal == normalizedNormal &&
            storage->comparison == VK_NULL_HANDLE,
            "normalized pixel normal representation");
        require(infos[1].compareEnable == VK_FALSE &&
            infos[1].unnormalizedCoordinates == VK_FALSE &&
            infos[1].mipLodBias == 0.0f &&
            storage->config.unnorm == 1,
            "normalized pixel normal state or source key changed");

        error = nil;
        VkSampler normalizedComparison =
            [state rdna4SamplerComparison:YES normalizedCoordinates:YES
                                    error:&error];
        require(normalizedComparison != VK_NULL_HANDLE && !error &&
            creates == 3 && allocationCount() == 3 &&
            storage->comparison == normalizedComparison &&
            storage->normalizedNormal == normalizedNormal,
            "normalized pixel comparison representation");
        require(infos[2].compareEnable == VK_TRUE &&
            infos[2].compareOp == VK_COMPARE_OP_LESS &&
            infos[2].unnormalizedCoordinates == VK_FALSE &&
            infos[2].mipLodBias == 0.0f &&
            storage->config.unnorm == 1,
            "normalized pixel comparison state or source key changed");
        require([state rdna4SamplerComparison:NO normalizedCoordinates:YES
                                        error:&error] == normalizedNormal &&
            [state rdna4SamplerComparison:YES normalizedCoordinates:YES
                                    error:&error] == normalizedComparison &&
            creates == 3, "normalized handles were not reused");
        error = nil;
        require([state rdna4SamplerComparison:YES error:&error] ==
            VK_NULL_HANDLE && error && creates == 3 &&
            allocationCount() == 3 &&
            storage->comparison == normalizedComparison,
            "legacy pixel comparison reused the normalized comparison handle");
        state = nil;
    }
    require(allocationCount() == 0 && destroys == 3 &&
        destroyed[0] != destroyed[1] && destroyed[1] != destroyed[2] &&
        destroyed[0] != destroyed[2],
        "pixel three-handle destruction accounting");
}

static void testNormalizedSourceReusesHandles(void) {
    reset("normalized-source-reuse");
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:descriptor(YES, @"normalized")
                     error:&error];
        require(state && !error, "normalized sampler creation");
        RDNA4SamplerStorage *storage = storageOf(state);
        VkSampler normal = storage->normal;
        require(![state rdna4PixelCoordinates] && allocationCount() == 1 &&
            storage->normalizedNormal == VK_NULL_HANDLE,
            "source-normalized storage initial state");
        require([state rdna4SamplerComparison:NO normalizedCoordinates:YES
                                        error:&error] == normal &&
            storage->normalizedNormal == VK_NULL_HANDLE && creates == 1,
            "source-normalized ordinary did not reuse the normal handle");
        VkSampler comparison =
            [state rdna4SamplerComparison:YES normalizedCoordinates:YES
                                    error:&error];
        require(comparison != VK_NULL_HANDLE && creates == 2 &&
            allocationCount() == 2 && storage->comparison == comparison &&
            storage->normalizedNormal == VK_NULL_HANDLE &&
            [state rdna4SamplerComparison:YES error:&error] == comparison,
            "source-normalized comparison did not reuse the comparison slot");
        state = nil;
    }
    require(allocationCount() == 0 && destroys == 2,
        "source-normalized two-handle destruction accounting");
}

static void testLazyFailureRollback(void) {
    reset("lazy-failure-rollback");
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:descriptor(NO, @"rollback")
                     error:&error];
        require(state && !error && allocationCount() == 1 &&
            creates == 1, "rollback test normal handle");
        RDNA4SamplerStorage *storage = storageOf(state);
        VkSampler normal = storage->normal;

        failNext = YES;
        error = nil;
        require([state rdna4SamplerComparison:NO normalizedCoordinates:YES
                                        error:&error] == VK_NULL_HANDLE &&
            error && allocationCount() == 1 && creates == 1 &&
            storage->normal == normal &&
            storage->normalizedNormal == VK_NULL_HANDLE,
            "failed normalized normal did not roll back");

        error = nil;
        VkSampler normalizedNormal =
            [state rdna4SamplerComparison:NO normalizedCoordinates:YES
                                    error:&error];
        require(normalizedNormal != VK_NULL_HANDLE && !error &&
            allocationCount() == 2 && creates == 2,
            "normalized normal retry after failure");

        failNext = YES;
        error = nil;
        require([state rdna4SamplerComparison:YES normalizedCoordinates:YES
                                        error:&error] == VK_NULL_HANDLE &&
            error && allocationCount() == 2 && creates == 2 &&
            storage->normal == normal &&
            storage->normalizedNormal == normalizedNormal &&
            storage->comparison == VK_NULL_HANDLE,
            "failed normalized comparison did not roll back");

        error = nil;
        require([state rdna4SamplerComparison:YES normalizedCoordinates:YES
                                        error:&error] != VK_NULL_HANDLE &&
            !error && allocationCount() == 3 && creates == 3,
            "normalized comparison retry after failure");
        state = nil;
    }
    require(allocationCount() == 0 && destroys == 3,
        "rollback test leaked or double-destroyed a handle");
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
        RDNA4R.samplerAnisotropy = YES;
        RDNA4R.samplerMirrorClamp = NO;
        RDNA4R.lim.maxSamplerAnisotropy = 16.0f;
        RDNA4R.lim.maxSamplerAllocationCount = 64;
        vkCreateSampler = createSampler;
        vkDestroySampler = destroySampler;

        testPixelThreeHandles();
        testNormalizedSourceReusesHandles();
        testLazyFailureRollback();
        printf("SAMPLER_COORDINATE_RUNTIME_PASS pixel_handles=3 "
               "normalized_reuse=1 pixel_validation=1 comparison=1 "
               "lazy_rollback=1 destroy_unique=1 "
               "simulated_vulkan=1 gpu_execution=0\n");
    }
    return 0;
}
