// Host-side sampler binding fixture for the tier-2 binding model.
//
// This file includes the real GarnetDevice.m. It mocks only Vulkan sampler
// creation/destruction and installs a small command-buffer probe into real
// encoder objects through test-only categories. That keeps encoder setup out
// of the Vulkan render-pass path while still calling the production setters.
//
// Build standalone with -DRDNA4_SAMPLER_BINDING_ORACLE_ONLY to run only the
// fixed CPU texture oracle.

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned oracleCases;

static int require(int ok, const char *message) {
    checks++;
    if (!ok) {
        fprintf(stderr, "SAMPLER_BINDING_STATE_FAIL %s\n", message);
        exit(1);
    }
    return 1;
}

#include "test-sampler-pattern-oracle.h"

#ifndef RDNA4_SAMPLER_BINDING_ORACLE_ONLY

#include "GarnetDevice.m"

static unsigned bindingCases;
static unsigned setterCases;
static unsigned mappingCases;
static unsigned wrappedRejects;

@interface RDNA4BindingStateProbe : NSObject {
@public
    unsigned argumentRetainCount;
    unsigned generalRetainCount;
    unsigned failureCount;
    BOOL failed;
    NSError *failureError;
    id __unsafe_unretained lastArgument;
}
- (void)rdna4RetainArgument:(id)object;
- (void)rdna4Retain:(id)object;
- (void)rdna4Fail:(NSString *)why;
- (NSError *)rdna4Error;
@end

@implementation RDNA4BindingStateProbe
- (void)rdna4RetainArgument:(id)object {
    argumentRetainCount++;
    lastArgument = object;
}
- (void)rdna4Retain:(id)object {
    (void)object;
    generalRetainCount++;
}
- (void)rdna4Fail:(NSString *)why {
    (void)why;
    failureCount++;
    failed = YES;
    if (!failureError) failureError = [NSError new];
}
- (NSError *)rdna4Error { return failed ? failureError : nil; }
@end

@interface RDNA4RenderEncoder (BindingStateTestAccess)
- (void)rdna4BindingTestInstallCommandBuffer:(id)commandBuffer;
- (RDNA4StageState *)rdna4BindingTestFragmentStage;
- (RDNA4StageState *)rdna4BindingTestVertexStage;
- (NSUInteger)rdna4BindingTestBindingCount;
@end

@implementation RDNA4RenderEncoder (BindingStateTestAccess)
- (void)rdna4BindingTestInstallCommandBuffer:(id)commandBuffer {
    _cb = (RDNA4CommandBuffer *)commandBuffer;
}
- (RDNA4StageState *)rdna4BindingTestFragmentStage { return &_sf; }
- (RDNA4StageState *)rdna4BindingTestVertexStage { return &_sv; }
- (NSUInteger)rdna4BindingTestBindingCount { return _samplerBindings.count; }
@end

@interface RDNA4ComputeEncoder (BindingStateTestAccess)
- (void)rdna4BindingTestInstallCommandBuffer:(id)commandBuffer;
- (RDNA4StageState *)rdna4BindingTestStage;
- (NSUInteger)rdna4BindingTestBindingCount;
@end

@implementation RDNA4ComputeEncoder (BindingStateTestAccess)
- (void)rdna4BindingTestInstallCommandBuffer:(id)commandBuffer {
    _cb = (RDNA4CommandBuffer *)commandBuffer;
}
- (RDNA4StageState *)rdna4BindingTestStage { return &_st; }
- (NSUInteger)rdna4BindingTestBindingCount { return _samplerBindings.count; }
@end

@interface RDNA4RenderEncoder (BindingStateBatchLODTestAccess)
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)samplers
                  lodMinClamps:(const float *)lodMinClamps
                  lodMaxClamps:(const float *)lodMaxClamps
                      withRange:(NSRange)range;
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)samplers
                    lodMinClamps:(const float *)lodMinClamps
                    lodMaxClamps:(const float *)lodMaxClamps
                        withRange:(NSRange)range;
@end

@interface RDNA4ComputeEncoder (BindingStateBatchLODTestAccess)
- (void)setSamplerStates:(const id __unsafe_unretained *)samplers
            lodMinClamps:(const float *)lodMinClamps
            lodMaxClamps:(const float *)lodMaxClamps
                withRange:(NSRange)range;
@end

@interface RDNA4SamplerState (BindingStateTestAccess)
- (RDNA4SamplerStorage *)rdna4BindingTestStorage;
@end

@implementation RDNA4SamplerState (BindingStateTestAccess)
- (RDNA4SamplerStorage *)rdna4BindingTestStorage { return _storage; }
@end

enum { MaxMockSamplers = 256 };

static unsigned attempts;
static unsigned creates;
static unsigned normalCreates;
static unsigned comparisonCreates;
static unsigned destroys;
static unsigned lifetimeCreates;
static unsigned lifetimeDestroys;
static unsigned biasViolations;
static unsigned nextHandle;
static BOOL failNext;
static VkSamplerCreateInfo createInfos[MaxMockSamplers];
static VkSampler destroyed[MaxMockSamplers];

static NSUInteger allocationCount(void) {
    return (NSUInteger)atomic_load(&rdna4_sampler_allocations);
}

static NSUInteger registryConfigCount(void) {
    @synchronized ([RDNA4SamplerStorage class]) {
        return rdna4_sampler_configs ? [rdna4_sampler_configs count] : 0;
    }
}

static NSUInteger argumentConfigCount(void) {
    @synchronized ([RDNA4SamplerStorage class]) {
        return rdna4_argument_sampler_configs;
    }
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSampler *sampler) {
    (void)device;
    (void)allocator;
    require(info != NULL && sampler != NULL, "mock create arguments");
    require(attempts < MaxMockSamplers, "mock create history overflow");
    VkSamplerCreateInfo copy = *info;
    createInfos[attempts++] = copy;
    if (copy.mipLodBias != 0.0f) biasViolations++;
    if (copy.compareEnable) comparisonCreates++;
    else normalCreates++;
    if (failNext) {
        failNext = NO;
        *sampler = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *sampler = (VkSampler)(uintptr_t)(0x9000u + nextHandle++);
    creates++;
    lifetimeCreates++;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
                                      const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(sampler != VK_NULL_HANDLE, "mock destroy received a null sampler");
    require(destroys < MaxMockSamplers, "mock destroy history overflow");
    for (unsigned i = 0; i < destroys; ++i)
        require(destroyed[i] != sampler, "sampler destroyed twice");
    destroyed[destroys++] = sampler;
    lifetimeDestroys++;
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
    attempts = creates = normalCreates = comparisonCreates = destroys = 0;
    failNext = NO;
    memset(createInfos, 0, sizeof createInfos);
    memset(destroyed, 0, sizeof destroyed);
    RDNA4R.lim.maxSamplerAnisotropy = 16.0f;
    RDNA4R.lim.maxSamplerAllocationCount = 256;
    RDNA4R.samplerAnisotropy = YES;
    RDNA4R.samplerMirrorClamp = NO;
}

static MTLSamplerDescriptor *bindingDescriptor(void) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.label = @"binding-state";
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.mipFilter = MTLSamplerMipFilterLinear;
    descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeMirrorRepeat;
    descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite;
    descriptor.compareFunction = MTLCompareFunctionLess;
    descriptor.lodMinClamp = 0.25;
    descriptor.lodMaxClamp = 12.5;
    descriptor.maxAnisotropy = 4;
    descriptor.supportArgumentBuffers = NO;
    return descriptor;
}

