// Exercise the public GarnetDevice sampler API with a mocked Vulkan backend.
// Lazy comparison failure/retry is already covered by test-sampler-runtime.m.
#include "GarnetDevice.m"
#include <limits.h>
#include <objc/runtime.h>

static unsigned checks;
static unsigned attempts;
static unsigned normalAttempts;
static unsigned comparisonAttempts;
static unsigned creates;
static unsigned destroys;
static BOOL failNext;
static VkSamplerCreateInfo lastInfo;
static VkSampler destroyed[8];
enum { invalidAddressMode = 7 };
enum { invalidCompareFunction = 99 };

static MTLCompareFunction invalidCompareGetter(id object, SEL selector) {
    (void)object;
    (void)selector;
    return (MTLCompareFunction)invalidCompareFunction;
}

static void require(BOOL ok, const char *message) {
    checks++;
    if (!ok) {
        fprintf(stderr, "SAMPLER_API_REJECT_FAIL %s\n", message);
        exit(1);
    }
}

static NSUInteger allocationCount(void) {
    return (NSUInteger)atomic_load(&rdna4_sampler_allocations);
}

static NSUInteger registryConfigCount(void) {
    @synchronized ([RDNA4SamplerStorage class]) {
        return rdna4_sampler_configs ? [rdna4_sampler_configs count] : 0;
    }
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSampler *sampler) {
    (void)device;
    (void)allocator;
    require(info != NULL && sampler != NULL, "mock create arguments");
    attempts++;
    if (info->compareEnable) comparisonAttempts++;
    else normalAttempts++;
    lastInfo = *info;
    if (failNext) {
        failNext = NO;
        *sampler = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *sampler = (VkSampler)(uintptr_t)(0x8000u + creates);
    creates++;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
                                      const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(sampler != VK_NULL_HANDLE, "mock destroy received a null sampler");
    require(destroys < sizeof destroyed / sizeof destroyed[0],
        "mock destroy history overflow");
    for (unsigned i = 0; i < destroys; ++i)
        require(destroyed[i] != sampler, "sampler destroyed twice");
    destroyed[destroys++] = sampler;
}

static MTLSamplerDescriptor *validDescriptor(void) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.label = @"api-reject";
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    descriptor.compareFunction = MTLCompareFunctionLess;
    descriptor.lodMinClamp = 0.0;
    descriptor.lodMaxClamp = 1.0;
    descriptor.maxAnisotropy = 1;
    descriptor.supportArgumentBuffers = NO;
    return descriptor;
}

static MTLSamplerDescriptor *invalidCompareDescriptor(void) {
    MTLSamplerDescriptor *descriptor = validDescriptor();
    // Preserve the class cluster's real storage and setters; only bypass its
    // comparison setter's masking when the driver reads the invalid enum.
    Class subclass = objc_allocateClassPair(object_getClass(descriptor),
        "RDNA4InvalidCompareSamplerDescriptor", 0);
    require(subclass != Nil, "cannot create invalid comparison descriptor class");
    Method getter = class_getInstanceMethod(object_getClass(descriptor),
        @selector(compareFunction));
    require(getter && class_addMethod(subclass, @selector(compareFunction),
        (IMP)invalidCompareGetter, method_getTypeEncoding(getter)),
        "cannot override comparison getter");
    objc_registerClassPair(subclass);
    object_setClass(descriptor, subclass);
    return descriptor;
}

static void resetMockAndRegistry(void) {
    @synchronized ([RDNA4SamplerStorage class]) {
        require(!rdna4_sampler_configs || ![rdna4_sampler_configs count],
            "registry was not empty before reset");
        require(allocationCount() == 0,
            "backend sampler count was not empty before reset");
        rdna4_sampler_configs = [NSMapTable strongToWeakObjectsMapTable];
        rdna4_argument_sampler_configs = 0;
        atomic_store(&rdna4_sampler_allocations, 0);
    }
    attempts = normalAttempts = comparisonAttempts = 0;
    creates = destroys = 0;
    failNext = NO;
    memset(&lastInfo, 0, sizeof lastInfo);
    memset(destroyed, 0, sizeof destroyed);
    RDNA4R.lim.maxSamplerAllocationCount = 64;
    RDNA4R.samplerMirrorClamp = NO;
}

