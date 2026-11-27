#include "GarnetDevice.m"

@interface RDNA4SamplerState (RuntimeTestAccess)
- (RDNA4SamplerStorage *)rdna4TestStorage;
@end

@implementation RDNA4SamplerState (RuntimeTestAccess)
- (RDNA4SamplerStorage *)rdna4TestStorage { return _storage; }
@end

static unsigned attempts, normalAttempts, comparisonAttempts;
static unsigned creates, normalCreates, comparisonCreates, destroys;
static BOOL failNext;
static VkSamplerCreateInfo lastInfo;
static VkSampler destroyed[64];

static void require(BOOL ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "SAMPLER_RUNTIME_FAIL %s\n", message);
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

static NSUInteger argumentTokenCount(void) {
    rdna4_argument_tokens_init();
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    NSUInteger count = rdna4_argument_tokens.count;
    [rdna4_argument_tokens_lock unlock];
    return count;
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSampler *sampler) {
    (void)device;
    (void)allocator;
    require(info != NULL && sampler != NULL, "mock create arguments");
    ++attempts;
    if (info->compareEnable) ++comparisonAttempts;
    else ++normalAttempts;
    lastInfo = *info;
    if (failNext) {
        failNext = NO;
        *sampler = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *sampler = (VkSampler)(uintptr_t)(0x2000u + creates);
    ++creates;
    if (info->compareEnable) ++comparisonCreates;
    else ++normalCreates;
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

static MTLSamplerDescriptor *samplerDescriptor(MTLCompareFunction function,
                                                NSUInteger anisotropy,
                                                BOOL argumentBuffers,
                                                NSString *label) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.label = label;
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.mipFilter = MTLSamplerMipFilterLinear;
    descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeMirrorRepeat;
    descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite;
    descriptor.compareFunction = function;
    descriptor.lodMinClamp = 0.25;
    descriptor.lodMaxClamp = 12.5;
    descriptor.maxAnisotropy = anisotropy;
    descriptor.supportArgumentBuffers = argumentBuffers;
    return descriptor;
}

static void resetMockAndRegistry(void) {
    @synchronized ([RDNA4SamplerStorage class]) {
        require(!rdna4_sampler_configs || ![rdna4_sampler_configs count],
            "registry was not empty before reset");
        require(allocationCount() == 0, "backend sampler count was not empty before reset");
        rdna4_sampler_configs = [NSMapTable strongToWeakObjectsMapTable];
        rdna4_argument_sampler_configs = 0;
        atomic_store(&rdna4_sampler_allocations, 0);
    }
    attempts = normalAttempts = comparisonAttempts = 0;
    creates = normalCreates = comparisonCreates = destroys = 0;
    failNext = NO;
    memset(&lastInfo, 0, sizeof lastInfo);
    memset(destroyed, 0, sizeof destroyed);
    RDNA4R.lim.maxSamplerAllocationCount = 64;
    RDNA4R.samplerMirrorClamp = NO;
}

static void testSharedConfigAndRepresentations(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *first = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 4, YES, @"first")
            error:&error];
        RDNA4SamplerState *second = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 4, YES, @"second")
            error:&error];
        require(first != nil && second != nil && error == nil,
            "shared sampler creation failed");
        require([first supportArgumentBuffers] && [second supportArgumentBuffers],
            "supportArgumentBuffers wrapper state");
        require(storageOf(first) == storageOf(second) &&
            storageOf(first)->normal == [first vkSampler] &&
            storageOf(first)->comparison == VK_NULL_HANDLE &&
            storageOf(first)->references == 2 &&
            storageOf(first)->argumentReferences == 2,
            "shared storage fields before comparison creation");
        require([first vkSampler] == [second vkSampler],
            "same config did not share the normal representation");
        require(normalAttempts == 1 && normalCreates == 1 &&
            comparisonCreates == 0, "same config created more than one normal sampler");
        require(allocationCount() == 1 && registryConfigCount() == 1 &&
            argumentConfigCount() == 1,
            "same config registry/allocation accounting");

        VkSampler comparison = [first rdna4SamplerComparison:YES error:&error];
        require(comparison != VK_NULL_HANDLE &&
            [second rdna4SamplerComparison:YES error:&error] == comparison,
            "comparison representation was not shared");
        require(storageOf(first)->comparison == comparison &&
            storageOf(first)->references == 2 &&
            storageOf(first)->argumentReferences == 2,
            "shared storage fields after comparison creation");
        require(comparisonAttempts == 1 && comparisonCreates == 1 &&
            allocationCount() == 2, "comparison representation creation count");
        require(lastInfo.compareEnable == VK_TRUE &&
            lastInfo.compareOp == VK_COMPARE_OP_NEVER &&
            lastInfo.mipLodBias == 0.0f,
            "comparison representation state");

        first = nil;
        require(storageOf(second)->references == 1 &&
            storageOf(second)->argumentReferences == 1 &&
            storageOf(second)->comparison == comparison,
            "shared storage references after one YES wrapper release");
        require(registryConfigCount() == 1 && argumentConfigCount() == 1 &&
            allocationCount() == 2 && destroys == 0,
            "releasing one shared reference retired storage too early");
        second = nil;
    }
    require(registryConfigCount() == 0 && argumentConfigCount() == 0 &&
        allocationCount() == 0 && destroys == 2,
        "last shared wrapper did not remove the weak registry key and handles");
}