static RDNA4SamplerState *newSamplerState(MTLSamplerDescriptor *descriptor) {
    NSError *error = nil;
    RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
        initWithDevice:nil descriptor:descriptor error:&error];
    require(state != nil && error == nil, "sampler state creation failed");
    return state;
}

static RDNA4RenderEncoder *newRenderTestEncoder(RDNA4BindingStateProbe *probe) {
    RDNA4RenderEncoder *encoder = [[RDNA4RenderEncoder alloc] init];
    require(encoder != nil, "render test encoder allocation failed");
    [encoder rdna4BindingTestInstallCommandBuffer:probe];
    return encoder;
}

static RDNA4ComputeEncoder *newComputeTestEncoder(RDNA4BindingStateProbe *probe) {
    RDNA4ComputeEncoder *encoder = [[RDNA4ComputeEncoder alloc] init];
    require(encoder != nil, "compute test encoder allocation failed");
    [encoder rdna4BindingTestInstallCommandBuffer:probe];
    return encoder;
}

static int bindingHasLODClamps(RDNA4SamplerBinding *binding, float min, float max) {
    RDNA4SamplerStorage *storage = [binding rdna4Storage];
    return storage && storage->config.lodMin == min && storage->config.lodMax == max;
}

static void requireMockAndRegistryEmpty(void) {
    require(allocationCount() == 0 && registryConfigCount() == 0 &&
        argumentConfigCount() == 0,
        "sampler registry, argument config or allocation count leaked");
}

static void testVariantStorageRepresentations(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        MTLSamplerDescriptor *descriptor = bindingDescriptor();
        descriptor.supportArgumentBuffers = YES;
        RDNA4SamplerState *state = newSamplerState(descriptor);
        RDNA4SamplerStorage *baseStorage = [state rdna4BindingTestStorage];
        VkSampler baseHandle = [state vkSampler];
        require(baseStorage != nil && baseHandle != VK_NULL_HANDLE,
            "base sampler storage");
        NSUInteger argumentConfigsBefore = argumentConfigCount();
        __unsafe_unretained RDNA4SamplerStorage *identityBefore =
            [state rdna4ArgumentIdentity];
        uint64_t tokenBefore = rdna4_argument_resource_token(state, nil, NO);
        require(argumentConfigsBefore == 1 && identityBefore == baseStorage &&
            tokenBefore != 0,
            "variant test did not establish the base argument identity");

        RDNA4SamplerBinding *baseBinding = [state rdna4Binding:&error];
        require(baseBinding != nil && error == nil &&
            [baseBinding rdna4Storage] == baseStorage &&
            [baseBinding vkSampler] == baseHandle,
            "base binding does not share the base storage");
        require(createInfos[0].mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR,
            "base mip map mode is not linear");

        unsigned firstCreate = attempts;
        RDNA4SamplerBinding *first = [state
            rdna4BindingWithLODMinClamp:1.0f lodMaxClamp:3.0f error:&error];
        RDNA4SamplerBinding *same = [state
            rdna4BindingWithLODMinClamp:1.0f lodMaxClamp:3.0f error:&error];
        require(attempts == firstCreate + 1 &&
            createInfos[firstCreate].minLod == 1.0f &&
            createInfos[firstCreate].maxLod == 3.0f,
            "first variant backend LOD clamps do not match its override");
        unsigned otherCreate = attempts;
        RDNA4SamplerBinding *other = [state
            rdna4BindingWithLODMinClamp:2.0f lodMaxClamp:4.0f error:&error];
        require(attempts == otherCreate + 1 &&
            createInfos[otherCreate].minLod == 2.0f &&
            createInfos[otherCreate].maxLod == 4.0f,
            "second variant backend LOD clamps do not match its override");
        require(first != nil && same != nil && other != nil && error == nil,
            "per-binding LOD variant creation failed");
        require(bindingHasLODClamps(first, 1.0f, 3.0f) &&
            bindingHasLODClamps(same, 1.0f, 3.0f) &&
            bindingHasLODClamps(other, 2.0f, 4.0f),
            "variant storage does not carry the binding LOD clamps");
        require([first rdna4Storage] == [same rdna4Storage] &&
            [first vkSampler] == [same vkSampler],
            "equal LOD clamps did not reuse storage and normal handle");
        require([other rdna4Storage] != [first rdna4Storage] &&
            [other vkSampler] != [first vkSampler],
            "different LOD clamps reused one variant");
        require(baseStorage->normal == baseHandle &&
            baseStorage->comparison == VK_NULL_HANDLE,
            "base storage representation fields changed");

        unsigned comparisonCreate = attempts;
        VkSampler comparison = [first rdna4SamplerComparison:YES error:&error];
        require(comparison != VK_NULL_HANDLE && error == nil &&
            [same rdna4SamplerComparison:YES error:&error] == comparison &&
            [first rdna4Storage]->comparison == comparison &&
            attempts == comparisonCreate + 1 &&
            createInfos[comparisonCreate].compareEnable == VK_TRUE &&
            createInfos[comparisonCreate].minLod == 1.0f &&
            createInfos[comparisonCreate].maxLod == 3.0f,
            "comparison representation did not come from the variant storage");
        unsigned noCreate = attempts;
        require([first rdna4SamplerComparison:YES
            normalizedCoordinates:YES error:&error] == comparison &&
            attempts == noCreate &&
            [first rdna4Storage]->comparison == comparison &&
            bindingHasLODClamps(first, 1.0f, 3.0f),
            "normalized variant binding did not reuse its comparison storage");
        require([first rdna4SamplerComparison:NO error:&error] == [first vkSampler] &&
            [first rdna4Storage]->normal == [first vkSampler],
            "normal representation did not come from the variant storage");
        require(attempts == 4 && creates == 4 &&
            normalCreates == 3 && comparisonCreates == 1,
            "variant sampler creation count");
        require(biasViolations == 0, "a creation used a non-zero mipLodBias");
        require(argumentConfigCount() == argumentConfigsBefore &&
            [state rdna4ArgumentIdentity] == identityBefore &&
            rdna4_argument_resource_token(state, nil, NO) == tokenBefore,
            "variant creation/reuse changed the argument config or token");
        bindingCases += 6;

        first = nil;
        same = nil;
        other = nil;
        baseBinding = nil;
        require(argumentConfigCount() == argumentConfigsBefore &&
            [state rdna4ArgumentIdentity] == identityBefore &&
            rdna4_argument_resource_token(state, nil, NO) == tokenBefore,
            "variant destruction changed the base argument identity");
        state = nil;
    }
    require(argumentConfigCount() == 0,
        "argument config count did not return to zero after variant teardown");
    require(lifetimeCreates == lifetimeDestroys,
        "variant handles were not all destroyed");
    requireMockAndRegistryEmpty();
}

