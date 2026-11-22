#include "GarnetDevice.m"
#include <assert.h>

static unsigned fixedQueries, fixedCreates;
static void VKAPI_CALL null_format_properties(VkPhysicalDevice device, VkFormat format,
    VkFormatProperties *properties) {
    (void)device;
    assert(format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32_UINT ||
        format == VK_FORMAT_R32_SINT);
    *properties = (VkFormatProperties){.optimalTilingFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT};
}
static void VKAPI_CALL null_layout_support(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info, VkDescriptorSetLayoutSupport *support) {
    (void)device;
    assert(info->bindingCount == 2 && !support->pNext);
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = info->pNext;
    assert(flags && !(flags->pBindingFlags[0] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
    assert(info->pBindings[0].binding == 0 && info->pBindings[1].binding == 1);
    assert(info->pBindings[1].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
    assert(info->pBindings[1].descriptorCount == 3);
    support->supported = VK_TRUE;
    ++fixedQueries;
}
static VkResult VKAPI_CALL null_layout_create(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    if (info->bindingCount == 2) {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = info->pNext;
        assert(!(flags->pBindingFlags[0] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
        ++fixedCreates;
    }
    *layout = (VkDescriptorSetLayout)(uintptr_t)(10 + fixedCreates);
    return VK_SUCCESS;
}
static void VKAPI_CALL null_layout_destroy(VkDevice device, VkDescriptorSetLayout layout,
    const VkAllocationCallbacks *allocator) { (void)device; (void)layout; (void)allocator; }

static NSDictionary *null_metadata(BOOL normalized, NSString *kind, unsigned bytes) {
    return @{
        @"trimmed_from_reflection_version": @65,
        @"normalized_sampler_coordinates": @(normalized),
        @"argument_resource_tables": @{@"version": @9, @"table_binding": @672,
            @"categories": @[@0], @"comparison_sampler": @NO,
            @"null_shapes": @[@0x001, @0x204]},
        @"bindings": @[@{@"kind": kind, @"access": @"ReadOnly", @"metal_index": @0,
            @"descriptor": @{@"set": @0, @"binding": @674, @"count": @1},
            @"declared_size": @(bytes), @"extent": @{@"Object": @{@"bytes": @(bytes)}}}]
    };
}

int main(void) {
    @autoreleasepool {
        vkGetPhysicalDeviceFormatProperties = null_format_properties;
        for (uint32_t arrayed = 0; arrayed < 2; ++arrayed)
            for (uint32_t component = 0; component < 3; ++component) {
                rdna4ns_shape shape = {.dim = RDNA4NS_DIM_CUBE, .arrayed = arrayed,
                    .component = component, .access = RDNA4NS_ACCESS_STORAGE};
                uint32_t legacy = 0;
                assert(rdna4ns_shape_image_legacy(&shape, &legacy) == RDNA4NS_OK);
                RDNA4DummyImagePlan plan; const char *why = NULL;
                assert(rdna4_dummy_image_plan_make(legacy, YES, 1, &plan, &why));
                assert(plan.textureType == (arrayed ? MTLTextureTypeCubeArray : MTLTextureTypeCube));
                assert(plan.usage == MTLTextureUsageShaderWrite &&
                    plan.storageMode == MTLStorageModePrivate && !plan.depth && plan.storage);
                vkGetPhysicalDeviceFormatProperties = NULL;
                assert(!rdna4_dummy_image_plan_make(legacy, YES, 1, &plan, &why) && why);
                vkGetPhysicalDeviceFormatProperties = null_format_properties;
            }
        RDNA4R.gpuArgumentTables = YES;
        RDNA4R.uniformTexelBuffers = YES;
        vkGetDescriptorSetLayoutSupport = null_layout_support;
        vkCreateDescriptorSetLayout = null_layout_create;
        vkDestroyDescriptorSetLayout = null_layout_destroy;
        RDNA4sRefl reflection = {0};
        reflection.maxset = 2;
        reflection.bind[0][674] = (RDNA4sBind){.used=1, .type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count=1};
        reflection.bind[2][0] = (RDNA4sBind){.used=1, .type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .runtime=1};
        reflection.bind[2][1] = (RDNA4sBind){.used=1, .type=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .runtime=1};
        reflection.argumentNullShapes[0] = 0x001;
        reflection.argumentNullShapes[1] = 0x204;
        reflection.argumentNullShapeCount = 2;
        for (unsigned normalized = 0; normalized < 2; ++normalized) {
            NSDictionary *metadata = null_metadata(normalized, @"StageResourceMetadata", 384);
            NSError *error = nil;
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            RDNA4PB *bindings = NULL; uint32_t count = 0;
            assert(rdna4_stage_dsl(&reflection, 0, VK_SHADER_STAGE_VERTEX_BIT,
                metadata, &layout, &bindings, &count, [NSMutableArray array], &error));
            assert(count == 1 && bindings[0].stageResourceMetadata && !bindings[0].stageTokenTable);
            RDNA4ArgumentTables tables = {0};
            assert(rdna4_argument_table_metadata_apply(&tables, &reflection, 0, metadata, &error));
            assert(rdna4_argument_stage_remap(&tables, bindings, count, &error));
            assert(bindings[0].nullShapeRemap[0] == 0 && bindings[0].nullShapeRemap[1] == 0);
            assert(bindings[0].nullShapeAccess[0] == RDNA4NS_ACCESS_SAMPLED);
            assert(bindings[0].nullShapeAccess[1] == RDNA4NS_ACCESS_UNIFORM_TEXEL);
            free(bindings);
        }
        for (unsigned bytes = 128; bytes <= 384; bytes += 256) {
            NSError *error = nil; VkDescriptorSetLayout layout;
            RDNA4PB *bindings = NULL; uint32_t count = 0;
            assert(!rdna4_stage_dsl(&reflection, 0, VK_SHADER_STAGE_VERTEX_BIT,
                null_metadata(NO, bytes == 128 ? @"StageResourceMetadata" : @"SamplerCoordinateFlags", bytes),
                &layout, &bindings, &count, [NSMutableArray array], &error));
            assert(error && !bindings);
        }
        {
            RDNA4sRefl dynamic = reflection;
            dynamic.bind[0][672] = (RDNA4sBind){.used=1,
                .type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count=1};
            RDNA4ArgumentTables tables = {0}; NSError *error = nil;
            assert(rdna4_argument_table_metadata_apply(&tables, &reflection, 0,
                null_metadata(YES, @"StageResourceMetadata", 384), &error));
            assert(rdna4_argument_table_metadata_apply(&tables, &dynamic, 0,
                null_metadata(NO, @"StageResourceMetadata", 384), &error));
            assert(!tables.samplerCoordinates);
            assert(!rdna4_argument_table_metadata_apply(&tables, &dynamic, 0,
                null_metadata(YES, @"StageResourceMetadata", 384), &error));
            assert(error);
        }
        rdna4_argument_capacity_limits limits = {
            .max_per_stage_uab={100,100,100}, .max_pipeline_uab={100,100,100},
            .max_per_stage_uab_resources=100, .max_per_stage_ordinary={100,100,100},
            .max_pipeline_ordinary={100,100,100}, .max_per_stage_ordinary_resources=100,
            .max_per_set_descriptors=100, .max_bound_descriptor_sets=5};
        RDNA4ArgumentTables tables = {.shapeRegistry=YES}; NSError *error = nil;
        assert(rdna4_argument_table_layouts_limits(&tables, &reflection, NULL, NO, &limits, &error));
        assert(fixedQueries == 1 && fixedCreates == 1 && !tables.variable[0] && tables.capacity[0]);
        rdna4_argument_tables_destroy(&tables);
        puts("NULL_RUNTIME_PASS static/no672 metadata, both normalized flags, strict 384B, fixed mixed bindings, cube_storage_plans=6 gpu_execution=0");
    }
    return 0;
}
