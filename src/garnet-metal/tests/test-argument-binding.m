// Exercise the production descriptor gate and metadata parser before Vulkan sees a layout.
#include "GarnetDevice.m"

static unsigned checks, creates;
static void require(BOOL ok, const char *message) {
    checks++;
    if (!ok) { fprintf(stderr, "ARGUMENT_BINDING_FAIL %s\n", message); exit(1); }
}
static VkResult VKAPI_CALL createLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    require(info->bindingCount == 2 && info->pBindings[0].binding == 32 &&
        info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
        info->pBindings[0].descriptorCount == 2 && info->pBindings[1].binding == 160 &&
        info->pBindings[1].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER &&
        info->pBindings[1].descriptorCount == 2, "descriptor array layout changed");
    creates++; *layout = (VkDescriptorSetLayout)(uintptr_t)1; return VK_SUCCESS;
}
static NSDictionary *row(BOOL sampler, id binding, id count, id buffer, id offset) {
    return @{ @"kind": sampler ? @"EmbeddedArgBufferSampler" : @"EmbeddedArgBufferTexture",
        @"descriptor": @{ @"binding": binding, @"count": count },
        @"embedded_source": @{ @"buffer_index": buffer, @"field_offset": offset } };
}
static void check(RDNA4sRefl *r, NSArray *rows, BOOL expected) {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL; uint32_t count = 0; NSError *error = nil;
    unsigned before = creates;
    BOOL ok = rdna4_stage_dsl(r, 0, VK_SHADER_STAGE_COMPUTE_BIT, @{@"bindings": rows},
        &layout, &bindings, &count, [NSMutableArray array], &error);
    require(ok == expected, "unexpected acceptance");
    if (expected) {
        require(layout && !error && count == 2 && creates == before + 1 &&
            bindings[0].embedded && bindings[0].embeddedOffset == 0 &&
            bindings[1].embedded && bindings[1].embeddedOffset == 16 &&
            bindings[0].embeddedBuffer == 3 && bindings[1].embeddedBuffer == 3, "array source coordinates changed");
    } else require(error && !bindings && !count && creates == before, "rejected input reached Vulkan");
    free(bindings);
}
int main(void) {
    @autoreleasepool {
        RDNA4sRefl *r = calloc(1, sizeof *r);
        require(r != NULL, "reflection allocation");
        r->bind[0][32] = (RDNA4sBind){ .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 2 };
        r->bind[0][160] = (RDNA4sBind){ .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 2 };
        r->descriptor_features = RDNA4S_NONUNIFORM_SAMPLED;
        vkCreateDescriptorSetLayout = createLayout;
        NSDictionary *image = row(NO, @32, @2, @3, @0), *sampler = row(YES, @160, @2, @3, @16);
        RDNA4R.nonUniformSampledImages = NO;
        check(r, @[image, sampler], NO);
        RDNA4R.nonUniformSampledImages = YES;
        check(r, @[image, sampler], YES);
        r->bind[0][32].runtime = 1;
        check(r, @[image, sampler], NO);
        r->bind[0][32].runtime = 0;
        r->descriptor_features |= RDNA4S_RUNTIME_ARRAY;
        check(r, @[image, sampler], NO);
        r->descriptor_features &= ~RDNA4S_RUNTIME_ARRAY;
        r->descriptor_features |= RDNA4S_NONUNIFORM_STORAGE;
        check(r, @[image, sampler], NO);
        RDNA4R.nonUniformStorageImages = YES;
        check(r, @[image, sampler], YES);
        r->descriptor_features |= RDNA4S_NONUNIFORM_BUFFERS;
        check(r, @[image, sampler], NO);
        RDNA4R.nonUniformBuffers = YES;
        check(r, @[image, sampler], YES);
        r->descriptor_features |= RDNA4S_NONUNIFORM_OTHER;
        check(r, @[image, sampler], NO);
        r->descriptor_features = RDNA4S_NONUNIFORM_SAMPLED;
        for (id value in @[@YES, @160.0, @-1, @4294967456ULL, @"160", [NSNull null]])
            check(r, @[image, row(YES, value, @2, @3, @16)], NO);
        for (id value in @[@YES, @2.0, @0, @3, @-2, @"2"])
            check(r, @[image, row(YES, @160, value, @3, @16)], NO);
        for (id value in @[@YES, @3.0, @31, @-1])
            check(r, @[image, row(YES, @160, @2, value, @16)], NO);
        for (id value in @[@YES, @16.0, @15, @-8])
            check(r, @[image, row(YES, @160, @2, @3, value)], NO);
        check(r, @[image, sampler, sampler], NO);
        check(r, @[image, row(NO, @160, @2, @3, @16)], NO);
        const uint32_t spv[] = { 0x07230203, 0x00010300, 0, 1, 0,
            0x00020011, 5307, 0x00020011, 5309, 0x00020011, 5306, 0x00020011, 5311, 0x00020011, 5302 };
        RDNA4sMod module = {0}; char why[128] = {0};
        require(!rdna4s_parse(spv, sizeof spv / sizeof *spv, &module, why, sizeof why), "capability parse");
        uint32_t expectedFeatures = RDNA4S_RUNTIME_ARRAY | RDNA4S_NONUNIFORM_SAMPLED |
            RDNA4S_NONUNIFORM_STORAGE | RDNA4S_NONUNIFORM_BUFFERS | RDNA4S_NONUNIFORM_UNIFORM_TEXEL;
        require(module.descriptor_features == expectedFeatures, "SPIR-V capability requirements lost");
        require(!rdna4s_reflect(&module, 2, r) && r->descriptor_features == expectedFeatures, "reflection requirements lost");
        rdna4s_free(&module);
        const uint32_t decorated[] = {0x07230203, 0x00010400, 0, 6, 0,
            0x00040047, 1, 34, 0, 0x00040047, 2, 34, 2,
            0x00040047, 3, 34, 3, 0x00040047, 4, 34, 4};
        NSData *relocated = rdna4_spv_set([NSData dataWithBytes:decorated length:sizeof decorated], 1);
        const uint32_t *words = relocated.bytes;
        require(words[8] == 1 && words[12] == 2 && words[16] == 3 && words[20] == 4,
            "fragment relocation overwrote shared GPU argument descriptor sets");
        free(r);
        printf("ARGUMENT_BINDING_PASS checks=%u accepted=%u simulated_vulkan=1 gpu_execution=0\n", checks, creates);
    }
    return 0;
}