static void testBackendLODBounds(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        MTLSamplerDescriptor *baseDescriptor = bindingDescriptor();
        baseDescriptor.lodMinClamp = 2000.0;
        baseDescriptor.lodMaxClamp = 3000.0;
        unsigned baseCreate = attempts;
        RDNA4SamplerState *base = newSamplerState(baseDescriptor);
        RDNA4SamplerStorage *baseStorage = [base rdna4BindingTestStorage];
        require(baseStorage != nil && baseStorage->config.lodMin == 2000.0f &&
            baseStorage->config.lodMax == 3000.0f &&
            attempts == baseCreate + 1 &&
            createInfos[baseCreate].mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR &&
            createInfos[baseCreate].minLod == 1000.0f &&
            createInfos[baseCreate].maxLod == 1000.0f &&
            createInfos[baseCreate].minLod <= createInfos[baseCreate].maxLod,
            "base LOD above the backend bound was not clamped consistently");
        unsigned publicComparisonCreate = attempts;
        VkSampler baseComparison = [base rdna4SamplerComparison:YES error:&error];
        require(baseComparison != VK_NULL_HANDLE && !error &&
            attempts == publicComparisonCreate + 1 &&
            createInfos[publicComparisonCreate].compareEnable == VK_TRUE &&
            createInfos[publicComparisonCreate].minLod == 1000.0f &&
            createInfos[publicComparisonCreate].maxLod == 1000.0f &&
            createInfos[publicComparisonCreate].mipLodBias == 0,
            "public comparison LOD bounds differ from the ordinary representation");
        unsigned comparisonAttemptsBefore = attempts;
        require([base rdna4SamplerComparison:YES normalizedCoordinates:YES
            error:&error] == baseComparison && !error &&
            attempts == comparisonAttemptsBefore,
            "normalized public comparison did not reuse bounded storage");

        RDNA4SamplerState *overrideBase = newSamplerState(bindingDescriptor());
        unsigned overrideCreate = attempts;
        RDNA4SamplerBinding *override = [overrideBase
            rdna4BindingWithLODMinClamp:2000.0f lodMaxClamp:3000.0f error:&error];
        RDNA4SamplerStorage *overrideStorage = [override rdna4Storage];
        require(override != nil && error == nil && overrideStorage != nil &&
            overrideStorage->config.lodMin == 2000.0f &&
            overrideStorage->config.lodMax == 3000.0f &&
            overrideStorage == baseStorage && [override vkSampler] == [base vkSampler] &&
            attempts == overrideCreate &&
            [override rdna4SamplerComparison:YES error:&error] == baseComparison &&
            attempts == overrideCreate,
            "scalar LOD override did not reuse the identical bounded configuration");
        bindingCases += 2;
        override = nil;
        overrideBase = nil;
        base = nil;
    }
    require(lifetimeCreates == lifetimeDestroys,
        "backend LOD bound test leaked a sampler");
    requireMockAndRegistryEmpty();
}