static void testSupportArgumentBufferLifecycle(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *yes = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, YES, @"yes")
            error:&error];
        RDNA4SamplerState *no = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, NO, @"no")
            error:&error];
        require(yes != nil && no != nil && error == nil,
            "support-flag sampler creation failed");
        require([yes supportArgumentBuffers] && ![no supportArgumentBuffers],
            "wrapper support-flag values");
        require(storageOf(yes) == storageOf(no) &&
            storageOf(yes)->references == 2 &&
            storageOf(yes)->argumentReferences == 1,
            "support flag changed storage or unique YES count");
        require([yes vkSampler] == [no vkSampler],
            "support flag forked the backend configuration");
        require(argumentConfigCount() == 1, "YES did not count one unique config");

        __unsafe_unretained RDNA4SamplerStorage *storage = storageOf(no);
        yes = nil;
        require(storage->references == 1 && storage->argumentReferences == 0 &&
            registryConfigCount() == 1,
            "last YES release did not preserve NO-owned storage");
        require(registryConfigCount() == 1 && argumentConfigCount() == 0 &&
            allocationCount() == 1 && destroys == 0,
            "last YES release with a NO wrapper did not preserve storage");
        RDNA4SamplerState *yesAgain = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, YES, @"yes-again")
            error:&error];
        require(yesAgain != nil && [yesAgain vkSampler] == [no vkSampler],
            "NO wrapper did not share the reactivated storage");
        require(storageOf(yesAgain) == storage &&
            storage->references == 2 && storage->argumentReferences == 1,
            "YES re-entry did not reuse storage and re-count the config");
        require(argumentConfigCount() == 1 && allocationCount() == 1,
            "YES re-entry did not increment the unique-config count");
        no = nil;
        require(storage->references == 1 && storage->argumentReferences == 1,
            "releasing NO changed the YES reference count");
        require(argumentConfigCount() == 1 && allocationCount() == 1,
            "releasing NO changed the YES reference count");
        yesAgain = nil;
    }
    require(registryConfigCount() == 0 && argumentConfigCount() == 0 &&
        allocationCount() == 0 && destroys == 1,
        "support-flag lifecycle leaked storage or backend sampler");
}