static void expectRejected(GarnetDevice *device,
                           MTLSamplerDescriptor *descriptor,
                           const char *name) {
    unsigned beforeAttempts = attempts;
    id state = [device newSamplerStateWithDescriptor:descriptor];
    char message[160];
    snprintf(message, sizeof message,
        "%s returned a sampler or reached Vulkan", name);
    require(state == nil && attempts == beforeAttempts &&
        allocationCount() == 0 && registryConfigCount() == 0,
        message);
    printf("SAMPLER_API_REJECT_OK name=%s vk_create_calls=0\n", name);
}

static void testInvalidDescriptors(GarnetDevice *device) {
    MTLSamplerDescriptor *descriptor = validDescriptor();
    descriptor.minFilter = (MTLSamplerMinMagFilter)2;
    expectRejected(device, descriptor, "invalid-min-filter");

    descriptor = validDescriptor();
    descriptor.magFilter = (MTLSamplerMinMagFilter)2;
    expectRejected(device, descriptor, "invalid-mag-filter");

    descriptor = validDescriptor();
    descriptor.mipFilter = (MTLSamplerMipFilter)3;
    expectRejected(device, descriptor, "invalid-mip-filter");

    descriptor = validDescriptor();
    descriptor.sAddressMode = (MTLSamplerAddressMode)invalidAddressMode;
    require((NSUInteger)descriptor.sAddressMode == invalidAddressMode,
        "invalid address mode was normalized by the descriptor setter");
    expectRejected(device, descriptor, "invalid-address-mode");

    descriptor = validDescriptor();
    descriptor.borderColor = (MTLSamplerBorderColor)99;
    expectRejected(device, descriptor, "invalid-border-color");

    descriptor = invalidCompareDescriptor();
    require((NSInteger)descriptor.compareFunction == invalidCompareFunction,
        "invalid compare function getter override was not preserved");
    expectRejected(device, descriptor, "invalid-compare-function");

    descriptor = validDescriptor();
    descriptor.maxAnisotropy = (NSUInteger)UINT_MAX + 1;
    expectRejected(device, descriptor, "anisotropy-overflow");

    require(attempts == 0 && normalAttempts == 0 && comparisonAttempts == 0 &&
        creates == 0 && allocationCount() == 0 && registryConfigCount() == 0,
        "invalid descriptors changed backend or registry state");
}

static void testPublicCreationFailureAndRetry(GarnetDevice *device) {
    @autoreleasepool {
        failNext = YES;
        id failed = [device newSamplerStateWithDescriptor:validDescriptor()];
        require(failed == nil && attempts == 1 && normalAttempts == 1 &&
            comparisonAttempts == 0 && creates == 0 && allocationCount() == 0 &&
            registryConfigCount() == 0,
            "public API published a failed normal sampler");

        id state = [device newSamplerStateWithDescriptor:validDescriptor()];
        require(state != nil && attempts == 2 && normalAttempts == 2 &&
            comparisonAttempts == 0 && creates == 1 && allocationCount() == 1 &&
            registryConfigCount() == 1,
            "public API retry did not create one normal sampler");
        VkSampler handle = [(RDNA4SamplerState *)state vkSampler];
        require(handle != VK_NULL_HANDLE && lastInfo.compareEnable == VK_FALSE &&
            lastInfo.unnormalizedCoordinates == VK_FALSE &&
            lastInfo.mipLodBias == 0.0f,
            "public API normal sampler state");
        state = nil;
        require(allocationCount() == 0 && registryConfigCount() == 0 &&
            destroys == 1 && destroyed[0] == handle,
            "public API retry lifecycle leaked or double-destroyed its handle");
    }
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
        resetMockAndRegistry();

        GarnetDevice *device = [GarnetDevice new];
        require(device != nil, "cannot allocate the public device facade");
        testInvalidDescriptors(device);
        testPublicCreationFailureAndRetry(device);
        device = nil;
        require(attempts == 2 && normalAttempts == 2 && comparisonAttempts == 0 &&
            creates == 1 && destroys == 1 && allocationCount() == 0 &&
            registryConfigCount() == 0,
            "final public API rejection counters");
        printf("SAMPLER_API_REJECT_PASS invalid_enums=6 anisotropy_overflow=1 "
               "invalid_vk_calls=0 initial_failure_rollback=1 retry=1 normal_handles=1 "
               "comparison_covered_by_runtime=1 checks=%u "
               "simulated_vulkan=1 gpu_execution=0\n", checks);
    }
    return 0;
}