static void testSetterAtomicUpdates(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *state = newSamplerState(bindingDescriptor());
        RDNA4SamplerBinding *old = [state rdna4Binding:&error];
        require(old != nil && error == nil, "initial binding for setter test");

        RDNA4BindingStateProbe *probe = [RDNA4BindingStateProbe new];
        RDNA4RenderEncoder *render = newRenderTestEncoder(probe);
        RDNA4StageState *fragment = [render rdna4BindingTestFragmentStage];
        fragment->smp[5] = old;
        unsigned beforeAttempts = attempts;
        [render setFragmentSamplerState:state lodMinClamp:1.0f
            lodMaxClamp:3.0f atIndex:5];
        require(fragment->smp[5] != NULL && fragment->smp[5] != old &&
            bindingHasLODClamps(fragment->smp[5], 1.0f, 3.0f),
            "render fragment setter did not install a LOD variant");
        require(attempts == beforeAttempts + 1 &&
            createInfos[beforeAttempts].minLod == 1.0f &&
            createInfos[beforeAttempts].maxLod == 3.0f &&
            probe->argumentRetainCount == 1 && probe->generalRetainCount == 0 &&
            [render rdna4BindingTestBindingCount] == 1,
            "render setter backend LOD, retention or ownership mismatch");

        beforeAttempts = attempts;
        [render setFragmentSamplerState:state lodMinClamp:2.0f
            lodMaxClamp:4.0f atIndex:5];
        require(fragment->smp[5] != NULL &&
            bindingHasLODClamps(fragment->smp[5], 2.0f, 4.0f) &&
            attempts == beforeAttempts + 1 &&
            createInfos[beforeAttempts].minLod == 2.0f &&
            createInfos[beforeAttempts].maxLod == 4.0f,
            "render setter replacement or backend LOD mismatch");
        require(probe->argumentRetainCount == 2 &&
            probe->generalRetainCount == 0 &&
            [render rdna4BindingTestBindingCount] == 1,
            "slot replacement grew the encoder binding set or command-buffer refs");

        [render setFragmentSamplerState:nil lodMinClamp:0.0f
            lodMaxClamp:0.0f atIndex:5];
        require(fragment->smp[5] == NULL &&
            probe->argumentRetainCount == 2 &&
            probe->generalRetainCount == 0 &&
            [render rdna4BindingTestBindingCount] == 0,
            "render nil setter did not clear the slot and scoped binding");

        static const struct {
            float min, max;
            const char *name;
        } invalid[] = {
            {NAN, 1.0f, "NaN-min"},
            {0.0f, INFINITY, "positive-infinity-max"},
            {-INFINITY, 1.0f, "negative-infinity-min"},
            {-1.0f, 3.0f, "negative-min"},
            {4.0f, 3.0f, "min-greater-than-max"},
        };
        unsigned registryBefore = (unsigned)registryConfigCount();
        NSUInteger allocationBefore = allocationCount();
        for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
            RDNA4BindingStateProbe *badProbe = [RDNA4BindingStateProbe new];
            RDNA4RenderEncoder *badEncoder = newRenderTestEncoder(badProbe);
            RDNA4StageState *badStage = [badEncoder rdna4BindingTestFragmentStage];
            badStage->smp[5] = old;
            beforeAttempts = attempts;
            [badEncoder setFragmentSamplerState:state lodMinClamp:invalid[i].min
                lodMaxClamp:invalid[i].max atIndex:5];
            require(badStage->smp[5] == old,
                invalid[i].name);
            require(attempts == beforeAttempts &&
                registryConfigCount() == registryBefore &&
                allocationCount() == allocationBefore &&
                badProbe->argumentRetainCount == 0 &&
                badProbe->generalRetainCount == 0 &&
                badProbe->failureCount != 0 &&
                [badEncoder rdna4BindingTestBindingCount] == 0,
                "invalid clamps changed backend, registry or slot state");
            setterCases++;
            badEncoder = nil;
            badProbe = nil;
        }

        failNext = YES;
        RDNA4BindingStateProbe *failProbe = [RDNA4BindingStateProbe new];
        RDNA4RenderEncoder *failEncoder = newRenderTestEncoder(failProbe);
        RDNA4StageState *failStage = [failEncoder rdna4BindingTestFragmentStage];
        failStage->smp[5] = old;
        beforeAttempts = attempts;
        unsigned createsBefore = creates;
        [failEncoder setFragmentSamplerState:state lodMinClamp:5.0f
            lodMaxClamp:6.0f atIndex:5];
        require(failStage->smp[5] == old && attempts == beforeAttempts + 1 &&
            creates == createsBefore && registryConfigCount() == registryBefore &&
            allocationCount() == allocationBefore &&
            failProbe->argumentRetainCount == 0 &&
            failProbe->generalRetainCount == 0,
            "backend failure was not atomic");
        setterCases++;
        failEncoder = nil;
        failProbe = nil;

        RDNA4BindingStateProbe *computeProbe = [RDNA4BindingStateProbe new];
        RDNA4ComputeEncoder *compute = newComputeTestEncoder(computeProbe);
        RDNA4StageState *computeStage = [compute rdna4BindingTestStage];
        computeStage->smp[7] = old;
        beforeAttempts = attempts;
        [compute setSamplerState:state lodMinClamp:1.0f
            lodMaxClamp:3.0f atIndex:7];
        require(computeStage->smp[7] != NULL &&
            bindingHasLODClamps(computeStage->smp[7], 1.0f, 3.0f) &&
            attempts == beforeAttempts + 1 &&
            createInfos[beforeAttempts].minLod == 1.0f &&
            createInfos[beforeAttempts].maxLod == 3.0f &&
            computeProbe->argumentRetainCount == 1 &&
            computeProbe->generalRetainCount == 0 &&
            [compute rdna4BindingTestBindingCount] == 1,
            "compute setter legal path or backend LOD mismatch");
        setterCases++;
        [compute setSamplerState:nil lodMinClamp:0.0f
            lodMaxClamp:0.0f atIndex:7];
        require(computeStage->smp[7] == NULL &&
            computeProbe->argumentRetainCount == 1 &&
            computeProbe->generalRetainCount == 0 &&
            [compute rdna4BindingTestBindingCount] == 0,
            "compute nil setter did not clear the scoped binding");
        compute = nil;

        RDNA4BindingStateProbe *badComputeProbe = [RDNA4BindingStateProbe new];
        RDNA4ComputeEncoder *badCompute = newComputeTestEncoder(badComputeProbe);
        RDNA4StageState *badComputeStage = [badCompute rdna4BindingTestStage];
        badComputeStage->smp[7] = old;
        beforeAttempts = attempts;
        [badCompute setSamplerState:state lodMinClamp:NAN
            lodMaxClamp:1.0f atIndex:7];
        require(badComputeStage->smp[7] == old && attempts == beforeAttempts &&
            badComputeProbe->argumentRetainCount == 0 &&
            badComputeProbe->generalRetainCount == 0 &&
            [badCompute rdna4BindingTestBindingCount] == 0,
            "compute setter rejected a bad clamp without keeping the old slot");
        setterCases++;

        badCompute = nil;
        badComputeProbe = nil;
        computeProbe = nil;
        render = nil;
        probe = nil;
        old = nil;
        state = nil;
    }
    require(lifetimeCreates == lifetimeDestroys,
        "setter-test handles were not all destroyed");
    requireMockAndRegistryEmpty();
}