static void testConfigIdentityAndWeakKeyCleanup(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *base = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"base")
            error:&error];
        RDNA4SamplerState *same = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"same")
            error:&error];
        require(base != nil && same != nil &&
            [base vkSampler] == [same vkSampler] &&
            storageOf(base) == storageOf(same) &&
            storageOf(base)->references == 2 &&
            normalAttempts == 1 && registryConfigCount() == 1,
            "label incorrectly participated in the config key");

        __unsafe_unretained RDNA4SamplerStorage *baseStorage = storageOf(base);
        MTLSamplerDescriptor *differentDescriptor =
            samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"different");
        differentDescriptor.lodMinClamp = 0.5;
        RDNA4SamplerState *different = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:differentDescriptor error:&error];
        require(different != nil && [different vkSampler] != [base vkSampler] &&
            storageOf(different) != baseStorage &&
            storageOf(different)->references == 1 &&
            normalAttempts == 2 && normalCreates == 2 &&
            registryConfigCount() == 2 && allocationCount() == 2,
            "one-field LOD difference did not create a distinct storage entry");
        require(argumentConfigCount() == 0, "NO-only samplers counted as argument configs");

        same = nil;
        require(baseStorage->references == 1,
            "releasing duplicate key changed the shared storage reference count");
        require(registryConfigCount() == 2,
            "releasing duplicate key removed the shared storage entry");
        different = nil;
        base = nil;
    }
    require(registryConfigCount() == 0 && allocationCount() == 0 && destroys == 2,
        "weak registry key or backend sampler remained after last wrapper");
}

static void testArgumentSamplerTokenAliases(void) {
    resetMockAndRegistry();
    require(argumentTokenCount() == 0, "token registry was not empty before alias test");
    __weak RDNA4SamplerState *weakFirst = nil;
    __weak RDNA4SamplerState *weakSecond = nil;
    __weak RDNA4SamplerState *weakThird = nil;
    __weak RDNA4SamplerState *weakOther = nil;
    uint64_t sharedToken = 0, otherToken = 0;

    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *first = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, YES, @"first")
            error:&error];
        RDNA4SamplerState *second = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"second")
            error:&error];
        require(first != nil && second != nil && error == nil,
            "token alias sampler creation failed");
        weakFirst = first;
        weakSecond = second;
        __unsafe_unretained RDNA4SamplerStorage *sharedStorage =
            [first rdna4ArgumentIdentity];
        require([first rdna4ArgumentIdentity] == storageOf(first) &&
            sharedStorage == [second rdna4ArgumentIdentity],
            "same-config wrappers did not expose one storage identity");

        sharedToken = rdna4_argument_resource_token(first, nil, NO);
        require(sharedToken != 0 &&
            rdna4_argument_resource_token(second, nil, NO) == sharedToken,
            "same storage identity allocated more than one sampler token");
        require(argumentTokenCount() == 1,
            "sampler aliases created more than one registry token");
        @autoreleasepool {
            NSDictionary *live = rdna4_argument_live_samplers(nil);
            require(live.count == 1 &&
                (live[@(sharedToken)] == first || live[@(sharedToken)] == second),
                "live-sampler query exposed more than one entry for a shared token");
        }

        second = nil;
        require(weakSecond == nil,
            "token registry or alias collection retained the latest wrapper");
        @autoreleasepool {
            require(rdna4_argument_resource_for_token(sharedToken, NO, nil) == first,
                "releasing the latest wrapper invalidated the older alias");
        }
        @autoreleasepool {
            NSDictionary *live = rdna4_argument_live_samplers(nil);
            require(live.count == 1 && live[@(sharedToken)] == first,
                "live-sampler query did not choose the surviving alias");
        }

        RDNA4SamplerState *third = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, YES, @"third")
            error:&error];
        require(third != nil && storageOf(third) == sharedStorage,
            "third same-config wrapper did not reuse storage");
        weakThird = third;
        require(rdna4_argument_resource_token(third, nil, NO) == sharedToken,
            "reactivated storage allocated a second sampler token");

        @autoreleasepool {
            NSArray *deduplicated = rdna4_argument_resource_snapshot(nil, [NSSet set],
                [NSSet setWithObjects:first, third, nil]);
            require(deduplicated.count == 1 &&
                [deduplicated[0][0] unsignedLongLongValue] == sharedToken &&
                (deduplicated[0][1] == first || deduplicated[0][1] == third) &&
                ![deduplicated[0][2] boolValue],
                "snapshot did not collapse aliases of one sampler token");
            NSArray *scoped = rdna4_argument_resource_snapshot(nil, [NSSet set],
                [NSSet setWithObject:third]);
            require(scoped.count == 1 &&
                [scoped[0][0] unsignedLongLongValue] == sharedToken &&
                scoped[0][1] == third,
                "snapshot did not select the live alias present in its scope");
        }

        first = nil;
        require(weakFirst == nil,
            "token registry or alias collection retained the original wrapper");
        @autoreleasepool {
            require(rdna4_argument_resource_for_token(sharedToken, NO, nil) == third,
                "releasing the original wrapper invalidated the surviving alias");
        }
        @autoreleasepool {
            NSDictionary *live = rdna4_argument_live_samplers(nil);
            require(live.count == 1 && live[@(sharedToken)] == third,
                "live-sampler query lost the remaining alias after original release");
        }

        MTLSamplerDescriptor *differentDescriptor =
            samplerDescriptor(MTLCompareFunctionNever, 1, YES, @"other");
        differentDescriptor.lodMinClamp = 0.5;
        RDNA4SamplerState *other = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:differentDescriptor error:&error];
        require(other != nil && storageOf(other) != sharedStorage,
            "different config unexpectedly reused storage");
        weakOther = other;
        otherToken = rdna4_argument_resource_token(other, nil, NO);
        require(otherToken != 0 && otherToken != sharedToken,
            "different config did not allocate a distinct sampler token");

        @autoreleasepool {
            NSArray *sharedScope = rdna4_argument_resource_snapshot(nil, [NSSet set],
                [NSSet setWithObject:third]);
            require(sharedScope.count == 1 &&
                [sharedScope[0][0] unsignedLongLongValue] == sharedToken &&
                sharedScope[0][1] == third,
                "snapshot included an unrelated sampler token or missed its scoped wrapper");
            NSArray *otherScope = rdna4_argument_resource_snapshot(nil, [NSSet set],
                [NSSet setWithObject:other]);
            require(otherScope.count == 1 &&
                [otherScope[0][0] unsignedLongLongValue] == otherToken &&
                otherScope[0][1] == other,
                "snapshot did not select the token owned by the scoped wrapper");
            require(rdna4_argument_resource_snapshot(nil, [NSSet set], [NSSet set]).count == 0,
                "empty sampler scope emitted token rows");
        }

        third = nil;
        other = nil;
    }

    require(weakFirst == nil && weakSecond == nil && weakThird == nil && weakOther == nil,
        "released sampler wrappers were retained by the token registry");
    require(rdna4_argument_resource_for_token(sharedToken, NO, nil) == nil &&
        rdna4_argument_resource_for_token(otherToken, NO, nil) == nil,
        "pruned sampler token still resolved");
    require(argumentTokenCount() == 0,
        "token registry retained an entry after every alias was released");
    require(registryConfigCount() == 0 && argumentConfigCount() == 0 &&
        allocationCount() == 0 && destroys == 2,
        "token alias lifecycle leaked sampler storage or backend handles");
}

