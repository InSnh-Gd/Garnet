#define main rdna4_legacy_sampler_main
#include "test-sampler-runtime.m"
#undef main
#include "test-argument-limit-snapshot-provider.c"

static int smallLimitProvider(const rdna4_radv_darwin_host_facts_v1 *facts,
    rdna4_radv_darwin_argument_limits_v1 *out) {
    int result = radv_darwin_get_argument_limits_v1(facts, out);
    out->max_sampler_allocations = 8;
    return result;
}

static void testSnapshotAdmission(void) {
    resetMockAndRegistry();
    rdna4_argument_limit_policy policy = rdna4_argument_limit_default_policy();
    require(rdna4_argument_limit_snapshot_init_from_export(&RDNA4ALS,
        smallLimitProvider, NULL, &policy) == RDNA4_ARGUMENT_LIMIT_RESULT_OK,
        "freeze capacity provider");
    require(RDNA4ALS.public_sampler_limit == 2, "two public configs after K=3/dummy reserve");
    rdna4_argument_limit_snapshot frozen = RDNA4ALS;
    atomic_store(&rdna4_argument_limit_full_open_state, 1);
    RDNA4R.lim.maxSamplerAllocationCount = 8;
    @autoreleasepool {
        NSError *error = nil;
        MTLSamplerDescriptor *pixel = samplerDescriptor(MTLCompareFunctionLess, 1, YES, @"pixel");
        pixel.normalizedCoordinates = NO;
        pixel.mipFilter = MTLSamplerMipFilterNotMipmapped;
        pixel.sAddressMode = pixel.tAddressMode = pixel.rAddressMode = MTLSamplerAddressModeClampToEdge;
        RDNA4SamplerState *first = [[RDNA4SamplerState alloc] initWithDevice:nil descriptor:pixel error:&error];
        RDNA4SamplerState *alias = [[RDNA4SamplerState alloc] initWithDevice:nil descriptor:pixel error:&error];
        require(first && alias && error == nil && argumentConfigCount() == 1 && allocationCount() == 1 &&
            rdna4_sampler_budget_state.reserved_objects == 3 &&
            rdna4_sampler_budget_state.reserved_actual_objects == 1,
            "alias reuses one public reservation and handle");
        RDNA4SamplerState *second = [[RDNA4SamplerState alloc] initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionAlways, 1, YES, @"second") error:&error];
        require(second && argumentConfigCount() == 2 && rdna4_sampler_budget_state.reserved_objects == 6,
            "last public configuration admitted");
        unsigned before = attempts;
        error = nil;
        RDNA4SamplerState *over = [[RDNA4SamplerState alloc] initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionGreater, 1, YES, @"over") error:&error];
        require(!over && error && attempts == before && argumentConfigCount() == 2,
            "one over public count rejected before backend creation");
        error = nil;
        RDNA4SamplerBinding *variant = [second rdna4BindingWithLODMinClamp:2 lodMaxClamp:4 error:&error];
        require(variant && error == nil && argumentConfigCount() == 2 &&
            ![variant rdna4Storage]->argumentReserved &&
            rdna4_sampler_committed_locked() == 7,
            "LOD variant consumes global object budget, no public config slot");
        RDNA4SamplerBinding *variant2 = [second rdna4BindingWithLODMinClamp:3 lodMaxClamp:5 error:&error];
        require(variant2 && rdna4_sampler_committed_locked() == 8,
            "direct LOD variant reaches last committed object");
        before = attempts;
        error = nil;
        RDNA4SamplerBinding *variantOver = [second rdna4BindingWithLODMinClamp:4 lodMaxClamp:6 error:&error];
        require(!variantOver && error && attempts == before,
            "direct allocation cannot steal argument representation reservation");
        error = nil;
        require([first rdna4SamplerComparison:NO normalizedCoordinates:YES error:&error] &&
            [first rdna4SamplerComparison:YES normalizedCoordinates:YES error:&error] && error == nil &&
            storageOf(first)->handles == 3 && rdna4_sampler_committed_locked() == 8,
            "all three pixel representations materialize at full commitment");
        first = nil;
        require(argumentConfigCount() == 2, "surviving public alias retains reservation");
        alias = nil;
        require(argumentConfigCount() == 1 && rdna4_sampler_budget_state.reserved_objects == 3,
            "last alias releases public reservation");
        variant = nil;
        variant2 = nil;
        second = nil;
    }
    require(allocationCount() == 0 && argumentConfigCount() == 0 && registryConfigCount() == 0 &&
        rdna4_sampler_budget_state.reserved_objects == 0 &&
        rdna4_sampler_budget_state.reserved_actual_objects == 0,
        "release restores every object and reservation");
    require(!memcmp(&frozen, &RDNA4ALS, sizeof(frozen)), "runtime accounting never mutates frozen limit");
    @autoreleasepool {
        NSError *error = nil;
        failNext = YES;
        RDNA4SamplerState *failed = [[RDNA4SamplerState alloc] initWithDevice:nil
            descriptor:samplerDescriptor(MTLCompareFunctionLess, 1, YES, @"rollback") error:&error];
        require(!failed && error && allocationCount() == 0 && argumentConfigCount() == 0 &&
            rdna4_sampler_budget_state.reserved_objects == 0 &&
            rdna4_sampler_budget_state.reserved_actual_objects == 0,
            "backend create failure rolls back configuration and object reservation");
    }
    printf("SNAPSHOT_ADMISSION_PASS public_last_one_over=1 alias=1 pixel_k3=1 lod_variant=1 global_objects=1 rollback=1 immutable=1 gpu_execution=0\n");
}

int main(void) {
    int result = rdna4_legacy_sampler_main();
    if (result) return result;
    @autoreleasepool { testSnapshotAdmission(); }
    return 0;
}