static void runRenderBatchLODChecks(BOOL fragment) {
    RDNA4BindingStateProbe *probe = [RDNA4BindingStateProbe new];
    RDNA4RenderEncoder *render = newRenderTestEncoder(probe);
    RDNA4StageState *stage = fragment
        ? [render rdna4BindingTestFragmentStage]
        : [render rdna4BindingTestVertexStage];
    RDNA4SamplerState *state = newSamplerState(bindingDescriptor());

    // Seed two slots through the scalar setter, then cover all three slots
    // with one batch. Replaying the batch must replace, not accumulate,
    // active binding wrappers.
    if (fragment) {
        [render setFragmentSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:4];
        [render setFragmentSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:6];
    } else {
        [render setVertexSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:4];
        [render setVertexSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:6];
    }

    id __unsafe_unretained samplers[3] = {state, state, state};
    float mins[3] = {1.0f, 2.0f, 3.0f};
    float maxs[3] = {3.0f, 4.0f, 5.0f};
    NSRange range = NSMakeRange(4, 3);
    unsigned createsBefore = creates;
    unsigned batchCreate = attempts;
    if (fragment)
        [render setFragmentSamplerStates:samplers lodMinClamps:mins
            lodMaxClamps:maxs withRange:range];
    else
        [render setVertexSamplerStates:samplers lodMinClamps:mins
            lodMaxClamps:maxs withRange:range];
    require(stage->smp[4] && stage->smp[5] && stage->smp[6] &&
        bindingHasLODClamps(stage->smp[4], 1.0f, 3.0f) &&
        bindingHasLODClamps(stage->smp[5], 2.0f, 4.0f) &&
        bindingHasLODClamps(stage->smp[6], 3.0f, 5.0f) &&
        attempts == batchCreate + 3 &&
        createInfos[batchCreate].minLod == 1.0f &&
        createInfos[batchCreate].maxLod == 3.0f &&
        createInfos[batchCreate + 1].minLod == 2.0f &&
        createInfos[batchCreate + 1].maxLod == 4.0f &&
        createInfos[batchCreate + 2].minLod == 3.0f &&
        createInfos[batchCreate + 2].maxLod == 5.0f,
        "render LOD batch did not install all legal clamps");
    require([render rdna4BindingTestBindingCount] == 3,
        "render LOD batch active-set count");

    unsigned createsAfterFirst = creates;
    for (unsigned replay = 0; replay < 8; ++replay) {
        if (fragment)
            [render setFragmentSamplerStates:samplers lodMinClamps:mins
                lodMaxClamps:maxs withRange:range];
        else
            [render setVertexSamplerStates:samplers lodMinClamps:mins
                lodMaxClamps:maxs withRange:range];
    }
    require(creates == createsAfterFirst &&
        [render rdna4BindingTestBindingCount] == 3,
        "render LOD batch replay grew storage or active set");
    require(createsAfterFirst >= createsBefore,
        "render LOD batch creation accounting");

    RDNA4SamplerBinding *old4 = stage->smp[4];
    RDNA4SamplerBinding *old5 = stage->smp[5];
    RDNA4SamplerBinding *old6 = stage->smp[6];
    float badMins[3] = {6.0f, NAN, 7.0f};
    float badMaxs[3] = {8.0f, 8.0f, 9.0f};
    unsigned failuresBefore = probe->failureCount;
    unsigned retainsBefore = probe->argumentRetainCount;
    unsigned generalRetainsBefore = probe->generalRetainCount;
    unsigned badCreate = attempts;
    if (fragment)
        [render setFragmentSamplerStates:samplers lodMinClamps:badMins
            lodMaxClamps:badMaxs withRange:range];
    else
        [render setVertexSamplerStates:samplers lodMinClamps:badMins
            lodMaxClamps:badMaxs withRange:range];
    require(stage->smp[4] != old4 &&
        bindingHasLODClamps(stage->smp[4], 6.0f, 8.0f) &&
        createInfos[badCreate].minLod == 6.0f &&
        createInfos[badCreate].maxLod == 8.0f &&
        attempts == badCreate + 1 &&
        probe->argumentRetainCount == retainsBefore + 1 &&
        probe->generalRetainCount == generalRetainsBefore &&
        stage->smp[5] == old5 &&
        stage->smp[6] == old6 &&
        probe->failureCount == failuresBefore + 1 &&
        [render rdna4BindingTestBindingCount] == 3,
        "render LOD batch invalid slot was not isolated");
    setterCases++;

    render = nil;
    probe = nil;
    state = nil;
}

static void testBatchLODSetters(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        runRenderBatchLODChecks(NO);
        runRenderBatchLODChecks(YES);
    }
    requireMockAndRegistryEmpty();

    resetMockAndRegistry();
    @autoreleasepool {
        RDNA4BindingStateProbe *probe = [RDNA4BindingStateProbe new];
        RDNA4ComputeEncoder *compute = newComputeTestEncoder(probe);
        RDNA4StageState *stage = [compute rdna4BindingTestStage];
        RDNA4SamplerState *state = newSamplerState(bindingDescriptor());

        [compute setSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:2];
        [compute setSamplerState:state lodMinClamp:0.0f
            lodMaxClamp:1.0f atIndex:4];

        id __unsafe_unretained samplers[3] = {state, state, state};
        float mins[3] = {1.0f, 2.0f, 3.0f};
        float maxs[3] = {3.0f, 4.0f, 5.0f};
        NSRange range = NSMakeRange(2, 3);
        unsigned createsAfterFirst = creates;
        unsigned batchCreate = attempts;
        [compute setSamplerStates:samplers lodMinClamps:mins
            lodMaxClamps:maxs withRange:range];
        require(stage->smp[2] && stage->smp[3] && stage->smp[4] &&
            bindingHasLODClamps(stage->smp[2], 1.0f, 3.0f) &&
            bindingHasLODClamps(stage->smp[3], 2.0f, 4.0f) &&
            bindingHasLODClamps(stage->smp[4], 3.0f, 5.0f) &&
            attempts == batchCreate + 3 &&
            createInfos[batchCreate].minLod == 1.0f &&
            createInfos[batchCreate].maxLod == 3.0f &&
            createInfos[batchCreate + 1].minLod == 2.0f &&
            createInfos[batchCreate + 1].maxLod == 4.0f &&
            createInfos[batchCreate + 2].minLod == 3.0f &&
            createInfos[batchCreate + 2].maxLod == 5.0f,
            "compute LOD batch did not install all legal clamps");
        require([compute rdna4BindingTestBindingCount] == 3,
            "compute LOD batch active-set count");

        unsigned createsAfterBatch = creates;
        for (unsigned replay = 0; replay < 8; ++replay)
            [compute setSamplerStates:samplers lodMinClamps:mins
                lodMaxClamps:maxs withRange:range];
        require(creates == createsAfterBatch &&
            [compute rdna4BindingTestBindingCount] == 3,
            "compute LOD batch replay grew storage or active set");
        require(createsAfterBatch >= createsAfterFirst,
            "compute LOD batch creation accounting");

        RDNA4SamplerBinding *old2 = stage->smp[2];
        RDNA4SamplerBinding *old3 = stage->smp[3];
        RDNA4SamplerBinding *old4 = stage->smp[4];
        float badMins[3] = {6.0f, NAN, 7.0f};
        float badMaxs[3] = {8.0f, 8.0f, 9.0f};
        unsigned failuresBefore = probe->failureCount;
        unsigned retainsBefore = probe->argumentRetainCount;
        unsigned generalRetainsBefore = probe->generalRetainCount;
        unsigned badCreate = attempts;
        [compute setSamplerStates:samplers lodMinClamps:badMins
            lodMaxClamps:badMaxs withRange:range];
        require(stage->smp[2] != old2 &&
            bindingHasLODClamps(stage->smp[2], 6.0f, 8.0f) &&
            createInfos[badCreate].minLod == 6.0f &&
            createInfos[badCreate].maxLod == 8.0f &&
            attempts == badCreate + 1 &&
            probe->argumentRetainCount == retainsBefore + 1 &&
            probe->generalRetainCount == generalRetainsBefore &&
            stage->smp[3] == old3 &&
            stage->smp[4] == old4 &&
            probe->failureCount == failuresBefore + 1 &&
            [compute rdna4BindingTestBindingCount] == 3,
            "compute LOD batch invalid slot was not isolated");
        setterCases++;

        compute = nil;
        probe = nil;
        state = nil;
    }
    requireMockAndRegistryEmpty();
}

