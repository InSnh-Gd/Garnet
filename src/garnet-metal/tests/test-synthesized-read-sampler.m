// Production stage-layout regression; Vulkan callbacks inspect the sampler without a GPU.
#include "GarnetDevice.m"

static unsigned checks, creates, destroys;

static void require(BOOL ok, const char *message) {
    ++checks;
    if (!ok) { fprintf(stderr, "READ_SAMPLER_FAIL %s\n", message); exit(1); }
}

static VkResult VKAPI_CALL createSampler(VkDevice device, const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    (void)device; (void)allocator;
    require(info && sampler, "sampler arguments");
    require(info->minFilter == VK_FILTER_NEAREST && info->magFilter == VK_FILTER_NEAREST &&
        info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST, "exact nearest and nearest mip");
    require(info->mipLodBias == 0 && info->minLod == 0 && info->maxLod >= 2,
        "read sampler must allow the explicit mip");
    require(!info->compareEnable && !info->unnormalizedCoordinates && !info->anisotropyEnable,
        "read sampler uses normalized non-comparison coordinates");
    *sampler = (VkSampler)(uintptr_t)(0xa000 + ++creates);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
    const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    require(sampler != VK_NULL_HANDLE, "destroy nonnull read sampler");
    ++destroys;
}

static VkResult VKAPI_CALL createLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    require(info && layout && info->bindingCount == 1 && info->pBindings[0].binding == 160,
        "one reflected read-sampler binding");
    *layout = (VkDescriptorSetLayout)(uintptr_t)0xb000;
    return VK_SUCCESS;
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
        RDNA4R.lim.maxSamplerAllocationCount = 64;
        RDNA4R.lim.maxSamplerAnisotropy = 1;
        vkCreateSampler = createSampler;
        vkDestroySampler = destroySampler;
        vkCreateDescriptorSetLayout = createLayout;
        NSDictionary *row = @{
            @"kind": @"SynthesizedReadSampler",
            @"descriptor": @{ @"set": @0, @"binding": @160, @"count": @1 },
            @"static_sampler": @{
                @"min_filter": @"Nearest", @"mag_filter": @"Nearest", @"mip_filter": @"Nearest",
                @"address_mode_s": @"ClampToEdge", @"address_mode_t": @"ClampToEdge",
                @"address_mode_r": @"ClampToEdge", @"border_color": @"TransparentBlack",
                @"compare_function": @"Never", @"coordinates": @"Normalized",
                @"lod_min_clamp": @0.0, @"lod_max_clamp": @65504.0, @"max_anisotropy": @1,
            },
        };
        for (int set = 0; set <= 1; ++set) {
            RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
            require(reflection != NULL, "reflection allocation");
            reflection->maxset = set;
            reflection->bind[set][160] = (RDNA4sBind){
                .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1 };
            NSDictionary *metadata = @{ @"bindings": @[row] };
            NSDictionary *relocated = rdna4_stage_meta_relocate(metadata, set);
            require([relocated[@"bindings"][0][@"descriptor"][@"set"] intValue] == set,
                "read sampler follows fragment descriptor relocation");
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            RDNA4PB *bindings = NULL;
            uint32_t count = 0;
            NSMutableArray *owned = [NSMutableArray array];
            NSError *error = nil;
            require(rdna4_stage_dsl(reflection, set, set ? VK_SHADER_STAGE_FRAGMENT_BIT :
                VK_SHADER_STAGE_COMPUTE_BIT, metadata, &layout, &bindings, &count, owned, &error),
                "read sampler stage layout accepted");
            require(!error && count == 1 && bindings[0].stat && owned.count == 1,
                "read sampler state materialized and owned");
            rdna4_destroy_sampler(bindings[0].stat, nil);
            free(bindings);
            NSMutableDictionary *missing = [row mutableCopy];
            [missing removeObjectForKey:@"static_sampler"];
            error = nil;
            require(!rdna4_stage_dsl(reflection, set, VK_SHADER_STAGE_COMPUTE_BIT,
                @{ @"bindings": @[missing] }, &layout, &bindings, &count, owned, &error) && error,
                "missing read sampler state cannot fall back to an application sampler");
            free(reflection);
        }
        require(creates == 2 && destroys == 2 && atomic_load(&rdna4_sampler_allocations) == 0,
            "sampler allocation and teardown balanced");
        printf("READ_SAMPLER_PASS checks=%u explicit_mip=1 fragment_relocation=1 "
            "missing_state_rejected=1 simulated_vulkan=1 gpu_execution=0\n", checks);
    }
    return 0;
}