static void testAllocationLimitAndRollback(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        RDNA4R.lim.maxSamplerAllocationCount = 0;
        RDNA4SamplerState *zero = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"limit-zero")
            error:&error];
        require(zero == nil && error != nil && normalAttempts == 0 &&
            allocationCount() == 0 && registryConfigCount() == 0,
            "zero global limit did not reject before backend creation");
    }

    RDNA4R.lim.maxSamplerAllocationCount = 1;
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *first = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"limit-one")
            error:&error];
        require(first != nil && allocationCount() == 1 && registryConfigCount() == 1,
            "last legal backend sampler was rejected");
        RDNA4SamplerState *over = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, NO, @"limit-over")
            error:&error];
        require(over == nil && error != nil && normalAttempts == 1 &&
            normalCreates == 1 && allocationCount() == 1 &&
            registryConfigCount() == 1,
            "over-limit request was not rejected before backend creation");
        first = nil;
        require(allocationCount() == 0 && registryConfigCount() == 0,
            "released limit owner did not free its slot");

        RDNA4R.lim.maxSamplerAllocationCount = 64;
        failNext = YES;
        error = nil;
        RDNA4SamplerState *failed = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionAlways, 1, NO, @"rollback")
            error:&error];
        require(failed == nil && error != nil && normalAttempts == 2 &&
            normalCreates == 1 && allocationCount() == 0 &&
            registryConfigCount() == 0,
            "failed backend create did not roll back the reserved count");

        error = nil;
        RDNA4SamplerState *retry = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionAlways, 1, NO, @"retry")
            error:&error];
        require(retry != nil && error == nil && normalAttempts == 3 &&
            normalCreates == 2 && allocationCount() == 1 &&
            registryConfigCount() == 1, "retry after backend failure was not clean");
        retry = nil;
    }
    require(allocationCount() == 0 && registryConfigCount() == 0 && destroys == 2,
        "allocation-limit rollback leaked a handle or registry key");
}