static void testDescriptorMapping(void) {
    static const VkFilter expectedFilter[2] = {
        VK_FILTER_NEAREST, VK_FILTER_LINEAR,
    };
    static const VkSamplerMipmapMode expectedMip[3] = {
        VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_MIPMAP_MODE_LINEAR,
    };
    static const VkSamplerAddressMode expectedAddress[6] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    };

    for (unsigned minFilter = 0; minFilter < 2; ++minFilter)
        for (unsigned magFilter = 0; magFilter < 2; ++magFilter)
            for (unsigned mipFilter = 0; mipFilter < 3; ++mipFilter) {
                resetMockAndRegistry();
                @autoreleasepool {
                    MTLSamplerDescriptor *descriptor = bindingDescriptor();
                    descriptor.minFilter = (MTLSamplerMinMagFilter)minFilter;
                    descriptor.magFilter = (MTLSamplerMinMagFilter)magFilter;
                    descriptor.mipFilter = (MTLSamplerMipFilter)mipFilter;
                    RDNA4SamplerState *state = newSamplerState(descriptor);
                    (void)state;
                    require(createInfos[0].minFilter == expectedFilter[minFilter] &&
                        createInfos[0].magFilter == expectedFilter[magFilter] &&
                        createInfos[0].mipmapMode == expectedMip[mipFilter],
                        "filter or mip mapping");
                    state = nil;
                }
                requireMockAndRegistryEmpty();
                mappingCases++;
            }

    for (unsigned mode = 0; mode < 6; ++mode) {
        resetMockAndRegistry();
        @autoreleasepool {
            RDNA4R.samplerMirrorClamp =
                mode == MTLSamplerAddressModeMirrorClampToEdge;
            MTLSamplerDescriptor *descriptor = bindingDescriptor();
            descriptor.sAddressMode = (MTLSamplerAddressMode)mode;
            descriptor.tAddressMode = (MTLSamplerAddressMode)mode;
            descriptor.rAddressMode = (MTLSamplerAddressMode)mode;
            descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite;
            RDNA4SamplerState *state = newSamplerState(descriptor);
            (void)state;
            require(createInfos[0].addressModeU == expectedAddress[mode] &&
                createInfos[0].addressModeV == expectedAddress[mode] &&
                createInfos[0].addressModeW == expectedAddress[mode],
                "address mode mapping");
            if ((MTLSamplerAddressMode)mode == MTLSamplerAddressModeClampToZero)
                require(createInfos[0].borderColor ==
                    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                    "ClampToZero did not force transparent black");
            if ((MTLSamplerAddressMode)mode == MTLSamplerAddressModeClampToBorderColor)
                require(createInfos[0].borderColor ==
                    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
                    "ClampToBorderColor ignored the border color");
            state = nil;
        }
        requireMockAndRegistryEmpty();
        mappingCases++;
    }

    static const VkBorderColor expectedBorder[3] = {
        VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    for (unsigned border = 0; border < 3; ++border) {
        resetMockAndRegistry();
        @autoreleasepool {
            MTLSamplerDescriptor *descriptor = bindingDescriptor();
            descriptor.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
            descriptor.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
            descriptor.rAddressMode = MTLSamplerAddressModeClampToBorderColor;
            descriptor.borderColor = (MTLSamplerBorderColor)border;
            RDNA4SamplerState *state = newSamplerState(descriptor);
            (void)state;
            require(createInfos[0].borderColor == expectedBorder[border],
                "border color mapping");
            state = nil;
        }
        requireMockAndRegistryEmpty();
        mappingCases++;
    }

    for (unsigned compare = 0; compare < 8; ++compare) {
        resetMockAndRegistry();
        @autoreleasepool {
            NSError *error = nil;
            MTLSamplerDescriptor *descriptor = bindingDescriptor();
            descriptor.compareFunction = (MTLCompareFunction)compare;
            RDNA4SamplerState *state = newSamplerState(descriptor);
            require(createInfos[0].compareEnable == VK_FALSE,
                "ordinary sampler unexpectedly enabled comparison");
            VkSampler handle = [state rdna4SamplerComparison:YES error:&error];
            require(handle != VK_NULL_HANDLE && error == nil &&
                createInfos[1].compareEnable == VK_TRUE &&
                createInfos[1].compareOp == (VkCompareOp)compare,
                "comparison function mapping");
            state = nil;
        }
        requireMockAndRegistryEmpty();
        mappingCases++;
    }

    static const NSUInteger anisotropyValues[] = {1, 4, 8};
    for (unsigned i = 0; i < sizeof anisotropyValues / sizeof anisotropyValues[0]; ++i) {
        resetMockAndRegistry();
        @autoreleasepool {
            MTLSamplerDescriptor *descriptor = bindingDescriptor();
            descriptor.maxAnisotropy = anisotropyValues[i];
            RDNA4SamplerState *state = newSamplerState(descriptor);
            (void)state;
            require(createInfos[0].maxAnisotropy == (float)anisotropyValues[i] &&
                createInfos[0].anisotropyEnable ==
                    (anisotropyValues[i] > 1 ? VK_TRUE : VK_FALSE),
                "anisotropy mapping");
            state = nil;
        }
        requireMockAndRegistryEmpty();
        mappingCases++;
    }

    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        unsigned normalizedCreate = attempts;
        RDNA4SamplerState *normalized = newSamplerState(bindingDescriptor());
        require(attempts == normalizedCreate + 1 &&
            createInfos[normalizedCreate].unnormalizedCoordinates == VK_FALSE,
            "normalized sampler backend coordinate mode");
        RDNA4SamplerBinding *normalizedBinding = [normalized rdna4Binding:&error];
        require(![normalized rdna4PixelCoordinates] &&
            ![normalizedBinding rdna4PixelCoordinates],
            "normalized coordinate mapping");
        require([normalizedBinding rdna4SamplerComparison:NO
            normalizedCoordinates:YES error:&error] == [normalized vkSampler],
            "normalized binding did not reuse its normal handle");

        unsigned normalizedVariantCreate = attempts;
        RDNA4SamplerBinding *normalizedVariant = [normalized
            rdna4BindingWithLODMinClamp:2.0f lodMaxClamp:4.0f error:&error];
        require(normalizedVariant != nil && error == nil &&
            attempts == normalizedVariantCreate + 1 &&
            createInfos[normalizedVariantCreate].minLod == 2.0f &&
            createInfos[normalizedVariantCreate].maxLod == 4.0f,
            "normalized LOD variant backend clamps");
        unsigned normalizedVariantComparisonCreate = attempts;
        VkSampler normalizedVariantComparison = [normalizedVariant
            rdna4SamplerComparison:YES normalizedCoordinates:YES error:&error];
        require(normalizedVariantComparison != VK_NULL_HANDLE && error == nil &&
            attempts == normalizedVariantComparisonCreate + 1 &&
            createInfos[normalizedVariantComparisonCreate].compareEnable == VK_TRUE &&
            createInfos[normalizedVariantComparisonCreate].unnormalizedCoordinates == VK_FALSE &&
            createInfos[normalizedVariantComparisonCreate].minLod == 2.0f &&
            createInfos[normalizedVariantComparisonCreate].maxLod == 4.0f &&
            [normalizedVariant rdna4Storage]->comparison == normalizedVariantComparison &&
            bindingHasLODClamps(normalizedVariant, 2.0f, 4.0f),
            "normalized variant comparison representation");

        MTLSamplerDescriptor *pixelDescriptor = bindingDescriptor();
        pixelDescriptor.normalizedCoordinates = NO;
        pixelDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
        pixelDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
        pixelDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        pixelDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        pixelDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        pixelDescriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
        pixelDescriptor.maxAnisotropy = 1;
        pixelDescriptor.compareFunction = MTLCompareFunctionNever;
        unsigned pixelCreate = attempts;
        RDNA4SamplerState *pixel = newSamplerState(pixelDescriptor);
        RDNA4SamplerBinding *pixelBinding = [pixel rdna4Binding:&error];
        VkSampler pixelNormal = [pixel vkSampler];
        require([pixel rdna4PixelCoordinates] &&
            [pixelBinding rdna4PixelCoordinates] &&
            attempts == pixelCreate + 1 &&
            createInfos[pixelCreate].unnormalizedCoordinates == VK_TRUE,
            "pixel coordinate mapping");

        unsigned beforeAttempts = attempts;
        error = nil;
        require([pixelBinding rdna4SamplerComparison:YES error:&error] ==
            VK_NULL_HANDLE && error != nil && attempts == beforeAttempts &&
            [pixelBinding rdna4Storage]->comparison == VK_NULL_HANDLE,
            "legacy pixel-coordinate comparison was not rejected atomically");

        error = nil;
        unsigned normalizedNormalCreate = attempts;
        VkSampler normalizedNormal = [pixelBinding rdna4SamplerComparison:NO
            normalizedCoordinates:YES error:&error];
        require(normalizedNormal != VK_NULL_HANDLE && normalizedNormal != pixelNormal &&
            error == nil && attempts == normalizedNormalCreate + 1 &&
            createInfos[normalizedNormalCreate].unnormalizedCoordinates == VK_FALSE &&
            createInfos[normalizedNormalCreate].compareEnable == VK_FALSE,
            "pixel sampler normalized representation");

        unsigned normalizedComparisonCreate = attempts;
        error = nil;
        VkSampler pixelComparison = [pixelBinding rdna4SamplerComparison:YES
            normalizedCoordinates:YES error:&error];
        require(pixelComparison != VK_NULL_HANDLE && error == nil &&
            attempts == normalizedComparisonCreate + 1 &&
            createInfos[normalizedComparisonCreate].compareEnable == VK_TRUE &&
            createInfos[normalizedComparisonCreate].unnormalizedCoordinates == VK_FALSE &&
            pixelComparison != pixelNormal &&
            [pixelBinding rdna4Storage]->comparison == pixelComparison,
            "pixel normalized comparison representation");

        unsigned legacyAfterNormalizedCreate = attempts;
        error = nil;
        VkSampler legacyAfterNormalized = [pixelBinding
            rdna4SamplerComparison:YES error:&error];
        VkSampler stillNormalized = [pixelBinding
            rdna4SamplerComparison:YES normalizedCoordinates:YES error:&error];
        require(legacyAfterNormalized == VK_NULL_HANDLE && error != nil &&
            attempts == legacyAfterNormalizedCreate &&
            [pixelBinding rdna4Storage]->comparison == pixelComparison &&
            stillNormalized == pixelComparison,
            "legacy pixel comparison reused the normalized comparison handle");
        error = nil;

        unsigned pixelVariantCreate = attempts;
        RDNA4SamplerBinding *pixelVariant = [pixel
            rdna4BindingWithLODMinClamp:2.0f lodMaxClamp:5.0f error:&error];
        require(pixelVariant != nil && error == nil &&
            pixelVariant != pixelBinding &&
            [pixelVariant rdna4Storage] != [pixelBinding rdna4Storage] &&
            attempts == pixelVariantCreate + 1 &&
            bindingHasLODClamps(pixelVariant, 2.0f, 5.0f),
            "pixel LOD variant binding");

        unsigned pixelVariantComparisonCreate = attempts;
        VkSampler pixelVariantComparison = [pixelVariant
            rdna4SamplerComparison:YES normalizedCoordinates:YES error:&error];
        require(pixelVariantComparison != VK_NULL_HANDLE && error == nil &&
            attempts == pixelVariantComparisonCreate + 1 &&
            createInfos[pixelVariantComparisonCreate].compareEnable == VK_TRUE &&
            createInfos[pixelVariantComparisonCreate].unnormalizedCoordinates == VK_FALSE &&
            pixelVariantComparison != pixelNormal &&
            [pixelVariant rdna4Storage]->comparison == pixelVariantComparison &&
            bindingHasLODClamps(pixelVariant, 2.0f, 5.0f),
            "pixel variant normalized comparison representation");
        mappingCases += 7;
        pixelVariant = nil;
        normalizedVariant = nil;
        pixel = nil;
        normalized = nil;
    }
    requireMockAndRegistryEmpty();
}

