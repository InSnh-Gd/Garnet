#include "GarnetDevice.m"
#include <assert.h>
static unsigned checks, creates, destroys, failAt;
static uint32_t createdBindingCount[16];
static VkDescriptorSetLayoutBinding createdBindings[16][2];
static VkDescriptorBindingFlags createdFlags[16][2];
#define CHECK(condition) do { ++checks; assert(condition); } while (0)
static VkResult VKAPI_CALL createLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    ++creates;
    if (creates == failAt) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (creates < 16) {
        createdBindingCount[creates] = info->bindingCount;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *bindingFlags = info->pNext;
        for (uint32_t i = 0; i < info->bindingCount && i < 2; ++i) {
            createdBindings[creates][i] = info->pBindings[i];
            createdFlags[creates][i] = bindingFlags
                ? bindingFlags->pBindingFlags[i] : 0;
        }
    }
    *layout = (VkDescriptorSetLayout)(uintptr_t)creates;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroyLayout(VkDevice device, VkDescriptorSetLayout layout,
    const VkAllocationCallbacks *allocator) {
    (void)device; (void)layout; (void)allocator; ++destroys;
}
static void VKAPI_CALL supportLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info, VkDescriptorSetLayoutSupport *support) {
    (void)device;
    support->supported = VK_TRUE;
    if (support->pNext) {
        if (info->bindingCount == 1)
            CHECK(info->pBindings[0].descriptorCount == 1);
        else {
            const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = info->pNext;
            CHECK(info->bindingCount == 2 && info->pBindings[0].binding == 0 &&
                info->pBindings[1].binding == 1 &&
                info->pBindings[1].descriptorCount == 1 &&
                !(flags->pBindingFlags[0] &
                  VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) &&
                (flags->pBindingFlags[1] &
                  VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
        }
        ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)support->pNext)->maxVariableDescriptorCount = 1000;
    }
}
static void limits(void) {
    RDNA4R.lim = (VkPhysicalDeviceLimits){
        .maxPerStageDescriptorSamplers = 100,
        .maxPerStageDescriptorUniformBuffers = 100,
        .maxPerStageDescriptorStorageBuffers = 100,
        .maxPerStageDescriptorSampledImages = 100,
        .maxPerStageDescriptorStorageImages = 100,
        .maxPerStageDescriptorInputAttachments = 100,
        .maxPerStageResources = 1000,
        .maxDescriptorSetSamplers = 100,
        .maxDescriptorSetUniformBuffers = 100,
        .maxDescriptorSetUniformBuffersDynamic = 100,
        .maxDescriptorSetStorageBuffers = 100,
        .maxDescriptorSetStorageBuffersDynamic = 100,
        .maxDescriptorSetSampledImages = 100,
        .maxDescriptorSetStorageImages = 100,
        .maxDescriptorSetInputAttachments = 100,
        .maxFragmentCombinedOutputResources = 1000,
        .maxColorAttachments = 8,
        .maxFragmentOutputAttachments = 8,
        .maxBoundDescriptorSets = 8,
        .maxStorageBufferRange = UINT32_MAX,
    };
    RDNA4R.descriptorLimits = (VkPhysicalDeviceVulkan12Properties){
        .maxPerStageDescriptorUpdateAfterBindSamplers = 100,
        .maxPerStageDescriptorUpdateAfterBindUniformBuffers = 100,
        .maxPerStageDescriptorUpdateAfterBindStorageBuffers = 100,
        .maxPerStageDescriptorUpdateAfterBindSampledImages = 100,
        .maxPerStageDescriptorUpdateAfterBindStorageImages = 100,
        .maxPerStageDescriptorUpdateAfterBindInputAttachments = 100,
        .maxPerStageUpdateAfterBindResources = 1000,
        .maxDescriptorSetUpdateAfterBindSamplers = 100,
        .maxDescriptorSetUpdateAfterBindUniformBuffers = 100,
        .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 100,
        .maxDescriptorSetUpdateAfterBindStorageBuffers = 100,
        .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 100,
        .maxDescriptorSetUpdateAfterBindSampledImages = 100,
        .maxDescriptorSetUpdateAfterBindStorageImages = 100,
        .maxDescriptorSetUpdateAfterBindInputAttachments = 100,
    };
    RDNA4R.maxPerSetDescriptors = 1000;
}
int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        vkCreateDescriptorSetLayout = createLayout;
        vkDestroyDescriptorSetLayout = destroyLayout;
        vkGetDescriptorSetLayoutSupport = supportLayout;
        RDNA4sRefl *v = calloc(1, sizeof *v), *f = calloc(1, sizeof *f);
        CHECK(v && f);
        const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT};
        for (unsigned i = 0; i < sizeof types / sizeof types[0]; ++i) {
            limits();
            memset(v, 0, sizeof *v);
            v->bind[0][0] = (RDNA4sBind){.used = 1, .type = types[i], .count = 100, .runtime = 0};
            RDNA4ArgumentTables table = {0}; NSError *error = nil;
            CHECK(rdna4_argument_table_layouts(&table, v, NULL, YES, &error));
            CHECK(!table.active && !error && table.descriptorBudgetReady);
            table = (RDNA4ArgumentTables){0};
            RDNA4ArgumentTables before = table;
            unsigned called = creates;
            v->bind[0][0].count = 101;
            CHECK(!rdna4_argument_table_layouts(&table, v, NULL, YES, &error));
            CHECK(error && creates == called && !memcmp(&before, &table, sizeof table));
        }
        limits();
        memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        for (unsigned c = 0; c < 2; ++c) {
            v->bind[2+c][0] = (RDNA4sBind){.used = 1, .type = rdna4_argument_descriptor_type(c), .count = 0, .runtime = 1};
            v->bind[2+c][1] = (RDNA4sBind){.used = 1, .type = c ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .count = RDNA4NS_TEXEL_SHAPES, .runtime = 0};
        }
        *f = *v;
        RDNA4R.descriptorLimits.maxPerStageDescriptorUpdateAfterBindSampledImages = 30;
        RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindSampledImages = 30;
        RDNA4R.lim.maxPerStageDescriptorSampledImages = 0;
        RDNA4R.lim.maxDescriptorSetSampledImages = 0;
        RDNA4R.lim.maxFragmentCombinedOutputResources = 8;
        RDNA4ArgumentTables table = {.shapeRegistry = YES, .fragmentColorAttachments = 2};
        NSError *error = nil;
        CHECK(rdna4_argument_table_layouts(&table, v, f, NO, &error));
        CHECK(!error && table.capacity[0] == 27 && table.capacity[1] == 3);
        CHECK(table.texelStages[0] == 3 && table.texelStages[1] == 3);
        CHECK(table.ordinaryUsage.pipeline.uniform_texel_buffer == 0);
        uint32_t allocated[3] = {27, 3, 0};
        uint32_t texelAllocated[2] = {RDNA4NS_TEXEL_SHAPES,
                                      RDNA4NS_TEXEL_SHAPES};
        CHECK(rdna4_argument_allocation_budget(&table, allocated,
            texelAllocated, &error));
        RDNA4ArgumentTables before = table;
        allocated[1] = 4; error = nil;
        CHECK(!rdna4_argument_allocation_budget(&table, allocated,
            texelAllocated, &error));
        CHECK(error && !memcmp(&before, &table, sizeof table));
        rdna4_argument_tables_destroy(&table);
        CHECK(!table.descriptorBudgetReady && !table.tableStages[0] && !table.texelStages[0]);
        before = (RDNA4ArgumentTables){.shapeRegistry = YES,
            .fragmentColorAttachments = 2};
        table = before;
        unsigned oldCreates = creates, oldDestroys = destroys;
        failAt = creates + 2;
        error = nil;
        CHECK(!rdna4_argument_table_layouts(&table, v, f, NO, &error));
        CHECK(error && creates == oldCreates + 2 && destroys == oldDestroys + 1 &&
            !memcmp(&before, &table, sizeof table));
        failAt = 0;
        limits(); memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        v->maxset = 2;
        v->bind[2][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 0, .runtime = 1};
        v->bind[2][1] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            0, 1, 0, 1u << RDNA4NS_COMP_UINT};
        RDNA4ArgumentTables dynamic = {.shapeRegistry = YES, .abi10 = YES,
            .dynamicTexel = {YES, NO}};
        error = nil;
        unsigned layoutBefore = creates;
        CHECK(rdna4_argument_table_layouts(&dynamic, v, NULL, YES, &error));
        CHECK(!error && dynamic.texelVariable[0] &&
            dynamic.texelCapacity[0] > 1 && dynamic.capacity[0] > 0);
        CHECK(createdBindingCount[layoutBefore + 1] == 2 &&
            createdBindings[layoutBefore + 1][0].binding == 0 &&
            createdBindings[layoutBefore + 1][1].binding == 1);
        CHECK(!(createdFlags[layoutBefore + 1][0] &
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
        CHECK(createdFlags[layoutBefore + 1][1] &
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT);
        rdna4_argument_tables_destroy(&dynamic);
        CHECK(!dynamic.texelVariable[0] && !dynamic.texelCapacity[0]);
        limits(); memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        v->maxset = 2;
        v->bind[2][1] = (RDNA4sBind){1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            0, 1, 0, 1u << RDNA4NS_COMP_SINT};
        dynamic = (RDNA4ArgumentTables){.shapeRegistry = YES, .abi10 = YES,
            .dynamicTexel = {YES, NO}};
        layoutBefore = creates;
        CHECK(rdna4_argument_table_layouts(&dynamic, v, NULL, YES, &error));
        CHECK(dynamic.texelVariable[0] && !dynamic.capacity[0] &&
            createdBindingCount[layoutBefore + 1] == 1 &&
            createdBindings[layoutBefore + 1][0].binding == 1 &&
            (createdFlags[layoutBefore + 1][0] &
             VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
        rdna4_argument_tables_destroy(&dynamic);
        limits(); memset(v, 0, sizeof *v); memset(f, 0, sizeof *f);
        table = (RDNA4ArgumentTables){.fragmentColorAttachments = 8};
        f->outmask = 1u << 7;
        CHECK(rdna4_argument_table_layouts(&table, v, f, NO, &error));
        table = (RDNA4ArgumentTables){.fragmentColorAttachments = 9};
        before = table;
        CHECK(!rdna4_argument_table_layouts(&table, v, f, NO, &error));
        CHECK(!memcmp(&before, &table, sizeof table));
        free(v); free(f);
        printf("ARGUMENT_DESCRIPTOR_LAYOUTS_PASS checks=%u reflected_types=11 shared_abi9_union=1 ordinary_uab=1 fragment_outputs=1 allocation_preflight=1 rollback=1 gpu_execution=0\n", checks);
    }
    return 0;
}