static void testComparisonAllocationLimit(void) {
    resetMockAndRegistry();
    RDNA4R.lim.maxSamplerAllocationCount = 1;
    @autoreleasepool {
        NSError *error = nil;
        RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, NO, @"compare-limit")
            error:&error];
        require(state != nil && allocationCount() == 1 && normalAttempts == 1,
            "normal owner for comparison limit was rejected");
        VkSampler normal = [state vkSampler];

        error = nil;
        require([state rdna4SamplerComparison:YES error:&error] ==
            VK_NULL_HANDLE && error != nil && comparisonAttempts == 0 &&
            allocationCount() == 1 && storageOf(state)->normal == normal &&
            storageOf(state)->comparison == VK_NULL_HANDLE,
            "comparison over the global limit reached the backend or changed normal state");

        RDNA4R.lim.maxSamplerAllocationCount = 2;
        error = nil;
        VkSampler comparison = [state rdna4SamplerComparison:YES error:&error];
        require(comparison != VK_NULL_HANDLE && error == nil &&
            attempts == 2 && normalAttempts == 1 && comparisonAttempts == 1 &&
            comparisonCreates == 1 && allocationCount() == 2 &&
            storageOf(state)->normal == normal &&
            storageOf(state)->comparison == comparison,
            "last legal comparison allocation was not created lazily");
        require([state rdna4SamplerComparison:YES error:&error] == comparison &&
            attempts == 2 && comparisonAttempts == 1,
            "repeated comparison lookup allocated another backend sampler");
        state = nil;
    }
    require(allocationCount() == 0 && registryConfigCount() == 0 && destroys == 2,
        "comparison-limit lifecycle leaked a handle or registry key");
}

static void testCreationAndComparisonFailures(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        failNext = YES;
        RDNA4SamplerState *failed = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionAlways, 1, NO, @"normal-fail")
            error:&error];
        require(failed == nil && error != nil && normalAttempts == 1 &&
            normalCreates == 0 && allocationCount() == 0 &&
            registryConfigCount() == 0, "failed normal create published registry state");

        error = nil;
        RDNA4SamplerState *state = [[RDNA4SamplerState alloc]
            initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, NO, @"compare-fail")
            error:&error];
        require(state != nil && allocationCount() == 1 &&
            registryConfigCount() == 1, "normal state before compare failure");
        VkSampler normal = [state vkSampler];
        failNext = YES;
        error = nil;
        require([state rdna4SamplerComparison:YES error:&error] ==
            VK_NULL_HANDLE && error != nil && comparisonAttempts == 1 &&
            comparisonCreates == 0 && allocationCount() == 1 &&
            storageOf(state)->normal == normal &&
            storageOf(state)->comparison == VK_NULL_HANDLE,
            "failed comparison did not retain only the normal representation");

        error = nil;
        VkSampler comparison = [state rdna4SamplerComparison:YES error:&error];
        require(comparison != VK_NULL_HANDLE && error == nil &&
            attempts == 4 && normalAttempts == 2 && comparisonAttempts == 2 &&
            comparisonCreates == 1 && allocationCount() == 2 &&
            storageOf(state)->normal == normal &&
            storageOf(state)->comparison == comparison,
            "comparison retry after backend failure was not clean");
        require([state rdna4SamplerComparison:YES error:&error] == comparison &&
            attempts == 4 && comparisonAttempts == 2,
            "comparison retry did not retain the successful representation");
        state = nil;
    }
    require(allocationCount() == 0 && registryConfigCount() == 0 && destroys == 2,
        "failed normal/comparison paths leaked ownership");
}