enum WrappedField {
    WrappedMinFilter,
    WrappedMagFilter,
    WrappedMipFilter,
    WrappedSAddress,
    WrappedTAddress,
    WrappedRAddress,
    WrappedBorder,
    WrappedCompare,
    WrappedAnisotropy,
    WrappedFieldCount,
};

// Apple normalizes out-of-range address enum values in the descriptor setter,
// so a plain setter round trip would silently truncate 0x100000000 to a legal
// value. Store and return the raw machine-word values ourselves.
@interface RDNA4RawWrappedSamplerDescriptor : MTLSamplerDescriptor {
@public
    NSUInteger rawMinFilter;
    NSUInteger rawMagFilter;
    NSUInteger rawMipFilter;
    NSUInteger rawSAddressMode;
    NSUInteger rawTAddressMode;
    NSUInteger rawRAddressMode;
    NSUInteger rawBorderColor;
    NSInteger rawCompareFunction;
    NSUInteger rawMaxAnisotropy;
}
@end

@implementation RDNA4RawWrappedSamplerDescriptor
- (NSString *)label { return @"wrapped-binding-state"; }
- (BOOL)normalizedCoordinates { return YES; }
- (float)lodMinClamp { return 0.25f; }
- (float)lodMaxClamp { return 12.5f; }
- (BOOL)supportArgumentBuffers { return NO; }
- (MTLSamplerMinMagFilter)minFilter { return rawMinFilter; }
- (MTLSamplerMinMagFilter)magFilter { return rawMagFilter; }
- (MTLSamplerMipFilter)mipFilter { return rawMipFilter; }
- (MTLSamplerAddressMode)sAddressMode { return rawSAddressMode; }
- (MTLSamplerAddressMode)tAddressMode { return rawTAddressMode; }
- (MTLSamplerAddressMode)rAddressMode { return rawRAddressMode; }
- (MTLSamplerBorderColor)borderColor { return rawBorderColor; }
- (MTLCompareFunction)compareFunction { return rawCompareFunction; }
- (NSUInteger)maxAnisotropy { return rawMaxAnisotropy; }
@end

static RDNA4RawWrappedSamplerDescriptor *rawWrappedDescriptor(void) {
    RDNA4RawWrappedSamplerDescriptor *descriptor =
        [RDNA4RawWrappedSamplerDescriptor new];
    descriptor->rawMinFilter = MTLSamplerMinMagFilterLinear;
    descriptor->rawMagFilter = MTLSamplerMinMagFilterLinear;
    descriptor->rawMipFilter = MTLSamplerMipFilterLinear;
    descriptor->rawSAddressMode = MTLSamplerAddressModeRepeat;
    descriptor->rawTAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor->rawRAddressMode = MTLSamplerAddressModeMirrorRepeat;
    descriptor->rawBorderColor = MTLSamplerBorderColorOpaqueWhite;
    descriptor->rawCompareFunction = MTLCompareFunctionLess;
    descriptor->rawMaxAnisotropy = 4;
    return descriptor;
}

static void setWrappedField(RDNA4RawWrappedSamplerDescriptor *descriptor,
                            enum WrappedField field, NSUInteger value) {
    switch (field) {
    case WrappedMinFilter:
        descriptor->rawMinFilter = value;
        break;
    case WrappedMagFilter:
        descriptor->rawMagFilter = value;
        break;
    case WrappedMipFilter:
        descriptor->rawMipFilter = value;
        break;
    case WrappedSAddress:
        descriptor->rawSAddressMode = value;
        break;
    case WrappedTAddress:
        descriptor->rawTAddressMode = value;
        break;
    case WrappedRAddress:
        descriptor->rawRAddressMode = value;
        break;
    case WrappedBorder:
        descriptor->rawBorderColor = value;
        break;
    case WrappedCompare:
        descriptor->rawCompareFunction = (NSInteger)value;
        break;
    case WrappedAnisotropy:
        descriptor->rawMaxAnisotropy = value;
        break;
    default:
        break;
    }
}

static void expectWrappedDescriptorRejected(GarnetDevice *device,
                                            enum WrappedField field,
                                            NSUInteger value,
                                            const char *name,
                                            NSUInteger registryBefore,
                                            NSUInteger allocationBefore) {
    RDNA4RawWrappedSamplerDescriptor *descriptor = rawWrappedDescriptor();
    setWrappedField(descriptor, field, value);
    unsigned beforeAttempts = attempts;
    id state = [device newSamplerStateWithDescriptor:descriptor];
    require(state == nil && attempts == beforeAttempts &&
        registryConfigCount() == registryBefore &&
        allocationCount() == allocationBefore,
        name);
    wrappedRejects++;
}

static void testWrappedDescriptorFields(void) {
    resetMockAndRegistry();
    unsigned attemptsBefore = 0;
    unsigned createsBefore = 0;
    @autoreleasepool {
        RDNA4SamplerState *baseline = newSamplerState(bindingDescriptor());
        NSUInteger registryBefore = registryConfigCount();
        NSUInteger allocationBefore = allocationCount();
        require(registryBefore == 1 && allocationBefore == 1,
            "wrapped-field baseline registry state");
        (void)baseline;
        attemptsBefore = attempts;
        createsBefore = creates;

        GarnetDevice *device = [GarnetDevice new];
        require(device != nil, "public device facade allocation");
        const NSUInteger values[] = {
            (NSUInteger)0x100000000ULL,
            (NSUInteger)0x100000001ULL,
            NSUIntegerMax,
        };
        for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
            for (unsigned field = 0; field < WrappedFieldCount; ++field)
                expectWrappedDescriptorRejected(device, (enum WrappedField)field,
                    values[i], "wrapped descriptor field was accepted",
                    registryBefore, allocationBefore);
        }
        device = nil;
        baseline = nil;
    }
    require(attempts == attemptsBefore && creates == createsBefore &&
        registryConfigCount() == 0 && allocationCount() == 0,
        "wrapped descriptor rejection reached Vulkan or changed registry state");
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
        vkCreateSampler = createSampler;
        vkDestroySampler = destroySampler;
        resetMockAndRegistry();

        testVariantStorageRepresentations();
        testBackendLODBounds();
        testSetterAtomicUpdates();
        testBatchLODSetters();
        testDescriptorMapping();
        testWrappedDescriptorFields();
        run_cpu_texture_oracle();

        require(biasViolations == 0, "one or more samplers used mipLodBias");
        require(lifetimeCreates == lifetimeDestroys,
            "successful sampler creations and destroys differ");
        requireMockAndRegistryEmpty();
        printf("SAMPLER_BINDING_STATE_PASS checks=%u binding_cases=%u "
               "setter_cases=%u mapping_cases=%u wrapped_rejects=%u "
               "oracle_cases=%u simulated_vulkan=1 gpu_execution=0\n",
               checks, bindingCases, setterCases, mappingCases,
               wrappedRejects, oracleCases);
    }
    return 0;
}

#else

int main(void) {
    run_cpu_texture_oracle();
    printf("SAMPLER_BINDING_ORACLE_PASS checks=%u oracle_cases=%u\n",
        checks, oracleCases);
    return 0;
}

#endif