static void testMirrorClampFeature(void) {
    resetMockAndRegistry();
    @autoreleasepool {
        NSError *error = nil;
        MTLSamplerDescriptor *descriptor =
            samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"mirror-off");
        descriptor.sAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
        RDNA4SamplerState *off = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:descriptor error:&error];
        require(off == nil && error != nil && attempts == 0 &&
            registryConfigCount() == 0 && allocationCount() == 0,
            "MirrorClampToEdge was accepted without the backend feature");

        RDNA4R.samplerMirrorClamp = YES;
        descriptor = samplerDescriptor(MTLCompareFunctionNever, 1, NO, @"mirror-on");
        descriptor.sAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
        error = nil;
        RDNA4SamplerState *on = [[RDNA4SamplerState alloc]
            initWithDevice:nil descriptor:descriptor error:&error];
        require(on != nil && error == nil && attempts == 1 &&
            lastInfo.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE &&
            allocationCount() == 1, "MirrorClampToEdge did not reach the backend");
        on = nil;
    }
    require(allocationCount() == 0 && registryConfigCount() == 0 && destroys == 1,
        "MirrorClampToEdge lifecycle leaked a backend sampler");
}

static NSDictionary *staticDescriptor(NSString *comparison) {
    return @{ @"min_filter": @"Nearest", @"mag_filter": @"Nearest",
        @"mip_filter": @"None", @"address_mode_s": @"ClampToEdge",
        @"address_mode_t": @"ClampToEdge", @"address_mode_r": @"ClampToEdge",
        @"border_color": @"TransparentBlack", @"compare_function": comparison,
        @"coordinates": @"Normalized", @"lod_min_clamp": @0.0,
        @"lod_max_clamp": @100.0, @"max_anisotropy": @1 };
}

static void testStaticComparisonMapping(void) {
    NSError *error = nil;
    RDNA4SampCfg config;
    require(rdna4_static_cfg(staticDescriptor(@"Never"), &config, &error) &&
        config.cmp == MTLCompareFunctionNever, "static Never mapping");
    require(rdna4_static_cfg(staticDescriptor(@"None"), &config, &error) &&
        config.cmp == 0, "static None mapping");
    require(rdna4_static_cfg(staticDescriptor(@"Less"), &config, &error) &&
        config.cmp == MTLCompareFunctionLess, "static Less mapping");
    require(rdna4_static_cfg(staticDescriptor(@"Always"), &config, &error) &&
        config.cmp == MTLCompareFunctionAlways, "static Always mapping");
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
        RDNA4R.samplerAnisotropy = YES;
        RDNA4R.samplerMirrorClamp = NO;
        RDNA4R.lim.maxSamplerAnisotropy = 8.0f;
        RDNA4R.lim.maxSamplerAllocationCount = 64;
        vkCreateSampler = createSampler;
        vkDestroySampler = destroySampler;

        testSharedConfigAndRepresentations();
        testSupportArgumentBufferLifecycle();
        testConfigIdentityAndWeakKeyCleanup();
        testArgumentSamplerTokenAliases();
        testAllocationLimitAndRollback();
        testComparisonAllocationLimit();
        testCreationAndComparisonFailures();
        testMirrorClampFeature();
        resetMockAndRegistry();
        testStaticComparisonMapping();
        require(attempts == 0 && creates == 0 && allocationCount() == 0 &&
            registryConfigCount() == 0,
            "static mapping created backend or registry state");
        printf("SAMPLER_RUNTIME_PASS registry_shared=1 config_identity=1 "
               "argument_refs=1 weak_cleanup=1 allocation_limit=1 "
               "comparison_limit=1 rollback=1 mirror_feature=1 static_mapping=1 "
               "sampler_token_identity=1 sampler_alias_weak=1 sampler_alias_survivor=1 "
               "sampler_snapshot_dedup=1 sampler_snapshot_scope=1 "
               "simulated_vulkan=1 gpu_execution=0\n");
    }
    return 0;
}
